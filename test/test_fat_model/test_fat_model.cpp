// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The host FatFs model (docs/METADATA.md 3.2.7, row N8 of 6.1): ChaN's
// FatFs R0.15 as the Core2 builds it, on RAM disks (test/support/
// FatModel.h).
// - The cache under FatFs: the same random file system work on two
//   drives, one through a SectorCache (a small one that evicts all the
//   time, and one big enough to keep what it read), gives the same
//   results, the same bytes read and the same card image. TRIMs scramble
//   their sectors and the allocation is sent back to the start now and
//   then, so freed clusters are used again: a trimmed sector the cache
//   kept would be read back.
// - A 20k tree in the user's shape (LibrarySynth's userShape): FatFs reads
//   exactly the directory sectors the research's model counts per lookup
//   (each level's sectors up to the name's entry, and a FAT sector where
//   a folder crosses a cluster); with the cache a lookup reads next to
//   nothing; the walk reads each folder's sectors about once.
// - A card swapped under the firmware's wrapper (CachedDrive, N10's
//   review): the card stops answering, FatFs mounts the volume again by
//   itself, and the wrapper's init() clears the cache, so FatFs reads the
//   new card's boot sector, FAT and folders, and a write there leaves the
//   image a FatFs without the cache would.
// tools/fatmodel.py runs the same model on tools/synthcard.py's card.
// Run: pio test -e native -f test_fat_model
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "LibrarySynth.h"
#include "SectorCache.h"

namespace {

using fatmodel::Drive;
using fatmodel::Meter;
using fatmodel::RamDisk;
using fatmodel::Reads;
using fatmodel::drive;
using fatmodel::path;

void detach(BYTE pdrv) {
  f_mount(nullptr, path(pdrv, "").c_str(), 0);
  drive(pdrv).disk = nullptr;
  drive(pdrv).cache = nullptr;
  drive(pdrv).wrapper = nullptr;
  drive(pdrv).noInit = false;
  drive(pdrv).asked = Drive::Asked();
}

}  // namespace

void setUp() {}
// Also after a failed test: no drive left pointing at a test's disk.
void tearDown() {
  detach(0);
  detach(1);
}

namespace {

// ---- the cache is transparent under FatFs ----

struct Twin {
  RamDisk a{524288}, b{524288};  // 256 MB each (sparse)
  SectorCache cache;
  FATFS fsA, fsB;
};

std::string p0(const std::string& rel) { return path(0, rel); }
std::string p1(const std::string& rel) { return path(1, rel); }

void remount(Twin& t) {
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "0:", 0));
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "1:", 0));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &t.fsA));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(1, &t.fsB));
}

