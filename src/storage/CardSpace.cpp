// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "storage/CardSpace.h"

#include <Arduino.h>

#include "FreeCount.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"
#include "ff.h"
#include "diskio.h"  // FatFs: disk_read() (after ff.h's ffconf names it)

namespace cardspace {

namespace {

FATFS* s_fs = nullptr;
FreeCount s_count(psramAlloc, psramFree);
uint8_t* s_buf = nullptr;  // a piece (PSRAM), while a count runs
WORD s_id = 0;             // the volume's mount ID when the count began
BYTE s_type = 0;
uint32_t s_startMs = 0;
Counted s_counted;
const char* s_failure = "";

// The sector size FatFs reads the volume in (FF_MAX_SS 4096 here, so it
// keeps one per volume; an SD card's is 512).
uint32_t sectorBytes(const FATFS* fs) {
#if FF_MAX_SS != FF_MIN_SS
  return fs->ssize;
#else
  return FF_MAX_SS;
#endif
}

// The volume as the count reads it, the lock held.
class Volume final : public FreeCount::Source {
public:
  bool read(uint32_t lba, uint8_t* out, uint32_t sectors) override {
    return disk_read(s_fs->pdrv, out, lba, sectors) == RES_OK;
  }
  bool window(uint32_t* lba, const uint8_t** bytes) override {
    if (!(s_fs->wflag & 1)) return false;
    *lba = static_cast<uint32_t>(s_fs->winsect);
    *bytes = s_fs->win;
    return true;
  }
};

// The write watch (storage/SectorDisk): every write FatFs makes to the
// card, under its volume lock (as the count's steps take it).
void onWrite(uint32_t lba, uint32_t count, void*) { s_count.written(lba, count); }

bool s_watching = false;  // the watch is on (it reads the count's table)
bool s_running = false;   // a count is under way (counting())

// The watch off, under the volume's lock: no write is under way then, so
// none is inside onWrite() after it. False: the lock wasn't had (FatFs's
// 10 s), the watch stays on.
bool unwatch() {
  if (!s_watching) return true;
  if (!ff_mutex_take(s_fs->ldrv)) return false;
  sectordisk::watchWrites(nullptr, nullptr);
  s_watching = false;
  ff_mutex_give(s_fs->ldrv);
  return true;
}

// The count's table and buffer freed: only with the watch off (a later
// start or stop tries again when the lock wasn't had).
void release() {
  if (!unwatch()) return;
  s_count.end();
  if (s_buf) psramFree(s_buf);
  s_buf = nullptr;
}

}  // namespace

void begin(uint8_t pdrv) {
  // A directory of the root names the volume FatFs mounted on this drive
  // (its FATFS, which the SD library keeps; no other way to it short of
  // f_getfree()). The volume is mounted: no sector read, one status command.
  char path[4] = {static_cast<char>('0' + pdrv), ':', '/', '\0'};
  FF_DIR dir;
  if (f_opendir(&dir, path) == FR_OK) {
    s_fs = dir.obj.fs;
    f_closedir(&dir);
  }
  if (s_fs) {
    Serial.printf("[storage] the card's volume: %s, %lu clusters of %lu B; its free count: %s\n",
                  hoststatus::cardName(card()), static_cast<unsigned long>(s_fs->n_fatent - 2),
                  static_cast<unsigned long>(clusterBytes()),
                  hoststatus::freeCountValid(s_fs->free_clst, s_fs->n_fatent) ? "known (FSINFO)" : "unknown (@count)");
  }
}

bool ready() { return s_fs != nullptr; }

hoststatus::Card card() {
  if (!s_fs) return hoststatus::Card::None;
  switch (s_fs->fs_type) {
    case FS_FAT32: return hoststatus::Card::Fat32;
    case FS_FAT16:
    case FS_FAT12: return hoststatus::Card::Fat16;
    default: return hoststatus::Card::None;  // (0: FatFs lost the volume)
  }
}

uint32_t clusterBytes() { return s_fs && s_fs->fs_type ? static_cast<uint32_t>(s_fs->csize) * sectorBytes(s_fs) : 0; }

bool freeBytes(uint64_t* bytes) {
  if (!s_fs || !s_fs->fs_type) return false;
  // (Words, each read as it is: FatFs changes the count under its lock.)
  const uint32_t n = s_fs->free_clst, entries = s_fs->n_fatent;
  if (!hoststatus::freeCountValid(n, entries)) return false;
  *bytes = static_cast<uint64_t>(n) * clusterBytes();
  return true;
}

bool startCount(uint32_t nowMs) {
  if (!s_fs || !s_fs->fs_type) {
    s_failure = "no volume";
    return false;
  }
  if (!sectordisk::installed()) {
    s_failure = "no write watch (no sector cache's wrapper in this build)";
    return false;
  }
  s_running = false;
  release();
  if (s_watching) {
    s_failure = "the volume's lock (10 s)";
    return false;
  }
  s_buf = static_cast<uint8_t*>(psramAlloc(FreeCount::kPieceBytes));
  FreeCount::Volume v;
  v.fat = s_fs->fs_type == FS_FAT32   ? FreeCount::Fat::Fat32
          : s_fs->fs_type == FS_FAT16 ? FreeCount::Fat::Fat16
                                      : FreeCount::Fat::Fat12;
  v.fatStart = static_cast<uint32_t>(s_fs->fatbase);
  v.fatSectors = s_fs->fsize;
  v.entries = s_fs->n_fatent;
  v.sectorBytes = sectorBytes(s_fs);
  if (!s_buf || !s_count.begin(v)) {
    s_failure = s_buf ? "a volume it can't count" : "no memory";
    release();
    return false;
  }
  // The watch goes on under the volume's lock: no write is under way.
  if (!ff_mutex_take(s_fs->ldrv)) {
    s_failure = "the volume's lock (10 s)";
    release();
    return false;
  }
  s_id = s_fs->id;
  s_type = s_fs->fs_type;
  sectordisk::watchWrites(onWrite, nullptr);
  s_watching = true;
  ff_mutex_give(s_fs->ldrv);
  s_startMs = nowMs;
  s_running = true;
  return true;
}

Step stepCount() {
  if (!s_running) return Step::Failed;
  // FatFs's own lock (lock_volume()), and its wait. Not had: the watch
  // and the table stay until a later start or stop can take it.
  if (!ff_mutex_take(s_fs->ldrv)) {
    s_failure = "the volume's lock (10 s)";
    s_running = false;
    return Step::Failed;
  }
  Step r = Step::Working;
  if (s_fs->fs_type != s_type || s_fs->id != s_id) {
    s_failure = "the card was mounted again";
    r = Step::Failed;
  } else {
    Volume vol;
    const FreeCount::Step s = s_count.step(vol, s_buf);
    if (s == FreeCount::Step::Failed) {
      s_failure = "a read error";
      r = Step::Failed;
    } else if (s == FreeCount::Step::Done) {
      // Exact as of this hold: FatFs's count from now on (it follows every
      // cluster it takes or frees from here), and FSINFO to be written at
      // its next sync, as f_getfree() does after its own count.
      s_fs->free_clst = s_count.freeClusters();
      s_fs->fsi_flag |= 1;
      s_counted.freeClusters = s_count.freeClusters();
      s_counted.clusterBytes = clusterBytes();
      s_counted.freeBytes = static_cast<uint64_t>(s_counted.freeClusters) * s_counted.clusterBytes;
      s_counted.fatBytes = s_count.fatBytes();
      s_counted.pieces = s_count.pieces();
      s_counted.reads = s_count.reads();
      s_counted.rereads = s_count.rereads();
      // (Bit 7: no FSINFO sector to write; FAT16 has none.)
      s_counted.fsinfo = s_fs->fs_type == FS_FAT32 && !(s_fs->fsi_flag & 0x80);
      r = Step::Done;
    }
  }
  if (r != Step::Working) {
    sectordisk::watchWrites(nullptr, nullptr);  // (the lock held: no write under way)
    s_watching = false;
  }
  ff_mutex_give(s_fs->ldrv);
  if (r != Step::Working) {
    if (r == Step::Done) s_counted.ms = millis() - s_startMs;
    s_running = false;
    release();
  }
  return r;
}

void stopCount() {
  s_running = false;
  release();
}

bool counting() { return s_running; }

uint8_t countPercent() { return s_count.percent(); }

const Counted& counted() { return s_counted; }

const char* failure() { return s_failure; }

}  // namespace cardspace
