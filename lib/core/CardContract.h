// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The card contract's shared conventions (docs/METADATA.md part 2, PROPOSED
// v1): what the transfer software (mstream-terminal, Rust) and the player
// keep on the SD card, byte for byte. This file holds the pieces every
// format uses: the bytes, CRC-32, FNV-1a 64 and the path hash, the quick
// fingerprint, the FAT time and the uniform-skew rule, part 5's number
// rule, the text rules (2.3.6) and the string run (2.6.5), names and the
// canonical order (2.6.8), the album folder (2.14.1) and
// /.player/device.txt (2.15). The container and the four binary formats
// are CardContainer.h, CardTags.h (MPTG), CardManifest.h (MSMF, MSPD) and
// CardAutoDj.h (MPDJ). Portable, no allocation; the vectors of 2.18 are
// test/fixtures/card/vectors.json, which test_card_contract reads.
namespace cardcontract {

// ---------------------------------------------------------------------------
// Bytes (2.3.1): little-endian integers; a fourcc is 4 ASCII bytes in reading
// order ("MPTG" is the bytes 4D 50 54 47, the u32 0x4754504D).
// ---------------------------------------------------------------------------
inline uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t get32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
inline uint64_t get64(const uint8_t* p) { return static_cast<uint64_t>(get32(p)) | static_cast<uint64_t>(get32(p + 4)) << 32; }
inline void put16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
inline void put64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
constexpr uint32_t fourcc(const char (&s)[5]) {
  return static_cast<uint32_t>(static_cast<uint8_t>(s[0])) | static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8 |
         static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24;
}

// ---------------------------------------------------------------------------
// CRC-32/ISO-HDLC (2.3.2), as zlib, PNG and Rust's crc32fast compute it: the
// reflected polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF.
// Chained like zlib's crc32(): crc32(b, crc32(a)) is the CRC of a then b, and
// crc32(nothing) is 0. Not the Ogg CRC of lib/core/OggPage, which is another.
// ---------------------------------------------------------------------------
uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);
// The CRC of a then b from the CRC of a, the CRC of b and b's length (zlib's
// crc32_combine): the MPTG walker reads STRS as two runs at once (CardTags.h).
uint32_t crc32Combine(uint32_t crcA, uint32_t crcB, uint64_t lenB);

// ---------------------------------------------------------------------------
// FNV-1a 64 (2.3.3): the path hash, qfp, artistKey and serverUrlKey. The
// device's thumbnails and the Opus open cache use the same function
// (thumbfile::pathHash), so the transfer's thumbnails key alike.
// ---------------------------------------------------------------------------
constexpr uint64_t kFnvBasis = 0xCBF29CE484222325ull;
constexpr uint64_t kFnvPrime = 0x00000100000001B3ull;
uint64_t fnv1a64(const void* data, size_t n, uint64_t h = kFnvBasis);
// The same over a NUL-terminated string. Named apart: an overload taking
// (const char*, uint64_t) would win over (const void*, size_t) for a call
// with a 64-bit size_t length, and lose to it on the 32-bit ESP32.
uint64_t fnv1a64Str(const char* s, uint64_t h = kFnvBasis);
// The literal "/music", always lowercase whatever case the card stores the
// folder in: the hash of the music root (75DC8A6A38687865), and the state a
// path's hash continues from.
constexpr uint64_t kMusicHash = 0x75DC8A6A38687865ull;
// The path hash of a path relative to /music ("Artist/Album/01 - x.mp3"; ""
// is /music itself): FNV-1a 64 of "/music", then "/" and the path, names
// spelled as the card's directory stores them (2.8.5), no trailing slash.
uint64_t pathHash(const char* rel, size_t len);
uint64_t pathHash(const char* rel);

// serverUrlKey (2.5.1): the FNV-1a 64 of the server's normalised base URL.
// The scheme and the host lowercased; no user name, password, query or
// fragment; an empty or default port (80 for http, 443 for https) dropped,
// any other written in decimal; the path kept, every trailing slash
// removed. "HTTP://Music.Example:3000/" is "http://music.example:3000",
// ED901EA3EE763AC7. normaliseServerUrl() returns the length written (`out`
// always NUL-terminated when cap > 0), 0 when the URL has no "scheme://host"
// or doesn't fit; serverUrlKey() is 0 then ("unknown", as the header says).
size_t normaliseServerUrl(const char* url, char* out, size_t cap);
uint64_t serverUrlKey(const char* url);

