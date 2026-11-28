// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for a list's frames (lib/core/ListFrames.h): what a loop pass
// does with the band (choose(), showRail()), when a move's hold begins on the
// panel's scan (window(), behind(), shownInUs(), start(), late(), lineTime()),
// and what the time between two frames is for (look(), rests()). Then the
// Core2's loop around a list, pass by pass with a clock of its own and a
// decoder above it on the core: a fling's frames, the ones that miss their
// refresh, and the rows they have to render themselves.
// Run: pio test -e native -f test_list_frames
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "FrameClock.h"
#include "KineticScroll.h"
#include "ListFrames.h"
#include "ListLayout.h"
#include "RowSlots.h"
#include "ScanSync.h"

#include "../support/ListRig.h"

using namespace listframes;
using Phase = KineticScroll::Phase;

void setUp() {}
void tearDown() {}

namespace {

constexpr int kBand = listrig::kBand;
// The Core2's panel as measured: 244 scan lines in 13.98 ms, row 0 at line 2.
constexpr int32_t kRefreshUs = 13980;
constexpr int kScanLines = 244;

ScanSync core2() {
  ScanSync s;
  ScanSync::Panel p;
  p.frameUs = kRefreshUs;
  p.lines = kScanLines;
  p.firstRow = 2;
  s.setPanel(p);
  s.setBand(72, kBand);
  return s;
}

// A list drawn at `drawn` with its scroll at `scroll`: at rest, the screen
// lit, the hardware scroll on, the panel's scan followed, a frame due.
Pass listAt(int32_t drawn, int32_t scroll) {
  Pass p;
  p.at = scroll;
  p.drawn = drawn;
  p.frameDue = true;
  p.scan = true;
  p.hwScroll = true;
  p.mapOffset = drawn;
  p.maxStep = kMaxStep;
  return p;
}

}  // namespace

// ---- a pass ----

void test_the_figures() {
  TEST_ASSERT_EQUAL_INT(42, kPitch);
  TEST_ASSERT_EQUAL_INT(84, kMaxStep);
  TEST_ASSERT_EQUAL_UINT32(60 + 84 * 140, holdFor(84, 140));
  TEST_ASSERT_EQUAL_UINT32(60 + 84 * 140, holdFor(-84, 140));
}

// A list at rest where it is drawn owes nothing; one not drawn yet, or owed
// whole, is drawn now where its scroll is.
void test_a_list_at_rest_and_a_first_frame() {
  Choice c = choose(listAt(4200, 4200));
  TEST_ASSERT_TRUE(c.frame == Frame::None);
  TEST_ASSERT_FALSE(c.owed);
  TEST_ASSERT_EQUAL_INT32(4200, c.off);

  Pass p = listAt(-1, 4200);  // nothing drawn yet
  p.hwScroll = false;
  p.scan = false;
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::Whole);
  TEST_ASSERT_TRUE(c.owed);
  TEST_ASSERT_EQUAL_INT32(4200, c.off);

  p = listAt(4200, 4200);  // drawn there, but owed whole (a reload, an overlay gone)
  p.force = true;
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::Whole);
  TEST_ASSERT_EQUAL_INT32(4200, c.off);
  // ... and it waits for the frame clock like any frame.
  p.frameDue = false;
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::None);
  TEST_ASSERT_TRUE(c.owed);
}

// A list that moves is drawn a step a frame on the panel's scan: where the
// scroll is, or two rows towards it.
void test_a_moving_list_steps_on_the_scan() {
  Pass p = listAt(4200, 4230);
  p.phase = Phase::Flinging;
  Choice c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::OnScan);
  TEST_ASSERT_EQUAL_INT32(4230, c.off);
  TEST_ASSERT_FALSE(c.rows);

  p.at = 4200 + 300;  // further than a hold moves: a step of two rows
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::OnScan);
  TEST_ASSERT_EQUAL_INT32(4200 + kMaxStep, c.off);
  p.at = 4200 - 300;
  TEST_ASSERT_EQUAL_INT32(4200 - kMaxStep, choose(p).off);

  // A finger that drags it, the same; one that is down and hasn't moved it
  // owes nothing.
  p = listAt(4200, 4212);
  p.phase = Phase::Dragging;
  p.dragged = true;
  p.touch = Touch::List;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::OnScan);
  p = listAt(4200, 4200);
  p.phase = Phase::Dragging;
  p.touch = Touch::List;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);

  // Not due: nothing yet, but owed.
  p = listAt(4200, 4230);
  p.phase = Phase::Flinging;
  p.frameDue = false;
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::None);
  TEST_ASSERT_TRUE(c.owed);
}

