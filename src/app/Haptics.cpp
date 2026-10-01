// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Haptics.h"

#include <M5Unified.h>
#include <esp_timer.h>

namespace {
// The one instance (not the timer's ID: pvTimerGetTimerID() is one of the
// FreeRTOS functions ESP-IDF places in IRAM, and IRAM is nearly full).
Haptics* self = nullptr;
}  // namespace

bool Haptics::begin() {
  self = this;
  timer_ = xTimerCreate("haptic", 1, pdFALSE, nullptr, onTimer);
  return timer_ != nullptr;
}

void Haptics::onTimer(TimerHandle_t) {
  if (self) self->step();
}

// Timer service task: apply the next step and arm the timer for its length.
void Haptics::step() {
  Step s{0, 0};
  bool done = false;
  portENTER_CRITICAL(&mux_);
  if (next_ < count_) {
    s = steps_[next_++];
  } else {
    done = true;
  }
  portEXIT_CRITICAL(&mux_);

  const int64_t now = esp_timer_get_time();
  if (onSinceUs_) {  // the motor was on until now
    onUs_ += static_cast<uint32_t>(now - onSinceUs_);
    onSinceUs_ = 0;
  }
  if (done) {
    M5.Power.setVibration(0);
    lastOnUs_ = onUs_;
    onUs_ = 0;
    busy_ = false;
    return;
  }
  M5.Power.setVibration(s.level);
  if (s.level) onSinceUs_ = esp_timer_get_time();
  const TickType_t ticks = pdMS_TO_TICKS(s.ms) ? pdMS_TO_TICKS(s.ms) : 1;
  xTimerChangePeriod(timer_, ticks, 0);  // also starts it
}

void Haptics::play(const Step* steps, int count) {
  if (!timer_ || !enabled_ || count <= 0) return;
  if (count > kMaxSteps) count = kMaxSteps;
  portENTER_CRITICAL(&mux_);
  for (int i = 0; i < count; ++i) {
    steps_[i] = steps[i];
    if (steps_[i].level && steps_[i].level < kMinLevel) steps_[i].level = kMinLevel;  // under 1.8 V: LDO3 off
  }
  count_ = count;
  next_ = 0;
  portEXIT_CRITICAL(&mux_);
  busy_ = true;
  xTimerChangePeriod(timer_, 1, 0);  // the first step at the next tick, on the timer task
}

void Haptics::tick(uint16_t ms, uint8_t level) {
  const Step s{level, ms};
  play(&s, 1);
}

void Haptics::pulses(uint16_t ms, uint8_t level, int count, uint16_t gapMs) {
  Step s[kMaxSteps];
  int n = 0;
  for (int i = 0; i < count && n + 1 < kMaxSteps; ++i) {
    if (i) s[n++] = {0, gapMs};
    s[n++] = {level, ms};
  }
  play(s, n);
}

void Haptics::stop() {
  if (!timer_) return;
  portENTER_CRITICAL(&mux_);
  count_ = 0;
  next_ = 0;
  portEXIT_CRITICAL(&mux_);
  xTimerChangePeriod(timer_, 1, 0);  // the next step finds nothing and switches the motor off
}

void Haptics::setEnabled(bool on) {
  enabled_ = on;
  if (!on) stop();
}
