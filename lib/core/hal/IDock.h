#pragma once
#include <cstdint>

enum class DockEvent { None, Inserted, Removed };

// HAL seam for dock detection + the USB mass-storage handoff.
//
//   Sim build (Wokwi):  SimDock turns the DOCK pushbutton into toggle events,
//                       so the dock state machine is exercisable without USB.
//   Real build:         detection comes from USB VBUS / host enumeration on the
//                       S3's native USB-OTG, and expose/reclaim drive the
//                       TinyUSB MSC class. No simulator models USB device mode,
//                       so this seam stays stubbed until the physical board.
class IDock {
public:
  virtual ~IDock() = default;

  virtual void begin() = 0;
  virtual DockEvent poll(uint32_t nowMs) = 0;   // debounced edge events
  virtual bool isDocked() const = 0;

  virtual void exposeMassStorage() = 0;         // hand the SD card to the host
  virtual void reclaimMassStorage() = 0;        // take it back for playback
};
