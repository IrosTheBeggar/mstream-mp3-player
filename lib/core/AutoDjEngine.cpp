// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjEngine.h"

#include <cstdlib>
#include <cstring>
#include <functional>

#include "CardContract.h"
#include "IdSort.h"

namespace autodj {

namespace mpdj = cardcontract::mpdj;

namespace {

constexpr uint32_t kSlotSpan = mpdj::kLargeSlot;

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

uint16_t rowOf(const Maps* m, uint32_t track) {
  return m && m->trackToRow && track < m->tracks ? m->trackToRow[track] : kNoRow;
}

// The filter word's Camelot code; one above 24 (no join writes it) is none.
uint8_t camelot(uint32_t f) {
  const uint8_t c = filterCamelot(f);
  return c <= 24 ? c : 0;
}

}  // namespace

Engine::~Engine() { end(); }

bool Engine::begin(AllocFn alloc, FreeFn release) {
  end();
  alloc_ = alloc ? alloc : defaultAlloc;
  free_ = release ? release : defaultFree;
  slotBytes_ = static_cast<uint8_t*>(alloc_((kSlotCache + 1) * kSlotSpan));
  keys_ = static_cast<uint32_t*>(alloc_(kMaxUnion * sizeof(uint32_t)));
  cand_ = static_cast<Cand*>(alloc_(kMaxUnion * sizeof(Cand)));
  if (!slotBytes_ || !keys_ || !cand_) {
    end();
    return false;
  }
  fileReopened();
  return true;
}

void Engine::end() {
  if (free_) {
    if (slotBytes_) free_(slotBytes_);
    if (keys_) free_(keys_);
    if (cand_) free_(cand_);
  }
  slotBytes_ = nullptr;
  keys_ = nullptr;
  cand_ = nullptr;
  for (CacheSlot& c : cache_) c = CacheSlot();
}

size_t Engine::heldBytes() const {
  return slotBytes_ ? (kSlotCache + 1) * kSlotSpan + kMaxUnion * (sizeof(uint32_t) + sizeof(Cand)) : 0;
}

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------
void Engine::newSession(uint32_t seedValue, uint32_t seedTrack, const Maps* maps) {
  ses_ = Session();
  ses_.rng.reseed(seedValue);
  if (seedTrack != kNoTrackId) seed(seedTrack, maps, true);
}

// The session's first anchor; `pin` also Stay on seed's (newSession()'s
// track, the playing track at the first pick), not the first track to
// start after a session began without one.
void Engine::seed(uint32_t track, const Maps* maps, bool pin) {
  ses_.seeded = true;
  ses_.ids.add(track);
  const uint16_t row = rowOf(maps, track);
  if (row == kNoRow) return;
  join(row, *maps);
  if (pin) ses_.pin = row;
}

void Engine::join(uint16_t row, const Maps& m) {
  ses_.ring.add(row);
  ses_.cool.add(row);
  const uint16_t a = filterArtist(m.filters[row]);
  if (a) ses_.arts.add(a);
}

void Engine::trackStarted(uint32_t track, const Maps* maps, bool byAutoDj) {
  if (!ses_.seeded) {
    seed(track, maps, false);
    return;
  }
  const uint16_t row = rowOf(maps, track);
  if (byAutoDj) {
    // Joined when picked (its row cooled since); one picked by track id
    // while the index was away joins now that its row is known (D1).
    if (row != kNoRow && !ses_.cool.has(row)) join(row, *maps);
    return;
  }
  ses_.ids.add(track);
  if (row != kNoRow) join(row, *maps);
}

void Engine::trackLeft(uint32_t track, const Maps* maps, uint32_t playedMs, bool naturalEnd) {
  rowLeft(rowOf(maps, track), playedMs, naturalEnd);
}

void Engine::rowLeft(uint16_t row, uint32_t playedMs, bool naturalEnd) {
  if (naturalEnd || playedMs >= kQuickSkipMs) return;
  ++stats_.quickSkips;
  if (row != kNoRow) ses_.ring.remove(row);
}

void Engine::pickRefused(uint16_t row) {
  if (row != kNoRow) ses_.ring.remove(row);
}

void Engine::modeChanged() { ses_.pin = kNoRow; }

void Engine::fileReopened() {
  for (CacheSlot& c : cache_) c = CacheSlot();
  damaged_.clear();
  suspect_ = kNoRow;
  broken_ = false;
}

void Engine::tracksRenumbered() { ses_.ids.clear(); }

void Engine::copySession(const Engine& from) {
  ses_ = from.ses_;
  clock_ = from.clock_;
  damaged_ = from.damaged_;
  suspect_ = from.suspect_;
  broken_ = from.broken_;
  stats_ = from.stats_;
  for (uint8_t i = 0; i < kSlotCache; ++i) {
    cache_[i] = from.cache_[i];
    if (cache_[i].row == kNoRow) continue;
    if (slotBytes_ && from.slotBytes_)
      std::memcpy(slotBytes_ + i * kSlotSpan, from.slotBytes_ + i * kSlotSpan, kSlotSpan);
    else
      cache_[i] = CacheSlot();
  }
}

// ---------------------------------------------------------------------------
// The slot cache and reads
// ---------------------------------------------------------------------------
int Engine::find(uint16_t row) const {
  if (row == kNoRow) return -1;
  for (uint8_t i = 0; i < kSlotCache; ++i)
    if (cache_[i].row == row) return i;
  return -1;
}

void Engine::insert(uint16_t row, uint16_t count) {
  int at = -1;
  for (uint8_t i = 0; i < kSlotCache && at < 0; ++i)
    if (cache_[i].row == kNoRow) at = i;
  if (at < 0) {
    // The least recently used slot that is neither in the ring nor the pin
    // (at most 5 + 1 rows, one of them `row`: one other is always there).
    for (uint8_t i = 0; i < kSlotCache; ++i) {
      const uint16_t r = cache_[i].row;
      if (ses_.ring.has(r) || r == ses_.pin) continue;
      if (at < 0 || cache_[i].stamp < cache_[at].stamp) at = i;
    }
  }
  if (at < 0) {
    at = 0;
    for (uint8_t i = 1; i < kSlotCache; ++i)
      if (cache_[i].stamp < cache_[at].stamp) at = i;
  }
  std::memcpy(slotBytes_ + at * kSlotSpan, slotBytes_ + kSlotCache * kSlotSpan, kSlotSpan);
  cache_[at].row = row;
  cache_[at].count = count;
  cache_[at].stamp = ++clock_;
}

SlotResult Engine::read(SlotReader& slots, uint16_t row) {
  uint8_t* buf = slotBytes_ + kSlotCache * kSlotSpan;
  uint32_t count = 0;
  cardcontract::Why why = cardcontract::Why::Ok;
  const mpdj::SlotRead r = slots.read(row, buf, &count, &why);
  if (r == mpdj::SlotRead::Ok) {
    insert(row, static_cast<uint16_t>(count > mpdj::kMaxK ? mpdj::kMaxK : count));
    if (suspect_ == row) suspect_ = kNoRow;
    return SlotResult::Ok;
  }
  if (r == mpdj::SlotRead::Broken) {
    broken_ = true;
    ++stats_.broken;
    return SlotResult::Broken;
  }
  if (why == cardcontract::Why::Io) {
    ++stats_.ioSkips;
    return SlotResult::Io;
  }
  if (suspect_ == row) {
    suspect_ = kNoRow;
    damaged_.add(row);
    ++stats_.damaged;
    return SlotResult::Damaged;
  }
  suspect_ = row;
  return SlotResult::Suspect;
}

bool Engine::prefetchDue(const Settings& s, uint16_t* row) const {
  if (broken_ || !slotBytes_) return false;
  if (s.mode == Mode::Locked) {
    const uint16_t r = ses_.pin;
    if (r == kNoRow || damaged_.has(r) || find(r) >= 0) return false;
    *row = r;
    return true;
  }
  for (int i = ses_.ring.size() - 1; i >= 0; --i) {
    const uint16_t r = ses_.ring.at(static_cast<uint8_t>(i));
    if (damaged_.has(r) || find(r) >= 0) continue;
    *row = r;
    return true;
  }
  return false;
}

SlotResult Engine::prefetch(SlotReader& slots, uint16_t row) {
  ++stats_.prefetchReads;
  if (!slotBytes_) return SlotResult::Io;
  return read(slots, row);
}

// ---------------------------------------------------------------------------
// The pick
// ---------------------------------------------------------------------------
Pick Engine::pick(const Settings& s, const PickIn& in) {
  Pick out;
  if (!in.maps || !in.slots || !slotBytes_ || in.maps->rows == 0) return byId(s, in, out);
  const Maps& m = *in.maps;
  const uint16_t playingRow = rowOf(&m, in.playingTrack);
  if (!ses_.seeded && in.playingTrack != kNoTrackId) seed(in.playingTrack, &m, true);
  if (s.matchKey && ses_.keyAnchor == 0 && playingRow != kNoRow) ses_.keyAnchor = camelot(m.filters[playingRow]);
  if (s.mode == Mode::Locked && ses_.pin == kNoRow && playingRow != kNoRow) ses_.pin = playingRow;

  // 1. The anchors, oldest first; their slots, newest first.
  uint16_t anchors[kAnchors];
  uint8_t na = 0;
  if (s.mode == Mode::Locked) {
    if (ses_.pin != kNoRow) anchors[na++] = ses_.pin;
  } else {
    for (uint8_t i = 0; i < ses_.ring.size(); ++i) anchors[na++] = ses_.ring.at(i);
  }
  int8_t usable[kAnchors];
  uint8_t nu = 0;
  if (!broken_) {
    for (int a = na - 1; a >= 0; --a) {
      const uint16_t r = anchors[a];
      if (damaged_.has(r)) continue;
      int i = find(r);
      if (i < 0) {
        if (out.reads >= kReadsPerPick) {
          ++stats_.leftOut;
          continue;
        }
        SlotResult res = SlotResult::Ok;
        while (out.reads < kReadsPerPick) {
          ++out.reads;
          res = read(*in.slots, r);
          if (res != SlotResult::Suspect) break;
        }
        if (res == SlotResult::Broken) break;
        if (res != SlotResult::Ok) continue;
        i = find(r);
      }
      usable[nu++] = static_cast<int8_t>(i);
    }
  }
  out.broken = broken_;
  if (broken_ || nu == 0) {
    // A damaged pin would make every Stay on seed pick random until the
    // file is reopened: this pick's row becomes the pin, as for a seed
    // without a row.
    if (s.mode == Mode::Locked && damaged_.has(ses_.pin)) ses_.pin = kNoRow;
    return randomPick(s, in, playingRow, anchors, na, out);
  }
  for (int u = nu - 1; u >= 0; --u) touch(usable[u]);

  // 2. The union: (row << 8 | level) sorted, then merged by row.
  const uint32_t k = in.k <= mpdj::kMaxK ? in.k : mpdj::kMaxK;
  uint32_t nk = 0;
  for (uint8_t u = 0; u < nu; ++u) {
    const CacheSlot& c = cache_[usable[u]];
    const uint8_t* b = slotBytes_ + usable[u] * kSlotSpan;
    const uint32_t count = c.count < k ? c.count : k;
    for (uint32_t j = 0; j < count; ++j) {
      const uint16_t row = cardcontract::get16(b + 2 * j);
      if (row >= m.rows || row == playingRow || m.rowToTrack[row] == kNoTrack) continue;
      bool anchor = false;
      for (uint8_t a = 0; a < na && !anchor; ++a) anchor = anchors[a] == row;
      if (anchor) continue;
      keys_[nk++] = static_cast<uint32_t>(row) << 8 | b[2 * k + j];
    }
  }
  idsort::sort(keys_, keys_ + nk, std::less<uint32_t>());
  uint32_t nc = 0;
  for (uint32_t i = 0; i < nk; ++i) {
    const uint16_t row = static_cast<uint16_t>(keys_[i] >> 8);
    const uint8_t level = static_cast<uint8_t>(keys_[i]);
    if (nc && cand_[nc - 1].row == row) {
      cand_[nc - 1].best = level;  // sorted: the run's last is its best
      cand_[nc - 1].sum = static_cast<uint16_t>(cand_[nc - 1].sum + level);
    } else {
      cand_[nc++] = Cand{row, level, level, 0, Tempo::Na, Tempo::Na, Key::Na, Key::Na};
    }
  }
  out.candidates = static_cast<uint16_t>(nc);
  if (nc == 0) return randomPick(s, in, playingRow, anchors, na, out);

  // 3. The gate, or dry: the kDry best sums (ties by row).
  const uint8_t gate = floorLevel(s.floorPct, in.selFloor);
  uint32_t ng = 0;
  for (uint32_t i = 0; i < nc; ++i)
    if (cand_[i].best >= gate) cand_[ng++] = cand_[i];
  if (ng == 0) {
    out.dry = true;
    auto dryKey = [](const Cand& c) { return static_cast<uint32_t>(2047 - c.sum) << 16 | c.row; };
    for (uint32_t i = 0; i < nc; ++i) keys_[i] = dryKey(cand_[i]);
    idsort::sort(keys_, keys_ + nc, std::less<uint32_t>());
    const uint32_t last = keys_[(nc < kDry ? nc : kDry) - 1];
    for (uint32_t i = 0; i < nc; ++i)
      if (dryKey(cand_[i]) <= last) cand_[ng++] = cand_[i];
  }
  out.gated = static_cast<uint16_t>(ng);

  // 4-5. The classes, the draw, the pick.
  const uint32_t j = choose(s, m, ng, playingRow, out);
  out.level = cand_[j].best;
  return finish(s, m, cand_[j].row, out);
}

Pick Engine::randomPick(const Settings& s, const PickIn& in, uint16_t playingRow, const uint16_t* anchors,
                        uint8_t na, Pick out) {
  out.random = true;
  const Maps& m = *in.maps;
  uint32_t n = 0;
  for (uint8_t d = 0; d < kRandomDraws; ++d) {
    const uint16_t r = static_cast<uint16_t>(ses_.rng.below(m.rows));
    if (r == playingRow || m.rowToTrack[r] == kNoTrack) continue;
    bool skip = false;
    for (uint8_t a = 0; a < na && !skip; ++a) skip = anchors[a] == r;
    for (uint32_t i = 0; i < n && !skip; ++i) skip = cand_[i].row == r;
    if (skip) continue;
    cand_[n++] = Cand{r, 0, 0, 0, Tempo::Na, Tempo::Na, Key::Na, Key::Na};
  }
  out.candidates = out.gated = static_cast<uint16_t>(n);
  if (n == 0) return byId(s, in, out);
  const uint32_t j = choose(s, m, n, playingRow, out);
  out.level = 0;
  return finish(s, m, cand_[j].row, out);
}

uint32_t Engine::choose(const Settings& s, const Maps& m, uint32_t n, uint16_t playingRow, Pick& out) {
  const uint8_t ref = s.matchTempo && playingRow != kNoRow ? filterBpm(m.filters[playingRow]) : 0;
  const uint8_t anchor = ses_.keyAnchor;
  TempoOdds to;
  KeyOdds ko;
  for (uint32_t i = 0; i < n; ++i) {
    Cand& c = cand_[i];
    const uint32_t f = m.filters[c.row];
    c.t = c.ti = tempoStatus(filterBpm(f), ref);
    c.k = c.ki = keyStatus(camelot(f), anchor, s.matchKey);
    to.add(c.t);
    ko.add(c.k);
  }
  // D3: every unknown tempo, then every unknown key, in the candidates' order.
  if (to.known())
    for (uint32_t i = 0; i < n; ++i)
      if (cand_[i].t == Tempo::Unknown) cand_[i].ti = to.draw(ses_.rng);
  if (ko.known())
    for (uint32_t i = 0; i < n; ++i)
      if (cand_[i].k == Key::Unknown) cand_[i].ki = ko.draw(ses_.rng);
  uint8_t best = 0xFF;
  uint32_t inBest = 0;
  for (uint32_t i = 0; i < n; ++i) {
    Cand& c = cand_[i];
    const uint16_t a = filterArtist(m.filters[c.row]);
    const uint8_t level = ses_.cool.has(c.row) ? 2 : a && ses_.arts.has(a) ? 1 : 0;
    c.cls = static_cast<uint8_t>(level * kRanks + musicRank(c.ti, c.ki));
    if (c.cls < best) {
      best = c.cls;
      inBest = 0;
    }
    if (c.cls == best) ++inBest;
  }
  uint32_t nth = ses_.rng.below(inBest);
  uint32_t j = 0;
  for (; j < n; ++j)
    if (cand_[j].cls == best && nth-- == 0) break;
  const Cand& c = cand_[j];
  out.cooldownLevel = best / kRanks;
  out.musicRank = best % kRanks;
  out.inClass = static_cast<uint16_t>(inBest);
  out.imputedTempo = c.t != c.ti;
  out.imputedKey = c.k != c.ki;
  out.tempoBreak = c.t == Tempo::Wide || c.t == Tempo::Wrong;
  out.keyBreak = c.k == Key::Wrong;
  return j;
}

Pick Engine::finish(const Settings& s, const Maps& m, uint16_t row, Pick out) {
  out.ok = true;
  out.row = row;
  out.track = m.rowToTrack[row];
  join(row, m);
  ses_.ids.add(out.track);
  if (s.matchKey && ses_.keyAnchor == 0) ses_.keyAnchor = camelot(m.filters[row]);
  if (s.mode == Mode::Locked && ses_.pin == kNoRow) ses_.pin = row;
  const int mode = static_cast<int>(s.mode);
  ++stats_.classes[out.cooldownLevel * kRanks + out.musicRank];
  stats_.imputedTempo += out.imputedTempo;
  stats_.imputedKey += out.imputedKey;
  stats_.tempoBreaks[mode] += out.tempoBreak;
  stats_.keyBreaks[mode] += out.keyBreak;
  return counted(s, out);
}

Pick Engine::byId(const Settings& s, const PickIn& in, Pick out) {
  out.random = true;
  out.noIndex = true;
  const uint32_t n = in.libraryTracks;
  uint32_t t = kNoTrackId;
  for (uint8_t i = 0; n && i < kNoIndexTries; ++i) {
    const uint32_t x = ses_.rng.below(n);
    if (x == in.playingTrack) continue;
    t = x;
    if (!ses_.ids.has(x)) break;
  }
  if (t == kNoTrackId) {
    ++stats_.failed;
    return out;
  }
  out.ok = true;
  out.track = t;
  out.row = kNoRow;
  ses_.ids.add(t);
  const uint16_t row = rowOf(in.maps, t);
  if (row != kNoRow) {
    out.row = row;
    join(row, *in.maps);
    if (s.mode == Mode::Locked && ses_.pin == kNoRow) ses_.pin = row;
  }
  return counted(s, out);
}

Pick Engine::counted(const Settings& s, const Pick& out) {
  ++stats_.picks;
  ++stats_.modePicks[static_cast<int>(s.mode)];
  ++stats_.readsAtPick[out.reads];
  stats_.dry += out.dry;
  stats_.random += out.random;
  stats_.noIndex += out.noIndex;
  return out;
}

Pick Engine::surprise(const Settings&, const PickIn& in) {
  Pick out;
  out.random = true;
  if (!in.maps || in.maps->rows == 0) return out;
  const Maps& m = *in.maps;
  for (uint8_t d = 0; d < kSurpriseDraws; ++d) {
    const uint16_t r = static_cast<uint16_t>(ses_.rng.below(m.rows));
    if (m.rowToTrack[r] == kNoTrack) continue;
    out.ok = true;
    out.row = r;
    out.track = m.rowToTrack[r];
    return out;
  }
  return out;
}

}  // namespace autodj
