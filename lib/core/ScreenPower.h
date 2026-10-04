// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The screen's power (docs/ENERGY.md item 2): lit, then dim, then off, and a
// wake that never acts. The screen at 127 cost ~15 USB mA of the ~116 while
// streaming (measured), and nobody looks at it most of the time.
//
//   Bright  the chosen brightness (Low 60, Medium 100, High 160, Max 255;
//           Medium by default: 127 cost 3 mA more, 255 26 mA more)
//   Dim     backlight 30, for the last 10 s before Off (from 7 s with the
//           15 s choice): the screen is about to go
//   Off     backlight off (DCDC3) and the panel's sleep-in; nothing drawn
//
// The countdown runs from the last input (a touch that acts, a finger still
// on the glass or the strip, the PWR key), the same whether playing or not:
// "Screen off after" 15 s / 30 s (default) / 1 / 2 / 5 min / Never.
//
// What wakes it: a touch on the glass or the strip, or a PWR short press
// (wake()), and the events that need the listener (Why::Event: the
// headphones lost, "Couldn't reach", a track that failed, USB in or out).
// A touch on a screen that isn't Bright only brightens it: the input layer
// swallows it through its lift (WakeLatch, below). That is a hearing-safety
// rule, from Dim as from Off: a pocket press of B can't start music.
//
// The pocket guard: a wake from Off by a touch or the key that no input
// follows goes back to Off 10 s later, without the dim step. An event's
// wake gets the whole countdown (the listener has to get the device out).
//
// Unattended (the pocket rule): after any wake from Off (a touch, the key,
// an event, or keepLit) nobody may be looking: the device may be in a
// pocket, where contacts come in clusters. Until a touch on the glass lands
// (attend(); the listener is looking at it), the PWR key while lit, or the
// console, unattended() is true, and a B click that would start playing on
// the speaker is refused (ButtonPolicy::Transport::startRefused(): the
// inert buzz). Everything else acts: a pause, the volume, a B hold (a move
// to the speaker pauses first), a play on the headphones. Going Off ends it
// (the next wake decides again).
//
// The touch that ends it (glassLanded()) is remembered for the rest of
// that touch (landedUnattended()): the sleep timer's fade toast doesn't
// take it for +10 min or Turn off, which bring the music back up. In a
// pocket the first contact wakes the screen (swallowed) and the next one
// lands on a lit toast: it attends, but acts on nothing that raises the
// level; a listener's next tap does.
//
// An event's wake from Off lights the screen for the dialog, but a touch
// still only answers it (touchActs() false until then): the first contact
// after it is swallowed like a wake, as from Off, so a pocket's contact
// can't tap the dialog's "Use speaker". It keeps the event's countdown.
//
// It stays Bright while `keepLit` (step()): a screen of its own owns the
// display (the touch calibration, a spike tool), a play waits for the
// headphones, a pairing is under way. Coming on while Off, that wakes it.
//
// Held lit (`holdLit`, step()): a toast with a countdown is up, which the
// listener may be reading (the idle power-off's "Turning off in 30 s"
// [Keep on], the sleep timer's fade [+10 min] [Turn off]), or the Pair
// screen's search runs (2 min at most), which the listener watches for
// the headphones, tapping the list. A lit screen
// (Bright or Dim) goes Bright and stays so until it ends, the countdown
// running from then; an Off one stays off (it may be night: nobody is
// reading it), and one woken during it is held from the wake. A screen
// under the pocket guard isn't held: it still goes off 10 s after the
// wake unless input follows (the input then ends the guard, and it is
// held from there). For the idle warning any input ends the warning too.
// Held, a touch acts (touchActs()): none is swallowed as a wake.
//
// The settings are indices into the tables below (saved as such in NVS).
// Portable: fed with timestamps, no clock of its own; wraparound-safe.
class ScreenPower {
public:
  enum class Level : uint8_t { Bright, Dim, Off };
  // Why the level last changed (the log).
  enum class Why : uint8_t {
    Boot, Timeout, PocketGuard, Console, Touch, PowerKey, Event, KeepLit, HoldLit, Setting, SleepTimer
  };

