// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "AutoDjOpen.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "WaveState.h"
#include "app/WavesContract.h"  // wavesReadMpwf()

class CardWorker;
class Library;

// The waveform index on the device (docs/WAVEFORM-INDEX.md 11): the shape Now
// Playing's bar draws when the transfer wrote one for the playing track. The
// decisions are host-tested in lib/core (WaveState: what is wanted, what came
// back, when to read again; wave::readShape(): the read; WaveBar: where the
// columns go). This is what ties them to the card: the root's row, the worker's
// job, and the console's gw letters.
//
//   Ui::begin(), in setup() after the library: begin(): the path and the entry
//     the boot's root names (Library::root()), if this build reads the major
//     (read.mpwf), and one log line. No card read: the file isn't opened until
//     a track wants it.
//   Now Playing, on a track change: want() its path hash; in its update(),
//     shape(), and version() to see that it changed (no callback, as
//     namesVersion()). Loop only, no card.
//   CardTasks, with the worker free: wantsRead() (ScanScheduler's In::wave),
//     then start(): the read is the worker's step, in the covers' lane
//     (Job::Wave). It opens the file through FatFs itself (not the VFS),
//     checks it against the entry, looks the slot up, reduces the bars and
//     closes the file. Opened per job and nothing open between jobs: no file
//     outlives a remount. Its FIL, 4.1 KB of PSRAM, is the job's, kept: no open
//     fails for want of memory. About 20 ms (ESTIMATED).
//   Ui::loop(), beside Thumbs' (in the dark too): loop(): the answer taken in.
//     One that started for another track, or before a remount, is dropped. A
//     file that isn't on the card, or fails its checks, is one log line, and
//     plain lines until a remount or the next boot. An open the card fails is a
//     failed read, as a slot's: read again once.
//   main.cpp, right after the card's guard: remount(): a remount seen
//     (sectordisk's counter, as AutoDj::remount(), but simpler: nothing is
//     open): the root read again into a Root of the moment (never
//     Library::root(): the update step holds it, TagStore its identity), its
//     row taken, the record dropped, the wanted hash asked for again. A read of
//     the root the card fails, here or at the boot, says nothing of the file:
//     read again after Auto DJ's waits (autodj::OpenTries), plain lines meanwhile,
//     and the remount such a read brings is taken for its echo, as Auto DJ's. (No
//     such waits when the boot failed it and the card jobs run: only a remount.)
//
// Nothing to do at a library update: the key is the file's path hash, so its
// Live keeps the record, and the page's want() on the new ids reads nothing.
// Behind its fence the page wants nothing new (no index), and the last record
// stays. In the dark, under a modal or on another tab nothing wants: the page's
// update() doesn't run.
//
// On the worker's 6 KB of stack: the deepest is the file's open, about 2 KB
// ESTIMATED from the image's frames (FatFile::openRead()'s path and f_open()'s
// long-name buffer, 960 B, over a read of the card), 3.6 KB with the SD driver's
// log line on top. The reduction's 500 B come after the file's last read. What
// the job holds is in PSRAM: the Job (the FIL, the Table, the Slot, the result:
// 4.9 KB), made when a root first names a file. This object is a member of ui::Ui,
// in PSRAM too (352 B): nothing in internal RAM.
//
// Loop task only, but for the step (stepEntry()), which touches only the job it
// is handed.
class Waves {
public:
  explicit Waves(Library& library) : library_(library) {}

  void begin();

  // ---- for Now Playing ----
  // The playing track's path hash, 0 when it has none (WaveState::want()).
  void want(uint64_t pathHash) { state_.want(pathHash); }
  // The wanted track's shape once it came, else nullptr. Moves on version().
  const wave::Shape* shape() const { return state_.shape(); }
  uint32_t version() const { return state_.version(); }

  // ---- for the card worker (app/CardTasks) ----
  // A read should start now (ScanScheduler's In::wave).
  bool wantsRead(uint32_t nowMs) const { return job_ && state_.wantsRead(nowMs); }
  // The read as the worker's step, at `priority`. False: nothing to read, or
  // the worker couldn't take it (asked again after WaveState::kRefusedMs).
  bool start(CardWorker& worker, uint8_t priority, uint32_t nowMs);

  // Every loop pass: takes in what the worker read.
  void loop(uint32_t nowMs);
  // Every loop pass, right after the card's guard. `cardHeld`: another card,
  // or a remount with a job under way: a restart comes, and nothing is read.
  // `retryHeld`: a read of the root that failed waits to be tried again (as
  // AutoDj::stepOpen(): before the UI's first frame, while a list moves, behind the
  // update's fence); a remount doesn't.
  void remount(bool cardHeld, bool retryHeld);

  // The console's gw letters: `arg` from the letter after the w (tagtext's
  // Command::Wave).
  void command(const char* arg);

private:
  enum State : uint8_t { Idle, Queued, Working, Done };
  struct Job;  // Waves.cpp: the one in flight, shared with the worker

  static void stepEntry(void* job);
  // The root's row into path_ and entry_ (and the job's memory made). False:
  // none to read, and whyNone_ says why.
  bool take(const cardroot::Root& root);
  // The card failed a read of the root: read again soon, or none until a remount.
  void rootFailed();
  void printState() const;

  Library& library_;
  WaveState state_;
  Job* job_ = nullptr;  // PSRAM, made when a root first names a file
  char path_[sizeof(cardroot::Root::Waves::path)] = "";
  cardcontract::msmf::Comp entry_;
  const char* whyNone_ = "no card records (the flash, or no PSRAM)";  // WaveState::File::None's
  uint32_t remountsSeen_ = 0;
  uint32_t remounts_ = 0;
  autodj::OpenTries rootTries_;
  bool rootAgain_ = false;  // a read of the root failed: read again when rootTries_ says
  bool silenced_ = false;   // CachedDrive::tooSilent(): no more of its reads this session
};
