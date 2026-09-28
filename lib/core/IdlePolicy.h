#pragma once
#include <cstddef>
#include <cstdint>

#include "PlaybackController.h"

// Idle power-off (docs/ENERGY.md item 4, step 5): the device turns itself
// off after N minutes idle, so a night or a week in a bag doesn't drain it
// (the quietest state left on is still ~22 USB mA; off through the AXP192
// is ~0.3).
//
// Idle means all of these, all the time:
//   - the player is Stopped or Paused (not Playing, not Waiting for the
//     headphones);
//   - not on USB (ACIN or VBUS: the AXP192 can't stay off with it, and a
//     charger has power to spare);
//   - no Pair screen scan or pairing;
//   - no queue write under way (or an edit waiting to be written);
//   - nothing else busy (a screen of its own: the calibration, a spike
//     tool);
//   - the setting isn't Never.
// Anything that blocks restarts the countdown from the moment it goes, and
// so does any input: a touch on the glass or the strip, the PWR key, a
// headphone key that acted, the console. So it counts from the pause, the
// sleep timer's too (its pause is a pause like any other: ENERGY.md section
// 3, step 6).
//
// The phases, fed by update() every loop pass:
//
//   Blocked    something above blocks (blocker() says what).
//   Counting   idle since sinceMs().
//   Warning    the last 30 s: the UI shows "Turning off in 30 s" [Keep on]
//              (on a lit screen; it doesn't light a dark one: it may be
//              night). Any input, or a blocker, ends it: Counting again.
//   Releasing  the time is up: shutdown (once) says flush the queue, save
//              the "turned off" note, and let the headphones go (the sleep
//              timer's release: expected, no dialog). It waits until they
//              are unlinked, at most kReleaseWaitMs, so they see a clean
//              disconnect rather than a lost link. Input or a blocker
//              meanwhile cancels it (the note is cleared; the headphones
//              stay let go: a play pages them).
//   Off        powerOff (once): the caller turns the power off. Nothing more
//              happens here.
//
// Portable (host-tested: test_idle_policy); ui/ and main.cpp carry it out.
class IdlePolicy {
public:
  // "Turn off when idle" (the Output tab; saved): 10 / 20 / 60 min / Never.
  static constexpr int kChoices = 4;
  static constexpr int kDefaultChoice = 1;
  static constexpr int kNever = 3;
  static uint32_t choiceMs(int choice);        // 0: never
  static const char* choiceLabel(int choice);  // "10 min", ..., "Never"

  // The warning's length, before the end.
  static constexpr uint32_t kWarnMs = 30000;
  // The headphones' release: at most this long before the power goes.
  static constexpr uint32_t kReleaseWaitMs = 3000;

  enum class Phase : uint8_t { Blocked, Counting, Warning, Releasing, Off };
  // What blocks it (the first that applies, in this order).
  enum class Blocker : uint8_t { None, Never, Playing, Waiting, Usb, Pairing, QueueWrite, Busy };

  struct In {
    uint32_t nowMs = 0;
    PlayState play = PlayState::Stopped;
    bool usb = false;         // external power (ACIN or VBUS); unknown counts as present
    bool input = false;       // any input this pass (touch, strip, PWR, headphone key, console)
    bool pairing = false;     // the Pair screen's scan, or a pairing under way
    bool queueWrite = false;  // the queue file being written, or an edit waiting to be
    bool busy = false;        // a screen of its own is up (the calibration, a spike tool)
    bool linked = false;      // the headphones are linked (Releasing waits for them to go)
  };
  struct Out {
    bool warn = false;       // the warning starts
    bool warnEnd = false;    // the warning ends without turning off (input, a blocker)
    bool shutdown = false;   // the time is up: flush the queue, save the note, let the headphones go
    bool cancelled = false;  // input or a blocker during the release: it stays on (clear the note)
    bool powerOff = false;   // turn the power off now
  };

  void begin(int choice, uint32_t nowMs);
  // The setting (saved by the caller): the countdown starts again.
  void setChoice(int choice, uint32_t nowMs);
  int choice() const { return choice_; }
  // The console's test length (I<min>, Is<sec>) in place of the setting's,
  // until restart; 0: the setting's again. The countdown starts again.
  void setTestMs(uint32_t ms, uint32_t nowMs);
  uint32_t testMs() const { return testMs_; }
  // The length in force (the test's, else the setting's); 0: never.
  uint32_t lengthMs() const;

  Out update(const In& in);
  // The caller couldn't turn it off after all (external power seen at the
  // last moment): back to Blocked, counted again once it clears.
  void cancel(uint32_t nowMs);

  Phase phase() const { return phase_; }
  Blocker blocker() const { return blocker_; }
  uint32_t sinceMs() const { return sinceMs_; }
  // Until the power-off (Counting, Warning); 0 otherwise.
  uint32_t msLeft(uint32_t nowMs) const;
  // While warning: the seconds left, rounded up (1-30); 0 otherwise.
  uint32_t warnSeconds(uint32_t nowMs) const;

  static const char* phaseName(Phase p);
  static const char* blockerName(Blocker b);
  // The warning's text: "Turning off in 30 s".
  static void warnText(uint32_t seconds, char* buf, size_t size);
  // The next boot's toast for an idle length: "Turned off after 20 minutes
  // idle" ("1 minute", "45 s" for the console's test lengths).
  static void offText(uint32_t ms, char* buf, size_t size);

private:
  Blocker blockerOf(const In& in) const;
  void restart(uint32_t nowMs);

  int choice_ = kDefaultChoice;
  uint32_t testMs_ = 0;
  Phase phase_ = Phase::Blocked;
  Blocker blocker_ = Blocker::None;
  uint32_t sinceMs_ = 0;
  uint32_t releaseMs_ = 0;
};
