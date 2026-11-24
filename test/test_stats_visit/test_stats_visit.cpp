// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the visit's snapshot and the grades (lib/core/StatsVisit;
// docs/LISTENING-STATS.md 9.7's S3a: "the snapshot", "the grades", V3):
// - the snapshot's bytes, 9.7's table: the 160 B header, each list's room
//   and stride, Not played yet's bits and A-Z directory; 3,860 B at 1,706
//   albums, 3,867 B at 1,747; at most 4,096 B, Old favourites and Recently
//   played giving up their last rows past 3,216 albums (9.7's "about
//   3,200");
// - write() and resolve() over every rank scenario (stats/rank.json): each
//   kept row straight in, its wanted row's index in the id field and its
//   facts packed; the header from the Ranker and the report (statsref's
//   totals and 7 days); a block never written, or part-written by an
//   earlier try, rewritten whole, byte for byte; then the ids: orphans and
//   collisions dropped, each list compacted and cut to its cap, the totals
//   less the orphans among the kept rows;
// - the counts' saturation (65,535; New finds' 16,383 beside its source)
//   and when's codes (minutes since 2026; undated this session, undated
//   earlier; saturating in 2057); ranked with no clock, the two codes
//   alone, this session's rows (the rank's) first (the owner's call,
//   2026-11-24);
// - notPlayed(): the album bits in A-Z order and the directory over
//   libraries of 1 to 6,000 albums, albumAt() for every k, nothing written
//   past the block (trimmed or not); refused for a map not Ready, another
//   index, a block for other albums, a block not written;
// - every fixture boot statsref names Not played yet for (stats/index.json
//   over stats/library.json), the visit's whole way: the rank (the u40
//   set), the wanted set in one region, write(), the walk, resolve(),
//   notPlayed(): each list statsref's less its orphans, cut to its cap, by
//   track id; Not played yet statsref's through albumAt(); and a lean
//   ask's bits written later into the held snapshot, its lists untouched;
// - layout() against 9.7's grade table (Full, Lists, Lean, No folders), the
//   spare's column and P = 19,478's, with and without a clock, at TRKD's
//   cap and at P = 2,214; each region inside the block, and what overlaps
//   dead by the time it is written;
// - phaseOf(): every why (S3a's tag scan's, 128, among them), each
//   Rank-lane wait (Working, then Waiting from 3 s, nothing counted), the boot's scan (Working however long), the
//   read faults (Unread, and nothing else Unread), the refusals (NoRoom),
//   the cap (Capped), the card (NoCard, and an ask it cancelled NoCard
//   until the next), the stats off (Idle); landed: Ready only while its
//   snapshot is served, Idle once it is freed;
// - the card worker's 30 s give-up (stallHolds(), waitStartMoves()) in
//   StatsLists' order, loop() then the turn in the same pass: held 30 s
//   from it (Waiting from 3 s), then taken, a Rank-lane wait, S2's gate or
//   a turn the write gate held across its end included; the rule before
//   the check of the review's fixes never took it;
// - the loan of the Dance tab's block (loanOk(), the owner's "Borrow
//   Dance's block"): only while the block exists and the tab is down, and
//   for a snapshot within kBound (4,096 B: to 18,432 albums);
// - the tag scan's word for kWhyRest (restHolds(), the owner's "Only while
//   held" and "Don't wait at the floor"): its truth table, and with the
//   scheduler's own battery floor and its hysteresis.
// Run: pio test -e native -f test_stats_visit
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "IdView.h"
#include "LibraryIndex.h"
#include "PathHashWalk.h"
#include "ScanScheduler.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsMap.h"
#include "StatsRank.h"
#include "StatsVisit.h"

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sm = statsmap;
namespace sr = statsrank;
namespace sv = statsvisit;
namespace sx = statsfile;
using pathhash::Walk;

void setUp() {}
void tearDown() {}

namespace {

// A block aligned as malloc's, `guard` bytes of 0xEE past `bytes`.
struct Block {
  std::unique_ptr<uint64_t[]> m;
  size_t bytes;
  static constexpr size_t kGuard = 64;
  explicit Block(size_t n) : m(new uint64_t[(n + kGuard + 7) / 8 + 1]), bytes(n) {
    std::memset(m.get(), 0, bytes);
    std::memset(p() + bytes, 0xEE, kGuard);
  }
  uint8_t* p() { return reinterpret_cast<uint8_t*>(m.get()); }
  const uint8_t* p() const { return reinterpret_cast<const uint8_t*>(m.get()); }
  void fill(uint8_t v) { std::memset(p(), v, bytes); }
  bool guarded() const {
    for (size_t i = 0; i < kGuard; ++i)
      if (p()[bytes + i] != 0xEE) return false;
    return true;
  }
  std::string str() const { return std::string(reinterpret_cast<const char*>(p()), bytes); }
};

sv::Snapshot* snapOf(Block& b) { return sv::Snapshot::at(b.p()); }

const sr::List kAll[sr::kLists] = {sr::kMost7, sr::kMost30, sr::kMostAll, sr::kNewFinds, sr::kOldFavourites,
                                   sr::kRecent};

// The wanted set over one region of the lists' rows (Full's and Lists':
// 9.7's grades, V2's rowsCap), every kept row of every list added, sealed.
struct WantedRegion {
  Block block{sm::Wanted::bytesFor(sr::Ranker::kEntries)};
  sm::Wanted w;
  explicit WantedRegion(const sr::Ranker& k) {
    TEST_ASSERT_TRUE(w.region(block.p(), sr::Ranker::kEntries));
    for (sr::List l : kAll)
      for (uint32_t i = 0; i < k.size(l); ++i) TEST_ASSERT_TRUE(w.add(k.at(l, i).hash));
    w.seal();
  }
};

// What the copy-out writes for a kept row's facts (9.7's table), worked out
// here from the Ranker's entry. Recently played ranked with no clock: This
// session for the rank's own rows of it (lastOrd its live round, whatever
// lastAt), else Earlier, never a minute (the owner's call, 2026-11-24).
uint16_t sat16(uint32_t v) { return static_cast<uint16_t>(v > 0xFFFF ? 0xFFFF : v); }
uint32_t whenOf(const sr::Entry& e, uint32_t liveRound, bool dated) {
  if (!dated) return e.session ? sv::kWhenSession : sv::kWhenEarlier;
  if (e.at == 0) return e.ord == liveRound ? sv::kWhenSession : sv::kWhenEarlier;
  const uint32_t m = (e.at - 1) / 60;
  return m > sv::kWhenMax ? sv::kWhenMax : m;
}
void sameFacts(sr::List l, const sr::Entry& e, const sv::Row& r, uint32_t liveRound, bool dated,
               const std::string& m) {
  switch (l) {
    case sr::kNewFinds:
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.count > 0x3FFF ? 0x3FFF : e.count, r.count, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.src & 3, r.src, m.c_str());
      break;
    case sr::kOldFavourites:
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sat16(e.count), r.count, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sf::dayOfTime(e.at), r.lastDay, m.c_str());
      break;
    case sr::kRecent:
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(whenOf(e, liveRound, dated), r.when, m.c_str());
      break;
    default:
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sat16(e.count), r.count, m.c_str());
      break;
  }
}

// ---------------------------------------------------------------------------
// The cards, ranked (test_stats_map's way: the boot's read, then the rank).
// ---------------------------------------------------------------------------
minijson::Value gIndex;
minijson::Value gRank;
const std::string kDir = "/.player/";
using Files = std::vector<std::pair<std::string, std::string>>;  // under /.player/: name, bytes

int todayOf(const minijson::Value& v) { return v.isNull() ? -1 : static_cast<int>(v.u64()); }

std::string sectionOf(const std::string& b, const cc::FormatSpec& spec, uint32_t i) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, spec, nullptr)));
  return b.substr(c.section(i).offset, c.section(i).bytes);
}

Files bootFiles(const minijson::Value& e) {
  Files f;
  for (const auto& m : e["out"].members)
    f.emplace_back(m.first, cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first));
  return f;
}

Files scenarioFiles(const minijson::Value& sc) {
  const std::string dir = "stats/" + sc["in"].str() + "/";
  Files f;
  f.emplace_back("stats.bin", cardfixtures::bytes(dir + "stats.bin"));
  TEST_ASSERT_TRUE(!f[0].second.empty());
  const std::string head = sectionOf(f[0].second, sx::kStatsSpec, sf::kHead);
  const sf::Head h = sf::decodeHead(reinterpret_cast<const uint8_t*>(head.data()));
  if (h.baseGen) {
    char n[sx::kNameCap];
    TEST_ASSERT_TRUE(sx::baseName(h.baseGen, n, sizeof(n)));
    const std::string name = std::string(n).substr(kDir.size());
    f.emplace_back(name, cardfixtures::bytes(dir + name));
    TEST_ASSERT_TRUE(!f.back().second.empty());
  }
  return f;
}

// A card read as the boot reads it and ranked on `today` (-1: the clock
// unset), into the u40 set: with the lists (a visit's Full grade) or
// without (the lean ask's played-set-only rank); by the session of
// `liveRound` (-1: the fold's, a session that boots on the card).
struct Ranked {
  cutfs::CutFs card;
  sf::Fold fold;
  Block work{sx::kWorkBytes};
  Block lists{sr::Ranker::kBytes};
  std::unique_ptr<Block> windows;
  std::unique_ptr<Block> rows40;
  sr::Played40 played40;
  sr::Ranker ranker;
  sr::Report rep;
};

std::unique_ptr<Ranked> rankOf(const Files& files, int today, bool lean, const std::string& what,
                               int64_t liveRound = -1) {
  std::unique_ptr<Ranked> r(new Ranked());
  for (const auto& f : files) r->card.put(kDir + f.first, std::vector<uint8_t>(f.second.begin(), f.second.end()));
  sx::Store store(r->card);
  TEST_ASSERT_TRUE_MESSAGE(sx::statsOn(store.read(r->fold, r->work.p(), nullptr).read), what.c_str());
  sr::Input in;
  in.present = store.cardCopy().present;
  in.generation = store.cardCopy().generation;
  in.headerCrc = store.cardCopy().headerCrc;
  in.baseGen = r->fold.head().baseGen;
  in.baseRows = r->fold.head().baseRows;
  in.baseCrc = r->fold.head().baseCrc;
  in.hasToday = today >= 0;
  in.today = static_cast<uint16_t>(today >= 0 ? today : 0);
  in.liveRound = liveRound < 0 ? r->fold.liveRound() : static_cast<uint32_t>(liveRound);
  const uint32_t rows = in.baseRows + 4096;
  sr::Memory mem;
  mem.work = r->work.p();
  if (!lean) {
    mem.lists = r->lists.p();
    if (in.hasToday) {
      r->windows.reset(new Block(sr::Windows::bytesFor(4096 + 64)));
      mem.windows = r->windows->p();
      mem.windowRows = 4096 + 64;
    }
  }
  r->rows40.reset(new Block(sr::Played40::bytesFor(rows)));
  r->played40.rows = r->rows40->p();
  r->played40.cap = rows;
  mem.played40 = &r->played40;
  r->rep = sr::rank(r->card, in, mem, r->ranker);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(r->rep.outcome), what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r->played40.lost, what.c_str());
  return r;
}

// The ask's own facts for write(): made up, but for the Ranker's today.
sv::Context contextOf(uint32_t albums, uint32_t liveRound, uint32_t version) {
  sv::Context c;
  c.grade = sv::Grade::Full;
  c.buildStamp = 0x0123456789abcdefull ^ albums;
  c.trackCount = 19417;
  c.albums = albums;
  c.time = 27731000 + version;
  c.liveRound = liveRound;
  c.sinceDay = 245;
  c.window = 1;
  c.version = version;
  return c;
}

uint16_t dayOr(const minijson::Value& v) { return v.isNull() ? sf::kUnsetDay : static_cast<uint16_t>(v.u64()); }

// rank.json's totals for list `l` (null: a list the clock unset can't give).
const minijson::Value& totalOf(const minijson::Value& t, sr::List l) {
  switch (l) {
    case sr::kMost7: return t["mostPlayed"]["7"];
    case sr::kMost30: return t["mostPlayed"]["30"];
    case sr::kMostAll: return t["mostPlayed"]["all"];
    case sr::kNewFinds: return t["newFinds"];
    case sr::kOldFavourites: return t["oldFavourites"];
    default: return t["recentlyPlayed"];
  }
}
const minijson::Value& listOf(const minijson::Value& ls, sr::List l) { return totalOf(ls, l); }

std::string hx(uint64_t h) {
  char b[24];
  snprintf(b, sizeof(b), "0x%016llx", static_cast<unsigned long long>(h));
  return b;
}

}  // namespace

