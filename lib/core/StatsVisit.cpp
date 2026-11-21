// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsVisit.h"

#include <algorithm>
#include <cstring>

#include "CardContract.h"
#include "PathHashWalk.h"
#include "StatsFile.h"

namespace statsvisit {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sm = statsmap;
namespace sr = statsrank;
namespace sx = statsfile;

namespace {

// The header (9.7's table, 160 B; 154 B used, the rest 0).
constexpr size_t kOffMagic = 0;      // u16 kMagic
constexpr size_t kOffFlags = 2;      // u8 kFlag...
constexpr size_t kOffGrade = 3;      // u8 Grade
constexpr size_t kOffStamp = 4;      // u64 buildStamp()
constexpr size_t kOffTracks = 12;    // u32 trackCount()
constexpr size_t kOffTime = 16;      // u32 the ask's time (a row's: seconds since 2026 + 1; 0 undated)
constexpr size_t kOffToday = 20;     // u16 (kUnsetDay undated)
constexpr size_t kOffRound = 22;     // u32 Fold::liveRound()
constexpr size_t kOffRows = 26;      // u16 a list
constexpr size_t kOffTotals = 38;    // u32 a list
constexpr size_t kOffFirstDay = 62;  // u16 each: firstDay, newestDay, rcntFirstDay, sinceDay
constexpr size_t kOffNewestDay = 64;
constexpr size_t kOffRcntFirst = 66;
constexpr size_t kOffSince = 68;
constexpr size_t kOffDays = 70;      // 7 x (listenedMs u32, plays u16, Auto DJ's plays u16 and skips u16)
constexpr size_t kDayBytes = 10;
constexpr size_t kOffCount = 140;    // u16 each: Not played yet's count, albums, the bits' and the directory's offsets
constexpr size_t kOffAlbums = 142;
constexpr size_t kOffBits = 144;
constexpr size_t kOffDir = 146;
constexpr size_t kOffWindow = 148;   // u8, then 1 B spare
constexpr size_t kOffVersion = 150;  // u32
static_assert(kOffDays + sr::kWeekDays * kDayBytes == kOffCount, "the 7 days' 70 B");
static_assert(kOffVersion + 4 <= kHeaderBytes, "the header's 160 B");

constexpr size_t kAlign = 16;
size_t up(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

// Not played yet's bytes: the bits, then a u16 a 64 albums.
uint32_t bitBytes(uint32_t albums) { return (albums + 7) / 8; }
uint32_t groups(uint32_t albums) { return (albums + 63) / 64; }
size_t notPlayedBytes(uint32_t albums) { return bitBytes(albums) + 2 * static_cast<size_t>(groups(albums)); }

// Each list in its room, untrimmed: 3,432 B after the header.
constexpr size_t listsBytes() {
  size_t n = 0;
  for (int l = 0; l < sr::kLists; ++l) n += static_cast<size_t>(sr::rowsOf(static_cast<List>(l))) * kStrides[l];
  return n;
}
static_assert(kHeaderBytes + listsBytes() == 3592, "9.7's lists: 792, 264, 1,296 and 1,080 B");
// The pair the trim takes a row of each: 11 B.
constexpr uint32_t kPairBytes = kStrides[sr::kOldFavourites] + kStrides[sr::kRecent];
constexpr uint32_t kTrimMost = sr::rowsOf(sr::kOldFavourites) < sr::rowsOf(sr::kRecent) ? sr::rowsOf(sr::kOldFavourites)
                                                                                           : sr::rowsOf(sr::kRecent);

uint16_t sat16(uint32_t v, uint32_t top) { return static_cast<uint16_t>(v > top ? top : v); }

}  // namespace

const char* gradeName(Grade g) {
  switch (g) {
    case Grade::Full: return "Full";
    case Grade::Lists: return "Lists";
    case Grade::Lean: return "Lean";
    case Grade::NoFolders: return "No folders";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// The snapshot's bytes
// ---------------------------------------------------------------------------
uint32_t trimFor(uint32_t albums) {
  const size_t whole = kHeaderBytes + listsBytes() + notPlayedBytes(albums);
  if (whole <= kBound) return 0;
  const size_t rows = (whole - kBound + kPairBytes - 1) / kPairBytes;
  return rows > kTrimMost ? kTrimMost : static_cast<uint32_t>(rows);
}

uint32_t roomOf(List l, uint32_t albums) {
  if (l >= sr::kLists) return 0;
  const uint32_t room = sr::rowsOf(l);
  return l == sr::kOldFavourites || l == sr::kRecent ? room - trimFor(albums) : room;
}

size_t offsetOf(List l, uint32_t albums) {
  if (l >= sr::kLists) return 0;
  size_t at = kHeaderBytes;
  for (int m = 0; m < l; ++m) at += static_cast<size_t>(roomOf(static_cast<List>(m), albums)) * kStrides[m];
  return at;
}

size_t bytesFor(uint32_t albums) {
  if (albums > kMaxAlbums) return 0;
  return offsetOf(sr::kRecent, albums) + static_cast<size_t>(roomOf(sr::kRecent, albums)) * kStrides[sr::kRecent] +
         notPlayedBytes(albums);
}

// ---------------------------------------------------------------------------
// The snapshot as the pages read it
// ---------------------------------------------------------------------------
bool Snapshot::valid() const { return cc::get16(b() + kOffMagic) == kMagic; }
uint8_t Snapshot::flags() const { return b()[kOffFlags]; }
Grade Snapshot::grade() const { return static_cast<Grade>(b()[kOffGrade]); }
uint64_t Snapshot::buildStamp() const { return cc::get64(b() + kOffStamp); }
uint32_t Snapshot::trackCount() const { return cc::get32(b() + kOffTracks); }
uint32_t Snapshot::time() const { return cc::get32(b() + kOffTime); }
uint16_t Snapshot::today() const { return cc::get16(b() + kOffToday); }
uint32_t Snapshot::liveRound() const { return cc::get32(b() + kOffRound); }
uint32_t Snapshot::rows(List l) const { return l < sr::kLists ? cc::get16(b() + kOffRows + 2 * l) : 0; }
uint32_t Snapshot::total(List l) const { return l < sr::kLists ? cc::get32(b() + kOffTotals + 4 * l) : 0; }
uint16_t Snapshot::firstDay() const { return cc::get16(b() + kOffFirstDay); }
uint16_t Snapshot::newestDay() const { return cc::get16(b() + kOffNewestDay); }
uint16_t Snapshot::rcntFirstDay() const { return cc::get16(b() + kOffRcntFirst); }
uint16_t Snapshot::sinceDay() const { return cc::get16(b() + kOffSince); }
uint32_t Snapshot::spanDays() const {
  const uint16_t first = firstDay(), newest = newestDay();
  return first == sf::kUnsetDay || newest == sf::kUnsetDay || newest < first ? 0 : newest - first + 1u;
}
uint32_t Snapshot::notPlayedCount() const { return hasNotPlayed() ? cc::get16(b() + kOffCount) : 0; }
uint32_t Snapshot::albums() const { return cc::get16(b() + kOffAlbums); }
uint32_t Snapshot::bitsOffset() const { return cc::get16(b() + kOffBits); }
uint32_t Snapshot::directoryOffset() const { return cc::get16(b() + kOffDir); }
uint8_t Snapshot::window() const { return b()[kOffWindow]; }
void Snapshot::setWindow(uint8_t w) { b()[kOffWindow] = w <= kAllTime ? w : kAllTime; }
uint32_t Snapshot::version() const { return cc::get32(b() + kOffVersion); }

sr::Day Snapshot::day(uint32_t i) const {
  sr::Day d;
  if (i >= sr::kWeekDays) return d;
  const uint8_t* p = b() + kOffDays + i * kDayBytes;
  d.listenedMs = cc::get32(p);
  d.plays = cc::get16(p + 4);
  d.autoDjPlays = cc::get16(p + 6);
  d.autoDjSkips = cc::get16(p + 8);
  return d;
}

const uint8_t* Snapshot::ids(List l) const { return l < sr::kLists ? b() + offsetOf(l, albums()) : nullptr; }

Row Snapshot::rowAt(List l, uint32_t i) const {
  Row r;
  if (l >= sr::kLists || i >= roomOf(l, albums())) return r;
  const uint8_t* p = ids(l) + static_cast<size_t>(i) * kStrides[l];
  r.id = cc::get16(p);
  switch (l) {
    case sr::kNewFinds: {
      const uint16_t v = cc::get16(p + 2);
      r.count = static_cast<uint16_t>(v & kNewFindsCountMax);
      r.src = static_cast<uint8_t>(v >> 14);
      break;
    }
    case sr::kOldFavourites:
      r.count = cc::get16(p + 2);
      r.lastDay = cc::get16(p + 4);
      break;
    case sr::kRecent:
      r.when = static_cast<uint32_t>(p[2]) | static_cast<uint32_t>(p[3]) << 8 | static_cast<uint32_t>(p[4]) << 16;
      break;
    default:
      r.count = cc::get16(p + 2);
      break;
  }
  return r;
}

uint32_t Snapshot::albumAt(uint32_t k) const {
  if (k >= notPlayedCount()) return kNoAlbum;
  const uint32_t n = albums();
  const uint8_t* bits = b() + bitsOffset();
  const uint8_t* dir = b() + directoryOffset();
  // The directory first (each 64 albums' clear bits), then the group's bits.
  for (uint32_t g = 0; g < groups(n); ++g) {
    const uint32_t clear = cc::get16(dir + 2 * g);
    if (k >= clear) {
      k -= clear;
      continue;
    }
    const uint32_t end = std::min(n, (g + 1) * 64);
    for (uint32_t pos = g * 64; pos < end; ++pos) {
      if (bits[pos / 8] >> (pos % 8) & 1) continue;
      if (k == 0) return pos;
      --k;
    }
    return kNoAlbum;  // (a directory that isn't its bits': none)
  }
  return kNoAlbum;
}

// ---------------------------------------------------------------------------
// The copy-out, the ids, Not played yet
// ---------------------------------------------------------------------------
bool write(const sr::Ranker& ranker, const sm::Wanted& wanted, const sr::Report& report, const Context& ctx,
           Snapshot* snap, size_t bytes) {
  if (!snap || ctx.trackCount > kMaxTracks || ctx.albums > kMaxAlbums) return false;
  // The block as carved, not the index's albums alone: a held snapshot
  // re-asked over an index with more albums would be written past its end.
  // (A backstop: StatsLists carves such a snapshot again at the turn, and
  // its loop()'s key check frees one over a remount's index.)
  if (bytesFor(ctx.albums) > bytes) return false;
  uint8_t* p = reinterpret_cast<uint8_t*>(snap);
  // The flags first, then every byte: a block an earlier try left
  // part-written (a yield, Unread) is rewritten whole, never read meanwhile.
  p[kOffFlags] = 0;
  std::memset(p, 0, bytesFor(ctx.albums));
  const bool dated = ranker.hasToday();
  cc::put16(p + kOffMagic, kMagic);
  p[kOffGrade] = static_cast<uint8_t>(ctx.grade);
  cc::put64(p + kOffStamp, ctx.buildStamp);
  cc::put32(p + kOffTracks, ctx.trackCount);
  cc::put32(p + kOffTime, dated ? ctx.time : 0);
  cc::put16(p + kOffToday, dated ? ranker.today() : sf::kUnsetDay);
  cc::put32(p + kOffRound, ctx.liveRound);
  // Each list: its kept rows straight in (9.7's copy-out, before the
  // Ranker's room is reused), each with its wanted row (resolve() makes it
  // the track's id) and its facts.
  for (int li = 0; li < sr::kLists; ++li) {
    const List l = static_cast<List>(li);
    const uint32_t n = std::min(ranker.size(l), roomOf(l, ctx.albums));
    uint8_t* row = p + offsetOf(l, ctx.albums);
    for (uint32_t i = 0; i < n; ++i, row += kStrides[l]) {
      const sr::Entry& e = ranker.at(l, i);
      const int64_t k = wanted.find(e.hash);
      cc::put16(row, k < 0 || k >= kNoRow ? kNoRow : static_cast<uint16_t>(k));
      switch (l) {
        case sr::kNewFinds:
          cc::put16(row + 2, static_cast<uint16_t>(sat16(e.count, kNewFindsCountMax) | (e.src & 3u) << 14));
          break;
        case sr::kOldFavourites:
          cc::put16(row + 2, sat16(e.count, kCountMax));
          cc::put16(row + 4, e.at ? sf::dayOfTime(e.at) : sf::kUnsetDay);
          break;
        case sr::kRecent: {
          // Minutes since 2026-01-01 (a row's time is seconds plus 1); no
          // dated play: this session's (lastOrd the live round) or earlier.
          uint32_t when = 0;
          if (e.at) {
            when = (e.at - 1) / 60;
            if (when > kWhenMax) when = kWhenMax;
          } else {
            when = e.ord == ctx.liveRound ? kWhenSession : kWhenEarlier;
          }
          row[2] = static_cast<uint8_t>(when);
          row[3] = static_cast<uint8_t>(when >> 8);
          row[4] = static_cast<uint8_t>(when >> 16);
          break;
        }
        default:
          cc::put16(row + 2, sat16(e.count, kCountMax));
          break;
      }
    }
    cc::put16(p + kOffRows + 2 * li, static_cast<uint16_t>(n));
    cc::put32(p + kOffTotals + 4 * li, ranker.total(l));
  }
  cc::put16(p + kOffFirstDay, report.firstDay);
  cc::put16(p + kOffNewestDay, report.newestDay);
  cc::put16(p + kOffRcntFirst, report.rcntFirstDay);
  cc::put16(p + kOffSince, ctx.sinceDay);
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    uint8_t* d = p + kOffDays + i * kDayBytes;
    cc::put32(d, report.days[i].listenedMs);
    cc::put16(d + 4, report.days[i].plays);
    cc::put16(d + 6, report.days[i].autoDjPlays);
    cc::put16(d + 8, report.days[i].autoDjSkips);
  }
  // Not played yet: none yet (notPlayed()), its room's place.
  const size_t bits = offsetOf(sr::kRecent, ctx.albums) +
                      static_cast<size_t>(roomOf(sr::kRecent, ctx.albums)) * kStrides[sr::kRecent];
  cc::put16(p + kOffAlbums, static_cast<uint16_t>(ctx.albums));
  cc::put16(p + kOffBits, static_cast<uint16_t>(bits));
  cc::put16(p + kOffDir, static_cast<uint16_t>(bits + bitBytes(ctx.albums)));
  p[kOffWindow] = ctx.window <= kAllTime ? ctx.window : kAllTime;
  cc::put32(p + kOffVersion, ctx.version);
  p[kOffFlags] = static_cast<uint8_t>((dated ? kFlagDated : 0) | kFlagWritten);
  return true;
}

bool resolve(const sm::Wanted& wanted, Snapshot* snap) {
  if (!snap || !snap->written() || snap->resolved()) return false;
  uint8_t* p = reinterpret_cast<uint8_t*>(snap);
  const uint32_t albums = snap->albums();
  for (int li = 0; li < sr::kLists; ++li) {
    const List l = static_cast<List>(li);
    const uint32_t stride = kStrides[l];
    uint8_t* base = p + offsetOf(l, albums);
    const uint32_t n = std::min(snap->rows(l), roomOf(l, albums));
    uint32_t out = 0, dropped = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* row = base + static_cast<size_t>(i) * stride;
      const uint16_t k = cc::get16(row);
      const uint32_t id = k < wanted.n ? wanted.idAt(k) : sm::kNoId;
      // An orphan (kNoId), a collision (kTwoTracks), no wanted row: out.
      if (id >= kMaxTracks) {
        ++dropped;
        continue;
      }
      uint8_t* to = base + static_cast<size_t>(out) * stride;
      if (to != row) std::memcpy(to, row, stride);  // (out < i: rows apart)
      cc::put16(to, static_cast<uint16_t>(id));
      ++out;
    }
    const uint32_t keep = std::min(out, sr::kCaps[l]);
    std::memset(base + static_cast<size_t>(keep) * stride, 0, static_cast<size_t>(n - keep) * stride);
    cc::put16(p + kOffRows + 2 * li, static_cast<uint16_t>(keep));
    const uint32_t total = cc::get32(p + kOffTotals + 4 * li);
    cc::put32(p + kOffTotals + 4 * li, total > dropped ? total - dropped : 0);
  }
  p[kOffFlags] = static_cast<uint8_t>(p[kOffFlags] | kFlagResolved);
  return true;
}

bool notPlayed(const sm::Map& map, const LibraryIndex& index, Snapshot* snap) {
  if (!snap || !snap->written()) return false;
  if (map.status() != sm::Map::Status::Ready || !map.matches(&index)) return false;
  const uint32_t n = snap->albums();
  if (n != index.albumCount() || map.albums() != n || !snap->matches(index.buildStamp(), index.trackCount()))
    return false;
  uint8_t* p = reinterpret_cast<uint8_t*>(snap);
  uint8_t* bits = p + snap->bitsOffset();
  uint8_t* dir = p + snap->directoryOffset();
  // The bits in albumsAZ()'s order (set: played), those past the last album
  // set (none reads as not played); then each 64 albums' clear bits.
  std::memset(bits, 0, bitBytes(n));
  const LibraryIndex::Span az = index.albumsAZ();
  for (uint32_t pos = 0; pos < bitBytes(n) * 8; ++pos)
    if (pos >= n || map.albumPlayed(az[pos])) bits[pos / 8] = static_cast<uint8_t>(bits[pos / 8] | 1u << (pos % 8));
  uint32_t count = 0;
  for (uint32_t g = 0; g < groups(n); ++g) {
    uint32_t clear = 0;
    const uint32_t end = std::min(n, (g + 1) * 64);
    for (uint32_t pos = g * 64; pos < end; ++pos) clear += (bits[pos / 8] >> (pos % 8) & 1) ? 0 : 1;
    cc::put16(dir + 2 * g, static_cast<uint16_t>(clear));
    count += clear;
  }
  cc::put16(p + kOffCount, static_cast<uint16_t>(count));
  p[kOffFlags] = static_cast<uint8_t>(p[kOffFlags] | kFlagNotPlayed);
  return true;
}

// ---------------------------------------------------------------------------
// The grades
// ---------------------------------------------------------------------------
namespace {

Region regionAt(size_t at, size_t bytes) { return bytes ? Region{at, bytes} : Region{}; }

}  // namespace

Layout layout(Grade grade, size_t head, uint32_t rows, size_t foldersBytes, uint32_t albums, bool dated) {
  Layout y;
  y.grade = grade;
  y.dated = dated;
  const bool lists = grade == Grade::Full || grade == Grade::Lists;
  const bool playedSet = grade != Grade::Lists;
  // The parts (9.7's grades): the records' copy all 40 of each kind the
  // rank reads (TRKD's 24 B, RCNT's 12, DAYS' 16), TRKD's alone for the
  // played-set-only rank; RCNT's window its 4,096 rows and the records'.
  const size_t front = up(head);
  const size_t work = up(sx::kWorkBytes);
  const size_t ranker = lists ? up(sr::Ranker::kBytes) : 0;
  const uint32_t recordRow = lists ? sf::kTrackBytes + sf::kStrides[sf::kRcnt] + sf::kStrides[sf::kDays]
                                   : sf::kTrackBytes;
  const size_t records = up(static_cast<size_t>(sf::kRecords) * recordRow);
  y.windowRows = lists && dated ? sf::kRcntRows + sf::kRecords : 0;
  const size_t windows = up(sr::Windows::bytesFor(y.windowRows));
  y.playedRows = playedSet ? rows : 0;
  const size_t played = playedSet ? sr::Played40::bytesFor(rows) : 0;
  const size_t folders = grade == Grade::NoFolders ? 0 : up(foldersBytes);
  const size_t bits = playedSet ? up(sm::albumBytes(albums)) : 0;
  y.wantedRows = lists ? sr::Ranker::kEntries : 0;
  const size_t wanted = up(sm::Wanted::bytesFor(y.wantedRows));
  y.head = regionAt(0, front);
  const size_t a = front;
  switch (grade) {
    case Grade::Full: {
      // A: the rank's, then the walk's over it (the wanted set at its tail,
      // apart from the Ranker it is made from); PW: the window, then the
      // u40 set over its dead rows.
      const size_t room = std::max({ranker + work + records, folders + bits + wanted, ranker + wanted});
      y.ranker = regionAt(a, ranker);
      y.work = regionAt(a + ranker, work);
      y.records = regionAt(a + ranker + work, records);
      y.folders = regionAt(a, folders);
      y.bits = regionAt(a + folders, bits);
      y.wanted = regionAt(a + room - wanted, wanted);
      y.windows = regionAt(a + room, windows);
      y.played = regionAt(a + room, played);
      y.bytes = a + room + std::max(windows, played);
      break;
    }
    case Grade::Lists: {
      const size_t room = std::max({ranker + work + windows + records, folders + wanted, ranker + wanted});
      y.ranker = regionAt(a, ranker);
      y.work = regionAt(a + ranker, work);
      y.windows = regionAt(a + ranker + work, windows);
      y.records = regionAt(a + ranker + work + windows, records);
      y.folders = regionAt(a, folders);
      y.wanted = regionAt(a + room - wanted, wanted);
      y.bytes = a + room;
      break;
    }
    case Grade::Lean:
    case Grade::NoFolders: {
      const size_t room = std::max(work + records, folders + bits);
      y.work = regionAt(a, work);
      y.records = regionAt(a + work, records);
      y.folders = regionAt(a, folders);
      y.bits = regionAt(a + folders, bits);
      y.played = regionAt(a + room, played);
      y.bytes = a + room + played;
      break;
    }
  }
  return y;
}

// ---------------------------------------------------------------------------
// The phase
// ---------------------------------------------------------------------------
const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Working: return "working";
    case Phase::Waiting: return "waiting";
    case Phase::Ready: return "ready";
    case Phase::NoRoom: return "no room";
    case Phase::Unread: return "unread";
    case Phase::NoCard: return "no card";
    case Phase::Capped: return "capped";
  }
  return "?";
}

