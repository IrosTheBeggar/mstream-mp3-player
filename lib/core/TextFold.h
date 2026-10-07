// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// UTF-8 names folded for fonts that only have 7-bit ASCII (the Adafruit GFX
// fonts M5GFX ships), and the library's sort order. Portable, no allocation.
//
// Two modes:
//   - Punctuation: the typographic characters that tag editors and file
//     names are full of become their ASCII look-alikes: ’ ‘ ‚ ′ -> ',
//     “ ” „ ″ « » -> ", ‐ ‑ ‒ – — ― − -> -, … -> ..., NBSP and the other
//     spaces -> space, soft hyphens and zero-width characters dropped.
//     Letters are kept, so a name that still has non-ASCII afterwards
//     ("Pénélope") can be drawn with a Unicode font instead.
//   - Full: the same, plus Latin-1 and Latin Extended-A letters without their
//     accents (é -> e, Æ -> AE, ß -> ss, Ł -> L), a few symbols (© -> (c),
//     ½ -> 1/2), and '?' for anything else. The result is pure ASCII.
namespace textfold {

enum class Mode : uint8_t { Punctuation, Full };

// Decodes the code point at `s` and advances past it. Returns 0 at the end
// of the string; a malformed byte decodes as U+FFFD and advances one byte.
uint32_t decode(const char*& s);

// The ASCII replacement for `cp` in `mode` ("" drops it), or nullptr when
// there is none (ASCII itself, or a letter Punctuation mode keeps). In Full
// mode every non-ASCII code point has one ("?" when nothing better).
const char* replacement(uint32_t cp, Mode mode);

struct Result {
  size_t length = 0;       // bytes written, not counting the NUL
  bool ascii = true;       // the output is pure 7-bit ASCII
  bool changed = false;    // something was replaced or dropped
  bool truncated = false;  // `out` was too small (cut at a code point boundary)
  uint32_t unknown = 0;    // code points with no good replacement ('?' in Full mode)
};

// Folds `in` into `out` (always NUL-terminated when outSize > 0).
Result fold(const char* in, char* out, size_t outSize, Mode mode);
// Same for the first `inLen` bytes of `in`.
Result fold(const char* in, size_t inLen, char* out, size_t outSize, Mode mode);

// The library's order: letters case- and accent-insensitive (Full folding,
// lowercased), everything that isn't a letter or digit before the digits,
// the digits before the letters. Ties (names that fold alike) fall back to
// the raw bytes, so the order is total. <0, 0, >0 like strcmp.
int compare(const char* a, const char* b);

// The name as the Artists and Albums lists sort it: past one leading
// article ("The Lantern Choir" sorts as "Lantern Choir", under L), when a
// word follows it ("The" alone, "Theory" and "The-Dream" stay as they are).
// The articles are mStream's (its sort key, orderName): the, el, la, los,
// las, le, les; not "A" or "An", which start far more titles than names. A
// pointer into `s`: the name shown is still the whole name.
const char* sortName(const char* s);
// compare() on the sort names; names that sort alike ("The Pale Ferns" and
// "Pale Ferns") by their whole names, so the order is total. <0, 0, >0.
int compareSorted(const char* a, const char* b);

// Whether two names are the same artist as a folder and a file name write
// it: Full folding, lower case, letters and digits only (so "AC/DC",
// "AC_DC" and "ACDC" agree: a FAT name can't hold / : ? " * < > | \ and
// tools replace or drop them), and one leading "The " dropped on either
// side. A name with no letter or digit (empty, or a script Full folding
// can't spell) matches nothing. The slices lie inside NUL-terminated
// strings (a UTF-8 sequence a slice's end cuts ends it).
bool sameName(const char* a, size_t aLen, const char* b, size_t bLen);
// Whether `s` begins with `name` that way, the word ending there: "Artist
// feat. Guest" and "Artist & Band" begin with "Artist"; "Artistry" doesn't.
bool startsWithName(const char* s, size_t sLen, const char* name, size_t nameLen);

// The A-Z rail's key: the first character, folded and upper-cased, when it
// is a letter; '#' otherwise (digits, symbols, empty). The Artists and
// Albums lists take it of their sortName().
char railKey(const char* s);
// '#' -> 0, 'A'..'Z' -> 1..26 (27 buckets, in the order compare() sorts them).
int bucketOf(char key);
// The jump grid's second level ("Ka", "Ke"...): the second character,
// folded and lower-cased, when it is a letter; '#' otherwise (a digit, a
// symbol, a space, or no second character). Within one railKey() its
// bucket (bucketOf) never goes down in compare() order.
char secondKey(const char* s);

}  // namespace textfold
