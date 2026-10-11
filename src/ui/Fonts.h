// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <M5GFX.h>

#include <cstddef>
#include <cstdint>

#include "TextFit.h"

// The UI's text layer: DejaVu Sans as anti-aliased VLW fonts from flash
// (ui/VlwFonts, as the UI spike recommended: it looked best, ’ ‐ é É
// included; Latin, Greek and Cyrillic since docs/I18N.md's phase 1), the
// Chinese, Japanese and Korean characters they lack from the built-in
// glyph store behind them (CjkGlyphs16: 16 px, 4 bits a pixel, in flash;
// phase 2), and the fitting that every label goes through (TextFit): a
// character neither has is folded (TextFold: to ASCII, or '?' in another
// script), and a name too wide for its column is cut with "…".
//
// Four roles:
//   Body   DejaVu Sans 16      row titles, buttons, the artist on Now Playing
//   Small  DejaVu Sans 13      secondary lines, labels, times
//   Bold   DejaVu Sans Bold 16 headers, the current row, primary buttons
//   Title  DejaVu Sans Bold 22 Now Playing's title (one with CJK in it:
//                              Bold, TitleLayout), big numbers
// The store has one size and one weight: every role draws its CJK glyphs
// at 16 px on the role's baseline in its row (the Small row's a pixel
// lower, so the 16 px em box fits its 17 px), inside the row; Bold and
// Title draw them a synthetic bold (glyphstore::embolden(): a column
// wider, a pixel further), and measure them so.
//
// load() reads the fonts' glyph tables into PSRAM (M5GFX does: 9 B a glyph,
// 6.6 KB a font) once; the glyph bitmaps stay in flash, and the store is
// read where it lies (no load, no copy). Text is drawn with a background
// colour (sprites, or the LCD in small strips), so a redraw in place never
// flickers. Loop task only.
namespace ui {

enum class Font : uint8_t { Body, Small, Bold, Title };

class Fonts {
public:
  // False: a font failed to load (no PSRAM); text then falls back to Font2.
  bool load();
  bool loaded() const { return loaded_; }
  const lgfx::IFont* get(Font f) const;
  // The TextFit view of a font (width and glyph coverage: the VLW font's,
  // then the glyph store's).
  textfit::Font fit(Font f) const;
  // Pixel width of a UTF-8 string as draw() draws it, uncut: composed and
  // folded first (textfit::drawnWidth(): halfwidth katakana as the
  // fullwidth, NFD Hangul as its syllables), each glyph by its advance.
  int width(Font f, const char* s) const;
  int height(Font f) const;
  // Whether `s` as draw() draws it in `f` (composed and folded first) has a
  // glyph from the store: a Chinese, Japanese or Korean character. Now
  // Playing's title then takes the Bold rows (TitleLayout). A lookup a
  // character, no buffer: cheap enough for each new title.
  bool fromStore(Font f, const char* s) const;

  // Draws `text` (the first `len` bytes; UTF-8, not necessarily
  // NUL-terminated) fitted to `maxW` px, vertically centred on `y`,
  // left-aligned at `x` (or right-aligned / centred by `datum`). Returns
  // the width drawn. The line's whole height is filled with `bg` behind
  // the glyphs, which cuts into a line drawn close above or below (its
  // descenders, a border); bg == fg draws the glyphs only, blended with
  // what is there (a sprite can be read back). A line with glyphs from the
  // store is drawn a glyph at a time here (drawMixed()); the rest by M5GFX.
  enum class Align : uint8_t { Left, Centre, Right };
  int draw(lgfx::LovyanGFX& g, Font f, const char* text, size_t len, int x, int y, int maxW, uint16_t fg,
           uint16_t bg, Align align = Align::Left) const;
  int draw(lgfx::LovyanGFX& g, Font f, const char* text, int x, int y, int maxW, uint16_t fg, uint16_t bg,
           Align align = Align::Left) const;

  static Fonts& instance();

private:
  struct Slot {
    lgfx::PointerWrapper data;
    lgfx::VLWfont font;
  };
  const lgfx::VLWfont* vlw(Font f) const { return loaded_ ? &slots_[static_cast<int>(f)].font : nullptr; }
  // TextFit's width: the Body and Small fonts', and the Bold and Title
  // fonts' (a store glyph bold: a pixel wider).
  static int widthOf(void* ctx, const char* s);
  static int boldWidthOf(void* ctx, const char* s);
  static bool hasGlyph(void* ctx, uint32_t cp);
  // A fitted line with a glyph from the store: aligned from its own width
  // and drawn a glyph at a time, the VLW ones by M5GFX's VLWfont::drawChar
  // (what drawString() calls), the store's by ours (bold in Bold and
  // Title). Only with the fonts loaded.
  static int drawMixed(lgfx::LovyanGFX& g, Font f, const char* s, int x, int y, Align align);

  Slot* slots_ = nullptr;  // PSRAM, 4
  bool loaded_ = false;
};

}  // namespace ui
