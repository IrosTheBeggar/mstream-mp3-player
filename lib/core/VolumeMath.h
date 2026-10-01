// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cmath>
#include <cstdint>

// Volume arithmetic for the Bluetooth output, kept free of hardware so it can
// be tested on the host.
//
// Gains are Q15: 32768 is 1.0 (0 dB). The UI volume is a percentage; AVRCP
// absolute volume is 0-127 (0x7F = 100 %, AVRCP 1.4 SetAbsoluteVolume).
namespace vol {

constexpr uint16_t kUnityQ15 = 32768;

// Always taken off the PCM before SBC: hot masters decoded at 0 dBFS clip in
// SBC's filter bank, and a clipped encode sounds worse than 2 dB less level.
constexpr float kHeadroomDb = -2.0f;
constexpr uint16_t kHeadroomQ15 = 26029;  // round(32768 * 10^(-2/20))

// Software volume (headphones without absolute volume): dB-linear over this
// range, 0 % = silence. 30 % is -28 dB (-30 dB with the headroom), about the
// -27.5 dB ESP32-A2DP's own curve gave at the old default of 30 %.
constexpr float kSoftwareRangeDb = 40.0f;

constexpr uint8_t kAbsMax = 127;

inline uint8_t percentToAbs(uint8_t percent) {
  if (percent > 100) percent = 100;
  return static_cast<uint8_t>((percent * 127u + 50u) / 100u);  // round(percent * 1.27)
}

// Inverse of percentToAbs() for every percentage (the round trip is exact);
// 128 steps onto 101 means some absolute values share a percentage.
inline uint8_t absToPercent(uint8_t absolute) {
  if (absolute > kAbsMax) absolute = kAbsMax;
  // round(abs * 100 / 127); abs * 200 is even and 127 odd, so there are no ties.
  return static_cast<uint8_t>((absolute * 100u + 63u) / 127u);
}

inline uint16_t dbToQ15(float db) {
  const float g = std::round(32768.0f * std::pow(10.0f, db / 20.0f));
  if (g <= 0.0f) return 0;
  if (g >= 32768.0f) return kUnityQ15;
  return static_cast<uint16_t>(g);
}

inline float q15ToDb(uint16_t q) {
  return q == 0 ? -INFINITY : 20.0f * std::log10(static_cast<float>(q) / 32768.0f);
}

// Our gain for a UI volume when the headphones don't apply it themselves
// (headroom not included).
inline uint16_t softwareVolumeQ15(uint8_t percent) {
  if (percent == 0) return 0;
  if (percent >= 100) return kUnityQ15;
  return dbToQ15(-kSoftwareRangeDb * static_cast<float>(100 - percent) / 100.0f);
}

// a * b in Q15, rounded. Both at most 32768, so it can't overflow.
inline uint16_t mulQ15(uint16_t a, uint16_t b) {
  return static_cast<uint16_t>((static_cast<uint32_t>(a) * b + 16384u) >> 15);
}

}  // namespace vol
