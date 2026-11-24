// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the listening stats' rankers (lib/core/StatsRank;
// docs/LISTENING-STATS.md 4.3, 6.8; S2's C12), against tools/statsref.py's
// lists (test/fixtures/card/stats):
// - every fixture boot that reads back (stats/index.json's `lists`): the
//   card's stats.bin and its base as the boot leaves them, read as the
//   firmware's Store reads them, ranked;
// - the rankers' scenarios (stats/rank.json): every list past its cap and
//   16, the clock unset, ties the hash decides, a window before day 0, TRKD
//   with no base;
// - the records (RAM's rows a save hasn't taken: 5.14) merged as a fourth
//   and fifth stream: random rows over rank-big's card, against an
//   independent ranking of base ⊕ TRKD ⊕ the records by sorting (which
//   gives statsref's lists with no records);
// - the played set (every hash with a play, ascending) and the sparse index
//   (each 64th base row: span() finds every base row);
// - the refusals: another stats.bin than the session's copy, none where
//   there was one, the base gone or another file, a read the card fails, a
//   section damaged, the windows' block short;
// - a track's rows (Hp's read): the base's by a binary search, TRKD's
//   streamed, as the files hold them.
// - a newer minor's base (longer header and rows): read by its rows'
//   prefixes, the rank's and a track's read alike (5.12).
// S3a's V1 (9.7, "S3a: tests"), on every fixture boot and rank scenario
// unless said:
// - the lists after the read's order swap (RCNT and DAYS first, the
//   windows finished into the Ranker, then the tracks), the u64 played set
//   and S2's report fields unchanged, against the independent ranking;
// - the totals (Ranker::total(): every row offered) at and past each cap,
//   and statsref's on the scenarios;
// - the 7 days and the span (DAYS ⊕ the records'), against statsref's:
//   the clock set and unset, plays across a UTC midnight (the card's and
//   the session's own fold's), DAYS at its 400 rows; RCNT's oldest day with
//   RCNT capped;
// - the u40 played set equal to the u64 set's top 40 bits (and statsref's
//   bytes on the scenarios), its room short;
// - a null window while undated; a dated one with no room is Room, before
//   the tracks are read;
// - the played-set-only rank (Memory::lists null): nothing offered, RCNT
//   and DAYS not read (a damaged one passes), no window, its u40 set the
//   full rank's, the clock set and unset;
// - the overlay: every region patterned first, the u40 set over the dead
//   window's rows, the lists, totals and set the separate regions'.
// The owner's call (2026-11-24, "Yes, fix the order"): with no clock,
// Recently played's rows of this session (lastOrd the live round, whatever
// lastAt) first, on rank-no-clock-session and on a session's own undated
// plays over 216 dated tracks or more; every card ranked by its session's
// live round (rank.json's, else the fold's).
// Run: pio test -e native -f test_stats_rank
#include <unity.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "PlayLog.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsRank.h"
#include "WallClock.h"

namespace cc = cardcontract;
namespace pl = playlog;
namespace sf = statsfold;
namespace sx = statsfile;
namespace sr = statsrank;

namespace {

minijson::Value gIndex;
minijson::Value gRank;
using Bytes = std::string;
const std::string kDir = "/.player/";

const uint8_t* u8(const Bytes& b) { return reinterpret_cast<const uint8_t*>(b.data()); }

// A block aligned as malloc's.
struct Block {
  std::unique_ptr<uint64_t[]> m;
  explicit Block(size_t bytes) : m(new uint64_t[(bytes + 7) / 8 + 1]) {}
  uint8_t* p() { return reinterpret_cast<uint8_t*>(m.get()); }
};

void put(cutfs::CutFs& card, const std::string& name, const Bytes& b) {
  card.put(kDir + name, std::vector<uint8_t>(b.begin(), b.end()));
}

// A frame's section bytes.
Bytes sectionOf(const Bytes& b, const cc::FormatSpec& spec, uint32_t i) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, spec, nullptr)));
  return b.substr(c.section(i).offset, c.section(i).bytes);
}

// The session's side: the card read as the boot reads it (no waits), and
// the rank's input from it.
struct Session {
  sf::Fold fold;
  std::unique_ptr<sx::Store> store;
  sx::Opened opened;
  sr::Input in;
};

void readCard(cutfs::CutFs& card, Session& s, int today) {
  Block work(sx::kWorkBytes);
  s.store.reset(new sx::Store(card));
  s.opened = s.store->read(s.fold, work.p(), nullptr);
  TEST_ASSERT_TRUE_MESSAGE(sx::statsOn(s.opened.read), sx::readName(s.opened.read));
  const sx::Store::Copy& c = s.store->cardCopy();
  s.in.present = c.present;
  s.in.generation = c.generation;
  s.in.headerCrc = c.headerCrc;
  s.in.baseGen = s.fold.head().baseGen;
  s.in.baseRows = s.fold.head().baseRows;
  s.in.baseCrc = s.fold.head().baseCrc;
  s.in.hasToday = today >= 0;
  s.in.today = static_cast<uint16_t>(today >= 0 ? today : 0);
  s.in.liveRound = s.fold.liveRound();  // (a session that boots on the card: HEAD's round + 2)
}

// A rank scenario's live round (rank.json's `liveRound`, given with the
// clock unset: the ranking session's, which may be one whose live save
// left the card), else the fold's.
void liveOf(const minijson::Value& sc, Session& s) {
  if (sc.has("liveRound")) s.in.liveRound = static_cast<uint32_t>(sc["liveRound"].u64());
}

// Everything a rank needs, carved for `windowRows` and `playedRows`: the
// u64 played set (S2's); use40() gives the u40 set instead, lean() takes
// the lists and the window away (the played-set-only rank).
struct Mem {
  Block work{sx::kWorkBytes};
  Block lists{sr::Ranker::kBytes};
  std::unique_ptr<Block> windows;
  std::vector<std::unique_ptr<Block>> chunks;
  std::unique_ptr<Block> sparseRows;
  std::unique_ptr<Block> rows40;
  sr::Played played;
  sr::Played40 played40;
  sr::Sparse sparse;
  sr::Memory m;
  void use40(uint32_t rows) {
    rows40.reset(new Block(sr::Played40::bytesFor(rows)));
    played40.rows = rows40->p();
    played40.cap = rows;
    m.played40 = &played40;
  }
  void lean() {
    m.lists = nullptr;
    m.windows = nullptr;
    m.windowRows = 0;
  }
  std::string bytes40() const {
    return std::string(reinterpret_cast<const char*>(played40.rows), played40.n * sr::Played40::kRowBytes);
  }
  Mem(uint32_t windowRows, uint32_t playedRows, uint32_t baseRows) {
    windows.reset(new Block(sr::Windows::bytesFor(windowRows)));
    for (uint32_t i = 0; i < sr::playedChunks(playedRows); ++i) {
      chunks.emplace_back(new Block(sr::kPlayedChunkBytes));
      played.chunk[i] = reinterpret_cast<uint64_t*>(chunks.back()->p());
    }
    played.chunks = static_cast<uint32_t>(chunks.size());
    sparse.cap = sr::sparseRows(baseRows);
    sparseRows.reset(new Block(sparse.cap * sr::kSparseRowBytes + 1));
    sparse.rows = sparseRows->p();
    m.work = work.p();
    m.lists = lists.p();
    m.windows = windows->p();
    m.windowRows = windowRows;
    m.played = &played;
    m.sparse = &sparse;
  }
};

std::string hx(uint64_t h) {
  char b[24];
  snprintf(b, sizeof(b), "0x%016llx", static_cast<unsigned long long>(h));
  return b;
}

std::string hexOf(const Bytes& b) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : b) {
    out += kDigits[c >> 4];
    out += kDigits[c & 15];
  }
  return out;
}

// A list's total against statsref's: null, a list the clock unset doesn't
// offer (none counted).
void sameTotal(const sr::Ranker& r, sr::List l, const minijson::Value& want, const std::string& what) {
  const std::string m = what + " total " + sr::listName(l);
  if (want.isNull()) {
    TEST_ASSERT_FALSE_MESSAGE(r.offered(l), m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.total(l), m.c_str());
    return;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.u64(), r.total(l), m.c_str());
}

void sameTotals(const sr::Ranker& r, const minijson::Value& t, const std::string& what) {
  sameTotal(r, sr::kMost7, t["mostPlayed"]["7"], what);
  sameTotal(r, sr::kMost30, t["mostPlayed"]["30"], what);
  sameTotal(r, sr::kMostAll, t["mostPlayed"]["all"], what);
  sameTotal(r, sr::kNewFinds, t["newFinds"], what);
  sameTotal(r, sr::kOldFavourites, t["oldFavourites"], what);
  sameTotal(r, sr::kRecent, t["recentlyPlayed"], what);
}

uint16_t dayOr(const minijson::Value& v) { return v.isNull() ? sf::kUnsetDay : static_cast<uint16_t>(v.u64()); }

// The 7 days and the span against statsref's days7().
void sameDays7(const sr::Report& rep, const minijson::Value& want, const std::string& what) {
  const minijson::Value& days = want["days"];
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    const std::string m = what + " day " + std::to_string(i);
    const sr::Day& d = rep.days[i];
    const minijson::Value& w = days[static_cast<size_t>(i)];
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(days.isNull() ? 0 : w[0].u64(), d.listenedMs, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(days.isNull() ? 0 : w[1].u64(), d.plays, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(days.isNull() ? 0 : w[2].u64(), d.autoDjPlays, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(days.isNull() ? 0 : w[3].u64(), d.autoDjSkips, m.c_str());
  }
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(dayOr(want["firstDay"]), rep.firstDay, (what + " firstDay").c_str());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(dayOr(want["newestDay"]), rep.newestDay, (what + " newestDay").c_str());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(dayOr(want["rcntFirstDay"]), rep.rcntFirstDay, (what + " rcntFirstDay").c_str());
  const uint32_t span = rep.firstDay == sf::kUnsetDay ? 0 : rep.newestDay - rep.firstDay + 1u;
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["spanDays"].u64(), span, (what + " spanDays").c_str());
}

// A list against the fixture's: [hash, count, ...] each, as statsref's
// lists_json() writes them.
void sameList(const sr::Ranker& r, sr::List l, const minijson::Value& want, const std::string& what) {
  const std::string m = what + " " + sr::listName(l);
  if (want.isNull()) {
    TEST_ASSERT_FALSE_MESSAGE(r.offered(l), m.c_str());
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(r.offered(l), m.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), r.size(l), m.c_str());
  for (uint32_t i = 0; i < r.size(l); ++i) {
    const sr::Entry& e = r.at(l, i);
    const minijson::Value& w = want[static_cast<size_t>(i)];
    const std::string mi = m + " row " + std::to_string(i);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(w[0].c_str(), hx(e.hash).c_str(), mi.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[1].u64(), e.count, mi.c_str());
    if (l == sr::kNewFinds) {
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[2].u64(), e.at, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[3].u64(), e.src, mi.c_str());
    } else if (l == sr::kOldFavourites) {
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[2].u64(), e.at, mi.c_str());
    } else if (l == sr::kRecent) {
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[2].u64(), e.at, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(w[3].u64(), e.ord, mi.c_str());
    }
  }
}

void sameLists(const sr::Ranker& r, const minijson::Value& lists, const std::string& what) {
  sameList(r, sr::kMost7, lists["mostPlayed"]["7"], what);
  sameList(r, sr::kMost30, lists["mostPlayed"]["30"], what);
  sameList(r, sr::kMostAll, lists["mostPlayed"]["all"], what);
  sameList(r, sr::kNewFinds, lists["newFinds"], what);
  sameList(r, sr::kOldFavourites, lists["oldFavourites"], what);
  sameList(r, sr::kRecent, lists["recentlyPlayed"], what);
}

int todayOf(const minijson::Value& v) { return v.isNull() ? -1 : static_cast<int>(v.u64()); }

