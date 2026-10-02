// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the USB visualizer's split of the beat tracker
// (docs/USB-VISUALIZER.md): the computer runs only the front end
// (HopFrontEnd) and sends the two energies per hop as text; the Core2 feeds
// them to its own BeatTracker (feedHop()). Fed that way the tracker must
// give the same beat grid as fed the audio, bit for bit, and still meet the
// click-track targets at 44.1 and 48 kHz after the energies went through
// text. Also writes (HOP_GOLDEN_OUT=<path>) or checks hop_golden.h, the
// energies a port of the front end checks itself against.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "BeatTracker.h"
#include "ClickGen.h"
#include "HopFrontEnd.h"
#include "hop_golden.h"

namespace {

// The tracker's targets (test_beat_tracker's, MASCOT-POC.md): lock within
// this long, then phase errors (ms) below these.
constexpr double kLockSeconds = 4.0;
constexpr double kMedianMs = 10.0;
constexpr double kP95Ms = 25.0;

// Mono track at `rate` plus the frames its beats fall on.
struct Track {
  uint32_t rate = 44100;
  std::vector<int16_t> mono;
  std::vector<double> beats;
};

uint32_t framesOf(double seconds, uint32_t rate) { return static_cast<uint32_t>(seconds * rate); }

Track clicks(uint32_t rate, float bpm, double seconds, float offsetBeats = 0.0f) {
  ClickGen g;
  ClickGen::Spec spec;
  spec.bpm = bpm;
  spec.offsetBeats = offsetBeats;
  const uint32_t n = framesOf(seconds, rate);
  g.start(rate, spec, n);
  std::vector<int16_t> stereo(2 * n);
  g.generate(stereo.data(), n);
  Track t;
  t.rate = rate;
  t.mono.resize(n);
  for (uint32_t i = 0; i < n; ++i) t.mono[i] = stereo[2 * i];  // both channels the same
  for (uint32_t k = 0; g.beatFrame(k) < n; ++k) t.beats.push_back(g.beatFrame(k));
  return t;
}

void addNoise(Track& t, double dbfs, uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 32768.0 * std::pow(10.0, dbfs / 20.0));
  for (auto& s : t.mono) {
    const double v = std::lround(s + gauss(rng)) * 1.0;
    s = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, v)));
  }
}

BeatTracker::Config configAt(uint32_t rate) {
  BeatTracker::Config c;
  c.sampleRate = rate;
  return c;
}

// How the tracker is fed: the audio itself, or a front end's hops, as
// numbers or after a trip through text (what the USB link carries), or
// with a little noise on every energy.
enum class Path { Audio, Hops, Text9, Text6, Noisy };

struct Feeder {
  Path path;
  HopFrontEnd fe;
  std::mt19937 rng{7};
  std::uniform_real_distribution<float> jitter{-1e-4f, 1e-4f};

  Feeder(Path p, uint32_t rate) : path(p) {
    const BeatTracker::Config c = configAt(rate);
    fe.begin(c.sampleRate, c.hop, c.decimation, c.lowpassHz);
  }
  float carry(float e) {
    char text[32];
    switch (path) {
      case Path::Text9:
        snprintf(text, sizeof(text), "%.9g", e);
        return std::strtof(text, nullptr);
      case Path::Text6:
        snprintf(text, sizeof(text), "%.6g", e);
        return std::strtof(text, nullptr);
      case Path::Noisy:
        return e * (1.0f + jitter(rng));
      default:
        return e;
    }
  }
  void feed(BeatTracker& bt, const int16_t* mono, uint32_t n) {
    if (path == Path::Audio) {
      bt.process(mono, n);
      return;
    }
    fe.process(mono, n, [&](float low, float mid) {
      const float l = carry(low);
      bt.feedHop(l, carry(mid));
    });
  }
};

struct Result {
  double lockSeconds = -1;
  std::vector<double> errorsMs;  // |phase error| at each true beat from `scoreFrom`, while locked
  std::vector<double> signedMs;
  int lockedBeats = 0, scoredBeats = 0;
  float bpm = 0;
};

