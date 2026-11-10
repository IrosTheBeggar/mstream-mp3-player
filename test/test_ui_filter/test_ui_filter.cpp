// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the mood filters' editor and the Albums list's first row
// as numbers (lib/core/FilterGrid; docs/MOOD-FILTERS.md 7.1-7.4, 4.1,
// 4.3): what a tap does (a second tap lets a tray's pick go, tags any
// number, the level cycles and alone filters nothing, Clear resets it);
// the widths' branches (equal, proportional, scaled, a chip row's overflow
// given up evenly, the wrap, a half rounded down) with a made-up font;
// every panel point one cell at most, in its row, in order; the title
// row's hits; the first row's zones at 284, 312 and 320; the dot; the
// summary's order and the count's forms in English. The real fonts, every
// language and the lab's panel are test_ui_library's. And the Albums
// list's filtered albums (lib/core/FilteredAlbums; 4.1, 4.2): a
// subsequence of albumsAZ(), by the majority rule, nothing held without a
// filter or once dropped, an allocator's failure keeping what was held,
// worked out again for a new filter and a new index. Run: pio test -e
// native -f test_ui_filter
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "FilterGrid.h"
#include "FilteredAlbums.h"
#include "LibraryIndex.h"
#include "MoodFilter.h"
#include "TextFold.h"
#include "UiStrings.h"

using filtergrid::Cell;
using filtergrid::Layout;
using moodfilter::Filter;
using moodfilter::Level;
using moodfilter::Scale;
using moodfilter::Voice;

void setUp() { uistr::setLang(uistr::Lang::En); }
void tearDown() {}

