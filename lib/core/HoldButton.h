// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DialogLayout.h"
#include "InputEvent.h"

// A button that acts only when held (ui/Overlays' Dialog in its hold mode:
// the format's second question, docs/HOST-STATUS.md "@format"). A finger
// that lands on it starts its fill; seen on it for kHoldMs, never off it in
// between, the button is pressed: Confirmed. Lifted sooner, however quick
// or slow (a tap): Short, it empties and nothing happens (the dialog shows
// how: its hint). Slid off it: Slid, the same. The touch taken away (a
// screen or the dark took it, the dialog closed): Dropped. Nothing adds
// up across touches: each press starts from nothing, so no double tap, no
// run of taps and no flicker of a noisy panel (a lift read for a moment)
// ever reaches the end; only one finger kept on it does.
//
// "Seen on it" is the rule that keeps a clock from pressing it. The time
// that counts is from the landing to the last look that saw the finger
// there (update(), every loop pass, with what the pass read of the panel;
// a move that stays on it), never to "now":
//   - only update() presses it, in a pass that itself sees the finger on
//     the button with kHoldMs behind it. A lift never does, whenever it
//     is seen: a tap whose lift is read two seconds late (the loop stalled
//     between: a card write, the display's bus) held it as long as it was
//     seen, a moment;
//   - two looks more than kGapMs apart end the hold, Stalled, before
//     anything else is asked: what the finger did between them isn't
//     known (lifted and landed again, as one touch to the recogniser), so
//     the time isn't counted, and nothing happens. A hold seen every
//     kGapMs or sooner goes on;
//   - a pass that sees no finger counts nothing and presses nothing (its
//     lift's event ends the hold; one that never comes, the gap does),
//     and one that sees it off the button ends the hold, Slid, as its
//     move's event would.
// So it comes out the same whether a pass's events reach it before its
// update() or after.
//
// And only a hand presses it: a hold any part of which was the console's
// scripted finger (ui/Input::simulate(): its events and its passes say so)
// ends Scripted where it would have been Confirmed, and nothing happens.
// Cut short it is Short or Slid as any other.
//
// It takes the glass's events as the Ui routes them, with the recogniser's
// hold turned off for the touch (Input::noHold() at Began: the touch then
// stays a press while the finger rests, a move past the slop is a drag,
// and the lift a Tap or a DragEnd; TouchRecognizer): Down lands; DragStart
// and DragMove move; Tap, Release and DragEnd lift; Cancel drops. "Off" is
// outside the button's hit area by more than kSlackPx (dialog::hitArea():
// a resting finger's drift and the panel's noise stay on it; one that
// moves to Cancel is off). Portable: no clock of its own, the times are
// the events' and update()'s (ms that wrap, as millis()).
class HoldButton {
public:
  static constexpr uint32_t kHoldMs = 2000;
  // Two looks at the finger further apart than this say nothing of the
  // time between them (a loop pass is 5-40 ms; a card write on the loop a
  // few hundred at its worst).
  static constexpr uint32_t kGapMs = 500;
  static constexpr int kSlackPx = 10;
  enum class News : uint8_t {
    None,
    Began,      // a finger landed on it: the fill starts
    Confirmed,  // seen on it kHoldMs, by hand: the button is pressed (once)
    Short,      // lifted before that: nothing happens (the hint)
    Slid,       // slid off it before that: nothing happens (the hint)
    Dropped,    // the touch was taken away: nothing happens
    Stalled,    // not seen for more than kGapMs: nothing happens (the hint)
    Scripted,   // seen on it kHoldMs, the scripted finger's: nothing happens
  };
  // What a pass read of the panel (ui/Input::finger()): a finger on it
  // (its first touch point, or the scripted one), where (screen px,
  // corrected, as the events'), and whether it is the console's scripted
  // finger.
  struct Finger {
    bool down = false;
    bool scripted = false;
    int16_t x = 0, y = 0;
  };

  // The button's hit area (screen px); arming ends a hold under way.
  void arm(const dialog::Area& area);
  void disarm();
  bool armed() const { return armed_; }
  // A glass event: what it did to the hold (None: nothing). Never
  // Confirmed, nor Scripted: only update() says those.
  News onEvent(const InputEvent& e);
  // Every pass, with what it read of the panel: Stalled when the last look
  // that saw the finger on it is more than kGapMs old; else, the finger
  // down, Slid when it is off the button, or the hold seen to `ms`, and
  // Confirmed (Scripted) once that is kHoldMs from its landing.
  News update(uint32_t ms, const Finger& finger);
  // The touch went elsewhere: Dropped if a hold was under way.
  News drop();
  bool holding() const { return holding_; }
  // How long the finger has been seen on it (0: no hold), and the fill:
  // how much of `width` px that covers, 0 to width. The same time update()
  // presses it by.
  uint32_t heldMs() const;
  int fill(int width) const;
  // How long the last hold was seen on the button, at its end (Short,
  // Slid, Stalled, Confirmed, Scripted; 0 for Dropped), and for Stalled
  // how late the look that ended it came: for the log.
  uint32_t lastHeldMs() const { return lastMs_; }
  uint32_t lastGapMs() const { return gapMs_; }
  // Whether (x, y) is on the button, with `slack` px around its area (a
  // button whose area reaches the screen's edge: every x from its left).
  bool on(int x, int y, int slack) const;

  static const char* name(News n);

private:
  // The hold over, as `n`: what it was seen for kept for the log.
  News end(News n);
  // A look at `ms` comes more than kGapMs after the last that saw the
  // finger on it: the hold is over, Stalled (true).
  bool stalled(uint32_t ms);
  // The finger seen on it at `ms`.
  void seen(uint32_t ms);

  dialog::Area area_;
  bool armed_ = false;
  bool holding_ = false;
  bool scripted_ = false;  // the scripted finger's, in part at least
  uint32_t downMs_ = 0;
  uint32_t seenMs_ = 0;  // the last look that saw the finger on it
  uint32_t lastMs_ = 0;
  uint32_t gapMs_ = 0;
};
