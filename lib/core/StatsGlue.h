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

// The listening stats' session (docs/LISTENING-STATS.md 5.4-5.8, 9.5's C9):
// the play log's Folds over the fold in RAM (StatsFold) and the card's files
// (StatsFile), with the rules one session keeps around them. Portable and
// host-tested: test_stats_store drives it through the real Journal, Tracker
// and Outbox against tools/statsref.py's fixtures and sessions.
// app/ListeningStats is the firmware's side (PSRAM, the card guard, the
// clocks, the update step's safe point, the [listen] lines, the console's H).
//   - The read (5.4): begin(), in PlayHistory::begin() before the journal's
//     begin(), with the boot's waits; again after each scanBegan(true) (a
//     remount: one read, no waits, the folds before it dropped at once, the
//     read itself at the scan's first reserve(), a pass that may carve).
//     None while the orderly end flushes: the folds are dropped then, and
//     nothing more is saved or trimmed by them this session. A read that
//     leaves the stats off gives every block of the fold back.
//   - The boot's fold (5.6): begun (its ranges, ACKS rows and indexes
//     carved) at the scan's first reserve(), not at scanBegan(): in the
//     loop, in a pass that may carve, never in setup(); slot() and
//     partial() to the fold, its room taken before each piece of the scan
//     (reserve()), ended when the journal is ready (ready(): a partial
//     scan's, which asks no save). A scan no reserve() came before (the
//     end's flush finishing it) begins it at its first call.
//   - The live fold of each play written (written(): Outbox::Env's).
//   - The saves (5.5): the boot's and a rotation's (the journal's), and Hs
//     (the console's, the rotation's path: never a compaction). Each is
//     refused first by the session's rules: the stats off (unread, newer,
//     no memory, the folds dropped); the boot's while the end flushes; a
//     rotation's in the end's flush with under kEndSaveMs of its budget
//     left; the card away or another card in the slot; a log file not read
//     whole this session; Hs during the boot's scan or behind the library
//     update's fence. Then the store's own (a held tmp promoted first, the
//     card's copy checked before anything is written: a stats.bin this RAM
//     doesn't stand on refuses this save and every later one).
//   - covered() and markOf() answer false and 0 while the stats are off:
//     with no MARK known nothing is trimmed, and the seq rule takes the
//     log's, NVS's and ACKS's alone.
//   - The work memory (statsfile::kWorkBytes) is carved for each read and
//     save through the allocator hook and given back after it, as the heap
//     model takes it (6.7).
//   - A library update's give-back (6.7, the owner's decision of
//     2026-11-11), giveBack(): at the update's give-back point (its memory
//     check's hook, LibraryUpdate::setGiveBack(), chained after Auto DJ's;
//     and its fence, after Auto DJ's join), a fold holding plays the card's
//     stats.bin doesn't is saved first (Kind::GiveBack: the rotation's path,
//     never a compaction), then every resident block is given back but
//     MARK's (Fold::giveBack()): covered() goes on answering by the card's
//     MARK, so a trim or a rotation never deletes a play the card's stats
//     don't hold. Not while the boot's fold runs (its blocks are the
//     scan's), nor while the end flushes. While given back: no save (each
//     refused, Refused::GivenBack; a rotation's then deletes what the card's
//     MARK doesn't cover, 5.5's hole), no fold: each play written goes into
//     a ring of kRing slots in this object (no carve), and one past it is
//     counted. takeBack(), once the update is over and Auto DJ's rejoin done
//     or abandoned, at a pass that may carve: stats.bin read once, no waits,
//     a remount's verdicts (5.4; the stats off for the session if it
//     doesn't read). Then the ring folded, but a play the card's MARK
//     covers (another player folded it). The read must find the copy that
//     holds every slot RAM held at the give-back (the card's copy then, or
//     the give-back save's held tmp, which the read promotes first), and the
//     ring must have held every play: else (the save didn't land, the fold
//     had run out of memory, another stats.bin, a play past the ring) RAM
//     lacks plays the log holds above the card's MARK, and every later save
//     this session is refused (Refused::Behind): the next boot folds them.
//     A remount's scan begun again while given back (Ready, before the
//     first slot) waits for takeBack(), then reads at its first reserve(),
//     as any remount's: its boot's fold folds the whole log again.
// Loop task only.
namespace statsglue {

// Which save.
enum class Kind : uint8_t {
  Boot,      // the boot's scan done, before its trim (Folds::Why::Boot)
  Rotation,  // plays.1.jnl about to leave the log's names (Folds::Why::Rotation)
  Hs,        // the console's (the rotation's path)
  GiveBack,  // a library update's give-back, first (the rotation's path)
};
const char* kindName(Kind k);  // "boot", "rotation", "Hs", "give-back"

// Why a save wasn't asked of the store (None: it was).
enum class Refused : uint8_t {
  None,
  Off,        // the stats off this session (unread, newer, no memory), or the folds dropped while ending
  Ending,     // the boot's save while the orderly end flushes
  Budget,     // a rotation's in the end's flush with under kEndSaveMs of its budget left
  CardAway,   // the card out of the slot, refusing writes, or another card in it
  Partial,    // a log file wasn't read whole this session
  Booting,    // Hs during the boot's scan
  Fenced,     // Hs behind the library update's fence
  GivenBack,  // the blocks given back to a library update (until takeBack())
  Behind,     // RAM lacks plays the log holds above the card's MARK (a give-back): none this session
};
const char* refusedName(Refused r);

// A rotation in the end's flush saves only with this much of the flush's
// budget left (5.5): otherwise the plays in the file it deletes are lost to
// the stats, and the restart isn't held up.
constexpr uint32_t kEndSaveMs = 1000;

// What the glue asks of the firmware, and tells it.
class Env {
public:
  // The board's monotonic ms (the end's flush's budget).
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
  // A save asked: refused (`r`, and `rep` empty), or what the store did.
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
constexpr uint32_t kLogKeys = 16;

// A track's row as the stats hold it (the console's Ht): the base's (the
// file stats.bin names, read now) ⊕ TRKD's (RAM's).
struct TrackLook {
  enum class Base : uint8_t {
    None,     // no base named
    Found,    // its row read
    NotThere, // the base holds no row for it
    Unread,   // the base couldn't be read (the card failed, or the file isn't the one named)
  };
  Base base = Base::None;
  bool delta = false;  // TRKD holds a row
  statsfold::Track baseRow, deltaRow, row;  // row: base ⊕ delta (hash 0: none)
  uint32_t reads = 0;  // the base's rows read (a binary search)
};

// The plays written while the blocks are given back, folded after the
// re-read: a ring in the glue's own object (80 B a slot; no carve). An
// update's give-back lasts its fence (15-25 s at 19,410 tracks) and the
// rejoin, and a slot is written only at a pass the write gate lets through,
// one a pass: a few plays at most (the end's flush: up to the Tracker's 32).
constexpr uint32_t kRing = 16;

// What the last giveBack() that gave back did.
struct GaveBack {
  size_t bytes = 0;           // the blocks given back (0: none; nothing held or not now)
  size_t kept = 0;            // MARK's block, kept (covered()'s)
  uint32_t markRows = 0;      // its rows
  bool saveAsked = false;     // the fold held plays the card's stats.bin didn't: a save first
  Refused refused = Refused::None;  // ... refused by the glue (None: the store's, Glue::last())
  bool covers = false;        // the card's copy (or the give-back save's held tmp) holds every slot RAM held
};

// What the last takeBack() did.
struct TookBack {
  bool read = false;      // stats.bin read again (Glue::opened(): the verdict)
  bool rescan = false;    // a remount's scan begun again meanwhile: its first reserve() reads it
  bool on = false;        // the stats on after the read (else off for the session)
  bool same = false;      // the read found the copy that holds every slot RAM held at the give-back
  uint32_t folded = 0;    // the ring's plays folded
  uint32_t covered = 0;   // ... not folded: the card's MARK covers them (another player folded them)
  uint32_t over = 0;      // plays written past the ring (folded by the next boot)
  uint32_t dropped = 0;   // the ring's plays not folded: the stats off after the read (the next boot's)
  bool behind = false;    // every later save this session refused (Refused::Behind)
};

class Glue final : public playlog::Folds {
public:
  // `fs`: the card's. Every block (the fold's, the work memory) through
  // `alloc` (null: malloc).
  Glue(tagstore::Fs& fs, Env& env, statsfold::Fold::AllocFn alloc = nullptr,
       statsfold::Fold::FreeFn release = nullptr);
  Glue(const Glue&) = delete;
  Glue& operator=(const Glue&) = delete;

