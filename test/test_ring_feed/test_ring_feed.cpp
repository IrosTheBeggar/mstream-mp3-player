// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for RingFeed: the decode side of the ring, converting
// every track to 44.1 kHz on its way into the real PcmRing
// (docs/RESAMPLER.md, section 5). Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "PcmRing.h"
#include "RateConverter.h"
#include "RingFeed.h"
#include "TableCopy.h"

namespace {
using Frames = std::vector<int16_t>;  // interleaved stereo

constexpr uint32_t kRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000};
constexpr uint32_t kCpu = 240;
constexpr bool kHiRes = true;  // 88.2/96 kHz are tested whether or not this build plays them
constexpr uint32_t kRingCap = 4096;  // small, so it fills often
constexpr uint8_t kReader = 1;
constexpr uint32_t kPass = 1024;     // the backend's source frames per pass

// Big objects live here, not on the stack.
int16_t gRingBuf[kRingCap * 2];
PcmRing gRing(gRingBuf, kRingCap);
RingFeed gFeed(gRing);
RateConverter gRef;

Frames noise(size_t frames, uint32_t seed, int amp) {
  std::mt19937 rng(seed);
  Frames v(frames * 2);
  for (auto& s : v) s = static_cast<int16_t>(static_cast<int>(rng() % (2u * amp + 1)) - amp);
  return v;
}

// The converter alone, one shot: the rate first, every frame, the tail.
Frames reference(uint32_t hz, const Frames& in, bool mono = false) {
  RateConverter& c = gRef;
  c.reset();
  TEST_ASSERT_TRUE(c.setRate(hz, kCpu, kHiRes));
  c.setMono(mono);
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

// A fresh ring and feed, the reader the consumer.
void fresh(uint32_t cpuMhz = kCpu, bool hiRes = kHiRes) {
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.setDiscard(false);
  gFeed.reset(cpuMhz, hiRes);
}

// The output side: takes up to `n` frames out of the ring.
void drain(Frames& out, uint32_t n) {
  int16_t buf[512 * 2];
  while (n > 0) {
    const uint32_t got = gRing.read(kReader, buf, std::min<uint32_t>(n, 512));
    if (got == 0) return;
    out.insert(out.end(), buf, buf + 2 * got);
    n -= got;
  }
}

// An ESP8266Audio generator handing `src` over through the feed, pass by
// pass, with an output that reads a random amount between passes (often
// nothing): it offers its last sample first, and keeps any sample the feed
// refuses for the next pass. MP3's order: its constructor's {0,0} goes
// first and its first decoded sample before setRate() (`mp3Order`).
Frames play(uint32_t hz, const Frames& src, uint32_t seed, bool mp3Order, uint64_t* refused = nullptr) {
  std::mt19937 rng(seed);
  fresh();
  Frames out;
  if (!mp3Order) TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
  const size_t frames = src.size() / 2;
  int16_t last[2] = {0, 0};  // lastSample: the constructor's {0,0}
  bool holding = mp3Order;   // an MP3 offers it first
  size_t next = 0;
  size_t offered = 0;
  while (next < frames || holding) {
    gFeed.setBudget(kPass);
    for (;;) {
      if (!holding) {
        if (next >= frames) break;
        last[0] = src[2 * next];
        last[1] = src[2 * next + 1];
        ++next;
        holding = true;
      }
      if (mp3Order && offered == 2) TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));  // with the 2nd decoded sample
      if (!gFeed.consume(last)) {
        if (refused && gFeed.budgetLeft() > 0) ++*refused;
        break;
      }
      ++offered;
      holding = false;
    }
    gFeed.commit();
    drain(out, rng() % 3 == 0 ? 0 : rng() % 900);
  }
  while (!gFeed.finish()) drain(out, rng() % 900 + 1);
  drain(out, kRingCap);
  return out;
}

void assertSame(const Frames& want, const Frames& got) {
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) {
    if (want[i] != got[i]) {
      char msg[64];
      snprintf(msg, sizeof(msg), "first difference at frame %u", static_cast<unsigned>(i / 2));
      TEST_FAIL_MESSAGE(msg);
    }
  }
}

uint64_t ringFrames(uint32_t hz, uint64_t src) { return RateConverter::plan(hz, kCpu, kHiRes).ringFrames(src); }
}  // namespace

