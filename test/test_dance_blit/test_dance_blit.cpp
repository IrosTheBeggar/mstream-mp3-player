// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the dance blitter (DanceBlit: lutEntry, makeLut,
// decodeRow, decodeGrid and blit): docs/DANCE-SPRITES.md, sections 6.2 and
// A.10, A.16 and D.4, item by item. Written from the doc alone, without
// reading the implementation.
//
// - The frames come from the fixture (TestPack.h) and A.17's dot, opened
//   by Pack::open from exact-size heap copies; the canvases and every
//   buffer handed to decodeRow and decodeGrid are exact-size heap blocks
//   too, so AddressSanitizer reports a write or read past them.
// - The reference draws from decodeGrid's grid, pixel by pixel, with D.1's
//   placement (box column i at i' = i, or w - 1 - i mirrored, each pixel
//   scale x scale); decodeGrid itself is pinned by the fixture's A.16
//   hashes first.
//
// Run: pio test -e native -f test_dance_blit (and native_asan in CI, D.5).
#include <unity.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../support/dance/PackEdit.h"
#include "DanceBlit.h"
#include "DancePack.h"
#include "Fnv1a.h"

using packedit::Bytes;
using packedit::CanaryCanvas;
using packedit::HeapCopy;
using packedit::Xorshift32;

namespace {

char g_ctx[200] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

// A pack opened from an exact-size copy, kept alive with its bytes.
struct Opened {
  HeapCopy buf;
  dance::Pack pack;
  bool open(const Bytes& b) {
    buf.assign(b);
    return pack.open(buf.data(), static_cast<uint32_t>(b.size()), static_cast<uint32_t>(b.size()));
  }
};

std::vector<uint8_t> gridOf(const dance::Pack& p, int i) {
  const dance::PackGeom g = p.geom();
  HeapCopy grid;
  std::vector<uint8_t> zero(static_cast<size_t>(g.cellW) * g.cellH, 0xEE);
  grid.assign(zero.data(), zero.size());
  TEST_ASSERT_TRUE_MESSAGE(dance::decodeGrid(p.frame(i), g.cellW, g.cellH, grid.data()), g_ctx);
  return std::vector<uint8_t>(grid.data(), grid.data() + grid.size());
}

// An exact-size heap canvas of 16-bit pixels.
struct Pixels {
  int w, h;
  uint16_t* px;
  Pixels(int w_, int h_, uint16_t fill) : w(w_), h(h_), px(new uint16_t[static_cast<size_t>(w_) * h_]) {
    for (int i = 0; i < w * h; ++i) px[i] = fill;
  }
  ~Pixels() { delete[] px; }
  Pixels(const Pixels&) = delete;
  Pixels& operator=(const Pixels&) = delete;
  dance::BlitCanvas canvas() const {
    dance::BlitCanvas c;
    c.px = px;
    c.w = static_cast<int16_t>(w);
    c.h = static_cast<int16_t>(h);
    c.stride = w;
    return c;
  }
  uint16_t at(int x, int y) const { return px[y * w + x]; }
};

void lutIdentity(uint16_t lut[16]) {
  for (int k = 0; k < 16; ++k) lut[k] = static_cast<uint16_t>(k);
}
void lutMarked(uint16_t lut[16]) {
  for (int k = 0; k < 16; ++k) lut[k] = static_cast<uint16_t>(0x7700 | k);
}

// D.1's placement, from the grid: box pixel (i, y) of index k != 0 fills
// [left + i'*s, +s) x [top + y*s, +s), i' = i or w - 1 - i, clipped to the
// canvas (x0 = ox.., a canvas of cw x ch inside px with the given stride).
void refBlit(uint16_t* px, int cw, int ch, int stride, const std::vector<uint8_t>& grid, int cellW,
             const dance::FrameBox& b, int left, int top, int s, bool mirrored, const uint16_t lut[16]) {
  for (int y = 0; y < b.h; ++y)
    for (int i = 0; i < b.w; ++i) {
      const uint8_t k = grid[static_cast<size_t>(b.y0 + y) * cellW + b.x0 + i];
      if (!k) continue;
      const int ip = mirrored ? b.w - 1 - i : i;
      for (int dy = 0; dy < s; ++dy)
        for (int dx = 0; dx < s; ++dx) {
          const int X = left + ip * s + dx, Y = top + y * s + dy;
          if (X >= 0 && X < cw && Y >= 0 && Y < ch) px[static_cast<size_t>(Y) * stride + X] = lut[k];
        }
    }
}

const Bytes& fixtureBytes() {
  static const Bytes b = packedit::testPack();
  return b;
}
const Bytes& dotBytes() {
  static const Bytes b = packedit::dotPack();
  return b;
}

}  // namespace

void setUp() {}
void tearDown() {}

