// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for what a failed open of the AutoDJ index comes to
// (lib/core/AutoDjOpen.h, docs/AUTODJ.md 6.6): what is the file's say and what
// is the card's, each try and its wait, when one falls due (a pass's older now,
// millis()'s wrap), a new count from outside; and, over the real reader and the
// golden index, that a read that fails at any point of an open is the card's
// fault and never reads as the file's, and a bad file the other way about. Run:
// pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "AutoDjOpen.h"
#include "CardAutoDj.h"
#include "CardContainer.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using autodj::OpenTries;
using autodj::OpenTry;

void setUp() {}
void tearDown() {}

namespace {

std::vector<uint8_t> golden(const char* name) {
  const std::string path = std::string("test/fixtures/card/golden/") + name;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  std::vector<uint8_t> out;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// A file whose read number `failAt` (from 0) fails: -1, none.
class Faulty : public cc::Source {
public:
  Faulty(const std::vector<uint8_t>& bytes, int failAt) : bytes_(bytes), failAt_(failAt) {}
  uint32_t size() const override { return static_cast<uint32_t>(bytes_.size()); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (static_cast<int>(reads_++) == failAt_) return false;
    if (offset > bytes_.size() || n > bytes_.size() - offset) return false;
    std::copy(bytes_.begin() + offset, bytes_.begin() + offset + n, static_cast<uint8_t*>(out));
    return true;
  }
  uint32_t reads() const { return reads_; }

private:
  const std::vector<uint8_t>& bytes_;
  int failAt_;
  uint32_t reads_ = 0;
};

}  // namespace

// ---- what a try is ----

void test_the_verdict_of_a_file_that_opened() {
  TEST_ASSERT_TRUE(autodj::openTryOf(cc::Why::Ok) == OpenTry::Open);
  TEST_ASSERT_TRUE(autodj::openTryOf(cc::Why::Io) == OpenTry::Card);
  // Every check is the file's: the entry's, and those of its frame and header.
  for (cc::Why w : {cc::Why::Comp, cc::Why::Short, cc::Why::Magic, cc::Why::Major, cc::Why::HeaderBytes, cc::Why::Enum,
                    cc::Why::DjSelection, cc::Why::Missing}) {
    TEST_ASSERT_TRUE_MESSAGE(autodj::openTryOf(w) == OpenTry::Checks, cc::whyName(w));
  }
}

// ---- which failures are tried again ----

void test_a_file_that_isnt_there_is_off_at_once() {
  OpenTries t;
  TEST_ASSERT_FALSE(t.failed(OpenTry::NotThere, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, t.used());
}

void test_a_failed_check_is_off_at_once() {
  OpenTries t;
  TEST_ASSERT_FALSE(t.failed(OpenTry::Checks, 1000));
  TEST_ASSERT_EQUAL_UINT8(0, t.used());
}

void test_a_fault_of_the_card_is_tried_three_times_more() {
  TEST_ASSERT_EQUAL_UINT8(3, OpenTries::kAgain);
  OpenTries t;
  const uint32_t waits[] = {2000, 10000, 30000};
  uint32_t now = 5000;
  for (uint8_t i = 0; i < OpenTries::kAgain; ++i) {
    TEST_ASSERT_TRUE(t.failed(OpenTry::Card, now));
    TEST_ASSERT_EQUAL_UINT8(i + 1, t.used());
    TEST_ASSERT_EQUAL_UINT32(waits[i], t.lastWaitMs());
    TEST_ASSERT_FALSE(t.due(now + waits[i] - 1));
    TEST_ASSERT_TRUE(t.due(now + waits[i]));
    now += waits[i];
  }
  TEST_ASSERT_FALSE_MESSAGE(t.failed(OpenTry::Card, now), "the last one failed: off until a remount or a boot");
  TEST_ASSERT_EQUAL_UINT8(OpenTries::kAgain, t.used());
}

void test_a_try_again_that_finds_the_file_gone_or_bad_is_off_at_once() {
  // The first try the card's fault; the next says what it is.
  for (OpenTry next : {OpenTry::NotThere, OpenTry::Checks}) {
    OpenTries t;
    TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 0));
    TEST_ASSERT_FALSE(t.failed(next, 2000));
  }
}

void test_an_open_from_outside_counts_from_its_first_try() {
  OpenTries t;
  for (uint8_t i = 0; i < OpenTries::kAgain; ++i) TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 0));
  TEST_ASSERT_FALSE(t.failed(OpenTry::Card, 0));
  t.reset();
  TEST_ASSERT_EQUAL_UINT8(0, t.used());
  TEST_ASSERT_EQUAL_UINT32(0, t.lastWaitMs());
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 100));
  TEST_ASSERT_EQUAL_UINT32(2000, t.lastWaitMs());
}

// ---- when one falls due ----

