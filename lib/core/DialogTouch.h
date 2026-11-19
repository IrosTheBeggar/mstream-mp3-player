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
// docs/HOST-STATUS.md "@format") the primary is a HoldButton: no event
// answers it, a tap, a lift however late or any other; only tick() does,
// in a pass that sees a hand's finger on it HoldButton::kHoldMs after it
// landed. A press on it that came to nothing (a tap, a lift, a slide off,
// a loop that lost sight of it, the console's scripted finger at its end)
// does nothing but set the hint's alert (hintAlert(): the dialog draws its
// hint amber). The other button is a button as ever.
// Portable, host-tested (test_hold_button: the real one under the Ui's
// routing, a finger on the real TouchRecognizer).
class DialogTouch {
public:
  // What the dialog draws again after an event, over the card as rendered
  // (a press never renders it all: Dialog::drawButton(), the whole dialog
  // was 44 ms, twice a tap): one button alone (a press highlight came or
  // went, the hold button emptied), and the hint's row when its alert came on.
  struct Result {
    int answer = -1;  // a button's index: the dialog's answer (-1 none)
    HoldButton::News news = HoldButton::News::None;  // what the event did to the hold
    int drawButton = -1;  // the button to draw again alone (-1 none)
    bool drawHint = false;
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

  // A glass event (the touch the Ui routes to the dialog). In the hold
  // mode its answer is never the hold button.
  Result onEvent(const InputEvent& e);
  // Every loop pass in the hold mode, with what the pass read of the panel
  // (HoldButton::update()): Confirmed once a hand's finger has been seen on
  // the hold button kHoldMs (the primary's answer, as a tap's); Stalled,
  // Slid or Scripted when the hold ends here with nothing done (the hint's
  // alert on: the dialog draws the hold button empty, and the hint's row
  // when hintAlert() changed).
  HoldButton::News tick(uint32_t ms, const HoldButton::Finger& finger);
  // The touch went elsewhere (the dark, a banner): Dropped if a hold was
  // under way (the hold button drawn empty).
  HoldButton::News dropHold() { return hold_.drop(); }

private:
  // A press on the hold button that came to nothing: empty again, the
  // hint's alert on.
  void fellShort(Result& r);
  HoldButton hold_;
  int n_ = 1;
  int primary_ = 0;
  int pressed_ = -1;
  bool hintAlert_ = false;
};
