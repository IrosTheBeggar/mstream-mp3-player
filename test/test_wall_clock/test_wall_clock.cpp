// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for WallClock (docs/HOST-STATUS.md, "@time"): the calendar
// both ways (the RTC's fields and Unix ms), the times believed, what the
// boot makes of the RTC's reading, a @time, the time at a moment of the
// boot, the boot's reading made exact (refine(): RtcSync's catch,
// test_rtc_sync), the RTC's writes cut at every byte, and its registers
// as one read gives them.
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
  // M5Unified's 1900s (the century bit): dates out of the range, before
  // 1970 too, and whatever the rest says (the year is read first).
  TEST_ASSERT_TRUE(k.boot(rtc(civil(1999, 12, 31)), 0) == Boot::Range);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(1950, 6, 1)), 0) == Boot::Range);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(1926, 10, 18, 14, 3, 22)), 0) == Boot::Range);  // a @time cut short
  TEST_ASSERT_TRUE(k.boot(rtc(civil(1926, 2, 30)), 0) == Boot::Range);
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2100, 1, 1)), 0) == Boot::Range);
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

// The boot's reading made exact at the chip's next tick (RtcSync's
// catch): only while the clock is the RTC's; a @time since wins, and an
// unset clock stays unset.
void test_refine() {
  Clock k;
  TEST_ASSERT_FALSE(k.refine(1792332203000ull, 900));  // unset
  TEST_ASSERT_FALSE(k.ok());
  TEST_ASSERT_TRUE(k.boot(rtc(civil(2026, 10, 18, 14, 3, 22)), 2500) == Boot::Ok);
  // The tick to :23 came 412 ms after the reading: the reading was 588 ms in.
  TEST_ASSERT_TRUE(k.refine(1792332203000ull, 2912));
  TEST_ASSERT_TRUE(k.source() == Source::Rtc);
  TEST_ASSERT_EQUAL_UINT64(1792332202588ull, k.at(2500));
  TEST_ASSERT_EQUAL_UINT64(1792332203000ull, k.at(2912));
  TEST_ASSERT_FALSE(k.refine(kMaxMs + 1, 3000));  // not a time believed
  TEST_ASSERT_EQUAL_UINT64(1792332203000ull, k.at(2912));
  TEST_ASSERT_TRUE(k.set(1792332210123ull, 10000));
  TEST_ASSERT_FALSE(k.refine(1792332211000ull, 10500));  // a @time since
  TEST_ASSERT_EQUAL_UINT64(1792332210623ull, k.at(10500));
}

namespace {

// The BM8563's time registers, 02h-08h, byte by byte as written; and
// M5Unified 0.2.23's read of them (PCF8563_Class::getDateTime(), its masks,
// its BCD and range checks, 19yy for the century bit; getVoltLow()).
struct Chip {
  uint8_t r[7];
};

// A chip saying `c` (2000-2099), its own encoding (not rtcWrites()'s).
Chip chipAt(const Civil& c, bool voltLow) {
  const auto bcd = [](int v) { return static_cast<uint8_t>(v / 10 * 16 + v % 10); };
  Chip k = {{bcd(c.second), bcd(c.minute), bcd(c.hour), bcd(c.day), static_cast<uint8_t>(c.weekday), bcd(c.month),
             bcd(c.year - 2000)}};
  if (voltLow) k.r[0] |= 0x80;
  return k;
}

Reading readChip(const Chip& k) {
  const auto bcdOk = [](uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; };
  const auto dec = [](uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); };
  Reading r;
  r.chip = true;
  r.voltLow = (k.r[0] & 0x80) != 0;
  const uint8_t sec = k.r[0] & 0x7f, min = k.r[1] & 0x7f, hour = k.r[2] & 0x3f, day = k.r[3] & 0x3f;
  const uint8_t mo = k.r[5] & 0x1f, yy = k.r[6];
  if (!bcdOk(sec) || !bcdOk(min) || !bcdOk(hour) || !bcdOk(day) || !bcdOk(mo) || !bcdOk(yy)) return r;
  Civil c;
  c.second = dec(sec);
  c.minute = dec(min);
  c.hour = dec(hour);
  c.day = dec(day);
  c.weekday = k.r[4] & 0x07;
  c.month = dec(mo);
  c.year = dec(yy) + ((k.r[5] & 0x80) ? 1900 : 2000);
  if (c.second > 59 || c.minute > 59 || c.hour > 23 || c.day < 1 || c.day > 31 || c.month < 1 || c.month > 12) {
    return r;
  }
  r.read = true;
  r.civil = c;
  return r;
}

