// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the screen policy (ScreenPower, docs/ENERGY.md item 2): the
// dim and off timings for each choice, what keeps it lit, what holds a
// lit one lit (a countdown toast, the Pair screen's search), the wakes,
// the PWR key, the pocket guard, "Touch when off" (touch(): nothing, a
// wake, or the glance), and the backlight levels. The wake latch's own cases (with
// the real recognisers) are in test_ui_input.
// Run: pio test -e native
#include <unity.h>

#include <initializer_list>

#include <cstring>

#include "ScreenPower.h"

void setUp() {}
void tearDown() {}

namespace {

using L = ScreenPower::Level;
using W = ScreenPower::Why;

// Steps every 50 ms from `from` to `to` (as the loop would), no input.
// Returns how many level changes it saw.
int run(ScreenPower& s, uint32_t from, uint32_t to, bool keepLit = false, bool holdLit = false) {
  int changes = 0;
  for (uint32_t t = from; t - from <= to - from; t += 50) {
    if (s.step(t, keepLit, holdLit)) ++changes;
    if (t == to) break;
  }
  return changes;
}

}  // namespace

void test_tables_and_defaults() {
  TEST_ASSERT_EQUAL_UINT32(15000, ScreenPower::timeoutMs(0));
  TEST_ASSERT_EQUAL_UINT32(30000, ScreenPower::timeoutMs(1));
  TEST_ASSERT_EQUAL_UINT32(60000, ScreenPower::timeoutMs(2));
  TEST_ASSERT_EQUAL_UINT32(120000, ScreenPower::timeoutMs(3));
  TEST_ASSERT_EQUAL_UINT32(300000, ScreenPower::timeoutMs(4));
  TEST_ASSERT_EQUAL_UINT32(0, ScreenPower::timeoutMs(ScreenPower::kNever));
  TEST_ASSERT_EQUAL_STRING("30 s", ScreenPower::timeoutLabel(ScreenPower::kDefaultTimeout));
  TEST_ASSERT_EQUAL_STRING("Never", ScreenPower::timeoutLabel(5));
  TEST_ASSERT_EQUAL_UINT8(60, ScreenPower::brightnessLevel(0));
  TEST_ASSERT_EQUAL_UINT8(100, ScreenPower::brightnessLevel(ScreenPower::kDefaultBrightness));
  TEST_ASSERT_EQUAL_UINT8(160, ScreenPower::brightnessLevel(2));
  TEST_ASSERT_EQUAL_UINT8(255, ScreenPower::brightnessLevel(3));
  TEST_ASSERT_EQUAL_STRING("Medium", ScreenPower::brightnessLabel(1));
  // Out of range (a damaged NVS value): the defaults.
  TEST_ASSERT_EQUAL_UINT32(30000, ScreenPower::timeoutMs(9));
  TEST_ASSERT_EQUAL_UINT8(100, ScreenPower::brightnessLevel(-1));
  // Dim for the last 10 s; the 15 s choice from 7 s.
  TEST_ASSERT_EQUAL_UINT32(7000, ScreenPower::dimAtMs(15000));
  TEST_ASSERT_EQUAL_UINT32(20000, ScreenPower::dimAtMs(30000));
  TEST_ASSERT_EQUAL_UINT32(290000, ScreenPower::dimAtMs(300000));
  TEST_ASSERT_EQUAL_UINT32(0, ScreenPower::dimAtMs(0));
  ScreenPower s;
  s.begin(0);
  TEST_ASSERT_EQUAL_INT(ScreenPower::kDefaultTimeout, s.timeout());
  TEST_ASSERT_EQUAL_INT(ScreenPower::kDefaultBrightness, s.brightness());
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_EQUAL_UINT8(100, s.backlight());
}

// The default: Bright for 20 s, Dim (30) for 10 s, then Off (backlight 0).
void test_default_dims_at_20_s_and_goes_off_at_30_s() {
  ScreenPower s;
  s.begin(1000);
  TEST_ASSERT_EQUAL_INT(0, run(s, 1000, 20950));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.step(21000, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Timeout), static_cast<int>(s.why()));
  TEST_ASSERT_EQUAL_UINT8(ScreenPower::kDimBacklight, s.backlight());
  TEST_ASSERT_EQUAL_UINT32(10000, s.msUntilNext(21000));
  TEST_ASSERT_EQUAL_INT(0, run(s, 21050, 30950));
  TEST_ASSERT_TRUE(s.step(31000, false));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.previous()));
  TEST_ASSERT_EQUAL_UINT8(0, s.backlight());
  // It stays off by itself: only a wake lights it.
  TEST_ASSERT_EQUAL_INT(0, run(s, 31050, 600000));
  TEST_ASSERT_TRUE(s.off());
}

