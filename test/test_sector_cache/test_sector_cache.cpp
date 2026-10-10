// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for SectorCache (docs/METADATA.md 3.2.4, row N8 of 6.1): hits
// and misses, the LRU's order and eviction, the bypass of multi-sector
// reads, writes through to the device (a cached sector refreshed, a failed
// write's sectors dropped, nothing added), a failed read that keeps
// nothing, invalidate() over short and long ranges and at the top of the
// LBA space, clear(), no memory (everything passes through), the block's
// size, and a random model check against a reference LRU and the device's
// own bytes. And the diskio wrapper's rules over it (CachedDrive, N10's
// review): a mount clears it, a write with the cache off drops what it
// held of those sectors, on again it starts empty, verify catches a sector
// the card no longer has, the switches and the counts' reset wait for the
// next disk call, every card read and write counted, a TRIM's range
// dropped; and since the 2026-10-09 device run, a write the card refused
// written again once, and the card's identity (another card at a remount
// is foreign until a restart, every write refused). FatFs over the cache
// is test_fat_model's. Run:
// pio test -e native -f test_sector_cache
#include <unity.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

#include "CachedDrive.h"
#include "SectorCache.h"

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kSS = SectorCache::kSectorBytes;
using Sector = std::array<uint8_t, kSS>;

// A sparse device: a sector never written reads as a pattern of its LBA.
// Faults on demand: the next `failReads` reads fail; the next `failWrites`
// writes fail writing nothing; the next write fails after writing its
// first `failWriteAfter` sectors (-1: no fault).
class RamDevice : public SectorCache::Device {
public:
  bool read(uint32_t lba, uint8_t* out, uint32_t count) override {
    ++reads;
    readSectors += count;
    lastReadCount = count;
    if (failReads > 0) {
      --failReads;
      std::memset(out, 0xEE, static_cast<size_t>(count) * kSS);  // a failed read may leave junk
      return false;
    }
    for (uint32_t k = 0; k < count; ++k) at(lba + k, out + static_cast<size_t>(k) * kSS);
    return true;
  }
  bool write(uint32_t lba, const uint8_t* data, uint32_t count) override {
    ++writes;
    if (failWrites > 0) {
      --failWrites;
      return false;
    }
    uint32_t n = count;
    const bool fail = failWriteAfter >= 0;
    if (fail) {
      n = static_cast<uint32_t>(failWriteAfter) < count ? static_cast<uint32_t>(failWriteAfter) : count;
      failWriteAfter = -1;
    }
    for (uint32_t k = 0; k < n; ++k) std::memcpy(sectors[lba + k].data(), data + static_cast<size_t>(k) * kSS, kSS);
    return !fail;
  }
  void at(uint32_t lba, uint8_t* out) const {
    auto it = sectors.find(lba);
    if (it != sectors.end()) {
      std::memcpy(out, it->second.data(), kSS);
      return;
    }
    for (uint32_t i = 0; i < kSS; ++i) out[i] = static_cast<uint8_t>((lba * 7u) ^ (lba >> 8) ^ (i * 13u));
  }
  bool equals(uint32_t lba, const uint8_t* bytes) const {
    uint8_t want[kSS];
    at(lba, want);
    return std::memcmp(want, bytes, kSS) == 0;
  }

  std::map<uint32_t, Sector> sectors;
  uint32_t reads = 0, readSectors = 0, lastReadCount = 0, writes = 0;
  int failReads = 0;
  int failWrites = 0;
  int failWriteAfter = -1;
};

Sector filled(uint8_t v) {
  Sector s;
  s.fill(v);
  return s;
}

std::vector<uint32_t> order(const SectorCache& c) {
  std::vector<uint32_t> v(c.size() + 1);
  v.resize(c.order(v.data(), static_cast<uint32_t>(v.size())));
  return v;
}

void expectOrder(const SectorCache& c, std::vector<uint32_t> want) {
  const std::vector<uint32_t> got = order(c);
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_UINT32(want[i], got[i]);
}

size_t live = 0;
void* countAlloc(size_t n) {
  live += n;
  return std::malloc(n);
}
void countFree(void* p) { std::free(p); }
void* noAlloc(size_t) { return nullptr; }

}  // namespace

void test_hit_and_miss() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(4));
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(c.read(dev, 100, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(100, buf));
  TEST_ASSERT_EQUAL_UINT32(1, dev.reads);
  TEST_ASSERT_TRUE(c.holds(100));
  std::memset(buf, 0, sizeof(buf));
  TEST_ASSERT_TRUE(c.read(dev, 100, buf, 1));  // a hit: the device isn't asked
  TEST_ASSERT_TRUE(dev.equals(100, buf));
  TEST_ASSERT_EQUAL_UINT32(1, dev.reads);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().hits);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().misses);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().deviceReads());
  TEST_ASSERT_TRUE(c.read(dev, 7, buf, 0));  // nothing asked, nothing done
  TEST_ASSERT_EQUAL_UINT32(1, dev.reads);
  TEST_ASSERT_EQUAL_UINT32(1, c.size());
}

