// Host tests for the UI spike's portable pieces: touch gesture
// classification, kinetic scrolling, the scroll governor and percentiles;
// and the screen's wake latch (WakeLatch) with the real recognisers.
// Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <vector>

#include "ButtonGesture.h"
#include "ButtonPolicy.h"
#include "InputEvent.h"
#include "KineticScroll.h"
#include "Percentiles.h"
#include "ScreenPower.h"
#include "ScrollGovernor.h"
#include "StripButtons.h"
#include "TouchGesture.h"
#include "TouchRecognizer.h"

void setUp() {}
void tearDown() {}

// ---- TouchGesture ----

void test_gesture_tap_and_hold() {
  TouchGesture g;
  g.down(1000, 100, 100);
  g.move(1040, 103, 98);
  TouchGesture::Result r = g.up(1120);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Tap), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL_UINT32(120, r.durationMs);
  TEST_ASSERT_EQUAL_INT(3, r.maxMovePx);
  TEST_ASSERT_EQUAL_INT(100, r.downX);
  TEST_ASSERT_EQUAL_INT(103, r.upX);

  g.down(2000, 50, 50);
  TEST_ASSERT_FALSE(g.holding(2400));
  g.move(2300, 58, 52);
  TEST_ASSERT_TRUE(g.holding(2500));
  r = g.up(2700);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Hold), static_cast<int>(r.kind));
  TEST_ASSERT_FALSE(g.active());
}

void test_gesture_drag_and_flick() {
  TouchGesture g;
  // Slow drag: 60 px over 600 ms, 10 px per 100 ms at the end.
  g.down(0, 100, 200);
  for (int i = 1; i <= 6; ++i) g.move(i * 100, 100, 200 - 10 * i);
  TouchGesture::Result r = g.up(610);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Drag), static_cast<int>(r.kind));
  TEST_ASSERT_EQUAL_INT(60, r.maxMovePx);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -100.0f, r.vy);

  // Flick: 20 px per 16 ms at the end, about 1250 px/s upward.
  g.down(0, 100, 200);
  for (int i = 1; i <= 6; ++i) g.move(i * 16, 100, 200 - 20 * i);
  r = g.up(100);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TouchGesture::Kind::Flick), static_cast<int>(r.kind));
  TEST_ASSERT_FLOAT_WITHIN(20.0f, -1250.0f, r.vy);
  TEST_ASSERT_FLOAT_WITHIN(20.0f, 1250.0f, r.speed);
  TEST_ASSERT_EQUAL_STRING("flick", TouchGesture::name(r.kind));
}

// ---- KineticScroll ----

void test_scroll_drag_is_one_to_one_and_clamped() {
  KineticScroll s;
  s.setExtent(20 * 42, 168);  // 20 rows, 4 visible
  TEST_ASSERT_EQUAL_FLOAT(20 * 42 - 168, s.maxOffset());
  s.press(0, 200);
  s.drag(10, 150);
  TEST_ASSERT_EQUAL_FLOAT(50, s.offset());
  s.drag(20, 260);  // past the top: clamped
  TEST_ASSERT_EQUAL_FLOAT(0, s.offset());
  s.drag(30, 180);
  TEST_ASSERT_EQUAL_FLOAT(20, s.offset());
}

void test_scroll_slow_release_snaps_to_nearest_row() {
  KineticScroll s;
  s.setExtent(20 * 42, 168);
  s.press(0, 200);
  s.drag(100, 170);   // offset 30
  s.drag(400, 170);   // held still
  s.release(500);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(KineticScroll::Phase::Snapping), static_cast<int>(s.phase()));
  uint32_t t = 500;
  while (s.moving() && t < 2000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(42, s.offset());
  TEST_ASSERT_FALSE(s.moving());
}

void test_scroll_fling_decays_and_lands_on_a_row() {
  KineticScroll s;
  s.setExtent(200 * 42, 168);
  s.fling(0, 2000);
  uint32_t t = 0;
  float last = 0;
  while (s.moving() && t < 10000) {
    s.update(t += 33);
    TEST_ASSERT_TRUE(s.offset() >= last - 0.001f);  // never goes back
    last = s.offset();
  }
  TEST_ASSERT_FALSE(s.moving());
  TEST_ASSERT_EQUAL_FLOAT(0, std::fmod(s.offset(), 42.0f));
  // The distance of a 0.92-per-66.7ms decay from 2000 px/s down to the stop
  // speed is ~ (2000-60) / k, k = -ln(0.92)/66.7 ms: about 1550 px, plus the snap.
  TEST_ASSERT_TRUE(s.offset() > 1400 && s.offset() < 1700);
  // The same fling at a different frame rate ends at the same row.
  KineticScroll s2;
  s2.setExtent(200 * 42, 168);
  s2.fling(0, 2000);
  t = 0;
  while (s2.moving() && t < 10000) s2.update(t += 10);
  TEST_ASSERT_FLOAT_WITHIN(42.0f, s.offset(), s2.offset());
}

