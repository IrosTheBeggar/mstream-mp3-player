#pragma once
#include <cstdint>
#include <functional>
#include "hal/IDock.h"

enum class DockState { Undocked, Docked };

// Coordinates the dock handoff: when the player is docked it stops playback,
// releases the SD card to the host as USB mass-storage, and shows a docked
// screen; on undock it reclaims the card and rescans the library.
//
// The side effects are injected as callbacks so this stays host-testable with
// a fake IDock — no audio/storage/display dependencies leak in here.
class DockController {
public:
  explicit DockController(IDock& dock) : dock_(dock) {}

  void begin() { dock_.begin(); }
  void update(uint32_t nowMs);
  DockState state() const { return state_; }

  std::function<void()> onDock;     // stop playback, release SD to host
  std::function<void()> onUndock;   // reclaim SD, rescan library

private:
  IDock& dock_;
  DockState state_ = DockState::Undocked;
};
