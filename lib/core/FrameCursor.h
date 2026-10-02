// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// Which MP3 frame the sample a generator offers now comes from (docs/SEEK.md
// section 4.1): the frame's file offset and the sample's index in it. The
// firmware's is src/audio/PinnedMp3 (ESP8266Audio's generator, read from
// its protected state); the host tests' is a model decoder.
//
// - TrimFeed's landing phase asks it for every sample until a planned
//   start's landing frame comes (TrimFeed::armAt()).
// - SeekIndex records a frame every few frames at the end of a decoding
//   pass, when the sample refused last is the next one to go in.
class FrameCursor {
public:
  // False: no frame yet (the generator's lead, or nothing decoded).
  virtual bool at(uint32_t* frameByte, uint32_t* sampleInFrame) const = 0;
  // The current frame's first bytes, as the file has them (at least
  // min(32, its length)), for SeekIndex's hash; nullptr: not available.
  virtual const uint8_t* frameBytes() const { return nullptr; }

protected:
  ~FrameCursor() = default;
};
