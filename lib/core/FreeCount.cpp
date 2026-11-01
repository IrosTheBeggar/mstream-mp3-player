// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "FreeCount.h"

#include <cstdlib>
#include <cstring>

namespace {

uint32_t le16(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8; }
uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
         static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

bool FreeCount::begin(const Volume& v) {
  end();
  const uint32_t ss = v.sectorBytes;
  if (ss < 512 || ss > kPieceBytes || (ss & (ss - 1)) != 0 || v.entries < 3 || v.fatSectors == 0) return false;
  // The bytes holding entries 0 to entries - 1: 4 or 2 a piece, FAT12's 1.5.
  uint64_t bytes = v.fat == Fat::Fat32   ? static_cast<uint64_t>(v.entries) * 4
                   : v.fat == Fat::Fat16 ? static_cast<uint64_t>(v.entries) * 2
                                         : (static_cast<uint64_t>(v.entries) * 3 + 1) / 2;
  const uint64_t sectors = (bytes + ss - 1) / ss;
  if (sectors > v.fatSectors) return false;  // the entries don't fit its FAT: not a volume FatFs mounts
  pieceSectors_ = kPieceBytes / ss;
  const uint64_t pieces = (sectors + pieceSectors_ - 1) / pieceSectors_;
  if (v.fat == Fat::Fat12 && pieces != 1) return false;  // (entries a piece apart would straddle)
  const size_t words = static_cast<size_t>((pieces + 31) / 32);
  const size_t countBytes = static_cast<size_t>(pieces) * sizeof(uint16_t);
  void* mem = alloc_ ? alloc_(countBytes + words * sizeof(uint32_t)) : std::malloc(countBytes + words * sizeof(uint32_t));
  if (!mem) return false;
  // The dirty bits first (4-byte aligned), then the counts.
  dirty_ = static_cast<uint32_t*>(mem);
  counts_ = reinterpret_cast<uint16_t*>(dirty_ + words);
  std::memset(mem, 0, countBytes + words * sizeof(uint32_t));
  fat_ = v.fat;
  fatStart_ = v.fatStart;
  sectors_ = static_cast<uint32_t>(sectors);
  entries_ = v.entries;
  sectorBytes_ = ss;
  pieces_ = static_cast<uint32_t>(pieces);
  bytes_ = static_cast<uint32_t>(bytes);
  cursor_ = 0;
  again_ = 0;
  total_ = 0;
  reads_ = 0;
  rereads_ = 0;
  return true;
}

void FreeCount::end() {
  if (dirty_) {
    if (release_) release_(dirty_);
    else std::free(dirty_);
  }
  dirty_ = nullptr;
  counts_ = nullptr;
  pieces_ = 0;
  cursor_ = 0;
  again_ = 0;
}

bool FreeCount::next(Piece* p) const {
  if (!active()) return false;
  uint32_t i = cursor_;
  if (i == pieces_) {
    if (again_ == 0) return false;
    const uint32_t words = (pieces_ + 31) / 32;
    uint32_t w = 0;
    while (w < words && dirty_[w] == 0) ++w;
    if (w == words) return false;  // (never: again_ counts the bits)
    uint32_t b = 0;
    while (!((dirty_[w] >> b) & 1u)) ++b;
    i = w * 32 + b;
  }
  p->index = i;
  p->lba = fatStart_ + i * pieceSectors_;
  const uint32_t left = sectors_ - i * pieceSectors_;
  p->sectors = left < pieceSectors_ ? left : pieceSectors_;
  return true;
}

uint32_t FreeCount::freeIn(uint32_t i, const uint8_t* data) const {
  const uint32_t from = i * pieceSectors_ * sectorBytes_;  // the piece's first byte in the FAT
  const uint32_t left = sectors_ - i * pieceSectors_;
  const uint32_t len = (left < pieceSectors_ ? left : pieceSectors_) * sectorBytes_;
  uint32_t n = 0;
  if (fat_ == Fat::Fat12) {  // the whole FAT: entry e at byte e + e / 2
    for (uint32_t e = 2; e < entries_; ++e) {
      const uint32_t o = e + e / 2;
      const uint32_t v = (e & 1) ? (data[o] >> 4) | (static_cast<uint32_t>(data[o + 1]) << 4)
                                 : data[o] | (static_cast<uint32_t>(data[o + 1] & 0x0F) << 8);
      n += v == 0;
    }
    return n;
  }
  const uint32_t size = fat_ == Fat::Fat32 ? 4 : 2;
  uint32_t first = from / size, last = (from + len) / size;  // [first, last) of the entries
  if (first < 2) first = 2;
  if (last > entries_) last = entries_;
  for (uint32_t e = first; e < last; ++e) {
    const uint8_t* q = data + (e * size - from);
    n += fat_ == Fat::Fat32 ? (le32(q) & 0x0FFFFFFFu) == 0 : le16(q) == 0;
  }
  return n;
}

void FreeCount::counted(const Piece& p, const uint8_t* data) {
  if (!active() || p.index >= pieces_) return;
  const uint32_t i = p.index;
  const uint32_t n = freeIn(i, data);
  if (i < cursor_) {  // read again: its old count goes
    total_ -= counts_[i];
    ++rereads_;
    if (dirty(i)) {
      dirty_[i >> 5] &= ~(1u << (i & 31));
      --again_;
    }
  } else {
    cursor_ = i + 1;  // (next() hands the pass's pieces in order)
  }
  counts_[i] = static_cast<uint16_t>(n);
  total_ += n;
  ++reads_;
}

void FreeCount::written(uint32_t lba, uint32_t n) {
  if (!active() || n == 0) return;
  const uint64_t from = lba, to = static_cast<uint64_t>(lba) + n;
  const uint64_t fatFrom = fatStart_, fatTo = static_cast<uint64_t>(fatStart_) + sectors_;
  if (to <= fatFrom || from >= fatTo) return;
  // [a, b): the FAT's sectors it reached.
  const auto a = static_cast<uint32_t>((from > fatFrom ? from : fatFrom) - fatFrom);
  const auto b = static_cast<uint32_t>((to < fatTo ? to : fatTo) - fatFrom);
  for (uint32_t i = a / pieceSectors_; i <= (b - 1) / pieceSectors_ && i < cursor_; ++i) {
    if (!dirty(i)) {
      dirty_[i >> 5] |= 1u << (i & 31);
      ++again_;
    }
  }
}

uint8_t FreeCount::percent() const {
  if (done()) return 100;
  if (!active() || pieces_ == 0) return 0;
  const uint32_t p = static_cast<uint32_t>(static_cast<uint64_t>(cursor_) * 100 / pieces_);
  return static_cast<uint8_t>(p > 99 ? 99 : p);
}

void FreeCount::overlay(uint8_t* data, uint32_t lba, uint32_t sectors, uint32_t sectorBytes, uint32_t winLba,
                        const uint8_t* win) {
  if (!data || !win || winLba < lba || winLba - lba >= sectors) return;
  std::memcpy(data + static_cast<size_t>(winLba - lba) * sectorBytes, win, sectorBytes);
}

bool FreeCount::readPiece(Source& src, const Piece& p, uint8_t* buf) {
  if (!src.read(p.lba, buf, p.sectors)) return false;
  uint32_t winLba = 0;
  const uint8_t* win = nullptr;
  if (src.window(&winLba, &win)) overlay(buf, p.lba, p.sectors, sectorBytes_, winLba, win);
  counted(p, buf);
  return true;
}

FreeCount::Step FreeCount::step(Source& src, uint8_t* buf) {
  if (!active()) return Step::Failed;
  Piece p;
  if (next(&p)) {
    if (!readPiece(src, p, buf)) return Step::Failed;
    if (!done()) return Step::Working;
  }
  // Every piece counted: the commit's check, in this hold.
  uint32_t winLba = 0;
  const uint8_t* win = nullptr;
  if (src.window(&winLba, &win) && inFat(winLba)) written(winLba, 1);
  for (int i = 0; i < kSettlePieces && next(&p); ++i) {
    if (!readPiece(src, p, buf)) return Step::Failed;
  }
  return done() ? Step::Done : Step::Working;
}
