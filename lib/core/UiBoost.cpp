#include "UiBoost.h"

bool UiBoost::update(uint32_t nowMs, bool everActive, uint32_t lastActiveMs, uint32_t ringMs, bool playing) {
  const bool wanted = everActive && static_cast<uint32_t>(nowMs - lastActiveMs) <= config_.lingerMs;
  if (!wanted) {
    on_ = false;
    why_ = Why::Idle;
  } else if (!playing) {
    on_ = true;
    why_ = Why::NotPlaying;
  } else if (ringMs < config_.floorMs) {
    if (on_) {  // a drop: starts the hold-off
      dropped_ = true;
      droppedAtMs_ = nowMs;
    }
    on_ = false;
    why_ = Why::BelowFloor;
  } else if (on_ || (ringMs >= config_.resumeMs &&
                     (!dropped_ || static_cast<uint32_t>(nowMs - droppedAtMs_) >= config_.holdOffMs))) {
    dropped_ = false;
    on_ = true;
    why_ = Why::Interacting;
  } else {
    // Between the floor and resumeMs (or in the hold-off), off: stay off
    // until the ring is back. (A BelowFloor drop stays BelowFloor in the
    // logs until then.)
    if (why_ != Why::BelowFloor) why_ = Why::BelowResume;
  }
  return on_;
}

const char* UiBoost::name(Why w) {
  switch (w) {
    case Why::Idle: return "idle";
    case Why::Interacting: return "interacting";
    case Why::NotPlaying: return "not playing";
    case Why::BelowFloor: return "ring below the floor";
    case Why::BelowResume: return "ring refilling";
  }
  return "?";
}

uint32_t RefillPacer::sleepMs(const Config& c, uint32_t ringMs, uint32_t frames, uint32_t rate, uint32_t passUs) {
  if (!c.enabled || ringMs < c.gentleFromMs || frames == 0 || rate == 0) return 1;
  const uint32_t cap = c.capX10 < kMinCapX10 ? kMinCapX10 : c.capX10;
  // The pass's audio, in us, divided by the cap: how long a pass may take at least.
  const uint64_t audioUs = static_cast<uint64_t>(frames) * 1000000u / rate;
  const uint64_t minWallUs = audioUs * 10u / cap;
  if (minWallUs <= passUs) return 1;
  uint64_t ms = (minWallUs - passUs + 999u) / 1000u;  // round up: never faster than the cap
  if (ms < 1) ms = 1;
  if (ms > c.maxSleepMs) ms = c.maxSleepMs;
  return static_cast<uint32_t>(ms);
}