// Where the scan can't be followed (it can't be read, uiv0, the screen dark,
// the hardware scroll off) a move is drawn now.
void test_off_the_scan_a_move_is_drawn_now() {
  Pass p = listAt(4200, 4230);
  p.phase = Phase::Flinging;
  p.scan = false;
  const Choice c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::Whole);
  TEST_ASSERT_EQUAL_INT32(4230, c.off);
}

// The governor's whole rows hold while the list moves, the rounding inside the
// step; a list at rest is drawn where it is.
void test_whole_rows_while_it_moves() {
  Pass p = listAt(4200, 4200 + 100);
  p.phase = Phase::Flinging;
  p.wholeRows = true;
  Choice c = choose(p);
  TEST_ASSERT_TRUE(c.rows);
  TEST_ASSERT_EQUAL_INT32(4200 + 2 * kPitch, c.off);  // 100 lines: two whole rows of them
  p.at = 4200 + 50;
  TEST_ASSERT_EQUAL_INT32(4200 + kPitch, choose(p).off);
  p.at = 4200 + 30;  // under a row: the list waits for one
  c = choose(p);
  TEST_ASSERT_EQUAL_INT32(4200, c.off);
  TEST_ASSERT_TRUE(c.frame == Frame::None);
  // At rest the governor's word says nothing.
  p = listAt(4200, 4230);
  p.wholeRows = true;
  c = choose(p);
  TEST_ASSERT_FALSE(c.rows);
  TEST_ASSERT_EQUAL_INT32(4230, c.off);
}

// A jump (a scroll to a row, the jump grid) goes straight there, however far:
// no step towards it, and not on the scan.
void test_a_jump_goes_straight_there() {
  Pass p = listAt(4200, 21000);
  p.jumped = true;
  const Choice c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::Whole);
  TEST_ASSERT_EQUAL_INT32(21000, c.off);
}

// The rail sits in the scrolled band: off the glass while the list moves or
// still catches up with a finger, back at rest.
void test_the_rail_leaves_while_the_list_moves() {
  Pass p = listAt(4200, 4230);
  p.edge = true;
  p.railSprite = true;
  p.railUp = true;
  p.railOffset = 4200;
  p.phase = Phase::Flinging;
  Choice c = choose(p);
  TEST_ASSERT_FALSE(c.railWanted);
  TEST_ASSERT_TRUE(c.hideRail);
  TEST_ASSERT_TRUE(c.frame == Frame::OnScan);  // (with the rail gone the scan is followed)

  // The finger lifted with the list 300 lines behind it: at rest by the
  // scroll, but a step a frame still: the rail stays away.
  p.phase = Phase::Idle;
  p.at = 4500;
  p.railUp = false;
  c = choose(p);
  TEST_ASSERT_FALSE(c.railWanted);
  TEST_ASSERT_FALSE(c.hideRail);
  TEST_ASSERT_TRUE(c.frame == Frame::OnScan);

  // At rest where it is drawn: wanted, and put up after a pass that drew
  // nothing; up already, nothing; drawn again when a recount left it owed.
  p = listAt(4200, 4200);
  p.edge = true;
  p.railSprite = true;
  c = choose(p);
  TEST_ASSERT_TRUE(c.railWanted);
  TEST_ASSERT_TRUE(c.frame == Frame::None);
  TEST_ASSERT_TRUE(showRail(c.owed, Touch::None, false, c.railWanted, false, -1));
  TEST_ASSERT_FALSE(showRail(c.owed, Touch::None, false, c.railWanted, true, 4200));
  TEST_ASSERT_TRUE(showRail(c.owed, Touch::None, false, c.railWanted, true, -1));
  // ... but not while a recount's items are owed (they go first), unless a
  // finger rests on the rail; and never over a frame that is owed.
  TEST_ASSERT_FALSE(showRail(false, Touch::None, true, true, false, -1));
  TEST_ASSERT_TRUE(showRail(false, Touch::Rail, true, true, false, -1));
  TEST_ASSERT_FALSE(showRail(true, Touch::None, false, true, false, -1));
  // A list with no edge has none.
  p.edge = false;
  TEST_ASSERT_FALSE(choose(p).railWanted);
}

