// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/ScreenControl.h"

#include <M5Unified.h>
#include <Preferences.h>

#include "NvsLayout.h"
#include "app/NvsLater.h"
#include "ui/Input.h"
#include "ui/LcdLock.h"

namespace {
constexpr const char* kPrefs = nvslayout::kScreenNamespace;  // "screen"
constexpr const char* kKeyTimeout = "off_after";
constexpr const char* kKeyBrightness = "bright";
constexpr const char* kKeyOffTouch = nvslayout::kOffTouchKey;  // (schema 6)
// ILI9342C: 120 ms between a sleep-in and a sleep-out (either way), and
// 5 ms after a sleep-out before the next command (the next frame's pixels).
constexpr uint32_t kPanelSettleMs = 120;
constexpr uint32_t kSleepOutMs = 5;
constexpr uint32_t kUsbPollMs = 1000;
}  // namespace

void ScreenControl::begin(uint32_t nowMs) {
  int timeout = ScreenPower::kDefaultTimeout;
  int brightness = ScreenPower::kDefaultBrightness;
  int offTouch = ScreenPower::kDefaultOffTouch;
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (p.begin(kPrefs, false)) {
    timeout = p.getUChar(kKeyTimeout, static_cast<uint8_t>(timeout));
    brightness = p.getUChar(kKeyBrightness, static_cast<uint8_t>(brightness));
    // (Absent before schema 6: Nothing, the default the update brings.)
    offTouch = nvslayout::offTouchFrom(p.isKey(kKeyOffTouch), p.getUChar(kKeyOffTouch, 0));
    p.end();
  }
  power_.begin(nowMs);
  power_.setTimeout(timeout, nowMs);
  power_.setBrightness(brightness);
  power_.setOffTouch(offTouch);
  power_.step(nowMs, false);  // (the setting's "change" to Bright: none)
  M5.Display.setBrightness(power_.backlight());
  Serial.printf("[screen] off after %s (dims 10 s before), brightness %s (%u), touch when off: %s\n",
                ScreenPower::timeoutLabel(power_.timeout()), ScreenPower::brightnessLabel(power_.brightness()),
                (unsigned)power_.backlight(), offTouchLabel());
}

const char* ScreenControl::offTouchLabel() const {
  return ScreenPower::offTouchLabel(static_cast<int>(power_.offTouch()));
}

bool ScreenControl::readExternalPower() {
  // By the power chip M5Unified found: an AXP192 (USB on ACIN or VBUS) or
  // an AXP2101 (the Core2 v1.1: VBUS only, it has no ACIN). Any other:
  // present, so the idle power-off never turns off a board it can't read.
  switch (M5.Power.getType()) {
    case m5::Power_Class::pmic_axp192: return M5.Power.Axp192.isACIN() || M5.Power.Axp192.isVBUS();
    case m5::Power_Class::pmic_axp2101: return M5.Power.Axp2101.isVBUS();
    default: return true;
  }
}

uint8_t ScreenControl::backlight() const { return asleep_ ? 0 : M5.Display.getBrightness(); }

void ScreenControl::beginPass(uint32_t nowMs) {
  // The PWR key (AXP192's PEK, read by M5.update()): a short press turns a
  // lit screen off and wakes an off one (ScreenPower::powerKey(); the off
  // is logged by step()). On a screen kept lit it is input like a touch,
  // and deliberate: it answers a wake from off, else the pocket rule ends.
  keyClicked_ = M5.BtnPWR.wasClicked() || consoleKey_;
  consoleKey_ = false;
  if (keyClicked_) {
    inputSeen_ = true;
    if (power_.powerKey(nowMs, keptLit_, asleep_)) {
      Serial.println("[screen] wake by the power key");
      woken_ = true;
    } else if (keptLit_ && !asleep_) {
      Serial.println("[screen] the power key: stays on (something keeps it lit)");
    }
  }
  // USB plugged in or out: the charging state changed under the listener.
  if (static_cast<int32_t>(nowMs - nextUsbMs_) >= 0) {
    nextUsbMs_ = nowMs + kUsbPollMs;
    const int8_t usb = readExternalPower() ? 1 : 0;
    if (usb_ >= 0 && usb != usb_) wake(usb ? "USB plugged in" : "USB unplugged");
    usb_ = usb;
  }
  // A touch counts only on a screen that is lit at its level (not dim, not
  // off, not waiting for the panel's sleep-out) and not lit by an event
  // nobody has answered yet.
  input_.setLit(power_.touchActs() && !asleep_);
}