// 1. A.16's hashes: drawn at scale 1, unmirrored, at the box's place in a
// cleared cell, through lut[i] = i; and decodeGrid's grid.
void test_frame_hashes() {
  struct Base {
    const Bytes* bytes;
    const char* name;
  };
  for (const Base& base : {Base{&fixtureBytes(), "fixture"}, Base{&dotBytes(), "dot"}}) {
    Opened o;
    TEST_ASSERT_TRUE(o.open(*base.bytes));
    const dance::PackGeom g = o.pack.geom();
    uint16_t lut[16];
    lutIdentity(lut);
    for (int i = 0; i < o.pack.frameCount(); ++i) {
      ctx("%s frame %d", base.name, i);
      const uint32_t want = base.bytes == &dotBytes() ? 0x18857B21u : dance::kTestPackFrameHash[i];
      const dance::PackFrame f = o.pack.frame(i);
      Pixels c(g.cellW, g.cellH, 0);
      dance::blit(c.canvas(), f, f.box.x0, f.box.y0, 1, false, lut);
      std::vector<uint8_t> low(static_cast<size_t>(c.w) * c.h);
      for (size_t k = 0; k < low.size(); ++k) {
        TEST_ASSERT_TRUE_MESSAGE(c.px[k] < 16, g_ctx);
        low[k] = static_cast<uint8_t>(c.px[k]);
      }
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(want, packedit::refFnv1a32(low.data(), low.size()), g_ctx);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(want, dance::fnv1a32(low.data(), low.size()), g_ctx);
      const std::vector<uint8_t> grid = gridOf(o.pack, i);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(want, packedit::refFnv1a32(grid.data(), grid.size()), g_ctx);
    }
    if (base.bytes == &fixtureBytes()) TEST_ASSERT_EQUAL_INT(dance::kTestPackFrameCount, o.pack.frameCount());
  }
}

// 2-3. Mirroring: drawn mirrored at left = cellW - x0 - w, the cell is the
// grid flipped, grid[y][cellW - 1 - x] (A.10).
void test_mirroring() {
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  const dance::PackGeom g = o.pack.geom();
  uint16_t lut[16];
  lutMarked(lut);
  for (int i = 0; i < o.pack.frameCount(); ++i) {
    ctx("mirrored frame %d", i);
    const dance::PackFrame f = o.pack.frame(i);
    const std::vector<uint8_t> grid = gridOf(o.pack, i);
    Pixels c(g.cellW, g.cellH, 0xBEEF);
    dance::blit(c.canvas(), f, g.cellW - f.box.x0 - f.box.w, f.box.y0, 1, true, lut);
    for (int y = 0; y < g.cellH; ++y)
      for (int x = 0; x < g.cellW; ++x) {
        const uint8_t k = grid[static_cast<size_t>(y) * g.cellW + (g.cellW - 1 - x)];
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(k ? lut[k] : 0xBEEF, c.at(x, y), g_ctx);
      }
  }
}

// 4. Scales 1-4, both orientations, every frame: the canvas is the cell
// grid scaled (and flipped), and nothing is written outside the box's
// w*scale x h*scale rect.
void test_scales() {
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  const dance::PackGeom g = o.pack.geom();
  uint16_t lut[16];
  lutMarked(lut);
  for (int i = 0; i < o.pack.frameCount(); ++i) {
    const dance::PackFrame f = o.pack.frame(i);
    const std::vector<uint8_t> grid = gridOf(o.pack, i);
    for (int s = 1; s <= 4; ++s)
      for (int m = 0; m < 2; ++m) {
        ctx("frame %d at scale %d%s", i, s, m ? ", mirrored" : "");
        Pixels c(g.cellW * s, g.cellH * s, 0xBEEF);
        const int left = (m ? g.cellW - f.box.x0 - f.box.w : f.box.x0) * s, top = f.box.y0 * s;
        dance::blit(c.canvas(), f, left, top, s, m != 0, lut);
        for (int Y = 0; Y < c.h; ++Y)
          for (int X = 0; X < c.w; ++X) {
            const int cx = m ? g.cellW - 1 - X / s : X / s;
            const uint8_t k = grid[static_cast<size_t>(Y / s) * g.cellW + cx];
            TEST_ASSERT_EQUAL_HEX16_MESSAGE(k ? lut[k] : 0xBEEF, c.at(X, Y), g_ctx);
            const bool inBox = X >= left && X < left + f.box.w * s && Y >= top && Y < top + f.box.h * s;
            if (!inBox) TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xBEEF, c.at(X, Y), g_ctx);
          }
      }
  }
}

