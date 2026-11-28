// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the list's row sprites (lib/core/RowSlots.h): which
// sprite an item takes and what is given away for it; the items a band shows,
// a move brings on and a forget keeps; the row rendered ahead of a moving
// list and beside a resting one, with the range each keeps; the time a row
// has before a frame; the row a caught list gets back. Then the list's own use
// of them, frame by frame over the real scroll and the hardware scroll's map: a
// fling, a slow move, a swipe from rest, a catch and a turn, counting the rows a
// frame has to render itself (the hitch a decoder pass stretches), and a random
// session's invariants.
// Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>

#include "KineticScroll.h"
#include "ListLayout.h"
#include "RowSlots.h"
#include "VScrollMap.h"

#include "../support/ListRig.h"

using namespace rowslots;
using listrig::kBand;
using listrig::List;

void setUp() {}
void tearDown() {}

namespace {

Held heldOf(int32_t a, int32_t b, int32_t c, int32_t d, int32_t e, int32_t f) {
  Held h;
  const int32_t v[kSlots] = {a, b, c, d, e, f};
  for (int i = 0; i < kSlots; ++i) h.item[i] = v[i];
  return h;
}

void assertKeep(uint32_t first, uint32_t last, Keep k) {
  TEST_ASSERT_EQUAL_UINT32(first, k.first);
  TEST_ASSERT_EQUAL_UINT32(last, k.last);
}

void assertItems(int32_t first, int32_t last, Items r) {
  TEST_ASSERT_FALSE(r.none());
  TEST_ASSERT_EQUAL_INT32(first, r.first);
  TEST_ASSERT_EQUAL_INT32(last, r.last);
}

void assertNext(int32_t item, uint32_t first, uint32_t last, Next n) {
  TEST_ASSERT_EQUAL_INT32(item, n.item);
  assertKeep(first, last, n.keep);
}

}  // namespace

// The sizes the list is built on: six sprites of a row's pitch.
void test_the_figures() {
  TEST_ASSERT_EQUAL_INT(6, kSlots);
  TEST_ASSERT_EQUAL_INT(42, kPitch);
  TEST_ASSERT_EQUAL_INT(10, kAheadTwoStep);
  const Held none;
  for (int32_t it : none.item) TEST_ASSERT_EQUAL_INT32(-1, it);
  TEST_ASSERT_FALSE(none.holds(0));
  TEST_ASSERT_TRUE(none.holds(-1));  // (an empty sprite "holds" none: the list never asks for -1)
}

// An item's own sprite first; else the first that holds none, or an item
// outside the kept range; else sprite 0.
void test_which_sprite_an_item_takes() {
  const Keep keep{100, 103};
  // Its own, wherever it is, kept or not.
  Claim c = claim(heldOf(100, 101, 102, 103, 104, 99), keep, 104);
  TEST_ASSERT_EQUAL_INT(4, c.slot);
  TEST_ASSERT_FALSE(c.fresh);
  // None holds it: the first empty one.
  c = claim(heldOf(100, 101, -1, 103, -1, 99), keep, 104);
  TEST_ASSERT_EQUAL_INT(2, c.slot);
  TEST_ASSERT_TRUE(c.fresh);
  // No empty one: the first whose item is outside the range, below it or above.
  c = claim(heldOf(100, 101, 102, 103, 99, 104), keep, 105);
  TEST_ASSERT_EQUAL_INT(4, c.slot);
  TEST_ASSERT_TRUE(c.fresh);
  c = claim(heldOf(100, 101, 102, 103, 104, 99), keep, 105);
  TEST_ASSERT_EQUAL_INT(4, c.slot);
  // The range's ends are kept.
  c = claim(heldOf(100, 103, 101, 102, 7, 8), Keep{100, 103}, 104);
  TEST_ASSERT_EQUAL_INT(4, c.slot);
  // Every sprite holds a kept item (more asked than is kept): sprite 0.
  c = claim(heldOf(100, 101, 102, 103, 104, 105), Keep{100, 105}, 106);
  TEST_ASSERT_EQUAL_INT(0, c.slot);
  TEST_ASSERT_TRUE(c.fresh);
  // What can be given away.
  TEST_ASSERT_TRUE(spare(Held{}, keep));
  TEST_ASSERT_TRUE(spare(heldOf(100, 101, 102, 103, 104, 105), keep));
  TEST_ASSERT_TRUE(spare(heldOf(100, 101, 102, 103, -1, 100), keep));
  TEST_ASSERT_FALSE(spare(heldOf(100, 101, 102, 103, 104, 105), Keep{100, 105}));
  TEST_ASSERT_FALSE(spare(heldOf(100, 100, 100, 100, 100, 100), Keep{100, 100}));
}

