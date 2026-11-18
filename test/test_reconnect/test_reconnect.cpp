// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for ReconnectPlanner (finding the headphones again while no
// Bluetooth link is up: burst, back-off, resting, the scan by name) and
// RadioMeter, against a model of what PlayerA2dp does with its answers and
// of ESP32-A2DP v1.8.11's state as far as it matters here: its own
// auto-reconnect is disarmed (it never pages or scans by itself), a page
// the headphones don't answer ends after the controller's page timeout, and
// a scan (DISCOVERING) runs round after round until it is stopped.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <vector>

#include "ReconnectPlanner.h"
#include "SinkSearch.h"

using Lib = ReconnectPlanner::Lib;
using Do = ReconnectPlanner::Do;
using Phase = ReconnectPlanner::Phase;
using Why = ReconnectPlanner::Why;

namespace {
constexpr uint32_t kTickMs = 250;        // BtAppT's ticks from the loop
constexpr uint32_t kPageTimeoutMs = 5120;  // the controller's page timeout
// A burst nobody answers, as the model below runs it: a page's timeout is
// seen at the tick after its 5.12 s (5.25 s), the next page goes
// kBurstPauseMs after that. burstMs(n): by when n pages have all had
// their answer (and before the back-off's first page, 30 s on).
constexpr uint32_t kTryMs = 5250 + ReconnectPlanner::kBurstPauseMs;
constexpr uint32_t burstMs(int pages) { return static_cast<uint32_t>(pages) * kTryMs; }

// PlayerA2dp and the library, as the planner sees them.
struct Sim {
  ReconnectPlanner p;
  Lib state = Lib::Unconnected;
  bool discoveryActive = false;
  bool remembered = true;
  bool canScan = true;
  bool quiet = false;
  bool linked = false;
  bool answers = false;          // the headphones answer the next page (they're on)
  std::vector<uint32_t> pages;   // when each page was made
  int scans = 0;                 // scans by name started
  int stops = 0;                 // scans stopped
  bool pageOpen = false;
  uint32_t pageEndsMs = 0;
  uint32_t now = 1000;

  ReconnectPlanner::In in() const {
    return {linked, state, discoveryActive, remembered, canScan, quiet, now};
  }

  Do step() {
    const Do d = p.step(in());
    switch (d) {
      case Do::Page:
        pages.push_back(now);
        p.pageMade(now);  // connect_to()
        pageOpen = true;
        pageEndsMs = now + (answers ? 800 : kPageTimeoutMs);
        break;
      case Do::Scan:
        ++scans;
        state = Lib::Discovering;
        discoveryActive = true;
        break;
      case Do::StopScan:
        ++stops;
        state = Lib::Unconnected;
        discoveryActive = false;
        break;
      case Do::Nothing:
        break;
    }
    return d;
  }

  // Time passes in ticks: pages get their answer, the planner steps.
  void runUntil(uint32_t t) {
    while (static_cast<int32_t>(t - now) > 0) {
      now += kTickMs;
      if (pageOpen && static_cast<int32_t>(now - pageEndsMs) >= 0) {
        pageOpen = false;
        if (answers) {
          linked = true;
          p.linked();
        } else {
          p.pageEnded(now);  // DISCONNECTED: nobody answered
        }
      }
      if (!linked) step();
    }
  }
  void run(uint32_t ms) { runUntil(now + ms); }
};
}  // namespace

void setUp() {}
void tearDown() {}

// The headphones drop and stay away (in their case, off): a burst of three
// pages, each kBurstPauseMs after the last one's page timeout, then one
// page 30 s, 1, 2, 5, 5 min after the one before, then nothing from 15 min
// on: connectable only. Never a scan.
void test_the_back_off_schedule() {
  Sim s;
  const uint32_t t0 = s.now;
  s.p.start(Why::Drop, true, true, s.now);
  TEST_ASSERT_EQUAL(Do::Page, s.step());
  TEST_ASSERT_EQUAL(Phase::Burst, s.p.phase());
  TEST_ASSERT_EQUAL_INT(1, s.p.burstTry());
  s.run(20 * 60000);
  // (A page's end is seen at the tick after its 5.12 s: 5.25 s.)
  const uint32_t expected[] = {0, 7250, 14500, 44500, 104500, 224500, 524500, 824500};
  const size_t n = sizeof(expected) / sizeof(expected[0]);
  TEST_ASSERT_EQUAL_size_t(n, s.pages.size());
  for (size_t i = 0; i < n; ++i) {
    // Each within a tick of its time.
    TEST_ASSERT_UINT32_WITHIN(kTickMs, expected[i], s.pages[i] - t0);
  }
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  TEST_ASSERT_EQUAL_INT(0, s.scans);
}

