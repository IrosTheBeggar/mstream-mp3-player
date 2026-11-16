// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardManifest.h"
#include "PlayLog.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "TagStore.h"

// The listening stats' session (docs/LISTENING-STATS.md 5.4-5.8 and S1's
// rework, 5.14: a fixed resident set): the play log's Folds over the fold
// (StatsFold: HEAD, MARK and the records in the object) and the card's files
// (StatsFile: each save merges the records with stats.bin's sections), with
// the rules one session keeps around them. Portable and host-tested:
// test_stats_store drives it through the real Journal, Tracker and Outbox
// against tools/statsref.py's fixtures and sessions. app/ListeningStats is
// the firmware's side (PSRAM, the card guard, the clocks, the update step's
// safe point, the [listen] lines, the console's H).
//   - The resident set: this object alone (PlayHistory::begin() carves it
//     once, in setup, and never frees it), at most 3,584 B on the device
//     (app/ListeningStats' static_assert); MARK's block beside it for a card
//     with more than 8 MARK rows (the store's, carved by the boot's read).
//     Everything else is carved and given back: a read's or a save's work
//     memory in one call (Memory::alloc), the boot's scan's ranges' block
//     and its overflow's chunks across passes (Memory::top: the firmware's
//     at the top of PSRAM's largest free block, 64 KB kept free).
//   - The read (5.4): begin(), in PlayHistory::begin() before the journal's
//     begin(), with the boot's waits; again after each scanBegan(true) (a
//     remount: one read, no waits, the folds before it dropped at once, the
//     read itself at the scan's first reserve(), a pass that may carve).
//     None while the orderly end flushes: the folds are dropped then, and
//     nothing more is saved or trimmed by them this session.
//   - The boot's fold (5.6, 5.14): begun at the scan's first reserve() (its
//     ranges' block carved at the top), never in setup(); slot() and
//     partial() to the fold, each piece's overflow chunks taken first
//     (reserve()); ended at the boot's save (the ranges' block given back
//     before the save carves its work memory), or when the journal is ready
//     (ready(): a partial scan's, which asks no save). A scan no reserve()
//     came before (the end's flush finishing it) begins it at its first call.
//   - The live fold of each play written (written(): Outbox::Env's): a
//     record; the 97th with the records full puts the session behind.
//   - The saves (5.5, 5.14): the boot's and a rotation's (the journal's), the
//     records' (Kind::Records: due at 64 records, at a pass the play log's
//     write gate lets through, before that pass's slot: turnOf()), and Hs
//     (the console's). Each is refused first by the session's rules: the
//     stats off (unread, newer, no memory, the folds dropped); the boot's and
//     the records' while the end flushes; a rotation's in the end's flush
//     with under kEndSaveMs of its budget left; the card away or another
//     card in the slot; a log file not read whole this session; behind
//     (RAM lacks slots the log holds above the card's MARK: the records
//     full, a save that didn't land with an overflow, a held tmp lost, a
//     section damaged); Hs during the boot's scan or behind the library
//     update's fence. Then the store's own (a held tmp promoted first, the
//     card's copy checked before anything is written: a stats.bin this RAM
//     doesn't stand on refuses this save and every later one). Only the
//     boot's may compact.
//   - The boot's overflow (past 96 records: chunks of 819): held until the
//     boot's save lands or is held; a save that failed is tried again at
//     later passes, tagstore::kBootWaitsMs apart (kBootTries tries in all),
//     and after the last, or at once for a refusal or the end's flush, the
//     session is behind (5.14's rotation-loss hole for a full log whose
//     every try fails: documented, not closed). While chunks are held a
//     track's start waits (overflowHeld()), and while the scan's blocks are
//     held (the ranges' or chunks: scanHolds()) a library update asked
//     waits in Asked (LibraryUpdate::Wait::Stats) and Auto DJ's join doesn't
//     begin. The scan's first piece waits for an update asked (scanWaits()):
//     never both waiting.
//   - covered() and markOf() answer false and 0 while the stats are off:
//     with no MARK known nothing is trimmed, and the seq rule takes the
//     log's, NVS's and ACKS's alone.
// Loop task only.
namespace statsglue {

// Which save.
enum class Kind : uint8_t {
  Boot,      // the boot's scan done, before its trim (Folds::Why::Boot), and its overflow's tries again
  Rotation,  // plays.1.jnl about to leave the log's names (Folds::Why::Rotation)
  Hs,        // the console's (never a compaction)
  Records,   // the records' save, due at statsfold::kSaveAt records (never a compaction)
};
const char* kindName(Kind k);  // "boot", "rotation", "Hs", "records"

// Why a save wasn't asked of the store (None: it was; Behind also for the
// store's Saved::Behind and Saved::Damaged, its report kept).
enum class Refused : uint8_t {
  None,
  Off,       // the stats off this session (unread, newer, no memory), or the folds dropped while ending
  Ending,    // the boot's or the records' save while the orderly end flushes
  Budget,    // a rotation's in the end's flush with under kEndSaveMs of its budget left
  CardAway,  // the card out of the slot, refusing writes, or another card in it
  Partial,   // a log file wasn't read whole this session
  Booting,   // Hs during the boot's scan, or while its overflow's save is tried again
  Fenced,    // Hs behind the library update's fence
  Behind,    // RAM lacks slots the log holds above the card's MARK: no save until the next boot
};
const char* refusedName(Refused r);  // "none", "off", "ending", "budget", "cardAway", "partial", ...

// A rotation in the end's flush saves only with this much of the flush's
// budget left (5.5): otherwise the plays in the file it deletes are lost to
// the stats, and the restart isn't held up.
constexpr uint32_t kEndSaveMs = 1000;
// The boot's save with an overflow: the boot's try and one after each of
// tagstore::kBootWaitsMs (statsref's BOOT_TRIES).
constexpr uint32_t kBootTries = sizeof(tagstore::kBootWaitsMs) / sizeof(tagstore::kBootWaitsMs[0]) + 1;

// What the glue asks of the firmware, and tells it.
class Env {
public:
  // The board's monotonic ms (the end's flush's budget, the overflow's tries).
  virtual uint64_t monoMs() = 0;
  // The card isn't one to write to now: out of the slot, refusing every
  // write, or another card in it (5.5, 5.7: one card's stats never go onto
  // another).
  virtual bool cardAway() { return false; }
  // A save is about to go to the store: `ask` brought up to date (the
  // board's clock, the root's commitTime, the update step's safe point;
  // the glue sets mayCompact after this, for the boot's alone).
  virtual void asking(Kind k, statsfile::SaveAsk* ask) {
    (void)k;
    (void)ask;
  }
  // The boot's scan began (`again`: after a remount). `read`: that remount's
  // read of stats.bin, or nullptr (the boot's own, which begin() made; or
  // none: the folds dropped while the end flushes).
  virtual void began(bool again, const statsfile::Opened* read) {
    (void)again;
    (void)read;
  }
  // The scan couldn't read a log file whole (once for each such file).
  virtual void partialFile() {}
  // A save asked: refused (`r`; `rep` empty but for Refused::Behind from the
  // store), or what the store did.
  virtual void saved(Kind k, Refused r, const statsfile::SaveReport& rep) {
    (void)k;
    (void)r;
    (void)rep;
  }

protected:
  ~Env() = default;
};

// A (deviceId, epoch) the boot's scan met in the log: its highest seq and
// how many valid play slots (the console's Hm, beside MARK and ACKS).
struct LogKey {
  uint8_t dev[6] = {};
  uint64_t epoch = 0;
  uint32_t seq = 0;
  uint32_t slots = 0;
};
constexpr uint32_t kLogKeys = 8;

// A track's row as the stats hold it (the console's Ht): the base's (the
// file stats.bin names, read now) ⊕ stats.bin's TRKD and the records'
// (merged as a save would).
struct TrackLook {
  enum class Base : uint8_t {
    None,      // no base named
    Found,     // its row read
    NotThere,  // the base holds no row for it
    Unread,    // the base couldn't be read (the card failed, or the file isn't the one named)
  };
  Base base = Base::None;
  bool delta = false;   // stats.bin's TRKD or the records hold a row
  bool unread = false;  // stats.bin's TRKD couldn't be read (the card failed, no memory, a CRC)
  statsfold::Track baseRow, deltaRow, row;  // row: base ⊕ delta (hash 0: none)
  uint32_t reads = 0;  // the base's rows read (a binary search)
};

// The last 7 UTC days as the stats hold them (the console's Hd): stats.bin's
// DAYS and HOUR merged with the records', HOUR summed by day.
struct DaysLook {
  bool read = false;  // stats.bin read whole (or none on the card)
  struct Day {
    uint16_t day = 0;
    bool any = false;   // a DAYS row
    uint8_t row[16] = {};  // the DAYS row (statsfold's layout)
    uint32_t hourPlays = 0, hours = 0;
    uint64_t hourMs = 0;
  } d[7];  // the oldest first, d[6] today
};

// One thing of the card's a pass the write gate lets through, the boot's
// scan done (app/PlayHistory::loop()), the first of these:
//   - a save (saveDue(): the boot's overflow tried again, or the records'
//     save), before the pass's slot, so it holds exactly the records taken
//     by then;
//   - a play's slot (Outbox::writeOne(): written, folded live through
//     written()) while one is due (`slotsDue`: the Tracker's pending(), the
//     journal ready, the card not away, the Outbox's head due);
//   - Hs, once no slot is due: its save holds every play the gate held
//     back (S1's device round 1).
enum class Turn : uint8_t { Nothing, Save, Slot, Hs };
Turn turnOf(bool saveDue, uint32_t slotsDue, bool hsAsked);

// Why the boot's scan waits this pass (scanWaits()).
enum class ScanWait : uint8_t {
  None,
  Update,  // its fold not begun, and a library update asked: the scan's blocks never behind its check or its build
  Join,    // Auto DJ's join Working: the fold carves only where the join doesn't
};

// Where the glue's blocks go (null: malloc and free).
struct Memory {
  void* (*alloc)(size_t bytes) = nullptr;  // a read's or a save's work memory (one call), MARK's block (setup)
  void* (*top)(size_t bytes) = nullptr;    // held across passes: the ranges' block, the overflow's chunks (null: alloc)
  void (*release)(void* p) = nullptr;
};

class Glue final : public playlog::Folds {
public:
  Glue(tagstore::Fs& fs, Env& env, const Memory& memory = Memory());
  ~Glue();
  Glue(const Glue&) = delete;
  Glue& operator=(const Glue&) = delete;

