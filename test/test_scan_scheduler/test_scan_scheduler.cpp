// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the card worker's scheduler (ScanScheduler,
// docs/METADATA.md 3.3.3-3.3.5, milestone N7): the scan runs on battery and
// while playing; one step at a time; covers first, the update step's build
// before everything; each yield (input, the ring, an underrun, a long decode
// pass, a track change, a seek, Bluetooth) holding the background work for
// exactly its window, in its order; the battery floor and its hysteresis;
// the jobs' and the scan's sources' order; millis()'s wrap; the build after a
// walk (U11); a slice of the walk or the scan cut, and dropped to 0, by any
// wait (2026-10-09); each step's units; and a random run against an
// independent model. The LibraryWrite blocker is in test_idle_policy.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <random>

#include "ScanScheduler.h"

void setUp() {}
void tearDown() {}

namespace {

using S = ScanScheduler;
using J = S::Job;
using W = S::Wait;
using Src = S::Source;

constexpr uint32_t kRing = 1490;  // the ring's size in ms (GAPLESS.md 2.1)

// Playing on battery at 60%, the ring full, the scan with files to read.
S::In base(uint32_t t) {
  S::In in;
  in.nowMs = t;
  in.playing = true;
  in.ringMs = kRing;
  in.ringCapacityMs = kRing;
  in.battery = 60;
  in.restPending = true;
  return in;
}

// Passes every 10 ms from `from` to `to` (inclusive) with `in` (its time
// set), the worker free: the first time a step of `job` was handed, or
// UINT32_MAX. `seen` gets the last pass's wait.
uint32_t firstStart(S& s, uint32_t from, uint32_t to, S::In in, J job = J::Scan, W* seen = nullptr) {
  for (uint32_t t = from;; t += 10) {
    in.nowMs = t;
    const S::Out o = s.update(in);
    if (seen) *seen = o.wait;
    if (o.job == job) return t;
    if (t == to) return UINT32_MAX;
  }
}

}  // namespace

void test_defaults() {
  const S::Config c;
  TEST_ASSERT_EQUAL_UINT32(500, c.inputQuietMs);
  TEST_ASSERT_EQUAL_UINT32(50, c.ringMinPct);  // 3.3.4: below 50%
  TEST_ASSERT_EQUAL_UINT32(30000, c.underrunBackoffMs);
  TEST_ASSERT_EQUAL_UINT32(40000, c.passLimitUs);
  TEST_ASSERT_EQUAL_UINT32(5000, c.passBackoffMs);
  TEST_ASSERT_EQUAL_UINT32(2000, c.trackSettleMs);
  TEST_ASSERT_EQUAL_UINT32(2000, c.seekSettleMs);
  TEST_ASSERT_EQUAL_UINT32(3000, c.btSettleMs);
  TEST_ASSERT_EQUAL_INT(10, c.floorPct);  // U13
  TEST_ASSERT_EQUAL_INT(15, c.resumeAbovePct);
  TEST_ASSERT_EQUAL_UINT32(3, S::kQueueNext);
  TEST_ASSERT_EQUAL_UINT32(200, S::kQueueSoon);
  TEST_ASSERT_EQUAL_UINT32(200, S::kBuildNowAdded);
  TEST_ASSERT_EQUAL_UINT32(60000, S::kBuildNowScanMs);
  // Nothing to do: nothing starts, nothing waits.
  S s;
  S::In in = base(0);
  in.restPending = false;
  const S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::None, o.wait);
}

// The user's choice (3.3.5): playing, on battery, at 11%, the scan goes on,
// a step whenever the worker is free; stopped or paused (the ring empty or
// held) too.
void test_runs_on_battery_and_while_playing() {
  S s;
  S::In in = base(0);
  in.battery = 11;
  // A worker whose steps take 25 ms: handed, under way, free again.
  uint32_t steps = 0, busyUntil = 0;
  for (uint32_t t = 0; t <= 60000; t += 10) {
    in.nowMs = t;
    in.running = t < busyUntil ? J::Scan : J::None;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    if (in.running != J::None) {
      TEST_ASSERT_EQUAL(J::None, o.job);
      TEST_ASSERT_EQUAL(W::Step, o.wait);
      continue;
    }
    TEST_ASSERT_EQUAL(J::Scan, o.job);
    TEST_ASSERT_EQUAL(Src::Rest, o.source);
    TEST_ASSERT_EQUAL(W::None, o.wait);
    ++steps;
    busyUntil = t + 25;
  }
  TEST_ASSERT_EQUAL_UINT32(2001, steps);  // one every 30 ms (the 25 ms step, then the next pass)
  TEST_ASSERT_FALSE(s.batteryLow());
  // Paused (the ring held), and stopped (the ring empty): no ring rule.
  for (int stopped = 0; stopped < 2; ++stopped) {
    S p;
    S::In q = base(0);
    q.playing = false;
    q.ringMs = stopped ? 0 : kRing;
    TEST_ASSERT_EQUAL(J::Scan, p.update(q).job);
  }
}

