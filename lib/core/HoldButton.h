// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DialogLayout.h"
#include "InputEvent.h"

// A button that acts only when held (ui/Overlays' Dialog in its hold mode:
// the format's second question, docs/HOST-STATUS.md "@format"). A finger
// that lands on it starts its fill; kept down kHoldMs, never off it in
// between, the button is pressed: Confirmed. Lifted sooner, however quick
// or slow (a tap): Short, it empties and nothing happens (the dialog shows
// how: its hint). Slid off it: Slid, the same. The touch taken away (a
// screen or the dark took it, the dialog closed): Dropped. Nothing adds
// up across touches: each press starts from nothing, so no double tap, no
// run of taps and no flicker of a noisy panel (a lift read for a moment)
// ever reaches the end; only one finger kept on it does.
//
// It takes the glass's events as the Ui routes them, with the recogniser's
// hold turned off for the touch (Input::noHold() at Began: the touch then
// stays a press while the finger rests, a move past the slop is a drag,
// and the lift a Tap or a DragEnd; TouchRecognizer): Down lands; DragStart
// and DragMove move; Tap, Release and DragEnd lift; Cancel drops. "Off" is
// outside the button's hit area by more than kSlackPx (dialog::hitArea():
// a resting finger's drift and the panel's noise stay on it; one that
// moves to Cancel is off). Portable: no clock of its own, the times are
// the events' and update()'s.
class HoldButton {
public:
  static constexpr uint32_t kHoldMs = 2000;
  static constexpr int kSlackPx = 10;
  enum class News : uint8_t {
    None,
    Began,      // a finger landed on it: the fill starts
    Confirmed,  // held kHoldMs: the button is pressed (once)
    Short,      // lifted before kHoldMs: nothing happens (the hint)
    Slid,       // slid off it before kHoldMs: nothing happens (the hint)
    Dropped,    // the touch was taken away: nothing happens
  };

  // The button's hit area (screen px); arming ends a hold under way.
  void arm(const dialog::Area& area);
  void disarm();
  bool armed() const { return armed_; }
  // A glass event: what it did to the hold (None: nothing).
  News onEvent(const InputEvent& e);
  // Every pass: Confirmed once the finger has been on it kHoldMs.
  News update(uint32_t ms);
  // The touch went elsewhere: Dropped if a hold was under way.
  News drop();
  bool holding() const { return holding_; }
  // How long the finger has been on it (0: no hold), and the fill: how
  // much of `width` px it covers at `ms`, 0 to width.
  uint32_t heldMs(uint32_t ms) const;
  int fill(uint32_t ms, int width) const;
  // How long the last hold lasted, as its end saw it (Short, Slid,
  // Confirmed; 0 for Dropped): for the log.
  uint32_t lastHeldMs() const { return lastMs_; }
  // Whether (x, y) is on the button, with `slack` px around its area (a
  // button whose area reaches the screen's edge: every x from its left).
  bool on(int x, int y, int slack) const;

  static const char* name(News n);

private:
  dialog::Area area_;
  bool armed_ = false;
  bool holding_ = false;
  uint32_t downMs_ = 0;
  uint32_t lastMs_ = 0;
};
