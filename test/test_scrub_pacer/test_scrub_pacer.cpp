// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for ScrubPacer: when the list under a rail scrub is drawn
// again (ui/ListView draws the rail alone on the frames between).
// Run: pio test -e native -f test_scrub_pacer
#include <unity.h>

#include <cstdint>

#include "ScrubPacer.h"

namespace {
constexpr uint32_t kCadence = 60;
constexpr uint32_t kSettle = 40;

ScrubPacer pacer(uint32_t beginMs) {
  ScrubPacer::Config c;
  c.cadenceMs = kCadence;
  c.settleMs = kSettle;
  ScrubPacer p(c);
  p.begin(beginMs);
  return p;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_nothing_is_due_until_a_position_is_asked_for() {
  ScrubPacer p = pacer(1000);
  TEST_ASSERT_FALSE(p.owed());
  TEST_ASSERT_FALSE(p.bandDue(1000));
  TEST_ASSERT_FALSE(p.bandDue(5000));
}

void test_the_first_position_is_due_at_once() {
  ScrubPacer p = pacer(1000);
  p.targetChanged(1030);  // the drag's first move, past the slop
  TEST_ASSERT_TRUE(p.owed());
  TEST_ASSERT_TRUE(p.bandDue(1030));
}

// Right after a band draw a moving finger waits for the cadence, counted
// from the draw's END (a slow draw doesn't make the next due at once).
void test_a_moving_finger_waits_for_the_cadence_after_the_draw_ended() {
  ScrubPacer p = pacer(1000);
  p.targetChanged(1030);
  p.bandDrawn(1100);  // a 70 ms draw
  TEST_ASSERT_FALSE(p.owed());
  TEST_ASSERT_FALSE(p.bandDue(1100));
  // The finger keeps moving: a change every 10 ms.
  for (uint32_t t = 1110; t < 1100 + kCadence; t += 10) {
    p.targetChanged(t);
    TEST_ASSERT_FALSE(p.bandDue(t));
  }
  p.targetChanged(1160);
  TEST_ASSERT_TRUE(p.bandDue(1160));
}

// A finger that rests gets the band before the cadence.
void test_a_resting_finger_gets_the_band_after_the_settle_time() {
  ScrubPacer p = pacer(1000);
  p.targetChanged(1030);
  p.bandDrawn(1100);
  p.targetChanged(1105);  // one more move, then it rests
  TEST_ASSERT_FALSE(p.bandDue(1105));
  TEST_ASSERT_FALSE(p.bandDue(1105 + kSettle - 1));
  TEST_ASSERT_TRUE(p.bandDue(1105 + kSettle));  // 1145: before the cadence's 1160
}

void test_a_draw_clears_what_was_owed() {
  ScrubPacer p = pacer(1000);
  p.targetChanged(1030);
  p.bandDrawn(1100);
  p.targetChanged(1110);
  p.bandDrawn(1200);
  TEST_ASSERT_FALSE(p.owed());
  TEST_ASSERT_FALSE(p.bandDue(1200 + kCadence + 1000));
}

void test_begin_forgets_the_last_scrub() {
  ScrubPacer p = pacer(1000);
  p.targetChanged(1030);
  p.bandDrawn(1100);
  p.targetChanged(1110);  // owed, not due yet
  p.begin(1120);          // a new finger on the rail
  TEST_ASSERT_FALSE(p.owed());
  TEST_ASSERT_FALSE(p.bandDue(1120));
  p.targetChanged(1140);
  TEST_ASSERT_TRUE(p.bandDue(1140));  // the new scrub's first position: at once
}

void test_the_times_wrap_safely() {
  const uint32_t t0 = 0xFFFFFFF0u;
  ScrubPacer p = pacer(t0);
  p.targetChanged(t0 + 5);
  TEST_ASSERT_TRUE(p.bandDue(t0 + 5));
  p.bandDrawn(t0 + 20);  // past the wrap: 4
  p.targetChanged(t0 + 25);
  TEST_ASSERT_FALSE(p.bandDue(t0 + 25));
  TEST_ASSERT_TRUE(p.bandDue(t0 + 25 + kSettle));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_is_due_until_a_position_is_asked_for);
  RUN_TEST(test_the_first_position_is_due_at_once);
  RUN_TEST(test_a_moving_finger_waits_for_the_cadence_after_the_draw_ended);
  RUN_TEST(test_a_resting_finger_gets_the_band_after_the_settle_time);
  RUN_TEST(test_a_draw_clears_what_was_owed);
  RUN_TEST(test_begin_forgets_the_last_scrub);
  RUN_TEST(test_the_times_wrap_safely);
  return UNITY_END();
}
