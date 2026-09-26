#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Percentile summary of a batch of samples, for the spike's measurement
// logs. Sorts the caller's array in place (pass a copy if the order
// matters); no allocation.
struct Percentiles {
  uint32_t n = 0;
  float min = 0, p10 = 0, p50 = 0, p90 = 0, p95 = 0, max = 0, mean = 0;

  static Percentiles of(float* v, uint32_t n) {
    Percentiles p;
    p.n = n;
    if (n == 0) return p;
    std::sort(v, v + n);
    double sum = 0;
    for (uint32_t i = 0; i < n; ++i) sum += v[i];
    p.mean = static_cast<float>(sum / n);
    p.min = v[0];
    p.max = v[n - 1];
    p.p10 = at(v, n, 0.10f);
    p.p50 = at(v, n, 0.50f);
    p.p90 = at(v, n, 0.90f);
    p.p95 = at(v, n, 0.95f);
    return p;
  }

  // Linear interpolation between the closest ranks, on sorted data.
  static float at(const float* sorted, uint32_t n, float q) {
    if (n == 0) return 0;
    const float pos = q * static_cast<float>(n - 1);
    const auto lo = static_cast<uint32_t>(std::floor(pos));
    const uint32_t hi = lo + 1 < n ? lo + 1 : lo;
    const float f = pos - static_cast<float>(lo);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * f;
  }
};
