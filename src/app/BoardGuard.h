// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The firmware is for the M5Stack Core2 only (its pins, power chip, panel
// and speaker are assumed everywhere). Flashed onto another ESP32 board
// that M5Unified recognises (an M5Stack Basic or Fire, a Tough, a
// StickC...), it says so on that board's display (if it has one) and on
// the serial port, and stops there: before the SD card, the audio and
// Bluetooth start, without rebooting over and over. With an AXP192 or
// AXP2101 (a Tough, or a Core2 M5GFX misread) it powers off after a minute
// on battery. The screen's lines are UiText's (measured in test_ui_library).
namespace board {

// setup(), right after M5.begin(): returns on a Core2 (any version: the
// AXP192 or the AXP2101); on anything else never returns.
void requireCore2();

}  // namespace board
