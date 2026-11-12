// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsFold.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace statsfold {

namespace cc = cardcontract;
using playlog::Slot;

namespace {

void* mallocAlloc(size_t n) { return std::malloc(n); }
void mallocFree(void* p) { std::free(p); }

constexpr uint16_t kEmpty = 0xFFFF;  // an index entry naming no row: rows 0 to 0xFFFE are indexed
constexpr uint32_t kTail = 64;       // unsorted rows a table keeps before it sorts them
constexpr uint32_t kMinIndex = 256;

uint64_t add64(uint64_t a, uint64_t b) {
  const uint64_t s = a + b;
  return s < a ? ~0ull : s;
}
uint32_t add32(uint32_t a, uint64_t b) {
  const uint64_t s = a + b;
  return s > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(s);
}
uint16_t add16(uint16_t a, uint32_t b) {
  const uint32_t s = a + b;
  return static_cast<uint16_t>(s > 0xFFFFu ? 0xFFFFu : s);
}
uint8_t add8(uint8_t a, uint32_t b) {
  const uint32_t s = a + b;
  return static_cast<uint8_t>(s > 0xFFu ? 0xFFu : s);
}
uint32_t round24(uint32_t r) { return r > kMaxRound ? kMaxRound : r; }

uint32_t mix32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7FEB352Du;
  x ^= x >> 15;
  x *= 0x846CA68Bu;
  x ^= x >> 16;
  return x;
}

// firstAt's order: a dated time, then an undated play (0), then none.
uint64_t firstKey(uint32_t t) { return t == 0 ? 1ull << 32 : t == kNoFirst ? (1ull << 32) + 1 : t; }

int cmpPair(const uint8_t* devA, uint64_t epochA, const uint8_t* devB, uint64_t epochB) {
  const int c = std::memcmp(devA, devB, 6);
  if (c != 0) return c;
  return epochA < epochB ? -1 : epochA > epochB ? 1 : 0;
}

using MarkRow = Fold::MarkRow;
using HourRow = Fold::HourRow;
using DaysRow = Fold::DaysRow;
using RcntRow = Fold::RcntRow;
using TrkdRow = Fold::TrkdRow;
// RAM's rows are the file's sizes (5.6's blocks: 1,536 B of MARK, 6,720 B
// of HOUR, 6,400 B of DAYS; 1,366 RCNT rows and 683 TRKD rows a chunk).
static_assert(sizeof(MarkRow) == 24 && sizeof(HourRow) == 8 && sizeof(DaysRow) == 16, "the rows' sizes");
static_assert(sizeof(RcntRow) == 12 && sizeof(TrkdRow) == 24, "the chunked rows' sizes");
// A chunk holds whole rows of both tables, nothing over: RCNT at its cap
// in three chunks, TRKD at the forced compaction's 96 KB in six (the
// plan's 11).
static_assert(kChunkBytes % sizeof(RcntRow) == 0 && kChunkBytes % sizeof(TrkdRow) == 0, "whole rows a chunk");
static_assert(kRcntRows * sizeof(RcntRow) <= 3 * kChunkBytes && 96 * 1024 <= 6 * kChunkBytes, "the caps' chunks");

// The chunked tables' keys.
struct RcntKeys {
  using Row = RcntRow;
  struct Key {
    uint16_t day;
    uint64_t hash;
  };
  static uint64_t hashOf(const Row& r) { return static_cast<uint64_t>(r.hi) << 32 | r.lo; }
  static Key keyOf(const Row& r) { return {r.day, hashOf(r)}; }
  static bool less(const Row& a, const Row& b) { return a.day != b.day ? a.day < b.day : hashOf(a) < hashOf(b); }
  static bool lessKey(const Row& a, const Key& k) { return a.day != k.day ? a.day < k.day : hashOf(a) < k.hash; }
  static bool same(const Row& a, const Key& k) {
    return a.day == k.day && a.lo == static_cast<uint32_t>(k.hash) && a.hi == static_cast<uint32_t>(k.hash >> 32);
  }
  static uint32_t mixOf(const Key& k) {
    return mix32(static_cast<uint32_t>(k.hash) ^ mix32(static_cast<uint32_t>(k.hash >> 32) ^ k.day));
  }
};
struct TrkdKeys {
  using Row = TrkdRow;
  using Key = uint64_t;
  static Key keyOf(const Row& r) { return r.hash; }
  static bool less(const Row& a, const Row& b) { return a.hash < b.hash; }
  static bool lessKey(const Row& a, const Key& k) { return a.hash < k; }
  static bool same(const Row& a, const Key& k) { return a.hash == k; }
  static uint32_t mixOf(const Key& k) { return mix32(static_cast<uint32_t>(k) ^ mix32(static_cast<uint32_t>(k >> 32))); }
};
// The capped tables' keys.
struct HourKeys {
  static uint32_t keyOf(const HourRow& r) { return r.hourPlays & 0xFFFFFFu; }
};
struct DaysKeys {
  static uint32_t keyOf(const DaysRow& r) { return r.day; }
};

template <class K, class Tab>
void indexPut(Tab& t, uint32_t row) {
  const uint32_t mask = t.ix.size - 1;
  uint32_t i = K::mixOf(K::keyOf(t.at(row))) & mask;
  while (t.ix.e[i] != kEmpty) i = (i + 1) & mask;
  t.ix.e[i] = static_cast<uint16_t>(row);
  ++t.ix.count;
}

// A row by key: the index while there is one, else each chunk's sorted
// rows (a binary search) and its unsorted tail. -1: none.
template <class K, class Tab>
int64_t findRow(Tab& t, const typename K::Key& k) {
  if (t.ix.e) {
    const uint32_t mask = t.ix.size - 1;
    for (uint32_t i = K::mixOf(k) & mask;; i = (i + 1) & mask) {
      const uint16_t r = t.ix.e[i];
      if (r == kEmpty) return -1;
      if (K::same(t.at(r), k)) return r;
    }
  }
  for (uint32_t c = 0; c < t.chunks; ++c) {
    const uint32_t n = t.inChunk(c);
    if (n == 0) break;
    const typename K::Row* rows = t.c[c];
    uint32_t lo = 0, hi = t.sorted[c];
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      if (K::lessKey(rows[mid], k)) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    if (lo < t.sorted[c] && K::same(rows[lo], k)) return static_cast<int64_t>(c) * Tab::kPer + lo;
    for (uint32_t i = t.sorted[c]; i < n; ++i)
      if (K::same(rows[i], k)) return static_cast<int64_t>(c) * Tab::kPer + i;
  }
  return -1;
}

// Each chunk's rows in order (no index may be standing on them).
template <class K, class Tab>
void sortChunks(Tab& t) {
  for (uint32_t c = 0; c < t.chunks; ++c) {
    const uint32_t n = t.inChunk(c);
    if (t.sorted[c] < n) std::sort(t.c[c], t.c[c] + n, K::less);
    t.sorted[c] = static_cast<uint16_t>(n);
  }
  t.unsorted = 0;
}

