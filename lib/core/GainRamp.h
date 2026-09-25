#pragma once
#include <atomic>
#include <cstdint>

// The Bluetooth output's gain stage: multiplies interleaved stereo int16
// frames by a Q15 gain (32768 = 0 dB), rounded half up and saturated, and
// never lets the gain step up. At unity the audio passes bit-exact.
//
// How the gain moves toward a new target, per frame:
//   - down: linearly, full scale in kDownFrames (~23 ms): a volume-down or a
//     loss of absolute volume must be felt at once;
//   - up: by at most kUpNum/2^20 of itself per frame, ~19.7 dB/s (above
//     ~-40 dB; below, at least kMinUpStep per frame so it gets off zero).
//     Nothing gets suddenly louder, whatever asked for it: a volume step, or
//     any other rise while audio plays (-30 dB -> -2 dB would take ~1.4 s);
//   - after restart() (a new or resumed stream: the listener heard silence):
//     from 0 back up to the level heard before, linearly in at most
//     kFadeFrames (~46 ms), then on at the rate above if the target is higher.
//     A restart() during that fade is ignored (it would be a step down).
//
// Two sides on different tasks:
//   control (one task at a time): request(), restart()
//   audio (one task, the Bluetooth data callback): process()
// Everything the control side hands over is an atomic word, so process()
// never blocks. Targets: when several arrive between two process() calls,
// only the latest counts. A snap (request(t, true), for a new Bluetooth link
// while no audio flows) lowers the gain to t at once, and is kept until the
// next process() even if later targets arrive; it never raises the gain.
// A lift (lift(t), for a handover to the headphones before anything was heard
// on a link) raises the level the next restart() fades in to, up to t (and
// never above the target then); it only ever applies together with a
// restart, so it can't raise audio that is already playing: a process()
// without a pending restart drops it, and the target is reached by the ramp
// instead. A later snap cancels it.
class GainRamp {
public:
  static constexpr uint16_t kUnity = 32768;
  static constexpr uint32_t kDownFrames = 1024;
  static constexpr uint32_t kFadeFrames = 2048;
  static constexpr int32_t kUpNum = 54;       // x 2^-20 of the gain per frame
  static constexpr int32_t kMinUpStep = 512;  // Q30 per frame (0 -> -60 dB in 2048 frames)

  explicit GainRamp(uint16_t initialQ15 = 0);

  // ---- control side ----
  void request(uint16_t targetQ15, bool snap);
  void lift(uint16_t targetQ15);
  void restart() { restart_.store(true, std::memory_order_release); }
  // Sets the gain outright. Only while process() can't run (before the audio
  // task starts).
  void reset(uint16_t gainQ15);

  // ---- audio side ----
  // Scales `frames` stereo frames in place.
  void process(int16_t* lr, uint32_t frames);

  // The gain of the last processed frame (stats; any task).
  uint16_t currentQ15() const { return current_.load(std::memory_order_relaxed); }

  // One sample times a Q15 gain, rounded half up and saturated.
  static int16_t scale(int16_t x, int32_t gainQ15) {
    const int32_t y = (static_cast<int32_t>(x) * gainQ15 + (1 << 14)) >> 15;
    return static_cast<int16_t>(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
  }

private:
  static constexpr int32_t kUnityQ30 = int32_t{1} << 30;
  static constexpr int32_t kNoSnap = -1;

  void step();

  // control side: target (bits 0-16) + sequence number (bits 17-31)
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint32_t> mailbox_;
  std::atomic<int32_t> snap_{kNoSnap};  // Q15 level to drop to, or kNoSnap
  std::atomic<int32_t> lift_{kNoSnap};  // Q15 level the next restart fades in to, or kNoSnap
  std::atomic<bool> restart_{false};
  // audio side; gains in Q30 (Q15 << 15) so slow ramps keep their precision
  uint32_t seen_;
  int32_t cur_;
  int32_t target_;
  int32_t ceiling_ = 0;  // after restart(): the level to fade back to quickly; 0 = none
  std::atomic<uint16_t> current_;
};
