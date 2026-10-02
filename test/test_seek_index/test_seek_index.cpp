// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for SeekIndex (the run index), SeekRecorder and resume
// anchors end to end (docs/SEEK.md sections 4.3, 5, 10): a model of the
// device's decoder (test/support/Mp3Synth.h: ESP8266Audio's generator over
// libmad's bit reservoir) plays synthetic CBR and VBR streams through the
// real TrimFeed, RingFeed and PcmRing, the run is recorded as the backend
// records it, and a pause's anchor, checked against the file
// (trackseek::checkAnchor()), restarts a fresh decoder that must give the
// paused sample and every one after it, bit for bit. Run: pio test -e native
// -f test_seek_index
#include <unity.h>

#include <algorithm>
#include <cstring>
#include <cstdint>
#include <random>
#include <vector>

#include "../support/Mp3Synth.h"
#include "PcmRing.h"
#include "RingFeed.h"
#include "SeekIndex.h"
#include "TrackSeek.h"
#include "TrimFeed.h"

namespace {

using mp3synth::ModelDecoder;
using mp3synth::Stream;
using Frames = std::vector<int16_t>;  // interleaved stereo

constexpr uint32_t kRingCap = 8192;
constexpr uint8_t kReader = 1;
constexpr uint32_t kPass = 1024;  // the backend's kChunkFrames
constexpr uint32_t kGen = 7;

int16_t gRingBuf[kRingCap * 2];
PcmRing gRing(gRingBuf, kRingCap);
RingFeed gFeed(gRing);
TrimFeed gTrim(gFeed);
int16_t gHold[TrimFeed::kMaxHold * 2];
SeekIndex::Entry gSlots[2][SeekIndex::kCapacity];
SeekIndex gIndex;
uint8_t gScratch[trackseek::kScratchBytes];

void drain(Frames& out) {
  int16_t buf[1024 * 2];
  for (;;) {
    const uint32_t got = gRing.read(kReader, buf, 1024);
    if (got == 0) return;
    out.insert(out.end(), buf, buf + 2 * got);
  }
}

void resetOutput() {
  gRing.discardAll();
  gRing.setConsumer(kReader);
  gFeed.reset(240, true);
  gTrim.setHoldBuffer(gHold, TrimFeed::kMaxHold);
}

// The run header the backend's beginRun() makes.
SeekIndex::Run topRun(const Stream& s, uint32_t pathHash) {
  SeekIndex::Run r;
  r.kind = SeekIndex::Kind::Mp3;
  r.gen = kGen;
  r.pathHash = pathHash;
  r.fileSize = s.size();
  r.rate = s.rate;
  r.spf = s.spf;
  r.base = 0;
  r.exact = true;
  r.origin = true;
  r.originByte = s.firstAudio;
  r.originT0 = -static_cast<int64_t>(s.trimSkip());
  r.originPreroll = s.firstAudio;
  r.originHash = resumeanchor::frameHash(s.bytes.data() + s.firstAudio, s.size() - s.firstAudio);
  return r;
}

SeekIndex::Run planRun(const Stream& s, uint32_t pathHash, const trackseek::Plan& p) {
  SeekIndex::Run r = topRun(s, pathHash);
  r.base = p.sample;
  r.exact = p.exact;
  r.originByte = p.landByte;
  r.originT0 = static_cast<int64_t>(p.sample) - static_cast<int64_t>(p.skip);
  r.originPreroll = p.prerollByte;
  r.originHash = p.landHash;
  return r;
}

// Decodes through the trim in passes of kPass, the run recorded (`record`)
// as the backend's produceDecoded() does, until `want` frames came out (0:
// to the end, the trim's end dropping the padding).
struct Play {
  Frames out;
  SeekRecorder::Settled settled = SeekRecorder::Settled::None;
  uint32_t passes = 0;
};

Play decode(ModelDecoder& dec, SeekRecorder* rec, size_t want) {
  Play p;
  for (;;) {
    gFeed.setBudget(kPass);
    const bool running = dec.loop(gTrim);
    gFeed.commit();
    ++p.passes;
    if (running && rec) {
      const SeekRecorder::Settled e = rec->afterPass(gTrim, dec);
      if (e != SeekRecorder::Settled::None) p.settled = e;
    }
    drain(p.out);
    if (!running) break;
    if (want && p.out.size() / 2 >= want) break;
  }
  if (!want || p.out.size() / 2 < want) {
    gFeed.setBudget(kPass);
    while (!gTrim.end(false)) {
      gFeed.commit();
      drain(p.out);
      gFeed.setBudget(kPass);
    }
    while (!gFeed.finish()) drain(p.out);
    drain(p.out);
  }
  return p;
}

// From the top, as the backend starts a request: the lead and LAME's delay
// + 529 skipped, the padding - 529 held; the run recorded into the
// decoding slot.
Frames playTop(const Stream& s, uint32_t pathHash = 1, bool record = true) {
  resetOutput();
  const lametag::Info info = [&] {
    lametag::Info i;
    lametag::parse(s.bytes.data() + s.audioStart, s.size() - s.audioStart, &i);
    return i;
  }();
  const lametag::Trim t = lametag::trim(info, 1, true, true);
  gTrim.arm(t.skip, t.hold);
  ModelDecoder dec(s, s.firstAudio);
  SeekRecorder rec;
  if (record) {
    gIndex.begin(topRun(s, pathHash));
    rec.begin(&gIndex, 0, true, false);
  }
  return decode(dec, record ? &rec : nullptr, 0).out;
}

uint32_t holdOf(const Stream& s) {
  lametag::Info info;
  lametag::parse(s.bytes.data() + s.audioStart, s.size() - s.audioStart, &info);
  return lametag::trim(info, 1, false, true).hold;
}

// A start by `plan`, as the backend's beginPrepared() arms it; `record`:
// its run into the decoding slot. `want` frames.
Play playPlan(const Stream& s, const trackseek::Plan& plan, size_t want, bool record, uint32_t pathHash = 1,
              std::vector<uint32_t> lose = {}) {
  resetOutput();
  ModelDecoder dec(s, plan.prerollByte, std::move(lose));
  gTrim.armAt(&dec, plan.landByte, plan.landLength, plan.spf, plan.skip, holdOf(s));
  SeekRecorder rec;
  if (record) {
    gIndex.begin(planRun(s, pathHash, plan));
    rec.begin(&gIndex, plan.sample, plan.exact, true);
  }
  return decode(dec, record ? &rec : nullptr, want);
}

// `n` frames of `got` from frame `from` match `want` from `at`.
void assertRun(const Frames& want, size_t at, const Frames& got, size_t from, size_t n) {
  TEST_ASSERT_TRUE_MESSAGE(want.size() >= 2 * (at + n), "the reference is too short");
  TEST_ASSERT_TRUE_MESSAGE(got.size() >= 2 * (from + n), "the restart gave too little");
  TEST_ASSERT_EQUAL_INT16_ARRAY(want.data() + 2 * at, got.data() + 2 * from, 2 * n);
}

mp3synth::Options vbrOptions(uint32_t frames, uint32_t seed, int version = 3, int rateIndex = 0) {
  mp3synth::Options o;
  o.version = version;
  o.rateIndex = rateIndex;
  o.kbps = mp3synth::vbrRates(version, frames, seed);
  o.header = mp3synth::Header::Xing;
  o.id3 = 1000;
  o.seed = seed;
  o.trailing = 128;
  return o;
}

mp3synth::Options cbrOptions(uint32_t frames, uint32_t kbps, uint32_t seed, int version = 3, int rateIndex = 0) {
  mp3synth::Options o;
  o.version = version;
  o.rateIndex = rateIndex;
  o.kbps.assign(frames, kbps);
  o.header = mp3synth::Header::Info;
  o.id3 = 300;
  o.seed = seed;
  return o;
}

}  // namespace

