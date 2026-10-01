// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "Declicker.h"

#include <cstring>  // std::memset

namespace {
int16_t saturate(int32_t v) {
  return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}

// (a * ga + b * gb) / 32768, rounded. Can't overflow: each product is at most
// 2^30 in size, so the sum stays within int32. >> on a negative int is an
// arithmetic shift on GCC (and defined as one from C++20).
int16_t mix(int32_t a, int32_t ga, int32_t b, int32_t gb) {
  return saturate((a * ga + b * gb + (1 << 14)) >> 15);
}
}  // namespace

Declicker::Declicker(uint32_t rampFrames) : step_(stepFor(rampFrames)) {}

void Declicker::startTail() {
  tailL_ = lastL_;
  tailR_ = lastR_;
  tailGain_ = (lastL_ != 0 || lastR_ != 0) ? kUnity : 0;
  gain_ = 0;
  live_ = false;
}

void Declicker::cut() {
  if (live_) startTail();  // not live: already decaying, nothing new to hold
}

void Declicker::reset() {
  live_ = false;
  gain_ = tailGain_ = 0;
  tailL_ = tailR_ = lastL_ = lastR_ = 0;
}

uint32_t Declicker::framesWanted(uint32_t count) const {
  if (open_) return count;
  if (!live_) return 0;
  const auto toSilence = static_cast<uint32_t>((gain_ + step_ - 1) / step_);
  return toSilence < count ? toSilence : count;
}

uint32_t Declicker::decayFrames() const {
  // Live: the first missing frame will hold the last output at full gain.
  const int32_t from = live_ ? ((lastL_ != 0 || lastR_ != 0) ? kUnity : 0) : tailGain_;
  return static_cast<uint32_t>((from + step_ - 1) / step_);
}

void Declicker::process(int16_t* frames, uint32_t valid, uint32_t total) {
  if (valid > total) valid = total;
  if (total == 0) return;

  // At rest: nothing real, nothing fading. Output = silence.
  if (valid == 0 && !live_ && tailGain_ == 0) {
    std::memset(frames, 0, total * 2 * sizeof(int16_t));
    lastL_ = lastR_ = 0;
    return;
  }
  // Steady state: full level, nothing fading, no gap. Output = input.
  if (open_ && live_ && gain_ == kUnity && tailGain_ == 0 && valid == total) {
    lastL_ = frames[2 * (total - 1)];
    lastR_ = frames[2 * (total - 1) + 1];
    return;
  }

  const int32_t target = open_ ? kUnity : 0;
  for (uint32_t i = 0; i < total; ++i) {
    int16_t* f = frames + 2 * i;
    tailGain_ = tailGain_ > step_ ? tailGain_ - step_ : 0;
    if (i < valid) {
      live_ = true;  // after a break gain_ is 0 here: this fades in
      if (gain_ < target) {
        gain_ = gain_ + step_ < target ? gain_ + step_ : target;
      } else if (gain_ > target) {
        gain_ = gain_ - step_ > target ? gain_ - step_ : target;
      }
      f[0] = mix(f[0], gain_, tailL_, tailGain_);
      f[1] = mix(f[1], gain_, tailR_, tailGain_);
    } else {
      if (live_) {
        startTail();  // the audio broke off: hold the last frame and let it decay
        tailGain_ = tailGain_ > step_ ? tailGain_ - step_ : 0;
      }
      f[0] = mix(0, 0, tailL_, tailGain_);
      f[1] = mix(0, 0, tailR_, tailGain_);
    }
    lastL_ = f[0];
    lastR_ = f[1];
  }
}
