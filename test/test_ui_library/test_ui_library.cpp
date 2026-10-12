// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Library screens' portable pieces: the A-Z jump grid
// and its second level (JumpIndex, textfold::secondKey), a list's step a
// frame (ListLayout::stepToward), and every text the tab bar can show
// measured with the real fonts (the VLW DejaVu data the firmware draws
// with) against the room TabBarModel gives it.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BootLayout.h"
#include "CardContract.h"
#include "CardTags.h"
#include "CjkGlyphs16.h"
#include "DeviceInfo.h"
#include "GlyphStore.h"
#include "HostStatus.h"
#include "IdlePolicy.h"
#include "JumpIndex.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "LibraryText.h"
#include "ListLayout.h"
#include "NameKey.h"
#include "OutputModel.h"
#include "PowerChoices.h"
#include "QueueView.h"
#include "RateConverter.h"
#include "ScreenPower.h"
#include "SeekBar.h"
#include "SheetLayout.h"
#include "SleepTimer.h"
#include "TabBarModel.h"
#include "TagScan.h"
#include "TagText.h"
#include "TextFit.h"
#include "TextFold.h"
#include "TitleLayout.h"
#include "TouchCalibration.h"
#include "TouchCheck.h"
#include "TrackCatalog.h"
#include "UiStrings.h"
#include "UiText.h"
#include "../support/TagFixtures.h"

// The fonts' data, as the firmware has it (src/ui/VlwFonts.cpp: arrays only).
#include "../../src/ui/VlwFonts.cpp"

// The texts are the catalogue's (i18n/en.json, English here): uistr::tr().
using uistr::S;

namespace {
// Line k of a message whose lines the layout places one by one.
std::string lineOf(S id, int k) {
  char b[256];
  uistr::line(uistr::tr(id), k, b, sizeof(b));
  return b;
}
// The ten languages, English first: a test that measures each sets it
// (tearDown() puts English back); one with no catalogue draws English.
constexpr uistr::Lang kAllLangs[] = {uistr::Lang::En, uistr::Lang::De, uistr::Lang::Es, uistr::Lang::Fr,
                                     uistr::Lang::It, uistr::Lang::Ja, uistr::Lang::Pl, uistr::Lang::Pt,
                                     uistr::Lang::Ru, uistr::Lang::Zh};
}  // namespace

void setUp() { uistr::setLang(uistr::Lang::En); }  // (a test that changes it may fail half-way)
void tearDown() { uistr::setLang(uistr::Lang::En); }

namespace {

// ---- a list sorted as the index sorts ----

std::vector<std::string> sorted(std::vector<std::string> names) {
  std::sort(names.begin(), names.end(),
            [](const std::string& a, const std::string& b) { return textfold::compare(a.c_str(), b.c_str()) < 0; });
  return names;
}

const char* nameOf(void* ctx, uint32_t row) { return (*static_cast<std::vector<std::string>*>(ctx))[row].c_str(); }

int bucketOfRow(const std::vector<std::string>& v, uint32_t row) {
  return textfold::bucketOf(textfold::railKey(v[row].c_str()));
}

// ---- the glyph store behind the fonts (ui/Fonts: docs/I18N.md, phase 2) ----

const glyphstore::Store& cjk() {
  static const glyphstore::Store store = [] {
    glyphstore::Store s;
    s.open(kCjkGlyphs16, kCjkGlyphs16Size);
    return s;
  }();
  return store;
}

// ---- the VLW fonts' metrics (what ui/Fonts measures with) ----

struct Vlw {
  std::vector<std::pair<uint32_t, int>> advance;  // code point, xAdvance
  struct Box {
    uint32_t cp;
    int height, top;  // rows, and the top's height above the baseline (dY)
  };
  std::vector<Box> boxes;
  int ascent = 0, descent = 0;  // the header's
  int lineH = 0;  // the ascent and the descent (the header's)
  int space = 0;  // what a missing glyph measures (M5GFX's spaceWidth)
  // M5GFX's line height (VLWfont::loadFont, what ui/Fonts::height() gives)
  // and baseline: the header's ascent and descent, grown by any glyph of
  // ASCII or above U+00FF that reaches further.
  int maxAscent = 0, yAdvance = 0;
  // The Bold and Title roles' fonts (DejaVu Sans Bold) draw the store's
  // glyphs bold: a pixel further (glyphstore::boldAdvance()).
  bool boldCjk = false;
  explicit Vlw(const uint8_t* d) : boldCjk(d == kVlwSansBold16 || d == kVlwSansBold22) {
    auto be32 = [&](size_t at) {
      return static_cast<uint32_t>(d[at]) << 24 | static_cast<uint32_t>(d[at + 1]) << 16 |
             static_cast<uint32_t>(d[at + 2]) << 8 | d[at + 3];
    };
    const uint32_t n = be32(0);
    ascent = static_cast<int>(be32(16));
    descent = static_cast<int>(be32(20));
    lineH = ascent + descent;
    // VLWfont::loadFont: the larger of the size and the ascent + descent,
    // times 2 / 7, before the glyphs' metrics move the line height.
    space = std::max(static_cast<int>(be32(8)), lineH) * 2 / 7;
    int maxDescent = descent;
    maxAscent = ascent;
    for (uint32_t i = 0; i < n; ++i) {
      const size_t g = 24 + static_cast<size_t>(i) * 28;
      const uint32_t cp = be32(g);
      const int h = static_cast<int>(be32(g + 4));
      const int top = static_cast<int>(static_cast<int32_t>(be32(g + 16)));
      advance.push_back({cp, static_cast<int>(be32(g + 12))});
      boxes.push_back({cp, h, top});
      if (cp > 0xFF || (cp > 0x20 && cp < 0xA0 && cp != 0x7F)) {
        maxAscent = std::max(maxAscent, top);
        maxDescent = std::max(maxDescent, h - top);
      }
    }
    yAdvance = maxAscent + maxDescent;
  }
  int find(uint32_t cp) const {
    for (const auto& a : advance) {
      if (a.first == cp) return a.second;
    }
    return -1;
  }
  // The font's glyph, else the glyph store's (as ui/Fonts: VLW first).
  bool has(uint32_t cp) const { return find(cp) >= 0 || cjk().contains(cp); }
  // Every character has a glyph (the firmware draws a missing one folded).
  bool hasAll(const char* s) const {
    for (const char* p = s; *p;) {
      if (!has(textfold::decode(p))) return false;
    }
    return true;
  }
  // As ui/Fonts measures: the font's advance, else the store's (bold in
  // the Bold and Title roles), else (a missing glyph) the space width.
  int width(const char* s) const {
    int w = 0;
    for (const char* p = s; *p;) {
      const uint32_t cp = textfold::decode(p);
      int a = find(cp);
      if (a < 0) {
        a = cjk().advance(cp);
        if (boldCjk) a = glyphstore::boldAdvance(a);
      }
      w += a >= 0 ? a : space;
    }
    return w;
  }
  // A glyph the store draws in this font: one the font lacks and the store
  // has (ui/Fonts::fromStore()'s test).
  bool fromStore(uint32_t cp) const { return find(cp) < 0 && cjk().contains(cp); }
};

}  // namespace

// ---- JumpIndex ----

void test_jump_letters_match_the_index_buckets() {
  // The synthetic library at 10,000 tracks: 600 artists, their accents,
  // digits and "The " included. The rows' rail names are their sort names
  // (LibraryPage::railName(): "The X" under X), as the index sorts them.
  LibraryIndex idx;
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  std::vector<std::string> names;
  for (uint32_t i = 0; i < idx.artistCount(); ++i) {
    names.push_back(textfold::sortName(idx.artistName(idx.artistsAZ()[i])));
  }
  int32_t first[jump::kCells], end[jump::kCells];
  jump::letters(static_cast<uint32_t>(names.size()), nameOf, &names, first, end);
  for (int b = 0; b < jump::kCells; ++b) {
    const uint32_t start = idx.bucketStart(LibraryIndex::View::Artists, b);
    const uint32_t stop = idx.bucketStart(LibraryIndex::View::Artists, b + 1);
    if (start == stop) {
      TEST_ASSERT_EQUAL_INT32(-1, first[b]);
      continue;
    }
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(start), first[b]);
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(stop), end[b]);
  }
}

void test_jump_second_level() {
  const std::vector<std::string> v = sorted({"Kanye West", "K-Os", "Kavinsky", "Kaiser Chiefs", "Keane", "Kings of Leon",
                                             "Klaxons", "Kraftwerk", "K7", "Kiss", "Kasabian", "Kéane Two", "Air",
                                             "Zola", "Kæthe"});
  auto vv = v;
  int32_t first[jump::kCells], end[jump::kCells];
  jump::letters(static_cast<uint32_t>(vv.size()), nameOf, &vv, first, end);
  const int k = textfold::bucketOf('K');
  TEST_ASSERT_TRUE(first[k] >= 0);
  int32_t second[jump::kCells];
  jump::seconds(static_cast<uint32_t>(first[k]), static_cast<uint32_t>(end[k]), nameOf, &vv, second);
  // Cell 0: the letter itself (its first row: "K-Os", "K7" sort first).
  TEST_ASSERT_EQUAL_INT32(first[k], second[0]);
  // Every cell's row is the first with that second letter; the rest none.
  for (int c = 1; c < jump::kCells; ++c) {
    const char want = static_cast<char>('a' + c - 1);
    int32_t brute = -1;
    for (int32_t r = first[k]; r < end[k]; ++r) {
      if (textfold::secondKey(vv[static_cast<size_t>(r)].c_str()) == want) {
        brute = r;
        break;
      }
    }
    TEST_ASSERT_EQUAL_INT32_MESSAGE(brute, second[c], vv[static_cast<size_t>(std::max(0, brute))].c_str());
  }
  TEST_ASSERT_TRUE(second[textfold::bucketOf('A')] >= 0);  // Kanye, Kaiser, Kavinsky, Kasabian, Kæthe
  TEST_ASSERT_TRUE(second[textfold::bucketOf('E')] >= 0);  // Keane, Kéane
  TEST_ASSERT_EQUAL_INT32(-1, second[textfold::bucketOf('B')]);
  // "Kæthe" folds to "kaethe": the first "Ka", before "Kaiser Chiefs".
  TEST_ASSERT_EQUAL_STRING("Kæthe", vv[static_cast<size_t>(second[1])].c_str());
}