// The writes' first `cut` bytes on `from`.
Chip cutShort(Chip from, const RtcWrite* w, int writes, int cut) {
  int sent = 0;
  for (int i = 0; i < writes; ++i) {
    for (int b = 0; b < w[i].len && sent < cut; ++b, ++sent) from.r[w[i].reg - kRtcTimeReg + b] = w[i].data[b];
  }
  return from;
}

struct Before {
  Chip chip;
  Civil target;  // the @time's second
};

// The chips a @time meets, each with a second to write that differs from
// it in a carry (or isn't a time at all).
void befores(Before (&out)[6]) {
  const Civil autumn = civil(2026, 10, 18, 14, 3, 22);
  out[0] = {chipAt(civil(2000, 1, 1, 0, 0, 12), true), autumn};          // never set: VL, 2000
  out[1] = {chipAt(civil(2026, 10, 17, 9, 15, 0), true), autumn};        // its supply failed after a set
  out[2] = {chipAt(civil(2026, 10, 18, 14, 3, 19), false), autumn};      // believed, 3 s behind
  out[3] = {chipAt(civil(2026, 10, 18, 13, 59, 58), false), civil(2026, 10, 18, 14, 0, 1)};  // over an hour
  out[4] = {chipAt(civil(2026, 12, 31, 23, 59, 58), false), civil(2027, 1, 1, 0, 0, 1)};     // over a year
  Chip junk;
  std::memset(junk.r, 0xFF, sizeof(junk.r));  // not a time
  out[5] = {junk, autumn};
}

}  // namespace

