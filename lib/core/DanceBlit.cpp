// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceBlit.h"

#include <cstddef>

namespace dance {

static_assert(lutEntry(0xFF, 0, 0) == 0x00F8, "B.9, byte-swapped");
static_assert(lutEntry(0x08, 0x0C, 0x10) == 0x6208, "the stage's background, 0x0862");

namespace {

const uint8_t* badRow(const uint8_t* end, bool* ok) {
  if (ok) *ok = false;
  return end;
}

int64_t clampTo(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

void reverseRow(uint8_t* row, int w) {
  for (int a = 0, b = w - 1; a < b; ++a, --b) {
    const uint8_t t = row[a];
    row[a] = row[b];
    row[b] = t;
  }
}

}  // namespace

void makeLut(const uint8_t* rgb, uint16_t lut[16]) {
  if (!lut) return;
  lut[0] = 0;
  for (int i = 1; i < 16; ++i) lut[i] = rgb ? lutEntry(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2]) : 0;
}

const uint8_t* decodeRow(const uint8_t* p, const uint8_t* end, int w, uint8_t* row, bool* ok) {
  if (w < 1 || w > kBlitRowMax || !row) return badRow(end, ok);
  for (int i = 0; i < w; ++i) row[i] = 0;
  if (!p || !end || p > end || end - p < 2) return badRow(end, ok);
  const int skip = p[0], n = p[1];
  p += 2;
  if (skip > w) return badRow(end, ok);
  int x = skip;
  for (int t = 0; t < n; ++t) {
    if (p >= end) return badRow(end, ok);  // the bytes ran out
    const uint8_t token = *p++;
    const int run = (token >> 4) + 1;
    if (x + run > w) return badRow(end, ok);  // the runs pass w
    const uint8_t index = token & 15u;
    for (int i = 0; i < run; ++i) row[x + i] = index;
    x += run;
  }
  return p;
}

bool decodeGrid(const PackFrame& f, int cellW, int cellH, uint8_t* grid) {
  if (!grid || cellW < 0 || cellH < 0) return false;
  const size_t cells = static_cast<size_t>(cellW) * static_cast<size_t>(cellH);
  for (size_t i = 0; i < cells; ++i) grid[i] = 0;
  const FrameBox& b = f.box;
  if (b.w == 0) return f.len == 0;
  if (b.x0 + b.w > cellW || b.y0 + b.h > cellH || b.w > kBlitRowMax || !f.data) return false;
  const uint8_t* p = f.data;
  const uint8_t* const end = f.data + f.len;
  bool ok = true;
  for (int y = 0; y < b.h && ok; ++y) {
    p = decodeRow(p, end, b.w, grid + static_cast<size_t>(b.y0 + y) * static_cast<size_t>(cellW) + b.x0, &ok);
  }
  return ok && p == end;
}

void blit(const BlitCanvas& c, const PackFrame& f, int left, int top, int scale, bool mirrored,
          const uint16_t lut[16]) {
  const int w = f.box.w, h = f.box.h;
  if (scale < 1 || scale > kMaxScale || w == 0 || w > kBlitRowMax || !f.data || !c.px || !lut) return;
  if (c.w <= 0 || c.h <= 0 || c.stride < c.w) return;
  uint8_t row[kBlitRowMax];
  const uint8_t* p = f.data;
  const uint8_t* const end = f.data + f.len;
  for (int y = 0; y < h; ++y) {
    bool ok = true;
    p = decodeRow(p, end, w, row, &ok);  // a broken row is drawn as far as it went, then the frame ends
    // The clamps in 64 bits (left and top may be anything); each result lies
    // in [0, c.w] or [0, c.h], so the fills run on int and a pointer.
    const int64_t rowTop = static_cast<int64_t>(top) + static_cast<int64_t>(y) * scale;
    const int y0 = static_cast<int>(clampTo(rowTop, 0, c.h));
    const int y1 = static_cast<int>(clampTo(rowTop + scale, 0, c.h));
    if (y0 < y1) {
      if (mirrored) reverseRow(row, w);  // row[i] is now canvas-side column i within the box
      for (int i = 0; i < w;) {
        const uint8_t index = row[i];
        int j = i + 1;
        while (j < w && row[j] == index) ++j;
        if (index) {
          const int x0 = static_cast<int>(clampTo(static_cast<int64_t>(left) + static_cast<int64_t>(i) * scale, 0, c.w));
          const int x1 = static_cast<int>(clampTo(static_cast<int64_t>(left) + static_cast<int64_t>(j) * scale, 0, c.w));
          if (x0 < x1) {
            const uint16_t value = lut[index];
            for (int yy = y0; yy < y1; ++yy) {
              uint16_t* q = c.px + static_cast<ptrdiff_t>(yy) * c.stride + x0;
              for (int n = x1 - x0; n > 0; --n) *q++ = value;
            }
          }
        }
        i = j;
      }
    }
    if (!ok) break;
  }
}

}  // namespace dance
