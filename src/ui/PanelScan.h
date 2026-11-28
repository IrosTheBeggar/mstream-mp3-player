// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <optional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ScanSync.h"
#include "ui/LcdLock.h"

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

// The loop task's priority for a frame's last wait and its hold: above the
// audio decoder (2, on this core too), level with the speaker's pump.
constexpr UBaseType_t kHoldPriority = 3;

// The loop task above the decoder while a frame waits out the last of its
// window and holds the bus (a millisecond or two, then 4 ms at a fling's
// top speed, 12 at the most): an MP3's decoder runs some 10 ms at a time,
// and a pass of it across a window made every other frame miss its
// refresh, one across a hold sent the address late (a tear). It has 1.4 s
// of audio buffered and every moment between the holds; rendering stays
// below it.
class HoldBoost {
public:
  HoldBoost() : was_(uxTaskPriorityGet(nullptr)) {
    if (was_ < kHoldPriority) vTaskPrioritySet(nullptr, kHoldPriority);
  }
  ~HoldBoost() {
    if (was_ < kHoldPriority) vTaskPrioritySet(nullptr, was_);
  }
  HoldBoost(const HoldBoost&) = delete;
  HoldBoost& operator=(const HoldBoost&) = delete;

private:
  UBaseType_t was_;
};

// The calling task asleep until `atUs` (esp_timer_get_time()), to a tenth
// of a millisecond: a one-shot timer wakes it (a tick's sleep would end up
// to a millisecond early, every frame, and the wait for the window is
// spent above the decoder). Made at the first use (a semaphore and a
// timer: ~150 B of internal RAM); without them, a tick's sleep that ends
// early.
void sleepUntil(int64_t atUs);

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

  // ---- a picture put in place, whole (Whole, below) ----
  // On (the default): timed to the scan and held above the decoder. Off
  // (console uiw0): put as before, only counted, to compare the two.
  void setWhole(bool on) { wholeOn_ = on; }
  bool wholeOn() const { return wholeOn_; }
  // Since the last take: the pictures; those put at once because their
  // caller said so (a list that moves); of the others, those the scan
  // came on the written line of (ScanSync::met(), by how long the writing
  // really took: shown half written for a refresh); and of them all,
  // those something cut into (the hold took over twice its slowest:
  // another task had the core, or the bus). Each stops at 65,535.
  struct WholeStats {
    uint16_t pictures = 0, atOnce = 0, met = 0, cut = 0;
  };
  WholeStats takeWhole();

private:
  friend class Whole;
  void noteWhole(bool atOnce, bool met, bool cut);

  ScanSync sync_;
  bool ok_ = false;
  bool wholeOn_ = true;
  // The calibration's, for the report (a byte each: 240 samples at most,
  // a handful of wraps. They were 32 bits, and the counts below take the
  // bytes: this object is ui::Ui's, whose size is pinned).
  uint8_t samples_ = 0;
  uint8_t wraps_ = 0;
  WholeStats whole_;
};

// A picture put in place on the glass (nothing moves: a list row drawn
// again where it shows, Now Playing's seek bar, a cover), whole to the eye:
// the one way such a picture is put there.
//
//   { Whole whole(scan, y, lines, width);   // waits, then holds the bus
//     ... push its rows, top to bottom ... }
//
// Composed before this (off the bus), it is then
//   - timed to the panel's scan: begun when the scan will not come on the
//     line being written, whatever the hold takes between its quickest
//     and its slowest (lib/core ScanSync::rows(): the wait is nothing most
//     times, 4.6 ms at the worst for a list row, 5.8 for the tallest
//     picture there is), so no refresh shows it half old, half new;
//   - written in one bus hold, the loop task above the audio decoder for
//     it (HoldBoost, as a list's moves): a decoder pass of 10 ms across
//     the push left it half written for that long.
// Before, each such push went out when its caller got to it, in holds of
// its own, at the loop's priority.
//
// With no scan (`scan` null, the panel's refresh not measured, the line
// unreadable), inside a hold already (a move's fixed parts), or switched
// off (uiw0): the push as it was. So is a picture too tall for its hold to
// stay clear of the scan: no hold here is longer than a list's move. Each
// is counted with what the scan saw of it (PanelScan::WholeStats).
//
// `wait` false: held and above the decoder, but begun at once, wherever
// the scan is. For a list that moves: its next frame has a window of its
// own to make, and a row half new for one refresh of a moving list is
// nothing the eye finds.
//
// Loop task only.
class Whole {
public:
  // Screen rows [y, y + lines), `widthPx` wide.
  Whole(PanelScan* scan, int y, int lines, int widthPx, bool wait = true);
  ~Whole();
  Whole(const Whole&) = delete;
  Whole& operator=(const Whole&) = delete;

private:
  PanelScan* scan_ = nullptr;
  std::optional<LcdLock> lock_;  // the bus, for the whole picture
  int64_t beganUs_ = 0;          // the writing's start
  uint32_t slowestUs_ = 0;       // the most its hold should take
  UBaseType_t was_ = 0;
  int16_t y_ = 0, lines_ = 0;
  int16_t line0_ = 0;            // the scan line at the writing's start
  bool boosted_ = false;
  bool counted_ = false;
  bool atOnce_ = false;
};

}  // namespace ui