// Under a finger on the rail every position is a jump. Paced, it waits for
// the band to be due and the rail alone follows the finger; then the band is
// drawn a row a pass. Never on the scan: the rail is in the band.
void test_a_finger_on_the_rail() {
  Pass p = listAt(4200, 21000);
  p.edge = true;
  p.railSprite = true;
  p.railUp = true;
  p.railOffset = 4200;
  p.touch = Touch::Rail;
  p.jumped = true;
  p.pacing = true;
  p.pacerOwed = true;
  p.pacerDue = false;
  Choice c = choose(p);
  TEST_ASSERT_TRUE(c.railWanted);
  TEST_ASSERT_FALSE(c.hideRail);
  TEST_ASSERT_TRUE(c.frame == Frame::Rail);
  TEST_ASSERT_EQUAL_INT32(21000, c.off);
  // The rail there already (or none to draw): nothing.
  p.railOffset = 21000;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);
  p.railOffset = 4200;
  p.railSprite = false;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);
  p.railSprite = true;
  // The band due: a row a pass.
  p.pacerDue = true;
  c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::Band);
  TEST_ASSERT_EQUAL_INT32(21000, c.off);
  // A position a move away is one hold already: drawn now.
  p.at = 4200 + kPitch;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Whole);
  p.at = 4200 + kMaxStep + 1;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Band);
  p.at = 4200 - kMaxStep - 1;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Band);
  // Unpaced (uir0): every position at once, in one go.
  p.at = 21000;
  p.pacing = false;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Whole);
  // Nothing drawn in rows on a dark screen, or with the hardware scroll off.
  p.pacing = true;
  p.dark = true;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Whole);
  p.dark = false;
  p.hwScroll = false;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Whole);
}

// A band under way goes on whatever the frame clock says, and before
// anything else that is owed.
void test_a_band_goes_on_a_row_a_pass() {
  Pass p = listAt(21000, 30000);
  p.bandActive = true;
  p.touch = Touch::Rail;
  p.edge = true;
  p.jumped = true;
  p.frameDue = false;
  const Choice c = choose(p);
  TEST_ASSERT_TRUE(c.frame == Frame::BandRow);
  TEST_ASSERT_TRUE(c.railWanted);
}

// A recount's stale items: one a frame at a still pass, nothing else owed.
void test_a_recounts_items_at_a_still_pass() {
  Pass p = listAt(4200, 4200);
  p.staged = true;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Stage);
  // Not under a finger, on the list or on the rail.
  p.touch = Touch::List;
  p.phase = Phase::Dragging;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);
  p.touch = Touch::Rail;
  p.phase = Phase::Idle;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);
  // Not before the frame clock, and a frame that is owed goes first.
  p.touch = Touch::None;
  p.frameDue = false;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::None);
  p.frameDue = true;
  p.at = 4230;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::OnScan);
  // Its jump, its rows landed more than a move from where the list was
  // drawn, is a band a row a pass too (no whole band in one pass).
  p = listAt(4200, 21000);
  p.staged = true;
  p.stageBand = true;
  p.jumped = true;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Band);
  p.at = 4200 + kPitch;
  TEST_ASSERT_TRUE(choose(p).frame == Frame::Whole);
}

// ---- a move on the panel's scan ----

// The window is the refresh under way's, but one frame a refresh: once this
// refresh has its frame the next one's.
void test_one_frame_a_refresh() {
  const ScanSync sync = core2();
  // The scan 40 lines into the band, a step of 28 lines down the list.
  const int line = 2 + 72 + 40;
  const int64_t readUs = 5000000;
  const ScanSync::Window first = window(sync, line, readUs, 0, 28, 140);
  TEST_ASSERT_TRUE(first.showsUs > 0 && first.showsUs <= kRefreshUs);
  // The frame before showed in this same refresh: the next one's window, a
  // refresh on.
  const ScanSync::Window next = window(sync, line, readUs, readUs + first.showsUs, 28, 140);
  TEST_ASSERT_EQUAL_INT32(first.showsUs + kRefreshUs, next.showsUs);
  TEST_ASSERT_TRUE(next.earliestUs >= first.earliestUs + kRefreshUs - 1);
  // The frame before showed a refresh ago: this refresh's.
  const ScanSync::Window on = window(sync, line, readUs, readUs + first.showsUs - kRefreshUs, 28, 140);
  TEST_ASSERT_EQUAL_INT32(first.showsUs, on.showsUs);
}

// A refresh that went by without a frame: the next one isn't kept for its
// window as well. Nor a motion's first.
void test_a_list_behind_isnt_kept_waiting() {
  const int64_t readUs = 5000000;
  TEST_ASSERT_TRUE(behind(readUs, 6000, 0, kRefreshUs));  // a motion's first frame
  TEST_ASSERT_FALSE(behind(readUs, 6000, readUs + 6000 - kRefreshUs, kRefreshUs));      // a refresh on: in step
  TEST_ASSERT_TRUE(behind(readUs, 6000, readUs + 6000 - 2 * kRefreshUs, kRefreshUs));   // one was missed
  TEST_ASSERT_FALSE(behind(readUs, 6000, readUs + 6000 - kRefreshUs * 3 / 2, kRefreshUs));  // (just not)
  TEST_ASSERT_TRUE(behind(readUs, 6000, readUs + 6000 - kRefreshUs * 3 / 2 - 1, kRefreshUs));
}

