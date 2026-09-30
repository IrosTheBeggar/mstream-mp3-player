#pragma once
#include <cstdint>

#include "InputEvent.h"
#include "TouchCalibration.h"
#include "TouchCheck.h"
#include "ui/Input.h"

// The touch check and calibration: the whole screen, in the UI's look
// (ui::Fonts, ui::col, the Output accent: it opens from Output). The rules
// are TouchCheck's (host-tested); the texts and their rooms UiText's.
// Opened three ways:
//
//   - FirstBoot: no table saved and the check never answered (TouchCheck's
//     due(), NVS "input"/"cal_ask"): before the UI starts, so the tips
//     follow it. Three dots, one at a time, where the lab's panel read 0,
//     +20 and +40 px off; each tap leaves a grey mark where it landed (one
//     more than 90 px off is asked again, once). Then the verdict: "Taps
//     land about 40 px to the right of your finger. Calibrate now?" with
//     [Calibrate] [Not now], or "Touch is accurate. Tap anywhere to go on".
//     Any way out answers it (Skip, Not now, the verdict, walking away).
//   - Crosses (Output > Touch calibration > Calibrate; the rescue hold on
//     the boot screen; console a, a5-a9): 5-9 crosses, one at a time,
//     spread over the screen (distinct x and y each, so both axes get as
//     many points as there are crosses; clear of the header's Cancel and
//     the A hint). A tap (or a press held longer) on the cross is a sample,
//     taken from the raw reading; a miss buzzes and asks again, and after
//     two misses a third tap that agrees with the second is taken
//     (TouchCheck::judgeCross). Each sample ticks and flashes the cross
//     green. Then the result: "Now: up to 42 px off, average 21" (the table
//     in use) and "Calibrated: up to 12 px off, average 6" (the new table,
//     on taps it wasn't fitted to: TouchCheck's measureUnseen()), with
//     [Save] [Try again] [Discard]; Save first only when the new table is
//     clearly better, else Discard (the touch already accurate, or no
//     better: TouchCheck's outcome()); Try again first when the taps didn't
//     agree. After Save, the test taps page, where A undoes the Save (the
//     table before is kept in RAM until the page closes).
//   - Check (Output > Touch calibration > Test taps; console ac): each tap
//     draws a coral ring where the Core2 reads it, and with a table saved a
//     grey dot where it would read without one. [Done].
//
// Every button is a full-width 40 px row, hit tested by y alone (the panel
// reads y true: a stack survives any x error). The glass's way out is a
// pill at the top LEFT (a panel that reads right carries a tap away from
// it, never onto it); the strip's A is the way out on every page (main.cpp
// routes its click here: leave()), with "A: Cancel" (the page's verb) over
// the A dot; a tap on that label on the glass does the same (on the
// crosses, TouchCheck's judgeCross() says when). Touches that went down
// before a page (or cross) was drawn, or within 300 ms of it, are ignored
// (the panel loses a finger and finds it again within ~145 ms: a bounce
// must not be a sample of the next cross); after a miss, only 150 ms (the
// same target: a quick retap is an attempt, a bounce isn't).
// With no touch for 60 s it closes as its way out (nothing saved): the
// screen stays lit while it's up and the idle power-off waits for it.
//
// While it's up it owns the screen (main.cpp suspends the UI and stops the
// dancer) and the glass; the touch buttons keep doing what they always do,
// but A's click. Loop task only; it draws in small pieces (the UI's strip
// sprite, pushed under its own locks; the marks straight to the LCD).
class CalibrationScreen {
public:
  static constexpr int kMinTargets = 5;
  static constexpr int kMaxTargets = touchcheck::kCrosses;
  static constexpr uint32_t kIdleCloseMs = 60000;
  static constexpr uint32_t kSettleMs = 300;
  // After a miss, only this (a bounce of the missed tap).
  static constexpr uint32_t kBounceMs = 150;
  enum class Start : uint8_t { Crosses, Check, FirstBoot };

  explicit CalibrationScreen(Input& input) : input_(input) {}

  // `targets` (5-9): the crosses' count.
  void open(Start how, int targets = kMaxTargets);
  void close();
  bool active() const { return page_ != Page::Closed; }
  // Called once when it closes (main.cpp redraws its own screen).
  void onClosed(void (*fn)()) { closed_ = fn; }

  // Glass events while active (the caller keeps the buttons, but A's click).
  void onEvent(const InputEvent& e);
  // The strip's A clicked: the page's way out (Skip, Not now, Cancel,
  // Discard, Done; Undo right after a Save).
  void leave();
  // Every loop pass: the accepted cross's flash, the 60 s timeout.
  void loop(uint32_t nowMs);

private:
  enum class Page : uint8_t { Closed, Probe, ProbeResult, Targets, Result, Check };
  enum class Act : uint8_t { None, Calibrate, NotNow, Save, TryAgain, Discard, Done };
  struct Row {
    const char* label;
    const char* detail;
    Act act;
    bool primary;
  };
  using Target = touchcheck::Dot;
  static constexpr int kMaxRows = 3;

  void shown();
  void rearm();
  void clear();
  void drawHeader(const char* title, const char* pill, const char* right, uint16_t rightColour);
  // A line centred across the screen at `y` (Body, else Small if Body
  // doesn't fit; or `font` as given).
  void drawLine(int y, const char* text, uint16_t colour, int font = -1);
  void drawHintBand(int y, int lines);
  void drawAHint(const char* verb);
  void setRows(const Row* rows, int n);
  void drawRow(int i, bool pressed);
  int rowAt(int y) const;
  void act(Act a);

  void startProbe();
  void drawProbeDot();
  void onProbeTap(const InputEvent& e);
  void finishProbe();
  void drawProbeResult();
  void drawProbeMarks();

  void startTargets();
  void drawTarget();
  void drawCross(const Target& t, uint16_t colour);
  void eraseCross(const Target& t);
  void targetHint(const char* text, uint16_t colour);
  void onTargetTap(const InputEvent& e);
  void finish();
  void drawResult();

  void openCheck();
  void drawCheckPage();
  void save();
  void undo();

  Input& input_;
  void (*closed_)() = nullptr;
  Page page_ = Page::Closed;
  bool firstBoot_ = false;
  // Touches: the page's drawing time (the settle guard), whether the touch
  // down now counts, the row under it, the last input (the timeout).
  uint32_t shownMs_ = 0;
  bool touchOk_ = false;
  int pressedRow_ = -1;
  uint32_t lastInputMs_ = 0;
  Row rows_[kMaxRows] = {};
  int nRows_ = 0;
  // The first-boot check.
  int dot_ = 0;
  bool dotRetried_ = false;
  touchcheck::Tap dotTaps_[touchcheck::kDots] = {};
  touchcheck::Verdict verdict_;
  // The crosses.
  int count_ = kMaxTargets;
  int next_ = 0;
  touchcheck::CrossTries tries_;
  bool flashing_ = false;
  uint32_t flashUntilMs_ = 0;
  TouchCalibration::Sample sx_[kMaxTargets] = {};
  TouchCalibration::Sample sy_[kMaxTargets] = {};
  TouchCalibration fitted_;
  TouchCalibration::FitReport repX_, repY_;
  touchcheck::Error nowErr_, newErr_;
  touchcheck::Outcome outcome_ = touchcheck::Outcome::Disagree;
  // The check page: its taps so far; a Save to undo (the table before it).
  int checkTaps_ = 0;
  bool justSaved_ = false;
  bool undone_ = false;
  TouchCalibration before_;
  bool beforeCustom_ = false;
};
