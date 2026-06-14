#include "dock/SimDock.h"

#include <Arduino.h>

#include "Pins.h"

namespace {
constexpr uint32_t kDebounceMs = 40;
}

void SimDock::begin() {
  pinMode(PIN_DOCK, INPUT_PULLUP);
  lastRaw_ = digitalRead(PIN_DOCK);
}

DockEvent SimDock::poll(uint32_t nowMs) {
  const int raw = digitalRead(PIN_DOCK);
  if (raw != lastRaw_ && (nowMs - lastChangeMs_) >= kDebounceMs) {
    lastChangeMs_ = nowMs;
    lastRaw_ = raw;
    if (raw == LOW) {            // press = toggle dock state
      docked_ = !docked_;
      return docked_ ? DockEvent::Inserted : DockEvent::Removed;
    }
  }
  return DockEvent::None;
}

void SimDock::exposeMassStorage() {
  Serial.println("[DOCK] exposing SD as USB mass-storage (simulated)");
}

void SimDock::reclaimMassStorage() {
  Serial.println("[DOCK] reclaiming SD from host (simulated)");
}
