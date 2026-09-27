// Host tests for the input layer's portable pieces: the touch correction
// (TouchCalibration, with the user's own target-practice logs), the touch
// buttons' click/hold/repeat (ButtonGesture) and what they do
// (ButtonPolicy), the glass's events (TouchRecognizer), and the fling cap.
// Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ButtonGesture.h"
#include "ButtonPolicy.h"
#include "InputEvent.h"
#include "KineticScroll.h"
#include "TouchCalibration.h"
#include "TouchRecognizer.h"

void setUp() {}
void tearDown() {}

namespace {

using Sample = TouchCalibration::Sample;

// The input lab's target practice on the device (u1; targets_thumb.log and
// targets_index.log), x only: {what the panel read, the target's centre}.
// 72 taps on 10 targets with the thumb and the index finger; the "last list
// row" taps are left out (that target is the whole width, so its centre
// says nothing about where the finger was aimed), and so is one drag.
const Sample kLogX[] = {
    {319, 299}, {80, 83},   {62, 70},   {304, 269}, {196, 183}, {84, 70},   {317, 273}, {206, 183}, {296, 269},
    {310, 273}, {50, 27},   {298, 251}, {130, 139}, {196, 183}, {71, 70},   {195, 183}, {319, 299}, {0, 27},
    {129, 139}, {319, 273}, {308, 269}, {306, 269}, {319, 273}, {79, 83},   {284, 251}, {75, 70},   {228, 195},
    {0, 27},    {319, 273}, {76, 70},   {307, 269}, {142, 139}, {319, 299}, {202, 183}, {287, 251}, {220, 195},
    {302, 269}, {75, 70},   {317, 273}, {204, 183}, {77, 83},   {57, 70},   {146, 139}, {311, 269}, {317, 273},
    {217, 195}, {285, 299}, {74, 70},   {309, 269}, {208, 183}, {80, 83},   {302, 273}, {204, 183}, {0, 27},
    {82, 70},   {276, 251}, {309, 273}, {306, 269}, {68, 70},   {76, 83},   {73, 70},   {304, 269}, {319, 273},
    {211, 195}, {192, 183}, {285, 251}, {149, 139}, {212, 183}, {319, 299}, {0, 27},    {138, 139}, {66, 70},
};
constexpr int kLogN = sizeof(kLogX) / sizeof(kLogX[0]);

float meanCorrected(const TouchCalibration::Axis& a, int target) {
  float sum = 0;
  int n = 0;
  for (const Sample& s : kLogX) {
    if (s.target != target) continue;
    sum += a.map(s.raw);
    ++n;
  }
  return n ? sum / n : -1;
}

}  // namespace

// ---- TouchCalibration ----

void test_default_table_is_the_fit_of_the_logs() {
  TEST_ASSERT_EQUAL_INT(72, kLogN);
  TouchCalibration::Axis fitted;
  TouchCalibration::FitReport r;
  TEST_ASSERT_TRUE(TouchCalibration::fitAxis(kLogX, kLogN, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots,
                                             &fitted, &r));
  const TouchCalibration d = TouchCalibration::defaults();
  TEST_ASSERT_TRUE(d.valid());
  TEST_ASSERT_EQUAL_INT(fitted.n, d.x.n);
  for (int i = 0; i < fitted.n; ++i) {
    char msg[64];
    snprintf(msg, sizeof(msg), "knot %d (raw %d): fit %.2f", i, fitted.raw[i], fitted.value[i]);
    TEST_ASSERT_EQUAL_INT16(fitted.raw[i], d.x.raw[i]);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.06f, fitted.value[i], d.x.value[i], msg);
  }
  // It more than halves the error on the taps it was fitted to.
  TEST_ASSERT_EQUAL_INT(kLogN, r.samples);
  TEST_ASSERT_TRUE(r.rmsBefore > 25.0f);
  TEST_ASSERT_TRUE(r.rmsAfter < 10.0f);
}

