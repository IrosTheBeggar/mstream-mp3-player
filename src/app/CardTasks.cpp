// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/CardTasks.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

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

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

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

// The update step's build and save (LibraryUpdate): behind the fence, the
// index the build's; then the finished index, which nothing changes while
// it is written.
void CardTasks::buildEntry(void* self) { static_cast<CardTasks*>(self)->lib_.update()->stepBuild(); }
void CardTasks::saveEntry(void* self) { static_cast<CardTasks*>(self)->lib_.update()->stepSave(); }

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
  if (!lib_.records() || !lib_.update() || !card_.begin()) return false;
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
  // A step of the walk or the scan's rest is a slice (3.3.9).
  c.nowUs = nowUs;
  c.sliceUs = kLitSliceUs;
  jobs_.begin(c);
  active_ = true;
  // A soft-stale index (the scan went on since its build, a build that left
  // records out, an update step a cut stopped) is rebuilt at the scan's
  // end: the first one after the boot's walk, even with nothing to scan
  // (a transfer card D has nothing of its own for, and a walk that finds
  // nothing new), whose scanOver_ would never go from false to true.
  pendingAfterScan_ = lib_.softStale();
  scanOver_ = !pendingAfterScan_;
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

void CardTasks::loop(ScanScheduler::In in, const Sources& src, ui::Thumbs* thumbs, bool covers, const UpdateEnv& env) {
  const uint32_t now = in.nowMs;
  playing_ = src.playing;
  // The update step's fence is up and its build not handed yet: the
  // worker's task (made before the fence: Out::wantWorker) stays for it.
  if (active_ && lib_.update()->phase() == LibraryUpdate::Phase::Build) worker_.ensure(now);
  // 1. The step that finished.
  Job done = Job::None;
  uint32_t ms = 0;
  if (worker_.poll(now, &done, &ms)) taken(done, now, ms);
  // 2. The boot's validation walk, 2 s after the UI's first frame; a failed
  // walk's retry.
  if (walkArmed_ && static_cast<int32_t>(now - walkAtMs_) >= 0) {
    walkArmed_ = false;
    jobs_.askWalk();
  }
  if (walkRetryArmed_ && static_cast<int32_t>(now - walkRetryAtMs_) >= 0) {
    walkRetryArmed_ = false;
    Serial.printf("[card] the walk again (retry %lu of %lu)\n", (unsigned long)walkRetries_,
                  (unsigned long)kWalkRetries);
    jobs_.askWalk();
  }
  // 3. The update step (LibraryUpdate, 3.4.2): what it waits for, what it
  // holds, and what the loop carries out this pass.
  LibraryUpdate* up = active_ ? lib_.update() : nullptr;
  LibraryUpdate::Out uo;
  if (up) {
    LibraryUpdate::In ui;
    ui.nowMs = now;
    ui.workerFree = !worker_.busy();
    ui.workerUp = worker_.alive();
    ui.walking = jobs_.walking();
    ui.journals = lib_.store()->hasJournals() || jobs_.chunkPending();
    ui.compactFailed = lastCompactFailed_;
    ui.playing = env.playing;
    ui.waiting = env.waiting;
    ui.trackLeftMs = env.trackLeftMs;
    ui.seekSeq = env.seekSeq;
    if (up->phase() == LibraryUpdate::Phase::Asked) {  // (the memory check's: read only when it may be asked)
      ui.psramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      ui.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
      ui.alsoFreed = env.alsoFreed;
    }
    uo = up->update(ui);
    // The fence goes up only once the build can start at once: the task
    // first (no internal RAM for it: the step waits, the index untouched).
    if (uo.wantWorker) worker_.ensure(now);
    // (A refusal for records that don't list the card isn't main.cpp's:
    // its Failed line says the card doesn't answer. This one says why.)
    const bool unlisted = uo.act == LibraryUpdate::Do::Failed && up->last().unlisted;
    if (uo.act != LibraryUpdate::Do::None && !unlisted) act_ = uo.act;
    switch (uo.act) {
      case LibraryUpdate::Do::Deferred:
        ++deferred_;
        updateOver();
        // (No marker on the card, no promise: none written, or none wanted
        // after this boot's own build ran short.)
        if (up->last().markerWritten) toast(uitext::kLibraryAtBoot);
        break;
      case LibraryUpdate::Do::Failed:
        ++updates_;
        updateOver();
        Serial.printf("[card] the update step (%s): FAILED before its fence (the library stays as it was)%s\n",
                      up->why(),
                      up->last().unlisted ? ": the card's records don't list it (no transfer data, and no walk yet "
                                            "listed it into tags.bin); a walk that does asks again"
                                          : "");
        break;
      case LibraryUpdate::Do::Saved: updateOver(); break;
      default: break;
    }
  }
  lastUpdate_ = uo;
  // 4. What has work (only read with the worker free: a step may change it).
  in.running = worker_.running();
  // Internal RAM's lowest while a step runs (gs; L3 and L4).
  if (in.running != Job::None) worker_.noteInternal(static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  const bool free = !worker_.busy();
  if (free) {
    // (Not in the pass that puts the fence up: Thumbs' pools go with it.)
    in.cover = covers && thumbs && thumbs->wantsCover(now) && uo.act != LibraryUpdate::Do::Fence;
    if (active_) {
      // A walk under way goes on to its end when the update step is asked:
      // the compaction before the build can't run while it writes walk.jnl
      // (the step would build from tags.bin alone).
      in.walk = (!uo.holdScan || jobs_.walking()) && jobs_.walkWork();
      in.compact = (jobs_.compactWork() || uo.compact) && !(uo.compact && lastCompactFailed_);
      // The scan once the boot's walk has said what D's to-do is (3.3.9);
      // not while the update step is asked or under way (the index is the
      // build's, then the save's).
      const bool held = uo.holdScan || uo.updating;
      if (!held && !in.walk && !in.compact && bootWalkDone_) sources(in, src);
      // The scan's end: nothing for it anywhere, no walk under way or asked.
      const bool scanWork = in.playingPending || in.queueNextPending || in.queueSoonPending || in.shownPending ||
                            in.restPending;
      if (!held && !in.walk && !jobs_.walking() && !in.compact && bootWalkDone_) {
        if (!scanWork && !scanOver_) {
          scanOver_ = true;
          scanEnded();
        } else if (scanWork) {
          scanOver_ = false;
        }
      }
    }
  }
  in.build = uo.build;
  in.save = uo.save;
  in.updating = uo.updating;
  // The UI isn't up and lit (dark; or not started, or a screen of its own,
  // where input holds the scan anyway): a scan's slice may be level with
  // the loop (3.3.9).
  in.dark = !covers;
  // 5. The scheduler.
  const ScanScheduler::Out o = sched_.update(in);
  if (o.batteryHeld) Serial.println("[card] the battery is below 10%: the scan waits for USB or 15%");
  if (o.batteryReleased) Serial.println("[card] the battery floor lifted: the scan goes on");
  lastWait_ = o.wait;
  if (in.running != Job::None) {
    worker_.setPriority(o.priority);
    // A wait came, or a list moves: the slice under way ends after its unit.
    if (o.cut) jobs_.cutSlice();
  }
  // 6. The step.
  if (o.job == Job::Cover) {
    if (thumbs) thumbs->startCover(worker_, o.priority, now);
  } else if (o.job == Job::Build) {
    if (up && worker_.start(Job::Build, o.priority, buildEntry, this, now)) {
      up->buildStarted();
      lastWorkMs_ = now;
    }
  } else if (o.job == Job::Save) {
    if (up && worker_.start(Job::Save, o.priority, saveEntry, this, now)) {
      up->saveStarted();
      lastWorkMs_ = now;
    }
  } else if (o.job == Job::Walk || o.job == Job::Compact || o.job == Job::Scan) {
    const int slot = o.job == Job::Scan ? slotOf(o.source) : -1;
    const char* rel = slot >= 0 ? picks_[slot] : nullptr;
    const size_t len = slot >= 0 ? pickLen_[slot] : 0;
    jobs_.setSlice(in.dark ? kDarkSliceUs : kLitSliceUs);
    if (jobs_.prepare(o.job, o.source, rel, len, now) && worker_.start(o.job, o.priority, stepEntry, this, now)) {
      if (o.job == Job::Compact) compacting_ = true;
      if (o.job == Job::Walk && !walkSeen_) {
        walkSeen_ = true;
        walkStartMs_ = now;
      }
      lastWorkMs_ = now;
    }
  }
  // 7. Between steps: the scan's chunk out once it has waited 5 s (a
  // pause, the battery floor; a chunk the card refused, 30 s after: each try
  // is a FatFs open, about 1 s of the SD driver's retries on a pulled card),
  // and its memory back once it has nothing.
  if (active_ && !worker_.busy()) {
    if (jobs_.chunkPending() && now - lastScanStepMs_ >= kChunkIdleMs) jobs_.idleFlush(now);
    if (now - lastScanStepMs_ >= kTrimMs && !jobs_.restWork()) jobs_.trim();
  }
}

// The scan's end: the update step when the journal took records since the
// last build (reads the card refused are none: the build would find them
// Pending again, and the scan would read them again, an update a pass), or
// a walk's changes, a Rescan or a soft-stale boot ask for it. Not when the
// rest stopped on a read error (the card pulled: the build would fail, and
// the next boot's scan rebuilds what this one recorded).
void CardTasks::scanEnded() {
  if (!jobs_.newRecords() && !pendingAfterScan_) return;
  if (restFailed_) {
    Serial.println("[card] the scan's end: its rest stopped on a read error (the card?): no update step now (the "
                   "next boot's scan's end has it)");
    return;
  }
  askUpdate("the scan's end");
}

void CardTasks::taken(Job job, uint32_t nowMs, uint32_t ms) {
  if (job == Job::Cover || job == Job::None) {
    sched_.stepDone(job, ms);
    return;  // Thumbs takes its own in
  }
  if (job == Job::Build || job == Job::Save) {
    sched_.stepDone(job, ms);
    LibraryUpdate* up = lib_.update();
    if (job == Job::Build) {
      up->buildDone();
    } else {
      up->saveDone();
    }
    return;
  }
  const cardjobs::Done& d = jobs_.finish();
  // The step's units for gs and the scan's rate: a slice's CardWalk steps,
  // or the files the scan took.
  sched_.stepDone(job, ms, job == Job::Walk ? d.walkSteps : job == Job::Scan ? d.files : 1);
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
    // Nothing of it counts. Often a glitch (a write the card refused once):
    // walked again a minute later, twice at most; then the next boot's. The
    // scan goes on; while the card's records don't list it, no update step
    // (askUpdate()): built from the files the scan read, it would drop the
    // rest of the library.
    walkFailed_ = true;
    char next[40];
    if (walkRetries_ < kWalkRetries) {
      ++walkRetries_;
      walkRetryArmed_ = true;
      walkRetryAtMs_ = millis() + kWalkRetryMs;
      snprintf(next, sizeof(next), "walked again in %lu s", (unsigned long)(kWalkRetryMs / 1000));
    } else {
      snprintf(next, sizeof(next), "the next boot walks again");
    }
    Serial.printf("[card] the walk FAILED (%s) after %lu steps: nothing of it counts (%s)%s\n",
                  cardwalk::CardWalk::errorName(r.error), (unsigned long)r.steps, next,
                  lib_.update()->recordsListCard() ? "" : "; no update step until a walk lists the card");
    return;
  }
  walkFailed_ = false;
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
    // The files the scan reads: the index's Pending tracks (a walked
    // index's are all of them), the files new to it and the changed ones.
    const uint32_t toScan = added + r.changed + indexPending();
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
  if (d.restEnded) restFailed_ = d.restFailed;
  if (d.verifyEnded) {
    const cardjobs::Verified& v = jobs_.verified();
    Serial.printf("[card] verify: %lu software files, %lu their qfp T's, %lu NOT, %lu with no qfp in T, %lu unreadable\n",
                  (unsigned long)v.checked, (unsigned long)v.equal, (unsigned long)v.differ, (unsigned long)v.noQfp,
                  (unsigned long)v.failed);
  }
  // Each file the step took (a slice's several): the index's track, if it
  // has one, no longer Pending; the playing one's tags shown now (the
  // record is the step's last file's: a slice of the rest that read the
  // playing track before its last file shows its tags at the next build).
  LibraryIndex* idx = lib_.index();
  const bool verifying = jobs_.mode() == cardjobs::Mode::Verify;
  for (uint32_t i = 0; i < d.files; ++i) {
    const cardjobs::FileDone f = jobs_.file(i);
    if (f.relLength == 0) continue;
    if (idx && idx->ready()) {
      char path[TrackCatalog::kMaxPath];
      snprintf(path, sizeof(path), "/music/%s", f.rel);
      const uint32_t id = idx->findTrack(path);
      if (id != LibraryIndex::kNone) {
        idx->clearPending(id);
        const bool tags = f.read && !f.readError && f.result != tagscan::Result::Unreadable;
        if (tags && i + 1 == d.files && id == playing_ && jobs_.record()) lib_.setOverlay(id, *jobs_.record());
      }
    }
    if (f.read && !f.readError && !verifying) {
      ++scanDone_;
      if (scanDone_ > scanTotal_) scanTotal_ = scanDone_;
    }
  }
}

