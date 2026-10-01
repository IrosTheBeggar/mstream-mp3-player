// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the dancing figure's pose math (DancePose), plus the small
// helpers of the dance diagnostics (RollingStats, Base64). Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstring>
#include <string>

#include "Base64Text.h"
#include "DancePose.h"
#include "RollingStats.h"

using dance::Point;
using dance::Pose;

namespace {
const dance::Box kBox;

float dist(Point a, Point b) { return std::hypot(a.x - b.x, a.y - b.y); }
float lowestFoot(const Pose& p) { return std::max(p.footL.y, p.footR.y); }
float torso(const Pose& p) { return dist(p.hip, p.neck); }

// Every joint of the pose, for the checks that apply to all of them.
void joints(const Pose& p, Point out[14]) {
  const Point all[] = {p.head,  p.neck,  p.hip,   p.shoulderL, p.elbowL, p.handL, p.shoulderR,
                       p.elbowR, p.handR, p.kneeL, p.footL,     p.kneeR,  p.footR, p.head};
  for (int i = 0; i < 14; ++i) out[i] = all[i];
}

float maxJointMove(const Pose& a, const Pose& b) {
  Point ja[14], jb[14];
  joints(a, ja);
  joints(b, jb);
  float m = 0.0f;
  for (int i = 0; i < 14; ++i) m = std::max(m, dist(ja[i], jb[i]));
  return m;
}
}  // namespace

void setUp() {}
void tearDown() {}

// Feet on the ground at the beat; at the top of the hop (phi 0.5) the body
// is highest.
void test_contact_on_the_beat() {
  const Pose beat = dance::dancePose(0.0f, false);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kBox.groundY, beat.footL.y);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kBox.groundY, beat.footR.y);
  float highest = 1e9f, at = -1.0f;
  for (int i = 0; i < 100; ++i) {
    const Pose p = dance::dancePose(i / 100.0f, false);
    if (p.hip.y < highest) {
      highest = p.hip.y;
      at = i / 100.0f;
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.5f, at);
  // The feet leave the ground at the top of the hop, and are back by the beat.
  TEST_ASSERT_TRUE(lowestFoot(dance::dancePose(0.5f, false)) < kBox.groundY - 1.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, kBox.groundY, lowestFoot(dance::dancePose(0.97f, false)));
}

// Squash on impact (hip lowest, torso shortest within ~0.05 beat of the
// beat: under a frame) and an anticipation stretch just before it.
void test_squash_and_stretch() {
  const Pose mid = dance::dancePose(0.5f, false);
  float lowestHip = -1e9f, shortest = 1e9f, lowAt = -1.0f, shortAt = -1.0f;
  for (int i = 0; i < 200; ++i) {
    const float phi = i / 200.0f;
    const Pose p = dance::dancePose(phi, false);
    if (p.hip.y > lowestHip) {
      lowestHip = p.hip.y;
      lowAt = phi;
    }
    if (torso(p) < shortest) {
      shortest = torso(p);
      shortAt = phi;
    }
  }
  TEST_ASSERT_TRUE(lowAt <= 0.05f);
  TEST_ASSERT_TRUE(shortAt <= 0.05f);
  TEST_ASSERT_TRUE(shortest < torso(mid) - 2.0f);
  TEST_ASSERT_TRUE(torso(dance::dancePose(0.93f, false)) > torso(mid) + 1.5f);
}