// The first row of a sorted chunk at or past `day` (RCNT).
uint32_t firstAtDay(const RcntRow* rows, uint32_t n, uint32_t day) {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (rows[mid].day < day) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

template <class Row, class K>
Row* cappedFind(Fold::Capped<Row>& t, uint32_t key) {
  if (t.hit < t.n && K::keyOf(t.rows[t.hit]) == key) return &t.rows[t.hit];
  uint32_t lo = t.s0, hi = t.s1;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (K::keyOf(t.rows[mid]) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo < t.s1 && K::keyOf(t.rows[lo]) == key) {
    t.hit = lo;
    return &t.rows[lo];
  }
  for (uint32_t i = 0; i < t.n; ++i) {
    if (i == t.s0) i = t.s1;  // (the sorted run searched above)
    if (i >= t.n) break;
    if (K::keyOf(t.rows[i]) == key) {
      t.hit = i;
      return &t.rows[i];
    }
  }
  return nullptr;
}

template <class Row, class K>
void cappedSort(Fold::Capped<Row>& t) {
  std::sort(t.rows, t.rows + t.n, [](const Row& a, const Row& b) { return K::keyOf(a) < K::keyOf(b); });
  t.s0 = 0;
  t.s1 = t.n;
  t.hit = 0;
}

// A new key: appended; a full table's oldest row gives its place, unless
// the new key is older than every row kept (then it is never among the
// newest `cap`, and its slot's part of this table goes: what a save's
// retention would drop).
template <class Row, class K>
void cappedAdd(Fold::Capped<Row>& t, const Row& r) {
  const uint32_t key = K::keyOf(r);
  if (t.n < t.cap) {
    t.rows[t.n] = r;
    t.hit = t.n++;
  } else {
    uint32_t v = t.n, vk = 0xFFFFFFFFu;
    if (t.s0 < t.s1) {
      v = t.s0;
      vk = K::keyOf(t.rows[t.s0]);
    }
    for (uint32_t i = 0; i < t.n; ++i) {
      if (i == t.s0) i = t.s1;
      if (i >= t.n) break;
      if (K::keyOf(t.rows[i]) < vk) {
        v = i;
        vk = K::keyOf(t.rows[i]);
      }
    }
    if (v == t.n || key < vk) return;
    t.rows[v] = r;
    t.hit = v;
    if (v == t.s0 && t.s0 < t.s1) ++t.s0;
  }
  if (t.s0 + (t.n - t.s1) > kTail) cappedSort<Row, K>(t);
}

void encodeMark(const MarkRow& m, uint8_t* p) {
  std::memcpy(p, m.dev, 6);
  p[6] = p[7] = 0;
  cc::put64(p + 8, m.epoch);
  cc::put32(p + 16, m.seq);
  cc::put32(p + 20, 0);
}
void encodeHour(const HourRow& h, uint8_t* p) {
  cc::put32(p, h.hourPlays);
  cc::put32(p + 4, h.ms);
}
void encodeDays(const DaysRow& d, uint8_t* p) {
  cc::put16(p, d.day);
  cc::put16(p + 2, d.plays);
  cc::put16(p + 4, d.skips);
  p[6] = d.djPlays;
  p[7] = d.djSkips;
  cc::put32(p + 8, d.ms);
  p[12] = d.shufPlays;
  p[13] = d.shufSkips;
  p[14] = p[15] = 0;
}
void encodeRcnt(const RcntRow& r, uint8_t* p) {
  cc::put16(p, r.day);
  cc::put16(p + 2, r.plays);
  cc::put32(p + 4, r.lo);
  cc::put32(p + 8, r.hi);
}
Track toTrack(const TrkdRow& r) {
  Track t;
  t.hash = r.hash;
  t.plays = r.plays;
  t.skips = r.skips;
  t.firstAt = r.firstAt;
  t.lastAt = r.lastAt;
  t.lastOrd = r.ord & kMaxRound;
  t.src = static_cast<uint8_t>(r.ord >> 24 & 3);
  return t;
}
TrkdRow toRow(const Track& t) {
  TrkdRow r;
  r.hash = t.hash;
  r.plays = t.plays;
  r.skips = t.skips;
  r.firstAt = t.firstAt;
  r.lastAt = t.lastAt;
  r.ord = (t.lastOrd & kMaxRound) | static_cast<uint32_t>(t.src & 3) << 24;
  return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// The rules (4.1).
// ---------------------------------------------------------------------------
bool counts(const Slot& s) {
  if (s.kind != playlog::kKindPlay || s.epoch == 0 || s.seq == 0) return false;
  for (int i = 0; i < 6; ++i)
    if (s.deviceId[i] != 0) return true;
  return false;
}

bool isPlay(const Slot& s) {
  const uint64_t line = s.durationMs ? std::min<uint64_t>(2ull * kLineMs, s.durationMs) : 2ull * kLineMs;
  return 2ull * s.playedMs >= line;
}

bool isSkip(const Slot& s) { return s.outcome == playlog::kSkipped && !isPlay(s); }

uint8_t sourceOf(const Slot& s) {
  return s.source == playlog::kSourceAutoDj ? kAutoDj : s.source == playlog::kSourceShuffle ? kShuffle : kYou;
}

bool todayOf(uint64_t nowMs, uint16_t* day) {
  if (!dated(nowMs)) return false;
  *day = dayOf(nowMs);
  return true;
}

Track merge(const Track& a, const Track& b) {
  Track t;
  t.hash = a.hash;
  t.plays = add16(a.plays, b.plays);
  t.skips = add16(a.skips, b.skips);
  const uint64_t ka = firstKey(a.firstAt), kb = firstKey(b.firstAt);
  const Track& first = ka < kb || (ka == kb && a.src <= b.src) ? a : b;
  t.firstAt = first.firstAt;
  t.src = first.src;
  t.lastAt = std::max(a.lastAt, b.lastAt);
  t.lastOrd = std::max(a.lastOrd, b.lastOrd);
  return t;
}

bool slotTrack(const Slot& s, uint32_t round, Track* out) {
  Track t;
  t.hash = s.pathHash;
  if (isPlay(s)) {
    const uint32_t at = dated(s.startedAt) ? timeOf(s.startedAt) : 0;
    t.plays = 1;
    t.firstAt = t.lastAt = at;
    t.lastOrd = round24(round);
    t.src = sourceOf(s);
  } else if (isSkip(s)) {
    t.skips = 1;  // (firstAt none, lastAt and lastOrd 0, you)
  } else {
    return false;
  }
  *out = t;
  return true;
}

bool compactionDue(uint32_t deltaRows, uint32_t baseRows, bool playing, bool safePoint) {
  const uint64_t d = static_cast<uint64_t>(deltaRows) * kTrackBytes;
  if (d > 96u * 1024u && (safePoint || !playing)) return true;
  return !playing && (d > 32u * 1024u || 4 * d > static_cast<uint64_t>(baseRows) * kTrackBytes);
}

// ---------------------------------------------------------------------------
// HEAD and a track's row, as the file has them.
// ---------------------------------------------------------------------------
void encodeHead(const Head& h, uint8_t out[kHeadBytes]) {
  std::memset(out, 0, kHeadBytes);
  cc::put64(out, h.plays);
  cc::put64(out + 8, h.skips);
  cc::put64(out + 16, h.listenedMs);
  cc::put64(out + 24, h.slots);
  for (int i = 0; i < 3; ++i) {
    uint8_t* p = out + 32 + 16 * i;
    cc::put32(p, h.src[i].plays);
    cc::put32(p + 4, h.src[i].skips);
    cc::put64(p + 8, h.src[i].listenedMs);
  }
  cc::put32(out + 80, h.undated);
  cc::put16(out + 84, h.sinceDay);
  cc::put16(out + 86, h.flags);
  cc::put32(out + 88, h.round);
  cc::put32(out + 92, h.baseGen);
  cc::put32(out + 96, h.baseRows);
  cc::put32(out + 100, h.baseCrc);
  cc::put32(out + 104, h.twice);
  cc::put32(out + 108, h.gap);
}

Head decodeHead(const uint8_t in[kHeadBytes]) {
  Head h;
  h.plays = cc::get64(in);
  h.skips = cc::get64(in + 8);
  h.listenedMs = cc::get64(in + 16);
  h.slots = cc::get64(in + 24);
  for (int i = 0; i < 3; ++i) {
    const uint8_t* p = in + 32 + 16 * i;
    h.src[i].plays = cc::get32(p);
    h.src[i].skips = cc::get32(p + 4);
    h.src[i].listenedMs = cc::get64(p + 8);
  }
  h.undated = cc::get32(in + 80);
  h.sinceDay = cc::get16(in + 84);
  h.flags = cc::get16(in + 86);
  h.round = cc::get32(in + 88);
  h.baseGen = cc::get32(in + 92);
  h.baseRows = cc::get32(in + 96);
  h.baseCrc = cc::get32(in + 100);
  h.twice = cc::get32(in + 104);
  h.gap = cc::get32(in + 108);
  return h;
}

void encodeTrack(const Track& t, uint8_t out[kTrackBytes]) {
  cc::put64(out, t.hash);
  cc::put16(out + 8, t.plays);
  cc::put16(out + 10, t.skips);
  cc::put32(out + 12, t.firstAt);
  cc::put32(out + 16, t.lastAt);
  cc::put32(out + 20, (t.lastOrd & kMaxRound) | static_cast<uint32_t>(t.src & 3) << 24);
}

Track decodeTrack(const uint8_t in[kTrackBytes]) {
  Track t;
  t.hash = cc::get64(in);
  t.plays = cc::get16(in + 8);
  t.skips = cc::get16(in + 10);
  t.firstAt = cc::get32(in + 12);
  t.lastAt = cc::get32(in + 16);
  const uint32_t w = cc::get32(in + 20);
  t.lastOrd = w & kMaxRound;
  t.src = static_cast<uint8_t>(w >> 24 & 3);
  return t;
}

// ---------------------------------------------------------------------------
// The memory.
// ---------------------------------------------------------------------------
Fold::Fold(AllocFn alloc, FreeFn release)
    : alloc_(alloc ? alloc : mallocAlloc), free_(release ? release : mallocFree) {}

void* Fold::grab(size_t bytes) {
  void* p = alloc_(bytes);
  if (p) {
    held_ += bytes;
    peak_ = std::max(peak_, held_);
  }
  return p;
}

void Fold::drop(void* p, size_t bytes) {
  free_(p);
  held_ -= bytes;
}

void* Fold::takeChunk() {
  void* p = spare_;
  spare_ = nullptr;
  return p ? p : grab(kChunkBytes);
}

void Fold::giveChunk(void* p) {
  if (spare_) {
    drop(p, kChunkBytes);
  } else {
    spare_ = p;
  }
}

template <class Tab>
bool Fold::room(Tab& t, uint32_t more) {
  while (static_cast<uint64_t>(t.chunks) * Tab::kPer < static_cast<uint64_t>(t.rows) + more) {
    if (t.chunks == Tab::kMaxChunks) return false;
    void* p = takeChunk();
    if (!p) return false;
    t.c[t.chunks] = static_cast<typename Tab::RowType*>(p);
    t.sorted[t.chunks] = 0;
    ++t.chunks;
  }
  return true;
}

template <class Tab>
void Fold::shrink(Tab& t) {
  const uint32_t need = (t.rows + Tab::kPer - 1) / Tab::kPer;
  while (t.chunks > need) {
    --t.chunks;
    giveChunk(t.c[t.chunks]);
    t.c[t.chunks] = nullptr;
    t.sorted[t.chunks] = 0;
  }
}

template <class Tab>
void Fold::releaseAll(Tab& t) {
  while (t.chunks > 0) {
    --t.chunks;
    drop(t.c[t.chunks], kChunkBytes);
    t.c[t.chunks] = nullptr;
    t.sorted[t.chunks] = 0;
  }
  t.rows = t.unsorted = 0;
}

template <class K, class Tab>
bool Fold::buildIndex(Tab& t, uint32_t atLeast) {
  if (t.rows > kEmpty || atLeast > kEmpty) return false;  // (row numbers are u16)
  uint32_t size = kMinIndex;
  while (size < 2 * std::max(atLeast, t.rows)) size <<= 1;
  uint16_t* e = static_cast<uint16_t*>(grab(size * sizeof(uint16_t)));
  if (!e) return false;
  std::memset(e, 0xFF, size * sizeof(uint16_t));
  freeIndex(t);  // (the old one after the new one is carved: 5.6)
  t.ix.e = e;
  t.ix.size = size;
  t.ix.count = 0;
  for (uint32_t r = 0; r < t.rows; ++r) indexPut<K>(t, r);
  return true;
}

template <class Tab>
void Fold::freeIndex(Tab& t) {
  if (t.ix.e) drop(t.ix.e, t.ix.size * sizeof(uint16_t));
  t.ix = Index();
}

template <class K, class Tab>
bool Fold::append(Tab& t, const typename K::Row& r) {
  if (!room(t, 1)) {
    noMemory_ = true;
    return false;
  }
  const uint32_t at = t.rows++;
  t.at(at) = r;
  ++t.unsorted;
  if (t.ix.e) {
    if (at < kEmpty && (t.ix.count + 1) * 2 <= t.ix.size) {
      indexPut<K>(t, at);
      return true;
    }
    // Past half full: carved again at twice the size and filled from the
    // rows (this one too). No memory for it, or past the rows a u16 names:
    // no index from here; each chunk's order finds the rows, as live
    // folds do.
    if (at < kEmpty && buildIndex<K>(t, t.rows)) return true;
    freeIndex(t);
    sortChunks<K>(t);
    return true;
  }
  if (t.unsorted > kTail) sortChunks<K>(t);
  return true;
}

Fold::~Fold() {
  booting_ = false;
  freeIndex(rcnt_);
  freeIndex(trkd_);
  releaseAll(rcnt_);
  releaseAll(trkd_);
  if (spare_) drop(spare_, kChunkBytes);
  if (ranges_) drop(ranges_, static_cast<size_t>(rangesCap_) * kFiles * sizeof(Range));
  if (acks_) drop(acks_, static_cast<size_t>(acksCap_) * sizeof(AckTrack));
  if (mark_) drop(mark_, static_cast<size_t>(markCap_) * sizeof(MarkRow));
  if (hour_.rows) drop(hour_.rows, static_cast<size_t>(hour_.cap) * sizeof(HourRow));
  if (days_.rows) drop(days_.rows, static_cast<size_t>(days_.cap) * sizeof(DaysRow));
}

bool Fold::begin() {
  if (!hour_.rows) {
    hour_.rows = static_cast<HourRow*>(grab(kHourRows * sizeof(HourRow)));
    if (!hour_.rows) return false;
    hour_.cap = kHourRows;
  }
  if (!days_.rows) {
    days_.rows = static_cast<DaysRow*>(grab(kDaysRows * sizeof(DaysRow)));
    if (!days_.rows) return false;
    days_.cap = kDaysRows;
  }
  if (!mark_) {
    mark_ = static_cast<MarkRow*>(grab(kMarkBlockRows * sizeof(MarkRow)));
    if (!mark_) return false;
    markCap_ = kMarkBlockRows;
  }
  return true;
}

void Fold::dropBoot() {
  booting_ = false;
  freeIndex(rcnt_);
  freeIndex(trkd_);
  if (ranges_) drop(ranges_, static_cast<size_t>(rangesCap_) * kFiles * sizeof(Range));
  ranges_ = nullptr;
  rangesCap_ = 0;
  if (acks_) drop(acks_, static_cast<size_t>(acksCap_) * sizeof(AckTrack));
  acks_ = nullptr;
  acksCap_ = acksN_ = ackHit_ = 0;
}

void Fold::clear() {
  dropBoot();
  head_ = Head();
  generation_ = loadedRound_ = used_ = 0;
  firstPlayDay_ = kUnsetDay;
  rcntFloor_ = 0;
  dirty_ = partial_ = noMemory_ = false;
  folded_ = twice_ = gap_ = 0;
  markN_ = markSorted_ = markHit_ = 0;
  hour_.n = hour_.s0 = hour_.s1 = hour_.hit = 0;
  days_.n = days_.s0 = days_.s1 = days_.hit = 0;
  rcnt_.rows = rcnt_.unsorted = 0;
  for (uint32_t c = 0; c < rcnt_.chunks; ++c) rcnt_.sorted[c] = 0;
  trkd_.rows = trkd_.unsorted = 0;
  for (uint32_t c = 0; c < trkd_.chunks; ++c) trkd_.sorted[c] = 0;
  for (bool& b : loadedAny_) b = false;
  plan_ = nullptr;
}

void Fold::release() {
  clear();
  releaseAll(rcnt_);
  releaseAll(trkd_);
  if (spare_) drop(spare_, kChunkBytes);
  spare_ = nullptr;
  if (mark_) drop(mark_, static_cast<size_t>(markCap_) * sizeof(MarkRow));
  mark_ = nullptr;
  markCap_ = 0;
  if (hour_.rows) drop(hour_.rows, static_cast<size_t>(hour_.cap) * sizeof(HourRow));
  hour_ = Capped<HourRow>();
  if (days_.rows) drop(days_.rows, static_cast<size_t>(days_.cap) * sizeof(DaysRow));
  days_ = Capped<DaysRow>();
}

size_t Fold::giveBack() {
  if (booting_) return 0;
  const size_t before = held_;
  plan_ = nullptr;
  releaseAll(rcnt_);
  releaseAll(trkd_);
  if (spare_) drop(spare_, kChunkBytes);
  spare_ = nullptr;
  if (hour_.rows) drop(hour_.rows, static_cast<size_t>(hour_.cap) * sizeof(HourRow));
  hour_ = Capped<HourRow>();
  if (days_.rows) drop(days_.rows, static_cast<size_t>(days_.cap) * sizeof(DaysRow));
  days_ = Capped<DaysRow>();
  return before - held_;
}

size_t Fold::givesBack() const {
  if (booting_) return 0;
  return held_ - static_cast<size_t>(markCap_) * sizeof(MarkRow);
}

bool Fold::reserveLoad(uint32_t rcntRows, uint32_t trkdRows) {
  return room(rcnt_, rcntRows) && room(trkd_, trkdRows) && keepSpare();
}

bool Fold::keepSpare() {
  if (!spare_) spare_ = grab(kChunkBytes);
  return spare_ != nullptr;
}

bool Fold::reserve(uint32_t slots) {
  bool ok = room(rcnt_, slots);
  ok = room(trkd_, slots) && ok;
  if (booting_) {
    // (Past what a u16 names an append drops the index itself.)
    if (rcnt_.ix.e && (rcnt_.ix.count + slots) * 2 > rcnt_.ix.size && rcnt_.rows + slots <= kEmpty)
      ok = buildIndex<RcntKeys>(rcnt_, rcnt_.rows + slots) && ok;
    if (trkd_.ix.e && (trkd_.ix.count + slots) * 2 > trkd_.ix.size && trkd_.rows + slots <= kEmpty)
      ok = buildIndex<TrkdKeys>(trkd_, trkd_.rows + slots) && ok;
  }
  return ok;
}

// ---------------------------------------------------------------------------
// MARK.
// ---------------------------------------------------------------------------
bool Fold::growMark() {
  const uint32_t cap = markCap_ ? markCap_ * 2 : kMarkBlockRows;
  MarkRow* m = static_cast<MarkRow*>(grab(static_cast<size_t>(cap) * sizeof(MarkRow)));
  if (!m) return false;
  Range* r = nullptr;
  if (booting_) {
    r = static_cast<Range*>(grab(static_cast<size_t>(cap) * kFiles * sizeof(Range)));
    if (!r) {
      drop(m, static_cast<size_t>(cap) * sizeof(MarkRow));
      return false;
    }
    std::memset(r, 0, static_cast<size_t>(cap) * kFiles * sizeof(Range));
    if (ranges_) {
      std::memcpy(r, ranges_, static_cast<size_t>(markN_) * kFiles * sizeof(Range));
      drop(ranges_, static_cast<size_t>(rangesCap_) * kFiles * sizeof(Range));
    }
    ranges_ = r;
    rangesCap_ = cap;
  }
  if (mark_) {
    std::memcpy(m, mark_, static_cast<size_t>(markN_) * sizeof(MarkRow));
    drop(mark_, static_cast<size_t>(markCap_) * sizeof(MarkRow));
  }
  mark_ = m;
  markCap_ = cap;
  return true;
}

Fold::MarkRow* Fold::findMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) const {
  if (markHit_ < markN_ && mark_[markHit_].epoch == epoch && std::memcmp(mark_[markHit_].dev, dev, 6) == 0) {
    if (at) *at = markHit_;
    return &mark_[markHit_];
  }
  uint32_t lo = 0, hi = markSorted_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (cmpPair(mark_[mid].dev, mark_[mid].epoch, dev, epoch) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  uint32_t i = lo;
  if (!(lo < markSorted_ && cmpPair(mark_[lo].dev, mark_[lo].epoch, dev, epoch) == 0)) {
    for (i = markSorted_; i < markN_; ++i)
      if (mark_[i].epoch == epoch && std::memcmp(mark_[i].dev, dev, 6) == 0) break;
    if (i == markN_) return nullptr;
  }
  markHit_ = i;
  if (at) *at = i;
  return &mark_[i];
}

Fold::MarkRow* Fold::addMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) {
  if (markN_ == markCap_ && !growMark()) return nullptr;
  if (booting_ && markN_ >= rangesCap_) return nullptr;  // (beginBoot sized them to MARK's block)
  MarkRow& m = mark_[markN_];
  std::memcpy(m.dev, dev, 6);
  m.pad = 0;
  m.epoch = epoch;
  m.seq = m.card = 0;
  markHit_ = markN_;
  if (at) *at = markN_;
  return &mark_[markN_++];
}

void Fold::sortMark() {
  if (markSorted_ < markN_)
    std::sort(mark_, mark_ + markN_,
              [](const MarkRow& a, const MarkRow& b) { return cmpPair(a.dev, a.epoch, b.dev, b.epoch) < 0; });
  markSorted_ = markN_;
  markHit_ = 0;
}

bool Fold::covered(const Slot& s) const {
  const MarkRow* m = findMark(s.deviceId, s.epoch, nullptr);
  return s.seq <= (m ? m->card : 0u);
}

uint32_t Fold::markOf(const uint8_t deviceId[6], uint64_t epoch) const {
  const MarkRow* m = findMark(deviceId, epoch, nullptr);
  return m ? m->seq : 0;
}

// ---------------------------------------------------------------------------
// The load.
// ---------------------------------------------------------------------------
void Fold::loadGeneration(uint32_t generation) { generation_ = generation; }

void Fold::loadHead(const uint8_t* bytes) {
  head_ = decodeHead(bytes);
  loadedRound_ = head_.round;
}

bool Fold::loadRows(Sec sec, const uint8_t* rows, uint32_t n, uint32_t stride) {
  if (sec == kHead || sec >= kSecs || stride < kStrides[sec]) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* p = rows + static_cast<size_t>(i) * stride;
    const bool first = !loadedAny_[sec];
    loadedAny_[sec] = true;
    switch (sec) {
      case kMark: {
        const uint64_t epoch = cc::get64(p + 8);
        if (!first && cmpPair(lastMark_, cc::get64(lastMark_ + 6), p, epoch) >= 0) return false;
        std::memcpy(lastMark_, p, 6);
        cc::put64(lastMark_ + 6, epoch);
        uint32_t at = 0;
        MarkRow* m = addMark(p, epoch, &at);
        if (!m) {
          noMemory_ = true;
          return false;
        }
        m->seq = m->card = cc::get32(p + 16);
        if (markSorted_ + 1 == markN_) markSorted_ = markN_;
        break;
      }
      case kHour: {
        HourRow h{cc::get32(p), cc::get32(p + 4)};
        const uint32_t key = h.hourPlays & 0xFFFFFFu;
        if (!first && key <= lastHour_) return false;
        lastHour_ = key;
        if (!hour_.rows) {
          noMemory_ = true;
          return false;
        }
        if (hour_.n < hour_.cap && hour_.s0 == 0 && hour_.s1 == hour_.n) {
          hour_.rows[hour_.n++] = h;
          hour_.s1 = hour_.n;  // (in order as they come)
        } else {
          cappedAdd<HourRow, HourKeys>(hour_, h);
        }
        break;
      }
      case kDays: {
        DaysRow d;
        d.day = cc::get16(p);
        d.plays = cc::get16(p + 2);
        d.skips = cc::get16(p + 4);
        d.djPlays = p[6];
        d.djSkips = p[7];
        d.ms = cc::get32(p + 8);
        d.shufPlays = p[12];
        d.shufSkips = p[13];
        d.pad = 0;
        if (!first && d.day <= lastDay_) return false;
        lastDay_ = d.day;
        if (!days_.rows) {
          noMemory_ = true;
          return false;
        }
        if (days_.n < days_.cap && days_.s0 == 0 && days_.s1 == days_.n) {
          days_.rows[days_.n++] = d;
          days_.s1 = days_.n;
        } else {
          cappedAdd<DaysRow, DaysKeys>(days_, d);
        }
        break;
      }
      case kRcnt: {
        RcntRow r;
        r.day = cc::get16(p);
        r.plays = cc::get16(p + 2);
        r.lo = cc::get32(p + 4);
        r.hi = cc::get32(p + 8);
        const uint64_t hash = RcntKeys::hashOf(r);
        if (!first && (r.day < lastRcntDay_ || (r.day == lastRcntDay_ && hash <= lastRcntHash_))) return false;
        lastRcntDay_ = r.day;
        lastRcntHash_ = hash;
        if (!room(rcnt_, 1)) {
          noMemory_ = true;
          return false;
        }
        const uint32_t at = rcnt_.rows++;
        rcnt_.at(at) = r;
        const uint32_t c = at / decltype(rcnt_)::kPer;
        if (rcnt_.sorted[c] == at % decltype(rcnt_)::kPer) ++rcnt_.sorted[c];
        break;
      }
      case kTrkd: {
        const Track t = decodeTrack(p);
        if (!first && t.hash <= lastHash_) return false;
        lastHash_ = t.hash;
        if (!room(trkd_, 1)) {
          noMemory_ = true;
          return false;
        }
        const uint32_t at = trkd_.rows++;
        trkd_.at(at) = toRow(t);
        const uint32_t c = at / decltype(trkd_)::kPer;
        if (trkd_.sorted[c] == at % decltype(trkd_)::kPer) ++trkd_.sorted[c];
        break;
      }
      default:
        return false;
    }
  }
  return true;
}