void test_eviction_is_lru() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(3));
  uint8_t buf[kSS];
  for (uint32_t lba : {10u, 11u, 12u}) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  expectOrder(c, {12, 11, 10});
  TEST_ASSERT_TRUE(c.read(dev, 10, buf, 1));  // a hit moves to the front
  expectOrder(c, {10, 12, 11});
  TEST_ASSERT_TRUE(c.read(dev, 13, buf, 1));  // full: 11, the least recently used, goes
  expectOrder(c, {13, 10, 12});
  TEST_ASSERT_FALSE(c.holds(11));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().evicted);
  TEST_ASSERT_TRUE(c.read(dev, 11, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(11, buf));
  expectOrder(c, {11, 13, 10});
  TEST_ASSERT_EQUAL_UINT32(5, dev.reads);

  // One entry: every new sector replaces the last.
  SectorCache one;
  TEST_ASSERT_TRUE(one.begin(1));
  TEST_ASSERT_TRUE(one.read(dev, 1, buf, 1));
  TEST_ASSERT_TRUE(one.read(dev, 2, buf, 1));
  expectOrder(one, {2});
  TEST_ASSERT_TRUE(one.read(dev, 2, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, one.stats().hits);
}

void test_multi_sector_reads_bypass() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  uint8_t buf[kSS * 8];
  TEST_ASSERT_TRUE(c.read(dev, 52, buf, 1));
  expectOrder(c, {52});
  const uint32_t before = dev.reads;
  TEST_ASSERT_TRUE(c.read(dev, 50, buf, 8));  // one device call, nothing kept or moved
  TEST_ASSERT_EQUAL_UINT32(before + 1, dev.reads);
  TEST_ASSERT_EQUAL_UINT32(8, dev.lastReadCount);
  for (uint32_t k = 0; k < 8; ++k) TEST_ASSERT_TRUE(dev.equals(50 + k, buf + k * kSS));
  expectOrder(c, {52});
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().bypassed);
  TEST_ASSERT_EQUAL_UINT32(8, c.stats().bypassedSectors);
  // The cached copy of a sector inside the range is still the card's.
  TEST_ASSERT_TRUE(c.read(dev, 52, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(52, buf));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().hits);
  // A failed bypass: false, counted, the cache as it was.
  dev.failReads = 1;
  TEST_ASSERT_FALSE(c.read(dev, 60, buf, 4));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().failedReads);
  expectOrder(c, {52});
}

void test_writes_go_through() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  uint8_t buf[kSS];
  for (uint32_t lba : {1u, 2u, 3u}) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  expectOrder(c, {3, 2, 1});
  // A cached sector takes the written bytes; the recency doesn't change.
  const Sector a = filled(0xA1);
  TEST_ASSERT_TRUE(c.write(dev, 1, a.data(), 1));
  TEST_ASSERT_TRUE(dev.equals(1, a.data()));
  expectOrder(c, {3, 2, 1});
  const uint32_t reads = dev.reads;
  TEST_ASSERT_TRUE(c.read(dev, 1, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(reads, dev.reads);
  TEST_ASSERT_EQUAL_MEMORY(a.data(), buf, kSS);
  // An uncached sector isn't added by a write.
  TEST_ASSERT_TRUE(c.write(dev, 9, a.data(), 1));
  TEST_ASSERT_FALSE(c.holds(9));
  // A multi-sector write refreshes each cached sector it covers.
  std::vector<uint8_t> run(kSS * 4);
  for (size_t i = 0; i < run.size(); ++i) run[i] = static_cast<uint8_t>(i / kSS + 0x40);
  TEST_ASSERT_TRUE(c.write(dev, 0, run.data(), 4));  // sectors 0..3: 1, 2, 3 cached
  TEST_ASSERT_EQUAL_UINT32(4, c.stats().updated);   // 1 above, then 1, 2, 3
  for (uint32_t lba = 1; lba <= 3; ++lba) {
    TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
    TEST_ASSERT_EQUAL_MEMORY(run.data() + lba * kSS, buf, kSS);
  }
  TEST_ASSERT_EQUAL_UINT32(reads, dev.reads);
  TEST_ASSERT_FALSE(c.holds(0));
  TEST_ASSERT_EQUAL_UINT32(3, c.stats().writes);
  TEST_ASSERT_EQUAL_UINT32(6, c.stats().writtenSectors);
  // A write longer than the cache holds (the cached sectors walked instead).
  std::vector<uint8_t> big(kSS * 20, 0x5C);
  TEST_ASSERT_TRUE(c.write(dev, 2, big.data(), 20));
  TEST_ASSERT_TRUE(c.read(dev, 3, buf, 1));
  TEST_ASSERT_EQUAL_HEX8(0x5C, buf[0]);
  TEST_ASSERT_EQUAL_HEX8(0x5C, buf[kSS - 1]);
  TEST_ASSERT_EQUAL_UINT32(reads, dev.reads);
}

void test_failed_write_drops() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  uint8_t buf[kSS];
  for (uint32_t lba : {4u, 5u, 6u, 7u}) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  // Fails after one sector: 5 was written, 6 and 7 weren't; all three dropped.
  std::vector<uint8_t> run(kSS * 3, 0x77);
  dev.failWriteAfter = 1;
  TEST_ASSERT_FALSE(c.write(dev, 5, run.data(), 3));
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().failedWrites);
  TEST_ASSERT_EQUAL_UINT32(3, c.stats().dropped);
  expectOrder(c, {4});
  for (uint32_t lba = 5; lba <= 7; ++lba) {
    TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));  // the device's bytes, whatever the write left
    TEST_ASSERT_TRUE(dev.equals(lba, buf));
  }
  TEST_ASSERT_EQUAL_UINT32(0, dev.sectors.count(6) + dev.sectors.count(7));  // 6 and 7 kept their old bytes
  TEST_ASSERT_TRUE(c.read(dev, 5, buf, 1));
  TEST_ASSERT_EQUAL_HEX8(0x77, buf[0]);  // 5 has the new ones
  // A single-sector write that fails before writing anything.
  dev.failWriteAfter = 0;
  const Sector z = filled(0x00);
  TEST_ASSERT_FALSE(c.write(dev, 4, z.data(), 1));
  TEST_ASSERT_FALSE(c.holds(4));
  TEST_ASSERT_TRUE(c.read(dev, 4, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(4, buf));
}

