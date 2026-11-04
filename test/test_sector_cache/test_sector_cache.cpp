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
// A command with no answer, and what the driver says after it.
namespace {
struct FakeLink : CachedDrive::Link {
  bool answersNow = true;  // the status: no mark, and CMD13 answered
  bool initWorks = true;   // a card answers its init
  uint32_t asks = 0, inits = 0;
  bool answers() override {
    ++asks;
    return answersNow;
  }
  bool init(uint32_t* sectors) override {
    ++inits;
    if (!initWorks) return false;
    answersNow = true;
    *sectors = kCardSectors;
    return true;
  }
};
}  // namespace

void test_drive_looks_after_a_fault_in_its_call() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  // A read that fails with the card answering (a CRC, no data token): nothing more.
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, link.asks);
  TEST_ASSERT_EQUAL_UINT32(0, link.inits);
  TEST_ASSERT_EQUAL_UINT32(0, d.kept());
  TEST_ASSERT_EQUAL_UINT32(100, d.stats().faultLba);
  // No answer: its init and its identity at once, FatFs told only of that read.
  dev.failReads = 1;
  link.answersNow = false;
  TEST_ASSERT_FALSE(d.read(101, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, link.inits);
  TEST_ASSERT_EQUAL_UINT32(1, d.kept());
  TEST_ASSERT_EQUAL_UINT32(0, d.remounts());
  TEST_ASSERT_FALSE(d.foreign());
  TEST_ASSERT_FALSE(d.remountDue());
  TEST_ASSERT_TRUE(d.read(101, buf, 1));  // the next one reads
  // The same for a write refused twice.
  link.answersNow = false;
  dev.failWrites = 2;
  const Sector w = filled(0x55);
  TEST_ASSERT_FALSE(d.write(102, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(2, d.kept());
  // A sector the cache holds stays held through a look.
  TEST_ASSERT_TRUE(d.read(50, buf, 1));
  link.answersNow = false;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(51, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(3, d.kept());
  const uint32_t reads = dev.reads;
  TEST_ASSERT_TRUE(d.read(50, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(reads, dev.reads);
  // The cache off: a read's and a write's the same.
  d.setEnabled(false);
  link.answersNow = false;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(52, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(4, d.kept());
  link.answersNow = false;
  dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(102, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(5, d.kept());
  d.setEnabled(true);
  // No look at a read that worked, nor a write.
  const uint32_t asks = link.asks;
  TEST_ASSERT_TRUE(d.read(103, buf, 1));
  TEST_ASSERT_TRUE(d.write(103, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(asks, link.asks);
}

// Nothing answers its init: nothing kept: the mark stands, for FatFs's remount and the guard. Another
// card at a look: no verdict there: remountDue(), and the remount gives it. No link, not armed, foreign:
// no look.
void test_drive_leaves_the_rest_to_the_remount() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  // Before the mount's identity: not armed, no look.
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(99, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(0, link.asks);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(200, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, link.inits);
  TEST_ASSERT_EQUAL_UINT32(0, d.kept());
  TEST_ASSERT_FALSE(d.remountDue());
  // Back: FatFs's remount.
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_EQUAL_UINT32(1, d.remounts());
  // Another card, seen at a look.
  link.answersNow = false;
  link.initWorks = true;
  dev.sectors.clear();
  formatLike(dev, 8192, 0xB);
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(300, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(0, d.kept());
  TEST_ASSERT_TRUE(d.remountDue());
  TEST_ASSERT_FALSE(d.foreign());
  const uint32_t inits = link.inits, asks = link.asks;
  link.answersNow = false;  // (the look's init said yes: no answer again, or answers() ends a second look by itself)
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(301, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(asks, link.asks);  // (due: no second look)
  TEST_ASSERT_EQUAL_UINT32(inits, link.inits);
  d.init();
  TEST_ASSERT_FALSE(d.remountDue());
  TEST_ASSERT_FALSE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.foreign());
  // Foreign: no look.
  link.answersNow = false;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(302, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(inits, link.inits);
}

// An identity the look can't read: no card kept: remountDue(), and the remount reads it again.
void test_drive_a_look_that_cant_read_the_identity() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  dev.failReads = 3;  // the read, and sector 0 twice
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, link.inits);
  TEST_ASSERT_EQUAL_UINT32(0, d.kept());
  TEST_ASSERT_TRUE(d.remountDue());
  TEST_ASSERT_FALSE(d.foreign());
  d.init();
  TEST_ASSERT_FALSE(d.remountDue());
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_FALSE(d.foreign());
}

// A size of 0 is the driver's CMD9 failing: the two sectors tell, whichever end it was. A card
// whose CMD9 always fails is that card; another card's sectors still differ.
void test_drive_identity_with_no_size() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  // At a remount.
  d.init();
  TEST_ASSERT_TRUE(d.remounted(0));
  TEST_ASSERT_FALSE(d.foreign());
  // At a look: FakeLink's size is kCardSectors: the mount's size of 0 this time.
  TEST_ASSERT_TRUE(d.remember(0));
  link.answersNow = false;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, d.kept());
  TEST_ASSERT_FALSE(d.remountDue());
  // Another card with no size: its sectors say so.
  dev.sectors.clear();
  formatLike(dev, 8192, 0xB);
  d.init();
  TEST_ASSERT_FALSE(d.remounted(0));
  TEST_ASSERT_TRUE(d.foreign());
  // Two sizes that are both known and differ are two cards, whatever the sectors say.
  CachedDrive::Identity a = d.mountIdentity(), b = a;
  a.sectors = kCardSectors;
  b.sectors = kCardSectors + 1;
  TEST_ASSERT_FALSE(a.sameCard(b));
  b.sectors = 0;
  TEST_ASSERT_TRUE(a.sameCard(b));
  TEST_ASSERT_TRUE(b.sameCard(a));
}

namespace {
// The remount a silent fault makes, and the mount's first read, which the card answers.
void mountAgain(CachedDrive& d, uint8_t* buf) {
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.read(8, buf, 2));
}
}  // namespace

// A read that keeps leaving the card silent past its init: kStopAfter times and it isn't asked again,
// whoever asks and through the remounts its faults make; the look neither. The rest of the card is asked
// as ever.
void test_drive_stops_asking_for_a_read_that_keeps_leaving_it_silent() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 8];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  const auto fail = [&](uint32_t lba, uint32_t count) {
    dev.failReads = 1;
    return d.read(lba, buf, count);
  };
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_FALSE(fail(100, 1));
    TEST_ASSERT_EQUAL_INT(0, dev.failReads);  // (asked of the card)
    TEST_ASSERT_EQUAL_UINT32(0, d.stats().stoppedReads);
    mountAgain(d, buf);  // (the count stands)
  }
  const uint32_t cardReads = d.stats().cardReads, asks = link.asks;
  // The fourth was the last: no read reaches the card, no look.
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(cardReads, d.stats().cardReads);
  TEST_ASSERT_EQUAL_UINT32(asks, link.asks);
  TEST_ASSERT_EQUAL_UINT32(2, d.stats().stoppedReads);
  // The cache off: the same.
  d.setEnabled(false);
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(cardReads, d.stats().cardReads);
  d.setEnabled(true);
  // The same sector in another read isn't that read; the sector beside it is asked.
  TEST_ASSERT_TRUE(d.read(100, buf, 2));
  TEST_ASSERT_TRUE(d.read(101, buf, 1));
  // A failure of another read that then works: its count goes, and it takes kStopAfter again.
  // (Two sectors: past the cache, whose hit would answer.)
  TEST_ASSERT_FALSE(fail(200, 2));
  mountAgain(d, buf);
  TEST_ASSERT_TRUE(d.read(200, buf, 2));
  for (uint32_t k = 0; k + 1 < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_FALSE(fail(200, 2));
    mountAgain(d, buf);
  }
  const uint32_t stopped = d.stats().stoppedReads;
  TEST_ASSERT_FALSE(fail(200, 2));
  TEST_ASSERT_EQUAL_UINT32(stopped, d.stats().stoppedReads);
  TEST_ASSERT_EQUAL_INT(0, dev.failReads);
  mountAgain(d, buf);
  // The fourth in a row: stopped now. A write of the sector, or one over it: asked again.
  TEST_ASSERT_FALSE(d.read(200, buf, 2));
  TEST_ASSERT_EQUAL_UINT32(stopped + 1, d.stats().stoppedReads);
  const Sector w = filled(0x5A);
  TEST_ASSERT_TRUE(d.write(200, w.data(), 1));
  TEST_ASSERT_TRUE(d.read(200, buf, 2));
  TEST_ASSERT_TRUE(dev.equals(200, buf));
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_FALSE(fail(300, 2));
    mountAgain(d, buf);
  }
  TEST_ASSERT_FALSE(d.read(300, buf, 2));
  uint8_t two[kSS * 2] = {};
  TEST_ASSERT_TRUE(d.write(301, two, 2));  // the second sector of it, and the one after
  TEST_ASSERT_TRUE(d.read(300, buf, 2));
  // The next boot's mount: a new CachedDrive: nothing kept (the session's RAM).
  CachedDrive next(c, dev);
  next.setScratch(scratch);
  TEST_ASSERT_TRUE(next.remember(kCardSectors));
  TEST_ASSERT_TRUE(next.read(100, buf, 1));
}

// A read that fails with the card answering (a CRC, no data token, an R1 error), or kept by the look, is
// the moment it costs and its reader's own tries: asked every time, as many as a shared directory's
// sector takes in a glitch's few milliseconds, and nothing it does touches the count.
void test_drive_never_stops_a_read_the_card_answered() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  const auto asked = [&](uint32_t lba) {
    dev.failReads = 1;
    const bool ok = d.read(lba, buf, 2);
    const bool reached = dev.failReads == 0;
    dev.failReads = 0;
    TEST_ASSERT_FALSE(ok);
    return reached;
  };
  for (uint32_t k = 0; k < 3 * CachedDrive::kStopAfter; ++k) TEST_ASSERT_TRUE(asked(100));
  // With no answer, and the card back at its init: the same.
  for (uint32_t k = 0; k < 3 * CachedDrive::kStopAfter; ++k) {
    link.answersNow = false;
    TEST_ASSERT_TRUE(asked(100));
  }
  TEST_ASSERT_EQUAL_UINT32(3 * CachedDrive::kStopAfter, d.kept());
  TEST_ASSERT_EQUAL_UINT32(0, d.stats().stoppedReads);
  TEST_ASSERT_FALSE(d.tooSilent());
  TEST_ASSERT_TRUE(d.read(100, buf, 2));  // and the next one reads
  // Between silent ones it starts nothing and ends nothing.
  for (uint32_t k = 0; k + 1 < CachedDrive::kStopAfter; ++k) {
    link.answersNow = false;
    link.initWorks = false;
    TEST_ASSERT_TRUE(asked(200));
    link.answersNow = true;
    link.initWorks = true;
    TEST_ASSERT_TRUE(asked(200));
  }
  TEST_ASSERT_TRUE(asked(200));
  link.answersNow = false;
  link.initWorks = false;
  TEST_ASSERT_TRUE(asked(200));
  TEST_ASSERT_FALSE(asked(200));
}

// A card put back (a remount with no look since the card last answered a read that found it silent: the
// first anyone saw of it was a status): the stops are asked again, and the silences count from none. A
// look that found it silent makes the next remount its own, until the card answers a read.
void test_drive_a_card_put_back_asks_again() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  const auto asked = [&](uint32_t lba) {
    dev.failReads = 1;
    const bool ok = d.read(lba, buf, 2);
    const bool reached = dev.failReads == 0;
    dev.failReads = 0;
    TEST_ASSERT_FALSE(ok);
    return reached;
  };
  const auto remount = [&] {
    d.init();
    TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  };
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_TRUE(asked(100));
    mountAgain(d, buf);
  }
  TEST_ASSERT_FALSE(asked(100));
  remount();  // put back
  TEST_ASSERT_TRUE(d.read(100, buf, 2));
  // The count from its start.
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_TRUE(asked(100));
    mountAgain(d, buf);
  }
  TEST_ASSERT_FALSE(asked(100));
  // A write's look counts as a read's for what a remount is.
  dev.failWrites = 2;
  const Sector w = filled(0x5A);
  TEST_ASSERT_FALSE(d.write(50, w.data(), 1));
  remount();
  TEST_ASSERT_FALSE(asked(100));
  // More than one with no read between (a write-mode call under STA_PROTECT mounts nothing): all its own.
  remount();
  remount();
  TEST_ASSERT_FALSE(asked(100));
  // A sector the cache holds isn't the card answering.
  TEST_ASSERT_TRUE(d.read(9, buf, 1));  // (the mount's first: on the card, and now in the cache)
  dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(51, w.data(), 1));
  TEST_ASSERT_TRUE(d.read(9, buf, 1));
  remount();
  TEST_ASSERT_FALSE(asked(100));
  // A remount with the card still silent at its init never gets here: the note waits for the one that does.
}

