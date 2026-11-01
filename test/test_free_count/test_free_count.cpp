// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for FreeCount (docs/HOST-STATUS.md "@count"): the free
// clusters of a FAT32, FAT16 and FAT12 volume counted a piece at a time,
// against a count of the whole FAT at once; pieces a write reached read
// again; FatFs's window over the card's sectors and the commit's check of
// it; and, on the host FatFs model (ChaN's R0.15 as the Core2 builds it,
// test/support/FatModel.h), counts while files are written, left open,
// grown, cut and deleted between the pieces, each equal to what FatFs's
// own f_getfree() counts at the commit, and the count in FSINFO at the
// next mount once FatFs synced it.
// Run: pio test -e native -f test_free_count
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "FreeCount.h"

namespace {

using Fat = FreeCount::Fat;
using Step = FreeCount::Step;
constexpr uint32_t kSS = 512;

// A FAT on its own: the sectors from fatStart, in RAM, entries set one by
// one; a window like FatFs's (a sector held changed, not written).
struct Image {
  FreeCount::Volume v;
  std::vector<uint8_t> fat;  // fatSectors * 512 bytes, from fatStart
  bool winDirty = false;
  uint32_t winLba = 0;
  uint8_t win[kSS] = {};

  Image(Fat type, uint32_t entries, uint32_t fatStart = 100, uint32_t extraSectors = 3) {
    v.fat = type;
    v.entries = entries;
    v.fatStart = fatStart;
    const uint32_t bytes = type == Fat::Fat32 ? entries * 4 : type == Fat::Fat16 ? entries * 2 : (entries * 3 + 1) / 2;
    v.fatSectors = (bytes + kSS - 1) / kSS + extraSectors;
    v.sectorBytes = kSS;
    fat.assign(static_cast<size_t>(v.fatSectors) * kSS, 0);
  }
  void set(uint32_t e, uint32_t value) {
    uint8_t* b = fat.data();
    if (v.fat == Fat::Fat32) {
      for (int k = 0; k < 4; ++k) b[e * 4 + k] = static_cast<uint8_t>(value >> (8 * k));
    } else if (v.fat == Fat::Fat16) {
      b[e * 2] = static_cast<uint8_t>(value);
      b[e * 2 + 1] = static_cast<uint8_t>(value >> 8);
    } else {
      const uint32_t o = e + e / 2;
      if (e & 1) {
        b[o] = static_cast<uint8_t>((b[o] & 0x0F) | ((value & 0x0F) << 4));
        b[o + 1] = static_cast<uint8_t>(value >> 4);
      } else {
        b[o] = static_cast<uint8_t>(value);
        b[o + 1] = static_cast<uint8_t>((b[o + 1] & 0xF0) | ((value >> 8) & 0x0F));
      }
    }
  }
  uint32_t get(uint32_t e) const {
    const uint8_t* b = fat.data();
    if (v.fat == Fat::Fat32) return (b[e * 4] | b[e * 4 + 1] << 8 | b[e * 4 + 2] << 16 | uint32_t(b[e * 4 + 3]) << 24);
    if (v.fat == Fat::Fat16) return b[e * 2] | b[e * 2 + 1] << 8;
    const uint32_t o = e + e / 2;
    return (e & 1) ? (b[o] >> 4) | (b[o + 1] << 4) : b[o] | ((b[o + 1] & 0x0F) << 8);
  }
  // The whole FAT at once: what f_getfree() counts.
  uint32_t freeNow() const {
    uint32_t n = 0;
    for (uint32_t e = 2; e < v.entries; ++e) n += (v.fat == Fat::Fat32 ? get(e) & 0x0FFFFFFFu : get(e)) == 0;
    return n;
  }
  // Fills the entries: the two reserved, then each free (some with
  // FAT32's top bits set: still free), in use or the end of a chain.
  void fill(uint32_t seed) {
    std::mt19937 rng(seed);
    const uint32_t mask = v.fat == Fat::Fat32 ? 0x0FFFFFFFu : v.fat == Fat::Fat16 ? 0xFFFFu : 0xFFFu;
    set(0, mask & 0xFFFFFFF8u);
    set(1, mask);
    for (uint32_t e = 2; e < v.entries; ++e) {
      const uint32_t r = rng() % 10;
      if (r < 4) set(e, 0);
      else if (r == 4 && v.fat == Fat::Fat32) set(e, 0xF0000000u);  // free: the top 4 bits aren't the entry's
      else if (r == 5) set(e, mask);
      else set(e, 2 + rng() % (v.entries - 2));
    }
  }
};

// The image as FatFs would give it to the count.
class ImageSource : public FreeCount::Source {
public:
  explicit ImageSource(Image& im) : im_(im) {}
  bool read(uint32_t lba, uint8_t* out, uint32_t sectors) override {
    ++reads;
    if (failAt && reads == failAt) return false;
    for (uint32_t k = 0; k < sectors; ++k) {
      const uint32_t s = lba + k - im_.v.fatStart;
      if (lba + k < im_.v.fatStart || s >= im_.v.fatSectors) return false;
      std::memcpy(out + k * kSS, im_.fat.data() + static_cast<size_t>(s) * kSS, kSS);
    }
    return true;
  }
  bool window(uint32_t* lba, const uint8_t** bytes) override {
    if (!im_.winDirty) return false;
    *lba = im_.winLba;
    *bytes = im_.win;
    return true;
  }
  uint32_t reads = 0;
  uint32_t failAt = 0;  // that read fails (1 the first)

private:
  Image& im_;
};

uint32_t countAll(FreeCount& c, FreeCount::Source& src, int maxSteps = 100000) {
  std::vector<uint8_t> buf(FreeCount::kPieceBytes);
  for (int i = 0; i < maxSteps; ++i) {
    const Step s = c.step(src, buf.data());
    if (s == Step::Done) return c.freeClusters();
    TEST_ASSERT_TRUE(s == Step::Working);
  }
  TEST_FAIL_MESSAGE("never done");
  return 0;
}

void synthetic(Fat type, uint32_t entries, uint32_t pieces, uint32_t seed) {
  Image im(type, entries);
  im.fill(seed);
  ImageSource src(im);
  FreeCount c;
  TEST_ASSERT_TRUE(c.begin(im.v));
  TEST_ASSERT_EQUAL_UINT32(pieces, c.pieces());
  TEST_ASSERT_EQUAL_UINT8(0, c.percent());
  TEST_ASSERT_EQUAL_UINT32(im.freeNow(), countAll(c, src));
  TEST_ASSERT_EQUAL_UINT8(100, c.percent());
  TEST_ASSERT_EQUAL_UINT32(pieces, c.reads());  // each once: nothing wrote
  TEST_ASSERT_EQUAL_UINT32(pieces, src.reads);
  TEST_ASSERT_EQUAL_UINT32(0, c.rereads());
}

}  // namespace

