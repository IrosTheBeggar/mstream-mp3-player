#include "DockController.h"

void DockController::update(uint32_t nowMs) {
  const DockEvent ev = dock_.poll(nowMs);

  if (ev == DockEvent::Inserted && state_ == DockState::Undocked) {
    if (onDock) onDock();          // stop playback + unmount before handing off
    dock_.exposeMassStorage();
    state_ = DockState::Docked;
  } else if (ev == DockEvent::Removed && state_ == DockState::Docked) {
    dock_.reclaimMassStorage();
    if (onUndock) onUndock();      // remount + rescan after taking the card back
    state_ = DockState::Undocked;
  }
}
