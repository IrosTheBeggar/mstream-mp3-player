// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// docs/METADATA.md part 5's small rules, as pure functions: what TagScan
// (the device's tag reader) applies to a value once it is decoded. They pin
// the reference reader's behaviour, mStream's rust-parser on lofty 0.25 (READ
// at mStream 926b97b1 and lofty 0.25.1), where part 5 leaves it to lofty: the
// ID3v1 genre table, TCON's genre items, lofty's timestamps, Rust's integer
// parse, the year and number-pair rules, the Camelot table, the picture MIME.
// Text is UTF-8 (bytes and a length, no NUL needed). Portable, no allocation.
namespace tagrules {

// ---------------------------------------------------------------------------
// The ID3v1 genre table: lofty's 192 names (id3/v1/constants.rs, GENRES), not
// Winamp's spellings ("Classic rock", "Jazz & Funk"). An ID3v1 genre byte
// and a TCON "(n)" name through it.
// ---------------------------------------------------------------------------
constexpr uint32_t kGenreCount = 192;
const char* genreName(uint32_t index);  // nullptr past the table

// ---------------------------------------------------------------------------
// Rust's text helpers.
// ---------------------------------------------------------------------------
// str::parse::<u32>(): an optional '+', then ASCII digits only (no spaces,
// no '-'), within u32.
bool parseU32(const char* s, size_t n, uint32_t* out);
// str::parse::<usize>() for small values (TCON's "(n)"): the same rule.
inline bool parseIndex(const char* s, size_t n, uint32_t* out) { return parseU32(s, n, out); }
// str::trim(): Unicode White_Space off both ends (a byte that isn't valid
// UTF-8 is not whitespace). Moves `s` and shortens `n`.
void trim(const char*& s, size_t& n);
// Empty, or White_Space only (mStream's blank(): v.trim().is_empty()).
bool isBlank(const char* s, size_t n);

// ---------------------------------------------------------------------------
// ID3v2 TCON (lofty's GenresIter and parse_genre): a value splits on NUL;
// the segment after the last NUL (trailing NULs already trimmed) is also
// split on "(...)" groups: "(17)Rock" gives "Rock" (17) then "Rock",
// "(4)(17)" gives "Disco" then "Rock", and "((" keeps the bracket of a
// refinement. A NUL-ended segment is taken whole, so "(17)\0Pop" gives
// "(17)" then "Pop". Each item: longer than 3 bytes as is; else a number
// below 192 is the table's name, "RX" Remix, "CR" Cover, anything else as is.
// ---------------------------------------------------------------------------
// One item through parse_genre: the name to use (a table entry or `g`
// itself) and its length.
const char* parseGenre(const char* g, size_t n, size_t* outLen);
// The items of a TCON segment that isn't followed by a NUL, one at a time:
// `pos` starts at 0; false when there are none left. The item is a slice of
// `s` (before parseGenre()).
bool nextParenItem(const char* s, size_t n, size_t* pos, size_t* itemAt, size_t* itemLen);

// ---------------------------------------------------------------------------
// lofty's Timestamp (tag/items/timestamp.rs) in its relaxed mode, as ID3v2's
// TDRC (and v2.3's TYER, v2.2's TYE and TRD, which lofty reads as TDRC) is
// read: leading ASCII whitespace skipped, then at most 19 bytes; the year is
// 4 bytes, each a digit or a space, at least one digit; then month, day,
// hour, minute, second of 2 bytes each after a separator ("-." for the date,
// 'T', ':'), the separators optional when the text has none of "-.:". A
// frame whose timestamp doesn't parse is dropped (no year at all), and one
// that parses but fails verify() (month 13, a year past 9999) isn't
// converted, so has no year either.
// ---------------------------------------------------------------------------
struct Timestamp {
  uint16_t year = 0;
  uint8_t fields = 0;  // how many after the year are present (0-5)
  uint8_t month = 0, day = 0, hour = 0, minute = 0, second = 0;
  bool operator==(const Timestamp& o) const {
    return year == o.year && fields == o.fields && month == o.month && day == o.day && hour == o.hour &&
           minute == o.minute && second == o.second;
  }
};
enum class TsParse : uint8_t { Ok, None, Error };  // None: empty, or a year short of 4 bytes
TsParse parseTimestamp(const char* s, size_t n, Timestamp* out);
// lofty's Timestamp::verify(): year <= 9999, month <= 12, day <= 31, hour
// <= 23, minute and second <= 59.
bool verifyTimestamp(const Timestamp& t);

// ---------------------------------------------------------------------------
// The year (5.3; mStream's lofty22_year): after leading White_Space, the
// first four characters must be ASCII digits, and they are the year.
// ---------------------------------------------------------------------------
bool yearOf(const char* s, size_t n, uint16_t* out);
// The year of a lofty Timestamp as its item reads ("{:04}" then the rest):
// the year itself.
inline uint16_t yearOf(const Timestamp& t) { return t.year; }

// ---------------------------------------------------------------------------
// Numbers and totals (5.3).
// ---------------------------------------------------------------------------
// The parts a pair rule found (an explicit 0 is found, unlike none).
constexpr uint8_t kHaveNumber = 1;
constexpr uint8_t kHaveTotal = 2;
// ID3v2 TRCK and TPOS (lofty's split_pair): split once at the first NUL or
// '/'; the number part, trimmed, must parse (Rust u32), and the total part,
// when there is one, too; else the frame isn't split at all and lofty's map
// sends it whole to TrackTotal (DiscTotal), whose first NUL-separated part
// then parses (untrimmed) as the total. `s` is the frame's whole text;
// `longer`: the text goes on past `n` bytes (a part that reaches the end
// doesn't parse then: it is longer than any number).
uint8_t id3Pair(const char* s, size_t n, uint32_t* number, uint32_t* total, bool longer = false);
// mStream's parse_num_of(): split once at '/'; each part trimmed and parsed
// as a Rust u32.
uint8_t numOf(const char* s, size_t n, uint32_t* number, uint32_t* total);
// A record's u16 from a reader's u32: 0 stays none, above 65,535 is 65,535.
inline uint16_t clampNumber(uint32_t v) { return v > 65535 ? 65535 : static_cast<uint16_t>(v); }

// ---------------------------------------------------------------------------
// Compilation (5.3): "1" or "true" (any ASCII case) is 1, "0" or "false" 2,
// anything else 0. The value as it is (not trimmed).
// ---------------------------------------------------------------------------
uint8_t compilationOf(const char* s, size_t n);

// ---------------------------------------------------------------------------
// The Camelot key (5.3): trimmed, its first 12 characters, matched ignoring
// ASCII case against mStream's CAMELOT_TO_KEYS aliases (src/api/random.js).
// 1-12 are 1A-12A, 13-24 1B-12B; 0 none.
// ---------------------------------------------------------------------------
uint8_t camelotOf(const char* s, size_t n);

// ---------------------------------------------------------------------------
// Pictures. picMime (2.6.4) from a MIME string as lofty's MimeType::from_str
// reads it: "image/jpeg" or "image/jpg" (any case) 1, "image/png" 2, anything
// else (an empty one included) 3. From an APE item's first 8 bytes as
// lofty's mimetype_from_bin: PNG 2, JPEG 1, GIF, BMP or TIFF 3, else 0 (not
// a picture: lofty drops it).
// ---------------------------------------------------------------------------
uint8_t mimeOf(const char* s, size_t n);
uint8_t mimeOfMagic(const uint8_t magic[8]);
// The APE cover keys (lofty's APE_PICTURE_TYPES, matched ignoring ASCII
// case): the picture type (0-20), or -1.
int apePictureType(const char* key, size_t n);

}  // namespace tagrules
