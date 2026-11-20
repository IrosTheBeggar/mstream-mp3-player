// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <atomic>

#include "AutoDjJoin.h"
#include "LibraryIndex.h"
#include "StatsGlue.h"
#include "StatsMap.h"
#include "StatsRank.h"
#include "TagStore.h"
#include "app/CardWorker.h"

// The listening stats' lists and map on the console (docs/LISTENING-STATS.md
// 6, 9.3, 9.6; S2): Hr, Hl<shelf>[w], Hw and Hp. The owner's "A: nothing
// held" (2026-11-20): nothing of them is held between asks, and S2 ranks
// only when asked (S3 decides how long a page holds its lists). Each ask, one
// at a time:
//   1. asked (command()); it waits for the stats on and the boot's scan done
//      (no hold of its own), no library update asked or under way, Auto DJ's
//      join neither Working nor due (6.8: a rank starts only when the join
//      is Ready or off), the card not away or held; then for a pass the
//      play log's write gate lets through, after a stats save due and the
//      plays waiting for their slots (PlayHistory's turn, as Hs and the
//      looks: its PSRAM carved only where theirs is);
//   2. at that turn, on the loop: stats.bin's directory read (its TRKD's
//      rows: the played set's room), ONE block carved at the top of PSRAM's
//      largest free block with the free block above it closed by caps (the
//      boot hold's way: autodj::TopHold, 5.14's fix A; a decoder's room
//      asked, as for the ranges' block: no room, the ask refused), holding
//      the ask's state (the ranker, the map, the walk's path buffer) and
//      every block its steps fill: the rank's work memory, the lists, the
//      30 days' RCNT rows, the records' copy (Fold::Cursor's rows, TRKD's and
//      RCNT's: 5.14's "The screens"), and for Hl or Hw the walk's: the
//      played set (Hw, Hl3), the album bits, the folders' states, the wanted
//      set; stats.bin and the base opened (a FatFile each, PSRAM: the
//      worker allocates nothing, 6.8);
//   3. the card worker's step (ScanScheduler's Job::Stats): the rank in the
//      Rank lane (background work after a compaction: never under the
//      update step or a yield, priority 0 lit, 1 dark, never cut); Hp's read
//      in the Track lane beside the waveform read (a binary search of the
//      base: no sparse index is kept between asks). While it runs no stats
//      save and no slot (a slot's rotation saves) is made: none replaces
//      stats.bin under its reads;
//   4. Hr, Hp: the result printed, the block given back. Hl, Hw: the walk
//      (StatsMap over pathhash::Walk) on the loop after Auto DJ's step, up
//      to 8 ms a pass (the loop's idle wait short meanwhile, as for the
//      join's), never in a pass the join works, with a list moving, the card
//      held or an update asked; an index that changes under it begins it
//      again (three times, then the ask ends); then printed, given back.
// While the block is held (holds()): a library update asked waits for it
// (LibraryUpdate::In::statsWait: it is never in the update's memory check or
// across a build), and Auto DJ's join doesn't begin (its Env::statsHold).
// The fence can't come while it is held; should it all the same
// (enterFence(): drop()), the block is given back, its bytes counted in the
// update's alsoFreed, and the ask begins again after the Live.
// Loop task, but for the worker's step (its own ask's fields, published by
// state_).
class StatsLists {
public:
  // The top of PSRAM's hold (app/ListeningStats: autodj::TopHold over the
  // heap, as the boot hold's blocks): take a block (the decoder's room
  // asked; null: no room, `r` says why), give it back.
  struct Hold {
    void* (*take)(size_t bytes, autodj::CloseReport* r) = nullptr;
    void (*give)(void* p) = nullptr;
  };
  // A track's facts printed (Hp's, as Ht prints them: app/ListeningStats).
  using PrintTrackFn = void (*)(const char* who, uint64_t hash, const statsglue::TrackLook& t, float ms);

  void begin(statsglue::Glue* glue, tagstore::Fs* fs, const Hold& hold, PrintTrackFn print);

