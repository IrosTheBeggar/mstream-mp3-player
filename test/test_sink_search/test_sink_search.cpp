// Host tests for SinkSearch: when the Core2 may scan for headphones by
// itself, and which device a scan may take. Pairing is the Pair screen's;
// nothing is ever taken by signal strength alone (a TV in the next room
// was, once), but for the console's one-scan Bs.
// Run: pio test -e native
#include <unity.h>

#include "SinkSearch.h"

using namespace sinksearch;

void setUp() {}
void tearDown() {}

namespace {
Setup release() { return Setup{}; }
Setup dev(const char* name = "SPYDRONE") {
  Setup s;
  s.name = name;
  return s;
}
}  // namespace

// A release build (no name): no scan, remembered or not.
void test_a_release_build_never_scans() {
  TEST_ASSERT_FALSE(mayScan(false, release()));
  TEST_ASSERT_FALSE(mayScan(true, release()));
  Setup forgot = release();
  forgot.forgotForGood = true;
  TEST_ASSERT_FALSE(mayScan(false, forgot));
  // An empty name is no name.
  TEST_ASSERT_FALSE(mayScan(false, dev("")));
  Setup nul;
  nul.name = nullptr;
  TEST_ASSERT_FALSE(mayScan(false, nul));
}

// A developer build scans by its name while none are remembered; never
// after Forget; never while any are remembered.
void test_a_named_build_scans_only_with_none_remembered() {
  TEST_ASSERT_TRUE(mayScan(false, dev()));
  TEST_ASSERT_FALSE(mayScan(true, dev()));
  Setup forgot = dev();
  forgot.forgotForGood = true;
  TEST_ASSERT_FALSE(mayScan(false, forgot));
}

// Bs: one scan, even in a release build or after Forget; never while
// headphones are remembered.
void test_bs_allows_a_scan_with_none_remembered() {
  Setup bs = release();
  bs.bySignal = true;
  TEST_ASSERT_TRUE(mayScan(false, bs));
  TEST_ASSERT_FALSE(mayScan(true, bs));
  bs.forgotForGood = true;
  TEST_ASSERT_TRUE(mayScan(false, bs));
}

// Found by a scan, a release build takes nothing: the TV in the next room
// at -40 dBm, headphones on the desk at -30.
void test_nothing_is_taken_by_signal() {
  TEST_ASSERT_EQUAL(Verdict::NoName, judge(release(), "Living Room TV", -40));
  TEST_ASSERT_EQUAL(Verdict::NoName, judge(release(), "WH-1000XM4", -30));
  TEST_ASSERT_EQUAL(Verdict::NoName, judge(release(), "", 0));
  TEST_ASSERT_EQUAL(Verdict::NoName, judge(release(), nullptr, 0));
  TEST_ASSERT_FALSE(taken(judge(release(), "WH-1000XM4", -30)));
  // A named build: a strong signal with another name isn't taken either.
  TEST_ASSERT_EQUAL(Verdict::OtherName, judge(dev(), "Living Room TV", -30));
}

// By name: contained, case-insensitive; not after Forget.
void test_taken_by_name() {
  TEST_ASSERT_EQUAL(Verdict::ByName, judge(dev(), "SPYDRONE", -80));
  TEST_ASSERT_EQUAL(Verdict::ByName, judge(dev(), "My spydrone 2", -90));
  TEST_ASSERT_EQUAL(Verdict::OtherName, judge(dev(), "SPYDRON", -30));
  Setup forgot = dev();
  forgot.forgotForGood = true;
  TEST_ASSERT_EQUAL(Verdict::Forgotten, judge(forgot, "SPYDRONE", -30));
  TEST_ASSERT_EQUAL(Verdict::OtherName, judge(forgot, "Other", -30));
  TEST_ASSERT_FALSE(taken(judge(forgot, "SPYDRONE", -30)));
}

// Bs: at kMinRssi or closer, whatever the name; farther isn't; a name
// match still counts.
void test_bs_takes_only_a_device_this_close() {
  Setup bs = release();
  bs.bySignal = true;
  TEST_ASSERT_EQUAL(Verdict::BySignal, judge(bs, "Anything", kMinRssi));
  TEST_ASSERT_EQUAL(Verdict::BySignal, judge(bs, "", -20));
  TEST_ASSERT_EQUAL(Verdict::TooFar, judge(bs, "Living Room TV", kMinRssi - 1));
  Setup both = dev();
  both.bySignal = true;
  TEST_ASSERT_EQUAL(Verdict::ByName, judge(both, "SPYDRONE", -90));
  TEST_ASSERT_EQUAL(Verdict::TooFar, judge(both, "Living Room TV", -70));
}

// Every refusal says why (the log); a take says nothing.
void test_every_verdict_has_its_reason() {
  const Verdict refused[] = {Verdict::NoName, Verdict::OtherName, Verdict::Forgotten, Verdict::TooFar};
  for (Verdict v : refused) {
    TEST_ASSERT_FALSE(taken(v));
    TEST_ASSERT_TRUE(whyNot(v)[0] != '\0');
  }
  TEST_ASSERT_EQUAL_STRING("", whyNot(Verdict::ByName));
  TEST_ASSERT_EQUAL_STRING("", whyNot(Verdict::BySignal));
}

void test_contains_ignore_case() {
  TEST_ASSERT_TRUE(containsIgnoreCase("Sony WH-1000XM4", "wh-1000"));
  TEST_ASSERT_TRUE(containsIgnoreCase("abc", "ABC"));
  TEST_ASSERT_FALSE(containsIgnoreCase("ab", "abc"));
  TEST_ASSERT_FALSE(containsIgnoreCase("abc", ""));
  TEST_ASSERT_FALSE(containsIgnoreCase("", "a"));
  TEST_ASSERT_FALSE(containsIgnoreCase(nullptr, "a"));
  TEST_ASSERT_FALSE(containsIgnoreCase("a", nullptr));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_release_build_never_scans);
  RUN_TEST(test_a_named_build_scans_only_with_none_remembered);
  RUN_TEST(test_bs_allows_a_scan_with_none_remembered);
  RUN_TEST(test_nothing_is_taken_by_signal);
  RUN_TEST(test_taken_by_name);
  RUN_TEST(test_bs_takes_only_a_device_this_close);
  RUN_TEST(test_every_verdict_has_its_reason);
  RUN_TEST(test_contains_ignore_case);
  return UNITY_END();
}