void test_each_choice_times() {
  const uint32_t dimAt[5] = {7000, 20000, 50000, 110000, 290000};
  for (int c = 0; c < 5; ++c) {
    ScreenPower s;
    s.begin(0);
    s.setTimeout(c, 0);
    s.step(0, false);
    const uint32_t off = ScreenPower::timeoutMs(c);
    s.step(dimAt[c] - 1, false);
    TEST_ASSERT_TRUE(s.bright());
    s.step(dimAt[c], false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
    s.step(off - 1, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
    s.step(off, false);
    TEST_ASSERT_TRUE(s.off());
  }
  // Never: bright for good.
  ScreenPower s;
  s.begin(0);
  s.setTimeout(ScreenPower::kNever, 0);
  s.step(0, false);
  TEST_ASSERT_EQUAL_INT(0, run(s, 0, 3600000));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_EQUAL_UINT32(0, s.msUntilNext(5000));
}

// Input restarts the countdown; a finger resting on the glass (activity
// every pass) keeps it bright.
void test_activity_restarts_the_countdown() {
  ScreenPower s;
  s.begin(0);
  s.step(15000, false);
  s.activity(15000);
  s.step(34000, false);
  TEST_ASSERT_TRUE(s.bright());
  // A finger held on the glass for a minute from here: it never dims.
  for (uint32_t t = 34000; t <= 94000; t += 20) {
    s.activity(t);
    TEST_ASSERT_FALSE(s.step(t, false));
  }
  TEST_ASSERT_TRUE(s.bright());
  s.step(113999, false);  // lifted at 94 s
  TEST_ASSERT_TRUE(s.bright());
  s.step(114000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  // Activity doesn't light a dim screen (a touch there is a wake's, and
  // swallowed): the countdown goes on.
  s.activity(115000);
  s.step(115000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  s.step(124000, false);
  TEST_ASSERT_TRUE(s.off());
}

// A touch on a dim screen only brightens it (true: the input layer swallows
// it); from Dim there is no pocket guard.
void test_wake_from_dim() {
  ScreenPower s;
  s.begin(0);
  s.step(20000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  TEST_ASSERT_TRUE(s.wake(25000, W::Touch));
  TEST_ASSERT_TRUE(s.step(25000, false));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Touch), static_cast<int>(s.why()));
  TEST_ASSERT_FALSE(s.pocketGuard());
  // The whole countdown again from the wake.
  s.step(44999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(45000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  // A touch while bright is no wake: it acts (false).
  ScreenPower b;
  b.begin(0);
  TEST_ASSERT_FALSE(b.wake(100, W::Touch));
  TEST_ASSERT_FALSE(b.step(100, false));
}

// A wake from Off by a touch or the PWR key with no input after it: off
// again 10 s later, with no dim step.
void test_pocket_guard() {
  const W whys[2] = {W::Touch, W::PowerKey};
  for (W why : whys) {
    ScreenPower s;
    s.begin(0);
    s.step(30000, false);
    TEST_ASSERT_TRUE(s.off());
    TEST_ASSERT_TRUE(s.wake(100000, why));
    TEST_ASSERT_TRUE(s.step(100000, false));
    TEST_ASSERT_TRUE(s.bright());
    TEST_ASSERT_TRUE(s.pocketGuard());
    TEST_ASSERT_EQUAL_UINT32(10000, s.msUntilNext(100000));
    TEST_ASSERT_EQUAL_INT(0, run(s, 100050, 109950));
    TEST_ASSERT_TRUE(s.bright());  // no dim step
    TEST_ASSERT_TRUE(s.step(110000, false));
    TEST_ASSERT_TRUE(s.off());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Bright), static_cast<int>(s.previous()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PocketGuard), static_cast<int>(s.why()));
    TEST_ASSERT_FALSE(s.pocketGuard());
  }
  // Input after the wake ends the guard: the normal countdown from there.
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  s.wake(100000, W::Touch);
  s.step(100000, false);
  s.activity(105000);
  TEST_ASSERT_FALSE(s.pocketGuard());
  s.step(124999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(125000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  s.step(135000, false);
  TEST_ASSERT_TRUE(s.off());
  // With Never chosen there is no guard: the listener wants it on.
  ScreenPower n;
  n.begin(0);
  n.setTimeout(ScreenPower::kNever, 0);
  n.turnOff(W::Console);
  n.step(1000, false);
  n.wake(2000, W::Touch);
  TEST_ASSERT_FALSE(n.pocketGuard());
  TEST_ASSERT_EQUAL_INT(1, run(n, 2000, 600000));  // the wake itself only
  TEST_ASSERT_TRUE(n.bright());
}

// An event that needs the listener (the headphones lost, "Couldn't reach",
// a failed track, USB) wakes it with the whole countdown, and ends a guard.
void test_event_wake_gets_the_whole_countdown() {
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_TRUE(s.wake(50000, W::Event));
  TEST_ASSERT_TRUE(s.step(50000, false));
  TEST_ASSERT_FALSE(s.pocketGuard());
  s.step(69999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(70000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  // An event while bright: not a wake, but the countdown starts again.
  ScreenPower b;
  b.begin(0);
  b.step(15000, false);
  TEST_ASSERT_FALSE(b.wake(15000, W::Event));
  b.step(34999, false);
  TEST_ASSERT_TRUE(b.bright());
  // An event during a pocket guard: no guard any more.
  ScreenPower g;
  g.begin(0);
  g.turnOff(W::Console);
  g.step(0, false);
  g.wake(1000, W::PowerKey);
  TEST_ASSERT_TRUE(g.pocketGuard());
  g.wake(2000, W::Event);
  TEST_ASSERT_FALSE(g.pocketGuard());
  g.step(15000, false);
  TEST_ASSERT_TRUE(g.bright());
}

// Kept lit (a screen of its own, a play waiting for the headphones, a
// pairing): no dim, no off; one that starts while Off lights it; the
// countdown runs from when it ends.
void test_keep_lit() {
  ScreenPower s;
  s.begin(0);
  TEST_ASSERT_EQUAL_INT(0, run(s, 0, 100000, /*keepLit=*/true));
  TEST_ASSERT_TRUE(s.bright());
  s.step(119999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(120000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  s.step(130000, false);
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_TRUE(s.step(131000, true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::KeepLit), static_cast<int>(s.why()));
}

// The console's Ps0, and the settings: a new timeout restarts it bright; the
// console's backlight override is the Bright level until cleared.
void test_turn_off_settings_and_override() {
  ScreenPower s;
  s.begin(0);
  s.turnOff(W::Console);
  TEST_ASSERT_TRUE(s.step(10, false));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Console), static_cast<int>(s.why()));
  TEST_ASSERT_TRUE(s.wake(500, W::Touch));
  s.step(500, false);
  TEST_ASSERT_TRUE(s.pocketGuard());
  s.step(19000, false);  // the guard: off again at 10.5 s
  TEST_ASSERT_TRUE(s.off());
  // Ps1 (someone at the console): the whole countdown, no guard.
  TEST_ASSERT_TRUE(s.wake(20000, W::Console));
  TEST_ASSERT_FALSE(s.pocketGuard());
  s.step(39999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(40000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));

  ScreenPower t;
  t.begin(0);
  t.step(25000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(t.level()));
  t.setTimeout(3, 26000);  // 2 min
  TEST_ASSERT_TRUE(t.step(26000, false));
  TEST_ASSERT_TRUE(t.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Setting), static_cast<int>(t.why()));
  t.step(26000 + 109999, false);
  TEST_ASSERT_TRUE(t.bright());
  t.step(26000 + 110000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(t.level()));
  TEST_ASSERT_EQUAL_UINT8(30, t.backlight());

  ScreenPower b;
  b.begin(0);
  b.setBrightness(3);
  TEST_ASSERT_EQUAL_UINT8(255, b.backlight());
  b.overrideBacklight(127);
  TEST_ASSERT_EQUAL_UINT8(127, b.backlight());
  b.overrideBacklight(0);
  TEST_ASSERT_EQUAL_UINT8(255, b.backlight());
  b.setBrightness(7);  // damaged: Medium
  TEST_ASSERT_EQUAL_INT(1, b.brightness());
}

// millis() wraps after 49.7 days: the countdown doesn't care.
void test_wraparound() {
  ScreenPower s;
  const uint32_t t0 = 0xFFFFFFFFu - 5000;
  s.begin(t0);
  s.step(t0 + 19999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(t0 + 20000, false);  // wrapped past 0
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  s.step(t0 + 30000, false);
  TEST_ASSERT_TRUE(s.off());
  s.wake(t0 + 40000, W::Touch);
  s.step(t0 + 49999, false);
  TEST_ASSERT_TRUE(s.bright());
  s.step(t0 + 50000, false);
  TEST_ASSERT_TRUE(s.off());
}

// The pocket rule: after any wake from Off (a touch, the key, an event,
// keepLit) unattended() until the glass is touched (attend()), the
// console, or Off again. From Dim or Bright nothing changes. An event's
// wake from Off also leaves touchActs() false until a touch answers it.
void test_unattended_and_the_event_answer() {
  const W whys[3] = {W::Touch, W::PowerKey, W::Event};
  for (W why : whys) {
    ScreenPower s;
    s.begin(0);
    TEST_ASSERT_FALSE(s.unattended());
    TEST_ASSERT_TRUE(s.touchActs());
    s.step(30000, false);
    TEST_ASSERT_TRUE(s.off());
    TEST_ASSERT_FALSE(s.touchActs());
    s.wake(40000, why);
    s.step(40000, false);
    TEST_ASSERT_TRUE(s.bright());
    TEST_ASSERT_TRUE(s.unattended());
    TEST_ASSERT_EQUAL(why != W::Event, s.touchActs());
    // Strip input (activity) doesn't attend; it ends the pocket guard.
    s.activity(41000);
    TEST_ASSERT_TRUE(s.unattended());
    if (why == W::Event) {
      // The first touch answers the event (the input layer swallowed it):
      // touches act from here, still unattended, no pocket guard.
      TEST_ASSERT_FALSE(s.wake(42000, W::Touch));
      TEST_ASSERT_TRUE(s.touchActs());
      TEST_ASSERT_FALSE(s.pocketGuard());
      TEST_ASSERT_TRUE(s.unattended());
    }
    s.attend();  // a touch landed on the glass
    TEST_ASSERT_FALSE(s.unattended());
    TEST_ASSERT_TRUE(s.touchActs());
  }
  // Off ends it: unattended until then, and after the next wake afresh.
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  s.wake(40000, W::Event);
  s.step(40000, false);
  TEST_ASSERT_EQUAL_INT(2, run(s, 40000, 80000));  // dim, off (the whole countdown)
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_FALSE(s.unattended());
  // From Dim: a touch's wake isn't unattended (it was lit moments ago).
  ScreenPower d;
  d.begin(0);
  d.step(25000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));
  TEST_ASSERT_FALSE(d.touchActs());
  d.wake(25000, W::Touch);
  TEST_ASSERT_FALSE(d.unattended());
  TEST_ASSERT_TRUE(d.touchActs());
  // An event's wake from Dim: lit, touches act.
  d.step(50000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));
  d.wake(50000, W::Event);
  TEST_ASSERT_TRUE(d.touchActs());
  TEST_ASSERT_FALSE(d.unattended());
  // The console (Ps1) is someone there: neither.
  ScreenPower c;
  c.begin(0);
  c.step(30000, false);
  c.wake(40000, W::Event);
  c.wake(41000, W::Console);
  TEST_ASSERT_TRUE(c.touchActs());
  TEST_ASSERT_FALSE(c.unattended());
  // keepLit from Off (a play waiting, a pairing): as an event's wake.
  ScreenPower k;
  k.begin(0);
  k.step(30000, false);
  TEST_ASSERT_TRUE(k.step(31000, true));
  TEST_ASSERT_TRUE(k.bright());
  TEST_ASSERT_TRUE(k.unattended());
  TEST_ASSERT_FALSE(k.touchActs());
}

// The wake latch alone: a finger landing while a touch doesn't act is a
// wake; held until no finger for kQuietMs, whatever the pass rate (a gap
// with no passes too); a finger already on as it dims is taken, not a
// wake; wraparound-safe.
void test_wake_latch_timing() {
  WakeLatch l;
  WakeLatch::Result r = l.update(1000, true, false);
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_TRUE(r.woke);
  TEST_ASSERT_FALSE(r.took);
  r = l.update(1001, true, true);  // lit now: still held
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_FALSE(r.woke);
  for (uint32_t t = 1002; t < 1300; ++t) TEST_ASSERT_TRUE(l.update(t, false, true).hold);  // lost ~300 ms
  TEST_ASSERT_TRUE(l.update(1300, true, true).hold);  // found again: still the waking touch
  TEST_ASSERT_TRUE(l.update(1300 + WakeLatch::kQuietMs - 1, false, true).hold);
  r = l.update(1300 + WakeLatch::kQuietMs, false, true);
  TEST_ASSERT_FALSE(r.hold);
  TEST_ASSERT_FALSE(l.holding());
  // Lit: fingers act.
  TEST_ASSERT_FALSE(l.update(3000, true, true).hold);
  // Resting there as the screen dims: taken, not a wake.
  r = l.update(3010, true, false);
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_TRUE(r.took);
  TEST_ASSERT_FALSE(r.woke);
  // No passes for a while (a stalled loop), then a finger: a new touch.
  r = l.update(3010 + WakeLatch::kQuietMs + 50, true, false);
  TEST_ASSERT_TRUE(r.hold);
  TEST_ASSERT_TRUE(r.woke);
  r = l.update(3010 + 2 * WakeLatch::kQuietMs + 100, true, true);
  TEST_ASSERT_FALSE(r.hold);  // ... and when lit by then, it acts
  // Across the clock's wrap.
  WakeLatch w;
  const uint32_t t0 = 0xFFFFFFFFu - 100;
  TEST_ASSERT_TRUE(w.update(t0, true, false).woke);
  TEST_ASSERT_TRUE(w.update(t0 + 50, false, true).hold);
  TEST_ASSERT_TRUE(w.update(t0 + WakeLatch::kQuietMs - 1, false, true).hold);
  TEST_ASSERT_FALSE(w.update(t0 + WakeLatch::kQuietMs, false, true).hold);
}

// A finger counts as input when it lands and when it moves; one resting
// within kMovePx stops counting after kStillCapMs.
void test_finger_activity() {
  FingerActivity f;
  TEST_ASSERT_FALSE(f.update(0, false, 0, 0));
  TEST_ASSERT_TRUE(f.update(1000, true, 100, 100));
  TEST_ASSERT_TRUE(f.update(1000 + FingerActivity::kStillCapMs - 1, true, 100 + FingerActivity::kMovePx, 100));
  TEST_ASSERT_FALSE(f.update(1000 + FingerActivity::kStillCapMs, true, 100, 100 - FingerActivity::kMovePx));
  TEST_ASSERT_FALSE(f.update(90000, true, 101, 99));
  TEST_ASSERT_TRUE(f.update(90010, true, 100, 100 + FingerActivity::kMovePx + 1));  // it moved
  TEST_ASSERT_TRUE(f.update(90010 + FingerActivity::kStillCapMs - 1, true, 100, 109));
  TEST_ASSERT_FALSE(f.update(90010 + FingerActivity::kStillCapMs, true, 100, 109));
  TEST_ASSERT_FALSE(f.update(200000, false, 0, 0));  // the lift doesn't count
  TEST_ASSERT_TRUE(f.update(200010, true, 100, 109));  // a new landing does
  // Wraparound.
  FingerActivity w;
  const uint32_t t0 = 0xFFFFFFFFu - 100;
  TEST_ASSERT_TRUE(w.update(t0, true, 5, 5));
  TEST_ASSERT_TRUE(w.update(t0 + FingerActivity::kStillCapMs - 1, true, 5, 5));
  TEST_ASSERT_FALSE(w.update(t0 + FingerActivity::kStillCapMs, true, 5, 5));
}

void test_names() {
  TEST_ASSERT_EQUAL_STRING("bright", ScreenPower::name(L::Bright));
  TEST_ASSERT_EQUAL_STRING("dim", ScreenPower::name(L::Dim));
  TEST_ASSERT_EQUAL_STRING("off", ScreenPower::name(L::Off));
  TEST_ASSERT_EQUAL_STRING("glance", ScreenPower::name(L::Glance));
  TEST_ASSERT_TRUE(std::strlen(ScreenPower::name(W::Glance)) > 0);
  TEST_ASSERT_TRUE(std::strlen(ScreenPower::name(W::PocketGuard)) > 0);
  TEST_ASSERT_TRUE(std::strlen(ScreenPower::name(W::HoldLit)) > 0);
}

// The touch that attends a screen woken from off is remembered for that
// touch (landedUnattended()): a pocket's second contact lands on a lit
// fade toast, and must not act on +10 min or Turn off. The next touch was
// made on an attended screen. PWR on a screen kept lit attends too, but no
// touch landed then.
void test_the_touch_that_attends_is_remembered() {
  for (W why : {W::Touch, W::PowerKey, W::Event}) {
    ScreenPower s;
    s.begin(0);
    s.step(30000, false);
    TEST_ASSERT_TRUE(s.off());
    s.wake(40000, why);
    s.step(40000, false);
    if (why == W::Event) s.wake(40500, W::Touch);  // the answer (swallowed)
    TEST_ASSERT_TRUE(s.unattended());
    s.glassLanded();  // the next contact: it acts, but it is the one that attended
    TEST_ASSERT_FALSE(s.unattended());
    TEST_ASSERT_TRUE(s.landedUnattended());
    s.glassLanded();  // another touch: made on an attended screen
    TEST_ASSERT_FALSE(s.landedUnattended());
  }
  // Lit all along (no wake from off): no touch lands unattended.
  ScreenPower s;
  s.begin(0);
  s.glassLanded();
  TEST_ASSERT_FALSE(s.landedUnattended());
  // Attended by the key while kept lit: the next touch lands attended.
  s.step(30000, false);
  s.wake(40000, W::PowerKey);
  s.step(40000, false);
  TEST_ASSERT_TRUE(s.unattended());
  s.powerKey(41000, /*keepLit=*/true);
  TEST_ASSERT_FALSE(s.unattended());
  s.glassLanded();
  TEST_ASSERT_FALSE(s.landedUnattended());
}

// The PWR key's short press: a lit screen goes off at once, from Bright (no
// dim step) and from Dim, and stays off with nothing else happening; the
// next press wakes it, as a key's wake from Off always was (the pocket
// guard, the pocket rule), and the one after turns it off again.
void test_power_key_turns_a_lit_screen_off() {
  ScreenPower s;
  s.begin(0);
  s.step(0, false);
  TEST_ASSERT_FALSE(s.powerKey(1000, false));  // (not a wake)
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_UINT8(0, s.backlight());
  TEST_ASSERT_FALSE(s.touchActs());  // (the next touch only wakes it, in this very pass too)
  TEST_ASSERT_TRUE(s.step(1000, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Bright), static_cast<int>(s.previous()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PowerKey), static_cast<int>(s.why()));
  TEST_ASSERT_EQUAL_INT(0, run(s, 1050, 600000));
  TEST_ASSERT_TRUE(s.off());

  TEST_ASSERT_TRUE(s.powerKey(700000, false));  // a wake
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.touchActs());
  TEST_ASSERT_TRUE(s.pocketGuard());
  TEST_ASSERT_TRUE(s.unattended());
  TEST_ASSERT_TRUE(s.step(700000, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PowerKey), static_cast<int>(s.why()));

  // Again, under the pocket guard: off, the guard and the pocket rule gone.
  TEST_ASSERT_FALSE(s.powerKey(702000, false));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_FALSE(s.pocketGuard());
  TEST_ASSERT_FALSE(s.unattended());
  TEST_ASSERT_TRUE(s.step(702000, false));
  TEST_ASSERT_EQUAL_INT(0, run(s, 702050, 720000));

  // From Dim: off, not bright again (a touch there is what brightens it).
  ScreenPower d;
  d.begin(0);
  d.step(25000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));
  TEST_ASSERT_FALSE(d.powerKey(26000, false));
  TEST_ASSERT_TRUE(d.off());
  TEST_ASSERT_TRUE(d.step(26000, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.previous()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PowerKey), static_cast<int>(d.why()));
  TEST_ASSERT_EQUAL_INT(0, run(d, 26050, 90000));
  TEST_ASSERT_TRUE(d.off());

  // With Never it is how the screen goes off, and it stays off: the wake
  // has no guard (the listener wants it on).
  ScreenPower n;
  n.begin(0);
  n.setTimeout(ScreenPower::kNever, 0);
  n.step(0, false);
  n.powerKey(5000, false);
  TEST_ASSERT_TRUE(n.step(5000, false));
  TEST_ASSERT_TRUE(n.off());
  TEST_ASSERT_EQUAL_INT(0, run(n, 5050, 3600000));
  TEST_ASSERT_TRUE(n.off());
  TEST_ASSERT_TRUE(n.powerKey(3700000, false));
  TEST_ASSERT_FALSE(n.pocketGuard());
  TEST_ASSERT_TRUE(n.step(3700000, false));
  TEST_ASSERT_EQUAL_INT(0, run(n, 3700050, 7300000));
  TEST_ASSERT_TRUE(n.bright());
}

// An event's wake from Off, nobody answered yet: the key puts it away, the
// answer's state with it (the dialog stays up for the next wake). The next
// wake is a key's or a touch's, not an event's: touches act.
void test_power_key_on_an_event_wake() {
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  TEST_ASSERT_TRUE(s.wake(40000, W::Event));
  s.step(40000, false);
  TEST_ASSERT_FALSE(s.touchActs());
  TEST_ASSERT_TRUE(s.unattended());
  TEST_ASSERT_FALSE(s.powerKey(41000, false));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_FALSE(s.unattended());
  TEST_ASSERT_TRUE(s.step(41000, false));
  TEST_ASSERT_TRUE(s.powerKey(50000, false));
  TEST_ASSERT_TRUE(s.touchActs());
  TEST_ASSERT_TRUE(s.unattended());
}

// What keeps it lit wins: the screen stays on, with no off and back, which
// would leave it unattended with touches swallowed. The press is input: it
// answers keepLit's wake from Off (touches act), else the pocket rule ends.
void test_power_key_on_a_screen_kept_lit() {
  ScreenPower s;
  s.begin(0);
  s.step(0, true);
  TEST_ASSERT_FALSE(s.powerKey(1000, true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.touchActs());
  TEST_ASSERT_FALSE(s.unattended());
  TEST_ASSERT_FALSE(s.step(1000, true));  // no change to log
  TEST_ASSERT_EQUAL_INT(0, run(s, 1050, 100000, /*keepLit=*/true));
  TEST_ASSERT_TRUE(s.bright());

  // keepLit lit it from Off: only answers it (touches act from here), and
  // the next press attends. No wake: it was Bright.
  ScreenPower k;
  k.begin(0);
  k.step(30000, false);
  TEST_ASSERT_TRUE(k.off());
  TEST_ASSERT_TRUE(k.step(31000, true));
  TEST_ASSERT_FALSE(k.touchActs());
  TEST_ASSERT_TRUE(k.unattended());
  TEST_ASSERT_FALSE(k.powerKey(32000, true));
  TEST_ASSERT_TRUE(k.bright());
  TEST_ASSERT_TRUE(k.touchActs());
  TEST_ASSERT_TRUE(k.unattended());
  TEST_ASSERT_FALSE(k.powerKey(33000, true));
  TEST_ASSERT_TRUE(k.bright());
  TEST_ASSERT_FALSE(k.unattended());
  TEST_ASSERT_FALSE(k.step(33000, true));

  // What keeps it lit ends: the key turns it off again, and the countdown
  // ran from its end until then.
  TEST_ASSERT_EQUAL_INT(0, run(k, 33050, 50000, false));
  TEST_ASSERT_FALSE(k.powerKey(50050, false));
  TEST_ASSERT_TRUE(k.off());

  // The one off and back: keepLit comes on in the key's own pass (the key
  // read last pass's). Lit again, as keepLit's own wake from Off: the next
  // touch only answers it. A window of one pass, and it errs on the safe side.
  ScreenPower r;
  r.begin(0);
  r.step(0, false);
  r.powerKey(1000, false);
  TEST_ASSERT_TRUE(r.step(1000, true));
  TEST_ASSERT_TRUE(r.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::KeepLit), static_cast<int>(r.why()));
  TEST_ASSERT_TRUE(r.unattended());
  TEST_ASSERT_FALSE(r.touchActs());
}

// The panel still in its sleep-in as the policy has it on (the sleep-out
// waits 120 ms after the sleep-in): it looks off, so a press means on. It
// stays on, and isn't a new wake (it was Bright already).
void test_power_key_while_the_panel_still_sleeps() {
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  TEST_ASSERT_TRUE(s.wake(40000, W::Touch));
  s.step(40000, false);
  TEST_ASSERT_FALSE(s.powerKey(40050, false, /*panelAsleep=*/true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_FALSE(s.step(40050, false));
  // Kept lit as well: on, and no attending a screen that still looks off.
  TEST_ASSERT_FALSE(s.powerKey(40060, true, true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.unattended());
  // Awake: the next press turns it off.
  TEST_ASSERT_FALSE(s.powerKey(40200, false, false));
  TEST_ASSERT_TRUE(s.off());
}

// A countdown toast or the Pair screen's search holds a lit screen lit, and
// the key turns it off anyway: the listener asked. It stays off through the
// hold (it may be night), and the key's wake from there has the pocket guard,
// not the hold, until input follows.
void test_power_key_during_a_hold() {
  ScreenPower s;
  s.begin(0);
  TEST_ASSERT_EQUAL_INT(0, run(s, 0, 40000, false, /*holdLit=*/true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_FALSE(s.powerKey(40050, false));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_TRUE(s.step(40050, false, true));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PowerKey), static_cast<int>(s.why()));
  TEST_ASSERT_EQUAL_INT(0, run(s, 40100, 80000, false, true));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_TRUE(s.powerKey(80050, false));
  TEST_ASSERT_TRUE(s.step(80050, false, true));
  TEST_ASSERT_TRUE(s.pocketGuard());
  TEST_ASSERT_EQUAL_INT(0, run(s, 80100, 90000, false, true));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.step(90050, false, true));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PocketGuard), static_cast<int>(s.why()));
}

// Held lit (a countdown toast: the idle power-off's warning, the sleep
// timer's fade): a lit screen stays Bright until it ends, then the whole
// countdown from there. The bug it fixes: the warning came up 1 s before
// the dim, and the screen went off 20 s before the power-off.
void test_hold_lit_keeps_a_lit_screen_lit() {
  ScreenPower s;
  s.begin(0);
  // The warning at 19 s (the dim was due at 20 s), 30 s long.
  TEST_ASSERT_EQUAL_INT(0, run(s, 0, 19000));
  TEST_ASSERT_EQUAL_INT(0, run(s, 19050, 49000, false, /*holdLit=*/true));
  TEST_ASSERT_TRUE(s.bright());
  // Gone (the power-off didn't come: USB, say): the countdown from its end.
  TEST_ASSERT_EQUAL_INT(0, run(s, 49050, 68950));
  TEST_ASSERT_TRUE(s.bright());
  s.step(69000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
  s.step(79000, false);
  TEST_ASSERT_TRUE(s.off());

  // Dim when it comes up: Bright at once (a tap on Keep on then acts), and
  // held.
  ScreenPower d;
  d.begin(0);
  d.step(25000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));
  TEST_ASSERT_TRUE(d.step(25050, false, true));
  TEST_ASSERT_TRUE(d.bright());
  TEST_ASSERT_TRUE(d.touchActs());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::HoldLit), static_cast<int>(d.why()));
  TEST_ASSERT_EQUAL_INT(0, run(d, 25100, 40000, false, true));
  // Keep on: input, which ends the warning; the countdown from the tap.
  d.activity(40000);
  d.step(40050, false);
  TEST_ASSERT_TRUE(d.bright());
  d.step(59999, false);
  TEST_ASSERT_TRUE(d.bright());
  d.step(60000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));

  // Never: nothing to hold, Bright either way.
  ScreenPower n;
  n.begin(0);
  n.setTimeout(ScreenPower::kNever, 0);
  n.step(0, false);
  TEST_ASSERT_EQUAL_INT(0, run(n, 0, 60000, false, true));
  TEST_ASSERT_TRUE(n.bright());
}

