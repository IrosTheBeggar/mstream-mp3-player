// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the waveform's geometry and inks (lib/core/WaveBar.h,
// docs/SEEK-BAR.md): the strip's rows and the bars' columns at their edges;
// which columns a region overlaps, against a walk over the px; what a playhead's
// move redraws, against the picture before and after; the played rule and the
// reach at their edges; the ink table, every look; when a track shows bars at
// all; the plain line's rows, the text row's and the readout's; what the strip
// shows through a real touch; which pass draws what, the second's move through a
// whole track and what goes as one push; and how often a track's strip is drawn
// whole: bars up under a length that is only an estimate, the bars left up while
// the next track's shape is read, a strip not on the screen yet left blank until
// then, and a track change and a wake pass by pass with the wave record
// (WaveState). No test can see the page: what it draws by is held here.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "SeekBar.h"
#include "TrackSeek.h"
#include "WaveBar.h"
#include "WaveShape.h"
#include "WaveState.h"

using namespace wave;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kL = 245000;  // 4:05: the reach is 3:59

}  // namespace

void test_the_rows_and_columns() {
  // The strip, y 137-157; the bars, y 138-156; the centre row, the strip's row 10.
  TEST_ASSERT_EQUAL_INT(137, kStripY);
  TEST_ASSERT_EQUAL_INT(157, kStripY + kStripH - 1);
  TEST_ASSERT_EQUAL_INT(138, kBarsY);
  TEST_ASSERT_EQUAL_INT(156, kBarsY + kBarsH - 1);
  TEST_ASSERT_EQUAL_INT(147, kCentreY);
  TEST_ASSERT_EQUAL_INT(10, kCentreY - kStripY);
  // 99 bars of 2 px, 1 px apart, ending on the line's last px.
  TEST_ASSERT_EQUAL_INT(99, kColumns);
  TEST_ASSERT_EQUAL_INT(2, kColumnW);
  TEST_ASSERT_EQUAL_INT(12, columnX(0));
  TEST_ASSERT_EQUAL_INT(15, columnX(1));
  TEST_ASSERT_EQUAL_INT(306, columnX(98));
  TEST_ASSERT_EQUAL_INT(307, columnX(98) + kColumnW - 1);
  TEST_ASSERT_EQUAL_INT(SeekBar::kLineX, columnX(0));
  TEST_ASSERT_EQUAL_INT(SeekBar::kLineX + SeekBar::kLineW - 1, columnX(kColumns - 1) + kColumnW - 1);
  TEST_ASSERT_EQUAL_INT(320, kScreenW);
  // A bar of height h: the centre row and h rows either side, in the bars' rows.
  for (int h = 0; h <= kMaxHeight; ++h) {
    TEST_ASSERT_EQUAL_INT(147 - h, barTop(h));
    TEST_ASSERT_EQUAL_INT(2 * h + 1, barRows(h));
    TEST_ASSERT_EQUAL_INT(147 + h, barTop(h) + barRows(h) - 1);
    TEST_ASSERT_TRUE(barTop(h) >= kBarsY);
    TEST_ASSERT_TRUE(barTop(h) + barRows(h) <= kBarsY + kBarsH);
  }
  TEST_ASSERT_EQUAL_INT(147, barTop(0));  // silence: a row of dots, 2 x 1 px
  TEST_ASSERT_EQUAL_INT(1, barRows(0));
  TEST_ASSERT_EQUAL_INT(138, barTop(9));
  TEST_ASSERT_EQUAL_INT(19, barRows(9));
}

void test_the_playhead_and_the_reach() {
  TEST_ASSERT_EQUAL_INT(12, playheadX(0, kL));
  TEST_ASSERT_EQUAL_INT(84, playheadX(60000, kL));  // the old knob at x 84
  TEST_ASSERT_EQUAL_INT(308, playheadX(kL, kL));
  TEST_ASSERT_EQUAL_INT(308, playheadX(kL + 5000, kL));
  TEST_ASSERT_EQUAL_INT(12, playheadX(60000, 0));  // no length
  // At rest, on the whole second: where the second's first ms puts it.
  TEST_ASSERT_EQUAL_INT(85, playheadX(60999, kL));
  TEST_ASSERT_EQUAL_INT(84, restPlayheadX(60000, kL));
  TEST_ASSERT_EQUAL_INT(84, restPlayheadX(60999, kL));
  TEST_ASSERT_EQUAL_INT(85, restPlayheadX(61000, kL));
  TEST_ASSERT_EQUAL_INT(12, restPlayheadX(999, kL));
  TEST_ASSERT_EQUAL_INT(12, restPlayheadX(60999, 0));
  // The reach, as the bar's own: the knob's x for the seek's limit.
  TEST_ASSERT_EQUAL_INT(12 + SeekBar::xOf(239000, kL), reachX(kL));
  TEST_ASSERT_EQUAL_INT(300, reachX(kL));
  TEST_ASSERT_EQUAL_INT(130, reachX(10000));  // the shortest seekable: its limit 4 s, 118 px
  TEST_ASSERT_EQUAL_INT(12, reachX(0));
  TEST_ASSERT_EQUAL_INT(12, reachX(6000));
  for (uint32_t len : {10000u, 60000u, 245000u, 3600000u}) {
    SeekBar b;
    InputEvent e;
    e.type = InputEvent::Type::Down;
    e.x = 100;
    e.y = kCentreY;
    TEST_ASSERT_TRUE(b.down(e, 7, 0, len));
    e.type = InputEvent::Type::DragStart;
    e.dx = 14;
    b.onEvent(e, 0);
    e.type = InputEvent::Type::DragMove;
    e.x = 319;  // the far right: the knob stops at the reach
    b.onEvent(e, 0);
    TEST_ASSERT_EQUAL_INT(b.knobX(), reachX(len));
  }
}

void test_the_columns_a_region_overlaps() {
  struct Case {
    int rx, rw, first, last;
  };
  const Case cases[] = {
      {0, 320, 0, 98},    // the whole strip
      {12, 296, 0, 98},   // the line
      {0, 12, 0, -1},     // left of the first bar
      {11, 1, 0, -1},     //
      {11, 2, 0, 0},      // its first px
      {12, 1, 0, 0},      //
      {13, 1, 0, 0},      // its second
      {14, 1, 0, -1},     // the gap
      {14, 2, 1, 1},      // the gap and the next bar's first px
      {13, 2, 0, 0},      // a bar's second px and the gap
      {13, 3, 0, 1},      // both bars
      {15, 2, 1, 1},      //
      {306, 2, 98, 98},   // the last
      {307, 1, 98, 98},   //
      {308, 1, 0, -1},    // right of it
      {308, 12, 0, -1},   //
      {300, 20, 96, 98},  // to the screen's edge
      {0, 13, 0, 0},      // from it
      {100, 10, 29, 32},  // the size of a move: column 29's second px to column 32's
      {100, 0, 0, -1},    // no width
      {100, -3, 0, -1},   //
      {-20, 33, 0, 0},    // off the screen on the left: columns only from x 0 on anyway
      {-20, 10, 0, -1},   //
      {300, 500, 96, 98}, //
      {400, 10, 0, -1},   // off on the right
  };
  for (const Case& c : cases) {
    const ColumnRange r = columnsIn(c.rx, c.rw);
    if (c.first > c.last) {
      TEST_ASSERT_TRUE_MESSAGE(r.empty(), "none");
    } else {
      TEST_ASSERT_FALSE_MESSAGE(r.empty(), "some");
      TEST_ASSERT_EQUAL_INT(c.first, r.first);
      TEST_ASSERT_EQUAL_INT(c.last, r.last);
    }
  }
  // Every region, against a walk over its px.
  for (int rx = -4; rx <= 324; ++rx) {
    for (int rw = 0; rw <= 340; rw += rw < 12 ? 1 : 13) {
      int first = kColumns, last = -1;
      for (int i = 0; i < kColumns; ++i) {
        const bool in = columnX(i) <= rx + rw - 1 && columnX(i) + kColumnW - 1 >= rx && rw > 0;
        if (in) {
          first = std::min(first, i);
          last = std::max(last, i);
        }
      }
      const ColumnRange r = columnsIn(rx, rw);
      if (last < 0) {
        TEST_ASSERT_TRUE(r.empty());
      } else {
        TEST_ASSERT_FALSE(r.empty());
        TEST_ASSERT_EQUAL_INT(first, r.first);
        TEST_ASSERT_EQUAL_INT(last, r.last);
      }
    }
  }
}

