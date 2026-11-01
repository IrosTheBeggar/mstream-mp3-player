// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for RtcSync (docs/HOST-STATUS.md, "@time"): the boot's catch
// of the RTC's next tick, a @time's write on a second and the phase it
// measures, against a model of the chip that restarts its divider at a
// seconds write and one whose divider runs on, read from a model of the
// loop (BoardClock::loop()'s steps) at its passes' pace. Real time here is
// Unix ms (the computer's clock, exact); the board's monotonic ms start
// anywhere (2^32 too: no wrap).
// Run: pio test -e native -f test_rtc_sync
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "RtcSync.h"
#include "WallClock.h"

using namespace rtcsync;
using wallclock::Clock;

void setUp() {}
void tearDown() {}

namespace {

constexpr int64_t kT0 = 1792332200000;  // 2026-10-18 14:03:20 UTC, a whole second

// The chip: the second it says (Unix ms) and when it next ticks (real ms).
struct Chip {
  bool resets = true;    // its divider restarts at a seconds write
  int firstAfterWrite = 1000;  // then its first tick this long after it
  bool counting = true;  // a chip that stopped (its oscillator)
  bool takes = true;     // a write changes it (false: one that ignores writes)
  bool stuckFlag = false;
  bool voltLow = false;
  uint64_t count = 0;
  int64_t nextTick = 0;
  void advance(int64_t t) {
    if (!counting) return;
    while (t >= nextTick) {
      count += 1000;
      nextTick += 1000;
    }
  }
  void write(int64_t t, uint64_t second) {
    advance(t);
    if (!takes) return;
    count = second;
    voltLow = stuckFlag;
    if (resets) nextTick = t + firstAfterWrite;
  }
  void registers(int64_t t, uint8_t (&r)[7]) {
    advance(t);
    wallclock::RtcWrite w[wallclock::kRtcWrites];
    wallclock::rtcWrites(wallclock::toCivil(count), w);
    std::memcpy(r, w[2].data, 7);
    if (voltLow) r[0] |= wallclock::kRtcVoltLow;
  }
};

// A chip whose seconds begin `phase` ms (0-999) after the true ones,
// saying `behind` ms (whole seconds) less than the true time: at kT0 the
// second it is in, then its next tick.
Chip chipAt(bool resets, int phase, int64_t behind = 3000) {
  Chip c;
  c.resets = resets;
  c.nextTick = kT0 + (phase == 0 ? 1000 : phase);
  c.count = static_cast<uint64_t>(kT0 - (phase == 0 ? 0 : 1000) - behind);
  return c;
}

// The loop's pace: a pass every 4-24 ms (a seeded walk), or a fixed one.
struct Pace {
  uint32_t seed = 1;
  int fixed = 0;      // > 0: every pass this long
  int64_t slowFrom = 0, slowTo = 0;  // passes of `slowMs` in [slowFrom, slowTo) (real ms)
  int slowMs = 0;
  int next(int64_t t) {
    if (t >= slowFrom && t < slowTo) return slowMs;
    if (fixed > 0) return fixed;
    seed = seed * 1103515245u + 12345u;
    return 4 + static_cast<int>((seed >> 16) % 21);
  }
};

// The board: the clock, the sync and NVS's phase, run as BoardClock does.
struct Board {
  Chip* chip = nullptr;
  int32_t nvs = kNoPhase;  // "clock"/"phase" (kNoPhase: absent)
  int64_t bootReal = 0;
  uint64_t monoBase = 0;
  Clock clock;
  Sync sync;
  // Reads that don't go through: in [failFrom, failTo), and every
  // `failEvery`th one (0: none).
  int64_t failFrom = 0, failTo = 0;
  int failEvery = 0;
  int reads = 0;
  int writesFailing = 0;  // the next this many writes don't go through
  int writes = 0;
  std::vector<Event> events;

  uint64_t mono(int64_t t) const { return monoBase + static_cast<uint64_t>(t - bootReal); }