// The phases in order, and when the burst ends: after its third page's
// answer (the page timeout), not before.
void test_the_burst_ends_once_its_last_page_has_its_answer() {
  Sim s;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(14500);  // the third page, just made
  TEST_ASSERT_EQUAL_size_t(3, s.pages.size());
  TEST_ASSERT_EQUAL(Phase::Burst, s.p.phase());
  TEST_ASSERT_EQUAL_INT(3, s.p.burstTry());
  s.run(kPageTimeoutMs + kTickMs);
  TEST_ASSERT_EQUAL(Phase::Backoff, s.p.phase());
  TEST_ASSERT_EQUAL_INT(0, s.p.burstTry());
  TEST_ASSERT_UINT32_WITHIN(kTickMs, 30000 - kPageTimeoutMs - kTickMs, s.p.nextPageInMs(s.now));
}

// A burst's pages follow each other closely: the radio pages for most of
// the time a listener waits. The three are done (each with its page
// timeout) inside PlayGate's 20 s, and they begin less than 7 s apart;
// between two of them the Core2 doesn't page for kBurstPauseMs (its own
// page scan's turn: headphones paging it get in).
void test_a_bursts_pages_follow_each_other() {
  Sim s;
  const uint32_t t0 = s.now;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(20000);
  TEST_ASSERT_EQUAL_size_t(3, s.pages.size());
  TEST_ASSERT_EQUAL(Phase::Backoff, s.p.phase());  // all three had their answer
  for (size_t i = 1; i < 3; ++i) {
    const uint32_t gap = s.pages[i] - s.pages[i - 1];
    TEST_ASSERT_TRUE(gap >= kPageTimeoutMs + ReconnectPlanner::kBurstPauseMs);
    TEST_ASSERT_TRUE(gap <= kPageTimeoutMs + ReconnectPlanner::kBurstPauseMs + 2 * kTickMs);
  }
  // On the air for 3 x 5.12 s of the first 20 s.
  TEST_ASSERT_TRUE(s.pages[2] + kPageTimeoutMs - t0 <= 20000);
}

// A listener's ask gets kAskPages tries: they are waiting, and headphones
// that sit on a phone answer a page only now and then (the device run of
// 2026-11-17, twice: five timed out in a row, the sixth was answered). A
// drop's burst and the boot's keep kBurstPages.
void test_an_ask_gets_more_pages_than_a_drop() {
  TEST_ASSERT_TRUE(ReconnectPlanner::kAskPages > ReconnectPlanner::kBurstPages);
  Sim a;
  const uint32_t t0 = a.now;
  a.p.start(Why::Ask, true, true, a.now);
  TEST_ASSERT_EQUAL_INT(ReconnectPlanner::kAskPages, a.p.burstPages());
  a.step();
  a.run(burstMs(ReconnectPlanner::kAskPages));
  TEST_ASSERT_EQUAL_size_t(ReconnectPlanner::kAskPages, a.pages.size());
  TEST_ASSERT_EQUAL(Phase::Backoff, a.p.phase());  // all of them had their answer
  TEST_ASSERT_TRUE(a.pages.back() + kPageTimeoutMs - t0 <= burstMs(ReconnectPlanner::kAskPages));
  // Long enough for headphones that answer 41 s after the first page (the
  // phone case, measured), with a page's length to spare.
  TEST_ASSERT_TRUE(a.pages.back() - t0 >= 41000 + kPageTimeoutMs);
  const Why others[] = {Why::Drop, Why::Boot};
  for (Why w : others) {
    Sim d;
    d.p.start(w, true, true, d.now);
    TEST_ASSERT_EQUAL_INT(ReconnectPlanner::kBurstPages, d.p.burstPages());
    d.step();
    d.run(burstMs(ReconnectPlanner::kBurstPages) + 10000);  // (its back-off's first page is 30 s on)
    TEST_ASSERT_EQUAL_size_t(ReconnectPlanner::kBurstPages, d.pages.size());
  }
  // The try's number runs to the burst's own count ("try 8 of 8").
  Sim n;
  n.p.start(Why::Ask, true, true, n.now);
  n.step();
  n.run((ReconnectPlanner::kAskPages - 1) * kTryMs + kTickMs);  // the last one, just made
  TEST_ASSERT_EQUAL_INT(ReconnectPlanner::kAskPages, n.p.burstTry());
  // An ask while a drop's burst is under way: the ask's count from there.
  Sim m;
  m.p.start(Why::Drop, true, true, m.now);
  m.step();
  m.run(3000);  // its first page on its way
  m.p.start(Why::Ask, true, true, m.now);
  TEST_ASSERT_EQUAL_INT(1, m.p.burstTry());
  TEST_ASSERT_EQUAL_INT(ReconnectPlanner::kAskPages, m.p.burstPages());
  m.run(burstMs(ReconnectPlanner::kAskPages));
  TEST_ASSERT_EQUAL_size_t(ReconnectPlanner::kAskPages, m.pages.size());
}

