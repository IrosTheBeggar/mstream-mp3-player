// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The Core2's touch controller (FocalTech FT6336U, I2C 0x38) and its power
// mode (docs/ENERGY.md section 5, P1): its registers, the boot line that
// reports them, and the console's Pf argument. app/BoardPower reads and
// writes the chip.
//
// The modes (reg 0xA5, ID_G_PMODE):
// - Active (0): it scans at its active rate (reg 0x88).
// - Monitor (1): its slow scan (reg 0x89); a touch takes it to Active (and
//   asserts INT) by itself.
// - Hibernate (3): only a reset brings it out, and on the Core2 its reset
//   line is the LCD's (AXP192 GPIO4): the panel would need a reset and a
//   re-init. Never written (Pf refuses it).
//
// With reg 0x86 (ID_G_CTRL) = 1 the chip goes from Active to Monitor by
// itself after reg 0x87 (ID_G_TIMEENTERMONITOR) seconds untouched; with 0
// it stays Active. M5GFX writes none of these (only 0xA4, the INT mode).
// Measured on this Core2 (ENERGY.md section 5, "Audit 2: measured"): 0x86 =
// 1, 0x87 = 30, 0x88 = 10, 0x89 = 40, and 0xA5 reads Monitor 30 s after the
// last touch, so nothing here sets the mode outside the console's Pf.
// 0x88 and 0x89 are shown raw: FocalTech's documents call them a period or
// a rate, without a unit.
//
// Portable (host-tested: test_touch_power).
namespace touchpower {

inline constexpr uint8_t kAddress = 0x38;
inline constexpr uint8_t kRegCtrl = 0x86;  // 0x86..0x89 read in one burst
inline constexpr uint8_t kRegMode = 0xA5;
inline constexpr uint8_t kActive = 0;
inline constexpr uint8_t kMonitor = 1;
inline constexpr uint8_t kHibernate = 3;

struct Regs {
  uint8_t ctrl = 0;           // 0x86: 1 = to Monitor by itself
  uint8_t monitorAfterS = 0;  // 0x87: after that many seconds untouched
  uint8_t periodActive = 0;   // 0x88: the scan setting in Active (raw)
  uint8_t periodMonitor = 0;  // 0x89: in Monitor (raw)
  uint8_t mode = 0;           // 0xA5
};

// From the four bytes read at kRegCtrl (0x86-0x89) and the one at kRegMode.
Regs fromBytes(const uint8_t ctrlBlock[4], uint8_t mode);

// "Active", "Monitor", "Hibernate"; anything else (or -1: no answer) "?".
const char* modeName(int mode);

// The chip drops to Monitor by itself (0x86 = 1, and a time to wait).
bool autoMonitors(const Regs& r);

// The boot line's body (after "[power] touch: "):
// "ctrl=1 monitor_after=30s period_active=10 period_monitor=40 mode=Active",
// then what it means: "(to Monitor by itself after 30 s untouched)" or
// "(no auto-switch: Active unless set)". A mode that isn't one of the three
// shows its raw value ("mode=0x02").
void describe(const Regs& r, char* buf, size_t size);

// The console's Pf (the text after "Pf"):
//   ""       Report: the registers, read now
//   "0"      Active now
//   "1"      Monitor now (a touch takes it back to Active)
// Anything else, 3 (Hibernate) included, is Refused.
enum class Pf : uint8_t { Report, Active, Monitor, Refused };
Pf parsePf(const char* arg);

}  // namespace touchpower
