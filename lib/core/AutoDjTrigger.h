// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "PlaybackController.h"

// When Auto DJ adds a track (the engine design's 2.3, the device form of
// the mobile app's shouldTopUpAutoDJ): a pure rule, read every loop pass
// in Auto DJ's step (after the card's guard, before the card's step), so it
// is level based: whatever made the queue's last entry current (a natural
// end, a skip, a remove of what followed, Auto DJ switched on) is seen the
// same way. Exactly one track is added when all of these hold:
//   1. Auto DJ is on, and its engine ready: joined, or picking at random
//      (no index); not while the index file reopens or the join runs.
//   2. The current entry is the last in play order (QueueModel::upNext()
//      0; shuffled, the last of the shuffled order: an append ranks after
//      the highest rank and is never shuffled in).
//   3. A playback session: Playing, or Waiting for the headphones; or a
//      finished queue (PlaybackController::endedAtEnd(): stopped on its
//      last entry at its end), whose pick is cued, paused at 0:00, and
//      nothing starts (2.3.1). Never a paused or a just-restored queue.
//   4. Not behind the library update's fence.
//   5. Not while the card is held (its guard), or a restart is set.
//   6. Repeat isn't One (it loops the track: a hold).
//   7. The sleep timer isn't End of queue (the queue's end is where it
//      stops: a hold; once its choice is over, a finished queue's pick is
//      cued).
//   8. Not paused by FailurePause (kFailedPicks of its picks in a row
//      failed to play: a hold).
// The holds (6-8) are what the "..." row shows: Waiting (Repeat One, the
// sleep timer) or Paused (failures).
//
// The update step's fence (LibraryUpdate::In::autoDjOwed): a pick is owed
// when it is due by 1-3 and 6-8 but the engine joins or reopens; no fence
// goes up then (the pick lands first). A hold never owes one, so a held
// Auto DJ never blocks an update.
//
// Portable, host-tested (test_autodj_trigger; with the player,
// test_playback).
namespace autodj {

// Why nothing is added this pass: the first of the conditions above that
// fails, in their order (the console's D). Due: add one now.
enum class Wait : uint8_t {
  Due,
  Off,         // 1: Auto DJ is off
  Empty,       // 2: the queue is empty (nothing to follow)
  UpNext,      // 2: entries follow the current one
  Idle,        // 3: paused, stopped or just restored, and not a finished queue
  Fenced,      // 4
  Card,        // 5: the card is held, or a restart is set
  Joining,     // 1, 5: the index reopens or the join runs (Out::owed, when the rest holds)
  RepeatOne,   // 6
  SleepTimer,  // 7
  Failures,    // 8
};
const char* waitName(Wait w);

// What the "..." row shows besides On and Off (whatever the queue does).
enum class Hold : uint8_t {
  None,
  RepeatOne,   // "Waiting"
  SleepTimer,  // "Waiting"
  Failures,    // "Paused"
};

struct TriggerIn {
  bool on = false;       // Auto DJ is on
  bool joining = false;  // the index file reopens, or the join runs
  // The queue and the player (fromPlayer() fills these).
  uint32_t queueSize = 0;
  uint32_t upNext = 0;                  // QueueModel::upNext()
  PlayState play = PlayState::Stopped;  // PlaybackController::state()
  bool finished = false;                // PlaybackController::endedAtEnd()
  bool fenced = false;                  // the library update's fence (PlaybackController::fenced())
  bool repeatOne = false;               // PlaybackController::repeat() is One
  // The rest of the firmware.
  bool cardHeld = false;         // the card's guard holds it
  bool restartSet = false;       // a restart is set (a CPU speed's)
  bool sleepEndOfQueue = false;  // SleepTimer::choice() is EndOfQueue
  bool failurePaused = false;    // FailurePause::paused()
};

struct TriggerOut {
  bool pick = false;  // pick one now and add it: PlaybackController::addFromAutoDj(track, cue)
  bool cue = false;   // ... cued: the queue finished (2.3.1)
  bool owed = false;  // LibraryUpdate::In::autoDjOwed
  Wait wait = Wait::Off;
  Hold hold = Hold::None;
};

TriggerOut decide(const TriggerIn& in);
// `in` with its queue and player fields from `player` (the rest as given).
TriggerIn fromPlayer(const PlaybackController& player, TriggerIn in = TriggerIn());

// The failure pause (2.3.2), the device form of the mobile app's guard on
// a failure walk: kFailedPicks of Auto DJ's picks in a row that fail to
// play pause it (a toast). A track of the listener's that fails neither
// counts nor ends the row. It clears when any track starts playing (the
// listener's too), or when Auto DJ is switched off and on (clear()).
class FailurePause {
public:
  static constexpr uint8_t kFailedPicks = 3;
  // A track failed to play (PlaybackController::lastFailure() counted one);
  // `byAutoDj`: one of Auto DJ's picks. True: this failure paused Auto DJ
  // (the toast, once).
  bool failed(bool byAutoDj);
  // A track started playing.
  void played() { n_ = 0; }
  void clear() { n_ = 0; }
  bool paused() const { return n_ >= kFailedPicks; }
  uint8_t inARow() const { return n_; }

private:
  uint8_t n_ = 0;
};

}  // namespace autodj