// A look that keeps the card, or a card that answers, leaves no note: the next remount is a card put back.
void test_drive_a_kept_look_leaves_no_note() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  const auto silent = [&](uint32_t lba) {
    link.answersNow = false;
    link.initWorks = false;
    dev.failReads = 1;
    TEST_ASSERT_FALSE(d.read(lba, buf, 2));
  };
  const auto stopHundred = [&] {
    for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
      silent(100);
      mountAgain(d, buf);
    }
  };
  stopHundred();
  silent(200);  // the note, and then a look that keeps the card
  link.initWorks = true;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(201, buf, 2));
  TEST_ASSERT_EQUAL_UINT32(1, d.kept());
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.read(100, buf, 2));
  stopHundred();
  silent(200);  // the note, and then a fault with the card answering
  link.answersNow = true;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(201, buf, 2));
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.read(100, buf, 2));
}

// A call of FatFs's that goes on past what it does without (FSINFO's read, the second FAT's write) with the
// mark standing: nothing reached the card: one silence, and the next read isn't counted on its sector.
void test_drive_a_read_with_the_mark_standing_counts_for_nothing() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  const auto asked = [&](uint32_t lba) {
    dev.failReads = 1;
    const bool ok = d.read(lba, buf, 2);
    const bool reached = dev.failReads == 0;
    dev.failReads = 0;
    TEST_ASSERT_FALSE(ok);
    return reached;
  };
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_TRUE(asked(64));    // FSINFO's: this one counts
    TEST_ASSERT_TRUE(asked(4191));  // the root's, the mark standing: these don't
    TEST_ASSERT_TRUE(asked(4191));
    mountAgain(d, buf);
  }
  TEST_ASSERT_FALSE(asked(64));
  TEST_ASSERT_TRUE(asked(4191));  // (not stopped: the card has answered nothing of it; this one's its first)
  mountAgain(d, buf);
  // And the silences are the firings: kStopAfter of FSINFO's and that one so far.
  for (uint32_t k = CachedDrive::kStopAfter + 1; k + 1 < CachedDrive::kTooSilentAfter; ++k) {
    TEST_ASSERT_TRUE(asked(5000 + 10 * k));
    TEST_ASSERT_TRUE(asked(4191));
    mountAgain(d, buf);
  }
  TEST_ASSERT_FALSE(d.tooSilent());
  TEST_ASSERT_TRUE(asked(6000));
  TEST_ASSERT_TRUE(d.tooSilent());
}

