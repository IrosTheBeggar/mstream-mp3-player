// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The beat tracker's onset front end (BeatTracker step 1; docs/USB-VISUALIZER.md
// "The front end"): mono in, the low and mid band energy of each hop out.
//
//   1. a box average of `decimation` frames (8: 5512.5 Hz at 44.1 kHz);
//   2. a DC blocker (a pole at about 5 Hz);
//   3. the low band: two RBJ low-pass biquads at `lowpassHz` (Q 0.5412 and
//      1.3066: fourth-order Butterworth), transposed direct form II;
//   4. the mid band: the DC-blocked signal less the low band;
//   5. each band's energy summed over a hop (`hop` frames: 64 decimated
//      samples).
//
// BeatTracker runs one on the Core2's own audio; the computer's player runs a
// port of it and sends the two energies per hop over USB (BeatTracker::feedHop()).
// process() is a template in this header so the tracker's per-sample loop
// stays what it was (the onHop call inlines). Portable, no allocation.
class HopFrontEnd {
public:
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    float run(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  // The filters for `sampleRate` (coefficients worked out in double, kept
  // as float), then reset(). `hop` is a multiple of `decimation`.
  void begin(uint32_t sampleRate, uint32_t hop, uint32_t decimation, float lowpassHz);
  // The filters and the hop in progress, all zero: the next sample starts a hop.
  void reset();
  // Mono samples, contiguous with what came before (since reset()). Calls
  // onHop(low, mid) for each hop completed.
  template <typename F>
  void process(const int16_t* mono, uint32_t frames, F&& onHop);

  uint32_t hopLength() const { return hopLen_; }  // decimated samples per hop
  // For the docs, the golden file and a port to check itself against.
  const Biquad& lowpass1() const { return lp1_; }
  const Biquad& lowpass2() const { return lp2_; }
  float dcPole() const { return dcPole_; }

private:
  uint32_t decimation_ = 8;
  float scale_ = 1.0f / (32768.0f * 8.0f);  // int16 sum -> full-scale mean
  float dcPole_ = 0.0f;
  int32_t decimSum_ = 0;
  uint32_t decimCount_ = 0;
  uint32_t hopFill_ = 0;   // decimated samples in the hop so far
  uint32_t hopLen_ = 64;   // decimated samples per hop
  float hopEnergy_ = 0.0f;
  float midEnergy_ = 0.0f; // the rest of the decimated band, above the low-pass
  float dcX_ = 0.0f, dcY_ = 0.0f;
  Biquad lp1_, lp2_;
};

template <typename F>
void HopFrontEnd::process(const int16_t* mono, uint32_t frames, F&& onHop) {
  for (uint32_t i = 0; i < frames; ++i) {
    decimSum_ += mono[i];
    if (++decimCount_ < decimation_) continue;
    const float x = static_cast<float>(decimSum_) * scale_;
    decimSum_ = 0;
    decimCount_ = 0;
    const float dc = x - dcX_ + dcPole_ * dcY_;  // DC blocker
    dcX_ = x;
    dcY_ = dc;
    const float y = lp2_.run(lp1_.run(dc));
    const float mid = dc - y;  // above the low band, up to the decimated Nyquist
    hopEnergy_ += y * y;
    midEnergy_ += mid * mid;
    if (++hopFill_ == hopLen_) {
      onHop(hopEnergy_, midEnergy_);
      hopEnergy_ = midEnergy_ = 0.0f;
      hopFill_ = 0;
    }
  }
}
