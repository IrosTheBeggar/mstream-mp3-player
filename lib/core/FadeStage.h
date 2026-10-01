// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>

// The sleep timer's fade (docs/ENERGY.md section 3): an extra gain after
// each output's own gain stage, on Bluetooth in BtSink::onData after
// GainRamp::process, on the speaker in SpeakerSink::pump before playRaw.
// ONE stage for both outputs (it lives in AudioShared), so an output
// switch during a fade carries its level: the new output doesn't start at
// full level and come down.
//
// It is our gain only: nothing is sent to the headphones (no AVRCP
// absolute volume), so their own level is untouched for tomorrow.
//
//   control (the loop task): setTarget() a Q15 target (32768 = 0 dB), never
//   above 1.0; restore() puts the level back to 1.0 at once, and may only
//   be called while nothing is heard (the pause's own fade has passed: the
//   outputs play silence without reading).
//   audio (both outputs' tasks): process(). Only the output that is the
//   ring's consumer moves the level (`advance`); the other (the Bluetooth
//   callback while the speaker plays, either one fading out after a
//   handover) scales by the level as it is. Two tasks moving it would each
//   take a block's step: twice the rate.
//
// How the level moves toward the target, per frame:
//   - down: at most 1/kDownFrames of full scale per frame (~23 ms from full
//     scale to 0, as GainRamp falls): the timer's schedule moves the
//     target in small steps every loop pass (~0.03 dB), and the ramp
//     smooths them;
//   - up: GainRamp's slow rise (kUpNum/2^20 of itself per frame, ~20 dB/s,
//     at least kMinUpStep): "+10 min" or "Turn off" during a fade brings the
//     music back over ~2.4 s, never in one step.
// The level never exceeds 1.0, and never rises unless the target does (the
// timer only raises it on +10 min, Turn off, or restore()). At 1.0 it
// passes the samples bit-exact.
//
// The level is one atomic word; process() publishes its new level with a
// compare-exchange, so a restore() (or the other output's block) in
// between wins over a block computed from an older level.
class FadeStage {
public:
  static constexpr uint16_t kUnity = 32768;
  static constexpr uint32_t kDownFrames = 1024;
  static constexpr int32_t kUpNum = 54;       // x 2^-20 of the level per frame (GainRamp's)
  static constexpr int32_t kMinUpStep = 512;  // Q30 per frame (GainRamp's)

  // ---- control side ----
  void setTarget(uint16_t q15) { target_.store(q15 > kUnity ? kUnity : q15, std::memory_order_release); }
  uint16_t target() const { return target_.load(std::memory_order_acquire); }
  // Level and target back to 1.0 at once. Only while the outputs play
  // silence (a confirmed pause): a jump nobody hears.
  void restore();

  // ---- any task ----
  // The level of the last frame processed, Q15.
  uint16_t levelQ15() const {
    return static_cast<uint16_t>((level_.load(std::memory_order_relaxed) + (1 << 14)) >> 15);
  }
  bool atUnity() const {
    return level_.load(std::memory_order_relaxed) == kUnityQ30 && target() == kUnity;
  }

  // ---- audio side ----
  // Scales `frames` interleaved stereo frames in place; `advance`: this
  // output is the ring's consumer, the level moves toward the target.
  void process(int16_t* lr, uint32_t frames, bool advance = true);

private:
  static constexpr int32_t kUnityQ30 = int32_t{1} << 30;
  std::atomic<uint16_t> target_{kUnity};
  std::atomic<int32_t> level_{kUnityQ30};  // Q30
};
