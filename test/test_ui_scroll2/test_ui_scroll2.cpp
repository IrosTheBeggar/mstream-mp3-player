// Host tests for the UI spike's scroll round 2 (docs/UI-SPIKE.md): the
// hardware vertical scroll's bookkeeping (VScrollMap) and the decoder's
// gentle refill (RefillPacer). (The interaction boost, UiBoost, measured
// worse on the device and was removed with its tests.)
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "RefillPacer.h"
#include "VScrollMap.h"

void setUp() {}
void tearDown() {}

// ---- VScrollMap ----

namespace {

// A fake panel: GRAM lines hold the content line drawn there (-1: never).
struct FakePanel {
  static constexpr int kLines = 240;
  int gram[kLines];
  int linesDrawn = 0;
  FakePanel() {
    for (int& g : gram) g = -1;
  }
  void draw(const VScrollMap::Span* spans, int n) {
    for (int i = 0; i < n; ++i) {
      TEST_ASSERT_TRUE(spans[i].h > 0);
      for (int k = 0; k < spans[i].h; ++k) {
        const int g = spans[i].gramY + k;
        gram[g] = static_cast<int>(spans[i].contentY + k);
        ++linesDrawn;
      }
    }
  }
  // What the controller shows on screen line y, given VSCRDEF (top, height)
  // and VSCRSADD vsp, straight from the datasheet's rule (not from the map).
  int shown(int y, int top, int height, int vsp) const {
    if (y < top || y >= top + height) return gram[y];
    const int g = top + ((vsp - top) + (y - top)) % height;
    return gram[g];
  }
};

void checkShows(const FakePanel& p, const VScrollMap& m, int32_t offset) {
  for (int k = 0; k < m.height(); ++k) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(offset + k), p.shown(m.top() + k, m.top(), m.height(), m.vsp()));
    // The map's own screen -> GRAM answer agrees with the controller's rule.
    const int g = m.gramLineForScreen(m.top() + k);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(offset + k), p.gram[g]);
    TEST_ASSERT_EQUAL_INT(m.top() + k, m.screenLineForGram(g));
  }
}

}  // namespace

void test_vscroll_first_plan_draws_the_whole_area() {
  VScrollMap m;
  m.configure(72, 168);
  VScrollMap::Span s[VScrollMap::kMaxSpans];
  const int n = m.plan(0, s);
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT(0, s[0].contentY);
  TEST_ASSERT_EQUAL_INT(72, s[0].gramY);
  TEST_ASSERT_EQUAL_INT(168, s[0].h);
  TEST_ASSERT_EQUAL_UINT16(72, m.vsp());
  // Outside the area the mapping is the identity.
  TEST_ASSERT_EQUAL_INT(10, m.gramLineForScreen(10));
  TEST_ASSERT_EQUAL_INT(71, m.screenLineForGram(71));
}

void test_vscroll_small_moves_draw_only_the_new_lines() {
  VScrollMap m;
  m.configure(72, 168);
  FakePanel p;
  VScrollMap::Span s[VScrollMap::kMaxSpans];
  p.draw(s, m.plan(0, s));
  checkShows(p, m, 0);

  p.linesDrawn = 0;
  p.draw(s, m.plan(10, s));  // up by 10: 10 new lines at the bottom
  TEST_ASSERT_EQUAL_INT(10, p.linesDrawn);
  TEST_ASSERT_EQUAL_UINT16(82, m.vsp());
  checkShows(p, m, 10);

  p.linesDrawn = 0;
  p.draw(s, m.plan(3, s));  // down by 7: 7 new lines at the top
  TEST_ASSERT_EQUAL_INT(7, p.linesDrawn);
  checkShows(p, m, 3);

  p.linesDrawn = 0;
  p.draw(s, m.plan(3, s));  // no move: nothing
  TEST_ASSERT_EQUAL_INT(0, p.linesDrawn);
}

