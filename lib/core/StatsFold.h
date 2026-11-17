// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardManifest.h"
#include "PlayLog.h"
#include "WallClock.h"

// The listening stats' fold of the play log's slots (docs/LISTENING-STATS.md
// 4.1, 5.2, 5.6, and S1's rework, 5.14: a fixed resident set). The fold
// keeps no table: HEAD and MARK in the object, and one record a slot that
// counts, folded since the card's stats.bin (or a held tmp) took them; a
// save merges them with the card's sections (lib/core StatsFile reads and
// writes the files). tools/statsref.py is the reference: the records merged
// with the card's rows equal its whole aggregate's, byte for byte, given the
// same save moments (test_stats_fold, every boot of test/fixtures/card/
// stats/index.json and random sessions against a port of it).
//   - the rules of 4.1: a slot that counts, the line, a play, a skip, the
//     source, dated, its hour, its day, a row's time;
//   - a track's row and the merge (5.2's ⊕): order-free, so a row folded
//     live and the same row folded at a boot (newest file first) agree but
//     for lastOrd, a round, compared by order; each section's rows as the
//     file has them, their order and their ⊕ (a save's merge);
//   - the records: 40 in the object (20 B each: the slot's hash, time,
//     playedMs, play or skip, source, round), and past them, during a
//     boot's fold only, overflow chunks of 819 (16,384 B, at most 16: a
//     full log's 13,106 slots) through the allocator hook (the firmware's:
//     at the top of PSRAM); a live slot with the records full is not folded
//     and the session is behind (no record, no save until the next boot);
//   - MARK (a row each (deviceId, epoch) ever folded: RAM's seq, the card's
//     for covered(), and a held tmp's): 16 rows in the object, or a block of
//     the caller's for a card with more than 8 (its rows and 8 more);
//   - the boot's fold: each slot that counts above the MARK read at the
//     boot, each once (5.6's ranges: in one file a device and epoch's seqs
//     only rise; across files a slot inside another file's range is met
//     twice), the ranges and ACKS's rows in the caller's memory (the
//     ranges' block, 3,072 B with 16 MARK rows); the gap (ACKS's seqs
//     above MARK the log no longer holds); the live fold, of a Written
//     slot; the fold rounds;
//   - the save's side: the round, sinceDay (set once), the generation,
//     MARK's rows, the records in each section's key order (Cursor: each
//     run sorted in place, up to 17 merged, a key's records ⊕'d into one
//     row), and RAM after a save landed, or was held.
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

// A section's rows as the file has them (kStrides[sec] bytes): their order
// and their ⊕, for a save's merge of the card's rows with the records'
// (5.14). compareRows(): <0, 0 or >0 by the section's key (MARK: deviceId,
// then epoch; HOUR: the hour; DAYS: the day; RCNT: the day, then the hash;
// TRKD: the hash). addRow(): `a` ⊕= `b`, their keys equal, `a` the older
// (the card's): HOUR's plays (u8) and listenedMs (u32), DAYS' plays and
// skips (u16), Auto DJ's and shuffle's (u8) and listenedMs (u32), RCNT's
// plays (u16), each a saturating sum as the fold's; TRKD by merge(). Not
// for HEAD or MARK (RAM writes them whole).
int compareRows(Sec sec, const uint8_t* a, const uint8_t* b);
void addRow(Sec sec, uint8_t* a, const uint8_t* b);

constexpr int kFiles = playlog::kNames;  // the log's files a scan reads (logName()'s i)

// The resident set (5.14): the records, and MARK's rows. In internal RAM on
// the device (the glue's object, app/ListeningStats: .bss, at most 2,048 B),
// so 40 records, not the design's 96: a save each 24 plays (5.14's "Moved
// to internal RAM").
constexpr uint32_t kRecords = 40;      // statsref's RECORDS: a live slot past them is behind
constexpr uint32_t kSaveAt = 24;       // statsref's SAVE_AT: the records' save is due from this many
constexpr uint32_t kRecordBytes = 20;
// The boot's overflow past the records: chunks of 16,384 B (819 records,
// 4 B over; a carve of 16,900 B with the poisoning's 12 B and the header,
// 6.5), at most 16: a full log's 13,106 slots (40 + 16 x 819 = 13,144).
constexpr uint32_t kChunkBytes = 16384;
constexpr uint32_t kChunkRecords = kChunkBytes / kRecordBytes;
constexpr uint32_t kMaxChunks = 16;
constexpr uint32_t kMarkRows = 16;  // MARK's rows in the object
constexpr uint32_t kMarkRoom = 8;   // new (deviceId, epoch) rows a boot may add to the card's