void test_jump_second_key() {
  TEST_ASSERT_EQUAL_CHAR('a', textfold::secondKey("Kavinsky"));
  TEST_ASSERT_EQUAL_CHAR('a', textfold::secondKey("KANYE"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K-Os"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K7"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey("K"));
  TEST_ASSERT_EQUAL_CHAR('#', textfold::secondKey(""));
  TEST_ASSERT_EQUAL_CHAR('m', textfold::secondKey("Émilie Simon"));
  TEST_ASSERT_EQUAL_CHAR('e', textfold::secondKey("Æther"));  // Æ folds to "AE"
}

void test_jump_second_level_on_a_big_library() {
  // Brute force against the grid on every letter of 600 synthetic artists.
  LibraryIndex idx;
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  std::vector<std::string> names;
  for (uint32_t i = 0; i < idx.albumCount(); ++i) {
    names.push_back(textfold::sortName(idx.albumName(idx.albumsAZ()[i])));
  }
  int32_t first[jump::kCells], end[jump::kCells], second[jump::kCells];
  jump::letters(static_cast<uint32_t>(names.size()), nameOf, &names, first, end);
  int big = 0;
  for (int b = 0; b < jump::kCells; ++b) {
    if (first[b] < 0) continue;
    if (static_cast<uint32_t>(end[b] - first[b]) > jump::kSecondLevelRows) ++big;
    jump::seconds(static_cast<uint32_t>(first[b]), static_cast<uint32_t>(end[b]), nameOf, &names, second);
    for (int c = 1; c < jump::kCells; ++c) {
      int32_t brute = -1;
      for (int32_t r = first[b]; r < end[b]; ++r) {
        if (textfold::bucketOf(textfold::secondKey(names[static_cast<size_t>(r)].c_str())) == c) {
          brute = r;
          break;
        }
      }
      TEST_ASSERT_EQUAL_INT32(brute, second[c]);
    }
    for (int32_t r = first[b]; r < end[b]; ++r) TEST_ASSERT_EQUAL_INT(b, bucketOfRow(names, static_cast<uint32_t>(r)));
  }
  TEST_ASSERT_TRUE(big > 5);  // 1,500 albums: most letters get the second level
}

void test_jump_grid_cells() {
  // 7 x 4 cells of 44 x 41 from (6, 74): '#' A-F, G-M, N-T, U-Z and the last.
  TEST_ASSERT_EQUAL_INT(0, jump::cellAt(6, 74, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(6, jump::cellAt(313, 74, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(7, jump::cellAt(6, 115, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(27, jump::cellAt(300, 237, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(5, 100, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(100, 238, 6, 74, 44, 41));
  TEST_ASSERT_EQUAL_INT(-1, jump::cellAt(314, 100, 6, 74, 44, 41));
}

// ---- a list's step a frame ----

void test_list_steps_toward_the_finger() {
  // A finger faster than a step a frame: the list trails it by a step...
  TEST_ASSERT_EQUAL_INT32(184, ListLayout::stepToward(100, 400, 84));
  TEST_ASSERT_EQUAL_INT32(16, ListLayout::stepToward(100, -50, 84));
  // ... and is there once it's within one.
  TEST_ASSERT_EQUAL_INT32(150, ListLayout::stepToward(100, 150, 84));
  TEST_ASSERT_EQUAL_INT32(16, ListLayout::stepToward(100, 16, 84));
  // A fling of 2,000 px/s at 30 fps: under a step, never capped.
  int32_t drawn = 0, finger = 0;
  for (int f = 0; f < 30; ++f) {
    finger += 2000 / 30;
    drawn = ListLayout::stepToward(drawn, finger, 84);
    TEST_ASSERT_EQUAL_INT32(finger, drawn);
  }
  // 4,000 px/s for 10 frames: behind by at most what it moved over the
  // cap, and caught up within two frames of the finger stopping.
  drawn = finger = 0;
  for (int f = 0; f < 10; ++f) {
    finger += 4000 / 30;
    drawn = ListLayout::stepToward(drawn, finger, 84);
  }
  TEST_ASSERT_EQUAL_INT32(840, drawn);
  int frames = 0;
  while (drawn != finger) {
    drawn = ListLayout::stepToward(drawn, finger, 84);
    ++frames;
  }
  TEST_ASSERT_TRUE(frames <= 7);
}

// In whole rows (the governor, the audio short of time), from an offset
// that isn't on a row: the step never grows past the cap (it used to round
// after capping: 167 -> 83 -> 42, a 125-line step, a full redraw).
void test_list_steps_in_whole_rows_never_exceed_the_cap() {
  const int32_t pitch = ListLayout::kPitch, cap = 84;
  TEST_ASSERT_EQUAL_INT32(84, ListLayout::stepTowardRows(167, 0, cap, pitch));
  for (int32_t drawn = 0; drawn < 2000; drawn += 7) {
    for (int32_t target = 0; target < 2000; target += 13) {
      const int32_t off = ListLayout::stepTowardRows(drawn, target, cap, pitch);
      const int32_t step = off > drawn ? off - drawn : drawn - off;
      TEST_ASSERT_TRUE(step <= cap);
      // Toward the target, never past its row; on a whole row.
      const int32_t t = target / pitch * pitch;
      TEST_ASSERT_EQUAL_INT32(0, off % pitch);
      if (t >= drawn) {
        TEST_ASSERT_TRUE(off <= t);
      } else {
        TEST_ASSERT_TRUE(off >= t);
      }
      // It gets there: every step moves unless it's there already.
      if (off == drawn) TEST_ASSERT_EQUAL_INT32(t, off);
    }
  }
}

// ---- the tab bar's texts ----

void test_tabbar_texts_fit_in_every_state() {
  const Vlw small(kVlwSans13);
  // The active tab's label on its plate, every tab (English).
  for (int t = 0; t < tabbar::kTabs; ++t) {
    TEST_ASSERT_TRUE_MESSAGE(small.width(tabbar::label(t)) <= tabbar::labelWidth(t), tabbar::label(t));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::Plate, tabbar::labelFit(t, small.width(tabbar::label(t))));
  }
  // Wider than its plate, the Library's and the Queue's label spills over
  // the plate's edges, centred under the icon, up to kLabelSpillW: clear of
  // what the neighbours draw in the label row (screen x; the icons as
  // ui/TabBar draws them, centred, the Queue's 3 px left), and below the
  // Queue's badge (y 1-14) and the sleep timer's moon (y 4-10).
  {
    using namespace tabbar;
    const int lib = cellX0(1) + kTabW / 2, queue = cellX0(2) + kTabW / 2;  // 81, 135
    const int hairlineRight = cellX0(0) + kTabW / 2 - kProgressW / 2 + kProgressW - 1;  // 42
    const int libIconRight = lib - kLibraryIconW / 2 + kLibraryIconW - 1;            // 93
    const int queueIconLeft = queue - kQueueIconShift - kQueueIconW / 2;              // 120
    const int crabLeft = cellX0(3) + kTabW / 2 - kCrabIconW / 2;                      // 178
    TEST_ASSERT_TRUE(lib - kLabelSpillW / 2 >= hairlineRight + 2);
    TEST_ASSERT_TRUE(lib - kLabelSpillW / 2 + kLabelSpillW - 1 <= queueIconLeft - 2);
    TEST_ASSERT_TRUE(queue - kLabelSpillW / 2 >= libIconRight + 2);
    TEST_ASSERT_TRUE(queue - kLabelSpillW / 2 + kLabelSpillW - 1 <= crabLeft - 2);
    TEST_ASSERT_TRUE(27 - small.yAdvance / 2 > 14);  // the label row's top (centred on y 27)
    TEST_ASSERT_TRUE(27 - small.yAdvance / 2 + small.yAdvance - 1 <= kHeight - 1);  // its last row the bar's rule's (drawn after)
    TEST_ASSERT_TRUE(spills(1) && spills(2) && !spills(0) && !spills(3) && !spills(4));
    TEST_ASSERT_EQUAL_INT(kLabelSpillW, labelMax(1));
    TEST_ASSERT_EQUAL_INT(kLabelW, labelMax(0));
    TEST_ASSERT_EQUAL_INT(kOutputPlateW, labelMax(4));
    TEST_ASSERT_EQUAL(LabelFit::Plate, labelFit(1, kLabelW));
    TEST_ASSERT_EQUAL(LabelFit::Spill, labelFit(1, kLabelW + 1));
    TEST_ASSERT_EQUAL(LabelFit::Spill, labelFit(2, kLabelSpillW));
    TEST_ASSERT_EQUAL(LabelFit::None, labelFit(2, kLabelSpillW + 1));
    TEST_ASSERT_EQUAL(LabelFit::None, labelFit(0, kLabelW + 1));  // Playing, Dance: their plate only
    TEST_ASSERT_EQUAL(LabelFit::Plate, labelFit(4, kOutputPlateW));
  }
  // Each language's labels: where each goes. Those that spill (German's
  // "Bibliothek"), those shown as the icon alone (Japanese's and Russian's
  // Library, German's Queue).
  {
    using L = uistr::Lang;
    std::string report;
    for (L l : kAllLangs) {
      if (!uistr::hasCatalogue(l)) continue;
      uistr::setLang(l);
      for (int t = 0; t < tabbar::kTabs; ++t) {
        const tabbar::LabelFit fit = tabbar::labelFit(t, small.width(tabbar::label(t)));
        TEST_ASSERT_TRUE(tabbar::spills(t) || fit != tabbar::LabelFit::Spill);
        if (fit == tabbar::LabelFit::Plate) continue;
        char m[96];
        snprintf(m, sizeof(m), "%s %s %d px: %s; ", uistr::tag(l), tabbar::label(t), small.width(tabbar::label(t)),
                 fit == tabbar::LabelFit::Spill ? "spills" : "icon alone");
        report += m;
      }
    }
    TEST_MESSAGE(report.c_str());
    auto fitIn = [&](L l, int t) {
      uistr::setLang(l);
      return tabbar::labelFit(t, small.width(tabbar::label(t)));
    };
    TEST_ASSERT_EQUAL(tabbar::LabelFit::Spill, fitIn(L::De, 1));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::None, fitIn(L::De, 2));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::None, fitIn(L::Ru, 1));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::Spill, fitIn(L::Ru, 2));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::None, fitIn(L::Ja, 1));
    TEST_ASSERT_EQUAL(tabbar::LabelFit::Plate, fitIn(L::Zh, 1));
    uistr::setLang(L::En);
  }
  // The volume beside the output icon and the battery under its icon, at
  // every level (0-100 %).
  char t[8];
  for (int v = 0; v <= 100; ++v) {
    snprintf(t, sizeof(t), "%d%%", v);
    TEST_ASSERT_TRUE_MESSAGE(small.width(t) <= tabbar::kVolumeW, t);
    TEST_ASSERT_TRUE_MESSAGE(small.width(t) <= tabbar::kBatteryTextW, t);
  }
  // The volume ends before the battery icon starts.
  TEST_ASSERT_TRUE(tabbar::kVolumeX + tabbar::kVolumeW <= tabbar::kBatteryX + 2);
  // The battery's text inside the cell (centred under the icon).
  const int bx = tabbar::kBatteryX + 12;
  TEST_ASSERT_TRUE(bx + tabbar::kBatteryTextW / 2 <= tabbar::cellX1(tabbar::kTabs - 1) - tabbar::kOutputX + 1);
  // The Queue's badge, every count: inside its cell.
  for (uint32_t n = 0; n < 200; ++n) {
    char b[4];
    tabbar::badgeText(n, b);
    const int bw = small.width(b) + tabbar::kBadgePad;
    const int left = tabbar::kTabW / 2 + tabbar::kBadgeRight - bw;
    TEST_ASSERT_TRUE_MESSAGE(left >= 0 && tabbar::kTabW / 2 + tabbar::kBadgeRight <= tabbar::kTabW, b);
  }
}

// ---- the other fixed texts (UiText: each next to its room) ----

namespace {
// It fits, and the font has every glyph of it: one it lacks would be drawn
// folded ('?' for a letter of another script), so a text that fits by its
// widths alone (a translation, say) could still not be what is drawn.
void fits(const Vlw& font, const char* text, int room) {
  char msg[160];
  snprintf(msg, sizeof(msg), "\"%s\": %d px in %d", text, font.width(text), room);
  TEST_ASSERT_TRUE_MESSAGE(font.width(text) <= room, msg);
  snprintf(msg, sizeof(msg), "\"%s\": a glyph the font doesn't have", text);
  TEST_ASSERT_TRUE_MESSAGE(font.hasAll(text), msg);
}

// It fits in `font`, or else in `smaller` (the label's fallback: a
// button's, a pill's, as ui/Fonts::fitting() picks).
void fitsIn(const Vlw& font, const Vlw& smaller, const char* text, int room) {
  fits(font.width(text) <= room ? font : smaller, text, room);
}

}  // namespace

// ---- names as the screen draws them (docs/I18N.md, phase 0) ----

namespace {

int vlwWidth(void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); }
bool vlwHas(void* ctx, uint32_t cp) { return static_cast<const Vlw*>(ctx)->has(cp); }
bool vlwFromStore(void* ctx, uint32_t cp) { return static_cast<const Vlw*>(ctx)->fromStore(cp); }

// The TextFit view of a font, as ui/Fonts::fit() makes it.
textfit::Font viewOf(const Vlw& v) {
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&v);
  f.width = vlwWidth;
  f.has = vlwHas;
  return f;
}

// ui/Fonts::fromStore(): a glyph from the store in the text as drawn.
bool fromStore(const Vlw& v, const char* s) {
  return textfit::drawnAny(viewOf(v), s, vlwFromStore, const_cast<Vlw*>(&v));
}

// `name` as ui/Fonts::draw fits it (its 200-byte buffer), in `font`.
std::string drawn(const Vlw& font, const char* name, int maxW = 300) {
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&font);
  f.width = vlwWidth;
  f.has = vlwHas;
  char buf[200];
  textfit::fit(f, name, std::strlen(name), buf, sizeof(buf), maxW);
  return buf;
}

}  // namespace

// TextFit with the fonts the firmware draws with: decomposed text composed
// first, a cp1252 byte (U+0080-009F) as its character, Greek, Cyrillic,
// Romanian's and Pinyin's letters drawn as they are (phase 1), Chinese,
// Japanese and Korean from the glyph store (phase 2), the Latin letters the
// fonts still lack (Vietnamese's) as their base letters; the scripts with
// no glyphs yet stay '?'. The tests measure a missing glyph as the firmware
// does: the space width M5GFX gives it, 5 px in DejaVu Sans 16.
void test_names_drawn_with_the_fonts() {
  const Vlw body(kVlwSans16), small(kVlwSans13);
  TEST_ASSERT_EQUAL_INT(5, body.space);
  TEST_ASSERT_EQUAL_INT(4, small.space);
  TEST_ASSERT_EQUAL_INT(body.width("a") + body.space + body.width("b"), body.width("a\xD5\x80" "b"));  // Հ
  TEST_ASSERT_EQUAL_INT(body.width("a") + 16 + body.width("b"), body.width("a\xE4\xB8\xAD" "b"));  // 中: the store's
  TEST_ASSERT_FALSE(body.hasAll("Հ"));
  TEST_ASSERT_TRUE(body.hasAll("中"));
  TEST_ASSERT_TRUE(body.hasAll("Ж"));
  TEST_ASSERT_TRUE(body.hasAll("Café ’…€"));
  TEST_ASSERT_EQUAL_STRING("Don’t Stop – Live…", drawn(body, "Don\xC2\x92t Stop \xC2\x96 Live\xC2\x85").c_str());
  TEST_ASSERT_EQUAL_STRING("€ ™ Š Ÿ “x”", drawn(body, "\xC2\x80 \xC2\x99 \xC2\x8A \xC2\x9F \xC2\x93x\xC2\x94").c_str());
  TEST_ASSERT_EQUAL_STRING("f^~?", drawn(body, "\xC2\x83\xC2\x88\xC2\x98\xC2\x81").c_str());  // no glyph: folded
  TEST_ASSERT_EQUAL_STRING("Beyoncé", drawn(body, "Beyonce\xCC\x81").c_str());
  TEST_ASSERT_EQUAL_STRING("Ștefan Bănică", drawn(body, "Ștefan Bănică").c_str());
  TEST_ASSERT_EQUAL_STRING("Ştefan", drawn(body, "Ştefan").c_str());  // the cedilla spelling, Latin Extended-A's
  TEST_ASSERT_EQUAL_STRING("My Tâm", drawn(body, "Mỹ Tâm").c_str());
  TEST_ASSERT_EQUAL_STRING("My Tâm", drawn(body, "My\xCC\x83 Ta\xCC\x82m").c_str());  // NFD: â is the font's
  TEST_ASSERT_EQUAL_STRING("ABC", drawn(body, "ＡＢＣ").c_str());
  TEST_ASSERT_EQUAL_STRING("Кино", drawn(body, "Кино").c_str());
  TEST_ASSERT_EQUAL_STRING("Χάρις Αλεξίου", drawn(body, "Χάρις Αλεξίου").c_str());
  TEST_ASSERT_EQUAL_STRING("Ђорђе Балашевић", drawn(body, "Ђорђе Балашевић").c_str());
  TEST_ASSERT_EQUAL_STRING("Łódź Wołanie", drawn(body, "Łódź Wołanie").c_str());
  TEST_ASSERT_EQUAL_STRING("Lǚ", drawn(body, "Lǚ").c_str());
  // NFD Greek and Cyrillic compose to the fonts' letters: Χάρις, Ёлка, Й.
  TEST_ASSERT_EQUAL_STRING("Χάρις", drawn(body, "Χα\xCC\x81ρις").c_str());
  TEST_ASSERT_EQUAL_STRING("Ёлка Й", drawn(body, "Е\xCC\x88лка И\xCC\x86").c_str());
  // Chinese, Japanese and Korean from the glyph store (phase 2); NFD
  // Hangul composed to the store's syllables; halfwidth katakana as the
  // fullwidth.
  TEST_ASSERT_EQUAL_STRING("中文", drawn(body, "中文").c_str());
  TEST_ASSERT_EQUAL_STRING("王菲", drawn(body, "王菲").c_str());
  TEST_ASSERT_EQUAL_STRING("周杰倫", drawn(small, "周杰倫").c_str());
  TEST_ASSERT_EQUAL_STRING("宇多田ヒカル", drawn(body, "宇多田ヒカル").c_str());
  TEST_ASSERT_EQUAL_STRING("坂本龍一", drawn(body, "坂本龍一").c_str());
  TEST_ASSERT_EQUAL_STRING("아이유", drawn(body, "아이유").c_str());
  TEST_ASSERT_EQUAL_STRING("한글", drawn(body, "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB\xE1\x84\x80\xE1\x85\xB3\xE1\x86\xAF").c_str());
  TEST_ASSERT_EQUAL_STRING("カタカナ", drawn(body, "ｶﾀｶﾅ").c_str());
  TEST_ASSERT_EQUAL_STRING("ガ パ ア゛", drawn(body, "ｶﾞ ﾊﾟ ｱﾞ").c_str());  // no ア with the mark: the spacing ゛
  TEST_ASSERT_EQUAL_STRING("「東京」 ・ ㄱ ー", drawn(body, "「東京」 ・ ㄱ ー").c_str());
  TEST_ASSERT_EQUAL_STRING("Tokyo 東京事変 – Live", drawn(body, "Tokyo 東京事変 \xE2\x80\x93 Live").c_str());
  TEST_ASSERT_EQUAL_INT(16 * 6, body.width("宇多田ヒカル"));  // Han and kana 16 px, Hangul 15
  TEST_ASSERT_EQUAL_INT(15 * 3, body.width("아이유"));
  // Cut at a character to the room, with the font's "…".
  const std::string cjkCut = drawn(body, "東京事変の夜明けのスキャット", 100);
  TEST_ASSERT_EQUAL_STRING("東京事変の…", cjkCut.c_str());
  TEST_ASSERT_TRUE(body.width(cjkCut.c_str()) <= 100);
  // Scripts with no glyphs yet: '?' a letter, as before; the store's set
  // has KS X 1001's Hangul and JIS X 0208 and GB2312's level 1, not all of them.
  TEST_ASSERT_EQUAL_STRING("???", drawn(body, "Հայ").c_str());  // Armenian
  TEST_ASSERT_EQUAL_STRING("?", drawn(body, "똠").c_str());     // a Hangul syllable outside KS X 1001
  TEST_ASSERT_EQUAL_STRING("?", drawn(body, "\xF0\xA0\x80\x80").c_str());  // U+20000, past the BMP
  TEST_ASSERT_EQUAL_STRING("Ninja ? Beats", drawn(body, "Ninja 🥷 Beats").c_str());
  TEST_ASSERT_EQUAL_STRING("?", drawn(body, "\xE2\x9D\xA4\xEF\xB8\x8F").c_str());  // ❤ + VS16: one '?'
  // Cut to the room as before, the composed text measured.
  const std::string cut = drawn(body, "Beyonce\xCC\x81 Beyonce\xCC\x81 Beyonce\xCC\x81", 100);
  TEST_ASSERT_TRUE(body.width(cut.c_str()) <= 100);
  TEST_ASSERT_TRUE(body.hasAll(cut.c_str()));
  TEST_ASSERT_EQUAL_STRING("Beyoncé…", cut.c_str());  // whole letters, the é one glyph
}

// What a page measures (ui/Fonts::width(): textfit::drawnWidth()) is what
// is drawn: a halfwidth or NFD name as its composed glyphs, a missing glyph
// as its fold. Measured raw, a halfwidth kana or a jamo is a 4-5 px blank,
// a third of what the store draws: a path kept whole that the draw then
// cuts at its end, an album line given its year and then cut.
void test_names_measured_as_drawn() {
  const Vlw body(kVlwSans16), small(kVlwSans13);
  auto view = [](const Vlw& v) {
    textfit::Font f;
    f.ctx = const_cast<Vlw*>(&v);
    f.width = vlwWidth;
    f.has = vlwHas;
    return f;
  };
  const char* nfdPath =
      "/music/\xE1\x84\x8B\xE1\x85\xA1\xE1\x84\x8B\xE1\x85\xB5\xE1\x84\x8B\xE1\x85\xB2/"  // 아이유
      "\xE1\x84\x89\xE1\x85\xA5\xE1\x84\x8B\xE1\x85\xAE\xE1\x86\xAF\xE1\x84\x8B\xE1\x85\xB4"  // 서울의
      " \xE1\x84\x87\xE1\x85\xA1\xE1\x86\xB7";                                              // 밤
  const char* names[] = {"/music/ｱﾆﾒ/ｻｳﾝﾄﾞﾄﾗｯｸ", "ﾀﾞﾌﾞﾙﾌｧﾝﾀｼﾞｰ · 2002", nfdPath, "Beyonce\xCC\x81",
                         "Mỹ Tâm", "Հայ", "Ninja 🥷 Beats", "Don\xC2\x92t", "Tokyo 東京事変 – Live"};
  for (const Vlw* v : {&body, &small}) {
    for (const char* n : names) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(v->width(drawn(*v, n, 10000).c_str()), textfit::drawnWidth(view(*v), n), n);
    }
  }
  TEST_ASSERT_EQUAL_INT(99, small.width("/music/ｱﾆﾒ/ｻｳﾝﾄﾞﾄﾗｯｸ"));  // raw: the 12 halfwidth ones blank
  TEST_ASSERT_EQUAL_INT(227, textfit::drawnWidth(view(small), "/music/ｱﾆﾒ/ｻｳﾝﾄﾞﾄﾗｯｸ"));
  // The Folders header and Go to folder: the last folders kept, as for
  // the fullwidth path.
  char out[160];
  textfit::cutPathLeft(view(small), "/music/ｱﾆﾒ/ｻｳﾝﾄﾞﾄﾗｯｸ", 200, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("…/ｱﾆﾒ/ｻｳﾝﾄﾞﾄﾗｯｸ", out);
  TEST_ASSERT_EQUAL_STRING("…/アニメ/サウンドトラック", drawn(small, out, 200).c_str());  // whole
  textfit::cutPathLeft(view(small), nfdPath, 150, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("…/아이유/서울의 밤", drawn(small, out, 150).c_str());
  // Now Playing's album line (190 px of Body): the year only when it fits.
  TEST_ASSERT_EQUAL_INT(199, textfit::drawnWidth(view(body), "ﾀﾞﾌﾞﾙﾌｧﾝﾀｼﾞｰ · 2002"));
}

// The Bold and Title roles draw the store's glyphs bold, a pixel wider
// (docs/I18N.md, phase 2), and measure them so: a Bold label, the current
// row, a cut, all as wide as drawn. Body and Small as before; the VLW
// glyphs unchanged in every role.
void test_store_glyphs_bold_in_bold_and_title() {
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  TEST_ASSERT_FALSE(body.boldCjk || small.boldCjk);
  TEST_ASSERT_TRUE(bold.boldCjk && title.boldCjk);
  TEST_ASSERT_EQUAL_INT(16 * 4, body.width("東京事変"));
  TEST_ASSERT_EQUAL_INT(16 * 4, small.width("東京事変"));
  TEST_ASSERT_EQUAL_INT(17 * 4, bold.width("東京事変"));
  TEST_ASSERT_EQUAL_INT(17 * 4, title.width("東京事変"));
  TEST_ASSERT_EQUAL_INT(15 * 3, body.width("아이유"));
  TEST_ASSERT_EQUAL_INT(16 * 3, bold.width("아이유"));
  TEST_ASSERT_EQUAL_INT(17, bold.width("\xE3\x80\x80"));  // the ideographic space moves the pen: a pixel more
  TEST_ASSERT_EQUAL_INT(bold.width("Tokyo ") + 17 * 2, bold.width("Tokyo 東京"));
  // A cut in Bold fits by the bold measure: in the Queue's 270 px, 14 Han
  // and "…" where Body has 15.
  const char* han = "東京事変の夜明けに歌う小さな歌とその続き";
  const std::string inBody = drawn(body, han, 270), inBold = drawn(bold, han, 270);
  TEST_ASSERT_TRUE(body.width(inBody.c_str()) <= 270);
  TEST_ASSERT_TRUE(bold.width(inBold.c_str()) <= 270);
  TEST_ASSERT_EQUAL_STRING("東京事変の夜明けに歌う小さな歌…", inBody.c_str());
  TEST_ASSERT_EQUAL_STRING("東京事変の夜明けに歌う小さな…", inBold.c_str());
  // What a page measures (Fonts::width(): textfit::drawnWidth()) is the
  // bold width too: halfwidth kana as the fullwidth, bold.
  TEST_ASSERT_EQUAL_INT(17 * 4, textfit::drawnWidth(viewOf(bold), "ｶﾀｶﾅ"));
}

// Now Playing's title (TitleLayout, 190 px): two lines of Title (22 px),
// else three of Bold (16 px), centred in the strip. A title with a glyph
// from the store goes to the Bold rows whatever its length: its CJK is
// 16 px in every role, small beside 22 px Latin. Decided on the text as
// drawn (composed and folded), with the real fonts and the store.
void test_now_playing_title_rows() {
  const Vlw bold(kVlwSansBold16), title(kVlwSansBold22);
  constexpr int kW = uitext::kNowPlayingTextW;
  char lines[titlelayout::kMaxLines][titlelayout::kLineSize];
  auto lay = [&](const char* t) {
    return titlelayout::wrap(viewOf(title), viewOf(bold), fromStore(title, t), t, kW, lines);
  };
  // Which texts have a glyph from the store, as drawn.
  TEST_ASSERT_TRUE(fromStore(title, "只爱陌生人"));
  TEST_ASSERT_TRUE(fromStore(title, "봄날"));
  TEST_ASSERT_TRUE(fromStore(title, "Tokyo 東京 Live"));
  TEST_ASSERT_TRUE(fromStore(title, "ｶﾀｶﾅ"));  // halfwidth: drawn as the store's カタカナ
  TEST_ASSERT_TRUE(fromStore(title, "\xE1\x84\x92\xE1\x85\xA1\xE1\x86\xAB"));  // NFD 한
  TEST_ASSERT_TRUE(fromStore(title, "Live\xE3\x80\x80"));  // the ideographic space is the store's
  TEST_ASSERT_FALSE(fromStore(title, "Кино – Группа крови"));
  TEST_ASSERT_FALSE(fromStore(title, "Minus \xE2\x88\x92" "1"));  // − (U+2212) is drawn as '-'
  TEST_ASSERT_FALSE(fromStore(title, "ＡＢＣ"));                    // as ABC
  TEST_ASSERT_FALSE(fromStore(title, "\xEB\x98\xA0"));           // 똠: not in the store, drawn '?'
  TEST_ASSERT_FALSE(fromStore(title, ""));
  TEST_ASSERT_FALSE(fromStore(title, nullptr));

  // Latin as before: one Title line in the middle, two at 13 and 39.
  titlelayout::Lines l = lay("Glass Harbour");
  TEST_ASSERT_FALSE(l.bold);
  TEST_ASSERT_EQUAL_INT(1, l.count);
  TEST_ASSERT_EQUAL_INT(26, titlelayout::lineY(l, 0));
  l = lay("Glass Harbour at Night");
  TEST_ASSERT_FALSE(l.bold);
  TEST_ASSERT_EQUAL_INT(2, l.count);
  TEST_ASSERT_EQUAL_INT(13, titlelayout::lineY(l, 0));
  TEST_ASSERT_EQUAL_INT(39, titlelayout::lineY(l, 1));
  // Too long for two Title lines: Bold, three lines at 9, 26 and 43.
  l = lay("The Longest Title Anyone Ever Gave a Song About a Very Long Road");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(3, l.count);
  TEST_ASSERT_EQUAL_INT(9, titlelayout::lineY(l, 0));
  TEST_ASSERT_EQUAL_INT(26, titlelayout::lineY(l, 1));
  TEST_ASSERT_EQUAL_INT(43, titlelayout::lineY(l, 2));
  // ... or two of Bold, centred: 17 and 34.
  l = lay("Glass Harbour at Night and the Pier");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(2, l.count);
  TEST_ASSERT_EQUAL_INT(17, titlelayout::lineY(l, 0));
  TEST_ASSERT_EQUAL_INT(34, titlelayout::lineY(l, 1));
  // CJK: Bold, however short. One line in the middle.
  l = lay("只爱陌生人");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(1, l.count);
  TEST_ASSERT_EQUAL_INT(26, titlelayout::lineY(l, 0));
  TEST_ASSERT_EQUAL_STRING("只爱陌生人", lines[0]);
  l = lay("봄날");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(1, l.count);
  l = lay("ｶﾀｶﾅ ﾊﾟﾗﾀﾞｲｽ");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_STRING("カタカナ パラダイス", lines[0]);
  l = lay("Tokyo 東京事変 Live 2004");  // two Title lines would have held it
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(2, l.count);
  // Any glyph from the store, not only Han, kana and Hangul: JIS X 0208's
  // symbols that the UI fonts lack are the store's too (16 px, bold here),
  // and so is the ideographic space. A Latin title with one is Bold.
  for (const char* sym : {"★", "☆", "♪", "→", "∞", "√", "※", "○", "●", "℃", "‥", "〜", "・", "\xE3\x80\x80"}) {
    TEST_ASSERT_TRUE_MESSAGE(fromStore(title, sym), sym);
  }
  l = lay("Lucky ☆ Star");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(1, l.count);
  TEST_ASSERT_EQUAL_INT(26, titlelayout::lineY(l, 0));
  l = lay("A → B");
  TEST_ASSERT_TRUE(l.bold);
  l = lay("Hello\xE3\x80\x80World");
  TEST_ASSERT_TRUE(l.bold);
  // A sign the UI fonts draw stays theirs: Title.
  l = lay("Star · 1° ©");
  TEST_ASSERT_FALSE(l.bold);
  TEST_ASSERT_EQUAL_INT(1, l.count);
  // 11 bold Han to a line (17 px: 187 of 190), wrapped between them.
  l = lay("東京の夜明けに歌う小さな歌とその続き");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(2, l.count);
  TEST_ASSERT_EQUAL_STRING("東京の夜明けに歌う小さ", lines[0]);
  TEST_ASSERT_EQUAL_STRING("な歌とその続き", lines[1]);
  TEST_ASSERT_EQUAL_INT(17, titlelayout::lineY(l, 0));
  // Three lines at most, the last ellipsised; every line in its 190 px.
  l = lay("東京事変の夜明けに歌う小さな歌とその続きの物語、そして最後の朝の光と長い長い帰り道");
  TEST_ASSERT_TRUE(l.bold);
  TEST_ASSERT_EQUAL_INT(3, l.count);
  for (int i = 0; i < l.count; ++i) TEST_ASSERT_TRUE(bold.width(lines[i]) <= kW);
  TEST_ASSERT_TRUE(titlelayout::cutShort(lines[2], "東京事変の夜明けに歌う小さな歌とその続きの物語、そして最後の朝の光と長い長い帰り道"));
  // Only a '?' where the store has none: Title, as a Latin title.
  l = lay("\xEB\x98\xA0");
  TEST_ASSERT_FALSE(l.bold);
  TEST_ASSERT_EQUAL_STRING("?", lines[0]);
  l = lay("");
  TEST_ASSERT_EQUAL_INT(0, l.count);
  // cutShort(): the last line ends in an ellipsis the text didn't have.
  TEST_ASSERT_TRUE(titlelayout::cutShort("abc…", "abcdef"));
  TEST_ASSERT_TRUE(titlelayout::cutShort("abc...", "abcdef"));
  TEST_ASSERT_FALSE(titlelayout::cutShort("abc…", "abc…"));
  TEST_ASSERT_FALSE(titlelayout::cutShort("abc", "abc"));
}

// Phase 1's glyphs (docs/I18N.md; tools/vlw_font.py's RANGES) keep every
// row of the UI as it was: M5GFX makes a font's line height its tallest
// glyph's ascent plus its deepest one's descent, and the generator fits
// the tall ones (Ǘ, Ă, ΐ) inside the header's. The line heights and
// baselines are the 256-glyph fonts' (3a1835a), and no glyph at all, Latin-1
// included (M5GFX doesn't measure it, so it would draw over the row
// above), leaves the line.
void test_fonts_keep_their_line_heights() {
  struct Want {
    const uint8_t* data;
    int ascent, descent;
  };
  const Want want[] = {{kVlwSans16, 15, 4}, {kVlwSans13, 13, 4}, {kVlwSansBold16, 15, 4}, {kVlwSansBold22, 21, 6}};
  const int line[] = {19, 17, 19, 27};
  for (int i = 0; i < 4; ++i) {
    const Vlw f(want[i].data);
    TEST_ASSERT_EQUAL_INT(want[i].ascent, f.ascent);
    TEST_ASSERT_EQUAL_INT(want[i].descent, f.descent);
    TEST_ASSERT_EQUAL_INT(want[i].ascent, f.maxAscent);  // the baseline
    TEST_ASSERT_EQUAL_INT(line[i], f.yAdvance);
    for (const Vlw::Box& b : f.boxes) {
      char msg[48];
      snprintf(msg, sizeof(msg), "U+%04X in font %d", static_cast<unsigned>(b.cp), i);
      TEST_ASSERT_TRUE_MESSAGE(b.top <= f.ascent && b.height - b.top <= f.descent, msg);
    }
  }
  // The Body and Small heights the boot screen keeps room for.
  TEST_ASSERT_EQUAL_INT(bootlayout::kBodyH, Vlw(kVlwSans16).yAdvance);
  TEST_ASSERT_EQUAL_INT(bootlayout::kSmallH, Vlw(kVlwSans13).yAdvance);
}

// The glyph store's glyphs in the four rows (docs/I18N.md, phase 2), placed
// as ui/Fonts places them: 16 px in every role, on the role's baseline, the
// Small row's a pixel lower (on DejaVu Sans 13's, 13 rows down, 8,902 of
// the 10,557 would reach above its 17 px: the em box is 14 above and 2
// below), and nothing outside the row. A glyph whose ink reaches above the
// row moves down into it (Å in Body and Bold; in Small, 31: box drawing,
// Å, ヷ and nine kanji), and only the full-height box drawing lose a row,
// in Small (12). No kana, Hangul or Han is cut in any role.
void test_store_glyphs_sit_in_the_rows() {
  const glyphstore::Store& s = cjk();
  TEST_ASSERT_TRUE(s.valid());
  struct Role {
    const uint8_t* data;
    int baseline, moved, cut;
  };
  const Role roles[] = {{kVlwSans16, 15, 1, 0}, {kVlwSans13, 14, 31, 12}, {kVlwSansBold16, 15, 1, 0},
                        {kVlwSansBold22, 21, 0, 0}};
  for (const Role& role : roles) {
    const Vlw f(role.data);
    const int base = s.baselineIn(f.maxAscent, f.yAdvance);
    TEST_ASSERT_EQUAL_INT(role.baseline, base);
    int moved = 0, cut = 0, glyphs = 0;
    for (uint32_t cp = 0; cp <= 0xFFFF; ++cp) {
      const glyphstore::Glyph g = s.find(cp);
      if (!g) continue;
      ++glyphs;
      const glyphstore::Metrics m = s.metrics(g);
      if (m.w == 0) continue;
      const glyphstore::Placement p = glyphstore::place(m, base, f.yAdvance);
      char msg[48];
      snprintf(msg, sizeof(msg), "U+%04X in a %d px row", static_cast<unsigned>(cp), f.yAdvance);
      TEST_ASSERT_TRUE_MESSAGE(p.top >= 0 && p.rows > 0 && p.top + p.rows <= f.yAdvance, msg);
      if (p.top != base - m.y) ++moved;
      if (p.rows < m.h) {
        ++cut;
        TEST_ASSERT_TRUE_MESSAGE(cp >= 0x2500 && cp <= 0x257F, msg);  // box drawing only
        TEST_ASSERT_EQUAL_INT_MESSAGE(m.h - 1, p.rows, msg);
      }
    }
    TEST_ASSERT_EQUAL_UINT32(s.glyphCount(), static_cast<uint32_t>(glyphs));
    TEST_ASSERT_EQUAL_INT(role.moved, moved);
    TEST_ASSERT_EQUAL_INT(role.cut, cut);
  }
}

// Now Playing's title lines share rows (TitleLayout): each line's row,
// centred on lineY() as draw() centres it, is taller than the pitch, so
// the next line's row starts over its last rows: Bold's two (19 rows 17
// apart, in two lines as in three) and Title's one (27 rows 26 apart). In
// Bold the ink of the line above reaches them: 157 of DejaVu Sans Bold's
// glyphs (J g j p q y _ { | }, letters with a cedilla or an ogonek, Greek's
// and Cyrillic's descenders: ç ą ρ щ у) and the store's glyphs 3 px under
// the baseline (283 Han, 19 Hangul, 20 box drawing); in Title nothing does.
// So NowPlayingPage::drawTitle() fills its strip and draws the lines with
// no background: a line's own would erase that row (docs/I18N.md, phase 2;
// the drawing is checked on the PC).
void test_now_playing_title_lines_share_rows() {
  const glyphstore::Store& s = cjk();
  struct Layout {
    const uint8_t* data;
    bool bold;
    int count, shared, vlw, han, hangul, other;
  };
  const Layout layouts[] = {{kVlwSansBold16, true, 2, 2, 157, 283, 19, 20},
                            {kVlwSansBold16, true, 3, 2, 157, 283, 19, 20},
                            {kVlwSansBold22, false, 2, 1, 0, 0, 0, 0}};
  for (const Layout& want : layouts) {
    const Vlw f(want.data);
    titlelayout::Lines l;
    l.bold = want.bold;
    l.count = want.count;
    const int base = s.baselineIn(f.maxAscent, f.yAdvance);
    for (int i = 0; i + 1 < l.count; ++i) {
      const int top = titlelayout::lineY(l, i) - (f.yAdvance >> 1);
      const int next = titlelayout::lineY(l, i + 1) - (f.yAdvance >> 1);
      TEST_ASSERT_EQUAL_INT(want.shared, top + f.yAdvance - next);
      const int from = next - top;  // line i's first row under the next line's
      // A VLW glyph's rows start at the baseline less its top (M5GFX).
      int vlw = 0;
      std::string some;
      for (const Vlw::Box& b : f.boxes) {
        if (b.height > 0 && f.maxAscent - b.top + b.height - 1 >= from) {
          ++vlw;
          if (b.cp < 0x80) some += static_cast<char>(b.cp);
        }
      }
      TEST_ASSERT_EQUAL_INT(want.vlw, vlw);
      if (want.vlw) TEST_ASSERT_EQUAL_STRING("J_gjpqy{|}", some.c_str());
      int han = 0, hangul = 0, other = 0;
      for (uint32_t cp = 0; cp <= 0xFFFF; ++cp) {
        const glyphstore::Glyph g = s.find(cp);
        if (!g) continue;
        const glyphstore::Metrics m = s.metrics(g);
        if (m.w == 0) continue;
        const glyphstore::Placement p = glyphstore::place(m, base, f.yAdvance);
        if (p.top + p.rows - 1 < from) continue;
        if (textfold::scriptOf(cp) == textfold::Script::Han) {
          ++han;
        } else if (cp >= 0xAC00 && cp <= 0xD7A3) {
          ++hangul;
        } else {
          ++other;
          TEST_ASSERT_TRUE(cp >= 0x2500 && cp <= 0x257F);  // box drawing
        }
      }
      TEST_ASSERT_EQUAL_INT(want.han, han);
      TEST_ASSERT_EQUAL_INT(want.hangul, hangul);
      TEST_ASSERT_EQUAL_INT(want.other, other);
    }
  }
}

// What each of the four fonts covers since phase 1, and what it leaves to
// TextFit (docs/I18N.md, phase 1: the blocks left out and their cost).
void test_fonts_cover_greek_cyrillic_and_latin() {
  const uint8_t* fonts[] = {kVlwSans16, kVlwSans13, kVlwSansBold16, kVlwSansBold22};
  for (const uint8_t* data : fonts) {
    const Vlw f(data);
    TEST_ASSERT_EQUAL_UINT32(734, f.advance.size());
    auto all = [&](uint32_t a, uint32_t b) {
      for (uint32_t cp = a; cp <= b; ++cp) {
        char msg[24];
        snprintf(msg, sizeof(msg), "U+%04X", static_cast<unsigned>(cp));
        TEST_ASSERT_TRUE_MESSAGE(f.find(cp) >= 0, msg);
      }
    };
    auto none = [&](uint32_t a, uint32_t b) {
      for (uint32_t cp = a; cp <= b; ++cp) {
        char msg[24];
        snprintf(msg, sizeof(msg), "U+%04X", static_cast<unsigned>(cp));
        TEST_ASSERT_TRUE_MESSAGE(f.find(cp) < 0, msg);
      }
    };
    all(0x20, 0x7E);
    all(0xA0, 0xFF);
    all(0x100, 0x17F);  // Latin Extended-A, all of it
    all(0x1CD, 0x1DC);  // Pinyin: ǎ ě ǐ ǒ ǔ, ǖ ǘ ǚ ǜ and their capitals
    all(0x218, 0x21B);  // Romanian: Ș ș Ț ț
    // Greek: the letters with tonos and dialytika, the alphabet, final
    // sigma, the symbols' letter forms and the archaic letters.
    all(0x386, 0x386);
    all(0x388, 0x38A);
    all(0x38C, 0x38C);
    all(0x38E, 0x3A1);
    all(0x3A3, 0x3E1);
    all(0x3F0, 0x3FF);
    all(0x384, 0x385);  // tonos, dialytika tonos
    all(0x37E, 0x37E);  // the Greek question mark, ano teleia
    all(0x387, 0x387);
    // Cyrillic: Russian, Ukrainian, Belarusian, Serbian, Macedonian,
    // Bulgarian, and the Central Asian and Caucasian letters.
    all(0x400, 0x482);
    all(0x48A, 0x4FF);
    // Left out: Church Slavonic's combining marks, the Coptic letters,
    // Cyrillic Supplement, Armenian, Georgian, the rest of Latin
    // Extended-B, Latin Extended Additional (Vietnamese), Greek Extended.
    none(0x483, 0x489);
    none(0x3E2, 0x3EF);
    none(0x500, 0x52F);
    none(0x531, 0x587);
    none(0x10A0, 0x10FF);
    none(0x180, 0x1CC);
    none(0x1DD, 0x217);
    none(0x21C, 0x24F);
    none(0x1E00, 0x1EFF);
    none(0x1F00, 0x1FFF);
    // The sample names, every glyph.
    TEST_ASSERT_TRUE(f.hasAll("Кино Земфира Ёлка Ђорђе Балашевић Ґанна Їжак Ўладзімір Қазақ"));
    TEST_ASSERT_TRUE(f.hasAll("Χάρις Αλεξίου Ϊ ϋ ΐ ΰ ς"));
    TEST_ASSERT_TRUE(f.hasAll("Ștefan Bănică Łódź Wołanie İstanbul Dvořák Ģirts Ļ ŀ ŉ ſ"));
    TEST_ASSERT_TRUE(f.hasAll("Lǚ Nǚ ǎ Ǎ ǜ Ǜ"));
  }
}

// The first-boot tips: the first screen a new user sees.
void test_coach_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, tr(S::coach_title), kCoachTextW);
  fits(small, tr(S::coach_line), kCoachTextW);
  TEST_ASSERT_TRUE(3 * kCoachBoxW + 2 * 4 <= 320);
  for (S id : {S::coach_click_prev, S::coach_click_play, S::coach_click_next}) fits(bold, tr(id), kCoachBoxTextW);
  for (S id : {S::coach_hold_prev, S::coach_hold_play, S::coach_hold_next}) {
    for (int k = 0; k < 2; ++k) fits(small, lineOf(id, k).c_str(), kCoachBoxTextW);
  }
  for (int k = 0; k < 2; ++k) {
    fits(title, lineOf(S::coach_tab, k).c_str(), kCoachTextW);
    fits(small, lineOf(S::coach_tab_more, k).c_str(), kCoachTextW);
  }
  fits(small, tr(S::coach_tips_where), kCoachTextW);
  (void)body;
  // The header (Coach::drawHeader()), each card in each language: "Tip 1
  // of 2" in its room, clear of the pill; the hint only whole, in what the
  // tip and that card's pill (Next, then Got it) leave it, never under the
  // pill (the device check: French's, German's and Russian's ran under it).
  // English's, Japanese's and Chinese's show on both cards.
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    for (int card = 1; card <= 2; ++card) {
      char tip[48];
      uistr::trf(tip, sizeof(tip), S::coach_header_tip, card, 2);
      fits(bold, tip, kCoachTipW);
      const char* label = tr(card == 1 ? S::coach_next : S::coach_got_it);
      const int tipW = std::min(bold.width(tip), kCoachTipW);
      const int pillX = coachPillX(bold.width(label));
      TEST_ASSERT_TRUE_MESSAGE(kCoachTipX + tipW + 8 <= pillX, tip);
      const char* hint = tr(S::coach_header_hint);
      const int room = coachHintRoom(tipW, bold.width(label));
      TEST_ASSERT_TRUE(room <= kCoachHintW);
      if (small.width(hint) <= room) {
        fits(small, hint, room);
        TEST_ASSERT_TRUE_MESSAGE(kCoachTipX + tipW + 8 + small.width(hint) + 8 <= pillX, hint);
      } else {
        char m[96];
        snprintf(m, sizeof(m), "%s card %d: no hint (%d px in %d); ", uistr::tag(l), card, small.width(hint), room);
        report += m;
      }
      if (l == uistr::Lang::En || l == uistr::Lang::Ja || l == uistr::Lang::Zh) {
        TEST_ASSERT_TRUE_MESSAGE(small.width(hint) <= room, uistr::tag(l));
      }
    }
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
  // English as before: the hint at x 12 + the tip + 8, 120 px at most.
  TEST_ASSERT_EQUAL_INT(kCoachHintW, coachHintRoom(bold.width("Tip 1 of 2"), bold.width(tr(S::coach_next))));
  TEST_ASSERT_EQUAL_INT(kCoachHintW, coachHintRoom(bold.width("Tip 2 of 2"), bold.width(tr(S::coach_got_it))));
}

// The play-next toast on two lines: a typical title in Body, mockup 12's
// long one in Small (the stage-1 fix left 168 px: both were cut).
void test_toast_names_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  const int room = kToastCompactTextRight - kToastTextX;
  fits(body, "Can'T Tell Me Nothing", room);
  fits(body, "One More Time", room);
  fits(small, "Harder, Better, Faster, Stronger", room);
  fits(small, uistr::trn(S::queue_cap_push_next, 1), room);
}

