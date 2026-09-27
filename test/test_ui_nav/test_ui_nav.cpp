// Host tests for the UI framework's portable pieces: the tab bar design's
// navigation (NavModel), the frame cap on deadlines (FrameClock), a list's
// items with its top bar and inline action bar (ListLayout), the selection
// (BitSet), the text layer's fitting (TextFit), the tab bar's layout and
// redraws (TabBarModel) and the track length estimate (TrackProgress).
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "BitSet.h"
#include "FrameClock.h"
#include "ListLayout.h"
#include "NavModel.h"
#include "TabBarModel.h"
#include "TextFit.h"
#include "TrackProgress.h"

void setUp() {}
void tearDown() {}

namespace {

using Tab = NavModel::Tab;
using PageRef = NavModel::PageRef;

PageRef page(uint8_t kind, uint32_t id = NavModel::kNone) {
  PageRef p;
  p.kind = kind;
  p.id = id;
  return p;
}

NavModel rootedModel() {
  NavModel m;
  for (int t = 0; t < NavModel::kTabs; ++t) m.setRoot(static_cast<Tab>(t), page(static_cast<uint8_t>(10 + t)));
  return m;
}

}  // namespace

// ---- NavModel ----

void test_nav_tabs_keep_their_stacks() {
  NavModel m = rootedModel();
  TEST_ASSERT_EQUAL_INT((int)Tab::NowPlaying, (int)m.tab());
  TEST_ASSERT_EQUAL_INT((int)NavModel::TabTap::Switched, (int)m.tapTab(Tab::Library));
  m.push(page(20, 7));   // an artist
  m.push(page(21, 42));  // an album
  m.top().scrollPx = 126;
  TEST_ASSERT_EQUAL_INT(3, m.depth());
  // Another tab and back: the Library is where it was, scroll and all.
  TEST_ASSERT_EQUAL_INT((int)NavModel::TabTap::Switched, (int)m.tapTab(Tab::Queue));
  TEST_ASSERT_EQUAL_UINT8(12, m.top().kind);
  TEST_ASSERT_EQUAL_INT((int)NavModel::TabTap::Switched, (int)m.tapTab(Tab::Library));
  TEST_ASSERT_EQUAL_INT(3, m.depth());
  TEST_ASSERT_EQUAL_UINT8(21, m.top().kind);
  TEST_ASSERT_EQUAL_UINT32(42, m.top().id);
  TEST_ASSERT_EQUAL_INT32(126, m.top().scrollPx);
  // The other tabs were never touched.
  TEST_ASSERT_EQUAL_INT(1, m.depth(Tab::Queue));
  TEST_ASSERT_EQUAL_UINT8(10, m.top(Tab::NowPlaying).kind);
}

void test_nav_tapping_the_active_tab_pops_to_its_root() {
  NavModel m = rootedModel();
  m.tapTab(Tab::Library);
  m.top().scrollPx = 84;  // the root's own place is kept
  m.push(page(20, 1));
  m.push(page(21, 2));
  TEST_ASSERT_EQUAL_INT((int)NavModel::TabTap::PoppedToRoot, (int)m.tapTab(Tab::Library));
  TEST_ASSERT_EQUAL_INT(1, m.depth());
  TEST_ASSERT_EQUAL_UINT8(11, m.top().kind);
  TEST_ASSERT_EQUAL_INT32(84, m.top().scrollPx);
  TEST_ASSERT_EQUAL_INT((int)NavModel::TabTap::AtRoot, (int)m.tapTab(Tab::Library));
  TEST_ASSERT_EQUAL_INT(1, m.depth());
}

void test_nav_back_pops_one_level_and_stops_at_the_root() {
  NavModel m = rootedModel();
  m.select(Tab::Library);
  m.push(page(20, 1));
  m.top().expanded = 3;
  m.push(page(21, 2));
  TEST_ASSERT_TRUE(m.back());
  TEST_ASSERT_EQUAL_UINT8(20, m.top().kind);
  TEST_ASSERT_EQUAL_INT32(3, m.top().expanded);  // the page's own state survives
  TEST_ASSERT_TRUE(m.back());
  TEST_ASSERT_FALSE(m.back());
  TEST_ASSERT_EQUAL_INT(1, m.depth());
  TEST_ASSERT_EQUAL_UINT8(11, m.top().kind);
}