void setUp() {
  gIndex.setStorage(gSlots[0], gSlots[1], SeekIndex::kCapacity);
  gIndex.reset();
}
void tearDown() {}

// The model decoder from the top through the trim gives exactly the samples
// the encoder was given: frames x spf - delay - padding, all exact.
void test_the_reference_decode_is_the_trimmed_stream() {
  const Stream s = mp3synth::make(vbrOptions(800, 11));
  TEST_ASSERT_TRUE(s.lame);
  const Frames ref = playTop(s, 1, false);
  TEST_ASSERT_EQUAL_UINT64(s.keptSamples(), ref.size() / 2);
  for (size_t i = 0; i < ref.size(); i += 2) TEST_ASSERT_EQUAL_INT16(ref[i] ^ 0x1234, ref[i + 1]);
  // Sample 0 is decoded sample delay + 529 of frame 0.
  int16_t first[2];
  mp3synth::sampleOf(s.frameOf(s.trimSkip()), s.trimSkip() % s.spf, true, first);
  TEST_ASSERT_EQUAL_INT16(first[0], ref[0]);
}

// The model is the device's decoder where it matters (docs/SEEK.md 2.4):
// handed the landing frame itself, that frame is lost (its reservoir is in
// frames it never saw); handed a preroll by the rule (trackseek::
// kPrerollBytes: two frames back and 1 KB before frame k - 1), the landing
// frame comes out exact, also where a loud frame k - 1 after a quiet stretch
// reaches 511 bytes back, which the first rule (1 KB before frame k) missed.
uint32_t prerollFor(const Stream& s, uint32_t k, uint32_t bytesBefore) {
  const uint32_t h = trackseek::historyFrames(s.spf);
  uint32_t pre = k - h - 1;
  while (pre > 0 && s.frames[k - bytesBefore].byte - s.frames[pre].byte < trackseek::kPrerollBytes) --pre;
  return pre;
}