// The queue's cap (docs/QUEUE-MODES.md 15): the refusal (full, and no
// played track to push out: 15.8) and a Play, Shuffle all or add it cut short,
// each "what: why" on the toast's two lines (too wide for one beside its
// buttons, as Toast::show() decides), each line in its room in Small at
// least (Toast draws the why in Body when it fits), with the counts of a
// library of 99,999. The refusal keeps the buttons of the toast it covers
// (Ui::refuse()): none, Undo, or Undo and View, each room measured.
void test_queue_cap_texts_fit() {
  using namespace uitext;
  using queueview::Capped;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  {
    char full[128];
    uistr::trnf(full, sizeof(full), S::queue_cap_full, 5000, uistr::grouped(5000));
    const char* kQueueFull = full;
    const std::string s(kQueueFull);
    const size_t colon = s.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    struct Kept {
      int oneLine, room;  // Toast's textRightOf(false, ..) and (true, ..), less kToastTextX
    } kept[] = {{kToastTextRight - kToastTextX, kToastTextRight - kToastTextX},
                {kToastUndoX - 6 - kToastTextX, kToastUndoCX - 6 - kToastTextX},
                {kToastViewX - 6 - kToastTextX, kToastCompactTextRight - kToastTextX}};
    for (const Kept& k : kept) {
      TEST_ASSERT_TRUE(body.width(kQueueFull) > k.oneLine);  // folded on two lines
      fits(small, s.substr(0, colon).c_str(), k.room);
      fits(small, kQueueFull + colon + 2, k.room);
    }
    fits(body, kQueueFull + colon + 2, kToastUndoCX - 6 - kToastTextX);  // Body with none or Undo kept
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: what %d (Small) in %d at the least", kQueueFull,
             small.width(s.substr(0, colon).c_str()), kept[2].room);
    TEST_MESSAGE(msg);
  }
  struct Case {
    Capped what;
    uint32_t took, asked;
    bool view;  // an add: View beside Undo
  } cases[] = {{Capped::Shuffle, 5000, 99999, false},
               {Capped::Play, 5000, 99999, false},
               {Capped::Add, 4999, 99999, true},
               {Capped::Next, 4999, 99999, true}};
  for (const Case& c : cases) {
    char t[128];
    queueview::cappedText(c.what, c.took, c.asked, t, sizeof(t));
    const std::string s(t);
    const size_t colon = s.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    const int oneLine = (c.view ? kToastViewX : kToastUndoX) - 6 - kToastTextX;
    const int room = c.view ? kToastCompactTextRight - kToastTextX : kToastUndoCX - 6 - kToastTextX;
    TEST_ASSERT_TRUE(body.width(t) > oneLine);
    fits(small, s.substr(0, colon).c_str(), room);
    fits(small, t + colon + 2, room);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: what %d (Small), why %d (Body) / %d (Small) in %d", t,
             small.width(s.substr(0, colon).c_str()), body.width(t + colon + 2), small.width(t + colon + 2), room);
    TEST_MESSAGE(msg);
  }
}

// An add that pushed out what played (docs/QUEUE-MODES.md 15.8): the add's
// what over one line of how many played tracks made way, on the toast's two
// lines beside Undo and View (as icons), each line in its room in Small at
// least, at the most an add can push out (4,999: all but the one that
// plays) and a library of 99,999.
void test_push_out_texts_fit() {
  using namespace uitext;
  using queueview::Capped;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  const int oneLine = kToastViewX - 6 - kToastTextX;
  const int room = kToastCompactTextRight - kToastTextX;
  struct Case {
    Capped what;
    uint32_t took, asked, pushed;
  } cases[] = {{Capped::Next, 1, 1, 1},          {Capped::Add, 1, 1, 1},
               {Capped::Next, 4999, 4999, 4999}, {Capped::Add, 4999, 4999, 4999},
               {Capped::Next, 4999, 99999, 4999}, {Capped::Add, 4999, 99999, 4999}};
  for (const Case& c : cases) {
    char t[128];
    queueview::pushedText(c.what, c.took, c.asked, c.pushed, t, sizeof(t));
    const std::string s(t);
    const size_t colon = s.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    TEST_ASSERT_TRUE(body.width(t) > oneLine);  // folded on two lines, the buttons as icons
    fits(small, s.substr(0, colon).c_str(), room);
    fits(small, t + colon + 2, room);
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: what %d (Small), line %d (Body) / %d (Small) in %d", t,
             small.width(s.substr(0, colon).c_str()), body.width(t + colon + 2), small.width(t + colon + 2), room);
    TEST_MESSAGE(msg);
  }
  // One played track's line is Body.
  char one[64];
  uistr::trnf(one, sizeof(one), S::queue_cap_pushed, 1, 1);
  fits(body, one, room);
}

// The empty states' two buttons (the empty queue, Nothing playing) and line.
void test_empty_state_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  TEST_ASSERT_TRUE(bold.width(tr(S::empty_open_library)) + kEmptyIconW[0] + 8 <= kEmptyPrimaryW - 12);
  TEST_ASSERT_TRUE(body.width(tr(S::empty_shuffle_all)) + kEmptyIconW[1] + 8 <= kEmptySecondW - 12);
  TEST_ASSERT_TRUE(kEmptyPrimaryX + kEmptyPrimaryW < kEmptySecondX && kEmptySecondX + kEmptySecondW <= 320 - 6);
  fits(small, tr(S::empty_pick_in_library), kEmptyLineW);
}

// No card, and each kind of card that didn't mount (cardMessage(): exFAT,
// NTFS, a GPT, nothing recognised): the empty state's title and lines, and
// Try again's note when it still fails (Small before ": ", Body after).
void test_no_card_texts_fit() {
  using namespace uitext;
  using cardformat::Kind;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, tr(S::empty_try_again), 320 - 140 - 12);  // the one button: x 70, 180 px, 6 px in at each end
  for (Kind k : {Kind::Unreadable, Kind::Other, Kind::ExFat, Kind::Ntfs, Kind::Gpt}) {
    const CardMessage& m = cardMessage(k);
    fits(title, tr(m.title), kEmptyTitleW);
    TEST_ASSERT_EQUAL_INT(2, uistr::lineCount(tr(m.lines)));
    const std::string line0 = lineOf(m.lines, 0), line1 = lineOf(m.lines, 1);
    fits(small, line0.c_str(), kEmptyLineW);
    fits(small, line1.c_str(), kEmptyLineW);
    const std::string n(tr(m.still));
    const size_t colon = n.find(": ");
    TEST_ASSERT_TRUE(colon != std::string::npos);
    fits(small, n.substr(0, colon).c_str(), kToastTwoLineW);
    fits(body, tr(m.still) + colon + 2, kToastTwoLineW);
    char msg[200];
    snprintf(msg, sizeof(msg), "%s: title %d; lines %d, %d; note %d, %d", cardformat::name(k),
             title.width(tr(m.title)), small.width(line0.c_str()), small.width(line1.c_str()),
             small.width(n.substr(0, colon).c_str()), body.width(tr(m.still) + colon + 2));
    TEST_MESSAGE(msg);
  }
}

// The board guard's screen (Font2, no wrap): every line at Font2's widest
// glyph per character, printable ASCII only (Font2 has nothing else), the
// "found: " line with the longest name BoardGuard allows.
void test_board_guard_texts_fit() {
  using namespace uitext;
  auto fitsFont2 = [](const char* text, size_t extraChars) {
    for (const char* p = text; *p; ++p) TEST_ASSERT_TRUE(*p >= 0x20 && *p < 0x7f);
    const int w = static_cast<int>(strlen(text) + extraChars) * kFont2MaxAdvance;
    char msg[120];
    snprintf(msg, sizeof(msg), "\"%s\" (+%u chars): at most %d px in %d", text, (unsigned)extraChars, w,
             kBoardGuardW);
    TEST_ASSERT_TRUE_MESSAGE(w <= kBoardGuardW, msg);
  };
  for (const char* t : kBoardGuardTop) fitsFont2(t, 0);
  fitsFont2(kBoardGuardFound, static_cast<size_t>(kBoardNameMaxChars));
  for (const char* t : kBoardGuardStop) fitsFont2(t, 0);
  fitsFont2(kBoardGuardToughHint, 0);
}

// The Queue's selection bar: Remove shows its count (with the icon up to
// 99, without it beyond), and its header keeps the position.
void test_queue_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  char t[24];
  for (unsigned n = 1; n <= 9999; n = n < 120 ? n + 1 : n * 3) {
    snprintf(t, sizeof(t), "Remove %u", n);
    if (n <= 99) {
      fits(bold, t, kRemoveW - kRemoveTextX - 4);
    } else {
      fits(bold, t, kRemoveW - 8);
    }
  }
  fits(body, tr(S::queue_play_next_bar), kQueueNextW - kQueueNextPad);
  fits(body, tr(S::queue_clear_bar), kQueueClearW - kQueueClearPad);
  TEST_ASSERT_TRUE(kRemoveX + kRemoveW < kQueueNextX && kQueueNextX + kQueueNextW < kQueueClearX &&
                   kQueueClearX + kQueueClearW <= 320);
  // Every language: Play next and Clear in Body, else Small.
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    fitsIn(body, small, tr(S::queue_play_next_bar), kQueueNextW - kQueueNextPad);
    fitsIn(body, small, tr(S::queue_clear_bar), kQueueClearW - kQueueClearPad);
  }
  uistr::setLang(uistr::Lang::En);
  // The header: "4 of 16 · 49 min" beside "Queue" and the Edit pill (the
  // room QueuePage::header() computes: the pill's label in Bold within
  // kQueuePillTextW, else Small, Header::pillW()), up to 999 entries and 16
  // hours.
  const char* edit = tr(S::queue_edit);
  const int pillW = (bold.width(edit) <= kQueuePillTextW ? bold : small).width(edit) + kHeaderPillPad;
  TEST_ASSERT_EQUAL_INT(bold.width(edit) + 24, pillW);  // English: Bold, as before
  const int room = (320 - 6 - pillW - 8) - (12 + bold.width(tr(S::queue_title)) + 8);
  TEST_ASSERT_EQUAL_INT(room, headerSubRoom(12, bold.width(tr(S::queue_title)), headerRight(pillW)));
  TEST_ASSERT_EQUAL_INT(169, room);
  queueview::Time time;
  time.knownS = 999 * 60;
  time.unknown = 1;
  char buf[64];
  queueview::summary(500, time, 999, 999, buf, sizeof(buf), false);
  fits(small, buf, room);
  time.knownS = 49 * 60;
  time.unknown = 0;
  queueview::summary(12, time, 4, 16, buf, sizeof(buf), false);
  fits(small, buf, room);
}

// The Queue header's summary in every language (QueuePage::header()): the
// first of its forms that fits whole beside that language's title and Edit
// pill (queueview::pickSummary(): the long one, the position and the time,
// the position alone, and only with no position the count and the time:
// the owner's call); when the position alone doesn't fit there, the first
// that fits whole on the header's second line, under the title, in the
// same order (queueview::placeSummary(): German's "Warteschlange", the
// apps' word, leaves 78 px beside it, too little for the position from six
// digits, and 221 under it); else none: never one cut (the device check's
// German "noch 9 · 3…" in 78 px). Where the queue is shows at each of these
// queues in every language, 4999 of 5000 too, and with no time known; each
// language's picks are pinned. English never goes under its title, and its
// picks are as before but near the end of a long queue with hours next:
// "1980 of 2000" where "20 up next · 1 h 40+ min" was.
void test_queue_summary_fits_every_language() {
  using namespace uitext;
  using queueview::kSummaryLong;
  using queueview::kSummaryMid;
  using queueview::kSummaryPosition;
  using queueview::kSummaryShort;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  // The pick itself, the widths in the order the forms are tried: {long,
  // mid, position, short}. The position alone before the count and the
  // time, which never come while there is a position.
  TEST_ASSERT_EQUAL_INT(kSummaryLong, queueview::pickSummary({100, 90, 40, 80}, 100));
  TEST_ASSERT_EQUAL_INT(kSummaryMid, queueview::pickSummary({120, 95, 40, 80}, 100));
  TEST_ASSERT_EQUAL_INT(kSummaryPosition, queueview::pickSummary({120, 110, 40, 80}, 100));
  TEST_ASSERT_EQUAL_INT(-1, queueview::pickSummary({120, 110, 101, 60}, 100));
  TEST_ASSERT_EQUAL_INT(kSummaryMid, queueview::pickSummary({-1, 60, -1, 50}, 100));  // an empty form is never picked
  TEST_ASSERT_EQUAL_INT(-1, queueview::pickSummary({-1, -1, -1, -1}, 100));
  // No position (nothing current): the count and the time, last (the long
  // and the mid forms say the same then: "Nothing up next").
  TEST_ASSERT_EQUAL_INT(kSummaryShort, queueview::pickSummary({-1, -1, -1, 80}, 100));
  TEST_ASSERT_EQUAL_INT(kSummaryLong, queueview::pickSummary({90, 90, -1, 90}, 100));
  TEST_ASSERT_EQUAL_INT(-1, queueview::pickSummary({120, 120, -1, 120}, 100));
  // Where: beside the title while the position alone fits there (over a
  // fuller form under it), the first form that fits there; else under it,
  // the first that fits there in the same order, else none. A count that
  // fits beside the title never hides where the queue is (German's "noch
  // 20", 51 px in 78, under "100 von 120 · noch 20" when no time is known);
  // with no position (nothing current), beside while a form fits there.
  {
    queueview::SummaryPick p = queueview::placeSummary({100, 90, 40, 80}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryLong, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({120, 110, 40, 101}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({120, 110, 40, 80}, 100, 200);  // the count and the time fit too
    TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({190, 180, 101, 160}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryLong, p.form);
    TEST_ASSERT_TRUE(p.below);
    p = queueview::placeSummary({250, 180, 101, 160}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryMid, p.form);
    TEST_ASSERT_TRUE(p.below);
    // Under the title in the same order: the position alone, though the
    // count and the time fit there (and beside the title).
    p = queueview::placeSummary({250, 230, 101, 60}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.form);
    TEST_ASSERT_TRUE(p.below);
    p = queueview::placeSummary({250, 240, 201, 230}, 100, 200);
    TEST_ASSERT_EQUAL_INT(-1, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({250, 240, 201, 60}, 100, 200);
    TEST_ASSERT_EQUAL_INT(-1, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({-1, -1, -1, -1}, 100, 200);
    TEST_ASSERT_EQUAL_INT(-1, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({143, 80, 80, 51}, 78, 221);
    TEST_ASSERT_EQUAL_INT(kSummaryLong, p.form);
    TEST_ASSERT_TRUE(p.below);
    p = queueview::placeSummary({100, 100, -1, 100}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryLong, p.form);
    TEST_ASSERT_FALSE(p.below);
    p = queueview::placeSummary({120, 120, -1, 120}, 100, 200);
    TEST_ASSERT_EQUAL_INT(kSummaryLong, p.form);
    TEST_ASSERT_TRUE(p.below);
  }
  struct Case {
    uint32_t upNext, mins;
    bool more;
    uint32_t pos, size;
  };
  const Case cases[] = {
      {9, 37, false, 1, 10},
      {12, 49, false, 4, 16},
      {500, 999, true, 999, 999},
      {4999, 5999, true, 4999, 5000},
      {0, 0, false, 10, 10},
      {99, 120, false, 1, 100},
      {999, 999, true, 1, 1000},
      {8, 0, false, 2, 10},
      {4998, 5999, true, 1, 4999},
      {20, 0, false, 100, 120},     // no time known: "100 von 120 · noch 20", "noch 20"
      {20, 60, false, 100, 120},    // an hour next: Russian's "ещё 20 · 60 мин" fits, "100 из 120" too
      {20, 100, true, 1980, 2000},  // English's "20 up next · 1 h 40+ min" fits, "1980 of 2000" too
      {0, 0, false, 0, 10},         // no position (nothing current)
  };
  // The UI's language's rooms and pick at a queue, as QueuePage::header()
  // makes them: beside the title (to 8 px before the Edit pill, the pill's
  // label in Bold within kQueuePillTextW, else Small: Header::pillW()) and
  // under it (from x 12 to the same right).
  struct Placed {
    int titleW, right, beside, below;
    char forms[queueview::kSummaryForms][64];
    int widths[queueview::kSummaryForms];
    queueview::SummaryPick pick;
  };
  auto place = [&](const Case& c, Placed& p) {
    const char* edit = tr(S::queue_edit);
    const int pillW = (bold.width(edit) <= kQueuePillTextW ? bold : small).width(edit) + kHeaderPillPad;
    p.titleW = bold.width(tr(S::queue_title));
    p.right = headerRight(pillW);
    p.beside = headerSubRoom(12, p.titleW, p.right);
    p.below = p.right - 12;
    queueview::Time t;
    t.knownS = c.mins * 60;
    t.unknown = c.more ? 1 : 0;
    char(&f)[queueview::kSummaryForms][64] = p.forms;
    queueview::summary(c.upNext, t, c.pos, c.size, f[kSummaryLong], sizeof(f[0]));
    queueview::summary(c.upNext, t, c.pos, c.size, f[kSummaryMid], sizeof(f[0]), false);
    // The position alone only with a position (the header makes it only
    // with a current entry).
    f[kSummaryPosition][0] = 0;
    if (c.pos > 0) queueview::summary(0, queueview::Time{}, c.pos, c.size, f[kSummaryPosition], sizeof(f[0]), false);
    queueview::summary(c.upNext, t, 0, c.size, f[kSummaryShort], sizeof(f[0]));
    for (int k = 0; k < queueview::kSummaryForms; ++k) p.widths[k] = f[k][0] ? small.width(f[k]) : -1;
    p.pick = queueview::placeSummary(p.widths, p.beside, p.below);
  };
  static const char* const kFormNames[queueview::kSummaryForms] = {"long", "mid", "position", "short"};
  // Each language's picks at the queues above, in their order: "<position
  // or 0>/<size> [<minutes known>m] <form>[ under]".
  struct Picks {
    uistr::Lang lang;
    const char* picks;
  };
  static const Picks kPicks[] = {
      {uistr::Lang::En,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m mid, 4999/5000 5999m position, 10/10 long, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::De,
       "1/10 37m position, 4/16 49m position, 999/999 999m mid under, 4999/5000 5999m mid under, 10/10 mid, "
       "1/100 120m position, 1/1000 999m position, 2/10 mid, 1/4999 5999m position, 100/120 long under, "
       "100/120 60m long under, 1980/2000 100m mid under, 0/10 long under"},
      {uistr::Lang::Es,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m position, 4999/5000 5999m position, 10/10 long, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 mid, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::Fr,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m mid, 4999/5000 5999m position, 10/10 long, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::It,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m position, 4999/5000 5999m position, 10/10 mid, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::Ja,
       "1/10 37m long, 4/16 49m long, 999/999 999m mid, 4999/5000 5999m position, 10/10 mid, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::Pl,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m position, 4999/5000 5999m position, 10/10 mid, "
       "1/100 120m position, 1/1000 999m position, 2/10 long, 1/4999 5999m position, 100/120 mid, 100/120 60m mid, "
       "1980/2000 100m position, 0/10 long"},
      {uistr::Lang::Pt,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m mid, 4999/5000 5999m position, 10/10 long, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m position, "
       "0/10 long"},
      {uistr::Lang::Ru,
       "1/10 37m mid, 4/16 49m mid, 999/999 999m position, 4999/5000 5999m position, 10/10 mid, "
       "1/100 120m position, 1/1000 999m position, 2/10 long, 1/4999 5999m position, 100/120 mid, "
       "100/120 60m position, 1980/2000 100m position, 0/10 long under"},
      {uistr::Lang::Zh,
       "1/10 37m long, 4/16 49m long, 999/999 999m mid, 4999/5000 5999m mid, 10/10 long, 1/100 120m mid, "
       "1/1000 999m mid, 2/10 long, 1/4999 5999m mid, 100/120 long, 100/120 60m mid, 1980/2000 100m mid, 0/10 long"},
  };
  std::string report;
  Placed p;
  int pinned = 0;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    place(cases[0], p);
    char m[160];
    snprintf(m, sizeof(m), "%s (%d px beside, %d under): ", uistr::tag(l), p.beside, p.below);
    report += m;
    std::string picks;
    for (const Case& c : cases) {
      place(c, p);
      const int k = p.pick.form;
      const bool position = p.widths[kSummaryPosition] >= 0;
      TEST_ASSERT_EQUAL_INT(c.pos > 0 ? 1 : 0, position ? 1 : 0);
      // The first form tried that fits its room: none tried before it fits
      // there (the count and the time aren't tried with a position).
      const int room = p.pick.below ? p.below : p.beside;
      const int tried = position ? static_cast<int>(kSummaryShort) : queueview::kSummaryForms;
      for (int j = 0; j < (k < 0 ? tried : k); ++j) {
        TEST_ASSERT_TRUE(p.widths[j] < 0 || p.widths[j] > room);
      }
      // A form at each of these queues, whole in its room, and drawn where
      // the pick says by Ui::drawHeader(): beside the title, or under it
      // (it doesn't fit beside).
      TEST_ASSERT_TRUE_MESSAGE(k >= 0, p.forms[kSummaryLong]);
      fits(small, p.forms[k], room);
      const HeaderSub how = headerSub(12, p.titleW, p.right, p.widths[k], true, true);
      TEST_ASSERT_TRUE(how == (p.pick.below ? HeaderSub::Below : HeaderSub::Beside));
      if (position) {
        // Under the title exactly when the position alone doesn't fit
        // beside it.
        TEST_ASSERT_EQUAL_INT_MESSAGE(p.widths[kSummaryPosition] > p.beside ? 1 : 0, p.pick.below ? 1 : 0,
                                      p.forms[kSummaryPosition]);
        // Where the queue is: a form that starts with the position, never
        // the count and the time.
        TEST_ASSERT_TRUE_MESSAGE(k != kSummaryShort, p.forms[kSummaryPosition]);
        TEST_ASSERT_TRUE_MESSAGE(strncmp(p.forms[k], p.forms[kSummaryPosition], strlen(p.forms[kSummaryPosition])) == 0,
                                 p.forms[kSummaryPosition]);
      } else {
        // No position: the count and the time ("Nothing up next"), the
        // long form saying it first; under the title when it doesn't fit
        // beside it.
        TEST_ASSERT_EQUAL_INT(kSummaryLong, k);
        TEST_ASSERT_EQUAL_STRING(p.forms[kSummaryShort], p.forms[k]);
        TEST_ASSERT_EQUAL_STRING(tr(S::queue_summary_nothing_next), p.forms[k]);
        TEST_ASSERT_EQUAL_INT(p.widths[k] > p.beside ? 1 : 0, p.pick.below ? 1 : 0);
      }
      // English: beside the title at every queue.
      if (l == uistr::Lang::En) TEST_ASSERT_FALSE_MESSAGE(p.pick.below, p.forms[k]);
      char mins[16] = "";
      if (c.mins) snprintf(mins, sizeof(mins), " %um", static_cast<unsigned>(c.mins));
      snprintf(m, sizeof(m), "%s%u/%u%s %s%s", picks.empty() ? "" : ", ", static_cast<unsigned>(c.pos),
               static_cast<unsigned>(c.size), mins, kFormNames[k], p.pick.below ? " under" : "");
      picks += m;
    }
    report += picks + "; ";
    // The picks, pinned.
    for (const Picks& want : kPicks) {
      if (want.lang != l) continue;
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want.picks, picks.c_str(), uistr::tag(l));
      ++pinned;
    }
  }
  TEST_MESSAGE(report.c_str());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sizeof(kPicks) / sizeof(kPicks[0])), pinned);
  // German: the apps' word (the mobile app's and the terminal player's,
  // as its other texts that name the queue), 78 px beside it and 221 under.
  uistr::setLang(uistr::Lang::De);
  TEST_ASSERT_EQUAL_STRING("Warteschlange", tr(S::queue_title));
  place(cases[0], p);
  TEST_ASSERT_EQUAL_INT(78, p.beside);
  TEST_ASSERT_EQUAL_INT(221, p.below);
  // The device check's queue, 1 of 10: the position alone, beside it.
  TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.pick.form);
  TEST_ASSERT_FALSE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("1 von 10", p.forms[p.pick.form]);
  // From six digits the position alone doesn't fit beside it: under it,
  // the fullest form that fits there.
  place(cases[2], p);
  TEST_ASSERT_EQUAL_INT(kSummaryMid, p.pick.form);
  TEST_ASSERT_TRUE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("999 von 999 \xC2\xB7 16 h 39+ min", p.forms[p.pick.form]);
  place(cases[3], p);
  TEST_ASSERT_EQUAL_INT(kSummaryMid, p.pick.form);
  TEST_ASSERT_TRUE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("4999 von 5000 \xC2\xB7 99 h 59+ min", p.forms[p.pick.form]);
  place(cases[10], p);  // six digits: "100 von 120" is 80 px
  TEST_ASSERT_EQUAL_INT(kSummaryLong, p.pick.form);
  TEST_ASSERT_TRUE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("100 von 120 \xC2\xB7 noch 20 \xC2\xB7 60 min", p.forms[p.pick.form]);
  // With no time known the count alone, "noch 20", would fit beside it:
  // under it, with the position, all the same.
  place(cases[9], p);
  TEST_ASSERT_EQUAL_STRING("noch 20", p.forms[kSummaryShort]);
  TEST_ASSERT_TRUE(p.widths[kSummaryShort] <= p.beside);
  TEST_ASSERT_EQUAL_INT(kSummaryLong, p.pick.form);
  TEST_ASSERT_TRUE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("100 von 120 \xC2\xB7 noch 20", p.forms[p.pick.form]);
  // Russian at 100 of 120 with an hour next (the owner's call): the
  // position alone beside "Очередь", where the count and the time fit too
  // and were drawn before ("ещё 20 · 60 мин"); the position and the time
  // don't fit there.
  uistr::setLang(uistr::Lang::Ru);
  place(cases[10], p);
  TEST_ASSERT_EQUAL_INT(122, p.beside);
  TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.pick.form);
  TEST_ASSERT_FALSE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("100 \xD0\xB8\xD0\xB7 120", p.forms[p.pick.form]);
  TEST_ASSERT_EQUAL_STRING("\xD0\xB5\xD1\x89\xD1\x91 20 \xC2\xB7 60 \xD0\xBC\xD0\xB8\xD0\xBD", p.forms[kSummaryShort]);
  TEST_ASSERT_TRUE(p.widths[kSummaryShort] <= p.beside);
  TEST_ASSERT_TRUE(p.widths[kSummaryMid] > p.beside);
  // English: the position and the time, the long form when nothing is
  // next; the position alone where both are too wide (all three were
  // before: 4999 of 5000), and before the count and the time near the end
  // of a long queue with hours next; beside the title, 169 px (235 under
  // it, never used).
  uistr::setLang(uistr::Lang::En);
  place(cases[0], p);
  TEST_ASSERT_EQUAL_INT(169, p.beside);
  TEST_ASSERT_EQUAL_INT(235, p.below);
  TEST_ASSERT_EQUAL_INT(kSummaryMid, p.pick.form);
  TEST_ASSERT_EQUAL_STRING("1 of 10 \xC2\xB7 37 min", p.forms[p.pick.form]);
  place(cases[1], p);
  TEST_ASSERT_EQUAL_INT(kSummaryMid, p.pick.form);
  place(cases[2], p);
  TEST_ASSERT_EQUAL_INT(kSummaryMid, p.pick.form);
  TEST_ASSERT_EQUAL_STRING("999 of 999 \xC2\xB7 16 h 39+ min", p.forms[p.pick.form]);
  place(cases[4], p);
  TEST_ASSERT_EQUAL_INT(kSummaryLong, p.pick.form);
  TEST_ASSERT_EQUAL_STRING("10 of 10 \xC2\xB7 Nothing up next", p.forms[p.pick.form]);
  place(cases[3], p);
  TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.pick.form);
  TEST_ASSERT_FALSE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("4999 of 5000", p.forms[p.pick.form]);
  TEST_ASSERT_TRUE(p.widths[kSummaryShort] > p.beside);  // (as before: all three too wide)
  place(cases[11], p);
  TEST_ASSERT_EQUAL_INT(kSummaryPosition, p.pick.form);
  TEST_ASSERT_FALSE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("1980 of 2000", p.forms[p.pick.form]);
  TEST_ASSERT_EQUAL_STRING("20 up next \xC2\xB7 1 h 40+ min", p.forms[kSummaryShort]);
  TEST_ASSERT_TRUE(p.widths[kSummaryShort] <= p.beside);  // (what it drew before)
  TEST_ASSERT_TRUE(p.widths[kSummaryMid] > p.beside);
  place(cases[12], p);
  TEST_ASSERT_EQUAL_INT(kSummaryLong, p.pick.form);
  TEST_ASSERT_FALSE(p.pick.below);
  TEST_ASSERT_EQUAL_STRING("Nothing up next", p.forms[p.pick.form]);
  uistr::setLang(uistr::Lang::En);
}