// As test_beat_tracker's sig::run(), at the track's rate: fed in chunks,
// each true beat from `scoreFrom` seconds on scored against the grid as it
// stands when the audio up to that beat has been fed.
Result run(BeatTracker& bt, Feeder& f, const Track& t, double scoreFrom, uint32_t chunk = 1024) {
  Result r;
  size_t nextBeat = 0;
  for (uint32_t at = 0; at < t.mono.size(); at += chunk) {
    const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(t.mono.size()) - at);
    while (nextBeat < t.beats.size() && t.beats[nextBeat] < at + n) {
      const double beat = t.beats[nextBeat++];
      if (beat < scoreFrom * t.rate) continue;
      ++r.scoredBeats;
      if (!bt.locked()) continue;
      ++r.lockedBeats;
      const double truePeriod = nextBeat < t.beats.size() ? t.beats[nextBeat] - beat : beat - t.beats[nextBeat - 2];
      const double e = bt.grid().errorAgainst(beat, truePeriod) * 1000.0 / t.rate;
      r.signedMs.push_back(e);
      r.errorsMs.push_back(std::fabs(e));
    }
    f.feed(bt, t.mono.data() + at, n);
    if (bt.locked() && r.lockSeconds < 0) r.lockSeconds = static_cast<double>(at + n) / t.rate;
  }
  r.bpm = bt.bpm();
  return r;
}

double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 1e9;
  std::sort(v.begin(), v.end());
  const auto i = static_cast<size_t>(std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5)));
  return v[i];
}

uint32_t bitsOf(float f) {
  uint32_t u;
  std::memcpy(&u, &f, sizeof(u));
  return u;
}

// Everything the dancer reads from the tracker, bit for bit.
void expectSameTracker(const BeatTracker& a, const BeatTracker& b, const char* what, bool sameFed = true) {
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(a.bpm()), bitsOf(b.bpm()), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(a.confidence()), bitsOf(b.confidence()), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(a.estimatedBpm()), bitsOf(b.estimatedBpm()), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(a.tempoClarity()), bitsOf(b.tempoClarity()), what);
  TEST_ASSERT_EQUAL_MESSAGE(a.locked(), b.locked(), what);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(a.framesToLock(), b.framesToLock(), what);
  if (sameFed) TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.framesSinceReset(), b.framesSinceReset(), what);
  const BeatTracker::Grid ga = a.grid(), gb = b.grid();
  TEST_ASSERT_EQUAL_MESSAGE(ga.valid, gb.valid, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(ga.beatFrame, gb.beatFrame, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(ga.beatFrac), bitsOf(gb.beatFrac), what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(bitsOf(ga.periodFrames), bitsOf(gb.periodFrames), what);
  TEST_ASSERT_EQUAL_INT32_MESSAGE(ga.beatIndex, gb.beatIndex, what);
}

// Two trackers fed the same track, one each way, compared after every
// chunk (512 frames: after every hop).
int compareAlong(const Track& t, Path second, uint32_t chunk, float prior, const char* what) {
  BeatTracker a, b;
  TEST_ASSERT_TRUE(a.begin(configAt(t.rate)));
  TEST_ASSERT_TRUE(b.begin(configAt(t.rate)));
  a.setPrior(prior);
  b.setPrior(prior);
  Feeder fa(Path::Audio, t.rate), fb(second, t.rate);
  int lockedChunks = 0;
  for (uint32_t at = 0; at < t.mono.size(); at += chunk) {
    const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(t.mono.size()) - at);
    fa.feed(a, t.mono.data() + at, n);
    fb.feed(b, t.mono.data() + at, n);
    // (The audio path counts a hop's frames as they come; the hops path
    // whole hops. They agree whenever the audio fed ends on a hop.)
    expectSameTracker(a, b, what, (at + n) % 512 == 0);
    if (a.locked()) ++lockedChunks;
  }
  return lockedChunks;
}

