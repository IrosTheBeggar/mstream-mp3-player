// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for CardMkfs (docs/HOST-STATUS.md "@format"; docs/METADATA.md
// 2.2, "A card the player formats"): the layout a card of each size gets
// (the cluster size, the 4 MiB boundaries, the refusals), checked against
// what ChaN's FatFs R0.15 itself makes of the same options on the host
// FatFs model (as the Core2 builds it: test/support/FatModel.h, with
// ESP-IDF's FF_MULTI_PARTITION and VolToPart[]); the MBR's bytes; the
// whole format on RAM disks (blank, an exFAT, a GPT and an old FAT32
// card): what FatFs mounts after it, FSINFO's count against a count of
// the FAT, /music and device.txt, and nothing left of the old card's
// schemes; a format cut at any write never leaving a card FatFs takes for
// a FAT volume; the refusals of each step; the progress; the request
// carried across the restart.
// Run: pio test -e native -f test_card_mkfs
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "CachedDrive.h"
#include "CardContract.h"
#include "CardFormat.h"
#include "CardMkfs.h"
#include "HostStatus.h"

namespace {

using cardmkfs::Layout;
using cardmkfs::Why;
constexpr uint32_t kSS = 512;
constexpr uint64_t kMiB = 1024 * 1024 / kSS;  // sectors
constexpr uint64_t kGB = 1000000000ull / kSS;  // a card's "GB" in sectors

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

// A card that refuses every write from its `failAt`th sector on (0: none),
// and counts what it was asked.
class Card : public SectorCache::Device {
public:
  explicit Card(fatmodel::RamDisk& d) : d_(d) {}
  bool read(uint32_t lba, uint8_t* out, uint32_t n) override { return d_.read(lba, out, n); }
  bool write(uint32_t lba, const uint8_t* data, uint32_t n) override {
    if (refused && n == 1 && lba == refused) return false;
    if (failAt && sectors + n >= failAt) {
      // The sectors before the cut reach the card (one command's blocks go
      // one by one).
      const uint64_t before = failAt > sectors + 1 ? failAt - sectors - 1 : 0;
      if (before) d_.write(lba, data, static_cast<uint32_t>(before));
      sectors += before;
      return false;
    }
    sectors += n;
    return d_.write(lba, data, n);
  }
  uint64_t failAt = 0;
  uint64_t sectors = 0;
  uint32_t refused = 0;  // a single-sector write to it is refused (0: none)

private:
  fatmodel::RamDisk& d_;
};

// f_mkfs as the device calls it (storage/CardFormatter): drive 0's
// partition set to 1 for the call, the work area 64 KB.
int modelMkfs(const cardmkfs::Mkfs& o, void* ctx) {
  (void)ctx;
  MKFS_PARM opt = {o.fmt, o.fats, o.align, o.rootEntries, o.clusterBytes};
  std::vector<uint8_t> work(64 * 1024);
  VolToPart[0].pt = 1;
  const FRESULT r = f_mkfs("0:", &opt, work.data(), static_cast<UINT>(work.size()));
  VolToPart[0].pt = 0;
  return r;
}

// The same with other options than the plan's (a check that must fail).
int wrongMkfs(const cardmkfs::Mkfs& o, void* ctx) {
  cardmkfs::Mkfs other = o;
  other.clusterBytes = o.clusterBytes / 2;
  return modelMkfs(other, ctx);
}

int refusingMkfs(const cardmkfs::Mkfs&, void*) { return FR_MKFS_ABORTED; }

// f_mkfs, then FSINFO's count one off (another FatFs's arithmetic, say):
// `ctx` the disk.
int miscountingMkfs(const cardmkfs::Mkfs& o, void* ctx) {
  const int r = modelMkfs(o, nullptr);
  auto* d = static_cast<fatmodel::RamDisk*>(ctx);
  uint8_t s[kSS];
  for (uint32_t at : {cardmkfs::kAlignSectors + 1, cardmkfs::kAlignSectors + 7}) {
    d->read(at, s, 1);
    s[488] = static_cast<uint8_t>(s[488] - 1);
    d->write(at, s, 1);
  }
  return r;
}

// cardmkfs::Files over the model's FatFs, as storage/CardFormatter's.
// `failMount`: the card stops answering for the mount (after the check).
class ModelFiles : public cardmkfs::Files {
public:
  bool mount() override {
    ++mounts;
    fatmodel::RamDisk* d = fatmodel::drive(0).disk;
    if (failMount) fatmodel::drive(0).disk = nullptr;
    const bool ok = f_mount(&fs, "0:", 1) == FR_OK;
    fatmodel::drive(0).disk = d;
    return ok;
  }
  bool mkdir(const char* path) override {
    if (failMkdir) return false;
    const FRESULT r = f_mkdir(fatmodel::path(0, path + 1).c_str());
    return r == FR_OK || r == FR_EXIST;
  }
  bool write(const char* path, const void* data, uint32_t n) override {
    FIL f;
    if (f_open(&f, fatmodel::path(0, path + 1).c_str(), FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return false;
    UINT put = 0;
    const bool ok = f_write(&f, data, n, &put) == FR_OK && put == n;
    return f_close(&f) == FR_OK && ok;
  }
  void unmount() override {
    ++unmounts;
    f_mount(nullptr, "0:", 0);
  }
  FATFS fs;
  bool failMkdir = false;
  bool failMount = false;
  int mounts = 0, unmounts = 0;
};

std::string deviceTxt() {
  char t[400];
  const size_t n =
      cardcontract::devicetxt::format(cardcontract::devicetxt::current("0.12.0"), t, sizeof(t));
  return std::string(t, n);
}

struct Formatted {
  cardmkfs::Result r;
  uint64_t written = 0;
  uint32_t calls = 0;     // progress callbacks
  bool monotonic = true;  // each callback's written above the last
  uint64_t lastTotal = 0;
};

void onWrite(uint64_t written, uint64_t total, void* ctx) {
  Formatted* f = static_cast<Formatted*>(ctx);
  if (written <= f->written && f->calls) f->monotonic = false;
  f->written = written;
  f->lastTotal = total;
  ++f->calls;
}

// The whole format of `disk` (through `card`, which may cut it), the
// writer in front of drive 0 as the device's disk functions put it.
Formatted format(fatmodel::RamDisk& disk, Card& card, uint32_t signature = 0x5eed1234u,
                 cardmkfs::MkfsFn mkfs = modelMkfs, ModelFiles* files = nullptr, void* mkfsCtx = nullptr) {
  const Layout l = cardmkfs::plan(disk.sectors());
  TEST_ASSERT_TRUE(l.ok());
  cardmkfs::Writer w(card, l);
  Formatted f;
  w.onWrite(onWrite, &f);
  fatmodel::drive(0).disk = &disk;
  fatmodel::drive(0).front = &w;
  ModelFiles own;
  const std::string text = deviceTxt();
  std::vector<uint8_t> buf(64 * kSS);
  f.r = cardmkfs::run(w, signature, buf.data(), 64, mkfs, mkfsCtx, files ? *files : own, text.data(),
                      static_cast<uint32_t>(text.size()));
  f.written = w.written();
  fatmodel::drive(0).front = nullptr;
  return f;
}

// What FatFs made of the options, read from the card: the boot sector's
// fields as a Layout, and FSINFO's free count.
struct Made {
  Layout l;
  uint32_t fsinfo = 0;
};
Made madeBy(fatmodel::RamDisk& d, uint32_t partStart) {
  uint8_t s[kSS];
  TEST_ASSERT_TRUE(d.read(partStart, s, 1));
  Made made;
  Layout& m = made.l;
  m.partStart = le32(s + 28);
  m.partSectors = le32(s + 32);
  m.clusterSectors = s[13];
  m.reserved = le16(s + 14);
  m.fatSectors = le32(s + 36);
  m.dataStart = m.partStart + m.reserved + m.fatSectors * s[16];
  m.clusters = (m.partSectors - m.reserved - m.fatSectors * s[16]) / m.clusterSectors;
  TEST_ASSERT_TRUE(d.read(partStart + 1, s, 1));
  made.fsinfo = le32(s + 488);
  return made;
}

void samePlan(const Layout& l, const Made& made, const char* what) {
  const Layout& m = made.l;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.partStart, m.partStart, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.partSectors, m.partSectors, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.clusterSectors, m.clusterSectors, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.reserved, m.reserved, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.fatSectors, m.fatSectors, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.dataStart, m.dataStart, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.clusters, m.clusters, what);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.freeClusters(), made.fsinfo, what);
}

// f_getfree() counting the whole FAT (FSINFO's count set aside).
uint32_t countedFree(FATFS* fs) {
  fs->free_clst = 0xFFFFFFFF;
  DWORD n = 0;
  FATFS* got = nullptr;
  TEST_ASSERT_EQUAL(FR_OK, f_getfree("0:", &n, &got));
  return n;
}

bool readSector(uint32_t lba, uint8_t* out, void* ctx) {
  return static_cast<fatmodel::RamDisk*>(ctx)->read(lba, out, 1);
}

bool zero(fatmodel::RamDisk& d, uint32_t lba) {
  uint8_t s[kSS];
  TEST_ASSERT_TRUE(d.read(lba, s, 1));
  for (uint32_t i = 0; i < kSS; ++i) {
    if (s[i]) return false;
  }
  return true;
}

// An old card's sectors: an exFAT superfloppy, or a GPT (its protective
// MBR, its header and table, their backups at the end).
void exfatCard(fatmodel::RamDisk& d) {
  uint8_t s[kSS] = {};
  s[0] = 0xEB;
  std::memcpy(s + 3, "EXFAT   ", 8);
  s[510] = 0x55;
  s[511] = 0xAA;
  d.write(0, s, 1);
  std::memset(s, 0x5A, kSS);
  d.write(12, s, 1);  // its FAT, say
}
void gptCard(fatmodel::RamDisk& d) {
  uint8_t s[kSS] = {};
  s[446 + 4] = 0xEE;
  s[446 + 8] = 1;
  s[510] = 0x55;
  s[511] = 0xAA;
  d.write(0, s, 1);
  std::memset(s, 0, kSS);
  std::memcpy(s, "EFI PART", 8);
  d.write(1, s, 1);
  d.write(d.sectors() - 1, s, 1);
  std::memset(s, 0x77, kSS);
  for (uint32_t k = 2; k < 34; ++k) d.write(k, s, 1);
  for (uint32_t k = d.sectors() - 33; k < d.sectors() - 1; ++k) d.write(k, s, 1);
}

}  // namespace

