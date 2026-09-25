// Host unit tests for TransportSync. Run: pio test -e native
#include <unity.h>

#include "TransportSync.h"

void setUp() {}
void tearDown() {}

void test_starts_idle_at_generation_zero() {
  TransportSync s;
  TEST_ASSERT_EQUAL_UINT32(0, s.generation());
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)s.phase());
}

void test_post_bumps_generation_and_is_pending() {
  TransportSync s;
  const uint32_t g1 = s.post();
  const uint32_t g2 = s.post();
  TEST_ASSERT_EQUAL_UINT32(g1 + 1, g2);
  TEST_ASSERT_EQUAL_UINT32(g2, s.generation());
  TEST_ASSERT_EQUAL_INT((int)Phase::Pending, (int)s.phase());
}

void test_report_for_latest_generation_updates_phase() {
  TransportSync s;
  const uint32_t g = s.post();
  TEST_ASSERT_TRUE(s.report(g, Phase::Decoding));
  TEST_ASSERT_EQUAL_INT((int)Phase::Decoding, (int)s.phase());
  TEST_ASSERT_TRUE(s.report(g, Phase::Ended));
  TEST_ASSERT_EQUAL_INT((int)Phase::Ended, (int)s.phase());
}

void test_stale_report_is_ignored() {
  // The old track ends just after the next one was requested: that "Ended"
  // must not count as the new track finishing.
  TransportSync s;
  const uint32_t oldGen = s.post();
  s.report(oldGen, Phase::Decoding);
  const uint32_t newGen = s.post();
  TEST_ASSERT_FALSE(s.report(oldGen, Phase::Ended));
  TEST_ASSERT_EQUAL_UINT32(newGen, s.generation());
  TEST_ASSERT_EQUAL_INT((int)Phase::Pending, (int)s.phase());
}

void test_post_with_initial_phase_for_stop() {
  TransportSync s;
  const uint32_t g = s.post();
  s.report(g, Phase::Decoding);
  s.post(Phase::Idle);  // stop()
  TEST_ASSERT_EQUAL_INT((int)Phase::Idle, (int)s.phase());
  TEST_ASSERT_FALSE(s.report(g, Phase::Ended));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_idle_at_generation_zero);
  RUN_TEST(test_post_bumps_generation_and_is_pending);
  RUN_TEST(test_report_for_latest_generation_updates_phase);
  RUN_TEST(test_stale_report_is_ignored);
  RUN_TEST(test_post_with_initial_phase_for_stop);
  return UNITY_END();
}
