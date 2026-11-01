// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for HostStatus (docs/HOST-STATUS.md): the @status line (its
// fields in their order, each case of each, the percent-encoding, the
// 255-byte cap and how bt= gives way to it), @identify's label, the FSINFO
// rule, and when @count's progress lines go out.
// Run: pio test -e native -f test_host_status
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "HostLine.h"
#include "HostStatus.h"

using namespace hoststatus;

namespace {

std::string line(const Facts& f) {
  char buf[kMaxLine + 1];
  const size_t n = format(f, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_size_t(std::strlen(buf), n);
  return buf;
}

// A board playing from a 64 GB FAT32 card whose FSINFO count is valid,
// paired with "Paul's headphones".
Facts playing64() {
  Facts f;
  f.fw = "v0.9.0";
  f.elf = "63ee7a2b";
  f.card = Card::Fat32;
  f.sizeBytes = 63864569856ull;  // 124,735,488 sectors
  f.freeKnown = true;
  f.freeBytes = 38214565888ull;
  f.tracks = Tracks::Count;
  f.trackCount = 1284;
  f.battery = 87;
  f.state = State::Playing;
  f.bt = "Paul's headphones";
  return f;
}

// The value of `key` in a line ("" when absent).
std::string field(const std::string& l, const char* key) {
  const std::string k = std::string(" ") + key + "=";
  const size_t at = l.find(k);
  if (at == std::string::npos) return "";
  const size_t from = at + k.size();
  const size_t to = l.find(' ', from);
  return l.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

// What a URL decoder makes of a value.
std::string decode(const std::string& v) {
  std::string out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == '%' && i + 2 < v.size()) {
      out += static_cast<char>(std::stoi(v.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      out += v[i];
    }
  }
  return out;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- @status ----

// The line for the board above, exactly: the fields in their order.
void test_status_line_exact() {
  TEST_ASSERT_EQUAL_STRING(
      "@status fw=v0.9.0 elf=63ee7a2b card=fat32 size=63864569856 free=38214565888 tracks=1284 music=? bat=87 "
      "state=playing bt=Paul%27s%20headphones",
      line(playing64()).c_str());
}

// Each field's other cases: nothing known, building, counting, no card.
void test_status_unknowns() {
  Facts f;
  f.fw = "v0.9.0-3-gabc1234-dirty";
  f.elf = "0123abcd";
  TEST_ASSERT_EQUAL_STRING(
      "@status fw=v0.9.0-3-gabc1234-dirty elf=0123abcd card=none size=- free=? tracks=- music=? bat=- state=idle bt=-",
      line(f).c_str());
  f = playing64();
  f.freeKnown = false;  // FSINFO's count unknown: '?', never 0 or the size
  TEST_ASSERT_EQUAL_STRING("?", field(line(f), "free").c_str());
  f.counting = true;  // counting wins over whatever is known
  f.freeKnown = true;
  TEST_ASSERT_EQUAL_STRING("counting", field(line(f), "free").c_str());
  f = playing64();
  f.tracks = Tracks::Building;
  TEST_ASSERT_EQUAL_STRING("building", field(line(f), "tracks").c_str());
  f.tracks = Tracks::Count;
  f.trackCount = 0;  // an empty /music
  TEST_ASSERT_EQUAL_STRING("0", field(line(f), "tracks").c_str());
  f.musicKnown = true;
  f.musicBytes = 20871651328ull;
  TEST_ASSERT_EQUAL_STRING("20871651328", field(line(f), "music").c_str());
  f.battery = 130;  // a reading above 100 reads 100
  TEST_ASSERT_EQUAL_STRING("100", field(line(f), "bat").c_str());
  f.battery = 0;
  TEST_ASSERT_EQUAL_STRING("0", field(line(f), "bat").c_str());
  f.bt = "";
  TEST_ASSERT_EQUAL_STRING("-", field(line(f), "bt").c_str());
  // The states and the cards, each by its name.
  const State states[] = {State::Idle, State::Paused, State::Playing, State::Host};
  const char* stateNames[] = {"idle", "paused", "playing", "host"};
  for (int i = 0; i < 4; ++i) {
    f.state = states[i];
    TEST_ASSERT_EQUAL_STRING(stateNames[i], field(line(f), "state").c_str());
  }
  const Card cards[] = {Card::None, Card::Fat32, Card::Fat16, Card::ExFat,
                        Card::Ntfs, Card::Gpt,   Card::Other, Card::Unreadable};
  const char* cardNames[] = {"none", "fat32", "fat16", "exfat", "ntfs", "gpt", "other", "unreadable"};
  for (int i = 0; i < 8; ++i) {
    f.card = cards[i];
    TEST_ASSERT_EQUAL_STRING(cardNames[i], field(line(f), "card").c_str());
  }
  // A 2 TB card's numbers, and the biggest a field can hold.
  f.sizeBytes = 2000398934016ull;
  TEST_ASSERT_EQUAL_STRING("2000398934016", field(line(f), "size").c_str());
  f.freeBytes = 18446744073709551615ull;
  TEST_ASSERT_EQUAL_STRING("18446744073709551615", field(line(f), "free").c_str());
}

// Every value is one token of 0x21-0x7E: nothing in the line but single
// spaces between key=value pairs, and the keys in their order.
void test_status_is_a_host_line() {
  Facts f = playing64();
  f.fw = "v1 2\t3";  // (can't happen: a space or a control byte would break the line)
  f.bt = "My @ headphones = 100% \xC3\xA9t\xC3\xA9 \xF0\x9F\x8E\xA7";
  const std::string l = line(f);
  const char* keys[] = {"fw", "elf", "card", "size", "free", "tracks", "music", "bat", "state", "bt"};
  size_t at = 0;
  for (const char* k : keys) {
    const size_t p = l.find(std::string(" ") + k + "=", at);
    TEST_ASSERT_TRUE_MESSAGE(p != std::string::npos, k);
    at = p + 1;
  }
  TEST_ASSERT_EQUAL_STRING("v1_2_3", field(l, "fw").c_str());
  for (size_t i = 0; i < l.size(); ++i) {
    const auto c = static_cast<unsigned char>(l[i]);
    TEST_ASSERT_TRUE(c >= 0x20 && c <= 0x7E);
    if (c == ' ') TEST_ASSERT_TRUE(i + 1 < l.size() && l[i + 1] != ' ');
  }
  TEST_ASSERT_EQUAL_UINT32(1, std::count(l.begin(), l.end(), '@'));
  // The headphones' name comes back from a URL decoder byte for byte.
  TEST_ASSERT_EQUAL_STRING(f.bt, decode(field(l, "bt")).c_str());
  // And the console reads it as one host line, its verb and ten fields.
  char buf[kMaxLine + 1];
  std::snprintf(buf, sizeof(buf), "%s", l.c_str());
  HostFields hf;
  TEST_ASSERT_TRUE(splitHostLine(buf, &hf));
  TEST_ASSERT_EQUAL_STRING("status", hf.verb);
  TEST_ASSERT_EQUAL_INT(8, hf.count);  // the most a host line's reader takes ...
  TEST_ASSERT_TRUE(hf.more);           // ... the rest ignored: a reader splits on spaces itself
}

// The percent-encoding: unreserved bytes as they are, the rest %XX; a
// cut never splits a character.
void test_percent_encode() {
  char out[64];
  size_t taken = 0;
  TEST_ASSERT_EQUAL_size_t(21, percentEncode("Paul's headphones", 17, out, 63, &taken));
  TEST_ASSERT_EQUAL_STRING("Paul%27s%20headphones", out);
  TEST_ASSERT_EQUAL_size_t(17, taken);
  percentEncode("A-Z_a.z~09", 10, out, 63);
  TEST_ASSERT_EQUAL_STRING("A-Z_a.z~09", out);
  percentEncode("%@ +=", 5, out, 63);
  TEST_ASSERT_EQUAL_STRING("%25%40%20%2B%3D", out);
  // "é" (2 bytes) and "🎧" (4): whole or not at all.
  const char* s = "e\xC3\xA9\xF0\x9F\x8E\xA7";
  TEST_ASSERT_EQUAL_size_t(7, percentEncode(s, 7, out, 7, &taken));  // e%C3%A9: 7 bytes
  TEST_ASSERT_EQUAL_STRING("e%C3%A9", out);
  TEST_ASSERT_EQUAL_size_t(3, taken);
  TEST_ASSERT_EQUAL_size_t(1, percentEncode(s, 7, out, 6, &taken));  // not half of é
  TEST_ASSERT_EQUAL_STRING("e", out);
  TEST_ASSERT_EQUAL_size_t(19, percentEncode(s, 7, out, 19, &taken));
  TEST_ASSERT_EQUAL_STRING("e%C3%A9%F0%9F%8E%A7", out);
  TEST_ASSERT_EQUAL_size_t(7, taken);
  TEST_ASSERT_EQUAL_size_t(7, percentEncode(s, 7, out, 18, &taken));  // the 4-byte one whole or not
  // A stray continuation byte, or a lead byte cut off: a byte alone.
  percentEncode("\x80" "a\xC3", 3, out, 63, &taken);
  TEST_ASSERT_EQUAL_STRING("%80a%C3", out);
  TEST_ASSERT_EQUAL_size_t(3, taken);
}

// The cap: 255 bytes from the '@', the fields before bt= whole; a long
// name gives way, cut at a character and ending in "…", and a decoder
// reads back a prefix of it.
void test_status_cap_and_bt() {
  Facts f = playing64();
  // The longest the fields before bt= can be (every one at its widest).
  f.fw = "v10.10.10-rc.10-9999-gabcdef12-dirty-0123456789abcdef";  // cut to 48
  f.elf = "0123456789abcdef0123";                                // cut to 16
  f.card = Card::Unreadable;
  f.sizeBytes = f.freeBytes = 18446744073709551615ull;
  f.trackCount = 4294967295u;
  f.musicKnown = true;
  f.musicBytes = 18446744073709551615ull;
  f.battery = 100;
  f.bt = "";
  const std::string widest = line(f);
  const size_t before = widest.size() - 1;  // without bt's '-'
  char msg[64];
  std::snprintf(msg, sizeof(msg), "the fields before bt's value: %u bytes", static_cast<unsigned>(before));
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(before <= 219);
  TEST_ASSERT_EQUAL_size_t(48, field(widest, "fw").size());
  TEST_ASSERT_EQUAL_size_t(16, field(widest, "elf").size());
  // A name that fits exactly: whole, no "…".
  std::string name(kMaxLine - before, 'x');
  f.bt = name.c_str();
  std::string l = line(f);
  TEST_ASSERT_EQUAL_size_t(kMaxLine, l.size());
  TEST_ASSERT_EQUAL_STRING(name.c_str(), field(l, "bt").c_str());
  // One byte more: cut, "…" at its end, the line exactly at the cap or a
  // character short of it.
  name += "y";
  f.bt = name.c_str();
  l = line(f);
  TEST_ASSERT_TRUE(l.size() <= kMaxLine && l.size() >= kMaxLine - 2);
  std::string v = field(l, "bt");
  TEST_ASSERT_EQUAL_STRING(kEllipsis, v.substr(v.size() - std::strlen(kEllipsis)).c_str());
  // A name of spaces and accents (3 and 6 bytes encoded a character): never
  // half a %XX or half a character; what it says is a prefix of the name.
  std::string accents;
  for (int i = 0; i < 40; ++i) accents += i % 2 ? " " : "\xC3\xA9";
  f = playing64();
  f.bt = accents.c_str();
  l = line(f);
  TEST_ASSERT_TRUE(l.size() <= kMaxLine);
  v = field(l, "bt");
  const std::string kept = decode(v.substr(0, v.size() - std::strlen(kEllipsis)));
  TEST_ASSERT_TRUE(kept.size() > 0 && accents.compare(0, kept.size(), kept) == 0);
  TEST_ASSERT_TRUE((kept.size() % 3) == 0 || (kept.size() % 3) == 2);  // é is 2 bytes, " é" 3: whole ones
  // BtSink keeps a name of up to 63 bytes: a typical line holds it whole.
  std::string longName(63, 'Q');
  f = playing64();
  f.bt = longName.c_str();
  TEST_ASSERT_EQUAL_STRING(longName.c_str(), field(line(f), "bt").c_str());
  // The output buffer's size cuts too (a caller's short buffer).
  char small[16];
  TEST_ASSERT_EQUAL_size_t(15, format(playing64(), small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("@status fw=v0.9", small);
}

// ---- @identify's label ----

void test_identify_label() {
  TEST_ASSERT_TRUE(validLabel("COM5"));
  TEST_ASSERT_TRUE(validLabel("ttyACM0"));
  TEST_ASSERT_TRUE(validLabel("cu.usbmodem14101"));  // 16
  TEST_ASSERT_TRUE(validLabel("!"));
  TEST_ASSERT_TRUE(validLabel("~~~~~~~~~~~~~~~~"));
  TEST_ASSERT_FALSE(validLabel("cu.usbserial-14130"));  // 18: too long
  TEST_ASSERT_FALSE(validLabel(""));
  TEST_ASSERT_FALSE(validLabel(nullptr));
  TEST_ASSERT_FALSE(validLabel("COM 5"));
  TEST_ASSERT_FALSE(validLabel("COM\x7F"));
  TEST_ASSERT_FALSE(validLabel("\xC3\xA9"));
}

// ---- the FSINFO rule ----

void test_fsinfo_rule() {
  // A 64 GB card: 1,948,672 clusters of 32 KB, so 1,948,674 entries.
  const uint32_t entries = 1948674;
  TEST_ASSERT_TRUE(freeCountValid(0, entries));               // full: a count
  TEST_ASSERT_TRUE(freeCountValid(1166216, entries));
  TEST_ASSERT_TRUE(freeCountValid(entries - 2, entries));     // empty: every cluster
  TEST_ASSERT_FALSE(freeCountValid(entries - 1, entries));    // more than the clusters: broken
  TEST_ASSERT_FALSE(freeCountValid(0xFFFFFFFFu, entries));    // FSINFO's "unknown" (FatFs's too)
  TEST_ASSERT_FALSE(freeCountValid(0x0FFFFFFFu, entries));
  TEST_ASSERT_FALSE(freeCountValid(0, 2));                    // no volume
  TEST_ASSERT_FALSE(freeCountValid(0, 0));
}

// ---- @count's progress lines ----

void test_count_progress() {
  Progress p;
  p.start(1000);  // "@count 0"
  TEST_ASSERT_EQUAL_UINT8(0, p.last());
  TEST_ASSERT_FALSE(p.due(0, 1000));
  TEST_ASSERT_FALSE(p.due(9, 1500));   // not a tenth yet
  TEST_ASSERT_TRUE(p.due(10, 1600));   // 10
  TEST_ASSERT_FALSE(p.due(19, 1700));
  TEST_ASSERT_TRUE(p.due(37, 1800));   // a jump past 20: the percent as it is
  TEST_ASSERT_EQUAL_UINT8(37, p.last());
  TEST_ASSERT_FALSE(p.due(39, 1900));
  TEST_ASSERT_TRUE(p.due(40, 2000));
  // Slow: 5 s without a line while it moves gives one.
  TEST_ASSERT_FALSE(p.due(41, 6999));
  TEST_ASSERT_TRUE(p.due(41, 7000));
  TEST_ASSERT_FALSE(p.due(41, 20000));  // not moving: nothing
  TEST_ASSERT_FALSE(p.due(40, 20000));  // never backwards
  // The end: 99 (pieces read again after a write) and then 100.
  TEST_ASSERT_TRUE(p.due(99, 21000));
  TEST_ASSERT_TRUE(p.due(100, 21001));
  TEST_ASSERT_FALSE(p.due(100, 30000));
  // A count from 0 straight to done: 100 at once.
  p.start(0);
  TEST_ASSERT_TRUE(p.due(100, 1));
  // Every tenth of a slow, even count, and nothing else.
  p.start(0);
  int lines = 0;
  for (int pct = 0; pct <= 100; ++pct) lines += p.due(static_cast<uint8_t>(pct), static_cast<uint32_t>(pct) * 40);
  TEST_ASSERT_EQUAL_INT(10, lines);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_status_line_exact);
  RUN_TEST(test_status_unknowns);
  RUN_TEST(test_status_is_a_host_line);
  RUN_TEST(test_percent_encode);
  RUN_TEST(test_status_cap_and_bt);
  RUN_TEST(test_identify_label);
  RUN_TEST(test_fsinfo_rule);
  RUN_TEST(test_count_progress);
  return UNITY_END();
}
