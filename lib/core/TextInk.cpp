// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TextInk.h"

#include "TextFold.h"

namespace textink {

namespace {
constexpr size_t kHeader = 24, kGlyph = 28;

// What M5GFX counts towards the line's ascent and descent
// (VLWfont::loadFont()): past Latin-1, and printable ASCII; never the
// ideographic space.
bool counts(uint32_t cp) {
  const uint16_t u = static_cast<uint16_t>(cp);  // (M5GFX keeps 16 bits)
  return (u > 0xFF || (u > 0x20 && u < 0xA0 && u != 0x7F)) && u != 0x3000;
}
}  // namespace

uint32_t Vlw::be32(size_t at) const {
  return static_cast<uint32_t>(data_[at]) << 24 | static_cast<uint32_t>(data_[at + 1]) << 16 |
         static_cast<uint32_t>(data_[at + 2]) << 8 | data_[at + 3];
}

bool Vlw::open(const uint8_t* data, size_t size) {
  data_ = nullptr;
  count_ = 0;
  baseline_ = lineH_ = 0;
  if (!data || size < kHeader) return false;
  data_ = data;
  const uint32_t n = be32(0);
  if (n == 0 || n > (size - kHeader) / kGlyph) {
    data_ = nullptr;
    return false;
  }
  count_ = n;
  const int32_t ascent = static_cast<int32_t>(be32(16)), descent = static_cast<int32_t>(be32(20));
  int maxAscent = ascent < 0 ? -ascent : ascent;
  int maxDescent = descent < 0 ? -descent : descent;
  for (uint32_t i = 0; i < n; ++i) {
    const size_t g = kHeader + static_cast<size_t>(i) * kGlyph;
    if (!counts(be32(g))) continue;
    const int h = static_cast<uint16_t>(be32(g + 4));
    const int dY = static_cast<int16_t>(be32(g + 16));
    if (maxAscent < dY) maxAscent = dY;
    if (maxDescent < h - dY) maxDescent = h - dY;
  }
  baseline_ = maxAscent;
  lineH_ = maxAscent + maxDescent;
  return true;
}

bool Vlw::rowsOf(uint32_t cp, Rows* out) const {
  if (!data_ || cp > 0xFFFF) return false;
  uint32_t lo = 0, hi = count_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (be32(kHeader + static_cast<size_t>(mid) * kGlyph) < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const size_t g = kHeader + static_cast<size_t>(lo) * kGlyph;
  if (lo == count_ || be32(g) != cp) return false;
  const int h = static_cast<uint16_t>(be32(g + 4));
  const int w = static_cast<uint8_t>(be32(g + 8));
  const int dY = static_cast<int16_t>(be32(g + 16));
  Rows r;
  if (h > 0 && w > 0) {
    r.top = baseline_ - dY;
    r.bottom = r.top + h - 1;
  }
  *out = r;
  return true;
}

Rows inkOf(const Vlw& font, const glyphstore::Store& store, const char* text) {
  Rows ink;
  const int baseline = store.baselineIn(font.baseline(), font.lineHeight());
  for (const char* p = text; p && *p;) {
    const uint32_t cp = textfold::decode(p);
    if (cp <= 0x20 || (cp >= 0xFE00 && cp < 0xFE10)) continue;  // (a space only advances)
    Rows g;
    if (!font.rowsOf(cp, &g)) {
      const glyphstore::Glyph glyph = store.find(cp);
      if (!glyph) continue;  // M5GFX's blank
      const glyphstore::Metrics m = store.metrics(glyph);
      const glyphstore::Placement at = glyphstore::place(m, baseline, font.lineHeight());
      if (m.w == 0 || at.rows <= 0) continue;
      g.top = at.top;
      g.bottom = at.top + at.rows - 1;
    }
    if (g.empty()) continue;
    if (ink.empty()) {
      ink = g;
    } else {
      if (g.top < ink.top) ink.top = g.top;
      if (g.bottom > ink.bottom) ink.bottom = g.bottom;
    }
  }
  return ink;
}

}  // namespace textink
