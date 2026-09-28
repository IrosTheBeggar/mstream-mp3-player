// Host tests for the output tap: PcmRing/DeclickReader reporting where the
// frames they read sit in the track, AudioTap's history and segments, and
// TapReader's runs and audible-time clock. Run: pio test -e native
#include <unity.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "AudioTap.h"
#include "DeclickReader.h"
#include "PcmRing.h"
#include "TapReader.h"

namespace {
constexpr uint8_t kBt = 1;
constexpr uint32_t kRate = 44100;

std::vector<int16_t> stereoRamp(int from, int count) {
  std::vector<int16_t> v;
  for (int i = from; i < from + count; ++i) {
    v.push_back(static_cast<int16_t>(i));
    v.push_back(static_cast<int16_t>(i + 2));  // mono = i + 1
  }
  return v;
}

struct Tap {
  std::vector<int16_t> buf = std::vector<int16_t>(1024);
  AudioTap tap{buf.data(), 1024};
};
}  // namespace

void setUp() {}
void tearDown() {}

// ---- PcmRing / DeclickReader positions ----

// A read reports its first frame's position in the epoch: the count
// positionMs() is made of, restarting at every discardAll().
void test_ring_reports_position_in_epoch() {
  std::vector<int16_t> buf(256 * 2);
  PcmRing ring(buf.data(), 256, 0xFFFFFF00u);  // near the wrap
  ring.setConsumer(kBt);
  auto in = stereoRamp(0, 100);
  ring.write(in.data(), 100);
  int16_t out[2 * 64];
  uint32_t epoch = 99, pos = 99;
  TEST_ASSERT_EQUAL_UINT32(40, ring.read(kBt, out, 40, &epoch, &pos));
  TEST_ASSERT_EQUAL_UINT32(0, epoch);
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  ring.read(kBt, out, 30, &epoch, &pos);
  TEST_ASSERT_EQUAL_UINT32(40, pos);
  const uint32_t start = ring.discardAll();
  ring.write(in.data(), 100);
  ring.read(kBt, out, 10, &epoch, &pos);
  TEST_ASSERT_EQUAL_UINT32(1, epoch);
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  ring.read(kBt, out, 10, &epoch, &pos);
  TEST_ASSERT_EQUAL_UINT32(10, pos);
  TEST_ASSERT_EQUAL_UINT32(20, ring.readPos() - start);  // what positionMs() uses
}

void test_declick_reader_passes_position_through() {
  std::vector<int16_t> buf(256 * 2);
  PcmRing ring(buf.data(), 256);
  ring.setConsumer(kBt);
  DeclickReader reader(kBt);
  reader.bind(ring);
  auto in = stereoRamp(0, 200);
  ring.write(in.data(), 200);
  int16_t out[2 * 128];
  DeclickReader::Result r = reader.fill(out, 128, true);
  TEST_ASSERT_EQUAL_UINT32(128, r.read);
  TEST_ASSERT_EQUAL_UINT32(0, r.position);
  r = reader.fill(out, 128, true);
  TEST_ASSERT_EQUAL_UINT32(72, r.read);  // then ran dry
  TEST_ASSERT_EQUAL_UINT32(128, r.position);
  ring.discardAll();
  ring.write(in.data(), 50);
  r = reader.fill(out, 128, true);
  TEST_ASSERT_EQUAL_UINT32(1, r.epoch);
  TEST_ASSERT_EQUAL_UINT32(0, r.position);
}

// ---- AudioTap ----

