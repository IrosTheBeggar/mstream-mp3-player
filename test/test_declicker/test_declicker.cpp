// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for Declicker and DeclickReader. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "DeclickReader.h"
#include "Declicker.h"
#include "PcmRing.h"

namespace {
constexpr uint32_t kRamp = Declicker::kDefaultRampFrames;  // 64
constexpr uint8_t kBt = 1;
constexpr uint8_t kSpeaker = 2;

std::vector<int16_t> constant(uint32_t n, int16_t l, int16_t r) {
  std::vector<int16_t> v;
  for (uint32_t i = 0; i < n; ++i) {
    v.push_back(l);
    v.push_back(r);
  }
  return v;
}

// A varied signal, so bit-exactness isn't checked on a constant alone.
std::vector<int16_t> varied(uint32_t n, uint32_t seed = 1) {
  std::vector<int16_t> v;
  uint32_t x = seed;
  for (uint32_t i = 0; i < 2 * n; ++i) {
    x = x * 1103515245u + 12345u;
    v.push_back(static_cast<int16_t>(x >> 16));
  }
  return v;
}

// Largest jump between neighbouring frames on either channel.
int maxStep(const std::vector<int16_t>& v, int16_t prevL, int16_t prevR) {
  int worst = 0;
  int pl = prevL, pr = prevR;
  for (size_t i = 0; i + 1 < v.size(); i += 2) {
    worst = std::max(worst, std::abs(v[i] - pl));
    worst = std::max(worst, std::abs(v[i + 1] - pr));
    pl = v[i];
    pr = v[i + 1];
  }
  return worst;
}

// Brings a fresh Declicker to full level on a constant signal.
void warmUp(Declicker& d, int16_t l, int16_t r) {
  auto v = constant(kRamp * 2, l, r);
  d.process(v.data(), kRamp * 2, kRamp * 2);
  TEST_ASSERT_EQUAL_INT16(l, v[v.size() - 2]);
}

struct TestRing {
  std::vector<int16_t> buf;
  PcmRing ring;
  explicit TestRing(uint32_t cap) : buf(cap * 2), ring(buf.data(), cap) { ring.setConsumer(kBt); }
  void put(const std::vector<int16_t>& v) {
    TEST_ASSERT_EQUAL_UINT32(v.size() / 2, ring.write(v.data(), static_cast<uint32_t>(v.size() / 2)));
  }
};
}  // namespace

void setUp() {}
void tearDown() {}

// ---- Declicker ----

void test_starts_silent() {
  Declicker d;
  TEST_ASSERT_TRUE(d.silent());
  TEST_ASSERT_EQUAL_UINT32(0, d.decayFrames());
  auto v = constant(10, 1234, 1234);
  d.process(v.data(), 0, 10);  // nothing real: zeros
  for (int16_t s : v) TEST_ASSERT_EQUAL_INT16(0, s);
  TEST_ASSERT_EQUAL_UINT32(64, Declicker::decayFramesFor(64));
}

void test_fades_in_linearly_then_is_bit_exact() {
  Declicker d;
  auto in = varied(1000);
  auto v = constant(kRamp, 16384, -16384);
  d.process(v.data(), kRamp, kRamp);
  for (uint32_t k = 1; k <= kRamp; ++k) {  // frame k-1 is at gain k/64
    TEST_ASSERT_INT_WITHIN(1, 16384 * static_cast<int>(k) / 64, v[2 * (k - 1)]);
    TEST_ASSERT_INT_WITHIN(1, -16384 * static_cast<int>(k) / 64, v[2 * (k - 1) + 1]);
  }
  auto out = in;
  d.process(out.data(), 1000, 1000);
  TEST_ASSERT_EQUAL_INT16_ARRAY(in.data(), out.data(), in.size());
  TEST_ASSERT_FALSE(d.silent());
}

