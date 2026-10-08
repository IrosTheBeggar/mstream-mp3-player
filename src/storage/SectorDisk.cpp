// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/SectorDisk.h"

#include <esp_timer.h>
#include <sd_diskio.h>

#include "CachedDrive.h"
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

// All in internal RAM (the globals): only the cache's block and verify's
// sector are in PSRAM. The rules are lib/core CachedDrive's (host-tested):
// this adapts them to the SD driver.
uint8_t s_pdrv = 0xFF;
bool s_installed = false;

class SdCard final : public SectorCache::Device {
public:
  bool read(uint32_t lba, uint8_t* out, uint32_t count) override { return ff_sd_read(s_pdrv, out, lba, count) == RES_OK; }
  bool write(uint32_t lba, const uint8_t* data, uint32_t count) override {
    return ff_sd_write(s_pdrv, data, lba, count) == RES_OK;
  }
};

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

SectorCache s_cache;
SdCard s_card;
CachedDrive s_drive(s_cache, s_card, nowUs);
uint8_t* s_check = nullptr;  // verify's sector (PSRAM)

// FatFs (re)mounts the volume: at the first access after the mount, and by
// itself once the card stopped answering (ff_sd_status()'s STA_NOINIT: a
// card pulled or swapped while on). The cache is cleared first: nothing of
// the card that was there is served to the one that is.
DSTATUS wInit(unsigned char pdrv) {
  s_drive.init();
  return ff_sd_initialize(pdrv);
}
DSTATUS wStatus(unsigned char pdrv) { return ff_sd_status(pdrv); }

DRESULT wRead(unsigned char pdrv, unsigned char* buff, uint32_t sector, unsigned count) {
  (void)pdrv;
  return s_drive.read(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT wWrite(unsigned char pdrv, const unsigned char* buff, uint32_t sector, unsigned count) {
  (void)pdrv;
  return s_drive.write(sector, buff, count) ? RES_OK : RES_ERROR;
}

DRESULT wIoctl(unsigned char pdrv, unsigned char cmd, void* buff) {
  if (cmd == CTRL_TRIM && buff) {
    // FatFs's range: the first and the last sector freed.
    const LBA_t* r = static_cast<const LBA_t*>(buff);
    s_drive.trim(static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1]));
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
  s_drive.setScratch(s_check);
  s_pdrv = pdrv;
  s_drive.init();  // a mount: nothing kept from before
  ff_diskio_register(pdrv, &kImpl);
  s_installed = true;
  return true;
#else
  (void)pdrv;
  return false;
#endif
}

bool installed() { return s_installed; }

void setEnabled(bool on) { s_drive.setEnabled(on); }

bool enabled() { return s_installed && s_drive.enabled(); }

void setVerify(bool on) { s_drive.setVerify(on); }
bool verifying() { return s_drive.verifying(); }

Stats stats() {
  // Read while FatFs may be inside a call on another task: each field is a
  // word, read as it is (a diagnostic).
  const CachedDrive::Stats d = s_drive.stats();
  Stats s;
  s.cache = s_cache.stats();
  s.capacity = s_cache.capacity();
  s.held = s_cache.size();
  s.bytes = s_cache.bytes();
  s.cardReads = d.cardReads;
  s.cardSingleReads = d.cardSingleReads;
  s.cardReadSectors = d.cardReadSectors;
  s.cardReadUs = d.cardReadUs;
  s.cardWrites = d.cardWrites;
  s.trims = d.trims;
  s.inits = d.inits;
  s.verified = d.verified;
  s.stale = d.stale;
  s.staleLba = d.staleLba;
  return s;
}

void resetStats() { s_drive.resetStats(); }

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
