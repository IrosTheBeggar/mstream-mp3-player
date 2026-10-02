// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueSaver.h"

#include <cstring>

namespace {
bool timeFor(uint32_t now, uint32_t since, uint32_t delay) { return now - since >= delay; }
constexpr uint32_t kAllLines = 0xFFFFFFFFu;
}  // namespace

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
  contentChangedMs_ = nowMs;
}

void QueueSaver::markSaved() {
  savedContent_ = lastContent_ = queue_.contentVersion();
  savedPosition_ = lastPosition_ = queue_.positionVersion();
  contentDirty_ = positionDirty_ = false;
}

void QueueSaver::noteChanges(uint32_t nowMs) {
  const uint32_t cv = queue_.contentVersion();
  const uint32_t pv = queue_.positionVersion();
  if (cv != lastContent_) {
    lastContent_ = cv;
    contentChangedMs_ = nowMs;
    contentDirty_ = cv != savedContent_;
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
  if (contentDirty_ && timeFor(nowMs, contentChangedMs_, kContentDelayMs) &&
      static_cast<int32_t>(nowMs - nextTryMs_) >= 0) {
    if (startWrite(nowMs)) stepWrite(nowMs, kLinesPerPass);
    return;
  }
  // A position only means something for the queue the file holds.
  if (positionDirty_ && !contentDirty_ && timeFor(nowMs, positionChangedMs_, kPositionDelayMs)) savePosition();
}

bool QueueSaver::flushNow(uint32_t nowMs) {
  noteChanges(nowMs);
  // A write under way: to its end. If the queue changed since it began,
  // the writer says so at once: dropped, and written again whole below.
  if (writing_) stepWrite(nowMs, kAllLines);
  // An edit not on the card yet: now, without its delay or a retry's wait.
  if (contentDirty_ && startWrite(nowMs)) stepWrite(nowMs, kAllLines);
  if (positionDirty_ && !contentDirty_) savePosition();
  stepResume();
  return !writing_ && !contentDirty_ && !positionDirty_;
}

void QueueSaver::abort() {
  if (writing_) dropWrite();
}

bool QueueSaver::startWrite(uint32_t nowMs) {
  sink_ = store_.openTemp();
  if (!sink_) {
    failed(nowMs);
    return false;
  }
  writeGeneration_ = generation_ + 1;
  writer_.begin(queue_, writeGeneration_);
  writeStartMs_ = nowMs;
  writing_ = true;
  return true;
}

void QueueSaver::stepWrite(uint32_t nowMs, uint32_t maxLines) {
  switch (writer_.step(queue_, catalog_, *sink_, maxLines)) {
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
  if (!store_.commitTemp()) {
    failed(nowMs);
    return;
  }
  generation_ = writeGeneration_;
  savedContent_ = queue_.contentVersion();  // Done: unchanged since begin()
  contentDirty_ = false;
  ++writes_;
  lastWriteMs_ = nowMs - writeStartMs_;
  savePosition();  // paired with this generation from now on
  stepResume();    // (the same: a resume point waiting for the file, or one of the old generation)
}

void QueueSaver::dropWrite() {
  writing_ = false;
  sink_ = nullptr;
  store_.discardTemp();
}

void QueueSaver::failed(uint32_t nowMs) {
  ++failures_;
  failedSinceEdit_ = true;
  nextTryMs_ = nowMs + kRetryMs;
}

void QueueSaver::savePosition() {
  store_.savePosition(generation_, queue_.current());
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
  // waiting its write: after it, with the new generation).
  if (contentDirty_ || writing_) return;
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