void Fold::baseLost() {
  head_.flags |= kFlagBaseLost;
  head_.baseGen = head_.baseRows = head_.baseCrc = 0;
}

// ---------------------------------------------------------------------------
// The boot's fold, the live fold.
// ---------------------------------------------------------------------------
bool Fold::beginBoot(const cc::msmf::Acks& acks) {
  dropBoot();
  if (!mark_ && !begin()) {
    noMemory_ = true;
    return false;
  }
  // Each MARK row's seq ranges, a range a log file (5.6), for as many rows
  // as MARK's block holds (the two grow together).
  rangesCap_ = markCap_;
  ranges_ = static_cast<Range*>(grab(static_cast<size_t>(rangesCap_) * kFiles * sizeof(Range)));
  if (!ranges_) {
    rangesCap_ = 0;
    noMemory_ = true;
    return false;
  }
  std::memset(ranges_, 0, static_cast<size_t>(rangesCap_) * kFiles * sizeof(Range));
  // ACKS's rows above the boot's MARK: the gap's.
  uint32_t above = 0;
  for (uint32_t i = 0; i < acks.count; ++i) {
    const MarkRow* m = findMark(acks.entries[i].deviceId, acks.entries[i].epoch, nullptr);
    if (acks.entries[i].seq > (m ? m->card : 0u)) ++above;
  }
  if (above) {
    acks_ = static_cast<AckTrack*>(grab(static_cast<size_t>(above) * sizeof(AckTrack)));
    if (!acks_) {
      noMemory_ = true;
      dropBoot();
      return false;
    }
    acksCap_ = above;
    for (uint32_t i = 0; i < acks.count; ++i) {
      const cc::msmf::Ack& e = acks.entries[i];
      const MarkRow* m = findMark(e.deviceId, e.epoch, nullptr);
      const uint32_t mark = m ? m->card : 0u;
      if (e.seq <= mark) continue;
      AckTrack& a = acks_[acksN_++];
      std::memset(&a, 0, sizeof(a));
      std::memcpy(a.dev, e.deviceId, 6);
      a.epoch = e.epoch;
      a.seq = e.seq;
      a.mark = mark;
    }
  }
  booting_ = true;
  // The indexes: no memory for one, and the rows are found by each
  // chunk's order instead (slower, the same rows).
  if (!buildIndex<RcntKeys>(rcnt_, rcnt_.rows + 128)) sortChunks<RcntKeys>(rcnt_);
  if (!buildIndex<TrkdKeys>(trkd_, trkd_.rows + 128)) sortChunks<TrkdKeys>(trkd_);
  return true;
}