// 5. Clipping: a 40x50 canvas inside a canary border of 4 px; every frame,
// scale and orientation, from fully off one side to fully off the other.
void test_clipping_canary() {
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  const dance::PackGeom g = o.pack.geom();
  uint16_t lut[16];
  lutMarked(lut);
  CanaryCanvas c(40, 50, 4), ref(40, 50, 4);
  for (int i = 0; i < o.pack.frameCount(); ++i) {
    const dance::PackFrame f = o.pack.frame(i);
    const std::vector<uint8_t> grid = gridOf(o.pack, i);
    for (int s = 1; s <= 4; ++s)
      for (int m = 0; m < 2; ++m) {
        const int ws = f.box.w * s, hs = f.box.h * s;
        const int lefts[] = {-ws - 1, -ws, -ws + 1, -ws / 2, -1, 0, 1, c.w - ws - 1, c.w - ws, c.w - ws / 2,
                             c.w - 1, c.w, c.w + 1};
        const int tops[] = {-hs - 1, -hs + 1, -hs / 2, -1, 0, 1, c.h - hs, c.h - 1, c.h};
        for (int left : lefts)
          for (int top : tops) {
            ctx("frame %d, scale %d%s at %d,%d", i, s, m ? " mirrored" : "", left, top);
            c.reset(0x1234);
            ref.reset(0x1234);
            dance::blit(c.canvas(), f, left, top, s, m != 0, lut);
            refBlit(&ref.at(0, 0), ref.w, ref.h, ref.stride, grid, g.cellW, f.box, left, top, s, m != 0, lut);
            TEST_ASSERT_TRUE_MESSAGE(c.borderIntact(), g_ctx);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ref.buf, c.buf, c.n * sizeof(uint16_t), g_ctx);
          }
      }
  }
}

// 6. The byte swap: an M5GFX RGB565 sprite holds each pixel big-endian.
void test_byte_swap() {
  static_assert(dance::lutEntry(0xFF, 0, 0) == 0x00F8, "lutEntry swaps the bytes");
  TEST_ASSERT_EQUAL_HEX16(0x00F8, dance::lutEntry(0xFF, 0, 0));
  TEST_ASSERT_EQUAL_HEX16(0x6208, dance::lutEntry(0x08, 0x0C, 0x10));  // the background, 0x0862
  TEST_ASSERT_EQUAL_HEX16(0xE007, dance::lutEntry(0, 0xFF, 0));
  TEST_ASSERT_EQUAL_HEX16(0x1F00, dance::lutEntry(0, 0, 0xFF));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, dance::lutEntry(0xFF, 0xFF, 0xFF));
  TEST_ASSERT_EQUAL_HEX16(0x0000, dance::lutEntry(0x07, 0x03, 0x07));
  // makeLut of both of the fixture's palettes: lutEntry per entry, lut[0] = 0.
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  for (const uint8_t* pal : {o.pack.palette(), o.pack.idlePalette()}) {
    uint16_t lut[16];
    for (auto& v : lut) v = 0xDEAD;
    dance::makeLut(pal, lut);
    TEST_ASSERT_EQUAL_HEX16(0, lut[0]);
    for (int k = 1; k < 16; ++k) {
      const uint8_t r = pal[3 * k], gg = pal[3 * k + 1], b = pal[3 * k + 2];
      const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (gg >> 2) << 5 | (b >> 3));
      TEST_ASSERT_EQUAL_HEX16(dance::lutEntry(r, gg, b), lut[k]);
      TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(v << 8 | v >> 8), lut[k]);
    }
  }
  TEST_ASSERT_EQUAL_HEX16(0x9FF7, dance::lutEntry(0xF0, 0xF0, 0xF8));  // the stick's ink, 0xF79F
  // lut[0] is 0 even when entry 0 isn't.
  uint8_t white[48];
  std::memset(white, 0xFF, sizeof white);
  uint16_t lut[16];
  dance::makeLut(white, lut);
  TEST_ASSERT_EQUAL_HEX16(0, lut[0]);
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, lut[1]);
  // The dot drawn through its palette: the canvas's bytes read F8 00.
  Opened dot;
  TEST_ASSERT_TRUE(dot.open(dotBytes()));
  dance::makeLut(dot.pack.palette(), lut);
  Pixels c(4, 4, 0);
  const dance::PackFrame f = dot.pack.frame(0);
  dance::blit(c.canvas(), f, f.box.x0, f.box.y0, 1, false, lut);
  uint8_t bytes[2];
  std::memcpy(bytes, &c.px[2 * 4 + 1], 2);
  TEST_ASSERT_EQUAL_HEX8(0xF8, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8(0x00, bytes[1]);
  TEST_ASSERT_EQUAL_HEX16(0, c.at(0, 0));
}

// 7. Index 0 leaves the canvas as it was.
void test_index_zero_untouched() {
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  const dance::PackGeom g = o.pack.geom();
  uint16_t lut[16];
  lutMarked(lut);
  for (int i = 0; i < o.pack.frameCount(); ++i)
    for (int s = 1; s <= 2; ++s) {
      ctx("pattern under frame %d at scale %d", i, s);
      const dance::PackFrame f = o.pack.frame(i);
      const std::vector<uint8_t> grid = gridOf(o.pack, i);
      Pixels c(g.cellW * s + 6, g.cellH * s + 6, 0);
      for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) c.px[y * c.w + x] = static_cast<uint16_t>(0x8000 | (x * 31 + y * 17));
      const int left = 3 + f.box.x0 * s, top = 3 + f.box.y0 * s;
      dance::blit(c.canvas(), f, left, top, s, false, lut);
      for (int y = 0; y < c.h; ++y)
        for (int x = 0; x < c.w; ++x) {
          const int X = x - 3, Y = y - 3;
          uint8_t k = 0;
          if (X >= 0 && Y >= 0 && X < g.cellW * s && Y < g.cellH * s) k = grid[static_cast<size_t>(Y / s) * g.cellW + X / s];
          const uint16_t pattern = static_cast<uint16_t>(0x8000 | (x * 31 + y * 17));
          TEST_ASSERT_EQUAL_HEX16_MESSAGE(k ? lut[k] : pattern, c.at(x, y), g_ctx);
        }
    }
}

