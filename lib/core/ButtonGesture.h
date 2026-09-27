#pragma once
#include <cstdint>

// One of the Core2's three touch buttons (BtnA/B/C), from its pressed state
// sampled every loop pass: click, hold, and the hold's auto-repeat.
//
//   Press   the button went down
//   Click   released before holdMs (acts on release: a hold never clicks)
//   Hold    holdMs reached while still down (acts at that moment, once)
//   Repeat  every repeatMs after the Hold while still down (repeatMs 0: none)
//   HoldEnd released after a Hold
//
// The user's measurements on the device: clicks last 17-143 ms, holds
// 509-2383 ms, so 500 ms separates them (the design's 800 ms for B would
// only delay the output switch). A repeat that falls behind (the loop
// stalled) is not caught up in a burst: the next one is a period from now.
//
// Portable: fed with timestamps, no clock of its own. One event per update
// (a press and its release can't land in the same update: the state is
// sampled).
class ButtonGesture {
public:
  enum class Event : uint8_t { None, Press, Click, Hold, Repeat, HoldEnd };

  struct Config {
    uint32_t holdMs = 500;
    uint32_t repeatMs = 0;  // 0: a hold doesn't repeat
  };

  ButtonGesture() = default;
  explicit ButtonGesture(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  Event update(uint32_t ms, bool pressed);
  bool pressed() const { return down_; }
  bool held() const { return held_; }
  // How long the current press has lasted (0 when up).
  uint32_t pressedFor(uint32_t ms) const { return down_ ? ms - downMs_ : 0; }
  // Repeats since the Hold (0 at the Hold itself).
  uint32_t repeats() const { return repeats_; }

  static const char* name(Event e);

private:
  Config config_;
  bool down_ = false;
  bool held_ = false;
  uint32_t downMs_ = 0;
  uint32_t nextRepeatMs_ = 0;
  uint32_t repeats_ = 0;
};