// One step at a time; the priority of the one under way.
void test_one_step_at_a_time() {
  S s;
  S::In in = base(0);
  in.cover = in.build = in.save = in.walk = true;
  const J kinds[] = {J::Cover, J::Build, J::Save, J::Walk, J::Compact, J::Scan, J::DjCheck};
  for (J running : kinds) {
    for (int moving = 0; moving < 2; ++moving) {
      for (int dark = 0; dark < 2; ++dark) {
        in.nowMs += 10;
        in.running = running;
        in.listMoving = moving == 1;
        in.dark = dark == 1;
        const S::Out o = s.update(in);
        TEST_ASSERT_EQUAL(J::None, o.job);
        TEST_ASSERT_EQUAL(W::Step, o.wait);
        // Covers and the walk's slices drop below the loop while a list
        // moves (Thumbs' rule); the build stays level with it (the listener
        // waits for it); the scan's slices are level with it only while the
        // screen is dark (3.3.9); the rest are below it.
        const uint8_t want = running == J::Build                         ? S::kHighPriority
                             : running == J::Cover || running == J::Walk ? (moving ? S::kLowPriority : S::kHighPriority)
                             : running == J::Scan                        ? (dark && !moving ? S::kHighPriority
                                                                                            : S::kLowPriority)
                                                                         : S::kLowPriority;
        TEST_ASSERT_EQUAL_UINT8(want, o.priority);
        TEST_ASSERT_EQUAL_UINT8(want, S::priorityOf(running, moving == 1, dark == 1));
        // A moving list cuts a slice (the walk's, the scan's) after its unit.
        TEST_ASSERT_EQUAL(moving == 1 && (running == J::Walk || running == J::Scan), o.cut);
      }
    }
  }
  // Handed: the walk level with the loop, the scan only in the dark.
  S t;
  S::In h = base(0);
  h.walk = true;
  S::Out o = t.update(h);
  TEST_ASSERT_EQUAL(J::Walk, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  h.walk = false;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  h.dark = true;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  h.compact = true;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Compact, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
}

// A slice of the walk or the scan under way (CardJobs, 2026-10-09) is cut
// after its unit, and dropped below the loop, by whatever would hold the
// next one: input, the ring, an underrun, a long decode pass, a track
// change, a seek, Bluetooth, a moving list, and for the scan the battery
// floor. So a wait takes effect within a unit and a pass, as with one-unit
// steps. A cover, a compaction, the build and the save aren't slices.
void test_a_slice_is_cut_by_any_wait() {
  struct Cause {
    const char* name;
    void (*set)(S::In&);
  } causes[] = {
      {"input", [](S::In& in) { in.input = true; }},
      {"ring", [](S::In& in) { in.ringMs = 100; }},
      {"underrun", [](S::In& in) { ++in.underruns; }},
      {"decode pass", [](S::In& in) { in.decodePassUs = 50000; }},
      {"track change", [](S::In& in) { in.decoderAtEnd = true; }},
      {"seek", [](S::In& in) { in.seeking = true; }},
      {"bluetooth", [](S::In& in) { in.btSetup = true; }},
      {"list", [](S::In& in) { in.listMoving = true; }},
  };
  const J kinds[] = {J::Walk, J::Scan, J::Cover, J::Compact, J::Build, J::Save};
  for (const Cause& c : causes) {
    for (J running : kinds) {
      S s;
      S::In in = base(0);
      in.dark = true;
      s.update(in);
      in.nowMs = 10;
      in.running = running;
      S::Out o = s.update(in);
      TEST_ASSERT_FALSE_MESSAGE(o.cut, c.name);  // nothing holds it
      const bool sliced = running == J::Walk || running == J::Scan;
      if (sliced) TEST_ASSERT_EQUAL_UINT8_MESSAGE(S::kHighPriority, o.priority, c.name);
      in.nowMs = 20;
      c.set(in);
      o = s.update(in);
      TEST_ASSERT_EQUAL(W::Step, o.wait);
      TEST_ASSERT_EQUAL_MESSAGE(sliced, o.cut, c.name);
      if (sliced) TEST_ASSERT_EQUAL_UINT8_MESSAGE(S::kLowPriority, o.priority, c.name);
    }
  }
  // The input's window: cut while it holds, level again after it (the slice
  // under way was cut; the next is handed only once it ends).
  S s;
  S::In in = base(0);
  in.running = J::Walk;
  s.update(in);
  in.nowMs = 100;
  in.input = true;
  TEST_ASSERT_TRUE(s.update(in).cut);
  in.input = false;
  in.nowMs = 599;
  TEST_ASSERT_TRUE(s.update(in).cut);
  in.nowMs = 600;
  S::Out o = s.update(in);
  TEST_ASSERT_FALSE(o.cut);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  // The battery floor cuts the scan's slice, not the walk's.
  S b;
  S::In low = base(0);
  low.battery = 5;
  low.running = J::Scan;
  TEST_ASSERT_TRUE(b.update(low).cut);
  low.running = J::Walk;
  o = b.update(low);
  TEST_ASSERT_FALSE(o.cut);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
}

// Each yield holds the background work, reports itself, and lets go exactly
// its window after its cause went (the cause from 1,000 ms to 5,000 ms, or
// at 1,000 ms for an event).
void test_each_yield_holds_and_lets_go() {
  enum Kind { Input, Ring, Underrun, Pass, Track, Seek, Bt, kKinds };
  const W expect[kKinds] = {W::Input, W::Ring, W::Underrun, W::DecodePass, W::TrackChange, W::Seek, W::Bluetooth};
  const uint32_t startsAt[kKinds] = {
      5500,   // input on every pass to 5,000, then 0.5 s
      5010,   // the ring below half to 5,000: the next pass
      31000,  // an underrun at 1,000: 30 s
      8000,   // passes over 40 ms at 1,000 and 3,000: 5 s after the last
      7010,   // the decoder at the end of file to 5,000, the next track heard at 5,010: 2 s into it
      3300,   // a seek asked at 1,000, its first audio at 1,300: 2 s after
      8000,   // a page burst to 5,000, the link up at 5,000: 3 s
  };
  for (int k = 0; k < kKinds; ++k) {
    S s;
    S::In in = base(0);
    in.underruns = 3;  // (counts from before: not news)
    TEST_ASSERT_EQUAL_UINT32(0, firstStart(s, 0, 0, in));
    uint32_t started = UINT32_MAX;
    for (uint32_t t = 10; t <= 40000 && started == UINT32_MAX; t += 10) {
      in = base(t);
      in.underruns = 3;
      const bool during = t >= 1000 && t <= 5000;
      switch (k) {
        case Input: in.input = during; break;
        case Ring: in.ringMs = during ? kRing / 2 - 1 : kRing; break;
        case Underrun: in.underruns = t >= 1000 ? 4 : 3; break;
        case Pass: in.decodePassUs = t == 1000 || t == 3000 ? 40001 : 12000; break;
        case Track:
          in.decoderAtEnd = during;
          in.trackSeq = t >= 5010 ? 1 : 0;
          break;
        case Seek:
          in.seeking = t >= 1000 && t <= 1300;
          in.seekSeq = t >= 1000 ? 1 : 0;
          break;
        case Bt:
          in.btSetup = during && t < 5000;
          in.btSeq = t >= 5000 ? 1 : 0;
          break;
      }
      const S::Out o = s.update(in);
      if (o.job == J::Scan) {
        if (t >= 1000) started = t;
      } else {
        TEST_ASSERT_TRUE_MESSAGE(t >= 1000, "held before its cause");
        TEST_ASSERT_EQUAL(expect[k], o.wait);
        TEST_ASSERT_EQUAL(J::None, o.job);
      }
    }
    TEST_ASSERT_EQUAL_UINT32(startsAt[k], started);
  }
}

// The ring: half of its size, only while playing, and not when its size
// isn't known.
void test_the_ring_rule() {
  S s;
  S::In in = base(0);
  in.ringMs = kRing / 2 - 1;  // 744 of 1,490
  TEST_ASSERT_EQUAL(W::Ring, s.update(in).wait);
  in.ringMs = kRing / 2;  // 745: half
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  in.ringMs = 0;
  in.playing = false;  // paused or stopped: the ring doesn't matter
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  in.playing = true;
  in.ringCapacityMs = 0;  // not known: no rule
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  // Another threshold (Config).
  S::Config c;
  c.ringMinPct = 80;
  S t(c);
  in = base(0);
  in.ringMs = 1191;  // 79.9%
  TEST_ASSERT_EQUAL(W::Ring, t.update(in).wait);
  in.ringMs = 1192;
  TEST_ASSERT_EQUAL(J::Scan, t.update(in).job);
}

// The track changes (GAPLESS.md 2.1): a gapless join holds it from the
// decoder's end of file (the next track decoded ahead into the full ring)
// to 2 s into the next; a start (a play, a skip) refills from empty, held
// by the ring and then by the track's first 2 s.
void test_track_changes_and_a_starts_refill() {
  S s;
  S::In in = base(0);
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  // The join: the ring stays full; EOF at 10,000, the next heard at 11,450.
  W seen = W::None;
  for (uint32_t t = 10000; t <= 11440; t += 10) {
    in = base(t);
    in.decoderAtEnd = true;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::TrackChange, o.wait);
  }
  in = base(11450);
  in.trackSeq = 1;
  TEST_ASSERT_EQUAL_UINT32(13450, firstStart(s, 11450, 20000, in, J::Scan, &seen));
  // A skip at 20,000: the ring empties and refills at 2x (half at about
  // 20,373), the first audio at 20,100.
  uint32_t started = UINT32_MAX;
  for (uint32_t t = 20000; t <= 30000 && started == UINT32_MAX; t += 10) {
    in = base(t);
    in.ringMs = t - 20000 >= kRing / 2 ? kRing : 2 * (t - 20000);
    in.trackSeq = t >= 20100 ? 2 : 1;
    const S::Out o = s.update(in);
    if (o.job == J::Scan) {
      started = t;
      break;
    }
    // The ring first (the refill), then the track's first 2 s.
    TEST_ASSERT_EQUAL(in.ringMs * 2 < kRing ? W::Ring : W::TrackChange, o.wait);
  }
  TEST_ASSERT_EQUAL_UINT32(22100, started);
}

