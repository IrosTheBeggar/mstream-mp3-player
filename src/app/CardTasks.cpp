// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/CardTasks.h"

#include <esp_heap_caps.h>

#include <cstring>

#include "QueueView.h"
#include "TagText.h"
#include "UiText.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"
#include "ui/Thumbs.h"

namespace {

using Job = ScanScheduler::Job;
using Src = ScanScheduler::Source;
constexpr size_t kMusicPrefix = 7;  // "/music/"
// The scan's chunk out after this long with the worker free (3.3.2: every
// 100 files or 5 s).
constexpr uint32_t kChunkIdleMs = 5000;
// The scan's memory given back after this long without a scan step.
constexpr uint32_t kTrimMs = 10000;

int slotOf(Src s) {
  switch (s) {
    case Src::Playing: return 0;
    case Src::QueueNext: return 1;
    case Src::QueueSoon: return 2;
    case Src::Shown: return 3;
    default: return -1;
  }
}

}  // namespace

CardTasks::CardTasks(Library& library, CardWorker& worker)
    : lib_(library), worker_(worker), jobs_(psramAlloc, psramFree) {}

void CardTasks::stepEntry(void* self) { static_cast<CardTasks*>(self)->jobs_.step(); }

// On the worker, during a walk step: the index is read only (nothing
// changes it while a step runs: the update step waits for the worker).
bool CardTasks::indexed(const char* rel, size_t len, void* ctx) {
  const LibraryIndex* idx = static_cast<CardTasks*>(ctx)->lib_.index();
  if (!idx || !idx->ready()) return false;
  char path[TrackCatalog::kMaxPath];
  const int n = snprintf(path, sizeof(path), "/music/%.*s", static_cast<int>(len), rel);
  return n > 0 && static_cast<size_t>(n) < sizeof(path) && idx->findTrack(path) != LibraryIndex::kNone;
}

bool CardTasks::begin() {
  active_ = false;
  if (!lib_.records() || !card_.begin()) return false;
  cardroot::Root* root = lib_.root();
  cardjobs::Config c;
  c.store = lib_.store();
  c.card = &card_;
  c.fs = lib_.fatfs();
  // T the boot's build found broken is left out (the walk would fail on it).
  const bool useT = root && root->present && !lib_.transferBad();
  c.transferPath = useT ? root->tagsPath : nullptr;
  c.root = useT ? root->identity : tagstore::Identity();
  c.alloc = psramAlloc;
  c.release = psramFree;
  c.indexed = indexed;
  c.indexedCtx = this;
  jobs_.begin(c);
  active_ = true;
  pendingAfterScan_ = lib_.softStale();
  scanTotal_ = indexPending();
  return true;
}

void CardTasks::armWalk(uint32_t nowMs) {
  if (!active_) return;
  walkArmed_ = true;
  walkAtMs_ = nowMs + kWalkDelayMs;
}

uint32_t CardTasks::indexPending() const {
  const LibraryIndex* idx = const_cast<Library&>(lib_).index();
  if (!idx || !idx->ready()) return 0;
  uint32_t n = 0;
  for (uint32_t t = 0; t < idx->trackCount(); ++t)
    if (idx->track(t).flags & LibraryIndex::kTrackPending) ++n;
  return n;
}

bool CardTasks::pendingTrack(uint32_t id) const {
  const LibraryIndex* idx = const_cast<Library&>(lib_).index();
  return idx && idx->ready() && id < idx->trackCount() && (idx->track(id).flags & LibraryIndex::kTrackPending);
}

// The track's path, relative to /music, into slot `slot`.
bool CardTasks::pick(uint32_t id, int slot) {
  LibraryIndex* idx = lib_.index();
  char path[TrackCatalog::kMaxPath];
  const size_t n = idx->trackPath(id, path, sizeof(path));
  if (n <= kMusicPrefix || std::strncmp(path, "/music/", kMusicPrefix) != 0) return false;
  pickLen_[slot] = n - kMusicPrefix;
  std::memcpy(picks_[slot], path + kMusicPrefix, pickLen_[slot] + 1);
  return true;
}

