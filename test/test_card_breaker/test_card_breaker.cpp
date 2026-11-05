// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Host tests of the card's breaker (lib/core/CardBreaker.h).
#include <unity.h>

#include <cstring>

#include "CardBreaker.h"

using namespace cardbreaker;

void setUp() {}
void tearDown() {}

static Word garbage() {
  Word w;
  std::memset(&w, 0xA7, sizeof(w));
  return w;
}

void test_a_power_on_says_nothing() {
  Word w = garbage();
  uint32_t found = 7;
  TEST_ASSERT_FALSE(atBoot(w, false, &found));
  TEST_ASSERT_EQUAL_UINT32(0, found);
  TEST_ASSERT_TRUE(valid(w));
  // RTC memory's noise after a restart too.
  w = garbage();
  TEST_ASSERT_FALSE(atBoot(w, true, &found));
  TEST_ASSERT_TRUE(valid(w));
}

void test_the_second_restart_in_a_row_holds_that_boot_alone() {
  Word w = garbage();
  atBoot(w, false);
  noteRestart(w);
  uint32_t found = 0;
  TEST_ASSERT_FALSE(atBoot(w, true, &found));  // once: a re-seat's, the next boot as ever
  TEST_ASSERT_EQUAL_UINT32(1, found);
  noteRestart(w);
  TEST_ASSERT_TRUE(atBoot(w, true, &found));
  TEST_ASSERT_EQUAL_UINT32(2, found);
  TEST_ASSERT_FALSE(atBoot(w, true, &found));
  TEST_ASSERT_EQUAL_UINT32(0, found);
}

void test_a_power_on_a_calm_session_or_another_card_starts_it_again() {
  Word w = garbage();
  atBoot(w, false);
  noteRestart(w);
  atBoot(w, true);
  clear(w);  // a calm session, or another card with its identity read
  noteRestart(w);
  TEST_ASSERT_FALSE(atBoot(w, true));
  noteRestart(w);
  TEST_ASSERT_FALSE(atBoot(w, false));  // a power-on, or a reset that isn't esp_restart()'s
  noteRestart(w);
  TEST_ASSERT_FALSE(atBoot(w, true));
}

void test_any_bit_flipped_is_no_word() {
  for (int word = 0; word < 3; ++word) {
    for (int bit = 0; bit < 32; ++bit) {
      Word w = garbage();
      atBoot(w, false);
      noteRestart(w);
      noteRestart(w);
      reinterpret_cast<uint32_t*>(&w)[word] ^= 1u << bit;
      TEST_ASSERT_FALSE(atBoot(w, true));
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_power_on_says_nothing);
  RUN_TEST(test_the_second_restart_in_a_row_holds_that_boot_alone);
  RUN_TEST(test_a_power_on_a_calm_session_or_another_card_starts_it_again);
  RUN_TEST(test_any_bit_flipped_is_no_word);
  return UNITY_END();
}