// ---------------------------------------------------------------------------
// The snapshot's bytes (9.7's table)
// ---------------------------------------------------------------------------
void test_the_snapshot_s_bytes() {
  TEST_ASSERT_EQUAL_UINT32(160, sizeof(sv::Snapshot));
  TEST_ASSERT_EQUAL_UINT32(160, sv::kHeaderBytes);
  // Each list's room (its cap and 16) and stride: 792, 264, 1,296 and
  // 1,080 B.
  const uint32_t strides[sr::kLists] = {4, 4, 4, 4, 6, 5};
  const uint32_t at[sr::kLists] = {160, 424, 688, 952, 1216, 2512};
  for (sr::List l : kAll) {
    TEST_ASSERT_EQUAL_UINT32(strides[l], sv::kStrides[l]);
    TEST_ASSERT_EQUAL_UINT32(sr::rowsOf(l), sv::roomOf(l, 1706));
    TEST_ASSERT_EQUAL_UINT32(at[l], sv::offsetOf(l, 1706));
  }
  TEST_ASSERT_EQUAL_UINT32(3 * 66 * 4, sv::offsetOf(sr::kNewFinds, 1706) - sv::offsetOf(sr::kMost7, 1706));
  TEST_ASSERT_EQUAL_UINT32(1296, sv::offsetOf(sr::kRecent, 1706) - sv::offsetOf(sr::kOldFavourites, 1706));
  // Not played yet: ceil(albums / 8) + 2 x ceil(albums / 64), 268 B at the
  // spare's 1,706 albums.
  TEST_ASSERT_EQUAL_UINT32(3592, sv::bytesFor(0));
  TEST_ASSERT_EQUAL_UINT32(3592 + 1 + 2, sv::bytesFor(1));
  TEST_ASSERT_EQUAL_UINT32(3592 + 8 + 2, sv::bytesFor(64));
  TEST_ASSERT_EQUAL_UINT32(3592 + 9 + 4, sv::bytesFor(65));
  TEST_ASSERT_EQUAL_UINT32(3860, sv::bytesFor(1706));
  TEST_ASSERT_EQUAL_UINT32(3867, sv::bytesFor(1747));
  // The 4,096 B bound: 3,216 albums fill it whole; from 3,217 Old favourites
  // and Recently played each give up as few last rows as keep it (11 B a
  // pair), down to none.
  TEST_ASSERT_EQUAL_UINT32(4096, sv::bytesFor(3216));
  TEST_ASSERT_EQUAL_UINT32(0, sv::trimFor(3216));
  // Every album count to 70,000: the first trimmed, the first with all 216
  // rows given up, the first past the bound.
  uint32_t firstTrim = 0, allGiven = 0, firstPast = 0;
  for (uint32_t a = 0; a <= 70000; ++a) {
    const size_t bytes = sv::bytesFor(a);
    if (a > sv::kMaxAlbums) {
      TEST_ASSERT_EQUAL_UINT32(0, bytes);  // (no library an ask takes)
      continue;
    }
    const uint32_t t = sv::trimFor(a);
    const size_t npy = (a + 7) / 8 + 2 * ((a + 63) / 64);
    TEST_ASSERT_EQUAL_UINT32(3592 - 11 * t + npy, bytes);
    TEST_ASSERT_EQUAL_UINT32(216 - t, sv::roomOf(sr::kOldFavourites, a));
    TEST_ASSERT_EQUAL_UINT32(216 - t, sv::roomOf(sr::kRecent, a));
    TEST_ASSERT_EQUAL_UINT32(66, sv::roomOf(sr::kNewFinds, a));
    TEST_ASSERT_EQUAL_UINT32(sv::offsetOf(sr::kOldFavourites, a) + (216 - t) * 6, sv::offsetOf(sr::kRecent, a));
    if (t < 216) {
      TEST_ASSERT_TRUE(bytes <= sv::kBound);
      if (t > 0) TEST_ASSERT_TRUE(bytes + 11 > sv::kBound);  // (the fewest rows given)
    } else {
      if (!allGiven) allGiven = a;
      if (bytes > sv::kBound && !firstPast) firstPast = a;  // (no rows left to give: past any library an ask takes)
    }
    if (t > 0 && !firstTrim) firstTrim = a;
  }
  TEST_ASSERT_EQUAL_UINT32(3217, firstTrim);
  TEST_ASSERT_EQUAL_UINT32(1, sv::trimFor(3217));
  TEST_ASSERT_EQUAL_UINT32(2, sv::trimFor(3300));
  TEST_ASSERT_EQUAL_UINT32(4087, sv::bytesFor(3300));
  // 18,432 albums the last the bound holds: 3,592 + 2,304 + 2 x 288 - 216
  // x 11 = 4,096 B; 18,433 albums 4,099 B.
  TEST_ASSERT_EQUAL_UINT32(216, sv::trimFor(18432));
  TEST_ASSERT_EQUAL_UINT32(4096, sv::bytesFor(18432));
  TEST_ASSERT_EQUAL_UINT32(4099, sv::bytesFor(18433));
  TEST_ASSERT_EQUAL_UINT32(18433, firstPast);
  TEST_ASSERT_EQUAL_UINT32(18361, allGiven);  // (2,870 B of bits and directory: 216 rows' 2,376 B under)
  TEST_ASSERT_EQUAL_UINT32(1216 + 8192 + 2048, sv::bytesFor(sv::kMaxAlbums));
  // A list number past the six: no room, no offset, no rows.
  for (uint32_t bad : {static_cast<uint32_t>(sr::kLists), 7u, 200u, 255u}) {
    const sr::List l = static_cast<sr::List>(bad);
    TEST_ASSERT_EQUAL_UINT32(0, sv::roomOf(l, 1706));
    TEST_ASSERT_EQUAL_UINT32(0, sv::offsetOf(l, 1706));
  }
  printf("[visit] the snapshot: 160 B header, 3,592 B of lists; 3,860 B at 1,706 albums, 3,867 B at 1,747; 4,096 B "
         "from 3,216 albums, the last rows of Old favourites and Recently played given up from %u, all 216 from %u, "
         "past 4,096 B from %u\n",
         static_cast<unsigned>(firstTrim), static_cast<unsigned>(allGiven), static_cast<unsigned>(firstPast));
}

// ---------------------------------------------------------------------------
// write() and resolve() over the rank scenarios
// ---------------------------------------------------------------------------
// Every rank scenario (stats/rank.json: each list past its cap), its Ranker
// copied out (write()) into a block never written (zeros, then a pattern)
// and one an earlier try left part-written (another scenario's write, cut
// short): the same bytes. The header from the Ranker, the report and the
// ask (statsref's totals and 7 days); each kept row's wanted index and
// facts. Then the walk's ids, made up (orphans and collisions among them),
// resolved: each list the Ranker's less the rows dropped, cut to its cap;
// the totals less the rows dropped among the kept ones.
void test_write_and_resolve_on_the_rank_scenarios() {
  std::string earlier;  // the previous scenario's block, as written
  uint32_t scenarios = 0, dropped = 0;
  for (size_t s = 0; s < gRank.size(); ++s) {
    const minijson::Value& sc = gRank[s];
    const std::string what = sc["name"].str();
    // (With the clock unset, by the scenario's session: rank.json's
    // liveRound.)
    const std::unique_ptr<Ranked> r =
        rankOf(scenarioFiles(sc), todayOf(sc["today"]), false, what,
               sc.has("liveRound") ? static_cast<int64_t>(sc["liveRound"].u64()) : -1);
    const sr::Ranker& k = r->ranker;
    WantedRegion wr(k);
    // The live round: dated, the newest undated Recently played row's (so
    // This session and Earlier both come up), else one no row has; undated,
    // the rank's own (the ask gives the copy-out the rank's: finishVisit()).
    uint32_t live = 0xABCDE;
    for (uint32_t i = 0; i < k.size(sr::kRecent) && k.hasToday(); ++i)
      if (k.at(sr::kRecent, i).at == 0) {
        live = k.at(sr::kRecent, i).ord;
        break;
      }
    if (!k.hasToday()) live = k.liveRound();
    const uint32_t albums = 1706;
    const sv::Context ctx = contextOf(albums, live, static_cast<uint32_t>(s) + 1);
    Block zero(sv::bytesFor(albums)), pattern(sv::bytesFor(albums)), part(sv::bytesFor(albums));
    pattern.fill(0x5A);
    if (!earlier.empty()) {
      std::memcpy(part.p(), earlier.data(), std::min(earlier.size(), part.bytes));
      std::memset(part.p() + 1500, 0xC3, 700);  // (cut short: a yield, Unread)
    } else {
      part.fill(0xFF);
    }
    TEST_ASSERT_TRUE_MESSAGE(sv::write(k, wr.w, r->rep, ctx, snapOf(zero), zero.bytes), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(sv::write(k, wr.w, r->rep, ctx, snapOf(pattern), pattern.bytes), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(sv::write(k, wr.w, r->rep, ctx, snapOf(part), part.bytes), what.c_str());
    TEST_ASSERT_TRUE_MESSAGE(zero.str() == pattern.str(), (what + ": rewritten whole over a pattern").c_str());
    TEST_ASSERT_TRUE_MESSAGE(zero.str() == part.str(), (what + ": rewritten whole over an earlier try").c_str());
    TEST_ASSERT_TRUE(zero.guarded() && pattern.guarded() && part.guarded());
    earlier = zero.str();
    const sv::Snapshot& sn = *snapOf(zero);
    // The header.
    TEST_ASSERT_TRUE(sn.valid());
    TEST_ASSERT_TRUE(sn.written());
    TEST_ASSERT_FALSE(sn.resolved());
    TEST_ASSERT_FALSE(sn.hasNotPlayed());
    TEST_ASSERT_EQUAL(k.hasToday(), sn.dated());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sv::Grade::Full), static_cast<int>(sn.grade()));
    TEST_ASSERT_TRUE(sn.matches(ctx.buildStamp, ctx.trackCount));
    TEST_ASSERT_FALSE(sn.matches(ctx.buildStamp, ctx.trackCount + 1));
    TEST_ASSERT_FALSE(sn.matches(ctx.buildStamp + 1, ctx.trackCount));
    TEST_ASSERT_EQUAL_UINT32(k.hasToday() ? ctx.time : 0, sn.time());
    TEST_ASSERT_EQUAL_UINT16(k.hasToday() ? k.today() : sf::kUnsetDay, sn.today());
    TEST_ASSERT_EQUAL_UINT32(live, sn.liveRound());
    TEST_ASSERT_EQUAL_UINT16(245, sn.sinceDay());
    TEST_ASSERT_EQUAL_UINT32(albums, sn.albums());
    TEST_ASSERT_EQUAL_UINT32(0, sn.notPlayedCount());
    TEST_ASSERT_EQUAL_UINT32(sv::offsetOf(sr::kRecent, albums) + 216 * 5, sn.bitsOffset());
    TEST_ASSERT_EQUAL_UINT32(sn.bitsOffset() + (albums + 7) / 8, sn.directoryOffset());
    TEST_ASSERT_EQUAL_UINT8(1, sn.window());
    TEST_ASSERT_EQUAL_UINT32(s + 1, sn.version());
    TEST_ASSERT_EQUAL_UINT32(sv::bytesFor(albums), sn.bytes());
    const minijson::Value& d7 = sc["days7"];
    TEST_ASSERT_EQUAL_UINT16(dayOr(d7["firstDay"]), sn.firstDay());
    TEST_ASSERT_EQUAL_UINT16(dayOr(d7["newestDay"]), sn.newestDay());
    TEST_ASSERT_EQUAL_UINT16(dayOr(d7["rcntFirstDay"]), sn.rcntFirstDay());
    TEST_ASSERT_EQUAL_UINT32(d7["spanDays"].u64(), sn.spanDays());
    for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
      const sr::Day d = sn.day(i);
      const minijson::Value& w = d7["days"][static_cast<size_t>(i)];
      const std::string m = what + " day " + std::to_string(i);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(d7["days"].isNull() ? 0 : w[0].u64(), d.listenedMs, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(d7["days"].isNull() ? 0 : w[1].u64(), d.plays, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(d7["days"].isNull() ? 0 : w[2].u64(), d.autoDjPlays, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(d7["days"].isNull() ? 0 : w[3].u64(), d.autoDjSkips, m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(r->rep.days[i].listenedMs, d.listenedMs, m.c_str());
    }
    // Each list: the Ranker's rows, its wanted row's index and its facts;
    // statsref's rows and totals.
    for (sr::List l : kAll) {
      const std::string m = what + " " + sr::listName(l);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(k.size(l), sn.rows(l), m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(k.total(l), sn.total(l), m.c_str());
      const minijson::Value& t = totalOf(sc["totals"], l);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(t.isNull() ? 0 : t.u64(), sn.total(l), m.c_str());
      const minijson::Value& want = listOf(sc["lists"], l);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.isNull() ? 0 : want.size(), sn.rows(l), m.c_str());
      for (uint32_t i = 0; i < sn.rows(l); ++i) {
        const sr::Entry& e = k.at(l, i);
        const sv::Row row = sn.rowAt(l, i);
        const std::string mi = m + " row " + std::to_string(i);
        const int64_t idx = wr.w.find(e.hash);
        TEST_ASSERT_TRUE_MESSAGE(idx >= 0, mi.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(idx), row.id, mi.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(row.id, cc::get16(sn.ids(l) + static_cast<size_t>(i) * sv::kStrides[l]),
                                         mi.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[static_cast<size_t>(i)][0].c_str(), hx(e.hash).c_str(), mi.c_str());
        sameFacts(l, e, row, live, k.hasToday(), mi);
      }
    }
    // Undated: Recently played's whens are the two codes alone, This
    // session's rows (the rank's: lastOrd its live round) first.
    if (!k.hasToday()) {
      uint32_t session = 0, others = 0;
      for (uint32_t i = 0; i < sn.rows(sr::kRecent); ++i) {
        const uint32_t when = sn.rowAt(sr::kRecent, i).when;
        const bool mine = k.at(sr::kRecent, i).ord == live;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(mine ? sv::kWhenSession : sv::kWhenEarlier, when, what.c_str());
        TEST_ASSERT_TRUE_MESSAGE(mine ? others == 0 : true, what.c_str());
        ++(mine ? session : others);
      }
      if (what == "rank-no-clock-session") TEST_ASSERT_EQUAL_UINT32(6, session);
      if (what == "rank-no-clock") TEST_ASSERT_EQUAL_UINT32(0, session);  // (a session that boots on it: none)
      TEST_ASSERT_TRUE(others > 0);
    }
    // A list number past the six: no ids, no rows, an empty row.
    for (uint32_t bad : {static_cast<uint32_t>(sr::kLists), 200u}) {
      const sr::List l = static_cast<sr::List>(bad);
      TEST_ASSERT_NULL(sn.ids(l));
      TEST_ASSERT_EQUAL_UINT32(0, sn.rows(l));
      TEST_ASSERT_EQUAL_UINT32(0, sn.total(l));
      TEST_ASSERT_EQUAL_UINT32(0, sn.rowAt(l, 0).id);
    }
    // The walk's ids, made up: an orphan each 5th wanted row, a collision
    // each 7th, else an id the index never gives the row's own place.
    for (uint32_t i = 0; i < wr.w.n; ++i)
      wr.w.setId(i, i % 5 == 3 ? sm::kNoId : i % 7 == 6 ? sm::kTwoTracks : 40000 + 3 * i);
    TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(zero)));
    TEST_ASSERT_TRUE(sn.resolved());
    TEST_ASSERT_TRUE(zero.guarded());
    for (sr::List l : kAll) {
      const std::string m = what + " " + sr::listName(l) + " resolved";
      std::vector<uint32_t> kept;  // the Ranker's rows left, by index
      uint32_t gone = 0;
      for (uint32_t i = 0; i < k.size(l); ++i) {
        const uint32_t id = wr.w.idAt(static_cast<uint32_t>(wr.w.find(k.at(l, i).hash)));
        if (id == sm::kNoId || id == sm::kTwoTracks) {
          ++gone;
        } else {
          kept.push_back(i);
        }
      }
      const uint32_t rows = std::min<uint32_t>(static_cast<uint32_t>(kept.size()), sr::kCaps[l]);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows, sn.rows(l), m.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(k.total(l) - gone, sn.total(l), m.c_str());
      for (uint32_t j = 0; j < rows; ++j) {
        const sr::Entry& e = k.at(l, kept[j]);
        const sv::Row row = sn.rowAt(l, j);
        const std::string mj = m + " row " + std::to_string(j);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(40000 + 3 * static_cast<uint32_t>(wr.w.find(e.hash)), row.id, mj.c_str());
        // The queue's id view over the list (7.6: a shelf acts through it)
        // reads the same id.
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(row.id, IdView::u16(sn.ids(l), sn.rows(l), sv::kStrides[l])[j], mj.c_str());
        sameFacts(l, e, row, live, k.hasToday(), mj);
      }
      // The rows past them zeroed (none left of the ask's indices).
      for (uint32_t j = rows; j < sv::roomOf(l, albums); ++j)
        for (uint32_t b = 0; b < sv::kStrides[l]; ++b)
          TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, sn.ids(l)[static_cast<size_t>(j) * sv::kStrides[l] + b], m.c_str());
      dropped += gone;
    }
    TEST_ASSERT_FALSE(sv::resolve(wr.w, snapOf(zero)));  // (once)
    // No orphan: each list its rows cut to the cap, the totals the Ranker's.
    TEST_ASSERT_TRUE(sv::write(k, wr.w, r->rep, ctx, snapOf(pattern), pattern.bytes));
    for (uint32_t i = 0; i < wr.w.n; ++i) wr.w.setId(i, i);
    TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(pattern)));
    for (sr::List l : kAll) {
      const sv::Snapshot& p = *snapOf(pattern);
      TEST_ASSERT_EQUAL_UINT32(std::min(k.size(l), sr::kCaps[l]), p.rows(l));
      TEST_ASSERT_EQUAL_UINT32(k.total(l), p.total(l));
      for (uint32_t j = 0; j < p.rows(l); ++j)
        TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(wr.w.find(k.at(l, j).hash)), p.rowAt(l, j).id);
    }
    ++scenarios;
  }
  TEST_ASSERT_EQUAL_UINT32(gRank.size(), scenarios);
  printf("[visit] write() and resolve() on %u rank scenarios: rewritten whole over a pattern and an earlier try; %u "
         "rows dropped as orphans or collisions\n",
         static_cast<unsigned>(scenarios), static_cast<unsigned>(dropped));
}

