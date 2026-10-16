// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjEngine.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "CardContract.h"
#include "IdSort.h"

namespace autodj {

namespace mpdj = cardcontract::mpdj;

namespace {

constexpr uint32_t kSlotSpan = mpdj::kLargeSlot;
constexpr uint32_t kRunSpan = mpdj::kMaxK + 1;  // a run's entries and its end

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

// The dry rule's order: the highest sum first, ties by row (distinct keys).
uint32_t dryKey(uint16_t sum, uint16_t row) { return static_cast<uint32_t>(2047 - sum) << 16 | row; }

// A Cand's bits.
uint8_t coolOf(uint8_t bits) { return bits & 3; }
Tempo tempoOf(uint8_t bits) { return static_cast<Tempo>((bits >> 2) & 7); }
Key keyOf(uint8_t bits) { return static_cast<Key>((bits >> 5) & 3); }
uint8_t withTempo(uint8_t bits, Tempo t) { return static_cast<uint8_t>((bits & ~(7 << 2)) | static_cast<int>(t) << 2); }
uint8_t withKey(uint8_t bits, Key k) { return static_cast<uint8_t>((bits & ~(3 << 5)) | static_cast<int>(k) << 5); }
uint8_t classOf(uint8_t bits) {
  return static_cast<uint8_t>(coolOf(bits) * kRanks + musicRank(tempoOf(bits), keyOf(bits)));
}

// Sorts n (at most a few dozen) rows into out, then kNoRow.
template <typename Get>
void sortedRows(uint8_t n, Get get, uint16_t* out) {
  for (uint8_t i = 0; i < n; ++i) {
    const uint16_t v = get(i);
    uint8_t q = i;
    for (; q > 0 && out[q - 1] > v; --q) out[q] = out[q - 1];
    out[q] = v;
  }
  out[n] = kNoRow;
}

}  // namespace

// One pick's view of the session (see the header).
struct Engine::Scope {
  uint16_t anc[kAnchors + 1];
  uint16_t cool[kTrackCooldown + 1];
  uint16_t arts[kArtistCooldown] = {};
  uint16_t playingRow;
  uint8_t gate;
  bool tempoOn;
  KeyRef key;
  const uint8_t* tempo;  // tempoStatus() of every BPM against the pick's (all Na with BPM continuity off)

