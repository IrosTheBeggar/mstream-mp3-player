#pragma once
#include <M5GFX.h>

#include <cstdint>

#include "InputEvent.h"
#include "JumpIndex.h"
#include "SheetLayout.h"
#include "ui/Icons.h"

// What comes over a page (spec §6, with the usability fixes). All opaque
// (reading the LCD back over the shared bus is too slow for blending);
// when one goes, the Ui has the page draw what it covered again.
//
//   Toast   feedback ("Added 14 tracks") with an optional Undo and, after an
//           add, "View" (the Queue at the added tracks), at the TOP of the
//           content area (y 36-71, over the page header): the usability
//           walk found the spec's bottom toast right above BtnC, where a
//           press meant as "next" hit Undo. 1.8 s, 4 s with Undo. A tap on
//           it (not on a button) dismisses it. "Plays next: <name>" whose
//           name doesn't fit beside the buttons takes two lines: "Plays
//           next" small over the name, and the buttons as icons (Undo's
//           arrow, View as the Queue tab's icon), so the name has 216 px
//           (on one line View and Undo left it ~50 px: "Plays next: Aer…";
//           two lines beside the word buttons, 168). It stays 36 px, out
//           of the list's hardware-scrolled band (from y 72), which would
//           carry a taller toast's lower rows away with the list. The
//           header's ‹ (x < 56), and its pill when the toast has no
//           buttons, still work under it (Ui::route).
//   Hud     over the tab bar (y 0-35) for 1.5 s after the last step: the
//           volume as 20 blocks and a value (A/C holds, the headphones'
//           volume keys), or where a B hold moved the output. The content
//           below keeps going (the dancer dances); a tap on the bar hides it
//           and still switches the tab.
//   Sheet   from the bottom: a title with a ✕ pill (its hit area x 250 to
//           the edge) and up to 4 rows of 40 px (SheetLayout.h: up to 3
//           in the list's band from y 72; 4, Now Playing's "...", from
//           y 40, over the header row like the dialog), each
//           with an optional dim detail on the right ("Go to artist   Daft
//           Punk"); optionally one row the primary (Bold, the accent: the
//           Library's Play) and one red (Forget); a tap outside closes it.
//   VolumeSheet  the active output's volume (spec §6.1, mockup 04): − and +,
//           a slider to tap or drag (5 % steps), the value; from y 124,
//           closes 3 s after the last change or on a tap above it. Nothing
//           changes on touch-down (a quick second tap on the control that
//           opened it lands on the slider), and a touch that starts in its
//           first 300 ms is ignored.
//   JumpGrid  the A-Z rail's tap (spec §6.3, mockup 08): the content area
//           as a 7 x 4 grid of '#', A-Z (letters with no rows faint and
//           inert), and for a letter with many rows a second level of its
//           two-letter starts ("Ka", "Ke"...).
//   Dialog  a modal card in the content area with up to 2 buttons; the tab
//           bar still works (a tab tap closes it, as "cancel"). Optionally
//           an icon before its title, a live status line under its body
//           (the headphones-lost dialog's "Reconnecting... try 2 of 3"),
//           and a red primary button for what can't be taken back.
//   Coach   the first-boot tips (once, remembered in NVS; About shows them
//           again): the three red touch buttons and what their clicks and
//           holds do, then "tap the tab you're on again to go back to its
//           start". Over the content area; the buttons and the tabs work.
//
// Each draws into a PSRAM sprite and pushes through ui/Gfx (over the lists'
// hardware scroll, and "over" the cover). Loop task only.
namespace ui {

// The sprite sheets and dialogs are drawn in (320 x 204, PSRAM: the
// content area, for the 4-row sheet; the others use its top 168 rows).
bool overlaysBegin();

class Toast {
public:
  static constexpr int kY = 36;
  static constexpr int kH = 36;  // the header row only: never in the list's scrolled band
  // hit()'s answers for the sleep timer's toast, and the idle warning's.
  static constexpr int kHitExtend = 4, kHitTurnOff = 5, kHitKeepOn = 6;
  // `holdMs`: how long it stays (0: 1.8 s, 4 s with Undo).
  void show(const char* text, bool undo, bool view, uint16_t accent, uint32_t nowMs, uint32_t holdMs = 0);
  // The sleep timer's fade (ENERGY.md section 3): "Sleep timer: fading"
  // with +10 min and Turn off (Turn off to the screen's edge). Up until
  // hidden (the fade ends) or replaced by another toast.
  void showSleep(uint16_t accent, uint32_t nowMs);
  bool sleep() const { return up_ && sleep_; }
  // The idle power-off's warning (IdlePolicy, its last 30 s): "Turning off
  // in 30 s" with Keep on (to the screen's edge). Up until hidden (it
  // counts down: setIdleSeconds()), or replaced by another toast.
  void showIdle(uint32_t seconds, uint16_t accent, uint32_t nowMs);
  bool idle() const { return up_ && idle_; }
  uint32_t idleSeconds() const { return idleSeconds_; }
  // The count changed: the new text, drawn.
  void setIdleSeconds(uint32_t seconds);
  void hide() { up_ = false; }
  bool up() const { return up_; }
  // The screen row under it (0 when it isn't up).
  int bottom() const { return up_ ? kY + kH : 0; }
  // A touch here goes to the page under it (and hides it): the header's
  // ‹ zone, and its pill's zone when the toast has no buttons there.
  bool passesThrough(const InputEvent& e) const;
  bool undo() const { return undo_; }
  bool expired(uint32_t nowMs) const { return up_ && static_cast<int32_t>(nowMs - untilMs_) >= 0; }
  // `pressed`: the button under a finger (hit()'s 2 or 3), 0 none.
  void draw(int pressed = 0);
  // A tap: 2 on Undo, 3 on View, kHitExtend / kHitTurnOff on the sleep
  // timer's buttons (SleepTimer::toastTap(): never a clamped reading),
  // kHitKeepOn on the idle warning's, 1 elsewhere on the toast, 0 not on
  // it. `slop`: the sleep and idle toasts' buttons also take a tap up to
  // uitext::kToastButtonSlop below it (no modal up).
  int hit(const InputEvent& e, bool slop = false) const;
  // The fade toast's buttons are under `e` (the toast's own 36 px): a
  // touch that goes to them even over a sheet (they are drawn on top).
  bool onSleepButton(const InputEvent& e) const {
    const int h = hit(e);
    return sleep_ && (h == kHitExtend || h == kHitTurnOff);
  }
  const char* text() const { return text_; }

private:
  char text_[128] = "";
  bool undo_ = false;
  bool view_ = false;
  bool up_ = false;
  bool compact_ = false;  // two lines, the buttons as icons
  bool sleep_ = false;    // the sleep timer's (showSleep())
  bool idle_ = false;     // the idle warning (showIdle())
  uint32_t idleSeconds_ = 0;
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
  // Up to 4 rows, each 40 px (the touch minimum): 4 (Now Playing's "...",
  // the sleep timer first) rise into the header row (SheetLayout.h).
  static constexpr int kMaxRows = sheet::kMaxRows;
  static constexpr int kRowH = sheet::kRowH;
  static constexpr int kTitleH = sheet::kTitleH;
  // `details`: nullptr, or a dim text per row (nullptr or "" for none).
  // `primary`: the row drawn as the main choice (-1 none); `danger`: the
  // row in red (-1 none).
  void open(const char* title, const char* const* rows, int n, uint16_t accent, const char* const* details = nullptr,
            int primary = -1, int danger = -1);
  void close() { up_ = false; }
  bool up() const { return up_; }
  int top() const { return y0_; }
  void draw();
  // A glass event: the row index on a Tap on a row, -2 on a Tap outside
  // or on ✕ (close), -1 otherwise. Presses highlight what's under the finger.
  int onEvent(const InputEvent& e);
  // Row `i`'s dim detail is now `text` (the "..." sheet's Sleep timer row
  // follows the timer): drawn again only if it changed.
  void setDetail(int i, const char* text);

private:
  static constexpr int kCross = -3;  // pressed_: the ✕ pill
  void render();
  void pushRow(int i);
  void pushTitle();
  char title_[64] = "";
  char rows_[kMaxRows][32] = {};
  char details_[kMaxRows][96] = {};
  int n_ = 0;
  int y0_ = 240;
  int pressed_ = -1;
  int primary_ = -1;
  int danger_ = -1;
  bool up_ = false;
  uint16_t accent_ = 0;
};

// The sleep timer's choices (ENERGY.md section 3; Now Playing's "..." >
// Sleep timer): the sheet panel (320 x 168 from y 72), the title with a
// close pill, then three rows of pills: "15", "30", "45", "60", "90 min";
// "End of track", "End of album", "End of queue"; and while a timer runs,
// "+10 min" and "Turn off" (red), else a line on what it does. The
// running choice is outlined in the accent. Texts and rooms: UiText.h.
class SleepSheet {
public:
  static constexpr int kY = 72;
  // onEvent()'s answers: 0-4 the minutes, then these.
  static constexpr int kTrack = 5, kAlbum = 6, kQueue = 7, kExtend = 8, kTurnOff = 9;
  // `current`: the pill outlined (0-7, -1 none); `running`: +10 min and
  // Turn off are offered; `extend`: +10 min acts (else drawn dim and inert:
  // End of album / queue before its last track, SleepTimer::canExtend());
  // `state`: the title ("Sleep timer: 23 min left").
  void open(int current, bool running, bool extend, const char* state, uint16_t accent);
  // Drawn again if any of it changed (the minutes pass, the timer ends).
  void refresh(int current, bool running, bool extend, const char* state);
  void close() { up_ = false; }
  bool up() const { return up_; }
  void draw();
  // A Tap: a choice (above), -2 on the close pill or above the sheet
  // (close), -1 otherwise. Presses highlight what's under the finger.
  int onEvent(const InputEvent& e);

private:
  static constexpr int kCross = -3;
  int pillAt(const InputEvent& e) const;
  void render();
  int current_ = -1;
  bool running_ = false;
  bool extend_ = false;
  char state_[40] = "";
  int pressed_ = -1;
  bool up_ = false;
  uint16_t accent_ = 0;
};

class VolumeSheet {
public:
  static constexpr int kY = 124;
  static constexpr uint32_t kCloseMs = 3000;
  static constexpr uint32_t kSettleMs = 300;  // touches that start sooner are ignored
  static constexpr int kStep = 5;
  void open(bool bluetooth, const char* name, int volume, uint16_t accent, uint32_t nowMs);
  void close() { up_ = false; }
  bool up() const { return up_; }
  bool expired(uint32_t nowMs) const { return up_ && static_cast<int32_t>(nowMs - untilMs_) >= 0; }
  // The volume changed elsewhere (A/C, the headphones' keys, or ours
  // landing): shown, unless a finger is setting it.
  void setVolume(int volume, bool bluetooth, uint32_t nowMs);
  void draw();
  struct Result {
    bool close = false;  // a tap above the sheet
    bool tapped = false; // a tap on a control (its tick, even at 0 % or 100 %)
    int target = -1;     // set the volume to this (0-100)
  };
  Result onEvent(const InputEvent& e, uint32_t nowMs);

private:
  enum Part : int8_t { None = -1, Minus, Slider, Plus };
  Part partAt(const InputEvent& e) const;
  static int valueAt(int x);
  void render();
  bool bluetooth_ = false;
  char name_[32] = "";
  int volume_ = 0;
  Part pressed_ = None;
  bool touching_ = false;
  bool ignoring_ = false;    // the touch began within kSettleMs of opening
  uint32_t openedMs_ = 0;
  uint32_t ownUntilMs_ = 0;  // our own change is on its way: the state lags
  bool up_ = false;
  uint16_t accent_ = 0;
  uint32_t untilMs_ = 0;
};

class JumpGrid {
public:
  static constexpr int kCellW = 44, kCellH = 41;
  static constexpr int kGridX = 6, kGridY = 74;
  static constexpr int kBack = jump::kGridCells - 1;  // the last cell: back to the letters
  // `labels`: kGridCells texts ("" for none); `enabled`: which do something;
  // `current`: the cell outlined (the list's letter now), -1 none.
  void open(const char* title, const char (*labels)[4], const bool* enabled, int current, uint16_t accent);
  void close() { up_ = false; }
  bool up() const { return up_; }
  void draw();
  // A Tap: the cell (0..kGridCells-1) if it's enabled, -2 on ✕ (close);
  // -1 otherwise.
  int onEvent(const InputEvent& e);

private:
  int cellAt(const InputEvent& e) const;
  void renderGrid();
  void drawHeader();
  char title_[40] = "";
  char labels_[jump::kGridCells][4] = {};
  bool enabled_[jump::kGridCells] = {};
  int current_ = -1;
  int pressed_ = -1;  // a cell, or -2 the ✕
  bool up_ = false;
  uint16_t accent_ = 0;
};

class Dialog {
public:
  static constexpr int kX = 14, kY = 58, kW = 292, kH = 164;
  // `buttons`: 1 or 2 labels, the last is the primary (red when `danger`).
  // `accent`: its outline and primary button.
  void open(const char* title, const char* body, const char* const* buttons, int n, uint16_t accent,
            bool danger = false);
  // An icon before the title (`slashed`: a red slash across it).
  void setIcon(const icons::Icon* icon, uint16_t colour, bool slashed);
  // A status line under the body (""): redrawn only when it changes.
  void setStatus(const char* text, uint16_t colour);
  // The room its title has (Bold 16), with an icon before it or not.
  static int titleRoom(bool icon);
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
  char status_[64] = "";
  uint16_t statusColour_ = 0;
  char buttons_[2][20] = {};
  int n_ = 0;
  int pressed_ = -1;
  bool up_ = false;
  bool danger_ = false;
  uint16_t accent_ = 0;
  const icons::Icon* icon_ = nullptr;
  uint16_t iconColour_ = 0;
  bool slashed_ = false;
};

class Coach {
public:
  static constexpr int kCards = 2;
  enum class Result : uint8_t { None, Next, Done };
  void open(int card, uint16_t accent);
  void close() { up_ = false; }
  bool up() const { return up_; }
  int card() const { return card_; }
  void draw();
  // A glass event (the content area): a Tap moves on (Next) or closes it
  // (Done, on the last card).
  Result onEvent(const InputEvent& e);

private:
  void render();
  void drawHeader();
  int card_ = 0;
  bool up_ = false;
  bool pressed_ = false;
  uint16_t accent_ = 0;
};

}  // namespace ui
