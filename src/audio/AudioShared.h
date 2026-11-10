// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>

#include "FadeStage.h"

// State the decode task shares with the two outputs (the Bluetooth callback
// and the speaker pump). All three run on different tasks.
struct AudioShared {
  // The rate of every frame in the ring: RingOutput converts each track to
  // it (docs/RESAMPLER.md), so Bluetooth's SBC (44.1 kHz only) and the
  // speaker take any track, and an output switch mid-track can't change
  // its speed.
  static constexpr int kRingRate = 44100;
  // Outputs fade the next 64 frames out, then play silence without reading:
  // pausing takes ~1.5 ms and the ring keeps its audio for resume.
  std::atomic<bool> paused{false};
  // A track is producing and past its pre-roll, so an empty ring now means an
  // audible gap: the outputs count it as an underrun.
  std::atomic<bool> expectingAudio{false};
  // Every read that came up short, and every break: a run of short reads,
  // one after another until the ring had what was wanted again, counts
  // once. The Bluetooth callback asks 128 frames at a time (the speaker
  // pump 1,024), so `underruns` says how long the audio was missing, not
  // how often: a 300 ms gap on Bluetooth is ~100 of them, a dip of one
  // callback 1; `breaks` says how often.
  std::atomic<uint32_t> underruns{0};
  std::atomic<uint32_t> breaks{0};
  // An output's read, from its own task (`wasShort` is that task's):
  // `shortRead` when it asked for audio the ring didn't have while audio
  // was expected. Counts, never blocks or logs (the Bluetooth callback).
  void noteRead(bool shortRead, bool* wasShort) {
    if (shortRead) {
      underruns.fetch_add(1, std::memory_order_relaxed);
      if (!*wasShort) breaks.fetch_add(1, std::memory_order_relaxed);
    }
    *wasShort = shortRead;
  }
  // The sleep timer's fade (ENERGY.md section 3): after each output's own
  // gain, one level for both (an output switch during a fade carries it).
  // Target set by the loop task; moved by the output that is the consumer.
  FadeStage fade;
};
