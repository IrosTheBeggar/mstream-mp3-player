// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <stdint.h>

// The console's write bench (gx, gxs: src/app/TagConsole.cpp; docs/SWAP-MODE.md
// section 7), in this copy's driver (sd_diskio.cpp, its "mstream-mp3-player:"
// lines; lib/SD/README.md). The timers run only while the bench is on; the CRC
// errors and the retries are counted always. Everything here is the loop task's,
// between the driver's transactions (no write under way).

typedef struct {
  uint32_t multiWrites;    // CMD25s that ended well, their CMD13 too
  uint32_t blocks;         // ... the blocks they carried
  uint64_t preEraseUs;     // ACMD23 before each CMD25 (CMD55 + CMD23)
  uint64_t crcUs;          // each block's CRC16, worked out (single writes' too)
  uint64_t blockBusyUs;    // the wait for the card before each block
  uint64_t wireUs;         // each block's token, 512 B, CRC and data response
  uint64_t stopBusyUs;     // a CMD25's end: the last block's busy, Stop Tran, the card's programming
  uint32_t maxStopBusyUs;  // ... the longest
  uint64_t statusUs;       // the CMD13 after each CMD25
  uint32_t crcErrors;      // R1s with the CRC bit, data blocks whose CRC16 failed
  uint32_t retries;        // CMD25s cut short by a refused block, the rest written again
  uint8_t lastRefusal;     // the last refused block's data response (0x0B CRC error, 0x0D write error; 0: none)
} sd_bench_stats_t;

// On: the counts zeroed and the timers running; off: the timers stop.
void sdcard_bench(bool on);
sd_bench_stats_t sdcard_bench_stats(void);
// ACMD23 (SET_WR_BLK_ERASE_COUNT) before every CMD25: on unless the bench
// turns it off.
void sdcard_set_pre_erase(bool on);
// The data blocks' CRC16: two bytes a step (lib/core SdCrc16.h, the default),
// or the stock CRC16() a byte a step (gxc0, for the bench's A/B). The same
// result either way.
void sdcard_set_crc16(bool fast);
bool sdcard_crc16_fast(void);
// The clock the driver asks the bus for (the HAL makes 80 MHz / n of it,
// rounded down), and the bench's setting of it, uncapped (the mount caps it
// at 25 MHz).
int sdcard_frequency(uint8_t pdrv);
void sdcard_set_frequency(uint8_t pdrv, int hz);
// The card's CID (CMD10) and CSD (CMD9) registers, 16 bytes each.
bool sdcard_read_cid(uint8_t pdrv, uint8_t *out);
bool sdcard_read_csd(uint8_t pdrv, uint8_t *out);
