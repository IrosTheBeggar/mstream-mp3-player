// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

// Waiting: a play the output can't carry yet (Bluetooth is the output and
// the headphones aren't connected): nothing starts and the position holds
// until PlayGate releases it (they connected) or it ends paused (cancelled,
// or they couldn't be reached). Never "Playing" meanwhile.
enum class PlayState { Stopped, Playing, Paused, Waiting };

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
//
// Prev restarts a track past its first 3 s (prevRule(), for every prev: the
// A click, Now Playing's, the console's p, the headphones'): back to 0:00
// of the same entry in the same state (playing: it starts again, faded in
// as any start; paused: it stays paused at 0:00, the backend letting go of
// the track, so nothing starts out loud). A second prev within the first
// 3 s goes to the entry before, as does one while stopped, on a cued entry
// (at 0:00 already) or on a track that failed. Until the backend has taken
// a start up (IAudioBackend::positionKnown()) its position may be the
// track before's: the position is then where that play asked to start (a
// quick second prev after a restart or a skip is at 0:00, the entry
// before; one right after a resume point's start at 2:30 is at 2:30, a
// restart). A restart is no skip and no edit: the queue, its undo and the
// sleep timer's "pause after this track" stay as they were.
//
// A Hold (main.cpp's: Bluetooth is the output and the headphones aren't
// connected) turns every play into a wait (PlayState::Waiting): a new track
// (play, playNow, next, prev, a resume of a cued entry) is only selected, the
// backend dropping what it had; a resume leaves the paused track where it is.
// Skips while waiting stay waiting, on the new track. togglePlayPause() while
// waiting cancels it (Paused); release() plays what waits; cancelWait() ends
// it paused (PlayGate decides which).
//
// The sleep timer (SleepTimer, docs/ENERGY.md section 3) asks two things:
//   - "pause after this track" (setPauseAfterTrack()): at the track's
//     natural end the player moves to the next entry and stays paused there
//     at 0:00 (cued: a later play starts it from its beginning); at the end
//     of the queue without repeat it stops, as it would. timerStops() counts
//     those. A skip or a failure is no end: the flag stays for the next track.
//   - pauseByTimer(): Playing pauses, Waiting ends paused, Paused stays.
// Either way the pause is marked "paused by the timer" (pausedByTimer()):
// headphone Play (HeadsetKeys) doesn't resume it, since in-ear detection
// sends Play when a sleeper turns over. Any play clears the mark (the
// Core2's own buttons, the screen, the console: the headphones can't).
// The USB visualizer's pause (pauseByComputer()) is marked the same way
// (pausedByComputer()), for the same reason: a bud put back in sends Play.
//
// Gapless playback (docs/GAPLESS.md, setGapless(): on by default): while
// the backend holds the current entry's track (playing, paused, or waiting
// to resume it), the player tells it what advance() would start next
// (IAudioBackend::setNext(): the entry QueueModel::peek(+1, repeat) names,
// its token kept while the same track stays next, a new one otherwise),
// or that nothing follows: gapless off, "pause after this track" set, the
// sleep timer ending at this entry (NextGate), or the end of the queue
// without repeat. Worked out again only when one of those changes (a
// signature of them), every update() and at the end of every action, so an
// edit's new word reaches the backend at once. When the backend reports
// that a joined track is heard (takeAdvance(), at the top of update() and
// of every action, so a next pressed just after a join skips the track
// that is heard), the player does what update() would do at its natural
// end right now:
//   - "pause after this track" or the timer ending here: the boundary's
//     pause (the next entry cued at 0:00), as at any natural end;
//   - what advance() would start isn't the joined track any more (an edit
//     that came too late to cut it out: Play next, a remove, repeat): that
//     is started (advance(); paused: cued);
//   - otherwise the joined entry becomes current without a play(): the
//     state, the queue and the backend stay as they are, the start point
//     goes and the failure count starts again.
//
// A start point (QueueStore's resume point after a boot, the console's qs):
// the current entry's next start begins that far in. Nothing plays by
// itself: it only waits for the next play (or, set while playing, starts
// there at once). It belongs to that entry: next, another entry, or an
// edit that changes the current entry drops it for good (an undo that
// brings the entry back doesn't bring the second back); prev on it goes to
// 0:00 of the same entry, whatever the second, and starts nothing (as a
// paused track's restart: stopped after a boot stays stopped). Its length
// goes to the backend with the play (it places a VBR MP3 without a table
// of contents by it). resumePoint() is what QueueSaver saves: a start
// point that waits, or a paused track's position.
class PlaybackController {
public:
  // Whether a play must wait for the output (read at every start).
  class Hold {
  public:
    virtual bool holdPlay() const = 0;

