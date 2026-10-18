// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for WallClock (docs/HOST-STATUS.md, "@time"): the calendar
// both ways (the RTC's fields and Unix ms), the times believed, what the
// boot makes of the RTC's reading, a @time, and the time at a moment of
// the boot.
// Run: pio test -e native -f test_wall_clock
#include <unity.h>

#include <cstring>

#include "WallClock.h"

using namespace wallclock;

namespace {

Civil civil(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  Civil c;
  c.year = y;
  c.month = mo;
  c.day = d;
  c.hour = h;
  c.minute = mi;
  c.second = s;
  return c;
}

uint64_t ms(const Civil& c) {
  uint64_t v = 0;
  TEST_ASSERT_TRUE(fromCivil(c, &v));
  return v;
}

// An RTC that answered, flag clear, saying `c`.
Reading rtc(const Civil& c) {
  Reading r;
  r.chip = true;
  r.read = true;
  r.civil = c;
  return r;
}

}  // namespace

void setUp() {}
void tearDown() {}

// Known days (the Unix seconds of each midnight, UTC) and their weekdays.
void test_known_days() {
  struct Day {
    int y, m, d;
    uint64_t seconds;
    int weekday;
  };
  const Day days[] = {
      {1970, 1, 1, 0, 4},              // a Thursday
      {2000, 1, 1, 946684800, 6},      // the fresh RTC's
      {2000, 2, 29, 951782400, 2},     // a leap day, in a century's year
      {2026, 1, 1, 1767225600, 4},     // kMinMs
      {2026, 10, 18, 1792281600, 0},   // a Sunday
      {2028, 2, 29, 1835395200, 2},
      {2038, 1, 19, 2147472000, 2},    // a signed 32-bit time_t's last day
      {2099, 12, 31, 4102358400, 4},
      {2100, 1, 1, 4102444800, 5},     // past the RTC's calendar
      {2106, 2, 7, 4294944000, 0},     // past an unsigned 32-bit second
  };
  for (const Day& d : days) {
    TEST_ASSERT_EQUAL_UINT64(d.seconds * 1000, ms(civil(d.y, d.m, d.d)));
    const Civil c = toCivil(d.seconds * 1000);
    TEST_ASSERT_EQUAL_INT(d.y, c.year);
    TEST_ASSERT_EQUAL_INT(d.m, c.month);
    TEST_ASSERT_EQUAL_INT(d.d, c.day);
    TEST_ASSERT_EQUAL_INT(0, c.hour + c.minute + c.second);
    TEST_ASSERT_EQUAL_INT(d.weekday, c.weekday);
  }
}

