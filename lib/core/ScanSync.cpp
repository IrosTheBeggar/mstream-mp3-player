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

ScanSync::Rows ScanSync::rows(int scanLine, int row, int lines, uint32_t quickestUs, uint32_t slowestUs) const {
  Rows r;
  const int32_t total = panel_.lines;
  // The scan lines that go by while the hold writes: at the least (the
  // writing at its quickest) and at the most. More than the picture's rows
  // when the writing is the slower of the two.
  const int32_t least = static_cast<int32_t>(static_cast<int64_t>(quickestUs) * total / panel_.frameUs);
  const int32_t most =
      static_cast<int32_t>((static_cast<int64_t>(slowestUs) * total + panel_.frameUs - 1) / panel_.frameUs);
  // How far the scan is past the picture's first row, 0 to a refresh.
  int32_t past = (scanLine - (row + panel_.firstRow)) % total;
  if (past < 0) past += total;
  // The line being written is at 0 when the hold starts and `lines` when it
  // ends; the scan, from `past`, goes `least` to `most` meanwhile. They
  // never meet when the scan starts in [lo, hi]:
  //   - past the first row, and beyond what the writing gains on it when
  //     the writing is the quicker (lines - least, at its quickest);
  //   - not so far on that it comes round and catches the writing up when
  //     that is the slower (most - lines to spare, at its slowest), or is
  //     still on its way to the picture's first row.
  const int32_t lo = config_.enterLines + std::max<int32_t>(0, lines - least);
  const int32_t hi = total - config_.exitLines - std::max<int32_t>(0, most - lines);
  if (lo > hi) return r;  // too tall for its hold: no clean start
  r.clean = true;
  if (past < lo) {
    r.waitUs = lineTime(lo - past);
    r.openUs = lineTime(hi - lo);
  } else if (past <= hi) {
    r.openUs = lineTime(hi - past);
  } else {
    r.waitUs = lineTime(total - past + lo);
    r.openUs = lineTime(hi - lo);
  }
  return r;
}

bool ScanSync::met(int scanLine, int row, int lines, uint32_t tookUs) const {
  const int64_t total = panel_.lines;
  const int64_t frame = panel_.frameUs;
  int64_t past = (scanLine - (row + panel_.firstRow)) % total;
  if (past < 0) past += total;
  if (past == 0) return true;  // on the first row as the writing begins
  // The scan's lead over the written line when the writing ends, in lines
  // times a refresh's microseconds: `past` at the start, then what the
  // scan went meanwhile less the picture's rows. It met the line if the
  // lead ran out (the writing caught it up) or grew to a whole refresh
  // (the scan came round to it).
  const int64_t lead = past * frame + static_cast<int64_t>(tookUs) * total - static_cast<int64_t>(lines) * frame;
  return lead <= 0 || lead >= total * frame;
}