// The items a band shows: four at a row's boundary, five across one.
void test_the_items_a_band_shows() {
  assertKeep(0, 3, shown(0, kBand, 700));
  assertKeep(0, 4, shown(1, kBand, 700));
  assertKeep(0, 4, shown(41, kBand, 700));
  assertKeep(1, 4, shown(42, kBand, 700));
  assertKeep(100, 103, shown(4200, kBand, 700));
  assertKeep(116, 120, shown(4902, kBand, 700));
  // Cut to the list's, at its end and for a list shorter than the band.
  assertKeep(696, 699, shown(700 * kPitch - kBand, kBand, 700));
  assertKeep(0, 2, shown(0, kBand, 3));
  assertKeep(0, 0, shown(0, kBand, 1));
  assertKeep(0, 0, shown(0, kBand, 0));  // (no items: nothing is asked for)
  // An offset below 0 is the top's.
  assertKeep(0, 3, shown(-5, kBand, 700));
  // The shorter bands: the Queue's selection mode (126 lines), the Library's
  // status line (148).
  assertKeep(0, 2, shown(0, 126, 700));
  assertKeep(0, 3, shown(1, 126, 700));
  assertKeep(0, 3, shown(0, 148, 700));
  assertKeep(0, 4, shown(21, 148, 700));
  // The band at an offset, not cut: what a forget keeps.
  assertItems(0, 3, inBand(0, kBand));
  assertItems(0, 4, inBand(30, kBand));
  assertItems(116, 120, inBand(4902, kBand));
}

// The items a move brings on: those of the lines it exposes.
void test_the_items_a_move_brings_on() {
  // Lines to items.
  assertItems(4, 4, ofLines(168, 169, 700));
  assertItems(4, 5, ofLines(168, 252, 700));
  assertItems(9, 9, ofLines(410, 420, 700));
  assertItems(699, 699, ofLines(700 * kPitch - 10, 700 * kPitch, 700));
  TEST_ASSERT_TRUE(ofLines(700 * kPitch, 700 * kPitch + 42, 700).none());  // past the list's end
  TEST_ASSERT_TRUE(ofLines(168, 252, 4).none());                           // a list of four rows has none there
  // Down the list: below the band. A line, a row, the hardware scroll's two rows.
  assertItems(4, 4, broughtOn(0, 1, kBand, 700));
  assertItems(4, 4, broughtOn(0, 42, kBand, 700));
  assertItems(4, 5, broughtOn(0, 84, kBand, 700));
  assertItems(4, 6, broughtOn(41, 125, kBand, 700));  // three at the most: a row's end and two more
  // Up it: above the band.
  assertItems(9, 9, broughtOn(420, 410, kBand, 700));
  assertItems(8, 9, broughtOn(420, 336, kBand, 700));
  assertItems(0, 0, broughtOn(42, 0, kBand, 700));
  // A list one row longer than the band: its last row.
  assertItems(4, 4, broughtOn(0, 42, kBand, 5));
  // The shorter band.
  assertItems(3, 3, broughtOn(0, 10, 126, 700));
}

