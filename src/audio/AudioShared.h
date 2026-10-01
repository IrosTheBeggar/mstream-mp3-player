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
  std::atomic<uint32_t> underruns{0};
  // The sleep timer's fade (ENERGY.md section 3): after each output's own
  // gain, one level for both (an output switch during a fade carries it).
  // Target set by the loop task; moved by the output that is the consumer.
  FadeStage fade;
};
