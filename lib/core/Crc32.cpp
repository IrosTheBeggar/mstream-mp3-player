// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "Crc32.h"

namespace dance {

namespace {

struct CrcTable {
  uint32_t v[256];
};

constexpr CrcTable makeTable() {
  CrcTable t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    t.v[i] = c;
  }
  return t;
}

constexpr CrcTable kCrcTable = makeTable();
static_assert(kCrcTable.v[1] == 0x77073096u, "zlib's table");
static_assert(kCrcTable.v[255] == 0x2D02EF8Du, "zlib's table");

}  // namespace

uint32_t crc32(const uint8_t* p, size_t n, uint32_t prev) {
  uint32_t c = prev ^ 0xFFFFFFFFu;
  if (p) {
    for (size_t i = 0; i < n; ++i) c = kCrcTable.v[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

}  // namespace dance
