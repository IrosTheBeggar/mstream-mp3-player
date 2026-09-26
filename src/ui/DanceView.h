#pragma once
#include <Arduino.h>
#include <M5GFX.h>

#include "DancePose.h"

// The dance screen: a title bar, the stick figure in its own box, and one
// line of numbers. The figure is drawn into an 8-bit sprite in PSRAM
// (setPsram() before createSprite(): the default would take ~18 KB of
// internal RAM) and pushed once per frame; M5GFX converts it to the panel's
// RGB565 through its small flip buffers. Each push takes the SPI bus (shared
// with the SD card) for itself, never a whole frame.
//
// Only what changed is redrawn and pushed: the rectangle around the figure
// now and in the last frame (and the beat dot's corner when it changes). A
// push from a PSRAM sprite is copied by the CPU (M5GFX doesn't use DMA for
// it), so the whole box would cost ~8.5 ms of the loop's core a frame; the
// figure's rectangle is about half of it.
// Loop task only: the one task that draws to M5.Display.
class DanceView {
public:
  static constexpr int kBoxX = 100;
  static constexpr int kBoxY = 32;
  static constexpr int kBoxW = 120;
  static constexpr int kBoxH = 150;

  // Creates the sprite. False: no PSRAM for it (the dance screen is then off).
  bool begin();
  bool ready() const { return ready_; }

  // Clears the screen for the dance: title bar, button labels, empty box.
  void enter();
  // The title bar's text; redrawn only when it changes.
  void setTitle(const String& title);
  // The line of numbers under the figure; redrawn only when it changes.
  void setStatus(const String& status);
  // Draws a pose into the sprite. `flash`: the beat dot; `dancing`: the
  // figure follows a beat (drawn brighter than the idle sway).
  void drawFigure(const dance::Pose& pose, bool flash, bool dancing);
  // Pushes the sprite to its box on the LCD.
  void push();

  // For screenshots: the sprite as last drawn.
  M5Canvas& sprite() { return sprite_; }

private:
  struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // [x0, x1) x [y0, y1), in the box
    bool empty() const { return x1 <= x0 || y1 <= y0; }
  };
  static Rect bounds(const dance::Pose& pose);
  static Rect unite(const Rect& a, const Rect& b);
  void pushRect(const Rect& r);

  M5Canvas sprite_;
  bool ready_ = false;
  bool full_ = true;   // the next push is the whole box (after enter())
  Rect last_;          // the figure's rectangle in the last frame
  Rect dirty_;         // to push: the figure's, now and last frame
  bool flash_ = false; // the beat dot as last drawn
  bool flashDirty_ = false;
  String title_;
  String status_;
};
