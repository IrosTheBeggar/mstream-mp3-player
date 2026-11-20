// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsRank.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "CardContract.h"

namespace statsrank {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sx = statsfile;

const char* listName(List l) {
  switch (l) {
    case kMost7: return "most7";
    case kMost30: return "most30";
    case kMostAll: return "mostAll";
    case kNewFinds: return "newFinds";
    case kOldFavourites: return "oldFavourites";
    case kRecent: return "recent";
    default: return "?";
  }
}

const char* outcomeName(Outcome o) {
  switch (o) {
    case Outcome::Ok: return "ok";
    case Outcome::Changed: return "changed";
    case Outcome::Base: return "base";
    case Outcome::Io: return "io";
    case Outcome::Damaged: return "damaged";
    case Outcome::Room: return "room";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The order (4.3)
// ---------------------------------------------------------------------------
bool before(List l, const Entry& a, const Entry& b) {
  if (l == kRecent) {
    // lastAt, newest first; then the rows with no dated play (lastAt 0) by
    // lastOrd, newest first; then the hash.
    const bool da = a.at != 0, db = b.at != 0;
    if (da != db) return da;
    if (da) {
      if (a.at != b.at) return a.at > b.at;
    } else if (a.ord != b.ord) {
      return a.ord > b.ord;
    }
    return a.hash < b.hash;
  }
  // The others: the count, most first; then `at`, newest first; then the
  // hash.
  if (a.count != b.count) return a.count > b.count;
  if (a.at != b.at) return a.at > b.at;
  return a.hash < b.hash;
}

namespace {

constexpr uint32_t kOffsets[kLists + 1] = {0,
                                           rowsOf(kMost7),
                                           rowsOf(kMost7) + rowsOf(kMost30),
                                           rowsOf(kMost7) + rowsOf(kMost30) + rowsOf(kMostAll),
                                           rowsOf(kMost7) + rowsOf(kMost30) + rowsOf(kMostAll) + rowsOf(kNewFinds),
                                           rowsOf(kMost7) + rowsOf(kMost30) + rowsOf(kMostAll) + rowsOf(kNewFinds) +
                                               rowsOf(kOldFavourites),
                                           rowsOf(kMost7) + rowsOf(kMost30) + rowsOf(kMostAll) + rowsOf(kNewFinds) +
                                               rowsOf(kOldFavourites) + rowsOf(kRecent)};
static_assert(kOffsets[kLists] == Ranker::kEntries, "the lists' block");

// A bounded heap of the list's best rows: the worst at the top (index 0),
// so a better row replaces it.
struct Heap {
  List l;
  Entry* e;
  // `a` is worse than `b`: it goes nearer the top.
  bool worse(const Entry& a, const Entry& b) const { return before(l, b, a); }
  void up(uint32_t i) {
    while (i > 0) {
      const uint32_t p = (i - 1) / 2;
      if (!worse(e[i], e[p])) return;
      std::swap(e[i], e[p]);
      i = p;
    }
  }
  void down(uint32_t i, uint32_t len) {
    for (;;) {
      uint32_t c = 2 * i + 1;
      if (c >= len) return;
      if (c + 1 < len && worse(e[c + 1], e[c])) ++c;
      if (!worse(e[c], e[i])) return;
      std::swap(e[i], e[c]);
      i = c;
    }
  }
};

}  // namespace

void Ranker::begin(void* mem, bool hasToday, uint16_t today) {
  Entry* base = static_cast<Entry*>(mem);
  for (int l = 0; l < kLists; ++l) {
    list_[l] = base + kOffsets[l];
    n_[l] = 0;
  }
  hasToday_ = hasToday;
  today_ = hasToday ? today : 0;
  first7_ = !hasToday ? 0 : today >= 6 ? static_cast<uint16_t>(today - 6) : 0;
  first30_ = !hasToday ? 0 : today >= 29 ? static_cast<uint16_t>(today - 29) : 0;
}

void Ranker::offer(List l, const Entry& x) {
  Heap h{l, list_[l]};
  const uint32_t cap = rowsOf(l);
  if (n_[l] < cap) {
    h.e[n_[l]] = x;
    h.up(n_[l]++);
    return;
  }
  if (!before(l, x, h.e[0])) return;  // no better than the worst kept
  h.e[0] = x;
  h.down(0, cap);
}

void Ranker::track(const sf::Track& t) {
  if (t.plays >= 1) {
    Entry e;
    e.hash = t.hash;
    e.count = t.plays;
    e.at = t.lastAt;
    offer(kMostAll, e);
    e.ord = t.lastOrd;
    offer(kRecent, e);
  }
  if (!hasToday_) return;
  // (A dated time: neither 0 nor kNoFirst.)
  if (t.firstAt != 0 && t.firstAt != sf::kNoFirst) {
    const uint16_t d = sf::dayOfTime(t.firstAt);
    if (d >= first30_ && d <= today_) {
      Entry e;
      e.hash = t.hash;
      e.count = t.plays;
      e.at = t.firstAt;
      e.src = t.src;
      offer(kNewFinds, e);
    }
  }
  if (t.plays >= kOldFavourite && t.lastAt != 0 && sf::dayOfTime(t.lastAt) < first30_) {
    Entry e;
    e.hash = t.hash;
    e.count = t.plays;
    e.at = t.lastAt;
    offer(kOldFavourites, e);
  }
}

void Ranker::windows(uint64_t hash, uint32_t plays7, uint16_t latest7, uint32_t plays30, uint16_t latest30) {
  if (!hasToday_) return;
  Entry e;
  e.hash = hash;
  if (plays7 >= 1) {
    e.count = plays7;
    e.at = latest7;
    offer(kMost7, e);
  }
  if (plays30 >= 1) {
    e.count = plays30;
    e.at = latest30;
    offer(kMost30, e);
  }
}

void Ranker::finish() {
  for (int l = 0; l < kLists; ++l) {
    // The heap sort: the worst to the end, one at a time.
    Heap h{static_cast<List>(l), list_[l]};
    for (uint32_t end = n_[l]; end > 1;) {
      --end;
      std::swap(h.e[0], h.e[end]);
      h.down(0, end);
    }
  }
}

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
void Windows::begin(void* mem, uint32_t cap, bool hasToday, uint16_t today) {
  rows_ = static_cast<Row*>(mem);
  cap_ = mem ? cap : 0;
  n_ = 0;
  hasToday_ = hasToday;
  today_ = today;
}

bool Windows::add(uint16_t day, uint64_t hash, uint32_t plays) {
  if (!hasToday_ || day > today_ || (today_ >= 29 && day < today_ - 29) || plays == 0) return true;
  if (n_ == cap_) return false;
  Row& r = rows_[n_++];
  r.lo = static_cast<uint32_t>(hash);
  r.hi = static_cast<uint32_t>(hash >> 32);
  r.day = day;
  r.plays = static_cast<uint16_t>(plays > 0xFFFF ? 0xFFFF : plays);
  return true;
}

void Windows::finish(Ranker& r) {
  static_assert(sizeof(Row) == kRowBytes, "a window's row");
  std::sort(rows_, rows_ + n_, [](const Row& a, const Row& b) {
    return a.hi != b.hi ? a.hi < b.hi : a.lo != b.lo ? a.lo < b.lo : a.day < b.day;
  });
  const uint16_t first7 = today_ >= 6 ? static_cast<uint16_t>(today_ - 6) : 0;
  for (uint32_t i = 0; i < n_;) {
    const uint32_t lo = rows_[i].lo, hi = rows_[i].hi;
    uint32_t p7 = 0, p30 = 0;
    uint16_t l7 = 0, l30 = 0;
    for (; i < n_ && rows_[i].lo == lo && rows_[i].hi == hi; ++i) {
      const Row& w = rows_[i];
      p30 += w.plays;
      l30 = std::max(l30, w.day);
      if (w.day >= first7) {
        p7 += w.plays;
        l7 = std::max(l7, w.day);
      }
    }
    r.windows(static_cast<uint64_t>(hi) << 32 | lo, p7, l7, p30, l30);
  }
}

// ---------------------------------------------------------------------------
// The played set and the sparse index
// ---------------------------------------------------------------------------
bool Played::contains(uint64_t hash) const {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint64_t h = at(mid);
    if (h == hash) return true;
    if (h < hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

uint64_t Sparse::hashAt(uint32_t i) const { return cc::get64(rows + static_cast<size_t>(i) * kSparseRowBytes); }
uint32_t Sparse::rowAt(uint32_t i) const { return cc::get32(rows + static_cast<size_t>(i) * kSparseRowBytes + 8); }

void Sparse::span(uint64_t hash, uint32_t baseRows, uint32_t* from, uint32_t* to) const {
  // The last index row at or below the hash, to the next one.
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (hashAt(mid) <= hash) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) {
    *from = *to = 0;  // below the base's first row: none holds it
    return;
  }
  *from = rowAt(lo - 1);
  *to = lo < n ? rowAt(lo) : baseRows;
}

// ---------------------------------------------------------------------------
// The rank's read
// ---------------------------------------------------------------------------
namespace {

Outcome ofWhy(cc::Why w) {
  if (w == cc::Why::Ok) return Outcome::Ok;
  return w == cc::Why::Io ? Outcome::Io : Outcome::Damaged;
}

// The records' rows, one array (`stride` bytes each), as a stream.
struct Rows {
  const uint8_t* p = nullptr;
  uint32_t n = 0, i = 0, stride = 0;
  bool next(uint8_t* out) {
    if (i == n) return false;
    std::memcpy(out, p + static_cast<size_t>(i++) * stride, stride);
    return true;
  }
};

}  // namespace

Outcome open(tagstore::Fs& fs, const Input& in, Files* out) {
  *out = Files();
  bool cardFailed = false;
  out->bin = fs.openRead(sx::kStatsBin, &cardFailed);
  if (!out->bin && cardFailed) return Outcome::Io;
  // stats.bin: the copy the session stands on, or none on either side.
  if (!out->bin != !in.present) {
    close(fs, out);
    return Outcome::Changed;
  }
  if (in.baseGen != 0) {
    char name[sx::kNameCap];
    sx::baseName(in.baseGen, name, sizeof(name));
    cardFailed = false;
    out->base = fs.openRead(name, &cardFailed);
    if (!out->base) {
      close(fs, out);
      return cardFailed ? Outcome::Io : Outcome::Base;
    }
  }
  return Outcome::Ok;
}

void close(tagstore::Fs& fs, Files* f) {
  if (f->base) fs.close(f->base);
  if (f->bin) fs.close(f->bin);
  *f = Files();
}

namespace {

// stats.bin's frame (the copy the session stands on: its generation and
// header CRC) and the base's (the one HEAD names: its generation, header
// CRC and rows), each through its watch.
Outcome frames(const Files& f, const Input& in, sx::Scratch* sc, tagstore::ReadWatch& binSrc,
               tagstore::ReadWatch& baseSrc, uint64_t* bytes) {
  if (!f.bin != !in.present || (in.baseGen != 0) != (f.base != nullptr)) return Outcome::Changed;
  if (f.bin) {
    const cc::Why w = sc->frame.open(binSrc, sx::kStatsSpec, nullptr);
    if (w != cc::Why::Ok) return binSrc.failed() ? Outcome::Io : Outcome::Changed;
    if (sc->frame.header().generation != in.generation || sc->frame.header().headerCrc != in.headerCrc)
      return Outcome::Changed;
    *bytes += sx::kHeaderBytes + static_cast<uint64_t>(sc->frame.header().sectionCount) * cc::kDirEntryBytes;
  }
  if (f.base) {
    cc::Why w = sc->base.open(baseSrc, sx::kBaseSpec, nullptr);
    if (w == cc::Why::Ok && (sc->base.header().generation != in.baseGen || sc->base.header().headerCrc != in.baseCrc ||
                             sc->base.section(0).count != in.baseRows))
      w = cc::Why::Counts;
    if (w != cc::Why::Ok) return baseSrc.failed() ? Outcome::Io : Outcome::Base;
    *bytes += sx::kBaseRowsAt;
  }
  return Outcome::Ok;
}

}  // namespace

Report rank(const Files& f, const Input& in, const Memory& mem, Ranker& ranker) {
  Report rep;
  ranker.begin(mem.lists, in.hasToday, in.today);
  Windows win;
  win.begin(mem.windows, mem.windowRows, in.hasToday, in.today);
  if (mem.played) {
    mem.played->n = 0;
    mem.played->lost = 0;
  }
  if (mem.sparse) mem.sparse->n = 0;

  sx::Scratch* sc = new (mem.work) sx::Scratch;
  uint8_t* r1 = mem.work + sx::kScratchBytes + sx::kBufBytes;
  uint8_t* r2 = r1 + sx::kReadBytes;
  tagstore::ReadWatch binSrc(f.bin), baseSrc(f.base);
  rep.outcome = frames(f, in, sc, binSrc, baseSrc, &rep.bytesRead);
  if (rep.outcome != Outcome::Ok) {
    ranker.finish();
    return rep;
  }

  // The tracks: base ⊕ TRKD ⊕ the records', by hash.
  sc->old.begin(f.base ? &baseSrc : nullptr, f.base ? sc->base.section(0) : cc::Section(), sf::kTrkd, r1,
                sx::kReadBytes);
  sc->card.begin(f.bin ? &binSrc : nullptr, f.bin ? sc->frame.section(sf::kTrkd) : cc::Section(), sf::kTrkd, r2,
                 sx::kReadBytes);
  Rows recs{in.trkd, in.trkdRows, 0, sf::kTrackBytes};
  uint8_t* bRow = sc->row[0];
  uint8_t* dRow = sc->row[1];
  uint8_t* rRow = sc->row[2];
  bool hb = sc->old.next(bRow), hd = sc->card.next(dRow), hr = recs.next(rRow);
  uint32_t baseAt = 0;
  Played* played = mem.played;
  const uint32_t room = played ? played->chunks * kPlayedChunk : 0;
  while (hb || hd || hr) {
    uint64_t h = UINT64_MAX;
    if (hb) h = std::min(h, cc::get64(bRow));
    if (hd) h = std::min(h, cc::get64(dRow));
    if (hr) h = std::min(h, cc::get64(rRow));
    sf::Track t;
    bool any = false;
    if (hb && cc::get64(bRow) == h) {
      t = sf::decodeTrack(bRow);
      any = true;
      if (mem.sparse && baseAt % kSparseEvery == 0 && mem.sparse->n < mem.sparse->cap) {
        uint8_t* s = mem.sparse->rows + static_cast<size_t>(mem.sparse->n++) * kSparseRowBytes;
        cc::put64(s, h);
        cc::put32(s + 8, baseAt);
      }
      ++baseAt;
      hb = sc->old.next(bRow);
    }
    if (hd && cc::get64(dRow) == h) {
      const sf::Track d = sf::decodeTrack(dRow);
      t = any ? sf::merge(t, d) : d;
      any = true;
      hd = sc->card.next(dRow);
    }
    if (hr && cc::get64(rRow) == h) {
      const sf::Track r = sf::decodeTrack(rRow);
      t = any ? sf::merge(t, r) : r;
      hr = recs.next(rRow);
    }
    ++rep.tracks;
    ranker.track(t);
    if (t.plays >= 1) {
      ++rep.played;
      if (played) {
        if (played->n < room) {
          played->chunk[played->n / kPlayedChunk][played->n % kPlayedChunk] = h;
          ++played->n;
        } else {
          ++played->lost;
        }
      }
    }
  }
  rep.baseRows = sc->old.rows();
  rep.trkdRows = sc->card.rows();
  rep.recordRows = in.trkdRows + in.rcntRows;
  rep.bytesRead += static_cast<uint64_t>(rep.baseRows + rep.trkdRows) * sf::kTrackBytes;
  Outcome o = ofWhy(sc->old.end());
  if (o == Outcome::Ok) o = ofWhy(sc->card.end());

  // RCNT's rows and the records': the windows.
  if (o == Outcome::Ok) {
    sc->card.begin(f.bin ? &binSrc : nullptr, f.bin ? sc->frame.section(sf::kRcnt) : cc::Section(), sf::kRcnt, r2,
                   sx::kReadBytes);
    Rows rc{in.rcnt, in.rcntRows, 0, sf::kStrides[sf::kRcnt]};
    uint8_t* row = sc->row[0];
    bool room30 = true;
    for (int from = 0; from < 2; ++from) {
      while (from == 0 ? sc->card.next(row) : rc.next(row)) {
        // (day u16, plays u16, hash u64)
        room30 = win.add(cc::get16(row), cc::get64(row + 4), cc::get16(row + 2)) && room30;
      }
    }
    rep.rcntRows = sc->card.rows();
    rep.bytesRead += static_cast<uint64_t>(rep.rcntRows) * sf::kStrides[sf::kRcnt];
    o = ofWhy(sc->card.end());
    if (o == Outcome::Ok && !room30) o = Outcome::Room;
    rep.windowRows = win.rows();
    if (o == Outcome::Ok) win.finish(ranker);
  }
  ranker.finish();
  rep.outcome = o;
  return rep;
}

Report rank(tagstore::Fs& fs, const Input& in, const Memory& mem, Ranker& ranker) {
  Files f;
  const Outcome o = open(fs, in, &f);
  if (o != Outcome::Ok) {
    Report rep;
    ranker.begin(mem.lists, in.hasToday, in.today);
    ranker.finish();
    rep.outcome = o;
    return rep;
  }
  const Report rep = rank(f, in, mem, ranker);
  close(fs, &f);
  return rep;
}

TrackRows trackRows(const Files& f, const Input& in, uint64_t hash, uint8_t* work) {
  TrackRows t;
  sx::Scratch* sc = new (work) sx::Scratch;
  uint8_t* r1 = work + sx::kScratchBytes + sx::kBufBytes;
  tagstore::ReadWatch binSrc(f.bin), baseSrc(f.base);
  uint64_t bytes = 0;
  t.outcome = frames(f, in, sc, binSrc, baseSrc, &bytes);
  if (t.outcome != Outcome::Ok) return t;
  // The base's row: a binary search, a 24 B read a step.
  if (f.base) {
    uint32_t lo = 0, hi = in.baseRows;
    uint8_t* row = sc->row[0];
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      ++t.reads;
      if (!baseSrc.read(sx::kBaseRowsAt + mid * sf::kTrackBytes, row, sf::kTrackBytes)) {
        t.outcome = Outcome::Io;
        return t;
      }
      const uint64_t h = cc::get64(row);
      if (h == hash) {
        t.base = true;
        t.baseRow = sf::decodeTrack(row);
        break;
      }
      if (h < hash) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
  }
  // stats.bin's TRKD row: streamed (its CRC and order checked to its end).
  if (f.bin) {
    sc->card.begin(&binSrc, sc->frame.section(sf::kTrkd), sf::kTrkd, r1, sx::kReadBytes);
    uint8_t* row = sc->row[1];
    while (sc->card.next(row)) {
      if (cc::get64(row) != hash) continue;
      t.delta = true;
      t.deltaRow = sf::decodeTrack(row);
    }
    t.outcome = ofWhy(sc->card.end());
    t.trkdRows = sc->card.rows();
  }
  return t;
}

}  // namespace statsrank