namespace {

// What px x of the strip's centre row shows: the knob over the playhead over a bar.
Ink pictureAt(Look look, int x, int playheadAt, int reach) {
  const Inks in = inksOf(look);
  if (in.knob != Ink::None && x >= playheadAt - 4 && x <= playheadAt + 4) return in.knob;
  if (x == playheadAt - 1 || x == playheadAt) return in.playhead;
  if (x < 12 || x > 307 || (x - 12) % 3 == 2) return Ink::None;
  return barInk(look, (x - 12) / 3, playheadAt, reach);
}

}  // namespace

void test_what_a_moved_playhead_redraws() {
  struct Case {
    int oldX, newX, x, w;
  };
  const Case cases[] = {
      {100, 101, 96, 10},    // a px: 10 x 21 = 210 px of the strip
      {101, 100, 96, 10},    // the other way, the same
      {100, 100, 96, 9},     // no move
      {100, 104, 96, 13},    //
      {12, 13, 8, 10},       // from the start
      {308, 308, 304, 9},    // at the end
      {3, 2, 0, 8},          // clamped on the left
      {0, 0, 0, 5},          //
      {317, 318, 313, 7},    // clamped on the right
      {319, 319, 315, 5},    //
      {12, 308, 8, 305},     // the whole line
      {-10, 400, 0, 320},    // the screen at most
  };
  for (const Case& c : cases) {
    const Region r = movedRegion(c.oldX, c.newX);
    TEST_ASSERT_EQUAL_INT(c.x, r.x);
    TEST_ASSERT_EQUAL_INT(c.w, r.w);
  }
  // A playing second's move stays cheap: a px on a song, 6 px on the 52 s of
  // SEEK-BAR.md's short track, each well under the whole strip's 6,720 px.
  TEST_ASSERT_EQUAL_INT(210, movedRegion(100, 101).w * kStripH);
  TEST_ASSERT_EQUAL_INT(315, movedRegion(100, 106).w * kStripH);
  // No px outside the region differs, in any look, between the two pictures.
  const Look looks[] = {Look::RestPlaying, Look::RestPaused, Look::Pressed, Look::Off};
  for (Look look : looks) {
    for (int from = 12; from <= 308; ++from) {
      for (int d : {-100, -13, -7, -3, -2, -1, 0, 1, 2, 3, 7, 13, 100}) {
        const int to = from + d;
        if (to < 12 || to > 308) continue;
        const Region r = movedRegion(from, to);
        for (int x = 0; x < kScreenW; ++x) {
          if (x >= r.x && x < r.x + r.w) continue;
          TEST_ASSERT_TRUE(pictureAt(look, x, from, 300) == pictureAt(look, x, to, 300));
        }
      }
    }
  }
}

// "A bar is played when its first px is left of the playhead's x."
void test_the_played_rule() {
  const Look looks[] = {Look::RestPlaying, Look::RestPaused, Look::Pressed, Look::Scrubbing, Look::Off};
  for (Look look : looks) {
    for (int i = 0; i < kColumns; ++i) {
      TEST_ASSERT_TRUE(barClass(look, i, columnX(i) + 1, 400) == BarClass::Played);  // the playhead on its second px
      TEST_ASSERT_TRUE(barClass(look, i, columnX(i), 400) == BarClass::Unplayed);    // on its first
      TEST_ASSERT_TRUE(barClass(look, i, columnX(i) - 1, 400) == BarClass::Unplayed);
    }
    // At the very start none is, at the very end all are.
    for (int i = 0; i < kColumns; ++i) {
      TEST_ASSERT_TRUE(barClass(look, i, playheadX(0, kL), 400) == BarClass::Unplayed);
      TEST_ASSERT_TRUE(barClass(look, i, playheadX(kL, kL), 400) == BarClass::Played);
    }
  }
}

// Under Scrubbing, FAINT to the reach, BTN past it: a bar whose first px is left
// of the reach's x can be played by a scrub; the old thick line ended there.
void test_past_the_reach() {
  const int reach = reachX(kL);
  TEST_ASSERT_EQUAL_INT(300, reach);
  TEST_ASSERT_EQUAL_INT(297, columnX(95));
  TEST_ASSERT_EQUAL_INT(300, columnX(96));
  const int at = 100;  // the knob, well left of it
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 95, at, reach) == BarClass::Unplayed);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 96, at, reach) == BarClass::Past);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 98, at, reach) == BarClass::Past);
  // A reach between a bar's two px: that bar is short of it.
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 96, at, 301) == BarClass::Unplayed);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 97, at, 301) == BarClass::Past);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 96, at, 302) == BarClass::Unplayed);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 97, at, 302) == BarClass::Past);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 97, at, 303) == BarClass::Past);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 97, at, 304) == BarClass::Unplayed);
  // The knob at the reach, the furthest a scrub goes: every bar it can play is
  // played, and only those past it are left.
  for (int i = 0; i < kColumns; ++i)
    TEST_ASSERT_TRUE(barClass(Look::Scrubbing, i, reach, reach) == (i <= 95 ? BarClass::Played : BarClass::Past));
  // Where it plays past the reach (the last 6 s), the knob staying on it: played
  // goes first, there is nothing faint between.
  const int live = playheadX(244000, kL);
  TEST_ASSERT_TRUE(live > reach);
  TEST_ASSERT_EQUAL_INT(306, live);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 96, live, reach) == BarClass::Played);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 97, live, reach) == BarClass::Played);
  TEST_ASSERT_TRUE(barClass(Look::Scrubbing, 98, live, reach) == BarClass::Past);
  // No other look has anything past the reach.
  for (Look look : {Look::RestPlaying, Look::RestPaused, Look::Pressed, Look::Off})
    for (int i = 0; i < kColumns; ++i)
      TEST_ASSERT_TRUE(barClass(look, i, at, reach) == (columnX(i) < at ? BarClass::Played : BarClass::Unplayed));
}