void test_failed_read_keeps_nothing() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(2));
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(c.read(dev, 1, buf, 1));
  TEST_ASSERT_TRUE(c.read(dev, 2, buf, 1));
  dev.failReads = 1;
  TEST_ASSERT_FALSE(c.read(dev, 3, buf, 1));  // full, but 1 isn't given up for nothing
  expectOrder(c, {2, 1});
  TEST_ASSERT_EQUAL_UINT32(0, c.stats().evicted);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().failedReads);
  TEST_ASSERT_TRUE(c.read(dev, 3, buf, 1));  // then it reads
  TEST_ASSERT_TRUE(dev.equals(3, buf));
  expectOrder(c, {3, 2});
}

void test_invalidate_and_clear() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(6));
  uint8_t buf[kSS];
  for (uint32_t lba : {10u, 11u, 12u, 20u, 0xFFFFFFFEu, 0xFFFFFFFFu}) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(0xFFFFFFFFu, buf));
  c.invalidate(11, 2);  // short: looked up one by one
  expectOrder(c, {0xFFFFFFFFu, 0xFFFFFFFEu, 20, 10});
  c.invalidate(0xFFFFFFF0u, 0x100);  // runs past the last LBA: stops there, no wrap to 10
  expectOrder(c, {20, 10});
  c.invalidate(0, 1000);  // long: the cached sectors walked
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  TEST_ASSERT_EQUAL_UINT32(6, c.stats().dropped);
  c.invalidate(5, 0);
  for (uint32_t lba : {1u, 2u, 3u}) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  c.clear();
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  TEST_ASSERT_FALSE(c.holds(1));
  const uint32_t reads = dev.reads;
  TEST_ASSERT_TRUE(c.read(dev, 1, buf, 1));  // a mount's first read: from the card
  TEST_ASSERT_EQUAL_UINT32(reads + 1, dev.reads);
  // After a clear the slots are all free again: six new sectors, no eviction.
  const uint32_t evicted = c.stats().evicted;
  for (uint32_t lba = 100; lba < 105; ++lba) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(6, c.size());
  TEST_ASSERT_EQUAL_UINT32(evicted, c.stats().evicted);
}

