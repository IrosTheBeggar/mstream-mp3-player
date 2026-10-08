// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"

// The device's tag reader (docs/METADATA.md part 5, 3.3.1; 6.1 row N6): one
// audio file's tags into the record of the card contract (2.6.4, 2.6.5), by
// the reading rules the transfer software follows too, so the two producers'
// records of one file are field-equal (2.17, item 3). The reference is
// mStream's default scanner, its rust-parser on lofty 0.25 in relaxed mode
// behind its ID3v2 repair pass (READ at mStream 926b97b1, lofty 0.25.1);
// part 5 says what is taken from it, and TagRules.h holds the small rules.
//
// What it reads:
//   MP3: ID3v2.2/2.3/2.4 at the head (a chain of tags merged frame by frame,
//        and a tag found in junk before the audio), ID3v1 and APEv2 at the
//        tail. The ID3v2 tag is the tag when there is one; else ID3v1, else
//        APE. ID3v1 fills a blank title, artist, album or genre and an absent
//        year or track number of the chosen tag (5.1).
//   FLAC: the Vorbis comments and PICTURE blocks; a front ID3v2 only when
//        the file has neither.
//   Opus: OpusTags (Vorbis comments across Ogg pages), with
//        METADATA_BLOCK_PICTURE.
// Text: ISO-8859-1, UTF-16 with a BOM, UTF-16BE, UTF-8; the repair pass's
// rules (5.2): invalid UTF-8 and an odd UTF-16 byte to U+FFFD, a v2.4 frame
// size that isn't syncsafe re-read when it lands on a frame boundary, v2.4
// unsynchronisation undone frame by frame. Then 2.3.6 (FieldBuilder): lists
// joined with U+001F, control characters to spaces, the limits.
//
// lofty's own rules are followed where they decide a value (each at its
// place in TagScan.cpp): an ID3v2 frame replaces an earlier one of the same
// id (lofty's frame list: a TXXX by its description, a picture never), an
// empty one doesn't within its tag, and a chained tag's does; TRCK's "N/M"
// must parse whole, else lofty's map reads it as the total; TDRC goes
// through lofty's timestamp parse; a v2.3 TYER with a month reads as no
// year; a v2.3 tag with an extended header reads as no frames, as lofty
// reads it; a Vorbis value that isn't UTF-8 is dropped, TRACKNUMBER's "N/M"
// sets both. The order of lofty's lists is modelled, not only their
// contents, since the first value of a field wins: a frame or a comment is
// located with its place in the list, and lofty's removals (a swap that
// reorders what is left: v2.3's date frames, the Vorbis number keys) move
// those places as lofty moves them. Vorbis comments take two passes: the
// first locates them and replays the list, the second decodes the values in
// the final order. Where lofty would fail the whole file (a bad text
// encoding, a UTF-16 string with no BOM), the reference record is
// UNREADABLE and the device reads the file itself (2.9); this reader is
// then lenient.
//
// Known differences from the reference, all rare (and listed in the test
// corpus's README): a compressed ID3v2 text frame is skipped (lofty inflates
// it; no zlib here), as a compressed or encrypted APIC is by the rules
// (5.3); the repair pass's fallback for a tag with no padding to grow into
// ('?' instead of U+FFFD) is not followed (5.2 says U+FFFD); COVERART (the
// deprecated Vorbis field) is never elected, as no picCoding can anchor it;
// an Opus picture's base64 is not checked past its head; an APE tag at the
// head of an MP3 is not read, nor a Lyrics3 block before ID3v1; a frame no
// field comes from is counted in its list but never compared, so taken to
// be never replaced (only where such a frame repeats in one tag does an
// entry move); a Vorbis value is checked as UTF-8 only in its first 4 KB.
//
// Bounded and fixed: every read goes through one caller buffer (a cursor:
// aligned refills, a short read after a seek), within a byte and a read
// budget per file; no heap, no recursion. Pictures are located, never read:
// the elected one's anchor (2.6.4: offset, stored length, type, MIME,
// coding) goes into the record. The Scanner (about 10 KB: the record, the
// ID3v2 frame or Vorbis item table, one value) belongs in PSRAM, never on
// the card worker's stack; a scan itself takes about 1 KB of stack.
namespace tagscan {

using cardcontract::Source;

// The MPTG header's parserVersion for the device's records (2.6.1): bump it
// when a change alters records, and the device reads its own records of an
// older version again (3.3.2, Rescan).
constexpr uint16_t kParserVersion = 1;

// The container, by the card's extension (as mStream picks lofty's reader):
// RECS's container codes (2.6.4).
enum class Kind : uint8_t { Unknown = 0, Mp3 = 1, Flac = 2, Opus = 3 };
Kind kindOf(const char* name);

// A single-valued field (2.3.6): the first non-empty value, control
// characters to spaces, cut to 255 bytes at a code-point boundary (then
// TRUNCATED). Later values are not the field's.
class SingleField {
public:
  static constexpr size_t kMax = cardcontract::FieldBuilder::kValueMax;
  SingleField() { clear(); }
  void clear();
  // One value's UTF-8 bytes. False: the field is set already, or the value
  // is empty (it doesn't set the field).
  bool add(const char* value, size_t n);
  const char* data() const { return buf_; }
  size_t size() const { return len_; }
  bool isSet() const { return set_; }
  bool truncated() const { return truncated_; }

private:
  char buf_[kMax + 1];
  uint16_t len_ = 0;
  bool set_ = false;
  bool truncated_ = false;
};

// One file's record: RECS's row and the run's fields, as the MPTG writer
// (mptg::RecordIn), the run's encoder (cardcontract::encodeRun) and the
// builder (cardcontract::RunFields, LibraryBuilder::viewOf) take them. The
// row's folder, name, strings, size, fatTime and qfp are the caller's.
struct Record {
  cardcontract::mptg::Record rec;
  SingleField title, album, titleSort, artistSort, albumSort, albumArtistSort, mbAlbumId, mbRecordingId;
  cardcontract::FieldBuilder artist, albumArtist, genre, composer;  // lists