void test_the_model_needs_the_preroll() {
  // Random stretches, and quiet 32 kbit/s ones (big reservoirs) each ended
  // by a loud frame that takes all of it.
  mp3synth::Options o = vbrOptions(1500, 13);
  o.fullness.assign(1500, 60);
  for (size_t at = 100; at + 12 < 1500; at += 97) {
    for (size_t i = at; i < at + 10; ++i) {
      o.kbps[i] = 32;
      o.fullness[i] = 0;
    }
    o.kbps[at + 10] = 320;
    o.fullness[at + 10] = 100;
  }
  const Stream s = mp3synth::make(o);
  const Frames ref = playTop(s, 1, false);
  std::mt19937 rng(3);
  int cold = 0, lost = 0, oldRuleMissed = 0;
  auto at = [&](uint32_t k, uint32_t pre) {
    trackseek::Plan plan;
    plan.landByte = s.frames[k].byte;
    plan.landLength = s.frames[k].length;
    plan.spf = s.spf;
    plan.sample = static_cast<uint64_t>(k) * s.spf - s.trimSkip();
    plan.prerollByte = s.frames[pre].byte;
    return plan;
  };
  std::vector<uint32_t> ks;
  for (int i = 0; i < 120; ++i) ks.push_back(20 + rng() % 1400);
  for (size_t a = 100; a + 12 < 1500; a += 97) ks.push_back(static_cast<uint32_t>(a + 11));  // right after the loud frame
  for (const uint32_t k : ks) {
    // Cold: the decoder handed the landing frame.
    if (s.frames[k].mdb > 0) {
      ++cold;
      playPlan(s, at(k, k), 2000, false);
      if (gTrim.landing() != TrimFeed::Landing::Exact) ++lost;  // (the next frame, or later)
    }
    // The first rule: 2 frames and 1 KB before frame k.
    Play p = playPlan(s, at(k, prerollFor(s, k, 0)), 3000, false);
    if (std::memcmp(p.out.data(), ref.data() + 2 * at(k, k).sample, 3000 * 4) != 0) ++oldRuleMissed;
    // The rule.
    const trackseek::Plan plan = at(k, prerollFor(s, k, 1));
    p = playPlan(s, plan, 3000, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Exact), static_cast<int>(gTrim.landing()));
    assertRun(ref, static_cast<size_t>(plan.sample), p.out, 0, 3000);
  }
  TEST_ASSERT_TRUE(cold > 100);
  TEST_ASSERT_EQUAL_INT(cold, lost);
  TEST_ASSERT_TRUE(oldRuleMissed > 0);
}