void coherence(uint32_t cacheEntries, uint32_t seed) {
  static Twin t;  // static: a failed assertion jumps past destructors (tearDown() detaches the drives)
  t.a = RamDisk(524288);
  t.b = RamDisk(524288);
  t.a.scrambleTrim = t.b.scrambleTrim = true;
  TEST_ASSERT_TRUE(t.cache.begin(cacheEntries));
  t.cache.resetStats();
  drive(0).disk = &t.a;
  drive(0).cache = &t.cache;
  drive(1).disk = &t.b;
  // FAT32 with 2 KB clusters: multi-sector reads and writes inside a cluster.
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, 2048));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(1, 2048));
  TEST_ASSERT_TRUE(t.a.same(t.b));
  remount(t);

  std::mt19937 rng(seed);
  auto pick = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  std::vector<std::string> dirs{""};
  std::vector<std::string> files;
  std::vector<uint8_t> wbuf(24 * 1024), ra(24 * 1024), rb(24 * 1024);
  uint32_t names = 0;
  auto newName = [&](bool dir) {
    char n[64];
    // Long names in mixed case (LFN entries), some 8.3 ones.
    if (pick(4) == 0) snprintf(n, sizeof(n), dir ? "D%u" : "F%u.BIN", names++);
    else snprintf(n, sizeof(n), dir ? "Folder number %u" : "A file called %u.mp3", names++);
    return std::string(n);
  };
  auto join = [](const std::string& d, const std::string& n) { return d.empty() ? n : d + "/" + n; };
  FIL fa, fb;
  const uint32_t kOps = 2500;
  for (uint32_t op = 0; op < kOps; ++op) {
    const uint32_t r = pick(100);
    if (r < 8 && dirs.size() < 40) {  // a folder
      const std::string d = join(dirs[pick(static_cast<uint32_t>(dirs.size()))], newName(true));
      const FRESULT x = f_mkdir(p0(d).c_str());
      TEST_ASSERT_EQUAL(x, f_mkdir(p1(d).c_str()));
      if (x == FR_OK) dirs.push_back(d);
    } else if (r < 28) {  // a new file, written in random chunks
      const std::string f = join(dirs[pick(static_cast<uint32_t>(dirs.size()))], newName(false));
      const FRESULT x = f_open(&fa, p0(f).c_str(), FA_CREATE_ALWAYS | FA_WRITE);
      TEST_ASSERT_EQUAL(x, f_open(&fb, p1(f).c_str(), FA_CREATE_ALWAYS | FA_WRITE));
      if (x != FR_OK) continue;
      uint32_t left = pick(20000);
      while (left) {
        const uint32_t n = std::min<uint32_t>(left, 1 + pick(5000));
        for (uint32_t i = 0; i < n; ++i) wbuf[i] = static_cast<uint8_t>(rng());
        UINT wa = 0, wb = 0;
        TEST_ASSERT_EQUAL(f_write(&fa, wbuf.data(), n, &wa), f_write(&fb, wbuf.data(), n, &wb));
        TEST_ASSERT_EQUAL_UINT32(wa, wb);
        left -= n;
      }
      TEST_ASSERT_EQUAL(f_close(&fa), f_close(&fb));
      if (std::find(files.begin(), files.end(), f) == files.end()) files.push_back(f);
    } else if (r < 40 && !files.empty()) {  // a write somewhere in a file (past its end too)
      const std::string f = files[pick(static_cast<uint32_t>(files.size()))];
      const FRESULT x = f_open(&fa, p0(f).c_str(), FA_OPEN_EXISTING | FA_WRITE | FA_READ);
      TEST_ASSERT_EQUAL(x, f_open(&fb, p1(f).c_str(), FA_OPEN_EXISTING | FA_WRITE | FA_READ));
      if (x != FR_OK) continue;
      const FSIZE_t at = pick(static_cast<uint32_t>(f_size(&fb)) + 4096);
      TEST_ASSERT_EQUAL(f_lseek(&fa, at), f_lseek(&fb, at));
      const uint32_t n = 1 + pick(6000);
      for (uint32_t i = 0; i < n; ++i) wbuf[i] = static_cast<uint8_t>(rng());
      UINT wa = 0, wb = 0;
      TEST_ASSERT_EQUAL(f_write(&fa, wbuf.data(), n, &wa), f_write(&fb, wbuf.data(), n, &wb));
      TEST_ASSERT_EQUAL_UINT32(wa, wb);
      if (pick(3) == 0) TEST_ASSERT_EQUAL(f_sync(&fa), f_sync(&fb));
      TEST_ASSERT_EQUAL(f_close(&fa), f_close(&fb));
    } else if (r < 70 && !files.empty()) {  // a read in random chunks: the same bytes
      const std::string f = files[pick(static_cast<uint32_t>(files.size()))];
      const FRESULT x = f_open(&fa, p0(f).c_str(), FA_READ);
      TEST_ASSERT_EQUAL(x, f_open(&fb, p1(f).c_str(), FA_READ));
      if (x != FR_OK) continue;
      const FSIZE_t at = pick(static_cast<uint32_t>(f_size(&fb)) + 1);
      TEST_ASSERT_EQUAL(f_lseek(&fa, at), f_lseek(&fb, at));
      for (int k = 0; k < 4; ++k) {
        const uint32_t n = 1 + pick(9000);
        UINT na = 0, nb = 0;
        TEST_ASSERT_EQUAL(f_read(&fa, ra.data(), n, &na), f_read(&fb, rb.data(), n, &nb));
        TEST_ASSERT_EQUAL_UINT32(nb, na);
        if (na) TEST_ASSERT_EQUAL_MEMORY(rb.data(), ra.data(), na);
      }
      TEST_ASSERT_EQUAL(f_close(&fa), f_close(&fb));
    } else if (r < 76 && !files.empty()) {  // a delete: its clusters TRIMmed
      const uint32_t k = pick(static_cast<uint32_t>(files.size()));
      TEST_ASSERT_EQUAL(f_unlink(p0(files[k]).c_str()), f_unlink(p1(files[k]).c_str()));
      files.erase(files.begin() + k);
      // As if the allocation had wrapped: the next clusters are the first
      // free ones, those just freed (and trimmed) among them.
      if (pick(2)) t.fsA.last_clst = t.fsB.last_clst = 2;
    } else if (r < 80 && !files.empty()) {  // a move into another folder
      const uint32_t k = pick(static_cast<uint32_t>(files.size()));
      const std::string to = join(dirs[pick(static_cast<uint32_t>(dirs.size()))], newName(false));
      const FRESULT x = f_rename(p0(files[k]).c_str(), p0(to).c_str());
      TEST_ASSERT_EQUAL(x, f_rename(p1(files[k]).c_str(), p1(to).c_str()));
      if (x == FR_OK) files[k] = to;
    } else if (r < 84 && !files.empty()) {  // a truncation
      const std::string f = files[pick(static_cast<uint32_t>(files.size()))];
      const FRESULT x = f_open(&fa, p0(f).c_str(), FA_OPEN_EXISTING | FA_WRITE);
      TEST_ASSERT_EQUAL(x, f_open(&fb, p1(f).c_str(), FA_OPEN_EXISTING | FA_WRITE));
      if (x != FR_OK) continue;
      const FSIZE_t at = pick(static_cast<uint32_t>(f_size(&fb)) + 1);
      TEST_ASSERT_EQUAL(f_lseek(&fa, at), f_lseek(&fb, at));
      TEST_ASSERT_EQUAL(f_truncate(&fa), f_truncate(&fb));
      TEST_ASSERT_EQUAL(f_close(&fa), f_close(&fb));
    } else if (r < 92) {  // a listing: the same entries
      const std::string d = dirs[pick(static_cast<uint32_t>(dirs.size()))];
      DIR da, db;
      FILINFO ia, ib;
      const FRESULT x = f_opendir(&da, p0(d).c_str());
      TEST_ASSERT_EQUAL(x, f_opendir(&db, p1(d).c_str()));
      if (x != FR_OK) continue;
      for (;;) {
        TEST_ASSERT_EQUAL(f_readdir(&da, &ia), f_readdir(&db, &ib));
        TEST_ASSERT_EQUAL_STRING(ib.fname, ia.fname);
        if (ib.fname[0] == 0) break;
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(ib.fsize), static_cast<uint32_t>(ia.fsize));
        TEST_ASSERT_EQUAL_UINT8(ib.fattrib, ia.fattrib);
      }
      f_closedir(&da);
      f_closedir(&db);
    } else if (r < 95) {  // a stat, of a file or of a name that isn't there
      const std::string dir = dirs[pick(static_cast<uint32_t>(dirs.size()))];
      const std::string f =
          !files.empty() && pick(2) ? files[pick(static_cast<uint32_t>(files.size()))] : join(dir, "none.mp3");
      FILINFO ia, ib;
      TEST_ASSERT_EQUAL(f_stat(p0(f).c_str(), &ia), f_stat(p1(f).c_str(), &ib));
    } else if (r < 97) {  // the cache dropped behind FatFs's back: always safe
      if (pick(2)) t.cache.invalidate(pick(t.a.sectors()), 1 + pick(4096));
      else t.cache.clear();
    } else {  // unmounted and mounted again (the firmware clears the cache)
      remount(t);
    }
  }
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "0:", 0));
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "1:", 0));
  // FatFs asked the same of both; the cards hold the same bytes.
  TEST_ASSERT_EQUAL_UINT64(drive(1).asked.reads, drive(0).asked.reads);
  TEST_ASSERT_EQUAL_UINT64(drive(1).asked.writes, drive(0).asked.writes);
  TEST_ASSERT_TRUE(drive(0).asked.trims > 0);
  TEST_ASSERT_TRUE(t.a.same(t.b));
  // ... and the run did exercise every path of the cache.
  const SectorCache::Stats& s = t.cache.stats();
  TEST_ASSERT_TRUE(s.hits > 1000);
  if (cacheEntries < 100) TEST_ASSERT_TRUE(s.evicted > 100);
  TEST_ASSERT_TRUE(s.bypassed > 100);
  TEST_ASSERT_TRUE(s.updated > 100);
  TEST_ASSERT_TRUE(s.dropped > 0);
  TEST_ASSERT_TRUE(t.a.counts.reads < drive(1).asked.reads);
  char msg[240];
  snprintf(msg, sizeof(msg),
           "%u sectors: FatFs asked %llu reads; %llu reached the card (hits %u, evicted %u, bypassed %u, dropped %u)",
           cacheEntries, static_cast<unsigned long long>(drive(0).asked.reads),
           static_cast<unsigned long long>(t.a.counts.reads), s.hits, s.evicted, s.bypassed, s.dropped);
  TEST_MESSAGE(msg);
  tearDown();
}

}  // namespace

