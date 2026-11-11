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
//     a restart asked isn't one), the spare chunk again where Auto DJ's join
//     isn't Working; before each piece of the boot's scan its room (mayScan():
//     none while the join is Working, and the scan waits that pass: 5.6);
//   - written(): the live fold of each play written (hash-keyed: no id in S1);
//   - ending(): the orderly end's flush (no read at a remount inside it, no
//     boot's save, a rotation's only with 1 s of its budget left);
//   - every save refused while the card is away: out of the slot, refusing
//     writes, or another card in it (sectordisk's guard);
//   - [listen] lines: the read (and a remount's), each save (the boot's, a
//     rotation's, Hs's) with its ms and bytes, a log file not read whole, the
//     boot's fold at Ready;
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
  };
  void pass(const Pass& p, uint32_t nowMs);
  // Before a piece of the boot's scan of `slots` slots: true, its room taken
  // (or nothing to take); false: the scan waits this pass (the join works).
  bool mayScan(uint32_t slots);
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
  void printState();
  void printDays();
  void printMarks();
  void printTrack(const char* hex);

  statsglue::Glue* glue_ = nullptr;  // PSRAM
  tagstore::Fs* fs_ = nullptr;
  bool hasCommit_ = false;
  uint32_t commitSec_ = 0;
  bool playing_ = false;
  bool safePoint_ = true;
  bool joinWorking_ = false;
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
