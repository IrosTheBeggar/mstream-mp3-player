// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// Everything a later start needs to pick up on the very sample a track
// paused at (docs/SEEK.md section 4.4): taken from the run index at a pause
// (SeekIndex::anchorAt()), saved with the resume point (QueueResume, the
// NVS blob's version 2), and checked against the file before it is used
// (trackseek::checkAnchor()); a start point carries it to the backend
// (IAudioBackend::StartAt).
//
// Samples are on the trimmed timeline (docs/GAPLESS.md section 4.6): 0 is
// the track's first kept sample, at the file's own rate.
struct ResumeAnchor {
  // Opus (3) is the FLAC model's (lib/core/OggOpus: makeAnchor(),
  // checkAnchor(); SeekIndex::anchorAt() makes one from an Opus run's
  // header, Core2AudioBackend::prepare() checks it and plans the start by
  // it: docs/OPUS.md section 9).
  enum class Kind : uint8_t { None, Mp3, Flac, Opus };
  Kind kind = Kind::None;
  bool exact = false;        // `sample` is the file's own time (else the time a TOC start showed)
  uint32_t rate = 0;         // the file's rate: the unit of `sample`
  uint64_t sample = 0;       // where it picks up (FLAC: the absolute sample; Opus: the trimmed one)
  uint32_t fileSize = 0;
  // MP3:
  uint32_t prerollByte = 0;  // the decoder is handed this frame's start
  uint32_t frameByte = 0;    // the landing frame's start
  uint32_t skip = 0;         // samples from the landing frame's first one to `sample`
  uint32_t frameHash = 0;    // resumeanchor::frameHash() of the landing frame
  // FLAC: frameHash holds STREAMINFO's total samples (low 32 bits); the byte
  // fields are 0. Opus: the exact trimmed length's low 32 bits, the same.

  bool valid() const { return kind != Kind::None; }
  bool operator==(const ResumeAnchor& o) const {
    return kind == o.kind && exact == o.exact && rate == o.rate && sample == o.sample && fileSize == o.fileSize &&
           prerollByte == o.prerollByte && frameByte == o.frameByte && skip == o.skip && frameHash == o.frameHash;
  }
  bool operator!=(const ResumeAnchor& o) const { return !(*this == o); }
};

// Portable, host-tested (test_seek_index).
namespace resumeanchor {

// The bytes a frame's hash covers: its header and side information.
constexpr uint32_t kHashBytes = 32;

// FNV-1a of an MP3 frame's first kHashBytes bytes, or of all of it when it
// is shorter (an 8 kbit/s MPEG-2 frame has 24); `avail` bytes are readable
// at `frame`. A frame header that doesn't parse: its first kHashBytes.
uint32_t frameHash(const uint8_t* frame, size_t avail);

// The anchor's sample in ms (rounded down; 0 without a rate).
uint32_t ms(const ResumeAnchor& a);

// For the logs: "MP3 frame at 2345678 + 517 samples, preroll 2343590,
// exact", "FLAC sample 3674562", "Opus sample 3674562", "none".
void describe(const ResumeAnchor& a, char* buf, size_t size);

}  // namespace resumeanchor