// The head bobs lowest (relative to the neck) about 0.1 beat after the beat.
void test_head_bob_lags_the_beat() {
  float lowest = -1e9f, at = -1.0f;
  for (int i = 0; i < 100; ++i) {
    const Pose p = dance::dancePose(i / 100.0f, false);
    const float drop = p.head.y - p.neck.y;
    if (drop > lowest) {
      lowest = drop;
      at = i / 100.0f;
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(0.03f, 0.1f, at);
}

// The arms alternate: on an even beat the screen-right hand is up (above the
// head), on an odd one the other; each is at its extreme on the beat.
void test_arms_alternate_with_extremes_on_the_beat() {
  const Pose even = dance::dancePose(0.0f, false);
  const Pose odd = dance::dancePose(0.0f, true);
  TEST_ASSERT_TRUE(even.handL.y < even.head.y);
  TEST_ASSERT_TRUE(even.handR.y > even.shoulderR.y);
  TEST_ASSERT_TRUE(odd.handR.y < odd.head.y);
  TEST_ASSERT_TRUE(odd.handL.y > odd.shoulderL.y);
  // The raised hand is highest (relative to its shoulder) on the beat.
  for (int i = 1; i < 100; ++i) {
    const Pose p = dance::dancePose(i / 100.0f, false);
    TEST_ASSERT_TRUE(p.handL.y - p.shoulderL.y >= even.handL.y - even.shoulderL.y - 0.01f);
  }
}

// Continuous across the beat: the end of an even beat is the start of the
// odd one, so the figure never jumps.
void test_continuous_across_beats() {
  const Pose endEven = dance::dancePose(0.9999f, false);
  const Pose startOdd = dance::dancePose(0.0f, true);
  TEST_ASSERT_TRUE(maxJointMove(endEven, startOdd) < 0.5f);
  // Within a beat nothing jumps: in steps of 1/200 beat joints move < 3 px
  // (a knee near straight moves fastest).
  for (int i = 0; i + 1 < 200; ++i) {
    const Pose a = dance::dancePose(i / 200.0f, false), b = dance::dancePose((i + 1) / 200.0f, false);
    TEST_ASSERT_TRUE(maxJointMove(a, b) < 3.0f);
  }
}

// Bones keep their length, the figure stays in its box, above the ground.
void test_bones_and_box() {
  for (int k = 0; k < 2; ++k) {
    for (int i = 0; i < 50; ++i) {
      const float phi = i / 50.0f;
      const Pose poses[] = {dance::dancePose(phi, k == 1), dance::idlePose(phi * 8.0f),
                            dance::blend(dance::idlePose(phi), dance::dancePose(phi, k == 1), 0.5f)};
      for (int j = 0; j < 2; ++j) {  // the blend doesn't keep lengths exactly
        const Pose& p = poses[j];
        TEST_ASSERT_FLOAT_WITHIN(0.05f, dance::kUpperArm, dist(p.shoulderL, p.elbowL));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, dance::kForearm, dist(p.elbowR, p.handR));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, dance::kThigh, dist({p.hip.x + 6.0f, p.hip.y}, p.kneeL));
        TEST_ASSERT_FLOAT_WITHIN(0.05f, dance::kShin, dist(p.kneeR, p.footR));
      }
      for (const Pose& p : poses) {
        Point all[14];
        joints(p, all);
        for (const Point& q : all) {
          TEST_ASSERT_TRUE(q.x >= 2.0f && q.x <= kBox.w - 2.0f);
          TEST_ASSERT_TRUE(q.y >= 2.0f && q.y <= kBox.groundY + 0.01f);
        }
        TEST_ASSERT_TRUE(p.head.y - p.headR >= 1.0f);
      }
    }
  }
}