// An Off screen stays off through it (it may be night), the sleep timer's
// turnOff() at its pause turns a held screen off, and keepLit still lights
// one from Off.
void test_hold_lit_leaves_an_off_screen_off() {
  ScreenPower s;
  s.begin(0);
  s.step(30000, false);
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_INT(0, run(s, 30050, 60050, false, true));
  TEST_ASSERT_TRUE(s.off());

  ScreenPower t;
  t.begin(0);
  run(t, 0, 40000, false, true);  // the fade, on a lit screen
  TEST_ASSERT_TRUE(t.bright());
  t.turnOff(W::SleepTimer);  // its pause
  TEST_ASSERT_TRUE(t.step(40050, false, true));
  TEST_ASSERT_TRUE(t.off());
  TEST_ASSERT_EQUAL_INT(0, run(t, 40100, 50000, false, true));
  TEST_ASSERT_TRUE(t.off());
  TEST_ASSERT_TRUE(t.step(50050, true, true));
  TEST_ASSERT_TRUE(t.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::KeepLit), static_cast<int>(t.why()));
}

// Woken during it. By an event (no input): held from the wake. By a touch
// or the key from Off: the pocket guard stands, held or not (off 10 s
// later); input after the wake ends the guard and the hold takes over.
// For the idle warning the swallowed wake is input itself: the warning
// (and the hold) ends in that pass, and the screen follows the guard,
// then the normal countdown once input follows.
void test_hold_lit_and_a_wake_during_it() {
  ScreenPower e;
  e.begin(0);
  e.step(30000, false);
  TEST_ASSERT_TRUE(e.wake(40000, W::Event));
  TEST_ASSERT_TRUE(e.step(40000, false, true));
  TEST_ASSERT_EQUAL_INT(0, run(e, 40050, 100000, false, true));  // past its own 30 s
  TEST_ASSERT_TRUE(e.bright());
  TEST_ASSERT_FALSE(e.touchActs());  // (still only answered by a touch: the event rule)

  for (W why : {W::Touch, W::PowerKey}) {
    ScreenPower p;
    p.begin(0);
    p.step(30000, false);
    TEST_ASSERT_TRUE(p.wake(40000, why));
    TEST_ASSERT_TRUE(p.step(40000, false, true));
    TEST_ASSERT_TRUE(p.pocketGuard());
    TEST_ASSERT_TRUE(p.unattended());
    TEST_ASSERT_EQUAL_INT(0, run(p, 40050, 49950, false, true));
    TEST_ASSERT_TRUE(p.step(50000, false, true));
    TEST_ASSERT_TRUE(p.off());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PocketGuard), static_cast<int>(p.why()));
    // Woken again, and this time a tap follows: held until it ends.
    p.wake(60000, why);
    p.step(60000, false, true);
    p.activity(62000);
    TEST_ASSERT_FALSE(p.pocketGuard());
    TEST_ASSERT_EQUAL_INT(0, run(p, 62050, 120000, false, true));
    TEST_ASSERT_TRUE(p.bright());
    p.step(139999, false);  // the countdown from the hold's last pass
    TEST_ASSERT_TRUE(p.bright());
    p.step(140000, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(p.level()));
  }

  // The idle warning, off screen: the swallowed wake ends it (input).
  ScreenPower w;
  w.begin(0);
  w.step(30000, false);
  run(w, 30050, 40000, false, true);
  TEST_ASSERT_TRUE(w.off());
  w.wake(40050, W::Touch);
  w.step(40050, false, /*holdLit=*/false);
  TEST_ASSERT_TRUE(w.pocketGuard());
  w.activity(45000);  // a tap on the lit screen: the guard ends
  w.step(64999, false);
  TEST_ASSERT_TRUE(w.bright());
  w.step(65000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(w.level()));
  w.step(75000, false);
  TEST_ASSERT_TRUE(w.off());
}

