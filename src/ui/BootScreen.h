// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The boot screen (lib/core/BootLayout): the mStream logo and the player's
// version under it, on screen from the start of setup() until the UI
// (ui/Ui) takes the display; and the rescue hold's line at the bottom, from
// the end of setup() (once the UI is made, after the library). What
// it used to list (the board, the chips, memory, the battery, the library)
// is Output > About > Device info now, and the boot log's [diag] lines.
class BootScreen {
public:
  // The logo and the version (app/Version), on a cleared screen. Loads the
  // UI's fonts first (ui/Fonts, kept for the UI), so the version is in
  // DejaVu as everything after it.
  void begin(const char* version);
  // A line at the bottom (Small): the rescue hold (boot.touch_hint),
  // drawn by setup() once the UI is made.
  void hint(const char* text);
  // The format at the boot (storage/CardFormatter; docs/HOST-STATUS.md
  // "@format"), under the version: "Formatting the card" (Bold), a bar at
  // `percent`, and "Leave the card in and the player on" in the rescue
  // line's place; after the first call only the bar is drawn again.
  // formatDone(): that area cleared again.
  void formatting(uint8_t percent);
  void formatDone();

private:
  bool formatShown_ = false;
  int formatBarW_ = 0;  // the bar's filled width drawn
};
