#pragma once
#include <Arduino.h>

#include <vector>

// Rendering on the Core2's 320x240 LCD (M5GFX). The app owns all state and
// tells the view what to draw. Text is drawn over its own background with
// padding, so refreshing a value in place doesn't flicker.
class DisplayView {
public:
  struct Row {
    String label;
    String value;
  };

  void begin();

  // Bring-up screen: a title bar plus label/value rows. Call again to refresh.
  void showDiagnostics(const std::vector<Row>& rows);

private:
  void drawHeader(const char* title);

  bool diagnosticsShown_ = false;
};
