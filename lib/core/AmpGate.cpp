// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AmpGate.h"

AmpGate::Do AmpGate::quiet(uint32_t nowMs, bool running, bool drained) {
  switch (ask_) {
    case Ask::On:
      ask_ = Ask::None;
      held_ = true;
      if (running) return Do::Nothing;
      why_ = Why::Asked;
      return Do::Start;
    case Ask::Off:
      if (!drained) return Do::Nothing;  // still playing out what was queued
      ask_ = Ask::None;
      held_ = false;
      known_ = false;
      if (!running) return Do::Nothing;
      why_ = Why::Asked;
      return Do::Stop;
    case Ask::None:
      break;
  }
  if (!running) {
    known_ = false;  // (a quiet spell counts from audio, or from the first pass it is on)
    return Do::Nothing;
  }
  if (!known_) {
    // On without audio queued since it was last off (Pa1 and then Pa0 is
    // an Off; this is only a safety net): its quiet spell starts now.
    lastAudioMs_ = nowMs;
    known_ = true;
  }
  if (held_ || !drained) return Do::Nothing;
  if (nowMs - lastAudioMs_ < kQuietMs) return Do::Nothing;
  known_ = false;
  why_ = Why::Quiet;
  return Do::Stop;
}
