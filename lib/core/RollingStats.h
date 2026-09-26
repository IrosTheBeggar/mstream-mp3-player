#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// The last N values with a timestamp each (any unit that counts up, e.g.
// track frames or milliseconds), summarised over a recent window: count,
// median and 95th percentile of the magnitude, and the signed mean. For the
// dance diagnostics' phase error. No allocation; summary() sorts a copy on
// the stack, so keep N small.
template <uint32_t N>
class RollingStats {
public:
  struct Summary {
    uint32_t count = 0;
    float medianAbs = 0.0f;
    float p95Abs = 0.0f;
    float mean = 0.0f;
  };

  void clear() { size_ = next_ = 0; }
  void add(float value, uint32_t stamp) {
    values_[next_] = value;
    stamps_[next_] = stamp;
    next_ = (next_ + 1) % N;
    if (size_ < N) ++size_;
  }
  uint32_t size() const { return size_; }

  // Over the values stamped within `window` of `now` (now - stamp <= window).
  Summary summary(uint32_t now, uint32_t window) const {
    float mags[N];
    Summary s;
    float sum = 0.0f;
    for (uint32_t i = 0; i < size_; ++i) {
      if (now - stamps_[i] > window) continue;
      mags[s.count++] = std::fabs(values_[i]);
      sum += values_[i];
    }
    if (s.count == 0) return s;
    std::sort(mags, mags + s.count);
    s.medianAbs = s.count % 2 ? mags[s.count / 2] : (mags[s.count / 2 - 1] + mags[s.count / 2]) / 2.0f;
    const auto i95 = static_cast<uint32_t>(std::ceil(0.95f * static_cast<float>(s.count))) - 1;
    s.p95Abs = mags[i95 < s.count ? i95 : s.count - 1];
    s.mean = sum / static_cast<float>(s.count);
    return s;
  }

private:
  float values_[N] = {};
  uint32_t stamps_[N] = {};
  uint32_t size_ = 0;
  uint32_t next_ = 0;
};
