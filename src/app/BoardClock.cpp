// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/BoardClock.h"

#include <M5Unified.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <sys/time.h>

#include <cstdio>

#include "NvsLayout.h"
#include "RtcSync.h"

namespace boardclock {
namespace {

using rtcsync::Event;

wallclock::Clock clock_;
// The loop task's alone (begin(), set(), loop()), called under the lock as
// it reads and refines the clock.
rtcsync::Sync sync_;
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

m5::RTC_Base* chip() { return M5.Rtc.isEnabled() ? M5.Rtc.getRtcInstancePtr() : nullptr; }

// Its seven time registers, 02h-08h, in one read (getDateTime()'s burst).
bool readRegisters(uint8_t (&r)[7]) {
  m5::RTC_Base* rtc = chip();
  return rtc != nullptr && rtc->readRegister(wallclock::kRtcTimeReg, r, sizeof(r));
}

// A second written in WallClock's three steps (a cut at any byte leaves no
// time the next boot believes wrongly).
bool writeRtc(uint64_t second) {
  m5::RTC_Base* rtc = chip();
  if (rtc == nullptr) return false;
  wallclock::RtcWrite w[wallclock::kRtcWrites];
  wallclock::rtcWrites(wallclock::toCivil(second), w);
  for (const wallclock::RtcWrite& x : w) {
    if (!rtc->writeRegister(x.reg, x.data, x.len)) return false;  // (the rest not sent: as it was, or 19yy)
  }
  return true;
}

// NVS's "clock"/"phase" (nvslayout, schema 5).
int32_t keptPhase() {
  Preferences p;
  // (Read-write, though it only reads, as AutoDjStore's load(): a read-only
  // open of a namespace not made yet fails with an [E] line at every boot
  // until the first @time; read-write makes the namespace, no key.)
  if (!p.begin(nvslayout::kClockNamespace, false)) return rtcsync::kNoPhase;
  const bool have = p.isKey(nvslayout::kClockPhaseKey);
  const int32_t v = have ? p.getInt(nvslayout::kClockPhaseKey, 0) : 0;
  p.end();
  return rtcsync::phaseFrom(have, v);
}

bool keepPhase(int32_t ms) {
  Preferences p;
  if (!p.begin(nvslayout::kClockNamespace, false)) return false;
  const bool ok = p.putInt(nvslayout::kClockPhaseKey, ms) == sizeof(int32_t);
  p.end();
  return ok;
}

bool forgetPhase() {
  Preferences p;
  if (!p.begin(nvslayout::kClockNamespace, false)) return false;
  const bool ok = !p.isKey(nvslayout::kClockPhaseKey) || p.remove(nvslayout::kClockPhaseKey);
  p.end();
  return ok;
}

void textOf(uint64_t ms, char (&out)[24]) { wallclock::text(wallclock::toCivil(ms), out, sizeof(out)); }

const char* why(Event::Why w) {
  switch (w) {
    case Event::Why::None: return "?";
    case Event::Why::Still: return "its second didn't change: it isn't counting";
    case Event::Why::Unanswered: return "too few of its reads went through in 3.3 s";
    case Event::Why::Disagreed: return "its tick isn't within a second of the boot's reading";
    case Event::Why::WriteFailed: return "a write didn't go through";
    case Event::Why::Unread: return "no read-back went through";
    case Event::Why::Flag: return "its voltage-low flag still set";
    case Event::Why::NotTime: return "read back as no time it believes";
    case Event::Why::Wrong: return "read back as another time";
  }
  return "?";
}

void report(const Event& e) {
  char t[24];
  switch (e.kind) {
    case Event::Kind::None:
      return;
    case Event::Kind::Caught: {
      textOf(e.time, t);
      const int64_t s = e.shiftMs < 0 ? -e.shiftMs : e.shiftMs;
      char phase[64];
      if (e.phaseMs == rtcsync::kNoPhase) {
        std::snprintf(phase, sizeof(phase), "no phase kept: its seconds taken as the true ones");
      } else {
        std::snprintf(phase, sizeof(phase), "its seconds %+ld ms from the true ones, the last @time's",
                      static_cast<long>(e.phaseMs));
      }
      Serial.printf("[clock] %s.%03u UTC at the RTC's tick, within %lu ms (%s); the boot's second was %lld.%03lld s "
                    "%s\n",
                    t, static_cast<unsigned>(e.time % 1000), static_cast<unsigned long>(e.withinMs), phase,
                    static_cast<long long>(s / 1000), static_cast<long long>(s % 1000),
                    e.shiftMs < 0 ? "ahead" : "behind");
      return;
    }
    case Event::Kind::Missed:
      Serial.printf("[clock] the RTC's tick not caught (%s): the boot's reading kept, to the second\n", why(e.why));
      return;
    case Event::Kind::Taken: {
      textOf(e.second, t);
      const bool kept = keepPhase(e.phaseMs);
      Serial.printf("[clock] the RTC set to %s UTC on its second (written %lu ms after it); its seconds begin %+ld ms "
                    "from the true ones (within %lu ms): %s\n",
                    t, static_cast<unsigned long>(e.lateMs), static_cast<long>(e.phaseMs),
                    static_cast<unsigned long>(e.withinMs),
                    kept ? "kept for the boots" : "NOT kept (NVS failed): the boots add none");
      return;
    }
    case Event::Kind::Untimed:
      textOf(e.second, t);
      Serial.printf("[clock] the RTC set to %s UTC on its second (written %lu ms after it); its next tick not caught "
                    "(%s): no phase kept, the boots add none\n",
                    t, static_cast<unsigned long>(e.lateMs), why(e.why));
      return;
    case Event::Kind::Refused: {
      textOf(e.second, t);
      char back[80] = "";
      if (e.back.answered) {
        char b[24] = "no time";
        if (e.back.believed) textOf(e.back.second, b);
        std::snprintf(back, sizeof(back), "; read back: %s, voltage-low flag %s", b, e.back.voltLow ? "set" : "clear");
      }
      if (e.again) {
        Serial.printf("[clock] @time: the RTC didn't take %s UTC (%s%s); written again at the next second (%d of "
                      "%d)\n",
                      t, why(e.why), back, e.attempt + 1, rtcsync::kAttempts);
      } else {
        Serial.printf("[clock] @time: the RTC didn't take %s UTC (%s%s); given up: this boot's clock is the "
                      "computer's, the next boot reads the RTC as it was or unset (the tab sets it again)\n",
                      t, why(e.why), back);
      }
      return;
    }
  }
}

void drain() {
  Event e;
  portENTER_CRITICAL(&lock_);
  const bool got = sync_.takeEvent(&e);
  portEXIT_CRITICAL(&lock_);
  if (got) report(e);
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
  const int32_t phase = r.chip ? keptPhase() : rtcsync::kNoPhase;
  portENTER_CRITICAL(&lock_);
  const wallclock::Boot b = clock_.boot(r, mono);
  sync_.boot(clock_, phase);  // (its catch from the loop's first pass)
  portEXIT_CRITICAL(&lock_);
  char when[24] = "";
  if (r.read) wallclock::text(r.civil, when, sizeof(when));
  switch (b) {
    case wallclock::Boot::Ok:
      Serial.printf("[clock] %s UTC, from the RTC (to the second; to a few ms at its next tick)\n", when);
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
      Serial.printf("[clock] unset: the RTC says %s UTC, outside 2026-2099 (never set, set wrong, or a @time cut "
                    "short); a computer's @time sets it\n",
                    when);
      return;
  }
}

void loop() {
  portENTER_CRITICAL(&lock_);
  const bool busy = sync_.busy();
  portEXIT_CRITICAL(&lock_);
  if (!busy) return;
  // A drop and a write, then a read (a write's read-back at once), in one
  // pass at most.
  for (int i = 0; i < 4; ++i) {
    uint64_t second = 0;
    const uint64_t mono = monoMs();
    portENTER_CRITICAL(&lock_);
    const rtcsync::Do d = sync_.step(mono, clock_, &second);
    portEXIT_CRITICAL(&lock_);
    drain();
    switch (d) {
      case rtcsync::Do::Nothing:
        return;
      case rtcsync::Do::Forget:
        if (!forgetPhase()) {
          Serial.printf("[clock] @time: the kept phase couldn't be removed (NVS); the next boot may add it until "
                        "this write's is kept\n");
        }
        portENTER_CRITICAL(&lock_);
        sync_.forgot();
        portEXIT_CRITICAL(&lock_);
        break;
      case rtcsync::Do::Write: {
        const bool ok = writeRtc(second);
        const uint64_t after = monoMs();
        portENTER_CRITICAL(&lock_);
        sync_.wrote(after, ok, clock_);
        portEXIT_CRITICAL(&lock_);
        drain();
        break;
      }
      case rtcsync::Do::Read: {
        uint8_t r[7] = {};
        const uint64_t taken = monoMs();  // (the chip holds its registers from the read's start)
        const rtcsync::Sample s = rtcsync::sample(readRegisters(r), r);
        portENTER_CRITICAL(&lock_);
        sync_.read(taken, s, clock_);
        portEXIT_CRITICAL(&lock_);
        drain();
        return;
      }
    }
  }
}

const char* set(uint64_t unixMs) {
  const uint64_t mono = monoMs();  // (as the line came: the read below takes a moment)
  if (!wallclock::believable(unixMs)) return "rtc";  // (HostLink's check: never)
  const uint64_t was = at(mono);
  if (chip() == nullptr) {
    Serial.printf("[clock] @time: no RTC to set (it didn't answer at the boot); nothing changed\n");
    return "rtc";
  }
  // The chip answers now (one read of its time, as the boot's); its write
  // waits for the clock's next whole second (loop()).
  uint8_t r[7];
  if (!readRegisters(r)) {
    Serial.printf("[clock] @time: the RTC didn't answer (a read of its time failed); nothing changed\n");
    return "rtc";
  }
  portENTER_CRITICAL(&lock_);
  clock_.set(unixMs, mono);
  sync_.plan(unixMs, mono);  // (the latest wins: a write or a catch under way dropped)
  portEXIT_CRITICAL(&lock_);
  // The system time too, as M5.begin() sets it from the RTC at every boot
  // (FatFs dates the files the board writes by it).
  timeval tv;
  tv.tv_sec = static_cast<time_t>(unixMs / 1000);
  tv.tv_usec = static_cast<suseconds_t>(unixMs % 1000 * 1000);
  settimeofday(&tv, nullptr);
  char when[24];
  wallclock::text(wallclock::toCivil(unixMs), when, sizeof(when));
  if (was == 0) {
    Serial.printf("[clock] %s.%03u UTC, set by a computer's @time (it was unset); the RTC at its next second\n", when,
                  static_cast<unsigned>(unixMs % 1000));
  } else {
    const int64_t off = static_cast<int64_t>(was - unixMs);
    Serial.printf("[clock] %s.%03u UTC, set by a computer's @time (it was %.3f s %s); the RTC at its next second\n",
                  when, static_cast<unsigned>(unixMs % 1000), (off < 0 ? -off : off) / 1000.0,
                  off < 0 ? "behind" : "ahead");
  }
  return nullptr;
}

}  // namespace boardclock