// The bytes a @time writes, and their order.
void test_rtc_writes() {
  RtcWrite w[kRtcWrites];
  rtcWrites(toCivil(1792332202000ull), w);  // 2026-10-18 14:03:22, a Sunday
  TEST_ASSERT_EQUAL_UINT8(0x07, w[0].reg);
  TEST_ASSERT_EQUAL_UINT8(1, w[0].len);
  TEST_ASSERT_EQUAL_HEX8(0x90, w[0].data[0]);  // October, the century bit
  const uint8_t marked[7] = {0x22, 0x03, 0x14, 0x18, 0x00, 0x90, 0x26};
  const uint8_t time[7] = {0x22, 0x03, 0x14, 0x18, 0x00, 0x10, 0x26};
  TEST_ASSERT_EQUAL_UINT8(0x02, w[1].reg);
  TEST_ASSERT_EQUAL_UINT8(7, w[1].len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(marked, w[1].data, 7);
  TEST_ASSERT_EQUAL_UINT8(0x02, w[2].reg);
  TEST_ASSERT_EQUAL_UINT8(7, w[2].len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(time, w[2].data, 7);
  // The range's ends.
  rtcWrites(toCivil(kMaxMs), w);  // 2099-12-31 23:59:59, a Thursday
  const uint8_t last[7] = {0x59, 0x59, 0x23, 0x31, 0x04, 0x12, 0x99};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(last, w[2].data, 7);
  rtcWrites(toCivil(kMinMs), w);
  const uint8_t firstSecond[7] = {0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x26};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(firstSecond, w[2].data, 7);
  // M5Unified reads back what was written (and the marked write as 1926).
  Chip k = cutShort(chipAt(civil(2000, 1, 1), true), w, kRtcWrites, 15);
  Reading r = readChip(k);
  TEST_ASSERT_TRUE(r.read && !r.voltLow);
  uint64_t v = 0;
  TEST_ASSERT_TRUE(fromCivil(r.civil, &v));
  TEST_ASSERT_EQUAL_UINT64(kMinMs, v);
}

// A cut at any byte of a @time's writes (a reset, a brown-out), on each
// chip it may meet: the chip as it was (no byte sent), unset at the next
// boot, or the time written; never a mix the boot believes. And the model
// finds the hole one write of 02h-08h, the seconds first, had.
void test_rtc_write_cut() {
  Before bs[6];
  befores(bs);
  for (const Before& b : bs) {
    uint64_t want = 0;
    TEST_ASSERT_TRUE(fromCivil(b.target, &want));
    RtcWrite w[kRtcWrites];
    rtcWrites(b.target, w);
    for (int cut = 0; cut <= 15; ++cut) {
      const Chip k = cutShort(b.chip, w, kRtcWrites, cut);
      Clock c;
      const Boot boot = c.boot(readChip(k), 0);
      if (cut == 0) {
        TEST_ASSERT_EQUAL_MEMORY(b.chip.r, k.r, 7);
      } else if (cut < 14) {
        TEST_ASSERT_FALSE(boot == Boot::Ok);  // 19yy from the first byte, until the last write's 07h
      } else {
        TEST_ASSERT_TRUE(boot == Boot::Ok);  // its 08h already the year
        TEST_ASSERT_EQUAL_UINT64(want, c.at(0));
      }
    }
  }
  // One write, 02h-08h: cut after the seconds and minutes over a chip
  // believed 3 s behind across an hour, 13:00:01 believed for 14:00:01.
  RtcWrite one[1];
  {
    RtcWrite w[kRtcWrites];
    rtcWrites(bs[3].target, w);
    one[0] = w[2];
  }
  Clock c;
  TEST_ASSERT_TRUE(c.boot(readChip(cutShort(bs[3].chip, one, 1, 2)), 0) == Boot::Ok);
  TEST_ASSERT_EQUAL_UINT64(ms(civil(2026, 10, 18, 13, 0, 1)), c.at(0));
}

// The registers as one read gives them (the catch's polls) decode as
// M5Unified's read does (the model above): every chip a @time meets, cut
// at every byte; and judge() is the boot's rule.
void test_from_registers() {
  Before bs[6];
  befores(bs);
  for (const Before& b : bs) {
    RtcWrite w[kRtcWrites];
    rtcWrites(b.target, w);
    for (int cut = 0; cut <= 15; ++cut) {
      const Chip k = cutShort(b.chip, w, kRtcWrites, cut);
      const Reading want = readChip(k);
      const Reading got = fromRegisters(k.r);
      TEST_ASSERT_TRUE(got.chip);
      TEST_ASSERT_EQUAL(want.voltLow, got.voltLow);
      TEST_ASSERT_EQUAL(want.read, got.read);
      if (want.read) {
        uint64_t a = 0, c = 0;
        Clock x, y;
        TEST_ASSERT_TRUE(judge(want, &a) == x.boot(want, 0));
        TEST_ASSERT_TRUE(judge(got, &c) == y.boot(got, 0));
        TEST_ASSERT_TRUE(judge(want, &a) == judge(got, &c));
        TEST_ASSERT_EQUAL_UINT64(a, c);
        TEST_ASSERT_EQUAL_UINT64(x.at(0), y.at(0));
      }
    }
  }
  // A weekday past Saturday isn't a date to M5Unified (its validateDateTime()).
  Chip k = chipAt(civil(2026, 10, 18, 14, 3, 22), false);
  k.r[4] = 7;
  TEST_ASSERT_FALSE(fromRegisters(k.r).read);
  k.r[4] = 0;
  TEST_ASSERT_TRUE(fromRegisters(k.r).read);
  uint64_t v = 0;
  TEST_ASSERT_TRUE(judge(fromRegisters(k.r), &v) == Boot::Ok);
  TEST_ASSERT_EQUAL_UINT64(1792332202000ull, v);
  k.r[0] |= 0x80;  // the flag
  TEST_ASSERT_TRUE(judge(fromRegisters(k.r), &v) == Boot::VoltLow);
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
  RUN_TEST(test_refine);
  RUN_TEST(test_from_registers);
  RUN_TEST(test_rtc_writes);
  RUN_TEST(test_rtc_write_cut);
  return UNITY_END();
}