// ---------------------------------------------------------------------------
// An independent ranking: every row sorted whole (statsref's way).
// ---------------------------------------------------------------------------
struct Ref {
  std::map<uint64_t, sf::Track> rows;
  std::map<std::pair<uint16_t, uint64_t>, uint32_t> rcnt;  // (day, hash): plays
  std::map<uint16_t, std::array<uint8_t, 16>> days;        // DAYS' rows by day, ⊕'d as a save's merge does
};

void addRow(Ref& ref, const sf::Track& t) {
  auto it = ref.rows.find(t.hash);
  if (it == ref.rows.end()) {
    ref.rows[t.hash] = t;
  } else {
    it->second = sf::merge(it->second, t);
  }
}

void addDays(Ref& ref, const uint8_t* row) {
  auto it = ref.days.find(cc::get16(row));
  if (it == ref.days.end()) {
    std::array<uint8_t, 16>& a = ref.days[cc::get16(row)];
    std::memcpy(a.data(), row, 16);
  } else {
    sf::addRow(sf::kDays, it->second.data(), row);
  }
}

void refFrom(const Bytes& bin, const Bytes& base, Ref* ref) {
  if (!base.empty()) {
    const Bytes trks = sectionOf(base, sx::kBaseSpec, 0);
    for (size_t o = 0; o < trks.size(); o += sf::kTrackBytes) addRow(*ref, sf::decodeTrack(u8(trks) + o));
  }
  if (bin.empty()) return;
  const Bytes trkd = sectionOf(bin, sx::kStatsSpec, sf::kTrkd);
  for (size_t o = 0; o < trkd.size(); o += sf::kTrackBytes) addRow(*ref, sf::decodeTrack(u8(trkd) + o));
  const Bytes rc = sectionOf(bin, sx::kStatsSpec, sf::kRcnt);
  for (size_t o = 0; o < rc.size(); o += 12)
    ref->rcnt[{cc::get16(u8(rc) + o), cc::get64(u8(rc) + o + 4)}] += cc::get16(u8(rc) + o + 2);
  const Bytes dy = sectionOf(bin, sx::kStatsSpec, sf::kDays);
  for (size_t o = 0; o < dy.size(); o += 16) addDays(*ref, u8(dy) + o);
}

// The report's DAYS and RCNT fields as the reference has them: the 7 days,
// oldest first (each field the ⊕'d row's), the first and newest DAYS day,
// RCNT's oldest, and (S3b) the 30-day window's first day with a play.
void sameDaysAsRef(const sr::Report& rep, const Ref& ref, int today, const std::string& what) {
  sr::Day want[sr::kWeekDays];
  uint16_t first = sf::kUnsetDay, newest = sf::kUnsetDay, rcntFirst = sf::kUnsetDay, first30 = sf::kUnsetDay;
  for (const auto& kv : ref.days) {
    const int day = kv.first;
    const uint8_t* row = kv.second.data();
    if (first == sf::kUnsetDay || day < first) first = static_cast<uint16_t>(day);
    if (newest == sf::kUnsetDay || day > newest) newest = static_cast<uint16_t>(day);
    if (today >= 0 && day <= today && today - day < 30 && cc::get16(row + 2) > 0 &&
        (first30 == sf::kUnsetDay || day < first30))
      first30 = static_cast<uint16_t>(day);
    if (today < 0 || day > today || today - day >= static_cast<int>(sr::kWeekDays)) continue;
    sr::Day& d = want[sr::kWeekDays - 1 - (today - day)];
    d.listenedMs = cc::get32(row + 8);
    d.plays = cc::get16(row + 2);
    d.autoDjPlays = row[6];
    d.autoDjSkips = row[7];
  }
  for (const auto& kv : ref.rcnt)
    if (rcntFirst == sf::kUnsetDay || kv.first.first < rcntFirst) rcntFirst = kv.first.first;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    const std::string m = what + " day " + std::to_string(i);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].listenedMs, rep.days[i].listenedMs, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].plays, rep.days[i].plays, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].autoDjPlays, rep.days[i].autoDjPlays, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[i].autoDjSkips, rep.days[i].autoDjSkips, m.c_str());
  }
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(first, rep.firstDay, (what + " firstDay").c_str());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(newest, rep.newestDay, (what + " newestDay").c_str());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(rcntFirst, rep.rcntFirstDay, (what + " rcntFirstDay").c_str());
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(first30, rep.days30FirstDay, (what + " days30FirstDay").c_str());
}

// A list by the reference's rule, best first; `capped`: cut to its cap and
// 16 (the Ranker's rows), else every row its rule takes (its total). With
// no clock, Recently played's rows whose lastOrd is `liveRound` (0: none)
// are this session's, first.
std::vector<sr::Entry> refList(const Ref& ref, sr::List l, int today, uint32_t liveRound, bool capped = true) {
  std::vector<sr::Entry> v;
  if (today < 0 && l != sr::kMostAll && l != sr::kRecent) return v;
  const int first30 = today - 29, first7 = today - 6;
  if (l == sr::kMost7 || l == sr::kMost30) {
    const int from = l == sr::kMost7 ? first7 : first30;
    std::map<uint64_t, sr::Entry> by;
    for (const auto& kv : ref.rcnt) {
      const int d = kv.first.first;
      if (d < from || d > today) continue;
      sr::Entry& e = by[kv.first.second];
      e.hash = kv.first.second;
      e.count += kv.second;
      e.at = std::max<uint32_t>(e.at, static_cast<uint32_t>(d));
    }
    for (const auto& kv : by) v.push_back(kv.second);
  } else {
    for (const auto& kv : ref.rows) {
      const sf::Track& t = kv.second;
      sr::Entry e;
      e.hash = t.hash;
      e.count = t.plays;
      if (l == sr::kMostAll || l == sr::kRecent) {
        if (t.plays < 1) continue;
        e.at = t.lastAt;
        e.ord = l == sr::kRecent ? t.lastOrd : 0;
        e.session = l == sr::kRecent && today < 0 && liveRound != 0 && t.lastOrd == liveRound;
      } else if (l == sr::kNewFinds) {
        if (t.firstAt == 0 || t.firstAt == sf::kNoFirst) continue;
        const int d = sf::dayOfTime(t.firstAt);
        if (d < first30 || d > today) continue;
        e.at = t.firstAt;
        e.src = t.src;
      } else {
        if (t.plays < sr::kOldFavourite || t.lastAt == 0 || static_cast<int>(sf::dayOfTime(t.lastAt)) >= first30)
          continue;
        e.at = t.lastAt;
      }
      v.push_back(e);
    }
  }
  std::sort(v.begin(), v.end(), [l](const sr::Entry& a, const sr::Entry& b) { return sr::before(l, a, b); });
  if (capped && v.size() > sr::rowsOf(l)) v.resize(sr::rowsOf(l));
  return v;
}

// The lists and their totals as the reference ranks them.
void sameAsRef(const sr::Ranker& r, const Ref& ref, int today, uint32_t liveRound, const std::string& what) {
  for (int l = 0; l < sr::kLists; ++l) {
    const sr::List li = static_cast<sr::List>(l);
    const std::vector<sr::Entry> want = refList(ref, li, today, liveRound);
    const std::string m = what + " " + sr::listName(li);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(refList(ref, li, today, liveRound, false).size(), r.total(li),
                                     (m + " total").c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), r.size(li), m.c_str());
    for (uint32_t i = 0; i < r.size(li); ++i) {
      const sr::Entry& a = r.at(li, i);
      const sr::Entry& b = want[i];
      const std::string mi = m + " row " + std::to_string(i);
      TEST_ASSERT_TRUE_MESSAGE(a.hash == b.hash, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.count, a.count, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.at, a.at, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.ord, a.ord, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b.src, a.src, mi.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(b.session, a.session, mi.c_str());
    }
  }
}

// rank.json's scenario `name`.
const minijson::Value& rankScenario(const char* name) {
  for (size_t i = 0; i < gRank.size(); ++i)
    if (gRank[i]["name"].str() == name) return gRank[i];
  TEST_FAIL_MESSAGE(name);
  return gRank[0];
}

struct RankCard {
  Bytes bin, base;
  std::string baseName;
};

RankCard rankCard(const minijson::Value& sc) {
  RankCard c;
  const std::string dir = "stats/" + sc["in"].str() + "/";
  c.bin = cardfixtures::bytes(dir + "stats.bin");
  TEST_ASSERT_TRUE(!c.bin.empty());
  const Bytes head = sectionOf(c.bin, sx::kStatsSpec, sf::kHead);
  const sf::Head h = sf::decodeHead(u8(head));
  if (h.baseGen) {
    char n[sx::kNameCap];
    TEST_ASSERT_TRUE(sx::baseName(h.baseGen, n, sizeof(n)));
    c.baseName = std::string(n).substr(kDir.size());
    c.base = cardfixtures::bytes(dir + c.baseName);
    TEST_ASSERT_TRUE(!c.base.empty());
  }
  return c;
}

void putRankCard(cutfs::CutFs& card, const RankCard& c) {
  put(card, "stats.bin", c.bin);
  if (!c.base.empty()) put(card, c.baseName, c.base);
}

// ---------------------------------------------------------------------------
// Every card: each fixture boot that reads back (stats/index.json's `out`)
// and each rank scenario (stats/rank.json), with its day (-1: the clock
// unset) and statsref's entry (`want`: the boot's, or the scenario).
// ---------------------------------------------------------------------------
struct Card {
  std::string what;
  std::vector<std::pair<std::string, Bytes>> files;  // under /.player/
  int today = -1;
  const minijson::Value* want = nullptr;
  bool scenario = false;
  bool hasLive = false;    // the scenario names its live round (rank.json's `liveRound`)
  uint32_t liveRound = 0;  // ... else the fold's: a session that boots on the card
  const Bytes* file(const std::string& name) const {
    for (const auto& f : files)
      if (f.first == name) return &f.second;
    return nullptr;
  }
};
std::vector<Card> gCards;

void loadCards() {
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    const minijson::Value& bootsV = sc["boots"];
    for (size_t b = 0; b < bootsV.size(); ++b) {
      const minijson::Value& e = bootsV[b];
      if (!e.has("lists")) continue;
      Card c;
      c.what = sc["name"].str() + " boot " + std::to_string(b + 1);
      for (const auto& m : e["out"].members)
        c.files.emplace_back(m.first, cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first));
      c.today = todayOf(e["today"]);
      c.want = &e;
      gCards.push_back(c);
    }
  }
  for (size_t i = 0; i < gRank.size(); ++i) {
    const minijson::Value& sc = gRank[i];
    const RankCard rc = rankCard(sc);
    Card c;
    c.what = sc["name"].str();
    c.files.emplace_back("stats.bin", rc.bin);
    if (!rc.base.empty()) c.files.emplace_back(rc.baseName, rc.base);
    c.today = todayOf(sc["today"]);
    c.want = &sc;
    c.scenario = true;
    c.hasLive = sc.has("liveRound");
    c.liveRound = c.hasLive ? static_cast<uint32_t>(sc["liveRound"].u64()) : 0;
    gCards.push_back(c);
  }
}

// A card on a CutFs, read as the boot reads it, for a rank on `today` by
// its session (the scenario's live round, else the fold's).
struct Run {
  cutfs::CutFs card;
  Session s;
  Run(const Card& c, int today) {
    for (const auto& f : c.files) put(card, f.first, f.second);
    readCard(card, s, today);
    if (c.hasLive) s.in.liveRound = c.liveRound;
  }
};