// An identity the look can't read: the card went silent after its init said yes (a select that timed out
// leaves no mark, so the init does nothing). The same cut, and the same read again after it: it counts.
void test_drive_an_identity_unread_is_a_silence() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    link.answersNow = false;
    dev.failReads = 3;  // the read, and sector 0 twice
    TEST_ASSERT_FALSE(d.read(100, buf, 2));
    TEST_ASSERT_EQUAL_INT(0, dev.failReads);
    TEST_ASSERT_TRUE(d.remountDue());
    TEST_ASSERT_EQUAL_UINT32(0, d.kept());
    mountAgain(d, buf);
    TEST_ASSERT_FALSE(d.foreign());
  }
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(100, buf, 2));
  TEST_ASSERT_EQUAL_INT(1, dev.failReads);  // (not asked)
  dev.failReads = 0;
  // Another card, read whole, is the guard's and no silence.
  TEST_ASSERT_FALSE(d.tooSilent());
}

// Reads that move on: the stop has nothing to hold. kTooSilentAfter silences, whichever reads and writes
// they were, and tooSilent() until the session ends; a card put back before that starts them over.
void test_drive_too_silent() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  const auto silence = [&](uint32_t lba) {
    dev.failReads = 1;
    TEST_ASSERT_FALSE(d.read(lba, buf, 2));
    mountAgain(d, buf);
  };
  for (uint32_t k = 0; k + 1 < CachedDrive::kTooSilentAfter; ++k) silence(1000 + 10 * k);
  TEST_ASSERT_FALSE(d.tooSilent());
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));  // put back
  for (uint32_t k = 0; k + 1 < CachedDrive::kTooSilentAfter; ++k) silence(2000 + 10 * k);
  TEST_ASSERT_FALSE(d.tooSilent());
  // A failure the card answered, or the look kept, isn't one.
  link.answersNow = true;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(3000, buf, 2));
  link.answersNow = false;
  link.initWorks = true;
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(3010, buf, 2));
  TEST_ASSERT_FALSE(d.tooSilent());
  // The last, a write's.
  link.answersNow = false;
  link.initWorks = false;
  dev.failWrites = 2;
  const Sector w = filled(0x5A);
  TEST_ASSERT_FALSE(d.write(3020, w.data(), 1));
  TEST_ASSERT_TRUE(d.tooSilent());
  mountAgain(d, buf);
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));  // put back: it stands
  TEST_ASSERT_TRUE(d.tooSilent());
  // The next boot's mount: nothing kept.
  CachedDrive next(c, dev);
  TEST_ASSERT_FALSE(next.tooSilent());
}

