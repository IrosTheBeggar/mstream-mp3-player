#pragma once
#include <M5GFX.h>

#include <cstdint>

#include "InputEvent.h"

// What comes over a page (spec §6, with the usability fixes). All opaque
// (reading the LCD back over the shared bus is too slow for blending);
// when one goes, the Ui has the page draw what it covered again.
//
//   Toast   one line of feedback ("Added 14 tracks") with an optional Undo
//           and, after an add, "View" (the Queue at the added tracks), at
//           the TOP of the content area (y 36-71, over the page header):
//           the usability walk found the spec's bottom toast right above
//           BtnC, where a press meant as "next" hit Undo. 1.8 s, 4 s with
//           Undo. A tap on it (not on a button) dismisses it.
//   Hud     over the tab bar (y 0-35) for 1.5 s after the last step: the
//           volume as 20 blocks and a value (A/C holds, the headphones'
//           volume keys), or where a B hold moved the output. The content
//           below keeps going (the dancer dances); a tap on the bar hides it
//           and still switches the tab.
//   Sheet   from the bottom (never above y 72): a title and up to 3 rows of
//           40 px; a tap outside closes it.
//   Dialog  a modal card in the content area with up to 2 buttons; the tab
//           bar still works (a tab tap closes it, as "cancel").
//
// Each draws into a PSRAM sprite and pushes through ui/Gfx (over the lists'
// hardware scroll, and "over" the cover). Loop task only.
namespace ui {

// The sprite sheets and dialogs are drawn in (320 x 168, PSRAM).
bool overlaysBegin();

class Toast {
public:
  static constexpr int kY = 36;
  static constexpr int kH = 36;
  void show(const char* text, bool undo, bool view, uint16_t accent, uint32_t nowMs);
  void hide() { up_ = false; }
  bool up() const { return up_; }
  bool undo() const { return undo_; }
  bool expired(uint32_t nowMs) const { return up_ && static_cast<int32_t>(nowMs - untilMs_) >= 0; }
  // `pressed`: the button under a finger (hit()'s 2 or 3), 0 none.
  void draw(int pressed = 0);
  // A tap: 2 on Undo, 3 on View, 1 elsewhere on the toast, 0 not on it.
  int hit(const InputEvent& e) const;
  const char* text() const { return text_; }

private:
  char text_[64] = "";
  bool undo_ = false;
  bool view_ = false;
  bool up_ = false;
  uint16_t accent_ = 0;
  uint32_t untilMs_ = 0;
};

class Hud {
public:
  static constexpr uint32_t kShowMs = 1500;
  enum class Kind : uint8_t { Volume, Output };
  void showVolume(int percent, bool bluetooth, uint32_t nowMs);
  // `line`: what happened ("Speaker, paused: B plays").
  void showOutput(bool bluetooth, const char* line, uint32_t nowMs);
  void hide() { up_ = false; }
  bool up() const { return up_; }
  bool expired(uint32_t nowMs) const { return up_ && static_cast<int32_t>(nowMs - untilMs_) >= 0; }
  bool showsVolume() const { return kind_ == Kind::Volume; }
  int volume() const { return volume_; }
  bool bluetooth() const { return bluetooth_; }
  uint32_t until() const { return untilMs_; }
  void draw();

private:
  Kind kind_ = Kind::Volume;
  int volume_ = 0;
  bool bluetooth_ = false;
  char line_[48] = "";
  bool up_ = false;
  uint32_t untilMs_ = 0;
};

class Sheet {
public:
  static constexpr int kMaxRows = 3;
  static constexpr int kRowH = 40;
  static constexpr int kTitleH = 36;
  void open(const char* title, const char* const* rows, int n, uint16_t accent);
  void close() { up_ = false; }
  bool up() const { return up_; }
  int top() const { return y0_; }
  void draw();
  // A glass event: the row index on a Tap on a row, -2 on a Tap outside
  // (close), -1 otherwise. Presses highlight the row under the finger.
  int onEvent(const InputEvent& e);

private:
  void render();
  void pushRow(int i);
  char title_[48] = "";
  char rows_[kMaxRows][32] = {};
  int n_ = 0;
  int y0_ = 240;
  int pressed_ = -1;
  bool up_ = false;
  uint16_t accent_ = 0;
};

class Dialog {
public:
  static constexpr int kX = 14, kY = 58, kW = 292, kH = 164;
  // `buttons`: 1 or 2 labels, the last is the primary.
  void open(const char* title, const char* body, const char* const* buttons, int n, uint16_t accent);
  void close() { up_ = false; }
  bool up() const { return up_; }
  void draw();
  // The button index on a Tap on a button, -1 otherwise (it is modal:
  // taps elsewhere do nothing).
  int onEvent(const InputEvent& e);

private:
  void render();
  int buttonAt(int x, int y) const;
  char title_[48] = "";
  char body_[128] = "";
  char buttons_[2][20] = {};
  int n_ = 0;
  int pressed_ = -1;
  bool up_ = false;
  uint16_t accent_ = 0;
};

}  // namespace ui