// The look's table, from the design, row by row.
void test_the_inks_of_every_look() {
  struct Row {
    Look look;
    Ink played, unplayed, past, playhead, knob, knobDot, marker;
    bool text;
    const char* what;
  };
  const Row rows[] = {
      {Look::RestPlaying, Ink::Accent, Ink::BtnHi, Ink::BtnHi, Ink::Txt, Ink::None, Ink::None, Ink::None, true,
       "rest, playing"},
      {Look::RestPaused, Ink::Dim, Ink::BtnHi, Ink::BtnHi, Ink::Soft, Ink::None, Ink::None, Ink::None, true,
       "rest, not playing"},
      {Look::Pressed, Ink::Accent, Ink::Faint, Ink::Faint, Ink::Txt, Ink::Txt, Ink::Accent, Ink::None, true,
       "pressed"},
      {Look::Scrubbing, Ink::Accent, Ink::Faint, Ink::Btn, Ink::Txt, Ink::Txt, Ink::Accent, Ink::Dim, false,
       "scrubbing"},
      {Look::Off, Ink::Dim, Ink::Btn, Ink::Btn, Ink::Dim, Ink::Dim, Ink::None, Ink::None, false, "off"},
      {Look::Inert, Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, Ink::None, true,
       "inert: the line"},
  };
  for (const Row& r : rows) {
    const Inks in = inksOf(r.look);
    TEST_ASSERT_TRUE_MESSAGE(in.played == r.played, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.unplayed == r.unplayed, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.pastReach == r.past, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.playhead == r.playhead, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.knob == r.knob, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.knobDot == r.knobDot, r.what);
    TEST_ASSERT_TRUE_MESSAGE(in.marker == r.marker, r.what);
    TEST_ASSERT_EQUAL_MESSAGE(r.text, in.text, r.what);
    // barInk() reads the table: a playhead at x 150 with the reach at x 300.
    TEST_ASSERT_TRUE_MESSAGE(barInk(r.look, 0, 150, 300) == r.played, r.what);
    TEST_ASSERT_TRUE_MESSAGE(barInk(r.look, 45, 150, 300) == r.played, r.what);      // x 147
    TEST_ASSERT_TRUE_MESSAGE(barInk(r.look, 46, 150, 300) == r.unplayed, r.what);    // x 150
    TEST_ASSERT_TRUE_MESSAGE(barInk(r.look, 95, 150, 300) == r.unplayed, r.what);    // x 297
    TEST_ASSERT_TRUE_MESSAGE(barInk(r.look, 96, 150, 300) == (r.look == Look::Scrubbing ? r.past : r.unplayed),
                             r.what);  // x 300
  }
}

void test_when_a_track_shows_bars() {
  Shape s;
  s.lengthMs = 180000;
  const Look looks[] = {Look::RestPlaying, Look::RestPaused, Look::Pressed, Look::Scrubbing, Look::Off};
  for (Look look : looks) {
    TEST_ASSERT_TRUE(showsBars(look, &s, 180000));
    TEST_ASSERT_TRUE(showsBars(look, &s, 182000));
    TEST_ASSERT_TRUE(showsBars(look, &s, 178000));
    TEST_ASSERT_TRUE(showsBars(look, &s, 176472));  // 2 % of the player's
    TEST_ASSERT_FALSE(showsBars(look, &s, 176470));
    TEST_ASSERT_FALSE(showsBars(look, &s, 120000));  // another recording
    TEST_ASSERT_FALSE(showsBars(look, nullptr, 180000));
  }
  // Inert never does. A length the slot doesn't know agrees with any.
  TEST_ASSERT_FALSE(showsBars(Look::Inert, &s, 180000));
  s.lengthMs = 0;
  TEST_ASSERT_FALSE(showsBars(Look::Inert, &s, 180000));
  TEST_ASSERT_TRUE(showsBars(Look::Pressed, &s, 180000));
  TEST_ASSERT_TRUE(showsBars(Look::Off, &s, 1));
}

// The plain line keeps the rows it had about its knob, on the bars' centre row.
void test_the_plain_lines_rows() {
  const int c = kCentreRow;
  TEST_ASSERT_EQUAL_INT(10, c);
  // Thin: y 145-148, at rest and inert.
  TEST_ASSERT_EQUAL_INT(c - 2, kThinRow);
  TEST_ASSERT_EQUAL_INT(c + 1, kThinRow + kThinH - 1);
  TEST_ASSERT_EQUAL_INT(145, kStripY + kThinRow);
  TEST_ASSERT_EQUAL_INT(148, kStripY + kThinRow + kThinH - 1);
  TEST_ASSERT_EQUAL_INT(2, kThinR);
  // Thick: y 144-149, under a finger.
  TEST_ASSERT_EQUAL_INT(c - 3, kThickRow);
  TEST_ASSERT_EQUAL_INT(c + 2, kThickRow + kThickH - 1);
  TEST_ASSERT_EQUAL_INT(144, kStripY + kThickRow);
  TEST_ASSERT_EQUAL_INT(149, kStripY + kThickRow + kThickH - 1);
  TEST_ASSERT_EQUAL_INT(3, kThickR);
  // Dots: y 146-147, 3 x 2 every 6 px.
  TEST_ASSERT_EQUAL_INT(c - 1, kDotsRow);
  TEST_ASSERT_EQUAL_INT(c, kDotsRow + kDotsH - 1);
  TEST_ASSERT_EQUAL_INT(3, kDotW);
  TEST_ASSERT_EQUAL_INT(6, kDotStep);
  // The marker: y 143-151, in the strip.
  TEST_ASSERT_EQUAL_INT(c - 4, kLineMarkerRow);
  TEST_ASSERT_EQUAL_INT(c + 4, kLineMarkerRow + kLineMarkerH - 1);
  // The knobs, y 144-150 at rest, y 143-151 under a finger: in the strip, with
  // room to spare, and the moved region's margin takes the wider.
  TEST_ASSERT_EQUAL_INT(3, kRestKnobR);
  TEST_ASSERT_EQUAL_INT(4, kKnobR);
  TEST_ASSERT_EQUAL_INT(2, kKnobDotR);
  TEST_ASSERT_TRUE(c - kKnobR >= 0 && c + kKnobR < kStripH);
  TEST_ASSERT_TRUE(kMoveMargin >= kKnobR && kMoveMargin >= kRestKnobR);
  // Nothing moves between a track with a shape and one without: the line is on
  // the bars' rows, x 12-307.
  TEST_ASSERT_TRUE(kStripY + kThickRow >= kBarsY && kStripY + kThickRow + kThickH <= kBarsY + kBarsH);
  TEST_ASSERT_EQUAL_INT(SeekBar::kLineX, columnX(0));
}

// The rows around the strip (the page's static_asserts tie them to its own).
void test_the_rows_around_the_strip() {
  TEST_ASSERT_EQUAL_INT(158, kTextRowY);
  TEST_ASSERT_EQUAL_INT(173, kTextRowY + kTextRowH - 1);
  TEST_ASSERT_EQUAL_INT(165, kTextCentreY);
  TEST_ASSERT_EQUAL_INT(110, kReadoutY);
  TEST_ASSERT_EQUAL_INT(27, kReadoutH);
  TEST_ASSERT_EQUAL_INT(136, kReadoutY + kReadoutH - 1);
  TEST_ASSERT_EQUAL_INT(kStripY, kReadoutY + kReadoutH);
  TEST_ASSERT_EQUAL_INT(13, kReadoutBigRow);
  TEST_ASSERT_EQUAL_INT(16, kReadoutSmallRow);
  // A strip's push: 6,720 px; the text row's 5,120, less than the 7,040 of the
  // band they replace; a readout's 8,640, was 10,560.
  TEST_ASSERT_EQUAL_INT(6720, kScreenW * kStripH);
  TEST_ASSERT_EQUAL_INT(5120, kScreenW * kTextRowH);
  TEST_ASSERT_EQUAL_INT(8640, kScreenW * kReadoutH);
}

void test_the_look_of_a_touch() {
  using P = SeekBar::Phase;
  for (bool seekable : {false, true}) {
    for (bool playing : {false, true}) {
      TEST_ASSERT_TRUE(lookOf(P::Pressed, seekable, playing) == Look::Pressed);
      TEST_ASSERT_TRUE(lookOf(P::Scrubbing, seekable, playing) == Look::Scrubbing);
      TEST_ASSERT_TRUE(lookOf(P::Off, seekable, playing) == Look::Off);
    }
  }
  TEST_ASSERT_TRUE(lookOf(P::Idle, false, false) == Look::Inert);
  TEST_ASSERT_TRUE(lookOf(P::Idle, false, true) == Look::Inert);
  TEST_ASSERT_TRUE(lookOf(P::Idle, true, false) == Look::RestPaused);
  TEST_ASSERT_TRUE(lookOf(P::Idle, true, true) == Look::RestPlaying);
}

