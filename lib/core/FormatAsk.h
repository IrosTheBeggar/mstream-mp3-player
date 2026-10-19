// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The format's question's clock (ui/Ui::askFormat(); docs/HOST-STATUS.md,
// "@format"): its two steps, each a dialog with Cancel. Ask, "Format this
// card?", answered by a tap on Format...; Sure, "Erase the whole card?",
// answered only by holding its red Format HoldButton::kHoldMs (the Dialog's
// hold mode). Cancel is the default: a step up kAskMs with no answer ends
// it (expired()), whatever is under way then, a hold too: the minute
// wins. A touch that lands in Sure's first kSettleMs goes nowhere
// (lands()): the finger that answered Ask, coming back (a double tap), so
// it neither starts a hold nor taps Cancel; the next touch counts.
// Portable: no clock of its own, the times are the caller's.
namespace formatask {

inline constexpr uint32_t kAskMs = 60000;
inline constexpr uint32_t kSettleMs = 600;

enum class Step : uint8_t { Off, Ask, Sure };

class Question {
public:
  // The first step up at `ms` (asked again while it is up: it stays, its
  // minute as it was).
  void ask(uint32_t ms);
  // The second, at `ms`: its own minute, and its settle.
  void sure(uint32_t ms);
  // Answered or gone.
  void end() { step_ = Step::Off; }
  Step step() const { return step_; }
  bool up() const { return step_ != Step::Off; }
  // A touch landing at `ms` counts (false: Sure's settle, it goes nowhere).
  bool lands(uint32_t ms) const;
  // The step has been up kAskMs at `ms`: its Cancel.
  bool expired(uint32_t ms) const;
  uint32_t openedMs() const { return openedMs_; }

private:
  Step step_ = Step::Off;
  uint32_t openedMs_ = 0;
};

}  // namespace formatask