void test_gap_decays_from_the_last_frame() {
  Declicker d;
  warmUp(d, 8000, -8000);
  // 10 real frames, then the ring ran dry.
  auto v = constant(200, 8000, -8000);
  TEST_ASSERT_EQUAL_UINT32(64, d.decayFrames());
  d.process(v.data(), 10, 200);
  for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL_INT16(8000, v[2 * i]);
  TEST_ASSERT_EQUAL_INT16(7875, v[2 * 10]);  // 8000 * 63/64
  TEST_ASSERT_EQUAL_INT16(-7875, v[2 * 10 + 1]);
  TEST_ASSERT_EQUAL_INT16(0, v[2 * (10 + 63)]);
  for (uint32_t i = 10 + 63; i < 200; ++i) TEST_ASSERT_EQUAL_INT16(0, v[2 * i]);
  TEST_ASSERT_LESS_OR_EQUAL_INT(8000 / 64 + 1, maxStep(v, 8000, -8000));
  TEST_ASSERT_TRUE(d.silent());
}

void test_audio_after_a_gap_fades_in() {
  Declicker d;
  warmUp(d, 8000, 8000);
  auto gap = constant(100, 0, 0);
  d.process(gap.data(), 0, 100);
  auto v = constant(kRamp, 8000, 8000);
  d.process(v.data(), kRamp, kRamp);
  TEST_ASSERT_EQUAL_INT16(125, v[0]);  // 8000 / 64
  TEST_ASSERT_EQUAL_INT16(8000, v[2 * (kRamp - 1)]);
}

void test_pause_fades_real_audio_and_then_wants_none() {
  Declicker d;
  warmUp(d, 10000, 10000);
  d.setOpen(false);
  TEST_ASSERT_EQUAL_UINT32(64, d.framesWanted(1000));
  TEST_ASSERT_EQUAL_UINT32(10, d.framesWanted(10));
  auto v = constant(kRamp, 10000, 10000);
  d.process(v.data(), kRamp, kRamp);
  TEST_ASSERT_INT_WITHIN(1, 10000 * 63 / 64, v[0]);
  TEST_ASSERT_EQUAL_INT16(0, v[2 * (kRamp - 1)]);
  TEST_ASSERT_LESS_OR_EQUAL_INT(10000 / 64 + 1, maxStep(v, 10000, 10000));
  TEST_ASSERT_EQUAL_UINT32(0, d.framesWanted(1000));
  TEST_ASSERT_TRUE(d.silent());

  // Paused output: zeros, and still silent.
  auto quiet = constant(128, 5, 5);
  d.process(quiet.data(), 0, 128);
  for (int16_t s : quiet) TEST_ASSERT_EQUAL_INT16(0, s);

  d.setOpen(true);  // resume fades in
  TEST_ASSERT_EQUAL_UINT32(100, d.framesWanted(100));
  auto w = constant(kRamp, 10000, 10000);
  d.process(w.data(), kRamp, kRamp);
  TEST_ASSERT_INT_WITHIN(1, 10000 / 64, w[0]);
  TEST_ASSERT_EQUAL_INT16(10000, w[2 * (kRamp - 1)]);
}

void test_resume_mid_fade_turns_back_smoothly() {
  Declicker d;
  warmUp(d, 10000, 10000);
  d.setOpen(false);
  auto v = constant(20, 10000, 10000);
  d.process(v.data(), 20, 20);
  d.setOpen(true);
  auto w = constant(40, 10000, 10000);
  d.process(w.data(), 40, 40);
  TEST_ASSERT_LESS_OR_EQUAL_INT(10000 / 64 + 1, maxStep(w, v[38], v[39]));
  TEST_ASSERT_EQUAL_INT16(10000, w[2 * 39]);
}

void test_cut_crossfades_from_the_held_frame() {
  Declicker d;
  warmUp(d, 10000, -10000);
  d.cut();
  auto v = constant(2 * kRamp, -10000, 10000);  // the next track
  d.process(v.data(), 2 * kRamp, 2 * kRamp);
  TEST_ASSERT_INT_WITHIN(1, 10000 - 2 * 10000 / 64, v[0]);
  TEST_ASSERT_EQUAL_INT16(-10000, v[2 * (kRamp - 1)]);
  TEST_ASSERT_EQUAL_INT16(10000, v[2 * (kRamp - 1) + 1]);
  TEST_ASSERT_LESS_OR_EQUAL_INT(2 * 10000 / 64 + 1, maxStep(v, 10000, -10000));
  for (uint32_t i = kRamp; i < 2 * kRamp; ++i) TEST_ASSERT_EQUAL_INT16(-10000, v[2 * i]);
}

