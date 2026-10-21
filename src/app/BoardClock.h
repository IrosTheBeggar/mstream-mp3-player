// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "WallClock.h"

// The board's wall clock (lib/core WallClock and RtcSync; docs/HOST-STATUS.md,
// "@time"): the Core2's RTC (a BM8563, M5Unified's M5.Rtc, which M5.begin()
// found) read at the boot with its voltage-low flag, made exact at its next
// tick (the loop's catch), and set by a computer's @time. How the rest of
// the firmware reads "now": now() (Unix ms, 0 while unset) and source() (the
// play log's clock byte, docs/METADATA.md 2.20). The RTC is touched only by
// begin(), set() and loop(), on the loop task (its I2C port is the AXP192's
// and the touch panel's, read there); the clock's words are read from any
// task (a spinlock around them).
namespace boardclock {

// setup(), after M5.begin(): the RTC read, the kept phase (NVS
// "clock"/"phase"), and the boot log's [clock] line.
void begin();
// Every loop pass: the RTC's catch after the boot (a read of its time every
// 10 ms or more, about a second; three at most), a @time's write on its
// second and the catch after it, and their [clock] lines. Nothing while
// neither runs.
void loop();
// A computer's @time (believable: HostLink checked it): when the RTC
// answers (a read of its time now), this boot's clock and the system time
// (FatFs's file dates, which M5.begin() copies from the RTC at every boot)
// set at once, and the RTC on the clock's next whole second (loop(): written
// in WallClock's rtcWrites() steps, read back, written again at the next
// second if it didn't take, three times at most; the log says how it went).
// nullptr: done ("@time ok"); else why not ("rtc": no RTC answered at the
// boot, or it doesn't now; nothing changed).
const char* set(uint64_t unixMs);
// The board's monotonic ms since the boot (esp_timer's: no wrap).
uint64_t monoMs();
// The time at `mono` (monoMs(): now, or any moment earlier in this boot);
// 0 while unset.
uint64_t at(uint64_t mono);
inline uint64_t now() { return at(monoMs()); }
wallclock::Source source();
inline bool ok() { return source() != wallclock::Source::Unset; }

}  // namespace boardclock