void setUp() {}
void tearDown() {
  f_mount(nullptr, "0:", 0);
  fatmodel::drive(0).disk = nullptr;
  fatmodel::drive(0).front = nullptr;
  VolToPart[0].pt = 0;
}

// ---- the layout ----

// The cluster size by the card's size: 32 KB from about 2.2 GB up (every
// card sold as 4 GB or more), the largest that keeps FAT32's 65,526
// clusters below that, 512 B down to about 40 MB; the partition and the
// data area on 4 MiB boundaries; the refusals.
void test_cluster_sizes() {
  struct Case {
    uint64_t sectors;
    uint32_t clusterBytes;  // 0: refused
    Why why;
  } cases[] = {
      {0, 0, Why::NoSize},
      {8192, 0, Why::TooSmall},
      {36 * kMiB, 0, Why::TooSmall},
      {40 * kMiB, 512, Why::Ok},
      {64 * kMiB, 512, Why::Ok},
      {128 * kMiB, 1024, Why::Ok},
      {256 * kMiB, 2048, Why::Ok},
      {512 * kMiB, 4096, Why::Ok},
      {1024 * kMiB, 8192, Why::Ok},
      {2048 * kMiB, 16384, Why::Ok},
      {4 * kGB - 3 * kMiB, 32768, Why::Ok},  // a "4 GB" card: 3,8xx MiB
      {7744512, 32768, Why::Ok},             // an SDHC card's CSD, "4 GB"
      {62333952, 32768, Why::Ok},            // "32 GB"
      {124735488, 32768, Why::Ok},           // "64 GB"
      {1953525168ull, 32768, Why::Ok},       // "1 TB"
      {3906250000ull, 32768, Why::Ok},       // "2 TB" (SDXC's most)
      {0xFFFFFFFFull, 32768, Why::Ok},       // FatFs's last sector number
      {0x100000000ull, 0, Why::TooBig},
  };
  for (const Case& c : cases) {
    const Layout l = cardmkfs::plan(c.sectors);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "%llu sectors", static_cast<unsigned long long>(c.sectors));
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(c.why), static_cast<int>(l.why), msg);
    if (!l.ok()) continue;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.clusterBytes, l.clusterBytes(), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(cardmkfs::kAlignSectors, l.partStart, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.sectors - cardmkfs::kAlignSectors, l.partSectors, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, l.dataStart % cardmkfs::kAlignSectors, msg);
    TEST_ASSERT_TRUE_MESSAGE(l.clusters >= cardmkfs::kMinClusters && l.clusters <= cardmkfs::kMaxClusters, msg);
    TEST_ASSERT_TRUE_MESSAGE(l.reserved >= 32 && l.reserved < 65536, msg);  // a 16-bit field
    // The FAT holds every cluster's entry, and the volume holds the FATs,
    // the clusters and nothing to spare beyond one cluster.
    TEST_ASSERT_TRUE_MESSAGE(static_cast<uint64_t>(l.fatSectors) * kSS / 4 >= l.clusters + 2ull, msg);
    const uint64_t used = l.reserved + 2ull * l.fatSectors + static_cast<uint64_t>(l.clusters) * l.clusterSectors;
    TEST_ASSERT_TRUE_MESSAGE(used <= l.partSectors && l.partSectors - used < l.clusterSectors, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(cardmkfs::kTailSectors, l.tail, msg);
    TEST_ASSERT_TRUE_MESSAGE(l.cardSectors - l.tail >= l.dataStart + l.clusterSectors, msg);
  }
  // Where 32 KB begins, and FAT32's smallest card: found by plan() and
  // said in the docs ("about 2.2 GB", "about 40 MB").
  uint64_t lo = 2048 * kMiB, hi = 4 * kGB;
  while (hi - lo > 1) {
    const uint64_t mid = (lo + hi) / 2;
    (cardmkfs::plan(mid).clusterBytes() == 32768 ? hi : lo) = mid;
  }
  uint64_t small = 0;
  for (uint64_t s = 36 * kMiB; s <= 40 * kMiB && !small; ++s) {
    if (cardmkfs::plan(s).ok()) small = s;
  }
  char msg[128];
  std::snprintf(msg, sizeof(msg), "32 KB clusters from %llu sectors (%.2f GB); FAT32 from %llu sectors (%.1f MB)",
                static_cast<unsigned long long>(hi), hi * kSS / 1e9, static_cast<unsigned long long>(small),
                small * kSS / 1e6);
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE(hi * kSS > 2.1e9 && hi * kSS < 2.2e9);
  TEST_ASSERT_TRUE(small * kSS > 37e6 && small * kSS < 42e6);
  TEST_ASSERT_TRUE(cardmkfs::plan(small - 1).why == Why::TooSmall);
  TEST_ASSERT_EQUAL_STRING("size", cardmkfs::detail(Why::TooSmall));
  TEST_ASSERT_EQUAL_STRING("size", cardmkfs::detail(Why::TooBig));
  TEST_ASSERT_EQUAL_STRING("size", cardmkfs::detail(Why::NoSize));
}

