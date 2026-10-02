// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>

#include "DecoderArena.h"

// The working state of one decoder, in parts, and who gives each back: the
// DecoderArena's block (claimed: its parts are this one's first, in the
// arena's order) and parts allocated for this decoder alone, each with the
// function that frees it.
//
// release() gives all of it back, once. The MP3 generator (PinnedMp3) calls
// it when it stops, as ESP8266Audio's own stop() frees what it malloc'd:
// the backend keeps a stopped generator until the next MP3 track, and
// without this its parts (the input buffer and the stream state, 4.1 KB of
// internal RAM, and the arena's block) stayed held through a FLAC track or
// an idle player after it. Again, and the destructor, do nothing more.
// Portable (test_decoder_parts).
class DecoderParts {
public:
  using FreeFn = void (*)(void*);
  static constexpr size_t kMaxParts = 6;

  DecoderParts() = default;
  DecoderParts(DecoderParts&& other) noexcept;
  DecoderParts(const DecoderParts&) = delete;
  DecoderParts& operator=(const DecoderParts&) = delete;
  DecoderParts& operator=(DecoderParts&&) = delete;
  ~DecoderParts() { release(); }

  // The arena's block: its parts become this one's first. Only before any
  // add(). False: none, lent out, or parts already added (nothing changes).
  bool claim(DecoderArena& arena);
  // A part allocated for this decoder, freed by `free` on release(). Null
  // (the allocation failed), or no room for it: ok() is false (a part
  // there is freed at once).
  void add(void* p, FreeFn free);

  // Every part there (at least one), none refused.
  bool ok() const { return count_ > 0 && !failed_; }
  // Parts held (0 after release()).
  size_t count() const { return count_; }
  void* part(size_t i) const { return i < count_ ? part_[i] : nullptr; }
  // The arena's block is among them.
  bool pinned() const { return arena_ != nullptr; }

  // Every part freed, the block given back; what is left is empty (and ok()
  // false).
  void release();

private:
  DecoderArena* arena_ = nullptr;
  size_t count_ = 0;
  void* part_[kMaxParts] = {};
  FreeFn free_[kMaxParts] = {};  // null: the arena's
  bool failed_ = false;
};
