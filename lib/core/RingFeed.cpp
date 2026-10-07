// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RingFeed.h"

#include <cstring>  // std::memmove

void RingFeed::reset(uint32_t cpuMhz, bool hiRes) {
  conv_.reset();
  mode_ = Mode::Hold;
  mono_ = false;
  staged_ = 0;
  heldN_ = 0;
  accept_ = 0;
  perFrame_ = 1;
  passed_ = 0;
  budget_ = 0;
  ringBudget_ = 0;
  cpuMhz_ = cpuMhz;
  hiRes_ = hiRes;
  rate_ = 0;
  forced_ = 0;
  made_ = 0;
}

void RingFeed::updateMode() {
  countPassed();
  accept_ = 0;
  if (conv_.refused()) {
    mode_ = Mode::Refused;
  } else if (!conv_.configured()) {
    mode_ = Mode::Hold;
  } else if (conv_.passthrough() && conv_.carried() == 0) {
    mode_ = Mode::Pass;
    // The passthrough checks only budget_ per frame; its frames are ring
    // frames one for one, so the ring budget (what a block path spent
    // earlier in the pass) caps it here.
    if (budget_ > ringBudget_) budget_ = ringBudget_;
  } else {
    mode_ = Mode::Block;
    perFrame_ = conv_.perFrameMax();
  }
}

bool RingFeed::setRate(int hz) {
  if (hz <= 0) return !conv_.refused();
  if (forced_ > 0) hz = forced_;
  convertHeld();  // taken at the rate before (a change mid-stream restarts the filters after them)
  countPassed();
  rate_ = hz;
  const bool ok = conv_.setRate(static_cast<uint32_t>(hz), cpuMhz_, hiRes_);
  updateMode();
  return ok;
}

void RingFeed::setChannels(int channels) {
  const bool mono = channels == 1;
  if (mono == mono_) return;
  convertHeld();  // the held frames as they were taken
  mono_ = mono;
  conv_.setMono(mono);
}

void RingFeed::convertHeld() {
  accept_ = 0;
  if (heldN_ == 0) return;
  const uint32_t n = conv_.convert(held_, heldN_, stage_ + 2 * staged_);
  staged_ += n;
  heldN_ = 0;
  ringBudget_ -= n < ringBudget_ ? n : ringBudget_;
}

bool RingFeed::commit() {
  convertHeld();
  countPassed();
  if (staged_ == 0) return true;
  const uint32_t n = discard_ ? staged_ : ring_.write(stage_, staged_);
  if (n < staged_) std::memmove(stage_, stage_ + 2 * n, (staged_ - n) * 2 * sizeof(int16_t));  // keep the rest, in order
  staged_ -= n;
  made_ += n;
  return staged_ == 0;
}

bool RingFeed::reserve() {
  convertHeld();
  updateMode();  // the frames replayed at 44.1 kHz are out: the passthrough
  if (mode_ != Mode::Block) return mode_ == Mode::Pass;
  const uint32_t carried = conv_.carried();
  // Room for a whole block if the ring takes the stage, else what fits.
  if (kStageFrames - staged_ < carried + kBlockFrames * perFrame_) commit();
  const uint32_t free = kStageFrames - staged_;
  if (free < carried + perFrame_ || ringBudget_ == 0) return false;  // the ring is full, or the pass is done
  uint32_t n = (free - carried) / perFrame_;
  const uint32_t byBudget = (ringBudget_ + perFrame_ - 1) / perFrame_;  // the last may go past it by perFrame_ - 1
  if (n > byBudget) n = byBudget;
  accept_ = n < kBlockFrames ? n : kBlockFrames;
  return true;
}

bool RingFeed::consumeSlow(const int16_t sample[2]) {
  switch (mode_) {
    case Mode::Pass:  // (consume() handles it; here after reserve() found the passthrough)
      return consume(sample);
    case Mode::Block:
      // accept_ is spent: convert the block, then room for the next one.
      if (budget_ == 0 || !reserve()) return false;
      return consume(sample);
    case Mode::Hold: {
      // No rate yet: the converter holds the frame (or, once its hold is
      // full, takes the stream as 44.1 kHz), one at a time.
      if (budget_ == 0 || ringBudget_ == 0 || !room()) return false;
      const uint32_t n = conv_.push(sample, stage_ + 2 * staged_);
      staged_ += n;
      --budget_;
      ringBudget_ -= n < ringBudget_ ? n : ringBudget_;
      updateMode();
      return true;
    }
    case Mode::Refused:
    default:
      return false;
  }
}

