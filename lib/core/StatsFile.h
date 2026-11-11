// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "StatsFold.h"
#include "TagStore.h"

// The listening stats' files on the card (docs/LISTENING-STATS.md 5.2-5.5,
// 5.7, 5.12): /.player/stats.bin, the aggregate (StatsFold's six sections),
// and the base it names, /.player/stats-<gen>.bin (one section, TRKS).
// tools/statsref.py is the reference: what a boot reads of them, and what a
// save and a compaction write, byte for byte, on every fixture of
// test/fixtures/card/stats (test_stats_file). Portable, host-tested through
// tagstore's file interface (test/support/CutFs.h cuts it at any step).
//
//   - The names (METADATA 2.12.6): stats.bin, stats.tmp, the twins
//     stats.xl1-.xl9, and settle()'s check, StatsCheck: Whole for a frame
//     whose checks and section CRCs hold, and for one whose open says Major
//     or UnknownRequired (a lone tmp of a newer stats firmware is promoted,
//     then read as Newer and left alone: torn, it would be that firmware's
//     whole history deleted); Unknown for a read the card fails, records
//     that are damaged, or no memory for the check (the tmp kept); Torn only
//     for a frame that fails its own checks.
//   - The read (5.4), Store::read(): settle(); stats.bin opened with
//     openRead(), its absence asked of look() (Gone: Absent; Unknown:
//     Unread); Container::open, then each section loaded into the Fold in
//     one pass with its CRC taken as it streams (Stream::crc()), the rows'
//     order checked by the load. The verdicts: Loaded; Absent; Newer (Major,
//     UnknownRequired: stats off, the file left alone); Unread (the card
//     failed, or the file's records are damaged: stats off, no fold, no
//     save, no trim); Corrupt (any other check: read once more after
//     100 ms, then renamed stats.bad, an older one removed first, and
//     counting starts again, as Absent); NoMemory (the load's chunks: stats
//     off). At the boot, a card fault is read again after kBootWaitsMs
//     (100, 300, 1,000 ms) through the caller's wait hook; at a remount (no
//     hook) it is read once, and a corrupt file is left for the next boot's
//     read (Unread), never renamed on one read.
//   - The base check (5.4, step 4), as statsref's load(): the base HEAD names
//     is read whole, its TRKS rows' CRC, order and count, no row kept. It is
//     lost (Fold::baseLost(): flags bit 1, the totals kept) only when the card
//     answers that it isn't there (look() Gone), or when its header reads
//     whole with a CRC other than baseCrc; then nothing is removed. A base
//     shorter than its header, one the card fails (retried at the boot), or
//     one whose header is the one named over rows that fail their CRC, order
//     or count, is Unread, never lost. Checked whole, its neighbours
//     stats-<gen-1>.bin and stats-<gen+1>.bin (a cut compaction's) go.
//   - The save (5.5), Store::save(), the queue's commit
//     (src/app/QueueStore.cpp): a tmp an earlier save holds (written whole,
//     its replace() failed) is promoted first, when its header is still the
//     one written; then settle() with StatsCheck; then the card's copy: the
//     header of stats.bin (none, or its generation and header CRC) against
//     the one this RAM stands on (read, or last written): a change refuses
//     this save and every later one this session; then prepareTmp(); then a
//     compaction, when asked and due (statsfold::compactionDue): base ⊕
//     delta, a merge of two sorted streams (the old base read from the card,
//     its CRC and order checked as it passes), written to
//     stats-<gen+1>.bin, created with FA_CREATE_ALWAYS (a stale file of
//     that name replaced), closed, its size checked; never under the name
//     the card's stats.bin names (a lost base's, a held tmp's: the
//     compaction waits for the next boot); a failure leaves the base as it
//     was and the save goes on; then stats.tmp through a
//     ContainerWriter in 4 KB pieces that end on the file's 4 KB boundaries,
//     closed, its size on the card checked against what was written (else
//     removed: false); then replace(): false keeps the tmp, whole, held for
//     the next save; true: the Fold takes the plan (saved()), and an old
//     base goes.
//   - No allocation here: the ContainerWriter (about 2.1 KB), the save's
//     plan, the frames' Container and the two 4 KB buffers are in the
//     caller's work memory (kWorkBytes, aligned as malloc's; PSRAM on the
//     device), given to each call, never on the loop's stack.
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
// The caller's memory: what would be the loop's stack (the writer, the
// save's plan and section table, a frame's Container and a Stream), then
// two 4 KB buffers. Every call keeps its own frame small: a save's
// (a rotation's, the loop's deepest path) is a few hundred bytes.
// ---------------------------------------------------------------------------
constexpr uint32_t kBufBytes = tagstore::kWriteAlign;  // 4 KB
struct Scratch {
  cardcontract::ContainerWriter writer;
  statsfold::Fold::Plan plan;
  cardcontract::SectionOut sections[statsfold::kSecs];
  cardcontract::Container frame;  // stats.bin's, a base's, the old base's
  cardcontract::Stream stream;    // the old base's rows (a compaction's)
};
constexpr size_t kScratchBytes = (sizeof(Scratch) + 15) & ~static_cast<size_t>(15);
constexpr size_t kWorkBytes = kScratchBytes + 2 * static_cast<size_t>(kBufBytes);

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
  Loaded,    // stats.bin read whole into the Fold, its base checked whole, or lost
  Absent,    // the card says there is no stats.bin: a fresh start
  Corrupt,   // stats.bin failed its checks twice: renamed stats.bad, then a fresh start
  Newer,     // a newer major, or an unknown REQUIRED section: stats off, the file left alone
  Unread,    // the card failed (or the records are damaged, or the base doesn't read whole): stats off
  NoMemory,  // no memory for the load (the Fold's chunks) or no work memory: stats off
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
};

