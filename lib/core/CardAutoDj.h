// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"

// The AutoDJ neighbour index, autodj-<gen>.bin, MPDJ v2 (docs/AUTODJ-
// INDEX.md; the v1 draft of METADATA 2.13 never had a reader): one slot per
// embedded recording on the card, built by the transfer software from
// mStream's embeddings. Rows are in canonical-hash order (5.1). Each row's
// slot in DJSL (4.3) holds up to k neighbours as (u16 row, u8 level), best
// first, and a CRC seeded with the row, so one pick reads one slot (512 B
// at the default k = 169: one sector, since the alignment rule of 3.3 puts
// DJSL on a slot boundary) and checks it alone. DJRF is a filter record per
// row (BPM, key, artistKey: 4.4), DJPH maps the card's files to their rows
// (4.5). The device's join after a build reads DJRF, DJPH and STRS (18 B a
// track), checking their CRCs as it streams them; there is no whole-file
// check of DJSL, whose slots are checked as they are read (8.5): a slot
// whose CRC fails is Damaged (that slot alone is unusable, the file goes
// on), a slot whose CRC passes over a bad list is Broken (a broken writer:
// the file is absent from then on). The device never computes the
// selection signature: it compares the root's COMP key with the header's
// bytes (6, 7.2).
//
// The writer (the host tests, a future sync agent) computes every list from
// the rows' embeddings by 5's rule: the dot product in f64 in index order,
// the level clamp(round(cos x 255), 0, 255) rounded half away from zero;
// the candidates (every other row but the same song's) by (level desc, row
// asc); then the selection the header names: the top k (selKind 0), or the
// exact top selHead and an evenly spaced sample of the rest down to level
// selFloor (selKind 1, the default "S84-55-169"). The licence of real
// indexes (12.5) keeps them out of the fixtures: those use synthetic
// embeddings.
namespace cardcontract {
namespace mpdj {

constexpr uint16_t kMajor2 = kMajorMpdj;   // the major this reader reads (2)
constexpr uint32_t kMinHeaderBytes = 104;  // minor 0's header (4.1)
constexpr uint32_t kDjsl = fourcc("DJSL");
constexpr uint32_t kDjrf = fourcc("DJRF");
constexpr uint32_t kDjph = fourcc("DJPH");
constexpr uint32_t kFilterStride = 8;
constexpr uint32_t kPathStride = 10;
enum Sec : uint32_t { kSecSlots = 0, kSecFilters, kSecPaths, kSecStrs, kSecCount };
extern const FormatSpec kSpec;

// k: 2-169 neighbours a row. 2-84 fill a 256-byte slot, 85-169 a 512-byte
// one: 3 bytes an entry and the CRC's 4.
constexpr uint32_t kMinK = 2;
constexpr uint32_t kMaxK = 169;
constexpr uint32_t kMaxSmallK = 84;
constexpr uint32_t kSmallSlot = 256;
constexpr uint32_t kLargeSlot = 512;
constexpr uint16_t kUnused = 0xFFFF;  // an unused entry's index (its level 0)
constexpr uint8_t kIndexBytes = 2;
constexpr uint8_t kScoreCosine = 1;   // the only scoreKind v2 reads (5.2's level)
constexpr uint8_t kSelTop = 0;        // the top k
constexpr uint8_t kSelStrided = 1;    // the top selHead, then strided by rank down to selFloor
// The writer's defaults (the user's decision, 2026-10-13: "S84-55-169"):
// the exact 84 nearest, then 85 at evenly spaced ranks down to the last
// candidate at level 140 or more (cos >= 0.547).
constexpr uint32_t kDefaultK = 169;
constexpr uint32_t kDefaultHead = 84;
constexpr uint32_t kDefaultFloor = 140;
constexpr uint8_t kFilterBpmAnalysed = 1;  // DJRF flags bit 0 (the MPTG record's flags bit 8)
constexpr uint32_t kMaxRows = 65535;       // u16 indexes, 0xFFFF the unused mark
constexpr uint32_t kNoRow = 0xFFFFFFFFu;
constexpr uint32_t kMaxString = 255;       // a header string's bytes (a writer's rule)

// The slot of k (4.3): 256 for k 2-84, 512 for 85-169; 0 for any other k.
uint32_t slotBytesFor(uint32_t k);
// The alignment rule (3.3): the smallest multiple of 8, at least `minimum`
// (the header's size: 104 in minor 0), that ends the directory of
// `sectionCount` entries on a slot boundary, so DJSL, the first section,
// starts on one. 0 for a slotBytes that isn't a non-zero multiple of 8.
uint32_t headerBytesFor(uint32_t sectionCount, uint32_t slotBytes, uint32_t minimum = kMinHeaderBytes);
// Entry j's rank in a strided list (5.4) of k entries, head H, when m > k
// candidates reach the floor: j below H, else H + (j - H)(m - 1 - H) /
// (k - 1 - H), rounded down. r_H = H and r_(k-1) = m - 1.
uint32_t strideRank(uint32_t j, uint32_t k, uint32_t head, uint32_t m);

// The level (5.2) of a cosine: clamp(round(cos x 255), 0, 255), one f64
// multiply, rounded half away from zero (C's round()); a negative value 0.
uint8_t level(double cos);
// The level of two rows: their cosine the sum, in index order, of the
// products of their f32 components, each product and partial sum in f64.
uint8_t level(const float* a, const float* b, uint32_t dim);

struct Entry {
  uint16_t row = 0;
  uint8_t level = 0;
};

// One slot (4.3, 8.4): Ok; Damaged when its CRC fails (S1: only this slot
// is unusable; a reader may read it again); Broken when its CRC passes but
// its list breaks a rule (S2-S4: a broken writer, the file is absent).
enum class SlotRead : uint8_t { Ok, Damaged, Broken };
// The CRC of row `row`'s slot: CRC-32 over the row as u32 LE, then the
// slot's first slotBytes - 4 bytes (a slot read for the wrong row fails).
uint32_t slotCrc(const uint8_t* slot, uint32_t slotBytes, uint32_t row);
// Checks a slot of row `row`, in 8.4's order, each its own pass: S1 its
// CRC (djSlotCrc); S2 no used entry after an unused one (djSlotUnused); S3
// every used index below rowCount and not the row (djSlotIndex); S4 levels
// never rise, and equal levels have rising indexes (djSlotOrder). Unused
// entries' level bytes and the padding aren't checked (the CRC covers
// them). On Ok, `out` (k of room, or nullptr) gets the used entries and
// `count` their number; `why` (or nullptr) gets the reason.
SlotRead checkSlot(const uint8_t* slot, uint32_t slotBytes, uint32_t k, uint32_t row, uint32_t rowCount, Entry* out,
                   uint32_t* count, Why* why = nullptr);

// A row's filter record (4.4): its primary file's BPM and key, and the
// artist cooldown's key.
struct Filter {
  uint16_t bpm10 = 0;      // 0 none
  uint8_t camelot = 0;     // 0 none, 1-12 = 1A-12A, 13-24 = 1B-12B (above 24 reads as 0)
  uint8_t flags = 0;       // kFilterBpmAnalysed (other bits are ignored)
  uint32_t artistKey = 0;  // 0 none (namekey::artistKey)
};

class Table {
public:
  static constexpr uint32_t kUseFilters = 1;  // DJRF
  static constexpr uint32_t kUsePaths = 2;    // DJPH
  static constexpr uint32_t kUseSlots = 4;    // DJSL
  static constexpr uint32_t kUseAll = 7;
  // The device: the join's sections; its slots are checked as it reads them.
  static constexpr uint32_t kUseDevice = kUseFilters | kUsePaths;