// Between two frames of a moving list: the row past the edge it moves toward,
// from a fast step the one after it.
void test_the_row_rendered_ahead() {
  // At a row's boundary, row 100 on top: four rows show, two sprites spare.
  const int32_t at = 4200;
  assertNext(104, 100, 103, ahead(heldOf(100, 101, 102, 103, -1, -1), at, kBand, 700, 1, 5));
  assertNext(99, 100, 103, ahead(heldOf(100, 101, 102, 103, -1, -1), at, kBand, 700, -1, 5));
  // It is there: at a slow step, nothing more.
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(100, 101, 102, 103, 104, -1), at, kBand, 700, 1, 5).item);
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(100, 101, 102, 103, 104, -1), at, kBand, 700, 1, kAheadTwoStep - 1).item);
  // From a fast step, the row after it; the first row ahead is kept too.
  assertNext(105, 100, 104, ahead(heldOf(100, 101, 102, 103, 104, -1), at, kBand, 700, 1, kAheadTwoStep));
  assertNext(98, 99, 103, ahead(heldOf(100, 101, 102, 103, 99, -1), at, kBand, 700, -1, 28));
  // ... into whatever is spare: the row behind goes before any on screen.
  Next n = ahead(heldOf(100, 101, 102, 103, 104, 99), at, kBand, 700, 1, 28);
  assertNext(105, 100, 104, n);
  TEST_ASSERT_EQUAL_INT(5, claim(heldOf(100, 101, 102, 103, 104, 99), n.keep, n.item).slot);

  // Across a row's boundary five rows show, and the first row ahead takes the
  // sixth sprite. The second then takes the sprite of the row about to leave at
  // the other edge: it is no longer kept.
  const int32_t mid = 4230;
  n = ahead(heldOf(100, 101, 102, 103, 104, 105), mid, kBand, 700, 1, 28);
  assertNext(106, 101, 105, n);
  TEST_ASSERT_EQUAL_INT(0, claim(heldOf(100, 101, 102, 103, 104, 105), n.keep, n.item).slot);  // row 100's
  n = ahead(heldOf(100, 101, 102, 103, 104, 99), mid, kBand, 700, -1, 28);
  assertNext(98, 99, 103, n);
  TEST_ASSERT_EQUAL_INT(4, claim(heldOf(100, 101, 102, 103, 104, 99), n.keep, n.item).slot);  // row 104's
  // At a slow step the rows on screen all keep theirs.
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(100, 101, 102, 103, 104, 105), mid, kBand, 700, 1, 5).item);

  // The list's ends: none past them, at any step.
  const int32_t end = 700 * kPitch - kBand;
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(696, 697, 698, 699, -1, -1), end, kBand, 700, 1, 28).item);
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(0, 1, 2, 3, -1, -1), 0, kBand, 700, -1, 28).item);
  // One row from the end: that row, and no second.
  assertNext(699, 695, 698, ahead(heldOf(695, 696, 697, 698, -1, -1), end - kPitch, kBand, 700, 1, 28));
  TEST_ASSERT_EQUAL_INT32(-1, ahead(heldOf(695, 696, 697, 698, 699, -1), end - kPitch, kBand, 700, 1, 28).item);
}

