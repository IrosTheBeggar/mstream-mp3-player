// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the hole hint (lib/core/HoleHint.h; docs/METADATA.md 3.2.4,
// "The hole hint"): on a nearly full card FatFs's next search for a free
// cluster starts at the clusters it just freed, as f_open()'s FA_CREATE_ALWAYS
// already does for the chain it frees.
// - U1-U5: the rule (lastClst(), clusterAt()) and the TRIM hook's glue
//   (onTrim()) over a FATFS of its own: the cluster, the gate, the unknown
//   count, mkfs's trims, another drive, the lowest run kept while the later
//   ones are near it.
// - M0-M10: under ChaN's FatFs R0.15 (test/support/FatModel.h, as the Core2
//   builds it) on the device's card, 62,333,952 sectors in FAT32 with 32 KB
//   clusters, filled to a few free clusters. Each scene is run with the hook
//   off (FatFs as it is, the firmware before the hint) and on, and counts the
//   FAT sectors a write reads (its disk_read calls: a write of whole sectors
//   reads nothing else): a search through the whole FAT reads about 7,608,
//   about 5.3 s on the device (2026-11-18: a queue save of 5,589 ms and 148
//   underruns, on the listening-stats firmware after its failed stats.tmp
//   took and freed the only cluster: M1's case). M1-M8: what the hint saves
//   (the "with" counts fail without it); M8 far apart also what the near
//   limit gives up. M9: what it costs, pinned: free clusters in a run far
//   from the one just freed, and a write of more than one cluster: one walk.
//   M10: a queue growing over 124 saves walks no more often than without the
//   hint (pinned: the rule before the near limit walked every second save).
// Run: pio test -e native -f test_hole_hint
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "../support/FatModelFs.h"
#include "HoleHint.h"
#include "TagStore.h"

namespace {

// ---- the rule ----

// The device card's FAT (973,584 clusters of 32 KB, 2026-11-18).
constexpr uint32_t kCardFatent = 973586;

holehint::Volume cardVolume(uint32_t freeClst) {
  holehint::Volume v;
  v.database = 32768;
  v.csize = 64;
  v.nFatent = kCardFatent;
  v.freeClst = freeClst;
  return v;
}

uint32_t firstSectorOf(const holehint::Volume& v, uint32_t cl) { return v.database + (cl - 2) * v.csize; }

// U1: a cluster's first sector gives FatFs's own cl - 1; cluster 2 gives
// n_fatent - 1 (the search wraps to 2 first).
void test_u1_cluster_first_sector() {
  const holehint::Volume v = cardVolume(1);
  for (uint32_t cl : {3u, 4u, 64u, 486793u, kCardFatent - 2, kCardFatent - 1}) {
    TEST_ASSERT_EQUAL_UINT32(cl, holehint::clusterAt(v, firstSectorOf(v, cl)));
    TEST_ASSERT_EQUAL_UINT32(cl - 1, holehint::lastClst(v, firstSectorOf(v, cl)));
  }
  TEST_ASSERT_EQUAL_UINT32(2, holehint::clusterAt(v, v.database));
  TEST_ASSERT_EQUAL_UINT32(kCardFatent - 1, holehint::lastClst(v, v.database));
  // One sector a cluster (FatFs's smallest).
  holehint::Volume one = v;
  one.csize = 1;
  TEST_ASSERT_EQUAL_UINT32(9, holehint::lastClst(one, one.database + 8));
}

// U2: not a cluster's first sector, below the data area, or at or past the
// last cluster: nothing.
void test_u2_not_a_cluster() {
  const holehint::Volume v = cardVolume(1);
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, v.database + 1));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, firstSectorOf(v, 1000) + 63));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, v.database - 1));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, 0));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, firstSectorOf(v, kCardFatent)));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, firstSectorOf(v, kCardFatent + 5)));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(v, 0xFFFFFFFFu));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::clusterAt(v, v.database + 1));
}

// U3: the free count unknown (FSINFO's 0xFFFFFFFF, or more than the
// clusters: hoststatus::freeCountValid()): nothing.
void test_u3_count_unknown() {
  const uint32_t s = firstSectorOf(cardVolume(1), 1000);
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(cardVolume(0xFFFFFFFFu), s));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(cardVolume(kCardFatent - 1), s));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(cardVolume(kCardFatent), s));
  // (A count of 0 at a TRIM would be FatFs counting after it: still only a start.)
  TEST_ASSERT_EQUAL_UINT32(999, holehint::lastClst(cardVolume(0), s));
}

// U4: the gate on the device card's FAT: 118 free clusters move the hint,
// 119 don't. A FAT16 volume's gate is a few clusters, a FAT12 one's none.
// No cluster size, or no clusters: nothing.
void test_u4_gate() {
  const uint32_t s = firstSectorOf(cardVolume(1), 1000);
  TEST_ASSERT_EQUAL_UINT32(118, (kCardFatent - 2) / holehint::kEntriesPerFree);
  TEST_ASSERT_EQUAL_UINT32(999, holehint::lastClst(cardVolume(1), s));
  TEST_ASSERT_EQUAL_UINT32(999, holehint::lastClst(cardVolume(118), s));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(cardVolume(119), s));
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(cardVolume(400000), s));
  holehint::Volume fat16 = cardVolume(7);
  fat16.nFatent = 60000;  // (59,998 / 8,192 = 7)
  TEST_ASSERT_EQUAL_UINT32(999, holehint::lastClst(fat16, s));
  fat16.freeClst = 8;
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(fat16, s));
  holehint::Volume fat12 = cardVolume(1);
  fat12.nFatent = 4000;
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(fat12, s));
  holehint::Volume none = cardVolume(1);
  none.csize = 0;
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(none, s));
  none = cardVolume(0);
  none.nFatent = 2;
  TEST_ASSERT_EQUAL_UINT32(0, holehint::lastClst(none, none.database));
}

