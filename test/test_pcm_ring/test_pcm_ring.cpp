// Host unit tests for PcmRing. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "PcmRing.h"

namespace {
constexpr uint8_t kBt = 1;
constexpr uint8_t kSpeaker = 2;

// A ring whose reads go to kBt, like most tests want.
struct TestRing {
  std::vector<int16_t> buf;
  PcmRing ring;
  explicit TestRing(uint32_t cap, uint32_t initialIndex = 0)
      : buf(cap * 2), ring(buf.data(), cap, initialIndex) {
    ring.setConsumer(kBt);
  }
};

// Frame i is (i, -i) so every frame is identifiable and L/R can't swap unnoticed.
std::vector<int16_t> frames(int from, int count) {
  std::vector<int16_t> v;
  for (int i = from; i < from + count; ++i) {
    v.push_back(static_cast<int16_t>(i));
    v.push_back(static_cast<int16_t>(-i));
  }
  return v;
}

void assertFrames(const std::vector<int16_t>& got, int from, int count) {
  TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(count) * 2, got.size());
  const std::vector<int16_t> want = frames(from, count);
  TEST_ASSERT_EQUAL_INT16_ARRAY(want.data(), got.data(), want.size());
}

std::vector<int16_t> readN(PcmRing& r, uint32_t n, uint8_t id = kBt) {
  std::vector<int16_t> out(n * 2);
  const uint32_t got = r.read(id, out.data(), n);
  out.resize(got * 2);
  return out;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_write_then_read_round_trip() {
  TestRing t(8);
  PcmRing& r = t.ring;
  const auto in = frames(1, 5);
  TEST_ASSERT_EQUAL_UINT32(5, r.write(in.data(), 5));
  TEST_ASSERT_EQUAL_UINT32(5, r.size());
  TEST_ASSERT_EQUAL_UINT32(3, r.space());
  assertFrames(readN(r, 5), 1, 5);
  TEST_ASSERT_EQUAL_UINT32(0, r.size());
  TEST_ASSERT_EQUAL_UINT32(5, r.readPos());
}

void test_wraparound_preserves_order() {
  TestRing t(8);
  PcmRing& r = t.ring;
  auto a = frames(0, 6);
  r.write(a.data(), 6);
  assertFrames(readN(r, 6), 0, 6);
  auto b = frames(6, 7);  // spans the end of the buffer
  TEST_ASSERT_EQUAL_UINT32(7, r.write(b.data(), 7));
  assertFrames(readN(r, 7), 6, 7);
}

void test_write_stops_when_full_and_read_returns_only_available() {
  TestRing t(4);
  PcmRing& r = t.ring;
  auto in = frames(0, 6);
  TEST_ASSERT_EQUAL_UINT32(4, r.write(in.data(), 6));
  TEST_ASSERT_EQUAL_UINT32(0, r.space());
  TEST_ASSERT_EQUAL_UINT32(0, r.write(in.data(), 1));
  assertFrames(readN(r, 10), 0, 4);  // asks for more than there is
  TEST_ASSERT_EQUAL_UINT32(0, readN(r, 1).size());
}

void test_discard_drops_unread_and_new_data_follows() {
  TestRing t(8);
  PcmRing& r = t.ring;
  auto oldTrack = frames(100, 8);
  r.write(oldTrack.data(), 8);
  readN(r, 3);  // consumer part-way through the old track

  const uint32_t start = r.discardAll();
  TEST_ASSERT_EQUAL_UINT32(8, start);
  TEST_ASSERT_EQUAL_UINT32(0, r.size());
  TEST_ASSERT_EQUAL_UINT32(8, r.space());  // producer can refill at once
  TEST_ASSERT_EQUAL_UINT32(start, r.readPos());

  auto newTrack = frames(0, 5);
  r.write(newTrack.data(), 5);
  assertFrames(readN(r, 8), 0, 5);  // nothing from the old track
  TEST_ASSERT_EQUAL_UINT32(5, r.readPos() - start);
}

void test_only_the_current_consumer_reads() {
  TestRing t(8);
  PcmRing& r = t.ring;
  auto in = frames(0, 6);
  r.write(in.data(), 6);
  TEST_ASSERT_EQUAL_UINT32(0, readN(r, 6, kSpeaker).size());  // not its turn
  assertFrames(readN(r, 2, kBt), 0, 2);

  r.setConsumer(kSpeaker);  // hand over mid-stream: order carries on
  TEST_ASSERT_EQUAL_UINT32(0, readN(r, 6, kBt).size());
  assertFrames(readN(r, 6, kSpeaker), 2, 4);

  r.setConsumer(PcmRing::kNoConsumer);
  r.write(in.data(), 1);
  TEST_ASSERT_EQUAL_UINT32(0, readN(r, 1, kSpeaker).size());
  TEST_ASSERT_EQUAL_UINT32(1, r.size());
}

void test_indices_survive_uint32_wrap() {
  TestRing t(8, 0xFFFFFFFCu);  // 4 frames before the counters wrap
  PcmRing& r = t.ring;
  for (int round = 0; round < 4; ++round) {
    auto in = frames(round * 6, 6);
    TEST_ASSERT_EQUAL_UINT32(6, r.write(in.data(), 6));
    TEST_ASSERT_EQUAL_UINT32(6, r.size());
    assertFrames(readN(r, 6), round * 6, 6);
  }
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFCu + 24, r.readPos());  // wrapped past 0
}