void setUp() {}
void tearDown() {
  fatmodel::drive(0).disk = nullptr;
  fatmodel::drive(0).cache = nullptr;
}

// ---- the count on its own ----

// The entries' encodings and the pieces: FAT32 over several pieces (the
// last one short, the bytes past the last entry zero and not counted),
// FAT16 over two, FAT12 in one (odd and even entries packed in 3 bytes).
void test_synthetic_fats() {
  synthetic(Fat::Fat32, 20000, 3, 1);   // 80,000 bytes: 157 sectors
  synthetic(Fat::Fat32, 8192, 1, 2);    // exactly one piece
  synthetic(Fat::Fat32, 8193, 2, 3);    // ... and one entry over
  synthetic(Fat::Fat16, 30000, 2, 4);
  synthetic(Fat::Fat12, 4087, 1, 5);    // the most FAT12 has (4085 clusters)
  synthetic(Fat::Fat12, 3, 1, 6);       // one cluster
  // Entries 0 and 1 are never clusters: zero, they still aren't counted.
  Image im(Fat::Fat32, 1000);
  ImageSource src(im);
  FreeCount c;
  TEST_ASSERT_TRUE(c.begin(im.v));
  TEST_ASSERT_EQUAL_UINT32(998, countAll(c, src));
  TEST_ASSERT_EQUAL_UINT32(4000, c.fatBytes());
}

