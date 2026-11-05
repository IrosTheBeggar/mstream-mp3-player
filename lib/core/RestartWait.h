// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The card guard's restart, held until the queue's flush works (docs/METADATA.md 3.8). The same
// card back with a card job under way restarts the player (main.cpp's stepCardGuard()), the
// card jobs and the queue's own saves held from that pass (cardHeld). The guard flushes the queue
// first; with the card silent again that flush fails, and a restart 1.2 s later lost the edits
// since the last save (the device check of 2026-11-04). So the restart waits:
//
//   - The guard's flush worked: the restart kShowMs later, as ever (the toast first).
//   - It failed: tried again on the queue saver's own backoff (QueueSaver::lastWaitMs(): the
//     wait its failures built up, 10 s and then twice as long), so the card is asked no more
//     often than the saver would ask it; never past kMaxMs from the guard's decision, where a
//     last try is made. The first try that works, or that last one whatever it did: the restart
//     kShowMs later.
//   - No flush can work (writes off, another card, the queue carried as text through an update
//     step): the restart now, as ever.
//   - A restart decided once the guard's toast has gone (kToastMs) says so again first
//     (Do::Announce), kShowMs before it.
//
// Nothing here counts restarts: the breaker counts this one when the guard decides it
// (lib/core/CardBreaker.h), and the restart comes, flushed or not, within kMaxMs, a try's own
// time and kShowMs. Times are a millisecond clock that may wrap. Portable, host-tested
// (test_card_breaker).
class RestartWait {
public:
  static constexpr uint32_t kShowMs = 1200;   // the toast before the restart (restartAtMs's)
  static constexpr uint32_t kToastMs = 3000;  // how long that toast shows
  static constexpr uint32_t kMaxMs = 30000;   // the bound, from the guard's decision

  enum class Do : uint8_t {
    Nothing,
    Flush,     // the queue's flush now (QueueStore::flushNow()), then tried()
    Announce,  // the toast again ("Card back: restarting"): the restart kShowMs on
    Restart,   // now (restartNow()); once
  };

  // The guard chose to restart. `flushed`: its flush worked; `retryMs`: the saver's wait after it.
  void begin(uint32_t nowMs, bool flushed, uint32_t retryMs) {
    *this = RestartWait();
    active_ = true;
    sinceMs_ = nowMs;
    tries_ = 1;
    flushed_ = flushed;
    if (flushed) {
      decide(nowMs);
    } else {
      plan(nowMs, retryMs);
    }
  }

  // Each pass while active(). `canFlush` false: no flush can work now.
  Do step(uint32_t nowMs, bool canFlush) {
    if (!active_) return Do::Nothing;
    if (!decided_ && !canFlush) decide(nowMs);
    if (announce_) {
      announce_ = false;
      return Do::Announce;
    }
    if (decided_) {
      if (!due(nowMs, restartAtMs_)) return Do::Nothing;
      active_ = false;
      return Do::Restart;
    }
    return due(nowMs, nextTryMs_) ? Do::Flush : Do::Nothing;
  }

  // Do::Flush's result, and the saver's wait after it.
  void tried(uint32_t nowMs, bool flushed, uint32_t retryMs) {
    if (!active_ || decided_) return;
    ++tries_;
    flushed_ = flushed;
    if (flushed || nowMs - sinceMs_ >= kMaxMs) {
      decide(nowMs);
    } else {
      plan(nowMs, retryMs);
    }
  }

  bool active() const { return active_; }
  // The restart is set (its time known): the queue saved, the bound reached or no flush possible.
  bool decided() const { return active_ && decided_; }
  // The last flush worked: the card has the queue as it is.
  bool flushed() const { return flushed_; }
  // Flushes made, the guard's own included.
  uint32_t tries() const { return tries_; }
  // The next try's wait from `nowMs` (0: due, or none to come).
  uint32_t nextTryInMs(uint32_t nowMs) const {
    return active_ && !decided_ && !due(nowMs, nextTryMs_) ? nextTryMs_ - nowMs : 0;
  }
  uint32_t sinceMs() const { return sinceMs_; }

private:
  static bool due(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }
  void plan(uint32_t nowMs, uint32_t retryMs) {
    // (Never past the bound: a last try there.)
    const uint32_t left = kMaxMs - (nowMs - sinceMs_);
    nextTryMs_ = nowMs + (retryMs < left ? retryMs : left);
  }
  void decide(uint32_t nowMs) {
    decided_ = true;
    if (nowMs - sinceMs_ < kToastMs) {
      // The guard's toast still up: kShowMs after it came, as ever.
      const uint32_t at = sinceMs_ + kShowMs;
      restartAtMs_ = due(nowMs, at) ? nowMs : at;
    } else {
      announce_ = true;
      restartAtMs_ = nowMs + kShowMs;
    }
  }

  bool active_ = false;
  bool decided_ = false;
  bool announce_ = false;
  bool flushed_ = false;
  uint32_t sinceMs_ = 0;
  uint32_t nextTryMs_ = 0;
  uint32_t restartAtMs_ = 0;
  uint32_t tries_ = 0;
};