void test_cache_transparent_under_fatfs() {
  coherence(24, 20261007);
  coherence(4096, 20261008);
}

// ---- a 20k tree in the user's shape ----

namespace {

// The card: /music/Artist/Album/NN - Title.ext, every folder and file
// created in the canonical order (a PC copies a folder in its names'
// order), each file one 32 KB cluster, FAT32 with 32 KB clusters on a
// 64 GB card (the research's figures).
struct ShapeCard {
  RamDisk disk{134217728u};
  FATFS fs;
  std::vector<std::string> files;  // relative to the root ("music/...")
  std::vector<std::string> folders;
};

std::string folderOf(const std::string& rel) { return rel.substr(0, rel.rfind('/')); }

void buildShape(ShapeCard& c, uint32_t tracks) {
  const synth::Spec spec = synth::userShape(tracks);
  char buf[320];
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    TEST_ASSERT_TRUE(synth::trackPath(spec, i, buf, sizeof(buf)));
    c.files.push_back(std::string(buf + 1));  // without the leading '/'
  }
  // The canonical order: a folder's name before what's inside it.
  auto key = [](std::string s) {
    for (char& ch : s)
      if (ch == '/') ch = '\x01';
    return s;
  };
  std::sort(c.files.begin(), c.files.end(),
            [&](const std::string& a, const std::string& b) { return key(a) < key(b); });
  drive(0).disk = &c.disk;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, 32768));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &c.fs));
  std::set<std::string> made;
  FIL f;
  for (const std::string& rel : c.files) {
    const std::string dir = folderOf(rel);
    for (size_t k = 0; !made.count(dir);) {  // "music", "music/A", "music/A/B"
      k = dir.find('/', k + 1);
      const std::string part = k == std::string::npos ? dir : dir.substr(0, k);
      if (made.count(part)) continue;
      TEST_ASSERT_EQUAL(FR_OK, f_mkdir(path(0, part).c_str()));
      made.insert(part);
      if (part != "music") c.folders.push_back(part);
    }
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, path(0, rel).c_str(), FA_CREATE_NEW | FA_WRITE));
    TEST_ASSERT_EQUAL(FR_OK, f_expand(&f, 4096, 1));
    TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  }
}