// A page whose end is never reported (no DISCONNECTED came): the next one
// waits kBurstGapMs from its start, as before.
void test_a_page_with_no_end_reported_keeps_the_old_spacing() {
  Sim s;
  s.p.start(Why::Ask, true, true, s.now);
  s.step();
  const uint32_t first = s.now;
  s.pageOpen = false;  // (the model never reports its end)
  s.run(ReconnectPlanner::kBurstGapMs - kTickMs);
  TEST_ASSERT_EQUAL_size_t(1, s.pages.size());
  s.run(2 * kTickMs);
  TEST_ASSERT_EQUAL_size_t(2, s.pages.size());
  TEST_ASSERT_UINT32_WITHIN(kTickMs, ReconnectPlanner::kBurstGapMs, s.pages[1] - first);
}

// A page that ends early (refused, or answered and then dropped: 0.8 s
// here) is followed as soon: kBurstPauseMs after its end.
void test_a_page_that_ends_early_is_followed_after_the_pause() {
  Sim s;
  s.p.start(Why::Ask, true, true, s.now);
  s.step();
  s.pageEndsMs = s.now + 800;
  s.run(800 + ReconnectPlanner::kBurstPauseMs + 2 * kTickMs);
  TEST_ASSERT_EQUAL_size_t(2, s.pages.size());
  TEST_ASSERT_UINT32_WITHIN(2 * kTickMs, 800 + ReconnectPlanner::kBurstPauseMs, s.pages[1] - s.pages[0]);
}

// A burst's next page waits for the last one's answer, however slow.
void test_a_page_on_its_way_is_never_paged_over() {
  Sim s;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.p.pageMade(s.now);  // (the step made it; again, as if still going)
  // No answer reported, but the page timeout passes: it no longer holds
  // the next one back (it can't be answered any more).
  TEST_ASSERT_TRUE(s.p.pageOnItsWay(s.now + ReconnectPlanner::kPageMs - 1));
  TEST_ASSERT_FALSE(s.p.pageOnItsWay(s.now + ReconnectPlanner::kPageMs));
}

// With headphones remembered, nothing scans in the background: not the
// burst, not the back-off, not resting. A scan found running (the library
// started one) is stopped.
void test_no_discovery_while_remembered() {
  Sim s;
  s.state = Lib::Discovering;  // e.g. left from before they were remembered
  s.discoveryActive = true;
  s.p.start(Why::Drop, true, true, s.now);
  TEST_ASSERT_EQUAL(Do::StopScan, s.step());
  s.run(20 * 60000);
  TEST_ASSERT_EQUAL_INT(0, s.scans);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  // Resting: a scan running is stopped too.
  s.state = Lib::Discovering;
  TEST_ASSERT_EQUAL(Do::StopScan, s.step());
  TEST_ASSERT_EQUAL(Do::Nothing, s.step());
}

