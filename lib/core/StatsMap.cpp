// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsMap.h"

#include <cstring>

#include "CardContract.h"

namespace statsmap {

namespace cc = cardcontract;

uint64_t Wanted::hashAt(uint32_t i) const { return cc::get64(row(i)); }
uint32_t Wanted::idAt(uint32_t i) const { return cc::get32(row(i) + 8); }
void Wanted::setId(uint32_t i, uint32_t id) { cc::put32(row(i) + 8, id); }

bool Wanted::add(uint64_t hash) {
  if (n == cap()) return false;
  uint8_t* r = row(n++);
  cc::put64(r, hash);
  cc::put32(r + 8, kNoId);
  return true;
}

void Wanted::seal() {
  // An insertion sort over the chunked rows (at most a few hundred: the
  // lists' hashes), then each hash once.
  for (uint32_t i = 1; i < n; ++i) {
    const uint64_t h = hashAt(i);
    uint32_t j = i;
    while (j > 0 && hashAt(j - 1) > h) {
      std::memcpy(row(j), row(j - 1), kRowBytes);
      --j;
    }
    cc::put64(row(j), h);
    cc::put32(row(j) + 8, kNoId);
  }
  uint32_t out = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (out > 0 && hashAt(out - 1) == hashAt(i)) continue;
    if (out != i) std::memcpy(row(out), row(i), kRowBytes);
    setId(out++, kNoId);
  }
  n = out;
}

int64_t Wanted::find(uint64_t hash) const {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint64_t h = hashAt(mid);
    if (h == hash) return mid;
    if (h < hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

uint32_t Wanted::idOf(uint64_t hash) const {
  const int64_t i = find(hash);
  if (i < 0) return kNoId;
  const uint32_t id = idAt(static_cast<uint32_t>(i));
  return id == kTwoTracks ? kNoId : id;
}

Map::Status Map::begin(const LibraryIndex* index, const statsrank::Played* played, Wanted* wanted,
                       uint8_t* albumBits, void* folders) {
  release();
  stats_ = Stats();
  if (!index || !index->ready()) {
    status_ = Status::Aborted;
    return status_;
  }
  index_ = index;
  played_ = played && albumBits ? played : nullptr;
  wanted_ = wanted;
  bits_ = played_ ? albumBits : nullptr;
  albums_ = bits_ ? index->albumCount() : 0;
  if (bits_) std::memset(bits_, 0, albumBytes(albums_));
  if (wanted_)
    for (uint32_t i = 0; i < wanted_->n; ++i) wanted_->setId(i, kNoId);
  walk_.begin(index, folders ? pathhash::Walk::foldersFor(*index) : 0, folders);
  status_ = Status::Working;
  return status_;
}

void Map::release() {
  walk_.end();
  index_ = nullptr;
  status_ = Status::Idle;
}

void Map::visit(void* ctx, uint32_t t, uint64_t h) {
  Map& m = *static_cast<Map*>(ctx);
  if (m.played_ && m.played_->contains(h)) {
    ++m.stats_.playedTracks;
    const uint32_t a = m.index_->track(t).album;
    if (a < m.albums_) m.bits_[a / 8] = static_cast<uint8_t>(m.bits_[a / 8] | 1u << (a % 8));
  }
  if (m.wanted_) {
    const int64_t i = m.wanted_->find(h);
    if (i >= 0) {
      const uint32_t k = static_cast<uint32_t>(i);
      // A second track with the hash: neither mapped (6.6).
      m.wanted_->setId(k, m.wanted_->idAt(k) == kNoId ? t : kTwoTracks);
    }
  }
}

Map::Status Map::step(const LibraryIndex* index) {
  if (status_ != Status::Working) return status_;
  if (!walk_.matches(index)) {
    walk_.end();
    status_ = Status::Aborted;
    return status_;
  }
  ++stats_.slices;
  if (!walk_.foldersDone()) {
    walk_.foldersSlice(cfg_.foldersPerSlice);
    return status_;
  }
  walk_.hashSlice(cfg_.tracksPerSlice, visit, this);
  stats_.tracks = walk_.hashed() + walk_.unhashed();
  stats_.unhashed = walk_.unhashed();
  if (!walk_.tracksDone()) return status_;
  // Ready: the counts.
  for (uint32_t a = 0; a < albums_; ++a) stats_.albumsPlayed += albumPlayed(a) ? 1 : 0;
  if (wanted_) {
    for (uint32_t i = 0; i < wanted_->n; ++i) {
      const uint32_t id = wanted_->idAt(i);
      if (id == kTwoTracks) {
        ++stats_.collisions;
      } else if (id != kNoId) {
        ++stats_.wantedMapped;
      }
    }
  }
  walk_.end();
  status_ = Status::Ready;
  return status_;
}

Map::Status Map::stepPass(const LibraryIndex* index, bool (*more)(void* ctx), void* ctx) {
  do {
    step(index);
  } while (status_ == Status::Working && (!more || more(ctx)));
  return status_;
}

}  // namespace statsmap