// The independent ranking of a card as the session stands on it (the base
// HEAD names, as read).
Ref refOf(const Card& c, const sr::Input& in) {
  Ref ref;
  const Bytes* bin = c.file("stats.bin");
  Bytes base;
  if (in.baseGen) {
    char n[sx::kNameCap];
    TEST_ASSERT_TRUE(sx::baseName(in.baseGen, n, sizeof(n)));
    const Bytes* b = c.file(std::string(n).substr(kDir.size()));
    TEST_ASSERT_NOT_NULL_MESSAGE(b, c.what.c_str());
    base = *b;
  }
  refFrom(bin ? *bin : Bytes(), base, &ref);
  return ref;
}

// The played hashes, ascending (the u64 set's).
std::vector<uint64_t> playedOf(const Ref& ref) {
  std::vector<uint64_t> v;
  for (const auto& kv : ref.rows)
    if (kv.second.plays) v.push_back(kv.first);
  return v;
}

// RCNT's rows in the 30-day window (none undated): the windows' rows.
uint32_t windowRowsOf(const Ref& ref, int today) {
  uint32_t n = 0;
  for (const auto& kv : ref.rcnt)
    if (today >= 0 && kv.first.first <= today && kv.first.first + 29 >= today) ++n;
  return n;
}

// Two Rankers' lists and totals the same, row by row, every field.
void sameRanker(const sr::Ranker& a, const sr::Ranker& b, const std::string& what) {
  for (int l = 0; l < sr::kLists; ++l) {
    const sr::List li = static_cast<sr::List>(l);
    const std::string m = what + " " + sr::listName(li);
    TEST_ASSERT_EQUAL_MESSAGE(a.offered(li), b.offered(li), m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.total(li), b.total(li), (m + " total").c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.size(li), b.size(li), m.c_str());
    for (uint32_t i = 0; i < a.size(li); ++i) {
      const sr::Entry& x = a.at(li, i);
      const sr::Entry& y = b.at(li, i);
      const std::string mi = m + " row " + std::to_string(i);
      TEST_ASSERT_TRUE_MESSAGE(x.hash == y.hash, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(x.count, y.count, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(x.at, y.at, mi.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(x.ord, y.ord, mi.c_str());
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(x.src, y.src, mi.c_str());
      TEST_ASSERT_EQUAL_MESSAGE(x.session, y.session, mi.c_str());
    }
  }
}

// A section's rows damaged (one byte of its first row): its CRC fails.
Bytes damaged(const Bytes& bin, sf::Sec sec) {
  Bytes out = bin;
  cc::MemSource src(out.data(), static_cast<uint32_t>(out.size()));
  cc::Container fr;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(fr.open(src, sx::kStatsSpec, nullptr)));
  TEST_ASSERT_TRUE(fr.section(sec).count > 0);
  out[fr.section(sec).offset + 1] ^= 0x40;
  return out;
}

// A live play of a made-up device's (locally administered), started on
// UTC day `day` at `msInDay`.
pl::Slot livePlay(uint32_t seq, uint64_t hash, int day, uint32_t msInDay, uint32_t played, uint32_t duration,
                  uint8_t outcome, uint8_t source) {
  static const uint8_t kDev[6] = {0x02, 0x00, 0x5e, 0x00, 0x53, 0x0b};
  pl::Slot s;
  s.kind = pl::kKindPlay;
  s.outcome = outcome;
  s.source = source;
  s.clock = pl::kClockRtc;
  s.seq = seq;
  s.epoch = 0x0c0ffee0d15ea5e5ull;
  std::memcpy(s.deviceId, kDev, 6);
  s.startedAt = wallclock::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs + msInDay;
  s.bootSeq = 1;
  s.uptimeMs = seq * 1000;
  s.playedMs = played;
  s.durationMs = duration;
  s.pathHash = hash;
  return s;
}

// The records' rows as the hand-off copies them (Fold::Cursor's, each
// section's in its key order).
struct Records {
  Bytes trkd, rcnt, days;
  uint32_t trkdRows = 0, rcntRows = 0, daysRows = 0;
  explicit Records(sf::Fold& fold) {
    copy(fold, sf::kTrkd, &trkd, &trkdRows);
    copy(fold, sf::kRcnt, &rcnt, &rcntRows);
    copy(fold, sf::kDays, &days, &daysRows);
  }
  static void copy(sf::Fold& fold, sf::Sec sec, Bytes* out, uint32_t* rows) {
    sf::Fold::Cursor cur(fold, sec);
    uint8_t row[sf::kTrackBytes];
    while (cur.next(row)) {
      out->append(reinterpret_cast<const char*>(row), sf::kStrides[sec]);
      ++*rows;
    }
  }
  void into(sr::Input* in) const {
    in->trkd = u8(trkd);
    in->trkdRows = trkdRows;
    in->rcnt = u8(rcnt);
    in->rcntRows = rcntRows;
    in->days = u8(days);
    in->daysRows = daysRows;
  }
  void addTo(Ref& ref) const {
    for (uint32_t i = 0; i < trkdRows; ++i) addRow(ref, sf::decodeTrack(u8(trkd) + i * sf::kTrackBytes));
    for (uint32_t i = 0; i < rcntRows; ++i) {
      const uint8_t* r = u8(rcnt) + i * 12;
      ref.rcnt[{cc::get16(r), cc::get64(r + 4)}] += cc::get16(r + 2);
    }
    for (uint32_t i = 0; i < daysRows; ++i) addDays(ref, u8(days) + i * 16);
  }
};

}  // namespace

void setUp() {}
void tearDown() {}

// Every fixture boot that reads back: the lists statsref gives over the
// card the boot leaves, after V1's order swap; and against the independent
// ranking, S2's report fields and u64 played set unchanged, the totals, the
// 7 days and the span.
void test_every_fixture_boot() {
  uint32_t boots = 0;
  for (const Card& c : gCards) {
    if (c.scenario) continue;
    const std::string& what = c.what;
    Run run(c, c.today);
    Mem mem(4096 + 64, 4096, run.s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(run.card, run.s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
    sameLists(r, (*c.want)["lists"], what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, run.card.openNow, what.c_str());
    const Ref ref = refOf(c, run.s.in);
    sameAsRef(r, ref, c.today, run.s.in.liveRound, what);
    sameDaysAsRef(rep, ref, c.today, what);
    // S2's fields: what it read and found.
    const Bytes* bin = c.file("stats.bin");
    TEST_ASSERT_NOT_NULL(bin);
    const std::vector<uint64_t> played = playedOf(ref);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(run.s.in.baseRows, rep.baseRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sectionOf(*bin, sx::kStatsSpec, sf::kTrkd).size() / sf::kTrackBytes, rep.trkdRows,
                                     what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.rcnt.size(), rep.rcntRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.days.size(), rep.daysRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rep.recordRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.rows.size(), rep.tracks, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(played.size(), rep.played, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(windowRowsOf(ref, c.today), rep.windowRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(played.size(), mem.played.n, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mem.played.lost, what.c_str());
    for (uint32_t k = 0; k < mem.played.n; ++k) TEST_ASSERT_TRUE_MESSAGE(played[k] == mem.played.at(k), what.c_str());
    ++boots;
  }
  TEST_ASSERT_TRUE(boots >= 25);
  printf("[rank] %u fixture boots ranked as statsref ranks them\n", static_cast<unsigned>(boots));
}

// The rankers' scenarios: each list past its cap and 16; since V1 their
// totals, the 7 days and the span, and the u40 set as statsref gives them.
void test_the_rank_scenarios() {
  TEST_ASSERT_TRUE(gRank.size() >= 8);
  for (size_t i = 0; i < gRank.size(); ++i) {
    const minijson::Value& sc = gRank[i];
    const std::string what = sc["name"].str();
    cutfs::CutFs card;
    const RankCard c = rankCard(sc);
    putRankCard(card, c);
    Session s;
    const int today = todayOf(sc["today"]);
    readCard(card, s, today);
    liveOf(sc, s);
    const minijson::Value& rows = sc["rows"];
    Mem mem(static_cast<uint32_t>(rows["rcnt"].u64()), static_cast<uint32_t>(rows["played"].u64()), s.in.baseRows);
    sr::Ranker r;
    card.bytesRead = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
    sameLists(r, sc["lists"], what);
    sameTotals(r, sc["totals"], what);
    sameDays7(rep, sc["days7"], what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["base"].u64(), rep.baseRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["trkd"].u64(), rep.trkdRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["rcnt"].u64(), rep.rcntRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["days"].u64(), rep.daysRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["tracks"].u64(), rep.tracks, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["window"].u64(), rep.windowRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["played"].u64(), rep.played, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["played"].u64(), mem.played.n, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mem.played.lost, what.c_str());
    // The u40 set: statsref's bytes; the u64 set, given too, not touched
    // (one set or the other).
    Mem mem40(static_cast<uint32_t>(rows["rcnt"].u64()), static_cast<uint32_t>(rows["played"].u64()),
              s.in.baseRows);
    mem40.use40(static_cast<uint32_t>(rows["played"].u64()));
    mem40.played.n = 7;
    mem40.played.lost = 9;
    sr::Ranker r40;
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(sr::rank(card, s.in, mem40.m, r40).outcome), what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["played"].u64(), mem40.played40.n, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mem40.played40.lost, what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(mem40.played.n == 7 && mem40.played.lost == 9, (what + ": the u40 set alone").c_str());
    TEST_ASSERT_EQUAL_STRING_MESSAGE(sc["played40"].c_str(), hexOf(mem40.bytes40()).c_str(), what.c_str());
    sameRanker(r, r40, what + " u40");
    // Every caps' list full where statsref's is (rank-big: each past it).
    if (what == "rank-big") {
      for (int l = 0; l < sr::kLists; ++l)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sr::rowsOf(static_cast<sr::List>(l)), r.size(static_cast<sr::List>(l)),
                                         sr::listName(static_cast<sr::List>(l)));
    }
    printf("[rank] %s: %u tracks (%u base rows, %u TRKD, %u RCNT, %u in the 30 days), %u played, %llu B read, "
           "%lld us on the host\n",
           what.c_str(), static_cast<unsigned>(rep.tracks), static_cast<unsigned>(rep.baseRows),
           static_cast<unsigned>(rep.trkdRows), static_cast<unsigned>(rep.rcntRows),
           static_cast<unsigned>(rep.windowRows), static_cast<unsigned>(rep.played),
           static_cast<unsigned long long>(card.bytesRead), static_cast<long long>(us));
  }
}

// The independent ranking gives statsref's lists, and with the records
// (random rows over base and TRKD hashes, and new ones; RCNT rows in and
// out of the windows) merged in, the rank's lists are that ranking's.
void test_the_records_merged_in() {
  const minijson::Value& sc = rankScenario("rank-big");
  const RankCard c = rankCard(sc);
  const int today = todayOf(sc["today"]);
  Ref plain;
  refFrom(c.bin, c.base, &plain);
  {
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, today);
    Mem mem(4096, 4096, s.in.baseRows);
    sr::Ranker r;
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, mem.m, r).outcome));
    sameAsRef(r, plain, today, s.in.liveRound, "no records");
    sameLists(r, sc["lists"], "no records");
  }
  std::mt19937_64 rng(7);
  std::vector<uint64_t> hashes;
  for (const auto& kv : plain.rows) hashes.push_back(kv.first);
  for (int round = 0; round < 40; ++round) {
    // The records as Fold::Cursor gives them: TRKD's and RCNT's rows,
    // each sorted, a key's records ⊕'d into one row.
    std::map<uint64_t, sf::Track> recs;
    std::map<std::pair<uint16_t, uint64_t>, uint32_t> recRcnt;
    const int n = 1 + static_cast<int>(rng() % 40);
    for (int k = 0; k < n; ++k) {
      const uint64_t h = rng() % 3 ? hashes[rng() % hashes.size()] : (rng() | 1);
      sf::Track t;
      t.hash = h;
      if (rng() % 5 == 0) {
        t.skips = 1;  // a skip
      } else {
        t.plays = static_cast<uint16_t>(1 + rng() % 3);
        const int day = today - static_cast<int>(rng() % 35) + (rng() % 9 == 0 ? 1 : 0);
        const bool dated = rng() % 6 != 0;
        t.firstAt = t.lastAt = dated ? static_cast<uint32_t>(day) * 86400 + 1 + rng() % 86400 : 0;
        t.lastOrd = static_cast<uint32_t>(900 + rng() % 50);
        t.src = static_cast<uint8_t>(rng() % 3);
        if (dated) recRcnt[{static_cast<uint16_t>(day), h}] += t.plays;
      }
      auto it = recs.find(h);
      if (it == recs.end()) {
        recs[h] = t;
      } else {
        it->second = sf::merge(it->second, t);
      }
    }
    // DAYS' rows (S3a): some days around the 7, each row a few slots' ⊕,
    // now and then a listened time that saturates the card's sum; day
    // 318's card row is at its fields' tops already.
    std::map<uint16_t, std::array<uint8_t, 16>> recDays;
    const int nd = static_cast<int>(rng() % 12);
    for (int k = 0; k < nd; ++k) {
      const int day = today - static_cast<int>(rng() % 10) + (rng() % 9 == 0 ? 1 : 0);
      uint8_t row[16] = {};
      cc::put16(row, static_cast<uint16_t>(day));
      const uint16_t p = static_cast<uint16_t>(rng() % 4), sk = static_cast<uint16_t>(rng() % 3);
      cc::put16(row + 2, p);
      cc::put16(row + 4, sk);
      row[6] = static_cast<uint8_t>(rng() % (p + 1));
      row[7] = static_cast<uint8_t>(rng() % (sk + 1));
      cc::put32(row + 8, rng() % 7 == 0 ? 0xFFFFFF00u : static_cast<uint32_t>(rng() % 900000));
      auto it = recDays.find(static_cast<uint16_t>(day));
      if (it == recDays.end()) {
        std::memcpy(recDays[static_cast<uint16_t>(day)].data(), row, 16);
      } else {
        sf::addRow(sf::kDays, it->second.data(), row);
      }
    }
    Bytes trkd, rcnt, days;
    for (const auto& kv : recs) {
      uint8_t row[sf::kTrackBytes];
      sf::encodeTrack(kv.second, row);
      trkd.append(reinterpret_cast<const char*>(row), sizeof(row));
    }
    for (const auto& kv : recRcnt) {
      uint8_t row[12];
      cc::put16(row, kv.first.first);
      cc::put16(row + 2, static_cast<uint16_t>(kv.second));
      cc::put64(row + 4, kv.first.second);
      rcnt.append(reinterpret_cast<const char*>(row), sizeof(row));
    }
    for (const auto& kv : recDays) days.append(reinterpret_cast<const char*>(kv.second.data()), 16);
    Ref ref = plain;
    for (const auto& kv : recs) addRow(ref, kv.second);
    for (const auto& kv : recRcnt) ref.rcnt[kv.first] += kv.second;
    for (const auto& kv : recDays) addDays(ref, kv.second.data());
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, today);
    s.in.trkd = u8(trkd);
    s.in.trkdRows = static_cast<uint32_t>(recs.size());
    s.in.rcnt = u8(rcnt);
    s.in.rcntRows = static_cast<uint32_t>(recRcnt.size());
    s.in.days = u8(days);
    s.in.daysRows = static_cast<uint32_t>(recDays.size());
    Mem mem(4096 + 40, 4096 + 40, s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    const std::string what = "round " + std::to_string(round);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
    sameAsRef(r, ref, today, s.in.liveRound, what);
    sameDaysAsRef(rep, ref, today, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.rows.size(), rep.tracks, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s.in.trkdRows + s.in.rcntRows + s.in.daysRows, rep.recordRows, what.c_str());
    // The played set: every hash with a play, ascending.
    std::vector<uint64_t> want;
    for (const auto& kv : ref.rows)
      if (kv.second.plays) want.push_back(kv.first);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), mem.played.n, what.c_str());
    for (uint32_t k = 0; k < mem.played.n; ++k) TEST_ASSERT_TRUE_MESSAGE(want[k] == mem.played.at(k), what.c_str());
    // The played-set-only rank over the same records: the records' TRKD
    // alone read, the same set in u40.
    Mem lean(0, 0, s.in.baseRows);
    lean.lean();
    lean.use40(4096 + 40);
    sr::Ranker unused;
    const sr::Report lr = sr::rank(card, s.in, lean.m, unused);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(lr.outcome), what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s.in.trkdRows, lr.recordRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), lean.played40.n, what.c_str());
    for (uint32_t k = 0; k < lean.played40.n; ++k)
      TEST_ASSERT_TRUE_MESSAGE(want[k] >> 24 == lean.played40.at(k), what.c_str());
  }
  // The records from the session's own fold (Fold::Cursor's rows, as the
  // hand-off copies them): plays across UTC midnights count whole on the
  // day they began (4.1); an Auto DJ play into day 318's row at its tops
  // leaves it there; an older dated play moves DAYS' and RCNT's first day,
  // one a day ahead DAYS' newest.
  {
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, today);
    const uint64_t fresh = 0x5eed000000000001ull;
    TEST_ASSERT_TRUE(s.fold.liveSlot(livePlay(1, hashes[0], today - 1, sf::kDayMs - 600000, 1200000, 1500000,
                                              pl::kCompleted, pl::kSourceAutoDj)));  // 23:50, 20 min
    TEST_ASSERT_TRUE(s.fold.liveSlot(livePlay(2, hashes[1], today - 1, sf::kDayMs - 1, 10000, 200000,
                                              pl::kSkipped, pl::kSourceAutoDj)));  // 23:59:59.999, a skip
    TEST_ASSERT_TRUE(s.fold.liveSlot(
        livePlay(3, hashes[2], today, 0, 200000, 200000, pl::kCompleted, pl::kSourceShuffle)));  // 00:00
    TEST_ASSERT_TRUE(s.fold.liveSlot(livePlay(4, hashes[3], today, sf::kDayMs - 1000, 240000, 240000,
                                              pl::kCompleted, pl::kSourceManual)));  // 23:59:59, into tomorrow
    TEST_ASSERT_TRUE(s.fold.liveSlot(livePlay(5, hashes[4], today - 2, 43200000, 100000, 100000, pl::kCompleted,
                                              pl::kSourceAutoDj)));  // into day 318's tops
    TEST_ASSERT_TRUE(s.fold.liveSlot(
        livePlay(6, fresh, today - 200, 36000000, 200000, 200000, pl::kCompleted, pl::kSourceManual)));
    TEST_ASSERT_TRUE(s.fold.liveSlot(
        livePlay(7, hashes[5], today + 1, 600000, 200000, 200000, pl::kCompleted, pl::kSourceManual)));
    const Records recs(s.fold);
    recs.into(&s.in);
    Ref ref = plain;
    recs.addTo(ref);
    Mem mem(4096 + 40, 4096 + 40, s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep.outcome));
    sameAsRef(r, ref, today, s.in.liveRound, "the session's fold");
    sameDaysAsRef(rep, ref, today, "the session's fold");
    // The same by hand, from the card's 7 days (statsref's).
    const minijson::Value& card7 = sc["days7"]["days"];
    auto cardDay = [&](int i, int field) { return static_cast<uint32_t>(card7[static_cast<size_t>(i)][field].u64()); };
    TEST_ASSERT_EQUAL_UINT32(cardDay(5, 0) + 1210000, rep.days[5].listenedMs);  // day 319: 23:50's and the skip
    TEST_ASSERT_EQUAL_UINT32(cardDay(5, 1) + 1, rep.days[5].plays);
    TEST_ASSERT_EQUAL_UINT32(cardDay(5, 2) + 1, rep.days[5].autoDjPlays);
    TEST_ASSERT_EQUAL_UINT32(cardDay(5, 3) + 1, rep.days[5].autoDjSkips);
    TEST_ASSERT_EQUAL_UINT32(cardDay(6, 0) + 440000, rep.days[6].listenedMs);  // day 320: 00:00's and 23:59:59's
    TEST_ASSERT_EQUAL_UINT32(cardDay(6, 1) + 2, rep.days[6].plays);
    TEST_ASSERT_EQUAL_UINT32(cardDay(6, 2), rep.days[6].autoDjPlays);
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, rep.days[4].listenedMs);  // day 318: at its tops still
    TEST_ASSERT_EQUAL_UINT32(0xFFFF, rep.days[4].plays);
    TEST_ASSERT_EQUAL_UINT32(0xFF, rep.days[4].autoDjPlays);
    TEST_ASSERT_EQUAL_UINT16(today - 200, rep.firstDay);
    TEST_ASSERT_EQUAL_UINT16(today + 1, rep.newestDay);
    TEST_ASSERT_EQUAL_UINT16(today - 200, rep.rcntFirstDay);
  }
  // With no stats.bin (a fresh start: the session stands on none), the
  // records alone; on day 3 too, whose 7 days begin before day 0.
  for (const int day : {today, 3}) {
    Ref ref;
    std::map<uint64_t, sf::Track> recs;
    Bytes trkd;
    for (int k = 0; k < 40; ++k) {
      sf::Track t;
      t.hash = rng() | 1;
      t.plays = 1;
      t.firstAt = t.lastAt = static_cast<uint32_t>(day) * 86400 + 1 + static_cast<uint32_t>(k);
      t.lastOrd = 2;
      recs[t.hash] = t;
      addRow(ref, t);
    }
    for (const auto& kv : recs) {
      uint8_t row[sf::kTrackBytes];
      sf::encodeTrack(kv.second, row);
      trkd.append(reinterpret_cast<const char*>(row), sizeof(row));
    }
    Bytes days;
    for (int d = day - 2; d <= day; ++d) {
      uint8_t row[16] = {};
      cc::put16(row, static_cast<uint16_t>(d));
      cc::put16(row + 2, static_cast<uint16_t>(d + 1));
      row[6] = 1;
      cc::put32(row + 8, 1000u * static_cast<uint32_t>(d + 1));
      days.append(reinterpret_cast<const char*>(row), 16);
      addDays(ref, row);
    }
    cutfs::CutFs card;
    sr::Input in;
    in.trkd = u8(trkd);
    in.trkdRows = 40;
    in.days = u8(days);
    in.daysRows = 3;
    in.hasToday = true;
    in.today = static_cast<uint16_t>(day);
    Mem mem(64, 64, 0);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep.outcome));
    sameAsRef(r, ref, day, in.liveRound, "records alone");
    sameDaysAsRef(rep, ref, day, "records alone");
    TEST_ASSERT_EQUAL_UINT32(40, mem.played.n);
    TEST_ASSERT_EQUAL_UINT32(1000u * static_cast<uint32_t>(day + 1), rep.days[6].listenedMs);
    TEST_ASSERT_EQUAL_UINT16(day - 2, rep.firstDay);
    TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, rep.rcntFirstDay);
  }
}