// The Output tab: every button of every Bluetooth card in its box (the
// armed Forget's "Tap again" too), the volume chip at 100 %, the status
// lines, the speaker's line, the hints.
void test_output_texts_fit() {
  using namespace uitext;
  using P = BtLink::Phase;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const P phases[] = {P::Off, P::Paging, P::Scanning, P::Linked, P::PairScan, P::Pairing, P::Backoff, P::Resting};
  int cards = 0;
  struct Box {
    BtButton b;
    int w;
  };
  std::vector<Box> boxes;  // every button in every box it is drawn in (for the other languages)
  for (int sess = 0; sess < 5; ++sess) {
    for (P phase : phases) {
      for (int remembered = 0; remembered < 2; ++remembered) {
        for (int lost = 0; lost < 2; ++lost) {
          BtSession s;
          if (sess == 1) s.connect(0);
          if (sess == 2 || sess == 3) {
            // A connect (2) or a pairing (3) that failed.
            if (sess == 2) s.connect(0); else s.pairStarted(0);
            BtLink l;
            l.phase = P::Paging;
            l.remembered = true;
            s.update(l, 10);
            l.phase = P::Off;
            s.update(l, 20);
          }
          if (sess == 4) s.pairStarted(0);
          BtLink link;
          link.phase = phase;
          link.remembered = remembered != 0;
          link.attempt = 3;
          link.attempts = 3;
          const BtCardView v = btCardView(link, s, lost != 0);
          ++cards;
          const int n = v.buttonCount;
          for (int i = 0; i < n; ++i) {
            const BtButton b = v.buttons[i];
            int w = n == 1 ? kBtButtons1W : n == 2 ? kBtButtons2W[i] : kBtButtons3W[i];
            if (n == 1 && b == BtButton::Pair) w = 320 - 32;
            const int room = w - kBtButtonPad;
            if (b == BtButton::Volume) {
              fits(body, "100%", w - kBtChipTextX - 4);
            } else if (b == BtButton::More) {
              TEST_ASSERT_TRUE(18 <= room);  // its icon
            } else {
              fits(b == BtButton::Pair ? bold : body, btButtonLabel(b, w < kBtWideButtonW), room);
              if (b == BtButton::Forget) fits(bold, tr(S::bt_tap_again), room);
              boxes.push_back({b, w});
            }
          }
          char line[80];
          btStatusLine(v, link, "SPYDRONE", "SBC 44.1 kHz, 175 ms", line, sizeof(line));
          fits(small, line, kBtStatusW);
          if (v.hint) {
            // Resting: its two lines beside the one button, right of it.
            TEST_ASSERT_EQUAL_INT(1, n);
            fits(small, lineOf(S::bt_hint, 0).c_str(), kBtHintW);
            fits(small, lineOf(S::bt_hint, 1).c_str(), kBtHintW);
          }
        }
      }
    }
  }
  TEST_ASSERT_TRUE(cards > 100);
  TEST_ASSERT_TRUE(kBtButtons3X[2] + kBtButtons3W[2] <= 320 - 16);
  // Every language: each label in its font (Bold for Pair and the armed
  // Forget), else Small (OutputPage::drawBtButtons()).
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    for (const Box& x : boxes) {
      const int room = x.w - kBtButtonPad;
      fitsIn(x.b == BtButton::Pair ? bold : body, small, btButtonLabel(x.b, x.w < kBtWideButtonW), room);
      if (x.b == BtButton::Forget) fitsIn(bold, small, tr(S::bt_tap_again), room);
    }
  }
  uistr::setLang(uistr::Lang::En);
  // The speaker card: "Here until SPYDRONE connects" doesn't fit beside the
  // chip; its shorter form does (OutputPage::drawSpeaker picks).
  TEST_ASSERT_TRUE(small.width("Here until SPYDRONE connects") > kSpeakerLineW);
  fits(small, "Waits for SPYDRONE", kSpeakerLineW);
  fits(small, "Here until they connect", kSpeakerLineW);
  fits(small, tr(S::speaker_silent_mode), kSpeakerLineW);
  fits(body, "100%", kSpeakerChipW - 2);
  TEST_ASSERT_TRUE(52 + kSpeakerLineW <= kSpeakerChipX);
  fits(small, tr(S::pair_hint), kPairHintW);
  // The resting card's hint starts past its button and ends before the edge.
  TEST_ASSERT_TRUE(kBtHintX >= kBtButtons1X + kBtButtons1W + 8);
  TEST_ASSERT_TRUE(kBtHintX + kBtHintW <= 320 - 8);
  // The lost dialog's line while resting (Small over the dialog's text
  // width: its body's, kDialogTitleW).
  fits(small, tr(S::dialog_lost_resting), kDialogTitleW);
  // The resting card's hint when they dropped while the output.
  fits(small, lineOf(S::bt_lost_hint, 0).c_str(), kBtHintW);
  fits(small, lineOf(S::bt_lost_hint, 1).c_str(), kBtHintW);
  // The pocket rule's note (Ui::warn: Body on one line, no buttons).
  fits(body, tr(S::toast_touch_first), kToastTextRight - kToastTextX);
  // None paired (a B hold, a play on Bluetooth): too long for one line, so
  // the toast takes its two ("No headphones paired" in Small over "Output
  // > Pair new headphones" in Body).
  {
    const char* colon = strstr(tr(S::toast_no_headphones), ": ");
    TEST_ASSERT_NOT_NULL(colon);
    TEST_ASSERT_TRUE(body.width(tr(S::toast_no_headphones)) > kToastTextRight - kToastTextX);
    const std::string what(tr(S::toast_no_headphones), colon);
    fits(small, what.c_str(), kToastTwoLineW);
    fits(body, colon + 2, kToastTwoLineW);
  }
  // A skipped track's why ("Skipped <title>: <why>"; a long title takes
  // the toast's two lines, the why in Body): each, with the widest rates
  // a refusal can name (RateConverter::rateText()).
  fits(body, tr(S::toast_why_cant_play), kToastTwoLineW);
  fits(body, tr(S::toast_why_cpu), kToastTwoLineW);
  for (uint32_t hz : {96000u, 88200u, 176400u, 352800u, 705600u, 44056u, 655350u, 1048575u}) {
    char rate[16], why[48];
    RateConverter::rateText(hz, rate, sizeof(rate));
    uistr::trf(why, sizeof(why), S::toast_why_rate, rate);
    fits(body, why, kToastTwoLineW);
  }
  // The Pair screen once its search stopped.
  fits(bold, tr(S::pair_search_again), 320 - 60);
  fits(small, tr(S::pair_search_stopped), kPairHintW);
  fits(small, tr(S::output_line_out_sub), kSettingSubW);
  // The screen settings: every choice in its pill, the lines beside it;
  // in every language in Body, else Small.
  for (int c = 0; c < ScreenPower::kTimeouts; ++c) fits(body, ScreenPower::timeoutLabel(c), kSettingPillW - kSettingPillPad);
  for (int c = 0; c < ScreenPower::kBrightnesses; ++c) {
    fits(body, ScreenPower::brightnessLabel(c), kSettingPillW - kSettingPillPad);
  }
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    for (int c = 0; c < ScreenPower::kTimeouts; ++c) {
      fitsIn(body, small, ScreenPower::timeoutLabel(c), kSettingPillW - kSettingPillPad);
    }
    for (int c = 0; c < ScreenPower::kBrightnesses; ++c) {
      fitsIn(body, small, ScreenPower::brightnessLabel(c), kSettingPillW - kSettingPillPad);
    }
    fitsIn(body, small, tr(S::library_row_rescan), kRescanPillW - kSettingPillPad);
  }
  uistr::setLang(uistr::Lang::En);
  fits(body, tr(S::settings_screen_off), kSettingValueSubW);
  fits(body, tr(S::settings_brightness), kSettingValueSubW);
  fits(small, tr(S::settings_screen_off_sub), kSettingValueSubW);
  fits(small, tr(S::settings_screen_never_sub), kSettingValueSubW);
  fits(small, tr(S::settings_brightness_sub), kSettingValueSubW);
  TEST_ASSERT_TRUE(kSettingValueSubW > 100);
  // About: a long value falls back to Small, which fits.
  char v[80];
  uistr::trf(v, sizeof(v), S::about_memory_value, 263ul, 158ul, uistr::decimal(3.2f, 1));
  fits(small, v, kAboutValueW);
  {
    char tracks[40], artists[40], albums[40];
    uistr::trnf(tracks, sizeof(tracks), S::library_tracks_n, 10000, 10000);
    uistr::trnf(artists, sizeof(artists), S::library_artists_n, 600, 600);
    uistr::trnf(albums, sizeof(albums), S::library_albums_n, 1500, 1500);
    uistr::trf(v, sizeof(v), S::about_library_value, tracks, artists, albums);
  }
  fits(small, v, kAboutValueW);
  // Bigger still, the three counts don't fit in Small: the tracks and the
  // albums (about.library_value_short, OutputPage::drawAbout()), in every
  // language.
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    char tracks[40], artists[40], albums[40];
    uistr::trnf(tracks, sizeof(tracks), S::library_tracks_n, 99999, uistr::grouped(99999));
    uistr::trnf(artists, sizeof(artists), S::library_artists_n, 9999, 9999);
    uistr::trnf(albums, sizeof(albums), S::library_albums_n, 1500, 1500);
    uistr::trf(v, sizeof(v), S::about_library_value, tracks, artists, albums);
    if (l == uistr::Lang::En) TEST_ASSERT_TRUE(small.width(v) > kAboutValueW);  // why it exists
    uistr::trf(v, sizeof(v), S::about_library_value_short, tracks, albums);
    fitsIn(body, small, v, kAboutValueW);
    if (l == uistr::Lang::En) TEST_ASSERT_EQUAL_STRING("99,999 tracks, 1500 albums", v);
  }
  uistr::setLang(uistr::Lang::En);
  // The volume sheet: "volume" whole beside the speaker's and the
  // headphones' names at 100 % (VolumeSheet::render(): whole or not at all).
  {
    const Vlw title(kVlwSansBold22);
    for (S name : {S::volume_speaker, S::volume_headphones}) {
      const int room = 320 - 12 - title.width("100%") - 12 - (46 + bold.width(tr(name)) + 6);
      fits(small, tr(S::volume_label), room);
    }
  }
  // The licence rows, each line in the font it's drawn in (the values fit
  // in Body), and the source row's two lines put back together are the URL.
  fits(small, tr(S::about_licence_label), kAboutValueW);
  fits(body, tr(S::about_licence), kAboutValueW);
  fits(small, tr(S::about_source_label), kAboutValueW);
  fits(body, kAboutSourceRepo, kAboutValueW);
  const std::string url = std::string("https://") + (tr(S::about_source_label) + strlen("Source: ")) + kAboutSourceRepo;
  TEST_ASSERT_EQUAL_STRING(kSourceUrl, url.c_str());
  // The version row: the label with the date and any 8 hex digits of the
  // ELF's hash; a release's version in Body, a long dev build's in Small
  // (tools/version.py: git describe, at most 31 characters in the image).
  for (const char* hex = "0123456789abcdef"; *hex; ++hex) {
    uistr::trf(v, sizeof(v), S::about_version_label, "2026-09-30", std::string(8, *hex).c_str());
    fits(small, v, kAboutValueW);
  }
  fits(body, "v10.10.10", kAboutValueW);
  fits(body, "v0.5.0-beta.10", kAboutValueW);
  fits(small, "v0.5.0-dev+abcdef1-dirty", kAboutValueW);
  fits(small, "v10.10.10-rc.10-9999-gabcdef12-dirty", kAboutValueW);
}

// The seek bar's readout (docs/SEEK-BAR.md section 4.2): "Release to
// cancel" in the line's width, and the group (the finger's second in
// Title, the gap, "no change" or the change in Small) within its
// kSeekReadoutW for any length up to 999:59 (a 16-hour mix), with the
// texts as SeekBar writes them. Every character is one the font has: one
// it lacks is measured as a space and drawn folded ("−" as a wider "-"),
// and the change came out "-0:…".
void test_seek_bar_texts_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, tr(S::now_playing_seek_cancel), 296);
  // The figures are all as wide (in both fonts), so the extremes below
  // stand for every time of as many digits.
  for (char d = '1'; d <= '9'; ++d) {
    const char one[2] = {d, 0};
    TEST_ASSERT_EQUAL_INT(title.width("0"), title.width(one));
    TEST_ASSERT_EQUAL_INT(small.width("0"), small.width(one));
  }
  // 4:05, 59:59, 99:59, 100:00 and 999:59: the second shown, and the
  // change all the way back or on.
  const uint32_t lengths[] = {245000, 3599000, 5999000, 6000000, 59999000};
  for (uint32_t len : lengths) {
    char big[16], change[24];
    SeekBar::timeText(len, big, sizeof(big));
    TEST_ASSERT_TRUE_MESSAGE(title.hasAll(big), big);
    const int room = kSeekReadoutW - title.width(big) - kSeekReadoutGap;
    fits(small, tr(S::now_playing_seek_stay), room);
    SeekBar::changeText(len, 0, change, sizeof(change));
    TEST_ASSERT_TRUE_MESSAGE(small.hasAll(change), change);
    fits(small, change, room);
    SeekBar::changeText(0, len, change, sizeof(change));
    TEST_ASSERT_TRUE_MESSAGE(small.hasAll(change), change);
    fits(small, change, room);
  }
  // "-0:45" whole, as drawn (it was cut to "-0:…").
  char change[24];
  SeekBar::changeText(25000, 70000, change, sizeof(change));
  TEST_ASSERT_EQUAL_STRING("-0:45", change);
  TEST_ASSERT_TRUE(small.hasAll(change));
}

// Now Playing while play waits for the headphones (PlayGate): the panel's
// lines and buttons, the notice's buttons, and the output line that says
// they aren't connected.
void test_waiting_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const int panelW = 320 - 112;  // the title strip and the rows under it
  fits(small, "Waiting for SPYDRONE\xE2\x80\xA6", kWaitTextW);
  fits(small, "Waiting for WH-1000XM4\xE2\x80\xA6", kWaitTextW);
  fits(small, tr(S::now_playing_waiting_for_headphones), kWaitTextW);
  fits(small, "try 3 of 3", kWaitTextW);
  fits(small, tr(S::now_playing_wait_looking), kWaitTextW);
  TEST_ASSERT_TRUE(kWaitTextX + kWaitTextW <= panelW);
  fits(body, tr(S::now_playing_play_on_speaker), kWaitSpeakerW - kWaitButtonPad);
  fits(body, tr(S::now_playing_wait_cancel), kWaitCancelW - kWaitButtonPad);
  TEST_ASSERT_TRUE(kWaitSpeakerX + kWaitSpeakerW < kWaitCancelX);
  TEST_ASSERT_TRUE(kWaitCancelX + kWaitCancelW <= panelW - 2);
  // The notice (a Dialog): its title, its buttons (the primary in Bold; a
  // label too wide for Body is drawn in Small).
  fits(bold, "Couldn't reach SPYDRONE", kDialogTitleW);
  TEST_ASSERT_TRUE(body.width(tr(S::now_playing_play_on_speaker)) > kDialogButtonTextW);
  fits(small, tr(S::now_playing_play_on_speaker), kDialogButtonTextW);
  fits(bold, tr(S::dialog_failed_try_again), kDialogButtonTextW);
  // The progress line's middle.
  fits(small, "SPYDRONE (not connected)", kNowPlayingMidW);
  fits(small, "Waiting, 24 of 86", kNowPlayingMidW);
  fits(small, "24 of 86 \xC2\xB7 SPYDRONE", kNowPlayingMidW);
  // ... clear of the times either side (x 12 and 308), long ones too.
  TEST_ASSERT_TRUE(12 + small.width("88:88") + 4 <= 160 - kNowPlayingMidW / 2);
  // The amber warning drawn alone has kNowPlayingWarnW: a 10-character
  // name whole, and still clear of a 16-hour mix's "999:59".
  char warn[96];
  uistr::trf(warn, sizeof(warn), S::now_playing_not_connected, "WH-1000XM4");
  TEST_ASSERT_TRUE(small.width(warn) > kNowPlayingMidW);
  fits(small, warn, kNowPlayingWarnW);
  fits(small, tr(S::now_playing_headphones_not_connected), kNowPlayingWarnW);
  TEST_ASSERT_TRUE(12 + small.width("999:59") + 4 <= 160 - kNowPlayingWarnW / 2);
  // The lost dialog's title beside its icon (x 14, the headphones' 22 px,
  // 8 px) to 12 px from the dialog's right edge (292 wide): the title with
  // no name whole; "SPYDRONE disconnected" too.
  TEST_ASSERT_EQUAL_INT(292, 14 + 22 + 8 + kDialogIconTitleW + 12);
  fits(bold, tr(S::dialog_lost_title_generic), kDialogIconTitleW);
  uistr::trf(warn, sizeof(warn), S::dialog_lost_title, "SPYDRONE");
  fits(bold, warn, kDialogIconTitleW);
}

// Now Playing's two menus (docs/QUEUE-MODES.md section 4.6): the rows'
// labels in a row, each state and detail in its row's room
// (sheet::detailRoom(), as Sheet::render() has it), the toasts on one
// line, "Playback" left of the ✕; the folder cut from the left by whole
// folders; the waiting title on one line.
void test_now_playing_menu_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  const char* const kGoTo[3] = {tr(S::now_playing_go_to_artist), tr(S::now_playing_go_to_album),
                                tr(S::now_playing_go_to_folder)};
  for (const char* l : {kGoTo[0], kGoTo[1], kGoTo[2], tr(S::now_playing_shuffle), tr(S::now_playing_repeat),
                        tr(S::now_playing_sleep_timer)}) {
    fits(body, l, 320 - 32);
  }
  // The rooms, as measured with the firmware's fonts.
  TEST_ASSERT_EQUAL_INT(183, sheet::detailRoom(body.width(kGoTo[0])));
  TEST_ASSERT_EQUAL_INT(174, sheet::detailRoom(body.width(kGoTo[1])));
  TEST_ASSERT_EQUAL_INT(177, sheet::detailRoom(body.width(kGoTo[2])));
  TEST_ASSERT_EQUAL_INT(216, sheet::detailRoom(body.width(tr(S::now_playing_shuffle))));
  TEST_ASSERT_EQUAL_INT(215, sheet::detailRoom(body.width(tr(S::now_playing_repeat))));
  TEST_ASSERT_EQUAL_INT(180, sheet::detailRoom(body.width(tr(S::now_playing_sleep_timer))));
  for (S s : {S::now_playing_off, S::now_playing_on}) {
    fits(small, tr(s), sheet::detailRoom(body.width(tr(S::now_playing_shuffle))));
  }
  for (unsigned m = 0; m < 3; ++m) {
    fits(small, tr(repeatMode(m)), sheet::detailRoom(body.width(tr(S::now_playing_repeat))));
  }
  TEST_ASSERT_TRUE(small.width("One") <= 26);
  // The sleep states as before, against the Sleep timer row's room.
  for (const char* t : {"Off", "90 min", "59 s", "End of track", "End of album", "End of queue", "Fading"}) {
    fits(small, t, sheet::detailRoom(body.width(tr(S::now_playing_sleep_timer))));
  }
  fits(small, tr(S::library_no_artist_folder), sheet::detailRoom(body.width(kGoTo[0])));
  fits(small, tr(S::library_loose_tracks), sheet::detailRoom(body.width(kGoTo[1])));
  // The refusals' toasts, and the sheet's title.
  fits(body, tr(S::now_playing_builtin_not_in_library), kToastTextRight - kToastTextX);
  fits(body, tr(S::now_playing_library_not_ready), kToastTextRight - kToastTextX);
  fits(small, tr(S::now_playing_playback_title), kSleepTitleW);
  // The folder: cut from the left by whole folders to Go to folder's room.
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  const int room = sheet::detailRoom(body.width(kGoTo[2]));
  char out[160];
  TEST_ASSERT_TRUE(small.width("/music/Daft Punk/Discovery") > room);
  textfit::cutPathLeft(f, "/music/Daft Punk/Discovery", room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6/Daft Punk/Discovery", out);
  fits(small, out, room);
  textfit::cutPathLeft(f, "/music/Kavinsky", room, out, sizeof(out));  // fits: whole
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky", out);
  textfit::cutPathLeft(f, "/music", room, out, sizeof(out));  // a track at the root
  TEST_ASSERT_EQUAL_STRING("/music", out);
  // A last folder too wide even alone: "/<last>" (the draw cuts its end).
  const std::string wide(40, 'W');
  textfit::cutPathLeft(f, ("/music/Some Artist/" + wide).c_str(), room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(("/" + wide).c_str(), out);
  // A deep path keeps as many folders as fit.
  textfit::cutPathLeft(f, "/music/a/b/c/Daft Punk/Discovery", room, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6/b/c/Daft Punk/Discovery", out);  // (never "/a/b/c/...": a cut says so)
  // The waiting title: one line of Bold 16 in the title's 190 px.
  textfit::Font b;
  b.ctx = const_cast<Vlw*>(&bold);
  b.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  const char* title = "Le voyage de P\xC3\xA9n\xC3\xA9lope (Remastered Edition)";
  textfit::fit(b, title, strlen(title), out, sizeof(out), 190);
  fits(bold, out, 190);
  fits(bold, "One More Time", 190);
}

// The sleep timer (ENERGY.md section 3): the "..." row and its state, the
// sheet's pills, the fade's toast, and the moon's text on Now Playing's
// progress line.
void test_sleep_timer_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  // The playback menu's row (Body from x 16), its state right-aligned
  // (Small) in what the label leaves (Sheet::render: 16 px each side and
  // between).
  const int stateRoom = sheet::detailRoom(body.width(tr(S::now_playing_sleep_timer)));
  for (const char* t : {"Off", "90 min", "59 s", "End of track", "End of album", "End of queue", "Fading"}) {
    fits(small, t, stateRoom);
  }
  // The sheet's title (Small), left of its close pill: SleepTimer::
  // titleText(), "Sleep timer: " and its state lower case (as the toasts),
  // in every state: off, each end-of choice, the longest counts, fading.
  {
    std::vector<std::string> titles;
    auto add = [&](const SleepTimer& t, uint32_t now) {
      char buf[96];
      t.titleText(now, buf, sizeof(buf));
      titles.push_back(buf);
    };
    SleepTimer t;
    add(t, 0);
    for (SleepTimer::Choice c : {SleepTimer::Choice::EndOfTrack, SleepTimer::Choice::EndOfAlbum,
                                 SleepTimer::Choice::EndOfQueue}) {
      t.setEnd(c);
      add(t, 0);
    }
    t.setTimed(90 * 60000, 0);
    t.extend(0, 0);  // 100 min left
    add(t, 0);
    add(t, 100 * 60000 - 59000);  // 59 s
    for (const std::string& s : titles) {
      fits(small, s.c_str(), kSleepTitleW);
      TEST_ASSERT_NULL(strstr(s.c_str(), ": End"));  // no capital after the colon
    }
    TEST_ASSERT_EQUAL_STRING("Sleep timer: end of album", titles[2].c_str());
    TEST_ASSERT_EQUAL_STRING("Sleep timer: 100 min left", titles[4].c_str());
    fits(small, "Sleep timer: fading", kSleepTitleW);
  }
  // The pills: each label in its pill less the pad; the rows inside the
  // screen with gaps between the pills.
  for (int i = 0; i < 5; ++i) {
    fits(body, i < 4 ? kSleepMinutes[i] : tr(S::sleep_minutes_90), kSleepMinW[i] - kSleepPillPad);
    if (i > 0) TEST_ASSERT_TRUE(kSleepMinX[i - 1] + kSleepMinW[i - 1] < kSleepMinX[i]);
  }
  TEST_ASSERT_TRUE(kSleepMinX[4] + kSleepMinW[4] <= 312);
  for (int i = 0; i < 3; ++i) {
    const S kSleepEnds[3] = {S::sleep_end_track, S::sleep_end_album, S::sleep_end_queue};
    TEST_ASSERT_TRUE(body.width(tr(kSleepEnds[1])) > kSleepEndW[1] - kSleepPillPad);  // why they are in Small
    fits(small, tr(kSleepEnds[i]), kSleepEndW[i] - kSleepPillPad);
    if (i > 0) TEST_ASSERT_TRUE(kSleepEndX[i - 1] + kSleepEndW[i - 1] < kSleepEndX[i]);
  }
  TEST_ASSERT_TRUE(kSleepEndX[2] + kSleepEndW[2] <= 312);
  fits(body, tr(S::sleep_extend), kSleepExtendW - kSleepPillPad);
  fits(body, tr(S::sleep_turn_off), kSleepOffW - kSleepPillPad);
  TEST_ASSERT_TRUE(kSleepExtendX + kSleepExtendW < kSleepOffX && kSleepOffX + kSleepOffW <= 312);
  fits(small, tr(S::sleep_hint), kSleepHintW);
  // The fade's toast: its line (Small) before +10 min, the two buttons (Body).
  fits(small, tr(S::sleep_fading_toast), kSleepToastPlusX - 6 - kToastTextX);
  fits(body, tr(S::sleep_extend), kSleepToastPlusW - kSleepToastPad);
  fits(body, tr(S::sleep_turn_off), kSleepToastOffW - kSleepToastPad);
  TEST_ASSERT_TRUE(kSleepToastPlusX + kSleepToastPlusW < kSleepToastOffX && kSleepToastOffX + kSleepToastOffW <= 312);
  // The toasts a choice gives (one line, no buttons).
  for (const char* t : {"Sleep timer: 90 min", "Sleep timer: end of album", "Sleep timer: end of queue",
                        "Sleep timer off", "Sleep timer: 100 min left", "Sleep timer: 59 s left"}) {
    fits(body, t, kToastTextRight - kToastTextX);
  }
  // Now Playing's progress line: the moon and its text alone always fit,
  // and after "4 of 16 · Speaker" (a long headphone name leaves it alone).
  for (const char* t : {"90 min", "59 s", "track", "album", "queue", "fading"}) {
    fits(small, t, kNowPlayingMidW - kSleepMoonW);
  }
  fits(small, "4 of 16 · Speaker", kNowPlayingMidW - kSleepGap - kSleepMoonW - small.width("23 min"));
  // What gives way when the line and the moon don't both fit
  // (sleepLineFit()): the output's name first, so the queue position stays.
  using Text = SleepLine::Text;
  const int moon23 = kSleepMoonW + small.width("23 min");
  const int moon90 = kSleepMoonW + small.width("90 min");
  SleepLine f = sleepLineFit(small.width("4 of 16 · Speaker"), small.width("4 of 16"), -1, moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Full, f.text);
  f = sleepLineFit(small.width("4 of 16 · SPYDRONE"), small.width("4 of 16"), -1, moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Base, f.text);  // "4 of 16" and the moon, not the moon alone
  TEST_ASSERT_TRUE(f.moon && f.moonText);
  TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  // Paused, with a long queue: the position still shows beside the moon.
  for (const char* base : {"Paused, 12 of 160", "Paused, 160 of 160", "Stopped, 99 of 999"}) {
    const std::string full = std::string(base) + " · SPYDRONE";
    f = sleepLineFit(small.width(full.c_str()), small.width(base), -1, moon90, kNowPlayingMidW);
    TEST_ASSERT_EQUAL(Text::Base, f.text);
    TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  }
  // The headphones not connected (the drop during the countdown pauses):
  // the amber warning never gives way to the timer, whatever the name.
  for (const char* warn : {"SPYDRONE (not connected)", "Headphones (not connected)",
                           "WH-1000XM4 Long Name (not connected)"}) {
    for (int moonText : {kSleepMoonW + small.width("45 s"), moon90, kSleepMoonW + small.width("fading")}) {
      f = sleepLineFit(small.width(warn), small.width("Paused, 4 of 16"), small.width(warn), moonText, kNowPlayingMidW);
      TEST_ASSERT_TRUE(f.text == Text::Warn || f.text == Text::Full);
      TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW || !f.moon);
    }
  }
  // "SPYDRONE (not connected)" (Small, 177 px measured: the room less the
  // moon is 171): the warning alone, the timer on the tab bar's badge.
  f = sleepLineFit(small.width("SPYDRONE (not connected)"), small.width("Paused, 4 of 16"),
                   small.width("SPYDRONE (not connected)"), moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Warn, f.text);
  TEST_ASSERT_TRUE(f.width <= kNowPlayingMidW);
  // A short name keeps the moon (and its text) beside the warning.
  f = sleepLineFit(small.width("Buds (not connected)"), small.width("Paused, 4 of 16"),
                   small.width("Buds (not connected)"), moon23, kNowPlayingMidW);
  TEST_ASSERT_EQUAL(Text::Warn, f.text);
  TEST_ASSERT_TRUE(f.moon);
  (void)bold;
}

// The idle power-off (IdlePolicy; ENERGY.md item 4): the setting's row
// (as the screen's), the warning toast and its button, and the next boot's
// toast for every length.
void test_idle_power_off_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  for (int c = 0; c < IdlePolicy::kChoices; ++c) fits(body, IdlePolicy::choiceLabel(c), kSettingPillW - kSettingPillPad);
  fits(body, tr(S::settings_idle_off), kSettingValueSubW);
  fits(small, tr(S::settings_idle_off_sub), kSettingValueSubW);
  fits(small, tr(S::settings_idle_never_sub), kSettingValueSubW);
  // The warning: its longest count, then [Keep on] to the edge.
  char t[48];
  IdlePolicy::warnText(30, t, sizeof(t));
  fits(body, t, kIdleToastKeepX - 6 - kToastTextX);
  fits(body, tr(S::settings_idle_keep_on), kIdleToastKeepW - kIdleToastPad);
  TEST_ASSERT_TRUE(kIdleToastKeepX + kIdleToastKeepW <= 312);
  // The next boot's toast: each setting, and the console's test lengths.
  for (uint32_t ms : {10u * 60000u, 20u * 60000u, 60u * 60000u, 60000u, 59000u, 90u * 60000u}) {
    IdlePolicy::offText(ms, t, sizeof(t));
    fits(body, t, kToastTextRight - kToastTextX);
  }
}

