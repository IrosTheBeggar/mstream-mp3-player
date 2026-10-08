// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// The host-only FatFs model (docs/METADATA.md 3.2.7, row N8 of 6.1): ChaN's
// FatFs R0.15, configured as the Core2's firmware builds it
// (test/support/fatfs/ffconf.h), on sparse RAM disks, with SectorCache in
// front of a drive or not. It counts what FatFs asks of its disk (what the
// stock SD driver reads: one card transaction per call) and what reaches
// the card through the cache, per lookup and per walk, on trees in the
// user's shape (test_fat_model; tools/fatmodel.py on tools/synthcard.py's
// card).
//
// FatFs calls the disk functions defined at the end of this header: include
// it in one translation unit per program, with FatFs itself compiled from
// test/support/fatfs/FatFsBuild.c. Host only: std containers.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "SectorCache.h"
#include "fatfs/FatFsHost.h"

namespace fatmodel {

constexpr uint32_t kSS = 512;

// A RAM disk that stores only the sectors holding a non-zero byte (a card
// image of 20,000 files is a few MB). A TRIM fills its range with a pattern
// of each LBA when `scrambleTrim` is set (a trimmed sector's bytes are the
// card's to choose), so a cache that kept a trimmed sector is caught.
class RamDisk : public SectorCache::Device {
public:
  explicit RamDisk(uint32_t sectors = 0) : count_(sectors) {}
  uint32_t sectors() const { return count_; }

  bool read(uint32_t lba, uint8_t* out, uint32_t n) override {
    ++counts.reads;
    counts.readSectors += n;
    if (static_cast<uint64_t>(lba) + n > count_) return false;
    for (uint32_t k = 0; k < n; ++k) {
      auto it = store_.find(lba + k);
      if (it == store_.end()) std::memset(out + static_cast<size_t>(k) * kSS, 0, kSS);
      else std::memcpy(out + static_cast<size_t>(k) * kSS, it->second.data(), kSS);
    }
    return true;
  }
  bool write(uint32_t lba, const uint8_t* data, uint32_t n) override {
    ++counts.writes;
    counts.writeSectors += n;
    if (static_cast<uint64_t>(lba) + n > count_) return false;
    for (uint32_t k = 0; k < n; ++k) put(lba + k, data + static_cast<size_t>(k) * kSS);
    return true;
  }
  void trim(uint32_t first, uint32_t last) {
    ++counts.trims;
    if (!scrambleTrim) return;
    uint8_t s[kSS];
    for (uint64_t lba = first; lba <= last && lba < count_; ++lba) {
      for (uint32_t i = 0; i < kSS; ++i) s[i] = static_cast<uint8_t>(lba * 31u + i * 7u + 1u);
      put(static_cast<uint32_t>(lba), s);
    }
  }
  bool same(const RamDisk& o) const { return count_ == o.count_ && store_ == o.store_; }
  size_t stored() const { return store_.size(); }

