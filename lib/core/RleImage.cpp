// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RleImage.h"

#include <cstring>

namespace rleimage {

bool Reader::row(uint8_t* out) {
  memset(out, 0, static_cast<size_t>(w_));
  if (bad_) return false;
  int x = 0;
  while (x < w_) {
    if (at_ >= size_) {
      bad_ = true;
      break;
    }
    const uint8_t b = data_[at_++];
    const int part = b >> 6;
    int n;
    uint8_t c;
    if (part == 0) {
      n = (b & 0x3F) + 1;
      c = 0;
    } else if (b & 0x20) {
      n = 1;
      c = code(part, (b & 0x1F) + 1);
    } else {
      n = (b & 0x1F) + 1;
      c = code(part, kLevels);
    }
    if (x + n > w_) {
      bad_ = true;
      break;
    }
    memset(out + x, c, static_cast<size_t>(n));
    x += n;
  }
  if (bad_) memset(out, 0, static_cast<size_t>(w_));
  return !bad_;
}

namespace {
// One channel, 8 bits: bg towards fg by level / kLevels, rounded.
uint32_t mix(uint32_t fg, uint32_t bg, int level) {
  const int f = static_cast<int>(fg), b = static_cast<int>(bg);
  return static_cast<uint32_t>(b + ((f - b) * level + (f >= b ? kLevels / 2 : -kLevels / 2)) / kLevels);
}
}  // namespace

uint16_t blend565(uint32_t fg, uint32_t bg, int level) {
  if (level < 0) level = 0;
  if (level > kLevels) level = kLevels;
  const uint32_t r = mix(fg >> 16 & 0xFF, bg >> 16 & 0xFF, level);
  const uint32_t g = mix(fg >> 8 & 0xFF, bg >> 8 & 0xFF, level);
  const uint32_t b = mix(fg & 0xFF, bg & 0xFF, level);
  // To 5-6-5 by the top bits, as M5GFX's color565() and ui/Theme's
  // colours (#F0F3F8 is col::TXT's 0xF79F).
  return static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

void Palette::set(const uint32_t parts[kParts + 1], uint32_t bg, bool swapped) {
  for (int p = 0; p <= kParts; ++p) {
    for (int l = 0; l <= kLevels; ++l) {
      const uint16_t c = blend565(p ? parts[p] : bg, bg, p ? l : 0);
      lut_[p][l] = swapped ? static_cast<uint16_t>(c >> 8 | c << 8) : c;
    }
  }
}

}  // namespace rleimage
