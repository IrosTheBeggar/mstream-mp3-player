#include "ScreenPower.h"

namespace {

constexpr uint32_t kTimeoutMs[ScreenPower::kTimeouts] = {15000, 30000, 60000, 120000, 300000, 0};
constexpr const char* kTimeoutLabel[ScreenPower::kTimeouts] = {"15 s", "30 s", "1 min", "2 min", "5 min", "Never"};
constexpr uint8_t kBrightness[ScreenPower::kBrightnesses] = {60, 100, 160, 255};
constexpr const char* kBrightnessLabel[ScreenPower::kBrightnesses] = {"Low", "Medium", "High", "Max"};

// Wraparound-safe: `now` is at least `ms` after `since`.
bool reached(uint32_t now, uint32_t since, uint32_t ms) { return now - since >= ms; }

}  // namespace

uint32_t ScreenPower::timeoutMs(int choice) { return kTimeoutMs[clampTimeout(choice)]; }
const char* ScreenPower::timeoutLabel(int choice) { return kTimeoutLabel[clampTimeout(choice)]; }
uint8_t ScreenPower::brightnessLevel(int choice) { return kBrightness[clampBrightness(choice)]; }
const char* ScreenPower::brightnessLabel(int choice) { return kBrightnessLabel[clampBrightness(choice)]; }

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

bool ScreenPower::wake(uint32_t nowMs, Why why) {
  const Level was = level_;
  sinceMs_ = nowMs;
  if (why == Why::Touch || why == Why::PowerKey) {
    // Maybe a pocket: from Off, off again soon unless input follows.
    if (was == Level::Off) pocket_ = timeoutMs(timeout_) != 0;
    eventLit_ = false;  // (lit by an event: this touch answered it)
  } else {
    // An event needs the listener (and the console means someone is
    // there): the whole countdown.
    pocket_ = false;
    if (why == Why::Event) {
      if (was == Level::Off) eventLit_ = true;  // the next touch only answers it
    } else {
      unattended_ = eventLit_ = false;  // the console: someone is there
    }
  }
  // From Off, nobody may be looking (a pocket): the pocket rule.
  if (was == Level::Off && (why == Why::Touch || why == Why::PowerKey || why == Why::Event)) unattended_ = true;
  set(Level::Bright, why);
  return was != Level::Bright;
}

void ScreenPower::turnOff(Why why) {
  pocket_ = false;
  set(Level::Off, why);
}

bool ScreenPower::step(uint32_t nowMs, bool keepLit, bool holdLit) {
  if (keepLit) {
    sinceMs_ = nowMs;
    pocket_ = false;
    // Lit from Off by what keeps it lit: as an event's wake (nobody may be
    // looking; the first touch only answers it).
    if (level_ == Level::Off) unattended_ = eventLit_ = true;
    set(Level::Bright, Why::KeepLit);
  } else if (level_ != Level::Off) {
    const uint32_t offAfter = timeoutMs(timeout_);
    if (holdLit && !pocket_) {
      // A countdown toast is up on a lit screen: Bright until it ends (not
      // input: the pocket guard's screen isn't held, above).
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
    case Level::Bright: break;
  }
  return override_ ? override_ : brightnessLevel(brightness_);
}

uint32_t ScreenPower::msUntilNext(uint32_t nowMs) const {
  if (level_ == Level::Off) return 0;
  const uint32_t elapsed = nowMs - sinceMs_;
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