  // PlayHistory::begin(), before the journal's begin(): stats.bin read into
  // the fold (5.4), `wait` between the reads a card fault asks for (the
  // boot's: tagstore::kBootWaitsMs), MARK's block carved when the card's
  // MARK needs it. `acks`: the root's, kept by reference for each boot's
  // fold (the root doesn't change in a boot).
  const statsfile::Opened& begin(const cardcontract::msmf::Acks& acks, statsfile::Store::WaitFn wait);
  // The stats run: read (or absent, or corrupt and set aside), with memory
  // for the read, not dropped at the end, and no remount's read waiting.
  bool on() const { return store_.on() && !dropped_ && !rereadPending_; }
  bool dropped() const { return dropped_; }      // a remount in the end's flush dropped the folds
  bool partialSeen() const { return partial_; }  // a log file not read whole this session
  // The boot's scan under way for the stats: a remount's read to make, the
  // boot's fold to begin, folding, or its overflow's save to try again.
  bool booting() const { return rereadPending_ || bootPending_ || fold_.booting() || retrying_; }
  bool ending() const { return ending_; }
  // The next piece of the boot's scan takes the stats' room first
  // (reserve()): a read to make, a fold to begin, or a fold under way.
  bool wantsRoom() const { return rereadPending_ || bootPending_ || (on() && fold_.booting()); }
  bool rereadPending() const { return rereadPending_; }
  bool bootPending() const { return bootPending_; }  // the boot's fold to begin (at the scan's next reserve())
  // Whether the next piece of the boot's scan waits this pass (app/
  // ListeningStats::mayScan()): before the fold begins, while a library
  // update is asked and not over (`updateAsked`); and while Auto DJ's join
  // is Working (`joinWorking`). Once the fold has begun the update waits for
  // its blocks instead (scanHolds()), never both.
  ScanWait scanWaits(bool updateAsked, bool joinWorking) const;
  // The boot's scan holds blocks across passes: its ranges' block, or the
  // overflow's chunks (until its save lands, or behind). A library update
  // asked waits (LibraryUpdate::In::statsWait), and Auto DJ's join doesn't
  // begin.
  bool scanHolds() const { return ranges_ != nullptr || fold_.chunks() > 0; }
  // The overflow's chunks are held: a track's start waits (app/main.cpp's
  // hold: PlayState::Waiting) until they are given back.
  bool overflowHeld() const { return fold_.chunks() > 0; }

