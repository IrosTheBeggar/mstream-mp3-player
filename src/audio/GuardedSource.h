// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioFileSource.h>

#include <cstring>

// An MP3 file with 8 zero bytes after its end (docs/GAPLESS.md section
// 4.3). libmad decodes a frame only with MAD_BUFFER_GUARD (8) bytes after
// it (mad_header_decode(): MAD_ERROR_BUFLEN otherwise), and at the end of
// a file ESP8266Audio's AudioGeneratorMP3::Input() reads nothing more and
// throws the rest away: a file that ends right after its last frame (no
// ID3v1 or APE tag behind it) loses that frame, up to 1,152 samples of
// real audio, and gapless trimming's padding arithmetic counts it. The
// usual libmad practice, once per open (a seek arms it again). Everything
// else passes through: getPos()/getSize() are the file's.
class GuardedSource : public AudioFileSource {
public:
  static constexpr uint32_t kGuard = 8;

  void attach(AudioFileSource* file) {
    file_ = file;
    left_ = kGuard;
  }
  uint32_t read(void* data, uint32_t len) override {
    const uint32_t n = file_->read(data, len);
    if (n > 0 || len == 0 || left_ == 0) return n;
    const uint32_t g = len < left_ ? len : left_;  // the end: the guard
    std::memset(data, 0, g);
    left_ -= g;
    return g;
  }
  uint32_t readNonBlock(void* data, uint32_t len) override { return read(data, len); }
  bool seek(int32_t pos, int dir) override {
    left_ = kGuard;
    return file_->seek(pos, dir);
  }
  bool close() override { return file_->close(); }
  bool isOpen() override { return file_->isOpen(); }
  uint32_t getSize() override { return file_->getSize(); }
  uint32_t getPos() override { return file_->getPos(); }
  bool loop() override { return file_->loop(); }

private:
  AudioFileSource* file_ = nullptr;
  uint32_t left_ = kGuard;
};