void test_vscroll_wraps_and_splits_spans() {
  VScrollMap m;
  m.configure(36, 204);  // the tab bar alone fixed: a 204-line list
  FakePanel p;
  VScrollMap::Span s[VScrollMap::kMaxSpans];
  p.draw(s, m.plan(0, s));
  p.draw(s, m.plan(190, s));  // a move of 190 lines: the address is at shift 190
  checkShows(p, m, 190);
  TEST_ASSERT_EQUAL_UINT16(36 + 190, m.vsp());
  // A move across the wrap point: the new lines straddle GRAM's end.
  int n = m.plan(230, s);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT(394, s[0].contentY);
  TEST_ASSERT_EQUAL_INT(36 + 190, s[0].gramY);
  TEST_ASSERT_EQUAL_INT(14, s[0].h);  // up to GRAM's end
  TEST_ASSERT_EQUAL_INT(408, s[1].contentY);
  TEST_ASSERT_EQUAL_INT(36, s[1].gramY);
  TEST_ASSERT_EQUAL_INT(26, s[1].h);
  p.draw(s, n);
  checkShows(p, m, 230);
  // A full redraw at a non-zero shift (26 now) wraps too: in place, from
  // the GRAM line on screen at the band's top.
  m.invalidate();
  n = m.plan(1000, s);
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT(1000, s[0].contentY);
  TEST_ASSERT_EQUAL_INT(36 + 26, s[0].gramY);
  TEST_ASSERT_EQUAL_INT(204 - 26, s[0].h);
  TEST_ASSERT_EQUAL_INT(1000 + 204 - 26, s[1].contentY);
  TEST_ASSERT_EQUAL_INT(36, s[1].gramY);
  TEST_ASSERT_EQUAL_INT(26, s[1].h);
  p.draw(s, n);
  checkShows(p, m, 1000);
}

void test_vscroll_big_jumps_and_invalidate_redraw_everything() {
  VScrollMap m;
  m.configure(72, 168);
  FakePanel p;
  VScrollMap::Span s[VScrollMap::kMaxSpans];
  p.draw(s, m.plan(0, s));
  p.linesDrawn = 0;
  p.draw(s, m.plan(168, s));  // exactly a screenful: everything is new
  TEST_ASSERT_EQUAL_INT(168, p.linesDrawn);
  checkShows(p, m, 168);
  p.linesDrawn = 0;
  p.draw(s, m.plan(5000, s));
  TEST_ASSERT_EQUAL_INT(168, p.linesDrawn);
  checkShows(p, m, 5000);
  m.invalidate();
  p.linesDrawn = 0;
  p.draw(s, m.plan(5001, s));
  TEST_ASSERT_EQUAL_INT(168, p.linesDrawn);
  checkShows(p, m, 5001);
}

void test_vscroll_random_walk_always_shows_the_offset() {
  for (int maxStep : {0, 84}) {  // 0: height - 1; 84: the scroll lab's
    VScrollMap m;
    m.configure(72, 168, maxStep);
    const int step = maxStep ? maxStep : 167;
    TEST_ASSERT_EQUAL_INT(step, m.maxStep());
    FakePanel p;
    VScrollMap::Span s[VScrollMap::kMaxSpans];
    std::srand(1234);
    int32_t off = 0;
    p.draw(s, m.plan(off, s));
    for (int i = 0; i < 3000; ++i) {
      // Mostly drags (a few px), some flick frames (tens to hundreds), some jumps.
      const int r = std::rand() % 100;
      int32_t d = r < 70 ? (std::rand() % 31) - 15 : r < 95 ? (std::rand() % 401) - 200 : (std::rand() % 20001) - 10000;
      if (r % 50 == 0) m.invalidate();
      off += d;
      if (off < 0) off = 0;
      const int before = p.linesDrawn;
      const bool valid = m.valid();
      const int32_t moved = off - m.offset();
      const uint16_t vspBefore = m.vsp();
      p.draw(s, m.plan(off, s));
      const int drawn = p.linesDrawn - before;
      const int32_t expect = moved < 0 ? -moved : moved;
      const bool full = !valid || expect > step;
      TEST_ASSERT_EQUAL(full, m.lastPlanFull());
      TEST_ASSERT_EQUAL_INT(full ? 168 : expect, drawn);
      // A full redraw keeps the panel's address: it is drawn in place.
      if (full) TEST_ASSERT_EQUAL_UINT16(vspBefore, m.vsp());
      checkShows(p, m, off);
    }
  }
}

