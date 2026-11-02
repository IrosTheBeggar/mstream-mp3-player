// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "WaveShape.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace cc = cardcontract;
namespace mpwf = cardcontract::mpwf;

namespace wave {

void reduce(const uint8_t* bars, uint8_t* heights) {
  constexpr int kBars = mpwf::kBarCount;
  // quiet is the terminal's sorted[len / 10]. Only the 10 smallest are kept in
  // order, not the 99: this frame stays under 500 B.
  constexpr int kQuietAt = kColumns / 10;
  float energy[kColumns];
  float smallest[kQuietAt + 1];
  float loudest = 0;
  for (int i = 0; i < kColumns; ++i) {
    const int start = i * kBars / kColumns, end = (i + 1) * kBars / kColumns;
    uint32_t n = 0;
    for (int b = start; b < end; ++b) n += static_cast<uint32_t>(bars[b]) * bars[b];
    const float e = std::sqrt(static_cast<float>(n) / static_cast<float>(end - start));
    energy[i] = e;
    loudest = std::max(loudest, e);
    if (i > kQuietAt && e >= smallest[kQuietAt]) continue;
    int at = std::min(i, kQuietAt);
    for (; at > 0 && smallest[at - 1] > e; --at) smallest[at] = smallest[at - 1];
    smallest[at] = e;
  }
  if (loudest <= 0) {
    std::memset(heights, 0, kColumns);
    return;
  }
  const float quiet = smallest[kQuietAt];
  const bool stretch = loudest - quiet > std::numeric_limits<float>::epsilon();
  const float floorE = stretch ? quiet : 0;
  const float span = stretch ? loudest - quiet : loudest;
  for (int i = 0; i < kColumns; ++i) {
    const float x = (energy[i] - floorE) / span * kMaxHeight;
    heights[i] = x <= 0 ? 0 : x >= kMaxHeight ? kMaxHeight : static_cast<uint8_t>(x + 0.5f);
  }
}

const char* readName(Read r) {
  switch (r) {
    case Read::Hit: return "hit";
    case Read::Miss: return "miss";
    case Read::Damaged: return "damaged";
    case Read::Absent: return "absent";
    case Read::Io: return "io";
  }
  return "?";
}

void readShape(cc::Source& src, const cc::msmf::Comp& entry, uint64_t pathHash, mpwf::Table* table, mpwf::Slot* slot,
               ReadResult* out) {
  out->slotReads = 0;
  out->why = table->open(src);
  if (out->why != cc::Why::Ok) {
    out->read = out->why == cc::Why::Io ? Read::Io : Read::Absent;
    return;
  }
  if (!cc::msmf::companionMatches(entry, table->frame(), 0, nullptr)) {
    out->read = Read::Absent;
    out->why = cc::Why::Comp;
    return;
  }
  const mpwf::Lookup l = table->find(pathHash, slot, &out->why);
  out->slotReads = table->reads();
  switch (l) {
    case mpwf::Lookup::Hit:
      out->read = Read::Hit;
      out->shape.lengthMs = slot->lengthMs();
      reduce(slot->bars(), out->shape.heights);
      break;
    case mpwf::Lookup::Miss: out->read = Read::Miss; break;
    case mpwf::Lookup::Damaged: out->read = Read::Damaged; break;
    case mpwf::Lookup::Broken: out->read = Read::Absent; break;
    case mpwf::Lookup::Io: out->read = Read::Io; break;
  }
}

void openFailed(bool missing, ReadResult* out) {
  out->read = missing ? Read::Absent : Read::Io;
  out->why = cc::Why::Io;
  out->slotReads = 0;
}

}  // namespace wave