void CardTasks::askUpdate(const char* why, bool deferToBoot) {
  if (!active_) return;
  LibraryUpdate* up = lib_.update();
  if (walkFailed_ && !up->recordsListCard()) {
    // (The update step would refuse it too, after its compaction:
    // LibraryUpdate::Step::unlisted. The walk that lists the card asks.)
    Serial.printf("[card] the update step (%s) waits: the walk failed, and the card's records don't list it yet\n",
                  why);
    return;
  }
  if (!up->asked()) Serial.printf("[card] the update step is asked (%s)\n", why);
  up->ask(why, deferToBoot);
}

void CardTasks::askWalkAndUpdate() {
  if (!active_) return;
  updateAfterWalk_ = true;
  jobs_.askWalk();
}

bool CardTasks::updateAsked() const { return active_ && const_cast<Library&>(lib_).update()->asked(); }

void CardTasks::fenceUp() {
  // The build streams tags.bin: the scan's View (it reads it) closed, its
  // chunk out, and its memory back for the build (nothing scans until the
  // save's end).
  jobs_.prepareUpdate();
  jobs_.trim();
  lib_.update()->fencedUp();
}

void CardTasks::live() {
  LibraryUpdate* up = lib_.update();
  const LibraryUpdate::Step& s = up->last();
  // The fence down: the index readable again (the save next, on the worker).
  up->lived();
  ++updates_;
  // What the journal had is the index's now, or (failed: no memory, the
  // card pulled mid-build) the next boot's: library.idx keeps the inputs of
  // its last build, which the records no longer match, so that boot's
  // scan's end rebuilds. Not asked again until new records reach the
  // journal.
  jobs_.markRecords();
  pendingAfterScan_ = false;
  lastCompactFailed_ = false;
  if (s.built) {
    jobs_.libraryRebuilt();
    scanTotal_ = indexPending();
    scanDone_ = 0;
    toast(uitext::kLibraryUpdated);
  }
  Serial.printf("[card] the update step (%s): %s%s\n", s.why,
                s.built ? "the library is rebuilt (its save next, on the card worker)"
                        : "FAILED (no library until the next boot, or Try again)",
                s.cardGone && s.readErrors ? "; a read of the card's records failed as it built (the card?): "
                                             "nothing walked or saved"
                : s.readErrors             ? "; a read of T failed: built from D alone, T tried again next time"
                                           : "");
}

