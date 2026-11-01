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
// runs while Auto DJ is on (switched on, and after every library build
// then: the boot's load, each leaveFence(), a remount that changed the
// index) on the loop, in slices of a millisecond or two: stepPass() a loop
// pass, as many as fit its time and at most one card read, so lists keep
// their 30 fps and a join with a track playing takes a couple of seconds.
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
//   Folders   each library folder's path hashed once (FNV-1a 64 is
//             sequential: a track's hash goes on from its folder's with "/"
//             and its name), with its length, 10 bytes a folder (25 KB at
//             the 19k card's 2,484 folders), foldersPerSlice a slice. With
//             the default hash only, and up to kMaxFolders folders (else
//             each track's whole path, as the hook does). The 2026-10-14
//             device check's join was CPU-bound: on the host the folders'
//             states make the hashing 3.3 times faster, and the join
//             (Hash and Paths' confirms, two thirds of it) about half.
//   Hash      every library track's path hash (2.3: FNV-1a 64 of
//             "/music/...", as trackPathHash() takes it), kept as its top
//             32 bits and its id in parallel u32 and u16 arrays (6 bytes a
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
//             among 20,000 tracks has about a 5 % chance anywhere): at
//             most confirmsPerSlice a slice (the next slice goes on in the
//             buffer, reading nothing). The one track it confirms gets the
//             entry's row; the row gets its first track (DJPH is in hash
//             order, so the present file with the smallest path hash:
//             11.2) and the present bit. An entry two library tracks hash
//             to (a 64-bit collision between two of this library's paths)
//             maps neither, as the writer leaves out a hash two card files
//             share (4.5). Then the runs and the stream's buffer are
//             freed: Ready.
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
// 120 KB, the 4 KB buffer and the folders' states (10 B a folder) while the
// join runs. The largest block is the filters' (rows x 4: 108 KB at 27k,
// under 256 KB up to 65,535 rows); a run's is 24 KB whatever the library,
// where one sorted array of (hash, track) would pass the 256 KB block limit
// above about 21,000 tracks.
//
// Where the blocks go (setPlacement(): the firmware's, for PSRAM's TLSF,
// which serves a block from the low end of the closest-fitting free block,
// the request rounded up a thirty-second first): PSRAM's largest free
// block is what a library update's new track table needs, so the join
// takes as little of it as it can. Each block, the kept ones first, goes
// where the heap puts it when that isn't the largest free block (a smaller
// one fits it: after a library update, the maps and the runs fit the holes
// the rebuild left, 2026-10-14). The rest go to that block's top, out of
// the way: spacers (spacerFor()) hold its bottom while the transient ones
// are taken, and once they are given back the kept ones go to the room
// left for them above, the closest fit they have. The runs' hole at the
// end then joins the free part below the maps, and what is carved later
// comes from that part's low end: the maps stay next to the rest of the
// block, and giving them back (release(): a library update's memory check,
// LibraryUpdate::setGiveBack()) makes one block of it again, as if Auto DJ
// had never joined. The spacers never leave less than the placement's
// margin free (the device's 2026-10-14 check: spacers over the whole block
// took PSRAM's free to 10 KB for a moment at every join, and another task
// allocating then would have failed): the kept ones taken after the
// spacers are part of it (155 KB at 19,410 tracks, over the margin), and
// when they aren't enough the spacers leave the rest of the margin above
// the blocks. Without the hooks (the host, a heap too full for a spacer):
// the kept ones first, the transient ones above them, whose hole joins the
// free tail when they go.
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
  uint32_t rowsMapped = 0;  // rows with a track (the present bit): rows when every row has one
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

// A library track's path hash as the join takes it (AUTODJ-INDEX.md 2.3):
// its path (index.trackPath(), through `path`, `size` bytes) from "/music/"
// (any case) on as cardcontract::pathHash() (the root's literal
// lowercase), any other path its bytes' FNV-1a 64. False: no path (it
// doesn't fit). Join's default hook; the firmware's Auto DJ button looks a
// track's row up with it (mpdj::Table::rowOf()) while nothing is joined.
bool trackPathHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash);