void ScreenControl::afterInput(uint32_t nowMs) {
  int x = 0, y = 0;
  if (input_.takeWake(&x, &y)) {
    swallowed(nowMs, x, y);
  } else if (input_.touching()) {
    inputSeen_ = true;
    if (input_.scriptedTouch() && !power_.bright()) {
      // The console's scripted finger acts in the dark (it bypasses the
      // latch); someone is at the console: lit, so what it does shows.
      power_.wake(nowMs, ScreenPower::Why::Console);
    } else {
      power_.activity(nowMs);  // a finger that acts, landing or moving
    }
  }
  if (input_.glassLanded()) {
    const bool was = power_.unattended();
    power_.glassLanded();  // someone is looking at it (this touch itself doesn't raise the sleep fade)
    if (was) Serial.println("[screen] a touch on the glass: attended (B plays on the speaker again)");
  }
}

// A finger that landed where a touch doesn't act (dim, off, the glance, lit
// by an event nobody has answered): the latch swallows it through its lift,
// and "Touch when off" and the level say what it does (ScreenPower::touch()).
// Every one is logged, with where it was (the panel's point, before the
// touch correction): a touch nobody made shows up here. Only a wake (from
// Dim, or Off with Wake) and an event's answer are input: an ignored touch
// and the glance's don't keep the player on (the idle power-off) or show
// the sleep timer's fade toast.
void ScreenControl::swallowed(uint32_t nowMs, int x, int y) {
  const bool strip = y >= 240;
  const char* on = strip ? " on the strip: " : "";
  const char* button = strip ? (x < 107 ? "A" : x < 214 ? "B" : "C") : "";
  using T = ScreenPower::Touched;
  const T what = power_.touch(nowMs);
  switch (what) {
    case T::Ignored:
      Serial.printf("[screen] no wake by touch at %d,%d (raw)%s%s: touch when off: %s (swallowed until it lifts)\n",
                    x, y, on, button, offTouchLabel());
      return;
    case T::Glance:
      Serial.printf("[screen] the glance by touch at %d,%d (raw)%s%s: off again in %lu s (swallowed until it lifts)\n",
                    x, y, on, button, (unsigned long)(ScreenPower::kGlanceMs / 1000));
      return;
    case T::GlanceAgain:
      Serial.printf("[screen] the glance again by touch at %d,%d (raw)%s%s: off in %lu s (swallowed until it lifts)\n",
                    x, y, on, button, (unsigned long)(ScreenPower::kGlanceMs / 1000));
      return;
    case T::Woke:
    case T::Answered:
      // (Answered: lit by an event, or the panel not awake yet: this touch
      // only answers it.)
      Serial.printf("[screen] %s by touch at %d,%d (raw)%s%s: swallowed until it lifts\n",
                    what == T::Answered ? "event's wake answered" : "wake", x, y, on, button);
      break;
  }
  woken_ = true;
  inputSeen_ = true;
}

void ScreenControl::consoleFinger() {
  if (power_.touchActs() && !asleep_) {
    Serial.println("[screen] Psf: a touch acts on this screen (not swallowed): uit taps it");
    return;
  }
  swallowed(millis(), 160, 120);
}

void ScreenControl::wake(const char* why) {
  const uint32_t now = millis();
  if (power_.wake(now, ScreenPower::Why::Event)) wakeWhy_ = why;  // (logged by step())
}

void ScreenControl::step(uint32_t nowMs, bool keepLit, Hold hold) {
  hold_ = hold;
  keptLit_ = keepLit;
  if (power_.step(nowMs, keepLit, hold != Hold::None)) logChange(nowMs);
  // The hold's start and end, on a screen it holds (a dim one brightening
  // is logged as a change too); one that changes what holds it starts
  // again. Ended on an off screen (the sleep timer's pause): nothing to say.
  // (The glance isn't held, like the pocket guard's screen.)
  const Hold held =
      hold != Hold::None && !power_.off() && !power_.glance() && !power_.pocketGuard() ? hold : Hold::None;
  if (held != held_) {
    if (held == Hold::Toast) {
      Serial.println("[screen] held lit while the toast counts down");
    } else if (held == Hold::PairSearch) {
      Serial.println("[screen] held lit while the Pair screen searches (2 min at most): no dim, no off");
    } else if (!power_.off() && !power_.glance()) {
      const char* gone = held_ == Hold::Toast ? "the toast is gone" : "the Pair screen's search stopped";
      const uint32_t next = power_.msUntilNext(nowMs);  // (0: Never)
      if (keepLit) {
        // (A device picked: the pairing keeps it lit from here.)
        Serial.printf("[screen] %s (still kept lit)\n", gone);
      } else if (next) {
        Serial.printf("[screen] %s: the countdown again (%s in %lu s)\n", gone, power_.pocketGuard() ? "off" : "dims",
                      (unsigned long)(next / 1000));
      } else {
        Serial.printf("[screen] %s (screen off after: Never)\n", gone);
      }
    }
    held_ = held;
  }
  apply(nowMs);
}

