// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjJoin.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace autodj {

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

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

namespace {

// What blocks of `bytes` in all can take of the heap: each one's rounding to
// its size class (at most a thirty-second of it), and 19 B or less of header,
// poisoning and alignment (a join has at most 21 blocks).
size_t carvedAtMost(size_t bytes) { return bytes + bytes / 32 + 1024; }

// The largest request that takes at most `room` bytes of the heap, its header
// aside: one just under a size class's boundary, which it is rounded up to
// whatever the heap adds to a request (12 B of poisoning here), so it takes
// that boundary and no more. Asked any higher, it would take the next one, a
// thirty-second of it more than it asked for.
size_t requestWithin(size_t room) {
  constexpr size_t kUnderBoundary = 64;
  if (room < 4096) return 0;
  int fl = 0;
  for (size_t r = room >> 1; r; r >>= 1) ++fl;
  return (room & ~((static_cast<size_t>(1) << (fl - 5)) - 1)) - kUnderBoundary;
}

}  // namespace

size_t spacerFor(size_t largest, size_t bytes) {
  const size_t keep = carvedAtMost(bytes);
  // (A few KB of spacer wouldn't move anything worth it.)
  return largest > keep + 4096 ? requestWithin(largest - keep) : 0;
}

bool takeSpacers(size_t bytes, size_t keepFree, size_t (*largest)(), size_t (*freeBytes)(), void* (*alloc)(size_t),
                 void (*release)(void*), Spacers* out) {
  bool placed = false;
  while (out->n < Spacers::kMax) {
    const size_t before = largest();
    size_t s = spacerFor(before, bytes);
    if (freeBytes) {
      // Never less than keepFree free while it is held (and a KB for the
      // heap's headers): a smaller one, the blocks then lower in the block
      // and the difference above them.
      const size_t free = freeBytes();
      const size_t most = free > keepFree + 1024 ? free - keepFree - 1024 : 0;
      if (s > most) s = requestWithin(most);
    }
    if (s < 4096) break;  // (spacerFor()'s: a few KB wouldn't move anything worth it)
    void* p = alloc(s);
    if (!p) break;
    const size_t after = largest();
    if (after >= before) {
      // From another block, of a size class TLSF tries first: the largest
      // is as it was. Not placed yet: held, and asked again. Placed: a
      // closer fit isn't worth a hole the blocks may want: given back.
      if (placed) {
        release(p);
        break;
      }
      out->p[out->n++] = p;
      continue;
    }
    // From the largest: its top is what is left of it. Another block is
    // the largest now: that's it (the block itself was up to a thirty-second
    // over what largest() said, so its top may be too); else the top is
    // closed in once more (spacerFor() is 0 when it is as tight as it gets).
    out->p[out->n++] = p;
    placed = true;
    if (after > before - s + before / 32) break;
  }
  return placed;
}

void giveSpacers(Spacers* s, void (*release)(void*)) {
  for (int i = 0; i < s->n; ++i) release(s->p[i]);
  s->n = 0;
}

// ---- The listening stats' hold ----
size_t capRequest(size_t bytes, size_t overhead) {
  // The smallest block a request takes: 16 B with the poisoning (1 + 12,
  // aligned to 4), TLSF's 12 B without.
  const size_t least = std::max<size_t>(12, (1 + overhead + 3) & ~static_cast<size_t>(3));
  if (bytes < least) return 0;
  if (bytes < 128) return bytes - overhead;
  int fl = 0;
  for (size_t r = bytes >> 1; r; r >>= 1) ++fl;  // fls
  return (bytes & ~((static_cast<size_t>(1) << (fl - 5)) - 1)) - overhead;
}

const char* closeName(Close c) {
  switch (c) {
    case Close::Nothing: return "nothing";
    case Close::Closed: return "closed";
    case Close::NoSpacers: return "noSpacers";
    case Close::NotOnTop: return "notOnTop";
    case Close::Margin: return "margin";
    case Close::Missed: return "missed";
    case Close::NoCap: return "noCap";
    case Close::Lost: return "lost";
    case Close::Left: return "left";
    case Close::Full: return "full";
    case Close::NoBlock: return "noBlock";
    case Close::Plain: return "plain";
    case Close::Room: return "room";
    case Close::NoRoom: return "noRoom";
  }
  return "?";
}