  // Before each piece of the boot's scan, in a pass that may carve (Auto
  // DJ's join not Working, the write gate open): a remount's read, the
  // boot's fold begun (its ranges' block), and the overflow's chunks for
  // the next `slots` folds. False: no room (nothing lost: a fold then
  // carves what it needs, or stops: no memory, nothing saved).
  bool reserve(uint32_t slots);
  // The journal ready: a boot's fold that asked no save (a partial scan)
  // ends here.
  void ready();
  // A play's slot written (Outbox::Env::written()): the live fold, a record.
  void written(const playlog::Slot& s);
  // The orderly end flushes (PlayHistory::flush()): from here no read (a
  // remount's still to make: the folds dropped), no boot's or records'
  // save (an overflow waiting for its tries: behind), and a rotation's only
  // while kEndSaveMs of its budget is left before `untilMs` (the board's
  // monotonic ms). notEnding(): back.
  void ending(uint64_t untilMs);
  void notEnding() { ending_ = false; }

  // ---- The saves at a pass the write gate lets through (turnOf()) ----
  // A save is due: the overflow's next try (its wait over), or the records'
  // save (recordsDue()). saveNow(): the one due.
  bool saveDue(bool fenced) { return retryDue() || recordsDue(fenced); }
  bool saveNow(bool fenced);
  // The records' save (5.14): kSaveAt records or more, not behind, the boot's
  // scan and its overflow done, the card not away; not behind the library
  // update's fence (`fenced`) until the records are full (kRecords: then
  // whatever the fence). One that didn't land is due again after a later
  // live record.
  bool recordsDue(bool fenced);
  bool recordsSave();
  // The boot's overflow: its save tried again (Kind::Boot) once its wait is
  // over (retryAtMs()); tries() made of it this scan.
  bool retryDue();
  bool retry();
  bool retrying() const { return retrying_; }
  uint32_t tries() const { return tries_; }
  uint64_t retryAtMs() const { return retryAt_; }
  // The console's Hs: a save now (never a compaction). `fenced`: the
  // library update's fence is up.
  bool hs(bool fenced);

