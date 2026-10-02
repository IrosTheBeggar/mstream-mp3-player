// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for HostClock (docs/USB-VISUALIZER.md "The heard clock"): the
// computer's @c samples arrive late by a varying delay; the clock must
// follow the least delayed ones, never jump its phase except at a snap,
// change its rate by at most 5 %, and go invalid when the computer pauses
// or goes quiet. Arrivals are simulated against a known heard frame.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "HostClock.h"

namespace {
constexpr uint32_t kRate = 44100;

// The computer: its heard frame at time t (microseconds on the Core2's
// counter, which may wrap), with its own audio clock `ppm` off the Core2's.
struct Sender {
  uint32_t t0 = 1000000;  // its frame h0 at t0
  double h0 = 0.0;
  double ppm = 0.0;
  uint32_t rate = kRate;
  double heard(uint32_t t) const { return h0 + rate * (1.0 + ppm * 1e-6) * static_cast<int32_t>(t - t0) * 1e-6; }
};

// The frames of a Heard as one number.
double framesOf(const HostClock::Heard& h) { return h.frame + static_cast<double>(h.frac); }

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 1e9;
  std::sort(v.begin(), v.end());
  return v[static_cast<size_t>(std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5)))];
}

// Runs `seconds` of @c at 10 Hz, written at the sender's times and arriving
// `delay(k)` us later; checks the clock every 5 ms between arrivals with
// `check(t, clock frames, the least delay in the window)`.
template <typename Delay, typename Check>
void run(HostClock& c, const Sender& s, double seconds, Delay&& delay, Check&& check, uint32_t from = 0) {
  struct Arrival {
    uint32_t at;
    uint32_t delay;
  };
  std::vector<Arrival> window;
  const int writes = static_cast<int>(seconds * 10);
  uint32_t t = s.t0 + from;
  for (int k = 0; k < writes; ++k) {
    const uint32_t written = s.t0 + from + static_cast<uint32_t>(k) * 100000u;
    const uint32_t d = delay(k);
    const uint32_t arrives = written + d;
    // The checks up to this arrival.
    for (; static_cast<int32_t>(arrives - t) > 0; t += 5000) {
      if (window.empty()) continue;
      uint32_t least = UINT32_MAX;
      for (const Arrival& a : window) {
        if (static_cast<int32_t>(t - a.at) <= static_cast<int32_t>(HostClock::kWindowUs)) least = std::min(least, a.delay);
      }
      check(t, c.at(t), least);
    }
    c.sample(arrives, static_cast<int32_t>(std::floor(s.heard(written))), true);
    window.push_back({arrives, d});
  }
}
}  // namespace

void setUp() {}
void tearDown() {}

// No delay: the clock is the computer's heard frame (to the floor of the
// frame it sent: the sender sends whole frames).
void test_no_delay_is_exact() {
  HostClock c;
  c.start(kRate);
  Sender s;
  s.h0 = 0.25;  // (whole frames sent: the clock is up to a frame behind the true one)
  double worst = 0.0;
  run(c, s, 10.0, [](int) { return 0u; }, [&](uint32_t t, const HostClock::Heard& h, uint32_t) {
    TEST_ASSERT_TRUE(h.valid);
    worst = std::max(worst, std::fabs(framesOf(h) - (s.heard(t) - 0.25)));
  });
  char msg[64];
  snprintf(msg, sizeof(msg), "worst %.4f frames", worst);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(worst < 0.01, msg);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(s.t0 + 10000000).snaps);  // only the first
}

// A constant delay: behind by exactly that delay.
void test_constant_delay_is_behind_by_it() {
  HostClock c;
  c.start(kRate);
  Sender s;
  double worst = 0.0;
  run(c, s, 10.0, [](int) { return 12000u; }, [&](uint32_t t, const HostClock::Heard& h, uint32_t) {
    worst = std::max(worst, std::fabs(framesOf(h) - (s.heard(t) - 0.012 * kRate)));
  });
  TEST_ASSERT_TRUE(worst < 1.0);  // (whole frames sent)
}