  struct Counts {
    uint64_t reads = 0, readSectors = 0, writes = 0, writeSectors = 0, trims = 0;
  } counts;
  bool scrambleTrim = false;

private:
  void put(uint32_t lba, const uint8_t* s) {
    bool zero = true;
    for (uint32_t i = 0; i < kSS && zero; ++i) zero = s[i] == 0;
    if (zero) {
      store_.erase(lba);
      return;
    }
    std::array<uint8_t, kSS>& d = store_[lba];
    std::memcpy(d.data(), s, kSS);
  }
  uint32_t count_;
  std::unordered_map<uint32_t, std::array<uint8_t, kSS>> store_;
};

// One FatFs drive: its disk, the cache in front of it (null: the stock
// driver, every call a card transaction), and what FatFs asked of it.
struct Drive {
  RamDisk* disk = nullptr;
  SectorCache* cache = nullptr;
  struct Asked {
    uint64_t reads = 0, singleReads = 0, readSectors = 0, writes = 0, trims = 0;
  } asked;
};

inline Drive& drive(BYTE pdrv) {
  static Drive drives[FF_VOLUMES];
  return drives[pdrv];
}

// What one stretch of work read: FatFs's disk_read calls (the stock
// driver's card reads) and the reads that reached the card.
struct Reads {
  uint64_t asked = 0;  // disk_read calls
  uint64_t card = 0;   // device reads (through the cache, or all of `asked`)
  uint64_t hits = 0;
  Reads& operator+=(const Reads& o) {
    asked += o.asked;
    card += o.card;
    hits += o.hits;
    return *this;
  }
};

class Meter {
public:
  explicit Meter(BYTE pdrv) : d_(drive(pdrv)) { reset(); }
  void reset() {
    asked0_ = d_.asked.reads;
    card0_ = d_.disk->counts.reads;
    hits0_ = d_.cache ? d_.cache->stats().hits : 0;
  }
  Reads read() const {
    Reads r;
    r.asked = d_.asked.reads - asked0_;
    r.card = d_.disk->counts.reads - card0_;
    r.hits = (d_.cache ? d_.cache->stats().hits : 0) - hits0_;
    return r;
  }

private:
  Drive& d_;
  uint64_t asked0_ = 0, card0_ = 0, hits0_ = 0;
};

// "N:" + "/" + rel (rel: "music/Artist").
inline std::string path(BYTE pdrv, const std::string& rel) {
  std::string p(1, static_cast<char>('0' + pdrv));
  p += ":/";
  p += rel;
  return p;
}

// FAT32 with 2 FATs and `clusterBytes` clusters on drive `pdrv`'s whole
// disk, as a card formatted for the player (README: FAT32).
inline FRESULT format(BYTE pdrv, uint32_t clusterBytes) {
  MKFS_PARM opt = {FM_FAT32, 2, 0, 0, clusterBytes};
  std::vector<uint8_t> work(64 * 1024);
  return f_mkfs(path(pdrv, "").c_str(), &opt, work.data(), static_cast<UINT>(work.size()));
}

// Mounts drive `pdrv` now (the firmware's mount: the cache cleared first).
inline FRESULT mount(BYTE pdrv, FATFS* fs) {
  if (drive(pdrv).cache) drive(pdrv).cache->clear();
  return f_mount(fs, path(pdrv, "").c_str(), 1);
}

// Sibling names in the canonical order (2.6.8: the names' bytes, shorter
// first on a common prefix).
inline bool nameBefore(const std::string& a, const std::string& b) {
  const int c = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
  return c != 0 ? c < 0 : a.size() < b.size();
}

// The card worker's walk (3.2.3, N5's CardWalk with N10's lister): one
// folder at a time, opened by its full path (FF_FS_RPATH 0: a lookup from
// the root), listed whole, closed; then its subfolders by name, in
// pre-order. Hidden names (".x") are listed, not entered. `onFile` sees
// each file's path relative to the card's root.
struct WalkStats {
  uint64_t folders = 0, files = 0, entries = 0;
  Reads reads;
};
template <class OnFile>
inline FRESULT walkFolders(BYTE pdrv, const std::string& rootRel, WalkStats* st, OnFile onFile) {
  Meter m(pdrv);
  std::vector<std::string> todo{rootRel};
  DIR dir;
  FILINFO fi;
  FRESULT res = FR_OK;
  while (!todo.empty() && res == FR_OK) {
    const std::string rel = todo.back();
    todo.pop_back();
    if ((res = f_opendir(&dir, path(pdrv, rel).c_str())) != FR_OK) break;
    std::vector<std::string> subs, files;
    for (;;) {
      if ((res = f_readdir(&dir, &fi)) != FR_OK || fi.fname[0] == 0) break;
      ++st->entries;
      if (fi.fattrib & AM_DIR) {
        if (fi.fname[0] != '.') subs.push_back(fi.fname);
      } else {
        files.push_back(fi.fname);
      }
    }
    f_closedir(&dir);
    if (res != FR_OK) break;
    ++st->folders;
    std::sort(files.begin(), files.end(), nameBefore);
    for (const std::string& f : files) {
      ++st->files;
      onFile(rel + "/" + f);
    }
    std::sort(subs.begin(), subs.end(), nameBefore);
    for (auto it = subs.rbegin(); it != subs.rend(); ++it) todo.push_back(rel + "/" + *it);
  }
  st->reads += m.read();
  return res;
}

// Today's walk (src/storage/LocalStorage.cpp, forEachFile: the VFS's
// opendir and readdir): recursive, in the directory's order, each level's
// DIR kept open while its subfolders are walked, hidden names skipped.
inline FRESULT walkNested(BYTE pdrv, const std::string& rel, WalkStats* st, int depth = 0, int maxDepth = 8) {
  Meter m(pdrv);
  DIR dir;
  FILINFO fi;
  FRESULT res = f_opendir(&dir, path(pdrv, rel).c_str());
  if (res != FR_OK) return res;
  ++st->folders;
  for (;;) {
    if ((res = f_readdir(&dir, &fi)) != FR_OK || fi.fname[0] == 0) break;
    ++st->entries;
    if (fi.fname[0] == '.') continue;
    if (fi.fattrib & AM_DIR) {
      if (depth < maxDepth) {
        WalkStats sub;
        res = walkNested(pdrv, rel + "/" + fi.fname, &sub, depth + 1, maxDepth);
        st->folders += sub.folders;
        st->files += sub.files;
        st->entries += sub.entries;
        if (res != FR_OK) break;
      }
    } else {
      ++st->files;
    }
  }
  f_closedir(&dir);
  if (depth == 0) st->reads += m.read();
  return res;
}

// The sectors a folder's entries take on the card: its 32-byte entries up
// to its last name's, "." and ".." (and each name's long-name entries)
// included, as tools/synthcard.py counts them. After an item, FatFs's
// dptr is past the item's last entry.
inline uint32_t folderSectors(BYTE pdrv, const std::string& rel, uint32_t* entries = nullptr) {
  DIR dir;
  if (f_opendir(&dir, path(pdrv, rel).c_str()) != FR_OK) return 0;
  FILINFO fi;
  uint32_t n = rel.empty() ? 0 : 2;
  while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0] != 0) n = dir.dptr / 32;
  f_closedir(&dir);
  if (entries) *entries = n;
  return (n * 32 + kSS - 1) / kSS;
}

}  // namespace fatmodel

