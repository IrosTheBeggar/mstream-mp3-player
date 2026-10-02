// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for TrimFeed: gapless trimming in front of RingFeed
// (docs/GAPLESS.md section 4.4), through the real feed and PcmRing with a
// reader that takes random amounts (so the ring is often full and frames
// are refused). Run: pio test -e native -f test_trim_feed
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <random>
#include <vector>

#include "PcmRing.h"
#include "RateConverter.h"
#include "RingFeed.h"
#include "TrimFeed.h"

namespace {
using Frames = std::vector<int16_t>;

constexpr uint32_t kRingCap = 2048;
constexpr uint8_t kReader = 1;
constexpr uint32_t kPass = 1024;

int16_t gRingBuf[kRingCap * 2];
PcmRing gRing(gRingBuf, kRingCap);
RingFeed gFeed(gRing);
TrimFeed gTrim(gFeed);
int16_t gHold[TrimFeed::kMaxHold * 2];
RateConverter gRef;

Frames noise(size_t frames, uint32_t seed) {
  std::mt19937 rng(seed);
  Frames v(frames * 2);
  for (auto& s : v) s = static_cast<int16_t>(static_cast<int>(rng() % 60001) - 30000);
  return v;
}

Frames slice(const Frames& v, size_t from, size_t to) {
  return Frames(v.begin() + 2 * from, v.begin() + 2 * to);
}

Frames reference(uint32_t hz, const Frames& in) {
  RateConverter& c = gRef;
  c.reset();
  TEST_ASSERT_TRUE(c.setRate(hz, 240, true));
  Frames out;
  int16_t buf[RateConverter::kMaxOut * 2];
  for (size_t i = 0; i + 1 < in.size(); i += 2) {
    const uint32_t n = c.push(&in[i], buf);
    out.insert(out.end(), buf, buf + 2 * n);
  }
  while (!c.finished()) {
    const uint32_t n = c.finishPush(buf);
    out.insert(out.end(), buf, buf + 2 * n);
  }
  return out;
}

void drain(Frames& out, uint32_t n) {
  int16_t buf[512 * 2];
  while (n > 0) {
    const uint32_t got = gRing.read(kReader, buf, std::min<uint32_t>(n, 512));
    if (got == 0) return;
    out.insert(out.end(), buf, buf + 2 * got);
    n -= got;
  }
}

void assertSame(const Frames& want, const Frames& got) {
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  if (!want.empty()) TEST_ASSERT_EQUAL_INT16_ARRAY(want.data(), got.data(), want.size());
}

// A generator: `src` (at `hz`, said before the first frame, or after the
// first `rateAfter` frames as MP3's does, or changed to `hz2` before frame
// `changeAt`), handed over one frame at a time through the trim, a refused
// frame offered again on the next pass, the reader taking random amounts
// between passes. Then the end (natural or `early`) and the converter's
// tail; returns what the reader got.
struct Gen {
  uint32_t hz = 44100;
  uint32_t hz2 = 0;
  size_t changeAt = 0;
  bool early = false;
};

Frames run(const Frames& src, uint32_t skip, uint32_t hold, const Gen& g, uint32_t seed) {
  std::mt19937 rng(seed);
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.setHoldBuffer(gHold, TrimFeed::kMaxHold);
  gTrim.arm(skip, hold);
  gTrim.setChannels(2);
  gTrim.setRate(static_cast<int>(g.hz));
  Frames out;
  const size_t frames = src.size() / 2;
  size_t next = 0;
  bool changed = false;
  while (next < frames) {
    gFeed.setBudget(kPass);
    while (next < frames) {
      if (g.hz2 && next == g.changeAt && !changed) {  // said once, before the frame (MP3's GetOneSample())
        gTrim.setRate(static_cast<int>(g.hz2));
        changed = true;
      }
      if (!gTrim.consume(&src[2 * next])) break;
      ++next;
    }
    gFeed.commit();
    drain(out, rng() % 3 == 0 ? 0 : rng() % 700);
  }
  gFeed.setBudget(kPass);  // (the end's flush is a pass of its own)
  while (!gTrim.end(g.early)) {
    gFeed.commit();
    drain(out, rng() % 700 + 1);
    gFeed.setBudget(kPass);
  }
  while (!gFeed.finish()) drain(out, rng() % 700 + 1);
  drain(out, kRingCap);
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// Exactly the middle comes out: `skip` dropped at the start, `hold` at the
// end, bit for bit, whatever the reader does (refusals never lose or repeat
// a frame: the FIFO's oldest goes in before the newest is taken).
void test_exactly_the_kept_frames_come_out() {
  const uint32_t cases[][3] = {
      // frames, skip, hold
      {20000, 1, 0}, {20000, 1106, 779}, {20000, 2, 4095}, {20000, 0, 1}, {9000, 4000, 4095}, {5000, 5000, 0},
      {3000, 529, 3000},  // the whole rest held: nothing after the skip comes out
  };
  for (const auto& c : cases) {
    const Frames src = noise(c[0], c[0] + c[1] + c[2]);
    const size_t keptTo = c[0] > c[1] + c[2] ? c[0] - c[2] : c[1];
    const Frames want = slice(src, std::min<size_t>(c[1], c[0]), std::max<size_t>(keptTo, std::min<size_t>(c[1], c[0])));
    for (uint32_t seed = 0; seed < 3; ++seed) {
      Gen g;
      assertSame(want, run(src, c[1], c[2], g, seed * 31 + c[1]));
      TEST_ASSERT_EQUAL_UINT64(std::min<uint32_t>(c[1], c[0]), gTrim.skipped());
    }
  }
}

// At a converting rate the trim is at the source rate, before the
// converter: the ring gets the trimmed middle converted.
void test_the_trim_is_at_the_source_rate() {
  const Frames src = noise(12000, 4);
  Gen g;
  g.hz = 48000;
  assertSame(reference(48000, slice(src, 1105, 12000 - 700)), run(src, 1105, 700, g, 3));
  g.hz = 22050;
  assertSame(reference(22050, slice(src, 1, 12000 - 4095)), run(src, 1, 4095, g, 4));
}

// Nothing to trim: every frame goes straight to the feed (inactive).
void test_zero_skip_and_hold_is_a_passthrough() {
  const Frames src = noise(7000, 5);
  Gen g;
  gTrim.arm(0, 0);
  TEST_ASSERT_FALSE(gTrim.active());
  gTrim.arm(1, 0);
  TEST_ASSERT_TRUE(gTrim.active());
  assertSame(src, run(src, 0, 0, g, 6));
  TEST_ASSERT_FALSE(gTrim.active());
}

// A start after a seek (a resume point) skips only the generator's lead;
// the end hold still applies, so a resumed track joins its next one
// gaplessly too.
void test_a_seek_start_skips_only_the_lead_and_holds_the_end() {
  const Frames src = noise(15000, 7);
  Gen g;
  assertSame(slice(src, 1, 15000 - 779), run(src, 1, 779, g, 8));
  TEST_ASSERT_EQUAL_UINT64(1, gTrim.skipped());
  TEST_ASSERT_EQUAL_UINT64(779, gTrim.dropped());
}

// An early end (a decode error, the file cut short): the held frames are
// real audio and go in.
void test_an_early_end_flushes_the_hold() {
  const Frames src = noise(10000, 9);
  Gen g;
  g.early = true;
  assertSame(slice(src, 600, 10000), run(src, 600, 4000, g, 10));
  TEST_ASSERT_EQUAL_UINT64(0, gTrim.dropped());
}

// A rate change in the middle with frames held: the held frames go in at
// the old rate first, then the change; the end trim is off for the rest
// of the track (otherwise up to 4,095 old-rate frames would be converted
// at the new rate).
void test_a_rate_change_mid_track_releases_the_hold() {
  const Frames src = noise(16000, 11);
  Gen g;
  g.hz = 48000;
  g.hz2 = 32000;
  g.changeAt = 9000;
  // Frames [1105, 9000) at 48 kHz, then a new stream from 9000 at 32 kHz
  // (RingFeed restarts its filters at a rate change): the old one's frames
  // all came out first, with nothing of its tail (RateConverter drops the
  // history at a change: what it would do without the trim).
  const Frames got = run(src, 1105, 700, g, 12);
  RingFeed::Mark* m = new RingFeed::Mark;  // the same without the trim, by the feed alone
  gRing.discardAll();
  gFeed.reset(240, true);
  gFeed.setRate(48000);
  Frames want;
  std::mt19937 rng(1);
  for (size_t i = 1105; i < 16000; ++i) {
    if (i == 9000) gFeed.setRate(32000);
    gFeed.setBudget(1);
    while (!gFeed.consume(&src[2 * i])) {
      gFeed.commit();
      drain(want, 512);
      gFeed.setBudget(1);
    }
    gFeed.commit();
    drain(want, rng() % 50);
  }
  while (!gFeed.finish()) drain(want, 512);
  drain(want, kRingCap);
  assertSame(want, got);
  delete m;
}

// Said again while it waits (FLAC's generator says its rate per frame
// block): it still waits for the held frames.
void test_a_change_said_again_still_waits() {
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.setHoldBuffer(gHold, TrimFeed::kMaxHold);
  gTrim.arm(0, 10);
  gTrim.setRate(48000);
  gFeed.setBudget(kPass);
  const Frames src = noise(20, 14);
  for (int i = 0; i < 12; ++i) TEST_ASSERT_TRUE(gTrim.consume(&src[2 * i]));  // 2 in, 10 held
  gTrim.setRate(32000);
  gTrim.setRate(32000);
  TEST_ASSERT_EQUAL_INT(48000, gFeed.rate());  // the held frames first
  TEST_ASSERT_TRUE(gTrim.consume(&src[24]));
  TEST_ASSERT_EQUAL_INT(32000, gFeed.rate());
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.holding());
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.holdArmed());  // no end trim for the rest
}

