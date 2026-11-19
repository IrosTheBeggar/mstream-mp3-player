// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for ScanSync: when a hardware-scrolled list frame goes on the
// bus, from where the panel's scan is (ui/ListView, ui/PanelScan).
// Run: pio test -e native -f test_scan_sync
#include <unity.h>

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
  return UNITY_END();
}
