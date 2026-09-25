// Host test of the Bluetooth output chain as BtSink::onData composes it: the
// DeclickReader (pause/skip/underrun/output-switch fades) and then the
// GainRamp (volume), 128 frames per call. Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <random>
#include <vector>

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_never_adds_level);
  RUN_TEST(test_restart_starts_from_silence);
  RUN_TEST(test_pause_keeps_the_ring_and_is_silent);
  return UNITY_END();
}