  void clear();
  // Field f (cardcontract::Field order): its text, "" when absent.
  const char* field(uint32_t f) const;
  size_t fieldLength(uint32_t f) const;
  // Every field, for mptg::RecordIn::fields and encodeRun().
  void fields(const char* out[cardcontract::kRunFields]) const;
  // The fields as a parsed run.
  void toRunFields(cardcontract::RunFields* out) const;
  // A value or a list was cut (2.3.6).
  bool truncated() const;
};

enum class Result : uint8_t {
  Ok,          // read: the record holds what the file carries (NO_TAGS when nothing)
  Unreadable,  // not a file of its kind: the record is UNREADABLE (known 0)
  ReadError,   // the source failed a read: try again later (the record is UNREADABLE meanwhile)
};

struct Limits {
  uint32_t readBudget = 512 * 1024;  // bytes asked of the source per file
  uint16_t maxReads = 160;           // reads per file (an Opus picture of about 8 MB is about 128 pages)
  uint16_t seekRead = 1024;          // a read after a seek: this much, not a whole buffer
  uint32_t maxText = 4096;           // stored bytes read of one value; the rest skipped
  uint32_t maxJunk = 1024;           // lofty's max_junk_bytes: an ID3v2 tag looked for in junk before the audio
  uint32_t maxSync = 64 * 1024;      // bytes searched for the first MPEG frame
  uint16_t maxFrames = 1024;         // per ID3v2 tag
  uint8_t maxId3Tags = 8;            // chained at the head
  uint8_t maxFlacBlocks = 128;
  uint16_t maxComments = 4096;       // Vorbis comments or APE items
  uint16_t maxOggPages = 4096;       // for one OpusTags packet
};

struct Stats {
  uint32_t reads = 0;  // Source::read calls
  uint32_t bytes = 0;  // bytes asked
  uint32_t seeks = 0;  // reads not starting where the previous one ended
};

// What was met (Scanner::issues()): diagnostics, never a field.
enum Issue : uint32_t {
  kIssueBudget = 1u << 0,           // the byte or read budget ran out: the parse stopped
  kIssueReadError = 1u << 1,        // the source failed a read
  kIssueCompressedFrame = 1u << 2,  // an ID3v2 frame compressed (zlib): skipped
  kIssueEncryptedFrame = 1u << 3,   // an ID3v2 frame encrypted: skipped
  kIssueBadFrame = 1u << 4,         // an ID3v2 frame header or size that didn't fit
  kIssueTruncatedTag = 1u << 5,     // a tag or block running past the end of the file
  kIssueTooMany = 1u << 6,          // a loop cap or the frame table's size was hit
  kIssueLofty = 1u << 7,            // something lofty would refuse: the reference reads the file as UNREADABLE
  kIssueRepaired = 1u << 8,         // the repair pass's rules changed something (5.2)
  kIssueNonSyncsafe = 1u << 9,      // a v2.4 frame size re-read as a plain integer
  kIssueExtendedHeader = 1u << 10,  // an ID3v2 extended header (read as lofty reads it)
  kIssueSkippedPicture = 1u << 11,  // a picture no coding can anchor (COVERART) or a compressed or encrypted APIC
};

// Reads files one at a time, reusing its memory. About 10 KB: allocate it
// once per card-worker job, in PSRAM.
class Scanner {
public:
  // One file through `src`, whose bytes are a `kind` file. `buf` (at least
  // 512 bytes; 4 KB is the design's) holds the reads. The record is
  // complete whatever the result (UNREADABLE when not Ok).
  Result scan(Source& src, Kind kind, uint8_t* buf, uint32_t bufBytes, const Limits& limits = Limits());
  const Record& record() const { return rec_; }
  const Stats& stats() const { return stats_; }
  uint32_t issues() const { return issues_; }