void CardTasks::sources(ScanScheduler::In& in, const Sources& src) {
  if (pendingTrack(src.playing)) in.playingPending = pick(src.playing, 0);
  if (src.queue && src.queue->current() >= 0) {
    const uint32_t first = static_cast<uint32_t>(src.queue->current()) + 1;
    const uint32_t size = src.queue->size();
    for (uint32_t i = 0; i < ScanScheduler::kQueueNext && first + i < size && !in.queueNextPending; ++i) {
      const uint32_t t = src.queue->trackAt(first + i);
      if (pendingTrack(t)) in.queueNextPending = pick(t, 1);
    }
    const uint32_t soon = first + ScanScheduler::kQueueNext;
    for (uint32_t i = 0; i < ScanScheduler::kQueueSoon && soon + i < size && !in.queueSoonPending; ++i) {
      const uint32_t t = src.queue->trackAt(soon + i);
      if (pendingTrack(t)) in.queueSoonPending = pick(t, 2);
    }
  }
  // The Library tab's page: its first Pending track (a long page is looked
  // at in its first 512 rows).
  for (uint32_t i = 0; i < src.shown.count && i < 512 && !in.shownPending; ++i)
    if (pendingTrack(src.shown[i])) in.shownPending = pick(src.shown[i], 3);
  in.restPending = jobs_.restWork();
}

void CardTasks::loop(ScanScheduler::In in, const Sources& src, ui::Thumbs* thumbs, bool covers) {
  const uint32_t now = in.nowMs;
  playing_ = src.playing;
  // 1. The step that finished.
  Job done = Job::None;
  uint32_t ms = 0;
  if (worker_.poll(now, &done, &ms)) {
    sched_.stepDone(done, ms);
    taken(done, now);
  }
  // 2. The boot's validation walk, 2 s after the UI's first frame.
  if (walkArmed_ && static_cast<int32_t>(now - walkAtMs_) >= 0) {
    walkArmed_ = false;
    jobs_.askWalk();
  }
  // 3. What has work (only read with the worker free: a step may change it).
  in.running = worker_.running();
  const bool free = !worker_.busy();
  if (free) {
    in.cover = covers && thumbs && thumbs->wantsCover(now);
    if (active_) {
      in.walk = !updateWanted_ && jobs_.walkWork();
      const bool beforeUpdate = updateWanted_ && (lib_.store()->hasJournals() || jobs_.chunkPending());
      in.compact = (jobs_.compactWork() || beforeUpdate) && !(beforeUpdate && lastCompactFailed_);
      // The scan once the boot's walk has said what D's to-do is (3.3.9).
      if (!updateWanted_ && !in.walk && !in.compact && bootWalkDone_) sources(in, src);
      // The scan's end: nothing for it anywhere, no walk under way or asked.
      const bool scanWork = in.playingPending || in.queueNextPending || in.queueSoonPending || in.shownPending ||
                            in.restPending;
      if (!updateWanted_ && !in.walk && !jobs_.walking() && !in.compact && bootWalkDone_) {
        if (!scanWork && !scanOver_) {
          scanOver_ = true;
          if (scannedSinceBuild_ > 0 || pendingAfterScan_) askUpdate("the scan's end");
        } else if (scanWork) {
          scanOver_ = false;
        }
      }
    }
  }
  // 4. The scheduler.
  const ScanScheduler::Out o = sched_.update(in);
  if (o.batteryHeld) Serial.println("[card] the battery is below 10%: the scan waits for USB or 15%");
  if (o.batteryReleased) Serial.println("[card] the battery floor lifted: the scan goes on");
  lastWait_ = o.wait;
  if (in.running != Job::None) worker_.setPriority(o.priority);
  // 5. The step.
  if (o.job == Job::Cover) {
    if (thumbs) thumbs->startCover(worker_, o.priority, now);
  } else if (o.job == Job::Walk || o.job == Job::Compact || o.job == Job::Scan) {
    const int slot = o.job == Job::Scan ? slotOf(o.source) : -1;
    const char* rel = slot >= 0 ? picks_[slot] : nullptr;
    const size_t len = slot >= 0 ? pickLen_[slot] : 0;
    if (jobs_.prepare(o.job, o.source, rel, len, now) && worker_.start(o.job, o.priority, stepEntry, this, now)) {
      if (o.job == Job::Compact) compacting_ = true;
      if (o.job == Job::Walk && !walkSeen_) {
        walkSeen_ = true;
        walkStartMs_ = now;
      }
      lastWorkMs_ = now;
    }
  }
  // 6. Between steps: the scan's chunk out once it has waited 5 s (a
  // pause, the battery floor), and its memory back once it has nothing.
  if (active_ && !worker_.busy()) {
    if (jobs_.chunkPending() && now - lastScanStepMs_ >= kChunkIdleMs) jobs_.flushChunk();
    if (now - lastScanStepMs_ >= kTrimMs && !jobs_.restWork()) jobs_.trim();
  }
}