// Every listener's ask (BtSink::connect(): a play waiting for them,
// Connect, a B hold; the Pair screen closing; Pr1: all start(Ask))
// restarts a full burst at once (an ask's: kAskPages), from the back-off
// and from resting.
// (Opening the Output tab isn't one: the resting card must be seen.)
void test_an_ask_restarts_the_burst() {
  Sim s;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(70000);  // in the back-off, its first page made and answered
  TEST_ASSERT_EQUAL(Phase::Backoff, s.p.phase());
  size_t before = s.pages.size();
  s.p.start(Why::Ask, true, true, s.now);
  TEST_ASSERT_EQUAL(Phase::Burst, s.p.phase());
  TEST_ASSERT_EQUAL(Do::Page, s.step());
  TEST_ASSERT_EQUAL_INT(1, s.p.burstTry());
  s.run(burstMs(ReconnectPlanner::kAskPages));
  TEST_ASSERT_EQUAL_size_t(before + ReconnectPlanner::kAskPages, s.pages.size());
  // From resting, likewise; and the 15 minutes count from the ask.
  s.run(20 * 60000);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  before = s.pages.size();
  const uint32_t askMs = s.now;
  s.p.start(Why::Ask, true, true, s.now);
  TEST_ASSERT_EQUAL(Do::Page, s.step());
  s.run(10 * 60000);
  TEST_ASSERT_EQUAL(Phase::Backoff, s.p.phase());
  TEST_ASSERT_TRUE(s.pages.size() > before + ReconnectPlanner::kAskPages);
  s.runUntil(askMs + ReconnectPlanner::kGiveUpMs + kTickMs);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
}

// ARCHITECTURE.md "Play while the headphones aren't connected": a
// background page still on its way when a play starts waiting is try 1
// of the play's burst (not paged over, and not the last of an old burst):
// the rest of an ask's tries follow, and only then the back-off.
void test_a_page_in_flight_counts_as_try_1_of_a_plays_burst() {
  Sim s;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(43500);
  TEST_ASSERT_EQUAL(Phase::Backoff, s.p.phase());
  s.run(1500);  // the back-off's first page, 44.5 s in, now on its way
  TEST_ASSERT_EQUAL_size_t(4, s.pages.size());
  TEST_ASSERT_TRUE(s.p.pageOnItsWay(s.now));
  const size_t before = s.pages.size();
  s.p.start(Why::Ask, true, true, s.now);  // the play waits: BtSink::connect()
  TEST_ASSERT_EQUAL_INT(1, s.p.burstTry());
  TEST_ASSERT_EQUAL(Do::Nothing, s.step());  // not paged over
  s.run(kPageTimeoutMs);                     // its answer: none
  s.run(burstMs(ReconnectPlanner::kAskPages));
  TEST_ASSERT_EQUAL_size_t(before + ReconnectPlanner::kAskPages - 1, s.pages.size());  // the ask's other tries
  // Try 2 only once that page had its answer, and the pause after it.
  TEST_ASSERT_TRUE(s.pages[before] - s.pages[before - 1] >= kPageTimeoutMs + ReconnectPlanner::kBurstPauseMs);
  // The same in the boot's case: the library's page at stack-up is try 1.
  Sim b;
  b.p.pageMade(b.now);  // av_hdl_stack_evt's connect_to(), before the planner starts
  b.p.start(Why::Boot, true, true, b.now + 20);
  TEST_ASSERT_EQUAL_INT(1, b.p.burstTry());
  b.now += 20;
  TEST_ASSERT_EQUAL(Do::Nothing, b.step());
}

// A page answered by the headphones: linked, the planner stands down.
void test_a_page_that_links_ends_the_search() {
  Sim s;
  s.answers = true;
  s.p.start(Why::Ask, true, true, s.now);
  s.step();
  s.run(2000);
  TEST_ASSERT_TRUE(s.linked);
  TEST_ASSERT_EQUAL(Phase::Idle, s.p.phase());
  TEST_ASSERT_EQUAL_size_t(1, s.pages.size());
}

