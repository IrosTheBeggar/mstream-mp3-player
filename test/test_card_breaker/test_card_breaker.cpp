// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar
// Host tests of the card's breaker (lib/core/CardBreaker.h) and the guard's restart waiting for
// the queue's flush (lib/core/RestartWait.h).
#include <unity.h>

#include <cstring>

#include "CardBreaker.h"
#include "RestartWait.h"

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

// ---- the guard's restart, waiting for the queue's flush (RestartWait) ----

using Do = RestartWait::Do;

// Nothing but Do::Nothing from `from` to before `to`, a pass every 10 ms.
static void quietUntil(RestartWait& w, uint32_t from, uint32_t to, bool canFlush = true) {
  for (uint32_t t = from; t != to; t += 10) TEST_ASSERT_EQUAL_INT((int)Do::Nothing, (int)w.step(t, canFlush));
}

// The guard's flush worked: the restart 1.2 s later, as ever, and no flush asked.
void test_a_flushed_restart_comes_as_before() {
  RestartWait w;
  TEST_ASSERT_EQUAL_INT((int)Do::Nothing, (int)w.step(0, true));  // (not begun)
  w.begin(5000, true, RestartWait::kMaxMs);
  TEST_ASSERT_TRUE(w.active());
  TEST_ASSERT_TRUE(w.decided());
  TEST_ASSERT_TRUE(w.flushed());
  quietUntil(w, 5000, 6200);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(6200, true));
  TEST_ASSERT_FALSE(w.active());
  TEST_ASSERT_EQUAL_INT((int)Do::Nothing, (int)w.step(6210, true));
  TEST_ASSERT_EQUAL_UINT32(1, w.tries());
}

// Not flushed: tried again on the saver's own wait (10 s after its first failure). The one that
// works: the restart 1.2 s after it, the toast said again first (the guard's has gone by then).
void test_the_restart_waits_for_a_flush_that_works() {
  RestartWait w;
  w.begin(0, false, 10000);
  TEST_ASSERT_FALSE(w.decided());
  TEST_ASSERT_EQUAL_UINT32(10000, w.nextTryInMs(0));
  quietUntil(w, 0, 10000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(10000, true));
  w.tried(10500, true, 10000);  // (the flush took half a second)
  TEST_ASSERT_TRUE(w.decided());
  TEST_ASSERT_TRUE(w.flushed());
  TEST_ASSERT_EQUAL_INT((int)Do::Announce, (int)w.step(10500, true));
  quietUntil(w, 10500, 11700);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(11700, true));
  TEST_ASSERT_EQUAL_UINT32(2, w.tries());
}

// A card that stays silent: flushes at 10 s and 30 s (the saver's 10 s, then its 20 s, the last at
// the bound), then the restart whatever that last one did.
void test_the_restart_comes_at_its_bound_unflushed() {
  RestartWait w;
  w.begin(0, false, 10000);
  quietUntil(w, 0, 10000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(10000, true));
  w.tried(10000, false, 20000);
  TEST_ASSERT_EQUAL_UINT32(20000, w.nextTryInMs(10000));
  quietUntil(w, 10000, 30000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(30000, true));
  w.tried(31000, false, 40000);
  TEST_ASSERT_TRUE(w.decided());
  TEST_ASSERT_FALSE(w.flushed());
  TEST_ASSERT_EQUAL_INT((int)Do::Announce, (int)w.step(31000, true));
  quietUntil(w, 31000, 32200);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(32200, true));
  TEST_ASSERT_EQUAL_UINT32(3, w.tries());
}

// The saver's wait long already (its failures before the card came back): one last flush, at the
// bound; a wait that would pass it is cut to it.
void test_a_saver_wait_past_the_bound_is_cut_to_it() {
  RestartWait w;
  w.begin(0, false, 160000);
  TEST_ASSERT_EQUAL_UINT32(RestartWait::kMaxMs, w.nextTryInMs(0));
  quietUntil(w, 0, RestartWait::kMaxMs);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(RestartWait::kMaxMs, true));
  w.tried(RestartWait::kMaxMs, false, 300000);
  TEST_ASSERT_TRUE(w.decided());
  RestartWait v;
  v.begin(0, false, 10000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)v.step(10000, true));
  v.tried(25000, false, 20000);  // (a slow failure: 15 s on the loop)
  TEST_ASSERT_EQUAL_UINT32(5000, v.nextTryInMs(25000));
}

// No flush can work (writes off, another card, the queue in PSRAM): the restart as before, 1.2 s
// after the guard's decision; or, found later, at once with the toast again.
void test_no_flush_that_can_work_restarts_as_before() {
  RestartWait w;
  w.begin(0, false, 10000);
  quietUntil(w, 0, 1200, false);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(1200, false));
  TEST_ASSERT_EQUAL_UINT32(1, w.tries());
  RestartWait v;
  v.begin(0, false, 10000);
  quietUntil(v, 0, 5000);
  TEST_ASSERT_EQUAL_INT((int)Do::Announce, (int)v.step(5000, false));  // writes went off meanwhile
  quietUntil(v, 5000, 6200, false);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)v.step(6200, false));
}

// A flush that works while the guard's toast still shows: no second toast, and never sooner than
// 1.2 s after the guard's decision.
void test_a_quick_flush_keeps_the_guards_toast() {
  RestartWait w;
  w.begin(0, false, 500);
  quietUntil(w, 0, 500);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(500, true));
  w.tried(600, true, 10000);
  quietUntil(w, 600, 1200);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(1200, true));
  RestartWait v;
  v.begin(0, false, 2000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)v.step(2000, true));
  v.tried(2900, true, 10000);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)v.step(2900, true));
  v.tried(3000, false, 10000);  // (after it: nothing)
  TEST_ASSERT_FALSE(v.active());
}

// The millisecond clock wraps under it.
void test_the_restart_wait_across_the_clocks_wrap() {
  const uint32_t t0 = 0xFFFFF000u;  // 4 s before the wrap
  RestartWait w;
  w.begin(t0, false, 10000);
  quietUntil(w, t0, t0 + 10000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(t0 + 10000, true));
  w.tried(t0 + 10000, false, 20000);
  quietUntil(w, t0 + 10000, t0 + 30000);
  TEST_ASSERT_EQUAL_INT((int)Do::Flush, (int)w.step(t0 + 30000, true));
  w.tried(t0 + 30000, false, 40000);
  TEST_ASSERT_EQUAL_INT((int)Do::Announce, (int)w.step(t0 + 30000, true));
  quietUntil(w, t0 + 30000, t0 + 31200);
  TEST_ASSERT_EQUAL_INT((int)Do::Restart, (int)w.step(t0 + 31200, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_power_on_says_nothing);
  RUN_TEST(test_the_second_restart_in_a_row_holds_that_boot_alone);
  RUN_TEST(test_a_power_on_a_calm_session_or_another_card_starts_it_again);
  RUN_TEST(test_any_bit_flipped_is_no_word);
  RUN_TEST(test_a_flushed_restart_comes_as_before);
  RUN_TEST(test_the_restart_waits_for_a_flush_that_works);
  RUN_TEST(test_the_restart_comes_at_its_bound_unflushed);
  RUN_TEST(test_a_saver_wait_past_the_bound_is_cut_to_it);
  RUN_TEST(test_no_flush_that_can_work_restarts_as_before);
  RUN_TEST(test_a_quick_flush_keeps_the_guards_toast);
  RUN_TEST(test_the_restart_wait_across_the_clocks_wrap);
  return UNITY_END();
}