void test_scroll_release_velocity_and_edges() {
  KineticScroll s;
  s.setExtent(30 * 42, 168);
  // Finger moves up 30 px per 16 ms: the list flings up (offset grows).
  s.press(0, 220);
  for (int i = 1; i <= 5; ++i) s.drag(i * 16, 220 - 30 * i);
  s.release(84);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(KineticScroll::Phase::Flinging), static_cast<int>(s.phase()));
  TEST_ASSERT_TRUE(s.velocity() > 1500);
  uint32_t t = 84;
  while (s.moving() && t < 10000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(s.maxOffset(), s.offset());  // hit the end and stopped there
  // A fling down from the end stops at 0.
  s.fling(t, -5000);
  while (s.moving() && t < 20000) s.update(t += 16);
  TEST_ASSERT_EQUAL_FLOAT(0, s.offset());
  // A finger that stopped before lifting doesn't fling.
  s.press(t, 200);
  s.drag(t + 16, 150);
  s.release(t + 300);
  TEST_ASSERT_NOT_EQUAL(static_cast<int>(KineticScroll::Phase::Flinging), static_cast<int>(s.phase()));
  // jumpTo stops everything.
  s.fling(t, 3000);
  s.jumpTo(10 * 42);
  TEST_ASSERT_FALSE(s.moving());
  TEST_ASSERT_EQUAL_FLOAT(420, s.offset());
}

// ---- ScrollGovernor ----

void test_governor_levels_follow_the_ring() {
  ScrollGovernor g;
  ScrollGovernor::Budget b = g.update(0, 1450, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  TEST_ASSERT_EQUAL_UINT32(66, b.frameMs);
  b = g.update(100, 800, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  TEST_ASSERT_EQUAL_UINT32(125, b.frameMs);
  b = g.update(200, 400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::WholeRows), static_cast<int>(b.level));
  TEST_ASSERT_TRUE(b.wholeRows);
  b = g.update(300, 100, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  TEST_ASSERT_FALSE(b.draw);
  // Recovery: one level per holdMs, and only above threshold + margin.
  b = g.update(400, 300, 0, true);  // above 250 but not 250 + 200: stays paused
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  b = g.update(2300, 1400, 0, true);  // clock started at 400
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Paused), static_cast<int>(b.level));
  b = g.update(2500, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::WholeRows), static_cast<int>(b.level));
  b = g.update(4600, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(6700, 1400, 0, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
}

void test_governor_underrun_and_idle() {
  ScrollGovernor g;
  g.update(0, 1450, 5, true);
  ScrollGovernor::Budget b = g.update(100, 1450, 6, true);  // an underrun with a full ring
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(1500, 1450, 6, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Reduced), static_cast<int>(b.level));
  b = g.update(2200, 1450, 6, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  // Nothing playing: an empty ring is no risk.
  b = g.update(3000, 0, 6, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
  // Disabled: always normal.
  ScrollGovernor::Config c;
  c.enabled = false;
  g.setConfig(c);
  b = g.update(4000, 50, 9, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScrollGovernor::Level::Normal), static_cast<int>(b.level));
}

// ---- Percentiles ----

void test_percentiles() {
  float v[] = {5, 1, 4, 2, 3, 10, 9, 8, 7, 6};
  const Percentiles p = Percentiles::of(v, 10);
  TEST_ASSERT_EQUAL_UINT32(10, p.n);
  TEST_ASSERT_EQUAL_FLOAT(1, p.min);
  TEST_ASSERT_EQUAL_FLOAT(10, p.max);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, p.p50);
  TEST_ASSERT_EQUAL_FLOAT(5.5f, p.mean);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.9f, p.p10);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 9.1f, p.p90);
  const Percentiles none = Percentiles::of(v, 0);
  TEST_ASSERT_EQUAL_UINT32(0, none.n);
}