void test_no_memory_passes_through() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_FALSE(c.begin(128, noAlloc, countFree));
  TEST_ASSERT_EQUAL_UINT32(0, c.capacity());
  TEST_ASSERT_EQUAL_UINT32(0, c.bytes());
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(c.read(dev, 5, buf, 1));
  TEST_ASSERT_TRUE(c.read(dev, 5, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(5, buf));
  TEST_ASSERT_EQUAL_UINT32(2, dev.reads);
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  const Sector a = filled(0x3C);
  TEST_ASSERT_TRUE(c.write(dev, 5, a.data(), 1));
  TEST_ASSERT_TRUE(c.read(dev, 5, buf, 1));
  TEST_ASSERT_EQUAL_MEMORY(a.data(), buf, kSS);
  c.invalidate(0, 10);
  c.clear();
  // Never begun: the same.
  SectorCache never;
  TEST_ASSERT_TRUE(never.read(dev, 6, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(6, buf));
  TEST_ASSERT_FALSE(never.holds(6));
}

void test_block_sizes() {
  // 128 KB of sectors (3.2.7), plus 12 B of links each and 512 heads;
  // 3.2.4's first 64 KB, 128 sectors, the same way.
  TEST_ASSERT_EQUAL_UINT32(135168, SectorCache::blockBytes(SectorCache::kDefaultEntries));
  TEST_ASSERT_EQUAL_UINT32(67584, SectorCache::blockBytes(128));
  TEST_ASSERT_EQUAL_UINT32(0, SectorCache::blockBytes(0));
  TEST_ASSERT_EQUAL_UINT32(0, SectorCache::blockBytes(SectorCache::kMaxEntries + 1));
  TEST_ASSERT_EQUAL_UINT32(4096u * 524u + 8192u * 2u, SectorCache::blockBytes(SectorCache::kMaxEntries));
  live = 0;
  {
    SectorCache c;
    TEST_ASSERT_TRUE(c.begin(SectorCache::kDefaultEntries, countAlloc, countFree));
    TEST_ASSERT_EQUAL_UINT32(135168, live);
    TEST_ASSERT_EQUAL_UINT32(135168, c.bytes());
    TEST_ASSERT_FALSE(c.begin(0, countAlloc, countFree));  // the old block went back first
    TEST_ASSERT_EQUAL_UINT32(0, c.capacity());
    TEST_ASSERT_TRUE(c.begin(SectorCache::kMaxEntries, countAlloc, countFree));
    RamDevice dev;
    uint8_t buf[kSS];
    for (uint32_t lba = 0; lba < SectorCache::kMaxEntries + 10; ++lba) TEST_ASSERT_TRUE(c.read(dev, lba, buf, 1));
    TEST_ASSERT_EQUAL_UINT32(SectorCache::kMaxEntries, c.size());
    TEST_ASSERT_EQUAL_UINT32(10, c.stats().evicted);
    TEST_ASSERT_TRUE(c.read(dev, 10, buf, 1));
    TEST_ASSERT_TRUE(dev.equals(10, buf));
    TEST_ASSERT_EQUAL_UINT32(1, c.stats().hits);
  }
}

// ---- the random model check ----

namespace {

// The reference: an LRU in a std::list (front: the most recently used),
// and its rules from the class comment.
class Reference {
public:
  explicit Reference(uint32_t cap) : cap_(cap) {}
  // A single-sector read: true if it hits. `deviceOk`: what the device
  // would answer on a miss.
  bool read(uint32_t lba, bool deviceOk) {
    auto it = pos_.find(lba);
    if (it != pos_.end()) {
      lru_.splice(lru_.begin(), lru_, it->second);
      return true;
    }
    if (!deviceOk) return false;
    if (lru_.size() == cap_) {
      pos_.erase(lru_.back());
      lru_.pop_back();
    }
    lru_.push_front(lba);
    pos_[lba] = lru_.begin();
    return false;
  }
  void drop(uint32_t lba, uint32_t count) {
    const uint64_t end = static_cast<uint64_t>(lba) + count;
    for (auto it = lru_.begin(); it != lru_.end();) {
      if (*it >= lba && *it < end) {
        pos_.erase(*it);
        it = lru_.erase(it);
      } else {
        ++it;
      }
    }
  }
  void clear() {
    lru_.clear();
    pos_.clear();
  }
  std::vector<uint32_t> order() const { return std::vector<uint32_t>(lru_.begin(), lru_.end()); }

private:
  uint32_t cap_;
  std::list<uint32_t> lru_;
  std::unordered_map<uint32_t, std::list<uint32_t>::iterator> pos_;
};

void randomRun(uint32_t cap, uint32_t seed, uint32_t ops) {
  std::mt19937 rng(seed);
  auto pick = [&](uint32_t n) { return static_cast<uint32_t>(rng() % n); };
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(cap));
  Reference ref(cap);
  const uint32_t space = cap * 4 + 8;
  auto lbaOf = [&]() -> uint32_t {
    const uint32_t r = pick(100);
    if (r < 2) return 0xFFFFFFFFu - pick(8);  // the top of the LBA space
    if (r < 70) return pick(cap + cap / 2 + 1);
    return pick(space);
  };
  std::vector<uint8_t> buf(kSS * (2 * cap + 16));
  uint32_t deviceReads = 0, hits = 0;
  for (uint32_t op = 0; op < ops; ++op) {
    const uint32_t r = pick(100);
    if (r < 50) {  // a single-sector read
      const uint32_t lba = lbaOf();
      const bool fail = pick(20) == 0;
      const bool hit = ref.read(lba, !fail);
      if (!hit) {
        ++deviceReads;
        dev.failReads = fail ? 1 : 0;
      }
      const bool ok = c.read(dev, lba, buf.data(), 1);
      dev.failReads = 0;
      TEST_ASSERT_EQUAL(hit || !fail, ok);
      if (ok) TEST_ASSERT_TRUE(dev.equals(lba, buf.data()));
      hits += hit;
    } else if (r < 60) {  // a multi-sector read: the device's, the cache untouched
      const uint32_t n = 2 + pick(9);
      const uint32_t lba = pick(space);
      const bool fail = pick(20) == 0;
      dev.failReads = fail ? 1 : 0;
      ++deviceReads;
      const bool ok = c.read(dev, lba, buf.data(), n);
      dev.failReads = 0;
      TEST_ASSERT_EQUAL(!fail, ok);
      if (ok)
        for (uint32_t k = 0; k < n; ++k) TEST_ASSERT_TRUE(dev.equals(lba + k, buf.data() + k * kSS));
    } else if (r < 88) {  // a write, sometimes long, sometimes failing part way
      const uint32_t n = pick(3) == 0 ? 2 + pick(2 * cap + 8) : 1;
      const uint32_t lba = pick(space);
      for (uint32_t i = 0; i < n * kSS; ++i) buf[i] = static_cast<uint8_t>(rng());
      const bool fail = pick(10) == 0;
      dev.failWriteAfter = fail ? static_cast<int>(pick(n + 1)) : -1;
      const bool ok = c.write(dev, lba, buf.data(), n);
      dev.failWriteAfter = -1;
      TEST_ASSERT_EQUAL(!fail, ok);
      if (!ok) ref.drop(lba, n);
    } else if (r < 97) {  // invalidate, short or long, sometimes at the top
      const uint32_t n = pick(4) == 0 ? 1 + pick(2 * cap + 4) : 1 + pick(4);
      const uint32_t lba = pick(10) == 0 ? 0xFFFFFFFFu - pick(16) : pick(space);
      c.invalidate(lba, n);
      ref.drop(lba, n);
    } else if (r < 98) {
      c.clear();
      ref.clear();
    } else {  // every cached sector checked against the card
      std::vector<uint32_t> held = order(c);
      for (uint32_t lba : held) {
        TEST_ASSERT_TRUE(ref.read(lba, true));
        TEST_ASSERT_TRUE(c.read(dev, lba, buf.data(), 1));
        TEST_ASSERT_TRUE(dev.equals(lba, buf.data()));
        ++hits;
      }
    }
    // The same sectors in the same order as the reference, and the device
    // asked exactly when the reference says.
    const std::vector<uint32_t> want = ref.order();
    const std::vector<uint32_t> got = order(c);
    TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
    for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_UINT32(want[i], got[i]);
    TEST_ASSERT_EQUAL_UINT32(deviceReads, dev.reads);
  }
  TEST_ASSERT_EQUAL_UINT32(hits, c.stats().hits);
  TEST_ASSERT_EQUAL_UINT32(deviceReads, c.stats().deviceReads());
  TEST_ASSERT_TRUE(c.stats().hits > ops / 10);  // the run did exercise hits
  TEST_ASSERT_TRUE(c.stats().evicted > 0);
}

}  // namespace