// A seek: under way, and 2 s after; one asked and done between two passes
// still counts (its count changed).
void test_seeks() {
  S s;
  S::In in = base(0);
  s.update(in);
  in = base(1000);
  in.seekSeq = 1;  // (never seen under way)
  TEST_ASSERT_EQUAL(W::Seek, s.update(in).wait);
  TEST_ASSERT_EQUAL_UINT32(3000, firstStart(s, 1010, 9000, in));
  // Under way for 4 s (a slow card): held all along, then 2 s.
  in = base(10000);
  in.seekSeq = 2;
  in.seeking = true;
  for (uint32_t t = 10000; t <= 14000; t += 10) {
    in.nowMs = t;
    TEST_ASSERT_EQUAL(W::Seek, s.update(in).wait);
  }
  in.seeking = false;
  TEST_ASSERT_EQUAL_UINT32(16000, firstStart(s, 14010, 20000, in));
}

// An underrun: 30 s from each one; the count at the first pass is a baseline.
void test_underruns() {
  S s;
  S::In in = base(0);
  in.underruns = 7;
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  in.nowMs = 1000;
  in.underruns = 8;
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  in.nowMs = 20000;
  in.underruns = 9;  // another inside the window: 30 s from it
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  TEST_ASSERT_EQUAL_UINT32(50000, firstStart(s, 20010, 60000, in));
}

// A decode pass over the limit (not at it) holds it 5 s.
void test_long_decode_passes() {
  S s;
  S::In in = base(0);
  in.decodePassUs = 40000;  // at the limit: fine
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  in.nowMs = 100;
  in.decodePassUs = 40001;
  TEST_ASSERT_EQUAL(W::DecodePass, s.update(in).wait);
  in.decodePassUs = 0;
  TEST_ASSERT_EQUAL_UINT32(5100, firstStart(s, 110, 9000, in));
}