// ---- WakeLatch: the touch that wakes the screen does nothing else ----

namespace {

// The input layer's order each pass (ui/Input::update): the latch (a
// finger it just took has its glass touch cancelled), then the strip's
// buttons (StripButtons, ButtonGesture with ButtonPolicy's timings; a
// swipe up from the strip handed to the glass), then the glass
// (TouchRecognizer). While the latch holds, every event is dropped, as
// Input drops them while suspended; the recognisers still follow the
// finger. With a ScreenPower, app/ScreenControl's order after it.
struct Pipeline {
  TouchRecognizer glass;
  StripButtons strip;
  ButtonGesture buttons[3];
  WakeLatch latch;
  FingerActivity still;
  bool bright = false;  // a touch acts (ScreenPower::touchActs())
  bool useLatch = true;
  int wakes = 0;
  int taken = 0;
  std::vector<InputEvent> events;
  ScreenPower* screen = nullptr;

  Pipeline() {
    for (int b = 0; b < 3; ++b) buttons[b].setConfig(ButtonPolicy::gestureFor(b));
    StripButtons::Config sc = strip.config();
    sc.holdMs = ButtonPolicy::kHoldMs;
    strip.setConfig(sc);
  }

  // One loop pass. The first point: `pressed` at (x, y) (screen pixels;
  // y >= 240 the strip); `fingers`: whether any real finger is on the
  // panel (false for the scripted finger's point); `newTouch`: the first
  // point is another finger than last pass's.
  void pass(uint32_t ms, bool pressed, int x, int y, bool fingers, bool newTouch = false) {
    const WakeLatch::Result w = useLatch ? latch.update(ms, fingers, bright) : WakeLatch::Result{};
    if (w.woke) ++wakes;
    if (w.took) {
      ++taken;
      const InputEvent c = glass.cancel(ms);
      if (c.type != InputEvent::Type::None) events.push_back(c);
    }
    TouchRecognizer::Sample s;
    s.pressed = pressed;
    s.x = s.rawX = static_cast<int16_t>(x);
    s.y = s.rawY = static_cast<int16_t>(y);
    const StripButtons::Result r = strip.update(ms, pressed, x, y, newTouch);
    if (r.scroll) glass.fromStrip();
    for (int b = 0; b < 3; ++b) {
      const ButtonGesture::Event g =
          r.cancelled && r.button == b ? buttons[b].cancel() : buttons[b].update(ms, r.pressed == b);
      InputEvent e;
      switch (g) {
        case ButtonGesture::Event::Click: e.type = InputEvent::Type::Click; break;
        case ButtonGesture::Event::Hold: e.type = InputEvent::Type::Hold; break;
        case ButtonGesture::Event::Repeat: e.type = InputEvent::Type::Repeat; break;
        case ButtonGesture::Event::HoldEnd: e.type = InputEvent::Type::HoldEnd; break;
        default: continue;
      }
      e.button = static_cast<uint8_t>(b);
      if (!w.hold) events.push_back(e);
    }
    InputEvent out[TouchRecognizer::kMaxEvents];
    const int n = glass.update(ms, s, out);
    bool landed = false;
    for (int i = 0; i < n; ++i) {
      if (w.hold) continue;
      if (out[i].type == InputEvent::Type::Down && !out[i].fromStrip) landed = true;
      events.push_back(out[i]);
    }
    const bool acting = still.update(ms, pressed && !w.hold, x, y);
    // With the screen policy: the wake, or a finger that acts as input
    // (landing or moving); a touch landing on the glass attends; then its
    // countdown.
    if (screen) {
      if (w.woke) {
        screen->wake(ms, ScreenPower::Why::Touch);
      } else if (acting) {
        screen->activity(ms);
      }
      if (landed) screen->attend();
      screen->step(ms, false);
      bright = screen->touchActs();
    }
  }

