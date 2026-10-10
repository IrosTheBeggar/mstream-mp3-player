// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// The index's sort of ids (LibraryIndex::buildViews(); docs/METADATA.md
// 3.9, the 2026-10-09 device run): an introsort like libstdc++'s
// std::sort, with its recursion bounded by the input's size alone.
//
// Why not std::sort: libstdc++ recurses into the right part of every
// partition and loops on the left, and picks its pivot as the median of
// first + 1, the middle and last - 1. The index's tracks arrive in the
// records' canonical order (the names' bytes, 2.6.8) and its views sort
// them by textfold's order (case and accents folded): sorted, then a tail
// of small keys (names with a lowercase or accented first letter, after
// 'Z' in bytes). That tail drags every median-of-3 pivot to the low end,
// each partition peels off one folder, and the recursion runs to the depth
// limit (2 lg n: 29 frames of 112 B at 19,410 ids, 3.2 KB of the card
// worker's 6 KB stack; the device measured 784 B left after a build).
//
// Here: the recursion takes the smaller part and the loop the larger (at
// most lg(n / 16) + 1 frames: 12 at 20k, 13 at 64k; about 128 B a frame on
// xtensa), the pivot is Tukey's ninther (medians of three triples spread
// n / 8 apart) above 128 ids, and median-of-3 below; after 2 lg n
// partitions on one path the rest is heap-sorted (n log n at worst, as
// std::sort); parts of 16 ids or fewer are left to one insertion pass at
// the end. With a comparator that is a strict total order (every view's
// ends with the ids' own order) the result is std::sort's, id for id.
//
// Portable, header-only, host-tested (test_id_sort).
namespace idsort {

// The tests' probe: the recursion's frames, the deepest seen.
struct Probe {
  int frames = 0;
  int most = 0;
};

namespace detail {

template <typename Less>
uint32_t* median3(uint32_t* a, uint32_t* b, uint32_t* c, Less& less) {
  if (less(*a, *b)) return less(*b, *c) ? b : (less(*a, *c) ? c : a);
  return less(*a, *c) ? a : (less(*b, *c) ? c : b);
}

template <typename Less>
void run(uint32_t* first, uint32_t* last, int budget, Less& less, Probe* probe) {
  if (probe && ++probe->frames > probe->most) probe->most = probe->frames;
  while (last - first > 16) {
    if (budget-- == 0) {
      std::make_heap(first, last, less);
      std::sort_heap(first, last, less);
      break;
    }
    const ptrdiff_t n = last - first;
    uint32_t* mid = first + n / 2;
    uint32_t* p;
    if (n > 128) {
      const ptrdiff_t s = n / 8;
      p = median3(median3(first + 1, first + 1 + s, first + 1 + 2 * s, less), median3(mid - s, mid, mid + s, less),
                  median3(last - 1 - 2 * s, last - 1 - s, last - 1, less), less);
    } else {
      p = median3(first + 1, mid, last - 1, less);
    }
    // The pivot at `first`; [first + 1, last) holds an id at least as big
    // (the triples' larger ones) and `first` stops the scan down: the
    // partition needs no bounds checks (libstdc++'s __unguarded_partition).
    std::iter_swap(first, p);
    uint32_t* lo = first + 1;
    uint32_t* hi = last;
    for (;;) {
      while (less(*lo, *first)) ++lo;
      --hi;
      while (less(*first, *hi)) --hi;
      if (!(lo < hi)) break;
      std::iter_swap(lo, hi);
      ++lo;
    }
    if (lo - first < last - lo) {
      run(first, lo, budget, less, probe);
      first = lo;
    } else {
      run(lo, last, budget, less, probe);
      last = lo;
    }
  }
  if (probe) --probe->frames;
}

}  // namespace detail

// Sorts [first, last) by `less` (a strict weak order; a total one for a
// result equal to std::sort's).
template <typename Less>
void sort(uint32_t* first, uint32_t* last, Less less, Probe* probe = nullptr) {
  const ptrdiff_t n = last - first;
  if (n < 2) return;
  int lg = 0;
  while ((static_cast<ptrdiff_t>(1) << (lg + 1)) <= n) ++lg;
  detail::run(first, last, 2 * lg, less, probe);
  // The parts left of 16 ids or fewer, each in its place: one pass.
  for (uint32_t* i = first + 1; i < last; ++i) {
    const uint32_t v = *i;
    uint32_t* j = i;
    while (j > first && less(v, *(j - 1))) {
      *j = *(j - 1);
      --j;
    }
    *j = v;
  }
}

}  // namespace idsort
