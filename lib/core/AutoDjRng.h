// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The Auto DJ engine's one random stream: PCG32 (O'Neill's pcg32_random_r,
// XSH RR 64/32: a 64-bit LCG state, a 32-bit output) seeded as
// pcg32_srandom_r(seed, stream), and below(n) by Lemire's multiply-shift
// with rejection (uniform, no modulo bias, one next() almost always).
// Integers only, so the host, the device and tools/autodj_ref.py's Pcg32
// give the same numbers from the same seed (test/fixtures/autodj: rng);
// a session's seed reproduces it (the console's Ds<hex>). Portable,
// header-only, host-tested (test_autodj_tiers).
namespace autodj {

class Rng {
public:
  static constexpr uint64_t kMultiplier = 6364136223846793005ULL;
  // The sessions' stream: "AUTODJ".
  static constexpr uint64_t kStream = 0x4155544F444AULL;

  explicit Rng(uint64_t seed = 0, uint64_t stream = kStream) { reseed(seed, stream); }

  void reseed(uint64_t seed, uint64_t stream = kStream) {
    inc_ = (stream << 1) | 1u;
    state_ = 0;
    next();
    state_ += seed;
    next();
  }

  uint32_t next() {
    const uint64_t old = state_;
    state_ = old * kMultiplier + inc_;
    const uint32_t x = static_cast<uint32_t>(((old >> 18) ^ old) >> 27);
    const uint32_t r = static_cast<uint32_t>(old >> 59);
    return (x >> r) | (x << ((32u - r) & 31u));
  }

  // Uniform in [0, n); n is at least 1 (0 gives 0 and draws nothing).
  uint32_t below(uint32_t n) {
    if (n == 0) return 0;
    uint64_t m = static_cast<uint64_t>(next()) * n;
    uint32_t lo = static_cast<uint32_t>(m);
    if (lo < n) {
      const uint32_t t = (0u - n) % n;  // 2^32 mod n
      while (lo < t) {
        m = static_cast<uint64_t>(next()) * n;
        lo = static_cast<uint32_t>(m);
      }
    }
    return static_cast<uint32_t>(m >> 32);
  }

private:
  uint64_t state_ = 0;
  uint64_t inc_ = 1;
};

}  // namespace autodj