  // The boot's reading at real `t` (monotonic `base` then), the clock, the
  // catch from the loop's first pass.
  void boot(int64_t t, uint64_t base = 300) {
    bootReal = t;
    monoBase = base;
    clock = Clock();
    sync = Sync();
    events.clear();
    uint8_t r[7];
    chip->registers(t, r);
    clock.boot(wallclock::fromRegisters(r), mono(t));
    sync.boot(clock, nvs);
  }
  void drain() {
    Event e;
    while (sync.takeEvent(&e)) {
      events.push_back(e);
      if (e.kind == Event::Kind::Taken) nvs = e.phaseMs;
    }
  }
  // One loop pass at real `t`. `stopAfterForget`: a restart right after
  // the phase was dropped (nothing else in the pass).
  bool pass(int64_t t, bool stopAfterForget = false) {
    for (int i = 0; i < 4; ++i) {
      uint64_t second = 0;
      const Do d = sync.step(mono(t), clock, &second);
      drain();
      if (d == Do::Nothing) break;
      if (d == Do::Forget) {
        nvs = kNoPhase;
        sync.forgot();
        if (stopAfterForget) return true;
        continue;
      }
      if (d == Do::Write) {
        ++writes;
        const bool ok = writesFailing <= 0;
        if (ok) {
          chip->write(t, second);
        } else {
          --writesFailing;
        }
        sync.wrote(mono(t), ok, clock);
        drain();
        continue;
      }
      ++reads;
      const bool fails = (t >= failFrom && t < failTo) || (failEvery > 0 && reads % failEvery == 0);
      uint8_t r[7] = {};
      if (!fails) chip->registers(t, r);
      sync.read(mono(t), sample(!fails, r), clock);
      drain();
      break;
    }
    return false;
  }
  // Passes from real `from` at `pace` while the sync is busy (`limit` ms at
  // most): the real time of the last.
  int64_t run(int64_t from, Pace& pace, int64_t limit = 15000) {
    int64_t t = from;
    while (sync.busy() && t - from < limit) {
      pass(t);
      t += pace.next(t);
    }
    return t;
  }
  bool set(int64_t t, uint64_t unixMs) {
    if (!clock.set(unixMs, mono(t))) return false;
    sync.plan(unixMs, mono(t));
    return true;
  }
  int64_t error(int64_t t) const { return static_cast<int64_t>(clock.at(mono(t))) - t; }
  const Event* last(Event::Kind k) const {
    for (size_t i = events.size(); i-- > 0;) {
      if (events[i].kind == k) return &events[i];
    }
    return nullptr;
  }
};

int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

// A @time at real `at` (exact; the board up 8 s, its catch done), its
// write and check run at `pace`; then a restart `after` ms later (its
// reading 300 ms in, the loop 2 s in): the clock's error once the
// restart's catch is done. `base`: the first board's monotonic ms at its
// reading (the second's 2.5 s more).
struct Outcome {
  int64_t error = 0;       // after the restart's catch
  int64_t coarse = 0;      // the boot's reading's, before it
  int32_t phase = kNoPhase;
  bool caught = false;
};
Outcome setThenRestart(Chip& chip, int64_t at, int64_t after, Pace& pace, uint64_t base = 300) {
  Board a;
  a.chip = &chip;
  a.boot(at - 8000, base);
  a.run(at - 6000, pace);
  TEST_ASSERT_FALSE(a.sync.busy());
  TEST_ASSERT_TRUE(a.set(at, static_cast<uint64_t>(at)));
  a.run(at, pace);
  TEST_ASSERT_FALSE(a.sync.busy());
  Outcome o;
  o.phase = a.nvs;
  Board b;
  b.chip = &chip;
  b.nvs = a.nvs;
  const int64_t r = at + after;
  b.boot(r + 300, base + 2500);
  o.coarse = b.error(r + 300);
  const int64_t end = b.run(r + 2000, pace);
  o.error = b.error(end);
  o.caught = b.last(Event::Kind::Caught) != nullptr;
  return o;
}

}  // namespace

// The phase as NVS holds it: absent, or past a second either way, is none.
void test_phase_from() {
  TEST_ASSERT_EQUAL_INT32(kNoPhase, phaseFrom(false, 12));
  TEST_ASSERT_EQUAL_INT32(12, phaseFrom(true, 12));
  TEST_ASSERT_EQUAL_INT32(-987, phaseFrom(true, -987));
  TEST_ASSERT_EQUAL_INT32(0, phaseFrom(true, 0));
  TEST_ASSERT_EQUAL_INT32(kMaxPhase - 1, phaseFrom(true, kMaxPhase - 1));
  TEST_ASSERT_EQUAL_INT32(kNoPhase, phaseFrom(true, kMaxPhase));
  TEST_ASSERT_EQUAL_INT32(kNoPhase, phaseFrom(true, -kMaxPhase));
  TEST_ASSERT_EQUAL_INT32(kNoPhase, phaseFrom(true, kNoPhase));
}

