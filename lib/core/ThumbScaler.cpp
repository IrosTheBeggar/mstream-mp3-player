// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ThumbScaler.h"

#include <cstdlib>
#include <cstring>

namespace {
void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }
}  // namespace

ThumbScaler::ThumbScaler(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

ThumbScaler::~ThumbScaler() { drop(); }

void ThumbScaler::drop() {
  for (int k = 0; k < kMaxSizes; ++k) {
    if (planes_[k].acc) freeFn_(planes_[k].acc);
    planes_[k] = Plane{};
  }
  n_ = 0;
  held_ = 0;
}

int ThumbScaler::decoderScale(int w, int h, int target) {
  const int shorter = w < h ? w : h;
  int s = 0;
  while (s < 3 && (shorter >> (s + 1)) >= target) ++s;
  return s;
}

bool ThumbScaler::begin(int srcW, int srcH, const int* sizes, int n) {
  drop();
  if (srcW <= 0 || srcH <= 0 || n <= 0 || n > kMaxSizes) return false;
  for (int k = 0; k < n; ++k) {
    const int s = sizes[k];
    if (s <= 0 || s > 1024) {
      drop();
      return false;
    }
    const size_t bytes = static_cast<size_t>(s) * s * 4 * sizeof(uint32_t);
    planes_[k].acc = static_cast<uint32_t*>(allocFn_(bytes));
    if (!planes_[k].acc) {
      drop();
      return false;
    }
    std::memset(planes_[k].acc, 0, bytes);
    planes_[k].size = s;
    held_ += bytes;
    n_ = k + 1;
  }
  side_ = srcW < srcH ? srcW : srcH;
  cropX_ = (srcW - side_) / 2;
  cropY_ = (srcH - side_) / 2;
  return true;
}

void ThumbScaler::add(int x, int y, int w, int h, const uint8_t* rgb) {
  if (n_ == 0 || !rgb) return;
  for (int j = 0; j < h; ++j) {
    const int sy = y + j - cropY_;
    if (sy < 0 || sy >= side_) continue;
    for (int i = 0; i < w; ++i) {
      const int sx = x + i - cropX_;
      if (sx < 0 || sx >= side_) continue;
      const uint8_t* px = rgb + (static_cast<size_t>(j) * w + i) * 3;
      for (int k = 0; k < n_; ++k) {
        const Plane& p = planes_[k];
        const int t = p.size;
        // The output pixels this source pixel falls in: exactly one when
        // shrinking, several when enlarging.
        int x0 = static_cast<int>(static_cast<int64_t>(sx) * t / side_);
        int x1 = static_cast<int>(static_cast<int64_t>(sx + 1) * t / side_);
        int y0 = static_cast<int>(static_cast<int64_t>(sy) * t / side_);
        int y1 = static_cast<int>(static_cast<int64_t>(sy + 1) * t / side_);
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (x1 > t) x1 = t;
        if (y1 > t) y1 = t;
        for (int ty = y0; ty < y1; ++ty) {
          uint32_t* a = p.acc + (static_cast<size_t>(ty) * t + x0) * 4;
          for (int tx = x0; tx < x1; ++tx, a += 4) {
            a[0] += px[0];
            a[1] += px[1];
            a[2] += px[2];
            a[3] += 1;
          }
        }
      }
    }
  }
}

void ThumbScaler::finish(int k, uint16_t* out) const {
  if (k < 0 || k >= n_ || !out) return;
  const Plane& p = planes_[k];
  const size_t n = static_cast<size_t>(p.size) * p.size;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t* a = p.acc + i * 4;
    const uint32_t c = a[3];
    if (c == 0) {
      out[i] = 0;
      continue;
    }
    out[i] = rgb565be(static_cast<uint8_t>((a[0] + c / 2) / c), static_cast<uint8_t>((a[1] + c / 2) / c),
                      static_cast<uint8_t>((a[2] + c / 2) / c));
  }
}