void test_nav_deep_pushes_drop_the_oldest_above_the_root() {
  NavModel m = rootedModel();
  m.select(Tab::Library);
  for (uint32_t i = 1; i <= NavModel::kMaxDepth + 3; ++i) m.push(page(30, i));
  TEST_ASSERT_EQUAL_INT(NavModel::kMaxDepth, m.depth());
  TEST_ASSERT_EQUAL_UINT8(11, m.at(Tab::Library, 0).kind);  // the root stays
  TEST_ASSERT_EQUAL_UINT32(NavModel::kMaxDepth + 3, m.top().id);
  // The pages under it are the newest ones, in order.
  for (int level = 1; level < NavModel::kMaxDepth; ++level) {
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(level + 4), m.at(Tab::Library, level).id);
  }
  // Back all the way: every level, then the root.
  int backs = 0;
  while (m.back()) ++backs;
  TEST_ASSERT_EQUAL_INT(NavModel::kMaxDepth - 1, backs);
}

void test_nav_replace_above_root() {
  NavModel m = rootedModel();
  m.select(Tab::NowPlaying);
  const PageRef jump[2] = {page(20, 5), page(21, 9)};
  m.replaceAboveRoot(Tab::Library, jump, 2);
  TEST_ASSERT_EQUAL_INT((int)Tab::NowPlaying, (int)m.tab());  // it doesn't switch by itself
  TEST_ASSERT_EQUAL_INT(3, m.depth(Tab::Library));
  TEST_ASSERT_EQUAL_UINT32(9, m.top(Tab::Library).id);
  m.select(Tab::Library);
  TEST_ASSERT_TRUE(m.back());
  TEST_ASSERT_EQUAL_UINT32(5, m.top().id);
  m.replaceAboveRoot(Tab::Library, jump, 0);
  TEST_ASSERT_EQUAL_INT(1, m.depth(Tab::Library));
  TEST_ASSERT_EQUAL_STRING("Output", NavModel::name(Tab::Output));
}

// ---- FrameClock ----

void test_frame_clock_keeps_deadlines() {
  FrameClock c(33);
  TEST_ASSERT_TRUE(c.due(1000));  // idle: at once
  c.drawn(1000);                  // the cadence starts here: 1033, 1066, ...
  TEST_ASSERT_FALSE(c.due(1032));
  TEST_ASSERT_EQUAL_UINT32(1, c.msUntilDue(1032));
  // A pass 4 ms late draws the frame; the next is still due at 1066, not 1070.
  TEST_ASSERT_TRUE(c.due(1037));
  c.drawn(1037);
  TEST_ASSERT_FALSE(c.due(1065));
  TEST_ASSERT_TRUE(c.due(1066));
  c.drawn(1066);
  // Over 30 frames with every pass 1-5 ms late, the frames stay 33 ms apart
  // on average: no drift.
  uint32_t now = 1066, frames = 0;
  for (int i = 0; i < 3000; ++i) {
    now += 1 + (i * 7) % 5;
    if (c.due(now)) {
      c.drawn(now);
      ++frames;
    }
    if (now >= 1066 + 990) break;
  }
  TEST_ASSERT_EQUAL_UINT32(30, frames);
}

void test_frame_clock_restarts_after_a_stall() {
  FrameClock c(33);
  c.drawn(0);
  // A 200 ms stall: one frame, then the cadence from there, no catch-up burst.
  TEST_ASSERT_TRUE(c.due(233));
  c.drawn(233);
  TEST_ASSERT_FALSE(c.due(240));
  TEST_ASSERT_FALSE(c.due(265));
  TEST_ASSERT_TRUE(c.due(266));
  // No cap: always due.
  FrameClock none(0);
  none.drawn(5);
  TEST_ASSERT_TRUE(none.due(5));
  TEST_ASSERT_EQUAL_UINT32(0, none.msUntilDue(5));
}

// ---- ListLayout ----

