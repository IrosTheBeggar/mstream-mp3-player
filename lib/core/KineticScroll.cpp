#include "KineticScroll.h"

#include <cmath>

float KineticScroll::clamp(float px) const {
  if (px < 0) return 0;
  if (px > maxOffset_) return maxOffset_;
  return px;
}

void KineticScroll::setExtent(float contentPx, float viewportPx) {
  maxOffset_ = contentPx > viewportPx ? contentPx - viewportPx : 0;
  offset_ = clamp(offset_);
  snapTarget_ = clamp(snapTarget_);
}

float KineticScroll::rowTarget(float from, float v) const {
  const float row = config_.rowPx;
  float t;
  if (v > 0) {
    t = std::ceil(from / row - 0.001f) * row;
  } else if (v < 0) {
    t = std::floor(from / row + 0.001f) * row;
  } else {
    t = std::round(from / row) * row;
  }
  return clamp(t);
}

void KineticScroll::startSnap(float target) {
  velocity_ = 0;
  snapTarget_ = clamp(target);
  phase_ = std::fabs(snapTarget_ - offset_) < 0.5f ? Phase::Idle : Phase::Snapping;
  if (phase_ == Phase::Idle) offset_ = snapTarget_;
}

void KineticScroll::press(uint32_t ms, int y) {
  phase_ = Phase::Dragging;
  velocity_ = 0;
  pressY_ = y;
  pressOffset_ = offset_;
  lastMs_ = ms;
  head_ = 0;
  count_ = 0;
  drag(ms, y);
}

void KineticScroll::drag(uint32_t ms, int y) {
  if (phase_ != Phase::Dragging) return;
  offset_ = clamp(pressOffset_ + static_cast<float>(pressY_ - y));
  histMs_[head_] = ms;
  histOffset_[head_] = offset_;
  head_ = (head_ + 1) % kHistory;
  if (count_ < kHistory) ++count_;
  lastMs_ = ms;
}

void KineticScroll::release(uint32_t ms) {
  if (phase_ != Phase::Dragging) return;
  // Release velocity over the last window of the drag.
  float v = 0;
  if (count_ >= 2) {
    const int last = (head_ + kHistory - 1) % kHistory;
    int ref = -1;
    for (int k = 2; k <= count_; ++k) {
      ref = (head_ + kHistory - k) % kHistory;
      if (histMs_[last] - histMs_[ref] >= config_.velocityWindowMs) break;
    }
    // A finger that stopped before lifting: no fling.
    const bool stale = ms - histMs_[last] > config_.velocityWindowMs;
    if (ref >= 0 && histMs_[last] > histMs_[ref] && !stale) {
      v = (histOffset_[last] - histOffset_[ref]) * 1000.0f / static_cast<float>(histMs_[last] - histMs_[ref]);
    }
  }
  lastMs_ = ms;
  if (std::fabs(v) >= config_.stopPxPerS) {
    fling(ms, v);
  } else {
    startSnap(rowTarget(offset_, 0));
  }
}

void KineticScroll::fling(uint32_t ms, float pxPerS) {
  if (pxPerS > config_.maxPxPerS) pxPerS = config_.maxPxPerS;
  if (pxPerS < -config_.maxPxPerS) pxPerS = -config_.maxPxPerS;
  velocity_ = pxPerS;
  lastMs_ = ms;
  phase_ = Phase::Flinging;
}

void KineticScroll::jumpTo(float px) {
  offset_ = clamp(px);
  velocity_ = 0;
  phase_ = Phase::Idle;
}

bool KineticScroll::update(uint32_t ms) {
  const float before = offset_;
  const float dt = static_cast<float>(ms - lastMs_);
  lastMs_ = ms;
  if (dt <= 0) return false;
  if (phase_ == Phase::Flinging) {
    // Exact decay of v' = -k v over dt: the offset moves by the integral.
    const float keep = std::pow(config_.frictionPerFrame, dt / config_.frameMs);
    const float k = -std::log(config_.frictionPerFrame) / config_.frameMs;  // per ms
    const float moved = velocity_ / 1000.0f * (1.0f - keep) / k;
    velocity_ *= keep;
    offset_ += moved;
    if (offset_ <= 0 || offset_ >= maxOffset_) {
      offset_ = clamp(offset_);
      velocity_ = 0;
      phase_ = Phase::Idle;  // the ends are row boundaries
      startSnap(rowTarget(offset_, 0));
    } else if (std::fabs(velocity_) < config_.stopPxPerS) {
      startSnap(rowTarget(offset_, velocity_));
    }
  } else if (phase_ == Phase::Snapping) {
    const float a = 1.0f - std::exp(-dt / config_.snapTauMs);
    offset_ += (snapTarget_ - offset_) * a;
    if (std::fabs(snapTarget_ - offset_) < 0.5f) {
      offset_ = snapTarget_;
      phase_ = Phase::Idle;
    }
  }
  return offset_ != before;
}

const char* KineticScroll::name(Phase p) {
  switch (p) {
    case Phase::Dragging: return "drag";
    case Phase::Flinging: return "fling";
    case Phase::Snapping: return "snap";
    default: return "idle";
  }
}
