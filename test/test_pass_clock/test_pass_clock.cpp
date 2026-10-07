// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for PassClock, the time rule of a decode pass (docs/OPUS.md
// section 8.2): the first step of a pass always runs, the next only while
// the pass's time plus the last step's fits the budget, exactly the budget
// fits, and a step longer than the budget ends its pass without ending
// every pass after it (the M2 review's stall). The loop is modelled as
// OpusGenerator::loop() drives the clock, on a fake clock the steps
// advance.
// Run: pio test -e native -f test_pass_clock
#include <unity.h>

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "PassClock.h"

namespace {
constexpr uint32_t kBudget = 15000;

// One pass as the generator runs it: steps of the given lengths offered in
// order from `next`, each taken only while the clock says it fits. Returns
// how many it took; `now` moves with them (and by `restUs` after the pass,
// the decode task's rest between passes).
uint32_t pass(PassClock& c, int64_t& now, const std::vector<uint32_t>& steps, size_t& next, uint32_t restUs = 1000) {
  c.begin(now);
  uint32_t took = 0;
  while (next < steps.size()) {
    if (!c.fits(now)) break;
    const int64_t s0 = now;
    now += steps[next++];
    c.stepped(s0, now);
    ++took;
  }
  now += restUs;
  return took;
}
}  // namespace

void setUp() {}
void tearDown() {}

// A track's first step runs with no last step; then the rule: 7 ms used
// with a 7 ms last step fits (14 <= 15), a second 7 ms step makes 14 used
// and the next would end at 21: the pass ends at two steps.
void test_steps_fit_while_the_pass_plus_the_last_step_fit() {
  PassClock c(kBudget);
  int64_t now = 1000000;
  c.begin(now);
  TEST_ASSERT_TRUE(c.fits(now));
  TEST_ASSERT_EQUAL_UINT32(0, c.steps());
  c.stepped(now, now + 7000);
  now += 7000;
  TEST_ASSERT_EQUAL_UINT32(7000, c.lastStepUs());
  TEST_ASSERT_TRUE(c.fits(now));  // 7,000 + 7,000
  c.stepped(now, now + 7000);
  now += 7000;
  TEST_ASSERT_FALSE(c.fits(now));  // 14,000 + 7,000
  TEST_ASSERT_EQUAL_UINT32(2, c.steps());
  TEST_ASSERT_EQUAL_UINT32(7000, c.maxStepUs());
}

// Exactly the budget fits; one microsecond over doesn't.
void test_exactly_the_budget_fits() {
  PassClock c(kBudget);
  int64_t now = 50;
  c.begin(now);
  c.stepped(now, now + 8000);
  now += 8000;
  // The pass has used 8,000 and the last step was 8,000: 16,000, over.
  TEST_ASSERT_FALSE(c.fits(now));
  // A shorter last step: 8,000 used + 7,000 = 15,000, exactly the budget.
  c.begin(now);
  c.stepped(now, now + 1000);
  now += 1000;
  c.stepped(now, now + 7000);
  now += 7000;
  TEST_ASSERT_TRUE(c.fits(now));
  TEST_ASSERT_FALSE(c.fits(now + 1));
}

// The first step of a pass runs whatever the last step took. The review's
// case: steps of 7, 9 and 16.5 ms, then a hundred passes of 7 ms steps.
// The 16.5 ms step ends its pass; every pass after it still takes a step
// (one at first, since 16.5 ms carried over fills the budget by itself,
// then two once a 7 ms step is the last one again).
void test_a_step_over_the_budget_ends_its_pass_only() {
  PassClock c(kBudget);
  int64_t now = 0;
  std::vector<uint32_t> steps = {7000, 9000, 16500};
  for (int i = 0; i < 200; ++i) steps.push_back(7000);
  size_t next = 0;
  // Pass 1: 7 ms, then 7,000 + 7,000 fits: 9 ms; 16,000 + 9,000 doesn't.
  TEST_ASSERT_EQUAL_UINT32(2, pass(c, now, steps, next));
  // Pass 2: the first step always runs: the 16.5 ms one; then 16,500 +
  // 16,500 is over the budget.
  TEST_ASSERT_EQUAL_UINT32(1, pass(c, now, steps, next));
  TEST_ASSERT_EQUAL_UINT32(16500, c.maxStepUs());
  // Pass 3: the first step runs (the stall would be here: 0 + 16,500 is
  // over the budget, and the old rule asked before stepping); it takes
  // 7 ms; then 7,000 + 7,000 fits: a second one; 14,000 + 7,000 doesn't.
  TEST_ASSERT_EQUAL_UINT32(2, pass(c, now, steps, next));
  // And on to the end, two 7 ms steps a pass: no pass takes nothing while
  // steps remain (198 left: 99 passes).
  uint32_t passes = 0;
  while (next < steps.size()) {
    TEST_ASSERT_TRUE(pass(c, now, steps, next) >= 1);
    ++passes;
  }
  TEST_ASSERT_EQUAL_UINT32(99, passes);
}

// Steps that are each over the budget (an SD card stalling 30 ms on every
// read, say): one a pass, every pass, until they are done.
void test_long_steps_go_one_a_pass() {
  PassClock c(kBudget);
  int64_t now = 123456789;
  const std::vector<uint32_t> steps(20, 30000);
  size_t next = 0;
  for (int i = 0; i < 20; ++i) TEST_ASSERT_EQUAL_UINT32(1, pass(c, now, steps, next));
  TEST_ASSERT_EQUAL_UINT32(20, next);
  TEST_ASSERT_EQUAL_UINT32(30000, c.maxStepUs());
}

// A new track forgets the last step and the longest one: its first pass
// behaves as a fresh clock's.
void test_reset_forgets_the_last_step() {
  PassClock c(kBudget);
  int64_t now = 0;
  c.begin(now);
  c.stepped(now, now + 20000);
  now += 20000;
  TEST_ASSERT_EQUAL_UINT32(20000, c.maxStepUs());
  c.reset();
  TEST_ASSERT_EQUAL_UINT32(0, c.lastStepUs());
  TEST_ASSERT_EQUAL_UINT32(0, c.maxStepUs());
  TEST_ASSERT_EQUAL_UINT32(0, c.steps());
  c.begin(now);
  c.stepped(now, now + 1000);
  now += 1000;
  TEST_ASSERT_TRUE(c.fits(now));  // 1,000 + 1,000: the 20 ms step is gone
}

// The clock's origin doesn't matter: a pass that begins near the top of
// the counter still fits its steps (the differences are what count).
void test_the_origin_is_the_callers() {
  for (const int64_t t0 : {int64_t(0), int64_t(-5000), int64_t(1) << 40}) {
    PassClock c(kBudget);
    int64_t now = t0;
    c.begin(now);
    c.stepped(now, now + 6000);
    now += 6000;
    TEST_ASSERT_TRUE(c.fits(now));
    c.stepped(now, now + 6000);
    now += 6000;
    TEST_ASSERT_FALSE(c.fits(now));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_steps_fit_while_the_pass_plus_the_last_step_fit);
  RUN_TEST(test_exactly_the_budget_fits);
  RUN_TEST(test_a_step_over_the_budget_ends_its_pass_only);
  RUN_TEST(test_long_steps_go_one_a_pass);
  RUN_TEST(test_reset_forgets_the_last_step);
  RUN_TEST(test_the_origin_is_the_callers);
  return UNITY_END();
}