// 2-32 ms delays (uniform) with a 100 ms outlier every 2 s: from 2 s on,
// the clock stays within 2.5 ms of the computer's frame less the least
// delay in the window (p95; measured 1.2-2.4 ms over these seeds: the lag
// of the slew when the least delayed sample leaves the window), its spread
// against the computer's own frame (p5-p95) is under 8 ms (1.9-5.7), its rate is
// never more than 5 % off, and it never snaps after the first sample.
void test_jitter_and_outliers() {
  for (uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    HostClock c;
    c.start(kRate);
    Sender s;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<uint32_t> jitter(0, 30000);
    std::vector<double> off, behind;
    double lastF = 0.0;
    uint32_t lastT = 0;
    uint32_t lastSnaps = 0;
    double worstSlope = 0.0;
    run(c, s, 60.0, [&](int k) { return k % 20 == 7 ? 100000u : 2000u + jitter(rng); },
        [&](uint32_t t, const HostClock::Heard& h, uint32_t least) {
          const double f = framesOf(h);
          if (static_cast<int32_t>(t - s.t0) >= 2000000) {
            off.push_back((f - (s.heard(t) - least * 1e-6 * kRate)) * 1000.0 / kRate);
            behind.push_back((s.heard(t) - f) * 1000.0 / kRate);
          }
          const uint32_t snaps = c.stats(t).snaps;
          if (lastT && snaps == lastSnaps) {
            const double slope = (f - lastF) / (kRate * static_cast<int32_t>(t - lastT) * 1e-6);
            worstSlope = std::max(worstSlope, std::fabs(slope - 1.0));
          }
          lastF = f;
          lastT = t;
          lastSnaps = snaps;
        });
    std::vector<double> absOff;
    for (double o : off) absOff.push_back(std::fabs(o));
    const double spread = percentile(behind, 0.95) - percentile(behind, 0.05);
    char msg[200];
    snprintf(msg, sizeof(msg),
             "seed %u: off the leading edge median %+.2f ms, p95 |%.2f| ms; behind the computer median %.2f ms, "
             "p5-p95 %.2f ms; rate off by at most %.2f %%; %lu snaps",
             seed, percentile(off, 0.5), percentile(absOff, 0.95), percentile(behind, 0.5), spread, worstSlope * 100,
             static_cast<unsigned long>(c.stats(lastT).snaps));
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(percentile(absOff, 0.95) < 2.5, msg);
    TEST_ASSERT_TRUE_MESSAGE(spread < 8.0, msg);
    TEST_ASSERT_TRUE_MESSAGE(percentile(behind, 0.05) > 1.5, msg);  // never ahead of what arrives first
    TEST_ASSERT_TRUE_MESSAGE(worstSlope <= 0.0501, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, c.stats(lastT).snaps, msg);
    // The spread: the delivery jitter, a few tens of ms.
    const HostClock::Stats st = c.stats(lastT);
    TEST_ASSERT_TRUE(st.valid);
    TEST_ASSERT_TRUE(st.spreadMs > 10.0f && st.spreadMs < 120.0f);
    TEST_ASSERT_TRUE(st.samples >= 18 && st.samples <= HostClock::kMaxSamples);
  }
}