void test_list_items_with_top_bar_and_inline_bar() {
  ListLayout l;
  l.set(10, true);
  TEST_ASSERT_EQUAL_UINT32(11, l.itemCount());
  TEST_ASSERT_EQUAL_INT((int)ListLayout::Kind::TopBar, (int)l.item(0).kind);
  TEST_ASSERT_EQUAL_INT32(0, l.item(1).row);
  TEST_ASSERT_EQUAL_UINT32(4, l.itemOfRow(3));
  l.setExpanded(3, 0, 168);
  TEST_ASSERT_EQUAL_UINT32(12, l.itemCount());
  TEST_ASSERT_EQUAL_INT((int)ListLayout::Kind::Row, (int)l.item(4).kind);
  TEST_ASSERT_EQUAL_INT32(3, l.item(4).row);
  TEST_ASSERT_EQUAL_INT((int)ListLayout::Kind::InlineBar, (int)l.item(5).kind);
  TEST_ASSERT_EQUAL_INT32(3, l.item(5).row);
  TEST_ASSERT_EQUAL_INT32(4, l.item(6).row);
  TEST_ASSERT_EQUAL_UINT32(6, l.itemOfRow(4));
  TEST_ASSERT_EQUAL_INT((int)ListLayout::Kind::None, (int)l.item(12).kind);
  // Hit test by content line.
  TEST_ASSERT_EQUAL_INT32(5, l.itemAt(5 * 42 + 41));
  TEST_ASSERT_EQUAL_INT32(-1, l.itemAt(12 * 42));
  TEST_ASSERT_EQUAL_INT32(-1, l.itemAt(-1));
  // Fewer rows: an expanded row past the end goes.
  l.set(3, true);
  TEST_ASSERT_EQUAL_INT32(-1, l.expanded());
  TEST_ASSERT_EQUAL_UINT32(4, l.itemCount());
}

void test_list_expand_keeps_the_row_and_shows_its_bar() {
  ListLayout l;
  l.set(100, false);
  // Row 12 at the top (offset 504): expanding it keeps it there, its bar is
  // on screen already.
  TEST_ASSERT_EQUAL_INT32(504, l.setExpanded(12, 504, 168));
  // Row 15 is the 4th row on screen (with row 12's bar above it: item 16 at
  // 672..714, the viewport 504..672 ends before it): expanding it closes
  // row 12's bar (everything below moves up 42), keeps row 15 in place,
  // then scrolls just enough for its own bar.
  const int32_t o = l.setExpanded(15, 504 + 42, 168);
  TEST_ASSERT_EQUAL_INT32(15, l.expanded());
  const int32_t rowTop = static_cast<int32_t>(l.itemOfRow(15)) * 42;
  const int32_t barBottom = rowTop + 2 * 42;
  TEST_ASSERT_TRUE(barBottom <= o + 168);
  TEST_ASSERT_TRUE(rowTop >= o);
  TEST_ASSERT_EQUAL_INT32(barBottom - 168, o);
  // Collapsing with the bar's row at the top: nothing on screen moves.
  const int32_t o2 = l.setExpanded(-1, rowTop, 168);
  TEST_ASSERT_EQUAL_INT32(rowTop, o2);
  // Collapsing with the bar above the viewport: the rows on screen stay put.
  l.setExpanded(2, 0, 168);
  const int32_t row20 = static_cast<int32_t>(l.itemOfRow(20)) * 42;
  const int32_t o3 = l.setExpanded(-1, row20, 168);
  TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(l.itemOfRow(20)) * 42, o3);
  TEST_ASSERT_EQUAL_INT32(row20 - 42, o3);
}

void test_list_expand_at_the_end_stays_in_the_list() {
  ListLayout l;
  l.set(6, false);  // 252 px: max offset 84
  const int32_t o = l.setExpanded(5, 84, 168);
  TEST_ASSERT_EQUAL_INT32(l.maxOffset(168), o);
  TEST_ASSERT_EQUAL_INT32(126, o);
  // A short list: no scrolling at all.
  ListLayout s;
  s.set(2, true);
  TEST_ASSERT_EQUAL_INT32(0, s.setExpanded(1, 0, 168));
  TEST_ASSERT_EQUAL_INT32(0, s.maxOffset(168));
  TEST_ASSERT_EQUAL_INT32(0, s.topOf(3, 168));
}

void test_list_reveal_and_top_of() {
  ListLayout l;
  l.set(50, false);
  TEST_ASSERT_EQUAL_INT32(42 * 10, l.reveal(10, 0, 168) + 168 - 42);  // just brought in at the bottom
  TEST_ASSERT_EQUAL_INT32(42 * 3, l.reveal(3, 42 * 20, 168));        // brought in at the top
  TEST_ASSERT_EQUAL_INT32(42 * 20, l.reveal(21, 42 * 20, 168));      // already visible: no move
  TEST_ASSERT_EQUAL_INT32(42 * 7, l.topOf(7, 168));
  TEST_ASSERT_EQUAL_INT32(l.maxOffset(168), l.topOf(49, 168));
}

