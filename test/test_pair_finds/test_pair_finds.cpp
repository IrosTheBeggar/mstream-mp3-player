// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Pair screen's search log (PairFinds): each audio
// device once per search, one more line when a nameless one's name comes,
// the rest only counted; the lines and the summary as the serial log
// prints them; the ring from the Bluetooth task, its order and its drops.
// (Made-up addresses: no real one belongs in a committed file.)
// Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "PairFinds.h"

void setUp() {}
void tearDown() {}

namespace {

using Say = PairFinds::Say;

constexpr uint32_t kHeadset = 0x240404;  // rendering + Audio/Video, wearable headset
constexpr uint32_t kSpeaker = 0x240414;  // ... loudspeaker
constexpr uint32_t kPhone = 0x5a020c;    // a smartphone: not audio

PairFind find(uint8_t id, const char* name, int rssi, uint32_t cod, bool audio) {
  PairFind f;
  const uint8_t addr[6] = {0x02, 0x00, 0x00, 0x00, 0x00, id};
  std::memcpy(f.addr, addr, sizeof(addr));
  snprintf(f.name, sizeof(f.name), "%s", name);
  f.rssi = static_cast<int8_t>(rssi);
  f.cod = cod;
  f.audio = audio;
  return f;
}

}  // namespace

// Repeats say nothing: an inquiry round answers every ~10 s, for 2 min.
void test_each_audio_device_once_per_search() {
  PairFinds p;
  p.start(0);
  TEST_ASSERT_EQUAL(Say::Found, p.note(find(1, "SPYDRONE", -62, kHeadset, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(1, "SPYDRONE", -55, kHeadset, true)));  // a new signal: not news
  TEST_ASSERT_EQUAL(Say::Found, p.note(find(2, "", -81, kSpeaker, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(2, "", -80, kSpeaker, true)));
  // Its name arrives later: one more line, then nothing again.
  TEST_ASSERT_EQUAL(Say::Named, p.note(find(2, "JBL Flip 5", -70, kSpeaker, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(2, "JBL Flip 5", -70, kSpeaker, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(2, "", -70, kSpeaker, true)));
  // A name that changes isn't news either.
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(1, "SPYDRONE 2", -60, kHeadset, true)));
  // Other devices: counted, never said.
  for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(3, "Pixel", -50, kPhone, false)));
  TEST_ASSERT_EQUAL_INT(3, p.devices());
  TEST_ASSERT_EQUAL_INT(2, p.audioDevices());
  TEST_ASSERT_EQUAL_UINT32(11, p.results());
  // The next search starts afresh: the same devices are news again.
  p.start(200000);
  TEST_ASSERT_EQUAL_INT(0, p.devices());
  TEST_ASSERT_EQUAL_UINT32(0, p.results());
  TEST_ASSERT_EQUAL(Say::Found, p.note(find(1, "SPYDRONE", -62, kHeadset, true)));
}