namespace {

// A live play of `hash` with the clock unset (an undated slot: 5.9); a
// skip (pl::kSkipped) 10 s of it, under the line.
pl::Slot undatedPlay(uint32_t seq, uint64_t hash, uint8_t outcome = pl::kCompleted) {
  pl::Slot s = livePlay(seq, hash, 0, 0, outcome == pl::kSkipped ? 10000 : 200000, 200000, outcome,
                        pl::kSourceManual);
  s.startedAt = 0;
  s.clock = pl::kClockUnset;
  return s;
}

}  // namespace

// The owner's call (2026-11-24, "Yes, fix the order"): with no clock,
// Recently played puts this session's plays first (4.3: "This session",
// then "Earlier"): every row whose lastOrd is the ask's live round
// (Fold::liveRound()), whatever its lastAt (an undated play leaves lastAt
// and sets lastOrd, 5.9). Before it, 4.3's order put the dated rows first,
// so on a card of 216 dated tracks or more this session's never-dated plays
// fell past the rows kept ("This session" empty) and a track played again
// sat among the dated rows by its old lastAt.
//   - rank-no-clock-session (statsref's): the card as the session's live
//     save leaves it (HEAD's round its live round, 902), 363 dated tracks
//     of earlier rounds, ranked by that session: its six rows first (two
//     dated, played again; four never dated), statsref's lists.
//   - The same card under the next session (it loads 902: live round 904),
//     its own fold's undated plays as the hand-off copies them (the
//     records): two tracks never played, one played before but never dated,
//     the oldest dated one, all first; the last session's rows Earlier now;
//     a skip moves no row. With the clock set the same rows rank by 4.3's
//     order as before, the never-dated plays past the 216 dated ones.
void test_this_session_first_with_no_clock() {
  const minijson::Value& sc = rankScenario("rank-no-clock-session");
  TEST_ASSERT_TRUE(sc["today"].isNull());
  const RankCard c = rankCard(sc);
  Ref plain;
  refFrom(c.bin, c.base, &plain);
  const uint32_t saved = static_cast<uint32_t>(sc["liveRound"].u64());
  // The scenario's session: its rows, those of earlier rounds.
  std::vector<sr::Entry> mine;
  uint32_t dated = 0;
  for (const auto& kv : plain.rows) {
    const sf::Track& t = kv.second;
    TEST_ASSERT_TRUE(t.lastOrd <= saved);  // (HEAD's round: every row's at most)
    if (t.plays < 1) continue;
    if (t.lastOrd == saved) {
      sr::Entry e;
      e.hash = t.hash;
      e.at = t.lastAt;
      mine.push_back(e);
    } else if (t.lastAt != 0) {
      ++dated;
    }
  }
  TEST_ASSERT_TRUE(dated >= sr::rowsOf(sr::kRecent));
  TEST_ASSERT_EQUAL_UINT32(6, mine.size());
  uint32_t mineDated = 0;
  for (const sr::Entry& e : mine) mineDated += e.at != 0 ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT32(2, mineDated);
  // Within this session 4.3's order as before: the dated lastAt, newest
  // first, then the never-dated ones by the hash (one round).
  std::sort(mine.begin(), mine.end(), [](const sr::Entry& a, const sr::Entry& b) {
    return (a.at != 0) != (b.at != 0) ? a.at != 0 : a.at != b.at ? a.at > b.at : a.hash < b.hash;
  });
  {
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, -1);
    TEST_ASSERT_EQUAL_UINT32(saved + 2, s.in.liveRound);  // (read fresh: the next session's)
    liveOf(sc, s);
    TEST_ASSERT_EQUAL_UINT32(saved, s.in.liveRound);
    Mem mem(4096, 4096, s.in.baseRows);
    sr::Ranker r;
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, mem.m, r).outcome));
    sameLists(r, sc["lists"], "the saving session");
    sameAsRef(r, plain, -1, saved, "the saving session");
    TEST_ASSERT_EQUAL_UINT32(sr::rowsOf(sr::kRecent), r.size(sr::kRecent));
    for (uint32_t i = 0; i < mine.size(); ++i) {
      const std::string m = "this session's row " + std::to_string(i);
      TEST_ASSERT_TRUE_MESSAGE(r.at(sr::kRecent, i).hash == mine[i].hash, m.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.at(sr::kRecent, i).session, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(saved, r.at(sr::kRecent, i).ord, m.c_str());
    }
    for (uint32_t i = static_cast<uint32_t>(mine.size()); i < r.size(sr::kRecent); ++i) {
      TEST_ASSERT_FALSE(r.at(sr::kRecent, i).session);
      TEST_ASSERT_TRUE(r.at(sr::kRecent, i).ord != saved);
    }
    // No other list marks a row; All time as before.
    for (uint32_t i = 0; i < r.size(sr::kMostAll); ++i) TEST_ASSERT_FALSE(r.at(sr::kMostAll, i).session);
    // Before the call (no row this session's): none of its never-dated
    // plays among the rows kept.
    const std::vector<sr::Entry> before = refList(plain, sr::kRecent, -1, 0);
    uint32_t keptBefore = 0;
    for (const sr::Entry& e : before) keptBefore += e.ord == saved && e.at == 0 ? 1 : 0;
    TEST_ASSERT_EQUAL_UINT32(0, keptBefore);
  }

  // The next session: the card read fresh, its own fold's undated plays.
  uint64_t oldDated = 0, neverDated = 0, skipped = 0;
  uint32_t oldestAt = UINT32_MAX;
  for (const auto& kv : plain.rows) {
    const sf::Track& t = kv.second;
    if (t.plays < 1 || t.lastOrd == saved) continue;
    if (t.lastAt != 0 && t.lastAt < oldestAt) {
      oldestAt = t.lastAt;
      oldDated = kv.first;
    } else if (t.lastAt == 0 && !neverDated) {
      neverDated = kv.first;
    }
  }
  for (const auto& kv : plain.rows)
    if (kv.second.plays >= 1 && kv.second.lastAt != 0 && kv.second.lastOrd != saved && kv.first != oldDated) {
      skipped = kv.first;  // a dated row only skipped this session: not this session's
      break;
    }
  TEST_ASSERT_TRUE(oldDated && neverDated && skipped);
  const uint64_t fresh1 = 0x5e55000000000011ull, fresh2 = 0x5e55000000000003ull;
  TEST_ASSERT_TRUE(plain.rows.count(fresh1) == 0 && plain.rows.count(fresh2) == 0);
  cutfs::CutFs card;
  putRankCard(card, c);
  Session s;
  readCard(card, s, -1);
  const uint32_t live = s.fold.liveRound();
  TEST_ASSERT_EQUAL_UINT32(saved + 2, live);
  TEST_ASSERT_EQUAL_UINT32(live, s.in.liveRound);
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(1, fresh1)));
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(2, oldDated)));
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(3, neverDated)));
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(4, skipped, pl::kSkipped)));
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(5, fresh2)));
  TEST_ASSERT_TRUE(s.fold.liveSlot(undatedPlay(6, fresh1)));  // (twice: one row)
  const Records recs(s.fold);
  recs.into(&s.in);
  Ref ref = plain;
  recs.addTo(ref);
  // The rows as the session holds them: the four played have lastOrd the
  // live round, the old one its dated lastAt still; the skip moved nothing.
  TEST_ASSERT_EQUAL_UINT32(live, ref.rows[oldDated].lastOrd);
  TEST_ASSERT_EQUAL_UINT32(oldestAt, ref.rows[oldDated].lastAt);
  TEST_ASSERT_EQUAL_UINT32(live, ref.rows[neverDated].lastOrd);
  TEST_ASSERT_EQUAL_UINT32(0, ref.rows[neverDated].lastAt);
  TEST_ASSERT_TRUE(ref.rows[skipped].lastOrd != live);
  uint32_t sessionRows = 0;
  for (const auto& kv : ref.rows) sessionRows += kv.second.plays >= 1 && kv.second.lastOrd == live ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT32(4, sessionRows);
  Mem mem(4096 + 40, 4096 + 40, s.in.baseRows);
  sr::Ranker r;
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, mem.m, r).outcome));
  sameAsRef(r, ref, -1, live, "the next session");
  TEST_ASSERT_EQUAL_UINT32(sr::rowsOf(sr::kRecent), r.size(sr::kRecent));
  std::vector<uint64_t> undated = {fresh1, fresh2, neverDated};
  std::sort(undated.begin(), undated.end());
  const uint64_t want[4] = {oldDated, undated[0], undated[1], undated[2]};
  for (uint32_t i = 0; i < 4; ++i) {
    const std::string m = "the next session's row " + std::to_string(i);
    TEST_ASSERT_TRUE_MESSAGE(r.at(sr::kRecent, i).hash == want[i], m.c_str());
    TEST_ASSERT_TRUE_MESSAGE(r.at(sr::kRecent, i).session, m.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(live, r.at(sr::kRecent, i).ord, m.c_str());
  }
  // Then Earlier, in 4.3's order: the last session's dated rows among the
  // dated, by lastAt; no row this session's.
  for (uint32_t i = 4; i < r.size(sr::kRecent); ++i) {
    TEST_ASSERT_FALSE(r.at(sr::kRecent, i).session);
    TEST_ASSERT_TRUE(r.at(sr::kRecent, i).at != 0);
    if (i > 4) TEST_ASSERT_TRUE(r.at(sr::kRecent, i).at <= r.at(sr::kRecent, i - 1).at);
  }
  // With the clock set, the same card and records: 4.3's order as before,
  // no row marked, the never-dated plays past the 216 dated ones.
  {
    cutfs::CutFs dcard;
    putRankCard(dcard, c);
    Session d;
    readCard(dcard, d, static_cast<int>(rankScenario("rank-big")["today"].u64()));
    recs.into(&d.in);
    Mem dm(4096 + 40, 4096 + 40, d.in.baseRows);
    sr::Ranker dr;
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(dcard, d.in, dm.m, dr).outcome));
    sameAsRef(dr, ref, static_cast<int>(d.in.today), d.in.liveRound, "the next session, dated");
    for (uint32_t i = 0; i < dr.size(sr::kRecent); ++i) {
      TEST_ASSERT_FALSE(dr.at(sr::kRecent, i).session);
      TEST_ASSERT_TRUE(dr.at(sr::kRecent, i).at != 0);
      TEST_ASSERT_TRUE(dr.at(sr::kRecent, i).hash != fresh1 && dr.at(sr::kRecent, i).hash != neverDated);
    }
  }
}