void test_crossfade_at_full_scale_never_wraps() {
  const int16_t extremes[] = {32767, -32768};
  for (int16_t held : extremes) {
    for (int16_t next : extremes) {
      Declicker d;
      warmUp(d, held, held);
      d.cut();
      auto v = constant(kRamp, next, next);
      d.process(v.data(), kRamp, kRamp);
      // A linear path between two extremes: no step bigger than the ramp's.
      TEST_ASSERT_LESS_OR_EQUAL_INT(65535 / 64 + 2, maxStep(v, held, held));
      TEST_ASSERT_EQUAL_INT16(next, v[2 * (kRamp - 1)]);
    }
  }
}

void test_zero_signal_break_is_already_silent() {
  Declicker d;
  warmUp(d, 0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, d.decayFrames());
  auto v = constant(10, 0, 0);
  d.process(v.data(), 0, 10);
  TEST_ASSERT_TRUE(d.silent());
}

void test_reset_forgets_everything() {
  Declicker d;
  warmUp(d, 9000, 9000);
  d.reset();
  TEST_ASSERT_TRUE(d.silent());
  auto v = constant(1, 9000, 9000);
  d.process(v.data(), 1, 1);
  TEST_ASSERT_INT_WITHIN(1, 9000 / 64, v[0]);  // fades in again
}

// Any split of the same real/missing sequence into calls gives the same output.
void test_output_does_not_depend_on_chunking() {
  struct Segment {
    uint32_t real, missing;
    bool open, cut;
  };
  const Segment plan[] = {{300, 0, true, false}, {17, 40, true, false}, {5, 200, true, false},
                          {90, 0, false, false}, {50, 0, true, false},  {70, 3, true, true},
                          {1, 1, true, false},   {128, 9, false, true}, {200, 100, true, false}};
  auto in = varied(2000, 7);
  std::vector<int16_t> whole, chunked;
  Declicker a, b;
  uint32_t at = 0;
  for (const Segment& s : plan) {
    const uint32_t n = s.real + s.missing;
    std::vector<int16_t> va(in.begin() + 2 * at, in.begin() + 2 * (at + n));
    std::vector<int16_t> vb = va;
    at += n;

    a.setOpen(s.open);
    if (s.cut) a.cut();
    a.process(va.data(), s.real, n);

    b.setOpen(s.open);
    if (s.cut) b.cut();
    uint32_t done = 0;
    for (uint32_t piece = 1; done < s.real; piece = piece * 3 % 37 + 1) {  // uneven pieces
      const uint32_t k = std::min(piece, s.real - done);
      b.process(vb.data() + 2 * done, k, k);
      done += k;
    }
    while (done < n) {
      const uint32_t k = std::min<uint32_t>(13, n - done);
      b.process(vb.data() + 2 * done, 0, k);
      done += k;
    }
    whole.insert(whole.end(), va.begin(), va.end());
    chunked.insert(chunked.end(), vb.begin(), vb.end());
  }
  TEST_ASSERT_EQUAL_INT16_ARRAY(whole.data(), chunked.data(), whole.size());
}

// ---- DeclickReader ----

void test_reader_fill_fades_in_and_decays_across_calls() {
  TestRing t(1024);
  t.put(constant(100, 10000, 10000));
  DeclickReader reader(kBt);
  reader.bind(t.ring);

  std::vector<int16_t> out(128 * 2);
  auto r = reader.fill(out.data(), 128, true);
  TEST_ASSERT_EQUAL_UINT32(128, r.wanted);
  TEST_ASSERT_EQUAL_UINT32(100, r.read);
  TEST_ASSERT_EQUAL_UINT32(128, r.total);
  TEST_ASSERT_INT_WITHIN(1, 10000 / 64, out[0]);  // a new output fades in
  TEST_ASSERT_EQUAL_INT16(10000, out[2 * 99]);
  TEST_ASSERT_LESS_THAN_INT16(10000, out[2 * 100]);  // then decays
  TEST_ASSERT_GREATER_THAN_INT16(0, out[2 * 127]);   // ...into the next call

  r = reader.fill(out.data(), 128, true);
  TEST_ASSERT_EQUAL_UINT32(0, r.read);
  TEST_ASSERT_GREATER_THAN_INT16(0, out[0]);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * 127]);
  TEST_ASSERT_TRUE(reader.silent());
}

