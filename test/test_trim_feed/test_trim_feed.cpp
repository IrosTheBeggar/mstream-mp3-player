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
#include "FrameCursor.h"
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
  size_t rateAfter = 0;  // > 0: the rate (and channels) said only before this frame, as MP3's generator does
};

Frames run(const Frames& src, uint32_t skip, uint32_t hold, const Gen& g, uint32_t seed) {
  std::mt19937 rng(seed);
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.setHoldBuffer(gHold, TrimFeed::kMaxHold);
  gTrim.arm(skip, hold);
  gTrim.setChannels(2);
  if (g.rateAfter == 0) gTrim.setRate(static_cast<int>(g.hz));
  Frames out;
  const size_t frames = src.size() / 2;
  size_t next = 0;
  bool changed = false;
  bool said = false;
  while (next < frames) {
    gFeed.setBudget(kPass);
    while (next < frames) {
      if (g.rateAfter && next == g.rateAfter && !said) {  // MP3's first word, after its first frame
        gTrim.setRate(static_cast<int>(g.hz));
        gTrim.setChannels(2);
        said = true;
      }
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

// The same as MP3's generator does it: its rate and channels said only
// after its first frame, when a seek start's one-frame skip is already
// behind it and a frame is held. That first word is the format of the
// frames before it, not a change: the end hold stays (it used to be turned
// off, so every resumed or seeked MP3 joined its next with the padding).
void test_a_seek_start_holds_the_end_when_the_rate_comes_after_a_frame() {
  const Frames src = noise(15000, 15);
  Gen g;
  g.rateAfter = 2;
  assertSame(slice(src, 1, 15000 - 779), run(src, 1, 779, g, 16));
  TEST_ASSERT_EQUAL_UINT64(1, gTrim.skipped());
  TEST_ASSERT_EQUAL_UINT64(779, gTrim.dropped());
  // From the top the word comes inside the start skip: the same.
  g.rateAfter = 600;
  assertSame(slice(src, 1105, 15000 - 779), run(src, 1105, 779, g, 17));
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

// ---- a planned start: the landing phase (docs/SEEK.md section 4.2) ----

namespace {

// The cursor a scripted generator moves: the frame of the sample it offers.
class ScriptCursor : public FrameCursor {
public:
  bool at(uint32_t* frameByte, uint32_t* sampleInFrame) const override {
    if (!valid) return false;
    *frameByte = byte;
    *sampleInFrame = index;
    return true;
  }
  bool valid = false;
  uint32_t byte = 0;
  uint32_t index = 0;
};

// The frames a decoder outputs, in order (a lost one is simply not there).
struct Seg {
  uint32_t byte;
  uint32_t n;
};

constexpr uint32_t kSpf = 1152;

// Frames of kSpf samples at bytes 1000, 1400, ... (`count` of them).
std::vector<Seg> frames(uint32_t count, uint32_t first = 1000, uint32_t step = 400) {
  std::vector<Seg> v;
  for (uint32_t i = 0; i < count; ++i) v.push_back({first + i * step, kSpf});
  return v;
}

// The generator's lead {0,0} (no frame yet), then every frame's samples
// (`src`, in order), its rate said before the first frame's first sample
// (MP3's first word), through the trim armed by armAt(); refused samples
// offered again; the end natural. What the reader got.
Frames runLanding(const std::vector<Seg>& segs, const Frames& src, uint32_t landByte, uint32_t skip,
                  uint32_t hold, uint32_t seed, ScriptCursor& cur) {
  std::mt19937 rng(seed);
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.setHoldBuffer(gHold, TrimFeed::kMaxHold);
  gTrim.armAt(&cur, landByte, 400, kSpf, skip, hold);
  gTrim.setChannels(2);  // (begin()'s SetChannels(2))
  // Where each offered sample comes from: -1 the lead.
  std::vector<std::pair<int, uint32_t>> order;
  order.push_back({-1, 0});
  for (size_t s = 0; s < segs.size(); ++s) {
    for (uint32_t i = 0; i < segs[s].n; ++i) order.push_back({static_cast<int>(s), i});
  }
  Frames out;
  size_t next = 0, at = 0;  // offered, and the sample index into src
  bool said = false;
  const int16_t lead[2] = {0, 0};
  while (next < order.size()) {
    gFeed.setBudget(kPass);
    while (next < order.size()) {
      const auto& o = order[next];
      cur.valid = o.first >= 0;
      if (cur.valid) {
        cur.byte = segs[static_cast<size_t>(o.first)].byte;
        cur.index = o.second;
        if (!said) {
          gTrim.setRate(44100);
          gTrim.setChannels(2);
          said = true;
        }
      }
      if (!gTrim.consume(cur.valid ? &src[2 * at] : lead)) break;
      if (cur.valid) ++at;
      ++next;
    }
    gFeed.commit();
    drain(out, rng() % 3 == 0 ? 0 : rng() % 700);
  }
  gFeed.setBudget(kPass);
  while (!gTrim.end(false)) {
    gFeed.commit();
    drain(out, rng() % 700 + 1);
    gFeed.setBudget(kPass);
  }
  while (!gFeed.finish()) drain(out, rng() % 700 + 1);
  drain(out, kRingCap);
  return out;
}

}  // namespace

// The lead and the preroll frames are dropped; the landing frame's sample
// `skip` is the first kept; the end held as any start's (the padding
// dropped); kept() counts every sample taken after the skip.
void test_a_planned_start_lands_on_its_frame() {
  const std::vector<Seg> segs = frames(12);  // 3 preroll frames, the landing frame at 2200, 8 after
  const Frames src = noise(12 * kSpf, 21);
  for (uint32_t seed = 0; seed < 4; ++seed) {
    ScriptCursor cur;
    const Frames got = runLanding(segs, src, 2200, 517, 779, seed, cur);
    assertSame(slice(src, 3 * kSpf + 517, 12 * kSpf - 779), got);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Exact), static_cast<int>(gTrim.landing()));
    TEST_ASSERT_EQUAL_UINT32(0, gTrim.lateBy());
    TEST_ASSERT_EQUAL_UINT64(1 + 3 * kSpf + 517, gTrim.skipped());  // the lead, the preroll, the skip
    TEST_ASSERT_EQUAL_UINT64(779, gTrim.dropped());
    TEST_ASSERT_EQUAL_UINT32(12 * kSpf - 3 * kSpf - 517, gTrim.kept());
  }
}

// A skip past the landing frame (the run index's entries are every 4th
// frame): it runs on into the frames after it.
void test_a_skip_past_the_landing_frame() {
  const std::vector<Seg> segs = frames(12);
  const Frames src = noise(12 * kSpf, 22);
  ScriptCursor cur;
  const Frames got = runLanding(segs, src, 2200, 3 * kSpf + 100, 0, 5, cur);
  assertSame(slice(src, 6 * kSpf + 100, 12 * kSpf), got);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Exact), static_cast<int>(gTrim.landing()));
}

