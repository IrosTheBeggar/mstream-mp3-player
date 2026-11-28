// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for ScanSync: when a hardware-scrolled list frame goes on the
// bus, from where the panel's scan is (ui/ListView, ui/PanelScan); and when
// a picture put in place does (a list row drawn again where it shows, Now
// Playing's seek bar, a cover: PanelScan's Whole).
// Run: pio test -e native -f test_scan_sync
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "ScanSync.h"

namespace {

// The Core2's panel as measured (2026-11-09): 244 scan lines in 13.98 ms.
constexpr uint32_t kFrameUs = 13980;
constexpr int kLines = 244;
constexpr int kFirstRow = 2;
constexpr int kTop = 72;
// A 320 px line on the bus at 40 MHz, with the hold's overhead.
constexpr uint32_t kLineUs = 140;
constexpr uint32_t kHoldExtraUs = 200;

ScanSync sync(int height = 168) {
  ScanSync s;
  ScanSync::Panel p;
  p.frameUs = kFrameUs;
  p.lines = kLines;
  p.firstRow = kFirstRow;
  s.setPanel(p);
  s.setBand(kTop, height);
  return s;
}

uint32_t holdFor(int d) { return static_cast<uint32_t>(d < 0 ? -d : d) * kLineUs + kHoldExtraUs; }

// What the scan shows of a hold that starts `startUs` after the scan was
// read at `scanLine`: the strip's rows written a row every kLineUs from the
// start, the address at the hold's end.
struct Seen {
  int earlyRows = 0;  // strip rows the scan passed between their write and the address
  bool torn = false;  // the address changed with the scan inside the band
  double showsUs = 0; // the scan's next arrival at the band's first row after the address
};

Seen simulate(int height, int scanLine, int d, double startUs) {
  Seen seen;
  const int n = d < 0 ? -d : d;
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  const double band = kTop + kFirstRow;  // the scan line of the band's first row
  const double hold = holdFor(d);
  const double addressAt = startUs + hold;
  auto scanAt = [&](double t) { return scanLine + t / perLine; };  // unwrapped
  const int stripFirst = d > 0 ? 0 : height - n;
  for (int k = 0; k < n; ++k) {
    const double written = startUs + kHoldExtraUs / 2.0 + k * static_cast<double>(kLineUs);
    const double row = band + stripFirst + k;
    // Any pass of the scan over this row in (written, addressAt]?
    const double a = scanAt(written), b = scanAt(addressAt);
    // The smallest row + m * kLines above a.
    double m = (a - row) / kLines;
    long mi = static_cast<long>(m);
    if (mi < m) ++mi;
    double pass = row + static_cast<double>(mi) * kLines;
    if (pass <= a) pass += kLines;
    if (pass <= b) ++seen.earlyRows;
  }
  double at = scanAt(addressAt) - band;
  double inFrame = at - static_cast<long>(at / kLines) * kLines;
  if (inFrame < 0) inFrame += kLines;
  seen.torn = inFrame < height;  // rows [0, height) of the band are being scanned
  seen.showsUs = addressAt + (kLines - inFrame) * perLine;
  return seen;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_until_band_counts_scan_lines_to_its_first_row() {
  const ScanSync s = sync();
  TEST_ASSERT_TRUE(s.ready());
  TEST_ASSERT_EQUAL(0, s.untilBand(kTop + kFirstRow));
  TEST_ASSERT_EQUAL(s.lineTime(74), s.untilBand(0));
  TEST_ASSERT_EQUAL(s.lineTime(1), s.untilBand(73));
  TEST_ASSERT_EQUAL(s.lineTime(kLines - 1), s.untilBand(75));
  TEST_ASSERT_EQUAL(s.lineTime(74 + 1), s.untilBand(kLines - 1));
}

void test_not_ready_without_a_panel_or_a_band() {
  ScanSync s;
  TEST_ASSERT_FALSE(s.ready());
  ScanSync::Panel p;
  p.frameUs = kFrameUs;
  p.lines = kLines;
  p.firstRow = kFirstRow;
  s.setPanel(p);
  TEST_ASSERT_FALSE(s.ready());
  s.setBand(kTop, 168);
  TEST_ASSERT_TRUE(s.ready());
  s.setBand(kTop, 200);  // past the panel's rows
  TEST_ASSERT_FALSE(s.ready());
}

// Wherever the scan is, a hold started anywhere in the window shows only
// the finished frame: no strip row early, the address outside the band,
// and the frame on screen when plan() says.
void test_a_hold_in_the_window_is_never_seen_half_done() {
  static const int kMoves[] = {1, 5, 14, 28, 42, 60, 84, -1, -5, -14, -28, -40};
  static const int kHeights[] = {168, 148, 126};
  for (int height : kHeights) {
    const ScanSync s = sync(height);
    for (int d : kMoves) {
      for (int scan = 0; scan < kLines; scan += 3) {
        const ScanSync::Window w = s.plan(scan, d, holdFor(d));
        char msg[96];
        snprintf(msg, sizeof(msg), "height %d, d %d, scan %d", height, d, scan);
        TEST_ASSERT_TRUE_MESSAGE(w.clean, msg);
        TEST_ASSERT_TRUE_MESSAGE(w.earliestUs >= 0 && w.earliestUs <= w.latestUs, msg);
        // Never more than a refresh to wait.
        TEST_ASSERT_TRUE_MESSAGE(w.earliestUs <= static_cast<int32_t>(kFrameUs), msg);
        const double starts[] = {static_cast<double>(w.earliestUs), (w.earliestUs + w.latestUs) / 2.0,
                                 static_cast<double>(w.latestUs)};
        for (double start : starts) {
          const Seen seen = simulate(height, scan, d, start);
          TEST_ASSERT_EQUAL_MESSAGE(0, seen.earlyRows, msg);
          TEST_ASSERT_FALSE_MESSAGE(seen.torn, msg);
          TEST_ASSERT_INT_WITHIN_MESSAGE(120, w.showsUs, static_cast<int>(seen.showsUs), msg);
        }
      }
    }
  }
}

// The windows on the Core2. Up the list (the strip at the band's top) the
// hold may start as soon as its address will fall after the band's last
// row: about 3.8 ms of room whatever the step. Down the list it starts
// behind the scan at the bottom strip, and has to be done before the scan
// is back: 3 ms for a slow move, 1.1 ms at a fling's top speed (28 lines a
// refresh at 2,000 px/s), none from 42 lines.
void test_window_widths_on_the_core2() {
  const ScanSync s = sync();
  // The scan just past the band's first row: the window of this refresh.
  const int scan = kTop + kFirstRow + 1;
  static const int kUp[] = {5, 28, 60};
  for (int d : kUp) {
    const ScanSync::Window up = s.plan(scan, d, holdFor(d));
    TEST_ASSERT_INT_WITHIN(150, 3780, up.latestUs - up.earliestUs);
    TEST_ASSERT_EQUAL(s.untilBand(scan), up.showsUs);
  }
  const ScanSync::Window slow = s.plan(scan, -5, holdFor(-5));
  TEST_ASSERT_INT_WITHIN(200, 3050, slow.latestUs - slow.earliestUs);
  const ScanSync::Window down = s.plan(scan, -28, holdFor(-28));
  TEST_ASSERT_INT_WITHIN(150, 1150, down.latestUs - down.earliestUs);
  TEST_ASSERT_EQUAL(s.untilBand(scan), down.showsUs);
  TEST_ASSERT_TRUE(s.plan(scan, -41, holdFor(-41)).clean);
  TEST_ASSERT_FALSE(s.plan(scan, -42, holdFor(-42)).clean);
}

// A hold that turns out shorter than the caller guessed: its address would
// go out early, with the scan still in the band. plan() says when the scan
// has left it, and a hold that waits for that is whole again.
void test_a_shorter_hold_waits_for_the_scan_to_leave_the_band() {
  const ScanSync s = sync();
  const int scan = kTop + kFirstRow + 1;
  const int d = 28;
  const uint32_t guessed = holdFor(d) + 600;  // 15 % over
  const ScanSync::Window w = s.plan(scan, d, guessed);
  // Started at the window's first moment, the real hold ends too early.
  const Seen early = simulate(168, scan, d, w.earliestUs);
  TEST_ASSERT_TRUE(early.torn);
  // The address held back until the scan has left the band: as late a start.
  const double realHold = holdFor(d);
  TEST_ASSERT_TRUE(w.earliestUs + realHold < w.addressUs);
  const Seen waited = simulate(168, scan, d, w.addressUs - realHold);
  TEST_ASSERT_FALSE(waited.torn);
  TEST_ASSERT_EQUAL(0, waited.earlyRows);
  // ... and still before the scan is back.
  TEST_ASSERT_TRUE(w.addressUs < w.showsUs - s.lineTime(s.config().exitLines));
  // Down the list the hold always outlasts the scan's way out of the band.
  const ScanSync::Window down = s.plan(scan, -28, holdFor(-28));
  TEST_ASSERT_TRUE(down.earliestUs + static_cast<int32_t>(holdFor(-28)) > down.addressUs);
}

// Past the window, the next refresh's: a frame is never planned into a
// window that is over.
void test_a_missed_window_is_the_next_refresh() {
  const ScanSync s = sync();
  const int d = -28;
  // The scan a little before the band: this refresh's window (which began
  // when the scan entered the strip) is nearly over or over.
  const ScanSync::Window late = s.plan(kTop + kFirstRow - 2, d, holdFor(d));
  TEST_ASSERT_TRUE(late.latestUs >= 0);
  TEST_ASSERT_TRUE(late.showsUs > static_cast<int32_t>(kFrameUs) / 2);  // not the band start 2 lines away
  const Seen seen = simulate(168, kTop + kFirstRow - 2, d, late.earliestUs);
  TEST_ASSERT_EQUAL(0, seen.earlyRows);
  TEST_ASSERT_FALSE(seen.torn);
}

// A window that closed a moment ago is still planned when the caller says
// how late it will go: the hold starts now, for the same refresh. Later
// than that, the next refresh's.
void test_a_window_just_closed_is_still_planned_when_late_is_allowed() {
  const ScanSync s = sync();
  const int d = 28;
  const uint32_t hold = holdFor(d);
  // The scan 20 lines (1.15 ms) before the band: the window (which ends a
  // hold and 8 lines before it) closed some 3.4 ms ago.
  const int scan = kTop + kFirstRow - 20;
  const ScanSync::Window strict = s.plan(scan, d, hold);
  TEST_ASSERT_TRUE(strict.earliestUs > 4000);  // the next refresh's
  TEST_ASSERT_TRUE(strict.showsUs > static_cast<int32_t>(kFrameUs));
  TEST_ASSERT_FALSE(strict.late);
  const ScanSync::Window late = s.plan(scan, d, hold, 5000);
  TEST_ASSERT_TRUE(late.late);
  TEST_ASSERT_EQUAL(0, late.earliestUs);
  TEST_ASSERT_EQUAL(0, late.latestUs);
  TEST_ASSERT_EQUAL(s.untilBand(scan), late.showsUs);  // this refresh's
  // ... but not one that closed longer ago than allowed.
  const ScanSync::Window tooLate = s.plan(scan, d, hold, 3000);
  TEST_ASSERT_FALSE(tooLate.late);
  TEST_ASSERT_EQUAL(strict.earliestUs, tooLate.earliestUs);
  TEST_ASSERT_EQUAL(strict.showsUs, tooLate.showsUs);
  // A whole refresh allowed: wherever the scan is, the window is this
  // refresh's (ahead, open or just over), never the next one's: no hold
  // ever waits longer than to its own window.
  for (int at = 0; at < kLines; ++at) {
    const ScanSync::Window w = s.plan(at, d, hold, kFrameUs);
    const ScanSync::Window plain = s.plan(at, d, hold);
    TEST_ASSERT_TRUE(w.earliestUs <= plain.earliestUs);
    TEST_ASSERT_TRUE(w.late ? w.earliestUs == 0 : w.earliestUs == plain.earliestUs);
    TEST_ASSERT_TRUE(w.showsUs <= static_cast<int32_t>(kFrameUs));
  }
  // An open window is the same with it.
  const int open = kTop + kFirstRow + 120;
  const ScanSync::Window a = s.plan(open, d, hold), b = s.plan(open, d, hold, 5000);
  TEST_ASSERT_FALSE(b.late);
  TEST_ASSERT_EQUAL(a.earliestUs, b.earliestUs);
  TEST_ASSERT_EQUAL(a.latestUs, b.latestUs);
  TEST_ASSERT_EQUAL(a.showsUs, b.showsUs);
}

// A step too big for its hold to fit behind the scan (the full 84 lines
// down the list): the address still lands outside the band; only the
// strip's first rows show a refresh early.
void test_a_step_too_big_keeps_the_address_on_time() {
  const ScanSync s = sync();
  for (int scan = 0; scan < kLines; scan += 5) {
    const ScanSync::Window w = s.plan(scan, -84, holdFor(-84));
    TEST_ASSERT_FALSE(w.clean);
    TEST_ASSERT_EQUAL(w.earliestUs, w.latestUs);
    const Seen seen = simulate(168, scan, -84, w.latestUs);
    TEST_ASSERT_FALSE(seen.torn);
    TEST_ASSERT_TRUE(seen.earlyRows > 0);
    TEST_ASSERT_TRUE(seen.earlyRows < 84);
  }
}

// ---- a picture put in place (rows(), met()) ----

namespace {

// How near the scan comes to the line being written, in scan lines, for a
// hold of `pushUs` over rows [row, row + lines) that starts `startUs` after
// the scan was read at `scanLine`: 0 or less when they meet. Worked out
// apart from rows() and met(): the scan's place past the picture's first
// row less the written line's, unwrapped; they meet when that difference
// passes a whole number of refreshes.
double clearance(int scanLine, int row, int lines, double pushUs, double startUs) {
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  double rel0 = scanLine - (row + kFirstRow) + startUs / perLine;
  rel0 -= static_cast<long>(rel0 / kLines) * static_cast<double>(kLines);
  if (rel0 < 0) rel0 += kLines;  // 0 .. a refresh
  const double rel1 = rel0 + pushUs / perLine - lines;
  const double lo = rel0 < rel1 ? rel0 : rel1, hi = rel0 < rel1 ? rel1 : rel0;
  return std::min(lo, kLines - hi);  // to the meeting behind it (0) and to the one ahead (a refresh)
}

bool meets(int scanLine, int row, int lines, double pushUs, double startUs) {
  return clearance(scanLine, row, lines, pushUs, startUs) <= 0;
}

// What a picture's hold takes on the bus (ui/PanelScan's two figures): at
// the quickest the wire's own speed, 0.4 us a pixel at 40 MHz; at the
// slowest a 320 px line 140 us, 8 us more a line, and the hold's own time.
uint32_t quickest(int lines, int width) { return static_cast<uint32_t>(lines) * static_cast<uint32_t>(width) * 2u / 5u; }
uint32_t slowest(int lines, int width) {
  return static_cast<uint32_t>(lines) * (static_cast<uint32_t>(width) * kLineUs / 320u + 8u) + 100u;
}

ScanSync::Rows rowsFor(const ScanSync& s, int scanLine, int row, int lines, int width) {
  return s.rows(scanLine, row, lines, quickest(lines, width), slowest(lines, width));
}

}  // namespace

// A list row at full width: its line takes longer than a scan row, so the
// scan must be past its first row and the hold must end before the scan
// comes round, at the hold's slowest. That leaves 163 of the refresh's 244
// lines to start in.
void test_rows_a_list_row_is_slower_than_the_scan() {
  const ScanSync s = sync();
  const uint32_t quick = quickest(42, 320), slow = slowest(42, 320);  // 5,376 and 6,316 us: 93 to 111 scan lines
  const int first = 114 + kFirstRow;                                  // the scan line of screen row 114
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  // The scan 4 lines past the first row: now, and for the 163 lines to the
  // last clean start.
  ScanSync::Rows r = rowsFor(s, first + 4, 114, 42, 320);
  TEST_ASSERT_TRUE(r.clean);
  TEST_ASSERT_EQUAL_INT32(0, r.waitUs);
  TEST_ASSERT_INT32_WITHIN(60, static_cast<int32_t>(163 * perLine), r.openUs);
  // The scan on the first row: 4 lines' wait (it has to be past it).
  r = rowsFor(s, first, 114, 42, 320);
  TEST_ASSERT_INT32_WITHIN(2, static_cast<int32_t>(4 * perLine), r.waitUs);
  // The scan 30 lines above it, coming: it would catch the writing up
  // inside the row. Wait for it to pass: 34 lines.
  r = rowsFor(s, first - 30, 114, 42, 320);
  TEST_ASSERT_TRUE(r.clean);
  TEST_ASSERT_INT32_WITHIN(2, static_cast<int32_t>(34 * perLine), r.waitUs);
  for (const uint32_t push : {quick, slow}) {
    TEST_ASSERT_TRUE(meets(first - 30, 114, 42, push, 0));
    TEST_ASSERT_FALSE(meets(first - 30, 114, 42, push, r.waitUs));
  }
  // The longest wait there is: 4.6 ms (the scan just past the last clean
  // start).
  int32_t longest = 0;
  for (int line = 0; line < kLines; ++line) longest = std::max(longest, rowsFor(s, line, 114, 42, 320).waitUs);
  TEST_ASSERT_TRUE(longest < 4700);
  TEST_ASSERT_TRUE(longest > 4400);
}

// A cover's line (40 px) is quicker than a scan row: the writing runs ahead
// of a scan that hasn't reached it, and must not overtake one inside it.
void test_rows_a_cover_is_quicker_than_the_scan() {
  const ScanSync s = sync();
  const uint32_t quick = quickest(40, 40), slow = slowest(40, 40);  // 640 and 1,120 us: 11 to 20 scan lines
  const int first = 100 + kFirstRow;
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  // The scan inside the cover, 10 rows down: the writing would catch it.
  ScanSync::Rows r = rowsFor(s, first + 10, 100, 40, 40);
  TEST_ASSERT_TRUE(r.clean);
  TEST_ASSERT_TRUE(meets(first + 10, 100, 40, quick, 0));
  TEST_ASSERT_TRUE(meets(first + 10, 100, 40, slow, 0));
  // To 33 lines past: the 29 the writing gains at its quickest, 4 over.
  TEST_ASSERT_INT32_WITHIN(60, static_cast<int32_t>(23 * perLine), r.waitUs);
  TEST_ASSERT_FALSE(meets(first + 10, 100, 40, quick, r.waitUs));
  TEST_ASSERT_FALSE(meets(first + 10, 100, 40, slow, r.waitUs));
  // The scan 20 lines above it, coming: the writing is done before it
  // arrives. Now.
  r = rowsFor(s, first - 20, 100, 40, 40);
  TEST_ASSERT_EQUAL_INT32(0, r.waitUs);
  TEST_ASSERT_FALSE(meets(first - 20, 100, 40, slow, 0));
  // Below it, the same.
  r = rowsFor(s, first + 120, 100, 40, 40);
  TEST_ASSERT_EQUAL_INT32(0, r.waitUs);
  // The scan about to reach it (5 lines above): past the cover's danger
  // first, a start a little late would find it inside.
  r = rowsFor(s, first - 5, 100, 40, 40);
  TEST_ASSERT_INT32_WITHIN(60, static_cast<int32_t>(38 * perLine), r.waitUs);
}

// The rule itself: over pictures of every shape the UI puts in place, and
// every place the scan can be, a hold begun anywhere in its window never
// has the scan on the line it writes, whatever it takes between its
// quickest and its slowest. And the window is no narrower than it has to
// be: a start a few lines before it opens meets the scan at one of the two.
void test_rows_a_hold_in_its_window_never_meets_the_scan() {
  const ScanSync s = sync();
  struct Shape {
    int lines, width;
  };
  // (120 x 131: a line of it is quicker than a scan row by the first
  // figure and slower by the second.)
  const Shape shapes[] = {{42, 320}, {42, 284}, {37, 320}, {21, 320}, {21, 60}, {27, 320},  {40, 40},  {98, 98},
                          {14, 36},  {30, 22},  {56, 106}, {96, 96},  {1, 320}, {120, 320}, {56, 320}, {120, 131}};
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  int checked = 0, outside = 0;
  for (const Shape& sh : shapes) {
    const uint32_t quick = quickest(sh.lines, sh.width), slow = slowest(sh.lines, sh.width);
    const double pushes[] = {static_cast<double>(quick), (quick + slow) / 2.0, static_cast<double>(slow)};
    for (int row = 0; row + sh.lines <= 240; row += 17) {
      for (int line = 0; line < kLines; ++line) {
        const ScanSync::Rows r = s.rows(line, row, sh.lines, quick, slow);
        TEST_ASSERT_TRUE(r.clean);
        TEST_ASSERT_TRUE(r.waitUs >= 0 && r.waitUs < static_cast<int32_t>(kFrameUs));
        TEST_ASSERT_TRUE(r.openUs >= 0);  // (0: the scan is at the last clean start)
        // Its first moment, its middle, and just before its last.
        for (const double at : {0.0, 0.5, 0.99}) {
          const double start = r.waitUs + at * r.openUs;
          for (const double push : pushes) {
            TEST_ASSERT_FALSE(meets(line, row, sh.lines, push, start));
            ++checked;
          }
        }
        // Six scan lines before it opens (when there is a wait): the
        // margin there is 4 lines, so that start is outside, and at its
        // quickest or its slowest the hold meets the scan.
        if (r.waitUs > static_cast<int32_t>(8 * perLine)) {
          const double early = r.waitUs - 6 * perLine;
          TEST_ASSERT_TRUE(meets(line, row, sh.lines, quick, early) || meets(line, row, sh.lines, slow, early));
          ++outside;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(checked > 300000);
  TEST_ASSERT_TRUE(outside > 1000);
}

// How much of a refresh a picture may start in (the rest is the wait's
// chance): a list row, Now Playing's strip with its text row, a cover.
void test_rows_windows_on_the_core2() {
  const ScanSync s = sync();
  const double perLine = static_cast<double>(kFrameUs) / kLines;
  auto share = [&](int lines, int width) {
    // (From a scan on the picture's first row, before any window: its
    // whole length.)
    const ScanSync::Rows r = rowsFor(s, 100 + kFirstRow, 100, lines, width);
    TEST_ASSERT_TRUE(r.waitUs > 0);
    return static_cast<int>(r.openUs / perLine + 0.5) * 100 / kLines;
  };
  TEST_ASSERT_INT_WITHIN(2, 67, share(42, 320));  // a row: 163 of 244 lines
  TEST_ASSERT_INT_WITHIN(2, 70, share(37, 320));  // the seek bar and its text row
  TEST_ASSERT_INT_WITHIN(2, 58, share(56, 320));  // Now Playing's transport strip, the tallest
  TEST_ASSERT_INT_WITHIN(2, 83, share(40, 40));   // a list's cover
  TEST_ASSERT_INT_WITHIN(2, 82, share(98, 98));   // Now Playing's cover
}

// A picture too tall for its hold to stay clear of the scan (a whole band
// at full width: 21.5 to 25 ms, a refresh and a half and more) has no clean
// start: it is written at once.
void test_rows_too_tall_for_a_hold_has_no_window() {
  const ScanSync s = sync();
  for (int line = 0; line < kLines; line += 7) {
    const ScanSync::Rows r = rowsFor(s, line, 72, 168, 320);
    TEST_ASSERT_FALSE(r.clean);
    TEST_ASSERT_EQUAL_INT32(0, r.waitUs);
    TEST_ASSERT_EQUAL_INT32(0, r.openUs);
  }
  // 145 rows still have one (the scan a refresh on comes up behind the
  // writing and doesn't reach it); 146 don't.
  TEST_ASSERT_TRUE(rowsFor(s, 0, 72, 145, 320).clean);
  TEST_ASSERT_FALSE(rowsFor(s, 0, 72, 146, 320).clean);
}

// rows() and met() ask nothing of the list's band: a page with no list has
// pictures too.
void test_rows_need_the_panel_only() {
  ScanSync s;
  TEST_ASSERT_FALSE(s.panelKnown());
  ScanSync::Panel p;
  p.frameUs = kFrameUs;
  p.lines = kLines;
  p.firstRow = kFirstRow;
  s.setPanel(p);
  TEST_ASSERT_TRUE(s.panelKnown());
  TEST_ASSERT_FALSE(s.ready());  // (no band)
  TEST_ASSERT_TRUE(rowsFor(s, 50, 137, 37, 320).clean);
  TEST_ASSERT_FALSE(s.met(137 + kFirstRow + 10, 137, 37, 5000));
}

// met(), of holds that have been: a row begun just behind the scan and
// done in time, one a decoder's pass cut into, one begun with the scan on
// its way.
void test_met_says_what_a_hold_did() {
  const ScanSync s = sync();
  const int first = 114 + kFirstRow;
  // Begun 4 lines behind the scan, 5.5 ms: the scan stays ahead, and isn't
  // back in time.
  TEST_ASSERT_FALSE(s.met(first + 4, 114, 42, 5500));
  // The same start, 16.5 ms (10 ms of them another task's): the scan came
  // round and passed the line the writing stood at.
  TEST_ASSERT_TRUE(s.met(first + 4, 114, 42, 16500));
  // The scan on the row's first line as the writing begins, or 30 lines
  // above it and coming: it crosses the written line.
  TEST_ASSERT_TRUE(s.met(first, 114, 42, 5500));
  TEST_ASSERT_TRUE(s.met(first - 30, 114, 42, 5500));
  // Begun late in the refresh, the scan well below the row: the scan is
  // back at the row before the writing's end only if that takes long.
  TEST_ASSERT_FALSE(s.met(first + 160, 114, 42, 5500));  // 84 lines to come round, 96 gone, the writing 42 down: 30 short of it
  TEST_ASSERT_TRUE(s.met(first + 200, 114, 42, 5500));   // 44 to come round: it catches the writing up
  // A cover (40 x 40, 0.8 ms): the scan 10 rows inside it is overtaken, 35
  // rows inside isn't, nor is one that has yet to reach it.
  TEST_ASSERT_TRUE(s.met(100 + kFirstRow + 10, 100, 40, 800));
  TEST_ASSERT_FALSE(s.met(100 + kFirstRow + 35, 100, 40, 800));
  TEST_ASSERT_FALSE(s.met(100 + kFirstRow - 20, 100, 40, 800));
}

// ... and the same answer as the scan's and the writing's places worked out
// one against the other, for every shape, place and length (but where the
// two come within a hundredth of a line of each other: either answer).
void test_met_agrees_with_the_scan_worked_out() {
  const ScanSync s = sync();
  struct Shape {
    int lines, width;
  };
  const Shape shapes[] = {{42, 320}, {37, 320}, {21, 60}, {40, 40}, {98, 98}, {56, 320}, {120, 131}, {1, 320}};
  int checked = 0, met = 0;
  for (const Shape& sh : shapes) {
    const uint32_t quick = quickest(sh.lines, sh.width), slow = slowest(sh.lines, sh.width);
    for (const uint32_t took : {quick, (quick + slow) / 2, slow, slow + 4000u, slow + 11000u, slow + 30000u}) {
      for (int row = 0; row + sh.lines <= 240; row += 23) {
        for (int line = 0; line < kLines; ++line) {
          const double c = clearance(line, row, sh.lines, took, 0);
          if (c > -0.01 && c < 0.01) continue;
          TEST_ASSERT_EQUAL(c <= 0, s.met(line, row, sh.lines, took));
          ++checked;
          if (c <= 0) ++met;
        }
      }
    }
  }
  TEST_ASSERT_TRUE(checked > 50000);
  TEST_ASSERT_TRUE(met > 10000 && met < checked - 10000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_until_band_counts_scan_lines_to_its_first_row);
  RUN_TEST(test_not_ready_without_a_panel_or_a_band);
  RUN_TEST(test_a_hold_in_the_window_is_never_seen_half_done);
  RUN_TEST(test_window_widths_on_the_core2);
  RUN_TEST(test_a_shorter_hold_waits_for_the_scan_to_leave_the_band);
  RUN_TEST(test_a_missed_window_is_the_next_refresh);
  RUN_TEST(test_a_window_just_closed_is_still_planned_when_late_is_allowed);
  RUN_TEST(test_a_step_too_big_keeps_the_address_on_time);
  RUN_TEST(test_rows_a_list_row_is_slower_than_the_scan);
  RUN_TEST(test_rows_a_cover_is_quicker_than_the_scan);
  RUN_TEST(test_rows_a_hold_in_its_window_never_meets_the_scan);
  RUN_TEST(test_rows_windows_on_the_core2);
  RUN_TEST(test_rows_too_tall_for_a_hold_has_no_window);
  RUN_TEST(test_rows_need_the_panel_only);
  RUN_TEST(test_met_says_what_a_hold_did);
  RUN_TEST(test_met_agrees_with_the_scan_worked_out);
  return UNITY_END();
}
