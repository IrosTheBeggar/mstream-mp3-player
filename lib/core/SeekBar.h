// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "InputEvent.h"

// Now Playing's progress line as a seek bar (docs/SEEK-BAR.md): where the
// finger means, and what one touch on the bar comes to. No drawing and no
// clock of its own (the events carry their times): the page draws what
// this says, and calls the player (PlaybackController::seek()) once, when
// a touch ends in a seek.
//
// - A tap goes to the second under the finger (where it landed). A tap on
//   the knob (within kStayPx of where it plays) is no seek.
// - A sideways drag (|dx| >= |dy| at the DragStart) scrubs: the knob and
//   the readout follow the finger while the music plays on where it was,
//   and the lift seeks once, to the second the readout showed. A drag that
//   isn't sideways is let go: nothing, then or at the lift.
// - The grab. A Down within kGrabPx of the knob (a reading that wasn't
//   clamped) grabs it: from the DragStart the knob moves by the finger's
//   movement, with no jump (only differences count, so the panel's few px
//   of calibration error don't matter). Anywhere else the knob comes to
//   the finger.
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
//   live position, which moves while playing). A target within kStayPx of
//   it (in line px) is "staying": the knob snaps onto the marker, and a
//   lift seeks nothing (a paused track keeps its exact resume anchor). A
//   finger that moves into it ticks; a knob grab that starts in it, and
//   the marker coming onto a still knob, don't.
// - Off. Past the album band (y < kOffAboveY) or onto the button strip
//   (y >= kOffBelowY) the knob goes back to where it plays and a lift
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
  static constexpr int kOffAboveY = 130, kBackAboveY = 138, kOffBelowY = 240, kBackBelowY = 232;
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

  static bool seekable(uint32_t durationMs) { return durationMs >= kMinLengthMs; }
  // Where `ms` is on the line, 0..kLineW (0: no length): the fill's width.
  static int xOf(uint32_t ms, uint32_t durationMs);
  // The second at screen x (whole seconds, 0..seekLimitMs()).
  static uint32_t msAt(int x, uint8_t edges, uint32_t durationMs);

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
  // Screen x: the knob (the target's; the marker's while pressed, staying
  // or off) and the marker (where it plays).
  int knobX() const;
  int markerX() const { return kLineX + xOf(liveMs_, lengthMs_); }
  // How long the target's second has been what it is (`nowMs` an event's
  // time): the lift guard's measure (docs/SEEK-BAR.md section 13).
  uint32_t heldMs(uint32_t nowMs) const { return nowMs - targetSinceMs_; }

private:
  // The finger at (x, edges): the target by the grab's mapping.
  void aim(const InputEvent& e);
  bool stays(uint32_t targetMs) const;
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
  int downKnobX_ = 0;  // the knob's screen x at the Down
  int grabDx_ = 0;     // a knob grab: the knob's x at the Down less the finger's at the DragStart
};