// Bluetooth: a pairing or a page burst holds it; a link event (up, down)
// holds it 3 s.
void test_bluetooth() {
  S s;
  S::In in = base(0);
  s.update(in);
  in.btSetup = true;
  for (uint32_t t = 10; t <= 45000; t += 500) {
    in.nowMs = t;
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
  }
  in.btSetup = false;
  in.btSeq = 1;  // linked at 45,010
  TEST_ASSERT_EQUAL_UINT32(48010, firstStart(s, 45010, 60000, in));
  in.btSeq = 2;  // dropped at 60,000
  TEST_ASSERT_EQUAL_UINT32(63000, firstStart(s, 60000, 70000, in));
}

// Input: each touch holds it 0.5 s; a moving list holds everything (covers
// too) for as long as it moves.
void test_input_and_lists() {
  S s;
  S::In in = base(0);
  s.update(in);
  in.nowMs = 100;
  in.input = true;
  TEST_ASSERT_EQUAL(W::Input, s.update(in).wait);
  in.input = false;
  TEST_ASSERT_EQUAL_UINT32(600, firstStart(s, 110, 2000, in));
  in.listMoving = true;
  in.cover = true;
  in.save = true;
  for (uint32_t t = 1000; t <= 9000; t += 10) {
    in.nowMs = t;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::List, o.wait);
  }
  in.listMoving = false;
  in.nowMs = 9010;
  TEST_ASSERT_EQUAL(J::Cover, s.update(in).job);  // (no quiet time after a list)
}

// Several at once: the first in the order holds, and the next shows as each
// window ends (shortened windows, so they end in order).
void test_the_order_of_the_yields() {
  S::Config c;
  c.inputQuietMs = 100;
  c.underrunBackoffMs = 200;
  c.passBackoffMs = 300;
  c.trackSettleMs = 400;
  c.seekSettleMs = 500;
  c.btSettleMs = 600;
  S s(c);
  S::In in = base(0);
  s.update(in);
  // At 1,000 everything at once; the ring stays low to 1,150.
  in.nowMs = 1000;
  in.input = true;
  in.underruns = 1;
  in.decodePassUs = 90000;
  in.trackSeq = 1;
  in.seekSeq = 1;
  in.btSeq = 1;
  in.ringMs = 100;
  in.listMoving = true;
  in.battery = 5;
  in.updating = true;
  TEST_ASSERT_EQUAL(W::List, s.update(in).wait);
  in.nowMs = 1001;
  in.listMoving = false;
  in.input = false;  // (the events were at 1,000 only)
  in.decodePassUs = 0;
  TEST_ASSERT_EQUAL(W::Updating, s.update(in).wait);
  in.nowMs = 1002;
  in.updating = false;
  TEST_ASSERT_EQUAL(W::Battery, s.update(in).wait);
  in.nowMs = 1003;
  in.usb = true;  // plugged in: the floor goes at once
  const struct {
    uint32_t until;
    W wait;
  } order[] = {{1100, W::Input}, {1150, W::Ring}, {1200, W::Underrun}, {1300, W::DecodePass},
               {1400, W::TrackChange}, {1500, W::Seek}, {1600, W::Bluetooth}};
  uint32_t t = 1003;
  for (const auto& step : order) {
    for (; t < step.until; ++t) {
      in.nowMs = t;
      in.ringMs = t < 1150 ? 100 : kRing;
      const S::Out o = s.update(in);
      TEST_ASSERT_EQUAL(J::None, o.job);
      TEST_ASSERT_EQUAL(step.wait, o.wait);
    }
  }
  in.nowMs = 1600;
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
}

// U13: below 10% off USB the scan and the DJNB check pause; 10-15% keeps
// the state; above 15%, or USB, lets them go. A reading not known keeps it.
// The rest doesn't stop for it.
void test_the_battery_floor() {
  S s;
  S::In in = base(0);
  in.battery = 10;
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  TEST_ASSERT_FALSE(o.batteryHeld);
  in.battery = 9;
  o = s.update(in);
  TEST_ASSERT_TRUE(o.batteryHeld);
  TEST_ASSERT_EQUAL(W::Battery, o.wait);
  TEST_ASSERT_TRUE(s.batteryLow());
  const int still[] = {9, 12, 15, -1, 10, 15};
  for (int pct : still) {
    in.battery = pct;
    o = s.update(in);
    TEST_ASSERT_FALSE(o.batteryHeld || o.batteryReleased);
    TEST_ASSERT_EQUAL(W::Battery, o.wait);
  }
  in.battery = 16;
  o = s.update(in);
  TEST_ASSERT_TRUE(o.batteryReleased);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  in.battery = -1;  // not known: stays let go
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
  // USB at 5%: runs; unplugged at 5%: held again; plugged: let go at once.
  in.battery = 5;
  in.usb = true;
  o = s.update(in);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  TEST_ASSERT_FALSE(o.batteryHeld);
  in.usb = false;
  o = s.update(in);
  TEST_ASSERT_TRUE(o.batteryHeld);
  in.usb = true;
  o = s.update(in);
  TEST_ASSERT_TRUE(o.batteryReleased);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  // Held: the DJNB check too; the walk, a compaction, the save, a cover and
  // the build go on.
  in.usb = false;
  s.update(in);
  TEST_ASSERT_TRUE(s.batteryLow());
  in.restPending = false;
  in.djCheck = true;
  TEST_ASSERT_EQUAL(W::Battery, s.update(in).wait);
  struct {
    bool S::In::*flag;
    J job;
  } go[] = {{&S::In::walk, J::Walk}, {&S::In::compact, J::Compact}, {&S::In::save, J::Save},
            {&S::In::cover, J::Cover}, {&S::In::build, J::Build}};
  for (const auto& g : go) {
    S::In one = in;
    one.restPending = true;
    one.*(g.flag) = true;
    o = s.update(one);
    TEST_ASSERT_EQUAL(g.job, o.job);
  }
  // The playing track's file is the scan's too: held.
  in.djCheck = false;
  in.playingPending = true;
  TEST_ASSERT_EQUAL(W::Battery, s.update(in).wait);
}

