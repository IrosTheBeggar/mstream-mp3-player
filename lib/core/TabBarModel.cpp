#include "TabBarModel.h"

#include <cstdio>

namespace tabbar {

uint8_t dirty(const State& a, const State& b) {
  uint8_t d = 0;
  // The active tab moving redraws the old and the new cell (plate, label, colour).
  if (a.active != b.active) d |= static_cast<uint8_t>((1u << a.active) | (1u << b.active));
  const bool eqMoves = b.play == Play::Playing && a.eqStep != b.eqStep;
  if (a.play != b.play || eqMoves || a.progressPx != b.progressPx || a.progressKnown != b.progressKnown) d |= 1u << 0;
  if (a.upNext != b.upNext || a.badgeFlash != b.badgeFlash) d |= 1u << 2;
  if (a.output != b.output || a.volume != b.volume || a.battery != b.battery || a.charging != b.charging ||
      a.lowBlink != b.lowBlink) {
    d |= 1u << 4;
  }
  return static_cast<uint8_t>(d & kAll);
}

void eqBars(uint8_t step, bool playing, uint8_t out[4]) {
  if (!playing) {
    // Paused: short flat bars.
    for (int i = 0; i < 4; ++i) out[i] = 3;
    return;
  }
  // A small hash per bar and step: lively, never all four at once at the top.
  for (int i = 0; i < 4; ++i) {
    uint32_t h = (step * 2654435761u) ^ (static_cast<uint32_t>(i + 1) * 40503u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    out[i] = static_cast<uint8_t>(4 + h % (kEqMaxH - 3));  // 4..16
  }
}

void badgeText(uint32_t n, char out[4]) {
  if (n == 0) {
    out[0] = 0;
  } else if (n > 99) {
    std::snprintf(out, 4, "99+");
  } else {
    std::snprintf(out, 4, "%u", static_cast<unsigned>(n));
  }
}

uint8_t progressPx(uint32_t positionMs, uint32_t durationMs) {
  if (durationMs == 0) return 0;
  if (positionMs >= durationMs) return kProgressW;
  return static_cast<uint8_t>(static_cast<uint64_t>(positionMs) * kProgressW / durationMs);
}

}  // namespace tabbar
