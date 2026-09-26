// Host unit tests for StreamRestart: when the Bluetooth data callback's audio
// fades in from 0, against the callback sequences ESP-IDF produces.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <initializer_list>

#include "StreamRestart.h"

namespace {
constexpr int32_t kFrames = 128;  // what ESP-IDF asks for per callback
constexpr int64_t kMs = 1000;

// The data callback as BtSink drives it: the time, and BtSink's stream epoch
// (bumped by BtAppT before every START of ours and on every link).
struct Callbacks {
  StreamRestart r;
  int64_t nowUs = 5 * kMs;  // esp_timer starts at boot, well before any audio
  uint32_t epoch = 0;
  int restarts = 0;

  // One callback with frames, `afterUs` after the previous one.
  bool pull(int64_t afterUs) {
    nowUs += afterUs;
    const bool restart = r.callback(nowUs, kFrames, epoch);
    restarts += restart ? 1 : 0;
    return restart;
  }
  // `n` callbacks of a running stream (~10 ms apart: several per ~30 ms tick).
  void stream(int n) {
    for (int i = 0; i < n; ++i) TEST_ASSERT_FALSE(pull(10 * kMs));
  }
  // ESP-IDF's flush: a callback with no frames (btc_a2dp_source_aa_tx_flush).
  void flush(int64_t afterUs = 1 * kMs) {
    nowUs += afterUs;
    TEST_ASSERT_FALSE(r.callback(nowUs, 0, epoch));
    TEST_ASSERT_FALSE(r.callback(nowUs, -1, epoch));  // len -1, as ESP-IDF passes it
  }
  void start() { ++epoch; }  // our START, or a new link
};

// A stream running for a while after the first callback.
Callbacks running() {
  Callbacks c;
  c.start();  // the link
  c.start();  // our START
  TEST_ASSERT_TRUE(c.pull(0));
  c.stream(50);
  TEST_ASSERT_EQUAL_INT(1, c.restarts);
  return c;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_the_first_callback_restarts() {
  Callbacks c;
  TEST_ASSERT_TRUE(c.pull(0));  // even without a new epoch
  TEST_ASSERT_EQUAL_UINT32(0, c.r.statGapUs());
  c.stream(10);
  TEST_ASSERT_EQUAL_UINT32(10 * kMs, c.r.statGapUs());
}

// We suspend (pause on the Core2, 3 s later), ESP-IDF flushes; our next START
// resumes it, however soon.
void test_our_suspend_then_start() {
  Callbacks c = running();
  c.flush();
  c.start();
  TEST_ASSERT_TRUE(c.pull(20 * kMs));
  c.stream(20);
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// The flush alone is enough: callbacks right after it (no new epoch, short
// gap) still start from silence.
void test_a_flush_restarts_the_next_audio_whatever_the_gap() {
  Callbacks c = running();
  c.flush();
  TEST_ASSERT_TRUE(c.pull(5 * kMs));
  c.stream(5);
  c.flush();
  c.flush();  // two flushes: still one restart
  TEST_ASSERT_TRUE(c.pull(5 * kMs));
  TEST_ASSERT_FALSE(c.pull(10 * kMs));
  TEST_ASSERT_EQUAL_INT(3, c.restarts);
}

// The headphones suspend our stream (out of the ear): flush; the player
// pauses, and the next play's START resumes it seconds later.
void test_remote_suspend_then_our_start() {
  Callbacks c = running();
  c.flush();
  c.start();
  TEST_ASSERT_TRUE(c.pull(4000 * kMs));
  c.stream(10);
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// The headphones start a stream themselves (no START of ours) and ESP-IDF
// suspends it at once: a burst of callbacks, then a flush.
void test_remote_start_that_esp_idf_suspends() {
  Callbacks c = running();
  c.flush();
  // After a long silence the burst fades in (the gap), without a new epoch.
  TEST_ASSERT_TRUE(c.pull(5000 * kMs));
  c.stream(3);
  c.flush();
  // Our START later: from silence again.
  c.start();
  TEST_ASSERT_TRUE(c.pull(300 * kMs));
  c.stream(5);
  TEST_ASSERT_EQUAL_INT(3, c.restarts);
}

// A remote start soon after our own suspend: the flush of our suspend is
// still pending, so its burst fades in although neither the gap nor an epoch
// says so.
void test_remote_start_soon_after_our_suspend() {
  Callbacks c = running();
  c.flush();
  TEST_ASSERT_TRUE(c.pull(50 * kMs));
  c.stream(2);
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// The link drops (ESP-IDF flushes), a new link comes up, our START.
void test_link_drop_with_flush_then_a_new_link() {
  Callbacks c = running();
  c.flush();
  c.start();  // the new link
  c.start();  // our START on it
  TEST_ASSERT_TRUE(c.pull(8000 * kMs));
  c.stream(5);
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// A link drop whose flush never reached us (the stream just stops): the new
// link's epoch and the gap restart it anyway.
void test_link_drop_without_flush() {
  Callbacks c = running();
  c.start();
  c.start();
  TEST_ASSERT_TRUE(c.pull(250 * kMs));
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// 100-999 ms without a callback and without a START: congestion, or a flash
// write stalling the task. The audio goes on (a fade from 0 would click).
void test_a_stall_without_start_does_not_restart() {
  Callbacks c = running();
  for (int64_t stall : {100 * kMs, 250 * kMs, 500 * kMs, 999 * kMs}) {
    TEST_ASSERT_FALSE(c.pull(stall));
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(stall), c.r.statGapUs());
    c.stream(3);
  }
  TEST_ASSERT_EQUAL_INT(1, c.restarts);
}

// From 1 s on the listener certainly heard silence: fade in, START or not.
// Such a gap is left out of the gap statistics.
void test_a_gap_of_a_second_restarts() {
  Callbacks c = running();
  TEST_ASSERT_FALSE(c.pull(1000 * kMs - 1));
  TEST_ASSERT_TRUE(c.pull(1000 * kMs));
  TEST_ASSERT_EQUAL_UINT32(0, c.r.statGapUs());
  c.stream(2);
  TEST_ASSERT_EQUAL_INT(2, c.restarts);
}

// A START of ours (a new epoch) counts as a new stream only after a wait of
// 100 ms or more: with a shorter one no audio was missed.
void test_a_new_epoch_needs_a_gap_of_100_ms() {
  Callbacks c = running();
  c.start();
  TEST_ASSERT_FALSE(c.pull(99 * kMs));
  c.stream(3);  // the epoch was seen: later callbacks don't restart
  c.start();
  TEST_ASSERT_TRUE(c.pull(100 * kMs));
  c.stream(3);
  c.start();
  c.start();  // several epochs in one wait count once
  TEST_ASSERT_TRUE(c.pull(400 * kMs));
  TEST_ASSERT_FALSE(c.pull(400 * kMs));  // the same stall again, epoch already seen
  TEST_ASSERT_EQUAL_INT(3, c.restarts);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_callback_restarts);
  RUN_TEST(test_our_suspend_then_start);
  RUN_TEST(test_a_flush_restarts_the_next_audio_whatever_the_gap);
  RUN_TEST(test_remote_suspend_then_our_start);
  RUN_TEST(test_remote_start_that_esp_idf_suspends);
  RUN_TEST(test_remote_start_soon_after_our_suspend);
  RUN_TEST(test_link_drop_with_flush_then_a_new_link);
  RUN_TEST(test_link_drop_without_flush);
  RUN_TEST(test_a_stall_without_start_does_not_restart);
  RUN_TEST(test_a_gap_of_a_second_restarts);
  RUN_TEST(test_a_new_epoch_needs_a_gap_of_100_ms);
  return UNITY_END();
}