// The Pair screen's search (2 min at most) holds the screen as a toast
// does (main.cpp, ScreenControl::Hold::PairSearch). The 2026-10-04 run
// without it: the search began at a tap, the screen dimmed 20 s later,
// each tap on the dim screen only woke it (swallowed), and at 81 s it went
// off, which stopped the search. Held: Bright for the whole search with
// every choice, every touch acts, then the normal countdown from its end.
void test_hold_lit_through_the_pair_search() {
  for (int choice : {0, ScreenPower::kDefaultTimeout, 4}) {
    ScreenPower s;
    s.begin(0);
    s.setTimeout(choice, 0);
    s.step(0, false);
    s.activity(1000);  // the tap that opens the Pair screen
    for (uint32_t t = 1000; t <= 121000; t += 50) {
      TEST_ASSERT_FALSE(s.step(t, false, /*holdLit=*/true));  // no dim, no off
      TEST_ASSERT_TRUE(s.touchActs());  // a tap on the list acts
    }
    TEST_ASSERT_TRUE(s.bright());
    // The search stopped (its 2 minutes): the countdown from there.
    const uint32_t off = ScreenPower::timeoutMs(choice);
    s.step(121000 + ScreenPower::dimAtMs(off) - 50, false);
    TEST_ASSERT_TRUE(s.bright());
    s.step(121000 + ScreenPower::dimAtMs(off), false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(s.level()));
    s.step(121000 + off, false);
    TEST_ASSERT_TRUE(s.off());
  }
  // The sleep timer's pause during the search: off (the page then stops
  // the search). The wake after: a pocket guard, nothing held any more.
  ScreenPower t;
  t.begin(0);
  run(t, 0, 30000, false, true);
  t.turnOff(W::SleepTimer);
  TEST_ASSERT_TRUE(t.step(30050, false, true));
  TEST_ASSERT_TRUE(t.off());
  TEST_ASSERT_EQUAL_INT(0, run(t, 30100, 31000, false, true));  // (until the page has stopped it)
  TEST_ASSERT_TRUE(t.off());
  t.wake(40000, W::Touch);
  t.step(40000, false);
  TEST_ASSERT_TRUE(t.pocketGuard());
  t.step(50000, false);
  TEST_ASSERT_TRUE(t.off());
}

