// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "PcmRing.h"
#include "RateConverter.h"

// The decode side of the ring: what the decoder (or a built-in track) hands
// over, converted to 44.1 kHz (RateConverter) and staged, then pushed into
// the PcmRing. The ring always holds 44.1 kHz, whatever the track's rate, so
// an output switch mid-track can't change the speed (docs/RESAMPLER.md,
// section 5). Portable, host-tested; the firmware's RingOutput wraps it as
// an ESP8266Audio AudioOutput.
//
// It never blocks. The rule that keeps the generators' "ring full: return
// false, offer the same sample again" contract: a source frame is taken
// only when the stage has room for everything it can make (maxOut()), after
// pushing the stage into the ring if need be; otherwise it is refused whole
// and nothing is lost or duplicated. The per-pass budget caps both sides:
// source frames, so a pass's decode work is as before, and ring frames, so
// a low rate's pass doesn't convert 5.5 times as much (8 kHz makes 441/80
// ring frames per source frame) above the UI loop.
//
// Counters: made() is ring frames (44.1 kHz) since reset(), the units of
// positions, durations and the decode load. The decoders' own seeks stay in
// their source units.
//
// One task (the decode task). ~2.6 KB, all in the object: keep it in
// internal RAM (the converter reads its histories 96 times per output).
class RingFeed {
public:
  static constexpr uint32_t kStageFrames = 256;

  explicit RingFeed(PcmRing& ring) : ring_(ring) {}

  // A new stream: nothing staged, the converter's history zeroed, no rate
  // yet. Every start, stop, skip and seek, right after the ring's
  // discardAll(), so nothing of the last track can come out after it.
  // `cpuMhz`: the speed the CPU was set to at boot (88.2/96 kHz need 240).
  // `hiRes`: whether 88.2/96 kHz play at all (RateConverter::kHiResOn; the
  // benches pass true).
  void reset(uint32_t cpuMhz, bool hiRes = RateConverter::kHiResOn);

  // ---- the generator's side (AudioOutput) ----
  // The stream's rate. <= 0 is ignored (FLAC before its header); the same
  // rate again changes nothing. False: refused (rejected(), refusal()):
  // nothing more is taken and the caller fails the track.
  bool setRate(int hz);
  // 1: mono (the left channel, copied). Never resets anything.
  void setChannels(int channels) { conv_.setMono(channels == 1); }
  // A pass: the generator may hand over up to `frames` source frames, and
  // none once the pass has made `frames` ring frames (the last one taken may
  // add up to maxOut() - 1 more). Then its loop() returns.
  void setBudget(uint32_t frames) {
    budget_ = frames;
    ringBudget_ = frames;
  }
  uint32_t budgetLeft() const { return budget_; }
  // One source frame; false: not taken (refused rate, budget spent, or the
  // ring full), the generator keeps it and offers it again.
  bool consume(const int16_t sample[2]);

  // ---- a block (the built-in tracks, the bench) ----
  // Takes as many of `frames` source frames as fit (no budget), making at
  // most `maxMade` ring frames; returns how many. The caller keeps the rest
  // and offers them again.
  uint32_t write(const int16_t* frames, uint32_t n, uint32_t maxMade = UINT32_MAX);

  // Pushes staged frames into the ring; true when nothing is left staged.
  bool commit();
  // The end of the stream: pushes the converter's tail (its last K/2
  // frames' worth) through the same rule. True once all of it is in the
  // ring; false: the ring is full, call again later.
  bool finish();

  // Ring space a pass of `srcFrames` source frames may need, with what is
  // staged (before the rate is known: at the largest ratio, 8 kHz's).
  uint32_t roomFor(uint32_t srcFrames) const;

  // The bench: committed frames are dropped instead of written.
  void setDiscard(bool discard) { discard_ = discard; }

  int rate() const { return rate_; }  // the source's, 0 until known
  bool rejected() const { return conv_.refused(); }
  // Why the rate was refused ("isn't supported (...)"), "" when it wasn't.
  const char* refusal() const { return conv_.refused() ? conv_.currentPlan().reason : ""; }
  RateConverter::Refusal refusalKind() const {
    return conv_.refused() ? conv_.currentPlan().refusal : RateConverter::Refusal::None;
  }
  // Ring frames made since reset() (staged or in the ring).
  uint64_t made() const { return made_; }
  const RateConverter& converter() const { return conv_; }

private:
  // Room in the stage for the converter's next push(), pushing the stage
  // into the ring first if it hasn't.
  bool room();

  PcmRing& ring_;
  RateConverter conv_;
  int16_t stage_[kStageFrames * 2];
  uint32_t staged_ = 0;
  uint32_t budget_ = 0;
  uint32_t ringBudget_ = 0;
  uint32_t cpuMhz_ = 0;
  bool hiRes_ = RateConverter::kHiResOn;
  int rate_ = 0;
  uint64_t made_ = 0;
  bool discard_ = false;
};