// Volumes FatFs couldn't have mounted, and no memory.
void test_begin_refuses() {
  FreeCount c;
  Image im(Fat::Fat32, 1000);
  FreeCount::Volume v = im.v;
  v.sectorBytes = 300;
  TEST_ASSERT_FALSE(c.begin(v));
  v = im.v;
  v.entries = 2;
  TEST_ASSERT_FALSE(c.begin(v));
  v = im.v;
  v.fatSectors = 7;  // 4000 bytes need 8
  TEST_ASSERT_FALSE(c.begin(v));
  v.fatSectors = 8;
  TEST_ASSERT_TRUE(c.begin(v));
  Image twelve(Fat::Fat12, 30000);  // 45,000 bytes: not FAT12
  TEST_ASSERT_FALSE(c.begin(twelve.v));
  TEST_ASSERT_FALSE(c.active());
  FreeCount none([](size_t) -> void* { return nullptr; }, [](void*) {});
  TEST_ASSERT_FALSE(none.begin(im.v));
  // 4 KB sectors (FatFs's FF_MAX_SS): 8 a piece.
  Image big(Fat::Fat32, 20000);
  big.v.sectorBytes = 4096;
  big.v.fatSectors = 20;
  TEST_ASSERT_TRUE(c.begin(big.v));
  TEST_ASSERT_EQUAL_UINT32(3, c.pieces());
}

// A write that reaches a piece already counted has it read again (only
// then: a piece not yet read is read as it is); the pass's pieces come
// first, in order, then those, the lowest first; the percent stays at 99
// until they are read.
void test_written_reads_again() {
  Image im(Fat::Fat32, 40000);  // 160,000 bytes: 5 pieces of 64 sectors
  im.fill(7);
  ImageSource src(im);
  FreeCount c;
  TEST_ASSERT_TRUE(c.begin(im.v));
  TEST_ASSERT_EQUAL_UINT32(5, c.pieces());
  std::vector<uint8_t> buf(FreeCount::kPieceBytes);
  TEST_ASSERT_TRUE(c.step(src, buf.data()) == Step::Working);  // piece 0
  TEST_ASSERT_TRUE(c.step(src, buf.data()) == Step::Working);  // piece 1
  TEST_ASSERT_EQUAL_UINT8(40, c.percent());
  // Entry 100 (piece 0) freed, entry 20000 (piece 2, not read yet) taken.
  im.set(100, 0);
  im.set(20000, 7);
  c.written(im.v.fatStart + 100 * 4 / kSS, 1);
  c.written(im.v.fatStart + 20000 * 4 / kSS, 1);
  c.written(5, 2);                                      // not the FAT
  c.written(im.v.fatStart + im.v.fatSectors - 1, 50);   // its unused tail and past it
  FreeCount::Piece p;
  TEST_ASSERT_TRUE(c.next(&p));
  TEST_ASSERT_EQUAL_UINT32(2, p.index);  // the pass first
  TEST_ASSERT_EQUAL_UINT32(im.v.fatStart + 128, p.lba);
  TEST_ASSERT_EQUAL_UINT32(64, p.sectors);
  for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(c.step(src, buf.data()) == Step::Working);  // 2, 3, 4
  TEST_ASSERT_FALSE(c.done());
  TEST_ASSERT_EQUAL_UINT8(99, c.percent());
  TEST_ASSERT_TRUE(c.next(&p));
  TEST_ASSERT_EQUAL_UINT32(0, p.index);  // then the one written
  // A write across pieces 1 to 3 marks them all.
  c.written(im.v.fatStart + 100, 100);
  TEST_ASSERT_EQUAL_UINT32(im.freeNow(), countAll(c, src));
  TEST_ASSERT_EQUAL_UINT32(4, c.rereads());  // 0, 1, 2, 3
  TEST_ASSERT_EQUAL_UINT32(9, c.reads());
  // The last piece is short: 160,000 bytes end 32 sectors into it.
  c.begin(im.v);
  for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(c.step(src, buf.data()) == Step::Working);
  TEST_ASSERT_TRUE(c.next(&p));
  TEST_ASSERT_EQUAL_UINT32(4, p.index);
  TEST_ASSERT_EQUAL_UINT32(313 - 256, p.sectors);
  // A read error fails the step.
  ImageSource bad(im);
  bad.failAt = 2;
  TEST_ASSERT_TRUE(c.begin(im.v));
  TEST_ASSERT_TRUE(c.step(bad, buf.data()) == Step::Working);
  TEST_ASSERT_TRUE(c.step(bad, buf.data()) == Step::Failed);
}

