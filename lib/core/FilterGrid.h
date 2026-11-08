// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "MoodFilter.h"
#include "TextFit.h"

// The mood filters' editor and the Albums list's first row, as numbers
// (docs/MOOD-FILTERS.md 7.1-7.4): the editor's cells, their font, widths
// and hit tests, what a tap does to the filter; the first row's summary,
// count, cross and the Albums segment's dot. Portable, host-tested
// (test_ui_filter; test_ui_library with the firmware's fonts in every
// language); ui/FilterSheet and ui/LibraryPage draw them. The fonts come
// as TextFit views (ui/Fonts::fit()), as librarytext's do.
//
// The editor (7.1): an opaque panel, y 36-239, under the tab bar.
//   Title row: Clear, a pill from x 12 (its word in Small, and 24 px); the
//   count (Small) centred in what it leaves before x 256; the cross, a
//   pill at x 262.
//   Rows 1-4 at y 68, 112, 156, 200, 36 tall, x 8-311:
//     1  valence  a tray: Happy | Neutral | Sad, pick one
//     2  energy   a tray: Relaxed | Neutral | Aggressive, pick one
//     3  voice    a tray: Instrumental | Vocals, pick one; then the level
//                 cell, Strict, Standard or Loose
//     4  tags     three chips: Acoustic, Party, Danceable, pick any
//   A tray's cells share its inside (2 px in from each end); the chips
//   share 304 px less their two 6 px gaps.
//
// Widths (7.3), each row on its own, in whole pixels:
//   - the grid's font: Body when every row's words fit it (each word + 12,
//     the gaps, within the row), else Small for every word (Polish and
//     Russian);
//   - equal while each word + 12 fits its share (the shares' edges
//     rounded: 97, 98, 97 of 292);
//   - else proportional: each cell its word + 12 and an even share of
//     what is left;
//   - in Small, a row still too narrow: a tray's cells scale to it (the
//     voice tray in Polish and Russian), the chips give up the overflow
//     evenly (Russian's tags, as the design's frame draws them);
//   - a tag over 80 px in Small with a space wraps there (Russian's "Для
//     вечеринки"), its need its wider line + 12. Its two lines are drawn
//     glyphs only (fg == bg) at y+10 and y+26 of the chip.
//   An even share of pixels rounds to the nearest, a half down (Spanish's
//   and French's voice trays, as drawn).
//
// Hits (7.4): y reads true (TouchCalibration). x reads too far right on
// some panels (the lab's: +20 px at x 190, +35-45 from 240), so each split
// between two cells sits right of their edge by skew(): 0 under x 150, 6
// under 190, else 13. A gap between chips, and the voice tray's before the
// level cell, split at their middle. A reading clamped at the right edge
// is its row's last cell. Above y 66 the title row: Clear to its pill's
// right edge (and the skew there), the cross from x 250 (a sheet's ✕), the
// count between them nothing.
namespace filtergrid {

// The editor's cells, in rows. None: the count, or no panel.
enum class Cell : uint8_t {
  Happy,
  NeutralV,
  Sad,
  Relaxed,
  NeutralE,
  Aggressive,
  Instrumental,
  Vocals,
  Level,
  Acoustic,
  Party,
  Danceable,
  Clear,
  Cross,
  None,
};
inline constexpr int kCells = 14;  // Happy to Cross
inline constexpr int index(Cell c) { return static_cast<int>(c); }

// ---- the editor's geometry (7.1) ----
inline constexpr int kPanelY = 36;  // the panel: y 36-239
inline constexpr int kPillY = 40, kPillH = 24, kPillR = 12;
inline constexpr int kClearX = 12, kClearPad = 24;      // Clear's pill: its word (Small) and 24 px
inline constexpr int kCrossX = 262, kCrossW = 50;       // the cross's pill; its icon at 287, 52
inline constexpr int kCrossHitX = 250;                  // its hit area, to the edge (a sheet's ✕)
inline constexpr int kCountRight = 256, kCountGap = 4;  // the count, 4 px clear of Clear and of x 256
inline constexpr int kRows = 4;
inline constexpr int kRowY[kRows] = {68, 112, 156, 200};
inline constexpr int kRowH = 36, kRowPitch = 44;
inline constexpr int kRowX = 8, kRowW = 304;
inline constexpr int kTrayR = 10, kTrayInset = 2;  // a tray's cells from x 10, in its width less 4
inline constexpr int kChipR = 18, kChipGap = 6;
inline constexpr int kSegmentInset = 2, kSegmentR = 8;  // a picked tray cell: (x, y+2, w, 32)
inline constexpr int kSeparatorTop = 10, kSeparatorH = 16;  // y+10 to y+25, between two unpicked
inline constexpr int kLevelGap = 6, kLevelMinW = 70, kLevelPad = 14;  // the level cell: widest word + 14
inline constexpr int kLevelLabelY = 12, kLevelDotsY = 27, kLevelDotStep = 10, kLevelDotD = 5;
inline constexpr int kWordPad = 12;  // a word's need: its width and this
inline constexpr int kWrapW = 80;    // a tag wider than this in Small, with a space, wraps
inline constexpr int kWrapLineY[2] = {10, 26};
// Hits: the title row above y 66, row k from 66 + 44 k (the last to the
// screen's bottom).
inline constexpr int kTitleBand = 66;

// Where a split sits right of the edge it splits at (above).
constexpr int skew(int x) { return x < 150 ? 0 : x < 190 ? 6 : 13; }

// The words a layout measures: each cell's (Level's is unused; Clear's is
// its pill's) and the level cell's three, in moodfilter::Level's order
// (Standard, Strict, Loose).
struct Words {
  const char* cell[kCells] = {};
  const char* level[3] = {};
};
// The UI's language's (uistr::tr(): the mood.* texts).
Words words();
// The level cell's word for `l`.
const char* levelWord(moodfilter::Level l);

struct Layout {
  // How a row shares out its width (above). The chips' overflow, given up
  // evenly, is Proportional (a spare below 0).
  enum class Share : uint8_t { Equal, Proportional, Scaled };
  bool small = false;        // every word in Small (else Body)
  Share share[kRows] = {};
  int16_t x[kCells] = {};    // a tray's cell, the level cell, a chip, a pill
  int16_t w[kCells] = {};
  int16_t trayW[3] = {};     // rows 1-3's trays, from x 8
  uint8_t wrap[kCells] = {};  // a tag on two lines: the byte its second line starts at (0: one line)
  int16_t countX = 0;        // the title row's count: its centre and its room (Small)
  int16_t countW = 0;
};
// The grid for these words, `body` and `small` the Body and Small fonts.
Layout layout(const Words& words, const textfit::Font& body, const textfit::Font& small);

// A cell's row (0-3; -1 the title row's Clear and Cross) and its box: a
// tray cell's is its share of the tray (x, its row's y, w, 36).
int rowOf(Cell c);
struct Box {
  int x = 0, y = 0, w = 0, h = 0;
};
Box box(const Layout& l, Cell c);

// The cell a reading at (x, y) on the panel is (7.4); `rightEdge`: the
// panel's reading was clamped at the right edge (InputEvent::atRightEdge()).
// None for the count.
Cell hit(const Layout& l, int x, int y, bool rightEdge);
// Where row `row`'s cell k ends for a hit, k from 0 to the row's cells
// less 2: a reading at or right of it is cell k + 1 (or later). For the
// host tests' margins.
int split(const Layout& l, int row, int k);

// ---- the filter (4.1, 4.3) ----

// The filter after a tap on `c`: a tray's cell picks it, or lets it go on
// a second tap; a tag toggles (any number); the level cycles Strict >
// Standard > Loose > Strict; Clear empties every group and the level goes
// back to Standard. The cross and None change nothing.
moodfilter::Filter tap(const moodfilter::Filter& f, Cell c);
// Whether `c` is drawn picked (in the accent): a tray's cell or a chip.
bool picked(const moodfilter::Filter& f, Cell c);
// Something to clear: a group, or a level other than Standard (Clear's
// word in TXT, else FAINT).
bool anythingSet(const moodfilter::Filter& f);
// The level cell's lit dot: 0 Strict, 1 Standard, 2 Loose (at dx -10, 0,
// 10). The cell is live only while the filter is active (a level alone
// filters nothing: 4.1).
int levelDot(moodfilter::Level l);

// What is set, as the first row says it (7.2): the picked words in the
// order valence, energy, voice, tags, then the level when it isn't
// Standard, joined by " · "; "" when nothing is. Into `out` (`size` > 0;
// a buffer too short ends after the last whole word that fits):
// kSummaryBytes holds every language's longest (Russian's, 186 B).
inline constexpr size_t kSummaryBytes = 200;
size_t summary(const moodfilter::Filter& f, char* out, size_t size);

// The count (7.2, 4.3), the albums on the Albums list's first row and in
// the editor opened from it, the tracks in the one Auto DJ opens. With an
// active filter "312 of 1,741 albums" (mood.albums_of, mood.tracks_of)
// when it fits `room` px in `small`, else "312 of 1,741"
// (queue.summary_position); with none "1,741 albums" (library.albums_n,
// library.tracks_n). Counts grouped (uistr::grouped()). Returns the bytes
// written; kCountBytes holds them.
enum class Scope : uint8_t { Albums, Tracks };
inline constexpr size_t kCountBytes = 64;
size_t countText(const textfit::Font& small, int room, Scope scope, bool active, uint32_t fits, uint32_t total,
                 char* out, size_t size);

// ---- the Albums list's first row (7.2) ----
// A list item (ListView's top row), 42 tall, scrolling with the list; its
// right edge `right` is the list's: 284 beside the A-Z rail, 312 beside
// the scroll bar, 320 with neither.
inline constexpr int kTopRowH = 42;
inline constexpr int kRightRail = 284, kRightBar = 312, kRightNone = 320;
inline constexpr int kRailHitX = 280;  // the rail takes a tap from here (ui::kEdgeHitX)
inline constexpr int kFunnelX = 26, kFunnelY = 21;
inline constexpr int kPlateX = 6, kPlateY = 1, kPlateD = 40, kPlateR = 4;
inline constexpr int kTopTextX = 54, kTopTitleY = 13, kTopCountY = 31;
inline constexpr int kChevronInset = 14;  // the chevron at right - 14
inline constexpr int kDiscR = 14;
// With a filter on: the cross's disc centre, 232 beside the rail (on the
// lab's panel its centre still reads left of the rail), else the right
// less 28: 284, 292.
constexpr int crossX(int right) { return right <= kRightRail ? 232 : right - 28; }
// Its hit area: from 8 px left of the centre to the edge, or to the rail.
constexpr int crossHitX(int right) { return crossX(right) - 8; }
// The text's room from x 54: at rest the row less 80 (204, 232, 240); on,
// to 72 px before the cross's centre (160, 212, 220).
constexpr int textRoom(int right, bool on) { return on ? crossX(right) - 72 : right - 80; }

// What a tap on the row is: the cross (on: clears the filter), the rail's
// (beside it: x 280 on, or a reading clamped at the edge), else the
// editor. On the lab's panel the last 14-18 px of the summary reads as the
// cross: that is accepted, a tap on the row's own text (7.4).
enum class Top : uint8_t { Editor, Cross, Rail };
Top topHit(int x, bool rightEdge, int right, bool on);

// The dot on the Albums segment (7.2): fillRoundRect(203, 15, 6, 6, 3) in
// the header strip's coordinates (y 54 on the screen), the Library's
// accent. Shown while a filter is on, Albums is the segment on show, and
// the list is scrolled past its first row.
inline constexpr int kDotX = 203, kDotY = 15, kDotD = 6;
constexpr bool dotShown(bool active, bool albums, int scrollPx) { return active && albums && scrollPx >= kTopRowH; }

}  // namespace filtergrid
