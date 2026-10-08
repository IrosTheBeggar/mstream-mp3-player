// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardJobs.h"
#include "LibraryIndex.h"
#include "LibraryText.h"
#include "LibraryUpdate.h"
#include "QueueModel.h"
#include "ScanScheduler.h"
#include "app/CardWorker.h"
#include "app/Library.h"
#include "storage/CardFat.h"

namespace ui {
class Thumbs;
}

// The card worker's loop side (docs/METADATA.md 3.2.3, 3.3, 3.4.2;
// milestones N10, N12): every loop pass it asks ScanScheduler (N7) what the
// one card worker may do next, hands it that one step at that priority, and
// takes in what the last step did. The jobs:
//   - covers (ui/Thumbs), on any storage;
//   - on the SD card, over the records Library opened: the validation
//     walk, 2 s after the UI's first frame (3.2.3); the compactions; the
//     tag scan (lib/core CardJobs); the update step's build and its save
//     (lib/core LibraryUpdate, Library's).
// What the steps do is CardJobs' and LibraryUpdate's; this is when they
// run, what the loop learns from them, and what follows:
//   - after a walk that found changes: "Found 12 new tracks", and the update
//     step at once when ScanScheduler::buildAfterWalk() says so (U11: 200
//     files or more, or a scan over 60 s), else at the scan's end;
//   - a file the scan read: the index stops calling it Pending (so the
//     playing track, the queue and the Library tab's page aren't asked for
//     again), and the playing track's tags go to Now Playing at once
//     (Library::setOverlay(), 3.3.3);
//   - the scan's end (nothing Pending in D or the index's sources): the
//     update step, when the journal took records since the last build
//     (CardJobs::newRecords(): reads a full card refused are none), or a
//     walk's changes, a Rescan, or a boot that loaded an index the scan had
//     gone past (Library::softStale(): the first scan's end after the
//     boot's walk, even with nothing to scan) ask for it; not when the rest
//     stopped on a read error (the card pulled: the next boot has it).
//     After an update step, built or failed or deferred, what the journal
//     had asks for no other: only new records (or a walk's changes, a
//     Rescan) do.
// The update step itself is LibraryUpdate's state machine (3.4.2), run here
// every pass with the worker's and the jobs' state and the loop's (UpdateEnv:
// the safe point, what the step frees): it holds the scan and new walks
// once asked, asks the compaction first, has the worker's task made
// (Out::wantWorker: the build must start at once, so no fence goes up
// without it) and kept to the hand-off, then (Do::Fence) main.cpp puts the
// fence up and calls fenceUp(); the build is the worker's (priority 1); at
// its end (Do::Live) main.cpp takes the fence down and calls live(); the
// save is the worker's too, and the background jobs wait for its end.
//
// The scheduler's inputs from the rest of the firmware come in each pass
// (`in`: the UI, the audio, Bluetooth, power, as N7's notes map them);
// this adds what has work, the step under way, and the scan's sources
// (Sources: the index's Pending tracks among the playing one, the queue's
// next 3 and 200, the Library tab's page).
//
// Loop task only.
class CardTasks {
public:
  struct Sources {
    uint32_t playing = LibraryIndex::kNone;  // the current entry's track (a library id; else kNone)
    const QueueModel* queue = nullptr;
    LibraryIndex::Span shown;  // the Library tab's album, artist or folder page (its tracks)
  };
  // What the update step needs of the loop (LibraryUpdate::In's player and
  // memory parts).
  struct UpdateEnv {
    bool playing = false;      // PlayState::Playing
    bool waiting = false;      // PlayState::Waiting (a play waiting for the headphones)
    uint32_t trackLeftMs = 0;  // what is left of the heard track (0: not known)
    uint32_t seekSeq = 0;      // PlaybackController::seeks()
    size_t alsoFreed = 0;      // what the step frees besides the index: the queue's, Thumbs' pools
  };

  CardTasks(Library& library, CardWorker& worker);

  // After Library::begin(): the jobs over the card's records (false: no
  // records here: covers only).
  bool begin();
  bool active() const { return active_; }
  // The UI's first frame: the validation walk kWalkDelayMs later.
  void armWalk(uint32_t nowMs);
  static constexpr uint32_t kWalkDelayMs = 2000;

  // Every loop pass. `in`: the environment (nowMs; listMoving, input; the
  // audio's ring, underruns, decode pass, track and seek; Bluetooth;
  // power); `covers`: a cover job may start (the UI is up and lit); `env`:
  // the update step's.
  void loop(ScanScheduler::In in, const Sources& src, ui::Thumbs* thumbs, bool covers, const UpdateEnv& env);

