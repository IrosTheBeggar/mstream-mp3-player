// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"
#include "ScanScheduler.h"
#include "StatsFold.h"
#include "StatsMap.h"
#include "StatsRank.h"

// The listening stats' visit (docs/LISTENING-STATS.md 9.7's S3a, under the
// owner's "A: nothing held" and the four calls of section 1): what a visit's
// ask leaves for the stats pages, and the arithmetic of its block.
//   - The snapshot (9.7's table): one plain block, the firmware's the Dance
//     tab's 4,096 B tracker scratch in PSRAM (DanceMode, kept for the
//     session), lent while that tab is down (the owner's "Borrow Dance's
//     block", 2026-11-21: the stats own no memory; loanOk()), byte-packed,
//     read with cardcontract's get16/get32. A 160 B header
//     (the grade and flags, the key, the ask's time, today, the live round,
//     each list's rows and total, the span, the 7 days, Not played yet's
//     count and offsets, Most played's window, the version); each list in
//     the room of its cap and 16 (u16 ids at their stride: 4, 4, 4, 4, 6
//     and 5 B); Not played yet's album bits in albumsAZ()'s order, then the
//     A-Z directory (each 64 albums' clear bits, u16). 3,860 B at the
//     spare's 1,706 albums, at most 4,096 B: past 3,216 albums Old
//     favourites and Recently played give up their last rows.
//   - write(), at finishRank() (the copy-out, before the Ranker's room is
//     reused): every kept row straight in, its wanted-set row (u16) in the
//     id field and its facts packed; the header whole, its flags first, so
//     a block an earlier try left part-written is rewritten whole.
//     resolve(), after the walk: each wanted row's track id (orphans and
//     collisions dropped, each list cut to its cap, the totals less the
//     rows dropped). notPlayed(): the map's album bits and the directory
//     (the Full grade's walk, or a lean ask's landing into the held
//     snapshot). Nothing reads the lists before resolve(): the ask serves
//     the snapshot only once it has landed.
//   - layout(): each grade's regions and the block's bytes, the dead
//     regions reused (9.7's grades), the one arithmetic for StatsLists, the
//     console and the heap model.
//   - phaseOf(): the stats pages' phase from the ask's state (9.7's "Phase
//     from why"), pure; stallHolds() and waitStartMoves(): the card
//     worker's 30 s give-up's hold and the wait's start (StatsLists'
//     gateOf() and loop()), pure.
// Portable, host-tested (test_stats_visit). No allocation.
namespace statsvisit {

using statsrank::List;

// ---------------------------------------------------------------------------
// The snapshot (9.7's table)
// ---------------------------------------------------------------------------
constexpr uint32_t kHeaderBytes = 160;
constexpr size_t kBound = 4096;        // the block's bytes at most, to 18,432 albums (bytesFor()): the Dance tab's block
// The loan of the Dance tab's block (DanceMode::lendScratch()): made only
// while the block exists (`exists`: carved at the boot; keyed on it, not on
// the dancer being ready, as a failed skin change clears that and keeps the
// block), the tab is down (`danceActive` false: from setActive(true) on the
// tab's tracker reads into it), and for `n` within it (kBound: 4,096 B; a
// library past 18,432 albums gets none, No room).
constexpr bool loanOk(bool exists, bool danceActive, size_t n) { return exists && !danceActive && n <= kBound; }
constexpr uint16_t kMagic = 0x5653;    // "SV", little-endian
constexpr uint32_t kMaxTracks = 65534;  // a library past it gets no ask: ids are u16 (0xFFFF none)
constexpr uint32_t kMaxAlbums = 0xFFFF;
constexpr uint32_t kStrides[statsrank::kLists] = {4, 4, 4, 4, 6, 5};  // a row's bytes, its id first
constexpr uint16_t kNoRow = 0xFFFF;    // an id field with no wanted row (resolve() drops it)
constexpr uint32_t kCountMax = 0xFFFF;         // a count saturates
constexpr uint32_t kNewFindsCountMax = 0x3FFF;  // ... New finds' (bits 14-15 the source)
constexpr uint32_t kWhenMax = 0xFFFFFD;        // `when` saturates (in 2057)
constexpr uint32_t kWhenEarlier = 0xFFFFFE;    // undated, before this session
constexpr uint32_t kWhenSession = 0xFFFFFF;    // undated, this session (lastOrd the live round)
constexpr uint32_t kNoAlbum = 0xFFFFFFFF;      // albumAt() past the count
// Most played's window (the header's byte; 7.1's PageRef window).
constexpr uint8_t k7Days = 0, k30Days = 1, kAllTime = 2;
// The header's flags.
constexpr uint8_t kFlagDated = 1;      // the clock was set at the ask
constexpr uint8_t kFlagNotPlayed = 2;  // the bits and the directory are there
constexpr uint8_t kFlagResolved = 4;   // the ids are track ids (resolve())
constexpr uint8_t kFlagWritten = 8;    // write() ended

// The grades (9.7): the visit's Full (Not played yet's played set) and
// Lists, and Not played yet's lean ask, with the folders' states or not.
enum class Grade : uint8_t { Full, Lists, Lean, NoFolders };
const char* gradeName(Grade g);  // "Full", "Lists", "Lean", "No folders"

// The rows Old favourites and Recently played each give up for a block of
// `albums`, the fewest that keep it to kBound: none up to 3,216 albums; all
// 216 from 18,361, and past 18,432 the block passes kBound (no library an
// ask takes: 11's ceiling is about 2,500 albums).
uint32_t trimFor(uint32_t albums);
// A list's room (its cap and 16, less the trim) and its first row's offset;
// 0 for a list past the six.
uint32_t roomOf(List l, uint32_t albums);
size_t offsetOf(List l, uint32_t albums);
// The block for `albums`: 3,860 B at 1,706; 0 past kMaxAlbums (no ask).
size_t bytesFor(uint32_t albums);

// A list's row as a page reads it.
struct Row {
  uint16_t id = 0;       // a track id once resolved (before: the wanted set's row, kNoRow none)
  uint16_t count = 0;    // plays (Most played's 7 and 30 days: in the window); 0 for Recently played
  uint8_t src = 0;       // New finds: the source firstAt keeps (statsfold's kYou, kShuffle, kAutoDj)
  uint16_t lastDay = 0;  // Old favourites: lastAt's UTC day
  uint32_t when = 0;     // Recently played: minutes since 2026-01-01, or kWhenSession, kWhenEarlier
};

// The block itself (a plain one, bytesFor(albums()) of it: no constructor
// runs, write() makes every byte). The pages read it through these.
class Snapshot {
public:
  static Snapshot* at(void* block) { return static_cast<Snapshot*>(block); }

