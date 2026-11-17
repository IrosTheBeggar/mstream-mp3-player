// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardRoot.h"
#include "PlayLog.h"
#include "StatsGlue.h"
#include "TagStore.h"

// The listening stats on the card (docs/LISTENING-STATS.md; S1, the store,
// as 5.14 reworked it: a fixed resident set): the firmware's side of lib/core
// StatsGlue, which keeps the session's rules over the fold (StatsFold: HEAD,
// MARK and 96 records in its object) and the card's files (StatsFile: each
// save merges the records with stats.bin's sections). app/PlayHistory owns
// it and joins it to the play log:
//   - begin(), in PlayHistory::begin() before the journal's begin(): the glue
//     in PSRAM (one block, never freed: the resident set), stats.bin and its
//     base read with the boot's waits (tagstore::kBootWaitsMs: nothing else
//     is held up yet), MARK's block beside it for a card whose MARK needs
//     one, and the Folds the journal is given. nullptr: no PSRAM for the
//     glue, so no stats and the play log as 0.14.0's (its trim by ACKS
//     alone). With the stats off (unread, a newer major, no memory) the glue
//     is still given: it covers nothing, so nothing is trimmed (5.5);
//   - each pass (pass()): the update step's safe point for a compaction
//     (LibraryUpdate::safePoint(): nothing playing, or 20 s or more left of
//     the heard track and no seek in the last 2 s; a play waiting, for the
//     headphones or for the stats' overflow, or a restart asked isn't one,
//     and counts as playing for a compaction); before each piece of the
//     boot's scan its room (mayScan(): before its first piece none while a
//     library update is asked and not over, and none while Auto DJ's join is
//     Working: the scan waits that pass), the ranges' block and the
//     overflow's chunks at the top of PSRAM's largest free block, 64 KB kept
//     free (as Auto DJ's join places its own);
//   - while the boot's scan holds blocks (scanHolds(): the ranges' block, the
//     overflow's chunks) a library update asked waits (LibraryUpdate::
//     Wait::Stats) and Auto DJ's join doesn't begin; while overflow chunks
//     are held (overflowHeld(): a fold past the 96 records, a full log's,
//     once a card; about 1.4 s and the boot's save) a track's start waits
//     (main.cpp: PlayState::Waiting, PlayGate's statsHold). The card going
//     away mid-scan ends the fold as a partial scan's, its blocks given back
//     (scanAway());
//   - written(): the live fold of each play written (a record);
//   - the saves at a pass the write gate lets through (saveDue(), runSave():
//     statsglue::turnOf(), before that pass's slot): the boot overflow's
//     tries again, the records' save (64 records; not behind a library
//     update's fence until 96);
//   - ending(): the orderly end's flush (no read at a remount inside it, no
//     boot's or records' save, a rotation's only with 1 s of its budget left);
//   - every save refused while the card is away: out of the slot, refusing
//     writes, or another card in it (sectordisk's guard);
//   - [listen] lines: the read (and a remount's), each save (the boot's and
//     its tries, a rotation's, the records', Hs's) with its ms and bytes, a
//     log file not read whole, the boot's fold at Ready. A rotation's at the
//     next pass or after the end's flush (owedLine()): its save runs under
//     the journal's append, the loop's deepest path, and printf's frames
//     stay out from under it;
//   - the console's H (command()): H the state; Hd the last 7 UTC days (the
//     card's DAYS and HOUR with the records); Hm MARK beside ACKS and the
//     log's highest seqs; Ht<16 hex> a track's row by path hash (base ⊕
//     stats.bin's TRKD ⊕ the records, read now on the loop); Hs a save now,
//     at the next pass the write gate lets through with no play waiting for
//     its slot that the card may take (refused during the boot's scan, behind
//     the update's fence, with the card away, after a partial scan, behind,
//     or after a changed stats.bin).
// Loop task only.
class ListeningStats final : private statsglue::Env {
public:
  playlog::Folds* begin(tagstore::Fs& fs, const cardroot::Root& root);
  bool active() const { return glue_ != nullptr; }

