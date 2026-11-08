// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for what a failed open of the AutoDJ index comes to
// (lib/core/AutoDjOpen.h, docs/AUTODJ.md 6.6): what is the file's say and what
// is the card's, each try and its wait, when one falls due (a pass's older now,
// millis()'s wrap), a new count from outside; and, over the real reader and the
// golden index, that a read that fails at any point of an open is the card's
// fault and never reads as the file's, and a bad file the other way about. Run:
// pio test -e native
//
// And what a remount comes to after a fault of the card: that fault's echo, or news.
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

// ---- a remount ----

void test_a_remount_with_no_fault_asks_at_once_from_the_first_try() {
  OpenTries t;
  TEST_ASSERT_TRUE(t.remounted(0));
  // The last try the file's own word: the card answered it.
  for (OpenTry last : {OpenTry::NotThere, OpenTry::Checks}) {
    TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 0));
    TEST_ASSERT_FALSE(t.failed(last, 2000));
    TEST_ASSERT_EQUAL_UINT8(1, t.used());
    TEST_ASSERT_TRUE(t.remounted(2000));
    TEST_ASSERT_EQUAL_UINT8(0, t.used());
  }
  // Or one that worked.
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, 0));
  t.reset();
  TEST_ASSERT_TRUE(t.remounted(1));
}

void test_a_remount_soon_after_a_fault_is_its_echo_and_the_wait_stands() {
  TEST_ASSERT_EQUAL_UINT32(30000, OpenTries::kEchoMs);
  OpenTries t;
  const uint32_t at = 100000;
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, at));
  TEST_ASSERT_FALSE(t.remounted(at - 5));  // (a pass's now, older than the try's)
  TEST_ASSERT_FALSE(t.remounted(at));
  TEST_ASSERT_FALSE(t.remounted(at + OpenTries::kEchoMs - 1));
  TEST_ASSERT_EQUAL_UINT8(1, t.used());
  TEST_ASSERT_FALSE(t.due(at + 1999));
  TEST_ASSERT_TRUE(t.due(at + 2000));
  // No longer an echo: it asks, the count where it was.
  TEST_ASSERT_TRUE(t.remounted(at + OpenTries::kEchoMs));
  TEST_ASSERT_EQUAL_UINT8(1, t.used());
  // The last try over too, that counts from the last failure: and it is the last.
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, at + 1));
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, at + 2));
  const uint32_t last = at + 3;
  TEST_ASSERT_FALSE(t.failed(OpenTry::Card, last));
  TEST_ASSERT_FALSE(t.remounted(last + OpenTries::kEchoMs - 1));
  TEST_ASSERT_TRUE(t.remounted(last + OpenTries::kEchoMs));
  TEST_ASSERT_FALSE(t.failed(OpenTry::Card, last + OpenTries::kEchoMs));
  // Across millis()'s wrap.
  OpenTries w;
  TEST_ASSERT_TRUE(w.failed(OpenTry::Card, 0xFFFFFF00u));
  TEST_ASSERT_FALSE(w.remounted(0x100));
  TEST_ASSERT_TRUE(w.remounted(0xFFFFFF00u + OpenTries::kEchoMs));
}

