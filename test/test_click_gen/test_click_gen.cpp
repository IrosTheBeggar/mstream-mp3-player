// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for ClickGen, the click tracks with a known beat grid.
// Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ClickGen.h"

void setUp() {}
void tearDown() {}

void test_parse_names() {
  ClickGen::Spec s;
  TEST_ASSERT_TRUE(ClickGen::parse("click120", &s));
  TEST_ASSERT_EQUAL_FLOAT(120.0f, s.bpm);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, s.offsetBeats);
  TEST_ASSERT_TRUE(ClickGen::parse("click120off", &s));
  TEST_ASSERT_EQUAL_FLOAT(120.0f, s.bpm);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.37f, s.offsetBeats);
  TEST_ASSERT_TRUE(ClickGen::parse("click174", &s));
  TEST_ASSERT_EQUAL_FLOAT(174.0f, s.bpm);
  TEST_ASSERT_FALSE(ClickGen::parse("440", &s));
  TEST_ASSERT_FALSE(ClickGen::parse("click", &s));
  TEST_ASSERT_FALSE(ClickGen::parse("clickoff", &s));
  TEST_ASSERT_FALSE(ClickGen::parse("click12x", &s));
  TEST_ASSERT_FALSE(ClickGen::parse("click5", &s));
}

// Beat k at exactly offset + round(k * 60 / bpm * rate).
void test_beat_frames_are_exact() {
  ClickGen g;
  ClickGen::Spec s;
  ClickGen::parse("click128", &s);
  g.start(44100, s, 44100 * 60);
  TEST_ASSERT_EQUAL_UINT32(0, g.beatFrame(0));
  TEST_ASSERT_EQUAL_UINT32(20672, g.beatFrame(1));   // 20671.875
  TEST_ASSERT_EQUAL_UINT32(41344, g.beatFrame(2));   // 41343.75
  TEST_ASSERT_EQUAL_UINT32(2646000, g.beatFrame(128));
  ClickGen::parse("click120off", &s);
  g.start(44100, s, 44100 * 60);
  TEST_ASSERT_EQUAL_UINT32(8159, g.beatFrame(0));    // 0.37 * 22050 = 8158.5
  TEST_ASSERT_EQUAL_UINT32(8159 + 22050, g.beatFrame(1));
  TEST_ASSERT_EQUAL_INT32(0, g.offsetFromNearestBeat(8159));
  TEST_ASSERT_EQUAL_INT32(-100, g.offsetFromNearestBeat(8059));
  TEST_ASSERT_EQUAL_INT32(11000, g.offsetFromNearestBeat(8159 + 11000));
  TEST_ASSERT_EQUAL_INT32(-11000, g.offsetFromNearestBeat(8159 + 22050 - 11000));
}

// Each click starts at its beat frame (silence before it), is ~15 ms long,
// and peaks at about -12 dBFS on the downbeat, ~4 dB lower on the others.
void test_clicks_start_on_their_beats() {
  ClickGen g;
  ClickGen::Spec s;
  ClickGen::parse("click140off", &s);
  const uint32_t n = 44100 * 4;
  g.start(44100, s, n);
  std::vector<int16_t> out(2 * n);
  // In odd chunks: the output doesn't depend on them.
  uint32_t at = 0;
  while (at < n) at += g.generate(out.data() + 2 * at, std::min<uint32_t>(999, n - at));
  TEST_ASSERT_TRUE(g.done());
  TEST_ASSERT_EQUAL_UINT32(0, g.generate(out.data(), 10));
  for (uint32_t k = 0; g.beatFrame(k) + 700 < n; ++k) {
    const uint32_t b = g.beatFrame(k);
    int peak = 0;
    for (uint32_t i = b; i < b + 700; ++i) peak = std::max(peak, std::abs(static_cast<int>(out[2 * i])));
    TEST_ASSERT_EQUAL_INT16(out[2 * b], out[2 * b + 1]);  // both channels
    if (b > 0) TEST_ASSERT_EQUAL_INT16(0, out[2 * (b - 1)]);
    const double db = 20.0 * std::log10(peak / 32768.0);
    if (k % 4 == 0) {
      TEST_ASSERT_TRUE(db > -14.0 && db < -11.0);
    } else {
      TEST_ASSERT_TRUE(db > -18.0 && db < -15.0);
    }
    // ~15 ms long: silent from 16 ms after the beat.
    for (uint32_t i = b + 706; i < b + 1000 && i < n; ++i) TEST_ASSERT_EQUAL_INT16(0, out[2 * i]);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_names);
  RUN_TEST(test_beat_frames_are_exact);
  RUN_TEST(test_clicks_start_on_their_beats);
  return UNITY_END();
}
