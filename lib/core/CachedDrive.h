// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "SectorCache.h"

// The diskio wrapper's rules over SectorCache (docs/METADATA.md 3.2.4, 3.8;
// milestone N10 and its review): what FatFs's disk calls do to the cache,
// and the device batch's switches. The firmware's storage/SectorDisk
// adapts it to ESP-IDF's ff_diskio_impl_t over the SD driver; the host's
// FatFs model (test/support/FatModel.h) runs ChaN's FatFs on it.
// Portable, host-tested (test_sector_cache, test_fat_model).
//
//   - init() (disk_initialize): FatFs mounts the volume again. It does at
//     the mount, and by itself whenever disk_status() says STA_NOINIT: the
//     card stopped answering (pulled, or swapped, while the player was on:
//     the Core2 has no card-detect), so the next FatFs call finds a card
//     that may not be the one the cache read. The cache is cleared:
//     nothing of the card that was there is kept, not even its boot sector.
//   - read(): single sectors through the cache when it is on, every read to
//     the card when it is off; with verify, each hit is read from the card
//     again and compared (a difference is counted, its LBA kept, the
//     card's bytes returned and the sector dropped).
//   - write(): through the cache when it is on; when it is off, straight to
//     the card, and the sectors it wrote are dropped from the cache. So the
//     cache never holds a byte the card doesn't, on or off, and turning it
//     on again can't serve a sector written while it was off.
//   - trim() (CTRL_TRIM: FatFs frees clusters): the range dropped.
//   - setEnabled(), setVerify() and resetStats() come from another task
//     (the console): each is taken at the next disk call, under FatFs's
//     volume lock, which serialises the cache (it has no lock of its own).
//     A call reads the switch before it takes the pending requests, and
//     setEnabled(true) stores its clear before the switch, so a call that
//     sees the cache on has seen the clear asked with it. The cache starts
//     empty when it is turned on again (a cold start for L0's figures).
//
// Every read and write that reaches the card is counted (on or off), with
// the reads' time by the clock given (none on the host: 0).
class CachedDrive {
public:
  struct Stats {
    uint32_t cardReads = 0;        // reads that reached the card (the cache on or off)
    uint32_t cardSingleReads = 0;  // ... of one sector (FatFs's folders and FAT, a partial sector)
    uint32_t cardReadSectors = 0;
    uint64_t cardReadUs = 0;  // their time (the clock's; 0 without one)
    uint32_t cardWrites = 0;
    uint32_t trims = 0;
    uint32_t inits = 0;     // mounts: the cache cleared
    uint32_t verified = 0;  // hits compared with the card (verify on)
    uint32_t stale = 0;     // ... that differed: a cache bug (L1 fails)
    uint32_t staleLba = 0;  // the last such sector
  };
  using ClockFn = uint64_t (*)();  // microseconds

  // `cache` (begin()'d or not: with no block every call goes to the card)
  // over `card`, both outliving this.
  CachedDrive(SectorCache& cache, SectorCache::Device& card, ClockFn clock = nullptr);
  CachedDrive(const CachedDrive&) = delete;
  CachedDrive& operator=(const CachedDrive&) = delete;

  // Verify's sector (512 B, the caller's: PSRAM on the device); nullptr:
  // verify does nothing.
  void setScratch(uint8_t* sector) { check_ = sector; }

  // ---- FatFs's calls, under its volume's lock ----
  void init();
  bool read(uint32_t lba, uint8_t* out, uint32_t count);
  bool write(uint32_t lba, const uint8_t* data, uint32_t count);
  // FatFs's CTRL_TRIM range: the first and the last sector freed.
  void trim(uint32_t first, uint32_t last);

  // ---- any task ----
  void setEnabled(bool on);
  bool enabled() const { return enabled_.load(); }
  void setVerify(bool on) { verify_.store(on); }
  bool verifying() const { return verify_.load(); }
  // The counts (the cache's too) start again at the next disk call.
  void resetStats() { reset_.store(true); }
  // A copy, read while FatFs may be inside a call on another task: each
  // field a word, read as it is (a diagnostic).
  Stats stats() const { return stats_; }

private:
  // The card, counted (and timed).
  class Card final : public SectorCache::Device {
  public:
    explicit Card(CachedDrive& d) : d_(d) {}
    bool read(uint32_t lba, uint8_t* out, uint32_t count) override;
    bool write(uint32_t lba, const uint8_t* data, uint32_t count) override;

  private:
    CachedDrive& d_;
  };
  // The requests from other tasks (the stats' reset, the clear).
  void settle();

  SectorCache& cache_;
  SectorCache::Device& card_;
  ClockFn clock_;
  Card counted_{*this};
  uint8_t* check_ = nullptr;
  std::atomic<bool> enabled_{true};
  std::atomic<bool> clear_{false};
  std::atomic<bool> reset_{false};
  std::atomic<bool> verify_{false};
  Stats stats_;
};