void test_default_table_puts_each_target_where_it_was() {
  const TouchCalibration::Axis& x = TouchCalibration::defaults().x;
  // Every target whose taps the panel didn't clamp lands within 7 px on
  // average (the raw readings were off by up to 36 px).
  const int targets[] = {27, 70, 83, 139, 183, 195, 251, 269, 273};
  for (int t : targets) {
    char msg[48];
    snprintf(msg, sizeof(msg), "target %d: %.1f", t, meanCorrected(x, t));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(7.0f, static_cast<float>(t), meanCorrected(x, t), msg);
  }
  // The volume chip (299) read 319 four times out of five: past the panel's
  // clamp nothing tells 273 from 299, so it lands at ~282, inside a
  // right-edge control that is ~40 px wide.
  TEST_ASSERT_TRUE(meanCorrected(x, 299) > 270.0f);
  // Close to the table the user sketched from the same data (raw 140 ->
  // 140, 212 -> 190, 290 -> 251, 306 -> 270, 319 -> ~285; raw 0 -> ~15?).
  const TouchCalibration d = TouchCalibration::defaults();
  TEST_ASSERT_INT_WITHIN(8, 140, d.mapX(140));
  TEST_ASSERT_INT_WITHIN(5, 190, d.mapX(212));
  TEST_ASSERT_INT_WITHIN(12, 251, d.mapX(290));
  TEST_ASSERT_INT_WITHIN(5, 270, d.mapX(306));
  TEST_ASSERT_INT_WITHIN(5, 285, d.mapX(319));
  TEST_ASSERT_INT_WITHIN(15, 15, d.mapX(0));
  // Monotonic: never goes back, never flat.
  for (int r = 1; r <= 319; ++r) TEST_ASSERT_TRUE(d.x.map(r) > d.x.map(r - 1));
  // y is left alone, on the glass and on the button strip below it.
  for (int y : {0, 17, 120, 217, 239, 250, 279}) TEST_ASSERT_EQUAL_INT(y, d.mapY(y));
  // Clamped to the screen.
  TEST_ASSERT_TRUE(d.mapX(-20) >= 0);
  TEST_ASSERT_TRUE(d.mapX(400) <= 319);
}

// The calibration screen's case: 9 crosshairs, one tap each, on a panel with
// a known distortion (reads more and more to the right from x 140, and
// clamps at 319 from x ~259). The fit undoes it up to about 20 px before
// the panel clamps (the two crosshairs past it both read 319: the table
// puts 319 between them, and the smoothing rounds the corner off).
void test_fit_recovers_a_distortion_from_nine_taps() {
  auto panel = [](float t) {
    float r = t <= 140 ? t : 140 + (t - 140) * 1.45f;
    return static_cast<int16_t>(r > 319 ? 319 : std::lround(r));
  };
  const int targets[] = {20, 55, 90, 125, 160, 195, 230, 265, 300};
  std::vector<Sample> s;
  for (int t : targets) s.push_back({panel(static_cast<float>(t)), static_cast<int16_t>(t)});
  TouchCalibration::Axis a;
  TouchCalibration::FitReport r;
  TEST_ASSERT_TRUE(TouchCalibration::fitAxis(s.data(), static_cast<int>(s.size()), TouchCalibration::kXKnotRaw,
                                             TouchCalibration::kXKnots, &a, &r));
  TEST_ASSERT_TRUE(a.valid());
  TEST_ASSERT_TRUE(r.maxBefore > 40.0f);
  TEST_ASSERT_TRUE(r.rmsAfter < r.rmsBefore / 2.5f);
  TEST_ASSERT_FLOAT_WITHIN(6.0f, 282.5f, a.map(319));
  for (int t = 10; t <= 230; t += 5) {
    char msg[32];
    snprintf(msg, sizeof(msg), "true %d", t);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(6.0f, static_cast<float>(t), a.map(panel(static_cast<float>(t))), msg);
  }
}

