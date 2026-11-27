// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for lib/core SdCrc16.h, the CRC16 the patched SD driver
// (lib/SD/src/sd_diskio.cpp) puts on and checks over every data block: the
// SD spec's example and CRC-16/XMODEM's check value, a bit-at-a-time
// reference, and the driver's old byte-at-a-time table (the framework's
// sd_diskio_crc.c, compiled in here as it is) over random blocks of every
// length up to two sectors, whole and in pieces.
// Run: pio test -e native -f test_sd_crc16
#include <unity.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "SdCrc16.h"

// The framework's file, unchanged (lib/SD/README.md): CRC7() and CRC16().
#include "../../lib/SD/src/sd_diskio_crc.c"

void setUp() {}
void tearDown() {}

namespace {

// CRC-16/XMODEM a bit at a time: the definition.
uint16_t bitwise(const uint8_t* p, size_t n, uint16_t crc = 0) {
  for (size_t i = 0; i < n; ++i) {
    crc = static_cast<uint16_t>(crc ^ (p[i] << 8));
    for (int b = 0; b < 8; ++b) crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc;
}

uint32_t s_rng = 0x2545F491u;
uint8_t nextByte() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return static_cast<uint8_t>(s_rng >> 24);
}

}  // namespace

// The SD Physical Layer spec's example (4.5): a block of 512 bytes of 0xFF
// has the CRC16 0x7FA1. And the catalogue's check value for CRC-16/XMODEM.
void test_known_values() {
  uint8_t block[512];
  memset(block, 0xFF, sizeof(block));
  TEST_ASSERT_EQUAL_HEX16(0x7FA1, sdcrc::crc16(block, sizeof(block)));
  const char* check = "123456789";
  TEST_ASSERT_EQUAL_HEX16(0x31C3, sdcrc::crc16(reinterpret_cast<const uint8_t*>(check), 9));
  TEST_ASSERT_EQUAL_HEX16(0x0000, sdcrc::crc16(block, 0));
  TEST_ASSERT_EQUAL_HEX16(0x1234, sdcrc::crc16(block, 0, 0x1234));  // nothing to add: the CRC as it was
  memset(block, 0, sizeof(block));
  TEST_ASSERT_EQUAL_HEX16(0x0000, sdcrc::crc16(block, sizeof(block)));
}

// Random data, every length from 0 to 1,024 bytes and at every offset in a
// word: the bit-at-a-time definition, and the driver's old function, agree.
void test_against_the_references() {
  std::vector<uint8_t> buf(1024 + 4);
  for (auto& b : buf) b = nextByte();
  for (size_t off = 0; off < 4; ++off) {
    for (size_t n = 0; n <= 1024; ++n) {
      const uint8_t* p = buf.data() + off;
      const uint16_t got = sdcrc::crc16(p, n);
      TEST_ASSERT_EQUAL_HEX16(bitwise(p, n), got);
      TEST_ASSERT_EQUAL_HEX16(CRC16(reinterpret_cast<const char*>(p), static_cast<int>(n)), got);
    }
  }
}

// A CRC carried on across pieces of any split equals the whole's.
void test_in_pieces() {
  std::vector<uint8_t> buf(512);
  for (int round = 0; round < 200; ++round) {
    for (auto& b : buf) b = nextByte();
    const uint16_t whole = sdcrc::crc16(buf.data(), buf.size());
    const size_t cut = nextByte() * 2u + (nextByte() & 1u);  // 0-511, odd ones too
    const uint16_t first = sdcrc::crc16(buf.data(), cut);
    TEST_ASSERT_EQUAL_HEX16(whole, sdcrc::crc16(buf.data() + cut, buf.size() - cut, first));
  }
}

// Many random blocks, as the driver sees them: the old and the new agree
// on each, so a card's CRC check can't tell them apart.
void test_random_blocks() {
  uint8_t block[512];
  for (int round = 0; round < 5000; ++round) {
    for (auto& b : block) b = nextByte();
    TEST_ASSERT_EQUAL_HEX16(CRC16(reinterpret_cast<const char*>(block), 512), sdcrc::crc16(block, 512));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_known_values);
  RUN_TEST(test_against_the_references);
  RUN_TEST(test_in_pieces);
  RUN_TEST(test_random_blocks);
  return UNITY_END();
}