  // The last save asked: which, refused or not, and what the store did.
  Kind lastKind() const { return lastKind_; }
  Refused lastRefused() const { return lastRefused_; }
  const statsfile::SaveReport& last() const { return last_; }
  bool anySave() const { return asked_ > 0; }
  uint32_t savesWritten() const { return written_; }  // this session's that wrote stats.bin
  // The boot's read, and the last read (a remount's).
  const statsfile::Opened& first() const { return first_; }
  const statsfile::Opened& opened() const { return opened_; }
  uint32_t rereads() const { return rereads_; }
  const cardcontract::msmf::Acks& acks() const { return *acks_; }
  // The log's (deviceId, epoch)s the last scan met (Hm); `more`: past kLogKeys.
  uint32_t logKeys() const { return logN_; }
  const LogKey& logKey(uint32_t i) const { return log_[i]; }
  uint32_t logKeysMore() const { return logMore_; }
  // The ranges' block this scan's fold carved (its bytes; 0: none, or not
  // carved: the fold out of memory), held while rangesHeld().
  size_t rangesBytes() const { return rangesBytes_; }
  bool rangesHeld() const { return ranges_ != nullptr; }

  // ---- The console's looks (on the loop; a work memory carved for each) ----
  // A track's row by path hash (Ht): the base read from the card now (a
  // binary search, 24 B a read), stats.bin's TRKD streamed, the records'.
  TrackLook track(uint64_t hash);
  // stats.bin's header and directory as the card has them now (H): false
  // `there` when it isn't there or can't be read.
  struct FileLook {
    bool there = false;   // the card answered, and it is there
    bool unread = false;  // the card failed the question
    bool frame = false;   // its header and directory read (their checks passed)
    uint32_t bytes = 0;
    uint32_t magic = 0;
    uint16_t major = 0, minor = 0;
    uint32_t generation = 0;
    uint32_t rows[statsfold::kSecs] = {};  // each section's (frame)
  };
  FileLook lookFile();
  // The last 7 UTC days to `today` (Hd).
  DaysLook lookDays(uint16_t today);