// The plan is f_mkfs's own arithmetic: FatFs given the plan's options
// (in the partition the MBR names) makes exactly that layout, for cards at
// each cluster size's edges and the biggest; and refuses a card the plan
// calls too small.
void test_plan_is_fmkfs() {
  const uint64_t sizes[] = {40 * kMiB,  41 * kMiB + 77, 64 * kMiB,       128 * kMiB + 5, 300 * kMiB + 1,
                            511 * kMiB, 1024 * kMiB + 3, 2100 * kMiB,    2200 * kMiB,    7744512,
                            62333952,   124735488,        1953525168ull, 0xFFFFFFFFull};
  for (const uint64_t n : sizes) {
    fatmodel::RamDisk disk(static_cast<uint32_t>(n));
    const Layout l = cardmkfs::plan(n);
    TEST_ASSERT_TRUE(l.ok());
    uint8_t s[kSS];
    cardmkfs::mbr(l, 1, s);
    TEST_ASSERT_TRUE(disk.write(0, s, 1));
    fatmodel::drive(0).disk = &disk;
    TEST_ASSERT_EQUAL(FR_OK, modelMkfs(cardmkfs::mkfsOptions(l), nullptr));
    char msg[48];
    std::snprintf(msg, sizeof(msg), "%llu sectors", static_cast<unsigned long long>(n));
    samePlan(l, madeBy(disk, l.partStart), msg);
    // What f_mkfs writes, as the plan counts it (prepare()'s aside).
    const uint64_t prep = cardmkfs::kHeadSectors + 1 + (l.partStart - 1) + l.tail;
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(l.writeSectors - prep, disk.counts.writeSectors - 1, msg);
  }
  // One sector short of FAT32's smallest: refused by both.
  uint64_t small = 36 * kMiB;
  while (!cardmkfs::plan(small).ok()) ++small;
  fatmodel::RamDisk disk(static_cast<uint32_t>(small - 1));
  Layout l = cardmkfs::plan(small);
  l.partSectors -= 1;
  uint8_t s[kSS];
  cardmkfs::mbr(l, 1, s);
  disk.write(0, s, 1);
  fatmodel::drive(0).disk = &disk;
  cardmkfs::Mkfs o = cardmkfs::mkfsOptions(l);
  TEST_ASSERT_EQUAL(FR_MKFS_ABORTED, modelMkfs(o, nullptr));
}

