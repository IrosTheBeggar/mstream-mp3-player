#include "app/ScreenControl.h"

#include <M5Unified.h>
#include <Preferences.h>

#include "ui/Input.h"
#include "ui/LcdLock.h"

namespace {
constexpr const char* kPrefs = "screen";
constexpr const char* kKeyTimeout = "off_after";
constexpr const char* kKeyBrightness = "bright";
// ILI9342C: 120 ms between a sleep-in and a sleep-out (either way), and
// 5 ms after a sleep-out before the next command (the next frame's pixels).
constexpr uint32_t kPanelSettleMs = 120;
constexpr uint32_t kSleepOutMs = 5;
constexpr uint32_t kUsbPollMs = 1000;
// AXP192 reg 0x00 (power status): bit 7 ACIN present, bit 5 VBUS present.
constexpr uint8_t kAxpStatus = 0x00;
constexpr uint8_t kAxpUsbBits = 0xA0;
}  // namespace

void ScreenControl::begin(uint32_t nowMs) {
  int timeout = ScreenPower::kDefaultTimeout;
  int brightness = ScreenPower::kDefaultBrightness;
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (p.begin(kPrefs, false)) {
    timeout = p.getUChar(kKeyTimeout, static_cast<uint8_t>(timeout));
    brightness = p.getUChar(kKeyBrightness, static_cast<uint8_t>(brightness));
    p.end();
  }
  power_.begin(nowMs);
  power_.setTimeout(timeout, nowMs);
  power_.setBrightness(brightness);
  power_.step(nowMs, false);  // (the setting's "change" to Bright: none)
  M5.Display.setBrightness(power_.backlight());
  Serial.printf("[screen] off after %s (dims 10 s before), brightness %s (%u)\n",
                ScreenPower::timeoutLabel(power_.timeout()), ScreenPower::brightnessLabel(power_.brightness()),
                (unsigned)power_.backlight());
}

bool ScreenControl::readExternalPower() {
  return (M5.Power.Axp192.readRegister8(kAxpStatus) & kAxpUsbBits) != 0;
}

uint8_t ScreenControl::backlight() const { return asleep_ ? 0 : M5.Display.getBrightness(); }