// FATFS's fields onTrim() reads and writes, in a struct of its own (the
// template's other volume: the device's ff.h has its own FATFS).
struct FakeFs {
  uint8_t fs_type = 3;  // FS_FAT32
  uint8_t pdrv = 0;
  uint16_t csize = 64;
  uint32_t last_clst = 500000;
  uint32_t free_clst = 1;
  uint32_t n_fatent = kCardFatent;
  uint32_t database = 32768;
};

uint32_t fakeSector(uint32_t cl) { return 32768 + (cl - 2) * 64; }

// U5: the hook's glue. No volume, an unmounted one (f_mkfs's trims), another
// drive's, a sector that isn't a cluster's: last_clst left. A run freed: last
// cl - 1. More runs with nothing allocated between: the lowest stays,
// whichever order they come in; an allocation (last_clst moved) lets the next
// run's in again.
void test_u5_on_trim() {
  holehint::Memory mem;
  FakeFs fs;
  TEST_ASSERT_FALSE(holehint::onTrim(static_cast<FakeFs*>(nullptr), 0, fakeSector(1000), mem));
  fs.fs_type = 0;
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 0, fakeSector(1000), mem));
  fs.fs_type = 3;
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 1, fakeSector(1000), mem));
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 0, fakeSector(1000) + 1, mem));
  fs.free_clst = 119;
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 0, fakeSector(1000), mem));
  TEST_ASSERT_EQUAL_UINT32(500000, fs.last_clst);

  // A chain freed low run first (A then B), B at most kNearRun clusters above
  // A: A's stays.
  TEST_ASSERT_EQUAL_UINT32(8192, holehint::kNearRun);
  fs.free_clst = 1;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(1000), mem));
  TEST_ASSERT_EQUAL_UINT32(999, fs.last_clst);
  fs.free_clst = 2;
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 0, fakeSector(1000 + holehint::kNearRun), mem));
  TEST_ASSERT_EQUAL_UINT32(999, fs.last_clst);
  // ... a run farther above the kept one: its own (where the chain's walk
  // ended), and then one far above that.
  fs.free_clst = 3;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(1001 + holehint::kNearRun), mem));
  TEST_ASSERT_EQUAL_UINT32(1000 + holehint::kNearRun, fs.last_clst);
  fs.free_clst = 4;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(300000), mem));
  TEST_ASSERT_EQUAL_UINT32(299999, fs.last_clst);
  // ... high run first (B then A): A's takes its place.
  holehint::Memory mem2;
  fs.last_clst = 500000;
  fs.free_clst = 1;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(300000), mem2));
  TEST_ASSERT_EQUAL_UINT32(299999, fs.last_clst);
  fs.free_clst = 2;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(1000), mem2));
  TEST_ASSERT_EQUAL_UINT32(999, fs.last_clst);
  // Cluster 2 is the lowest there is (its hint n_fatent - 1 is no "higher"):
  // a run near it stays behind it, a run far above it doesn't.
  fs.free_clst = 3;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(2), mem2));
  TEST_ASSERT_EQUAL_UINT32(kCardFatent - 1, fs.last_clst);
  fs.free_clst = 4;
  TEST_ASSERT_FALSE(holehint::onTrim(&fs, 0, fakeSector(1000), mem2));
  TEST_ASSERT_EQUAL_UINT32(kCardFatent - 1, fs.last_clst);
  fs.free_clst = 5;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(400000), mem2));
  TEST_ASSERT_EQUAL_UINT32(399999, fs.last_clst);
  // FatFs allocated (last_clst moved): a run freed now is the start, even one
  // near above the last.
  fs.last_clst = 2;
  fs.free_clst = 5;
  TEST_ASSERT_TRUE(holehint::onTrim(&fs, 0, fakeSector(400100), mem2));
  TEST_ASSERT_EQUAL_UINT32(400099, fs.last_clst);
}

// ---- under FatFs, on the device's card ----

// The device's card: 62,333,952 sectors, FAT32, 32 KB clusters.
constexpr uint32_t kSectors = 62333952;
constexpr uint32_t kClusterBytes = 32768;
// A file's clusters at most: FatFs's file size is 32 bits (4 GB - 1 B).
constexpr uint32_t kMaxRun = 131071;
// queue.txt's cluster: the player's files sit low on a card filled after them.
constexpr uint32_t kPlayer = 64;
// The model's format of that card (one MBR partition, FatFs's layout):
// 973,728 clusters. (The device's card, its partition at sector 8192, has
// 973,584: the same gate, 118, and the same FAT within a sector.)
constexpr uint32_t kFatent = 973730;

// The FAT sectors that hold entries: a search through the whole FAT reads
// about this many.
constexpr uint32_t kSpan = (kFatent + 127) / 128;

// One cluster of the scene's card: free (name nullptr) or a file of one
// cluster by that name. Every other cluster from 3 on is a filler's.
struct Spot {
  uint32_t cluster;
  const char* name;
};

std::string at(const std::string& name) { return fatmodel::path(0, name); }

void attach(fatmodel::RamDisk* disk, FATFS* fs, bool hint) {
  fatmodel::drive(0) = fatmodel::Drive{};
  fatmodel::drive(0).disk = disk;
  fatmodel::drive(0).fs = fs;
  fatmodel::drive(0).holeHint = hint;
}