// FatFs's window: a FAT sector changed in RAM and not written is counted
// from the window, not the card; one changed after its piece was read,
// and still not written, is caught by the commit's check.
void test_window() {
  Image im(Fat::Fat32, 40000);
  im.fill(8);
  ImageSource src(im);
  FreeCount c;
  // Sector 3 of the FAT in the window: entries 384-511, all taken there,
  // all free on the card.
  for (uint32_t e = 384; e < 512; ++e) im.set(e, 0);
  im.winLba = im.v.fatStart + 3;
  im.winDirty = true;
  for (int k = 0; k < 128; ++k) {
    im.win[k * 4] = 9;
    im.win[k * 4 + 1] = im.win[k * 4 + 2] = im.win[k * 4 + 3] = 0;
  }
  const uint32_t onCard = im.freeNow();
  TEST_ASSERT_TRUE(c.begin(im.v));
  TEST_ASSERT_EQUAL_UINT32(onCard - 128, countAll(c, src));
  TEST_ASSERT_EQUAL_UINT32(5 + 1, c.reads());  // the commit read the window's piece once more
  // Now the window changes after piece 0 was read (entry 1000 of piece 0,
  // in sector 7, freed in RAM only): the commit's check reads it again.
  im.winDirty = false;
  TEST_ASSERT_TRUE(c.begin(im.v));
  std::vector<uint8_t> buf(FreeCount::kPieceBytes);
  TEST_ASSERT_TRUE(c.step(src, buf.data()) == Step::Working);
  const uint32_t before = im.freeNow();
  std::memcpy(im.win, im.fat.data() + 7 * kSS, kSS);
  const bool wasUsed = (im.get(1000) & 0x0FFFFFFFu) != 0;
  std::memset(im.win + (1000 * 4 - 7 * kSS), 0, 4);
  im.winLba = im.v.fatStart + 7;
  im.winDirty = true;
  uint32_t n = countAll(c, src);
  TEST_ASSERT_EQUAL_UINT32(before + (wasUsed ? 1 : 0), n);
  // The window outside the FAT (a directory's sector): nothing to check.
  im.winLba = im.v.fatStart + im.v.fatSectors + 10;
  TEST_ASSERT_TRUE(c.begin(im.v));
  countAll(c, src);
  TEST_ASSERT_EQUAL_UINT32(5, c.reads());
  // The overlay on its own: only the window's sector, only inside the range.
  uint8_t data[3 * kSS];
  std::memset(data, 0xAA, sizeof(data));
  uint8_t w[kSS];
  std::memset(w, 0x55, sizeof(w));
  FreeCount::overlay(data, 10, 3, kSS, 13, w);
  TEST_ASSERT_EQUAL_HEX8(0xAA, data[2 * kSS + 511]);
  FreeCount::overlay(data, 10, 3, kSS, 11, w);
  TEST_ASSERT_EQUAL_HEX8(0xAA, data[kSS - 1]);
  TEST_ASSERT_EQUAL_HEX8(0x55, data[kSS]);
  TEST_ASSERT_EQUAL_HEX8(0x55, data[2 * kSS - 1]);
  TEST_ASSERT_EQUAL_HEX8(0xAA, data[2 * kSS]);
}

// ---- under FatFs ----