// One entry every 4 frames (each the frame's own byte and first sample),
// the first one past the start trim; t0 < 0 for a frame inside the trim.
void test_entries_every_fourth_frame() {
  const Stream s = mp3synth::make(vbrOptions(600, 3));
  playTop(s);
  const uint32_t n = gIndex.entries(gIndex.decodingSlot());
  TEST_ASSERT_TRUE(n >= 600 / 5 && n <= 600 / 4 + 1);
  // Every frame's anchor at its first sample: frame byte and skip 0 at
  // entries, the right frame for every t.
  for (uint32_t k = 2; k < 560; k += 7) {
    const uint64_t t = static_cast<uint64_t>(k) * s.spf - s.trimSkip();
    ResumeAnchor a;
    TEST_ASSERT_TRUE(gIndex.find(1, s.size(), t, &a));
    // The landing entry's frame plus the skip is frame k's start.
    uint32_t landing = 0;
    while (s.frames[landing].byte != a.frameByte) ++landing;
    TEST_ASSERT_EQUAL_UINT64(static_cast<uint64_t>(k - landing) * s.spf, a.skip);
    TEST_ASSERT_TRUE(k - landing < 2 * SeekIndex::kEveryFrames);
    TEST_ASSERT_TRUE(a.exact);
  }
  // The first frame is inside the trim: a time under it lands on the
  // origin (the first audio frame), preroll itself.
  ResumeAnchor a;
  TEST_ASSERT_TRUE(gIndex.find(1, s.size(), 10, &a));
  TEST_ASSERT_EQUAL_UINT32(s.firstAudio, a.frameByte);
  TEST_ASSERT_EQUAL_UINT32(s.firstAudio, a.prerollByte);
  TEST_ASSERT_EQUAL_UINT32(10 + s.trimSkip(), a.skip);
}

// The preroll: at least 2 frames and 1 KB before the landing, or the
// origin's.
void test_the_preroll_is_two_frames_and_a_kilobyte_back() {
  // Quiet stretches (32 kbit/s: 104-byte frames) need many frames for 1 KB.
  mp3synth::Options o = vbrOptions(900, 5);
  for (size_t i = 300; i < 600; ++i) o.kbps[i] = 32;
  const Stream s = mp3synth::make(o);
  playTop(s);
  for (uint64_t t = 0; t < 850ull * s.spf; t += 997) {
    ResumeAnchor a;
    TEST_ASSERT_TRUE(gIndex.find(1, s.size(), t, &a));
    if (a.prerollByte == s.firstAudio) continue;  // (the origin's: from the top)
    TEST_ASSERT_TRUE(a.frameByte - a.prerollByte >= 1024);
    uint32_t landing = 0, pre = 0;
    while (s.frames[landing].byte != a.frameByte) ++landing;
    while (s.frames[pre].byte != a.prerollByte) ++pre;
    TEST_ASSERT_TRUE(landing - pre >= 2);
  }
}

// Nothing outside what the run covers: before a plan run's landing, or far
// past its last entry; another file (path or size) finds nothing.
void test_lookups_outside_the_run_find_nothing() {
  const Stream s = mp3synth::make(cbrOptions(1500, 128, 9));
  mp3synth::Reader r(s.bytes);
  trackseek::PlanIn in;
  in.probe = s.bytes.data() + s.audioStart;
  in.probeBytes = 4096;
  in.audioStart = s.audioStart;
  in.fileSize = s.size();
  in.targetMs = 20000;
  in.lengthMs = s.lengthMs();
  in.useTag = true;
  const trackseek::Plan plan = trackseek::plan(in, r, gScratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::Cbr), static_cast<int>(plan.source));
  playPlan(s, plan, 5 * 44100, true);
  ResumeAnchor a;
  TEST_ASSERT_FALSE(gIndex.find(1, s.size(), plan.sample - plan.skip - 1, &a));  // before its landing frame
  TEST_ASSERT_TRUE(gIndex.find(1, s.size(), plan.sample, &a));
  TEST_ASSERT_TRUE(gIndex.find(1, s.size(), plan.sample + 4 * 44100, &a));
  TEST_ASSERT_FALSE(gIndex.find(1, s.size(), plan.sample + 60 * 44100, &a));  // far past what it decoded
  TEST_ASSERT_FALSE(gIndex.find(2, s.size(), plan.sample, &a));
  TEST_ASSERT_FALSE(gIndex.find(1, s.size() + 1, plan.sample, &a));
}

// A ring that wraps: the oldest entries go; their times find nothing (no
// skip of minutes from the origin), the newest still do.
void test_the_ring_overwrites_the_oldest() {
  static SeekIndex::Entry small[2][32];
  gIndex.setStorage(small[0], small[1], 32);
  const Stream s = mp3synth::make(vbrOptions(400, 21));
  playTop(s);
  TEST_ASSERT_EQUAL_UINT32(32, gIndex.entries(gIndex.decodingSlot()));
  ResumeAnchor a;
  TEST_ASSERT_TRUE(gIndex.find(1, s.size(), 395ull * s.spf - s.trimSkip(), &a));
  TEST_ASSERT_FALSE(gIndex.find(1, s.size(), 100ull * s.spf, &a));
  TEST_ASSERT_TRUE(gIndex.find(1, s.size(), 100, &a));  // the origin still covers the first frames
}

