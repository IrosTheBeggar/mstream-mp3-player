// Host unit tests for ReconnectPlanner (finding the headphones again while no
// Bluetooth link is up), against a model of ESP32-A2DP v1.8.11's heartbeat
// handling (BluetoothA2DPSource.cpp handle_reconnect_logic and the
// unconnected / connecting handlers). Run: pio test -e native
#include <unity.h>

#include <cstdint>

#include "ReconnectPlanner.h"

using Lib = ReconnectPlanner::Lib;
using Do = ReconnectPlanner::Do;

namespace {
constexpr uint32_t kHeartbeatMs = 10000;
constexpr int kMaxRetries = 3;

// The library's reconnect state, as far as the heartbeat is concerned.
struct LibModel {
  Lib state = Lib::Unconnected;
  bool discoveryActive = false;
  bool allowed = false;      // is_autoreconnect_allowed
  bool autoReconnect = false;  // reconnect_status == AutoReconnect
  int retries = kMaxRetries;
  bool remembered = false;
  int connectingBeats = 0;
  int pages = 0;             // connect_to(last_connection) calls

  bool armed() const { return allowed && autoReconnect; }

  // handle_reconnect_logic()
  void reconnectLogic() {
    if (!allowed) return;
    if (autoReconnect && retries > 0) {
      --retries;
      state = Lib::Connecting;
      connectingBeats = 0;
      ++pages;
    } else if (autoReconnect) {
      autoReconnect = false;  // NoReconnect
      retries = kMaxRetries;
      state = Lib::Discovering;
      discoveryActive = true;
    }
  }

  // The library's own heartbeat handling.
  void heartbeat() {
    if (state == Lib::Unconnected) {
      reconnectLogic();
    } else if (state == Lib::Connecting) {
      if (++connectingBeats >= 2) state = Lib::Unconnected;  // the headphones never answered
    }
  }

  void arm() {
    autoReconnect = true;
    retries = kMaxRetries;
    allowed = true;
  }
};

// One heartbeat: the planner decides, the model carries it out as
// PlayerA2dp::heartbeat() does.
Do beat(ReconnectPlanner& p, LibModel& m, uint32_t nowMs, bool linked = false) {
  const Do d = p.heartbeat({linked, m.state, m.discoveryActive, m.armed(), m.remembered, nowMs});
  switch (d) {
    case Do::KeepAlive:
    case Do::KeepScanning:
      break;
    case Do::StopScanAndPage:
      m.state = Lib::Unconnected;
      m.discoveryActive = false;
      m.arm();
      m.heartbeat();
      break;
    case Do::ReArm:
      m.arm();
      m.heartbeat();
      break;
    case Do::Scan:
      m.state = Lib::Discovering;
      m.discoveryActive = true;
      break;
    case Do::PassOn:
      m.heartbeat();
      break;
  }
  return d;
}
}  // namespace

void setUp() {}
void tearDown() {}

// Fresh NVS: start() scanned (auto-reconnect off), found the headphones,
// and the connection failed. The library alone would sit there until reboot.
void test_failed_first_connect_on_fresh_nvs_scans_again() {
  ReconnectPlanner p;
  LibModel m;  // nothing remembered, not armed
  m.state = Lib::Unconnected;  // the connect attempt just failed
  TEST_ASSERT_EQUAL(Do::Scan, beat(p, m, 0));
  TEST_ASSERT_EQUAL(Lib::Discovering, m.state);
  // Without a remembered device it keeps scanning.
  for (uint32_t t = kHeartbeatMs; t < 10 * 60000; t += kHeartbeatMs) {
    TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, t));
  }
}