  // ---- internals (TagScan.cpp) ----
  static constexpr uint32_t kMaxFrames = 96;
  static constexpr uint32_t kMaxItems = 96;
  static constexpr uint32_t kValueBytes = 260;  // one decoded value: 255 bytes and a code point to see the cut
  // One ID3v2 frame lofty's frame list holds that a field may come from,
  // located (its value is decoded at the end, in the list's order).
  struct FrameRef {
    uint32_t at;     // its content's first stored byte
    uint32_t end;    // the stored bytes' end (the frame's, or the tag's under tag-level unsynchronisation)
    uint32_t limit;  // logical bytes of content (under tag-level unsynchronisation; else end - at)
    uint32_t eq;     // lofty's frame equality within a key (an id, a TXXX's description, a timestamp)
    uint16_t seq;    // its place in the merged list (0 while its tag is read)
    uint16_t place;  // its place in its own tag's list, while that tag is read
    uint16_t year;   // a timestamp's
    uint8_t key;
    uint8_t tag;     // its tag in the chain
    uint8_t enc;     // text encoding; a picture's type
    uint8_t mode;    // how its content is stored (TagScan.cpp)
    uint8_t aux;     // a picture's MIME; a TXXX value's inherited BOM; a timestamp's state
    uint8_t live;
    uint8_t empty;
    uint8_t pad[3];
  };
  // One Vorbis comment lofty's item list holds that a field may come from
  // (or that moves the others: TRACKNUM), located.
  struct Item {
    uint32_t at;      // its value's first byte (in an Ogg file: its offset in the page body)
    uint32_t len;     // its value's bytes
    uint32_t number;  // a number lofty wrote itself (TRACKNUMBER's "N/M" read)
    uint32_t page;    // an Ogg file: the page the value starts in
    uint16_t place;   // its place in lofty's item list
    uint8_t key;      // what it fills (TagScan.cpp)
    uint8_t kind;     // what lofty's insert() and remove() match it by
    uint8_t flags;
    uint8_t pad[3];
  };
  struct Value {
    char buf[kValueBytes];
    uint16_t len;
    bool longer;  // the value went on past buf
  };

private:
  friend struct ScanCtx;
  Record rec_;
  Stats stats_;
  uint32_t issues_ = 0;
  union {
    FrameRef frames_[kMaxFrames];  // ID3v2 (a FLAC's front tag is read before its comments)
    Item items_[kMaxItems];        // Vorbis comments
  };
  uint32_t nFrames_ = 0;
  uint32_t nItems_ = 0;
  uint16_t seq_ = 0;
  uint8_t v1_[128];
  Value value_;  // the value being decoded
  Value spare_;  // one kept while another is decoded
};

}  // namespace tagscan
