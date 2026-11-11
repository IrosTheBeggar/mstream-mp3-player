// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "PlayLog.h"
#include "PlaybackController.h"
#include "app/Library.h"
#include "app/ListeningStats.h"

class AutoDj;

// The play log on the card, /.player/plays.jnl (docs/METADATA.md 2.20;
// lib/core PlayLog, which holds every rule): the firmware's side.
//   - The player's word (PlaybackController::setPlays()) goes to the
//     Tracker: a play's file at its start (its path as the library has it,
//     its path hash, the root's generation, its source: Auto DJ's mark, the
//     queue shuffled, else manual), its time played and its pauses from the
//     backend's position each pass, its outcome at its end.
//   - The Journal writes on the loop task, never on the audio's tasks: a
//     piece of the boot's scan, or one play's slot a pass (the Outbox),
//     when the WriteGate lets it (out of the gapless window, a start's
//     refill, a seek, a low ring, an underrun's wake, a moving list) and
//     the card is one to ask (METADATA.md 3.8): not held for the guard's
//     restart, not out of the slot (the card hold's: none answering, or
//     another card), not refusing every write. Then the plays wait, 32 at
//     most, for the remount or the end's flush(). A slot is the file's
//     size and FAT time (f_stat, at the write), the board's clock for its
//     start (boardclock::at(): a clock set during the play still dates it;
//     0 while unset) and the clock's source, the seq in NVS first ("plays",
//     nvslayout's schema 6), then one append and its f_sync.
//   - A play the card fails (the f_stat, the append) isn't lost at once:
//     it stays first in line and is tried again 5 s later, or when the card
//     mounts again, three tries in all, the journal's same slot each time
//     (its seq, its bytes: one that had landed isn't written twice);
//     nothing behind it is written meanwhile. Then it is lost, and the log
//     says so. A file that isn't on the card any more, NVS refusing the
//     seq and a blocked log lose the play at once, as before.
//   - flush() before every orderly power-off and restart (main.cpp's
//     endInOrder(), after the settings the taps queued: the idle power-off,
//     the CPU speed's restart, restartNow(): the card guard's, "Try again"'s,
//     the format's, the console's): the open play ended (stopped), every
//     play waiting tried once, in order, until the card fails one or
//     kFlushBudgetMs is up: the end doesn't wait on a card gone bad.
//   - deviceId: the eFuse MAC (esp_efuse_mac_get_default()); a new epoch:
//     the hardware RNG (esp_random()); ACKS: the root the boot read
//     (Library::root()).
//   - The listening stats (app/ListeningStats, docs/LISTENING-STATS.md):
//     stats.bin read before the journal's begin(), the journal's Folds (the
//     boot's fold and save before its trim, a rotation's save first), the
//     live fold of each play written, the ending flag around the end's
//     flush, and the console's H (Hs runs at a pass the gate lets through).
// Only on the card with its records (Library::records()); never on the
// flash. Loop task only.
class PlayHistory final : private playlog::Tracker::Env, private playlog::Outbox::Env {
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
  // (`gate`: this pass's audio and list) and the card isn't held (`cardHeld`:
  // the card guard's restart comes), out of the slot or refusing writes
  // (the guard's own word), a piece of the boot's scan or one slot (not
  // while a play the card failed waits for its next try).
  struct Look {
    PlayState state = PlayState::Stopped;
    uint32_t positionMs = 0;
    bool positionKnown = true;
    uint32_t durationMs = 0;
    // A restart asked (the update step's safe point says no to it too: the
    // stats' compaction keeps out of it the same way).
    bool restartAsked = false;
  };
  void loop(const Look& look, const playlog::WriteGate::In& gate, bool cardHeld);

  // Before an orderly power-off or restart (`why`: its log's words): the
  // open play ended (Stopped), the boot's scan finished, every play
  // waiting tried once, whatever the gate says, until the card fails one
  // or kFlushBudgetMs is up (the rest NOT written, and said so). Not to
  // another card in the slot, nor one that takes no more writes (FatFs
  // refuses them all: those plays are lost), nor with none in it: a card
  // that was out is asked once (put back, it is a remount: remounted()),
  // then left alone.
  void flush(const char* why);
  // The same card back (the card guard's remount): the cut-rename rule
  // again before the next append, and before this boot's first slot the
  // boot's scan again from its start (Journal::remounted()); a play the
  // card failed is tried again at once.
  void remounted();
  // The console's H (app/ListeningStats): the text after the H.
  void statsCommand(const char* arg) { stats_.command(arg); }

  // The boot's scan: slots a pass (four reads: about 10 KB).
  static constexpr uint32_t kScanSlotsPerPass = 4 * playlog::kReadBytes / playlog::kSlot;
  // The flush's deadline. A healthy card's worst case is well inside it
  // (ESTIMATED from the host model's card commands, 3.10's costs, at the
  // device's 15-20 ms a slot: all 32 plays waiting 0.5-0.7 s; a rotation
  // among them, plays.1.jnl's 524 KB read for its count, about 1 s more;
  // the boot's scan cut short by a restart, the log's 1 MB at its fullest,
  // 1-2 s with nothing much waiting). A card past it is one going bad: each
  // call it fails is the SD driver's second or two, and the restart or the
  // power-off comes after this and the call under way, not after every
  // play has had its turn.
  static constexpr uint32_t kFlushBudgetMs = 3000;

private:
  // playlog::Tracker::Env, playlog::Outbox::Env
  uint64_t monoMs() override;
  bool file(uint32_t key, uint32_t track, playlog::PlayFile* out) override;
  playlog::Outbox::Env::File stat(const char* path, uint32_t* size, uint32_t* fatTime) override;
  uint64_t startedAt(uint64_t startMono, uint8_t* clock) override;
  // A try's line.
  void tried(const playlog::Outbox::Tried& t, const playlog::Played& p) override;
  // A play's slot written: the listening stats' live fold.
  void written(const playlog::Slot& s, const playlog::Played& p) override;
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
  playlog::Outbox* outbox_ = nullptr;
  playlog::WriteGate gate_;
  int64_t tryT0_ = 0;  // when the try under way began (esp_timer's us: its line's ms)
  ListeningStats stats_;
  int64_t scanUs_ = 0;  // the boot's scan's pieces on the loop
};