// CPU speed and Bluetooth power (PowerChoices; ENERGY.md items 6 and 7):
// the rows (as the screen's), every choice in its pill and every line
// beside it, the restart dialog, its toast, the next boot's toast, and
// About's power row.
void test_power_settings_texts_fit() {
  using namespace uitext;
  namespace pc = powerchoice;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  fits(body, tr(S::settings_cpu), kSettingValueSubW);
  fits(body, tr(S::settings_bt_power), kSettingValueSubW);
  char buf[64];
  for (uint16_t mhz : pc::kCpuMhz) {
    fits(body, pc::cpuLabel(mhz), kSettingPillW - kSettingPillPad);
    fits(small, pc::cpuSub(mhz, mhz, buf, sizeof(buf)), kSettingValueSubW);
    // Saved, not running yet (the console's Pcb): what runs until a restart.
    fits(small, pc::cpuSub(pc::otherCpuMhz(mhz), mhz, buf, sizeof(buf)), kSettingValueSubW);
    pc::cpuDialogTitle(mhz, buf, sizeof(buf));
    fits(bold, buf, kDialogTitleW);
    pc::cpuRestartingText(mhz, buf, sizeof(buf));
    fits(body, buf, kToastTextRight - kToastTextX);
    pc::cpuBootText(mhz, buf, sizeof(buf));
    fits(body, buf, kToastTextRight - kToastTextX);
  }
  for (int c = 0; c < pc::kBtChoices; ++c) {
    fits(body, pc::btLabel(c), kSettingPillW - kSettingPillPad);
    fits(small, pc::btSub(c, false), kSettingValueSubW);
  }
  fits(small, pc::btSub(pc::kBtNormal, true), kSettingValueSubW);
  // The dialog's bodies (to 240, and to 160 with its cost): 3 lines of
  // Small over 260 px, none cut, within the dialog's 128 bytes.
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* s) { return static_cast<const Vlw*>(ctx)->width(s); };
  for (uint16_t mhz : pc::kCpuMhz) {
    const char* dialogBody = pc::cpuDialogBody(mhz);
    TEST_ASSERT_TRUE(strlen(dialogBody) < 128);
    char lines[4][96];
    const int n = textfit::wrap(f, dialogBody, strlen(dialogBody), kDialogTitleW, 3, &lines[0][0], sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 3);
    std::string joined;
    for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
    TEST_ASSERT_EQUAL_STRING(dialogBody, joined.c_str());
  }
  fits(bold, tr(S::settings_cpu_restart), kDialogButtonTextW);
  fits(body, tr(S::settings_cpu_wait_pairing), kToastTextRight - kToastTextX);
  // About: its label (Small), and every value in Body (all fit; Small is
  // the fallback).
  fits(small, tr(S::about_power), kAboutValueW);
  for (uint16_t mhz : pc::kCpuMhz) {
    for (int c = 0; c < pc::kBtChoices; ++c) {
      pc::aboutText(mhz, c, buf, sizeof(buf));
      fits(body, buf, kAboutValueW);
    }
  }
}

// The touch calibration: its Output row, sheet, dialog and toast; the
// calibration screen's header, lines, rows and A hints; the first-boot
// check's; the boot screen's rescue line (docs: README's touch section).
void test_touch_calibration_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  // Output: the row, its sheet (Calibrate the primary, its detail beside
  // it), the dialog, the toast.
  fits(body, tr(S::settings_cal), kSettingSubW);
  fits(small, tr(S::settings_cal_off), kSettingSubW);
  fits(small, tr(S::settings_cal_on), kSettingSubW);
  const int sheetLabelW = 320 - 32;
  fits(bold, tr(S::settings_cal_calibrate), sheetLabelW);
  fits(small, tr(S::settings_cal_calibrate_detail), 320 - 16 - (16 + bold.width(tr(S::settings_cal_calibrate)) + 16));
  fits(body, tr(S::settings_cal_test_taps), sheetLabelW);
  fits(body, tr(S::settings_cal_remove_row), sheetLabelW);
  fits(bold, tr(S::settings_cal_remove_title), kDialogTitleW);
  textfit::Font f;
  f.ctx = const_cast<Vlw*>(&small);
  f.width = [](void* ctx, const char* t) { return static_cast<const Vlw*>(ctx)->width(t); };
  {
    TEST_ASSERT_TRUE(strlen(tr(S::settings_cal_remove_body)) < 128);
    char lines[4][96];
    const char* removeBody = tr(S::settings_cal_remove_body);
    const int n = textfit::wrap(f, removeBody, strlen(removeBody), kDialogTitleW, 3, &lines[0][0],
                                sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 3);
    std::string joined;
    for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
    TEST_ASSERT_EQUAL_STRING(tr(S::settings_cal_remove_body), joined.c_str());
  }
  fits(bold, tr(S::settings_cal_remove), kDialogButtonTextW);
  fits(body, tr(S::settings_cal_removed), kToastTextRight - kToastTextX);

  // The header: each pill before the title, the titles, the progress.
  for (const char* pill : {tr(S::calibration_cancel), tr(S::calibration_skip)}) {
    TEST_ASSERT_TRUE(kCalPillX + bold.width(pill) + kCalPillPad <= kCalTitleX - 4);
  }
  fits(bold, tr(S::calibration_title), kCalTitleW);
  fits(bold, tr(S::calibration_check_title), kCalTitleW);
  fits(bold, tr(S::settings_cal_test_taps), kCalTitleW);
  fits(small, "9 of 9", kCalProgressW);
  TEST_ASSERT_TRUE(kCalTitleX + kCalTitleW + 6 + kCalProgressW <= 312);
  // The test taps page (no pill): its title from x 10, its status after a
  // Save or an Undo in what that leaves (CalibrationScreen::drawHeader()).
  fits(small, tr(S::calibration_saved), kCalStatusW);
  fits(small, tr(S::calibration_undone), kCalStatusW);
  TEST_ASSERT_TRUE(10 + kCalTitleW + 6 + kCalStatusW <= 312);
  TEST_ASSERT_TRUE(kCalStatusW > kCalProgressW);
  // The Cancel pill's hit area covers it.
  TEST_ASSERT_TRUE(kCalPillX + bold.width(tr(S::calibration_cancel)) + kCalPillPad <= touchcheck::kCancelW);
  TEST_ASSERT_EQUAL_INT(kCalHeaderH, touchcheck::kCancelH);

  // The lines (Body; the result's numbers fall back to Small).
  char buf[96];
  char hint[160];
  uistr::trnf(hint, sizeof(hint), S::calibration_first_hint, 9, 9);
  fits(body, lineOf(S::calibration_first_hint, 0).c_str(), kCalLineW);
  uistr::line(hint, 1, buf, sizeof(buf));
  fits(body, buf, kCalLineW);
  std::vector<std::string> calLines = {tr(S::calibration_hint), tr(S::calibration_missed), tr(S::check_hint),
                                       tr(S::check_missed), tr(S::calibration_save_line), tr(S::calibration_accurate),
                                       tr(S::calibration_no_better), tr(S::check_go_on)};
  for (S two : {S::check_first_hint, S::calibration_check_lines, S::calibration_disagree}) {
    for (int k = 0; k < 2; ++k) calLines.push_back(lineOf(two, k));
  }
  for (const std::string& t : calLines) {
    fits(body, t.c_str(), kCalLineW);
  }
  fits(bold, tr(S::check_ask), kCalLineW);
  fits(title, tr(S::check_accurate), kCalLineW);
  // Typical results in Body; the widest possible in Small.
  touchcheck::errorText(touchcheck::Table::InUse, touchcheck::Error{42, 21}, buf, sizeof(buf));
  fits(body, buf, kCalLineW);
  touchcheck::errorText(touchcheck::Table::New, touchcheck::Error{5, 3}, buf, sizeof(buf));
  fits(body, buf, kCalLineW);
  touchcheck::errorText(touchcheck::Table::New, touchcheck::Error{999, 999}, buf, sizeof(buf));
  fits(small, buf, kCalLineW);
  // The check's verdict: two Body lines at most, the longest way and size.
  f.ctx = const_cast<Vlw*>(&body);
  for (touchcheck::Dir d : {touchcheck::Dir::Right, touchcheck::Dir::Left, touchcheck::Dir::Below,
                            touchcheck::Dir::Above, touchcheck::Dir::None}) {
    touchcheck::verdictText(touchcheck::Verdict{true, 300, d}, buf, sizeof(buf));
    char lines[3][64];
    const int n = textfit::wrap(f, buf, strlen(buf), kCalLineW, 3, &lines[0][0], sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 2);
  }

  // The rows: centred labels, and labels with their details.
  const int rowText = kCalRowTextW;
  for (S id : {S::calibration_save, S::calibration_try_again, S::calibration_discard, S::calibration_done}) {
    const char* t = tr(id);
    fits(bold, t, rowText);
    fits(body, t, rowText);
  }
  TEST_ASSERT_TRUE(bold.width(tr(S::settings_cal_calibrate)) + small.width(tr(S::settings_cal_calibrate_detail)) +
                       3 * kCalRowPad <=
                   kCalRowW);
  TEST_ASSERT_TRUE(body.width(tr(S::check_not_now)) + small.width(tr(S::check_later)) + 3 * kCalRowPad <= kCalRowW);
  // ... in every language: the detail beside its label when it fits whole
  // in what that language's label leaves it, else under it, whole
  // (CalibrationScreen::drawRow(): German's "Ausgabe > Touch-Kalibrierung",
  // 198 px beside "Nicht jetzt"'s 188, was cut). English's beside, as before.
  {
    // The two lines inside the button (y 2-37), neither over the other.
    TEST_ASSERT_TRUE(kCalRowLabelY - body.yAdvance / 2 >= 2 && kCalRowLabelY - bold.yAdvance / 2 >= 2);
    TEST_ASSERT_TRUE(kCalRowLabelY - body.yAdvance / 2 + body.yAdvance <= kCalRowDetailY - small.yAdvance / 2);
    TEST_ASSERT_TRUE(kCalRowLabelY - bold.yAdvance / 2 + bold.yAdvance <= kCalRowDetailY - small.yAdvance / 2);
    TEST_ASSERT_TRUE(kCalRowDetailY - small.yAdvance / 2 + small.yAdvance <= 38);
    std::string report;
    for (uistr::Lang l : kAllLangs) {
      if (!uistr::hasCatalogue(l)) continue;
      uistr::setLang(l);
      struct CalRow {
        S label, detail;
        bool primary;
      };
      for (const CalRow r : {CalRow{S::settings_cal_calibrate, S::settings_cal_calibrate_detail, true},
                             CalRow{S::check_not_now, S::check_later, false}}) {
        const Vlw& lf = r.primary ? bold : body;
        const char* label = tr(r.label);
        const char* detail = tr(r.detail);
        fits(lf, label, rowText);
        if (!calDetailBelow(lf.width(label), small.width(detail))) {
          fits(small, detail, kCalRowW - 3 * kCalRowPad - lf.width(label));
          continue;
        }
        fits(small, detail, rowText);
        TEST_ASSERT_TRUE_MESSAGE(l != uistr::Lang::En, detail);
        char m[160];
        snprintf(m, sizeof(m), "%s \"%s\" under \"%s\"; ", uistr::tag(l), detail, label);
        report += m;
      }
    }
    TEST_MESSAGE(report.c_str());
    uistr::setLang(uistr::Lang::En);
  }
  // Three rows, the lines above them, and the A hint under them.
  TEST_ASSERT_TRUE(kCalRowsBottom - 3 * 40 >= 86 + 11 && kCalRowsBottom <= kCalHintY);

  // The A hints: "A: " and each page's verb.
  for (S id : {S::calibration_cancel, S::calibration_skip, S::check_not_now, S::calibration_discard,
               S::calibration_done, S::calibration_undo}) {
    const char* verb = tr(id);
    uistr::trf(buf, sizeof(buf), S::calibration_a_hint, verb);
    fits(small, buf, kCalAHintW);
  }
  // The test taps page's key, after its A hint (Done, or Undo after a
  // Save; 6 px clear of it) and its dot, to the band's end.
  for (const char* verb : {tr(S::calibration_done), tr(S::calibration_undo)}) {
    uistr::trf(buf, sizeof(buf), S::calibration_a_hint, verb);
    TEST_ASSERT_TRUE(kCalAHintX + small.width(buf) + 6 <= kCalKeyX);
  }
  TEST_ASSERT_TRUE(kCalKeyX + 6 < kCalKeyTextX);
  fits(small, tr(S::calibration_check_key), kCalKeyTextW);
  TEST_ASSERT_EQUAL_INT(312, kCalKeyTextX + kCalKeyTextW);
  // The boot screen's rescue line.
  fits(small, tr(S::boot_touch_hint), kCalLineW);
}

// With no correction (the default, until the owner calibrates), a panel
// that reads x too far right like the lab's (TouchCalibration::labFitX():
// +20 px at x 190, +35-45 from 240) must still hit the left one of two
// controls side by side on the right half when the finger is on its
// centre: that is the "taps are off" state the calibration is for, and
// View -> Undo takes an add back. On a panel that reads true, each centre
// is its own control too.
void test_right_half_neighbours_survive_the_lab_panel() {
  using namespace uitext;
  const TouchCalibration::Axis lab = TouchCalibration::labFitX();
  struct Reading {
    int x;
    bool edge;
  };
  auto lab_ = [&](int x) {
    const long r = std::lround(lab.unmap(static_cast<float>(x)));
    return r >= 319 ? Reading{319, true} : Reading{static_cast<int>(r), false};
  };
  auto truth = [](int x) { return Reading{x, x >= 319}; };
  for (auto read : {std::function<Reading(int)>(lab_), std::function<Reading(int)>(truth)}) {
    // The toast's View and Undo, on one line and on two.
    for (bool compact : {false, true}) {
      const int viewC = compact ? kToastViewCX + kToastViewCW / 2 : kToastViewX + kToastViewW / 2;
      const int undoC = compact ? kToastUndoCX + kToastUndoCW / 2 : kToastUndoX + kToastUndoW / 2;
      Reading r = read(viewC);
      char msg[64];
      snprintf(msg, sizeof(msg), "View (%s) at %d reads %d", compact ? "icons" : "words", viewC, r.x);
      TEST_ASSERT_EQUAL_INT_MESSAGE(3, toastButtonAt(r.x, r.edge, compact, true, true), msg);
      r = read(undoC);
      TEST_ASSERT_EQUAL_INT(2, toastButtonAt(r.x, r.edge, compact, true, true));
      // The text never runs under View.
      TEST_ASSERT_TRUE((compact ? kToastCompactTextRight : kToastViewX - 6) <= (compact ? kToastViewCX : kToastViewX));
    }
    // The sleep timer's fade toast: +10 min next to Turn off.
    Reading r = read(kSleepToastPlusX + kSleepToastPlusW / 2);
    TEST_ASSERT_EQUAL_INT((int)SleepTimer::ToastButton::Extend, (int)SleepTimer::toastTap(r.x, r.edge, false));
    r = read(kSleepToastOffX + kSleepToastOffW / 2);
    TEST_ASSERT_TRUE(SleepTimer::toastTap(r.x, r.edge, false) != SleepTimer::ToastButton::Extend);
    // The tab bar: every tab's centre (Dance next to Output at 216).
    for (int t = 0; t < tabbar::kTabs; ++t) {
      const int centre = (tabbar::cellX0(t) + tabbar::cellX1(t)) / 2;
      r = read(centre);
      TEST_ASSERT_EQUAL_INT(t, tabbar::tabAt(r.x, r.edge));
    }
    // The speaker card: its volume chip's centre is the chip, its radio
    // (x 290) isn't.
    r = read(kSpeakerChipX + kSpeakerChipW / 2);
    TEST_ASSERT_TRUE(!r.edge && r.x >= kSpeakerChipHitX && r.x < kSpeakerChipHitEnd);
    r = read(290);
    TEST_ASSERT_TRUE(r.edge || r.x >= kSpeakerChipHitEnd);
  }
  // The compact toast still has the room its long names need (Small).
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  fits(small, "Harder, Better, Faster, Stronger", kToastCompactTextRight - kToastTextX);
  // The buttons are icons (the geometry and hit test above) when a label
  // doesn't fit its pill (toastIcons()): either one, both or neither.
  TEST_ASSERT_FALSE(toastIcons(true, bold.width(tr(S::toast_undo)), true, bold.width(tr(S::toast_view))));
  TEST_ASSERT_FALSE(toastIcons(true, kToastUndoW - kToastLabelPad, true, kToastViewW - kToastLabelPad));
  TEST_ASSERT_TRUE(toastIcons(true, kToastUndoW - kToastLabelPad + 1, false, 0));  // Polish's "Cofnij", 51 px
  TEST_ASSERT_TRUE(toastIcons(true, 46, true, kToastViewW - kToastLabelPad + 1));  // View alone too wide
  TEST_ASSERT_FALSE(toastIcons(true, 46, false, 999));                              // no View: its label is no matter
  TEST_ASSERT_FALSE(toastIcons(false, 999, false, 999));                            // no buttons
  // Every language: words only where both fit their pills.
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    const int undoW = bold.width(tr(S::toast_undo)), viewW = bold.width(tr(S::toast_view));
    if (!toastIcons(true, undoW, true, viewW)) {
      fits(bold, tr(S::toast_undo), kToastUndoW - kToastLabelPad);
      fits(bold, tr(S::toast_view), kToastViewW - kToastLabelPad);
    }
  }
  uistr::setLang(uistr::Lang::En);
}

// ---- the Library's names from the tags (docs/METADATA.md 5.4, 3.6; N9) ----

namespace {

// A record's view with the fields a test gives (the rest absent).
struct View {
  LibraryIndex::TagView v;
  View& title(const char* s) { return set(&v.title, &v.titleLen, s); }
  View& artist(const char* s) { return set(&v.artist, &v.artistLen, s); }
  View& album(const char* s) { return set(&v.album, &v.albumLen, s); }
  View& albumArtist(const char* s) { return set(&v.albumArtist, &v.albumArtistLen, s); }
  View& artistSort(const char* s) { return set(&v.artistSort, &v.artistSortLen, s); }
  View& albumSort(const char* s) { return set(&v.albumSort, &v.albumSortLen, s); }
  View& year(uint16_t y) {
    v.year = y;
    return *this;
  }
  View& track(uint16_t t) {
    v.track = t;
    return *this;
  }
  View& disc(uint16_t d) {
    v.disc = d;
    return *this;
  }
  View& ms(uint32_t d) {
    v.durationMs = d;
    return *this;
  }
  View& compilation() {
    v.compilation = 1;
    return *this;
  }
  View& transfer() {
    v.source = LibraryIndex::kFromTransfer;
    return *this;
  }
  View& set(const char** p, size_t* n, const char* s) {
    *p = s;
    *n = strlen(s);
    return *this;
  }
};

// The tagged library of these tests: made-up names (no real library's).
//   Lantern Choir/      (its tags say "The Lantern Choir")
//     First Light/      2001, three tracks, a guest on the third
//     Second Wind/      2010, one track
//     Demos/            no records: named by its paths
//     04 - Stray.mp3    the artist folder's loose track, tagged with an album
//   Various/Summer Mix/ a compilation of two artists, 2015
//   Orchard Hum/Archive/CD1, CD2: two discs, the tags say so
void buildTagged(LibraryIndex& idx) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  auto add = [&](const char* path, const View& v) {
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addRecord(path, v.v));
  };
  const char* lc = "The Lantern Choir";
  add("/music/Lantern Choir/First Light/01 - Dawn.mp3",
      View().title("Dawn").artist(lc).album("First Light").albumArtist(lc).year(2001).track(1).ms(201000));
  add("/music/Lantern Choir/First Light/02 - Noon.mp3",
      View().title("Noon").artist(lc).album("First Light").albumArtist(lc).year(2001).track(2).ms(1000));
  add("/music/Lantern Choir/First Light/03 - Dusk.mp3", View()
                                                            .title("Dusk")
                                                            .artist("The Lantern Choir\x1FMara Quill")
                                                            .album("First Light")
                                                            .albumArtist(lc)
                                                            .year(2001)
                                                            .track(3)
                                                            .ms(1000));
  add("/music/Lantern Choir/Second Wind/01 - Gale.flac",
      View().title("Gale").artist(lc).album("Second Wind").year(2010).track(1).ms(199400).transfer());
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile("/music/Lantern Choir/Demos/01 - Sketch.mp3"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addFile("/music/Lantern Choir/Demos/02 - Sketch Two.mp3", LibraryIndex::kAddPending));
  add("/music/Lantern Choir/04 - Stray.mp3", View().title("Stray").artist(lc).album("A Single").year(2005));
  add("/music/Orchard Hum/Archive/CD1/01 - One.mp3", View().title("One").artist("Orchard Hum").album("Archive").disc(1).track(1));
  add("/music/Orchard Hum/Archive/CD1/02 - Two.mp3", View().title("Two").artist("Orchard Hum").album("Archive").disc(1).track(2));
  add("/music/Orchard Hum/Archive/CD2/01 - Three.mp3",
      View().title("Three").artist("Orchard Hum").album("Archive").disc(2).track(1));
  add("/music/Orchard Hum/Archive/CD2/02 - Four.mp3",
      View().title("Four").artist("Orchard Hum").album("Archive").disc(2).track(2));
  add("/music/Various/Summer Mix/01 - Wave.mp3",
      View().title("Wave").artist("Ola Brenmark").album("Summer Mix").year(2015).track(1).compilation());
  add("/music/Various/Summer Mix/02 - Tide.mp3",
      View().title("Tide").artist("Pim Vossaert").album("Summer Mix").year(2015).track(2).compilation());
  TEST_ASSERT_TRUE(idx.finish());
}

uint32_t artistNamed(const LibraryIndex& idx, const char* name) {
  for (uint32_t a = 0; a < idx.artistCount(); ++a) {
    if (strcmp(idx.artistName(a), name) == 0) return a;
  }
  TEST_FAIL_MESSAGE(name);
  return LibraryIndex::kNone;
}

uint32_t albumOf(const LibraryIndex& idx, const char* path) {
  const uint32_t t = idx.findTrack(path);
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, t);
  return idx.track(t).album;
}

std::string text(size_t (*fn)(const LibraryIndex&, uint32_t, char*, size_t), const LibraryIndex& idx, uint32_t id) {
  char b[160];
  fn(idx, id, b, sizeof(b));
  return b;
}

std::string albumSub(const LibraryIndex& idx, uint32_t album, librarytext::AlbumPlace p) {
  char b[160];
  librarytext::albumSub(idx, album, p, b, sizeof(b));
  return b;
}

std::string trackSub(const LibraryIndex& idx, const char* path, bool album) {
  char b[160];
  librarytext::trackSub(idx, idx.findTrack(path), album, b, sizeof(b));
  return b;
}

std::string titleOf(const TrackCatalog& c, uint32_t id) {
  char b[260];
  c.title(id, b, sizeof(b));
  return b;
}

}  // namespace

