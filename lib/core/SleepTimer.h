// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "PlaybackController.h"

class LibraryIndex;

// The sleep timer (docs/ENERGY.md section 3): the listener falls asleep
// with music on; the timer fades it out, pauses, turns the screen off, and
// 5 min later lets the headphones go (their battery, their keys). Chained
// with the idle power-off, "plays until flat" becomes "off with charge left
// in the morning".
//
// Choices: 15, 30, 45, 60 or 90 min (and any length from the console's
// Ts<sec>, for tests); End of track; End of album (pause when the next queue
// entry is on another album; with no album, another folder); End of queue.
// RAM only: a reboot or power-off clears it (nothing to time after a boot,
// which comes up stopped).
//
// At expiry, in this order:
//   1. The fade, our gain only (FadeStage: no AVRCP absolute volume, the
//      headphones' own level untouched): linear in dB from 0 to -40 dB,
//      then 0. A timed choice fades over the 30 s after it expires, and
//      pauses where the fade ends. End of track and End of album fade over
//      the track's last 10 s when its length is known, else not at all, and
//      the player pauses at the boundary itself
//      (PlaybackController::setPauseAfterTrack(): it moves to the next entry
//      and stays paused at 0:00). End of queue is the same on the queue's
//      last entry (repeat off: the natural stop).
//   2. Pause, never stop: the player's pauseByTimer() (a play waiting for
//      the headphones ends paused; a pause that is there is marked). The
//      mark: headphone Play doesn't resume it (in-ear detection sends Play
//      when a sleeper turns over); the Core2's own play does.
//   3. The factor back to 1.0 (restore), only once the pause is confirmed:
//      kSettleMs without playing, well past the pause's own 64-frame fade,
//      so nothing is heard. A resume then fades in from silence at the
//      listener's own level, as any resume does. Nothing resumes by itself.
//   4. The screen off (ScreenPower's Off, the pocket guard for any wake).
//   5. After kReleaseMs paused: the headphones released (no "lost" dialog,
//      the reconnect resting, the output still Bluetooth, connectable).
//      A play before then cancels it.
//
// The rules around it:
//   - The fade only falls by itself. It rises only on +10 min or Turn off
//     (the stage's slow rise, ~20 dB/s), and restore() while silent.
//   - A skip during the countdown or the fade: the timer runs on, a fade
//     keeps its level (the new track starts faded). End of track then
//     applies to the new track; End of album reads the new entry's album.
//   - A pause during the countdown: the timer runs on (as on phones). A
//     pause during the fade finishes it at once (steps 2-5). A timer that
//     expires while paused (or stopped, or waiting for the headphones)
//     skips the fade.
//   - The timer never moves the output (a drop pauses as ever; an output
//     switch pauses first, as ever: the factor carries over).
//
// Portable, host-tested (test_sleep_timer); main.cpp feeds it every loop
// pass (before PlaybackController::update) and carries out what it says.
class SleepTimer {
public:
  enum class Choice : uint8_t { Off, Timed, EndOfTrack, EndOfAlbum, EndOfQueue };
  enum class Phase : uint8_t {
    Off,       // nothing to time
    Counting,  // a timed choice counts down
    Armed,     // End of track / album / queue: the boundary is ahead
    Fading,    // the fade runs
    Ending,    // expired: the pause asked for, waiting for it to be confirmed
    Ended,     // done (shown as off): the headphones are released after kReleaseMs paused
  };

  static constexpr int kTimedChoices = 5;
  static constexpr uint32_t kMinutes[kTimedChoices] = {15, 30, 45, 60, 90};
  static constexpr uint32_t kFadeMs = 30000;       // a timed choice's fade
  static constexpr uint32_t kTrackFadeMs = 10000;  // End of track / album / queue: the track's last 10 s
  static constexpr uint32_t kZeroMs = 100;         // after -40 dB: 0, then the pause
  static constexpr uint32_t kSettleMs = 250;       // paused this long: nothing is heard any more
  static constexpr uint32_t kExtendMs = 10u * 60u * 1000u;
  static constexpr uint32_t kReleaseMs = 5u * 60u * 1000u;
  static constexpr float kFloorDb = -40.0f;
  static constexpr uint16_t kUnity = 32768;