WalkAbove::WalkAbove(const void* b, const void* s, size_t h, uint32_t bnd, Above* o)
    : block(reinterpret_cast<uintptr_t>(b) - h),
      spacer(s ? reinterpret_cast<uintptr_t>(s) - h : 0),
      head(h),
      bound(bnd),
      out(o) {
  *out = Above();
}

bool WalkAbove::step(intptr_t heapStart, uintptr_t at, size_t size, bool used) {
  if (done) return false;
  ++out->visits;
  if (!seen && bound && out->visits > bound) {
    out->bounded = true;
    done = true;
    return false;
  }
  if (seen) {
    // The block physically right above the held one (blocks come in
    // address order), if in the same heap.
    done = true;
    if (heapStart == heap) {
      out->free = !used;
      out->bytes = size;
      if (!used) out->capAt = reinterpret_cast<void*>(at + head);
    }
    return false;
  }
  if (at == block && used) {
    seen = true;
    heap = heapStart;
    out->found = true;
    out->blockBytes = size;
    out->onSpacers = spacer && prevFree && prevHeap == heapStart && prevAt <= spacer && spacer < at;
    return true;
  }
  prevAt = at;
  prevFree = !used;
  prevHeap = heapStart;
  return true;
}

namespace {

void walked(const Above& a, CloseReport* rep) {
  ++rep->walks;
  rep->us += a.us;
  rep->usMax = std::max(rep->usMax, a.us);
  rep->visits = std::max(rep->visits, a.visits);
}

}  // namespace

Close closeAbove(const void* block, const void* spacer, const CloseHooks& h, Caps* out, CloseReport* rep) {
  Above a;
  h.above(block, spacer, &a);
  walked(a, rep);
  if (!a.found) {
    rep->bounded = a.bounded;
    return rep->why = Close::Lost;
  }
  rep->blockBytes = a.blockBytes;
  if (spacer && !a.onSpacers) return rep->why = Close::NotOnTop;
  if (!a.free) {
    rep->usedAbove = a.bytes;
    return rep->why = Close::Nothing;
  }
  rep->aboveBytes = a.bytes;
  // (Rule 3: the caps never leave under the margin free.)
  if (h.freeBytes && h.freeBytes() < a.bytes + 4 + h.keepFree + 1024) return rep->why = Close::Margin;
  for (;;) {
    const size_t req = capRequest(a.bytes, h.overhead);
    if (!req) return rep->why = out->n ? Close::Closed : Close::Nothing;  // (12 B: no request can take it)
    if (out->n == Caps::kMax) return rep->why = Close::Left;
    // (A cap after the first: the block above is what the caps left, T less
    // their footprints, and the margin holds as it did at the first, unless
    // another task freed the block above it meanwhile: merged in, it heads
    // its list at the walk's capAt, so the next cap would land there and
    // take the freed block's room with no margin asked. Left open.)
    if (out->n && (a.bytes + rep->capBytes > rep->aboveBytes ||
                   (h.freeBytes && h.freeBytes() < a.bytes + 4 + h.keepFree + 1024)))
      return rep->why = Close::Left;
    void* c = h.alloc(req);
    if (c != a.capAt) {
      // Another block heads T's list, or another task carved first: the
      // same request would find the same head, so no retry.
      rep->want = a.capAt;
      rep->got = c;
      if (c) h.release(c);
      return rep->why = c ? Close::Missed : Close::NoCap;
    }
    out->p[out->n++] = c;
    rep->caps = out->n;
    h.above(c, nullptr, &a);
    walked(a, rep);
    rep->capBytes += a.blockBytes + 4;
    if (!a.found) return rep->why = Close::Lost;  // (never: just carved)
    // In use above the cap: the caps took T whole (their footprints T's +
    // 4: a cap that leaves a rest of 16 B or more splits it, else takes it
    // all), or another task's block took the rest's head between the cap
    // and this walk, the rest above it open: Left, not Closed.
    if (!a.free) return rep->why = rep->capBytes >= rep->aboveBytes + 4 ? Close::Closed : Close::Left;
  }
}