void CardTasks::taken(Job job, uint32_t nowMs) {
  if (job == Job::Cover || job == Job::None) return;  // Thumbs takes its own in
  const cardjobs::Done& d = jobs_.finish();
  switch (job) {
    case Job::Walk:
      if (d.walkEnded) afterWalk(d);
      break;
    case Job::Compact:
      compacting_ = false;
      lastCompaction_ = d.compaction;
      lastCompactFailed_ = !d.compacted;
      Serial.printf("[card] compaction %s: %lu records, %lu folders, %lu chunks merged%s%s\n",
                    d.compacted ? "done" : "FAILED", (unsigned long)d.compaction.records,
                    (unsigned long)d.compaction.folders, (unsigned long)d.compaction.chunksMerged,
                    d.compaction.walkMerged ? ", the walk's" : "", d.compaction.rescanned ? ", a Rescan" : "");
      if (!d.compacted && d.compaction.error) Serial.printf("[card] compaction: %s\n", d.compaction.error);
      break;
    case Job::Scan: afterScan(d, nowMs); break;
    default: break;
  }
}

void CardTasks::afterWalk(const cardjobs::Done& d) {
  const cardwalk::CardWalk::Result& r = d.walk;
  lastWalk_ = r;
  walked_ = true;
  walkMs_ = millis() - walkStartMs_;
  walkSeen_ = false;
  if (d.walkRetried) {
    lib_.setTransferBad();
    Serial.println("[card] the walk: the transfer's tags file failed its checks as it was read: walked again without "
                   "it (no transfer data this session)");
    return;
  }
  bootWalkDone_ = true;  // the scan goes on from D as it is, whatever the walk found
  if (r.state != cardwalk::CardWalk::State::Done) {
    Serial.printf("[card] the walk FAILED (error %u) after %lu steps: nothing of it counts (the next boot walks "
                  "again)\n",
                  static_cast<unsigned>(r.error), (unsigned long)r.steps);
    return;
  }
  Serial.printf("[card] the walk: %lu folders (%lu listings, %lu merged), %lu audio, %lu images, %lu other; %lu "
                "added, %lu changed, %lu gone; %lu doubtful (%lu by the skew, %lu by D's qfp, %lu read, %lu not T's)%s; "
                "%lu steps in %lu ms\n",
                (unsigned long)r.folders, (unsigned long)r.listings, (unsigned long)r.foldersMerged,
                (unsigned long)r.audio, (unsigned long)r.images, (unsigned long)r.others, (unsigned long)r.added,
                (unsigned long)r.changed, (unsigned long)r.gone, (unsigned long)r.doubtful, (unsigned long)r.bySkew,
                (unsigned long)r.byDeviceQfp, (unsigned long)r.byQfp, (unsigned long)r.notTransfer,
                r.summary.firstAfterCommit ? "; the first walk after a commit" : "", (unsigned long)r.steps,
                (unsigned long)walkMs_);
  // New to the index (an index built from T, or walked from /music, lists
  // files before D has them): the toast's count and U11's.
  const uint32_t added = d.newToIndex;
  if (added > 0) {
    char t[48];
    librarytext::foundText(added, t, sizeof(t));
    toast(t);
  }
  if (r.summary.changed) {
    const uint32_t toScan = r.added + r.changed + indexPending();
    scanTotal_ = toScan;
    scanDone_ = 0;
    scanOver_ = false;
    // U11: the new files show at once with their names, or at the scan's
    // end (no new files: nothing to show before the tags).
    if (added > 0 && ScanScheduler::buildAfterWalk(added, toScan, sched_.scanMsPerFile())) {
      askUpdate("the walk found new files");
    } else {
      pendingAfterScan_ = true;
    }
  }
  if (updateAfterWalk_) {
    updateAfterWalk_ = false;
    askUpdate("g0");
  }
}

void CardTasks::afterScan(const cardjobs::Done& d, uint32_t nowMs) {
  lastScanStepMs_ = nowMs;
  if (d.verifyEnded) {
    const cardjobs::Verified& v = jobs_.verified();
    Serial.printf("[card] verify: %lu software files, %lu their qfp T's, %lu NOT, %lu with no qfp in T, %lu unreadable\n",
                  (unsigned long)v.checked, (unsigned long)v.equal, (unsigned long)v.differ, (unsigned long)v.noQfp,
                  (unsigned long)v.failed);
  }
  if (!d.handled || d.relLength == 0) return;
  // The index's track, if it has one: no longer Pending; the playing one's
  // tags shown now.
  LibraryIndex* idx = lib_.index();
  if (idx && idx->ready()) {
    char path[TrackCatalog::kMaxPath];
    snprintf(path, sizeof(path), "/music/%s", d.rel);
    const uint32_t id = idx->findTrack(path);
    if (id != LibraryIndex::kNone) {
      idx->clearPending(id);
      const bool tags = d.read && !d.readError && d.result != tagscan::Result::Unreadable;
      if (tags && id == playing_ && jobs_.record()) lib_.setOverlay(id, *jobs_.record());
    }
  }
  if (d.read && !d.readError && jobs_.mode() != cardjobs::Mode::Verify) {
    ++scannedSinceBuild_;
    ++scanDone_;
    if (scanDone_ > scanTotal_) scanTotal_ = scanDone_;
  }
}