// A list moving by itself is drawn where it will be when the frame shows; one
// drawn at once shows when its address goes out.
void test_where_a_coasting_list_is_drawn() {
  ScanSync::Window w;
  w.showsUs = 9000;
  TEST_ASSERT_EQUAL_INT32(9000, shownInUs(w, false, 4000));
  TEST_ASSERT_EQUAL_INT32(4000, shownInUs(w, true, 4000));
  w.late = true;
  TEST_ASSERT_EQUAL_INT32(4000, shownInUs(w, false, 4000));
}

// The hold begins now, at its window, or not in this pass.
void test_when_the_hold_begins() {
  const int64_t readUs = 5000000;
  ScanSync::Window w;
  w.earliestUs = 4000;
  w.latestUs = 7000;
  w.addressUs = 9000;
  w.showsUs = 12000;
  // A list moving by itself sleeps to its window (its rows took 1 ms).
  Start s = start(w, false, readUs, readUs + 1000, true);
  TEST_ASSERT_FALSE(s.atOnce);
  TEST_ASSERT_FALSE(s.comeBack);
  TEST_ASSERT_TRUE(s.earliestUs == readUs + 4000);
  TEST_ASSERT_TRUE(s.addressUs == readUs + 9000);
  // Under a finger the loop comes back for it, until it is 1.5 ms off.
  s = start(w, false, readUs, readUs + 1000, false);
  TEST_ASSERT_TRUE(s.comeBack);
  s = start(w, false, readUs, readUs + 2500, false);
  TEST_ASSERT_FALSE(s.comeBack);
  TEST_ASSERT_TRUE(s.earliestUs == readUs + 4000);
  // A window further off than a list sleeps (13 ms): the loop comes back.
  w.earliestUs = 15000;
  TEST_ASSERT_TRUE(start(w, false, readUs, readUs + 1000, true).comeBack);
  // Its window over, or the list behind: at once, the address as its lines
  // are out.
  w.earliestUs = 0;
  w.late = true;
  s = start(w, false, readUs, readUs + 1000, true);
  TEST_ASSERT_TRUE(s.atOnce);
  TEST_ASSERT_FALSE(s.comeBack);
  TEST_ASSERT_TRUE(s.earliestUs == readUs + 1000);
  TEST_ASSERT_TRUE(s.addressUs == 0);
  w.late = false;
  w.earliestUs = 4000;
  s = start(w, true, readUs, readUs + 1000, false);
  TEST_ASSERT_TRUE(s.atOnce);
  TEST_ASSERT_FALSE(s.comeBack);
}

void test_a_late_hold() {
  const int64_t readUs = 5000000;
  ScanSync::Window w;
  w.latestUs = 7000;
  TEST_ASSERT_FALSE(late(w, false, readUs, readUs + 7000));
  TEST_ASSERT_FALSE(late(w, false, readUs, readUs + 7000 + kLateOkUs));  // (half the margin before the band)
  TEST_ASSERT_TRUE(late(w, false, readUs, readUs + 7000 + kLateOkUs + 1));
  TEST_ASSERT_TRUE(late(w, true, readUs, readUs + 3000));  // drawn at once: late by its name
}

// What a line costs on the bus follows the holds, a quarter at a time; a
// short hold, or one something cut into, says nothing.
void test_what_a_line_costs() {
  TEST_ASSERT_EQUAL_UINT32(140, lineTime(140, 28, 60 + 28 * 140));
  TEST_ASSERT_EQUAL_UINT32(137, lineTime(140, 28, 60 + 28 * 128));   // (140 * 3 + 128) / 4
  TEST_ASSERT_EQUAL_UINT32(140, lineTime(140, 7, 60 + 7 * 100));     // under 8 lines
  TEST_ASSERT_EQUAL_UINT32(140, lineTime(140, 28, 60 + 28 * 280));   // twice a line's time: something cut in
  TEST_ASSERT_EQUAL_UINT32(140, lineTime(140, 28, 40));
}

// ---- between two frames ----

