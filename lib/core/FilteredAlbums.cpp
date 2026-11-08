// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "FilteredAlbums.h"

#include <cstdlib>

namespace {
void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }
}  // namespace

FilteredAlbums::FilteredAlbums(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

FilteredAlbums::~FilteredAlbums() { drop(); }

void FilteredAlbums::drop() {
  if (ids_) freeFn_(ids_);
  ids_ = nullptr;
  cap_ = count_ = total_ = 0;
  word_ = 0;
  held_ = false;
}

FilteredAlbums::Result FilteredAlbums::recompute(const LibraryIndex& index, const moodfilter::Filter& f) {
  if (!f.active() || !index.ready()) {
    drop();
    return Result::Unfiltered;
  }
  const LibraryIndex::Span az = index.albumsAZ();
  if (!held_ || cap_ != az.count) {
    // The new block before the old one goes: without it, what was held
    // stays (an index with no albums needs none).
    uint32_t* fresh = nullptr;
    if (az.count) {
      fresh = static_cast<uint32_t*>(allocFn_(static_cast<size_t>(az.count) * sizeof(uint32_t)));
      if (!fresh) return Result::NoMemory;
    }
    drop();
    ids_ = fresh;
    cap_ = az.count;
  }
  // 4.1's album rule over each album's tracks (as tagtext::albumFits()):
  // with no words every track's is 0, so an active filter lets none through.
  const uint16_t* words = index.moodWords();
  uint32_t n = 0;
  for (uint32_t k = 0; words && k < az.count; ++k) {
    const LibraryIndex::Span t = index.tracksOfAlbum(az[k]);
    if (moodfilter::majority(moodfilter::tally(words, t.ids, t.count, f))) ids_[n++] = az[k];
  }
  count_ = n;
  total_ = az.count;
  word_ = f.pack();
  held_ = true;
  return Result::Filtered;
}