void Fold::heldSlot(const Slot& s, int file) {
  AckTrack* a = nullptr;
  if (ackHit_ < acksN_ && acks_[ackHit_].epoch == s.epoch && std::memcmp(acks_[ackHit_].dev, s.deviceId, 6) == 0) {
    a = &acks_[ackHit_];
  } else {
    for (uint32_t i = 0; i < acksN_ && !a; ++i)
      if (acks_[i].epoch == s.epoch && std::memcmp(acks_[i].dev, s.deviceId, 6) == 0) {
        a = &acks_[i];
        ackHit_ = i;
      }
  }
  if (!a || s.seq <= a->mark || s.seq > a->seq) return;
  Range* r = a->r;
  if (r[file].hi && s.seq <= r[file].hi) return;
  for (int g = 0; g < kFiles; ++g)
    if (g != file && r[g].hi && r[g].lo <= s.seq && s.seq <= r[g].hi) return;
  if (!r[file].hi) r[file].lo = s.seq;
  r[file].hi = s.seq;
  ++a->held;
}

void Fold::bootSlot(const Slot& s, int file) {
  if (!booting_ || noMemory_ || s.kind != playlog::kKindPlay || file < 0 || file >= kFiles) return;
  // The gap's count of the seqs the log holds: every valid play slot's.
  if (acksN_) heldSlot(s, file);
  if (!counts(s)) return;
  uint32_t at = 0;
  const MarkRow* m = findMark(s.deviceId, s.epoch, &at);
  if (m && s.seq <= m->card) return;  // at or under the MARK read at the boot
  if (!m && !addMark(s.deviceId, s.epoch, &at)) {
    noMemory_ = true;
    return;
  }
  // Each slot once (5.6): in one file a device and epoch's seqs only rise,
  // so one at or under the last this file gave is met again; across files,
  // one inside another file's range is met twice.
  Range* r = ranges_ + static_cast<size_t>(at) * kFiles;
  if (r[file].hi && s.seq <= r[file].hi) return;
  for (int g = 0; g < kFiles; ++g) {
    if (g != file && r[g].hi && r[g].lo <= s.seq && s.seq <= r[g].hi) {
      twice_ = add32(twice_, 1);
      return;
    }
  }
  if (!r[file].hi) r[file].lo = s.seq;
  r[file].hi = s.seq;
  fold(s, round24(loadedRound_ + 1));
}