// ---- "Touch when off" (touch(): the swallowed touch, by the choice) ----

using T = ScreenPower::Touched;

// Off by the countdown at 30 s (the default), with `choice`.
ScreenPower offWith(int choice) {
  ScreenPower s;
  s.begin(0);
  s.setOffTouch(choice);
  s.step(30000, false);
  TEST_ASSERT_TRUE(s.off());
  return s;
}

void test_off_touch_choices_and_labels() {
  TEST_ASSERT_EQUAL_INT(3, ScreenPower::kOffTouches);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPower::OffTouch::Nothing), ScreenPower::kDefaultOffTouch);
  TEST_ASSERT_EQUAL_STRING("Nothing", ScreenPower::offTouchLabel(0));
  TEST_ASSERT_EQUAL_STRING("Wake", ScreenPower::offTouchLabel(1));
  TEST_ASSERT_EQUAL_STRING("Track", ScreenPower::offTouchLabel(2));
  // Out of range (a damaged NVS value): the default, Nothing.
  TEST_ASSERT_EQUAL_INT(0, ScreenPower::clampOffTouch(3));
  TEST_ASSERT_EQUAL_INT(0, ScreenPower::clampOffTouch(-1));
  TEST_ASSERT_EQUAL_STRING("Nothing", ScreenPower::offTouchLabel(9));
  ScreenPower s;
  s.begin(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPower::OffTouch::Nothing), static_cast<int>(s.offTouch()));
  s.setOffTouch(2);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPower::OffTouch::Glance), static_cast<int>(s.offTouch()));
  s.setOffTouch(7);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPower::OffTouch::Nothing), static_cast<int>(s.offTouch()));
  // The choice changes nothing now (the row is tapped on a lit screen).
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_FALSE(s.step(100, false));
}

