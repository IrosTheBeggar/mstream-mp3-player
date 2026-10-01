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

void applyBootPower() {
  const int addr = bmi270Address();
  const char* imu = "none found";
  if (addr >= 0) {
    setImuSuspended(addr, true);
    imu = "BMI270 suspended";
  } else if (M5.Imu.getType() == m5::imu_t::imu_mpu6886) {
    // The Core2 v1.0 and the Core2 for AWS: M5Unified's sleep (PWR_MGMT_1's
    // SLEEP bit: everything off but the registers, a few uA).
    imu = M5.Imu.sleep() ? "MPU6886 asleep" : "MPU6886: its sleep FAILED (left on)";
  } else if (M5.Imu.getType() != m5::imu_t::imu_none) {
    imu = "not a BMI270 or an MPU6886: left as it is";
  }
  const bool axp192 = M5.Power.getType() == m5::Power_Class::pmic_axp192;
  // The LED is the AXP192's PWM1: duty register 0x9A, 255 = off.
  Serial.printf("[power] boot: IMU %s, 5 V boost (EXTEN) %s, green LED %s\n", imu,
                M5.Power.getExtOutput() ? "ON" : "off",
                !axp192 ? "?" : M5.Power.Axp192.readRegister8(0x9A) == 255 ? "off" : "on");
}

}  // namespace board