  struct Pass {
    bool playing = false;      // PlayState::Playing
    bool waiting = false;      // a play waiting (the headphones, the stats' overflow), or a restart asked
    uint32_t trackLeftMs = 0;  // of the heard track (0: not known)
    uint32_t seeks = 0;        // PlaybackController::seeks(), free-running
    bool joinWorking = false;  // Auto DJ's join (AutoDj::joinWorking())
    bool updating = false;     // a library update step asked, not over (LibraryUpdate::asked())
  };
  void pass(const Pass& p, uint32_t nowMs);
  // Before a piece of the boot's scan: true, its room taken (a remount's read
  // made, the boot's fold begun, first: statsglue::kReserveAhead, no chunk
  // ahead of the piece), or nothing to take; false: the scan waits this
  // pass (statsglue::Glue::scanWaits(): the join works; or its fold not
  // begun and a library update asked).
  bool mayScan();
  // The card went away mid-scan (PlayHistory's cardAway(): the scan stops
  // until a remount): the fold ended as a partial scan's, its blocks given
  // back (statsglue::Glue::scanAway()); its line once.
  void scanAway();
  // The boot's scan holds blocks across passes (the update step's
  // Wait::Stats, Auto DJ's join's hold); overflow chunks among them (a
  // track's start waits).
  bool scanHolds() const { return glue_ && glue_->scanHolds(); }
  bool overflowHeld() const { return glue_ && glue_->overflowHeld(); }
  // The journal ready (the boot's scan done: `passes` and `scanUs` its
  // pieces' on the loop): a partial scan's fold ended; the fold's line.
  void ready(uint32_t passes, int64_t scanUs);
  // A play's slot written (Outbox::Env::written()).
  void written(const playlog::Slot& s);
  // PlayHistory::flush(): the orderly end (`untilMs`: the flush's deadline on
  // the board's monotonic clock; 0 before it starts).
  void ending(uint64_t untilMs);
  // A save due at this pass (the overflow's next try, the records' save),
  // before the pass's slot; runSave() makes it. `fenced`: the library
  // update's fence is up.
  bool saveDue(bool fenced) { return glue_ && glue_->saveDue(fenced); }
  void runSave(bool fenced);
  // Hs asked, waiting for a pass the write gate lets through with no slot
  // due (PlayHistory::loop(): statsglue::turnOf()); run it then.
  bool hsWaiting() const { return hsWaiting_; }
  void runHs(bool fenced);
  // The console's H (the text after the H, Enter-ended).
  void command(const char* arg);
  // A rotation's save's line, owed since its save (PlayHistory::loop() at
  // each pass's start, flush() after the end's): printed now, from a frame
  // not under the journal's append.
  void owedLine();

private:
  // statsglue::Env
  uint64_t monoMs() override;
  bool cardAway() override;
  void asking(statsglue::Kind k, statsfile::SaveAsk* ask) override;
  void began(bool again, const statsfile::Opened* read) override;
  void partialFile() override;
  void saved(statsglue::Kind k, statsglue::Refused r, const statsfile::SaveReport& rep) override;

  void logRead(const statsfile::Opened& o, const char* when, float ms);
  void logSaved(statsglue::Kind k, const statsfile::SaveReport& rep, float ms);
  void printState();
  void printDays();
  void printMarks();
  void printTrack(const char* hex);

  statsglue::Glue* glue_ = nullptr;  // PSRAM: the resident set
  tagstore::Fs* fs_ = nullptr;
  bool hasCommit_ = false;
  uint32_t commitSec_ = 0;
  bool playing_ = false;
  bool waiting_ = false;  // a play waiting (the headphones, the stats' overflow), or a restart asked
  bool safePoint_ = true;
  bool joinWorking_ = false;
  bool updating_ = false;
  bool seeksKnown_ = false;
  uint32_t seeks_ = 0;
  bool seekSeen_ = false;
  uint32_t seekAt_ = 0;
  bool hsWaiting_ = false;
  bool readyLogged_ = false;
  int64_t saveT0_ = 0;     // the save under way began (esp_timer's us)
  float lastMs_ = 0.0f;    // the last save's
  bool owed_ = false;      // a rotation's save's line (owedLine())
  float owedMs_ = 0.0f;
  uint32_t reserveFailed_ = 0;
  uint32_t scanWaits_ = 0;    // passes the boot's scan waited for the join
  uint32_t updateWaits_ = 0;  // ... for a library update asked before its fold began (LibraryUpdate::asked())
};
