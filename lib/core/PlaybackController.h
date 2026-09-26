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
  // Move to the next or previous track without starting it: Stopped stays
  // Stopped; Paused stays Paused, on the new track from its start (the old
  // one is dropped), and the next togglePlayPause() starts it. While Playing
  // the same as next()/prev().
  void cueNext();
  void cuePrev();

  // Advance state: moves to the next track when the current one ends or can't
  // be played. Stops once every track in the playlist has failed in a row, so
  // an unplayable playlist doesn't spin forever.
  void update(uint32_t nowMs);

  PlayState state() const { return state_; }
  int currentIndex() const { return index_; }
  bool hasTrack() const { return index_ >= 0 && index_ < static_cast<int>(playlist_.size()); }
  const Track* currentTrack() const { return hasTrack() ? &playlist_[index_] : nullptr; }
  const std::vector<Track>& playlist() const { return playlist_; }
  uint32_t positionMs() const { return audio_.positionMs(); }

private:
  void startCurrent();
  void advance();  // next track without counting as a user action
  void cue(int delta);

  IAudioBackend& audio_;
  std::vector<Track> playlist_;
  int index_ = -1;
  PlayState state_ = PlayState::Stopped;
  // Paused on a cued track: the backend holds nothing, so a resume starts it.
  bool cued_ = false;
  // Tracks that failed since the last one that played through or the last
  // user action.
  size_t failuresInARow_ = 0;
};