  protected:
    ~Hold() = default;
  };

  // Whether the sleep timer ends at the current entry (main.cpp's: End of
  // track always; End of album or queue on its last track): the track
  // after it is never decoded ahead, so the pause at the boundary hears
  // nothing of it.
  class NextGate {
  public:
    virtual bool endsHere() const = 0;

  protected:
    ~NextGate() = default;
  };

  PlaybackController(IAudioBackend& audio, QueueModel& queue, const TrackCatalog& catalog)
      : audio_(audio), queue_(queue), catalog_(catalog) {}

  // nullptr (the default): nothing waits.
  void setHold(const Hold* hold) { hold_ = hold; }
  // nullptr (the default): no sleep timer.
  void setNextGate(const NextGate* gate) { gate_ = gate; }
  // Gapless playback (the console's G0/G1): off, the backend is told that
  // nothing follows (a track decoded ahead is cut back out if not heard
  // yet) and every track ends as before.
  void setGapless(bool on);
  bool gapless() const { return gapless_; }
  // The console's G: what the player has done with joins since boot.
  struct GaplessStats {
    uint32_t offers = 0;     // words sent (setNext())
    uint32_t adopted = 0;    // joined tracks taken as the current entry
    uint32_t restarted = 0;  // joined tracks that weren't what came next any more: started again
    uint32_t paused = 0;     // the boundary's pause, at a joined track
  };
  const GaplessStats& gaplessStats() const { return gaplessStats_; }
  // The word sent last: the token (0: nothing follows) and the entry's key.
  uint32_t offeredToken() const { return sentToken_; }
  uint32_t offeredKey() const { return offer_.key; }
  // Waiting: plays now, whatever the hold says (the headphones connected, or
  // the listener chose the speaker). Anything else: nothing.
  void release();
  // Waiting: ends paused, on the entry that waited (cancelled, or the
  // headphones couldn't be reached). Anything else: nothing.
  void cancelWait();

  void play(size_t position);  // start the queue entry at `position`
  // Stopped or Paused: plays (or waits); Playing: pauses; Waiting: cancels
  // the wait (Paused).
  void togglePlayPause();
  // The USB visualizer's entry. Playing: pauses; Waiting: the wait ends
  // paused (as cancelWait()); either way the pause is marked the
  // computer's (pausedByComputer()). Stopped and Paused stay as they are,
  // and so does their mark: it never starts anything (togglePlayPause()
  // would play from Paused or Stopped).
  void pauseByComputer();
  void next();
  // prevAction()'s: the entry before (and it plays, or waits), or this one
  // from 0:00 in the same state.
  void prev();
  void stop();
  // stop(), keeping the listener's place in the current entry: a paused (or
  // waiting-to-resume) track's position, or a playing one's, becomes a
  // start point (a waiting one stays), so the next play, or the next boot's
  // resume point, picks up there. For the console's tests that borrow the
  // backend (Rt, Rb, b<n>). True when a place was kept.
  bool stopKeepingPlace();
  // Move to the next or previous track without starting it: Stopped stays
  // Stopped; Paused stays Paused, on the new track from its start (the old
  // one is dropped), and the next togglePlayPause() starts it. While Playing
  // (or Waiting) the same as next()/prev().
  // cuePrev() restarts as prev() does (paused: at 0:00, still paused).
  void cueNext();
  void cuePrev();