void test_list_rail_geometry() {
  // Track 160 px, thumb 30: 130 px of travel over the whole list.
  TEST_ASSERT_EQUAL_INT(0, ListLayout::thumbTop(0, 4200, 160, 30));
  TEST_ASSERT_EQUAL_INT(130, ListLayout::thumbTop(4200, 4200, 160, 30));
  TEST_ASSERT_EQUAL_INT(65, ListLayout::thumbTop(2100, 4200, 160, 30));
  TEST_ASSERT_EQUAL_INT(0, ListLayout::thumbTop(100, 0, 160, 30));
  // A scrub: the thumb's centre follows the finger, whole rows, both ends reachable.
  TEST_ASSERT_EQUAL_INT32(0, ListLayout::scrubOffset(0, 4200, 160, 30));
  TEST_ASSERT_EQUAL_INT32(4200, ListLayout::scrubOffset(159, 4200, 160, 30));
  const int32_t mid = ListLayout::scrubOffset(80, 4200, 160, 30);
  TEST_ASSERT_EQUAL_INT32(0, mid % 42);
  TEST_ASSERT_INT32_WITHIN(42, 2100, mid);
  // Monotonic along the track.
  int32_t last = -1;
  for (int y = 0; y < 160; ++y) {
    const int32_t o = ListLayout::scrubOffset(y, 4200, 160, 30);
    TEST_ASSERT_TRUE(o >= last);
    last = o;
  }
}

// ---- BitSet ----

void test_bitset_select_count_list() {
  BitSet s;
  TEST_ASSERT_TRUE(s.resize(100));
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  s.set(3, true);
  s.set(64, true);
  s.set(99, true);
  s.set(99, true);  // again: no change
  s.set(100, true); // out of range: ignored
  TEST_ASSERT_EQUAL_UINT32(3, s.count());
  s.toggle(3);
  TEST_ASSERT_FALSE(s.get(3));
  TEST_ASSERT_EQUAL_UINT32(2, s.count());
  uint32_t out[8];
  TEST_ASSERT_EQUAL_UINT32(2, s.list(out, 8));
  TEST_ASSERT_EQUAL_UINT32(64, out[0]);
  TEST_ASSERT_EQUAL_UINT32(99, out[1]);
  s.setAll(true);
  TEST_ASSERT_EQUAL_UINT32(100, s.count());
  TEST_ASSERT_EQUAL_UINT32(8, s.list(out, 8));
  TEST_ASSERT_EQUAL_UINT32(7, out[7]);
  s.setAll(false);
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
  TEST_ASSERT_TRUE(s.resize(10));  // a new size: clear
  TEST_ASSERT_EQUAL_UINT32(10, s.size());
  TEST_ASSERT_FALSE(s.get(9));
}

namespace {
void* noMemory(size_t) { return nullptr; }
void noFree(void*) {}
}  // namespace

void test_bitset_without_memory() {
  BitSet s(noMemory, noFree);
  TEST_ASSERT_FALSE(s.resize(10));
  TEST_ASSERT_EQUAL_UINT32(0, s.size());
  s.set(1, true);
  TEST_ASSERT_FALSE(s.get(1));
  TEST_ASSERT_EQUAL_UINT32(0, s.count());
}

// ---- TextFit ----

namespace {

// A fake font: 7 px a character (a code point), "…" 9 px; no glyphs for
// Ł (U+0141) or CJK.
int fakeWidth(void*, const char* s) {
  int w = 0;
  for (const char* p = s; *p;) {
    const auto c = static_cast<unsigned char>(*p);
    if (c == 0xE2 && static_cast<unsigned char>(p[1]) == 0x80 && static_cast<unsigned char>(p[2]) == 0xA6) {
      w += 9;
      p += 3;
      continue;
    }
    w += 7;
    ++p;
    while ((static_cast<unsigned char>(*p) & 0xC0) == 0x80) ++p;
  }
  return w;
}
bool fakeHas(void*, uint32_t cp) { return cp != 0x141 && cp < 0x3000; }
bool noEllipsis(void*, uint32_t cp) { return cp != 0x2026; }

textfit::Font fakeFont() {
  textfit::Font f;
  f.width = fakeWidth;
  f.has = fakeHas;
  return f;
}

}  // namespace

