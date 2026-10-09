// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// zlib's CRC-32, the checksum of a dance pack (docs/DANCE-SPRITES.md, A.2):
// the reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR
// 0xFFFFFFFF, exactly what Python's `zlib.crc32(data) & 0xFFFFFFFF` gives,
// so tools/dancepack.py and the device agree on every pack.
//
// It is not ogg::crc32 (OggPage.h): Ogg's pages use the unreflected
// polynomial 0x04C11DB7 with no XORs, a different checksum that stays where
// it is.
//
// Table-driven, a byte at a time: the 1 KB table is made at compile time
// (constexpr, so it sits in flash as rodata), and a 128 KB card pack is
// checked once per load, never per frame. Portable, no allocation.
namespace dance {

// zlib's CRC-32 (A.2) of n bytes. `prev` continues an earlier result, as
// zlib.crc32's second argument does: crc32(b, nb, crc32(a, na)) is the CRC
// of a then b, and 0 starts. "123456789" gives 0xCBF43926. A 1 KB table
// made at compile time. p may be null when n is 0.
uint32_t crc32(const uint8_t* p, size_t n, uint32_t prev = 0);

}  // namespace dance