void setUp() {}
void tearDown() {}

// Every rate: the ring gets exactly the converter's output, bit for bit,
// and exactly ceil(N x 44100 / rate) frames, however often it is full.
void test_the_ring_gets_exactly_the_converted_stream() {
  for (uint32_t hz : kRates) {
    const Frames src = noise(hz / 4 + 7, hz, 20000);
    uint64_t refused = 0;
    const Frames got = play(hz, src, hz + 1, false, &refused);
    TEST_ASSERT_TRUE(refused > 0);  // the ring was full, often
    assertSame(reference(hz, src), got);
    TEST_ASSERT_EQUAL_UINT64(ringFrames(hz, src.size() / 2), got.size() / 2);
    TEST_ASSERT_EQUAL_UINT64(got.size() / 2, gFeed.made());
  }
}

// The firmware's real order: an MP3 hands over {0,0} and its first sample
// before it says its rate. Both are kept; the stream is the same as one
// that said its rate first (with that {0,0} in front).
void test_the_mp3_call_order() {
  for (uint32_t hz : kRates) {
    const Frames src = noise(hz / 8 + 3, hz + 7, 20000);
    Frames withZero(2, 0);
    withZero.insert(withZero.end(), src.begin(), src.end());
    assertSame(reference(hz, withZero), play(hz, src, hz + 2, true));
  }
}

// A rate the converter doesn't take: nothing reaches the ring, the reason
// is there for the failure text, and the end comes at once.
void test_a_refused_rate_takes_nothing() {
  fresh();
  TEST_ASSERT_FALSE(gFeed.setRate(37800));
  TEST_ASSERT_TRUE(gFeed.rejected());
  TEST_ASSERT_EQUAL_INT(37800, gFeed.rate());
  TEST_ASSERT_EQUAL_STRING("isn't supported (8-48 kHz, 88.2 and 96 kHz)", gFeed.refusal());
  gFeed.setBudget(kPass);
  const int16_t s[2] = {1000, -1000};
  TEST_ASSERT_FALSE(gFeed.consume(s));
  TEST_ASSERT_EQUAL_UINT32(0, gFeed.write(s, 1));
  TEST_ASSERT_TRUE(gFeed.finish());
  TEST_ASSERT_EQUAL_UINT32(0, gRing.size());
  TEST_ASSERT_EQUAL_UINT64(0, gFeed.made());
  // 0 (FLAC before its header) changes nothing, refused or not.
  TEST_ASSERT_FALSE(gFeed.setRate(0));
  // An MP3's frames before its (refused) rate never come out either.
  fresh();
  gFeed.setBudget(kPass);
  TEST_ASSERT_TRUE(gFeed.consume(s));
  TEST_ASSERT_TRUE(gFeed.consume(s));
  TEST_ASSERT_FALSE(gFeed.setRate(192000));
  TEST_ASSERT_TRUE(gFeed.finish());
  TEST_ASSERT_EQUAL_UINT32(0, gRing.size());
}

// 88.2 and 96 kHz need the 240 MHz CPU speed (the one set at boot, passed
// by the caller); everything up to 48 kHz plays at 160.
void test_hi_res_needs_240_mhz() {
  for (uint32_t hz : {88200u, 96000u}) {
    fresh(160);
    TEST_ASSERT_FALSE(gFeed.setRate(static_cast<int>(hz)));
    TEST_ASSERT_EQUAL_STRING("needs the 240 MHz CPU speed", gFeed.refusal());
    TEST_ASSERT_EQUAL(RateConverter::Refusal::NeedsCpu, gFeed.refusalKind());
    fresh(240);
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    TEST_ASSERT_EQUAL_STRING("", gFeed.refusal());
    TEST_ASSERT_EQUAL(RateConverter::Refusal::None, gFeed.refusalKind());
  }
  for (uint32_t hz : kRates) {
    if (hz > 48000) continue;
    fresh(160);
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
  }
}