// A read's sample: what the boot would believe, the flag, a read that
// didn't go through.
void test_sample() {
  Chip c = chipAt(true, 0, 0);
  uint8_t r[7];
  c.registers(kT0, r);
  Sample s = sample(true, r);
  TEST_ASSERT_TRUE(s.answered && s.believed && !s.voltLow);
  TEST_ASSERT_EQUAL_UINT64(kT0, s.second);
  s = sample(false, r);
  TEST_ASSERT_FALSE(s.answered || s.believed);
  r[0] |= 0x80;
  s = sample(true, r);
  TEST_ASSERT_TRUE(s.answered && s.voltLow && !s.believed);
  r[0] &= 0x7f;
  r[5] |= 0x80;  // the century bit: 1926
  s = sample(true, r);
  TEST_ASSERT_TRUE(s.answered && !s.believed);
  std::memset(r, 0xFF, sizeof(r));
  TEST_ASSERT_FALSE(sample(true, r).believed);
}

// The boot's catch: the clock is the boot's reading (its second's start)
// until the chip's next tick, then exact within half the bracket; with a
// phase kept, the phase added. Every 10 ms of where the boot reads in the
// chip's second, both dividers.
void test_the_boots_catch() {
  for (int phase : {0, 5, 250, 731, 999}) {
    for (int at = 0; at < 1000; at += 10) {
      Chip chip = chipAt(false, phase, 0);  // its seconds `phase` after the true ones
      Board b;
      b.chip = &chip;
      b.nvs = phase;
      const int64_t read = kT0 + 5000 + at;
      b.boot(read);
      // Before the catch: the second read, taken as its start (behind by
      // where in it the reading came, the phase less).
      const int64_t coarse = b.error(read);
      TEST_ASSERT_TRUE(coarse <= -phase + 0 && coarse > -phase - 1000);
      Pace pace;
      pace.seed = static_cast<uint32_t>(at + 7 * phase + 1);
      const int64_t end = b.run(read + 1500, pace);
      TEST_ASSERT_FALSE(b.sync.busy());
      const Event* e = b.last(Event::Kind::Caught);
      TEST_ASSERT_NOT_NULL(e);
      TEST_ASSERT_TRUE(abs64(b.error(end)) <= static_cast<int64_t>(e->withinMs));
      TEST_ASSERT_TRUE(e->withinMs <= kCloseMs / 2);
      TEST_ASSERT_EQUAL_INT32(phase, e->phaseMs);
      TEST_ASSERT_EQUAL_UINT64(e->second + static_cast<uint64_t>(phase), e->time);
      TEST_ASSERT_TRUE(abs64(e->shiftMs + coarse) <= static_cast<int64_t>(e->withinMs));  // it was that far behind
      TEST_ASSERT_TRUE(b.clock.source() == wallclock::Source::Rtc);
    }
  }
}

// A @time, then a restart: within the two catches' brackets (under 30 ms,
// the loop's 4-24 ms passes) of the computer's time, whatever the chip's
// divider does at a write, wherever its own seconds fall and wherever in
// its second the @time came. Right on average. (B13's nearest second,
// read to the second, spread over 1.5 s, and the device's four restarts
// read 0.4-1.3 s behind.)
void test_set_then_restart_both_dividers() {
  for (bool resets : {true, false}) {
    int64_t worst = 0, sum = 0;
    int n = 0;
    for (int phase = 0; phase < 1000; phase += 37) {
      for (int frac = 0; frac < 1000; frac += 61) {
        Chip chip = chipAt(resets, phase);
        Pace pace;
        pace.seed = static_cast<uint32_t>(phase * 1000 + frac + (resets ? 1 : 2));
        const Outcome o = setThenRestart(chip, kT0 + 10000 + frac, 7000 + 13 * frac, pace);
        TEST_ASSERT_TRUE(o.caught);
        TEST_ASSERT_TRUE(o.phase != kNoPhase);
        TEST_ASSERT_TRUE_MESSAGE(abs64(o.error) <= 30, resets ? "resets" : "runs on");
        if (abs64(o.error) > worst) worst = abs64(o.error);
        sum += o.error;
        ++n;
        // The phase measured: the write's lateness for a chip that restarts
        // its divider (a pass at most); its own seconds for the other.
        if (resets) {
          TEST_ASSERT_TRUE(o.phase >= -15 && o.phase <= static_cast<int32_t>(kLateMs) + 15);
        } else {
          TEST_ASSERT_TRUE(o.phase > -1000 && o.phase <= static_cast<int32_t>(kLateMs) + 15);
          const int32_t want = phase == 0 ? 0 : phase - 1000;  // the chip ticks `phase` after: -(1000 - phase) of the next
          TEST_ASSERT_TRUE(abs64(o.phase - want) <= 15 || abs64(o.phase - (want + 1000)) <= 15);
        }
      }
    }
    TEST_ASSERT_TRUE(worst <= 30);
    TEST_ASSERT_TRUE(abs64(sum / n) <= 5);
  }
}