// Taps that contradict each other (a mis-tap, a finger that rolled) still
// give a table that only ever goes up, with every slope in bounds.
void test_fit_stays_monotonic_on_contradictory_taps() {
  const Sample s[] = {{20, 100}, {60, 20}, {100, 250}, {140, 90}, {200, 60}, {260, 300}, {300, 10}};
  TouchCalibration::Axis a;
  TEST_ASSERT_TRUE(TouchCalibration::fitAxis(s, 7, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &a));
  TEST_ASSERT_TRUE(a.valid());
  const TouchCalibration::FitOptions o;
  for (int i = 1; i < a.n; ++i) {
    const float slope = (a.value[i] - a.value[i - 1]) / static_cast<float>(a.raw[i] - a.raw[i - 1]);
    TEST_ASSERT_TRUE(slope >= o.minSlope - 1e-3f);
    TEST_ASSERT_TRUE(slope <= o.maxSlope + 1e-3f);
  }
  // Nothing to fit, or knots that don't go up: no table, out untouched.
  TouchCalibration::Axis untouched = TouchCalibration::identity().x;
  const TouchCalibration::Axis before = untouched;
  TEST_ASSERT_FALSE(TouchCalibration::fitAxis(s, 0, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &untouched));
  const int16_t bad[] = {0, 100, 100, 319};
  TEST_ASSERT_FALSE(TouchCalibration::fitAxis(s, 7, bad, 4, &untouched));
  TEST_ASSERT_FALSE(TouchCalibration::fitAxis(s, 7, TouchCalibration::kXKnotRaw, 1, &untouched));
  TEST_ASSERT_TRUE(untouched == before);
  // No samples near some knots: they follow the rest (smooth, near the
  // identity), no wild values.
  const Sample few[] = {{100, 100}, {110, 110}};
  TEST_ASSERT_TRUE(TouchCalibration::fitAxis(few, 2, TouchCalibration::kXKnotRaw, TouchCalibration::kXKnots, &a));
  for (int i = 0; i < a.n; ++i) TEST_ASSERT_FLOAT_WITHIN(3.0f, static_cast<float>(a.raw[i]), a.value[i]);
}

void test_axis_map_outside_the_knots_keeps_slope_one() {
  TouchCalibration c = TouchCalibration::identity();
  c.y.value[4] = 229;  // raw 239 -> 229
  TEST_ASSERT_TRUE(c.y.valid());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 229.0f, c.y.map(239));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, c.y.map(260));  // the strip: the same offset
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 204.5f, c.y.map(209.5f));  // halfway 180 -> 180 and 239 -> 229
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -5.0f, c.x.map(-5));
  // Not monotonic, too few knots, or values out of range: not valid.
  TouchCalibration::Axis a = c.x;
  a.value[3] = a.value[2];
  TEST_ASSERT_FALSE(a.valid());
  a = c.x;
  a.n = 1;
  TEST_ASSERT_FALSE(a.valid());
  a = c.x;
  a.value[8] = 1000;
  TEST_ASSERT_FALSE(a.valid());
  a = c.x;
  a.value[0] = NAN;
  TEST_ASSERT_FALSE(a.valid());
}

void test_calibration_save_and_load() {
  const TouchCalibration d = TouchCalibration::defaults();
  uint8_t buf[TouchCalibration::kMaxBlob];
  const size_t n = d.save(buf, sizeof(buf));
  TEST_ASSERT_EQUAL_UINT32(8 + (9 + 5) * 6 + 4, n);
  TouchCalibration c = TouchCalibration::identity();
  TEST_ASSERT_TRUE(c != d);
  TEST_ASSERT_TRUE(c.load(buf, n));
  TEST_ASSERT_TRUE(c == d);
  TEST_ASSERT_EQUAL_INT(d.mapX(300), c.mapX(300));
  // Too small a buffer: nothing written.
  TEST_ASSERT_EQUAL_UINT32(0, d.save(buf, n - 1));
  TEST_ASSERT_TRUE(d.save(buf, sizeof(buf)) == n);

  // Anything but an intact, valid table of this version is refused and
  // changes nothing.
  const TouchCalibration id = TouchCalibration::identity();
  for (size_t i = 0; i < n; ++i) {
    std::vector<uint8_t> b(buf, buf + n);
    b[i] ^= 0x10;
    TouchCalibration t = id;
    TEST_ASSERT_FALSE(t.load(b.data(), n));
    TEST_ASSERT_TRUE(t == id);
  }
  TouchCalibration t = id;
  TEST_ASSERT_FALSE(t.load(buf, n - 1));
  TEST_ASSERT_FALSE(t.load(nullptr, n));
  std::vector<uint8_t> longer(buf, buf + n);
  longer.push_back(0);
  TEST_ASSERT_FALSE(t.load(longer.data(), longer.size()));
  TEST_ASSERT_TRUE(t == id);

  // An invalid table (not monotonic) is never saved.
  TouchCalibration bad = d;
  bad.x.value[4] = bad.x.value[3] - 1;
  TEST_ASSERT_EQUAL_UINT32(0, bad.save(buf, sizeof(buf)));
}