void test_random_against_reference() {
  const uint32_t caps[] = {1, 2, 3, 8, 13, 64};
  uint32_t seed = 1;
  for (uint32_t cap : caps)
    for (int k = 0; k < 3; ++k) randomRun(cap, seed++, cap >= 64 ? 20000 : 30000);
}

// ---- CachedDrive: the diskio wrapper's rules ----

// A mount (FatFs's disk_initialize: also after the card stopped answering,
// a card pulled or swapped while on) clears the cache: the next read of a
// sector it held goes to the card, whatever card is there now.
void test_drive_mount_clears() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  d.init();
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(0, buf, 1));  // the boot sector, say
  TEST_ASSERT_TRUE(d.read(5, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(2, c.size());
  // Another card in the slot: its sectors 0 and 5 aren't the cached ones.
  dev.sectors[0] = filled(0xA0);
  dev.sectors[5] = filled(0xA5);
  TEST_ASSERT_TRUE(d.read(5, buf, 1));
  TEST_ASSERT_FALSE(dev.equals(5, buf));  // (what a cache kept across the swap serves)
  d.init();
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
  TEST_ASSERT_TRUE(d.read(0, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(0, buf));
  TEST_ASSERT_TRUE(d.read(5, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(5, buf));
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().inits);
}

// Off: every read and write goes to the card, and a write drops what the
// cache held of its sectors (so it can't be stale when the cache comes on
// again, whatever the order of the switch and a call under way on another
// task). On again: it starts empty, at the next call.
void test_drive_off_and_on() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  d.init();
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(3, buf, 1));
  TEST_ASSERT_TRUE(d.read(4, buf, 1));
  TEST_ASSERT_TRUE(c.holds(3));
  d.setEnabled(false);
  TEST_ASSERT_FALSE(d.enabled());
  const uint32_t before = dev.reads;
  TEST_ASSERT_TRUE(d.read(3, buf, 1));  // off: the card's, though the cache holds it
  TEST_ASSERT_EQUAL_UINT32(before + 1, dev.reads);
  TEST_ASSERT_EQUAL_UINT32(0, c.stats().hits);
  const Sector w = filled(0x5C);
  TEST_ASSERT_TRUE(d.write(3, w.data(), 1));
  TEST_ASSERT_TRUE(dev.equals(3, w.data()));
  TEST_ASSERT_FALSE(c.holds(3));  // dropped at once, not at the switch
  TEST_ASSERT_TRUE(c.holds(4));
  TEST_ASSERT_EQUAL_UINT32(0, c.stats().writes);  // the cache wasn't written through
  d.setEnabled(true);
  TEST_ASSERT_TRUE(c.holds(4));  // the clear waits for the next disk call
  TEST_ASSERT_TRUE(d.read(3, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(3, buf));
  TEST_ASSERT_FALSE(c.holds(4));  // cleared: a cold start
  TEST_ASSERT_TRUE(c.holds(3));
  // A failed write while off drops the sector too (refused twice: the
  // wrapper writes it again once).
  d.setEnabled(false);
  dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(3, w.data(), 1));
  TEST_ASSERT_FALSE(c.holds(3));
}

