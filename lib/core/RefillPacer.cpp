#include "RefillPacer.h"

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
