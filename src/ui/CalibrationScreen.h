#pragma once
#include <cstdint>

#include "InputEvent.h"
#include "TouchCalibration.h"
#include "ui/Input.h"

// The touch calibration screen: tap 5-9 crosshairs, and a new correction
// table is fitted to where the panel read them (TouchCalibration::fitAxis,
// the same fit that made the default table from the input lab's logs).
// Reachable from the console now (a, a5-a9), and later from the Output /
// Settings screen: open() and the events are all it needs.
//
// Pages:
//   1. crosshairs, one at a time, spread over the screen (distinct x and y
//      each, so both axes get as many points as there are crosshairs). A tap
//      (or a press held longer) on the cross is a sample, taken from the raw
//      reading where the finger landed; one further than 70 px (x) or 50 px
//      (y) from the cross is asked again. "Cancel" is at the top right.
//   2. the fit: the error before and after, per axis; Save or Discard (or,
//      if the taps don't agree, Try again). These big buttons are hit
//      tested with the table in use, not the new one.
//   3. after Save, a check: each tap shows where the Core2 now reads it (a
//      coral dot) and the panel's raw reading (grey); Done closes.
//
// While it's up it owns the screen (main.cpp stops the now-playing and
// dance screens) and the glass; BtnA/B/C keep doing what they always do.
// Loop task only; it draws in small pieces, each under its own LcdLock.
class CalibrationScreen {
public:
  static constexpr int kMinTargets = 5;
  static constexpr int kMaxTargets = 9;

  explicit CalibrationScreen(Input& input) : input_(input) {}

  // Opens on the crosshairs (`targets` 5-9), or on the check page.
  void open(int targets = kMaxTargets);
  void openCheck();
  void close();
  bool active() const { return page_ != Page::Closed; }
  // Called once when it closes (main.cpp redraws its own screen).
  void onClosed(void (*fn)()) { closed_ = fn; }

  // Glass events while active (the caller keeps the buttons).
  void onEvent(const InputEvent& e);

private:
  enum class Page : uint8_t { Closed, Targets, Result, Check };
  struct Target {
    int16_t x, y;
  };
  static const Target kTargets[kMaxTargets];

  void clear();
  void drawHeader(const char* title, const char* right);
  void drawTarget();
  void drawHint(const char* text, uint16_t colour);
  void finish();
  void drawResult();
  void drawCheckPage();
  void drawButton(int x, int y, int w, int h, const char* label, bool primary);

  Input& input_;
  void (*closed_)() = nullptr;
  Page page_ = Page::Closed;
  int count_ = kMaxTargets;
  int next_ = 0;
  TouchCalibration::Sample sx_[kMaxTargets] = {};
  TouchCalibration::Sample sy_[kMaxTargets] = {};
  TouchCalibration fitted_;
  TouchCalibration::FitReport repX_, repY_;
  bool fitOk_ = false;
  int checkTaps_ = 0;
};
