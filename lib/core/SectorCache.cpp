// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SectorCache.h"

#include <cstdlib>
#include <cstring>

namespace {

void* heapAlloc(size_t n) { return std::malloc(n); }
void heapFree(void* p) { std::free(p); }

}  // namespace

uint32_t SectorCache::bucketsFor(uint32_t entries) {
  uint32_t nb = 2;
  while (nb < 2 * entries) nb <<= 1;
  return nb;
}

size_t SectorCache::blockBytes(uint32_t entries) {
  if (entries == 0 || entries > kMaxEntries) return 0;
  return static_cast<size_t>(entries) * (kSectorBytes + sizeof(Slot)) + bucketsFor(entries) * sizeof(uint16_t);
}

SectorCache::~SectorCache() { end(); }

void SectorCache::end() {
  if (data_ && free_) free_(data_);
  data_ = nullptr;
  slots_ = nullptr;
  buckets_ = nullptr;
  cap_ = n_ = 0;
  head_ = tail_ = free_list_ = kNone;
}

bool SectorCache::begin(uint32_t entries, AllocFn alloc, FreeFn release) {
  end();
  alloc_ = alloc ? alloc : heapAlloc;
  free_ = release ? release : heapFree;
  const size_t bytes = blockBytes(entries);
  if (bytes == 0) return false;
  void* p = alloc_(bytes);
  if (!p) return false;
  data_ = static_cast<uint8_t*>(p);
  slots_ = reinterpret_cast<Slot*>(data_ + static_cast<size_t>(entries) * kSectorBytes);
  buckets_ = reinterpret_cast<uint16_t*>(slots_ + entries);
  cap_ = entries;
  uint32_t bits = 0;
  while ((1u << bits) < bucketsFor(entries)) ++bits;
  shift_ = 32 - bits;
  clear();
  return true;
}

void SectorCache::clear() {
  n_ = 0;
  head_ = tail_ = kNone;
  free_list_ = cap_ ? 0 : kNone;
  for (uint32_t i = 0; i < cap_; ++i) {
    slots_[i].lba = 0;
    slots_[i].prev = kNone;
    slots_[i].next = i + 1 < cap_ ? static_cast<uint16_t>(i + 1) : kNone;
    slots_[i].chain = kNone;
    slots_[i].pad = 0;
  }
  if (buckets_) {
    const uint32_t nb = bucketsFor(cap_);
    for (uint32_t b = 0; b < nb; ++b) buckets_[b] = kNone;
  }
}

uint16_t SectorCache::find(uint32_t lba) const {
  if (n_ == 0) return kNone;
  for (uint16_t i = buckets_[bucket(lba)]; i != kNone; i = slots_[i].chain)
    if (slots_[i].lba == lba) return i;
  return kNone;
}

void SectorCache::unlink(uint16_t i) {
  Slot& s = slots_[i];
  if (s.prev != kNone) slots_[s.prev].next = s.next;
  else head_ = s.next;
  if (s.next != kNone) slots_[s.next].prev = s.prev;
  else tail_ = s.prev;
  s.prev = s.next = kNone;
}

void SectorCache::pushFront(uint16_t i) {
  Slot& s = slots_[i];
  s.prev = kNone;
  s.next = head_;
  if (head_ != kNone) slots_[head_].prev = i;
  head_ = i;
  if (tail_ == kNone) tail_ = i;
}

void SectorCache::unhash(uint16_t i) {
  uint16_t* link = &buckets_[bucket(slots_[i].lba)];
  while (*link != i) link = &slots_[*link].chain;
  *link = slots_[i].chain;
  slots_[i].chain = kNone;
}

void SectorCache::remove(uint16_t i) {
  unhash(i);
  unlink(i);
  slots_[i].next = free_list_;
  free_list_ = i;
  --n_;
}

void SectorCache::insert(uint32_t lba, const uint8_t* bytes) {
  uint16_t i = free_list_;
  if (i != kNone) {
    free_list_ = slots_[i].next;
  } else {
    i = tail_;
    unhash(i);
    unlink(i);
    --n_;
    ++stats_.evicted;
  }
  Slot& s = slots_[i];
  s.lba = lba;
  const uint32_t b = bucket(lba);
  s.chain = buckets_[b];
  buckets_[b] = i;
  pushFront(i);
  ++n_;
  std::memcpy(sector(i), bytes, kSectorBytes);
}

uint32_t SectorCache::settle(uint32_t lba, uint32_t count, const uint8_t* data) {
  if (n_ == 0 || count == 0) return 0;
  const uint64_t end = static_cast<uint64_t>(lba) + count;  // past the last LBA: no wrap
  uint32_t k = 0;
  if (count <= n_) {
    // A short range: each of its sectors looked up.
    for (uint64_t s = lba; s < end && s <= 0xFFFFFFFFull; ++s) {
      const uint16_t i = find(static_cast<uint32_t>(s));
      if (i == kNone) continue;
      if (data) std::memcpy(sector(i), data + (s - lba) * kSectorBytes, kSectorBytes);
      else remove(i);
      ++k;
    }
  } else {
    // A long one (a TRIM, a big write): the cached sectors looked at.
    for (uint16_t i = head_; i != kNone;) {
      const uint16_t next = slots_[i].next;
      const uint32_t s = slots_[i].lba;
      if (s >= lba && s < end) {
        if (data) std::memcpy(sector(i), data + static_cast<size_t>(s - lba) * kSectorBytes, kSectorBytes);
        else remove(i);
        ++k;
      }
      i = next;
    }
  }
  return k;
}

bool SectorCache::read(Device& dev, uint32_t lba, uint8_t* out, uint32_t count) {
  if (count == 0) return true;
  if (count > 1) {
    ++stats_.bypassed;
    stats_.bypassedSectors += count;
    if (dev.read(lba, out, count)) return true;
    ++stats_.failedReads;
    return false;
  }
  const uint16_t i = find(lba);
  if (i != kNone) {
    ++stats_.hits;
    std::memcpy(out, sector(i), kSectorBytes);
    if (head_ != i) {
      unlink(i);
      pushFront(i);
    }
    return true;
  }
  ++stats_.misses;
  // Into the caller's buffer first: a failed read costs the cache nothing.
  if (!dev.read(lba, out, 1)) {
    ++stats_.failedReads;
    return false;
  }
  if (cap_) insert(lba, out);
  return true;
}

bool SectorCache::write(Device& dev, uint32_t lba, const uint8_t* data, uint32_t count) {
  if (count == 0) return true;
  ++stats_.writes;
  stats_.writtenSectors += count;
  const bool ok = dev.write(lba, data, count);
  if (ok) {
    stats_.updated += settle(lba, count, data);
  } else {
    ++stats_.failedWrites;
    stats_.dropped += settle(lba, count, nullptr);
  }
  return ok;
}

void SectorCache::invalidate(uint32_t lba, uint32_t count) { stats_.dropped += settle(lba, count, nullptr); }

uint32_t SectorCache::order(uint32_t* out, uint32_t max) const {
  uint32_t k = 0;
  for (uint16_t i = head_; i != kNone && k < max; i = slots_[i].next) out[k++] = slots_[i].lba;
  return k;
}
