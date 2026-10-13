// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjJoin.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace autodj {

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

// The path hash (AUTODJ-INDEX.md 2.3) of the track's path as the index
// writes it: "/music/" (any case) and the rest is pathHash()'s, the root's
// literal lowercase; any other path its bytes' FNV-1a 64.
bool defaultPathHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash, void*) {
  const size_t n = index.trackPath(track, path, size);
  if (n == 0) return false;
  static const char kRoot[] = "/music/";
  const size_t k = sizeof(kRoot) - 1;
  bool root = n > k;
  for (size_t i = 0; i < k && root; ++i) {
    const char c = path[i];
    root = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == kRoot[i];
  }
  *hash = root ? cc::pathHash(path + k, n - k) : cc::fnv1a64(path, n);
  return true;
}

// ---- the run sort: IdSort.h's introsort over two parallel arrays ----
struct Pairs {
  uint32_t* k;
  uint16_t* v;
  uint64_t at(uint32_t i) const { return static_cast<uint64_t>(k[i]) << 16 | v[i]; }
  void swap(uint32_t a, uint32_t b) {
    std::swap(k[a], k[b]);
    std::swap(v[a], v[b]);
  }
};

uint32_t median3(const Pairs& p, uint32_t a, uint32_t b, uint32_t c) {
  const uint64_t x = p.at(a), y = p.at(b), z = p.at(c);
  if (x < y) return y < z ? b : (x < z ? c : a);
  return x < z ? a : (y < z ? c : b);
}

void siftDown(Pairs& p, uint32_t base, uint32_t root, uint32_t n) {
  for (;;) {
    uint32_t child = 2 * root + 1;
    if (child >= n) return;
    if (child + 1 < n && p.at(base + child) < p.at(base + child + 1)) ++child;
    if (!(p.at(base + root) < p.at(base + child))) return;
    p.swap(base + root, base + child);
    root = child;
  }
}

void heapSort(Pairs& p, uint32_t first, uint32_t last) {
  const uint32_t n = last - first;
  for (uint32_t i = n / 2; i-- > 0;) siftDown(p, first, i, n);
  for (uint32_t end = n; end > 1;) {
    --end;
    p.swap(first, first + end);
    siftDown(p, first, 0, end);
  }
}

void introRun(Pairs& p, uint32_t first, uint32_t last, int budget) {
  while (last - first > 16) {
    if (budget-- == 0) {
      heapSort(p, first, last);
      return;
    }
    const uint32_t n = last - first;
    const uint32_t mid = first + n / 2;
    uint32_t piv;
    if (n > 128) {
      const uint32_t s = n / 8;
      piv = median3(p, median3(p, first + 1, first + 1 + s, first + 1 + 2 * s), median3(p, mid - s, mid, mid + s),
                    median3(p, last - 1 - 2 * s, last - 1 - s, last - 1));
    } else {
      piv = median3(p, first + 1, mid, last - 1);
    }
    // The pivot at `first`; [first + 1, last) holds a pair at least as big
    // (the triples' larger ones) and `first` stops the scan down, so the
    // scans need no bounds checks (IdSort.h).
    p.swap(first, piv);
    const uint64_t pv = p.at(first);
    uint32_t lo = first + 1, hi = last;
    for (;;) {
      while (p.at(lo) < pv) ++lo;
      --hi;
      while (pv < p.at(hi)) --hi;
      if (!(lo < hi)) break;
      p.swap(lo, hi);
      ++lo;
    }
    if (lo - first < last - lo) {
      introRun(p, first, lo, budget);
      first = lo;
    } else {
      introRun(p, lo, last, budget);
      last = lo;
    }
  }
}

}  // namespace

uint32_t filterWord(uint16_t bpm10, uint8_t camelot, uint8_t flags, uint32_t artistKey) {
  uint32_t bpm = (static_cast<uint32_t>(bpm10) + 5) / 10;
  if (bpm > 255) bpm = 255;
  uint32_t cam = camelot <= 24 ? camelot : 0;
  if (flags & mpdj::kFilterBpmAnalysed) cam |= kCamAnalysed;
  uint32_t art = artistKey & 0xFFFF;
  if (art == 0 && artistKey != 0) art = 1;
  return bpm | cam << 8 | art << 16;
}