// Each name's entry slot in its folder (FatFs's own listing: after an item,
// dptr is past its SFN entry), and each folder's slots per cluster.
std::map<std::string, uint32_t> slots(const ShapeCard& c) {
  std::map<std::string, uint32_t> out;
  std::vector<std::string> dirs{"", "music"};
  dirs.insert(dirs.end(), c.folders.begin(), c.folders.end());
  DIR d;
  FILINFO fi;
  for (const std::string& dir : dirs) {
    TEST_ASSERT_EQUAL(FR_OK, f_opendir(&d, path(0, dir).c_str()));
    while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
      const uint32_t slot = d.dptr / 32 - 1;
      out[dir.empty() ? std::string(fi.fname) : dir + "/" + fi.fname] = slot;
    }
    f_closedir(&d);
  }
  return out;
}

// The research's model of a lookup (metascan 5.3), on this card's own
// layout: at each level the sectors up to the name's entry, plus a FAT read
// for each cluster the scan crosses into.
uint32_t modelReads(const std::map<std::string, uint32_t>& slot, const std::string& rel, uint32_t sectorsPerCluster) {
  uint32_t n = 0;
  for (size_t k = 0;;) {
    const size_t e = rel.find('/', k);
    const std::string upto = e == std::string::npos ? rel : rel.substr(0, e);
    const uint32_t sector = slot.at(upto) / 16;
    n += sector + 1 + sector / sectorsPerCluster;
    if (e == std::string::npos) break;
    k = e + 1;
  }
  return n;
}

