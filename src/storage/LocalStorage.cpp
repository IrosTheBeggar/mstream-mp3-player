// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/LocalStorage.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>
#include <dirent.h>
#include <sd_diskio.h>
#include <sys/stat.h>

#include <cstring>

#include "diskio.h"  // FatFs: disk_initialize(), disk_read() (after ff.h's ffconf names them)
#include "ff.h"

namespace {
constexpr const char* kMusicDir = "/music";
constexpr const char* kStateDir = "/.player";
constexpr const char* kSdMount = "/sd";
constexpr const char* kFlashMount = "/littlefs";
// The VFS path: the mount point, then what the callback sees ("/music/...").
constexpr size_t kPathMax = 300;
constexpr uint32_t kSdHz = 25000000;

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
  if (SD.begin(cs, SPI, kSdHz, kSdMount)) {
    fs_ = &SD;
    name_ = "SD";
    mount_ = kSdMount;
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
  if (!SD.begin(cs, SPI, kSdHz, kSdMount)) {
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
  Serial.printf("[storage] no card mounted; its first sectors: %s%s (%lu ms)\n", cardformat::name(cardKind_),
                cardformat::notFat32(cardKind_) ? ": not FAT32 (MBR), the pages say so" : "",
                (unsigned long)(millis() - t0));
}

uint64_t LocalStorage::totalBytes() const {
  if (!available()) return 0;
  return onCard() ? SD.totalBytes() : LittleFS.totalBytes();
}
