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
class PcmRing {
public:
  static constexpr uint8_t kNoConsumer = 0;

  // `initialIndex` exists so tests can start the counters near the 2^32 wrap.
  PcmRing(int16_t* buffer, uint32_t capacityFrames, uint32_t initialIndex = 0);

  // ---- producer ----
  // Copies up to `count` frames; returns how many fit.
  uint32_t write(const int16_t* frames, uint32_t count);
  uint32_t space() const;
  // Drops every unread frame (track change). Returns the index the next
  // written frame will get, i.e. the start of the new track in readPos() terms.
  uint32_t discardAll();

  // ---- control ----
  // Hands the right to read to `id` (kNoConsumer: nobody). Waits for a read
  // already in progress, so two outputs never read at the same time.
  void setConsumer(uint8_t id);
  uint8_t consumer() const { return consumer_.load(std::memory_order_acquire); }

  // ---- consumer ----
  // Copies up to `count` frames into `out` and returns how many. Returns 0
  // without blocking if `id` isn't the current consumer or the ring is busy
  // with setConsumer()/discardAll().
  uint32_t read(uint8_t id, int16_t* out, uint32_t count);
  // Total frames consumed so far (free-running).
  uint32_t readPos() const { return readIdx_.load(std::memory_order_acquire); }

  // ---- either side ----
  uint32_t size() const;  // frames ready to read
  uint32_t capacity() const { return cap_; }

private:
  int16_t* const buf_;
  const uint32_t cap_;
  const uint32_t mask_;
  std::atomic<uint32_t> writeIdx_;
  std::atomic<uint32_t> readIdx_;
  std::atomic<uint8_t> consumer_{kNoConsumer};
  std::mutex lock_;
};
