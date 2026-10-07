// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The time rule of a decode pass (OpusGenerator::loop(); docs/OPUS.md
// section 8.2, gate G6): a pass takes another step only while the time it
// has used so far plus what the last step took fits the budget, so a pass
// ends about a budget in unless one step is longer than that by itself.
// The first step of every pass runs whatever the last step took. That is
// the rule's one subtlety: a step over the budget (an SD stall; a page turn
// at 160 MHz; M2's resync after a damaged page, one read of tens of KB
// until docs/OPUS.md 8.11 made it steps) can't be stopped from
// inside, and it ends the pass it is in; were it allowed to end the next
// pass too, before that pass's first step, no step would ever run again
// (the last step's time is only replaced by a step), and the track would
// stall for good with the loop still saying it has more. The times are the
// caller's microsecond clock (esp_timer_get_time() on the device: any
// origin). Portable, host-tested (test_pass_clock).
class PassClock {
public:
  explicit PassClock(uint32_t budgetUs) : budget_(budgetUs) {}

  // A new track: no last step, the longest step forgotten.
  void reset() {
    lastStepUs_ = 0;
    maxStepUs_ = 0;
    steps_ = 0;
  }
  // A pass begins at `nowUs`.
  void begin(int64_t nowUs) {
    t0_ = nowUs;
    steps_ = 0;
  }
  // Whether the pass may take another step at `nowUs`: its first always;
  // after that only while the pass's time so far plus the last step's
  // stays within the budget (exactly the budget fits).
  bool fits(int64_t nowUs) const {
    if (steps_ == 0) return true;
    return static_cast<uint64_t>(nowUs - t0_) + lastStepUs_ <= budget_;
  }
  // A step ran from `fromUs` to `nowUs`.
  void stepped(int64_t fromUs, int64_t nowUs) {
    lastStepUs_ = static_cast<uint32_t>(nowUs - fromUs);
    if (lastStepUs_ > maxStepUs_) maxStepUs_ = lastStepUs_;
    ++steps_;
  }

  uint32_t budgetUs() const { return budget_; }
  uint32_t lastStepUs() const { return lastStepUs_; }  // what the last step took (0: none since reset())
  uint32_t maxStepUs() const { return maxStepUs_; }    // the longest step since reset()
  uint32_t steps() const { return steps_; }            // this pass's steps so far

private:
  uint32_t budget_;
  int64_t t0_ = 0;
  uint32_t lastStepUs_ = 0;
  uint32_t maxStepUs_ = 0;
  uint32_t steps_ = 0;
};