// A device whose first result had no class that says audio, then one
// that does: found then (once).
void test_a_class_that_comes_later() {
  PairFinds p;
  p.start(0);
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(1, "", -60, 0, false)));
  TEST_ASSERT_EQUAL_INT(0, p.audioDevices());
  TEST_ASSERT_EQUAL(Say::Found, p.note(find(1, "SPYDRONE", -60, kHeadset, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(1, "SPYDRONE", -60, kHeadset, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(1, "", -60, 0, false)));  // a classless repeat
  TEST_ASSERT_EQUAL_INT(1, p.devices());
  TEST_ASSERT_EQUAL_INT(1, p.audioDevices());
}

void test_the_lines() {
  char buf[160];
  PairFind f = find(1, "SPYDRONE", -62, kHeadset, true);
  f.remembered = true;
  PairFinds::describe(f, Say::Found, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("found \"SPYDRONE\" (headphones, class 0x240404), rssi -62, the remembered headphones", buf);
  PairFinds::describe(find(2, "", -81, kSpeaker, true), Say::Found, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("found (no name) (speaker, class 0x240414), rssi -81", buf);
  PairFinds::describe(find(2, "JBL Flip 5", -70, kSpeaker, true), Say::Named, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("the one found with no name is \"JBL Flip 5\" (speaker, class 0x240414), rssi -70", buf);
  f.remembered = false;
  f.linked = true;
  f.rssi = -127;
  PairFinds::describe(f, Say::Found, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("found \"SPYDRONE\" (headphones, class 0x240404), no rssi, linked now: not listed", buf);
  // A name of the full 31 characters, and a small buffer: cut, terminated.
  PairFind l = find(3, "0123456789012345678901234567890", -50, kHeadset, true);
  PairFinds::describe(l, Say::Found, buf, sizeof(buf));
  TEST_ASSERT_NOT_NULL(std::strstr(buf, "\"0123456789012345678901234567890\""));
  char tiny[12];
  PairFinds::describe(l, Say::Found, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("found \"0123", tiny);
}

// What the log says when a search ends: the question the 2026-10-04 log
// couldn't answer (did it see the headphones, or nothing at all?).
void test_the_summary() {
  char buf[256];
  PairFinds p;
  // Nothing at all.
  p.start(1000);
  p.summary(121000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("the search saw nothing in 120 s (not one inquiry result)", buf);
  // Others only.
  p.start(0);
  for (int i = 0; i < 5; ++i) p.note(find(static_cast<uint8_t>(10 + i % 2), "", -70, kPhone, false));
  p.summary(30400, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("the search saw 2 devices in 30 s, none of them audio; 5 inquiry results", buf);
  // Some audio among them, in the order found, the remembered marked.
  p.start(0);
  PairFind r = find(1, "SPYDRONE", -62, kHeadset, true);
  r.remembered = true;
  p.note(find(20, "", -88, kPhone, false));
  p.note(r);
  p.note(find(21, "", -75, kPhone, false));
  p.note(find(2, "", -81, kSpeaker, true));
  p.note(r);
  p.note(find(22, "", -90, kPhone, false));
  p.summary(81000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "the search saw 5 devices in 81 s, 2 of them audio: \"SPYDRONE\" (remembered), (no name); 6 inquiry results", buf);
  // One device, and results the ring lost.
  p.start(0);
  p.note(find(1, "SPYDRONE", -62, kHeadset, true));
  p.summary(9600, 3, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "the search saw 1 device in 10 s, an audio one: \"SPYDRONE\"; 1 inquiry result; 3 more lost on the way to the log",
      buf);
  p.start(0);
  p.note(find(20, "", -88, kPhone, false));
  p.summary(1000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("the search saw 1 device in 1 s, not an audio one; 1 inquiry result", buf);
  // The linked ones (left out of the list) say so.
  p.start(0);
  PairFind k = find(1, "SPYDRONE", -50, kHeadset, true);
  k.linked = true;
  p.note(k);
  p.note(find(2, "JBL Flip 5", -70, kSpeaker, true));
  p.summary(2000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "the search saw 2 devices in 2 s, 2 of them audio: \"SPYDRONE\" (linked), \"JBL Flip 5\"; 2 inquiry results", buf);
}

// A room full of audio devices: the names are cut with ", ...", the
// counts after them always fit.
void test_the_summary_is_cut_to_the_buffer() {
  PairFinds p;
  p.start(0);
  for (int i = 0; i < 12; ++i) {
    char name[32];
    snprintf(name, sizeof(name), "Some Long Speaker Name %02d", i);
    p.note(find(static_cast<uint8_t>(i), name, -70, kSpeaker, true));
  }
  char buf[200];
  p.summary(120000, 0, buf, sizeof(buf));
  TEST_ASSERT_TRUE(std::strlen(buf) < sizeof(buf));
  TEST_ASSERT_EQUAL_INT(0, std::strncmp(buf, "the search saw 12 devices in 120 s, 12 of them audio: \"Some Long", 64));
  TEST_ASSERT_NOT_NULL(std::strstr(buf, ", ...; 12 inquiry results"));
  const char* end = "; 12 inquiry results";
  TEST_ASSERT_EQUAL_STRING(end, buf + std::strlen(buf) - std::strlen(end));
  // A buffer too small for even the head: cut, terminated.
  char tiny[20];
  p.summary(120000, 0, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_STRING("the search saw 12 d", tiny);
}

// More devices than it tells apart: the rest are counted, never said
// (each of their results would otherwise look new).
void test_more_devices_than_it_tells_apart() {
  PairFinds p;
  p.start(0);
  for (int i = 0; i < PairFinds::kDevices; ++i) p.note(find(static_cast<uint8_t>(i), "", -80, kPhone, false));
  TEST_ASSERT_EQUAL_INT(PairFinds::kDevices, p.devices());
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(200, "Late Speaker", -60, kSpeaker, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(200, "Late Speaker", -60, kSpeaker, true)));
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(201, "", -60, kPhone, false)));
  TEST_ASSERT_EQUAL_UINT32(3, p.untold());
  TEST_ASSERT_EQUAL_UINT32(2, p.untoldAudio());
  // The ones it tells apart still do.
  TEST_ASSERT_EQUAL(Say::Nothing, p.note(find(0, "", -80, kPhone, false)));
  TEST_ASSERT_EQUAL_UINT32(3, p.untold());
  char buf[256];
  p.summary(60000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "the search saw more than 48 devices in 60 s, none of the first 48 audio; 52 inquiry results; 3 from devices "
      "past the first 48 (not told apart; 2 of those audio)",
      buf);
  // With audio among the first: counted against those.
  p.start(0);
  p.note(find(0, "SPYDRONE", -60, kHeadset, true));
  for (int i = 1; i < PairFinds::kDevices + 1; ++i) p.note(find(static_cast<uint8_t>(i), "", -80, kPhone, false));
  p.summary(60000, 0, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING(
      "the search saw more than 48 devices in 60 s, 1 of the first 48 audio: \"SPYDRONE\"; 49 inquiry results; 1 from "
      "devices past the first 48 (not told apart; 0 of those audio)",
      buf);
}

// From the BTC task to the loop: first in, first out across the wrap; a
// full ring drops and counts; clear() starts over.
void test_the_ring() {
  PairFindRing r;
  PairFind out;
  TEST_ASSERT_FALSE(r.pop(out));
  for (int round = 0; round < 3; ++round) {
    for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(r.push(find(static_cast<uint8_t>(round * 10 + i), "", -60, 0, false)));
    for (int i = 0; i < 10; ++i) {
      TEST_ASSERT_TRUE(r.pop(out));
      TEST_ASSERT_EQUAL_UINT8(round * 10 + i, out.addr[5]);
    }
  }
  for (int i = 0; i < PairFindRing::kSize; ++i) TEST_ASSERT_TRUE(r.push(find(static_cast<uint8_t>(i), "", -60, 0, false)));
  TEST_ASSERT_FALSE(r.push(find(99, "", -60, 0, false)));
  TEST_ASSERT_FALSE(r.push(find(98, "", -60, 0, false)));
  TEST_ASSERT_EQUAL_UINT32(2, r.dropped());
  TEST_ASSERT_EQUAL_INT(PairFindRing::kSize, r.size());
  TEST_ASSERT_TRUE(r.pop(out));
  TEST_ASSERT_EQUAL_UINT8(0, out.addr[5]);
  TEST_ASSERT_TRUE(r.push(find(97, "", -60, 0, false)));  // room again: the newest goes last
  for (int i = 1; i < PairFindRing::kSize; ++i) {
    TEST_ASSERT_TRUE(r.pop(out));
    TEST_ASSERT_EQUAL_UINT8(i, out.addr[5]);
  }
  TEST_ASSERT_TRUE(r.pop(out));
  TEST_ASSERT_EQUAL_UINT8(97, out.addr[5]);
  TEST_ASSERT_FALSE(r.pop(out));
  r.push(find(1, "", -60, 0, false));
  r.clear();
  TEST_ASSERT_EQUAL_INT(0, r.size());
  TEST_ASSERT_EQUAL_UINT32(0, r.dropped());
  TEST_ASSERT_FALSE(r.pop(out));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_each_audio_device_once_per_search);
  RUN_TEST(test_a_class_that_comes_later);
  RUN_TEST(test_the_lines);
  RUN_TEST(test_the_summary);
  RUN_TEST(test_the_summary_is_cut_to_the_buffer);
  RUN_TEST(test_more_devices_than_it_tells_apart);
  RUN_TEST(test_the_ring);
  return UNITY_END();
}