  // ---- the listener (the sheet, the toast, the console) ----
  // A timed choice (any length: the console's Ts<sec>): restarts from now;
  // a fade that runs comes back up slowly.
  void setTimed(uint32_t ms, uint32_t nowMs);
  // End of track / album / queue.
  void setEnd(Choice c);
  // +10 min: added to what is left, never less than before (a fade comes
  // back up slowly and the timer counts again, 10 min from now). An end-of
  // choice on its boundary track (End of track always; End of album or
  // queue on the album's or queue's last track) becomes a timed one: what
  // is left of the track (`trackLeftMs`) plus 10 min. Earlier in the album
  // or the queue what is left isn't known, so there is nothing to add to:
  // refused (canExtend()), as with a track of unknown length. A track's
  // fade after a skip away from the boundary track: back to armed, the
  // level back up. False: refused, or nothing runs.
  bool extend(uint32_t nowMs, uint32_t trackLeftMs);
  // What extend() would do something now (the sheet's +10 min is live).
  bool canExtend() const;
  // Turn off: a fade comes back up slowly.
  void cancel();

  // ---- every loop pass ----
  struct In {
    uint32_t nowMs = 0;
    PlayState play = PlayState::Stopped;
    // PlaybackController::timerStops(): counts its pauses at a boundary
    // (setPauseAfterTrack()), free-running.
    uint32_t boundaryStops = 0;
    uint32_t positionMs = 0;  // in the current track
    uint32_t durationMs = 0;  // its length, 0 unknown
    bool lastOfAlbum = false;  // the next queue entry is on another album (or there is none)
    bool lastOfQueue = false;  // the current entry is the queue's last
  };
  struct Out {
    uint16_t fadeQ15 = kUnity;     // FadeStage::setTarget()
    bool restore = false;          // FadeStage::restore(): the pause is confirmed, nothing is heard
    bool pauseAfterTrack = false;  // PlaybackController::setPauseAfterTrack()
    bool pauseNow = false;         // PlaybackController::pauseByTimer()
    bool screenOff = false;        // ScreenPower's Off
    bool release = false;          // the headphones: releaseHeadphones()
    bool expired = false;          // (the log) it went off this pass
    bool fadeStarted = false;      // (the log, the UI's toast)
  };
  Out update(const In& in);

  // ---- what it shows ----
  Phase phase() const { return phase_; }
  Choice choice() const { return phase_ == Phase::Off || phase_ == Phase::Ended ? Choice::Off : choice_; }
  // Counting, Armed or Fading: the moon is shown, and +10 min / Turn off.
  bool running() const { return phase_ == Phase::Counting || phase_ == Phase::Armed || phase_ == Phase::Fading; }
  bool fading() const { return phase_ == Phase::Fading; }
  // The fade counts down to the pause now: a timed fade (its 30 s), or a
  // track's in the boundary track's last 10 s (the last update()'s). Not a
  // track's fade held after a skip (to a track that isn't the boundary's,
  // or one with more than 10 s left): that can last a whole track or an
  // album, with the toast still up. What holds a lit screen lit (main.cpp:
  // ScreenPower's holdLit), never longer than the fade itself.
  bool fadeCountingDown() const { return phase_ == Phase::Fading && (!trackFade_ || trackFadeLive_); }
  // The headphones' release is still to come (Ended).
  bool releasePending() const { return phase_ == Phase::Ended; }
  // Counting: ms to expiry (0 otherwise).
  uint32_t msLeft(uint32_t nowMs) const;
  // The timed choice shown outlined on the sheet (0..kTimedChoices-1), or -1.
  int timedIndex() const;
  // The fade's target now (the last update()'s).
  uint16_t fadeQ15() const { return held_; }