// The same with a slow loop (a pass every 60 ms: the brackets wider than
// kCloseMs, the catch takes the closest of its three seconds' ticks, the
// write waits twice then goes up to a pass late and the phase takes it
// in): within a pass.
void test_slow_loop() {
  for (bool resets : {true, false}) {
    for (int frac = 0; frac < 1000; frac += 97) {
      Chip chip = chipAt(resets, 412);
      Pace pace;
      pace.fixed = 60;
      const Outcome o = setThenRestart(chip, kT0 + 10000 + frac, 9000, pace);
      TEST_ASSERT_TRUE(o.caught);
      TEST_ASSERT_TRUE(abs64(o.error) <= 60);
    }
  }
}

// A chip that restarts its divider at a write but holds its first tick
// (as the PCF8563 does after its STOP bit: half a second; or later than a
// second): its seconds begin that much after the write, the phase says so,
// and a restart is as exact.
void test_a_first_tick_held_after_a_write() {
  for (int first : {500, 1500, 1990}) {
    for (int frac = 0; frac < 1000; frac += 113) {
      Chip chip = chipAt(true, 250);
      chip.firstAfterWrite = first;
      Pace pace;
      pace.seed = static_cast<uint32_t>(first + frac);
      const Outcome o = setThenRestart(chip, kT0 + 10000 + frac, 8000, pace);
      TEST_ASSERT_TRUE(o.caught);
      TEST_ASSERT_TRUE(abs64(o.error) <= 30);
      TEST_ASSERT_TRUE(abs64(o.phase - (first - 1000)) <= static_cast<int64_t>(kLateMs) + 15);
    }
  }
}

// A slow pass at the write's second (a list's frame): the write waits for
// the next second, and goes on time.
void test_a_late_pass_waits() {
  Chip chip = chipAt(true, 100);
  Board a;
  a.chip = &chip;
  a.boot(kT0, 300);
  Pace pace;
  a.run(kT0 + 2000, pace);
  const int64_t at = kT0 + 6400;  // a @time at .400: its write at .000 of the next second
  TEST_ASSERT_TRUE(a.set(at, static_cast<uint64_t>(at)));
  pace.slowFrom = at;
  pace.slowTo = at + 800;  // passes of 170 ms over the second: the first after it at .080
  pace.slowMs = 170;
  a.run(at, pace);
  const Event* e = a.last(Event::Kind::Taken);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_TRUE(e->lateMs <= kLateMs);
  TEST_ASSERT_TRUE(e->second > static_cast<uint64_t>(at) + 1000);  // the second after
  TEST_ASSERT_TRUE(abs64(e->phaseMs - static_cast<int32_t>(e->lateMs)) <= 15);
  TEST_ASSERT_EQUAL_INT(1, a.writes);
}