// MPEG-2 (576 samples a frame): a pass of 1,024 ends in every frame or the
// one after, so an entry is the 4th frame or the 5th; lookups don't mind.
void test_a_pass_that_skips_a_frame() {
  const Stream s = mp3synth::make(vbrOptions(1200, 8, 2, 0));
  TEST_ASSERT_EQUAL_UINT32(576, s.spf);
  const Frames ref = playTop(s);
  const uint32_t n = gIndex.entries(gIndex.decodingSlot());
  TEST_ASSERT_TRUE(n > 1200 / 5 - 2 && n <= 1200 / 4 + 1);
  std::mt19937 rng(4);
  for (int i = 0; i < 40; ++i) {
    const uint64_t t = rng() % (ref.size() / 2 - 30000);
    ResumeAnchor a;
    TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, static_cast<uint32_t>(t), s.rate, &a));
    TEST_ASSERT_EQUAL_UINT64(t, a.sample);
    TEST_ASSERT_TRUE(a.skip < 2 * (SeekIndex::kEveryFrames + 1) * s.spf);
  }
}

// The two slots: a request resets both after its own lookup; a join records
// into the other; the advance makes it the heard one; a cut clears it.
void test_two_slots_follow_the_gapless_player() {
  const Stream a = mp3synth::make(cbrOptions(400, 160, 1));
  const Stream b = mp3synth::make(vbrOptions(400, 2));
  playTop(a, 11);
  const int heard = gIndex.heardSlot();
  TEST_ASSERT_EQUAL_INT(heard, gIndex.decodingSlot());
  ResumeAnchor x;
  // A join: B decodes into the other slot; anchors still A's.
  gIndex.beginJoin();
  TEST_ASSERT_EQUAL_INT(1 - heard, gIndex.decodingSlot());
  playTop(b, 22);
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 44100, a.rate, &x));
  TEST_ASSERT_EQUAL_UINT32(a.size(), x.fileSize);
  TEST_ASSERT_TRUE(gIndex.find(22, b.size(), 44100, &x));  // B's run is there too
  // The advance: B is heard.
  gIndex.advance();
  TEST_ASSERT_EQUAL_INT(1 - heard, gIndex.heardSlot());
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 44100, b.rate, &x));
  TEST_ASSERT_EQUAL_UINT32(b.size(), x.fileSize);
  TEST_ASSERT_TRUE(gIndex.find(11, a.size(), 44100, &x));  // A's until the next join clears it
  // The next join goes into A's old slot; cut: back to B, the slot cleared.
  gIndex.beginJoin();
  TEST_ASSERT_EQUAL_INT(heard, gIndex.decodingSlot());
  TEST_ASSERT_FALSE(gIndex.find(11, a.size(), 44100, &x));
  playTop(a, 33);
  TEST_ASSERT_TRUE(gIndex.find(33, a.size(), 44100, &x));
  gIndex.cut();
  TEST_ASSERT_EQUAL_INT(gIndex.heardSlot(), gIndex.decodingSlot());
  TEST_ASSERT_FALSE(gIndex.find(33, a.size(), 44100, &x));
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 44100, b.rate, &x));
  // Another request's generation: no anchor; a request resets both.
  TEST_ASSERT_FALSE(gIndex.anchorAt(kGen + 1, 44100, b.rate, &x));
  gIndex.reset();
  TEST_ASSERT_FALSE(gIndex.find(22, b.size(), 44100, &x));
  TEST_ASSERT_FALSE(gIndex.anchorAt(kGen, 44100, b.rate, &x));
}

// A FLAC run: its header makes the anchor (the sample, the size, the rate,
// the total's low bits).
void test_a_flac_run_anchors_by_its_sample() {
  SeekIndex::Run r;
  r.kind = SeekIndex::Kind::Flac;
  r.gen = kGen;
  r.fileSize = 30000000;
  r.rate = 96000;
  r.base = 96000 * 30;
  r.exact = true;
  r.totalSamples = 0x1234567890ull;
  gIndex.begin(r);
  ResumeAnchor a;
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 44100, 44100, &a));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ResumeAnchor::Kind::Flac), static_cast<int>(a.kind));
  TEST_ASSERT_EQUAL_UINT64(96000 * 31, a.sample);  // a second of ring frames is a second of the file
  TEST_ASSERT_EQUAL_UINT32(0x34567890u, a.frameHash);
  TEST_ASSERT_EQUAL_UINT32(30000000, a.fileSize);
  TEST_ASSERT_TRUE(a.exact);
}