namespace {

// A made-up font: a letter `letter` px, '.' 3, a space `space` (so a
// width can be odd).
struct Fake {
  int letter, space;
};
int fakeWidth(void* ctx, const char* s) {
  const Fake& f = *static_cast<const Fake*>(ctx);
  int w = 0;
  for (const char* p = s; *p;) {
    const uint32_t cp = textfold::decode(p);
    w += cp == ' ' ? f.space : cp == '.' ? 3 : f.letter;
  }
  return w;
}
Fake bodyFake{8, 4}, smallFake{6, 3};
textfit::Font viewOf(Fake& f) {
  textfit::Font t;
  t.ctx = &f;
  t.width = fakeWidth;
  return t;
}
const textfit::Font& body() {
  static const textfit::Font f = viewOf(bodyFake);
  return f;
}
const textfit::Font& small() {
  static const textfit::Font f = viewOf(smallFake);
  return f;
}

// Words: English's, with some cells' replaced.
struct WordsOf {
  filtergrid::Words w;
  std::string held[filtergrid::kCells];
  WordsOf() {
    const char* cells[filtergrid::kCells] = {"Happy", "Neutral", "Sad",  "Relaxed", "Neutral", "Aggressive", "Instrumental",
                                             "Vocals", nullptr,  "Acoustic", "Party", "Danceable", "Clear", nullptr};
    for (int c = 0; c < filtergrid::kCells; ++c) w.cell[c] = cells[c];
    w.level[0] = "Standard";
    w.level[1] = "Strict";
    w.level[2] = "Loose";
  }
  WordsOf& set(Cell c, const std::string& s) {
    held[filtergrid::index(c)] = s;
    w.cell[filtergrid::index(c)] = held[filtergrid::index(c)].c_str();
    return *this;
  }
};

std::string letters(int n, char c = 'a') { return std::string(static_cast<size_t>(n), c); }

void cellIs(const Layout& l, Cell c, int x, int w) {
  char msg[48];
  snprintf(msg, sizeof(msg), "cell %d: (%d,%d)", filtergrid::index(c), l.x[filtergrid::index(c)],
           l.w[filtergrid::index(c)]);
  TEST_ASSERT_EQUAL_INT_MESSAGE(x, l.x[filtergrid::index(c)], msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(w, l.w[filtergrid::index(c)], msg);
}

constexpr Cell kTrays[] = {Cell::Happy,    Cell::NeutralV,     Cell::Sad,    Cell::Relaxed,
                           Cell::NeutralE, Cell::Aggressive,   Cell::Instrumental, Cell::Vocals};

}  // namespace

// ---- taps (4.1, 4.3) ----

void test_a_second_tap_lets_go() {
  for (Cell c : kTrays) {
    const Filter on = filtergrid::tap(Filter(), c);
    TEST_ASSERT_TRUE(on.active());
    TEST_ASSERT_TRUE(filtergrid::picked(on, c));
    const Filter off = filtergrid::tap(on, c);
    TEST_ASSERT_FALSE(off.active());
    TEST_ASSERT_EQUAL_UINT16(0, off.pack());
  }
  // Another cell of the same tray moves the pick; another tray's adds.
  Filter f = filtergrid::tap(Filter(), Cell::Happy);
  f = filtergrid::tap(f, Cell::Sad);
  TEST_ASSERT_TRUE(f.valence == Scale::Second);
  TEST_ASSERT_FALSE(filtergrid::picked(f, Cell::Happy));
  f = filtergrid::tap(f, Cell::NeutralE);
  TEST_ASSERT_TRUE(f.valence == Scale::Second && f.energy == Scale::Neutral);
  f = filtergrid::tap(f, Cell::Vocals);
  f = filtergrid::tap(f, Cell::Instrumental);
  TEST_ASSERT_TRUE(f.voice == Voice::Instrumental);
  TEST_ASSERT_FALSE(filtergrid::picked(f, Cell::Vocals));
  // The two Neutrals are two cells.
  TEST_ASSERT_FALSE(filtergrid::picked(f, Cell::NeutralV));
  TEST_ASSERT_TRUE(filtergrid::picked(f, Cell::NeutralE));
  // The cross, None and the count change nothing.
  for (Cell c : {Cell::Cross, Cell::None}) TEST_ASSERT_EQUAL_UINT16(f.pack(), filtergrid::tap(f, c).pack());
}

void test_tags_any_number() {
  Filter f;
  f = filtergrid::tap(f, Cell::Acoustic);
  f = filtergrid::tap(f, Cell::Party);
  f = filtergrid::tap(f, Cell::Danceable);
  TEST_ASSERT_EQUAL_UINT8(moodfilter::kAcoustic | moodfilter::kParty | moodfilter::kDanceable, f.tags);
  for (Cell c : {Cell::Acoustic, Cell::Party, Cell::Danceable}) TEST_ASSERT_TRUE(filtergrid::picked(f, c));
  f = filtergrid::tap(f, Cell::Party);
  TEST_ASSERT_EQUAL_UINT8(moodfilter::kAcoustic | moodfilter::kDanceable, f.tags);
  TEST_ASSERT_FALSE(filtergrid::picked(f, Cell::Party));
  TEST_ASSERT_TRUE(f.active());
  // Each tag alone is a filter; none left is none.
  f = filtergrid::tap(filtergrid::tap(f, Cell::Acoustic), Cell::Danceable);
  TEST_ASSERT_FALSE(f.active());
}

void test_a_level_alone_is_inactive() {
  // Strict > Standard > Loose > Strict (3.2), from the default.
  Filter f;
  TEST_ASSERT_TRUE(f.level == Level::Standard);
  TEST_ASSERT_EQUAL_INT(1, filtergrid::levelDot(f.level));
  TEST_ASSERT_FALSE(filtergrid::anythingSet(f));
  const Level order[] = {Level::Loose, Level::Strict, Level::Standard, Level::Loose};
  const int dots[] = {2, 0, 1, 2};
  for (int k = 0; k < 4; ++k) {
    f = filtergrid::tap(f, Cell::Level);
    TEST_ASSERT_TRUE(f.level == order[k]);
    TEST_ASSERT_EQUAL_INT(dots[k], filtergrid::levelDot(f.level));
    // A level alone filters nothing, but there is something to clear.
    TEST_ASSERT_FALSE(f.active());
    TEST_ASSERT_EQUAL(f.level != Level::Standard, filtergrid::anythingSet(f));
    TEST_ASSERT_FALSE(filtergrid::picked(f, Cell::Level));
    TEST_ASSERT_TRUE(moodfilter::matches(0, f));  // even word 0, no row
  }
  // With a group set it filters, at the level.
  f = filtergrid::tap(filtergrid::tap(Filter(), Cell::Level), Cell::Party);  // Loose, Party
  TEST_ASSERT_TRUE(f.active());
  TEST_ASSERT_TRUE(f.level == Level::Loose);
}

void test_clear_resets_the_level() {
  Filter f;
  for (Cell c : {Cell::Happy, Cell::Aggressive, Cell::Vocals, Cell::Acoustic, Cell::Danceable, Cell::Level,
                 Cell::Level})
    f = filtergrid::tap(f, c);
  TEST_ASSERT_TRUE(f.level == Level::Strict);
  TEST_ASSERT_TRUE(filtergrid::anythingSet(f));
  const Filter cleared = filtergrid::tap(f, Cell::Clear);
  TEST_ASSERT_EQUAL_UINT16(0, cleared.pack());
  TEST_ASSERT_TRUE(cleared.level == Level::Standard);
  TEST_ASSERT_FALSE(filtergrid::anythingSet(cleared));
  // A level alone clears too.
  TEST_ASSERT_EQUAL_UINT16(0, filtergrid::tap(filtergrid::tap(Filter(), Cell::Level), Cell::Clear).pack());
}

// ---- widths (7.3) ----

void test_equal_widths() {
  const Layout l = filtergrid::layout(WordsOf().w, body(), small());
  TEST_ASSERT_FALSE(l.small);
  // Body: Instrumental is 96 px, + 12 = 108 of the voice tray's 112.
  for (int row = 0; row < filtergrid::kRows; ++row) TEST_ASSERT_TRUE(l.share[row] == Layout::Share::Equal);
  cellIs(l, Cell::Happy, 10, 100);
  cellIs(l, Cell::NeutralV, 110, 100);
  cellIs(l, Cell::Sad, 210, 100);
  cellIs(l, Cell::Aggressive, 210, 100);
  // The level cell: "Standard" in Small (48) + 14 is under 70.
  cellIs(l, Cell::Level, 242, 70);
  TEST_ASSERT_EQUAL_INT(228, l.trayW[2]);
  cellIs(l, Cell::Instrumental, 10, 112);
  cellIs(l, Cell::Vocals, 122, 112);
  // The chips: 292 shared with their edges rounded (97.3, 194.7).
  cellIs(l, Cell::Acoustic, 8, 97);
  cellIs(l, Cell::Party, 111, 98);
  cellIs(l, Cell::Danceable, 215, 97);
  // The title row: Clear (30 px in Small) + 24; the count between.
  cellIs(l, Cell::Clear, 12, 54);
  cellIs(l, Cell::Cross, 262, 50);
  TEST_ASSERT_EQUAL_INT(256 - 66 - 8, l.countW);
  TEST_ASSERT_EQUAL_INT((66 + 256) / 2, l.countX);
  const filtergrid::Box b = filtergrid::box(l, Cell::Danceable);
  TEST_ASSERT_EQUAL_INT(200, b.y);
  TEST_ASSERT_EQUAL_INT(36, b.h);
  TEST_ASSERT_EQUAL_INT(40, filtergrid::box(l, Cell::Clear).y);
  TEST_ASSERT_EQUAL_INT(24, filtergrid::box(l, Cell::Clear).h);
  for (int c = 0; c < filtergrid::kCells; ++c) TEST_ASSERT_EQUAL_INT(0, l.wrap[c]);
}

void test_proportional_widths() {
  // 13 letters: 104 + 12 = 116, over its equal 100. The spare, 128, goes
  // a third to each: 42.7 rounded, the edges 159 and 229.
  const Layout l = filtergrid::layout(WordsOf().set(Cell::Happy, letters(13)).set(Cell::NeutralV, "ab").set(Cell::Sad, "cd").w,
                                      body(), small());
  TEST_ASSERT_FALSE(l.small);
  TEST_ASSERT_TRUE(l.share[0] == Layout::Share::Proportional);
  TEST_ASSERT_TRUE(l.share[1] == Layout::Share::Equal);
  cellIs(l, Cell::Happy, 10, 159);
  cellIs(l, Cell::NeutralV, 169, 70);
  cellIs(l, Cell::Sad, 239, 71);
}

void test_a_half_rounds_down() {
  // The voice tray's 224: 14 letters and a '.' (115) + 12, and "V" (8) +
  // 12, leave 77; 38.5 each, so the edge is 127 + 38 (as the design drew
  // Spanish's and French's).
  const Layout l = filtergrid::layout(WordsOf().set(Cell::Instrumental, letters(14) + ".").set(Cell::Vocals, "V").w,
                                      body(), small());
  TEST_ASSERT_TRUE(l.share[2] == Layout::Share::Proportional);
  cellIs(l, Cell::Instrumental, 10, 165);
  cellIs(l, Cell::Vocals, 175, 59);
}

void test_small_and_scaled_widths() {
  // Three 13-letter words don't fit the valence tray in Body (116 x 3):
  // every word goes to Small, where they do (90 each: equal).
  WordsOf w;
  w.set(Cell::Happy, letters(13)).set(Cell::NeutralV, letters(13)).set(Cell::Sad, letters(13));
  // The voice tray in Small: 30 letters (180) + 12 and 5 (30) + 12 are 234
  // in 224: they scale, the edge at 192 x 224 / 234 = 183.8.
  w.set(Cell::Instrumental, letters(30)).set(Cell::Vocals, letters(5));
  const Layout l = filtergrid::layout(w.w, body(), small());
  TEST_ASSERT_TRUE(l.small);
  TEST_ASSERT_TRUE(l.share[0] == Layout::Share::Equal);
  TEST_ASSERT_TRUE(l.share[2] == Layout::Share::Scaled);
  cellIs(l, Cell::Happy, 10, 100);
  cellIs(l, Cell::Instrumental, 10, 184);
  cellIs(l, Cell::Vocals, 194, 40);
  // English's other words are in Small too: Danceable's 54 + 12, equal.
  TEST_ASSERT_TRUE(l.share[3] == Layout::Share::Equal);
  // The level cell is Small in either grid: a 12-letter word, 72 + 14.
  w.set(Cell::Instrumental, "I");
  w.w.level[1] = "Strictissimo";
  const Layout k = filtergrid::layout(w.w, body(), small());
  cellIs(k, Cell::Level, 304 + 8 - 86, 86);
  TEST_ASSERT_EQUAL_INT(304 - 6 - 86, k.trayW[2]);
}

void test_a_tag_wraps_and_the_chips_give_up_an_overflow_evenly() {
  // In Small (the valence tray's 13-letter words force it), a tag over 80
  // px with a space wraps at it: "aaaa bbbbbbbbbbbbbb" is 99 px, its lines
  // 24 and 84, its need 96. The space that leaves the wider line the
  // narrowest is the one taken.
  WordsOf w;
  w.set(Cell::Happy, letters(13)).set(Cell::NeutralV, letters(13)).set(Cell::Sad, letters(13));
  w.set(Cell::Acoustic, letters(21)).set(Cell::Party, "aaaa " + letters(14, 'b')).set(Cell::Danceable, letters(11));
  const Layout l = filtergrid::layout(w.w, body(), small());
  TEST_ASSERT_TRUE(l.small);
  TEST_ASSERT_EQUAL_INT(5, l.wrap[filtergrid::index(Cell::Party)]);
  TEST_ASSERT_EQUAL_INT(0, l.wrap[filtergrid::index(Cell::Acoustic)]);
  // Needs 138, 96, 78: 312 in 292. The 20 px go evenly (-6.7 a chip:
  // edges 131 and 221), not scaled (129 and 219).
  TEST_ASSERT_TRUE(l.share[3] == Layout::Share::Proportional);
  cellIs(l, Cell::Acoustic, 8, 131);
  cellIs(l, Cell::Party, 145, 90);
  cellIs(l, Cell::Danceable, 241, 71);
  // Two spaces: the break that balances the lines.
  w.set(Cell::Party, "aa bbbbbbbb cccccccc");  // lines 12 | 99, or 63 | 48
  TEST_ASSERT_EQUAL_INT(12, filtergrid::layout(w.w, body(), small()).wrap[filtergrid::index(Cell::Party)]);
  w.set(Cell::Party, "aaaaaaaa bbbbbbbb cc");  // 48 | 63, or 99 | 12
  TEST_ASSERT_EQUAL_INT(9, filtergrid::layout(w.w, body(), small()).wrap[filtergrid::index(Cell::Party)]);
  // 80 px or under, or no space, or in Body: one line.
  w.set(Cell::Party, letters(6) + " " + letters(6));  // 75
  TEST_ASSERT_EQUAL_INT(0, filtergrid::layout(w.w, body(), small()).wrap[filtergrid::index(Cell::Party)]);
  w.set(Cell::Party, letters(16));  // 96, no space
  TEST_ASSERT_EQUAL_INT(0, filtergrid::layout(w.w, body(), small()).wrap[filtergrid::index(Cell::Party)]);
  // (Body 120, Small 81: the chips' row fits in Body.)
  const Layout bodyGrid = filtergrid::layout(WordsOf().set(Cell::Party, "aaaa " + letters(9, 'b')).w, body(), small());
  TEST_ASSERT_FALSE(bodyGrid.small);
  TEST_ASSERT_EQUAL_INT(0, bodyGrid.wrap[filtergrid::index(Cell::Party)]);
}

// ---- hits (7.4) ----

namespace {

// Every point of the panel: at most one cell, in its own row's band, a
// row's cells left to right each in one run, and each cell's centre its
// own; a clamped reading the row's last.
void auditHits(const Layout& l) {
  for (int y = filtergrid::kPanelY; y < 240; ++y) {
    const int row = y < filtergrid::kTitleBand ? -1 : std::min(3, (y - filtergrid::kTitleBand) / filtergrid::kRowPitch);
    int last = -1;
    Cell prev = Cell::None;
    for (int x = 0; x < 320; ++x) {
      const Cell c = filtergrid::hit(l, x, y, false);
      char msg[64];
      snprintf(msg, sizeof(msg), "(%d, %d) is cell %d", x, y, filtergrid::index(c));
      if (c == Cell::None) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, row, msg);  // only the title row's count is nothing
        continue;
      }
      TEST_ASSERT_EQUAL_INT_MESSAGE(row, filtergrid::rowOf(c), msg);
      if (c != prev) {
        TEST_ASSERT_TRUE_MESSAGE(filtergrid::index(c) > last, msg);  // a new cell, never one seen before
        last = filtergrid::index(c);
        prev = c;
      }
    }
    const Cell edge = filtergrid::hit(l, 120, y, true);
    TEST_ASSERT_TRUE(edge == (row < 0 ? Cell::Cross : row == 2 ? Cell::Level : row == 0 ? Cell::Sad
                                                    : row == 1 ? Cell::Aggressive : Cell::Danceable));
  }
  for (int c = 0; c <= filtergrid::index(Cell::Cross); ++c) {
    const filtergrid::Box b = filtergrid::box(l, static_cast<Cell>(c));
    TEST_ASSERT_TRUE(filtergrid::hit(l, b.x + b.w / 2, b.y + b.h / 2, false) == static_cast<Cell>(c));
  }
}

}  // namespace