void test_what_the_time_between_frames_is_for() {
  LookIn l;
  l.at = 4230;
  l.drawn = 4230;
  l.moving = true;
  l.lastDir = 1;
  l.items = 700;
  l.onScan = true;
  l.frameUs = kRefreshUs;
  l.rowUs = 4000;
  // A refresh after the last hold began the next one does: a row fits until
  // 5 ms before (its 4 ms and one more).
  l.sinceHoldUs = 3000;
  TEST_ASSERT_TRUE(look(l) == Look::Ahead);
  l.sinceHoldUs = 8900;
  TEST_ASSERT_TRUE(look(l) == Look::Ahead);
  l.sinceHoldUs = 9000;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  // A hold two refreshes ago says nothing: a motion's first frames take
  // what comes.
  l.sinceHoldUs = 2 * kRefreshUs;
  TEST_ASSERT_TRUE(look(l) == Look::Ahead);
  // No hold yet, or off the scan: by the frame clock's deadline.
  l.sinceHoldUs = -1;
  l.untilDueMs = 5;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.untilDueMs = 6;
  TEST_ASSERT_TRUE(look(l) == Look::Ahead);
  l.onScan = false;
  l.sinceHoldUs = 3000;
  l.untilDueMs = 0;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  // Nothing to be ahead of: no frame yet this way, at rest, a band owed
  // whole, a jump, nothing drawn, no items.
  l.onScan = true;
  l.lastDir = 0;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.lastDir = -1;
  TEST_ASSERT_TRUE(look(l) == Look::Ahead);
  l.moving = false;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.moving = true;
  l.force = true;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.force = false;
  l.jumped = true;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.jumped = false;
  l.drawn = -1;
  TEST_ASSERT_TRUE(look(l) == Look::None);
  l.drawn = 4230;
  l.items = 0;
  TEST_ASSERT_TRUE(look(l) == Look::None);
}

// A caught list, standing still under its finger: the row its fling's
// look-ahead took, whatever the clock says; caught between two of its frames
// it owes the last one first.
void test_a_caught_list_gets_its_row() {
  LookIn l;
  l.at = 4230;
  l.drawn = 4230;
  l.moving = true;
  l.grabbed = true;
  l.items = 700;
  l.onScan = true;
  l.frameUs = kRefreshUs;
  l.rowUs = 4000;
  l.sinceHoldUs = 9500;  // (no room by the scan: it doesn't ask)
  TEST_ASSERT_TRUE(look(l) == Look::Unheld);
  l.at = 4240;
  TEST_ASSERT_TRUE(look(l) == Look::None);
}

void test_a_list_at_rest() {
  RestIn r;
  r.at = 4200;
  r.drawn = 4200;
  r.items = 700;
  TEST_ASSERT_TRUE(rests(r));
  r.touch = Touch::List;
  TEST_ASSERT_FALSE(rests(r));
  r.touch = Touch::None;
  r.phase = Phase::Snapping;
  TEST_ASSERT_FALSE(rests(r));
  r.phase = Phase::Idle;
  r.at = 4201;
  TEST_ASSERT_FALSE(rests(r));
  r.at = 4200;
  r.bandActive = true;
  TEST_ASSERT_FALSE(rests(r));
  r.bandActive = false;
  r.dark = true;
  TEST_ASSERT_FALSE(rests(r));
  r.dark = false;
  r.force = true;
  TEST_ASSERT_FALSE(rests(r));
  r.force = false;
  r.items = 0;
  TEST_ASSERT_FALSE(rests(r));
}

// ---- the loop around a list, with a clock ----

namespace {

// The Core2's loop with a list page up, a pass after another, each doing what
// ui/Ui and ui/ListView do with it: the frame clock's deadline, the pass's
// choice (choose()), a move's hold on the panel's scan (window(), start()),
// a row between two frames (look()), over the real scroll (KineticScroll),
// the real scan rule (ScanSync) and the real sprites' rules (RowSlots, the
// hardware scroll's map: listrig::List).
//
// What things cost is the model's: a row's render, a line on the bus, the
// rest of a pass. And one task above the loop, the decoder, which takes the
// core for a while at a steady pace (an MP3: 10 ms in every 27). The loop's
// own work waits for it; the loop asleep for its window, and its hold (above
// the decoder: ui/PanelScan's HoldBoost), don't.
struct Loop {
  // What things cost, in microseconds.
  uint32_t rowUs = 3000;   // a row's render with the core to itself
  uint32_t lineUs = 130;   // a line on the bus
  uint32_t passUs = 1200;  // a pass besides the list
  // The decoder: `decodeUs` of the core in every `decodeEveryUs` (0: nothing plays).
  uint32_t decodeUs = 0, decodeEveryUs = 27000;
  int64_t decodeFrom = 1003000;  // its first turn

  int64_t us = 1000000;  // the clock
  int64_t decoded = 0;   // the decoder's work done
  listrig::List list{700};
  KineticScroll scroll;
  ScanSync sync = core2();
  FrameClock clock{10};  // (a list's frame clock on the scan: it only keeps two frames out of one refresh)
  // The list's own.
  bool dragged = false;
  int64_t lastHoldUs = 0, lastShowUs = 0;
  uint32_t lineEst = 140, rowEst = 4000;
  // The counts of a motion.
  int frames = 0;        // moves drawn
  int late = 0;          // ... at once, or begun after their window
  int cadence = 0;       // a fast fling's frames after another of its frames
  int gaps = 0;          // ... more than a refresh and a half after it
  int64_t gapMaxUs = 0;
  int64_t lastFastUs = 0;
  int rowsInFrames = 0;  // rows a frame had to render itself
  int rowsAhead = 0;     // ... rendered between two frames
  int64_t lastFrameUs = 0;
  int64_t firstFrameUs = 0;

