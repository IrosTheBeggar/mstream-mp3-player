// Host tests for the UI spike's portable pieces: touch gesture
// classification, kinetic scrolling, the scroll governor and percentiles.
// Run: pio test -e native
#include <unity.h>

#include <cmath>

#include "KineticScroll.h"
#include "Percentiles.h"
#include "ScrollGovernor.h"
#include "TouchGesture.h"

void setUp() {}
void tearDown() {}

// ---- TouchGesture ----

void test_gesture_tap_and_hold() {
  TouchGesture g;
  g.down(1000, 100, 100);
  g.move(1040, 103, 98);
  TouchGesture::Result r = g.up(1120);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Tap), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL_UINT32(120, r.durationMs);
  TEST_ASSERT_EQUAL_INT(3, r.maxMovePx);
  TEST_ASSERT_EQUAL_INT(100, r.downX);
  TEST_ASSERT_EQUAL_INT(103, r.upX);

  g.down(2000, 50, 50);
  TEST_ASSERT_FALSE(g.holding(2400));
  g.move(2300, 58, 52);
  TEST_ASSERT_TRUE(g.holding(2500));
  r = g.up(2700);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Hold), static_cast<int>(r.kind));
  TEST_ASSERT_FALSE(g.active());
}

void test_gesture_drag_and_flick() {
  TouchGesture g;
  // Slow drag: 60 px over 600 ms, 10 px per 100 ms at the end.
  g.down(0, 100, 200);
  for (int i = 1; i <= 6; ++i) g.move(i * 100, 100, 200 - 10 * i);
  TouchGesture::Result r = g.up(610);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Drag), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL_INT(60, r.maxMovePx);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -100.0f, r.vy);

  // Flick: 20 px per 16 ms at the end, about 1250 px/s upward.
  g.down(0, 100, 200);
  for (int i = 1; i <= 6; ++i) g.move(i * 16, 100, 200 - 20 * i);
  r = g.up(100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Flick), static_cast<int>(r.kind));
  TEST_ASSERT_FLOAT_WITHIN(20.0f, -1250.0f, r.vy);
  TEST_ASSERT_FLOAT_WITHIN(20.0f, 1250.0f, r.speed);
  TEST_ASSERT_EQUAL_STRING("flick", TouchGesture::name(r.kind));
}

// ---- KineticScroll ----

void test_scroll_drag_is_one_to_one_and_clamped() {
  KineticScroll s;
  s.setExtent(20 * 42, 168);  // 20 rows, 4 visible
  TEST_ASSERT_EQUAL_FLOAT(20 * 42 - 168, s.maxOffset());
  s.press(0, 200);
  s.drag(10, 150);
  TEST_ASSERT_EQUAL_FLOAT(50, s.offset());
  s.drag(20, 260);  // past the top: clamped
  TEST_ASSERT_EQUAL_FLOAT(0, s.offset());
  s.drag(30, 180);
  TEST_ASSERT_EQUAL_FLOAT(20, s.offset());
}

void test_scroll_slow_release_snaps_to_nearest_row() {
  KineticScroll s;
  s.setExtent(20 * 42, 168);
  s.press(0, 200);
  s.drag(100, 170);   // offset 30
  s.drag(400, 170);   // held still
  s.release(500);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(KineticScroll::Phase::Snapping), static_cast<int>(s.phase()));
  uint32_t t = 500;
  while (s.moving() && t < 2000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(42, s.offset());
  TEST_ASSERT_FALSE(s.moving());
}

void test_scroll_fling_decays_and_lands_on_a_row() {
  KineticScroll s;
  s.setExtent(200 * 42, 168);
  s.fling(0, 2000);
  uint32_t t = 0;
  float last = 0;
  while (s.moving() && t < 10000) {
    s.update(t += 33);
    TEST_ASSERT_TRUE(s.offset() >= last - 0.001f);  // never goes back
    last = s.offset();
  }
  TEST_ASSERT_FALSE(s.moving());
  TEST_ASSERT_EQUAL_FLOAT(0, std::fmod(s.offset(), 42.0f));
  // The distance of a 0.92-per-66.7ms decay from 2000 px/s down to the stop
  // speed is ~ (2000-60) / k, k = -ln(0.92)/66.7 ms: about 1550 px, plus the snap.
  TEST_ASSERT_TRUE(s.offset() > 1400 && s.offset() < 1700);
  // The same fling at a different frame rate ends at the same row.
  KineticScroll s2;
  s2.setExtent(200 * 42, 168);
  s2.fling(0, 2000);
  t = 0;
  while (s2.moving() && t < 10000) s2.update(t += 10);
  TEST_ASSERT_FLOAT_WITHIN(42.0f, s.offset(), s2.offset());
}