  // A real finger resting at (x, y) from t for `ms`, then lifted (and the
  // passes after the lift until t + ms + after). Returns the time reached.
  uint32_t press(uint32_t t, int x, int y, uint32_t ms, uint32_t after = 60) {
    for (uint32_t e = 0; e < ms; e += 10) pass(t + e, true, x, y, true);
    for (uint32_t e = 0; e <= after; e += 10) pass(t + ms + e, false, 0, 0, false);
    return t + ms + after + 10;
  }
  // A real finger from (x0, y0) to (x1, y1) in `ms`, lifted at once.
  uint32_t swipe(uint32_t t, int x0, int y0, int x1, int y1, uint32_t ms) {
    pass(t, true, x0, y0, true);
    for (uint32_t e = 10; e <= ms; e += 10) {
      const int x = x0 + (x1 - x0) * static_cast<int>(e) / static_cast<int>(ms);
      const int y = y0 + (y1 - y0) * static_cast<int>(e) / static_cast<int>(ms);
      pass(t + e, true, x, y, true);
    }
    for (uint32_t e = 10; e <= 60; e += 10) pass(t + ms + e, false, 0, 0, false);
    return t + ms + 70;
  }

  int count(InputEvent::Type t) const {
    int n = 0;
    for (const InputEvent& e : events) n += e.type == t ? 1 : 0;
    return n;
  }
};

// main.cpp's ButtonTransport, as far as the pocket rule goes: the speaker
// or the headphones, playing or paused.
struct PocketTransport : ButtonPolicy::Transport {
  const ScreenPower* screen = nullptr;
  bool bluetooth = false;
  bool isPlaying = false;
  int starts = 0;
  void prev() override {}
  void next() override {}
  void playPause() override {
    if (!isPlaying) ++starts;
    isPlaying = !isPlaying;
  }
  bool playing() const override { return isPlaying; }
  void pause() override { isPlaying = false; }
  void stepVolume(int) override {}
  int volume() const override { return 50; }
  bool onBluetooth() const override { return bluetooth; }
  bool switchOutput() override { return true; }
  bool startRefused() const override { return !bluetooth && screen->unattended(); }
};

// Hands the button events from `from` on to ButtonPolicy.
void handleButtons(const Pipeline& p, size_t from, PocketTransport& t) {
  ButtonPolicy policy;
  for (size_t i = from; i < p.events.size(); ++i) {
    if (p.events[i].isButton()) policy.handle(p.events[i], t);
  }
}

constexpr int kBx = 160, kCx = 270, kStripY = 262;  // the user's presses: y 247-278

}  // namespace

// Control: the harness makes the events a lit screen would act on.
void test_latch_lit_screen_acts() {
  Pipeline p;
  p.bright = true;
  uint32_t t = p.press(0, kBx, kStripY, 80);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
  t = p.press(t + 500, kBx, kStripY, 700);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Hold));
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::HoldEnd));
  t = p.swipe(t + 500, kBx, 265, kBx, 100, 120);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::DragStart));
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Fling));
  p.press(t + 500, 100, 120, 60);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Tap));
  TEST_ASSERT_EQUAL_INT(0, p.wakes);
}

// In the dark (or dim): a B click, a B hold, a C hold (volume repeats) and
// a swipe up from the strip each only wake; nothing reaches ButtonPolicy
// or a page. Each is its own wake (the screen is kept dark here, as if the
// pocket guard had turned it off again; each lands 500 ms after the last
// lifted, past the latch's quiet window).
void test_latch_swallows_strip_presses_in_the_dark() {
  Pipeline p;
  uint32_t t = p.press(0, kBx, kStripY, 80);  // B click
  t = p.press(t + 500, kBx, kStripY, 800);    // B hold (output switch)
  t = p.press(t + 500, kCx, kStripY, 1500);   // C hold (volume up, repeating)
  p.swipe(t + 500, kBx, 265, kBx, 100, 120);  // a swipe up from the strip
  TEST_ASSERT_EQUAL_INT(4, p.wakes);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
}