void test_every_panel_point_is_one_cell_at_most() {
  auditHits(filtergrid::layout(WordsOf().w, body(), small()));
  WordsOf w;
  w.set(Cell::Happy, letters(13)).set(Cell::NeutralV, letters(13)).set(Cell::Sad, letters(13));
  w.set(Cell::Instrumental, letters(30)).set(Cell::Vocals, letters(5));
  w.set(Cell::Acoustic, letters(21)).set(Cell::Party, "aaaa " + letters(14, 'b')).set(Cell::Danceable, letters(11));
  auditHits(filtergrid::layout(w.w, body(), small()));
}

void test_splits_skew_right() {
  TEST_ASSERT_EQUAL_INT(0, filtergrid::skew(149));
  TEST_ASSERT_EQUAL_INT(6, filtergrid::skew(150));
  TEST_ASSERT_EQUAL_INT(6, filtergrid::skew(189));
  TEST_ASSERT_EQUAL_INT(13, filtergrid::skew(190));
  const Layout l = filtergrid::layout(WordsOf().w, body(), small());
  // Valence: edges 110 and 210, the second skewed 13.
  TEST_ASSERT_EQUAL_INT(110, filtergrid::split(l, 0, 0));
  TEST_ASSERT_EQUAL_INT(223, filtergrid::split(l, 0, 1));
  TEST_ASSERT_TRUE(filtergrid::hit(l, 222, 90, false) == Cell::NeutralV);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 223, 90, false) == Cell::Sad);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 109, 90, false) == Cell::Happy);
  TEST_ASSERT_TRUE(filtergrid::hit(l, -4, 90, false) == Cell::Happy);
  // Voice: Instrumental | Vocals at 122; the tray's gap before the level
  // cell (236-241) at its middle, 239, + 13.
  TEST_ASSERT_EQUAL_INT(122, filtergrid::split(l, 2, 0));
  TEST_ASSERT_EQUAL_INT(252, filtergrid::split(l, 2, 1));
  // The chips' gaps (105-110, 209-214) at their middles, 108 and 212.
  TEST_ASSERT_EQUAL_INT(108, filtergrid::split(l, 3, 0));
  TEST_ASSERT_EQUAL_INT(225, filtergrid::split(l, 3, 1));
  // The bands: y 66, 110, 154, 198 to the bottom.
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 66, false) == Cell::Happy);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 109, false) == Cell::Happy);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 110, false) == Cell::Relaxed);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 153, false) == Cell::Relaxed);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 154, false) == Cell::Instrumental);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 197, false) == Cell::Instrumental);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 198, false) == Cell::Acoustic);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 20, 239, false) == Cell::Acoustic);
}

