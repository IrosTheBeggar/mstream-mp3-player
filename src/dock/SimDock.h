#pragma once
#include "hal/IDock.h"

// Simulated dock for Wokwi: the DOCK pushbutton toggles docked/undocked, so the
// DockController state machine and the MSC handoff path are fully exercisable
// without USB. Each press emits one Inserted or Removed edge (debounced).
//
// On hardware, replace poll() with USB VBUS / host-enumeration detection and
// wire expose/reclaim to the TinyUSB MSC class.
class SimDock : public IDock {
public:
  void begin() override;
  DockEvent poll(uint32_t nowMs) override;
  bool isDocked() const override { return docked_; }

  void exposeMassStorage() override;
  void reclaimMassStorage() override;

private:
  bool docked_ = false;
  int lastRaw_ = 1;          // INPUT_PULLUP idle = HIGH
  uint32_t lastChangeMs_ = 0;
};