void* TopHold::take(const Hooks& h, size_t bytes, CloseReport* rep, bool close, size_t room, bool yield) {
  *rep = CloseReport();
  // No free block takes it: said without asking (largest() is the largest
  // request TLSF's good fit serves, heap_caps_get_largest_free_block(): its
  // largest free block's class floor less the poisoning's 12 B), a failed
  // heap_caps_malloc being the firmware's "[heap] FAILED" line and count.
  if (h.largest && h.largest() < bytes) {
    rep->why = Close::NoBlock;
    return nullptr;
  }
  // The spacers keep keepFree free with the block itself carved (its carve
  // counted, as the join counts its own): another task's carve in this
  // window (a track start's probe, an Opus packet buffer, the card worker's)
  // finds the margin, whatever the block's size. Not closing (the
  // overflow's chunks one by one): keepFree alone, as before the caps.
  Spacers sp;
  const size_t keep = close ? h.keepFree + carvedAtMost(bytes) : h.keepFree;
  const bool placed = h.largest && takeSpacers(bytes, keep, h.largest, h.freeBytes, h.alloc, h.release, &sp);
  void* p = h.alloc(bytes);
  const void* spacer = placed ? sp.p[sp.n - 1] : nullptr;  // (the last: the largest's, right under the block)
  giveSpacers(&sp, h.release);  // (the caps come after: free is back near what it was)
  if (!p) {
    rep->why = Close::NoBlock;
    return nullptr;
  }
  if (n_ == kMax) {
    rep->why = Close::Full;
    return p;
  }
  Entry& e = e_[n_++];
  e = Entry();
  e.block = p;
  if (!placed) {
    rep->why = Close::NoSpacers;
  } else if (!close) {
    rep->why = Close::Plain;
  } else if (!h.above) {
    rep->why = Close::Nothing;
  } else {
    CloseHooks c;
    c.above = h.above;
    c.alloc = h.alloc;
    c.release = h.release;
    c.freeBytes = h.freeBytes;
    c.keepFree = h.keepFree;
    c.overhead = h.overhead;
    closeAbove(p, spacer, c, &e.caps, rep);
  }
  // A decoder's room (an open in the hold: a playing track's next, or a
  // start nothing holds; GAPLESS 2.1's one decoder at a time, the old
  // one's blocks given back first, but an MP3 gives back none and the
  // session's first Opus carves 125 KB, kept): the caps keep the margin
  // free, not those blocks, so while one may open the largest free block
  // must still take them with every cap held. Short of that the hold's
  // caps are given back, the newest block's first, as the overflow's
  // fallback gives them back; still short, a block that can yield is given
  // back too (the arena, before its first chunk is in use).
  if (room && h.largest && h.largest() < carvedAtMost(room)) {
    rep->uncapped = static_cast<uint8_t>(uncap(h));
    if (yield && h.largest() < carvedAtMost(room)) {
      give(h, p);  // (the newest entry: freed now, block first, its caps gone already)
      rep->why = Close::NoRoom;
      return nullptr;
    }
    if (rep->uncapped) rep->why = Close::Room;  // (none held: what the take did stands)
  }
  return p;
}

void TopHold::give(const Hooks& h, void* p) {
  if (!p) return;
  int i = n_ - 1;
  while (i >= 0 && e_[i].block != p) --i;
  if (i < 0) {
    h.release(p);
    return;
  }
  e_[i].given = true;
  while (n_ > 0 && e_[n_ - 1].given) {  // the newest first: the lowest
    Entry& e = e_[--n_];
    h.release(e.block);  // the block first: it merges with the free block under it
    for (int k = 0; k < e.caps.n; ++k) h.release(e.caps.p[k]);  // then its caps, bottom-up
    e = Entry();
  }
}