  // `table`: 256 bytes for the tempo statuses (the engine's block).
  Scope(const Session& ses, const Settings& s, const Maps& m, uint16_t playing, const uint16_t* anchors, uint8_t na,
        uint8_t selFloor, uint8_t* table)
      : playingRow(playing), gate(floorLevel(s.floorPct, selFloor)), key(ses.keyAnchor, s.matchKey), tempo(table) {
    sortedRows(na, [&](uint8_t i) { return anchors[i]; }, anc);
    sortedRows(ses.cool.size(), [&](uint8_t i) { return ses.cool.at(i); }, cool);
    for (uint8_t i = 0; i < ses.arts.size(); ++i) arts[i] = ses.arts.at(i);
    const uint8_t ref = s.matchTempo && playing != kNoRow ? filterBpm(m.filters[playing]) : 0;
    tempoOn = ref != 0;
    tempoStatuses(ref, table);
  }
  // BPM continuity or Harmonic mixing on for this pick (off: every status Na).
  bool ranked() const { return tempoOn || key.on(); }
  // The artist cooldown (artist 0 never cooled; an empty place holds 0).
  bool artistCooled(uint32_t word) const {
    const uint16_t a = filterArtist(word);
    return a && (a == arts[0] || a == arts[1] || a == arts[2]);
  }
  uint8_t bits(uint8_t cooldownLevel, uint32_t word) const {
    return static_cast<uint8_t>(cooldownLevel | tempo[filterBpm(word)] << 2 |
                                static_cast<int>(key.status(filterCamelot(word))) << 5);
  }
};

// A pool counted as it is made, by its candidates' bits (one count each):
// its classes, its tempo and key statuses (D3's odds) and its cooldown
// levels come from them; D3's draws move the drawn ones.
struct Engine::Counts {
  uint16_t n[128] = {};
  void add(uint8_t bits) { ++n[bits]; }
  void move(uint8_t from, uint8_t to) {
    --n[from];
    ++n[to];
  }
  // Outside the track cooldown (cooldown levels 0 and 1).
  uint32_t fresh() const {
    uint32_t f = 0;
    for (uint32_t b = 0; b < 128; ++b) f += (b & 3) < 2 ? n[b] : 0;
    return f;
  }
  // By tempo status (5) and by key status (4).
  void statuses(uint32_t* tempo, uint32_t* key) const {
    for (uint32_t b = 0; b < 128; ++b) {
      tempo[(b >> 2) & 7] += n[b];  // (patterns above Unknown never counted)
      key[(b >> 5) & 3] += n[b];
    }
  }
  // The lowest class present and its count.
  uint8_t best(uint32_t* in) const {
    uint32_t cls[kClasses] = {};
    for (uint32_t b = 0; b < 128; ++b)
      if (n[b]) cls[classOf(static_cast<uint8_t>(b))] += n[b];
    uint8_t c = 0;
    while (c + 1 < kClasses && cls[c] == 0) ++c;
    *in = cls[c];
    return c;
  }
};

Engine::~Engine() { end(); }

namespace {
constexpr size_t kRunBytes = kSlotCache * kRunSpan * 8;
constexpr size_t kKeyBytes = kMaxUnion * sizeof(uint32_t);
constexpr size_t kCandBytes = kMaxUnion * 4;
constexpr size_t kCacheBytes = (kSlotCache + 1) * kSlotSpan;
constexpr size_t kTempoBytes = 256;
constexpr size_t kCountBytes = 256;
constexpr size_t kBlockBytes = kRunBytes + kKeyBytes + kCandBytes + kCacheBytes + kTempoBytes + kCountBytes;
}  // namespace

bool Engine::begin(AllocFn alloc, FreeFn release) {
  static_assert(sizeof(RunEntry) == 8 && sizeof(Cand) == 4 && sizeof(Counts) == kCountBytes, "the block's layout");
  end();
  alloc_ = alloc ? alloc : defaultAlloc;
  free_ = release ? release : defaultFree;
  block_ = alloc_(kBlockBytes);
  if (!block_) return false;
  uint8_t* p = static_cast<uint8_t*>(block_);
  runs_ = reinterpret_cast<RunEntry*>(p);
  p += kRunBytes;
  keys_ = reinterpret_cast<uint32_t*>(p);
  p += kKeyBytes;
  cand_ = reinterpret_cast<Cand*>(p);
  p += kCandBytes;
  counts_ = reinterpret_cast<Counts*>(p);
  p += kCountBytes;
  tempo_ = p;
  p += kTempoBytes;
  slotBytes_ = p;
  fileReopened();
  return true;
}

void Engine::end() {
  if (free_ && block_) free_(block_);
  block_ = nullptr;
  runs_ = nullptr;
  keys_ = nullptr;
  cand_ = nullptr;
  counts_ = nullptr;
  tempo_ = nullptr;
  slotBytes_ = nullptr;
  for (CacheSlot& c : cache_) c = CacheSlot();
}

size_t Engine::heldBytes() const { return block_ ? kBlockBytes : 0; }

// ---------------------------------------------------------------------------
// The session
// ---------------------------------------------------------------------------
void Engine::newSession(uint32_t seedValue, uint32_t seedTrack, const Maps* maps) {
  ses_ = Session();
  ses_.rng.reseed(seedValue);
  if (seedTrack == kNoTrackId) return;
  seed(seedTrack, maps, true);
  if (!maps) ses_.seedAwaits = seedTrack;
}

void Engine::mapsArrived(const Maps& maps) {
  const uint32_t track = ses_.seedAwaits;
  ses_.seedAwaits = kNoTrackId;
  if (track == kNoTrackId || ses_.pin != kNoRow) return;
  ses_.pin = rowOf(&maps, track);
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

bool Engine::trackLeft(uint32_t track, const Maps* maps, uint32_t playedMs, bool naturalEnd) {
  return rowLeft(rowOf(maps, track), playedMs, naturalEnd);
}

bool Engine::rowLeft(uint16_t row, uint32_t playedMs, bool naturalEnd) {
  if (naturalEnd || playedMs >= kQuickSkipMs) return false;
  ++stats_.quickSkips;
  return row != kNoRow && ses_.ring.remove(row);
}

void Engine::pickRefused(uint16_t row) {
  if (row != kNoRow) ses_.ring.remove(row);
}

void Engine::modeChanged() {
  ses_.pin = kNoRow;
  ses_.seedAwaits = kNoTrackId;
}

void Engine::fileReopened() {
  for (CacheSlot& c : cache_) c = CacheSlot();
  damaged_.clear();
  suspect_ = kNoRow;
  broken_ = false;
}

void Engine::tracksRenumbered() {
  ses_.ids.clear();
  ses_.seedAwaits = kNoTrackId;
}

void Engine::copySession(const Engine& from) {
  ses_ = from.ses_;
  clock_ = from.clock_;
  mapsGen_ = from.mapsGen_;
  print_ = from.print_;
  damaged_ = from.damaged_;
  suspect_ = from.suspect_;
  broken_ = from.broken_;
  stats_ = from.stats_;
  for (uint8_t i = 0; i < kSlotCache; ++i) {
    cache_[i] = from.cache_[i];
    if (cache_[i].row == kNoRow) continue;
    if (slotBytes_ && from.slotBytes_) {
      std::memcpy(slotBytes_ + i * kSlotSpan, from.slotBytes_ + i * kSlotSpan, kSlotSpan);
      std::memcpy(runs_ + i * kRunSpan, from.runs_ + i * kRunSpan, kRunSpan * sizeof(RunEntry));
    } else {
      cache_[i] = CacheSlot();
    }
  }
}

// ---------------------------------------------------------------------------
// The slot cache, reads and runs
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
  cache_[at] = CacheSlot();
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

Engine::MapsPrint Engine::printOf(const Maps& m) {
  MapsPrint p;
  p.filters = m.filters;
  p.rowToTrack = m.rowToTrack;
  p.rows = m.rows;
  p.tracks = m.tracks;
  p.mapped = m.mapped;
  return p;
}

bool Engine::runFresh(int i, const Maps& m, uint32_t k) const {
  return cache_[i].runGen == mapsGen_ && cache_[i].runK == k && print_ == printOf(m);
}

// Slot i's run for these maps and k: its first min(count, k) entries (the
// levels from byte 2k), those whose row maps to a track, sorted by row
// (then by place: a row listed twice keeps both, as the union does).
void Engine::buildRun(int i, const Maps& m, uint32_t k) {
  const MapsPrint p = printOf(m);
  if (!(print_ == p)) {
    print_ = p;
    ++mapsGen_;  // other maps: every run made before is stale
  }
  const uint8_t* b = slotBytes_ + i * kSlotSpan;
  const uint32_t n = cache_[i].count < k ? cache_[i].count : k;
  uint32_t nk = 0;
  for (uint32_t j = 0; j < n; ++j) {
    const uint16_t row = cardcontract::get16(b + 2 * j);
    if (row >= m.rows || m.rowToTrack[row] == kNoTrack) continue;
    keys_[nk++] = static_cast<uint32_t>(row) << 8 | j;
  }
  idsort::sort(keys_, keys_ + nk, std::less<uint32_t>());
  RunEntry* r = runs_ + i * kRunSpan;
  for (uint32_t q = 0; q < nk; ++q) {
    const uint16_t row = static_cast<uint16_t>(keys_[q] >> 8);
    r[q] = RunEntry{row, b[2 * k + (keys_[q] & 0xFF)], 0, m.filters[row]};
  }
  r[nk] = RunEntry{kNoRow, 0, 0, 0};
  cache_[i].runGen = mapsGen_;
  cache_[i].runK = static_cast<uint16_t>(k);
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

SlotResult Engine::prefetch(SlotReader& slots, uint16_t row, const Maps* maps, uint16_t k) {
  ++stats_.prefetchReads;
  if (!slotBytes_) return SlotResult::Io;
  const SlotResult r = read(slots, row);
  if (r == SlotResult::Ok && maps && maps->rows && k) buildRun(find(row), *maps, k <= mpdj::kMaxK ? k : mpdj::kMaxK);
  return r;
}

bool Engine::runDue(const Settings& s, const Maps& maps, uint16_t k, uint16_t* row) const {
  if (broken_ || !slotBytes_ || maps.rows == 0 || k == 0) return false;
  const uint32_t kk = k <= mpdj::kMaxK ? k : mpdj::kMaxK;
  auto due = [&](uint16_t r) {
    if (r == kNoRow || damaged_.has(r)) return false;
    const int i = find(r);
    return i >= 0 && !runFresh(i, maps, kk);
  };
  if (s.mode == Mode::Locked) {
    if (!due(ses_.pin)) return false;
    *row = ses_.pin;
    return true;
  }
  for (int i = ses_.ring.size() - 1; i >= 0; --i) {
    const uint16_t r = ses_.ring.at(static_cast<uint8_t>(i));
    if (!due(r)) continue;
    *row = r;
    return true;
  }
  return false;
}

void Engine::makeRun(uint16_t row, const Maps& maps, uint16_t k) {
  const int i = find(row);
  if (i < 0 || !slotBytes_ || maps.rows == 0 || k == 0) return;
  buildRun(i, maps, k <= mpdj::kMaxK ? k : mpdj::kMaxK);
}

// ---------------------------------------------------------------------------
// The pick
// ---------------------------------------------------------------------------
// The union of the usable slots' runs in row order: visit(row, best level,
// level sum, word) for each row but the anchors and the playing track's.
template <typename Visit>
void Engine::merge(const int8_t* usable, uint8_t nu, const Scope& sc, Visit&& visit) const {
  // The runs' next entries, as heads (row << 8 | run) kept in order (an
  // insertion sort of at most 5, then a sentinel): the first is the union's
  // next row. A run moved on goes back in its place, past the heads below
  // its next row (two on average).
  const RunEntry* at[kAnchors];
  uint32_t head[kAnchors + 1];
  for (uint8_t u = 0; u < nu; ++u) {
    at[u] = runs_ + usable[u] * kRunSpan;
    const uint32_t h = static_cast<uint32_t>(at[u]->row) << 8 | u;
    uint8_t i = u;
    for (; i > 0 && head[i - 1] > h; --i) head[i] = head[i - 1];
    head[i] = h;
  }
  head[nu] = 0xFFFFFFFFu;
  const uint16_t* anc = sc.anc;
  while (nu) {
    const uint32_t row = head[0] >> 8;
    if (row == kNoRow) return;
    uint32_t best = 0, sum = 0, word = 0;
    do {
      const uint32_t u = head[0] & 0xFF;
      const RunEntry* p = at[u];
      do {
        const uint32_t level = p->level;
        best = level > best ? level : best;
        sum += level;
        word = p->word;
        ++p;
      } while (p->row == row);
      at[u] = p;
      const uint32_t h = static_cast<uint32_t>(p->row) << 8 | u;
      uint32_t* q = head;
      for (; q[1] < h; ++q) q[0] = q[1];
      *q = h;
    } while (head[0] >> 8 == row);
    if (row == sc.playingRow) continue;
    while (*anc < row) ++anc;
    if (*anc == row) continue;
    visit(static_cast<uint16_t>(row), static_cast<uint8_t>(best), static_cast<uint16_t>(sum), word);
  }
}

// The kDry candidates (all, or only those outside the track cooldown) with
// the highest sums, ties by row, into cand_ in row order, counted into
// *counts; their count. None (0): cand_ and *counts are left as they were.
uint32_t Engine::closest(const int8_t* usable, uint8_t nu, const Scope& sc, bool freshOnly, Counts* counts) {
  uint32_t n = 0;
  const uint16_t* cp = sc.cool;
  merge(usable, nu, sc, [&](uint16_t row, uint8_t, uint16_t sum, uint32_t) {
    while (*cp < row) ++cp;
    if (!freshOnly || *cp != row) keys_[n++] = dryKey(sum, row);
  });
  if (n == 0) return 0;
  const uint32_t keep = n < kDry ? n : kDry;
  std::nth_element(keys_, keys_ + (keep - 1), keys_ + n);
  const uint32_t last = keys_[keep - 1];
  *counts = Counts();
  uint32_t ng = 0;
  cp = sc.cool;
  merge(usable, nu, sc, [&](uint16_t row, uint8_t best, uint16_t sum, uint32_t w) {
    while (*cp < row) ++cp;
    const bool cooled = *cp == row;
    if ((freshOnly && cooled) || dryKey(sum, row) > last) return;
    const uint8_t b = sc.bits(cooled ? 2 : sc.artistCooled(w) ? 1 : 0, w);
    counts->add(b);
    cand_[ng++] = Cand{row, best, b};
  });
  return ng;
}

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
          // (At most one a read: within kReadsPerPick.)
          if (res == SlotResult::Damaged && out.damaged < kReadsPerPick) out.damagedRow[out.damaged++] = r;
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
  const Scope sc(ses_, s, m, playingRow, anchors, na, in.selFloor, tempo_);
  if (broken_ || nu == 0) {
    // A damaged pin would make every Stay on seed pick random until the
    // file is reopened: this pick's row becomes the pin, as for a seed
    // without a row.
    if (s.mode == Mode::Locked && damaged_.has(ses_.pin)) ses_.pin = kNoRow;
    return randomPick(s, in, sc, out);
  }
  for (int u = nu - 1; u >= 0; --u) touch(usable[u]);

  const uint32_t k = in.k <= mpdj::kMaxK ? in.k : mpdj::kMaxK;
  const Rng start = ses_.rng;
  for (uint8_t attempt = 0;; ++attempt) {
    for (uint8_t u = 0; u < nu; ++u)
      if (!runFresh(usable[u], m, k)) buildRun(usable[u], m, k);

    // 2-3. The candidates, counted, and the gate's pool (with each one's
    // cooldown level and statuses), counted. Matching off, every status is
    // Na: the pool's bits are its cooldown levels, its classes 0, 8, 16.
    uint32_t nc = 0, ng = 0;
    Counts& counts = *counts_;
    counts = Counts();
    const uint16_t* cp = sc.cool;
    auto census = [&](auto count) {
      merge(usable, nu, sc, [&](uint16_t row, uint8_t best, uint16_t, uint32_t w) {
        ++nc;
        if (best < sc.gate) return;
        while (*cp < row) ++cp;
        cand_[ng++] = Cand{row, best, count(*cp == row ? 2 : sc.artistCooled(w) ? 1 : 0, w)};
      });
    };
    if (sc.ranked()) {
      census([&](uint8_t cl, uint32_t w) {
        const uint8_t b = sc.bits(cl, w);
        counts.add(b);
        return b;
      });
    } else {
      census([&](uint8_t cl, uint32_t) {
        counts.add(cl);
        return cl;
      });
    }
    out.candidates = static_cast<uint16_t>(nc);
    out.dry = out.loosened = false;
    if (nc == 0) return randomPick(s, in, sc, out);
    // Dry: the kDry best sums. Every row of the pool cooled: loosen before
    // repeating, the kDry best sums outside the track cooldown, if any.
    if (ng == 0) {
      out.dry = true;
      ng = closest(usable, nu, sc, false, &counts);
    }
    if (counts.fresh() == 0) {
      const uint32_t fresh = closest(usable, nu, sc, true, &counts);
      if (fresh) {
        out.loosened = true;
        ng = fresh;
      }
    }
    out.gated = static_cast<uint16_t>(ng);

    // 4-5. The classes, the draw, the pick.
    const uint32_t j = choose(sc, m, ng, counts, out);
    const uint16_t row = cand_[j].row;
    if (attempt == 0 && (row >= m.rows || m.rowToTrack[row] == kNoTrack)) {
      // Runs from maps that changed in place, unannounced: remade, and the
      // same stream picks again.
      ++mapsGen_;
      ses_.rng = start;
      continue;
    }
    out.level = cand_[j].best;
    return finish(s, m, row, out);
  }
}

Pick Engine::randomPick(const Settings& s, const PickIn& in, const Scope& sc, Pick out) {
  out.random = true;
  const Maps& m = *in.maps;
  uint32_t n = 0;
  Counts& counts = *counts_;
  counts = Counts();
  for (uint8_t d = 0; d < kRandomDraws; ++d) {
    const uint16_t r = static_cast<uint16_t>(ses_.rng.below(m.rows));
    if (r == sc.playingRow || m.rowToTrack[r] == kNoTrack) continue;
    bool skip = false;
    for (const uint16_t* a = sc.anc; *a != kNoRow && !skip; ++a) skip = *a == r;
    for (uint32_t i = 0; i < n && !skip; ++i) skip = cand_[i].row == r;
    if (skip) continue;
    const uint32_t w = m.filters[r];
    const uint8_t b = sc.bits(ses_.cool.has(r) ? 2 : sc.artistCooled(w) ? 1 : 0, w);
    counts.add(b);
    cand_[n++] = Cand{r, 0, b};
  }
  out.candidates = out.gated = static_cast<uint16_t>(n);
  if (n == 0) return byId(s, in, out);
  const uint32_t j = choose(sc, m, n, counts, out);
  out.level = 0;
  return finish(s, m, cand_[j].row, out);
}

// The pool's classes and the draw; `counts` the pool's (classes before D3).
uint32_t Engine::choose(const Scope& sc, const Maps& m, uint32_t n, Counts& counts, Pick& out) {
  const bool ranked = sc.ranked();
  if (ranked) {
    // D3: every unknown tempo, then every unknown key, in the pool's order,
    // with the odds of the pool's known ones; each drawn one changes class.
    uint32_t tempo[8] = {}, key[4] = {};
    counts.statuses(tempo, key);
    TempoOdds to;
    to.tight = tempo[static_cast<int>(Tempo::Tight)];
    to.wide = tempo[static_cast<int>(Tempo::Wide)];
    to.wrong = tempo[static_cast<int>(Tempo::Wrong)];
    KeyOdds ko;
    ko.good = key[static_cast<int>(Key::Good)];
    ko.wrong = key[static_cast<int>(Key::Wrong)];
    if (to.known() && tempo[static_cast<int>(Tempo::Unknown)])
      for (uint32_t i = 0; i < n; ++i) {
        Cand& c = cand_[i];
        if (tempoOf(c.bits) != Tempo::Unknown) continue;
        const uint8_t was = c.bits;
        c.bits = withTempo(c.bits, to.draw(ses_.rng));
        counts.move(was, c.bits);
      }
    if (ko.known() && key[static_cast<int>(Key::Unknown)])
      for (uint32_t i = 0; i < n; ++i) {
        Cand& c = cand_[i];
        if (keyOf(c.bits) != Key::Unknown) continue;
        const uint8_t was = c.bits;
        c.bits = withKey(c.bits, ko.draw(ses_.rng));
        counts.move(was, c.bits);
      }
  }
  uint32_t inBest = 0;
  const uint8_t best = counts.best(&inBest);  // (n > 0: one class holds them)
  uint32_t nth = ses_.rng.below(inBest);
  uint32_t j = 0;
  for (; j < n; ++j)
    if (classOf(cand_[j].bits) == best && nth-- == 0) break;
  const Cand& c = cand_[j];
  out.cooldownLevel = best / kRanks;
  out.musicRank = best % kRanks;
  out.inClass = static_cast<uint16_t>(inBest);
  if (ranked) {
    // The statuses it had before D3 (its row's word: the run's).
    const uint32_t w = m.filters[c.row];
    const Tempo t = static_cast<Tempo>(sc.tempo[filterBpm(w)]);
    const Key k = sc.key.status(filterCamelot(w));
    out.imputedTempo = t != tempoOf(c.bits);
    out.imputedKey = k != keyOf(c.bits);
    out.tempoBreak = t == Tempo::Wide || t == Tempo::Wrong;
    out.keyBreak = k == Key::Wrong;
  }
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
  stats_.loosened += out.loosened;
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