  // What the loop knows this pass.
  struct Gates {
    uint32_t nowMs = 0;
    const LibraryIndex* index = nullptr;  // Library::index() (nullptr behind the fence)
    bool updateAsked = false;             // a library update asked and not over (LibraryUpdate::asked())
    bool joinPending = false;             // Auto DJ's join Working or due (AutoDj::joinPending())
    bool joinWorking = false;             // ... Working this pass (AutoDj::joinWorking())
    bool cardHeld = false;                // the card's guard holds the card
    bool cardAway = false;                // out of the slot, refusing writes, another card
    bool uiStarted = true;
    bool listMoving = false;
    uint64_t playingHash = 0;             // the open play's path hash (Hp; 0: none)
  };

  // The console's H's letters r, l, w, p (the text from that letter on).
  void command(const char* arg);
  // An ask waits and may carve now (PlayHistory's turn, statsglue::turnOf()'s
  // Hs: after a save due and the slots due).
  bool turnWanted(const Gates& g) const;
  // At that turn: the carve, the records' copy, the opens; or a refusal.
  void turn(const Gates& g);
  // Every pass, before the card's step: a step the worker finished taken in
  // (the files closed on the loop), its result printed or the walk begun.
  void loop(const Gates& g);
  // After Auto DJ's step: the walk's slices.
  void stepMap(const Gates& g);

  // ---- the card worker (app/CardTasks) ----
  bool wantsRank() const { return state_.load() == kRankQueued; }
  bool wantsTrack() const { return state_.load() == kTrackQueued; }
  bool rankRunning() const { return state_.load() == kRunning && rank_; }
  // Hands the queued step to the worker (Job::Stats; `rank`: the Rank lane's).
  bool start(CardWorker& worker, uint8_t priority, bool rank, uint32_t nowMs);
  // The worker's step queued or under way: no stats save and no slot meanwhile.
  bool workerOwned() const {
    const uint8_t s = state_.load();
    return s == kRankQueued || s == kTrackQueued || s == kRunning || s == kDone;
  }

  // ---- the rest of the firmware ----
  // The ask holds its block (a library update waits; Auto DJ's join doesn't
  // begin).
  bool holds() const { return block_ != nullptr; }
  // The walk works (main.cpp: the loop's idle wait short meanwhile).
  bool walking() const { return state_.load() == kWalk; }
  // The update's fence went up (it shouldn't while held): the block given
  // back (its bytes: alsoFreed), the ask kept to begin again after the Live.
  size_t drop();
  // The orderly end: a worker step under way finished first (at most
  // `maxMs`), then the block given back.
  void ending(uint32_t maxMs);

private:
  enum Kind : uint8_t { kNone, kRank, kList, kWalkAsk, kTrack };
  enum State : uint8_t { kIdle, kAsked, kRankQueued, kTrackQueued, kRunning, kDone, kWalk };
  struct Ask;  // the block's head (StatsLists.cpp)
  static void entry(void* self);
  void giveBack();
  void finishRank(const Gates& g);
  void finishTrack();
  void printLists(const Gates& g);
  void printWalk(const Gates& g);
  void refuse(const char* why);
  bool waitWorker(uint32_t maxMs);  // the worker's step under way ended (at most maxMs): true
  const char* name() const;

  statsglue::Glue* glue_ = nullptr;
  tagstore::Fs* fs_ = nullptr;
  Hold hold_;
  PrintTrackFn print_ = nullptr;
  Ask* block_ = nullptr;  // the ask's block (its head: Ask)
  size_t bytes_ = 0;
  std::atomic<uint8_t> state_{kIdle};
  Kind kind_ = kNone;
  bool rank_ = false;         // the queued (or running) step is a rank
  bool again_ = false;        // dropped at a fence: begin again after the Live
  uint8_t shelf_ = 0;         // Hl's: 0 Most played, 1 New finds, 2 Old favourites, 3 Not played yet, 4 Recently played
  uint8_t window_ = 2;        // Most played's: 0 7 days, 1 30 days, 2 All time
  uint8_t restarts_ = 0;      // the walk begun again (an index that changed)
  uint32_t askedMs_ = 0;
  uint32_t waitLogged_ = 0;   // the gate that held the ask, said once (a bit each)
};