// ---------------------------------------------------------------------------
// The save (5.5).
// ---------------------------------------------------------------------------
struct SaveAsk {
  uint64_t nowMs = 0;        // the board's clock (0: unset)
  bool hasCommit = false;    // the root's commitTime, for sinceDay
  uint64_t commitSec = 0;
  bool mayCompact = false;   // the boot's save (a rotation's and Hs never compact)
  bool playing = false;      // compactionDue()'s
  bool safePoint = true;
};

enum class Saved : uint8_t {
  Wrote,     // stats.bin is this RAM's now
  Nothing,   // nothing new since the card's copy: nothing written (true)
  Off,       // stats off this session, a partial scan, or the fold out of memory
  Changed,   // the card's stats.bin isn't the copy RAM stands on: this and every later save refused
  NoMemory,  // no work memory
  Failed,    // a step failed (or the card failed a question): the card's copy as it was
  Held,      // the tmp written whole, but replace() failed: held, promoted first by the next save
};
const char* savedName(Saved s);
constexpr bool savedOk(Saved s) { return s == Saved::Wrote || s == Saved::Nothing; }

struct SaveReport {
  Saved what = Saved::Off;
  const char* step = "";         // where a Failed save stopped ("settle", "card copy", "tmp", ...)
  bool heldPromoted = false;     // a held tmp promoted first
  bool compacted = false;        // a new base written and named
  bool compactFailed = false;    // a compaction was due and failed (the save went on)
  bool compactSkipped = false;   // due, but its name is the one the card's stats.bin names (5.3)
  uint32_t generation = 0;       // the file written
  uint32_t bytes = 0;            // stats.bin's (stats.tmp's)
  uint32_t baseBytes = 0;        // the new base's
  uint32_t baseRows = 0;
  bool oldBaseRemoved = false;
};

// One card's stats files, for a session: what the card's copy is, a held
// tmp, a change seen.
class Store {
public:
  using WaitFn = void (*)(uint32_t ms);
  explicit Store(tagstore::Fs& fs) : fs_(fs) {}

  // The read into `fold` (cleared first; begin()'d by the caller). `work`:
  // kWorkBytes. `wait`: the boot's (tagstore::kBootWaitsMs between reads the
  // card failed, 100 ms before a corrupt file's second read); nullptr: a
  // remount's, one read. A read starts the session again: the card's copy
  // is the one read, and a change seen before is forgotten.
  Opened read(statsfold::Fold& fold, uint8_t* work, WaitFn wait);

  // The card's copy this RAM stands on: none, or stats.bin's generation and
  // header CRC.
  struct Copy {
    bool present = false;
    uint32_t generation = 0;
    uint32_t headerCrc = 0;
    uint32_t baseGen = 0;  // the base it names (0: none), whole or lost: a compaction never writes that name
  };

  // The save of `fold` (5.5). The glue's own guards come first (the card
  // away or another card, the end's flush for the boot's save). Its step 1
  // in two calls: promote() (a tmp held by an earlier save takes stats.bin's
  // place, when the save would go that far), then save() with what it came
  // to: the glue calls them one after the other, so the promotion's frames
  // (replace()'s and a twin's rename's) are never under the save's (the
  // loop's stack: 11). save(fold, ask, work) makes both calls. (One byte
  // between the two: the held copy itself stays the store's.)
  enum class Held : uint8_t {
    None,      // no tmp held, or the save wouldn't come to its step 1
    Promoted,  // it took stats.bin's place
    Gone,      // it was gone: stats.bin is the held copy, or the one before it
    Fault,     // the card failed a question of it: the save fails ("held tmp")
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

private:
  // What a read found: final; a card fault (read again at the boot); a
  // file that failed its own checks (read once more at the boot).
  enum class Again : uint8_t { Final, CardFault, Corrupt };
  Again readStats(statsfold::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, Opened* o);
  Again checkBase(const statsfold::Head& h, Scratch* sc, uint8_t* a, Opened* o, bool* lost);
  bool quarantine();
  enum class Promote : uint8_t { Promoted, Gone, NotOurs, Fault };
  Promote promoteHeld();
  enum class Hdr : uint8_t { None, Whole, Short, Fault };  // a file's header: none there, read, too short, the card failed
  Hdr headerOf(const char* path, Copy* out);
  bool compact(statsfold::Fold& fold, Scratch* sc, uint8_t* a, uint8_t* b, SaveReport* r, uint32_t* oldGen);

  tagstore::Fs& fs_;
  Copy expect_;
  Copy held_;          // a tmp written whole whose replace() failed
  bool on_ = false;
  bool changed_ = false;
};

}  // namespace statsfile