void test_a_pass_older_than_the_try_isnt_due() {
  // A remount's open in this pass: the pass's now is a little older than the
  // millis() the try took, and the step after it must not read that as due.
  OpenTries t;
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 100000));
  TEST_ASSERT_FALSE(t.due(99995));
  TEST_ASSERT_FALSE(t.due(100000));
  TEST_ASSERT_TRUE(t.due(102000));
}

void test_the_wait_holds_across_millis_wrap() {
  OpenTries t;
  const uint32_t now = 0xFFFFFF00u;
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, now));
  // Before the wrap, its end after it: what a plain comparison reads as due.
  TEST_ASSERT_FALSE(t.due(now));
  TEST_ASSERT_FALSE(t.due(now + 255));
  TEST_ASSERT_FALSE(t.due(now + 1999));
  TEST_ASSERT_FALSE(t.due(0x100));  // (past the wrap, the wait not over)
  TEST_ASSERT_TRUE(t.due(now + 2000));
  TEST_ASSERT_TRUE(t.due(now + 9000));
}

// ---- the real reader ----

// Each read of a good file's open, failed in its turn: a fault of the card, never
// the file's; and the same file with no fault opens.
void openWithEachReadFailed(const char* name) {
  const std::vector<uint8_t> bytes = golden(name);
  Faulty clean(bytes, -1);
  mpdj::Table table;
  TEST_ASSERT_TRUE_MESSAGE(table.open(clean) == cc::Why::Ok, name);
  const uint32_t reads = clean.reads();
  TEST_ASSERT_GREATER_THAN_UINT32(0, reads);
  for (uint32_t k = 0; k < reads; ++k) {
    Faulty faulty(bytes, static_cast<int>(k));
    mpdj::Table t;
    const cc::Why w = t.open(faulty);
    char msg[80];
    std::snprintf(msg, sizeof(msg), "%s, read %lu of %lu: %s", name, (unsigned long)k, (unsigned long)reads,
                  cc::whyName(w));
    TEST_ASSERT_TRUE_MESSAGE(autodj::openTryOf(w) == OpenTry::Card, msg);
  }
}

void test_a_failed_read_at_any_point_of_an_open_is_the_cards_fault() {
  openWithEachReadFailed("autodj-00000029.bin");
  openWithEachReadFailed("autodj-tiny.bin");
}

// The other way about: every read answered, a bad file is the file's fault, so it
// goes off at once and isn't tried for 42 s. Cut at every length (a read past its
// end answers false, as FatFile::read does: it must not read as the card's), and
// each byte of its header and directory changed.
void test_a_bad_file_is_the_files_fault_and_never_the_cards() {
  const std::vector<uint8_t> whole = golden("autodj-tiny.bin");
  uint32_t checked = 0;
  for (size_t len = 0; len < whole.size(); ++len) {
    const std::vector<uint8_t> cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(len));
    Faulty src(cut, -1);
    mpdj::Table t;
    const cc::Why w = t.open(src);
    char msg[80];
    std::snprintf(msg, sizeof(msg), "cut to %lu bytes: %s", (unsigned long)len, cc::whyName(w));
    TEST_ASSERT_TRUE_MESSAGE(autodj::openTryOf(w) == OpenTry::Checks, msg);
    ++checked;
  }
  Faulty clean(whole, -1);
  mpdj::Table table;
  TEST_ASSERT_TRUE(table.open(clean) == cc::Why::Ok);
  const size_t head = std::min<size_t>(whole.size(), table.frame().headerBytes + 32u * table.frame().sectionCount);
  for (size_t at = 0; at < head; ++at) {
    std::vector<uint8_t> bad = whole;
    bad[at] ^= 0xFF;
    Faulty src(bad, -1);
    mpdj::Table t;
    const cc::Why w = t.open(src);
    char msg[80];
    std::snprintf(msg, sizeof(msg), "byte %lu changed: %s", (unsigned long)at, cc::whyName(w));
    TEST_ASSERT_TRUE_MESSAGE(autodj::openTryOf(w) == OpenTry::Checks, msg);
    ++checked;
  }
  TEST_ASSERT_GREATER_THAN_UINT32(100, checked);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_verdict_of_a_file_that_opened);
  RUN_TEST(test_a_file_that_isnt_there_is_off_at_once);
  RUN_TEST(test_a_failed_check_is_off_at_once);
  RUN_TEST(test_a_fault_of_the_card_is_tried_three_times_more);
  RUN_TEST(test_a_try_again_that_finds_the_file_gone_or_bad_is_off_at_once);
  RUN_TEST(test_an_open_from_outside_counts_from_its_first_try);
  RUN_TEST(test_a_pass_older_than_the_try_isnt_due);
  RUN_TEST(test_the_wait_holds_across_millis_wrap);
  RUN_TEST(test_a_failed_read_at_any_point_of_an_open_is_the_cards_fault);
  RUN_TEST(test_a_bad_file_is_the_files_fault_and_never_the_cards);
  return UNITY_END();
}
