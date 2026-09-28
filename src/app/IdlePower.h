#pragma once
#include <Arduino.h>

#include "IdlePolicy.h"

// The idle power-off on the Core2 (docs/ENERGY.md item 4, step 5): the
// decisions are IdlePolicy's (host-tested); main.cpp's stepIdle() feeds it
// and carries out what it says (the queue flushed, the headphones let go).
// This keeps what outlives a boot:
//
//   - the setting, "Turn off when idle" (the Output tab): NVS "power" /
//     "idle_after", the choice's index;
//   - the note that the device turned itself off: NVS "power" / "off_idle",
//     the idle length in ms, saved just before the power goes, read (and
//     removed) at the next boot for its toast ("Turned off after 20
//     minutes idle").
//
// And it turns the power off: M5.Power.powerOff() (the AXP192's power-off
// bit, then deep sleep with no wake source in case it didn't take). PWR
// boots it again. Never on USB (the caller's IdlePolicy blocks it, and
// stepIdle() reads the power status once more just before).
//
// Loop task only.
class IdlePower {
public:
  // setup(): the setting, and the note the last power-off left.
  void begin(uint32_t nowMs);
  IdlePolicy& policy() { return policy_; }
  const IdlePolicy& policy() const { return policy_; }

  // The setting (saved); the countdown starts again.
  void setChoice(int choice);
  int choice() const { return policy_.choice(); }

  // The next boot's toast: noted just before the power goes; cleared again
  // if it stays on after all.
  void noteOff(uint32_t idleMs);
  void clearNote();
  // The note found at boot, once: its toast's text. False: none.
  bool takeBootNote(char* buf, size_t size);

  // The power off, now: the display asleep, the AXP192 off. Doesn't return
  // (on the device; M5Unified deep-sleeps if the PMIC didn't take it).
  void powerOff();

  // "[power] idle ..." for the console.
  void printStatus(uint32_t nowMs) const;

private:
  IdlePolicy policy_;
  uint32_t bootNoteMs_ = 0;  // the note found at boot (0: none, or taken)
};
