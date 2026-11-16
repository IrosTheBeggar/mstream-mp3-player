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

// firstAt's order: a dated time, then an undated play (0), then none.
uint64_t firstKey(uint32_t t) { return t == 0 ? 1ull << 32 : t == kNoFirst ? (1ull << 32) + 1 : t; }

int cmpPair(const uint8_t* devA, uint64_t epochA, const uint8_t* devB, uint64_t epochB) {
  const int c = std::memcmp(devA, devB, 6);
  if (c != 0) return c;
  return epochA < epochB ? -1 : epochA > epochB ? 1 : 0;
}

int cmp64(uint64_t a, uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }

// A sort with no recursion: a save sorts the records on the loop's stack
// (5.14), and std::sort's introsort recurses up to 2 x log2(n) frames deep
// (112 B each in the core2 build: 2 KB for a chunk's 819 records). A heap
// sort's frames are a few, whatever the input; equal keys' records ⊕ into
// one row in any order (5.2), so the order among them doesn't matter.
template <typename T, typename Less>
void heapSort(T* first, T* last, Less less) {
  std::make_heap(first, last, less);
  std::sort_heap(first, last, less);
}

using Record = Fold::Record;
using MarkRow = Fold::MarkRow;
// The resident set's rows (5.14): a record 20 B (96 in the object: 1,920 B),
// MARK's row 28 B (24 B as the file has it, and the held tmp's seq); 819
// records a chunk, 16 chunks past the 96 for a full log (2 x 6,553 slots);
// the ranges' block 3,072 B with 16 MARK rows and ACKS's 16.
static_assert(sizeof(Record) == kRecordBytes && alignof(Record) == 4, "a record's 20 bytes");
static_assert(sizeof(MarkRow) == 28 && alignof(MarkRow) == 4, "MARK's row");
static_assert(kChunkRecords == 819 && kRecords + kMaxChunks * kChunkRecords >= 2 * playlog::kFileSlots,
              "a full log's slots");
static_assert(sizeof(Fold::Range) * kFiles * kMarkRows + sizeof(Fold::AckTrack) * cc::msmf::Acks::kMax == 3072,
              "the ranges' block");

uint64_t hashOf(const Record& r) { return static_cast<uint64_t>(r.hashHi) << 32 | r.hashLo; }
uint8_t kindOf(const Record& r) { return r.what & 3; }
uint8_t srcOf(const Record& r) { return static_cast<uint8_t>(r.what >> 2 & 3); }
uint32_t hourOfTime(uint32_t t) { return (t - 1) / 3600; }
// RCNT's sort: the day (undated first), then the hash.
uint32_t dayKey(const Record& r) { return r.time ? dayOfTime(r.time) + 1u : 0u; }

