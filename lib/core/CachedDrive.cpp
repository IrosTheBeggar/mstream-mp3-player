// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CachedDrive.h"

#include <cstring>

CachedDrive::CachedDrive(SectorCache& cache, SectorCache::Device& card, ClockFn clock)
    : cache_(cache), card_(card), clock_(clock) {}

bool CachedDrive::Card::read(uint32_t lba, uint8_t* out, uint32_t count) {
  const uint64_t t0 = d_.clock_ ? d_.clock_() : 0;
  const bool ok = d_.card_.read(lba, out, count);
  if (d_.clock_) d_.stats_.cardReadUs += d_.clock_() - t0;
  ++d_.stats_.cardReads;
  if (count == 1) ++d_.stats_.cardSingleReads;
  d_.stats_.cardReadSectors += count;
  return ok;
}

bool CachedDrive::Card::write(uint32_t lba, const uint8_t* data, uint32_t count) {
  ++d_.stats_.cardWrites;
  return d_.card_.write(lba, data, count);
}

void CachedDrive::settle() {
  if (reset_.exchange(false)) {
    cache_.resetStats();
    stats_ = Stats();
  }
  if (clear_.exchange(false)) cache_.clear();
}

void CachedDrive::init() {
  settle();
  cache_.clear();  // whatever card is there now, none of the last one's sectors
  ++stats_.inits;
}

bool CachedDrive::read(uint32_t lba, uint8_t* out, uint32_t count) {
  const bool on = enabled_.load();  // before settle(): see the class comment
  settle();
  if (!on) return counted_.read(lba, out, count);
  const bool check = verify_.load() && check_ && count == 1 && cache_.holds(lba);
  if (!cache_.read(counted_, lba, out, count)) return false;
  if (check && counted_.read(lba, check_, 1)) {
    ++stats_.verified;
    if (std::memcmp(check_, out, SectorCache::kSectorBytes) != 0) {
      ++stats_.stale;
      stats_.staleLba = lba;
      std::memcpy(out, check_, SectorCache::kSectorBytes);  // the card's bytes win
      cache_.invalidate(lba, 1);
    }
  }
  return true;
}

bool CachedDrive::write(uint32_t lba, const uint8_t* data, uint32_t count) {
  const bool on = enabled_.load();
  settle();
  if (on) return cache_.write(counted_, lba, data, count);
  // Off: the card alone, and what the cache held of these sectors is
  // dropped (it would be stale when the cache comes on again).
  const bool ok = counted_.write(lba, data, count);
  cache_.invalidate(lba, count);
  return ok;
}

void CachedDrive::trim(uint32_t first, uint32_t last) {
  settle();
  if (last >= first) cache_.invalidate(first, last - first + 1);
  ++stats_.trims;
}

void CachedDrive::setEnabled(bool on) {
  // The clear before the switch (a call that sees it on sees the clear).
  if (on && !enabled_.load()) clear_.store(true);
  enabled_.store(on);
}
