// Host test of the Bluetooth output chain as BtSink::onData composes it: the
// DeclickReader (pause/skip/underrun/output-switch fades) and then the
// GainRamp (volume), 128 frames per call; and the speaker pump's amp gate
// (AmpGate: the amp and I2S off 2 s after the speaker goes quiet).
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

#include "AmpGate.h"
#include "DeclickReader.h"
#include "GainRamp.h"
#include "PcmRing.h"
#include "VolumeMath.h"

namespace {
constexpr uint8_t kBt = 1;
constexpr uint8_t kSpeaker = 2;
constexpr uint32_t kCall = 128;  // frames per data callback

struct Chain {
  std::vector<int16_t> buf = std::vector<int16_t>(4096 * 2);
  PcmRing ring{buf.data(), 4096};
  DeclickReader reader{kBt};
  GainRamp gain{vol::kHeadroomQ15};
  Chain() {
    ring.setConsumer(kBt);
    reader.bind(ring);
  }
  // One data callback, as onData does it.
  std::vector<int16_t> pull(bool playing, bool restart) {
    if (restart) {
      gain.restart();
      reader.reset();
    }
    std::vector<int16_t> out(kCall * 2, 12345);  // garbage: all of it must be overwritten
    reader.fill(out.data(), kCall, playing);
    gain.process(out.data(), kCall);
    return out;
  }
};
}  // namespace

void setUp() {}
void tearDown() {}

// Whatever happens, the chain never makes anything louder than what went in:
// both stages multiply by at most 1, and a crossfade's two gains add up to 1.
void test_never_adds_level() {
  constexpr int kAmp = 30000;
  std::mt19937 rng(7);
  Chain c;
  bool playing = true;
  for (int i = 0; i < 40000; ++i) {
    switch (rng() % 12) {
      case 0: playing = !playing; break;
      case 1: c.ring.discardAll(); break;                                    // skip
      case 2: c.ring.setConsumer(rng() % 2 ? kBt : kSpeaker); break;         // output switch
      case 3: c.gain.request(static_cast<uint16_t>(rng() % 32769), rng() % 3 == 0); break;
      default: {
        std::vector<int16_t> in(2 * (rng() % 300));
        for (auto& s : in) s = static_cast<int16_t>(static_cast<int>(rng() % (2 * kAmp + 1)) - kAmp);
        c.ring.write(in.data(), static_cast<uint32_t>(in.size() / 2));
        break;
      }
    }
    const auto out = c.pull(playing, rng() % 50 == 0);
    for (int16_t s : out) TEST_ASSERT_TRUE(std::abs(static_cast<int>(s)) <= kAmp);
  }
}

// A restarted stream (the listener heard silence) starts from silence and
// fades in, whatever the level before.
void test_restart_starts_from_silence() {
  Chain c;
  std::vector<int16_t> in(2 * 4000, 32000);
  c.ring.write(in.data(), 4000);
  c.pull(true, false);
  c.pull(true, false);
  const auto out = c.pull(true, true);
  TEST_ASSERT_TRUE(std::abs(static_cast<int>(out[0])) <= 32000 / 64 + 1);
  for (uint32_t i = 1; i < kCall; ++i) TEST_ASSERT_TRUE(out[2 * i] >= out[2 * (i - 1)]);  // only rises
}

// Paused, only the fade-out is taken from the ring; the rest waits for resume.
void test_pause_keeps_the_ring_and_is_silent() {
  Chain c;
  std::vector<int16_t> in(2 * 2000, 20000);
  c.ring.write(in.data(), 2000);
  c.pull(true, false);
  const uint32_t before = c.ring.size();
  auto out = c.pull(false, false);
  TEST_ASSERT_EQUAL_INT16(0, out[2 * (kCall - 1)]);
  out = c.pull(false, false);
  for (int16_t s : out) TEST_ASSERT_EQUAL_INT16(0, s);
  TEST_ASSERT_TRUE(before - c.ring.size() <= Declicker::kDefaultRampFrames);
}


// ---- the speaker amp (AmpGate), as SpeakerSink's pump drives it ----

namespace {
// The pump's side: M5.Speaker running or not, and what the gate made it do.
struct Pump {
  AmpGate gate;
  bool running = false;
  int starts = 0, stops = 0;
  void apply(AmpGate::Do d) {
    if (d == AmpGate::Do::Start) {
      TEST_ASSERT_FALSE(running);
      running = true;
      ++starts;
    } else if (d == AmpGate::Do::Stop) {
      TEST_ASSERT_TRUE(running);
      running = false;
      ++stops;
    }
  }
  // A buffer of audio queued at `t` (the amp on first if it was off).
  void queue(uint32_t t) {
    apply(gate.beforeQueue(running));
    TEST_ASSERT_TRUE(running);
    gate.queued(t);
  }
  // Nothing to queue at `t` (paused, stopped, or the output is Bluetooth).
  void quiet(uint32_t t, bool drained = true) { apply(gate.quiet(t, running, drained)); }
  // Play for `ms` from `t`, a buffer every 23 ms with quiet passes between
  // them (the ring busy, a short wait): never switched off meanwhile.
  uint32_t play(uint32_t t, uint32_t ms) {
    for (uint32_t end = t + ms; t != end; ++t) {
      if (t % 23 == 0) {
        queue(t);
      } else if (t % 10 == 0) {
        quiet(t, false);
      }
    }
    return t;
  }
};
}  // namespace

// At boot the amp is off, and quiet passes leave it so.
void test_amp_stays_off_until_audio() {
  Pump p;
  for (uint32_t t = 0; t < 10000; t += 10) p.quiet(t);
  TEST_ASSERT_FALSE(p.running);
  TEST_ASSERT_EQUAL(0, p.starts + p.stops);
}