// Sector 0: the disk signature, the one partition (not active, 0x0C, its
// start and size, CHS as partitioners write it), nothing else.
void test_mbr() {
  const Layout l = cardmkfs::plan(124735488);  // "64 GB"
  uint8_t s[kSS];
  std::memset(s, 0xCC, kSS);
  cardmkfs::mbr(l, 0xA1B2C3D4u, s);
  for (uint32_t i = 0; i < 440; ++i) TEST_ASSERT_EQUAL_UINT8(0, s[i]);  // no boot code
  TEST_ASSERT_EQUAL_HEX32(0xA1B2C3D4u, le32(s + 440));
  TEST_ASSERT_EQUAL_UINT16(0, le16(s + 444));
  const uint8_t* e = s + 446;
  TEST_ASSERT_EQUAL_HEX8(0x00, e[0]);
  // 8192 = cylinder 0, head 130, sector 3 (8192 / 63 = 130 r 2).
  TEST_ASSERT_EQUAL_HEX8(130, e[1]);
  TEST_ASSERT_EQUAL_HEX8(3, e[2]);
  TEST_ASSERT_EQUAL_HEX8(0, e[3]);
  TEST_ASSERT_EQUAL_HEX8(0x0C, e[4]);
  TEST_ASSERT_EQUAL_HEX8(0xFE, e[5]);  // its end: past cylinder 1023
  TEST_ASSERT_EQUAL_HEX8(0xFF, e[6]);
  TEST_ASSERT_EQUAL_HEX8(0xFF, e[7]);
  TEST_ASSERT_EQUAL_UINT32(8192, le32(e + 8));
  TEST_ASSERT_EQUAL_UINT32(124735488 - 8192, le32(e + 12));
  for (uint32_t i = 446 + 16; i < 510; ++i) TEST_ASSERT_EQUAL_UINT8(0, s[i]);
  TEST_ASSERT_EQUAL_HEX8(0x55, s[510]);
  TEST_ASSERT_EQUAL_HEX8(0xAA, s[511]);
  // A small card's end inside CHS: cylinder 4, head 254, sector 63 at
  // 81,919 (= (4 * 255 + 254) * 63 + 62)... the last sector of 40 MiB.
  const Layout small = cardmkfs::plan(40 * kMiB);
  cardmkfs::mbr(small, 1, s);
  const uint32_t last = 40 * kMiB - 1;
  const uint32_t c = last / (255 * 63), h = last / 63 % 255, sec = last % 63 + 1;
  TEST_ASSERT_EQUAL_HEX8(h, s[446 + 5]);
  TEST_ASSERT_EQUAL_HEX8(((c >> 2) & 0xC0) | sec, s[446 + 6]);
  TEST_ASSERT_EQUAL_HEX8(c & 0xFF, s[446 + 7]);
}

// ---- the format, whole ----