// A full redraw (a jump, a big flick frame, after invalidate()) must write
// each content line straight into the screen line it will show on, under
// the address the panel ALREADY has: the band never shows a wrapped or
// shifted picture while it is drawn (it fills in top to bottom, in place).
void test_vscroll_full_redraw_draws_in_place_under_the_current_address() {
  VScrollMap m;
  m.configure(72, 168, 84);
  FakePanel p;
  VScrollMap::Span s[VScrollMap::kMaxSpans];
  p.draw(s, m.plan(0, s));
  p.draw(s, m.plan(50, s));  // a drag: the address moves (shift 50)
  const uint16_t panelVsp = m.vsp();
  TEST_ASSERT_EQUAL_UINT16(72 + 50, panelVsp);
  // A jump (shift 10 in the old scheme), a jump back, a step over maxStep,
  // and near the top.
  const int32_t jumps[] = {10 * 42 + 10, 100, 100 + 85, 7};
  for (int32_t off : jumps) {
    const int n = m.plan(off, s);
    TEST_ASSERT_TRUE(m.lastPlanFull());
    TEST_ASSERT_EQUAL_UINT16(panelVsp, m.vsp());  // nothing to send
    // Line by line, as the pushes land: each one shows at its final place
    // under the address the panel has, in content order, top to bottom.
    int expectScreen = 72;
    for (int i = 0; i < n; ++i) {
      for (int k = 0; k < s[i].h; ++k) {
        p.gram[s[i].gramY + k] = static_cast<int>(s[i].contentY + k);
        const int y = m.screenLineForGram(s[i].gramY + k);
        TEST_ASSERT_EQUAL_INT(expectScreen, y);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(s[i].contentY + k), p.shown(y, 72, 168, panelVsp));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(off) + (y - 72), static_cast<int>(s[i].contentY + k));
        ++expectScreen;
      }
    }
    TEST_ASSERT_EQUAL_INT(72 + 168, expectScreen);
    checkShows(p, m, off);
  }
  // Incremental moves go on from the new base.
  p.linesDrawn = 0;
  p.draw(s, m.plan(7 + 30, s));
  TEST_ASSERT_FALSE(m.lastPlanFull());
  TEST_ASSERT_EQUAL_INT(30, p.linesDrawn);
  checkShows(p, m, 37);
}

// ---- RefillPacer ----

void test_pacer_flat_out_when_off_or_low() {
  RefillPacer::Config c;
  TEST_ASSERT_TRUE(c.enabled);  // on by default (1.5x from 500 ms)
  c.enabled = false;            // off: a plain 1 ms per pass, flat out
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 1200, 1024, 44100, 9300));
  c.enabled = true;
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 0, 1024, 44100, 9300));
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 499, 1024, 44100, 9300));
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 800, 0, 44100, 9300));  // nothing produced
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 800, 1024, 0, 9300));   // no rate yet
}

void test_pacer_caps_the_rate_above_the_threshold() {
  RefillPacer::Config c;
  c.enabled = true;  // 1.5x from 500 ms
  // An MP3 pass: 1024 frames (23.2 ms of audio) in 9.3 ms. At 1.5x a pass
  // must take >= 15.48 ms: sleep 6.18 -> 7 ms (rounded up).
  TEST_ASSERT_EQUAL_UINT32(7, RefillPacer::sleepMs(c, 500, 1024, 44100, 9300));
  // FLAC, 7.2 ms a pass: 9 ms.
  TEST_ASSERT_EQUAL_UINT32(9, RefillPacer::sleepMs(c, 1200, 1024, 44100, 7200));
  // A pass already slower than the cap (SD waits): the usual 1 ms.
  TEST_ASSERT_EQUAL_UINT32(1, RefillPacer::sleepMs(c, 800, 1024, 44100, 16000));
  // 2x: 11.6 ms a pass.
  c.capX10 = 20;
  TEST_ASSERT_EQUAL_UINT32(3, RefillPacer::sleepMs(c, 800, 1024, 44100, 9300));
  // Never more than maxSleepMs (a pass of 1 s of audio in 0.1 ms).
  TEST_ASSERT_EQUAL_UINT32(c.maxSleepMs, RefillPacer::sleepMs(c, 800, 44100, 44100, 100));
  // Caps under kMinCapX10 (1.5x) are applied as 1.5x: near 1x the paced
  // ring would stop growing at 500 ms.
  c.capX10 = 10;
  TEST_ASSERT_EQUAL_UINT32(7, RefillPacer::sleepMs(c, 800, 1024, 44100, 9300));
  c.capX10 = 0;
  TEST_ASSERT_EQUAL_UINT32(7, RefillPacer::sleepMs(c, 800, 1024, 44100, 9300));
}

