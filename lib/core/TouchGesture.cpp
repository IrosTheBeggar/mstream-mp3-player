// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TouchGesture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

void TouchGesture::down(uint32_t ms, int x, int y) {
  active_ = true;
  downMs_ = ms;
  downX_ = x;
  downY_ = y;
  maxMove_ = 0;
  head_ = 0;
  count_ = 0;
  move(ms, x, y);
}

void TouchGesture::move(uint32_t ms, int x, int y) {
  if (!active_) return;
  hist_[head_] = {ms, static_cast<int16_t>(x), static_cast<int16_t>(y)};
  head_ = (head_ + 1) % kHistory;
  ++count_;
  const int d = std::max(std::abs(x - downX_), std::abs(y - downY_));
  if (d > maxMove_) maxMove_ = d;
}

TouchGesture::Result TouchGesture::up(uint32_t ms) {
  Result r;
  if (!active_) return r;
  active_ = false;
  const int n = count_ < static_cast<uint32_t>(kHistory) ? static_cast<int>(count_) : kHistory;
  const Sample& last = hist_[(head_ + kHistory - 1) % kHistory];
  r.downMs = downMs_;
  r.durationMs = ms - downMs_;
  r.downX = downX_;
  r.downY = downY_;
  r.upX = last.x;
  r.upY = last.y;
  r.maxMovePx = maxMove_;
  r.samples = count_;

  // Release velocity: from the newest sample at least a window older than
  // the last one (else the oldest kept) to the last.
  const Sample* ref = nullptr;
  for (int k = 2; k <= n; ++k) {
    const Sample& s = hist_[(head_ + kHistory - k) % kHistory];
    ref = &s;
    if (last.ms - s.ms >= config_.velocityWindowMs) break;
  }
  if (ref && last.ms > ref->ms) {
    const float dt = static_cast<float>(last.ms - ref->ms) / 1000.0f;
    r.vx = static_cast<float>(last.x - ref->x) / dt;
    r.vy = static_cast<float>(last.y - ref->y) / dt;
    r.speed = std::sqrt(r.vx * r.vx + r.vy * r.vy);
  }

  if (maxMove_ > config_.slopPx) {
    r.kind = r.speed >= config_.flickPxPerS ? Kind::Flick : Kind::Drag;
  } else {
    r.kind = r.durationMs >= config_.holdMs ? Kind::Hold : Kind::Tap;
  }
  return r;
}

const char* TouchGesture::name(Kind k) {
  switch (k) {
    case Kind::Tap: return "tap";
    case Kind::Hold: return "hold";
    case Kind::Drag: return "drag";
    case Kind::Flick: return "flick";
    default: return "none";
  }
}
