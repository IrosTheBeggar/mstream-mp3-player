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

bool Wanted::region(uint8_t* p, uint32_t rows) {
  *this = Wanted();
  if (rows == 0) return true;
  if (!p || rows > kMaxChunks * kChunkRows) return false;
  // Chunk k at the region's row k x kChunkRows: row(i) is the region's i-th.
  chunks = wantedChunks(rows);
  for (uint32_t k = 0; k < chunks; ++k) chunk[k] = p + static_cast<size_t>(k) * kChunkBytes;
  rowsCap = rows;
  return true;
}

bool Wanted::add(uint64_t hash) {
  if (n == cap()) return false;
  uint8_t* r = row(n++);
  cc::put64(r, hash);
  cc::put32(r + 8, kNoId);
  return true;
}

namespace {

// The row held in `held` (its hash `key`) sifted down from `hole` in the
// max-heap of rows [0, end): the larger child moved up into the hole while
// it is larger than the row, then the row put in the hole. A loop, no
// recursion, as libstdc++'s __adjust_heap under the save's sort (11's
// loop-stack row), which goes on to a leaf and pushes back up instead of
// stopping.
void siftDown(const Wanted& w, uint32_t hole, uint32_t end, const uint8_t* held, uint64_t key) {
  for (;;) {
    uint32_t c = 2 * hole + 1;
    if (c >= end) break;
    uint64_t hc = w.hashAt(c);
    if (c + 1 < end) {
      const uint64_t right = w.hashAt(c + 1);
      if (right > hc) {
        ++c;
        hc = right;
      }
    }
    if (hc <= key) break;
    std::memcpy(w.row(hole), w.row(c), kRowBytes);
    hole = c;
  }
  std::memcpy(w.row(hole), held, kRowBytes);
}

}  // namespace

void Wanted::seal() {
  // A heap sort in place over the rows, chunked or one region (S3a's V2;
  // finishRank() seals on the loop): S2's insertion sort took about n^2 / 4
  // steps for the lists' 674-696 rows, which come in rank order (by hash,
  // as good as random), a heap sort about 2n log2 n. Its frames are a few,
  // whatever the input, as the save's sort's (lib/core/StatsFold.cpp: no
  // recursion on the loop's stack). Equal hashes become one row below, so
  // their order doesn't matter.
  uint8_t held[kRowBytes];
  for (uint32_t i = n / 2; i-- > 0;) {
    std::memcpy(held, row(i), kRowBytes);
    siftDown(*this, i, n, held, cc::get64(held));
  }
  for (uint32_t end = n; end-- > 1;) {
    std::memcpy(held, row(end), kRowBytes);
    std::memcpy(row(end), row(0), kRowBytes);
    siftDown(*this, 0, end, held, cc::get64(held));
  }
  // Each hash once, every id kNoId.
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
  return start(index, played, nullptr, wanted, albumBits, folders);
}

Map::Status Map::begin(const LibraryIndex* index, const statsrank::Played40* played, Wanted* wanted,
                       uint8_t* albumBits, void* folders) {
  return start(index, nullptr, played, wanted, albumBits, folders);
}

Map::Status Map::start(const LibraryIndex* index, const statsrank::Played* played,
                       const statsrank::Played40* played40, Wanted* wanted, uint8_t* albumBits, void* folders) {
  release();
  stats_ = Stats();
  played_ = nullptr;
  played40_ = nullptr;
  if (!index || !index->ready()) {
    status_ = Status::Aborted;
    return status_;
  }
  index_ = index;
  tracks_ = index->trackCount();
  stamp_ = index->buildStamp();
  visited_ = 0;
  played_ = played && albumBits ? played : nullptr;
  played40_ = played40 && albumBits ? played40 : nullptr;
  wanted_ = wanted;
  bits_ = played_ || played40_ ? albumBits : nullptr;
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
  ++m.visited_;
  if (m.isPlayed(h)) {
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
  if (!matches(index)) {
    walk_.end();
    index_ = nullptr;
    status_ = Status::Aborted;
    return status_;
  }
  ++stats_.slices;
  if (!walk_.foldersDone()) {
    walk_.foldersSlice(cfg_.foldersPerSlice);
    return status_;
  }
  walk_.hashSlice(cfg_.tracksPerSlice, visit, this);
  stats_.tracks = walk_.position();
  stats_.unhashed = walk_.position() - visited_;
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