// The default: a touch on an Off screen does nothing at all (no wake, no
// countdown, no pocket guard, nobody looking), as often as it lands. The
// PWR key still wakes it; Dim isn't off (a touch brightens it); an event's
// wake is still answered by a touch.
void test_a_touch_on_an_off_screen_does_nothing_by_default() {
  ScreenPower s = offWith(ScreenPower::kDefaultOffTouch);
  for (uint32_t t = 40000; t < 45000; t += 500) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Ignored), static_cast<int>(s.touch(t)));
    TEST_ASSERT_FALSE(s.step(t, false));
  }
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_FALSE(s.pocketGuard());
  TEST_ASSERT_FALSE(s.unattended());
  TEST_ASSERT_EQUAL_UINT8(0, s.backlight());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Timeout), static_cast<int>(s.why()));  // (still the countdown's)
  // The PWR key: the wake, with the pocket guard and the pocket rule.
  TEST_ASSERT_TRUE(s.powerKey(50000, false));
  TEST_ASSERT_TRUE(s.step(50000, false));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_TRUE(s.pocketGuard());
  TEST_ASSERT_TRUE(s.unattended());
  // Dim: a touch brightens it (swallowed), with no guard and nobody unattended.
  ScreenPower d;
  d.begin(0);
  d.step(21000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Dim), static_cast<int>(d.level()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Woke), static_cast<int>(d.touch(22000)));
  TEST_ASSERT_TRUE(d.step(22000, false));
  TEST_ASSERT_TRUE(d.bright());
  TEST_ASSERT_FALSE(d.pocketGuard());
  TEST_ASSERT_FALSE(d.unattended());
  TEST_ASSERT_EQUAL_UINT32(20000, d.msUntilNext(22000));  // the whole countdown again
  // An event's wake from Off: the first touch only answers it.
  ScreenPower e = offWith(ScreenPower::kDefaultOffTouch);
  e.wake(40000, W::Event);
  e.step(40000, false);
  TEST_ASSERT_FALSE(e.touchActs());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Answered), static_cast<int>(e.touch(41000)));
  TEST_ASSERT_FALSE(e.step(41000, false));
  TEST_ASSERT_TRUE(e.touchActs());
  TEST_ASSERT_TRUE(e.unattended());  // (the next landing on the glass attends)
}