// Rows on screen first (3.3.3, 0): a cover starts before any other work,
// whatever the audio, at the loop's priority; only a moving list holds it.
void test_covers_come_first() {
  S s;
  S::In in = base(0);
  in.underruns = 1;
  s.update(in);
  in.nowMs = 10;
  in.underruns = 2;
  in.input = true;
  in.ringMs = 0;
  in.decoderAtEnd = true;
  in.seeking = true;
  in.btSetup = true;
  in.battery = 3;
  in.cover = in.save = in.walk = in.compact = in.djCheck = true;
  in.playingPending = true;
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::Cover, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  TEST_ASSERT_EQUAL(W::None, o.wait);
  in.listMoving = true;
  o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::List, o.wait);
  // Under way when a list starts moving: below the loop until it stops.
  in.running = J::Cover;
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, s.update(in).priority);
  in.listMoving = false;
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, s.update(in).priority);
  // Done; no more covers: the save next, held by the audio (here the input).
  in.running = J::None;
  in.cover = false;
  TEST_ASSERT_EQUAL(W::Input, s.update(in).wait);
}

// The update step (3.4.2, N12): its build before everything and held by
// nothing but a step under way; while it holds the worker the background
// work waits, covers don't; its save yields as the background does, but
// not to the floor; after it, the scan again.
void test_the_update_step() {
  S s;
  S::In in = base(0);
  in.compact = true;  // the journals compacted first: background work
  in.ringMs = 0;
  TEST_ASSERT_EQUAL(W::Ring, s.update(in).wait);
  in.ringMs = kRing;
  in.nowMs = 10;
  TEST_ASSERT_EQUAL(J::Compact, s.update(in).job);
  in.compact = false;
  // The fence; the build asked, with everything against it.
  in.nowMs = 20;
  in.updating = in.build = in.cover = true;
  in.listMoving = true;
  in.battery = 2;
  in.ringMs = 0;
  in.input = true;
  in.underruns = 5;
  in.btSetup = true;
  in.running = J::Scan;  // a step under way finishes first
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(W::Step, o.wait);
  in.nowMs = 30;
  in.running = J::None;
  o = s.update(in);
  TEST_ASSERT_EQUAL(J::Build, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  in.nowMs = 40;
  in.running = J::Build;
  in.build = false;
  o = s.update(in);
  TEST_ASSERT_EQUAL(W::Step, o.wait);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  // Built; the loop re-reads the queue (step 5): the scan waits, a cover
  // doesn't.
  in = base(9000);
  in.updating = true;
  in.battery = 2;
  in.underruns = 5;
  TEST_ASSERT_EQUAL(W::Updating, s.update(in).wait);
  in.nowMs = 9010;
  in.cover = true;
  TEST_ASSERT_EQUAL(J::Cover, s.update(in).job);
  in.cover = false;
  // The save: below the floor still, but after the audio's yields (the
  // underrun at 20 held it 30 s).
  in.save = true;
  in.nowMs = 9020;
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  TEST_ASSERT_EQUAL_UINT32(30020, firstStart(s, 9030, 40000, in, J::Save));
  in.save = false;
  in.updating = false;
  in.battery = 60;
  in.nowMs = 40000;
  // (The floor took 2%: it lets go above 15%.)
  TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
}

// One job at a time, the first with work: the walk, a compaction, the scan,
// the DJNB check; the scan's file from 3.3.3's sources in order.
void test_the_order_of_the_work() {
  S s;
  S::In in = base(0);
  in.walk = in.compact = in.djCheck = true;
  in.playingPending = in.queueNextPending = in.queueSoonPending = in.shownPending = true;
  const J jobs[] = {J::Walk, J::Compact, J::Scan};
  for (J j : jobs) {
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(j, o.job);
    if (j == J::Walk) in.walk = false;
    if (j == J::Compact) in.compact = false;
  }
  const Src sources[] = {Src::Playing, Src::QueueNext, Src::QueueSoon, Src::Shown, Src::Rest};
  for (Src src : sources) {
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::Scan, o.job);
    TEST_ASSERT_EQUAL(src, o.source);
    TEST_ASSERT_EQUAL(src, S::sourceOf(in));
    switch (src) {
      case Src::Playing: in.playingPending = false; break;
      case Src::QueueNext: in.queueNextPending = false; break;
      case Src::QueueSoon: in.queueSoonPending = false; break;
      case Src::Shown: in.shownPending = false; break;
      default: in.restPending = false; break;
    }
  }
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::DjCheck, o.job);
  TEST_ASSERT_EQUAL(Src::None, o.source);
  in.djCheck = false;
  o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::None, o.wait);
}