  // ---- prev: this track again, or the one before ----
  static constexpr uint32_t kRestartAfterMs = 3000;
  enum class Prev : uint8_t {
    Previous,  // the entry before (at the first, without repeat: the first again)
    Restart,   // this entry from 0:00, in the same state (a start point dropped)
  };
  // The rule. A start point waiting: Restart (its 0:00, whatever the
  // second). Stopped: Previous. Otherwise Restart once more than
  // kRestartAfterMs in, Previous at kRestartAfterMs or less, or when the
  // position means nothing (`positionKnown` false: a track that failed; a
  // cued entry is at 0, known; a start the backend hasn't taken up yet is
  // where it was asked to start).
  static Prev prevRule(PlayState state, bool startPointWaits, bool positionKnown, uint32_t positionMs);
  // prevRule() for the player as it is now: what prev() or cuePrev() would do.
  Prev prevAction() const;

  // ---- starting part of the way in ----
  // The current entry's next start begins `ms` in (a track `durationMs`
  // long then: what Now Playing shows until it plays, and the backend's
  // hint; 0: the length as known here, a waiting start point's, the held
  // track's, else the catalog's hint).
  // Stopped: it waits. Playing: it starts there now (a wait for the
  // headphones: it starts there when they connect). Paused on a track the
  // backend holds: that track is let go, the next play starts there.
  // 0: none.
  void setStartPoint(uint32_t ms, uint32_t durationMs);
  // The current entry's start point, if one waits.
  bool startPoint(uint32_t* ms, uint32_t* durationMs) const;
  // Where the current entry would pick up after a boot: a start point that
  // waits, or the position of a paused track (Paused, or Waiting to resume
  // it). False while it plays (a second saved now would be stale at once)
  // and when it would start at 0:00 anyway (stopped, cued).
  bool resumePoint(uint32_t* ms, uint32_t* durationMs) const;

  // ---- the sleep timer ----
  // At the current track's natural end: the next entry, paused at 0:00.
  void setPauseAfterTrack(bool on) { pauseAfter_ = on; }
  bool pauseAfterTrack() const { return pauseAfter_; }
  // Pauses (a wait ends paused) and marks it the timer's. Stopped: nothing.
  void pauseByTimer();
  // Paused by the timer and not played since: headphone Play doesn't resume.
  bool pausedByTimer() const { return pausedByTimer_; }
  // Paused by pauseByComputer() and not played since: the same (in-ear
  // detection's Play isn't the listener asking for the Core2's music back).
  bool pausedByComputer() const { return pausedByComputer_; }
  // Either: a pause the listener didn't make (HeadsetKeys).
  bool pausedNotByListener() const { return pausedByTimer_ || pausedByComputer_; }
  // The pauses (or the stop, at the queue's end) setPauseAfterTrack() made,
  // free-running.
  uint32_t timerStops() const { return timerStops_; }

  // Advance state: moves to the next track when the current one ends or can't
  // be played. Stops once every track in the queue has failed in a row, so
  // an unplayable queue doesn't spin forever.
  void update(uint32_t nowMs);

  // ---- the queue's edits, with what they do to playback ----
  // Play: the queue becomes `tracks`, and the one at `start` plays.
  bool playNow(const uint32_t* tracks, uint32_t n, uint32_t start);
  bool playNext(const uint32_t* tracks, uint32_t n);
  bool addToQueue(const uint32_t* tracks, uint32_t n);
  QueueModel::Removed remove(const uint32_t* positions, uint32_t n);
  bool moveNext(const uint32_t* positions, uint32_t n);
  bool clearUpNext();
  void clearQueue();
  bool undo();
  // The queue was replaced behind our back (restored from the card, or
  // remapped after a library rebuild). `currentKept`: the current entry is
  // still the track the backend has; otherwise, if it plays, the new
  // current one starts.
  void queueReplaced(bool currentKept);

