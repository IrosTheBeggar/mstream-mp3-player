// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TapReader.h"

#include <cmath>

namespace {
constexpr float kSag = 0.003f;       // of the rate: how fast the clock gives way to a slower writer
constexpr float kStallSeconds = 0.06f;  // behind the clock by more than this: start over
}  // namespace

void TapReader::attach(const AudioTap* tap, float sampleRate) {
  tap_ = tap;
  rate_ = sampleRate;
  clockValid_ = false;
  lost_ = 0;
  if (tap_) next_ = tap_->count();
}

void TapReader::updateClock(uint32_t count, uint32_t us) {
  if (!clockValid_) {
    clockValid_ = true;
    refCount_ = count;
    refUs_ = us;
    refFrac_ = 0.0f;
    return;
  }
  if (us == refUs_ && count == refCount_) return;  // nothing written since
  const float dt = static_cast<float>(static_cast<int32_t>(us - refUs_)) * 1e-6f;
  const float predicted = refFrac_ + dt * rate_;  // relative to refCount_
  const auto measured = static_cast<float>(static_cast<int32_t>(count - refCount_));
  float clock;
  if (predicted - measured > kStallSeconds * rate_ || dt < 0.0f) {
    clock = measured;  // the writer stalled (or time ran backwards): start over
  } else {
    clock = predicted - kSag * rate_ * dt;
    if (clock < measured) clock = measured;  // a write ahead of the clock moves it up
  }
  refCount_ = count;
  refUs_ = us;
  refFrac_ = clock - measured;
}

TapReader::Audible TapReader::audibleAt(uint32_t nowUs, uint32_t latencyUs) const {
  Audible a;
  if (!tap_ || !clockValid_) return a;
  const float dt = static_cast<float>(static_cast<int32_t>(nowUs - refUs_)) * 1e-6f;
  float rel = refFrac_ + (dt - static_cast<float>(latencyUs) * 1e-6f) * rate_;
  // Heard well past the last frame written: nothing is playing (the speaker
  // writes nothing while paused, stopped or starved). Invalid, so the figure
  // fades to its idle sway rather than holding a pose mid-hop.
  if (rel > kStallSeconds * rate_) return a;
  if (rel > -1.0f) rel = -1.0f;  // a burst late: hold at the last frame written
  const float whole = std::floor(rel);
  const uint32_t frame = refCount_ + static_cast<uint32_t>(static_cast<int32_t>(whole));
  AudioTap::Segment seg;
  if (!tap_->segmentAt(frame, refCount_, &seg) || seg.trackStart == AudioTap::kNoTrack) return a;
  a.valid = true;
  a.trackFrame = seg.trackStart + (frame - seg.tapStart);
  a.frac = rel - whole;
  a.epoch = seg.epoch;
  return a;
}