// The spacer that puts `bytes` at the top of a free block of `largest`
// bytes under a TLSF heap (ESP-IDF's: a request is served from the low end
// of a free block whose size class, rounded up a thirty-second, holds it):
// what is left above the spacer is `bytes`, a thirty-second more and 1 KB
// (a block's rounding as it is searched for, the headers), or a
// thirty-second of `largest` when that is more (the spacer's own rounding:
// its request rounded up must still be within the block). 0: no room for
// one (the blocks go where the heap puts them).
size_t spacerFor(size_t largest, size_t bytes);

// The placement's spacers (Join::Placement), over a heap's hooks: spacers
// of spacerFor() from the largest free block, added to those `out` holds
// (kMax in all): asked again while one comes from another block (TLSF
// serves a request from the closest size class that holds it), and once
// more on what is left of the largest while it is still the largest and
// leaves room for one (a small library's block: its own thirty-second);
// such a closer fit that lands in another block (a hole the blocks
// themselves may want) is given back at once, and that's it. None is ever
// so big that less than `keepFree` bytes stay free (`freeBytes`; nullptr:
// not asked; a KB more for the headers): a smaller one leaves the
// difference above the blocks. True:
// the largest block's top is what is left; false: none could be placed
// there. Those taken are held until giveSpacers() frees them.
struct Spacers {
  static constexpr int kMax = 8;
  void* p[kMax] = {};
  int n = 0;
};
bool takeSpacers(size_t bytes, size_t keepFree, size_t (*largest)(), size_t (*freeBytes)(), void* (*alloc)(size_t),
                 void (*release)(void*), Spacers* out);
