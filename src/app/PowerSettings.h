#pragma once
#include <Arduino.h>

#include "PowerChoices.h"

class BtSink;

// The Output tab's "CPU speed" and "Bluetooth power" on the Core2
// (docs/ENERGY.md items 6 and 7): the choices and texts are PowerChoices'
// (host-tested); this keeps them in NVS "power" and applies them.
//
//   - "cpu_mhz": 160 or 240 (absent: powerchoice::kDefaultCpuMhz), shared
//     with the console's Pcb. applyBootClock() sets it first thing in
//     setup(), before Bluetooth starts: 240 <-> 160 retunes the PLL the
//     radio runs from, so a change is saved here and main.cpp restarts
//     (the idle power-off's orderly way: the queue flushed, the headphones
//     let go). "boot_cpu" notes that restart for the next boot's toast
//     ("CPU speed: 160 MHz").
//   - "bt_tx": the Bluetooth power choice (absent: Normal), handed to
//     BtSink before the stack starts and again at once on a change.
//
// Loop task only (applyBootClock(): setup(), before M5.begin()).
class PowerSettings {
public:
  // setup(), first: the saved speed (or the default), before Bluetooth.
  static void applyBootClock();
  // setup(), once Serial is up: what applyBootClock() did, and the restart
  // note (read and removed: its toast, once).
  void begin();
  // setup(), before audio.begin(): the Bluetooth power, for the stack's start.
  void beginBluetooth(BtSink& bt);

  // ---- CPU speed ----
  // The speed saved (what the next boot runs), and the clock set at boot
  // (on its PLL until the next boot: the console's Pc80 doesn't change it).
  uint16_t cpuSaved() const { return cpuSaved_; }
  static uint16_t cpuBootMhz();
  // Saves `mhz` (160 or 240: the Output row, Pcb160 / Pcb240), from the
  // next boot. False: not a speed it takes, or NVS failed.
  bool saveCpu(uint16_t mhz);
  // Pcb0: back to the firmware's default (the key removed).
  void clearCpu();
  // The key as stored: 0 when absent (the default).
  static uint16_t cpuStored();
  // A restart for the new speed: its note for the next boot's toast.
  void noteRestart(uint16_t mhz);
  // The note found at boot, once: its toast's text. False: none.
  bool takeBootNote(char* buf, size_t size);
  // The restart itself: Serial flushed, the panel's SPI lock held (the card
  // shares the bus), esp_restart(). Doesn't return.
  void restart();

  // ---- Bluetooth power ----
  int btChoice() const { return btChoice_; }
  // Saved, and applied at once (from the next page, scan or connection).
  void setBt(int choice, BtSink& bt);
  // setBt()s so far (free-running): the console's Pt test is replaced by
  // the next one (PowerLab's lines then report the row's levels again).
  uint32_t btChanges() const { return btChanges_; }
  // Every loop pass: which choice the link that is up was made with.
  void update(bool linked) { linkLevel_.update(linked, btChoice_); }
  // Linked with another choice's levels: the new one applies from the next
  // connection (the row says so).
  bool btPending() const { return linkLevel_.pending(btChoice_); }

private:
  uint16_t cpuSaved_ = powerchoice::kDefaultCpuMhz;
  int btChoice_ = powerchoice::kDefaultBt;
  uint16_t bootNoteMhz_ = 0;  // the restart's note found at boot (0: none, or taken)
  uint32_t btChanges_ = 0;
  powerchoice::BtLinkLevel linkLevel_;
};
