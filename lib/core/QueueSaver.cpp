// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueSaver.h"

#include <cstdlib>
#include <cstring>

namespace {
bool timeFor(uint32_t now, uint32_t since, uint32_t delay) { return now - since >= delay; }
constexpr uint32_t kAllLines = 0xFFFFFFFFu;
void* heapAlloc(size_t n) { return std::malloc(n); }
void heapFree(void* p) { std::free(p); }
}  // namespace

QueueSaver::QueueSaver(Store& store, const QueueModel& queue, const TrackCatalog& catalog, AllocFn alloc,
                       FreeFn release)
    : store_(store),
      queue_(queue),
      catalog_(catalog),
      alloc_(alloc && release ? alloc : heapAlloc),
      free_(alloc && release ? release : heapFree) {}

QueueSaver::~QueueSaver() { freeSnapshot(); }

uint32_t QueueSaver::pathHash(const char* path) {
  uint32_t h = 2166136261u;
  for (const char* p = path; p && *p; ++p) h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
  return h;
}

bool QueueSaver::resumeApplies(const QueueResume& r, uint32_t fileGeneration, int32_t line, bool kept,
                               const char* path) {
  return r.valid && r.positionMs > 0 && kept && line >= 0 && r.generation == fileGeneration && r.entry == line &&
         path && path[0] && r.pathHash == pathHash(path);
}

void QueueSaver::loaded(uint32_t generation, bool rewrite, uint32_t nowMs) {
  generation_ = generation;
  savedContent_ = lastContent_ = queue_.contentVersion();
  savedPosition_ = lastPosition_ = queue_.positionVersion();
  contentDirty_ = rewrite;
  positionDirty_ = false;
  failedSinceEdit_ = false;
  contentChangedMs_ = dirtySinceMs_ = nowMs;
  fileIsQueue_ = true;
  // Read whole, the queue's entries are the file's lines; with tracks
  // dropped (to be written again), the file's line isn't the entry's: its
  // own, until the write.
  fileLine_ = rewrite ? -1 : queue_.current();
}

void QueueSaver::keptFile(uint32_t generation, int32_t fileLine) {
  generation_ = generation;
  savedContent_ = lastContent_ = queue_.contentVersion();
  savedPosition_ = lastPosition_ = queue_.positionVersion();
  contentDirty_ = positionDirty_ = false;
  fileIsQueue_ = false;
  fileLine_ = fileLine;
}

void QueueSaver::noteChanges(uint32_t nowMs) {
  const uint32_t cv = queue_.contentVersion();
  const uint32_t pv = queue_.positionVersion();
  if (cv != lastContent_) {
    lastContent_ = cv;
    contentChangedMs_ = nowMs;
    const bool was = contentDirty_;
    contentDirty_ = cv != savedContent_;
    if (contentDirty_ && !was) dirtySinceMs_ = nowMs;
    // (A snapshot's write goes on: this edit is the next write's.)
    if (writingSnapshot() && !editedSinceSnap_) {
      editedSinceSnap_ = true;
      editedSinceSnapMs_ = nowMs;
    }
    failedSinceEdit_ = false;
  }
  if (pv != lastPosition_) {
    lastPosition_ = pv;
    positionChangedMs_ = nowMs;
    positionDirty_ = pv != savedPosition_;
  }
}

bool QueueSaver::busy() const {
  if (writing_) return true;
  return (contentDirty_ || positionDirty_) && !failedSinceEdit_;
}

void QueueSaver::loop(uint32_t nowMs) {
  noteChanges(nowMs);
  stepResume();
  if (writing_) {
    stepWrite(nowMs, kLinesPerPass);
    return;
  }
  // 2 s after the last edit; or, edits that never settle, once the file
  // is kMaxStaleMs behind (a snapshot's write: the class comment; after
  // no memory for one, again kRetryMs later).
  const bool quiet = timeFor(nowMs, contentChangedMs_, kContentDelayMs);
  const bool stale = timeFor(nowMs, dirtySinceMs_, kMaxStaleMs) && static_cast<int32_t>(nowMs - snapshotAtMs_) >= 0;
  if (contentDirty_ && (quiet || stale) && static_cast<int32_t>(nowMs - nextTryMs_) >= 0) {
    if (startWrite(nowMs, !quiet)) stepWrite(nowMs, kLinesPerPass);
    return;
  }
  // A position only means something for the queue the file holds.
  if (positionDirty_ && !contentDirty_ && timeFor(nowMs, positionChangedMs_, kPositionDelayMs)) savePosition();
}