  statsfold::Fold& fold() { return fold_; }
  const statsfold::Fold& fold() const { return fold_; }
  statsfile::Store& store() { return store_; }
  const statsfile::Store& store() const { return store_; }

  // playlog::Folds
  void scanBegan(bool again) override;
  void slot(const playlog::Slot& s, int file) override;
  void partial() override;
  bool covered(const playlog::Slot& s) override;
  uint32_t markOf(const uint8_t deviceId[6], uint64_t epoch) override;
  bool save(Why why) override;

private:
  uint8_t* carve();
  void release(void* p);
  statsfile::Opened readNow(statsfile::Store::WaitFn wait);
  void settle();     // a remount's read, the boot's fold begun: whatever waits
  void beginBoot();  // the ranges' block at the top, the fold begun in it
  void endBoot();    // the fold's end, its ranges' block given back
  void drop();       // the folds dropped (a remount while the end flushes)
  Refused refusal(Kind k);
  bool refuse(Kind k, Refused r);
  bool commit(Kind k);
  void afterBootTry();
  void noteLog(const playlog::Slot& s);
  using RowFn = void (*)(void* ctx, const uint8_t* row);
  bool lookRows(statsfold::Sec sec, RowFn fn, void* ctx);

  tagstore::Fs& fs_;
  Env& env_;
  Memory mem_;
  statsfold::Fold fold_;
  statsfile::Store store_;
  statsfile::SaveAsk ask_;
  const cardcontract::msmf::Acks* acks_;
  statsfile::Opened first_, opened_;
  statsfile::SaveReport last_;
  Kind lastKind_ = Kind::Boot;
  Refused lastRefused_ = Refused::None;
  bool rereadPending_ = false;  // a remount's read of stats.bin to make (settle())
  bool bootPending_ = false;    // the boot's fold to begin (settle())
  bool partial_ = false;
  bool dropped_ = false;
  bool ending_ = false;
  bool retrying_ = false;  // the boot's overflow: its save tried again at retryAt_
  uint8_t tries_ = 0;      // the boot's save's tries this scan
  uint32_t asked_ = 0, written_ = 0, rereads_ = 0;
  uint32_t recordsTried_ = 0;  // the records when the records' save last didn't land (0: none since)
  uint64_t until_ = 0;
  uint64_t retryAt_ = 0;
  void* ranges_ = nullptr;  // the ranges' block (the boot's fold under way)
  size_t rangesBytes_ = 0;
  LogKey log_[kLogKeys];
  uint32_t logN_ = 0, logMore_ = 0;
};

}  // namespace statsglue