// A start, skip or seek: discardAll() then reset(). Nothing of the last
// stream comes out after it: not its staged frames, not its history.
void test_reset_leaves_nothing_behind() {
  for (uint32_t hz : {48000u, 8000u, 96000u, 44100u}) {
    const Frames a = noise(20000, hz + 11, 30000);
    const Frames b = noise(2000, hz + 12, 20000);
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    gFeed.setBudget(100000);
    size_t f = 0;
    while (f < a.size() / 2 && gFeed.consume(&a[2 * f])) ++f;  // until the ring and the stage are full
    TEST_ASSERT_TRUE(f < a.size() / 2);
    gRing.discardAll();
    gFeed.reset(kCpu, kHiRes);
    TEST_ASSERT_EQUAL_UINT64(0, gFeed.made());
    Frames out;
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    gFeed.setBudget(100000);
    for (size_t i = 0; i < b.size() / 2; ++i) {
      while (!gFeed.consume(&b[2 * i])) drain(out, 700);
    }
    while (!gFeed.finish()) drain(out, 700);
    drain(out, kRingCap);
    assertSame(reference(hz, b), out);
  }
}

// The built-in tracks hand over blocks: what the ring has no room for is
// left with the caller, and the stream comes out whole and exact.
void test_blocks_resume_where_the_ring_was_full() {
  std::mt19937 rng(5);
  for (uint32_t hz : {48000u, 22050u, 8000u, 96000u, 44100u}) {
    const Frames src = noise(hz / 3 + 5, hz + 21, 20000);
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    Frames out;
    const uint32_t total = static_cast<uint32_t>(src.size() / 2);
    uint32_t at = 0;
    bool partial = false;
    while (at < total) {
      const uint32_t n = std::min<uint32_t>(1024, total - at);
      const uint32_t taken = gFeed.write(&src[2 * at], n);
      TEST_ASSERT_TRUE(taken <= n);
      partial = partial || taken < n;
      at += taken;
      gFeed.commit();
      drain(out, rng() % 3 == 0 ? 0 : rng() % 700);
    }
    TEST_ASSERT_TRUE(partial);
    while (!gFeed.finish()) drain(out, 900);
    drain(out, kRingCap);
    assertSame(reference(hz, src), out);
  }
}

// Mono (SetChannels(1)): the left channel, copied; the right is ignored.
// A channel change never resets: MP3 says 2, then 1 two frames in.
void test_mono_copies_the_left_channel() {
  const Frames src = noise(4000, 77, 20000);
  Frames mono = src;
  for (size_t i = 0; i < mono.size(); i += 2) mono[i + 1] = mono[i];
  for (uint32_t hz : {48000u, 44100u, 16000u}) {
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    gFeed.setChannels(2);
    Frames out;
    gFeed.setBudget(100000);
    for (size_t i = 0; i < src.size() / 2; ++i) {
      if (i == 2) gFeed.setChannels(1);
      const int16_t frame[2] = {src[2 * i], static_cast<int16_t>(i < 2 ? src[2 * i] : -src[2 * i])};
      while (!gFeed.consume(frame)) drain(out, 700);
    }
    while (!gFeed.finish()) drain(out, 700);
    drain(out, kRingCap);
    assertSame(reference(hz, mono), out);
    for (size_t i = 0; i < out.size(); i += 2) TEST_ASSERT_EQUAL_INT16(out[i], out[i + 1]);
  }
}

// The budget counts source frames, so a pass never decodes more than at
// 44.1 kHz: within the converter's delay (no ring frames yet) the source
// frames run out first.
void test_the_budget_counts_source_frames() {
  fresh();
  TEST_ASSERT_TRUE(gFeed.setRate(8000));
  const Frames src = noise(100, 3, 1000);
  gFeed.setBudget(30);
  uint32_t taken = 0;
  while (gFeed.consume(&src[2 * taken])) ++taken;
  TEST_ASSERT_EQUAL_UINT32(30, taken);
  TEST_ASSERT_EQUAL_UINT32(0, gFeed.budgetLeft());
  // 30 frames at 8 kHz, less the 6x stage's delay (24 frames): 36 at
  // 48 kHz, all inside the 147/160 stage's delay (24 of them) and then 12.
  // (Held for the block path until the pass's commit() converts them.)
  gFeed.commit();
  TEST_ASSERT_EQUAL_UINT64((12 * 147 + 159) / 160, gFeed.made());
}