void CardTasks::updateOver() {
  // The marker (a deferral's) makes the next boot build everything there
  // is; a failure leaves library.idx's inputs, which the records no longer
  // match. Either way: no other step for what the journal had.
  jobs_.markRecords();
  pendingAfterScan_ = false;
  lastCompactFailed_ = false;
}

void CardTasks::resetStats() {
  sched_.resetStats();
  worker_.resetStats();
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
  if (worker_.poll(millis(), &done, &ms)) taken(done, millis(), ms);
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
  const LibraryUpdate::Phase up = const_cast<Library&>(lib_).update()->phase();
  // From the step's fence to its Live (the library hidden), and while it
  // waits only for the safe point or the worker (its compaction done).
  const bool updating = up == LibraryUpdate::Phase::Fence || up == LibraryUpdate::Phase::Build ||
                        up == LibraryUpdate::Phase::Building || up == LibraryUpdate::Phase::Live ||
                        (up == LibraryUpdate::Phase::Asked && (lastUpdate_.wait == LibraryUpdate::Wait::SafePoint ||
                                                                lastUpdate_.wait == LibraryUpdate::Wait::Worker));
  if (updating) {
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
  static const char* const kUpdateWaits[] = {"", "a walk's end", "its compaction", "the worker", "the safe point"};
  char update[64] = "";
  if (updateAsked()) {
    const int w = static_cast<int>(lastUpdate_.wait);
    snprintf(update, sizeof(update), "; the update step is asked%s%s", w ? ", waiting for " : "",
             w > 0 && w < 5 ? kUpdateWaits[w] : "");
  }
  snprintf(line, size, "%lu read (%lu unreadable, %lu partial, %lu read errors, %lu skipped); waiting for %s%s",
           (unsigned long)c.scanned, (unsigned long)c.unreadable, (unsigned long)c.partial,
           (unsigned long)c.readErrors, (unsigned long)c.skipped, ScanScheduler::waitName(lastWait_), update);
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
    // The walk's steps are slices (their CardWalk steps: a folder each); the
    // scan's, a loop source's file or a slice of the rest's (their files).
    char units[40] = "";
    if (job == Job::Walk) snprintf(units, sizeof(units), " (%lu walk steps)", (unsigned long)sched_.units(job));
    if (job == Job::Scan) snprintf(units, sizeof(units), " (%lu files)", (unsigned long)sched_.units(job));
    Serial.printf("[card] %s: %lu %s%s, mean %lu ms, longest %lu ms\n", ScanScheduler::jobName(job),
                  (unsigned long)sched_.steps(job), job == Job::Walk ? "slices" : "steps", units,
                  (unsigned long)sched_.meanStepMs(job), (unsigned long)sched_.maxStepMs(job));
  }
  // Since the boot or gs0: the stack's least left over every life of the
  // task (each life's own mark starts again), internal RAM's lowest while a
  // step ran (sampled at each step's start and end and each loop pass
  // during it); and internal RAM's lowest since the boot.
  const uint32_t internalMin = worker_.internalMin();
  char lowest[24] = "(no step)";
  if (internalMin != UINT32_MAX) snprintf(lowest, sizeof(lowest), "%lu B", (unsigned long)internalMin);
  Serial.printf("[card] the worker: %s, %lu steps, its 6 KB stack's least left %lu B (this life %lu B)%s; internal "
                "RAM free %u B, lowest %s while a step ran, %u B since the boot\n",
                worker_.alive() ? "up" : "not running", (unsigned long)worker_.steps(),
                (unsigned long)worker_.stackLeastLeft(), (unsigned long)worker_.stackLeft(),
                worker_.failedStarts() ? " (it couldn't always start)" : "",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), lowest,
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
