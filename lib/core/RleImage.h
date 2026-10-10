// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// A run-length coded picture of a few flat colours with anti-aliased edges
// (the boot screen's logo: LogoArt, written by tools/make_logo.py). Each
// pixel is a part (0 nothing; 1-3, each drawn in its own colour) and how
// much of it the part covers (1-32 of 32). A row is a run of one-byte
// tokens, none crossing the row's end:
//
//   00nnnnnn    n+1 pixels of nothing (1-64)
//   pp0nnnnn    n+1 pixels of part pp, wholly covered (1-32)
//   pp1aaaaa    one pixel of part pp, covered a+1 of 32 (an edge)
//
// The reader hands out a row at a time as pixel codes, and a Palette turns
// a code into RGB565: each part's colour blended into the background at
// each coverage, worked out once per drawing. So a picture costs its runs
// in flash (the 240 px logo ~2 KB; RGB565 would be 22 KB) and a row's
// buffer while it is drawn, and it can be drawn on any background.
namespace rleimage {

constexpr int kParts = 3;
constexpr int kLevels = 32;

// A pixel's code: part << 6 | coverage (1..kLevels); 0 is nothing.
constexpr uint8_t code(int part, int level) {
  return part > 0 && level > 0 ? static_cast<uint8_t>(part << 6 | level) : 0;
}
constexpr int partOf(uint8_t c) { return c >> 6; }
constexpr int levelOf(uint8_t c) { return c & 0x3F; }

class Reader {
public:
  Reader(const uint8_t* data, size_t size, int w) : data_(data), size_(size), w_(w) {}
  // The next row's w codes into `out`. False when the data ended first or
  // a run crossed the row's end (data that isn't such a picture): `out` is
  // then all nothing, and so is every later row.
  bool row(uint8_t* out);
  // Every byte read, and no more (after the last row).
  bool done() const { return !bad_ && at_ == size_; }

private:
  const uint8_t* data_;
  size_t size_;
  int w_;
  size_t at_ = 0;
  bool bad_ = false;
};

// RGB565 of `fg` over `bg` (RGB888, 0xRRGGBB) at `level` of kLevels.
uint16_t blend565(uint32_t fg, uint32_t bg, int level);

// Each part's colour at every coverage over one background, as RGB565, or
// byte-swapped for a push to the LCD (M5GFX's swap565_t, what the panel
// takes as it is).
class Palette {
public:
  // `parts[p]`: part p's colour (index 0 unused), RGB888.
  void set(const uint32_t parts[kParts + 1], uint32_t bg, bool swapped);
  uint16_t operator()(uint8_t c) const { return lut_[partOf(c)][levelOf(c) <= kLevels ? levelOf(c) : kLevels]; }

private:
  uint16_t lut_[kParts + 1][kLevels + 1] = {};
};

}  // namespace rleimage
