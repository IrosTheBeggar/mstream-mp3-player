// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StreamRestart.h"

bool StreamRestart::callback(int64_t nowUs, int32_t count, uint32_t epoch) {
  if (count <= 0) {
    flushed_ = true;
    return false;
  }
  const bool first = !any_;
  const int64_t gap = nowUs - lastUs_;
  any_ = true;
  lastUs_ = nowUs;
  const bool newStream = epoch != seenEpoch_;  // a START (or a new link) since the last callback
  seenEpoch_ = epoch;
  const bool flushed = flushed_;
  flushed_ = false;
  // The gap statistics: every wait of a running stream, however long (a
  // stall of a second or more is the one worth seeing), none after a flush
  // (the stream was stopped: that wait is its stop, however short).
  statGapUs_ = 0;
  if (!first && !flushed && gap > 0) {
    statGapUs_ = gap > static_cast<int64_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(gap);
  }
  return flushed || first || gap >= kGapResetUs || (newStream && gap >= kRestartGapUs);
}
