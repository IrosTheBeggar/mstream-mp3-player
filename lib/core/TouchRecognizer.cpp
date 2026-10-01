// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TouchRecognizer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

const char* InputEvent::name(Type t) {
  switch (t) {
    case Type::Down: return "down";
    case Type::Tap: return "tap";
    case Type::LongPress: return "long press";
    case Type::Release: return "release";
    case Type::DragStart: return "drag start";
    case Type::DragMove: return "drag";
    case Type::DragEnd: return "drag end";
    case Type::Fling: return "fling";
    case Type::Cancel: return "cancel";
    case Type::Click: return "click";
    case Type::Hold: return "hold";
    case Type::Repeat: return "repeat";
    case Type::HoldEnd: return "hold end";
    default: return "none";
  }
}

InputEvent TouchRecognizer::make(InputEvent::Type t, uint32_t ms, const Sample& s) const {
  InputEvent e;
  e.type = t;
  e.ms = ms;
  e.x = s.x;
  e.y = s.y;
  e.rawX = s.rawX;
  e.rawY = s.rawY;
  e.edges = s.edges;
  e.fromStrip = strip_;
  return e;
}

void TouchRecognizer::remember(uint32_t ms, int x, int y) {
  hist_[head_] = {ms, static_cast<int16_t>(x), static_cast<int16_t>(y)};
  head_ = (head_ + 1) % kHistory;
  if (count_ < kHistory) ++count_;
}

// From the newest sample at least a window older than the last one (else
// the oldest kept) to the last, capped at maxFlingPxPerS (the direction
// kept).
void TouchRecognizer::releaseVelocity(float* vx, float* vy) const {
  *vx = *vy = 0;
  if (count_ < 2) return;
  const Point& last = hist_[(head_ + kHistory - 1) % kHistory];
  const Point* ref = nullptr;
  for (int k = 2; k <= count_; ++k) {
    ref = &hist_[(head_ + kHistory - k) % kHistory];
    if (last.ms - ref->ms >= config_.velocityWindowMs) break;
  }
  if (!ref || last.ms <= ref->ms) return;
  const float dt = static_cast<float>(last.ms - ref->ms) / 1000.0f;
  float x = static_cast<float>(last.x - ref->x) / dt;
  float y = static_cast<float>(last.y - ref->y) / dt;
  const float speed = std::sqrt(x * x + y * y);
  if (speed > config_.maxFlingPxPerS && speed > 0) {
    const float k = config_.maxFlingPxPerS / speed;
    x *= k;
    y *= k;
  }
  *vx = x;
  *vy = y;
}

int TouchRecognizer::update(uint32_t ms, const Sample& s, InputEvent* out) {
  using T = InputEvent::Type;
  if (!s.pressed) {
    const State was = state_;
    state_ = State::Idle;
    handOver_ = false;
    switch (was) {
      case State::Pressed: {
        // Where it landed: the point that was aimed.
        InputEvent e = make(T::Tap, ms, last_);
        e.x = downX_;
        e.y = downY_;
        e.rawX = downRawX_;
        e.rawY = downRawY_;
        e.edges = downEdges_;
        e.dx = static_cast<int16_t>(last_.x - downX_);
        e.dy = static_cast<int16_t>(last_.y - downY_);
        out[0] = e;
        return 1;
      }
      case State::LongPressed:
        out[0] = make(T::Release, ms, last_);
        return 1;
      case State::Dragging: {
        InputEvent e = make(T::DragEnd, ms, last_);
        e.dx = static_cast<int16_t>(last_.x - downX_);
        e.dy = static_cast<int16_t>(last_.y - downY_);
        releaseVelocity(&e.vx, &e.vy);
        out[0] = e;
        if (std::sqrt(e.vx * e.vx + e.vy * e.vy) < config_.flickPxPerS) return 1;
        out[1] = e;
        out[1].type = T::Fling;
        return 2;
      }
      default:
        return 0;
    }
  }

  switch (state_) {
    case State::Ignored:
      return 0;
    case State::Strip: {
      if (!handOver_) return 0;
      // A swipe up from the strip: a drag from here (where it was handed
      // over is where it "landed": the list moves with the finger from now
      // on, no jump by the way it came).
      handOver_ = false;
      state_ = State::Dragging;
      strip_ = true;
      downMs_ = ms;
      downX_ = lastX_ = s.x;
      downY_ = lastY_ = s.y;
      downRawX_ = s.rawX;
      downRawY_ = s.rawY;
      downEdges_ = s.edges;
      last_ = s;
      head_ = 0;
      count_ = 0;
      remember(ms, s.x, s.y);
      out[0] = make(T::DragStart, ms, s);
      return 1;
    }
    case State::Idle:
      strip_ = false;
      handOver_ = false;
      if (s.rawY >= config_.stripY) {  // the button strip (as M5Unified reads it): the buttons', not the glass
        state_ = State::Strip;
        return 0;
      }
      state_ = State::Pressed;
      holdOff_ = false;
      downMs_ = ms;
      downX_ = lastX_ = s.x;
      downY_ = lastY_ = s.y;
      downRawX_ = s.rawX;
      downRawY_ = s.rawY;
      downEdges_ = s.edges;
      last_ = s;
      head_ = 0;
      count_ = 0;
      remember(ms, s.x, s.y);
      out[0] = make(T::Down, ms, s);
      return 1;
    default:
      break;
  }

  last_ = s;
  remember(ms, s.x, s.y);
  if (state_ == State::Pressed) {
    const int moved = std::max(std::abs(s.x - downX_), std::abs(s.y - downY_));
    if (moved > config_.slopPx) {
      state_ = State::Dragging;
      InputEvent e = make(T::DragStart, ms, s);
      e.dx = static_cast<int16_t>(s.x - downX_);
      e.dy = static_cast<int16_t>(s.y - downY_);
      lastX_ = s.x;
      lastY_ = s.y;
      out[0] = e;
      return 1;
    }
    if (!holdOff_ && ms - downMs_ >= config_.holdMs) {
      state_ = State::LongPressed;
      InputEvent e = make(T::LongPress, ms, s);
      e.x = downX_;
      e.y = downY_;
      e.rawX = downRawX_;
      e.rawY = downRawY_;
      e.edges = downEdges_;
      out[0] = e;
      return 1;
    }
    return 0;
  }
  if (state_ == State::Dragging && (s.x != lastX_ || s.y != lastY_)) {
    InputEvent e = make(T::DragMove, ms, s);
    e.dx = static_cast<int16_t>(s.x - lastX_);
    e.dy = static_cast<int16_t>(s.y - lastY_);
    lastX_ = s.x;
    lastY_ = s.y;
    out[0] = e;
    return 1;
  }
  return 0;
}

InputEvent TouchRecognizer::cancel(uint32_t ms) {
  InputEvent e;
  if (state_ == State::Idle || state_ == State::Ignored) return e;
  if (state_ == State::Strip) {  // not the glass's (yet): nothing to end, and no swipe from it now
    state_ = State::Ignored;
    handOver_ = false;
    return e;
  }
  e = make(InputEvent::Type::Cancel, ms, last_);
  state_ = State::Ignored;  // until the finger lifts
  return e;
}