// A write that leaves the card silent, kStopAfter times: no reader's bound holds the queue's save, and each
// is a cut. No more writes until a restart; reads go on. With the card answering, or kept: no such count.
void test_drive_writes_that_leave_it_silent_stop() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  const Sector w = filled(0x5A);
  for (uint32_t k = 0; k < 3 * CachedDrive::kStopAfter; ++k) {
    dev.failWrites = 2;
    TEST_ASSERT_FALSE(d.write(700, w.data(), 1));  // (answering)
    link.answersNow = false;
    dev.failWrites = 2;
    TEST_ASSERT_FALSE(d.write(700, w.data(), 1));  // (kept)
  }
  TEST_ASSERT_FALSE(d.writesOff());
  link.answersNow = false;
  link.initWorks = false;
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
    TEST_ASSERT_FALSE(d.writesOff());
    dev.failWrites = 2;
    TEST_ASSERT_FALSE(d.write(700, w.data(), 1));
    mountAgain(d, buf);
  }
  TEST_ASSERT_TRUE(d.writesOff());
  const uint32_t writes = dev.writes;
  TEST_ASSERT_FALSE(d.write(701, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(writes, dev.writes);
  TEST_ASSERT_TRUE(d.read(701, buf, 2));
}

namespace {
uint64_t g_nowUs = 0;
uint64_t testClock() { return g_nowUs; }
}  // namespace

// With a clock: asked once more each kStopUs after the last, so a card that mends is read again. More
// reads than it keeps: the one longest since a failure goes.
void test_drive_asks_again_after_a_while() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  g_nowUs = 1000000;
  CachedDrive d(c, dev, testClock);
  uint8_t scratch[kSS], buf[kSS * 2];
  d.setScratch(scratch);
  FakeLink link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  link.answersNow = false;
  link.initWorks = false;
  // (Two sectors: past the cache, whose hit would answer.)
  const auto fail = [&](uint32_t lba) {
    dev.failReads = 1;
    const bool ok = d.read(lba, buf, 2);
    const bool asked = dev.failReads == 0;
    dev.failReads = 0;
    TEST_ASSERT_FALSE(ok);
    if (asked && !link.answersNow) mountAgain(d, buf);
    return asked;
  };
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) TEST_ASSERT_TRUE(fail(100));
  g_nowUs += CachedDrive::kStopUs - 1;
  TEST_ASSERT_FALSE(fail(100));
  g_nowUs += 1;
  TEST_ASSERT_TRUE(fail(100));  // once
  TEST_ASSERT_FALSE(fail(100));  // ... and the wait again, from that failure
  // Its wait over, and failing now with the card answering: asked each time, the wait not started again.
  g_nowUs += CachedDrive::kStopUs;
  link.answersNow = true;
  TEST_ASSERT_TRUE(fail(100));
  TEST_ASSERT_TRUE(fail(100));
  link.answersNow = false;
  TEST_ASSERT_TRUE(d.read(100, buf, 2));  // mended
  TEST_ASSERT_TRUE(fail(100));  // ... and the count from its start
  TEST_ASSERT_TRUE(fail(100));
  // Eviction: 100 has the oldest failure of nine reads, and the ninth takes its place.
  for (uint32_t lba = 500; lba < 508; ++lba) {
    g_nowUs += 1000;
    TEST_ASSERT_TRUE(fail(lba));
  }
  TEST_ASSERT_TRUE(fail(100));  // (it had two: it starts over)
  TEST_ASSERT_TRUE(fail(100));
  TEST_ASSERT_TRUE(fail(100));
  TEST_ASSERT_TRUE(fail(100));
  TEST_ASSERT_FALSE(fail(100));
}

