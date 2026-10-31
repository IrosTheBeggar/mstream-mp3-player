// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// When a setting changed on the loop task is written to flash (NVS): not
// under the finger. A write holds the loop for milliseconds (a sector erase
// for tens), and the other core's tasks that run from flash stand still
// while it lasts; a tap on a settings row cycles its value, and a listener
// taps it a few times in a row to reach the one they want. So the change
// is kept and written once the taps rest: quietMs after the last change,
// or latestMs after the first unwritten one when they keep coming. The
// times are millis() (they wrap). Portable, host-tested (test_save_later);
// app/NvsLater keeps the values and writes them.
class SaveLater {
public:
  struct Config {
    uint32_t quietMs = 1500;
    uint32_t latestMs = 6000;
  };
  // (No default argument of Config(): the Xtensa GCC wants the nested
  // struct's initializers before the class ends.)
  SaveLater() = default;
  explicit SaveLater(Config c) : cfg_(c) {}

  // Something to write (again).
  void changed(uint32_t nowMs) {
    if (!pending_) firstMs_ = nowMs;
    lastMs_ = nowMs;
    pending_ = true;
  }
  // A write is owed.
  bool pending() const { return pending_; }
  // Owed, and its wait is over: write now.
  bool due(uint32_t nowMs) const { return pending_ && static_cast<int32_t>(nowMs - dueAtMs()) >= 0; }
  // Written (by the loop when due, or by a flush before a restart).
  void written() { pending_ = false; }
  // When the owed write is due (pending() only): the quiet after the last
  // change, or the latest after the first, whichever comes first.
  uint32_t dueAtMs() const {
    const uint32_t quiet = lastMs_ + cfg_.quietMs;
    const uint32_t latest = firstMs_ + cfg_.latestMs;
    return static_cast<int32_t>(quiet - latest) < 0 ? quiet : latest;
  }

private:
  Config cfg_{};
  bool pending_ = false;
  uint32_t firstMs_ = 0;
  uint32_t lastMs_ = 0;
};
