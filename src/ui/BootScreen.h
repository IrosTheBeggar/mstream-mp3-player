// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once

// The boot screen (lib/core/BootLayout): the mStream logo, the player's
// version under it, and the rescue hold's line at the bottom; on screen
// from the start of setup() until the UI (ui/Ui) takes the display. What
// it used to list (the board, the chips, memory, the battery, the library)
// is Output > About > Device info now, and the boot log's [diag] lines.
class BootScreen {
public:
  // The logo and the version (app/Version), on a cleared screen. Loads the
  // UI's fonts first (ui/Fonts, kept for the UI), so the version is in
  // DejaVu as everything after it.
  void begin(const char* version);
  // A line at the bottom (Small): the rescue hold (uitext::kBootTouchHint).
  void hint(const char* text);
};
