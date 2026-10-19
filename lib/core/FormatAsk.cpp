// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "FormatAsk.h"

namespace formatask {

void Question::ask(uint32_t ms) {
  if (step_ != Step::Off) return;
  step_ = Step::Ask;
  openedMs_ = ms;
}

void Question::sure(uint32_t ms) {
  step_ = Step::Sure;
  openedMs_ = ms;
}

Answer Question::answered(int button, uint32_t ms) {
  const Step was = step_;
  step_ = Step::Off;
  if (was == Step::Ask && button == kAskFormatButton) {
    sure(ms);
    return Answer::Second;
  }
  return was == Step::Sure && button == kSureFormatButton ? Answer::Format : Answer::Declined;
}

bool Question::lands(uint32_t ms) const {
  // (The time since it opened as a signed span: an event stamped before the
  // step's own time is in its settle, never a wrap past it.)
  return step_ != Step::Sure || static_cast<int32_t>(ms - openedMs_) >= static_cast<int32_t>(kSettleMs);
}

bool Question::expired(uint32_t ms) const {
  return step_ != Step::Off && static_cast<int32_t>(ms - openedMs_) >= static_cast<int32_t>(kAskMs);
}

}  // namespace formatask