  // The frame (8.1: major 2, DJSL, DJRF, DJPH and STRS present, their
  // strides at least v2's) and the header (8.2): rowCount 1-65,535, k 2-169,
  // slotBytes k's, indexBytes 2, scoreKind 1 (Enum); the selection: kind 0
  // with selHead k and selFloor 0, kind 1 with selHead 1 to k - 2 and
  // selFloor 1-255, a later kind with selHead at most k (DjSelection);
  // DJSL and DJRF rowCount rows, DJPH pathCount (Counts); DJSL's stride
  // slotBytes (Shape); DJSL the first directory entry, on a slot boundary,
  // and the four in the order DJSL, DJRF, DJPH, STRS (DjLayout). Reads only
  // the header and the directory (the types twice).
  Why open(Source& src);
  // The sections in `uses`, in 8.6's order, streamed through `scratch`: STRS
  // always (byte 0 NUL, the header's strings inside it, NUL-terminated and
  // in header order, then its CRC); DJRF's CRC; DJPH's rows below rowCount
  // and hashes strictly increasing, then its CRC; every slot in row order
  // (checkSlot), then DJSL's CRC. The first failure is the reason. At least
  // 64 bytes of scratch, 1,024 with kUseSlots (a slot and the stream's
  // buffer).
  Why check(uint32_t uses, uint8_t* scratch, uint32_t scratchBytes) const;