// ---- ButtonGesture ----

void test_button_click_and_hold() {
  ButtonGesture b(ButtonPolicy::gestureFor(ButtonPolicy::kButtonB));
  using E = ButtonGesture::Event;
  TEST_ASSERT_EQUAL_INT((int)E::None, (int)b.update(0, false));
  TEST_ASSERT_EQUAL_INT((int)E::Press, (int)b.update(10, true));
  TEST_ASSERT_EQUAL_INT((int)E::None, (int)b.update(100, true));
  // The user's longest click, 143 ms.
  TEST_ASSERT_EQUAL_INT((int)E::Click, (int)b.update(153, false));
  // A hold acts at 500 ms, while still down, once; B doesn't repeat.
  TEST_ASSERT_EQUAL_INT((int)E::Press, (int)b.update(1000, true));
  TEST_ASSERT_EQUAL_INT((int)E::None, (int)b.update(1499, true));
  TEST_ASSERT_EQUAL_INT((int)E::Hold, (int)b.update(1500, true));
  TEST_ASSERT_TRUE(b.held());
  for (uint32_t t = 1510; t < 4000; t += 10) TEST_ASSERT_EQUAL_INT((int)E::None, (int)b.update(t, true));
  TEST_ASSERT_EQUAL_INT((int)E::HoldEnd, (int)b.update(4000, false));  // never a click after a hold
  TEST_ASSERT_FALSE(b.held());
  TEST_ASSERT_EQUAL_INT((int)E::None, (int)b.update(4010, false));
  // The user's shortest hold, 509 ms, is a hold.
  b.update(5000, true);
  TEST_ASSERT_EQUAL_INT((int)E::Hold, (int)b.update(5505, true));
  TEST_ASSERT_EQUAL_INT((int)E::HoldEnd, (int)b.update(5509, false));
  TEST_ASSERT_EQUAL_STRING("hold end", ButtonGesture::name(E::HoldEnd));
}

void test_button_hold_repeats_on_schedule() {
  ButtonGesture a(ButtonPolicy::gestureFor(ButtonPolicy::kButtonA));
  TEST_ASSERT_EQUAL_UINT32(200, a.config().repeatMs);
  using E = ButtonGesture::Event;
  a.update(0, true);
  std::vector<uint32_t> repeats;
  uint32_t holdAt = 0;
  // A loop pass every 7 ms.
  for (uint32_t t = 7; t <= 1500; t += 7) {
    const E e = a.update(t, true);
    if (e == E::Hold) holdAt = t;
    if (e == E::Repeat) repeats.push_back(t);
  }
  TEST_ASSERT_EQUAL_UINT32(504, holdAt);  // the first pass at or after 500
  // Every 200 ms from the hold, without drifting with the 7 ms passes.
  TEST_ASSERT_EQUAL_INT(4, static_cast<int>(repeats.size()));
  const uint32_t expect[] = {707, 910, 1106, 1309};
  for (int i = 0; i < 4; ++i) TEST_ASSERT_UINT32_WITHIN(7, expect[i], repeats[i]);
  TEST_ASSERT_EQUAL_UINT32(4, a.repeats());
  // A stalled loop (600 ms) gets one repeat, not three at once.
  int burst = 0;
  if (a.update(2100, true) == E::Repeat) ++burst;
  if (a.update(2101, true) == E::Repeat) ++burst;
  if (a.update(2102, true) == E::Repeat) ++burst;
  TEST_ASSERT_EQUAL_INT(1, burst);
  TEST_ASSERT_EQUAL_INT((int)E::None, (int)a.update(2299, true));
  TEST_ASSERT_EQUAL_INT((int)E::Repeat, (int)a.update(2300, true));
  TEST_ASSERT_EQUAL_INT((int)E::HoldEnd, (int)a.update(2310, false));
}

// ---- ButtonPolicy ----