// Nothing remembered: a scan by name, given kScanForMs from the boot or
// the ask, then resting (it doesn't scan forever). The Pair screen is the
// way to find new headphones after that.
void test_the_scan_deadline_with_none_remembered() {
  Sim s;
  s.remembered = false;
  s.p.start(Why::Boot, false, true, s.now);
  TEST_ASSERT_EQUAL(Phase::Scan, s.p.phase());
  TEST_ASSERT_EQUAL(Do::Scan, s.step());
  const uint32_t t0 = s.now;
  s.runUntil(t0 + ReconnectPlanner::kScanForMs - kTickMs);
  TEST_ASSERT_EQUAL(Phase::Scan, s.p.phase());
  TEST_ASSERT_EQUAL_INT(1, s.scans);  // (the library runs its rounds)
  s.runUntil(t0 + ReconnectPlanner::kScanForMs + kTickMs);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  TEST_ASSERT_EQUAL_INT(1, s.stops);
  TEST_ASSERT_EQUAL(Lib::Unconnected, s.state);
  s.run(30 * 60000);
  TEST_ASSERT_EQUAL_INT(1, s.scans);
  TEST_ASSERT_EQUAL_size_t(0, s.pages.size());
  // An ask scans again, for as long.
  s.p.start(Why::Ask, false, true, s.now);
  TEST_ASSERT_EQUAL(Do::Scan, s.step());
  // A scan stopped by someone else (the stack) while still in time: again.
  s.state = Lib::Unconnected;
  s.discoveryActive = false;
  TEST_ASSERT_EQUAL(Do::Scan, s.step());
  // Forgotten for good (no name to look for): no scan at all.
  Sim f;
  f.remembered = false;
  f.canScan = false;
  f.p.start(Why::Ask, false, false, f.now);
  TEST_ASSERT_EQUAL(Do::Nothing, f.step());
  TEST_ASSERT_EQUAL(Phase::Resting, f.p.phase());
}

// A release build (no BT_SINK_NAME: sinksearch::mayScan() says no) with
// nothing remembered never scans: not at the boot, not on any ask (a play
// waiting, Connect, a B hold, the console's o or Pr1, the Pair screen
// closing: all start(Ask)), not in an hour. It rests at once, connectable
// only; no page either (there is nobody to page).
void test_no_name_nothing_remembered_never_scans() {
  sinksearch::Setup release;  // no name, not forgotten, no Bs
  Sim s;
  s.remembered = false;
  s.canScan = sinksearch::mayScan(false, release);
  TEST_ASSERT_FALSE(s.canScan);
  s.p.start(Why::Boot, false, s.canScan, s.now);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());  // straight away: never "scan" on the card
  TEST_ASSERT_EQUAL(Do::Nothing, s.step());
  s.run(60 * 60000);
  for (int ask = 0; ask < 5; ++ask) {
    s.p.start(Why::Ask, false, s.canScan, s.now);
    TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
    s.run(10 * 60000);
  }
  TEST_ASSERT_EQUAL_INT(0, s.scans);
  TEST_ASSERT_EQUAL_size_t(0, s.pages.size());
  // A scan found running (the library's, at stack-up) is stopped, and
  // none follows.
  s.state = Lib::Discovering;
  s.discoveryActive = true;
  TEST_ASSERT_EQUAL(Do::StopScan, s.step());
  s.run(10 * 60000);
  TEST_ASSERT_EQUAL_INT(0, s.scans);
  // A developer build (a name) scans, as before; so does Bs's one scan.
  sinksearch::Setup dev;
  dev.name = "SPYDRONE";
  Sim d;
  d.remembered = false;
  d.canScan = sinksearch::mayScan(false, dev);
  d.p.start(Why::Boot, false, d.canScan, d.now);
  TEST_ASSERT_EQUAL(Phase::Scan, d.p.phase());
  TEST_ASSERT_EQUAL(Do::Scan, d.step());
  sinksearch::Setup bs;
  bs.bySignal = true;
  TEST_ASSERT_TRUE(sinksearch::mayScan(false, bs));
}