// While the list rests: the row below the band, then the row above it, into
// what is spare. The rows on screen and one either side are kept either way.
void test_the_rows_rendered_beside_a_resting_list() {
  const int32_t at = 4200;
  assertNext(104, 99, 104, neighbour(heldOf(100, 101, 102, 103, -1, -1), at, kBand, 700));
  assertNext(99, 99, 104, neighbour(heldOf(100, 101, 102, 103, 104, -1), at, kBand, 700));
  assertNext(-1, 99, 104, neighbour(heldOf(100, 101, 102, 103, 104, 99), at, kBand, 700));
  // A row kept from somewhere else is spare.
  assertNext(104, 99, 104, neighbour(heldOf(100, 101, 102, 103, 7, 250), at, kBand, 700));
  // Across a row's boundary (a list held there by a finger's lift on a shorter
  // band, or one row of a shorter band): five show, one sprite is spare, so
  // the row below alone.
  const int32_t mid = 4230;
  assertNext(105, 99, 105, neighbour(heldOf(100, 101, 102, 103, 104, -1), mid, kBand, 700));
  assertNext(-1, 99, 105, neighbour(heldOf(100, 101, 102, 103, 104, 105), mid, kBand, 700));
  // The list's ends: the one side there is.
  assertNext(4, 0, 4, neighbour(heldOf(0, 1, 2, 3, -1, -1), 0, kBand, 700));
  assertNext(-1, 0, 4, neighbour(heldOf(0, 1, 2, 3, 4, -1), 0, kBand, 700));
  const int32_t end = 700 * kPitch - kBand;
  assertNext(695, 695, 699, neighbour(heldOf(696, 697, 698, 699, -1, -1), end, kBand, 700));
  // A list that fits the band has none.
  assertNext(-1, 0, 2, neighbour(heldOf(0, 1, 2, -1, -1, -1), 0, kBand, 3));
}

// A list a finger caught while it moved: the row on screen whose sprite the
// fling's second row ahead took gets it back, from the furthest row ahead.
void test_the_row_a_caught_list_gets_back() {
  // Every row on screen has its sprite: nothing to do.
  TEST_ASSERT_EQUAL_INT32(-1, unheld(heldOf(100, 101, 102, 103, 104, 105), 4230, kBand, 700).item);
  TEST_ASSERT_EQUAL_INT32(-1, unheld(heldOf(100, 101, 102, 103, -1, -1), 4200, kBand, 700).item);
  // After a fast fling down the list, across a row: rows 100-104 show, row
  // 100's sprite went to row 106. It comes back; the rows on screen and one
  // either side are kept, so row 106's sprite is the one it takes, and row
  // 105 stays for a flick on the same way.
  Held h = heldOf(106, 101, 102, 103, 104, 105);
  Next n = unheld(h, 4230, kBand, 700);
  assertNext(100, 99, 105, n);
  TEST_ASSERT_EQUAL_INT(0, claim(h, n.keep, n.item).slot);
  // After one up it: row 104's went to row 98.
  h = heldOf(100, 101, 102, 103, 98, 99);
  n = unheld(h, 4230, kBand, 700);
  assertNext(104, 99, 105, n);
  TEST_ASSERT_EQUAL_INT(4, claim(h, n.keep, n.item).slot);
  // Nothing spare beyond the rows beside the band: the rows on screen alone are
  // kept, and one of those beside it gives its sprite.
  h = heldOf(99, 101, 102, 103, 104, 105);
  n = unheld(h, 4230, kBand, 700);
  assertNext(100, 100, 104, n);
  TEST_ASSERT_EQUAL_INT(0, claim(h, n.keep, n.item).slot);
  // More than one gone: the first, top down; the next call has the next.
  h = heldOf(-1, -1, 102, 103, 104, 105);
  n = unheld(h, 4230, kBand, 700);
  assertNext(100, 99, 105, n);
  h.item[claim(h, n.keep, n.item).slot] = n.item;
  assertNext(101, 99, 105, unheld(h, 4230, kBand, 700));
  // At the list's ends the range stops with it.
  assertNext(0, 0, 5, unheld(heldOf(6, 1, 2, 3, 4, 5), 30, kBand, 700));
  const int32_t end = 700 * kPitch - kBand;
  assertNext(699, 695, 699, unheld(heldOf(696, 697, 698, 694, 693, -1), end, kBand, 700));
}