// Reads that don't go through during a catch: every third, a burst over
// the first tick, all of them. The catch takes the next tick, or gives up
// in kCatchMs and the clock stays the boot's reading.
void test_read_failures() {
  for (bool resets : {true, false}) {
    // Every third read: still within the brackets.
    {
      Chip chip = chipAt(resets, 640, 0);
      Board b;
      b.chip = &chip;
      b.nvs = 640;
      b.failEvery = 3;
      b.boot(kT0 + 5100);
      Pace pace;
      const int64_t end = b.run(kT0 + 7000, pace);
      TEST_ASSERT_NOT_NULL(b.last(Event::Kind::Caught));
      TEST_ASSERT_TRUE(abs64(b.error(end)) <= 30);
    }
    // A burst over the first tick: the next one.
    {
      Chip chip = chipAt(resets, 640, 0);
      Board b;
      b.chip = &chip;
      b.nvs = 640;
      b.boot(kT0 + 5100);
      b.failFrom = kT0 + 7000;
      b.failTo = kT0 + 7900;  // the tick at 7.640 among them
      Pace pace;
      const int64_t end = b.run(kT0 + 7000, pace);
      const Event* e = b.last(Event::Kind::Caught);
      TEST_ASSERT_NOT_NULL(e);
      TEST_ASSERT_TRUE(abs64(b.error(end)) <= 30);
      TEST_ASSERT_EQUAL_UINT64(kT0 + 8000, e->second);  // the tick after the burst, at 8.640
    }
    // None: given up after kCatchMs, the boot's reading kept.
    {
      Chip chip = chipAt(resets, 640, 0);
      Board b;
      b.chip = &chip;
      b.boot(kT0 + 5100);
      const int64_t coarse = b.error(kT0 + 5100);
      b.failFrom = 0;
      b.failTo = INT64_MAX;
      Pace pace;
      const int64_t end = b.run(kT0 + 7000, pace);
      const Event* e = b.last(Event::Kind::Missed);
      TEST_ASSERT_NOT_NULL(e);
      TEST_ASSERT_TRUE(e->why == Event::Why::Unanswered);
      TEST_ASSERT_TRUE(end - (kT0 + 7000) <= static_cast<int64_t>(kCatchMs) + 60);
      TEST_ASSERT_EQUAL_INT64(coarse, b.error(end));
    }
  }
}

// A chip that isn't counting: given up once its second stays for kStillMs
// (not the whole kCatchMs), the clock as the boot read it.
void test_a_chip_that_stopped() {
  Chip chip = chipAt(true, 300);
  chip.counting = false;
  Board b;
  b.chip = &chip;
  b.boot(kT0 + 5100);
  const int64_t coarse = b.error(kT0 + 5100);
  Pace pace;
  const int64_t end = b.run(kT0 + 7000, pace);
  const Event* e = b.last(Event::Kind::Missed);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_TRUE(e->why == Event::Why::Still);
  TEST_ASSERT_TRUE(end - (kT0 + 7000) <= static_cast<int64_t>(kStillMs) + 60);  // (a poll and two passes)
  TEST_ASSERT_EQUAL_INT64(coarse, b.error(end));
}

// A tick that doesn't agree with the boot's reading (a misread at the
// boot, a chip set elsewhere since): the boot's reading kept.
void test_a_tick_that_disagrees() {
  Chip chip = chipAt(true, 300, 0);
  Board b;
  b.chip = &chip;
  b.boot(kT0 + 5100);
  chip.count += 5000;  // 5 s on after the boot's reading
  const int64_t coarse = b.error(kT0 + 5100);
  Pace pace;
  const int64_t end = b.run(kT0 + 7000, pace);
  const Event* e = b.last(Event::Kind::Missed);
  TEST_ASSERT_NOT_NULL(e);
  TEST_ASSERT_TRUE(e->why == Event::Why::Disagreed);
  TEST_ASSERT_EQUAL_INT64(coarse, b.error(end));
}

