// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>
#include <M5GFX.h>

// Screenshots over the serial console. request() reads the rectangle back
// from the LCD at once (M5.Display.readRect, a few rows per SPI transaction,
// so the SD card is never locked out for long) into PSRAM; poll() then
// prints it one row per call, so the loop keeps running meanwhile:
//
//   [shot] format rgb565 big-endian, one base64 line per row[, note]
//   [shot] begin <x> <y> <w> <h>
//   <base64 of row 0>
//   ...
//   [shot] end
//
// Rows are the panel's RGB565, high byte first. While a list scrolls in
// hardware (ui/ListScroller) each row is read from the GRAM row the panel
// shows there, so the shot is what is on screen, and the format line says
// so ("hardware scroll active ... start address n"). If the LCD can't be read
// (or what it gives back doesn't match `check`, the sprite last pushed at
// checkX/checkY), the sprite's own pixels are dumped instead, and the format
// line says so. Loop task only: the one task that draws, so the capture
// can't tear.
class Screenshot {
public:
  // `check`: a sprite known to be on screen at (checkX, checkY), or nullptr.
  void request(int x, int y, int w, int h, M5Canvas* check, int checkX, int checkY);
  void poll();
  bool busy() const { return pixels_ != nullptr; }

private:
  void finish();

  uint16_t* pixels_ = nullptr;  // PSRAM, w * h, as read (big-endian RGB565)
  char* line_ = nullptr;        // PSRAM, one row of base64
  int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
  int row_ = -1;                // next row to print; -1: the header
};