// Verify: a hit read from the card again and compared; a sector the card
// changed behind the cache (what a cache bug would leave) is counted, its
// LBA kept, the card's bytes returned and the sector dropped.
void test_drive_verify() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  d.init();
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(9, buf, 1));
  TEST_ASSERT_TRUE(d.read(9, buf, 1));  // verify off: a plain hit
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().verified);
  d.setVerify(true);
  TEST_ASSERT_TRUE(d.verifying());
  TEST_ASSERT_TRUE(d.read(9, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, d.stats().verified);
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().stale);
  dev.sectors[9] = filled(0x99);  // around the cache
  TEST_ASSERT_TRUE(d.read(9, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(9, buf));
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().verified);
  TEST_ASSERT_EQUAL_UINT32(1, d.stats().stale);
  TEST_ASSERT_EQUAL_UINT32(9, d.stats().staleLba);
  TEST_ASSERT_FALSE(c.holds(9));
  // A miss and a multi-sector read aren't checked (nothing cached to doubt).
  uint8_t two[2 * kSS];
  TEST_ASSERT_TRUE(d.read(20, two, 2));
  TEST_ASSERT_TRUE(d.read(21, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().verified);
  // No scratch: verify does nothing.
  d.setScratch(nullptr);
  TEST_ASSERT_TRUE(d.read(21, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().verified);
}

// The counts: every card read (its sectors, single ones apart, its time by
// the clock) and write, on or off; TRIMs (their range dropped); the reset
// taken at the next disk call, the cache's counts with it.
uint64_t fakeUs = 0;
uint64_t fakeClock() { return fakeUs += 7; }

void test_drive_counts_and_reset() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev, fakeClock);
  d.init();
  uint8_t buf[4 * kSS];
  TEST_ASSERT_TRUE(d.read(1, buf, 1));   // a miss: a card read
  TEST_ASSERT_TRUE(d.read(1, buf, 1));   // a hit: none
  TEST_ASSERT_TRUE(d.read(10, buf, 4));  // bypassed: one read of 4
  d.setEnabled(false);
  TEST_ASSERT_TRUE(d.read(1, buf, 1));
  TEST_ASSERT_TRUE(d.write(30, buf, 2));
  d.setEnabled(true);
  TEST_ASSERT_TRUE(d.write(31, buf, 1));
  CachedDrive::Stats s = d.stats();
  TEST_ASSERT_EQUAL_UINT32(3, s.cardReads);
  TEST_ASSERT_EQUAL_UINT32(2, s.cardSingleReads);
  TEST_ASSERT_EQUAL_UINT32(6, s.cardReadSectors);
  TEST_ASSERT_EQUAL_UINT64(3 * 7, s.cardReadUs);
  TEST_ASSERT_EQUAL_UINT32(2, s.cardWrites);
  // A TRIM drops its range, first to last.
  TEST_ASSERT_TRUE(d.read(40, buf, 1));
  TEST_ASSERT_TRUE(d.read(41, buf, 1));
  TEST_ASSERT_TRUE(d.read(42, buf, 1));
  d.trim(40, 41);
  TEST_ASSERT_FALSE(c.holds(40));
  TEST_ASSERT_FALSE(c.holds(41));
  TEST_ASSERT_TRUE(c.holds(42));
  TEST_ASSERT_EQUAL_UINT32(1, d.stats().trims);
  // The reset: asked now, taken at the next call (the console prints the
  // counts before it, as the totals up to the switch).
  d.resetStats();
  TEST_ASSERT_EQUAL_UINT32(6, d.stats().cardReads);
  TEST_ASSERT_TRUE(c.stats().hits > 0);
  TEST_ASSERT_TRUE(d.read(42, buf, 1));  // a hit, after the reset
  s = d.stats();
  TEST_ASSERT_EQUAL_UINT32(0, s.cardReads);
  TEST_ASSERT_EQUAL_UINT32(0, s.trims);
  TEST_ASSERT_EQUAL_UINT32(0, s.inits);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().hits);
  TEST_ASSERT_EQUAL_UINT32(0, c.stats().misses);
}