// The name taken away in the middle of a scan (the fresh-unit test, Bs
// used up): the scan stops, Resting.
void test_a_scan_stops_when_nothing_may_be_looked_for() {
  Sim s;
  s.remembered = false;
  s.p.start(Why::Ask, false, true, s.now);
  TEST_ASSERT_EQUAL(Do::Scan, s.step());
  s.run(5000);
  s.canScan = false;
  TEST_ASSERT_EQUAL(Do::StopScan, s.step());
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  s.run(30 * 60000);
  TEST_ASSERT_EQUAL_INT(1, s.scans);
}

// Remembered headphones: never an inquiry, whatever may be looked for
// (a name, Bs): they are paged.
void test_remembered_never_scans_whatever_the_setup() {
  sinksearch::Setup any;
  any.name = "SPYDRONE";
  any.bySignal = true;
  TEST_ASSERT_FALSE(sinksearch::mayScan(true, any));
  Sim s;
  s.canScan = sinksearch::mayScan(true, any);
  s.p.start(Why::Boot, true, s.canScan, s.now);
  TEST_ASSERT_EQUAL(Do::Page, s.step());
  s.run(20 * 60000);
  TEST_ASSERT_EQUAL_INT(0, s.scans);
}

// The scan found them by name and the connection failed: they're
// remembered now, so they are paged (a burst), no more inquiry.
void test_a_failed_connection_to_what_the_scan_found_pages_it() {
  Sim s;
  s.remembered = false;
  s.p.start(Why::Boot, false, true, s.now);
  s.step();
  s.run(5000);
  // Found: the library stops the scan and connects (DISCOVERED, CONNECTING).
  s.remembered = true;
  s.state = Lib::Other;
  s.discoveryActive = false;
  TEST_ASSERT_EQUAL(Do::Nothing, s.step());
  s.state = Lib::Connecting;
  TEST_ASSERT_EQUAL(Do::Nothing, s.step());
  s.state = Lib::Unconnected;  // it failed
  TEST_ASSERT_EQUAL(Do::Page, s.step());
  TEST_ASSERT_EQUAL(Phase::Burst, s.p.phase());
  s.run(10 * 60000);
  TEST_ASSERT_EQUAL_INT(1, s.scans);
}

// Nobody around (the screen off, nothing playing or waiting): the burst
// still runs (a drop at night may be the headphones' hiccup), then it
// rests at once, no back-off. The screen coming back on doesn't page by
// itself: a listener's ask does.
void test_resting_at_once_when_the_screen_is_off_and_nothing_plays() {
  Sim s;
  s.quiet = true;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(60 * 60000);
  TEST_ASSERT_EQUAL_size_t(3, s.pages.size());
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  // Going quiet in the middle of the back-off rests at once too.
  Sim b;
  b.p.start(Why::Drop, true, true, b.now);
  b.step();
  b.run(3 * 60000);
  TEST_ASSERT_EQUAL(Phase::Backoff, b.p.phase());
  const size_t pages = b.pages.size();
  b.quiet = true;
  b.run(kTickMs);
  TEST_ASSERT_EQUAL(Phase::Resting, b.p.phase());
  b.quiet = false;
  b.run(30 * 60000);
  TEST_ASSERT_EQUAL_size_t(pages, b.pages.size());
}

// A connection the library is making (its boot page, a device found) is
// waited for, not paged over.
void test_the_librarys_connection_is_waited_for() {
  Sim s;
  s.state = Lib::Connecting;
  s.p.start(Why::Boot, true, true, s.now);
  s.run(20000);
  TEST_ASSERT_EQUAL_size_t(0, s.pages.size());
  s.state = Lib::Unconnected;  // its 2-heartbeat timeout, or the DISCONNECTED
  TEST_ASSERT_EQUAL(Do::Page, s.step());
}

// Forgotten in the middle of a back-off: no pages to the zero address; a
// scan by name if there's a name to look for, else rest.
void test_forgotten_mid_backoff() {
  Sim s;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(60000);
  s.remembered = false;
  s.canScan = false;  // the Output tab's Forget
  const size_t pages = s.pages.size();
  s.run(10 * 60000);
  TEST_ASSERT_EQUAL_size_t(pages, s.pages.size());
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
  TEST_ASSERT_EQUAL_INT(0, s.scans);
}