namespace {

struct FakeTransport : ButtonPolicy::Transport {
  std::string log;
  bool isPlaying = true;
  bool bluetooth = true;
  bool audioBt = true;  // false: asked for, the audio still on the speaker
  bool empty = false;
  bool allowSwitch = true;
  int vol = 50;
  void prev() override { log += "prev;"; }
  void next() override { log += "next;"; }
  void playPause() override {
    log += "playpause;";
    isPlaying = !isPlaying;
  }
  bool playing() const override { return isPlaying; }
  void pause() override {
    log += "pause;";
    isPlaying = false;
  }
  void stepVolume(int d) override {
    vol += d;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    log += "vol" + std::to_string(d) + ";";
  }
  int volume() const override { return vol; }
  bool onBluetooth() const override { return bluetooth; }
  bool audioOnBluetooth() const override { return bluetooth && audioBt; }
  bool idle() const override { return empty; }
  bool switchOutput() override {
    if (!allowSwitch) {
      log += "refused;";
      return false;
    }
    bluetooth = !bluetooth;
    log += bluetooth ? "to-bt;" : "to-speaker;";
    return true;
  }
};

InputEvent button(InputEvent::Type t, int b, uint32_t ms = 0) {
  InputEvent e;
  e.type = t;
  e.button = static_cast<uint8_t>(b);
  e.ms = ms;
  return e;
}

}  // namespace

void test_policy_clicks_are_transport() {
  ButtonPolicy p;
  FakeTransport t;
  using T = InputEvent::Type;
  TEST_ASSERT_TRUE(p.handle(button(T::Click, 0), t));
  TEST_ASSERT_TRUE(p.handle(button(T::Click, 1), t));
  TEST_ASSERT_TRUE(p.handle(button(T::Click, 2), t));
  TEST_ASSERT_EQUAL_STRING("prev;playpause;next;", t.log.c_str());
  // HoldEnd, a B repeat, and glass events do nothing.
  TEST_ASSERT_FALSE(p.handle(button(T::HoldEnd, 0), t));
  TEST_ASSERT_FALSE(p.handle(button(T::Repeat, 1), t));
  InputEvent tap;
  tap.type = T::Tap;
  TEST_ASSERT_FALSE(p.handle(tap, t));
  TEST_ASSERT_EQUAL_STRING("prev;playpause;next;", t.log.c_str());
  TEST_ASSERT_EQUAL_INT((int)ButtonPolicy::Hud::None, (int)p.feedback().kind);
}

void test_policy_holds_step_the_volume() {
  ButtonPolicy p;
  FakeTransport t;
  using T = InputEvent::Type;
  p.handle(button(T::Hold, 0, 500), t);
  p.handle(button(T::Repeat, 0, 700), t);
  p.handle(button(T::Repeat, 0, 900), t);
  TEST_ASSERT_EQUAL_STRING("vol-5;vol-5;vol-5;", t.log.c_str());
  TEST_ASSERT_EQUAL_INT(35, t.vol);
  TEST_ASSERT_EQUAL_INT((int)ButtonPolicy::Hud::Volume, (int)p.feedback().kind);
  TEST_ASSERT_EQUAL_INT(35, p.feedback().volume);
  TEST_ASSERT_EQUAL_UINT32(900, p.feedback().ms);
  TEST_ASSERT_EQUAL_UINT32(3, p.feedback().seq);
  p.handle(button(T::Hold, 2, 2000), t);
  TEST_ASSERT_EQUAL_INT(40, p.feedback().volume);
}

void test_policy_b_hold_to_the_speaker_pauses_first() {
  ButtonPolicy p;
  using T = InputEvent::Type;
  // Bluetooth, playing: pause, then the speaker.
  FakeTransport t;
  p.handle(button(T::Hold, 1, 600), t);
  TEST_ASSERT_EQUAL_STRING("pause;to-speaker;", t.log.c_str());
  TEST_ASSERT_FALSE(t.isPlaying);
  TEST_ASSERT_EQUAL_INT((int)ButtonPolicy::Hud::Output, (int)p.feedback().kind);
  TEST_ASSERT_FALSE(p.feedback().toBluetooth);
  TEST_ASSERT_TRUE(p.feedback().paused);
  // Back to Bluetooth: no pause, no play (B plays it).
  t.log.clear();
  p.handle(button(T::Hold, 1, 3000), t);
  TEST_ASSERT_EQUAL_STRING("to-bt;", t.log.c_str());
  TEST_ASSERT_TRUE(p.feedback().toBluetooth);
  TEST_ASSERT_FALSE(p.feedback().paused);
  // Paused already: to the speaker without another pause.
  t.log.clear();
  p.handle(button(T::Hold, 1, 5000), t);
  TEST_ASSERT_EQUAL_STRING("to-speaker;", t.log.c_str());
  TEST_ASSERT_FALSE(p.feedback().paused);
  // Speaker, playing, to Bluetooth: keeps playing.
  t.log.clear();
  t.isPlaying = true;
  p.handle(button(T::Hold, 1, 7000), t);
  TEST_ASSERT_EQUAL_STRING("to-bt;", t.log.c_str());
  TEST_ASSERT_TRUE(t.isPlaying);
  // Refused (silent test mode): said so.
  t.log.clear();
  t.bluetooth = false;
  t.allowSwitch = false;
  p.handle(button(T::Hold, 1, 9000), t);
  TEST_ASSERT_EQUAL_STRING("refused;", t.log.c_str());
  TEST_ASSERT_TRUE(p.feedback().refused);
}