// The counts saturate (65,535; New finds' 16,383, its source in bits
// 14-15 beside it), and `when` is minutes since 2026-01-01 (saturating in
// 2057), 0xFFFFFF undated this session (lastOrd the live round), 0xFFFFFE
// undated earlier. A kept row whose hash isn't in the wanted set (no room
// there) gets no index, and resolve() drops it.
void test_saturation_and_when_s_codes() {
  Block mem(sr::Ranker::kBytes);
  sr::Ranker k;
  const uint16_t today = 320;
  const uint32_t live = 77;
  k.begin(mem.p(), true, today, live);
  auto timeAt = [](uint32_t day, uint32_t minute) { return day * 86400u + minute * 60u + 1u; };
  // New finds: plays past 16,383 and under, each source.
  for (uint32_t i = 0; i < 4; ++i) {
    sf::Track t;
    t.hash = 0x1000 + i;
    t.plays = static_cast<uint16_t>(i == 0 ? 65535 : i == 1 ? 16384 : i == 2 ? 16383 : 7);
    t.firstAt = timeAt(today - 3, i);
    t.lastAt = timeAt(today, i);
    t.lastOrd = 5;
    t.src = static_cast<uint8_t>(i % 3);
    k.track(t);
  }
  // Recently played: a play at 2026-01-01 00:00:59 (minute 0), one in the
  // last minute `when` holds, one past it (2058, dated to 2099), and
  // undated ones of this session and earlier.
  const uint32_t whenMaxAt = sv::kWhenMax * 60u + 59u + 1u;
  const uint32_t pastAt = (sv::kWhenMax + 1000u) * 60u + 1u;
  const struct {
    uint64_t hash;
    uint32_t lastAt, ord;
  } recent[] = {{0x2001, 60, 3}, {0x2002, whenMaxAt, 4}, {0x2003, pastAt, 5}, {0x2004, 0, live}, {0x2005, 0, live - 1}};
  for (const auto& x : recent) {
    sf::Track t;
    t.hash = x.hash;
    t.plays = 1;
    t.firstAt = x.lastAt;
    t.lastAt = x.lastAt;
    t.lastOrd = x.ord;
    k.track(t);
  }
  // Old favourites: 70,000 plays saturating, lastAt day 100.
  {
    sf::Track t;
    t.hash = 0x3001;
    t.plays = 65535;
    t.firstAt = timeAt(90, 0);
    t.lastAt = timeAt(100, 5);
    k.track(t);
  }
  // Most played's windows: sums past 65,535.
  k.windows(0x4001, 70000, today, 70000, today);
  k.windows(0x4002, 3, today, 9, today);
  k.finish();
  // The wanted set: every hash but 0x4002's (no room for it).
  sm::Wanted w;
  Block wb(sm::Wanted::bytesFor(64));
  TEST_ASSERT_TRUE(w.region(wb.p(), 64));
  for (sr::List l : kAll)
    for (uint32_t i = 0; i < k.size(l); ++i)
      if (k.at(l, i).hash != 0x4002) w.add(k.at(l, i).hash);
  w.seal();
  sv::Context ctx = contextOf(10, live, 1);
  Block b(sv::bytesFor(10));
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  const sv::Snapshot& sn = *snapOf(b);
  auto rowOf = [&](sr::List l, uint64_t hash) {
    for (uint32_t i = 0; i < k.size(l); ++i)
      if (k.at(l, i).hash == hash) return sn.rowAt(l, i);
    TEST_FAIL_MESSAGE("no such row");
    return sv::Row();
  };
  TEST_ASSERT_EQUAL_UINT32(0x3FFF, rowOf(sr::kNewFinds, 0x1000).count);
  TEST_ASSERT_EQUAL_UINT32(0, rowOf(sr::kNewFinds, 0x1000).src);
  TEST_ASSERT_EQUAL_UINT32(0x3FFF, rowOf(sr::kNewFinds, 0x1001).count);
  TEST_ASSERT_EQUAL_UINT32(sf::kShuffle, rowOf(sr::kNewFinds, 0x1001).src);
  TEST_ASSERT_EQUAL_UINT32(0x3FFF, rowOf(sr::kNewFinds, 0x1002).count);
  TEST_ASSERT_EQUAL_UINT32(sf::kAutoDj, rowOf(sr::kNewFinds, 0x1002).src);
  TEST_ASSERT_EQUAL_UINT32(7, rowOf(sr::kNewFinds, 0x1003).count);
  TEST_ASSERT_EQUAL_UINT32(sf::kYou, rowOf(sr::kNewFinds, 0x1003).src);
  TEST_ASSERT_EQUAL_UINT32(65535, rowOf(sr::kMostAll, 0x1000).count);
  TEST_ASSERT_EQUAL_UINT32(65535, rowOf(sr::kOldFavourites, 0x3001).count);
  TEST_ASSERT_EQUAL_UINT32(100, rowOf(sr::kOldFavourites, 0x3001).lastDay);
  TEST_ASSERT_EQUAL_UINT32(65535, rowOf(sr::kMost7, 0x4001).count);
  TEST_ASSERT_EQUAL_UINT32(65535, rowOf(sr::kMost30, 0x4001).count);
  TEST_ASSERT_EQUAL_UINT32(9, rowOf(sr::kMost30, 0x4002).count);
  TEST_ASSERT_EQUAL_UINT32(sv::kNoRow, rowOf(sr::kMost30, 0x4002).id);  // (not in the wanted set)
  TEST_ASSERT_EQUAL_UINT32(0, rowOf(sr::kRecent, 0x2001).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenMax, rowOf(sr::kRecent, 0x2002).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenMax, rowOf(sr::kRecent, 0x2003).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenSession, rowOf(sr::kRecent, 0x2004).when);
  TEST_ASSERT_EQUAL_UINT32(sv::kWhenEarlier, rowOf(sr::kRecent, 0x2005).when);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFF, sv::kWhenSession);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFE, sv::kWhenEarlier);
  // 2057: the last minute `when` holds is in it.
  TEST_ASSERT_EQUAL_UINT32(2057, 2026 + (sv::kWhenMax / (60 * 24)) / 365);
  // Recently played by minute (day 0): a row's time (seconds since 2026,
  // plus 1) 1 and 60 are minute 0, 61 to 120 minute 1.
  auto byMinute = [](sr::Ranker& r) {
    for (uint32_t s : {1u, 60u, 61u, 119u, 120u, 121u}) {
      sf::Track t;
      t.hash = 0x5000 + s;
      t.plays = 1;
      t.lastAt = s;  // (seconds since 2026, plus 1)
      t.firstAt = s;
      t.lastOrd = 5;
      r.track(t);
    }
  };
  {
    Block memd(sr::Ranker::kBytes);
    sr::Ranker kd;
    kd.begin(memd.p(), true, 0, live);
    byMinute(kd);
    kd.finish();
    WantedRegion wrd(kd);
    Block bd(sv::bytesFor(1));
    TEST_ASSERT_TRUE(sv::write(kd, wrd.w, sr::Report(), contextOf(1, live, 1), snapOf(bd), bd.bytes));
    TEST_ASSERT_TRUE(snapOf(bd)->dated());
    TEST_ASSERT_EQUAL_UINT32(6, snapOf(bd)->rows(sr::kRecent));
    const uint32_t minutes[] = {2, 1, 1, 1, 0, 0};  // newest first: 121, 120, 119, 61, 60, 1
    for (uint32_t i = 0; i < 6; ++i) TEST_ASSERT_EQUAL_UINT32(minutes[i], snapOf(bd)->rowAt(sr::kRecent, i).when);
  }
  // The clock unset (the owner's call, 2026-11-24): no minute, the two
  // codes alone. This session's rows (lastOrd the live round, whatever
  // lastAt) first, kWhenSession: a track played again with no clock (its
  // dated lastAt kept), then one never dated; then every other row
  // kWhenEarlier, the dated ones (by lastAt) and one never dated of the
  // round before.
  Block mem2(sr::Ranker::kBytes);
  sr::Ranker k2;
  k2.begin(mem2.p(), false, 0, live);
  byMinute(k2);
  const struct {
    uint64_t hash;
    uint32_t lastAt, ord;
  } undated[] = {{0x6001, 0, live}, {0x6002, 30, live}, {0x6003, 0, live - 1}};
  for (const auto& x : undated) {
    sf::Track t;
    t.hash = x.hash;
    t.plays = 2;
    t.firstAt = x.lastAt;
    t.lastAt = x.lastAt;
    t.lastOrd = x.ord;
    k2.track(t);
  }
  k2.finish();
  WantedRegion wr2(k2);
  Block b2(sv::bytesFor(1));
  TEST_ASSERT_TRUE(sv::write(k2, wr2.w, sr::Report(), contextOf(1, live, 1), snapOf(b2), b2.bytes));
  const sv::Snapshot& s2 = *snapOf(b2);
  TEST_ASSERT_FALSE(s2.dated());
  TEST_ASSERT_EQUAL_UINT32(0, s2.time());
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, s2.today());
  TEST_ASSERT_EQUAL_UINT32(live, s2.liveRound());
  TEST_ASSERT_EQUAL_UINT32(9, s2.rows(sr::kRecent));
  const uint64_t order[] = {0x6002, 0x6001, 0x5000 + 121, 0x5000 + 120, 0x5000 + 119,
                            0x5000 + 61, 0x5000 + 60, 0x5000 + 1, 0x6003};
  for (uint32_t i = 0; i < 9; ++i) {
    TEST_ASSERT_TRUE(k2.at(sr::kRecent, i).hash == order[i]);
    TEST_ASSERT_EQUAL(i < 2, k2.at(sr::kRecent, i).session);
    TEST_ASSERT_EQUAL_UINT32(i < 2 ? sv::kWhenSession : sv::kWhenEarlier, s2.rowAt(sr::kRecent, i).when);
  }
  // Every other list marks nothing; All time by plays as before.
  for (uint32_t i = 0; i < k2.size(sr::kMostAll); ++i) TEST_ASSERT_FALSE(k2.at(sr::kMostAll, i).session);
  for (sr::List l : {sr::kMost7, sr::kMost30, sr::kNewFinds, sr::kOldFavourites}) {
    TEST_ASSERT_EQUAL_UINT32(0, s2.rows(l));
    TEST_ASSERT_EQUAL_UINT32(0, s2.total(l));
  }
  // Live round 0 (none): no row this session's, every one Earlier.
  {
    Block mem0(sr::Ranker::kBytes);
    sr::Ranker k0;
    k0.begin(mem0.p(), false, 0, 0);
    byMinute(k0);
    sf::Track t;
    t.hash = 0x6004;
    t.plays = 1;  // (lastOrd 0: none has it, a play's round is 1 at least)
    k0.track(t);
    k0.finish();
    for (uint32_t i = 0; i < k0.size(sr::kRecent); ++i) TEST_ASSERT_FALSE(k0.at(sr::kRecent, i).session);
  }
  // resolve() drops the row with no index, and counts it out of the total.
  for (uint32_t i = 0; i < w.n; ++i) w.setId(i, i + 1);
  const uint32_t total30 = sn.total(sr::kMost30);
  TEST_ASSERT_TRUE(sv::resolve(w, snapOf(b)));
  TEST_ASSERT_EQUAL_UINT32(1, sn.rows(sr::kMost30));
  TEST_ASSERT_EQUAL_UINT32(total30 - 1, sn.total(sr::kMost30));
  // A library past 65,534 tracks gets no snapshot.
  ctx.trackCount = sv::kMaxTracks + 1;
  TEST_ASSERT_FALSE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  ctx.trackCount = sv::kMaxTracks;
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  ctx.albums = sv::kMaxAlbums + 1;
  TEST_ASSERT_FALSE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  // A held block (carved for 10 albums: 3,596 B) re-asked over an index
  // with more albums (a remount's, before update()'s key check frees it):
  // refused, nothing written, when the block is short by a byte or more;
  // taken when it holds them (16 albums: 3,596 B too).
  ctx.albums = 10;
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  const std::string held = b.str();
  TEST_ASSERT_EQUAL_UINT32(3596, b.bytes);
  for (uint32_t more : {17u, 65u, 3000u}) {
    ctx.albums = more;
    TEST_ASSERT_TRUE(sv::bytesFor(more) > b.bytes);
    TEST_ASSERT_FALSE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
    TEST_ASSERT_TRUE(held == b.str() && b.guarded());
  }
  ctx.albums = 10;
  TEST_ASSERT_FALSE(sv::write(k, w, sr::Report(), ctx, snapOf(b), 0));  // (no block)
  TEST_ASSERT_FALSE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes - 1));
  TEST_ASSERT_TRUE(held == b.str());
  ctx.albums = 16;
  TEST_ASSERT_EQUAL_UINT32(b.bytes, sv::bytesFor(16));
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  TEST_ASSERT_EQUAL_UINT32(16, sn.albums());
  TEST_ASSERT_TRUE(b.guarded());
  // The window: the UI's one write; a value past All time reads All time.
  ctx.albums = 10;
  TEST_ASSERT_TRUE(sv::write(k, w, sr::Report(), ctx, snapOf(b), b.bytes));
  snapOf(b)->setWindow(0);
  TEST_ASSERT_EQUAL_UINT8(0, sn.window());
  snapOf(b)->setWindow(9);
  TEST_ASSERT_EQUAL_UINT8(2, sn.window());
  TEST_ASSERT_TRUE(b.guarded() && b2.guarded());
}