  Loop() {
    scroll.setExtent(700.0f * kPitch, static_cast<float>(kBand));
    scroll.jumpTo(100.0f * kPitch);
    list.jump(100 * kPitch);
    list.rest();
  }

  // ---- the core ----
  int64_t owed() const {
    if (!decodeUs || us < decodeFrom) return 0;
    return ((us - decodeFrom) / decodeEveryUs + 1) * decodeUs - decoded;
  }
  int64_t nextTurn() const {
    if (us < decodeFrom) return decodeFrom;
    return decodeFrom + ((us - decodeFrom) / decodeEveryUs + 1) * decodeEveryUs;
  }
  // The loop works `cost` at its own priority: the decoder's turns come first.
  void work(int64_t cost) {
    for (;;) {
      const int64_t turn = owed();
      if (turn > 0) {
        us += turn;
        decoded += turn;
        continue;
      }
      if (cost <= 0) return;
      const int64_t run = decodeUs ? std::min(cost, nextTurn() - us) : cost;
      us += run;
      cost -= run;
    }
  }
  // ... above the decoder (the hold): its turns wait, and come the moment
  // the loop is back at its own priority.
  void above(int64_t cost) {
    us += cost;
    work(0);
  }
  // The loop asleep until `at` (its timer wakes it over the decoder): the
  // decoder has the core meanwhile.
  void sleepTo(int64_t at) {
    while (us < at) {
      const int64_t turn = owed();
      if (turn > 0) {
        const int64_t run = std::min(turn, at - us);
        us += run;
        decoded += run;
      } else {
        us = decodeUs ? std::min(at, nextTurn()) : at;
      }
    }
  }

  // ---- the list ----
  int scanLine() const { return static_cast<int>((us % kRefreshUs) * kScanLines / kRefreshUs); }
  int32_t at() const { return static_cast<int32_t>(std::lround(scroll.offset())); }
  bool moving() const { return listframes::moving(scroll.phase(), dragged); }
  uint32_t ms() const { return static_cast<uint32_t>(us / 1000); }

  // A row rendered at the loop's priority; what it took is what the next one
  // is taken to cost (ListView::noteRowTime()).
  void renderRow() {
    const int64_t t0 = us;
    work(rowUs);
    rowEst = rowslots::rowTime(rowEst, static_cast<uint32_t>(us - t0));
  }

  // ui/ListView::scanFrame().
  bool scanFrame(bool rows) {
    const int line = scanLine();
    const int64_t readUs = us;
    int32_t off = frameOffset(list.drawn, at(), true, rows);
    int d = off - list.drawn;
    if (d == 0) return false;
    ScanSync::Window w = window(sync, line, readUs, lastShowUs, d, lineEst);
    const bool isBehind = behind(readUs, w.showsUs, lastShowUs, kRefreshUs);
    const bool coasting = scroll.phase() == Phase::Flinging || scroll.phase() == Phase::Snapping;
    if (coasting) {
      const int32_t showsUs = shownInUs(w, isBehind, holdFor(d, lineEst));
      scroll.update(ms() + static_cast<uint32_t>((showsUs + 500) / 1000));
      off = frameOffset(list.drawn, at(), true, rows);
      d = off - list.drawn;
      if (d == 0) return false;
      w = window(sync, line, readUs, lastShowUs, d, lineEst);
    }
    // prerender(): the rows the move brings on, at the loop's priority.
    const rowslots::Items on = rowslots::broughtOn(list.drawn, off, list.height, list.items);
    list.keep = rowslots::shown(off, list.height, static_cast<uint32_t>(list.items));
    for (int32_t i = on.first; i <= on.last; ++i) {
      if (list.rendered(i)) {
        renderRow();
        ++rowsInFrames;
      }
    }
    const Start s = start(w, isBehind, readUs, us, coasting);
    if (s.comeBack) return false;
    sleepTo(s.earliestUs);
    const int64_t began = us;
    // The hold, above the decoder: its lines (a row still to render is
    // rendered in it), then the address once the scan has left the band.
    const int inHold = list.frame(off);
    rowsInFrames += inHold;
    int64_t held = holdFor(d, lineUs) + static_cast<int64_t>(inHold) * rowUs;
    if (s.addressUs > began + held) held = s.addressUs - began;
    above(held);
    if (listframes::late(w, s.atOnce, readUs, began)) ++late;
    // A fling's cadence: the time between its frames while every refresh
    // moves it (over 150 px/s: the Ui's count).
    const bool fast = scroll.phase() == Phase::Flinging && std::fabs(scroll.velocity()) > 150.0f;
    if (fast && lastFastUs) {
      const int64_t gap = began - lastFastUs;
      ++cadence;
      gapMaxUs = std::max(gapMaxUs, gap);
      if (gap > kRefreshUs + kRefreshUs / 2) ++gaps;
    }
    lastFastUs = fast ? began : 0;
    if (!lastFrameUs) firstFrameUs = began;
    lastFrameUs = began;
    lastHoldUs = began;
    lastShowUs = readUs + w.showsUs;
    lineEst = lineTime(lineEst, d < 0 ? -d : d, static_cast<uint32_t>(holdFor(d, lineUs)));
    ++frames;
    return true;
  }