namespace {
// A drive on a card silent past its init at every look, for the table's rules below.
struct Stopper {
  RamDevice dev;
  SectorCache c;
  FakeLink link;
  uint8_t scratch[kSS], buf[kSS * 8];
  void arm(CachedDrive& d) {
    formatLike(dev, 8192, 0xA);
    d.setScratch(scratch);
    d.setLink(&link);
    TEST_ASSERT_TRUE(d.remember(kCardSectors));
    link.answersNow = false;
    link.initWorks = false;
  }
};
// Asked of the card? (Armed for one failure, and whether it was taken.)
bool askedFailing(CachedDrive& d, RamDevice& dev, uint8_t* buf, uint32_t lba, uint32_t count) {
  dev.failReads = 1;
  const bool ok = d.read(lba, buf, count);
  const bool asked = dev.failReads == 0;
  dev.failReads = 0;
  TEST_ASSERT_FALSE(ok);
  return asked;
}
// (Asked and silent, and the remount that makes.)
bool silence(CachedDrive& d, RamDevice& dev, uint8_t* buf, uint32_t lba, uint32_t count) {
  const bool asked = askedFailing(d, dev, buf, lba, count);
  if (asked) mountAgain(d, buf);
  return asked;
}
void stop(CachedDrive& d, RamDevice& dev, uint8_t* buf, uint32_t lba, uint32_t count) {
  for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) TEST_ASSERT_TRUE(silence(d, dev, buf, lba, count));
  TEST_ASSERT_FALSE(askedFailing(d, dev, buf, lba, count));
}
}  // namespace