// ---------------------------------------------------------------------------
// Not played yet: the bits in A-Z order, the directory, albumAt()
// ---------------------------------------------------------------------------
namespace {

std::unique_ptr<LibraryIndex> libraryOf(uint32_t albums, uint32_t perAlbum) {
  std::unique_ptr<LibraryIndex> idx(new LibraryIndex());
  TEST_ASSERT_TRUE(idx->begin("/music"));
  char b[96];
  // Artists and albums named against their order, so A-Z isn't the ids'.
  for (uint32_t a = 0; a < albums; ++a)
    for (uint32_t t = 0; t < perAlbum; ++t) {
      snprintf(b, sizeof(b), "/music/Artist %u/Album %05u/%02u.mp3", static_cast<unsigned>((a * 7919u) % 97u),
               static_cast<unsigned>((a * 104729u) % 99991u), static_cast<unsigned>(t + 1));
      idx->addFile(b);
    }
  TEST_ASSERT_TRUE(idx->finish());
  return idx;
}

uint64_t hashOf(const LibraryIndex& idx, uint32_t t) {
  char p[Walk::kPathBytes];
  uint64_t h = 0;
  TEST_ASSERT_TRUE(pathhash::trackPathHash(idx, t, p, sizeof(p), &h));
  return h;
}

sm::Map::Status runMap(sm::Map& m, const LibraryIndex* idx) {
  for (int i = 0; i < 10000000 && m.status() == sm::Map::Status::Working; ++i) m.step(idx);
  return m.status();
}

// The u40 set over `hashes`, as the rank writes it.
struct Played40Set {
  std::unique_ptr<Block> rows;
  sr::Played40 p;
  explicit Played40Set(std::vector<uint64_t> hashes) {
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    p.cap = static_cast<uint32_t>(hashes.size());
    rows.reset(new Block(sr::Played40::bytesFor(p.cap)));
    p.rows = rows->p();
    for (uint64_t h : hashes) {
      uint8_t* r = p.rows + static_cast<size_t>(p.n++) * sr::Played40::kRowBytes;
      const uint64_t key = sr::Played40::keyOf(h);
      cc::put32(r, static_cast<uint32_t>(key));
      r[4] = static_cast<uint8_t>(key >> 32);
    }
  }
};

// The not-played albums' A-Z positions, as the map gives them.
std::vector<uint32_t> notPlayedOf(const sm::Map& m, const LibraryIndex& idx) {
  std::vector<uint32_t> v;
  const LibraryIndex::Span az = idx.albumsAZ();
  for (uint32_t p = 0; p < az.count; ++p)
    if (!m.albumPlayed(az[p])) v.push_back(p);
  return v;
}

}  // namespace

// Libraries of 1 to 6,000 albums (around a byte's and a directory group's
// edges; past the trim's start), some albums played: notPlayed() writes the
// map's bits in albumsAZ()'s order (bits past the last album set), the
// directory's clear bits each 64 albums, the count; albumAt(k) is the k-th
// not-played album's A-Z position, none past the count. Nothing written
// past the block, and the lists' bytes untouched. Refused (nothing
// written): a map not Ready or over another index, a block for other
// albums or another key, a block not written.
void test_the_directory_and_album_at() {
  std::mt19937_64 rng(1706);
  for (uint32_t albums : {1u, 2u, 7u, 8u, 9u, 63u, 64u, 65u, 127u, 128u, 129u, 200u, 1706u, 3300u, 6000u}) {
    const std::string what = std::to_string(albums) + " albums";
    const auto idx = libraryOf(albums, 2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(albums, idx->albumCount(), what.c_str());
    // Played: about half the albums (one track of each), none or all for
    // the smallest.
    std::vector<uint64_t> played;
    for (uint32_t t = 0; t < idx->trackCount(); t += 2) {
      const bool on = albums <= 2 ? albums == 2 && t == 0 : rng() % 2 == 0;
      if (on) played.push_back(hashOf(*idx, t + static_cast<uint32_t>(rng() % 2)));
    }
    const Played40Set p40(played);
    Block bits(sm::albumBytes(albums));
    Block folders(Walk::bytesFor(Walk::foldersFor(*idx)));
    sm::Map m;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sm::Map::Status::Working),
                          static_cast<int>(m.begin(idx.get(), &p40.p, nullptr, bits.p(), folders.p())));
    // Not Ready: refused.
    Block b(sv::bytesFor(albums));
    Block mem(sr::Ranker::kBytes);
    sr::Ranker k;
    k.begin(mem.p(), true, 300, 0);
    for (uint32_t i = 0; i < 230; ++i) {
      sf::Track t;
      t.hash = 0x9000 + i;
      t.plays = static_cast<uint16_t>(5 + i % 9);
      t.firstAt = 86400u * 10 + i + 1;
      t.lastAt = 86400u * 200 + i + 1;
      k.track(t);
    }
    k.finish();
    WantedRegion wr(k);
    sv::Context ctx = contextOf(albums, 1, 1);
    ctx.buildStamp = idx->buildStamp();
    ctx.trackCount = idx->trackCount();
    TEST_ASSERT_FALSE(sv::notPlayed(m, *idx, snapOf(b)));  // (not written)
    TEST_ASSERT_TRUE(sv::write(k, wr.w, sr::Report(), ctx, snapOf(b), b.bytes));
    TEST_ASSERT_FALSE_MESSAGE(sv::notPlayed(m, *idx, snapOf(b)), what.c_str());  // (the walk not Ready)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(sm::Map::Status::Ready), static_cast<int>(runMap(m, idx.get())));
    for (uint32_t i = 0; i < wr.w.n; ++i) wr.w.setId(i, i);
    TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(b)));
    const std::string lists = b.str().substr(sv::kHeaderBytes, snapOf(b)->bitsOffset() - sv::kHeaderBytes);
    // Another key, another index, a block for other albums: refused.
    sv::Context other = ctx;
    other.trackCount = ctx.trackCount + 1;
    Block ob(sv::bytesFor(albums));
    TEST_ASSERT_TRUE(sv::write(k, wr.w, sr::Report(), other, snapOf(ob), ob.bytes));
    TEST_ASSERT_FALSE(sv::notPlayed(m, *idx, snapOf(ob)));
    other = ctx;
    other.albums = albums + 1;
    Block ob2(sv::bytesFor(albums + 1));
    TEST_ASSERT_TRUE(sv::write(k, wr.w, sr::Report(), other, snapOf(ob2), ob2.bytes));
    TEST_ASSERT_FALSE(sv::notPlayed(m, *idx, snapOf(ob2)));
    const auto idx2 = libraryOf(albums, 3);
    TEST_ASSERT_FALSE(sv::notPlayed(m, *idx2, snapOf(b)));
    TEST_ASSERT_FALSE(snapOf(b)->hasNotPlayed());
    // The bits.
    b.fill(0);
    TEST_ASSERT_TRUE(sv::write(k, wr.w, sr::Report(), ctx, snapOf(b), b.bytes));
    TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(b)));
    std::memset(b.p() + snapOf(b)->bitsOffset(), 0x77, b.bytes - snapOf(b)->bitsOffset());  // (any old bytes)
    TEST_ASSERT_TRUE_MESSAGE(sv::notPlayed(m, *idx, snapOf(b)), what.c_str());
    const sv::Snapshot& sn = *snapOf(b);
    TEST_ASSERT_TRUE(sn.hasNotPlayed());
    TEST_ASSERT_TRUE(b.guarded());
    TEST_ASSERT_TRUE_MESSAGE(lists == b.str().substr(sv::kHeaderBytes, sn.bitsOffset() - sv::kHeaderBytes),
                             what.c_str());  // (the lists untouched)
    const std::vector<uint32_t> want = notPlayedOf(m, *idx);
    TEST_ASSERT_TRUE_MESSAGE(!want.empty() || albums <= 2, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), sn.notPlayedCount(), what.c_str());
    const uint8_t* bitsAt = b.p() + sn.bitsOffset();
    const LibraryIndex::Span az = idx->albumsAZ();
    for (uint32_t p = 0; p < (albums + 7) / 8 * 8; ++p) {
      const bool set = (bitsAt[p / 8] >> (p % 8) & 1) != 0;
      TEST_ASSERT_EQUAL_MESSAGE(p >= albums || m.albumPlayed(az[p]), set, what.c_str());
    }
    const uint8_t* dir = b.p() + sn.directoryOffset();
    TEST_ASSERT_EQUAL_UINT32(sn.bitsOffset() + (albums + 7) / 8, sn.directoryOffset());
    TEST_ASSERT_EQUAL_UINT32(sv::bytesFor(albums), sn.directoryOffset() + 2 * ((albums + 63) / 64));
    for (uint32_t g = 0; g < (albums + 63) / 64; ++g) {
      uint32_t clear = 0;
      for (uint32_t pos : want) clear += pos / 64 == g ? 1 : 0;
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(clear, cc::get16(dir + 2 * g), what.c_str());
    }
    for (uint32_t j = 0; j < want.size(); ++j) TEST_ASSERT_EQUAL_UINT32_MESSAGE(want[j], sn.albumAt(j), what.c_str());
    TEST_ASSERT_EQUAL_UINT32(sv::kNoAlbum, sn.albumAt(static_cast<uint32_t>(want.size())));
    TEST_ASSERT_EQUAL_UINT32(sv::kNoAlbum, sn.albumAt(0xFFFFFFFFu));
    // A trimmed block (past 3,216 albums): the lists' rooms shorter, the
    // bits after them, nothing past the block.
    if (sv::trimFor(albums)) {
      TEST_ASSERT_EQUAL_UINT32(216 - sv::trimFor(albums), sv::roomOf(sr::kRecent, albums));
      TEST_ASSERT_TRUE(sv::bytesFor(albums) <= sv::kBound);
      TEST_ASSERT_EQUAL_UINT32(std::min(k.size(sr::kOldFavourites), std::min(sr::kCaps[sr::kOldFavourites],
                                                                               sv::roomOf(sr::kOldFavourites, albums))),
                               sn.rows(sr::kOldFavourites));
      TEST_ASSERT_EQUAL_UINT32(std::min(k.size(sr::kRecent), std::min(sr::kCaps[sr::kRecent],
                                                                       sv::roomOf(sr::kRecent, albums))),
                               sn.rows(sr::kRecent));
      TEST_ASSERT_EQUAL_UINT32(k.total(sr::kRecent), sn.total(sr::kRecent));  // (rows given up aren't orphans)
    }
    // write() again (a visit's re-ask): the bits gone, the flag clear.
    TEST_ASSERT_TRUE(sv::write(k, wr.w, sr::Report(), ctx, snapOf(b), b.bytes));
    TEST_ASSERT_FALSE(sn.hasNotPlayed());
    TEST_ASSERT_EQUAL_UINT32(0, sn.notPlayedCount());
    TEST_ASSERT_EQUAL_UINT32(sv::kNoAlbum, sn.albumAt(0));
  }
}

// ---------------------------------------------------------------------------
// The visit's whole way, on every fixture boot that names Not played yet
// ---------------------------------------------------------------------------
// The card ranked into the u40 set with the lists (the Full grade), the
// wanted set in one region (sealed), write(), the walk (the u40 set's album
// bits and the wanted set's ids, over stats/library.json's library: the
// fixtures' lists hold hashes it lacks, orphans), resolve() and
// notPlayed(): each list statsref's (index.json's) less the hashes the
// library lacks, cut to its cap, by track id (each id's hash the row's);
// the totals the Ranker's less them; Not played yet statsref's through
// albumAt(). Then the Lists grade (no bits) and a lean ask after it: its
// played-set-only rank and walk, its bits written into the held snapshot
// at its landing, the lists' bytes untouched, the same albums.
void test_the_fixture_boots_end_to_end() {
  minijson::Value lib;
  TEST_ASSERT_TRUE(cardfixtures::json("stats/library.json", &lib));
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (size_t a = 0; a < lib.size(); ++a)
    for (size_t t = 0; t < lib[a]["tracks"].size(); ++t)
      idx.addFile(("/music/" + lib[a]["tracks"][t]["path"].str()).c_str());
  TEST_ASSERT_TRUE(idx.finish());
  std::map<uint64_t, uint32_t> idOf;  // the library's hashes
  for (uint32_t t = 0; t < idx.trackCount(); ++t) idOf[hashOf(idx, t)] = t;
  uint32_t boots = 0, orphans = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& bootsV = gIndex[i]["boots"];
    for (size_t bi = 0; bi < bootsV.size(); ++bi) {
      const minijson::Value& e = bootsV[bi];
      if (!e.has("lists") || !e["lists"].has("notPlayed")) continue;
      const std::string what = gIndex[i]["name"].str() + " boot " + std::to_string(bi + 1);
      const Files files = bootFiles(e);
      const int today = todayOf(e["today"]);
      const std::unique_ptr<Ranked> r = rankOf(files, today, false, what);
      const sr::Ranker& k = r->ranker;
      WantedRegion wr(k);
      sv::Context ctx = contextOf(idx.albumCount(), 3, 1);
      ctx.buildStamp = idx.buildStamp();
      ctx.trackCount = idx.trackCount();
      Block b(sv::bytesFor(idx.albumCount()));
      b.fill(0xA7);
      TEST_ASSERT_TRUE(sv::write(k, wr.w, r->rep, ctx, snapOf(b), b.bytes));
      Block bits(sm::albumBytes(idx.albumCount()));
      Block folders(Walk::bytesFor(Walk::foldersFor(idx)));
      sm::Map m;
      m.begin(&idx, &r->played40, &wr.w, bits.p(), folders.p());
      TEST_ASSERT_EQUAL_INT(static_cast<int>(sm::Map::Status::Ready), static_cast<int>(runMap(m, &idx)));
      TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(b)));
      TEST_ASSERT_TRUE(sv::notPlayed(m, idx, snapOf(b)));
      TEST_ASSERT_TRUE(b.guarded());
      const sv::Snapshot& sn = *snapOf(b);
      for (sr::List l : kAll) {
        const std::string m2 = what + " " + sr::listName(l);
        const minijson::Value& want = listOf(e["lists"], l);
        std::vector<std::pair<uint64_t, uint32_t>> kept;  // statsref's rows the library has: hash, count
        uint32_t gone = 0;
        for (size_t j = 0; !want.isNull() && j < want.size(); ++j) {
          const uint64_t h = want[j][0].u64();
          if (idOf.count(h)) {
            kept.emplace_back(h, static_cast<uint32_t>(want[j][1].u64()));
          } else {
            ++gone;
          }
        }
        orphans += gone;
        const uint32_t rows = std::min<uint32_t>(static_cast<uint32_t>(kept.size()), sr::kCaps[l]);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows, sn.rows(l), m2.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(k.total(l) - gone, sn.total(l), m2.c_str());
        for (uint32_t j = 0; j < rows; ++j) {
          const sv::Row row = sn.rowAt(l, j);
          const std::string mj = m2 + " row " + std::to_string(j);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(idOf[kept[j].first], row.id, mj.c_str());
          TEST_ASSERT_TRUE_MESSAGE(hashOf(idx, row.id) == kept[j].first, mj.c_str());
          if (l != sr::kRecent) TEST_ASSERT_EQUAL_UINT32_MESSAGE(kept[j].second, row.count, mj.c_str());
        }
      }
      // Not played yet: statsref's albums, A-Z.
      const minijson::Value& npy = e["lists"]["notPlayed"];
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(npy.size(), sn.notPlayedCount(), what.c_str());
      for (uint32_t j = 0; j < sn.notPlayedCount(); ++j)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(npy[static_cast<size_t>(j)].c_str(),
                                         idx.albumName(idx.albumsAZ()[sn.albumAt(j)]), what.c_str());
      // The Lists grade, then a lean ask: its bits into the held snapshot at
      // its landing.
      const std::string full = b.str();
      ctx.grade = sv::Grade::Lists;
      Block held(sv::bytesFor(idx.albumCount()));
      TEST_ASSERT_TRUE(sv::write(k, wr.w, r->rep, ctx, snapOf(held), held.bytes));
      TEST_ASSERT_TRUE(sv::resolve(wr.w, snapOf(held)));
      TEST_ASSERT_FALSE(snapOf(held)->hasNotPlayed());
      const std::string listsOnly = held.str();
      const std::unique_ptr<Ranked> lean = rankOf(files, today, true, what + " (lean)");
      Block bits2(sm::albumBytes(idx.albumCount()));
      sm::Map m3;
      m3.begin(&idx, &lean->played40, nullptr, bits2.p(), nullptr);
      TEST_ASSERT_EQUAL_INT(static_cast<int>(sm::Map::Status::Ready), static_cast<int>(runMap(m3, &idx)));
      TEST_ASSERT_TRUE(sv::notPlayed(m3, idx, snapOf(held)));
      const sv::Snapshot& hs = *snapOf(held);
      TEST_ASSERT_TRUE(hs.hasNotPlayed());
      TEST_ASSERT_EQUAL_INT(static_cast<int>(sv::Grade::Lists), static_cast<int>(hs.grade()));
      const size_t from = sv::kHeaderBytes, n = hs.bitsOffset() - sv::kHeaderBytes;
      TEST_ASSERT_TRUE_MESSAGE(listsOnly.substr(from, n) == held.str().substr(from, n), what.c_str());
      for (sr::List l : kAll) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sn.rows(l), hs.rows(l), what.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(sn.total(l), hs.total(l), what.c_str());
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sn.notPlayedCount(), hs.notPlayedCount(), what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(full.substr(hs.bitsOffset()) == held.str().substr(hs.bitsOffset()), what.c_str());
      ++boots;
    }
  }
  TEST_ASSERT_TRUE(boots >= 25);
  printf("[visit] the visit's whole way on %u fixture boots: every list statsref's less %u rows the library lacks, "
         "Not played yet statsref's, a lean ask's bits the Full grade's\n",
         static_cast<unsigned>(boots), static_cast<unsigned>(orphans));
}

