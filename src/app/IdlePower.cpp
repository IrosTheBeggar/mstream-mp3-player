#include "app/IdlePower.h"

#include <M5Unified.h>
#include <Preferences.h>

#include "ui/LcdLock.h"

namespace {
constexpr const char* kPrefs = "power";
constexpr const char* kKeyChoice = "idle_after";
constexpr const char* kKeyNote = "off_idle";
}  // namespace

void IdlePower::begin(uint32_t nowMs) {
  int choice = IdlePolicy::kDefaultChoice;
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (p.begin(kPrefs, false)) {
    choice = p.getUChar(kKeyChoice, static_cast<uint8_t>(choice));
    if (p.isKey(kKeyNote)) {
      bootNoteMs_ = p.getUInt(kKeyNote, 0);
      p.remove(kKeyNote);  // once
    }
    p.end();
  }
  policy_.begin(choice, nowMs);
  Serial.printf("[power] turn off when idle: %s (stopped or paused, on battery, no input)\n",
                IdlePolicy::choiceLabel(policy_.choice()));
  if (bootNoteMs_) {
    char text[48];
    IdlePolicy::offText(bootNoteMs_, text, sizeof(text));
    Serial.printf("[power] the last power-off was the idle one: %s\n", text);
  }
}

void IdlePower::setChoice(int choice) {
  const int before = policy_.choice();
  policy_.setChoice(choice, millis());
  Preferences p;
  if (p.begin(kPrefs, false)) {
    p.putUChar(kKeyChoice, static_cast<uint8_t>(policy_.choice()));
    p.end();
  }
  Serial.printf("[power] turn off when idle: %s -> %s (saved)%s\n", IdlePolicy::choiceLabel(before),
                IdlePolicy::choiceLabel(policy_.choice()),
                policy_.testMs() ? "; the console's test length still wins until I0 or a restart" : "");
}

void IdlePower::noteOff(uint32_t idleMs) {
  Preferences p;
  if (!p.begin(kPrefs, false)) return;
  p.putUInt(kKeyNote, idleMs ? idleMs : 1);
  p.end();
}

void IdlePower::clearNote() {
  Preferences p;
  if (!p.begin(kPrefs, false)) return;
  if (p.isKey(kKeyNote)) p.remove(kKeyNote);
  p.end();
}

bool IdlePower::takeBootNote(char* buf, size_t size) {
  if (!bootNoteMs_) return false;
  IdlePolicy::offText(bootNoteMs_, buf, size);
  bootNoteMs_ = 0;
  return true;
}

void IdlePower::powerOff() {
  Serial.flush();
  // The panel shares SPI with the card: its sleep command under the lock
  // (nothing else runs after this).
  LcdLock lock;
  M5.Power.powerOff();
}

void IdlePower::printStatus(uint32_t nowMs) const {
  const IdlePolicy& p = policy_;
  char length[24];
  if (p.testMs()) {
    snprintf(length, sizeof(length), "%lu s (console test)", (unsigned long)(p.testMs() / 1000));
  } else {
    snprintf(length, sizeof(length), "%s", IdlePolicy::choiceLabel(p.choice()));
  }
  switch (p.phase()) {
    case IdlePolicy::Phase::Counting:
    case IdlePolicy::Phase::Warning:
      Serial.printf("[power] idle: off after %s; %s, off in %lu s\n", length, IdlePolicy::phaseName(p.phase()),
                    (unsigned long)((p.msLeft(nowMs) + 999) / 1000));
      break;
    case IdlePolicy::Phase::Blocked:
      Serial.printf("[power] idle: off after %s; waiting: %s\n", length, IdlePolicy::blockerName(p.blocker()));
      break;
    default:
      Serial.printf("[power] idle: off after %s; %s\n", length, IdlePolicy::phaseName(p.phase()));
      break;
  }
}
