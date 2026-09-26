#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

#include <atomic>
#include <cstdint>

// The Core2's vibration motor (M5.Power.setVibration: the AXP192's LDO3),
// played in short patterns without blocking the loop: a FreeRTOS one-shot
// timer steps through the pattern on the timer service task, so a 15 ms tick
// lasts 15 ms (1 ms ticks) whatever the loop is doing. The AXP192 is on the
// same I2C bus as the touch panel; M5GFX's I2C takes a mutex per transaction,
// so the timer task and the loop's touch reads don't collide.
//
// `level` is setVibration()'s 1-255. On the Core2 M5Unified asks the
// AXP192's LDO3 for 480 + 12 x level mV, and the LDO does 1.8-3.3 V in
// 100 mV steps (rounding down): below 1.8 V (level < 110) it switches the
// LDO off, so the motor doesn't run at all. The strengths the input lab
// offers: kSoft 110 (1.8 V, the lowest the LDO gives; whether the motor
// starts at it is for the haptic test to find out), kMedium 150 (2.2 V),
// kStrong 235 (3.3 V). motorMv() gives the real voltage for a level; play()
// raises a non-zero level below kMinLevel to it, so no pulse is silently off.
class Haptics {
public:
  static constexpr uint8_t kMinLevel = 110;
  static constexpr uint8_t kSoft = 110;
  static constexpr uint8_t kMedium = 150;
  static constexpr uint8_t kStrong = 235;
  static constexpr int kMaxSteps = 8;

  // The LDO3 voltage a setVibration(level) gives (0: off).
  static int motorMv(uint8_t level) {
    const int mv = level ? 480 + 12 * level : 0;
    if (mv < 1800) return 0;
    const int step = (mv - 1800) / 100;
    return 1800 + 100 * (step > 15 ? 15 : step);
  }

  struct Step {
    uint8_t level;  // 0: off (a gap)
    uint16_t ms;
  };

  bool begin();
  // One pulse.
  void tick(uint16_t ms, uint8_t level = kMedium);
  // `count` pulses of `ms`, `gapMs` apart (the spec's double tick: 2, 20, 80).
  void pulses(uint16_t ms, uint8_t level, int count, uint16_t gapMs);
  // Any pattern; a new one replaces what's playing.
  void play(const Step* steps, int count);
  void stop();
  bool busy() const { return busy_.load(); }
  void setEnabled(bool on);
  bool enabled() const { return enabled_; }
  // The motor's measured on-time in the last finished pattern (us), for the lab.
  uint32_t lastOnUs() const { return lastOnUs_.load(); }

private:
  static void onTimer(TimerHandle_t t);
  void step();

  TimerHandle_t timer_ = nullptr;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  Step steps_[kMaxSteps] = {};
  int count_ = 0;
  int next_ = 0;
  bool enabled_ = true;
  std::atomic<bool> busy_{false};
  std::atomic<uint32_t> lastOnUs_{0};
  int64_t onSinceUs_ = 0;  // timer task only
  uint32_t onUs_ = 0;      // timer task only
};