// ---------------------------------------------------------------------------
// The quick fingerprint (2.3.5): FNV-1a 64 of the size as a little-endian
// u64, the head [0, min(size, 4096)) and the tail [max(4096, size - 4096),
// size), empty when size <= 4096. Two reads, which never overlap.
// ---------------------------------------------------------------------------
constexpr uint32_t kQfpPart = 4096;
struct QfpRanges {
  uint32_t headBytes = 0;   // from offset 0
  uint32_t tailOffset = 0;
  uint32_t tailBytes = 0;
};
QfpRanges qfpRanges(uint32_t size);
// `head` holds headBytes, `tail` tailBytes (either may be null when 0).
uint64_t qfp(uint32_t size, const uint8_t* head, const uint8_t* tail);

// ---------------------------------------------------------------------------
// FAT timestamps (2.3.4): fatTime = (fdate << 16) | ftime, FatFs's FILINFO
// fields, local wall-clock time at 2 s. 0 is unknown. A stamp is invalid when
// its month is 0 or above 12, its day 0 or past its month's last day, its
// hour 24 or more, its minute 60 or more or its sec2 30 or more. A 0 or an
// invalid stamp never matches by time (qfp decides) and is left out of the
// skew histogram.
// ---------------------------------------------------------------------------
// The stamp of a wall-clock time (the seconds halved, rounded down); 0 when
// it can't be one (a year outside 1980-2107, or an invalid date or time).
uint32_t fatTime(int year, int month, int day, int hour, int minute, int second);
bool fatTimeValid(uint32_t t);  // non-zero and valid
// W: seconds since 1980-01-01 00:00 with no time zone, for a valid stamp
// (else -1). Always even.
int64_t fatWallSeconds(uint32_t t);
// Its inverse: the stamp of W (an odd W rounds down); 0 outside 1980-2107.
uint32_t fatTimeFromWall(int64_t w);

// The uniform-skew rule (2.3.4), over the files of one tags file whose sizes
// match: add() each (recorded, observed) pair; skew() is D, or 0 for no skew.
// D is the most frequent non-zero delta (ties: the smaller |D|, then the
// negative one), and only when at least 8 pairs have it, they are at least
// half of all the pairs, |D| <= 86,400 and D is a multiple of 900.
//
// In fixed memory (2 KB: kMaxDeltas slots, each a delta / 2, which fits an
// i32 over FAT's 128 years since a delta is even, and its count), and the
// same answer whatever order the pairs come in. While at most kMaxDeltas
// distinct deltas have come, the counts are exact. Past that the slots are a
// Misra-Gries summary: a new delta when every slot is taken takes one from
// each slot's count (a slot at 0 is freed) and isn't kept itself. A delta
// that more than 1/257 of the pairs have is then still in a slot, with a
// count low by at most what was taken; D needs half the pairs, so it is
// always there. Hence:
//   - needsRecount() false: skew() is 2.3.4's exactly;
//   - needsRecount() true and no second pass: skew() is D when its count, as
//     low as it may be, still passes the rule (a delta that has half the
//     pairs is the only one that can), else no skew. Never a wrong skew; a
//     card the summary can't settle falls to qfp;
//   - the second pass, beginRecount() then recount() with every pair's delta
//     again (the walk's pass over its Doubtful entries, 3.2.3), counts the
//     slots exactly, and skew() is then 2.3.4's exactly.
class SkewHistogram {
public:
  static constexpr uint32_t kMaxDeltas = 256;
  static constexpr uint32_t kMinPairs = 8;
  static constexpr int32_t kMaxSkew = 86400;
  static constexpr int32_t kSkewStep = 900;