// ---------------------------------------------------------------------------
// The grades (9.7's table)
// ---------------------------------------------------------------------------
namespace {

constexpr size_t kHead = 1488;  // the ask's head: 9.7's estimate (the firmware's sizeof(Ask) on the device)
constexpr uint32_t kSpareRows = 19079 + 46 + 40;     // base + TRKD + the records: 19,165
constexpr uint32_t kPRows = 19478 + 1365 + 40;       // P = 19,478 with TRKD 1,365: 20,883
constexpr uint32_t kCapRows = 19478 + 4096 + 40;     // TRKD's forced-compaction cap: 23,614
constexpr uint32_t kSmallRows = 2214 + 1365 + 40;    // P = 2,214
constexpr size_t kFolders = 2478 * 10;               // the spare's 2,478 folders
constexpr uint32_t kAlbums = 1706;

bool inside(const sv::Region& r, size_t bytes) { return r.bytes == 0 || r.at + r.bytes <= bytes; }
bool apart(const sv::Region& a, const sv::Region& b) {
  return a.bytes == 0 || b.bytes == 0 || a.at + a.bytes <= b.at || b.at + b.bytes <= a.at;
}

// Each region inside the block and 16-aligned; those live at once apart:
// the head from all; during the rank the work memory, the Ranker, the
// window, the records and the played set; after it the played set, the
// folders, the bits and the wanted set; and the wanted set (built from the
// Ranker's rows, before the folders go over them) apart from the Ranker.
void wellMade(const sv::Layout& y, const std::string& what) {
  const sv::Region* all[] = {&y.head, &y.work, &y.ranker, &y.windows, &y.records, &y.played, &y.folders, &y.bits,
                             &y.wanted};
  for (const sv::Region* r : all) {
    TEST_ASSERT_TRUE_MESSAGE(inside(*r, y.bytes), what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r->at % 16, what.c_str());
    if (r != &y.head) TEST_ASSERT_TRUE_MESSAGE(apart(y.head, *r), what.c_str());
  }
  const sv::Region* rank[] = {&y.work, &y.ranker, &y.windows, &y.records};
  for (const sv::Region* a : rank)
    for (const sv::Region* b : rank)
      if (a != b) TEST_ASSERT_TRUE_MESSAGE(apart(*a, *b), what.c_str());
  for (const sv::Region* a : rank) {
    // The played set: written while the rank reads, but over the dead
    // window's rows (V1's order).
    if (a != &y.windows) TEST_ASSERT_TRUE_MESSAGE(apart(*a, y.played), what.c_str());
  }
  const sv::Region* walk[] = {&y.played, &y.folders, &y.bits, &y.wanted};
  for (const sv::Region* a : walk)
    for (const sv::Region* b : walk)
      if (a != b) TEST_ASSERT_TRUE_MESSAGE(apart(*a, *b), what.c_str());
  TEST_ASSERT_TRUE_MESSAGE(apart(y.ranker, y.wanted), what.c_str());
}

}  // namespace

// layout() against 9.7's grade table: the spare's column (19,165 played
// rows) and P = 19,478's (20,883), with and without a clock (the window's
// room), at TRKD's cap (23,614 rows) and at P = 2,214 (the window outweighs
// the played set); the parts as 9.7 gives them. The head is 9.7's estimate,
// 1,488 B, and the work memory the host's: 11,488 B, the device's 11,456 B
// (5.14's R4: the host's Scratch is 32 B larger). Every grade moves with
// the firmware's sizeof(Ask) byte for byte, and a grade whose room the work
// memory sets (Lists with the clock, No folders) with the work memory: the
// table's figures are the device's.
void test_the_layouts_against_the_grade_table() {
  const size_t dw = sx::kWorkBytes - 11456;  // the host's over the device's
  TEST_ASSERT_EQUAL_UINT32(32, dw);
  TEST_ASSERT_EQUAL_UINT32(16704, sr::Ranker::kBytes);
  TEST_ASSERT_EQUAL_UINT32(8352, sm::Wanted::bytesFor(sr::Ranker::kEntries));
  TEST_ASSERT_EQUAL_UINT32(95840, sr::Played40::bytesFor(kSpareRows));
  TEST_ASSERT_EQUAL_UINT32(104416, sr::Played40::bytesFor(kPRows));
  const struct {
    sv::Grade g;
    uint32_t rows;
    bool dated;
    size_t bytes;  // the device's
    bool work;     // the work memory sets the room (the host's 32 B more)
    const char* what;
  } table[] = {
      {sv::Grade::Full, kSpareRows, true, 130688, false, "Full, the spare"},
      {sv::Grade::Full, kPRows, true, 139264, false, "Full, P = 19,478"},
      {sv::Grade::Full, kSpareRows, false, 130688, false, "Full, the spare, no clock"},
      {sv::Grade::Full, kPRows, false, 139264, false, "Full, P = 19,478, no clock"},
      {sv::Grade::Lists, kSpareRows, true, 81360, true, "Lists, the spare"},
      {sv::Grade::Lists, kPRows, true, 81360, true, "Lists, P = 19,478"},
      {sv::Grade::Lists, kSpareRows, false, 34624, false, "Lists, the spare, no clock"},
      {sv::Grade::Lists, kPRows, false, 34624, false, "Lists, P = 19,478, no clock"},
      {sv::Grade::Lean, kSpareRows, true, 122336, false, "Lean, the spare"},
      {sv::Grade::Lean, kPRows, true, 130912, false, "Lean, P = 19,478"},
      {sv::Grade::Lean, kSpareRows, false, 122336, false, "Lean, the spare, no clock"},
      {sv::Grade::Lean, kPRows, false, 130912, false, "Lean, P = 19,478, no clock"},
      {sv::Grade::NoFolders, kSpareRows, true, 109744, true, "No folders, the spare"},
      {sv::Grade::NoFolders, kPRows, true, 118320, true, "No folders, P = 19,478"},
      {sv::Grade::NoFolders, kSpareRows, false, 109744, true, "No folders, the spare, no clock"},
      {sv::Grade::NoFolders, kPRows, false, 118320, true, "No folders, P = 19,478, no clock"},
      {sv::Grade::Full, kCapRows, true, 152928, false, "Full, TRKD's cap"},
      {sv::Grade::Lean, kCapRows, true, 144576, false, "Lean, TRKD's cap"},
      {sv::Grade::NoFolders, kCapRows, true, 131984, true, "No folders, TRKD's cap"},
      {sv::Grade::Full, kSmallRows, true, 84480, false, "Full, P = 2,214"},
      {sv::Grade::Full, kSmallRows, false, 1488 + 33360 + 18096, false, "Full, P = 2,214, no clock"},
  };
  for (const auto& row : table) {
    const sv::Layout y = sv::layout(row.g, kHead, row.rows, kFolders, kAlbums, row.dated);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(row.bytes + (row.work ? dw : 0), y.bytes, row.what);
    wellMade(y, row.what);
    printf("[visit] %-34s %7u B on the device (%u B here)\n", row.what, static_cast<unsigned>(row.bytes),
           static_cast<unsigned>(y.bytes));
  }
  // The parts, as 9.7 names them.
  const sv::Layout full = sv::layout(sv::Grade::Full, kHead, kSpareRows, kFolders, kAlbums, true);
  TEST_ASSERT_EQUAL_UINT32(0, full.head.at);
  TEST_ASSERT_EQUAL_UINT32(1488, full.head.bytes);
  TEST_ASSERT_EQUAL_UINT32(11456 + dw, full.work.bytes);
  TEST_ASSERT_EQUAL_UINT32(16704, full.ranker.bytes);
  TEST_ASSERT_EQUAL_UINT32(49632, full.windows.bytes);
  TEST_ASSERT_EQUAL_UINT32(4136, full.windowRows);
  TEST_ASSERT_EQUAL_UINT32(2080, full.records.bytes);
  TEST_ASSERT_EQUAL_UINT32(24784, full.folders.bytes);
  TEST_ASSERT_EQUAL_UINT32(224, full.bits.bytes);
  TEST_ASSERT_EQUAL_UINT32(8352, full.wanted.bytes);
  TEST_ASSERT_EQUAL_UINT32(696, full.wantedRows);
  TEST_ASSERT_EQUAL_UINT32(95840, full.played.bytes);
  TEST_ASSERT_EQUAL_UINT32(kSpareRows, full.playedRows);
  // A (33,360 B) then PW (95,840 B): the Ranker at A's head, the wanted set
  // at its tail, the folders and bits at its head over the dead Ranker; the
  // window, then the played set, at PW.
  TEST_ASSERT_EQUAL_UINT32(1488, full.ranker.at);
  TEST_ASSERT_EQUAL_UINT32(1488, full.folders.at);
  TEST_ASSERT_EQUAL_UINT32(1488 + 33360 - 8352, full.wanted.at);
  TEST_ASSERT_EQUAL_UINT32(1488 + 33360, full.windows.at);
  TEST_ASSERT_EQUAL_UINT32(1488 + 33360, full.played.at);
  // No clock: no window; Lists: no played set, no bits.
  const sv::Layout fullNoClock = sv::layout(sv::Grade::Full, kHead, kSpareRows, kFolders, kAlbums, false);
  TEST_ASSERT_EQUAL_UINT32(0, fullNoClock.windows.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, fullNoClock.windowRows);
  const sv::Layout lists = sv::layout(sv::Grade::Lists, kHead, kSpareRows, kFolders, kAlbums, true);
  TEST_ASSERT_EQUAL_UINT32(0, lists.played.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, lists.bits.bytes);
  TEST_ASSERT_EQUAL_UINT32(24784, lists.folders.bytes);
  TEST_ASSERT_EQUAL_UINT32(49632, lists.windows.bytes);
  // The lean grades: no Ranker, no window, the TRKD records alone (960 B).
  const sv::Layout lean = sv::layout(sv::Grade::Lean, kHead, kSpareRows, kFolders, kAlbums, true);
  TEST_ASSERT_EQUAL_UINT32(0, lean.ranker.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, lean.windows.bytes);
  TEST_ASSERT_EQUAL_UINT32(0, lean.wanted.bytes);
  TEST_ASSERT_EQUAL_UINT32(960, lean.records.bytes);
  TEST_ASSERT_EQUAL_UINT32(24784, lean.folders.bytes);
  TEST_ASSERT_EQUAL_UINT32(224, lean.bits.bytes);
  const sv::Layout bare = sv::layout(sv::Grade::NoFolders, kHead, kSpareRows, kFolders, kAlbums, true);
  TEST_ASSERT_EQUAL_UINT32(0, bare.folders.bytes);
  TEST_ASSERT_EQUAL_UINT32(224, bare.bits.bytes);
  // No folders' states for the library (past kMaxFolders): Full's A is the
  // rank's room; the head moves every grade byte for byte.
  const sv::Layout noStates = sv::layout(sv::Grade::Full, kHead, kSpareRows, 0, kAlbums, true);
  TEST_ASSERT_EQUAL_UINT32(1488 + 30240 + dw + 95840, noStates.bytes);
  wellMade(noStates, "Full, no folders' states");
  for (sv::Grade g : {sv::Grade::Full, sv::Grade::Lists, sv::Grade::Lean, sv::Grade::NoFolders}) {
    const sv::Layout a = sv::layout(g, kHead, kPRows, kFolders, kAlbums, true);
    const sv::Layout b = sv::layout(g, kHead + 120, kPRows, kFolders, kAlbums, true);
    TEST_ASSERT_EQUAL_UINT32(a.bytes + 128, b.bytes);  // (the head rounded to 16)
    TEST_ASSERT_EQUAL_STRING(sv::gradeName(g), sv::gradeName(a.grade));
  }
}

