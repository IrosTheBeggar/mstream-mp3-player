// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SeekBar.h"

#include <algorithm>
#include <cstdlib>

#include "TrackSeek.h"

int SeekBar::xOf(uint32_t ms, uint32_t durationMs) {
  if (durationMs == 0) return 0;
  return static_cast<int>(static_cast<uint64_t>(std::min(ms, durationMs)) * kLineW / durationMs);
}

uint32_t SeekBar::msAt(int x, uint8_t edges, uint32_t durationMs) {
  const uint32_t reach = trackseek::seekLimitMs(durationMs);
  // A reading clamped at an edge: the finger may have been further out, and
  // on a panel that never reads the ends, it is the only way there.
  if (edges & InputEvent::kEdgeLeft) return 0;
  if (edges & InputEvent::kEdgeRight) return reach;
  if (x <= kLineX) return 0;
  if (x >= kLineX + kLineW) return reach;
  const uint64_t ms = static_cast<uint64_t>(x - kLineX) * durationMs / kLineW;
  return std::min(static_cast<uint32_t>(ms / 1000 * 1000), reach);
}

bool SeekBar::down(const InputEvent& e, uint32_t key, uint32_t liveMs, uint32_t durationMs) {
  phase_ = Phase::Idle;
  if (!seekable(durationMs)) return false;
  phase_ = Phase::Pressed;
  key_ = key;
  lengthMs_ = durationMs;
  liveMs_ = liveMs;
  targetMs_ = liveMs;
  targetSinceMs_ = e.ms;
  staying_ = false;
  downKnobX_ = markerX();
  // (A clamped reading's x is no place: never a grab.)
  knobGrab_ = e.edges == 0 && std::abs(e.x - downKnobX_) <= kGrabPx;
  grabDx_ = 0;
  readoutLeft_ = downKnobX_ >= kReadoutStartX;
  return true;
}

bool SeekBar::stays(uint32_t targetMs) const {
  return std::abs(xOf(targetMs, lengthMs_) - xOf(liveMs_, lengthMs_)) <= kStayPx;
}

void SeekBar::aim(const InputEvent& e) {
  const uint32_t t = msAt(e.x + grabDx_, e.edges, lengthMs_);
  if (t != targetMs_) {
    targetMs_ = t;
    targetSinceMs_ = e.ms;
  }
  staying_ = stays(t);
}

void SeekBar::side() {
  const int k = knobX();
  if (k > kReadoutLeftX) {
    readoutLeft_ = true;
  } else if (k < kReadoutRightX) {
    readoutLeft_ = false;
  }
}

int SeekBar::knobX() const {
  if (phase_ == Phase::Scrubbing && !staying_) return kLineX + xOf(targetMs_, lengthMs_);
  return markerX();  // pressed, staying, off: where it plays
}

SeekBar::Out SeekBar::end(End how, uint32_t ms) {
  phase_ = Phase::Idle;
  Out o;
  o.end = how;
  o.ms = ms;
  return o;
}

void SeekBar::live(uint32_t liveMs) {
  liveMs_ = liveMs;
  if (phase_ != Phase::Scrubbing) return;
  staying_ = stays(targetMs_);  // the marker onto a still knob: staying, no tick
  side();
}

SeekBar::Out SeekBar::onEvent(const InputEvent& e, uint32_t liveMs) {
  using T = InputEvent::Type;
  Out o;
  if (phase_ == Phase::Idle) return o;
  liveMs_ = liveMs;
  switch (e.type) {
    case T::Tap: {
      if (phase_ != Phase::Pressed) return o;
      // Where it landed (the recogniser's Tap carries the Down's point).
      const uint32_t t = msAt(e.x, e.edges, lengthMs_);
      targetMs_ = t;
      targetSinceMs_ = e.ms;
      if (stays(t)) return end(End::Stay);  // a tap on the knob
      o = end(End::Seek, t);
      o.tap = true;
      return o;
    }
    case T::DragStart: {
      if (phase_ != Phase::Pressed) return o;
      if (std::abs(e.dx) < std::abs(e.dy)) return end(End::Let);  // not sideways: not the bar's
      // From the knob: it stays where it was at the Down and moves by the
      // finger's movement from here. Anywhere else: it comes to the finger.
      grabDx_ = knobGrab_ ? downKnobX_ - e.x : 0;
      phase_ = e.y < kOffAboveY || e.y >= kOffBelowY ? Phase::Off : Phase::Scrubbing;  // (a steep, fast start)
      aim(e);  // (staying here is no tick of its own: the scrub's is enough)
      targetSinceMs_ = e.ms;
      readoutLeft_ = knobX() >= kReadoutStartX;
      o.tick = true;
      return o;
    }
    case T::DragMove:
    case T::DragEnd: {
      if (!scrubbing()) return o;
      if (phase_ == Phase::Scrubbing && (e.y < kOffAboveY || e.y >= kOffBelowY)) {
        phase_ = Phase::Off;  // the knob back where it plays
        o.tick = true;
      } else if (phase_ == Phase::Off && e.y >= kBackAboveY && e.y < kBackBelowY) {
        phase_ = Phase::Scrubbing;  // by the grab's mapping again
        aim(e);
        o.tick = true;
      } else if (phase_ == Phase::Scrubbing) {
        const bool was = staying_;
        aim(e);
        if (staying_ && !was) o.tick = true;  // into the detent
      }
      side();
      if (e.type == T::DragMove) return o;
      // The lift: nothing to feel (a seek's audio jump confirms it), and at
      // most one seek, to the second the readout showed.
      if (phase_ == Phase::Off) return end(End::Off);
      if (staying_) return end(End::Stay);
      return end(End::Seek, targetMs_);
    }
    case T::Release:
    case T::Cancel:
      return end(End::Cancel);
    default:
      return o;
  }
}
