// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ScanSync.h"

#include <algorithm>

int32_t ScanSync::untilBand(int scanLine) const {
  const int first = top_ + panel_.firstRow;
  int ahead = (first - scanLine) % panel_.lines;
  if (ahead < 0) ahead += panel_.lines;
  return lineTime(ahead);
}

ScanSync::Window ScanSync::plan(int scanLine, int d, uint32_t holdUs, uint32_t lateUs) const {
  Window w;
  const int n = d < 0 ? -d : d;
  const int32_t frame = static_cast<int32_t>(panel_.frameUs);
  const int32_t hold = static_cast<int32_t>(holdUs);
  // The hold as scan lines: how far the scan gets while it runs.
  const int32_t holdLines = static_cast<int32_t>(static_cast<int64_t>(holdUs) * panel_.lines / panel_.frameUs);
  // From the band's first row of one refresh (0) to its first row of the
  // next (`frame`): the earliest and the latest start.
  //   - Behind the scan at the strip's first row (and, were the bus faster
  //     than the scan, far enough behind not to catch it up).
  //   - The address (the hold's end) once the scan has left the band.
  //   - The address before the scan is back at the band.
  const int stripFirst = d > 0 ? 0 : height_ - n;
  const int32_t behind = lineTime(stripFirst + config_.enterLines + std::max<int32_t>(0, n - holdLines));
  const int32_t left = lineTime(height_ + config_.leaveLines) - hold;
  int32_t earliest = std::max(behind, left);
  const int32_t latest = frame - lineTime(config_.exitLines) - hold;
  w.clean = earliest <= latest;
  if (!w.clean) earliest = latest;  // the address on time: the strip's first rows show early
  // The first refresh whose window isn't over: its band's first row is
  // `ahead` from now, the one before it a frame earlier.
  const int32_t ahead = untilBand(scanLine);
  int32_t base = ahead - frame;  // the refresh under way (or just done)
  if (base + latest < 0) {
    // Its window is over: late within `lateUs`, else the next refresh's.
    if (base + latest + static_cast<int64_t>(lateUs) < 0) {
      base += frame;
    } else {
      w.late = true;
    }
  }
  w.earliestUs = std::max<int32_t>(0, base + earliest);
  w.latestUs = std::max<int32_t>(0, base + latest);
  w.addressUs = base + lineTime(height_ + config_.leaveLines);
  w.showsUs = base + frame;
  return w;
}