void test_what_the_strip_has() {
  Shape s;
  s.lengthMs = 180000;
  TEST_ASSERT_TRUE(fillOf(Look::RestPlaying, &s, 180000) == Fill::Bars);
  TEST_ASSERT_TRUE(fillOf(Look::RestPlaying, nullptr, 180000) == Fill::Line);
  TEST_ASSERT_TRUE(fillOf(Look::RestPlaying, &s, 120000) == Fill::Line);  // another recording
  TEST_ASSERT_TRUE(fillOf(Look::Inert, &s, 180000) == Fill::Line);        // never bars
  TEST_ASSERT_TRUE(fillOf(Look::Inert, &s, 0) == Fill::Dots);
  TEST_ASSERT_TRUE(fillOf(Look::Inert, nullptr, 0) == Fill::Dots);
  TEST_ASSERT_TRUE(fillOf(Look::Inert, nullptr, 5000) == Fill::Line);  // too short to seek: its line, no knob
}

namespace {

InputEvent ev(InputEvent::Type type, int x, int y = kCentreY, int dx = 0) {
  InputEvent e;
  e.type = type;
  e.x = x;
  e.y = y;
  e.dx = dx;
  return e;
}

}  // namespace

// What the strip shows, through a real touch: the page asks stripOf() each pass.
void test_the_strip_through_a_touch() {
  Shape shape;
  shape.lengthMs = kL;
  using T = InputEvent::Type;
  SeekBar b;
  // At rest: the player's. A px is 245000 / 296 = 828 ms.
  Strip s = stripOf(b, true, true, 60000, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::RestPlaying);
  TEST_ASSERT_TRUE(s.fill == Fill::Bars);
  TEST_ASSERT_TRUE(s.playing);
  TEST_ASSERT_EQUAL_UINT32(7, s.track);
  TEST_ASSERT_EQUAL_UINT32(42, s.version);
  TEST_ASSERT_EQUAL_INT(84, s.playheadX);
  TEST_ASSERT_EQUAL_INT(-1, s.markerX);
  // Paused: a look of its own.
  s = stripOf(b, true, false, 60000, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::RestPaused);
  TEST_ASSERT_FALSE(s.playing);
  // A version counts only while the bars show: a Miss draws nothing.
  s = stripOf(b, true, true, 60000, kL, nullptr, 7, 42);
  TEST_ASSERT_TRUE(s.fill == Fill::Line);
  TEST_ASSERT_EQUAL_UINT32(0, s.version);
  s = stripOf(b, false, true, 60000, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::Inert);
  TEST_ASSERT_TRUE(s.fill == Fill::Line);
  TEST_ASSERT_EQUAL_UINT32(0, s.version);
  s = stripOf(b, false, true, 0, 0, nullptr, 7, 42);
  TEST_ASSERT_TRUE(s.fill == Fill::Dots);
  TEST_ASSERT_EQUAL_INT(12, s.playheadX);

  // A Down: the knob at where it plays, the marker still none.
  TEST_ASSERT_TRUE(b.down(ev(T::Down, 200), 1, 60000, kL));
  s = stripOf(b, true, false, 60000, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::Pressed);
  TEST_ASSERT_TRUE(s.fill == Fill::Bars);
  TEST_ASSERT_EQUAL_INT(84, s.playheadX);
  TEST_ASSERT_EQUAL_INT(-1, s.markerX);
  // The player's length moves under the finger (an estimate settling): the bar's
  // stays what it was at the Down, for the agreement too.
  s = stripOf(b, true, false, 60000, 100000, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.fill == Fill::Bars);
  TEST_ASSERT_EQUAL_INT(84, s.playheadX);
  // The finger scrubs: the knob at its second, the marker where it plays.
  b.onEvent(ev(T::DragStart, 214, kCentreY, 14), 60000);
  s = stripOf(b, true, false, 60000, 100000, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::Scrubbing);
  TEST_ASSERT_TRUE(s.fill == Fill::Bars);
  TEST_ASSERT_EQUAL_INT(b.knobX(), s.playheadX);
  TEST_ASSERT_TRUE(s.playheadX > 150);
  TEST_ASSERT_EQUAL_INT(84, s.markerX);
  // Off: both where it plays.
  b.onEvent(ev(T::DragMove, 214, SeekBar::kOffAboveY - 1), 60000);
  s = stripOf(b, true, false, 60000, 100000, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::Off);
  TEST_ASSERT_EQUAL_INT(84, s.playheadX);
  TEST_ASSERT_EQUAL_INT(84, s.markerX);
  // The touch ended: the player's again.
  b.cancel();
  s = stripOf(b, true, false, 30000, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::RestPaused);
  TEST_ASSERT_EQUAL_INT(48, s.playheadX);
  TEST_ASSERT_EQUAL_INT(-1, s.markerX);

  // At rest, played or paused, the playhead is on the whole second; under a finger
  // the knob and the marker are on the live x.
  TEST_ASSERT_EQUAL_INT(84, stripOf(b, true, true, 60999, kL, &shape, 7, 42).playheadX);
  TEST_ASSERT_EQUAL_INT(84, stripOf(b, true, false, 60999, kL, &shape, 7, 42).playheadX);
  TEST_ASSERT_EQUAL_INT(84, stripOf(b, false, true, 60999, kL, nullptr, 7, 42).playheadX);
  TEST_ASSERT_TRUE(b.down(ev(T::Down, 200), 1, 60999, kL));
  TEST_ASSERT_EQUAL_INT(85, stripOf(b, true, true, 60999, kL, &shape, 7, 42).playheadX);
  b.onEvent(ev(T::DragStart, 214, kCentreY, 14), 60999);
  b.onEvent(ev(T::DragMove, 214, SeekBar::kOffAboveY - 1), 60999);
  s = stripOf(b, true, true, 60999, kL, &shape, 7, 42);
  TEST_ASSERT_TRUE(s.look == Look::Off);
  TEST_ASSERT_EQUAL_INT(85, s.playheadX);
  TEST_ASSERT_EQUAL_INT(85, s.markerX);
  b.cancel();
}

namespace {

Strip restStrip(int x) {
  Strip s;
  s.look = Look::RestPlaying;
  s.fill = Fill::Bars;
  s.playing = true;
  s.track = 7;
  s.version = 42;
  s.playheadX = x;
  return s;
}

void assertRegion(int x, int w, const Region& r) {
  TEST_ASSERT_EQUAL_INT(x, r.x);
  TEST_ASSERT_EQUAL_INT(w, r.w);
}

}  // namespace

// Which pass draws what of the strip (docs/SEEK-BAR.md 4.5).
void test_what_a_pass_redraws_of_the_strip() {
  const Strip drawn = restStrip(100);
  // Nothing moved: nothing. (A second that moves no px is the text row's alone.)
  assertRegion(0, 0, regionToDraw(drawn, restStrip(100)));
  // The playhead: a part, its old and new places and what's between.
  assertRegion(96, 10, regionToDraw(drawn, restStrip(101)));
  assertRegion(95, 10, regionToDraw(drawn, restStrip(99)));
  assertRegion(96, 109, regionToDraw(drawn, restStrip(200)));  // a seek
  // Each of the others: the whole strip.
  const auto whole = [&](Strip now) { assertRegion(0, kScreenW, regionToDraw(drawn, now)); };
  Strip s = restStrip(100);
  s.look = Look::Pressed;  // a look
  whole(s);
  s = restStrip(100);
  s.look = Look::RestPaused;  // a play state, at rest
  s.playing = false;
  whole(s);
  s = restStrip(100);
  s.playing = false;  // a play state, the plain line inert
  whole(s);
  s = restStrip(100);
  s.fill = Fill::Line;  // the bars gone (a length that stopped agreeing, a file gone)
  s.version = 0;
  whole(s);
  s = restStrip(100);
  s.track = 8;  // a track
  whole(s);
  s = restStrip(100);
  s.version = 43;  // a new shape
  whole(s);
  s = restStrip(100);
  s.markerX = 50;  // a scrub's marker
  whole(s);
  // A repaint: Drawn{} has no playhead, so anything is new to it.
  assertRegion(0, kScreenW, regionToDraw(Strip{}, restStrip(100)));
  assertRegion(0, kScreenW, regionToDraw(Strip{}, Strip{}));
  // The bars arriving over the line, a look away from it, the dots gone.
  Strip line = restStrip(100);
  line.fill = Fill::Line;
  line.version = 0;
  assertRegion(0, kScreenW, regionToDraw(line, restStrip(100)));
  Strip dots = line;
  dots.fill = Fill::Dots;
  assertRegion(0, kScreenW, regionToDraw(dots, line));
  // Under the plain line a read that came to nothing moves no version (stripOf()):
  // the same as nothing.
  assertRegion(0, 0, regionToDraw(line, line));
}