bool QueueSaver::flushNow(uint32_t nowMs) {
  noteChanges(nowMs);
  // A write under way: to its end. If the queue changed since it began,
  // the writer says so at once (a snapshot's isn't the queue then either):
  // dropped, and written again whole below.
  if (writingSnapshot() && queue_.contentVersion() != snap_.version) dropWrite();
  if (writing_) stepWrite(nowMs, kAllLines);
  // An edit not on the card yet: now, without its delay or a retry's wait.
  if (contentDirty_ && startWrite(nowMs, false)) stepWrite(nowMs, kAllLines);
  if (positionDirty_ && !contentDirty_) savePosition();
  stepResume();
  return !writing_ && !contentDirty_ && !positionDirty_;
}

void QueueSaver::abort() {
  if (writing_) dropWrite();
}

bool QueueSaver::startWrite(uint32_t nowMs, bool snapshot) {
  // (A queue that fits a pass is written whole in it: no edit can come.)
  if (snapshot && queue_.size() + 1 > kLinesPerPass && !takeSnapshot()) {
    // No memory for one: the plain write after 2 s of quiet meanwhile.
    snapshotAtMs_ = nowMs + kRetryMs;
    return false;
  }
  sink_ = store_.openTemp();
  if (!sink_) {
    freeSnapshot();
    failed(nowMs);
    return false;
  }
  writeGeneration_ = generation_ + 1;
  if (!snap_.ids) writer_.begin(queue_, writeGeneration_);
  editedSinceSnap_ = false;
  writeStartMs_ = nowMs;
  writing_ = true;
  return true;
}

bool QueueSaver::takeSnapshot() {
  freeSnapshot();
  const uint32_t n = queue_.size();
  const bool shuffled = queue_.shuffled();
  if (n == 0) return false;
  auto* block = static_cast<uint32_t*>(alloc_(static_cast<size_t>(n) * sizeof(uint32_t) * (shuffled ? 2 : 1)));
  if (!block) return false;
  snap_.ids = block;
  snap_.ranks = shuffled ? block + n : nullptr;
  for (uint32_t i = 0; i < n; ++i) {
    snap_.ids[i] = queue_.trackAt(i);
    if (shuffled) snap_.ranks[i] = queue_.rankAt(i);
  }
  snap_.size = n;
  snap_.current = queue_.current();
  snap_.shuffled = shuffled;
  snap_.version = queue_.contentVersion();
  snap_.next = 0;
  snap_.headerDone = false;
  return true;
}

void QueueSaver::freeSnapshot() {
  if (snap_.ids) free_(snap_.ids);
  snap_ = Snapshot();
}

queuetext::Writer::Step QueueSaver::stepSnapshot(uint32_t maxLines) {
  using Step = queuetext::Writer::Step;
  // (QueueText's own lines, from the snapshot's values.)
  if (!snap_.headerDone && maxLines) {
    if (!queuetext::writeHeader(snap_.shuffled, snap_.size, snap_.current, writeGeneration_, *sink_)) {
      return Step::Failed;
    }
    snap_.headerDone = true;
    --maxLines;
  }
  for (; maxLines && snap_.next < snap_.size; --maxLines, ++snap_.next) {
    const uint32_t rank = snap_.shuffled ? snap_.ranks[snap_.next] : 0;
    if (!queuetext::writeEntry(catalog_, snap_.ids[snap_.next], snap_.shuffled, rank, *sink_)) return Step::Failed;
  }
  return snap_.headerDone && snap_.next >= snap_.size ? Step::Done : Step::More;
}