// The sparse index: each 64th base row; span() gives a range that holds
// every base row's hash, and none for a hash below the first.
void test_the_sparse_index() {
  const minijson::Value& sc = rankScenario("rank-big");
  const RankCard c = rankCard(sc);
  cutfs::CutFs card;
  putRankCard(card, c);
  Session s;
  readCard(card, s, todayOf(sc["today"]));
  Mem mem(4096, 4096, s.in.baseRows);
  sr::Ranker r;
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, mem.m, r).outcome));
  TEST_ASSERT_EQUAL_UINT32(sr::sparseRows(s.in.baseRows), mem.sparse.n);
  const Bytes trks = sectionOf(c.base, sx::kBaseSpec, 0);
  const uint32_t rows = static_cast<uint32_t>(trks.size() / sf::kTrackBytes);
  TEST_ASSERT_EQUAL_UINT32(s.in.baseRows, rows);
  for (uint32_t i = 0; i < rows; ++i) {
    const uint64_t h = cc::get64(u8(trks) + i * sf::kTrackBytes);
    if (i % sr::kSparseEvery == 0) {
      TEST_ASSERT_TRUE(mem.sparse.hashAt(i / sr::kSparseEvery) == h);
      TEST_ASSERT_EQUAL_UINT32(i, mem.sparse.rowAt(i / sr::kSparseEvery));
    }
    uint32_t from = 0, to = 0;
    mem.sparse.span(h, rows, &from, &to);
    TEST_ASSERT_TRUE(from <= i && i < to);
    TEST_ASSERT_TRUE(to - from <= sr::kSparseEvery);
  }
  uint32_t from = 9, to = 9;
  mem.sparse.span(cc::get64(u8(trks)) - 1, rows, &from, &to);
  TEST_ASSERT_EQUAL_UINT32(0, to - from);
  // The played set's contains().
  for (uint32_t k = 0; k < mem.played.n; ++k) TEST_ASSERT_TRUE(mem.played.contains(mem.played.at(k)));
  TEST_ASSERT_FALSE(mem.played.contains(0));
  // No chunks: every played hash counted, none kept.
  Mem none(4096, 0, s.in.baseRows);
  sr::Ranker r2;
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, none.m, r2).outcome));
  TEST_ASSERT_EQUAL_UINT32(0, none.played.n);
  TEST_ASSERT_EQUAL_UINT32(sc["rows"]["played"].u64(), none.played.lost);
}

