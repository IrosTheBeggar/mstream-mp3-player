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

  // Everything on the now-playing screen, already formatted.
  struct NowPlaying {
    String battery;   // "87%"
    String position;  // "Track 2 of 8"
    String title;
    String subtitle;  // artist, or what's being decoded
    String status;    // "Playing 0:12", "Paused 0:12", "Stopped"
    String output;    // "Bluetooth: WH-1000XM4", "Speaker"
    String volume;    // "Volume 30%"
    String note;      // why the last track failed; highlighted
    String stats;     // one line of numbers for testing
  };

  void begin();

  // Bring-up screen: a title bar plus label/value rows. Call again to refresh.
  void showDiagnostics(const std::vector<Row>& rows);
  // Player screen, with labels for the three touch buttons along the bottom.
  void showNowPlaying(const NowPlaying& np);
  // Someone else drew on the screen (the dance screen): the next show*()
  // redraws all of it.
  void forget() { screen_ = Screen::None; }

private:
  enum class Screen { None, Diagnostics, NowPlaying };
  void enter(Screen screen, const char* title);
  void drawHeader(const char* title);

  Screen screen_ = Screen::None;
};
