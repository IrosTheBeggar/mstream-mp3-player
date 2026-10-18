// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/BoardClock.h"

#include <M5Unified.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <sys/time.h>

namespace boardclock {
namespace {

wallclock::Clock clock_;
portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;

wallclock::Civil civil(const m5::rtc_datetime_t& dt) {
  wallclock::Civil c;
  c.year = dt.date.year;
  c.month = dt.date.month;
  c.day = dt.date.date;
  c.weekday = dt.date.weekDay;
  c.hour = dt.time.hours;
  c.minute = dt.time.minutes;
  c.second = dt.time.seconds;
  return c;
}

// What the RTC says now (M5Unified's read: false when it failed or the
// registers aren't a date).
bool readRtc(wallclock::Civil* c) {
  m5::rtc_datetime_t dt;
  if (!M5.Rtc.getDateTime(&dt)) return false;
  *c = civil(dt);
  return true;
}

}  // namespace

uint64_t monoMs() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

uint64_t at(uint64_t mono) {
  portENTER_CRITICAL(&lock_);
  const uint64_t ms = clock_.at(mono);
  portEXIT_CRITICAL(&lock_);
  return ms;
}

wallclock::Source source() {
  portENTER_CRITICAL(&lock_);
  const wallclock::Source s = clock_.source();
  portEXIT_CRITICAL(&lock_);
  return s;
}

void begin() {
  wallclock::Reading r;
  r.chip = M5.Rtc.isEnabled();
  if (r.chip) {
    r.voltLow = M5.Rtc.getVoltLow();
    r.read = readRtc(&r.civil);
  }
  const uint64_t mono = monoMs();
  portENTER_CRITICAL(&lock_);
  const wallclock::Boot b = clock_.boot(r, mono);
  portEXIT_CRITICAL(&lock_);
  char when[24] = "";
  if (r.read) wallclock::text(r.civil, when, sizeof(when));
  switch (b) {
    case wallclock::Boot::Ok:
      Serial.printf("[clock] %s UTC, from the RTC (to the second)\n", when);
      return;
    case wallclock::Boot::NoChip:
      Serial.printf("[clock] unset: no RTC answered at the boot (a computer's @time can't set it)\n");
      return;
    case wallclock::Boot::VoltLow:
      Serial.printf("[clock] unset: the RTC lost its time (its voltage-low flag: no supply since it was set, or never "
                    "set; it says %s%s); a computer's @time sets it\n",
                    r.read ? when : "no date", r.read ? " UTC" : "");
      return;
    case wallclock::Boot::Unreadable:
      Serial.printf("[clock] unset: the RTC's reading isn't a date (or the read failed); a computer's @time sets it\n");
      return;
    case wallclock::Boot::Range:
      Serial.printf("[clock] unset: the RTC says %s UTC, outside 2026-2099 (never set, or set wrong); a computer's "
                    "@time sets it\n",
                    when);
      return;
  }
}

const char* set(uint64_t unixMs) {
  const uint64_t mono = monoMs();  // (as the line came: the I2C below takes a few ms)
  if (!wallclock::believable(unixMs)) return "rtc";  // (HostLink's check: never)
  const uint64_t was = at(mono);
  if (!M5.Rtc.isEnabled()) {
    Serial.printf("[clock] @time: no RTC to set (it didn't answer at the boot); nothing changed\n");
    return "rtc";
  }
  // The second it is in (the RTC counts whole seconds). Writing the seconds
  // clears the voltage-low flag.
  const wallclock::Civil c = wallclock::toCivil(unixMs);
  m5::rtc_datetime_t dt;
  dt.date.year = static_cast<int16_t>(c.year);
  dt.date.month = static_cast<int8_t>(c.month);
  dt.date.date = static_cast<int8_t>(c.day);
  dt.date.weekDay = static_cast<int8_t>(c.weekday);
  dt.time.hours = static_cast<int8_t>(c.hour);
  dt.time.minutes = static_cast<int8_t>(c.minute);
  dt.time.seconds = static_cast<int8_t>(c.second);
  M5.Rtc.setDateTime(dt);  // (no result: read it back)
  // Read back: the flag clear, and the second written or the next (one may
  // tick between the write and the read).
  const uint64_t second = unixMs / 1000 * 1000;
  wallclock::Civil back;
  uint64_t backMs = 0;
  const bool flag = M5.Rtc.getVoltLow();
  const bool read = readRtc(&back) && wallclock::fromCivil(back, &backMs);
  if (flag || !read || backMs < second || backMs > second + 2000) {
    char said[24] = "?";
    if (read) wallclock::text(back, said, sizeof(said));
    Serial.printf("[clock] @time: the RTC didn't take it (read back: %s, voltage-low flag %s); nothing changed\n", said,
                  flag ? "set" : "clear");
    return "rtc";
  }
  portENTER_CRITICAL(&lock_);
  clock_.set(unixMs, mono);
  portEXIT_CRITICAL(&lock_);
  // The system time too, as M5.begin() sets it from the RTC at every boot
  // (FatFs dates the files the board writes by it).
  timeval tv;
  tv.tv_sec = static_cast<time_t>(unixMs / 1000);
  tv.tv_usec = static_cast<suseconds_t>(unixMs % 1000 * 1000);
  settimeofday(&tv, nullptr);
  char when[24];
  wallclock::text(c, when, sizeof(when));
  if (was == 0) {
    Serial.printf("[clock] %s.%03u UTC, set by a computer's @time (it was unset)\n", when,
                  static_cast<unsigned>(unixMs % 1000));
  } else {
    const int64_t off = static_cast<int64_t>(was - unixMs);
    Serial.printf("[clock] %s.%03u UTC, set by a computer's @time (it was %.1f s %s)\n", when,
                  static_cast<unsigned>(unixMs % 1000), (off < 0 ? -off : off) / 1000.0, off < 0 ? "behind" : "ahead");
  }
  return nullptr;
}

}  // namespace boardclock
