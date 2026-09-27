#pragma once
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ByteStream.h"

// ByteSink/ByteSource over an open fs::File (the card or LittleFS), for
// the portable savers: LibraryIndex::save()/load(), queuetext.
//
// Big blocks go in kChunk pieces: one f_write holds the FAT volume's lock
// for all of it, and the decoder reads the playing track through the same
// lock. A library cache block is hundreds of KB at 10,000 tracks (a g0
// rebuild while playing): in one piece that's 0.3-1 s the decoder waits. In
// 4 KB pieces it gets the lock between them (it runs above the loop, so it
// takes it the moment it's released), and a yield lets it run.
namespace filestream {
constexpr size_t kChunk = 4096;
}

class FileSink : public ByteSink {
public:
  explicit FileSink(fs::File& f) : f_(f) {}
  bool write(const void* data, size_t n) override {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n) {
      const size_t k = n < filestream::kChunk ? n : filestream::kChunk;
      if (f_.write(p, k) != k) return false;
      p += k;
      n -= k;
      if (n) taskYIELD();
    }
    return true;
  }

private:
  fs::File& f_;
};

class FileSource : public ByteSource {
public:
  explicit FileSource(fs::File& f) : f_(f) {}
  size_t read(void* data, size_t n) override {
    auto* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < n) {
      const size_t want = n - got < filestream::kChunk ? n - got : filestream::kChunk;
      const size_t k = f_.read(p + got, want);
      got += k;
      if (k < want) break;  // the end, or an error
      if (got < n) taskYIELD();
    }
    return got;
  }

private:
  fs::File& f_;
};

// A FileSink that gathers small writes (the queue's lines) into a buffer
// the caller owns (in PSRAM) and passes them on a buffer at a time. It can
// outlive a loop pass: reset() points it at an open file.
class BufferedFileSink : public ByteSink {
public:
  void reset(fs::File* f, uint8_t* buf, size_t size) {
    f_ = f;
    buf_ = buf;
    size_ = size;
    used_ = 0;
  }
  bool write(const void* data, size_t n) override {
    if (!f_ || !buf_) return false;
    const auto* p = static_cast<const uint8_t*>(data);
    while (n) {
      if (used_ == size_ && !flush()) return false;
      size_t k = size_ - used_;
      if (k > n) k = n;
      memcpy(buf_ + used_, p, k);
      used_ += k;
      p += k;
      n -= k;
    }
    return true;
  }
  bool flush() {
    if (used_ == 0) return true;
    const bool ok = f_ && f_->write(buf_, used_) == used_;
    used_ = 0;
    return ok;
  }

private:
  fs::File* f_ = nullptr;
  uint8_t* buf_ = nullptr;
  size_t size_ = 0;
  size_t used_ = 0;
};
