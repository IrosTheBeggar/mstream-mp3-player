#pragma once
#include <cstdint>

#include "ButtonGesture.h"
#include "InputEvent.h"
#include "TouchCalibration.h"
#include "TouchRecognizer.h"
#include "app/Haptics.h"

// The one input layer (the tab bar spec's dispatcher, §10.1). It alone reads
// the touch panel and M5Unified's BtnA/B/C; everything downstream gets
// InputEvents from poll():
//
//   - the glass: every touch point is corrected (TouchCalibration: the
//     user's panel reads x up to ~45 px too far right) before anything hit
//     tests it, then TouchRecognizer makes Down, Tap, LongPress, Release,
//     DragStart/Move/End and Fling (capped at 2,000 px/s) of it;
//   - the buttons: ButtonGesture makes Click, Hold (500 ms), Repeat (A and
//     C, every 200 ms) and HoldEnd; ButtonPolicy (main.cpp) decides what
//     they do.
//
// The feedback, as the user chose it: a tap tick (33 ms, strong) and, the
// moment a hold is recognised, a double tick. The buttons' it plays itself
// (every click and hold of theirs does something). The glass's is played by
// whatever acts on the touch (tapTick(), holdTick()), so a tap on nothing,
// or a long press on a control that has no hold, doesn't confirm anything.
// Nothing on scroll frames; the A-Z rail asks for a tick per new letter
// (railTick()). Both can be turned off (saved).
//
// Saved in NVS (namespace "input"): the touch calibration ("cal", the
// TouchCalibration bytes; the default table when absent), "haptics" and
// "railtick". Loop task only.
class Input {
public:
  explicit Input(Haptics& haptics) : haptics_(haptics) {}

  // Loads the settings and the calibration.
  void begin();
  // Every loop pass, after M5.update(): samples the glass and the three
  // buttons, queues their events and plays the feedback.
  void update(uint32_t nowMs);
  // The next event, oldest first. False when there is none.
  bool poll(InputEvent& e);

  // Something else reads the panel and the buttons itself (the input lab):
  // no events, no feedback. The recognisers keep following the finger and
  // the buttons, so a press that began before doesn't fire afterwards.
  void setSuspended(bool on) { suspended_ = on; }
  bool suspended() const { return suspended_; }
  // A screen changed under the finger: the touch ends with a Cancel event,
  // and nothing more comes of it until it lifts.
  void cancelTouch(uint32_t nowMs);
  // The touch in progress has nothing to hold (the A-Z rail): no LongPress
  // for it (TouchRecognizer::noHold()).
  void noHold() { glass_.noHold(); }

  // ---- settings (saved) ----
  bool hapticsOn() const { return hapticsOn_; }
  void setHapticsOn(bool on);
  bool railTicksOn() const { return railTicks_; }
  void setRailTicksOn(bool on);
  // The A-Z rail reached a new letter: a tap tick if both are on.
  void railTick();
  // A glass tap did something: the tap tick.
  void tapTick();
  // A long press did something (it has a hold action): the double tick.
  void holdTick();
  // Whether holdTick() was called since the last call (the Ui: a long press
  // nobody used ends as a tap).
  bool takeHoldUsed();

  // ---- touch calibration ----
  const TouchCalibration& calibration() const { return cal_; }
  bool calibrated() const { return custom_; }  // a table of the user's, not the default
  // Applies and saves a table; false (nothing changed) if it isn't valid or
  // can't be saved.
  bool setCalibration(const TouchCalibration& c);
  // Back to the default table (the saved one is erased).
  void resetCalibration();

  // "[input] ..." lines: the tables, the settings, events dropped.
  void printStatus() const;

  // ---- a scripted finger, for tests over the console (ui t/h/s/d) ----
  // Replaces the panel until it lifts: lands on (x0, y0) (screen pixels,
  // already corrected), rests there dwellMs, slides to (x1, y1) in moveMs,
  // rests restMs, lifts. A fast slide that lifts at once is a fling (the
  // recogniser measures its speed as a finger's). A real touch cancels it.
  void simulate(int x0, int y0, int x1, int y1, uint32_t dwellMs, uint32_t moveMs, uint32_t restMs);
  bool simulating() const { return sim_.on; }
  // The last touch was the scripted finger's (for the log).
  bool scriptedTouch() const { return scripted_; }

private:
  static constexpr int kQueue = 8;

  void push(const InputEvent& e);
  void feedback(const InputEvent& e);
  void saveFlag(const char* key, bool on);

  Haptics& haptics_;
  TouchCalibration cal_ = TouchCalibration::defaults();
  bool custom_ = false;
  TouchRecognizer glass_;
  ButtonGesture buttons_[3];
  InputEvent queue_[kQueue];
  uint8_t head_ = 0;
  uint8_t count_ = 0;
  uint32_t dropped_ = 0;
  bool suspended_ = false;
  bool hapticsOn_ = true;
  bool railTicks_ = true;
  bool holdUsed_ = false;
  struct Sim {
    bool on = false;
    bool started = false;
    uint32_t t0 = 0;
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    uint32_t dwell = 0, move = 0, rest = 0;
  } sim_;
  bool scripted_ = false;
  bool simSample(uint32_t nowMs, TouchRecognizer::Sample& s);
};
