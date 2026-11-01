// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "HostStatus.h"

// The card's free space (docs/HOST-STATUS.md; the computer's @status and
// @count, app/HostQuery): what FatFs holds about the mounted volume, read
// with no card access and no lock, and the count of its free clusters on
// request, a piece of the FAT per loop pass (lib/core FreeCount).
//
//   - The volume: its FATFS, found once right after the mount
//     (LocalStorage::begin(): a FatFs directory of the root, whose object
//     names it; FatFs mounted the volume already, so that reads no sector)
//     and the same object for the whole boot (FatFs mounts the volume
//     again in it after a card error).
//   - The free count FatFs keeps (FATFS::free_clst): the mount takes it
//     from FAT32's FSINFO sector, and from then on FatFs follows every
//     cluster it takes or frees, as long as it is a count (the FSINFO rule,
//     hoststatus::freeCountValid()). Read as a word: never f_getfree(),
//     which counts the whole FAT when the count is unknown (minutes on a big
//     card, the volume's lock held throughout: LocalStorage::totalBytes()).
//   - The count (@count): FreeCount's steps, each under FatFs's own volume
//     lock (ff_mutex_take() of the volume's drive, the lock every FatFs
//     call takes: lock_volume() in ff.c; not recursive, so nothing here
//     calls FatFs while it holds it, only the disk functions), one piece of
//     32 KB at a time (~13 ms of reading), let go between passes. The disk's
//     write watch (storage/SectorDisk, called under that lock by every
//     FatFs write) tells it which pieces a write reached. Done, under the
//     same hold: FatFs's free count set to it and FSINFO marked to be
//     written (FATFS::fsi_flag's bit 0), exactly what f_getfree() does after
//     its own count; FatFs writes FSINFO at its next sync (a file it wrote
//     closed or synced, a delete, a rename: the queue's save, at the
//     latest), FAT32 with an FSINFO sector only, so the next mount reads
//     the count at once. A card FatFs mounted again meanwhile (its mount ID
//     changed) fails the count.
//
// Loop task only; the reads of what FatFs holds are single words, safe from
// any task (a status line's numbers, while a mount is under way, may be the
// old volume's).
namespace cardspace {

// After a successful mount of the SD card on FatFs drive `pdrv`.
void begin(uint8_t pdrv);
bool ready();

// What FatFs mounted: Fat32 or Fat16 (FAT12 too), None once FatFs lost the
// volume (a card that stopped answering and didn't mount again).
hoststatus::Card card();
// FatFs's free count in bytes, when it is one (the FSINFO rule). False:
// unknown.
bool freeBytes(uint64_t* bytes);
// The volume's cluster size in bytes (0: no volume).
uint32_t clusterBytes();

// ---- the count (@count) ----
enum class Step : uint8_t { Working, Done, Failed };
// False: no volume, no write watch (a build without SectorDisk's wrapper:
// the count couldn't see FatFs's writes), or no memory (its table and a
// 32 KB buffer, PSRAM).
bool startCount(uint32_t nowMs);
// One step: a piece, or the commit (Done: FatFs's count set, FSINFO
// marked). Failed: a read error, the volume mounted again, or its lock
// not had in FatFs's own wait (10 s). Either end frees the count.
Step stepCount();
// Abandoned (playing started, the library update): nothing set.
void stopCount();
bool counting();
uint8_t countPercent();
// The last count's result and figures (after Done), for its lines.
struct Counted {
  uint64_t freeBytes = 0;
  uint32_t freeClusters = 0;
  uint32_t clusterBytes = 0;
  uint32_t fatBytes = 0;
  uint32_t pieces = 0, reads = 0, rereads = 0;
  uint32_t ms = 0;
  bool fsinfo = false;  // FAT32 with an FSINFO sector: marked to be written
};
const Counted& counted();
// Why the last count failed, for the log ("a read error", ...).
const char* failure();

}  // namespace cardspace
