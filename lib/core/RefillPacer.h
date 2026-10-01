// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// How fast the decoder refills its ring once it holds enough (docs/UI-SPIKE.md,
// "Scroll round 2"). At a track start or skip the ring is empty and the
// decoder, above the UI on core 1, fills all ~1.45 s of it as fast as it can:
// for an MP3 that is ~2.3x realtime, the whole core, for ~0.7 s, and the UI
// stalls that long. Below gentleFromMs the decoder still runs flat out (the
// time to first audio and the early ring are as before); from there it sleeps
// after each pass so it produces at most capX10 / 10 times realtime, and the
// rest of the core goes to the UI. The caller applies it to the fill after a
// start or skip only (until the ring is first full), not to later dips.
//
// On by default: on the device it halved the UI's stall at every start and
// skip (MP3 ~800 ms to ~150-380 ms) with no underrun in 41 paced starts, and
// left the time to first audio where it was.
struct RefillPacer {
  // The lowest cap it applies (1.5x): at ~1x the paced ring would stop
  // growing and settle at gentleFromMs (the sleep is rounded up).
  static constexpr uint32_t kMinCapX10 = 15;
  struct Config {
    bool enabled = true;
    uint32_t gentleFromMs = 500;  // flat out below this much buffered
    uint32_t capX10 = 15;         // at most 1.5x realtime above it (kMinCapX10 at least)
    uint32_t maxSleepMs = 40;     // never sleep longer than this per pass
  };

  // How long the decode task sleeps after a pass that produced `frames` at
  // `rate` Hz in `passUs` of its time, with `ringMs` buffered after it.
  // 1 (a plain vTaskDelay(1)) when disabled, below gentleFromMs, or when
  // the pass is already slower than the cap.
  static uint32_t sleepMs(const Config& c, uint32_t ringMs, uint32_t frames, uint32_t rate, uint32_t passUs);
};