// The card filled as the spots say (built once per layout, with FatFs: the
// fillers by f_expand, each spot a file of one cluster, the free spots removed
// at the end, the hook off).
const fatmodel::RamDisk& image(std::vector<Spot> spots) {
  static std::map<std::string, fatmodel::RamDisk*> built;
  std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.cluster < b.cluster; });
  std::string key;
  for (const Spot& s : spots) key += std::to_string(s.cluster) + (s.name ? s.name : "-") + ",";
  auto it = built.find(key);
  if (it != built.end()) return *it->second;

  auto* disk = new fatmodel::RamDisk(kSectors);
  auto* fs = new FATFS();
  attach(disk, fs, false);
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, kClusterBytes));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, fs));
  TEST_ASSERT_EQUAL_UINT32(kFatent, fs->n_fatent);
  TEST_ASSERT_EQUAL_UINT32(64, fs->csize);
  uint32_t cursor = 3;  // (2: the root)
  int fillers = 0;
  std::vector<std::string> spacers;
  auto expand = [&](const std::string& name, uint32_t first, uint32_t clusters) {
    fs->last_clst = first - 1;  // (f_expand looks from here; everything past it is free)
    FIL f;
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at(name).c_str(), FA_CREATE_NEW | FA_WRITE));
    TEST_ASSERT_EQUAL(FR_OK, f_expand(&f, static_cast<FSIZE_t>(clusters) * kClusterBytes, 1));
    TEST_ASSERT_EQUAL_UINT32(first, f.obj.sclust);
    TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  };
  auto fillTo = [&](uint32_t end) {
    while (cursor < end) {
      const uint32_t k = std::min(end - cursor, kMaxRun);
      expand("fill" + std::to_string(fillers++) + ".bin", cursor, k);
      cursor += k;
    }
  };
  for (const Spot& s : spots) {
    TEST_ASSERT_TRUE(s.cluster >= cursor && s.cluster < kFatent);
    fillTo(s.cluster);
    const std::string name = s.name ? s.name : "free" + std::to_string(spacers.size()) + ".bin";
    expand(name, s.cluster, 1);
    if (!s.name) spacers.push_back(name);
    cursor = s.cluster + 1;
  }
  fillTo(kFatent);
  for (const std::string& s : spacers) TEST_ASSERT_EQUAL(FR_OK, f_unlink(at(s).c_str()));
  TEST_ASSERT_EQUAL_UINT32(spacers.size(), fs->free_clst);
  f_mount(nullptr, "0:", 0);
  delete fs;
  built[key] = disk;
  return *disk;
}

// A scene's card: a copy of its image, mounted (on the heap, kept until the
// next scene: a failed assertion jumps past destructors).
struct Card {
  fatmodel::RamDisk disk;
  FATFS fs;
};

Card& scene(const std::vector<Spot>& spots, bool hint) {
  static Card* c = nullptr;
  if (c) {
    f_mount(nullptr, "0:", 0);
    fatmodel::drive(0) = fatmodel::Drive{};
    delete c;
    c = nullptr;
  }
  const fatmodel::RamDisk& img = image(spots);
  c = new Card{img, FATFS()};
  attach(&c->disk, &c->fs, hint);
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &c->fs));
  return *c;
}

// A fresh format of the device's card (plenty of room).
Card& blankScene(bool hint) {
  static Card* c = nullptr;
  if (c) {
    f_mount(nullptr, "0:", 0);
    fatmodel::drive(0) = fatmodel::Drive{};
    delete c;
    c = nullptr;
  }
  c = new Card{fatmodel::RamDisk(kSectors), FATFS()};
  attach(&c->disk, &c->fs, hint);
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, kClusterBytes));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &c->fs));
  return *c;
}

const uint8_t* bytes() {
  static std::vector<uint8_t> b(65536, 0x5A);
  return b.data();
}

// `n` bytes written to an open file at its end (whole sectors: nothing read
// but the FAT): the disk reads they took. `put`: what f_write wrote.
uint32_t writeAt(FIL* f, uint32_t n, UINT* put = nullptr) {
  fatmodel::Meter m(0);
  UINT w = 0;
  TEST_ASSERT_EQUAL(FR_OK, f_write(f, bytes(), n, &w));
  const uint32_t reads = static_cast<uint32_t>(m.read().asked);
  if (put) {
    *put = w;
  } else {
    TEST_ASSERT_EQUAL_UINT32(n, w);
  }
  return reads;
}

// A new file (`mode`: FatFs's), `n` bytes, closed: its write's disk reads.
// `first`: its first cluster.
uint32_t writeNew(const std::string& name, uint32_t n, uint32_t* first = nullptr,
                  BYTE mode = FA_CREATE_ALWAYS | FA_WRITE) {
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at(name).c_str(), mode));
  const uint32_t reads = writeAt(&f, n);
  if (first) *first = f.obj.sclust;
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  return reads;
}

uint32_t firstCluster(const std::string& name) {
  FIL f;
  if (f_open(&f, at(name).c_str(), FA_READ) != FR_OK) return 0;
  const uint32_t c = f.obj.sclust;
  f_close(&f);
  return c;
}

uint32_t sectorOf(uint32_t cl) { return cl / 128; }  // (FAT32: 128 entries a sector)

// A file stretched by a cluster reads its last cluster's FAT sector before the
// search and again to link the new one (FatFs's window; the device's sector
// cache has it): a search that finds the cluster at once reads at most this.
constexpr uint32_t kStretchReads = 3;

