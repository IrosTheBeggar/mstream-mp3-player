// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "ScanSync.h"

// Where the LCD's scan is: the clock a list's frames go by (lib/core
// ScanSync has the why and the windows).
//
// The Core2 doesn't wire the panel's TE pin, but the ILI9342C answers
// Get_Scanline (45h, M5GFX's getScanLine()): the line its scan is at, 0 at
// the first line of the back porch. MEASURED on a Core2 v1.3 (2026-11-09):
// a read takes 16 us on the bus; 244 lines a refresh (240 rows and two
// porches of 2), 13.98 ms: 71.5 Hz from the controller's own oscillator
// (M5GFX leaves the frame rate at its reset value). A sleeping panel
// (sleep-in: the screen off) doesn't scan and reads 0.
//
// calibrate() times a few refreshes (about 45 ms on the loop, once, when
// the UI starts with the panel awake) and takes them only if they make
// sense: the line climbing at a steady rate and wrapping at the same count
// each time. A panel that doesn't answer so (another controller, one that
// reads garbage) leaves ok() false and the list's frames on the frame
// clock, as before. The rate is the oscillator's (an RC: it drifts with
// temperature by a fraction of a percent): every plan reads the line
// again, so only the wait of at most a refresh rests on the measured rate.
//
// Loop task only (it takes the LCD's bus for each read).
namespace ui {

class PanelScan {
public:
  // Measures the refresh. False (and ok() false): the scan can't be read.
  bool calibrate();
  bool ok() const { return ok_; }
  // The scan line now; -1: not one the measured refresh has.
  int line();
  // The band a list scrolls in (screen rows): what the windows are for.
  void setBand(int top, int height) { sync_.setBand(top, height); }
  const ScanSync& sync() const { return sync_; }
  bool ready() const { return ok_ && sync_.ready(); }
  // "[ui] ..." for the log.
  void print() const;

private:
  ScanSync sync_;
  bool ok_ = false;
  uint32_t samples_ = 0;  // the calibration's, for the report
  uint32_t wraps_ = 0;
};

}  // namespace ui
