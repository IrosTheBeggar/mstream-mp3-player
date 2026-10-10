// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "GlyphStore.h"

#include <cstring>

namespace glyphstore {

namespace {

// The format (tools/glyph_store.py): version 1, little-endian.
constexpr uint8_t kMagic[4] = {'G', 'L', 'Y', 'S'};
constexpr uint16_t kVersion = 1;
constexpr size_t kHeader = 32;
constexpr size_t kSection = 12;    // tag, offset, size
constexpr size_t kPageMap = 48;    // 8 u32 presence words, first glyph, bitmaps' start, 8 u8 counts
constexpr uint32_t kMaxSections = 64;
constexpr uint8_t kRaw = 0, kRuns = 1;

// Byte by byte: a store's fields are little-endian whatever the CPU, and
// the ESP32 faults on an unaligned word load.
uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

uint32_t bitCount(uint32_t v) {
  v = v - ((v >> 1) & 0x55555555u);
  v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
  return (((v + (v >> 4)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24;
}

int signed7(uint32_t v) { return (v & 0x40) ? static_cast<int>(v) - 128 : static_cast<int>(v); }

bool tagIs(const uint8_t* p, const char* tag) { return std::memcmp(p, tag, 4) == 0; }

// Coverage values from the nibbles at [p, end), the high one first.
class Nibbles {
public:
  Nibbles(const uint8_t* p, const uint8_t* end) : p_(p), end_(end) {}
  // The next value; -1 past the end.
  int next() {
    if (high_) {
      if (p_ >= end_) return -1;
      high_ = false;
      return *p_ >> 4;
    }
    high_ = true;
    return *p_++ & 15;
  }

private:
  const uint8_t* p_;
  const uint8_t* end_;
  bool high_ = true;
};

// Writes values into a box row by row (`w` a row, rows `stride` apart).
class Box {
public:
  Box(uint8_t* out, uint32_t w, size_t stride, bool bytes)
      : out_(out), w_(w), stride_(stride), scale_(bytes ? 17 : 1) {}
  // `n` values `v`, which may run on into the rows below; a value at a
  // time (the runs are short: memset's call would cost more).
  void put(int v, uint32_t n) {
    const uint8_t c = static_cast<uint8_t>(v * scale_);
    while (n--) {
      out_[at_ + x_] = c;
      if (++x_ == w_) {
        x_ = 0;
        at_ += stride_;
      }
    }
  }

private:
  uint8_t* out_;
  uint32_t w_;
  size_t stride_;
  int scale_;
  size_t at_ = 0;  // the row's start
  uint32_t x_ = 0;
};

}  // namespace

bool Store::open(const void* data, size_t size) {
  *this = Store();
  const auto* d = static_cast<const uint8_t*>(data);
  if (!d || size < kHeader || std::memcmp(d, kMagic, 4) != 0 || le16(d + 4) != kVersion) return false;
  const uint32_t sections = le16(d + 6);
  const uint32_t total = le32(d + 28);
  if (total > size || sections > kMaxSections || kHeader + kSection * sections > total) return false;
  const uint8_t bpp = d[9], encoding = d[10];
  if (bpp != 4 || (encoding != kRaw && encoding != kRuns) || d[8] == 0) return false;
  const uint32_t count = le32(d + 20), pageCount = le32(d + 24);
  if (pageCount > 0x1100 || count > pageCount * 256) return false;

  const uint8_t *pages = nullptr, *pageMap = nullptr, *metrics = nullptr, *offsets = nullptr, *bits = nullptr;
  uint32_t bitsSize = 0;
  for (uint32_t i = 0; i < sections; ++i) {
    const uint8_t* s = d + kHeader + kSection * i;
    const uint32_t at = le32(s + 4), len = le32(s + 8);
    if (at > total || len > total - at) return false;
    // Each the size its counts need (BITS any); a tag this reader doesn't know is skipped.
    if (tagIs(s, "PAGE")) {
      if (len < 2 * pageCount) return false;
      pages = d + at;
    } else if (tagIs(s, "PMAP")) {
      if (len < kPageMap * pageCount) return false;
      pageMap = d + at;
    } else if (tagIs(s, "METR")) {
      if (len < 4 * count) return false;
      metrics = d + at;
    } else if (tagIs(s, "OFFS")) {
      if (len < 2 * count) return false;
      offsets = d + at;
    } else if (tagIs(s, "BITS")) {
      bits = d + at;
      bitsSize = len;
    }
  }
  if (!pages || !pageMap || !metrics || !offsets || !bits) return false;

  // The pages ascending, each one's glyphs numbered on from the last's, all
  // of them the header's count: so find() never reads past METR or OFFS.
  uint32_t glyphs = 0;
  for (uint32_t p = 0; p < pageCount; ++p) {
    const uint32_t page = le16(pages + 2 * p);
    if (page > 0x10FF || (p > 0 && page <= le16(pages + 2 * (p - 1)))) return false;
    const uint8_t* m = pageMap + kPageMap * p;
    if (le32(m + 32) != glyphs || le32(m + 36) > bitsSize) return false;
    uint32_t before = 0;
    for (int w = 0; w < 8; ++w) {
      if (m[40 + w] != before) return false;
      before += bitCount(le32(m + 4 * w));
    }
    glyphs += before;
  }
  if (glyphs != count) return false;

  pages_ = pages;
  pageMap_ = pageMap;
  metrics_ = metrics;
  offsets_ = offsets;
  bits_ = bits;
  bitsSize_ = bitsSize;
  pageCount_ = pageCount;
  count_ = count;
  pixelSize_ = d[8];
  encoding_ = encoding;
  ascent_ = static_cast<int8_t>(d[12]);
  descent_ = static_cast<int8_t>(d[13]);
  inkAscent_ = static_cast<int8_t>(d[14]);
  inkDescent_ = static_cast<int8_t>(d[15]);
  maxW_ = d[16];
  maxH_ = d[17];
  return true;
}

Glyph Store::find(uint32_t cp) const {
  Glyph g;
  if (!bits_ || cp > 0x10FFFF) return g;
  const uint32_t page = cp >> 8;
  uint32_t lo = 0, hi = pageCount_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (le16(pages_ + 2 * mid) < page) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == pageCount_ || le16(pages_ + 2 * lo) != page) return g;
  const uint8_t* m = pageMap_ + kPageMap * lo;
  const uint32_t bit = cp & 0xFF;
  const uint32_t word = le32(m + 4 * (bit >> 5));
  const uint32_t mask = 1u << (bit & 31);
  if (!(word & mask)) return g;
  g.index = le32(m + 32) + m[40 + (bit >> 5)] + bitCount(word & (mask - 1));
  g.page = lo;
  return g;
}

Metrics Store::metrics(const Glyph& g) const {
  Metrics r;
  if (!g || g.index >= count_) return r;
  const uint32_t v = le32(metrics_ + 4 * g.index);
  r.w = static_cast<uint8_t>(v & 63);
  r.h = static_cast<uint8_t>(v >> 6 & 63);
  r.x = static_cast<int8_t>(signed7(v >> 12 & 0x7F));
  r.y = static_cast<int8_t>(signed7(v >> 19 & 0x7F));
  r.advance = static_cast<uint8_t>(v >> 26);
  return r;
}

int Store::advance(uint32_t cp) const {
  const Glyph g = find(cp);
  return g ? metrics(g).advance : -1;
}

int Store::baselineIn(int fontBaseline, int lineH) const {
  int b = fontBaseline > ascent_ ? fontBaseline : ascent_;
  if (b + descent_ > lineH) b = lineH - descent_;
  return b;
}

Placement place(const Metrics& m, int baseline, int lineH) {
  Placement p;
  p.top = baseline - m.y;
  if (p.top < 0) p.top = 0;
  p.rows = m.h < lineH - p.top ? m.h : lineH - p.top;
  if (p.rows < 0) p.rows = 0;
  return p;
}

bool Store::decode(const Glyph& g, uint8_t* out, size_t stride, size_t outSize, Alpha alpha) const {
  if (!g || g.index >= count_ || g.page >= pageCount_) return false;
  const Metrics m = metrics(g);
  if (m.w == 0 || m.h == 0) return true;
  if (!out || stride < m.w || (m.h - 1) * stride + m.w > outSize) return false;
  Box box(out, m.w, stride, alpha == Alpha::Byte);
  // Its bitmap: its page's start in BITS, and its own offset from there.
  const uint32_t at = le32(pageMap_ + kPageMap * g.page + 36) + le16(offsets_ + 2 * g.index);
  Nibbles in(bits_ + (at < bitsSize_ ? at : bitsSize_), bits_ + bitsSize_);
  uint32_t left = static_cast<uint32_t>(m.w) * m.h;
  bool ok = true;
  while (left > 0) {
    const int v = in.next();
    if (v < 0) {
      ok = false;
      break;
    }
    uint32_t k = 1;
    if (encoding_ == kRuns && (v == 0 || v == 15)) {
      const int c = in.next();
      if (c < 0 || static_cast<uint32_t>(c) + 1 > left) {
        ok = false;
        break;
      }
      k = static_cast<uint32_t>(c) + 1;
    }
    box.put(v, k);
    left -= k;
  }
  if (!ok) {
    for (uint32_t r = 0; r < m.h; ++r) std::memset(out + r * stride, 0, m.w);
  }
  return ok;
}

}  // namespace glyphstore