// The queue's save as the firmware makes it through the VFS (QueueStore:
// openTemp() opens queue.tmp to write, a new file; commitTemp() closes it;
// replaceFile() removes queue.txt and renames queue.tmp): its writes' disk
// reads. `n` bytes (a multiple of 4 KB) in 4 KB writes, the stdio buffer's
// flushes; 4 KB: a text that fits one cluster.
uint32_t queueSave(uint32_t n = 4096) {
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at("queue.tmp").c_str(), FA_CREATE_ALWAYS | FA_WRITE));
  uint32_t reads = 0;
  for (uint32_t done = 0; done < n; done += 4096) reads += writeAt(&f, 4096);
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("queue.txt").c_str()));
  TEST_ASSERT_EQUAL(FR_OK, f_rename(at("queue.tmp").c_str(), at("queue.txt").c_str()));
  return reads;
}

constexpr uint32_t kHalf = kFatent / 2;

// M0: the model's card: 973,730 FAT entries in 7,608 sectors (the device's
// 7,607); a search through the whole FAT reads them all.
void test_m0_the_card() {
  Card& c = scene({{kHalf, nullptr}}, false);
  TEST_ASSERT_EQUAL_UINT32(kFatent, c.fs.n_fatent);
  TEST_ASSERT_EQUAL_UINT32(7608, kSpan);
  TEST_ASSERT_EQUAL_UINT32(118, (kFatent - 2) / holehint::kEntriesPerFree);
  TEST_ASSERT_EQUAL_UINT32(1, c.fs.free_clst);
  printf("[hole hint] the card: %lu FAT entries, fsize %lu sectors, data from sector %lu\n",
         (unsigned long)c.fs.n_fatent, (unsigned long)c.fs.fsize, (unsigned long)c.fs.database);
}

// M1: a write that takes a cluster and frees it again (the stats branch's
// failed stats.tmp, the queue's dropped write) leaves last_clst on the only
// free cluster: the next new file's write reads the whole FAT. With the hint
// it finds it at once.
uint32_t allocThenFree(bool hint, uint32_t* tmpReads) {
  Card& c = scene({{kHalf, nullptr}}, hint);
  c.fs.last_clst = kFatent - 1;  // the fill's last allocation
  uint32_t got = 0;
  *tmpReads = writeNew("x.tmp", 4096, &got);
  TEST_ASSERT_EQUAL_UINT32(kHalf, got);
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("x.tmp").c_str()));
  const uint32_t reads = writeNew("y.tmp", 4096, &got);
  TEST_ASSERT_EQUAL_UINT32(kHalf, got);
  return reads;
}

void test_m1_alloc_then_free() {
  uint32_t offTmp = 0, onTmp = 0;
  const uint32_t off = allocThenFree(false, &offTmp);
  TEST_ASSERT_EQUAL_UINT32(0, fatmodel::drive(0).asked.holeHints);
  const uint32_t on = allocThenFree(true, &onTmp);
  printf("[hole hint] M1 alloc then free: the next write read %lu FAT sectors without the hint, %lu with it\n",
         (unsigned long)off, (unsigned long)on);
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(kSpan - 2, off);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, on);
  TEST_ASSERT_EQUAL_UINT32(offTmp, onTmp);  // (the first: from the same start)
  TEST_ASSERT_EQUAL_UINT32(1, fatmodel::drive(0).asked.holeHints);
}

// M2: the queue's save six times on a card with one free cluster. Without the
// hint queue.tmp and queue.txt take turns in two clusters, and every two
// saves in a row read the whole FAT between them. With it each save after the
// first takes the cluster the last one freed, at once. The first starts where
// the scene does, the same either way.
void queueSaves(bool hint, uint32_t reads[6]) {
  Card& c = scene({{kPlayer, "queue.txt"}, {kHalf, nullptr}}, hint);
  c.fs.last_clst = kFatent - 1;
  for (int i = 0; i < 6; ++i) reads[i] = queueSave();
  TEST_ASSERT_EQUAL_UINT32(1, c.fs.free_clst);
}

void test_m2_queue_saves() {
  uint32_t off[6], on[6];
  queueSaves(false, off);
  queueSaves(true, on);
  printf("[hole hint] M2 queue saves, FAT sectors read: without %lu %lu %lu %lu %lu %lu; with %lu %lu %lu %lu %lu %lu\n",
         (unsigned long)off[0], (unsigned long)off[1], (unsigned long)off[2], (unsigned long)off[3],
         (unsigned long)off[4], (unsigned long)off[5], (unsigned long)on[0], (unsigned long)on[1],
         (unsigned long)on[2], (unsigned long)on[3], (unsigned long)on[4], (unsigned long)on[5]);
  for (int i = 1; i < 5; ++i) TEST_ASSERT_GREATER_OR_EQUAL_UINT32(kSpan - 2, off[i] + off[i + 1]);
  TEST_ASSERT_EQUAL_UINT32(off[0], on[0]);
  for (int i = 1; i < 6; ++i) TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, on[i]);
  TEST_ASSERT_EQUAL_UINT32(6, fatmodel::drive(0).asked.holeHints);
}

// ... the same through tagstore::replace() over FatFs (the queue with twins
// on the card, device.txt, library.idx: CardFat's files, ModelFs here).
void replaceSaves(bool hint, uint32_t reads[6]) {
  Card& c = scene({{kPlayer, "queue.txt"}, {kHalf, nullptr}}, hint);
  c.fs.last_clst = kFatent - 1;
  fatmodel::ModelFs mfs;
  const tagstore::Names names{"/queue.txt", "/queue.tmp", "/queue"};
  for (int i = 0; i < 6; ++i) {
    tagstore::File* f = mfs.open("/queue.tmp", tagstore::Fs::Mode::Create);
    TEST_ASSERT_NOT_NULL(f);
    fatmodel::Meter m(0);
    TEST_ASSERT_TRUE(f->write(0, bytes(), 4096));
    reads[i] = static_cast<uint32_t>(m.read().asked);
    TEST_ASSERT_TRUE(mfs.close(f));
    TEST_ASSERT_TRUE(tagstore::replace(mfs, names));
  }
}

