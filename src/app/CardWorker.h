// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

#include "ScanScheduler.h"

// The one card worker (docs/METADATA.md 3.3.4; milestone N10): a task on
// core 1, below the audio decoder (2), that takes one step at a time of
// whatever the loop hands it: a cover (ui/Thumbs), a folder of the
// validation walk, a compaction, a file of the scan (app/CardTasks,
// lib/core CardJobs). Thumbs' worker, generalised: it runs only the step
// it is handed, so no two jobs overlap, and the loop (ScanScheduler)
// decides what starts and at what priority:
//   - 1 (the loop's) for a cover on screen and the update step's build;
//   - 0 (the idle task's) for the walk, the scan and the compaction: the SD
//     driver's reads busy-wait the CPU, so at the loop's priority a step
//     would time-slice with the loop for its whole length; at 0 the loop
//     preempts it whenever it is ready.
// Its priority follows the step under way (a list that starts moving drops
// a cover to 0).
//
// Its 6 KB stack is in internal RAM (a task that reads the card or the
// flash can't have its stack in PSRAM), only while there is work: the task
// is made for the first step and ends itself kIdleExitMs after the last.
// What the steps hold is in PSRAM (their jobs' memory, FatFs's FIL, DIR
// and FILINFO); on the stack: TagScan's about 1 KB, FatFs's 512 B long-name
// buffer, the frames. The 'gs' line prints its stack's high-water mark,
// the least left over every life of the task since the boot or `gs0` (a
// task's own mark starts again when it is made), and the internal RAM's
// lowest while a step ran (L3 and L4 measure them).
//
// Loop task only, but for the step itself.
class CardWorker {
public:
  using Job = ScanScheduler::Job;
  using StepFn = void (*)(void* ctx);
  static constexpr uint32_t kStackBytes = 6144;
  static constexpr uint32_t kIdleExitMs = 3000;

  // A step of `job` at FreeRTOS `priority` (ScanScheduler's 0 or 1): `fn(ctx)`
  // on the worker. False: one is under way or waiting to be taken in, or
  // no internal RAM for the task (asked again after kRetryMs).
  bool start(Job job, uint8_t priority, StepFn fn, void* ctx, uint32_t nowMs);
  // A step is handed and not yet taken in by poll().
  bool busy() const { return state_.load() != kIdle; }
  // The step under way (None: none, or finished and waiting for poll()).
  Job running() const;
  // Every loop pass: the finished step, if one (its job and its time in
  // ms); the task's end after kIdleExitMs without a step. True: `job` and
  // `ms` are a finished step's.
  bool poll(uint32_t nowMs, Job* job, uint32_t* ms);
  // The step under way's priority (ScanScheduler's Out::priority).
  void setPriority(uint8_t priority);
  // Waits (yielding) until no step is under way, at most `maxMs`: before
  // the loop itself uses the card's records (the update step, the idle
  // power-off's flush). True: free.
  bool waitIdle(uint32_t maxMs);

  bool alive() const { return alive_.load(); }
  // Its stack's high-water mark: the least left over every life of the
  // task since resetStats() (bytes; 0: no step yet), and this life's.
  uint32_t stackLeastLeft() const { return resetLeast_.load() || leastLeft_ == UINT32_MAX ? 0 : leastLeft_; }
  uint32_t stackLeft() const { return stackLeft_; }
  // Internal RAM's lowest free, sampled as each step starts and ends (a
  // step's own dip between them isn't seen: CardTasks samples the loop's
  // passes too, noteInternal()); UINT32_MAX: none yet.
  uint32_t internalMin() const { return internalMin_.load(); }
  void noteInternal(uint32_t freeBytes);
  // gs0 (L3's figures per condition): the stack's and internal RAM's
  // lowest, and the steps' count, start again.
  void resetStats();
  uint32_t steps() const { return steps_; }
  uint32_t failedStarts() const { return failedStarts_; }

private:
  static constexpr uint8_t kIdle = 0, kQueued = 1, kWorking = 2, kDone = 3, kExit = 4;
  static constexpr uint32_t kRetryMs = 5000;
  static void entry(void* self);
  void work();

  std::atomic<uint8_t> state_{kIdle};
  std::atomic<bool> alive_{false};
  TaskHandle_t task_ = nullptr;
  StepFn fn_ = nullptr;
  void* ctx_ = nullptr;
  Job job_ = Job::None;
  uint8_t priority_ = 0;
  uint32_t startedUs_ = 0;  // esp_timer, the step's start (the worker's)
  std::atomic<uint32_t> tookUs_{0};
  uint32_t lastStepMs_ = 0;
  uint32_t retryAtMs_ = 0;
  uint32_t stackLeft_ = 0;
  uint32_t leastLeft_ = UINT32_MAX;  // written by the worker, read by the loop (a diagnostic)
  std::atomic<uint32_t> internalMin_{UINT32_MAX};
  std::atomic<bool> resetLeast_{false};
  uint32_t steps_ = 0;
  uint32_t failedStarts_ = 0;
};
