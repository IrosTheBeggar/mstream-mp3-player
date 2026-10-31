// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for SaveLater: when a setting changed by taps is written to
// NVS (app/NvsLater writes it then).
// Run: pio test -e native -f test_save_later
#include <unity.h>

#include <cstdint>

#include "SaveLater.h"

namespace {
constexpr uint32_t kQuiet = 1500;
constexpr uint32_t kLatest = 6000;

SaveLater later() {
  SaveLater::Config c;
  c.quietMs = kQuiet;
  c.latestMs = kLatest;
  return SaveLater(c);
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_nothing_is_owed_until_something_changes() {
  SaveLater s = later();
  TEST_ASSERT_FALSE(s.pending());
  TEST_ASSERT_FALSE(s.due(0));
  TEST_ASSERT_FALSE(s.due(100000));
}

void test_one_change_is_written_after_the_quiet() {
  SaveLater s = later();
  s.changed(1000);
  TEST_ASSERT_TRUE(s.pending());
  TEST_ASSERT_EQUAL_UINT32(2500, s.dueAtMs());
  TEST_ASSERT_FALSE(s.due(1000));
  TEST_ASSERT_FALSE(s.due(2499));
  TEST_ASSERT_TRUE(s.due(2500));
  TEST_ASSERT_TRUE(s.due(9000));
}

void test_taps_in_a_row_are_written_once_after_the_last() {
  SaveLater s = later();
  s.changed(1000);
  s.changed(2000);
  TEST_ASSERT_FALSE(s.due(2500));  // the first change's quiet: the second put it off
  s.changed(3000);
  TEST_ASSERT_EQUAL_UINT32(4500, s.dueAtMs());
  TEST_ASSERT_FALSE(s.due(4499));
  TEST_ASSERT_TRUE(s.due(4500));
}

void test_taps_that_never_rest_are_written_by_the_latest() {
  SaveLater s = later();
  for (uint32_t t = 1000; t <= 6500; t += 500) s.changed(t);
  // The quiet after the last (8000) is past the latest after the first (7000).
  TEST_ASSERT_EQUAL_UINT32(7000, s.dueAtMs());
  TEST_ASSERT_FALSE(s.due(6999));
  TEST_ASSERT_TRUE(s.due(7000));
}

void test_written_clears_it_and_the_next_change_starts_over() {
  SaveLater s = later();
  s.changed(1000);
  s.written();
  TEST_ASSERT_FALSE(s.pending());
  TEST_ASSERT_FALSE(s.due(9000));
  s.changed(10000);  // a new first change: its own latest
  TEST_ASSERT_EQUAL_UINT32(11500, s.dueAtMs());
  for (uint32_t t = 10500; t <= 16000; t += 500) s.changed(t);
  TEST_ASSERT_EQUAL_UINT32(16000, s.dueAtMs());
}

void test_the_clock_may_wrap() {
  SaveLater s = later();
  const uint32_t t0 = 0xFFFFFF00u;
  s.changed(t0);
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(t0 + kQuiet), s.dueAtMs());  // 1244, wrapped
  TEST_ASSERT_FALSE(s.due(t0 + 100));
  TEST_ASSERT_FALSE(s.due(1243));
  TEST_ASSERT_TRUE(s.due(1244));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_nothing_is_owed_until_something_changes);
  RUN_TEST(test_one_change_is_written_after_the_quiet);
  RUN_TEST(test_taps_in_a_row_are_written_once_after_the_last);
  RUN_TEST(test_taps_that_never_rest_are_written_by_the_latest);
  RUN_TEST(test_written_clears_it_and_the_next_change_starts_over);
  RUN_TEST(test_the_clock_may_wrap);
  return UNITY_END();
}
