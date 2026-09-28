// Host tests for the screen policy (ScreenPower, docs/ENERGY.md item 2): the
// dim and off timings for each choice, what keeps it lit, the wakes, the
// pocket guard, and the backlight levels. The wake latch's own cases (with
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
int run(ScreenPower& s, uint32_t from, uint32_t to, bool keepLit = false) {
  int changes = 0;
  for (uint32_t t = from; t - from <= to - from; t += 50) {
    if (s.step(t, keepLit)) ++changes;
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
  TEST_ASSERT_TRUE(std::strlen(ScreenPower::name(W::PocketGuard)) > 0);
}

// The touch that attends a screen woken from off is remembered for that
// touch (landedUnattended()): a pocket's second contact lands on a lit
// fade toast, and must not act on +10 min or Turn off. The next touch was
// made on an attended screen. PWR while lit attends too, but no touch
// landed then.
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
  // Attended by the key while lit: the next touch lands attended.
  s.step(30000, false);
  s.wake(40000, W::PowerKey);
  s.step(40000, false);
  s.attend();
  s.glassLanded();
  TEST_ASSERT_FALSE(s.landedUnattended());
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
  return UNITY_END();
}