// A blank card formatted: FatFs (looking for its volume as the boot's
// mount does, partition 0: automatic) mounts FAT32 in the partition, with
// the plan's layout; FSINFO's count valid and a true count of the FAT,
// the three clusters /music, /.player and device.txt took aside; /music
// empty; device.txt the firmware's; the check passed; the progress went
// up to the plan's total and stopped there.
void test_blank_card() {
  for (const uint64_t n : {uint64_t{41 * kMiB}, uint64_t{7744512}, uint64_t{124735488}}) {
    fatmodel::RamDisk disk(static_cast<uint32_t>(n));
    Card card(disk);
    const Layout l = cardmkfs::plan(n);
    const Formatted f = format(disk, card);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Ok), static_cast<int>(f.r.why));
    TEST_ASSERT_EQUAL_UINT64(l.writeSectors, f.written);
    TEST_ASSERT_EQUAL_UINT64(l.writeSectors, f.lastTotal);
    TEST_ASSERT_TRUE(f.monotonic && f.calls > 3);
    fatmodel::drive(0).disk = &disk;
    FATFS fs;
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&fs, "0:", 1));
    TEST_ASSERT_EQUAL(FS_FAT32, fs.fs_type);
    TEST_ASSERT_EQUAL_UINT32(l.partStart, fs.volbase);
    TEST_ASSERT_EQUAL_UINT32(l.fatStart(), fs.fatbase);
    TEST_ASSERT_EQUAL_UINT32(l.dataStart, fs.database);
    TEST_ASSERT_EQUAL_UINT32(l.clusterSectors, fs.csize);
    TEST_ASSERT_EQUAL_UINT32(l.clusters, fs.n_fatent - 2);
    TEST_ASSERT_EQUAL_UINT8(2, fs.n_fats);
    // FSINFO's count, as the mount read it: valid, and the FAT's own.
    const uint32_t fsinfo = fs.free_clst;
    TEST_ASSERT_TRUE(hoststatus::freeCountValid(fsinfo, fs.n_fatent));
    TEST_ASSERT_EQUAL_UINT32(l.freeClusters() - 3, fsinfo);
    TEST_ASSERT_EQUAL_UINT32(fsinfo, countedFree(&fs));
    // The tree: /music (empty), /.player/device.txt, nothing else.
    DIR dir;
    FILINFO fi;
    std::vector<std::string> names;
    TEST_ASSERT_EQUAL(FR_OK, f_opendir(&dir, "0:/"));
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) names.push_back(fi.fname);
    f_closedir(&dir);
    TEST_ASSERT_EQUAL(2, static_cast<int>(names.size()));
    TEST_ASSERT_EQUAL_STRING("music", names[0].c_str());
    TEST_ASSERT_EQUAL_STRING(".player", names[1].c_str());
    TEST_ASSERT_EQUAL(FR_OK, f_opendir(&dir, "0:/music"));
    TEST_ASSERT_EQUAL(FR_OK, f_readdir(&dir, &fi));
    TEST_ASSERT_EQUAL_UINT8(0, fi.fname[0]);
    f_closedir(&dir);
    FIL file;
    TEST_ASSERT_EQUAL(FR_OK, f_open(&file, "0:/.player/device.txt", FA_READ));
    char text[400];
    UINT got = 0;
    TEST_ASSERT_EQUAL(FR_OK, f_read(&file, text, sizeof(text), &got));
    f_close(&file);
    const std::string want = deviceTxt();
    TEST_ASSERT_EQUAL(static_cast<int>(want.size()), static_cast<int>(got));
    TEST_ASSERT_EQUAL_MEMORY(want.data(), text, got);
    cardcontract::devicetxt::Info info;
    TEST_ASSERT_TRUE(cardcontract::devicetxt::parse(text, got, &info));
    f_mount(nullptr, "0:", 0);
    // The card's first sectors: not a scheme the player refuses.
    uint8_t sector[kSS];
    TEST_ASSERT_TRUE(cardformat::classify(readSector, &disk, sector) == cardformat::Kind::Other);
    TEST_ASSERT_FALSE(cardformat::notFat32(cardformat::classify(readSector, &disk, sector)));
  }
}

