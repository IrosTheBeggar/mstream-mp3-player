// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SeekBar.h"

#include <algorithm>
#include <cstdio>
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

void SeekBar::timeText(uint32_t ms, char* buf, size_t size) {
  snprintf(buf, size, "%lu:%02lu", static_cast<unsigned long>(ms / 60000), static_cast<unsigned long>(ms / 1000 % 60));
}

void SeekBar::changeText(uint32_t targetMs, uint32_t liveMs, char* buf, size_t size) {
  const int32_t d = static_cast<int32_t>(targetMs / 1000) - static_cast<int32_t>(liveMs / 1000);
  const uint32_t a = static_cast<uint32_t>(d < 0 ? -d : d);
  snprintf(buf, size, "%c%lu:%02lu", d < 0 ? '-' : '+', static_cast<unsigned long>(a / 60),
           static_cast<unsigned long>(a % 60));
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
  const int knob = markerX();
  placePx_ = knob - kLineX;
  // (A clamped reading's x is no place: never a grab.)
  knobGrab_ = e.edges == 0 && std::abs(e.x - knob) <= kGrabPx;
  grabMs_ = 0;
  grabX_ = 0;
  readoutLeft_ = knob >= kReadoutStartX;
  return true;
}

SeekBar::Readout SeekBar::readout() const {
  Readout r;
  if (phase_ == Phase::Off) {
    r.off = true;
    return r;
  }
  r.left = readoutLeft_;
  r.staying = staying();
  r.liveS = liveMs_ / 1000;
  if (!r.staying) r.targetS = targetMs_ / 1000;
  return r;
}

bool SeekBar::stays() const {
  // The second playing: a seek there would only start it again.
  if (targetMs_ / 1000 == liveMs_ / 1000) return true;
  // The finger's place, not the target's x: that is its second's start, up
  // to a second's px behind the finger (5.6 on a 52 s track).
  return std::abs(placePx_ - xOf(liveMs_, lengthMs_)) <= kStayPx;
}

int SeekBar::placeAt(int px, uint8_t edges) const {
  const int reach = xOf(trackseek::seekLimitMs(lengthMs_), lengthMs_);
  if (edges & InputEvent::kEdgeLeft) return 0;
  if (edges & InputEvent::kEdgeRight) return reach;
  return std::max(0, std::min(px, reach));
}

uint32_t SeekBar::fromGrab(int dx) const {
  const int64_t ms = static_cast<int64_t>(grabMs_) + static_cast<int64_t>(dx) * lengthMs_ / kLineW;
  const uint32_t reach = trackseek::seekLimitMs(lengthMs_);
  if (ms <= 0) return 0;
  if (ms >= reach) return reach;
  return static_cast<uint32_t>(ms) / 1000 * 1000;
}

void SeekBar::aim(const InputEvent& e) {
  // (A clamped reading reaches its end, grab or not.)
  const bool grab = knobGrab_ && e.edges == 0;
  const int dx = e.x - grabX_;
  const uint32_t t = grab ? fromGrab(dx) : msAt(e.x, e.edges, lengthMs_);
  if (t != targetMs_) {
    targetMs_ = t;
    targetSinceMs_ = e.ms;
  }
  placePx_ = placeAt(grab ? xOf(grabMs_, lengthMs_) + dx : e.x - kLineX, e.edges);
  staying_ = stays();
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
  staying_ = stays();  // the marker onto a still knob: staying, no tick
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
      placePx_ = placeAt(e.x - kLineX, e.edges);
      if (stays()) return end(End::Stay);  // a tap on the knob
      o = end(End::Seek, t);
      o.tap = true;
      return o;
    }
    case T::DragStart: {
      if (phase_ != Phase::Pressed) return o;
      if (std::abs(e.dx) < std::abs(e.dy)) return end(End::Let);  // not sideways: not the bar's
      // From the knob: it stays where it is now (where it plays: not where
      // it was at the Down, which a finger that rested on it while the
      // music played on would see it jump back to) and moves by the
      // finger's movement from here. The anchor is the time, not the
      // knob's pixel, so the grab starts on the second playing. Anywhere
      // else: it comes to the finger.
      grabMs_ = liveMs_;
      grabX_ = e.x;
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
        // Where it plays now first: the marker onto a still knob is no
        // tick (live()'s rule), here as at a pass; the finger's move is.
        const bool was = stays();
        aim(e);
        if (staying_ && !was) o.tick = true;  // into the detent
      }
      side();
      if (e.type == T::DragMove) return o;
      // The lift: nothing more to feel (a seek's audio jump confirms it),
      // and at most one seek, to the second the readout showed; none while
      // staying, just measured against where it plays at the lift (so
      // never to the second playing).
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