  // False: the pair is left out (a 0 or an invalid stamp on either side).
  bool add(uint32_t recorded, uint32_t observed);
  int32_t skew() const;
  uint32_t pairs() const { return pairs_; }
  // More distinct deltas came than the slots hold, and no recount yet:
  // skew() may say no skew where 2.3.4 finds one (never another skew).
  bool needsRecount() const { return summary_ && !recounted_; }
  // The second pass: every slot's count back to 0, then each pair add()
  // took, as its delta in seconds (W(observed) - W(recorded): a Doubtful
  // entry's delta; a 0 delta may be passed or not).
  void beginRecount();
  void recount(int64_t delta);
  void clear() {
    n_ = pairs_ = 0;
    summary_ = recounted_ = false;
  }

private:
  int32_t half_[kMaxDeltas] = {};  // delta / 2
  uint32_t count_[kMaxDeltas] = {};
  uint32_t n_ = 0;
  uint32_t pairs_ = 0;
  bool summary_ = false;    // a delta came when every slot was taken
  bool recounted_ = false;  // beginRecount() since
};
// A file's time matches: both stamps valid and non-zero, and the delta is 0
// or the tags file's skew (when it has one: skew != 0).
bool timeMatches(uint32_t recorded, uint32_t observed, int32_t skew);

// ---------------------------------------------------------------------------
// The number rule (5.3), so the ESP32's f32, Rust's f64 and JS can't
// disagree: trim ASCII whitespace (space, tab, LF, FF, CR: Rust's
// is_ascii_whitespace, not VT); for a gain strip one trailing "dB" in any
// ASCII case and trim again; the rest must match
// [+-]?([0-9]+(\.[0-9]*)?|\.[0-9]+); exact decimal arithmetic at `scale`
// fraction digits, the next digit 5 or more rounding the magnitude up (half
// away from zero). `n` bytes of `s`, no NUL needed.
// ---------------------------------------------------------------------------
// bpm10 from a BPM tag: rounded to an integer, kept from 20 to 300, x 10.
bool bpm10FromText(const char* s, size_t n, uint16_t* out);
// A ReplayGain gain in hundredths of a dB; outside the i16 range: absent.
bool gainFromText(const char* s, size_t n, int16_t* out);
// A ReplayGain peak in ten-thousandths, saturating at 65,535; negative: absent.
bool peakFromText(const char* s, size_t n, uint16_t* out);
// An Opus R128 gain (an integer [+-]?[0-9]+ in the i16 range, a Q7.8 at
// -23 LUFS) as a ReplayGain gain at -18 LUFS: (q + 1280) x 25 / 64 in
// hundredths, rounded half away from zero (RG_FROM_R128, 2.6.4).
bool r128FromText(const char* s, size_t n, int16_t* out);
int16_t r128ToGain(int16_t q);
// ORIG's HASH_SAMPLED (2.6.6): mStream's hashes are sampled digests, exactly
// when hash-v is 2 or more and the file is 25 MiB (26,214,400 bytes) or more.
constexpr bool hashSampled(uint16_t hashV, uint64_t fileSize) { return hashV >= 2 && fileSize >= 26214400ull; }

// ---------------------------------------------------------------------------
// Text (2.3.6, 5.2). All text is UTF-8 with no NUL; tag text is stored as
// decoded, never normalised. Decoders write into a caller's buffer, cut at a
// code-point boundary when it is too small (DecodeResult::cut).
// ---------------------------------------------------------------------------
struct DecodeResult {
  size_t length = 0;  // bytes written, not counting the NUL (`out` is NUL-terminated when cap > 0)
  bool cut = false;   // `out` was too small
};
// ISO-8859-1: each byte is U+00xx (0x80-0x9F stay U+0080-U+009F, not
// cp1252), trailing NULs trimmed (lofty's latin1_decode).
DecodeResult latin1ToUtf8(const uint8_t* in, size_t n, char* out, size_t cap);
// UTF-8 as Rust's String::from_utf8_lossy reads it: each maximal invalid
// subpart becomes U+FFFD.
DecodeResult utf8Lossy(const uint8_t* in, size_t n, char* out, size_t cap);
// UTF-16 (`bigEndian` or not, no BOM: the caller reads it): an unpaired
// surrogate, and an odd length's stray last byte, become U+FFFD.
DecodeResult utf16ToUtf8(const uint8_t* in, size_t n, bool bigEndian, char* out, size_t cap);
// The longest prefix of s[0, len) of at most `max` bytes that ends on a
// code-point boundary (never before a continuation byte).
size_t utf8CutLength(const char* s, size_t len, size_t max);