struct Spread {
  double mean = 0;
  uint64_t p50 = 0, p90 = 0, max = 0;
};
Spread spread(std::vector<uint64_t> v) {
  Spread s;
  if (v.empty()) return s;
  std::sort(v.begin(), v.end());
  uint64_t sum = 0;
  for (uint64_t x : v) sum += x;
  s.mean = static_cast<double>(sum) / v.size();
  s.p50 = v[v.size() / 2];
  s.p90 = v[v.size() * 9 / 10];
  s.max = v.back();
  return s;
}

// Opens every file once (FA_READ, as the decoder and the scanner do), in
// `order`; the card reads per open.
std::vector<uint64_t> openAll(const ShapeCard& c, const std::vector<uint32_t>& order, std::vector<uint64_t>* asked) {
  std::vector<uint64_t> card;
  FIL f;
  Meter m(0);
  for (uint32_t i : order) {
    m.reset();
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, path(0, c.files[i]).c_str(), FA_READ));
    f_close(&f);
    const Reads r = m.read();
    card.push_back(r.card);
    if (asked) asked->push_back(r.asked);
  }
  return card;
}

}  // namespace

void test_user_shape_lookups_and_walk() {
  static ShapeCard c;  // static, as coherence()'s twin
  buildShape(c, 19519);
  const std::map<std::string, uint32_t> slot = slots(c);
  const uint32_t spc = c.fs.csize;
  TEST_ASSERT_EQUAL_UINT32(64, spc);
  uint32_t musicEntries = 0;
  const uint32_t musicSectors = fatmodel::folderSectors(0, "music", &musicEntries);
  char msg[300];
  snprintf(msg, sizeof(msg), "%u files, %u folders; /music: %u entries in %u sectors", (unsigned)c.files.size(),
           (unsigned)c.folders.size() + 1, musicEntries, musicSectors);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(musicSectors > 64);  // past one cluster, as the user's (102 sectors)

  std::vector<uint32_t> order(c.files.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::mt19937 rng(7);
  std::shuffle(order.begin(), order.end(), rng);

  // Uncached: what FatFs asks is the model's count, file for file.
  std::vector<uint64_t> asked;
  const std::vector<uint64_t> stock = openAll(c, order, &asked);
  uint64_t modelSum = 0;
  for (size_t k = 0; k < order.size(); ++k) {
    const uint32_t want = modelReads(slot, c.files[order[k]], spc);
    modelSum += want;
    if (asked[k] != want) {
      snprintf(msg, sizeof(msg), "%s: FatFs read %llu sectors, the model %u", c.files[order[k]].c_str(),
               (unsigned long long)asked[k], want);
      TEST_FAIL_MESSAGE(msg);
    }
    TEST_ASSERT_EQUAL_UINT64(asked[k], stock[k]);  // the stock driver: every call a card read
  }
  const Spread su = spread(stock);
  snprintf(msg, sizeof(msg), "a lookup, stock: %.1f sectors (p50 %llu, p90 %llu, max %llu)", su.mean,
           (unsigned long long)su.p50, (unsigned long long)su.p90, (unsigned long long)su.max);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(su.mean > 30 && su.mean < 80);  // the research's 56 at the user's 102-sector /music

  // Cached: 10,000 opens in another random order, the cache warm after
  // the first 2,000.
  SectorCache cache;
  for (uint32_t entries : {64u, 128u, 256u}) {
    TEST_ASSERT_TRUE(cache.begin(entries));
    drive(0).cache = &cache;
    std::shuffle(order.begin(), order.end(), rng);
    const std::vector<uint32_t> some(order.begin(), order.begin() + 10000);
    const std::vector<uint64_t> cached = openAll(c, some, nullptr);
    const Spread sc = spread(std::vector<uint64_t>(cached.begin() + 2000, cached.end()));
    snprintf(msg, sizeof(msg), "a lookup, %u-sector cache: %.2f card reads (p50 %llu, p90 %llu, max %llu)", entries,
             sc.mean, (unsigned long long)sc.p50, (unsigned long long)sc.p90, (unsigned long long)sc.max);
    TEST_MESSAGE(msg);
    // Smaller than /music (125 sectors here): each lookup pushes out what
    // the next needs. Just past it: mostly hits, a long tail. Twice it: the
    // folder's own sectors, about the floor for a random open.
    if (entries == 64) TEST_ASSERT_TRUE(sc.mean > su.mean / 2);
    if (entries == 128) TEST_ASSERT_TRUE(sc.mean < su.mean / 4);
    if (entries == 256) TEST_ASSERT_TRUE(sc.mean < 4 && sc.p90 <= 6);
    drive(0).cache = nullptr;
  }

  // The walk (3.2.3): stock, every folder's lookup and its own sectors;
  // cached (128, a cold cache: the boot's), each folder's sectors about
  // once.
  uint64_t ownSectors = 0;
  for (const std::string& d : c.folders) ownSectors += fatmodel::folderSectors(0, d);
  ownSectors += musicSectors;
  fatmodel::WalkStats ws;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::walkFolders(0, "music", &ws, [](const std::string&) {}));
  TEST_ASSERT_EQUAL_UINT64(c.files.size(), ws.files);
  TEST_ASSERT_EQUAL_UINT64(c.folders.size() + 1, ws.folders);
  TEST_ASSERT_TRUE(cache.begin(SectorCache::kDefaultEntries));
  drive(0).cache = &cache;
  cache.clear();
  fatmodel::WalkStats wc;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::walkFolders(0, "music", &wc, [](const std::string&) {}));
  drive(0).cache = nullptr;
  snprintf(msg, sizeof(msg), "the walk: %llu folders' own sectors; stock %llu reads, cached (%u) %llu card reads",
           (unsigned long long)ownSectors, (unsigned long long)ws.reads.card, SectorCache::kDefaultEntries,
           (unsigned long long)wc.reads.card);
  TEST_MESSAGE(msg);
  TEST_ASSERT_EQUAL_UINT64(ws.reads.asked, wc.reads.asked);
  TEST_ASSERT_TRUE(ws.reads.card > ownSectors * 10);  // the lookups dominate
  TEST_ASSERT_TRUE(wc.reads.card >= ownSectors);      // every sector once at least
  TEST_ASSERT_TRUE(wc.reads.card < ownSectors * 11 / 10);
}

