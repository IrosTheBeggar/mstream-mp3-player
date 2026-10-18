// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The board's wall clock (docs/HOST-STATUS.md, "@time"): Unix time in ms
// (UTC), or unset. The Core2 keeps the time in its RTC, a BM8563 on the
// internal I2C bus that counts seconds while the board is off (ESTIMATED:
// the device check, HOST-STATUS.md step 10); the boot reads it once
// (app/BoardClock), and from then on the time is that reading plus the
// board's monotonic ms since (esp_timer's: it never wraps or jumps). A
// computer's @time sets the RTC and this. The play log (docs/METADATA.md 2.20) takes its startedAt and
// its clock byte from here. Portable: the firmware reads and writes the
// chip and passes the monotonic ms; nothing here touches the hardware.
// Host-tested: test_wall_clock.
namespace wallclock {

// The times believed, a @time's and the RTC's: from 2026-01-01 00:00:00
// UTC (no board ran this firmware before; a computer whose own clock was
// never set says 1970 or 1980, and seconds sent for ms read January 1970)
// to 2099-12-31 23:59:59.999 UTC (the RTC's calendar holds 2000-2099;
// microseconds sent for ms read past it).
inline constexpr uint64_t kMinMs = 1767225600000ull;  // 2026-01-01T00:00:00Z
inline constexpr uint64_t kMaxMs = 4102444799999ull;  // 2099-12-31T23:59:59.999Z
constexpr bool believable(uint64_t unixMs) { return unixMs >= kMinMs && unixMs <= kMaxMs; }

// A time as the RTC keeps it: the UTC calendar, to the second.
struct Civil {
  int year = 1970;
  int month = 1;    // 1-12
  int day = 1;      // 1-31
  int hour = 0;     // 0-23
  int minute = 0;   // 0-59
  int second = 0;   // 0-59
  int weekday = 4;  // 0 Sunday to 6 Saturday (1970-01-01 was a Thursday)
};

// Unix ms to the calendar: the second it falls in (the ms dropped).
Civil toCivil(uint64_t unixMs);
// The calendar to Unix ms (a whole second): false when it isn't a real
// time from 1970 to 9999 (February 30, hour 24, ...). The weekday isn't read.
bool fromCivil(const Civil& c, uint64_t* unixMs);
// "2026-10-18 14:03:22" (19 bytes and the terminator), cut to `size`.
void text(const Civil& c, char* out, size_t size);

// Where the time came from: the play log's clock byte (docs/METADATA.md
// 2.20.2), the same numbers.
enum class Source : uint8_t {
  Unset = 0,  // neither gave a time believed
  Rtc = 1,    // the RTC's, read at the boot
  Host = 2,   // a computer's @time, this boot
};

// The boot's reading of the RTC (M5Unified's M5.Rtc).
struct Reading {
  bool chip = false;     // the RTC answered at M5.begin() (M5.Rtc.isEnabled())
  bool read = false;     // its time registers were read and are a date (M5.Rtc.getDateTime())
  bool voltLow = false;  // its voltage-low flag (VL, bit 7 of VL_seconds: M5.Rtc.getVoltLow())
  Civil civil;           // what it says (when `read`)
};

// What the boot made of it: Ok, or why the clock is unset (the boot log's;
// the first that applies, in this order).
enum class Boot : uint8_t {
  Ok,
  NoChip,      // no RTC answered
  VoltLow,     // the chip lost its supply since it was last set (the
               // battery ran flat or was out), or was never set: the
               // flag is set at its power-on and cleared only by setting
               // the time, so its time isn't kept, whatever it says
  Unreadable,  // the read failed, or the registers aren't a date
  Range,       // a date outside kMinMs..kMaxMs: never set (a fresh
               // chip counts from 2000), or set wrong
};
const char* bootName(Boot b);  // "ok", "no RTC", "unreadable", "voltage low", "out of range"

class Clock {
public:
  // The boot's reading, taken at `monoMs`: the clock is the RTC's (to the
  // second) when the chip answered, its flag is clear and its date is a
  // date believed; else unset.
  Boot boot(const Reading& r, uint64_t monoMs);
  // A computer's @time at `monoMs`: true and the clock set when the time
  // is believed; else false and nothing changes.
  bool set(uint64_t unixMs, uint64_t monoMs);
  Source source() const { return source_; }
  bool ok() const { return source_ != Source::Unset; }
  // The time at `monoMs`, any moment of this boot: now, or earlier (before
  // the clock was set too: a play's start, dated by the clock at its end).
  // 0 while unset.
  uint64_t at(uint64_t monoMs) const;

private:
  Source source_ = Source::Unset;
  uint64_t unixMs_ = 0;  // the time at monoMs_
  uint64_t monoMs_ = 0;
};

}  // namespace wallclock