// ...and ring frames: a low rate's pass stops once it has made a pass's
// worth, so its conversion above the UI loop is no longer than 44.1 kHz's
// (8 kHz's 1024 source frames would make 5,645). Each frame is taken
// whole: the last may add up to maxOut() - 1 more. At 44.1 kHz and above
// the source frames run out first, as before.
void test_the_budget_caps_ring_frames_too() {
  const Frames src = noise(20000, 8, 20000);
  for (uint32_t hz : kRates) {
    fresh();
    gFeed.setDiscard(true);  // the ring never refuses: only the budget stops a pass
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    uint32_t at = 0;
    for (int pass = 0; pass < 12; ++pass) {
      gFeed.setBudget(kPass);
      const uint64_t before = gFeed.made();
      uint32_t taken = 0;
      while (gFeed.consume(&src[2 * at])) {
        ++at;
        ++taken;
      }
      gFeed.commit();
      const uint64_t made = gFeed.made() - before;
      TEST_ASSERT_TRUE(taken <= kPass);
      TEST_ASSERT_TRUE(made < kPass + gFeed.converter().maxOut());
      if (pass < 2) continue;  // past the delay: one budget or the other is spent
      if (hz >= 44100) {
        TEST_ASSERT_EQUAL_UINT32(kPass, taken);
      } else {
        TEST_ASSERT_TRUE(made >= kPass);
        TEST_ASSERT_TRUE(taken < kPass);
      }
    }
  }
  // Before the rate is known (an MP3's first frames) the cap holds too.
  fresh();
  gFeed.setDiscard(true);
  gFeed.setBudget(kPass);
  TEST_ASSERT_TRUE(gFeed.consume(&src[0]));
  TEST_ASSERT_TRUE(gFeed.consume(&src[2]));
  TEST_ASSERT_TRUE(gFeed.setRate(8000));
  uint32_t taken = 2;
  while (gFeed.consume(&src[2 * taken])) ++taken;
  TEST_ASSERT_TRUE(gFeed.made() < kPass + 6);
  TEST_ASSERT_TRUE(taken > 180 && taken < 230);  // 1024 x 80 / 441 = 186, and the delays' ~28
  gFeed.setDiscard(false);
}

// The ring budget holds across a rate change mid-pass into or out of
// 44.1 kHz: the passthrough's frames count against it (a pass that passed
// 600 frames and then goes to 12 kHz has 424 left, not 1024), and a pass
// that spent it at 12 kHz takes nothing more at 44.1. The last frame taken
// may still add up to perFrameMax() - 1 (6 at most: 8 kHz's) more.
void test_the_ring_budget_holds_across_a_rate_change() {
  const Frames src = noise(40000, 12, 20000);
  const uint32_t pairs[][2] = {{44100, 12000}, {12000, 44100}, {44100, 8000}, {8000, 44100},
                               {44100, 48000}, {48000, 44100}, {22050, 44100}, {44100, 22050}};
  for (const auto& p : pairs) {
    for (uint32_t switchAt : {1u, 40u, 333u, 600u, 1000u}) {
      fresh();
      gFeed.setDiscard(true);  // the ring never refuses: only the budgets stop a pass
      TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(p[0])));
      uint32_t at = 0;
      // Past the first route's delay, so the pass with the switch is a full one.
      for (int pass = 0; pass < 3; ++pass) {
        gFeed.setBudget(kPass);
        while (gFeed.consume(&src[2 * at])) ++at;
        gFeed.commit();
      }
      for (int pass = 0; pass < 3; ++pass) {  // the switch, then two passes at the new rate
        gFeed.setBudget(kPass);
        const uint64_t before = gFeed.made();
        uint32_t taken = 0;
        for (;;) {
          if (pass == 0 && taken == switchAt) TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(p[1])));
          if (!gFeed.consume(&src[2 * at])) break;
          ++at;
          ++taken;
        }
        gFeed.commit();
        const uint64_t made = gFeed.made() - before;
        char msg[96];
        snprintf(msg, sizeof(msg), "%u -> %u Hz after %u frames, pass %d: %u made, %u taken",
                 static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]), static_cast<unsigned>(switchAt), pass,
                 static_cast<unsigned>(made), static_cast<unsigned>(taken));
        TEST_ASSERT_TRUE_MESSAGE(taken <= kPass, msg);
        TEST_ASSERT_TRUE_MESSAGE(made <= kPass + 5, msg);
        // Not stopped early either: one budget or the other is spent.
        TEST_ASSERT_TRUE_MESSAGE(taken == kPass || made + 6 > kPass, msg);
      }
    }
  }
  gFeed.setDiscard(false);
}