int TopHold::uncap(const Hooks& h) {
  int n = 0;
  for (int i = n_ - 1; i >= 0; --i) {
    Caps& c = e_[i].caps;
    for (int k = 0; k < c.n; ++k) h.release(c.p[k]);
    n += c.n;
    c = Caps();
  }
  return n;
}

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
    : alloc_(alloc ? alloc : defaultAlloc), free_(dealloc ? dealloc : defaultFree) {}

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
  give(walk_.block(), folderBytes());
  walk_.end();  // (the folders' states and their block forgotten)
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

  // The blocks (the class comment's placement); without it, the kept ones
  // first (the filters, the largest) and the transient ones above, whose
  // hole then joins the free tail. A failed one stays nullptr, which
  // freeAll() passes over.
  maps_.tracks = n;
  maps_.rows = rows;
  runCount_ = (n + c.runTracks - 1) / c.runTracks;
  // The folders' states: with the default hash, for a library of up to
  // kMaxFolders folders.
  walk_.wantFolders(walk_.hashIsDefault() ? pathhash::Walk::foldersFor(*index) : 0);
  const bool ok = place_.largest && place_.freeBytes ? takePlaced(n, rows) : takeKept(n, rows) && takeTransient(n);
  if (!ok) {
    fail(Status::NoMemory, cc::Why::Ok);
    return status_;
  }
  if (n) std::memset(maps_.trackToRow, 0xFF, static_cast<size_t>(n) * sizeof(uint16_t));
  std::memset(maps_.rowToTrack, 0xFF, static_cast<size_t>(rows) * sizeof(uint16_t));
  std::memset(maps_.filters, 0, static_cast<size_t>(rows) * sizeof(uint32_t));
  fillRun_ = sortRun_ = 0;
  walk_.begin(index);
  phase_ = walk_.folders() ? Phase::Folders : Phase::Hash;
  status_ = Status::Working;
  why_ = cc::Why::Ok;
  return status_;
}

bool Join::takeKept(uint32_t n, uint32_t rows) {
  maps_.filters = static_cast<uint32_t*>(take(static_cast<size_t>(rows) * sizeof(uint32_t)));
  if (maps_.filters) maps_.rowToTrack = static_cast<uint16_t*>(take(static_cast<size_t>(rows) * sizeof(uint16_t)));
  if (maps_.rowToTrack && n) maps_.trackToRow = static_cast<uint16_t*>(take(static_cast<size_t>(n) * sizeof(uint16_t)));
  return maps_.rowToTrack && (n == 0 || maps_.trackToRow);
}

bool Join::takeTransient(uint32_t n) {
  if (runCount_) {
    runs_ = static_cast<Run*>(take(sizeof(Run) * runCount_));
    if (!runs_) {
      runCount_ = 0;
      return false;
    }
    for (uint32_t r = 0; r < runCount_; ++r) runs_[r] = Run();
    for (uint32_t r = 0; r < runCount_; ++r) {
      const uint32_t cap = r + 1 < runCount_ ? cfg_.runTracks : n - r * cfg_.runTracks;
      void* block = take(static_cast<size_t>(cap) * 6);
      if (!block) return false;
      runs_[r].keys = static_cast<uint32_t*>(block);
      runs_[r].ids = reinterpret_cast<uint16_t*>(runs_[r].keys + cap);
      runs_[r].cap = cap;
    }
  }
  buf_ = static_cast<uint8_t*>(take(cfg_.sliceBytes));
  if (!buf_) return false;
  bufBytes_ = cfg_.sliceBytes;
  return !walk_.folders() || takeFolders(take(folderBytes()));
}

bool Join::takeFolders(void* block) {
  if (!walk_.folders()) return true;
  if (!block) {
    walk_.dropFolders();
    return false;
  }
  walk_.giveBlock(block);
  return true;
}

void Join::noteFree() {
  const size_t f = place_.freeBytes();
  if (f < stats_.lowestFree) stats_.lowestFree = f;
}