// Ring frames (44.1 kHz) to the file's samples: exact at 44.1 kHz, within
// one source sample otherwise.
void test_ring_frames_to_samples() {
  TEST_ASSERT_EQUAL_UINT64(1000 + 44100, SeekIndex::sampleAt(1000, 44100, 44100, 44100));
  TEST_ASSERT_EQUAL_UINT64(48000, SeekIndex::sampleAt(0, 44100, 48000, 44100));
  TEST_ASSERT_EQUAL_UINT64(22050 / 2, SeekIndex::sampleAt(0, 22050, 22050, 44100));
  for (uint32_t n = 0; n < 200000; n += 37) {
    const uint64_t t = SeekIndex::sampleAt(0, n, 48000, 44100);
    const double exact = n * 48000.0 / 44100.0;
    TEST_ASSERT_TRUE(t <= exact && exact - t < 1.0);
  }
}

// End to end: pauses at 1,200 random samples of runs from the top, each
// anchor checked against the file and started by a fresh decoder: the first
// sample out is the paused one, and the next 10,000 are the top decode's.
// CBR and VBR, 44.1, 48 and 22.05 kHz, with and without LAME's tag.
void test_a_resume_continues_bit_for_bit() {
  std::vector<mp3synth::Options> cases;
  cases.push_back(vbrOptions(1400, 31));            // LAME VBR, 44.1 kHz
  cases.push_back(cbrOptions(1400, 192, 32, 3, 1));  // LAME CBR, 48 kHz
  cases.push_back(vbrOptions(1800, 33, 2, 0));       // MPEG-2 VBR, 22.05 kHz
  mp3synth::Options plain = cbrOptions(1400, 128, 34);
  plain.header = mp3synth::Header::None;  // no header, no tag
  cases.push_back(plain);
  mp3synth::Options quiet = vbrOptions(1400, 35);
  for (auto& k : quiet.kbps) k = k < 64 ? 32 : k;  // big reservoirs in the quiet frames
  quiet.fullness.assign(1400, 5);
  for (size_t i = 0; i < 1400; i += 3) quiet.fullness[i] = 100;
  cases.push_back(quiet);
  mp3synth::Options onsets = vbrOptions(1400, 36);  // loud frames after quiet stretches
  onsets.fullness.assign(1400, 70);
  for (size_t at = 50; at + 12 < 1400; at += 41) {
    for (size_t i = at; i < at + 10; ++i) {
      onsets.kbps[i] = 32;
      onsets.fullness[i] = 0;
    }
    onsets.kbps[at + 10] = 320;
    onsets.fullness[at + 10] = 100;
  }
  cases.push_back(onsets);
  std::mt19937 rng(77);
  for (const mp3synth::Options& o : cases) {
    gIndex.reset();
    const Stream s = mp3synth::make(o);
    const Frames ref = playTop(s);
    TEST_ASSERT_EQUAL_UINT64(s.keptSamples(), ref.size() / 2);
    mp3synth::Reader reader(s.bytes);
    const uint32_t kept = static_cast<uint32_t>(ref.size() / 2);
    for (int i = 0; i < 200; ++i) {
      const uint32_t t = rng() % (kept - 10000 - 6 * s.rate);
      ResumeAnchor a;
      TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, t, s.rate, &a));  // ring frames at the file's rate here
      TEST_ASSERT_EQUAL_UINT64(t, a.sample);
      TEST_ASSERT_TRUE(a.exact);
      trackseek::Plan plan;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                            static_cast<int>(trackseek::checkAnchor(a, reader, s.size(), s.firstAudio,
                                                                    s.lengthMs(), gScratch, &plan)));
      const Play p = playPlan(s, plan, 10000, false);
      TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Exact), static_cast<int>(gTrim.landing()));
      assertRun(ref, t, p.out, 0, 10000);
    }
  }
}

