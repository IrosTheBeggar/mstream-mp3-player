// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Names made to fit a column in a given font: the UI's text layer (the
// VLW DejaVu fonts), portable and host-tested with a fake font.
//
//   - A code point the font has no glyph for becomes its TextFold
//     look-alike (Full mode: "Ł" -> "L", "½" -> "1/2", "?" when nothing
//     better): the DejaVu set covers Latin-1, the common Latin Extended-A
//     letters and the typographic punctuation, so this is rare, but a
//     missing glyph would otherwise draw as a blank box.
//   - Too wide: cut at a character boundary, trailing spaces dropped, and an
//     ellipsis added ("…" when the font has it, else "...").
//   - wrap(): up to n lines, broken at spaces, the last one ellipsised
//     (Now Playing's title).
//
// The font is two callbacks: a string's width in pixels, and whether it has
// a glyph for a code point (nullptr: every one).
namespace textfit {

struct Font {
  void* ctx = nullptr;
  int (*width)(void* ctx, const char* utf8) = nullptr;  // pixels of a NUL-terminated string
  bool (*has)(void* ctx, uint32_t cp) = nullptr;        // nullptr: every code point
};

struct Result {
  size_t length = 0;    // bytes written, not counting the NUL
  bool folded = false;  // a code point was replaced
  bool cut = false;     // cut and ellipsised
};

// The first `inLen` bytes of `in` (UTF-8), with the glyphs the font lacks
// replaced. `out` is always NUL-terminated (outSize > 0); an input longer
// than `out` is cut at a character boundary.
Result prepare(const Font& f, const char* in, size_t inLen, char* out, size_t outSize);
// prepare(), then cut with an ellipsis to fit `maxW` px.
Result fit(const Font& f, const char* in, size_t inLen, char* out, size_t outSize, int maxW);
// Cuts `text` (prepared, NUL-terminated, in a buffer of `size`) in place.
bool ellipsize(const Font& f, char* text, size_t size, int maxW);

// Up to `maxLines` lines of at most `maxW` px, broken after spaces (a word
// wider than a line is cut where it overflows); the last line holds the
// rest, ellipsised. Line k goes to out + k * lineSize. Returns the lines
// written (0 for an empty text).
int wrap(const Font& f, const char* in, size_t inLen, int maxW, int maxLines, char* out, size_t lineSize);

}  // namespace textfit
