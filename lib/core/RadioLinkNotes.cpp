// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RadioLinkNotes.h"

RadioLinkNotes::Opened RadioLinkNotes::opened(uint64_t bda, bool ok, bool paged, uint32_t nowMs) {
  if (!ok) {
    // One radio link a device: a failure for the one noted up isn't its
    // end (that comes as its close), so its set-up goes on.
    return closes(bda) ? Opened::Kept : Opened::Failed;
  }
  bda_ = bda;
  up_ = true;
  theirs_ = !paged;
  linked_ = false;
  upAtMs_ = nowMs;
  recount(true);
  return paged ? Opened::Answered : Opened::Theirs;
}

bool RadioLinkNotes::closed(uint64_t bda) {
  if (!closes(bda)) return false;
  up_ = theirs_ = linked_ = false;
  recount(false);
  return true;
}

void RadioLinkNotes::linkUp(uint64_t bda) {
  bda_ = bda;
  up_ = true;
  linked_ = true;
  link_ = true;
  recount(false);
}

void RadioLinkNotes::linkDown() {
  link_ = false;
  recount(false);
}

void RadioLinkNotes::paged(uint64_t bda) {
  if (settingUp() && bda == bda_) recount(true);
}

void RadioLinkNotes::forget() {
  if (link_) return;
  up_ = theirs_ = linked_ = false;
  bda_ = 0;
  recount(false);
}

void RadioLinkNotes::recount(bool again) {
  const bool on = settingUp();
  const bool was = (count_ & 1u) != 0;
  if (on != was) {
    ++count_;
  } else if (on && again) {
    count_ += 2;
  }
}