// A chain of five resumes, each started by the last one's anchor and
// recording its own run: still the top decode, sample for sample.
void test_a_chain_of_resumes_never_drifts() {
  const Stream s = mp3synth::make(vbrOptions(2000, 41));
  const Frames ref = playTop(s, 1, false);
  gIndex.reset();
  playTop(s);
  mp3synth::Reader reader(s.bytes);
  std::mt19937 rng(5);
  uint64_t base = 0;  // the run's
  uint32_t t = 3000;
  ResumeAnchor a;
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, t, s.rate, &a));
  for (int step = 0; step < 5; ++step) {
    trackseek::Plan plan;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                          static_cast<int>(trackseek::checkAnchor(a, reader, s.size(), s.firstAudio, s.lengthMs(),
                                                                  gScratch, &plan)));
    gIndex.reset();
    const uint32_t play = 20000 + rng() % 150000;
    const Play p = playPlan(s, plan, play + 10000, true);
    assertRun(ref, static_cast<size_t>(plan.sample), p.out, 0, play + 10000);
    base = plan.sample;
    // The next pause, in this run.
    t = play;
    TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, t, s.rate, &a));
    TEST_ASSERT_EQUAL_UINT64(base + t, a.sample);
    TEST_ASSERT_TRUE(a.exact);
  }
}

// A TOC start is inexact: its run's timeline is the time asked. A pause in
// it resumes the very same audio (bit for bit what that run played on), its
// anchor carrying the shown time and exact false.
void test_a_toc_runs_resume_keeps_its_shown_time() {
  const Stream s = mp3synth::make(vbrOptions(3000, 51));
  mp3synth::Reader reader(s.bytes);
  trackseek::PlanIn in;
  in.probe = s.bytes.data() + s.audioStart;
  in.probeBytes = 4096;
  in.audioStart = s.audioStart;
  in.fileSize = s.size();
  in.targetMs = 40000;
  in.lengthMs = s.lengthMs();
  in.useTag = true;
  const trackseek::Plan plan = trackseek::plan(in, reader, gScratch);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::Source::LameToc), static_cast<int>(plan.source));
  TEST_ASSERT_FALSE(plan.exact);
  const Play run = playPlan(s, plan, 200000, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::Exact), static_cast<int>(gTrim.landing()));
  for (uint32_t n : {0u, 1u, 5000u, 77777u, 150000u}) {
    ResumeAnchor a;
    TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, n, s.rate, &a));
    TEST_ASSERT_FALSE(a.exact);
    TEST_ASSERT_EQUAL_UINT64(plan.sample + n, a.sample);
    trackseek::Plan again;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                          static_cast<int>(trackseek::checkAnchor(a, reader, s.size(), s.firstAudio, s.lengthMs(),
                                                                  gScratch, &again)));
    const Play p = playPlan(s, again, 10000, false);
    assertRun(run.out, n, p.out, 0, 10000);
  }
}

// A landing frame lost (bad data): the start lands on the next frame, a
// frame late, and the run's base moves by that: its anchors stay exact.
void test_a_lost_landing_frame_moves_the_base() {
  const Stream s = mp3synth::make(cbrOptions(1500, 128, 61));
  const Frames ref = playTop(s, 1, false);
  gIndex.reset();
  mp3synth::Reader reader(s.bytes);
  trackseek::PlanIn in;
  in.probe = s.bytes.data() + s.audioStart;
  in.probeBytes = 4096;
  in.audioStart = s.audioStart;
  in.fileSize = s.size();
  in.targetMs = 15000;
  in.lengthMs = s.lengthMs();
  in.useTag = true;
  const trackseek::Plan plan = trackseek::plan(in, reader, gScratch);
  uint32_t landing = 0;
  while (s.frames[landing].byte != plan.landByte) ++landing;
  const Play p = playPlan(s, plan, 50000, true, 1, {landing});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(TrimFeed::Landing::NextFrame), static_cast<int>(gTrim.landing()));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SeekRecorder::Settled::Late), static_cast<int>(p.settled));
  const uint32_t late = gTrim.lateBy();
  TEST_ASSERT_EQUAL_UINT32(s.spf - plan.skip, late);
  // What it played is the reference from the frame after (its first frame
  // has no overlap from the lost one: from the second frame on, exact).
  assertRun(ref, static_cast<size_t>(plan.sample) + late + s.spf, p.out, s.spf, 20000);
  // A pause 30,000 frames in: the anchor's sample is the base + lateBy + 30,000.
  ResumeAnchor a;
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 30000, s.rate, &a));
  TEST_ASSERT_EQUAL_UINT64(plan.sample + late + 30000, a.sample);
  TEST_ASSERT_TRUE(a.exact);
}