void test_a_remount_weeks_after_a_fault_asks() {
  OpenTries t;
  const uint32_t at = 5000;
  TEST_ASSERT_TRUE(t.failed(OpenTry::Card, at));
  TEST_ASSERT_TRUE(t.remounted(at + 0x7FFFFFFFu));  // (24.8 days: the largest age a signed compare reads right)
  TEST_ASSERT_TRUE(t.remounted(at + 0x80000000u));  // (1 ms on, and it reads as a long way before the try)
  TEST_ASSERT_TRUE(t.remounted(at + 0xFFFF0000u));  // (49.7 days, a minute short of the wrap)
  // At a full turn of the clock the age is small again, and an echo it can't be told from.
  TEST_ASSERT_FALSE(t.remounted(at + 0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_UINT8(1, t.used());
}

// Why it is so, on a model of the two readers of the root (app/AutoDj and app/Waves
// are device code). A read with no answer leaves the card failed, so FatFs's next
// call, whoever's, mounts it again and moves the counter both watch. With that taken
// for news, each read would bring the next at once: hundreds in ten minutes, seconds
// of the loop each. And a reader makes that call itself after its read, or a call
// coming later than the window would read as news: the next read, and its mark for
// the next call, without end, on a player where the calls come far apart.
struct RootReader {
  OpenTries tries;
  uint32_t seen = 0;
  bool again = false;
  uint32_t reads = 0;
};

struct Silent {
  uint32_t remounts = 0;
  bool markedFailed = true;  // (the boot's reads left it so)
  uint32_t readMs = 0;       // (what a read costs: a card that doesn't answer takes seconds)
  void call() {
    if (markedFailed) ++remounts;
    markedFailed = false;
  }
  void readFailed() {
    call();
    markedFailed = true;
  }
  void pass(RootReader& r, uint32_t& nowMs) {
    const bool moved = remounts != r.seen;
    r.seen = remounts;
    const bool news = moved && r.tries.remounted(nowMs);
    const bool due = !moved && r.again && r.tries.due(nowMs);
    if (!news && !due) return;
    readFailed();
    nowMs += readMs;
    call();  // (FatFs::echoNow())
    ++r.reads;
    r.again = r.tries.failed(OpenTry::Card, nowMs);
  }
};

void test_two_readers_of_a_root_that_never_answers_stay_on_their_waits() {
  // Another caller of FatFs (a decoder's read, a saved queue): each pass, far apart
  // however late each comes after the last read, or none at all.
  for (uint32_t everyMs : {0u, 20u, 5000u, 33000u, 45000u, 60000u, 120000u}) {
    for (uint32_t readMs : {0u, 800u, 1600u, 2800u, 4000u}) {
      Silent card;
      card.readMs = readMs;
      RootReader dj, wave;
      dj.again = dj.tries.failed(OpenTry::Card, 0);
      wave.again = wave.tries.failed(OpenTry::Card, 0);
      const uint32_t kPassMs = 20;
      uint32_t now = 0, nextCall = everyMs;
      const auto run = [&](uint32_t end) {
        for (; now < end; now += kPassMs) {
          if (everyMs && now >= nextCall) {
            card.call();
            nextCall += everyMs;
          }
          card.pass(dj, now);
          card.pass(wave, now);
        }
      };
      run(600000);
      char msg[80];
      std::snprintf(msg, sizeof(msg), "a call each %lu ms, a read of %lu ms", (unsigned long)everyMs, (unsigned long)readMs);
      // The boot's, then 2 s, 12 s and 42 s on: three each, and quiet.
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, dj.reads, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, wave.reads, msg);
      TEST_ASSERT_FALSE(dj.again);
      TEST_ASSERT_FALSE(wave.again);
      // A remount long after: the card may be back. One read each, and quiet again.
      ++card.remounts;
      run(now + 300000);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, dj.reads, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, wave.reads, msg);
    }
  }
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
  RUN_TEST(test_a_remount_with_no_fault_asks_at_once_from_the_first_try);
  RUN_TEST(test_a_remount_soon_after_a_fault_is_its_echo_and_the_wait_stands);
  RUN_TEST(test_a_remount_weeks_after_a_fault_asks);
  RUN_TEST(test_two_readers_of_a_root_that_never_answers_stay_on_their_waits);
  RUN_TEST(test_a_pass_older_than_the_try_isnt_due);
  RUN_TEST(test_the_wait_holds_across_millis_wrap);
  RUN_TEST(test_a_failed_read_at_any_point_of_an_open_is_the_cards_fault);
  RUN_TEST(test_a_bad_file_is_the_files_fault_and_never_the_cards);
  return UNITY_END();
}