void test_m2_through_tagstore_replace() {
  uint32_t off[6], on[6];
  replaceSaves(false, off);
  replaceSaves(true, on);
  for (int i = 1; i < 5; ++i) TEST_ASSERT_GREATER_OR_EQUAL_UINT32(kSpan - 2, off[i] + off[i + 1]);
  for (int i = 1; i < 6; ++i) TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, on[i]);
  TEST_ASSERT_EQUAL_UINT32(6, fatmodel::drive(0).asked.holeHints);
}

// M3: FSINFO carries the hint: f_unlink()'s sync writes the count it changed
// and last_clst with it. Mounted again, FatFs starts there, and the next
// save's write finds the cluster at once. Without the hint the mount starts
// at the last allocation (queue.tmp's cluster).
void test_m3_fsinfo_carries_it() {
  for (int hint = 0; hint < 2; ++hint) {
    Card& c = scene({{kPlayer, "queue.txt"}, {kHalf, nullptr}}, hint != 0);
    c.fs.last_clst = kFatent - 1;
    queueSave();
    const uint32_t freed = firstCluster("queue.txt");  // the next remove frees this one
    TEST_ASSERT_EQUAL_UINT32(kHalf, freed);
    queueSave();
    const uint32_t tmp = firstCluster("queue.txt");
    TEST_ASSERT_EQUAL_UINT32(kPlayer, tmp);
    TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "0:", 0));
    TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &c.fs));
    TEST_ASSERT_EQUAL_UINT32(1, c.fs.free_clst);
    if (hint) {
      TEST_ASSERT_EQUAL_UINT32(freed - 1, c.fs.last_clst);
      TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, queueSave());
    } else {
      TEST_ASSERT_EQUAL_UINT32(tmp, c.fs.last_clst);
    }
  }
}

// M4: another writer takes the hole first (the card worker's thumbnail between
// two of the queue's saves). The card is full: the queue's next write stops at
// FatFs's early out (a known count of 0) at once, holding no cluster, and the
// remove of that empty queue.tmp frees nothing: no TRIM, no hint, last_clst
// as it was. Its retry (10 s on) fails as fast. Once the thumbnail is
// removed, its cluster is the next save's at once.
void test_m4_another_writer_first() {
  Card& c = scene({{kPlayer, "queue.txt"}, {kHalf, nullptr}}, true);
  c.fs.last_clst = kFatent - 1;
  queueSave();
  TEST_ASSERT_EQUAL_UINT32(kPlayer - 1, c.fs.last_clst);
  uint32_t thumb = 0;
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, writeNew("thumb.bin", 4096, &thumb));
  TEST_ASSERT_EQUAL_UINT32(kPlayer, thumb);
  TEST_ASSERT_EQUAL_UINT32(0, c.fs.free_clst);
  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint64_t trims = fatmodel::drive(0).asked.trims;
    const uint64_t hints = fatmodel::drive(0).asked.holeHints;
    FIL f;
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at("queue.tmp").c_str(), FA_CREATE_ALWAYS | FA_WRITE));
    UINT put = 1;
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(1, writeAt(&f, 4096, &put));
    TEST_ASSERT_EQUAL_UINT32(0, put);
    TEST_ASSERT_EQUAL_UINT32(0, f.obj.sclust);
    TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
    const uint32_t before = c.fs.last_clst;
    TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("queue.tmp").c_str()));  // (commitTemp(): not whole, removed)
    TEST_ASSERT_EQUAL_UINT32(before, c.fs.last_clst);
    TEST_ASSERT_EQUAL_UINT64(trims, fatmodel::drive(0).asked.trims);
    TEST_ASSERT_EQUAL_UINT64(hints, fatmodel::drive(0).asked.holeHints);
  }
  TEST_ASSERT_EQUAL_UINT32(kHalf, firstCluster("queue.txt"));  // the queue as it was
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("thumb.bin").c_str()));
  TEST_ASSERT_EQUAL_UINT32(kPlayer - 1, c.fs.last_clst);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, queueSave());
  TEST_ASSERT_EQUAL_UINT32(kPlayer, firstCluster("queue.txt"));
}

// M5: room. On a card with more free clusters than the gate a remove leaves
// last_clst as it was and the next new file goes to last_clst + 1 (FatFs's
// order kept). At the gate on the full card: a remove that makes 119 free
// leaves it too, one that makes 118 moves it.
void test_m5_room_keeps_fatfs_order() {
  Card& c = blankScene(true);
  uint32_t a = 0, b = 0, d = 0;
  writeNew("a.bin", 32768, &a);
  writeNew("b.bin", 32768, &b);
  TEST_ASSERT_EQUAL_UINT32(a + 1, b);
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("a.bin").c_str()));
  TEST_ASSERT_EQUAL_UINT32(b, c.fs.last_clst);
  writeNew("d.bin", 32768, &d);
  TEST_ASSERT_EQUAL_UINT32(b + 1, d);
  TEST_ASSERT_EQUAL_UINT64(0, fatmodel::drive(0).asked.holeHints);

  for (uint32_t left : {118u, 117u}) {
    std::vector<Spot> spots{{kPlayer, "old.bin"}};
    for (uint32_t k = 0; k < left; ++k) spots.push_back({kFatent - 1 - k, nullptr});
    Card& full = scene(spots, true);
    full.fs.last_clst = kFatent - 1 - left;
    TEST_ASSERT_EQUAL_UINT32(left, full.fs.free_clst);
    TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("old.bin").c_str()));
    TEST_ASSERT_EQUAL_UINT32(left == 118 ? kFatent - 1 - left : kPlayer - 1, full.fs.last_clst);
  }
}

