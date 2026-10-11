// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>

#include "TextFit.h"

// Now Playing's title (ui/NowPlayingPage) in its strip, 52 rows tall, the
// text 190 px wide (x 120-310):
//   - Title (DejaVu Sans Bold 22, rows of 27 px): up to two lines, one on
//     the strip's middle row (26), two on rows 13 and 39.
//   - Bold (DejaVu Sans Bold 16, rows of 19 px): up to three lines 17 rows
//     apart, centred: 9, 26 and 43; 17 and 34 for two; 26 for one.
// Bold when two Title lines would cut the title short, and for any title
// with a glyph from the glyph store: Han, kana and Hangul, and the symbols
// the store has and the UI fonts don't (JIS X 0208's ★ ☆ ♪ → ∞ ※ ...,
// the ideographic space U+3000). The store's glyphs are 16 px in every
// role, so beside 22 px Latin they look small; in Bold the Latin is their
// size, and they are drawn bold too (docs/I18N.md, phase 2). A line's row
// is taller than the pitch (Bold's 19 rows 17 apart, Title's 27 rows 26
// apart), so the page draws the lines with no background, over the strip
// it filled: a line's own background would erase the lowest row of the
// ink above it. Portable, host-tested (test_ui_library, with the fonts'
// own data and the store).
namespace titlelayout {

constexpr int kStripH = 52;
constexpr int kMaxLines = 3;       // Bold's; Title's is 2
constexpr int kBigPitch = 26;      // Title's lines, rows apart
constexpr int kBoldPitch = 17;     // Bold's
constexpr int kBoldFirstY = 9;     // Bold's first line of three
constexpr size_t kLineSize = 112;  // a line's bytes, its NUL included

struct Lines {
  bool bold = false;  // in Bold, else in Title
  int count = 0;      // the lines written; 0 for an empty title
};

// `title` (UTF-8, NUL-terminated, as stored) in lines of at most `maxW` px,
// into `out` (kMaxLines lines: TextFit's wrap()). `big` and `bold` are the
// Title and the Bold font as TextFit sees them (ui/Fonts::fit());
// `fromStore` says whether the title as drawn has a glyph from the store
// (ui/Fonts::fromStore()). Six arguments: on the ESP32 all in registers.
Lines wrap(const textfit::Font& big, const textfit::Font& bold, bool fromStore, const char* title, int maxW,
           char out[][kLineSize]);

// Line i's middle row, from the strip's top (Fonts::draw() centres on it).
int lineY(const Lines& l, int i);

// The last line of a wrap was cut ("…" or "..." added): `text` needed more
// room than the lines had.
bool cutShort(const char* line, const char* text);

}  // namespace titlelayout