// A length the player only estimates can put the bars up, not take them down
// (showsBars()): in a track's first seconds it moves in and out of the agreement
// with every decoder pass, and each crossing was the strip drawn whole.
void test_bars_up_stay_up_under_an_estimate() {
  using T = InputEvent::Type;
  Shape shape;
  shape.lengthMs = kL;
  SeekBar b;
  const auto fill = [&](uint32_t lengthMs, bool known, const Strip& drawn, const Shape* sh = nullptr,
                        uint32_t track = 7, uint32_t version = 42, bool seekable = true) {
    return stripOf(b, seekable, true, 2000, lengthMs, sh ? sh : &shape, track, version, known, drawn).fill;
  };
  const uint32_t kOff = 200000;  // 45 s short: no agreement (2 % of it is 4 s)
  TEST_ASSERT_FALSE(lengthAgrees(kL, kOff));
  const Strip up = stripOf(b, true, true, 2000, kL, &shape, 7, 42, false);
  TEST_ASSERT_TRUE(up.fill == Fill::Bars);
  TEST_ASSERT_TRUE(barsUp(up, 7, 42));
  // The file says its length: the agreement decides at every pass, bars up or
  // not (a file replaced under its path shows no other recording's shape).
  TEST_ASSERT_TRUE(fill(kOff, true, up) == Fill::Line);
  TEST_ASSERT_TRUE(fill(kL, true, up) == Fill::Bars);
  // An estimate: the bars up stay, wherever it wanders.
  for (uint32_t ms : {kOff, 150000u, 239000u, kL, 400000u, 10000u}) {
    const Strip now = stripOf(b, true, true, 2000, ms, &shape, 7, 42, false, up);
    TEST_ASSERT_TRUE(now.fill == Fill::Bars);
    TEST_ASSERT_EQUAL_UINT32(42, now.version);
    // ... so nothing of the strip is drawn again for it but a playhead's move.
    TEST_ASSERT_TRUE(regionToDraw(up, now).w < kScreenW);
  }
  // It can't keep up what isn't: the line stays the line until it agrees, once.
  const Strip line = stripOf(b, true, true, 2000, kOff, &shape, 7, 42, false);
  TEST_ASSERT_TRUE(line.fill == Fill::Line);
  TEST_ASSERT_FALSE(barsUp(line, 7, 42));
  TEST_ASSERT_TRUE(fill(kOff, false, line) == Fill::Line);
  TEST_ASSERT_TRUE(fill(kL - 1000, false, line) == Fill::Bars);
  // Nothing drawn yet (a repaint): as the line.
  TEST_ASSERT_TRUE(fill(kOff, false, Strip{}) == Fill::Line);
  // Only this track's bars, from this shape: another track's, or a shape read
  // since (WaveState::version()), start over.
  TEST_ASSERT_TRUE(fill(kOff, false, up, nullptr, 8, 42) == Fill::Line);
  TEST_ASSERT_TRUE(fill(kOff, false, up, nullptr, 7, 43) == Fill::Line);
  // Still no bars with no shape, nor under the inert look.
  TEST_ASSERT_TRUE(stripOf(b, true, true, 2000, kOff, nullptr, 7, 42, false, up).fill == Fill::Line);
  TEST_ASSERT_TRUE(fill(kOff, false, up, nullptr, 7, 42, false) == Fill::Line);
  // Under a finger the bar's own length, frozen at the Down, is judged the same.
  TEST_ASSERT_TRUE(b.down(ev(T::Down, 200), 1, 60000, kOff));
  TEST_ASSERT_TRUE(stripOf(b, true, true, 60000, kOff, &shape, 7, 42, false, up).fill == Fill::Bars);
  TEST_ASSERT_TRUE(stripOf(b, true, true, 60000, kOff, &shape, 7, 42, true, up).fill == Fill::Line);
  b.cancel();
  // The rule alone.
  TEST_ASSERT_TRUE(showsBars(Look::RestPlaying, &shape, kOff, false, true));
  TEST_ASSERT_FALSE(showsBars(Look::RestPlaying, &shape, kOff, false, false));
  TEST_ASSERT_FALSE(showsBars(Look::RestPlaying, &shape, kOff, true, true));
  TEST_ASSERT_FALSE(showsBars(Look::Inert, &shape, kOff, false, true));
  TEST_ASSERT_FALSE(showsBars(Look::RestPlaying, nullptr, kOff, false, true));
}

// At a track change the bars stay as drawn while the new shape is read
// (heldForShape()): one whole strip for the change, not the line and then the bars.
void test_the_bars_stay_while_a_shape_is_read() {
  const Strip bars = restStrip(100);  // track 7's, playing
  Strip line = restStrip(12);         // track 8 at its start, no shape yet
  line.track = 8;
  line.fill = Fill::Line;
  line.version = 0;
  // Coming: nothing of the strip is drawn.
  TEST_ASSERT_TRUE(heldForShape(bars, line, true));
  assertRegion(0, 0, regionToDraw(bars, line, true));
  // Not coming (answered with none, a failed read, the wait over): the line.
  TEST_ASSERT_FALSE(heldForShape(bars, line, false));
  assertRegion(0, kScreenW, regionToDraw(bars, line, false));
  assertRegion(0, kScreenW, regionToDraw(bars, line));
  // The shape landed: the new bars, whole, whatever the wait says.
  Strip next = restStrip(12);
  next.track = 8;
  next.version = 43;
  assertRegion(0, kScreenW, regionToDraw(bars, next, true));
  assertRegion(0, kScreenW, regionToDraw(bars, next, false));
  // The new track with no length yet (the dots, inert), or paused: held the same.
  Strip dots = line;
  dots.look = Look::Inert;
  dots.fill = Fill::Dots;
  assertRegion(0, 0, regionToDraw(bars, dots, true));
  Strip paused = line;
  paused.look = Look::RestPaused;
  paused.playing = false;
  assertRegion(0, 0, regionToDraw(bars, paused, true));
  Strip pausedBars = bars;
  pausedBars.look = Look::RestPaused;
  pausedBars.playing = false;
  assertRegion(0, 0, regionToDraw(pausedBars, line, true));
  // Only bars are held: a line is drawn again at once (its fill back at the start).
  Strip oldLine = restStrip(100);
  oldLine.fill = Fill::Line;
  oldLine.version = 0;
  assertRegion(0, kScreenW, regionToDraw(oldLine, line, true));
  // Never under a finger, nor over a look a finger left.
  for (Look look : {Look::Pressed, Look::Scrubbing, Look::Off}) {
    Strip touched = line;
    touched.look = look;
    assertRegion(0, kScreenW, regionToDraw(bars, touched, true));
    Strip left = bars;
    left.look = look;
    assertRegion(0, kScreenW, regionToDraw(left, line, true));
  }
  // A repaint (nothing drawn) draws what there is.
  assertRegion(0, kScreenW, regionToDraw(Strip{}, line, true));
  // Bars up with nothing changed, or their playhead moved: as without the wait.
  assertRegion(0, 0, regionToDraw(bars, bars, true));
  assertRegion(96, 10, regionToDraw(bars, restStrip(101), true));
}