namespace {

// The model's card with the firmware's write watch (storage/SectorDisk).
class WatchedDisk : public fatmodel::RamDisk {
public:
  using RamDisk::RamDisk;
  bool write(uint32_t lba, const uint8_t* data, uint32_t n) override {
    if (watch) watch->written(lba, n);
    return RamDisk::write(lba, data, n);
  }
  FreeCount* watch = nullptr;
};

// The firmware's source (storage/CardSpace): the card through the disk
// functions, FatFs's window from its FATFS.
class FatSource : public FreeCount::Source {
public:
  explicit FatSource(FATFS& fs) : fs_(fs) {}
  bool read(uint32_t lba, uint8_t* out, uint32_t sectors) override {
    return disk_read(fs_.pdrv, out, lba, sectors) == RES_OK;
  }
  bool window(uint32_t* lba, const uint8_t** bytes) override {
    if (!(fs_.wflag & 1)) return false;
    *lba = static_cast<uint32_t>(fs_.winsect);
    *bytes = fs_.win;
    return true;
  }

private:
  FATFS& fs_;
};

FreeCount::Volume volumeOf(const FATFS& fs) {
  FreeCount::Volume v;
  v.fat = fs.fs_type == FS_FAT32 ? Fat::Fat32 : fs.fs_type == FS_FAT16 ? Fat::Fat16 : Fat::Fat12;
  v.fatStart = static_cast<uint32_t>(fs.fatbase);
  v.fatSectors = fs.fsize;
  v.entries = fs.n_fatent;
  v.sectorBytes = kSS;
  return v;
}

// What FatFs counts itself now, holding its lock throughout (the free
// count made unknown first, so it reads the whole FAT).
uint32_t fatfsCount(FATFS& fs) {
  const DWORD kept = fs.free_clst;
  fs.free_clst = 0xFFFFFFFF;
  DWORD n = 0;
  FATFS* f = nullptr;
  TEST_ASSERT_EQUAL(FR_OK, f_getfree("0:", &n, &f));
  fs.free_clst = kept;
  return n;
}

struct Model {
  WatchedDisk disk{262144};  // 128 MB, sparse
  FATFS fs;
  FIL open[3];
  bool isOpen[3] = {};
  std::vector<std::string> files;
  uint32_t names = 0;
};

// A new card for a test (on the heap, kept until the next: a failed
// assertion jumps past destructors), the last one let go first.
Model& freshModel() {
  static Model* m = nullptr;
  if (m) {
    f_mount(nullptr, "0:", 0);
    fatmodel::drive(0).disk = nullptr;
    delete m;
  }
  m = new Model();
  return *m;
}

void mountModel(Model& m, uint32_t clusterBytes) {
  fatmodel::drive(0).disk = &m.disk;
  fatmodel::drive(0).cache = nullptr;
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::format(0, clusterBytes));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &m.fs));
}