// The unit separator between a list's values (U+001F, as mStream's
// genres_concat).
constexpr char kSeparator = '\x1F';

// One record field, built from its values by 2.3.6's steps in their order:
// control characters (U+0000-U+001F, U+007F) to spaces; empty values
// dropped; each value cut to 255 bytes at a code-point boundary (TRUNCATED);
// byte-identical repeats dropped, the first kept; then the list limits, at
// most 16 values and 1,023 bytes with the separators counted: the first
// value that would pass either ends the list, it and every value after it
// dropped (TRUNCATED). A single-valued field (title, album, the sort names,
// the MusicBrainz ids: 2.6.5) is the first value left after the empty ones
// are dropped; later values aren't the field's and never set TRUNCATED.
// About 1 KB: the producer's (TagScan's) record holds one per field.
class FieldBuilder {
public:
  static constexpr size_t kValueMax = 255;
  static constexpr size_t kListMax = 1023;
  static constexpr uint32_t kListValues = 16;

  explicit FieldBuilder(bool list = true) : list_(list) { buf_[0] = 0; }
  void add(const char* value, size_t n);
  void add(const char* value);
  void clear();
  const char* data() const { return buf_; }  // NUL-terminated
  size_t size() const { return len_; }
  uint32_t values() const { return values_; }
  bool truncated() const { return truncated_; }

private:
  char buf_[kListMax + 1];
  size_t len_ = 0;
  uint32_t values_ = 0;
  bool list_;
  bool ended_ = false;
  bool truncated_ = false;
};

// The string run (2.6.5): one byte n, then n NUL-terminated fields in this
// order. A field at an index >= n is absent; n is the last non-empty field's
// index plus one, and a record with no field has no run (strings = 0).
enum Field : uint8_t {
  kTitle = 0,
  kArtist,           // a list
  kAlbum,
  kAlbumArtist,      // a list
  kGenre,            // a list
  kComposer,         // a list
  kTitleSort,
  kArtistSort,
  kAlbumSort,
  kAlbumArtistSort,
  kMbAlbumId,
  kMbRecordingId,
  kRunFields         // 12 in readRules 1
};
constexpr bool isListField(uint32_t f) { return f == kArtist || f == kAlbumArtist || f == kGenre || f == kComposer; }
// The longest a field may be in a reader's hands: a list's 1,023 bytes, a
// single value's 255 (2.3.6; readers MUST accept these and MAY cut longer).
constexpr size_t fieldMax(uint32_t f) { return isListField(f) ? FieldBuilder::kListMax : FieldBuilder::kValueMax; }

// The run of `fields` (kRunFields NUL-terminated strings, nullptr or "" for
// absent; none may hold a NUL byte) into `out`: its length, 0 for no run
// (no field) or when it doesn't fit `cap`.
size_t encodeRun(const char* const fields[kRunFields], uint8_t* out, size_t cap);
// The largest run: n, each field at its limit and its NUL.
constexpr size_t kRunMax = 1 + 8 * (FieldBuilder::kValueMax + 1) + 4 * (FieldBuilder::kListMax + 1);

// A parsed run: each field a NUL-terminated copy in one fixed buffer, cut at
// fieldMax() (at a code-point boundary) should a writer have passed it.
// About 6 KB: a heap or PSRAM object, never the card worker's stack.
struct RunFields {
  char text[kRunMax];
  uint16_t offset[kRunFields] = {};
  uint16_t length[kRunFields] = {};
  uint8_t n = 0;         // fields present in the run (may exceed kRunFields)
  bool cut = false;      // a field was longer than fieldMax()
  RunFields() { clear(); }
  void clear();
  const char* get(uint32_t f) const { return f < kRunFields ? text + offset[f] : ""; }
  size_t len(uint32_t f) const { return f < kRunFields ? length[f] : 0; }
  bool has(uint32_t f) const { return len(f) > 0; }
  // Appends field `f` (the next in order); used by the parsers.
  void set(uint32_t f, const char* s, size_t n);
};
// Parses a whole run held in memory (from its n byte); false when a field
// has no NUL before `len`.
bool parseRun(const uint8_t* run, size_t len, RunFields* out, size_t* used = nullptr);