void test_title_row_hits() {
  const Layout l = filtergrid::layout(WordsOf().w, body(), small());
  // Clear's pill 12-65: its hit to 66 (no skew under 150).
  TEST_ASSERT_TRUE(filtergrid::hit(l, 0, 50, false) == Cell::Clear);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 65, 40, false) == Cell::Clear);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 66, 50, false) == Cell::None);
  // The count is nothing; the cross from x 250 to the edge.
  TEST_ASSERT_TRUE(filtergrid::hit(l, l.countX, 52, false) == Cell::None);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 249, 52, false) == Cell::None);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 250, 52, false) == Cell::Cross);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 319, 65, false) == Cell::Cross);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 30, 52, true) == Cell::Cross);
  TEST_ASSERT_TRUE(filtergrid::hit(l, 300, 66, false) == Cell::Sad);  // the first row's band
  // A Clear pill that ends right of x 150 has its skew.
  const Layout wide = filtergrid::layout(WordsOf().set(Cell::Clear, letters(21)).w, body(), small());
  TEST_ASSERT_EQUAL_INT(126 + 24, wide.w[filtergrid::index(Cell::Clear)]);  // to x 162
  TEST_ASSERT_TRUE(filtergrid::hit(wide, 167, 50, false) == Cell::Clear);
  TEST_ASSERT_TRUE(filtergrid::hit(wide, 168, 50, false) == Cell::None);
}

