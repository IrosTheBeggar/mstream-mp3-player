// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for SectorCache (docs/METADATA.md 3.2.4, row N8 of 6.1): hits
// and misses, the LRU's order and eviction, the bypass of multi-sector
// reads, writes through to the device (a cached sector refreshed, a failed
// write's sectors dropped, nothing added), a failed read that keeps
// nothing, invalidate() over short and long ranges and at the top of the
// LBA space, clear(), no memory (everything passes through), the block's
// size, and a random model check against a reference LRU and the device's
// own bytes. FatFs over the cache is test_fat_model's. Run:
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

#include "SectorCache.h"

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kSS = SectorCache::kSectorBytes;
using Sector = std::array<uint8_t, kSS>;

// A sparse device: a sector never written reads as a pattern of its LBA.
// Faults on demand: the next `failReads` reads fail; the next write fails
// after writing its first `failWriteAfter` sectors (-1: no fault).
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
  return UNITY_END();
}
