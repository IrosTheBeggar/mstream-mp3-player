// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "TouchPower.h"

// The Core2's small consumers that nothing uses (docs/ENERGY.md item 9),
// each below the probe's noise, together 2.5-3.9 USB mA (measured):
//
// - the IMU (BMI270): suspended at boot (~3.5 uA); nothing reads it.
// - the 5 V boost (the AXP192's EXTEN, the M-Bus/Grove 5 V): off from
//   M5.begin() (main.cpp sets cfg.output_power = false). The speaker amp
//   isn't fed from it on USB (measured); still to check by ear on battery.
// - the green LED: off (M5Unified's default, led_brightness 0).
//
// - the touch controller (FT6336U, docs/ENERGY.md section 5, P1): its power
//   registers on a boot line. Nothing to set: it drops to Monitor (its slow
//   scan) by itself 30 s after the last touch (reg 0x86 = 1, 0x87 = 30, read
//   on the device), and Monitor against Active measured under 1 USB mA.
//
// The console's Pi, Pe, Pg and Pf switch them back for measurements
// (app/PowerLab). Loop task: the touch controller shares its I2C port with
// the AXP192 and the IMU, and M5.update() reads the touch there.
namespace board {

// setup(), after M5.begin(): the IMU suspended, and one [power] line saying
// what the boot left on or off.
void applyBootPower();

// The BMI270's I2C address (0x68 or 0x69), -1 if there isn't one (only that
// IMU is handled).
int bmi270Address();
// Its PWR_CTRL (0x7D) and PWR_CONF (0x7C).
uint8_t bmi270PwrCtrl(int addr);
uint8_t bmi270PwrConf(int addr);
// Suspended (everything off, then advanced power save) or on again as
// M5Unified leaves it (temperature, accel, gyro).
void setImuSuspended(int addr, bool suspended);
bool imuSuspended();

// ---- the touch controller (FT6336U at 0x38; lib/core/TouchPower.h) ----

// Its power registers (0x86-0x89 and 0xA5), read now. False: no answer.
bool readTouchPower(touchpower::Regs* out);
// 0xA5 alone, read now; -1: no answer.
int readTouchMode();
// Active or Monitor (anything else is refused: Hibernate needs the LCD's
// reset). False: refused, or no answer.
bool setTouchMode(uint8_t mode);
// 0xA5 as last read or written (-1: never, or no answer). The P line shows
// this rather than reading the chip every 5 s; a touch can have taken it
// from Monitor back to Active since.
int touchModeKnown();

}  // namespace board
