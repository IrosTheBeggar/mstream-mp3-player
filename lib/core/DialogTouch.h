// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DialogLayout.h"
#include "HoldButton.h"
#include "InputEvent.h"

// A dialog's buttons as the glass answers them: ui/Overlays' Dialog, its
// drawing aside (it draws what this says changed). One or two buttons in
// the card's last rows (dialog::buttonAt()): a finger that lands on one
// presses it (its highlight), a move past the slop, a Cancel or a long
// press's lift lets it go, and a Tap is the pressed one's answer (where
// the touch landed). In the hold mode (the format's second question,
// docs/HOST-STATUS.md "@format") the primary is a HoldButton: no tap
// answers it, only a finger kept on it HoldButton::kHoldMs (a lift at that
// end here, or tick() at it); a press on it cut short (a tap, a lift, a
// slide off) does nothing but set the hint's alert (hintAlert(): the
// dialog draws its hint amber). The other button is a button as ever.
// Portable, host-tested (test_hold_button: the real one under the Ui's
// routing, a finger on the real TouchRecognizer).
class DialogTouch {
public:
  // What the dialog draws again after an event: nothing, all of it (a
  // press highlight came or went, the hint's alert), or the hold button
  // alone, empty (a hold dropped).
  enum class Redraw : uint8_t { None, All, HoldButton };
  struct Result {
    int answer = -1;  // a button's index: the dialog's answer (-1 none)
    HoldButton::News news = HoldButton::News::None;  // what the event did to the hold
    Redraw redraw = Redraw::None;
  };

  // `n` buttons (1 or 2), `primary` the primary's index (-1 or out of
  // range: the last); `hold`: the hold mode, the primary the hold button.
  // A press, a hold and the alert from before go.
  void open(int n, int primary, bool hold);
  void close();
  int n() const { return n_; }
  int primary() const { return primary_; }
  int pressed() const { return pressed_; }  // the button highlighted, -1 none
  bool holdMode() const { return hold_.armed(); }
  bool holding() const { return hold_.holding(); }
  bool hintAlert() const { return hintAlert_; }
  const HoldButton& hold() const { return hold_; }

  // A glass event (the touch the Ui routes to the dialog).
  Result onEvent(const InputEvent& e);
  // Every loop pass in the hold mode: Confirmed once the finger has been
  // on the hold button kHoldMs (the primary's answer, as a tap's).
  HoldButton::News tick(uint32_t ms) { return hold_.update(ms); }
  // The touch went elsewhere (the dark, a banner): Dropped if a hold was
  // under way (the hold button drawn empty).
  HoldButton::News dropHold() { return hold_.drop(); }

private:
  HoldButton hold_;
  int n_ = 1;
  int primary_ = 0;
  int pressed_ = -1;
  bool hintAlert_ = false;
};
