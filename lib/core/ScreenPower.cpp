// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ScreenPower.h"

#include "UiStrings.h"

namespace {

using uistr::S;
constexpr uint32_t kTimeoutMs[ScreenPower::kTimeouts] = {15000, 30000, 60000, 120000, 300000, 0};
constexpr S kTimeoutLabel[ScreenPower::kTimeouts] = {S::settings_screen_off_15s,  S::settings_screen_off_30s,
                                                     S::settings_screen_off_1min, S::settings_screen_off_2min,
                                                     S::settings_screen_off_5min, S::settings_screen_off_never};
constexpr uint8_t kBrightness[ScreenPower::kBrightnesses] = {60, 100, 160, 255};
constexpr S kBrightnessLabel[ScreenPower::kBrightnesses] = {S::settings_brightness_low, S::settings_brightness_medium,
                                                            S::settings_brightness_high, S::settings_brightness_max};
constexpr S kOffTouchLabel[ScreenPower::kOffTouches] = {S::settings_screen_touch_nothing, S::settings_screen_touch_wake,
                                                        S::settings_screen_touch_glance};

// Wraparound-safe: `now` is at least `ms` after `since`.
bool reached(uint32_t now, uint32_t since, uint32_t ms) { return now - since >= ms; }

}  // namespace

uint32_t ScreenPower::timeoutMs(int choice) { return kTimeoutMs[clampTimeout(choice)]; }
const char* ScreenPower::timeoutLabel(int choice) { return tr(kTimeoutLabel[clampTimeout(choice)]); }
uint8_t ScreenPower::brightnessLevel(int choice) { return kBrightness[clampBrightness(choice)]; }
const char* ScreenPower::brightnessLabel(int choice) { return tr(kBrightnessLabel[clampBrightness(choice)]); }
const char* ScreenPower::offTouchLabel(int choice) { return tr(kOffTouchLabel[clampOffTouch(choice)]); }

uint32_t ScreenPower::dimAtMs(uint32_t offAfterMs) {
  if (offAfterMs == 0) return 0;
  return offAfterMs > 2 * kDimForMs ? offAfterMs - kDimForMs : kShortDimAtMs;
}

void ScreenPower::begin(uint32_t nowMs) {
  level_ = previous_ = Level::Bright;
  why_ = Why::Boot;
  changed_ = false;
  pocket_ = false;
  unattended_ = eventLit_ = false;
  sinceMs_ = nowMs;
}

void ScreenPower::set(Level l, Why why) {
  if (l == level_) return;
  previous_ = level_;
  level_ = l;
  why_ = why;
  changed_ = true;
  // Off: nobody is looking; the next wake decides afresh.
  if (l == Level::Off) unattended_ = eventLit_ = false;
}

void ScreenPower::setTimeout(int choice, uint32_t nowMs) {
  timeout_ = clampTimeout(choice);
  sinceMs_ = nowMs;
  pocket_ = false;
  set(Level::Bright, Why::Setting);
}

void ScreenPower::activity(uint32_t nowMs) {
  // Not while it isn't Bright: that is a wake's (the touch is swallowed).
  if (level_ != Level::Bright) return;
  sinceMs_ = nowMs;
  pocket_ = false;
}

ScreenPower::Touched ScreenPower::touch(uint32_t nowMs) {
  switch (level_) {
    case Level::Off:
      switch (offTouch_) {
        case OffTouch::Nothing: return Touched::Ignored;  // (not even the countdown: it isn't input)
        case OffTouch::Wake: wake(nowMs, Why::Touch); return Touched::Woke;
        case OffTouch::Glance: break;
      }
      // The glance: lit, nothing to tap, Off again 10 s from now. Nobody has
      // looked yet (the PWR key from here is a wake from Off: the pocket
      // rule and the pocket guard).
      sinceMs_ = nowMs;
      pocket_ = eventLit_ = false;
      unattended_ = true;
      set(Level::Glance, Why::Touch);
      return Touched::Glance;
    case Level::Glance:
      // (A choice changed during it, by the console: the new one, as from Off.)
      if (offTouch_ == OffTouch::Nothing) return Touched::Ignored;
      if (offTouch_ == OffTouch::Wake) {
        wake(nowMs, Why::Touch);
        return Touched::Woke;
      }
      sinceMs_ = nowMs;  // the listener looks again: 10 s more (not input)
      return Touched::GlanceAgain;
    case Level::Dim: wake(nowMs, Why::Touch); return Touched::Woke;
    case Level::Bright: break;
  }
  wake(nowMs, Why::Touch);  // (lit by an event or keepLit from Off: this touch answers it)
  return Touched::Answered;
}

bool ScreenPower::wake(uint32_t nowMs, Why why) {
  const Level was = level_;
  const bool fromDark = dark();  // Off, or the glance (which nobody has used)
  sinceMs_ = nowMs;
  if (why == Why::Touch || why == Why::PowerKey) {
    // Maybe a pocket: from Off, off again soon unless input follows.
    if (fromDark) pocket_ = timeoutMs(timeout_) != 0;
    eventLit_ = false;  // (lit by an event: this touch answered it)
  } else {
    // An event needs the listener (and the console means someone is
    // there): the whole countdown.
    pocket_ = false;
    if (why == Why::Event) {
      if (fromDark) eventLit_ = true;  // the next touch only answers it
    } else {
      unattended_ = eventLit_ = false;  // the console: someone is there
    }
  }
  // From Off, nobody may be looking (a pocket): the pocket rule.
  if (fromDark && (why == Why::Touch || why == Why::PowerKey || why == Why::Event)) unattended_ = true;
  set(Level::Bright, why);
  return was != Level::Bright;
}

void ScreenPower::turnOff(Why why) {
  pocket_ = false;
  set(Level::Off, why);
}