void QueueSaver::stepWrite(uint32_t nowMs, uint32_t maxLines) {
  switch (snap_.ids ? stepSnapshot(maxLines) : writer_.step(queue_, catalog_, *sink_, maxLines)) {
    case queuetext::Writer::Step::More:
      break;
    case queuetext::Writer::Step::Done:
      finishWrite(nowMs);
      break;
    case queuetext::Writer::Step::Changed:
      dropWrite();  // edited meanwhile: written again once it settles
      break;
    case queuetext::Writer::Step::Failed:
      dropWrite();
      failed(nowMs);
      break;
  }
}

void QueueSaver::finishWrite(uint32_t nowMs) {
  writing_ = false;
  sink_ = nullptr;
  // What the file holds: the queue (the plain writer's Done: unchanged
  // since begin()), or the snapshot's.
  const bool snapshot = snap_.ids != nullptr;
  const uint32_t version = snapshot ? snap_.version : queue_.contentVersion();
  const int32_t line = snapshot ? snap_.current : queue_.current();
  freeSnapshot();
  if (!store_.commitTemp()) {
    failed(nowMs);
    return;
  }
  generation_ = writeGeneration_;
  savedContent_ = version;
  contentDirty_ = queue_.contentVersion() != version;
  // (Edited since the snapshot: the next write's, its staleness from the
  // first of those edits.)
  if (contentDirty_) dirtySinceMs_ = editedSinceSnap_ ? editedSinceSnapMs_ : nowMs;
  fileIsQueue_ = true;  // (after keptFile(): the listener's edit replaced the file's queue)
  ++writes_;
  if (snapshot) ++snapshotWrites_;
  lastWriteMs_ = nowMs - writeStartMs_;
  if (!contentDirty_) {
    savePosition();  // paired with this generation from now on
  } else {
    // The queue moved on from the snapshot: its positions aren't the
    // file's lines. NVS keeps the older generation's position, which a boot
    // doesn't pair with this file: it takes the header's line, this one.
    fileLine_ = line;
  }
  stepResume();  // (the same: a resume point waiting for the file, or one of the old generation)
}

void QueueSaver::dropWrite() {
  writing_ = false;
  sink_ = nullptr;
  freeSnapshot();
  store_.discardTemp();
}

void QueueSaver::failed(uint32_t nowMs) {
  ++failures_;
  failedSinceEdit_ = true;
  nextTryMs_ = nowMs + kRetryMs;
}

void QueueSaver::savePosition() {
  // A position of a queue that is less than the file (keptFile()) isn't one
  // of its lines: the file's line stays.
  if (fileIsQueue_) {
    store_.savePosition(generation_, queue_.current());
    fileLine_ = queue_.current();
  }
  savedPosition_ = queue_.positionVersion();
  positionDirty_ = false;
}

void QueueSaver::stepResume() {
  const int32_t cur = queue_.current();
  if (!transport_.have || cur < 0) {
    // Playback moved on (or nothing to pick up): cleared at once, whatever
    // the file holds.
    if (resume_.valid) {
      resume_ = QueueResume{};
      store_.saveResume(resume_);
      ++resumeWrites_;
    }
    return;
  }
  // Paired with a file only once it holds the queue as it is (an edit
  // waiting its write: after it, with the new generation; a file that holds
  // more than the queue, keptFile(): not until it is the queue's again).
  if (contentDirty_ || writing_ || !fileIsQueue_) return;
  const uint32_t pos = transport_.positionMs;
  if (resume_.valid && resume_.generation == generation_ && resume_.entry == cur) {
    const uint32_t moved = pos > resume_.positionMs ? pos - resume_.positionMs : resume_.positionMs - pos;
    if (moved < kResumeSlackMs && resume_.anchor == transport_.anchor) return;  // saved already
  }
  char path[TrackCatalog::kMaxPath];
  catalog_.path(queue_.currentTrack(), path, sizeof(path));
  QueueResume r;
  r.valid = true;
  r.generation = generation_;
  r.entry = cur;
  r.pathHash = pathHash(path);
  r.positionMs = pos;
  r.durationMs = transport_.durationMs;
  r.anchor = transport_.anchor;
  resume_ = r;
  store_.saveResume(resume_);
  ++resumeWrites_;
}
