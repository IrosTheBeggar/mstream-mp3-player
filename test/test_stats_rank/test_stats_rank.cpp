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
// Run: pio test -e native -f test_stats_rank
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsRank.h"

namespace cc = cardcontract;
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
}

// Everything a rank needs, carved for `windowRows` and `playedRows`.
struct Mem {
  Block work{sx::kWorkBytes};
  Block lists{sr::Ranker::kBytes};
  std::unique_ptr<Block> windows;
  std::vector<std::unique_ptr<Block>> chunks;
  std::unique_ptr<Block> sparseRows;
  sr::Played played;
  sr::Sparse sparse;
  sr::Memory m;
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
};

void addRow(Ref& ref, const sf::Track& t) {
  auto it = ref.rows.find(t.hash);
  if (it == ref.rows.end()) {
    ref.rows[t.hash] = t;
  } else {
    it->second = sf::merge(it->second, t);
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
}

std::vector<sr::Entry> refList(const Ref& ref, sr::List l, int today) {
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
  if (v.size() > sr::rowsOf(l)) v.resize(sr::rowsOf(l));
  return v;
}

void sameAsRef(const sr::Ranker& r, const Ref& ref, int today, const std::string& what) {
  for (int l = 0; l < sr::kLists; ++l) {
    const sr::List li = static_cast<sr::List>(l);
    const std::vector<sr::Entry> want = refList(ref, li, today);
    const std::string m = what + " " + sr::listName(li);
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

}  // namespace

void setUp() {}
void tearDown() {}

// Every fixture boot that reads back: the lists statsref gives over the
// card the boot leaves.
void test_every_fixture_boot() {
  uint32_t boots = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    const minijson::Value& bootsV = sc["boots"];
    for (size_t b = 0; b < bootsV.size(); ++b) {
      const minijson::Value& e = bootsV[b];
      if (!e.has("lists")) continue;
      const std::string what = sc["name"].str() + " boot " + std::to_string(b + 1);
      cutfs::CutFs card;
      for (const auto& m : e["out"].members)
        put(card, m.first, cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first));
      Session s;
      readCard(card, s, todayOf(e["today"]));
      Mem mem(4096 + 64, 4096, s.in.baseRows);
      sr::Ranker r;
      const sr::Report rep = sr::rank(card, s.in, mem.m, r);
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
      sameLists(r, e["lists"], what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.openNow, what.c_str());
      ++boots;
    }
  }
  TEST_ASSERT_TRUE(boots >= 25);
  printf("[rank] %u fixture boots ranked as statsref ranks them\n", static_cast<unsigned>(boots));
}

// The rankers' scenarios: each list past its cap and 16.
void test_the_rank_scenarios() {
  TEST_ASSERT_TRUE(gRank.size() >= 5);
  for (size_t i = 0; i < gRank.size(); ++i) {
    const minijson::Value& sc = gRank[i];
    const std::string what = sc["name"].str();
    cutfs::CutFs card;
    const RankCard c = rankCard(sc);
    putRankCard(card, c);
    Session s;
    const int today = todayOf(sc["today"]);
    readCard(card, s, today);
    const minijson::Value& rows = sc["rows"];
    Mem mem(static_cast<uint32_t>(rows["rcnt"].u64()), static_cast<uint32_t>(rows["played"].u64()), s.in.baseRows);
    sr::Ranker r;
    card.bytesRead = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
    sameLists(r, sc["lists"], what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["base"].u64(), rep.baseRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["trkd"].u64(), rep.trkdRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["rcnt"].u64(), rep.rcntRows, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["played"].u64(), rep.played, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows["played"].u64(), mem.played.n, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, mem.played.lost, what.c_str());
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
    sameAsRef(r, plain, today, "no records");
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
    Bytes trkd, rcnt;
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
    Ref ref = plain;
    for (const auto& kv : recs) addRow(ref, kv.second);
    for (const auto& kv : recRcnt) ref.rcnt[kv.first] += kv.second;
    cutfs::CutFs card;
    putRankCard(card, c);
    Session s;
    readCard(card, s, today);
    s.in.trkd = u8(trkd);
    s.in.trkdRows = static_cast<uint32_t>(recs.size());
    s.in.rcnt = u8(rcnt);
    s.in.rcntRows = static_cast<uint32_t>(recRcnt.size());
    Mem mem(4096 + 40, 4096 + 40, s.in.baseRows);
    sr::Ranker r;
    const sr::Report rep = sr::rank(card, s.in, mem.m, r);
    const std::string what = "round " + std::to_string(round);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(rep.outcome), what.c_str());
    sameAsRef(r, ref, today, what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.rows.size(), rep.tracks, what.c_str());
    // The played set: every hash with a play, ascending.
    std::vector<uint64_t> want;
    for (const auto& kv : ref.rows)
      if (kv.second.plays) want.push_back(kv.first);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), mem.played.n, what.c_str());
    for (uint32_t k = 0; k < mem.played.n; ++k) TEST_ASSERT_TRUE_MESSAGE(want[k] == mem.played.at(k), what.c_str());
  }
  // With no stats.bin (a fresh start: the session stands on none), the
  // records alone.
  {
    Ref ref;
    std::map<uint64_t, sf::Track> recs;
    Bytes trkd;
    for (int k = 0; k < 40; ++k) {
      sf::Track t;
      t.hash = rng() | 1;
      t.plays = 1;
      t.firstAt = t.lastAt = static_cast<uint32_t>(today) * 86400 + 1 + static_cast<uint32_t>(k);
      t.lastOrd = 2;
      recs[t.hash] = t;
      addRow(ref, t);
    }
    for (const auto& kv : recs) {
      uint8_t row[sf::kTrackBytes];
      sf::encodeTrack(kv.second, row);
      trkd.append(reinterpret_cast<const char*>(row), sizeof(row));
    }
    cutfs::CutFs card;
    sr::Input in;
    in.trkd = u8(trkd);
    in.trkdRows = 40;
    in.hasToday = true;
    in.today = static_cast<uint16_t>(today);
    Mem mem(64, 64, 0);
    sr::Ranker r;
    TEST_ASSERT_EQUAL_STRING("ok", sr::outcomeName(sr::rank(card, in, mem.m, r).outcome));
    sameAsRef(r, ref, today, "records alone");
    TEST_ASSERT_EQUAL_UINT32(40, mem.played.n);
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
  sameAsRef(r, ref, today, "a newer minor's base");
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

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/rank.json", &gRank) || gRank.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_the_rank_scenarios);
  RUN_TEST(test_the_records_merged_in);
  RUN_TEST(test_the_sparse_index);
  RUN_TEST(test_the_refusals);
  RUN_TEST(test_a_tracks_rows);
  RUN_TEST(test_a_newer_minor_s_base);
  return UNITY_END();
}