// ---------------------------------------------------------------------------
// phaseOf()
// ---------------------------------------------------------------------------
void test_phase_of() {
  using W = ScanScheduler::Wait;
  using O = sv::Outcome;
  using P = sv::Phase;
  // (`served`: the landed snapshot still held, snapshot() non-null; it
  // matters to Ok alone, below.)
  auto ph = [](uint8_t why, O o, W w, uint32_t ms, bool capped, bool served = true) {
    return static_cast<int>(sv::phaseOf(why, o, w, ms, capped, served));
  };
  const int working = static_cast<int>(P::Working), waiting = static_cast<int>(P::Waiting);
  // Under way, nothing holding it: Working, however long it ranks or walks.
  TEST_ASSERT_EQUAL_INT(working, ph(0, O::None, W::None, 0, false));
  TEST_ASSERT_EQUAL_INT(working, ph(0, O::None, W::None, 60000, false));
  // S2's whys 4, 8, 32 and 64, S3a's tag scan's 128 (its memory held: the
  // owner's "Only while held"), and each Rank-lane wait (a wait after the
  // take, its block given back, is the same): Working, then Waiting from
  // 3 s; never a refusal or a fault (nothing counted).
  const uint8_t whys[] = {4, 8, 32, 64, 128, 4 | 64, 8 | 32, 64 | 128, 8 | 128};
  for (uint8_t why : whys) {
    TEST_ASSERT_EQUAL_INT(working, ph(why, O::None, W::None, 0, false));
    TEST_ASSERT_EQUAL_INT(working, ph(why, O::None, W::None, 2999, false));
    TEST_ASSERT_EQUAL_INT(waiting, ph(why, O::None, W::None, 3000, false));
    TEST_ASSERT_EQUAL_INT(waiting, ph(why, O::None, W::None, 600000, false));
  }
  // The boot's scan (2): Working however long it holds (4.4's Folding,
  // "Counting plays…"), a Rank-lane wait or a later gate beside it or not
  // (S2 gives 2 before them all).
  for (int w = 0; w < ScanScheduler::kWaits; ++w)
    for (uint32_t ms : {0u, 2999u, 3000u, 600000u})
      for (uint8_t why : {2, 2 | 4, 2 | 64, 2 | 8 | 32, 2 | 128}) {
        TEST_ASSERT_EQUAL_INT(working, ph(why, O::None, static_cast<W>(w), ms, false));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Capped), ph(why, O::None, static_cast<W>(w), ms, true));
      }
  for (int w = 1; w < ScanScheduler::kWaits; ++w) {
    const W wait = static_cast<W>(w);
    TEST_ASSERT_EQUAL_INT(working, ph(0, O::None, wait, 0, false));
    TEST_ASSERT_EQUAL_INT(working, ph(0, O::None, wait, 2999, false));
    TEST_ASSERT_EQUAL_INT(waiting, ph(0, O::None, wait, 3000, false));
    TEST_ASSERT_EQUAL_INT(waiting, ph(32, O::None, wait, 3000, false));
    TEST_ASSERT_EQUAL_INT(working, ph(128, O::None, wait, 2999, false));
    TEST_ASSERT_EQUAL_INT(waiting, ph(128, O::None, wait, 3000, false));
  }
  TEST_ASSERT_EQUAL_UINT32(3000, sv::kWaitingMs);
  // The stats off (why 1): Idle, whatever else. The card away or held (16):
  // NoCard, whatever else but the stats off.
  for (int o = 0; o <= static_cast<int>(O::CardAway); ++o) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(1, static_cast<O>(o), W::Input, 5000, true));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(1 | 16, static_cast<O>(o), W::None, 0, false));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoCard), ph(16, static_cast<O>(o), W::None, 0, false));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoCard), ph(16 | 8, static_cast<O>(o), W::Seek, 9000, true));
  }
  // An ask the card cancelled (CardAway) stays NoCard once the card is
  // back (16 gone), until the next ask: the page offers Try again there
  // (9.7's "the card back"; 4.4's No card), never Idle.
  for (int w = 0; w < ScanScheduler::kWaits; ++w)
    for (uint8_t why : {0, 2, 4, 8, 32, 64, 128})
      for (bool capped : {false, true})
        TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoCard), ph(why, O::CardAway, static_cast<W>(w), 9000, capped));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(1, O::CardAway, W::None, 0, false));
  TEST_ASSERT_EQUAL_STRING("card away", sv::outcomeName(O::CardAway));
  // Landed: Ready (its holds done; the gates are for an ask in Asked),
  // while its snapshot is served. Freed since (release(), a console ask's
  // own, the key's free after a remount, Hu0): Idle, never Ready with
  // snapshot() null (V4's check: a page read Ready over no lists).
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Ready), ph(0, O::Ok, W::None, 0, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Ready), ph(8, O::Ok, W::Input, 5000, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(0, O::Ok, W::None, 0, false, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(8, O::Ok, W::Input, 5000, false, false));
  // ... and the card's or the stats off's word over it, served or not.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoCard), ph(16, O::Ok, W::None, 0, false, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoCard), ph(0, O::CardAway, W::None, 0, false, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(1, O::Ok, W::None, 0, false, true));
  // `served` changes nothing but a landing's Ready: every other input
  // gives the same phase either way.
  for (int why = 0; why < 256; ++why)
    for (int o = 0; o <= static_cast<int>(O::CardAway); ++o)
      for (int w = 0; w < ScanScheduler::kWaits; ++w)
        for (uint32_t ms : {0u, 3000u})
          for (bool capped : {false, true}) {
            const int a = ph(static_cast<uint8_t>(why), static_cast<O>(o), static_cast<W>(w), ms, capped, true);
            const int b = ph(static_cast<uint8_t>(why), static_cast<O>(o), static_cast<W>(w), ms, capped, false);
            if (a == static_cast<int>(P::Ready)) {
              TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), b);
            } else {
              TEST_ASSERT_EQUAL_INT(a, b);
              TEST_ASSERT_TRUE(b != static_cast<int>(P::Ready));
            }
          }
  // Every grade refused (NoRoom, NoBlock, the rank's Room): NoRoom.
  for (O o : {O::NoRoom, O::NoBlock, O::Room}) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoRoom), ph(0, o, W::None, 0, false));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::NoRoom), ph(32, o, W::Ring, 4000, false));
  }
  // A read fault: Unread.
  for (O o : {O::Changed, O::Io, O::Damaged, O::Base})
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Unread), ph(0, o, W::None, 0, false));
  // The lit cap reached (on): Capped. Cancelled: Idle.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Capped), ph(0, O::None, W::None, 0, true));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Capped), ph(64, O::Cancelled, W::Input, 9000, true));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(0, O::Cancelled, W::None, 0, false));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Idle), ph(4, O::Cancelled, W::Bluetooth, 9000, false));
  // Unread for the read faults alone, every input: no wait, why, refusal,
  // time or cap gives it.
  uint32_t unread = 0, cases = 0;
  for (int why = 0; why < 256; ++why)
    for (int o = 0; o <= static_cast<int>(O::CardAway); ++o)
      for (int w = 0; w < ScanScheduler::kWaits; ++w)
        for (uint32_t ms : {0u, 2999u, 3000u, 100000u})
          for (bool capped : {false, true}) {
            const O out = static_cast<O>(o);
            const bool fault = out == O::Changed || out == O::Io || out == O::Damaged || out == O::Base;
            const bool want = fault && (why & 17) == 0;
            const int got = ph(static_cast<uint8_t>(why), out, static_cast<W>(w), ms, capped);
            TEST_ASSERT_EQUAL(want, got == static_cast<int>(P::Unread));
            // Waiting only for an ask in Asked held 3 s by a gate but the
            // stats off, the boot's scan and the card (1 | 2 | 16).
            TEST_ASSERT_TRUE(got != static_cast<int>(P::Waiting) ||
                             (out == O::None && !capped && (why & 19) == 0 && ms >= 3000 &&
                              (why != 0 || w != static_cast<int>(W::None))));
            unread += got == static_cast<int>(P::Unread) ? 1 : 0;
            ++cases;
          }
  TEST_ASSERT_TRUE(unread > 0);
  // The rank's outcomes, as the visit's.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Ok), static_cast<int>(sv::outcomeOf(sr::Outcome::Ok)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Changed), static_cast<int>(sv::outcomeOf(sr::Outcome::Changed)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Base), static_cast<int>(sv::outcomeOf(sr::Outcome::Base)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Io), static_cast<int>(sv::outcomeOf(sr::Outcome::Io)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Damaged), static_cast<int>(sv::outcomeOf(sr::Outcome::Damaged)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(O::Room), static_cast<int>(sv::outcomeOf(sr::Outcome::Room)));
  for (int p = 0; p <= static_cast<int>(P::Capped); ++p) TEST_ASSERT_NOT_NULL(sv::phaseName(static_cast<P>(p)));
  printf("[visit] phaseOf(): %u cases, Unread in %u (the read faults alone)\n", static_cast<unsigned>(cases),
         static_cast<unsigned>(unread));
}

// ---------------------------------------------------------------------------
// The card worker's give-up (stallHolds(), waitStartMoves())
// ---------------------------------------------------------------------------
// StatsLists' order at each pass for an S3a ask in Asked, over the
// firmware's own rules (the host can't build src/app): loop()'s gate
// (gateOf(): S2's other gates first, then the stall), the wait's start
// moved or not, the phase; then PlayHistory's turn in the same pass, with
// the same nowMs (turnWanted(): the same gate and no Rank-lane wait),
// which takes the ask and clears the stall. The give-up's own pass first
// (putBack(): the wait's start that pass, the stall set, loop()'s kAsked
// block after it).
namespace {
struct StallRun {
  uint32_t takenMs = 0;    // the take's pass (0: none in the run)
  uint32_t waitingMs = 0;  // the first pass the phase read Waiting (0: none)
  uint32_t backToWorking = 0;  // passes read Working after a Waiting, but the take's own
  uint32_t passes = 0;
};
// `other(now)`: S2's other gates' bits; `lane(now)`: the Rank lane's wait;
// `turn(now)`: PlayHistory's turn reached that pass (the write gate's);
// `stallKept`: false for the rule before this fix (loop() moved the start
// whenever nothing held the ask, the stall's own pass after its 30 s too).
template <typename Other, typename Lane, typename Turn>
StallRun runStall(uint32_t giveUpMs, uint32_t passMs, uint32_t passes, Other other, Lane lane, Turn turn,
                  bool stallKept = true) {
  StallRun r;
  uint32_t start = giveUpMs;
  bool stalled = true;
  auto gate = [&](uint32_t now) -> uint8_t {
    const uint8_t o = other(now);
    if (o) return o;
    return sv::stallHolds(stalled, now, start) ? sv::kWhyWorker : 0;
  };
  for (uint32_t i = 0; i <= passes; ++i) {
    const uint32_t now = giveUpMs + i * passMs;
    ++r.passes;
    const uint8_t why = gate(now);
    const ScanScheduler::Wait w = lane(now);
    if (sv::waitStartMoves(why, w, stallKept && stalled)) start = now;
    const sv::Phase p = sv::phaseOf(why, sv::Outcome::None, w, now - start, false, false);
    if (p == sv::Phase::Waiting && !r.waitingMs) r.waitingMs = now;
    if (turn(now) && gate(now) == 0 && w == ScanScheduler::Wait::None) {
      r.takenMs = now;  // (its pass Working: nothing holds it, loop()'s phase before the turn)
      return r;
    }
    if (p == sv::Phase::Working && r.waitingMs) ++r.backToWorking;
  }
  return r;
}
}  // namespace

void test_the_worker_s_give_up_holds_30_s_then_the_take() {
  using W = ScanScheduler::Wait;
  TEST_ASSERT_EQUAL_UINT32(30000, sv::kStallMs);
  // The rules alone: held from the give-up for kStallMs, not a ms more
  // (millis() wrapping between them too); never when not stalled.
  for (uint32_t t0 : {0u, 1000u, 0xFFFFFFFFu - 10000u}) {
    TEST_ASSERT_TRUE(sv::stallHolds(true, t0, t0));
    TEST_ASSERT_TRUE(sv::stallHolds(true, t0 + 29999, t0));
    TEST_ASSERT_FALSE(sv::stallHolds(true, t0 + 30000, t0));
    TEST_ASSERT_FALSE(sv::stallHolds(true, t0 + 600000, t0));
    TEST_ASSERT_FALSE(sv::stallHolds(false, t0, t0));
    TEST_ASSERT_FALSE(sv::stallHolds(false, t0 + 29999, t0));
  }
  // The start moves only at a pass nothing holds, and never while stalled.
  TEST_ASSERT_TRUE(sv::waitStartMoves(0, W::None, false));
  TEST_ASSERT_FALSE(sv::waitStartMoves(0, W::None, true));
  for (uint8_t why : {4, 8, 32, 64, 128})
    for (bool stalled : {false, true}) TEST_ASSERT_FALSE(sv::waitStartMoves(why, W::None, stalled));
  for (int w = 1; w < ScanScheduler::kWaits; ++w) {
    TEST_ASSERT_FALSE(sv::waitStartMoves(0, static_cast<W>(w), false));
    TEST_ASSERT_FALSE(sv::waitStartMoves(0, static_cast<W>(w), true));
  }
  auto none = [](uint32_t) -> uint8_t { return 0; };
  auto noLane = [](uint32_t) { return W::None; };
  auto always = [](uint32_t) { return true; };
  const uint32_t kTenMinutes = 60000;  // passes (10 minutes of 10 ms ones)
  // Nothing else holding it: Working, Waiting from 3 s, taken at the first
  // pass 30 s from the give-up (10 ms passes, and an odd 37 ms pace), and
  // with millis() wrapping under it.
  for (uint32_t t0 : {1000u, 0xFFFFFFFFu - 12345u}) {
    const StallRun a = runStall(t0, 10, kTenMinutes, none, noLane, always);
    TEST_ASSERT_EQUAL_UINT32(t0 + 30000, a.takenMs);
    TEST_ASSERT_EQUAL_UINT32(t0 + 3000, a.waitingMs);
    const StallRun b = runStall(t0, 37, kTenMinutes, none, noLane, always);
    TEST_ASSERT_EQUAL_UINT32(t0 + (30000 + 36) / 37 * 37, b.takenMs);
  }
  const uint32_t t0 = 5000;
  // A Rank-lane wait across the stall's end (input from 29 s to 31.5 s):
  // taken at its first pass clear; Waiting throughout (the wait's start
  // still the give-up's), never Working again before the take.
  {
    auto input = [t0](uint32_t now) { return now - t0 >= 29000 && now - t0 < 31500 ? W::Input : W::None; };
    const StallRun r = runStall(t0, 10, kTenMinutes, none, input, always);
    TEST_ASSERT_EQUAL_UINT32(t0 + 31500, r.takenMs);
    TEST_ASSERT_EQUAL_UINT32(t0 + 3000, r.waitingMs);
    TEST_ASSERT_EQUAL_UINT32(0, r.backToWorking);
  }
  // S2's other gate over it (Auto DJ's join, 20 s to 45 s): taken at once
  // when it clears, the stall's 30 s long run out.
  {
    auto join = [t0](uint32_t now) -> uint8_t { return now - t0 >= 20000 && now - t0 < 45000 ? 32 : 0; };
    const StallRun r = runStall(t0, 10, kTenMinutes, join, noLane, always);
    TEST_ASSERT_EQUAL_UINT32(t0 + 45000, r.takenMs);
    TEST_ASSERT_EQUAL_UINT32(0, r.backToWorking);
  }
  // The turn not reached where the stall runs out (the write gate holding
  // PlayHistory's turn from 29 s to 30.5 s): loop()'s passes don't restart
  // the 30 s meanwhile; taken at the turn's first pass back.
  {
    auto turn = [t0](uint32_t now) { return now - t0 < 29000 || now - t0 >= 30500; };
    const StallRun r = runStall(t0, 10, kTenMinutes, none, noLane, turn);
    TEST_ASSERT_EQUAL_UINT32(t0 + 30500, r.takenMs);
  }
  // The rule before this fix (the check of the review's fixes): loop()'s
  // pass that saw the 30 s run out moved the start, and the same pass's
  // turn held it again: no take in 10 minutes of passes, whatever else.
  {
    const StallRun r = runStall(t0, 10, kTenMinutes, none, noLane, always, false);
    TEST_ASSERT_EQUAL_UINT32(0, r.takenMs);
    TEST_ASSERT_EQUAL_UINT32(kTenMinutes + 1, r.passes);
  }
  printf("[visit] the worker's give-up: held 30 s from it, then taken (lane waits, S2's gates, a turn held after it); "
         "the old rule never took it in 10 minutes\n");
}

