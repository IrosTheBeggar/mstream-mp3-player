// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "HoldButton.h"

// The format's question's clock (ui/Ui::askFormat(); docs/HOST-STATUS.md,
// "@format"): its two steps, each a dialog with Cancel. Ask, "Format this
// card?", answered by a tap on Format...; Sure, "Erase the whole card?",
// answered only by holding its red Format HoldButton::kHoldMs (the
// Dialog's hold mode: DialogTouch). Cancel is the default: a step up
// kAskMs with no answer ends it (expired()), whatever is under way then, a
// hold too: the minute wins (pass()). A touch that lands in Sure's first
// kSettleMs goes nowhere (lands()): the finger that answered Ask, coming
// back (a double tap), so it neither starts a hold nor taps Cancel; the
// next touch counts. Sure's minute and settle run from the answer that
// opened it (answered()'s time: the tap's own, not the loop's).
// Portable: no clock of its own, the times are the caller's.
namespace formatask {

inline constexpr uint32_t kAskMs = 60000;
inline constexpr uint32_t kSettleMs = 600;

// Where each step's yes is (the dialogs' buttons, left 0 and right 1):
// Ask's Format... on the right, its Cancel on the left; Sure's hold button
// on the left, where Ask's Cancel was, its Cancel on the right.
inline constexpr int kAskFormatButton = 1;
inline constexpr int kSureFormatButton = 0;

enum class Step : uint8_t { Off, Ask, Sure };

// What a step's answer leads to.
enum class Answer : uint8_t {
  Declined,  // Cancel, the default (a button, a tab, another dialog, the minute)
  Second,    // Ask's Format...: Sure is up
  Format,    // Sure's hold button, held to its end: the card is formatted
};

class Question {
public:
  // The first step up at `ms` (asked again while it is up: it stays, its
  // minute as it was).
  void ask(uint32_t ms);
  // The second, at `ms`: its own minute, and its settle.
  void sure(uint32_t ms);
  // The step up answered with `button` at `ms` (-1: closed, by a tab,
  // another dialog or the minute): Ask's kAskFormatButton opens Sure from
  // `ms` (Second); Sure's kSureFormatButton is Format; anything else, or
  // no step up, Declined. Format and Declined end it.
  Answer answered(int button, uint32_t ms);
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

// One loop pass of the question (ui/Ui::loop(), after the pass's touch
// events), in this order: the minute first (`asking`: the question's
// dialog is up), Cancel over a hold under way too; then the dialog's hold
// (its hold mode): dropped when the touch that began it is no longer the
// dialog's (`touchIsDialogs` false: the dark, a banner, a Cancel took it),
// else on towards its end (`dialog.tick()`: the fill drawn as it grows),
// and kept to it, the hold button's answer. `D`: ui/Overlays' Dialog, or
// DialogTouch (the tests): holdMode(), holding(), dropHold(), tick(ms).
enum class Pass : uint8_t { None, Expired, Dropped, Confirmed };
template <class D>
Pass pass(bool asking, const Question& q, D& dialog, uint32_t ms, bool touchIsDialogs) {
  if (asking && q.expired(ms)) return Pass::Expired;
  if (!dialog.holdMode()) return Pass::None;
  if (dialog.holding() && !touchIsDialogs) {
    dialog.dropHold();
    return Pass::Dropped;
  }
  return dialog.tick(ms) == HoldButton::News::Confirmed ? Pass::Confirmed : Pass::None;
}

}  // namespace formatask