void Fold::partial() { partial_ = true; }

void Fold::endBoot() {
  if (!booting_) return;
  // The gap: for each ACKS row above the MARK read at the boot, the seqs it
  // covers that no valid play slot of the log holds (a firmware without
  // stats trimmed them), counted, and covered by MARK from now on. A
  // partial scan counts none: the slots it didn't read may hold them.
  if (!partial_ && !noMemory_) {
    for (uint32_t i = 0; i < acksN_; ++i) {
      const AckTrack& a = acks_[i];
      gap_ = add32(gap_, (a.seq - a.mark) - std::min(a.held, a.seq - a.mark));
      MarkRow* m = findMark(a.dev, a.epoch, nullptr);
      if (!m) m = addMark(a.dev, a.epoch, nullptr);
      if (!m) {
        noMemory_ = true;
        break;
      }
      m->seq = std::max(m->seq, a.seq);
      dirty_ = true;
    }
  }
  head_.twice = add32(head_.twice, twice_);
  head_.gap = add32(head_.gap, gap_);
  dropBoot();
  sortTables();
  // RCNT, pruned only by a save that lands, to its 4,096-row cap by whole
  // days (the save's cut without the date's): a boot whose save doesn't
  // land (refused, failed, held) doesn't hold a full log's 13,000 rows for
  // the session. Any later save cuts at least these days (they are under
  // its cap whatever comes after), so it writes the same file.
  uint32_t kept = 0;
  const uint32_t cut = rcntCut(0, &kept);
  if (kept < rcnt_.rows) {
    closeRcnt(cut);
    rcntFloor_ = std::max(rcntFloor_, cut);
  }
}

