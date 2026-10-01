// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioOutput.h>

#include <cstdint>

#include "PcmRing.h"
#include "RingFeed.h"
#include "audio/AudioShared.h"

// ESP8266Audio output that converts decoded frames to 44.1 kHz, stages them
// and pushes them into the PcmRing (lib/core RingFeed, host-tested; the
// converter is docs/RESAMPLER.md). It never blocks: when the ring is full,
// or the decode task's per-pass budget is spent, ConsumeSample() returns
// false, the generator keeps that sample and hands it over again on its
// next loop(). A rate the converter doesn't take fails the track
// (feed().rejected()), on both outputs.
class RingOutput : public AudioOutput {
public:
  explicit RingOutput(PcmRing& ring) : feed_(ring) {}

  RingFeed& feed() { return feed_; }

  // ---- AudioOutput ----
  bool begin() override { return true; }
  // The base class keeps the rate in a uint16_t, which 88.2/96 kHz overflow.
  bool SetRate(int hz) override { return feed_.setRate(hz); }
  bool SetChannels(int channels) override {
    feed_.setChannels(channels);
    return true;
  }
  bool ConsumeSample(int16_t sample[2]) override { return feed_.consume(sample); }
  void flush() override { feed_.commit(); }
  // generator->stop() calls this: keep what's buffered, the outputs play it out.
  bool stop() override { return true; }

private:
  RingFeed feed_;
};

static_assert(AudioShared::kRingRate == RateConverter::kOutRate, "the ring holds what the converter makes");
// The converter's histories are read 96 times per output: RingOutput must
// stay in internal RAM. With CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL = 4096, a
// `new` of 4 KB or more would go to PSRAM without a word.
static_assert(sizeof(RingOutput) < 4096, "RingOutput must stay under 4 KB to be allocated in internal RAM");