namespace {

// A file of `n` bytes from `seed`, written whole.
void putFile(BYTE pdrv, const std::string& rel, uint32_t n, uint32_t seed) {
  std::vector<uint8_t> b(n);
  for (uint32_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(seed * 131u + i * 7u + (i >> 9));
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, path(pdrv, rel).c_str(), FA_CREATE_ALWAYS | FA_WRITE));
  UINT w = 0;
  TEST_ASSERT_EQUAL(FR_OK, f_write(&f, b.data(), n, &w));
  TEST_ASSERT_EQUAL_UINT32(n, w);
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
}

// The file's bytes are putFile()'s.
void checkFile(BYTE pdrv, const std::string& rel, uint32_t n, uint32_t seed) {
  FIL f;
  TEST_ASSERT_EQUAL(FR_OK, f_open(&f, path(pdrv, rel).c_str(), FA_READ));
  TEST_ASSERT_EQUAL_UINT32(n, static_cast<uint32_t>(f_size(&f)));
  std::vector<uint8_t> b(n);
  UINT r = 0;
  TEST_ASSERT_EQUAL(FR_OK, f_read(&f, b.data(), n, &r));
  TEST_ASSERT_EQUAL_UINT32(n, r);
  for (uint32_t i = 0; i < n; ++i) {
    if (b[i] != static_cast<uint8_t>(seed * 131u + i * 7u + (i >> 9))) TEST_FAIL_MESSAGE(rel.c_str());
  }
  TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
}

