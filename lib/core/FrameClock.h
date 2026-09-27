#pragma once
#include <cstdint>

// The UI's frame cap, kept on deadlines (docs/UI-SPIKE.md, round 2): each
// frame is due one period after the previous frame's deadline, not after
// the loop pass that happened to draw it, so a late pass (the loop's
// sleep, a decoder pass) doesn't push every later frame back. When a frame
// comes a whole period late or more (a stall, or nothing animated for a
// while) the cadence starts over from that frame: no burst of catch-up
// frames. Portable: the caller passes the time.
//
//   if (clock.due(now) && somethingMoves) { draw(); clock.drawn(now); }
class FrameClock {
public:
  explicit FrameClock(uint32_t periodMs = 33) : periodMs_(periodMs) {}

  // 0: no cap (every pass may draw).
  void setPeriod(uint32_t ms) { periodMs_ = ms; }
  uint32_t period() const { return periodMs_; }

  // A frame may be drawn at `now`.
  bool due(uint32_t nowMs) const { return periodMs_ == 0 || static_cast<int32_t>(nowMs - nextMs_) >= 0; }
  // One was drawn at `now` (call only when due()).
  void drawn(uint32_t nowMs) {
    nextMs_ += periodMs_;
    if (static_cast<int32_t>(nowMs - nextMs_) >= 0) nextMs_ = nowMs + periodMs_;
  }
  // How long until the next frame is due (0: now).
  uint32_t msUntilDue(uint32_t nowMs) const {
    const int32_t d = static_cast<int32_t>(nextMs_ - nowMs);
    return d > 0 && periodMs_ ? static_cast<uint32_t>(d) : 0u;
  }

private:
  uint32_t periodMs_;
  uint32_t nextMs_ = 0;
};
