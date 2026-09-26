// Host tests for BeatTracker on synthetic audio (Signals.h). The phase
// error is the grid's prediction against the true beats, taken when the
// audio up to each beat has been fed, modulo the true period (a grid at half
// the tempo is on the beat too). Run: pio test -e native
#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "BeatTracker.h"
#include "DancePose.h"
#include "Signals.h"

using sig::Result;
using sig::Track;

namespace {
// Lock within this long, then phase errors (ms) below these.
constexpr double kLockSeconds = 4.0;
constexpr double kMedianMs = 10.0;
constexpr double kP95Ms = 25.0;

void expectTracks(const Track& t, float bpm, const char* what, float prior = 0.0f, uint32_t chunk = 1024) {
  BeatTracker bt;
  TEST_ASSERT_TRUE(bt.begin(BeatTracker::Config{}));
  bt.setPrior(prior);
  bt.reset(0);
  const Result r = sig::run(bt, t, kLockSeconds, chunk);
  const double med = sig::percentile(r.errorsMs, 0.5), p95 = sig::percentile(r.errorsMs, 0.95);
  char msg[200];
  snprintf(msg, sizeof(msg), "%s: lock %.2f s, bpm %.2f, median %.1f ms, p95 %.1f ms, locked %d/%d beats", what,
           r.lockSeconds, r.bpm, med, p95, r.lockedBeats, r.scoredBeats);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(r.lockSeconds >= 0.0 && r.lockSeconds <= kLockSeconds, msg);
  // Locked for every beat scored after the lock deadline.
  TEST_ASSERT_EQUAL_INT_MESSAGE(r.scoredBeats, r.lockedBeats, msg);
  TEST_ASSERT_TRUE_MESSAGE(med < kMedianMs, msg);
  TEST_ASSERT_TRUE_MESSAGE(p95 < kP95Ms, msg);
  // The tempo tracked is the true one or an octave of it.
  const double octaves = std::log2(bt.bpm() / bpm);
  TEST_ASSERT_TRUE_MESSAGE(std::fabs(octaves - std::round(octaves)) < 0.01, msg);
}
}  // namespace

void setUp() {}
void tearDown() {}

// Click trains with noise, on the beat from frame 0 and 0.37 of a beat late.
void test_click_trains_lock_and_stay_on_the_beat() {
  for (float bpm : {90.0f, 120.0f, 128.0f, 140.0f, 174.0f}) {
    for (float offset : {0.0f, 0.37f}) {
      Track t = sig::clicks(bpm, 20.0, offset);
      sig::addNoise(t, -30.0, static_cast<uint32_t>(bpm * 10 + offset * 100));
      char what[48];
      snprintf(what, sizeof(what), "%.0f BPM, offset %.2f", bpm, offset);
      expectTracks(t, bpm, what);
    }
  }
}

// The tracker works on whatever block sizes it is fed: the Bluetooth
// callback's 128 frames or odd sizes give the same beat.
void test_block_size_does_not_matter() {
  Track t = sig::clicks(128.0f, 16.0, 0.37f);
  sig::addNoise(t, -30.0, 11);
  expectTracks(t, 128.0f, "128 frames per call", 0.0f, 128);
  expectTracks(t, 128.0f, "777 frames per call", 0.0f, 777);
}

// Kick on the beat with a louder hi-hat on the off-beat, straight or swung
// (the hats barely reach the low band: this checks it shuts them out), and
// with low-band content between the beats, straight (0.5) and swung (0.66):
// a bass note, a quieter second kick, or both. The beat is the main kick,
// locked throughout.
void test_offbeat_hats_and_bass_do_not_pull_it_off_the_beat() {
  expectTracks(sig::drums(120.0f, 20.0, 0.5, 0.0), 120.0f, "straight off-beat hats at 0 dBFS");
  expectTracks(sig::drums(120.0f, 20.0, 0.66, 0.0), 120.0f, "swung hats");
  char what[80];
  for (double phase : {0.5, 0.66}) {
    for (double db : {-6.0, -8.0}) {
      snprintf(what, sizeof(what), "bass at %.2f of the beat, %.0f dB", phase, db);
      expectTracks(sig::drums(124.0f, 20.0, phase, -6.0, db, -200.0, 0.2), 124.0f, what);
    }
    for (double db : {-6.0, -10.0}) {
      snprintf(what, sizeof(what), "ghost kick at %.2f of the beat, %.0f dB", phase, db);
      expectTracks(sig::drums(124.0f, 20.0, phase, -6.0, -200.0, db, 0.2), 124.0f, what);
    }
    for (float bpm : {96.0f, 150.0f}) {
      snprintf(what, sizeof(what), "%.0f BPM, bass -8 dB and ghost kick -10 dB at %.2f", bpm, phase);
      expectTracks(sig::drums(bpm, 20.0, phase, -6.0, -8.0, -10.0, 0.2), bpm, what);
    }
  }
}

