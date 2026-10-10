// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the boot screen's portable pieces: the logo's runs
// (RleImage reading LogoArt, against the counts and the digest
// tools/make_logo.py wrote beside them), the palette's blends, and the
// screen's layout (BootLayout). The texts' widths are test_ui_library's.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "BootLayout.h"
#include "LogoArt.h"
#include "RleImage.h"

using rleimage::code;
using rleimage::levelOf;
using rleimage::partOf;

void setUp() {}
void tearDown() {}

namespace {
uint16_t rgb565(uint32_t c) {
  const uint32_t r = c >> 16 & 0xFF, g = c >> 8 & 0xFF, b = c & 0xFF;
  return static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3));
}

// FNV-1a (32-bit) over pixel codes, as tools/make_logo.py's digest().
struct Fnv {
  uint32_t h = 0x811C9DC5u;
  void add(uint8_t c) { h = (h ^ c) * 0x01000193u; }
};

// The whole logo read, kW codes a row, top row first.
std::vector<uint8_t> readLogo() {
  rleimage::Reader r(logo::kData, logo::kSize, logo::kW);
  std::vector<uint8_t> px(static_cast<size_t>(logo::kW) * logo::kH);
  for (int y = 0; y < logo::kH; ++y) TEST_ASSERT_TRUE(r.row(&px[static_cast<size_t>(y) * logo::kW]));
  TEST_ASSERT_TRUE(r.done());
  return px;
}
}  // namespace

// ---- the logo's runs ----

// Every row reads whole, the data ends with the last one, and each part's
// pixels and coverage are what the tool counted when it wrote them.
void test_logo_reads_whole() {
  rleimage::Reader r(logo::kData, logo::kSize, logo::kW);
  std::vector<uint8_t> row(logo::kW);
  uint32_t pixels[logo::kParts + 1] = {}, coverage[logo::kParts + 1] = {};
  for (int y = 0; y < logo::kH; ++y) {
    TEST_ASSERT_TRUE(r.row(row.data()));
    for (const uint8_t c : row) {
      const int p = partOf(c);
      TEST_ASSERT_TRUE(p <= logo::kParts);
      if (!p) {
        TEST_ASSERT_EQUAL_UINT8(0, c);
        continue;
      }
      TEST_ASSERT_TRUE(levelOf(c) >= 1 && levelOf(c) <= rleimage::kLevels);
      ++pixels[p];
      coverage[p] += static_cast<uint32_t>(levelOf(c));
    }
  }
  TEST_ASSERT_TRUE(r.done());
  for (int p = 1; p <= logo::kParts; ++p) {
    TEST_ASSERT_TRUE(pixels[p] > 0);
    TEST_ASSERT_EQUAL_UINT32(logo::kPixels[p], pixels[p]);
    TEST_ASSERT_EQUAL_UINT32(logo::kCoverage[p], coverage[p]);
  }
  // Past the last row: nothing more.
  TEST_ASSERT_FALSE(r.row(row.data()));
  TEST_ASSERT_EQUAL_INT(rleimage::kLevels, logo::kLevels);
  TEST_ASSERT_EQUAL_INT(rleimage::kParts, logo::kParts);
}

// Every pixel where the tool put it: the codes, top row first and each row
// left to right, hash to the digest it wrote. (The counts above don't see
// a pixel out of place.)
void test_logo_pixels_in_place() {
  const std::vector<uint8_t> px = readLogo();
  Fnv inOrder;
  for (const uint8_t c : px) inOrder.add(c);
  TEST_ASSERT_EQUAL_HEX32(logo::kDigest, inOrder.h);
  // And the digest has teeth for this logo: the rows bottom up, or each
  // row mirrored (what a reader filling runs from the wrong end would draw;
  // the counts and the empty border are the same), hash to something else.
  Fnv upsideDown, mirrored;
  for (int y = logo::kH - 1; y >= 0; --y) {
    for (int x = 0; x < logo::kW; ++x) upsideDown.add(px[static_cast<size_t>(y) * logo::kW + x]);
  }
  for (int y = 0; y < logo::kH; ++y) {
    for (int x = logo::kW - 1; x >= 0; --x) mirrored.add(px[static_cast<size_t>(y) * logo::kW + x]);
  }
  TEST_ASSERT_NOT_EQUAL(logo::kDigest, upsideDown.h);
  TEST_ASSERT_NOT_EQUAL(logo::kDigest, mirrored.h);
  // And by the logo's own shape, not the tool's numbers: the "m" (the bars,
  // parts 2 and 3) comes before the word (part 1), every pixel of it left
  // of every pixel of the word; and its middle bar's notched top is lower
  // than the outer bars' tops (all three end on one line), so the logo is
  // the right way up.
  int mRight = -1, wordLeft = logo::kW, outerTop = logo::kH, middleTop = logo::kH;
  for (int y = 0; y < logo::kH; ++y) {
    for (int x = 0; x < logo::kW; ++x) {
      const int p = partOf(px[static_cast<size_t>(y) * logo::kW + x]);
      if (p == 1 && x < wordLeft) wordLeft = x;
      if ((p == 2 || p == 3) && x > mRight) mRight = x;
      if (p == 2 && y < outerTop) outerTop = y;
      if (p == 3 && y < middleTop) middleTop = y;
    }
  }
  TEST_ASSERT_TRUE(mRight >= 0 && mRight < wordLeft);
  TEST_ASSERT_TRUE(mRight < logo::kW / 4);
  TEST_ASSERT_TRUE(outerTop < logo::kH && middleTop < logo::kH);
  TEST_ASSERT_TRUE(middleTop > outerTop + 2);
}