  // ui/ListView::update(), ui/Ui's pass around it.
  void pass() {
    const bool due = clock.due(ms());
    scroll.update(ms());
    Pass p;
    p.at = at();
    p.drawn = list.drawn;
    p.phase = scroll.phase();
    p.dragged = dragged;
    p.frameDue = due;
    p.hwScroll = list.map.valid();
    p.scan = p.hwScroll;
    p.mapOffset = list.map.offset();
    p.maxStep = kMaxStep;
    const Choice c = choose(p);
    bool frame = false;
    if (c.frame == Frame::OnScan) {
      frame = scanFrame(c.rows);
    } else if (c.frame == Frame::Whole) {
      rowsInFrames += list.jump(c.off);
      frame = true;
    }
    if (frame) clock.drawn(ms());
    // Between frames: a row ahead while the list moves, a neighbour at rest.
    const bool animating = scroll.moving() || at() != list.drawn;
    if (animating) {
      LookIn l;
      l.at = at();
      l.drawn = list.drawn;
      l.moving = moving();
      l.grabbed = list.grabbed;
      l.lastDir = list.dir;
      l.items = list.items;
      l.onScan = p.scan;
      l.sinceHoldUs = lastHoldUs ? us - lastHoldUs : -1;
      l.frameUs = kRefreshUs;
      l.untilDueMs = clock.msUntilDue(ms());
      l.rowUs = rowEst;
      if (look(l) != Look::None && list.renderAhead()) {
        renderRow();
        ++rowsAhead;
      }
    } else if (!frame && list.renderNeighbours()) {
      renderRow();
    }
    work(passUs);
  }

  // A fling at `pxPerS` from where the list rests, to its rest; the counts
  // are the motion's.
  void fling(float pxPerS) {
    frames = late = cadence = gaps = rowsInFrames = rowsAhead = 0;
    gapMaxUs = lastFrameUs = firstFrameUs = lastFastUs = 0;
    lastHoldUs = lastShowUs = 0;
    list.dir = 0;
    list.betweenFrames = 0;
    scroll.fling(ms(), pxPerS);
    int guard = 0;
    while ((scroll.moving() || at() != list.drawn) && ++guard < 20000) {
      pass();
      list.check();
    }
    TEST_ASSERT_TRUE(guard < 20000);
    // At rest: the neighbours, for the next one.
    for (int i = 0; i < 8; ++i) pass();
  }
  float fps() const {
    return frames > 1 ? (frames - 1) * 1e6f / static_cast<float>(lastFrameUs - firstFrameUs) : 0.0f;
  }
};

}  // namespace

// With the core to itself: a fling at the cap has a frame at nearly every
// refresh, none of them a refresh and a half after the one before, and every
// row it brings on was rendered between two frames. (The Core2, the same
// fling with nothing playing: 199-203 frames at 67-69 a second, at most 1 of
// 140 apart, 14-16 ms at the most, no row in a frame.)
void test_a_fling_with_the_core_to_itself() {
  Loop l;
  l.fling(2000.0f);
  char line[200];
  snprintf(line, sizeof(line),
           "nothing playing: %d frames (%.1f a second), %d late; %d of %d apart (%.1f ms at most); rows: %d in frames, %d ahead",
           l.frames, l.fps(), l.late, l.gaps, l.cadence, l.gapMaxUs / 1000.0, l.rowsInFrames, l.rowsAhead);
  TEST_MESSAGE(line);
  TEST_ASSERT_TRUE(l.frames >= 195);
  TEST_ASSERT_TRUE(l.fps() > 68.0f);
  TEST_ASSERT_TRUE(l.cadence > 120);
  TEST_ASSERT_EQUAL_INT(0, l.gaps);
  TEST_ASSERT_TRUE(l.gapMaxUs < kRefreshUs + kRefreshUs / 2);
  TEST_ASSERT_EQUAL_INT(0, l.rowsInFrames);
  TEST_ASSERT_TRUE(l.rowsAhead >= 30);
  // And back up the list.
  l.fling(-2000.0f);
  TEST_ASSERT_EQUAL_INT(0, l.rowsInFrames);
  TEST_ASSERT_EQUAL_INT(0, l.gaps);
  TEST_ASSERT_TRUE(l.fps() > 68.0f);
}

