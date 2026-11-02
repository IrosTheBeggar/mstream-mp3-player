// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CardWaves.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace cardcontract {
namespace mpwf {

namespace {

const SectionSpec kSections[kSecCount] = {
    {kWvsl, kSlotBytes, true},
};

}  // namespace

const FormatSpec kSpec = {kMagicMpwf, kHeaderBytes, kSections, kSecCount, kMajor1};

uint32_t slotCrc(const uint8_t* slot, uint32_t row) {
  uint8_t r[4];
  put32(r, row);
  return crc32(slot, kCrcAt, crc32(r, sizeof(r)));
}

uint32_t pick(uint64_t h, uint32_t lo, uint32_t hi, uint64_t loH, uint64_t hiH, uint32_t probes) {
  const uint64_t span = hi - lo;
  const uint64_t den = (hiH - loH) >> 32;
  if (probes < kInterpolated && den != 0)
    return lo + static_cast<uint32_t>((span * ((h - loH) >> 32) + den / 2) / den);
  return lo + static_cast<uint32_t>(span / 2);
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------
Why Table::open(Source& src) {
  rows_ = 0;
  reads_ = 0;
  // No room for the 184 bytes of type header on this stack: only their first
  // 8 mean anything to this minor, read alone.
  const Why w = c_.open(src, kSpec, nullptr);
  if (w != Why::Ok) return w;
  uint8_t th[8];
  if (!src.read(kCommonHeaderBytes, th, sizeof(th))) return Why::Io;
  // 8.2, in its order.
  if (get32(th) != kSlotBytes || get32(th + 4) != kBarCount) return Why::Enum;  // 40, 44
  const Section& slots = c_.section(kSecSlots);
  if (slots.stride != kSlotBytes) return Why::Shape;
  if (slots.count < 1 || slots.count > kMaxRows) return Why::Enum;
  if (slots.offset != kSlotsOffset) return Why::WvLayout;
  rows_ = slots.count;
  return Why::Ok;
}

Why Table::check(uint8_t* scratch, uint32_t scratchBytes) const {
  if (!scratch || scratchBytes < kSlotBytes + 64) return Why::Io;
  const Section& sec = c_.section(kSecSlots);
  uint8_t* slot = scratch;
  Stream s;
  s.begin(c_.source(), sec.offset, sec.offset + sec.bytes, scratch + kSlotBytes, scratchBytes - kSlotBytes);
  uint64_t prev = 0;
  for (uint32_t r = 0; r < rows_; ++r) {
    if (!s.read(slot, kSlotBytes)) return Why::Io;
    if (slotCrc(slot, r) != get32(slot + kCrcAt)) return Why::WvSlotCrc;
    const uint64_t h = get64(slot);
    if (r > 0 && h <= prev) return Why::WvSlotOrder;
    prev = h;
  }
  if (!s.drain()) return Why::Io;
  return s.crc() == sec.crc ? Why::Ok : Why::SectionCrc;
}

Lookup Table::find(uint64_t pathHash, Slot* out, Why* why) {
  auto done = [&](Lookup l, Why w) {
    if (why) *why = w;
    return l;
  };
  reads_ = 0;
  if (!out) return done(Lookup::Io, Why::Io);
  // Signed: hi reaches -1 when row 0 is above the hash. The first bounds
  // aren't slots, so the sort rule looks at loH only once lo has moved, and
  // at hiH only once hi has: a slot may hold hash 0 or 2^64 - 1.
  const int32_t last = static_cast<int32_t>(rows_) - 1;
  int32_t lo = 0, hi = last;
  uint64_t loH = 0, hiH = ~static_cast<uint64_t>(0);
  while (lo <= hi && reads_ < kMaxProbes) {
    const uint32_t r = pick(pathHash, static_cast<uint32_t>(lo), static_cast<uint32_t>(hi), loH, hiH, reads_);
    ++reads_;
    if (!c_.source()->read(kSlotsOffset + r * kSlotBytes, out->bytes, kSlotBytes)) return done(Lookup::Io, Why::Io);
    if (slotCrc(out->bytes, r) != out->crc()) return done(Lookup::Damaged, Why::WvSlotCrc);
    const uint64_t v = out->pathHash();
    if ((lo > 0 && v <= loH) || (hi < last && v >= hiH)) return done(Lookup::Broken, Why::WvSlotOrder);
    if (v == pathHash) return done(Lookup::Hit, Why::Ok);
    if (v < pathHash) {
      lo = static_cast<int32_t>(r) + 1;
      loH = v;
    } else {
      hi = static_cast<int32_t>(r) - 1;
      hiH = v;
    }
  }
  return done(Lookup::Miss, Why::Ok);
}

// ---------------------------------------------------------------------------
// The bars (5) and the writer
// ---------------------------------------------------------------------------
void barRange(uint32_t i, uint32_t m, uint32_t* start, uint32_t* end) {
  const uint32_t s = static_cast<uint32_t>(static_cast<uint64_t>(i) * m / kBarCount);
  const uint32_t e = static_cast<uint32_t>(static_cast<uint64_t>(i + 1) * m / kBarCount);
  *start = s;
  *end = std::min(m, std::max(s + 1, e));
}

void barsFrom(const uint8_t* peaks, uint32_t m, uint8_t* bars) {
  if (m == 0) {
    std::memset(bars, 0, kBarCount);
    return;
  }
  for (uint32_t i = 0; i < kBarCount; ++i) {
    uint32_t start, end;
    barRange(i, m, &start, &end);
    uint64_t n = 0;
    for (uint32_t j = start; j < end; ++j) n += static_cast<uint32_t>(peaks[j]) * peaks[j];
    // n and the width are exact; a tie is exact too, and anything else is far
    // enough from one for any f64 to agree (5).
    const double rms = std::sqrt(static_cast<double>(n) / static_cast<double>(end - start));
    bars[i] = static_cast<uint8_t>(std::floor(rms + 0.5));
  }
}

bool write(Sink& out, const TableIn& in, uint32_t* fileBytes, uint32_t* headerCrc, const char** error) {
  auto bad = [&](const char* e) {
    if (error) *error = e;
    return false;
  };
  if (!in.rows && in.rowCount) return bad("rows missing");
  std::vector<uint32_t> order(in.rowCount);
  for (uint32_t i = 0; i < in.rowCount; ++i) {
    if (!in.rows[i].bars) return bad("a row with no bars");
    order[i] = i;
  }
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return in.rows[a].pathHash < in.rows[b].pathHash; });
  // The sort rule's strict: a hash two or more rows hold is left out altogether.
  std::vector<uint32_t> kept;
  kept.reserve(order.size());
  for (size_t i = 0; i < order.size();) {
    size_t e = i + 1;
    while (e < order.size() && in.rows[order[e]].pathHash == in.rows[order[i]].pathHash) ++e;
    if (e - i == 1) kept.push_back(order[i]);
    i = e;
  }
  if (kept.empty()) return bad("no row to write");
  if (kept.size() > kMaxRows) return bad("rows must be 1-250,000");
  const uint32_t rows = static_cast<uint32_t>(kept.size());

  uint8_t th[8];
  put32(th, kSlotBytes);
  put32(th + 4, kBarCount);
  SectionOut s{kWvsl, kSectionRequired, rows, kSlotBytes, rows * kSlotBytes};
  FileMeta fm;
  fm.magic = kMagicMpwf;
  fm.major = kMajor1;
  fm.minor = in.minor;
  fm.generation = in.generation;
  fm.cardId = in.cardId;
  ContainerWriter cw;
  if (!cw.begin(out, fm, th, kHeaderBytes, &s, 1, sizeof(th))) return bad("layout");
  for (uint32_t r = 0; r < rows; ++r) {
    const RowIn& x = in.rows[kept[r]];
    uint8_t slot[kSlotBytes];
    put64(slot, x.pathHash);
    put32(slot + 8, x.lengthMs);
    std::memcpy(slot + kBarsAt, x.bars, kBarCount);
    put32(slot + kCrcAt, slotCrc(slot, r));
    if (!cw.write(slot, kSlotBytes)) return bad("sink");
  }
  if (!cw.finish(fileBytes, headerCrc)) return bad("sink");
  return true;
}

}  // namespace mpwf
}  // namespace cardcontract