// A restart in the write's window. Before the write: the RTC as it was
// with its phase (a set before it: still exact; a chip never set: unset).
// Right after the phase was dropped, before the write: the RTC as it was,
// no phase (within a second). After the write, before its tick was
// caught: the new time, no phase (within the write's lateness for a chip
// that restarts its divider, a second for the other).
void test_a_restart_in_the_writes_window() {
  for (bool resets : {true, false}) {
    // A first @time, done: its phase kept.
    Chip chip = chipAt(resets, 555);
    Pace pace;
    Board a;
    a.chip = &chip;
    a.boot(kT0, 300);
    a.run(kT0 + 2000, pace);
    TEST_ASSERT_TRUE(a.set(kT0 + 6250, kT0 + 6250));
    a.run(kT0 + 6250, pace);
    const int32_t kept = a.nvs;
    TEST_ASSERT_TRUE(kept != kNoPhase);
    // A second one, then a restart before its write (its second is .000).
    const int64_t at = kT0 + 20300;
    TEST_ASSERT_TRUE(a.set(at, at));
    a.pass(at);
    a.pass(at + 300);  // (still waiting)
    TEST_ASSERT_TRUE(a.sync.busy());
    TEST_ASSERT_EQUAL_INT32(kept, a.nvs);
    Board b;
    b.chip = &chip;
    b.nvs = a.nvs;
    b.boot(at + 700);
    int64_t end = b.run(at + 2500, pace);
    TEST_ASSERT_TRUE(abs64(b.error(end)) <= 30);  // the first @time's, exact
    // Right after the drop.
    Board c;
    c.chip = &chip;
    c.nvs = kept;
    c.boot(kT0 + 30000);
    c.run(kT0 + 32000, pace);
    const int64_t at2 = kT0 + 40500;
    TEST_ASSERT_TRUE(c.set(at2, at2));
    c.pass(at2);
    TEST_ASSERT_TRUE(c.pass(at2 + 503, true));  // the drop, then the restart
    TEST_ASSERT_EQUAL_INT32(kNoPhase, c.nvs);
    Board d;
    d.chip = &chip;
    d.nvs = c.nvs;
    d.boot(at2 + 900);
    end = d.run(at2 + 2500, pace);
    TEST_ASSERT_NOT_NULL(d.last(Event::Kind::Caught));
    TEST_ASSERT_TRUE(d.error(end) > -1000 - 30 && d.error(end) < 1000 + 30);
    // After the write, before its tick.
    Board e;
    e.chip = &chip;
    e.boot(kT0 + 50000);
    e.run(kT0 + 52000, pace);
    const int64_t at3 = kT0 + 60800;
    TEST_ASSERT_TRUE(e.set(at3, at3));
    e.pass(at3);
    e.pass(at3 + 204);  // the write at 61.004, its read-back
    TEST_ASSERT_EQUAL_INT(1, e.writes);
    TEST_ASSERT_TRUE(e.sync.busy());
    TEST_ASSERT_EQUAL_INT32(kNoPhase, e.nvs);
    Board f;
    f.chip = &chip;
    f.nvs = e.nvs;
    f.boot(at3 + 900);
    end = f.run(at3 + 2500, pace);
    TEST_ASSERT_NOT_NULL(f.last(Event::Kind::Caught));
    if (resets) {
      TEST_ASSERT_TRUE(f.error(end) >= -30 && f.error(end) <= 30);  // (written 4 ms late)
    } else {
      TEST_ASSERT_TRUE(f.error(end) > -1000 - 30 && f.error(end) < 1000 + 30);
    }
  }
}

// The monotonic ms past 2^32 (where a 32-bit millis() wraps, 49.7 days
// up) in the middle of the windows: the same as anywhere.
void test_past_two_to_the_32() {
  for (bool resets : {true, false}) {
    for (int frac : {0, 250, 999}) {
      Chip chip = chipAt(resets, 820);
      Pace pace;
      pace.seed = static_cast<uint32_t>(frac + 3);
      // The first board's reading at mono 2^32 - 8.5 s: its @time at 2^32 -
      // 0.5 s, its write and check over 2^32; the second's reading at 2^32
      // - 2 s, its loop from 2^32 - 0.3 s, its catch over 2^32.
      const Outcome o = setThenRestart(chip, kT0 + 10000 + frac, 7000, pace, 0x100000000ull - 8500);
      TEST_ASSERT_TRUE(o.caught);
      TEST_ASSERT_TRUE(abs64(o.error) <= 30);
    }
  }
}

