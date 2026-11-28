// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// A list's sprites and its hardware scroll's map, with no sprites: for the
// host tests that take a list through its frames (test_row_slots: the
// sprites' rules; test_list_frames: the frames' own, with a clock).
#pragma once
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "ListLayout.h"
#include "RowSlots.h"
#include "VScrollMap.h"

namespace listrig {

using namespace rowslots;

constexpr int kBand = 168;  // the list's band: four rows

// The list's use of the rules (ui/ListView.cpp: slotFor(), renderedSlot(),
// render(), prepare(), push(), prerender(), renderAhead(), renderNeighbours(),
// forgetHidden()), with no sprites: the same calls in the same order, over the
// real hardware scroll's map. It counts the rows rendered, in a frame's way
// and between frames, and checks what must always hold. (When a frame is
// drawn, and when a row ahead, is the caller's: lib/core's ListFrames says.)
struct List {
  Held held;
  Keep keep;
  VScrollMap map;
  int32_t items;
  int height;
  int32_t drawn = -1;
  int dir = 0, step = 0;
  int frames = 0;
  int inFrames = 0;   // rows a frame rendered itself
  int lastFrame = 0;  // ... the last frame
  int32_t lastFrameItem = -1;  // ... and the last of them (-1: it rendered none)
  int32_t lastItem = -1;       // the last row rendered at all
  int betweenFrames = 0;  // rows rendered ahead, or beside a resting list
  int unkept = 0;         // an item took a sprite whose row was kept (claim()'s last resort)
  bool grabbed = false;   // a finger caught it moving, and hasn't moved it yet

  explicit List(int32_t n, int h = kBand) : items(n), height(h) { map.configure(72, h, 2 * kPitch); }

  bool rendered(int32_t item) {
    const Claim c = claim(held, keep, item);
    if (!c.fresh) return false;
    if (held.item[c.slot] >= 0 && keep.has(held.item[c.slot])) ++unkept;
    held.item[c.slot] = item;
    lastItem = item;
    return true;
  }
  // push(): every item a span's lines come from.
  int pushed(const VScrollMap::Span& s) {
    int n = 0;
    for (int32_t c = s.contentY; c < s.contentY + s.h;) {
      const int32_t item = c >= 0 ? c / kPitch : -1;
      if (item >= 0 && item < items) n += rendered(item);
      c = std::min<int32_t>(s.contentY + s.h, c >= 0 ? (item + 1) * kPitch : 0);
    }
    return n;
  }
  // A frame at `off`, a step from where the list is drawn (two rows at the
  // most: ListLayout::stepToward()). On the panel's scan its rows are rendered
  // first (prerender()); then render(), and the hardware scroll's plan: a
  // redraw in place, or a move (prepare(), then the pushes).
  int frame(int32_t off, bool onScan = true) {
    int n = 0;
    if (onScan && map.valid() && drawn >= 0 && off != drawn) {
      const Items on = broughtOn(drawn, off, height, items);
      keep = shown(off, height, static_cast<uint32_t>(items));
      for (int32_t i = on.first; i <= on.last; ++i) n += rendered(i);
    }
    keep = shown(off, height, static_cast<uint32_t>(items));
    VScrollMap::Span spans[VScrollMap::kMaxSpans];
    const uint16_t was = map.vsp();
    const int count = map.plan(off, spans);
    if (map.vsp() != was && count > 0) {
      const Items on = ofLines(spans[0].contentY, spans[count - 1].contentY + spans[count - 1].h, items);
      if (!on.none()) {
        keep = Keep{static_cast<uint32_t>(on.first), static_cast<uint32_t>(on.last)};
        for (int32_t i = on.first; i <= on.last; ++i) n += rendered(i);
      }
    }
    for (int i = 0; i < count; ++i) n += pushed(spans[i]);
    if (drawn >= 0 && off != drawn) {
      dir = off > drawn ? 1 : -1;
      step = std::abs(off - drawn);
    }
    drawn = off;
    ++frames;
    inFrames += n;
    lastFrame = n;
    lastFrameItem = n ? lastItem : -1;
    return n;
  }
  // A jump (the A-Z rail, a scroll to a row, a page's first frame): drawn at
  // once where it goes (drawFrame()), nothing rendered ahead of the plan.
  int jump(int32_t off) { return frame(off, false); }
  bool renderAhead() {
    if (!grabbed && dir == 0) return false;
    const Next next = grabbed ? unheld(held, drawn, height, items) : ahead(held, drawn, height, items, dir, step);
    if (next.item < 0) return false;
    keep = next.keep;
    rendered(next.item);
    ++betweenFrames;
    return true;
  }
  bool renderNeighbours() {
    const Next next = neighbour(held, drawn, height, items);
    keep = next.keep;
    if (next.item < 0) return false;
    rendered(next.item);
    ++betweenFrames;
    return true;
  }
  void rest() {
    while (renderNeighbours()) {
    }
  }
  void forgetHidden() {
    const Items band = inBand(drawn, height);
    for (int32_t& it : held.item) {
      if (it < band.first || it > band.last) it = -1;
    }
  }
  // A finger takes hold of it while it moves (which way next is the finger's),
  // and then moves it, or lifts.
  void grab() { grabbed = true; }
  void letGo() { grabbed = false; }
  // No item is in two sprites, and none is past the list.
  void check() const {
    for (int i = 0; i < kSlots; ++i) {
      TEST_ASSERT_TRUE(held.item[i] >= -1 && held.item[i] < items);
      for (int k = i + 1; k < kSlots; ++k) TEST_ASSERT_TRUE(held.item[i] < 0 || held.item[i] != held.item[k]);
    }
  }
};

}  // namespace listrig