// A format change while nothing is held passes straight through (the
// first setRate() of every track comes during its start skip).
void test_a_change_with_nothing_held_passes_through() {
  gFeed.reset(240, true);
  gTrim.arm(1000, 500);
  TEST_ASSERT_TRUE(gTrim.setRate(48000));
  TEST_ASSERT_EQUAL_INT(48000, gFeed.rate());
  TEST_ASSERT_FALSE(gTrim.setRate(37800));  // refused by the feed: said so
}

// The FIFO is clamped to the buffer it was given; none: no end trim.
void test_the_hold_is_clamped_to_its_buffer() {
  const Frames src = noise(4000, 13);
  static int16_t small[100 * 2];
  gTrim.setHoldBuffer(small, 100);
  gTrim.arm(0, 4095);
  TEST_ASSERT_EQUAL_UINT32(100, gTrim.holdArmed());
  gTrim.setHoldBuffer(nullptr, 0);
  gTrim.arm(0, 4095);
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.holdArmed());
  (void)src;
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_exactly_the_kept_frames_come_out);
  RUN_TEST(test_the_trim_is_at_the_source_rate);
  RUN_TEST(test_zero_skip_and_hold_is_a_passthrough);
  RUN_TEST(test_a_seek_start_skips_only_the_lead_and_holds_the_end);
  RUN_TEST(test_an_early_end_flushes_the_hold);
  RUN_TEST(test_a_rate_change_mid_track_releases_the_hold);
  RUN_TEST(test_a_change_said_again_still_waits);
  RUN_TEST(test_a_change_with_nothing_held_passes_through);
  RUN_TEST(test_the_hold_is_clamped_to_its_buffer);
  return UNITY_END();
}