// Low confidence: the idle sway (no hop); the Dancer fades between them
// smoothly and holds the last dance pose while a lost beat fades out.
void test_low_confidence_blends_to_idle() {
  TEST_ASSERT_EQUAL_FLOAT(0.0f, dance::danceWeight(0.1f));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, dance::danceWeight(0.9f));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, dance::danceWeight(0.5f));
  // Idle keeps both feet down.
  for (int i = 0; i < 40; ++i) {
    const Pose p = dance::idlePose(i * 0.1f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, kBox.groundY, p.footL.y);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, kBox.groundY, p.footR.y);
  }
  dance::Dancer d;
  const float dt = 1.0f / 30.0f;
  d.update(0.0f, false, 0.0f, false, dt);
  float phi = 0.0f;
  bool odd = false;
  for (int f = 0; f < 150; ++f) {  // 5 s: confidence jumps to 1 after 1 s, the beat is lost after 3.5 s
    phi += dt * 2.0f;  // 120 BPM
    if (phi >= 1.0f) {
      phi -= 1.0f;
      odd = !odd;
    }
    const float conf = f < 30 ? 0.0f : 1.0f;
    const bool beat = f < 105;
    const float before = d.weight();
    d.update(phi, odd, conf, beat, dt);
    if (f == 29) TEST_ASSERT_EQUAL_FLOAT(0.0f, d.weight());
    if (f == 100) TEST_ASSERT_TRUE(d.weight() > 0.95f);
    // Confidence jumping from 0 to 1 fades in: no more than 1/12 per frame.
    TEST_ASSERT_TRUE(std::fabs(d.weight() - before) < 1.0f / 12.0f);
  }
  TEST_ASSERT_TRUE(d.weight() < 0.25f);  // 1.5 s after the beat was lost
  // Frozen poses (screenshots) are the pure dance pose.
  const Pose f = d.frozen(0.25f, true);
  const Pose ref = dance::dancePose(0.25f, true);
  TEST_ASSERT_EQUAL_FLOAT(ref.handL.x, f.handL.x);
  // A skin switch hands the crab's weight over (clamped to 0..1).
  d.reset(0.8f);
  TEST_ASSERT_EQUAL_FLOAT(0.8f, d.weight());
  d.reset(-1.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.weight());
  d.reset();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, d.weight());
}

// The dance tempo is the tracked one folded into 80-160 BPM.
void test_dance_step_folds_the_tempo() {
  dance::Step s = dance::danceStep(10.25, 120.0f);
  TEST_ASSERT_EQUAL_INT32(10, s.index);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.25f, s.phi);
  TEST_ASSERT_FALSE(s.odd);
  TEST_ASSERT_EQUAL_FLOAT(120.0f, s.bpm);
  s = dance::danceStep(10.5, 174.0f);  // danced at 87: 5.25 dance beats
  TEST_ASSERT_EQUAL_INT32(5, s.index);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.25f, s.phi);
  TEST_ASSERT_TRUE(s.odd);
  TEST_ASSERT_EQUAL_FLOAT(87.0f, s.bpm);
  s = dance::danceStep(3.75, 70.0f);  // danced at 140: 7.5
  TEST_ASSERT_EQUAL_INT32(7, s.index);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, s.phi);
  TEST_ASSERT_EQUAL_FLOAT(140.0f, s.bpm);
  s = dance::danceStep(-0.25, 100.0f);  // before the grid's beat 0
  TEST_ASSERT_EQUAL_INT32(-1, s.index);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.75f, s.phi);
  TEST_ASSERT_TRUE(s.odd);
}

// A tempo wobbling across the fold's edge (the PLL moves it every beat) keeps
// one fold: phi advances smoothly and the parity flips only as phi wraps.
// Without the hysteresis, 159.9/160.1 would switch between dancing at 160
// and at 80 each beat, and phi would jump.
void test_tempo_fold_holds_at_the_edge() {
  const float edges[][2] = {{159.9f, 160.1f}, {79.9f, 80.1f}};
  for (const auto& e : edges) {
    dance::TempoFold fold;
    double beats = 0.0;
    dance::Step last;
    bool first = true;
    int wraps = 0;
    for (int f = 0; f < 30 * 30; ++f) {  // 30 s at 30 fps
      const float bpm = (static_cast<int>(beats) & 1) ? e[0] : e[1];
      beats += bpm / 60.0 / 30.0;
      const dance::Step s = dance::danceStep(beats, bpm, fold.apply(bpm));
      if (!first) {
        float step = s.phi - last.phi;
        if (step < 0.0f) {
          step += 1.0f;
          ++wraps;
          TEST_ASSERT_TRUE(s.odd != last.odd);
        } else {
          TEST_ASSERT_TRUE(s.odd == last.odd);
        }
        TEST_ASSERT_TRUE(step > 0.0f && step < 0.1f);  // one frame at <= 176 BPM: under 0.1 beat
      }
      last = s;
      first = false;
    }
    TEST_ASSERT_TRUE(wraps > 30);
  }
  // A real change of tempo does refold, into 80-160.
  dance::TempoFold fold;
  TEST_ASSERT_EQUAL_FLOAT(0.5f, static_cast<float>(fold.apply(160.1f)));
  TEST_ASSERT_EQUAL_FLOAT(0.5f, static_cast<float>(fold.apply(159.0f)));  // kept: 79.5 is inside 72-176
  TEST_ASSERT_EQUAL_FLOAT(1.0f, static_cast<float>(fold.apply(140.0f)));  // 70 left the band: fresh fold
  TEST_ASSERT_EQUAL_FLOAT(1.0f, static_cast<float>(fold.apply(170.0f)));  // kept
  TEST_ASSERT_EQUAL_FLOAT(0.5f, static_cast<float>(fold.apply(180.0f)));  // 180 left it
  fold.reset();
  TEST_ASSERT_EQUAL_FLOAT(1.0f, static_cast<float>(fold.apply(0.0f)));  // no tempo: no fold
  TEST_ASSERT_EQUAL_FLOAT(1.0f, static_cast<float>(fold.apply(120.0f)));
}