  static constexpr uint8_t kDimBacklight = 30;
  static constexpr uint32_t kDimForMs = 10000;     // Dim for the last 10 s ...
  static constexpr uint32_t kShortDimAtMs = 7000;  // ... from 7 s for a timeout of 20 s or less
  static constexpr uint32_t kPocketMs = 10000;
  static constexpr int kTimeouts = 6;
  static constexpr int kDefaultTimeout = 1;  // 30 s
  static constexpr int kNever = kTimeouts - 1;
  static constexpr int kBrightnesses = 4;
  static constexpr int kDefaultBrightness = 1;  // Medium, 100

  // "Screen off after": its ms (0: never) and label ("15 s" ... "Never").
  static uint32_t timeoutMs(int choice);
  static const char* timeoutLabel(int choice);
  // "Brightness": the backlight level (M5GFX's 0-255) and label.
  static uint8_t brightnessLevel(int choice);
  static const char* brightnessLabel(int choice);
  // When a countdown of `offAfterMs` dims (0: never).
  static uint32_t dimAtMs(uint32_t offAfterMs);
  static int clampTimeout(int choice) { return choice < 0 || choice >= kTimeouts ? kDefaultTimeout : choice; }
  static int clampBrightness(int choice) {
    return choice < 0 || choice >= kBrightnesses ? kDefaultBrightness : choice;
  }

  // Bright from now (the boot); the countdown starts.
  void begin(uint32_t nowMs);

  // ---- settings ----
  // A new timeout (clamped): the countdown starts again, Bright.
  void setTimeout(int choice, uint32_t nowMs);
  int timeout() const { return timeout_; }
  void setBrightness(int choice) { brightness_ = clampBrightness(choice); }
  int brightness() const { return brightness_; }
  // Bright at `level` instead of the choice's (the console's Pb, not saved;
  // 0: the choice's again).
  void overrideBacklight(uint8_t level) { override_ = level; }
  uint8_t backlightOverride() const { return override_; }

  // ---- input ----
  // Input that acts (a touch while Bright, the scripted finger, the PWR key
  // while Bright) or a finger that moves or has only just landed
  // (FingerActivity): the countdown starts again and the pocket guard
  // ends. It never lights a screen that isn't Bright: only wake() does.
  void activity(uint32_t nowMs);
  // A deliberate input: a touch on the glass that lands and acts, the PWR
  // key while lit. Ends unattended().
  void attend() { unattended_ = false; }
  // A touch landing on the glass: attend(), and whether this touch is the
  // one that ended unattended() (landedUnattended(), until the next landing).
  void glassLanded() {
    landedUnattended_ = unattended_;
    unattended_ = false;
  }
  // The last touch to land on the glass landed while unattended() (it
  // woke nothing, but nobody had looked yet): it may be a pocket's second
  // contact. The fade toast's buttons don't act on it.
  bool landedUnattended() const { return landedUnattended_; }
  // A touch or the PWR key on a screen where a touch doesn't act (the input
  // layer swallows that touch; touchActs()), or an event that needs the
  // listener (Why::Event, at any level: the countdown starts again), or the
  // console (Ps1). True if it wasn't Bright: a wake. From Off by Touch or
  // PowerKey (with a timeout set): the pocket guard; any other wake ends
  // one. From Off by Touch, PowerKey or Event: unattended(); by Event also
  // !touchActs() until a Touch or PowerKey wake answers it. The console
  // ends both.
  bool wake(uint32_t nowMs, Why why);
  // Off now (the console's Ps0, the sleep timer's end). A touch's or the
  // key's wake from it has the pocket guard.
  void turnOff(Why why);

  // Every loop pass: the countdown, `keepLit` and `holdLit` (above; what
  // holds it is the caller's: ScreenControl::Hold). True
  // if the level changed since the last call (in here, or by wake() or
  // turnOff()).
  bool step(uint32_t nowMs, bool keepLit, bool holdLit = false);

  Level level() const { return level_; }
  bool bright() const { return level_ == Level::Bright; }
  bool off() const { return level_ == Level::Off; }
  // What the backlight should be: 0 Off, kDimBacklight, or the Bright level.
  uint8_t backlight() const;
  // The level before the last change, and why it changed.
  Level previous() const { return previous_; }
  Why why() const { return why_; }
  bool pocketGuard() const { return pocket_; }
  // A touch now acts: Bright, and not lit by an event from Off that no
  // touch has answered yet (else it only wakes, swallowed: Input::setLit).
  bool touchActs() const { return level_ == Level::Bright && !eventLit_; }
  // Woken from Off and nobody has touched the glass since (the pocket
  // rule, above).
  bool unattended() const { return unattended_; }
  // Ms until the next change by the countdown (0: none due; for the log).
  uint32_t msUntilNext(uint32_t nowMs) const;

