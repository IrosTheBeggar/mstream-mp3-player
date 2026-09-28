#pragma once
#include <cstdint>

// The Core2's small consumers that nothing uses (docs/ENERGY.md item 9),
// each below the probe's noise, together 2.5-3.9 USB mA (measured):
//
// - the IMU (BMI270): suspended at boot (~3.5 uA); nothing reads it.
// - the 5 V boost (the AXP192's EXTEN, the M-Bus/Grove 5 V): off from
//   M5.begin() (main.cpp sets cfg.output_power = false). The speaker amp
//   isn't fed from it on USB (measured); still to check by ear on battery.
// - the green LED: off (M5Unified's default, led_brightness 0).
//
// The console's Pi, Pe and Pg switch them back for measurements
// (app/PowerLab). Loop task.
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

}  // namespace board