// An old card: an exFAT superfloppy, a GPT card (its backup at the card's
// end) and a FAT32 card with files (formatted with FatFs's own MBR at 63):
// after the format nothing of their schemes is left (sector 0 is the MBR,
// sectors 1 to the partition and the card's last 64 zero), FatFs mounts
// the new volume (the old files gone), and the card's identity (what the
// player's guard holds a card to) is another card's.
void test_old_cards() {
  const uint32_t n = 7744512;
  for (int kind = 0; kind < 3; ++kind) {
    fatmodel::RamDisk disk(n);
    if (kind == 0) exfatCard(disk);
    if (kind == 1) gptCard(disk);
    if (kind == 2) {
      fatmodel::drive(0).disk = &disk;
      TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, 16384));
      FATFS fs;
      TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &fs));
      TEST_ASSERT_EQUAL(FR_OK, f_mkdir("0:/music"));
      FIL f;
      TEST_ASSERT_EQUAL(FR_OK, f_open(&f, "0:/music/old.mp3", FA_WRITE | FA_CREATE_ALWAYS));
      std::vector<uint8_t> junk(100000, 0x42);
      UINT put = 0;
      f_write(&f, junk.data(), static_cast<UINT>(junk.size()), &put);
      f_close(&f);
      f_mount(nullptr, "0:", 0);
    }
    std::vector<uint8_t> scratch(kSS);
    const CachedDrive::Identity before = CachedDrive::identityOf(disk, n, scratch.data());
    uint8_t s[kSS];
    if (kind == 1) TEST_ASSERT_TRUE(cardformat::classify(readSector, &disk, s) == cardformat::Kind::Gpt);
    if (kind == 0) TEST_ASSERT_TRUE(cardformat::classify(readSector, &disk, s) == cardformat::Kind::ExFat);
    Card card(disk);
    const Formatted f = format(disk, card);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Ok), static_cast<int>(f.r.why));
    for (uint32_t lba = 1; lba < cardmkfs::kAlignSectors; lba += 97) TEST_ASSERT_TRUE(zero(disk, lba));
    for (uint32_t lba = 1; lba < 40; ++lba) TEST_ASSERT_TRUE(zero(disk, lba));
    for (uint32_t lba = n - cardmkfs::kTailSectors; lba < n; ++lba) TEST_ASSERT_TRUE(zero(disk, lba));
    TEST_ASSERT_FALSE(cardformat::notFat32(cardformat::classify(readSector, &disk, s)));
    fatmodel::drive(0).disk = &disk;
    FATFS fs;
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&fs, "0:", 1));
    TEST_ASSERT_EQUAL_UINT32(cardmkfs::kAlignSectors, fs.volbase);
    FILINFO fi;
    TEST_ASSERT_EQUAL(FR_NO_FILE, f_stat("0:/music/old.mp3", &fi));
    TEST_ASSERT_EQUAL_UINT32(cardmkfs::plan(n).freeClusters() - 3, countedFree(&fs));
    f_mount(nullptr, "0:", 0);
    const CachedDrive::Identity after = CachedDrive::identityOf(disk, n, scratch.data());
    TEST_ASSERT_TRUE(before.valid && after.valid);
    TEST_ASSERT_FALSE(before.sameCard(after));
    TEST_ASSERT_EQUAL_UINT32(cardmkfs::kAlignSectors, after.bootLba);
  }
  // The same card formatted again: another disk signature, so another
  // card to the guard (on the device the boot sector's serial is the
  // clock's too).
  fatmodel::RamDisk disk(n);
  Card card(disk);
  TEST_ASSERT_TRUE(format(disk, card, 1).r.why == Why::Ok);
  std::vector<uint8_t> scratch(kSS);
  const CachedDrive::Identity one = CachedDrive::identityOf(disk, n, scratch.data());
  TEST_ASSERT_TRUE(format(disk, card, 2).r.why == Why::Ok);
  TEST_ASSERT_FALSE(one.sameCard(CachedDrive::identityOf(disk, n, scratch.data())));
}

// Cut at any write: from the new MBR (the 9th sector written) until the
// boot sector, the format's last write, the card holds no FAT volume
// FatFs would mount (FR_NO_FILESYSTEM: the player's "can't read this
// card", and its Format), never a FAT32 whose FAT is part the old card's;
// and formatted again, it is whole. (Cut before the new MBR, an old
// card's own volume is still there, the sectors at 8192 already zeroed:
// it was being erased.)
void test_cut_anywhere() {
  const uint32_t n = 7744512;
  const Layout l = cardmkfs::plan(n);
  const uint64_t total = l.writeSectors;
  const uint64_t prep = cardmkfs::kHeadSectors + 1 + (l.partStart - 1) + l.tail;
  const uint64_t cuts[] = {1,         8,         9,           10,           5000,
                           prep,      prep + 1,  prep + 2,    prep + 3,     prep + l.fatSectors,
                           prep + 2 * l.fatSectors + 4,      total - 2,    total - 1,
                           total};
  for (const uint64_t at : cuts) {
    fatmodel::RamDisk disk(n);
    // An old FAT32 card under it: what a cut must not bring back as ours.
    fatmodel::drive(0).disk = &disk;
    TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, 32768));
    Card card(disk);
    card.failAt = at;
    const Formatted f = format(disk, card);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "cut at sector %llu of %llu", static_cast<unsigned long long>(at),
                  static_cast<unsigned long long>(total));
    // (A cut inside f_mkfs is its FR_DISK_ERR; its FSINFO writes' errors
    // it ignores: the check finds those.)
    TEST_ASSERT_TRUE_MESSAGE(f.r.why == Why::Write || f.r.why == Why::Mkfs || f.r.why == Why::Check, msg);
    fatmodel::drive(0).disk = &disk;
    FATFS fs;
    const FRESULT m = f_mount(&fs, "0:", 1);
    if (at > cardmkfs::kHeadSectors + 1) {
      // The new MBR (or its head zeroed): no volume.
      TEST_ASSERT_EQUAL_MESSAGE(FR_NO_FILESYSTEM, m, msg);
    } else {
      // Before it: the old card's MBR, its volume at 63.
      TEST_ASSERT_TRUE_MESSAGE(m != FR_OK || fs.volbase != cardmkfs::kAlignSectors, msg);
    }
    f_mount(nullptr, "0:", 0);
    // Again, uncut: whole.
    card.failAt = 0;
    const Formatted again = format(disk, card);
    TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(Why::Ok), static_cast<int>(again.r.why), msg);
    fatmodel::drive(0).disk = &disk;
    TEST_ASSERT_EQUAL_MESSAGE(FR_OK, f_mount(&fs, "0:", 1), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(l.freeClusters() - 3, countedFree(&fs), msg);
    f_mount(nullptr, "0:", 0);
  }
}

