#include "DanceRate.h"

namespace dancerate {

Mode mode(const Scene& s) {
  if (s.frozen) return Mode::Dancing;
  // A beat locked on: dance, also while the weight fades in from the idle.
  if (s.beat && s.locked) return Mode::Dancing;
  // No beat (paused, stopped) or no lock: still dancing while it fades out.
  return s.weight > kIdleWeight ? Mode::Dancing : Mode::Idle;
}

const char* modeName(Mode m) { return m == Mode::Dancing ? "dancing" : "idle"; }

uint32_t fps(Mode m, uint32_t cpuMhz) {
  if (m == Mode::Idle) return kIdleFps;
  return cpuMhz >= kFullMhz ? kFullFps : kSlowFps;
}

uint32_t periodMs(uint32_t fps) { return fps ? (1000 + fps / 2) / fps : 0; }

bool Pacer::due(uint32_t nowMs, uint32_t periodMs) {
  if (periodMs != periodMs_) {
    periodMs_ = periodMs;
    deadlineMs_ = lastMs_;  // from the last frame drawn, not the old cadence
    fresh_ = true;
  }
  const uint32_t late = nowMs - deadlineMs_;
  if (drawn_ && late < periodMs_) return false;
  // On schedule, not "a period after whenever the last one was drawn".
  deadlineMs_ = drawn_ && !fresh_ && late < 2 * periodMs_ ? deadlineMs_ + periodMs_ : nowMs;
  lastMs_ = nowMs;
  drawn_ = true;
  fresh_ = false;
  return true;
}

}  // namespace dancerate
