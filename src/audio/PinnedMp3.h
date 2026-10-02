// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <AudioGeneratorMP3.h>
#include <esp_heap_caps.h>

#include "DecoderArena.h"

// ESP8266Audio's MP3 generator with libmad's state where it decodes
// fastest (docs/RESAMPLER.md section 10d). libmad's frame and synthesis
// state (25 KB) is the backend's DecoderArena: one PSRAM block allocated at
// boot in the lower 2 MB of the PSRAM window, the same for every track
// (malloc'd per track it could land above 0x3FA00000, where the same file
// decoded 1.7-3.6x realtime instead of 4.7-5.0x). The input buffer and the
// stream state (1.5 + 2.6 KB) are allocated per track in internal RAM, as
// ESP8266Audio's malloc put them (each under the framework's 4 KB
// threshold), and PSRAM only without room there.
//
// Through ESP8266Audio's constructor for separately preallocated parts: it
// then frees none of them (its destructor and stop() free only what it
// malloc'd itself), so this frees the two it allocated and lets the block
// go. The arena is held from make() until the generator is destroyed: the
// backend resets the generator before it makes the next one, so a track
// never shares the block with another.
class PinnedMp3 : public AudioGeneratorMP3 {
public:
  static constexpr size_t kFrame = 0;  // the arena's parts
  static constexpr size_t kSynth = 1;
  static constexpr size_t kArenaParts[] = {static_cast<size_t>(preAllocFrameSize()),
                                           static_cast<size_t>(preAllocSynthSize())};

  // A generator on the arena's block, or null: no block, the block is lent
  // out, or no RAM for the buffer and stream state. The caller then makes a
  // plain AudioGeneratorMP3 (its own malloc, as before).
  static PinnedMp3* make(DecoderArena& arena) {
    if (!arena.claim()) return nullptr;
    void* buf = allocSmall(preAllocBuffSize());
    void* stream = allocSmall(preAllocStreamSize());
    if (!buf || !stream) {
      heap_caps_free(buf);
      heap_caps_free(stream);
      arena.release();
      return nullptr;
    }
    return new PinnedMp3(arena, buf, stream);
  }

  ~PinnedMp3() override {
    heap_caps_free(buf_);
    heap_caps_free(stream_);
    arena_.release();
  }

private:
  PinnedMp3(DecoderArena& arena, void* buf, void* stream)
      : AudioGeneratorMP3(buf, preAllocBuffSize(), stream, preAllocStreamSize(), arena.part(kFrame),
                          preAllocFrameSize(), arena.part(kSynth), preAllocSynthSize()),
        arena_(arena),
        buf_(buf),
        stream_(stream) {}

  static void* allocSmall(size_t bytes) {
    return heap_caps_malloc_prefer(bytes, 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, MALLOC_CAP_SPIRAM);
  }

  DecoderArena& arena_;
  void* buf_;
  void* stream_;
};