// Writes that don't go through, a read-back that isn't the time: written
// again on the next second, kAttempts in all; then given up, the clock
// still the computer's, no phase kept.
void test_a_write_refused() {
  // One failed write: the next second's goes.
  {
    Chip chip = chipAt(true, 0);
    Board a;
    a.chip = &chip;
    a.nvs = 77;
    a.boot(kT0, 300);
    Pace pace;
    a.run(kT0 + 2000, pace);
    a.writesFailing = 1;
    TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
    const int64_t end = a.run(kT0 + 6100, pace);
    const Event* r = a.last(Event::Kind::Refused);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(r->why == Event::Why::WriteFailed && r->again && r->attempt == 1);
    const Event* t = a.last(Event::Kind::Taken);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_INT(2, a.writes);
    TEST_ASSERT_EQUAL_UINT64(kT0 + 8000, t->second);
    TEST_ASSERT_EQUAL_INT32(t->phaseMs, a.nvs);
    TEST_ASSERT_EQUAL_INT64(0, a.error(end));
  }
  // Every write fails: three, then given up.
  {
    Chip chip = chipAt(true, 0);
    Board a;
    a.chip = &chip;
    a.nvs = 77;
    a.boot(kT0, 300);
    Pace pace;
    a.run(kT0 + 2000, pace);
    a.writesFailing = 100;
    TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
    const int64_t end = a.run(kT0 + 6100, pace);
    TEST_ASSERT_FALSE(a.sync.busy());
    TEST_ASSERT_EQUAL_INT(kAttempts, a.writes);
    const Event* r = a.last(Event::Kind::Refused);
    TEST_ASSERT_TRUE(r->why == Event::Why::WriteFailed && !r->again && r->attempt == kAttempts);
    TEST_ASSERT_EQUAL_INT32(kNoPhase, a.nvs);  // dropped before the first
    TEST_ASSERT_EQUAL_INT64(0, a.error(end));
    TEST_ASSERT_TRUE(a.clock.source() == wallclock::Source::Host);
  }
  // A chip that ignores writes: its read-back the old time.
  {
    Chip chip = chipAt(true, 0);
    chip.takes = false;
    Board a;
    a.chip = &chip;
    a.boot(kT0, 300);
    Pace pace;
    a.run(kT0 + 2000, pace);
    TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
    a.run(kT0 + 6100, pace);
    TEST_ASSERT_EQUAL_INT(kAttempts, a.writes);
    const Event* r = a.last(Event::Kind::Refused);
    TEST_ASSERT_TRUE(r->why == Event::Why::Wrong && !r->again);
    TEST_ASSERT_TRUE(r->back.believed);
    TEST_ASSERT_TRUE(r->back.second < r->second);
  }
  // Its flag still set after the write.
  {
    Chip chip = chipAt(true, 0);
    chip.stuckFlag = true;
    Board a;
    a.chip = &chip;
    a.boot(kT0, 300);
    Pace pace;
    a.run(kT0 + 2000, pace);
    TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
    a.run(kT0 + 6100, pace);
    const Event* r = a.last(Event::Kind::Refused);
    TEST_ASSERT_TRUE(r->why == Event::Why::Flag && !r->again);
  }
  // No read-back goes through: written again, then given up.
  {
    Chip chip = chipAt(true, 0);
    Board a;
    a.chip = &chip;
    a.boot(kT0, 300);
    Pace pace;
    a.run(kT0 + 2000, pace);
    a.failFrom = kT0 + 6000;
    a.failTo = INT64_MAX;
    TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
    a.run(kT0 + 6100, pace, 30000);
    TEST_ASSERT_FALSE(a.sync.busy());
    TEST_ASSERT_EQUAL_INT(kAttempts, a.writes);
    const Event* r = a.last(Event::Kind::Refused);
    TEST_ASSERT_TRUE(r->why == Event::Why::Unread && !r->again);
  }
}

// After the write, its tick not caught (a chip that stopped, reads lost
// after the read-back): taken, no phase kept.
void test_written_but_untimed() {
  Chip chip = chipAt(true, 0);
  Board a;
  a.chip = &chip;
  a.nvs = 77;
  a.boot(kT0, 300);
  Pace pace;
  a.run(kT0 + 2000, pace);
  TEST_ASSERT_TRUE(a.set(kT0 + 6100, kT0 + 6100));
  a.pass(kT0 + 6100);
  a.pass(kT0 + 7003);  // the write and its read-back
  TEST_ASSERT_EQUAL_INT(1, a.writes);
  chip.counting = false;
  a.run(kT0 + 7015, pace);
  const Event* u = a.last(Event::Kind::Untimed);
  TEST_ASSERT_NOT_NULL(u);
  TEST_ASSERT_TRUE(u->why == Event::Why::Still);
  TEST_ASSERT_EQUAL_UINT64(kT0 + 7000, u->second);
  TEST_ASSERT_EQUAL_UINT32(3, u->lateMs);
  TEST_ASSERT_EQUAL_INT32(kNoPhase, a.nvs);
}