// Two cards formatted alike (the same boot sector: the model's clock is
// fixed), their folders and FATs not: one with an artist's album, the
// other with another's and more files.
void fillCard(BYTE pdrv, bool second) {
  TEST_ASSERT_EQUAL(FR_OK, f_mkdir(path(pdrv, "music").c_str()));
  if (!second) {
    TEST_ASSERT_EQUAL(FR_OK, f_mkdir(path(pdrv, "music/First Artist").c_str()));
    for (uint32_t k = 0; k < 6; ++k) putFile(pdrv, "music/First Artist/0" + std::to_string(k) + " - a.mp3", 9000 + k, k);
  } else {
    TEST_ASSERT_EQUAL(FR_OK, f_mkdir(path(pdrv, "music/Second Artist").c_str()));
    TEST_ASSERT_EQUAL(FR_OK, f_mkdir(path(pdrv, "music/Third Artist").c_str()));
    for (uint32_t k = 0; k < 9; ++k) putFile(pdrv, "music/Second Artist/0" + std::to_string(k) + " - b.mp3", 7000 + k, 50 + k);
    putFile(pdrv, "music/Third Artist/01 - c.mp3", 20000, 99);
  }
}

}  // namespace

void test_card_swapped_under_the_wrapper() {
  static RamDisk a(524288), b(524288), ref(1);  // 256 MB each (sparse); static: see coherence()
  a = RamDisk(524288);
  b = RamDisk(524288);
  // The second card, made on drive 1 (no cache), and a copy of it for the
  // reference.
  drive(1).disk = &b;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(1, 2048));
  FATFS fs1;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(1, &fs1));
  fillCard(1, true);
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "1:", 0));
  ref = b;
  // The first card under the wrapper, as the firmware mounts it.
  static SectorCache cache;
  TEST_ASSERT_TRUE(cache.begin(SectorCache::kDefaultEntries));
  fatmodel::Slot slot(0);
  CachedDrive wrapper(cache, slot);
  drive(0).disk = &a;
  drive(0).wrapper = &wrapper;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, 2048));
  FATFS fs0;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &fs0));
  fillCard(0, false);
  for (uint32_t k = 0; k < 6; ++k) checkFile(0, "music/First Artist/0" + std::to_string(k) + " - a.mp3", 9000 + k, k);
  TEST_ASSERT_TRUE(cache.size() > 4);  // its boot sector, FAT and folders
  const uint32_t inits = wrapper.stats().inits;

  // The card pulled and the other put in, the player on: the SD driver's
  // status says not initialised, and FatFs's next call mounts again.
  drive(0).disk = &b;
  drive(0).noInit = true;
  FILINFO fi;
  TEST_ASSERT_EQUAL(FR_OK, f_stat(path(0, "music/Second Artist/03 - b.mp3").c_str(), &fi));
  TEST_ASSERT_EQUAL_UINT32(inits + 1, wrapper.stats().inits);
  TEST_ASSERT_EQUAL_UINT32(7003, static_cast<uint32_t>(fi.fsize));
  TEST_ASSERT_EQUAL(FR_NO_PATH, f_stat(path(0, "music/First Artist/00 - a.mp3").c_str(), &fi));
  for (uint32_t k = 0; k < 9; ++k) checkFile(0, "music/Second Artist/0" + std::to_string(k) + " - b.mp3", 7000 + k, 50 + k);
  // A write there (a queue save, a journal append): the image FatFs
  // without the cache leaves on the same card.
  putFile(0, "music/Second Artist/queue.tmp", 3000, 7);
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(path(0, "music/Third Artist/01 - c.mp3").c_str()));
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "0:", 0));
  drive(1).disk = &ref;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(1, &fs1));
  putFile(1, "music/Second Artist/queue.tmp", 3000, 7);
  TEST_ASSERT_EQUAL(FR_OK, f_unlink(path(1, "music/Third Artist/01 - c.mp3").c_str()));
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "1:", 0));
  TEST_ASSERT_TRUE(b.same(ref));

  // And back: the first card again, as it was.
  drive(0).disk = &a;
  drive(0).noInit = true;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &fs0));
  for (uint32_t k = 0; k < 6; ++k) checkFile(0, "music/First Artist/0" + std::to_string(k) + " - a.mp3", 9000 + k, k);
  TEST_ASSERT_EQUAL(FR_NO_PATH, f_stat(path(0, "music/Second Artist/03 - b.mp3").c_str(), &fi));
  tearDown();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cache_transparent_under_fatfs);
  RUN_TEST(test_user_shape_lookups_and_walk);
  RUN_TEST(test_card_swapped_under_the_wrapper);
  return UNITY_END();
}