// ---------------------------------------------------------------------------
// The fold.
// ---------------------------------------------------------------------------
class Fold {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  // The overflow's chunks through `alloc` (null: malloc): the firmware's
  // hook carves them at the top of PSRAM. Nothing else is carved here: the
  // ranges' block and MARK's block are the caller's memory.
  explicit Fold(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~Fold();
  Fold(const Fold&) = delete;
  Fold& operator=(const Fold&) = delete;

  // ---- MARK's room ----
  // The rows MARK holds: the object's 16, or the caller's block.
  uint32_t markCap() const { return markCap_; }
  // A block for a card whose MARK has `cardRows` rows: those and kMarkRoom
  // more. The boot's read gives one when cardRows + kMarkRoom > markCap()
  // (5.14: carved once, in setup, kept for the session); a remount's read
  // never does (MARK past markCap(): a read with no memory).
  static size_t markBlockBytes(uint32_t cardRows);
  // MARK in `mem` (`bytes`, 4-byte aligned) from now on, its rows copied:
  // false when it holds fewer rows than MARK has. The caller keeps the
  // block until dropMarkBlock() (or the fold's end).
  bool markBlock(void* mem, size_t bytes);
  // Back to the object's rows, MARK emptied (the stats off): the caller
  // may free its block after.
  void dropMarkBlock();

  // ---- The load (StatsFile's read) ----
  // Empty, as a fresh start (no stats.bin: every slot on the card folds):
  // HEAD, MARK, the records (the chunks given back), every counter and
  // flag. MARK's block kept.
  void clear();
  // stats.bin's generation, HEAD (its first 128 bytes), MARK's rows in the
  // file's order (batches are fine; a newer minor's longer rows by their
  // prefix: `stride` at least 24). loadMark false: a row out of order (the
  // file is corrupt: 5.4), or more rows than markCap() (noMemory()).
  void loadGeneration(uint32_t generation);
  void loadHead(const uint8_t* bytes);
  bool loadMark(const uint8_t* rows, uint32_t n, uint32_t stride);
  // The base stats.bin names is gone, or another file (5.4): flags bit 1,
  // baseGen, baseRows and baseCrc 0. The rows before it are lost; the
  // totals stay.
  void baseLost();

  // ---- The boot's fold (the play log's Folds) ----
  // The ranges' block a boot needs (after the load): a seq range a log
  // file for each MARK row it may hold (80 B a row), and the root's ACKS
  // rows above the card's MARK (112 B each): 3,072 B at most with 16 rows.
  size_t bootBytes(const cardcontract::msmf::Acks& acks) const;
  // Begun after the load in the caller's `mem` (`bytes`, 8-byte aligned,
  // at least bootBytes(acks)), until endBoot(); each slot of the scan in
  // its order (`file`: logName()'s i); partial() for each log file the scan
  // couldn't read whole; ended when the scan is: the gap (with `acks`)
  // unless partial, HEAD's twice and gap, the caller's block no longer
  // used, the chunks no record needs given back, and a partial scan's or a
  // fold out of memory's overflow dropped (behind: no save may take it).
  // plan() ends it too; after a partial scan (no save) the glue ends it
  // when the journal is ready. A scan begun again (a remount) starts from
  // clear() and the load: the folds made before are dropped. beginBoot
  // false: no memory (no block, or too small): nothing is saved.
  bool beginBoot(const cardcontract::msmf::Acks& acks, void* mem, size_t bytes);
  void bootSlot(const playlog::Slot& s, int file);
  void partial();
  void endBoot();
  bool booting() const { return booting_; }
  // Room for the next `slots` boot folds with no carve (the overflow's
  // chunks): the glue's, before each piece of the boot's scan, in a pass
  // where it may carve. False: no memory (a fold then carves what it needs
  // itself, or stops: noMemory()). Outside the boot's fold: nothing.
  bool reserve(uint32_t slots);

  // ---- The live fold ----
  // A play written this session (Outbox::done(), Written only), at round
  // HEAD.round + 2, a record. True when folded. With the records full (and
  // no chunk's room: a boot's overflow waiting for its save) the slot isn't
  // folded and the session is behind (statsref's one past RECORDS).
  bool liveSlot(const playlog::Slot& s);
  // The session lacks a slot the log holds above the card's MARK (5.14's
  // causes: the records full, a boot overflow's save not landing, a held
  // tmp lost, a section damaged): the records dropped (no save may take
  // them: the next boot folds the log again), the chunks given back, no
  // more folded. MARK and covered() as they were.
  void fallBehind();

  // ---- Questions ----
  // A play slot at or under the card's MARK for its (deviceId, epoch) (the
  // one read at the boot, or the last save's that landed, or a held tmp's
  // once promoted); none: seq 0 only. The trim's and the rotation's.
  bool covered(const playlog::Slot& s) const;
  // RAM's MARK for (deviceId, epoch): the highest seq folded (0: none).
  uint32_t markOf(const uint8_t deviceId[6], uint64_t epoch) const;
  // Folded, or MARK raised (the gap), since the card's copy (or clear()).
  bool dirty() const { return dirty_; }
  // A log file wasn't read whole this session: no save (5.6).
  bool partialScan() const { return partial_; }
  // A fold found no memory (no ranges' block, MARK full, no chunk): nothing
  // more is folded and nothing saved.
  bool noMemory() const { return noMemory_; }
  bool behind() const { return behind_; }
  // The records held (the boot's fold's past kRecords in the chunks), and
  // whether their save is due (kSaveAt or more, not behind).
  uint32_t records() const { return n_; }
  bool due() const { return !behind_ && n_ >= kSaveAt; }
  // This session's counts (since clear()): slots folded (the boot's and
  // live), met twice, the gap counted.
  uint32_t folded() const { return folded_; }
  uint32_t twice() const { return twice_; }
  uint32_t gap() const { return gap_; }
  const Head& head() const { return head_; }
  uint32_t generation() const { return generation_; }
  // The records' row for a path hash, ⊕'d (Ht: the card's row ⊕ this).
  // False: none.
  bool track(uint64_t hash, Track* out) const;
  // The chunks held, their bytes, and the most held at once.
  uint32_t chunks() const { return chunks_; }
  size_t held() const { return held_; }
  size_t peak() const { return peak_; }
  void resetPeak() { peak_ = held_; }

  // ---- The save (StatsFile's commit) ----
  // plan(): what stats.bin is to hold of HEAD and MARK now, RAM unchanged
  // (a save that fails changes nothing): the round (the highest used),
  // sinceDay (set once: the day of the earliest dated play folded this
  // session, not after today; else today; else the root's commitTime,
  // flagged), the generation, MARK's rows (sorted: markRows()). False:
  // nothing may be saved (a partial scan, no memory, behind). The caller
  // counts and writes HOUR, DAYS, RCNT and TRKD: the card's rows merged
  // with the records (Cursor), the retention after; a compaction writes
  // base ⊕ TRKD first and sets the plan's base fields and `compact`.
  // saved(): the file landed: RAM takes the plan's HEAD and generation, the
  // records are emptied (the chunks given back), the card's MARK is RAM's.
  // held(): replace() failed with the tmp whole (5.14's Held): the same,
  // but the card's MARK stays the one before until promoted() (the tmp is
  // stats.bin now): then it is the held tmp's. plan(), the rows and
  // saved() or held() in one loop call: no fold between them.
  struct Plan {
    Head head;
    uint32_t generation = 0;
    uint32_t rows[kSecs] = {};  // HEAD 1 and MARK's: plan()'s; the rest the caller's count
    bool compact = false;
    uint32_t rowsOf(Sec sec) const { return sec == kTrkd && compact ? 0 : rows[sec]; }
    uint32_t bytesOf(Sec sec) const { return sec == kHead ? kHeadBytes : rowsOf(sec) * kStrides[sec]; }
  };
  bool plan(uint64_t nowMs, bool hasCommit, uint64_t commitSec, Plan* out);
  // MARK's rows [from, from + n) as the file has them (24 B each, RAM's
  // seqs), in order after plan(); n (at most maxRows) given.
  uint32_t markRows(uint32_t from, uint8_t* out, uint32_t maxRows) const;
  uint32_t marks() const { return markN_; }
  void saved(const Plan& p);
  void held(const Plan& p);
  void promoted();
  bool heldPending() const { return heldPending_; }

  // Rows as RAM holds them (public for the implementation's helpers and
  // the console's look).
  struct Record {
    uint32_t hashLo, hashHi;  // the path hash
    uint32_t time;            // timeOf(startedAt); 0 undated
    uint32_t playedMs;
    uint8_t what;             // bits 0-1 the kind (kRecPlay, kRecSkip; 0 neither), 2-3 the source, 4 live
    uint8_t reserved[3];
  };
  static constexpr uint8_t kRecPlay = 1, kRecSkip = 2, kRecLive = 0x10;
  struct MarkRow {
    uint8_t dev[6];
    uint16_t pad;
    uint32_t epochLo, epochHi;
    uint32_t seq;   // RAM's
    uint32_t card;  // the card's (the boot's read, the last save's that landed, a held tmp's promoted)
    uint32_t heldSeq;  // RAM's at a held save: the held tmp's
    uint64_t epoch() const { return static_cast<uint64_t>(epochHi) << 32 | epochLo; }
  };
  // MARK's row `i` of marks(), in no set order (plan() sorts them): the
  // console's look (Hm). Null past the last.
  const MarkRow* markRow(uint32_t i) const { return i < markN_ ? &mark_[i] : nullptr; }
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

  // The records in one section's key order, a key's records ⊕'d into one
  // row as the file has it (kStrides[sec] bytes; HOUR, DAYS and RCNT of
  // the dated ones, RCNT and TRKD's plays of plays, TRKD of plays and
  // skips; a play's lastOrd its round). Each run (the object's records,
  // each chunk) sorted in place first, then merged, up to 17: no memory but
  // the cursor's own. No fold while one is in use.
  class Cursor {
  public:
    Cursor(Fold& f, Sec sec);
    // The next row; false: none left.
    bool next(uint8_t* row);

  private:
    bool contributes(const Record& r) const;
    int keyCmp(const Record& a, const Record& b) const;
    void rowOf(const Record& r, uint8_t* out) const;
    Fold& f_;
    Sec sec_;
    uint32_t runs_ = 0;
    uint16_t pos_[kMaxChunks + 1] = {};
  };

private:
  enum class Order : uint8_t { None, Time, DayHash, Hash };
  Record& rec(uint32_t i) { return i < kRecords ? recs_[i] : chunk_[(i - kRecords) / kChunkRecords][(i - kRecords) % kChunkRecords]; }
  uint32_t capacity() const { return kRecords + chunks_ * kChunkRecords; }
  bool carveChunk();
  void giveChunks(uint32_t keep);
  void dropRecords();
  void dropBoot();
  void sortRuns(Order o);
  uint32_t runLength(uint32_t run) const;
  Record* run(uint32_t r) { return r == 0 ? recs_ : chunk_[r - 1]; }
  MarkRow* findMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) const;
  MarkRow* addMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at);
  void sortMark();
  void fold(const playlog::Slot& s, bool live);
  void heldSlot(const playlog::Slot& s, int file);
  uint32_t roundOf(const Record& r) const;

  AllocFn alloc_;
  FreeFn free_;
  size_t held_ = 0;
  size_t peak_ = 0;

  Head head_;
  uint32_t generation_ = 0;
  uint32_t loadedRound_ = 0;  // HEAD's round as loaded: the boot's +1, live +2
  uint32_t used_ = 0;         // the highest round used this session (0: none)
  uint16_t firstPlayDay_ = kUnsetDay;
  bool dirty_ = false;
  bool partial_ = false;
  bool noMemory_ = false;
  bool behind_ = false;
  bool booting_ = false;
  bool heldPending_ = false;
  Order order_ = Order::None;
  uint32_t folded_ = 0, twice_ = 0, gap_ = 0;

  // The records: the object's kRecords, then the chunks' (the boot's overflow).
  uint32_t n_ = 0;
  uint32_t chunks_ = 0;
  Record recs_[kRecords];
  Record* chunk_[kMaxChunks] = {};

  // MARK: the object's rows, or the caller's block.
  MarkRow markIn_[kMarkRows];
  MarkRow* mark_ = markIn_;
  uint32_t markCap_ = kMarkRows, markN_ = 0, markSorted_ = 0;
  mutable uint32_t markHit_ = 0;
  uint8_t lastMark_[14] = {};  // the load's order check

  // The boot's, in the caller's block.
  Range* ranges_ = nullptr;  // kFiles a MARK row
  AckTrack* acks_ = nullptr;
  uint32_t acksN_ = 0;
  uint32_t ackHit_ = 0;
};

}  // namespace statsfold