// The latest @time wins: one during the boot's catch drops it (the clock
// the computer's, no Caught); one during another's check starts over, its
// own phase kept.
void test_the_latest_time_wins() {
  Chip chip = chipAt(false, 333);
  Board a;
  a.chip = &chip;
  a.nvs = 333 - 1000;
  a.boot(kT0 + 1000, 300);
  Pace pace;
  a.pass(kT0 + 2000);
  a.pass(kT0 + 2015);
  TEST_ASSERT_TRUE(a.sync.busy());
  TEST_ASSERT_TRUE(a.set(kT0 + 2020, kT0 + 2020));
  int64_t end = a.run(kT0 + 2030, pace);
  TEST_ASSERT_NULL(a.last(Event::Kind::Caught));
  TEST_ASSERT_NOT_NULL(a.last(Event::Kind::Taken));
  TEST_ASSERT_EQUAL_INT64(0, a.error(end));
  // Another during the check of a third.
  TEST_ASSERT_TRUE(a.set(kT0 + 9600, kT0 + 9600));
  a.pass(kT0 + 9600);
  a.pass(kT0 + 10005);  // its write
  a.pass(kT0 + 10020);
  TEST_ASSERT_TRUE(a.sync.busy());
  // The computer's clock now says 0.25 s more (another computer, say).
  TEST_ASSERT_TRUE(a.set(kT0 + 10030, kT0 + 10030 + 250));
  a.events.clear();
  end = a.run(kT0 + 10040, pace);
  const Event* t = a.last(Event::Kind::Taken);
  TEST_ASSERT_NOT_NULL(t);
  TEST_ASSERT_EQUAL_UINT64(kT0 + 11000, t->second);  // the second by the clock it set
  TEST_ASSERT_EQUAL_INT64(250, a.error(end));
  // A restart: the latest @time's clock, within the brackets.
  Board b;
  b.chip = &chip;
  b.nvs = a.nvs;
  b.boot(kT0 + 20100);
  end = b.run(kT0 + 22000, pace);
  TEST_ASSERT_TRUE(abs64(b.error(end) - 250) <= 30);
}

// 2099's last second: the next is past the chip's calendar, so the write
// goes at once, the second it is in; nothing waits on a 2100.
void test_the_last_second() {
  const int64_t last = static_cast<int64_t>(wallclock::kMaxMs / 1000 * 1000);  // 2099-12-31 23:59:59
  Chip chip;
  chip.resets = true;
  chip.count = static_cast<uint64_t>(last - 600000);
  chip.nextTick = last - 600000 + 400;
  Board a;
  a.chip = &chip;
  a.boot(last - 599800, 300);
  Pace pace;
  a.run(last - 598000, pace);
  TEST_ASSERT_TRUE(a.set(last + 200, static_cast<uint64_t>(last + 200)));
  a.pass(last + 200);
  TEST_ASSERT_EQUAL_INT(1, a.writes);
  TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(last), chip.count);
  a.run(last + 210, pace);  // (2100 isn't a time it believes: no tick caught)
  TEST_ASSERT_FALSE(a.sync.busy());
}

// Nothing to do: an unset clock at the boot (no catch), and no reads
// while idle.
void test_idle() {
  Chip chip = chipAt(true, 0);
  chip.voltLow = true;
  Board b;
  b.chip = &chip;
  b.boot(kT0);
  TEST_ASSERT_FALSE(b.clock.ok());
  TEST_ASSERT_FALSE(b.sync.busy());
  b.pass(kT0 + 2000);
  TEST_ASSERT_EQUAL_INT(0, b.reads);
  Event e;
  TEST_ASSERT_FALSE(b.sync.takeEvent(&e));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_phase_from);
  RUN_TEST(test_sample);
  RUN_TEST(test_the_boots_catch);
  RUN_TEST(test_set_then_restart_both_dividers);
  RUN_TEST(test_slow_loop);
  RUN_TEST(test_a_first_tick_held_after_a_write);
  RUN_TEST(test_a_late_pass_waits);
  RUN_TEST(test_read_failures);
  RUN_TEST(test_a_chip_that_stopped);
  RUN_TEST(test_a_tick_that_disagrees);
  RUN_TEST(test_a_restart_in_the_writes_window);
  RUN_TEST(test_past_two_to_the_32);
  RUN_TEST(test_a_write_refused);
  RUN_TEST(test_written_but_untimed);
  RUN_TEST(test_the_latest_time_wins);
  RUN_TEST(test_the_last_second);
  RUN_TEST(test_idle);
  return UNITY_END();
}
