// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for DancePanels: which of the Dance tab's panels are drawn
// (ui/DancePage), and that a panel not drawn keeps its old value, so what
// changed under a toast's end still reaches the screen.
// Run: pio test -e native -f test_dance_panels
#include <unity.h>

#include "DancePanels.h"

using dancepanels::Draw;
using dancepanels::plan;
using dancepanels::Values;

namespace {

Values state() {
  Values v;
  v.bpmX10 = 1200;
  v.lock = 2;
  v.conf = 80;
  v.skin = 0;
  v.track = 7;
  v.progress = 100;
  v.host = 0;
  return v;
}

int count(const Draw& d) {
  return d.beat + d.lock + d.conf + d.dancer + d.track + d.progress;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_full_repaint_draws_every_panel_and_remembers_them() {
  Values shown;
  const Values now = state();
  TEST_ASSERT_EQUAL_INT(6, count(plan(shown, now, true)));
  TEST_ASSERT_EQUAL_INT(0, count(plan(shown, now, false)));
}

void test_a_pass_draws_only_what_changed() {
  Values shown;
  const Values first = state();
  plan(shown, first, true);

  Values now = first;
  now.lock = 1;
  Draw d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.lock);
  TEST_ASSERT_EQUAL_INT(1, count(d));

  now.conf = 60;
  d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.conf);
  TEST_ASSERT_EQUAL_INT(1, count(d));

  now.skin = 1;
  d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.dancer);
  TEST_ASSERT_EQUAL_INT(1, count(d));

  now.track = 8;
  d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.track);
  TEST_ASSERT_EQUAL_INT(1, count(d));

  now.progress = 101;
  d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.progress);
  TEST_ASSERT_EQUAL_INT(1, count(d));
}

void test_the_beat_panel_follows_whole_tens_of_tenths() {
  Values shown;
  Values now = state();
  plan(shown, now, true);
  now.bpmX10 = 1209;
  TEST_ASSERT_FALSE(plan(shown, now, false).beat);
  now.bpmX10 = 1210;
  TEST_ASSERT_TRUE(plan(shown, now, false).beat);
}

void test_a_computer_taking_over_draws_the_bottom_and_the_line() {
  Values shown;
  Values now = state();
  plan(shown, now, true);
  now.host = 1;
  const Draw d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.track);
  TEST_ASSERT_TRUE(d.progress);
  TEST_ASSERT_EQUAL_INT(2, count(d));
}

void test_a_toasts_end_draws_the_top_panels_alone() {
  Values shown;
  const Values now = state();
  plan(shown, now, true);
  const Draw d = plan(shown, now, true, true);
  TEST_ASSERT_TRUE(d.beat);
  TEST_ASSERT_TRUE(d.dancer);
  TEST_ASSERT_EQUAL_INT(2, count(d));
}

// What this suite is for: a new track falls between two passes, a toast ends in
// that gap, and the next pass must still draw the title.
void test_a_track_change_under_a_toasts_end_is_drawn_at_the_next_pass() {
  Values shown;
  Values now = state();
  plan(shown, now, true);
  now.track = 8;
  TEST_ASSERT_FALSE(plan(shown, now, true, true).track);
  const Draw d = plan(shown, now, false);
  TEST_ASSERT_TRUE(d.track);
  TEST_ASSERT_EQUAL_INT(1, count(d));
  TEST_ASSERT_EQUAL_INT(0, count(plan(shown, now, false)));  // and once
}

void test_each_panel_below_a_toasts_end_is_drawn_at_the_next_pass() {
  {
    Values shown;
    Values now = state();
    plan(shown, now, true);
    now.lock = 1;
    plan(shown, now, true, true);
    TEST_ASSERT_TRUE(plan(shown, now, false).lock);
  }
  {
    Values shown;
    Values now = state();
    plan(shown, now, true);
    now.conf = 10;
    plan(shown, now, true, true);
    TEST_ASSERT_TRUE(plan(shown, now, false).conf);
  }
  {
    Values shown;
    Values now = state();
    plan(shown, now, true);
    now.progress = 200;
    plan(shown, now, true, true);
    TEST_ASSERT_TRUE(plan(shown, now, false).progress);
  }
  {
    Values shown;
    Values now = state();
    plan(shown, now, true);
    now.host = 1;
    plan(shown, now, true, true);
    const Draw d = plan(shown, now, false);
    TEST_ASSERT_TRUE(d.track);
    TEST_ASSERT_TRUE(d.progress);
  }
}

// The top panels are drawn either way: nothing is owed for them.
void test_a_toasts_end_settles_the_panels_it_draws() {
  Values shown;
  Values now = state();
  plan(shown, now, true);
  now.bpmX10 = 1300;
  now.skin = 1;
  plan(shown, now, true, true);
  TEST_ASSERT_EQUAL_INT(0, count(plan(shown, now, false)));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_full_repaint_draws_every_panel_and_remembers_them);
  RUN_TEST(test_a_pass_draws_only_what_changed);
  RUN_TEST(test_the_beat_panel_follows_whole_tens_of_tenths);
  RUN_TEST(test_a_computer_taking_over_draws_the_bottom_and_the_line);
  RUN_TEST(test_a_toasts_end_draws_the_top_panels_alone);
  RUN_TEST(test_a_track_change_under_a_toasts_end_is_drawn_at_the_next_pass);
  RUN_TEST(test_each_panel_below_a_toasts_end_is_drawn_at_the_next_pass);
  RUN_TEST(test_a_toasts_end_settles_the_panels_it_draws);
  return UNITY_END();
}
