// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardAutoDj.h"
#include "CardContainer.h"
#include "LibraryIndex.h"
#include "MoodFilter.h"

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
// Where the blocks go (setPlacement(): the firmware's, for PSRAM's TLSF, which
// rounds a request up to its size class, by up to a thirty-second of it, and
// carves that size from the low end of the closest-fitting free block):
// PSRAM's largest free block is what a library update's new track table needs,
// so the join takes as little of it as it can. Each block, the kept ones
// first, goes where the heap puts it when that isn't the largest free block (a
// smaller one fits it: after a library update, the maps and the runs fit the
// holes the rebuild left, 2026-10-14). The rest go to that block's top, out of
// the way: spacers (spacerFor()) hold its bottom while the transient ones are
// taken, and once they are given back the kept ones go to the room left for
// them above, the closest fit they have. The runs' hole at the end then joins
// the free part below the maps, and what is carved later comes from that
// part's low end: the maps stay next to the rest of the block, and giving them
// back (release(): a library update's memory check,
// LibraryUpdate::setGiveBack()) makes one block of it again, as if Auto DJ had
// never joined. The spacers never leave less than the placement's margin free
// (the device's 2026-10-14 check: spacers over the whole block took PSRAM's
// free to 10 KB for a moment at every join, and another task allocating then
// would have failed): the kept ones taken after the spacers are part of it
// (155 KB at 19,410 tracks, over the margin), and when they aren't enough the
// spacers leave the rest of the margin above the blocks. Another task's block
// can still take that room in those milliseconds: the placement falls short
// then (Stats::topShort), not the join: the spacers go and the rest go where
// the heap puts them. Without the hooks (the host, a heap too full for a
// spacer): the kept ones first, the transient ones above them, whose hole
// joins the free tail when they go.
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

// Auto DJ's mood filter over the library's words (docs/MOOD-FILTERS.md
// 6.1): on() with words and an active filter, and then a row is a
// candidate only when its track passes (the engine's runs hold no other).
// Off (no mood data, no filter, a level alone): every track passes, and
// the engine is as without one. The firmware's AutoDj sets it on its copy
// of the maps; the join never does.
struct MoodView {
  const uint16_t* words = nullptr;  // LibraryIndex::moodWords(): a word per track id (null: no mood data)
  uint32_t tracks = 0;              // ... how many
  uint16_t q = 0;                   // the filter's 11 bits (5.4), as read: an invalid q is 0
  moodfilter::Filter f;             // ... unpacked
  // ... made ready once: passes() runs once a track in the scan and the
  // passes by id (6.3), where matches() made it again each time. On the
  // Core2 at 19,413 tracks a jump's pick took 29.5 ms that way, 11.7 ms
  // this way, idle (MEASURED 2026-11-06, MOOD-FILTERS.md 9.8).
  moodfilter::Test test;
  bool active = false;  // f.active()
  MoodView() = default;
  MoodView(const uint16_t* w, uint32_t n, uint16_t bits)
      : words(w),
        tracks(n),
        q(moodfilter::valid(bits) ? bits : 0),
        f(moodfilter::Filter::unpack(bits)),
        test(moodfilter::Test::of(f)),
        active(f.active()) {}
  bool on() const { return words && active; }
  // A track with no word (beyond `tracks`) passes no active filter.
  bool passes(uint32_t track) const { return !on() || (track < tracks && test.holds(words[track])); }
};

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
  MoodView mood;                   // off in the join's own; AutoDj::index() sets its copy's
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

// The spacer that puts `bytes` at the top of the free block that `largest`
// tells of, under a TLSF heap (ESP-IDF's: a request is rounded up to its size
// class's next boundary, by up to a thirty-second of it, and a block of that
// size is carved from the low end of a free block that holds it; `largest` is
// heap_caps_get_largest_free_block()'s answer, the largest request that
// succeeds, that block rounded down to its class): what stays above the spacer
// is at least `bytes`, a thirty-second more and 1 KB (each block's rounding, its
// header and poisoning), wherever in its class the free block is; a request
// just under a class's boundary, so that it takes no more than that boundary.
// 0: no room for one (the blocks go where the heap puts them). What stays can
// be over that by a class of the spacer's and one of the free block's, a
// sixteenth of it in all: takeSpacers() closes it in when it can.
size_t spacerFor(size_t largest, size_t bytes);

