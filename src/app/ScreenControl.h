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
//   beginPass()   the PWR key (a short press wakes, or counts as input),
//                 USB in or out (read once a second: an event that wakes),
//                 and Input::setLit(): a touch on a screen that isn't
//                 fully lit (or lit by an event nobody has answered) is a
//                 wake, swallowed by the input layer;
//   afterInput()  the wake the input layer saw (logged as "[screen] wake by
//                 touch at x,y (raw)") and whether a finger acts (input:
//                 the countdown starts again; the console's scripted
//                 finger, which acts in the dark, also lights it); a touch
//                 landing on the glass ends the pocket rule;
//   step()        the countdown and what keeps it lit; a change is applied
//                 and logged. Off: onDark(true) (the UI stops drawing),
//                 then the backlight off and sleep-in. Awake again:
//                 sleep-out with the backlight still off, 5 ms (the
//                 panel's wait before the next command), onDark(false)
//                 (the UI draws everything, still dark), then the
//                 backlight. Nothing is drawn into a sleeping panel: on
//                 the device those pixels landed garbled in its memory
//                 (rows shifted, colours byte-swapped). The panel wants 120 ms between
//                 sleep-in and sleep-out: a change sooner waits for it
//                 (the touch still counts as a wake, and is swallowed).
//
// Events that need the listener call wake(why) at any time.
//
// Saved in NVS (namespace "screen"): "off_after" and "bright", the
// choices' indices. Loop task only.
class ScreenControl {
public:
  explicit ScreenControl(Input& input) : input_(input) {}

  // setup(), after M5.begin(): the settings, the backlight at the chosen
  // level, the countdown from now.
  void begin(uint32_t nowMs);
  // The UI's switch (Ui::setDark), called around the panel's sleep.
  void onDark(std::function<void(bool dark)> fn) { onDark_ = std::move(fn); }

  void beginPass(uint32_t nowMs);
  void afterInput(uint32_t nowMs);
  void step(uint32_t nowMs, bool keepLit);

  // Something needs the listener: lit (if dim or off), and the whole
  // countdown again. Logged when it was a wake.
  void wake(const char* why);

  // Off: nobody is looking (the reconnect's quiet rule, the probe).
  bool off() const { return power_.off(); }
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
  void setTimeout(int choice);
  void setBrightness(int choice);

  // The sleep timer paused (ENERGY.md section 3, step 4): off now, as the
  // countdown would (a touch's or the key's wake has the pocket guard).
  void sleepTimerOff();
  // A touch or the PWR key woke it (dim or off) since the last call: the
  // sleep timer's fade shows its toast then.
  bool takeWoken() {
    const bool w = woken_;
    woken_ = false;
    return w;
  }
  // Someone did something since the last call: a touch on the glass or
  // the strip (a wake too), the PWR key. The idle power-off's countdown
  // starts again (IdlePolicy).
  bool takeInput() {
    const bool i = inputSeen_;
    inputSeen_ = false;
    return i;
  }
  // External power: USB (ACIN) or VBUS present, as read once a second (the
  // AXP192's power status). Not read yet counts as present: the idle
  // power-off never acts on a guess.
  bool externalPower() const { return usb_ != 0; }
  // The same, read now (the idle power-off's last check).
  static bool readExternalPower();

  // ---- the console (P) ----
  // Ps0: off now (as the countdown would; a wake has the pocket guard).
  void consoleOff();
  // Ps1: on, as the PWR key does.
  void consoleOn();
  // Pb<n>: the Bright level until restart (not saved; 0: the setting's).
  // False: the screen isn't bright now (nothing changed).
  bool overrideBacklight(uint8_t level);
  // "[screen] ..." status.
  void printStatus() const;

private:
  void apply(uint32_t nowMs);
  void save(const char* key, uint8_t value);
  void logChange(uint32_t nowMs);

  Input& input_;
  ScreenPower power_;
  std::function<void(bool)> onDark_;
  bool asleep_ = false;          // the panel is in sleep-in (and the UI dark)
  uint32_t panelSinceMs_ = 0;    // the last sleep-in or sleep-out
  bool panelEver_ = false;       // (none yet: no 120 ms to wait)
  const char* wakeWhy_ = nullptr;  // an event's wake, for the log
  uint32_t nextUsbMs_ = 0;
  int8_t usb_ = -1;              // USB power present (ACIN or VBUS); -1 not read yet
  bool woken_ = false;           // takeWoken()
  bool inputSeen_ = false;      // takeInput()
};
