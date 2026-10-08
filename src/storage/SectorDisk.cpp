// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/SectorDisk.h"

#include <esp_timer.h>
#include <sd_diskio.h>

#include <atomic>
#include <cstring>

#include "app/Psram.h"
#include "diskio_impl.h"  // ff_diskio_register(), ff_diskio_impl_t
#include "ff.h"

// The SD library's FatFs driver (sd_diskio.cpp): global, with no header
// that declares them; the same signatures as its definitions.
DSTATUS ff_sd_initialize(uint8_t pdrv);
DSTATUS ff_sd_status(uint8_t pdrv);
DRESULT ff_sd_read(uint8_t pdrv, uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_write(uint8_t pdrv, const uint8_t* buffer, DWORD sector, UINT count);
DRESULT ff_sd_ioctl(uint8_t pdrv, uint8_t cmd, void* buff);

namespace sectordisk {

namespace {

// All in internal RAM (the globals): only the cache's block is in PSRAM.
SectorCache s_cache;
uint8_t s_pdrv = 0xFF;
bool s_installed = false;
std::atomic<bool> s_enabled{true};
std::atomic<bool> s_clear{false};   // clear at the next disk call (under FatFs's lock)
std::atomic<bool> s_reset{false};   // reset the stats at the next disk call
std::atomic<bool> s_verify{false};
uint8_t* s_check = nullptr;         // verify's sector (PSRAM)
Stats s_stats;

class SdDevice final : public SectorCache::Device {
public:
  bool read(uint32_t lba, uint8_t* out, uint32_t count) override {
    const int64_t t0 = esp_timer_get_time();
    const bool ok = ff_sd_read(s_pdrv, out, lba, count) == RES_OK;
    s_stats.cardReadUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
    ++s_stats.cardReads;
    if (count == 1) ++s_stats.cardSingleReads;
    s_stats.cardReadSectors += count;
    return ok;
  }
  bool write(uint32_t lba, const uint8_t* data, uint32_t count) override {
    ++s_stats.cardWrites;
    return ff_sd_write(s_pdrv, data, lba, count) == RES_OK;
  }
};
SdDevice s_device;

// Pending requests from other tasks, taken under FatFs's lock.
void settle() {
  if (s_reset.exchange(false)) {
    s_cache.resetStats();
    const uint32_t capacity = s_stats.capacity;
    s_stats = Stats();
    s_stats.capacity = capacity;
  }
  if (s_clear.exchange(false)) s_cache.clear();
}

DSTATUS wInit(unsigned char pdrv) { return ff_sd_initialize(pdrv); }
DSTATUS wStatus(unsigned char pdrv) { return ff_sd_status(pdrv); }

DRESULT wRead(unsigned char pdrv, unsigned char* buff, uint32_t sector, unsigned count) {
  settle();
  if (!s_enabled.load(std::memory_order_relaxed)) {
    return s_device.read(sector, buff, count) ? RES_OK : RES_ERROR;
  }
  const bool check = s_verify.load(std::memory_order_relaxed) && s_check && count == 1 && s_cache.holds(sector);
  if (!s_cache.read(s_device, sector, buff, count)) return RES_ERROR;
  if (check && s_device.read(sector, s_check, 1)) {
    ++s_stats.verified;
    if (std::memcmp(s_check, buff, SectorCache::kSectorBytes) != 0) {
      ++s_stats.stale;
      s_stats.staleLba = sector;
      std::memcpy(buff, s_check, SectorCache::kSectorBytes);  // the card's bytes win
      s_cache.invalidate(sector, 1);
    }
  }
  (void)pdrv;
  return RES_OK;
}

DRESULT wWrite(unsigned char pdrv, const unsigned char* buff, uint32_t sector, unsigned count) {
  settle();
  (void)pdrv;
  if (!s_enabled.load(std::memory_order_relaxed)) return s_device.write(sector, buff, count) ? RES_OK : RES_ERROR;
  return s_cache.write(s_device, sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT wIoctl(unsigned char pdrv, unsigned char cmd, void* buff) {
  settle();
  if (cmd == CTRL_TRIM && buff) {
    // FatFs's range: the first and the last sector freed.
    const LBA_t* r = static_cast<const LBA_t*>(buff);
    if (r[1] >= r[0]) s_cache.invalidate(static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1] - r[0] + 1));
    ++s_stats.trims;
  }
  return ff_sd_ioctl(pdrv, cmd, buff);
}

const ff_diskio_impl_t kImpl = {&wInit, &wStatus, &wRead, &wWrite, &wIoctl};

}  // namespace

bool install(uint8_t pdrv) {
#if MSTREAM_SECTOR_CACHE
  if (pdrv == 0xFF) return false;
  if (s_cache.capacity() == 0 && !s_cache.begin(SectorCache::kDefaultEntries, psramAlloc, psramFree)) return false;
  if (!s_check) s_check = static_cast<uint8_t*>(psramAlloc(SectorCache::kSectorBytes));
  s_pdrv = pdrv;
  s_cache.clear();  // a mount: nothing kept from before
  s_clear.store(false);
  s_stats.capacity = s_cache.capacity();
  ff_diskio_register(pdrv, &kImpl);
  s_installed = true;
  return true;
#else
  (void)pdrv;
  return false;
#endif
}

bool installed() { return s_installed; }

void setEnabled(bool on) {
  if (on && !s_enabled.load()) s_clear.store(true);  // what was written while off isn't in it
  s_enabled.store(on);
}

bool enabled() { return s_installed && s_enabled.load(); }

void setVerify(bool on) { s_verify.store(on); }
bool verifying() { return s_verify.load(); }

Stats stats() {
  // Read while FatFs may be inside a call on another task: each field is a
  // word, read as it is (a diagnostic).
  Stats s = s_stats;
  s.cache = s_cache.stats();
  s.capacity = s_cache.capacity();
  s.held = s_cache.size();
  s.bytes = s_cache.bytes();
  return s;
}

void resetStats() { s_reset.store(true); }

Bench benchReads(uint32_t n) {
  Bench b;
  if (s_pdrv == 0xFF || n == 0) return b;
  uint8_t* buf = static_cast<uint8_t*>(psramAlloc(SectorCache::kSectorBytes));
  if (!buf) return b;
  const uint32_t sectors = sdcard_num_sectors(s_pdrv);
  uint64_t sum = 0;
  b.minUs = UINT32_MAX;
  for (uint32_t i = 0; i < n; ++i) {
    // Spread over the card (a prime stride), never the same sector twice in a row.
    const uint32_t lba = sectors ? static_cast<uint32_t>((static_cast<uint64_t>(i) * 2654435761u + 8192u) % sectors) : i;
    const int64_t t0 = esp_timer_get_time();
    const bool ok = ff_sd_read(s_pdrv, buf, lba, 1) == RES_OK;
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
    if (!ok) {
      ++b.failed;
      continue;
    }
    ++b.reads;
    sum += us;
    if (us < b.minUs) b.minUs = us;
    if (us > b.maxUs) b.maxUs = us;
  }
  psramFree(buf);
  b.meanUs = b.reads ? static_cast<uint32_t>(sum / b.reads) : 0;
  if (b.reads == 0) b.minUs = 0;
  return b;
}

}  // namespace sectordisk
