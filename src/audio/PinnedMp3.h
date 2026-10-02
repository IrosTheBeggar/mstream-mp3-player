// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioGeneratorMP3.h>
#include <esp_heap_caps.h>

#include <cstdlib>

#include "DecoderArena.h"
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
// preallocated parts: it then frees none of them, so this frees what it
// allocated and lets the block go. The backend resets the generator before
// it makes the next one, so a track never shares the block with another.
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
  static constexpr size_t kFrame = 0;  // the arena's parts
  static constexpr size_t kSynth = 1;
  static constexpr size_t kArenaParts[] = {static_cast<size_t>(preAllocFrameSize()),
                                           static_cast<size_t>(preAllocSynthSize())};

  // A generator on the arena's block (`pinned` true), or, without it, on
  // state of its own. Null: no RAM.
  static PinnedMp3* make(DecoderArena& arena, bool* pinned) {
    *pinned = arena.claim();
    void* frame = *pinned ? arena.part(kFrame) : std::malloc(preAllocFrameSize());
    void* synth = *pinned ? arena.part(kSynth) : std::malloc(preAllocSynthSize());
    void* buf = allocSmall(preAllocBuffSize());
    void* stream = allocSmall(preAllocStreamSize());
    if (!buf || !stream || !frame || !synth) {
      heap_caps_free(buf);
      heap_caps_free(stream);
      if (*pinned) {
        arena.release();
      } else {
        std::free(frame);
        std::free(synth);
      }
      return nullptr;
    }
    return new PinnedMp3(*pinned ? &arena : nullptr, buf, stream, frame, synth);
  }

  ~PinnedMp3() override {
    heap_caps_free(buf_);
    heap_caps_free(stream_);
    if (arena_) {
      arena_->release();
    } else {
      std::free(frame_);
      std::free(synth_);
    }
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
  PinnedMp3(DecoderArena* arena, void* buf, void* stream, void* frame, void* synth)
      : AudioGeneratorMP3(buf, preAllocBuffSize(), stream, preAllocStreamSize(), frame, preAllocFrameSize(), synth,
                          preAllocSynthSize()),
        arena_(arena),
        buf_(buf),
        stream_(stream),
        frame_(frame),
        synth_(synth) {}

  static void* allocSmall(size_t bytes) {
    return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM);
  }

  DecoderArena* arena_;  // null: frame_ and synth_ are this generator's own
  void* buf_;
  void* stream_;
  void* frame_;
  void* synth_;
};