// The device pass's run 3a-6: a console ask stamps its start with millis()
// after the loop's pass took its nowMs, so that pass's loop() and phase read
// a start ahead of now ("held 4294967294 ms"). elapsedMs() reads 0 there, and
// the phase is Working for 3 s from the ask, as for any other ask.
void test_elapsed_ms_with_a_start_ahead_of_now() {
  using W = ScanScheduler::Wait;
  using O = sv::Outcome;
  using P = sv::Phase;
  TEST_ASSERT_EQUAL_UINT32(100, sv::elapsedMs(1000, 900));
  TEST_ASSERT_EQUAL_UINT32(0, sv::elapsedMs(1000, 1000));
  // A start a pass ahead (a console ask's millis(), the fence's re-ask).
  TEST_ASSERT_EQUAL_UINT32(0, sv::elapsedMs(1000, 1002));
  TEST_ASSERT_EQUAL_UINT32(0, sv::elapsedMs(1000, 1000 + 0x7FFFFFFFu));
  // millis() wrapping: a start just before the wrap, now just after it.
  TEST_ASSERT_EQUAL_UINT32(21, sv::elapsedMs(5, 0xFFFFFFF0u));
  TEST_ASSERT_EQUAL_UINT32(0x7FFFFFFFu, sv::elapsedMs(0x7FFFFFFFu, 0));
  // The phase over it: each wait but the boot's scan's reads Working for its
  // first 3 s (the start ahead read as 0 ms held), not Waiting at once.
  const uint8_t whys[] = {4, 8, 32, 64, 128};
  for (uint8_t why : whys) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Working),
                          static_cast<int>(sv::phaseOf(why, O::None, W::None, sv::elapsedMs(1000, 1002), false, false)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Waiting),
                          static_cast<int>(sv::phaseOf(why, O::None, W::None, sv::elapsedMs(4002, 1002), false, false)));
  }
  for (int w = 1; w < ScanScheduler::kWaits; ++w) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(P::Working),
                          static_cast<int>(sv::phaseOf(0, O::None, static_cast<W>(w), sv::elapsedMs(1000, 1003),
                                                       false, false)));
  }
  // The stall's hold from a start ahead: held (0 ms of its 30 s gone), and
  // let go 30 s after it.
  TEST_ASSERT_TRUE(sv::stallHolds(true, 1000, 1002));
  TEST_ASSERT_TRUE(sv::stallHolds(true, 31001, 1002));
  TEST_ASSERT_FALSE(sv::stallHolds(true, 31002, 1002));
  TEST_ASSERT_FALSE(sv::stallHolds(false, 1000, 1002));
}

// ---------------------------------------------------------------------------
// The loan of the Dance tab's block (the owner's "Borrow Dance's block",
// 2026-11-21): the snapshot sits in the Dance tab's 4,096 B tracker scratch
// (DanceMode::lendScratch() is loanOk() over its block and its tab), so
// the stats carve nothing.
// ---------------------------------------------------------------------------
void test_the_dance_tab_s_block_lent() {
  TEST_ASSERT_EQUAL_UINT32(4096, sv::kBound);
  // The block there and the tab down: lent for any snapshot within it.
  TEST_ASSERT_TRUE(sv::loanOk(true, false, sv::kHeaderBytes));
  TEST_ASSERT_TRUE(sv::loanOk(true, false, sv::bytesFor(1706)));  // the spare's 3,860 B
  TEST_ASSERT_EQUAL_UINT32(3860, sv::bytesFor(1706));
  TEST_ASSERT_TRUE(sv::loanOk(true, false, sv::kBound));
  // The tab up (its tracker reads into it from setActive(true)): never,
  // whatever the size; no block (no PSRAM for the dancer at the boot):
  // never.
  for (size_t n : {size_t{1}, size_t{sv::kHeaderBytes}, sv::bytesFor(1706), sv::kBound}) {
    TEST_ASSERT_FALSE(sv::loanOk(true, true, n));
    TEST_ASSERT_FALSE(sv::loanOk(false, false, n));
    TEST_ASSERT_FALSE(sv::loanOk(false, true, n));
  }
  // Past the block: no loan (the library past 18,432 albums: No room).
  TEST_ASSERT_FALSE(sv::loanOk(true, false, sv::kBound + 1));
  // Every library up to 18,432 albums fits the block; one more doesn't.
  uint32_t last = 0;
  for (uint32_t albums = 0; albums <= 20000; ++albums) {
    if (sv::loanOk(true, false, sv::bytesFor(albums))) last = albums;
  }
  TEST_ASSERT_EQUAL_UINT32(18432, last);
  TEST_ASSERT_TRUE(sv::loanOk(true, false, sv::bytesFor(18432)));
  TEST_ASSERT_FALSE(sv::loanOk(true, false, sv::bytesFor(18433)));
  static_assert(sv::loanOk(true, false, sv::kBound) && !sv::loanOk(true, true, 1), "loanOk() at compile time");
  printf("[visit] the Dance tab's block: lent while the tab is down, to 18,432 albums (%u B at 18,432)\n",
         static_cast<unsigned>(sv::bytesFor(18432)));
}

// ---------------------------------------------------------------------------
// The tag scan's word for kWhyRest (restHolds(): CardTasks' restAhead()):
// the owner's "Only while held" (2026-11-21) and "Don't wait at the floor"
// (2026-11-22). The visits wait while the scan holds its memory, but not
// for memory no step gives back (scanStuck(), kept as built: "Keep both as
// built"), and not while the battery floor pauses the scan: memory carved
// mid-rest stays held there with no step to give it back, so a visit goes
// ahead and its take's decoder-room check guards playback.
// ---------------------------------------------------------------------------
void test_the_rest_gate_s_word() {
  // The whole truth table: held only with the memory carved, not stuck, and
  // the floor not holding the scan.
  for (int i = 0; i < 8; ++i) {
    const bool held = (i & 1) != 0, stuck = (i & 2) != 0, atFloor = (i & 4) != 0;
    TEST_ASSERT_EQUAL_MESSAGE(held && !stuck && !atFloor, sv::restHolds(held, stuck, atFloor), "restHolds()");
  }
  TEST_ASSERT_TRUE(sv::restHolds(true, false, false));    // the rest's memory: a fresh card's, a Rescan's, the boot's
  TEST_ASSERT_FALSE(sv::restHolds(true, false, true));    // at the floor: the visit goes ahead
  TEST_ASSERT_FALSE(sv::restHolds(true, true, false));    // stuck: no step gives it back
  TEST_ASSERT_FALSE(sv::restHolds(false, false, false));  // nothing carved (trimmed, a fence emptied it, none yet)
  TEST_ASSERT_FALSE(sv::restHolds(false, false, true));   // a floor met before the scan's first step
  static_assert(sv::restHolds(true, false, false) && !sv::restHolds(true, false, true), "restHolds() at compile time");

  // With the scheduler's own floor (ScanScheduler::batteryLow(), as
  // CardTasks hands it): the memory held, the word follows the floor's
  // hysteresis (below 10% off USB; kept to 15%; let go above 15% or on USB).
  ScanScheduler s;
  ScanScheduler::In in;
  in.restPending = true;
  in.battery = 60;
  uint32_t t = 0;
  auto word = [&](int pct, bool usb) {
    in.nowMs = (t += 100);
    in.battery = pct;
    in.usb = usb;
    s.update(in);
    return sv::restHolds(true, false, s.batteryLow());
  };
  TEST_ASSERT_TRUE(word(60, false));   // above the floor: the visits wait the rest out
  TEST_ASSERT_TRUE(word(10, false));   // at 10%: not below it
  TEST_ASSERT_FALSE(word(9, false));   // below it, off USB: the scan pauses, the visits don't wait
  TEST_ASSERT_FALSE(word(15, false));  // kept to 15%
  TEST_ASSERT_FALSE(word(-1, false));  // a reading not known keeps it
  TEST_ASSERT_TRUE(word(16, false));   // above 15%: the scan goes on, the visits wait again
  TEST_ASSERT_FALSE(word(5, false));
  TEST_ASSERT_TRUE(word(5, true));     // USB: let go at once, whatever the reading
  TEST_ASSERT_TRUE(word(-1, true));
}

// ---------------------------------------------------------------------------
// Not played yet's 30 days (call 4, O5): one rule in lib/core for the
// visit's grade (StatsLists' notPlayedMayOpen(), which the native env
// doesn't build) and the shelf (statsview::notPlayedOpen(): test_ui_stats).
// The cases are S3a's code's rule: undated, perhaps (true: DAYS' span
// decides the shelf); dated with no first day, never; dated, today - since
// + 1 >= 30.
// ---------------------------------------------------------------------------
void test_the_shared_not_played_rule() {
  // S3a's rule, as StatsLists.cpp had it before the move.
  auto s3a = [](bool isDated, uint16_t today, uint16_t since) {
    if (!isDated) return true;
    if (since == sf::kUnsetDay) return false;
    return static_cast<long>(today) - static_cast<long>(since) + 1 >= 30;
  };
  const uint16_t today = 313;
  TEST_ASSERT_TRUE(sv::notPlayedMayOpen(false, 0, sf::kUnsetDay));
  TEST_ASSERT_TRUE(sv::notPlayedMayOpen(false, today, 243));
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(true, today, sf::kUnsetDay));
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(true, today, today - 28));  // 29 days
  TEST_ASSERT_TRUE(sv::notPlayedMayOpen(true, today, today - 29));   // 30
  TEST_ASSERT_TRUE(sv::notPlayedMayOpen(true, today, today - 30));   // 31
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(true, today, today));       // day 1
  TEST_ASSERT_FALSE(sv::notPlayedMayOpen(true, today, today + 5));   // a first day after today
  // Every first day around the month, and at the days' ends, as S3a's.
  for (int t : {0, 29, 30, 313, 65534})
    for (int d = -40; d <= 40; ++d) {
      const int since = t + d;
      if (since < 0 || since > 0xFFFF) continue;
      const uint16_t day = static_cast<uint16_t>(t), s = static_cast<uint16_t>(since);
      TEST_ASSERT_EQUAL(s3a(true, day, s), sv::notPlayedMayOpen(true, day, s));
      TEST_ASSERT_EQUAL(s3a(false, day, s), sv::notPlayedMayOpen(false, day, s));
    }
  // The counting days (4.1): at least 1; 0 not known.
  TEST_ASSERT_EQUAL_UINT32(71, sv::countingDaysOf(true, today, 243));
  TEST_ASSERT_EQUAL_UINT32(1, sv::countingDaysOf(true, today, today));
  TEST_ASSERT_EQUAL_UINT32(1, sv::countingDaysOf(true, today, today + 3));
  TEST_ASSERT_EQUAL_UINT32(0, sv::countingDaysOf(true, today, sf::kUnsetDay));
  TEST_ASSERT_EQUAL_UINT32(0, sv::countingDaysOf(false, today, 243));
  TEST_ASSERT_EQUAL_UINT32(30, sv::kNotPlayedDays);
  TEST_ASSERT_FALSE(sv::notPlayedDays(29));
  TEST_ASSERT_TRUE(sv::notPlayedDays(30));
  static_assert(sv::notPlayedMayOpen(true, 313, 284) && !sv::notPlayedMayOpen(true, 313, 285),
                "notPlayedMayOpen() at compile time");
  // UiAsk's values (StatsLists keeps its name: `using UiAsk`).
  TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(sv::UiAsk::Visit));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(sv::UiAsk::NotPlayed));
}

