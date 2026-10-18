// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "PlayLog.h"
#include "PlaybackController.h"
#include "app/Library.h"

class AutoDj;

// The play log on the card, /.player/plays.jnl (docs/METADATA.md 2.20;
// lib/core PlayLog, which holds every rule): the firmware's side.
//   - The player's word (PlaybackController::setPlays()) goes to the
//     Tracker: a play's file at its start (its path as the library has it,
//     its path hash, the root's generation, its source: Auto DJ's mark, the
//     queue shuffled, else manual), its time played and its pauses from the
//     backend's position each pass, its outcome at its end.
//   - The Journal writes on the loop task, never on the audio's tasks: a
//     piece of the boot's scan, or one play's slot a pass, when the
//     WriteGate lets it (out of the gapless window, a start's refill, a
//     seek, a low ring, an underrun's wake). A slot is the file's size and
//     FAT time (f_stat, at the write), the board's clock for its start
//     (boardclock::at(): a clock set during the play still dates it; 0
//     while unset) and the clock's source, the seq in NVS first ("plays",
//     nvslayout's schema 5), then one append and its f_sync.
//   - flush() before every orderly power-off and restart (main.cpp's:
//     the idle power-off, the CPU speed's restart, restartNow(): the card
//     guard's, "Try again"'s, the console's): the open play ended
//     (stopped), every play waiting written at once.
//   - deviceId: the eFuse MAC (esp_efuse_mac_get_default()); a new epoch:
//     the hardware RNG (esp_random()); ACKS: the root the boot read
//     (Library::root()).
// Only on the card with its records (Library::records()); never on the
// flash. Loop task only.
class PlayHistory final : private playlog::Tracker::Env {
public:
  PlayHistory(Library& library, PlaybackController& player) : library_(library), player_(player) {}

  // setup(), after Library::begin() (the card's records open, the root
  // read) and Auto DJ's (its marks name the source; nullptr: none): NVS's
  // ids, the cut-rename rule, the boot's scan begun, the player's word
  // taken from here on. Its line says why not, when not.
  void begin(AutoDj* autoDj);
  bool active() const { return active_; }

  // Every loop pass, after the player's update() (a join is taken first):
  // the backend's position for the open play; then, when the gate lets it
  // (`gate`: this pass's audio) and the card isn't held (`cardHeld`: the
  // card guard's restart comes), a piece of the boot's scan or one slot.
  struct Look {
    PlayState state = PlayState::Stopped;
    uint32_t positionMs = 0;
    bool positionKnown = true;
    uint32_t durationMs = 0;
  };
  void loop(const Look& look, const playlog::WriteGate::In& gate, bool cardHeld);

  // Before an orderly power-off or restart (`why`: its log's words): the
  // open play ended (Stopped), the boot's scan finished, every play
  // waiting written now, whatever the gate says. Not to another card in
  // the slot (FatFs refuses every write to it: those plays are lost).
  void flush(const char* why);
  // The same card back (the card guard's remount, nothing under way): the
  // cut-rename rule again before the next append.
  void remounted();

  // The boot's scan: slots a pass (four reads: about 10 KB).
  static constexpr uint32_t kScanSlotsPerPass = 4 * playlog::kReadBytes / playlog::kSlot;

private:
  // playlog::Tracker::Env
  uint64_t monoMs() override;
  bool file(uint32_t key, uint32_t track, playlog::PlayFile* out) override;
  // The oldest play waiting: its slot written (or lost), and its line.
  void writeOne();
  static void onOp(void* ctx, playlog::Journal::Op op, const char* a, const char* b, uint64_t n);

  Library& library_;
  PlaybackController& player_;
  AutoDj* autoDj_ = nullptr;
  bool active_ = false;
  bool booted_ = false;  // the boot's scan's line is out
  uint32_t scanPasses_ = 0;
  uint8_t* buf_ = nullptr;
  playlog::Tracker* tracker_ = nullptr;
  playlog::Journal* journal_ = nullptr;
  playlog::WriteGate gate_;
  uint32_t written_ = 0, lost_ = 0;
};