void test_textfit_keeps_what_fits() {
  const textfit::Font f = fakeFont();
  char out[64];
  const textfit::Result r = textfit::fit(f, "Pénélope", strlen("Pénélope"), out, sizeof(out), 100);
  TEST_ASSERT_EQUAL_STRING("Pénélope", out);  // the font has é: kept as it is
  TEST_ASSERT_FALSE(r.folded);
  TEST_ASSERT_FALSE(r.cut);
  // Only the first inLen bytes (titles aren't NUL-terminated).
  textfit::fit(f, "Good Life.mp3", 9, out, sizeof(out), 200);
  TEST_ASSERT_EQUAL_STRING("Good Life", out);
}

void test_textfit_folds_missing_glyphs() {
  const textfit::Font f = fakeFont();
  char out[64];
  const char* in = "Łódź 東";
  const textfit::Result r = textfit::fit(f, in, strlen(in), out, sizeof(out), 300);
  TEST_ASSERT_TRUE(r.folded);
  TEST_ASSERT_EQUAL_STRING("Lódź ?", out);  // Ł folded, ó and ź kept, CJK '?'
}

void test_textfit_ellipsises_to_the_width() {
  const textfit::Font f = fakeFont();
  char out[64];
  const char* in = "Harder, Better, Faster, Stronger";
  const textfit::Result r = textfit::fit(f, in, strlen(in), out, sizeof(out), 100);
  TEST_ASSERT_TRUE(r.cut);
  TEST_ASSERT_TRUE(fakeWidth(nullptr, out) <= 100);
  // 13 characters (91 px) + "…" (9 px) = 100: the longest that fits.
  TEST_ASSERT_EQUAL_STRING("Harder, Bette\xE2\x80\xA6", out);
  // A font without "…": three dots.
  textfit::Font g = f;
  g.has = noEllipsis;
  textfit::fit(g, in, strlen(in), out, sizeof(out), 100);
  TEST_ASSERT_EQUAL_STRING("Harder, Bet...", out);
  // Never cuts inside a character.
  const char* acc = "éééééééééé";
  textfit::fit(f, acc, strlen(acc), out, sizeof(out), 40);
  TEST_ASSERT_EQUAL_STRING("éééé\xE2\x80\xA6", out);
}

void test_textfit_short_buffer_still_ends_in_an_ellipsis() {
  const textfit::Font f = fakeFont();
  char out[12];
  const char* in = "A very long album name indeed";
  const textfit::Result r = textfit::fit(f, in, strlen(in), out, sizeof(out), 1000);
  TEST_ASSERT_TRUE(r.cut);
  TEST_ASSERT_TRUE(strlen(out) < sizeof(out));
  TEST_ASSERT_EQUAL_STRING("A very\xE2\x80\xA6", out);  // the space before it dropped
  // Malformed UTF-8 never runs past the input.
  const char bad[] = {'a', static_cast<char>(0xE2), static_cast<char>(0x80)};
  char o2[16];
  textfit::fit(f, bad, sizeof(bad), o2, sizeof(o2), 1000);
  TEST_ASSERT_EQUAL_STRING("a??", o2);
}

void test_textfit_wraps_two_lines() {
  const textfit::Font f = fakeFont();
  char lines[3][64];
  const char* in = "Harder, Better, Faster, Stronger";
  // 140 px = 20 characters a line.
  int n = textfit::wrap(f, in, strlen(in), 140, 2, &lines[0][0], sizeof(lines[0]));
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_STRING("Harder, Better,", lines[0]);
  TEST_ASSERT_EQUAL_STRING("Faster, Stronger", lines[1]);
  // One line only: the rest ellipsised.
  n = textfit::wrap(f, in, strlen(in), 140, 1, &lines[0][0], sizeof(lines[0]));
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_TRUE(fakeWidth(nullptr, lines[0]) <= 140);
  TEST_ASSERT_EQUAL_STRING("Harder, Better, Fa\xE2\x80\xA6", lines[0]);
  // Short: one line.
  n = textfit::wrap(f, "Air", 3, 140, 2, &lines[0][0], sizeof(lines[0]));
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_STRING("Air", lines[0]);
  // A word longer than a line is cut where it overflows.
  const char* longWord = "Supercalifragilisticexpialidocious";
  n = textfit::wrap(f, longWord, strlen(longWord), 70, 3, &lines[0][0], sizeof(lines[0]));
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_STRING("Supercalif", lines[0]);
  TEST_ASSERT_EQUAL_STRING("ragilistic", lines[1]);
  TEST_ASSERT_TRUE(fakeWidth(nullptr, lines[2]) <= 70);
  // Empty.
  TEST_ASSERT_EQUAL_INT(0, textfit::wrap(f, "", 0, 140, 2, &lines[0][0], sizeof(lines[0])));
}

