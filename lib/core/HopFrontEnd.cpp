// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HopFrontEnd.h"

#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979;
constexpr float kDcHz = 5.0f;

// RBJ cookbook low-pass, worked out in double.
void lowpass(float fs, float hz, float q, HopFrontEnd::Biquad* f) {
  const double w0 = 2.0 * kPi * hz / fs;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double c = std::cos(w0);
  const double a0 = 1.0 + alpha;
  f->b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
  f->b1 = static_cast<float>((1.0 - c) / a0);
  f->b2 = f->b0;
  f->a1 = static_cast<float>(-2.0 * c / a0);
  f->a2 = static_cast<float>((1.0 - alpha) / a0);
}
}  // namespace

void HopFrontEnd::begin(uint32_t sampleRate, uint32_t hop, uint32_t decimation, float lowpassHz) {
  decimation_ = decimation;
  hopLen_ = hop / decimation;
  scale_ = 1.0f / (32768.0f * static_cast<float>(decimation));
  // (The code's approximation of a 5 Hz pole: 1 - 2 pi f / fs.)
  dcPole_ = 1.0f - 2.0f * static_cast<float>(kPi) * kDcHz * decimation / sampleRate;
  const float fs = static_cast<float>(sampleRate) / decimation;
  // Fourth-order Butterworth as two biquads.
  lowpass(fs, lowpassHz, 0.5412f, &lp1_);
  lowpass(fs, lowpassHz, 1.3066f, &lp2_);
  reset();
}

void HopFrontEnd::reset() {
  decimSum_ = 0;
  decimCount_ = 0;
  hopFill_ = 0;
  hopEnergy_ = midEnergy_ = 0.0f;
  dcX_ = dcY_ = 0.0f;
  lp1_.z1 = lp1_.z2 = lp2_.z1 = lp2_.z2 = 0.0f;
}