void CardTasks::askUpdate(const char* why) {
  if (!active_) return;
  if (!updateWanted_) Serial.printf("[card] the update step is asked (%s)\n", why);
  updateWanted_ = true;
  updateShown_ = false;
  updateWhy_ = why;
}

void CardTasks::askWalkAndUpdate() {
  if (!active_) return;
  updateAfterWalk_ = true;
  jobs_.askWalk();
}

bool CardTasks::updateDue(uint32_t nowMs, bool safePoint) {
  if (!updateWanted_ || worker_.busy()) return false;
  const bool journals = lib_.store()->hasJournals() || jobs_.chunkPending();
  if (journals && !lastCompactFailed_) return false;  // compacted first (a worker step)
  if (!safePoint) return false;
  // "Updating library…" drawn before the loop stops for the build (the
  // status line redraws at most twice a second).
  if (!updateShown_) {
    updateShown_ = true;
    updating_ = true;
    updateShownMs_ = nowMs;
    return false;
  }
  return nowMs - updateShownMs_ >= kShowUpdatingMs;
}

void CardTasks::beforeUpdate() {
  waitIdle(30000);
  updating_ = true;
  jobs_.prepareUpdate();
}

void CardTasks::afterUpdate(bool ok) {
  updating_ = false;
  updateWanted_ = false;
  updateShown_ = false;
  lastCompactFailed_ = false;
  ++updates_;
  if (ok) {
    scannedSinceBuild_ = 0;
    pendingAfterScan_ = false;
    jobs_.libraryRebuilt();
    scanTotal_ = indexPending();
    scanDone_ = 0;
    toast(uitext::kLibraryUpdated);
  }
  Serial.printf("[card] the update step (%s): %s\n", updateWhy_, ok ? "the library is rebuilt" : "FAILED");
}

void CardTasks::updateDeferred() {
  updating_ = false;
  updateWanted_ = false;
  updateShown_ = false;
  ++deferred_;
  toast(uitext::kLibraryAtBoot);
}

bool CardTasks::rescan(bool everything) {
  if (!active_) return false;
  jobs_.askCompact(true);
  jobs_.askRest(everything ? cardjobs::Mode::All : cardjobs::Mode::Normal);
  pendingAfterScan_ = true;
  scanOver_ = false;
  const LibraryIndex* idx = lib_.index();
  scanTotal_ = idx && idx->ready() ? idx->trackCount() : 0;
  scanDone_ = 0;
  return true;
}

bool CardTasks::walkNow() {
  if (!active_) return false;
  jobs_.askWalk();
  return true;
}

bool CardTasks::verify() {
  if (!active_ || !lib_.root() || !lib_.root()->present || lib_.transferBad()) return false;
  jobs_.askRest(cardjobs::Mode::Verify);
  return true;
}

bool CardTasks::waitIdle(uint32_t maxMs) {
  const bool ok = worker_.waitIdle(maxMs);
  Job done = Job::None;
  uint32_t ms = 0;
  if (worker_.poll(millis(), &done, &ms)) {
    sched_.stepDone(done, ms);
    taken(done, millis());
  }
  return ok;
}

void CardTasks::flushNow() {
  if (!active_) return;
  waitIdle(5000);
  if (jobs_.chunkPending() && !jobs_.flushChunk()) Serial.println("[card] the scan's chunk couldn't be written");
}

librarytext::Status CardTasks::status() const {
  librarytext::Status s;
  if (!active_) return s;
  using P = librarytext::Status::Phase;
  if (updating_ || (updateWanted_ && updateShown_)) {
    s.phase = P::Updating;
  } else if (jobs_.walking() || walkSeen_ || walkArmed_) {
    s.phase = P::Checking;
  } else if (!scanOver_ && (scanTotal_ > 0 || scanDone_ > 0) && jobs_.mode() != cardjobs::Mode::Verify) {
    s.phase = P::Reading;
    s.done = scanDone_;
    s.total = scanTotal_ > scanDone_ ? scanTotal_ : scanDone_;
  } else if (lib_.root() && lib_.root()->plan) {
    s.phase = P::Unfinished;
  }
  return s;
}