// The landing frame lost (bad data, a reservoir the preroll didn't
// cover): the cursor says the frame after it. A skip under a frame: the
// start lands there, lateBy() later; a skip of a frame or more: still
// exactly there.
void test_a_lost_landing_frame_lands_on_the_next() {
  std::vector<Seg> segs = frames(12);
  segs.erase(segs.begin() + 3);  // 2200 never comes out
  const Frames src = noise(11 * kSpf, 23);
  ScriptCursor cur;
  Frames got = runLanding(segs, src, 2200, 517, 0, 6, cur);
  assertSame(slice(src, 3 * kSpf, 11 * kSpf), got);  // from the next frame's first sample
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::NextFrame), static_cast<int>(gTrim.landing()));
  TEST_ASSERT_EQUAL_UINT32(kSpf - 517, gTrim.lateBy());
  got = runLanding(segs, src, 2200, kSpf + 40, 0, 7, cur);
  assertSame(slice(src, 3 * kSpf + 40, 11 * kSpf), got);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::NextFrame), static_cast<int>(gTrim.landing()));
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.lateBy());
}

// A resync elsewhere (junk, the library's offset quirk): it lands where
// the cursor is, inexact, nothing skipped.
void test_a_resync_elsewhere_lands_inexact() {
  std::vector<Seg> segs = frames(12);
  segs.erase(segs.begin() + 3, segs.begin() + 5);  // 2200 and 2600 lost: 3000 next
  const Frames src = noise(10 * kSpf, 24);
  ScriptCursor cur;
  const Frames got = runLanding(segs, src, 2200, 517, 0, 8, cur);
  assertSame(slice(src, 3 * kSpf, 10 * kSpf), got);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Elsewhere), static_cast<int>(gTrim.landing()));
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.lateBy());
}