// millis() wraps after 49.7 days: a window across the wrap ends on time,
// and an old one never comes back (a device on USB for weeks).
void test_the_wrap() {
  S s;
  S::In in = base(0xFFFFF000u);
  in.underruns = 1;
  s.update(in);
  in.nowMs = 0xFFFFF100u;
  in.underruns = 2;
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  // 30 s from 0xFFFFF100: 0x00006630 after the wrap.
  in.nowMs = 0xFFFFF110u;
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  in.nowMs = 0;
  TEST_ASSERT_EQUAL(W::Underrun, s.update(in).wait);
  TEST_ASSERT_EQUAL_UINT32(0x6630u, firstStart(s, 10, 0x10000, in));
  // 60 days, a pass every 10 s, with an underrun, a seek and a long pass on
  // day 1: never held again after their windows.
  S t;
  in = base(5);
  t.update(in);
  uint64_t held = 0;
  for (uint64_t ms = 86400000ull; ms < 60ull * 86400000ull; ms += 10000) {
    in = base(static_cast<uint32_t>(ms));
    const bool day1 = ms == 86400000ull;
    in.underruns = ms >= 86400000ull ? 1 : 0;
    in.seekSeq = in.underruns;
    in.decodePassUs = day1 ? 99000 : 0;
    const S::Out o = t.update(in);
    if (o.job != J::Scan) ++held;
  }
  TEST_ASSERT_EQUAL_UINT64(3, held);  // the passes at 0, 10 and 20 s of the underrun's 30
}