// Each step's refusal: f_mkfs refusing (nothing mounts after it), f_mkfs
// making another layout than the plan's (the check), the files; and the
// writer's commit with nothing held.
void test_refusals() {
  const uint32_t n = 7744512;
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    const Formatted f = format(disk, card, 1, refusingMkfs);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Mkfs), static_cast<int>(f.r.why));
    TEST_ASSERT_EQUAL(FR_MKFS_ABORTED, f.r.mkfs);
    TEST_ASSERT_EQUAL_STRING("mkfs", cardmkfs::detail(f.r.why));
    fatmodel::drive(0).disk = &disk;
    FATFS fs;
    TEST_ASSERT_EQUAL(FR_NO_FILESYSTEM, f_mount(&fs, "0:", 1));
    f_mount(nullptr, "0:", 0);  // (f_mount keeps `fs` even when it fails)
  }
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    const Formatted f = format(disk, card, 1, wrongMkfs);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Check), static_cast<int>(f.r.why));
    TEST_ASSERT_EQUAL_STRING("check", cardmkfs::detail(f.r.why));
  }
  // FSINFO's count other than the plan's; FSINFO's writes refused (f_mkfs
  // ignores their errors: only the check sees it).
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    const Formatted f = format(disk, card, 1, miscountingMkfs, nullptr, &disk);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Check), static_cast<int>(f.r.why));
  }
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    card.refused = cardmkfs::kAlignSectors + 1;
    const Formatted f = format(disk, card);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Check), static_cast<int>(f.r.why));
  }
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    ModelFiles files;
    files.failMkdir = true;
    const Formatted f = format(disk, card, 1, modelMkfs, &files);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Files), static_cast<int>(f.r.why));
    TEST_ASSERT_EQUAL_STRING("files", cardmkfs::detail(f.r.why));
    TEST_ASSERT_EQUAL(1, files.unmounts);
  }
  // The mount refused (the card stopped answering after the check): Files,
  // and FatFs let go of the object the failed f_mount() registered (the
  // device frees it then): the boot's next mount, SD.begin()'s f_mount(),
  // writes nothing through it.
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    ModelFiles files;
    files.failMount = true;
    const Formatted f = format(disk, card, 1, modelMkfs, &files);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Files), static_cast<int>(f.r.why));
    TEST_ASSERT_EQUAL(1, files.mounts);
    TEST_ASSERT_EQUAL(1, files.unmounts);
    files.fs.fs_type = 0xA5;  // (freed: whatever the next allocation put there)
    FATFS next;
    f_mount(&next, "0:", 0);
    TEST_ASSERT_EQUAL_HEX8(0xA5, files.fs.fs_type);
    f_mount(nullptr, "0:", 0);
    // The card itself formatted whole: it mounts once it answers.
    fatmodel::drive(0).disk = &disk;
    TEST_ASSERT_EQUAL(FR_OK, f_mount(&next, "0:", 1));
  }
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    cardmkfs::Writer w(card, cardmkfs::plan(n));
    TEST_ASSERT_FALSE(w.commit());  // f_mkfs never wrote the boot sectors
    std::vector<uint8_t> buf(kSS);
    TEST_ASSERT_FALSE(cardmkfs::Writer(card, cardmkfs::plan(0)).prepare(1, buf.data(), 1));
    card.failAt = 1;
    TEST_ASSERT_FALSE(w.prepare(1, buf.data(), 1));
  }
  // The check on a card that isn't one the plan made: Check; a card whose
  // reads fail: Card.
  {
    fatmodel::RamDisk disk(n);
    Card card(disk);
    uint8_t s[kSS];
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Check), static_cast<int>(cardmkfs::check(card, cardmkfs::plan(n), s)));
    fatmodel::RamDisk tiny(0);
    Card none(tiny);
    TEST_ASSERT_EQUAL(static_cast<int>(Why::Card), static_cast<int>(cardmkfs::check(none, cardmkfs::plan(n), s)));
  }
}