void sortPairs(uint32_t* keys, uint16_t* ids, uint32_t n) {
  if (n < 2) return;
  Pairs p{keys, ids};
  int lg = 0;
  while ((static_cast<uint64_t>(1) << (lg + 1)) <= n) ++lg;
  introRun(p, 0, n, 2 * lg);
  // The parts of 16 pairs or fewer left, each in its place: one pass.
  for (uint32_t i = 1; i < n; ++i) {
    const uint32_t k = keys[i];
    const uint16_t v = ids[i];
    const uint64_t x = static_cast<uint64_t>(k) << 16 | v;
    uint32_t j = i;
    while (j > 0 && x < p.at(j - 1)) {
      keys[j] = keys[j - 1];
      ids[j] = ids[j - 1];
      --j;
    }
    keys[j] = k;
    ids[j] = v;
  }
}

// ---------------------------------------------------------------------------
// Join
// ---------------------------------------------------------------------------
Join::Join(AllocFn alloc, FreeFn dealloc)
    : alloc_(alloc ? alloc : defaultAlloc), free_(dealloc ? dealloc : defaultFree), hash_(defaultPathHash) {}

Join::~Join() { freeAll(); }

void* Join::take(size_t bytes) {
  void* p = alloc_(bytes);
  if (!p) return nullptr;
  held_ += bytes;
  if (held_ > stats_.peakBytes) stats_.peakBytes = held_;
  if (bytes > stats_.largestBlock) stats_.largestBlock = bytes;
  return p;
}

void Join::give(void* p, size_t bytes) {
  if (!p) return;
  free_(p);
  held_ -= bytes;
}

size_t Join::freeTransient() {
  const size_t before = held_;
  if (runs_) {
    for (uint32_t r = 0; r < runCount_; ++r) give(runs_[r].keys, static_cast<size_t>(runs_[r].cap) * 6);
    give(runs_, sizeof(Run) * runCount_);
  }
  runs_ = nullptr;
  runCount_ = 0;
  give(buf_, bufBytes_);
  buf_ = nullptr;
  bufBytes_ = 0;
  return before - held_;
}

size_t Join::freeAll() {
  const size_t before = held_;
  freeTransient();
  give(maps_.trackToRow, static_cast<size_t>(maps_.tracks) * sizeof(uint16_t));
  give(maps_.rowToTrack, static_cast<size_t>(maps_.rows) * sizeof(uint16_t));
  give(maps_.filters, static_cast<size_t>(maps_.rows) * sizeof(uint32_t));
  maps_ = Maps();
  return before - held_;
}

void Join::fail(Status s, cc::Why w) {
  freeAll();
  status_ = s;
  why_ = w;
}

size_t Join::release() {
  const size_t n = freeAll();
  status_ = Status::Idle;
  phase_ = Phase::None;
  why_ = cc::Why::Ok;
  table_ = nullptr;
  index_ = nullptr;
  return n;
}

bool Join::matches(const LibraryIndex* index) const {
  return (status_ == Status::Working || status_ == Status::Ready) && index && index == index_ && index->ready() &&
         index->trackCount() == maps_.tracks && index->buildStamp() == stamp_;
}

