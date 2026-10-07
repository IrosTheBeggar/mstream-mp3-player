// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// mStream's name helpers (src/db/name-key.js), as docs/METADATA.md 5.4
// pins them for the card contract: the builder's votes and the artist
// display join, and later AutoDJ's keys (DJRW's artistKey and the same-song
// rule, 2.13.2), which one side writes and the other compares. Portable, no
// allocation, little stack (no buffer of the whole string: the Final_Sigma
// lookahead re-reads the input).
//
// nameKey(s): whitespace runs collapsed to one space and trimmed; the
// Unicode quotes (U+2018 U+2019 U+201A U+201B U+2032 to ', U+201C U+201D
// U+201E U+201F U+2033 to ") and dashes (U+2010-U+2015, U+2212 to -) folded;
// then lowercased. No accent folding. Whitespace is the Unicode White_Space
// property (Rust's char::is_whitespace, not JS's \s: U+FEFF is not
// whitespace), and the lowercase is the default full mapping with
// Final_Sigma (Rust's str::to_lowercase): "ΣΟΦΙΑΣ" gives "σοφιας", and
// U+0130 gives "i" + U+0307. The contract follows the Rust side where
// mStream's two engines differ. The tables are Rust's own
// (NameKeyTables.cpp, made by tools/unicode_case.rs).
//
// The text is UTF-8; a byte that isn't part of a valid sequence reads as
// U+FFFD (records are valid UTF-8: their writers decode lossily, 5.2).
namespace namekey {

namespace tables {
struct Range {
  uint32_t first, last;
};
struct LowerRun {  // first + k x step (k < count) lowercases to itself + delta
  uint32_t first;
  uint16_t count;
  uint8_t step;
  int32_t delta;
};
struct LowerSpecial {  // a code point whose lowercase is several (U+0130)
  uint32_t cp;
  uint8_t n;
  uint32_t to[3];
};
struct ClassRange {  // Final_Sigma's context: lastAndClass = last | class << 24
  uint32_t first;
  uint32_t lastAndClass;
};
extern const uint16_t kUnicodeVersion[3];
extern const Range kWhiteSpace[];
extern const uint32_t kWhiteSpaceCount;
extern const LowerRun kLower[];
extern const uint32_t kLowerCount;
extern const LowerSpecial kLowerSpecial[];
extern const uint32_t kLowerSpecialCount;
extern const ClassRange kSigmaContext[];
extern const uint32_t kSigmaContextCount;
// The generator's digest of every code point's lowercase, White_Space bit
// and class as Rust's std gives them (test_name_key recomputes it).
extern const uint64_t kRustDigest;
}  // namespace tables

// Unicode White_Space.
bool isWhiteSpace(uint32_t cp);
// The default lowercase mapping of one code point, without context (so
// U+03A3 gives U+03C3): up to 3 code points into `out`; how many.
uint32_t toLower(uint32_t cp, uint32_t out[3]);
// Final_Sigma's context classes: a cased letter, a case-ignorable character
// (skipped when looking for one), or neither.
enum SigmaClass : uint8_t { kOther = 0, kCased = 1, kIgnorable = 2 };
uint8_t sigmaClass(uint32_t cp);
// The quote and dash fold alone: the code point nameKey() keeps for `cp`.
uint32_t foldPunctuation(uint32_t cp);

// The key of s[0, n) into `out`: the key's whole length in bytes, of which
// the first cap - 1 at most are written, cut at a code-point boundary
// (`out` NUL-terminated when cap > 0). A key is at most 3/2 of its input
// plus 3 bytes (U+0130's two code points take 3 bytes for its 2).
size_t nameKey(const char* s, size_t n, char* out, size_t cap);
// The FNV-1a 64 of the key's bytes, without a buffer (continuing `h`).
uint64_t nameKeyHash(const char* s, size_t n, uint64_t h = 0xCBF29CE484222325ull);

// orderName (5.4): the nameKey of the sort tag when it has one (a key that
// isn't empty), else of the name, minus one leading article ("the", "el",
// "la", "los", "las", "le", "les") followed by a space. Same contract as
// nameKey() for `out`.
size_t orderName(const char* name, size_t n, const char* sortTag, size_t sortLen, char* out, size_t cap);

// s[0, n) without White_Space at either end: where it starts, its length in
// *len.
const char* trim(const char* s, size_t n, size_t* len);

// The artist display (mStream's credit_display, 5.4) of a list field (its
// values separated by U+001F): each value trimmed, the empty ones dropped;
// one value as it is; several deduplicated by nameKey (the first kept, by
// the keys' 64-bit hashes) and joined with ", ". Its whole length, at most
// cap - 1 bytes written (cut at a code-point boundary, `out`
// NUL-terminated). A list of 2.3.6's limits (1,023 bytes, 16 values) fits
// kDisplayMax bytes.
constexpr size_t kDisplayMax = 1023 + 15;
size_t displayJoin(const char* list, size_t n, char* out, size_t cap);

// DJRW's artistKey (2.13.2): the low 32 bits of the FNV-1a 64 of the
// nameKey of a list's first value; 0 when it has none.
uint32_t artistKey(const char* list, size_t n);

}  // namespace namekey