// Some file system work, as the card worker's and the loop's writes would
// do it between two pieces: a new file, a file grown, cut or deleted,
// and some left open with their FAT changes in FatFs's window.
void work(Model& m, std::mt19937& rng) {
  std::vector<uint8_t> data(9000);
  for (auto& b : data) b = static_cast<uint8_t>(rng() | 1);
  UINT wrote = 0;
  const uint32_t r = rng() % 6;
  const int slot = static_cast<int>(rng() % 3);
  // FatFs looks for free clusters from the last it took (FSINFO's hint):
  // sent anywhere now and then, so the writes reach pieces all over the
  // FAT, counted ones and ones still to count.
  if (rng() % 2) m.fs.last_clst = 2 + rng() % (m.fs.n_fatent - 2);
  if (r == 0 || m.files.empty()) {
    char name[32];
    std::snprintf(name, sizeof(name), "0:/f%u.bin", m.names++);
    FIL f;
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, name, FA_WRITE | FA_CREATE_NEW));
    TEST_ASSERT_EQUAL(FR_OK, f_write(&f, data.data(), static_cast<UINT>(1 + rng() % data.size()), &wrote));
    TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
    m.files.push_back(name);
  } else if (r == 1) {
    const size_t i = rng() % m.files.size();
    TEST_ASSERT_EQUAL(FR_OK, f_unlink(m.files[i].c_str()));
    m.files.erase(m.files.begin() + static_cast<long>(i));
  } else if (r == 2 || r == 3) {
    // Grow one left open (its FAT sector stays in the window).
    if (!m.isOpen[slot]) {
      char name[32];
      std::snprintf(name, sizeof(name), "0:/open%d.bin", slot);
      TEST_ASSERT_EQUAL(FR_OK, f_open(&m.open[slot], name, FA_WRITE | FA_OPEN_APPEND));
      m.isOpen[slot] = true;
    }
    TEST_ASSERT_EQUAL(FR_OK, f_write(&m.open[slot], data.data(), static_cast<UINT>(1 + rng() % data.size()), &wrote));
  } else if (r == 4 && m.isOpen[slot]) {
    // Cut it (its clusters freed), or close it (synced: the window written).
    if (rng() % 2) {
      TEST_ASSERT_EQUAL(FR_OK, f_lseek(&m.open[slot], f_size(&m.open[slot]) / 2));
      TEST_ASSERT_EQUAL(FR_OK, f_truncate(&m.open[slot]));
    } else {
      TEST_ASSERT_EQUAL(FR_OK, f_close(&m.open[slot]));
      m.isOpen[slot] = false;
    }
  } else {
    // Rewrite a file whole (the queue's save): freed, then taken again elsewhere.
    const size_t i = rng() % m.files.size();
    FIL f;
    TEST_ASSERT_EQUAL(FR_OK, f_open(&f, m.files[i].c_str(), FA_WRITE | FA_CREATE_ALWAYS));
    TEST_ASSERT_EQUAL(FR_OK, f_write(&f, data.data(), static_cast<UINT>(1 + rng() % data.size()), &wrote));
    TEST_ASSERT_EQUAL(FR_OK, f_close(&f));
  }
}

}  // namespace

// Counts on a card that FatFs writes between the pieces, every step or now
// and then: each equal to f_getfree() at the commit (FAT32, 512 B clusters
// so the FAT is 32 pieces; FAT16 too).
void test_against_fatfs_while_writing() {
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    Model& m = freshModel();
    mountModel(m, 512);
    TEST_ASSERT_EQUAL(FS_FAT32, m.fs.fs_type);
    std::mt19937 rng(seed);
    for (int i = 0; i < 40; ++i) work(m, rng);
    FreeCount c;
    TEST_ASSERT_TRUE(c.begin(volumeOf(m.fs)));
    TEST_ASSERT_TRUE(c.pieces() >= 30);  // about 260k clusters: a 1 MB FAT
    m.disk.watch = &c;
    FatSource src(m.fs);
    std::vector<uint8_t> buf(FreeCount::kPieceBytes);
    Step s = Step::Working;
    int steps = 0;
    const uint32_t every = 1 + seed % 3;  // FatFs's work between every step, or every other, or third
    while ((s = c.step(src, buf.data())) == Step::Working) {
      TEST_ASSERT_TRUE(++steps < 10000);
      if (steps % every == 0) work(m, rng);
    }
    TEST_ASSERT_TRUE(s == Step::Done);
    m.disk.watch = nullptr;
    // The commit: what FatFs itself counts now, its window included.
    TEST_ASSERT_EQUAL_UINT32(fatfsCount(m.fs), c.freeClusters());
    char msg[96];
    std::snprintf(msg, sizeof(msg), "seed %u: %u steps, %u pieces read (%u again), %u free", static_cast<unsigned>(seed),
                  static_cast<unsigned>(steps), static_cast<unsigned>(c.reads()), static_cast<unsigned>(c.rereads()),
                  static_cast<unsigned>(c.freeClusters()));
    TEST_MESSAGE(msg);
    TEST_ASSERT_TRUE(c.rereads() > 0);  // the work did reach counted pieces
    for (int k = 0; k < 3; ++k) {
      if (m.isOpen[k]) f_close(&m.open[k]);
    }
  }
}

