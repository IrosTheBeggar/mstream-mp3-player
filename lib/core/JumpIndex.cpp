// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "JumpIndex.h"

#include "TextFold.h"

namespace jump {

namespace {

// The first row in [lo, hi) whose key is >= want (keys never go down).
template <typename KeyOf>
uint32_t lowerBound(uint32_t lo, uint32_t hi, int want, KeyOf key) {
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (key(mid) < want) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

}  // namespace

void letters(uint32_t count, NameFn name, void* ctx, int32_t first[kCells], int32_t end[kCells]) {
  auto key = [&](uint32_t row) { return textfold::bucketOf(textfold::railKey(name(ctx, row))); };
  uint32_t lo = 0;
  for (int b = 0; b < kCells; ++b) {
    const uint32_t start = lowerBound(lo, count, b, key);
    const uint32_t stop = lowerBound(start, count, b + 1, key);
    first[b] = start < stop ? static_cast<int32_t>(start) : -1;
    if (end) end[b] = start < stop ? static_cast<int32_t>(stop) : -1;
    lo = stop;
  }
}

void seconds(uint32_t begin, uint32_t end, NameFn name, void* ctx, int32_t first[kCells]) {
  auto key = [&](uint32_t row) { return textfold::bucketOf(textfold::secondKey(name(ctx, row))); };
  for (int k = 0; k < kCells; ++k) first[k] = -1;
  if (begin >= end) return;
  first[0] = static_cast<int32_t>(begin);
  uint32_t lo = begin;
  for (int k = 1; k < kCells; ++k) {
    const uint32_t start = lowerBound(lo, end, k, key);
    if (start < end && key(start) == k) first[k] = static_cast<int32_t>(start);
    lo = start;
  }
}

int cellAt(int x, int y, int x0, int y0, int w, int h) {
  if (x < x0 || y < y0 || w <= 0 || h <= 0) return -1;
  const int c = (x - x0) / w, r = (y - y0) / h;
  if (c >= 7 || r >= 4) return -1;
  return r * 7 + c;
}

}  // namespace jump