// Wake: a touch on an Off screen wakes it, as every touch did before the
// setting (the pocket guard, the pocket rule; no guard with Never).
void test_the_wake_choice_is_the_old_touch() {
  ScreenPower s = offWith(static_cast<int>(ScreenPower::OffTouch::Wake));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Woke), static_cast<int>(s.touch(40000)));
  TEST_ASSERT_TRUE(s.step(40000, false));
  TEST_ASSERT_TRUE(s.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Touch), static_cast<int>(s.why()));
  TEST_ASSERT_TRUE(s.pocketGuard());
  TEST_ASSERT_TRUE(s.unattended());
  TEST_ASSERT_TRUE(s.touchActs());  // (the latch, Input's, still holds this touch through its lift)
  s.step(50000, false);
  TEST_ASSERT_TRUE(s.off());
  ScreenPower n;
  n.begin(0);
  n.setOffTouch(1);
  n.setTimeout(ScreenPower::kNever, 0);
  n.turnOff(W::Console);
  n.step(1000, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Woke), static_cast<int>(n.touch(2000)));
  TEST_ASSERT_FALSE(n.pocketGuard());
  TEST_ASSERT_EQUAL_INT(1, run(n, 2000, 600000));  // the wake itself only
  TEST_ASSERT_TRUE(n.bright());
}

// Track: a touch on an Off screen shows the glance: lit at the Bright
// level, a touch never acts on it, nobody has looked yet, and Off again 10 s
// after its last touch. Activity can't keep it (nothing on it acts), nor can
// a hold (the toast isn't on it), nor Never.
void test_the_glance() {
  ScreenPower s = offWith(static_cast<int>(ScreenPower::OffTouch::Glance));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Glance), static_cast<int>(s.touch(40000)));
  TEST_ASSERT_TRUE(s.step(40000, false));
  TEST_ASSERT_TRUE(s.glance());
  TEST_ASSERT_FALSE(s.off());
  TEST_ASSERT_FALSE(s.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Off), static_cast<int>(s.previous()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Touch), static_cast<int>(s.why()));
  TEST_ASSERT_EQUAL_UINT8(100, s.backlight());  // the Bright level
  TEST_ASSERT_FALSE(s.touchActs());
  TEST_ASSERT_TRUE(s.unattended());
  TEST_ASSERT_FALSE(s.pocketGuard());
  TEST_ASSERT_EQUAL_UINT32(10000, s.msUntilNext(40000));
  TEST_ASSERT_EQUAL_INT(0, run(s, 40050, 44950));
  // Another touch: 10 s more, no change of level.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::GlanceAgain), static_cast<int>(s.touch(45000)));
  TEST_ASSERT_FALSE(s.step(45000, false));
  TEST_ASSERT_EQUAL_UINT32(10000, s.msUntilNext(45000));
  s.activity(50000);  // (no effect on the glance)
  TEST_ASSERT_EQUAL_INT(0, run(s, 45050, 54950, false, true));  // held? no: nothing on it to read
  TEST_ASSERT_TRUE(s.glance());  // (and no dim step)
  TEST_ASSERT_TRUE(s.step(55000, false, true));
  TEST_ASSERT_TRUE(s.off());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Glance), static_cast<int>(s.previous()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::Glance), static_cast<int>(s.why()));
  TEST_ASSERT_FALSE(s.unattended());  // Off: the next wake decides again
  // The next touch: the glance again.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Glance), static_cast<int>(s.touch(60000)));
  TEST_ASSERT_TRUE(s.step(60000, false));
  TEST_ASSERT_TRUE(s.glance());
  // With Never: still 10 s. The console's Pb shows on it too.
  ScreenPower n;
  n.begin(0);
  n.setOffTouch(2);
  n.setTimeout(ScreenPower::kNever, 0);
  n.overrideBacklight(200);
  n.turnOff(W::Console);
  n.step(1000, false);
  n.touch(2000);
  TEST_ASSERT_TRUE(n.step(2000, false));
  TEST_ASSERT_EQUAL_UINT8(200, n.backlight());
  TEST_ASSERT_EQUAL_INT(0, run(n, 2050, 11950));
  TEST_ASSERT_TRUE(n.step(12000, false));
  TEST_ASSERT_TRUE(n.off());
  // Wraparound-safe.
  const uint32_t t0 = 0xFFFFF000u;
  ScreenPower w;
  w.begin(t0 - 30000);
  w.setOffTouch(2);
  w.step(t0, false);
  TEST_ASSERT_TRUE(w.off());
  w.touch(t0 + 100);
  w.step(t0 + 100, false);
  TEST_ASSERT_EQUAL_INT(0, run(w, t0 + 150, t0 + 10050));
  TEST_ASSERT_TRUE(w.glance());
  TEST_ASSERT_TRUE(w.step(t0 + 10100, false));
  TEST_ASSERT_TRUE(w.off());
}