// ---------------------------------------------------------------------------
// The made-up snapshot (9.3's uiSx<n>: writeMadeUp()), read back through
// every accessor; its mark (kFlagMadeUp, the flags' 16: 1, 2, 4 and 8 are
// the rank's) cleared by a real ask's write().
// ---------------------------------------------------------------------------
void test_the_made_up_writer_reads_back() {
  const uint32_t albums = 1706, tracks = 19417;
  sv::MadeUp m;
  m.buildStamp = 0x0123456789abcdefull;
  m.trackCount = tracks;
  m.albums = albums;
  m.grade = sv::Grade::Full;
  m.dated = true;
  m.today = 313;
  m.time = (313u * 1440 + 1300) * 60 + 1;  // 21:40 on day 313
  m.liveRound = 9;
  m.sinceDay = 243;
  m.firstDay = 243;
  m.newestDay = 313;
  m.rcntFirstDay = 282;
  m.window = sv::k30Days;
  m.version = 42;
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    m.days[i].listenedMs = 60000u * (10 + i);
    m.days[i].plays = static_cast<uint16_t>(i + 1);
    m.days[i].autoDjPlays = static_cast<uint16_t>(i);
    m.days[i].autoDjSkips = static_cast<uint16_t>(i / 2);
  }
  const uint32_t rows[sr::kLists] = {18, 50, 300, 23, 41, 60};  // All time past its cap and room: 50
  const uint32_t totals[sr::kLists] = {18, 64, 212, 5, 41, 0};  // New finds' less than its rows: the rows
  const uint32_t tops[sr::kLists] = {12, 23, 41, 20000, 70000, 0};
  for (int l = 0; l < sr::kLists; ++l) {
    m.rows[l] = rows[l];
    m.totals[l] = totals[l];
    m.topCount[l] = tops[l];
  }
  m.recentFirstAgoMin = 25;
  m.recentStepMin = 97;
  m.sessionRows = 3;
  m.earlierRows = 2;
  m.notPlayedEvery = 3;
  Block b(sv::bytesFor(albums));
  b.fill(0xA5);
  sv::Snapshot* s = snapOf(b);
  TEST_ASSERT_TRUE(sv::writeMadeUp(m, s, b.bytes));
  TEST_ASSERT_TRUE(b.guarded());
  // The header.
  TEST_ASSERT_TRUE(s->valid());
  TEST_ASSERT_TRUE(s->written());
  TEST_ASSERT_TRUE(s->resolved());
  TEST_ASSERT_TRUE(s->dated());
  TEST_ASSERT_TRUE(s->hasNotPlayed());
  TEST_ASSERT_TRUE(s->madeUp());
  TEST_ASSERT_EQUAL_UINT8(16, sv::kFlagMadeUp);
  TEST_ASSERT_EQUAL(sv::Grade::Full, s->grade());
  TEST_ASSERT_EQUAL_UINT64(m.buildStamp, s->buildStamp());
  TEST_ASSERT_EQUAL_UINT32(tracks, s->trackCount());
  TEST_ASSERT_TRUE(s->matches(m.buildStamp, tracks));  // StatsLists::loop()'s key check keeps it
  TEST_ASSERT_FALSE(s->matches(m.buildStamp, tracks + 1));
  TEST_ASSERT_EQUAL_UINT32(m.time, s->time());
  TEST_ASSERT_EQUAL_UINT16(313, s->today());
  TEST_ASSERT_EQUAL_UINT32(9, s->liveRound());
  TEST_ASSERT_EQUAL_UINT16(243, s->firstDay());
  TEST_ASSERT_EQUAL_UINT16(313, s->newestDay());
  TEST_ASSERT_EQUAL_UINT16(282, s->rcntFirstDay());
  TEST_ASSERT_EQUAL_UINT16(243, s->sinceDay());
  TEST_ASSERT_EQUAL_UINT32(71, s->spanDays());
  TEST_ASSERT_EQUAL_UINT8(sv::k30Days, s->window());
  TEST_ASSERT_EQUAL_UINT32(42, s->version());
  TEST_ASSERT_EQUAL_UINT32(albums, s->albums());
  TEST_ASSERT_EQUAL_UINT32(sv::bytesFor(albums), s->bytes());
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) {
    const sr::Day d = s->day(i);
    TEST_ASSERT_EQUAL_UINT32(m.days[i].listenedMs, d.listenedMs);
    TEST_ASSERT_EQUAL_UINT16(m.days[i].plays, d.plays);
    TEST_ASSERT_EQUAL_UINT16(m.days[i].autoDjPlays, d.autoDjPlays);
    TEST_ASSERT_EQUAL_UINT16(m.days[i].autoDjSkips, d.autoDjSkips);
  }
  // Each list: its rows cut to its cap (All time's 300 to 50), its total
  // at least its rows, ids inside the library spread over it, the counts
  // falling from the first row's (saturating), the facts by list.
  const uint32_t nowMin = 313u * 1440 + 1300;
  for (sr::List l : kAll) {
    const uint32_t want = l == sr::kRecent ? 60 + 3 + 2 : std::min(rows[l], sr::kCaps[l]);
    TEST_ASSERT_EQUAL_UINT32(want, s->rows(l));
    TEST_ASSERT_EQUAL_UINT32(std::max(totals[l], want), s->total(l));
    TEST_ASSERT_EQUAL_PTR(b.p() + sv::offsetOf(l, albums), s->ids(l));
    std::map<uint32_t, int> seen;
    for (uint32_t i = 0; i < want; ++i) {
      const sv::Row r = s->rowAt(l, i);
      TEST_ASSERT_TRUE(r.id < tracks);
      TEST_ASSERT_EQUAL_UINT32(r.id, cc::get16(s->ids(l) + i * sv::kStrides[l]));  // the queue's id view
      ++seen[r.id];
      const uint32_t count = tops[l] > i + 1 ? tops[l] - i : 1;
      switch (l) {
        case sr::kNewFinds:
          TEST_ASSERT_EQUAL_UINT32(std::min<uint32_t>(count, sv::kNewFindsCountMax), r.count);
          TEST_ASSERT_EQUAL_UINT8(i % 3, r.src);
          break;
        case sr::kOldFavourites:
          // 4.3's rule: plays at least 5; last played before the 30 days
          // (282, today - 31, the first row's) and not before sinceDay
          // (243, the last row's), spread, each no newer than the last.
          TEST_ASSERT_EQUAL_UINT32(std::min<uint32_t>(std::max<uint32_t>(count, sr::kOldFavourite), sv::kCountMax),
                                   r.count);
          TEST_ASSERT_EQUAL_UINT16(282 - i * 39 / 40, r.lastDay);
          TEST_ASSERT_TRUE(r.lastDay >= 243 && r.lastDay <= 313 - 30);
          if (i > 0) TEST_ASSERT_TRUE(r.lastDay <= s->rowAt(l, i - 1).lastDay);
          break;
        case sr::kRecent:
          TEST_ASSERT_EQUAL_UINT32(0, r.count);
          if (i < 60) {
            TEST_ASSERT_EQUAL_UINT32(nowMin - 25 - 97 * i, r.when);
          } else {
            TEST_ASSERT_EQUAL_UINT32(i < 63 ? sv::kWhenSession : sv::kWhenEarlier, r.when);
          }
          break;
        default:
          TEST_ASSERT_EQUAL_UINT32(count, r.count);
          break;
      }
    }
    TEST_ASSERT_EQUAL_UINT32(want, seen.size());  // no id twice in a list
    // The rows past it zero, as resolve() leaves them.
    if (want < sv::roomOf(l, albums)) TEST_ASSERT_EQUAL_UINT16(0, s->rowAt(l, want).id);
  }
  TEST_ASSERT_FALSE(s->rcntShort());  // not asked (m.rcntShort false)
  // Old favourites' plays at least 5 when the recipe's run lower (4.3).
  {
    sv::MadeUp low = m;
    low.topCount[sr::kOldFavourites] = 14;  // view 1's: 14 down to 1 by the old recipe
    Block lb(sv::bytesFor(albums));
    sv::Snapshot* ls = snapOf(lb);
    TEST_ASSERT_TRUE(sv::writeMadeUp(low, ls, lb.bytes));
    for (uint32_t i = 0; i < ls->rows(sr::kOldFavourites); ++i)
      TEST_ASSERT_EQUAL_UINT32(i < 9 ? 14 - i : sr::kOldFavourite, ls->rowAt(sr::kOldFavourites, i).count);
  }
  // A track count a multiple of 7,919 (a prime: the step's first try), and
  // 7,919 itself: still no id twice in a list (the step coprime to it).
  for (const uint32_t n : {7919u, 15838u, 63352u}) {
    sv::MadeUp p = m;
    p.trackCount = n;
    Block pb(sv::bytesFor(albums));
    sv::Snapshot* ps = snapOf(pb);
    TEST_ASSERT_TRUE(sv::writeMadeUp(p, ps, pb.bytes));
    for (sr::List l : kAll) {
      std::map<uint32_t, int> seen;
      for (uint32_t i = 0; i < ps->rows(l); ++i) {
        TEST_ASSERT_TRUE(ps->rowAt(l, i).id < n);
        ++seen[ps->rowAt(l, i).id];
      }
      TEST_ASSERT_EQUAL_UINT32(ps->rows(l), seen.size());
    }
  }
  // Not played yet: every third album in A-Z order, through the directory.
  TEST_ASSERT_EQUAL_UINT32((albums + 2) / 3, s->notPlayedCount());
  for (uint32_t k = 0; k < s->notPlayedCount(); ++k) TEST_ASSERT_EQUAL_UINT32(3 * k, s->albumAt(k));
  TEST_ASSERT_EQUAL_UINT32(sv::kNoAlbum, s->albumAt(s->notPlayedCount()));
  TEST_ASSERT_EQUAL_UINT32(sv::offsetOf(sr::kRecent, albums) + sv::roomOf(sr::kRecent, albums) * 5,
                           s->bitsOffset());
  TEST_ASSERT_EQUAL_UINT32(s->bitsOffset() + (albums + 7) / 8, s->directoryOffset());
  // The UI's one write.
  s->setWindow(sv::kAllTime);
  TEST_ASSERT_EQUAL_UINT8(sv::kAllTime, s->window());

  // No clock: the lists that need it have none, Recently played this
  // session's then earlier rows, the 7 days zero; a Lists grade, no bits.
  // As a rank with no clock codes them (the owner's call, 2026-11-24): the
  // two codes alone, This session's first, no dated row (the recipe's 60
  // dated ones not written).
  sv::MadeUp u = m;
  u.dated = false;
  u.grade = sv::Grade::Lists;
  u.notPlayedEvery = 0;
  Block c(sv::bytesFor(albums));
  sv::Snapshot* t = snapOf(c);
  TEST_ASSERT_TRUE(sv::writeMadeUp(u, t, c.bytes));
  TEST_ASSERT_FALSE(t->dated());
  TEST_ASSERT_TRUE(t->resolved() && t->madeUp());
  TEST_ASSERT_FALSE(t->hasNotPlayed());
  TEST_ASSERT_EQUAL_UINT32(0, t->notPlayedCount());
  TEST_ASSERT_EQUAL_UINT32(0, t->time());
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, t->today());
  for (sr::List l : {sr::kMost7, sr::kMost30, sr::kNewFinds, sr::kOldFavourites}) {
    TEST_ASSERT_EQUAL_UINT32(0, t->rows(l));
    TEST_ASSERT_EQUAL_UINT32(0, t->total(l));
  }
  TEST_ASSERT_EQUAL_UINT32(50, t->rows(sr::kMostAll));
  TEST_ASSERT_EQUAL_UINT32(60, u.rows[sr::kRecent]);
  TEST_ASSERT_EQUAL_UINT32(5, t->rows(sr::kRecent));
  for (uint32_t i = 0; i < 5; ++i)
    TEST_ASSERT_EQUAL_UINT32(i < 3 ? sv::kWhenSession : sv::kWhenEarlier, t->rowAt(sr::kRecent, i).when);
  for (uint32_t i = 0; i < sr::kWeekDays; ++i) TEST_ASSERT_EQUAL_UINT32(0, t->day(i).listenedMs);
  TEST_ASSERT_EQUAL_UINT32(71, t->spanDays());  // DAYS' span, clock or not (O5)

  // A small library: the rows cut to its tracks.
  sv::MadeUp small = m;
  small.trackCount = 7;
  small.albums = 2;
  Block d(sv::bytesFor(2));
  TEST_ASSERT_TRUE(sv::writeMadeUp(small, snapOf(d), d.bytes));
  TEST_ASSERT_EQUAL_UINT32(7, snapOf(d)->rows(sr::kMostAll));
  TEST_ASSERT_EQUAL_UINT32(1, snapOf(d)->notPlayedCount());  // album 0 of 2
  // Refused, nothing written: no block, a block short, past the ids.
  TEST_ASSERT_FALSE(sv::writeMadeUp(m, nullptr, b.bytes));
  Block e(sv::bytesFor(albums) - 1);
  e.fill(0x5A);
  TEST_ASSERT_FALSE(sv::writeMadeUp(m, snapOf(e), e.bytes));
  TEST_ASSERT_EQUAL_UINT8(0x5A, e.p()[0]);
  sv::MadeUp big = m;
  big.trackCount = sv::kMaxTracks + 1;
  TEST_ASSERT_FALSE(sv::writeMadeUp(big, s, b.bytes));
  TEST_ASSERT_TRUE(s->madeUp());  // untouched

  // A trimmed layout (past 3,216 albums Old favourites and Recently played
  // give rows up: 5,000 albums, 190 each): every list asked past its room
  // is cut to it, and Not played yet's bits and directory, right after
  // Recently played's room, read back whole.
  {
    const uint32_t many = 5000;
    TEST_ASSERT_EQUAL_UINT32(26, sv::trimFor(many));
    TEST_ASSERT_EQUAL_UINT32(190, sv::roomOf(sr::kRecent, many));
    TEST_ASSERT_EQUAL_UINT32(190, sv::roomOf(sr::kOldFavourites, many));
    TEST_ASSERT_TRUE(sv::bytesFor(many) <= sv::kBound);
    sv::MadeUp t5 = m;
    t5.albums = many;
    for (int l = 0; l < sr::kLists; ++l) t5.rows[l] = 216;
    t5.rows[sr::kRecent] = 175;  // 175 dated, 10 this session's, 10 earlier: 195 asked
    t5.sessionRows = 10;
    t5.earlierRows = 10;
    t5.notPlayedEvery = 7;
    Block tb(sv::bytesFor(many));
    tb.fill(0xA5);
    sv::Snapshot* ts = snapOf(tb);
    TEST_ASSERT_TRUE(sv::writeMadeUp(t5, ts, tb.bytes));
    TEST_ASSERT_TRUE(tb.guarded());
    for (sr::List l : kAll)
      TEST_ASSERT_EQUAL_UINT32(std::min(sv::roomOf(l, many), sr::kCaps[l]), ts->rows(l));
    TEST_ASSERT_EQUAL_UINT32(190, ts->rows(sr::kRecent));
    TEST_ASSERT_EQUAL_UINT32(190, ts->rows(sr::kOldFavourites));
    TEST_ASSERT_EQUAL_UINT32(sv::offsetOf(sr::kRecent, many) + 190u * sv::kStrides[sr::kRecent], ts->bitsOffset());
    TEST_ASSERT_EQUAL_UINT32((many + 6) / 7, ts->notPlayedCount());
    for (uint32_t k = 0; k < ts->notPlayedCount(); ++k) TEST_ASSERT_EQUAL_UINT32(7 * k, ts->albumAt(k));
    // Recently played cut from its end: 175 dated, 10 this session's, 5 of
    // the earlier; its last row whole, not under the bits.
    TEST_ASSERT_TRUE(ts->rowAt(sr::kRecent, 174).when < sv::kWhenEarlier);
    TEST_ASSERT_EQUAL_UINT32(sv::kWhenSession, ts->rowAt(sr::kRecent, 175).when);
    TEST_ASSERT_EQUAL_UINT32(sv::kWhenEarlier, ts->rowAt(sr::kRecent, 189).when);
  }

  // RCNT short (the rank's mark, kFlagRcntShort: 32): the recipe's, dated
  // only.
  {
    sv::MadeUp sh = m;
    sh.rcntShort = true;
    Block hb(sv::bytesFor(albums));
    TEST_ASSERT_TRUE(sv::writeMadeUp(sh, snapOf(hb), hb.bytes));
    TEST_ASSERT_TRUE(snapOf(hb)->rcntShort());
    TEST_ASSERT_EQUAL_UINT8(32, sv::kFlagRcntShort);
    sh.dated = false;
    TEST_ASSERT_TRUE(sv::writeMadeUp(sh, snapOf(hb), hb.bytes));
    TEST_ASSERT_FALSE(snapOf(hb)->rcntShort());
  }

  // A real ask's write() over it: the mark goes with the rest of the flags.
  Block mem(sr::Ranker::kBytes);
  sr::Ranker k;
  k.begin(mem.p(), true, 313, 9);
  k.finish();
  WantedRegion w(k);
  sr::Report rep;
  TEST_ASSERT_TRUE(sv::write(k, w.w, rep, contextOf(albums, 9, 43), s, b.bytes));
  TEST_ASSERT_TRUE(s->written());
  TEST_ASSERT_FALSE(s->madeUp());
  TEST_ASSERT_FALSE(s->resolved());
  TEST_ASSERT_FALSE(s->rcntShort());
  // ... and the RCNT mark from its report (statsrank::rcntShort()): RCNT's
  // oldest day after DAYS' first played day in the 30 days; a gap at the
  // window's start (both the same day) none.
  rep.rcntFirstDay = 298;
  rep.days30FirstDay = 284;
  TEST_ASSERT_TRUE(sv::write(k, w.w, rep, contextOf(albums, 9, 44), s, b.bytes));
  TEST_ASSERT_TRUE(s->rcntShort());
  TEST_ASSERT_EQUAL_UINT16(298, s->rcntFirstDay());
  rep.days30FirstDay = 298;
  TEST_ASSERT_TRUE(sv::write(k, w.w, rep, contextOf(albums, 9, 45), s, b.bytes));
  TEST_ASSERT_FALSE(s->rcntShort());
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/rank.json", &gRank) || gRank.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_snapshot_s_bytes);
  RUN_TEST(test_write_and_resolve_on_the_rank_scenarios);
  RUN_TEST(test_saturation_and_when_s_codes);
  RUN_TEST(test_the_directory_and_album_at);
  RUN_TEST(test_the_fixture_boots_end_to_end);
  RUN_TEST(test_the_layouts_against_the_grade_table);
  RUN_TEST(test_phase_of);
  RUN_TEST(test_the_worker_s_give_up_holds_30_s_then_the_take);
  RUN_TEST(test_elapsed_ms_with_a_start_ahead_of_now);
  RUN_TEST(test_the_dance_tab_s_block_lent);
  RUN_TEST(test_the_rest_gate_s_word);
  RUN_TEST(test_the_shared_not_played_rule);
  RUN_TEST(test_the_made_up_writer_reads_back);
  return UNITY_END();
}
