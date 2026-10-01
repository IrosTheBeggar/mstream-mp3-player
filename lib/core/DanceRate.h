// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The Dance tab's frame rate (docs/ENERGY.md item 8): what the dancer needs,
// not always 30. Measured: +4.5 USB mA with the crab idle at 30 fps, +8.8
// while dancing.
//
//   - Idle (the crab, or the stick figure, swaying on its own: paused,
//     stopped, starved, or no beat lock, with a dance weight of 0.5 or less):
//     10 fps. Breathing and blinking still read fine.
//   - Dancing: 30 fps at 240 MHz; below that (the CPU speed row's 160 MHz, or
//     the console's Pc80) a steady 24, where 30 lands at 20-24 with an MP3.
//     The clock that runs is what counts (getCpuFrequencyMhz()).
//   - The screen off: no frames at all (the UI turns DanceMode off).
//
// The mode comes from the last frame drawn (its beat and weight), so a
// fade in or out runs at the full rate until the weight is past 0.5, and a
// beat that comes back is danced to from the next idle frame (at most
// 100 ms). Pacer keeps the frames on deadlines, and a new period starts the
// cadence over from the last frame drawn: no catch-up burst.
//
// Portable (host-tested: test_dance_rate).
namespace dancerate {

inline constexpr uint32_t kIdleFps = 10;
inline constexpr uint32_t kFullFps = 30;   // at kFullMhz and above
inline constexpr uint32_t kSlowFps = 24;   // below it
inline constexpr uint32_t kFullMhz = 240;
// A weight above this dances (the drawing's `dancing` too: the beat dot).
inline constexpr float kIdleWeight = 0.5f;

// What the last frame showed.
struct Scene {
  bool frozen = false;  // the console's k<n>: a fixed dance pose
  bool beat = false;    // the audio heard had a beat grid (not paused, stopped or starved)
  bool locked = false;  // the tracker was locked
  float weight = 0.0f;  // the dancer's dance weight after that frame
};

enum class Mode : uint8_t { Idle, Dancing };
Mode mode(const Scene& s);
const char* modeName(Mode m);  // "idle", "dancing"

// Frames a second for a mode at a CPU clock.
uint32_t fps(Mode m, uint32_t cpuMhz);
// A frame period in whole ms, rounded: 10 -> 100, 24 -> 42, 30 -> 33.
uint32_t periodMs(uint32_t fps);

// Frame deadlines: each frame is due one period after the last one's
// deadline (the loop's own sleep doesn't push every later frame back); one
// that comes two periods late or more starts the cadence over from itself.
// A period unlike the last one's is counted from the last frame drawn, and
// its first frame starts the new cadence: no burst after a speed-up, no
// leftover wait of the old period. Portable: the caller passes the time.
class Pacer {
public:
  // A frame is to be drawn at `now` (it counts as drawn).
  bool due(uint32_t nowMs, uint32_t periodMs);
  // The next due() is a frame at once (the tab shown again).
  void restart() { drawn_ = false; }
  uint32_t period() const { return periodMs_; }

private:
  uint32_t periodMs_ = 0;
  uint32_t deadlineMs_ = 0;  // the last frame's deadline
  uint32_t lastMs_ = 0;      // when the last frame was drawn
  bool drawn_ = false;
  bool fresh_ = false;       // a new period: its first frame starts the cadence
};

}  // namespace dancerate