// A block (a built-in track's pass) stops at `maxMade` ring frames, never
// past them; the rest stays with the caller, and the stream is the same.
void test_a_block_makes_at_most_max_made() {
  for (uint32_t hz : {8000u, 22050u, 48000u, 44100u}) {
    const Frames src = noise(hz / 4, hz + 31, 20000);
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    Frames out;
    const uint32_t total = static_cast<uint32_t>(src.size() / 2);
    uint32_t at = 0;
    while (at < total) {
      const uint64_t before = gFeed.made();
      const uint32_t taken = gFeed.write(&src[2 * at], total - at, kPass);
      TEST_ASSERT_TRUE(gFeed.made() - before <= kPass);
      if (hz < 44100 && at > 0 && taken < total - at && gRing.space() >= kRingCap / 2) {
        TEST_ASSERT_TRUE(gFeed.made() - before + gFeed.converter().maxOut() > kPass);  // stopped by the cap
      }
      at += taken;
      gFeed.commit();
      drain(out, kRingCap);
    }
    while (!gFeed.finish()) drain(out, kRingCap);
    drain(out, kRingCap);
    assertSame(reference(hz, src), out);
  }
}

// 88.2/96 kHz don't play in this build (RateConverter::kHiResOn) until the
// device check: refused at 240 MHz too, with their own refusal; the benches
// reset with hiRes true.
void test_hi_res_is_off_by_default() {
  for (uint32_t hz : {88200u, 96000u}) {
    gRing.discardAll();
    gFeed.reset(240);  // the firmware's call: the build's default
    TEST_ASSERT_EQUAL(RateConverter::kHiResOn, gFeed.setRate(static_cast<int>(hz)));
    if (!RateConverter::kHiResOn) {
      TEST_ASSERT_EQUAL(RateConverter::Refusal::Off, gFeed.refusalKind());
      const int16_t s[2] = {1000, -1000};
      gFeed.setBudget(kPass);
      TEST_ASSERT_FALSE(gFeed.consume(s));
    }
    fresh(240, true);
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
  }
}

// The same rate again (a FLAC seek) and 0 (FLAC before its header) change
// nothing mid-stream.
void test_the_same_rate_again_changes_nothing() {
  const Frames src = noise(5000, 9, 20000);
  for (uint32_t hz : {48000u, 32000u, 88200u}) {
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
    Frames out;
    gFeed.setBudget(100000);
    for (size_t i = 0; i < src.size() / 2; ++i) {
      if (i == 2500) {
        TEST_ASSERT_TRUE(gFeed.setRate(static_cast<int>(hz)));
        TEST_ASSERT_TRUE(gFeed.setRate(0));
        TEST_ASSERT_TRUE(gFeed.setRate(-1));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(hz), gFeed.rate());
      }
      while (!gFeed.consume(&src[2 * i])) drain(out, 700);
    }
    while (!gFeed.finish()) drain(out, 700);
    drain(out, kRingCap);
    assertSame(reference(hz, src), out);
  }
}

// The room a pass needs, in ring frames: at the track's ratio once known,
// at the largest (8 kHz's) before; plus what is staged and one push's worth.
void test_room_for_a_pass() {
  fresh();
  TEST_ASSERT_EQUAL_UINT32((2048u * 441 + 79) / 80 + RateConverter::kMaxOut, gFeed.roomFor(2048));
  TEST_ASSERT_TRUE(gFeed.setRate(48000));
  TEST_ASSERT_EQUAL_UINT32((2048u * 147 + 159) / 160 + RateConverter::kMaxOut, gFeed.roomFor(2048));
  TEST_ASSERT_TRUE(gFeed.setRate(44100));  // a change mid-stream: the new ratio
  TEST_ASSERT_EQUAL_UINT32(2048u + RateConverter::kMaxOut, gFeed.roomFor(2048));
  const Frames src = noise(10, 4, 100);
  gFeed.setBudget(10);
  for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(gFeed.consume(&src[2 * i]));
  TEST_ASSERT_EQUAL_UINT32(10 + 2048u + RateConverter::kMaxOut, gFeed.roomFor(2048));
}

