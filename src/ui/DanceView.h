#pragma once
#include <Arduino.h>
#include <M5GFX.h>

#include "CrabPose.h"
#include "DancePose.h"
#include "DanceSkin.h"

// The dance screen: a title bar, the dancing character in its own box, and
// one line of numbers. The character is drawn into a sprite in PSRAM
// (setPsram() before createSprite(): the default would take internal RAM)
// and pushed once per frame; M5GFX converts it to the panel's RGB565 through
// its small flip buffers. Each push takes the SPI bus (shared with the SD
// card) for itself, never a whole frame.
//
// The sprite's format follows the skin:
//   - stick figure: 8-bit RGB332 (18 KB), anti-aliased wide lines;
//   - crab: RGB565 (36 KB), the crab's own 16 colours exactly (RGB332 would
//     turn its reds pink), pixel art blitted at an integer 3x with no
//     smoothing. RGB565 is the panel's format, so the push is a plain copy;
//     a 4-bit palette sprite (9 KB) pushed ~4 ms slower a frame while an
//     MP3 played, converting every pixel through its palette. The 16
//     colours are recomputed as the dance weight fades between the idle and
//     the dance palettes.
// A skin switch deletes the sprite and creates it in the other format.
//
// Only what changed is redrawn and pushed: the rectangle around the
// character now and in the last frame (for the crab, its contact shadow
// too), and the beat dot's corner when it changes. That rectangle holds
// every pixel of the character, so a palette change needs no bigger push. A
// push from a PSRAM sprite is copied by the CPU (M5GFX doesn't use DMA for
// it), so the whole box would cost ~8.5 ms of the loop's core a frame; the
// character's rectangle is about half of it.
// Loop task only: the one task that draws to M5.Display.
class DanceView {
public:
  static constexpr int kBoxX = 100;
  static constexpr int kBoxY = 32;
  static constexpr int kBoxW = 120;
  static constexpr int kBoxH = 150;

  // Creates the sprite for `skin`. False: no PSRAM for it (the dance screen
  // is then off).
  bool begin(dance::Skin skin = dance::kDefaultSkin);
  bool ready() const { return ready_; }
  // Switches the sprite to another skin's format; the next frame pushes the
  // whole box. False: no PSRAM for it (the dance screen is then off).
  bool setSkin(dance::Skin skin);
  dance::Skin skin() const { return skin_; }
  // A screen point inside the character's box.
  static bool inBox(int x, int y) { return x >= kBoxX && x < kBoxX + kBoxW && y >= kBoxY && y < kBoxY + kBoxH; }

  // Clears the screen for the dance: title bar, button labels, empty box.
  void enter();
  // The title bar's text; redrawn only when it changes.
  void setTitle(const String& title);
  // The line of numbers under the figure; redrawn only when it changes.
  void setStatus(const String& status);
  // Draws a pose into the sprite. `flash`: the beat dot; `dancing`: the
  // figure follows a beat (drawn brighter than the idle sway). Stick skin only.
  void drawFigure(const dance::Pose& pose, bool flash, bool dancing);
  // The crab. `weight`: 0 idle .. 1 dancing, for the palette. Crab skin only.
  void drawCrab(const crab::Pose& pose, float weight, bool flash);
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
  bool create();
  // Clears the dirty rectangle (the new one united with the last) and clips to it.
  void beginFrame(const Rect& now, int bg);
  // The beat dot, in its corner: drawn only when it changes (or the
  // character's rectangle reached into its corner and cleared it).
  void drawDot(bool flash, int bg, int colour, bool smooth);
  void pushRect(const Rect& r);

  M5Canvas sprite_;
  dance::Skin skin_ = dance::kDefaultSkin;
  bool ready_ = false;
  int paletteStep_ = -1;  // crab: the palette in ink_, or -1 for none yet
  uint16_t ink_[crab::kPaletteSize] = {};  // crab: RGB565 of each palette index
  bool full_ = true;   // the next push is the whole box (after enter())
  Rect last_;          // the figure's rectangle in the last frame
  Rect dirty_;         // to push: the figure's, now and last frame
  bool flash_ = false; // the beat dot as last drawn
  bool flashDirty_ = false;
  String title_;
  String status_;
};
