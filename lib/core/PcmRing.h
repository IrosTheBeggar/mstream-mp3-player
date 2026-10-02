// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

// Ring buffer of interleaved stereo int16 frames between the decode task (the
// single producer) and whichever output is active (the Bluetooth callback or
// the speaker pump). The buffer is caller-provided so the firmware can put it
// in PSRAM; capacity must be a power of two.
//
// Indices are free-running uint32 frame counters, so wraparound is harmless and
// readPos() doubles as "frames played". write() is lock-free.
//
// Two outputs share the ring but only the one selected with setConsumer() may
// read; the other gets 0 frames. read() only ever *tries* to take the ring's
// mutex and reads nothing if it is busy, so the Bluetooth callback never blocks
// (it plays silence for that tick). The mutex is taken for real only by
// setConsumer() and discardAll(), which are rare and hold it for a few index
// updates, and it serializes them against an in-progress read.
//
// cutBack() (docs/GAPLESS.md section 5.1) takes the frames written after an
// index back out, if the consumer hasn't read past it: a track decoded
// ahead that is no longer what comes next. It takes no lock, so a read never
// fails for it. Instead the producer raises a fence at the index and the
// consumer marks each read (reading_): a sequentially consistent pair
// (Dekker's), so either the read sees the fence and stops at it, or the
// producer sees the read under way and tries again later (Pending).
class PcmRing {
public:
  static constexpr uint8_t kNoConsumer = 0;
  // cutBack()'s outcome.
  enum class Cut : uint8_t {
    Done,     // the write index is back at the cut: nothing after it is read
    Pending,  // a read is under way: the fence stays up, call again
    Crossed,  // the consumer has read past the cut: nothing changed
  };

  // `initialIndex` exists so tests can start the counters near the 2^32 wrap.
  PcmRing(int16_t* buffer, uint32_t capacityFrames, uint32_t initialIndex = 0);

  // ---- producer ----
  // Copies up to `count` frames; returns how many fit.
  uint32_t write(const int16_t* frames, uint32_t count);
  uint32_t space() const;
  // Drops every unread frame (track change) and bumps epoch(). Returns the
  // index the next written frame will get, i.e. the start of the new track in
  // readPos() terms.
  uint32_t discardAll();
  // The index the next written frame gets (free-running, as readPos()).
  uint32_t writePos() const { return writeIdx_.load(std::memory_order_acquire); }
  // Takes back every frame written at `index` or after it (an index this
  // producer wrote, at most writePos()), if the consumer hasn't read past
  // `index`. Pending: a read was under way; the fence stays up at `index`
  // (reads stop there) until the next call, which must be for the same
  // index; discardAll() takes it down too. Never blocks, never makes a read
  // fail: only a read that reaches `index` while the fence is up is cut
  // short there.
  Cut cutBack(uint32_t index);

  // ---- control ----
  // Hands the right to read to `id` (kNoConsumer: nobody). Waits for a read
  // already in progress, so two outputs never read at the same time.
  void setConsumer(uint8_t id);
  uint8_t consumer() const { return consumer_.load(std::memory_order_acquire); }

  // ---- consumer ----
  // Copies up to `count` frames into `out` and returns how many. Returns 0
  // without blocking if `id` isn't the current consumer or the ring is busy
  // with setConsumer()/discardAll(). When it did get to read (even 0 frames)
  // it stores the epoch the frames belong to in `*epoch`; otherwise `*epoch`
  // is left alone. A consumer that sees it change knows the audio jumped (a
  // skip) and can crossfade. `*position` (same rule) gets where the first
  // frame read sits in its epoch: frames since the discardAll() that began it,
  // the count positionMs() is made of (readPos() minus the track's start).
  uint32_t read(uint8_t id, int16_t* out, uint32_t count, uint32_t* epoch = nullptr,
                uint32_t* position = nullptr);
  // Total frames consumed so far (free-running).
  uint32_t readPos() const { return readIdx_.load(std::memory_order_acquire); }
  // Number of discardAll() calls so far (free-running).
  uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

  // ---- either side ----
  uint32_t size() const;  // frames ready to read
  uint32_t capacity() const { return cap_; }

private:
  friend class PcmRingProbe;  // the host tests' look at the reading mark

  int16_t* const buf_;
  const uint32_t cap_;
  const uint32_t mask_;
  std::atomic<uint32_t> writeIdx_;
  std::atomic<uint32_t> readIdx_;
  std::atomic<uint8_t> consumer_{kNoConsumer};
  std::atomic<uint32_t> epoch_{0};  // written only under lock_
  uint32_t epochStart_;             // readIdx_ when epoch_ began; under lock_
  std::mutex lock_;
  // cutBack()'s fence and the consumer's mark (seq_cst: see the class).
  std::atomic<bool> fenced_{false};
  std::atomic<uint32_t> fenceIdx_{0};
  std::atomic<bool> reading_{false};
};
