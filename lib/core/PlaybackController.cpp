// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PlaybackController.h"

#include <string>

void PlaybackController::startCurrent() {
  if (!hasTrack()) return;
  if (held()) {
    // The output can't be heard yet: this entry waits, selected. The
    // backend lets go of what it had (a track playing or paused).
    if (state_ != PlayState::Stopped && !cued_) audio_.stop();
    setPlaying(PlayState::Waiting);
    cued_ = true;
    return;
  }
  startNow();
}

void PlaybackController::startNow() {
  if (!hasTrack()) return;
  // An unknown id gives "": the backend fails it and update() skips on.
  char path[TrackCatalog::kMaxPath];
  const uint32_t id = queue_.currentTrack();
  catalog_.path(id, path, sizeof(path));
  const uint32_t at = hasStartPoint() ? startMs_ : 0;
  // Its length: a start point's (the resume point's, as the backend had it:
  // a VBR file without a table of contents is placed by it), else the
  // catalog's hint.
  const uint32_t hint = at > 0 && startDurationMs_ > 0 ? startDurationMs_ : catalog_.durationHintMs(id);
  clearStartPoint();  // once: a later start of the entry is from its beginning
  playedFromMs_ = at;
  audio_.play(std::string(path), hint, at);
  setPlaying(PlayState::Playing);
  cued_ = false;
}

void PlaybackController::play(size_t position) {
  if (position >= queue_.size()) return;
  failuresInARow_ = 0;
  queue_.setCurrent(static_cast<uint32_t>(position));
  startCurrent();
}

void PlaybackController::togglePlayPause() {
  switch (state_) {
    case PlayState::Stopped:
      if (!queue_.empty()) play(queue_.current() < 0 ? 0 : static_cast<size_t>(queue_.current()));
      break;
    case PlayState::Playing:
      audio_.pause();
      state_ = PlayState::Paused;
      break;
    case PlayState::Paused:
      if (cued_) {
        startCurrent();
        break;
      }
      if (held()) {
        setPlaying(PlayState::Waiting);  // the backend keeps the paused track: release() resumes it
        break;
      }
      audio_.resume();
      setPlaying(PlayState::Playing);
      break;
    case PlayState::Waiting:
      state_ = PlayState::Paused;  // cancelled: a cued entry stays cued, a paused track paused
      break;
  }
}

void PlaybackController::release() {
  if (state_ != PlayState::Waiting) return;
  if (cued_) {
    startNow();
    return;
  }
  audio_.resume();
  setPlaying(PlayState::Playing);
}

void PlaybackController::cancelWait() {
  if (state_ == PlayState::Waiting) state_ = PlayState::Paused;
}

void PlaybackController::next() {
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  clearStartPoint();  // (a queue of one wraps to the same entry: from its start)
  advance();
}

void PlaybackController::advance() {
  if (!queue_.step(+1, repeat_)) {
    stop();  // the end of the queue, and no repeat
    return;
  }
  startCurrent();
}

PlaybackController::Prev PlaybackController::prevRule(PlayState state, bool startPointWaits, bool positionKnown,
                                                      uint32_t positionMs) {
  if (startPointWaits) return Prev::Restart;  // the entry it would pick up in, from 0:00
  if (state == PlayState::Stopped) return Prev::Previous;
  return positionKnown && positionMs > kRestartAfterMs ? Prev::Restart : Prev::Previous;
}

PlaybackController::Prev PlaybackController::prevAction() const {
  if (!hasTrack()) return Prev::Previous;
  // Stopped or cued, the backend holds nothing of this entry: at 0:00.
  // A start it hasn't taken up yet may still count the track before: it
  // is where that play asked to start (a resume point's 2:30 is 2:30 at
  // once, a restart's or a skip's 0:00 is 0:00). A track that failed has
  // no place to go back to.
  const bool holding = state_ != PlayState::Stopped && !cued_;
  if (!holding) return prevRule(state_, hasStartPoint(), true, 0);
  const uint32_t at = audio_.positionKnown() ? audio_.positionMs() : playedFromMs_;
  return prevRule(state_, hasStartPoint(), !audio_.failed(), at);
}

void PlaybackController::prev() {
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  if (prevAction() == Prev::Restart) {
    restart();
    return;
  }
  clearStartPoint();
  queue_.step(-1, repeat_);  // at the start without repeat: the first track again
  startCurrent();
}