// Whether a row fits before the next frame, and what the last one says a row costs.
void test_the_time_a_row_has() {
  constexpr int64_t kFrame = 14000;  // a refresh of the Core2's panel
  constexpr uint32_t kRow = 3400;
  // On the scan: done a millisecond before the next hold, a refresh after the last.
  TEST_ASSERT_TRUE(roomOnScan(0, kFrame, kRow));
  TEST_ASSERT_TRUE(roomOnScan(9600, kFrame, kRow));
  TEST_ASSERT_FALSE(roomOnScan(9601, kFrame, kRow));
  TEST_ASSERT_FALSE(roomOnScan(13999, kFrame, kRow));
  TEST_ASSERT_FALSE(roomOnScan(2 * kFrame - 1, kFrame, kRow));  // the frame is late: it comes first
  TEST_ASSERT_TRUE(roomOnScan(2 * kFrame, kFrame, kRow));       // two refreshes: that hold says nothing now
  // On the clock: 2 ms to spare before the deadline.
  TEST_ASSERT_FALSE(roomOnClock(5, kRow));
  TEST_ASSERT_TRUE(roomOnClock(6, kRow));
  TEST_ASSERT_TRUE(roomOnClock(16, kRow));
  // A row's cost: three parts what it was, one the last; one the decoder cut
  // into counts as three rows at most.
  TEST_ASSERT_EQUAL_UINT32(4000, rowTime(4000, 4000));
  TEST_ASSERT_EQUAL_UINT32(3500, rowTime(4000, 2000));
  TEST_ASSERT_EQUAL_UINT32(3000, rowTime(4000, 0));
  TEST_ASSERT_EQUAL_UINT32(6000, rowTime(4000, 12000));
  TEST_ASSERT_EQUAL_UINT32(6000, rowTime(4000, 40000));
}

namespace {


constexpr uint32_t kFrameMs = 14;  // a refresh
constexpr int kStep = 2 * kPitch;  // the most a frame moves (ListView::kMaxStep)

int32_t roundOf(float px) { return static_cast<int32_t>(std::lround(px)); }

// A fling from where the list stands, a frame a refresh and a row ahead
// between two, to its rest.
void fling(List& l, KineticScroll& k, uint32_t& ms, float pxPerS) {
  k.fling(ms, pxPerS);
  while (k.moving()) {
    ms += kFrameMs;
    k.update(ms);
    const int32_t off = ListLayout::stepToward(l.drawn, roundOf(k.offset()), kStep);
    if (off != l.drawn) l.frame(off);
    l.renderAhead();
    l.check();
  }
}

// A list of 700 rows drawn at row 100, at rest.
struct Rig {
  List list{700};
  KineticScroll scroll;
  uint32_t ms = 1000;
  Rig() {
    scroll.setExtent(700.0f * kPitch, static_cast<float>(kBand));
    scroll.jumpTo(100.0f * kPitch);
    list.jump(100 * kPitch);
  }
};

}  // namespace

// A page's first frame renders the band, and the list at rest the row either
// side: the next swipe's first frame then only pushes.
void test_a_first_frame_and_a_swipe_from_rest() {
  Rig r;
  TEST_ASSERT_EQUAL_INT(4, r.list.lastFrame);
  // A swipe before the rows beside it are in renders its row itself...
  List cold = r.list;
  TEST_ASSERT_EQUAL_INT(1, cold.frame(4220));
  TEST_ASSERT_EQUAL_INT32(104, cold.lastItem);
  // ... one after, neither way.
  r.list.rest();
  TEST_ASSERT_EQUAL_INT(2, r.list.betweenFrames);
  TEST_ASSERT_TRUE(r.list.held.holds(104) && r.list.held.holds(99));
  List down = r.list, up = r.list;
  TEST_ASSERT_EQUAL_INT(0, down.frame(4220));
  TEST_ASSERT_EQUAL_INT(0, up.frame(4180));
  // The same redrawn in place (an overlay went) renders nothing again.
  r.list.map.invalidate();
  TEST_ASSERT_EQUAL_INT(0, r.list.frame(4200));
  // A cover arrived: the rows kept off screen are forgotten (they hold the
  // placeholder), the rows on screen stay.
  r.list.forgetHidden();
  TEST_ASSERT_FALSE(r.list.held.holds(104) || r.list.held.holds(99));
  for (int32_t i = 100; i <= 103; ++i) TEST_ASSERT_TRUE(r.list.held.holds(i));
  r.list.check();
  TEST_ASSERT_EQUAL_INT(0, r.list.unkept);
}

