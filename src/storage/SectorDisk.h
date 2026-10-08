// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "SectorCache.h"

// The PSRAM sector cache under FatFs (docs/METADATA.md 3.2.4, 3.2.7;
// milestone N10): a diskio driver for the SD card's FatFs drive that
// forwards to the SD library's own (sd_diskio.cpp's ff_sd_initialize,
// ff_sd_status, ff_sd_read, ff_sd_write, ff_sd_ioctl) through N8's
// SectorCache (lib/core): 256 single sectors, 135,168 B of PSRAM, the
// directories' and the FAT's sectors that every path lookup reads again
// from the root. The rules are lib/core CachedDrive's (host-tested in
// test_sector_cache, and under FatFs in test_fat_model); this file adapts
// them to the SD driver.
//
//   - install(), right after every mount: SD.begin() registers the stock
//     driver each time (sdcard_init()), so the wrapper goes in after it, the
//     cache cleared first. LocalStorage::begin() runs before the audio
//     starts, so no other task is inside a disk call during the swap.
//     probeCard() mounts only to look and unmounts (SD.end()): no wrapper.
//   - disk_initialize (FatFs mounting the volume again by itself: after
//     ff_sd_status() said STA_NOINIT, the card pulled or swapped while on;
//     there is no card-detect) clears the cache before the SD driver's
//     init: the card that answers now may not be the one the cache read.
//   - CTRL_TRIM (FatFs trims the clusters it frees, FF_USE_TRIM): the range
//     is invalidated, then the SD driver's ioctl runs (it answers PARERR to
//     a trim: FatFs ignores the answer).
//   - Nothing writes the card around FatFs (SD.writeRAW() isn't used): if
//     something ever does, it must invalidate() the range.
//   - FatFs calls the driver under its volume's lock (one volume on the
//     card), which serialises the cache: it has no lock of its own.
//
// The device batch's A/B switches (6.3, L0 and L1), at runtime from the
// console (gc0, gc1, gc2):
//   - off: every call goes straight to the SD driver (the stock build's
//     reads; L0's "uncached" figures), and a write drops the sectors it
//     wrote from the cache. On again: the cache starts empty.
//   - verify: every cache hit is also read from the card and compared (L1's
//     write soak: a sector the cache holds that the card doesn't is counted
//     and its LBA kept). Costs the read the cache saves.
//   - The switches and the counts' reset are taken at the next disk call
//     (under FatFs's lock): stats() read right after a reset still has the
//     totals up to it.
// The build's default (MSTREAM_SECTOR_CACHE, 1: on) is the spec's choice;
// 0 builds the stock driver alone (no wrapper, no PSRAM taken).
#ifndef MSTREAM_SECTOR_CACHE
#define MSTREAM_SECTOR_CACHE 1
#endif

namespace sectordisk {

// After a successful SD.begin() for drive `pdrv`: the cache (allocated
// once, cleared every time) and the wrapper registered. False: the build
// leaves it out, or no PSRAM (the stock driver stays).
bool install(uint8_t pdrv);
bool installed();

void setEnabled(bool on);
bool enabled();
void setVerify(bool on);
bool verifying();

struct Stats {
  SectorCache::Stats cache;  // hits, misses, bypassed, writes, ... (SectorCache)
  uint32_t capacity = 0;     // sectors
  uint32_t held = 0;         // sectors held now
  size_t bytes = 0;          // its block
  // Every call that reached the SD driver (cache on or off): reads, the
  // sectors they moved, the time they took.
  uint32_t cardReads = 0;
  uint32_t cardSingleReads = 0;  // ... of one sector (FatFs's folders and FAT, a partial sector)
  uint32_t cardReadSectors = 0;
  uint64_t cardReadUs = 0;
  uint32_t cardWrites = 0;
  uint32_t trims = 0;
  uint32_t inits = 0;        // mounts (FatFs's own after a card stopped answering too): the cache cleared
  uint32_t verified = 0;     // hits compared with the card (verify on)
  uint32_t stale = 0;        // ... that differed: a cache bug (L1 fails)
  uint32_t staleLba = 0;     // the last such sector
};
Stats stats();
void resetStats();

// L0's per-sector figure: `n` single-sector reads straight from the SD
// driver (never the cache) at sectors spread over the card, timed. The
// mean, the fastest and the slowest in microseconds.
struct Bench {
  uint32_t reads = 0, failed = 0;
  uint32_t meanUs = 0, minUs = 0, maxUs = 0;
};
Bench benchReads(uint32_t n);

}  // namespace sectordisk