// Idle (linked, the listener let go, the Pair screen) plans nothing; rest()
// (the console's Pr0) stops at once.
void test_idle_and_rest() {
  Sim s;
  TEST_ASSERT_EQUAL(Phase::Idle, s.p.phase());
  s.run(60000);
  TEST_ASSERT_EQUAL_size_t(0, s.pages.size());
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.p.rest();
  s.run(60 * 60000);
  TEST_ASSERT_EQUAL_size_t(1, s.pages.size());
  s.p.start(Why::Ask, true, true, s.now);
  s.p.stop();
  s.run(60000);
  TEST_ASSERT_EQUAL_size_t(1, s.pages.size());
}

void test_the_schedule_survives_millis_wraparound() {
  Sim s;
  s.now = 0xFFFF0000u;
  const uint32_t t0 = s.now;
  s.p.start(Why::Drop, true, true, s.now);
  s.step();
  s.run(20 * 60000);
  TEST_ASSERT_EQUAL_size_t(8, s.pages.size());
  TEST_ASSERT_UINT32_WITHIN(kTickMs, 824500, s.pages[7] - t0);
  TEST_ASSERT_EQUAL(Phase::Resting, s.p.phase());
}

// ---- RadioMeter ----

void test_radio_meter_per_minute() {
  RadioMeter m;
  TEST_ASSERT_EQUAL_INT(-1, m.lastMinutePercent());
  uint32_t t = 5000;
  // A minute: busy for its first 15 s (a page burst), then idle.
  for (uint32_t i = 0; i <= 60000 / kTickMs; ++i, t += kTickMs) m.sample(t, i * kTickMs < 15000);
  TEST_ASSERT_INT_WITHIN(1, 25, m.lastMinutePercent());
  // Always busy (the old cycle's inquiry).
  for (uint32_t i = 0; i < 60000 / kTickMs; ++i, t += kTickMs) m.sample(t, true);
  TEST_ASSERT_INT_WITHIN(1, 100, m.lastMinutePercent());
  // Resting: nothing.
  for (uint32_t i = 0; i < 60000 / kTickMs; ++i, t += kTickMs) m.sample(t, false);
  TEST_ASSERT_INT_WITHIN(1, 0, m.lastMinutePercent());
  // A long gap between samples (BtAppT busy) counts only kMaxStepMs.
  RadioMeter g;
  g.sample(0, true);
  g.sample(30000, false);
  TEST_ASSERT_EQUAL_UINT32(RadioMeter::kMaxStepMs, g.busyMsThisMinute());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_back_off_schedule);
  RUN_TEST(test_the_burst_ends_once_its_last_page_has_its_answer);
  RUN_TEST(test_a_bursts_pages_follow_each_other);
  RUN_TEST(test_an_ask_gets_more_pages_than_a_drop);
  RUN_TEST(test_a_page_with_no_end_reported_keeps_the_old_spacing);
  RUN_TEST(test_a_page_that_ends_early_is_followed_after_the_pause);
  RUN_TEST(test_a_page_on_its_way_is_never_paged_over);
  RUN_TEST(test_no_discovery_while_remembered);
  RUN_TEST(test_an_ask_restarts_the_burst);
  RUN_TEST(test_a_page_in_flight_counts_as_try_1_of_a_plays_burst);
  RUN_TEST(test_a_page_that_links_ends_the_search);
  RUN_TEST(test_the_scan_deadline_with_none_remembered);
  RUN_TEST(test_no_name_nothing_remembered_never_scans);
  RUN_TEST(test_a_scan_stops_when_nothing_may_be_looked_for);
  RUN_TEST(test_remembered_never_scans_whatever_the_setup);
  RUN_TEST(test_a_failed_connection_to_what_the_scan_found_pages_it);
  RUN_TEST(test_resting_at_once_when_the_screen_is_off_and_nothing_plays);
  RUN_TEST(test_the_librarys_connection_is_waited_for);
  RUN_TEST(test_forgotten_mid_backoff);
  RUN_TEST(test_idle_and_rest);
  RUN_TEST(test_the_schedule_survives_millis_wraparound);
  RUN_TEST(test_radio_meter_per_minute);
  return UNITY_END();
}