// A fling at the cap, 28 lines a frame: every row that comes on was rendered
// between two frames, none in a frame's way. And a slow one.
void test_a_fling_renders_no_row_in_a_frame() {
  for (float v : {2000.0f, -2000.0f, 300.0f, -90.0f}) {
    Rig r;
    r.list.rest();
    const int ahead0 = r.list.betweenFrames, frames0 = r.list.frames;
    fling(r.list, r.scroll, r.ms, v);
    TEST_ASSERT_TRUE(r.list.frames - frames0 > 20);
    TEST_ASSERT_EQUAL_INT(0, r.list.inFrames - 4);  // (the first frame's four)
    // A row for each that came on, less the one beside the band at the start.
    const int rows = std::abs(r.list.drawn - 100 * kPitch) / kPitch;
    TEST_ASSERT_TRUE(rows >= 1);
    TEST_ASSERT_EQUAL_INT(rows, r.list.betweenFrames - ahead0);
    TEST_ASSERT_EQUAL_INT(0, r.list.drawn % kPitch);  // it rests on a row
    TEST_ASSERT_EQUAL_INT(0, r.list.unkept);
  }
  // With no time between frames for a row (the decoder took it), the frames
  // render their rows themselves: one, at most two.
  Rig r;
  r.list.rest();
  r.scroll.fling(r.ms, 2000.0f);
  int worst = 0;
  while (r.scroll.moving()) {
    r.ms += kFrameMs;
    r.scroll.update(r.ms);
    const int32_t off = ListLayout::stepToward(r.list.drawn, roundOf(r.scroll.offset()), kStep);
    if (off != r.list.drawn) worst = std::max(worst, r.list.frame(off));
  }
  TEST_ASSERT_TRUE(r.list.inFrames - 4 >= 30);
  TEST_ASSERT_TRUE(worst >= 1 && worst <= 2);
}

// A fling down the list at the cap, caught 0.3 s in across a row: the top row's
// sprite went to the second row ahead (rowslots::ahead(), "only a turn back
// would want it again").
static Rig caught() {
  Rig r;
  r.list.rest();
  r.scroll.fling(r.ms, 2000.0f);
  for (int i = 0; i < 21; ++i) {
    r.ms += kFrameMs;
    r.scroll.update(r.ms);
    const int32_t off = ListLayout::stepToward(r.list.drawn, roundOf(r.scroll.offset()), kStep);
    if (off != r.list.drawn) r.list.frame(off);
    r.list.renderAhead();
  }
  // The finger lands (ListView's Down: the list stops, and is the finger's).
  r.scroll.press(r.ms, 100);
  r.list.grab();
  const int32_t top = r.list.drawn / kPitch;
  TEST_ASSERT_EQUAL_INT(4, r.list.inFrames);
  TEST_ASSERT_TRUE(r.list.drawn % kPitch != 0);  // across a row: five rows show
  TEST_ASSERT_FALSE(r.list.held.holds(top));     // ... and the top one has no sprite
  TEST_ASSERT_TRUE(r.list.held.holds(top + 5) && r.list.held.holds(top + 6));
  return r;
}

