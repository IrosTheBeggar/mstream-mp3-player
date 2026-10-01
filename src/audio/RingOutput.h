// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioOutput.h>

#include <cstdint>

#include "PcmRing.h"
#include "audio/AudioShared.h"

// ESP8266Audio output that stages decoded frames and pushes them into the
// PcmRing. It never blocks: when the ring is full, or the decode task's
// per-pass budget is spent, ConsumeSample() returns false, the generator keeps
// that sample and hands it over again on its next loop().
class RingOutput : public AudioOutput {
public:
  RingOutput(PcmRing& ring, AudioShared& shared) : ring_(ring), shared_(shared) {}

  // Before each track. `only44k`: the active output takes nothing else
  // (Bluetooth), so any other rate is refused and the track fails.
  void reset(bool only44k) {
    staged_ = 0;
    budget_ = 0;
    rate_ = 0;
    channels_ = 2;
    only44k_ = only44k;
    rejected_ = false;
  }
  // How many frames the generator may hand over before loop() returns.
  void setBudget(uint32_t frames) { budget_ = frames; }
  uint32_t budgetLeft() const { return budget_; }
  // Pushes staged frames into the ring; true when nothing is left staged.
  bool commit() {
    if (staged_ == 0) return true;
    const uint32_t n = ring_.write(stage_, staged_);
    for (uint32_t i = n; i < staged_; ++i) {  // keep what didn't fit, in order
      stage_[2 * (i - n)] = stage_[2 * i];
      stage_[2 * (i - n) + 1] = stage_[2 * i + 1];
    }
    staged_ -= n;
    return staged_ == 0;
  }
  int rate() const { return rate_; }
  bool rateRejected() const { return rejected_; }

  // ---- AudioOutput ----
  bool begin() override { return true; }
  // The base class keeps the rate in a uint16_t, which 88.2/96 kHz overflow.
  bool SetRate(int hz) override {
    if (hz <= 0) return true;  // FLAC says 0 before it has read the stream header
    rate_ = hz;
    rejected_ = only44k_ && hz != 44100;
    // Published before this track's first frame reaches the ring (see AudioShared).
    if (!rejected_) shared_.rate = hz;
    return !rejected_;
  }
  bool SetChannels(int channels) override {
    channels_ = channels;
    return true;
  }
  bool ConsumeSample(int16_t sample[2]) override {
    if (rejected_ || budget_ == 0) return false;
    if (staged_ == kStageFrames && !commit()) return false;
    stage_[2 * staged_] = sample[0];
    stage_[2 * staged_ + 1] = channels_ == 1 ? sample[0] : sample[1];
    ++staged_;
    --budget_;
    return true;
  }
  void flush() override { commit(); }
  // generator->stop() calls this: keep what's buffered, the outputs play it out.
  bool stop() override { return true; }

private:
  static constexpr uint32_t kStageFrames = 256;

  PcmRing& ring_;
  AudioShared& shared_;
  int16_t stage_[kStageFrames * 2];
  uint32_t staged_ = 0;
  uint32_t budget_ = 0;
  int rate_ = 0;
  int channels_ = 2;
  bool only44k_ = false;
  bool rejected_ = false;
};
