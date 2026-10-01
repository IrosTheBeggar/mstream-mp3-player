// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "FadeStage.h"

#include <cstring>  // std::memset

#include "GainRamp.h"  // GainRamp::scale

void FadeStage::restore() {
  target_.store(kUnity, std::memory_order_release);
  level_.store(kUnityQ30, std::memory_order_release);
}

void FadeStage::process(int16_t* lr, uint32_t frames, bool advance) {
  int32_t start = level_.load(std::memory_order_acquire);
  const int32_t target = static_cast<int32_t>(target_.load(std::memory_order_acquire)) << 15;
  if (start > kUnityQ30) start = kUnityQ30;  // (never: kept so a torn word can't add level)
  if (start == target || !advance) {
    // Steady: one gain for the whole block.
    const int32_t g = (start + (1 << 14)) >> 15;
    if (g >= kUnity) return;  // bit-exact
    if (g == 0) {
      std::memset(lr, 0, frames * 2 * sizeof(int16_t));
      return;
    }
    for (uint32_t k = 0, n = 2 * frames; k < n; ++k) lr[k] = GainRamp::scale(lr[k], g);
    return;
  }
  int32_t cur = start;
  for (uint32_t i = 0; i < frames; ++i) {
    if (cur > target) {
      constexpr int32_t kDown = kUnityQ30 / static_cast<int32_t>(kDownFrames);
      cur = cur - target > kDown ? cur - kDown : target;
    } else if (cur < target) {
      int32_t up = ((cur >> 10) * kUpNum) >> 10;  // cur <= 2^30: no overflow
      if (up < kMinUpStep) up = kMinUpStep;
      cur = target - cur > up ? cur + up : target;
    }
    const int32_t g = (cur + (1 << 14)) >> 15;
    lr[2 * i] = GainRamp::scale(lr[2 * i], g);
    lr[2 * i + 1] = GainRamp::scale(lr[2 * i + 1], g);
  }
  // Published only if nobody changed it meanwhile (a restore(), or the
  // other output's block during a handover): theirs is newer.
  level_.compare_exchange_strong(start, cur, std::memory_order_acq_rel, std::memory_order_relaxed);
}