// ---- TabBarModel ----

void test_tabbar_hit_areas_reach_the_right_edge() {
  TEST_ASSERT_EQUAL_INT(0, tabbar::tabAt(0, false));
  TEST_ASSERT_EQUAL_INT(0, tabbar::tabAt(53, false));
  TEST_ASSERT_EQUAL_INT(1, tabbar::tabAt(54, false));
  TEST_ASSERT_EQUAL_INT(3, tabbar::tabAt(215, false));
  TEST_ASSERT_EQUAL_INT(4, tabbar::tabAt(216, false));
  TEST_ASSERT_EQUAL_INT(4, tabbar::tabAt(319, false));
  // The panel stops at 319 and the correction puts that at ~281: a clamped
  // reading is the Output tab wherever it was corrected to.
  TEST_ASSERT_EQUAL_INT(4, tabbar::tabAt(190, true));
  TEST_ASSERT_EQUAL_INT(319, tabbar::cellX1(4));
  TEST_ASSERT_EQUAL_INT(161, tabbar::cellX1(2));
}

void test_tabbar_redraws_only_what_changed() {
  tabbar::State a;
  a.active = 1;
  a.play = tabbar::Play::Playing;
  tabbar::State b = a;
  TEST_ASSERT_EQUAL_UINT8(0, tabbar::dirty(a, b));
  b.active = 3;
  TEST_ASSERT_EQUAL_UINT8((1u << 1) | (1u << 3), tabbar::dirty(a, b));
  b = a;
  b.eqStep = 5;
  TEST_ASSERT_EQUAL_UINT8(1u << 0, tabbar::dirty(a, b));
  a.play = b.play = tabbar::Play::Paused;  // paused: the step doesn't move anything
  TEST_ASSERT_EQUAL_UINT8(0, tabbar::dirty(a, b));
  b = a;
  b.upNext = 12;
  TEST_ASSERT_EQUAL_UINT8(1u << 2, tabbar::dirty(a, b));
  b = a;
  b.volume = 65;  // the volume lives in the Output cell
  TEST_ASSERT_EQUAL_UINT8(1u << 4, tabbar::dirty(a, b));
  b = a;
  b.battery = 50;
  b.output = tabbar::Output::BtConnecting;
  TEST_ASSERT_EQUAL_UINT8(1u << 4, tabbar::dirty(a, b));
}

void test_tabbar_eq_badge_progress() {
  uint8_t h[4], h2[4];
  tabbar::eqBars(7, true, h);
  tabbar::eqBars(7, true, h2);
  bool differs = false;
  for (int i = 0; i < 4; ++i) {
    TEST_ASSERT_TRUE(h[i] >= 1 && h[i] <= tabbar::kEqMaxH);
    TEST_ASSERT_EQUAL_UINT8(h[i], h2[i]);  // the same step, the same bars
  }
  for (uint8_t s = 0; s < 16; ++s) {
    tabbar::eqBars(s, true, h2);
    for (int i = 0; i < 4; ++i) differs |= h2[i] != h[i];
  }
  TEST_ASSERT_TRUE(differs);
  tabbar::eqBars(7, false, h);
  TEST_ASSERT_EQUAL_UINT8(h[0], h[3]);  // paused: flat
  char t[4];
  tabbar::badgeText(0, t);
  TEST_ASSERT_EQUAL_STRING("", t);
  tabbar::badgeText(12, t);
  TEST_ASSERT_EQUAL_STRING("12", t);
  tabbar::badgeText(1234, t);
  TEST_ASSERT_EQUAL_STRING("99+", t);
  TEST_ASSERT_EQUAL_UINT8(0, tabbar::progressPx(1000, 0));
  TEST_ASSERT_EQUAL_UINT8(16, tabbar::progressPx(30000, 60000));
  TEST_ASSERT_EQUAL_UINT8(tabbar::kProgressW, tabbar::progressPx(70000, 60000));
}

// ---- TrackProgress ----

