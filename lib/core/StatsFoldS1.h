// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "StatsFold.h"

// S1 as built (414488c): the listening stats' whole aggregate in RAM, the
// fold the S1 rework replaces (docs/LISTENING-STATS.md 5.14). Its R3 made
// statsfold::Fold the rework's (the records, MARK and HEAD in the object);
// this class is the one before it, moved here unchanged, so lib/core
// StatsFile and StatsGlue and app/ListeningStats keep building until R4
// and R5 move them onto statsfold::Fold. Deleted then. Its rules, rows,
// HEAD and the file's sections are statsfold's (StatsFold.h).
namespace statsfold {
namespace s1 {

// The memory (5.6): RCNT and TRKD in chunks of this size, whole rows of
// both tables and nothing over (1,366 of RCNT's 12 B, 683 of TRKD's 24 B),
// so RCNT at its 4,096-row cap fills three and TRKD at the forced
// compaction's 96 KB (4,096 rows) six, as the plan's budget has them (11).
// 16,384 B held 1,365 and 682: a fourth and a seventh chunk for the caps'
// last rows (S1's gate after the give-back). A chunk carves what 16,384 B
// did, 16,900 B with the poisoning's 12 B and the header (6.5). HOUR and
// DAYS once at their caps (6,720 B, 6,400 B); MARK in a block of 64 rows
// (1,536 B), twice the rows each time it fills.
constexpr uint32_t kChunkBytes = 16392;
constexpr uint32_t kMarkBlockRows = 64;
constexpr uint32_t kMaxTrkdChunks = 96;  // 65,568 rows (past 65,535 the boot's fold has no index: 5.6)
constexpr uint32_t kMaxRcntChunks = 48;  // 65,568 rows

// ---------------------------------------------------------------------------
// The aggregate.
// ---------------------------------------------------------------------------
class Fold {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  // Every block through `alloc` (null: malloc). The object itself is about
  // 1.5 KB: the firmware places it in PSRAM too.
  explicit Fold(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~Fold();
  Fold(const Fold&) = delete;
  Fold& operator=(const Fold&) = delete;

  // HOUR's, DAYS' and MARK's blocks, carved once. False: no memory.
  bool begin();
  // Empty, as a fresh start (no stats.bin: every slot on the card folds):
  // every counter, flag and table; the blocks kept for the next load.
  void clear();
  // Empty, and every block given back (the chunks, the spare, HOUR, DAYS,
  // MARK): the stats off for the session (a read that isn't on), nothing
  // held for them. begin() before any load again.
  void release();
  // A library update's give-back (docs/LISTENING-STATS.md 6.7, the owner's
  // decision of 2026-11-11): the resident blocks given back (RCNT's and
  // TRKD's chunks, the spare, HOUR, DAYS), MARK's block and rows kept, so
  // covered() and markOf() answer as before (the card's MARK and RAM's);
  // nothing else answers, and nothing may be folded, until the next load
  // (begin(), then StatsFile's read). The bytes given back; 0 while the
  // boot's fold runs (its blocks are the scan's).
  size_t giveBack();
  // What giveBack() would give back now (the update's memory check counts
  // it in the step's room).
  size_t givesBack() const;
  // The load's chunks: room for these rows of RCNT and TRKD (stats.bin's
  // counts) and one spare chunk (5.6). False: no memory.
  bool reserveLoad(uint32_t rcntRows, uint32_t trkdRows);
  // One spare chunk held for whichever of TRKD and RCNT a fold fills first
  // (the glue's, at a pass where Auto DJ's join isn't Working). False: no
  // memory.
  bool keepSpare();
  // Room for the next `slots` folds with no allocation: TRKD's and RCNT's
  // chunks, and during the boot's fold their indexes (each at most half
  // full). The glue's, before each piece of the boot's scan, in a pass
  // where it may carve. False: no memory (nothing lost: a fold then carves
  // what it needs itself).
  bool reserve(uint32_t slots);

  // The load (StatsFile's read), in the file's order: stats.bin's
  // generation, HEAD (its first 128 bytes), then each array's rows (a
  // newer minor's longer rows by their prefix: `stride` at least the
  // section's). False: a row out of order (the file is corrupt: 5.4), or
  // no memory (noMemory()).
  void loadGeneration(uint32_t generation);
  void loadHead(const uint8_t* bytes);
  bool loadRows(Sec sec, const uint8_t* rows, uint32_t n, uint32_t stride);
  // The base stats.bin names is gone, or another file (5.4): flags bit 1,
  // baseGen, baseRows and baseCrc 0. The rows before it are lost; the
  // totals stay.
  void baseLost();

  // The boot's fold (the play log's Folds): begun after the load, each slot
  // of the scan in its order (`file`: logName()'s i), partial() for each
  // log file the scan couldn't read whole, ended when the scan is: the
  // gap (with `acks`, the root's ACKS) unless partial, HEAD's twice and
  // gap, the transient index and ranges freed, and RCNT held to its
  // 4,096-row cap by whole days, oldest first (the save's cut without the
  // date's: a boot whose save doesn't land holds no more than that; a
  // live play dated before that cut then adds no RCNT row, which any later
  // save would cut: the file the same byte for byte). plan() ends it too;
  // after a partial scan (no save) the glue ends it when the journal is
  // ready.
  // A scan begun again (a remount) starts from clear() and the load: the
  // folds made before are dropped. beginBoot false: no memory.
  bool beginBoot(const cardcontract::msmf::Acks& acks);
  void bootSlot(const playlog::Slot& s, int file);
  void partial();
  void endBoot();
  bool booting() const { return booting_; }
  // A play written this session (Outbox::done(), Written only), at round
  // HEAD.round + 2: RAM only.
  void liveSlot(const playlog::Slot& s);

  // A play slot at or under the card's MARK for its (deviceId, epoch) (the
  // one read at the boot, or the last save's that landed); none: seq 0
  // only. The trim's and the rotation's question.
  bool covered(const playlog::Slot& s) const;
  // RAM's MARK for (deviceId, epoch): the highest seq folded (0: none).
  uint32_t markOf(const uint8_t deviceId[6], uint64_t epoch) const;
  // Folded, or MARK raised, since the card's copy (or since clear()).
  bool dirty() const { return dirty_; }
  // A log file wasn't read whole this session: no save (5.6).
  bool partialScan() const { return partial_; }
  // A fold found no memory: nothing more is folded and nothing saved.
  bool noMemory() const { return noMemory_; }

  // This session's counts (since clear()): slots folded (the boot's and
  // live), met twice, the gap counted.
  uint32_t folded() const { return folded_; }
  uint32_t twice() const { return twice_; }
  uint32_t gap() const { return gap_; }
  const Head& head() const { return head_; }
  uint32_t generation() const { return generation_; }
  uint32_t rows(Sec sec) const;
  // TRKD's row (the delta) for a path hash. False: none.
  bool track(uint64_t hash, Track* out);
  // HOUR's row for an hour; DAYS' for a day (16 bytes as the file has
  // them). False: none.
  bool hour(uint32_t hour, uint8_t* plays, uint32_t* listenedMs);
  bool day(uint16_t day, uint8_t out[16]);
  // The bytes held, and the most held at once (5.6's figures).
  size_t held() const { return held_; }
  size_t peak() const { return peak_; }
  void resetPeak() { peak_ = held_; }

  // The save (StatsFile's commit). plan(): what stats.bin is to hold now,
  // RAM unchanged (a save that fails changes nothing): the round, sinceDay
  // (set once: the day of the earliest dated play folded this session,
  // not after today; else today; else the root's commitTime, flagged),
  // the generation, and each section's rows after the retention. False:
  // nothing may be saved (a partial scan, no memory). A save that
  // compacts writes base ⊕ delta first (merge() over the base's rows and
  // TRKD's, from rowsBegin(plan, kTrkd)), then sets the plan's base fields
  // and `compact`: TRKD is then empty in this file. saved(): the file
  // landed: RAM takes the plan's HEAD and retention, TRKD empties when it
  // compacted, and the card's MARK is RAM's. plan(), the rows and saved()
  // in one loop call: no fold between them.
  struct Plan {
    Head head;
    uint32_t generation = 0;
    uint32_t rows[kSecs] = {};  // HEAD 1 (a blob); TRKD's before a compaction
    uint32_t rcntCut = 0;       // RCNT's rows with an earlier day left out
    bool compact = false;
    uint32_t rowsOf(Sec sec) const { return sec == kTrkd && compact ? 0 : rows[sec]; }
    uint32_t bytesOf(Sec sec) const { return sec == kHead ? kHeadBytes : rowsOf(sec) * kStrides[sec]; }
  };
  bool plan(uint64_t nowMs, bool hasCommit, uint64_t commitSec, Plan* out);
  // A section's rows as `p` writes them, in order: rowsBegin(), then
  // nextRows() until it gives 0 (each row at the section's stride; HEAD
  // is encodeHead(p.head)). nextTrack() reads TRKD's as rows.
  void rowsBegin(const Plan& p, Sec sec);
  uint32_t nextRows(uint8_t* out, uint32_t maxRows);
  bool nextTrack(Track* out);
  void saved(const Plan& p);

  // (The tests' look inside.)
  uint32_t indexSize(Sec sec) const;
  uint32_t chunks(Sec sec) const;
  bool spare() const { return spare_ != nullptr; }
  uint32_t rcntFloor() const { return rcntFloor_; }

  // Rows as RAM holds them (public for the implementation's helpers).
  struct MarkRow {
    uint8_t dev[6];
    uint16_t pad;
    uint64_t epoch;
    uint32_t seq;   // RAM's
    uint32_t card;  // the card's (the boot's read, or the last save's that landed)
  };
  // MARK's row `i` of rows(kMark), in no set order (the save sorts them):
  // the console's look (app/ListeningStats, Hm). Null past the last.
  const MarkRow* markRow(uint32_t i) const { return i < markN_ ? &mark_[i] : nullptr; }
  struct HourRow {
    uint32_t hourPlays;  // hour u24 | plays u8 << 24
    uint32_t ms;
  };
  struct DaysRow {
    uint16_t day, plays, skips;
    uint8_t djPlays, djSkips;
    uint32_t ms;
    uint8_t shufPlays, shufSkips;
    uint16_t pad;
  };
  struct RcntRow {
    uint32_t lo, hi;  // the path hash
    uint16_t day, plays;
  };
  struct TrkdRow {
    uint64_t hash;
    uint16_t plays, skips;
    uint32_t firstAt, lastAt;
    uint32_t ord;  // lastOrd u24 | src << 24
  };
  struct Range {
    uint32_t lo, hi;  // 0: none (seqs are at least 1)
  };
  struct AckTrack {
    uint8_t dev[6];
    uint64_t epoch;
    uint32_t seq;    // ACKS's
    uint32_t mark;   // the boot's MARK
    uint32_t held;   // seqs in (mark, seq] a valid play slot of the log holds, each once
    Range r[kFiles];
  };
  struct Index {
    uint16_t* e = nullptr;  // row numbers; 0xFFFF empty
    uint32_t size = 0;      // a power of two
    uint32_t count = 0;
  };
  // A table of fixed-size rows in chunks of kChunkBytes: rows dense from
  // row 0 (every chunk full but the last with rows), chunks past it empty
  // capacity; each chunk's rows [0, sorted) in order.
  template <class Row, uint32_t kMax>
  struct Chunked {
    using RowType = Row;
    static constexpr uint32_t kPer = kChunkBytes / sizeof(Row);
    static constexpr uint32_t kMaxChunks = kMax;
    Row* c[kMax] = {};
    uint16_t sorted[kMax] = {};
    uint32_t chunks = 0;
    uint32_t rows = 0;
    uint32_t unsorted = 0;  // rows appended since the last sort
    Index ix;
    Row& at(uint32_t r) { return c[r / kPer][r % kPer]; }
    uint32_t inChunk(uint32_t i) const {
      const uint32_t b = i * kPer;
      return rows > b ? (rows - b < kPer ? rows - b : kPer) : 0;
    }
  };
  // HOUR, DAYS: one block of `cap` rows; rows [s0, s1) in order, the rest
  // (at most 64) not; full, a new key takes the oldest row's place.
  template <class Row>
  struct Capped {
    Row* rows = nullptr;
    uint32_t cap = 0, n = 0, s0 = 0, s1 = 0;
    uint32_t hit = 0;  // the last row found (a slot's neighbours share it)
  };

private:
  void* grab(size_t bytes);
  void drop(void* p, size_t bytes);
  void* takeChunk();
  void giveChunk(void* p);
  template <class Tab>
  bool room(Tab& t, uint32_t more);
  template <class Tab>
  void shrink(Tab& t);
  template <class Tab>
  void releaseAll(Tab& t);
  template <class K, class Tab>
  bool buildIndex(Tab& t, uint32_t atLeast);
  template <class Tab>
  void freeIndex(Tab& t);
  template <class K, class Tab>
  bool append(Tab& t, const typename K::Row& r);
  void dropBoot();
  bool growMark();
  MarkRow* findMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) const;
  MarkRow* addMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at);
  void sortMark();
  void fold(const playlog::Slot& s, uint32_t round);
  void foldTrack(const playlog::Slot& s, uint32_t round);
  void foldRcnt(uint16_t day, uint64_t hash);
  void heldSlot(const playlog::Slot& s, int file);
  void sortTables();
  uint16_t newestDay() const;
  uint32_t rcntCut(uint32_t dateCut, uint32_t* kept);
  void closeRcnt(uint32_t cut);