// A remembered device that stays away: the library's retries, a minute of
// scanning, then paging it again, over and over (never stuck in either).
void test_retries_then_scan_then_page_again() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.arm();
  uint32_t t = 0;
  int lastPages = 0;
  uint32_t lastPageMs = 0;
  bool scanned = false;
  for (int i = 0; i < 200; ++i, t += kHeartbeatMs) {
    beat(p, m, t);
    if (m.state == Lib::Discovering) scanned = true;
    if (m.pages != lastPages) {
      if (lastPages > 0) TEST_ASSERT_TRUE(t - lastPageMs <= 2 * ReconnectPlanner::kScanForMs);
      lastPages = m.pages;
      lastPageMs = t;
    }
  }
  TEST_ASSERT_TRUE(scanned);
  TEST_ASSERT_TRUE(m.pages > 10);
  TEST_ASSERT_TRUE(t - lastPageMs <= 2 * ReconnectPlanner::kScanForMs);
}

void test_scan_is_given_its_minute() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.state = Lib::Discovering;  // the library's retries ran out
  m.discoveryActive = true;
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 1000));
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 1000 + ReconnectPlanner::kScanForMs - 1));
  TEST_ASSERT_EQUAL(Do::StopScanAndPage, beat(p, m, 1000 + ReconnectPlanner::kScanForMs));
  TEST_ASSERT_EQUAL(Lib::Connecting, m.state);
  TEST_ASSERT_EQUAL(1, m.pages);
}

void test_scan_timer_survives_millis_wraparound() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.state = Lib::Discovering;
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 0xFFFFF000u));
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 0x00001000u));  // 8 s later
  TEST_ASSERT_EQUAL(Do::StopScanAndPage, beat(p, m, 0xFFFFF000u + ReconnectPlanner::kScanForMs));
}

// Given up after a connection found by scanning failed (NoReconnect), but a
// device is remembered: page it again.
void test_given_up_with_a_remembered_device_rearms() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.allowed = true;
  m.autoReconnect = false;
  TEST_ASSERT_EQUAL(Do::ReArm, beat(p, m, 0));
  TEST_ASSERT_EQUAL(Lib::Connecting, m.state);
  TEST_ASSERT_EQUAL(1, m.pages);
}

void test_a_scan_still_running_is_waited_for() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.state = Lib::Unconnected;
  m.discoveryActive = true;  // stopping: the stack hasn't said so yet
  TEST_ASSERT_EQUAL(Do::PassOn, beat(p, m, 0));
  TEST_ASSERT_EQUAL(0, m.pages);
}

void test_linked_only_keeps_alive_and_restarts_the_scan_timer() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.state = Lib::Discovering;
  beat(p, m, 0);
  TEST_ASSERT_EQUAL(Do::KeepAlive, beat(p, m, 50000, /*linked=*/true));
  // Link lost again, scanning: a whole new minute.
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 70000));
  TEST_ASSERT_EQUAL(Do::KeepScanning, beat(p, m, 70000 + ReconnectPlanner::kScanForMs - 1));
  TEST_ASSERT_EQUAL(Do::StopScanAndPage, beat(p, m, 70000 + ReconnectPlanner::kScanForMs));
}

void test_connecting_is_left_to_the_library() {
  ReconnectPlanner p;
  LibModel m;
  m.remembered = true;
  m.state = Lib::Connecting;
  TEST_ASSERT_EQUAL(Do::PassOn, beat(p, m, 0));
  TEST_ASSERT_EQUAL(Do::PassOn, beat(p, m, kHeartbeatMs));
  TEST_ASSERT_EQUAL(Lib::Unconnected, m.state);  // its 2-heartbeat timeout
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_failed_first_connect_on_fresh_nvs_scans_again);
  RUN_TEST(test_retries_then_scan_then_page_again);
  RUN_TEST(test_scan_is_given_its_minute);
  RUN_TEST(test_scan_timer_survives_millis_wraparound);
  RUN_TEST(test_given_up_with_a_remembered_device_rearms);
  RUN_TEST(test_a_scan_still_running_is_waited_for);
  RUN_TEST(test_linked_only_keeps_alive_and_restarts_the_scan_timer);
  RUN_TEST(test_connecting_is_left_to_the_library);
  return UNITY_END();
}