  static const char* name(Level l);
  static const char* name(Why w);

private:
  void set(Level l, Why why);

  Level level_ = Level::Bright;
  Level previous_ = Level::Bright;
  Why why_ = Why::Boot;
  bool changed_ = false;
  bool pocket_ = false;
  bool unattended_ = false;
  bool landedUnattended_ = false;
  bool eventLit_ = false;
  uint32_t sinceMs_ = 0;  // the last input (or the wake, for the pocket guard)
  int timeout_ = kDefaultTimeout;
  int brightness_ = kDefaultBrightness;
  uint8_t override_ = 0;
};

// The touch that wakes the screen does nothing else, its lift included (a
// button clicks on its lift, a swipe ends with a fling there): the input
// layer drops every event while it holds (ui/Input, the path its
// setSuspended() uses; the recognisers keep following the finger, so
// nothing fires afterwards).
//
// It arms when a finger is on the panel while a touch doesn't act
// (ScreenPower::touchActs() false: dim, off, or lit by an event), and holds
// until no finger has been on the panel for kQuietMs. The panel loses a
// finger and finds it again (StripButtons: the captured one came back
// within ~145 ms, a swipe's later): a pass-count window rode out only ~10
// ms at the lit screen's 1-5 ms loop, and the finger found again was a
// fresh press of B. kQuietMs is StripButtons' swipe bounce window, the
// slowest find it allows for. Any finger counts, a second one too: while
// one is on, it holds. A touch that lands again within kQuietMs is still
// held (and holds it longer): a tap meant to act waits until then.
//
// A wake is a finger that LANDS while a touch doesn't act (none on the
// panel for kQuietMs before). A finger already on when the screen dimmed
// (one resting still: FingerActivity stopped counting it) is taken by the
// latch too, so it does nothing more in the dim, but it isn't a wake
// (`took`: the input layer ends its touch with a Cancel): else a resting
// finger would wake the screen as soon as it dimmed, and it could never
// go off.
//
// The scripted finger (the console's uit/uis/uip) isn't a finger here, so
// it bypasses the latch.
//
// Portable: one update per loop pass, before the touch becomes events;
// wraparound-safe.
class WakeLatch {
public:
  static constexpr uint32_t kQuietMs = 400;
  struct Result {
    bool hold = false;  // drop this pass's events
    bool woke = false;  // a touch just woke the screen (its first pass)
    bool took = false;  // a finger already on was just taken (not a wake)
  };
  // `touched`: a real finger on the panel (any); `acts`: a touch acts now
  // (ScreenPower::touchActs()).
  Result update(uint32_t nowMs, bool touched, bool acts);
  bool holding() const { return holding_; }

private:
  bool holding_ = false;
  bool seen_ = false;  // a finger was ever on (lastMs_ is valid)
  uint32_t lastMs_ = 0;  // the last pass a finger was on
};

// Whether a finger on the panel is input for the screen's countdown
// (ScreenPower::activity()): its landing and its moves are; a finger that
// has stayed within kMovePx of one spot for kStillCapMs no longer is. A
// pocket's pressure or a thigh resting on the glass would otherwise keep
// the screen lit for hours: now it dims and goes off under it (and the
// wake latch takes that finger, without a wake). A finger that acts, a
// drag, a hold on the A-Z rail all move or end well within the cap.
//
// Portable: one update per loop pass; wraparound-safe.
class FingerActivity {
public:
  static constexpr uint32_t kStillCapMs = 15000;
  static constexpr int kMovePx = 8;
  // `on`: a finger that acts is on the panel, at (x, y) (any consistent
  // pixels). True: it counts as input this pass.
  bool update(uint32_t nowMs, bool on, int x, int y);

private:
  bool on_ = false;
  uint32_t sinceMs_ = 0;  // it landed or last moved
  int16_t x_ = 0, y_ = 0;
};
