// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Byte streams for what the player saves and loads (the library index's
// cache, the queue): the firmware wraps a file on the card, the host tests
// and the queue's remap around a library rebuild use a buffer.
class ByteSink {
public:
  virtual ~ByteSink() = default;
  // All `n` bytes, or false (the sink is then unusable).
  virtual bool write(const void* data, size_t n) = 0;
};

class ByteSource {
public:
  virtual ~ByteSource() = default;
  // Up to `n` bytes; how many were read (0: the end, or an error).
  virtual size_t read(void* data, size_t n) = 0;
};

// Reads exactly `n` bytes (false: the source ended first).
inline bool readFully(ByteSource& in, void* data, size_t n) {
  auto* p = static_cast<uint8_t*>(data);
  while (n) {
    const size_t got = in.read(p, n);
    if (got == 0) return false;
    p += got;
    n -= got;
  }
  return true;
}

// A growable buffer from allocator hooks (the firmware points them at
// PSRAM). Grows by doubling; a failed growth leaves it failed().
class MemorySink : public ByteSink {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  explicit MemorySink(AllocFn alloc = nullptr, FreeFn release = nullptr)
      : alloc_(alloc ? alloc : heapAlloc), free_(release ? release : heapFree) {}
  ~MemorySink() override { free_(data_); }
  MemorySink(const MemorySink&) = delete;
  MemorySink& operator=(const MemorySink&) = delete;

  bool write(const void* data, size_t n) override {
    if (failed_) return false;
    if (size_ + n > cap_) {
      size_t cap = cap_ ? cap_ * 2 : 256;
      while (cap < size_ + n) cap *= 2;
      auto* p = static_cast<uint8_t*>(alloc_(cap));
      if (!p) {
        failed_ = true;
        return false;
      }
      if (size_) std::memcpy(p, data_, size_);
      free_(data_);
      data_ = p;
      cap_ = cap;
    }
    if (n) std::memcpy(data_ + size_, data, n);
    size_ += n;
    return true;
  }

  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }
  bool failed() const { return failed_; }

private:
  static void* heapAlloc(size_t n) { return std::malloc(n); }
  static void heapFree(void* p) { std::free(p); }

  AllocFn alloc_;
  FreeFn free_;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t cap_ = 0;
  bool failed_ = false;
};

// Reads a buffer it doesn't own, at most `chunk` bytes a call (tests use a
// small one to split every read).
class MemorySource : public ByteSource {
public:
  MemorySource(const void* data, size_t size, size_t chunk = SIZE_MAX)
      : data_(static_cast<const uint8_t*>(data)), size_(size), chunk_(chunk ? chunk : 1) {}

  size_t read(void* out, size_t n) override {
    if (n > size_ - at_) n = size_ - at_;
    if (n > chunk_) n = chunk_;
    if (n) std::memcpy(out, data_ + at_, n);
    at_ += n;
    return n;
  }

private:
  const uint8_t* data_;
  size_t size_;
  size_t chunk_;
  size_t at_ = 0;
};