// A step in the computer's frames (it corrected its anchor): 50 ms slews,
// no snap, the rate within 5 %, converging; 150 ms snaps.
void test_slew_and_snap_thresholds() {
  HostClock c;
  c.start(kRate);
  uint32_t t = 0;
  double h = 0.0;
  auto step = [&](double jumpMs) {
    h += jumpMs * 1e-3 * kRate;
    for (int k = 0; k < 40; ++k) {  // 4 s
      c.sample(t, static_cast<int32_t>(std::lround(h)), true);
      t += 100000;
      h += 0.1 * kRate;
    }
  };
  step(0);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(t).snaps);
  // +50 ms: below the snap threshold. The window still holds the old
  // samples, but the new ones lead: the edge moves 50 ms at once and the
  // clock slews after it.
  const double before = c.frameAt(t);
  h += 0.05 * kRate;
  c.sample(t, static_cast<int32_t>(std::lround(h)), true);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(t).snaps);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.05f, c.stats(t).slew);  // the most it may
  // 100 ms later it has taken out 5 ms (5 % of 100 ms), not jumped.
  TEST_ASSERT_FLOAT_WITHIN(2.0, before + 0.1 * kRate * 1.05, c.frameAt(t + 100000));
  t += 100000;
  h += 0.1 * kRate;
  step(0);  // converged within 4 s
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(t).snaps);
  TEST_ASSERT_FLOAT_WITHIN(1.0, h - 0.1 * kRate, c.frameAt(t - 100000));
  // -50 ms (the computer went back a little): the window's leading edge
  // stays on the old samples for 2 s, then follows; slewed, never snapped.
  h -= 0.05 * kRate;
  step(0);
  step(0);  // (2 s before the edge moves, then the slew)
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(t).snaps);
  TEST_ASSERT_FLOAT_WITHIN(1.0, h - 0.1 * kRate, c.frameAt(t - 100000));
  // +150 ms: a snap.
  step(150);
  TEST_ASSERT_EQUAL_UINT32(2, c.stats(t).snaps);
  TEST_ASSERT_FLOAT_WITHIN(1.0, h - 0.1 * kRate, c.frameAt(t - 100000));
}

// The computer's audio clock 200 ppm off the Core2's: followed within 2 ms.
void test_drift_is_followed() {
  for (double ppm : {200.0, -200.0}) {
    HostClock c;
    c.start(48000);
    Sender s;
    s.rate = 48000;
    s.ppm = ppm;
    double worst = 0.0;
    run(c, s, 120.0, [](int) { return 3000u; }, [&](uint32_t t, const HostClock::Heard& h, uint32_t) {
      if (static_cast<int32_t>(t - s.t0) < 2000000) return;
      worst = std::max(worst, std::fabs(framesOf(h) - (s.heard(t) - 0.003 * 48000)) * 1000.0 / 48000);
    });
    char msg[64];
    snprintf(msg, sizeof(msg), "%+.0f ppm: worst %.3f ms", ppm, worst);
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE_MESSAGE(worst < 2.0, msg);
    TEST_ASSERT_EQUAL_UINT32(1, c.stats(s.t0 + 120000000).snaps);
  }
}

// playing 0: invalid at once (the dancer idles); the next playing 1 snaps,
// to where the computer says, with nothing left of before.
void test_pause_and_resume() {
  HostClock c;
  c.start(kRate);
  TEST_ASSERT_FALSE(c.at(0).valid);  // nothing yet
  uint32_t t = 0;
  for (int k = 0; k < 20; ++k, t += 100000) c.sample(t, static_cast<int32_t>(k * 4410), true);
  TEST_ASSERT_TRUE(c.at(t).valid);
  c.sample(t, 88200, false);
  TEST_ASSERT_FALSE(c.at(t).valid);
  TEST_ASSERT_FALSE(c.at(t + 1).valid);
  TEST_ASSERT_FALSE(c.stats(t).valid);
  // Paused for 5 s: @c goes on at 10 Hz, playing 0, the frame where it stopped.
  for (int k = 0; k < 50; ++k) c.sample(t += 100000, 88200, false);
  TEST_ASSERT_FALSE(c.at(t).valid);
  TEST_ASSERT_EQUAL_INT(0, c.stats(t).samples);
  // Resumed: valid at once, from that frame.
  c.sample(t += 100000, 88200, true);
  const HostClock::Heard h = c.at(t);
  TEST_ASSERT_TRUE(h.valid);
  TEST_ASSERT_EQUAL_INT32(88200, h.frame);
  TEST_ASSERT_EQUAL_UINT32(2, c.stats(t).snaps);
  TEST_ASSERT_FLOAT_WITHIN(0.5, 88200 + 4410, framesOf(c.at(t + 100000)));
}