void* Join::probe(size_t bytes, size_t largest, size_t* minTop, void** low, bool* failed) {
  if (bytes >= *minTop) return nullptr;
  void* p = take(bytes);
  if (!p) {
    *failed = true;
    return nullptr;
  }
  // (The largest's low end, once known, is the sure sign: place_.largest() only
  // says its size class, which a block under a class of it may leave as it was.)
  if (p != *low && place_.largest() >= largest) {
    noteFree();
    return p;
  }
  give(p, bytes);  // (from the largest: it joins it again, as it was)
  *low = p;
  *minTop = bytes;
  return nullptr;
}

bool Join::takePlaced(uint32_t n, uint32_t rows) {
  stats_.lowestFree = place_.freeBytes();
  // The runs' table (tens of bytes): where the heap puts it.
  if (runCount_) {
    runs_ = static_cast<Run*>(take(sizeof(Run) * runCount_));
    if (!runs_) {
      runCount_ = 0;
      return false;
    }
    for (uint32_t r = 0; r < runCount_; ++r) runs_[r] = Run();
  }
  // Each kept block, then each transient one (the runs, the folders'
  // states, the buffer), where the heap puts it unless that is the largest
  // free block.
  const size_t largest = place_.largest();
  size_t minTop = SIZE_MAX;
  void* low = nullptr;
  bool failed = false;
  void** const kept[3] = {reinterpret_cast<void**>(&maps_.filters), reinterpret_cast<void**>(&maps_.rowToTrack),
                          reinterpret_cast<void**>(&maps_.trackToRow)};
  const size_t keptBytes[3] = {static_cast<size_t>(rows) * sizeof(uint32_t),
                               static_cast<size_t>(rows) * sizeof(uint16_t), static_cast<size_t>(n) * sizeof(uint16_t)};
  bool onTop[3] = {};
  size_t topKept = 0, topTransient = 0;
  for (int i = 0; i < 3; ++i) {
    if (keptBytes[i] == 0) continue;  // (trackToRow, with no tracks)
    *kept[i] = probe(keptBytes[i], largest, &minTop, &low, &failed);
    if (failed) return false;
    if (*kept[i]) {
      ++stats_.keptElsewhere;
    } else {
      onTop[i] = true;
      topKept += keptBytes[i];
    }
  }
  for (uint32_t r = 0; r < runCount_; ++r) {
    runs_[r].cap = r + 1 < runCount_ ? cfg_.runTracks : n - r * cfg_.runTracks;
    void* block = probe(static_cast<size_t>(runs_[r].cap) * 6, largest, &minTop, &low, &failed);
    if (failed) return false;
    if (block) {
      runs_[r].keys = static_cast<uint32_t*>(block);
      runs_[r].ids = reinterpret_cast<uint16_t*>(runs_[r].keys + runs_[r].cap);
      ++stats_.runsElsewhere;
    } else {
      topTransient += static_cast<size_t>(runs_[r].cap) * 6;
    }
  }
  if (walk_.folders()) {
    void* block = probe(folderBytes(), largest, &minTop, &low, &failed);
    if (failed) {
      walk_.dropFolders();
      return false;
    }
    if (block) {
      takeFolders(block);
      ++stats_.runsElsewhere;
    } else {
      topTransient += folderBytes();
    }
  }
  buf_ = static_cast<uint8_t*>(probe(cfg_.sliceBytes, largest, &minTop, &low, &failed));
  if (failed) return false;
  if (buf_) {
    bufBytes_ = cfg_.sliceBytes;
    ++stats_.runsElsewhere;
  } else {
    topTransient += cfg_.sliceBytes;
  }
  // The rest at the largest free block's top: the transient ones under the
  // spacers, then the kept ones in the room left above them (their closest
  // fit, none smaller holding them). No transient one there: the first kept
  // one goes under the spacers. Free never under the margin meanwhile.
  stats_.topBytes = topKept + topTransient;
  stats_.topKept = topKept;
  if (stats_.topBytes == 0) return true;
  int under = -1;
  for (int i = 0; i < 3 && topTransient == 0; ++i) {
    if (onTop[i]) {
      under = i;
      break;
    }
  }
  const size_t underBytes = topTransient + (under >= 0 ? keptBytes[under] : 0);
  Spacers sp;
  stats_.placed = takeSpacers(stats_.topBytes, carvedAtMost(underBytes) + place_.margin, place_.largest,
                              place_.freeBytes, alloc_, free_, &sp);
  if (!stats_.placed) giveSpacers(&sp, free_);  // (none placed: where the heap puts them, the kept ones first)
  noteFree();
  const uint32_t folders = walk_.folders();
  bool ok = true;
  auto takeKeptOnTop = [&](int i) {
    *kept[i] = take(keptBytes[i]);
    ok = ok && *kept[i];
  };
  auto takeTransientOnTop = [&]() {
    for (uint32_t r = 0; r < runCount_ && ok; ++r) {
      if (runs_[r].keys) continue;
      runs_[r].keys = static_cast<uint32_t*>(take(static_cast<size_t>(runs_[r].cap) * 6));
      ok = runs_[r].keys != nullptr;
      if (ok) runs_[r].ids = reinterpret_cast<uint16_t*>(runs_[r].keys + runs_[r].cap);
    }
    if (ok && walk_.folders() && !walk_.block()) ok = takeFolders(take(folderBytes()));
    if (ok && !buf_) {
      buf_ = static_cast<uint8_t*>(take(cfg_.sliceBytes));
      ok = buf_ != nullptr;
      if (ok) bufBytes_ = cfg_.sliceBytes;
    }
  };
  if (stats_.placed) {
    if (under >= 0) takeKeptOnTop(under);
    if (ok) takeTransientOnTop();
    noteFree();
    giveSpacers(&sp, free_);
    if (!ok) {
      // The room above the spacers fell short (another task's block took
      // some of it meanwhile): the rest where the heap puts them, with the
      // spacers gone. A failed block isn't a heap with no room for it.
      ok = true;
      if (walk_.folders() != folders) walk_.wantFolders(folders);  // (none taken: takeFolders() let them go)
      stats_.placed = false;
      stats_.topShort = true;
    }
  }
  for (int i = 0; i < 3 && ok; ++i) {
    if (onTop[i] && !*kept[i]) takeKeptOnTop(i);
  }
  if (ok) takeTransientOnTop();  // (placed: nothing left to take)
  noteFree();
  return ok;
}

