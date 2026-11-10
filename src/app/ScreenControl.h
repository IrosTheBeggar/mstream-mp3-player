// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>

#include "ScreenPower.h"

class Input;

// The screen policy on the Core2 (docs/ENERGY.md item 2; the decisions are
// ScreenPower's, host-tested): it alone switches the backlight (M5GFX's
// brightness: the AXP192's DCDC3, 0 = off) and the panel's sleep-in and
// sleep-out (under LcdLock: the panel shares SPI with the SD card).
//
// Each loop pass, in this order:
//
//   beginPass()   the PWR key (a short press turns a lit screen off and
//                 wakes an off one; on a screen kept lit it counts as input:
//                 ScreenPower::powerKey()),
//                 USB in or out (read once a second: an event that wakes),
//                 and Input::setLit(): a touch on a screen that isn't
//                 fully lit (dim, off, the glance, or lit by an event
//                 nobody has answered) is swallowed by the input layer;
//   afterInput()  the swallowed touch the input layer saw, and what "Touch
//                 when off" and the level make of it (ScreenPower::touch():
//                 nothing, a wake, the glance or its 10 s again, an event's
//                 answer; each logged, e.g. "[screen] wake by touch at x,y
//                 (raw)"), and whether a finger acts (input: the countdown
//                 starts again; the console's scripted finger, which acts
//                 in the dark, also lights it); a touch landing on the
//                 glass ends the pocket rule;
//   step()        the countdown, what keeps it lit, and what holds a lit
//                 screen lit (a countdown toast: the idle power-off's
//                 warning, the sleep timer's fade; the Pair screen's
//                 search, its 2 minutes at most); a change is applied
//                 and logged. Off: onDark(true) (the UI stops drawing),
//                 then the backlight off and sleep-in. Awake again:
//                 sleep-out with the backlight still off, 5 ms (the
//                 panel's wait before the next command), onDark(false)
//                 (the UI draws everything, still dark), then the
//                 backlight. The glance: the UI stays dark and
//                 onGlance(true) draws the glance instead of onDark(false);
//                 leaving it, onGlance(false) first (then the sleep-in, or
//                 onDark(false) for the whole screen: the panel is awake
//                 already). Nothing is drawn into a sleeping panel: on
//                 the device those pixels landed garbled in its memory
//                 (rows shifted, colours byte-swapped). The panel wants 120 ms between
//                 sleep-in and sleep-out: a change sooner waits for it
//                 (the touch still counts as a wake, and is swallowed).
//
// Events that need the listener call wake(why) at any time.
//
// Saved in NVS (namespace "screen"): "off_after", "bright" and "touch_off"
// (schema 6: "Touch when off"), the choices' indices. Loop task only.
class ScreenControl {
public:
  explicit ScreenControl(Input& input) : input_(input) {}

  // setup(), after M5.begin(): the settings, the backlight at the chosen
  // level, the countdown from now (with a UI, in effect from its first frame:
  // the start-up screen keeps it lit).
  void begin(uint32_t nowMs);
  // The UI's switch (Ui::setDark), called around the panel's sleep.
  void onDark(std::function<void(bool dark)> fn) { onDark_ = std::move(fn); }
  // The glance over the dark UI (Ui::setGlance): on once the panel is
  // awake (true: it was drawn; false: the backlight stays off), off before
  // the sleep-in or the whole screen's redraw (the return unused).
  void onGlance(std::function<bool(bool on)> fn) { onGlance_ = std::move(fn); }

  void beginPass(uint32_t nowMs);
  // The PWR key's short press this pass (beginPass() read it): what else it
  // ends (the computer's visualizer).
  bool powerKeyClicked() const { return keyClicked_; }
  void afterInput(uint32_t nowMs);
  // What holds a lit screen lit (ScreenPower's holdLit: it stays bright
  // until the hold ends, then the countdown from there; an off one stays
  // off; one under the pocket guard isn't held). Which one, for the log.
  //   Toast       a toast with a countdown is up (the idle power-off's
  //               warning, the sleep timer's fade): the listener may be
  //               reading it
  //   PairSearch  the Pair screen's search runs (Ui::pairSearching(), 2
  //               min at most): the listener watches the list for the
  //               headphones, and taps it; dim, the screen swallowed
  //               those taps as wakes, and off, it stopped the search
  enum class Hold : uint8_t { None, Toast, PairSearch };
  void step(uint32_t nowMs, bool keepLit, Hold hold);

  // Something needs the listener: lit (if dim or off), and the whole
  // countdown again. Logged when it was a wake.
  void wake(const char* why);