bool CardTasks::takeToast(char* buf, size_t size) {
  if (!toast_[0]) return false;
  snprintf(buf, size, "%s", toast_);
  toast_[0] = 0;
  return true;
}

void CardTasks::toast(const char* text) { snprintf(toast_, sizeof(toast_), "%s", text); }

void CardTasks::state(librarytext::Status* s, char* line, size_t size) const {
  *s = status();
  if (!active_) {
    snprintf(line, size, "no records here (the flash, or no PSRAM): covers only");
    return;
  }
  const cardjobs::Jobs::Counts& c = jobs_.counts();
  snprintf(line, size, "%lu read (%lu unreadable, %lu partial, %lu read errors, %lu skipped); waiting for %s%s",
           (unsigned long)c.scanned, (unsigned long)c.unreadable, (unsigned long)c.partial,
           (unsigned long)c.readErrors, (unsigned long)c.skipped, ScanScheduler::waitName(lastWait_),
           updateWanted_ ? "; the update step is asked" : "");
}

void CardTasks::report() const {
  // The scheduler: where the time went, each job's steps (3.3.9, L3).
  Serial.print("[card] waited (s):");
  for (int w = 1; w < ScanScheduler::kWaits; ++w) {
    const uint64_t ms = sched_.waitedMs(static_cast<ScanScheduler::Wait>(w));
    if (ms) Serial.printf(" %s %.1f;", ScanScheduler::waitName(static_cast<ScanScheduler::Wait>(w)), ms / 1000.0);
  }
  Serial.printf(" now: %s\n", ScanScheduler::waitName(lastWait_));
  for (int j = 1; j < ScanScheduler::kJobs; ++j) {
    const auto job = static_cast<Job>(j);
    if (!sched_.steps(job)) continue;
    Serial.printf("[card] %s: %lu steps, mean %lu ms, longest %lu ms\n", ScanScheduler::jobName(job),
                  (unsigned long)sched_.steps(job), (unsigned long)sched_.meanStepMs(job),
                  (unsigned long)sched_.maxStepMs(job));
  }
  Serial.printf("[card] the worker: %s, %lu steps, its 6 KB stack's least left %lu B%s; internal RAM free %u B, "
                "lowest %u B\n",
                worker_.alive() ? "up" : "not running", (unsigned long)worker_.steps(),
                (unsigned long)worker_.stackLeft(), worker_.failedStarts() ? " (it couldn't always start)" : "",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  if (!active_) return;
  const cardjobs::Jobs::Counts& c = jobs_.counts();
  Serial.printf("[card] jobs: %lu walks (%lu steps, %lu failed), %lu compactions (%lu failed), %lu files read, %lu "
                "chunks (%lu refused), %lu Views (%lu rows), %lu update steps (%lu deferred to the boot); the scan "
                "~%lu ms a file%s\n",
                (unsigned long)c.walks, (unsigned long)c.walkSteps, (unsigned long)c.walksFailed,
                (unsigned long)c.compactions, (unsigned long)c.compactionsFailed, (unsigned long)c.scanned,
                (unsigned long)c.chunks, (unsigned long)c.appendFailures, (unsigned long)c.viewsOpened,
                (unsigned long)c.rowsLooked, (unsigned long)updates_, (unsigned long)deferred_,
                (unsigned long)sched_.scanMsPerFile(), jobs_.transferBad() ? "; T absent (failed its checks)" : "");
  if (walked_) {
    Serial.printf("[card] the last walk: %lu folders, %lu audio, %lu added, %lu changed, %lu gone, %lu doubtful, %lu "
                  "qfp reads, %lu ms\n",
                  (unsigned long)lastWalk_.folders, (unsigned long)lastWalk_.audio, (unsigned long)lastWalk_.added,
                  (unsigned long)lastWalk_.changed, (unsigned long)lastWalk_.gone, (unsigned long)lastWalk_.doubtful,
                  (unsigned long)lastWalk_.qfpReads, (unsigned long)walkMs_);
  }
  Serial.printf("[card] FatFs: %lu folders listed, %lu entries, %lu files opened by the jobs\n",
                (unsigned long)card_.listings(), (unsigned long)card_.entries(), (unsigned long)card_.fileOpens());
}
