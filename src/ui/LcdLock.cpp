#include "ui/LcdLock.h"

#include <M5Unified.h>
#include <esp_timer.h>

SpiHoldStats& LcdLock::global() {
  static SpiHoldStats stats;
  return stats;
}

LcdLock::LcdLock(SpiHoldStats* stats) : stats_(stats) {
  // Only the outermost startWrite() takes the bus (and its mutex); a nested
  // one only counts. Time what the SD card actually waits for.
  outermost_ = M5.Display.getStartCount() == 0;
  M5.Display.startWrite();
  t0_ = esp_timer_get_time();
}

LcdLock::~LcdLock() {
  M5.Display.endWrite();
  if (!outermost_) return;
  const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0_);
  global().add(us);
  if (stats_) stats_->add(us);
}

uint32_t LcdLock::heldUs() const { return static_cast<uint32_t>(esp_timer_get_time() - t0_); }
