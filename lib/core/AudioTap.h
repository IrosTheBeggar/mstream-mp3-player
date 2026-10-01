// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>

// A copy of what one output just played, for the beat tracker: mono
// ((L + R) / 2) int16 in a history ring the caller provides (PSRAM on the
// Core2; capacity a power of two). Written by exactly one task, the output's
// own (the Bluetooth data callback, or the speaker pump), which only copies
// and publishes: no locks, no allocation, nothing that can block. Read by
// the loop task.
//
// Tap frames are counted by a free-running 32-bit counter (count(),
// release/acquire), with a 32-bit microsecond timestamp of the last write.
// Both wrap harmlessly; nothing here is 64-bit, which isn't lock-free on
// the ESP32. A second counter runs ahead of it by the write in progress:
// that write is already overwriting the oldest frames before count() moves,
// so read() checks against it, not count().
//
// Each write also says where its audio came from: how many of its frames
// are real (from the ring; the rest are fades or silence), the ring's epoch
// and the first real frame's position in it (the track frame, the counter
// positionMs() is made of). The tap keeps that as segments: a new one
// starts wherever the track position doesn't follow on (a skip, a pause,
// an underrun, silence), so any recent tap frame can be placed in its track.
//
// It can be switched off (setEnabled(false), from any task): write() then
// copies nothing and the count stands still. It is on only while the Dance
// tab is up (ENERGY.md item 9). The first write after it is switched back
// on never continues the segment from before: what played meanwhile isn't
// in the tap, so the frames on either side aren't contiguous in time even
// when the track position follows on.
class AudioTap {
public:
  static constexpr uint32_t kNoTrack = 0xFFFFFFFFu;  // a segment of silence or fade
  static constexpr uint32_t kSegments = 8;           // segments kept

  struct Segment {
    uint32_t tapStart = 0;         // its first tap frame
    uint32_t tapEnd = 0;           // one past its last (the next segment's start, or the limit asked for)
    uint32_t trackStart = kNoTrack;  // track frame of tapStart, kNoTrack for silence
    uint32_t epoch = 0;            // the ring's epoch
  };

  AudioTap(int16_t* buffer, uint32_t capacityFrames);

  // Any task. On from construction.
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // ---- writer (one task) ----
  // `frames` interleaved stereo frames just played; the first `realFrames`
  // came from the ring, the first of them at `trackFrame` of epoch `epoch`.
  // Nothing while switched off.
  void write(const int16_t* stereo, uint32_t frames, uint32_t realFrames, uint32_t epoch, uint32_t trackFrame,
             uint32_t nowUs);

  // ---- reader (one task) ----
  // Frames written so far, and when the last write happened, read together.
  uint32_t count() const { return count_.load(std::memory_order_acquire); }
  void clock(uint32_t* count, uint32_t* us) const;
  // Copies tap frames [from, from + n). False: some were overwritten already
  // (the reader fell more than a capacity behind); `out` is then garbage.
  bool read(uint32_t from, int16_t* out, uint32_t n) const;
  // The segment holding tap frame `frame`, clipped to `limit` (a count() the
  // reader took). False: older than the segments kept, or nothing written.
  bool segmentAt(uint32_t frame, uint32_t limit, Segment* out) const;
  uint32_t capacity() const { return cap_; }

private:
  struct Slot {
    std::atomic<uint32_t> tapStart{0};
    std::atomic<uint32_t> trackStart{kNoTrack};
    std::atomic<uint32_t> epoch{0};
  };
  void startSegment(uint32_t tapStart, uint32_t trackStart, uint32_t epoch);

  int16_t* const buf_;
  const uint32_t cap_;
  const uint32_t mask_;
  std::atomic<uint32_t> count_{0};
  std::atomic<uint32_t> head_{0};  // count_ plus the write in progress
  std::atomic<uint32_t> us_{0};
  // Segment table under a sequence lock: odd while the writer changes it.
  std::atomic<uint32_t> seq_{0};
  std::atomic<uint32_t> segments_{0};  // started so far (free-running)
  Slot slots_[kSegments];
  std::atomic<bool> enabled_{true};

  // Writer only.
  bool skipped_ = false;       // a write was dropped while switched off
  bool real_ = false;          // the current segment is real audio
  uint32_t epoch_ = 0;
  uint32_t nextTrack_ = 0;     // track frame that would continue it
};
