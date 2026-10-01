// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// Vertical list scrolling with inertia, as the tab bar spec (§6.3) has it: a
// drag moves the list 1:1, a release faster than `stopPxPerS` flings on with
// friction (0.92 per 15 fps frame, whatever the real frame rate), and the
// list always comes to rest on a row boundary (a short eased snap). The
// offset is in pixels, 0 = the first row at the top, growing as the list
// moves up. Flings are capped at maxPxPerS, 2,000 px/s: faster, nearly every
// frame of the hardware scroll (ui/ListScroller) moves more than its 84-line
// step and becomes a full redraw (docs/UI-SPIKE.md). Portable: driven by
// timestamps, no clock of its own.
class KineticScroll {
public:
  enum class Phase : uint8_t { Idle, Dragging, Flinging, Snapping };

  struct Config {
    float rowPx = 42.0f;
    float frictionPerFrame = 0.92f;  // velocity kept per reference frame
    float frameMs = 1000.0f / 15.0f; // the reference frame
    float stopPxPerS = 60.0f;        // a fling slower than this ends (and snaps)
    float maxPxPerS = 2000.0f;       // the fling cap
    float snapTauMs = 45.0f;         // the snap's time constant
    uint32_t velocityWindowMs = 60;  // release velocity over this much of the drag
  };

  KineticScroll() = default;
  explicit KineticScroll(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // The list's height and the viewport's. The offset is clamped to them.
  void setExtent(float contentPx, float viewportPx);
  float maxOffset() const { return maxOffset_; }

  void press(uint32_t ms, int y);
  void drag(uint32_t ms, int y);
  void release(uint32_t ms);
  // The same with the release velocity measured by the caller (the input
  // layer's DragEnd, already capped): `pxPerS` in offset units (positive:
  // the list moves up, so a finger moving up at v px/s is -vy).
  void release(uint32_t ms, float pxPerS);
  // Starts a fling at `pxPerS` (positive: the offset grows, the list moves up).
  void fling(uint32_t ms, float pxPerS);
  // Jumps (A-Z rail, jump grid), no animation; stops any motion.
  void jumpTo(float px);

  // Advances the fling or the snap to `ms`. True if the offset changed.
  bool update(uint32_t ms);

  float offset() const { return offset_; }
  float velocity() const { return velocity_; }
  Phase phase() const { return phase_; }
  bool moving() const { return phase_ != Phase::Idle; }
  // Where a snap from here would end, rounding in the direction of `v`.
  float rowTarget(float from, float v) const;

  static const char* name(Phase p);

private:
  float clamp(float px) const;
  void startSnap(float target);

  Config config_;
  float offset_ = 0;
  float velocity_ = 0;   // px/s, offset units
  float maxOffset_ = 0;
  float snapTarget_ = 0;
  Phase phase_ = Phase::Idle;
  uint32_t lastMs_ = 0;
  // The drag.
  int pressY_ = 0;
  float pressOffset_ = 0;
  static constexpr int kHistory = 8;
  uint32_t histMs_[kHistory] = {};
  float histOffset_[kHistory] = {};
  int head_ = 0;
  int count_ = 0;
};