// The refusals: never a list over a card that isn't the session's.
void test_the_refusals() {
  const minijson::Value& sc = rankScenario("rank-big");
  const RankCard c = rankCard(sc);
  const int today = todayOf(sc["today"]);
  auto run = [&](cutfs::CutFs& card, const sr::Input& in, uint32_t windowRows = 4096) {
    Mem mem(windowRows, 4096, in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, in, mem.m, r);
    TEST_ASSERT_EQUAL_UINT32(0, card.openNow);
    return std::string(sr::outcomeName(rep.outcome));
  };
  cutfs::CutFs card;
  putRankCard(card, c);
  Session s;
  readCard(card, s, today);
  TEST_ASSERT_EQUAL_STRING("ok", run(card, s.in).c_str());
  sr::Input in = s.in;
  in.generation += 1;  // another player saved since
  TEST_ASSERT_EQUAL_STRING("changed", run(card, in).c_str());
  in = s.in;
  in.headerCrc ^= 1;
  TEST_ASSERT_EQUAL_STRING("changed", run(card, in).c_str());
  in = s.in;
  in.present = false;  // the session stands on none, the card has one
  TEST_ASSERT_EQUAL_STRING("changed", run(card, in).c_str());
  in = s.in;
  in.baseCrc ^= 1;  // the base another file
  TEST_ASSERT_EQUAL_STRING("base", run(card, in).c_str());
  in = s.in;
  in.baseRows -= 1;
  TEST_ASSERT_EQUAL_STRING("base", run(card, in).c_str());
  TEST_ASSERT_EQUAL_STRING("room", run(card, s.in, 10).c_str());
  {
    cutfs::CutFs gone;
    put(gone, "stats.bin", c.bin);  // the base gone
    TEST_ASSERT_EQUAL_STRING("base", run(gone, s.in).c_str());
    cutfs::CutFs none;  // stats.bin gone where the session stands on one
    TEST_ASSERT_EQUAL_STRING("changed", run(none, s.in).c_str());
  }
  {
    // A read the card fails: every read and open after the first k.
    for (long k = 0; k < 12; ++k) {
      cutfs::CutFs bad;
      putRankCard(bad, c);
      bad.faultPrefix = kDir;
      bad.faultFrom = k;
      const std::string o = run(bad, s.in);
      TEST_ASSERT_TRUE_MESSAGE(o == "io", (std::to_string(k) + ": " + o).c_str());
    }
  }
  {
    // A byte of TRKD's rows damaged: its CRC.
    Bytes bin = c.bin;
    cc::MemSource src(bin.data(), static_cast<uint32_t>(bin.size()));
    cc::Container fr;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(fr.open(src, sx::kStatsSpec, nullptr)));
    bin[fr.section(sf::kTrkd).offset + 30] ^= 0x40;
    cutfs::CutFs dmg;
    put(dmg, "stats.bin", bin);
    put(dmg, c.baseName, c.base);
    TEST_ASSERT_EQUAL_STRING("damaged", run(dmg, s.in).c_str());
  }
}

void sameTrack(const sf::Track& a, const sf::Track& b) {
  TEST_ASSERT_TRUE(a.hash == b.hash);
  TEST_ASSERT_EQUAL_UINT16(a.plays, b.plays);
  TEST_ASSERT_EQUAL_UINT16(a.skips, b.skips);
  TEST_ASSERT_EQUAL_UINT32(a.firstAt, b.firstAt);
  TEST_ASSERT_EQUAL_UINT32(a.lastAt, b.lastAt);
  TEST_ASSERT_EQUAL_UINT32(a.lastOrd, b.lastOrd);
  TEST_ASSERT_EQUAL_UINT8(a.src, b.src);
}

// A track's rows (Hp's read: the Track lane): the base's by a binary search
// (at most a read a level), stats.bin's TRKD streamed; for every hash of
// rank-big's base and TRKD, and hashes in neither, as the files hold them;
// files open()ed once for many reads, closed after; another copy refused.
void test_a_tracks_rows() {
  const minijson::Value& sc = rankScenario("rank-big");
  const RankCard c = rankCard(sc);
  cutfs::CutFs card;
  putRankCard(card, c);
  Session s;
  readCard(card, s, todayOf(sc["today"]));
  std::map<uint64_t, sf::Track> base, trkd;
  const Bytes trks = sectionOf(c.base, sx::kBaseSpec, 0);
  for (size_t o = 0; o < trks.size(); o += sf::kTrackBytes) {
    const sf::Track t = sf::decodeTrack(u8(trks) + o);
    base[t.hash] = t;
  }
  const Bytes td = sectionOf(c.bin, sx::kStatsSpec, sf::kTrkd);
  for (size_t o = 0; o < td.size(); o += sf::kTrackBytes) {
    const sf::Track t = sf::decodeTrack(u8(td) + o);
    trkd[t.hash] = t;
  }
  std::vector<uint64_t> hashes;
  for (const auto& kv : base) hashes.push_back(kv.first);
  for (const auto& kv : trkd) hashes.push_back(kv.first);
  for (uint64_t k = 1; k < 40; ++k) hashes.push_back(k * 0x9E3779B97F4A7C15ull);
  hashes.push_back(0);
  hashes.push_back(UINT64_MAX);
  Block work(sx::kWorkBytes);
  sr::Files f;
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::open(card, s.in, &f)));
  TEST_ASSERT_EQUAL_UINT32(2, card.openNow);
  uint32_t most = 0;
  for (uint64_t h : hashes) {
    const sr::TrackRows t = sr::trackRows(f, s.in, h, work.p());
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(t.outcome));
    TEST_ASSERT_EQUAL(base.count(h) == 1, t.base);
    TEST_ASSERT_EQUAL(trkd.count(h) == 1, t.delta);
    if (t.base) sameTrack(base[h], t.baseRow);
    if (t.delta) sameTrack(trkd[h], t.deltaRow);
    TEST_ASSERT_EQUAL_UINT32(trkd.size(), t.trkdRows);
    if (t.reads > most) most = t.reads;
  }
  uint32_t levels = 0;
  while ((1u << levels) <= s.in.baseRows) ++levels;
  TEST_ASSERT_TRUE(most <= levels);
  printf("[rank] a track's rows: at most %u base reads over %u rows\n", static_cast<unsigned>(most),
         static_cast<unsigned>(s.in.baseRows));
  sr::Input other = s.in;
  other.generation += 1;
  TEST_ASSERT_EQUAL_STRING("changed", sr::outcomeName(sr::trackRows(f, other, hashes[0], work.p()).outcome));
  sr::close(card, &f);
  TEST_ASSERT_EQUAL_UINT32(0, card.openNow);
  // open()'s refusals: none there where the session stands on one; the base gone.
  cutfs::CutFs none;
  TEST_ASSERT_EQUAL_STRING("changed", sr::outcomeName(sr::open(none, s.in, &f)));
  cutfs::CutFs noBase;
  put(noBase, "stats.bin", c.bin);
  TEST_ASSERT_EQUAL_STRING("base", sr::outcomeName(sr::open(noBase, s.in, &f)));
  TEST_ASSERT_EQUAL_UINT32(0, noBase.openNow);
  TEST_ASSERT_TRUE(f.bin == nullptr && f.base == nullptr);
}

// A newer minor's base (5.12: read by its known sections and its rows'
// prefixes): TRKS rows of 32 B (the first minor's 24 and 8 more) after a
// longer header. The rank (CardRows: the section's stride) and a track's
// read (the binary search: TRKS's own offset and stride) give the rows the
// 24 B prefixes hold.
void test_a_newer_minor_s_base() {
  const minijson::Value& sc = rankScenario("rank-no-base");
  const RankCard c = rankCard(sc);  // (TRKD alone: its stats.bin names no base)
  // A base of 300 rows of 32 B, minor 3, a 48 B header.
  std::vector<sf::Track> rows;
  std::mt19937_64 rng(3);
  std::set<uint64_t> seen;
  while (rows.size() < 300) {
    sf::Track t;
    t.hash = rng() | 1;
    if (!seen.insert(t.hash).second) continue;
    t.plays = static_cast<uint16_t>(1 + rng() % 9);
    t.firstAt = 300u * 86400 + 1;
    t.lastAt = 310u * 86400 + 1 + static_cast<uint32_t>(rng() % 86400);
    t.lastOrd = 7;
    rows.push_back(t);
  }
  std::sort(rows.begin(), rows.end(), [](const sf::Track& a, const sf::Track& b) { return a.hash < b.hash; });
  Bytes data;
  for (const sf::Track& t : rows) {
    uint8_t r[32] = {};
    sf::encodeTrack(t, r);
    std::memset(r + 24, 0xA5, 8);  // (a newer minor's tail)
    data.append(reinterpret_cast<const char*>(r), sizeof(r));
  }
  cc::SectionOut out;
  out.type = sf::kTrks;
  out.flags = cc::kSectionRequired;
  out.stride = 32;
  out.count = static_cast<uint32_t>(rows.size());
  out.bytes = static_cast<uint32_t>(data.size());
  std::vector<uint8_t> buf(48 + 32 + data.size() + 64);
  cc::BufferSink sink(buf.data(), static_cast<uint32_t>(buf.size()));
  cc::FileMeta meta;
  meta.magic = sx::kMagic;
  meta.major = sx::kMajor;
  meta.minor = 3;
  meta.generation = 5;
  const uint8_t typeHeader[8] = {};
  cc::ContainerWriter w;
  TEST_ASSERT_TRUE(w.begin(sink, meta, typeHeader, 48, &out, 1));
  TEST_ASSERT_TRUE(w.write(data.data(), static_cast<uint32_t>(data.size())));
  uint32_t fileBytes = 0;
  TEST_ASSERT_TRUE(w.finish(&fileBytes));
  const Bytes base(reinterpret_cast<const char*>(buf.data()), fileBytes);
  cc::MemSource src(base.data(), static_cast<uint32_t>(base.size()));
  cc::Container fr;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(fr.open(src, sx::kBaseSpec, nullptr)));
  TEST_ASSERT_TRUE(fr.section(0).offset != sx::kBaseRowsAt);  // (not where the first minor's rows are)
  // A card of TRKD (rank-no-base's stats.bin) and this base: HEAD's base
  // fields given as the input's (the rank checks them, not HEAD).
  cutfs::CutFs card;
  put(card, "stats.bin", c.bin);
  char n[sx::kNameCap];
  TEST_ASSERT_TRUE(sx::baseName(5, n, sizeof(n)));
  put(card, std::string(n).substr(kDir.size()), base);
  Session s;
  const int today = todayOf(sc["today"]);
  readCard(card, s, today);
  s.in.baseGen = 5;
  s.in.baseRows = static_cast<uint32_t>(rows.size());
  s.in.baseCrc = fr.header().headerCrc;
  Mem mem(4096, 4096, s.in.baseRows);
  sr::Ranker r;
  const sr::Report rep = sr::rank(card, s.in, mem.m, r);
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep.outcome));
  TEST_ASSERT_EQUAL_UINT32(rows.size(), rep.baseRows);
  Ref ref;
  for (const sf::Track& t : rows) addRow(ref, t);
  {
    const Bytes trkd = sectionOf(c.bin, sx::kStatsSpec, sf::kTrkd);
    for (size_t o = 0; o < trkd.size(); o += sf::kTrackBytes) addRow(ref, sf::decodeTrack(u8(trkd) + o));
    const Bytes rc = sectionOf(c.bin, sx::kStatsSpec, sf::kRcnt);
    for (size_t o = 0; o < rc.size(); o += 12)
      ref.rcnt[{cc::get16(u8(rc) + o), cc::get64(u8(rc) + o + 4)}] += cc::get16(u8(rc) + o + 2);
  }
  sameAsRef(r, ref, today, s.in.liveRound, "a newer minor's base");
  Block work(sx::kWorkBytes);
  sr::Files f;
  TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::open(card, s.in, &f)));
  for (const sf::Track& t : rows) {
    const sr::TrackRows tr = sr::trackRows(f, s.in, t.hash, work.p());
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(tr.outcome));
    TEST_ASSERT_TRUE(tr.base);
    sameTrack(t, tr.baseRow);
  }
  sr::close(card, &f);
}

