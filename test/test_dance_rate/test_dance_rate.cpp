// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Dance tab's frame rate (DanceRate, docs/ENERGY.md item
// 8): the mode, the rate at a CPU clock, and the pacer's deadlines, alone
// and driven the way DanceMode::render() drives them, with the real crab.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <vector>

#include "CrabPose.h"
#include "DanceRate.h"

using dancerate::Mode;
using dancerate::Pacer;
using dancerate::Scene;

namespace {

Scene scene(bool beat, bool locked, float weight, bool frozen = false) {
  Scene s;
  s.beat = beat;
  s.locked = locked;
  s.weight = weight;
  s.frozen = frozen;
  return s;
}

// Loop passes every `stepMs` from `from` to `to` (excluded) at a fixed
// period: the times a frame was drawn.
std::vector<uint32_t> run(Pacer& p, uint32_t from, uint32_t to, uint32_t stepMs, uint32_t periodMs) {
  std::vector<uint32_t> frames;
  for (uint32_t t = from; t != to; t += stepMs) {
    if (p.due(t, periodMs)) frames.push_back(t);
  }
  return frames;
}

uint32_t minGap(const std::vector<uint32_t>& f) {
  uint32_t m = UINT32_MAX;
  for (size_t i = 1; i < f.size(); ++i) m = f[i] - f[i - 1] < m ? f[i] - f[i - 1] : m;
  return m;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_mode() {
  // Paused or stopped: no beat heard; idle once the weight is down.
  TEST_ASSERT_TRUE(dancerate::mode(scene(false, false, 0.0f)) == Mode::Idle);
  TEST_ASSERT_TRUE(dancerate::mode(scene(false, true, 0.5f)) == Mode::Idle);
  // ... but dancing while it fades out.
  TEST_ASSERT_TRUE(dancerate::mode(scene(false, true, 0.51f)) == Mode::Dancing);
  // Playing without a lock: idle at a low weight.
  TEST_ASSERT_TRUE(dancerate::mode(scene(true, false, 0.2f)) == Mode::Idle);
  TEST_ASSERT_TRUE(dancerate::mode(scene(true, false, 0.8f)) == Mode::Dancing);
  // Locked on a beat: dancing from the fade-in's first frame.
  TEST_ASSERT_TRUE(dancerate::mode(scene(true, true, 0.0f)) == Mode::Dancing);
  // A lock with nothing heard (paused on a locked grid) is not a beat.
  TEST_ASSERT_TRUE(dancerate::mode(scene(false, true, 0.0f)) == Mode::Idle);
  // The console's frozen pose is a dance pose.
  TEST_ASSERT_TRUE(dancerate::mode(scene(false, false, 0.0f, true)) == Mode::Dancing);
  TEST_ASSERT_EQUAL_STRING("idle", dancerate::modeName(Mode::Idle));
  TEST_ASSERT_EQUAL_STRING("dancing", dancerate::modeName(Mode::Dancing));
}

void test_the_rate_follows_the_clock() {
  TEST_ASSERT_EQUAL_UINT32(30, dancerate::fps(Mode::Dancing, 240));
  TEST_ASSERT_EQUAL_UINT32(24, dancerate::fps(Mode::Dancing, 160));
  TEST_ASSERT_EQUAL_UINT32(24, dancerate::fps(Mode::Dancing, 80));
  TEST_ASSERT_EQUAL_UINT32(10, dancerate::fps(Mode::Idle, 240));
  TEST_ASSERT_EQUAL_UINT32(10, dancerate::fps(Mode::Idle, 160));
  TEST_ASSERT_EQUAL_UINT32(100, dancerate::periodMs(10));
  TEST_ASSERT_EQUAL_UINT32(42, dancerate::periodMs(24));
  TEST_ASSERT_EQUAL_UINT32(33, dancerate::periodMs(30));
  TEST_ASSERT_EQUAL_UINT32(0, dancerate::periodMs(0));
}

void test_a_steady_rate_keeps_its_deadlines() {
  // Loop passes every 5 ms: frames on the 33 ms deadlines, not 35 ms apart.
  Pacer p;
  const auto f = run(p, 1000, 11000, 5, 33);
  TEST_ASSERT_INT_WITHIN(2, 303, static_cast<int>(f.size()));
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(33 - 5, minGap(f));
  Pacer q;
  const auto g = run(q, 1000, 11000, 5, 42);
  TEST_ASSERT_INT_WITHIN(2, 238, static_cast<int>(g.size()));
  Pacer r;
  const auto h = run(r, 1000, 11000, 5, 100);
  TEST_ASSERT_EQUAL(100, static_cast<int>(h.size()));
}

void test_the_first_frame_is_at_once() {
  Pacer p;
  TEST_ASSERT_TRUE(p.due(12345, 100));
  TEST_ASSERT_FALSE(p.due(12346, 100));
  TEST_ASSERT_TRUE(p.due(12445, 100));
  p.restart();  // the tab shown again
  TEST_ASSERT_TRUE(p.due(12450, 100));
}

void test_a_speed_up_has_no_catch_up_burst() {
  // Idle frames at 100 ms; 60 ms after the last one it dances at 33.
  Pacer p;
  auto f = run(p, 0, 1000, 5, 100);
  const uint32_t last = f.back();  // 900
  TEST_ASSERT_EQUAL_UINT32(900, last);
  // The first fast frame: at once (60 ms is past a 33 ms period) ...
  TEST_ASSERT_TRUE(p.due(last + 60, 33));
  // ... and the next a whole period later, not 6 ms later to catch up.
  const auto g = run(p, last + 65, last + 2000, 5, 33);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(last + 60 + 33, g.front());
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(last + 60 + 33 + 5, g.front());
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(33 - 5, minGap(g));
}

void test_a_slow_down_waits_the_new_period() {
  Pacer p;
  auto f = run(p, 0, 1000, 1, 33);
  const uint32_t last = f.back();
  // Idle from here: nothing until 100 ms after the last frame drawn.
  TEST_ASSERT_FALSE(p.due(last + 34, 100));
  TEST_ASSERT_FALSE(p.due(last + 99, 100));
  TEST_ASSERT_TRUE(p.due(last + 100, 100));
  TEST_ASSERT_FALSE(p.due(last + 150, 100));
  TEST_ASSERT_TRUE(p.due(last + 200, 100));
}

void test_a_stall_starts_the_cadence_over() {
  Pacer p;
  TEST_ASSERT_TRUE(p.due(0, 33));
  // A 500 ms stall (a screenshot, a decoder refill): one frame, then 33 on.
  TEST_ASSERT_TRUE(p.due(500, 33));
  TEST_ASSERT_FALSE(p.due(501, 33));
  TEST_ASSERT_FALSE(p.due(532, 33));
  TEST_ASSERT_TRUE(p.due(533, 33));
}

void test_the_clock_wraps() {
  Pacer p;
  const auto f = run(p, 0xFFFFFF00u, 0x00000200u, 1, 33);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(33, minGap(f));
  TEST_ASSERT_INT_WITHIN(1, 0x300 / 33, static_cast<int>(f.size()));
}

// DanceMode::render()'s way, with the real crab: dancing at 160 MHz, a
// pause (the beat stops), the fade, idle at 10 fps, a play again.
void test_a_pause_and_a_play_with_the_crab() {
  crab::Crab crab;
  Pacer p;
  Scene last;
  std::vector<uint32_t> frames;
  std::vector<Mode> modes;
  uint32_t prevFrame = 0;
  const uint32_t pauseAt = 5000, playAt = 9000;
  for (uint32_t t = 0; t < 12000; t += 5) {
    const Mode m = dancerate::mode(last);
    if (!p.due(t, dancerate::periodMs(dancerate::fps(m, 160)))) continue;
    const bool beat = t < pauseAt || t >= playAt;
    const float dt = frames.empty() ? 0.0f : (t - prevFrame) * 1e-3f;
    crab.update(0.25f, false, beat ? 0.9f : 0.0f, beat, dt);
    last = scene(beat, beat, crab.weight());
    frames.push_back(t);
    modes.push_back(m);
    prevFrame = t;
  }
  auto countIn = [&](uint32_t a, uint32_t b) {
    int n = 0;
    for (uint32_t t : frames) n += t >= a && t < b;
    return n;
  };
  // Dancing: a steady 24 (42 ms).
  TEST_ASSERT_INT_WITHIN(2, 48, countIn(2000, 4000));
  // Fading out (0.4 s time constant) then idle: 10 fps.
  TEST_ASSERT_INT_WITHIN(1, 20, countIn(6000, 8000));
  // Back to 24 within one idle frame of the play.
  size_t firstFast = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    if (frames[i] >= playAt && modes[i] == Mode::Dancing) {
      firstFast = i;
      break;
    }
  }
  TEST_ASSERT_TRUE(firstFast > 0);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(playAt + 100 + 42 + 5, frames[firstFast]);
  TEST_ASSERT_INT_WITHIN(2, 48, countIn(10000, 12000));
  // No burst anywhere: no two frames closer than a dancing period's, less
  // one loop pass.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(42 - 5, minGap(frames));
  // The fade out ran at the full rate while the weight was above 0.5.
  int fastAfterPause = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    fastAfterPause += frames[i] > pauseAt && frames[i] < playAt && modes[i] == Mode::Dancing;
  }
  TEST_ASSERT_GREATER_THAN(3, fastAfterPause);
  TEST_ASSERT_LESS_THAN(20, fastAfterPause);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_mode);
  RUN_TEST(test_the_rate_follows_the_clock);
  RUN_TEST(test_a_steady_rate_keeps_its_deadlines);
  RUN_TEST(test_the_first_frame_is_at_once);
  RUN_TEST(test_a_speed_up_has_no_catch_up_burst);
  RUN_TEST(test_a_slow_down_waits_the_new_period);
  RUN_TEST(test_a_stall_starts_the_cadence_over);
  RUN_TEST(test_the_clock_wraps);
  RUN_TEST(test_a_pause_and_a_play_with_the_crab);
  return UNITY_END();
}
