// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for Now Playing's seek bar (SeekBar, docs/SEEK-BAR.md): x to
// time and back, the ends and the reach, the tap, the drag and its grabs,
// the stay detent, sliding off, the readout's side, what it shows and its
// texts, every threshold on both sides, and one seek per touch, also with
// a real TouchRecognizer. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "InputEvent.h"
#include "SeekBar.h"
#include "TouchRecognizer.h"
#include "TrackSeek.h"

void setUp() {}
void tearDown() {}

namespace {

using T = InputEvent::Type;
using End = SeekBar::End;
constexpr uint32_t kL = 245000;  // 4:05: the reach is 3:59
constexpr int kY = 150;          // the line's row

InputEvent ev(T type, int x, int y = kY, uint32_t ms = 0, int dx = 0, int dy = 0, uint8_t edges = 0) {
  InputEvent e;
  e.type = type;
  e.x = static_cast<int16_t>(x);
  e.y = static_cast<int16_t>(y);
  e.dx = static_cast<int16_t>(dx);
  e.dy = static_cast<int16_t>(dy);
  e.edges = edges;
  e.ms = ms;
  return e;
}

// A Down at x on entry 7, playing at `live` of `len`.
SeekBar pressed(int x, uint32_t live, uint32_t len = kL, uint32_t ms = 0) {
  SeekBar b;
  TEST_ASSERT_TRUE(b.down(ev(T::Down, x, kY, ms), 7, live, len));
  return b;
}

}  // namespace

// ---- x and time ----