void test_tap_stores_mono_history() {
  Tap t;
  auto in = stereoRamp(0, 300);
  t.tap.write(in.data(), 300, 300, 0, 0, 1000);
  TEST_ASSERT_EQUAL_UINT32(300, t.tap.count());
  int16_t out[300];
  TEST_ASSERT_TRUE(t.tap.read(0, out, 300));
  for (int i = 0; i < 300; ++i) TEST_ASSERT_EQUAL_INT16(i + 1, out[i]);
  uint32_t c, us;
  t.tap.clock(&c, &us);
  TEST_ASSERT_EQUAL_UINT32(300, c);
  TEST_ASSERT_EQUAL_UINT32(1000, us);
  // Lapped: frames older than the capacity are gone.
  for (int k = 0; k < 4; ++k) t.tap.write(in.data(), 300, 300, 0, 300 + 300 * k, 2000);
  TEST_ASSERT_FALSE(t.tap.read(0, out, 10));
  TEST_ASSERT_TRUE(t.tap.read(t.tap.count() - 1024, out, 10));
}

void test_tap_mono_is_the_average() {
  Tap t;
  const int16_t in[] = {32767, 32767, -32768, -32768, 32767, -32768, 1000, 3000};
  t.tap.write(in, 4, 4, 0, 0, 0);
  int16_t out[4];
  t.tap.read(0, out, 4);
  TEST_ASSERT_EQUAL_INT16(32767, out[0]);
  TEST_ASSERT_EQUAL_INT16(-32768, out[1]);
  TEST_ASSERT_EQUAL_INT16(-1, out[2]);
  TEST_ASSERT_EQUAL_INT16(2000, out[3]);
}

