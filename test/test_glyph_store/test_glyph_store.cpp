// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the glyph store (GlyphStore, docs/I18N.md phase 2): the
// reader on tiny stores made from hand-drawn glyphs (fixture_store.h,
// tools/glyph_store.py --fixture), with runs and raw, against the glyphs
// as drawn; stores it must refuse; and the built-in 16 px CJK store
// (CjkGlyphs16): every glyph against the generator's digest, the set it
// holds, and nothing the VLW DejaVu fonts draw.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "CjkGlyphs16.h"
#include "GlyphStore.h"
#include "TextFold.h"
#include "fixture_store.h"

// The VLW fonts' data, as the firmware has it (src/ui/VlwFonts.cpp: arrays only).
#include "../../src/ui/VlwFonts.cpp"

using glyphstore::Alpha;
using glyphstore::Glyph;
using glyphstore::Metrics;
using glyphstore::Store;

void setUp() {}
void tearDown() {}

namespace {

// FNV-1a 64, as tools/glyph_store.py's digest().
struct Fnv {
  uint64_t h = 0xCBF29CE484222325ull;
  void add(uint8_t b) { h = (h ^ b) * 0x100000001B3ull; }
  void add32(uint32_t v) {
    for (int i = 0; i < 4; ++i) add(static_cast<uint8_t>(v >> (8 * i)));
  }
};

// One glyph into the digest: its code point, metrics and values (0-15).
void digestGlyph(Fnv& f, const Store& s, uint32_t cp, const Glyph& g) {
  const Metrics m = s.metrics(g);
  f.add32(cp);
  f.add(m.w);
  f.add(m.h);
  f.add(static_cast<uint8_t>(m.x));
  f.add(static_cast<uint8_t>(m.y));
  f.add(m.advance);
  std::vector<uint8_t> px(static_cast<size_t>(m.w) * m.h);
  TEST_ASSERT_TRUE(s.decode(g, px.data(), m.w, px.size(), Alpha::Nibble));
  for (const uint8_t v : px) f.add(v);
}

int hexValue(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

const Store& builtin() {
  static Store s;
  static bool opened = s.open(kCjkGlyphs16, kCjkGlyphs16Size);
  TEST_ASSERT_TRUE(opened);
  return s;
}

std::vector<uint8_t> bytesOf(const uint32_t* words, size_t size) {
  std::vector<uint8_t> b(size);
  std::memcpy(b.data(), words, size);
  return b;
}

// A section's place in a store's bytes (its directory entry's offset and size).
struct Section {
  size_t entry = 0, at = 0, size = 0;
};
Section sectionOf(const std::vector<uint8_t>& b, const char* tag) {
  Section s;
  const size_t n = b[6] | b[7] << 8;
  for (size_t i = 0; i < n; ++i) {
    const size_t e = 32 + 12 * i;
    if (std::memcmp(&b[e], tag, 4) != 0) continue;
    s.entry = e;
    s.at = b[e + 4] | b[e + 5] << 8 | b[e + 6] << 16 | static_cast<size_t>(b[e + 7]) << 24;
    s.size = b[e + 8] | b[e + 9] << 8 | b[e + 10] << 16 | static_cast<size_t>(b[e + 11]) << 24;
  }
  return s;
}

// Where a glyph's bitmap starts in BITS: its page's start plus its offset.
size_t bitmapOf(const std::vector<uint8_t>& b, const Glyph& g) {
  auto le = [&](size_t at, int n) {
    size_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = v << 8 | b[at + i];
    return v;
  };
  return le(sectionOf(b, "PMAP").at + 48 * g.page + 36, 4) + le(sectionOf(b, "OFFS").at + 2 * g.index, 2);
}

// The code points of the four VLW fonts (big-endian: a 24 B header whose
// first field is the glyph count, then 28 B of metrics a glyph, the code
// point first).
std::vector<uint32_t> vlwCodePoints() {
  const uint8_t* fonts[] = {kVlwSans16, kVlwSans13, kVlwSansBold16, kVlwSansBold22};
  auto be32 = [](const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | p[3];
  };
  std::vector<uint32_t> cps;
  for (const uint8_t* f : fonts) {
    const uint32_t n = be32(f);
    for (uint32_t i = 0; i < n; ++i) cps.push_back(be32(f + 24 + 28 * i));
  }
  return cps;
}

}  // namespace

// ---- the fixture: the reader against hand-drawn glyphs ----

void test_fixture_header() {
  for (int raw = 0; raw < 2; ++raw) {
    Store s;
    TEST_ASSERT_TRUE(raw ? s.open(fixture::kRaw, fixture::kRawSize) : s.open(fixture::kRuns, fixture::kRunsSize));
    TEST_ASSERT_TRUE(s.valid());
    TEST_ASSERT_EQUAL_INT(fixture::kSize, s.pixelSize());
    TEST_ASSERT_EQUAL_INT(fixture::kAscent, s.ascent());
    TEST_ASSERT_EQUAL_INT(fixture::kDescent, s.descent());
    TEST_ASSERT_EQUAL_UINT32(fixture::kCount, s.glyphCount());
    // From the glyphs: U+4E01's top 14 above; U+3100 and U+4E00 end on the
    // baseline; U+20000 is the widest, U+4E00 the tallest.
    TEST_ASSERT_EQUAL_INT(14, s.inkAscent());
    TEST_ASSERT_EQUAL_INT(0, s.inkDescent());
    TEST_ASSERT_EQUAL_INT(19, s.maxWidth());
    TEST_ASSERT_EQUAL_INT(7, s.maxHeight());
  }
}

// Every glyph is found, in code point order, with its metrics and its
// values as drawn, 0-15 and 0-255; raw and runs alike.
void test_fixture_glyphs_as_drawn() {
  for (int raw = 0; raw < 2; ++raw) {
    Store s;
    TEST_ASSERT_TRUE(raw ? s.open(fixture::kRaw, fixture::kRawSize) : s.open(fixture::kRuns, fixture::kRunsSize));
    uint32_t next = 0;
    for (const fixture::Drawn& d : fixture::kGlyphs) {
      const Glyph g = s.find(d.cp);
      TEST_ASSERT_TRUE_MESSAGE(static_cast<bool>(g), "found");
      TEST_ASSERT_TRUE(s.contains(d.cp));
      TEST_ASSERT_EQUAL_UINT32(next++, g.index);
      const Metrics m = s.metrics(g);
      TEST_ASSERT_EQUAL_INT(d.w, m.w);
      TEST_ASSERT_EQUAL_INT(d.h, m.h);
      TEST_ASSERT_EQUAL_INT(d.x, m.x);
      TEST_ASSERT_EQUAL_INT(d.y, m.y);
      TEST_ASSERT_EQUAL_INT(d.advance, m.advance);
      TEST_ASSERT_EQUAL_INT(d.advance, s.advance(d.cp));
      const size_t n = static_cast<size_t>(d.w) * d.h;
      TEST_ASSERT_EQUAL_size_t(n, strlen(d.rows));
      std::vector<uint8_t> nib(n + 1, 0xAA), byte(n + 1, 0xAA);
      TEST_ASSERT_TRUE(s.decode(g, nib.data(), d.w, n, Alpha::Nibble));
      TEST_ASSERT_TRUE(s.decode(g, byte.data(), d.w, n, Alpha::Byte));
      for (size_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_UINT8(hexValue(d.rows[i]), nib[i]);
        TEST_ASSERT_EQUAL_UINT8(hexValue(d.rows[i]) * 17, byte[i]);
      }
      TEST_ASSERT_EQUAL_HEX8(0xAA, nib[n]);  // nothing past the box
    }
    Fnv f;
    for (const fixture::Drawn& d : fixture::kGlyphs) digestGlyph(f, s, d.cp, s.find(d.cp));
    TEST_ASSERT_TRUE(f.h == fixture::kDigest);
  }
}

// Absent: the glyphs' neighbours, pages the store doesn't have, code
// points past Unicode.
void test_fixture_absent() {
  Store s;
  TEST_ASSERT_TRUE(s.open(fixture::kRuns, fixture::kRunsSize));
  const uint32_t absent[] = {0,       0x41,    0x2FFF,  0x3001,  0x3041,  0x3043,   0x30FE,   0x3101,
                             0x31FF,  0x3200,  0x4DFF,  0x4E02,  0x4EFF,  0x1F5FF,  0x1F601,  0x1F700,
                             0x1FFFF, 0x20001, 0x10FFFF, 0x110000, 0x7FFFFFFF, 0xFFFFFFFFu};
  for (const uint32_t cp : absent) {
    const Glyph g = s.find(cp);
    TEST_ASSERT_FALSE(static_cast<bool>(g));
    TEST_ASSERT_FALSE(s.contains(cp));
    TEST_ASSERT_EQUAL_INT(-1, s.advance(cp));
    const Metrics m = s.metrics(g);
    TEST_ASSERT_EQUAL_INT(0, m.w + m.h + m.x + m.y + m.advance);
    uint8_t buf[4] = {1, 2, 3, 4};
    TEST_ASSERT_FALSE(s.decode(g, buf, 2, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT8(1, buf[0]);
  }
}

// U+30FF ends page 0x30 and U+3100 starts page 0x31: next to each other,
// each its own glyph. U+1F600 and U+20000 lie past the BMP.
void test_fixture_page_edges_and_planes() {
  Store s;
  TEST_ASSERT_TRUE(s.open(fixture::kRuns, fixture::kRunsSize));
  const Glyph a = s.find(0x30FF), b = s.find(0x3100);
  TEST_ASSERT_TRUE(a && b);
  TEST_ASSERT_EQUAL_UINT32(a.index + 1, b.index);
  TEST_ASSERT_EQUAL_UINT32(a.page + 1, b.page);
  TEST_ASSERT_EQUAL_INT(15, s.metrics(a).w);
  TEST_ASSERT_EQUAL_INT(-2, s.metrics(b).x);
  TEST_ASSERT_EQUAL_INT(0, s.metrics(b).advance);
  TEST_ASSERT_TRUE(s.contains(0x1F600));
  TEST_ASSERT_TRUE(s.contains(0x20000));
  TEST_ASSERT_EQUAL_INT(12, s.advance(0x20000));
}

// A box into rows `stride` apart leaves the bytes between them; a buffer
// too small is refused untouched; a 0 x 0 glyph writes nothing.
void test_decode_buffers() {
  Store s;
  TEST_ASSERT_TRUE(s.open(fixture::kRuns, fixture::kRunsSize));
  const Glyph g = s.find(0x3042);  // 5 x 5
  uint8_t buf[5 * 8];
  std::memset(buf, 0xEE, sizeof(buf));
  TEST_ASSERT_TRUE(s.decode(g, buf, 8, 4 * 8 + 5, Alpha::Nibble));
  const char* rows = "00f00fffff07f80a0f0a0f9f0";
  for (int r = 0; r < 5; ++r) {
    for (int c = 0; c < 8; ++c) {
      if (c < 5) {
        TEST_ASSERT_EQUAL_UINT8(hexValue(rows[r * 5 + c]), buf[r * 8 + c]);
      } else {
        TEST_ASSERT_EQUAL_HEX8(0xEE, buf[r * 8 + c]);
      }
    }
  }
  std::memset(buf, 0xEE, sizeof(buf));
  TEST_ASSERT_FALSE(s.decode(g, buf, 8, 4 * 8 + 4));  // one byte short
  TEST_ASSERT_FALSE(s.decode(g, buf, 4, sizeof(buf)));  // stride under the width
  TEST_ASSERT_FALSE(s.decode(g, nullptr, 5, 25));
  for (const uint8_t b : buf) TEST_ASSERT_EQUAL_HEX8(0xEE, b);
  const Glyph space = s.find(0x3000);
  TEST_ASSERT_TRUE(static_cast<bool>(space));
  TEST_ASSERT_EQUAL_INT(16, s.metrics(space).advance);
  TEST_ASSERT_TRUE(s.decode(space, buf, 1, 0));
  TEST_ASSERT_EQUAL_HEX8(0xEE, buf[0]);
}

// What isn't a store this reader knows is refused, and the Store is empty.
void test_open_refuses() {
  const std::vector<uint8_t> good = bytesOf(fixture::kRuns, fixture::kRunsSize);
  Store s;
  TEST_ASSERT_TRUE(s.open(good.data(), good.size()));
  auto refused = [&](std::vector<uint8_t> b, size_t size, const char* what) {
    Store t;
    TEST_ASSERT_TRUE(t.open(good.data(), good.size()));
    TEST_ASSERT_FALSE_MESSAGE(t.open(b.data(), size), what);
    TEST_ASSERT_FALSE(t.valid());
    TEST_ASSERT_FALSE(t.contains(0x3042));
    TEST_ASSERT_EQUAL_UINT32(0, t.glyphCount());
  };
  TEST_ASSERT_FALSE(s.open(nullptr, 100));
  TEST_ASSERT_FALSE(s.valid());
  refused(good, 31, "shorter than a header");
  refused(good, good.size() - 4, "shorter than it says");
  std::vector<uint8_t> b = good;
  b[0] = 'X';
  refused(b, b.size(), "magic");
  b = good;
  b[4] = 2;
  refused(b, b.size(), "version");
  b = good;
  b[9] = 2;
  refused(b, b.size(), "bits a pixel");
  b = good;
  b[10] = 7;
  refused(b, b.size(), "encoding");
  // The pages out of order: 0x30 and 0x31 swapped.
  b = good;
  const Section page = sectionOf(b, "PAGE");
  std::swap(b[page.at], b[page.at + 2]);
  refused(b, b.size(), "pages out of order");
  // A section past the end.
  b = good;
  const Section bits = sectionOf(b, "BITS");
  b[bits.entry + 8] = 0xFF;
  b[bits.entry + 9] = 0xFF;
  refused(b, b.size(), "a section past the end");
  // A section missing (its tag one the reader doesn't know).
  b = good;
  b[sectionOf(b, "METR").entry] = 'X';
  refused(b, b.size(), "no METR");
  // A page's first glyph not the glyphs before it.
  b = good;
  const Section map = sectionOf(b, "PMAP");
  b[map.at + 48 + 32] += 1;
  refused(b, b.size(), "a page's first glyph");
  // A word's count not the bits before it (page 0x30: U+3042 is in word 2).
  b = good;
  b[map.at + 40 + 3] += 1;
  refused(b, b.size(), "a word's count");
  // A page's presence bit more than the count says.
  b = good;
  b[map.at] |= 0x02;
  refused(b, b.size(), "more glyphs than the count");
}

// A bitmap that isn't well formed: false, and its box all 0.
void test_bad_bitmap_blanks_the_box() {
  std::vector<uint8_t> b = bytesOf(fixture::kRuns, fixture::kRunsSize);
  const Section bits = sectionOf(b, "BITS");
  Store s;
  TEST_ASSERT_TRUE(s.open(b.data(), b.size()));
  // U+3100's "f81" is the runs F 0 (one 15), 8, 1: make it F 5, six 15s in a box of three.
  const Glyph g = s.find(0x3100);
  const size_t at = bits.at + bitmapOf(b, g);
  TEST_ASSERT_EQUAL_HEX8(0xF0, b[at]);
  b[at] = 0xF5;
  uint8_t box[3] = {9, 9, 9};
  TEST_ASSERT_FALSE(s.decode(g, box, 1, sizeof(box)));
  for (const uint8_t v : box) TEST_ASSERT_EQUAL_UINT8(0, v);
  // The last glyph's bitmap cut short by its section's end.
  b = bytesOf(fixture::kRuns, fixture::kRunsSize);
  TEST_ASSERT_TRUE(s.open(b.data(), b.size()));
  const size_t last = bitmapOf(b, s.find(0x20000));
  b[bits.entry + 8] = static_cast<uint8_t>(last + 2);  // BITS is under 256 B here
  TEST_ASSERT_TRUE(s.open(b.data(), b.size()));
  uint8_t row[19];
  std::memset(row, 9, sizeof(row));
  TEST_ASSERT_FALSE(s.decode(s.find(0x20000), row, 19, sizeof(row)));
  for (const uint8_t v : row) TEST_ASSERT_EQUAL_UINT8(0, v);
  TEST_ASSERT_TRUE(s.contains(0x3042));  // the rest still reads
  uint8_t ok[25];
  TEST_ASSERT_TRUE(s.decode(s.find(0x3042), ok, 5, sizeof(ok)));
}

// ---- the built-in store ----

void test_builtin_header() {
  const Store& s = builtin();
  TEST_ASSERT_EQUAL_INT(16, s.pixelSize());
  // The CJK em box: 0.88 and 0.12 em, on DejaVu 16's baseline (15 above, 4
  // below: the Body row of 19), and the ink about as far.
  TEST_ASSERT_EQUAL_INT(14, s.ascent());
  TEST_ASSERT_EQUAL_INT(2, s.descent());
  TEST_ASSERT_TRUE(s.inkAscent() >= 14 && s.inkAscent() <= 16);
  TEST_ASSERT_TRUE(s.inkDescent() >= 2 && s.inkDescent() <= 4);
  TEST_ASSERT_TRUE(s.maxWidth() <= 16 + 2);
  TEST_ASSERT_TRUE(s.maxHeight() <= 16 + 4);
  // The flash it costs: the "pan" set measured 1.05 MB (docs/I18N.md).
  TEST_ASSERT_TRUE(kCjkGlyphs16Size < 1150u * 1024);
  TEST_ASSERT_TRUE(kCjkGlyphs16Size % 4 == 0);
}

// Every glyph: inside its box (the box tight: ink on its edges), the box
// within the store's limits and 2 px of the em box, and the whole as the
// generator drew it (its digest).
void test_builtin_every_glyph() {
  const Store& s = builtin();
  Fnv f;
  uint32_t found = 0, inked = 0;
  int up = 0, down = 0;
  std::vector<uint8_t> px(64 * 64);
  for (uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
    const Glyph g = s.find(cp);
    if (!g) continue;
    TEST_ASSERT_EQUAL_UINT32(found, g.index);  // in code point order
    ++found;
    digestGlyph(f, s, cp, g);
    const Metrics m = s.metrics(g);
    TEST_ASSERT_TRUE(m.advance <= 16);
    if (m.w == 0) {
      TEST_ASSERT_EQUAL_INT(0, m.h);
      continue;
    }
    ++inked;
    TEST_ASSERT_TRUE(m.w <= s.maxWidth() && m.h <= s.maxHeight());
    TEST_ASSERT_TRUE(m.y <= s.ascent() + 2 && m.h - m.y <= s.descent() + 2);
    up = m.y > up ? m.y : up;
    down = m.h - m.y > down ? m.h - m.y : down;
    TEST_ASSERT_TRUE(s.decode(g, px.data(), m.w, px.size(), Alpha::Nibble));
    bool top = false, bottom = false, left = false, right = false;
    for (int r = 0; r < m.h; ++r) {
      for (int c = 0; c < m.w; ++c) {
        const uint8_t v = px[r * m.w + c];
        TEST_ASSERT_TRUE(v <= 15);
        if (!v) continue;
        top |= r == 0;
        bottom |= r == m.h - 1;
        left |= c == 0;
        right |= c == m.w - 1;
      }
    }
    TEST_ASSERT_TRUE_MESSAGE(top && bottom && left && right, "a tight box");
  }
  TEST_ASSERT_EQUAL_UINT32(s.glyphCount(), found);
  TEST_ASSERT_TRUE(inked > 10000);
  TEST_ASSERT_EQUAL_INT(s.inkAscent(), up);
  TEST_ASSERT_EQUAL_INT(s.inkDescent(), down);
  TEST_ASSERT_TRUE_MESSAGE(f.h == kCjkGlyphs16Digest, "the generator's digest");
}

// Names in Hangul, kana and Han (public ones) draw whole, at the em's
// advance (Hangul is narrower in this font: 15 px).
void test_builtin_draws_names() {
  const Store& s = builtin();
  const char* names[] = {"東京事変", "宇多田ヒカル", "坂本龍一", "王菲", "周杰倫", "椎名林檎", "久石譲",
                         "アイユ", "ガンダム", "パフューム", "アイドル", "「」。、ー・〜"};
  for (const char* n : names) {
    for (const char* p = n; *p;) {
      const uint32_t cp = textfold::decode(p);
      TEST_ASSERT_TRUE_MESSAGE(s.contains(cp), n);
      TEST_ASSERT_EQUAL_INT_MESSAGE(16, s.advance(cp), n);
    }
  }
  const char* hangul[] = {"아이유", "방탄소년단", "서울", "빅뱅", "소녀시대"};
  for (const char* n : hangul) {
    for (const char* p = n; *p;) {
      const uint32_t cp = textfold::decode(p);
      TEST_ASSERT_TRUE_MESSAGE(s.contains(cp), n);
      TEST_ASSERT_EQUAL_INT_MESSAGE(15, s.advance(cp), n);
    }
  }
  // The jamo, and a kana voicing mark left over after composing: no advance,
  // drawn over the glyph before it.
  TEST_ASSERT_TRUE(s.contains(0x314B));  // ㅋ
  const Metrics mark = s.metrics(s.find(0x3099));
  TEST_ASSERT_EQUAL_INT(0, mark.advance);
  TEST_ASSERT_TRUE(mark.x < 0);
  // The ideographic space: a blank of the em.
  TEST_ASSERT_EQUAL_INT(16, s.advance(0x3000));
  TEST_ASSERT_EQUAL_INT(0, s.metrics(s.find(0x3000)).w);
}

// What it leaves out: what the VLW fonts draw, what TextFold folds first
// (fullwidth ASCII, halfwidth katakana), the Hangul KS X 1001 lacks, the
// marks only vertical text uses, the planes past the BMP.
void test_builtin_leaves_out() {
  const Store& s = builtin();
  const std::vector<uint32_t> vlw = vlwCodePoints();
  TEST_ASSERT_EQUAL_size_t(4 * 734, vlw.size());
  for (const uint32_t cp : vlw) TEST_ASSERT_FALSE(s.contains(cp));
  for (uint32_t cp = 0; cp < 0x2000; ++cp) TEST_ASSERT_FALSE(s.contains(cp));
  for (uint32_t cp = 0xFF01; cp <= 0xFF5E; ++cp) TEST_ASSERT_FALSE(s.contains(cp));
  for (uint32_t cp = 0xFF61; cp <= 0xFF9F; ++cp) TEST_ASSERT_FALSE(s.contains(cp));
  const uint32_t absent[] = {0xB620,  // 똠, outside KS X 1001's 2,350
                             0xD7A3,  // 힣, the last syllable
                             0x3031, 0x3032, 0x302A,  // vertical text's marks
                             0x3100,  // Bopomofo
                             0x2252,  // ≒: the face has no glyph
                             0x1F600, 0x20000, 0x2A6D6, 0x10FFFF, 0x110000};
  for (const uint32_t cp : absent) TEST_ASSERT_FALSE(s.contains(cp));
}

// The set: whole blocks where it takes them, and the edges of the Han and
// Hangul ranges.
void test_builtin_set() {
  const Store& s = builtin();
  uint32_t syllables = 0, han = 0;
  for (uint32_t cp = 0xAC00; cp <= 0xD7A3; ++cp) syllables += s.contains(cp);
  for (uint32_t cp = 0x4E00; cp <= 0x9FFF; ++cp) han += s.contains(cp);
  TEST_ASSERT_EQUAL_UINT32(2350, syllables);
  TEST_ASSERT_TRUE(han > 7500 && han < 8000);  // JIS X 0208's 6,355 and GB2312's level 1, together
  for (uint32_t cp = 0x3131; cp <= 0x318E; ++cp) TEST_ASSERT_TRUE(s.contains(cp));  // jamo
  for (uint32_t cp = 0x3041; cp <= 0x3096; ++cp) TEST_ASSERT_TRUE(s.contains(cp));  // hiragana
  for (uint32_t cp = 0x3099; cp <= 0x30FF; ++cp) TEST_ASSERT_TRUE(s.contains(cp));  // and katakana
  for (uint32_t cp = 0x31F0; cp <= 0x31FF; ++cp) TEST_ASSERT_TRUE(s.contains(cp));
  for (uint32_t cp = 0x3000; cp <= 0x303F; ++cp) {
    TEST_ASSERT_EQUAL(!(cp >= 0x302A && cp <= 0x302D) && cp != 0x3031 && cp != 0x3032, s.contains(cp));
  }
  // Page edges: the first Han and Hangul, and the code points before them.
  TEST_ASSERT_TRUE(s.contains(0x4E00));  // 一
  TEST_ASSERT_FALSE(s.contains(0x4DFF));
  TEST_ASSERT_TRUE(s.contains(0xAC00));  // 가
  TEST_ASSERT_FALSE(s.contains(0xABFF));
  TEST_ASSERT_TRUE(s.contains(0xD79D));  // 힝, KS X 1001's last
  TEST_ASSERT_TRUE(s.contains(0x30FF) && !s.contains(0x3100));
  // The first and the last glyph.
  uint32_t first = 0, last = 0;
  for (uint32_t cp = 0; cp <= 0xFFFF; ++cp) {
    if (!s.contains(cp)) continue;
    if (!first) first = cp;
    last = cp;
  }
  TEST_ASSERT_EQUAL_UINT32(0, s.find(first).index);
  TEST_ASSERT_EQUAL_UINT32(s.glyphCount() - 1, s.find(last).index);
  TEST_ASSERT_TRUE(first >= 0x2000 && first < 0x3000);  // a JIS symbol DejaVu lacks
  TEST_ASSERT_EQUAL_HEX32(0xFFEE, last);                // ￮, the last halfwidth sign
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fixture_header);
  RUN_TEST(test_fixture_glyphs_as_drawn);
  RUN_TEST(test_fixture_absent);
  RUN_TEST(test_fixture_page_edges_and_planes);
  RUN_TEST(test_decode_buffers);
  RUN_TEST(test_open_refuses);
  RUN_TEST(test_bad_bitmap_blanks_the_box);
  RUN_TEST(test_builtin_header);
  RUN_TEST(test_builtin_every_glyph);
  RUN_TEST(test_builtin_draws_names);
  RUN_TEST(test_builtin_leaves_out);
  RUN_TEST(test_builtin_set);
  return UNITY_END();
}