void test_scroll_release_velocity_and_edges() {
  KineticScroll s;
  s.setExtent(30 * 42, 168);
  // Finger moves up 30 px per 16 ms: the list flings up (offset grows).
  s.press(0, 220);
  for (int i = 1; i <= 5; ++i) s.drag(i * 16, 220 - 30 * i);
  s.release(84);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(KineticScroll::Phase::Flinging), static_cast<int>(s.phase()));
  TEST_ASSERT_TRUE(s.velocity() > 1500);
  uint32_t t = 84;
  while (s.moving() && t < 10000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(s.maxOffset(), s.offset());  // hit the end and stopped there
  // A fling down from the end stops at 0.
  s.fling(t, -5000);
  while (s.moving() && t < 20000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(0, s.offset());
  // A finger that stopped before lifting doesn't fling.
  s.press(t, 200);
  s.drag(t + 16, 150);
  s.release(t + 300);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(KineticScroll::Phase::Flinging), static_cast<int>(s.phase()));
  // jumpTo stops everything.
  s.fling(t, 3000);
  s.jumpTo(10 * 42);
  TEST_ASSERT_FALSE(s.moving());
  TEST_ASSERT_EQUAL_FLOAT(420, s.offset());
}

// ---- ScrollGovernor ----

void test_governor_levels_follow_the_ring() {
  ScrollGovernor g;
  ScrollGovernor::Budget b = g.update(0, 1450, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  TEST_ASSERT_EQUAL_UINT32(66, b.frameMs);
  b = g.update(100, 800, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  TEST_ASSERT_EQUAL_UINT32(125, b.frameMs);
  b = g.update(200, 400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::WholeRows), static_cast<int>(b.level));
  TEST_ASSERT_TRUE(b.wholeRows);
  b = g.update(300, 100, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  TEST_ASSERT_FALSE(b.draw);
  // Recovery: one level per holdMs, and only above threshold + margin.
  b = g.update(400, 300, 0, true);  // above 250 but not 250 + 200: stays paused
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  b = g.update(2300, 1400, 0, true);  // clock started at 400
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  b = g.update(2500, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::WholeRows), static_cast<int>(b.level));
  b = g.update(4600, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(6700, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
}

void test_governor_underrun_and_idle() {
  ScrollGovernor g;
  g.update(0, 1450, 5, true);
  ScrollGovernor::Budget b = g.update(100, 1450, 6, true);  // an underrun with a full ring
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(1500, 1450, 6, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(2200, 1450, 6, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  // Nothing playing: an empty ring is no risk.
  b = g.update(3000, 0, 6, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  // Disabled: always normal.
  ScrollGovernor::Config c;
  c.enabled = false;
  g.setConfig(c);
  b = g.update(4000, 50, 9, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
}

// ---- Percentiles ----

void test_percentiles() {
  float v[] = {5, 1, 4, 2, 3, 10, 9, 8, 7, 6};
  const Percentiles p = Percentiles::of(v, 10);
  TEST_ASSERT_EQUAL_UINT32(10, p.n);
  TEST_ASSERT_EQUAL_FLOAT(1, p.min);
  TEST_ASSERT_EQUAL_FLOAT(10, p.max);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, p.p50);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, p.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.9f, p.p10);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.1f, p.p90);
  const Percentiles none = Percentiles::of(v, 0);
  TEST_ASSERT_EQUAL_UINT32(0, none.n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_gesture_tap_and_hold);
  RUN_TEST(test_gesture_drag_and_flick);
  RUN_TEST(test_scroll_drag_is_one_to_one_and_clamped);
  RUN_TEST(test_scroll_slow_release_snaps_to_nearest_row);
  RUN_TEST(test_scroll_fling_decays_and_lands_on_a_row);
  RUN_TEST(test_scroll_release_velocity_and_edges);
  RUN_TEST(test_governor_levels_follow_the_ring);
  RUN_TEST(test_governor_underrun_and_idle);
  RUN_TEST(test_percentiles);
  return UNITY_END();
}