// The finger pulls the list `px` a frame for `frames` frames (down the screen
// for more than 0: back up the list). Returns the rows its first frame rendered.
static int pull(Rig& r, int px, int frames) {
  r.list.letGo();
  int first = -1;
  for (int i = 1; i <= frames; ++i) {
    r.ms += kFrameMs;
    r.scroll.drag(r.ms, 100 + px * i);
    const int32_t off = ListLayout::stepToward(r.list.drawn, roundOf(r.scroll.offset()), kStep);
    const int n = off != r.list.drawn ? r.list.frame(off) : 0;
    if (i == 1) first = n;
    r.list.renderAhead();
    r.list.check();
  }
  return first;
}

// A turn: the caught list stands still under the finger for a pass, gets its
// top row's sprite back, and the pull back only pushes: no row is rendered in a
// frame, the turn's first least of all. (Before, that frame rendered the row.)
void test_a_turn_only_pushes() {
  Rig r = caught();
  const int32_t top = r.list.drawn / kPitch;
  // While it stands: the top row, into the second row ahead's sprite; the
  // first row ahead stays. Then nothing more, however long the finger rests.
  const int before = r.list.betweenFrames;
  TEST_ASSERT_TRUE(r.list.renderAhead());
  TEST_ASSERT_EQUAL_INT32(top, r.list.lastItem);
  TEST_ASSERT_TRUE(r.list.held.holds(top + 5));
  TEST_ASSERT_FALSE(r.list.held.holds(top + 6));
  for (int i = 0; i < 50; ++i) TEST_ASSERT_FALSE(r.list.renderAhead());
  TEST_ASSERT_EQUAL_INT(1, r.list.betweenFrames - before);
  // It pulls back up the list, 3 px a frame, then faster.
  TEST_ASSERT_EQUAL_INT(0, pull(r, 3, 40));
  TEST_ASSERT_EQUAL_INT(4, r.list.inFrames);
  TEST_ASSERT_EQUAL_INT(0, r.list.unkept);
  Rig fast = caught();
  fast.list.renderAhead();
  TEST_ASSERT_EQUAL_INT(0, pull(fast, 20, 8));
  TEST_ASSERT_EQUAL_INT(4, fast.list.inFrames);
}

// The catch followed by a flick on the same way (a long list scrolled flick by
// flick): the row ahead kept its sprite through the catch, so that frame only
// pushes too.
void test_a_flick_on_after_a_catch_only_pushes() {
  Rig r = caught();
  r.list.renderAhead();
  TEST_ASSERT_EQUAL_INT(0, pull(r, -20, 8));
  TEST_ASSERT_EQUAL_INT(4, r.list.inFrames);
  TEST_ASSERT_EQUAL_INT(0, r.list.unkept);
}

// A finger that turns the list before it has stood a pass under it (no time to
// give the row back) still renders that row in the turn's first frame, as
// before: once.
void test_a_turn_at_once_renders_its_row() {
  Rig r = caught();
  const int32_t top = r.list.drawn / kPitch;
  TEST_ASSERT_EQUAL_INT(1, pull(r, 3, 1));
  TEST_ASSERT_EQUAL_INT32(top, r.list.lastFrameItem);
  pull(r, 3, 39);
  TEST_ASSERT_EQUAL_INT(5, r.list.inFrames);
}

// A jump (the A-Z rail, a scroll to a row): the band in place, its rows
// rendered unless they are held already.
void test_a_jump_renders_its_band() {
  Rig r;
  r.list.rest();
  // Far: none of its rows is held.
  TEST_ASSERT_EQUAL_INT(4, r.list.jump(300 * kPitch));
  TEST_ASSERT_TRUE(r.list.map.lastPlanFull());
  TEST_ASSERT_EQUAL_INT(5, r.list.jump(500 * kPitch + 10));
  // Three rows on (more than the hardware scroll's step): two rows are shared.
  TEST_ASSERT_EQUAL_INT(3, r.list.jump(503 * kPitch + 10));
  TEST_ASSERT_TRUE(r.list.map.lastPlanFull());
  // One row on: the hardware scroll takes it as a move, and its row alone is new.
  TEST_ASSERT_EQUAL_INT(1, r.list.jump(504 * kPitch + 10));
  TEST_ASSERT_FALSE(r.list.map.lastPlanFull());
  // To the list's end, and its start.
  TEST_ASSERT_EQUAL_INT(4, r.list.jump(700 * kPitch - kBand));
  TEST_ASSERT_EQUAL_INT(4, r.list.jump(0));
  r.list.check();
  TEST_ASSERT_EQUAL_INT(0, r.list.unkept);
}