// ---- the disk functions FatFs calls ----

extern "C" DSTATUS disk_status(BYTE pdrv) {
  return pdrv < FF_VOLUMES && fatmodel::drive(pdrv).disk ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize(BYTE pdrv) { return disk_status(pdrv); }

extern "C" DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
  if (disk_status(pdrv)) return RES_NOTRDY;
  fatmodel::Drive& d = fatmodel::drive(pdrv);
  ++d.asked.reads;
  d.asked.singleReads += count == 1;
  d.asked.readSectors += count;
  const uint32_t lba = static_cast<uint32_t>(sector);
  const bool ok = d.cache ? d.cache->read(*d.disk, lba, buff, count) : d.disk->read(lba, buff, count);
  return ok ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
  if (disk_status(pdrv)) return RES_NOTRDY;
  fatmodel::Drive& d = fatmodel::drive(pdrv);
  ++d.asked.writes;
  const uint32_t lba = static_cast<uint32_t>(sector);
  const bool ok = d.cache ? d.cache->write(*d.disk, lba, buff, count) : d.disk->write(lba, buff, count);
  return ok ? RES_OK : RES_ERROR;
}

extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
  if (disk_status(pdrv)) return RES_NOTRDY;
  fatmodel::Drive& d = fatmodel::drive(pdrv);
  switch (cmd) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *static_cast<LBA_t*>(buff) = d.disk->sectors(); return RES_OK;
    case GET_SECTOR_SIZE: *static_cast<WORD*>(buff) = fatmodel::kSS; return RES_OK;
    case GET_BLOCK_SIZE: *static_cast<DWORD*>(buff) = 1; return RES_OK;  // as the SD driver says
    case CTRL_TRIM: {
      // The wrapper's rule (N10): a TRIM drops the range from the cache.
      const LBA_t* r = static_cast<const LBA_t*>(buff);
      ++d.asked.trims;
      if (d.cache) d.cache->invalidate(static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1] - r[0] + 1));
      d.disk->trim(static_cast<uint32_t>(r[0]), static_cast<uint32_t>(r[1]));
      return RES_OK;
    }
  }
  return RES_PARERR;
}

extern "C" DWORD get_fattime(void) {
  // 2026-10-07 12:00:00: a fixed clock, so two formats are the same bytes.
  return (static_cast<DWORD>(2026 - 1980) << 25) | (10u << 21) | (7u << 16) | (12u << 11);
}