// Paused (or stopped, or moved to Bluetooth: all the same to the pump), it
// goes off 2 s after the last buffer was queued, not a pass sooner; and on
// again before the next buffer.
void test_amp_off_two_seconds_after_the_speaker_goes_quiet() {
  Pump p;
  uint32_t t = p.play(1000, 5000);
  TEST_ASSERT_EQUAL(1, p.starts);  // on before the first buffer
  TEST_ASSERT_EQUAL(AmpGate::Why::Playing, p.gate.why());
  const uint32_t last = t - 1 - (t - 1) % 23;  // the last buffer queued
  for (; t - last < AmpGate::kQuietMs; t += 10) {
    p.quiet(t);
    TEST_ASSERT_TRUE(p.running);
  }
  p.quiet(last + AmpGate::kQuietMs);
  TEST_ASSERT_FALSE(p.running);
  TEST_ASSERT_EQUAL(1, p.stops);
  TEST_ASSERT_EQUAL(AmpGate::Why::Quiet, p.gate.why());
  for (t = last + AmpGate::kQuietMs; t < last + 60000; t += 10) p.quiet(t);  // stays off
  TEST_ASSERT_EQUAL(1, p.stops);
  p.queue(t);  // resume
  TEST_ASSERT_EQUAL(2, p.starts);
  TEST_ASSERT_EQUAL(AmpGate::Why::Playing, p.gate.why());
  // A pause shorter than 2 s never switches it.
  t = p.play(t, 3000);
  for (uint32_t end = t + 1900; t < end; t += 10) p.quiet(t);
  p.play(t, 1000);
  TEST_ASSERT_EQUAL(2, p.starts);
  TEST_ASSERT_EQUAL(1, p.stops);
}

// A buffer still queued (not released yet) holds it on: end() must not drop
// one. It goes off at the first pass once all are back.
void test_amp_waits_for_the_last_buffer() {
  Pump p;
  p.queue(0);
  p.quiet(AmpGate::kQuietMs + 500, false);
  TEST_ASSERT_TRUE(p.running);
  p.quiet(AmpGate::kQuietMs + 510, true);
  TEST_ASSERT_FALSE(p.running);
}

// Pa0: off as soon as the speaker is quiet, without the 2 s wait; while it
// plays the request waits. With the amp already off it is only cleared.
void test_amp_pa0_switches_off_once_quiet() {
  Pump p;
  uint32_t t = p.play(0, 1000);
  p.gate.ask(AmpGate::Ask::Off);
  p.quiet(t, false);  // still playing out what was queued
  TEST_ASSERT_TRUE(p.running);
  TEST_ASSERT_TRUE(p.gate.asking());
  p.quiet(t + 10);
  TEST_ASSERT_FALSE(p.running);
  TEST_ASSERT_FALSE(p.gate.asking());
  TEST_ASSERT_EQUAL(AmpGate::Why::Asked, p.gate.why());
  p.gate.ask(AmpGate::Ask::Off);
  p.quiet(t + 20);
  TEST_ASSERT_FALSE(p.gate.asking());
  TEST_ASSERT_EQUAL(1, p.stops);
}

// Pa1: on (zeros) and held on through playing and pausing, until Pa0.
void test_amp_pa1_holds_it_on_until_pa0() {
  Pump p;
  p.gate.ask(AmpGate::Ask::On);
  p.quiet(0);
  TEST_ASSERT_TRUE(p.running);
  TEST_ASSERT_TRUE(p.gate.held());
  TEST_ASSERT_EQUAL(AmpGate::Why::Asked, p.gate.why());
  uint32_t t = 10;
  for (; t < 10000; t += 10) p.quiet(t);
  t = p.play(t, 2000);
  for (uint32_t end = t + 10000; t < end; t += 10) p.quiet(t);
  TEST_ASSERT_TRUE(p.running);
  TEST_ASSERT_EQUAL(1, p.starts);
  p.gate.ask(AmpGate::Ask::Off);
  p.quiet(t);
  TEST_ASSERT_FALSE(p.running);
  TEST_ASSERT_FALSE(p.gate.held());
  // Back to the automatic gate.
  t = p.play(t + 10, 1000);
  p.quiet(t + AmpGate::kQuietMs);
  TEST_ASSERT_FALSE(p.running);
  TEST_ASSERT_EQUAL(2, p.stops);
  // Pa1 with it already on only holds it.
  t = p.play(t + AmpGate::kQuietMs + 10, 1000);
  p.gate.ask(AmpGate::Ask::On);
  p.quiet(t);
  TEST_ASSERT_EQUAL(3, p.starts);
  p.quiet(t + 60000);
  TEST_ASSERT_TRUE(p.running);
}

// millis() wraps after ~49.7 days: the 2 s still counts across it.
void test_amp_quiet_across_the_clock_wrap() {
  Pump p;
  const uint32_t t0 = 0xFFFFFFFFu - 500;
  p.queue(t0);
  p.quiet(t0 + 1000);  // wrapped
  TEST_ASSERT_TRUE(p.running);
  p.quiet(t0 + AmpGate::kQuietMs);
  TEST_ASSERT_FALSE(p.running);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_never_adds_level);
  RUN_TEST(test_restart_starts_from_silence);
  RUN_TEST(test_pause_keeps_the_ring_and_is_silent);
  RUN_TEST(test_amp_stays_off_until_audio);
  RUN_TEST(test_amp_off_two_seconds_after_the_speaker_goes_quiet);
  RUN_TEST(test_amp_waits_for_the_last_buffer);
  RUN_TEST(test_amp_pa0_switches_off_once_quiet);
  RUN_TEST(test_amp_pa1_holds_it_on_until_pa0);
  RUN_TEST(test_amp_quiet_across_the_clock_wrap);
  return UNITY_END();
}