  // PlayHistory::begin(), before the journal's begin(): the fold's HOUR,
  // DAYS and MARK carved, then stats.bin read into it (5.4), `wait` between
  // the reads a card fault asks for (the boot's: tagstore::kBootWaitsMs).
  // `acks`: the root's (copied), for each boot's fold.
  const statsfile::Opened& begin(const cardcontract::msmf::Acks& acks, statsfile::Store::WaitFn wait);
  // The stats run: read (or absent, or corrupt and set aside), with memory
  // for the fold, not dropped at the end, and no remount's read waiting.
  bool on() const { return store_.on() && !dropped_ && !rereadPending_; }
  bool dropped() const { return dropped_; }      // a remount in the end's flush dropped the folds
  bool partialSeen() const { return partial_; }  // a log file not read whole this session
  // The boot's scan under way for the stats: a remount's read to make, the
  // boot's fold to begin, or folding.
  bool booting() const { return rereadPending_ || bootPending_ || fold_.booting(); }
  bool ending() const { return ending_; }
  // The next piece of the boot's scan takes the stats' room first
  // (reserve()): a read to make, a fold to begin, or a fold under way.
  bool wantsRoom() const { return rereadPending_ || bootPending_ || (on() && fold_.booting()); }
  bool rereadPending() const { return rereadPending_; }

