#include "app/PowerSettings.h"

#include <Preferences.h>

#include "audio/BtSink.h"
#include "ui/LcdLock.h"

namespace pc = powerchoice;

namespace {
constexpr const char* kPrefs = "power";
constexpr const char* kKeyCpu = "cpu_mhz";    // 160 or 240; absent: the default (shared with Pcb)
constexpr const char* kKeyBt = "bt_tx";       // the Bluetooth power choice; absent: Normal
constexpr const char* kKeyNote = "boot_cpu";  // a restart for the CPU speed: the speed, for the toast

// What applyBootClock() found and did, for begin()'s log line.
uint16_t bootStored = 0;
bool bootSet = false;
uint16_t bootMhz = 0;  // the clock it left
}  // namespace

// ---- the boot ----

void PowerSettings::applyBootClock() {
  bootStored = cpuStored();
  const uint16_t want = pc::cpuMhzFromStored(bootStored);
  // Before Bluetooth starts: 240 <-> 160 retunes the PLL the radio runs
  // from, which is only safe while the radio is off.
  if (want != getCpuFrequencyMhz()) bootSet = setCpuFrequencyMhz(want);
  bootMhz = static_cast<uint16_t>(getCpuFrequencyMhz());
}

uint16_t PowerSettings::cpuStored() {
  Preferences p;
  // Read-write: a read-only open of a namespace never written logs an error.
  if (!p.begin(kPrefs, false)) return 0;
  const uint16_t v = p.getUShort(kKeyCpu, 0);
  p.end();
  return v;
}

uint16_t PowerSettings::cpuBootMhz() { return bootMhz ? bootMhz : static_cast<uint16_t>(getCpuFrequencyMhz()); }

void PowerSettings::begin() {
  cpuSaved_ = pc::cpuMhzFromStored(bootStored);
  Preferences p;
  if (p.begin(kPrefs, false)) {
    btChoice_ = pc::btChoiceFromStored(p.isKey(kKeyBt) ? p.getUChar(kKeyBt, pc::kDefaultBt) : -1);
    if (p.isKey(kKeyNote)) {
      bootNoteMhz_ = p.getUShort(kKeyNote, 0);
      p.remove(kKeyNote);  // once
    }
    p.end();
  }
  // (The Output tab's CPU speed, or the console's Pcb, saves it.)
  const char* from = !bootStored                   ? "the default"
                     : pc::validCpuMhz(bootStored) ? "saved; Pcb0 goes back to the default"
                                                   : "the default: the saved value isn't a speed it takes";
  if (cpuBootMhz() == cpuSaved_) {
    Serial.printf("[power] CPU %u MHz from boot (%s)\n", (unsigned)cpuBootMhz(), from);
  } else {
    Serial.printf("[power] CPU %u MHz from boot: COULDN'T SET %u MHz (%s; %s)\n", (unsigned)cpuBootMhz(),
                  (unsigned)cpuSaved_, from, bootSet ? "set, but it reads back otherwise" : "refused");
  }
  char range[16];
  pc::btRangeText(btChoice_, range, sizeof(range));
  Serial.printf("[power] bluetooth power: %s (%s)\n", pc::btLabel(btChoice_), range);
  if (bootNoteMhz_) Serial.printf("[power] restarted for the CPU speed: %u MHz asked\n", (unsigned)bootNoteMhz_);
}

void PowerSettings::beginBluetooth(BtSink& bt) {
  const pc::TxLevels l = pc::btLevels(btChoice_);
  bt.setTxPower(l.min, l.max);  // applied as its controller comes up
}

// ---- CPU speed ----

bool PowerSettings::saveCpu(uint16_t mhz) {
  if (!pc::validCpuMhz(mhz)) return false;
  Preferences p;
  if (!p.begin(kPrefs, false)) return false;
  const bool ok = p.putUShort(kKeyCpu, mhz) == sizeof(uint16_t);
  p.end();
  if (ok) cpuSaved_ = mhz;
  return ok;
}

void PowerSettings::clearCpu() {
  Preferences p;
  if (p.begin(kPrefs, false)) {
    if (p.isKey(kKeyCpu)) p.remove(kKeyCpu);
    p.end();
  }
  cpuSaved_ = pc::kDefaultCpuMhz;
}

void PowerSettings::noteRestart(uint16_t mhz) {
  Preferences p;
  if (!p.begin(kPrefs, false)) return;
  p.putUShort(kKeyNote, mhz);
  p.end();
}

bool PowerSettings::takeBootNote(char* buf, size_t size) {
  if (!bootNoteMhz_) return false;
  // What runs (the note says what was asked; a failed switch shows as is).
  pc::cpuBootText(cpuBootMhz(), buf, size);
  bootNoteMhz_ = 0;
  return true;
}

void PowerSettings::restart() {
  Serial.flush();
  // The panel shares SPI with the card: nothing half-sent to either (the
  // lock is never released: nothing else runs after this).
  LcdLock lock;
  esp_restart();
}

// ---- Bluetooth power ----

void PowerSettings::setBt(int choice, BtSink& bt) {
  const int before = btChoice_;
  btChoice_ = pc::btChoiceFromStored(choice);
  ++btChanges_;
  Preferences p;
  if (p.begin(kPrefs, false)) {
    p.putUChar(kKeyBt, static_cast<uint8_t>(btChoice_));
    p.end();
  }
  char range[16];
  pc::btRangeText(btChoice_, range, sizeof(range));
  Serial.printf("[power] bluetooth power: %s -> %s (%s, saved)\n", pc::btLabel(before), pc::btLabel(btChoice_),
                range);
  const pc::TxLevels l = pc::btLevels(btChoice_);
  bt.setTxPower(l.min, l.max);  // (its line says when it applies)
}