// ---------------------------------------------------------------------------
// Names, paths and the canonical order (2.6.8, 2.8).
// ---------------------------------------------------------------------------
// The longest path the device sees: "/music/" + the path is at most 255
// bytes of UTF-8 (2.8.2; the software keeps to it by 2.8.3, step 5). Every
// path a contract file names (folders, records, LIBR roots, PEND paths)
// keeps to it, or the file is absent (2.4.3).
constexpr size_t kMaxCardPath = 255;
constexpr size_t kMaxRelPath = kMaxCardPath - 7;  // "/music/" is 7 bytes
// A name: non-empty, no '/', not "." or ".." (no NUL: names are strings).
bool validName(const char* s, size_t n);
// A relative path: non-empty, no leading or trailing '/', every component
// a validName(), at most kMaxRelPath bytes.
bool validRelPath(const char* s, size_t n);
// Bytes order (memcmp, the shorter first on a common prefix). <0, 0, >0.
int compareNames(const char* a, size_t an, const char* b, size_t bn);
// The canonical order of two files' relative paths: a walk that, in each
// folder, lists its files by name and then descends into its subfolders by
// name. Not the byte order of the strings: "A/x.mp3" before "A B/y.mp3", and
// "A/z.mp3" before "A/B/y.mp3".
int compareFilePaths(const char* a, size_t an, const char* b, size_t bn);
int compareFilePaths(const char* a, const char* b);
// Folders in pre-order: an ancestor before its descendants, siblings by
// their names' bytes. "" (/music) first.
int compareFolderPaths(const char* a, size_t an, const char* b, size_t bn);
int compareFolderPaths(const char* a, const char* b);

// The album folder of a file (2.14.1, LibraryIndex's Album.folder): the
// folder at depth 2 below the file's root, the longest of `roots` (LIBR,
// relative to /music) that contains it, else /music; a file at depth 0 or 1
// has its own folder. Returns the album folder's length as a prefix of `rel`
// ("Artist/Album/CD1/01.flac" gives 12, "Artist/Album"; "x.mp3" gives 0,
// /music). `roots` may be null when nRoots is 0.
size_t albumFolderLength(const char* rel, size_t len, const char* const* roots, size_t nRoots);
// The transfer thumbnail's path (2.14.1) for an album folder's path hash:
// "<dir>/B/B1F7E69F.565". False: it didn't fit.
bool transferThumbPath(const char* dir, uint64_t folderHash, char* out, size_t cap);

// ---------------------------------------------------------------------------
// /.player/device.txt (2.15): ASCII, LF line ends, one key=value per line,
// unknown keys ignored. A file with a key missing reads that key as the
// no-file default; a key with an empty value says "none".
// ---------------------------------------------------------------------------
namespace devicetxt {

constexpr size_t kValueMax = 48;
struct Info {
  uint16_t contract = 1;
  char firmware[kValueMax] = "";
  // The majors the firmware reads, one bit per major (bit 0: major 1).
  uint32_t readMsmf = 1, readMptg = 1, readMpdj = 1, readMpth = 1;
  char codecs[kValueMax] = "mp3,flac";
  char extensions[kValueMax] = "mp3,flac";
  uint32_t maxRate = 48000;
  uint32_t maxChannels = 2;
};
// What a card with no device.txt means (firmware before this design):
// majors 1, mp3 and flac, 48 kHz, 2 channels.
Info defaults();
// This firmware's file (the example of 2.15, for `firmware`).
Info current(const char* firmware);
// The text, keys in 2.15's order; its length, 0 when it doesn't fit.
size_t format(const Info& info, char* out, size_t cap);
// Reads `n` bytes of a file (CRLF tolerated) over the defaults: each known
// key replaces its default, any other line is ignored (a longer value is
// cut at kValueMax - 1). False when the text has no "contract=" line, so
// isn't a device.txt at all (`out` is then the defaults).
bool parse(const char* text, size_t n, Info* out);
// `name` is one of a comma-separated list's items ("opus" in
// "mp3,flac,opus"), compared ignoring ASCII case.
bool listHas(const char* list, const char* name);
constexpr bool readsMajor(uint32_t mask, uint32_t major) { return major >= 1 && major <= 32 && (mask >> (major - 1)) & 1u; }

}  // namespace devicetxt

}  // namespace cardcontract