// Headphones asked for, not connected yet: the music still plays on the
// speaker. A B hold cancels them and leaves it playing (as the Output
// tab's Speaker row does); only music on the headphones is paused first.
void test_policy_b_hold_cancelling_a_connection_keeps_the_speaker_playing() {
  ButtonPolicy p;
  using T = InputEvent::Type;
  FakeTransport t;
  t.bluetooth = true;  // on its way
  t.audioBt = false;   // the audio on the speaker meanwhile
  t.isPlaying = true;
  p.handle(button(T::Hold, 1, 600), t);
  TEST_ASSERT_EQUAL_STRING("to-speaker;", t.log.c_str());
  TEST_ASSERT_TRUE(t.isPlaying);
  TEST_ASSERT_FALSE(p.feedback().paused);
  TEST_ASSERT_FALSE(p.feedback().toBluetooth);
}

// Nothing to play: the clicks are inert (false: the "inert" buzz); the
// volume holds still work.
void test_policy_clicks_with_nothing_to_play_are_inert() {
  ButtonPolicy p;
  using T = InputEvent::Type;
  FakeTransport t;
  t.empty = true;
  TEST_ASSERT_FALSE(p.handle(button(T::Click, 0), t));
  TEST_ASSERT_FALSE(p.handle(button(T::Click, 1), t));
  TEST_ASSERT_FALSE(p.handle(button(T::Click, 2), t));
  TEST_ASSERT_EQUAL_STRING("", t.log.c_str());
  TEST_ASSERT_TRUE(p.handle(button(T::Hold, 2, 500), t));
  TEST_ASSERT_EQUAL_STRING("vol5;", t.log.c_str());
}

// ---- TouchRecognizer ----

namespace {

struct Rec {
  TouchRecognizer r;
  std::vector<InputEvent> events;
  void at(uint32_t ms, bool pressed, int x = 0, int y = 0, uint8_t edges = 0) {
    TouchRecognizer::Sample s;
    s.pressed = pressed;
    s.x = static_cast<int16_t>(x);
    s.y = static_cast<int16_t>(y);
    s.rawX = static_cast<int16_t>(x + 1000);  // tells raw from corrected
    s.rawY = static_cast<int16_t>(y);         // the strip test reads it
    s.edges = edges;
    InputEvent out[TouchRecognizer::kMaxEvents];
    const int n = r.update(ms, s, out);
    for (int i = 0; i < n; ++i) events.push_back(out[i]);
  }
  std::string types() const {
    std::string s;
    for (const auto& e : events) s += std::string(InputEvent::name(e.type)) + ";";
    return s;
  }
};

}  // namespace

void test_touch_tap_and_long_press() {
  Rec t;
  t.at(0, true, 100, 120);
  t.at(30, true, 104, 118);  // within the slop
  t.at(90, false);
  TEST_ASSERT_EQUAL_STRING("down;tap;", t.types().c_str());
  const InputEvent& tap = t.events[1];
  TEST_ASSERT_EQUAL_INT(100, tap.x);  // where it landed
  TEST_ASSERT_EQUAL_INT(120, tap.y);
  TEST_ASSERT_EQUAL_INT(1100, tap.rawX);
  TEST_ASSERT_EQUAL_INT(4, tap.dx);
  TEST_ASSERT_EQUAL_UINT32(90, tap.ms);
  TEST_ASSERT_TRUE(tap.isGlass());
  TEST_ASSERT_FALSE(tap.isButton());

  Rec h;
  h.at(0, true, 50, 60);
  h.at(499, true, 52, 61);
  TEST_ASSERT_EQUAL_STRING("down;", h.types().c_str());
  h.at(500, true, 52, 61);  // fires while still down
  h.at(900, true, 70, 90);  // moving after it doesn't make a drag
  h.at(950, false);
  TEST_ASSERT_EQUAL_STRING("down;long press;release;", h.types().c_str());
  TEST_ASSERT_EQUAL_INT(50, h.events[1].x);
}

