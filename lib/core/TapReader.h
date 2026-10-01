// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "AudioTap.h"

// The loop task's side of an AudioTap: hands over the real audio written
// since the last poll(), placed in its track, and says which track frame is
// being heard at a given time.
//
// Audible time. The outputs write in bursts (Bluetooth pulls ~30 ms at a
// tick, the speaker a 23 ms buffer at a time), so "the last write" jitters.
// The reader keeps a clock that follows the leading edge of the writes: it
// runs at the sample rate, jumps up to any write ahead of it, sags slowly
// (0.3 %) so it can't run away from a writer that is a little slow, and
// starts over after a stall (a pause, or the speaker going idle). The frame
// heard at time t is that clock at t minus the output latency, and never
// later than the last frame written; more than the stall time (60 ms) past
// it, nothing is being heard.
class TapReader {
public:
  struct Run {
    const int16_t* samples;  // mono
    uint32_t frames;
    uint32_t trackFrame;     // of samples[0]
    uint32_t epoch;
  };
  struct Audible {
    bool valid = false;      // false: silence, a fade, a stall, or nothing written yet
    uint32_t trackFrame = 0;
    float frac = 0.0f;
    uint32_t epoch = 0;
  };

  // Follows `tap` from now on (what it already holds is skipped). nullptr: none.
  void attach(const AudioTap* tap, float sampleRate);
  const AudioTap* tap() const { return tap_; }
  void setSampleRate(float rate) { rate_ = rate; }

  // Everything written since the last poll, as runs of real audio of at
  // most `scratchFrames` each, copied to `scratch` and passed to
  // onRun(const Run&). Silence and fades are skipped. Returns the frames
  // handed over. If the writer lapped the reader (it fell more than the
  // tap's capacity behind), what was lost is skipped and counted.
  template <typename F>
  uint32_t poll(int16_t* scratch, uint32_t scratchFrames, F&& onRun);

  // The track frame heard at `nowUs`, `latencyUs` after it was written.
  Audible audibleAt(uint32_t nowUs, uint32_t latencyUs) const;

  uint32_t lostFrames() const { return lost_; }

private:
  void updateClock(uint32_t count, uint32_t us);

  const AudioTap* tap_ = nullptr;
  float rate_ = 44100.0f;
  uint32_t next_ = 0;  // next tap frame to hand over
  uint32_t lost_ = 0;
  // The clock: at refUs_ it stood at refCount_ + refFrac_ (tap frames).
  bool clockValid_ = false;
  uint32_t refCount_ = 0;
  uint32_t refUs_ = 0;
  float refFrac_ = 0.0f;
};

template <typename F>
uint32_t TapReader::poll(int16_t* scratch, uint32_t scratchFrames, F&& onRun) {
  if (!tap_ || scratchFrames == 0) return 0;
  uint32_t count, us;
  tap_->clock(&count, &us);
  updateClock(count, us);
  // Keep a margin: the writer may be writing right behind what we copy.
  const uint32_t keep = tap_->capacity() - tap_->capacity() / 4;
  if (count - next_ > keep) {
    lost_ += count - next_ - keep;
    next_ = count - keep;
  }
  uint32_t handed = 0;
  while (next_ != count) {
    AudioTap::Segment seg;
    if (!tap_->segmentAt(next_, count, &seg) || seg.tapEnd == next_) {
      lost_ += count - next_;  // older than the segments kept
      next_ = count;
      break;
    }
    uint32_t end = seg.tapEnd;
    if (end - next_ > scratchFrames) end = next_ + scratchFrames;
    if (seg.trackStart != AudioTap::kNoTrack) {
      const uint32_t n = end - next_;
      if (!tap_->read(next_, scratch, n)) {
        lost_ += count - next_;
        next_ = count;
        break;
      }
      onRun(Run{scratch, n, seg.trackStart + (next_ - seg.tapStart), seg.epoch});
      handed += n;
    }
    next_ = end;
  }
  return handed;
}