// A write beside a stopped read, either side, or anywhere else, leaves it stopped; a write the card
// refused says nothing either.
void test_drive_a_write_elsewhere_leaves_the_stop() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  CachedDrive d(s->c, s->dev);
  s->arm(d);
  const Sector w = filled(0x5A);
  stop(d, s->dev, s->buf, 300, 2);
  TEST_ASSERT_TRUE(d.write(299, w.data(), 1));  // just before
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));
  TEST_ASSERT_TRUE(d.write(302, w.data(), 1));  // just after
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));
  TEST_ASSERT_TRUE(d.write(1000, w.data(), 1));  // far after
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));
  TEST_ASSERT_TRUE(d.write(10, w.data(), 1));  // far before
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));
  s->dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(300, w.data(), 1));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));
  delete s;
}

// The table's holes: a read that works or a write over one takes its own entry, not the last, and one
// write over two takes both.
void test_drive_the_stops_are_kept_entry_by_entry() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  CachedDrive d(s->c, s->dev);
  s->arm(d);
  const Sector w = filled(0x5A);
  // A (one failure), B stopped; A works: B stays stopped.
  TEST_ASSERT_TRUE(silence(d, s->dev, s->buf, 400, 2));
  stop(d, s->dev, s->buf, 500, 2);
  TEST_ASSERT_TRUE(d.read(400, s->buf, 2));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 500, 2));
  // C and D stopped in the slots after B; a write over C leaves D and B.
  stop(d, s->dev, s->buf, 600, 2);
  stop(d, s->dev, s->buf, 700, 2);
  TEST_ASSERT_TRUE(d.write(600, w.data(), 1));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 700, 2));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 500, 2));
  TEST_ASSERT_TRUE(silence(d, s->dev, s->buf, 600, 2));
  // One write over two stopped reads: both asked again.
  stop(d, s->dev, s->buf, 800, 2);
  stop(d, s->dev, s->buf, 802, 2);
  uint8_t four[kSS * 4] = {};
  TEST_ASSERT_TRUE(d.write(800, four, 4));
  TEST_ASSERT_TRUE(askedFailing(d, s->dev, s->buf, 800, 2));
  TEST_ASSERT_TRUE(askedFailing(d, s->dev, s->buf, 802, 2));
  delete s;
}

// The same rules with the cache off: the failures count, a write ends the stop, a refused one doesn't.
void test_drive_the_stop_with_the_cache_off() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  CachedDrive d(s->c, s->dev);
  s->arm(d);
  d.setEnabled(false);
  const Sector w = filled(0x5A);
  stop(d, s->dev, s->buf, 100, 1);
  s->dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(100, w.data(), 1));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 100, 1));
  TEST_ASSERT_TRUE(d.write(100, w.data(), 1));
  TEST_ASSERT_TRUE(d.read(100, s->buf, 1));
  delete s;
}

// A read over the same sector with another count that works isn't that read: it stays stopped.
void test_drive_a_read_of_another_count_leaves_the_stop() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  CachedDrive d(s->c, s->dev);
  s->arm(d);
  stop(d, s->dev, s->buf, 100, 1);
  TEST_ASSERT_TRUE(d.read(100, s->buf, 2));
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 100, 1));
  delete s;
}