// No block: every call to the card, nothing kept, the rules the same.
void test_drive_without_memory() {
  RamDevice dev;
  SectorCache c;
  CachedDrive d(c, dev);
  d.init();
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(2, buf, 1));
  TEST_ASSERT_TRUE(d.read(2, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(2, dev.reads);
  TEST_ASSERT_TRUE(d.write(2, buf, 1));
  d.trim(0, 100);
  TEST_ASSERT_EQUAL_UINT32(0, c.size());
}

// A write the card refused is written again once (sdbusy::kWriteTries: the
// device run's glitch, a status command sent to a card still busy, failed
// a write the card had taken): the same sectors and bytes, counted. Refused
// twice, it fails, and the cache drops what it held of them.
void test_drive_write_retried_once() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  d.init();
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(7, buf, 1));
  TEST_ASSERT_TRUE(c.holds(7));
  const Sector w = filled(0x77);
  dev.failWriteAfter = 0;  // refused once, nothing written
  const uint32_t writes = dev.writes;
  TEST_ASSERT_TRUE(d.write(7, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(writes + 2, dev.writes);
  TEST_ASSERT_TRUE(dev.equals(7, w.data()));
  TEST_ASSERT_TRUE(c.holds(7));  // refreshed: the second try was taken
  const uint32_t reads = dev.reads;
  TEST_ASSERT_TRUE(d.read(7, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(reads, dev.reads);
  TEST_ASSERT_EQUAL_MEMORY(w.data(), buf, kSS);
  CachedDrive::Stats s = d.stats();
  TEST_ASSERT_EQUAL_UINT32(2, s.cardWrites);
  TEST_ASSERT_EQUAL_UINT32(1, s.writeRetries);
  TEST_ASSERT_EQUAL_UINT32(0, s.writeFails);
  TEST_ASSERT_EQUAL_UINT32(0, c.stats().failedWrites);
  // A multi-sector write that failed half way: written again whole.
  std::vector<uint8_t> run(2 * kSS, 0x42);
  dev.failWriteAfter = 1;
  TEST_ASSERT_TRUE(d.write(20, run.data(), 2));
  TEST_ASSERT_TRUE(dev.equals(20, run.data()));
  TEST_ASSERT_TRUE(dev.equals(21, run.data() + kSS));
  // Refused twice: the write fails, the cache drops the sector.
  const Sector w2 = filled(0x78);
  dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(7, w2.data(), 1));
  TEST_ASSERT_FALSE(c.holds(7));
  TEST_ASSERT_TRUE(dev.equals(7, w.data()));
  s = d.stats();
  TEST_ASSERT_EQUAL_UINT32(3, s.writeRetries);
  TEST_ASSERT_EQUAL_UINT32(1, s.writeFails);
  TEST_ASSERT_EQUAL_UINT32(1, c.stats().failedWrites);
  // The cache off: the same rule.
  d.setEnabled(false);
  dev.failWrites = 1;
  TEST_ASSERT_TRUE(d.write(7, w2.data(), 1));
  TEST_ASSERT_TRUE(dev.equals(7, w2.data()));
  TEST_ASSERT_EQUAL_UINT32(4, d.stats().writeRetries);
  // Reads aren't tried again here (the SD driver does, three times).
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(30, buf, 1));
}

namespace {

void put32le(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// A card as a PC formats it: a partition table in sector 0 (a disk
// signature, the first entry FAT32 from LBA `boot`) and a FAT boot sector
// there with a volume serial (BS_VolID, offset 67).
constexpr uint32_t kCardSectors = 1u << 20;
void formatLike(RamDevice& dev, uint32_t boot, uint32_t serial, uint32_t signature = 0x5EED0001) {
  Sector mbr{};
  put32le(mbr.data() + 440, signature);
  mbr[446 + 4] = 0x0C;
  put32le(mbr.data() + 446 + 8, boot);
  put32le(mbr.data() + 446 + 12, kCardSectors - boot);
  mbr[510] = 0x55;
  mbr[511] = 0xAA;
  dev.sectors[0] = mbr;
  Sector vbr{};
  vbr[0] = 0xEB;
  vbr[1] = 0x58;
  vbr[2] = 0x90;
  vbr[12] = 0x02;  // 512 B a sector
  put32le(vbr.data() + 67, serial);
  vbr[510] = 0x55;
  vbr[511] = 0xAA;
  dev.sectors[boot] = vbr;
}

// The mount's card remembered, `change` done to the card in the slot, a
// remount at `sectors`: foreign?
bool foreignAfter(void (*change)(RamDevice&), uint32_t sectors = kCardSectors) {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  formatLike(dev, 8192, 0x1234ABCD);
  d.init();
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  change(dev);
  d.init();
  const bool same = d.remounted(sectors);
  TEST_ASSERT_EQUAL(!same, d.foreign());
  TEST_ASSERT_EQUAL_UINT32(1, d.remounts());
  return d.foreign();
}

}  // namespace

// The card's identity: its size, sector 0's CRC and its first partition's
// boot sector's. The same card (pulled and put back, or FatFs's own
// remount after a status glitch) passes; each part tells another card
// apart; a card whose identity can't be read is taken for another. FatFs's
// own writes (the FSINFO sector after the boot sector, the FAT, folders)
// change none of it.
void test_drive_identity() {
  TEST_ASSERT_FALSE(foreignAfter([](RamDevice&) {}));
  TEST_ASSERT_FALSE(foreignAfter([](RamDevice& d) {
    d.sectors[8193] = filled(0x46);  // FSINFO
    d.sectors[8200] = filled(0xFA);  // the FAT
    d.sectors[40000] = filled(0xD1);
  }));
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice& d) { put32le(d.sectors[8192].data() + 67, 0x1234ABCE); }));
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice& d) { put32le(d.sectors[0].data() + 440, 0x5EED0002); }));
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice& d) { formatLike(d, 2048, 0x1234ABCD); }));  // the partition moved
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice&) {}, kCardSectors / 2));                     // another size
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice& d) { d.sectors.clear(); }));                 // blank (no table)
  // One failed read of it is read again; two make it unreadable: another.
  TEST_ASSERT_FALSE(foreignAfter([](RamDevice& d) { d.failReads = 1; }));
  TEST_ASSERT_TRUE(foreignAfter([](RamDevice& d) { d.failReads = 2; }));
  // A card with no partition table (its boot sector at 0): sector 0 alone.
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  formatLike(dev, 8192, 0x0BADF00D);
  dev.sectors[0] = dev.sectors[8192];
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  TEST_ASSERT_EQUAL_UINT32(0, d.mountIdentity().bootLba);
  TEST_ASSERT_TRUE(d.mountIdentity().valid);
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  put32le(dev.sectors[0].data() + 67, 0x0BADF00E);
  TEST_ASSERT_FALSE(d.remounted(kCardSectors));
}