bool ScreenPower::powerKey(uint32_t nowMs, bool keepLit, bool panelAsleep) {
  // (The glance: the key opens the whole screen, as from Off.)
  if (dark() || panelAsleep) return wake(nowMs, Why::PowerKey);
  if (!keepLit) {
    turnOff(Why::PowerKey);
    return false;
  }
  if (!touchActs()) return wake(nowMs, Why::PowerKey);
  activity(nowMs);
  attend();
  return false;
}

bool ScreenPower::step(uint32_t nowMs, bool keepLit, bool holdLit) {
  if (keepLit) {
    sinceMs_ = nowMs;
    pocket_ = false;
    // Lit from Off (or the glance) by what keeps it lit: as an event's wake
    // (nobody may be looking; the first touch only answers it).
    if (dark()) unattended_ = eventLit_ = true;
    set(Level::Bright, Why::KeepLit);
  } else if (level_ == Level::Glance) {
    // Its 10 s from the last touch, whatever the countdown's choice (Never
    // too) and whatever holds a lit screen lit (the toast isn't on it).
    if (reached(nowMs, sinceMs_, kGlanceMs)) set(Level::Off, Why::Glance);
  } else if (level_ != Level::Off) {
    const uint32_t offAfter = timeoutMs(timeout_);
    if (holdLit && !pocket_) {
      // A countdown toast is up, or the Pair screen searches, on a lit
      // screen: Bright until it ends (not input: the pocket guard's screen
      // isn't held, above).
      sinceMs_ = nowMs;
      set(Level::Bright, Why::HoldLit);
    } else if (pocket_) {
      // Woken in a pocket, maybe: no dim step, off again 10 s after the wake.
      if (reached(nowMs, sinceMs_, kPocketMs)) {
        pocket_ = false;
        set(Level::Off, Why::PocketGuard);
      }
    } else if (offAfter == 0) {
      set(Level::Bright, Why::Setting);  // Never (a dim left from before goes)
    } else if (reached(nowMs, sinceMs_, offAfter)) {
      set(Level::Off, Why::Timeout);
    } else if (reached(nowMs, sinceMs_, dimAtMs(offAfter))) {
      set(Level::Dim, Why::Timeout);
    }
  }
  const bool changed = changed_;
  changed_ = false;
  return changed;
}

uint8_t ScreenPower::backlight() const {
  switch (level_) {
    case Level::Off: return 0;
    case Level::Dim: return kDimBacklight;
    case Level::Bright:
    case Level::Glance: break;
  }
  return override_ ? override_ : brightnessLevel(brightness_);
}

uint32_t ScreenPower::msUntilNext(uint32_t nowMs) const {
  if (level_ == Level::Off) return 0;
  const uint32_t elapsed = nowMs - sinceMs_;
  if (level_ == Level::Glance) return elapsed >= kGlanceMs ? 0 : kGlanceMs - elapsed;
  if (pocket_) return elapsed >= kPocketMs ? 0 : kPocketMs - elapsed;
  const uint32_t offAfter = timeoutMs(timeout_);
  if (offAfter == 0) return 0;
  const uint32_t at = level_ == Level::Bright ? dimAtMs(offAfter) : offAfter;
  return elapsed >= at ? 0 : at - elapsed;
}

const char* ScreenPower::name(Level l) {
  switch (l) {
    case Level::Bright: return "bright";
    case Level::Dim: return "dim";
    case Level::Off: return "off";
    case Level::Glance: return "glance";
  }
  return "?";
}

const char* ScreenPower::name(Why w) {
  switch (w) {
    case Why::Boot: return "boot";
    case Why::Timeout: return "no input";
    case Why::PocketGuard: return "pocket guard: no input after the wake";
    case Why::Console: return "console";
    case Why::Touch: return "a touch, swallowed";
    case Why::PowerKey: return "the power key";
    case Why::Event: return "an event for the listener";
    case Why::KeepLit: return "kept lit";
    case Why::HoldLit: return "held lit for a countdown toast";
    case Why::Setting: return "the setting";
    case Why::SleepTimer: return "the sleep timer";
    case Why::Glance: return "the glance's 10 s";
  }
  return "?";
}

// ---- WakeLatch ----

WakeLatch::Result WakeLatch::update(uint32_t nowMs, bool touched, bool acts) {
  Result r;
  // A finger on the panel within kQuietMs before this pass (the same
  // finger, lost for a moment, or one already resting).
  const bool recent = seen_ && nowMs - lastMs_ < kQuietMs;
  if (touched) {
    seen_ = true;
    lastMs_ = nowMs;
  }
  // No finger for kQuietMs (a stalled loop may not have run the passes
  // between): the hold is over, and a finger now is a new touch.
  if (holding_ && !recent) holding_ = false;
  if (!holding_) {
    if (!touched || acts) return r;
    holding_ = true;
    r.hold = true;
    // A finger landing is a wake; one already on as the screen dimmed isn't.
    if (recent) {
      r.took = true;
    } else {
      r.woke = true;
    }
    return r;
  }
  // Held through the lift, and until no finger has been on for kQuietMs.
  r.hold = true;
  return r;
}

// ---- FingerActivity ----

bool FingerActivity::update(uint32_t nowMs, bool on, int x, int y) {
  if (!on) {
    on_ = false;
    return false;
  }
  const int dx = x - x_, dy = y - y_;
  const bool moved = dx > kMovePx || dx < -kMovePx || dy > kMovePx || dy < -kMovePx;
  if (!on_ || moved) {
    on_ = true;
    sinceMs_ = nowMs;
    x_ = static_cast<int16_t>(x);
    y_ = static_cast<int16_t>(y);
    return true;
  }
  return nowMs - sinceMs_ < kStillCapMs;
}