// ---------------------------------------------------------------------------
// S3a's V1
// ---------------------------------------------------------------------------

// Ranker::total(): every row offered, kept or not, at each cap (kCaps), at
// its cap and 16 (the rows kept), one past and well past; begin() starts
// them over; the lists the clock unset doesn't offer count none.
void test_totals_at_and_past_each_cap() {
  Block mem(sr::Ranker::kBytes);
  const uint16_t today = 320;
  auto timeOn = [](int day) { return static_cast<uint32_t>(day) * 86400 + 1 + 3600; };
  for (int l = 0; l < sr::kLists; ++l) {
    const sr::List li = static_cast<sr::List>(l);
    for (const uint32_t n : {sr::kCaps[li], sr::rowsOf(li), sr::rowsOf(li) + 1, 3 * sr::rowsOf(li) + 5}) {
      for (const bool dated : {true, false}) {
        sr::Ranker r;
        r.begin(mem.p(), true, today, 0);
        sf::Track stale;  // (offered before: begin() below starts the totals over)
        stale.plays = 1;
        stale.firstAt = 0;
        for (uint32_t i = 0; i < 9; ++i) {
          stale.hash = i + 1;
          r.track(stale);
        }
        r.begin(mem.p(), dated, today, 0);
        uint32_t want[sr::kLists] = {};
        for (uint32_t i = 0; i < n; ++i) {
          const uint64_t h = 0x9E3779B97F4A7C15ull * (i + 1);
          sf::Track t;
          t.hash = h;
          switch (li) {
            case sr::kMost7:  // a play today: both windows
              r.windows(h, 1 + i % 3, today, 1 + i % 3, today);
              want[sr::kMost7] += dated;
              want[sr::kMost30] += dated;
              continue;
            case sr::kMost30:  // a play 10 days back: the 30 days alone
              r.windows(h, 0, 0, 1 + i % 3, today - 10);
              want[sr::kMost30] += dated;
              continue;
            case sr::kNewFinds:
              t.plays = static_cast<uint16_t>(1 + i % 4);
              t.firstAt = t.lastAt = timeOn(today - static_cast<int>(i % 30));
              want[sr::kNewFinds] += dated;
              break;
            case sr::kOldFavourites:
              t.plays = static_cast<uint16_t>(5 + i % 4);
              t.firstAt = t.lastAt = timeOn(today - 30 - static_cast<int>(i % 100));
              want[sr::kOldFavourites] += dated;
              break;
            default:  // All time and Recently played: an undated play
              t.plays = 1;
              t.firstAt = 0;
              t.lastOrd = i;
              break;
          }
          r.track(t);
          ++want[sr::kMostAll];
          ++want[sr::kRecent];
        }
        r.finish();
        const std::string what =
            std::string(sr::listName(li)) + " at " + std::to_string(n) + (dated ? "" : " undated");
        for (int k = 0; k < sr::kLists; ++k) {
          const sr::List lk = static_cast<sr::List>(k);
          const std::string m = what + ": " + sr::listName(lk);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[k], r.total(lk), m.c_str());
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(std::min(want[k], sr::rowsOf(lk)), r.size(lk), m.c_str());
        }
      }
    }
  }
}

// The 7 days and the span against statsref's: the clock set (rank-big:
// day 318's row at its fields' tops), unset (rank-no-clock: no days, the
// span still), plays across UTC midnights (rank-midnight), DAYS at its 400
// rows (rank-days). The records' side is test_the_records_merged_in's.
void test_the_seven_days_and_the_span() {
  for (const char* name : {"rank-big", "rank-no-clock", "rank-midnight", "rank-days"}) {
    const minijson::Value& sc = rankScenario(name);
    const RankCard c = rankCard(sc);
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, todayOf(sc["today"]));
    Mem mem(4096 + 40, 4096, s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), name);
    sameDays7(rep, sc["days7"], name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sc["rows"]["days"].u64(), rep.daysRows, name);
  }
  const minijson::Value& days = rankScenario("rank-days");
  TEST_ASSERT_EQUAL_UINT32(sf::kDaysRows, days["rows"]["days"].u64());
  const minijson::Value& mid = rankScenario("rank-midnight")["days7"]["days"];
  TEST_ASSERT_EQUAL_UINT32(1510000, mid[5][0].u64());  // day 319: 23:50's 20 min, the 23:59:59.999 skip, 00:00's
  TEST_ASSERT_EQUAL_UINT32(440000, mid[6][0].u64());   // day 320: 00:00's and 23:59:59's whole
  TEST_ASSERT_TRUE(rankScenario("rank-no-clock")["days7"]["days"].isNull());
}

// RCNT's oldest day (7.6's stats.days_n): with RCNT capped, inside the 30
// days (rank-rcnt-cap; the fixture boot rcnt-cap); the clock unset still
// reads it; a record's older row moves it. S3b's mark (rcntShort()): DAYS'
// first played day in the window before RCNT's oldest; a records' DAYS
// play inside the window gives it, a skip or a day outside doesn't, and the
// fixture boot's cap (day 288 gone, 289 kept) leaves the window whole.
void test_rcnt_first_day_with_rcnt_capped() {
  const minijson::Value& sc = rankScenario("rank-rcnt-cap");
  const RankCard c = rankCard(sc);
  const int today = todayOf(sc["today"]);
  for (const int day : {today, -1}) {
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, day);
    Mem mem(4096 + 40, 4096, s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep.outcome));
    TEST_ASSERT_EQUAL_UINT16(sc["days7"]["rcntFirstDay"].u64(), rep.rcntFirstDay);
    TEST_ASSERT_EQUAL_UINT16(today - 12, rep.rcntFirstDay);
    TEST_ASSERT_TRUE(rep.rcntFirstDay > today - 29);  // the 30 days hold 13
    TEST_ASSERT_TRUE(rep.rcntRows <= sf::kRcntRows);
    if (day >= 0) {
      TEST_ASSERT_EQUAL_UINT32(rep.rcntRows, rep.windowRows);
      sameTotals(r, sc["totals"], "rank-rcnt-cap");
    }
    // S3b's mark (rcntShort(), the 30-day cell's "13 days"): DAYS played on
    // the window's days before RCNT's oldest, or not. A records' DAYS row
    // (the session's) with a play 20 days back marks it; one with only a
    // skip, or a day before the window, doesn't.
    if (day >= 0) {
      const uint16_t d30 = rep.days30FirstDay;
      TEST_ASSERT_EQUAL(d30 != sf::kUnsetDay && d30 < rep.rcntFirstDay, sr::rcntShort(rep));
      const sr::Input keep = s.in;
      for (const int k : {0, 1, 2}) {
        uint8_t drow[16] = {};
        cc::put16(drow, static_cast<uint16_t>(k == 2 ? today - 30 : today - 20));
        cc::put16(drow + 2, static_cast<uint16_t>(k == 1 ? 0 : 1));  // plays
        cc::put16(drow + 4, static_cast<uint16_t>(k == 1 ? 1 : 0));  // skips
        cc::put32(drow + 8, 200000);
        s.in.days = drow;
        s.in.daysRows = 1;
        sr::Ranker rd;
        const sr::Report rr = sr::rank(card, s.in, mem.m, rd);
        TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rr.outcome));
        TEST_ASSERT_EQUAL_UINT16(today - 12, rr.rcntFirstDay);
        if (k == 0) {
          TEST_ASSERT_EQUAL_UINT16(std::min<int>(today - 20, d30 == sf::kUnsetDay ? today : d30), rr.days30FirstDay);
          TEST_ASSERT_TRUE(sr::rcntShort(rr));
        } else {
          TEST_ASSERT_EQUAL_UINT16(d30, rr.days30FirstDay);
          TEST_ASSERT_EQUAL(sr::rcntShort(rep), sr::rcntShort(rr));
        }
      }
      s.in = keep;
    }
    // A record's row 40 days back (the session's: RCNT not yet pruned).
    uint8_t row[12];
    cc::put16(row, static_cast<uint16_t>(today - 40));
    cc::put16(row + 2, 1);
    cc::put64(row + 4, 0x1234567890ABCDEFull);
    s.in.rcnt = row;
    s.in.rcntRows = 1;
    sr::Ranker r2;
    const sr::Report rep2 = sr::rank(card, s.in, mem.m, r2);
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep2.outcome));
    TEST_ASSERT_EQUAL_UINT16(today - 40, rep2.rcntFirstDay);
    TEST_ASSERT_EQUAL_UINT32(rep.windowRows, rep2.windowRows);  // (outside the window)
    // ... RCNT's oldest now before the window: not short, whatever DAYS has.
    TEST_ASSERT_FALSE(sr::rcntShort(rep2));
  }
  uint32_t boots = 0;
  for (const Card& k : gCards) {
    if (k.what.compare(0, 9, "rcnt-cap ") != 0) continue;
    Run run(k, k.today);
    Mem mem(4096 + 40, 4096, run.s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(run.card, run.s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(rep.outcome));
    const Ref ref = refOf(k, run.s.in);
    TEST_ASSERT_EQUAL_UINT16(ref.rcnt.begin()->first.first, rep.rcntFirstDay);
    TEST_ASSERT_EQUAL_UINT16(289, rep.rcntFirstDay);  // day 288 dropped whole
    // ... before the window (290-319): it holds 30 days still, no mark.
    TEST_ASSERT_EQUAL_UINT16(290, rep.days30FirstDay);
    TEST_ASSERT_FALSE(sr::rcntShort(rep));
    ++boots;
  }
  TEST_ASSERT_EQUAL_UINT32(1, boots);
}

