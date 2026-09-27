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

  void begin();
  // Draws (or redraws) the rows.
  void show(const std::vector<Row>& rows);
};