// Another card: every write refused (the card never sees one), reads
// served (FatFs mounts it; the restart reads it), until a restart: the
// mount's card put back is still refused. Remounts are counted.
void test_drive_foreign_card() {
  RamDevice dev;
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  // Nothing remembered (a model's first mount, before the firmware's
  // install()): a remount checks nothing.
  formatLike(dev, 8192, 0xA);
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_FALSE(d.armed());
  TEST_ASSERT_EQUAL_UINT32(0, d.remounts());
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  TEST_ASSERT_TRUE(d.armed());
  uint8_t buf[kSS];
  TEST_ASSERT_TRUE(d.read(8192, buf, 1));  // cached
  // Another card in the slot.
  const std::map<uint32_t, Sector> first = dev.sectors;
  dev.sectors.clear();
  formatLike(dev, 8192, 0xB);
  d.init();
  TEST_ASSERT_FALSE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.foreign());
  TEST_ASSERT_TRUE(d.read(8192, buf, 1));
  TEST_ASSERT_TRUE(dev.equals(8192, buf));  // the new card's (the cache was cleared)
  const uint32_t writes = dev.writes;
  const Sector w = filled(0x99);
  TEST_ASSERT_FALSE(d.write(8192, w.data(), 1));
  TEST_ASSERT_FALSE(d.write(100, w.data(), 1));
  d.setEnabled(false);
  TEST_ASSERT_FALSE(d.write(100, w.data(), 1));
  d.setEnabled(true);
  TEST_ASSERT_EQUAL_UINT32(writes, dev.writes);
  TEST_ASSERT_EQUAL_UINT32(3, d.stats().refused);
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().cardWrites);
  // The first card back: still foreign (a restart is coming).
  dev.sectors = first;
  d.init();
  TEST_ASSERT_FALSE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.foreign());
  TEST_ASSERT_EQUAL_UINT32(2, d.remounts());
  TEST_ASSERT_FALSE(d.write(100, w.data(), 1));
  // remember() (the next boot's mount) starts again.
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  TEST_ASSERT_FALSE(d.foreign());
  TEST_ASSERT_TRUE(d.write(100, w.data(), 1));
}

// No scratch (no PSRAM for it at the mount): no identity can be read, so
// none is known, and any remount is taken for another card (a restart
// reads the card again).
void test_drive_identity_without_scratch() {
  RamDevice dev;
  SectorCache c;
  CachedDrive d(c, dev);
  formatLike(dev, 8192, 0xC);
  d.init();
  TEST_ASSERT_FALSE(d.remember(kCardSectors));
  TEST_ASSERT_TRUE(d.armed());
  TEST_ASSERT_FALSE(d.mountIdentity().valid);
  TEST_ASSERT_FALSE(d.foreign());
  uint8_t buf[kSS] = {};
  TEST_ASSERT_TRUE(d.write(5, buf, 1));
  d.init();
  TEST_ASSERT_FALSE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.foreign());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hit_and_miss);
  RUN_TEST(test_eviction_is_lru);
  RUN_TEST(test_multi_sector_reads_bypass);
  RUN_TEST(test_writes_go_through);
  RUN_TEST(test_failed_write_drops);
  RUN_TEST(test_failed_read_keeps_nothing);
  RUN_TEST(test_invalidate_and_clear);
  RUN_TEST(test_no_memory_passes_through);
  RUN_TEST(test_block_sizes);
  RUN_TEST(test_random_against_reference);
  RUN_TEST(test_drive_mount_clears);
  RUN_TEST(test_drive_off_and_on);
  RUN_TEST(test_drive_verify);
  RUN_TEST(test_drive_counts_and_reset);
  RUN_TEST(test_drive_without_memory);
  RUN_TEST(test_drive_write_retried_once);
  RUN_TEST(test_drive_identity);
  RUN_TEST(test_drive_foreign_card);
  RUN_TEST(test_drive_identity_without_scratch);
  return UNITY_END();
}