// What the firmware does with the count (storage/CardSpace): FatFs's free
// count set, and FSINFO marked to be written, as f_getfree() does; FatFs
// keeps the count as files come and go; it is written at FatFs's next
// sync, and the next mount reads it from FSINFO at once.
void test_commit_and_fsinfo() {
  Model& m = freshModel();
  mountModel(m, 512);
  std::mt19937 rng(42);
  for (int i = 0; i < 30; ++i) work(m, rng);
  for (int k = 0; k < 3; ++k) {
    if (m.isOpen[k]) {
      f_close(&m.open[k]);
      m.isOpen[k] = false;
    }
  }
  // A card from elsewhere: its FSINFO says "unknown", as the mount reads.
  m.fs.free_clst = 0xFFFFFFFF;
  FreeCount c;
  TEST_ASSERT_TRUE(c.begin(volumeOf(m.fs)));
  m.disk.watch = &c;
  FatSource src(m.fs);
  std::vector<uint8_t> buf(FreeCount::kPieceBytes);
  Step s;
  while ((s = c.step(src, buf.data())) == Step::Working) {
  }
  TEST_ASSERT_TRUE(s == Step::Done);
  m.disk.watch = nullptr;
  m.fs.free_clst = c.freeClusters();
  m.fs.fsi_flag |= 1;
  // FatFs follows it: a file written and one deleted.
  for (int i = 0; i < 10; ++i) work(m, rng);
  for (int k = 0; k < 3; ++k) {
    if (m.isOpen[k]) {
      f_close(&m.open[k]);
      m.isOpen[k] = false;
    }
  }
  const uint32_t followed = m.fs.free_clst;
  TEST_ASSERT_EQUAL_UINT32(fatfsCount(m.fs), followed);
  // Synced (the work's f_close() did): the next mount reads it from FSINFO.
  TEST_ASSERT_EQUAL(FR_OK, f_mount(nullptr, "0:", 0));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &m.fs));
  TEST_ASSERT_EQUAL_UINT32(followed, m.fs.free_clst);
}

// FAT16 under FatFs (no FSINFO there: FatFs's count is unknown after the
// mount, the count makes it known).
void test_fat16_under_fatfs() {
  Model& m = freshModel();
  fatmodel::drive(0).disk = &m.disk;
  MKFS_PARM opt = {FM_FAT, 2, 0, 0, 4096};
  std::vector<uint8_t> work16(64 * 1024);
  TEST_ASSERT_EQUAL(FR_OK, f_mkfs("0:", &opt, work16.data(), static_cast<UINT>(work16.size())));
  TEST_ASSERT_EQUAL(FR_OK, fatmodel::mount(0, &m.fs));
  TEST_ASSERT_EQUAL(FS_FAT16, m.fs.fs_type);
  std::mt19937 rng(16);
  for (int i = 0; i < 30; ++i) work(m, rng);
  FreeCount c;
  TEST_ASSERT_TRUE(c.begin(volumeOf(m.fs)));
  m.disk.watch = &c;
  FatSource src(m.fs);
  std::vector<uint8_t> buf(FreeCount::kPieceBytes);
  Step s;
  int steps = 0;
  while ((s = c.step(src, buf.data())) == Step::Working) {
    if (++steps % 2 == 0) work(m, rng);
  }
  TEST_ASSERT_TRUE(s == Step::Done);
  TEST_ASSERT_EQUAL_UINT32(fatfsCount(m.fs), c.freeClusters());
  m.disk.watch = nullptr;
  for (int k = 0; k < 3; ++k) {
    if (m.isOpen[k]) f_close(&m.open[k]);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_synthetic_fats);
  RUN_TEST(test_begin_refuses);
  RUN_TEST(test_written_reads_again);
  RUN_TEST(test_window);
  RUN_TEST(test_against_fatfs_while_writing);
  RUN_TEST(test_commit_and_fsinfo);
  RUN_TEST(test_fat16_under_fatfs);
  return UNITY_END();
}