void test_progress_estimate_from_the_file() {
  // A 128 kbit/s MP3 (16,000 B/s) of 200 s after a 300 KB ID3 tag with a
  // cover: 10 s in, the decoder has read 160,000 B past the tag.
  const uint32_t tag = 300000, size = tag + 200 * 16000;
  const uint64_t frames = 10ull * 44100;
  const uint32_t est = progress::estimateDurationMs(frames, 44100, tag, tag + 160000, size);
  TEST_ASSERT_UINT32_WITHIN(5, 200000, est);
  // Too early, nothing read, or nonsense: unknown.
  TEST_ASSERT_EQUAL_UINT32(0, progress::estimateDurationMs(1000, 44100, tag, tag + 400, size));
  TEST_ASSERT_EQUAL_UINT32(0, progress::estimateDurationMs(frames, 44100, tag, tag, size));
  TEST_ASSERT_EQUAL_UINT32(0, progress::estimateDurationMs(frames, 0, tag, tag + 160000, size));
  TEST_ASSERT_EQUAL_UINT32(0, progress::estimateDurationMs(frames, 44100, tag, size + 1, size));
  // At the end of the file: what was made.
  TEST_ASSERT_EQUAL_UINT32(10000, progress::estimateDurationMs(frames, 44100, 0, 1000, 1000));
}

// A Layer III frame header: MPEG-1, 128 kbit/s, 44.1 kHz, joint stereo.
static void putFrameHeader(uint8_t* p, bool mpeg1 = true, bool mono = false) {
  p[0] = 0xFF;
  p[1] = mpeg1 ? 0xFB : 0xF3;           // MPEG-1 or -2, Layer III, no CRC
  p[2] = 0x90;                          // 128 kbit/s (MPEG-1) / 80 (MPEG-2), 44.1 / 22.05 kHz, no padding
  p[3] = mono ? 0xC4 : 0x44;
}

static void putBe32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}