void PlaybackController::restart() {
  clearStartPoint();
  switch (state_) {
    case PlayState::Playing:
      startCurrent();  // from 0:00: a start like any (faded in; held: it waits)
      break;
    case PlayState::Paused:
    case PlayState::Waiting:
      // Nothing starts: the held track is let go and the next play (or the
      // wait's release) starts the entry from its beginning.
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

void PlaybackController::cueNext() { cue(+1); }
void PlaybackController::cuePrev() { cue(-1); }

void PlaybackController::cue(int delta) {
  if (state_ == PlayState::Playing || state_ == PlayState::Waiting) {
    delta > 0 ? next() : prev();
    return;
  }
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  if (delta < 0 && prevAction() == Prev::Restart) {
    restart();  // as prev(): the same entry, at 0:00, still paused (or stopped)
    return;
  }
  clearStartPoint();
  queue_.step(delta, repeat_);
  if (state_ == PlayState::Paused && !cued_) {
    audio_.stop();  // the paused track can't be resumed any more
    cued_ = true;
  }
}

void PlaybackController::stop() {
  audio_.stop();
  state_ = PlayState::Stopped;
  cued_ = false;
  pausedByTimer_ = false;  // (stopped: headphone Play starts nothing anyway)
}

bool PlaybackController::stopKeepingPlace() {
  uint32_t ms = 0, dur = 0;
  // A start point waiting (after a boot, qs): stop() leaves it as it is.
  if (hasStartPoint()) {
    stop();
    return true;
  }
  // The backend holds this entry's track: where it is, as prevAction()
  // reads it (a start not taken up yet: where it was asked to start; a
  // failed track has no place).
  const bool holding = hasTrack() && state_ != PlayState::Stopped && !cued_ && !audio_.failed();
  if (holding) {
    const bool known = audio_.positionKnown();
    ms = known ? audio_.positionMs() : playedFromMs_;
    dur = known ? audio_.durationMs() : 0;
  }
  stop();
  if (ms == 0) return false;
  setStartPoint(ms, dur);  // stopped: it waits for the next play
  return true;
}

void PlaybackController::setStartPoint(uint32_t ms, uint32_t durationMs) {
  if (!hasTrack() || ms == 0) {
    clearStartPoint();
    return;
  }
  if (durationMs == 0) {
    // Not said (the console's qs): the length as known here, so Now
    // Playing keeps it and the backend can place the start by it.
    if (hasStartPoint()) {
      durationMs = startDurationMs_;
    } else if (state_ != PlayState::Stopped && !cued_) {
      durationMs = audio_.durationMs();  // the backend holds this entry's track
    } else {
      durationMs = catalog_.durationHintMs(queue_.currentTrack());
    }
  }
  startMs_ = ms;
  startDurationMs_ = durationMs;
  startKey_ = queue_.currentKey();
  switch (state_) {
    case PlayState::Playing:
      failuresInARow_ = 0;
      startCurrent();  // there, now
      break;
    case PlayState::Paused:
    case PlayState::Waiting:
      // A held track (paused, or waiting to resume) is let go: the next
      // play starts the entry again, there.
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

bool PlaybackController::startPoint(uint32_t* ms, uint32_t* durationMs) const {
  if (!hasStartPoint()) return false;
  *ms = startMs_;
  *durationMs = startDurationMs_;
  return true;
}

bool PlaybackController::resumePoint(uint32_t* ms, uint32_t* durationMs) const {
  if (startPoint(ms, durationMs)) return true;
  if ((state_ != PlayState::Paused && state_ != PlayState::Waiting) || cued_ || !hasTrack()) return false;
  *ms = audio_.positionMs();
  *durationMs = audio_.durationMs();
  return *ms > 0;
}

void PlaybackController::pauseByTimer() {
  switch (state_) {
    case PlayState::Playing:
      audio_.pause();
      state_ = PlayState::Paused;
      break;
    case PlayState::Waiting:
      state_ = PlayState::Paused;  // as cancelWait(): a cued entry stays cued
      break;
    case PlayState::Paused:
      break;
    case PlayState::Stopped:
      return;
  }
  pausedByTimer_ = true;
}

void PlaybackController::pauseAtBoundary() {
  pauseAfter_ = false;
  ++timerStops_;
  if (!queue_.step(+1, repeat_)) {
    stop();  // the end of the queue, and no repeat: the natural stop
    return;
  }
  audio_.stop();  // the finished track lets go; the next one is cued
  state_ = PlayState::Paused;
  cued_ = true;
  pausedByTimer_ = true;
}

void PlaybackController::update(uint32_t nowMs) {
  (void)nowMs;  // the backend owns the clock via its own loop(); reserved here
  if (state_ != PlayState::Playing) return;
  if (audio_.failed()) {
    ++failure_.count;
    failure_.track = queue_.currentTrack();
    failure_.key = queue_.currentKey();
    failure_.rate = audio_.rateRefusal();
    if (++failuresInARow_ >= queue_.size()) {
      stop();  // every track failed in a row: nothing here plays
      return;
    }
    advance();
  } else if (audio_.finished()) {
    failuresInARow_ = 0;
    if (pauseAfter_) {
      pauseAtBoundary();  // the sleep timer: the next entry, paused at 0:00
      return;
    }
    advance();  // wraps to the start of the queue at the end (with repeat)
  }
}

void PlaybackController::currentMoved() {
  failuresInARow_ = 0;
  // The start point was the old entry's: dropped, not only hidden (an undo
  // that brings that entry back must not bring its second back too).
  clearStartPoint();
  if (!hasTrack()) {
    stop();
    return;
  }
  switch (state_) {
    case PlayState::Playing:
    case PlayState::Waiting:
      startCurrent();
      break;
    case PlayState::Paused:
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

bool PlaybackController::playNow(const uint32_t* tracks, uint32_t n, uint32_t start) {
  if (!queue_.replace(tracks, n, start)) return false;
  failuresInARow_ = 0;
  clearStartPoint();
  if (hasTrack()) {
    startCurrent();
  } else {
    stop();
  }
  return true;
}

QueueModel::Removed PlaybackController::remove(const uint32_t* positions, uint32_t n) {
  const QueueModel::Removed r = queue_.remove(positions, n);
  if (!r.current) return r;
  clearStartPoint();  // (as currentMoved(); stop() doesn't)
  if (r.pastEnd) {
    stop();  // nothing after it stayed: stopped, on the last track
  } else {
    currentMoved();
  }
  return r;
}

void PlaybackController::clearQueue() {
  clearStartPoint();  // an undo brings the queue back, not the second
  stop();
  queue_.clear();
}

bool PlaybackController::undo() {
  const uint32_t key = queue_.currentKey();
  if (!queue_.undo()) return false;
  if (queue_.currentKey() != key) currentMoved();
  return true;
}

void PlaybackController::queueReplaced(bool currentKept) {
  if (!currentKept) currentMoved();
}