void test_first_row_zones() {
  using filtergrid::Top;
  struct Case {
    int right, cross, hitX, roomOn, roomOff;
  };
  const Case cases[] = {{284, 232, 224, 160, 204}, {312, 284, 276, 212, 232}, {320, 292, 284, 220, 240}};
  for (const Case& c : cases) {
    TEST_ASSERT_EQUAL_INT(c.cross, filtergrid::crossX(c.right));
    TEST_ASSERT_EQUAL_INT(c.hitX, filtergrid::crossHitX(c.right));
    TEST_ASSERT_EQUAL_INT(c.roomOn, filtergrid::textRoom(c.right, true));
    TEST_ASSERT_EQUAL_INT(c.roomOff, filtergrid::textRoom(c.right, false));
    // On: the cross from its zone, the rest the editor.
    TEST_ASSERT_TRUE(filtergrid::topHit(0, false, c.right, true) == Top::Editor);
    TEST_ASSERT_TRUE(filtergrid::topHit(c.hitX - 1, false, c.right, true) == Top::Editor);
    TEST_ASSERT_TRUE(filtergrid::topHit(c.hitX, false, c.right, true) == Top::Cross);
    // At rest: no cross.
    TEST_ASSERT_TRUE(filtergrid::topHit(c.hitX, false, c.right, false) == Top::Editor);
    const bool rail = c.right == filtergrid::kRightRail;
    // Beside the rail, its zone stops at 280: the rail's, a clamped reading too.
    TEST_ASSERT_TRUE(filtergrid::topHit(279, false, c.right, true) == (279 >= c.hitX ? Top::Cross : Top::Editor));
    TEST_ASSERT_TRUE(filtergrid::topHit(280, false, c.right, true) ==
                     (rail ? Top::Rail : 280 >= c.hitX ? Top::Cross : Top::Editor));
    TEST_ASSERT_TRUE(filtergrid::topHit(100, true, c.right, true) == (rail ? Top::Rail : Top::Cross));
    TEST_ASSERT_TRUE(filtergrid::topHit(280, false, c.right, false) == (rail ? Top::Rail : Top::Editor));
    TEST_ASSERT_TRUE(filtergrid::topHit(319, true, c.right, false) == (rail ? Top::Rail : Top::Editor));
    // The disc (r 14) inside the row, clear of the rail's column.
    TEST_ASSERT_TRUE(c.cross + filtergrid::kDiscR <= c.right);
  }
}

void test_the_dot() {
  TEST_ASSERT_FALSE(filtergrid::dotShown(true, true, 0));
  TEST_ASSERT_FALSE(filtergrid::dotShown(true, true, 41));  // the first row still shows
  TEST_ASSERT_TRUE(filtergrid::dotShown(true, true, 42));
  TEST_ASSERT_TRUE(filtergrid::dotShown(true, true, 5000));
  TEST_ASSERT_FALSE(filtergrid::dotShown(false, true, 5000));  // no filter
  TEST_ASSERT_FALSE(filtergrid::dotShown(true, false, 5000));  // Artists or Folders on show
  // In the strip, right of the segment's centre (x 160), inside it (110-209).
  TEST_ASSERT_TRUE(filtergrid::kDotX > 160 && filtergrid::kDotX + filtergrid::kDotD <= 210);
}

// ---- the texts (7.2) ----

