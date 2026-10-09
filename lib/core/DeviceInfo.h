// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// What the device is and has, as text: Output > About > Device info (ui/
// OutputPage), and the boot log's "[diag]" lines (main.cpp), which the boot
// screen used to show. The firmware fills the facts (MainUiHost::
// deviceInfo(), from app/Diagnostics, the card and the library); the rows'
// labels and values are made here, so the host tests measure them
// (test_device_info; their widths in test_ui_library).
namespace deviceinfo {

struct Facts {
  // Static strings (the firmware's literals and version texts); never null.
  const char* board = "";      // "M5Stack Core2"
  const char* pmic = "";       // "AXP192"
  const char* imu = "";        // "MPU6886"
  const char* chip = "";       // "ESP32-D0WDQ6-V3"
  const char* lastReset = "";  // "power-on"
  const char* storage = "";    // LocalStorage::name(): "SD", "flash", "none"
  const char* version = "";    // app/Version: "v0.8.0", "v0.8.0-dev+abc1234"
  const char* commit = "";     // "abc1234" ("" without git)
  const char* built = "";      // the commit's date, "2026-10-09"
  const char* elf = "";        // the ELF's SHA-256, 8 hex digits
  uint16_t chipRevision = 0;   // esp_chip_info()'s: major * 100 + minor (301: 3.1)
  uint16_t cpuMhz = 0;         // the clock now
  uint32_t flashBytes = 0;
  uint32_t psramBytes = 0, psramFree = 0;
  int battery = -1;            // %, -1 when the power chip doesn't say
  uint16_t batteryMv = 0;      // 0: not known
  bool charging = false;
  uint32_t tracks = 0;         // the library's (0 while it isn't ready)
  uint32_t ramFree = 0, ramMin = 0, ramBlock = 0;  // internal RAM: free, lowest since boot, largest block
  uint32_t uptimeS = 0;
};

// The rows, in the page's order. The boot screen's ten (Board to RAM free,
// with CPU after Chip), then what only the page needs.
enum class Item : uint8_t {
  Board,
  PowerChip,
  Imu,
  Chip,
  Cpu,
  Flash,
  Psram,
  LastReset,
  Battery,
  Library,
  RamFree,
  Uptime,
  Firmware,
  Build,
};
constexpr int kItems = static_cast<int>(Item::Build) + 1;
// The boot log's [diag] lines: the items before Uptime (the version and the
// build are in the banner above them).
constexpr int kLogged = static_cast<int>(Item::Uptime);

const char* label(Item i);
// The value's text ("ESP32-D0WDQ6-V3 rev 3.1", "4096K (3210K free)"):
// written into `out` (cut to `size`), which it returns.
const char* value(Item i, const Facts& f, char* out, size_t size);
// It changes while the page is open (redrawn at each refresh).
bool changes(Item i);
// "45 s", "3 min 05 s", "2 h 05 min", "4 d 2 h".
void uptimeText(uint32_t seconds, char* out, size_t size);

}  // namespace deviceinfo
