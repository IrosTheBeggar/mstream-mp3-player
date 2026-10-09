// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// A cache of single 512-byte card sectors under FatFs, in PSRAM
// (docs/METADATA.md 3.2.4, row N8 of 6.1). The firmware's FatFs has no
// relative paths (FF_FS_RPATH 0) and the SD driver caches nothing, so
// every open, stat and opendir reads the path's directories again from
// the root, one sector per disk_read: about 56 sectors per lookup on the
// user's card, 51 of them in a /music of 705 artists. Those sectors, and
// the FAT's, are what this keeps.
//
// - Single-sector reads are cached: FatFs reads its directories and
//   its FAT one sector at a time (move_window), and so does a file's
//   partial sector. A hit is the most recently used; a miss is read from
//   the device and kept, the least recently used sector giving up its
//   slot when the cache is full. A failed read keeps nothing and moves
//   nothing.
// - Multi-sector reads bypass it (file data: a decoder's 4 KB reads,
//   the scanner's heads): they go straight to the device in one call, and
//   leave the cache as it was (nothing is written behind its back, so
//   what it holds is still the card's).
// - Writes go through, all of them, to the device first. Each cached
//   sector a write covers takes the written bytes once the device has
//   taken the write, and is dropped when the write failed (what a failed
//   write left on the card is unknown). A write never adds a sector the
//   cache didn't hold, and never changes the recency.
// - invalidate() drops a range (a TRIM, or anything that wrote the
//   card around the cache); clear() drops everything: at every mount,
//   before the wrapper goes in (N10).
//
// So the cache never holds a byte the card doesn't: the host tests check
// it against a reference model under random reads, writes, failures and
// invalidations (test_sector_cache), and under FatFs itself on a RAM disk
// (test_fat_model: the same operations through the cache and without it
// leave the same image).
//
// One block from the caller's allocator (PSRAM on the device): the sectors
// (512 B each), then 12 B of links per sector and a hash of 2-byte heads
// (a power of two, at least twice the sectors): 256 sectors are 135,168 B.
// No block (no memory, or begin() not called): every call goes to the
// device, and nothing is kept.
//
// Not thread-safe: FatFs calls its disk under the volume's lock (one
// volume on the card), which serialises the cache too. Portable,
// host-tested; the diskio wrapper that installs it is the firmware's (N10).
class SectorCache {
public:
  static constexpr uint32_t kSectorBytes = 512;
  // 128 KB of sectors. An LRU smaller than /music falls off a cliff: a
  // lookup scans /music from its first sector, so each one pushes out the
  // sectors the next needs. The user's /music is 102 sectors (705
  // artists); on tools/synthcard.py's card (104) tools/fatmodel.py
  // measured 41.7 card reads a random open at 64 sectors, 4.8 at 128 (p99
  // 27) and 3.5 at 256 (p99 8), against 58 without the cache (3.2.7).
  // 256 keeps the knee at about 1,500 artists, not 750.
  static constexpr uint32_t kDefaultEntries = 256;
  // Links are 16-bit: at most 4,096 sectors (2 MB).
  static constexpr uint32_t kMaxEntries = 4096;

  // The card under the cache: the SD driver's sector reads and writes
  // (the firmware's diskio wrapper), a RAM disk in the tests. `count`
  // sectors at `lba`; false: the transfer failed.
  class Device {
  public:
    virtual bool read(uint32_t lba, uint8_t* out, uint32_t count) = 0;
    virtual bool write(uint32_t lba, const uint8_t* data, uint32_t count) = 0;

  protected:
    ~Device() = default;
  };

  struct Stats {
    uint32_t hits = 0;             // single-sector reads served from the cache
    uint32_t misses = 0;           // single-sector reads the device served
    uint32_t bypassed = 0;         // multi-sector reads (straight to the device)
    uint32_t bypassedSectors = 0;  // ... and their sectors
    uint32_t failedReads = 0;      // device reads that failed (nothing kept)
    uint32_t writes = 0;           // write calls (every one goes through)
    uint32_t writtenSectors = 0;
    uint32_t failedWrites = 0;
    uint32_t updated = 0;   // cached sectors a write refreshed
    uint32_t dropped = 0;   // cached sectors a failed write or invalidate() dropped
    uint32_t evicted = 0;   // least recently used sectors given up for a new one
    uint32_t deviceReads() const { return misses + bypassed; }
  };

  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  // The block for `entries` sectors (0 when out of range).
  static size_t blockBytes(uint32_t entries);

  SectorCache() = default;
  ~SectorCache();
  SectorCache(const SectorCache&) = delete;
  SectorCache& operator=(const SectorCache&) = delete;

  // `entries` sectors (1 .. kMaxEntries) from `alloc` (freed by `release`;
  // null hooks: the heap), empty. False: out of range or no memory (the
  // cache then keeps nothing: every call goes to the device). The stats
  // are kept.
  bool begin(uint32_t entries, AllocFn alloc = nullptr, FreeFn release = nullptr);
  // The block back; every call goes to the device until the next begin().
  void end();
  uint32_t capacity() const { return cap_; }
  uint32_t size() const { return n_; }
  size_t bytes() const { return cap_ ? blockBytes(cap_) : 0; }

  // FatFs's disk_read and disk_write, through the cache (the class
  // comment). `count` 0 does nothing and succeeds.
  bool read(Device& dev, uint32_t lba, uint8_t* out, uint32_t count);
  bool write(Device& dev, uint32_t lba, const uint8_t* data, uint32_t count);

  // Drops the cached sectors in [lba, lba + count) (the range ends at the
  // last LBA if it would pass it).
  void invalidate(uint32_t lba, uint32_t count);
  // Drops everything (a mount).
  void clear();

  bool holds(uint32_t lba) const { return find(lba) != kNone; }
  // The cached LBAs from the most recently used, at most `max` of them, into
  // `out`; returns how many (the tests' view of the order).
  uint32_t order(uint32_t* out, uint32_t max) const;

  const Stats& stats() const { return stats_; }
  void resetStats() { stats_ = Stats(); }

private:
  static constexpr uint16_t kNone = 0xFFFF;
  struct Slot {
    uint32_t lba;
    uint16_t prev, next;  // the recency list (prev: more recent); `next` links the free list too
    uint16_t chain;       // the hash bucket's next slot
    uint16_t pad;
  };
  static_assert(sizeof(Slot) == 12, "the class comment quotes 12 B of links per sector");
  static uint32_t bucketsFor(uint32_t entries);

  uint16_t find(uint32_t lba) const;
  uint32_t bucket(uint32_t lba) const { return (lba * 0x9E3779B1u) >> shift_; }
  uint8_t* sector(uint16_t i) const { return data_ + static_cast<size_t>(i) * kSectorBytes; }
  void unlink(uint16_t i);
  void pushFront(uint16_t i);
  void unhash(uint16_t i);
  void remove(uint16_t i);
  void insert(uint32_t lba, const uint8_t* bytes);
  // Each cached sector in [lba, lba + count): refreshed from `data` (non-null)
  // or dropped. How many.
  uint32_t settle(uint32_t lba, uint32_t count, const uint8_t* data);

  AllocFn alloc_ = nullptr;
  FreeFn free_ = nullptr;
  uint8_t* data_ = nullptr;  // the block: the sectors, then the slots, then the buckets
  Slot* slots_ = nullptr;
  uint16_t* buckets_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t n_ = 0;
  uint32_t shift_ = 31;
  uint16_t head_ = kNone;  // the most recently used
  uint16_t tail_ = kNone;  // the least recently used
  uint16_t free_list_ = kNone;
  Stats stats_;
};
