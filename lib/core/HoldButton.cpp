// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HoldButton.h"

void HoldButton::arm(const dialog::Area& area) {
  area_ = area;
  armed_ = true;
  holding_ = false;
  lastMs_ = 0;
}

void HoldButton::disarm() {
  armed_ = false;
  holding_ = false;
}

bool HoldButton::on(int x, int y, int slack) const {
  if (y < area_.y0 - slack || y >= area_.y1 + slack) return false;
  if (x < area_.x0 - slack) return false;
  // A button that reaches the screen's right edge has every reading there,
  // a clamped one too, wherever the finger was.
  return area_.toEdge || x < area_.x1 + slack;
}

namespace {
// The time since `from`, never negative: an earlier `ms` (an update whose
// time was taken before the event's) is no time at all, never a wrap.
uint32_t since(uint32_t ms, uint32_t from) {
  const int32_t d = static_cast<int32_t>(ms - from);
  return d > 0 ? static_cast<uint32_t>(d) : 0;
}
}  // namespace

HoldButton::News HoldButton::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!armed_) return News::None;
  switch (e.type) {
    case T::Down:
      // A new touch starts from nothing (a lift between, however short).
      holding_ = !e.fromStrip && on(e.x, e.y, 0);
      downMs_ = e.ms;
      return holding_ ? News::Began : News::None;
    case T::DragStart:
    case T::DragMove:
      if (!holding_) return News::None;
      if (on(e.x, e.y, kSlackPx)) return News::None;
      holding_ = false;
      lastMs_ = since(e.ms, downMs_);
      return News::Slid;
    case T::Tap:
    case T::Release:
    case T::DragEnd: {
      if (!holding_) return News::None;
      holding_ = false;
      lastMs_ = since(e.ms, downMs_);
      // (A Tap's point is where it landed; a DragEnd's where it was last,
      // which its DragMove already judged.)
      if (e.type == T::DragEnd && !on(e.x, e.y, kSlackPx)) return News::Slid;
      // Lifted at or after the end that update() hadn't seen yet: the
      // finger was on it that long.
      return lastMs_ >= kHoldMs ? News::Confirmed : News::Short;
    }
    case T::Cancel:
      return drop();
    default:
      return News::None;
  }
}

HoldButton::News HoldButton::update(uint32_t ms) {
  if (!holding_ || since(ms, downMs_) < kHoldMs) return News::None;
  holding_ = false;
  lastMs_ = since(ms, downMs_);
  return News::Confirmed;
}

HoldButton::News HoldButton::drop() {
  if (!holding_) return News::None;
  holding_ = false;
  lastMs_ = 0;
  return News::Dropped;
}

uint32_t HoldButton::heldMs(uint32_t ms) const {
  return holding_ ? since(ms, downMs_) : 0;
}

int HoldButton::fill(uint32_t ms, int width) const {
  if (width <= 0) return 0;
  const uint32_t held = heldMs(ms);
  if (held >= kHoldMs) return width;
  return static_cast<int>(static_cast<uint64_t>(held) * static_cast<uint32_t>(width) / kHoldMs);
}

const char* HoldButton::name(News n) {
  switch (n) {
    case News::None: return "none";
    case News::Began: return "began";
    case News::Confirmed: return "confirmed";
    case News::Short: return "short";
    case News::Slid: return "slid off";
    case News::Dropped: return "dropped";
  }
  return "?";
}