// The A-Z rail (ListView calls noHold() on the Down there): a finger that
// rests on it past the hold time, then slides, still drags (it scrubs), and
// no LongPress means no hold tick. Only that touch: the next one holds.
void test_touch_no_hold_for_the_rail() {
  Rec r;
  r.at(0, true, 300, 100);
  r.r.noHold();
  r.at(300, true, 301, 102);
  r.at(600, true, 301, 102);  // past 500 ms, still within the slop
  r.at(700, true, 301, 102);
  TEST_ASSERT_EQUAL_STRING("down;", r.types().c_str());
  r.at(750, true, 300, 130);  // then it slides
  r.at(780, true, 300, 160);
  r.at(800, false);
  TEST_ASSERT_EQUAL_STRING("down;drag start;drag;drag end;fling;", r.types().c_str());
  // Resting and lifting without moving: a tap, however long it was down.
  Rec t;
  t.at(0, true, 300, 100);
  t.r.noHold();
  t.at(900, true, 300, 100);
  t.at(950, false);
  TEST_ASSERT_EQUAL_STRING("down;tap;", t.types().c_str());
  // The next touch has its hold again.
  t.at(1000, true, 100, 100);
  t.at(1600, true, 100, 100);
  TEST_ASSERT_EQUAL_STRING("down;tap;down;long press;", t.types().c_str());
}

void test_touch_drag_and_capped_fling() {
  Rec d;
  d.at(0, true, 150, 200);
  d.at(16, true, 150, 190);  // 10 px: still within the slop
  d.at(32, true, 150, 180);  // 20 px: a drag
  d.at(48, true, 150, 180);  // no change, no event
  d.at(64, true, 151, 170);
  // 10 px per 16 ms = 625 px/s at the lift, upwards: a fling.
  d.at(80, true, 151, 160);
  d.at(96, false);
  TEST_ASSERT_EQUAL_STRING("down;drag start;drag;drag;drag end;fling;", d.types().c_str());
  TEST_ASSERT_EQUAL_INT(-20, d.events[1].dy);  // from where it landed
  TEST_ASSERT_EQUAL_INT(-10, d.events[2].dy);  // since the last
  const InputEvent& end = d.events[4];
  TEST_ASSERT_EQUAL_INT(160, end.y);
  TEST_ASSERT_EQUAL_INT(-40, end.dy);
  TEST_ASSERT_TRUE(end.vy < -400 && end.vy > -800);
  TEST_ASSERT_EQUAL_FLOAT(end.vy, d.events[5].vy);

  // A flick at 5,000 px/s is capped at 2,000, the direction kept.
  Rec f;
  f.at(0, true, 100, 230);
  for (int i = 1; i <= 5; ++i) f.at(i * 10, true, 100 + 5 * i, 230 - 50 * i);
  f.at(60, false);
  const InputEvent& fl = f.events.back();
  TEST_ASSERT_EQUAL_INT((int)InputEvent::Type::Fling, (int)fl.type);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 2000.0f, std::sqrt(fl.vx * fl.vx + fl.vy * fl.vy));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.1f, fl.vx / -fl.vy);

  // A drag that stops before the lift: no fling.
  Rec s;
  s.at(0, true, 100, 100);
  s.at(20, true, 100, 140);
  for (uint32_t ms = 40; ms <= 200; ms += 20) s.at(ms, true, 100, 140);
  s.at(220, false);
  TEST_ASSERT_EQUAL_STRING("down;drag start;drag end;", s.types().c_str());
  TEST_ASSERT_EQUAL_FLOAT(0, s.events.back().vy);
}