// A time of day, the ms dropped (the second it falls in), and the text.
void test_time_of_day() {
  const Civil c = toCivil(1792332202123ull);  // 2026-10-18 14:03:22.123
  TEST_ASSERT_EQUAL_INT(2026, c.year);
  TEST_ASSERT_EQUAL_INT(14, c.hour);
  TEST_ASSERT_EQUAL_INT(3, c.minute);
  TEST_ASSERT_EQUAL_INT(22, c.second);
  TEST_ASSERT_EQUAL_UINT64(1792332202000ull, ms(c));
  char buf[24];
  text(c, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("2026-10-18 14:03:22", buf);
  char small[11];
  text(c, small, sizeof(small));
  TEST_ASSERT_EQUAL_STRING("2026-10-18", small);
  // The last ms of a day, and of the range.
  const Civil e = toCivil(kMaxMs);
  TEST_ASSERT_EQUAL_INT(2099, e.year);
  TEST_ASSERT_EQUAL_INT(12, e.month);
  TEST_ASSERT_EQUAL_INT(31, e.day);
  TEST_ASSERT_EQUAL_INT(23, e.hour);
  TEST_ASSERT_EQUAL_INT(59, e.minute);
  TEST_ASSERT_EQUAL_INT(59, e.second);
}

// Every day from 1970 to 2100 there and back, and its weekday one more
// than the day before's.
void test_every_day_round_trips() {
  int weekday = 3;  // 1969-12-31's
  for (uint64_t day = 0; day <= 47482; ++day) {  // to 2100-01-01
    const uint64_t t = day * 86400000ull + 86399999ull;  // its last ms
    const Civil c = toCivil(t);
    TEST_ASSERT_EQUAL_UINT64(t / 1000 * 1000, ms(c));
    weekday = (weekday + 1) % 7;
    TEST_ASSERT_EQUAL_INT(weekday, c.weekday);
  }
}

// What isn't a time: the calendar's impossible days and hours, and before
// 1970.
void test_not_a_time() {
  uint64_t v = 7;
  const Civil bad[] = {
      civil(2027, 2, 29),  civil(2100, 2, 29),  civil(2026, 4, 31), civil(2026, 13, 1),
      civil(2026, 0, 1),   civil(2026, 1, 0),   civil(2026, 1, 32), civil(2026, 1, 1, 24),
      civil(2026, 1, 1, 0, 60), civil(2026, 1, 1, 0, 0, 60), civil(2026, 1, 1, -1), civil(1969, 12, 31, 23, 59, 59),
  };
  for (const Civil& c : bad) {
    TEST_ASSERT_FALSE(fromCivil(c, &v));
    TEST_ASSERT_EQUAL_UINT64(7, v);  // untouched
  }
  TEST_ASSERT_TRUE(fromCivil(civil(2024, 2, 29), &v));  // a real leap day
  TEST_ASSERT_TRUE(fromCivil(civil(2000, 2, 29), &v));  // 2000 is one (divisible by 400)
}

// The times believed: 2026-01-01 00:00:00.000 to 2099-12-31 23:59:59.999.
void test_believable() {
  TEST_ASSERT_EQUAL_UINT64(ms(civil(2026, 1, 1)), kMinMs);
  TEST_ASSERT_EQUAL_UINT64(ms(civil(2100, 1, 1)) - 1, kMaxMs);
  TEST_ASSERT_TRUE(believable(kMinMs));
  TEST_ASSERT_TRUE(believable(kMaxMs));
  TEST_ASSERT_TRUE(believable(1792332202123ull));
  TEST_ASSERT_FALSE(believable(kMinMs - 1));
  TEST_ASSERT_FALSE(believable(kMaxMs + 1));
  TEST_ASSERT_FALSE(believable(0));                  // (the @time question's mark)
  TEST_ASSERT_FALSE(believable(315532800000ull));    // 1980-01-01: a PC whose clock was never set
  TEST_ASSERT_FALSE(believable(1792332202ull));      // seconds sent for ms: 1970-01-21
  TEST_ASSERT_FALSE(believable(1792332202123000ull));  // microseconds: past the year 50,000
  TEST_ASSERT_FALSE(believable(ms(civil(2000, 1, 1, 0, 0, 12))));  // the fresh RTC's
}

// The boot: the RTC's time when it answered, its flag is clear and its
// date is believed; else unset, and why.
void test_boot() {
  Clock k;
  TEST_ASSERT_FALSE(k.ok());
  TEST_ASSERT_EQUAL_UINT64(0, k.at(1234));
  const Civil now = civil(2026, 10, 18, 14, 3, 22);
  TEST_ASSERT_TRUE(k.boot(rtc(now), 2500) == Boot::Ok);
  TEST_ASSERT_TRUE(k.ok());
  TEST_ASSERT_TRUE(k.source() == Source::Rtc);
  TEST_ASSERT_EQUAL_UINT64(1792332202000ull, k.at(2500));  // to the second
  TEST_ASSERT_EQUAL_UINT64(1792332202000ull + 7500, k.at(10000));
  // Unset, each reason (the first that applies).
  Reading r;
  TEST_ASSERT_TRUE(k.boot(r, 0) == Boot::NoChip);
  TEST_ASSERT_FALSE(k.ok());
  TEST_ASSERT_EQUAL_UINT64(0, k.at(2500));
  r = rtc(now);
  r.voltLow = true;  // whatever it says
  TEST_ASSERT_TRUE(k.boot(r, 0) == Boot::VoltLow);
  r.read = false;
  TEST_ASSERT_TRUE(k.boot(r, 0) == Boot::VoltLow);
  r = rtc(now);
  r.read = false;
  TEST_ASSERT_TRUE(k.boot(r, 0) == Boot::Unreadable);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2026, 2, 30)), 0) == Boot::Unreadable);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2000, 1, 1, 0, 0, 12)), 0) == Boot::Range);  // never set
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2025, 12, 31, 23, 59, 59)), 0) == Boot::Range);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2026, 1, 1)), 0) == Boot::Ok);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2099, 12, 31, 23, 59, 59)), 0) == Boot::Ok);
  TEST_ASSERT_FALSE(k.boot(rtc(civil(1999, 12, 31)), 0) == Boot::Ok);  // (M5Unified's 1900s: the century bit)
  TEST_ASSERT_FALSE(k.ok());
  TEST_ASSERT_EQUAL_STRING("voltage low", bootName(Boot::VoltLow));
}