void ScreenControl::logChange(uint32_t nowMs) {
  const ScreenPower::Level l = power_.level();
  char after[64] = "";
  const uint32_t next = power_.msUntilNext(nowMs);
  if (power_.why() == ScreenPower::Why::HoldLit) {
    // (until the hold ends: step() logs that)
  } else if (l == ScreenPower::Level::Glance) {
    // (Rounded up: logged in the pass it opened, a few ms after its touch.)
    snprintf(after, sizeof(after), "; off again in %lu s", (unsigned long)((next + 999) / 1000));
  } else if (l == ScreenPower::Level::Bright && power_.pocketGuard()) {
    snprintf(after, sizeof(after), "; off again in %lu s without input", (unsigned long)(next / 1000));
  } else if (l == ScreenPower::Level::Bright && next) {
    snprintf(after, sizeof(after), "; dims in %lu s", (unsigned long)(next / 1000));
  } else if (l == ScreenPower::Level::Dim && next) {
    snprintf(after, sizeof(after), "; off in %lu s", (unsigned long)(next / 1000));
  }
  const bool event = power_.why() == ScreenPower::Why::Event && wakeWhy_;
  // (Held lit: ScreenPower names the toasts' hold; the search's is ours.)
  const char* why = power_.why() == ScreenPower::Why::HoldLit && hold_ == Hold::PairSearch
                        ? "held lit for the Pair screen's search"
                        : ScreenPower::name(power_.why());
  Serial.printf("[screen] %s -> %s (%s%s%s)%s\n", ScreenPower::name(power_.previous()), ScreenPower::name(l), why,
                event ? ": " : "", event ? wakeWhy_ : "", after);
  wakeWhy_ = nullptr;
}

// What the policy says, on the panel: the backlight at once; the sleep-in or
// sleep-out once 120 ms have passed since the last one. The glance is the
// UI's dark mode with the glance drawn over it (Ui::setGlance). Decided by
// what the panel and the UI show now (asleep_, glanceShown_), never by the
// level before: one pass can change the level twice (a touch's glance, then
// an event's wake), and a step that waits for the settle keeps its state.
//   Off -> glance     sleep-out, the glance drawn whole, the backlight
//   glance -> Off     the glance goes (nothing drawn), sleep-in
//   glance -> lit     the backlight off, the glance goes, the UI drawn again,
//                     the backlight (the PWR key, an event, keepLit, the
//                     console; the panel is awake)
//   lit -> glance     the same, dark first (a PWR off and a fresh landing in
//                     one pass, before the sleep-in)
// A glance the UI didn't draw (no UI: no PSRAM) keeps the backlight off:
// the panel's memory is stale.
void ScreenControl::apply(uint32_t nowMs) {
  const bool wantOff = power_.off();
  const bool wantGlance = power_.glance();
  if (wantOff != asleep_) {
    if (panelEver_ && nowMs - panelSinceMs_ < kPanelSettleMs) return;  // next pass
    panelEver_ = true;
    panelSinceMs_ = nowMs;
    if (wantOff) {
      showGlance(false);           // (nothing drawn: the UI is dark under it)
      if (onDark_) onDark_(true);  // the UI stops drawing first
      {
        LcdLock lock;  // the LCD shares the SPI bus with the card
        M5.Display.sleep();  // backlight off (DCDC3), then sleep-in (the panel keeps its memory)
      }
      asleep_ = true;
      return;
    }
    // Sleep-out with the backlight still off, then the UI draws it all
    // again, then the backlight: no half-drawn screen is seen. Not drawn
    // while the panel sleeps: measured on the device, pixels written to
    // this Core2's panel in sleep-in land garbled in its memory (rows
    // shifted, colours byte-swapped), and stay so until drawn again lit.
    const uint32_t t0 = millis();
    asleep_ = false;
    {
      LcdLock lock;
      M5.Display.getPanel()->setSleep(false);  // sleep-out only: DCDC3 stays off
    }
    // The panel takes 5 ms after a sleep-out before the next command (the
    // redraw's pixels, a list band's scroll registers).
    delay(kSleepOutMs);
    if (wantGlance) {
      showGlance(true);  // the UI stays dark; the glance draws itself whole
    } else if (onDark_) {
      onDark_(false);
    }
    M5.Display.setBrightness(lit());
    Serial.printf("[screen] awake in %lu ms (%s and sleep-out)\n", (unsigned long)(millis() - t0),
                  wantGlance ? "the glance" : "redraw");
    return;
  }
  if (!asleep_ && wantGlance != glanceShown_) {
    // The panel is awake and lit: dark while the UI swaps what it shows.
    const uint32_t t0 = millis();
    M5.Display.setBrightness(0);
    if (wantGlance) {
      if (onDark_) onDark_(true);
      showGlance(true);
    } else {
      showGlance(false);
      if (onDark_) onDark_(false);  // the whole screen, drawn again
    }
    M5.Display.setBrightness(lit());
    Serial.printf("[screen] %s in %lu ms (the panel awake)\n", wantGlance ? "the glance" : "the whole screen",
                  (unsigned long)(millis() - t0));
    return;
  }
  if (!asleep_ && M5.Display.getBrightness() != lit()) M5.Display.setBrightness(lit());
}