void expectTracks(const Track& t, float bpm, Path path, const char* what, float prior = 0.0f) {
  BeatTracker bt;
  TEST_ASSERT_TRUE(bt.begin(configAt(t.rate)));
  bt.setPrior(prior);
  Feeder f(path, t.rate);
  const Result r = run(bt, f, t, kLockSeconds);
  const double med = percentile(r.errorsMs, 0.5), p95 = percentile(r.errorsMs, 0.95);
  char msg[200];
  snprintf(msg, sizeof(msg), "%s: lock %.2f s, bpm %.2f, median %.1f ms, p95 %.1f ms, locked %d/%d beats", what,
           r.lockSeconds, r.bpm, med, p95, r.lockedBeats, r.scoredBeats);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(r.lockSeconds >= 0.0 && r.lockSeconds <= kLockSeconds, msg);
  TEST_ASSERT_EQUAL_INT_MESSAGE(r.scoredBeats, r.lockedBeats, msg);
  TEST_ASSERT_TRUE_MESSAGE(med < kMedianMs, msg);
  TEST_ASSERT_TRUE_MESSAGE(p95 < kP95Ms, msg);
  const double octaves = std::log2(bt.bpm() / bpm);
  TEST_ASSERT_TRUE_MESSAGE(std::fabs(octaves - std::round(octaves)) < 0.01, msg);
}

// The click-train table (MASCOT-POC.md round 1; test_beat_tracker's).
constexpr float kBpms[] = {90.0f, 120.0f, 128.0f, 140.0f, 174.0f};
constexpr float kOffsets[] = {0.0f, 0.37f};