// The lift is swallowed too: a click is made on the lift pass, a tap and a
// fling too. The latch holds until no finger has been on for kQuietMs.
void test_latch_swallows_the_lift() {
  Pipeline p;
  for (uint32_t t = 0; t < 80; t += 10) p.pass(t, true, kBx, kStripY, true);
  TEST_ASSERT_TRUE(p.latch.holding());
  p.pass(80, false, 0, 0, false);  // the lift: ButtonGesture's Click is made here
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
  TEST_ASSERT_TRUE(p.latch.holding());
  p.pass(90, false, 0, 0, false);
  p.pass(70 + WakeLatch::kQuietMs - 5, false, 0, 0, false);
  TEST_ASSERT_TRUE(p.latch.holding());
  p.pass(70 + WakeLatch::kQuietMs, false, 0, 0, false);  // 400 ms since the finger was last seen
  TEST_ASSERT_FALSE(p.latch.holding());
  // Then the screen is bright (woken), and the next press acts.
  p.bright = true;
  p.press(600, kBx, kStripY, 80);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  // Without the latch, the very same lift would have clicked.
  Pipeline q;
  q.useLatch = false;
  for (uint32_t t = 0; t < 80; t += 10) q.pass(t, true, kBx, kStripY, true);
  q.pass(80, false, 0, 0, false);
  TEST_ASSERT_EQUAL_INT(1, q.count(InputEvent::Type::Click));
  // A glass tap in the dark: no Down, no Tap (a tap is made on its lift).
  Pipeline g;
  g.press(0, 100, 120, 60);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(g.events.size()));
  TEST_ASSERT_EQUAL_INT(1, g.wakes);
}

// A second finger: the waking finger on the glass, then another presses B;
// the first lifts, so the first point becomes the second finger (on B),
// which then lifts. Nothing acts while any finger is on; one wake.
void test_latch_second_finger() {
  Pipeline p;
  uint32_t t = 0;
  for (; t < 100; t += 10) p.pass(t, true, 100, 120, true);  // finger 1 wakes it
  for (; t < 200; t += 10) p.pass(t, true, 100, 120, true);  // finger 2 down on B too (not the first point)
  p.pass(t, true, kBx, kStripY, true, /*newTouch=*/true);    // finger 1 lifted: finger 2 is the first point
  for (t += 10; t < 400; t += 10) p.pass(t, true, kBx, kStripY, true);
  for (; t < 800; t += 10) p.pass(t, false, 0, 0, false);
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
  TEST_ASSERT_FALSE(p.latch.holding());
}

// The panel loses a finger and finds it again (StripButtons: within ~145
// ms, a swipe's later). Once the wake has lit the screen the loop runs
// every 1-5 ms: the waking B press, lost for 10 to 390 ms in the middle,
// is still the waking one when it comes back, whether it then lifts (it
// would click) or stays on (it would hold: the output switch). The old
// two-pass window let go after ~10 ms, and the finger found again clicked.
void test_latch_rides_out_a_dropout() {
  const uint32_t drops[] = {10, 20, 50, 100, 140, 300, WakeLatch::kQuietMs - 10};
  const uint32_t stays[] = {100, 800};
  for (const uint32_t drop : drops) {
    for (const uint32_t stay : stays) {
      Pipeline p;
      uint32_t t = 0;
      for (; t < 50; t += 5) {
        p.pass(t, true, kBx, kStripY, true);
        p.bright = true;  // woken on the first pass: the loop runs lit from here
      }
      const uint32_t back = t + drop;
      for (; t < back; t += 5) p.pass(t, false, 0, 0, false);  // the panel lost it
      const uint32_t lift = t + stay;
      for (; t < lift; t += 5) p.pass(t, true, kBx, kStripY, true);
      for (const uint32_t end = t + 600; t < end; t += 5) p.pass(t, false, 0, 0, false);
      TEST_ASSERT_EQUAL_INT(1, p.wakes);
      TEST_ASSERT_EQUAL_INT(0, p.count(InputEvent::Type::Click));
      TEST_ASSERT_EQUAL_INT(0, p.count(InputEvent::Type::Hold));
      TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
      TEST_ASSERT_FALSE(p.latch.holding());
    }
  }
  // The same on the glass: no Down, no Tap from the finger found again.
  Pipeline g;
  uint32_t t = 0;
  for (; t < 60; t += 5) {
    g.pass(t, true, 100, 120, true);
    g.bright = true;
  }
  for (; t < 180; t += 5) g.pass(t, false, 0, 0, false);
  for (; t < 260; t += 5) g.pass(t, true, 100, 120, true);
  for (; t < 900; t += 5) g.pass(t, false, 0, 0, false);
  TEST_ASSERT_EQUAL_INT(1, g.wakes);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(g.events.size()));
}

// The scripted finger (uit/uip) is no finger for the latch: it acts in the
// dark (the console's tests run with the screen off too).
void test_latch_scripted_finger_bypasses() {
  Pipeline p;
  for (uint32_t t = 0; t < 80; t += 10) p.pass(t, true, kBx, 260, /*fingers=*/false);
  p.pass(80, false, 0, 0, false);
  TEST_ASSERT_EQUAL_INT(0, p.wakes);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
}

