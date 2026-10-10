// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/LocalStorage.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <dirent.h>
#include <esp_partition.h>
#include <sd_diskio.h>
#include <sys/stat.h>

#include <cstring>

#include "UiText.h"
#include "storage/CardFat.h"
#include "storage/SectorDisk.h"
#include "diskio.h"  // FatFs: disk_initialize(), disk_read() (after ff.h's ffconf names them)
#include "ff.h"

namespace {
constexpr const char* kMusicDir = "/music";
constexpr const char* kStateDir = "/.player";
constexpr const char* kSdMount = "/sd";
constexpr const char* kFlashMount = "/littlefs";
constexpr const char* kFlashLabel = "spiffs";  // LittleFS.begin()'s default partition (partitions.csv)
// The VFS path: the mount point, then what the callback sees ("/music/...").
constexpr size_t kPathMax = 300;
constexpr uint32_t kSdHz = 25000000;

// The SD library's FatFs drive (SDFS::_pdrv, protected): read through a
// member pointer, which a derived class may name.
struct SdDrive : fs::SDFS {
  static uint8_t of(const fs::SDFS& sd) { return sd.*(&SdDrive::_pdrv); }
};

// One sector of a card that didn't mount, through the SD driver's FatFs
// disk (cardformat's reader; ctx: the drive number).
bool readRawSector(uint32_t lba, uint8_t* out, void* ctx) {
  return disk_read(*static_cast<const uint8_t*>(ctx), out, lba, 1) == RES_OK;
}

struct Walk {
  char path[kPathMax];
  size_t prefix;  // the mount point's length: path + prefix is "/music/..."
  int maxDepth;
  void (*fn)(const char*, void*);
  void* ctx;
};

// One directory: `w.path` holds its path (`len` bytes). A path that would
// be too long is skipped (the index takes up to 255 bytes anyway).
uint32_t walk(Walk& w, size_t len, int depth) {
  DIR* d = opendir(w.path);
  if (!d) return 0;
  uint32_t n = 0;
  while (const dirent* e = readdir(d)) {
    const char* name = e->d_name;
    if (name[0] == '.') continue;  // ".", "..", and hidden files and folders
    const size_t nameLen = std::strlen(name);
    if (len + 1 + nameLen + 1 > sizeof(w.path) || len + 1 + nameLen - w.prefix > 255) continue;
    w.path[len] = '/';
    std::memcpy(w.path + len + 1, name, nameLen + 1);
    bool isDir = e->d_type == DT_DIR;
    if (e->d_type != DT_DIR && e->d_type != DT_REG) {  // the VFS didn't say: ask
      struct stat st;
      isDir = stat(w.path, &st) == 0 && S_ISDIR(st.st_mode);
    }
    if (isDir) {
      if (depth < w.maxDepth) n += walk(w, len + 1 + nameLen, depth + 1);
    } else {
      w.fn(w.path + w.prefix, w.ctx);
      ++n;
    }
    w.path[len] = 0;
  }
  closedir(d);
  return n;
}
}  // namespace

uint32_t LocalStorage::forEachFile(void (*fn)(const char* path, void* ctx), void* ctx, int maxDepth) {
  if (!available()) return 0;
  // The path (~300 B) is on the caller's stack (the loop task's is 8 KB);
  // each folder level adds a small frame and an open DIR (VFS-allocated).
  Walk w;
  w.prefix = std::strlen(mount_);
  w.maxDepth = maxDepth;
  w.fn = fn;
  w.ctx = ctx;
  const int len = snprintf(w.path, sizeof(w.path), "%s%s", mount_, kMusicDir);
  return walk(w, static_cast<size_t>(len), 0);
}

const char* LocalStorage::stateDir() {
  if (available() && !stateDirMade_) {
    if (!fs_->exists(kStateDir)) fs_->mkdir(kStateDir);
    stateDirMade_ = true;
  }
  return kStateDir;
}

