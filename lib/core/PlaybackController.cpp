#include "PlaybackController.h"

#include <string>

void PlaybackController::startCurrent() {
  if (!hasTrack()) return;
  if (held()) {
    // The output can't be heard yet: this entry waits, selected. The
    // backend lets go of what it had (a track playing or paused).
    if (state_ != PlayState::Stopped && !cued_) audio_.stop();
    state_ = PlayState::Waiting;
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
  audio_.play(std::string(path), catalog_.durationHintMs(id));
  state_ = PlayState::Playing;
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
        state_ = PlayState::Waiting;  // the backend keeps the paused track: release() resumes it
        break;
      }
      audio_.resume();
      state_ = PlayState::Playing;
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
  state_ = PlayState::Playing;
}

void PlaybackController::cancelWait() {
  if (state_ == PlayState::Waiting) state_ = PlayState::Paused;
}

void PlaybackController::next() {
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  advance();
}

void PlaybackController::advance() {
  if (!queue_.step(+1, repeat_)) {
    stop();  // the end of the queue, and no repeat
    return;
  }
  startCurrent();
}

void PlaybackController::prev() {
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  queue_.step(-1, repeat_);  // at the start without repeat: the first track again
  startCurrent();
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
}

void PlaybackController::update(uint32_t nowMs) {
  (void)nowMs;  // the backend owns the clock via its own loop(); reserved here
  if (state_ != PlayState::Playing) return;
  if (audio_.failed()) {
    ++failure_.count;
    failure_.track = queue_.currentTrack();
    failure_.key = queue_.currentKey();
    if (++failuresInARow_ >= queue_.size()) {
      stop();  // every track failed in a row: nothing here plays
      return;
    }
    advance();
  } else if (audio_.finished()) {
    failuresInARow_ = 0;
    advance();  // wraps to the start of the queue at the end (with repeat)
  }
}

void PlaybackController::currentMoved() {
  failuresInARow_ = 0;
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
  if (r.pastEnd) {
    stop();  // nothing after it stayed: stopped, on the last track
  } else {
    currentMoved();
  }
  return r;
}

void PlaybackController::clearQueue() {
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
