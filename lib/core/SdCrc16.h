// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The SD card's CRC16 in SPI mode (the SD Physical Layer spec, 4.5): the CRC
// over each 512-byte data block, both ways. CRC-16/XMODEM: polynomial
// 0x1021, init 0, not reflected, no final xor (512 bytes of 0xFF give 0x7FA1,
// "123456789" 0x31C3).
//
// Two bytes a step from two 256-entry tables (slicing by 2): the same
// result as the byte-at-a-time table of lib/SD/src/sd_diskio_crc.c's CRC16(),
// which the driver used before (test_sd_crc16 checks them against each other
// and a bit-at-a-time reference). The tables (1 KB) are in RAM, not flash: a
// block's data comes from PSRAM through the same cache as flash, and a 32 KB
// write streams through the whole of it (docs/SWAP-MODE.md section 7.2).
//
// `crc` carries a CRC on across calls (0 for the first).
namespace sdcrc {
uint16_t crc16(const uint8_t* data, size_t n, uint16_t crc = 0);
}  // namespace sdcrc
