#include "PcmRing.h"

#include <algorithm>  // std::min
#include <cstring>    // std::memcpy

namespace {
constexpr uint32_t kChannels = 2;
}  // namespace

PcmRing::PcmRing(int16_t* buffer, uint32_t capacityFrames, uint32_t initialIndex)
    : buf_(buffer),
      cap_(capacityFrames),
      mask_(capacityFrames - 1),
      writeIdx_(initialIndex),
      readIdx_(initialIndex) {}

uint32_t PcmRing::size() const {
  return writeIdx_.load(std::memory_order_acquire) - readIdx_.load(std::memory_order_acquire);
}

uint32_t PcmRing::space() const { return cap_ - size(); }

uint32_t PcmRing::write(const int16_t* frames, uint32_t count) {
  const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
  const uint32_t n = std::min(count, space());
  // Copy in up to two runs: to the end of the buffer, then from its start.
  const uint32_t start = w & mask_;
  const uint32_t first = std::min(n, cap_ - start);
  std::memcpy(buf_ + start * kChannels, frames, first * kChannels * sizeof(int16_t));
  std::memcpy(buf_, frames + first * kChannels, (n - first) * kChannels * sizeof(int16_t));
  writeIdx_.store(w + n, std::memory_order_release);
  return n;
}

uint32_t PcmRing::read(uint8_t id, int16_t* out, uint32_t count, uint32_t* epoch) {
  std::unique_lock<std::mutex> lock(lock_, std::try_to_lock);
  if (!lock.owns_lock() || id != consumer_.load(std::memory_order_relaxed)) return 0;
  // Under the lock, so it can't change between here and the frames below.
  if (epoch) *epoch = epoch_.load(std::memory_order_relaxed);

  const uint32_t r = readIdx_.load(std::memory_order_relaxed);
  const uint32_t n = std::min(count, writeIdx_.load(std::memory_order_acquire) - r);
  const uint32_t start = r & mask_;
  const uint32_t first = std::min(n, cap_ - start);
  std::memcpy(out, buf_ + start * kChannels, first * kChannels * sizeof(int16_t));
  std::memcpy(out + first * kChannels, buf_, (n - first) * kChannels * sizeof(int16_t));
  readIdx_.store(r + n, std::memory_order_release);
  return n;
}

uint32_t PcmRing::discardAll() {
  std::lock_guard<std::mutex> lock(lock_);
  const uint32_t w = writeIdx_.load(std::memory_order_relaxed);
  readIdx_.store(w, std::memory_order_release);
  epoch_.store(epoch_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
  return w;
}

void PcmRing::setConsumer(uint8_t id) {
  std::lock_guard<std::mutex> lock(lock_);
  consumer_.store(id, std::memory_order_release);
}
