#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

enum class PlayState { Stopped, Playing, Paused };

// Transport over the play queue. Framework-agnostic: it talks only to an
// IAudioBackend, reads the queue (QueueModel: track ids) and turns the
// current id into a path through the TrackCatalog when it starts a track,
// so it holds no strings of its own. Driven by an injected clock
// (update(nowMs)), so it can be unit-tested on the host with a fake backend.
//
// The queue's edits that touch what plays go through here (the queue's own
// rules are QueueModel's): playNow() starts the new queue; removing the
// current entry skips to the next one that stayed (or stops if none did);
// clearQueue() stops; undo() goes back to the track that was current if
// the one playing isn't in the restored queue. Edits that don't touch it
// (Play next, + Queue, Clear up next) change nothing that plays.
//
// At either end of the queue next() and prev() wrap around, as does a
// track ending (setRepeat(false): the end of the last track stops there).
class PlaybackController {
public:
  PlaybackController(IAudioBackend& audio, QueueModel& queue, const TrackCatalog& catalog)
      : audio_(audio), queue_(queue), catalog_(catalog) {}

  void play(size_t position);  // start the queue entry at `position`
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
  // be played. Stops once every track in the queue has failed in a row, so
  // an unplayable queue doesn't spin forever.
  void update(uint32_t nowMs);

  // ---- the queue's edits, with what they do to playback ----
  // Play: the queue becomes `tracks`, and the one at `start` plays.
  bool playNow(const uint32_t* tracks, uint32_t n, uint32_t start);
  bool playNext(const uint32_t* tracks, uint32_t n) { return queue_.insertNext(tracks, n); }
  bool addToQueue(const uint32_t* tracks, uint32_t n) { return queue_.append(tracks, n); }
  QueueModel::Removed remove(const uint32_t* positions, uint32_t n);
  bool moveNext(const uint32_t* positions, uint32_t n) { return queue_.moveNext(positions, n); }
  bool clearUpNext() { return queue_.clearUpNext(); }
  void clearQueue();
  bool undo();
  // The queue was replaced behind our back (restored from the card, or
  // remapped after a library rebuild). `currentKept`: the current entry is
  // still the track the backend has; otherwise, if it plays, the new
  // current one starts.
  void queueReplaced(bool currentKept);

  // The last track that couldn't be played (skipped by update()), for the
  // UI's note ("Skipped 07 - x.flac: can't play it") and the Queue's mark
  // on its row. `count` goes up by one per failure.
  struct Failure {
    uint32_t count = 0;
    uint32_t track = QueueModel::kNone;  // its TrackCatalog id
    uint32_t key = QueueModel::kNone;    // its queue entry's key
  };
  const Failure& lastFailure() const { return failure_; }

  void setRepeat(bool on) { repeat_ = on; }
  bool repeat() const { return repeat_; }

  PlayState state() const { return state_; }
  int currentIndex() const { return queue_.current(); }
  bool hasTrack() const { return queue_.current() >= 0; }
  uint32_t currentTrack() const { return queue_.currentTrack(); }  // TrackCatalog id, or kNone
  // The current track's path into buf (the length; 0 and "" if none).
  size_t currentPath(char* buf, size_t size) const { return catalog_.path(currentTrack(), buf, size); }
  const QueueModel& queue() const { return queue_; }
  const TrackCatalog& catalog() const { return catalog_; }
  uint32_t positionMs() const { return audio_.positionMs(); }

private:
  void startCurrent();
  void advance();  // next track without counting as a user action
  void cue(int delta);
  // The current entry changed under the backend: carry on from the new one
  // in the same state (Playing starts it, Paused cues it, Stopped waits).
  void currentMoved();

  IAudioBackend& audio_;
  QueueModel& queue_;
  const TrackCatalog& catalog_;
  PlayState state_ = PlayState::Stopped;
  bool repeat_ = true;
  // Paused on a cued track: the backend holds nothing, so a resume starts it.
  bool cued_ = false;
  // Tracks that failed since the last one that played through or the last
  // user action.
  size_t failuresInARow_ = 0;
  Failure failure_;
};