  // ---- the update step (3.4.2, lib/core LibraryUpdate) ----
  // `deferToBoot` (gb!, L4.4): the memory check made to fail, so the step
  // writes the build-at-boot marker as a short PSRAM would.
  void askUpdate(const char* why, bool deferToBoot = false);
  // g0: the walk now, the update step after it whatever it finds.
  void askWalkAndUpdate();
  bool updateAsked() const;
  // From the fence to the save's end (the console's commands that read
  // the card's records or the library wait; the CPU speed's restart).
  bool updating() const { return lastUpdate_.updating; }
  // What main.cpp carries out after loop(), each once: Fence (the fence up,
  // then fenceUp()), Live (the fence down, then live()), Deferred, Failed,
  // Saved (for their lines; the jobs' side is done here).
  LibraryUpdate::Do takeAct() {
    const LibraryUpdate::Do a = act_;
    act_ = LibraryUpdate::Do::None;
    return a;
  }
  // Right after main.cpp's steps 1-3: the scan's View closed, its chunk
  // out, its memory back; the old index cleared (LibraryUpdate::fencedUp()).
  void fenceUp();
  // Right after main.cpp's step 5 (the queue read back, the readers back):
  // the scan's state starts over with the new index; "Library updated";
  // the save next (LibraryUpdate::lived()).
  void live();

  // ---- the console (TagConsole's hooks) ----
  bool rescan(bool everything);
  bool walkNow();
  bool verify();
  void state(librarytext::Status* s, char* line, size_t size) const;
  void report() const;  // gs's lines: the waits, the steps, the worker, the jobs, the cache
  // gs0: the scheduler's waits and steps, the worker's stack and internal
  // RAM's lowest start again (L3's figures, one condition at a time).
  void resetStats();
  // Waits for the worker's step to finish and takes it in (before the
  // loop reads the card's records itself: gs, gt; the idle power-off).
  bool waitIdle(uint32_t maxMs);

  // ---- the rest of the firmware ----
  // The Library tab's status line (3.3.6).
  librarytext::Status status() const;
  // A compaction under way, or the update step from its fence to its
  // save's end: IdlePolicy's LibraryWrite.
  bool libraryWrite() const { return compacting_ || lastUpdate_.libraryWrite; }
  // The idle power-off's shutdown: the scan's chunk to the card next to the
  // queue (3.3.5).
  void flushNow();
  // A toast to show ("Found 12 new tracks", "Library updated", "Library
  // updates at next boot"); false: none.
  bool takeToast(char* buf, size_t size);
  ScanScheduler& scheduler() { return sched_; }
  CardWorker& worker() { return worker_; }

private:
  static void stepEntry(void* self);
  static void buildEntry(void* self);
  static void saveEntry(void* self);
  static bool indexed(const char* rel, size_t len, void* ctx);
  void taken(ScanScheduler::Job job, uint32_t nowMs);
  void afterWalk(const cardjobs::Done& d);
  void afterScan(const cardjobs::Done& d, uint32_t nowMs);
  void scanEnded();
  // The update step's ends the jobs hear of (a deferral, a failure, the
  // save): what the journal had asks for no other step.
  void updateOver();
  // The scan's sources from the index (3.3.3): each source's first Pending
  // track, its path for the step.
  void sources(ScanScheduler::In& in, const Sources& src);
  bool pendingTrack(uint32_t id) const;
  bool pick(uint32_t id, int slot);
  void toast(const char* text);
  uint32_t indexPending() const;

  Library& lib_;
  CardWorker& worker_;
  cardfat::FatCard card_;
  cardjobs::Jobs jobs_;
  ScanScheduler sched_;
  bool active_ = false;
  bool walkArmed_ = false;
  uint32_t walkAtMs_ = 0;
  // The step handed (its kind) and the scan's file per source.
  static constexpr int kSlots = 4;  // Playing, QueueNext, QueueSoon, Shown
  char picks_[kSlots][TrackCatalog::kMaxPath] = {};
  size_t pickLen_[kSlots] = {};
  uint32_t playing_ = LibraryIndex::kNone;
  bool compacting_ = false;
  // The update step.
  LibraryUpdate::Out lastUpdate_;
  LibraryUpdate::Do act_ = LibraryUpdate::Do::None;
  bool updateAfterWalk_ = false;
  bool lastCompactFailed_ = false;
  bool pendingAfterScan_ = false;  // the scan's end rebuilds (a walk's changes, a soft-stale index, a Rescan)
  bool restFailed_ = false;        // the rest's last end was a read error (no update at the scan's end)
  bool scanOver_ = true;
  // The status line's counts.
  uint32_t scanDone_ = 0, scanTotal_ = 0;
  uint32_t lastScanStepMs_ = 0;
  uint32_t lastWorkMs_ = 0;
  bool walkSeen_ = false;  // the boot's walk has begun (the status says "Checking the card…" until it ends)
  bool bootWalkDone_ = false;  // the boot's walk ended: D's to-do is known, the scan may run
  ScanScheduler::Wait lastWait_ = ScanScheduler::Wait::None;
  char toast_[64] = "";
  // For the console.
  cardwalk::CardWalk::Result lastWalk_;
  bool walked_ = false;
  uint32_t walkStartMs_ = 0, walkMs_ = 0;
  tagstore::TagStore::Compacted lastCompaction_;
  uint32_t updates_ = 0, deferred_ = 0;
};