// The placement's spacers (Join::Placement), over a heap's hooks: spacers
// of spacerFor() from the largest free block, added to those `out` holds
// (kMax in all): asked again while one comes from another block (TLSF
// serves a request from the closest size class that holds it), and once
// more on what is left of the largest while it is still the largest and
// leaves room for one (spacerFor()'s sixteenth); such a closer fit that lands
// in another block (a hole the blocks themselves may want) is given back at
// once, and that's it. None is ever so big that, rounded up, it leaves less
// than `keepFree` bytes free (`freeBytes`; nullptr: not asked; a KB more for
// the headers): a smaller one leaves the difference above the blocks. True:
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

// ---- The listening stats' hold (docs/LISTENING-STATS.md 5.14, fix A) ----
// A block held across passes at the top of the largest free block (the
// ranges' block, a full log's overflow) leaves a free block T above it, the
// room the spacers left over; under TLSF's good fit any long-lived carve
// made while it is held (a frame's canvas, a queue) goes into T, not where
// main puts it, and pins the largest free block split for the session once
// the hold ends (the device's B1, 2026-11-17). So T is closed: caps carved
// into it, held with the block, given back after it.
//
// What a heap walk says of a block in use (by the pointer its alloc gave)
// and of the block physically right above it. The firmware's hook walks
// PSRAM's heap (heap_caps_walk(): ESP-IDF 5.5's TLSF, light poisoning); the
// host's walks its model heap.
struct Above {
  bool found = false;      // the walk met the block, in use
  bool bounded = false;    // the walk gave up at its visit bound before it met the block
  bool onSpacers = false;  // (asked with a spacer) the free block right under it holds that spacer's address
  bool free = false;       // the block right above it is free (false: in use, or the heap's end)
  size_t blockBytes = 0;   // the block's own size as the heap counts it (TLSF's block_size(): poisoning, rounding)
  size_t bytes = 0;        // the block right above: its size as the heap counts it (0: the heap's end)
  void* capAt = nullptr;   // (free) the pointer a carve taking its head returns
  uint32_t visits = 0;     // blocks the walk looked at
  uint32_t us = 0;         // the walk's time (the firmware's; 0 on the host)
};
using AboveFn = void (*)(const void* block, const void* spacer, Above* out);

// One heap walk's state for an AboveFn: the walk hands step() each block in
// address order, heap by heap, by the walker's pointer (the alloc's pointer
// less `head`: light poisoning's 8 B under ESP-IDF 5.5; the host model's
// 4 B header) and size, in use or free; step() fills `out` and says when to
// stop. The firmware's walkStep() (app/ListeningStats) is a line over it,
// run inside the heap's lock (heap_caps_walk()): it compares and copies
// words, nothing else (no allocation, no line, no heap call, no timing).
// Portable, so the host tests what the device's walk decides: the block met
// by its pointer and in use, the block right above it in the same heap (its
// cap's pointer its walker's + `head`), the free block under it holding the
// spacer's address (onSpacers), the bound before the block (Lost).
struct WalkAbove {
  // `spacer` nullptr: not asked. `bound`: the blocks looked at before it
  // meets the block (past it: out->bounded, found false); 0: none. `out` is
  // reset here.
  WalkAbove(const void* block, const void* spacer, size_t head, uint32_t bound, Above* out);
  // The walk's next block; false: stop (the rest of the walk is ignored).
  bool step(intptr_t heapStart, uintptr_t ptr, size_t size, bool used);

  uintptr_t block = 0, spacer = 0;  // walker pointers (the alloc's less head); spacer 0: not asked
  uintptr_t head = 0;
  uint32_t bound = 0;
  intptr_t heap = 0, prevHeap = 0;
  uintptr_t prevAt = 0;
  bool prevFree = false, seen = false, done = false;
  Above* out = nullptr;
};

