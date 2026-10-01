// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for ToneGen. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ToneGen.h"

void setUp() {}
void tearDown() {}

void test_chunked_output_matches_one_call() {
  ToneGen whole, chunked;
  whole.start(44100, 1000.0f, -18.0f, ToneGen::Channels::Both, 3000);
  chunked.start(44100, 1000.0f, -18.0f, ToneGen::Channels::Both, 3000);

  std::vector<int16_t> a(3000 * 2), b(3000 * 2);
  TEST_ASSERT_EQUAL_UINT32(3000, whole.generate(a.data(), 3000));
  uint32_t at = 0;
  for (uint32_t n : {1u, 7u, 512u, 333u, 2147u}) {
    at += chunked.generate(b.data() + at * 2, n);
  }
  TEST_ASSERT_EQUAL_UINT32(3000, at);
  TEST_ASSERT_EQUAL_INT16_ARRAY(a.data(), b.data(), a.size());
}

void test_left_only_silences_right() {
  ToneGen t;
  t.start(44100, 440.0f, -18.0f, ToneGen::Channels::LeftOnly, 1000);
  std::vector<int16_t> out(1000 * 2);
  t.generate(out.data(), 1000);
  bool leftHasSignal = false;
  for (int i = 0; i < 1000; ++i) {
    TEST_ASSERT_EQUAL_INT16(0, out[2 * i + 1]);
    if (out[2 * i] != 0) leftHasSignal = true;
  }
  TEST_ASSERT_TRUE(leftHasSignal);
}

void test_stops_after_duration() {
  ToneGen t;
  t.start(44100, 440.0f, -18.0f, ToneGen::Channels::Both, 100);
  std::vector<int16_t> out(256 * 2);
  TEST_ASSERT_EQUAL_UINT32(100, t.generate(out.data(), 256));
  TEST_ASSERT_TRUE(t.done());
  TEST_ASSERT_EQUAL_UINT32(0, t.generate(out.data(), 256));
}

void test_peak_level_matches_dbfs() {
  ToneGen t;
  t.start(44100, 1000.0f, -18.0f, ToneGen::Channels::Both, 44100);
  std::vector<int16_t> out(44100 * 2);
  t.generate(out.data(), 44100);
  int peak = 0;
  for (int16_t s : out) peak = std::max(peak, std::abs(static_cast<int>(s)));
  // -18 dBFS of full scale is ~4125.
  TEST_ASSERT_INT_WITHIN(5, 4125, peak);
}

void test_fades_in_and_out_over_5_ms() {
  ToneGen t;
  constexpr uint32_t kFrames = 44100 / 10;  // 100 ms
  constexpr uint32_t kEdge = 220;            // 5 ms at 44.1 kHz
  t.start(44100, 1000.0f, -18.0f, ToneGen::Channels::Both, kFrames);
  std::vector<int16_t> out(kFrames * 2);
  TEST_ASSERT_EQUAL_UINT32(kFrames, t.generate(out.data(), kFrames));

  TEST_ASSERT_EQUAL_INT16(0, out[0]);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * (kFrames - 1)]);
  auto peak = [&out](uint32_t from, uint32_t to) {
    int p = 0;
    for (uint32_t i = from; i < to; ++i) p = std::max(p, std::abs(static_cast<int>(out[2 * i])));
    return p;
  };
  // First and last 1 ms: at most 44/220 of full level (~825 of 4125).
  TEST_ASSERT_LESS_OR_EQUAL_INT(4125 * 45 / kEdge, peak(0, 44));
  TEST_ASSERT_LESS_OR_EQUAL_INT(4125 * 45 / kEdge, peak(kFrames - 44, kFrames));
  TEST_ASSERT_INT_WITHIN(5, 4125, peak(kEdge, kFrames - kEdge));  // full level in between
}

void test_very_short_tone_is_all_ramp() {
  ToneGen t;
  t.start(44100, 1000.0f, 0.0f, ToneGen::Channels::Both, 50);
  std::vector<int16_t> out(50 * 2);
  TEST_ASSERT_EQUAL_UINT32(50, t.generate(out.data(), 50));
  for (int16_t s : out) TEST_ASSERT_LESS_OR_EQUAL_INT(32767 * 25 / 220 + 1, std::abs(static_cast<int>(s)));
  TEST_ASSERT_EQUAL_INT16(0, out[2 * 49]);
}

void test_silence_is_zeros_for_its_duration() {
  ToneGen t;
  t.startSilence(44100, 3000);
  std::vector<int16_t> out(4096 * 2, 123);
  TEST_ASSERT_EQUAL_UINT32(2048, t.generate(out.data(), 2048));
  for (int i = 0; i < 2048 * 2; ++i) TEST_ASSERT_EQUAL_INT16(0, out[i]);
  TEST_ASSERT_FALSE(t.done());
  std::fill(out.begin(), out.end(), static_cast<int16_t>(123));
  TEST_ASSERT_EQUAL_UINT32(952, t.generate(out.data(), 2048));
  TEST_ASSERT_EQUAL_INT16(0, out[951 * 2 + 1]);
  TEST_ASSERT_EQUAL_INT16(123, out[952 * 2]);  // nothing written past the end
  TEST_ASSERT_TRUE(t.done());
  TEST_ASSERT_EQUAL_UINT32(0, t.generate(out.data(), 16));
  // A tone started afterwards is a tone again.
  t.start(44100, 1000.0f, -18.0f, ToneGen::Channels::Both, 1000);
  t.generate(out.data(), 1000);
  bool signal = false;
  for (int i = 0; i < 2000; ++i) signal = signal || out[i] != 0;
  TEST_ASSERT_TRUE(signal);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_chunked_output_matches_one_call);
  RUN_TEST(test_left_only_silences_right);
  RUN_TEST(test_stops_after_duration);
  RUN_TEST(test_peak_level_matches_dbfs);
  RUN_TEST(test_fades_in_and_out_over_5_ms);
  RUN_TEST(test_very_short_tone_is_all_ramp);
  RUN_TEST(test_silence_is_zeros_for_its_duration);
  return UNITY_END();
}
