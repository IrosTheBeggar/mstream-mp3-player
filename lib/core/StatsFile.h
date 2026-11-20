// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "StatsFold.h"
#include "TagStore.h"

// The listening stats' files on the card (docs/LISTENING-STATS.md 5.2-5.5,
// 5.7, 5.12, and S1's rework, 5.14): /.player/stats.bin, the aggregate
// (StatsFold's six sections), and the base it names,
// /.player/stats-<gen>.bin (one section, TRKS). RAM keeps HEAD, MARK and
// the records (statsfold::Fold); the rest lives in stats.bin alone, and each
// save merges it there. tools/statsref.py is the reference: what a boot
// reads of them, and what a save and a compaction write, byte for byte, on
// every fixture of test/fixtures/card/stats (test_stats_file). Portable,
// host-tested through tagstore's file interface (test/support/CutFs.h cuts
// it at any step).
//
//   - The names (METADATA 2.12.6): stats.bin, stats.tmp, the twins
//     stats.xl1-.xl9, and settle()'s check, StatsCheck: Whole for a frame
//     whose checks and section CRCs hold, and for one whose open says Major
//     or UnknownRequired (a lone tmp of a newer stats firmware is promoted,
//     then read as Newer and left alone: torn, it would be that firmware's
//     whole history deleted); Unknown for a read the card fails, records
//     that are damaged, or no memory for the check (the tmp kept); Torn only
//     for a frame that fails its own checks.
//   - The read (5.4, 5.14), Store::read(): settle(); stats.bin opened with
//     openRead(), its absence asked of look() (Gone: Absent; Unknown:
//     Unread); Container::open, then stats.bin read whole in one pass, each
//     section's CRC taken as it streams (Stream::crc()): HEAD and MARK
//     loaded into the Fold, HOUR to TRKD only checked (their rows' order by
//     statsfold::compareRows()), none kept. Every CRC is taken before the
//     order is judged, as statsref's read: SectionCrc, then Shape (HEAD
//     under 128 bytes), then Order. The verdicts: Loaded; Absent; Newer
//     (Major, UnknownRequired: stats off, the file left alone); Unread (the
//     card failed, or the file's records are damaged: stats off, no fold, no
//     save, no trim); Corrupt (any other check: read once more after
//     100 ms, then renamed stats.bad, an older one removed first, and
//     counting starts again, as Absent); NoMemory (no work memory, or MARK's
//     rows past the room: stats off). At the boot (a wait hook), a card fault
//     is read again after kBootWaitsMs (100, 300, 1,000 ms), and a card
//     whose MARK has more rows than the object holds with kMarkRoom more
//     gets MARK's block (Fold::markBlockBytes(): its rows and 8 more),
//     carved through the store's hook once and kept for the session; at a
//     remount (no hook) it is read once, nothing is carved (MARK past
//     markCap(): NoMemory, the stats off for the session), and a corrupt
//     file is left for the next boot's read (Unread), never renamed on one
//     read.
//   - The base check (5.4, step 4), as statsref's load(): the base HEAD names
//     is read whole, its TRKS rows' CRC, order and count, no row kept. It is
//     lost (Fold::baseLost(): flags bit 1, the totals kept) only when the card
//     answers that it isn't there (look() Gone), or when its header reads
//     whole with a CRC other than baseCrc; then nothing is removed. A base
//     shorter than its header, one the card fails (retried at the boot), or
//     one whose header is the one named over rows that fail their CRC, order
//     or count, is Unread, never lost. Checked whole, its neighbours
//     stats-<gen-1>.bin and stats-<gen+1>.bin (a cut compaction's) go.
//   - The save (5.5, 5.14), Store::save(), the queue's commit
//     (src/app/QueueStore.cpp): a tmp an earlier save holds (written whole,
//     its replace() failed) is promoted first (promote(): its header still
//     the one written; the Fold's card MARK then the held copy's), or, lost
//     or no longer ours, the session is behind; nothing new: nothing
//     written; then settle() with StatsCheck; then the card's copy: the
//     header of stats.bin (none, or its generation and header CRC) against
//     the one this RAM stands on (read, or last written): a change refuses
//     this save and every later one this session; then prepareTmp(). Then
//     the streaming merge, nothing waiting in RAM but the records:
//       1. the counting pass: stats.bin's sections read in order, each
//          section's CRC taken as it streams (HEAD's and MARK's too), HOUR,
//          DAYS, RCNT and TRKD merged with the records (a Fold::Cursor each:
//          by compareRows(), equal keys ⊕'d by addRow(), the card's the
//          older), counting the rows each keeps after its cut: HOUR's newest
//          840, DAYS' newest 400, RCNT's 32 days back from the earlier of
//          today and DAYS' newest merged day (nothing by date with the clock
//          unset), then whole days, oldest first, while the rows from the
//          oldest on number more than 4,096 (RCNT read again for that, only
//          when its count passes 4,096); TRKD never cut;
//       2. a compaction, when asked and due (statsfold::compactionDue on the
//          merged TRKD's count): base ⊕ TRKD ⊕ the records, three sorted
//          streams (the old base and stats.bin's TRKD read from the card,
//          their CRCs and order checked as they pass), written to
//          stats-<gen+1>.bin, created with FA_CREATE_ALWAYS (a stale file of
//          that name replaced), closed, its size checked; never under the
//          name the card's stats.bin names (a lost base's: the compaction
//          waits for the next boot); a failure leaves the base as it was and
//          the save goes on; the new stats.bin's TRKD then empty;
//       3. the writing pass: stats.tmp through a ContainerWriter in 4 KB
//          pieces that end on the file's 4 KB boundaries: HEAD and MARK from
//          RAM (the plan), HOUR, DAYS, RCNT and TRKD merged again from the
//          card (their CRCs again) with the records, each section's rows as
//          the counting pass found them; closed, its size on the card
//          checked against what was written (else removed: false);
//     then replace(): false keeps the tmp, whole, held for the next save
//     (the Fold takes the plan, the card's MARK the one before:
//     Fold::held()); true: the Fold takes the plan (saved()), and an old
//     base goes. A read the card fails in either pass (Io) is a save that
//     failed, tried again: the tmp removed, the card's file untouched. A
//     section whose CRC (or rows' order) fails with the header the one RAM
//     stands on is the card's copy damaged under the session (Damaged): the
//     session behind (Fold::fallBehind(): no more saves; the next boot
//     reads the file Corrupt), never healed from RAM, which no longer holds
//     the data.
//   - No allocation here but MARK's block: the ContainerWriter (about
//     2.1 KB), the save's plan, the frames' Containers, the row readers and
//     the cursor are in the caller's work memory (kWorkBytes, aligned as
//     malloc's; PSRAM on the device), with a 4 KB buffer for the writer and
//     two of 2 KB for the reads, given to each call, never on the loop's
//     stack. A save opens two files at once (stats.bin read, stats.tmp
//     written), a compaction three (the old base, stats.bin, the new base).
namespace statsfile {

// ---------------------------------------------------------------------------
// The names and the frame.
// ---------------------------------------------------------------------------
constexpr const char* kStatsBin = "/.player/stats.bin";
constexpr const char* kStatsTmp = "/.player/stats.tmp";
constexpr const char* kStatsBad = "/.player/stats.bad";
constexpr tagstore::Names kStatsNames{kStatsBin, kStatsTmp, "/.player/stats"};
constexpr uint32_t kMagic = cardcontract::fourcc("MPST");
constexpr uint16_t kMajor = 1;
constexpr uint16_t kMinor = 0;
constexpr uint32_t kHeaderBytes = 40;  // the common header alone
constexpr uint32_t kBaseRowsAt = kHeaderBytes + cardcontract::kDirEntryBytes;  // a base's TRKS (72)
// stats.bin's six sections (statsfold's Sec order), every one REQUIRED;
// the base's TRKS.
extern const cardcontract::FormatSpec kStatsSpec;
extern const cardcontract::FormatSpec kBaseSpec;
// "/.player/stats-<gen>.bin", the generation in 8 lowercase hex digits.
constexpr size_t kNameCap = 32;
bool baseName(uint32_t gen, char* out, size_t cap);
// A base file's frame for `rows` TRKS rows whose bytes have CRC `rowsCrc`:
// the 72 bytes before them (the header, its CRC last, and the directory's
// one entry), as ContainerWriter lays them out. `headerCrc`: the header's.
void baseFrame(uint32_t gen, uint32_t rows, uint32_t rowsCrc, uint8_t out[kBaseRowsAt], uint32_t* headerCrc);

// ---------------------------------------------------------------------------
// One array section of a card file, read front to back by its rows'
// first-minor prefix (a newer minor's longer rows: 5.12), its CRC taken as
// the rows pass and their order checked (statsfold::compareRows(); a
// base's TRKS by TRKD's key, the hash). The save's merges read the card's
// rows through it.
// ---------------------------------------------------------------------------
class CardRows {
public:
  // `s` not present (type 0: no stats.bin): no rows. `buf`: a read
  // stream's (kReadBytes).
  void begin(cardcontract::Source* src, const cardcontract::Section& s, statsfold::Sec sec, uint8_t* buf,
             uint32_t bytes);
  // The next row's statsfold::kStrides[sec] bytes into `row`. False at the
  // end, or at a read the card failed or a row out of order (end() says).
  bool next(uint8_t* row);
  // Once next() said false (or to stop early): the rest passed, and Ok
  // (every byte passed, the CRC the directory's), Io (a read the card
  // failed), SectionCrc, or Order.
  cardcontract::Why end();
  uint32_t rows() const { return n_; }  // the rows passed

private:
  cardcontract::Stream st_;
  uint32_t left_ = 0, stride_ = 0, crc_ = 0, n_ = 0;
  statsfold::Sec sec_ = statsfold::kTrkd;
  bool present_ = false, order_ = true, failed_ = false;
  uint8_t last_[statsfold::kTrackBytes] = {};
};

// ---------------------------------------------------------------------------
// The caller's memory: what would be the loop's stack (the writer, the
// save's plan and section table, the frames, the row readers, a cursor and
// the merge's rows), then a 4 KB buffer for the writer's pieces and two of
// 2 KB for the reads. Every call keeps its own frame small: a save's (a
// rotation's, the loop's deepest path) is a few hundred bytes.
// ---------------------------------------------------------------------------
constexpr uint32_t kBufBytes = tagstore::kWriteAlign;  // 4 KB: the writer's (a save's tmp, a compaction's base)
constexpr uint32_t kReadBytes = kBufBytes / 2;          // 2 KB: a read stream's (two at once: a compaction's)
struct Scratch {
  cardcontract::ContainerWriter writer;
  statsfold::Fold::Plan plan;
  cardcontract::SectionOut sections[statsfold::kSecs];
  cardcontract::Container frame;  // stats.bin's (the read's, the save's card copy)
  cardcontract::Container base;   // a base's (the read's check, the compaction's old base)
  CardRows card;                  // a section of stats.bin
  CardRows old;                   // the old base's TRKS (a compaction's)
  // The counting pass's cuts: the merged rows HOUR and DAYS drop first;
  // RCNT's first day kept (0x10000: none).
  uint32_t drop[statsfold::kSecs];
  uint32_t rcntFrom;
  uint8_t row[4][statsfold::kTrackBytes];  // the merge's: the card's, the records', the merged, the base's
  alignas(statsfold::Fold::Cursor) unsigned char cursor[sizeof(statsfold::Fold::Cursor)];
};
constexpr size_t kScratchBytes = (sizeof(Scratch) + 15) & ~static_cast<size_t>(15);
constexpr size_t kWorkBytes = kScratchBytes + kBufBytes + 2 * static_cast<size_t>(kReadBytes);

// ---------------------------------------------------------------------------
// settle()'s check of a lone stats.tmp.
// ---------------------------------------------------------------------------
class StatsCheck : public tagstore::TmpCheck {
public:
  // `buf` (null: no memory: Unknown) streams the sections.
  StatsCheck(uint8_t* buf, uint32_t bytes) : buf_(buf), bytes_(bytes) {}
  Verdict check(tagstore::Fs& fs, const char* tmp) override;
  // The last check's Why (Ok for a whole frame; Io for a read the card
  // failed; FatRecords for damaged records).
  cardcontract::Why why() const { return why_; }

private:
  uint8_t* buf_;
  uint32_t bytes_;
  cardcontract::Why why_ = cardcontract::Why::Ok;
};

// The frame of `src` as a file of `spec`, and every known section's CRC,
// through `buf`.
cardcontract::Why checkWhole(cardcontract::Source& src, const cardcontract::FormatSpec& spec, uint8_t* buf,
                             uint32_t bytes);

// ---------------------------------------------------------------------------
// The read (5.4).
// ---------------------------------------------------------------------------
enum class Read : uint8_t {
  Loaded,    // stats.bin read whole (HEAD and MARK into the Fold), its base checked whole, or lost
  Absent,    // the card says there is no stats.bin: a fresh start
  Corrupt,   // stats.bin failed its checks twice: renamed stats.bad, then a fresh start
  Newer,     // a newer major, or an unknown REQUIRED section: stats off, the file left alone
  Unread,    // the card failed (or the records are damaged, or the base doesn't read whole): stats off
  NoMemory,  // no work memory, or MARK past the room (no block for it): stats off
};
const char* readName(Read r);  // "loaded", "absent", "corrupt", "newer", "unread", "noMemory"
// The stats run this session (fold, save, trim) after these.
constexpr bool statsOn(Read r) { return r == Read::Loaded || r == Read::Absent || r == Read::Corrupt; }

struct Opened {
  Read read = Read::Unread;
  // stats.bin's Why (Corrupt: the last read's; Unread: Io, FatRecords), or
  // the base's when `inBase`.
  cardcontract::Why why = cardcontract::Why::Ok;
  bool inBase = false;
  tagstore::Settled settled;     // stats.tmp's fate, and the twins left (a disk check wanted)
  bool heldPromoted = false;     // a tmp this session held, promoted first (a remount's read)
  uint32_t reads = 0;            // stats.bin's reads (the boot's waits, a corrupt file's second)
  uint32_t baseReads = 0;        // the base's
  uint32_t bytes = 0;            // stats.bin's
  uint32_t generation = 0;       // stats.bin's (Loaded)
  uint32_t headerCrc = 0;        // stats.bin's (Loaded)
  uint32_t baseBytes = 0;        // the base's (Loaded with a base)
  bool baseLost = false;         // the base gone or another file: flags bit 1 now
  bool quarantined = false;      // stats.bin renamed stats.bad (or to a twin, when one shared its chain)
  uint32_t removed = 0;          // neighbouring bases removed
  uint32_t marks = 0;            // MARK's rows on the card (the last read's directory)
  uint32_t markBytes = 0;        // MARK's block this read carved (0: none: the object's rows, or the block before)
};
// Unread because the card failed a read (Io: stats.bin's, its tmp's, a held
// tmp's promotion, the base's; or the base's rows failing their CRC, a card
// fault too): what the boot reads again after kBootWaitsMs. Never damaged
// records (FatRecords: no read again changes them), a newer or corrupt
// file, or no memory.
inline bool cardFault(const Opened& o) {
  return o.read == Read::Unread &&
         (o.why == cardcontract::Why::Io || (o.inBase && o.why == cardcontract::Why::SectionCrc));
}

// ---------------------------------------------------------------------------
// The save (5.5, 5.14).
// ---------------------------------------------------------------------------
struct SaveAsk {
  uint64_t nowMs = 0;        // the board's clock (0: unset)
  bool hasCommit = false;    // the root's commitTime, for sinceDay
  uint64_t commitSec = 0;
  bool mayCompact = false;   // the boot's save (the records', a rotation's and Hs never compact)
  bool playing = false;      // compactionDue()'s
  bool safePoint = true;
};

enum class Saved : uint8_t {
  Wrote,     // stats.bin is this RAM's now
  Nothing,   // nothing new since the card's copy (a held tmp promoted, or none): nothing written (true)
  Off,       // stats off this session, a partial scan, or the fold out of memory
  Changed,   // the card's stats.bin isn't the copy RAM stands on: this and every later save refused
  NoMemory,  // no work memory
  Failed,    // a step failed (or the card failed a question or a read): the card's copy as it was
  Held,      // the tmp written whole, but replace() failed: held, promoted first by the next save
  Behind,    // the session lacks slots the log holds above the card's MARK (the records full, a held tmp
             // lost or no longer ours, a section damaged before): no save until the next boot
  Damaged,   // a section of the card's stats.bin fails its CRC (or its rows' order) under the header RAM
             // stands on: the session behind from now, the file as it was (the next boot reads it Corrupt)
};
const char* savedName(Saved s);
constexpr bool savedOk(Saved s) { return s == Saved::Wrote || s == Saved::Nothing; }

struct SaveReport {
  Saved what = Saved::Off;
  const char* step = "";         // where a Failed save stopped ("settle", "card copy", "read", "tmp", ...)
  uint32_t section = 0;          // Damaged: the section's type (fourcc; 0: stats.bin's frame)
  bool heldPromoted = false;     // a held tmp promoted first
  bool compacted = false;        // a new base written and named
  bool compactFailed = false;    // a compaction was due and failed (the save went on)
  bool compactSkipped = false;   // due, but its name is the one the card's stats.bin names (5.3)
  uint32_t generation = 0;       // the file written
  uint32_t bytes = 0;            // stats.bin's (stats.tmp's)
  uint32_t baseBytes = 0;        // the new base's
  uint32_t baseRows = 0;
  bool oldBaseRemoved = false;
  uint32_t rows[statsfold::kSecs] = {};  // the file's rows (HEAD 1), as written
};

// One card's stats files, for a session: what the card's copy is, a held
// tmp, a change seen, and MARK's block.
class Store {
public:
  using WaitFn = void (*)(uint32_t ms);
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  // `alloc`, `release`: MARK's block (null: malloc and free; the
  // firmware's: the top of PSRAM's largest free block in setup, kept for
  // the session: the glue's Memory::keep).
  explicit Store(tagstore::Fs& fs, AllocFn alloc = nullptr, FreeFn release = nullptr);
  // MARK's block given back: the Fold must use MARK no more (the glue holds
  // both for the session).
  ~Store();
  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // The read into `fold` (cleared first). `work`: kWorkBytes. `wait`: the
  // boot's (tagstore::kBootWaitsMs between reads the card failed, 100 ms
  // before a corrupt file's second read; MARK's block carved when the card's
  // MARK needs it); nullptr: a remount's, one read, nothing carved. A read
  // starts the session again: the card's copy is the one read, and a change
  // seen before is forgotten.
  Opened read(statsfold::Fold& fold, uint8_t* work, WaitFn wait);

