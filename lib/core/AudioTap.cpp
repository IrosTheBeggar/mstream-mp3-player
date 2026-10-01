// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AudioTap.h"

AudioTap::AudioTap(int16_t* buffer, uint32_t capacityFrames)
    : buf_(buffer), cap_(capacityFrames), mask_(capacityFrames - 1) {}

void AudioTap::startSegment(uint32_t tapStart, uint32_t trackStart, uint32_t epoch) {
  const uint32_t s = seq_.load(std::memory_order_relaxed);
  seq_.store(s + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const uint32_t n = segments_.load(std::memory_order_relaxed);
  Slot& slot = slots_[n % kSegments];
  slot.tapStart.store(tapStart, std::memory_order_relaxed);
  slot.trackStart.store(trackStart, std::memory_order_relaxed);
  slot.epoch.store(epoch, std::memory_order_relaxed);
  segments_.store(n + 1, std::memory_order_relaxed);
  seq_.store(s + 2, std::memory_order_release);
}

void AudioTap::write(const int16_t* stereo, uint32_t frames, uint32_t realFrames, uint32_t epoch,
                     uint32_t trackFrame, uint32_t nowUs) {
  if (frames == 0) return;
  if (!enabled_.load(std::memory_order_relaxed)) {
    skipped_ = true;
    return;
  }
  if (realFrames > frames) realFrames = frames;
  const uint32_t c = count_.load(std::memory_order_relaxed);
  if (skipped_) {
    // Audio played that the tap doesn't have: close the real segment with
    // an empty one of silence, so what comes next starts its own.
    skipped_ = false;
    if (real_) {
      startSegment(c, kNoTrack, epoch_);
      real_ = false;
    }
  }
  const bool any = segments_.load(std::memory_order_relaxed) > 0;
  if (realFrames > 0) {
    if (!real_ || epoch != epoch_ || trackFrame != nextTrack_) startSegment(c, trackFrame, epoch);
    real_ = true;
    epoch_ = epoch;
    nextTrack_ = trackFrame + realFrames;
  }
  if (realFrames < frames && (real_ || !any)) {
    startSegment(c + realFrames, kNoTrack, epoch_);
    real_ = false;
  }
  for (uint32_t i = 0; i < frames; ++i) {
    const int32_t mono = (static_cast<int32_t>(stereo[2 * i]) + stereo[2 * i + 1]) >> 1;
    buf_[(c + i) & mask_] = static_cast<int16_t>(mono);
  }
  us_.store(nowUs, std::memory_order_relaxed);
  count_.store(c + frames, std::memory_order_release);
}

void AudioTap::clock(uint32_t* count, uint32_t* us) const {
  // The two are written separately; take a pair the count didn't move across.
  uint32_t c = count_.load(std::memory_order_acquire);
  for (int tries = 0; tries < 4; ++tries) {
    const uint32_t t = us_.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t again = count_.load(std::memory_order_acquire);
    if (again == c) {
      *count = c;
      *us = t;
      return;
    }
    c = again;
  }
  *count = c;
  *us = us_.load(std::memory_order_relaxed);
}

bool AudioTap::read(uint32_t from, int16_t* out, uint32_t n) const {
  if (count_.load(std::memory_order_acquire) - from > cap_) return false;
  for (uint32_t i = 0; i < n; ++i) out[i] = buf_[(from + i) & mask_];
  std::atomic_thread_fence(std::memory_order_acquire);
  // Still there after the copy: the writer didn't lap us meanwhile.
  return count_.load(std::memory_order_relaxed) - from <= cap_;
}

bool AudioTap::segmentAt(uint32_t frame, uint32_t limit, Segment* out) const {
  for (int tries = 0; tries < 8; ++tries) {
    const uint32_t s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1) continue;
    const uint32_t n = segments_.load(std::memory_order_relaxed);
    const uint32_t kept = n < kSegments ? n : kSegments;
    bool found = false;
    Segment seg;
    uint32_t nextStart = limit;
    for (uint32_t k = 0; k < kept; ++k) {
      const Slot& slot = slots_[(n - 1 - k) % kSegments];
      const uint32_t start = slot.tapStart.load(std::memory_order_relaxed);
      if (static_cast<int32_t>(frame - start) >= 0) {
        seg.tapStart = start;
        seg.trackStart = slot.trackStart.load(std::memory_order_relaxed);
        seg.epoch = slot.epoch.load(std::memory_order_relaxed);
        // A segment published after `limit` was taken starts at or after it.
        seg.tapEnd = static_cast<int32_t>(limit - nextStart) < 0 ? limit : nextStart;
        found = true;
        break;
      }
      nextStart = start;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (seq_.load(std::memory_order_relaxed) != s1) continue;
    if (found) *out = seg;
    return found;
  }
  return false;
}
