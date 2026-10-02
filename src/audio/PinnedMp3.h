// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioGeneratorMP3.h>
#include <esp_heap_caps.h>

#include <cstdlib>
#include <utility>

#include "DecoderArena.h"
#include "DecoderParts.h"
#include "FrameCursor.h"

// ESP8266Audio's MP3 generator with two additions: libmad's state where it
// decodes fastest, and the cursor (FrameCursor) that says which frame the
// sample it offers comes from.
//
// The state (docs/RESAMPLER.md section 10d): libmad's frame and synthesis
// state (25 KB) is the backend's DecoderArena when it can have it: one
// PSRAM block allocated at boot in the lower 2 MB of the PSRAM window, the
// same for every track (malloc'd per track it could land above 0x3FA00000,
// where the same file decoded 1.7-3.6x realtime instead of 4.7-5.0x).
// Without it (no block, or lent out) they are malloc'd per track, as
// ESP8266Audio does. The input buffer and the stream state (1.5 + 2.6 KB)
// are allocated per track in internal RAM, as ESP8266Audio's malloc put
// them (each under the framework's 4 KB threshold), and PSRAM only without
// room there. Through ESP8266Audio's constructor for separately
// preallocated parts: it then frees none of them, so this gives them back
// itself (DecoderParts) when it stops, as ESP8266Audio's stop() frees what
// it malloc'd: the backend keeps a stopped generator until the next MP3
// track, and a FLAC track or an idle player after it gets the 4.1 KB of
// internal RAM back. Also after a begin() that failed. One begin() per
// generator (the backend makes one per track). The block is back on stop(),
// and the backend resets the generator before it makes the next one
// anyway, so a track never shares it with another.
//
// The cursor (docs/SEEK.md section 4.1), from the generator's protected
// state: the frame's file offset is lastReadPos + (stream->this_frame -
// buff) (as ErrorToFlow() logs it), the sample's index in it (nsCount - 1)
// x 32 + (samplePtr - 1): GetOneSample() synthesizes 32 samples per
// mad_synth_frame_onens() and has moved both on before the sample is
// offered, also the one loop() offers again. Valid while 1 <= nsCount <=
// nsCountMax (begin() sets 9,999, a decoded frame 0, its first sample 1).
// Checked on the PC against a header walk of 412,324 frames (the tree's own
// libmad and a copy of Input()). A known limit, from the library: Input()'s
// resync shift (junk before a header in a refill) moves the buffer but not
// lastReadPos, so after junk the offset is off until the next refill: in a
// damaged file a landing can miss (TrimFeed lands it elsewhere, inexact),
// never a wrong exact claim. Re-check these members on any ESP8266Audio
// update (lastReadPos, buff, stream, samplePtr, nsCount, nsCountMax).
class PinnedMp3 : public AudioGeneratorMP3, public FrameCursor {
public:
  static constexpr size_t kFrame = 0;  // the arena's parts (or malloc'd in their place)
  static constexpr size_t kSynth = 1;
  static constexpr size_t kBuff = 2;  // per track, internal RAM first
  static constexpr size_t kStream = 3;
  static constexpr size_t kArenaParts[] = {static_cast<size_t>(preAllocFrameSize()),
                                           static_cast<size_t>(preAllocSynthSize())};

  // A generator on the arena's block (`pinned` true), or, without it, on
  // state of its own. Null: no RAM.
  static PinnedMp3* make(DecoderArena& arena, bool* pinned) {
    DecoderParts parts;
    *pinned = parts.claim(arena);
    if (!*pinned) {
      parts.add(std::malloc(preAllocFrameSize()), freeMalloc);
      parts.add(std::malloc(preAllocSynthSize()), freeMalloc);
    }
    parts.add(allocSmall(preAllocBuffSize()), freeCaps);
    parts.add(allocSmall(preAllocStreamSize()), freeCaps);
    if (!parts.ok()) return nullptr;  // (what there is goes back)
    return new PinnedMp3(std::move(parts));
  }

  bool begin(AudioFileSource* source, AudioOutput* output) override {
    if (!parts_.ok()) return false;  // stopped: its state is gone
    if (AudioGeneratorMP3::begin(source, output)) return true;
    // (It sets libmad's pointers only on success: nothing points at them.)
    parts_.release();
    return false;
  }

  // ESP8266Audio's stop() (libmad finished, its pointers null; also the
  // one loop() calls after three MAD_ERROR_BUFLEN), then the state given
  // back. Safe to repeat. The cursor stays valid: at() says no frame.
  bool stop() override {
    const bool closed = AudioGeneratorMP3::stop();
    parts_.release();
    return closed;
  }

  // FrameCursor: the sample offered now (or refused last).
  bool at(uint32_t* frameByte, uint32_t* sampleInFrame) const override {
    if (nsCount < 1 || nsCount > nsCountMax || samplePtr < 1 || !stream || !buff || !stream->this_frame) {
      return false;
    }
    *frameByte = static_cast<uint32_t>(lastReadPos + (stream->this_frame - buff));
    *sampleInFrame = static_cast<uint32_t>((nsCount - 1) * 32 + (samplePtr - 1));
    return true;
  }
  const uint8_t* frameBytes() const override { return stream ? stream->this_frame : nullptr; }

private:
  // (The base is built before parts_ takes them over.)
  explicit PinnedMp3(DecoderParts&& parts)
      : AudioGeneratorMP3(parts.part(kBuff), preAllocBuffSize(), parts.part(kStream), preAllocStreamSize(),
                          parts.part(kFrame), preAllocFrameSize(), parts.part(kSynth), preAllocSynthSize()),
        parts_(std::move(parts)) {}

  static void* allocSmall(size_t bytes) {
    return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM);
  }
  static void freeMalloc(void* p) { std::free(p); }
  static void freeCaps(void* p) { heap_caps_free(p); }

  DecoderParts parts_;  // given back by stop(), a failed begin(), or the destructor
};
