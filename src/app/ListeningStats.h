// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardRoot.h"
#include "PlayLog.h"
#include "StatsGlue.h"
#include "TagStore.h"

// The listening stats on the card (docs/LISTENING-STATS.md; S1, the store:
// /.player/stats.bin and its base, nothing on the screen yet): the
// firmware's side of lib/core StatsGlue, which keeps the session's rules over
// the fold in RAM (StatsFold) and the card's files (StatsFile). app/
// PlayHistory owns it and joins it to the play log:
//   - begin(), in PlayHistory::begin() before the journal's begin(): the glue
//     in PSRAM (the fold's blocks through psramAlloc too), stats.bin and its
//     base read with the boot's waits (tagstore::kBootWaitsMs: nothing else
//     is held up yet) and loaded with their chunks, and the Folds the journal
//     is given. nullptr: no PSRAM for the glue, so no stats and the play log
//     as 0.14.0's (its trim by ACKS alone). With the stats off (unread, a
//     newer major, no memory for the fold) the glue is still given: it covers
//     nothing, so nothing is trimmed (5.5);
//   - each pass (pass()): the update step's safe point for a compaction
//     (LibraryUpdate::safePoint(): nothing playing, or 20 s left of the heard
//     track and no seek in the last 2 s; a play waiting for the headphones or
//     a restart asked isn't one, and counts as playing for a compaction),
//     the spare chunk again where Auto DJ's join isn't Working; before each
//     piece of the boot's scan its room, and before its first the boot's
//     fold begun or a remount's read of stats.bin made (mayScan(): none
//     while the join is Working, and the scan waits that pass: 5.4, 5.6);
//   - written(): the live fold of each play written (hash-keyed: no id in S1);
//   - ending(): the orderly end's flush (no read at a remount inside it, no
//     boot's save, a rotation's only with 1 s of its budget left);
//   - every save refused while the card is away: out of the slot, refusing
//     writes, or another card in it (sectordisk's guard);
//   - a library update's give-back (6.7, the owner's decision of
//     2026-11-11): giveBack() at the update's memory check (main.cpp's
//     setGiveBack() hook, after Auto DJ's join) and at its fence (after
//     Auto DJ's fence()): the fold saved first when it holds plays the
//     card's stats.bin doesn't, then every resident block but MARK's given
//     back (resident(): the step's UpdateEnv::alsoFreed counts them); the
//     plays written meanwhile wait in the glue's ring. takeBack() once the
//     update is over (LibraryUpdate::asked() false) and Auto DJ's join is
//     settled (AutoDj::joinSettled(): rejoined, or none to come), at a pass
//     the write gate lets through, the join not Working and the card not
//     away: stats.bin read once (a remount's verdicts), the ring folded;
//     the boot's scan begun again by a remount meanwhile waits for it
//     (mayScan()) and reads it at its next piece;
//   - [listen] lines: the read (and a remount's), each save (the boot's, a
//     rotation's, Hs's) with its ms and bytes, a log file not read whole, the
//     boot's fold at Ready, a library update's give-back (its save in the
//     same line) and the re-read after it;
//   - the console's H (command()): H the state; Hd the last 7 UTC days; Hm
//     MARK beside ACKS and the log's highest seqs; Ht<16 hex> a track's row by
//     path hash (base ⊕ delta, the base read now on the loop); Hs a save now,
//     the rotation's path, at the next pass the write gate lets through
//     (refused during the boot's scan, behind the update's fence, with the
//     card away, after a partial scan or a changed stats.bin).
// Loop task only.
class ListeningStats final : private statsglue::Env {
public:
  playlog::Folds* begin(tagstore::Fs& fs, const cardroot::Root& root);
  bool active() const { return glue_ != nullptr; }

  struct Pass {
    bool playing = false;      // PlayState::Playing
    bool waiting = false;      // a play waiting for the headphones, or a restart asked
    uint32_t trackLeftMs = 0;  // of the heard track (0: not known)
    uint32_t seeks = 0;        // PlaybackController::seeks(), free-running
    bool joinWorking = false;  // Auto DJ's join (AutoDj::joinWorking())
    bool joinSettled = true;   // ... neither under way nor to come (AutoDj::joinSettled(); no Auto DJ: true)
    bool updating = false;     // a library update step asked, not over (LibraryUpdate::asked())
  };
  void pass(const Pass& p, uint32_t nowMs);
  // Before a piece of the boot's scan of `slots` slots: true, its room taken
  // (a remount's read made, the boot's fold begun, first), or nothing to
  // take; false: the scan waits this pass (the join works, or the blocks
  // are given back to a library update until takeBack()).
  bool mayScan(uint32_t slots);
  // A library update's give-back (6.7): `where` its point ("its memory
  // check", "its fence"); the bytes given back (0: none held, given back
  // already, the boot's fold under way). Its [listen] line.
  size_t giveBack(const char* where);
  // What giveBack() would give back now (the step's UpdateEnv::alsoFreed).
  size_t resident() const { return glue_ ? glue_->resident() : 0; }
  // The blocks again is due: given back, the update over, Auto DJ's join
  // settled and not Working (this pass's look). takeBack() at a pass the
  // write gate lets through, the card not away: the re-read, its line.
  bool takeBackDue() const;
  void takeBack();
  // The journal ready (the boot's scan done: `passes` and `scanUs` its
  // pieces' on the loop): a partial scan's fold ended; the fold's line.
  void ready(uint32_t passes, int64_t scanUs);
  // A play's slot written (Outbox::Env::written()).
  void written(const playlog::Slot& s);
  // PlayHistory::flush(): the orderly end (`untilMs`: the flush's deadline on
  // the board's monotonic clock; 0 before it starts).
  void ending(uint64_t untilMs);
  // Hs asked, waiting for a pass the write gate lets through; run it then.
  bool hsWaiting() const { return hsWaiting_; }
  void runHs(bool fenced);
  // The console's H (the text after the H, Enter-ended).
  void command(const char* arg);

private:
  // statsglue::Env
  uint64_t monoMs() override;
  bool cardAway() override;
  void asking(statsglue::Kind k, statsfile::SaveAsk* ask) override;
  void began(bool again, const statsfile::Opened* read) override;
  void partialFile() override;
  void saved(statsglue::Kind k, statsglue::Refused r, const statsfile::SaveReport& rep) override;

  void logRead(const statsfile::Opened& o, const char* when, float ms);
  void logGiveBack(const char* where, size_t bytes);
  void logTakeBack(float ms);
  void printState();
  void printDays();
  void printMarks();
  void printTrack(const char* hex);

  statsglue::Glue* glue_ = nullptr;  // PSRAM
  tagstore::Fs* fs_ = nullptr;
  bool hasCommit_ = false;
  uint32_t commitSec_ = 0;
  bool playing_ = false;
  bool waiting_ = false;  // a play waiting for the headphones, or a restart asked
  bool safePoint_ = true;
  bool joinWorking_ = false;
  bool joinSettled_ = true;
  bool updating_ = false;
  uint32_t givenAt_ = 0;     // the last give-back (millis())
  uint32_t givenWaits_ = 0;  // the boot's scan's passes waiting for the take-back
  bool seeksKnown_ = false;
  uint32_t seeks_ = 0;
  bool seekSeen_ = false;
  uint32_t seekAt_ = 0;
  bool hsWaiting_ = false;
  bool readyLogged_ = false;
  int64_t saveT0_ = 0;     // the save under way began (esp_timer's us)
  float lastMs_ = 0.0f;    // the last save's
  uint32_t reserveFailed_ = 0;
  uint32_t scanWaits_ = 0;  // passes the boot's scan waited for the join
};
