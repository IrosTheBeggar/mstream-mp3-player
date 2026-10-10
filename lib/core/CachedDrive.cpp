// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CachedDrive.h"

#include <cstring>

#include "SdBusy.h"

namespace {

// CRC-32/ISO-HDLC (zlib's), bit by bit: two sectors at a remount. Here
// rather than cardcontract::crc32() so the wrapper's rules stay on their
// own (tools/fatmodel.py builds them alone).
uint32_t crc32(const uint8_t* p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n--) {
    c ^= *p++;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

// A partition table in sector 0 (not a FAT boot sector, whose first byte
// is a jump): its first entry with a type and a start inside the card.
uint32_t firstPartition(const uint8_t* s, uint32_t sectors) {
  if (s[0] == 0xEB || s[0] == 0xE9 || s[0] == 0xE8) return 0;  // the volume's own boot sector
  if (s[510] != 0x55 || s[511] != 0xAA) return 0;
  for (int i = 0; i < 4; ++i) {
    const uint8_t* e = s + 446 + 16 * i;
    const uint32_t lba = le32(e + 8);
    if (e[4] != 0 && lba != 0 && (sectors == 0 || lba < sectors)) return lba;
  }
  return 0;
}

}  // namespace

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
  for (int tried = 1;; ++tried) {
    ++d_.stats_.cardWrites;
    if (d_.card_.write(lba, data, count)) return true;
    if (tried >= sdbusy::kWriteTries) break;
    ++d_.stats_.writeRetries;  // the same sectors again: a write the card took is only written twice
  }
  ++d_.stats_.writeFails;
  return false;
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
  if (foreign_.load()) {
    // Another card (or one that couldn't be told apart): nothing of this
    // session's goes on it. Nothing reached the card, so nothing the cache
    // holds is stale.
    ++stats_.refused;
    return false;
  }
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

CachedDrive::Identity CachedDrive::readIdentity(uint32_t sectors) {
  Identity id;
  id.sectors = sectors;
  // Each sector read twice at most: an unreadable identity counts as
  // another card (a restart), so one failed read shouldn't make one.
  auto readSector = [this](uint32_t lba) {
    return counted_.read(lba, check_, 1) || counted_.read(lba, check_, 1);
  };
  if (!check_ || !readSector(0)) return id;
  id.sector0Crc = crc32(check_, SectorCache::kSectorBytes);
  id.bootLba = firstPartition(check_, sectors);
  if (id.bootLba) {
    if (!readSector(id.bootLba)) return id;
    id.bootCrc = crc32(check_, SectorCache::kSectorBytes);
  }
  id.valid = true;
  return id;
}

bool CachedDrive::remember(uint32_t sectors) {
  mountId_ = readIdentity(sectors);
  lastId_ = mountId_;
  foreign_.store(false);
  armed_.store(true);
  return mountId_.valid;
}

bool CachedDrive::remounted(uint32_t sectors) {
  if (!armed_.load()) return true;
  if (!foreign_.load()) {  // (foreign until a restart, whatever comes back)
    lastId_ = readIdentity(sectors);
    if (!mountId_.sameCard(lastId_)) foreign_.store(true);
  }
  // Counted after the verdict: a task that sees the count sees the verdict
  // (the loop's poll reads the count first).
  remounts_.fetch_add(1);
  return !foreign_.load();
}

void CachedDrive::setEnabled(bool on) {
  // The clear before the switch (a call that sees it on sees the clear).
  if (on && !enabled_.load()) clear_.store(true);
  enabled_.store(on);
}