void test_ms_at_maps_the_line() {
  TEST_ASSERT_EQUAL_UINT32(0, SeekBar::msAt(12, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(0, SeekBar::msAt(0, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(0, SeekBar::msAt(-5, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(23000, SeekBar::msAt(40, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(122000, SeekBar::msAt(160, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(196000, SeekBar::msAt(250, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(238000, SeekBar::msAt(300, 0, kL));
  TEST_ASSERT_EQUAL_UINT32(239000, SeekBar::msAt(308, 0, kL));  // the reach
  TEST_ASSERT_EQUAL_UINT32(239000, SeekBar::msAt(319, 0, kL));
  uint32_t before = 0;
  for (int x = -10; x <= 330; ++x) {
    const uint32_t ms = SeekBar::msAt(x, 0, kL);
    TEST_ASSERT_EQUAL_UINT32(0, ms % 1000);
    TEST_ASSERT_TRUE(ms >= before);
    before = ms;
  }
}

// The user's calibrated panel never reads below ~26 or above ~281: the
// clamped readings' flags are how the ends are reached.
void test_ms_at_takes_the_clamped_edges() {
  TEST_ASSERT_EQUAL_UINT32(0, SeekBar::msAt(200, InputEvent::kEdgeLeft, kL));
  TEST_ASSERT_EQUAL_UINT32(239000, SeekBar::msAt(282, InputEvent::kEdgeRight, kL));
  TEST_ASSERT_EQUAL_UINT32(0, SeekBar::msAt(26, InputEvent::kEdgeLeft, kL));
}

// Whatever x, a seek never lands in the tail rule's last 5 s (where a
// start goes to 0:00), and its reach is the length less 6 s at most.
void test_no_seek_lands_in_the_tail() {
  const uint32_t lengths[] = {10000, 10999, 60000, 245000, 3600000, 36000000};
  for (uint32_t len : lengths) {
    for (int x = -10; x <= 330; ++x) {
      const uint32_t ms = SeekBar::msAt(x, 0, len);
      TEST_ASSERT_EQUAL_UINT32(ms, trackseek::startMs(ms, len));
      TEST_ASSERT_TRUE(ms <= len - 6000);
    }
    TEST_ASSERT_EQUAL_UINT32(trackseek::seekLimitMs(len), SeekBar::msAt(0, InputEvent::kEdgeRight, len));
  }
}

void test_seekable() {
  TEST_ASSERT_FALSE(SeekBar::seekable(0));
  TEST_ASSERT_FALSE(SeekBar::seekable(9999));
  TEST_ASSERT_TRUE(SeekBar::seekable(10000));
  SeekBar b;
  TEST_ASSERT_FALSE(b.down(ev(T::Down, 160), 7, 2000, 9999));
  TEST_ASSERT_FALSE(b.active());
  const SeekBar::Out o = b.onEvent(ev(T::Tap, 160), 2000);
  TEST_ASSERT_EQUAL(End::None, o.end);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_FALSE(b.down(ev(T::Down, 160), 7, 0, 0));  // the dotted line
}

// xOf() is drawProgress()'s fill: min(pos, len) x 296 / len.
void test_x_of_is_the_drawing() {
  TEST_ASSERT_EQUAL_INT(0, SeekBar::xOf(0, kL));
  TEST_ASSERT_EQUAL_INT(296, SeekBar::xOf(kL, kL));
  TEST_ASSERT_EQUAL_INT(296, SeekBar::xOf(kL + 5000, kL));
  TEST_ASSERT_EQUAL_INT(0, SeekBar::xOf(60000, 0));
  TEST_ASSERT_EQUAL_INT(72, SeekBar::xOf(60000, kL));  // the knob at x 84
  // Back and forth, up to the reach: within a second's px (at least 1).
  const uint32_t lengths[] = {10000, 60000, 245000, 3600000};
  for (uint32_t len : lengths) {
    const int perSecond = std::max(1, static_cast<int>(296000 / len + 1));
    for (int x = 12; x < 308; ++x) {
      const uint32_t ms = SeekBar::msAt(x, 0, len);
      if (ms >= trackseek::seekLimitMs(len)) break;
      TEST_ASSERT_TRUE(std::abs(SeekBar::xOf(ms, len) - (x - 12)) <= perSecond);
    }
  }
}

// ---- the tap ----

void test_a_tap_seeks_where_it_landed() {
  SeekBar b = pressed(160, 60000);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Pressed, b.phase());
  TEST_ASSERT_EQUAL_INT(84, b.knobX());
  const SeekBar::Out o = b.onEvent(ev(T::Tap, 160), 60000);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_TRUE(o.tap);
  TEST_ASSERT_FALSE(o.tick);  // (the tap's tick waits for the player)
  TEST_ASSERT_EQUAL_UINT32(122000, o.ms);
  TEST_ASSERT_FALSE(b.active());
  TEST_ASSERT_EQUAL_UINT32(7, b.key());
  TEST_ASSERT_EQUAL_UINT32(kL, b.lengthMs());
}

// The knob is at x 84 (60 s of 4:05): a tap within 4 line px of it is no seek.
void test_a_tap_on_the_knob_seeks_nothing() {
  const int stay[] = {81, 84, 89};
  for (int x : stay) {
    SeekBar b = pressed(x, 60000);
    TEST_ASSERT_EQUAL(End::Stay, b.onEvent(ev(T::Tap, x), 60000).end);
    TEST_ASSERT_FALSE(b.active());
  }
  SeekBar b = pressed(80, 60000);
  SeekBar::Out o = b.onEvent(ev(T::Tap, 80), 60000);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_EQUAL_UINT32(56000, o.ms);
  b = pressed(90, 60000);
  o = b.onEvent(ev(T::Tap, 90), 60000);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_EQUAL_UINT32(64000, o.ms);
}

// ---- the drag ----

// From away from the knob: the knob comes to the finger; the music plays on
// meanwhile (nothing but the knob moves), and the lift seeks once.
void test_a_drag_seeks_once_at_the_lift() {
  SeekBar b = pressed(200, 60000, kL, 1000);
  TEST_ASSERT_FALSE(b.knobGrab());
  SeekBar::Out o = b.onEvent(ev(T::DragStart, 214, kY, 1100, 14, 0), 60000);
  TEST_ASSERT_EQUAL(End::None, o.end);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_TRUE(b.scrubbing());
  TEST_ASSERT_EQUAL_UINT32(167000, b.targetMs());
  int ticks = 0;
  for (int x = 215; x <= 250; ++x) {
    o = b.onEvent(ev(T::DragMove, x, kY, 1100 + x, 1, 0), 60000);
    TEST_ASSERT_EQUAL(End::None, o.end);
    ticks += o.tick;
  }
  TEST_ASSERT_EQUAL_INT(0, ticks);
  TEST_ASSERT_EQUAL_UINT32(196000, b.targetMs());
  TEST_ASSERT_EQUAL_INT(12 + SeekBar::xOf(196000, kL), b.knobX());
  TEST_ASSERT_EQUAL_INT(84, b.markerX());
  o = b.onEvent(ev(T::DragEnd, 250, kY, 1600), 60000);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_FALSE(o.tap);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_EQUAL_UINT32(196000, o.ms);
  TEST_ASSERT_FALSE(b.active());
  TEST_ASSERT_EQUAL_UINT32(1600 - (1100 + 249), b.heldMs(1600));  // 3:16 since the move to x 249
  o = b.onEvent(ev(T::Fling, 250, kY, 1600), 60000);
  TEST_ASSERT_EQUAL(End::None, o.end);
  TEST_ASSERT_FALSE(o.tick);
}

// From the knob: no jump at the DragStart (the slop already moved the
// finger 13 px), then the finger's movement from there.
void test_a_knob_grab_doesnt_jump() {
  SeekBar b = pressed(90, 60000);
  TEST_ASSERT_TRUE(b.knobGrab());
  SeekBar::Out o = b.onEvent(ev(T::DragStart, 103, kY, 0, 13, 0), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_TRUE(b.staying());
  TEST_ASSERT_EQUAL_INT(84, b.knobX());
  o = b.onEvent(ev(T::DragMove, 153, kY, 0, 50, 0), 60000);  // vx 134
  TEST_ASSERT_EQUAL_UINT32(100000, b.targetMs());
  TEST_ASSERT_FALSE(b.staying());
  o = b.onEvent(ev(T::DragEnd, 153), 60000);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_EQUAL_UINT32(100000, o.ms);
  // A clamped reading at the Down is never a grab.
  SeekBar c;
  TEST_ASSERT_TRUE(c.down(ev(T::Down, 26, kY, 0, 0, 0, InputEvent::kEdgeLeft), 7, 0, kL));
  TEST_ASSERT_FALSE(c.knobGrab());
}

// A finger that rests on the knob (noHold() lets it) while the music plays
// on, then slides: the knob stays where it is at the DragStart (where it
// plays then), not where it was at the Down, and follows the finger from
// there: no jump back, no seek behind where it plays.
void test_a_knob_grab_after_a_rest_doesnt_jump_back() {
  constexpr uint32_t len = 60000;  // a 1:00 click track: 4.9 px a second
  SeekBar b = pressed(110, 20000, len);  // the knob at x 110
  TEST_ASSERT_TRUE(b.knobGrab());
  b.live(21500);  // 1.5 s on the knob: the pressed knob at x 118
  TEST_ASSERT_EQUAL_INT(118, b.knobX());
  SeekBar::Out o = b.onEvent(ev(T::DragStart, 123, kY, 1500, 13, 0), 21500);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_TRUE(b.staying());
  TEST_ASSERT_EQUAL_INT(118, b.knobX());
  o = b.onEvent(ev(T::DragMove, 133, kY, 1600, 10, 0), 21600);  // 10 px on: vx 128
  TEST_ASSERT_FALSE(b.staying());
  TEST_ASSERT_EQUAL_UINT32(23000, b.targetMs());
  TEST_ASSERT_TRUE(b.knobX() > 118);
  o = b.onEvent(ev(T::DragEnd, 133, kY, 1700), 21700);
  TEST_ASSERT_EQUAL(End::Seek, o.end);
  TEST_ASSERT_EQUAL_UINT32(23000, o.ms);  // forward, as the finger went
}

void test_a_vertical_drag_is_let_go() {
  SeekBar b = pressed(160, 60000);
  SeekBar::Out o = b.onEvent(ev(T::DragStart, 163, kY - 13, 0, 3, -13), 60000);
  TEST_ASSERT_EQUAL(End::Let, o.end);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_FALSE(b.active());
  o = b.onEvent(ev(T::DragEnd, 170, 120), 60000);
  TEST_ASSERT_EQUAL(End::None, o.end);
}

// The stay detent: a move into it ticks once; a knob grab that starts in
// it doesn't (beyond the scrub's own tick); the marker coming onto a still
// knob makes it staying without a tick; a lift there seeks nothing.
void test_the_detent_snaps_and_ticks() {
  SeekBar b = pressed(200, 60000);
  b.onEvent(ev(T::DragStart, 214, kY, 0, 14, 0), 60000);
  TEST_ASSERT_FALSE(b.staying());
  int ticks = 0;
  for (int x = 213; x >= 86; --x) ticks += b.onEvent(ev(T::DragMove, x, kY, 0, -1, 0), 60000).tick;
  TEST_ASSERT_EQUAL_INT(1, ticks);
  TEST_ASSERT_TRUE(b.staying());
  TEST_ASSERT_EQUAL_INT(84, b.knobX());  // snapped onto the marker
  ticks = 0;
  for (int x = 85; x >= 80; --x) ticks += b.onEvent(ev(T::DragMove, x, kY, 0, -1, 0), 60000).tick;
  TEST_ASSERT_EQUAL_INT(0, ticks);  // (inside it, then out: nothing)
  TEST_ASSERT_FALSE(b.staying());
  TEST_ASSERT_EQUAL_INT(1, b.onEvent(ev(T::DragMove, 86, kY), 60000).tick);  // in again
  TEST_ASSERT_EQUAL(End::Stay, b.onEvent(ev(T::DragEnd, 86), 60000).end);

  // A knob grab: staying at its start, one tick (the scrub's).
  SeekBar g = pressed(84, 60000);
  SeekBar::Out o = g.onEvent(ev(T::DragStart, 97, kY, 0, 13, 0), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_TRUE(g.staying());
  TEST_ASSERT_EQUAL_INT(0, g.onEvent(ev(T::DragMove, 98, kY), 60000).tick);  // still in it

  // The marker comes onto a still knob (playing): staying, no tick.
  SeekBar m = pressed(200, 60000);
  m.onEvent(ev(T::DragStart, 120, kY, 0, -80, 0), 60000);  // the knob at x 120 (89 s)
  TEST_ASSERT_FALSE(m.staying());
  m.live(80000);
  TEST_ASSERT_FALSE(m.staying());
  m.live(87000);
  TEST_ASSERT_TRUE(m.staying());
  TEST_ASSERT_EQUAL_INT(m.markerX(), m.knobX());
  TEST_ASSERT_EQUAL(End::Stay, m.onEvent(ev(T::DragEnd, 120), 87000).end);
}

// Off the bar: above y 106 (onto the artist row) or onto the strip (y 240);
// back from y 114, or above 232. A tick each way; a lift while off seeks
// nothing; while off the knob is back where it plays.
void test_off_and_back() {
  SeekBar b = pressed(200, 60000);
  b.onEvent(ev(T::DragStart, 214, kY, 0, 14, 0), 60000);
  SeekBar::Out o = b.onEvent(ev(T::DragMove, 214, 105), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_EQUAL_INT(b.markerX(), b.knobX());
  o = b.onEvent(ev(T::DragMove, 230, 111), 60000);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_EQUAL_INT(b.markerX(), b.knobX());
  o = b.onEvent(ev(T::DragMove, 230, 114), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
  TEST_ASSERT_EQUAL_UINT32(SeekBar::msAt(230, 0, kL), b.targetMs());  // back by the grab's mapping
  o = b.onEvent(ev(T::DragMove, 230, 240), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  o = b.onEvent(ev(T::DragMove, 230, 235), 60000);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  o = b.onEvent(ev(T::DragMove, 230, 231), 60000);
  TEST_ASSERT_TRUE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
  b.onEvent(ev(T::DragMove, 230, 60), 60000);
  o = b.onEvent(ev(T::DragEnd, 230, 60), 60000);
  TEST_ASSERT_EQUAL(End::Off, o.end);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_FALSE(b.active());
}

// The readout goes to the side away from the knob, with hysteresis: left
// once the knob is right of x 184, right once it is left of 136. (296 s:
// one second a line px, so the knob is where the finger is.)
void test_the_readout_changes_side_with_hysteresis() {
  constexpr uint32_t len = 296000;
  SeekBar b = pressed(137, 0, len);
  b.onEvent(ev(T::DragStart, 150, kY, 0, 13, 0), 0);
  TEST_ASSERT_EQUAL_INT(150, b.knobX());
  TEST_ASSERT_FALSE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 190, kY), 0);
  TEST_ASSERT_TRUE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 150, kY), 0);
  TEST_ASSERT_TRUE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 130, kY), 0);
  TEST_ASSERT_FALSE(b.readoutLeft());
  // At the start: left from x 160.
  SeekBar c = pressed(147, 0, len);
  c.onEvent(ev(T::DragStart, 160, kY, 0, 13, 0), 0);
  TEST_ASSERT_TRUE(c.readoutLeft());
}

// ---- every threshold, on both sides ----

// y: off above 106 (106 still on), back from 114 (113 still off); off on
// the strip from 240 (239 still on), back above 232 (232 still off).
void test_off_and_back_at_their_rows() {
  SeekBar b = pressed(200, 60000);
  b.onEvent(ev(T::DragStart, 214, kY, 0, 14, 0), 60000);
  SeekBar::Out o = b.onEvent(ev(T::DragMove, 214, 106), 60000);
  TEST_ASSERT_FALSE(o.tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
  TEST_ASSERT_TRUE(b.onEvent(ev(T::DragMove, 214, 105), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_FALSE(b.onEvent(ev(T::DragMove, 214, 113), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_TRUE(b.onEvent(ev(T::DragMove, 214, 114), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
  TEST_ASSERT_FALSE(b.onEvent(ev(T::DragMove, 214, 239), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
  TEST_ASSERT_TRUE(b.onEvent(ev(T::DragMove, 214, 240), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_FALSE(b.onEvent(ev(T::DragMove, 214, 232), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Off, b.phase());
  TEST_ASSERT_TRUE(b.onEvent(ev(T::DragMove, 214, 231), 60000).tick);
  TEST_ASSERT_EQUAL(SeekBar::Phase::Scrubbing, b.phase());
}

// The grab: a Down 16 px from the knob (x 84) grabs it, 17 px doesn't.
void test_the_grab_reaches_16_px() {
  const int grab[] = {68, 100}, far[] = {67, 101};
  for (int x : grab) TEST_ASSERT_TRUE(pressed(x, 60000).knobGrab());
  for (int x : far) TEST_ASSERT_FALSE(pressed(x, 60000).knobGrab());
}

// The readout's side at its exact edges (296 s: the knob is where the
// finger is): the knob at 184 keeps it right, 185 sends it left; at 136
// keeps it left, 135 sends it right; at the scrub's start, 159 is right and
// 160 left.
void test_the_readout_sides_at_their_edges() {
  constexpr uint32_t len = 296000;
  SeekBar b = pressed(137, 0, len);
  b.onEvent(ev(T::DragStart, 150, kY, 0, 13, 0), 0);
  b.onEvent(ev(T::DragMove, 184, kY), 0);
  TEST_ASSERT_EQUAL_INT(184, b.knobX());
  TEST_ASSERT_FALSE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 185, kY), 0);
  TEST_ASSERT_TRUE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 136, kY), 0);
  TEST_ASSERT_EQUAL_INT(136, b.knobX());
  TEST_ASSERT_TRUE(b.readoutLeft());
  b.onEvent(ev(T::DragMove, 135, kY), 0);
  TEST_ASSERT_FALSE(b.readoutLeft());
  SeekBar c = pressed(146, 0, len);
  c.onEvent(ev(T::DragStart, 159, kY, 0, 13, 0), 0);
  TEST_ASSERT_EQUAL_INT(159, c.knobX());
  TEST_ASSERT_FALSE(c.readoutLeft());
}

// ---- the readout ----

// What the readout shows changes whenever its side, its second or "no
// change" does: the page draws it again exactly then. Two cases a hash of
// those once gave the same number for, so the row kept the old text:
void test_the_readout_changes_with_what_it_shows() {
  {
    // A 51:00 mix paused at 10:00, a slow drag right: at x 184 the knob is
    // at 183 (29:38, the readout right); 3 px on, at 186 (30:09) and the
    // readout goes left, 31 s on.
    SeekBar b = pressed(100, 600000, 3060000);
    TEST_ASSERT_FALSE(b.knobGrab());
    b.onEvent(ev(T::DragStart, 113, kY, 0, 13, 0), 600000);
    b.onEvent(ev(T::DragMove, 184, kY), 600000);
    const SeekBar::Readout before = b.readout();
    TEST_ASSERT_FALSE(before.left);
    TEST_ASSERT_EQUAL_UINT32(1778, before.targetS);
    b.onEvent(ev(T::DragMove, 187, kY), 600000);
    const SeekBar::Readout after = b.readout();
    TEST_ASSERT_TRUE(after.left);
    TEST_ASSERT_EQUAL_UINT32(1809, after.targetS);
    TEST_ASSERT_TRUE(before != after);
  }
  {
    // 4:05 playing at 1:40: staying ("1:40 no change"), then swept in one
    // frame to 0:07, the readout on the right both times.
    SeekBar b = pressed(60, 100000);
    b.onEvent(ev(T::DragStart, 47, kY, 0, -13, 0), 100000);
    b.onEvent(ev(T::DragMove, 132, kY), 100000);
    const SeekBar::Readout stay = b.readout();
    TEST_ASSERT_TRUE(stay.staying);
    TEST_ASSERT_FALSE(stay.left);
    TEST_ASSERT_EQUAL_UINT32(100, stay.liveS);
    b.onEvent(ev(T::DragMove, 21, kY), 100000);
    const SeekBar::Readout moved = b.readout();
    TEST_ASSERT_FALSE(moved.staying);
    TEST_ASSERT_FALSE(moved.left);
    TEST_ASSERT_EQUAL_UINT32(7, moved.targetS);
    TEST_ASSERT_TRUE(stay != moved);
    // Off: "Release to cancel", whatever else.
    b.onEvent(ev(T::DragMove, 21, 100), 100000);
    TEST_ASSERT_TRUE(b.readout().off);
    TEST_ASSERT_TRUE(b.readout() != moved);
  }
}

// The readout's texts: m:ss (100 min and more: "100:00"), and the change in
// whole seconds with an ASCII minus (no font has U+2212).
void test_the_readout_texts() {
  char t[24];
  SeekBar::timeText(0, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("0:00", t);
  SeekBar::timeText(151999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("2:31", t);
  SeekBar::timeText(6000000, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("100:00", t);
  SeekBar::changeText(151000, 70000, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("+1:21", t);
  SeekBar::changeText(25000, 70999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("-0:45", t);
  SeekBar::changeText(70000, 70999, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING("+0:00", t);
}

// The length is the one at the Down for the whole touch: an estimate that
// settles meanwhile can't move the knob under a still finger.
void test_the_length_is_frozen_for_the_touch() {
  SeekBar b = pressed(200, 60000, kL);
  b.onEvent(ev(T::DragStart, 214, kY, 0, 14, 0), 60000);
  b.live(61000);
  b.onEvent(ev(T::DragMove, 250, kY), 62000);
  TEST_ASSERT_EQUAL_UINT32(kL, b.lengthMs());
  TEST_ASSERT_EQUAL_UINT32(SeekBar::msAt(250, 0, kL), b.targetMs());
  const SeekBar::Out o = b.onEvent(ev(T::DragEnd, 250), 62000);
  TEST_ASSERT_EQUAL_UINT32(196000, o.ms);
  TEST_ASSERT_EQUAL_UINT32(kL, b.lengthMs());
}

// A Cancel (a sheet or dialog took the touch; the screen dimmed under it)
// or a Release: no seek, in any phase.
void test_a_cancel_or_release_seeks_nothing() {
  for (int phase = 0; phase < 3; ++phase) {
    for (int release = 0; release < 2; ++release) {
      SeekBar b = pressed(200, 60000);
      if (phase >= 1) b.onEvent(ev(T::DragStart, 214, kY, 0, 14, 0), 60000);
      if (phase == 2) b.onEvent(ev(T::DragMove, 214, 100), 60000);  // off
      const SeekBar::Out o = b.onEvent(ev(release ? T::Release : T::Cancel, 214), 60000);
      TEST_ASSERT_EQUAL(End::Cancel, o.end);
      TEST_ASSERT_FALSE(o.tick);
      TEST_ASSERT_FALSE(b.active());
    }
  }
  SeekBar b = pressed(200, 60000);
  b.cancel();
  TEST_ASSERT_FALSE(b.active());
  TEST_ASSERT_EQUAL(End::None, b.onEvent(ev(T::Tap, 200), 60000).end);
}

// ---- with the real recogniser (noHold() on the bar's Down, as the page) ----

namespace {

struct Finger {
  TouchRecognizer rec;
  SeekBar bar;
  uint32_t live = 200000;  // the knob at x 248: away from where these land
  std::vector<SeekBar::Out> ends;
  int longPresses = 0, taps = 0, dragEnds = 0, flings = 0;

  void sample(uint32_t ms, bool pressed, int x, int y = kY) {
    TouchRecognizer::Sample s;
    s.pressed = pressed;
    s.x = s.rawX = static_cast<int16_t>(x);
    s.y = s.rawY = static_cast<int16_t>(y);
    InputEvent out[TouchRecognizer::kMaxEvents];
    const int n = rec.update(ms, s, out);
    for (int i = 0; i < n; ++i) {
      const InputEvent& e = out[i];
      if (e.type == T::LongPress) ++longPresses;
      if (e.type == T::Tap) ++taps;
      if (e.type == T::DragEnd) ++dragEnds;
      if (e.type == T::Fling) ++flings;
      if (e.type == T::Down) {
        if (bar.down(e, 7, live, kL)) rec.noHold();
        continue;
      }
      const SeekBar::Out o = bar.onEvent(e, live);
      if (o.end != End::None) ends.push_back(o);
    }
  }
};

}  // namespace

void test_with_the_recognizer() {
  {
    // Resting 700 ms, then sliding: no LongPress, one seek where it stops.
    Finger f;
    uint32_t t = 0;
    for (; t <= 700; t += 20) f.sample(t, true, 100);
    for (int x = 104; x <= 200; x += 4, t += 20) f.sample(t, true, x);
    f.sample(t, false, 200);
    TEST_ASSERT_EQUAL_INT(0, f.longPresses);
    TEST_ASSERT_EQUAL_UINT32(1, f.ends.size());
    TEST_ASSERT_EQUAL(End::Seek, f.ends[0].end);
    TEST_ASSERT_FALSE(f.ends[0].tap);
    TEST_ASSERT_EQUAL_UINT32(SeekBar::msAt(200, 0, kL), f.ends[0].ms);
  }
  {
    // Resting 900 ms, then lifting: a Tap (never a long press), a seek.
    Finger f;
    uint32_t t = 0;
    for (; t <= 900; t += 20) f.sample(t, true, 100);
    f.sample(t, false, 100);
    TEST_ASSERT_EQUAL_INT(0, f.longPresses);
    TEST_ASSERT_EQUAL_INT(1, f.taps);
    TEST_ASSERT_EQUAL_UINT32(1, f.ends.size());
    TEST_ASSERT_EQUAL(End::Seek, f.ends[0].end);
    TEST_ASSERT_TRUE(f.ends[0].tap);
    TEST_ASSERT_EQUAL_UINT32(SeekBar::msAt(100, 0, kL), f.ends[0].ms);
  }
  {
    // A flick: a DragEnd, then a Fling, and one seek.
    Finger f;
    f.live = 60000;
    uint32_t t = 0;
    f.sample(t, true, 50);
    for (int x = 70; x <= 250; x += 20) f.sample(t += 10, true, x);
    f.sample(t += 10, false, 250);
    TEST_ASSERT_EQUAL_INT(1, f.dragEnds);
    TEST_ASSERT_EQUAL_INT(1, f.flings);
    TEST_ASSERT_EQUAL_UINT32(1, f.ends.size());
    TEST_ASSERT_EQUAL(End::Seek, f.ends[0].end);
    TEST_ASSERT_EQUAL_UINT32(SeekBar::msAt(250, 0, kL), f.ends[0].ms);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ms_at_maps_the_line);
  RUN_TEST(test_ms_at_takes_the_clamped_edges);
  RUN_TEST(test_no_seek_lands_in_the_tail);
  RUN_TEST(test_seekable);
  RUN_TEST(test_x_of_is_the_drawing);
  RUN_TEST(test_a_tap_seeks_where_it_landed);
  RUN_TEST(test_a_tap_on_the_knob_seeks_nothing);
  RUN_TEST(test_a_drag_seeks_once_at_the_lift);
  RUN_TEST(test_a_knob_grab_doesnt_jump);
  RUN_TEST(test_a_knob_grab_after_a_rest_doesnt_jump_back);
  RUN_TEST(test_a_vertical_drag_is_let_go);
  RUN_TEST(test_the_detent_snaps_and_ticks);
  RUN_TEST(test_off_and_back);
  RUN_TEST(test_the_readout_changes_side_with_hysteresis);
  RUN_TEST(test_off_and_back_at_their_rows);
  RUN_TEST(test_the_grab_reaches_16_px);
  RUN_TEST(test_the_readout_sides_at_their_edges);
  RUN_TEST(test_the_readout_changes_with_what_it_shows);
  RUN_TEST(test_the_readout_texts);
  RUN_TEST(test_the_length_is_frozen_for_the_touch);
  RUN_TEST(test_a_cancel_or_release_seeks_nothing);
  RUN_TEST(test_with_the_recognizer);
  return UNITY_END();
}