void Fold::liveSlot(const Slot& s) {
  if (noMemory_ || !counts(s)) return;
  fold(s, round24(loadedRound_ + 2));
}

void Fold::foldTrack(const Slot& s, uint32_t round) {
  Track t;
  if (!slotTrack(s, round, &t)) return;
  const int64_t r = findRow<TrkdKeys>(trkd_, s.pathHash);
  if (r >= 0) {
    TrkdRow& row = trkd_.at(static_cast<uint32_t>(r));
    row = toRow(merge(toTrack(row), t));
  } else {
    append<TrkdKeys>(trkd_, toRow(t));
  }
}

void Fold::foldRcnt(uint16_t day, uint64_t hash) {
  if (day < rcntFloor_) return;  // (a day the boot's cap cut: every later save cuts it too)
  const int64_t r = findRow<RcntKeys>(rcnt_, RcntKeys::Key{day, hash});
  if (r >= 0) {
    RcntRow& row = rcnt_.at(static_cast<uint32_t>(r));
    row.plays = add16(row.plays, 1);
  } else {
    RcntRow row;
    row.lo = static_cast<uint32_t>(hash);
    row.hi = static_cast<uint32_t>(hash >> 32);
    row.day = day;
    row.plays = 1;
    append<RcntKeys>(rcnt_, row);
  }
}

