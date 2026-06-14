#pragma once
#include <cstddef>
#include <vector>
#include "Track.h"
#include "hal/IAudioBackend.h"

enum class PlayState { Stopped, Playing, Paused };

// Transport + playlist logic. Framework-agnostic: it talks only to an
// IAudioBackend and is driven by an injected clock (update(nowMs)), so it can
// be unit-tested on the host with a fake backend.
class PlaybackController {
public:
  explicit PlaybackController(IAudioBackend& audio) : audio_(audio) {}

  void setPlaylist(std::vector<Track> tracks);

  void play(size_t index);     // start a specific track
  void togglePlayPause();
  void next();
  void prev();
  void stop();

  // Advance state: auto-skips to the next track when the current one ends.
  void update(uint32_t nowMs);

  PlayState state() const { return state_; }
  int currentIndex() const { return index_; }
  bool hasTrack() const { return index_ >= 0 && index_ < static_cast<int>(playlist_.size()); }
  const Track* currentTrack() const { return hasTrack() ? &playlist_[index_] : nullptr; }
  const std::vector<Track>& playlist() const { return playlist_; }
  uint32_t positionMs() const { return audio_.positionMs(); }

private:
  void startCurrent();

  IAudioBackend& audio_;
  std::vector<Track> playlist_;
  int index_ = -1;
  PlayState state_ = PlayState::Stopped;
};
