// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "InputEvent.h"

// Now Playing's progress line as a seek bar (docs/SEEK-BAR.md): where the
// finger means, and what one touch on the bar comes to. No drawing and no
// clock of its own (the events carry their times): the page draws what
// this says, and calls the player (PlaybackController::seek()) once, when
// a touch ends in a seek.
//
// - A tap goes to the second under the finger (where it landed). A tap on
//   the knob (in the stay detent, below) is no seek.
// - A sideways drag (|dx| >= |dy| at the DragStart) scrubs: the knob and
//   the readout follow the finger while the music plays on where it was,
//   and the lift seeks once, to the second the readout showed. A drag that
//   isn't sideways is let go: nothing, then or at the lift.
// - The grab. A Down within kGrabPx of the knob (a reading that wasn't
//   clamped) grabs it: it stays where it is at the DragStart (where it
//   plays then: a finger that rested on it while the music played on
//   doesn't pull it back to the Down's x), then moves by the finger's
//   movement, with no jump (only differences count, so the panel's few px
//   of calibration error don't matter). The anchor is that time, not the
//   knob's pixel (one is 1/296 of the length: 177 ms on a 52 s track):
//   rounded to it, a grab that began in a second's first pixel aimed at
//   the second before. Anywhere else the knob comes to the finger.
// - Whole seconds. The line is x kLineX to kLineX + kLineW - 1 (the
//   drawing's); msAt() floors to a second, which the readout shows and the
//   seek asks for. The far left is 0:00; the far right the reach,
//   trackseek::seekLimitMs() (the length less 6 s: the tail rule turns a
//   start in the last 5 s into 0:00). A reading clamped at either screen
//   edge (InputEvent::edges) reaches the end on its side: the user's
//   calibrated panel never reads x below ~26 or above ~281, so without the
//   flags neither end could be reached. The length is frozen at the Down,
//   so a length estimate that settles can't move the knob under a still
//   finger.
// - The stay detent. While scrubbing, a marker shows where it plays (the
//   live position, which moves while playing). The touch is "staying" when
//   its target is the second already playing, or when the finger's place
//   (where it puts the knob, before the whole second) is within kStayPx
//   line px of the marker: the knob snaps onto the marker, the readout
//   says "no change", and a lift (or a tap) seeks nothing (a paused track
//   keeps its exact resume anchor). Not the target's x: that is its
//   second's start, up to a second's px behind the finger. A second is
//   wider than kStayPx on a track under 74 s (296 px / 4 px), so a lift on
//   the knob could seek to the second playing; and the one before starts a
//   second and a px or two behind, so up to about 99 s (a second over
//   3 px) a tap 1 px left of the knob, or a drift of 1 px, sought it.
//   Measured against where it plays at each event, the lift's included. A
//   finger that moves into it ticks; a knob grab that starts in it, and
//   the marker coming onto a still knob (at a pass or at an event), don't.
// - Off. Onto the artist row or above (y < kOffAboveY: Now Playing's
//   layout, its static_asserts tie these rows to it) or onto the button
//   strip (y >= kOffBelowY) the knob goes back to where it plays and a lift
//   seeks nothing; back from kBackAboveY down (or above kBackBelowY) it
//   scrubs again. Off and back tick once each; the 8 px between stop it
//   flapping.
// - One seek per touch at most: a Tap or a DragEnd. A Cancel (a sheet or a
//   dialog took the touch, the screen dimmed under it) or a Release ends
//   it with none.
//
// Portable, host-tested (test_seek_bar, with a real TouchRecognizer).
class SeekBar {
public:
  static constexpr int kLineX = 12, kLineW = 296;  // the line: x 12-307
  static constexpr uint32_t kMinLengthMs = 10000;  // shorter tracks: inert
  static constexpr int kGrabPx = 16, kStayPx = 4;
  static constexpr int kOffAboveY = 106, kBackAboveY = 114, kOffBelowY = 240, kBackBelowY = 232;
  // The readout goes to the side away from the knob: left once the knob is
  // right of kReadoutLeftX, right once it is left of kReadoutRightX; in
  // between it stays (at the scrub's start: left from kReadoutStartX).
  static constexpr int kReadoutLeftX = 184, kReadoutRightX = 136, kReadoutStartX = 160;

  enum class Phase : uint8_t { Idle, Pressed, Scrubbing, Off };
  // How a touch on the bar ended (None: it hasn't). Let: a drag that
  // wasn't sideways, let go.
  enum class End : uint8_t { None, Seek, Stay, Off, Cancel, Let };
  struct Out {
    End end = End::None;
    bool tick = false;  // the scrub began, the detent entered, off or back
    bool tap = false;   // the Seek was a tap's (its tick waits for the player)
    uint32_t ms = 0;    // Seek: the target
  };