void encodeMark(const MarkRow& m, uint8_t* p) {
  std::memcpy(p, m.dev, 6);
  p[6] = p[7] = 0;
  cc::put64(p + 8, m.epoch());
  cc::put32(p + 16, m.seq);
  cc::put32(p + 20, 0);
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
// A section's rows: their order and their ⊕ (a save's merge).
// ---------------------------------------------------------------------------
int compareRows(Sec sec, const uint8_t* a, const uint8_t* b) {
  switch (sec) {
    case kMark: {
      const int c = cmpPair(a, cc::get64(a + 8), b, cc::get64(b + 8));
      return c < 0 ? -1 : c > 0 ? 1 : 0;
    }
    case kHour: return cmp64(cc::get32(a) & kMaxRound, cc::get32(b) & kMaxRound);
    case kDays: return cmp64(cc::get16(a), cc::get16(b));
    case kRcnt: {
      const int c = cmp64(cc::get16(a), cc::get16(b));
      return c ? c : cmp64(cc::get64(a + 4), cc::get64(b + 4));
    }
    case kTrkd: return cmp64(cc::get64(a), cc::get64(b));
    default: return 0;
  }
}

void addRow(Sec sec, uint8_t* a, const uint8_t* b) {
  switch (sec) {
    case kHour: {
      const uint32_t wa = cc::get32(a), wb = cc::get32(b);
      const uint32_t plays = add8(static_cast<uint8_t>(wa >> 24), wb >> 24);
      cc::put32(a, (wa & kMaxRound) | plays << 24);
      cc::put32(a + 4, add32(cc::get32(a + 4), cc::get32(b + 4)));
      break;
    }
    case kDays:
      cc::put16(a + 2, add16(cc::get16(a + 2), cc::get16(b + 2)));
      cc::put16(a + 4, add16(cc::get16(a + 4), cc::get16(b + 4)));
      a[6] = add8(a[6], b[6]);
      a[7] = add8(a[7], b[7]);
      cc::put32(a + 8, add32(cc::get32(a + 8), cc::get32(b + 8)));
      a[12] = add8(a[12], b[12]);
      a[13] = add8(a[13], b[13]);
      break;
    case kRcnt: cc::put16(a + 2, add16(cc::get16(a + 2), cc::get16(b + 2))); break;
    case kTrkd: encodeTrack(merge(decodeTrack(a), decodeTrack(b)), a); break;
    default: break;
  }
}

// ---------------------------------------------------------------------------
// The records' memory.
// ---------------------------------------------------------------------------
Fold::Fold(AllocFn alloc, FreeFn release)
    : alloc_(alloc ? alloc : mallocAlloc), free_(release ? release : mallocFree) {
  std::memset(recs_, 0, sizeof(recs_));
  std::memset(markIn_, 0, sizeof(markIn_));
}

Fold::~Fold() { giveChunks(0); }

bool Fold::carveChunk() {
  if (chunks_ == kMaxChunks) return false;
  void* p = alloc_(kChunkBytes);
  if (!p) return false;
  chunk_[chunks_++] = static_cast<Record*>(p);
  held_ += kChunkBytes;
  peak_ = std::max(peak_, held_);
  return true;
}

void Fold::giveChunks(uint32_t keep) {
  while (chunks_ > keep) {
    --chunks_;
    free_(chunk_[chunks_]);
    chunk_[chunks_] = nullptr;
    held_ -= kChunkBytes;
  }
}

void Fold::dropRecords() {
  n_ = 0;
  giveChunks(0);
  order_ = Order::None;
}

uint32_t Fold::runLength(uint32_t r) const {
  if (r == 0) return std::min(n_, kRecords);
  const uint32_t from = kRecords + (r - 1) * kChunkRecords;
  return n_ > from ? std::min(n_ - from, kChunkRecords) : 0;
}

void Fold::sortRuns(Order o) {
  if (order_ == o) return;
  for (uint32_t r = 0; r <= chunks_; ++r) {
    Record* x = run(r);
    const uint32_t n = runLength(r);
    switch (o) {
      case Order::Time:
        heapSort(x, x + n, [](const Record& a, const Record& b) { return a.time < b.time; });
        break;
      case Order::DayHash:
        heapSort(x, x + n, [](const Record& a, const Record& b) {
          const uint32_t da = dayKey(a), db = dayKey(b);
          return da != db ? da < db : hashOf(a) < hashOf(b);
        });
        break;
      case Order::Hash:
        heapSort(x, x + n, [](const Record& a, const Record& b) { return hashOf(a) < hashOf(b); });
        break;
      default:
        break;
    }
  }
  order_ = o;
}

void Fold::dropBoot() {
  booting_ = false;
  ranges_ = nullptr;
  acks_ = nullptr;
  acksN_ = ackHit_ = 0;
}

void Fold::clear() {
  dropBoot();
  dropRecords();
  head_ = Head();
  generation_ = loadedRound_ = used_ = 0;
  firstPlayDay_ = kUnsetDay;
  dirty_ = partial_ = noMemory_ = behind_ = heldPending_ = false;
  folded_ = twice_ = gap_ = 0;
  markN_ = markSorted_ = markHit_ = 0;
  std::memset(lastMark_, 0, sizeof(lastMark_));
}

// ---------------------------------------------------------------------------
// MARK.
// ---------------------------------------------------------------------------
size_t Fold::markBlockBytes(uint32_t cardRows) { return static_cast<size_t>(cardRows + kMarkRoom) * sizeof(MarkRow); }

bool Fold::markBlock(void* mem, size_t bytes) {
  const size_t cap = bytes / sizeof(MarkRow);
  if (!mem || booting_ || cap < markN_ || reinterpret_cast<uintptr_t>(mem) % alignof(MarkRow) != 0)
    return false;
  MarkRow* m = static_cast<MarkRow*>(mem);
  if (markN_) std::memmove(m, mark_, static_cast<size_t>(markN_) * sizeof(MarkRow));
  mark_ = m;
  markCap_ = static_cast<uint32_t>(cap);
  markHit_ = 0;
  return true;
}

void Fold::dropMarkBlock() {
  clear();
  mark_ = markIn_;
  markCap_ = kMarkRows;
}

Fold::MarkRow* Fold::findMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) const {
  if (markHit_ < markN_ && mark_[markHit_].epoch() == epoch && std::memcmp(mark_[markHit_].dev, dev, 6) == 0) {
    if (at) *at = markHit_;
    return &mark_[markHit_];
  }
  uint32_t lo = 0, hi = markSorted_;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (cmpPair(mark_[mid].dev, mark_[mid].epoch(), dev, epoch) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  uint32_t i = lo;
  if (!(lo < markSorted_ && cmpPair(mark_[lo].dev, mark_[lo].epoch(), dev, epoch) == 0)) {
    for (i = markSorted_; i < markN_; ++i)
      if (mark_[i].epoch() == epoch && std::memcmp(mark_[i].dev, dev, 6) == 0) break;
    if (i == markN_) return nullptr;
  }
  markHit_ = i;
  if (at) *at = i;
  return &mark_[i];
}

Fold::MarkRow* Fold::addMark(const uint8_t dev[6], uint64_t epoch, uint32_t* at) {
  if (markN_ == markCap_) return nullptr;  // (more new pairs than the room: no memory, 5.14)
  MarkRow& m = mark_[markN_];
  std::memcpy(m.dev, dev, 6);
  m.pad = 0;
  m.epochLo = static_cast<uint32_t>(epoch);
  m.epochHi = static_cast<uint32_t>(epoch >> 32);
  m.seq = m.card = m.heldSeq = 0;
  markHit_ = markN_;
  if (at) *at = markN_;
  return &mark_[markN_++];
}

void Fold::sortMark() {
  if (markSorted_ < markN_)
    heapSort(mark_, mark_ + markN_,
              [](const MarkRow& a, const MarkRow& b) { return cmpPair(a.dev, a.epoch(), b.dev, b.epoch()) < 0; });
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

uint32_t Fold::markRows(uint32_t from, uint8_t* out, uint32_t maxRows) const {
  uint32_t k = 0;
  for (; k < maxRows && from + k < markN_; ++k) encodeMark(mark_[from + k], out + k * kStrides[kMark]);
  return k;
}

// ---------------------------------------------------------------------------
// The load.
// ---------------------------------------------------------------------------
void Fold::loadGeneration(uint32_t generation) { generation_ = generation; }

void Fold::loadHead(const uint8_t* bytes) {
  head_ = decodeHead(bytes);
  loadedRound_ = head_.round;
}

bool Fold::loadMark(const uint8_t* rows, uint32_t n, uint32_t stride) {
  if (stride < kStrides[kMark]) return false;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* p = rows + static_cast<size_t>(i) * stride;
    const uint64_t epoch = cc::get64(p + 8);
    if (markN_ && cmpPair(lastMark_, cc::get64(lastMark_ + 6), p, epoch) >= 0) return false;
    std::memcpy(lastMark_, p, 6);
    cc::put64(lastMark_ + 6, epoch);
    MarkRow* m = addMark(p, epoch, nullptr);
    if (!m) {
      noMemory_ = true;
      return false;
    }
    m->seq = m->card = m->heldSeq = cc::get32(p + 16);
    if (markSorted_ + 1 == markN_) markSorted_ = markN_;
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
size_t Fold::bootBytes(const cc::msmf::Acks& acks) const {
  uint32_t above = 0;
  for (uint32_t i = 0; i < acks.count; ++i) {
    const MarkRow* m = findMark(acks.entries[i].deviceId, acks.entries[i].epoch, nullptr);
    if (acks.entries[i].seq > (m ? m->card : 0u)) ++above;
  }
  return static_cast<size_t>(above) * sizeof(AckTrack) + static_cast<size_t>(markCap_) * kFiles * sizeof(Range);
}

bool Fold::beginBoot(const cc::msmf::Acks& acks, void* mem, size_t bytes) {
  dropBoot();
  if (!mem || bytes < bootBytes(acks) || reinterpret_cast<uintptr_t>(mem) % alignof(AckTrack) != 0) {
    noMemory_ = true;
    return false;
  }
  // ACKS's rows above the boot's MARK (the gap's), then each MARK row's seq
  // ranges, a range a log file (5.6), for every row MARK may hold.
  acks_ = static_cast<AckTrack*>(mem);
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
  ranges_ = reinterpret_cast<Range*>(acks_ + acksN_);
  std::memset(ranges_, 0, static_cast<size_t>(markCap_) * kFiles * sizeof(Range));
  booting_ = true;
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
  if (!booting_ || noMemory_ || behind_ || s.kind != playlog::kKindPlay || file < 0 || file >= kFiles) return;
  // The gap's count of the seqs the log holds: every valid play slot's.
  if (acksN_) heldSlot(s, file);
  if (!counts(s)) return;
  uint32_t at = 0;
  MarkRow* m = findMark(s.deviceId, s.epoch, &at);
  if (m && s.seq <= m->card) return;  // at or under the MARK read at the boot
  if (!m && !(m = addMark(s.deviceId, s.epoch, &at))) {
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
  // A record: past the 96, the overflow's room (reserve()'s, or a chunk
  // carved now); no chunk: no memory, nothing more folded.
  if (n_ == capacity() && !carveChunk()) {
    noMemory_ = true;
    return;
  }
  if (!r[file].hi) r[file].lo = s.seq;
  r[file].hi = s.seq;
  fold(s, false);
  m->seq = std::max(m->seq, s.seq);
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
  dropBoot();  // (the caller's block no longer used)
  // The chunks no record needs (reserve()'s room) given back; an overflow
  // no save may take (a partial scan's, a fold out of memory's) dropped:
  // the session is behind (statsref's boot-overflow-partial).
  giveChunks(n_ > kRecords ? (n_ - kRecords + kChunkRecords - 1) / kChunkRecords : 0);
  if ((partial_ || noMemory_) && n_ > kRecords) fallBehind();
}

bool Fold::reserve(uint32_t slots) {
  if (!booting_ || noMemory_ || behind_) return true;
  while (static_cast<uint64_t>(capacity()) < static_cast<uint64_t>(n_) + slots)
    if (!carveChunk()) return false;
  return true;
}

bool Fold::liveSlot(const Slot& s) {
  if (noMemory_ || behind_ || !counts(s)) return false;
  // The records full (and no chunk's room): statsref's 97th.
  if (n_ >= capacity()) {
    fallBehind();
    return false;
  }
  MarkRow* m = findMark(s.deviceId, s.epoch, nullptr);
  if (!m) m = addMark(s.deviceId, s.epoch, nullptr);
  if (!m) {
    noMemory_ = true;
    return false;
  }
  fold(s, true);
  m->seq = std::max(m->seq, s.seq);
  return true;
}

void Fold::fallBehind() {
  behind_ = true;
  dropRecords();
}

uint32_t Fold::roundOf(const Record& r) const { return round24(loadedRound_ + (r.what & kRecLive ? 2 : 1)); }

void Fold::fold(const Slot& s, bool live) {
  const bool play = isPlay(s), skip = isSkip(s), isDated = dated(s.startedAt);
  const uint8_t src = sourceOf(s);
  Record& r = rec(n_++);
  r.hashLo = static_cast<uint32_t>(s.pathHash);
  r.hashHi = static_cast<uint32_t>(s.pathHash >> 32);
  r.time = isDated ? timeOf(s.startedAt) : 0;
  r.playedMs = s.playedMs;
  r.what = static_cast<uint8_t>((play ? kRecPlay : skip ? kRecSkip : 0) | src << 2 | (live ? kRecLive : 0));
  r.reserved[0] = r.reserved[1] = r.reserved[2] = 0;
  order_ = Order::None;
  Head& h = head_;
  h.slots = add64(h.slots, 1);
  h.listenedMs = add64(h.listenedMs, s.playedMs);
  h.src[src].listenedMs = add64(h.src[src].listenedMs, s.playedMs);
  if (play) {
    h.plays = add64(h.plays, 1);
    h.src[src].plays = add32(h.src[src].plays, 1);
    if (!isDated) h.undated = add32(h.undated, 1);
    if (isDated) firstPlayDay_ = std::min(firstPlayDay_, dayOf(s.startedAt));
  }
  if (skip) {
    h.skips = add64(h.skips, 1);
    h.src[src].skips = add32(h.src[src].skips, 1);
  }
  ++folded_;
  used_ = std::max(used_, roundOf(r));
  dirty_ = true;
}

bool Fold::track(uint64_t hash, Track* out) const {
  bool any = false;
  Track t;
  for (uint32_t i = 0; i < n_; ++i) {
    const Record& r = i < kRecords ? recs_[i] : chunk_[(i - kRecords) / kChunkRecords][(i - kRecords) % kChunkRecords];
    const uint8_t k = kindOf(r);
    if (hashOf(r) != hash || (k != kRecPlay && k != kRecSkip)) continue;
    Track one;
    one.hash = hash;
    if (k == kRecPlay) {
      one.plays = 1;
      one.firstAt = one.lastAt = r.time;
      one.lastOrd = roundOf(r);
      one.src = srcOf(r);
    } else {
      one.skips = 1;
    }
    t = any ? merge(t, one) : one;
    any = true;
  }
  if (any) *out = t;
  return any;
}

// ---------------------------------------------------------------------------
// The save.
// ---------------------------------------------------------------------------
bool Fold::plan(uint64_t nowMs, bool hasCommit, uint64_t commitSec, Plan* out) {
  endBoot();
  if (partial_ || noMemory_ || behind_) return false;
  sortMark();
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
  p.rows[kHead] = 1;
  p.rows[kMark] = markN_;
  p.generation = generation_ == 0xFFFFFFFFu ? generation_ : generation_ + 1;
  *out = p;
  return true;
}

void Fold::saved(const Plan& p) {
  head_ = p.head;
  generation_ = p.generation;
  dropRecords();
  for (uint32_t i = 0; i < markN_; ++i) mark_[i].card = mark_[i].heldSeq = mark_[i].seq;
  heldPending_ = false;
  dirty_ = false;
}

void Fold::held(const Plan& p) {
  head_ = p.head;
  generation_ = p.generation;
  dropRecords();
  for (uint32_t i = 0; i < markN_; ++i) mark_[i].heldSeq = mark_[i].seq;
  heldPending_ = true;
  dirty_ = false;
}

void Fold::promoted() {
  if (!heldPending_) return;
  for (uint32_t i = 0; i < markN_; ++i) mark_[i].card = mark_[i].heldSeq;
  heldPending_ = false;
}

// ---------------------------------------------------------------------------
// The records in a section's key order.
// ---------------------------------------------------------------------------
Fold::Cursor::Cursor(Fold& f, Sec sec) : f_(f), sec_(sec) {
  f.sortRuns(sec == kRcnt ? Order::DayHash : sec == kTrkd ? Order::Hash : Order::Time);
  runs_ = 1 + f.chunks_;
}

bool Fold::Cursor::contributes(const Record& r) const {
  switch (sec_) {
    case kHour:
    case kDays: return r.time != 0;
    case kRcnt: return r.time != 0 && kindOf(r) == kRecPlay;
    case kTrkd: return kindOf(r) == kRecPlay || kindOf(r) == kRecSkip;
    default: return false;
  }
}

int Fold::Cursor::keyCmp(const Record& a, const Record& b) const {
  switch (sec_) {
    case kHour: return cmp64(hourOfTime(a.time), hourOfTime(b.time));
    case kDays: return cmp64(dayOfTime(a.time), dayOfTime(b.time));
    case kRcnt: {
      const int c = cmp64(dayOfTime(a.time), dayOfTime(b.time));
      return c ? c : cmp64(hashOf(a), hashOf(b));
    }
    default: return cmp64(hashOf(a), hashOf(b));
  }
}

void Fold::Cursor::rowOf(const Record& r, uint8_t* out) const {
  const bool play = kindOf(r) == kRecPlay, skip = kindOf(r) == kRecSkip;
  switch (sec_) {
    case kHour:
      cc::put32(out, hourOfTime(r.time) | static_cast<uint32_t>(play) << 24);
      cc::put32(out + 4, r.playedMs);
      break;
    case kDays: {
      std::memset(out, 0, 16);
      cc::put16(out, dayOfTime(r.time));
      cc::put16(out + 2, play);
      cc::put16(out + 4, skip);
      if (srcOf(r) == kAutoDj) {
        out[6] = play;
        out[7] = skip;
      }
      cc::put32(out + 8, r.playedMs);
      if (srcOf(r) == kShuffle) {
        out[12] = play;
        out[13] = skip;
      }
      break;
    }
    case kRcnt:
      cc::put16(out, dayOfTime(r.time));
      cc::put16(out + 2, 1);
      cc::put64(out + 4, hashOf(r));
      break;
    default: {
      Track t;
      t.hash = hashOf(r);
      if (play) {
        t.plays = 1;
        t.firstAt = t.lastAt = r.time;  // (0 undated)
        t.lastOrd = f_.roundOf(r);
        t.src = srcOf(r);
      } else {
        t.skips = 1;  // (firstAt none, lastAt and lastOrd 0, you)
      }
      encodeTrack(t, out);
      break;
    }
  }
}

bool Fold::Cursor::next(uint8_t* row) {
  // The least key at the runs' heads (each run sorted), skipping records
  // this section doesn't take; then every record of that key, in any run.
  uint32_t best = runs_;
  for (uint32_t r = 0; r < runs_; ++r) {
    const Record* x = f_.run(r);
    const uint32_t n = f_.runLength(r);
    while (pos_[r] < n && !contributes(x[pos_[r]])) ++pos_[r];
    if (pos_[r] < n && (best == runs_ || keyCmp(x[pos_[r]], f_.run(best)[pos_[best]]) < 0)) best = r;
  }
  if (best == runs_) return false;
  const Record key = f_.run(best)[pos_[best]++];
  rowOf(key, row);
  uint8_t one[kTrackBytes];
  for (uint32_t r = 0; r < runs_; ++r) {
    const Record* x = f_.run(r);
    const uint32_t n = f_.runLength(r);
    for (; pos_[r] < n; ++pos_[r]) {
      const Record& y = x[pos_[r]];
      if (!contributes(y)) continue;
      if (keyCmp(y, key) != 0) break;
      rowOf(y, one);
      addRow(sec_, row, one);
    }
  }
  return true;
}

}  // namespace statsfold