// The writer holds the boot sector and its backup out of any run of
// writes (one in the middle of a longer write too), serves them to reads
// until commit(), writes the backup and then the boot sector, and passes
// everything straight to the card once committed.
void test_writer_holds() {
  const uint32_t n = 7744512;
  const Layout l = cardmkfs::plan(n);
  fatmodel::RamDisk disk(n);
  Card card(disk);
  cardmkfs::Writer w(card, l);
  std::vector<uint8_t> run(16 * kSS);
  for (uint32_t k = 0; k < 16; ++k) std::memset(run.data() + k * kSS, 0x10 + k, kSS);
  TEST_ASSERT_TRUE(w.write(l.partStart - 2, run.data(), 16));  // covers both
  TEST_ASSERT_EQUAL_UINT64(14, w.written());
  TEST_ASSERT_TRUE(zero(disk, l.partStart));
  TEST_ASSERT_TRUE(zero(disk, l.partStart + cardmkfs::kBackupBoot));
  uint8_t s[kSS];
  TEST_ASSERT_TRUE(w.read(l.partStart, s, 1));
  TEST_ASSERT_EQUAL_HEX8(0x12, s[0]);
  TEST_ASSERT_TRUE(disk.read(l.partStart + 1, s, 1));
  TEST_ASSERT_EQUAL_HEX8(0x13, s[0]);
  const uint64_t writesBefore = disk.counts.writes;
  TEST_ASSERT_TRUE(w.commit());
  TEST_ASSERT_EQUAL_UINT64(writesBefore + 2, disk.counts.writes);
  TEST_ASSERT_TRUE(disk.read(l.partStart, s, 1));
  TEST_ASSERT_EQUAL_HEX8(0x12, s[0]);
  TEST_ASSERT_TRUE(disk.read(l.partStart + cardmkfs::kBackupBoot, s, 1));
  TEST_ASSERT_EQUAL_HEX8(0x18, s[0]);
  TEST_ASSERT_EQUAL_UINT64(16, w.written());
  TEST_ASSERT_FALSE(w.commit());  // once
  // Committed: the card itself, uncounted.
  std::memset(s, 0x99, kSS);
  TEST_ASSERT_TRUE(w.write(l.partStart, s, 1));
  TEST_ASSERT_EQUAL_UINT64(16, w.written());
  TEST_ASSERT_TRUE(disk.read(l.partStart, s, 1));
  TEST_ASSERT_EQUAL_HEX8(0x99, s[0]);
}

// ---- the request across the restart ----

void test_request() {
  CachedDrive::Identity id;
  id.valid = true;
  id.sectors = 124735488;
  id.bootLba = 32768;
  id.sector0Crc = 0x11223344;
  id.bootCrc = 0x55667788;
  const cardmkfs::Request r = cardmkfs::request(id, cardmkfs::Origin::Computer);
  TEST_ASSERT_TRUE(cardmkfs::valid(r));
  TEST_ASSERT_TRUE(cardmkfs::sameCard(r, id));
  TEST_ASSERT_EQUAL_STRING("a computer's @format", cardmkfs::originName(static_cast<cardmkfs::Origin>(r.origin)));
  // Another card in its place: any field.
  CachedDrive::Identity other = id;
  other.sector0Crc ^= 1;
  TEST_ASSERT_FALSE(cardmkfs::sameCard(r, other));
  other = id;
  other.sectors += 1;
  TEST_ASSERT_FALSE(cardmkfs::sameCard(r, other));
  other = id;
  other.bootCrc = 0;
  TEST_ASSERT_FALSE(cardmkfs::sameCard(r, other));
  other.valid = false;  // unreadable now
  TEST_ASSERT_FALSE(cardmkfs::sameCard(r, other));
  // Every bit of the stored bytes matters: RTC memory after a power-on is
  // anything.
  for (size_t byte = 0; byte < sizeof(cardmkfs::Request); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      cardmkfs::Request x = r;
      reinterpret_cast<uint8_t*>(&x)[byte] ^= static_cast<uint8_t>(1u << bit);
      TEST_ASSERT_FALSE(cardmkfs::valid(x));
      TEST_ASSERT_FALSE(cardmkfs::sameCard(x, id));
    }
  }
  cardmkfs::Request garbage;
  std::memset(&garbage, 0xA5, sizeof(garbage));
  TEST_ASSERT_FALSE(cardmkfs::valid(garbage));
  cardmkfs::Request zeros;
  std::memset(&zeros, 0, sizeof(zeros));
  TEST_ASSERT_FALSE(cardmkfs::valid(zeros));
  // A card unreadable when confirmed: its size, and unreadable still.
  CachedDrive::Identity bad;
  bad.sectors = 62333952;
  const cardmkfs::Request u = cardmkfs::request(bad, cardmkfs::Origin::Screen);
  TEST_ASSERT_TRUE(cardmkfs::valid(u));
  TEST_ASSERT_TRUE(cardmkfs::sameCard(u, bad));
  CachedDrive::Identity readable = id;
  readable.sectors = bad.sectors;
  TEST_ASSERT_FALSE(cardmkfs::sameCard(u, readable));
  bad.sectors += 8;
  TEST_ASSERT_FALSE(cardmkfs::sameCard(u, bad));
  // No size: never a request.
  CachedDrive::Identity none;
  TEST_ASSERT_FALSE(cardmkfs::valid(cardmkfs::request(none, cardmkfs::Origin::Screen)));
  TEST_ASSERT_FALSE(cardmkfs::valid(cardmkfs::request(id, cardmkfs::Origin::None)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_cluster_sizes);
  RUN_TEST(test_plan_is_fmkfs);
  RUN_TEST(test_mbr);
  RUN_TEST(test_blank_card);
  RUN_TEST(test_old_cards);
  RUN_TEST(test_cut_anywhere);
  RUN_TEST(test_refusals);
  RUN_TEST(test_writer_holds);
  RUN_TEST(test_request);
  return UNITY_END();
}