// The PWR key on the glance opens the whole screen, as from Off: a wake,
// with the pocket guard (none with Never) and the pocket rule. A keepLit
// pass or the panel's sleep-out don't change that.
void test_power_key_on_the_glance_opens_the_screen() {
  for (int c = 0; c < 3; ++c) {  // (keepLit, then the panel still asleep, then neither)
    ScreenPower s = offWith(2);
    s.touch(40000);
    s.step(40000, false);
    TEST_ASSERT_TRUE(s.glance());
    TEST_ASSERT_TRUE(s.powerKey(42000, c == 0, c == 1));
    TEST_ASSERT_TRUE(s.step(42000, false));
    TEST_ASSERT_TRUE(s.bright());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(L::Glance), static_cast<int>(s.previous()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PowerKey), static_cast<int>(s.why()));
    TEST_ASSERT_TRUE(s.pocketGuard());
    TEST_ASSERT_TRUE(s.unattended());
    TEST_ASSERT_TRUE(s.touchActs());
    TEST_ASSERT_EQUAL_INT(0, run(s, 42050, 51950));
    TEST_ASSERT_TRUE(s.step(52000, false));  // no input: off again
    TEST_ASSERT_TRUE(s.off());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(W::PocketGuard), static_cast<int>(s.why()));
  }
  // Input after it ends the guard: the countdown, as ever.
  ScreenPower s = offWith(2);
  s.touch(40000);
  s.step(40000, false);
  s.powerKey(42000, false);
  s.step(42000, false);
  s.activity(45000);
  TEST_ASSERT_FALSE(s.pocketGuard());
  TEST_ASSERT_EQUAL_UINT32(20000, s.msUntilNext(45000));
  // With Never: no guard.
  ScreenPower n;
  n.begin(0);
  n.setOffTouch(2);
  n.setTimeout(ScreenPower::kNever, 0);
  n.turnOff(W::Console);
  n.step(1000, false);
  n.touch(2000);
  n.step(2000, false);
  TEST_ASSERT_TRUE(n.powerKey(3000, false));
  TEST_ASSERT_FALSE(n.pocketGuard());
  TEST_ASSERT_EQUAL_INT(1, run(n, 3000, 600000));
  TEST_ASSERT_TRUE(n.bright());
}

// What else ends the glance: an event (the whole screen and its countdown,
// the first touch only answers it), what keeps the screen lit (the same),
// the console (someone is there), and what turns a screen off.
void test_what_ends_the_glance() {
  ScreenPower e = offWith(2);
  e.touch(40000);
  e.step(40000, false);
  TEST_ASSERT_TRUE(e.wake(41000, W::Event));
  TEST_ASSERT_TRUE(e.step(41000, false));
  TEST_ASSERT_TRUE(e.bright());
  TEST_ASSERT_FALSE(e.pocketGuard());
  TEST_ASSERT_FALSE(e.touchActs());
  TEST_ASSERT_TRUE(e.unattended());
  TEST_ASSERT_EQUAL_UINT32(20000, e.msUntilNext(41000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Answered), static_cast<int>(e.touch(42000)));
  TEST_ASSERT_TRUE(e.touchActs());
  // keepLit: as an event's wake.
  ScreenPower k = offWith(2);
  k.touch(40000);
  k.step(40000, false);
  TEST_ASSERT_TRUE(k.step(41000, true));
  TEST_ASSERT_TRUE(k.bright());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(W::KeepLit), static_cast<int>(k.why()));
  TEST_ASSERT_FALSE(k.touchActs());
  TEST_ASSERT_TRUE(k.unattended());
  // The console (Ps1, the scripted finger): lit and attended.
  ScreenPower c = offWith(2);
  c.touch(40000);
  c.step(40000, false);
  TEST_ASSERT_TRUE(c.wake(41000, W::Console));
  c.step(41000, false);
  TEST_ASSERT_TRUE(c.bright());
  TEST_ASSERT_FALSE(c.unattended());
  TEST_ASSERT_TRUE(c.touchActs());
  // The choice changed during it (the console's Pst): the next touch
  // follows the new one.
  ScreenPower n = offWith(2);
  n.touch(40000);
  n.step(40000, false);
  n.setOffTouch(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Ignored), static_cast<int>(n.touch(45000)));
  TEST_ASSERT_TRUE(n.step(50000, false));  // (its 10 s from the first touch)
  TEST_ASSERT_TRUE(n.off());
  ScreenPower w = offWith(2);
  w.touch(40000);
  w.step(40000, false);
  w.setOffTouch(1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(T::Woke), static_cast<int>(w.touch(41000)));
  TEST_ASSERT_TRUE(w.step(41000, false));
  TEST_ASSERT_TRUE(w.bright());
  TEST_ASSERT_TRUE(w.pocketGuard());
  TEST_ASSERT_TRUE(w.unattended());
  // The sleep timer's pause, the console's Ps0: off now.
  ScreenPower t = offWith(2);
  t.touch(40000);
  t.step(40000, false);
  t.turnOff(W::SleepTimer);
  TEST_ASSERT_TRUE(t.step(41000, false));
  TEST_ASSERT_TRUE(t.off());
  TEST_ASSERT_FALSE(t.unattended());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_tables_and_defaults);
  RUN_TEST(test_default_dims_at_20_s_and_goes_off_at_30_s);
  RUN_TEST(test_each_choice_times);
  RUN_TEST(test_activity_restarts_the_countdown);
  RUN_TEST(test_wake_from_dim);
  RUN_TEST(test_pocket_guard);
  RUN_TEST(test_event_wake_gets_the_whole_countdown);
  RUN_TEST(test_keep_lit);
  RUN_TEST(test_turn_off_settings_and_override);
  RUN_TEST(test_wraparound);
  RUN_TEST(test_names);
  RUN_TEST(test_unattended_and_the_event_answer);
  RUN_TEST(test_wake_latch_timing);
  RUN_TEST(test_finger_activity);
  RUN_TEST(test_the_touch_that_attends_is_remembered);
  RUN_TEST(test_power_key_turns_a_lit_screen_off);
  RUN_TEST(test_power_key_on_an_event_wake);
  RUN_TEST(test_power_key_on_a_screen_kept_lit);
  RUN_TEST(test_power_key_while_the_panel_still_sleeps);
  RUN_TEST(test_power_key_during_a_hold);
  RUN_TEST(test_hold_lit_keeps_a_lit_screen_lit);
  RUN_TEST(test_hold_lit_leaves_an_off_screen_off);
  RUN_TEST(test_hold_lit_and_a_wake_during_it);
  RUN_TEST(test_hold_lit_through_the_pair_search);
  RUN_TEST(test_off_touch_choices_and_labels);
  RUN_TEST(test_a_touch_on_an_off_screen_does_nothing_by_default);
  RUN_TEST(test_the_wake_choice_is_the_old_touch);
  RUN_TEST(test_the_glance);
  RUN_TEST(test_power_key_on_the_glance_opens_the_screen);
  RUN_TEST(test_what_ends_the_glance);
  return UNITY_END();
}
