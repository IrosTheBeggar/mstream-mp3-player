// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardRoot.h"
#include "PlayLog.h"
#include "StatsGlue.h"
#include "TagStore.h"
#include "app/StatsLists.h"

// The listening stats on the card (docs/LISTENING-STATS.md; S1, the store,
// as 5.14 reworked it: a fixed resident set): the firmware's side of lib/core
// StatsGlue, which keeps the session's rules over the fold (StatsFold: HEAD,
// MARK and 40 records in its object) and the card's files (StatsFile: each
// save merges the records with stats.bin's sections). app/PlayHistory owns
// it and joins it to the play log:
//   - begin(), in PlayHistory::begin() before the journal's begin(): the glue
//     made in internal RAM (.bss, never destroyed: the resident set, at most
//     2,048 B, and all of the stats' in internal RAM; nothing of the stats'
//     is held in PSRAM between calls but the boot's scan's blocks and MARK's
//     block below), stats.bin and its base read with the boot's
//     waits (tagstore::kBootWaitsMs: nothing else is held up yet), MARK's
//     block (PSRAM's, plain psramAlloc, kept for the session) for a card
//     whose MARK needs one (more than 8 rows),
//     and the Folds the journal is given. With the stats off (unread, a
//     newer major, no memory) the glue is still given: it covers nothing, so
//     nothing is trimmed (5.5);
//   - each pass (pass()): the update step's safe point for a compaction
//     (LibraryUpdate::safePoint(): nothing playing, or 20 s or more left of
//     the heard track and no seek in the last 2 s; a play waiting, for the
//     headphones or for the stats' overflow, or a restart asked isn't one,
//     and counts as playing for a compaction); before each piece of the
//     boot's scan its room (mayScan(): before its first piece none while a
//     library update is asked and not over, and none while Auto DJ's join is
//     Working: the scan waits that pass), the ranges' block and the
//     overflow's arena at the top of PSRAM's largest free block, 64 KB kept
//     free (as Auto DJ's join places its own), the free block left above
//     each closed by caps until the hold ends (5.14, fix A: its [listen]
//     line at the next pass, owedLine());
//   - while the boot's scan holds blocks (scanHolds(): the ranges' block, the
//     overflow's chunks) a library update asked waits (LibraryUpdate::
//     Wait::Stats) and Auto DJ's join doesn't begin; while overflow chunks
//     are held (overflowHeld(): a fold past the 40 records, a full log's,
//     once a card; about 1.4 s and the boot's save) a track's start waits
//     (main.cpp: PlayState::Waiting, PlayGate's statsHold). The card going
//     away mid-scan ends the fold as a partial scan's, its blocks given back
//     (scanAway());
//   - written(): the live fold of each play written (a record);
//   - the saves at a pass the write gate lets through (saveDue(), runSave():
//     statsglue::turnOf(), before that pass's slot): the boot overflow's
//     tries again, the records' save (24 records; not behind a library
//     update's fence until 40);
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
//   - the console's H (command()): H the state (RAM's at once; the card's
//     header and rows at a look's pass); Hd the last 7 UTC days (the card's
//     DAYS and HOUR with the records); Hm MARK beside ACKS and the log's
//     highest seqs (RAM's, at once); Ht<16 hex> a track's row by path hash
//     (base ⊕ stats.bin's TRKD ⊕ the records). The looks that read the card
//     wait for a pass the write gate lets through with no slot due and the
//     update's fence down (lookWaiting(), runLook(): one a pass). Hs a save
//     now, at the next pass the write gate lets through with no play
//     waiting for its slot that the card may take (refused during the
//     boot's scan, behind the update's fence, with the card away, after a
//     partial scan, behind, or after a changed stats.bin). Hr, Hl, Hw and Hp
//     (S2: the lists, the map and a track's facts) are app/StatsLists',
//     one ask at a time, nothing held between asks; their block comes from
//     the same top hold as the boot scan's (listsTake()), and the boot's
//     scan (a remount's) waits a pass while an ask holds it. S3a's Hv, Hn,
//     Hu, Hk and Hc too: the stats pages' asks (a visit's, Not played yet's
//     lean ask) as a page makes them, their snapshot (the Dance tab's
//     block, lent; held only by a page or Hu, released at the fence, the
//     Dance tab coming up and the orderly end) and the lit cap
//     (docs/LISTENING-STATS.md 9.3, 9.7).
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
  // The console's looks that read the card (H's card part, Hd, Ht), asked
  // by command() and made one a pass in Hs's turn (PlayHistory::loop():
  // statsglue::turnOf(), Hs first), at a pass the write gate lets through
  // and not behind the library update's fence, as the saves: their work
  // memory and FatFile (PSRAM, given back in the call) are never carved in
  // a start's, a jump's or a join's window, the gapless window or the fence,
  // where a decoder's or the update's long-lived blocks may be carved
  // beside them (the internal-RAM review; docs/LISTENING-STATS.md 5.14).
  bool lookWaiting() const { return looks_ != 0; }
  void runLook();
  // The console's H (the text after the H, Enter-ended).
  void command(const char* arg);
  // The owed lines (PlayHistory::loop() at each pass's start, flush()
  // after the end's), printed now from a frame under neither the journal's
  // append nor the scan: the hold's takes' (a line each: the block, the
  // caps that closed the free block above it or why it was left open, the
  // heap walks' time), then a rotation's save's.
  void owedLine();
  // S2's asks (Hr, Hl, Hw, Hp).
  StatsLists& lists() { return lists_; }
  const StatsLists& lists() const { return lists_; }
  // A track's facts as Ht prints them (Hp's too: `who`, its ms).
  static void printTrackLook(const char* who, uint64_t hash, const statsglue::TrackLook& t, float ms);

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
  void askLook(uint8_t look, const char* what);
  void printState();  // H's RAM part (at once)
  void printFile();   // H's card part (runLook())
  void printDays();
  void printMarks();
  void printTrack(uint64_t hash);

  static constexpr uint8_t kLookState = 1, kLookDays = 2, kLookTrack = 4;
  statsglue::Glue* glue_ = nullptr;  // internal RAM (.bss): the resident set
  StatsLists lists_;                 // S2's asks: their state alone (their memory is an ask's block)
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
  uint8_t looks_ = 0;  // the looks asked (kLook*), made one a pass (runLook())
  bool readyLogged_ = false;
  uint64_t lookHash_ = 0;  // Ht's path hash, the last asked
  int64_t saveT0_ = 0;     // the save under way began (esp_timer's us)
  float lastMs_ = 0.0f;    // the last save's
  bool owed_ = false;      // a rotation's save's line (owedLine())
  float owedMs_ = 0.0f;
  uint32_t reserveFailed_ = 0;
  uint32_t scanWaits_ = 0;    // passes the boot's scan waited for the join
  uint32_t updateWaits_ = 0;  // ... for a library update asked before its fold began (LibraryUpdate::asked())
};