// A seek back into what the run played (qs30 after 90 s): the index's
// plan, exact, the same audio as from the top.
void test_a_seek_back_into_the_run_is_exact() {
  const Stream s = mp3synth::make(vbrOptions(4000, 71));  // ~104 s
  const Frames ref = playTop(s);
  mp3synth::Reader reader(s.bytes);
  for (uint32_t ms : {30000u, 85000u, 1u, 100000u}) {
    const uint64_t t = static_cast<uint64_t>(ms) * s.rate / 1000;
    ResumeAnchor a;
    TEST_ASSERT_TRUE(gIndex.find(1, s.size(), t, &a));
    trackseek::Plan plan;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                          static_cast<int>(trackseek::checkAnchor(a, reader, s.size(), s.firstAudio, 0, gScratch,
                                                                  &plan)));
    const Play p = playPlan(s, plan, 5000, false);
    assertRun(ref, static_cast<size_t>(t), p.out, 0, 5000);
  }
}

// The gapless player's interplay: a pause in A's tail while B is decoded
// ahead anchors in A (its run is the heard one until the advance); after the
// advance a pause anchors in B, whose run began at the join: exact.
void test_a_pause_around_a_join_anchors_in_the_heard_track() {
  const Stream a = mp3synth::make(vbrOptions(900, 81));
  const Stream b = mp3synth::make(cbrOptions(900, 192, 82));
  const Frames refA = playTop(a, 1, false);
  const Frames refB = playTop(b, 2, false);
  gIndex.reset();
  playTop(a, 1);
  gIndex.beginJoin();  // B decoded ahead (the backend's Tracks::start())
  playTop(b, 2);
  mp3synth::Reader readerA(a.bytes), readerB(b.bytes);
  const uint32_t keptA = static_cast<uint32_t>(refA.size() / 2);
  // 1.4 s before A's end (the outputs still in A).
  const uint32_t t = keptA - 61740;
  ResumeAnchor x;
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, t, a.rate, &x));
  TEST_ASSERT_EQUAL_UINT32(a.size(), x.fileSize);
  trackseek::Plan plan;
  // (With its length the tail rule would start it at 0:00, as any start in
  // a track's last 5 s.)
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Tail),
                        static_cast<int>(trackseek::checkAnchor(x, readerA, a.size(), a.firstAudio, a.lengthMs(),
                                                                gScratch, &plan)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                        static_cast<int>(trackseek::checkAnchor(x, readerA, a.size(), a.firstAudio, 0, gScratch, &plan)));
  Play p = playPlan(a, plan, 0, false);
  // To A's end, its padding cut by the hold: exactly what the top decode had.
  TEST_ASSERT_EQUAL_UINT32(keptA - t, p.out.size() / 2);
  assertRun(refA, t, p.out, 0, keptA - t);
  // The advance: 0.2 s into B.
  gIndex.advance();
  TEST_ASSERT_TRUE(gIndex.anchorAt(kGen, 8820, b.rate, &x));
  TEST_ASSERT_EQUAL_UINT32(b.size(), x.fileSize);
  TEST_ASSERT_EQUAL_UINT64(8820, x.sample);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(trackseek::AnchorCheck::Ok),
                        static_cast<int>(trackseek::checkAnchor(x, readerB, b.size(), b.firstAudio, 0, gScratch, &plan)));
  p = playPlan(b, plan, 10000, false);
  assertRun(refB, 8820, p.out, 0, 10000);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_reference_decode_is_the_trimmed_stream);
  RUN_TEST(test_the_model_needs_the_preroll);
  RUN_TEST(test_entries_every_fourth_frame);
  RUN_TEST(test_the_preroll_is_two_frames_and_a_kilobyte_back);
  RUN_TEST(test_lookups_outside_the_run_find_nothing);
  RUN_TEST(test_the_ring_overwrites_the_oldest);
  RUN_TEST(test_a_pass_that_skips_a_frame);
  RUN_TEST(test_two_slots_follow_the_gapless_player);
  RUN_TEST(test_a_flac_run_anchors_by_its_sample);
  RUN_TEST(test_ring_frames_to_samples);
  RUN_TEST(test_a_resume_continues_bit_for_bit);
  RUN_TEST(test_a_chain_of_resumes_never_drifts);
  RUN_TEST(test_a_toc_runs_resume_keeps_its_shown_time);
  RUN_TEST(test_a_lost_landing_frame_moves_the_base);
  RUN_TEST(test_a_seek_back_into_the_run_is_exact);
  RUN_TEST(test_a_pause_around_a_join_anchors_in_the_heard_track);
  return UNITY_END();
}