namespace {

// What the page's update() does with the strip (NowPlayingPage.cpp), without the
// drawing: the strip now, the part to draw, and the strip kept when any of it
// was drawn. It counts the strips drawn whole.
struct PageModel {
  Strip drawn;
  int whole = 0;
  int lines = 0;  // ... and those of them that were the plain line (or its dots)
  Fill pass(const WaveState& w, uint32_t nowMs, uint32_t track, uint32_t posMs, uint32_t lengthMs, bool known) {
    SeekBar bar;
    const bool coming = w.coming(nowMs);
    const Strip now = stripOf(bar, true, true, posMs, lengthMs, w.shape(), track, w.version(), known, drawn, coming);
    const Region r = regionToDraw(drawn, now, coming);
    if (r.w == kScreenW) ++whole;
    if (r.w == kScreenW && (now.fill == Fill::Line || now.fill == Fill::Dots)) ++lines;
    if (r.w > 0) drawn = now;
    return drawn.fill;
  }
};

ReadResult hitOf(uint32_t lengthMs) {
  ReadResult r;
  r.read = Read::Hit;
  r.jobMs = 20;
  r.shape.lengthMs = lengthMs;
  for (uint8_t& h : r.shape.heights) h = 5;
  return r;
}

constexpr uint64_t kHashA = 0xA1, kHashB = 0xB2;

// Track 7 playing with its bars up, at `nowMs`.
void barsUpFor7(WaveState* w, PageModel* page, uint32_t nowMs) {
  w->begin(true);
  w->want(kHashA, nowMs);
  w->started();
  w->finished(hitOf(kL), nowMs);
  TEST_ASSERT_TRUE(page->pass(*w, nowMs, 7, 240000, kL, true) == Fill::Bars);
  page->whole = 0;
}

}  // namespace

// A track change, pass by pass, with the wave record: the strip is drawn whole
// once, when the new shape lands. (Before the bars were left up: twice, the line
// first.) Then a length that is only an estimate, wandering through the track's
// first seconds, draws no strip whole at all.
void test_a_track_change_draws_the_strip_once() {
  WaveState w;
  PageModel page;
  barsUpFor7(&w, &page, 10000);
  // Track 8: wanted in the pass that sees it; its read takes four passes.
  uint32_t t = 20000;
  w.want(kHashB, t);
  TEST_ASSERT_NULL(w.shape());
  for (int i = 0; i < 4; ++i, t += 20) {
    TEST_ASSERT_TRUE(page.pass(w, t, 8, 0, 200000, true) == Fill::Bars);  // (track 7's, as drawn)
    if (i == 0) w.started();
  }
  TEST_ASSERT_EQUAL_INT(0, page.whole);
  TEST_ASSERT_EQUAL(WaveState::Finish::Recorded, w.finished(hitOf(200000), t));
  TEST_ASSERT_TRUE(page.pass(w, t, 8, 0, 200000, true) == Fill::Bars);
  TEST_ASSERT_EQUAL_INT(1, page.whole);
  TEST_ASSERT_EQUAL_UINT32(8, page.drawn.track);
  // Its length is an estimate from its second second on (a file with none in its
  // header): 9 % short, closing in, the next pass's a little off the last's.
  for (uint32_t ms = 1000; ms < 12000; ms += 20) {
    const uint32_t off = 200000u * 90 / ms;                 // 18 s at 1 s, 1.5 s at 12 s
    const uint32_t est = 200000u - (ms / 20 % 2 ? off : 0);  // every other pass
    TEST_ASSERT_TRUE(page.pass(w, t + ms, 8, ms, est, false) == Fill::Bars);
  }
  TEST_ASSERT_EQUAL_INT(1, page.whole);
  // (The same passes with the bars taken down at each: the rule before.)
  int flips = 0;
  bool was = true;
  for (uint32_t ms = 1000; ms < 12000; ms += 20) {
    const uint32_t off = 200000u * 90 / ms;
    const bool agrees = lengthAgrees(200000, 200000u - (ms / 20 % 2 ? off : 0));
    flips += agrees != was;
    was = agrees;
  }
  TEST_ASSERT_TRUE(flips > 100);
}

// The read doesn't come in time, or comes to nothing: the line, once.
void test_a_track_change_with_no_shape_in_time() {
  // A track with no slot: the line at the pass after the answer.
  {
    WaveState w;
    PageModel page;
    barsUpFor7(&w, &page, 10000);
    w.want(kHashB, 20000);
    TEST_ASSERT_TRUE(page.pass(w, 20000, 8, 0, 200000, true) == Fill::Bars);
    w.started();
    ReadResult miss;
    miss.read = Read::Miss;
    TEST_ASSERT_EQUAL(WaveState::Finish::None, w.finished(miss, 20040));
    TEST_ASSERT_TRUE(page.pass(w, 20040, 8, 0, 200000, true) == Fill::Line);
    TEST_ASSERT_EQUAL_INT(1, page.whole);
  }
  // The worker busy with a step of its own: the line when the wait is over, the
  // bars when the read lands after all.
  {
    WaveState w;
    PageModel page;
    barsUpFor7(&w, &page, 10000);
    w.want(kHashB, 20000);
    uint32_t t = 20000;
    for (; t < 20000 + WaveState::kComingMs; t += 20) {
      TEST_ASSERT_TRUE(page.pass(w, t, 8, 0, 200000, true) == Fill::Bars);
    }
    TEST_ASSERT_EQUAL_INT(0, page.whole);
    TEST_ASSERT_TRUE(page.pass(w, t, 8, 0, 200000, true) == Fill::Line);
    TEST_ASSERT_EQUAL_INT(1, page.whole);
    w.started();
    w.finished(hitOf(200000), t + 3000);
    TEST_ASSERT_TRUE(page.pass(w, t + 3000, 8, 3000, 200000, true) == Fill::Bars);
    TEST_ASSERT_EQUAL_INT(2, page.whole);
  }
  // A repaint in the wait (a sheet closed over it) has none of the old bars
  // left to keep: the strip's background alone, then the bars. No line.
  {
    WaveState w;
    PageModel page;
    barsUpFor7(&w, &page, 10000);
    w.want(kHashB, 20000);
    TEST_ASSERT_TRUE(page.pass(w, 20000, 8, 0, 200000, true) == Fill::Bars);
    page.drawn = Strip{};
    TEST_ASSERT_TRUE(page.pass(w, 20020, 8, 0, 200000, true) == Fill::Blank);
    w.started();
    w.finished(hitOf(200000), 20060);
    TEST_ASSERT_TRUE(page.pass(w, 20060, 8, 0, 200000, true) == Fill::Bars);
    TEST_ASSERT_EQUAL_INT(0, page.lines);
  }
}