// 1.5 s without a sample: invalid (the computer went quiet or stalled).
void test_stale() {
  HostClock c;
  c.start(kRate);
  c.sample(1000, 0, true);
  TEST_ASSERT_TRUE(c.at(1000 + 1499999).valid);
  TEST_ASSERT_FALSE(c.at(1000 + 1500000).valid);
  TEST_ASSERT_FALSE(c.valid(1000 + 1500000));
  // Asked ahead of now (the LCD's lead): the validity is now's.
  TEST_ASSERT_TRUE(c.at(1000 + 1499000, 30000).valid);
  // The stats: the newest sample's age.
  TEST_ASSERT_EQUAL_UINT32(1500, c.stats(1000 + 1500000).ageMs);
  // start() forgets everything.
  c.sample(2000000, 100, true);
  c.start(48000);
  TEST_ASSERT_FALSE(c.at(2000000).valid);
  TEST_ASSERT_EQUAL_UINT32(0, c.stats(2000000).snaps);
  TEST_ASSERT_EQUAL_UINT32(48000, c.rate());
}

// The microsecond counter wraps every 71 minutes: nothing happens.
void test_counter_wrap() {
  HostClock c;
  c.start(kRate);
  Sender s;
  s.t0 = 0xFFFFFFFFu - 3000000u;  // wraps 3 s in
  double worst = 0.0;
  run(c, s, 10.0, [](int) { return 0u; }, [&](uint32_t t, const HostClock::Heard& h, uint32_t) {
    TEST_ASSERT_TRUE(h.valid);
    worst = std::max(worst, std::fabs(framesOf(h) - std::floor(s.heard(t))));
  });
  TEST_ASSERT_TRUE(worst < 1.0);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats(s.t0 + 10000000u).snaps);
}

// Frames before 0 (right after a seek, frame 0 not heard yet): negative,
// with the fraction counted up from the frame below.
void test_negative_frames() {
  HostClock c;
  c.start(kRate);
  c.sample(0, -6615, true);
  HostClock::Heard h = c.at(0);
  TEST_ASSERT_TRUE(h.valid);
  TEST_ASSERT_EQUAL_INT32(-6615, h.frame);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, h.frac);
  h = c.at(0, 11);  // 11 us: 0.4851 of a frame later
  TEST_ASSERT_EQUAL_INT32(-6615, h.frame);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.4851f, h.frac);
  h = c.at(0, 150000);  // 150 ms on: past frame 0
  TEST_ASSERT_EQUAL_INT32(0, h.frame);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, h.frac);
}

// Past 2^24 frames (6 minutes; a float would lose the fraction) and near
// 2^31 (12 h at 48 kHz): the fraction is still right.
void test_precision_far_into_an_epoch() {
  for (int32_t base : {1 << 24, 0x7F000000}) {
    HostClock c;
    c.start(48000);
    c.sample(0, base, true);
    for (int32_t us : {7, 14, 21, 1000}) {  // 0.336, 0.672, 1.008, 48 frames later
      const HostClock::Heard h = c.at(0, us);
      const double want = us * 48000e-6;
      TEST_ASSERT_EQUAL_INT32(base + static_cast<int32_t>(std::floor(want)), h.frame);
      TEST_ASSERT_FLOAT_WITHIN(0.001f, static_cast<float>(want - std::floor(want)), h.frac);
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_delay_is_exact);
  RUN_TEST(test_constant_delay_is_behind_by_it);
  RUN_TEST(test_jitter_and_outliers);
  RUN_TEST(test_slew_and_snap_thresholds);
  RUN_TEST(test_drift_is_followed);
  RUN_TEST(test_pause_and_resume);
  RUN_TEST(test_stale);
  RUN_TEST(test_counter_wrap);
  RUN_TEST(test_negative_frames);
  RUN_TEST(test_precision_far_into_an_epoch);
  return UNITY_END();
}
