#include "PlaybackController.h"

#include <utility>  // std::move

void PlaybackController::setPlaylist(std::vector<Track> tracks) {
  stop();
  playlist_ = std::move(tracks);
  index_ = playlist_.empty() ? -1 : 0;
}

void PlaybackController::startCurrent() {
  if (!hasTrack()) return;
  const Track& t = playlist_[index_];
  audio_.play(t.path, t.durationMs);
  state_ = PlayState::Playing;
  cued_ = false;
}

void PlaybackController::play(size_t index) {
  if (index >= playlist_.size()) return;
  failuresInARow_ = 0;
  index_ = static_cast<int>(index);
  startCurrent();
}

void PlaybackController::togglePlayPause() {
  switch (state_) {
    case PlayState::Stopped:
      if (!playlist_.empty()) play(index_ < 0 ? 0 : static_cast<size_t>(index_));
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
      audio_.resume();
      state_ = PlayState::Playing;
      break;
  }
}

void PlaybackController::next() {
  if (playlist_.empty()) return;
  failuresInARow_ = 0;
  advance();
}

void PlaybackController::advance() {
  index_ = (index_ + 1) % static_cast<int>(playlist_.size());
  startCurrent();
}

void PlaybackController::prev() {
  if (playlist_.empty()) return;
  failuresInARow_ = 0;
  const int n = static_cast<int>(playlist_.size());
  index_ = (index_ - 1 + n) % n;
  startCurrent();
}

void PlaybackController::cueNext() { cue(+1); }
void PlaybackController::cuePrev() { cue(-1); }

void PlaybackController::cue(int delta) {
  if (state_ == PlayState::Playing) {
    delta > 0 ? next() : prev();
    return;
  }
  if (playlist_.empty()) return;
  failuresInARow_ = 0;
  const int n = static_cast<int>(playlist_.size());
  index_ = ((index_ + delta) % n + n) % n;
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
    if (++failuresInARow_ >= playlist_.size()) {
      stop();  // every track failed in a row: nothing here plays
      return;
    }
    advance();
  } else if (audio_.finished()) {
    failuresInARow_ = 0;
    advance();  // wraps to the start of the playlist at the end
  }
}