uint8_t ScreenControl::lit() const { return power_.glance() && !glanceDrawn_ ? 0 : power_.backlight(); }

void ScreenControl::showGlance(bool on) {
  if (on == glanceShown_) return;
  glanceShown_ = on;
  if (!on) {
    glanceDrawn_ = false;
    if (onGlance_) onGlance_(false);
    return;
  }
  glanceDrawn_ = onGlance_ && onGlance_(true);
  if (!glanceDrawn_) Serial.println("[screen] the glance: not drawn (no UI to draw it): the backlight stays off");
}

// Queued: written once the taps rest (app/NvsLater), not under the finger.
void ScreenControl::save(const char* key, uint8_t value) { nvslater::putU8(kPrefs, key, value); }

void ScreenControl::setTimeout(int choice) {
  const int before = power_.timeout();
  power_.setTimeout(choice, millis());
  save(kKeyTimeout, static_cast<uint8_t>(power_.timeout()));
  Serial.printf("[screen] off after: %s -> %s (saved once the taps rest)\n", ScreenPower::timeoutLabel(before),
                ScreenPower::timeoutLabel(power_.timeout()));
}

void ScreenControl::setOffTouch(int choice) {
  const char* before = offTouchLabel();
  power_.setOffTouch(choice);
  save(kKeyOffTouch, static_cast<uint8_t>(power_.offTouch()));
  Serial.printf("[screen] touch when off: %s -> %s (saved once the taps rest)\n", before, offTouchLabel());
}

void ScreenControl::setBrightness(int choice) {
  const int before = power_.brightness();
  power_.setBrightness(choice);
  power_.overrideBacklight(0);  // the listener's choice wins over a console Pb
  save(kKeyBrightness, static_cast<uint8_t>(power_.brightness()));
  Serial.printf("[screen] brightness: %s (%u) -> %s (%u) (saved once the taps rest)\n",
                ScreenPower::brightnessLabel(before),
                (unsigned)ScreenPower::brightnessLevel(before), ScreenPower::brightnessLabel(power_.brightness()),
                (unsigned)ScreenPower::brightnessLevel(power_.brightness()));
  // (Applied by the next step(), at once if the screen is bright.)
}

void ScreenControl::consoleOff() { power_.turnOff(ScreenPower::Why::Console); }

void ScreenControl::sleepTimerOff() { power_.turnOff(ScreenPower::Why::SleepTimer); }

void ScreenControl::consoleOn() { power_.wake(millis(), ScreenPower::Why::Console); }

bool ScreenControl::overrideBacklight(uint8_t level) {
  if (!power_.bright() || asleep_) return false;
  power_.overrideBacklight(level);
  apply(millis());  // now (bright, awake: only the backlight)
  return true;
}

void ScreenControl::printStatus() const {
  const uint32_t now = millis();
  Serial.printf("[screen] %s (backlight %u%s), off after %s, brightness %s (%u)%s, touch when off: %s; next change "
                "in %lu s%s%s\n",
                ScreenPower::name(power_.level()), (unsigned)backlight(), asleep_ ? ", panel asleep" : "",
                ScreenPower::timeoutLabel(power_.timeout()), ScreenPower::brightnessLabel(power_.brightness()),
                (unsigned)ScreenPower::brightnessLevel(power_.brightness()),
                power_.backlightOverride() ? " (console Pb override)" : "", offTouchLabel(),
                (unsigned long)(power_.msUntilNext(now) / 1000), power_.pocketGuard() ? " (pocket guard)" : "",
                !power_.unattended() ? ""
                : power_.glance()    ? " (the glance: nothing on it acts; the PWR key opens the screen)"
                : power_.touchActs() ? " (unattended: B won't start the speaker)"
                                     : " (lit by an event: a touch only answers it)");
}