// A random session: moves of a line to the hardware scroll's step either way,
// jumps, rows ahead or not, rests, covers arriving. A move brings on three
// rows at the most, a band five; no item is ever in two sprites; and no row
// that is kept loses its sprite.
void test_a_random_session() {
  std::mt19937 rng(20261127);
  for (int32_t items : {700, 5, 40}) {
    for (int height : {kBand, 126, 148}) {
      List l(items, height);
      const int32_t maxOffset = std::max<int32_t>(0, items * kPitch - height);
      l.jump(0);
      for (int i = 0; i < 4000; ++i) {
        const int what = static_cast<int>(rng() % 100);
        if (what < 70) {  // a move
          const int32_t d = static_cast<int32_t>(rng() % kStep) + 1;
          const int32_t to = ListLayout::clamp(l.drawn + (rng() % 2 ? d : -d), maxOffset);
          if (to == l.drawn) continue;
          const int n = l.frame(to, rng() % 4 != 0);
          TEST_ASSERT_TRUE(n <= 3);
          TEST_ASSERT_FALSE(l.map.lastPlanFull());
          for (int k = static_cast<int>(rng() % 3); k > 0; --k) l.renderAhead();
        } else if (what < 78) {  // a jump
          const int32_t to = maxOffset ? static_cast<int32_t>(rng() % (maxOffset + 1)) : 0;
          TEST_ASSERT_TRUE(l.jump(to) <= 5);
          l.dir = 0;
        } else if (what < 86) {  // it rests
          l.dir = 0;
          l.rest();
        } else if (what < 93) {  // a finger catches it, and it gets its rows back
          l.grab();
          int back = 0;
          while (l.renderAhead()) ++back;
          TEST_ASSERT_TRUE(back <= 2);
          const Keep on = shown(l.drawn, l.height, static_cast<uint32_t>(l.items));
          for (uint32_t it = on.first; it <= on.last && l.items > 0; ++it) {
            TEST_ASSERT_TRUE(l.held.holds(static_cast<int32_t>(it)));
          }
          l.letGo();
        } else {  // a cover arrived
          l.forgetHidden();
        }
        l.check();
        // What shows is drawn from a sprite the frame had: every row on screen
        // was rendered by now, in a frame or before one.
        TEST_ASSERT_TRUE(l.drawn >= 0 && l.drawn <= maxOffset);
      }
      TEST_ASSERT_EQUAL_INT(0, l.unkept);
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_figures);
  RUN_TEST(test_which_sprite_an_item_takes);
  RUN_TEST(test_the_items_a_band_shows);
  RUN_TEST(test_the_items_a_move_brings_on);
  RUN_TEST(test_the_row_rendered_ahead);
  RUN_TEST(test_the_rows_rendered_beside_a_resting_list);
  RUN_TEST(test_the_row_a_caught_list_gets_back);
  RUN_TEST(test_the_time_a_row_has);
  RUN_TEST(test_a_first_frame_and_a_swipe_from_rest);
  RUN_TEST(test_a_fling_renders_no_row_in_a_frame);
  RUN_TEST(test_a_turn_only_pushes);
  RUN_TEST(test_a_flick_on_after_a_catch_only_pushes);
  RUN_TEST(test_a_turn_at_once_renders_its_row);
  RUN_TEST(test_a_jump_renders_its_band);
  RUN_TEST(test_a_random_session);
  return UNITY_END();
}