void test_concurrent_producer_and_consumer_keep_order() {
  constexpr int kTotal = 200000;
  TestRing t(256);
  PcmRing& r = t.ring;

  std::thread producer([&r] {
    int next = 0;
    while (next < kTotal) {
      const int n = std::min((next % 37) + 1, kTotal - next);  // uneven chunk sizes
      auto in = frames(next, n);
      next += static_cast<int>(r.write(in.data(), static_cast<uint32_t>(n)));
    }
  });

  int expected = 0;
  bool ordered = true;
  std::vector<int16_t> out(64 * 2);
  while (expected < kTotal && ordered) {
    const uint32_t got = r.read(kBt, out.data(), 1 + static_cast<uint32_t>(expected % 64));
    for (uint32_t i = 0; i < got; ++i) {
      const auto l = static_cast<int16_t>(expected);
      if (out[2 * i] != l || out[2 * i + 1] != static_cast<int16_t>(-l)) ordered = false;
      ++expected;
    }
  }
  producer.join();
  TEST_ASSERT_TRUE(ordered);
  TEST_ASSERT_EQUAL_INT(kTotal, expected);
}

void test_discard_bumps_the_epoch_reads_report() {
  TestRing t(8);
  PcmRing& r = t.ring;
  TEST_ASSERT_EQUAL_UINT32(0, r.epoch());
  auto in = frames(0, 4);
  r.write(in.data(), 4);
  std::vector<int16_t> out(8 * 2);
  uint32_t epoch = 99;
  TEST_ASSERT_EQUAL_UINT32(2, r.read(kBt, out.data(), 2, &epoch));
  TEST_ASSERT_EQUAL_UINT32(0, epoch);

  r.discardAll();
  TEST_ASSERT_EQUAL_UINT32(1, r.epoch());
  TEST_ASSERT_EQUAL_UINT32(0, r.read(kBt, out.data(), 2, &epoch));  // empty, but reported
  TEST_ASSERT_EQUAL_UINT32(1, epoch);
  r.discardAll();
  r.write(in.data(), 4);
  TEST_ASSERT_EQUAL_UINT32(4, r.read(kBt, out.data(), 8, &epoch));
  TEST_ASSERT_EQUAL_UINT32(2, epoch);
}

void test_epoch_left_alone_for_a_non_consumer() {
  TestRing t(8);
  PcmRing& r = t.ring;
  r.discardAll();
  std::vector<int16_t> out(2);
  uint32_t epoch = 42;
  TEST_ASSERT_EQUAL_UINT32(0, r.read(kSpeaker, out.data(), 1, &epoch));
  TEST_ASSERT_EQUAL_UINT32(42, epoch);
}

// Frames always arrive with the epoch of the track they belong to, even while
// the producer skips tracks under the consumer's feet.
void test_concurrent_skips_never_mix_epochs() {
  constexpr int kTracks = 3000;
  TestRing t(64);
  PcmRing& r = t.ring;
  std::atomic<bool> done{false};

  std::thread producer([&r, &done] {
    for (int track = 1; track <= kTracks; ++track) {
      r.discardAll();  // epoch == track from here on
      std::vector<int16_t> in;
      const int n = 1 + track % 97;
      for (int i = 0; i < n; ++i) {
        in.push_back(static_cast<int16_t>(track));
        in.push_back(static_cast<int16_t>(i));
      }
      int written = 0;
      int spins = track % 5;  // sometimes skip before the track has been written
      while (written < n && spins-- >= 0) {
        written += static_cast<int>(r.write(in.data() + 2 * written, static_cast<uint32_t>(n - written)));
      }
    }
    done = true;
  });

  bool consistent = true;
  uint32_t seen = 0;
  std::vector<int16_t> out(16 * 2);
  while (!done.load() || r.size() > 0) {
    uint32_t epoch = 0xFFFFFFFFu;
    const uint32_t got = r.read(kBt, out.data(), 1 + seen % 16, &epoch);
    for (uint32_t i = 0; i < got; ++i) {
      if (static_cast<uint32_t>(out[2 * i]) != epoch) consistent = false;
    }
    seen += got;
  }
  producer.join();
  TEST_ASSERT_TRUE(consistent);
  TEST_ASSERT_EQUAL_UINT32(kTracks, r.epoch());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_write_then_read_round_trip);
  RUN_TEST(test_wraparound_preserves_order);
  RUN_TEST(test_write_stops_when_full_and_read_returns_only_available);
  RUN_TEST(test_discard_drops_unread_and_new_data_follows);
  RUN_TEST(test_only_the_current_consumer_reads);
  RUN_TEST(test_indices_survive_uint32_wrap);
  RUN_TEST(test_concurrent_producer_and_consumer_keep_order);
  RUN_TEST(test_discard_bumps_the_epoch_reads_report);
  RUN_TEST(test_epoch_left_alone_for_a_non_consumer);
  RUN_TEST(test_concurrent_skips_never_mix_epochs);
  return UNITY_END();
}