  // Off: nobody is looking (the reconnect's quiet rule, the probe). The
  // glance is lit: not off.
  bool off() const { return power_.off(); }
  // The glance ("Touch when off": Track): lit, the UI dark under it, nothing
  // on it acts; its 10 s, or the PWR key (the whole screen), end it.
  bool glancing() const { return power_.glance(); }
  // The panel is in sleep-in: nothing may be drawn (it lands garbled). It
  // can lag off() by a pass or two after a wake (sleep-out waits 120 ms
  // after a sleep-in).
  bool panelAsleep() const { return asleep_; }
  // Woken from off and nobody has touched the glass since (maybe a pocket):
  // a B click doesn't start the speaker (ScreenPower's pocket rule).
  bool unattended() const { return power_.unattended(); }
  // The touch on the glass now (the last to land) is the one that ended
  // unattended(): maybe a pocket's; the fade toast's buttons ignore it.
  bool landedUnattended() const { return power_.landedUnattended(); }
  const char* levelName() const { return ScreenPower::name(power_.level()); }
  // What the backlight is now (0 while off).
  uint8_t backlight() const;

  // ---- settings (saved) ----
  int timeoutChoice() const { return power_.timeout(); }
  int brightnessChoice() const { return power_.brightness(); }
  int offTouchChoice() const { return static_cast<int>(power_.offTouch()); }
  void setTimeout(int choice);
  void setBrightness(int choice);
  // "Touch when off" (ScreenPower::OffTouch's index): from the next touch
  // on an off screen.
  void setOffTouch(int choice);

  // The sleep timer paused (ENERGY.md section 3, step 4): off now, as the
  // countdown would (a touch's or the key's wake has the pocket guard).
  void sleepTimerOff();
  // A touch (dim, or off with Wake) or the PWR key (off, the glance) woke
  // it since the last call: the sleep timer's fade shows its toast then.
  bool takeWoken() {
    const bool w = woken_;
    woken_ = false;
    return w;
  }
  // Someone did something since the last call: a touch on the glass or
  // the strip (a wake too, but not a touch "Touch when off" ignores nor the
  // glance's), the PWR key. The idle power-off's countdown starts again
  // (IdlePolicy).
  bool takeInput() {
    const bool i = inputSeen_;
    inputSeen_ = false;
    return i;
  }
  // External power: USB (ACIN) or VBUS present, as read once a second (the
  // power chip's status: the AXP192's ACIN or VBUS, the AXP2101's VBUS; a
  // chip M5Unified doesn't name counts as present). Not read yet counts as
  // present: the idle power-off never acts on a guess.
  bool externalPower() const { return usb_ != 0; }
  // The same, read now (the idle power-off's last check).
  static bool readExternalPower();

  // ---- the console (P) ----
  // Ps0: off now (as the countdown would; a wake has the pocket guard).
  void consoleOff();
  // Ps1: on, from any level, the whole countdown and no pocket guard: someone
  // is at the console.
  void consoleOn();
  // Psk: the PWR key's short press, as if pressed (the next pass takes it).
  void consoleKey() { consoleKey_ = true; }
  // Psf: a real finger landing at 160,120, as the input layer's latch would
  // report it on a screen where a touch doesn't act (off, dim, the glance,
  // an event's wake): "Touch when off" decides, logged as such. The
  // scripted finger (uit) bypasses the latch, so it can't show this.
  void consoleFinger();
  // Pb<n>: the Bright level until restart (not saved; 0: the setting's),
  // applied at once. False: the screen isn't bright now (nothing changed).
  bool overrideBacklight(uint8_t level);
  // "[screen] ..." status.
  void printStatus() const;

private:
  void apply(uint32_t nowMs);
  void swallowed(uint32_t nowMs, int x, int y);
  void showGlance(bool on);
  uint8_t lit() const;  // the backlight now (0 for a glance nobody drew)
  const char* offTouchLabel() const;
  void save(const char* key, uint8_t value);
  void logChange(uint32_t nowMs);

  Input& input_;
  ScreenPower power_;
  std::function<void(bool)> onDark_;
  std::function<bool(bool)> onGlance_;
  bool asleep_ = false;          // the panel is in sleep-in (and the UI dark)
  bool glanceShown_ = false;     // the UI shows the glance (onGlance(true))
  bool glanceDrawn_ = false;     // ... and drew it
  uint32_t panelSinceMs_ = 0;    // the last sleep-in or sleep-out
  bool panelEver_ = false;       // (none yet: no 120 ms to wait)
  const char* wakeWhy_ = nullptr;  // an event's wake, for the log
  uint32_t nextUsbMs_ = 0;
  int8_t usb_ = -1;              // USB power present (ACIN or VBUS); -1 not read yet
  bool woken_ = false;           // takeWoken()
  bool inputSeen_ = false;      // takeInput()
  bool keyClicked_ = false;     // powerKeyClicked()
  bool consoleKey_ = false;     // consoleKey()
  // What keeps it lit, as the last step() was told (the PWR key reads it).
  // Lit from the start: loop()'s first M5.update() may read a press made
  // during the boot, maybe the one that powered the board on (the chip
  // latches it).
  bool keptLit_ = true;
  Hold hold_ = Hold::None;      // this pass's hold (for logChange())
  Hold held_ = Hold::None;      // what holds the screen lit now (for the log)
};