// 8. Nothing drawn: a bad scale, a blank frame, a box wider than 160, a
// null px, an empty canvas; and a stride above the width.
void test_nothing_drawn() {
  Opened o;
  TEST_ASSERT_TRUE(o.open(fixtureBytes()));
  uint16_t lut[16];
  lutMarked(lut);
  const dance::PackFrame body = o.pack.frame(0), blank = o.pack.frame(8);
  TEST_ASSERT_EQUAL_UINT8(0, blank.box.w);
  auto untouched = [](const Pixels& c) {
    for (int i = 0; i < c.w * c.h; ++i)
      if (c.px[i] != 0x1234) return false;
    return true;
  };
  for (int s : {0, 5, -1, 255, 1000}) {
    ctx("scale %d", s);
    Pixels c(64, 64, 0x1234);
    dance::blit(c.canvas(), body, 0, 0, s, false, lut);
    dance::blit(c.canvas(), body, 0, 0, s, true, lut);
    TEST_ASSERT_TRUE_MESSAGE(untouched(c), g_ctx);
  }
  {
    ctx("a blank frame");
    Pixels c(64, 64, 0x1234);
    for (int s = 1; s <= 4; ++s) dance::blit(c.canvas(), blank, 0, 0, s, false, lut);
    TEST_ASSERT_TRUE_MESSAGE(untouched(c), g_ctx);
  }
  // Boxes wider than 160, over rows that would draw if the width were taken.
  for (int w : {161, 200, 255}) {
    ctx("a box %d wide", w);
    Bytes rows;
    for (int y = 0; y < 2; ++y) {
      rows.push_back(0);
      rows.push_back(static_cast<uint8_t>((w + 15) / 16));
      for (int t = 0; t < (w + 15) / 16; ++t) rows.push_back(0xF1);
    }
    HeapCopy data(rows.data(), rows.size());
    dance::PackFrame f;
    f.box = {0, 0, static_cast<uint8_t>(w), 2};
    f.data = data.data();
    f.len = static_cast<uint16_t>(rows.size());
    Pixels c(64, 64, 0x1234);
    dance::blit(c.canvas(), f, 0, 0, 1, false, lut);
    dance::blit(c.canvas(), f, -100, 0, 1, true, lut);
    TEST_ASSERT_TRUE_MESSAGE(untouched(c), g_ctx);
  }
  // A null px.
  dance::BlitCanvas none;
  none.px = nullptr;
  none.w = 64;
  none.h = 64;
  none.stride = 64;
  dance::blit(none, body, 0, 0, 1, false, lut);
  // A canvas of width or height 0.
  {
    Pixels c(64, 64, 0x1234);
    dance::BlitCanvas z = c.canvas();
    z.w = 0;
    dance::blit(z, body, 0, 0, 1, false, lut);
    z = c.canvas();
    z.h = 0;
    dance::blit(z, body, 0, 0, 1, false, lut);
    ctx("an empty canvas");
    TEST_ASSERT_TRUE_MESSAGE(untouched(c), g_ctx);
  }
  // A stride below the width: nothing drawn. A 4x2 frame over a canvas
  // that claims to be 4 px wide with a stride of 2: in a 2x2 block with
  // canary pixels after it, then in an exact-size 2x2 heap block (ASan).
  {
    const Bytes rows = packedit::hex("00 01 31 00 01 31");
    HeapCopy data(rows.data(), rows.size());
    dance::PackFrame f;
    f.box = {0, 0, 4, 2};
    f.data = data.data();
    f.len = static_cast<uint16_t>(rows.size());
    ctx("a stride below the width");
    Pixels canaried(12, 1, 0x1234), block(2, 2, 0x1234);
    for (Pixels* p : {&canaried, &block}) {
      dance::BlitCanvas c = p->canvas();
      c.w = 4;
      c.h = 2;
      c.stride = 2;
      for (int s = 1; s <= 4; ++s)
        for (int m = 0; m < 2; ++m) dance::blit(c, f, 0, 0, s, m != 0, lut);
      TEST_ASSERT_TRUE_MESSAGE(untouched(*p), g_ctx);
    }
  }
  // A stride above the width leaves the columns past it untouched: frame 18
  // fills its whole 24x32 cell, drawn into a 20x20 canvas of stride 30.
  {
    const dance::PackFrame full = o.pack.frame(18);
    TEST_ASSERT_EQUAL_UINT8(24, full.box.w);
    Pixels buf(30, 20, 0x1234);
    dance::BlitCanvas c = buf.canvas();
    c.w = 20;
    c.stride = 30;
    dance::blit(c, full, 0, 0, 1, false, lut);
    const std::vector<uint8_t> grid = gridOf(o.pack, 18);
    for (int y = 0; y < 20; ++y)
      for (int x = 0; x < 30; ++x) {
        ctx("stride 30, width 20: pixel %d,%d", x, y);
        if (x >= 20) {
          TEST_ASSERT_EQUAL_HEX16_MESSAGE(0x1234, buf.at(x, y), g_ctx);
        } else {
          const uint8_t k = grid[static_cast<size_t>(y) * 24 + x];
          TEST_ASSERT_EQUAL_HEX16_MESSAGE(k ? lut[k] : 0x1234, buf.at(x, y), g_ctx);
        }
      }
  }
}