// After a walk that found changes (3.3.3, U11): build at once for 200 new
// files or more, or a scan over 60 s; else at the scan's end.
void test_the_build_after_a_walk() {
  TEST_ASSERT_TRUE(S::buildAfterWalk(200, 200));
  TEST_ASSERT_FALSE(S::buildAfterWalk(199, 199));
  // 18 ms a file: 3,333 files are 59,994 ms, 3,334 are 60,012.
  TEST_ASSERT_FALSE(S::buildAfterWalk(0, 3333));
  TEST_ASSERT_TRUE(S::buildAfterWalk(0, 3334));
  TEST_ASSERT_TRUE(S::buildAfterWalk(12, 20000));
  // The scan's own rate: 60 ms a file (no sector cache) is 1,000 files.
  TEST_ASSERT_FALSE(S::buildAfterWalk(50, 1000, 60));
  TEST_ASSERT_TRUE(S::buildAfterWalk(50, 1001, 60));
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, S::scanEstimateMs(UINT32_MAX, 1000));
  TEST_ASSERT_EQUAL_UINT32(360000, S::scanEstimateMs(20000, 18));  // 3.3.4: 6 min while playing
  // The rate the scan measured replaces the estimate once it has one.
  S s;
  TEST_ASSERT_EQUAL_UINT32(S::kEstimateMsPerFile, s.scanMsPerFile());
  s.stepDone(J::Scan, 10);
  s.stepDone(J::Scan, 20);
  s.stepDone(J::Scan, 31);
  s.stepDone(J::Walk, 400);
  TEST_ASSERT_EQUAL_UINT32(3, s.steps(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(20, s.meanStepMs(J::Scan));  // 61 / 3
  TEST_ASSERT_EQUAL_UINT32(31, s.maxStepMs(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(20, s.scanMsPerFile());
  TEST_ASSERT_EQUAL_UINT32(400, s.meanStepMs(J::Walk));
  s.stepDone(J::None, 5);  // (nothing)
  TEST_ASSERT_EQUAL_UINT32(0, s.steps(J::None));
  s.resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, s.steps(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(0, s.meanStepMs(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(S::kEstimateMsPerFile, s.scanMsPerFile());
  // Slices (2026-10-09): the rate is a file's, not a step's. Two slices of
  // 16 ms and 17 ms took 2 files each, a loop source's file 10 ms, and a
  // look at rows found none (5 ms, no file): 48 ms for 5 files.
  s.stepDone(J::Scan, 16, 2);
  s.stepDone(J::Scan, 17, 2);
  s.stepDone(J::Scan, 10, 1);
  s.stepDone(J::Scan, 5, 0);
  TEST_ASSERT_EQUAL_UINT32(4, s.steps(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(5, s.units(J::Scan));
  TEST_ASSERT_EQUAL_UINT32(12, s.meanStepMs(J::Scan));  // 48 / 4 steps
  TEST_ASSERT_EQUAL_UINT32(10, s.scanMsPerFile());      // 48 / 5 files, rounded
  // Only rows looked at so far: the estimate still.
  S r;
  r.stepDone(J::Scan, 5, 0);
  TEST_ASSERT_EQUAL_UINT32(S::kEstimateMsPerFile, r.scanMsPerFile());
  // The walk's slices count their CardWalk steps.
  s.stepDone(J::Walk, 18, 6);
  s.stepDone(J::Walk, 15, 5);
  TEST_ASSERT_EQUAL_UINT32(2, s.steps(J::Walk));
  TEST_ASSERT_EQUAL_UINT32(11, s.units(J::Walk));
  s.resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, s.units(J::Walk));
  TEST_ASSERT_EQUAL_UINT32(0, s.units(J::Scan));
}

// Where the time went (the console's 'gs', L3): each pass's time to what it
// waited for.
void test_the_time_waited() {
  S s;
  S::In in = base(0);
  in.ringMs = 100;
  for (uint32_t t = 0; t <= 1000; t += 10) {
    in.nowMs = t;
    s.update(in);
  }
  in.ringMs = kRing;
  in.running = J::Scan;
  for (uint32_t t = 1010; t <= 1500; t += 10) {
    in.nowMs = t;
    s.update(in);
  }
  in.running = J::None;
  in.restPending = false;
  for (uint32_t t = 1510; t <= 3000; t += 10) {
    in.nowMs = t;
    s.update(in);
  }
  TEST_ASSERT_EQUAL_UINT64(1010, s.waitedMs(W::Ring));  // 0 to 1,010
  TEST_ASSERT_EQUAL_UINT64(500, s.waitedMs(W::Step));   // 1,010 to 1,510
  TEST_ASSERT_EQUAL_UINT64(0, s.waitedMs(W::None));
  s.resetStats();
  TEST_ASSERT_EQUAL_UINT64(0, s.waitedMs(W::Ring));
}

void test_names() {
  TEST_ASSERT_EQUAL_STRING("scan", S::jobName(J::Scan));
  TEST_ASSERT_EQUAL_STRING("compaction", S::jobName(J::Compact));
  TEST_ASSERT_EQUAL_STRING("the playing track", S::sourceName(Src::Playing));
  TEST_ASSERT_EQUAL_STRING("the ring below half", S::waitName(W::Ring));
  TEST_ASSERT_EQUAL_STRING("the battery (below the floor)", S::waitName(W::Battery));
  for (int w = 0; w < S::kWaits; ++w) TEST_ASSERT_TRUE(S::waitName(static_cast<W>(w))[0] != '?');
  for (int j = 0; j < S::kJobs; ++j) TEST_ASSERT_TRUE(S::jobName(static_cast<J>(j))[0] != '?');
}

namespace {

// The rules again, kept apart from the code: each cause as the last time it
// was seen, in 64-bit time (no wrap), against the window it opens.
struct Model {
  S::Config c;
  bool primed = false;
  uint32_t underruns = 0, trackSeq = 0, seekSeq = 0, btSeq = 0;
  int64_t inputAt = INT64_MIN / 2, underrunAt = INT64_MIN / 2, passAt = INT64_MIN / 2, trackAt = INT64_MIN / 2,
          seekAt = INT64_MIN / 2, btAt = INT64_MIN / 2;
  bool low = false;

  static bool within(int64_t t, int64_t at, uint32_t windowMs) { return t - at < static_cast<int64_t>(windowMs); }

  S::Out step(const S::In& in, int64_t t) {
    if (!primed) {
      underruns = in.underruns;
      trackSeq = in.trackSeq;
      seekSeq = in.seekSeq;
      btSeq = in.btSeq;
      primed = true;
    }
    if (in.input) inputAt = t;
    if (in.underruns != underruns) underrunAt = t;
    if (in.decodePassUs > c.passLimitUs) passAt = t;
    if (in.decoderAtEnd || in.trackSeq != trackSeq) trackAt = t;
    if (in.seeking || in.seekSeq != seekSeq) seekAt = t;
    if (in.btSetup || in.btSeq != btSeq) btAt = t;
    underruns = in.underruns;
    trackSeq = in.trackSeq;
    seekSeq = in.seekSeq;
    btSeq = in.btSeq;
    S::Out o;
    const bool wasLow = low;
    if (in.usb) low = false;
    else if (in.battery >= 0 && in.battery < c.floorPct) low = true;
    else if (in.battery > c.resumeAbovePct) low = false;
    o.batteryHeld = low && !wasLow;
    o.batteryReleased = wasLow && !low;

    const bool scan = in.playingPending || in.queueNextPending || in.queueSoonPending || in.shownPending ||
                      in.restPending;
    W yield = W::None;
    if (within(t, inputAt, c.inputQuietMs)) yield = W::Input;
    else if (in.playing && in.ringCapacityMs && in.ringMs * 100ull < in.ringCapacityMs * 1ull * c.ringMinPct)
      yield = W::Ring;
    else if (within(t, underrunAt, c.underrunBackoffMs)) yield = W::Underrun;
    else if (within(t, passAt, c.passBackoffMs)) yield = W::DecodePass;
    else if (within(t, trackAt, c.trackSettleMs)) yield = W::TrackChange;
    else if (within(t, seekAt, c.seekSettleMs)) yield = W::Seek;
    else if (within(t, btAt, c.btSettleMs)) yield = W::Bluetooth;
    if (in.running != J::None) {
      o.wait = W::Step;
      const bool slice = in.running == J::Walk || in.running == J::Scan;
      o.cut = slice && (in.listMoving || yield != W::None || (in.running == J::Scan && low));
      if (o.cut) o.priority = 0;
      else if (in.running == J::Build) o.priority = 1;
      else if (in.running == J::Cover || in.running == J::Walk) o.priority = in.listMoving ? 0 : 1;
      else if (in.running == J::Scan) o.priority = in.dark && !in.listMoving ? 1 : 0;
      else o.priority = 0;
      return o;
    }
    if (in.build) {
      o.job = J::Build;
      o.priority = 1;
      return o;
    }
    const bool bg = in.walk || in.compact || scan || in.djCheck;
    if (!in.cover && !in.save && !bg) return o;
    if (in.listMoving) {
      o.wait = W::List;
      return o;
    }
    if (in.cover) {
      o.job = J::Cover;
      o.priority = 1;
      return o;
    }
    J next = in.save ? J::Save : in.updating ? J::None : in.walk ? J::Walk : in.compact ? J::Compact
                                                     : scan     ? J::Scan
                                                                : J::DjCheck;
    if (next == J::None) {
      o.wait = W::Updating;
      return o;
    }
    const W w = (next == J::Scan || next == J::DjCheck) && low ? W::Battery : yield;
    if (w != W::None) {
      o.wait = w;
      return o;
    }
    o.job = next;
    o.priority = next == J::Walk || (next == J::Scan && in.dark) ? 1 : 0;
    o.source = next == J::Scan ? (in.playingPending     ? Src::Playing
                                  : in.queueNextPending ? Src::QueueNext
                                  : in.queueSoonPending ? Src::QueueSoon
                                  : in.shownPending     ? Src::Shown
                                                        : Src::Rest)
                               : Src::None;
    return o;
  }
};

}  // namespace

// A random session (five simulated hours, a pass every 5-40 ms, starting
// just before millis() wraps): every pass's answer equals the model's, and a
// worker that takes what it is handed never runs two steps at once, never
// starts background work below the floor, and gets through the scan.
void test_random_against_a_model() {
  std::mt19937 rng(20261008);
  // Per 100,000 passes.
  auto chance = [&](uint32_t n) { return rng() % 100000 < n; };
  // A state that lasts: on with `on` a pass, off again with `off`.
  auto flip = [&](bool& b, uint32_t on, uint32_t off) { b = b ? !chance(off) : chance(on); };
  S s;
  Model m;
  S::In in = base(0);
  int64_t t = 0xFFFFFFFFll - 3600000ll;  // an hour before the wrap
  uint64_t runUntil = 0;
  uint32_t scans = 0, covers = 0, builds = 0, cuts = 0, held[S::kWaits] = {};
  for (int pass = 0; pass < 900000; ++pass) {
    t += 5 + rng() % 36;
    in.nowMs = static_cast<uint32_t>(t);
    if (static_cast<uint64_t>(t) >= runUntil) in.running = J::None;
    flip(in.playing, 200, 100);
    flip(in.listMoving, 200, 2000);
    flip(in.dark, 100, 100);
    flip(in.decoderAtEnd, in.playing ? 100 : 0, 1500);
    flip(in.seeking, in.playing ? 50 : 0, 3000);
    flip(in.btSetup, 20, 500);
    flip(in.usb, 10, 10);
    flip(in.updating, 100, 200);
    if (chance(500)) in.build = in.updating && chance(30000);
    if (chance(500)) in.save = in.updating && chance(50000);
    if (chance(1000)) in.cover = chance(40000);
    if (chance(300)) in.walk = chance(20000);
    if (chance(300)) in.compact = chance(20000);
    if (chance(300)) in.djCheck = chance(20000);
    if (chance(500)) in.playingPending = chance(20000);
    if (chance(500)) in.queueNextPending = chance(30000);
    if (chance(500)) in.queueSoonPending = chance(40000);
    if (chance(500)) in.shownPending = chance(30000);
    if (chance(200)) in.restPending = chance(80000);
    // Events.
    in.input = chance(800);
    if (chance(5)) ++in.underruns;
    in.decodePassUs = chance(100) ? 30000 + rng() % 30000 : rng() % 25000;
    if (chance(100)) ++in.trackSeq;
    if (chance(50)) ++in.seekSeq;
    if (chance(30)) ++in.btSeq;
    if (chance(2000)) in.ringMs = chance(70000) ? kRing : rng() % (kRing + 1);
    if (chance(500)) in.battery = chance(10000) ? -1 : static_cast<int>(rng() % 30);
    if (chance(100)) in.ringCapacityMs = chance(90000) ? kRing : 0;

    const S::Out want = m.step(in, t);
    const S::Out got = s.update(in);
    TEST_ASSERT_EQUAL_MESSAGE(want.job, got.job, "job");
    TEST_ASSERT_EQUAL_MESSAGE(want.wait, got.wait, "wait");
    TEST_ASSERT_EQUAL_MESSAGE(want.source, got.source, "source");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(want.priority, got.priority, "priority");
    TEST_ASSERT_EQUAL_MESSAGE(want.cut, got.cut, "cut");
    TEST_ASSERT_EQUAL_MESSAGE(want.batteryHeld, got.batteryHeld, "held");
    TEST_ASSERT_EQUAL_MESSAGE(want.batteryReleased, got.batteryReleased, "released");
    TEST_ASSERT_EQUAL(m.low, s.batteryLow());
    ++held[static_cast<int>(got.wait)];
    if (got.cut) ++cuts;
    if (got.job != J::None) {
      TEST_ASSERT_EQUAL(J::None, in.running);
      if ((got.job == J::Scan || got.job == J::DjCheck)) TEST_ASSERT_FALSE(s.batteryLow());
      if (got.job == J::Scan) ++scans;
      if (got.job == J::Cover) ++covers;
      if (got.job == J::Build) ++builds;
      in.running = got.job;
      runUntil = static_cast<uint64_t>(t) + 5 + rng() % 80;
    }
  }
  // It got through, and every wait came up.
  TEST_ASSERT_TRUE(scans > 10000);
  TEST_ASSERT_TRUE(covers > 1000);
  TEST_ASSERT_TRUE(builds > 10);
  TEST_ASSERT_TRUE(cuts > 1000);
  for (int w = 1; w < S::kWaits; ++w) TEST_ASSERT_TRUE_MESSAGE(held[w] > 0, S::waitName(static_cast<W>(w)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_defaults);
  RUN_TEST(test_runs_on_battery_and_while_playing);
  RUN_TEST(test_one_step_at_a_time);
  RUN_TEST(test_a_slice_is_cut_by_any_wait);
  RUN_TEST(test_each_yield_holds_and_lets_go);
  RUN_TEST(test_the_ring_rule);
  RUN_TEST(test_track_changes_and_a_starts_refill);
  RUN_TEST(test_seeks);
  RUN_TEST(test_underruns);
  RUN_TEST(test_long_decode_passes);
  RUN_TEST(test_bluetooth);
  RUN_TEST(test_input_and_lists);
  RUN_TEST(test_the_order_of_the_yields);
  RUN_TEST(test_the_battery_floor);
  RUN_TEST(test_covers_come_first);
  RUN_TEST(test_the_update_step);
  RUN_TEST(test_the_order_of_the_work);
  RUN_TEST(test_the_wrap);
  RUN_TEST(test_the_build_after_a_walk);
  RUN_TEST(test_the_time_waited);
  RUN_TEST(test_names);
  RUN_TEST(test_random_against_a_model);
  return UNITY_END();
}