  const Header& frame() const { return c_.header(); }
  uint32_t rowCount() const { return rowCount_; }
  uint32_t pathCount() const { return pathCount_; }
  uint32_t k() const { return k_; }
  uint32_t slotBytes() const { return slotBytes_; }
  uint8_t selKind() const { return selKind_; }
  // The first min(selHead, c) entries of every list are the exact top of
  // its candidates, whatever the kind.
  uint32_t selHead() const { return selHead_; }
  uint32_t selFloor() const { return selFloor_; }
  uint32_t builtTime() const { return builtTime_; }
  uint32_t tagsGeneration() const { return tagsGeneration_; }
  const uint8_t* selectionSig() const { return sig_; }
  uint32_t slotsOffset() const { return c_.section(kSecSlots).offset; }
  // The header's strings: 0 modelId, 1 modelVersion, 2 metric, 3 license,
  // 4 attribution.
  Why string(uint32_t which, char* out, size_t cap) const;

  // Row r's filter record (the first 8 bytes of a longer one).
  bool filter(uint32_t r, Filter* out) const;
  // The row of a card file's path hash, or kNoRow (a binary search of DJPH).
  uint32_t rowOf(uint64_t pathHash) const;
  // Row r's slot, as a pick reads it: one read of slotBytes() bytes into
  // `buf`, then checkSlot(). A failed read is Damaged (Why::Io: the card's,
  // worth one more read); a row at or past rowCount() is Broken
  // (Why::DjSlotIndex: no checked row names it, so the caller's state is
  // wrong).
  SlotRead slot(uint32_t r, uint8_t* buf, Entry* out, uint32_t* count, Why* why = nullptr) const;

private:
  Container c_;
  uint32_t rowCount_ = 0, pathCount_ = 0, k_ = 0, slotBytes_ = 0, selHead_ = 0, selFloor_ = 0;
  uint32_t builtTime_ = 0, tagsGeneration_ = 0;
  uint8_t selKind_ = 0;
  uint32_t strings_[5] = {};
  uint8_t sig_[16] = {};
};

// SHA-256, for the selection signature.
class Sha256 {
public:
  Sha256();
  void update(const void* data, size_t n);
  void final(uint8_t out[32]);

private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  uint64_t bytes_ = 0;
  uint32_t fill_ = 0;
};

// The SHA-256 of every row's dim f32 values, little-endian, in row order
// (the signature's `vec` line). `rows[r]` is row r's embedding.
void vecDigest(const float* const* rows, uint32_t rowCount, uint32_t dim, uint8_t out[32]);

// What the selection signature (6) covers. `hashes` are the rows' canonical
// hashes in row order; the same-song groups (two or more rows sharing a
// song key) are `groupRows`, each group's members ascending, the groups by
// their first member, group g ending before groupRows[groupEnds[g]].
struct SigIn {
  const char* modelId = "";
  const char* modelVersion = "";
  const char* metric = "cosine";
  uint32_t k = 0;
  uint8_t selKind = kSelStrided;
  uint32_t selHead = 0;
  uint32_t selFloor = 0;
  uint32_t dim = 0;
  uint8_t vecSha256[32] = {};
  const uint8_t (*hashes)[16] = nullptr;
  uint32_t rowCount = 0;
  const uint32_t* groupRows = nullptr;
  const uint32_t* groupEnds = nullptr;
  uint32_t groupCount = 0;
};
// The first 16 bytes of the SHA-256 of "MPDJ-SEL 2", "model <id>
// <version>", "metric <metric>", "k <k>", "sel top" or "sel strided <H>
// <Lf>", "vec <dim> <64 hex>", a line per row's hash (32 lowercase hex), a
// "same <i> <j> ..." line per group, every line ended by LF.
void selectionSignature(const SigIn& in, uint8_t out[16]);

// One row's selection (5.3, 5.4): `levels[b]` its level to row b of the n,
// or kNotCandidate for itself and its song's rows. The candidates in rank
// order (level desc, row asc: one counting sort) into `order` (n words of
// scratch), then the entries into `out` (k of room) by the kind (kSelTop:
// the top k; else strided when more than k candidates reach `floor`, the
// top k when not). Returns c, the entries used.
constexpr uint16_t kNotCandidate = 256;
uint32_t selectEntries(const uint16_t* levels, uint32_t n, uint8_t kind, uint32_t k, uint32_t head, uint32_t floor,
                       Entry* out, uint16_t* order);

// A row of the writer's input (5.1, 5.3): its record values are its
// primary file's (the card file with the smallest path hash; two of one
// hash, the smaller path by its bytes), from that file's MPTG record.
struct RowIn {
  uint8_t hash[16] = {};             // the canonical hash: audio-hash, else hash
  const uint64_t* paths = nullptr;   // the path hashes of its card files (at least one)
  uint32_t pathCount = 0;
  uint16_t bpm10 = 0;
  uint8_t camelot = 0;               // above 24 is written as 0
  uint8_t flags = 0;                 // kFilterBpmAnalysed
  uint32_t artistKey = 0;            // namekey::artistKey of the record's artist list
  const char* songKey = nullptr;     // nameKey(artist display) + "|" + nameKey(title); nullptr when the title is blank
  const float* embedding = nullptr;  // `dim` components
};

struct TableIn {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  uint32_t k = kDefaultK;
  uint8_t selKind = kSelStrided;
  uint32_t selHead = kDefaultHead;    // kSelTop: ignored, written as k
  uint32_t selFloor = kDefaultFloor;  // kSelTop: ignored, written as 0
  uint32_t builtTime = 0;
  uint32_t tagsGeneration = 0;
  const char* modelId = "";
  const char* modelVersion = "";
  const char* metric = "cosine";
  const char* license = "";
  const char* attribution = "";
  uint32_t dim = 0;
  const RowIn* rows = nullptr;  // any order (sorted by canonical hash here)
  uint32_t rowCount = 0;
};

// Writes an index; `sig` receives its selection signature (the root's COMP
// key). DJPH leaves out every path hash two or more card files hold (in one
// row or across rows: 4.5); a row left without one stays in the table. False
// with `error` on bad input (rowCount outside 1-65,535, a k or selection
// out of range, a hash twice, a row with no path or no embedding, a value
// that isn't finite, a string over 255 bytes) or a failed sink. Every
// pair's level is computed: n^2 x dim multiplies, a host's work.
bool write(Sink& out, const TableIn& in, uint8_t sig[16], uint32_t* fileBytes, uint32_t* headerCrc, const char** error);

}  // namespace mpdj
}  // namespace cardcontract