// With ScreenPower deciding "lit": off after 30 s; a B click in the dark
// only wakes it (the pocket guard); the next click acts and ends the
// guard (it reaches ButtonPolicy; the pocket rule is below); a C hold on
// the dim screen only brightens it (no volume steps).
void test_latch_with_the_screen_policy() {
  ScreenPower sp;
  sp.begin(0);
  Pipeline p;
  p.screen = &sp;
  p.bright = true;
  for (uint32_t t = 0; t <= 31000; t += 50) p.pass(t, false, 0, 0, false);
  TEST_ASSERT_TRUE(sp.off());
  uint32_t t = p.press(40000, kBx, kStripY, 80);  // in a pocket, say
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
  TEST_ASSERT_TRUE(sp.bright());
  TEST_ASSERT_TRUE(sp.pocketGuard());
  TEST_ASSERT_TRUE(sp.unattended());
  t = p.press(t + 500, kBx, kStripY, 80);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
  TEST_ASSERT_FALSE(sp.pocketGuard());
  TEST_ASSERT_TRUE(sp.unattended());  // (the strip isn't a look at the screen)
  // 20 s later it dims; a C hold there is a wake, nothing else.
  for (; t <= 62000; t += 50) p.pass(t, false, 0, 0, false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPower::Level::Dim), static_cast<int>(sp.level()));
  p.press(t, kCx, kStripY, 1500);
  TEST_ASSERT_EQUAL_INT(2, p.wakes);
  TEST_ASSERT_EQUAL_INT(0, p.count(InputEvent::Type::Hold));
  TEST_ASSERT_EQUAL_INT(0, p.count(InputEvent::Type::Repeat));
  TEST_ASSERT_TRUE(sp.bright());
  TEST_ASSERT_FALSE(sp.pocketGuard());  // (from Dim: no guard)
}

// The pocket rule: the speaker the output, paused, the screen off. A
// pocket's contact on B wakes it (swallowed); the next contacts on B reach
// ButtonPolicy but don't start the speaker. The headphones play (not out
// loud). A tap on the glass (someone looking) ends it: then B plays.
void test_pocket_rule_second_b_click() {
  ScreenPower sp;
  sp.begin(0);
  sp.turnOff(ScreenPower::Why::Console);
  sp.step(0, false);
  Pipeline p;
  p.screen = &sp;
  PocketTransport tr;
  tr.screen = &sp;
  uint32_t t = p.press(1000, kBx, kStripY, 80);
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  TEST_ASSERT_TRUE(sp.unattended());
  size_t from = p.events.size();
  t = p.press(t + WakeLatch::kQuietMs, kBx, kStripY, 80);  // the next contact, past the latch
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
  handleButtons(p, from, tr);
  TEST_ASSERT_EQUAL_INT(0, tr.starts);
  TEST_ASSERT_FALSE(tr.isPlaying);
  // And again: still refused (any number of pocket contacts on the strip).
  from = p.events.size();
  t = p.press(t + 500, kBx, kStripY, 80);
  handleButtons(p, from, tr);
  TEST_ASSERT_EQUAL_INT(2, p.count(InputEvent::Type::Click));
  TEST_ASSERT_EQUAL_INT(0, tr.starts);
  // On the headphones: B plays (not out loud).
  tr.bluetooth = true;
  from = p.events.size();
  t = p.press(t + 500, kBx, kStripY, 80);
  handleButtons(p, from, tr);
  TEST_ASSERT_EQUAL_INT(1, tr.starts);
  TEST_ASSERT_TRUE(tr.isPlaying);
  tr.bluetooth = false;  // (moved to the speaker: paused first)
  tr.isPlaying = false;
  // A tap on the glass: someone is looking. B plays on the speaker.
  t = p.press(t + 500, 160, 150, 60);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Tap));
  TEST_ASSERT_FALSE(sp.unattended());
  from = p.events.size();
  p.press(t + 500, kBx, kStripY, 80);
  handleButtons(p, from, tr);
  TEST_ASSERT_EQUAL_INT(2, tr.starts);
  TEST_ASSERT_TRUE(tr.isPlaying);
}