  // What the readout row shows (docs/SEEK-BAR.md section 4.2), field by
  // field: the page draws it again whenever this changes. Off: "Release to
  // cancel" (nothing else counts). Otherwise its side, and staying: the
  // live second and "no change"; or the finger's second and the change from
  // the live one.
  struct Readout {
    bool off = false;
    bool staying = false;
    bool left = false;
    uint32_t targetS = 0;  // the finger's second (0 while staying or off)
    uint32_t liveS = 0;    // where it plays (0 while off)
    bool operator==(const Readout& o) const {
      return off == o.off && staying == o.staying && left == o.left && targetS == o.targetS && liveS == o.liveS;
    }
    bool operator!=(const Readout& o) const { return !(*this == o); }
  };

  static bool seekable(uint32_t durationMs) { return durationMs >= kMinLengthMs; }
  // Where `ms` is on the line, 0..kLineW (0: no length): the fill's width.
  static int xOf(uint32_t ms, uint32_t durationMs);
  // The second at screen x (whole seconds, 0..seekLimitMs()).
  static uint32_t msAt(int x, uint8_t edges, uint32_t durationMs);
  // The readout's texts, and the band's times: a second as m:ss ("2:31";
  // a mix of 100 min or more, "100:00"); the change from where it plays
  // in whole seconds ("+1:21", "-0:45"). The minus is ASCII: none of the
  // fonts has U+2212, and the width measured would not be the one drawn
  // (TextFit folds it).
  static void timeText(uint32_t ms, char* buf, size_t size);
  static void changeText(uint32_t targetMs, uint32_t liveMs, char* buf, size_t size);

  // A Down in the bar's zone, on entry `key` playing at `liveMs` of
  // `durationMs` (frozen for the touch). False (inert, Idle): not seekable.
  bool down(const InputEvent& e, uint32_t key, uint32_t liveMs, uint32_t durationMs);
  // The touch's other events (Tap, DragStart, DragMove, DragEnd, Release,
  // Cancel), with where it plays now. Anything while Idle: nothing.
  Out onEvent(const InputEvent& e, uint32_t liveMs);
  // Every pass while active: the marker follows where it plays (staying
  // may change; no tick).
  void live(uint32_t liveMs);
  // The touch ends with nothing (the page left, the entry changed).
  void cancel() { phase_ = Phase::Idle; }

  Phase phase() const { return phase_; }
  bool active() const { return phase_ != Phase::Idle; }
  bool scrubbing() const { return phase_ == Phase::Scrubbing || phase_ == Phase::Off; }
  // The touch's (the last one's once it ended): its entry, its length, the
  // finger's second, where it plays, the grab.
  uint32_t key() const { return key_; }
  uint32_t lengthMs() const { return lengthMs_; }
  uint32_t targetMs() const { return targetMs_; }
  uint32_t liveMs() const { return liveMs_; }
  bool staying() const { return phase_ == Phase::Scrubbing && staying_; }
  bool knobGrab() const { return knobGrab_; }
  bool readoutLeft() const { return readoutLeft_; }
  // While scrubbing or off: what the readout shows.
  Readout readout() const;
  // Screen x: the knob (the target's; the marker's while pressed, staying
  // or off) and the marker (where it plays).
  int knobX() const;
  int markerX() const { return kLineX + xOf(liveMs_, lengthMs_); }
  // How long the target's second has been what it is (`nowMs` an event's
  // time): the lift guard's measure (docs/SEEK-BAR.md section 13).
  uint32_t heldMs(uint32_t nowMs) const { return nowMs - targetSinceMs_; }

private:
  // The finger at (x, edges): the target by the grab's mapping, its
  // place, staying.
  void aim(const InputEvent& e);
  // A knob grab's target: where it played at the DragStart, moved by the
  // finger's `dx` px since (whole seconds, 0..seekLimitMs()).
  uint32_t fromGrab(int dx) const;
  // The finger's place for line px `px` (a clamped reading: its end), on
  // the line up to the reach's px.
  int placeAt(int px, uint8_t edges) const;
  // The detent: the target on the second playing, or the finger's place
  // within kStayPx of where it plays.
  bool stays() const;
  void side();
  Out end(End how, uint32_t ms = 0);

  Phase phase_ = Phase::Idle;
  uint32_t key_ = 0;
  uint32_t lengthMs_ = 0;
  uint32_t targetMs_ = 0;
  uint32_t liveMs_ = 0;
  uint32_t targetSinceMs_ = 0;
  bool staying_ = false;
  bool knobGrab_ = false;
  bool readoutLeft_ = false;
  // Where the finger puts the knob, in line px, before the whole second
  // (the detent's measure: the target's x is its second's start).
  int placePx_ = 0;
  // A knob grab, at the DragStart: where it played, and the finger's x.
  uint32_t grabMs_ = 0;
  int grabX_ = 0;
};