// The bench drops what it makes: the ring stays empty and never refuses.
void test_discard_drops_everything() {
  fresh();
  gFeed.setDiscard(true);
  TEST_ASSERT_TRUE(gFeed.setRate(8000));
  const Frames src = noise(8000, 1, 8000);
  TEST_ASSERT_EQUAL_UINT32(8000, gFeed.write(src.data(), 8000));
  TEST_ASSERT_TRUE(gFeed.finish());
  TEST_ASSERT_EQUAL_UINT32(0, gRing.size());
  TEST_ASSERT_EQUAL_UINT64(44100, gFeed.made());
  gFeed.setDiscard(false);
}

// Silence in, exact zeros out, at every rate, tail included: nothing plays
// by itself.
void test_silence_gives_exact_zeros() {
  const Frames zeros(2 * 5000, 0);
  for (uint32_t hz : kRates) {
    const Frames got = play(hz, zeros, hz + 3, true);
    TEST_ASSERT_EQUAL_UINT64(ringFrames(hz, 5001), got.size() / 2);
    for (int16_t v : got) TEST_ASSERT_EQUAL_INT16(0, v);
  }
}

// 44.1 kHz takes the fast path: each frame is in the stage at once (none
// held), the converter's counters stay exact, and the ring gets the frames
// as they came, in mono too.
void test_the_passthrough_goes_straight_to_the_stage() {
  for (bool mono : {false, true}) {
    fresh();
    TEST_ASSERT_TRUE(gFeed.setRate(44100));
    gFeed.setChannels(mono ? 1 : 2);
    gFeed.setBudget(kPass);
    const Frames src = noise(600, 5, 20000);
    for (size_t i = 0; i < src.size() / 2; ++i) {
      TEST_ASSERT_TRUE(gFeed.consume(&src[2 * i]));
      TEST_ASSERT_EQUAL_UINT64(i + 1, gFeed.made());
    }
    gFeed.commit();
    TEST_ASSERT_EQUAL_UINT64(600, gFeed.converter().taken());
    TEST_ASSERT_EQUAL_UINT64(600, gFeed.converter().produced());
    TEST_ASSERT_TRUE(gFeed.finish());
    Frames out;
    drain(out, kRingCap);
    Frames want = src;
    if (mono) {
      for (size_t i = 0; i < want.size(); i += 2) want[i + 1] = want[i];
    }
    assertSame(want, out);
  }
}

// Frames held for the block path go out as they were taken: a rate change
// (a new route, the filters restarted) or a channel change mid-block
// converts them first, and a full ring never splits or loses one.
void test_held_frames_keep_their_rate_and_channels() {
  std::mt19937 rng(17);
  const Frames src = noise(3000, 23, 20000);
  // The reference: the converter frame by frame, 48 kHz for 1000 frames,
  // then 22.05 kHz, mono from frame 2000.
  RateConverter& c = gRef;
  c.reset();
  TEST_ASSERT_TRUE(c.setRate(48000, kCpu, kHiRes));
  Frames want;
  int16_t buf[RateConverter::kMaxOut * 2];
  for (size_t i = 0; i < 3000; ++i) {
    if (i == 1000) TEST_ASSERT_TRUE(c.setRate(22050, kCpu, kHiRes));
    if (i == 2000) c.setMono(true);
    const uint32_t n = c.push(&src[2 * i], buf);
    want.insert(want.end(), buf, buf + 2 * n);
  }
  while (!c.finished()) {
    const uint32_t n = c.finishPush(buf);
    want.insert(want.end(), buf, buf + 2 * n);
  }
  fresh();
  TEST_ASSERT_TRUE(gFeed.setRate(48000));
  Frames out;
  gFeed.setBudget(kPass);
  for (size_t i = 0; i < 3000; ++i) {
    if (i == 1000) TEST_ASSERT_TRUE(gFeed.setRate(22050));
    if (i == 2000) gFeed.setChannels(1);
    while (!gFeed.consume(&src[2 * i])) {
      gFeed.commit();
      drain(out, rng() % 3 == 0 ? 0 : rng() % 400);
      gFeed.setBudget(kPass);
    }
  }
  while (!gFeed.finish()) drain(out, 700);
  drain(out, kRingCap);
  assertSame(want, out);
}

