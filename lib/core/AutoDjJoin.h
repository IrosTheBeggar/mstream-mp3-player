// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardAutoDj.h"
#include "CardContainer.h"
#include "LibraryIndex.h"

// Auto DJ's join (docs/AUTODJ-INDEX.md 11.2; the engine design's 3.3): the
// card's AutoDJ index, an open mpdj::Table, turned into this library's
// track ids, and its DJRF filter records into one resident word a row. It
// runs after every library build (the boot's load, each leaveFence(), a
// remount that changed the index) on the loop, in slices: one step() a
// loop pass, each a few milliseconds, so lists keep their 30 fps.
// Portable, host-tested (test_autodj_join).
//
// The product is Maps (below), in blocks from the caller's hooks (the
// firmware's PSRAM): trackToRow (u16 a track), rowToTrack (u16 a row) and
// the filters (u32 a row), which the engine reads at every pick. The join
// owns them until release().
//
// The steps, in the format's order (8.6), each record read by its prefix
// and passed by its section's stride (a later minor's longer records still
// read, 9.2):
//   Hash      every library track's path hash (2.3: FNV-1a 64 of
//             "/music/...", LibraryIndex::trackPath()), kept as its top 32
//             bits and its id in parallel u32 and u16 arrays (6 bytes a
//             track, no padding), in runs of 4,096 (24 KB a block, so no
//             block grows with the library): hashPerSlice tracks a slice.
//   Sort      each run by (prefix, id), one run a slice: sortPairs(), the
//             bounded introsort of IdSort.h over the two arrays.
//   Strings   STRS checked by mpdj::Table::check() (8.3; a few hundred
//             bytes, one read).
//   Filters   DJRF streamed with its CRC: each row's word.
//   Paths     DJPH streamed with its CRC, its rows below rowCount and its
//             hashes strictly increasing (djPaths). For each entry, a
//             cursor in every run moves past the prefixes below the hash's
//             top 32 bits; a track whose prefix equals it is confirmed by
//             recomputing its full 64-bit hash (a 32-bit prefix collision
//             among 20,000 tracks has about a 5 % chance anywhere). The
//             one track it confirms gets the entry's row; the row gets its
//             first track (DJPH is in hash order, so the present file with
//             the smallest path hash: 11.2) and the present bit. An entry
//             two library tracks hash to (a 64-bit collision between two
//             of this library's paths) maps neither, as the writer leaves
//             out a hash two card files share (4.5). Then the runs and the
//             stream's buffer are freed: Ready.
// The two streamed sections go through cardcontract::Stream with a buffer
// of sliceBytes (4 KB, one FatFs piece), which refills whole sectors from
// a sector boundary (CardContainer.h): a slice takes the records that need
// at most one refill, so it reads at most one piece (a record longer than
// a piece, which no writer makes, takes a slice of its own). The verdict
// on a file is mpdj::Table::check(kUseDevice)'s, reason for reason: the
// join is that check, made while it builds.
//
// Memory, at 20,000 tracks and rows (the design's 3.4; test_autodj_join
// measures it): the maps 40 + 40 KB and the filters 80 KB, kept; the runs
// 120 KB and the 4 KB buffer while the join runs. The largest block is the
// filters' (rows x 4: 108 KB at 27k, under 256 KB up to 65,535 rows); a
// run's is 24 KB whatever the library, where one sorted array of (hash,
// track) would pass the 256 KB block limit above about 21,000 tracks.
//
// Aborts: every step() is given the library's index as the loop sees it
// now (Library::index(): nullptr behind the update step's fence). One that
// isn't the index the join began on (another pointer, none, not ready, or
// the same object rebuilt: another trackCount() or buildStamp()) aborts the
// join and frees everything, so a join never reads a freed index; release()
// does the same for the fence (enterFence() calls it before the index
// goes, its bytes into the update step's freed count). The Table and its
// Source must stay open until the join is Ready or released.
namespace autodj {

constexpr uint16_t kNoRow = 0xFFFF;    // trackToRow: the track has no row
constexpr uint16_t kNoTrack = 0xFFFF;  // rowToTrack: no track of this library is the row's
// Track ids are u16 in the maps: a library of more tracks doesn't use the
// index (random picks; the device tops out near 23,000-27,000 anyway).
constexpr uint32_t kMaxTracks = 65534;

// A row's filter word: bpm | cam << 8 | art << 16.
//   bpm  (bpm10 + 5) / 10, at most 255; 0 none (a bpm10 of 1-4 too)
//   cam  bits 0-4 the Camelot code (0 none, 1-12 = 1A-12A, 13-24 = 1B-12B;
//        DJRF's above 24 reads as 0), bit 5 BPM_ANALYSED, bit 7 present: a
//        track of this library is the row's (set by the join's last step)
//   art  the artistKey's low 16 bits, 0 made 1 when the key isn't 0, so 0
//        is "no artist" alone (never cooled; the design's 2.7)
constexpr uint8_t kCamCode = 0x1F;
constexpr uint8_t kCamAnalysed = 0x20;
constexpr uint8_t kCamPresent = 0x80;
inline uint8_t filterBpm(uint32_t f) { return static_cast<uint8_t>(f); }
inline uint8_t filterCam(uint32_t f) { return static_cast<uint8_t>(f >> 8); }
inline uint8_t filterCamelot(uint32_t f) { return filterCam(f) & kCamCode; }
inline bool filterAnalysed(uint32_t f) { return (filterCam(f) & kCamAnalysed) != 0; }
inline bool filterPresent(uint32_t f) { return (filterCam(f) & kCamPresent) != 0; }
inline uint16_t filterArtist(uint32_t f) { return static_cast<uint16_t>(f >> 16); }
// The word of a DJRF record's values (4.4), present clear.
uint32_t filterWord(uint16_t bpm10, uint8_t camelot, uint8_t flags, uint32_t artistKey);

// The join's product. The coverage counts are in tracks, so the Auto DJ
// page's "{mapped} of {tracks} tracks have neighbours" and "BPM {n}% · key
// {n}%" (withBpm and withKey over mapped) share one denominator.
struct Maps {
  uint32_t tracks = 0;   // the library's tracks: trackToRow's length
  uint32_t rows = 0;     // the index's rows: rowToTrack's and filters' length
  uint32_t mapped = 0;   // tracks that have a row
  uint32_t withBpm = 0;  // of those, tracks whose row has a BPM
  uint32_t withKey = 0;  // ... a key
  uint16_t* trackToRow = nullptr;  // kNoRow: no row (nullptr when tracks is 0)
  uint16_t* rowToTrack = nullptr;  // kNoTrack: no track (and filterPresent() false)
  uint32_t* filters = nullptr;     // filterWord()s, present set
};

// Sorts n (key, id) pairs, held in two parallel arrays, by key then id: a
// total order, as a run's ids are distinct (otherwise equal pairs may come
// in any order). IdSort.h's introsort: its recursion takes the smaller
// part (at most lg(n / 16) + 1 frames), a heap sort after 2 lg n
// partitions on one path, one insertion pass at the end.
void sortPairs(uint32_t* keys, uint16_t* ids, uint32_t n);

class Join {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  // A library track's path hash into `hash`, through `path` (`size`
  // bytes); false: it has none (its path doesn't fit), and the track gets
  // no row. The default hashes index.trackPath(), "/music/" then the rest
  // as cardcontract::pathHash() (the literal lowercase root, 2.3). The
  // tests inject hashes through it: no two paths here make a 64-bit
  // collision.
  using PathHashFn = bool (*)(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash,
                              void* ctx);

