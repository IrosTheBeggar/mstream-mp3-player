// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardManifest.h"
#include "PlayLog.h"
#include "WallClock.h"

// The listening stats' aggregate in RAM, and the fold of the play log's
// slots into it (docs/LISTENING-STATS.md 4.1, 5.2, 5.6). tools/statsref.py
// is the reference: what a boot folds, what a save writes of each section
// (HEAD, MARK, HOUR, DAYS, RCNT, TRKD of /.player/stats.bin) byte for byte,
// on every fixture of test/fixtures/card/stats (test_stats_fold).
// Portable, host-tested; no file here: lib/core StatsFile reads and writes
// the card's files (stats.bin, its base), app/ListeningStats is the glue
// (the play log's Folds, the save's guards).
//   - the rules of 4.1: a slot that counts, the line, a play, a skip, the
//     source, dated, its hour, its day, a row's time;
//   - a track's row and the merge (5.2's ⊕): order-free, so a row folded
//     live and the same row folded at a boot (newest file first) agree but
//     for lastOrd, a round, compared by order;
//   - the aggregate: HEAD; MARK (kept for good, a row each (deviceId,
//     epoch) ever folded; the card's MARK beside RAM's, for the trim); HOUR
//     and DAYS (their newest 840 and 400 rows, carved once at those caps);
//     RCNT and TRKD in chunks of 16,384 B, never one block, never
//     realloc'd; every block through the allocator hook (PSRAM on the
//     device);
//   - the boot's fold: each slot that counts above the MARK read at the
//     boot, each once (5.6's ranges: in one file a device and epoch's seqs
//     only rise; across files a slot inside another file's range is met
//     twice), through a transient open-addressing index of u16 row numbers
//     for TRKD and RCNT; the gap (ACKS's seqs above MARK the log no longer
//     holds); the live fold, of a Written slot; the fold rounds;
//   - the save's side: the round, sinceDay (set once), the retention
//     (HOUR's 840 rows, DAYS' 400, RCNT's 32 days back from the earlier of
//     today and the newest dated day, then whole days past 4,096 rows), the
//     sections' rows in order for the writer, and RAM after a save landed.
// Nothing here moves a table for a slot: a row is found by key (the index,
// or a binary search of each chunk's sorted rows and a short unsorted
// tail) and a new one appended; sorting is a save's (and a short tail's).
namespace statsfold {

// ---------------------------------------------------------------------------
// The rules (4.1).
// ---------------------------------------------------------------------------
constexpr uint64_t kMinMs = wallclock::kMinMs;  // day 0, hour 0: 2026-01-01T00:00:00Z
constexpr uint32_t kHourMs = 3600000;
constexpr uint32_t kDayMs = 86400000;
constexpr uint32_t kLineMs = 30000;
// A source as HEAD's order and TRKS's flags keep it.
constexpr uint8_t kYou = 0, kShuffle = 1, kAutoDj = 2;
constexpr uint32_t kNoFirst = 0xFFFFFFFF;  // a row's firstAt with skips only
constexpr uint16_t kUnsetDay = 0xFFFF;     // HEAD's sinceDay, not set yet
constexpr uint16_t kFlagSinceCommit = 1;   // HEAD's flags bit 0: sinceDay from the sync's commitTime
constexpr uint16_t kFlagBaseLost = 2;      // bit 1: a base was lost (5.4)
constexpr uint32_t kMaxRound = 0xFFFFFF;   // rounds are 24 bits, saturating

// A slot that counts: a valid play slot (kind 1) with an epoch, a seq and
// a deviceId (METADATA 2.20.6, item 2). Once per (deviceId, epoch, seq):
// the boot's fold's business.
bool counts(const playlog::Slot& s);
// playedMs at or over the line, min(30 s, durationMs / 2), 30 s when
// durationMs is 0, the half not rounded: 2 x playedMs >= min(60,000,
// durationMs). Whatever the outcome.
bool isPlay(const playlog::Slot& s);
// Outcome 2 (skipped) under the line. Completed or stopped under it is
// neither; an outcome of 0 or above 3 reads as stopped.
bool isSkip(const playlog::Slot& s);
// 3 Auto DJ, 2 shuffle, anything else (1 manual, 0 not said, above 3) you.
uint8_t sourceOf(const playlog::Slot& s);
// startedAt from 2026-01-01 to 2099-12-31 UTC; else undated (0 while the
// clock was unset, or another device's clock).
constexpr bool dated(uint64_t ms) { return wallclock::believable(ms); }
// Of a dated time: its UTC hour (u24) and day (u16) since 2026-01-01, and
// a row's time, seconds since then plus 1 (0 is none).
constexpr uint32_t hourOf(uint64_t ms) { return static_cast<uint32_t>((ms - kMinMs) / kHourMs); }
constexpr uint16_t dayOf(uint64_t ms) { return static_cast<uint16_t>((ms - kMinMs) / kDayMs); }
constexpr uint32_t timeOf(uint64_t ms) { return static_cast<uint32_t>((ms - kMinMs) / 1000 + 1); }
// A row's time (timeOf's) back to its UTC day.
constexpr uint16_t dayOfTime(uint32_t t) { return static_cast<uint16_t>((t - 1) / 86400); }
// The board's clock as a UTC day: false while it is unset or not believed.
bool todayOf(uint64_t nowMs, uint16_t* day);

// ---------------------------------------------------------------------------
// A track's row (5.2: TRKS's, TRKD's) and the merge.
// ---------------------------------------------------------------------------
struct Track {
  uint64_t hash = 0;
  uint16_t plays = 0;          // saturating
  uint16_t skips = 0;          // saturating
  uint32_t firstAt = kNoFirst; // the earliest dated play's time; 0 plays, none dated; kNoFirst none
  uint32_t lastAt = 0;         // the latest dated play's time; 0 none
  uint32_t lastOrd = 0;        // the fold round of the latest play (u24)
  uint8_t src = kYou;          // the source of the play firstAt keeps
};
// a ⊕ b (a the older: the base before the delta, a row before a slot),
// a's hash. Every rule is order-free: plays and skips summed, saturating;
// firstAt the earlier dated time (0 only when neither side has one;
// kNoFirst loses to both), its source going with it, the smaller source on
// equal times (two undated included); lastAt and lastOrd the larger.
Track merge(const Track& a, const Track& b);
// A slot as the row of that one slot: a play is plays 1, firstAt and
// lastAt its time (firstAt 0 and lastAt 0 when undated), lastOrd `round`,
// its source; a skip is skips 1, firstAt kNoFirst. False for a slot that
// is neither.
bool slotTrack(const playlog::Slot& s, uint32_t round, Track* out);

// ---------------------------------------------------------------------------
// The file's sections (5.2): their types, strides, and HEAD.
// ---------------------------------------------------------------------------
enum Sec : uint8_t { kHead = 0, kMark, kHour, kDays, kRcnt, kTrkd, kSecs };
inline constexpr uint32_t kTypes[kSecs] = {cardcontract::fourcc("HEAD"), cardcontract::fourcc("MARK"),
                                    cardcontract::fourcc("HOUR"), cardcontract::fourcc("DAYS"),
                                    cardcontract::fourcc("RCNT"), cardcontract::fourcc("TRKD")};
inline constexpr uint32_t kStrides[kSecs] = {0, 24, 8, 16, 12, 24};  // HEAD a blob
constexpr uint32_t kTrks = cardcontract::fourcc("TRKS");      // the base file's one section
constexpr uint32_t kHeadBytes = 128;
constexpr uint32_t kTrackBytes = 24;  // a TRKS (TRKD) row

struct Head {
  struct Source {
    uint32_t plays = 0;
    uint32_t skips = 0;
    uint64_t listenedMs = 0;
  };
  uint64_t plays = 0;
  uint64_t skips = 0;
  uint64_t listenedMs = 0;
  uint64_t slots = 0;  // slots that count, folded
  Source src[3];       // you, shuffle, Auto DJ
  uint32_t undated = 0;
  uint16_t sinceDay = kUnsetDay;
  uint16_t flags = 0;
  uint32_t round = 0;  // the last fold round given out
  uint32_t baseGen = 0;
  uint32_t baseRows = 0;
  uint32_t baseCrc = 0;
  uint32_t twice = 0;
  uint32_t gap = 0;
};
void encodeHead(const Head& h, uint8_t out[kHeadBytes]);
Head decodeHead(const uint8_t in[kHeadBytes]);
// A TRKS (TRKD) row's 24 bytes; decodeTrack keeps flags' bits 0-1 only.
void encodeTrack(const Track& t, uint8_t out[kTrackBytes]);
Track decodeTrack(const uint8_t in[kTrackBytes]);

// The retention (5.2), and compaction's rule (5.3: a delta of `deltaRows`
// over a base of `baseRows`, at a boot's save): while nothing plays, a
// delta over 32 KB or over a quarter of the base (any delta over no base);
// forced past 96 KB at a safe point.
constexpr uint32_t kHourRows = 840;
constexpr uint32_t kDaysRows = 400;
constexpr uint32_t kRcntDays = 32;
constexpr uint32_t kRcntRows = 4096;
bool compactionDue(uint32_t deltaRows, uint32_t baseRows, bool playing, bool safePoint);

// The memory (5.6): RCNT and TRKD in chunks of this size (16,900 B carved:
// 6.5); HOUR and DAYS once at their caps (6,720 B, 6,400 B); MARK in a
// block of 64 rows (1,536 B), twice the rows each time it fills.
constexpr uint32_t kChunkBytes = 16384;
constexpr uint32_t kMarkBlockRows = 64;
constexpr uint32_t kMaxTrkdChunks = 96;  // 65,472 rows
constexpr uint32_t kMaxRcntChunks = 48;  // 65,520 rows
constexpr int kFiles = playlog::kNames;  // the log's files a scan reads (logName()'s i)

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

}  // namespace statsfold