// The request whose carve takes the head of a free block of `bytes` (a TLSF
// block size) from that block's own free list and no more than it holds:
// its size class's floor less `overhead` (the poisoning's 12 B), the whole
// of it under 128 B (those lists hold one size each); 0 under 16 B with the
// poisoning, which no request can take. TLSF rounds a request up to its
// class's next boundary; the floor is one, so the carve is the floor and it
// maps to the free block's own list, whose head that block is. Three caps
// at most close any free block up to 4 MB, leaving 0 B or 12 B
// (docs/LISTENING-STATS.md 5.14, fix A).
size_t capRequest(size_t bytes, size_t overhead);

enum class Close : uint8_t {  // (Nothing first: a report's default is all zeros, so .bss)
  Nothing,    // the block above in use, or the heap's end, or a free block no request can take: nothing to close
  Closed,     // the free block above closed (caps n >= 1; nothing a request can take is left above)
  NoSpacers,  // takeSpacers() placed none: the block went where the heap put it, nothing closed
  NotOnTop,   // not on the spacers' top (a smaller free block took it): nothing closed
  Margin,     // closing it would leave under keepFree + 1 KB free: left open
  Missed,     // a cap came back elsewhere (another task's carve first): given back at once, the rest left open
  NoCap,      // no memory for a cap: the rest left open
  Lost,       // the walk didn't meet the block (or gave up at its visit bound): nothing closed
  Left,       // Caps::kMax caps and a free block still above (never under ESP-IDF 5.5's TLSF), or the block
              // above grew between two caps (another task's block freed into it), or another task's block
              // took the rest's head between a cap and the next walk (the caps short of T): the caps held
              // kept, the rest left open
  Full,       // TopHold already holds kMax blocks: carved, not tracked, nothing closed
  NoBlock,    // no memory for the block itself (no free block takes it: asked of `largest` first, no carve)
  Plain,      // taken with close false (the overflow's chunks without the arena): under its spacers,
              // keepFree alone kept, nothing closed by design
  Room,       // (`room`) with the hold's caps held no free block took a decoder's blocks: every cap the
              // hold held given back (this block's and the older blocks', at least one), T open again
  NoRoom,     // (`room`, `yield`) not even with them given back: the block given back too (no block)
};
const char* closeName(Close c);  // "closed", "nothing", ...

struct Caps {
  static constexpr int kMax = 3;
  void* p[kMax] = {};
  uint8_t n = 0;
};

// What a take did (the firmware's [listen] line).
struct CloseReport {
  Close why = Close::Nothing;
  bool bounded = false;       // (Lost) the walk gave up at its visit bound
  uint8_t caps = 0;           // the caps held (Room, NoRoom: those this take carved, given back since)
  uint8_t uncapped = 0;       // (Room, NoRoom) the hold's caps given back, this block's and the older blocks'
  size_t blockBytes = 0;      // the block as the heap counts it
  size_t aboveBytes = 0;      // the free block found above it (T; 0: none)
  size_t usedAbove = 0;       // (Nothing) the block in use above it
  size_t capBytes = 0;        // the caps' footprints (each the heap's size + 4)
  const void* want = nullptr; // (Missed) where the cap should have come back
  const void* got = nullptr;  // ... and where it did
  uint32_t walks = 0;
  uint32_t us = 0, usMax = 0;  // the walks' time in all, and the longest one's
  uint32_t visits = 0;         // the most blocks one walk looked at
};

struct CloseHooks {
  AboveFn above = nullptr;
  void* (*alloc)(size_t) = nullptr;
  void (*release)(void*) = nullptr;
  size_t (*freeBytes)() = nullptr;  // nullptr: no margin check
  size_t keepFree = 0;
  size_t overhead = 0;              // the heap's bytes a request (12: light poisoning)
};
// Close the free block above `block`: walk, then caps of capRequest() each
// at the address the walk gave (anything else freed at once: Missed; no
// retry, the same request finds the same list head), a walk after each,
// until the block above is in use or nothing a request can take is left.
// A block in use above the last cap is Closed only when the caps'
// footprints are T's (each the heap's size + 4: they sum to T's + 4 when
// they took it whole); short of that, another task's carve took the rest's
// head between the cap and its walk, and the rest above that block is
// open (Left).
// The margin (keepFree + 1 KB free once closed) is checked against the
// first walk's free block; each cap after the first only while the block
// above is what the caps before it left (its size the first's less their
// footprints) and the margin holds: a larger one means another task freed a
// block into it, which the caps would otherwise take whole (Left).
// `spacer`: the last spacer takeSpacers() placed (given back already: its
// address compared, never read); nullptr: not checked. The caps go to `out`.
Close closeAbove(const void* block, const void* spacer, const CloseHooks& h, Caps* out, CloseReport* rep);

