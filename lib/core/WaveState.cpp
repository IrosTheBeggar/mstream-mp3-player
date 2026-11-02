// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "WaveState.h"

#include <algorithm>

namespace cc = cardcontract;

void WaveState::begin(bool named) {
  file_ = named ? File::Named : File::None;
  why_ = cc::Why::Ok;
}

void WaveState::want(uint64_t pathHash) {
  if (pathHash == wanted_) return;
  const bool had = holds();
  wanted_ = pathHash;
  ioFails_ = 0;
  waiting_ = false;
  if (had || holds()) ++version_;
}

bool WaveState::wantsRead(uint32_t nowMs) const {
  if (wanted_ == 0 || file_ != File::Named || running_ || injected_) return false;
  if (heldHash_ == wanted_ || ioFails_ >= kIoReads) return false;
  // (Elapsed, not an end: no stale wait comes back when millis() wraps.)
  return !waiting_ || nowMs - waitFromMs_ >= waitMs_;
}

void WaveState::started() {
  running_ = true;
  jobHash_ = wanted_;
  jobGeneration_ = generation_;
}

void WaveState::refused(uint32_t nowMs) { wait(nowMs, kRefusedMs); }

void WaveState::wait(uint32_t nowMs, uint32_t forMs) {
  waiting_ = true;
  waitFromMs_ = nowMs;
  waitMs_ = forMs;
}

WaveState::Finish WaveState::finished(const wave::ReadResult& r, uint32_t nowMs) {
  if (!running_) return Finish::Dropped;
  running_ = false;
  ++reads_;
  slotReads_ += r.slotReads;
  jobMsSum_ += r.jobMs;
  jobMsMax_ = std::max(jobMsMax_, r.jobMs);
  if (jobGeneration_ != generation_ || jobHash_ != wanted_ || injected_) {
    ++dropped_;
    return Finish::Dropped;
  }
  switch (r.read) {
    case wave::Read::Hit:
      ++hits_;
      heldHash_ = jobHash_;
      heldShape_ = true;
      shape_ = r.shape;
      ++version_;
      return Finish::Recorded;
    case wave::Read::Miss:
      ++misses_;
      heldHash_ = jobHash_;
      heldShape_ = false;
      return Finish::None;
    case wave::Read::Damaged:
      ++damaged_;
      heldHash_ = jobHash_;
      heldShape_ = false;
      return Finish::None;
    case wave::Read::Absent:
      ++absents_;
      file_ = File::Absent;
      why_ = r.why;
      heldHash_ = 0;
      heldShape_ = false;
      return Finish::Absent;
    case wave::Read::Io:
      break;
  }
  ++ioErrors_;
  if (++ioFails_ >= kIoReads) return Finish::GaveUp;
  wait(nowMs, kIoRetryMs);
  return Finish::Retry;
}

void WaveState::remounted(bool named) {
  if (holds()) ++version_;
  ++generation_;
  heldHash_ = 0;
  heldShape_ = false;
  ioFails_ = 0;
  waiting_ = false;
  begin(named);
}

const wave::Shape* WaveState::shape() const { return injected_ || holds() ? &shape_ : nullptr; }

void WaveState::inject(const uint8_t* heights, uint32_t lengthMs) {
  injected_ = true;
  heldHash_ = 0;
  heldShape_ = false;
  shape_.lengthMs = lengthMs;
  for (int i = 0; i < wave::kColumns; ++i)
    shape_.heights[i] = std::min(heights[i], static_cast<uint8_t>(wave::kMaxHeight));
  ++version_;
}

void WaveState::dropInjected() {
  if (!injected_) return;
  injected_ = false;
  ++version_;
}

uint32_t WaveState::meanJobMs() const {
  return reads_ ? static_cast<uint32_t>((jobMsSum_ + reads_ / 2) / reads_) : 0;
}
