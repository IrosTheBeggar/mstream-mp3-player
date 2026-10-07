// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioFileSource.h>

#include <cstdint>

#include "TrackSeek.h"

// trackseek's reads (TrackSeek.h: FileReader, by absolute offset), from the
// decode task's open file: the MP3 start plans (Core2AudioBackend::planMp3)
// and the Ogg Opus reader (OpusGenerator) read through it. The file stays
// the backend's; a read moves its position.
class SourceReader : public trackseek::FileReader {
public:
  explicit SourceReader(AudioFileSource* f = nullptr) : f_(f) {}
  void attach(AudioFileSource* f) { f_ = f; }
  AudioFileSource* file() const { return f_; }

  uint32_t readAt(uint32_t offset, uint8_t* buf, uint32_t n) override {
    if (!f_ || !f_->seek(static_cast<int32_t>(offset), SEEK_SET)) return 0;
    uint32_t got = 0;
    while (got < n) {
      const uint32_t r = f_->read(buf + got, n - got);
      if (r == 0) break;
      got += r;
    }
    return got;
  }

private:
  AudioFileSource* f_;
};
