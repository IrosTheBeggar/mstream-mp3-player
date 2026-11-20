// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the card worker's scheduler (ScanScheduler,
// docs/METADATA.md 3.3.3-3.3.5, milestone N7): the scan runs on battery and
// while playing; one step at a time; a waveform's read, then covers, first;
// the update step's build before everything; each yield (input, the ring, an
// underrun, a long decode pass, a track change, a seek, Bluetooth) holding the
// background work for exactly its window, in its order; a link's wait for its
// AVRCP, capped, and a radio link's set-up before it, capped too
// (2026-11-19); a list "moving" in the dark holding nothing
// (2026-10-14); the battery floor and its hysteresis;
// the jobs' and the scan's sources' order; millis()'s wrap; the build after a
// walk (U11); a slice of the walk or the scan cut, and dropped to 0, by any
// wait (2026-10-09); each step's units; the listening stats' two lanes
// (docs/LISTENING-STATS.md 6.8: a track's read after a waveform's, a rank
// as background work after a compaction, never under the update step or a
// yield, at 0 while lit, never cut); and a random run against an
// independent model. The LibraryWrite blocker is in test_idle_policy.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <random>

#include "RadioLinkNotes.h"
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
  TEST_ASSERT_EQUAL_UINT32(10000, c.btControlWaitMs);  // the wait for AVRCP's cap (2026-11-19)
  TEST_ASSERT_EQUAL_UINT32(8000, c.btLinkSetupMs);     // the radio link's set-up's cap (2026-11-19)
  TEST_ASSERT_EQUAL_INT(10, c.floorPct);  // U13
  TEST_ASSERT_EQUAL_INT(15, c.resumeAbovePct);
  TEST_ASSERT_EQUAL_UINT32(3, S::kQueueNext);
  TEST_ASSERT_EQUAL_UINT32(200, S::kQueueSoon);
  TEST_ASSERT_EQUAL_UINT32(200, S::kBuildNowAdded);
  TEST_ASSERT_EQUAL_UINT32(60000, S::kBuildNowScanMs);
  // No link and no AVRCP unless the glue says so: the tests that leave them
  // out keep their meaning.
  const S::In blank;
  TEST_ASSERT_FALSE(blank.btLinked);
  TEST_ASSERT_FALSE(blank.btControlUp);
  TEST_ASSERT_FALSE(blank.btLinkSetup);
  TEST_ASSERT_EQUAL_UINT32(0, blank.btLinkSetupSeq);
  // Nothing to do: nothing starts, nothing waits.
  S s;
  S::In in = base(0);
  in.restPending = false;
  const S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::None, o.wait);
  TEST_ASSERT_FALSE(o.btControlWait);
  TEST_ASSERT_FALSE(o.btLinkSetupWait);
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
  in.cover = in.wave = in.build = in.save = in.walk = true;
  const J kinds[] = {J::Cover, J::Wave, J::Build, J::Save, J::Walk, J::Compact, J::Scan};
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
        // Covers, waveforms' reads and the walk's slices drop below the loop
        // while a list moves (Thumbs' rule); the build stays level with it
        // (the listener waits for it); the scan's slices, a compaction and the
        // save are level with it only while the screen is dark (3.3.9; the
        // 2026-10-09 device run). A list "moving" in the dark holds nothing
        // (listHolds(): none is seen moving).
        const bool held = moving == 1 && dark == 0;
        const bool lane = running == J::Cover || running == J::Wave || running == J::Walk;
        const uint8_t want = running == J::Build ? S::kHighPriority
                             : lane              ? (held ? S::kLowPriority : S::kHighPriority)
                             : dark              ? S::kHighPriority
                                                 : S::kLowPriority;
        TEST_ASSERT_EQUAL_UINT8(want, o.priority);
        TEST_ASSERT_EQUAL_UINT8(want, S::priorityOf(running, moving == 1, dark == 1));
        // A moving list cuts a slice (the walk's, the scan's) after its unit.
        TEST_ASSERT_EQUAL(held && (running == J::Walk || running == J::Scan), o.cut);
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
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);  // dark
  h.dark = false;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Compact, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  // The save: level with the loop only in the dark too.
  h.save = true;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Save, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  h.dark = true;
  o = t.update(h);
  TEST_ASSERT_EQUAL(J::Save, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
}

// A slice of the walk or the scan under way (CardJobs, 2026-10-09) is cut
// after its unit, and dropped below the loop, by whatever would hold the
// next one: input, the ring, an underrun, a long decode pass, a track
// change, a seek, Bluetooth (a link's wait for AVRCP too), a moving list
// (the screen lit: a list moves only there), and for the scan the battery
// floor. So a wait takes effect within a unit and a pass, as with one-unit
// steps. A cover, a waveform's read, a compaction, the build and the save
// aren't slices.
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
      {"the wait for AVRCP", [](S::In& in) { in.btLinked = true; }},  // (no btSeq: the wait alone)
      {"list", [](S::In& in) {
         in.listMoving = true;
         in.dark = false;
       }},
  };
  const J kinds[] = {J::Walk, J::Scan, J::Cover, J::Wave, J::Compact, J::Build, J::Save};
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

namespace {

// A link as main.cpp's stepCard feeds it: up from `linkAt` until `dropAt`
// (UINT32_MAX: never), btSeq counting its flips; AVRCP up from `controlAt`
// (UINT32_MAX: never), only while linked (BtSink::avrcpUp() is tied to the
// link) unless `tied` is false (AVRCP's flag up before the link: not what the
// glue gives, the scheduler takes it all the same).
struct Link {
  uint32_t linkAt = UINT32_MAX, dropAt = UINT32_MAX, controlAt = UINT32_MAX;
  bool tied = true;
  void set(S::In& in, uint32_t t) const {
    const bool up = t >= linkAt && t < dropAt;
    in.btLinked = up;
    in.btControlUp = (up || !tied) && t >= controlAt;
    in.btSeq = (t >= linkAt ? 1u : 0u) + (t >= dropAt ? 1u : 0u);
  }
};

// Passes every 10 ms from `from` to `to` (inclusive), the worker free, the
// link as `link` has it: the first that hands `job` (UINT32_MAX: none);
// every pass before it waits for Bluetooth.
uint32_t firstStartLinked(S& s, uint32_t from, uint32_t to, S::In in, const Link& link, J job = J::Scan) {
  for (uint32_t t = from;; t += 10) {
    in.nowMs = t;
    link.set(in, t);
    const S::Out o = s.update(in);
    if (o.job == job) return t;
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    if (t == to) return UINT32_MAX;
  }
}

}  // namespace

