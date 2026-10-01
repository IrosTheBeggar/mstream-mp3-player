// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ButtonGesture.h"

ButtonGesture::Event ButtonGesture::update(uint32_t ms, bool pressed) {
  if (pressed && !down_) {
    down_ = true;
    held_ = false;
    downMs_ = ms;
    repeats_ = 0;
    return Event::Press;
  }
  if (!pressed && down_) {
    down_ = false;
    const bool wasHeld = held_;
    held_ = false;
    return wasHeld ? Event::HoldEnd : Event::Click;
  }
  if (!pressed) return Event::None;
  if (!held_) {
    if (ms - downMs_ < config_.holdMs) return Event::None;
    held_ = true;
    nextRepeatMs_ = ms + config_.repeatMs;
    return Event::Hold;
  }
  if (config_.repeatMs == 0 || static_cast<int32_t>(ms - nextRepeatMs_) < 0) return Event::None;
  ++repeats_;
  // On schedule if this pass is within a period of the deadline; after a
  // stall, a period from now (no burst of catch-up repeats).
  nextRepeatMs_ += config_.repeatMs;
  if (static_cast<int32_t>(ms - nextRepeatMs_) >= 0) nextRepeatMs_ = ms + config_.repeatMs;
  return Event::Repeat;
}

ButtonGesture::Event ButtonGesture::cancel() {
  const bool wasHeld = down_ && held_;
  down_ = false;
  held_ = false;
  return wasHeld ? Event::HoldEnd : Event::None;
}

const char* ButtonGesture::name(Event e) {
  switch (e) {
    case Event::Press: return "press";
    case Event::Click: return "click";
    case Event::Hold: return "hold";
    case Event::Repeat: return "repeat";
    case Event::HoldEnd: return "hold end";
    default: return "none";
  }
}