void ScreenControl::beginPass(uint32_t nowMs) {
  // The PWR key (AXP192's PEK, read by M5.update()): a short press wakes a
  // screen that isn't bright (or answers an event's wake), else it is
  // input like a touch, and deliberate: the pocket rule ends.
  if (M5.BtnPWR.wasClicked()) {
    inputSeen_ = true;
    if (power_.touchActs() && !asleep_) {
      power_.activity(nowMs);
      power_.attend();
    } else if (power_.wake(nowMs, ScreenPower::Why::PowerKey)) {
      Serial.println("[screen] wake by the power key");
      woken_ = true;
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
    // Every swallowed wake, with where it was (the panel's point, before
    // the touch correction): a wake nobody made shows up here.
    const bool strip = y >= 240;
    const bool answer = power_.bright();  // lit by an event: this touch only answers it
    Serial.printf("[screen] %s by touch at %d,%d (raw)%s%s: swallowed until it lifts\n",
                  answer ? "event's wake answered" : "wake", x, y, strip ? " on the strip: " : "",
                  strip ? (x < 107 ? "A" : x < 214 ? "B" : "C") : "");
    power_.wake(nowMs, ScreenPower::Why::Touch);
    woken_ = true;
    inputSeen_ = true;
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

void ScreenControl::wake(const char* why) {
  const uint32_t now = millis();
  if (power_.wake(now, ScreenPower::Why::Event)) wakeWhy_ = why;  // (logged by step())
}

void ScreenControl::step(uint32_t nowMs, bool keepLit, bool holdLit) {
  if (power_.step(nowMs, keepLit, holdLit)) logChange(nowMs);
  // The hold's start and end, on a screen it holds (a dim one brightening
  // is logged as a change too).
  const bool held = holdLit && !power_.off() && !power_.pocketGuard();
  if (held != held_) {
    if (held) {
      Serial.println("[screen] held lit while the toast counts down");
    } else if (!power_.off()) {
      const uint32_t next = power_.msUntilNext(nowMs);  // (0: Never)
      if (next) {
        Serial.printf("[screen] the toast is gone: the countdown again (%s in %lu s)\n",
                      power_.pocketGuard() ? "off" : "dims", (unsigned long)(next / 1000));
      } else {
        Serial.println("[screen] the toast is gone (screen off after: Never)");
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
    // (until the toast ends: step() logs that)
  } else if (l == ScreenPower::Level::Bright && power_.pocketGuard()) {
    snprintf(after, sizeof(after), "; off again in %lu s without input", (unsigned long)(next / 1000));
  } else if (l == ScreenPower::Level::Bright && next) {
    snprintf(after, sizeof(after), "; dims in %lu s", (unsigned long)(next / 1000));
  } else if (l == ScreenPower::Level::Dim && next) {
    snprintf(after, sizeof(after), "; off in %lu s", (unsigned long)(next / 1000));
  }
  const bool event = power_.why() == ScreenPower::Why::Event && wakeWhy_;
  Serial.printf("[screen] %s -> %s (%s%s%s)%s\n", ScreenPower::name(power_.previous()), ScreenPower::name(l),
                ScreenPower::name(power_.why()), event ? ": " : "", event ? wakeWhy_ : "", after);
  wakeWhy_ = nullptr;
}

// What the policy says, on the panel: the backlight at once; the sleep-in or
// sleep-out once 120 ms have passed since the last one.
void ScreenControl::apply(uint32_t nowMs) {
  const bool wantOff = power_.off();
  if (wantOff != asleep_) {
    if (panelEver_ && nowMs - panelSinceMs_ < kPanelSettleMs) return;  // next pass
    panelEver_ = true;
    panelSinceMs_ = nowMs;
    if (wantOff) {
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
    if (onDark_) onDark_(false);
    M5.Display.setBrightness(power_.backlight());
    Serial.printf("[screen] awake in %lu ms (redraw and sleep-out)\n", (unsigned long)(millis() - t0));
    return;
  }
  if (!asleep_ && M5.Display.getBrightness() != power_.backlight()) M5.Display.setBrightness(power_.backlight());
}

void ScreenControl::save(const char* key, uint8_t value) {
  Preferences p;
  if (!p.begin(kPrefs, false)) return;
  p.putUChar(key, value);
  p.end();
}

void ScreenControl::setTimeout(int choice) {
  const int before = power_.timeout();
  power_.setTimeout(choice, millis());
  save(kKeyTimeout, static_cast<uint8_t>(power_.timeout()));
  Serial.printf("[screen] off after: %s -> %s (saved)\n", ScreenPower::timeoutLabel(before),
                ScreenPower::timeoutLabel(power_.timeout()));
}

void ScreenControl::setBrightness(int choice) {
  const int before = power_.brightness();
  power_.setBrightness(choice);
  power_.overrideBacklight(0);  // the listener's choice wins over a console Pb
  save(kKeyBrightness, static_cast<uint8_t>(power_.brightness()));
  Serial.printf("[screen] brightness: %s (%u) -> %s (%u) (saved)\n", ScreenPower::brightnessLabel(before),
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
  Serial.printf("[screen] %s (backlight %u%s), off after %s, brightness %s (%u)%s; next change in %lu s%s%s\n",
                ScreenPower::name(power_.level()), (unsigned)backlight(), asleep_ ? ", panel asleep" : "",
                ScreenPower::timeoutLabel(power_.timeout()), ScreenPower::brightnessLabel(power_.brightness()),
                (unsigned)ScreenPower::brightnessLevel(power_.brightness()),
                power_.backlightOverride() ? " (console Pb override)" : "",
                (unsigned long)(power_.msUntilNext(now) / 1000), power_.pocketGuard() ? " (pocket guard)" : "",
                !power_.unattended()  ? ""
                : power_.touchActs() ? " (unattended: B won't start the speaker)"
                                     : " (lit by an event: a touch only answers it)");
}