// An artist's page and its rows: the albums newest first with their years,
// the elected spelling of the artist folder, the album's line, a track's
// own artist only where it differs; the root's Albums with the line and the
// year; an album's header.
void test_library_rows_from_tags() {
  using librarytext::AlbumPlace;
  LibraryIndex idx;
  buildTagged(idx);
  const uint32_t lc = artistNamed(idx, "The Lantern Choir");  // the folder "Lantern Choir", elected
  TEST_ASSERT_EQUAL_STRING("4 albums, 7 tracks", text(librarytext::artistCounts, idx, lc).c_str());
  TEST_ASSERT_EQUAL_STRING("1 album, 4 tracks",
                           text(librarytext::artistCounts, idx, artistNamed(idx, "Orchard Hum")).c_str());
  // Newest first: 2010, 2001, then the ones with no year.
  const LibraryIndex::Span albums = idx.albumsOf(lc);
  TEST_ASSERT_EQUAL_UINT32(4, albums.count);
  TEST_ASSERT_EQUAL_STRING("Second Wind", idx.albumName(albums[0]));
  TEST_ASSERT_EQUAL_STRING("First Light", idx.albumName(albums[1]));
  TEST_ASSERT_EQUAL_STRING("2010 \xC2\xB7 1 track", albumSub(idx, albums[0], AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING("2001 \xC2\xB7 3 tracks", albumSub(idx, albums[1], AlbumPlace::OfArtist).c_str());
  const uint32_t first = albumOf(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3");
  const uint32_t demos = albumOf(idx, "/music/Lantern Choir/Demos/01 - Sketch.mp3");
  const uint32_t loose = albumOf(idx, "/music/Lantern Choir/04 - Stray.mp3");
  const uint32_t mix = albumOf(idx, "/music/Various/Summer Mix/01 - Wave.mp3");
  TEST_ASSERT_EQUAL_STRING("2 tracks", albumSub(idx, demos, AlbumPlace::OfArtist).c_str());
  // The loose tracks keep no name and no year, whatever their tags say.
  TEST_ASSERT_EQUAL_STRING("1 track", albumSub(idx, loose, AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING(tr(S::library_loose_tracks), librarytext::albumShown(idx, loose));
  // The root's Albums: the line and the year; an album with no records
  // shows its artist's (elected) name.
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2001", albumSub(idx, first, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", albumSub(idx, demos, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("Various Artists \xC2\xB7 2015", albumSub(idx, mix, AlbumPlace::AZ).c_str());
  // A compilation's line is the UI's words (LibraryIndex::kVarious): in
  // the pseudo-locale here, the index keeping its English.
  uistr::setLang(uistr::Lang::EnXA);
  TEST_ASSERT_EQUAL_STRING(tr(S::library_various_artists), librarytext::albumArtist(idx, mix));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(albumSub(idx, mix, AlbumPlace::AZ).find(tr(S::library_various_artists))));
  TEST_ASSERT_EQUAL_STRING("Various Artists", idx.albumArtistLine(mix));
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", librarytext::albumArtist(idx, first));  // a name stays
  uistr::setLang(uistr::Lang::En);
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2001 \xC2\xB7 3 tracks",
                           text(librarytext::albumHeader, idx, first).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 2 tracks", text(librarytext::albumHeader, idx, demos).c_str());
  // The tracks: a subtitle only where the artist isn't the album's line.
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill",
                           trackSub(idx, "/music/Lantern Choir/First Light/03 - Dusk.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("Ola Brenmark", trackSub(idx, "/music/Various/Summer Mix/01 - Wave.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Lantern Choir/Demos/01 - Sketch.mp3", false).c_str());
  // An artist's All tracks: the album after it.
  TEST_ASSERT_EQUAL_STRING("First Light", trackSub(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3", true).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill \xC2\xB7 First Light",
                           trackSub(idx, "/music/Lantern Choir/First Light/03 - Dusk.mp3", true).c_str());
  TEST_ASSERT_EQUAL_STRING("(loose tracks)", trackSub(idx, "/music/Lantern Choir/04 - Stray.mp3", true).c_str());
  // A text cut to its buffer: "The Lantern Choir, Mara Quill" in 9 bytes.
  char small[9];
  librarytext::trackSub(idx, idx.findTrack("/music/Lantern Choir/First Light/03 - Dusk.mp3"), false, small,
                        sizeof(small));
  TEST_ASSERT_EQUAL_STRING("The Lant", small);
  char tiny[4];
  librarytext::albumSub(idx, first, AlbumPlace::OfArtist, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("200", tiny);
  // ... never mid-character: "\xC3\x89lan Vey" (its first letter two
  // bytes) in 2 bytes is "", in 3 the letter whole.
  LibraryIndex accents;
  TEST_ASSERT_TRUE(accents.begin("/music"));
  accents.addFile("/music/\xC3\x89lan Vey/Set/01 - a.mp3");
  TEST_ASSERT_TRUE(accents.finish());
  char two[2], three[3];
  librarytext::albumSub(accents, 0, AlbumPlace::AZ, two, sizeof(two));
  TEST_ASSERT_EQUAL_STRING("", two);
  librarytext::albumSub(accents, 0, AlbumPlace::AZ, three, sizeof(three));
  TEST_ASSERT_EQUAL_STRING("\xC3\x89", three);
}

// The rows keep working with no tags at all (no tags.bin, no transfer: the
// index of paths, as before N9): the folder names, no years, no subtitles
// on tracks; the files right under /music are the "(no artist folder)"
// entity; a path-only album with "1-01" names still gets its dividers.
void test_library_rows_from_paths() {
  using librarytext::AlbumPlace;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const char* p : {"/music/Glass Orchard/Morning Set/01 - Opening.mp3", "/music/Glass Orchard/Morning Set/02 - Second.mp3",
                        "/music/Glass Orchard/Late Set/1-01 Intro.mp3", "/music/Glass Orchard/Late Set/1-02 Middle.mp3",
                        "/music/Glass Orchard/Late Set/2-01 Outro.mp3", "/music/Glass Orchard/03 - Loose One.mp3",
                        "/music/Top Level.mp3"}) {
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addFile(p));
  }
  TEST_ASSERT_TRUE(idx.finish());
  const uint32_t morning = albumOf(idx, "/music/Glass Orchard/Morning Set/01 - Opening.mp3");
  const uint32_t late = albumOf(idx, "/music/Glass Orchard/Late Set/1-01 Intro.mp3");
  const uint32_t top = albumOf(idx, "/music/Top Level.mp3");
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", albumSub(idx, morning, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING("2 tracks", albumSub(idx, morning, AlbumPlace::OfArtist).c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard \xC2\xB7 2 tracks", text(librarytext::albumHeader, idx, morning).c_str());
  TEST_ASSERT_EQUAL_STRING("", trackSub(idx, "/music/Glass Orchard/Morning Set/01 - Opening.mp3", false).c_str());
  TEST_ASSERT_EQUAL_STRING(tr(S::library_no_artist_folder), albumSub(idx, top, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING(tr(S::library_no_artist_folder), librarytext::artistShown(idx, idx.album(top).artist));
  TEST_ASSERT_EQUAL_STRING(tr(S::library_loose_tracks), librarytext::albumShown(idx, top));
  // The rail keys on the names (no sort tags): "The" aside, as before.
  for (uint32_t i = 0; i < idx.artistCount(); ++i) {
    const uint32_t a = idx.artistsAZ()[i];
    TEST_ASSERT_EQUAL_STRING(textfold::sortName(idx.artistName(a)),
                             librarytext::railName(idx, LibraryIndex::View::Artists, a));
  }
  // The catalog: the folder artist, the folder album, no year, no length.
  TrackCatalog c(&idx);
  const uint32_t opening = idx.findTrack("/music/Glass Orchard/Morning Set/01 - Opening.mp3");
  TEST_ASSERT_EQUAL_STRING("Opening", titleOf(c, opening).c_str());
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", c.artist(opening));
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", c.albumArtist(opening));
  TEST_ASSERT_EQUAL_STRING("Morning Set", c.album(opening));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(opening));
  TEST_ASSERT_EQUAL_UINT32(0, c.durationHintMs(opening));
  TEST_ASSERT_EQUAL_STRING("", c.artist(idx.findTrack("/music/Top Level.mp3")));  // Now Playing: now_playing.unknown_artist
  // "1-01", "1-02", "2-01": two discs from the names, a divider each.
  librarytext::Discs d;
  d.set(idx, late);
  TEST_ASSERT_EQUAL_UINT32(2, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(5, d.rows(idx.album(late).trackCount));
  d.set(idx, morning);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(2, d.rows(2));
}

// An album of two discs: "Disc 1" and "Disc 2" rows before each disc's
// first track, the tracks' places kept; one disc: no rows; discs that
// don't run in order past kMax changes: none at all.
void test_disc_dividers() {
  LibraryIndex idx;
  buildTagged(idx);
  const uint32_t archive = albumOf(idx, "/music/Orchard Hum/Archive/CD1/01 - One.mp3");
  TEST_ASSERT_EQUAL_UINT8(2, idx.album(archive).discs);
  librarytext::Discs d;
  d.set(idx, archive);
  TEST_ASSERT_EQUAL_UINT32(2, d.dividers());
  const LibraryIndex::Span t = idx.tracksOfAlbum(archive);
  TEST_ASSERT_EQUAL_UINT32(6, d.rows(t.count));
  // Rows: Disc 1, One, Two, Disc 2, Three, Four.
  const char* want[6] = {"Disc 1", "One", "Two", "Disc 2", "Three", "Four"};
  TrackCatalog c(&idx);
  for (uint32_t r = 0; r < 6; ++r) {
    const librarytext::Discs::Row row = d.at(r);
    if (row.divider) {
      char b[16];
      librarytext::discText(row.disc, b, sizeof(b));
      TEST_ASSERT_EQUAL_STRING(want[r], b);
    } else {
      TEST_ASSERT_EQUAL_STRING(want[r], titleOf(c, t[row.track]).c_str());
      TEST_ASSERT_EQUAL_UINT32(r, d.rowOf(row.track));
    }
  }
  // One disc: no dividers.
  d.set(idx, albumOf(idx, "/music/Lantern Choir/First Light/01 - Dawn.mp3"));
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(3, d.rows(3));
  TEST_ASSERT_FALSE(d.at(0).divider);
  TEST_ASSERT_EQUAL_UINT32(2, d.at(2).track);
  TEST_ASSERT_EQUAL_UINT32(1, d.rowOf(1));
  d.set(idx, LibraryIndex::kNone);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  // A path-only album whose subfolders each hold a disc 1 and a disc 2
  // ("1-01", "2-01"): sorted folder by folder, the discs alternate, a change
  // per file; past kMax changes, no dividers.
  LibraryIndex alt;
  TEST_ASSERT_TRUE(alt.begin("/music"));
  char p[96];
  for (int f = 0; f < 40; ++f) {
    snprintf(p, sizeof(p), "/music/A/Big/Part %02d/1-01 a.mp3", f);
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, alt.addFile(p));
    snprintf(p, sizeof(p), "/music/A/Big/Part %02d/2-01 b.mp3", f);
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, alt.addFile(p));
  }
  TEST_ASSERT_TRUE(alt.finish());
  const uint32_t big = albumOf(alt, "/music/A/Big/Part 00/1-01 a.mp3");
  TEST_ASSERT_EQUAL_UINT8(2, alt.album(big).discs);
  d.set(alt, big);
  TEST_ASSERT_EQUAL_UINT32(0, d.dividers());
  TEST_ASSERT_EQUAL_UINT32(80, d.rows(80));
}

// The catalog: the tag's title, the track's own artist else the album's
// line, the album's name and year, the record's length; then the overlay
// (the playing track's fresh tags, 3.3.3) for that track in that index
// only, the loose tracks still nameless, gone with clear() and with
// another build.
void test_catalog_names_and_overlay() {
  LibraryIndex idx;
  buildTagged(idx);
  TrackCatalog c(&idx);
  const uint32_t dawn = idx.findTrack("/music/Lantern Choir/First Light/01 - Dawn.mp3");
  const uint32_t dusk = idx.findTrack("/music/Lantern Choir/First Light/03 - Dusk.mp3");
  const uint32_t gale = idx.findTrack("/music/Lantern Choir/Second Wind/01 - Gale.flac");
  const uint32_t sketch = idx.findTrack("/music/Lantern Choir/Demos/01 - Sketch.mp3");
  const uint32_t stray = idx.findTrack("/music/Lantern Choir/04 - Stray.mp3");
  const uint32_t wave = idx.findTrack("/music/Various/Summer Mix/01 - Wave.mp3");
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(dawn));
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Mara Quill", c.artist(dusk));
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(sketch));  // no record: its artist's elected name
  TEST_ASSERT_EQUAL_STRING("Ola Brenmark", c.artist(wave));
  TEST_ASSERT_EQUAL_STRING("Various Artists", c.albumArtist(wave));
  TEST_ASSERT_EQUAL_STRING("First Light", c.album(dawn));
  TEST_ASSERT_EQUAL_STRING("", c.album(stray));
  TEST_ASSERT_EQUAL_UINT16(2001, c.year(dawn));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(stray));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(sketch));
  TEST_ASSERT_EQUAL_UINT32(201000, c.durationHintMs(dawn));
  TEST_ASSERT_EQUAL_UINT32(199000, c.durationHintMs(gale));  // whole seconds, rounded
  TEST_ASSERT_EQUAL_UINT32(0, c.durationHintMs(sketch));
  TEST_ASSERT_EQUAL_UINT32(60000, c.durationHintMs(TrackCatalog::builtins()[3]));  // the click tracks', as before
  // The overlay: the playing track's tags, read after the build.
  TrackCatalog::Overlay o;
  c.setOverlay(&o);
  const uint32_t v0 = c.namesVersion();
  o.set(idx, sketch,
        View().title("Sketch (Final)").artist("The Lantern Choir\x1FGuest Horn").album("Demos 2003").year(2003).ms(123456).v);
  TEST_ASSERT_TRUE(c.namesVersion() != v0);
  TEST_ASSERT_EQUAL_STRING("Sketch (Final)", titleOf(c, sketch).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir, Guest Horn", c.artist(sketch));
  TEST_ASSERT_EQUAL_STRING("Demos 2003", c.album(sketch));
  TEST_ASSERT_EQUAL_UINT16(2003, c.year(sketch));
  TEST_ASSERT_EQUAL_UINT32(123456, c.durationHintMs(sketch));
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());  // the others: the index's
  // A field the record lacks stays the index's.
  o.set(idx, dawn, View().title("Dawn (Live)").v);
  TEST_ASSERT_EQUAL_STRING("Dawn (Live)", titleOf(c, dawn).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(dawn));
  TEST_ASSERT_EQUAL_STRING("First Light", c.album(dawn));
  TEST_ASSERT_EQUAL_UINT16(2001, c.year(dawn));
  TEST_ASSERT_EQUAL_UINT32(201000, c.durationHintMs(dawn));
  TEST_ASSERT_EQUAL_STRING("Sketch", titleOf(c, sketch).c_str());  // one slot
  // The loose tracks stay nameless (the next build would show them so).
  o.set(idx, stray, View().title("Stray (Edit)").album("Another Single").year(2006).v);
  TEST_ASSERT_EQUAL_STRING("Stray (Edit)", titleOf(c, stray).c_str());
  TEST_ASSERT_EQUAL_STRING("", c.album(stray));
  TEST_ASSERT_EQUAL_UINT16(0, c.year(stray));
  // A field cut to 255 bytes at a character boundary.
  std::string longTitle;
  while (longTitle.size() < 300) longTitle += "\xC3\xA9";  // é, 2 bytes
  o.set(idx, dawn, View().title(longTitle.c_str()).v);
  TEST_ASSERT_EQUAL_UINT32(254, strlen(o.title));
  // Another build renumbers the tracks: the overlay is ignored there.
  o.set(idx, sketch, View().title("Sketch (Final)").v);
  LibraryIndex other;
  TEST_ASSERT_TRUE(other.begin("/music"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, other.addFile("/music/Lantern Choir/Demos/01 - Sketch.mp3"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, other.addFile("/music/Lantern Choir/Demos/00 - Intro.mp3"));
  TEST_ASSERT_TRUE(other.finish());
  TEST_ASSERT_TRUE(other.buildStamp() != idx.buildStamp());
  c.setIndex(&other);
  for (uint32_t i = 0; i < other.trackCount(); ++i) TEST_ASSERT_TRUE(titleOf(c, i) != "Sketch (Final)");
  c.setIndex(&idx);
  TEST_ASSERT_EQUAL_STRING("Sketch (Final)", titleOf(c, sketch).c_str());
  const uint32_t v1 = c.namesVersion();
  o.clear();
  TEST_ASSERT_TRUE(c.namesVersion() != v1);
  TEST_ASSERT_EQUAL_STRING("Sketch", titleOf(c, sketch).c_str());
  c.setOverlay(nullptr);
  TEST_ASSERT_EQUAL_UINT32(0, c.namesVersion());
  // The library update's fence (docs/METADATA.md 3.4.2, N12): the catalog
  // has no index while the build runs; the playing track's names come from
  // the copy taken before (Now Playing keeps them), its path doesn't (the
  // player starts nothing), every other library id is unknown.
  TrackCatalog::Held h;
  TEST_ASSERT_TRUE(c.take(dawn, &h));
  TEST_ASSERT_EQUAL_UINT32(dawn, h.track);
  const uint32_t v2 = c.namesVersion();
  c.setHeld(&h);
  TEST_ASSERT_TRUE(c.namesVersion() != v2);
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());  // the index still answers while it is there
  c.setIndex(nullptr);
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());
  TEST_ASSERT_EQUAL_STRING("The Lantern Choir", c.artist(dawn));
  TEST_ASSERT_EQUAL_STRING("First Light", c.album(dawn));
  TEST_ASSERT_EQUAL_UINT16(2001, c.year(dawn));
  TEST_ASSERT_EQUAL_UINT32(201000, c.durationHintMs(dawn));
  char p[TrackCatalog::kMaxPath];
  TEST_ASSERT_EQUAL_size_t(0, c.path(dawn, p, sizeof(p)));
  TEST_ASSERT_FALSE(c.valid(dawn));
  TEST_ASSERT_EQUAL_STRING("", titleOf(c, gale).c_str());
  TEST_ASSERT_EQUAL_STRING("", c.artist(gale));
  TEST_ASSERT_EQUAL_STRING("Built-in", c.album(TrackCatalog::builtins()[0]));  // the built-in tracks as ever
  // The index back (the new build's): it answers, the copy is dropped.
  c.setIndex(&idx);
  const uint32_t v3 = c.namesVersion();
  c.setHeld(nullptr);
  TEST_ASSERT_TRUE(c.namesVersion() != v3);
  TEST_ASSERT_EQUAL_STRING("Dawn", titleOf(c, dawn).c_str());
  TEST_ASSERT_FALSE(c.take(TrackCatalog::kNone, &h));
  TEST_ASSERT_EQUAL_UINT32(TrackCatalog::kNone, h.track);
}

// The A-Z rail, a row's letter and the jump grid key on the sort keys
// (an elected sort tag, else the name): "Daniel Bowery" tagged "Bowery,
// Daniel" is a B, an album tagged " Zebra Songs" a Z (its leading
// White_Space doesn't count: run 4's review), and on the synthetic tagged
// library (2 % of tracks with sort tags) the letters
// librarytext::railName() gives are the index's buckets, row for row, in
// both lists.
void test_rail_follows_the_sort_keys() {
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Daniel Bowery/Rooms/01 - Hall.mp3",
                                  View().title("Hall").artist("Daniel Bowery").artistSort("Bowery, Daniel").album("Rooms").v));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Ada Crane/The Quiet Year/01 - Snow.mp3",
                                  View().title("Snow").artist("Ada Crane").album("The Quiet Year").albumSort("Quiet Year").v));
  // An album value keeps its bytes, White_Space first too (2.3.6 stores a
  // tab as a space); its sort key doesn't (5.4's orderName: the nameKey,
  // trimmed).
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Pell Corrin/Zebra/01 - Stripe.mp3",
                                  View().title("Stripe").artist("Pell Corrin").album(" Zebra Songs").v));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Pell Corrin/Apple/01 - Core.mp3",
                                  View().title("Core").artist("Pell Corrin").album("Apple Pie").v));
  TEST_ASSERT_EQUAL(LibraryIndex::Add::Added,
                    idx.addRecord("/music/Pell Corrin/Orchard/01 - Bough.mp3",
                                  View().title("Bough").artist("Pell Corrin").album("\xE3\x80\x80The Orchard").v));
  TEST_ASSERT_TRUE(idx.finish());
  const uint32_t bowery = artistNamed(idx, "Daniel Bowery");
  TEST_ASSERT_EQUAL_STRING("Bowery, Daniel", librarytext::railName(idx, LibraryIndex::View::Artists, bowery));
  TEST_ASSERT_EQUAL('B', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Artists, bowery)));
  TEST_ASSERT_EQUAL_UINT32(bowery, idx.artistsAZ()[1]);  // after Ada Crane (A), as a B
  const uint32_t quiet = albumOf(idx, "/music/Ada Crane/The Quiet Year/01 - Snow.mp3");
  TEST_ASSERT_EQUAL('Q', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, quiet)));
  // " Zebra Songs" is shown as it is and sorts as a Z: last in the Albums
  // A-Z, in the Z bucket, not first among the '#' rows; an ideographic
  // space then "The Orchard" under O (past the article too).
  const uint32_t zebra = albumOf(idx, "/music/Pell Corrin/Zebra/01 - Stripe.mp3");
  const uint32_t orchard = albumOf(idx, "/music/Pell Corrin/Orchard/01 - Bough.mp3");
  const uint32_t apple = albumOf(idx, "/music/Pell Corrin/Apple/01 - Core.mp3");
  TEST_ASSERT_EQUAL_STRING(" Zebra Songs", idx.albumName(zebra));
  TEST_ASSERT_EQUAL_STRING(" Zebra Songs", librarytext::albumShown(idx, zebra));
  TEST_ASSERT_EQUAL_STRING("Zebra Songs", idx.albumSortKey(zebra));
  TEST_ASSERT_EQUAL_STRING("The Orchard", idx.albumSortKey(orchard));
  TEST_ASSERT_EQUAL('Z', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, zebra)));
  TEST_ASSERT_EQUAL('O', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, orchard)));
  const LibraryIndex::Span albums = idx.albumsAZ();
  TEST_ASSERT_EQUAL_UINT32(5, albums.count);
  const uint32_t want[] = {apple, orchard, quiet, albumOf(idx, "/music/Daniel Bowery/Rooms/01 - Hall.mp3"), zebra};
  for (uint32_t i = 0; i < albums.count; ++i) {
    TEST_ASSERT_EQUAL_UINT32(want[i], albums[i]);
    TEST_ASSERT_EQUAL_INT(idx.bucketAt(LibraryIndex::View::Albums, i),
                          textfold::bucketOf(textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums,
                                                                                     albums[i]))));
  }
  TEST_ASSERT_EQUAL_UINT32(0, idx.bucketStart(LibraryIndex::View::Albums, 1));  // no '#' rows
  TEST_ASSERT_EQUAL_UINT32(4, idx.bucketStart(LibraryIndex::View::Albums, textfold::bucketOf('Z')));
  // Pell Corrin's albums: no years, so A-Z by the same keys.
  const LibraryIndex::Span pell = idx.albumsOf(artistNamed(idx, "Pell Corrin"));
  TEST_ASSERT_EQUAL_UINT32(3, pell.count);
  TEST_ASSERT_EQUAL_UINT32(zebra, pell[2]);

  LibraryIndex big;
  const synth::Spec spec = synth::specFor(3000);
  TEST_ASSERT_TRUE(big.begin(spec.root, spec.tracks));
  synth::Tagged t;
  uint32_t sorted = 0;
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    TEST_ASSERT_TRUE(synth::tagged(spec, i, &t));
    if (t.noTags) {
      big.addFile(t.path);
      continue;
    }
    View v;
    v.title(t.title).artist(t.artist).album(t.album).albumArtist(t.albumArtist).year(t.year).track(t.track).disc(t.disc);
    v.artistSort(t.artistSort).albumSort(t.albumSort);
    v.set(&v.v.albumArtistSort, &v.v.albumArtistSortLen, t.albumArtistSort);
    if (t.compilation == 1) v.compilation();
    sorted += t.artistSort[0] || t.albumSort[0] || t.albumArtistSort[0];
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, big.addRecord(t.path, v.v));
  }
  TEST_ASSERT_TRUE(big.finish());
  TEST_ASSERT_TRUE(sorted > 0);
  struct Rows {
    const LibraryIndex* idx;
    LibraryIndex::View view;
    LibraryIndex::Span span;
  };
  for (const LibraryIndex::View view : {LibraryIndex::View::Artists, LibraryIndex::View::Albums}) {
    Rows rows{&big, view, view == LibraryIndex::View::Artists ? big.artistsAZ() : big.albumsAZ()};
    auto name = [](void* ctx, uint32_t row) {
      const Rows& r = *static_cast<Rows*>(ctx);
      return librarytext::railName(*r.idx, r.view, r.span[row]);
    };
    int32_t first[jump::kCells], end[jump::kCells];
    jump::letters(rows.span.count, name, &rows, first, end);
    for (int b = 0; b < jump::kCells; ++b) {
      const uint32_t start = big.bucketStart(view, b);
      const uint32_t stop = big.bucketStart(view, b + 1);
      if (start == stop) {
        TEST_ASSERT_EQUAL_INT32(-1, first[b]);
        continue;
      }
      TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(start), first[b]);
      TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(stop), end[b]);
    }
    for (uint32_t i = 0; i < rows.span.count; ++i) {
      TEST_ASSERT_EQUAL_INT(big.bucketAt(view, i), textfold::bucketOf(textfold::railKey(name(&rows, i))));
    }
  }
}

// The names the tags don't give (docs/METADATA.md 5.4; the 2026-10-09
// device run, B1). An album with no album value keeps its folder's name, and
// so does one whose values are all blank (White_Space alone: a space, a tab,
// an ideographic space), never "": in the lists, in its sort key and its
// rail letter, in the catalog (Now Playing, the sheet, the queue's lines).
// One real value among blanks names it. An artist with no artist tag is its
// folder's; a track with no title, or a blank one, its file name's. The
// blank albums the console's `ql` listed on N11's card were the artist
// folders' own tracks, named "" in the index by design (3.4.3) and before
// the tags too: every place that names them says "(loose tracks)", and
// "(no artist folder)" for the artist of the files right under /music
// (librarytext, which the console's lines use now).
void test_names_without_tags() {
  using librarytext::AlbumPlace;
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  auto add = [&](const char* path, const View& v) {
    TEST_ASSERT_EQUAL(LibraryIndex::Add::Added, idx.addRecord(path, v.v));
  };
  add("/music/Kelvar Moss/Wind Harbour (2020)/01 - Gull.mp3",
      View().title("Gull").artist("Kelvar Moss").track(1).year(2020));
  add("/music/Kelvar Moss/Wind Harbour (2020)/02 - Tern.mp3", View().title("Tern").artist("Kelvar Moss").track(2));
  add("/music/Kelvar Moss/Salt Road/01 - Ferry.mp3", View().title(" ").artist("Kelvar Moss").album(" "));
  add("/music/Kelvar Moss/Salt Road/02 - Pier.mp3", View().title("\t").artist("Kelvar Moss").album("\xE3\x80\x80"));
  add("/music/Kelvar Moss/Mixed/01 - One.mp3", View().title("One").album(" "));
  add("/music/Kelvar Moss/Mixed/02 - Two.mp3", View().title("Two").album("Mixed Signals"));
  add("/music/Kelvar Moss/Mixed/03 - Three.mp3", View().title("Three").album("  "));
  add("/music/Orvel Tasse/Night Ride/01 - Road.mp3", View().album("Night Ride").track(1));
  add("/music/Orvel Tasse/Night Ride/02 - Turn.mp3", View().title("Turn").album("Night Ride").track(2));
  add("/music/Orvel Tasse/03 - Stray.mp3", View().title("Stray").artist("Orvel Tasse").album("A Single"));
  add("/music/Top Level.mp3", View().title("Top Level"));
  TEST_ASSERT_TRUE(idx.finish());
  TrackCatalog c(&idx);
  const uint32_t gull = idx.findTrack("/music/Kelvar Moss/Wind Harbour (2020)/01 - Gull.mp3");
  const uint32_t ferry = idx.findTrack("/music/Kelvar Moss/Salt Road/01 - Ferry.mp3");
  const uint32_t pier = idx.findTrack("/music/Kelvar Moss/Salt Road/02 - Pier.mp3");
  const uint32_t road = idx.findTrack("/music/Orvel Tasse/Night Ride/01 - Road.mp3");
  const uint32_t stray = idx.findTrack("/music/Orvel Tasse/03 - Stray.mp3");
  const uint32_t top = idx.findTrack("/music/Top Level.mp3");
  // The albums: the folder's name, the year still voted; the one real value.
  const uint32_t wind = idx.track(gull).album, salt = idx.track(ferry).album, night = idx.track(road).album;
  const uint32_t mixed = albumOf(idx, "/music/Kelvar Moss/Mixed/01 - One.mp3");
  TEST_ASSERT_EQUAL_STRING("Wind Harbour (2020)", idx.albumName(wind));
  TEST_ASSERT_EQUAL_UINT16(2020, idx.album(wind).year);
  TEST_ASSERT_EQUAL_STRING("Salt Road", idx.albumName(salt));
  TEST_ASSERT_EQUAL_STRING("Mixed Signals", idx.albumName(mixed));
  TEST_ASSERT_EQUAL_STRING("Night Ride", idx.albumName(night));
  TEST_ASSERT_EQUAL_STRING("Wind Harbour (2020)", librarytext::albumShown(idx, wind));
  TEST_ASSERT_EQUAL_STRING("Wind Harbour (2020)", c.album(gull));
  TEST_ASSERT_EQUAL_STRING("Salt Road", c.album(pier));
  // Their sort keys and rail letters are the folders' names too: W and S,
  // not '#' (a blank name would sort first, under '#').
  TEST_ASSERT_EQUAL_STRING("Wind Harbour (2020)", idx.albumSortKey(wind));
  TEST_ASSERT_EQUAL('W', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, wind)));
  TEST_ASSERT_EQUAL('S', textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, salt)));
  // Every album that isn't an artist folder's own tracks has a name that
  // isn't blank; the rail's letters are the index's buckets row for row.
  const LibraryIndex::Span az = idx.albumsAZ();
  for (uint32_t i = 0; i < az.count; ++i) {
    const uint32_t a = az[i];
    if (!(idx.album(a).flags & LibraryIndex::kLoose)) TEST_ASSERT_FALSE(namekey::blank(idx.albumName(a), strlen(idx.albumName(a))));
    TEST_ASSERT_EQUAL_INT(idx.bucketAt(LibraryIndex::View::Albums, i),
                          textfold::bucketOf(textfold::railKey(librarytext::railName(idx, LibraryIndex::View::Albums, a))));
  }
  // The titles: the file name's for none and for blank ones.
  TEST_ASSERT_EQUAL_STRING("Ferry", titleOf(c, ferry).c_str());
  TEST_ASSERT_EQUAL_STRING("Pier", titleOf(c, pier).c_str());
  TEST_ASSERT_EQUAL_STRING("Road", titleOf(c, road).c_str());
  TEST_ASSERT_EQUAL_UINT16(1, idx.track(road).number);
  // No artist tag: the artist folder's name, everywhere.
  const uint32_t orvel = idx.track(road).artist;
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse", librarytext::artistShown(idx, orvel));
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse", c.artist(road));
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse", c.albumArtist(road));
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse \xC2\xB7 2 tracks", text(librarytext::albumHeader, idx, night).c_str());
  // The loose tracks and the top of /music: "" in the index, named so by
  // librarytext (the rows, the sheet, the console's `ql` and queue lines).
  const uint32_t loose = idx.track(stray).album, topAlbum = idx.track(top).album;
  TEST_ASSERT_TRUE(idx.album(loose).flags & LibraryIndex::kLoose);
  TEST_ASSERT_EQUAL_STRING("", idx.albumName(loose));
  TEST_ASSERT_EQUAL_STRING("", c.album(stray));  // Now Playing: library.loose_tracks
  TEST_ASSERT_EQUAL_STRING(tr(S::library_loose_tracks), librarytext::albumShown(idx, loose));
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse", librarytext::artistShown(idx, idx.album(loose).artist));
  TEST_ASSERT_EQUAL_STRING(tr(S::library_loose_tracks), librarytext::albumShown(idx, topAlbum));
  TEST_ASSERT_EQUAL_STRING(tr(S::library_no_artist_folder), librarytext::artistShown(idx, idx.album(topAlbum).artist));
  TEST_ASSERT_EQUAL_STRING("Orvel Tasse", albumSub(idx, loose, AlbumPlace::AZ).c_str());
  TEST_ASSERT_EQUAL_STRING(tr(S::library_no_artist_folder), albumSub(idx, topAlbum, AlbumPlace::AZ).c_str());
  // The overlay (the playing track's record read after the build): a blank
  // title or album is none there too, the index's names stay.
  TrackCatalog::Overlay o;
  c.setOverlay(&o);
  o.set(idx, road, View().title(" ").album("\t").year(1999).v);
  TEST_ASSERT_EQUAL_STRING("Road", titleOf(c, road).c_str());
  TEST_ASSERT_EQUAL_STRING("Night Ride", c.album(road));
  TEST_ASSERT_EQUAL_UINT16(1999, c.year(road));
  c.setOverlay(nullptr);
  // The helper itself: White_Space alone, or nothing.
  TEST_ASSERT_TRUE(namekey::blank(nullptr, 0));
  TEST_ASSERT_TRUE(namekey::blank("", 0));
  TEST_ASSERT_TRUE(namekey::blank(" \t\xC2\xA0\xE3\x80\x80", 7));
  TEST_ASSERT_FALSE(namekey::blank(" a ", 3));
  TEST_ASSERT_FALSE(namekey::blank("_", 1));
}