  // The card's copy this RAM stands on: none, or stats.bin's generation and
  // header CRC.
  struct Copy {
    bool present = false;
    uint32_t generation = 0;
    uint32_t headerCrc = 0;
    uint32_t baseGen = 0;  // the base it names (0: none), whole or lost: a compaction never writes that name
  };

  // The save of `fold` (5.5, 5.14). The glue's own guards come first (the
  // card away or another card, the end's flush, the fence, the budget). Its
  // step 1 in two calls: promote() (a tmp held by an earlier save takes
  // stats.bin's place, whatever is new since), then save() with what it
  // came to: the glue calls them one after the other, so the promotion's
  // frames (replace()'s and a twin's rename's) are never under the save's
  // (the loop's stack: 11). save(fold, ask, work) makes both calls.
  enum class Held : uint8_t {
    None,      // no tmp held, or the save wouldn't come to its step 1 (off, changed, partial, behind)
    Promoted,  // it took stats.bin's place (or was gone with stats.bin its copy): the card's MARK the held copy's
    Behind,    // lost (gone with stats.bin the copy before it) or no longer ours: the session behind
    Changed,   // gone with stats.bin another copy: this save and every later one refused
    Fault,     // the card failed a question of it: the save fails ("held tmp"), the tmp still held
  };
  Held promote(statsfold::Fold& fold);
  SaveReport save(statsfold::Fold& fold, const SaveAsk& ask, uint8_t* work, Held held);
  SaveReport save(statsfold::Fold& fold, const SaveAsk& ask, uint8_t* work) {
    const Held held = promote(fold);
    return save(fold, ask, work, held);
  }
  const Copy& cardCopy() const { return expect_; }
  bool on() const { return on_; }
  bool changed() const { return changed_; }
  bool held() const { return held_.present; }
  // The tmp held (written whole, its replace() failed): its generation and
  // header CRC (present false: none held).
  const Copy& heldCopy() const { return held_; }
  // MARK's block, as the boot's read carved it (0: none).
  size_t markBytes() const { return markBytes_; }

private:
  // What a read found: final; a card fault (read again at the boot); a
  // file that failed its own checks (read once more at the boot).
  enum class Again : uint8_t { Final, CardFault, Corrupt };
  Again readStats(statsfold::Fold& fold, Scratch* sc, uint8_t* buf, uint8_t* stage, bool carve, Opened* o);
  Again checkBase(const statsfold::Head& h, Scratch* sc, uint8_t* buf, Opened* o, bool* lost);
  bool quarantine();
  enum class Promote : uint8_t { Promoted, Gone, NotOurs, Fault };
  Promote promoteHeld();
  enum class Hdr : uint8_t { None, Whole, Short, Fault };  // a file's header: none there, read, too short, the card failed
  Hdr headerOf(const char* path, Copy* out);
  // The save's passes over one section (the card's rows ⊕ the records'):
  // counted, RCNT's whole-day cut found, or written.
  enum class Pass : uint8_t { Count, Cut, Write };
  struct Tally {
    uint32_t rows = 0;      // Count: the rows from `from` on; Write: the rows written
    uint32_t lastDay = 0;   // Count: the last row's day (DAYS, RCNT)
    uint32_t cutDay = 0;    // Cut: the first day kept (0x10000: none)
    uint32_t cutRows = 0;   // Cut: the rows from it on
    bool more = false;      // Write: a row past the count (the card's rows changed)
    bool wrote = true;      // Write: false, the writer failed (the tmp's write)
  };
  cardcontract::Why pass(statsfold::Fold& fold, Scratch* sc, cardcontract::Source* bin, statsfold::Sec sec, Pass how,
                         uint32_t from, uint32_t total, uint8_t* buf, Tally* t);
  cardcontract::Why count(statsfold::Fold& fold, Scratch* sc, cardcontract::Source* bin, const SaveAsk& ask,
                          uint8_t* buf, uint32_t* failedSec);
  bool compact(statsfold::Fold& fold, Scratch* sc, cardcontract::Source* bin, uint8_t* w, uint8_t* r1, uint8_t* r2,
               SaveReport* r, uint32_t* oldGen);
  // The writing pass's end: whole; the tmp's write failed; the card failed
  // a read; the rows not as counted; a section damaged (*failedSec).
  enum class Step : uint8_t { Ok, Tmp, Read, Rows, Damaged };
  Step write(statsfold::Fold& fold, Scratch* sc, cardcontract::Source* bin, tagstore::File& tmp, uint8_t* w,
             uint8_t* r1, uint8_t* r2, uint32_t* failedSec, uint32_t* fileBytes, uint32_t* headerCrc);

  tagstore::Fs& fs_;
  AllocFn alloc_;
  FreeFn free_;
  void* mark_ = nullptr;  // MARK's block (the boot's read's)
  size_t markBytes_ = 0;
  Copy expect_;
  Copy held_;          // a tmp written whole whose replace() failed
  bool on_ = false;
  bool changed_ = false;
};

}  // namespace statsfile