void test_reader_only_reads_as_the_consumer() {
  TestRing t(1024);
  t.put(constant(300, 10000, 10000));
  DeclickReader reader(kSpeaker);
  reader.bind(t.ring);
  std::vector<int16_t> out(128 * 2, 77);
  const auto r = reader.fill(out.data(), 128, true);
  TEST_ASSERT_EQUAL_UINT32(0, r.wanted);
  TEST_ASSERT_EQUAL_UINT32(0, r.read);
  TEST_ASSERT_EQUAL_UINT32(300, t.ring.size());  // untouched
  for (int16_t s : out) TEST_ASSERT_EQUAL_INT16(0, s);
}

void test_reader_pause_takes_only_the_fade_from_the_ring() {
  TestRing t(1024);
  t.put(constant(1000, 10000, 10000));
  DeclickReader reader(kBt);
  reader.bind(t.ring);
  std::vector<int16_t> out(128 * 2);
  reader.fill(out.data(), 128, true);
  TEST_ASSERT_EQUAL_UINT32(872, t.ring.size());

  auto r = reader.fill(out.data(), 128, false);  // paused
  TEST_ASSERT_EQUAL_UINT32(64, r.wanted);
  TEST_ASSERT_EQUAL_UINT32(64, r.read);
  TEST_ASSERT_EQUAL_UINT32(808, t.ring.size());
  TEST_ASSERT_EQUAL_INT16(0, out[2 * 63]);
  for (uint32_t i = 64; i < 128; ++i) TEST_ASSERT_EQUAL_INT16(0, out[2 * i]);
  TEST_ASSERT_TRUE(reader.silent());

  r = reader.fill(out.data(), 128, false);
  TEST_ASSERT_EQUAL_UINT32(0, r.wanted);
  TEST_ASSERT_EQUAL_UINT32(808, t.ring.size());

  r = reader.fill(out.data(), 128, true);  // resume: carries on where it stopped
  TEST_ASSERT_EQUAL_UINT32(128, r.read);
  TEST_ASSERT_INT_WITHIN(1, 10000 / 64, out[0]);
}

void test_reader_turns_a_skip_into_a_crossfade() {
  TestRing t(1024);
  t.put(constant(500, 10000, 10000));
  DeclickReader reader(kBt);
  reader.bind(t.ring);
  std::vector<int16_t> out(128 * 2);
  reader.fill(out.data(), 128, true);
  TEST_ASSERT_EQUAL_INT16(10000, out[2 * 127]);

  t.ring.discardAll();  // skip: the ring refills at once with the next track
  t.put(constant(500, -10000, -10000));
  reader.fill(out.data(), 128, true);
  TEST_ASSERT_GREATER_THAN_INT16(9000, out[0]);  // no jump to -10000
  TEST_ASSERT_LESS_OR_EQUAL_INT(2 * 10000 / 64 + 1, maxStep(out, 10000, 10000));
  TEST_ASSERT_EQUAL_INT16(-10000, out[2 * 63]);
  TEST_ASSERT_EQUAL_INT16(-10000, out[2 * 127]);
}

void test_reader_pull_ends_what_it_queues_at_zero() {
  TestRing t(4096);
  t.put(constant(1100, 10000, 10000));
  DeclickReader reader(kSpeaker);
  reader.bind(t.ring);
  t.ring.setConsumer(kSpeaker);
  std::vector<int16_t> out((1024 + Declicker::decayFramesFor(kRamp)) * 2);

  auto r = reader.pull(out.data(), 1024, true);  // a full buffer: no decay added
  TEST_ASSERT_EQUAL_UINT32(1024, r.read);
  TEST_ASSERT_EQUAL_UINT32(1024, r.total);

  r = reader.pull(out.data(), 1024, true);  // the ring ran short
  TEST_ASSERT_EQUAL_UINT32(76, r.read);
  TEST_ASSERT_EQUAL_UINT32(76 + 64, r.total);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * (r.total - 1)]);
  TEST_ASSERT_TRUE(reader.silent());

  r = reader.pull(out.data(), 1024, true);  // nothing at all: nothing to send
  TEST_ASSERT_EQUAL_UINT32(0, r.total);
}