  // The last track that couldn't be played (skipped by update()), for the
  // UI's note ("Skipped 07 - x.flac: can't play it", or why its sample rate
  // was refused) and the Queue's mark on its row. `count` goes up by one
  // per failure.
  struct Failure {
    uint32_t count = 0;
    uint32_t track = QueueModel::kNone;  // its TrackCatalog id
    uint32_t key = QueueModel::kNone;    // its queue entry's key
    IAudioBackend::RateRefusal rate;     // hz 0: not its rate
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
  // Every public action: the heard advance first, the word to the backend
  // after (nested actions do it again: harmless).
  class Act {
  public:
    explicit Act(PlaybackController& p) : p_(p) { p_.syncHeard(); }
    ~Act() { p_.refreshOffer(); }

  private:
    PlaybackController& p_;
  };
  // An entry offered (its token's), for the advance.
  struct Offered {
    uint32_t token = 0;
    uint32_t key = QueueModel::kNone;
    uint32_t track = QueueModel::kNone;
  };
  struct Signature {
    uint32_t position = 0, content = 0, heard = 0;
    bool repeat = false, pauseAfter = false, gapless = false, gate = false;
    bool operator==(const Signature& o) const {
      return position == o.position && content == o.content && heard == o.heard && repeat == o.repeat &&
             pauseAfter == o.pauseAfter && gapless == o.gapless && gate == o.gate;
    }
  };

  bool held() const { return hold_ && hold_->holdPlay(); }
  // The backend reports joined tracks heard (see the class).
  void syncHeard();
  // The word on what follows, sent when it changed.
  void refreshOffer();
  void remember(const Offered& o);
  const Offered* offered(uint32_t token) const;
  // update()'s natural end and failure handling.
  void checkEnd();
  // The current entry plays, or (held) waits.
  void startCurrent();
  void startNow();
  void advance();  // next track without counting as a user action
  void cue(int delta);
  // Prev::Restart: the current entry from 0:00, in the same state.
  void restart();
  // The current entry changed under the backend: carry on from the new one
  // in the same state (Playing starts it, Paused cues it, Stopped waits).
  void currentMoved();
  // The track ended with "pause after this track": the next entry, cued.
  void pauseAtBoundary();
  bool hasStartPoint() const {
    return startMs_ > 0 && startKey_ != QueueModel::kNone && startKey_ == queue_.currentKey();
  }
  void clearStartPoint() {
    startMs_ = 0;
    startKey_ = QueueModel::kNone;
  }
  // Playing or Waiting from now: any play clears the timer's and the
  // computer's marks.
  void setPlaying(PlayState s) {
    state_ = s;
    pausedByTimer_ = pausedByComputer_ = false;
  }

  IAudioBackend& audio_;
  QueueModel& queue_;
  const TrackCatalog& catalog_;
  const Hold* hold_ = nullptr;
  PlayState state_ = PlayState::Stopped;
  bool repeat_ = true;
  // Paused (or Waiting) on a cued track: the backend holds nothing, so a
  // resume starts it.
  bool cued_ = false;
  // Tracks that failed since the last one that played through or the last
  // user action.
  size_t failuresInARow_ = 0;
  Failure failure_;
  bool pauseAfter_ = false;
  bool pausedByTimer_ = false;
  bool pausedByComputer_ = false;
  uint32_t timerStops_ = 0;
  // The start point: this far into the entry with key startKey_.
  uint32_t startMs_ = 0;
  uint32_t startDurationMs_ = 0;
  uint32_t startKey_ = QueueModel::kNone;
  // Where the last play() asked to start (prevAction(): the position until
  // the backend takes that start up).
  uint32_t playedFromMs_ = 0;

  // Gapless playback.
  const NextGate* gate_ = nullptr;
  bool gapless_ = true;
  uint32_t heardToken_ = 0;  // the backend's track: 0 the last play()'s, else a joined one's
  uint32_t nextToken_ = 0;   // the last token given
  Offered offer_;            // the entry offered now (token 0: none)
  Offered history_[4];       // the last offers, for an advance that comes late
  uint8_t historyAt_ = 0;
  bool sent_ = false;        // a word went to the backend since its last play()
  uint32_t sentToken_ = 0;
  uint32_t sentAfter_ = 0;
  Signature signature_;
  GaplessStats gaplessStats_;
};
