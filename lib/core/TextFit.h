// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Names made to fit a column in a given font: the UI's text layer (the
// VLW DejaVu fonts), portable and host-tested with a fake font.
//
//   - The text is composed first (textfold::Composer: NFD "e" + U+0301
//     draws as the font's "é", halfwidth "ｶﾞ" as "ガ"), for the screen
//     only. A kana voicing mark left over (ア + U+3099: no such kana) is
//     the spacing mark the font has, ゛ or ゜.
//   - A code point the font has no glyph for becomes, in this order: its
//     Windows-1252 character when it is a C1 control the font has that for
//     (U+0092 as ’: Latin-1 tags keep cp1252 bytes so, docs/METADATA.md
//     5.2); else its TextFold look-alike (Full mode: "ỹ" -> "y", "ǆ" ->
//     "dz", "Ａ" -> "A", "½" -> "1/2", a stray combining mark dropped, "?"
//     when nothing better): the DejaVu set covers Latin-1, Latin
//     Extended-A, Greek, Cyrillic and the typographic punctuation, and the
//     glyph store behind it (ui/Fonts: docs/I18N.md, phase 2) Chinese,
//     Japanese and Korean, so this is rare, but a missing glyph would
//     otherwise draw as a blank box.
//   - Too wide: cut at a character boundary (never between a letter and a
//     combining mark on it), trailing spaces dropped, and an ellipsis added
//     ("…" when the font has it, else "...").
//   - wrap(): up to n lines, broken at spaces and between Chinese and
//     Japanese characters (not before a closing mark or a small kana, not
//     after an opening mark), the last one ellipsised (Now Playing's title).
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
// The width of `text` (UTF-8, NUL-terminated, as stored) as fit() draws it
// uncut: composed and folded first, so halfwidth "ｶﾞ" measures as "ガ", NFD
// jamo as their syllable, a missing glyph as its fold. What a caller
// compares with its room to choose a font or a line (ui/Fonts::width()).
// No buffer: the text is measured in pieces, so the font's widths must add
// up (ui/Fonts' are sums of advances).
int drawnWidth(const Font& f, const char* text);
// Whether `test` holds for a code point of `text` (UTF-8, NUL-terminated,
// as stored) as fit() draws it: composed and folded first, as
// drawnWidth() measures, so halfwidth "ｶﾀｶﾅ" is asked about as "カタカナ",
// NFD jamo as their syllable, "Ａ" as "A". No buffer; it stops at the first
// yes. What a caller asks of a text before it chooses a font
// (ui/Fonts::fromStore(): a glyph from the store, Now Playing's title).
bool drawnAny(const Font& f, const char* text, bool (*test)(void* ctx, uint32_t cp), void* ctx);

// Up to `maxLines` lines of at most `maxW` px, broken after spaces, and
// between two characters where one is Han, kana or CJK punctuation (the
// other may be Latin), but never before a closing mark (、。」』）” ’...), an
// ellipsis, 〜, a small kana, ー, 々 or an iteration mark, nor after an
// opening one (「『（“ ‘...): TextFit.cpp lists them.
// Hangul breaks at its spaces. A word with no such place that is wider than
// a line is cut where it overflows. The last line holds the rest,
// ellipsised. Line k goes to out + k * lineSize. Returns the lines written
// (0 for an empty text).
int wrap(const Font& f, const char* in, size_t inLen, int maxW, int maxLines, char* out, size_t lineSize);

// `path` cut from the left by whole folders to fit `maxW` px, the nearest
// kept: "…/Daft Punk/Discovery" (the Folders header's path, Now Playing's
// Go to folder). A path that fits stays whole; a cut one always starts
// with "…". If even "…/<last>" is too wide, "/<last>" (the draw cuts its
// end). Measured as drawn (drawnWidth()); the folders' bytes are kept as
// they are. Into `out` (`size` > 0).
void cutPathLeft(const Font& f, const char* path, int maxW, char* out, size_t size);

}  // namespace textfit
