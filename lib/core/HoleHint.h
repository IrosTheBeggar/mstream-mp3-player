// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "HostStatus.h"  // hoststatus::freeCountValid()

// Where FatFs looks for its next free cluster on a nearly full card
// (docs/METADATA.md 3.2.4, "The hole hint"). Header only: the firmware's
// diskio wrapper (storage/SectorDisk's wIoctl()) and the host FatFs model
// (test/support/FatModel.h's disk_ioctl, with Drive::holeHint) run the same
// onTrim(), each over its own ff.h's FATFS. Host-tested: test_hole_hint.
//
// FatFs (R0.15, as ESP-IDF 5.5 builds it) looks for a free cluster from the
// one after the last it allocated (FATFS::last_clst, create_chain()), one FAT
// entry at a time, wrapping at the end. Freeing a chain (remove_chain(), from
// f_unlink() and f_truncate()) leaves last_clst where it was. So on a card
// with one free cluster, a write that needs the cluster the last remove freed
// reads the whole FAT first: on the Core2's 32 GB card 7,607 single-sector
// reads, about 5.3 s, all of it under the volume's lock that the decoder's
// reads wait on (a queue save of 5,589 ms on the device, 2026-11-18, after a
// write that took and freed the only cluster; the queue's own saves with one
// free cluster read the whole FAT every two saves on the host model). FatFs
// points its search at a chain it frees in one place only, f_open()'s
// FA_CREATE_ALWAYS over a file (`last_clst = cl - 1`, "Reuse the cluster
// hole").
//
// onTrim() applies that rule to every chain FatFs frees. remove_chain()
// trims each contiguous run of clusters it frees (CTRL_TRIM, FF_USE_TRIM 1),
// in the chain's order, after it zeroed their FAT entries and counted them in
// free_clst, inside the FatFs call that holds the volume's lock. At each such
// TRIM:
//   - only on a nearly full volume (lastClst()'s gate: at most one free
//     cluster for every 64 FAT sectors, 118 clusters on the 32 GB card).
//     With more room FatFs's own order stays: a search from anywhere is short;
//   - the lowest run freed since FatFs last allocated, while every run freed
//     after it lies at most kNearRun clusters above it: a search from it finds
//     those runs within 64 FAT sectors (a fragmented file freed, or several
//     removes in a row). A run farther above starts the search instead.
// last_clst is only where a search starts: every FatFs reader clamps it to
// the volume, and a cluster is taken only when its FAT entry reads free. A
// wrong hint costs time, never a cluster. It writes no FAT sector, no count
// and no flag. f_unlink()'s sync puts it in FSINFO's next-free field when it
// writes the count it changed; a later allocation's sync replaces it.
namespace holehint {

// The gate: a hint only while the free clusters are at most
// (FAT entries - 2) / kEntriesPerFree. A search from anywhere passes about
// entries/free FAT entries before it finds one, and a FAT32 sector holds 128
// entries: 64 sectors, about 45 ms of card reads on the Core2's card. A
// judgement, not a measurement: any gate from about 16 clusters to a few
// hundred fixes the one-free-cluster case.
constexpr uint32_t kEntriesPerFree = 64u * 128u;

// How far above the lowest run a run freed after it may lie and leave the
// start on the lowest: 64 FAT sectors again (8,192 clusters). Farther, that
// run is the start, as it is when it comes alone. A chain that took a lone
// hole low on the card and then walked to the free run far above (the queue's
// text grown past the cluster its old chain started in, on a card whose few
// free clusters are a run) ends in that run; a start left on the hole sent
// every second save back through that walk, a whole FAT (test_hole_hint M9,
// M10). The cost: a file in two pieces far apart, low piece first, starts at
// the high one, and the next write but one wraps to the low (M8). A
// judgement: on the host model 1,024 gave the same counts as 8,192.
constexpr uint32_t kNearRun = 64u * 128u;

// FATFS's fields the rule reads.
struct Volume {
  uint32_t database = 0;  // the data area's first sector (cluster 2's)
  uint32_t csize = 0;     // sectors a cluster
  uint32_t nFatent = 0;   // FAT entries: the clusters + 2
  uint32_t freeClst = 0;  // FatFs's free count (FSINFO's; 0xFFFFFFFF: unknown)
};

// The cluster whose first sector is `firstSector` (a TRIM's first LBA); 0
// when that isn't a cluster's first sector in the volume.
constexpr uint32_t clusterAt(const Volume& v, uint32_t firstSector) {
  if (v.csize == 0 || v.nFatent < 3 || firstSector < v.database) return 0;
  const uint32_t off = firstSector - v.database;
  if (off % v.csize != 0) return 0;
  const uint32_t cl = off / v.csize + 2;
  return cl < v.nFatent ? cl : 0;
}

// FatFs freed a run of clusters from `firstSector` on: the value for
// FATFS::last_clst that makes its next search test that run's first cluster
// first; 0: leave last_clst as it is (not a cluster's first sector, the free
// count unknown, or more room than the gate). FatFs's own `cl - 1`, except
// for cluster 2: `n_fatent - 1`, so the search wraps to 2 first and FSINFO's
// next-free still names a cluster (1 doesn't).
constexpr uint32_t lastClst(const Volume& v, uint32_t firstSector) {
  const uint32_t cl = clusterAt(v, firstSector);
  if (cl == 0) return 0;
  if (!hoststatus::freeCountValid(v.freeClst, v.nFatent)) return 0;
  if (v.freeClst > (v.nFatent - 2) / kEntriesPerFree) return 0;
  return cl == 2 ? v.nFatent - 1 : cl - 1;
}

// What onTrim() last set: last_clst's value and its run's first cluster.
// Cleared at each mount (storage/SectorDisk's install()).
struct Memory {
  uint32_t hint = 0;
  uint32_t cluster = 0;  // 0: none
};

// FatFs's CTRL_TRIM on drive `pdrv` for a run from `firstSector`, with `fs`
// the volume mounted on it (nullptr: none found). Called after the driver's
// own work for the TRIM, under the volume's lock (FatFs's call); it takes no
// lock and calls no FatFs function. f_mkfs()'s trims come with fs_type 0
// (it unmounts the volume first): skipped. True when it moved last_clst.
template <class Fatfs>
inline bool onTrim(Fatfs* fs, uint8_t pdrv, uint32_t firstSector, Memory& mem) {
  if (!fs || fs->fs_type == 0 || fs->pdrv != pdrv) return false;
  Volume v;
  v.database = static_cast<uint32_t>(fs->database);
  v.csize = static_cast<uint32_t>(fs->csize);
  v.nFatent = static_cast<uint32_t>(fs->n_fatent);
  v.freeClst = static_cast<uint32_t>(fs->free_clst);
  const uint32_t at = lastClst(v, firstSector);
  if (at == 0) return false;
  const uint32_t cl = clusterAt(v, firstSector);
  // Nothing allocated since the last hint (an allocation moves last_clst),
  // and that hint's run is lower and near: it stays.
  if (mem.cluster != 0 && static_cast<uint32_t>(fs->last_clst) == mem.hint && cl > mem.cluster &&
      cl - mem.cluster <= kNearRun) {
    return false;
  }
  fs->last_clst = at;
  mem.hint = at;
  mem.cluster = cl;
  return true;
}

}  // namespace holehint
