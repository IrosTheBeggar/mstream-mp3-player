// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"
#include "MoodFilter.h"

// The Albums list under a mood filter (docs/MOOD-FILTERS.md 4.2, 4.6,
// 7.6): the albums of albumsAZ() that fit it, in that order, an album id
// (u32) each. An album fits by 4.1's rule, 2*pass > analysed over its
// tracksOfAlbum(), counting only its analysed tracks (moodfilter::tally(),
// majority()), as gm<hex> counts them (tagtext::fittingAlbums()); an index
// without words lets none through.
//
// One block from allocator hooks (the firmware points them at PSRAM), room
// for every album (albumCount() u32s: 6.8 KB at 1,707 albums), carved when
// a filter goes on and kept while one stays on over as many albums; an
// inactive filter (none, or a level alone) holds none, and the list shows
// albumsAZ() itself. A rebuilt index's ids mean other albums: its owner
// drops it (the library update's fence, a rebuild), as buildStamp() can't
// tell (it holds no words). Portable, host-tested (test_ui_filter). Loop
// task only.
class FilteredAlbums {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  explicit FilteredAlbums(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~FilteredAlbums();
  FilteredAlbums(const FilteredAlbums&) = delete;
  FilteredAlbums& operator=(const FilteredAlbums&) = delete;

  enum class Result : uint8_t {
    Unfiltered,  // `f` filters nothing (or the index isn't ready): nothing held
    Filtered,    // held: the albums that fit
    NoMemory,    // no block for them: what was held stays, the old filter's (4.2: it reverts)
  };
  // The albums of `index` that fit `f`. The block is carved only when none
  // is held or the album count differs (a rebuild the owner didn't drop);
  // over the same albums it is used again.
  Result recompute(const LibraryIndex& index, const moodfilter::Filter& f);
  // Lets go of the block: nothing held.
  void drop();

  // A filter's albums are held: the list shows these, not albumsAZ().
  bool held() const { return held_; }
  uint32_t count() const { return held_ ? count_ : 0; }
  uint32_t operator[](uint32_t i) const { return ids_[i]; }
  LibraryIndex::Span span() const { return {ids_, count()}; }
  // The albums they were picked from (albumsAZ()'s count then), and the
  // filter, packed (Filter::pack()): 0 when nothing is held.
  uint32_t total() const { return held_ ? total_ : 0; }
  uint16_t word() const { return held_ ? word_ : 0; }
  // The block's size (the carve): 0 when none is held.
  size_t bytes() const { return static_cast<size_t>(cap_) * sizeof(uint32_t); }

private:
  AllocFn allocFn_;
  FreeFn freeFn_;
  uint32_t* ids_ = nullptr;
  uint32_t cap_ = 0;  // the block's u32s
  uint32_t count_ = 0;
  uint32_t total_ = 0;
  uint16_t word_ = 0;
  bool held_ = false;
};