// The listening stats' hold: blocks held across passes at the top of the
// largest free block, each carved under takeSpacers()'s spacers (keepFree
// and the block's own carve kept free while they are held: free never under
// keepFree once it is carved), the spacers given back, then the free block
// left above it closed (closeAbove()) when the block sits on the spacers'
// top (in a hole it moves nothing main would put there). Given back
// bottom-up: the newest first (each is carved under the ones before it, the
// top of the largest free block being right under them), and a block given
// back while a newer one is held waits for it; each block before its caps,
// the caps in carve order. So no free block ever opens above a held one. Not
// for a block held for the session (its caps would be). Loop task only. No
// hooks stored: the caller passes the same Hooks each call.
class TopHold {
public:
  static constexpr int kMax = 2;  // the ranges' block and the overflow's arena
  struct Hooks {
    size_t (*largest)() = nullptr;  // nullptr: no spacers (the block where the heap puts it, nothing closed)
    size_t (*freeBytes)() = nullptr;
    void* (*alloc)(size_t) = nullptr;
    void (*release)(void*) = nullptr;
    AboveFn above = nullptr;  // nullptr: no walk (nothing closed)
    size_t keepFree = 0;
    size_t overhead = 0;
  };
  // nullptr: no memory (NoBlock: no free block takes `bytes`, asked of
  // `largest` first, so no carve fails: the firmware's failed carve is a
  // "[heap] FAILED" line and a count). `close` false (the overflow's chunks
  // one by one, no room for its arena): carved as before the caps, under
  // spacers keeping keepFree alone, nothing closed (Plain), still tracked
  // while there is room.
  // `room` (0: none): the bytes a decoder carves at its open, asked while
  // one may open in the hold (a track playing or a start waiting: the next
  // track's open is never held, docs/LISTENING-STATS.md 5.14). The caps
  // keep keepFree + 1 KB free, not a decoder's blocks: with the block and
  // every cap held, the largest free block must still take them (`largest`
  // at least the room's carves, a thirty-second and 1 KB over), or the
  // hold's caps are all given back, the newest block's first (Room: T open
  // again, as before the caps), and with `yield` (a block that has a way
  // without itself: the overflow's arena, its chunks one by one), if that
  // isn't enough either, the block is given back too: nullptr (NoRoom).
  void* take(const Hooks& h, size_t bytes, CloseReport* rep, bool close = true, size_t room = 0,
             bool yield = false);
  // `p` given back: freed with its caps once every block taken after it is
  // given back (now, or with the newest's give-back). One not taken here
  // (or taken while Full) is freed at once; nullptr: nothing.
  void give(const Hooks& h, void* p);
  // Every held block's caps given back now, the newest block's first (the
  // blocks stay held, in order): the overflow's fallback, its chunks carved
  // as before the caps, with the room the caps held. The caps freed.
  int uncap(const Hooks& h);
  int held() const { return n_; }

private:
  struct Entry {
    void* block = nullptr;
    Caps caps;
    bool given = false;
  };
  Entry e_[kMax];
  uint8_t n_ = 0;
};

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
    NoMemory,   // no free block held one, spacers gone: everything freed (random picks; retried after the next build)
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
    bool topShort = false;        // that top fell short of a block: not placed, the rest where the heap put them
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
  // block (`largest` before it, or at `*low`, where the last one from it
  // was): given back then, and nullptr (also when the allocation failed:
  // *failed). A block at least `*minTop` bytes isn't tried (a smaller one
  // needed the largest, so it would); one that needs it lowers *minTop.
  void* probe(size_t bytes, size_t largest, size_t* minTop, void** low, bool* failed);
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