void Fold::fold(const Slot& s, uint32_t round) {
  if (!hour_.rows || !days_.rows) {
    noMemory_ = true;
    return;
  }
  const bool play = isPlay(s), skip = isSkip(s), isDated = dated(s.startedAt);
  const uint8_t src = sourceOf(s);
  Head& h = head_;
  h.slots = add64(h.slots, 1);
  h.listenedMs = add64(h.listenedMs, s.playedMs);
  h.src[src].listenedMs = add64(h.src[src].listenedMs, s.playedMs);
  if (play) {
    h.plays = add64(h.plays, 1);
    h.src[src].plays = add32(h.src[src].plays, 1);
    if (!isDated) h.undated = add32(h.undated, 1);
  }
  if (skip) {
    h.skips = add64(h.skips, 1);
    h.src[src].skips = add32(h.src[src].skips, 1);
  }
  if (isDated) {
    const uint32_t hr = hourOf(s.startedAt);
    if (HourRow* r = cappedFind<HourRow, HourKeys>(hour_, hr)) {
      const uint8_t plays = add8(static_cast<uint8_t>(r->hourPlays >> 24), play);
      r->hourPlays = hr | static_cast<uint32_t>(plays) << 24;
      r->ms = add32(r->ms, s.playedMs);
    } else {
      cappedAdd<HourRow, HourKeys>(hour_, HourRow{hr | static_cast<uint32_t>(play) << 24, s.playedMs});
    }
    const uint16_t day = dayOf(s.startedAt);
    DaysRow* d = cappedFind<DaysRow, DaysKeys>(days_, day);
    DaysRow fresh{};
    if (!d) {
      fresh.day = day;
      d = &fresh;
    }
    d->plays = add16(d->plays, play);
    d->skips = add16(d->skips, skip);
    if (src == kAutoDj) {
      d->djPlays = add8(d->djPlays, play);
      d->djSkips = add8(d->djSkips, skip);
    }
    if (src == kShuffle) {
      d->shufPlays = add8(d->shufPlays, play);
      d->shufSkips = add8(d->shufSkips, skip);
    }
    d->ms = add32(d->ms, s.playedMs);
    if (d == &fresh) cappedAdd<DaysRow, DaysKeys>(days_, fresh);
    if (play) foldRcnt(day, s.pathHash);
    if (play) firstPlayDay_ = std::min(firstPlayDay_, day);
  }
  foldTrack(s, round);
  MarkRow* m = findMark(s.deviceId, s.epoch, nullptr);
  if (!m) m = addMark(s.deviceId, s.epoch, nullptr);
  if (!m) {
    noMemory_ = true;
    return;
  }
  m->seq = std::max(m->seq, s.seq);
  ++folded_;
  used_ = std::max(used_, round);
  dirty_ = true;
}

// ---------------------------------------------------------------------------
// Looking.
// ---------------------------------------------------------------------------
uint32_t Fold::rows(Sec sec) const {
  switch (sec) {
    case kHead: return 1;
    case kMark: return markN_;
    case kHour: return hour_.n;
    case kDays: return days_.n;
    case kRcnt: return rcnt_.rows;
    case kTrkd: return trkd_.rows;
    default: return 0;
  }
}

bool Fold::track(uint64_t hash, Track* out) {
  const int64_t r = findRow<TrkdKeys>(trkd_, hash);
  if (r < 0) return false;
  *out = toTrack(trkd_.at(static_cast<uint32_t>(r)));
  return true;
}

bool Fold::hour(uint32_t hour, uint8_t* plays, uint32_t* listenedMs) {
  const HourRow* r = hour_.rows ? cappedFind<HourRow, HourKeys>(hour_, hour) : nullptr;
  if (!r) return false;
  *plays = static_cast<uint8_t>(r->hourPlays >> 24);
  *listenedMs = r->ms;
  return true;
}

bool Fold::day(uint16_t day, uint8_t out[16]) {
  const DaysRow* r = days_.rows ? cappedFind<DaysRow, DaysKeys>(days_, day) : nullptr;
  if (!r) return false;
  encodeDays(*r, out);
  return true;
}

uint32_t Fold::indexSize(Sec sec) const {
  return sec == kRcnt ? rcnt_.ix.size : sec == kTrkd ? trkd_.ix.size : 0;
}

uint32_t Fold::chunks(Sec sec) const { return sec == kRcnt ? rcnt_.chunks : sec == kTrkd ? trkd_.chunks : 0; }

// ---------------------------------------------------------------------------
// The save.
// ---------------------------------------------------------------------------
void Fold::sortTables() {
  if (booting_) return;  // (the ranges and the indexes stand on row numbers)
  sortMark();
  if (hour_.s0 != 0 || hour_.s1 != hour_.n) cappedSort<HourRow, HourKeys>(hour_);
  if (days_.s0 != 0 || days_.s1 != days_.n) cappedSort<DaysRow, DaysKeys>(days_);
  sortChunks<RcntKeys>(rcnt_);
  sortChunks<TrkdKeys>(trkd_);
}

uint16_t Fold::newestDay() const {
  uint16_t d = 0;
  for (uint32_t i = 0; i < days_.n; ++i) d = std::max(d, days_.rows[i].day);
  return d;
}