// 9. decodeRow on test_dancepack.py's accepted token sequences and on broken
// rows, each in an exact-size copy, with a canary after the row.
void test_decode_row() {
  struct Case {
    const char* bytes;
    int w;
    bool ok;
    int used;              // bytes a good row consumes
    uint8_t row[16];       // a good row's indices
  };
  const Case cases[] = {
      {"00 02 00 11", 4, true, 4, {0, 1, 1, 0}},  // a leading gap
      {"00 02 11 00", 3, true, 4, {1, 1, 0}},     // a trailing gap
      {"00 02 01 01", 2, true, 4, {1, 1}},        // a run split in two
      {"02 00", 2, true, 2, {0, 0}},              // skip = w, no tokens
      {"00 00", 3, true, 2, {0, 0, 0}},           // nothing in the row
      {"01 01 21", 4, true, 3, {0, 1, 1, 1}},
      {"01 01 12", 5, true, 3, {0, 2, 2, 0, 0}},  // the end past the last token: 0
      {"00 01 F5", 16, true, 3, {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5}},
      {"00 03 13 04 1F", 5, true, 5, {3, 3, 4, 15, 15}},
      {"00 01 11 99", 2, true, 3, {1, 1}},        // bytes after the row aren't its
      // Broken rows: cut where they break, 0 past the cut (row: the indices up to it).
      {"03 00", 2, false, 0, {}},                 // skip > w
      {"00 01 21", 2, false, 0, {}},              // a run past w
      {"01 01 11", 2, false, 0, {}},              // skip + run past w
      {"02 01 11", 3, false, 0, {}},
      {"00 02 11 F1", 4, false, 0, {1, 1}},       // the second token passes w
      {"00 03 11", 8, false, 0, {1, 1}},          // n past the end
      {"00 01", 8, false, 0, {}},
      {"00", 8, false, 0, {}},                    // a row head of one byte
  };
  for (const Case& c : cases) {
    for (int withOk = 0; withOk < 2; ++withOk) {
      ctx("decodeRow %s (w %d)", c.bytes, c.w);
      const Bytes in = packedit::hex(c.bytes);
      HeapCopy data(in.data(), in.size());
      uint8_t* row = new uint8_t[c.w + 8];
      std::memset(row, 0xCC, c.w + 8);
      bool ok = true;
      const uint8_t* end = data.data() + data.size();
      const uint8_t* next = dance::decodeRow(data.data(), end, c.w, row, withOk ? &ok : nullptr);
      for (int k = c.w; k < c.w + 8; ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, row[k], g_ctx);
      // All w indices are written, a broken row's too (D.1), so blit never
      // reads a stale byte as an index.
      TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(c.row, row, c.w, g_ctx);
      if (c.ok) {
        TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
        TEST_ASSERT_TRUE_MESSAGE(next == data.data() + c.used, g_ctx);
      } else {
        if (withOk) TEST_ASSERT_FALSE_MESSAGE(ok, g_ctx);
        TEST_ASSERT_TRUE_MESSAGE(next == end, g_ctx);
      }
      delete[] row;
    }
  }
  // A good row leaves *ok as it was.
  {
    const Bytes in = packedit::hex("00 01 11");
    HeapCopy data(in.data(), in.size());
    uint8_t row[2];
    bool ok = false;
    dance::decodeRow(data.data(), data.data() + 3, 2, row, &ok);
    TEST_ASSERT_FALSE(ok);
  }
  // Rows one after another.
  {
    const Bytes in = packedit::hex("00 01 11 01 01 11");
    HeapCopy data(in.data(), in.size());
    uint8_t row[4];
    const uint8_t* end = data.data() + 6;
    bool ok = true;
    const uint8_t* p = dance::decodeRow(data.data(), end, 4, row, &ok);
    TEST_ASSERT_TRUE(p == data.data() + 3);
    const uint8_t a[4] = {1, 1, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, row, 4);
    p = dance::decodeRow(p, end, 4, row, &ok);
    TEST_ASSERT_TRUE(p == end);
    const uint8_t b[4] = {0, 1, 1, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, row, 4);
    TEST_ASSERT_TRUE(ok);
  }
  // No bytes at all: the row is still written, all 0.
  {
    uint8_t one[1] = {0};
    uint8_t row[4] = {0xCC, 0xCC, 0xCC, 0xCC};
    bool ok = true;
    TEST_ASSERT_TRUE(dance::decodeRow(one + 1, one + 1, 4, row, &ok) == one + 1);
    TEST_ASSERT_FALSE(ok);
    const uint8_t zero[4] = {0, 0, 0, 0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zero, row, 4);
  }
  // The widest row, kBlitRowMax (160) px: ten 16-px runs fill row[0..159]
  // exactly, and an eleventh passes w.
  for (int tokens : {10, 11}) {
    ctx("decodeRow, %d runs of 16 px into 160", tokens);
    Bytes in = {0, static_cast<uint8_t>(tokens)};
    for (int t = 0; t < tokens; ++t) in.push_back(static_cast<uint8_t>(0xF1 + (t < 10 ? t : 0)));
    HeapCopy data(in.data(), in.size());
    uint8_t* row = new uint8_t[160 + 8];
    std::memset(row, 0xCC, 160 + 8);
    bool ok = true;
    const uint8_t* end = data.data() + data.size();
    const uint8_t* next = dance::decodeRow(data.data(), end, 160, row, &ok);
    TEST_ASSERT_EQUAL_MESSAGE(tokens == 10, ok, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(next == end, g_ctx);
    for (int k = 0; k < 160; ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(1 + k / 16, row[k], g_ctx);
    for (int k = 160; k < 168; ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, row[k], g_ctx);
    delete[] row;
  }
  // A w outside 1..160 writes nothing and clears *ok.
  for (int w : {0, 161, 200, -1}) {
    ctx("decodeRow with w %d", w);
    const Bytes in = packedit::hex("00 01 11");
    HeapCopy data(in.data(), in.size());
    uint8_t row[8];
    std::memset(row, 0xCC, sizeof row);
    bool ok = true;
    dance::decodeRow(data.data(), data.data() + 3, w, row, &ok);
    TEST_ASSERT_FALSE_MESSAGE(ok, g_ctx);
    for (uint8_t v : row) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, v, g_ctx);
  }
}

// The widest frame, 160 px (cellW 160 at scale 1). D.3's anchor pack, whose
// one row fills blit's stack row exactly, decodes and draws to dancepack.py's
// frame hash; and a 160x2 frame of ten indices, drawn both ways round at
// scales 1 and 2 and clipped, equals the reference.
void test_full_width() {
  Bytes fdat = packedit::hex("00 0A");
  for (int t = 0; t < 10; ++t) fdat.push_back(0xF1);
  const Bytes pack = packedit::dotWith({{"GEOM", packedit::hex("01 00 a0 04 02 04 00 00 01 00 00 00")},
                                        {"FTAB", packedit::hex("00 03 a0 01 00 00 00 00 0c 00 00 00")},
                                        {"FDAT", fdat}});
  Opened o;
  TEST_ASSERT_TRUE(o.open(pack));
  TEST_ASSERT_EQUAL_UINT8(160, o.pack.frame(0).box.w);
  const std::vector<uint8_t> cell = gridOf(o.pack, 0);
  TEST_ASSERT_EQUAL_HEX32(0x47D06265u, packedit::refFnv1a32(cell.data(), cell.size()));
  uint16_t lut[16];
  lutIdentity(lut);
  {
    Pixels c(160, 4, 0);
    dance::blit(c.canvas(), o.pack.frame(0), 0, 3, 1, false, lut);
    std::vector<uint8_t> low(160 * 4);
    for (size_t k = 0; k < low.size(); ++k) low[k] = static_cast<uint8_t>(c.px[k]);
    TEST_ASSERT_EQUAL_HEX32(0x47D06265u, packedit::refFnv1a32(low.data(), low.size()));
  }
  // Rows: indices 1-10, 16 px each; then nine runs of index 5 and one of 15 px of 7.
  Bytes rows = {0, 10};
  for (int t = 0; t < 10; ++t) rows.push_back(static_cast<uint8_t>(0xF1 + t));
  rows.push_back(0);
  rows.push_back(10);
  for (int t = 0; t < 9; ++t) rows.push_back(0xF5);
  rows.push_back(0xE7);
  HeapCopy data(rows.data(), rows.size());
  dance::PackFrame f;
  f.box = {0, 0, 160, 2};
  f.data = data.data();
  f.len = static_cast<uint16_t>(rows.size());
  std::vector<uint8_t> fill(320, 0xEE);
  HeapCopy gridBuf(fill.data(), fill.size());
  TEST_ASSERT_TRUE(dance::decodeGrid(f, 160, 2, gridBuf.data()));
  const std::vector<uint8_t> grid(gridBuf.data(), gridBuf.data() + 320);
  TEST_ASSERT_EQUAL_UINT8(10, grid[159]);
  TEST_ASSERT_EQUAL_UINT8(0, grid[319]);
  lutMarked(lut);
  CanaryCanvas c(170, 9, 4), ref(170, 9, 4);
  for (int s = 1; s <= 2; ++s)
    for (int m = 0; m < 2; ++m)
      for (int left : {0, 10, -7, -160}) {
        ctx("160 px at scale %d%s, left %d", s, m ? " mirrored" : "", left);
        c.reset(0x1234);
        ref.reset(0x1234);
        dance::blit(c.canvas(), f, left, 1, s, m != 0, lut);
        refBlit(&ref.at(0, 0), ref.w, ref.h, ref.stride, grid, 160, f.box, left, 1, s, m != 0, lut);
        TEST_ASSERT_TRUE_MESSAGE(c.borderIntact(), g_ctx);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ref.buf, c.buf, c.n * sizeof(uint16_t), g_ctx);
      }
}

