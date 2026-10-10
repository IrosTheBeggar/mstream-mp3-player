// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for Device info's rows (DeviceInfo): each value's text, the
// uptime's, what the boot log keeps of them, and that a short buffer only
// cuts. The widths in the fonts are test_ui_library's.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "DeviceInfo.h"

using deviceinfo::Item;

void setUp() {}
void tearDown() {}

namespace {

deviceinfo::Facts core2() {
  deviceinfo::Facts f;
  f.board = "M5Stack Core2";
  f.pmic = "AXP192";
  f.imu = "MPU6886";
  f.chip = "ESP32-D0WDQ6-V3";
  f.chipRevision = 301;
  f.cpuMhz = 240;
  f.flashBytes = 16u * 1024 * 1024;
  f.psramBytes = 4096u * 1024;
  f.psramFree = 3210u * 1024 + 300;
  f.lastReset = "power-on";
  f.battery = 87;
  f.batteryMv = 4052;
  f.charging = false;
  f.storage = "SD";
  f.tracks = 1234;
  f.ramFree = 180u * 1024 + 1000;
  f.ramMin = 120u * 1024;
  f.ramBlock = 110u * 1024 + 1023;
  f.uptimeS = 125;
  f.version = "v0.8.0-dev+abc1234";
  f.commit = "abc1234";
  f.built = "2026-10-09";
  f.elf = "1a2b3c4d";
  return f;
}

std::string value(Item i, const deviceinfo::Facts& f) {
  char buf[96];
  return deviceinfo::value(i, f, buf, sizeof(buf));
}

std::string uptime(uint32_t s) {
  char buf[32];
  deviceinfo::uptimeText(s, buf, sizeof(buf));
  return buf;
}

}  // namespace

// Each row, as the boot screen wrote it before (the [diag] lines keep
// those texts; the Library's count is grouped now, as the Output tab's
// Library row has it) and as the new ones read.
void test_values() {
  const deviceinfo::Facts f = core2();
  TEST_ASSERT_EQUAL_STRING("M5Stack Core2", value(Item::Board, f).c_str());
  TEST_ASSERT_EQUAL_STRING("AXP192", value(Item::PowerChip, f).c_str());
  TEST_ASSERT_EQUAL_STRING("MPU6886", value(Item::Imu, f).c_str());
  TEST_ASSERT_EQUAL_STRING("ESP32-D0WDQ6-V3 rev 3.1", value(Item::Chip, f).c_str());
  TEST_ASSERT_EQUAL_STRING("240 MHz", value(Item::Cpu, f).c_str());
  TEST_ASSERT_EQUAL_STRING("16 MB", value(Item::Flash, f).c_str());
  TEST_ASSERT_EQUAL_STRING("4096K (3210K free)", value(Item::Psram, f).c_str());
  TEST_ASSERT_EQUAL_STRING("power-on", value(Item::LastReset, f).c_str());
  TEST_ASSERT_EQUAL_STRING("87%, 4.05 V", value(Item::Battery, f).c_str());
  TEST_ASSERT_EQUAL_STRING("SD, 1,234 tracks", value(Item::Library, f).c_str());
  TEST_ASSERT_EQUAL_STRING("180K (min 120K, block 110K)", value(Item::RamFree, f).c_str());
  TEST_ASSERT_EQUAL_STRING("2 min 05 s", value(Item::Uptime, f).c_str());
  TEST_ASSERT_EQUAL_STRING("v0.8.0-dev+abc1234", value(Item::Firmware, f).c_str());
  TEST_ASSERT_EQUAL_STRING("abc1234, 2026-10-09, ELF 1a2b3c4d", value(Item::Build, f).c_str());
}

void test_value_variants() {
  deviceinfo::Facts f = core2();
  f.chipRevision = 300;
  TEST_ASSERT_EQUAL_STRING("ESP32-D0WDQ6-V3 rev 3.0", value(Item::Chip, f).c_str());
  // The battery: charging, the volts rounded (4.996 V is 5.00), no volts
  // when the chip didn't say, and a level it didn't give.
  f.charging = true;
  TEST_ASSERT_EQUAL_STRING("87%, 4.05 V, charging", value(Item::Battery, f).c_str());
  f.batteryMv = 4996;
  f.battery = 100;
  TEST_ASSERT_EQUAL_STRING("100%, 5.00 V, charging", value(Item::Battery, f).c_str());
  f.batteryMv = 3004;
  f.battery = 0;
  f.charging = false;
  TEST_ASSERT_EQUAL_STRING("0%, 3.00 V", value(Item::Battery, f).c_str());
  f.batteryMv = 0;
  TEST_ASSERT_EQUAL_STRING("0%", value(Item::Battery, f).c_str());
  f.battery = -1;
  TEST_ASSERT_EQUAL_STRING("not known", value(Item::Battery, f).c_str());
  // The library: one track, none (no card, nothing found).
  f.tracks = 1;
  TEST_ASSERT_EQUAL_STRING("SD, 1 track", value(Item::Library, f).c_str());
  f.tracks = 0;
  f.storage = "none";
  TEST_ASSERT_EQUAL_STRING("none, 0 tracks", value(Item::Library, f).c_str());
  // A card filled by hand, its tags not read yet; then read: where the names
  // come from in the Output tab's Library row's shortest form (100% only
  // when all are).
  f.storage = "SD";
  f.tracks = 19410;
  f.fromNone = 19410;
  TEST_ASSERT_EQUAL_STRING("SD, 19,410 tracks, names from the files", value(Item::Library, f).c_str());
  f.fromTransfer = 18000;
  f.fromDevice = 1400;
  f.fromNone = 10;
  TEST_ASSERT_EQUAL_STRING("SD, 19,410 tracks, 99% tagged", value(Item::Library, f).c_str());
  f.tracks = 19400;
  f.fromNone = 0;
  TEST_ASSERT_EQUAL_STRING("SD, 19,400 tracks, 100% tagged", value(Item::Library, f).c_str());
  // Behind the library update's fence: the count from before it, and that
  // it updates, whatever the counts.
  f.updating = true;
  TEST_ASSERT_EQUAL_STRING("SD, 19,400 tracks, updating\xE2\x80\xA6", value(Item::Library, f).c_str());
  // No tracks: no more than the count.
  f.tracks = 0;
  TEST_ASSERT_EQUAL_STRING("SD, 0 tracks", value(Item::Library, f).c_str());
  f.updating = false;
  TEST_ASSERT_EQUAL_STRING("SD, 0 tracks", value(Item::Library, f).c_str());
  // A buffer that the count fills: cut, nothing after it.
  f.tracks = 19400;
  char cut[18];
  TEST_ASSERT_EQUAL_STRING("SD, 19,400 tracks", deviceinfo::value(Item::Library, f, cut, sizeof(cut)));
  char cut2[22];
  TEST_ASSERT_EQUAL_STRING("SD, 19,400 tracks, 10", deviceinfo::value(Item::Library, f, cut2, sizeof(cut2)));
  // A build without git: no commit.
  f.commit = "";
  TEST_ASSERT_EQUAL_STRING("2026-10-09, ELF 1a2b3c4d", value(Item::Build, f).c_str());
}

