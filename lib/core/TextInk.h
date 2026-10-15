// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "GlyphStore.h"

// Which pixel rows a line of text inks, as ui/Fonts draws it: the VLW
// DejaVu fonts' glyphs where M5GFX puts them (VLWfont::drawChar(): a glyph
// `dY` above the baseline starts maxAscent - dY rows below the line's top,
// maxAscent and the line's height worked out over the glyphs as
// VLWfont::loadFont() does), the glyph store's (CJK, Hangul, ★ ♪) on
// Store::baselineIn() by glyphstore::place(). Portable, so the host tests
// measure with the firmware's own font data (src/ui/VlwFonts.cpp) and the
// store: what lets two lines share a 36 px toast without touching
// (uitext::toastLines(), Toast in ui/Overlays).
//
// A glyph's rows are its box's (the VLW bitmap's rows, the store's tight
// box): a box's blank rows count as ink, so the answer is never short.
// What draws nothing inks nothing: a space, a control character, a
// variation selector (M5GFX skips them), a character neither has (M5GFX's
// blank; TextFit folds them first anyway).
namespace textink {

// Rows from the line's top (top > bottom: none).
struct Rows {
  int top = 0;
  int bottom = -1;
  bool empty() const { return bottom < top; }
  int height() const { return empty() ? 0 : bottom - top + 1; }
};

// A VLW font file (tools/vlw_font.py's: big-endian, a 24-byte header, then
// 28 bytes a glyph sorted by code point), read where it lies.
class Vlw {
public:
  // False: not one (no glyphs, or the table past `size`): empty then.
  bool open(const uint8_t* data, size_t size);
  // M5GFX's maxAscent (the baseline, rows from the line's top) and its
  // yAdvance (the line's height).
  int baseline() const { return baseline_; }
  int lineHeight() const { return lineH_; }
  // The rows glyph `cp` inks in its line; false: the font hasn't it.
  bool rowsOf(uint32_t cp, Rows* out) const;

private:
  uint32_t be32(size_t at) const;
  const uint8_t* data_ = nullptr;
  uint32_t count_ = 0;
  int baseline_ = 0;
  int lineH_ = 0;
};

// The rows `text` (UTF-8, NUL-terminated: as TextFit fitted it, what
// ui/Fonts draws) inks in a line of `font`, a glyph the font lacks from
// `store`.
Rows inkOf(const Vlw& font, const glyphstore::Store& store, const char* text);

}  // namespace textink
