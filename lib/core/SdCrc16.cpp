// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SdCrc16.h"

namespace sdcrc {
namespace {

// t0: a byte's table (the byte-at-a-time CRC's). t1: the table for the byte
// before it, one byte further on: t1[i] = (t0[i] << 8) ^ t0[t0[i] >> 8]. Two
// bytes then replace the whole register: with x = crc ^ (b0 << 8 | b1),
// crc = t1[x >> 8] ^ t0[x & 0xFF] (t0 is linear, so the second byte's index
// splits).
struct Tables {
  uint16_t t0[256];
  uint16_t t1[256];
};

constexpr Tables makeTables() {
  Tables t{};
  for (int i = 0; i < 256; ++i) {
    uint16_t c = static_cast<uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit) c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    t.t0[i] = c;
  }
  for (int i = 0; i < 256; ++i) t.t1[i] = static_cast<uint16_t>((t.t0[i] << 8) ^ t.t0[t.t0[i] >> 8]);
  return t;
}

// Not const: in RAM (.data), so the loop's loads never wait on the flash
// cache (SdCrc16.h).
Tables s_tables = makeTables();

}  // namespace

uint16_t crc16(const uint8_t* data, size_t n, uint16_t crc) {
  const uint16_t* t0 = s_tables.t0;
  const uint16_t* t1 = s_tables.t1;
  for (; n >= 2; data += 2, n -= 2) {
    const uint16_t x = static_cast<uint16_t>(crc ^ ((data[0] << 8) | data[1]));
    crc = static_cast<uint16_t>(t1[x >> 8] ^ t0[x & 0xFF]);
  }
  if (n) crc = static_cast<uint16_t>((crc << 8) ^ t0[(crc >> 8) ^ data[0]]);
  return crc;
}

}  // namespace sdcrc