Join::Status Join::begin(const mpdj::Table& table, const LibraryIndex* index) {
  release();
  stats_ = Stats();
  table_ = &table;
  index_ = index;
  if (!index || !index->ready()) {
    fail(Status::Aborted, cc::Why::Ok);
    return status_;
  }
  if (!table.container().source() || table.rowCount() == 0) {
    fail(Status::Absent, cc::Why::Missing);
    return status_;
  }
  // open() said Ok (the header's contract), so DJRF holds rowCount records,
  // one filter word each. Checked again: a Table whose open() failed after
  // its frame (Why::Counts) would have the filters overrun by a longer DJRF.
  if (table.container().section(mpdj::kSecFilters).count != table.rowCount()) {
    fail(Status::Absent, cc::Why::Counts);
    return status_;
  }
  const uint32_t n = index->trackCount();
  if (n > kMaxTracks) {
    fail(Status::TooBig, cc::Why::Ok);
    return status_;
  }
  const uint32_t rows = table.rowCount();
  stamp_ = index->buildStamp();
  Config c = cfg_;
  if (c.runTracks == 0) c.runTracks = 1;
  if (c.hashPerSlice == 0) c.hashPerSlice = 1;
  c.sliceBytes -= c.sliceBytes % cc::Stream::kSector;
  if (c.sliceBytes < 2 * cc::Stream::kSector) c.sliceBytes = 2 * cc::Stream::kSector;
  cfg_ = c;

  // The kept blocks first (the filters, the largest), then the transient
  // ones, whose holes then come last. A failed one stays nullptr, which
  // freeAll() passes over.
  maps_.tracks = n;
  maps_.rows = rows;
  maps_.filters = static_cast<uint32_t*>(take(static_cast<size_t>(rows) * sizeof(uint32_t)));
  if (maps_.filters) maps_.rowToTrack = static_cast<uint16_t*>(take(static_cast<size_t>(rows) * sizeof(uint16_t)));
  if (maps_.rowToTrack && n) maps_.trackToRow = static_cast<uint16_t*>(take(static_cast<size_t>(n) * sizeof(uint16_t)));
  if (!maps_.rowToTrack || (n && !maps_.trackToRow)) {
    fail(Status::NoMemory, cc::Why::Ok);
    return status_;
  }
  runCount_ = (n + c.runTracks - 1) / c.runTracks;
  if (runCount_) {
    runs_ = static_cast<Run*>(take(sizeof(Run) * runCount_));
    if (!runs_) {
      runCount_ = 0;
      fail(Status::NoMemory, cc::Why::Ok);
      return status_;
    }
    for (uint32_t r = 0; r < runCount_; ++r) runs_[r] = Run();
    for (uint32_t r = 0; r < runCount_; ++r) {
      const uint32_t cap = r + 1 < runCount_ ? c.runTracks : n - r * c.runTracks;
      void* block = take(static_cast<size_t>(cap) * 6);
      if (!block) {
        fail(Status::NoMemory, cc::Why::Ok);
        return status_;
      }
      runs_[r].keys = static_cast<uint32_t*>(block);
      runs_[r].ids = reinterpret_cast<uint16_t*>(runs_[r].keys + cap);
      runs_[r].cap = cap;
    }
  }
  buf_ = static_cast<uint8_t*>(take(c.sliceBytes));
  if (!buf_) {
    fail(Status::NoMemory, cc::Why::Ok);
    return status_;
  }
  bufBytes_ = c.sliceBytes;
  if (n) std::memset(maps_.trackToRow, 0xFF, static_cast<size_t>(n) * sizeof(uint16_t));
  std::memset(maps_.rowToTrack, 0xFF, static_cast<size_t>(rows) * sizeof(uint16_t));
  std::memset(maps_.filters, 0, static_cast<size_t>(rows) * sizeof(uint32_t));
  nextTrack_ = fillRun_ = sortRun_ = 0;
  phase_ = Phase::Hash;
  status_ = Status::Working;
  why_ = cc::Why::Ok;
  return status_;
}

Join::Status Join::step(const LibraryIndex* index) {
  if (!working()) return status_;
  if (!matches(index)) {
    fail(Status::Aborted, cc::Why::Ok);
    return status_;
  }
  ++stats_.slices;
  // A phase with nothing to do hands the slice to the next one.
  for (bool worked = false; !worked && working();) {
    switch (phase_) {
      case Phase::Hash: worked = hashSlice(); break;
      case Phase::Sort: worked = sortSlice(); break;
      case Phase::Strings: worked = stringsSlice(); break;
      case Phase::Filters: worked = filtersSlice(); break;
      case Phase::Paths: worked = pathsSlice(); break;
      default: worked = true; break;
    }
  }
  return status_;
}

