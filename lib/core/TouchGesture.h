#pragma once
#include <cstdint>

// One finger on the glass, from press to release, classified the way the tab
// bar spec (§5) defines the gestures:
//   tap:   released within `slopPx` of the press and within `holdMs`;
//   hold:  `holdMs` or longer without moving `slopPx`;
//   drag:  moved more than `slopPx`;
//   flick: a drag released faster than `flickPxPerS` (the release velocity,
//          over the last `velocityWindowMs` of samples).
// Portable: fed with timestamps and screen coordinates, no clock of its own.
class TouchGesture {
public:
  enum class Kind : uint8_t { None, Tap, Hold, Drag, Flick };

  struct Config {
    int slopPx = 12;
    uint32_t holdMs = 500;
    float flickPxPerS = 400.0f;
    uint32_t velocityWindowMs = 60;
  };

  struct Result {
    Kind kind = Kind::None;
    uint32_t downMs = 0, durationMs = 0;
    int downX = 0, downY = 0, upX = 0, upY = 0;
    int maxMovePx = 0;    // furthest from the press point (Chebyshev: max of |dx|, |dy|)
    float vx = 0, vy = 0; // release velocity, px/s
    float speed = 0;      // |v|
    uint32_t samples = 0;
  };

  TouchGesture() = default;
  explicit TouchGesture(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  void down(uint32_t ms, int x, int y);
  void move(uint32_t ms, int x, int y);
  // Classifies the gesture; the release point is the last sample.
  Result up(uint32_t ms);
  bool active() const { return active_; }
  // While pressed: the hold threshold has passed without moving too far.
  bool holding(uint32_t ms) const {
    return active_ && maxMove_ <= config_.slopPx && ms - downMs_ >= config_.holdMs;
  }
  int maxMove() const { return maxMove_; }

  static const char* name(Kind k);

private:
  struct Sample {
    uint32_t ms;
    int16_t x, y;
  };
  static constexpr int kHistory = 16;
  Config config_;
  bool active_ = false;
  uint32_t downMs_ = 0;
  int downX_ = 0, downY_ = 0;
  int maxMove_ = 0;
  Sample hist_[kHistory] = {};
  int head_ = 0;  // next write
  uint32_t count_ = 0;
};
