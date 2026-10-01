// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RingFeed.h"

#include <cstring>  // std::memmove

void RingFeed::reset(uint32_t cpuMhz, bool hiRes) {
  conv_.reset();
  staged_ = 0;
  budget_ = 0;
  ringBudget_ = 0;
  cpuMhz_ = cpuMhz;
  hiRes_ = hiRes;
  rate_ = 0;
  made_ = 0;
}

bool RingFeed::setRate(int hz) {
  if (hz <= 0) return !conv_.refused();
  rate_ = hz;
  return conv_.setRate(static_cast<uint32_t>(hz), cpuMhz_, hiRes_);
}

bool RingFeed::commit() {
  if (staged_ == 0) return true;
  const uint32_t n = discard_ ? staged_ : ring_.write(stage_, staged_);
  if (n < staged_) std::memmove(stage_, stage_ + 2 * n, (staged_ - n) * 2 * sizeof(int16_t));  // keep the rest, in order
  staged_ -= n;
  return staged_ == 0;
}

bool RingFeed::room() {
  if (kStageFrames - staged_ >= conv_.maxOut()) return true;
  commit();
  return kStageFrames - staged_ >= conv_.maxOut();
}

bool RingFeed::consume(const int16_t sample[2]) {
  if (conv_.refused() || budget_ == 0 || ringBudget_ == 0 || !room()) return false;
  const uint32_t n = conv_.push(sample, stage_ + 2 * staged_);
  staged_ += n;
  made_ += n;
  --budget_;
  ringBudget_ -= n < ringBudget_ ? n : ringBudget_;
  return true;
}

uint32_t RingFeed::write(const int16_t* frames, uint32_t n, uint32_t maxMade) {
  uint32_t taken = 0;
  uint32_t left = maxMade;  // ring frames this call may still make
  while (taken < n && !conv_.refused() && left >= conv_.maxOut() && room()) {
    // process() takes a frame only while `space` has room for maxOut()
    // more: never past the stage's end, nor past `left`.
    const uint32_t space = kStageFrames - staged_ < left ? kStageFrames - staged_ : left;
    uint32_t written = 0;
    taken += conv_.process(frames + 2 * taken, n - taken, stage_ + 2 * staged_, space, &written);
    staged_ += written;
    made_ += written;
    left -= written;
  }
  return taken;
}

bool RingFeed::finish() {
  while (!conv_.finished()) {
    if (!room()) return false;
    const uint32_t n = conv_.finishPush(stage_ + 2 * staged_);
    staged_ += n;
    made_ += n;
  }
  return commit();
}

uint32_t RingFeed::roomFor(uint32_t srcFrames) const {
  const RateConverter::Plan p = conv_.configured() ? conv_.currentPlan() : RateConverter::plan(8000, 0);
  return staged_ + static_cast<uint32_t>(p.ringFrames(srcFrames)) + RateConverter::kMaxOut;
}
