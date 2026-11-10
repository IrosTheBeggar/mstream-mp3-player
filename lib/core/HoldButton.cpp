// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HoldButton.h"

void HoldButton::arm(const dialog::Area& area) {
  area_ = area;
  armed_ = true;
  holding_ = false;
  lastMs_ = 0;
  gapMs_ = 0;
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

HoldButton::News HoldButton::end(News n) {
  holding_ = false;
  lastMs_ = since(seenMs_, downMs_);
  return n;
}

bool HoldButton::stalled(uint32_t ms) {
  const uint32_t gap = since(ms, seenMs_);
  if (gap <= kGapMs) return false;
  gapMs_ = gap;
  end(News::Stalled);
  return true;
}

void HoldButton::seen(uint32_t ms) {
  if (since(ms, seenMs_)) seenMs_ = ms;
}

HoldButton::News HoldButton::onEvent(const InputEvent& e) {
  using T = InputEvent::Type;
  if (!armed_) return News::None;
  switch (e.type) {
    case T::Down:
      // A new touch starts from nothing (a lift between, however short).
      holding_ = !e.fromStrip && on(e.x, e.y, 0);
      scripted_ = e.scripted;
      downMs_ = seenMs_ = e.ms;
      return holding_ ? News::Began : News::None;
    case T::DragStart:
    case T::DragMove:
      if (!holding_) return News::None;
      if (e.scripted) scripted_ = true;
      if (!on(e.x, e.y, kSlackPx)) return end(News::Slid);
      // On it still: a look at the finger, as update()'s, and held to the
      // same gap (a move is no way round it).
      if (stalled(e.ms)) return News::Stalled;
      seen(e.ms);
      return News::None;
    case T::Tap:
    case T::Release:
    case T::DragEnd:
      if (!holding_) return News::None;
      // A lift never presses it, however late it is seen: the hold was as
      // long as the finger was seen on it. (A Tap's point is where it
      // landed; a DragEnd's where it was last, which its DragMove judged.)
      return end(e.type == T::DragEnd && !on(e.x, e.y, kSlackPx) ? News::Slid : News::Short);
    case T::Cancel:
      return drop();
    default:
      return News::None;
  }
}

HoldButton::News HoldButton::update(uint32_t ms, const Finger& finger) {
  if (!holding_) return News::None;
  // The gap first, whatever this pass sees: once it is too long, the time
  // since the last look counts for nothing.
  if (stalled(ms)) return News::Stalled;
  // No finger this pass: nothing counted, nothing pressed (its lift's
  // event ends the hold, before this or after).
  if (!finger.down) return News::None;
  if (finger.scripted) scripted_ = true;
  // Off the button (its move's event says so too, before this or after).
  if (!on(finger.x, finger.y, kSlackPx)) return end(News::Slid);
  seen(ms);
  if (since(seenMs_, downMs_) < kHoldMs) return News::None;
  return end(scripted_ ? News::Scripted : News::Confirmed);
}

HoldButton::News HoldButton::drop() {
  if (!holding_) return News::None;
  holding_ = false;
  lastMs_ = 0;
  return News::Dropped;
}

uint32_t HoldButton::heldMs() const {
  return holding_ ? since(seenMs_, downMs_) : 0;
}

int HoldButton::fill(int width) const {
  if (width <= 0) return 0;
  const uint32_t held = heldMs();
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
    case News::Stalled: return "stalled";
    case News::Scripted: return "scripted";
  }
  return "?";
}