// The ink spans the width less a pixel each side (its edges' room): the
// outer columns are empty, the next ones are not; the same top and bottom.
void test_logo_fills_its_box() {
  rleimage::Reader r(logo::kData, logo::kSize, logo::kW);
  std::vector<uint8_t> row(logo::kW);
  bool inkAt1 = false, inkAtW2 = false, inkTop = false, inkBottom = false;
  for (int y = 0; y < logo::kH; ++y) {
    TEST_ASSERT_TRUE(r.row(row.data()));
    TEST_ASSERT_EQUAL_UINT8(0, row[0]);
    TEST_ASSERT_EQUAL_UINT8(0, row[logo::kW - 1]);
    inkAt1 = inkAt1 || row[1];
    inkAtW2 = inkAtW2 || row[logo::kW - 2];
    bool any = false;
    for (const uint8_t c : row) any = any || c;
    if (y == 0 || y == logo::kH - 1) TEST_ASSERT_FALSE(any);
    if (y == 1) inkTop = any;
    if (y == logo::kH - 2) inkBottom = any;
  }
  TEST_ASSERT_TRUE(inkAt1 && inkAtW2 && inkTop && inkBottom);
}

// The runs are small (the bar: well under the 24 KB a raw RGB565 copy of
// a logo this size would be).
void test_logo_is_small() {
  TEST_ASSERT_TRUE(logo::kW >= 220 && logo::kW <= 260);
  TEST_ASSERT_TRUE(logo::kSize < 6 * 1024);
  TEST_ASSERT_TRUE(logo::kSize * 4 < static_cast<uint32_t>(logo::kW * logo::kH * 2));
}

// A run past the row's end, or data that ends early, is refused: the row
// (and every later one) reads as nothing.
void test_reader_refuses_bad_runs() {
  uint8_t out[4];
  {
    // part 1 x2, part 2 at 16/32, one of nothing.
    const uint8_t data[] = {0x41, 0x80 | 0x20 | 15, 0x00};
    rleimage::Reader r(data, sizeof(data), 4);
    TEST_ASSERT_TRUE(r.row(out));
    TEST_ASSERT_EQUAL_UINT8(code(1, 32), out[0]);
    TEST_ASSERT_EQUAL_UINT8(code(1, 32), out[1]);
    TEST_ASSERT_EQUAL_UINT8(code(2, 16), out[2]);
    TEST_ASSERT_EQUAL_UINT8(0, out[3]);
    TEST_ASSERT_TRUE(r.done());
  }
  {
    const uint8_t data[] = {0x05};  // 6 of nothing in a 4 px row
    rleimage::Reader r(data, sizeof(data), 4);
    TEST_ASSERT_FALSE(r.row(out));
    for (const uint8_t c : out) TEST_ASSERT_EQUAL_UINT8(0, c);
    TEST_ASSERT_FALSE(r.done());
  }
  {
    const uint8_t data[] = {0xC1, 0x01};  // part 3 x2, then 2 of nothing; then nothing for row 2
    rleimage::Reader r(data, sizeof(data), 4);
    TEST_ASSERT_TRUE(r.row(out));
    TEST_ASSERT_EQUAL_UINT8(code(3, 32), out[1]);
    TEST_ASSERT_FALSE(r.row(out));
    TEST_ASSERT_EQUAL_UINT8(0, out[0]);
    TEST_ASSERT_FALSE(r.row(out));  // and stays refused
    TEST_ASSERT_FALSE(r.done());
  }
}

