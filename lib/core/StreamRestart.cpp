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
  statGapUs_ = !first && gap > 0 && gap < kGapResetUs ? static_cast<uint32_t>(gap) : 0;
  const bool flushed = flushed_;
  flushed_ = false;
  return flushed || first || gap >= kGapResetUs || (newStream && gap >= kRestartGapUs);
}