  // Before each piece of the boot's scan, in a pass that may carve (Auto
  // DJ's join not Working, the write gate open): a remount's read, the
  // boot's fold begun, and room for the next `slots` folds. False: no room
  // (nothing lost: a fold then carves what it needs).
  bool reserve(uint32_t slots);
  // The journal ready: a boot's fold that asked no save (a partial scan)
  // ends here.
  void ready();
  // A play's slot written (Outbox::Env::written()): the live fold, RAM only.
  void written(const playlog::Slot& s);
  // The spare chunk held again after a live fold took it (a pass where the
  // join isn't Working). True: one is held (or nothing to hold it for).
  bool keepSpare();
  // The console's Hs: a save now, the rotation's path. `fenced`: the
  // library update's fence is up.
  bool hs(bool fenced);
  // The orderly end flushes (PlayHistory::flush()): from here no read (a
  // remount's still to make: the folds dropped), no boot's save, and a
  // rotation's only while kEndSaveMs of its budget is left before
  // `untilMs` (the board's monotonic ms). notEnding(): back.
  void ending(uint64_t untilMs);
  void notEnding() { ending_ = false; }

  // A library update's give-back (6.7): the fold saved first when it holds
  // plays the card's stats.bin doesn't (Kind::GiveBack), then every
  // resident block but MARK's given back. The bytes given back; 0: nothing
  // held (the stats off), given back already, the boot's fold under way,
  // or the end flushing. gaveBack() says what it did.
  size_t giveBack();
  // What giveBack() would give back now (the update's memory check counts
  // it in the step's room: UpdateEnv::alsoFreed).
  size_t resident() const;
  bool givenBack() const { return given_; }
  // The blocks again (the update over, Auto DJ's rejoin done or abandoned,
  // a pass that may carve, the card not away; never while the end
  // flushes): stats.bin read once, no waits, the ring folded. tookBack()
  // says what it did.
  const TookBack& takeBack();
  // Every later save this session refused: RAM lacks plays the log holds
  // above the card's MARK (the next boot folds them). A scan begun again
  // (a remount's) folds the log again and clears it.
  bool behind() const { return behind_; }
  uint32_t ringCount() const { return ringN_; }  // plays waiting in the ring
  uint32_t ringOver() const { return ringOver_; }  // ... and past it
  uint32_t giveBacks() const { return giveBacks_; }
  const GaveBack& gaveBack() const { return gave_; }
  const TookBack& tookBack() const { return took_; }

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
  const cardcontract::msmf::Acks& acks() const { return acks_; }
  // The log's (deviceId, epoch)s the last scan met (Hm); `more`: past kLogKeys.
  uint32_t logKeys() const { return logN_; }
  const LogKey& logKey(uint32_t i) const { return log_[i]; }
  uint32_t logKeysMore() const { return logMore_; }
  // A track's row by path hash (Ht): the base read from the card now, on
  // the loop (a binary search, 24 B a read).
  TrackLook track(uint64_t hash);
  // stats.bin's header as the card has it now (H): false when it isn't
  // there or can't be read.
  struct FileLook {
    bool there = false;   // the card answered, and it is there
    bool unread = false;  // the card failed the question
    uint32_t bytes = 0;
    uint32_t magic = 0;
    uint16_t major = 0, minor = 0;
    uint32_t generation = 0;
  };
  FileLook lookFile();

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
  void giveBack(uint8_t* p);
  statsfile::Opened readNow(statsfile::Store::WaitFn wait);
  void settle();  // a remount's read, the boot's fold begun: whatever waits
  void drop();    // the folds dropped (a remount while the end flushes)
  bool refuse(Kind k, Refused r);
  bool commit(Kind k);
  void noteLog(const playlog::Slot& s);
  void foldRing();

  tagstore::Fs& fs_;
  Env& env_;
  statsfold::Fold::AllocFn alloc_;
  statsfold::Fold::FreeFn free_;
  statsfold::Fold fold_;
  statsfile::Store store_;
  statsfile::SaveAsk ask_;
  cardcontract::msmf::Acks acks_;
  statsfile::Opened first_, opened_;
  statsfile::SaveReport last_;
  Kind lastKind_ = Kind::Boot;
  Refused lastRefused_ = Refused::None;
  uint32_t asked_ = 0, written_ = 0, rereads_ = 0;
  bool rereadPending_ = false;  // a remount's read of stats.bin to make (settle())
  bool bootPending_ = false;    // the boot's fold to begin (settle())
  bool partial_ = false;
  bool dropped_ = false;
  bool ending_ = false;
  uint64_t until_ = 0;
  LogKey log_[kLogKeys];
  uint32_t logN_ = 0, logMore_ = 0;
  // The give-back (6.7).
  bool given_ = false;   // the resident blocks given back (MARK's kept)
  bool behind_ = false;  // every save refused this session (Refused::Behind)
  bool holds_ = false;   // holding_ is a copy that holds every slot RAM held at the give-back
  statsfile::Store::Copy holding_;
  playlog::Slot ring_[kRing];
  uint32_t ringN_ = 0, ringOver_ = 0, giveBacks_ = 0;
  GaveBack gave_;
  TookBack took_;
};

}  // namespace statsglue