const char* outcomeName(Outcome o) {
  switch (o) {
    case Outcome::None: return "none";
    case Outcome::Ok: return "ok";
    case Outcome::NoRoom: return "no room";
    case Outcome::NoBlock: return "no block";
    case Outcome::Changed: return "changed";
    case Outcome::Io: return "io";
    case Outcome::Damaged: return "damaged";
    case Outcome::Base: return "base";
    case Outcome::Cancelled: return "cancelled";
    case Outcome::Room: return "room";
    case Outcome::CardAway: return "card away";
  }
  return "?";
}

Outcome outcomeOf(sr::Outcome o) {
  switch (o) {
    case sr::Outcome::Ok: return Outcome::Ok;
    case sr::Outcome::Changed: return Outcome::Changed;
    case sr::Outcome::Base: return Outcome::Base;
    case sr::Outcome::Io: return Outcome::Io;
    case sr::Outcome::Damaged: return Outcome::Damaged;
    case sr::Outcome::Room: return Outcome::Room;
  }
  return Outcome::Io;
}

Phase phaseOf(uint8_t why, Outcome outcome, ScanScheduler::Wait wait, uint32_t waitedMs, bool capped, bool served) {
  if (why & kWhyOff) return Phase::Idle;
  // The card away or held: the ask cancelled, the snapshot freed (9.7);
  // an ask it ended stays No card until the next, the page offering Try
  // again once the card is back (CardAway, why 16 gone).
  if (why & kWhyCard) return Phase::NoCard;
  switch (outcome) {
    case Outcome::CardAway: return Phase::NoCard;
    case Outcome::Ok: return served ? Phase::Ready : Phase::Idle;  // (no Ready over no lists)
    case Outcome::NoRoom:
    case Outcome::NoBlock:
    case Outcome::Room: return Phase::NoRoom;  // every grade refused
    case Outcome::Changed:
    case Outcome::Io:
    case Outcome::Damaged:
    case Outcome::Base: return Phase::Unread;  // a read fault, and nothing else
    default: break;
  }
  if (capped) return Phase::Capped;
  if (outcome == Outcome::Cancelled) return Phase::Idle;
  // The boot's scan (its fold, its save): Working however long, 9.7's "2
  // (the boot's scan): Working" (4.4's Folding, "Counting plays…"). S2
  // gives it before every later gate, so it stands for them too.
  if (why & kWhyBoot) return Phase::Working;
  // S2's other gates (4, 8, 32, 64), S3a's tag scan's (128) and the Rank
  // lane's waits (before the take, or after it, the block given back):
  // Working, then Waiting from 3 s. Nothing is counted.
  if (why != 0 || wait != ScanScheduler::Wait::None) return waitedMs >= kWaitingMs ? Phase::Waiting : Phase::Working;
  return Phase::Working;
}

}  // namespace statsvisit
