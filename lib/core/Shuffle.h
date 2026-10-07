// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The shuffle's loop (docs/QUEUE-MODES.md section 2.7): Fisher-Yates over
// any element type, from the end, with a xorshift32 seeded by the caller
// and j by multiply-shift (j = (x * (i + 1)) >> 32: a bias of at most
// n / 2^32, 2e-6 at 10,000). The same seed gives the same order, so the
// host tests repeat; the firmware seeds it from esp_random(). Only 2^32
// orders are reachable from a 32-bit seed: plenty for listening. No
// spreading of an artist's tracks. Allocates nothing. Portable,
// host-tested (test_queue).
namespace shuffle {

// a[0..n) shuffled in place; `seed` 0 is taken as 1 (xorshift's one
// fixed point).
template <typename T>
void permute(T* a, uint32_t n, uint32_t seed) {
  if (!a || n < 2) return;
  uint32_t x = seed ? seed : 1u;
  for (uint32_t i = n - 1; i > 0; --i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    const uint32_t j = static_cast<uint32_t>((static_cast<uint64_t>(x) * (i + 1)) >> 32);
    const T t = a[i];
    a[i] = a[j];
    a[j] = t;
  }
}

}  // namespace shuffle
