// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>

#include "PowerWindow.h"

class LocalStorage;

// What the Core2 draws, read from its power chip (AXP192) through
// M5Unified's I2C, for measuring what the firmware costs (the console's P
// commands, app/PowerLab; docs/ARCHITECTURE.md "Power measurement").
//
// While anything wants it (a P line, the 5 s log, the CSV file), it reads
// the chip ~10 times a second from the loop task: reg 0x00 (which supply
// is there), 0x56-0x5F (ACIN and VBUS voltage and current, the chip's
// temperature) and 0x78-0x7F (battery voltage, charge and discharge
// current, the APS system rail): three I2C transactions, ~0.7 ms. The
// samples go into 5 s windows (power::Window): mean, min and max. When
// nothing wants it, loop() returns at once: no I2C, nothing kept.
//
// The ADCs it needs are switched on the first time (M5Unified already has
// them all on: reg 0x82 = 0xFF, 0x83 = 0x80, 25 Hz). The AXP192's
// battery current only sees the battery: with USB in, the Core2 runs from
// USB, so measure either on USB with a full battery (the input current is
// then the device's) or on battery alone with the CSV file on the card
// (serial goes with the cable). The coulomb counter (Pq) integrates the
// battery current in the chip itself, for overnight runs.
class PowerProbe {
public:
  // Appends the device's state to a line (the backlight, the CPU clock, the
  // output, the knobs): spaces only, no commas (it also goes in the CSV).
  using Describe = std::function<void(char* buf, size_t size)>;

  PowerProbe(LocalStorage& storage, Describe describe) : storage_(storage), describe_(std::move(describe)) {}

  // Loop task, every pass: samples when a sample is due, prints or writes
  // a window when it's over.
  void loop(uint32_t nowMs);

  // P: one [power] line at the end of the window in progress (a 5 s one
  // from now if nothing was being measured).
  void requestLine(uint32_t nowMs);
  // Pl: a [power] line every 5 s, on / off.
  void toggleLog(uint32_t nowMs);
  // Pw: every window (and every mark) appended to /.player/power.csv on the
  // card, on / off; for runs on battery, where the serial cable is out.
  void toggleCsv(uint32_t nowMs);
  // Pm<name>: a marker in the log and the CSV. The window in progress ends
  // there (printed as partial), and the next starts after the change.
  void mark(const char* name, uint32_t nowMs);
  // Pq: the coulomb counter. "" reads it, "1" clears and starts it, "0" stops it.
  void coulomb(const char* arg, uint32_t nowMs);

  // A sample now, outside the windows (a knob's before/after): false when
  // the chip didn't answer.
  bool sampleNow(power::Sample* out);
  bool logging() const { return log_; }
  bool csv() const { return csv_; }

private:
  static constexpr uint32_t kSampleMs = 100;
  static constexpr uint32_t kWindowMs = 5000;

  bool wanted() const { return log_ || csv_ || oneShot_; }
  void ensureAdcs();
  bool read(power::Sample* out);
  void startWindow(uint32_t nowMs);
  void emit(uint32_t nowMs, bool partial);
  void appendCsv(const char* row);
  void coulombText(char* buf, size_t size, uint32_t nowMs);

  LocalStorage& storage_;
  Describe describe_;
  power::Window window_;
  bool sampling_ = false;
  bool adcsChecked_ = false;
  bool log_ = false;
  bool csv_ = false;
  bool oneShot_ = false;
  uint32_t nextSampleMs_ = 0;
  uint32_t readFailures_ = 0;
  bool csvHeader_ = false;       // the header was checked/written this boot
  uint32_t coulombSinceMs_ = 0;  // Pq1's millis(), 0: not started here
};
