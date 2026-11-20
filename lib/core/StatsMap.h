// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"
#include "PathHashWalk.h"
#include "StatsRank.h"

// The listening stats' map (docs/LISTENING-STATS.md 6; S2's C13): the
// library's tracks walked once (pathhash::Walk, the hashing Auto DJ's join
// takes them by), each track's hash looked up
//   - in the played set (statsrank::Played: every hash with a play): its
//     album's bit set (Not played yet: the albums whose bit stays clear);
//   - in the wanted set (the hashes the lists hold): its track id kept
//     (u32: no 65,534 cap, unlike Auto DJ's u16 maps). A hash two tracks
//     of this library share (a 64-bit collision) maps neither, as the join
//     does (6.6); a hash no track has (an orphan: a rename, a file gone) has
//     no id and is left out of a shelf.
// Run on the loop in slices (stepPass(): a pass's worth, as the join's),
// never in a pass the join works in (the firmware's gate). It aborts when
// the index isn't the one it began on (matches(): the pointer, ready(),
// trackCount(), buildStamp()); the caller starts it again, never resumes.
// No allocation: every block the caller's (the album bits, the folders'
// states, the wanted set's chunks). Portable, host-tested (test_stats_map).
namespace statsmap {

// The wanted set: rows of (hash u64, id u32), 12 B each, ascending by hash,
// in chunks of 16,380 B (1,365 rows: 16,392 B carves 16,900 B, as a
// 16,384 B chunk, 6.5).
constexpr uint32_t kRowBytes = 12;
constexpr uint32_t kChunkRows = 1365;
constexpr size_t kChunkBytes = static_cast<size_t>(kChunkRows) * kRowBytes;
constexpr uint32_t kMaxChunks = 8;               // 10,920 rows
constexpr uint32_t kNoId = 0xFFFFFFFFu;          // no track of this library has the hash (an orphan)
constexpr uint32_t kTwoTracks = 0xFFFFFFFEu;     // two tracks share it (a 64-bit collision): neither mapped
constexpr uint32_t wantedChunks(uint32_t rows) { return (rows + kChunkRows - 1) / kChunkRows; }

struct Wanted {
  uint8_t* chunk[kMaxChunks] = {};
  uint32_t chunks = 0;  // the chunks given
  uint32_t n = 0;       // rows in use
  uint32_t cap() const { return chunks * kChunkRows; }
  uint8_t* row(uint32_t i) const { return chunk[i / kChunkRows] + static_cast<size_t>(i % kChunkRows) * kRowBytes; }
  uint64_t hashAt(uint32_t i) const;
  uint32_t idAt(uint32_t i) const;
  void setId(uint32_t i, uint32_t id);
  // Empty, then hashes added in any order, then seal(): sorted, each once,
  // every id kNoId. add() false: no room.
  void clear() { n = 0; }
  bool add(uint64_t hash);
  void seal();
  // The row of `hash` (binary search), or -1.
  int64_t find(uint64_t hash) const;
  // The id of `hash`: kNoId when it has none (an orphan, a collision, not
  // wanted).
  uint32_t idOf(uint64_t hash) const;
};

// The album bits: one an album, set when a track of it has a play.
constexpr size_t albumBytes(uint32_t albums) { return (static_cast<size_t>(albums) + 7) / 8; }

class Map {
public:
  enum class Status : uint8_t {
    Idle,     // nothing begun
    Working,  // slices to go
    Ready,    // every track walked: the album bits and the ids are the product
    Aborted,  // the index changed or went (or wasn't ready at begin())
  };
  struct Config {
    uint32_t foldersPerSlice = 250;  // the join's
    uint32_t tracksPerSlice = 250;
  };
  struct Stats {
    uint32_t slices = 0;
    uint32_t tracks = 0;          // walked
    uint32_t unhashed = 0;        // ... with no path hash
    uint32_t playedTracks = 0;    // ... whose hash has a play
    uint32_t albumsPlayed = 0;    // albums with a played track (Ready)
    uint32_t wantedMapped = 0;    // wanted hashes with one track (Ready)
    uint32_t collisions = 0;      // wanted hashes two tracks share
  };

  void setConfig(const Config& c) { cfg_ = c; }
  // A track's hash where it has no folder's state (begin() with no
  // folders' block): trackPathHash() by default; the tests inject hashes.
  void setPathHash(pathhash::Walk::HashFn fn, void* ctx) { walk_.setHash(fn, ctx); }
  // Starts over `index`: `played` (null: no album bits), `wanted` (sealed;
  // null: no ids), the album bits (albumBytes(index->albumCount()); null
  // with `played` null), the folders' states (pathhash::Walk::bytesFor(
  // Walk::foldersFor(*index)); null: whole paths). Working, or Aborted (no
  // ready index).
  Status begin(const LibraryIndex* index, const statsrank::Played* played, Wanted* wanted, uint8_t* albumBits,
               void* folders);
  // One slice; `index` is the library's index now.
  Status step(const LibraryIndex* index);
  // Slices while it works and `more(ctx)` (asked after each) says the
  // pass's time isn't up.
  Status stepPass(const LibraryIndex* index, bool (*more)(void* ctx), void* ctx);
  // Forgets the walk (the blocks stay the caller's): Idle.
  void release();

  Status status() const { return status_; }
  // `index` is the one begin() was given, as it was then: the same object,
  // ready, the same tracks and build stamp (Join::matches()' test). False
  // once released, or after an Abort.
  bool matches(const LibraryIndex* index) const {
    return index_ && index == index_ && index->ready() && index->trackCount() == tracks_ &&
           index->buildStamp() == stamp_;
  }
  const Stats& stats() const { return stats_; }
  // Ready: whether album `a` has a played track; the albums without one.
  bool albumPlayed(uint32_t a) const { return bits_ && a < albums_ && (bits_[a / 8] >> (a % 8) & 1) != 0; }
  uint32_t albums() const { return albums_; }

private:
  static void visit(void* ctx, uint32_t track, uint64_t hash);
  Config cfg_;
  pathhash::Walk walk_;
  const LibraryIndex* index_ = nullptr;
  uint32_t tracks_ = 0;
  uint64_t stamp_ = 0;
  uint32_t visited_ = 0;  // tracks the walk handed (hashed)
  const statsrank::Played* played_ = nullptr;
  Wanted* wanted_ = nullptr;
  uint8_t* bits_ = nullptr;
  uint32_t albums_ = 0;
  Status status_ = Status::Idle;
  Stats stats_;
};

}  // namespace statsmap
