// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"

// The AutoDJ neighbour table, autodj-<gen>.bin, MPDJ v1 (docs/METADATA.md
// 2.13): a top-K neighbour list per embedded recording on the card, built
// by the transfer software from mStream's embeddings and keyed by path
// hash. Rows (DJRW) are in canonical-hash order; each row's K neighbours
// (DJNB) are row indexes with a quantised cosine score; DJPH maps every
// card file with a row to it. On the device a pick reads one row of K
// neighbours (300 B at K = 100); the join after a build reads DJRW and DJPH
// (0.7 MB at 20k), checking their CRCs as it reads; DJNB's CRC (6 MB) is
// checked once per commit in the background (3.4.2), so Table::check()
// takes the sections to check. The device never computes the selection
// signature: it compares the root's COMP key with the header's bytes.
//
// The writer (the host tests, the future sync agent) computes everything
// from the rows' embeddings by 2.13.2's exact rule: the dot product in f64,
// in index order; the score clamp(round(cosine x 255), 0, 255) rounded half
// away from zero; quantised first, then the K best by (score descending,
// row ascending), the row's own and the same song's rows left out. The
// licence of real tables (2.13.4) keeps them out of the fixtures: those
// use small synthetic embeddings.
namespace cardcontract {
namespace mpdj {

constexpr uint32_t kHeaderBytes = 96;
constexpr uint32_t kDjrw = fourcc("DJRW");
constexpr uint32_t kDjnb = fourcc("DJNB");
constexpr uint32_t kDjph = fourcc("DJPH");
constexpr uint32_t kRowStride = 24;
constexpr uint32_t kPathStride = 12;
enum Sec : uint32_t { kSecRows = 0, kSecNeighbours, kSecPaths, kSecStrs, kSecCount };
extern const FormatSpec kSpec;

// K, the neighbours per row: 100 is 2.13.1's recommendation and part 7's
// proposed answer to U15 (about 6 MB per 20k tracks on the card), not yet
// the user's; a writer's default only, readers take any K from the header.
constexpr uint32_t kDefaultK = 100;
constexpr uint8_t kScoreCosine = 1;   // the only scoreKind v1 reads
constexpr uint8_t kRowBpmAnalysed = 1;  // DJRW flags
constexpr uint32_t kNoRow = 0xFFFFFFFFu;

struct Row {
  uint64_t pathHash = 0;        // its primary file's (the smallest of its files')
  uint8_t hashPrefix[8] = {};   // the canonical hash's first 8 bytes
  uint16_t bpm10 = 0;
  uint8_t camelot = 0;
  uint8_t flags = 0;
  uint32_t artistKey = 0;
};

struct Neighbour {
  uint32_t row = 0;
  uint8_t score = 0;
};

class Table {
public:
  static constexpr uint32_t kUseRows = 1;        // DJRW
  static constexpr uint32_t kUsePaths = 2;       // DJPH
  static constexpr uint32_t kUseNeighbours = 4;  // DJNB
  static constexpr uint32_t kUseAll = 7;

  // The frame and the header (2.4.3): indexBytes 2 or 4, and 2 exactly when
  // rowCount is 65,535 or less; k at least 1; scoreKind 1; DJRW and DJNB
  // rowCount rows, DJNB's stride k x (indexBytes + 1), DJPH pathCount rows.
  Why open(Source& src);
  // The CRCs and rules of the sections in `uses`, and STRS (the header's
  // strings, in 2.4.4's order), streamed through `scratch` (at least 64
  // bytes): DJRW in non-decreasing hashPrefix order; DJPH strictly
  // increasing by (pathHash, row), each row below rowCount; each DJNB index
  // below rowCount, or all ones (an unused slot).
  Why check(uint32_t uses, uint8_t* scratch, uint32_t scratchBytes) const;

  const Header& frame() const { return c_.header(); }
  uint32_t rowCount() const { return rowCount_; }
  uint32_t pathCount() const { return pathCount_; }
  uint32_t k() const { return k_; }
  uint32_t indexBytes() const { return indexBytes_; }
  uint32_t builtTime() const { return builtTime_; }
  uint32_t tagsGeneration() const { return tagsGeneration_; }
  const uint8_t* selectionSig() const { return sig_; }
  // The header's strings: 0 modelId, 1 modelVersion, 2 metric, 3 license,
  // 4 attribution.
  Why string(uint32_t which, char* out, size_t cap) const;

  bool row(uint32_t i, Row* out) const;
  // Row i's neighbours, best first, unused slots left out: their count.
  uint32_t neighbours(uint32_t i, Neighbour* out, uint32_t cap) const;
  // The row of a card file's path hash, or kNoRow (a binary search of DJPH).
  uint32_t rowOf(uint64_t pathHash) const;

private:
  Container c_;
  uint32_t rowCount_ = 0, pathCount_ = 0, k_ = 0, indexBytes_ = 0, builtTime_ = 0, tagsGeneration_ = 0;
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

// The selection signature (2.13.3): the first 16 bytes of the SHA-256 of
// "MPDJ-SEL 1\nmodel <id> <version>\nmetric <metric>\nk <k>\n" and one line
// per row's canonical hash (32 lowercase hex digits), ascending, no
// duplicates (the caller's: `hashes` sorted and unique), every line ended
// by LF.
void selectionSignature(const char* modelId, const char* modelVersion, const char* metric, uint32_t k,
                        const uint8_t (*hashes)[16], size_t n, uint8_t out[16]);

// A score: clamp(round(dot(a, b) x 255), 0, 255), the dot product's
// products and sums in f64 in index order, rounded half away from zero.
uint8_t score(const float* a, const float* b, uint32_t dim);

struct RowIn {
  uint8_t hash[16] = {};             // the canonical hash: audio-hash, else hash
  const uint64_t* paths = nullptr;   // the path hashes of its card files (at least one)
  uint32_t pathCount = 0;
  uint16_t bpm10 = 0;
  uint8_t camelot = 0;
  uint8_t flags = 0;
  uint32_t artistKey = 0;            // low 32 bits of FNV-1a 64 of nameKey(first artist value); 0 none
  const char* songKey = "";          // nameKey(artist display) + "|" + nameKey(title): equal keys are one song
  const float* embedding = nullptr;  // `dim` components
};

struct TableIn {
  uint32_t generation = 0;
  uint64_t cardId = 0;
  uint16_t minor = 0;
  uint32_t k = kDefaultK;
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

// Writes a table; `sig` receives its selection signature (the root's COMP
// key). False with `error` on bad input (a hash twice, a row with no path, a
// path twice in a row, k 0) or a failed sink.
bool write(Sink& out, const TableIn& in, uint8_t sig[16], uint32_t* fileBytes, uint32_t* headerCrc, const char** error);

}  // namespace mpdj
}  // namespace cardcontract
