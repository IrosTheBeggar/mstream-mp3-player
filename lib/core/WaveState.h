// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CardContainer.h"
#include "WaveShape.h"

// What Now Playing wants of the waveform index, what came back, and when to
// read again (docs/WAVEFORM-INDEX.md 11): all of the read path but the card
// read. The page wants the playing track's path hash and asks for shape(); the
// card worker reads when wantsRead() says so, one job at a time, with nothing
// held between jobs (wave::readShape(), its job's PSRAM); the loop takes the
// answer in with finished(). One record is kept, whichever hash it answers for,
// so a repaint (a sheet closed, a dark wake) reads no card.
//
// - Keyed by path hash, not by track id: a library update's renumbering wants
//   the same hash again, and that costs nothing.
// - A want while a job runs for another hash: the late answer is dropped, and
//   the new hash read next. A burst of skips costs the job under way and the
//   last hash's.
// - Hit: the record. Miss and Damaged: none for this hash, known (a track with
//   no data, or a slot the card damaged). Absent: the file is off until a
//   remount, whichever track wanted it, and even a shape read before goes.
// - Io: read again once after kIoRetryMs, then none until the track changes.
// - A remount: everything goes, the file may be another, and an answer that
//   started before it is dropped. The wanted hash is asked for again.
//
// version() is what the page compares in its update(): no callback, as
// namesVersion(). A test hook (the console's gw=, gw-) sets a shape with no card
// at all: it shows on whatever plays until it is dropped, and no read runs under
// it.
//
// Loop only, no clock of its own. Portable, host-tested (test_wave_state).
class WaveState {
public:
  static constexpr uint32_t kIoRetryMs = 2000;
  static constexpr uint32_t kIoReads = 2;       // the failed reads after which a track has none
  static constexpr uint32_t kRefusedMs = 1000;  // the worker wouldn't take a job: asked again after this (as covers)

  // The file: the root names none; it does; it did, and it failed (why() says
  // how: off until a remount).
  enum class File : uint8_t { None, Named, Absent };
  // What finished() did with an answer. Dropped: it wasn't wanted any more.
  // Recorded: shape() has it. None: none for this hash. Absent: the file went
  // off (once, for the log's one line). Retry: read again after kIoRetryMs.
  // GaveUp: none until the track changes.
  enum class Finish : uint8_t { Dropped, Recorded, None, Absent, Retry, GaveUp };

  // At the boot: whether the root names a file (cardroot::Root::waves.present).
  void begin(bool named);

  // The track Now Playing shows: its path hash, 0 when it has none (a built-in
  // tone, no library). The hash it holds already: nothing changes. Another: its
  // record, if that is the one kept, shows with no read.
  void want(uint64_t pathHash);
  uint64_t wanted() const { return wanted_; }

  // A job should start now: a hash is wanted, its answer isn't known, the file
  // may be read, and no job is under way.
  bool wantsRead(uint32_t nowMs) const;
  // The worker took it, for wanted(); or it wouldn't.
  void started();
  void refused(uint32_t nowMs);
  // The answer, on the loop. `nowMs` starts a retry's wait.
  Finish finished(const wave::ReadResult& r, uint32_t nowMs);

  // The card was mounted again. `named`: the root read again names a file.
  void remounted(bool named);

  // The shape to draw: the wanted hash's record once it came, else nullptr.
  // The pointer is the record's: finished() and inject() write over it.
  const wave::Shape* shape() const;
  // Moves whenever shape() may have changed.
  uint32_t version() const { return version_; }
  // Moves at each remount: an answer from before one is dropped.
  uint32_t generation() const { return generation_; }

  // The test hook: `heights` (wave::kColumns, each cut to 0-9) as the shape,
  // whatever is wanted. `lengthMs` 0 agrees with any length.
  void inject(const uint8_t* heights, uint32_t lengthMs = 0);
  void dropInjected();
  bool injected() const { return injected_; }

  // For the console (gw?).
  File file() const { return file_; }
  cardcontract::Why why() const { return why_; }  // file() Absent: the check that failed
  bool reading() const { return running_; }
  uint64_t heldHash() const { return heldHash_; }  // the hash the record answers for; 0: none
  bool heldShape() const { return heldShape_; }    // ... with a shape, not with none
  uint32_t failedReads() const { return ioFails_; }  // the wanted hash's so far
  // Since the boot: the jobs taken in; each ended in one of the next six. Then
  // the slots they read, and their time.
  uint32_t reads() const { return reads_; }
  uint32_t hits() const { return hits_; }
  uint32_t misses() const { return misses_; }
  uint32_t damaged() const { return damaged_; }
  uint32_t absents() const { return absents_; }
  uint32_t ioErrors() const { return ioErrors_; }
  uint32_t dropped() const { return dropped_; }
  uint32_t slotReads() const { return slotReads_; }
  uint32_t meanJobMs() const;  // 0: none yet
  uint32_t maxJobMs() const { return jobMsMax_; }

private:
  // The record answers for the wanted hash, with a shape.
  bool holds() const { return heldShape_ && heldHash_ == wanted_; }
  void wait(uint32_t nowMs, uint32_t forMs);

  File file_ = File::None;
  cardcontract::Why why_ = cardcontract::Why::Ok;
  uint64_t wanted_ = 0;
  uint64_t heldHash_ = 0;
  bool heldShape_ = false;
  bool injected_ = false;
  wave::Shape shape_;  // the record's, or the hook's
  bool running_ = false;
  uint64_t jobHash_ = 0;
  uint32_t jobGeneration_ = 0;
  uint32_t generation_ = 0;
  uint32_t version_ = 0;
  uint32_t ioFails_ = 0;
  bool waiting_ = false;
  uint32_t waitFromMs_ = 0, waitMs_ = 0;
  uint32_t reads_ = 0, hits_ = 0, misses_ = 0, damaged_ = 0, absents_ = 0, ioErrors_ = 0, dropped_ = 0;
  uint32_t slotReads_ = 0;
  uint64_t jobMsSum_ = 0;
  uint32_t jobMsMax_ = 0;
};