// The u40 set on every card: each row the u64 set's top 40 bits, in its
// order, a shared top kept twice; contains() by the top 40 bits; its room
// short: the rest lost, counted; bytesFor() the grades' figures (9.7).
void test_the_u40_set() {
  TEST_ASSERT_EQUAL_UINT32(0, sr::Played40::bytesFor(0));
  TEST_ASSERT_EQUAL_UINT32(16, sr::Played40::bytesFor(1));
  TEST_ASSERT_EQUAL_UINT32(95840, sr::Played40::bytesFor(19165));   // the spare: 19,079 + 46 + 40 rows
  TEST_ASSERT_EQUAL_UINT32(104416, sr::Played40::bytesFor(20883));  // P = 19,478 with TRKD 1,365
  TEST_ASSERT_TRUE(sr::Played40::keyOf(0xFEDCBA9876543210ull) == 0xFEDCBA9876ull);
  for (const Card& c : gCards) {
    for (const int today : {c.today, -1}) {
      const std::string what = c.what + (today < 0 ? " undated" : "");
      Run run(c, today);
      Mem m64(4096 + 64, 4096, run.s.in.baseRows);
      sr::Ranker r64;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(sr::rank(run.card, run.s.in, m64.m, r64).outcome),
                                       what.c_str());
      Mem m40(4096 + 64, 0, run.s.in.baseRows);
      m40.use40(4096);
      sr::Ranker r40;
      const sr::Report rep = sr::rank(run.card, run.s.in, m40.m, r40);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
      sameRanker(r64, r40, what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(m64.played.n, m40.played40.n, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(rep.played, m40.played40.n, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, m40.played40.lost, what.c_str());
      std::vector<uint64_t> keys;
      for (uint32_t k = 0; k < m40.played40.n; ++k) {
        TEST_ASSERT_TRUE_MESSAGE(m40.played40.at(k) == m64.played.at(k) >> 24, what.c_str());
        keys.push_back(m40.played40.at(k));
      }
      TEST_ASSERT_TRUE_MESSAGE(std::is_sorted(keys.begin(), keys.end()), what.c_str());
      const Ref ref = refOf(c, run.s.in);
      for (const auto& kv : ref.rows) {
        const bool want = std::binary_search(keys.begin(), keys.end(), kv.first >> 24);
        TEST_ASSERT_EQUAL_MESSAGE(want, m40.played40.contains(kv.first), what.c_str());
        if (kv.second.plays) TEST_ASSERT_TRUE_MESSAGE(want, what.c_str());
      }
      TEST_ASSERT_FALSE(m40.played40.contains(0));
      // Its room short by 5 (or none): the first rows kept, the rest counted.
      if (m40.played40.n == 0) continue;
      const uint32_t cap = m40.played40.n > 5 ? m40.played40.n - 5 : 0;
      Mem shortM(4096 + 64, 0, run.s.in.baseRows);
      shortM.use40(cap);
      sr::Ranker rs;
      TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(run.card, run.s.in, shortM.m, rs).outcome));
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(cap, shortM.played40.n, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(m40.played40.n - cap, shortM.played40.lost, what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(m40.bytes40().compare(0, cap * sr::Played40::kRowBytes, shortM.bytes40()) == 0,
                               what.c_str());
    }
  }
  // Two played hashes that share their top 40 bits: two equal rows, both
  // found; an unplayed one under the same top found too (call 3's one way).
  {
    const uint64_t a = 0x0123456789000001ull, b = 0x0123456789FFFFFFull, other = 0x0123456789800000ull;
    Bytes trkd;
    // One type for the list: uint64_t is unsigned long on Linux's GCC, so a
    // bare ull literal beside a and b leaves the list's type undeduced.
    for (const uint64_t h : {a, b, uint64_t{0x7000000000000000ull}}) {
      sf::Track t;
      t.hash = h;
      t.plays = 1;
      t.firstAt = 0;
      uint8_t row[sf::kTrackBytes];
      sf::encodeTrack(t, row);
      trkd.append(reinterpret_cast<const char*>(row), sizeof(row));
    }
    cutfs::CutFs card;
    sr::Input in;
    in.trkd = u8(trkd);
    in.trkdRows = 3;
    Mem m(0, 0, 0);
    m.use40(3);
    m.lean();
    sr::Ranker r;
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, in, m.m, r).outcome));
    TEST_ASSERT_EQUAL_UINT32(3, m.played40.n);
    TEST_ASSERT_TRUE(m.played40.at(0) == 0x0123456789ull && m.played40.at(1) == 0x0123456789ull);
    TEST_ASSERT_TRUE(m.played40.at(2) == 0x7000000000ull);
    TEST_ASSERT_EQUAL_STRING("896745230189674523010000000070", hexOf(m.bytes40()).c_str());
    TEST_ASSERT_TRUE(m.played40.contains(a) && m.played40.contains(b) && m.played40.contains(other));
    TEST_ASSERT_FALSE(m.played40.contains(0x0123456788FFFFFFull));
    TEST_ASSERT_FALSE(m.played40.contains(0x012345678A000000ull));
  }
}

// No clock: a null window (no room carved: Windows::add() returns first),
// the lists the undated ones, RCNT and DAYS still read (their span). Dated
// with no window room: Room, said before the tracks are read.
void test_a_null_window_while_undated() {
  uint32_t rooms = 0;
  for (const Card& c : gCards) {
    Run run(c, -1);
    Mem mem(0, 4096, run.s.in.baseRows);
    mem.m.windows = nullptr;
    mem.m.windowRows = 0;
    sr::Ranker r;
    const sr::Report rep = sr::rank(run.card, run.s.in, mem.m, r);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), c.what.c_str());
    const Ref ref = refOf(c, run.s.in);
    sameAsRef(r, ref, -1, run.s.in.liveRound, c.what + " undated");
    sameDaysAsRef(rep, ref, -1, c.what + " undated");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.rcnt.size(), rep.rcntRows, c.what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rep.windowRows, c.what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(playedOf(ref).size(), mem.played.n, c.what.c_str());
    if (c.today < 0) continue;
    Run dated(c, c.today);
    sr::Ranker rd;
    const sr::Report rd2 = sr::rank(dated.card, dated.s.in, mem.m, rd);
    if (windowRowsOf(ref, c.today) == 0) {
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rd2.outcome), c.what.c_str());
      continue;
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE("room", sr::outcomeName(rd2.outcome), c.what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rd2.tracks, c.what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rd2.baseRows + rd2.trkdRows, c.what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, dated.card.openNow, c.what.c_str());
    ++rooms;
  }
  TEST_ASSERT_TRUE(rooms >= 5);
}

// The played-set-only rank (the lean Not played yet ask's): no Ranker
// (begin() and track() not called: a Ranker filled before keeps its lists
// and totals), no window, RCNT and DAYS not read (each damaged, it still
// ranks), only TRKS ⊕ TRKD ⊕ the records' TRKD into the u40 set: the full
// rank's, the clock set and unset; dated with no window room, Ok.
void test_the_played_set_only_rank() {
  for (const Card& c : gCards) {
    for (const int today : {c.today, -1}) {
      const std::string what = c.what + (today < 0 ? " undated" : "");
      Run run(c, today);
      Mem full(4096 + 64, 0, run.s.in.baseRows);
      full.use40(4096);
      sr::Ranker r;
      const sr::Report fr = sr::rank(run.card, run.s.in, full.m, r);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(fr.outcome), what.c_str());
      Mem lean(0, 0, run.s.in.baseRows);
      lean.lean();
      lean.use40(4096);
      const sr::Report lr = sr::rank(run.card, run.s.in, lean.m, r);  // (r: the full rank's, kept)
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(lr.outcome), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(full.played40.n, lean.played40.n, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lean.played40.lost, what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(full.bytes40() == lean.bytes40(), what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(fr.tracks, lr.tracks, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(fr.played, lr.played, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(fr.baseRows, lr.baseRows, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(fr.trkdRows, lr.trkdRows, what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, lr.rcntRows + lr.daysRows + lr.windowRows + lr.recordRows, what.c_str());
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(sf::kUnsetDay, lr.firstDay, what.c_str());
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(sf::kUnsetDay, lr.newestDay, what.c_str());
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(sf::kUnsetDay, lr.rcntFirstDay, what.c_str());
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(sf::kUnsetDay, lr.days30FirstDay, what.c_str());
      for (uint32_t i = 0; i < sr::kWeekDays; ++i)
        TEST_ASSERT_TRUE_MESSAGE(lr.days[i].listenedMs == 0 && lr.days[i].plays == 0, what.c_str());
      // Nothing offered: the full rank's lists and totals as they were.
      Mem again(4096 + 64, 0, run.s.in.baseRows);
      again.use40(4096);
      sr::Ranker r2;
      TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(run.card, run.s.in, again.m, r2).outcome));
      sameRanker(r2, r, what + " after the lean rank");
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, run.card.openNow, what.c_str());
    }
  }
  // RCNT, then DAYS, damaged: the full rank says so, the lean one never
  // reads them; its set the undamaged card's.
  const minijson::Value& sc = rankScenario("rank-big");
  const RankCard c = rankCard(sc);
  for (const sf::Sec sec : {sf::kRcnt, sf::kDays}) {
    for (const int today : {todayOf(sc["today"]), -1}) {
      RankCard bad = c;
      bad.bin = damaged(c.bin, sec);
      cutfs::CutFs good;
      putRankCard(good, c);
      Session s;
      readCard(good, s, today);  // (the session read the card before the damage)
      cutfs::CutFs card;
      putRankCard(card, bad);
      Mem full(4096 + 64, 4096, s.in.baseRows);
      sr::Ranker r;
      TEST_ASSERT_EQUAL_STRING("damaged", sr::outcomeName(sr::rank(card, s.in, full.m, r).outcome));
      Mem lean(0, 0, s.in.baseRows);
      lean.lean();
      lean.use40(4096);
      TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, s.in, lean.m, r).outcome));
      TEST_ASSERT_EQUAL_UINT32(sc["rows"]["played"].u64(), lean.played40.n);
      TEST_ASSERT_EQUAL_STRING(sc["played40"].c_str(), hexOf(lean.bytes40()).c_str());
      TEST_ASSERT_EQUAL_UINT32(0, card.openNow);
    }
  }
}

// The overlay (the Full grade's PW region): every region filled with a
// pattern before the rank; the u40 set given the window's own room, so it
// is written over the window's rows once they are dead (finished into the
// Ranker before the tracks stream). The lists, totals, report and set are
// a rank's with separate regions, on every card, the clock set and unset.
void test_the_overlay() {
  for (const Card& c : gCards) {
    for (const int today : {c.today, -1}) {
      const std::string what = c.what + (today < 0 ? " undated" : "");
      Run run(c, today);
      Mem apart(4096 + 64, 0, run.s.in.baseRows);
      apart.use40(4096);
      sr::Ranker want;
      const sr::Report wr = sr::rank(run.card, run.s.in, apart.m, want);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(wr.outcome), what.c_str());
      for (const uint8_t pattern : {uint8_t{0xA5}, uint8_t{0x00}, uint8_t{0xFF}}) {
        const uint32_t windowRows = 4096 + 64, rows = 4096;
        const size_t pw = std::max(sr::Windows::bytesFor(windowRows), sr::Played40::bytesFor(rows));
        Block block(sx::kWorkBytes + sr::Ranker::kBytes + pw);
        std::memset(block.p(), pattern, sx::kWorkBytes + sr::Ranker::kBytes + pw);
        sr::Played40 set;
        set.rows = block.p() + sx::kWorkBytes + sr::Ranker::kBytes;
        set.cap = rows;
        sr::Memory m;
        m.work = block.p();
        m.lists = block.p() + sx::kWorkBytes;
        m.windows = set.rows;  // the same room
        m.windowRows = windowRows;
        m.played40 = &set;
        sr::Ranker got;
        const sr::Report gr = sr::rank(run.card, run.s.in, m, got);
        const std::string mp = what + " pattern " + std::to_string(pattern);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(gr.outcome), mp.c_str());
        sameRanker(want, got, mp);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(apart.played40.n, set.n, mp.c_str());
        TEST_ASSERT_TRUE_MESSAGE(std::memcmp(apart.played40.rows, set.rows, set.n * sr::Played40::kRowBytes) == 0,
                                 mp.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(wr.windowRows, gr.windowRows, mp.c_str());
        for (uint32_t i = 0; i < sr::kWeekDays; ++i)
          TEST_ASSERT_TRUE_MESSAGE(wr.days[i].listenedMs == gr.days[i].listenedMs &&
                                       wr.days[i].plays == gr.days[i].plays &&
                                       wr.days[i].autoDjPlays == gr.days[i].autoDjPlays &&
                                       wr.days[i].autoDjSkips == gr.days[i].autoDjSkips,
                                   mp.c_str());
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(wr.rcntFirstDay, gr.rcntFirstDay, mp.c_str());
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(wr.days30FirstDay, gr.days30FirstDay, mp.c_str());
      }
    }
  }
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/rank.json", &gRank) || gRank.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  loadCards();
  UNITY_BEGIN();
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_the_rank_scenarios);
  RUN_TEST(test_the_records_merged_in);
  RUN_TEST(test_this_session_first_with_no_clock);
  RUN_TEST(test_the_sparse_index);
  RUN_TEST(test_the_refusals);
  RUN_TEST(test_a_tracks_rows);
  RUN_TEST(test_a_newer_minor_s_base);
  RUN_TEST(test_totals_at_and_past_each_cap);
  RUN_TEST(test_the_seven_days_and_the_span);
  RUN_TEST(test_rcnt_first_day_with_rcnt_capped);
  RUN_TEST(test_the_u40_set);
  RUN_TEST(test_a_null_window_while_undated);
  RUN_TEST(test_the_played_set_only_rank);
  RUN_TEST(test_the_overlay);
  return UNITY_END();
}