// M6: f_truncate() of a tail that isn't the next cluster (the play log's cut,
// a fragmented file): FatFs stretches the file next from last_clst, far past
// the freed cluster without the hint; with it the freed cluster is found at
// once. Clusters A < B < C free; the file is A -> B; another file takes C;
// the cut frees B.
uint32_t truncateScene(bool hint, uint32_t* got) {
  const uint32_t A = kFatent / 4, B = kFatent / 2, C = kFatent / 4 * 3;
  Card& c = scene({{A, nullptr}, {B, nullptr}, {C, nullptr}}, hint);
  c.fs.last_clst = A - 1;
  FIL p;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&p, at("plays.jnl").c_str(), FA_CREATE_NEW | FA_WRITE));
  writeAt(&p, 32768);
  writeAt(&p, 32768);
  TEST_ASSERT_EQUAL_UINT32(A, p.obj.sclust);
  TEST_ASSERT_EQUAL_UINT32(B, p.clust);
  TEST_ASSERT_EQUAL(FR_OK, f_sync(&p));
  uint32_t w = 0;
  writeNew("w.bin", 32768, &w);
  TEST_ASSERT_EQUAL_UINT32(C, w);
  TEST_ASSERT_EQUAL(FR_OK, f_lseek(&p, 32768));
  TEST_ASSERT_EQUAL(FR_OK, f_truncate(&p));
  TEST_ASSERT_EQUAL_UINT32(hint ? B - 1 : C, c.fs.last_clst);
  const uint32_t reads = writeAt(&p, 32768);
  *got = p.clust;
  TEST_ASSERT_EQUAL(FR_OK, f_close(&p));
  return reads;
}

void test_m6_truncated_tail() {
  uint32_t offGot = 0, onGot = 0;
  const uint32_t off = truncateScene(false, &offGot);
  const uint32_t on = truncateScene(true, &onGot);
  printf("[hole hint] M6 a cut tail: the file's next cluster read %lu FAT sectors without the hint, %lu with it\n",
         (unsigned long)off, (unsigned long)on);
  const uint32_t B = kFatent / 2, C = kFatent / 4 * 3;
  TEST_ASSERT_EQUAL_UINT32(B, offGot);
  TEST_ASSERT_EQUAL_UINT32(B, onGot);
  // Without: from C to the end, then from the start to B.
  TEST_ASSERT_GREATER_OR_EQUAL_UINT32(sectorOf(kFatent - 1) - sectorOf(C) + sectorOf(B) - 1, off);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(kStretchReads, on);
}

// M7: f_open(FA_CREATE_ALWAYS) over a file: FatFs's own hole reuse
// (last_clst = cl - 1) is the last word, and the same value.
void test_m7_create_always_over_a_file() {
  const uint32_t K = kFatent / 3;
  Card& c = scene({{K, "x.bin"}, {kHalf, nullptr}}, true);
  c.fs.last_clst = kFatent - 1;
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at("x.bin").c_str(), FA_CREATE_ALWAYS | FA_WRITE));
  TEST_ASSERT_EQUAL_UINT32(K - 1, c.fs.last_clst);
  TEST_ASSERT_EQUAL_UINT64(1, fatmodel::drive(0).asked.holeHints);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, writeAt(&f, 4096));
  TEST_ASSERT_EQUAL_UINT32(K, f.obj.sclust);
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
}

// M8: a file in two pieces (A < B, the only free clusters) removed, then two
// files of one cluster. remove_chain() trims a run at a time in the chain's
// order. Without the hint the two writes read the whole FAT between them.
// The pieces near (B at most kNearRun clusters above A): the hint is A's
// whichever comes first, so the first write takes A at once and the second
// searches only from A to B. Far apart (a quarter of the FAT): the low piece
// first (A then B) leaves B's, the start the chain's walk ended at, so the
// second write wraps to A; the high piece first leaves A's.
uint32_t twoPieces(bool hint, bool lowFirst, uint32_t A, uint32_t B, uint32_t* firstReads, uint32_t* firstGot) {
  Card& c = scene({{A, nullptr}, {B, nullptr}}, hint);
  c.fs.last_clst = (lowFirst ? A : B) - 1;
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, at("frag.bin").c_str(), FA_CREATE_NEW | FA_WRITE));
  writeAt(&f, 65536);
  TEST_ASSERT_EQUAL_UINT32(lowFirst ? A : B, f.obj.sclust);
  TEST_ASSERT_EQUAL_UINT32(lowFirst ? B : A, f.clust);
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("frag.bin").c_str()));
  TEST_ASSERT_EQUAL_UINT32(2, c.fs.free_clst);
  uint32_t g2 = 0;
  *firstReads = writeNew("g1.bin", 32768, firstGot);
  const uint32_t second = writeNew("g2.bin", 32768, &g2);
  TEST_ASSERT_TRUE((*firstGot == A && g2 == B) || (*firstGot == B && g2 == A));
  return *firstReads + second;
}

void test_m8_two_pieces_near() {
  const uint32_t A = kFatent / 4, B = A + 4000;
  for (int lowFirst = 0; lowFirst < 2; ++lowFirst) {
    uint32_t offFirst = 0, onFirst = 0, offGot = 0, onGot = 0;
    const uint32_t off = twoPieces(false, lowFirst != 0, A, B, &offFirst, &offGot);
    const uint32_t on = twoPieces(true, lowFirst != 0, A, B, &onFirst, &onGot);
    printf("[hole hint] M8 two pieces near (%s first): two writes read %lu FAT sectors without the hint, %lu with it\n",
           lowFirst ? "the low" : "the high", (unsigned long)off, (unsigned long)on);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(kSpan - 2, off);
    TEST_ASSERT_EQUAL_UINT32(A, onGot);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, onFirst);
    // The first at once; the second from A's FAT sector to B's.
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(sectorOf(B) - sectorOf(A) + 3, on);
  }
}

