// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for chiprev (the ESP32 revision the firmware needs: the
// boot guard for the build without the rev-1 PSRAM workaround).
// Run: pio test -e native
#include <unity.h>

#include "ChipRevision.h"

void setUp() {}
void tearDown() {}

void test_revision_3_and_later_are_supported() {
  TEST_ASSERT_TRUE(chiprev::supported(300));  // 3.0 (ECO3)
  TEST_ASSERT_TRUE(chiprev::supported(301));  // 3.1: this Core2
  TEST_ASSERT_TRUE(chiprev::supported(400));
}

void test_older_revisions_are_not() {
  TEST_ASSERT_FALSE(chiprev::supported(0));    // 0.0
  TEST_ASSERT_FALSE(chiprev::supported(100));  // 1.0: the PSRAM cache bug
  TEST_ASSERT_FALSE(chiprev::supported(101));
  TEST_ASSERT_FALSE(chiprev::supported(200));
  TEST_ASSERT_FALSE(chiprev::supported(299));
}

void test_text() {
  char buf[8];
  chiprev::text(301, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("3.1", buf);
  chiprev::text(100, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("1.0", buf);
  chiprev::text(0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("0.0", buf);
  char small[3];
  chiprev::text(301, small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("3.", small);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_revision_3_and_later_are_supported);
  RUN_TEST(test_older_revisions_are_not);
  RUN_TEST(test_text);
  return UNITY_END();
}