void test_reader_pull_fades_out_when_the_ring_moves_to_another_output() {
  TestRing t(4096);
  t.put(constant(3000, 10000, 10000));
  DeclickReader reader(kSpeaker);
  reader.bind(t.ring);
  t.ring.setConsumer(kSpeaker);
  std::vector<int16_t> out((1024 + 64) * 2);
  reader.pull(out.data(), 1024, true);

  t.ring.setConsumer(kBt);  // output switched to Bluetooth
  auto r = reader.pull(out.data(), 1024, true);
  TEST_ASSERT_EQUAL_UINT32(0, r.wanted);
  TEST_ASSERT_EQUAL_UINT32(64, r.total);  // just the decay of the last frame
  TEST_ASSERT_LESS_THAN_INT16(10000, out[0]);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * 63]);
  TEST_ASSERT_TRUE(reader.silent());
  TEST_ASSERT_EQUAL_UINT32(0, reader.pull(out.data(), 1024, true).total);
}

void test_reader_pull_pause_is_one_short_fade_buffer() {
  TestRing t(4096);
  t.put(constant(3000, 10000, 10000));
  DeclickReader reader(kSpeaker);
  reader.bind(t.ring);
  t.ring.setConsumer(kSpeaker);
  std::vector<int16_t> out((1024 + 64) * 2);
  reader.pull(out.data(), 1024, true);

  auto r = reader.pull(out.data(), 1024, false);
  TEST_ASSERT_EQUAL_UINT32(64, r.read);
  TEST_ASSERT_EQUAL_UINT32(64, r.total);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * 63]);
  TEST_ASSERT_TRUE(reader.silent());
  TEST_ASSERT_EQUAL_UINT32(0, reader.pull(out.data(), 1024, false).total);
}

// Whatever state it's in, a pull that came up short leaves the reader silent,
// so the speaker pump can go idle instead of spinning.
void test_reader_pull_always_settles() {
  TestRing t(4096);
  t.ring.setConsumer(kSpeaker);
  t.put(constant(1024, 0, 0));  // digital silence: live, full gain, last frame 0
  DeclickReader reader(kSpeaker);
  reader.bind(t.ring);
  std::vector<int16_t> out((1024 + 64) * 2);
  TEST_ASSERT_EQUAL_UINT32(1024, reader.pull(out.data(), 1024, true).total);
  TEST_ASSERT_FALSE(reader.silent());

  const auto r = reader.pull(out.data(), 1024, false);  // paused with the ring empty
  TEST_ASSERT_EQUAL_UINT32(0, r.total);
  TEST_ASSERT_TRUE(reader.silent());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_starts_silent);
  RUN_TEST(test_fades_in_linearly_then_is_bit_exact);
  RUN_TEST(test_gap_decays_from_the_last_frame);
  RUN_TEST(test_audio_after_a_gap_fades_in);
  RUN_TEST(test_pause_fades_real_audio_and_then_wants_none);
  RUN_TEST(test_resume_mid_fade_turns_back_smoothly);
  RUN_TEST(test_cut_crossfades_from_the_held_frame);
  RUN_TEST(test_crossfade_at_full_scale_never_wraps);
  RUN_TEST(test_zero_signal_break_is_already_silent);
  RUN_TEST(test_reset_forgets_everything);
  RUN_TEST(test_output_does_not_depend_on_chunking);
  RUN_TEST(test_reader_fill_fades_in_and_decays_across_calls);
  RUN_TEST(test_reader_only_reads_as_the_consumer);
  RUN_TEST(test_reader_pause_takes_only_the_fade_from_the_ring);
  RUN_TEST(test_reader_turns_a_skip_into_a_crossfade);
  RUN_TEST(test_reader_pull_ends_what_it_queues_at_zero);
  RUN_TEST(test_reader_pull_fades_out_when_the_ring_moves_to_another_output);
  RUN_TEST(test_reader_pull_pause_is_one_short_fade_buffer);
  RUN_TEST(test_reader_pull_always_settles);
  return UNITY_END();
}