void test_m8_two_pieces_far_apart() {
  const uint32_t A = kFatent / 4, B = kFatent / 2;
  uint32_t off[2], on[2];
  for (int lowFirst = 0; lowFirst < 2; ++lowFirst) {
    uint32_t offFirst = 0, onFirst = 0, offGot = 0, onGot = 0;
    off[lowFirst] = twoPieces(false, lowFirst != 0, A, B, &offFirst, &offGot);
    on[lowFirst] = twoPieces(true, lowFirst != 0, A, B, &onFirst, &onGot);
    printf("[hole hint] M8 two pieces far apart (%s first): two writes read %lu FAT sectors without the hint, %lu "
           "with it\n",
           lowFirst ? "the low" : "the high", (unsigned long)off[lowFirst], (unsigned long)on[lowFirst]);
    TEST_ASSERT_EQUAL_UINT32(lowFirst ? B : A, onGot);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(2, onFirst);
  }
  // Pinned: the high first, the second write from A's FAT sector to B's; the
  // low first, from B's to the end and from the start to A's. FatFs alone
  // read the whole FAT either way.
  TEST_ASSERT_EQUAL_UINT32(7610, off[0]);
  TEST_ASSERT_EQUAL_UINT32(7610, off[1]);
  TEST_ASSERT_EQUAL_UINT32(1904, on[0]);
  TEST_ASSERT_EQUAL_UINT32(5708, on[1]);
}

// M9: what the hint costs. A hole low on the card (a 1-cluster file removed)
// and a run of 100 free clusters at the end, where FatFs's next search would
// start (101 free: under the gate). A write of two clusters takes the hole,
// then searches from it to the run: one walk (FatFs alone: none). From then on
// the run, at once. Pinned so a change to the gate or the rule shows here.
uint32_t holeAndRun(bool hint, uint32_t* next) {
  const uint32_t run = 100, H = 1000;
  std::vector<Spot> spots{{H, "old.bin"}};
  for (uint32_t k = 0; k < run; ++k) spots.push_back({kFatent - 1 - k, nullptr});
  Card& c = scene(spots, hint);
  c.fs.last_clst = kFatent - 1 - run;  // the allocation just before the run
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(at("old.bin").c_str()));
  uint32_t first = 0;
  const uint32_t reads = writeNew("lib.idx", 65536, &first);
  TEST_ASSERT_EQUAL_UINT32(hint ? H : kFatent - run, first);
  *next = writeNew("next.bin", 65536);
  return reads;
}

void test_m9_hole_far_from_the_run() {
  const uint32_t run = 100, H = 1000;
  uint32_t offNext = 0, onNext = 0;
  const uint32_t off = holeAndRun(false, &offNext);
  const uint32_t on = holeAndRun(true, &onNext);
  printf("[hole hint] M9 a hole far from the run: a 2-cluster write read %lu FAT sectors without the hint, %lu with "
         "it; the next %lu and %lu\n",
         (unsigned long)off, (unsigned long)on, (unsigned long)offNext, (unsigned long)onNext);
  // Pinned. The hole's FAT sector, the walk to the run's, the hole's again
  // (the link): a whole FAT less the run's few sectors.
  TEST_ASSERT_EQUAL_UINT32(1, off);
  TEST_ASSERT_EQUAL_UINT32(7601, on);
  TEST_ASSERT_LESS_OR_EQUAL_UINT32(sectorOf(kFatent - run) - sectorOf(H) + 3, on);
  TEST_ASSERT_EQUAL_UINT32(1, offNext);
  TEST_ASSERT_EQUAL_UINT32(1, onNext);
}

// The card of M9 and M10: queue.txt in kPlayer, one cluster; free a run of
// `run` clusters at the end (under the gate), where FatFs's next search starts.
Card& queueAndRun(bool hint, uint32_t run = 100) {
  std::vector<Spot> spots{{kPlayer, "queue.txt"}};
  for (uint32_t k = 0; k < run; ++k) spots.push_back({kFatent - 1 - k, nullptr});
  Card& c = scene(spots, hint);
  c.fs.last_clst = kFatent - 1 - run;
  return c;
}

void printSaves(const char* what, const std::vector<uint32_t>& off, const std::vector<uint32_t>& on) {
  printf("[hole hint] %s, FAT sectors read a save: without", what);
  for (uint32_t r : off) printf(" %lu", (unsigned long)r);
  printf("; with");
  for (uint32_t r : on) printf(" %lu", (unsigned long)r);
  printf("\n");
}

// ... and the queue's saves there once its text needs two clusters (more than
// 32 KB: about 400 tracks or more). Without the hint they go through the run
// at once (a walk only when it wraps, every 50 saves here). With it the first
// save's remove points FatFs at queue.txt's old low cluster, and the second
// save takes it and walks to the run: M9's walk, once. That save's chain
// starts low and ends in the run; when the third save's remove frees it, the
// run's piece (far above the low one) is the start, and from then on the run
// at once. Pinned: under a hint that kept the low piece (the lowest run freed,
// wherever the later ones lay) every second save walked: 1 7608 1 7608 ...
void test_m9_two_cluster_queue_saves() {
  std::vector<uint32_t> reads[2];
  for (int hint = 0; hint < 2; ++hint) {
    queueAndRun(hint != 0);
    for (int i = 0; i < 6; ++i) reads[hint].push_back(queueSave(65536));
  }
  printSaves("M9 two-cluster queue saves, a run far from queue.txt", reads[0], reads[1]);
  const std::vector<uint32_t> without{1, 1, 1, 1, 1, 1}, with{1, 7608, 1, 1, 1, 1};
  TEST_ASSERT_EQUAL_UINT32_ARRAY(without.data(), reads[0].data(), 6);
  TEST_ASSERT_EQUAL_UINT32_ARRAY(with.data(), reads[1].data(), 6);
}

