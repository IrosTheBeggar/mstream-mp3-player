// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StripButtons.h"

#include <algorithm>
#include <cstdlib>

namespace {

int chebyshev(int x0, int y0, int x1, int y1) { return std::max(std::abs(x1 - x0), std::abs(y1 - y0)); }

}  // namespace

int StripButtons::column(int x) {
  if (x < 0) x = 0;
  if (x > 319) x = 319;
  return (x * 614) >> 16;  // M5Unified's (65536 * 3 / 320)
}

// The touch lifted. One that wasn't a press (glass, a swipe from the
// strip, or a strip touch already ignored) opens the bounce windows for the
// next.
void StripButtons::lift(uint32_t ms) {
  if (state_ == State::Glass || state_ == State::Ignored || state_ == State::Scrolling) {
    lost_ = true;
    lostMs_ = ms;
    lostX_ = lastX_;
    lostY_ = lastY_;
    lostMoved_ = moved_;
  }
  state_ = State::Idle;
  button_ = -1;
}

void StripButtons::down(uint32_t ms, int x, int y, Result& r) {
  downX_ = lastX_ = static_cast<int16_t>(x);
  downY_ = lastY_ = static_cast<int16_t>(y);
  downMs_ = ms;
  reported_ = false;
  moved_ = false;
  held_ = false;
  if (y < config_.stripY) {
    state_ = State::Glass;  // its lift opens the windows afresh
    lost_ = false;
    return;
  }
  if (lost_) {
    const uint32_t since = ms - lostMs_;
    const int from = chebyshev(lostX_, lostY_, x, y);
    const bool swipe = lostMoved_ && since < config_.swipeBounceMs && from <= config_.swipeBouncePx;
    if (since < config_.bounceMs || swipe) {
      state_ = State::Ignored;
      moved_ = lostMoved_;  // the same finger: if it's lost again, still a swipe's
      r.ignored = Why::Bounce;
      r.button = static_cast<int8_t>(column(x));
      r.sinceLiftMs = since;
      r.fromLiftPx = static_cast<int16_t>(from);
      r.afterSwipe = lostMoved_;
      return;
    }
  }
  lost_ = false;  // a real press: whatever came before is done with
  state_ = State::Pressing;
  button_ = static_cast<int8_t>(column(x));
  r.pressed = button_;
}

bool StripButtons::upward(int x, int y) const {
  const int dx = x - downX_, dy = y - downY_;
  if (std::max(std::abs(dx), std::abs(dy)) <= config_.slopPx) return false;
  return y < config_.stripY || (dy < 0 && -dy >= std::abs(dx));
}

StripButtons::Result StripButtons::update(uint32_t ms, bool pressed, int x, int y, bool newTouch) {
  Result r;
  if (!pressed) {
    lift(ms);
    return r;
  }

  if (newTouch && state_ != State::Idle) {
    // Another finger took over: the old touch lifted, this one goes down. A
    // press that ends here is a release, not a cancel (its ButtonGesture
    // sees the button up; a new press of the same button just carries on).
    lift(ms);
    down(ms, x, y, r);
    return r;
  }
  if (state_ == State::Idle) {  // it went down
    down(ms, x, y, r);
    return r;
  }

  lastX_ = static_cast<int16_t>(x);
  lastY_ = static_cast<int16_t>(y);
  const int moved = chebyshev(downX_, downY_, x, y);
  if (moved > config_.slopPx) moved_ = true;
  switch (state_) {
    case State::Glass:
      if (y >= config_.stripY && !reported_) {
        reported_ = true;
        r.ignored = Why::Glass;
        r.button = static_cast<int8_t>(column(x));
      }
      break;
    case State::Pressing:
      if (moved > config_.slopPx) {
        r.cancelled = true;
        r.button = button_;
        r.moved = static_cast<int16_t>(moved);
        button_ = -1;
        if (!held_ && upward(x, y)) {
          state_ = State::Scrolling;
          r.scroll = true;
        } else {
          state_ = State::Ignored;
          r.ignored = Why::Moved;
        }
      } else {
        r.pressed = button_;
        // ButtonGesture holds in this update if it is due (the same test).
        if (ms - downMs_ >= config_.holdMs) held_ = true;
      }
      break;
    case State::Ignored:
      // Pressing nothing, but a swipe up from here still scrolls.
      if (!held_ && upward(x, y)) {
        state_ = State::Scrolling;
        r.scroll = true;
        r.button = static_cast<int8_t>(column(downX_));
        r.moved = static_cast<int16_t>(moved);
      }
      break;
    default:
      break;
  }
  return r;
}

const char* StripButtons::name(Why w) {
  switch (w) {
    case Why::Glass: return "a glass touch reached the strip";
    case Why::Moved: return "moved off the button";
    case Why::Bounce: return "right after a touch that wasn't a press lifted (a bounce)";
    default: return "none";
  }
}