// A strip with nothing on the screen yet (a repaint: the page entered, the
// screen woken) while the track's shape is on its way: its background alone,
// then the bars. Drawn as it stood it was the line, then the bars over it.
void test_a_strip_not_on_the_screen_waits_blank() {
  Shape shape;
  shape.lengthMs = kL;
  SeekBar b;
  const Strip none;
  // At rest, with the shape coming and none to show: blank. Each rest look.
  for (bool playing : {true, false}) {
    const Strip s = stripOf(b, true, playing, 5000, kL, nullptr, 7, 42, true, none, true);
    TEST_ASSERT_TRUE(s.fill == Fill::Blank);
    TEST_ASSERT_EQUAL_UINT32(0, s.version);
    TEST_ASSERT_TRUE(s.look == (playing ? Look::RestPlaying : Look::RestPaused));
  }
  TEST_ASSERT_TRUE(stripOf(b, false, true, 0, 0, nullptr, 7, 42, true, none, true).fill == Fill::Blank);  // (inert)
  // Not coming: what there is, as ever.
  TEST_ASSERT_TRUE(stripOf(b, true, true, 5000, kL, nullptr, 7, 42, true, none, false).fill == Fill::Line);
  TEST_ASSERT_TRUE(stripOf(b, false, true, 0, 0, nullptr, 7, 42, true, none, false).fill == Fill::Dots);
  // The shape here: the bars, coming or not.
  TEST_ASSERT_TRUE(stripOf(b, true, true, 5000, kL, &shape, 7, 42, true, none, true).fill == Fill::Bars);
  // Blank stays blank while it comes, whatever else moves: nothing is drawn.
  const Strip blank = stripOf(b, true, true, 5000, kL, nullptr, 7, 42, true, none, true);
  Strip later = stripOf(b, true, false, 9000, kL, nullptr, 8, 43, true, blank, true);
  TEST_ASSERT_TRUE(later.fill == Fill::Blank);
  assertRegion(0, 0, regionToDraw(blank, later, true));
  assertRegion(0, 0, regionToDraw(blank, later, false));
  // The wait over with none: the line, whole. The shape landed: the bars, whole.
  later = stripOf(b, true, true, 5000, kL, nullptr, 7, 42, true, blank, false);
  TEST_ASSERT_TRUE(later.fill == Fill::Line);
  assertRegion(0, kScreenW, regionToDraw(blank, later, false));
  later = stripOf(b, true, true, 5000, kL, &shape, 7, 43, true, blank, false);
  TEST_ASSERT_TRUE(later.fill == Fill::Bars);
  assertRegion(0, kScreenW, regionToDraw(blank, later, false));
  // Only a strip not on the screen: a line drawn stays a line through a track
  // change (no blank between two lines), and bars are held (heldForShape()).
  Strip line = restStrip(100);
  line.fill = Fill::Line;
  line.version = 0;
  TEST_ASSERT_TRUE(stripOf(b, true, true, 0, kL, nullptr, 8, 42, true, line, true).fill == Fill::Line);
  TEST_ASSERT_TRUE(stripOf(b, true, true, 0, kL, nullptr, 8, 42, true, restStrip(100), true).fill == Fill::Line);
  // Never under a finger: the bar pressed shows its line at once.
  using T = InputEvent::Type;
  TEST_ASSERT_TRUE(b.down(ev(T::Down, 200), 1, 5000, kL));
  const Strip pressed = stripOf(b, true, true, 5000, kL, nullptr, 7, 42, true, blank, true);
  TEST_ASSERT_TRUE(pressed.look == Look::Pressed);
  TEST_ASSERT_TRUE(pressed.fill == Fill::Line);
  assertRegion(0, kScreenW, regionToDraw(blank, pressed, true));
  b.cancel();
}

// A wake onto a track that changed in the dark, pass by pass: the strip's
// background, then the bars. And the cases around it.
void test_a_wake_draws_no_line_before_the_bars() {
  // The read takes three passes.
  {
    WaveState w;
    PageModel page;
    w.begin(true);
    uint32_t t = 30000;
    w.want(kHashB, t);
    TEST_ASSERT_TRUE(page.pass(w, t, 8, 61000, 200000, true) == Fill::Blank);
    w.started();
    for (int i = 0; i < 3; ++i) {
      t += 20;
      TEST_ASSERT_TRUE(page.pass(w, t, 8, 61000 + 20 * (i + 1), 200000, true) == Fill::Blank);
    }
    TEST_ASSERT_EQUAL_INT(1, page.whole);  // (the background, with the text row's push)
    w.finished(hitOf(200000), t);
    TEST_ASSERT_TRUE(page.pass(w, t, 8, 61080, 200000, true) == Fill::Bars);
    TEST_ASSERT_EQUAL_INT(2, page.whole);
    TEST_ASSERT_EQUAL_INT(0, page.lines);
  }
  // The track that was playing when the screen went off: its record is kept,
  // the bars at once.
  {
    WaveState w;
    PageModel page;
    barsUpFor7(&w, &page, 10000);
    page.drawn = Strip{};
    TEST_ASSERT_TRUE(page.pass(w, 30000, 7, 250000, kL, true) == Fill::Bars);
    TEST_ASSERT_EQUAL_INT(1, page.whole);
  }
  // A card with no waveform file: nothing is coming, the line at once.
  {
    WaveState w;
    PageModel page;
    w.begin(false);
    w.want(kHashB, 30000);
    TEST_ASSERT_TRUE(page.pass(w, 30000, 8, 0, 200000, true) == Fill::Line);
    TEST_ASSERT_EQUAL_INT(1, page.whole);
  }
  // A track with no slot: blank until the read says so, then the line.
  {
    WaveState w;
    PageModel page;
    w.begin(true);
    w.want(kHashB, 30000);
    TEST_ASSERT_TRUE(page.pass(w, 30000, 8, 0, 200000, true) == Fill::Blank);
    w.started();
    ReadResult miss;
    miss.read = Read::Miss;
    w.finished(miss, 30040);
    TEST_ASSERT_TRUE(page.pass(w, 30040, 8, 0, 200000, true) == Fill::Line);
    TEST_ASSERT_EQUAL_INT(1, page.lines);
  }
  // No read in time: the line when the wait is over, half a second in.
  {
    WaveState w;
    PageModel page;
    w.begin(true);
    w.want(kHashB, 30000);
    uint32_t t = 30000;
    for (; t < 30000 + WaveState::kComingMs; t += 20) {
      TEST_ASSERT_TRUE(page.pass(w, t, 8, t - 30000, 200000, true) == Fill::Blank);
    }
    TEST_ASSERT_TRUE(page.pass(w, t, 8, t - 30000, 200000, true) == Fill::Line);
  }
}

// A second that changes during a scrub draws nothing there; the blank goes in
// once; leaving draws it; a repaint under any look starts it over.
void test_what_a_pass_does_to_the_text_row() {
  using R = TextRow;
  using D = TextDraw;
  for (Look look : {Look::Inert, Look::RestPlaying, Look::RestPaused, Look::Pressed}) {
    TEST_ASSERT_TRUE(textDraw(look, R::Shown, false) == D::None);
    TEST_ASSERT_TRUE(textDraw(look, R::Shown, true) == D::Draw);
    TEST_ASSERT_TRUE(textDraw(look, R::Blank, false) == D::Draw);  // leaving a scrub
    TEST_ASSERT_TRUE(textDraw(look, R::Blank, true) == D::Draw);
    TEST_ASSERT_TRUE(textDraw(look, R::Unknown, false) == D::Draw);  // a repaint
    TEST_ASSERT_TRUE(textDraw(look, R::Unknown, true) == D::Draw);
  }
  for (Look look : {Look::Scrubbing, Look::Off}) {
    TEST_ASSERT_TRUE(textDraw(look, R::Shown, false) == D::Blank);  // entering a scrub
    TEST_ASSERT_TRUE(textDraw(look, R::Shown, true) == D::Blank);
    TEST_ASSERT_TRUE(textDraw(look, R::Blank, false) == D::None);
    TEST_ASSERT_TRUE(textDraw(look, R::Blank, true) == D::None);  // a second, during it
    TEST_ASSERT_TRUE(textDraw(look, R::Unknown, false) == D::Blank);  // a repaint under it
    TEST_ASSERT_TRUE(textDraw(look, R::Unknown, true) == D::Blank);
  }
}

