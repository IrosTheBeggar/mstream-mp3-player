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
// the one playing isn't in the restored queue, in the shuffle mode the edit
// was made in (Shuffle all's Play turns it on; its undo, off again). Edits
// that don't touch it (Play next, + Queue, Clear up next) change nothing
// that plays.
//
// Repeat (setRepeat(), docs/QUEUE-MODES.md section 3): Off, All or One.
// The rule: a natural end follows the mode; a skip wraps unless the mode is
// Off. All wraps from the last entry to the first, at an end as at a skip
// (gaplessly: the first entry is the word while the last plays). One plays
// the entry again at its natural end (gaplessly: its word is itself, a new
// token each loop, the self-join a queue of one on repeat always had;
// repeats() counts the loops); next, prev and a failure still move, and
// wrap at the queue's ends as All's do. Off: the end of the last track
// stops there, on it; next at the last entry stops too, and prev at the
// first plays the first again. The engine's own default is All (the host
// tests' wrapping); the firmware sets the saved mode at boot (Off unless
// changed).
//
// Shuffle (setShuffle(): QueueModel::setShuffled(), which reorders the
// entries) changes nothing that plays: the current entry is the same entry
// in the same state, its start point and length kept. The word on what
// follows moves to the new next entry (a cut, inside the decode-ahead
// window, or past the join the heard advance starts what now comes next).
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
//     of the queue with repeat Off it stops, as it would; with Repeat One
//     the same entry is cued at 0:00 (the timer wins: it pauses, and a later
//     play starts the track from the top, as One would have). timerStops()
//     counts those. A skip or a failure is no end: the flag stays for the
//     next track.
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
// (IAudioBackend::setNext(): the entry a natural end would start, endNext():
// the next one, wrapping unless repeat is Off, or the entry itself with
// Repeat One; its token kept while the same track stays next, a new one
// otherwise), or that nothing follows: gapless off, "pause after this
// track" set, the sleep timer ending at this entry (NextGate), or the end
// of the queue with repeat Off. Worked out again only when one of those changes (a
// signature of them), every update() and at the end of every action, so an
// edit's new word reaches the backend at once. When the backend reports
// that a joined track is heard (takeAdvance(), at the top of update() and
// of every action, so a next pressed just after a join skips the track
// that is heard), the player does what update() would do at its natural
// end right now:
//   - "pause after this track" or the timer ending here: the boundary's
//     pause (the next entry cued at 0:00), as at any natural end;
//   - what advance() would start isn't the joined track any more (an edit
//     that came too late to cut it out: Play next, a remove, repeat,
//     shuffle): that is started (advance(); paused: cued);
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
// point that waits, or a paused track's position. Either may carry a resume
// anchor (docs/SEEK.md section 5): the bytes that start the track on the
// very sample it paused at. The start point owns its anchor: both go
// together, and a start point set without one (qs) has none.
//
// A seek (Now Playing's seek bar, docs/SEEK-BAR.md: seek()) is a start
// point without an anchor, by the same rules: playing, it starts there at
// once (the ring cut, faded in, as a skip); paused or waiting, the held
// track is let go and the next play (or the wait's release) starts there,
// still paused or waiting; stopped, it waits. 0:00 is prev's restart (a
// start point of 0 only clears one). It acts only on the entry the finger
// landed on (its key), checked after the heard join is taken, with nothing
// between the check and the start that could take another: a join heard
// under the finger is never a seek of the next track to this one's second.
// It never asks for the tail rule's last 5 s (trackseek::seekLimitMs(): the
// length less 6 s). It never plays from a pause, never clears the timer's
// or the computer's marks, and leaves the queue, its undo and "pause after
// this track" as they are. Until the backend takes a start up, what Now
// Playing shows is where it asked to start (pendingStart()), with the
// length the player was told (lengthHint(); a skip's, none: never the
// track before's); after, the backend's, with that length while the
// backend knows none (shownTime()).
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
  // playNow()'s start: shuffled, a random track first; not shuffled, the first.
  static constexpr uint32_t kAnyStart = QueueModel::kAnyStart;
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
  // track's, the one told for the entry (lengthHint()), else the catalog's
  // hint).
  // Stopped: it waits. Playing: it starts there now (a wait for the
  // headphones: it starts there when they connect). Paused on a track the
  // backend holds: that track is let go, the next play starts there.
  // 0: none.
  // `anchor`: the resume point's (nullptr or kind None: none); it goes to
  // the backend with the play (IAudioBackend::StartAt).
  void setStartPoint(uint32_t ms, uint32_t durationMs, const ResumeAnchor* anchor = nullptr);
  // The current entry's start point, if one waits (and its anchor).
  bool startPoint(uint32_t* ms, uint32_t* durationMs, ResumeAnchor* anchor = nullptr) const;
  // Where the current entry would pick up after a boot: a start point that
  // waits, or the position of a paused track (Paused, or Waiting to resume
  // it). False while it plays (a second saved now would be stale at once)
  // and when it would start at 0:00 anyway (stopped, cued). `anchor`: the
  // start point's, or the backend's for the paused track
  // (IAudioBackend::resumeAnchor(); kind None: none). Paused before the
  // backend took a start up (within ~150 ms of a seek): where that start
  // was asked for, with lengthHint(), and no anchor.
  bool resumePoint(uint32_t* ms, uint32_t* durationMs, ResumeAnchor* anchor = nullptr) const;
  // Now Playing's seek bar: what seek() did.
  enum class Seek : uint8_t {
    Started,  // playing: it starts there now (the ring cut, faded in, as a skip)
    Waits,    // paused, waiting, stopped or held: the next play (or the release) starts there
    Moved,    // the current entry isn't `key` any more (a join heard, an end, a skip): nothing done
    NoPlace,  // no track, no length, or the backend's track failed: nothing done
  };
  // The current entry from `ms` in (at most trackseek::seekLimitMs()), a
  // track `durationMs` long (the bar's: the backend's hint), if it is still
  // the entry `key` (the one the finger landed on). The heard join is taken
  // first, so one heard since is Moved, never a seek of the next track. ms
  // 0: prev's restart (playing: from 0:00 again; paused or waiting: the
  // held track let go, cued at 0:00; stopped: a start point dropped).
  // Otherwise as setStartPoint() with no anchor. Never plays from a pause
  // and never clears the timer's or the computer's marks; the queue, its
  // undo and "pause after this track" stay as they are.
  Seek seek(uint32_t key, uint32_t ms, uint32_t durationMs);
  // A play the backend hasn't taken up yet (IAudioBackend::positionKnown()
  // false, while the player holds the entry's track): where it asked to
  // start. The backend's position may still be the track before's.
  bool pendingStart(uint32_t* ms) const;
  // The current entry's length as the player was last told it (a seek's, a
  // play's hint, the held track's at a restart); 0: none. Now Playing shows
  // it while the backend knows none.
  uint32_t lengthHint() const { return lengthKey_ == queue_.currentKey() ? lengthMs_ : 0; }
  // Where the current entry is and how long it is, as Now Playing shows
  // them (MainUiHost::snapshot(); 0 and 0 with no entry). A start point
  // that waits: its second and the length it went with. A start the
  // backend hasn't taken up yet (pendingStart()): where it was asked to
  // start, and lengthHint() alone: the backend's length may still be the
  // track before's (a skip's request before the decode task takes it up),
  // and the seek bar would take it for this entry's. Otherwise the
  // backend's, with lengthHint() while it knows none (a header-less file's
  // first second, a track let go at 0:00) unless the track failed.
  void shownTime(uint32_t* positionMs, uint32_t* durationMs) const;

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
  // Play: the queue becomes `tracks`, and the one at `start` plays
  // (shuffled: first, the rest shuffled after it; kAnyStart: a random one,
  // or the first when not shuffled).
  bool playNow(const uint32_t* tracks, uint32_t n, uint32_t start) { return playNow(tracks, n, start, shuffle()); }
  // Play with the shuffle mode set as part of it (Shuffle all: on): one
  // edit (QueueModel::replace(.., shuffled)), so undo() puts the queue and
  // the mode back. Out of memory: false, and neither changed. Past the
  // queue's cap (QueueModel::kMaxEntries), 5,000 of the tracks: the first
  // (or the window that holds `start`), or shuffled a random 5,000.
  bool playNow(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffle);
  // As many as fit under the cap (QueueModel::room()), the first ones;
  // false when none do (the queue full) or out of memory.
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
  // UI's note ("Skipped 07 - x.flac: can't play it", why its sample rate
  // was refused, or the backend's own few words: "surround Opus isn't
  // supported") and the Queue's mark on its row. `count` goes up by one
  // per failure.
  struct Failure {
    uint32_t count = 0;
    uint32_t track = QueueModel::kNone;  // its TrackCatalog id
    uint32_t key = QueueModel::kNone;    // its queue entry's key
    IAudioBackend::RateRefusal rate;     // hz 0: not its rate
    char note[48] = "";                  // IAudioBackend::failureNote() ("": nothing better than "can't play it")
  };
  const Failure& lastFailure() const { return failure_; }
  // The current entry can start part of the way in (IAudioBackend::
  // seekable(), by its path: Now Playing's seek bar shows a knob), whichever
  // way it became current: a play, a gapless join heard without a play(),
  // a cue, a boot's restore. No entry: false.
  bool seekable() const;

  // ---- repeat and shuffle (docs/QUEUE-MODES.md) ----
  enum class Repeat : uint8_t { Off, All, One };
  // An action: the word on what follows changes at once, not at the next
  // update().
  void setRepeat(Repeat r);
  Repeat repeat() const { return repeat_; }
  // An action: the queue's order (QueueModel::setShuffled()); the same
  // entry in the same state, nothing started, stopped or cued.
  void setShuffle(bool on);
  bool shuffle() const { return queue_.shuffled(); }
  // Repeat One's loops (a natural end that played the entry again),
  // free-running: main's "[queue] repeat one" line.
  uint32_t repeats() const { return repeats_; }
  // The seeks that did something (Started or Waits), free-running: the
  // card worker waits 2 s after one (ScanScheduler's seekSeq).
  uint32_t seeks() const { return seeks_; }

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
    uint8_t repeat = 0xFF;  // Repeat
    bool pauseAfter = false, gapless = false, gate = false;
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
  // A skip wraps unless repeat is Off; a natural end follows the mode.
  bool wraps() const { return repeat_ != Repeat::Off; }
  // What a natural end would start: the current position with One, else
  // the next (wrapping unless Off); kNone: nothing (the end, Off).
  uint32_t endNext() const;
  // A natural end's step: One stays (and counts a loop), else the next.
  // False: nothing to step to (the end, Off).
  bool stepAtEnd();
  // The next track without counting as a user action: at a natural end
  // (`atEnd`) by the repeat mode, else as a skip; stopped when it can't.
  void advance(bool atEnd);
  void cue(int delta);
  // Prev::Restart: the current entry from 0:00, in the same state.
  void restart();
  // setStartPoint()'s work, without an Act of its own (seek(): nothing may
  // take a join between its key check and the start).
  void placeStart(uint32_t ms, uint32_t durationMs, const ResumeAnchor* anchor);
  // The current entry's length as told (lengthHint()); 0 changes nothing.
  void noteLength(uint32_t ms) {
    if (ms > 0 && hasTrack()) {
      lengthKey_ = queue_.currentKey();
      lengthMs_ = ms;
    }
  }
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
    startAnchor_ = ResumeAnchor{};
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
  Repeat repeat_ = Repeat::All;
  uint32_t repeats_ = 0;
  uint32_t seeks_ = 0;
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
  ResumeAnchor startAnchor_;
  // Where the last play() asked to start (prevAction(): the position until
  // the backend takes that start up).
  uint32_t playedFromMs_ = 0;
  // lengthHint(): the length last told for the entry with key lengthKey_
  // (another entry current: none, with no bookkeeping; an entry never
  // changes its track, so the same key coming round again still has it).
  uint32_t lengthKey_ = QueueModel::kNone;
  uint32_t lengthMs_ = 0;

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
