// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "WallClock.h"

#include <cstdio>

namespace wallclock {

namespace {

constexpr uint64_t kDayMs = 86400000ull;

bool leapYear(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int monthDays(int y, int m) {
  static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && leapYear(y) ? 29 : kDays[m - 1];
}

// Days from 1970-01-01 to y-m-d, y from 1970 (Howard Hinnant's
// days_from_civil: the year begins in March, so the leap day is its last).
int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = y / 400;
  const int64_t yoe = y - era * 400;                              // 0-399
  const int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;  // 0-365
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     // 0-146096
  return era * 146097 + doe - 719468;
}

}  // namespace

Civil toCivil(uint64_t unixMs) {
  const int64_t days = static_cast<int64_t>(unixMs / kDayMs);
  const uint64_t msOfDay = unixMs % kDayMs;
  // civil_from_days, its inverse.
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  Civil c;
  c.day = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  c.month = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  c.year = static_cast<int>(yoe + era * 400 + (c.month <= 2));
  const uint64_t s = msOfDay / 1000;
  c.hour = static_cast<int>(s / 3600);
  c.minute = static_cast<int>(s / 60 % 60);
  c.second = static_cast<int>(s % 60);
  c.weekday = static_cast<int>((days + 4) % 7);
  return c;
}

bool fromCivil(const Civil& c, uint64_t* unixMs) {
  if (c.year < 1970 || c.year > 9999 || c.month < 1 || c.month > 12 || c.day < 1 ||
      c.day > monthDays(c.year, c.month) || c.hour < 0 || c.hour > 23 || c.minute < 0 || c.minute > 59 ||
      c.second < 0 || c.second > 59) {
    return false;
  }
  const auto days = static_cast<uint64_t>(daysFromCivil(c.year, c.month, c.day));
  *unixMs = days * kDayMs + (static_cast<uint64_t>(c.hour) * 3600 + c.minute * 60 + c.second) * 1000;
  return true;
}

void text(const Civil& c, char* out, size_t size) {
  if (size == 0) return;
  std::snprintf(out, size, "%04d-%02d-%02d %02d:%02d:%02d", c.year, c.month, c.day, c.hour, c.minute, c.second);
}

void rtcWrites(const Civil& c, RtcWrite (&out)[kRtcWrites]) {
  const auto bcd = [](int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); };
  RtcWrite time;
  time.reg = kRtcTimeReg;
  time.len = 7;
  time.data[0] = bcd(c.second);  // (VL clear)
  time.data[1] = bcd(c.minute);
  time.data[2] = bcd(c.hour);
  time.data[3] = bcd(c.day);
  time.data[4] = static_cast<uint8_t>(c.weekday & 7);
  time.data[5] = bcd(c.month);  // (20yy)
  time.data[6] = bcd(c.year % 100);
  out[0].reg = kRtcMonthReg;
  out[0].len = 1;
  out[0].data[0] = time.data[5] | kRtcCentury;
  out[1] = time;
  out[1].data[5] |= kRtcCentury;
  out[2] = time;
}

const char* bootName(Boot b) {
  switch (b) {
    case Boot::Ok: return "ok";
    case Boot::NoChip: return "no RTC";
    case Boot::Unreadable: return "unreadable";
    case Boot::VoltLow: return "voltage low";
    case Boot::Range: return "out of range";
  }
  return "?";
}

Boot judge(const Reading& r, uint64_t* unixMs) {
  uint64_t ms = 0;
  if (!r.chip) return Boot::NoChip;
  if (r.voltLow) return Boot::VoltLow;  // (whatever its registers say)
  if (!r.read) return Boot::Unreadable;
  // The year before the rest: a 19yy (M5Unified's read of the century
  // bit, a @time cut short's) is a date out of the range, before 1970 too.
  if (r.civil.year < kMinYear || r.civil.year > kMaxYear) return Boot::Range;
  if (!fromCivil(r.civil, &ms)) return Boot::Unreadable;
  if (!believable(ms)) return Boot::Range;  // (never, the year in it)
  *unixMs = ms;
  return Boot::Ok;
}

Reading fromRegisters(const uint8_t (&regs)[7]) {
  const auto bcdOk = [](uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; };
  const auto dec = [](uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); };
  Reading r;
  r.chip = true;
  r.voltLow = (regs[0] & kRtcVoltLow) != 0;
  const uint8_t sec = regs[0] & 0x7f, min = regs[1] & 0x7f, hour = regs[2] & 0x3f;
  const uint8_t day = regs[3] & 0x3f, wd = regs[4] & 0x07, mo = regs[5] & 0x1f, yy = regs[6];
  if (!bcdOk(sec) || !bcdOk(min) || !bcdOk(hour) || !bcdOk(day) || !bcdOk(mo) || !bcdOk(yy)) return r;
  Civil c;
  c.second = dec(sec);
  c.minute = dec(min);
  c.hour = dec(hour);
  c.day = dec(day);
  c.weekday = wd;
  c.month = dec(mo);
  c.year = dec(yy) + ((regs[5] & kRtcCentury) ? 1900 : 2000);
  // (RTC_Base::validateDateTime())
  if (c.second > 59 || c.minute > 59 || c.hour > 23 || c.day < 1 || c.day > 31 || c.month < 1 || c.month > 12 ||
      c.weekday > 6) {
    return r;
  }
  r.read = true;
  r.civil = c;
  return r;
}

Boot Clock::boot(const Reading& r, uint64_t monoMs) {
  source_ = Source::Unset;
  uint64_t ms = 0;
  const Boot b = judge(r, &ms);
  if (b != Boot::Ok) return b;
  source_ = Source::Rtc;
  unixMs_ = ms;
  monoMs_ = monoMs;
  return Boot::Ok;
}

bool Clock::refine(uint64_t unixMs, uint64_t monoMs) {
  if (source_ != Source::Rtc || !believable(unixMs)) return false;
  unixMs_ = unixMs;
  monoMs_ = monoMs;
  return true;
}

bool Clock::set(uint64_t unixMs, uint64_t monoMs) {
  if (!believable(unixMs)) return false;
  source_ = Source::Host;
  unixMs_ = unixMs;
  monoMs_ = monoMs;
  return true;
}

uint64_t Clock::at(uint64_t monoMs) const {
  if (!ok()) return 0;
  // (Signed: a moment before the reading or the @time. Never below
  // kMinMs less this boot's uptime: far from 0.)
  return static_cast<uint64_t>(static_cast<int64_t>(unixMs_) + static_cast<int64_t>(monoMs - monoMs_));
}

}  // namespace wallclock