// The wait for AVRCP (2026-11-19): a link holds the background work until
// its AVRCP is up, then btSettleMs (AVRCP's set-up, an SDP search 3.5 s after
// A2DP opens, took internal RAM to 35,916 B under the boot's walk); at most
// btControlWaitMs from the link, then btSettleMs; a link that drops ends it.
void test_the_wait_for_avrcp() {
  const uint32_t t = 45010;  // the link
  // AVRCP 3.8 s after the link (Powerbeats Pro, 3.73-3.80 s): the first
  // start at AVRCP + 3 s, not at the link + 3 s.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 3800;
    TEST_ASSERT_EQUAL_UINT32(t + 6800, firstStartLinked(s, t, t + 20000, base(t), l));
  }
  // Out::btControlWait: from the link to AVRCP, not after (the settle that
  // follows is the plain Bluetooth wait).
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 3800;
    for (uint32_t now = t - 10; now <= t + 7000; now += 10) {
      S::In in = base(now);
      l.set(in, now);
      const S::Out o = s.update(in);
      TEST_ASSERT_EQUAL(now >= t && now < t + 3800, o.btControlWait);
    }
  }
  // AVRCP never: the cap at the link + 10 s, then 3 s. The cap's settle is
  // the plain Bluetooth wait: the worker isn't released in it.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    TEST_ASSERT_EQUAL_UINT32(t + 13000, firstStartLinked(s, t, t + 30000, base(t), l));
    S s2;
    s2.update(base(0));
    for (uint32_t now = t; now < t + 13000; now += 10) {
      S::In in = base(now);
      l.set(in, now);
      const S::Out o = s2.update(in);
      TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
      TEST_ASSERT_EQUAL(now < t + 10000, o.btControlWait);
    }
    // ... and the link stays up without it: no wait again.
    S::In in = base(t + 20000);
    l.set(in, t + 20000);
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::Scan, o.job);
    TEST_ASSERT_FALSE(o.btControlWait);
  }
  // Sparse passes (the loop held): the cap's settle still ends at the link +
  // 13 s, and a first pass past it holds nothing.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    S::In in = base(t);
    l.set(in, t);
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
    in.nowMs = t + 12990;
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
    in.nowMs = t + 13000;
    TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    S s2;
    s2.update(base(0));
    in.nowMs = t;
    TEST_ASSERT_EQUAL(W::Bluetooth, s2.update(in).wait);
    in.nowMs = t + 25000;  // the next pass long after the cap
    TEST_ASSERT_EQUAL(J::Scan, s2.update(in).job);
  }
  // AVRCP up with the link (the same pass), or before it: the link's own
  // settle, as before the wait.
  for (int before = 0; before < 2; ++before) {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = before ? t - 1000 : t;
    l.tied = !before;
    for (uint32_t now = t - 1000; now < t; now += 10) {  // (before the link: nothing holds it)
      S::In in = base(now);
      l.set(in, now);
      TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    }
    TEST_ASSERT_EQUAL_UINT32(t + 3000, firstStartLinked(s, t, t + 20000, base(t), l));
  }
  // A later AVRCP (the same headphones, about 7 s once a stream had started
  // before it: a listener's PlayGate start): 3 s after it.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 7000;
    TEST_ASSERT_EQUAL_UINT32(t + 10000, firstStartLinked(s, t, t + 20000, base(t), l));
  }
  // AVRCP after the cap, while the cap's settle still holds: a link event
  // of its own, 3 s after it.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 11000;
    TEST_ASSERT_EQUAL_UINT32(t + 14000, firstStartLinked(s, t, t + 30000, base(t), l));
  }
  // The link drops before AVRCP: the drop's settle, as before (the wait
  // ends with the link). The next link waits for its own AVRCP, its cap
  // from its own rise.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.dropAt = t + 2000;
    TEST_ASSERT_EQUAL_UINT32(t + 5000, firstStartLinked(s, t, t + 20000, base(t), l));
    S::In in = base(t + 30000);
    in.btSeq = 3;  // up again at t + 30 s
    in.btLinked = true;
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    TEST_ASSERT_TRUE(o.btControlWait);
    uint32_t started = UINT32_MAX;
    for (uint32_t now = t + 30010; now <= t + 50000 && started == UINT32_MAX; now += 10) {
      in.nowMs = now;
      if (s.update(in).job == J::Scan) started = now;
    }
    TEST_ASSERT_EQUAL_UINT32(t + 43000, started);
  }
  // A link that comes mid-slice: the slice is cut at once (at 0), the worker
  // told the wait holds; the next is held to AVRCP + 3 s.
  {
    S s;
    S::In in = base(0);
    in.walk = true;
    in.running = J::Walk;
    s.update(in);
    Link l;
    l.linkAt = t;
    l.controlAt = t + 3750;
    in.nowMs = t;
    l.set(in, t);
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_TRUE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    TEST_ASSERT_TRUE(o.btControlWait);
    in.running = J::None;  // (the slice ended after its unit)
    TEST_ASSERT_EQUAL_UINT32(t + 6750, firstStartLinked(s, t + 10, t + 20000, in, l, J::Walk));
  }
  // AVRCP that drops and comes back mid-link, a scan slice running: its
  // coming back is a link event (it cuts the slice and holds the next 3 s),
  // not a wait for AVRCP (the link's wait ended with its first AVRCP).
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 3750;
    TEST_ASSERT_EQUAL_UINT32(t + 6750, firstStartLinked(s, t, t + 20000, base(t), l));
    S::In in = base(t + 20000);
    l.set(in, t + 20000);
    in.running = J::Scan;
    S::Out o = s.update(in);
    TEST_ASSERT_FALSE(o.cut);
    in.nowMs = t + 21000;
    in.btControlUp = false;  // AVRCP down, the link up
    o = s.update(in);
    TEST_ASSERT_FALSE(o.cut);  // (AVRCP's going isn't held)
    TEST_ASSERT_FALSE(o.btControlWait);
    in.nowMs = t + 25000;
    in.btControlUp = true;  // ... and back
    o = s.update(in);
    TEST_ASSERT_TRUE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    TEST_ASSERT_FALSE(o.btControlWait);
    in.running = J::None;  // (the slice ended after its unit)
    TEST_ASSERT_EQUAL_UINT32(t + 28000, firstStart(s, t + 25010, t + 40000, in));
  }
  // A drop and a relink between two passes (the loop held; BtSink counts
  // the flips, so btSeq moves by two while the link reads up at both): a
  // new link, whose wait for its own AVRCP starts again, even with the old
  // link's AVRCP seen up at the last pass.
  {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    l.controlAt = t + 3750;
    TEST_ASSERT_EQUAL_UINT32(t + 6750, firstStartLinked(s, t, t + 20000, base(t), l));
    S::In in = base(t + 20000);
    l.set(in, t + 20000);
    TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    in.nowMs = t + 30000;
    in.btSeq += 2;
    in.btControlUp = false;  // the new link's AVRCP not up yet
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    TEST_ASSERT_TRUE(o.btControlWait);
    uint32_t started = UINT32_MAX;
    for (uint32_t now = t + 30010; now <= t + 50000 && started == UINT32_MAX; now += 10) {
      in.nowMs = now;
      in.btControlUp = now >= t + 33750;
      if (s.update(in).job == J::Scan) started = now;
    }
    TEST_ASSERT_EQUAL_UINT32(t + 36750, started);
    // ... and one whose AVRCP was up by the next pass: the relink's settle.
    in.nowMs = t + 60000;
    in.btSeq += 2;
    o = s.update(in);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    TEST_ASSERT_FALSE(o.btControlWait);
    TEST_ASSERT_EQUAL_UINT32(t + 63000, firstStart(s, t + 60010, t + 70000, in));
  }
  // What the wait holds is what the Bluetooth wait holds: the save, the
  // walk, a compaction, the scan; not a waveform's read, a cover or the
  // update step's build.
  struct Work {
    const char* name;
    void (*set)(S::In&);
    J job;  // handed during the wait; None: held
  } work[] = {
      {"build", [](S::In& in) { in.build = true; }, J::Build},
      {"wave", [](S::In& in) { in.wave = true; }, J::Wave},
      {"cover", [](S::In& in) { in.cover = true; }, J::Cover},
      {"save", [](S::In& in) { in.save = true; }, J::None},
      {"walk", [](S::In& in) { in.walk = true; }, J::None},
      {"compaction", [](S::In& in) {
         in.compact = true;
         in.restPending = false;
       }, J::None},
      {"scan", [](S::In&) {}, J::None},
  };
  for (const Work& w : work) {
    S s;
    s.update(base(0));
    Link l;
    l.linkAt = t;
    S::In in = base(t);
    l.set(in, t);
    s.update(in);
    in.nowMs = t + 5000;  // (the link's own settle ended at t + 3 s: the wait for AVRCP alone)
    l.set(in, in.nowMs);
    w.set(in);
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL_MESSAGE(w.job, o.job, w.name);
    TEST_ASSERT_EQUAL_MESSAGE(w.job == J::None ? W::Bluetooth : W::None, o.wait, w.name);
  }
  // A cap of 0: no wait for AVRCP, the link's settle alone.
  {
    S::Config c;
    c.btControlWaitMs = 0;
    S s(c);
    s.update(base(0));
    Link l;
    l.linkAt = t;
    TEST_ASSERT_EQUAL_UINT32(t + 3000, firstStartLinked(s, t, t + 20000, base(t), l));
  }
}