// With an MP3's decoder above the loop (10 ms of the core in every 27), at
// each of nine places its turns can fall against the fling: the frames keep
// most of their pace, but one in five comes a refresh and a half and more
// after the one before, and some rows are left to the frames that bring them
// on. A row the decoder cut into counts for more (ListView::noteRowTime()),
// so the next ones find no time before a frame, until a frame's own render
// brings the figure down again. (The Core2, an MP3 playing: 54-56 frames a
// second, 26-30 of a hundred apart, 37-54 ms at the most, 2 to 10 rows
// rendered in frames a fling.) This is the cost a later change is to take
// away: today's rules' figures, written down to be moved.
void test_a_fling_under_a_decoder() {
  int rows = 0, rowsMin = 1000, rowsMax = 0, gaps = 0, cadence = 0;
  float fps = 0;
  int64_t gapMaxUs = 0;
  constexpr int kPlaces = 9;
  for (int place = 0; place < kPlaces; ++place) {
    Loop l;
    l.decodeUs = 10000;
    l.decodeFrom = l.us + place * 3000;
    l.fling(2000.0f);
    TEST_ASSERT_TRUE(l.frames > 150);
    TEST_ASSERT_TRUE(l.fps() > 52.0f && l.fps() < 63.0f);
    TEST_ASSERT_TRUE(l.gaps > 0);
    TEST_ASSERT_TRUE(l.rowsInFrames >= 1 && l.rowsInFrames <= 20);
    TEST_ASSERT_TRUE(l.rowsInFrames + l.rowsAhead >= 30);
    rows += l.rowsInFrames;
    rowsMin = std::min(rowsMin, l.rowsInFrames);
    rowsMax = std::max(rowsMax, l.rowsInFrames);
    gaps += l.gaps;
    cadence += l.cadence;
    fps += l.fps();
    gapMaxUs = std::max(gapMaxUs, l.gapMaxUs);
  }
  char line[200];
  snprintf(line, sizeof(line),
           "an MP3's decoder: %.1f frames a second, %d of 100 apart (%.1f ms at most); rows in frames %d-%d a fling (%.1f)",
           fps / kPlaces, gaps * 100 / cadence, gapMaxUs / 1000.0, rowsMin, rowsMax, static_cast<float>(rows) / kPlaces);
  TEST_MESSAGE(line);
  TEST_ASSERT_TRUE(gaps * 100 / cadence >= 10 && gaps * 100 / cadence <= 35);
}

// The decoder's share is what it costs: a lighter one (8 ms in 27) leaves
// fewer frames apart than an MP3's, a heavier one (12 ms) more.
void test_a_heavier_decoder_costs_more() {
  int apart[3] = {0, 0, 0};
  const uint32_t turns[3] = {8000, 10000, 12000};
  for (int k = 0; k < 3; ++k) {
    int gaps = 0, cadence = 0;
    for (int place = 0; place < 9; ++place) {
      Loop l;
      l.decodeUs = turns[k];
      l.decodeFrom = l.us + place * 3000;
      l.fling(2000.0f);
      gaps += l.gaps;
      cadence += l.cadence;
    }
    apart[k] = gaps * 100 / cadence;
  }
  TEST_ASSERT_TRUE(apart[0] < apart[1]);
  TEST_ASSERT_TRUE(apart[1] < apart[2]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_figures);
  RUN_TEST(test_a_list_at_rest_and_a_first_frame);
  RUN_TEST(test_a_moving_list_steps_on_the_scan);
  RUN_TEST(test_off_the_scan_a_move_is_drawn_now);
  RUN_TEST(test_whole_rows_while_it_moves);
  RUN_TEST(test_a_jump_goes_straight_there);
  RUN_TEST(test_the_rail_leaves_while_the_list_moves);
  RUN_TEST(test_a_finger_on_the_rail);
  RUN_TEST(test_a_band_goes_on_a_row_a_pass);
  RUN_TEST(test_a_recounts_items_at_a_still_pass);
  RUN_TEST(test_one_frame_a_refresh);
  RUN_TEST(test_a_list_behind_isnt_kept_waiting);
  RUN_TEST(test_where_a_coasting_list_is_drawn);
  RUN_TEST(test_when_the_hold_begins);
  RUN_TEST(test_a_late_hold);
  RUN_TEST(test_what_a_line_costs);
  RUN_TEST(test_what_the_time_between_frames_is_for);
  RUN_TEST(test_a_caught_list_gets_its_row);
  RUN_TEST(test_a_list_at_rest);
  RUN_TEST(test_a_fling_with_the_core_to_itself);
  RUN_TEST(test_a_fling_under_a_decoder);
  RUN_TEST(test_a_heavier_decoder_costs_more);
  return UNITY_END();
}
