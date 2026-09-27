#pragma once
#include <cstdint>

#include "ButtonGesture.h"
#include "InputEvent.h"
#include "StripButtons.h"
#include "TouchCalibration.h"
#include "TouchRecognizer.h"
#include "app/Haptics.h"

// The one input layer (the tab bar spec's dispatcher, §10.1). It alone reads
// the touch panel; everything downstream gets InputEvents from poll():
//
//   - the glass: every touch point is corrected (TouchCalibration: the
//     user's panel reads x up to ~45 px too far right) before anything hit
//     tests it, then TouchRecognizer makes Down, Tap, LongPress, Release,
//     DragStart/Move/End and Fling (capped at 2,000 px/s) of it;
//   - the buttons (the strip below the LCD, raw y >= 240): StripButtons
//     makes their presses from the same touch point (only a touch that
//     went down there, and stays put; never a swipe from the glass; a
//     swipe UP from the strip is handed to the glass as a drag, so a scroll
//     that starts on the strip scrolls: InputEvent::fromStrip), then
//     ButtonGesture makes Click, Hold (500 ms), Repeat (A and C, every
//     200 ms) and HoldEnd; ButtonPolicy (main.cpp) decides what they do.
//     M5Unified's BtnA/B/C are not read (they press for any point in the
//     strip, a swipe's end too); only the input lab looks at them. Like the
//     glass, only the panel's first touch point counts: a second finger
//     pressing the strip does nothing (and is logged).
//
// A strip touch that doesn't count is logged ("[button] ignored: ..."), and
// so is a swipe handed over ("[button] B at x,y (raw): a swipe from the
// strip (n px): scrolling").
//
// The feedback, as the user chose it: a tap tick (33 ms, strong) and, the
// moment a hold is recognised, a double tick. The buttons' is played once
// ButtonPolicy has acted (buttonFeedback()): a click with nothing to play
// gets a short double buzz instead ("inert", spec §4 and §7). The glass's is played by
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
  // Every loop pass, after M5.update(): samples the touch panel (the glass
  // and the button strip), queues their events.
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
  // A touch button's event once ButtonPolicy has handled it: the tick for
  // a click, the double tick for a hold; `acted` false (a click with
  // nothing to play): the inert buzz.
  void buttonFeedback(const InputEvent& e, bool acted);
  // Not a touch's: the headphones asked for are connected (the double
  // tick), or dropped while playing (one long buzz, 80 ms).
  void connectedTick();
  void alertBuzz();
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

  // ---- a scripted finger, for tests over the console (ui t/h/s/d/p) ----
  // Replaces the panel until it lifts: lands on (x0, y0) (screen pixels,
  // already corrected), rests there dwellMs, slides to (x1, y1) in moveMs,
  // rests restMs, lifts. A fast slide that lifts at once is a fling (the
  // recogniser measures its speed as a finger's). Points at y >= 240 are on
  // the button strip and go through StripButtons like a finger's (a press
  // there is a click or a hold; a swipe up from there scrolls). A real
  // touch cancels it.
  void simulate(int x0, int y0, int x1, int y1, uint32_t dwellMs, uint32_t moveMs, uint32_t restMs);
  bool simulating() const { return sim_.on; }
  // The last touch was the scripted finger's (for the log).
  bool scriptedTouch() const { return scripted_; }

private:
  static constexpr int kQueue = 8;

  void push(const InputEvent& e);
  void saveFlag(const char* key, bool on);

  Haptics& haptics_;
  TouchCalibration cal_ = TouchCalibration::defaults();
  bool custom_ = false;
  TouchRecognizer glass_;
  StripButtons strip_;
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
  // The finger of the first touch point last pass (the panel's id;
  // kScriptedId for the scripted finger; -1 none), and the other fingers
  // whose strip press was logged as ignored (a bit per id).
  static constexpr int kScriptedId = 0x7f;
  int touchId_ = -1;
  uint8_t otherLogged_ = 0;
  bool simSample(uint32_t nowMs, TouchRecognizer::Sample& s);
  void updateButtons(uint32_t nowMs, const TouchRecognizer::Sample& s, bool newTouch);
  void logOtherFingers(int firstId);
};