void giveSpacers(Spacers* s, void (*release)(void*));

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
  enum class Phase : uint8_t { None, Hash, Sort, Strings, Filters, Paths, Done, Folders };

  // The slices' sizes (the design's 3.3; 2026-10-13: 112 slices of 1,000
  // hashes or a piece's ~400 confirms took 2.1 s idle on the device, about
  // 10 ms of work each: these are a quarter of that, several a pass); the
  // tests shrink them.
  struct Config {
    uint32_t runTracks = 4096;     // a run's pairs: 24 KB
    uint32_t hashPerSlice = 250;   // tracks hashed a slice
    uint32_t sliceBytes = 4096;    // the stream's buffer (a multiple of 512, at least 1,024): a slice's read
    uint32_t confirmsPerSlice = 100;  // Paths: full hashes recomputed a slice (0: no limit)
    uint32_t foldersPerSlice = 250;   // Folders: folders hashed a slice
  };
  // The folders' states up to this many folders (an 80 KB block).
  static constexpr uint32_t kMaxFolders = 8192;

  // For the console's D, the tests and the one-off measurements.
  struct Stats {
    uint32_t slices = 0;      // step()s that did work, this join
    uint32_t reads = 0;       // card reads: the stream's pieces, STRS's check
    uint32_t hashed = 0;      // tracks in the runs
    uint32_t unhashed = 0;    // tracks without a path hash
    uint32_t paths = 0;       // DJPH entries read
    uint32_t folders = 0;     // folders whose path was hashed (0: each track's whole path)
    uint32_t confirms = 0;    // full hashes recomputed for an equal prefix
    uint32_t prefixOnly = 0;  // ... that weren't the entry's: 32-bit prefix collisions
    uint32_t ambiguous = 0;   // entries two or more tracks hash to (none of them mapped)
    uint32_t rowsMapped = 0;  // rows with a track (the present bit)
    size_t peakBytes = 0;     // the most held at once
    size_t largestBlock = 0;  // the largest single allocation
    // The placement (setPlacement()), at begin():
    bool placed = false;          // blocks taken at the largest free block's top, under spacers
    uint8_t keptElsewhere = 0;    // the kept blocks (of 3) in a smaller free block
    uint32_t runsElsewhere = 0;   // the transient blocks (runs, folders' states, buffer) in a smaller free block
    size_t topBytes = 0;          // the bytes taken at the largest free block's top
    size_t topKept = 0;           // ... of the kept blocks (what giving them back gives that block)
    size_t lowestFree = 0;        // the heap's lowest free bytes while begin() took its blocks
  };

  // Where begin()'s blocks go (above): the heap's largest free block and
  // its free bytes (the firmware's heap_caps_get_largest_free_block() and
  // heap_caps_get_free_size() for PSRAM; the blocks come from the join's
  // own alloc), and the bytes the spacers leave free at least. nullptr
  // hooks: none (the blocks go where the heap puts them).
  struct Placement {
    size_t (*largest)() = nullptr;
    size_t (*freeBytes)() = nullptr;
    size_t margin = 0;
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
  void setPlacement(const Placement& p) { place_ = p; }

  // Starts a join of `table` (opened: Table::open() said Ok) and `index`
  // (ready), dropping whatever was here: the maps, the runs and the buffer
  // allocated, the maps filled with "none". Working, or Aborted (no ready
  // index), Absent (Why::Missing: the table isn't open; Why::Counts: its
  // open() failed after all, DJRF not rowCount records), TooBig, NoMemory.
  Status begin(const cardcontract::mpdj::Table& table, const LibraryIndex* index);
  // One slice; `index` is the library's index now. The status after it.
  Status step(const LibraryIndex* index);
  // The firmware's loop pass: slices while the join works, until one has
  // read the card (at most one read a pass: a piece of DJRF or DJPH, or
  // STRS, behind the decoder's own reads) or `more(ctx)`, asked after each
  // slice, says the pass's time is up. Hashing and sorting read nothing,
  // so a pass takes several of them; so do the Paths slices that go on in
  // the buffer. The status after it.
  Status stepPass(const LibraryIndex* index, bool (*more)(void* ctx), void* ctx);
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
  bool foldersSlice();
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
  // A track's path hash: from its folder's state (Folders), else the hook.
  bool trackHash(uint32_t track, uint64_t* hash);

  // begin()'s blocks: the kept ones (the maps), the transient ones (the
  // runs' table, the runs, the buffer, the folders' states). False: one
  // failed (fail() frees).
  bool takeKept(uint32_t n, uint32_t rows);
  bool takeTransient(uint32_t n);
  // The folders' states in `block` (folderBytes() of it); none wanted: true.
  bool takeFolders(void* block);
  size_t folderBytes() const { return static_cast<size_t>(folderCount_) * (sizeof(uint64_t) + sizeof(uint16_t)); }
  // Under the placement (setPlacement()): every block where the heap puts
  // it unless that is the largest free block, the rest at its top.
  bool takePlaced(uint32_t n, uint32_t rows);
  // A block where the heap puts it, unless it came from the largest free
  // block (`largest` before it): given back then, and nullptr (also when
  // the allocation failed: *failed). A block at least `*minTop` bytes isn't
  // tried (a smaller one needed the largest, so it would); one that needs
  // it lowers *minTop.
  void* probe(size_t bytes, size_t largest, size_t* minTop, bool* failed);
  void noteFree();  // Stats::lowestFree

  AllocFn alloc_;
  FreeFn free_;
  PathHashFn hash_;
  void* hashCtx_ = nullptr;
  Placement place_;
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
  // Folders: each folder's path hash so far and its path's length
  // (kNoPath: it has none: deeper than LibraryIndex::folderPath() goes, or
  // longer than kPathBytes), in one block; nullptr: the hook's hash.
  static constexpr uint16_t kNoPath = 0xFFFF;
  uint64_t* folderHash_ = nullptr;
  uint16_t* folderLen_ = nullptr;
  uint32_t folderCount_ = 0;
  uint32_t nextFolder_ = 0;
  cardcontract::Stream stream_;
  uint32_t streamEnd_ = 0;  // the section's end
  uint32_t bufEnd_ = 0;     // the end of the stream's buffered bytes (refillsTo())
  uint32_t rec_ = 0;        // the next record of the section
  uint64_t prevHash_ = 0;   // Paths: the last entry's hash
  char path_[kPathBytes] = {};
};

}  // namespace autodj