// ---- helpers ----

void test_rolling_stats() {
  RollingStats<8> r;
  RollingStats<8>::Summary s = r.summary(100, 50);
  TEST_ASSERT_EQUAL_UINT32(0, s.count);
  const float values[] = {1, -2, 3, -4, 5, -6, 7, -8, 9, -10};
  for (int i = 0; i < 10; ++i) r.add(values[i], static_cast<uint32_t>(i * 10));
  TEST_ASSERT_EQUAL_UINT32(8, r.size());  // the last 8: -3..(-10) by magnitude 3..10
  s = r.summary(90, 1000);
  TEST_ASSERT_EQUAL_UINT32(8, s.count);
  TEST_ASSERT_EQUAL_FLOAT(6.5f, s.medianAbs);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, s.p95Abs);
  TEST_ASSERT_EQUAL_FLOAT(-0.5f, s.mean);
  s = r.summary(90, 25);  // stamps 70, 80, 90: -8, 9, -10
  TEST_ASSERT_EQUAL_UINT32(3, s.count);
  TEST_ASSERT_EQUAL_FLOAT(9.0f, s.medianAbs);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, s.p95Abs);
  TEST_ASSERT_EQUAL_FLOAT(-3.0f, s.mean);
  r.clear();
  TEST_ASSERT_EQUAL_UINT32(0, r.size());
}

void test_base64() {
  const struct {
    const char* in;
    const char* out;
  } cases[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
               {"foobar", "Zm9vYmFy"}};
  for (const auto& c : cases) {
    char out[32];
    const size_t n = base64Encode(reinterpret_cast<const uint8_t*>(c.in), std::strlen(c.in), out);
    TEST_ASSERT_EQUAL_STRING(c.out, out);
    TEST_ASSERT_EQUAL_UINT32(std::strlen(c.out), n);
    TEST_ASSERT_EQUAL_UINT32(n, base64Length(std::strlen(c.in)));
  }
  const uint8_t bin[] = {0x00, 0xFF, 0x10, 0xFB};
  char out[16];
  base64Encode(bin, 4, out);
  TEST_ASSERT_EQUAL_STRING("AP8Q+w==", out);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_contact_on_the_beat);
  RUN_TEST(test_squash_and_stretch);
  RUN_TEST(test_head_bob_lags_the_beat);
  RUN_TEST(test_arms_alternate_with_extremes_on_the_beat);
  RUN_TEST(test_continuous_across_beats);
  RUN_TEST(test_bones_and_box);
  RUN_TEST(test_low_confidence_blends_to_idle);
  RUN_TEST(test_dance_step_folds_the_tempo);
  RUN_TEST(test_tempo_fold_holds_at_the_edge);
  RUN_TEST(test_rolling_stats);
  RUN_TEST(test_base64);
  return UNITY_END();
}