// decodeGrid: A.10's drawing into a zeroed cell, and false (with a zero grid)
// wherever open()'s dry run would fail.
void test_decode_grid() {
  struct Case {
    uint8_t x0, y0, w, h;
    const char* bytes;
    int cellW, cellH;
    bool ok;
    bool zero;  // D.1 promises a zero grid: a blank frame, or a box that doesn't fit
  };
  const Case cases[] = {
      {1, 2, 2, 2, "00 01 11 00 01 11", 4, 4, true, false},   // the dot
      {0, 0, 0, 0, "", 4, 4, true, true},                     // a blank frame
      {0, 0, 0, 0, "00 00 00", 4, 4, false, true},            // blank, but with bytes
      {3, 0, 2, 1, "00 01 11", 4, 4, false, true},            // x0 + w past cellW
      {0, 3, 2, 2, "00 01 11 00 01 11", 4, 4, false, true},   // y0 + h past cellH
      {0, 0, 161, 1, "00 0B FF FF FF FF FF FF FF FF FF FF 01", 200, 1, false, true},  // w above 160
      {0, 0, 2, 1, "00 01 11 00", 4, 4, false, false},        // a byte left over
      {0, 0, 2, 2, "00 01 11", 4, 4, false, false},           // a row missing
      {0, 0, 2, 1, "00 01 21", 4, 4, false, false},           // a run past w
      {0, 0, 4, 1, "04 00", 4, 1, true, true},                // skip = w
  };
  for (const Case& c : cases) {
    ctx("decodeGrid %u,%u %ux%u \"%s\" in %dx%d", c.x0, c.y0, c.w, c.h, c.bytes, c.cellW, c.cellH);
    const Bytes in = packedit::hex(c.bytes);
    HeapCopy data(in.data(), in.size());
    dance::PackFrame f;
    f.box = {c.x0, c.y0, c.w, c.h};
    f.data = data.data();
    f.len = static_cast<uint16_t>(in.size());
    const size_t n = static_cast<size_t>(c.cellW) * c.cellH;
    uint8_t* grid = new uint8_t[n];
    std::memset(grid, 0xCC, n);
    TEST_ASSERT_EQUAL_MESSAGE(c.ok, dance::decodeGrid(f, c.cellW, c.cellH, grid), g_ctx);
    if (c.zero)
      for (size_t k = 0; k < n; ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, grid[k], g_ctx);
    for (size_t k = 0; k < n; ++k) TEST_ASSERT_TRUE_MESSAGE(grid[k] < 16, g_ctx);  // every byte written
    delete[] grid;
  }
  // The dot, exactly.
  const Bytes in = packedit::hex("00 01 11 00 01 11");
  HeapCopy data(in.data(), in.size());
  dance::PackFrame f;
  f.box = {1, 2, 2, 2};
  f.data = data.data();
  f.len = 6;
  uint8_t grid[16];
  TEST_ASSERT_TRUE(dance::decodeGrid(f, 4, 4, grid));
  const uint8_t want[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, grid, 16);
}