// An event (the headphones lost) lights the screen from off: the dialog
// shows, but the first contact only answers it (swallowed, as a wake from
// off would be), so a pocket's contact can't tap "Use speaker"; the next
// one acts. The pocket rule holds until the glass is touched.
void test_event_wake_first_touch_only_answers() {
  ScreenPower sp;
  sp.begin(0);
  Pipeline p;
  p.screen = &sp;
  p.bright = true;
  for (uint32_t t = 0; t <= 31000; t += 50) p.pass(t, false, 0, 0, false);
  TEST_ASSERT_TRUE(sp.off());
  sp.wake(40000, ScreenPower::Why::Event);
  TEST_ASSERT_TRUE(sp.bright());
  TEST_ASSERT_FALSE(sp.touchActs());
  TEST_ASSERT_TRUE(sp.unattended());
  p.bright = sp.touchActs();
  uint32_t t = p.press(41000, 100, 150, 60);  // where "Use speaker" is, say
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(p.events.size()));
  TEST_ASSERT_TRUE(sp.touchActs());
  TEST_ASSERT_FALSE(sp.pocketGuard());  // (the event's countdown stands)
  TEST_ASSERT_TRUE(sp.unattended());
  // B from here doesn't start the speaker yet; a tap on the glass acts.
  PocketTransport tr;
  tr.screen = &sp;
  size_t from = p.events.size();
  t = p.press(t + 500, kBx, kStripY, 80);
  handleButtons(p, from, tr);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Click));
  TEST_ASSERT_EQUAL_INT(0, tr.starts);
  p.press(t + 500, 100, 150, 60);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Tap));
  TEST_ASSERT_FALSE(sp.unattended());
}

// A finger resting still on the glass (a pocket's pressure) stops counting
// as input after FingerActivity::kStillCapMs: the screen dims and goes off
// under it. The latch takes that finger as it dims (its touch cancelled,
// nothing more from it), without a wake; lifting and landing again is one.
void test_resting_finger_lets_the_screen_go_off() {
  ScreenPower sp;
  sp.begin(0);
  Pipeline p;
  p.screen = &sp;
  p.bright = true;
  uint32_t t = 1000;
  size_t atTake = 0;
  for (; t <= 120000; t += 50) {
    p.pass(t, true, 100, 120, true);
    if (p.taken == 1 && atTake == 0) atTake = p.events.size();
  }
  TEST_ASSERT_TRUE(sp.off());
  TEST_ASSERT_EQUAL_INT(1, p.taken);
  TEST_ASSERT_EQUAL_INT(0, p.wakes);
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Down));
  TEST_ASSERT_EQUAL_INT(1, p.count(InputEvent::Type::Cancel));
  TEST_ASSERT_TRUE(atTake > 0);
  TEST_ASSERT_EQUAL(InputEvent::Type::Cancel, p.events[atTake - 1].type);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(atTake), static_cast<int>(p.events.size()));  // nothing after it
  for (; t <= 121000; t += 50) p.pass(t, false, 0, 0, false);
  p.press(t + 1000, 100, 120, 60);
  TEST_ASSERT_EQUAL_INT(1, p.wakes);
  TEST_ASSERT_TRUE(sp.bright());
  TEST_ASSERT_TRUE(sp.pocketGuard());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(atTake), static_cast<int>(p.events.size()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_gesture_tap_and_hold);
  RUN_TEST(test_gesture_drag_and_flick);
  RUN_TEST(test_scroll_drag_is_one_to_one_and_clamped);
  RUN_TEST(test_scroll_slow_release_snaps_to_nearest_row);
  RUN_TEST(test_scroll_fling_decays_and_lands_on_a_row);
  RUN_TEST(test_scroll_release_velocity_and_edges);
  RUN_TEST(test_governor_levels_follow_the_ring);
  RUN_TEST(test_governor_underrun_and_idle);
  RUN_TEST(test_percentiles);
  RUN_TEST(test_latch_lit_screen_acts);
  RUN_TEST(test_latch_swallows_strip_presses_in_the_dark);
  RUN_TEST(test_latch_swallows_the_lift);
  RUN_TEST(test_latch_second_finger);
  RUN_TEST(test_latch_rides_out_a_dropout);
  RUN_TEST(test_latch_scripted_finger_bypasses);
  RUN_TEST(test_latch_with_the_screen_policy);
  RUN_TEST(test_pocket_rule_second_b_click);
  RUN_TEST(test_event_wake_first_touch_only_answers);
  RUN_TEST(test_resting_finger_lets_the_screen_go_off);
  return UNITY_END();
}
