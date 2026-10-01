// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "InputEvent.h"

// One finger on the glass, sampled every loop pass, turned into the
// InputEvents the screens use, as the tab bar spec (§5) defines them:
//
//   Down       at once, for the press highlight
//   Tap        lifted within slopPx of where it landed, before holdMs
//   LongPress  holdMs down without moving slopPx (fires while still down,
//              once); the lift that follows is a Release, never a tap
//   DragStart  moved more than slopPx (Chebyshev) before a LongPress; then
//   DragMove   each time the point changes, and on the lift
//   DragEnd    with the release velocity over the last velocityWindowMs,
//              capped at maxFlingPxPerS, and a
//   Fling      after it when that speed is at least flickPxPerS
//
// The fling cap (2,000 px/s) keeps a flicked list on the hardware scroll's
// cheap path: above ~2,500 px/s every frame at 25-30 fps moves more than
// the 84-line step, and each becomes a full redraw (docs/UI-SPIKE.md).
//
// A touch that lands on the button strip (raw y >= stripY: the buttons,
// which StripButtons makes from the same points) is the buttons': it makes
// no glass events, unless StripButtons finds it is a swipe up from the
// strip and hands it over (fromStrip()). From that sample on it is a drag
// that starts there: DragStart (dx, dy 0; no Down before it), DragMove,
// then DragEnd and Fling as any drag's, every event flagged
// InputEvent::fromStrip; never a Tap or a LongPress. One that lands on the
// glass and slides onto the strip stays a glass touch (its y goes on past
// the glass), and never presses a button.
//
// Portable: fed with timestamps and corrected points, no clock of its own.
class TouchRecognizer {
public:
  struct Config {
    int slopPx = 12;
    uint32_t holdMs = 500;
    float flickPxPerS = 400.0f;
    float maxFlingPxPerS = 2000.0f;
    uint32_t velocityWindowMs = 60;
    int stripY = 240;  // touches landing here or lower (raw y) belong to the buttons
  };

  // One sample: the finger (if any) at corrected (x, y), what the panel
  // read, and the Edge bits.
  struct Sample {
    bool pressed = false;
    int16_t x = 0, y = 0;
    int16_t rawX = 0, rawY = 0;
    uint8_t edges = 0;
  };

  static constexpr int kMaxEvents = 3;  // per update: e.g. DragMove, DragEnd, Fling

  TouchRecognizer() = default;
  explicit TouchRecognizer(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // Writes up to kMaxEvents events to `out`; returns how many.
  int update(uint32_t ms, const Sample& s, InputEvent* out);
  // Drops the touch in progress (a screen changed under the finger):
  // returns a Cancel event if there was one (else None), and ignores the
  // finger until it lifts.
  InputEvent cancel(uint32_t ms);
  // The touch in progress has nothing to hold (the A-Z rail: a finger that
  // rests on it to read the letter, then slides, must still scrub): no
  // LongPress for it; it stays a press until it moves past the slop (a
  // drag) or lifts (a tap). The next touch has its hold again.
  void noHold() { holdOff_ = true; }
  // The touch in progress, which landed on the button strip, is a swipe up
  // from it (StripButtons' `scroll`): the next update, with the same
  // sample, makes it a drag from that point. Nothing for any other touch
  // (one on the glass, or one cancel() dropped).
  void fromStrip() {
    if (state_ == State::Strip) handOver_ = true;
  }

  bool active() const { return state_ != State::Idle && state_ != State::Strip && state_ != State::Ignored; }
  bool dragging() const { return state_ == State::Dragging; }

private:
  // Strip: landed on the button strip (the buttons' until fromStrip()).
  enum class State : uint8_t { Idle, Pressed, LongPressed, Dragging, Strip, Ignored };
  struct Point {
    uint32_t ms;
    int16_t x, y;
  };
  static constexpr int kHistory = 16;

  InputEvent make(InputEvent::Type t, uint32_t ms, const Sample& s) const;
  void remember(uint32_t ms, int x, int y);
  void releaseVelocity(float* vx, float* vy) const;

  Config config_;
  State state_ = State::Idle;
  bool holdOff_ = false;  // noHold(): this touch never becomes a LongPress
  bool handOver_ = false;  // fromStrip(): the next update starts the drag
  bool strip_ = false;     // this touch is a swipe from the strip (its events say so)
  uint32_t downMs_ = 0;
  int16_t downX_ = 0, downY_ = 0, downRawX_ = 0, downRawY_ = 0;
  uint8_t downEdges_ = 0;
  int16_t lastX_ = 0, lastY_ = 0;
  Sample last_;
  Point hist_[kHistory] = {};
  int head_ = 0;
  int count_ = 0;
};