uint32_t Fold::rcntCut(uint32_t dateCut, uint32_t* kept) {
  // RCNT sorted by (day, hash) in each chunk: each chunk's first row at or
  // past the date's cut, then whole days, oldest first, past 4,096 rows.
  uint64_t n = 0;
  for (uint32_t c = 0; c < rcnt_.chunks; ++c) {
    const uint32_t in = rcnt_.inChunk(c);
    pos_[c] = static_cast<uint16_t>(firstAtDay(rcnt_.c[c], in, dateCut));
    n += in - pos_[c];
  }
  uint32_t cut = dateCut;
  while (n > kRcntRows) {
    uint32_t d = 0x10000;
    for (uint32_t c = 0; c < rcnt_.chunks; ++c)
      if (pos_[c] < rcnt_.inChunk(c)) d = std::min<uint32_t>(d, rcnt_.c[c][pos_[c]].day);
    if (d == 0x10000) break;
    for (uint32_t c = 0; c < rcnt_.chunks; ++c) {
      const uint32_t in = rcnt_.inChunk(c);
      while (pos_[c] < in && rcnt_.c[c][pos_[c]].day == d) {
        ++pos_[c];
        --n;
      }
    }
    cut = d + 1;
  }
  *kept = static_cast<uint32_t>(n);
  return cut;
}

bool Fold::plan(uint64_t nowMs, bool hasCommit, uint64_t commitSec, Plan* out) {
  endBoot();
  if (partial_ || noMemory_) return false;
  sortTables();
  Plan p;
  p.head = head_;
  p.head.round = std::max(head_.round, used_);
  uint16_t today = 0;
  const bool known = todayOf(nowMs, &today);
  // sinceDay (4.1), set once: the earliest dated play folded, not after
  // today; else today; else the sync's commitTime, flagged; else unset.
  if (p.head.sinceDay == kUnsetDay) {
    uint16_t first = firstPlayDay_;
    if (first != kUnsetDay && known) first = std::min(first, today);
    if (first != kUnsetDay) {
      p.head.sinceDay = first;
    } else if (known) {
      p.head.sinceDay = today;
    } else if (hasCommit && commitSec <= wallclock::kMaxMs / 1000 && dated(commitSec * 1000)) {
      p.head.sinceDay = dayOf(commitSec * 1000);
      p.head.flags |= kFlagSinceCommit;
    }
  }
  // RCNT (5.2): 32 days back from the earlier of today and the newest dated
  // day folded (nothing by date with the clock unset), then whole days past
  // 4,096 rows. HOUR and DAYS are at their caps already: the newest rows.
  uint32_t dateCut = 0;
  if (known && days_.n) {
    const uint32_t ref = std::min<uint32_t>(today, newestDay());
    dateCut = ref >= kRcntDays - 1 ? ref - (kRcntDays - 1) : 0;
  }
  p.rcntCut = rcntCut(dateCut, &p.rows[kRcnt]);
  p.rows[kHead] = 1;
  p.rows[kMark] = markN_;
  p.rows[kHour] = hour_.n;
  p.rows[kDays] = days_.n;
  p.rows[kTrkd] = trkd_.rows;
  p.generation = generation_ == 0xFFFFFFFFu ? generation_ : generation_ + 1;
  *out = p;
  return true;
}

void Fold::rowsBegin(const Plan& p, Sec sec) {
  plan_ = &p;
  sec_ = sec;
  at_ = 0;
  if (sec == kRcnt) {
    for (uint32_t c = 0; c < rcnt_.chunks; ++c)
      pos_[c] = static_cast<uint16_t>(firstAtDay(rcnt_.c[c], rcnt_.inChunk(c), p.rcntCut));
  } else if (sec == kTrkd) {
    for (uint32_t c = 0; c < trkd_.chunks; ++c) pos_[c] = 0;
  }
}

uint32_t Fold::nextRows(uint8_t* out, uint32_t maxRows) {
  if (!plan_) return 0;
  uint32_t k = 0;
  const uint32_t stride = kStrides[sec_];
  switch (sec_) {
    case kMark:
      for (; k < maxRows && at_ < markN_; ++k) encodeMark(mark_[at_++], out + k * stride);
      break;
    case kHour:
      for (; k < maxRows && at_ < hour_.n; ++k) encodeHour(hour_.rows[at_++], out + k * stride);
      break;
    case kDays:
      for (; k < maxRows && at_ < days_.n; ++k) encodeDays(days_.rows[at_++], out + k * stride);
      break;
    case kRcnt:
      for (; k < maxRows; ++k) {
        // The least row at the chunks' heads (each chunk sorted).
        uint32_t best = rcnt_.chunks;
        for (uint32_t c = 0; c < rcnt_.chunks; ++c)
          if (pos_[c] < rcnt_.inChunk(c) &&
              (best == rcnt_.chunks || RcntKeys::less(rcnt_.c[c][pos_[c]], rcnt_.c[best][pos_[best]])))
            best = c;
        if (best == rcnt_.chunks) break;
        encodeRcnt(rcnt_.c[best][pos_[best]++], out + k * stride);
      }
      break;
    case kTrkd: {
      Track t;
      for (; k < maxRows && nextTrack(&t); ++k) encodeTrack(t, out + k * stride);
      break;
    }
    default:
      break;
  }
  return k;
}

bool Fold::nextTrack(Track* out) {
  if (!plan_ || sec_ != kTrkd || plan_->compact) return false;
  uint32_t best = trkd_.chunks;
  for (uint32_t c = 0; c < trkd_.chunks; ++c)
    if (pos_[c] < trkd_.inChunk(c) &&
        (best == trkd_.chunks || trkd_.c[c][pos_[c]].hash < trkd_.c[best][pos_[best]].hash))
      best = c;
  if (best == trkd_.chunks) return false;
  *out = toTrack(trkd_.c[best][pos_[best]++]);
  return true;
}

void Fold::closeRcnt(uint32_t cut) {
  // The rows kept close up (one pass over the table, never a row at a
  // time), each chunk sorted again, the chunks past them given back.
  uint32_t w = 0;
  for (uint32_t r = 0; r < rcnt_.rows; ++r) {
    const RcntRow& x = rcnt_.at(r);
    if (x.day < cut) continue;
    if (w != r) rcnt_.at(w) = x;
    ++w;
  }
  if (w != rcnt_.rows) {
    rcnt_.rows = w;
    rcnt_.unsorted = 0;
    for (uint32_t c = 0; c < rcnt_.chunks; ++c) rcnt_.sorted[c] = 0;
    sortChunks<RcntKeys>(rcnt_);
    shrink(rcnt_);
  }
}

void Fold::saved(const Plan& p) {
  head_ = p.head;
  generation_ = p.generation;
  // RCNT's retention (the card's file is RAM's now: no floor from here).
  closeRcnt(p.rcntCut);
  rcntFloor_ = 0;
  if (p.compact) {
    // Its rows are in the base now.
    trkd_.rows = trkd_.unsorted = 0;
    for (uint32_t c = 0; c < trkd_.chunks; ++c) trkd_.sorted[c] = 0;
    shrink(trkd_);
  }
  for (uint32_t i = 0; i < markN_; ++i) mark_[i].card = mark_[i].seq;
  dirty_ = false;
  plan_ = nullptr;
}

}  // namespace statsfold
