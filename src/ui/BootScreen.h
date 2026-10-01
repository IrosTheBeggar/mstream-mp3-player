// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <vector>

// The boot diagnostics: a title bar and label/value rows (board, chips,
// memory, battery, the library), on screen for the first seconds until the
// UI (ui/Ui) takes the display. The same rows go to the serial log.
class BootScreen {
public:
  struct Row {
    String label;
    String value;
  };

  // The title bar: "mStream Player <version>" (app/Version).
  void begin(const char* version);
  // Draws (or redraws) the rows.
  void show(const std::vector<Row>& rows);
  // A line at the bottom (the UI's Small font once it is loaded: after
  // Ui::begin()): the rescue hold (uitext::kBootTouchHint).
  void hint(const char* text);
};
