// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The heard clock of the USB visualizer (docs/USB-VISUALIZER.md "The heard
// clock"): the computer says, about 10 times a second, which frame of the
// epoch its listener hears as it writes the line (@c); this turns those
// samples, stamped on arrival, into a smooth clock the dancer reads at any
// moment, in place of TapReader::audibleAt().
//
// The delay from the computer's write to the stamp is always positive and
// varies (USB, the UART, up to a loop pass before the console reads it), so
// the clock follows the least delayed samples:
//
//   1. the samples of the last kWindowUs (at most kMaxSamples);
//   2. their leading edge E(t) = max of heard_i + rate * (t - t_i): each
//      says "at least this far by now", the least delayed the most;
//   3. the clock C(t) = A + rate * (1 + s) * (t - tA), moved at each sample
//      by its error e = E(t) - C(t): no clock yet, or |e| over kSnapUs of
//      frames: a snap (A = E, s = 0); otherwise a slew (A = C(t), s =
//      e / (rate * kSlewUs), within +-kMaxSlew), applied for at most
//      kSlewHoldUs, after which it runs at the plain rate. The error
//      shrinks by a fifth per 100 ms; the beat's phase never jumps.
//
// Valid only while the computer says it plays (playing 1) and the newest
// sample is younger than kStaleUs. A pause empties the window; the next
// sample that plays snaps. Times are uint32_t microseconds (esp_timer's low
// half) compared by signed difference, so the counter's wrap (71 minutes)
// doesn't matter. Frames are kept as a double: sub-frame precision for any
// frame count an epoch reaches (12 h at 48 kHz is 2^31). Portable, no
// allocation (~0.5 KB); host-tested: test_host_clock.
class HostClock {
public:
  static constexpr int kMaxSamples = 32;
  static constexpr uint32_t kWindowUs = 2000000;
  static constexpr uint32_t kSnapUs = 100000;      // an error this large snaps
  static constexpr uint32_t kSlewUs = 500000;      // the slew takes the error out over about this long
  static constexpr float kMaxSlew = 0.05f;         // +-5 % of the rate
  static constexpr uint32_t kSlewHoldUs = 200000;  // a slew applies this long after its sample
  static constexpr uint32_t kStaleUs = 1500000;

  struct Heard {
    bool valid = false;
    int32_t frame = 0;   // the epoch frame heard (negative before frame 0 reaches the ear)
    float frac = 0.0f;   // 0..1 past it
  };
  struct Stats {
    bool valid = false;
    uint32_t snaps = 0;     // since start()
    float slew = 0.0f;      // s now (0.004: 0.4 % fast)
    float spreadMs = 0.0f;  // how far the window's samples sit behind its leading edge (p95)
    uint32_t ageMs = 0;     // the newest sample's age
    int samples = 0;
  };

  // A new epoch or session: forgets everything.
  void start(uint32_t rate);
  // An @c of the current epoch, stamped `nowUs` on arrival.
  void sample(uint32_t nowUs, int32_t heard, bool playing);
  // The frame heard `aheadUs` after `nowUs` (the moment a frame will be on
  // the LCD; negative: before), if the clock is valid at `nowUs`.
  Heard at(uint32_t nowUs, int32_t aheadUs = 0) const;
  // The clock as a frame count (for tests and the log); NAN-free: 0 when invalid.
  double frameAt(uint32_t atUs) const;
  bool valid(uint32_t nowUs) const;
  Stats stats(uint32_t nowUs) const;
  uint32_t rate() const { return rate_; }

private:
  double edgeAt(uint32_t atUs) const;   // E(t)
  double clockAt(uint32_t atUs) const;  // C(t)
  void dropOld(uint32_t nowUs);

  struct Sample {
    uint32_t us;
    int32_t heard;
  };
  uint32_t rate_ = 44100;
  Sample samples_[kMaxSamples] = {};
  int first_ = 0, count_ = 0;   // a ring, oldest first
  bool playing_ = false;
  bool have_ = false;           // a clock (A, tA) since the last snap
  double anchor_ = 0.0;         // A, in frames
  uint32_t anchorUs_ = 0;       // tA
  float slew_ = 0.0f;           // s
  uint32_t lastUs_ = 0;         // the newest sample's arrival (any, playing or not)
  bool any_ = false;
  uint32_t snaps_ = 0;
};
