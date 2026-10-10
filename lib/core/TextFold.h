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
//   - Full: the same, plus the Latin letters without their accents (Latin-1,
//     Extended-A, Extended-B and Extended Additional: é -> e, Æ -> AE,
//     ß -> ss, Ł -> L, ș -> s, ỹ -> y, ǆ -> dz), fullwidth ASCII as ASCII
//     (Ａ -> A), combining marks and variation selectors dropped, a few
//     symbols (© -> (c), ½ -> 1/2), and '?' for anything else. The result
//     is pure ASCII.
// In both, U+0080-009F (Latin-1 tags keep a cp1252 byte so: docs/METADATA.md
// 5.2) fold as their Windows-1252 characters (U+0092 as ’).
//
// Composer gives a text's code points composed as NFC composes them, for
// drawing and sorting only (docs/I18N.md, phase 0).
namespace textfold {

enum class Mode : uint8_t { Punctuation, Full };

// Decodes the code point at `s` and advances past it. Returns 0 at the end
// of the string; a malformed byte decodes as U+FFFD and advances one byte.
uint32_t decode(const char*& s);
// The same within [s, end) (end nullptr: to the NUL): 0 at `end`; a
// sequence `end` cuts decodes as U+FFFD and advances one byte.
uint32_t decode(const char*& s, const char* end);
// `cp` as UTF-8 into out: its length (1-4).
size_t encode(uint32_t cp, char out[4]);

// The ASCII replacement for `cp` in `mode` ("" drops it), or nullptr when
// there is none (ASCII itself, or a letter Punctuation mode keeps). In Full
// mode every non-ASCII code point has one ("?" when nothing better).
const char* replacement(uint32_t cp, Mode mode);

// U+0080-009F as Windows-1252's characters (U+0092 -> U+2019 ’, U+008A ->
// U+0160 Š): docs/METADATA.md 5.2 lets a device draw them so, the record
// keeping them. `cp` itself for every other code point, and for the five
// that cp1252 leaves undefined (U+0081 U+008D U+008F U+0090 U+009D).
uint32_t fromC1(uint32_t cp);

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

// ---- Canonical composition: for drawing and sorting only ----
//
// NFD text (macOS writes file names so; some tag editors decompose) draws
// and sorts as its NFC form: the code points come out with each combining
// mark composed into the letter before it where NFC would, by NFC's own
// pairs for Latin, Greek, Cyrillic and kana (tables::kPairs) and Hangul's
// jamo by arithmetic, the marks left over after it in canonical order. Never
// applied to what is stored: records keep their bytes, and the index's
// track names (files are opened by them) and the path hashes are compared
// byte for byte (docs/METADATA.md 2.3.6, 2.4; CardContract.h).
//
// The output is NFC's for NFD text and for NFC text in those scripts
// (test_text_fold checks random strings against Python's unicodedata). Not
// done: the singletons NFC maps alone (Greek oxia letters to their tonos
// twins, U+037E to ';'), and a mark after a precomposed letter that NFC
// would sort before the letter's own marks (ǖ + U+0323): both are left as
// they are. A run of more than 16 marks is not composed.
class Composer {
public:
  // [s, end); end nullptr: up to the NUL.
  explicit Composer(const char* s, const char* end = nullptr) : s_(s), end_(end) {}
  // The next code point, 0 at the end.
  uint32_t next();

private:
  uint32_t leftover();

  const char* s_;
  const char* end_;
  // The marks after the last letter that it didn't take, still to come
  // out: `count_` marks from `run_`, a bit each in `taken_` for the ones it
  // took; `level_` the class coming out, `at_` the next mark to look at.
  const char* run_ = nullptr;
  const char* runEnd_ = nullptr;
  uint16_t taken_ = 0;
  uint8_t count_ = 0;
  uint8_t level_ = 0;
  uint8_t at_ = 0;
};

// Canonical_Combining_Class of the marks the composer knows (U+0300-036F,
// U+0483-0487, U+3099-309A); 0 for every other code point.
uint8_t combiningClass(uint32_t cp);
// NFC's primary composite of `first` + `second` (Latin, Greek, Cyrillic,
// kana, and Hangul's LV and LVT), 0 when there is none.
uint32_t composePair(uint32_t first, uint32_t second);

// ---- The library's order ----

// The library's order: letters case- and accent-insensitive (Full folding,
// lowercased), everything that isn't a letter or digit before the digits,
// the digits before the letters. Composed first (Composer): NFD and NFC
// sort alike. Ties (names that fold alike) fall back to the raw bytes, so
// the order is total. <0, 0, >0 like strcmp.
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
// it: Full folding (composed first), lower case, letters and digits only
// (so "AC/DC", "AC_DC" and "ACDC" agree: a FAT name can't hold / : ? " * <
// > | \ and tools replace or drop them), and one leading "The " dropped on
// either side. A name with no letter or digit (empty, or a script Full
// folding can't spell) matches nothing. The slices lie inside
// NUL-terminated strings (a UTF-8 sequence a slice's end cuts ends it).
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

// The generated tables (TextFoldTables.cpp, tools/gen_text_tables.py, from
// Python's unicodedata).
namespace tables {
// NFC's composition pairs: first << 18 | the second's index in kPairMarks
// << 13 | the composite's low 13 bits, sorted; the composite is those
// bits | (first & 0x2000).
extern const uint32_t kPairs[];
extern const uint32_t kPairCount;
extern const uint16_t kPairMarks[];
extern const uint32_t kPairMarkCount;
// Canonical_Combining_Class of U+0300..U+036F.
extern const uint8_t kCombiningClass[0x70];
// U+0180..U+024F and U+1E00..U+1EFF: the base letter, '\0' for none or for
// more than one (then kFolds).
extern const char kLatinExtB[0xD0 + 1];
extern const char kLatinExtAdditional[0x100 + 1];
struct Fold {
  uint16_t cp;
  char to[3];
};
// Two-letter folds of the two blocks, and the IPA letters that are a
// folded Extended-B letter's other case; by code point.
extern const Fold kFolds[];
extern const uint32_t kFoldCount;
// U+0080..U+009F as Windows-1252 (0: undefined there).
extern const uint16_t kCp1252[32];
// The generator's digest of what the tables say (test_text_fold
// recomputes it through composePair(), combiningClass(), replacement() and
// fromC1()).
extern const uint64_t kDigest;
}  // namespace tables

}  // namespace textfold
