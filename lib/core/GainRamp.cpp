// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "GainRamp.h"

#include <cstring>  // std::memset

namespace {
uint16_t clampQ15(uint16_t q) { return q > GainRamp::kUnity ? GainRamp::kUnity : q; }
uint32_t pack(uint16_t target, uint32_t seq) { return static_cast<uint32_t>(target) | (seq << 17); }
}  // namespace

GainRamp::GainRamp(uint16_t initialQ15)
    : mailbox_(pack(clampQ15(initialQ15), 0)),
      seen_(mailbox_.load(std::memory_order_relaxed)),
      cur_(static_cast<int32_t>(clampQ15(initialQ15)) << 15),
      target_(cur_),
      current_(clampQ15(initialQ15)) {}

void GainRamp::request(uint16_t targetQ15, bool snap) {
  const uint16_t t = clampQ15(targetQ15);
  if (snap) {
    lift_.store(kNoSnap, std::memory_order_relaxed);  // a new link: an old lift is void
    snap_.store(t, std::memory_order_relaxed);        // published by the release below
  }
  const uint32_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  mailbox_.store(pack(t, seq), std::memory_order_release);
}

void GainRamp::lift(uint16_t targetQ15) {
  const uint16_t t = clampQ15(targetQ15);
  lift_.store(t, std::memory_order_relaxed);  // published by the release below
  const uint32_t seq = seq_.fetch_add(1, std::memory_order_relaxed) + 1;
  mailbox_.store(pack(t, seq), std::memory_order_release);
}

void GainRamp::reset(uint16_t gainQ15) {
  const uint16_t g = clampQ15(gainQ15);
  const uint32_t word = pack(g, seq_.fetch_add(1, std::memory_order_relaxed) + 1);
  mailbox_.store(word, std::memory_order_relaxed);
  snap_.store(kNoSnap, std::memory_order_relaxed);
  lift_.store(kNoSnap, std::memory_order_relaxed);
  restart_.store(false, std::memory_order_relaxed);
  seen_ = word;
  cur_ = target_ = static_cast<int32_t>(g) << 15;
  ceiling_ = 0;
  current_.store(g, std::memory_order_relaxed);
}

// One frame's move toward target_.
void GainRamp::step() {
  // The quick fade back after restart() ends once it reaches the level heard
  // before (or the target, if that is lower), or when the target drops.
  if (ceiling_ != 0 && !(cur_ < target_ && cur_ < ceiling_)) ceiling_ = 0;
  if (cur_ > target_) {
    constexpr int32_t kDown = kUnityQ30 / static_cast<int32_t>(kDownFrames);
    cur_ = cur_ - target_ > kDown ? cur_ - kDown : target_;
  } else if (cur_ < target_) {
    if (ceiling_ != 0) {
      constexpr int32_t kFade = kUnityQ30 / static_cast<int32_t>(kFadeFrames);
      const int32_t limit = target_ < ceiling_ ? target_ : ceiling_;
      cur_ = limit - cur_ > kFade ? cur_ + kFade : limit;
    } else {
      int32_t up = ((cur_ >> 10) * kUpNum) >> 10;  // cur_ <= 2^30: no overflow
      if (up < kMinUpStep) up = kMinUpStep;
      cur_ = target_ - cur_ > up ? cur_ + up : target_;
    }
  }
}

void GainRamp::process(int16_t* lr, uint32_t frames) {
  const uint32_t word = mailbox_.load(std::memory_order_acquire);
  if (word != seen_) {
    seen_ = word;
    target_ = static_cast<int32_t>(word & 0x1FFFFu) << 15;
  }
  // A snap only ever lowers the gain, and also caps a pending quick fade.
  if (snap_.load(std::memory_order_relaxed) != kNoSnap) {
    const int32_t level = snap_.exchange(kNoSnap, std::memory_order_acquire);
    if (level != kNoSnap) {
      const int32_t q30 = level << 15;
      if (cur_ > q30) cur_ = q30;
      if (ceiling_ > q30) ceiling_ = q30;
    }
  }
  int32_t lift = kNoSnap;
  if (lift_.load(std::memory_order_relaxed) != kNoSnap) lift = lift_.exchange(kNoSnap, std::memory_order_acquire);
  // After the snap, so a new link's first stream fades in to the snapped (or
  // lifted) level. A second restart while that fade still runs is the same
  // silence ending: dropping back to 0 mid-fade would be a step of its own.
  if (restart_.load(std::memory_order_relaxed) && restart_.exchange(false, std::memory_order_acquire) &&
      ceiling_ == 0) {
    if (lift != kNoSnap) {
      int32_t q30 = lift << 15;
      if (q30 > target_) q30 = target_;
      if (q30 > cur_) cur_ = q30;
    }
    ceiling_ = cur_;
    cur_ = 0;
  }

  if (cur_ == target_ && ceiling_ == 0) {  // steady: one gain for the whole block
    const int32_t g = (cur_ + (1 << 14)) >> 15;
    if (g == kUnity) {
      // bit-exact: leave the samples alone
    } else if (g == 0) {
      std::memset(lr, 0, frames * 2 * sizeof(int16_t));
    } else {
      for (uint32_t k = 0, n = 2 * frames; k < n; ++k) lr[k] = scale(lr[k], g);
    }
    current_.store(static_cast<uint16_t>(g), std::memory_order_relaxed);
    return;
  }

  int32_t g = 0;
  for (uint32_t i = 0; i < frames; ++i) {
    step();
    g = (cur_ + (1 << 14)) >> 15;
    lr[2 * i] = scale(lr[2 * i], g);
    lr[2 * i + 1] = scale(lr[2 * i + 1], g);
  }
  if (frames > 0) current_.store(static_cast<uint16_t>(g), std::memory_order_relaxed);
}