// M10: the queue growing a cluster at a time there (2, then 3, then 4
// clusters: Auto DJ adding tracks). Each removed chain keeps its pieces near
// each other in the run, the low one stays the start, and the saves take
// turns in the run without drifting through it. Pinned, 12 saves: the one
// walk of M9 and nothing more (under the lowest-run hint: 1 7608 1 7609 ...).
void test_m10_growing_queue_saves() {
  const uint32_t sizes[] = {65536, 65536, 65536, 98304, 98304, 98304, 98304, 131072, 131072, 131072, 131072, 131072};
  std::vector<uint32_t> reads[2];
  for (int hint = 0; hint < 2; ++hint) {
    queueAndRun(hint != 0);
    for (uint32_t n : sizes) reads[hint].push_back(queueSave(n));
  }
  printSaves("M10 a growing queue, a run far from queue.txt", reads[0], reads[1]);
  const std::vector<uint32_t> without(12, 1);
  std::vector<uint32_t> with(12, 1);
  with[1] = 7608;
  TEST_ASSERT_EQUAL_UINT32_ARRAY(without.data(), reads[0].data(), 12);
  TEST_ASSERT_EQUAL_UINT32_ARRAY(with.data(), reads[1].data(), 12);
}

// ... over a long session: the text from 20 KB to 140 KB, 4 KB at a time, four
// saves a size (124 saves), on three cards: the run at the end, the run in
// the middle of the card (FatFs's start at the end), two lone holes and a run
// of 50 at the end. Counted: the saves that read more than a stretch's
// kStretchReads (each a walk of hundreds to thousands of FAT sectors here).
// The hint walks no more often than FatFs alone (3, 7 and 11 times), and
// pinned: 1, 2 and 3 times. Under rules tried before this one: the lowest run
// freed, wherever the later ones lay, 54, 55 and 54 (every second save); the
// last run freed, 5, 5 and 12 (the saves drift through the run and wrap).
uint32_t longSession(bool hint, const std::vector<Spot>& spots, uint32_t lastClst) {
  Card& c = scene(spots, hint);
  c.fs.last_clst = lastClst;
  uint32_t walks = 0;
  for (uint32_t kb = 20; kb <= 140; kb += 4) {
    for (int i = 0; i < 4; ++i) {
      if (queueSave(kb * 1024) > kStretchReads) ++walks;
    }
  }
  return walks;
}

void test_m10_long_session() {
  const uint32_t run = 100;
  std::vector<Spot> end{{kPlayer, "queue.txt"}}, middle{{kPlayer, "queue.txt"}},
      holes{{kPlayer, "queue.txt"}, {kFatent / 4, nullptr}, {kHalf, nullptr}};
  for (uint32_t k = 0; k < run; ++k) end.push_back({kFatent - 1 - k, nullptr});
  for (uint32_t k = 0; k < run; ++k) middle.push_back({kHalf + k, nullptr});
  for (uint32_t k = 0; k < 50; ++k) holes.push_back({kFatent - 1 - k, nullptr});
  struct Layout {
    const char* name;
    const std::vector<Spot>& spots;
    uint32_t lastClst;
    uint32_t withoutWalks, withWalks;
  } layouts[] = {
      {"the run at the end", end, kFatent - 1 - run, 3, 1},
      {"the run in the middle", middle, kFatent - 1, 7, 2},
      {"two holes and a run", holes, kFatent - 51, 11, 3},
  };
  for (const Layout& l : layouts) {
    const uint32_t off = longSession(false, l.spots, l.lastClst);
    const uint32_t on = longSession(true, l.spots, l.lastClst);
    printf("[hole hint] M10 124 saves of a growing queue, %s: %lu walked without the hint, %lu with it\n", l.name,
           (unsigned long)off, (unsigned long)on);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(off, on);
    TEST_ASSERT_EQUAL_UINT32(l.withoutWalks, off);
    TEST_ASSERT_EQUAL_UINT32(l.withWalks, on);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_u1_cluster_first_sector);
  RUN_TEST(test_u2_not_a_cluster);
  RUN_TEST(test_u3_count_unknown);
  RUN_TEST(test_u4_gate);
  RUN_TEST(test_u5_on_trim);
  RUN_TEST(test_m0_the_card);
  RUN_TEST(test_m1_alloc_then_free);
  RUN_TEST(test_m2_queue_saves);
  RUN_TEST(test_m2_through_tagstore_replace);
  RUN_TEST(test_m3_fsinfo_carries_it);
  RUN_TEST(test_m4_another_writer_first);
  RUN_TEST(test_m5_room_keeps_fatfs_order);
  RUN_TEST(test_m6_truncated_tail);
  RUN_TEST(test_m7_create_always_over_a_file);
  RUN_TEST(test_m8_two_pieces_near);
  RUN_TEST(test_m8_two_pieces_far_apart);
  RUN_TEST(test_m9_hole_far_from_the_run);
  RUN_TEST(test_m9_two_cluster_queue_saves);
  RUN_TEST(test_m10_growing_queue_saves);
  RUN_TEST(test_m10_long_session);
  return UNITY_END();
}