  enum class Status : uint8_t {
    Idle,       // nothing begun, or released
    Working,    // slices to go
    Ready,      // maps() is the product; the runs and the buffer are freed
    Absent,     // the file failed a check (why()): absent until the next remount or boot
    ReadError,  // a read failed (why() Io): the card's, not the file's; join again once it answers
    Aborted,    // the library index changed or went (or wasn't ready at begin()): everything freed
    NoMemory,   // an allocation failed: everything freed (random picks; retried after the next build)
    TooBig,     // a library of more than kMaxTracks tracks: the index isn't used
  };
  enum class Phase : uint8_t { None, Hash, Sort, Strings, Filters, Paths, Done };

  // The slices' sizes (the design's 3.3); the tests shrink them.
  struct Config {
    uint32_t runTracks = 4096;     // a run's pairs: 24 KB
    uint32_t hashPerSlice = 1000;  // tracks hashed a slice
    uint32_t sliceBytes = 4096;    // the stream's buffer (a multiple of 512, at least 1,024): a slice's read
  };

  // For the console's D, the tests and the one-off measurements.
  struct Stats {
    uint32_t slices = 0;      // step()s that did work, this join
    uint32_t hashed = 0;      // tracks in the runs
    uint32_t unhashed = 0;    // tracks without a path hash
    uint32_t paths = 0;       // DJPH entries read
    uint32_t confirms = 0;    // full hashes recomputed for an equal prefix
    uint32_t prefixOnly = 0;  // ... that weren't the entry's: 32-bit prefix collisions
    uint32_t ambiguous = 0;   // entries two or more tracks hash to (none of them mapped)
    uint32_t rowsMapped = 0;  // rows with a track (the present bit)
    size_t peakBytes = 0;     // the most held at once
    size_t largestBlock = 0;  // the largest single allocation
  };