// ---- the colours ----

void test_blend_ends_and_order() {
  using namespace bootlayout;
  // Nothing is the background, full is the colour, as the theme has them
  // (col::BG 0x0862, col::TXT 0xF79F).
  TEST_ASSERT_EQUAL_HEX16(0x0862, rleimage::blend565(kLogoPart1, kLogoBg, 0));
  TEST_ASSERT_EQUAL_HEX16(0xF79F, rleimage::blend565(kLogoPart1, kLogoBg, rleimage::kLevels));
  for (int p = 1; p <= logo::kParts; ++p) {
    TEST_ASSERT_EQUAL_HEX16(rgb565(kLogoBg), rleimage::blend565(kLogoParts[p], kLogoBg, 0));
    TEST_ASSERT_EQUAL_HEX16(rgb565(kLogoParts[p]), rleimage::blend565(kLogoParts[p], kLogoBg, rleimage::kLevels));
    // Every part is lighter than the background: each channel only grows
    // with the coverage.
    uint16_t last = rleimage::blend565(kLogoParts[p], kLogoBg, 0);
    for (int l = 1; l <= rleimage::kLevels; ++l) {
      const uint16_t c = rleimage::blend565(kLogoParts[p], kLogoBg, l);
      TEST_ASSERT_TRUE((c >> 11) >= (last >> 11));
      TEST_ASSERT_TRUE((c >> 5 & 0x3F) >= (last >> 5 & 0x3F));
      TEST_ASSERT_TRUE((c & 0x1F) >= (last & 0x1F));
      last = c;
    }
  }
  // Out of range: clamped.
  TEST_ASSERT_EQUAL_HEX16(rgb565(0xFFFFFF), rleimage::blend565(0xFFFFFF, 0, 99));
  TEST_ASSERT_EQUAL_HEX16(0, rleimage::blend565(0xFFFFFF, 0, -3));
}

// The palette maps a code to its blend, byte-swapped for the panel when
// asked; nothing is the background.
void test_palette() {
  using namespace bootlayout;
  rleimage::Palette plain, swapped;
  plain.set(kLogoParts, kLogoBg, false);
  swapped.set(kLogoParts, kLogoBg, true);
  TEST_ASSERT_EQUAL_HEX16(0x0862, plain(0));
  TEST_ASSERT_EQUAL_HEX16(0x6208, swapped(0));
  for (int p = 1; p <= logo::kParts; ++p) {
    for (int l = 1; l <= rleimage::kLevels; ++l) {
      const uint16_t c = rleimage::blend565(kLogoParts[p], kLogoBg, l);
      TEST_ASSERT_EQUAL_HEX16(c, plain(code(p, l)));
      TEST_ASSERT_EQUAL_HEX16(static_cast<uint16_t>(c >> 8 | c << 8), swapped(code(p, l)));
    }
  }
}

// ---- the layout ----

void test_layout_fits_the_screen() {
  using namespace bootlayout;
  // The logo: centred, inside the screen.
  TEST_ASSERT_TRUE(kLogoX >= 0 && kLogoX + logo::kW <= kW);
  TEST_ASSERT_TRUE(kLogoX * 2 + logo::kW == kW || kLogoX * 2 + logo::kW == kW - 1);
  TEST_ASSERT_TRUE(kLogoY >= 0);
  // The version under it, kGap apart, and well above the rescue line.
  const int versionTop = kVersionY - kBodyH / 2, versionBottom = versionTop + kBodyH;
  TEST_ASSERT_TRUE(versionTop >= kLogoY + logo::kH + kGap);
  const int hintTop = kHintY - kSmallH / 2;
  TEST_ASSERT_TRUE(versionBottom + 30 <= hintTop);
  TEST_ASSERT_TRUE(hintTop + kSmallH <= kH);
  // The logo and the version together: at or a little above the middle.
  const int middle = (kLogoY + versionBottom) / 2;
  TEST_ASSERT_TRUE(middle <= kH / 2 && middle >= kH / 2 - 12);
  TEST_ASSERT_TRUE(kTextW <= kW - 16);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_logo_reads_whole);
  RUN_TEST(test_logo_pixels_in_place);
  RUN_TEST(test_logo_fills_its_box);
  RUN_TEST(test_logo_is_small);
  RUN_TEST(test_reader_refuses_bad_runs);
  RUN_TEST(test_blend_ends_and_order);
  RUN_TEST(test_palette);
  RUN_TEST(test_layout_fits_the_screen);
  return UNITY_END();
}
