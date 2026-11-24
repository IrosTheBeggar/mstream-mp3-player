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
    // With the clock unset, this session's rows first (the owner's call,
    // 2026-11-24, "Yes, fix the order": 4.3's "This session", then
    // "Earlier"; the Ranker marks them, never in a dated rank). Then, within
    // each part, 4.3's order as before: lastAt, newest first; then the rows
    // with no dated play (lastAt 0) by lastOrd, newest first; then the hash.
    // Within this session no play order is known (its plays have no time
    // and one round), so its rows keep that order too: a track played
    // again keeps its dated lastAt from before and comes first, then the
    // never-dated ones by the hash. Earlier keeps it whole, as a dated rank
    // orders the same rows (the dated by lastAt, then the rest by round).
    if (a.session != b.session) return a.session;
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

void Ranker::begin(void* mem, bool hasToday, uint16_t today, uint32_t liveRound) {
  Entry* base = static_cast<Entry*>(mem);
  for (int l = 0; l < kLists; ++l) {
    list_[l] = base + kOffsets[l];
    n_[l] = 0;
    total_[l] = 0;
  }
  hasToday_ = hasToday;
  today_ = hasToday ? today : 0;
  liveRound_ = liveRound;
}

void Ranker::offer(List l, const Entry& x) {
  ++total_[l];  // every offer, kept or not (S3a's totals)
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
    // With the clock unset: this session's when its latest play has the
    // live round, whatever lastAt (a track played again with no clock
    // keeps its dated lastAt: 5.9).
    e.session = !hasToday_ && liveRound_ != 0 && t.lastOrd == liveRound_;
    offer(kRecent, e);
  }
  if (!hasToday_) return;
  const uint16_t from30 = first30();
  // (A dated time: neither 0 nor kNoFirst.)
  if (t.firstAt != 0 && t.firstAt != sf::kNoFirst) {
    const uint16_t d = sf::dayOfTime(t.firstAt);
    if (d >= from30 && d <= today_) {
      Entry e;
      e.hash = t.hash;
      e.count = t.plays;
      e.at = t.firstAt;
      e.src = t.src;
      offer(kNewFinds, e);
    }
  }
  if (t.plays >= kOldFavourite && t.lastAt != 0 && sf::dayOfTime(t.lastAt) < from30) {
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

uint64_t Played40::at(uint32_t i) const {
  const uint8_t* p = rows + static_cast<size_t>(i) * kRowBytes;
  return static_cast<uint64_t>(p[4]) << 32 | cc::get32(p);
}

bool Played40::contains(uint64_t hash) const {
  const uint64_t key = keyOf(hash);
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint64_t k = at(mid);
    if (k == key) return true;
    if (k < key) {
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
  // No index: the whole base.
  if (n == 0) {
    *from = 0;
    *to = baseRows;
    return;
  }
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
    *bytes += sc->base.section(0).offset;
  }
  return Outcome::Ok;
}

// A saturating sum at a row field's width (a save's ⊕: addRow()).
uint32_t addAt(uint32_t a, uint32_t b, uint32_t top) {
  const uint64_t s = static_cast<uint64_t>(a) + b;
  return s > top ? top : static_cast<uint32_t>(s);
}

// What one rank reads with, through its three parts below.
struct Read {
  const Files& f;
  const Input& in;
  const Memory& mem;
  sx::Scratch* sc;
  tagstore::ReadWatch& binSrc;
  tagstore::ReadWatch& baseSrc;
  uint8_t* r1;  // a read stream's buffer (the base's)
  uint8_t* r2;  // another (stats.bin's sections)
  Report& rep;
};

// DAYS ⊕ the records' DAYS rows (S3a; 4.2 as decided): every row's day to
// the span, clock or not; a day of the 7-day window to its report's day,
// its fields summed as a save's ⊕ sums them. stats.bin's rows are read
// whole (their CRC and order).
Outcome readDays(Read& r) {
  r.sc->card.begin(r.f.bin ? &r.binSrc : nullptr, r.f.bin ? r.sc->frame.section(sf::kDays) : cc::Section(), sf::kDays,
                   r.r2, sx::kReadBytes);
  Rows recs{r.in.days, r.in.daysRows, 0, sf::kStrides[sf::kDays]};
  uint8_t* row = r.sc->row[0];
  Report& rep = r.rep;
  for (int from = 0; from < 2; ++from) {
    // (day u16, plays u16, skips u16, Auto DJ's plays u8 and skips u8,
    // listenedMs u32, shuffle's plays and skips u8, 2 B)
    while (from == 0 ? r.sc->card.next(row) : recs.next(row)) {
      const uint16_t day = cc::get16(row);
      if (rep.firstDay == sf::kUnsetDay || day < rep.firstDay) rep.firstDay = day;
      if (rep.newestDay == sf::kUnsetDay || day > rep.newestDay) rep.newestDay = day;
      // The 30-day window's first played day (Windows::add()'s days), for
      // rcntShort().
      if (r.in.hasToday && day <= r.in.today && static_cast<uint32_t>(r.in.today - day) < 30u &&
          cc::get16(row + 2) > 0 && (rep.days30FirstDay == sf::kUnsetDay || day < rep.days30FirstDay))
        rep.days30FirstDay = day;
      if (!r.in.hasToday || day > r.in.today || static_cast<uint32_t>(r.in.today - day) >= kWeekDays) continue;
      Day& d = rep.days[kWeekDays - 1 - (r.in.today - day)];
      d.plays = static_cast<uint16_t>(addAt(d.plays, cc::get16(row + 2), 0xFFFF));
      d.autoDjPlays = static_cast<uint16_t>(addAt(d.autoDjPlays, row[6], 0xFF));
      d.autoDjSkips = static_cast<uint16_t>(addAt(d.autoDjSkips, row[7], 0xFF));
      d.listenedMs = addAt(d.listenedMs, cc::get32(row + 8), 0xFFFFFFFF);
    }
  }
  rep.daysRows = r.sc->card.rows();
  rep.recordRows += r.in.daysRows;
  rep.bytesRead += static_cast<uint64_t>(rep.daysRows) * sf::kStrides[sf::kDays];
  return ofWhy(r.sc->card.end());
}

// RCNT's rows and the records' into the windows, finished into the Ranker
// (the windows' rows dead from here); RCNT's oldest day to the report.
// Room (the windows' block short) is said once the section is read whole.
Outcome readWindows(Read& r, Ranker& ranker) {
  Windows win;
  win.begin(r.mem.windows, r.mem.windowRows, r.in.hasToday, r.in.today);
  r.sc->card.begin(r.f.bin ? &r.binSrc : nullptr, r.f.bin ? r.sc->frame.section(sf::kRcnt) : cc::Section(), sf::kRcnt,
                   r.r2, sx::kReadBytes);
  Rows rc{r.in.rcnt, r.in.rcntRows, 0, sf::kStrides[sf::kRcnt]};
  uint8_t* row = r.sc->row[0];
  Report& rep = r.rep;
  bool room30 = true;
  for (int from = 0; from < 2; ++from) {
    while (from == 0 ? r.sc->card.next(row) : rc.next(row)) {
      // (day u16, plays u16, hash u64)
      const uint16_t day = cc::get16(row);
      if (rep.rcntFirstDay == sf::kUnsetDay || day < rep.rcntFirstDay) rep.rcntFirstDay = day;
      room30 = win.add(day, cc::get64(row + 4), cc::get16(row + 2)) && room30;
    }
  }
  rep.rcntRows = r.sc->card.rows();
  rep.recordRows += r.in.rcntRows;
  rep.bytesRead += static_cast<uint64_t>(rep.rcntRows) * sf::kStrides[sf::kRcnt];
  Outcome o = ofWhy(r.sc->card.end());
  if (o == Outcome::Ok && !room30) o = Outcome::Room;
  rep.windowRows = win.rows();
  if (o == Outcome::Ok) win.finish(ranker);
  return o;
}

// The tracks: base ⊕ TRKD ⊕ the records', by hash, each whole row to the
// Ranker (none: the played-set-only rank) and, played, to the played set.
Outcome readTracks(Read& r, Ranker* ranker) {
  const Files& f = r.f;
  sx::Scratch* sc = r.sc;
  Report& rep = r.rep;
  sc->old.begin(f.base ? &r.baseSrc : nullptr, f.base ? sc->base.section(0) : cc::Section(), sf::kTrkd, r.r1,
                sx::kReadBytes);
  sc->card.begin(f.bin ? &r.binSrc : nullptr, f.bin ? sc->frame.section(sf::kTrkd) : cc::Section(), sf::kTrkd, r.r2,
                 sx::kReadBytes);
  Rows recs{r.in.trkd, r.in.trkdRows, 0, sf::kTrackBytes};
  uint8_t* bRow = sc->row[0];
  uint8_t* dRow = sc->row[1];
  uint8_t* rRow = sc->row[2];
  bool hb = sc->old.next(bRow), hd = sc->card.next(dRow), hr = recs.next(rRow);
  uint32_t baseAt = 0;
  // One set or the other: the u40 set when given.
  Played40* played40 = r.mem.played40;
  Played* played = played40 ? nullptr : r.mem.played;
  const uint32_t room = played ? played->chunks * kPlayedChunk : 0;
  Sparse* sparse = r.mem.sparse;
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
      if (sparse && baseAt % kSparseEvery == 0 && sparse->n < sparse->cap) {
        uint8_t* s = sparse->rows + static_cast<size_t>(sparse->n++) * kSparseRowBytes;
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
      const sf::Track rec = sf::decodeTrack(rRow);
      t = any ? sf::merge(t, rec) : rec;
      hr = recs.next(rRow);
    }
    ++rep.tracks;
    if (ranker) ranker->track(t);
    if (t.plays >= 1) {
      ++rep.played;
      if (played40) {
        if (played40->n < played40->cap) {
          uint8_t* p = played40->rows + static_cast<size_t>(played40->n++) * Played40::kRowBytes;
          const uint64_t key = Played40::keyOf(h);
          cc::put32(p, static_cast<uint32_t>(key));
          p[4] = static_cast<uint8_t>(key >> 32);
        } else {
          ++played40->lost;
        }
      } else if (played) {
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
  rep.recordRows += r.in.trkdRows;
  rep.bytesRead += static_cast<uint64_t>(rep.baseRows + rep.trkdRows) * sf::kTrackBytes;
  const Outcome o = ofWhy(sc->old.end());
  return o == Outcome::Ok ? ofWhy(sc->card.end()) : o;
}

}  // namespace

Report rank(const Files& f, const Input& in, const Memory& mem, Ranker& ranker) {
  Report rep;
  // No lists: the played-set-only rank (S3a's V1): the Ranker not touched.
  const bool lists = mem.lists != nullptr;
  if (lists) ranker.begin(mem.lists, in.hasToday, in.today, in.liveRound);
  if (mem.played40) {
    mem.played40->n = 0;
    mem.played40->lost = 0;
  } else if (mem.played) {
    mem.played->n = 0;
    mem.played->lost = 0;
  }
  if (mem.sparse) mem.sparse->n = 0;

  sx::Scratch* sc = new (mem.work) sx::Scratch;
  uint8_t* r1 = mem.work + sx::kScratchBytes + sx::kBufBytes;
  uint8_t* r2 = r1 + sx::kReadBytes;
  tagstore::ReadWatch binSrc(f.bin), baseSrc(f.base);
  Outcome o = frames(f, in, sc, binSrc, baseSrc, &rep.bytesRead);
  Read rd{f, in, mem, sc, binSrc, baseSrc, r1, r2, rep};
  // S3a's order (9.7's V1; 6.8 as decided): DAYS and RCNT first (stats.bin
  // holds them before TRKD), the windows finished into the Ranker, so
  // their rows are dead before the first played hash is written (the u40
  // set may take their room) and Room is said before the tracks are read;
  // then the tracks. Each list is its own bounded heap with the hash as its
  // last tie-break, so the order of the offers moves no row.
  if (o == Outcome::Ok && lists) o = readDays(rd);
  if (o == Outcome::Ok && lists) o = readWindows(rd, ranker);
  if (o == Outcome::Ok) o = readTracks(rd, lists ? &ranker : nullptr);
  if (lists) ranker.finish();
  rep.outcome = o;
  return rep;
}

Report rank(tagstore::Fs& fs, const Input& in, const Memory& mem, Ranker& ranker) {
  Files f;
  const Outcome o = open(fs, in, &f);
  if (o != Outcome::Ok) {
    Report rep;
    if (mem.lists) {
      ranker.begin(mem.lists, in.hasToday, in.today, in.liveRound);
      ranker.finish();
    }
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
  // The base's row: a binary search, a 24 B read a step, by TRKS's own
  // offset and stride (a newer minor's base: longer rows, read by their
  // prefix, 5.12).
  if (f.base) {
    const cc::Section& trks = sc->base.section(0);
    uint32_t lo = 0, hi = in.baseRows;
    uint8_t* row = sc->row[0];
    while (lo < hi) {
      const uint32_t mid = lo + (hi - lo) / 2;
      ++t.reads;
      if (!baseSrc.read(trks.offset + mid * trks.stride, row, sf::kTrackBytes)) {
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