bool Join::hashSlice() {
  const uint32_t n = maps_.tracks;
  const uint32_t end = n - nextTrack_ > cfg_.hashPerSlice ? nextTrack_ + cfg_.hashPerSlice : n;
  const bool worked = nextTrack_ < end;
  for (uint32_t t = nextTrack_; t < end; ++t) {
    uint64_t h;
    if (!hash_(*index_, t, path_, sizeof(path_), &h, hashCtx_)) {
      ++stats_.unhashed;
      continue;
    }
    while (runs_[fillRun_].len == runs_[fillRun_].cap) ++fillRun_;  // every track has a place
    Run& r = runs_[fillRun_];
    r.keys[r.len] = static_cast<uint32_t>(h >> 32);
    r.ids[r.len] = static_cast<uint16_t>(t);
    ++r.len;
    ++stats_.hashed;
  }
  nextTrack_ = end;
  if (nextTrack_ == n) phase_ = Phase::Sort;
  return worked;
}

bool Join::sortSlice() {
  while (sortRun_ < runCount_) {
    Run& r = runs_[sortRun_++];
    if (r.len == 0) continue;
    sortPairs(r.keys, r.ids, r.len);
    if (sortRun_ == runCount_) phase_ = Phase::Strings;
    return true;
  }
  phase_ = Phase::Strings;
  return false;
}

bool Join::stringsSlice() {
  // STRS (8.3): byte 0, the header's strings in order, the CRC: the
  // reference reader's own check (its first reason is the join's).
  const cc::Why w = table_->check(0, buf_, bufBytes_);
  if (w != cc::Why::Ok) {
    // check() takes a read that failed in STRS for a bad string: STRS read
    // again tells the card's fault (ReadError: join again after the
    // remount) from the file's (Absent). Only on a failure: a second read.
    const cc::Section& strs = table_->container().section(mpdj::kSecStrs);
    const uint32_t n = strs.bytes < bufBytes_ ? strs.bytes : bufBytes_;
    const bool io = w == cc::Why::Io || !table_->container().source()->read(strs.offset, buf_, n);
    fail(io ? Status::ReadError : Status::Absent, io ? cc::Why::Io : w);
    return true;
  }
  phase_ = Phase::Filters;
  startStream(mpdj::kSecFilters);
  return true;
}

void Join::startStream(uint32_t section) {
  const cc::Section& s = table_->container().section(section);
  stream_.begin(table_->container().source(), s.offset, s.offset + s.bytes, buf_, bufBytes_);
  streamEnd_ = s.offset + s.bytes;
  bufEnd_ = s.offset;  // nothing buffered: the first read refills
  rec_ = 0;
}

uint32_t Join::refillsTo(uint32_t recEnd, uint32_t* bufEnd) const {
  // Stream's rule (CardContainer.h): a spent buffer of two sectors or more
  // refills from the sector boundary at or before the next byte, as many
  // whole sectors as it holds, never past the end.
  uint32_t n = 0;
  while (*bufEnd < recEnd) {
    const uint32_t at = *bufEnd - *bufEnd % cc::Stream::kSector;
    const uint32_t left = streamEnd_ - at;
    *bufEnd = at + (left < bufBytes_ ? left : bufBytes_);
    ++n;
  }
  return n;
}

