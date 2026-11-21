// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "StatsFile.h"
#include "StatsFold.h"
#include "TagStore.h"

// The listening stats' rankers (docs/LISTENING-STATS.md 4.3, 6.8; S2's
// C12): the shelves' lists, by path hash, over the card's stats as the
// session holds them: the base stats.bin names ⊕ stats.bin's TRKD ⊕ the
// records' rows (5.14's "The screens": the rank reads the card's sections
// and takes a copy of the records), and RCNT's rows with the records'.
// tools/statsref.py is the reference: its lists() on every fixture boot of
// test/fixtures/card/stats and its rank scenarios (test_stats_rank).
// Portable, host-tested.
//
//   - Ranker, the lists alone: each track's whole row and each hash's
//     window counts given in any order, each list kept to its cap and 16
//     more (6.6: a few orphans don't shorten a shelf) by a bounded heap, then
//     sorted best first. No allocation: the caller's block (kBytes).
//       Most played, All time  plays at least 1; plays, then lastAt, newest
//                              first; then the hash
//       Most played, 7 and 30  plays in the window (RCNT's), at least 1; then
//         days                 the latest day of a play in it, newest first;
//                              then the hash. None while the clock is unset
//       New finds              firstAt dated and inside the 30-day window;
//                              plays, then firstAt, newest first; then the
//                              hash. None while the clock is unset
//       Old favourites         plays at least 5, lastAt dated and before the
//                              30-day window; plays, then lastAt, newest
//                              first; then the hash. None while the clock is
//                              unset
//       Recently played        plays at least 1; lastAt, newest first; then
//                              the rows with no dated play by lastOrd, newest
//                              first; then the hash
//     "Then the hash": the smaller first, as an unsigned 64-bit integer.
//     Not played yet is the map's (StatsMap: the album bits).
//   - Windows: RCNT's rows in the 30-day window, gathered in the caller's
//     block (12 B a row), then summed by hash for the Ranker.
//   - Card, the rank's read (the card worker's Job::Stats, one step), in
//     S3a's order (9.7's V1, 6.8 as decided): DAYS ⊕ the records' DAYS rows
//     into the report (the 7 days, the span); RCNT's rows and the records'
//     to Windows, finished into the Ranker (their rows dead from here, so
//     the u40 set may take their room); then the base's TRKS, stats.bin's
//     TRKD and the records' rows merged by hash (three sorted streams, each
//     row once: the base the older, ⊕), each whole row to the Ranker and,
//     played, to one played set: the u64 set (S2's: chunks of 2,048 hashes,
//     in hash order; Hw, Hl3) or the u40 set (S3's asks); every 64th base
//     row's hash to the sparse index (optional). With no lists (the lean
//     Not played yet ask's played-set-only rank) the Ranker isn't touched
//     and DAYS and RCNT aren't read. Every section's CRC and order checked
//     as it streams; the frame stats.bin must be the copy the session
//     stands on (its generation and header CRC) and the base the one its
//     HEAD names. No allocation: the caller carves every block before (the
//     worker allocates nothing: 6.8).
namespace statsrank {

// The lists. A shelf of 4.3 and its window: Most played's three, then
// New finds, Old favourites and Recently played.
enum List : uint8_t { kMost7 = 0, kMost30, kMostAll, kNewFinds, kOldFavourites, kRecent, kLists };
constexpr uint32_t kPastCap = 16;  // rows kept past each cap (6.6)
constexpr uint32_t kCaps[kLists] = {50, 50, 50, 50, 200, 200};
constexpr uint32_t kOldFavourite = 5;  // plays at least
constexpr uint32_t rowsOf(List l) { return kCaps[l] + kPastCap; }
const char* listName(List l);  // "most7", "most30", "mostAll", "newFinds", "oldFavourites", "recent"

// A list's row. count: the plays (in the window, for Most played's 7 and
// 30 days). at: lastAt (Most played All time, Old favourites, Recently
// played), firstAt (New finds), or the latest day of a play in the window
// (Most played's 7 and 30 days). ord: lastOrd (Recently played). src: the
// source firstAt keeps (New finds).
struct Entry {
  uint64_t hash = 0;
  uint32_t count = 0;
  uint32_t at = 0;
  uint32_t ord = 0;
  uint8_t src = 0;
};

class Ranker {
public:
  // The caller's block: every list's cap and 16 more.
  static constexpr uint32_t kEntries = 3 * (50 + kPastCap) + (50 + kPastCap) + 2 * (200 + kPastCap);
  static constexpr size_t kBytes = kEntries * sizeof(Entry);

