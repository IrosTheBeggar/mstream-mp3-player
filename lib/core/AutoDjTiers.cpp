// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjTiers.h"

namespace autodj {

uint8_t floorLevel(uint32_t percent, uint32_t selFloor) {
  const uint32_t p = percent > 100 ? 100 : percent;
  const uint32_t level = (255 * p + 99) / 100;
  const uint32_t clamped = level < selFloor ? selFloor : level;
  return static_cast<uint8_t>(clamped > 255 ? 255 : clamped);
}

void windows(int ref, int tolerance, Window out[3]) {
  const int half = (ref + 1) / 2;
  const int centres[3] = {ref, half, 2 * ref};
  for (int i = 0; i < 3; ++i) {
    out[i].lo = centres[i] - tolerance;
    out[i].hi = centres[i] + tolerance;
  }
}

namespace {
bool inWindows(int bpm, int ref, int tolerance) {
  Window w[3];
  windows(ref, tolerance, w);
  for (const Window& x : w)
    if (bpm >= x.lo && bpm <= x.hi) return true;
  return false;
}
}  // namespace

Tempo tempoStatus(uint8_t bpm, uint8_t ref) {
  if (ref == 0) return Tempo::Na;
  if (bpm == 0) return Tempo::Unknown;
  if (inWindows(bpm, ref, kTightBpm)) return Tempo::Tight;
  if (inWindows(bpm, ref, kWideBpm)) return Tempo::Wide;
  return Tempo::Wrong;
}

bool camelotNear(uint8_t anchor, uint8_t code) {
  const int d = ((anchor - 1) % 12 - (code - 1) % 12 + 12) % 12;
  return d == 0 || d == 1 || d == 11;
}

Key keyStatus(uint8_t code, uint8_t anchor, bool on) {
  if (!on) return Key::Na;
  if (code == 0) return Key::Unknown;
  if (anchor == 0 || camelotNear(anchor, code)) return Key::Good;
  return Key::Wrong;
}

uint8_t musicRank(Tempo t, Key k) {
  if (t != Tempo::Na && k != Key::Na) {
    // [tempo: Tight, Wide, Wrong, Unknown][key: Good, Wrong, Unknown]
    static const uint8_t kBoth[4][3] = {{0, 3, 2}, {1, 4, 4}, {7, 7, 7}, {5, 7, 6}};
    return kBoth[static_cast<int>(t) - 1][static_cast<int>(k) - 1];
  }
  if (t != Tempo::Na) {
    static const uint8_t kTempo[4] = {0, 1, 3, 2};  // Tight, Wide, Wrong, Unknown
    return kTempo[static_cast<int>(t) - 1];
  }
  if (k != Key::Na) {
    static const uint8_t kKey[3] = {0, 2, 1};  // Good, Wrong, Unknown
    return kKey[static_cast<int>(k) - 1];
  }
  return 0;
}

}  // namespace autodj