  // "Off", "23 min", "45 s" (the last minute), "End of track", "End of
  // album", "End of queue", "Fading": the "..." sheet's row.
  void rowText(uint32_t nowMs, char* buf, size_t size) const;
  // "23 min", "45 s", "track", "album", "queue", "fading" (Now Playing's
  // progress line, beside the moon); "" when it doesn't run.
  void shortText(uint32_t nowMs, char* buf, size_t size) const;
  // The same in a sentence, lower case: "23 min left", "45 s left", "end
  // of track", "fading", "off" (the Sleep timer sheet's title, after
  // "Sleep timer: ").
  void titleText(uint32_t nowMs, char* buf, size_t size) const;
  static const char* phaseName(Phase p);
  static const char* choiceName(Choice c);

  // The fade's toast (ui/Overlays' Toast: "Sleep timer: fading" [+10 min]
  // [Turn off], UiText's kSleepToast*): what a tap at `x` on it does. Both
  // buttons raise the level, so neither acts:
  //   - on a clamped reading at the right edge (`clampedRight`,
  //     InputEvent::atRightEdge()): fabric pressure in a pocket makes them;
  //   - on the touch that attended a screen woken from off
  //     (`landedUnattended`, ScreenPower::landedUnattended()): in a pocket
  //     the first contact wakes it (swallowed) and the next lands on the
  //     lit toast. The listener's next tap acts.
  // Each button's hit area reaches half the gap to the other; Turn off the
  // screen's edge.
  enum class ToastButton : uint8_t { None, Extend, TurnOff };
  static ToastButton toastTap(int x, bool clampedRight, bool landedUnattended);

  // The fade curve: `p` 0..1 of the way, linear in dB from 0 to -40 dB (Q15).
  static uint16_t curveQ15(float p);

  // End of album: the next queue entry's track `b` is on another album
  // than `a`: the album's; with no album information (the "loose tracks",
  // album ""), the folder. A built-in track, or one the index doesn't
  // have, is an album of its own. `b` kNone (no next entry): true.
  static bool albumEndsBetween(const LibraryIndex* index, uint32_t a, uint32_t b);

private:
  bool atBoundaryTrack(const In& in) const;
  void beginEnding(bool faded);

  Choice choice_ = Choice::Off;
  Phase phase_ = Phase::Off;
  uint32_t endAtMs_ = 0;     // Counting: the expiry
  uint32_t timedMs_ = 0;     // the timed choice's length (the sheet's outline)
  uint32_t fadeFromMs_ = 0;  // Fading (timed): its start
  bool trackFade_ = false;   // Fading: the track's last 10 s (not a timed fade)
  uint16_t held_ = kUnity;   // the fade's target: only ever lower while it runs
  uint32_t seenStops_ = 0;
  bool stopsSeen_ = false;
  bool quiet_ = false;       // Ending: not playing since quietSinceMs_
  uint32_t quietSinceMs_ = 0;
  uint32_t endedAtMs_ = 0;   // Ended: since when
  bool atBoundary_ = false;  // the last update(): the current track is the boundary's
  bool knownLength_ = false; // ... and its length is known
  bool trackFadeLive_ = false; // ... and in its last kTrackFadeMs (fadeCountingDown())
};

// Whether the backend's position and length are the current queue entry's
// yet. A skip changes the entry at once, but the backend starts it a
// moment later, and until then it reports the last track's (near its end,
// End of track would fade the new one to about -40 dB at once, and hold
// it there: the fade never rises by itself). main.cpp feeds it every pass
// and hands the timer a length of 0 (unknown) until it says started.
// Started: the entry changed while the position was under 1 s (its own
// start already, or the last one barely begun), or since the change the
// backend started a track (its start count moved) or the position went
// back. Portable, host-tested (test_sleep_timer).
class EntryStart {
public:
  static constexpr uint32_t kFreshMs = 1000;
  // `key`: the current entry (QueueModel's key); `startSeq`: the backend's
  // start count; `positionMs`: its position. True: they are this entry's.
  bool update(uint32_t key, uint32_t startSeq, uint32_t positionMs);
  bool started() const { return started_; }

private:
  bool seen_ = false;
  bool started_ = false;
  uint32_t key_ = 0;
  uint32_t seq_ = 0;
  uint32_t pos_ = 0;
};