Join::Status Join::stepPass(const LibraryIndex* index, bool (*more)(void* ctx), void* ctx) {
  const uint32_t reads = stats_.reads;
  do {
    step(index);
  } while (working() && stats_.reads == reads && (!more || more(ctx)));
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
      case Phase::Folders: worked = foldersSlice(); break;
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

bool Join::foldersSlice() {
  walk_.foldersSlice(cfg_.foldersPerSlice);
  if (walk_.foldersDone()) {
    stats_.folders = walk_.folders();
    phase_ = Phase::Hash;
  }
  return true;
}

void Join::fillRun(void* ctx, uint32_t t, uint64_t h) {
  Join& j = *static_cast<Join*>(ctx);
  while (j.runs_[j.fillRun_].len == j.runs_[j.fillRun_].cap) ++j.fillRun_;  // every track has a place
  Run& r = j.runs_[j.fillRun_];
  r.keys[r.len] = static_cast<uint32_t>(h >> 32);
  r.ids[r.len] = static_cast<uint16_t>(t);
  ++r.len;
  ++j.stats_.hashed;
}

bool Join::hashSlice() {
  const bool worked = walk_.hashSlice(cfg_.hashPerSlice, fillRun, this);
  stats_.unhashed = walk_.position() - stats_.hashed;
  if (walk_.tracksDone()) phase_ = Phase::Sort;
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
  ++stats_.reads;
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
    stats_.reads += need;
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
  const uint32_t confirms = stats_.confirms;
  for (bool first = true; rec_ < sec.count; first = false) {
    // (The slice's confirms: the next slice goes on in the buffer.)
    if (!first && cfg_.confirmsPerSlice && stats_.confirms - confirms >= cfg_.confirmsPerSlice) break;
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
    stats_.reads += need;
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
      if (!walk_.trackHash(run.ids[j], &full) || full != hash) {
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
    ++maps_.rowsMapped;
  }
}

}  // namespace autodj