Track noisyClicks(uint32_t rate, float bpm, float offset, double seconds = 20.0) {
  Track t = clicks(rate, bpm, seconds, offset);
  addNoise(t, -30.0, static_cast<uint32_t>(bpm * 10 + offset * 100));
  return t;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the same tracker either way ----

// The click-train table at both rates: fed the hops of the same front end,
// the tracker is the same after every hop, bit for bit, as fed the audio.
void test_hops_give_the_same_tracker_as_audio() {
  for (uint32_t rate : {44100u, 48000u}) {
    for (float bpm : kBpms) {
      for (float offset : kOffsets) {
        const Track t = noisyClicks(rate, bpm, offset);
        char what[64];
        snprintf(what, sizeof(what), "%lu Hz, %.0f BPM, offset %.2f", static_cast<unsigned long>(rate), bpm, offset);
        const int locked = compareAlong(t, Path::Hops, 512, 0.0f, what);
        TEST_ASSERT_TRUE_MESSAGE(locked > 0, what);  // (a comparison of something that locks)
      }
    }
  }
}

// Any block size, and a prior: still the same (the front end's hop doesn't
// care how the audio is cut).
void test_hops_match_at_odd_block_sizes_and_with_a_prior() {
  const Track t = noisyClicks(44100, 174.0f, 0.37f);
  compareAlong(t, Path::Hops, 777, 0.0f, "777 frames per call");
  compareAlong(t, Path::Hops, 128, 87.0f, "128 frames per call, prior 87");
  const Track u = noisyClicks(48000, 128.0f, 0.37f);
  compareAlong(u, Path::Hops, 2048, 128.0f, "48 kHz, 2048 frames per call, prior 128");
}

// Through text with 9 significant digits (a float's round trip): exact.
void test_nine_digits_of_text_are_exact() {
  for (uint32_t rate : {44100u, 48000u}) {
    const Track t = noisyClicks(rate, 120.0f, 0.37f);
    compareAlong(t, Path::Text9, 512, 0.0f, "%.9g");
  }
}

// Through text with 6 significant digits (the protocol's least): the same
// lock to within a hop, and every beat's phase error within 0.5 ms of the
// exact path's.
void test_six_digits_of_text_are_close_enough() {
  for (uint32_t rate : {44100u, 48000u}) {
    for (float bpm : kBpms) {
      for (float offset : kOffsets) {
        const Track t = noisyClicks(rate, bpm, offset);
        BeatTracker exact, text;
        TEST_ASSERT_TRUE(exact.begin(configAt(rate)));
        TEST_ASSERT_TRUE(text.begin(configAt(rate)));
        Feeder fe(Path::Hops, rate), ft(Path::Text6, rate);
        const Result re = run(exact, fe, t, kLockSeconds), rt = run(text, ft, t, kLockSeconds);
        char msg[160];
        snprintf(msg, sizeof(msg), "%lu Hz, %.0f BPM, offset %.2f: lock at frame %ld / %ld", static_cast<unsigned long>(rate),
                 bpm, offset, static_cast<long>(exact.framesToLock()), static_cast<long>(text.framesToLock()));
        TEST_ASSERT_TRUE_MESSAGE(exact.framesToLock() >= 0 && text.framesToLock() >= 0, msg);
        TEST_ASSERT_TRUE_MESSAGE(std::abs(exact.framesToLock() - text.framesToLock()) <= 512, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(re.signedMs.size(), rt.signedMs.size(), msg);
        double worst = 0.0;
        for (size_t i = 0; i < re.signedMs.size(); ++i) worst = std::max(worst, std::fabs(re.signedMs[i] - rt.signedMs[i]));
        snprintf(msg + strlen(msg), sizeof(msg) - strlen(msg), ", phase within %.3f ms", worst);
        TEST_ASSERT_TRUE_MESSAGE(worst < 0.5, msg);
      }
    }
  }
}

// ---- the targets, fed the hops ----

// The click-train table at 48 kHz (the computer's other rate: the Core2's
// own audio only ever reaches the tracker at 44.1), fed the hops through
// text as the link carries them: lock within 4 s, then on the beat.
void test_click_trains_at_48k_through_text() {
  for (float bpm : kBpms) {
    for (float offset : kOffsets) {
      char what[64];
      snprintf(what, sizeof(what), "48 kHz, %.0f BPM, offset %.2f, %%.6g", bpm, offset);
      expectTracks(noisyClicks(48000, bpm, offset), bpm, Path::Text6, what);
    }
  }
  // A prior picks the octave at 48 kHz too.
  const Track fast = clicks(48000, 174.0f, 20.0, 0.37f);
  for (float prior : {87.0f, 174.0f}) {
    BeatTracker bt;
    TEST_ASSERT_TRUE(bt.begin(configAt(48000)));
    bt.setPrior(prior);
    Feeder f(Path::Text6, 48000);
    run(bt, f, fast, 0.0);
    TEST_ASSERT_FLOAT_WITHIN(prior * 0.006f, prior, bt.bpm());
  }
  expectTracks(fast, 174.0f, Path::Text6, "48 kHz, 174 with prior 87", 87.0f);
}

// The same at 44.1 kHz, through text.
void test_click_trains_at_44k_through_text() {
  for (float bpm : kBpms) {
    for (float offset : kOffsets) {
      char what[64];
      snprintf(what, sizeof(what), "44.1 kHz, %.0f BPM, offset %.2f, %%.6g", bpm, offset);
      expectTracks(noisyClicks(44100, bpm, offset), bpm, Path::Text6, what);
    }
  }
}

// 1e-4 relative noise on every energy (any port's float error is far
// below it): still within the targets.
void test_noise_on_the_energies_stays_within_the_targets() {
  for (uint32_t rate : {44100u, 48000u}) {
    for (float bpm : {90.0f, 128.0f, 174.0f}) {
      char what[64];
      snprintf(what, sizeof(what), "%lu Hz, %.0f BPM off-beat, 1e-4 noise", static_cast<unsigned long>(rate), bpm);
      expectTracks(noisyClicks(rate, bpm, 0.37f), bpm, Path::Noisy, what);
    }
  }
}

// ---- the sample rate ----

namespace {
size_t allocated = 0;
int blocks = 0;
int allocations = 0;
void* countingAlloc(size_t n) {
  allocated += n;
  ++blocks;
  ++allocations;
  return std::malloc(n);
}
void countingFree(void* p) {
  if (p) --blocks;
  std::free(p);
}
void* failingAlloc(size_t) { return nullptr; }
}  // namespace

// setSampleRate(): the tables and filters follow the rate (a 48 kHz track
// tracked right after a 44.1 kHz one, and back), the prior stays, nothing
// is allocated at the rate it has, and everything goes back through the hook.
void test_set_sample_rate() {
  {
    BeatTracker bt;
    TEST_ASSERT_TRUE(bt.begin(configAt(44100), countingAlloc, countingFree));
    const int before = allocations;
    TEST_ASSERT_TRUE(bt.setSampleRate(44100));
    TEST_ASSERT_EQUAL_INT(before, allocations);  // the same rate: nothing to do

    bt.setPrior(128.0f);
    TEST_ASSERT_TRUE(bt.setSampleRate(48000));
    TEST_ASSERT_TRUE(allocations > before);
    TEST_ASSERT_EQUAL_UINT32(48000, bt.config().sampleRate);
    TEST_ASSERT_EQUAL_FLOAT(128.0f, bt.prior());
    TEST_ASSERT_FALSE(bt.locked());
    TEST_ASSERT_EQUAL_UINT32(0, bt.framesSinceReset());

    // Tracked at 48 kHz as a tracker begun there would.
    const Track t = noisyClicks(48000, 128.0f, 0.37f, 12.0);
    BeatTracker fresh;
    TEST_ASSERT_TRUE(fresh.begin(configAt(48000)));
    fresh.setPrior(128.0f);
    Feeder fa(Path::Hops, 48000), fb(Path::Hops, 48000);
    for (uint32_t at = 0; at < t.mono.size(); at += 512) {
      const uint32_t n = std::min<uint32_t>(512, static_cast<uint32_t>(t.mono.size()) - at);
      fa.feed(bt, t.mono.data() + at, n);
      fb.feed(fresh, t.mono.data() + at, n);
      expectSameTracker(bt, fresh, "after setSampleRate(48000)");
    }
    TEST_ASSERT_TRUE(bt.locked());
    TEST_ASSERT_FLOAT_WITHIN(0.7f, 128.0f, bt.bpm());

    // And back: the audio path at 44.1 (the Core2's own audio after a session).
    TEST_ASSERT_TRUE(bt.setSampleRate(44100));
    bt.setPrior(0.0f);
    Feeder audio(Path::Audio, 44100);
    const Result r = run(bt, audio, noisyClicks(44100, 120.0f, 0.37f), kLockSeconds);
    TEST_ASSERT_TRUE(r.lockSeconds >= 0.0 && r.lockSeconds <= kLockSeconds);
    TEST_ASSERT_TRUE(percentile(r.errorsMs, 0.95) < kP95Ms);
  }
  TEST_ASSERT_EQUAL_INT(0, blocks);  // all freed through the hook

  // Out of memory: false, and it tracks nothing (no crash) until a later
  // call succeeds.
  BeatTracker none;
  TEST_ASSERT_FALSE(none.setSampleRate(48000));  // never begun
  TEST_ASSERT_FALSE(none.begin(configAt(44100), failingAlloc, countingFree));
  none.feedHop(1.0f, 1.0f);
  int16_t silence[512] = {};
  none.process(silence, 512);
  TEST_ASSERT_FALSE(none.grid().valid);
}

// feedHop() counts a hop of frames, so framesSinceReset() and the lock time
// read the same on both paths; reset() puts the grid at the caller's origin.
void test_feed_hop_counts_frames_and_keeps_the_origin() {
  BeatTracker bt;
  TEST_ASSERT_TRUE(bt.begin(configAt(44100)));
  bt.reset(512 * 1000);  // the computer's hop 1000: origin at frame 512000
  Feeder f(Path::Hops, 44100);
  const Track t = clicks(44100, 120.0f, 10.0, 0.25f);
  f.feed(bt, t.mono.data(), static_cast<uint32_t>(t.mono.size()));
  const uint32_t hops = static_cast<uint32_t>(t.mono.size()) / 512;
  TEST_ASSERT_EQUAL_UINT32(hops * 512, bt.framesSinceReset());
  TEST_ASSERT_TRUE(bt.locked());
  const BeatTracker::Grid g = bt.grid();
  const double beat10 = 512000.0 + 0.25 * 22050.0 + 10 * 22050.0;
  TEST_ASSERT_TRUE(std::fabs(g.errorAgainst(beat10, 22050.0)) < 0.010 * 44100);
}

// ---- the golden file ----

namespace {

struct GoldenSpec {
  const char* name;
  uint32_t rate;
  float bpm;
  float offset;
};
// Two 4 s click tracks, one at each rate.
constexpr GoldenSpec kGolden[] = {{"click120", 44100, 120.0f, 0.0f}, {"click120", 48000, 120.0f, 0.0f}};
constexpr double kGoldenSeconds = 4.0;

struct Computed {
  Track track;
  std::vector<float> low, mid;
  uint32_t fnv = 0;
  HopFrontEnd fe;
};

uint32_t fnv1a(const std::vector<int16_t>& mono) {
  uint32_t h = 2166136261u;
  for (int16_t s : mono) {
    const auto u = static_cast<uint16_t>(s);
    for (uint8_t byte : {static_cast<uint8_t>(u & 0xFF), static_cast<uint8_t>(u >> 8)}) {
      h ^= byte;
      h *= 16777619u;
    }
  }
  return h;
}

Computed compute(const GoldenSpec& g) {
  Computed c;
  c.track = clicks(g.rate, g.bpm, kGoldenSeconds, g.offset);
  const BeatTracker::Config cfg = configAt(g.rate);
  c.fe.begin(cfg.sampleRate, cfg.hop, cfg.decimation, cfg.lowpassHz);
  c.fe.process(c.track.mono.data(), static_cast<uint32_t>(c.track.mono.size()), [&](float lo, float mi) {
    c.low.push_back(lo);
    c.mid.push_back(mi);
  });
  c.fnv = fnv1a(c.track.mono);
  return c;
}

// A float as a C++ literal that reads back exactly: %.9g, with a point
// where it printed a whole number ("120" -> "120.0f").
struct Lit {
  char s[24];
  explicit Lit(float v) {
    snprintf(s, sizeof(s), "%.9g", v);
    if (!std::strpbrk(s, ".en")) std::strncat(s, ".0", sizeof(s) - std::strlen(s) - 1);
    std::strncat(s, "f", sizeof(s) - std::strlen(s) - 1);
  }
};

void writeGolden(const char* path) {
  FILE* f = std::fopen(path, "w");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
  std::fprintf(f,
               "// SPDX-License-Identifier: GPL-3.0-or-later\n"
               "// Copyright (C) 2026 IrosTheBeggar\n\n"
               "// GENERATED by test/test_hop_feed: don't edit. To write it again (only\n"
               "// after a deliberate change to lib/core/HopFrontEnd or lib/core/ClickGen):\n"
               "//   HOP_GOLDEN_OUT=test/test_hop_feed/hop_golden.h pio test -e native -f test_hop_feed\n"
               "// Otherwise the test checks the front end against it (relative 1e-5).\n"
               "//\n"
               "// The beat tracker's front end (lib/core/HopFrontEnd; docs/USB-VISUALIZER.md\n"
               "// \"The front end\") on two 4 s click tracks, for a port of it (the sender in\n"
               "// tools/, the terminal player) to check itself against:\n"
               "//   - the track: ClickGen tone:click<bpm>, -12 dBFS, both channels the same,\n"
               "//     so mono is the left channel; `frames` frames at `rate`;\n"
               "//     mono_fnv1a: FNV-1a (32 bit) over the mono int16 samples, each\n"
               "//     little-endian, to check a ClickGen port first; beats: its beat frames;\n"
               "//   - the front end at that rate (hop 512, decimation 8, low-pass 150 Hz)\n"
               "//     from zero state: its DC pole and biquad coefficients (float), and\n"
               "//     every hop's low and mid band energy (float, %%.9g: exact).\n"
               "// One `// track` line per track, then its arrays.\n"
               "#pragma once\n#include <cstdint>\n\nnamespace hopgolden {\n\n");
  std::fprintf(f,
               "struct Track {\n"
               "  const char* name;\n"
               "  uint32_t rate;\n"
               "  float bpm, offsetBeats;\n"
               "  uint32_t frames;\n"
               "  uint32_t monoFnv1a;\n"
               "  float dcPole;\n"
               "  float lp1[5], lp2[5];  // b0, b1, b2, a1, a2\n"
               "  const uint32_t* beats;\n"
               "  uint32_t beatCount;\n"
               "  const float (*hops)[2];  // low, mid\n"
               "  uint32_t hopCount;\n"
               "};\n\n");
  int i = 0;
  for (const GoldenSpec& g : kGolden) {
    const Computed c = compute(g);
    const HopFrontEnd::Biquad &a = c.fe.lowpass1(), &b = c.fe.lowpass2();
    std::fprintf(f,
                 "// track %d: name=%s rate=%lu bpm=%g offset=%g frames=%lu mono_fnv1a=0x%08lx hops=%lu beats=%lu "
                 "dc_pole=%.9g lp1=%.9g,%.9g,%.9g,%.9g,%.9g lp2=%.9g,%.9g,%.9g,%.9g,%.9g\n",
                 i, g.name, static_cast<unsigned long>(g.rate), g.bpm, g.offset,
                 static_cast<unsigned long>(c.track.mono.size()), static_cast<unsigned long>(c.fnv),
                 static_cast<unsigned long>(c.low.size()), static_cast<unsigned long>(c.track.beats.size()),
                 c.fe.dcPole(), a.b0, a.b1, a.b2, a.a1, a.a2, b.b0, b.b1, b.b2, b.a1, b.a2);
    std::fprintf(f, "inline constexpr uint32_t kBeats%d[] = {", i);
    for (size_t k = 0; k < c.track.beats.size(); ++k) {
      std::fprintf(f, "%s%lu", k ? ", " : "", static_cast<unsigned long>(c.track.beats[k]));
    }
    std::fprintf(f, "};\ninline constexpr float kHops%d[][2] = {\n", i);
    for (size_t k = 0; k < c.low.size(); ++k) {
      std::fprintf(f, "    {%.9gf, %.9gf},  // %lu\n", c.low[k], c.mid[k], static_cast<unsigned long>(k));
    }
    std::fprintf(f, "};\n\n");
    ++i;
  }
  std::fprintf(f, "inline constexpr Track kTracks[] = {\n");
  i = 0;
  for (const GoldenSpec& g : kGolden) {
    const Computed c = compute(g);
    const HopFrontEnd::Biquad &a = c.fe.lowpass1(), &b = c.fe.lowpass2();
    std::fprintf(f, "    {\"%s\", %lu, %s, %s, %lu, 0x%08lxu, %s,\n", g.name, static_cast<unsigned long>(g.rate),
                 Lit(g.bpm).s, Lit(g.offset).s, static_cast<unsigned long>(c.track.mono.size()),
                 static_cast<unsigned long>(c.fnv), Lit(c.fe.dcPole()).s);
    std::fprintf(f, "     {%s, %s, %s, %s, %s},\n", Lit(a.b0).s, Lit(a.b1).s, Lit(a.b2).s, Lit(a.a1).s, Lit(a.a2).s);
    std::fprintf(f, "     {%s, %s, %s, %s, %s},\n", Lit(b.b0).s, Lit(b.b1).s, Lit(b.b2).s, Lit(b.a1).s, Lit(b.a2).s);
    std::fprintf(f, "     kBeats%d, %lu, kHops%d, %lu},\n", i, static_cast<unsigned long>(c.track.beats.size()), i,
                 static_cast<unsigned long>(c.low.size()));
    ++i;
  }
  std::fprintf(f, "};\ninline constexpr int kTrackCount = %d;\n\n}  // namespace hopgolden\n", i);
  std::fclose(f);
  char msg[300];
  snprintf(msg, sizeof(msg), "wrote %s", path);
  TEST_MESSAGE(msg);
}

bool close(float got, float want) {
  const double d = std::fabs(static_cast<double>(got) - want);
  return d <= 1e-12 || d <= 1e-5 * std::fabs(static_cast<double>(want));
}

}  // namespace

// The front end against the checked-in golden file (or, with
// HOP_GOLDEN_OUT set, writes it).
void test_golden_file() {
  const char* out = std::getenv("HOP_GOLDEN_OUT");
  if (out && out[0]) {
    writeGolden(out);
    return;
  }
#ifdef HOP_GOLDEN_EMPTY
  TEST_FAIL_MESSAGE("hop_golden.h is a stub: HOP_GOLDEN_OUT=test/test_hop_feed/hop_golden.h pio test -e native -f test_hop_feed");
#else
  TEST_ASSERT_EQUAL_INT(sizeof(kGolden) / sizeof(kGolden[0]), hopgolden::kTrackCount);
  for (int i = 0; i < hopgolden::kTrackCount; ++i) {
    const hopgolden::Track& want = hopgolden::kTracks[i];
    const Computed c = compute(kGolden[i]);
    TEST_ASSERT_EQUAL_UINT32(want.rate, kGolden[i].rate);
    TEST_ASSERT_EQUAL_UINT32(want.frames, c.track.mono.size());
    TEST_ASSERT_EQUAL_HEX32(want.monoFnv1a, c.fnv);  // ClickGen made the same samples
    TEST_ASSERT_EQUAL_UINT32(want.beatCount, c.track.beats.size());
    for (uint32_t k = 0; k < want.beatCount; ++k) TEST_ASSERT_EQUAL_UINT32(want.beats[k], c.track.beats[k]);
    TEST_ASSERT_TRUE(close(c.fe.dcPole(), want.dcPole));
    const HopFrontEnd::Biquad &a = c.fe.lowpass1(), &b = c.fe.lowpass2();
    const float got1[5] = {a.b0, a.b1, a.b2, a.a1, a.a2}, got2[5] = {b.b0, b.b1, b.b2, b.a1, b.a2};
    for (int k = 0; k < 5; ++k) {
      TEST_ASSERT_TRUE(close(got1[k], want.lp1[k]));
      TEST_ASSERT_TRUE(close(got2[k], want.lp2[k]));
    }
    TEST_ASSERT_EQUAL_UINT32(want.hopCount, c.low.size());
    for (uint32_t k = 0; k < want.hopCount; ++k) {
      char msg[96];
      snprintf(msg, sizeof(msg), "track %d hop %lu: %.9g %.9g against %.9g %.9g", i, static_cast<unsigned long>(k),
               c.low[k], c.mid[k], want.hops[k][0], want.hops[k][1]);
      TEST_ASSERT_TRUE_MESSAGE(close(c.low[k], want.hops[k][0]) && close(c.mid[k], want.hops[k][1]), msg);
    }
  }
#endif
}

// The reference values printed in docs/USB-VISUALIZER.md ("The front end").
void test_reference_values_in_the_doc() {
  const Computed c44 = compute(kGolden[0]), c48 = compute(kGolden[1]);
  const auto near = [](float got, double want) { return std::fabs(got - want) <= 1e-5 * std::fabs(want) + 1e-12; };
  TEST_ASSERT_TRUE(near(c44.fe.dcPole(), 0.994300961));
  TEST_ASSERT_TRUE(near(c48.fe.dcPole(), 0.99476403));
  TEST_ASSERT_TRUE(near(c44.fe.lowpass1().b0, 0.00629974995));
  TEST_ASSERT_TRUE(near(c44.fe.lowpass1().a1, -1.70313001));
  TEST_ASSERT_TRUE(near(c44.fe.lowpass2().a2, 0.877744496));
  TEST_ASSERT_TRUE(near(c48.fe.lowpass1().a2, 0.747448981));
  TEST_ASSERT_TRUE(near(c48.fe.lowpass2().b1, 0.0116162719));
  const double low44[4] = {0.672406, 0.0291153, 0.00766506, 0.0036862};
  const double mid44[4] = {1.09918, 0.00515993, 5.37276e-05, 2.56443e-05};
  const double low48[4] = {0.712341, 0.0495953, 0.00879115, 0.00452538};
  const double mid48[4] = {1.19113, 0.0126538, 5.67129e-05, 3.15944e-05};
  const auto six = [](float got, double want) { return std::fabs(got - want) <= 1e-5 * std::fabs(want); };
  for (int k = 0; k < 4; ++k) {
    TEST_ASSERT_TRUE(six(c44.low[k], low44[k]));
    TEST_ASSERT_TRUE(six(c44.mid[k], mid44[k]));
    TEST_ASSERT_TRUE(six(c48.low[k], low48[k]));
    TEST_ASSERT_TRUE(six(c48.mid[k], mid48[k]));
  }
  TEST_ASSERT_TRUE(six(c44.low[43], 0.261903));
  TEST_ASSERT_TRUE(six(c44.mid[43], 0.43584));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_golden_file);
  RUN_TEST(test_reference_values_in_the_doc);
  RUN_TEST(test_hops_give_the_same_tracker_as_audio);
  RUN_TEST(test_hops_match_at_odd_block_sizes_and_with_a_prior);
  RUN_TEST(test_nine_digits_of_text_are_exact);
  RUN_TEST(test_six_digits_of_text_are_close_enough);
  RUN_TEST(test_click_trains_at_48k_through_text);
  RUN_TEST(test_click_trains_at_44k_through_text);
  RUN_TEST(test_noise_on_the_energies_stays_within_the_targets);
  RUN_TEST(test_set_sample_rate);
  RUN_TEST(test_feed_hop_counts_frames_and_keeps_the_origin);
  return UNITY_END();
}