bool Join::filtersSlice() {
  // DJRF (4.4): each row's first 8 bytes, the rest of its stride passed;
  // the CRC at the end.
  const cc::Section& sec = table_->container().section(mpdj::kSecFilters);
  uint32_t refills = 0;
  for (bool first = true; rec_ < sec.count; first = false) {
    uint32_t be = bufEnd_;
    const uint32_t need = refillsTo(stream_.position() + sec.stride, &be);
    if (!first && refills + need > 1) break;
    uint8_t e[mpdj::kFilterStride];
    if (!stream_.read(e, mpdj::kFilterStride) ||
        !stream_.skipTo(stream_.position() + sec.stride - mpdj::kFilterStride)) {
      fail(Status::ReadError, cc::Why::Io);
      return true;
    }
    bufEnd_ = be;
    refills += need;
    maps_.filters[rec_++] = filterWord(cc::get16(e), e[2], e[3], cc::get32(e + 4));
  }
  if (rec_ < sec.count) return true;
  if (!stream_.drain()) {
    fail(Status::ReadError, cc::Why::Io);
    return true;
  }
  if (stream_.crc() != sec.crc) {
    fail(Status::Absent, cc::Why::SectionCrc);
    return true;
  }
  phase_ = Phase::Paths;
  startStream(mpdj::kSecPaths);
  prevHash_ = 0;
  return true;
}

bool Join::pathsSlice() {
  // DJPH (4.5): each entry's first 10 bytes, checked as Table::check()
  // does (the row below rowCount, the hashes strictly increasing) before
  // it is used; the CRC at the end.
  const cc::Section& sec = table_->container().section(mpdj::kSecPaths);
  uint32_t refills = 0;
  for (bool first = true; rec_ < sec.count; first = false) {
    uint32_t be = bufEnd_;
    const uint32_t need = refillsTo(stream_.position() + sec.stride, &be);
    if (!first && refills + need > 1) break;
    uint8_t e[mpdj::kPathStride];
    if (!stream_.read(e, mpdj::kPathStride) ||
        !stream_.skipTo(stream_.position() + sec.stride - mpdj::kPathStride)) {
      fail(Status::ReadError, cc::Why::Io);
      return true;
    }
    bufEnd_ = be;
    refills += need;
    const uint64_t h = cc::get64(e);
    const uint32_t row = cc::get16(e + 8);
    if (row >= maps_.rows || (rec_ > 0 && h <= prevHash_)) {
      fail(Status::Absent, cc::Why::DjPaths);
      return true;
    }
    prevHash_ = h;
    ++rec_;
    ++stats_.paths;
    merge(h, static_cast<uint16_t>(row));
  }
  if (rec_ < sec.count) return true;
  if (!stream_.drain()) {
    fail(Status::ReadError, cc::Why::Io);
    return true;
  }
  if (stream_.crc() != sec.crc) {
    fail(Status::Absent, cc::Why::SectionCrc);
    return true;
  }
  freeTransient();
  phase_ = Phase::Done;
  status_ = Status::Ready;
  return true;
}

void Join::merge(uint64_t hash, uint16_t row) {
  const uint32_t prefix = static_cast<uint32_t>(hash >> 32);
  uint32_t found = 0;
  uint16_t track = kNoTrack;
  for (uint32_t r = 0; r < runCount_; ++r) {
    Run& run = runs_[r];
    while (run.cur < run.len && run.keys[run.cur] < prefix) ++run.cur;
    // The pairs of this prefix stay under the cursor: the next entry may
    // share it (DJPH's hashes rise, their top 32 bits may not).
    for (uint32_t j = run.cur; j < run.len && run.keys[j] == prefix; ++j) {
      uint64_t full;
      ++stats_.confirms;
      if (!hash_(*index_, run.ids[j], path_, sizeof(path_), &full, hashCtx_) || full != hash) {
        ++stats_.prefixOnly;
        continue;
      }
      if (found++ == 0) track = run.ids[j];
    }
  }
  if (found == 0) return;
  if (found > 1) {
    ++stats_.ambiguous;
    return;
  }
  maps_.trackToRow[track] = row;
  const uint32_t f = maps_.filters[row];
  ++maps_.mapped;
  if (filterBpm(f)) ++maps_.withBpm;
  if (filterCamelot(f)) ++maps_.withKey;
  if (maps_.rowToTrack[row] == kNoTrack) {
    // The first file of the row found: the smallest path hash (11.2).
    maps_.rowToTrack[row] = track;
    maps_.filters[row] = f | static_cast<uint32_t>(kCamPresent) << 8;
    ++stats_.rowsMapped;
  }
}

}  // namespace autodj
