// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/BoardPower.h"

#include <M5Unified.h>

namespace board {
namespace {
// BMI270 (datasheet rev 1.x): chip id 0x24 at reg 0x00; PWR_CONF 0x7C
// (bit 0 advanced power save), PWR_CTRL 0x7D (bit 3 temperature, 2 accel,
// 1 gyro, 0 aux). Suspend: everything off, then power save on (~3.5 uA).
constexpr uint8_t kBmiChipId = 0x24;
constexpr uint8_t kBmiRegChipId = 0x00;
constexpr uint8_t kBmiPwrConf = 0x7C;
constexpr uint8_t kBmiPwrCtrl = 0x7D;
constexpr uint8_t kBmiOnCtrl = 0x0E;  // as M5Unified leaves it: temperature, accel, gyro
constexpr uint32_t kI2cHz = 400000;

bool suspended = false;

// The touch controller's 0xA5, as last read or written.
int touchMode = -1;
}  // namespace

int bmi270Address() {
  if (M5.Imu.getType() != m5::imu_t::imu_bmi270) return -1;
  for (uint8_t addr : {0x68, 0x69}) {
    if (M5.In_I2C.readRegister8(addr, kBmiRegChipId, kI2cHz) == kBmiChipId) return addr;
  }
  return -1;
}

uint8_t bmi270PwrCtrl(int addr) { return M5.In_I2C.readRegister8(addr, kBmiPwrCtrl, kI2cHz); }
uint8_t bmi270PwrConf(int addr) { return M5.In_I2C.readRegister8(addr, kBmiPwrConf, kI2cHz); }

void setImuSuspended(int addr, bool suspend) {
  if (suspend) {
    M5.In_I2C.writeRegister8(addr, kBmiPwrCtrl, 0x00, kI2cHz);  // accel, gyro, temperature off
    delayMicroseconds(1000);
    M5.In_I2C.writeRegister8(addr, kBmiPwrConf, 0x01, kI2cHz);  // advanced power save: suspend
  } else {
    M5.In_I2C.writeRegister8(addr, kBmiPwrConf, 0x00, kI2cHz);  // power save off first
    delayMicroseconds(1000);                                     // (450 us before the next write)
    M5.In_I2C.writeRegister8(addr, kBmiPwrCtrl, kBmiOnCtrl, kI2cHz);
  }
  delayMicroseconds(1000);
  suspended = suspend;
}

bool imuSuspended() { return suspended; }

bool readTouchPower(touchpower::Regs* out) {
  uint8_t block[4] = {};
  uint8_t mode = 0;
  // (The chip's register pointer moves on by itself: 0x86-0x89 in one read.)
  if (!M5.In_I2C.readRegister(touchpower::kAddress, touchpower::kRegCtrl, block, sizeof(block), kI2cHz) ||
      !M5.In_I2C.readRegister(touchpower::kAddress, touchpower::kRegMode, &mode, 1, kI2cHz)) {
    touchMode = -1;
    return false;
  }
  *out = touchpower::fromBytes(block, mode);
  touchMode = mode;
  return true;
}

int readTouchMode() {
  uint8_t mode = 0;
  touchMode = M5.In_I2C.readRegister(touchpower::kAddress, touchpower::kRegMode, &mode, 1, kI2cHz) ? mode : -1;
  return touchMode;
}

bool setTouchMode(uint8_t mode) {
  if (mode != touchpower::kActive && mode != touchpower::kMonitor) return false;
  if (!M5.In_I2C.writeRegister8(touchpower::kAddress, touchpower::kRegMode, mode, kI2cHz)) {
    touchMode = -1;
    return false;
  }
  touchMode = mode;
  return true;
}

int touchModeKnown() { return touchMode; }

void applyBootPower() {
  const int addr = bmi270Address();
  if (addr >= 0) setImuSuspended(addr, true);
  const bool axp192 = M5.Power.getType() == m5::Power_Class::pmic_axp192;
  // The LED is the AXP192's PWM1: duty register 0x9A, 255 = off.
  Serial.printf("[power] boot: IMU %s, 5 V boost (EXTEN) %s, green LED %s\n",
                addr < 0 ? "not a BMI270: left as it is" : suspended ? "suspended" : "on",
                M5.Power.getExtOutput() ? "ON" : "off",
                !axp192 ? "?" : M5.Power.Axp192.readRegister8(0x9A) == 255 ? "off" : "on");
  touchpower::Regs touch;
  if (readTouchPower(&touch)) {
    char line[160];
    touchpower::describe(touch, line, sizeof(line));
    Serial.printf("[power] touch: %s (Pf)\n", line);
  } else {
    Serial.println("[power] touch: no answer at 0x38");
  }
}

}  // namespace board