// Eight reads stay stopped at once; the ninth takes the place of the oldest, wherever it sits.
void test_drive_eight_reads_and_the_oldest_anywhere() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  g_nowUs = 1000000;
  CachedDrive d(s->c, s->dev, testClock);
  s->arm(d);
  for (uint32_t i = 0; i < 8; ++i) {
    for (uint32_t k = 0; k < CachedDrive::kStopAfter; ++k) {
      g_nowUs += 10;
      TEST_ASSERT_TRUE(silence(d, s->dev, s->buf, 100 + 10 * i, 2));
    }
  }
  for (uint32_t i = 0; i < 8; ++i) TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 100 + 10 * i, 2));
  // Slot 0's a stopped read too, and younger than slot 1's: 100 fails again after a wait, 110 doesn't.
  g_nowUs += CachedDrive::kStopUs;
  TEST_ASSERT_TRUE(silence(d, s->dev, s->buf, 100, 2));  // its wait over: once more
  g_nowUs += 10;
  TEST_ASSERT_TRUE(silence(d, s->dev, s->buf, 900, 2));  // the ninth: 110 goes, the oldest
  // 100 is still known, stopped.
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 100, 2));
  delete s;
}

// A failed window, and writes that aren't of that sector between: the count stands.
void test_drive_a_write_elsewhere_isnt_the_windows() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  const Sector w = filled(0x33);
  uint8_t two[kSS * 2] = {};
  d.windowFailed(703);
  TEST_ASSERT_TRUE(d.write(702, w.data(), 1));  // just before
  TEST_ASSERT_TRUE(d.write(704, w.data(), 1));  // just after
  TEST_ASSERT_TRUE(d.write(701, two, 2));       // ends just before
  TEST_ASSERT_TRUE(d.write(10, w.data(), 1));   // far before (a data sector, a FAT's)
  TEST_ASSERT_TRUE(d.write(5000, w.data(), 1));  // far after
  TEST_ASSERT_FALSE(d.remountDue());
  d.windowFailed(703);
  TEST_ASSERT_TRUE(d.remountDue());
}

// A look that finds another size: two cards, whatever the sectors say, and the size is the init's, not the mount's.
void test_drive_a_look_holds_the_init_size_to_the_mount() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  struct SizedLink : CachedDrive::Link {
    bool answers() override { return false; }
    bool init(uint32_t* sectors) override {
      *sectors = kCardSectors + 8;
      return true;
    }
  } link;
  d.setLink(&link);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(0, d.kept());
  TEST_ASSERT_TRUE(d.remountDue());
}

// faultLba: the last read's or write's, not the first's.
void test_drive_the_fault_sector_is_the_last() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(100, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(100, d.stats().faultLba);
  dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(101, buf, 1));
  TEST_ASSERT_EQUAL_UINT32(101, d.stats().faultLba);
  const Sector w = filled(0x33);
  dev.failWrites = 2;
  TEST_ASSERT_FALSE(d.write(102, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(102, d.stats().faultLba);
}

// A remount between a window's first failure and the same sector's next: the rule is no write of it
// taken, so it counts all the same.
void test_drive_a_remount_doesnt_forget_a_windows_first_failure() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS];
  d.setScratch(scratch);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  d.windowFailed(703);
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  d.windowFailed(703);
  TEST_ASSERT_TRUE(d.remountDue());
}

// gc's reset is the counts' alone: the stop, the silences, the window's count and kept stay.
void test_drive_gcs_reset_leaves_the_rules() {
  auto* s = new Stopper();
  TEST_ASSERT_TRUE(s->c.begin(8));
  CachedDrive d(s->c, s->dev);
  s->arm(d);
  s->link.initWorks = true;
  s->dev.failReads = 1;
  TEST_ASSERT_FALSE(d.read(90, s->buf, 1));
  TEST_ASSERT_EQUAL_UINT32(1, d.kept());
  s->link.answersNow = false;
  s->link.initWorks = false;
  stop(d, s->dev, s->buf, 300, 2);
  d.windowFailed(700);
  d.windowFailed(700);
  d.resetStats();
  TEST_ASSERT_FALSE(askedFailing(d, s->dev, s->buf, 300, 2));  // the next disk call takes the reset
  TEST_ASSERT_EQUAL_UINT32(1, d.kept());
  d.windowFailed(701);
  d.windowFailed(701);
  TEST_ASSERT_TRUE(d.writesOff());  // the second drop, the reset between
  delete s;
}

