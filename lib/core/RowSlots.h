// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <algorithm>
#include <cstdint>

#include "ListLayout.h"

// Which of a list's items its row sprites hold, which sprite a new item
// takes, and which item is rendered next while there is time: the list's
// decisions about its six sprites (ui/ListView: only the rows on screen
// exist, and one or two past the edge), apart from the sprites and the clock,
// so a host test can hold them (test_row_slots: each rule at its edges, and a
// fling, a drag and a turn frame by frame, counting the rows a frame has to
// render itself).
//
// The list keeps the table (an item per sprite) and the kept range itself and
// hands them in; what comes back is a decision it carries out: the sprite to
// render into, the range to keep, the item to render ahead. An item is a
// position in the list's layout (ListLayout: lead items, rows, an inline bar),
// kPitch lines tall, so an offset in lines says which items show.
//
// Portable, host-tested.
namespace rowslots {

constexpr int kSlots = 6;
constexpr int kPitch = ListLayout::kPitch;
// A second row is rendered ahead from this many lines a frame.
constexpr int kAheadTwoStep = 10;

// The item each sprite holds, as the list has them now (-1: none).
struct Held {
  int32_t item[kSlots] = {-1, -1, -1, -1, -1, -1};
  constexpr bool holds(int32_t it) const {
    for (int i = 0; i < kSlots; ++i) {
      if (item[i] == it) return true;
    }
    return false;
  }
};

// The items whose sprites aren't given to another item: first..last.
struct Keep {
  uint32_t first = 0, last = 0;
  constexpr bool has(int32_t item) const {
    return item >= static_cast<int32_t>(first) && item <= static_cast<int32_t>(last);
  }
};

// A run of items, first..last: none when last < first.
struct Items {
  int32_t first = 0, last = -1;
  constexpr bool none() const { return last < first; }
};

// A sprite holds none, or an item outside the kept range: it can be given away.
constexpr bool spare(const Held& h, Keep keep) {
  for (int i = 0; i < kSlots; ++i) {
    if (h.item[i] < 0 || !keep.has(h.item[i])) return true;
  }
  return false;
}

// The sprite for `item`: the one that holds it; else the first that can be
// given away; else sprite 0 (every one holds a kept item: the caller asked
// for more than the range it keeps). `fresh`: it doesn't hold the item yet
// (the list empties it and renders).
struct Claim {
  int slot;
  bool fresh;
};
constexpr Claim claim(const Held& h, Keep keep, int32_t item) {
  for (int i = 0; i < kSlots; ++i) {
    if (h.item[i] == item) return Claim{i, false};
  }
  for (int i = 0; i < kSlots; ++i) {
    if (h.item[i] < 0 || !keep.has(h.item[i])) return Claim{i, true};
  }
  return Claim{0, true};
}

// The items a band of `height` lines shows at `offset`: what a frame there
// keeps (a redraw in place, a scrub's band, a move once it is drawn). `items`:
// how many the list has.
constexpr Keep shown(int32_t offset, int height, uint32_t items) {
  const int32_t top = std::max<int32_t>(offset, 0);
  return Keep{static_cast<uint32_t>(top / kPitch),
              std::min<uint32_t>(items ? items - 1 : 0, static_cast<uint32_t>((top + height - 1) / kPitch))};
}

// The items content lines [firstLine, endLine) come from, cut to the list's.
constexpr Items ofLines(int32_t firstLine, int32_t endLine, int32_t items) {
  return Items{std::max<int32_t>(0, firstLine / kPitch), std::min<int32_t>(items - 1, (endLine - 1) / kPitch)};
}

// The items a move from `drawn` to `to` brings on: those of the lines it
// exposes, below the band going down the list, above it going up. They are
// rendered before the move's bus hold.
constexpr Items broughtOn(int32_t drawn, int32_t to, int height, int32_t items) {
  const bool down = to > drawn;
  return ofLines(down ? drawn + height : to, down ? to + height : drawn, items);
}

// The items with a line in the band at `drawn` (>= 0), not cut to the list's:
// what stays when the rows kept off screen are forgotten.
constexpr Items inBand(int32_t drawn, int height) { return Items{drawn / kPitch, (drawn + height - 1) / kPitch}; }

// An item to render now, into a sprite claim()ed under `keep`; item -1: none.
struct Next {
  int32_t item = -1;
  Keep keep;
};

// Between two frames of a list that moves: the item just past the edge it
// moves toward (`dir` +1 down the list, -1 up), and from a fast step (`step`
// lines a frame, kAheadTwoStep or more) the one after it, so the frame that
// brings them on only pushes. The rows on screen keep their sprites, and the
// first row ahead; with none spare, the second takes the sprite of the row
// about to leave at the other edge (its lines are on the glass: only a turn
// back would want it again, and renders it again then). None: both are held,
// or the list ends there.
constexpr Next ahead(const Held& h, int32_t drawn, int height, int32_t items, int dir, int step) {
  const int32_t first = std::max<int32_t>(0, drawn / kPitch);
  const int32_t last = std::min<int32_t>(items - 1, (drawn + height - 1) / kPitch);
  for (int k = 1; k <= (step >= kAheadTwoStep ? 2 : 1); ++k) {
    const int32_t next = dir > 0 ? last + k : first - k;
    if (next < 0 || next >= items) return Next{};
    if (h.holds(next)) continue;
    Keep keep{static_cast<uint32_t>(dir > 0 ? first : std::max<int32_t>(0, first - (k - 1))),
              static_cast<uint32_t>(dir > 0 ? std::min<int32_t>(items - 1, last + (k - 1)) : last)};
    if (k == 2 && !spare(h, keep)) {
      if (dir > 0) {
        ++keep.first;
      } else {
        --keep.last;
      }
    }
    return Next{next, keep};
  }
  return Next{};
}

// While the list rests (`items` > 0): the item just below the band, then the
// one just above it, into the sprites a list at rest has spare, so the next
// motion's first frame only pushes its row. The kept range is the rows on
// screen and one either side, whether or not there is an item to render
// (none: both are held, no sprite is spare, or the list ends there).
constexpr Next neighbour(const Held& h, int32_t drawn, int height, int32_t items) {
  const int32_t first = std::max<int32_t>(0, drawn / kPitch);
  const int32_t last = std::min<int32_t>(items - 1, (drawn + height - 1) / kPitch);
  const Keep keep{static_cast<uint32_t>(std::max<int32_t>(0, first - 1)),
                  static_cast<uint32_t>(std::min<int32_t>(items - 1, last + 1))};
  const bool room = spare(h, keep);
  if (last + 1 < items && !h.holds(last + 1) && room) return Next{last + 1, keep};
  if (first - 1 >= 0 && first - 1 < items && !h.holds(first - 1) && room) return Next{first - 1, keep};
  return Next{-1, keep};
}

// Whether a row can be rendered before the next frame, `rowUs` being what one
// takes. On the panel's scan the next hold begins about a refresh after the
// last one did (`sinceHoldUs` ago): a row must be done a millisecond before,
// and once that moment is near, or just past, the frame comes first. (A hold
// two refreshes ago says nothing: a motion's first frames take what comes.)
// On the frame clock: `untilDueMs` to the next frame's deadline.
constexpr bool roomOnScan(int64_t sinceHoldUs, int64_t frameUs, uint32_t rowUs) {
  return !(sinceHoldUs < 2 * frameUs && sinceHoldUs + rowUs + 1000 > frameUs);
}
constexpr bool roomOnClock(uint32_t untilDueMs, uint32_t rowUs) { return untilDueMs * 1000 >= rowUs + 2000; }

// What a row is taken to cost after one that took `us` (a render the decoder
// cut into counts for three rows at most: it would only scare the look-ahead
// off).
constexpr uint32_t rowTime(uint32_t rowUs, uint32_t us) { return (rowUs * 3 + std::min(us, rowUs * 3)) / 4; }

}  // namespace rowslots