bool LocalStorage::begin() {
  // The SD card shares the LCD's SPI bus; M5Unified knows the pins.
  const int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk), M5.getPin(m5::pin_name_t::sd_spi_miso),
            M5.getPin(m5::pin_name_t::sd_spi_mosi), cs);
  // SD.begin()'s last argument, format_if_empty, stays false (the default),
  // here and in probeCard(): true makes sdcard_mount() run f_mkfs(FM_ANY)
  // on any card FatFs finds no FAT volume on (FR_NO_FILESYSTEM, in
  // sd_diskio.cpp's sdcard_mount()), which is every exFAT, NTFS or GPT
  // card and every blank one: a card with someone's music on it, wiped at
  // boot without a word. Formatting is the listener's choice, asked first.
  if (SD.begin(cs, SPI, kSdHz, kSdMount)) {
    fs_ = &SD;
    name_ = "SD";
    mount_ = kSdMount;
    // The card's FatFs drive, for what reads it through FatFs itself (the
    // walk, the scan, the device's records: storage/CardFat), and the PSRAM
    // sector cache under it (storage/SectorDisk: docs/METADATA.md 3.2.4).
    // SD.begin() registered the stock driver: the wrapper goes in after it,
    // before the audio starts (no other task is inside a disk call).
    const uint8_t pdrv = SdDrive::of(SD);
    cardfat::setDrive(pdrv);
    const bool cached = sectordisk::install(pdrv);
    Serial.printf("[storage] SD card on FatFs drive %u; the sector cache: %s\n", static_cast<unsigned>(pdrv),
                  cached ? "on (256 sectors, 135168 B of PSRAM)"
                  : MSTREAM_SECTOR_CACHE ? "OFF (no PSRAM)" : "off (this build: MSTREAM_SECTOR_CACHE=0)");
    // The wrapper's guard (METADATA.md 3.8): a card put in while the
    // player is on is held to this one, and write-protected if another.
    char id[112];
    sectordisk::identityText(id, sizeof(id));
    Serial.printf("[storage] the card's identity: %s%s\n", id,
                  !cached                     ? " (no guard: a card swapped while on isn't noticed)"
                  : sectordisk::guard().known ? ""
                                              : " (any remount restarts: nothing to compare it with)");
    return true;
  }
  lookAtCard(cs);  // a card that isn't FAT32 says so (the empty state)
  if (LittleFS.begin(true /* format the partition if it has never been used */, kFlashMount)) {
    fs_ = &LittleFS;
    name_ = "flash";
    mount_ = kFlashMount;
  }
  return available();
}

bool LocalStorage::probeCard() {
  if (onCard()) return true;
  const int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  if (!SD.begin(cs, SPI, kSdHz, kSdMount)) {  // never format_if_empty: begin()
    lookAtCard(cs);
    return false;
  }
  SD.end();  // only a look: the restart mounts it properly
  return true;
}

void LocalStorage::lookAtCard(int cs) {
  const uint32_t t0 = millis();
  cardKind_ = cardformat::Kind::Unreadable;
  // The same driver SD.begin() used (it let go of its drive when the mount
  // failed), without the mount: the card initialised, its sectors read.
  uint8_t pdrv = sdcard_init(static_cast<uint8_t>(cs), &SPI, kSdHz);
  if (pdrv != 0xFF) {
    if ((disk_initialize(pdrv) & STA_NOINIT) == 0) {
      uint8_t sector[cardformat::kSectorBytes];
      cardKind_ = cardformat::classify(readRawSector, &pdrv, sector);
    }
    sdcard_uninit(pdrv);
  }
  Serial.printf("[storage] no card mounted; its first sectors: %s%s; the pages say \"%s\" (%lu ms)\n",
                cardformat::name(cardKind_), cardformat::notFat32(cardKind_) ? ": not FAT32 (MBR)" : "",
                uitext::cardMessage(cardKind_).title, (unsigned long)(millis() - t0));
}

uint64_t LocalStorage::totalBytes() const {
  if (!available()) return 0;
  if (onCard()) {
    // The card's size: the sector count its CSD gave at the mount, kept
    // by the SD driver (sdcard_num_sectors()): no card I/O, no lock. Not
    // SD.totalBytes() (nor usedBytes()): both are f_getfree(), which on
    // FAT32 takes the free count the mount read from the FSINFO sector;
    // a card whose count is unset ("unknown", 0xFFFFFFFF) or whose FSINFO
    // is missing makes it count the free clusters, every sector of the
    // FAT: ~244k reads on a 1 TB card, minutes, holding the FatFs
    // volume's lock all along, so the decode task's reads fail
    // (FR_TIMEOUT, CONFIG_FATFS_TIMEOUT_MS: 10 s) and the music stops.
    // About asked for it at its first open after each boot (the count is
    // kept for the mount after that). The card's size is the volume's
    // plus what comes before it and its FATs (~0.03% more on a big card).
    // Nothing on the device needs the free space today. The WiFi sync
    // will ("does it fit?"): it must count it once, in one controlled
    // scan with progress, outside playback and never at boot; not
    // through About, or a usedBytes() here.
    return SD.cardSize();
  }
  // The flash's: its partition's size, which is the LittleFS's (its
  // blocks fill the partition). Not LittleFS.totalBytes(): that is
  // esp_littlefs_info(), which also counts the used blocks (lfs_fs_size(),
  // a walk of the whole file system under its lock, while the decode task
  // reads from it) for a number it throws away.
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, kFlashLabel);
  return p ? p->size : 0;
}