namespace {

// The headphones as main.cpp's stepCard feeds them, the set-up too: the
// radio link being set up over the spans [from, to) (UINT32_MAX: never
// ends), BtSink's count moving at each rise and end; a page burst (btSetup)
// before `pagingUntil`; the link and its AVRCP as `link` has them.
struct Radio {
  uint32_t from[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  uint32_t to[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
  uint32_t pagingUntil = 0;
  Link link;
  void set(S::In& in, uint32_t t) const {
    link.set(in, t);
    in.btSetup = t < pagingUntil;
    uint32_t seq = 0;
    bool on = false;
    for (int i = 0; i < 3; ++i) {
      seq += (t >= from[i] ? 1u : 0u) + (t >= to[i] ? 1u : 0u);
      if (t >= from[i] && t < to[i]) on = true;
    }
    in.btLinkSetup = on;
    in.btLinkSetupSeq = seq;
  }
};

// Passes every 10 ms from `from` to `to` (inclusive), the worker free, the
// headphones as `radio` has them: the first that hands `job` (UINT32_MAX:
// none); every pass before it waits for Bluetooth.
uint32_t firstStartRadio(S& s, uint32_t from, uint32_t to, S::In in, const Radio& radio, J job = J::Scan) {
  for (uint32_t t = from;; t += 10) {
    in.nowMs = t;
    radio.set(in, t);
    const S::Out o = s.update(in);
    if (o.job == job) return t;
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    if (t == to) return UINT32_MAX;
  }
}

}  // namespace

// The radio link's set-up (2026-11-19): the headphones' radio link up with
// A2DP not yet over it holds the background work as a page burst does,
// then btSettleMs from its end; A2DP's coming up hands over to the wait for
// AVRCP with no pass between; at most btLinkSetupMs from its rise, then
// btSettleMs; a new radio link waits again. (On the device a walk the
// burst's settle released started about 0.15 s into one, and internal RAM
// went to 42,544 B, under G5's floor.)
void test_the_radio_links_set_up() {
  const uint32_t t = 45010;  // the radio link
  // Its end without A2DP (the radio link closed): 3 s after it. The wait's
  // flag from the rise to the end, not in the settle after it.
  {
    S s;
    s.update(base(0));
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 2000;
    TEST_ASSERT_EQUAL_UINT32(t + 5000, firstStartRadio(s, t, t + 20000, base(t), r));
    S s2;
    s2.update(base(0));
    for (uint32_t now = t - 10; now < t + 5000; now += 10) {
      S::In in = base(now);
      r.set(in, now);
      const S::Out o = s2.update(in);
      TEST_ASSERT_EQUAL(now >= t ? W::Bluetooth : W::None, o.wait);
      TEST_ASSERT_EQUAL(now >= t && now < t + 2000, o.btLinkSetupWait);
      TEST_ASSERT_FALSE(o.btControlWait);
    }
  }
  // A2DP and AVRCP in the same pass as the set-up's end (rd-g5-fix1's
  // second radio link: A2DP up about 0.6 s after it, AVRCP's lines in the
  // same burst): the link's own settle, from the link.
  {
    S s;
    s.update(base(0));
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 610;
    r.link.linkAt = t + 610;
    r.link.controlAt = t + 610;
    TEST_ASSERT_EQUAL_UINT32(t + 3610, firstStartRadio(s, t, t + 20000, base(t), r));
  }
  // ... and the loop reading the set-up still up at the link's first pass
  // (BtSink counts the link's rise first): its end a pass later holds 3 s
  // from that pass.
  {
    S s;
    s.update(base(0));
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 620;
    r.link.linkAt = t + 610;
    r.link.controlAt = t + 610;
    TEST_ASSERT_EQUAL_UINT32(t + 3620, firstStartRadio(s, t, t + 20000, base(t), r));
  }
  // A2DP, then AVRCP about 3.8 s later (rd-g5-fix2, its times on the
  // loop's 10 ms: the radio link at 10.02 s, A2DP at 11.92 s, AVRCP at
  // about 15.70 s; its log lines arrived at 15.77 s, behind the console's
  // D, and the walk on the device began at about 18.70 s): AVRCP + 3 s, and
  // no pass between the two waits: from the radio link to AVRCP one of the
  // two flags holds (the worker may go throughout).
  {
    const uint32_t acl = 10020, a2dp = 11920, avrcp = 15700;
    Radio r;
    r.from[0] = acl;
    r.to[0] = a2dp;
    r.link.linkAt = a2dp;
    r.link.controlAt = avrcp;
    S s;
    s.update(base(0));
    TEST_ASSERT_EQUAL_UINT32(avrcp + 3000, firstStartRadio(s, acl, acl + 20000, base(acl), r));
    S s2;
    s2.update(base(0));
    for (uint32_t now = acl; now < avrcp + 3000; now += 10) {
      S::In in = base(now);
      r.set(in, now);
      const S::Out o = s2.update(in);
      TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
      TEST_ASSERT_EQUAL(now < a2dp, o.btLinkSetupWait);
      TEST_ASSERT_EQUAL(now >= a2dp && now < avrcp, o.btControlWait);
      TEST_ASSERT_EQUAL(now < avrcp, S::mayRelease(o, true, false));
    }
  }
  // The cap: a radio link that never brings A2DP (rd-g5-fix1's first stayed
  // 30.3 s): the rise + 8 s, then 3 s; its end after the cap holds nothing.
  {
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 30300;
    S s;
    s.update(base(0));
    TEST_ASSERT_EQUAL_UINT32(t + 11000, firstStartRadio(s, t, t + 20000, base(t), r));
    S s2;
    s2.update(base(0));
    for (uint32_t now = t; now <= t + 40000; now += 10) {
      S::In in = base(now);
      r.set(in, now);
      const S::Out o = s2.update(in);
      TEST_ASSERT_EQUAL(now < t + 11000 ? W::Bluetooth : W::None, o.wait);
      TEST_ASSERT_EQUAL(now < t + 8000, o.btLinkSetupWait);
    }
  }
  // Sparse passes (the loop held): the cap's settle still ends at the rise +
  // 11 s, and a first pass past it holds nothing, whether the set-up still
  // reads up there or ended unseen.
  {
    Radio r;
    r.from[0] = t;
    S s;
    s.update(base(0));
    S::In in = base(t);
    r.set(in, t);
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
    in.nowMs = t + 10990;
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
    in.nowMs = t + 11000;
    TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    for (int ended = 0; ended < 2; ++ended) {
      S s2;
      s2.update(base(0));
      in = base(t);
      r.set(in, t);
      TEST_ASSERT_EQUAL(W::Bluetooth, s2.update(in).wait);
      in.nowMs = t + 25000;  // the next pass long after the cap
      if (ended) {
        in.btLinkSetup = false;
        ++in.btLinkSetupSeq;
      }
      const S::Out o = s2.update(in);
      TEST_ASSERT_EQUAL(J::Scan, o.job);
      TEST_ASSERT_FALSE(o.btLinkSetupWait);
    }
  }
  // A new rise after an end restarts the cap: rd-g5-fix1's two radio links
  // (the first never brought A2DP; the second came 2.86 s after it closed),
  // the second held from its own rise.
  {
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 30300;
    r.from[1] = t + 33160;
    S s;
    s.update(base(0));
    TEST_ASSERT_EQUAL_UINT32(t + 11000, firstStartRadio(s, t, t + 20000, base(t), r));
    S::In in = base(t + 11000);
    for (uint32_t now = t + 11000; now < t + 33160; now += 10) {
      in.nowMs = now;
      r.set(in, now);
      TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    }
    TEST_ASSERT_EQUAL_UINT32(t + 33160 + 11000, firstStartRadio(s, t + 33160, t + 60000, in, r));
  }
  // A new radio link over one never seen to close (BtSink counts two: the
  // set-up reads up at both passes), past the first one's cap: held again,
  // its cap from its own rise.
  {
    Radio r;
    r.from[0] = t;
    S s;
    s.update(base(0));
    TEST_ASSERT_EQUAL_UINT32(t + 11000, firstStartRadio(s, t, t + 20000, base(t), r));
    S::In in = base(t + 20000);
    r.set(in, t + 20000);
    TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    in.btLinkSetupSeq += 2;
    in.nowMs = t + 20010;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    TEST_ASSERT_TRUE(o.btLinkSetupWait);
    TEST_ASSERT_EQUAL_UINT32(t + 31010, firstStart(s, t + 20020, t + 40000, in));
  }
  // A set-up that came and went between two passes (the count moved by two,
  // down at both): the settle from the pass that sees it; the worker isn't
  // released (the wait is over).
  {
    S s;
    s.update(base(0));
    S::In in = base(t);
    in.btLinkSetupSeq = 2;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    TEST_ASSERT_FALSE(o.btLinkSetupWait);
    TEST_ASSERT_EQUAL_UINT32(t + 3000, firstStart(s, t + 10, t + 10000, in));
  }
  // ... and after a set-up read up past its cap at the last pass (a stuck
  // radio link, whose own end holds nothing): its end and a whole set-up
  // after it before the next pass (the count moved by three, the loop held
  // a while) hold the settle from that pass; its end alone (by one) holds
  // nothing.
  for (uint32_t by = 1; by <= 3; by += 2) {
    S s;
    s.update(base(0));
    S::In in = base(t);
    in.btLinkSetup = true;
    in.btLinkSetupSeq = 1;
    TEST_ASSERT_EQUAL(W::Bluetooth, s.update(in).wait);
    in.nowMs = t + 20000;  // past the cap and its settle, still up
    TEST_ASSERT_EQUAL(J::Scan, s.update(in).job);
    in.nowMs = t + 20010;
    in.btLinkSetup = false;
    in.btLinkSetupSeq = 1 + by;
    const S::Out o = s.update(in);
    TEST_ASSERT_FALSE(o.btLinkSetupWait);
    if (by == 1) {
      TEST_ASSERT_EQUAL(J::Scan, o.job);
    } else {
      TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
      TEST_ASSERT_EQUAL_UINT32(t + 23010, firstStart(s, t + 20020, t + 30000, in));
    }
  }
  // A radio link that comes mid-slice: the slice is cut at once (at 0) and
  // the wait's flag is set; the first pass with the slice ended releases
  // the worker (before the 3 s idle exit); the next is held to the set-up's
  // end + 3 s.
  {
    S s;
    S::In in = base(0);
    in.walk = true;
    in.running = J::Walk;
    s.update(in);
    Radio r;
    r.from[0] = t;
    r.to[0] = t + 1900;
    in.nowMs = t;
    r.set(in, t);
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_TRUE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    TEST_ASSERT_TRUE(o.btLinkSetupWait);
    TEST_ASSERT_FALSE(S::mayRelease(o, false, false));  // (the slice under way)
    in.running = J::None;  // (the slice ended after its unit)
    in.nowMs = t + 10;
    r.set(in, t + 10);
    const S::Out freed = s.update(in);
    TEST_ASSERT_EQUAL(J::None, freed.job);
    TEST_ASSERT_TRUE(S::mayRelease(freed, true, false));
    TEST_ASSERT_EQUAL_UINT32(t + 4900, firstStartRadio(s, t + 20, t + 20000, in, r, J::Walk));
  }
  // What it holds is what the Bluetooth wait holds: the save, the walk, a
  // compaction, the scan; not a waveform's read, a cover or the update
  // step's build.
  struct Work {
    const char* name;
    void (*set)(S::In&);
    J job;  // handed during the set-up; None: held
  } work[] = {
      {"build", [](S::In& in) { in.build = true; }, J::Build},
      {"wave", [](S::In& in) { in.wave = true; }, J::Wave},
      {"cover", [](S::In& in) { in.cover = true; }, J::Cover},
      {"save", [](S::In& in) { in.save = true; }, J::None},
      {"walk", [](S::In& in) { in.walk = true; }, J::None},
      {"compaction", [](S::In& in) {
         in.compact = true;
         in.restPending = false;
       }, J::None},
      {"scan", [](S::In&) {}, J::None},
  };
  for (const Work& w : work) {
    S s;
    s.update(base(0));
    Radio r;
    r.from[0] = t;
    S::In in = base(t);
    r.set(in, t);
    s.update(in);
    in.nowMs = t + 5000;  // (past a settle from the rise: the set-up alone)
    r.set(in, in.nowMs);
    w.set(in);
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL_MESSAGE(w.job, o.job, w.name);
    TEST_ASSERT_EQUAL_MESSAGE(w.job == J::None ? W::Bluetooth : W::None, o.wait, w.name);
    TEST_ASSERT_TRUE_MESSAGE(o.btLinkSetupWait, w.name);
  }
  // A cap of 0: the rise's settle alone.
  {
    S::Config c;
    c.btLinkSetupMs = 0;
    S s(c);
    s.update(base(0));
    Radio r;
    r.from[0] = t;
    TEST_ASSERT_EQUAL_UINT32(t + 3000, firstStartRadio(s, t, t + 20000, base(t), r));
  }
}

// rd-g5-fix1 (2026-11-19), its times on the loop's 10 ms: the boot's burst
// pages from 6.36 s (page 1 unanswered at 11.48 s); the headphones open a
// radio link at 12.83 s that never brings A2DP (pages 2 and 3 go over it,
// at 17.08 s and 36.34 s) and closes 30.3 s later, at 43.17 s, as the burst
// backs off; they open another at 46.03 s, and A2DP and AVRCP come up
// together at 46.64 s. Before the set-up's wait the walk the burst held
// started 3 s after the back-off, 0.13 s into the second radio link (about
// 0.15 s on the device, the walk's first slice at about 46.18 s), and met
// the incoming set-up (internal RAM to 42,544 B). Now it waits for the
// link + 3 s.
void test_the_set_up_after_a_back_off() {
  Radio r;
  r.pagingUntil = 43170;
  r.from[0] = 12830;
  r.to[0] = 43170;
  r.from[1] = 46030;
  r.to[1] = 46640;
  r.link.linkAt = 46640;
  r.link.controlAt = 46640;
  S::In in = base(6360);
  in.walk = true;
  in.restPending = false;
  // As before the set-up's wait (the glue without it): 3 s after the back-off.
  {
    S s;
    s.update(base(0));
    uint32_t started = UINT32_MAX;
    for (uint32_t now = 6360; now <= 60000 && started == UINT32_MAX; now += 10) {
      S::In at = in;
      at.nowMs = now;
      r.set(at, now);
      at.btLinkSetup = false;
      at.btLinkSetupSeq = 0;
      if (s.update(at).job == J::Walk) started = now;
    }
    TEST_ASSERT_EQUAL_UINT32(46160, started);
    TEST_ASSERT_TRUE(started > r.from[1] && started < r.to[1]);  // inside the second set-up
  }
  // Now, the set-up as BtSink counts it (RadioLinkNotes, fed what BtAppT
  // saw; the link's rise counted before the set-up's end): the link + 3 s.
  // Pages 2 and 3 over the first radio link are set-ups again, each its own
  // cap. The worker may go during each set-up's wait (the first radio
  // link's from 12.83 s, page 2's to its cap at 25.08 s, page 3's to the
  // radio link's close; the burst's pages alone between them don't release
  // it), and during the second radio link's.
  RadioLinkNotes notes;
  const uint64_t them = 0x0011223344AAull;
  S s;
  s.update(base(0));
  for (uint32_t now = 6360; now < 49640; now += 10) {
    if (now == 11480) notes.opened(them, false, true, now);  // page 1: no answer (HCI 0x04)
    if (now == 12830 || now == 46030) notes.opened(them, true, false, now);
    if (now == 17080 || now == 36340) notes.paged(them);  // pages 2 and 3, over it
    if (now == 43170) notes.closed(them);                 // HCI 0x08, no A2DP
    in.nowMs = now;
    r.link.set(in, now);
    if (now == r.link.linkAt) notes.linkUp(them);
    in.btSetup = now < r.pagingUntil;
    in.btLinkSetup = notes.settingUp();
    in.btLinkSetupSeq = notes.count();
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
    const bool waits = (now >= 12830 && now < 25080) || (now >= 36340 && now < 43170) || (now >= 46030 && now < 46640);
    TEST_ASSERT_EQUAL(waits, o.btLinkSetupWait);
    TEST_ASSERT_EQUAL(waits, S::mayRelease(o, true, false));
  }
  TEST_ASSERT_EQUAL_UINT32(8, notes.count());
  in.nowMs = 49640;
  r.link.set(in, 49640);
  TEST_ASSERT_EQUAL(J::Walk, s.update(in).job);
}

// When the card worker's idle task may end at once (mayRelease(), app/
// CardTasks: CardWorker::release()): only in the link's wait for AVRCP or a
// radio link's set-up (each capped), with the worker free, nothing handed
// that pass and no update step asked. Not in a page burst's or a pairing's
// wait (each cover would end and remake the task for tens of seconds), nor
// in the settle after AVRCP, after a set-up's end or after either cap.
void test_when_the_worker_may_go() {
  const uint32_t t = 45010;  // the link
  auto at = [](S& s, uint32_t now, void (*set)(S::In&)) {
    S::In in = base(now);
    set(in);
    return s.update(in);
  };
  S s;
  s.update(base(0));
  // A page burst (unlinked): the Bluetooth wait, not the link's.
  S::Out o = at(s, t - 1000, [](S::In& in) { in.btSetup = true; });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
  // The link, AVRCP not up: the scan held, nothing handed.
  o = at(s, t, [](S::In& in) {
    in.btLinked = true;
    in.btSeq = 1;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_TRUE(S::mayRelease(o, true, false));
  TEST_ASSERT_FALSE(S::mayRelease(o, false, false));  // a step under way, or gs holding it
  TEST_ASSERT_FALSE(S::mayRelease(o, true, true));    // an update step asked
  // A cover, a waveform's read or the build handed in the wait: not then.
  void (*handed[])(S::In&) = {
      [](S::In& in) {
        in.btLinked = true;
        in.btSeq = 1;
        in.cover = true;
      },
      [](S::In& in) {
        in.btLinked = true;
        in.btSeq = 1;
        in.wave = true;
      },
      [](S::In& in) {
        in.btLinked = true;
        in.btSeq = 1;
        in.build = true;
      },
  };
  uint32_t now = t + 1000;
  for (auto set : handed) {
    o = at(s, now, set);
    TEST_ASSERT_TRUE(o.btControlWait);
    TEST_ASSERT_TRUE(o.job != J::None);
    TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
    now += 10;
  }
  // A pairing on the link, its AVRCP up: the Bluetooth wait, not the link's.
  o = at(s, t + 4000, [](S::In& in) {
    in.btLinked = true;
    in.btSeq = 1;
    in.btControlUp = true;
    in.btSetup = true;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
  // The settle after AVRCP.
  o = at(s, t + 6000, [](S::In& in) {
    in.btLinked = true;
    in.btSeq = 1;
    in.btControlUp = true;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
  // The cap's settle (another link, AVRCP never).
  S s2;
  s2.update(base(0));
  o = at(s2, t, [](S::In& in) {
    in.btLinked = true;
    in.btSeq = 1;
  });
  TEST_ASSERT_TRUE(S::mayRelease(o, true, false));
  o = at(s2, t + 11000, [](S::In& in) {
    in.btLinked = true;
    in.btSeq = 1;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));

  // A radio link being set up during a page burst (a page of ours
  // answered): the set-up's wait, so it may go; not with a step under way
  // or an update step asked.
  S s3;
  s3.update(base(0));
  o = at(s3, t - 2000, [](S::In& in) { in.btSetup = true; });
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
  o = at(s3, t, [](S::In& in) {
    in.btSetup = true;
    in.btLinkSetup = true;
    in.btLinkSetupSeq = 1;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_TRUE(o.btLinkSetupWait);
  TEST_ASSERT_FALSE(o.btControlWait);
  TEST_ASSERT_TRUE(S::mayRelease(o, true, false));
  TEST_ASSERT_FALSE(S::mayRelease(o, false, false));
  TEST_ASSERT_FALSE(S::mayRelease(o, true, true));
  // A cover, a waveform's read or the build handed in it: not then.
  void (*inSetup[])(S::In&) = {
      [](S::In& in) {
        in.btLinkSetup = true;
        in.btLinkSetupSeq = 1;
        in.cover = true;
      },
      [](S::In& in) {
        in.btLinkSetup = true;
        in.btLinkSetupSeq = 1;
        in.wave = true;
      },
      [](S::In& in) {
        in.btLinkSetup = true;
        in.btLinkSetupSeq = 1;
        in.build = true;
      },
  };
  now = t + 1000;
  for (auto set : inSetup) {
    o = at(s3, now, set);
    TEST_ASSERT_TRUE(o.btLinkSetupWait);
    TEST_ASSERT_TRUE(o.job != J::None);
    TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
    now += 10;
  }
  // Past its cap, the burst's pages still on: the page's wait alone.
  o = at(s3, t + 8000, [](S::In& in) {
    in.btSetup = true;
    in.btLinkSetup = true;
    in.btLinkSetupSeq = 1;
  });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(o.btLinkSetupWait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
  // The settle after a set-up's end (another radio link, closed at + 1 s).
  S s4;
  s4.update(base(0));
  o = at(s4, t, [](S::In& in) {
    in.btLinkSetup = true;
    in.btLinkSetupSeq = 1;
  });
  TEST_ASSERT_TRUE(S::mayRelease(o, true, false));
  o = at(s4, t + 1000, [](S::In& in) { in.btLinkSetupSeq = 2; });
  TEST_ASSERT_EQUAL(W::Bluetooth, o.wait);
  TEST_ASSERT_FALSE(S::mayRelease(o, true, false));
}

// Input: each touch holds it 0.5 s; a moving list holds everything (covers
// and waveforms' reads too) for as long as it moves.
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
  in.wave = true;
  in.save = true;
  for (uint32_t t = 1000; t <= 9000; t += 10) {
    in.nowMs = t;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::List, o.wait);
  }
  in.listMoving = false;
  in.nowMs = 9010;
  TEST_ASSERT_EQUAL(J::Wave, s.update(in).job);  // (no quiet time after a list)
  in.wave = false;
  in.nowMs = 9020;
  TEST_ASSERT_EQUAL(J::Cover, s.update(in).job);  // (nor for a cover)
  // A waveform's read alone, with nothing else asked, is held the same.
  S w;
  S::In one = base(0);
  one.restPending = false;
  one.wave = true;
  one.listMoving = true;
  TEST_ASSERT_EQUAL(W::List, w.update(one).wait);
  one.listMoving = false;
  one.nowMs = 10;
  TEST_ASSERT_EQUAL(J::Wave, w.update(one).job);
}

// A list "moving" while the screen is dark holds nothing (listHolds()):
// none is seen moving then, whatever its page owes the wake. On 2026-10-14
// the library update went live with the screen off on Library > Albums;
// the list, reloaded in the dark, owed its frame (ListView::animating())
// until the screen woke, and the save of library.idx waited for "a list
// moving" 260 s (built 2055.0, the wake 2314.6, saved 2319.9). The same
// save with the same list in the dark: handed at once, level with the
// loop; the background work too, and a slice under way isn't cut.
void test_a_dark_screen_holds_nothing_for_a_list() {
  // The device's case: the update step live (its save asked, the worker
  // held for it), nothing playing, the screen off, the list "animating".
  S s;
  S::In in = base(0);
  in.playing = false;
  in.restPending = false;
  in.updating = true;
  in.save = true;
  in.dark = true;
  in.listMoving = true;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, firstStart(s, 0, 260000, in, J::Save),
                                   "the save in the dark, a list owing its frame");
  in.nowMs = 10;
  in.running = J::Save;
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(W::Step, o.wait);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);  // (dark: level with the loop)
  TEST_ASSERT_EQUAL_UINT64(0, s.waitedMs(W::List));
  // Lit, the same list moving: the save waits for it (frames first), and
  // starts once it settles.
  S lit;
  in.running = J::None;
  in.dark = false;
  W seen = W::None;
  TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, firstStart(lit, 0, 10000, in, J::Save, &seen));
  TEST_ASSERT_EQUAL(W::List, seen);
  TEST_ASSERT_TRUE(lit.waitedMs(W::List) >= 9990);
  in.listMoving = false;
  TEST_ASSERT_EQUAL_UINT32(10010, firstStart(lit, 10010, 20000, in, J::Save));
  // The background work in the dark, the list "moving": the walk, a
  // compaction, the scan, each handed; a slice under way not cut, at the
  // dark's priority; the screen lit, the same list cuts it.
  S bg;
  S::In b = base(0);
  b.dark = true;
  b.listMoving = true;
  b.walk = true;
  o = bg.update(b);
  TEST_ASSERT_EQUAL(J::Walk, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  b.walk = false;
  b.compact = true;
  b.nowMs = 10;
  o = bg.update(b);
  TEST_ASSERT_EQUAL(J::Compact, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  b.compact = false;
  b.nowMs = 20;
  o = bg.update(b);
  TEST_ASSERT_EQUAL(J::Scan, o.job);
  TEST_ASSERT_EQUAL(Src::Rest, o.source);
  for (J running : {J::Walk, J::Scan}) {
    b.running = running;
    b.dark = true;
    b.nowMs += 10;
    o = bg.update(b);
    TEST_ASSERT_FALSE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    b.dark = false;
    b.nowMs += 10;
    o = bg.update(b);
    TEST_ASSERT_TRUE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  }
  TEST_ASSERT_EQUAL_UINT64(0, bg.waitedMs(W::List));
  // A waveform's read and a cover go by the same rule: handed in the dark, level
  // with the loop, the list "moving"; held by it once the screen is lit, and
  // below the loop if already under way.
  for (J job : {J::Wave, J::Cover}) {
    S lane;
    S::In l = base(0);
    l.restPending = false;
    l.wave = job == J::Wave;
    l.cover = job == J::Cover;
    l.dark = true;
    l.listMoving = true;
    o = lane.update(l);
    TEST_ASSERT_EQUAL(job, o.job);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    TEST_ASSERT_EQUAL(W::None, o.wait);
    l.running = job;
    l.nowMs = 10;
    o = lane.update(l);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    l.dark = false;
    l.nowMs = 20;
    o = lane.update(l);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    l.running = J::None;
    l.nowMs = 30;
    o = lane.update(l);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::List, o.wait);
  }
  // The rule.
  TEST_ASSERT_FALSE(S::listHolds(false, false));
  TEST_ASSERT_TRUE(S::listHolds(true, false));
  TEST_ASSERT_FALSE(S::listHolds(true, true));
  TEST_ASSERT_FALSE(S::listHolds(false, true));
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

// U13: below 10% off USB the scan pauses; 10-15% keeps the state; above
// 15%, or USB, lets it go. A reading not known keeps it. The rest doesn't
// stop for it.
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
  // Held: the walk, a compaction, the save, a cover, a waveform's read and the
  // build go on.
  in.usb = false;
  s.update(in);
  TEST_ASSERT_TRUE(s.batteryLow());
  TEST_ASSERT_EQUAL(W::Battery, s.update(in).wait);
  struct {
    bool S::In::*flag;
    J job;
  } go[] = {{&S::In::walk, J::Walk}, {&S::In::compact, J::Compact}, {&S::In::save, J::Save},
            {&S::In::cover, J::Cover}, {&S::In::wave, J::Wave},      {&S::In::build, J::Build}};
  for (const auto& g : go) {
    S::In one = in;
    one.*(g.flag) = true;
    o = s.update(one);
    TEST_ASSERT_EQUAL(g.job, o.job);
  }
  // The playing track's file is the scan's too: held.
  in.restPending = false;
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
  in.cover = in.save = in.walk = in.compact = true;
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

// A waveform's read is in the covers' lane, and takes it right before a cover:
// whatever the audio, at the loop's priority; a moving list holds it, and a
// build goes first. It is work by itself, and no audio's yield waits for it.
void test_a_waves_read_comes_before_a_cover() {
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
  in.wave = in.cover = in.save = in.walk = in.compact = true;
  in.playingPending = true;
  S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::Wave, o.job);
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  TEST_ASSERT_EQUAL(W::None, o.wait);
  TEST_ASSERT_EQUAL(Src::None, o.source);
  // A build asked with it goes first.
  in.build = true;
  TEST_ASSERT_EQUAL(J::Build, s.update(in).job);
  in.build = false;
  // A moving list: nothing starts.
  in.listMoving = true;
  o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::List, o.wait);
  // Under way when a list starts moving: below the loop until it stops, and it
  // isn't cut (it isn't a slice).
  in.running = J::Wave;
  o = s.update(in);
  TEST_ASSERT_EQUAL(W::Step, o.wait);
  TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  TEST_ASSERT_FALSE(o.cut);
  in.listMoving = false;
  TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, s.update(in).priority);
  // Done: the cover next, then the save held by the audio (here the input).
  in.running = J::None;
  in.wave = false;
  TEST_ASSERT_EQUAL(J::Cover, s.update(in).job);
  in.cover = false;
  TEST_ASSERT_EQUAL(W::Input, s.update(in).wait);

  // Asked with nothing else to do: it starts. Not asked: nothing does.
  S alone;
  S::In a = base(0);
  a.restPending = false;
  TEST_ASSERT_EQUAL(J::None, alone.update(a).job);
  a.wave = true;
  a.nowMs = 10;
  TEST_ASSERT_EQUAL(J::Wave, alone.update(a).job);
  a.wave = false;
  a.nowMs = 20;
  o = alone.update(a);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(W::None, o.wait);

  // While the update step holds the worker it still goes, as a cover does: the
  // read is its own, and nothing it reads moves under it.
  S held;
  S::In h = base(0);
  h.updating = true;
  TEST_ASSERT_EQUAL(W::Updating, held.update(h).wait);
  h.wave = true;
  h.nowMs = 10;
  TEST_ASSERT_EQUAL(J::Wave, held.update(h).job);

  // Its own line in the steps' count.
  S count;
  count.stepDone(J::Wave, 18);
  count.stepDone(J::Wave, 24);
  count.stepDone(J::Cover, 300);
  TEST_ASSERT_EQUAL_UINT32(2, count.steps(J::Wave));
  TEST_ASSERT_EQUAL_UINT32(21, count.meanStepMs(J::Wave));
  TEST_ASSERT_EQUAL_UINT32(24, count.maxStepMs(J::Wave));
  TEST_ASSERT_EQUAL_UINT32(2, count.units(J::Wave));
  TEST_ASSERT_EQUAL_UINT32(1, count.steps(J::Cover));
  count.resetStats();
  TEST_ASSERT_EQUAL_UINT32(0, count.steps(J::Wave));
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
  in.updating = in.build = in.cover = in.wave = true;
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
  // and a waveform's read don't.
  in = base(9000);
  in.updating = true;
  in.battery = 2;
  in.underruns = 5;
  TEST_ASSERT_EQUAL(W::Updating, s.update(in).wait);
  in.nowMs = 9010;
  in.cover = true;
  TEST_ASSERT_EQUAL(J::Cover, s.update(in).job);
  in.cover = false;
  in.wave = true;
  TEST_ASSERT_EQUAL(J::Wave, s.update(in).job);
  in.wave = false;
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

// One job at a time, the first with work: the walk, a compaction, the scan;
// the scan's file from 3.3.3's sources in order.
void test_the_order_of_the_work() {
  S s;
  S::In in = base(0);
  in.walk = in.compact = true;
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
  const S::Out o = s.update(in);
  TEST_ASSERT_EQUAL(J::None, o.job);
  TEST_ASSERT_EQUAL(Src::None, o.source);
  TEST_ASSERT_EQUAL(W::None, o.wait);
}

// millis() wraps after 49.7 days: a window across the wrap ends on time,
// and an old one never comes back (a device on USB for weeks); the radio
// link's set-up and the wait for AVRCP end at their caps across it, and a
// set-up or a link that stays up for weeks never waits again.
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
  // A radio link's set-up and a link waiting for its AVRCP, each rising 4 s
  // before the wrap and never ending, a pass every 10 ms: the wait's flag
  // for exactly its cap (8 s, 10 s), the scan at the cap + 3 s.
  for (int link = 0; link < 2; ++link) {
    S u;
    S::In w = base(0xFFFFE000u);
    u.update(w);
    const uint32_t rise = 0xFFFFF000u, capMs = link ? 10000u : 8000u;
    if (link) {
      w.btLinked = true;
      w.btSeq = 1;
    } else {
      w.btLinkSetup = true;
      w.btLinkSetupSeq = 1;
    }
    uint32_t scanAt = UINT32_MAX;
    for (uint32_t i = 0; i <= 2000 && scanAt == UINT32_MAX; ++i) {
      w.nowMs = rise + i * 10;
      const S::Out o = u.update(w);
      TEST_ASSERT_EQUAL(i * 10 < capMs, link ? o.btControlWait : o.btLinkSetupWait);
      TEST_ASSERT_EQUAL(i * 10 < capMs, S::mayRelease(o, true, false));
      if (o.job == J::Scan) scanAt = w.nowMs;
    }
    TEST_ASSERT_EQUAL_UINT32(rise + capMs + 3000u, scanAt);
  }
  // ... and up for 60 days from day 1, a pass every 10 s: held at the rise's
  // pass and the next (the cap's settle) only, the wait's flag at the rise's
  // alone, never waiting again across millis()' wrap.
  for (int link = 0; link < 2; ++link) {
    S u;
    S::In w = base(5);
    u.update(w);
    uint64_t heldPasses = 0, waits = 0;
    for (uint64_t ms = 86400000ull; ms < 60ull * 86400000ull; ms += 10000) {
      w.nowMs = static_cast<uint32_t>(ms);
      if (link) {
        w.btLinked = true;
        w.btSeq = 1;
      } else {
        w.btLinkSetup = true;
        w.btLinkSetupSeq = 1;
      }
      const S::Out o = u.update(w);
      if (o.job != J::Scan) ++heldPasses;
      if (o.btLinkSetupWait || o.btControlWait) ++waits;
    }
    TEST_ASSERT_EQUAL_UINT64(2, heldPasses);
    TEST_ASSERT_EQUAL_UINT64(1, waits);
  }
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

// The listening stats' lanes (6.8): the track's read beside a waveform's,
// after it and before a cover, at 1 (0 while a list moves under it), held
// by a moving list but not by the audio's yields; the rank as background
// work, after the walk and a compaction and before the scan, never under
// the update step, a moving list or any yield, never before a waveform's
// read or a cover, the battery floor not holding it, at 0 while lit and 1
// while dark, and under way never cut, dropping to 0 the pass the screen
// lights.
void test_the_listening_stats_lanes() {
  // The track's read: after the waveform's, before a cover.
  {
    S s;
    S::In in = base(0);
    in.restPending = false;
    in.wave = in.statsTrack = in.cover = true;
    in.statsRank = true;
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::Wave, o.job);
    in.wave = false;
    o = s.update(in);
    TEST_ASSERT_EQUAL(J::Stats, o.job);
    TEST_ASSERT_FALSE(o.statsRank);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    in.statsTrack = false;
    o = s.update(in);
    TEST_ASSERT_EQUAL(J::Cover, o.job);
    in.cover = false;
    o = s.update(in);
    TEST_ASSERT_EQUAL(J::Stats, o.job);  // the rank, last
    TEST_ASSERT_TRUE(o.statsRank);
  }
  // The track's read under the audio's yields: it starts; a moving list
  // holds it; under way, 0 while a list moves.
  {
    S s;
    S::In in = base(0);
    in.statsTrack = true;
    in.input = true;
    ++in.trackSeq;
    s.update(in);  // (primed)
    in.underruns = 3;
    in.nowMs = 10;
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::Stats, o.job);
    TEST_ASSERT_FALSE(o.statsRank);
    in.listMoving = true;
    o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::List, o.wait);
    in.running = J::Stats;
    o = s.update(in);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    TEST_ASSERT_FALSE(o.cut);
    in.listMoving = false;
    o = s.update(in);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
  }
  // The rank: the work's order (the walk, a compaction, the rank, the scan).
  {
    S s;
    S::In in = base(0);
    in.walk = in.compact = in.statsRank = true;
    const J jobs[] = {J::Walk, J::Compact, J::Stats, J::Scan};
    for (J j : jobs) {
      const S::Out o = s.update(in);
      TEST_ASSERT_EQUAL(j, o.job);
      TEST_ASSERT_EQUAL(j == J::Stats, o.statsRank);
      if (j == J::Walk) in.walk = false;
      if (j == J::Compact) in.compact = false;
      if (j == J::Stats) in.statsRank = false;
    }
  }
  // The update step holds it (and the save of library.idx comes first); a
  // moving list; each yield, for its window.
  {
    S s;
    S::In in = base(0);
    in.restPending = false;
    in.statsRank = true;
    in.updating = true;
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::None, o.job);
    TEST_ASSERT_EQUAL(W::Updating, o.wait);
    in.save = true;
    o = s.update(in);
    TEST_ASSERT_EQUAL(J::Save, o.job);
    in.save = in.updating = false;
    in.listMoving = true;
    o = s.update(in);
    TEST_ASSERT_EQUAL(W::List, o.wait);
    in.listMoving = false;
    TEST_ASSERT_EQUAL(J::Stats, s.update(in).job);
  }
  {
    // Each yield holds it until its window ends, as it holds the scan.
    struct Case {
      const char* name;
      W wait;
      uint32_t ms;
    };
    const S::Config c;
    const Case cases[] = {{"input", W::Input, c.inputQuietMs},       {"underrun", W::Underrun, c.underrunBackoffMs},
                          {"pass", W::DecodePass, c.passBackoffMs},   {"track", W::TrackChange, c.trackSettleMs},
                          {"seek", W::Seek, c.seekSettleMs},          {"bt", W::Bluetooth, c.btSettleMs}};
    for (const Case& k : cases) {
      S s;
      S::In in = base(1000);
      in.restPending = false;
      in.statsRank = true;
      s.update(in);
      in.nowMs = 2000;
      if (k.wait == W::Input) in.input = true;
      if (k.wait == W::Underrun) ++in.underruns;
      if (k.wait == W::DecodePass) in.decodePassUs = 50000;
      if (k.wait == W::TrackChange) ++in.trackSeq;
      if (k.wait == W::Seek) ++in.seekSeq;
      if (k.wait == W::Bluetooth) ++in.btSeq;
      S::Out o = s.update(in);
      TEST_ASSERT_EQUAL_MESSAGE(k.wait, o.wait, k.name);
      TEST_ASSERT_EQUAL_MESSAGE(J::None, o.job, k.name);
      in.input = false;
      in.decodePassUs = 0;
      W seen = W::None;
      const uint32_t at = firstStart(s, 2010, 2000 + k.ms + 100, in, J::Stats, &seen);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(2000 + k.ms, at, k.name);
    }
    // The ring below half.
    S s;
    S::In in = base(0);
    in.restPending = false;
    in.statsRank = true;
    in.ringMs = kRing / 3;
    TEST_ASSERT_EQUAL(W::Ring, s.update(in).wait);
  }
  // The battery floor holds the scan, not the rank.
  {
    S s;
    S::In in = base(0);
    in.battery = 5;
    in.statsRank = true;
    S::Out o = s.update(in);
    TEST_ASSERT_TRUE(s.batteryLow());
    TEST_ASSERT_EQUAL(J::Stats, o.job);
    in.statsRank = false;
    TEST_ASSERT_EQUAL(W::Battery, s.update(in).wait);
  }
  // Its priority: 0 while lit, 1 while dark; under way never cut, 0 the
  // pass the screen lights, whatever waits.
  {
    S s;
    S::In in = base(0);
    in.restPending = false;
    in.statsRank = true;
    S::Out o = s.update(in);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    in.dark = true;
    o = s.update(in);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, S::rankPriority(false, true));
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, S::rankPriority(false, false));
    in.running = J::Stats;
    in.runningRank = true;
    in.input = true;
    in.nowMs = 10;
    o = s.update(in);
    TEST_ASSERT_EQUAL(W::Step, o.wait);
    TEST_ASSERT_FALSE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kHighPriority, o.priority);
    in.dark = false;
    o = s.update(in);
    TEST_ASSERT_FALSE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
    in.listMoving = true;
    o = s.update(in);
    TEST_ASSERT_FALSE(o.cut);
    TEST_ASSERT_EQUAL_UINT8(S::kLowPriority, o.priority);
  }
  // A track's read and a rank asked, nothing else: the read first.
  {
    S s;
    S::In in = base(0);
    in.restPending = false;
    in.statsRank = in.statsTrack = true;
    const S::Out o = s.update(in);
    TEST_ASSERT_EQUAL(J::Stats, o.job);
    TEST_ASSERT_FALSE(o.statsRank);
  }
}