// The firmware's table copy (TableCopy) across tracks played through the
// feed in both call orders: made when a converted track starts, freed when
// a 44.1 kHz one does, after the feed's reset(); a freed block is poisoned
// and kept, so a track reading it would differ from the flash tables' bits.
namespace pool {
std::vector<std::vector<uint32_t>> blocks;
int live = 0;
void* alloc(size_t bytes) {
  blocks.emplace_back(bytes / 4 + 1, 0u);
  ++live;
  return blocks.back().data();
}
void release(void* p) {
  for (auto& b : blocks) {
    if (b.data() == p) std::fill(b.begin(), b.end(), 0x7FFF7FFFu);
  }
  --live;
}
}  // namespace pool
TableCopy gCopy(pool::alloc, pool::release);
int gFreed = 0, gCopied = 0;
void copyHook(bool wanted) {
  const TableCopy::Event e = gCopy.want(wanted);
  gFreed += e == TableCopy::Event::Freed;
  gCopied += e == TableCopy::Event::Copied;
}

void test_the_table_copy_is_swapped_only_at_a_track_start() {
  const uint32_t tracks[] = {48000, 44100, 22050, 44100, 8000, 32000, 44100, 48000};
  std::vector<Frames> src, want[2];
  for (uint32_t hz : tracks) {
    src.push_back(noise(hz / 6 + 11, hz + 3, 30000));
    want[0].push_back(reference(hz, src.back()));  // (no hook yet: the flash tables)
    Frames withZero(2, 0);
    withZero.insert(withZero.end(), src.back().begin(), src.back().end());
    want[1].push_back(reference(hz, withZero));
  }
  RateConverter::setTablesWanted(copyHook);
  for (int mp3 = 0; mp3 < 2; ++mp3) {
    for (size_t t = 0; t < sizeof(tracks) / sizeof(tracks[0]); ++t) {
      assertSame(want[mp3][t], play(tracks[t], src[t], 900 + t, mp3 == 1));
      TEST_ASSERT_EQUAL(tracks[t] != 44100, gCopy.copied());
    }
  }
  TEST_ASSERT_EQUAL_INT(6, gFreed);   // each 44.1 kHz track after a converted one
  TEST_ASSERT_EQUAL_INT(7, gCopied);  // each converted track after a 44.1 kHz one (and the first)
  gCopy.want(false);
  RateConverter::setTablesWanted(nullptr);
  TEST_ASSERT_EQUAL_INT(0, pool::live);
  TEST_ASSERT_FALSE(RateConverter::tablesCopied());
}

void test_it_is_small() {
  // Inside RingOutput, which must stay under 4 KB (internal RAM).
  // (The converter ~1.9 KB, the stage 1 KB, the block 128 B.)
  TEST_ASSERT_TRUE(sizeof(RingFeed) < 3584);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_ring_gets_exactly_the_converted_stream);
  RUN_TEST(test_the_mp3_call_order);
  RUN_TEST(test_a_refused_rate_takes_nothing);
  RUN_TEST(test_hi_res_needs_240_mhz);
  RUN_TEST(test_reset_leaves_nothing_behind);
  RUN_TEST(test_blocks_resume_where_the_ring_was_full);
  RUN_TEST(test_mono_copies_the_left_channel);
  RUN_TEST(test_the_budget_counts_source_frames);
  RUN_TEST(test_the_budget_caps_ring_frames_too);
  RUN_TEST(test_the_ring_budget_holds_across_a_rate_change);
  RUN_TEST(test_a_block_makes_at_most_max_made);
  RUN_TEST(test_hi_res_is_off_by_default);
  RUN_TEST(test_the_same_rate_again_changes_nothing);
  RUN_TEST(test_room_for_a_pass);
  RUN_TEST(test_discard_drops_everything);
  RUN_TEST(test_silence_gives_exact_zeros);
  RUN_TEST(test_the_passthrough_goes_straight_to_the_stage);
  RUN_TEST(test_held_frames_keep_their_rate_and_channels);
  RUN_TEST(test_the_table_copy_is_swapped_only_at_a_track_start);
  RUN_TEST(test_it_is_small);
  return UNITY_END();
}