// At the lowest cap, with the sleep rounded up, a paced ring still grows
// (production well over 1x) for MP3- and FLAC-like passes: it can't settle
// at gentleFromMs.
void test_pacer_ring_always_grows_above_the_threshold() {
  RefillPacer::Config c;
  c.enabled = true;
  c.capX10 = 1;  // applied as kMinCapX10
  // (A pass slower than the cap, 15.5 ms here, isn't paced: that is the
  // decoder's own speed, as without pacing.)
  for (uint32_t passUs : {2000u, 7200u, 9300u, 15000u}) {
    const uint32_t sleep = RefillPacer::sleepMs(c, 600, 1024, 44100, passUs);
    // vTaskDelay(n) at 1 kHz sleeps up to n ms.
    const double wallUs = passUs + sleep * 1000.0;
    const double audioUs = 1024 * 1e6 / 44100;
    TEST_ASSERT_TRUE(audioUs / wallUs > 1.3);
  }
}

// Simulated refill from a track start: flat out (2.3x) to 500 ms, then the
// cap. The ring still fills (net growth), the decoder's share of the core
// above 500 ms drops to cap x load, and the first 500 ms are unchanged.
void test_pacer_refill_timeline() {
  RefillPacer::Config c;
  c.enabled = true;
  const double passUs = 9300, audioUs = 1024 * 1e6 / 44100;
  double t = 0, ring = 0, busy = 0, busyAbove = 0, timeAbove = 0, t500 = -1;
  while (ring < 1400) {
    const uint32_t sleep = RefillPacer::sleepMs(c, static_cast<uint32_t>(ring), 1024, 44100, 9300);
    const double wall = passUs + sleep * 1000.0;
    const bool above = ring >= 500;
    ring += audioUs / 1000.0 - wall / 1000.0;  // produced minus played
    busy += passUs;
    if (above) {
      busyAbove += passUs;
      timeAbove += wall;
    }
    t += wall;
    if (t500 < 0 && ring >= 500) t500 = t;
    TEST_ASSERT_TRUE(t < 10e6);
  }
  // To 500 ms at ~(23.2 / 10.3) - 1 = 1.25x net: ~0.4 s, as without pacing.
  TEST_ASSERT_TRUE(t500 > 300e3 && t500 < 500e3);
  // Above it the decoder takes ~60 % of the core (9.3 of 16.3 ms), not ~90 %.
  TEST_ASSERT_TRUE(busyAbove / timeAbove < 0.62);
  // And the whole fill is still done in under 3 s.
  TEST_ASSERT_TRUE(t < 3e6);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_vscroll_first_plan_draws_the_whole_area);
  RUN_TEST(test_vscroll_small_moves_draw_only_the_new_lines);
  RUN_TEST(test_vscroll_wraps_and_splits_spans);
  RUN_TEST(test_vscroll_big_jumps_and_invalidate_redraw_everything);
  RUN_TEST(test_vscroll_random_walk_always_shows_the_offset);
  RUN_TEST(test_vscroll_full_redraw_draws_in_place_under_the_current_address);
  RUN_TEST(test_pacer_flat_out_when_off_or_low);
  RUN_TEST(test_pacer_caps_the_rate_above_the_threshold);
  RUN_TEST(test_pacer_ring_always_grows_above_the_threshold);
  RUN_TEST(test_pacer_refill_timeline);
  return UNITY_END();
}