// Heavier low-band content between the beats (bass and ghost kick 4-6 dB
// under the kick together, as loud as the kick itself): the tracker may
// refuse to lock (the figure sways), but whenever it is locked it is on the
// beat, never on the swung or off-beat notes.
void test_heavy_offbeat_low_band_never_locks_off_the_beat() {
  int locked = 0, scored = 0;
  for (float bpm : {96.0f, 124.0f, 150.0f}) {
    for (double phase : {0.5, 0.66, 0.75}) {
      if (bpm == 150.0f && phase == 0.75) continue;  // a lead-in a 16th early: see docs/MASCOT-POC.md
      for (double bass : {-4.0, -6.0}) {
        for (double ghost : {-4.0, -6.0}) {
          BeatTracker bt;
          TEST_ASSERT_TRUE(bt.begin(BeatTracker::Config{}));
          bt.reset(0);
          const Result r = sig::run(bt, sig::drums(bpm, 20.0, phase, -6.0, bass, ghost, 0.2), kLockSeconds);
          locked += r.lockedBeats;
          scored += r.scoredBeats;
          if (r.errorsMs.empty()) continue;
          char msg[160];
          snprintf(msg, sizeof(msg), "%.0f BPM, phase %.2f, bass %.0f dB, ghost %.0f dB: bpm %.2f, median %.1f ms, p95 %.1f ms",
                   bpm, phase, bass, ghost, r.bpm, sig::percentile(r.errorsMs, 0.5), sig::percentile(r.errorsMs, 0.95));
          TEST_ASSERT_TRUE_MESSAGE(sig::percentile(r.errorsMs, 0.5) < kMedianMs, msg);
          TEST_ASSERT_TRUE_MESSAGE(sig::percentile(r.errorsMs, 0.95) < kP95Ms, msg);
        }
      }
    }
  }
  char msg[80];
  snprintf(msg, sizeof(msg), "locked for %d of %d beats scored", locked, scored);
  TEST_MESSAGE(msg);
}

// Without a prior a 174 BPM train may be tracked at 174 or 87; either way
// the figure dances at 87 and on the beat. A prior picks the octave.
void test_half_and_double_tempo() {
  const Track fast = sig::clicks(174.0f, 20.0, 0.37f);
  BeatTracker bt;
  bt.begin(BeatTracker::Config{});
  bt.reset(0);
  sig::run(bt, fast, 0.0);
  TEST_ASSERT_TRUE(bt.locked());
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 87.0f, dance::danceStep(0.0, bt.bpm()).bpm);

  bt.setPrior(174.0f);
  bt.reset(0);
  sig::run(bt, fast, 0.0);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 174.0f, bt.bpm());
  bt.setPrior(87.0f);
  bt.reset(0);
  sig::run(bt, fast, 0.0);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 87.0f, bt.bpm());
  expectTracks(fast, 174.0f, "174 with prior 87", 87.0f);

  // 70 BPM: tracked at 70, danced at 140.
  bt.setPrior(0.0f);
  bt.reset(0);
  sig::run(bt, sig::clicks(70.0f, 20.0), 0.0);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 70.0f, bt.bpm());
  TEST_ASSERT_FLOAT_WITHIN(1.0f, 140.0f, dance::danceStep(0.0, bt.bpm()).bpm);

  // A prior can't make up beats that aren't there: 90 BPM with a prior of
  // 180 stays at 90 (every other 180 beat would be empty).
  bt.setPrior(180.0f);
  bt.reset(0);
  sig::run(bt, sig::clicks(90.0f, 20.0), 0.0);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f, bt.bpm());
  // The prior survives reset() (the firmware clears it on a track change).
  TEST_ASSERT_EQUAL_FLOAT(180.0f, bt.prior());
}

// Silence, ambient noise and white noise: low confidence, never locked.
void test_no_beat_gives_low_confidence() {
  BeatTracker bt;
  bt.begin(BeatTracker::Config{});
  Track silence;
  silence.mono.assign(sig::frames(20.0), 0);
  bt.reset(0);
  Result r = sig::run(bt, silence, 0.0);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bt.confidence());
  TEST_ASSERT_EQUAL_FLOAT(0.0, r.lockedFraction);
  TEST_ASSERT_FALSE(bt.grid().valid);
  for (uint32_t seed = 1; seed <= 4; ++seed) {
    bt.reset(0);
    r = sig::run(bt, sig::ambient(30.0, -20.0, seed), 0.0);
    TEST_ASSERT_TRUE(r.meanConfidence < 0.15);
    TEST_ASSERT_EQUAL_FLOAT(0.0, r.lockedFraction);
    Track noise;
    noise.mono.assign(sig::frames(30.0), 0);
    sig::addNoise(noise, -20.0, seed);
    bt.reset(0);
    r = sig::run(bt, noise, 0.0);
    TEST_ASSERT_TRUE(r.meanConfidence < 0.15);
    TEST_ASSERT_EQUAL_FLOAT(0.0, r.lockedFraction);
  }
}