// 10. Garbage: hand-built frames over seeded random bytes. blit writes only
// inside the box's rect and the canvas; decodeRow and decodeGrid stay inside
// their buffers; and where decodeGrid accepts the bytes, blit draws exactly
// the reference.
void test_garbage() {
  Xorshift32 rng(0x47415242u);
  uint16_t lut[16];
  lutMarked(lut);
  CanaryCanvas c(40, 50, 4), ref(40, 50, 4);
  int exact = 0;
  for (int it = 0; it < 4000; ++it) {
    const int w0 = rng.below(5) == 0 ? 161 + static_cast<int>(rng.below(95)) : static_cast<int>(rng.below(41));
    const int w = w0 == 40 ? 160 : w0;  // kBlitRowMax too: the stack row filled
    const int h = static_cast<int>(rng.below(24));
    Bytes bytes;
    if (rng.below(2)) {
      const size_t n = rng.below(300);
      for (size_t k = 0; k < n; ++k) bytes.push_back(static_cast<uint8_t>(rng.next()));
    } else {  // rows that follow A.10, then sometimes a byte spoiled or the end cut
      for (int y = 0; y < h; ++y) {
        const int skip = w ? static_cast<int>(rng.below(static_cast<uint32_t>(w + 1))) : 0;
        Bytes toks;
        int room = w - skip;
        while (room > 0 && toks.size() < 6 && rng.below(4)) {
          const int run = 1 + static_cast<int>(rng.below(static_cast<uint32_t>(room < 16 ? room : 16)));
          toks.push_back(static_cast<uint8_t>((run - 1) << 4 | rng.below(16)));
          room -= run;
        }
        bytes.push_back(static_cast<uint8_t>(skip));
        bytes.push_back(static_cast<uint8_t>(toks.size()));
        bytes.insert(bytes.end(), toks.begin(), toks.end());
      }
      if (!bytes.empty() && rng.below(3) == 0)
        bytes[rng.below(static_cast<uint32_t>(bytes.size()))] ^= static_cast<uint8_t>(1 + rng.below(255));
      if (!bytes.empty() && rng.below(6) == 0) bytes.resize(rng.below(static_cast<uint32_t>(bytes.size())));
    }
    HeapCopy data(bytes.data(), bytes.size());
    dance::PackFrame f;
    f.box = {0, 0, static_cast<uint8_t>(w), static_cast<uint8_t>(h)};
    f.data = data.data();
    f.len = static_cast<uint16_t>(bytes.size());
    const int s = static_cast<int>(rng.below(6));
    const bool mirrored = rng.below(2) != 0;
    const int bw = w * (s ? s : 1), bh = h * (s ? s : 1);
    const int left = -bw - 3 + static_cast<int>(rng.below(static_cast<uint32_t>(bw + 46)));
    const int top = -bh - 3 + static_cast<int>(rng.below(static_cast<uint32_t>(bh + 56)));
    ctx("garbage %d: %dx%d, %u bytes, scale %d%s at %d,%d", it, w, h, static_cast<unsigned>(bytes.size()), s,
        mirrored ? " mirrored" : "", left, top);
    c.reset(0x1234);
    dance::blit(c.canvas(), f, left, top, s, mirrored, lut);
    TEST_ASSERT_TRUE_MESSAGE(c.borderIntact(), g_ctx);
    for (int y = 0; y < c.h; ++y)
      for (int x = 0; x < c.w; ++x)
        if (c.at(x, y) != 0x1234) {
          TEST_ASSERT_TRUE_MESSAGE(s >= 1 && s <= 4 && w <= 160, g_ctx);
          TEST_ASSERT_TRUE_MESSAGE(x >= left && x < left + bw && y >= top && y < top + bh, g_ctx);
        }
    // decodeGrid, in a cell the box fits (or doesn't).
    const int cellW = 1 + static_cast<int>(rng.below(200)), cellH = 1 + static_cast<int>(rng.below(30));
    const size_t n = static_cast<size_t>(cellW) * cellH;
    uint8_t* grid = new uint8_t[n];
    const bool fits = dance::decodeGrid(f, cellW, cellH, grid);
    if (fits && w > 0) TEST_ASSERT_TRUE_MESSAGE(w <= cellW && h <= cellH && w <= 160, g_ctx);
    delete[] grid;
    // Where the bytes pass the dry run, blit is exact.
    if (w >= 1 && w <= 160 && s >= 1 && s <= 4) {
      std::vector<uint8_t> cell(static_cast<size_t>(w) * (h ? h : 1));
      HeapCopy g2(cell.data(), cell.size());
      if (h > 0 && dance::decodeGrid(f, w, h, g2.data())) {
        ++exact;
        const std::vector<uint8_t> gv(g2.data(), g2.data() + g2.size());
        ref.reset(0x1234);
        refBlit(&ref.at(0, 0), ref.w, ref.h, ref.stride, gv, w, f.box, left, top, s, mirrored, lut);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(ref.buf, c.buf, c.n * sizeof(uint16_t), g_ctx);
      }
    }
    // decodeRow at a random start, into a row with a canary after it.
    if (!bytes.empty()) {
      const int rw = 1 + static_cast<int>(rng.below(160));
      uint8_t* row = new uint8_t[rw + 4];
      std::memset(row + rw, 0xCC, 4);
      const uint8_t* p = data.data() + rng.below(static_cast<uint32_t>(bytes.size()));
      const uint8_t* end = data.data() + bytes.size();
      bool ok = true;
      const uint8_t* next = dance::decodeRow(p, end, rw, row, &ok);
      TEST_ASSERT_TRUE_MESSAGE(next > p && next <= end, g_ctx);
      for (int k = 0; k < 4; ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xCC, row[rw + k], g_ctx);
      delete[] row;
    }
  }
  TEST_ASSERT_TRUE(exact > 300);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_frame_hashes);
  RUN_TEST(test_mirroring);
  RUN_TEST(test_scales);
  RUN_TEST(test_clipping_canary);
  RUN_TEST(test_byte_swap);
  RUN_TEST(test_index_zero_untouched);
  RUN_TEST(test_nothing_drawn);
  RUN_TEST(test_decode_row);
  RUN_TEST(test_full_width);
  RUN_TEST(test_decode_grid);
  RUN_TEST(test_garbage);
  return UNITY_END();
}
