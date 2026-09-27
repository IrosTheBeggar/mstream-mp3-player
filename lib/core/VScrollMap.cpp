#include "VScrollMap.h"

namespace {
int32_t floorMod(int32_t a, int32_t m) {
  const int32_t r = a % m;
  return r < 0 ? r + m : r;
}
}  // namespace

void VScrollMap::configure(int top, int height, int maxStep) {
  top_ = top < 0 ? 0 : top;
  height_ = height < 1 ? 1 : height;
  maxStep_ = maxStep <= 0 || maxStep >= height_ ? height_ - 1 : maxStep;
  offset_ = 0;
  base_ = 0;
  shift_ = 0;
  valid_ = false;
  lastFull_ = false;
}

int VScrollMap::gramLineForContent(int32_t c) const { return top_ + static_cast<int>(floorMod(c - base_, height_)); }

int VScrollMap::gramLineForScreen(int y) const {
  if (y < top_ || y >= top_ + height_) return y;
  return top_ + static_cast<int>(floorMod(y - top_ + shift_, height_));
}

int VScrollMap::screenLineForGram(int gramY) const {
  if (gramY < top_ || gramY >= top_ + height_) return gramY;
  return top_ + static_cast<int>(floorMod(gramY - top_ - shift_, height_));
}

int VScrollMap::emit(int32_t c, int32_t n, Span* out) const {
  int used = 0;
  while (n > 0 && used < kMaxSpans) {
    const int32_t g = floorMod(c - base_, height_);
    const int32_t run = n < height_ - g ? n : height_ - g;  // up to the wrap
    out[used].contentY = c;
    out[used].gramY = top_ + g;
    out[used].h = run;
    ++used;
    c += run;
    n -= run;
  }
  return used;
}

int VScrollMap::plan(int32_t offset, Span out[kMaxSpans]) {
  const int32_t d = offset - offset_;
  int used = 0;
  lastFull_ = !valid_ || d > maxStep_ || -d > maxStep_;
  if (lastFull_) {
    // In place: keep the panel's start address, so screen line top + k
    // already shows the GRAM line that content offset + k is written to.
    base_ = offset - shift_;
    used = emit(offset, height_, out);
  } else if (d > 0) {
    used = emit(offset_ + height_, d, out);  // the list moved up: new lines at the bottom
  } else if (d < 0) {
    used = emit(offset, -d, out);  // moved down: new lines at the top
  }
  offset_ = offset;
  shift_ = static_cast<int>(floorMod(offset - base_, height_));
  valid_ = true;
  return used;
}
