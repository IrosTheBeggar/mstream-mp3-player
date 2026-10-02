// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for touchpower (the FT6336U touch controller's power
// registers, their boot line and the console's Pf argument).
// Run: pio test -e native
#include <unity.h>

#include <cstring>

#include "TouchPower.h"

void setUp() {}
void tearDown() {}

namespace tp = touchpower;

void test_from_bytes_maps_the_burst_in_register_order() {
  const uint8_t block[4] = {1, 30, 60, 25};  // 0x86, 0x87, 0x88, 0x89
  const tp::Regs r = tp::fromBytes(block, tp::kMonitor);
  TEST_ASSERT_EQUAL_UINT8(1, r.ctrl);
  TEST_ASSERT_EQUAL_UINT8(30, r.monitorAfterS);
  TEST_ASSERT_EQUAL_UINT8(60, r.periodActive);
  TEST_ASSERT_EQUAL_UINT8(25, r.periodMonitor);
  TEST_ASSERT_EQUAL_UINT8(1, r.mode);
}

void test_mode_names() {
  TEST_ASSERT_EQUAL_STRING("Active", tp::modeName(0));
  TEST_ASSERT_EQUAL_STRING("Monitor", tp::modeName(1));
  TEST_ASSERT_EQUAL_STRING("Hibernate", tp::modeName(3));
  TEST_ASSERT_EQUAL_STRING("?", tp::modeName(2));
  TEST_ASSERT_EQUAL_STRING("?", tp::modeName(-1));  // no answer
}

void test_auto_monitor_needs_the_switch_and_a_time() {
  tp::Regs r;
  r.ctrl = 1;
  r.monitorAfterS = 10;
  TEST_ASSERT_TRUE(tp::autoMonitors(r));
  r.monitorAfterS = 0;
  TEST_ASSERT_FALSE(tp::autoMonitors(r));
  r.ctrl = 0;
  r.monitorAfterS = 10;
  TEST_ASSERT_FALSE(tp::autoMonitors(r));
}

void test_describe_the_boot_line() {
  const uint8_t on[4] = {1, 30, 60, 25};
  char buf[160];
  tp::describe(tp::fromBytes(on, tp::kActive), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "ctrl=1 monitor_after=30s period_active=60 period_monitor=25 mode=Active (to Monitor by itself after 30 s "
      "untouched)", buf);

  const uint8_t off[4] = {0, 0, 14, 40};
  tp::describe(tp::fromBytes(off, tp::kMonitor), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "ctrl=0 monitor_after=0s period_active=14 period_monitor=40 mode=Monitor (no auto-switch: Active unless set)",
      buf);
}

void test_describe_shows_an_unknown_mode_raw() {
  const uint8_t block[4] = {0, 0, 60, 25};
  char buf[160];
  tp::describe(tp::fromBytes(block, 0x02), buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(strstr(buf, "mode=0x02 "));
}

void test_describe_truncates_safely() {
  const uint8_t block[4] = {1, 30, 60, 25};
  char buf[12];
  tp::describe(tp::fromBytes(block, 0), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("ctrl=1 moni", buf);
  char none[1] = {'x'};
  tp::describe(tp::fromBytes(block, 0), none, 0);  // nothing written
  TEST_ASSERT_EQUAL_CHAR('x', none[0]);
}

void test_pf_arguments() {
  TEST_ASSERT_EQUAL(tp::Pf::Report, tp::parsePf(""));
  TEST_ASSERT_EQUAL(tp::Pf::Report, tp::parsePf(nullptr));
  TEST_ASSERT_EQUAL(tp::Pf::Active, tp::parsePf("0"));
  TEST_ASSERT_EQUAL(tp::Pf::Monitor, tp::parsePf("1"));
}

void test_pf_refuses_hibernate_and_anything_else() {
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("3"));  // Hibernate: the LCD's reset line
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("2"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("10"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("01"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("o"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("o3"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("o10"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("x"));
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("o0"));  // the screen-off policy is gone (measured: no gain)
  TEST_ASSERT_EQUAL(tp::Pf::Refused, tp::parsePf("o1"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_from_bytes_maps_the_burst_in_register_order);
  RUN_TEST(test_mode_names);
  RUN_TEST(test_auto_monitor_needs_the_switch_and_a_time);
  RUN_TEST(test_describe_the_boot_line);
  RUN_TEST(test_describe_shows_an_unknown_mode_raw);
  RUN_TEST(test_describe_truncates_safely);
  RUN_TEST(test_pf_arguments);
  RUN_TEST(test_pf_refuses_hibernate_and_anything_else);
  return UNITY_END();
}