// Segments: continuous audio is one; a fade tail, a skip (new epoch) or a
// gap in the track position starts a new one.
void test_tap_segments_follow_the_track() {
  Tap t;
  auto in = stereoRamp(0, 128);
  t.tap.write(in.data(), 128, 128, 3, 1000, 0);  // track 1000..1127
  t.tap.write(in.data(), 128, 128, 3, 1128, 0);  // continues
  AudioTap::Segment s;
  TEST_ASSERT_TRUE(t.tap.segmentAt(200, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(0, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(256, s.tapEnd);
  TEST_ASSERT_EQUAL_UINT32(1000, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(3, s.epoch);

  t.tap.write(in.data(), 128, 50, 3, 1256, 0);  // underrun: 50 real, then fade/zeros
  t.tap.write(in.data(), 128, 0, 3, 0, 0);      // silence
  t.tap.write(in.data(), 128, 128, 3, 1306, 0); // back, the track continues
  t.tap.write(in.data(), 128, 128, 4, 0, 0);    // skip: new epoch from 0
  const uint32_t limit = t.tap.count();
  TEST_ASSERT_TRUE(t.tap.segmentAt(256 + 10, limit, &s));
  TEST_ASSERT_EQUAL_UINT32(0, s.tapStart);  // the 50 real frames still belong to the first
  TEST_ASSERT_EQUAL_UINT32(306, s.tapEnd);
  TEST_ASSERT_TRUE(t.tap.segmentAt(306, limit, &s));
  TEST_ASSERT_EQUAL_UINT32(AudioTap::kNoTrack, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(512, s.tapEnd);
  TEST_ASSERT_TRUE(t.tap.segmentAt(600, limit, &s));
  TEST_ASSERT_EQUAL_UINT32(512, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(1306, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(640, s.tapEnd);
  TEST_ASSERT_TRUE(t.tap.segmentAt(700, limit, &s));
  TEST_ASSERT_EQUAL_UINT32(4, s.epoch);
  TEST_ASSERT_EQUAL_UINT32(0, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(limit, s.tapEnd);
}

// Switched off (the Dance tab not up), a write copies nothing and the count
// stands still; switched back on, the next write starts a new segment even
// where the track position follows on, and a reader gets it as its own run.
void test_tap_switched_off_skips_and_starts_a_new_segment() {
  Tap t;
  auto in = stereoRamp(0, 128);
  t.tap.write(in.data(), 128, 128, 3, 1000, 10);  // track 1000..1127
  TapReader reader;
  reader.attach(&t.tap, kRate);
  t.tap.setEnabled(false);
  TEST_ASSERT_FALSE(t.tap.enabled());
  t.tap.write(in.data(), 128, 128, 3, 1128, 20);  // played, not copied
  TEST_ASSERT_EQUAL_UINT32(128, t.tap.count());
  uint32_t c, us;
  t.tap.clock(&c, &us);
  TEST_ASSERT_EQUAL_UINT32(10, us);  // not touched either
  t.tap.setEnabled(true);
  t.tap.write(in.data(), 128, 128, 3, 1128, 30);  // the position follows on, as if paused meanwhile
  TEST_ASSERT_EQUAL_UINT32(256, t.tap.count());
  AudioTap::Segment s;
  TEST_ASSERT_TRUE(t.tap.segmentAt(10, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(0, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(128, s.tapEnd);  // the old one ends where the tap was off
  TEST_ASSERT_TRUE(t.tap.segmentAt(128, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(128, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(1128, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(3, s.epoch);
  TEST_ASSERT_EQUAL_UINT32(256, s.tapEnd);

  // Off and on around silence: the silence carries on, the audio after it is new.
  t.tap.setEnabled(false);
  t.tap.write(in.data(), 128, 0, 3, 0, 40);
  t.tap.setEnabled(true);
  t.tap.write(in.data(), 128, 0, 3, 0, 50);       // silence: a segment of its own after the audio
  t.tap.write(in.data(), 128, 128, 3, 1256, 60);  // audio again
  TEST_ASSERT_TRUE(t.tap.segmentAt(300, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(AudioTap::kNoTrack, s.trackStart);
  TEST_ASSERT_EQUAL_UINT32(256, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(384, s.tapEnd);
  TEST_ASSERT_TRUE(t.tap.segmentAt(400, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(384, s.tapStart);
  TEST_ASSERT_EQUAL_UINT32(1256, s.trackStart);

  // The reader attached before the gap: the runs it gets never splice the
  // two sides of it together.
  std::vector<int16_t> scratch(1024);
  std::vector<uint32_t> starts;
  reader.poll(scratch.data(), 1024, [&](const TapReader::Run& r) { starts.push_back(r.trackFrame); });
  TEST_ASSERT_EQUAL_UINT32(2, starts.size());
  TEST_ASSERT_EQUAL_UINT32(1128, starts[0]);
  TEST_ASSERT_EQUAL_UINT32(1256, starts[1]);
}

// Only the last kSegments segments are kept; older frames can't be placed.
void test_tap_forgets_old_segments() {
  Tap t;
  auto in = stereoRamp(0, 16);
  for (uint32_t k = 0; k < AudioTap::kSegments + 2; ++k) t.tap.write(in.data(), 16, 16, k, 0, 0);
  AudioTap::Segment s;
  TEST_ASSERT_FALSE(t.tap.segmentAt(0, t.tap.count(), &s));
  TEST_ASSERT_TRUE(t.tap.segmentAt(t.tap.count() - 1, t.tap.count(), &s));
  TEST_ASSERT_EQUAL_UINT32(AudioTap::kSegments + 1, s.epoch);
}

// A writer on another thread and a reader polling: every run the reader gets
// holds exactly the frames written at those track positions, in order.
void test_tap_concurrent_writer_and_reader() {
  std::vector<int16_t> buf(4096);
  AudioTap tap(buf.data(), 4096);
  std::atomic<bool> done{false};
  constexpr uint32_t kTotal = 400000;
  std::thread writer([&] {
    std::vector<int16_t> block(2 * 128);
    uint32_t track = 0;
    while (track < kTotal) {
      for (uint32_t i = 0; i < 128; ++i) {
        const auto v = static_cast<int16_t>((track + i) & 0x7FFF);
        block[2 * i] = v;
        block[2 * i + 1] = v;
      }
      tap.write(block.data(), 128, 128, 0, track, track);
      track += 128;
      if (track % 1024 == 0) std::this_thread::yield();
    }
    done = true;
  });
  TapReader reader;
  reader.attach(&tap, kRate);
  std::vector<int16_t> scratch(700);
  uint32_t expected = 0;
  bool first = true, ok = true;
  auto check = [&](const TapReader::Run& r) {
    if (first) {
      expected = r.trackFrame;
      first = false;
    }
    if (r.trackFrame != expected) ok = false;
    for (uint32_t i = 0; i < r.frames; ++i) {
      if (r.samples[i] != static_cast<int16_t>((r.trackFrame + i) & 0x7FFF)) ok = false;
    }
    expected = r.trackFrame + r.frames;
  };
  while (!done) {
    const uint32_t lost = reader.lostFrames();
    reader.poll(scratch.data(), 700, check);
    if (reader.lostFrames() != lost) first = true;  // lapped: the next run starts over
  }
  reader.poll(scratch.data(), 700, check);
  writer.join();
  TEST_ASSERT_TRUE(ok);
}

// ---- TapReader ----

void test_reader_hands_over_real_audio_in_runs() {
  Tap t;
  TapReader reader;
  auto in = stereoRamp(0, 128);
  t.tap.write(in.data(), 128, 128, 0, 0, 0);  // before attach: skipped
  reader.attach(&t.tap, kRate);
  t.tap.write(in.data(), 128, 128, 0, 128, 0);
  t.tap.write(in.data(), 128, 30, 0, 256, 0);   // 30 real, 98 fade
  t.tap.write(in.data(), 128, 128, 1, 0, 0);    // new track
  std::vector<TapReader::Run> runs;
  std::vector<int16_t> firsts;
  int16_t scratch[100];
  const uint32_t n = reader.poll(scratch, 100, [&](const TapReader::Run& r) {
    runs.push_back(r);
    firsts.push_back(r.samples[0]);
  });
  TEST_ASSERT_EQUAL_UINT32(128 + 30 + 128, n);
  // 128 + 30 of epoch 0 continue each other (split by the scratch size),
  // the fade is skipped, then epoch 1.
  TEST_ASSERT_EQUAL_UINT32(4, runs.size());
  TEST_ASSERT_EQUAL_UINT32(128, runs[0].trackFrame);
  TEST_ASSERT_EQUAL_UINT32(100, runs[0].frames);
  TEST_ASSERT_EQUAL_UINT32(228, runs[1].trackFrame);
  TEST_ASSERT_EQUAL_UINT32(58, runs[1].frames);
  TEST_ASSERT_EQUAL_INT16(1 + 100, firsts[1]);
  TEST_ASSERT_EQUAL_UINT32(1, runs[2].epoch);
  TEST_ASSERT_EQUAL_UINT32(0, runs[2].trackFrame);
  TEST_ASSERT_EQUAL_UINT32(100, runs[2].frames);
  TEST_ASSERT_EQUAL_UINT32(100, runs[3].trackFrame);
  TEST_ASSERT_EQUAL_UINT32(28, runs[3].frames);
  TEST_ASSERT_EQUAL_UINT32(0, reader.lostFrames());
  // Nothing new: nothing handed over.
  TEST_ASSERT_EQUAL_UINT32(0, reader.poll(scratch, 100, [](const TapReader::Run&) {}));
}

void test_reader_counts_what_it_lost() {
  Tap t;
  TapReader reader;
  reader.attach(&t.tap, kRate);
  auto in = stereoRamp(0, 128);
  for (int k = 0; k < 20; ++k) t.tap.write(in.data(), 128, 128, 0, 128 * k, 0);
  uint32_t first = 0;
  bool got = false;
  int16_t scratch[256];
  reader.poll(scratch, 256, [&](const TapReader::Run& r) {
    if (!got) first = r.trackFrame;
    got = true;
  });
  TEST_ASSERT_TRUE(got);
  TEST_ASSERT_TRUE(reader.lostFrames() > 0);
  TEST_ASSERT_EQUAL_UINT32(20 * 128 - 768, first);  // kept the newest 3/4 of the capacity
}

// Bursty writes (Bluetooth: ~1300 frames every 30 ms): the audible frame
// advances smoothly at the sample rate, `latency` behind the leading edge.
void test_audible_clock_smooths_bursts() {
  std::vector<int16_t> buf(1 << 16);
  AudioTap tap(buf.data(), 1 << 16);
  TapReader reader;
  reader.attach(&tap, kRate);
  std::vector<int16_t> block(2 * 1323);
  int16_t scratch[2048];
  uint32_t track = 0;
  uint32_t us = 5000000;
  double maxDev = 0;
  for (int tick = 0; tick < 100; ++tick) {
    tap.write(block.data(), 1323, 1323, 0, track, us);  // 30 ms of audio at once
    track += 1323;
    reader.poll(scratch, 2048, [](const TapReader::Run&) {});
    // Sample the audible frame across the 30 ms until the next burst.
    for (uint32_t d = 0; d < 30000; d += 5000) {
      const TapReader::Audible a = reader.audibleAt(us + d, 150000);
      if (tick < 5) continue;
      TEST_ASSERT_TRUE(a.valid);
      // Ideal: the burst's end, 150 ms before, advancing at the rate.
      const double ideal = static_cast<double>(track) + d * 1e-6 * kRate - 0.150 * kRate;
      const double dev = std::fabs(a.trackFrame + a.frac - ideal);
      if (dev > maxDev) maxDev = dev;
    }
    us += 30000;
  }
  TEST_ASSERT_TRUE(maxDev < 0.004 * kRate);  // within 4 ms
}

// A stall (speaker paused): the audible frame holds at the last frame
// written for a late burst's worth (60 ms), then nothing is heard, so the
// figure doesn't freeze mid-dance through a pause.
void test_audible_clock_stops_at_the_last_frame() {
  Tap t;
  TapReader reader;
  reader.attach(&t.tap, kRate);
  auto in = stereoRamp(0, 441);
  t.tap.write(in.data(), 441, 441, 0, 0, 1000000);
  int16_t scratch[512];
  reader.poll(scratch, 512, [](const TapReader::Run&) {});
  // Written at 1 s with 10 ms of latency: 5 ms later the listener hears the
  // frame 5 ms (220.5 frames) before the block's end.
  TapReader::Audible a = reader.audibleAt(1000000 + 5000, 10000);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_UINT32(220, a.trackFrame);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.5f, a.frac);
  // The block's end is heard at 1.010 s. 30 ms after that (a burst late),
  // the last frame; 70 ms after, and 2 s after, nothing.
  a = reader.audibleAt(1000000 + 10000 + 30000, 10000);
  TEST_ASSERT_TRUE(a.valid);
  TEST_ASSERT_EQUAL_UINT32(440, a.trackFrame);
  TEST_ASSERT_FALSE(reader.audibleAt(1000000 + 10000 + 70000, 10000).valid);
  TEST_ASSERT_FALSE(reader.audibleAt(1000000 + 2000000, 10000).valid);
  // A fade/silence being heard: not valid.
  t.tap.write(in.data(), 441, 0, 0, 0, 3000000);
  reader.poll(scratch, 512, [](const TapReader::Run&) {});
  TEST_ASSERT_FALSE(reader.audibleAt(3000000 + 5000, 0).valid);
  TEST_ASSERT_TRUE(reader.audibleAt(3000000 + 5000, 20000).valid);  // still hearing the audio before it
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_ring_reports_position_in_epoch);
  RUN_TEST(test_declick_reader_passes_position_through);
  RUN_TEST(test_tap_stores_mono_history);
  RUN_TEST(test_tap_mono_is_the_average);
  RUN_TEST(test_tap_segments_follow_the_track);
  RUN_TEST(test_tap_switched_off_skips_and_starts_a_new_segment);
  RUN_TEST(test_tap_forgets_old_segments);
  RUN_TEST(test_tap_concurrent_writer_and_reader);
  RUN_TEST(test_reader_hands_over_real_audio_in_runs);
  RUN_TEST(test_reader_counts_what_it_lost);
  RUN_TEST(test_audible_clock_smooths_bursts);
  RUN_TEST(test_audible_clock_stops_at_the_last_frame);
  return UNITY_END();
}
