// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardJobs.h"
#include "LibraryIndex.h"
#include "LibraryText.h"
#include "QueueModel.h"
#include "ScanScheduler.h"
#include "app/CardWorker.h"
#include "app/Library.h"
#include "storage/CardFat.h"

namespace ui {
class Thumbs;
}

// The card worker's loop side (docs/METADATA.md 3.2.3, 3.3, 3.4.2;
// milestone N10): every loop pass it asks ScanScheduler (N7) what the one
// card worker may do next, hands it that one step at that priority, and
// takes in what the last step did. The jobs:
//   - covers (ui/Thumbs), on any storage;
//   - on the SD card, over the records Library opened: the validation
//     walk, 2 s after the UI's first frame (3.2.3); the compactions; the
//     tag scan (lib/core CardJobs).
// What the steps do is CardJobs'; this is when they run, what the loop
// learns from them, and what follows:
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
//     gone past (Library::softStale()) ask for it; not when the rest
//     stopped on a read error (the card pulled: the next boot has it).
//     After an update step, built or failed or deferred, what the journal
//     had asks for no other: only new records (or a walk's changes, a
//     Rescan) do.
// The update step itself (3.4.2) is main.cpp's (rebuildLibrary(): the queue
// carried through queue.txt around Library::rebuild()), on the loop, once
// updateDue() says: a walk under way ended (its walk.jnl can't be merged
// before), the journals compacted (a worker step, first), the worker free,
// the status line drawn as "Updating library…". The build on
// the worker behind the "Updating library" fence, the queue's remap split
// around it, is N12's.
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
  // power); `covers`: a cover job may start (the UI is up and lit).
  void loop(ScanScheduler::In in, const Sources& src, ui::Thumbs* thumbs, bool covers);

  // ---- the update step (3.4.2) ----
  // `deferToBoot` (gb!, L4.4): the memory check made to fail, so the step
  // writes the build-at-boot marker as a short PSRAM would.
  void askUpdate(const char* why, bool deferToBoot = false);
  bool deferAsked() const { return deferAsked_; }
  // g0: the walk now, the update step after it whatever it finds.
  void askWalkAndUpdate();
  bool updateWanted() const { return updateWanted_; }
  // The update step may run now (the caller's safe point given): the
  // journals compacted, the worker free, and "Updating library…" shown for
  // a pass. Once true, the caller runs it at once.
  bool updateDue(uint32_t nowMs, bool safePoint);
  // Right before it: the worker's step finished (waited for), the scan's
  // View closed and its chunk written.
  void beforeUpdate();
  // After it (`ok`: built): the scan's state starts over with the new
  // index; "Library updated" (not after a boot's build: that's Library's).
  void afterUpdate(bool ok);
  // The memory check deferred it to the next boot (the marker written).
  void updateDeferred();

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
  // A compaction under way, or the update step: IdlePolicy's LibraryWrite.
  bool libraryWrite() const { return compacting_ || updating_; }
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
  static bool indexed(const char* rel, size_t len, void* ctx);
  void taken(ScanScheduler::Job job, uint32_t nowMs);
  void afterWalk(const cardjobs::Done& d);
  void afterScan(const cardjobs::Done& d, uint32_t nowMs);
  void scanEnded();
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
  bool updating_ = false;
  // The update step.
  bool updateWanted_ = false;
  bool updateShown_ = false;   // "Updating library…" is in the status line
  uint32_t updateShownMs_ = 0;
  static constexpr uint32_t kShowUpdatingMs = 600;  // ... at least this long before the build holds the loop
  bool updateAfterWalk_ = false;
  bool lastCompactFailed_ = false;
  bool deferAsked_ = false;  // gb!
  const char* updateWhy_ = "";
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