  // Starts over in `mem` (kBytes, 8-byte aligned). `hasToday` false: the
  // clock unset (the windows' lists, New finds and Old favourites none).
  void begin(void* mem, bool hasToday, uint16_t today);
  // A track's whole row (base ⊕ delta), once a hash, in any order.
  void track(const statsfold::Track& t);
  // A hash's plays in the 7- and 30-day windows and the latest day of a play
  // in each (RCNT's), once a hash with any, in any order.
  void windows(uint64_t hash, uint32_t plays7, uint16_t latest7, uint32_t plays30, uint16_t latest30);
  // Each list sorted, best first.
  void finish();

  // Whether the list is offered (the windows', New finds and Old
  // favourites need the clock), its rows and one of them.
  bool offered(List l) const { return l == kMostAll || l == kRecent || hasToday_; }
  uint32_t size(List l) const { return n_[l]; }
  const Entry& at(List l, uint32_t i) const { return list_[l][i]; }
  // Every row offered to the list since begin(), kept or not (S3a: hub
  // rows 3-4 count New finds' and Old favourites', 4.2 as decided). 0 for a
  // list not offered.
  uint32_t total(List l) const { return total_[l]; }
  bool hasToday() const { return hasToday_; }
  uint16_t today() const { return today_; }
  // The 30-day window's first day (today - 29, at least day 0).
  uint16_t first30() const { return first30_; }
  uint16_t first7() const { return first7_; }

private:
  void offer(List l, const Entry& e);
  Entry* list_[kLists] = {};
  uint32_t n_[kLists] = {};
  uint32_t total_[kLists] = {};
  bool hasToday_ = false;
  uint16_t today_ = 0, first7_ = 0, first30_ = 0;
};

// Whether `a` comes before `b` in list `l` (the order of 4.3).
bool before(List l, const Entry& a, const Entry& b);

// RCNT's rows in the 30-day window, summed by hash for the Ranker.
class Windows {
public:
  static constexpr size_t kRowBytes = 12;
  static size_t bytesFor(uint32_t rows) { return static_cast<size_t>(rows) * kRowBytes; }
  // `mem` holds `cap` rows (bytesFor(cap), 4-byte aligned). No clock: none
  // taken, and `mem` may be null (S3a: no window room carved undated).
  void begin(void* mem, uint32_t cap, bool hasToday, uint16_t today);
  // A row of RCNT's (or the records'): a day, a hash, its plays that day.
  // Outside the 30-day window (or no clock): passed over. False: no room
  // (the block holds `cap` rows).
  bool add(uint16_t day, uint64_t hash, uint32_t plays);
  // Sorted by hash, each hash's sums and latest days to `r`.
  void finish(Ranker& r);
  uint32_t rows() const { return n_; }

private:
  struct Row {
    uint32_t lo, hi;
    uint16_t day, plays;
  };
  Row* rows_ = nullptr;
  uint32_t cap_ = 0, n_ = 0;
  bool hasToday_ = false;
  uint16_t today_ = 0;
};

// ---------------------------------------------------------------------------
// The rank's read (the card worker's).
// ---------------------------------------------------------------------------
constexpr uint32_t kPlayedChunk = 2048;  // hashes a played set's chunk (16,384 B)
constexpr size_t kPlayedChunkBytes = kPlayedChunk * sizeof(uint64_t);
constexpr uint32_t kSparseEvery = 64;    // a sparse index row each 64 base rows (12 B)
constexpr size_t kSparseRowBytes = 12;
// The played set's chunks for at most `rows` played hashes.
constexpr uint32_t playedChunks(uint32_t rows) { return (rows + kPlayedChunk - 1) / kPlayedChunk; }
constexpr uint32_t sparseRows(uint32_t baseRows) { return (baseRows + kSparseEvery - 1) / kSparseEvery; }
constexpr uint32_t kMaxPlayedChunks = 64;  // 131,072 hashes: past it, the rest isn't in the set (counted)

// The played set: every hash with a play, ascending, in chunks (S2's, the
// exact reference: Hw, Hl3).
struct Played {
  uint64_t* chunk[kMaxPlayedChunks] = {};
  uint32_t chunks = 0;  // the chunks given
  uint32_t n = 0;       // hashes in it
  uint32_t lost = 0;    // played hashes past its room
  uint64_t at(uint32_t i) const { return chunk[i / kPlayedChunk][i % kPlayedChunk]; }
  bool contains(uint64_t hash) const;
};

// The u40 played set (S3a; the owner's call 3, "5-byte fingerprints", 4.3):
// each played hash's top 40 bits, 5 B a row (little-endian, as the card's
// fields), in one region of the caller's. Ascending as the rank streams the
// hashes: the top bits of an ascending u64 sequence ascend, so no sort. Two
// played hashes that share their top 40 bits give two equal rows, as the
// u64 set gives two. contains() matches by the top 40 bits alone, so the
// error goes one way: a hash whose top 40 bits equal a played one's reads
// as played (an unplayed album hidden, never a played one shown).
struct Played40 {
  static constexpr size_t kRowBytes = 5;
  // The region for `rows`: rows x 5, rounded up to 16 (9.7's grades).
  static constexpr size_t bytesFor(uint32_t rows) {
    return (static_cast<size_t>(rows) * kRowBytes + 15) & ~static_cast<size_t>(15);
  }
  // A hash's row: its top 40 bits.
  static constexpr uint64_t keyOf(uint64_t hash) { return hash >> 24; }
  uint8_t* rows = nullptr;  // bytesFor(cap)
  uint32_t cap = 0;         // rows the region holds
  uint32_t n = 0;           // hashes in it
  uint32_t lost = 0;        // played hashes past its room
  // Row i: a hash's top 40 bits (keyOf()'s).
  uint64_t at(uint32_t i) const;
  // A row equal to keyOf(hash).
  bool contains(uint64_t hash) const;
};

// The sparse index of the base: each kSparseEvery-th row's hash and row.
struct Sparse {
  uint8_t* rows = nullptr;  // kSparseRowBytes each: the hash (8 B), the row (4 B)
  uint32_t cap = 0, n = 0;
  uint64_t hashAt(uint32_t i) const;
  uint32_t rowAt(uint32_t i) const;
  // The base rows [*from, *to) that hold `hash` if any row does (no index:
  // the whole base).
  void span(uint64_t hash, uint32_t baseRows, uint32_t* from, uint32_t* to) const;
};

// What the rank reads besides the card: the copy the session stands on, its
// base, the records' rows (Fold::Cursor's, TRKD's, RCNT's and DAYS', each
// sorted and a key's records ⊕'d), and today.
struct Input {
  uint32_t generation = 0;  // stats.bin's (0 with headerCrc 0: none on the card)
  uint32_t headerCrc = 0;
  bool present = false;     // a stats.bin the session stands on
  uint32_t baseGen = 0;     // HEAD's (0: none)
  uint32_t baseRows = 0;
  uint32_t baseCrc = 0;     // the base's header CRC, HEAD's
  const uint8_t* trkd = nullptr;  // the records' TRKD rows (24 B each)
  uint32_t trkdRows = 0;
  const uint8_t* rcnt = nullptr;  // the records' RCNT rows (12 B each)
  uint32_t rcntRows = 0;
  const uint8_t* days = nullptr;  // the records' DAYS rows (16 B each; S3a)
  uint32_t daysRows = 0;
  bool hasToday = false;
  uint16_t today = 0;
};

// The rank's memory, every block the caller's.
struct Memory {
  uint8_t* work = nullptr;  // statsfile::kWorkBytes: the frames, the row readers, two read buffers
  // Ranker::kBytes. Null: the played-set-only rank (S3a, the lean Not
  // played yet ask's): the Ranker not touched, nothing offered, RCNT and
  // DAYS not read, no window, only TRKS ⊕ TRKD ⊕ the records' TRKD into
  // the played set.
  void* lists = nullptr;
  // Windows::bytesFor(windowRows). Null with the clock unset: no window
  // room is needed (dated, a rank with RCNT rows in the 30 days is Room).
  void* windows = nullptr;
  uint32_t windowRows = 0;
  // One played set or the other: the u40 set when given (S3's asks; it may
  // take the window's room, dead before the tracks stream), the u64 set
  // then left as it is; else the u64 set (S2's: Hw, Hl3); neither: none
  // kept.
  Played* played = nullptr;
  Played40* played40 = nullptr;
  Sparse* sparse = nullptr; // its rows given (null: none kept)
};

enum class Outcome : uint8_t {
  Ok,
  Changed,  // stats.bin isn't the copy the session stands on (another player's save, or gone)
  Base,     // the base isn't the one HEAD names (gone, another file, short)
  Io,       // the card failed a read
  Damaged,  // a section failed its CRC or its rows' order
  Room,     // the windows' block was short (the caller's count): said before the tracks are read
};
const char* outcomeName(Outcome o);

// A day of the 7-day window (4.2 as decided: hub row 1 and Week by UTC day):
// DAYS' row ⊕ the records' for that day, each field saturating at its
// row's width as a save's ⊕ would write it (plays u16, Auto DJ's u8,
// listenedMs u32).
struct Day {
  uint32_t listenedMs = 0;
  uint16_t plays = 0;
  uint16_t autoDjPlays = 0;
  uint16_t autoDjSkips = 0;
};
constexpr uint32_t kWeekDays = 7;

struct Report {
  Outcome outcome = Outcome::Ok;
  uint32_t baseRows = 0;    // read
  uint32_t trkdRows = 0;
  uint32_t rcntRows = 0;
  uint32_t recordRows = 0;  // the records' rows read: TRKD's, RCNT's and DAYS'
  uint32_t tracks = 0;      // whole rows (distinct hashes)
  uint32_t played = 0;      // ... with a play
  uint32_t windowRows = 0;  // RCNT rows in the 30-day window
  uint64_t bytesRead = 0;
  // S3a (none read by the played-set-only rank):
  uint32_t daysRows = 0;    // stats.bin's DAYS rows read
  Day days[kWeekDays];      // oldest first, days[6] today; all 0 with the clock unset
  // DAYS ⊕ the records': the first and newest day, clock or not (O5's span,
  // 3.1: newestDay - firstDay + 1); RCNT ⊕ the records': its oldest day
  // (7.6's stats.days_n: RCNT capped shortens the 30 days). kUnsetDay: no row.
  uint16_t firstDay = statsfold::kUnsetDay;
  uint16_t newestDay = statsfold::kUnsetDay;
  uint16_t rcntFirstDay = statsfold::kUnsetDay;
};

// The files a rank (or a track's read) reads: stats.bin and the base HEAD
// names, opened by the caller (the loop: a FatFile is a PSRAM carve, and the
// worker allocates nothing: 6.8), closed by it after.
struct Files {
  tagstore::File* bin = nullptr;
  tagstore::File* base = nullptr;
};
// The opens: Ok; Changed (stats.bin there where the session stands on none,
// or gone where it stands on one); Io (the card failed an open); Base (the
// base HEAD names isn't there). Nothing left open but on Ok.
Outcome open(tagstore::Fs& fs, const Input& in, Files* out);
void close(tagstore::Fs& fs, Files* f);

// The rank (the card worker's step, over files open()ed): the lists into
// `ranker` (none with mem.lists null: `ranker` untouched), the report's
// days, the played set and the sparse index. Reads only.
Report rank(const Files& f, const Input& in, const Memory& mem, Ranker& ranker);
// open(), the rank, close() (the host's tests).
Report rank(tagstore::Fs& fs, const Input& in, const Memory& mem, Ranker& ranker);

// A track's rows (the console's Hp: the Track lane, the card worker's step):
// the base's by a binary search (a 24 B read a step: no sparse index is
// kept between asks, the owner's "A", 2026-11-20), stats.bin's TRKD streamed
// for it (its CRC and order to its end), the frames checked as rank()'s.
// `work`: statsfile::kWorkBytes. The records' row is the loop's to add
// (Fold::track()).
struct TrackRows {
  Outcome outcome = Outcome::Ok;
  bool base = false, delta = false;  // a row found in the base, in TRKD
  statsfold::Track baseRow, deltaRow;
  uint32_t reads = 0;     // the base's rows read
  uint32_t trkdRows = 0;  // TRKD's rows passed
};
TrackRows trackRows(const Files& f, const Input& in, uint64_t hash, uint8_t* work);

}  // namespace statsrank