void test_progress_mp3_header_length() {
  static uint8_t buf[2048];
  // A LAME VBR file: junk, then a Xing frame (frames flag) and the next frame.
  memset(buf, 0, sizeof(buf));
  buf[3] = 0xFF;  // a stray sync byte in the junk
  const int at = 20;
  putFrameHeader(buf + at);
  memcpy(buf + at + 4 + 32, "Xing", 4);
  putBe32(buf + at + 4 + 32 + 4, 0x0F);
  putBe32(buf + at + 4 + 32 + 8, 12000);  // frames
  putFrameHeader(buf + at + 417);         // 144 * 128000 / 44100 = 417
  // 12,000 x 1152 / 44100 = 313.469 s
  TEST_ASSERT_EQUAL_UINT32(313469, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  // "Info" (LAME's CBR tag) says it the same way.
  memcpy(buf + at + 4 + 32, "Info", 4);
  TEST_ASSERT_EQUAL_UINT32(313469, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  // No frame count in the flags: unknown.
  putBe32(buf + at + 4 + 32 + 4, 0x0E);
  TEST_ASSERT_EQUAL_UINT32(0, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  // A plain CBR frame: no header, 0 (the estimate is exact there).
  memset(buf + at + 4, 0, 60);
  TEST_ASSERT_EQUAL_UINT32(0, progress::mp3HeaderDurationMs(buf, sizeof(buf)));

  // VBRI (Fraunhofer), 32 bytes after the header whatever the mode.
  memset(buf, 0, sizeof(buf));
  putFrameHeader(buf);
  memcpy(buf + 36, "VBRI", 4);
  putBe32(buf + 36 + 14, 4410);
  putFrameHeader(buf + 417);
  TEST_ASSERT_EQUAL_UINT32(115200, progress::mp3HeaderDurationMs(buf, sizeof(buf)));  // 4410 x 1152 / 44.1

  // MPEG-2 mono: 576 samples a frame at 22,050 Hz, side info 9 bytes.
  memset(buf, 0, sizeof(buf));
  putFrameHeader(buf, false, true);
  memcpy(buf + 4 + 9, "Xing", 4);
  putBe32(buf + 4 + 9 + 4, 1);
  putBe32(buf + 4 + 9 + 8, 3828);
  TEST_ASSERT_EQUAL_UINT32(99996, progress::mp3HeaderDurationMs(buf, 64));  // 3828 x 576 / 22050, next frame past the end

  // Nothing but zeros, or too short.
  memset(buf, 0, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(0, progress::mp3HeaderDurationMs(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_UINT32(0, progress::mp3HeaderDurationMs(buf, 3));
}

void test_progress_flac_streaminfo() {
  uint8_t b[42] = {'f', 'L', 'a', 'C', 0x80, 0, 0, 34};  // last block, STREAMINFO, 34 bytes
  // 44,100 Hz (0x0AC44), 2 channels, 16 bits, 10,000,000 samples (0x989680).
  b[18] = 0x0A;
  b[19] = 0xC4;
  b[20] = 0x42;  // rate's last 4 bits (4), channels-1 (1) and the top bit of bps-1 (0)
  b[21] = 0xF0;  // the rest of bps-1 (15), total's top 4 bits (0)
  putBe32(b + 22, 10000000);
  TEST_ASSERT_EQUAL_UINT32(226757, progress::flacDurationMs(b, sizeof(b)));  // 10e6 / 44100 s
  b[21] = 0xF1;  // 36-bit totals: + 2^32 samples
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>((4294967296ull + 10000000ull) * 1000 / 44100),
                           progress::flacDurationMs(b, sizeof(b)));
  b[21] = 0xF0;
  putBe32(b + 22, 0);  // total unknown
  TEST_ASSERT_EQUAL_UINT32(0, progress::flacDurationMs(b, sizeof(b)));
  putBe32(b + 22, 10000000);
  b[0] = 'I';  // not a FLAC file
  TEST_ASSERT_EQUAL_UINT32(0, progress::flacDurationMs(b, sizeof(b)));
  b[0] = 'f';
  TEST_ASSERT_EQUAL_UINT32(0, progress::flacDurationMs(b, 20));
}

void test_progress_id3v2_size() {
  const uint8_t tag[10] = {'I', 'D', '3', 4, 0, 0, 0x00, 0x12, 0x2B, 0x05};  // syncsafe 0x12 0x2B 0x05
  TEST_ASSERT_EQUAL_UINT32(10 + ((0x12u << 14) | (0x2Bu << 7) | 0x05u), progress::id3v2Size(tag, 10));
  uint8_t footer[10];
  memcpy(footer, tag, 10);
  footer[5] = 0x10;
  TEST_ASSERT_EQUAL_UINT32(20 + ((0x12u << 14) | (0x2Bu << 7) | 0x05u), progress::id3v2Size(footer, 10));
  const uint8_t frame[10] = {0xFF, 0xFB, 0x90, 0x44, 0, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT32(0, progress::id3v2Size(frame, 10));
  uint8_t bad[10];
  memcpy(bad, tag, 10);
  bad[7] = 0x80;  // not syncsafe
  TEST_ASSERT_EQUAL_UINT32(0, progress::id3v2Size(bad, 10));
  TEST_ASSERT_EQUAL_UINT32(0, progress::id3v2Size(tag, 9));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nav_tabs_keep_their_stacks);
  RUN_TEST(test_nav_tapping_the_active_tab_pops_to_its_root);
  RUN_TEST(test_nav_back_pops_one_level_and_stops_at_the_root);
  RUN_TEST(test_nav_deep_pushes_drop_the_oldest_above_the_root);
  RUN_TEST(test_nav_replace_above_root);
  RUN_TEST(test_frame_clock_keeps_deadlines);
  RUN_TEST(test_frame_clock_restarts_after_a_stall);
  RUN_TEST(test_list_items_with_top_bar_and_inline_bar);
  RUN_TEST(test_list_expand_keeps_the_row_and_shows_its_bar);
  RUN_TEST(test_list_expand_at_the_end_stays_in_the_list);
  RUN_TEST(test_list_reveal_and_top_of);
  RUN_TEST(test_list_rail_geometry);
  RUN_TEST(test_bitset_select_count_list);
  RUN_TEST(test_bitset_without_memory);
  RUN_TEST(test_textfit_keeps_what_fits);
  RUN_TEST(test_textfit_folds_missing_glyphs);
  RUN_TEST(test_textfit_ellipsises_to_the_width);
  RUN_TEST(test_textfit_short_buffer_still_ends_in_an_ellipsis);
  RUN_TEST(test_textfit_wraps_two_lines);
  RUN_TEST(test_tabbar_hit_areas_reach_the_right_edge);
  RUN_TEST(test_tabbar_redraws_only_what_changed);
  RUN_TEST(test_tabbar_eq_badge_progress);
  RUN_TEST(test_progress_estimate_from_the_file);
  RUN_TEST(test_progress_mp3_header_length);
  RUN_TEST(test_progress_id3v2_size);
  RUN_TEST(test_progress_flac_streaminfo);
  return UNITY_END();
}