// At rest the playhead stands on the whole second, so what it moves goes in the
// pass the text row's time does, once a second at most, whatever the length: a pass
// every 40 ms through a track, as the page's update() sees it. By the px it was 30
// pushes a second at 10 s, each a bus hold of its own. With the text row's 5,120 px
// a second stays under the band's 7,040.
void test_the_playhead_moves_with_the_second() {
  struct Case {
    uint32_t lengthMs;
    int mostPx;  // of the strip, a second's move
  };
  const Case cases[] = {
      {5000, 1428},    // 59 px a second (inert: the plain line's fill): (59 + 9) x 21
      {10000, 819},    // the shortest seekable, 30 px
      {30000, 399},    // 10 px
      {52000, 315},    // SEEK-BAR.md's short track, 6 px
      {245000, 231},   // a song, 2 px at most
      {600000, 210},   // a px, in some seconds
      {3600000, 210},  // a px, every 12 s
  };
  for (const Case& c : cases) {
    const bool seekable = SeekBar::seekable(c.lengthMs);
    SeekBar idle;
    Strip drawn = stripOf(idle, seekable, true, 0, c.lengthMs, nullptr, 7, 0);
    uint32_t second = 0;
    int moves = 0, mostPx = 0;
    for (uint32_t ms = 40; ms < c.lengthMs; ms += 40) {
      const Strip now = stripOf(idle, seekable, true, ms, c.lengthMs, nullptr, 7, 0);
      const Region r = regionToDraw(drawn, now);
      const uint32_t nowSecond = ms / 1000;
      if (r.w > 0) {
        TEST_ASSERT_EQUAL_MESSAGE(second + 1, nowSecond, "a move in the pass the second changes, and only then");
        TEST_ASSERT_TRUE(r.w < kScreenW);
        ++moves;
        mostPx = std::max(mostPx, r.w * kStripH);
        drawn = now;
      }
      second = nowSecond;
    }
    TEST_ASSERT_TRUE(moves > 0);
    TEST_ASSERT_TRUE(static_cast<uint32_t>(moves) <= c.lengthMs / 1000);
    TEST_ASSERT_EQUAL_INT(c.mostPx, mostPx);
    TEST_ASSERT_TRUE(kScreenW * kTextRowH + mostPx < 7040);
  }
}

// What goes to the screen as one: a whole strip with the text row due, to draw or
// to blank. One hold for the px of two pushes.
void test_what_goes_as_one_push() {
  using R = TextRow;
  using D = TextDraw;
  const Region whole{0, kScreenW};
  TEST_ASSERT_TRUE(pushedAsOne(whole, D::Draw));
  TEST_ASSERT_TRUE(pushedAsOne(whole, D::Blank));
  TEST_ASSERT_FALSE(pushedAsOne(whole, D::None));
  TEST_ASSERT_FALSE(pushedAsOne(Region{96, 10}, D::Draw));
  TEST_ASSERT_FALSE(pushedAsOne(Region{8, 305}, D::Draw));  // the longest a seek moves it: still a part
  TEST_ASSERT_FALSE(pushedAsOne(Region{0, 0}, D::Draw));
  TEST_ASSERT_FALSE(pushedAsOne(Region{0, 0}, D::None));
  // y 137-173, under the 40 rows of a bus hold (the page's static_assert has
  // gfx::kBand), with the strip's last row its own. The px of the two pushes.
  TEST_ASSERT_EQUAL_INT(37, kStripAndTextH);
  TEST_ASSERT_EQUAL_INT(kTextRowY + kTextRowH, kStripY + kStripAndTextH);
  TEST_ASSERT_EQUAL_INT(11840, kScreenW * kStripAndTextH);
  TEST_ASSERT_EQUAL_INT(6720 + 5120, kScreenW * kStripAndTextH);

  // The passes, by the two rules. A strip of each look that shows.
  const Strip rest = restStrip(100);
  Strip paused = rest;
  paused.look = Look::RestPaused;
  paused.playing = false;
  Strip pressed = rest;
  pressed.look = Look::Pressed;
  Strip scrubbing = rest;
  scrubbing.look = Look::Scrubbing;
  scrubbing.markerX = 90;
  // A play state, a track: as one, with the text they change.
  TEST_ASSERT_TRUE(pushedAsOne(regionToDraw(rest, paused), textDraw(paused.look, R::Shown, true)));
  Strip next = rest;
  next.track = 8;
  TEST_ASSERT_TRUE(pushedAsOne(regionToDraw(rest, next), textDraw(next.look, R::Shown, true)));
  // A repaint, in any look: as one.
  for (const Strip& s : {rest, paused, pressed, scrubbing}) {
    TEST_ASSERT_TRUE(pushedAsOne(regionToDraw(Strip{}, s), textDraw(s.look, R::Unknown, false)));
  }
  // A Down, and a shape that arrives: the strip alone, the text row says the same.
  TEST_ASSERT_FALSE(pushedAsOne(regionToDraw(rest, pressed), textDraw(pressed.look, R::Shown, false)));
  Strip shaped = rest;
  shaped.version = 43;
  TEST_ASSERT_FALSE(pushedAsOne(regionToDraw(rest, shaped), textDraw(shaped.look, R::Shown, false)));
  // A playing second: the text row alone, or with a part of the strip: two pushes.
  TEST_ASSERT_FALSE(pushedAsOne(regionToDraw(rest, rest), textDraw(rest.look, R::Shown, true)));
  TEST_ASSERT_FALSE(pushedAsOne(regionToDraw(rest, restStrip(101)), textDraw(rest.look, R::Shown, true)));
  // A scrub: the first frame blanks the row in the strip's push; the later ones are
  // the strip alone; the lift is its look and the row back, as one.
  TEST_ASSERT_TRUE(pushedAsOne(whole, textDraw(scrubbing.look, R::Shown, false)));
  TEST_ASSERT_FALSE(pushedAsOne(whole, textDraw(scrubbing.look, R::Blank, true)));
  TEST_ASSERT_TRUE(pushedAsOne(regionToDraw(scrubbing, rest), textDraw(rest.look, R::Blank, false)));
  // A blank only comes with a whole strip: a look without text is a finger's scrub
  // (SeekBar::scrubbing()), and the page draws its frames whole.
  using P = SeekBar::Phase;
  for (P phase : {P::Idle, P::Pressed, P::Scrubbing, P::Off}) {
    for (bool seekable : {false, true}) {
      for (bool playing : {false, true}) {
        TEST_ASSERT_EQUAL(phase == P::Scrubbing || phase == P::Off, !inksOf(lookOf(phase, seekable, playing)).text);
      }
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_rows_and_columns);
  RUN_TEST(test_the_playhead_and_the_reach);
  RUN_TEST(test_the_columns_a_region_overlaps);
  RUN_TEST(test_what_a_moved_playhead_redraws);
  RUN_TEST(test_the_played_rule);
  RUN_TEST(test_past_the_reach);
  RUN_TEST(test_the_inks_of_every_look);
  RUN_TEST(test_when_a_track_shows_bars);
  RUN_TEST(test_the_plain_lines_rows);
  RUN_TEST(test_the_rows_around_the_strip);
  RUN_TEST(test_the_look_of_a_touch);
  RUN_TEST(test_what_the_strip_has);
  RUN_TEST(test_the_strip_through_a_touch);
  RUN_TEST(test_what_a_pass_redraws_of_the_strip);
  RUN_TEST(test_bars_up_stay_up_under_an_estimate);
  RUN_TEST(test_the_bars_stay_while_a_shape_is_read);
  RUN_TEST(test_a_track_change_draws_the_strip_once);
  RUN_TEST(test_a_track_change_with_no_shape_in_time);
  RUN_TEST(test_a_strip_not_on_the_screen_waits_blank);
  RUN_TEST(test_a_wake_draws_no_line_before_the_bars);
  RUN_TEST(test_what_a_pass_does_to_the_text_row);
  RUN_TEST(test_the_playhead_moves_with_the_second);
  RUN_TEST(test_what_goes_as_one_push);
  return UNITY_END();
}
