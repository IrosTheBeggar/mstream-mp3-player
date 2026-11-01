// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ScrubPacer.h"

namespace {
// `a` at or after `b`, as millis() goes (wrap-safe within 24 days).
bool reached(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) >= 0; }
}  // namespace

void ScrubPacer::begin(uint32_t nowMs) {
  owed_ = false;
  changedMs_ = nowMs;
  drawnMs_ = nowMs - config_.cadenceMs;  // the first position: at once
}

void ScrubPacer::targetChanged(uint32_t nowMs) {
  owed_ = true;
  changedMs_ = nowMs;
}

void ScrubPacer::bandDrawn(uint32_t endMs) {
  owed_ = false;
  drawnMs_ = endMs;
}

bool ScrubPacer::bandDue(uint32_t nowMs) const {
  if (!owed_) return false;
  return reached(nowMs, changedMs_ + config_.settleMs) || reached(nowMs, drawnMs_ + config_.cadenceMs);
}