void test_summary_order() {
  char buf[filtergrid::kSummaryBytes];
  TEST_ASSERT_EQUAL_UINT(0, filtergrid::summary(Filter(), buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
  // A level alone: said, though it filters nothing (the row isn't on then).
  filtergrid::summary(filtergrid::tap(Filter(), Cell::Level), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Loose", buf);
  // Tapped in any order, said valence, energy, voice, tags, level.
  Filter f;
  for (Cell c : {Cell::Level, Cell::Level, Cell::Danceable, Cell::Vocals, Cell::Acoustic, Cell::Aggressive,
                 Cell::Party, Cell::Happy})
    f = filtergrid::tap(f, c);
  filtergrid::summary(f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Happy \xC2\xB7 Aggressive \xC2\xB7 Vocals \xC2\xB7 Acoustic \xC2\xB7 Party \xC2\xB7 "
                           "Danceable \xC2\xB7 Strict",
                           buf);
  // Standard isn't said; the two Neutrals both are.
  f = Filter();
  for (Cell c : {Cell::NeutralE, Cell::Instrumental, Cell::NeutralV}) f = filtergrid::tap(f, c);
  filtergrid::summary(f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("Neutral \xC2\xB7 Neutral \xC2\xB7 Instrumental", buf);
  // The pack round trip says the same.
  char again[filtergrid::kSummaryBytes];
  filtergrid::summary(Filter::unpack(f.pack()), again, sizeof(again));
  TEST_ASSERT_EQUAL_STRING(buf, again);
  // A short buffer keeps whole words: no stray " ·" or half a word.
  char tiny[12];
  filtergrid::summary(f, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("Neutral", tiny);
}

void test_count_forms() {
  char buf[filtergrid::kCountBytes];
  using filtergrid::Scope;
  filtergrid::countText(small(), 999, Scope::Albums, false, 312, 1741, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1,741 albums", buf);
  filtergrid::countText(small(), 999, Scope::Albums, true, 312, 1741, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("312 of 1,741 albums", buf);
  filtergrid::countText(small(), 999, Scope::Albums, true, 0, 1, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0 of 1 album", buf);
  filtergrid::countText(small(), 999, Scope::Tracks, true, 1204, 19412, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1,204 of 19,412 tracks", buf);
  filtergrid::countText(small(), 999, Scope::Tracks, false, 1204, 19412, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("19,412 tracks", buf);
  // Over its room: the short form.
  const int longW = textfit::drawnWidth(small(), "312 of 1,741 albums");
  filtergrid::countText(small(), longW, Scope::Albums, true, 312, 1741, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("312 of 1,741 albums", buf);
  filtergrid::countText(small(), longW - 1, Scope::Albums, true, 312, 1741, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("312 of 1,741", buf);
  // The short form, whatever it measures (the draw cuts it then).
  filtergrid::countText(small(), 1, Scope::Tracks, true, 99999, 99999, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("99,999 of 99,999", buf);
}

// ---- the Albums list's filtered albums (4.1, 4.2; FilteredAlbums) ----

namespace {

// Two words (test_ui_library's): 5.7's worked example, which 0x127 (sad,
// relaxed, vocals, danceable) lets through, and a happy, relaxed song.
constexpr uint16_t kFits = 0xB313;
constexpr uint16_t kMiss = 0x9B00;
const Filter kSadRelaxed = Filter::unpack(0x127);

// The allocator hooks, counted; `failing` refuses every block.
int allocs = 0, frees = 0;
bool failing = false;
void* countedAlloc(size_t n) {
  if (failing) return nullptr;
  ++allocs;
  return std::malloc(n);
}
void countedFree(void* p) {
  ++frees;
  std::free(p);
}

void addTrack(LibraryIndex& idx, const char* path, uint16_t word) {
  LibraryIndex::TagView v;
  v.source = LibraryIndex::kFromTransfer;
  v.title = "Song";
  v.titleLen = 4;
  v.moods = word;
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addRecord(path, v));
}

// test_ui_library's library of every album shape, with words (its
// buildMoodLibrary()); under 0x127:
//   A/One     3 of 4 analysed fit: fits
//   A/Two     2 of 4: half, no
//   A/Three   2 of 3 analysed (a 4th without a word): fits
//   B/Unheard no word at all: 0 of 0, never
//   B/        an artist's loose track that fits: 1 of 1
//   C/Four    two discs' folders, 2 of 3: fits
//   /         the root's loose track, which doesn't: 0 of 1
void buildMoodLibrary(LibraryIndex& idx) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  addTrack(idx, "/music/A/One/01 - a.mp3", kFits);
  addTrack(idx, "/music/A/One/02 - b.mp3", kFits);
  addTrack(idx, "/music/A/One/03 - c.mp3", kFits);
  addTrack(idx, "/music/A/One/04 - d.mp3", kMiss);
  addTrack(idx, "/music/A/Three/01 - a.mp3", kFits);
  addTrack(idx, "/music/A/Three/02 - b.mp3", kFits);
  addTrack(idx, "/music/A/Three/03 - c.mp3", kMiss);
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile("/music/A/Three/04 - d.mp3"));
  addTrack(idx, "/music/A/Two/01 - a.mp3", kFits);
  addTrack(idx, "/music/A/Two/02 - b.mp3", kFits);
  addTrack(idx, "/music/A/Two/03 - c.mp3", kMiss);
  addTrack(idx, "/music/A/Two/04 - d.mp3", kMiss);
  addTrack(idx, "/music/B/05 - Loose.mp3", kFits);
  addTrack(idx, "/music/B/Unheard/01 - a.mp3", 0);
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile("/music/B/Unheard/02 - b.mp3"));
  addTrack(idx, "/music/C/Four/CD1/01 - a.mp3", kFits);
  addTrack(idx, "/music/C/Four/CD2/01 - b.mp3", kFits);
  addTrack(idx, "/music/C/Four/CD2/02 - c.mp3", kMiss);
  addTrack(idx, "/music/06 - Root.mp3", kMiss);
  TEST_ASSERT_TRUE(idx.finish());
}

// Two albums, no words.
void buildPlainLibrary(LibraryIndex& idx) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addFile("/music/A/One/01 - a.mp3");
  idx.addFile("/music/A/One/02 - b.mp3");
  idx.addFile("/music/B/03 - c.mp3");
  TEST_ASSERT_TRUE(idx.finish());
}

std::string folderOf(const LibraryIndex& idx, uint32_t album) {
  char path[256];
  idx.folderPath(idx.album(album).folder, path, sizeof(path));
  return path;
}

// The held albums, by folder, in their order.
std::string heldFolders(const LibraryIndex& idx, const FilteredAlbums& fa) {
  std::string out;
  for (uint32_t i = 0; i < fa.count(); ++i) out += (i ? " " : "") + folderOf(idx, fa[i]);
  return out;
}

// What the list would hold: albumsAZ() in its order, each album by 4.1's
// rule over its tracks, from the words.
std::string wantedFolders(const LibraryIndex& idx, const Filter& f) {
  std::string out;
  const LibraryIndex::Span az = idx.albumsAZ();
  for (uint32_t k = 0; k < az.count; ++k) {
    const LibraryIndex::Span t = idx.tracksOfAlbum(az[k]);
    uint32_t pass = 0, analysed = 0;
    for (uint32_t j = 0; j < t.count; ++j) {
      const uint16_t w = idx.moodWord(t[j]);
      if (!(w & moodfilter::kAnalysed)) continue;
      ++analysed;
      pass += moodfilter::matches(w, f);
    }
    if (2 * pass > analysed) out += (out.empty() ? "" : " ") + folderOf(idx, az[k]);
  }
  return out;
}

}  // namespace

// Under 0x127, the four albums that fit, in albumsAZ()'s order: each one
// of its entries, none twice, none out of order.
void test_filtered_albums_are_a_subsequence() {
  static LibraryIndex idx;
  buildMoodLibrary(idx);
  FilteredAlbums fa(countedAlloc, countedFree);
  TEST_ASSERT_FALSE(fa.held());
  TEST_ASSERT_TRUE(fa.recompute(idx, kSadRelaxed) == FilteredAlbums::Result::Filtered);
  TEST_ASSERT_TRUE(fa.held());
  TEST_ASSERT_EQUAL_UINT32(4, fa.count());
  TEST_ASSERT_EQUAL_UINT32(7, fa.total());
  TEST_ASSERT_EQUAL_HEX16(0x127, fa.word());
  TEST_ASSERT_EQUAL_size_t(7 * sizeof(uint32_t), fa.bytes());  // a u32 an album, every album's room
  const LibraryIndex::Span az = idx.albumsAZ();
  uint32_t k = 0;
  for (uint32_t i = 0; i < fa.count(); ++i) {
    while (k < az.count && az[k] != fa[i]) ++k;
    TEST_ASSERT_TRUE_MESSAGE(k < az.count, "a held album out of albumsAZ()'s order, or not in it");
    ++k;
  }
  const LibraryIndex::Span s = fa.span();
  TEST_ASSERT_EQUAL_UINT32(4, s.count);
  for (uint32_t i = 0; i < s.count; ++i) TEST_ASSERT_EQUAL_UINT32(fa[i], s[i]);
}

// An album fits when more than half its analysed tracks do: A/Two's half
// doesn't, A/Three's 2 of 3 does (its track with no word not counted),
// B/Unheard's none of none never. And so for every filter there is.
void test_filtered_albums_by_majority() {
  static LibraryIndex idx;
  buildMoodLibrary(idx);
  FilteredAlbums fa;
  fa.recompute(idx, kSadRelaxed);
  TEST_ASSERT_EQUAL_STRING(wantedFolders(idx, kSadRelaxed).c_str(), heldFolders(idx, fa).c_str());
  const std::string held = " " + heldFolders(idx, fa) + " ";
  for (const char* in : {"/music/A/One", "/music/A/Three", "/music/B", "/music/C/Four"}) {
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(held.c_str(), (std::string(" ") + in + " ").c_str()), in);
  }
  for (const char* out : {"/music/A/Two", "/music/B/Unheard", "/music"}) {
    TEST_ASSERT_NULL_MESSAGE(strstr(held.c_str(), (std::string(" ") + out + " ").c_str()), out);
  }
  // Happy: only the root's loose track (1 of 1).
  fa.recompute(idx, Filter::unpack(0x001));
  TEST_ASSERT_EQUAL_STRING("/music", heldFolders(idx, fa).c_str());
  // Every active q: as the rule says.
  int active = 0;
  for (uint16_t q = 0; q < 0x800; ++q) {
    if (!moodfilter::valid(q) || !Filter::unpack(q).active()) continue;
    ++active;
    const Filter f = Filter::unpack(q);
    TEST_ASSERT_TRUE(fa.recompute(idx, f) == FilteredAlbums::Result::Filtered);
    char msg[16];
    snprintf(msg, sizeof(msg), "q 0x%03X", q);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(wantedFolders(idx, f).c_str(), heldFolders(idx, fa).c_str(), msg);
  }
  TEST_ASSERT_EQUAL_INT(4 * 4 * 3 * 8 * 3 - 3, active);  // every group's choices, at three levels, less a level alone
}

// A filter that filters nothing holds nothing (the list shows albumsAZ()
// itself), a level alone included; one taken off lets its block go, and
// so does drop() and the end.
void test_filtered_albums_unfiltered_and_dropped() {
  static LibraryIndex idx;
  buildMoodLibrary(idx);
  allocs = frees = 0;
  {
    FilteredAlbums fa(countedAlloc, countedFree);
    for (uint16_t q : {0x000, 0x200, 0x400}) {
      TEST_ASSERT_TRUE(fa.recompute(idx, Filter::unpack(q)) == FilteredAlbums::Result::Unfiltered);
      TEST_ASSERT_FALSE(fa.held());
      TEST_ASSERT_EQUAL_UINT32(0, fa.count());
      TEST_ASSERT_EQUAL_size_t(0, fa.bytes());
    }
    TEST_ASSERT_EQUAL_INT(0, allocs);  // nothing carved for them
    fa.recompute(idx, kSadRelaxed);
    TEST_ASSERT_EQUAL_INT(1, allocs);
    TEST_ASSERT_TRUE(fa.recompute(idx, Filter::unpack(0x200)) == FilteredAlbums::Result::Unfiltered);
    TEST_ASSERT_EQUAL_INT(1, frees);
    TEST_ASSERT_EQUAL_UINT32(0, fa.total());
    TEST_ASSERT_EQUAL_HEX16(0, fa.word());
    fa.recompute(idx, kSadRelaxed);
    fa.drop();
    TEST_ASSERT_EQUAL_INT(2, frees);
    TEST_ASSERT_FALSE(fa.held());
    TEST_ASSERT_EQUAL_UINT32(0, fa.count());
    TEST_ASSERT_EQUAL_size_t(0, fa.bytes());
    fa.drop();  // (again: nothing to let go)
    TEST_ASSERT_EQUAL_INT(2, frees);
    fa.recompute(idx, kSadRelaxed);
  }
  TEST_ASSERT_EQUAL_INT(3, allocs);
  TEST_ASSERT_EQUAL_INT(3, frees);  // the last one at its end
  // An index not built: nothing.
  LibraryIndex none;
  FilteredAlbums fa;
  TEST_ASSERT_TRUE(fa.recompute(none, kSadRelaxed) == FilteredAlbums::Result::Unfiltered);
  TEST_ASSERT_FALSE(fa.held());
}

// No block for the albums: reported, and what was held stays, the old
// filter's (its owner keeps that filter: 4.2).
void test_filtered_albums_allocator_failure() {
  static LibraryIndex idx, plain;
  buildMoodLibrary(idx);
  buildPlainLibrary(plain);
  allocs = frees = 0;
  FilteredAlbums fa(countedAlloc, countedFree);
  failing = true;
  TEST_ASSERT_TRUE(fa.recompute(idx, kSadRelaxed) == FilteredAlbums::Result::NoMemory);
  TEST_ASSERT_FALSE(fa.held());
  TEST_ASSERT_EQUAL_size_t(0, fa.bytes());
  failing = false;
  TEST_ASSERT_TRUE(fa.recompute(idx, kSadRelaxed) == FilteredAlbums::Result::Filtered);
  // Another filter over the same albums needs no block: it can't fail.
  failing = true;
  TEST_ASSERT_TRUE(fa.recompute(idx, Filter::unpack(0x001)) == FilteredAlbums::Result::Filtered);
  TEST_ASSERT_TRUE(fa.recompute(idx, kSadRelaxed) == FilteredAlbums::Result::Filtered);
  // Another index (2 albums): no block, and the old one's albums stay.
  TEST_ASSERT_TRUE(fa.recompute(plain, kSadRelaxed) == FilteredAlbums::Result::NoMemory);
  TEST_ASSERT_TRUE(fa.held());
  TEST_ASSERT_EQUAL_UINT32(4, fa.count());
  TEST_ASSERT_EQUAL_UINT32(7, fa.total());
  TEST_ASSERT_EQUAL_HEX16(0x127, fa.word());
  TEST_ASSERT_EQUAL_STRING(wantedFolders(idx, kSadRelaxed).c_str(), heldFolders(idx, fa).c_str());
  TEST_ASSERT_EQUAL_INT(0, frees);
  failing = false;
}

// Worked out again: a new filter over the same albums in the same block;
// a rebuilt index with as many albums too, another count in a new block
// (the old one let go); an index without words lets nothing through.
void test_filtered_albums_recompute() {
  static LibraryIndex idx, plain;
  buildMoodLibrary(idx);
  buildPlainLibrary(plain);
  allocs = frees = 0;
  FilteredAlbums fa(countedAlloc, countedFree);
  fa.recompute(idx, kSadRelaxed);
  TEST_ASSERT_EQUAL_UINT32(4, fa.count());
  fa.recompute(idx, Filter::unpack(0x001));
  TEST_ASSERT_EQUAL_UINT32(1, fa.count());
  TEST_ASSERT_EQUAL_HEX16(0x001, fa.word());
  TEST_ASSERT_EQUAL_INT(1, allocs);
  // The same library built again: the same albums, the block kept.
  static LibraryIndex again;
  buildMoodLibrary(again);
  TEST_ASSERT_TRUE(fa.recompute(again, kSadRelaxed) == FilteredAlbums::Result::Filtered);
  TEST_ASSERT_EQUAL_STRING(wantedFolders(again, kSadRelaxed).c_str(), heldFolders(again, fa).c_str());
  TEST_ASSERT_EQUAL_INT(1, allocs);
  // No words: an active filter lets no album through (every track's word
  // is 0), in a block of the new count.
  TEST_ASSERT_FALSE(plain.hasMoods());
  TEST_ASSERT_TRUE(fa.recompute(plain, kSadRelaxed) == FilteredAlbums::Result::Filtered);
  TEST_ASSERT_TRUE(fa.held());
  TEST_ASSERT_EQUAL_UINT32(0, fa.count());
  TEST_ASSERT_EQUAL_UINT32(2, fa.total());
  TEST_ASSERT_EQUAL_size_t(2 * sizeof(uint32_t), fa.bytes());
  TEST_ASSERT_EQUAL_INT(2, allocs);
  TEST_ASSERT_EQUAL_INT(1, frees);
  TEST_ASSERT_TRUE(fa.recompute(plain, Filter()) == FilteredAlbums::Result::Unfiltered);
  TEST_ASSERT_EQUAL_INT(2, frees);
}

// What the Albums list shows (shown()): albumsAZ() until a filter's albums
// are held, then those; over an index with another album count (a rebuild
// not yet dropped) or one without words, albumsAZ() again.
void test_filtered_albums_shown() {
  static LibraryIndex idx, plain;
  buildMoodLibrary(idx);
  buildPlainLibrary(plain);
  FilteredAlbums fa;
  const LibraryIndex::Span az = idx.albumsAZ();
  TEST_ASSERT_EQUAL_PTR(az.ids, fa.shown(idx).ids);
  TEST_ASSERT_EQUAL_UINT32(7, fa.shown(idx).count);
  fa.recompute(idx, kSadRelaxed);
  const LibraryIndex::Span s = fa.shown(idx);
  TEST_ASSERT_EQUAL_UINT32(4, s.count);
  for (uint32_t i = 0; i < s.count; ++i) TEST_ASSERT_EQUAL_UINT32(fa[i], s[i]);
  TEST_ASSERT_EQUAL_PTR(plain.albumsAZ().ids, fa.shown(plain).ids);  // 2 albums, not 7
  TEST_ASSERT_EQUAL_UINT32(2, fa.shown(plain).count);
  // Held over the plain library: none fit, but with no words it shows them all.
  fa.recompute(plain, kSadRelaxed);
  TEST_ASSERT_TRUE(fa.held());
  TEST_ASSERT_EQUAL_UINT32(0, fa.count());
  TEST_ASSERT_EQUAL_UINT32(2, fa.shown(plain).count);
  // A filter none fits, over the words: no rows (the empty line).
  fa.recompute(idx, Filter::unpack(0x00F));  // Sad, Aggressive
  TEST_ASSERT_TRUE(fa.held());
  TEST_ASSERT_EQUAL_UINT32(0, fa.count());
  TEST_ASSERT_EQUAL_UINT32(0, fa.shown(idx).count);
  fa.drop();
  TEST_ASSERT_EQUAL_UINT32(7, fa.shown(idx).count);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_second_tap_lets_go);
  RUN_TEST(test_tags_any_number);
  RUN_TEST(test_a_level_alone_is_inactive);
  RUN_TEST(test_clear_resets_the_level);
  RUN_TEST(test_equal_widths);
  RUN_TEST(test_proportional_widths);
  RUN_TEST(test_a_half_rounds_down);
  RUN_TEST(test_small_and_scaled_widths);
  RUN_TEST(test_a_tag_wraps_and_the_chips_give_up_an_overflow_evenly);
  RUN_TEST(test_every_panel_point_is_one_cell_at_most);
  RUN_TEST(test_splits_skew_right);
  RUN_TEST(test_title_row_hits);
  RUN_TEST(test_first_row_zones);
  RUN_TEST(test_the_dot);
  RUN_TEST(test_summary_order);
  RUN_TEST(test_count_forms);
  RUN_TEST(test_filtered_albums_are_a_subsequence);
  RUN_TEST(test_filtered_albums_by_majority);
  RUN_TEST(test_filtered_albums_unfiltered_and_dropped);
  RUN_TEST(test_filtered_albums_allocator_failure);
  RUN_TEST(test_filtered_albums_recompute);
  RUN_TEST(test_filtered_albums_shown);
  return UNITY_END();
}