// The Library's and Now Playing's new texts in their rooms: the empty
// state's lines (reworded: tags are read), the placeholders and "Unknown
// artist" in Now Playing's rows, a disc divider up to disc 255, the scan's
// status line up to 99,999 tracks and its toasts on one line.
void test_library_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(title, tr(S::empty_no_music_title), kEmptyTitleW);
  for (int k = 0; k < 2; ++k) fits(small, lineOf(S::empty_no_music_lines, k).c_str(), kEmptyLineW);
  for (int k = 0; k < 2; ++k) fits(small, lineOf(kNoCard.lines, k).c_str(), kEmptyLineW);
  for (const char* l :
       {tr(S::now_playing_unknown_artist), tr(S::library_no_artist_folder), tr(S::library_loose_tracks)}) {
    fits(body, l, kNowPlayingTextW);
  }
  // The album row with its year: a short album name keeps it.
  fits(body, "First Light \xC2\xB7 2001", kNowPlayingTextW);
  char t[64];
  librarytext::discText(255, t, sizeof(t));
  fits(bold, t, kDiscTextW);
  using P = librarytext::Status::Phase;
  for (const P phase : {P::Checking, P::Reading, P::Updating, P::Unfinished}) {
    librarytext::Status s;
    s.phase = phase;
    s.done = 99999;
    s.total = 99999;
    TEST_ASSERT_TRUE(librarytext::statusText(s, t, sizeof(t)) > 0);
    fits(small, t, kStatusW);
  }
  librarytext::Status s;
  TEST_ASSERT_EQUAL_UINT32(0, librarytext::statusText(s, t, sizeof(t)));  // Idle: no line
  s.phase = P::Reading;
  s.done = 1234;
  s.total = 19410;
  librarytext::statusText(s, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Reading tags 1,234 / 19,410", t);
  librarytext::foundText(1, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Found 1 new track", t);
  fits(body, t, kToastTextRight - kToastTextX);
  librarytext::foundText(99999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Found 99,999 new tracks", t);
  fits(body, t, kToastTextRight - kToastTextX);
  fits(body, tr(S::library_updated), kToastTextRight - kToastTextX);
  fits(body, tr(S::library_updates_at_boot), kToastTextRight - kToastTextX);
  TEST_ASSERT_TRUE(small.hasAll(tr(S::library_status_checking)) && small.hasAll(tr(S::library_status_updating)));
  // The update step's fence (N12): the lists' line, and the note for what waits.
  fits(body, tr(S::library_updating_list), kListLineW);
  TEST_ASSERT_TRUE(body.hasAll(tr(S::library_updating_list)));
  // A list's other lone lines (centred, nothing beside them).
  for (S id : {S::library_empty_no_music, S::library_empty_no_audio, S::library_empty_nothing, S::queue_empty_text}) {
    fits(body, tr(id), kListLineW);
  }
  TEST_ASSERT_TRUE(320 - kListLineW >= 2 * 10);
  // The action bars (ListView::renderBar()): the Folders root's two buttons
  // beside the A-Z rail (a 284 px list), "Play all 19410" Bold; a row's
  // three, in the Library's 284 px and the Queue's 312 (a scroll bar), each
  // in its font, else Small, in every language.
  {
    const int rootW = (284 - 16 - 6) / 2 - kListBarPad;
    const int rowW = (284 - 16 - 12) / 3 - kListBarPad;
    const int queueW = (312 - 16 - 12) / 3 - kListBarPad;
    TEST_ASSERT_EQUAL_INT(127, rootW);
    char all[48];
    uistr::trnf(all, sizeof(all), S::library_play_all, 19410, 19410);
    TEST_ASSERT_EQUAL_STRING("Play all 19410", all);
    fits(bold, all, rootW);
    for (uistr::Lang l : kAllLangs) {
      if (!uistr::hasCatalogue(l)) continue;
      uistr::setLang(l);
      for (uint32_t n : {1u, 2u, 5u, 1234u, 19410u, 99999u}) {
        uistr::trnf(all, sizeof(all), S::library_play_all, n, n);
        fitsIn(bold, small, all, rootW);
      }
      fitsIn(body, small, tr(S::library_action_queue), rootW);
      fitsIn(bold, small, tr(S::library_action_play), rowW);
      fitsIn(body, small, tr(S::library_action_play_next), rowW);
      fitsIn(body, small, tr(S::library_action_queue), rowW);
      fitsIn(bold, small, tr(S::queue_action_play_now), queueW);
      fitsIn(body, small, tr(S::queue_action_play_next), queueW);
      fitsIn(body, small, tr(S::queue_action_remove), queueW);
      // The Library root's segments (Bold, else Small: Japanese's アーティスト).
      for (S seg : {S::library_segment_artists, S::library_segment_albums, S::library_segment_folders}) {
        fitsIn(bold, small, tr(seg), 100 - kSegmentPad);
      }
    }
    uistr::setLang(uistr::Lang::En);
    for (S seg : {S::library_segment_artists, S::library_segment_albums, S::library_segment_folders}) {
      fits(bold, tr(seg), 100 - kSegmentPad);  // English in Bold
    }
  }
  fits(body, tr(S::library_updating_wait), kToastTextRight - kToastTextX);
  // A card swapped while on: the toasts before the restart.
  fits(body, tr(S::card_another), kToastTextRight - kToastTextX);
  fits(body, tr(S::card_back), kToastTextRight - kToastTextX);
}

// ---- the console's tag commands (tagtext, docs/METADATA.md 3.3.6) ----

namespace {
void collect(void* ctx, const char* line) { static_cast<std::vector<std::string>*>(ctx)->push_back(line); }
bool hasLine(const std::vector<std::string>& lines, const std::string& want) {
  for (const std::string& l : lines) {
    if (l == want) return true;
  }
  return false;
}
}  // namespace

// g's argument: today's g, g0 and g<n>, and the tags' gs, gt, gr, gr!,
// gw, gb, gv; anything else is the help.
void test_console_tag_commands() {
  using C = tagtext::Command;
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse("").command);
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse(nullptr).command);
  TEST_ASSERT_EQUAL(C::Report, tagtext::parse("  ").command);
  TEST_ASSERT_EQUAL(C::Rebuild, tagtext::parse("0").command);
  TEST_ASSERT_EQUAL(C::Rebuild, tagtext::parse("00").command);
  TEST_ASSERT_EQUAL(C::Synthetic, tagtext::parse("12").command);
  TEST_ASSERT_EQUAL_UINT32(12, tagtext::parse("12").n);
  TEST_ASSERT_EQUAL_UINT32(50000, tagtext::parse("50000").n);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("50001").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("99999999999999").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("12a").command);
  TEST_ASSERT_EQUAL(C::Status, tagtext::parse("s").command);
  TEST_ASSERT_EQUAL_UINT32(0, tagtext::parse("s").n);
  TEST_ASSERT_EQUAL(C::Rescan, tagtext::parse("r").command);
  TEST_ASSERT_EQUAL(C::RescanAll, tagtext::parse("r!").command);
  TEST_ASSERT_EQUAL(C::Walk, tagtext::parse("w").command);
  TEST_ASSERT_EQUAL(C::Build, tagtext::parse("b").command);
  TEST_ASSERT_EQUAL_UINT32(0, tagtext::parse("b").n);
  // gs0: the worker's figures from now (L3); gb!: the update step deferred
  // to the next boot (L4.4).
  TEST_ASSERT_EQUAL(C::Status, tagtext::parse("s0").command);
  TEST_ASSERT_EQUAL_UINT32(1, tagtext::parse("s0").n);
  TEST_ASSERT_EQUAL(C::Build, tagtext::parse(" b! ").command);
  TEST_ASSERT_EQUAL_UINT32(1, tagtext::parse("b!").n);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("s1").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("b0").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("w!").command);
  TEST_ASSERT_TRUE(strstr(tagtext::kHelp, "gs0") != nullptr);
  TEST_ASSERT_TRUE(strstr(tagtext::kHelp, "gb!") != nullptr);
  TEST_ASSERT_EQUAL(C::Verify, tagtext::parse("v").command);
  // The sector cache (gc: the counts; gc0-gc2 the switch) and L0's bench.
  TEST_ASSERT_EQUAL(C::Cache, tagtext::parse("c").command);
  TEST_ASSERT_EQUAL_UINT32(tagtext::kCacheReport, tagtext::parse("c").n);
  for (uint32_t k = 0; k <= 2; ++k) {
    const char arg[3] = {'c', static_cast<char>('0' + k), 0};
    TEST_ASSERT_EQUAL(C::Cache, tagtext::parse(arg).command);
    TEST_ASSERT_EQUAL_UINT32(k, tagtext::parse(arg).n);
  }
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("c3").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("c12").command);
  TEST_ASSERT_EQUAL(C::Bench, tagtext::parse("l").command);
  TEST_ASSERT_EQUAL_UINT32(0, tagtext::parse("l").n);
  TEST_ASSERT_EQUAL(C::Bench, tagtext::parse(" lw ").command);
  TEST_ASSERT_EQUAL_UINT32(1, tagtext::parse("lw").n);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("lx").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("x").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("?").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("ss").command);
  TEST_ASSERT_EQUAL(C::Bad, tagtext::parse("t").command);
  const tagtext::Parsed d = tagtext::parse("t  /music/A B/01 - x.mp3");
  TEST_ASSERT_EQUAL(C::Dump, d.command);
  TEST_ASSERT_EQUAL_STRING("/music/A B/01 - x.mp3", d.path);
  TEST_ASSERT_TRUE(strstr(tagtext::kHelp, "gt</music/...>") != nullptr);
}

// A record as the console prints it: each text field the run has (lists
// as "a" | "b"), the numbers on one line, the ReplayGain, the picture's
// anchor, the flags; an UNREADABLE record in one line. And a real file's
// record (the parity corpus's flac_full.flac, read by TagScan): a line per
// field its run holds.
void test_console_tag_dump() {
  namespace mptg = cardcontract::mptg;
  mptg::Record r;
  r.known = mptg::kKnownRules1;
  r.container = mptg::kContainerMp3;
  r.year = 2001;
  r.track = 3;
  r.trackTotal = 12;
  r.disc = 1;
  r.discTotal = 2;
  r.durationMs = 201250;
  r.bpm10 = 1280;
  r.camelot = 8;
  r.flags = 1 | mptg::kHasRgTrack | mptg::kHasRgAlbum | mptg::kTruncated;  // a compilation
  r.rgTrackGain = -650;
  r.rgTrackPeak = 9876;
  r.rgAlbumGain = 125;
  r.picOffset = 1234;
  r.picLength = 23456;
  r.picType = 3;
  r.picMime = mptg::kMimeJpeg;
  r.picCoding = mptg::kCodingRaw;
  cardcontract::RunFields run;
  run.set(cardcontract::kTitle, "Dawn", 4);
  run.set(cardcontract::kArtist, "Ada Crane\x1FMara Quill", 20);
  run.set(cardcontract::kAlbum, "First Light", 11);
  std::vector<std::string> lines;
  tagtext::dumpRecord(r, &run, collect, &lines);
  TEST_ASSERT_TRUE(hasLine(lines, "  title: \"Dawn\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  artist: \"Ada Crane\" | \"Mara Quill\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  album: \"First Light\""));
  TEST_ASSERT_TRUE(
      hasLine(lines, "  MP3, year 2001, track 3/12, disc 1/2, length 3:21.250, BPM 128.0, key 8A, a compilation"));
  TEST_ASSERT_TRUE(hasLine(lines, "  ReplayGain: track -6.50 dB peak 0.9876, album +1.25 dB"));
  TEST_ASSERT_TRUE(hasLine(lines, "  picture: JPEG, type 3 (front cover), 23,456 B at 1,234 (raw)"));
  TEST_ASSERT_TRUE(hasLine(lines, "  flags: TRUNCATED"));
  for (const std::string& l : lines) TEST_ASSERT_TRUE(l.find("looked for") == std::string::npos);
  // What it looked for, when that isn't everything.
  r.known = mptg::kKnownTitle;
  lines.clear();
  tagtext::dumpRecord(r, &run, collect, &lines);
  TEST_ASSERT_TRUE(hasLine(lines, "  looked for: 0x00001 (rules 1 look for all of 0x1ffff)"));
  // UNREADABLE: one line.
  mptg::Record u;
  u.flags = mptg::kUnreadable;
  lines.clear();
  tagtext::dumpRecord(u, nullptr, collect, &lines);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());
  // The small pieces.
  char t[64];
  tagtext::fatTimeText(cardcontract::fatTime(2026, 10, 1, 12, 34, 56), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("2026-10-01 12:34:56", t);
  tagtext::fatTimeText(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("none", t);
  tagtext::lengthText(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("unknown", t);
  TEST_ASSERT_EQUAL_STRING("1B", tagtext::camelotName(13));
  TEST_ASSERT_EQUAL_STRING("12B", tagtext::camelotName(24));
  TEST_ASSERT_EQUAL_STRING("", tagtext::camelotName(25));
  tagtext::flagsText(mptg::kNoTags | mptg::kFromApi, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("NO_TAGS, FROM_API", t);
  // A real file's record.
  const tagfixtures::Bytes bytes = tagfixtures::fileBytes(tagfixtures::dir() + "/flac_full.flac");
  TEST_ASSERT_TRUE(bytes.size() > 0);
  cardcontract::MemSource src(bytes.data(), static_cast<uint32_t>(bytes.size()));
  static tagscan::Scanner scanner;  // (about 10 KB: off the stack)
  static uint8_t buf[4096];
  TEST_ASSERT_EQUAL(tagscan::Result::Ok, scanner.scan(src, tagscan::Kind::Flac, buf, sizeof(buf)));
  static cardcontract::RunFields full;
  scanner.record().toRunFields(&full);
  lines.clear();
  tagtext::dumpRecord(scanner.record().rec, &full, collect, &lines);
  static const char* const kNames[cardcontract::kRunFields] = {
      "title", "artist", "album", "album artist", "genre", "composer", "title sort", "artist sort", "album sort",
      "album artist sort", "MusicBrainz album", "MusicBrainz recording"};
  uint32_t fields = 0;
  for (uint32_t f = 0; f < cardcontract::kRunFields; ++f) {
    if (!full.has(f)) continue;
    ++fields;
    bool found = false;
    for (const std::string& l : lines) found = found || l.rfind(std::string("  ") + kNames[f] + ": \"", 0) == 0;
    TEST_ASSERT_TRUE_MESSAGE(found, kNames[f]);
  }
  TEST_ASSERT_EQUAL_UINT32(cardcontract::kRunFields, fields);  // every field (expected.json's)
  TEST_ASSERT_TRUE(hasLine(lines, "  title: \"Lantern Song\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  artist: \"Mike Duo\" | \"November\""));
  TEST_ASSERT_TRUE(hasLine(lines, "  genre: \"Folk\" | \"Acoustic\""));
  TEST_ASSERT_TRUE(
      hasLine(lines, "  FLAC, year 2003, track 3/12, disc 1/2, length 0:30.000, BPM 96.0, key 8B, a compilation"));
  TEST_ASSERT_TRUE(hasLine(lines, "  ReplayGain: track -6.79 dB peak 0.9886, album +1.01 dB peak 1.0000"));
  TEST_ASSERT_TRUE(hasLine(lines, "  picture: JPEG, type 3 (front cover), 96 B at 748 (raw)"));
}

// Where the index's names came from: g's line.
void test_console_sources() {
  LibraryIndex idx;
  buildTagged(idx);
  const tagtext::Sources s = tagtext::countSources(idx);
  TEST_ASSERT_EQUAL_UINT32(13, s.tracks);
  TEST_ASSERT_EQUAL_UINT32(1, s.transfer);
  TEST_ASSERT_EQUAL_UINT32(10, s.device);
  TEST_ASSERT_EQUAL_UINT32(2, s.path);
  TEST_ASSERT_EQUAL_UINT32(1, s.pending);
  char t[192];
  tagtext::sourcesText(s, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING(
      "13 tracks: 1 from the transfer's records, 10 from the device's, 2 by their paths (1 for the scan)", t);
  LibraryIndex paths;
  TEST_ASSERT_TRUE(paths.begin("/music"));
  paths.addFile("/music/A/B/01 - x.mp3");
  TEST_ASSERT_TRUE(paths.finish());
  tagtext::sourcesText(tagtext::countSources(paths), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("1 track: 1 named by their paths", t);
  LibraryIndex none;
  tagtext::sourcesText(tagtext::countSources(none), t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("0 tracks", t);
}

// The Dance tab's bottom line while a computer drives the dancer (the USB
// visualizer): the title in Bold, the hint in Small, each in kW - 16.
void test_dance_texts_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16), title(kVlwSansBold22);
  fits(bold, tr(S::dance_viz_title), kDanceBottomW);
  fits(small, tr(S::dance_viz_hint), kDanceBottomW);
  // The dancer's name: Title (English), else Bold (every language).
  fits(title, tr(S::dance_crab), kDanceNameW);
  fits(title, tr(S::dance_stick), kDanceNameW);
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    fitsIn(title, bold, tr(S::dance_crab), kDanceNameW);
    fitsIn(title, bold, tr(S::dance_stick), kDanceNameW);
  }
  uistr::setLang(uistr::Lang::En);
  char msg[96];
  snprintf(msg, sizeof(msg), "\"%s\" %d px, \"%s\" %d px, in %d", tr(S::dance_viz_title),
           bold.width(tr(S::dance_viz_title)), tr(S::dance_viz_hint),
           small.width(tr(S::dance_viz_hint)), kDanceBottomW);
  TEST_MESSAGE(msg);
}

// A computer's @identify (ui/Ui::identify()): "This one" in Title, and the
// computer's name for its port in Title when it fits (the usual ones do),
// else Bold, which takes 16 of the widest printable ASCII character
// (hoststatus::kMaxLabel), so a label is never cut.
void test_identify_texts_fit() {
  using namespace uitext;
  const Vlw bold(kVlwSansBold16), title(kVlwSansBold22);
  TEST_ASSERT_TRUE(title.hasAll(kIdentifyTitle));
  fits(title, kIdentifyTitle, kIdentifyW);
  for (const char* port : {"COM5", "COM256", "ttyACM0", "ttyUSB10", "cu.usbmodem14101", "cu.SLAB_USBtoUART"}) {
    fits(title, port, kIdentifyW);
  }
  // The widest of 0x21-0x7E in Bold, 16 of it.
  char widest = '!';
  for (char c = '!'; c <= '~'; ++c) {
    const char a[2] = {c, 0}, b[2] = {widest, 0};
    TEST_ASSERT_TRUE(bold.hasAll(a));
    if (bold.width(a) > bold.width(b)) widest = c;
  }
  const std::string worst(hoststatus::kMaxLabel, widest);
  fits(bold, worst.c_str(), kIdentifyW);
  char msg[96];
  snprintf(msg, sizeof(msg), "16 x '%c' in Bold: %d px in %d", widest, bold.width(worst.c_str()), kIdentifyW);
  TEST_MESSAGE(msg);
}

// The boot screen (BootLayout, ui/BootScreen): the version under the logo
// in Body, or Small when Body doesn't fit (a long dev build's), and the
// rescue line; the line heights the layout keeps room for are the fonts'.
void test_boot_texts_fit() {
  using namespace bootlayout;
  const Vlw body(kVlwSans16), small(kVlwSans13);
  TEST_ASSERT_EQUAL_INT(kBodyH, body.yAdvance);
  TEST_ASSERT_EQUAL_INT(kSmallH, small.yAdvance);
  fits(body, "v10.10.10", kTextW);
  fits(body, "v10.10.10-rc.10", kTextW);
  fits(body, "v0.8.0-dev+abcdef1-dirty", kTextW);
  fits(small, "v10.10.10-rc.10-9999-gabcdef12-dirty", kTextW);
  fits(small, tr(S::boot_touch_hint), kTextW);
}

// The header's pill (Ui::drawHeader(), Header::pillW()): as wide as its
// label (Bold within its budget, else Small), the crumb's with its chevron.
// In every language the label fits its budget in one of the two, and the
// pill leaves the title its room: the crumb starts at x 196 or right of it
// (the title from x 28 keeps 160 px); the Queue's Edit, All and None at
// 230 or right of it. The pill's hit area (Header::hit(),
// uitext::headerPillZoneX()) runs to the edge from x 240, or from the
// pill's round end and the crumb's chevron when it starts further left:
// no more of any pill is left of its zone than of English's crumb (from x
// 215, hit from 240, as it always was). The titles drawn beside the crumb
// (an artist's All tracks, an album's or a folder's while the index isn't
// ready) fit what it leaves them, in Bold (Ui::drawHeader(): from x 28 to
// 8 px short of the pill), with that language's crumb.
void test_header_pills_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  auto pill = [&](const char* label, int maxW, bool chevron) {
    const Vlw& font = bold.width(label) <= maxW ? bold : small;
    fits(font, label, maxW);
    return 320 - 6 - (font.width(label) + kHeaderPillPad + (chevron ? kHeaderChevronW : 0));
  };
  TEST_ASSERT_EQUAL_INT(240, kHeaderPillZoneX);
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    const int crumbX = pill(tr(S::library_crumb), kCrumbTextW, true);
    TEST_ASSERT_TRUE_MESSAGE(crumbX >= 196, tr(S::library_crumb));
    TEST_ASSERT_TRUE(28 + 160 + 8 <= crumbX);
    // Its zone: at most its round end and its chevron left of it (25 px,
    // English's), never right of x 240, and 40 px wide or more.
    const int crumbZone = headerPillZoneX(crumbX, true);
    TEST_ASSERT_TRUE_MESSAGE(crumbZone <= crumbX + kHeaderPillEndW + kHeaderChevronW, tr(S::library_crumb));
    TEST_ASSERT_TRUE(crumbZone == kHeaderPillZoneX || crumbZone == crumbX + 25);
    TEST_ASSERT_TRUE(320 - crumbZone >= 40);
    for (S id : {S::library_all_tracks, S::library_header_album, S::library_header_folder}) {
      TEST_ASSERT_TRUE_MESSAGE(bold.width(tr(id)) <= crumbX - 8 - 28, tr(id));
    }
    int leftmost = 320, zone = 320;
    for (S id : {S::queue_edit, S::queue_all, S::queue_none}) {
      const int x = pill(tr(id), kQueuePillTextW, false);
      TEST_ASSERT_TRUE_MESSAGE(x >= 230, tr(id));
      // At most its round end left of its zone.
      TEST_ASSERT_TRUE_MESSAGE(headerPillZoneX(x, false) <= x + kHeaderPillEndW, tr(id));
      leftmost = std::min(leftmost, x);
      zone = std::min(zone, headerPillZoneX(x, false));
    }
    char m[96];
    snprintf(m, sizeof(m), "%s: crumb from x %d (hit from %d), the Queue's pills from %d (hit from %d); ",
             uistr::tag(l), crumbX, crumbZone, leftmost, zone);
    report += m;
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
  // English: Bold, as before ("‹ Library" from x 215), and every pill hit
  // from x 240 as before.
  TEST_ASSERT_TRUE(bold.width(tr(S::library_crumb)) <= kCrumbTextW);
  const int enCrumbX = pill(tr(S::library_crumb), kCrumbTextW, true);
  TEST_ASSERT_EQUAL_INT(215, enCrumbX);
  TEST_ASSERT_EQUAL_INT(240, headerPillZoneX(enCrumbX, true));
  for (S id : {S::queue_edit, S::queue_all, S::queue_none}) {
    TEST_ASSERT_TRUE(bold.width(tr(id)) <= kQueuePillTextW);
    TEST_ASSERT_EQUAL_INT(240, headerPillZoneX(pill(tr(id), kQueuePillTextW, false), false));
  }
}

// The headers' lines beside their titles (Ui::drawHeader(),
// uitext::headerSub()), in every language, with what that language's title
// and pill leave them: a phrase or a count whole, or not at all (Device
// info's "este Core2" beside Spanish's title was "es…"); the pages' own
// lines (Output, About, Language, the first boot's, the empty Queue's)
// whole in every language; English's every one, as before.
void test_header_subs_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  // The rule itself: a name cut beside the title only with more than 16 px
  // there (as before); a whole one or none; one that may go below.
  TEST_ASSERT_TRUE(headerSub(28, 100, 310, 999, false, false) == HeaderSub::Beside);  // 174 px: cut there
  TEST_ASSERT_TRUE(headerSub(28, 258, 310, 10, false, false) == HeaderSub::None);     // 16 px
  TEST_ASSERT_TRUE(headerSub(28, 100, 310, 174, true, false) == HeaderSub::Beside);
  TEST_ASSERT_TRUE(headerSub(28, 100, 310, 175, true, false) == HeaderSub::None);
  TEST_ASSERT_TRUE(headerSub(28, 100, 310, 175, false, true) == HeaderSub::Below);
  TEST_ASSERT_EQUAL_INT(310, headerRight(0));
  TEST_ASSERT_EQUAL_INT(320 - 6 - 60 - 8, headerRight(60));
  struct Line {
    const char* page;
    int x;
    S title;
    const char* sub;
    int pillW;
    bool always;  // shown in every language
  };
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    char found1[48], found99[48];
    uistr::trnf(found1, sizeof(found1), S::pair_found, 1, 1);
    uistr::trnf(found99, sizeof(found99), S::pair_found, 99, 99);
    std::vector<Line> lines = {
        {"Output", 12, S::output_title, tr(S::output_subtitle), 0, true},
        {"About", 28, S::about_title, tr(S::about_subtitle), 0, true},
        {"Language", 28, S::language_title, tr(S::language_subtitle), 0, true},
        {"first boot", 12, S::language_title, tr(S::language_first_boot_sub), 0, true},
        {"Queue (empty)", 12, S::queue_title, tr(S::queue_empty_sub), 0, true},
        {"Device info", 28, S::about_device_info, tr(S::device_info_subtitle), 0, false},
        {"Pair (1)", 28, S::pair_title, found1, 0, false},
        {"Pair (99)", 28, S::pair_title, found99, 0, false},
    };
    for (const Line& ln : lines) {
      const int right = headerRight(ln.pillW);
      const int titleW = std::min(bold.width(tr(ln.title)), right - ln.x);
      fits(bold, tr(ln.title), right - ln.x);
      const HeaderSub how = headerSub(ln.x, titleW, right, small.width(ln.sub), true, false);
      if (how == HeaderSub::Beside) {
        fits(small, ln.sub, headerSubRoom(ln.x, titleW, right));
      } else {
        TEST_ASSERT_TRUE(how == HeaderSub::None);
        char m[128];
        snprintf(m, sizeof(m), "%s %s: no \"%s\" (%d px in %d); ", uistr::tag(l), ln.page, ln.sub,
                 small.width(ln.sub), headerSubRoom(ln.x, titleW, right));
        report += m;
      }
      if (ln.always || l == uistr::Lang::En) TEST_ASSERT_TRUE_MESSAGE(how == HeaderSub::Beside, ln.sub);
    }
    // Selection mode: "N selected" from x 36 (after the ✕), "of N" beside
    // it, the All pill (None when every entry is selected).
    struct Sel {
      uint32_t n, size;
    };
    for (const Sel s : {Sel{1, 16}, Sel{12, 16}, Sel{12, 999}, Sel{998, 999}, Sel{12, 5000}, Sel{4999, 5000},
                        Sel{16, 16}, Sel{120, 120}, Sel{999, 999}, Sel{5000, 5000}}) {
      const char* pill = tr(s.n == s.size ? S::queue_none : S::queue_all);
      const int pillW = (bold.width(pill) <= kQueuePillTextW ? bold : small).width(pill) + kHeaderPillPad;
      const int right = headerRight(pillW);
      char title[64], of[64];
      uistr::trnf(title, sizeof(title), S::queue_selected, s.n, s.n);
      uistr::trf(of, sizeof(of), S::queue_selected_of, s.size);
      fits(bold, title, right - 36);
      const int titleW = bold.width(title);
      const HeaderSub how = headerSub(36, titleW, right, small.width(of), true, false);
      if (how == HeaderSub::Beside) {
        fits(small, of, headerSubRoom(36, titleW, right));
      } else {
        char m[128];
        snprintf(m, sizeof(m), "%s %s: no \"%s\" (%d px in %d); ", uistr::tag(l), title, of, small.width(of),
                 headerSubRoom(36, titleW, right));
        report += m;
      }
      if (l == uistr::Lang::En) TEST_ASSERT_TRUE_MESSAGE(how == HeaderSub::Beside, of);
    }
    // The jump grid's header: what it jumps in, whole beside "Jump to"
    // (the segment's name, and at the second level "Artists, W: 9999").
    fits(bold, tr(S::jump_title), kJumpTitleW);
    const int jumpRoom = jumpSubRoom(bold.width(tr(S::jump_title)));
    for (S seg : {S::library_segment_artists, S::library_segment_albums, S::library_segment_folders}) {
      fits(small, tr(seg), jumpRoom);
      char second[64];
      uistr::trf(second, sizeof(second), S::jump_second_title, tr(seg), "W", 9999);
      fits(small, second, jumpRoom);
    }
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
}

// Sheet rows (ui/Overlays' Sheet: Now Playing's two menus, the Queue's
// Clear, Output's touch calibration), in every language: each detail beside
// its label when it fits whole in what that language's label leaves it,
// else under the label (sheet::detailOnTwoLines()), whole there too: never
// cut (Spanish's "Fin de la cola" beside "Temporizador de apagado" was
// "Fin de l…"). English's all on one line, as before.
void test_sheet_details_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  // The two lines inside the 40 px row (its divider on y 0), neither over
  // the other: the label (Body or Bold, 19 rows) at 13, the detail (Small,
  // 17) at 31, as a list's two-line row.
  for (const Vlw* f : {&body, &bold}) {
    TEST_ASSERT_TRUE(sheet::kTwoLineLabelY - f->yAdvance / 2 >= 1);
    TEST_ASSERT_TRUE(sheet::kTwoLineLabelY - f->yAdvance / 2 + f->yAdvance <=
                     sheet::kTwoLineDetailY - small.yAdvance / 2);
  }
  TEST_ASSERT_TRUE(sheet::kTwoLineDetailY - small.yAdvance / 2 + small.yAdvance <= sheet::kRowH);
  TEST_ASSERT_EQUAL_INT(288, sheet::kTwoLineW);
  TEST_ASSERT_FALSE(sheet::detailOnTwoLines(100, 172));
  TEST_ASSERT_TRUE(sheet::detailOnTwoLines(100, 173));
  TEST_ASSERT_TRUE(sheet::detailOnTwoLines(248, 10));  // 24 px beside it: under it
  struct Row {
    S label;
    bool primary;
    std::vector<std::string> details;
  };
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    char minutes[32], seconds[32];
    uistr::trf(minutes, sizeof(minutes), S::format_minutes, 999);
    uistr::trf(seconds, sizeof(seconds), S::format_seconds, 59);
    const std::vector<Row> rows = {
        {S::now_playing_shuffle, false, {tr(S::now_playing_on), tr(S::now_playing_off)}},
        {S::now_playing_repeat, false, {tr(repeatMode(0)), tr(repeatMode(1)), tr(repeatMode(2))}},
        {S::now_playing_sleep_timer,
         false,
         {tr(S::sleep_row_off), tr(S::sleep_row_end_track), tr(S::sleep_row_end_album), tr(S::sleep_row_end_queue),
          tr(S::sleep_row_fading), minutes, seconds}},
        {S::now_playing_go_to_artist, false, {tr(S::library_no_artist_folder)}},
        {S::now_playing_go_to_album, false, {tr(S::library_loose_tracks)}},
        {S::queue_clear_up_next, false, {tr(S::queue_clear_up_next_detail)}},
        {S::queue_clear_all, false, {tr(S::queue_clear_all_detail)}},
        {S::settings_cal_calibrate, true, {tr(S::settings_cal_calibrate_detail)}},
    };
    for (const Row& r : rows) {
      const Vlw& lf = r.primary ? bold : body;
      const char* label = tr(r.label);
      fits(lf, label, sheet::kTwoLineW);
      const int labelW = lf.width(label);
      for (const std::string& d : r.details) {
        if (!sheet::detailOnTwoLines(labelW, small.width(d.c_str()))) {
          fits(small, d.c_str(), sheet::detailRoom(labelW));
          continue;
        }
        fits(small, d.c_str(), sheet::kTwoLineW);
        TEST_ASSERT_TRUE_MESSAGE(l != uistr::Lang::En, d.c_str());  // English: one line, as before
        char m[160];
        snprintf(m, sizeof(m), "%s \"%s\" under \"%s\" (%d px in %d); ", uistr::tag(l), d.c_str(), label,
                 small.width(d.c_str()), sheet::detailRoom(labelW));
        report += m;
      }
    }
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
  // A name in the navigation sheet is cut to its row's room first (Now
  // Playing), so it stays beside its label, as the sheet cut it before.
  const textfit::Font f = viewOf(small);
  const int room = sheet::detailRoom(body.width(tr(S::now_playing_go_to_artist)));
  const char* name = "The Lantern Choir and the Very Long Orchestra of Somewhere";
  char cut[96];
  textfit::fit(f, name, strlen(name), cut, sizeof(cut), room);
  TEST_ASSERT_TRUE(strstr(cut, "\xE2\x80\xA6") != nullptr);
  TEST_ASSERT_FALSE(sheet::detailOnTwoLines(body.width(tr(S::now_playing_go_to_artist)), small.width(cut)));
  TEST_ASSERT_EQUAL_STRING(drawn(small, name, room).c_str(), cut);
}