  AllocFn alloc_;
  FreeFn free_;
  size_t held_ = 0;
  size_t peak_ = 0;
  void* spare_ = nullptr;

  Head head_;
  uint32_t generation_ = 0;
  uint32_t loadedRound_ = 0;  // HEAD's round as loaded: the boot's +1, live +2
  uint32_t used_ = 0;         // the highest round used this session (0: none)
  uint16_t firstPlayDay_ = kUnsetDay;
  uint32_t rcntFloor_ = 0;  // RCNT's rows end at the boot's cap: no new row dated before it until a save lands
  bool dirty_ = false;
  bool partial_ = false;
  bool noMemory_ = false;
  bool booting_ = false;
  uint32_t folded_ = 0, twice_ = 0, gap_ = 0;

  MarkRow* mark_ = nullptr;
  uint32_t markCap_ = 0, markN_ = 0, markSorted_ = 0;
  mutable uint32_t markHit_ = 0;
  Range* ranges_ = nullptr;  // the boot's: kFiles a MARK row, while booting
  uint32_t rangesCap_ = 0;
  AckTrack* acks_ = nullptr;  // the boot's: ACKS's rows above MARK
  uint32_t acksCap_ = 0;
  uint32_t acksN_ = 0;
  uint32_t ackHit_ = 0;

  Capped<HourRow> hour_;
  Capped<DaysRow> days_;
  Chunked<RcntRow, kMaxRcntChunks> rcnt_;
  Chunked<TrkdRow, kMaxTrkdChunks> trkd_;

  // The load's order check: the last key of each section.
  bool loadedAny_[kSecs] = {};
  uint8_t lastMark_[14] = {};
  uint32_t lastHour_ = 0;
  uint16_t lastDay_ = 0;
  uint16_t lastRcntDay_ = 0;
  uint64_t lastRcntHash_ = 0;
  uint64_t lastHash_ = 0;

  // A save's rows, as rowsBegin() set them.
  const Plan* plan_ = nullptr;
  Sec sec_ = kHead;
  uint32_t at_ = 0;
  uint16_t pos_[kMaxTrkdChunks] = {};
};

}  // namespace s1
}  // namespace statsfold