void test_uptime() {
  TEST_ASSERT_EQUAL_STRING("0 s", uptime(0).c_str());
  TEST_ASSERT_EQUAL_STRING("59 s", uptime(59).c_str());
  TEST_ASSERT_EQUAL_STRING("1 min 00 s", uptime(60).c_str());
  TEST_ASSERT_EQUAL_STRING("59 min 59 s", uptime(3599).c_str());
  TEST_ASSERT_EQUAL_STRING("1 h 00 min", uptime(3600).c_str());
  TEST_ASSERT_EQUAL_STRING("23 h 59 min", uptime(86399).c_str());
  TEST_ASSERT_EQUAL_STRING("1 d 0 h", uptime(86400).c_str());
  TEST_ASSERT_EQUAL_STRING("49 d 17 h", uptime(49u * 86400 + 17 * 3600 + 3599).c_str());
  TEST_ASSERT_EQUAL_STRING("49710 d 6 h", uptime(UINT32_MAX).c_str());
}

// The labels: each one, none twice, and short enough for the boot log's
// column ("[diag] %-10s %s"). The boot screen's ten rows are all logged,
// in their old order; the version and build are in the banner instead.
void test_labels_and_the_boot_log() {
  for (int i = 0; i < deviceinfo::kItems; ++i) {
    const char* a = deviceinfo::label(static_cast<Item>(i));
    TEST_ASSERT_TRUE(a[0] != 0);
    TEST_ASSERT_TRUE(strlen(a) <= 10);
    for (int j = 0; j < i; ++j) TEST_ASSERT_TRUE(strcmp(a, deviceinfo::label(static_cast<Item>(j))) != 0);
  }
  static const char* const kOld[] = {"Board",      "Power chip", "IMU",     "Chip",    "Flash",
                                     "PSRAM",      "Last reset", "Battery", "Library", "RAM free"};
  int at = 0;
  for (const char* old : kOld) {
    while (at < deviceinfo::kLogged && strcmp(deviceinfo::label(static_cast<Item>(at)), old) != 0) ++at;
    TEST_ASSERT_TRUE_MESSAGE(at < deviceinfo::kLogged, old);
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Item::Uptime), deviceinfo::kLogged);
  TEST_ASSERT_EQUAL_INT(14, deviceinfo::kItems);
}

// What the page redraws at its refresh: what can change while it's open.
void test_what_changes() {
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::Battery));
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::RamFree));
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::Psram));
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::Uptime));
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::Cpu));
  TEST_ASSERT_TRUE(deviceinfo::changes(Item::Library));
  TEST_ASSERT_FALSE(deviceinfo::changes(Item::Board));
  TEST_ASSERT_FALSE(deviceinfo::changes(Item::Chip));
  TEST_ASSERT_FALSE(deviceinfo::changes(Item::Firmware));
  TEST_ASSERT_FALSE(deviceinfo::changes(Item::Build));
}

// A buffer too short: the text cut, always ended.
void test_short_buffers_cut() {
  const deviceinfo::Facts f = core2();
  for (int i = 0; i < deviceinfo::kItems; ++i) {
    char buf[9];
    memset(buf, 'x', sizeof(buf));
    deviceinfo::value(static_cast<Item>(i), f, buf, 6);
    TEST_ASSERT_TRUE(strlen(buf) <= 5);
    TEST_ASSERT_EQUAL_CHAR('x', buf[6]);
  }
  deviceinfo::Facts g = f;
  g.charging = true;
  char tiny[4];
  TEST_ASSERT_EQUAL_STRING("87%", deviceinfo::value(Item::Battery, g, tiny, sizeof(tiny)));
  char none[1] = {'x'};
  deviceinfo::value(Item::Board, g, none, 0);
  TEST_ASSERT_EQUAL_CHAR('x', none[0]);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_values);
  RUN_TEST(test_value_variants);
  RUN_TEST(test_uptime);
  RUN_TEST(test_labels_and_the_boot_log);
  RUN_TEST(test_what_changes);
  RUN_TEST(test_short_buffers_cut);
  return UNITY_END();
}