  // nullptr hooks: malloc and free.
  explicit Join(AllocFn alloc = nullptr, FreeFn dealloc = nullptr);
  ~Join();
  Join(const Join&) = delete;
  Join& operator=(const Join&) = delete;

  // Before begin(); the next begin() uses them.
  void setConfig(const Config& c) { cfg_ = c; }
  void setPathHash(PathHashFn fn, void* ctx) {
    hash_ = fn;
    hashCtx_ = ctx;
  }

  // Starts a join of `table` (opened: Table::open() said Ok) and `index`
  // (ready), dropping whatever was here: the maps, the runs and the buffer
  // allocated, the maps filled with "none". Working, or Aborted (no ready
  // index), Absent (Why::Missing: the table isn't open; Why::Counts: its
  // open() failed after all, DJRF not rowCount records), TooBig, NoMemory.
  Status begin(const cardcontract::mpdj::Table& table, const LibraryIndex* index);
  // One slice; `index` is the library's index now. The status after it.
  Status step(const LibraryIndex* index);
  // Aborts a join under way and frees everything, the maps too: the bytes
  // given back (the fence's alsoFreed). Idle after it.
  size_t release();

  Status status() const { return status_; }
  Phase phase() const { return phase_; }
  cardcontract::Why why() const { return why_; }
  const Maps* maps() const { return status_ == Status::Ready ? &maps_ : nullptr; }
  // Working or Ready, and `index` is still the one the join is for (the
  // same object, ready, the same tracks and build stamp).
  bool matches(const LibraryIndex* index) const;
  size_t heldBytes() const { return held_; }
  const Stats& stats() const { return stats_; }

  static constexpr size_t kPathBytes = 512;  // the default hash's path buffer

private:
  struct Run {
    uint32_t* keys = nullptr;  // the block: cap keys, then cap ids
    uint16_t* ids = nullptr;
    uint32_t len = 0, cap = 0;
    uint32_t cur = 0;          // Paths: the first pair whose key isn't below the entry's
  };

  void* take(size_t bytes);
  void give(void* p, size_t bytes);
  size_t freeTransient();
  size_t freeAll();
  void fail(Status s, cardcontract::Why w);
  bool working() const { return status_ == Status::Working; }
  // The phases: false when there was nothing to do (the next phase goes on
  // in the same slice).
  bool hashSlice();
  bool sortSlice();
  bool stringsSlice();
  bool filtersSlice();
  bool pathsSlice();
  void startStream(uint32_t section);
  // The refills Stream makes to read up to `recEnd` (its mirror, kept in
  // `bufEnd`): a slice takes records while it needs at most one.
  uint32_t refillsTo(uint32_t recEnd, uint32_t* bufEnd) const;
  void merge(uint64_t hash, uint16_t row);

  AllocFn alloc_;
  FreeFn free_;
  PathHashFn hash_;
  void* hashCtx_ = nullptr;
  Config cfg_;

  const cardcontract::mpdj::Table* table_ = nullptr;
  const LibraryIndex* index_ = nullptr;
  uint64_t stamp_ = 0;
  Status status_ = Status::Idle;
  Phase phase_ = Phase::None;
  cardcontract::Why why_ = cardcontract::Why::Ok;
  Maps maps_;
  Stats stats_;
  size_t held_ = 0;

  Run* runs_ = nullptr;
  uint32_t runCount_ = 0;
  uint32_t nextTrack_ = 0;  // Hash: the next track
  uint32_t fillRun_ = 0;    // Hash: the run being filled
  uint32_t sortRun_ = 0;    // Sort: the next run
  uint8_t* buf_ = nullptr;  // the stream's buffer
  uint32_t bufBytes_ = 0;
  cardcontract::Stream stream_;
  uint32_t streamEnd_ = 0;  // the section's end
  uint32_t bufEnd_ = 0;     // the end of the stream's buffered bytes (refillsTo())
  uint32_t rec_ = 0;        // the next record of the section
  uint64_t prevHash_ = 0;   // Paths: the last entry's hash
  char path_[kPathBytes] = {};
};

}  // namespace autodj
