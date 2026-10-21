// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DialogTouch.h"

void DialogTouch::open(int n, int primary, bool hold) {
  n_ = n < 1 ? 1 : n > 2 ? 2 : n;
  primary_ = primary >= 0 && primary < n_ ? primary : n_ - 1;
  pressed_ = -1;
  hintAlert_ = false;
  if (hold) {
    hold_.arm(dialog::hitArea(primary_, n_));
  } else {
    hold_.disarm();
  }
}

void DialogTouch::close() {
  pressed_ = -1;
  hold_.disarm();
}

DialogTouch::Result DialogTouch::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  using News = HoldButton::News;
  Result r;
  if (hold_.armed()) {
    // The hold button's touch is the hold's: no press highlight (its fill
    // is one), and no tap answers it, only a hold kept to its end.
    r.news = hold_.onEvent(e);
    switch (r.news) {
      case News::Began:
        return r;  // (the fill: the dialog's tick() draws it as it grows)
      case News::Short:
      case News::Slid:
        // Nothing happens: empty again, and the hint says how, amber.
        hintAlert_ = true;
        r.redraw = Redraw::All;
        return r;
      case News::Dropped:
        r.redraw = Redraw::HoldButton;
        return r;
      case News::Confirmed:
        r.answer = primary_;  // (lifted at its end, before tick() saw it)
        return r;
      default:
        break;
    }
    if (hold_.holding()) return r;  // (a move on it)
  }
  if (e.type == T::Down) {
    pressed_ = dialog::buttonAt(e.x, e.y, n_);
    if (hold_.armed() && pressed_ == primary_) pressed_ = -1;  // only a hold answers it
    if (pressed_ >= 0) r.redraw = Redraw::All;
    return r;
  }
  if (e.type == T::DragStart || e.type == T::Cancel || e.type == T::Release) {
    if (pressed_ >= 0) {
      pressed_ = -1;
      r.redraw = Redraw::All;
    }
    return r;
  }
  if (e.type != T::Tap) return r;
  r.answer = pressed_;
  pressed_ = -1;
  return r;
}
