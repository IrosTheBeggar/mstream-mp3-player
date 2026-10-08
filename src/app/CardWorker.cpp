// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/CardWorker.h"

#include <Arduino.h>
#include <esp_timer.h>

void CardWorker::entry(void* self) { static_cast<CardWorker*>(self)->work(); }

void CardWorker::work() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint8_t st = state_.load();
    if (st == kExit) {
      stackLeft_ = uxTaskGetStackHighWaterMark(nullptr);
      // Not alive first, then Idle (Thumbs' order): the loop never sees Idle
      // with this task still counted as alive, which would hand a step to a
      // task about to delete itself.
      alive_.store(false);
      state_.store(kIdle);
      vTaskDelete(nullptr);
    }
    if (st != kQueued) continue;
    state_.store(kWorking);
    const int64_t t0 = esp_timer_get_time();
    fn_(ctx_);
    tookUs_.store(static_cast<uint32_t>(esp_timer_get_time() - t0));
    stackLeft_ = uxTaskGetStackHighWaterMark(nullptr);
    state_.store(kDone);
  }
}

bool CardWorker::start(Job job, uint8_t priority, StepFn fn, void* ctx, uint32_t nowMs) {
  if (state_.load() != kIdle || !fn) return false;
  if (!alive_.load()) {
    if (static_cast<int32_t>(nowMs - retryAtMs_) < 0) return false;
    alive_.store(true);
    priority_ = priority;
    if (xTaskCreatePinnedToCore(entry, "card", kStackBytes, this, priority, &task_, 1) != pdPASS) {
      alive_.store(false);
      task_ = nullptr;
      retryAtMs_ = nowMs + kRetryMs;
      if (failedStarts_++ == 0) {
        Serial.printf("[card] no internal RAM for the card worker's %lu B stack: its jobs wait\n",
                      (unsigned long)kStackBytes);
      }
      return false;
    }
  }
  fn_ = fn;
  ctx_ = ctx;
  job_ = job;
  setPriority(priority);
  state_.store(kQueued);
  xTaskNotifyGive(task_);
  lastStepMs_ = nowMs;
  return true;
}

CardWorker::Job CardWorker::running() const {
  const uint8_t st = state_.load();
  return st == kQueued || st == kWorking ? job_ : Job::None;
}

bool CardWorker::poll(uint32_t nowMs, Job* job, uint32_t* ms) {
  bool got = false;
  if (state_.load() == kDone) {
    *job = job_;
    *ms = (tookUs_.load() + 500) / 1000;
    ++steps_;
    job_ = Job::None;
    lastStepMs_ = nowMs;
    state_.store(kIdle);
    got = true;
  }
  // Nothing to do for a while: the task ends, its stack goes back.
  if (alive_.load() && state_.load() == kIdle && nowMs - lastStepMs_ >= kIdleExitMs) {
    state_.store(kExit);
    xTaskNotifyGive(task_);
  }
  return got;
}

void CardWorker::setPriority(uint8_t priority) {
  if (!task_ || !alive_.load() || priority_ == priority) return;
  const uint8_t st = state_.load();
  if (st == kExit) return;
  vTaskPrioritySet(task_, priority);
  priority_ = priority;
}

bool CardWorker::waitIdle(uint32_t maxMs) {
  const uint32_t t0 = millis();
  for (;;) {
    const uint8_t st = state_.load();
    if (st != kQueued && st != kWorking) return true;
    if (millis() - t0 >= maxMs) return false;
    vTaskDelay(1);  // the worker (at 0 or 1) runs meanwhile
  }
}