// A cursor that never says a frame (the lead, nothing decoded): everything
// is dropped, nothing lands.
void test_no_frame_yet_drops() {
  gRing.discardAll();
  gFeed.reset(240, true);
  ScriptCursor cur;
  gTrim.armAt(&cur, 2200, 400, kSpf, 0, 0);
  gFeed.setBudget(kPass);
  const int16_t s[2] = {5, 5};
  for (int i = 0; i < 5000; ++i) TEST_ASSERT_TRUE(gTrim.consume(s));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Waiting), static_cast<int>(gTrim.landing()));
  TEST_ASSERT_EQUAL_UINT32(0, gTrim.kept());
  TEST_ASSERT_FALSE(gTrim.landed());
  TEST_ASSERT_EQUAL_UINT32(0, gRing.size());
  TEST_ASSERT_TRUE(gTrim.active());
}

// The generator's first word comes with the first preroll frame, while
// nothing is held: it goes to the feed, and the end hold stays on, so a
// resumed track's end drops its padding (the "Gapless: device fixes" bug,
// through the landing).
void test_the_first_word_during_the_landing_keeps_the_end_hold() {
  const std::vector<Seg> segs = frames(10);
  const Frames src = noise(10 * kSpf, 25);
  ScriptCursor cur;
  const Frames got = runLanding(segs, src, 1800, 0, 1200, 9, cur);
  assertSame(slice(src, 2 * kSpf, 10 * kSpf - 1200), got);
  TEST_ASSERT_EQUAL_UINT64(1200, gTrim.dropped());
  TEST_ASSERT_EQUAL_INT(44100, gFeed.rate());
}

// kept() counts in the inactive path too (nothing to trim), and after the
// landing with nothing held the trim is as cheap as after arm(0, 0).
void test_kept_counts_with_nothing_to_trim() {
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.arm(0, 0);
  gTrim.setRate(44100);
  TEST_ASSERT_FALSE(gTrim.active());
  gFeed.setBudget(kPass);
  const Frames src = noise(600, 26);
  for (int i = 0; i < 600; ++i) TEST_ASSERT_TRUE(gTrim.consume(&src[2 * i]));
  TEST_ASSERT_EQUAL_UINT32(600, gTrim.kept());
  TEST_ASSERT_TRUE(gTrim.landed());
  // A start skip of 5: kept from after it.
  gTrim.arm(5, 0);
  gFeed.setBudget(kPass);
  for (int i = 0; i < 300; ++i) TEST_ASSERT_TRUE(gTrim.consume(&src[2 * i]));
  TEST_ASSERT_EQUAL_UINT32(295, gTrim.kept());
  TEST_ASSERT_FALSE(gTrim.active());
  // A landing with no hold: inactive once landed and skipped.
  ScriptCursor cur;
  cur.valid = true;
  cur.byte = 2200;
  cur.index = 0;
  gTrim.armAt(&cur, 2200, 400, kSpf, 3, 0);
  TEST_ASSERT_TRUE(gTrim.active());
  gFeed.setBudget(kPass);
  for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(gTrim.consume(&src[2 * i]));
  TEST_ASSERT_FALSE(gTrim.active());
  TEST_ASSERT_EQUAL_UINT32(7, gTrim.kept());
  // A refused sample isn't counted.
  gFeed.setBudget(0);
  TEST_ASSERT_FALSE(gTrim.consume(&src[0]));
  TEST_ASSERT_EQUAL_UINT32(7, gTrim.kept());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_exactly_the_kept_frames_come_out);
  RUN_TEST(test_the_trim_is_at_the_source_rate);
  RUN_TEST(test_zero_skip_and_hold_is_a_passthrough);
  RUN_TEST(test_a_seek_start_skips_only_the_lead_and_holds_the_end);
  RUN_TEST(test_a_seek_start_holds_the_end_when_the_rate_comes_after_a_frame);
  RUN_TEST(test_an_early_end_flushes_the_hold);
  RUN_TEST(test_a_rate_change_mid_track_releases_the_hold);
  RUN_TEST(test_a_change_said_again_still_waits);
  RUN_TEST(test_a_change_with_nothing_held_passes_through);
  RUN_TEST(test_the_hold_is_clamped_to_its_buffer);
  RUN_TEST(test_a_planned_start_lands_on_its_frame);
  RUN_TEST(test_a_skip_past_the_landing_frame);
  RUN_TEST(test_a_lost_landing_frame_lands_on_the_next);
  RUN_TEST(test_a_resync_elsewhere_lands_inexact);
  RUN_TEST(test_no_frame_yet_drops);
  RUN_TEST(test_the_first_word_during_the_landing_keeps_the_end_hold);
  RUN_TEST(test_kept_counts_with_nothing_to_trim);
  return UNITY_END();
}