  bool valid() const;  // the magic
  bool written() const { return valid() && (flags() & kFlagWritten) != 0; }
  bool resolved() const { return written() && (flags() & kFlagResolved) != 0; }
  bool dated() const { return (flags() & kFlagDated) != 0; }
  bool hasNotPlayed() const { return written() && (flags() & kFlagNotPlayed) != 0; }
  Grade grade() const;
  // The key (the index it was made over) and whether `stamp` and `tracks`
  // are it (a remount's index that isn't releases it: 9.7's "the snapshot's
  // life").
  uint64_t buildStamp() const;
  uint32_t trackCount() const;
  bool matches(uint64_t stamp, uint32_t tracks) const {
    return valid() && buildStamp() == stamp && trackCount() == tracks;
  }
  // The ask's time (a row's: seconds since 2026-01-01 plus 1, so 0 stays
  // undated and a row's minutes are (time - 1) / 60 as `when` reads them;
  // 9.7's table says plain seconds), today (kUnsetDay undated) and the live
  // round.
  uint32_t time() const;
  uint16_t today() const;
  uint32_t liveRound() const;
  // A list's rows and its total (every row offered, less the orphans
  // resolve() dropped among the kept rows).
  uint32_t rows(List l) const;
  uint32_t total(List l) const;
  // DAYS ⊕ the records' first and newest day (O5's span: newest - first +
  // 1, 0 with none), RCNT's oldest, sinceDay (Fold::sinceDayNow()).
  uint16_t firstDay() const;
  uint16_t newestDay() const;
  uint16_t rcntFirstDay() const;
  uint16_t sinceDay() const;
  uint32_t spanDays() const;
  // The 7 days, oldest first (6: today); all 0 undated.
  statsrank::Day day(uint32_t i) const;
  // Not played yet: its albums (the clear bits), the block's albums, and
  // the k-th not-played album's place in albumsAZ() (kNoAlbum past them).
  uint32_t notPlayedCount() const;
  uint32_t albums() const;
  uint32_t bitsOffset() const;
  uint32_t directoryOffset() const;
  uint32_t albumAt(uint32_t k) const;
  // Most played's window (k7Days, k30Days, kAllTime): the UI's one write.
  uint8_t window() const;
  void setWindow(uint8_t w);
  // The ask's version, as write() was given it.
  uint32_t version() const;
  Row rowAt(List l, uint32_t i) const;
  // A list's first row: its id (u16) at every kStrides[l] bytes (the
  // queue's id view, 7.6); null for a list past the six.
  const uint8_t* ids(List l) const;
  size_t bytes() const { return bytesFor(albums()); }

private:
  const uint8_t* b() const { return reinterpret_cast<const uint8_t*>(this); }
  uint8_t* b() { return reinterpret_cast<uint8_t*>(this); }
  uint8_t flags() const;
  uint8_t head_[kHeaderBytes];
};
static_assert(sizeof(Snapshot) == kHeaderBytes, "the snapshot's header");

// What the ask knows besides the rank: the header's key, times and state.
struct Context {
  Grade grade = Grade::Lists;
  uint64_t buildStamp = 0;  // the index's at the turn (the key)
  uint32_t trackCount = 0;
  uint32_t albums = 0;      // its albumCount(): the block is bytesFor(albums)
  uint32_t time = 0;        // now, a row's time (statsfold::timeOf()); 0 undated
  uint32_t liveRound = 0;   // Fold::liveRound()
  uint16_t sinceDay = statsfold::kUnsetDay;  // Fold::sinceDayNow()
  uint8_t window = kAllTime;  // Most played's (a held snapshot's window() on a re-ask)
  uint32_t version = 0;     // the ask's (the pages tell a new ask's rows by it)
};

// The copy-out (finishRank(), the wanted set sealed): the header (its flags
// cleared first), every kept row of `ranker` (each list to its room) with
// its row in `wanted` (kNoRow when it has none) and its facts, the totals,
// the 7 days and the span from `report`; every other byte of the block
// (bytesFor(ctx.albums)) zero. `snap` is a block of `bytes`, as carved.
// False, nothing written: a library past kMaxTracks or kMaxAlbums, or a
// block short of bytesFor(ctx.albums) (a held snapshot re-asked over an
// index with more albums). A backstop in the firmware: StatsLists'
// turnVisit() releases a held snapshot short of the index's albums (the
// loan is kBound, so only past 18,432 albums: no loan then, No room),
// finishVisit() puts the ask back when the index's albums aren't the
// turn's, and StatsLists::loop()'s key check releases a snapshot over another
// index (a remount's: 9.7's "the snapshot's life").
bool write(const statsrank::Ranker& ranker, const statsmap::Wanted& wanted, const statsrank::Report& report,
           const Context& ctx, Snapshot* snap, size_t bytes);
// After the walk: each row's id the wanted row's track id; a row with none
// (kNoRow, an orphan, a collision) dropped, each list compacted and cut to
// its cap, its total less the rows dropped; the rows past it zeroed. False:
// not written, or resolved already.
bool resolve(const statsmap::Wanted& wanted, Snapshot* snap);
// The map's album bits (a Ready walk over `index` with a played set) into
// the snapshot in albumsAZ()'s order, the directory and the count; the
// lists untouched. False, nothing written: not written, the map not Ready
// over `index` or without bits, or the snapshot's key or albums not the
// index's.
bool notPlayed(const statsmap::Map& map, const LibraryIndex& index, Snapshot* snap);

// ---------------------------------------------------------------------------
// The grades (9.7): each region's place in the ask's one block
// ---------------------------------------------------------------------------
struct Region {
  size_t at = 0;     // from the block's start
  size_t bytes = 0;  // 0: none
};
struct Layout {
  Grade grade = Grade::Lists;
  bool dated = false;
  Region head;     // the ask's state (the caller's sizeof(Ask))
  Region work;     // statsfile::kWorkBytes
  Region ranker;   // Ranker::kBytes (Full, Lists)
  Region windows;  // RCNT's window, windowRows (Full and Lists with the clock)
  Region records;  // the records' copy: TRKD's, RCNT's and DAYS' rows (Lean, NoFolders: TRKD's)
  Region played;   // the u40 set, playedRows (Full, Lean, NoFolders)
  Region folders;  // the folders' states (none: NoFolders, or none given)
  Region bits;     // the album bits (Full, Lean, NoFolders)
  Region wanted;   // the wanted set, wantedRows in one region (Full, Lists)
  uint32_t windowRows = 0;
  uint32_t playedRows = 0;
  uint32_t wantedRows = 0;
  size_t bytes = 0;  // the block's
};
// Every region rounded to 16 B, in the order the ask uses them:
//   Full       head | A: the Ranker, the work memory, the records; at
//              finishRank() the wanted set in A's tail, then the folders
//              and bits at A's head over the dead Ranker | PW: RCNT's
//              window; once finished, the u40 set
//   Lists      head | the Ranker, the work memory, RCNT's window, the
//              records; then the folders at its head, the wanted set at its
//              tail
//   Lean       head | the work memory, the TRKD records; then the folders
//              and bits | the u40 set
//   NoFolders  head | the work memory, the TRKD records; then the bits |
//              the u40 set
// `head` the ask's state's bytes (the firmware's sizeof); `rows` the u40
// set's (HEAD's baseRows + TRKD's rows + the records'); `foldersBytes`
// pathhash::Walk::bytesFor(foldersFor(index)) (0: whole paths); `dated`
// the clock set (no window room without it: Windows::add() returns first).
Layout layout(Grade grade, size_t head, uint32_t rows, size_t foldersBytes, uint32_t albums, bool dated);
// The firmware's `head`: sizeof(StatsLists::Ask) (src/app/StatsLists.cpp,
// whose static_assert pins it to this), as the xtensa gdb reads it on the
// core2 and core2-dio ELFs. The heap model's S3 grades
// (test/test_autodj_join) read it, as the host can't build src/app: a
// change to Ask fails the firmware's build until this and the model's
// pinned grades move with it. (test_stats_visit keeps 9.7's 1,488 B.)
constexpr size_t kAskHeadBytes = 1600;

// ---------------------------------------------------------------------------
// The phase (9.7's API: the pages' line)
// ---------------------------------------------------------------------------
enum class Phase : uint8_t { Idle, Working, Waiting, Ready, NoRoom, Unread, NoCard, Capped };
// An ask's end. Room: the rank's (the window's block short), which every
// grade's refusal reads as. Cancelled: by the UI (cancel(), release()).
// CardAway: by the card, away or held (an ask dropped at once while it is,
// or cancelled when it goes), which keeps the page's No card until the
// next ask, the page offering Try again once the card is back (9.7's "the
// card back"; 4.4's No card). Room and CardAway are beyond 9.7's API enum.
enum class Outcome : uint8_t { None, Ok, NoRoom, NoBlock, Changed, Io, Damaged, Base, Cancelled, Room, CardAway };
const char* phaseName(Phase p);
const char* outcomeName(Outcome o);
Outcome outcomeOf(statsrank::Outcome o);
// S2's gate bits (StatsLists::turnWanted()'s why: one at a time, the first
// that holds in this order), and S3a's own last: kWhyRest, the card
// worker's tag scan holding its memory (its View or its work memory:
// CardJobs::scanHeld(), not CardJobs::scanStuck(): app/CardTasks; the
// owner's "Only while held", 2026-11-21), which only the visit's and lean
// asks wait out (S2's asks don't).
constexpr uint8_t kWhyOff = 1, kWhyBoot = 2, kWhyTmp = 4, kWhyUpdate = 8, kWhyCard = 16, kWhyJoin = 32,
                  kWhyWorker = 64, kWhyRest = 128;
constexpr uint32_t kWaitingMs = 3000;  // held this long by a wait: Waiting
// The phase of an ask made (Asked, under way, or ended with `outcome`; with
// none made the page's phase is Idle, the caller's). `why`: the gate's bits
// holding it; `wait`: the Rank lane's; `waitedMs`: how long a wait has held
// it; `capped`: the lit cap reached (cap on); `served`: the landed snapshot
// still held (StatsLists::snapshot() non-null). In order:
//   the stats off (1): Idle; the card away or held (16), or an ask it
//   ended (CardAway): NoCard;
//   landed: Ready while its snapshot is served, else Idle (released since:
//   release(), a console ask's own, a remount's key, Hu0); NoRoom, NoBlock
//   or Room: NoRoom; Io, Changed, Damaged, Base: Unread (nothing else);
//   the cap: Capped; Cancelled: Idle;
//   the boot's scan (2): Working, however long (4.4's Folding, "Counting
//   plays…"; S2 gives it before every later gate);
//   4, 8, 32, 64 or 128 (the tag scan's memory held), or a Rank-lane wait:
//   Working, then Waiting from kWaitingMs (no count);
//   else (carved, ranking, walking): Working.
// A wait met after the take, its block given back (V4's hold bounds), is
// the gate's wait again, `waitedMs` from its start: Working, then Waiting
// from kWaitingMs, as 9.7's V3 bullet and its tests have it, so a tap's
// 0.5 s wait doesn't flash "Waiting for the card…" (the API's phase list
// says Waiting at once; 4.4's Waiting is "held past 3 s").
Phase phaseOf(uint8_t why, Outcome outcome, ScanScheduler::Wait wait, uint32_t waitedMs, bool capped, bool served);

// The wait's start in Asked (StatsLists' queuedMs_: phaseOf()'s waitedMs)
// and the card worker's give-up. An S3a ask the worker took no step of in
// 30 s is given back to Asked, stalled: held kStallMs from that give-up
// (stallHolds(): gateOf()'s kWhyWorker), then taken again; the stall
// clears at the take. loop()'s pass moves the start to this pass when
// nothing holds the ask (`why` 0, no Rank-lane wait), but never while it
// is stalled: the pass that saw the stall's 30 s run out would restart
// them, and the turn's gate in the same pass (the same nowMs) would hold
// the ask again, every 30 s for good (the check of the review's fixes).
constexpr uint32_t kStallMs = 30000;
// The ms from the wait's start to nowMs, millis() wrapping, and 0 for a
// start a little after nowMs: a console ask (and Hu's re-ask at a fence)
// stamps its start with millis() after the loop's pass took its nowMs, so
// that pass's loop() and phase read a start up to a pass ahead (the device
// pass, run 3a-6: "held 4294967294 ms", and a wait but the boot's scan's
// would have read Waiting at once, not from 3 s). A start more than 2^31 ms
// ahead reads as one behind, as millis() wrapping makes it.
inline uint32_t elapsedMs(uint32_t nowMs, uint32_t startMs) {
  const uint32_t d = nowMs - startMs;
  return d > 0x7FFFFFFFu ? 0 : d;
}
inline bool stallHolds(bool stalled, uint32_t nowMs, uint32_t startMs) {
  return stalled && elapsedMs(nowMs, startMs) < kStallMs;
}
inline bool waitStartMoves(uint8_t why, ScanScheduler::Wait wait, bool stalled) {
  return why == 0 && wait == ScanScheduler::Wait::None && !stalled;
}

}  // namespace statsvisit