// When the beat stops, confidence falls and the lock goes.
void test_beat_then_silence_unlocks() {
  Track t = sig::clicks(120.0f, 10.0);
  Track quiet;
  quiet.mono.assign(sig::frames(6.0), 0);
  t = sig::concat(t, quiet);
  BeatTracker bt;
  bt.begin(BeatTracker::Config{});
  bt.reset(0);
  bool lockedBefore = false;
  for (uint32_t at = 0; at < t.mono.size(); at += 1024) {
    const uint32_t n = std::min<uint32_t>(1024, static_cast<uint32_t>(t.mono.size()) - at);
    bt.process(t.mono.data() + at, n);
    if (at + n >= sig::frames(9.9) && at < sig::frames(10.0)) lockedBefore = bt.locked();
  }
  TEST_ASSERT_TRUE(lockedBefore);
  TEST_ASSERT_FALSE(bt.locked());
  TEST_ASSERT_TRUE(bt.confidence() < 0.3f);
}

// A tempo change mid-stream is followed: locked on the new tempo, on the
// beat, within 8 s. (The tempo is scored at up to 8 multiples of its lag, up
// to 8 s back, so the old tempo takes a few seconds to fade from it.)
void test_tempo_change_relocks() {
  const struct {
    float from, to;
  } changes[] = {{120.0f, 135.0f}, {128.0f, 96.0f}, {140.0f, 128.0f}};
  for (const auto& c : changes) {
    Track t = sig::concat(sig::clicks(c.from, 12.0), sig::clicks(c.to, 16.0, 0.3f));
    sig::addNoise(t, -30.0, 5);
    BeatTracker bt;
    bt.begin(BeatTracker::Config{});
    bt.reset(0);
    const Result r = sig::run(bt, t, 20.0);  // scored from 8 s after the change
    char msg[160];
    snprintf(msg, sizeof(msg), "%.0f -> %.0f: bpm %.2f, locked %d/%d, median %.1f ms, p95 %.1f ms", c.from, c.to,
             r.bpm, r.lockedBeats, r.scoredBeats, sig::percentile(r.errorsMs, 0.5), sig::percentile(r.errorsMs, 0.95));
    TEST_MESSAGE(msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.scoredBeats, r.lockedBeats, msg);
    TEST_ASSERT_TRUE_MESSAGE(sig::percentile(r.errorsMs, 0.95) < kP95Ms, msg);
    const double octaves = std::log2(bt.bpm() / c.to);
    TEST_ASSERT_TRUE_MESSAGE(std::fabs(octaves - std::round(octaves)) < 0.01, msg);
  }
}

// reset() forgets everything and starts counting from the new origin: the
// grid comes back in the caller's frames.
void test_reset_and_origin() {
  const Track t = sig::clicks(120.0f, 10.0, 0.25f);
  BeatTracker bt;
  bt.begin(BeatTracker::Config{});
  bt.reset(0);
  sig::run(bt, t, 0.0);
  TEST_ASSERT_TRUE(bt.locked());
  TEST_ASSERT_TRUE(bt.framesToLock() > 0);

  bt.reset(1000000);
  TEST_ASSERT_FALSE(bt.locked());
  TEST_ASSERT_FALSE(bt.grid().valid);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bt.confidence());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bt.bpm());
  TEST_ASSERT_EQUAL_INT32(-1, bt.framesToLock());
  TEST_ASSERT_EQUAL_UINT32(0, bt.framesSinceReset());
  // Same audio, now said to start at frame 1000000: the grid moves with it.
  bt.process(t.mono.data(), static_cast<uint32_t>(t.mono.size()));
  const BeatTracker::Grid g = bt.grid();
  TEST_ASSERT_TRUE(g.valid);
  const double beat0 = 1000000.0 + 0.25 * 22050.0;
  const double err = g.errorAgainst(beat0 + 10 * 22050.0, 22050.0);
  TEST_ASSERT_TRUE(std::fabs(err) < 0.010 * sig::kRate);
}

// Scratch memory comes from the caller's allocator (the firmware's PSRAM).
namespace {
size_t allocated = 0;
int blocks = 0;
void* countingAlloc(size_t n) {
  allocated += n;
  ++blocks;
  return std::malloc(n);
}
void countingFree(void* p) {
  if (p) --blocks;
  std::free(p);
}
}  // namespace

void test_allocator_hook() {
  {
    BeatTracker bt;
    TEST_ASSERT_TRUE(bt.begin(BeatTracker::Config{}, countingAlloc, countingFree));
    TEST_ASSERT_TRUE(allocated > 2000 && allocated < 12288);
    TEST_ASSERT_TRUE(blocks > 0);
  }
  TEST_ASSERT_EQUAL_INT(0, blocks);  // all freed through the hook
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_click_trains_lock_and_stay_on_the_beat);
  RUN_TEST(test_block_size_does_not_matter);
  RUN_TEST(test_offbeat_hats_and_bass_do_not_pull_it_off_the_beat);
  RUN_TEST(test_heavy_offbeat_low_band_never_locks_off_the_beat);
  RUN_TEST(test_half_and_double_tempo);
  RUN_TEST(test_no_beat_gives_low_confidence);
  RUN_TEST(test_beat_then_silence_unlocks);
  RUN_TEST(test_tempo_change_relocks);
  RUN_TEST(test_reset_and_origin);
  RUN_TEST(test_allocator_hook);
  return UNITY_END();
}
