// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// One fixed block of memory for a decoder's working state, laid out once
// and lent to one decoder at a time (docs/RESAMPLER.md section 10d).
//
// The MP3 decoder (libmad) keeps its frame and synthesis state, 25 KB, in
// PSRAM. ESP8266Audio used to malloc it for every track, so it landed
// wherever the heap had room. On the Core2 that decided its speed: in the
// lower 2 MB of the PSRAM data window (below 0x3FA00000) the same file
// decoded at 4.7-5.0x realtime on every build and at every offset tried;
// above it, 1.7-3.6x, depending on the address modulo 16 KB and on the
// build's code layout (a cache conflict the lower half doesn't show). So
// the block is allocated once, at boot, while the lower half is free, and
// every MP3 track decodes in the same place.
//
// The layout: each part starts on a 32-byte boundary (the ESP32 cache's
// line), at the offset layout() gives. claim()/release() keep it to one
// decoder at a time: gapless playback's decode-ahead opens the next track
// only after the one before is closed, and a decoder made while the block
// is lent out gets none (it allocates its own, as before). One task (the
// decode task); the block itself is handed in (heap_caps on the ESP32), so
// the host tests can place it anywhere. Portable (test_decoder_arena).
//
// Several decoders, one block (docs/OPUS.md): each has a layout of its
// own, the constructor's being layout 0 (libmad's two parts) and
// addLayout()'s the others (the Opus decoder's state and its frame's
// PCM), and the block is sized to the largest. A claim names its layout;
// parts(), size(), offset() and part() describe the claimed one (layout 0
// while nothing is claimed), so a decoder that claims by DecoderParts
// sees its own parts and nothing changes for the MP3 path.
class DecoderArena {
public:
  static constexpr size_t kAlign = 32;
  static constexpr size_t kMaxParts = 4;
  static constexpr size_t kMaxLayouts = 2;

  // The ESP32's PSRAM data window and where its fast lower half ends.
  static constexpr uintptr_t kPsramStart = 0x3F800000u;
  static constexpr uintptr_t kPsramFastEnd = 0x3FA00000u;
  static constexpr uintptr_t kPsramEnd = 0x3FC00000u;

  enum class Where : uint8_t {
    Elsewhere,  // not PSRAM (internal RAM; or a host address)
    PsramLow,   // all of it below kPsramFastEnd
    PsramHigh,  // some of it at or above kPsramFastEnd
  };
  static Where where(const void* p, size_t bytes);
  static const char* whereName(Where w);

  static constexpr size_t alignUp(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

  // Layout 0: the parts' sizes, in order (at most kMaxParts; more are
  // ignored).
  DecoderArena(const size_t* sizes, size_t count);
  // Another decoder's layout: its index (1..), or -1 (no room for more
  // layouts, or the block is already attached). Before attach().
  int addLayout(const size_t* sizes, size_t count);
  size_t layouts() const { return layouts_; }
  // A layout's parts and bytes (every part, each aligned).
  size_t layoutParts(size_t layout) const { return layout < layouts_ ? count_[layout] : 0; }
  size_t layoutBytes(size_t layout) const { return layout < layouts_ ? bytes_[layout] : 0; }

  // The claimed layout's (layout 0 while nothing is claimed).
  size_t parts() const { return count_[current_]; }
  size_t size(size_t part) const { return part < count_[current_] ? size_[current_][part] : 0; }
  size_t offset(size_t part) const { return part < count_[current_] ? offset_[current_][part] : 0; }
  // What the block must hold: the largest layout.
  size_t bytes() const { return max_; }

  // The block (bytes() long, kAlign-aligned; misaligned or null: none).
  // Only while nothing has it claimed.
  bool attach(void* block);
  bool attached() const { return block_ != nullptr; }
  void* block() const { return block_; }
  void* part(size_t i) const;

  // True: the block is the caller's until release(), laid out as `layout`
  // says. False: there is none, no such layout, or another decoder has it
  // (counted in refused()).
  bool claim(size_t layout = 0);
  void release();
  bool inUse() const { return inUse_; }
  size_t claimed() const { return current_; }  // the claimed layout (0 when none)

  uint32_t claims() const { return claims_; }
  uint32_t refused() const { return refused_; }

private:
  void setLayout(size_t layout, const size_t* sizes, size_t count);

  size_t layouts_ = 1;
  size_t current_ = 0;
  size_t count_[kMaxLayouts] = {};
  size_t size_[kMaxLayouts][kMaxParts] = {};
  size_t offset_[kMaxLayouts][kMaxParts] = {};
  size_t bytes_[kMaxLayouts] = {};
  size_t max_ = 0;
  uint8_t* block_ = nullptr;
  bool inUse_ = false;
  uint32_t claims_ = 0;
  uint32_t refused_ = 0;
};