// A @time: set when believed (its ms kept), from then the time runs on the
// board's monotonic ms; a time not believed changes nothing.
void test_set() {
  Clock k;
  TEST_ASSERT_FALSE(k.set(kMinMs - 1, 100));
  TEST_ASSERT_FALSE(k.set(0, 100));
  TEST_ASSERT_FALSE(k.ok());
  TEST_ASSERT_TRUE(k.set(1792332202123ull, 100000));
  TEST_ASSERT_TRUE(k.source() == Source::Host);
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull, k.at(100000));
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull + 1, k.at(100001));
  // Over the boot's set clock: the RTC's replaced.
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2026, 10, 18, 14, 3, 20)), 0) == Boot::Ok);
  TEST_ASSERT_TRUE(k.set(1792332202123ull, 2000));  // the RTC was 0.123 s behind
  TEST_ASSERT_TRUE(k.source() == Source::Host);
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull, k.at(2000));
  // Refused: the clock as it was.
  TEST_ASSERT_FALSE(k.set(kMaxMs + 1, 3000));
  TEST_ASSERT_TRUE(k.source() == Source::Host);
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull + 1000, k.at(3000));
  // A boot reading that isn't believed leaves it unset (a new boot).
  TEST_ASSERT_TRUE(k.boot(Reading{}, 0) == Boot::NoChip);
  TEST_ASSERT_FALSE(k.ok());
}

// The time at a moment earlier in the boot, before the clock was set too
// (a play's start, dated at its end: docs/METADATA.md 2.20.2); and after
// 49.7 days on (the monotonic ms are 64-bit: no wrap at 2^32).
void test_at_any_moment() {
  Clock k;
  const uint64_t started = 5000;  // a play began, the clock unset
  TEST_ASSERT_EQUAL_UINT64(0, k.at(started));
  TEST_ASSERT_TRUE(k.set(1792332202123ull, 65000));  // a computer set it a minute later
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull - 60000, k.at(started));
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull - 65000, k.at(0));
  const uint64_t later = 65000 + 0x100000000ull;  // 2^32 ms on
  TEST_ASSERT_EQUAL_UINT64(1792332202123ull + 0x100000000ull, k.at(later));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_known_days);
  RUN_TEST(test_time_of_day);
  RUN_TEST(test_every_day_round_trips);
  RUN_TEST(test_not_a_time);
  RUN_TEST(test_believable);
  RUN_TEST(test_boot);
  RUN_TEST(test_set);
  RUN_TEST(test_at_any_moment);
  return UNITY_END();
}