// The Folder page's header counts (librarytext::folderHeaderCounts()), in
// every language, beside its crumb (two levels down) and without it: the
// first form that fits whole, never one cut (German's "14 Audiodateien, 1
// sonst…"). English's two-part lines as before; its three-part line beside
// the crumb, cut before, says "files".
void test_folder_header_counts_fit() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  const textfit::Font f = viewOf(small);
  struct Counts {
    uint32_t folders, files, other;
  };
  const Counts counts[] = {{0, 14, 1}, {3, 12, 0}, {12, 140, 3}, {999, 9999, 9999}, {0, 0, 5},
                           {5, 0, 0},  {0, 0, 0},  {1, 1, 1},   {999, 0, 9999},    {0, 9999, 0}};
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    const char* crumb = tr(S::library_crumb);
    const int pillW =
        (bold.width(crumb) <= kCrumbTextW ? bold : small).width(crumb) + kHeaderPillPad + kHeaderChevronW;
    for (const int right : {headerRight(pillW), headerRight(0)}) {
      const int room = right - 28;
      for (const Counts& c : counts) {
        char t[64], all[64];
        librarytext::folderHeaderCounts(f, room, c.folders, c.files, c.other, t, sizeof(t));
        fits(small, t, room);
        librarytext::folderCounts(c.folders, c.files, c.other, true, all, sizeof(all));
        if (strcmp(t, all) != 0) {
          char m[192];
          snprintf(m, sizeof(m), "%s %d px: \"%s\" for \"%s\"; ", uistr::tag(l), room, t, all);
          report += m;
        }
      }
    }
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
  const int crumbRoom = headerRight(bold.width(tr(S::library_crumb)) + kHeaderPillPad + kHeaderChevronW) - 28;
  TEST_ASSERT_EQUAL_INT(179, crumbRoom);
  char t[64];
  librarytext::folderHeaderCounts(f, crumbRoom, 0, 14, 1, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("14 audio files, 1 other", t);
  librarytext::folderHeaderCounts(f, crumbRoom, 3, 12, 0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("3 folders, 12 audio files", t);
  librarytext::folderHeaderCounts(f, crumbRoom, 12, 140, 3, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("12 folders, 140 files", t);
  librarytext::folderHeaderCounts(f, 282, 12, 140, 3, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("12 folders, 140 audio files, 3 other", t);
  librarytext::folderHeaderCounts(f, crumbRoom, 0, 0, 0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("empty", t);
  librarytext::folderHeaderCounts(f, 40, 5, 0, 7, t, sizeof(t));  // one part: the folders
  TEST_ASSERT_EQUAL_STRING("5 folders", t);
  librarytext::folderHeaderCounts(f, 40, 0, 0, 7, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("7 other", t);
  // A folder row's counts, as before ("files").
  librarytext::folderCounts(3, 12, 1, false, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("3 folders, 12 files, 1 other", t);
  uistr::setLang(uistr::Lang::De);
  const char* crumb = tr(S::library_crumb);
  const int deRoom =
      headerRight((bold.width(crumb) <= kCrumbTextW ? bold : small).width(crumb) + kHeaderPillPad + kHeaderChevronW) -
      28;
  librarytext::folderHeaderCounts(f, deRoom, 0, 14, 1, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("14 Dateien, 1 sonstige", t);
  uistr::setLang(uistr::Lang::En);
}

// The Library's headers with a line that may go under the title
// (Header::subBelow): an artist's All tracks (the artist's name), an
// Artist's counts, an Album's line; in every language, beside the crumb
// where the page has it. Beside the title only whole; else on the second
// line, where the counts are whole too (French's "Tous les morceaux" left
// the name 16 px, Polish's 7: it wasn't drawn).
void test_library_header_lines() {
  using namespace uitext;
  const Vlw small(kVlwSans13), bold(kVlwSansBold16);
  std::string report;
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    const char* crumb = tr(S::library_crumb);
    const int pillW =
        (bold.width(crumb) <= kCrumbTextW ? bold : small).width(crumb) + kHeaderPillPad + kHeaderChevronW;
    // All tracks, two levels down (the crumb): a name beside the title, or
    // under it with 160 px or more.
    {
      const int right = headerRight(pillW);
      const int titleW = std::min(bold.width(tr(S::library_all_tracks)), right - 28);
      for (const char* name : {"Air", "Daft Punk", "The Lantern Choir", "Harder, Better, Faster, Stronger"}) {
        const HeaderSub how = headerSub(28, titleW, right, small.width(name), false, true);
        if (how == HeaderSub::Beside) {
          fits(small, name, headerSubRoom(28, titleW, right));
        } else {
          TEST_ASSERT_TRUE(how == HeaderSub::Below);
          TEST_ASSERT_TRUE(right - 28 >= 160);
        }
      }
      char m[96];
      snprintf(m, sizeof(m), "%s All tracks: a name beside it in %d px, under it %d; ", uistr::tag(l),
               headerSubRoom(28, titleW, right), right - 28);
      report += m;
    }
    // An Artist (one level down: no crumb), "(no artist folder)" or a name:
    // the counts beside it whole, or under it whole, at the most there are.
    for (const char* artist : {tr(S::library_no_artist_folder), "Air", "The Very Long Name of an Artist Band"}) {
      const int right = headerRight(0);
      const int titleW = std::min(bold.width(artist), right - 28);
      for (uint32_t albums : {1u, 3u, 1500u}) {
        for (uint32_t tracks : {1u, 41u, 99999u}) {
          char a[48], tk[48], line[96];
          uistr::trnf(a, sizeof(a), S::library_albums_n, albums, albums);
          uistr::trnf(tk, sizeof(tk), S::library_tracks_n, tracks, uistr::grouped(tracks));
          uistr::trf(line, sizeof(line), S::library_artist_counts, a, tk);
          const HeaderSub how = headerSub(28, titleW, right, small.width(line), false, true);
          fits(small, line, how == HeaderSub::Beside ? headerSubRoom(28, titleW, right) : right - 28);
        }
      }
    }
    // An Album's line ("Artist · 2001 · 14 tracks"), one level down and two
    // (beside the crumb), in the second line's room (beside the title it is
    // drawn only whole): whole; else without the year; else the artist's
    // name cut to what the count leaves; else the year and the count. A
    // phrase whole or left out, never cut; the count always whole.
    const textfit::Font f = viewOf(small);
    struct AlbumArtist {
      const char* name;
      bool phrase;
    };
    for (const int right : {headerRight(0), headerRight(pillW)}) {
      const int room = right - 28;
      for (const AlbumArtist a : {AlbumArtist{"Air", false}, AlbumArtist{"Daft Punk", false},
                                  AlbumArtist{"The Lantern Choir", false},
                                  AlbumArtist{"The Lantern Choir and the Longest Name of All", false},
                                  AlbumArtist{tr(S::library_various_artists), true},
                                  AlbumArtist{tr(S::library_no_artist_folder), true}}) {
        for (uint32_t year : {0u, 2001u}) {
          for (uint32_t tracks : {1u, 14u, 999u}) {
            char line[64], whole[160], count[48], noYear[160];
            librarytext::albumHeaderLine(&f, room, a.name, a.phrase, year, tracks, line, sizeof(line));
            librarytext::albumHeaderLine(nullptr, 0, a.name, a.phrase, year, tracks, whole, sizeof(whole));
            librarytext::albumHeaderLine(nullptr, 0, a.name, a.phrase, 0, tracks, noYear, sizeof(noYear));
            librarytext::trackCount(tracks, count, sizeof(count));
            fits(small, line, room);
            const size_t ln = strlen(line), cn = strlen(count);
            TEST_ASSERT_TRUE_MESSAGE(ln >= cn && strcmp(line + ln - cn, count) == 0, line);  // the count, whole
            if (strcmp(line, whole) == 0) continue;
            TEST_ASSERT_TRUE_MESSAGE(small.width(whole) > room, whole);
            if (strcmp(line, noYear) == 0) continue;  // the year gave way
            TEST_ASSERT_TRUE_MESSAGE(small.width(noYear) > room, noYear);
            if (a.phrase) {  // left out, never cut
              TEST_ASSERT_NULL_MESSAGE(strstr(line, a.name), line);
              TEST_ASSERT_NULL_MESSAGE(strstr(line, "\xE2\x80\xA6"), line);
            }
            if (tracks == 14 && year) {
              char m[192];
              snprintf(m, sizeof(m), "%s %d px: \"%s\" for \"%s\"; ", uistr::tag(l), room, line, a.name);
              report += m;
            }
          }
        }
      }
    }
  }
  TEST_MESSAGE(report.c_str());
  uistr::setLang(uistr::Lang::En);
  {
    // English's Album lines: whole where they fit, as before; where they
    // were cut, the year gives way, then the artist's name: never the count
    // (beside the crumb "Daft Punk · 2001 · 14 tracks", 180 px in 179, was cut).
    const textfit::Font f = viewOf(small);
    const int crumbRoom = headerRight(bold.width(tr(S::library_crumb)) + kHeaderPillPad + kHeaderChevronW) - 28;
    TEST_ASSERT_EQUAL_INT(179, crumbRoom);
    char line[64];
    librarytext::albumHeaderLine(&f, 282, "Daft Punk", false, 2001, 14, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("Daft Punk \xC2\xB7 2001 \xC2\xB7 14 tracks", line);
    librarytext::albumHeaderLine(&f, crumbRoom, "Daft Punk", false, 2001, 14, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("Daft Punk \xC2\xB7 14 tracks", line);
    librarytext::albumHeaderLine(&f, 282, "The Lantern Choir", false, 1999, 11, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("The Lantern Choir \xC2\xB7 1999 \xC2\xB7 11 tracks", line);
    librarytext::albumHeaderLine(&f, crumbRoom, "The Lantern Choir", false, 1999, 11, line, sizeof(line));
    TEST_ASSERT_EQUAL_INT(0, strncmp(line, "The Lantern", 11));
    TEST_ASSERT_NOT_NULL(strstr(line, "\xE2\x80\xA6 \xC2\xB7 11 tracks"));
    TEST_ASSERT_TRUE(small.width(line) <= crumbRoom);
    librarytext::albumHeaderLine(&f, 100, tr(S::library_various_artists), true, 2001, 14, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("2001 \xC2\xB7 14 tracks", line);
    librarytext::albumHeaderLine(&f, 40, "Daft Punk", false, 2001, 14, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("2001 \xC2\xB7 14 tracks", line);  // (no room: the name left out)
    // A name that fits the pixels but not the header's buffer (two bytes a
    // Cyrillic letter): cut in the buffer, with "…", the count whole.
    const char* cyr = "\xD0\x90\xD0\xB1\xD0\xB2\xD0\xB3\xD0\xB4\xD0\xB5\xD0\xB6\xD0\xB7\xD0\xB8\xD0\xBA "
                      "\xD0\xBB\xD0\xBC\xD0\xBD\xD0\xBE\xD0\xBF\xD1\x80\xD1\x81\xD1\x82\xD1\x83\xD1\x84 "
                      "\xD1\x85\xD1\x86\xD1\x87\xD1\x88\xD1\x89\xD1\x8A\xD1\x8B\xD1\x8C";  // 28 letters: 58 B
    TEST_ASSERT_TRUE(small.width(cyr) + small.width(" \xC2\xB7 2001 \xC2\xB7 14 tracks") <= 400);
    librarytext::albumHeaderLine(&f, 400, cyr, false, 2001, 14, line, sizeof(line));
    TEST_ASSERT_TRUE(strlen(line) < sizeof(line));
    TEST_ASSERT_NOT_NULL(strstr(line, "\xE2\x80\xA6 \xC2\xB7 14 tracks"));
    TEST_ASSERT_EQUAL_INT(0, strncmp(line, cyr, 20));
  }
  // English: a short name beside "All tracks" as before; a long one, cut
  // there before, under it.
  const int right = headerRight(bold.width(tr(S::library_crumb)) + kHeaderPillPad + kHeaderChevronW);
  const int titleW = bold.width(tr(S::library_all_tracks));
  TEST_ASSERT_TRUE(headerSub(28, titleW, right, small.width("Daft Punk"), false, true) == HeaderSub::Beside);
  TEST_ASSERT_TRUE(headerSub(28, titleW, right, small.width("The Lantern Choir"), false, true) == HeaderSub::Below);
}

// The hold sheet's title (librarytext::holdTitle()): a long name is cut to
// what the count leaves, never the count, in every language; a short one
// is whole, as the message formats it.
void test_hold_title_keeps_its_count() {
  const Vlw small(kVlwSans13);
  const textfit::Font f = viewOf(small);
  const std::string longName = "The Very Long Name Of A Compilation Album Of Songs Remastered";  // 61 characters
  for (uistr::Lang l : kAllLangs) {
    if (!uistr::hasCatalogue(l)) continue;
    uistr::setLang(l);
    for (uint32_t n : {1u, 14u, 1234u}) {
      char t[128], rest[96], plain[128], digits[16];
      librarytext::holdTitle(f, sheet::kTitleW, longName.c_str(), n, t, sizeof(t));
      fits(small, t, sheet::kTitleW);
      // The rest (the name empty) whole in it, and the count's digits.
      uistr::trnf(rest, sizeof(rest), S::library_hold_title, n, "", n);
      snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(n));
      TEST_ASSERT_NOT_NULL_MESSAGE(strstr(t, digits), t);
      TEST_ASSERT_TRUE_MESSAGE(strstr(t, "\xE2\x80\xA6") != nullptr, t);  // the name cut
      TEST_ASSERT_TRUE_MESSAGE(small.width(rest) + 30 <= sheet::kTitleW, rest);  // a name keeps room
      // A short name: whole, the message as it is.
      librarytext::holdTitle(f, sheet::kTitleW, "Discovery", n, t, sizeof(t));
      uistr::trnf(plain, sizeof(plain), S::library_hold_title, n, "Discovery", n);
      TEST_ASSERT_EQUAL_STRING(plain, t);
    }
  }
  uistr::setLang(uistr::Lang::En);
  char t[128];
  librarytext::holdTitle(f, sheet::kTitleW, "Harder, Better, Faster, Stronger, and Longer Still", 14, t, sizeof(t));
  const size_t len = strlen(t);
  TEST_ASSERT_TRUE(len > 11 && strcmp(t + len - 11, ", 14 tracks") == 0);
  fits(small, t, sheet::kTitleW);
  librarytext::holdTitle(f, sheet::kTitleW, "Discovery", 14, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Discovery, 14 tracks", t);
  TEST_ASSERT_EQUAL_INT(226, sheet::kTitleW);
}

// Device info (Output > About > Device info): About's row to it, the
// page's header, and every row with the widest facts the firmware can
// give (app/Diagnostics' names, Arduino's chip models): the label in
// Small, the value in Body when it fits and Small otherwise, so Small
// must always fit. The usual values read in Body.
void test_device_info_texts_fit() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  fits(body, tr(S::about_device_info), kDeviceInfoRowW);
  fits(small, tr(S::about_device_info_sub), kDeviceInfoRowW);
  // The header: the title (Bold) after the back chevron (x 28), the line
  // (Small) 8 px after it, to x 310; drawn only when it starts 16 px
  // before that (Ui::drawHeader()).
  const int subX = 28 + bold.width(tr(S::about_device_info)) + 8;
  TEST_ASSERT_TRUE(subX < 310 - 16);
  fits(small, tr(S::device_info_subtitle), 310 - subX);
  TEST_ASSERT_EQUAL_INT(312 - 8, kDeviceInfoX + kDeviceInfoW);

  char v[96];
  deviceinfo::Facts f;
  f.chipRevision = 399;
  f.cpuMhz = 240;
  f.flashBytes = 16u * 1024 * 1024;
  f.psramBytes = 8192u * 1024;
  f.psramFree = 8191u * 1024;
  f.battery = 100;
  f.batteryMv = 4999;
  f.charging = true;
  f.tracks = 99999;
  f.ramFree = f.ramMin = f.ramBlock = 999u * 1024;
  f.commit = "abcdef1";
  f.built = "2026-12-31";
  f.board = "M5Stack Core2";
  f.pmic = f.imu = "none found";
  f.chip = "ESP32-PICO-V3-02";
  f.lastReset = "interrupt watchdog";
  f.storage = "flash";
  f.version = "v10.10.10-rc.10-9999-gabcdef12-dirty";
  f.elf = "dddddddd";
  // Every row with these, then each fact's other values in turn (a value
  // depends on its own facts only).
  auto all = [&]() {
    for (int i = 0; i < deviceinfo::kItems; ++i) {
      const auto item = static_cast<deviceinfo::Item>(i);
      fits(small, deviceinfo::label(item), kDeviceInfoW);
      fits(small, deviceinfo::value(item, f, v, sizeof(v)), kDeviceInfoW);
    }
  };
  all();
  for (const char* board : {"M5Stack Core2", "unknown"}) {
    f.board = board;
    all();
  }
  for (const char* name : {"AXP192", "AXP2101", "none found", "other", "BMI270", "MPU6886"}) {
    f.pmic = f.imu = name;
    all();
  }
  for (const char* chip : {"ESP32-D0WDQ6-V3", "ESP32-D0WDR2-V3", "ESP32-PICO-V3-02", "ESP32-D2WDQ5", "Unknown"}) {
    f.chip = chip;
    all();
  }
  for (const char* reset : {"interrupt watchdog", "task watchdog", "BROWNOUT", "deep sleep", "PANIC"}) {
    f.lastReset = reset;
    all();
  }
  for (const char* storage : {"SD", "flash", "none"}) {
    f.storage = storage;
    all();
  }
  // The library's names: each form the row takes (none counted, all from
  // the files, some tagged, all tagged, behind the update's fence), with
  // the widest storage; the widest last, for the loops after.
  struct Src {
    uint32_t transfer, device, none;
    bool updating;
  };
  f.storage = "flash";
  for (const Src src : {Src{0, 0, 0, false}, Src{99998, 0, 1, false}, Src{0, 99999, 0, false},
                        Src{50000, 49999, 0, true}, Src{0, 0, 99999, false}}) {
    f.fromTransfer = src.transfer;
    f.fromDevice = src.device;
    f.fromNone = src.none;
    f.updating = src.updating;
    all();
  }
  for (const uint32_t up : {3599u, 86399u, 49u * 86400 + 23 * 3600, 0xFFFFFFFFu}) {
    f.uptimeS = up;
    all();
  }
  std::string elf;
  for (const char* hex = "0123456789abcdef"; *hex; ++hex) {
    elf.assign(8, *hex);
    f.elf = elf.c_str();
    all();
  }
  f.elf = "dddddddd";
  for (const char* ver : {"v10.10.10-rc.10-9999-gabcdef12-dirty", "v0.8.0-dev+abcdef1-dirty", "v10.10.10"}) {
    f.version = ver;
    all();
  }
  // What a Core2 shows: in Body.
  f.board = "M5Stack Core2";
  f.chip = "ESP32-D0WDQ6-V3";
  f.chipRevision = 301;
  f.psramBytes = 4096u * 1024;
  f.psramFree = 3210u * 1024;
  f.ramFree = 180u * 1024;
  f.ramMin = 120u * 1024;
  f.ramBlock = 110u * 1024;
  f.storage = "SD";
  f.tracks = 19410;  // most from the transfer, some read here
  f.fromTransfer = 18000;
  f.fromDevice = 1400;
  f.fromNone = 10;
  f.uptimeS = 23 * 3600 + 59 * 60;
  f.version = "v0.8.0";
  for (const auto item : {deviceinfo::Item::Chip, deviceinfo::Item::Psram, deviceinfo::Item::Battery,
                          deviceinfo::Item::Library, deviceinfo::Item::RamFree, deviceinfo::Item::Uptime,
                          deviceinfo::Item::Firmware}) {
    fits(body, deviceinfo::value(item, f, v, sizeof(v)), kDeviceInfoW);
  }
}

// The Output tab's Library row (3.3.6, N12): its title, where its names
// come from (the longest form that fits; the shortest always does), the
// Rescan pill and its dialog.
void test_library_row_texts() {
  using namespace uitext;
  const Vlw body(kVlwSans16), small(kVlwSans13), bold(kVlwSansBold16);
  char t[96];
  librarytext::libraryRowTitle(19410, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Library: 19,410 tracks", t);
  fits(body, t, kLibraryRowW);
  librarytext::libraryRowTitle(99999, t, sizeof(t), 1);  // the short form, when the long one doesn't fit
  TEST_ASSERT_EQUAL_STRING("99,999 tracks", t);
  fits(body, t, kLibraryRowW);
  librarytext::libraryRowTitle(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("Library: no tracks", t);
  librarytext::Sources s;
  s.transfer = 18000;
  s.device = 1400;
  s.none = 10;
  librarytext::sourcesText(s, 0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("18,000 from the transfer, 1,400 read here, 10 without tags", t);
  librarytext::sourcesText(s, 1, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("18,000 transfer, 1,400 here, 10 none", t);
  librarytext::sourcesText(s, 2, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("99% tagged", t);
  // The parts with a count only; all from one source.
  s = librarytext::Sources{};
  s.device = 77;
  librarytext::sourcesText(s, 0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("77 read here", t);
  fits(small, t, kLibraryRowW);
  librarytext::sourcesText(s, 2, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("100% tagged", t);
  s.none = 1;
  librarytext::sourcesText(s, 2, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("98% tagged", t);  // rounded down: 100% only when all are
  s = librarytext::Sources{};
  s.none = 5;
  librarytext::sourcesText(s, 0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("names from the files", t);
  // The shortest form fits whatever the counts; the short one for a card
  // of the user's shape (most from the transfer, some read here).
  s.transfer = 99999;
  s.device = 99999;
  s.none = 99999;
  librarytext::sourcesText(s, librarytext::kSourceForms - 1, t, sizeof(t));
  fits(small, t, kLibraryRowW);
  s.transfer = 18000;
  s.device = 1400;
  s.none = 0;
  librarytext::sourcesText(s, 1, t, sizeof(t));
  fits(small, t, kLibraryRowW);
  for (S id : {S::library_row_paths, S::library_row_no_card, S::library_row_updating}) {
    fits(small, tr(id), kLibraryRowW);
  }
  TEST_ASSERT_TRUE(small.hasAll(tr(S::library_row_updating)));
  fits(body, tr(S::library_row_rescan), kRescanPillW - kSettingPillPad);
  fits(body, tr(S::library_row_rescan_started), kToastTextRight - kToastTextX);
  fits(body, tr(S::library_row_rescan_no_card), kToastTextRight - kToastTextX);
  // The dialog: its title in Bold, its body in Small on its 3 lines (as
  // the calibration's Remove), its button.
  fits(bold, tr(S::library_row_rescan_title), kDialogTitleW);
  fits(bold, tr(S::library_row_rescan), kDialogButtonTextW);
  {
    textfit::Font f;
    f.ctx = const_cast<Vlw*>(&small);
    f.width = [](void* ctx, const char* text) { return static_cast<const Vlw*>(ctx)->width(text); };
    TEST_ASSERT_TRUE(strlen(tr(S::library_row_rescan_body)) < 128);
    char lines[4][96];
    const char* rescanBody = tr(S::library_row_rescan_body);
    const int n = textfit::wrap(f, rescanBody, strlen(rescanBody), kDialogTitleW, 3, &lines[0][0], sizeof(lines[0]));
    TEST_ASSERT_TRUE(n <= 3);
    std::string joined;
    for (int i = 0; i < n; ++i) joined += std::string(i ? " " : "") + lines[i];
    TEST_ASSERT_EQUAL_STRING(tr(S::library_row_rescan_body), joined.c_str());
  }
  // A buffer too small: cut, never past it.
  char tiny[12];
  s.transfer = 18000;
  s.device = 1400;
  s.none = 10;
  TEST_ASSERT_TRUE(librarytext::sourcesText(s, 0, tiny, sizeof(tiny)) < sizeof(tiny));
  TEST_ASSERT_TRUE(strlen(tiny) < sizeof(tiny));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_jump_letters_match_the_index_buckets);
  RUN_TEST(test_jump_second_level);
  RUN_TEST(test_jump_second_key);
  RUN_TEST(test_jump_second_level_on_a_big_library);
  RUN_TEST(test_jump_grid_cells);
  RUN_TEST(test_list_steps_toward_the_finger);
  RUN_TEST(test_list_steps_in_whole_rows_never_exceed_the_cap);
  RUN_TEST(test_tabbar_texts_fit_in_every_state);
  RUN_TEST(test_coach_texts_fit);
  RUN_TEST(test_toast_names_fit);
  RUN_TEST(test_names_drawn_with_the_fonts);
  RUN_TEST(test_names_measured_as_drawn);
  RUN_TEST(test_store_glyphs_bold_in_bold_and_title);
  RUN_TEST(test_now_playing_title_rows);
  RUN_TEST(test_fonts_keep_their_line_heights);
  RUN_TEST(test_store_glyphs_sit_in_the_rows);
  RUN_TEST(test_now_playing_title_lines_share_rows);
  RUN_TEST(test_fonts_cover_greek_cyrillic_and_latin);
  RUN_TEST(test_queue_cap_texts_fit);
  RUN_TEST(test_push_out_texts_fit);
  RUN_TEST(test_empty_state_texts_fit);
  RUN_TEST(test_no_card_texts_fit);
  RUN_TEST(test_board_guard_texts_fit);
  RUN_TEST(test_queue_texts_fit);
  RUN_TEST(test_queue_summary_fits_every_language);
  RUN_TEST(test_output_texts_fit);
  RUN_TEST(test_waiting_texts_fit);
  RUN_TEST(test_now_playing_menu_texts_fit);
  RUN_TEST(test_seek_bar_texts_fit);
  RUN_TEST(test_sleep_timer_texts_fit);
  RUN_TEST(test_idle_power_off_texts_fit);
  RUN_TEST(test_power_settings_texts_fit);
  RUN_TEST(test_touch_calibration_texts_fit);
  RUN_TEST(test_dance_texts_fit);
  RUN_TEST(test_boot_texts_fit);
  RUN_TEST(test_device_info_texts_fit);
  RUN_TEST(test_header_pills_fit);
  RUN_TEST(test_header_subs_fit);
  RUN_TEST(test_sheet_details_fit);
  RUN_TEST(test_folder_header_counts_fit);
  RUN_TEST(test_library_header_lines);
  RUN_TEST(test_hold_title_keeps_its_count);
  RUN_TEST(test_right_half_neighbours_survive_the_lab_panel);
  RUN_TEST(test_library_rows_from_tags);
  RUN_TEST(test_library_rows_from_paths);
  RUN_TEST(test_disc_dividers);
  RUN_TEST(test_catalog_names_and_overlay);
  RUN_TEST(test_rail_follows_the_sort_keys);
  RUN_TEST(test_names_without_tags);
  RUN_TEST(test_identify_texts_fit);
  RUN_TEST(test_library_texts_fit);
  RUN_TEST(test_library_row_texts);
  RUN_TEST(test_console_tag_commands);
  RUN_TEST(test_console_tag_dump);
  RUN_TEST(test_console_sources);
  return UNITY_END();
}