bool RingFeed::room() {
  if (kStageFrames - staged_ >= conv_.maxOut()) return true;
  commit();
  return kStageFrames - staged_ >= conv_.maxOut();
}

uint32_t RingFeed::write(const int16_t* frames, uint32_t n, uint32_t maxMade) {
  convertHeld();
  countPassed();
  uint32_t taken = 0;
  uint32_t left = maxMade;  // ring frames this call may still make
  while (taken < n && !conv_.refused() && left >= conv_.maxOut() && room()) {
    // process() takes a frame only while `space` has room for maxOut()
    // more: never past the stage's end, nor past `left`.
    const uint32_t space = kStageFrames - staged_ < left ? kStageFrames - staged_ : left;
    uint32_t written = 0;
    taken += conv_.process(frames + 2 * taken, n - taken, stage_ + 2 * staged_, space, &written);
    staged_ += written;
    left -= written;
  }
  updateMode();
  return taken;
}

uint32_t RingFeed::writeBudgeted(const int16_t* frames, uint32_t n) {
  if (n > budget_) n = budget_;
  if (n == 0 || ringBudget_ == 0) return 0;
  if (mode_ == Mode::Pass) {
    // 44.1 kHz: straight into the stage, as consume() does it, a block at
    // a time (budget_ is already capped to the ring budget here).
    uint32_t taken = 0;
    while (taken < n) {
      if (staged_ == kStageFrames) {
        commit();
        if (staged_ == kStageFrames) break;  // the ring is full
      }
      uint32_t k = kStageFrames - staged_;
      if (k > n - taken) k = n - taken;
      if (mono_) {
        for (uint32_t i = 0; i < k; ++i) {
          storeFrame(stage_ + 2 * (staged_ + i), frames[2 * (taken + i)], frames[2 * (taken + i)]);
        }
      } else {
        std::memcpy(stage_ + 2 * staged_, frames + 2 * taken, k * 2 * sizeof(int16_t));
      }
      staged_ += k;
      passed_ += k;
      budget_ -= k;
      taken += k;
    }
    return taken;
  }
  // Another rate (or none yet): write()'s block path. Frames held for the
  // per-frame path and passthrough frames not yet counted go first, on
  // their own (write() does the same), so that only this block's ring
  // frames come off the ring budget below.
  convertHeld();
  countPassed();
  const uint64_t before = made();
  const uint32_t taken = write(frames, n, ringBudget_);
  budget_ -= taken;
  const uint64_t ringMade = made() - before;
  ringBudget_ -= ringMade < ringBudget_ ? static_cast<uint32_t>(ringMade) : ringBudget_;
  return taken;
}

bool RingFeed::finish() {
  convertHeld();
  countPassed();
  while (!conv_.finished()) {
    if (!room()) return false;
    staged_ += conv_.finishPush(stage_ + 2 * staged_);
  }
  updateMode();
  return commit();
}

uint32_t RingFeed::roomFor(uint32_t srcFrames) const {
  const RateConverter::Plan p = conv_.configured() ? conv_.currentPlan() : RateConverter::plan(8000, 0);
  return staged_ + heldN_ * perFrame_ + static_cast<uint32_t>(p.ringFrames(srcFrames)) + RateConverter::kMaxOut;
}

bool RingFeed::mark(Mark* m) const {
  if (staged_ != 0 || heldN_ != 0 || passed_ != 0) return false;
  m->conv = conv_;
  m->made = made_;
  m->rate = rate_;
  m->forced = forced_;
  m->perFrame = perFrame_;
  m->mode = mode_;
  m->mono = mono_;
  return true;
}

void RingFeed::rewind(const Mark& m) {
  conv_ = m.conv;
  made_ = m.made;
  rate_ = m.rate;
  forced_ = m.forced;
  perFrame_ = m.perFrame;
  mode_ = m.mode;
  mono_ = m.mono;
  staged_ = 0;
  heldN_ = 0;
  accept_ = 0;
  passed_ = 0;
  budget_ = 0;
  ringBudget_ = 0;
}

void RingFeed::restartStream() {
  const uint64_t made = made_;  // (nothing staged after finish())
  const int forced = forced_;
  reset(cpuMhz_, hiRes_);
  made_ = made;
  forced_ = forced;
}