void test_names() {
  TEST_ASSERT_EQUAL_INT(9, S::kJobs);
  TEST_ASSERT_EQUAL_STRING("stats", S::jobName(J::Stats));
  TEST_ASSERT_EQUAL_STRING("scan", S::jobName(J::Scan));
  TEST_ASSERT_EQUAL_STRING("compaction", S::jobName(J::Compact));
  TEST_ASSERT_EQUAL_STRING("wave", S::jobName(J::Wave));
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
  // The wait for AVRCP: the link since linkAt, the wait holding, AVRCP at
  // the last pass.
  bool linked = false, controlWait = false, controlWas = false;
  int64_t linkAt = 0;
  uint32_t caps = 0;  // the waits the cap ended
  // The radio link's set-up: seen at the last pass, since setupAt, the
  // wait holding; its count at the last pass.
  bool setup = false, setupWait = false;
  int64_t setupAt = 0;
  uint32_t setupSeq = 0;
  uint32_t setupCaps = 0, setupEnds = 0;  // the set-up waits the cap ended, and those that ended within it

  static bool within(int64_t t, int64_t at, uint32_t windowMs) { return t - at < static_cast<int64_t>(windowMs); }

  S::Out step(const S::In& in, int64_t t) {
    if (!primed) {
      underruns = in.underruns;
      trackSeq = in.trackSeq;
      seekSeq = in.seekSeq;
      btSeq = in.btSeq;
      controlWas = in.btControlUp;
      setupSeq = in.btLinkSetupSeq;
      primed = true;
    }
    if (in.input) inputAt = t;
    if (in.underruns != underruns) underrunAt = t;
    if (in.decodePassUs > c.passLimitUs) passAt = t;
    if (in.decoderAtEnd || in.trackSeq != trackSeq) trackAt = t;
    if (in.seeking || in.seekSeq != seekSeq) seekAt = t;
    if (in.btSetup || in.btSeq != btSeq) btAt = t;
    // A radio link being set up holds as a page does, to its cap, and its
    // end within that is an event; the cap is one, at its time. A new one:
    // a rise, or the count moved while up at both passes. Down now: the
    // count's moves are the end of the one up at the last pass (if it was)
    // and then whole set-ups, rise and end, between the passes; any of
    // those is an event.
    const bool setupMoved = in.btLinkSetupSeq != setupSeq;
    if (in.btLinkSetup && (!setup || setupMoved)) {
      setupAt = t;
      setupWait = true;
    }
    if (!in.btLinkSetup) {
      const uint32_t moved = in.btLinkSetupSeq - setupSeq, ends = setup ? 1u : 0u;
      if (moved >= ends && (moved - ends) / 2 > 0) btAt = t;
    }
    setup = in.btLinkSetup;
    setupSeq = in.btLinkSetupSeq;
    if (setupWait && t - setupAt >= c.btLinkSetupMs) {
      setupWait = false;
      ++setupCaps;
      if (setupAt + c.btLinkSetupMs > btAt) btAt = setupAt + c.btLinkSetupMs;
    } else if (setupWait) {
      btAt = t;
      if (!in.btLinkSetup) {
        setupWait = false;
        ++setupEnds;
      }
    }
    // A link without AVRCP holds as a page does, to the cap; AVRCP's
    // coming up (linked) is a link event, and so is the cap, at its time.
    if (!in.btLinked) {
      linked = controlWait = false;
    } else {
      if (!linked || in.btSeq != btSeq) {  // a rise, or a drop and a relink between passes
        linked = true;
        linkAt = t;
        controlWait = !in.btControlUp;
      }
      if (in.btControlUp) {
        if (!controlWas) btAt = t;
        controlWait = false;
      }
      if (controlWait && t - linkAt < c.btControlWaitMs) {
        btAt = t;
      } else if (controlWait) {
        controlWait = false;
        ++caps;
        if (linkAt + c.btControlWaitMs > btAt) btAt = linkAt + c.btControlWaitMs;
      }
    }
    controlWas = in.btControlUp;
    underruns = in.underruns;
    trackSeq = in.trackSeq;
    seekSeq = in.seekSeq;
    btSeq = in.btSeq;
    S::Out o;
    o.btControlWait = controlWait;
    o.btLinkSetupWait = setupWait;
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
    // A list moves only on a lit screen.
    const bool moving = in.listMoving && !in.dark;
    if (in.running != J::None) {
      o.wait = W::Step;
      const bool slice = in.running == J::Walk || in.running == J::Scan;
      o.cut = slice && (moving || yield != W::None || (in.running == J::Scan && low));
      const bool rank = in.running == J::Stats && in.runningRank;
      if (o.cut) o.priority = 0;
      else if (in.running == J::Build) o.priority = 1;
      else if (rank) o.priority = in.dark ? 1 : 0;
      else if (in.running == J::Cover || in.running == J::Wave || in.running == J::Walk || in.running == J::Stats)
        o.priority = moving ? 0 : 1;
      else if (in.running == J::Scan || in.running == J::Compact || in.running == J::Save)
        o.priority = in.dark ? 1 : 0;
      else o.priority = 0;
      return o;
    }
    if (in.build) {
      o.job = J::Build;
      o.priority = 1;
      return o;
    }
    const bool bg = in.walk || in.compact || in.statsRank || scan;
    if (!in.wave && !in.statsTrack && !in.cover && !in.save && !bg) return o;
    if (moving) {
      o.wait = W::List;
      return o;
    }
    if (in.wave) {
      o.job = J::Wave;
      o.priority = 1;
      return o;
    }
    if (in.statsTrack) {
      o.job = J::Stats;
      o.priority = 1;
      return o;
    }
    if (in.cover) {
      o.job = J::Cover;
      o.priority = 1;
      return o;
    }
    J next = in.save        ? J::Save
             : in.updating  ? J::None
             : in.walk      ? J::Walk
             : in.compact   ? J::Compact
             : in.statsRank ? J::Stats
                            : J::Scan;
    if (next == J::None) {
      o.wait = W::Updating;
      return o;
    }
    const W w = next == J::Scan && low ? W::Battery : yield;
    if (w != W::None) {
      o.wait = w;
      return o;
    }
    o.job = next;
    o.statsRank = next == J::Stats;
    o.priority =
        next == J::Walk || ((next == J::Scan || next == J::Compact || next == J::Save || next == J::Stats) && in.dark)
            ? 1
            : 0;
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
  // The radio link's set-up draws from its own (2026-11-19), so the rest
  // of the session is the one it was before it.
  std::mt19937 rngSetup(20261119);
  // Per 100,000 passes.
  auto chance = [&](uint32_t n) { return rng() % 100000 < n; };
  // A state that lasts: on with `on` a pass, off again with `off`.
  auto flip = [&](bool& b, uint32_t on, uint32_t off) { b = b ? !chance(off) : chance(on); };
  auto chanceSetup = [&](uint32_t n) { return rngSetup() % 100000 < n; };
  // The listening stats' lanes (S2) from their own draws too.
  std::mt19937 rngStats(20261120);
  auto chanceStats = [&](uint32_t n) { return rngStats() % 100000 < n; };
  uint32_t ranks = 0, trackReads = 0;
  S s;
  Model m;
  S::In in = base(0);
  int64_t t = 0xFFFFFFFFll - 3600000ll;  // an hour before the wrap
  uint64_t runUntil = 0;
  uint32_t scans = 0, covers = 0, waves = 0, builds = 0, cuts = 0, controlWaits = 0, setupWaits = 0,
           held[S::kWaits] = {};
  for (int pass = 0; pass < 900000; ++pass) {
    t += 5 + rng() % 36;
    in.nowMs = static_cast<uint32_t>(t);
    if (static_cast<uint64_t>(t) >= runUntil) {
      in.running = J::None;
      in.runningRank = false;
    }
    flip(in.playing, 200, 100);
    flip(in.listMoving, 200, 2000);
    flip(in.dark, 100, 100);
    flip(in.decoderAtEnd, in.playing ? 100 : 0, 1500);
    flip(in.seeking, in.playing ? 50 : 0, 3000);
    flip(in.btSetup, 20, 500);
    // A link for about 37 s every 2 min; AVRCP about 5.6 s into it (past
    // the cap about one link in six), rarely up unlinked.
    flip(in.btLinked, 20, 60);
    flip(in.btControlUp, in.btLinked ? 400 : 20, in.btLinked ? 30 : 400);
    // A radio link set up about every 4 min, for about 11 s (past the cap
    // about one in two), its count moving at each rise and end; now and
    // then by two (a new radio link over one never seen to close, or one
    // that came and went between two passes).
    const bool setupWas = in.btLinkSetup;
    in.btLinkSetup = setupWas ? !chanceSetup(200) : chanceSetup(10);
    if (in.btLinkSetup != setupWas) ++in.btLinkSetupSeq;
    if (chanceSetup(10)) in.btLinkSetupSeq += 2;
    flip(in.usb, 10, 10);
    flip(in.updating, 100, 200);
    if (chance(500)) in.build = in.updating && chance(30000);
    if (chance(500)) in.save = in.updating && chance(50000);
    if (chance(1000)) in.cover = chance(40000);
    if (chance(1000)) in.wave = chance(20000);
    if (chance(300)) in.walk = chance(20000);
    if (chance(300)) in.compact = chance(20000);
    if (chance(500)) in.playingPending = chance(20000);
    if (chance(500)) in.queueNextPending = chance(30000);
    if (chance(500)) in.queueSoonPending = chance(40000);
    if (chance(500)) in.shownPending = chance(30000);
    if (chance(200)) in.restPending = chance(80000);
    if (chanceStats(300)) in.statsTrack = chanceStats(20000);
    if (chanceStats(300)) in.statsRank = chanceStats(30000);
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
    TEST_ASSERT_EQUAL_MESSAGE(want.btControlWait, got.btControlWait, "the wait for AVRCP");
    TEST_ASSERT_EQUAL_MESSAGE(want.btLinkSetupWait, got.btLinkSetupWait, "the radio link's set-up");
    TEST_ASSERT_EQUAL_MESSAGE(want.statsRank, got.statsRank, "a rank");
    TEST_ASSERT_EQUAL(m.low, s.batteryLow());
    ++held[static_cast<int>(got.wait)];
    if (got.cut) ++cuts;
    if (got.btControlWait) ++controlWaits;
    if (got.btLinkSetupWait) ++setupWaits;
    if (got.job != J::None) {
      TEST_ASSERT_EQUAL(J::None, in.running);
      if (got.job == J::Scan) {
        TEST_ASSERT_FALSE(s.batteryLow());
        ++scans;
      }
      if (got.job == J::Cover) ++covers;
      if (got.job == J::Wave) ++waves;
      if (got.job == J::Build) ++builds;
      if (got.job == J::Stats) {
        // A rank: never under the update step or a yield.
        // Each ask is one step: handed, it is taken.
        if (got.statsRank) {
          TEST_ASSERT_FALSE(in.updating);
          in.statsRank = false;
          ++ranks;
        } else {
          in.statsTrack = false;
          ++trackReads;
        }
      }
      in.running = got.job;
      in.runningRank = got.statsRank;
      runUntil = static_cast<uint64_t>(t) + 5 + rng() % 80;
    }
  }
  // It got through, and every wait came up.
  TEST_ASSERT_TRUE(scans > 10000);
  TEST_ASSERT_TRUE(covers > 1000);
  TEST_ASSERT_TRUE(waves > 1000);
  TEST_ASSERT_TRUE(builds > 10);
  TEST_ASSERT_TRUE(ranks > 100);
  TEST_ASSERT_TRUE(trackReads > 100);
  TEST_ASSERT_TRUE(cuts > 1000);
  TEST_ASSERT_TRUE(controlWaits > 1000);  // links waited for AVRCP ...
  TEST_ASSERT_TRUE(m.caps > 10);          // ... and the cap ended some of the waits
  TEST_ASSERT_TRUE(setupWaits > 1000);    // radio links held it while set up ...
  TEST_ASSERT_TRUE(m.setupCaps > 10);     // ... some to the cap ...
  TEST_ASSERT_TRUE(m.setupEnds > 10);     // ... and some ending within it
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
  RUN_TEST(test_the_wait_for_avrcp);
  RUN_TEST(test_the_radio_links_set_up);
  RUN_TEST(test_the_set_up_after_a_back_off);
  RUN_TEST(test_when_the_worker_may_go);
  RUN_TEST(test_input_and_lists);
  RUN_TEST(test_a_dark_screen_holds_nothing_for_a_list);
  RUN_TEST(test_the_order_of_the_yields);
  RUN_TEST(test_the_battery_floor);
  RUN_TEST(test_covers_come_first);
  RUN_TEST(test_a_waves_read_comes_before_a_cover);
  RUN_TEST(test_the_update_step);
  RUN_TEST(test_the_order_of_the_work);
  RUN_TEST(test_the_wrap);
  RUN_TEST(test_the_build_after_a_walk);
  RUN_TEST(test_the_time_waited);
  RUN_TEST(test_the_listening_stats_lanes);
  RUN_TEST(test_names);
  RUN_TEST(test_random_against_a_model);
  return UNITY_END();
}