// FatFs's window that won't write. Once: nothing: FatFs writes it again. The same sector again with no
// write of it taken: a remount; a write taken between: from the start; another sector: from the start.
// The second such remount, and no more writes: reads go on, and the next boot asks again.
void test_drive_a_window_that_wont_write() {
  RamDevice dev;
  formatLike(dev, 8192, 0xA);
  SectorCache c;
  TEST_ASSERT_TRUE(c.begin(8));
  CachedDrive d(c, dev);
  uint8_t scratch[kSS], buf[kSS];
  d.setScratch(scratch);
  TEST_ASSERT_TRUE(d.remember(kCardSectors));
  const Sector w = filled(0x33);
  d.windowFailed(700);
  TEST_ASSERT_FALSE(d.remountDue());
  TEST_ASSERT_TRUE(d.write(700, w.data(), 1));  // the one-off: taken
  d.windowFailed(700);
  TEST_ASSERT_FALSE(d.remountDue());
  d.windowFailed(701);  // another sector: from the start
  TEST_ASSERT_FALSE(d.remountDue());
  d.windowFailed(701);
  TEST_ASSERT_TRUE(d.remountDue());
  TEST_ASSERT_FALSE(d.writesOff());
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_FALSE(d.remountDue());
  TEST_ASSERT_TRUE(d.write(702, w.data(), 1));  // writes go on
  // A write over it, of several sectors, counts too.
  d.windowFailed(703);
  uint8_t two[kSS * 2] = {};
  TEST_ASSERT_TRUE(d.write(702, two, 2));
  d.windowFailed(703);
  TEST_ASSERT_FALSE(d.remountDue());
  d.windowFailed(703);
  TEST_ASSERT_TRUE(d.remountDue());
  TEST_ASSERT_TRUE(d.writesOff());
  const uint32_t writes = dev.writes;
  TEST_ASSERT_FALSE(d.write(704, w.data(), 1));
  TEST_ASSERT_EQUAL_UINT32(writes, dev.writes);
  TEST_ASSERT_EQUAL_UINT32(1, d.stats().refused);
  d.init();
  TEST_ASSERT_TRUE(d.remounted(kCardSectors));
  TEST_ASSERT_TRUE(d.writesOff());
  TEST_ASSERT_TRUE(d.read(704, buf, 1));
  // The next boot's mount: a new CachedDrive: nothing kept.
  CachedDrive next(c, dev);
  next.setScratch(scratch);
  TEST_ASSERT_TRUE(next.remember(kCardSectors));
  TEST_ASSERT_TRUE(next.write(704, w.data(), 1));
}

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
  RUN_TEST(test_drive_looks_after_a_fault_in_its_call);
  RUN_TEST(test_drive_leaves_the_rest_to_the_remount);
  RUN_TEST(test_drive_a_look_that_cant_read_the_identity);
  RUN_TEST(test_drive_identity_with_no_size);
  RUN_TEST(test_drive_stops_asking_for_a_read_that_keeps_leaving_it_silent);
  RUN_TEST(test_drive_never_stops_a_read_the_card_answered);
  RUN_TEST(test_drive_a_card_put_back_asks_again);
  RUN_TEST(test_drive_a_kept_look_leaves_no_note);
  RUN_TEST(test_drive_a_read_with_the_mark_standing_counts_for_nothing);
  RUN_TEST(test_drive_an_identity_unread_is_a_silence);
  RUN_TEST(test_drive_too_silent);
  RUN_TEST(test_drive_writes_that_leave_it_silent_stop);
  RUN_TEST(test_drive_asks_again_after_a_while);
  RUN_TEST(test_drive_a_write_elsewhere_leaves_the_stop);
  RUN_TEST(test_drive_the_stops_are_kept_entry_by_entry);
  RUN_TEST(test_drive_the_stop_with_the_cache_off);
  RUN_TEST(test_drive_a_read_of_another_count_leaves_the_stop);
  RUN_TEST(test_drive_eight_reads_and_the_oldest_anywhere);
  RUN_TEST(test_drive_a_write_elsewhere_isnt_the_windows);
  RUN_TEST(test_drive_a_look_holds_the_init_size_to_the_mount);
  RUN_TEST(test_drive_the_fault_sector_is_the_last);
  RUN_TEST(test_drive_a_remount_doesnt_forget_a_windows_first_failure);
  RUN_TEST(test_drive_gcs_reset_leaves_the_rules);
  RUN_TEST(test_drive_a_window_that_wont_write);
  RUN_TEST(test_drive_foreign_card);
  RUN_TEST(test_drive_identity_without_scratch);
  return UNITY_END();
}