void test_touch_button_strip_and_cancel() {
  // Landing on the strip (y >= 240) is BtnA/B/C's: no glass events at all.
  Rec b;
  b.at(0, true, 160, 255);
  b.at(100, true, 160, 200);
  b.at(600, true, 160, 200);
  b.at(700, false);
  TEST_ASSERT_EQUAL_STRING("", b.types().c_str());
  // Landing on the glass and sliding onto the strip stays a glass drag.
  Rec g;
  g.at(0, true, 160, 200);
  g.at(20, true, 160, 250);
  g.at(40, false);
  TEST_ASSERT_EQUAL_STRING("down;drag start;drag end;fling;", g.types().c_str());
  // Cancel: one Cancel event, then nothing until the finger lifts.
  Rec c;
  c.at(0, true, 10, 10);
  const InputEvent e = c.r.cancel(5);
  TEST_ASSERT_EQUAL_INT((int)InputEvent::Type::Cancel, (int)e.type);
  c.at(600, true, 60, 60);
  c.at(700, false);
  TEST_ASSERT_EQUAL_STRING("down;", c.types().c_str());
  TEST_ASSERT_EQUAL_INT((int)InputEvent::Type::None, (int)c.r.cancel(800).type);
  c.at(800, true, 10, 10);
  c.at(850, false);
  TEST_ASSERT_EQUAL_STRING("down;down;tap;", c.types().c_str());
}

void test_touch_right_edge_flag() {
  Rec t;
  t.at(0, true, 282, 20, InputEvent::kEdgeRight);  // read 319: clamped
  t.at(50, false);
  const InputEvent& tap = t.events.back();
  TEST_ASSERT_TRUE(tap.atRightEdge());
  TEST_ASSERT_TRUE(tap.inRightEdgeZone(300));  // a control at 300-319 still gets it
  InputEvent plain;
  plain.x = 282;
  TEST_ASSERT_FALSE(plain.inRightEdgeZone(300));
  TEST_ASSERT_TRUE(plain.inRightEdgeZone(280));
}

// ---- the fling cap in KineticScroll ----

void test_kinetic_scroll_caps_flings() {
  KineticScroll s;
  TEST_ASSERT_EQUAL_FLOAT(2000.0f, s.config().maxPxPerS);
  s.setExtent(500 * 42, 168);
  s.fling(0, 5000);
  TEST_ASSERT_EQUAL_FLOAT(2000.0f, s.velocity());
  s.fling(0, -9000);
  TEST_ASSERT_EQUAL_FLOAT(-2000.0f, s.velocity());
  // release() with the input layer's velocity: a finger moving up at
  // 1,500 px/s flings the list up (the offset grows).
  s.jumpTo(100 * 42);
  s.press(0, 200);
  s.drag(16, 170);
  s.release(20, 1500.0f);
  TEST_ASSERT_EQUAL_INT((int)KineticScroll::Phase::Flinging, (int)s.phase());
  TEST_ASSERT_EQUAL_FLOAT(1500.0f, s.velocity());
  // Slow: a snap to a row instead.
  s.jumpTo(100 * 42 + 5);
  s.press(40, 200);
  s.release(50, 20.0f);
  TEST_ASSERT_EQUAL_INT((int)KineticScroll::Phase::Snapping, (int)s.phase());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_default_table_is_the_fit_of_the_logs);
  RUN_TEST(test_default_table_puts_each_target_where_it_was);
  RUN_TEST(test_fit_recovers_a_distortion_from_nine_taps);
  RUN_TEST(test_fit_stays_monotonic_on_contradictory_taps);
  RUN_TEST(test_axis_map_outside_the_knots_keeps_slope_one);
  RUN_TEST(test_calibration_save_and_load);
  RUN_TEST(test_button_click_and_hold);
  RUN_TEST(test_button_hold_repeats_on_schedule);
  RUN_TEST(test_policy_clicks_are_transport);
  RUN_TEST(test_policy_holds_step_the_volume);
  RUN_TEST(test_policy_b_hold_to_the_speaker_pauses_first);
  RUN_TEST(test_policy_b_hold_cancelling_a_connection_keeps_the_speaker_playing);
  RUN_TEST(test_policy_clicks_with_nothing_to_play_are_inert);
  RUN_TEST(test_touch_tap_and_long_press);
  RUN_TEST(test_touch_no_hold_for_the_rail);
  RUN_TEST(test_touch_drag_and_capped_fling);
  RUN_TEST(test_touch_button_strip_and_cancel);
  RUN_TEST(test_touch_right_edge_flag);
  RUN_TEST(test_kinetic_scroll_caps_flings);
  return UNITY_END();
}
