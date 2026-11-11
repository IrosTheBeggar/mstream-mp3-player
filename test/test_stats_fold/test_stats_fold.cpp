// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' fold (lib/core StatsFold;
// docs/LISTENING-STATS.md 4.1, 5.2, 5.6), against tools/statsref.py's
// fixtures (test/fixtures/card/stats/index.json) and a model of it:
// - the rules of 4.1 (the line at an odd durationMs and at 0, the
//   outcomes, the sources, dated's ends) and the worked example's hour,
//   day and time; HEAD and a track's row byte for byte;
// - the merge (5.2's ⊕) order-free: every order of the same slots gives
//   the same row, but for lastOrd;
// - every boot of every fixture: stats.bin and its base read as a boot
//   reads them (a small reader here; StatsFile is the firmware's), the
//   boot's fold over the log's files in the scan's order (a partial scan
//   as the fixture says), the counts (folded, twice, gap), the save's
//   stats.bin and a compaction's base byte for byte, and the trim the
//   card's MARK allows, file for file;
// - each slot once (5.6's ranges: a slot met again in its own file, one
//   met in another file's range, counted twice), the gap (an ACKS row of
//   a key that never counts, a partial scan's none), the fold rounds (the
//   boot's +1, live +2), MARK kept for good (the card's beside RAM's),
//   sinceDay's four sources;
// - the retention: HOUR's and DAYS' newest rows whatever order the slots
//   come in, RCNT's 32 days from today or the newest day, and its 4,096
//   rows (a day of more goes whole), nothing by date with the clock unset;
// - random folds, reloads and saves against a model of statsref's fold (a
//   port here, std::map): the same file byte for byte, past every chunk's
//   rows, HOUR's 840, DAYS' 400 and RCNT's 4,096;
// - the memory: the blocks (chunks of 16,384 B, HOUR's 6,720, DAYS'
//   6,400, MARK's 1,536), never one table in one block, the spare, the
//   index carved again at twice the size, reserve() leaving the folds
//   nothing to carve, no memory at each carve (the fold stops, nothing is
//   saved; an index that can't be carved finds the same rows without it);
// - a full log never synced (13,106 slots): TRKD's rows the distinct
//   hashes, the peak bytes and the µs a slot, printed.
// Run: pio test -e native -f test_stats_fold
#include <unity.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "CardContainer.h"
#include "CardManifest.h"
#include "PlayLog.h"
#include "StatsFold.h"

namespace cc = cardcontract;
namespace pl = playlog;
namespace sf = statsfold;

void setUp() {}
void tearDown() {}

namespace {

minijson::Value gIndex;

// Made-up devices (RFC 7042's documentation MACs) and epochs.
const uint8_t kDevA[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
const uint8_t kDevB[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x02};
const uint8_t kDevC[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x03};
const uint8_t kZeros[6] = {};
constexpr uint64_t kEpochA = 0x1122334455667788ull;
constexpr uint64_t kEpochB = 0x0a0b0c0d0e0f1011ull;
constexpr uint64_t kMinMs = sf::kMinMs;

uint64_t at(uint32_t day, uint32_t hour, uint32_t minute = 0) {
  return kMinMs + static_cast<uint64_t>(day) * sf::kDayMs + static_cast<uint64_t>(hour) * sf::kHourMs +
         static_cast<uint64_t>(minute) * 60000;
}

pl::Slot slot(const uint8_t dev[6], uint64_t epoch, uint32_t seq, uint64_t hash, uint32_t played, uint32_t duration,
              uint8_t outcome = pl::kCompleted, uint8_t source = pl::kSourceManual, uint64_t startedAt = 0) {
  pl::Slot s;
  s.kind = pl::kKindPlay;
  s.outcome = outcome;
  s.source = source;
  s.clock = startedAt ? pl::kClockRtc : pl::kClockUnset;
  s.seq = seq;
  s.epoch = epoch;
  std::memcpy(s.deviceId, dev, 6);
  s.startedAt = startedAt;
  s.bootSeq = 1;
  s.uptimeMs = seq * 1000;
  s.playedMs = played;
  s.durationMs = duration;
  s.pathHash = hash;
  return s;
}

// ---------------------------------------------------------------------------
// A heap that counts: every block's size, the live bytes, and a carve that
// fails on demand.
// ---------------------------------------------------------------------------
struct Heap {
  std::map<void*, size_t> blocks;
  std::vector<size_t> carved;  // every carve's size, in order
  size_t live = 0, peak = 0;
  int failAfter = -1;          // carves before one fails (-1: none fails)
  size_t failSize = 0;         // a carve of this size fails (0: none)
  void reset() {
    carved.clear();
    peak = live;
    failAfter = -1;
    failSize = 0;
  }
};
Heap gHeap;

void* heapAlloc(size_t n) {
  if (gHeap.failAfter == 0 || (gHeap.failSize && n == gHeap.failSize)) return nullptr;
  if (gHeap.failAfter > 0) --gHeap.failAfter;
  void* p = std::malloc(n);
  gHeap.blocks[p] = n;
  gHeap.carved.push_back(n);
  gHeap.live += n;
  gHeap.peak = std::max(gHeap.peak, gHeap.live);
  return p;
}

void heapFree(void* p) {
  auto it = gHeap.blocks.find(p);
  TEST_ASSERT_TRUE_MESSAGE(it != gHeap.blocks.end(), "a block freed that the heap never gave");
  gHeap.live -= it->second;
  gHeap.blocks.erase(it);
  std::free(p);
}

// ---------------------------------------------------------------------------
// The file's frame (METADATA 2.4) as stats.bin and the base have it.
// ---------------------------------------------------------------------------
constexpr uint32_t kMagic = cc::fourcc("MPST");
const cc::SectionSpec kStatsSecs[] = {{sf::kTypes[0], 0, true},  {sf::kTypes[1], 24, true}, {sf::kTypes[2], 8, true},
                                      {sf::kTypes[3], 16, true}, {sf::kTypes[4], 12, true}, {sf::kTypes[5], 24, true}};
const cc::FormatSpec kStatsSpec{kMagic, 40, kStatsSecs, 6};
const cc::SectionSpec kBaseSecs[] = {{sf::kTrks, 24, true}};
const cc::FormatSpec kBaseSpec{kMagic, 40, kBaseSecs, 1};

using Bytes = std::string;
using Files = std::map<std::string, Bytes>;

Bytes frame(uint32_t generation, const std::vector<std::pair<uint32_t, std::pair<uint32_t, Bytes>>>& secs) {
  std::vector<cc::SectionOut> out;
  size_t cap = 40 + 32 * secs.size();
  for (const auto& s : secs) {
    cc::SectionOut o;
    o.type = s.first;
    o.flags = cc::kSectionRequired;
    o.stride = s.second.first;
    o.count = o.stride ? static_cast<uint32_t>(s.second.second.size() / o.stride) : 0;
    o.bytes = static_cast<uint32_t>(s.second.second.size());
    out.push_back(o);
    cap += o.bytes + 8;
  }
  std::vector<uint8_t> buf(cap);
  cc::BufferSink sink(buf.data(), static_cast<uint32_t>(buf.size()));
  cc::FileMeta meta;
  meta.magic = kMagic;
  meta.generation = generation;
  cc::ContainerWriter w;
  TEST_ASSERT_TRUE(w.begin(sink, meta, nullptr, 40, out.data(), static_cast<uint32_t>(out.size())));
  for (const auto& s : secs)
    if (!s.second.second.empty()) TEST_ASSERT_TRUE(w.write(s.second.second.data(), static_cast<uint32_t>(s.second.second.size())));
  uint32_t fileBytes = 0;
  TEST_ASSERT_TRUE(w.finish(&fileBytes));
  return Bytes(reinterpret_cast<const char*>(buf.data()), fileBytes);
}

uint32_t headerCrc(const Bytes& b) { return cc::get32(reinterpret_cast<const uint8_t*>(b.data()) + 32); }

// A section's rows as the fold gives them for a plan.
Bytes rowsOf(sf::Fold& f, const sf::Fold::Plan& p, sf::Sec sec) {
  Bytes out;
  if (sec == sf::kHead) {
    uint8_t h[sf::kHeadBytes];
    sf::encodeHead(p.head, h);
    return Bytes(reinterpret_cast<const char*>(h), sizeof(h));
  }
  f.rowsBegin(p, sec);
  uint8_t buf[24 * 37];
  for (uint32_t n; (n = f.nextRows(buf, 37)) > 0;) out.append(reinterpret_cast<const char*>(buf), n * sf::kStrides[sec]);
  TEST_ASSERT_EQUAL_UINT32(p.bytesOf(sec), out.size());
  return out;
}

Bytes statsBin(sf::Fold& f, const sf::Fold::Plan& p) {
  std::vector<std::pair<uint32_t, std::pair<uint32_t, Bytes>>> secs;
  for (int i = 0; i < sf::kSecs; ++i) {
    const sf::Sec sec = static_cast<sf::Sec>(i);
    secs.push_back({sf::kTypes[i], {sf::kStrides[i], rowsOf(f, p, sec)}});
  }
  return frame(p.generation, secs);
}

std::string baseName(uint32_t gen) {
  char n[32];
  std::snprintf(n, sizeof(n), "stats-%08x.bin", static_cast<unsigned>(gen));
  return n;
}

// A compaction (5.3), as StatsFile will do it: base ⊕ delta, a merge of
// two sorted streams, written as stats-<gen+1>.bin; the plan then names it
// with an empty TRKD.
Bytes compact(sf::Fold& f, sf::Fold::Plan* p, const std::vector<sf::Track>& base) {
  Bytes rows;
  f.rowsBegin(*p, sf::kTrkd);
  sf::Track d;
  bool more = f.nextTrack(&d);
  size_t i = 0;
  uint32_t n = 0;
  uint8_t r[sf::kTrackBytes];
  while (more || i < base.size()) {
    sf::Track t;
    if (more && (i == base.size() || d.hash < base[i].hash)) {
      t = d;
      more = f.nextTrack(&d);
    } else if (!more || base[i].hash < d.hash) {
      t = base[i++];
    } else {
      t = sf::merge(base[i++], d);
      more = f.nextTrack(&d);
    }
    sf::encodeTrack(t, r);
    rows.append(reinterpret_cast<const char*>(r), sizeof(r));
    ++n;
  }
  const uint32_t gen = p->head.baseGen + 1;
  const Bytes b = frame(gen, {{sf::kTrks, {24, rows}}});
  p->head.baseGen = gen;
  p->head.baseRows = n;
  p->head.baseCrc = headerCrc(b);
  p->compact = true;
  return b;
}

// ---------------------------------------------------------------------------
// The read (5.4) as a boot does it, small: StatsFile is the firmware's.
// ---------------------------------------------------------------------------
enum class Read { Loaded, Absent, Corrupt, Newer, Unread };
const char* readName(Read r) {
  switch (r) {
    case Read::Loaded: return "loaded";
    case Read::Absent: return "absent";
    case Read::Corrupt: return "corrupt";
    case Read::Newer: return "newer";
    default: return "unread";
  }
}

struct Section {
  const uint8_t* p;
  uint32_t bytes, count, stride;
};

bool sections(const Bytes& b, const cc::FormatSpec& spec, cc::Why* why, Section* out, uint32_t* generation) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  uint8_t th[8];
  *why = c.open(src, spec, th);
  if (*why != cc::Why::Ok) return false;
  uint8_t buf[512];
  for (uint32_t i = 0; i < spec.count; ++i) {
    *why = c.checkCrc(i, buf, sizeof(buf));
    if (*why != cc::Why::Ok) return false;
    const cc::Section& s = c.section(i);
    out[i] = {reinterpret_cast<const uint8_t*>(b.data()) + s.offset, s.bytes, s.count, s.stride};
  }
  *generation = c.header().generation;
  return true;
}

// `named`: the base stats.bin names, whole or lost (a compaction never
// writes that name: 5.3).
Read readStats(sf::Fold& f, const Files& in, std::vector<sf::Track>* base, uint32_t* named = nullptr) {
  f.clear();
  base->clear();
  if (named) *named = 0;
  auto it = in.find("stats.bin");
  if (it == in.end()) return Read::Absent;
  cc::Why why;
  Section s[6];
  uint32_t gen = 0;
  if (!sections(it->second, kStatsSpec, &why, s, &gen)) {
    if (why == cc::Why::Major || why == cc::Why::UnknownRequired) return Read::Newer;
    return Read::Corrupt;
  }
  if (s[0].bytes < sf::kHeadBytes) return Read::Corrupt;
  TEST_ASSERT_TRUE(f.reserveLoad(s[sf::kRcnt].count, s[sf::kTrkd].count));
  f.loadGeneration(gen);
  f.loadHead(s[0].p);
  for (int i = 1; i < sf::kSecs; ++i) {
    if (!f.loadRows(static_cast<sf::Sec>(i), s[i].p, s[i].count, s[i].stride)) {
      f.clear();
      return Read::Corrupt;
    }
  }
  const sf::Head h = f.head();
  if (named) *named = h.baseGen;
  if (h.baseGen) {
    auto b = in.find(baseName(h.baseGen));
    if (b != in.end() && b->second.size() < 40) return Read::Unread;  // its header doesn't read whole
    if (b == in.end() || headerCrc(b->second) != h.baseCrc) {
      f.baseLost();
    } else {
      Section t;
      uint32_t g = 0;
      if (!sections(b->second, kBaseSpec, &why, &t, &g) || g != h.baseGen || t.count != h.baseRows) return Read::Unread;
      for (uint32_t i = 0; i < t.count; ++i) {
        base->push_back(sf::decodeTrack(t.p + static_cast<size_t>(i) * t.stride));
        if (i && (*base)[i].hash <= (*base)[i - 1].hash) return Read::Unread;
      }
    }
  }
  return Read::Loaded;
}

cc::msmf::Acks acksOf(const minijson::Value& v) {
  cc::msmf::Acks a;
  a.present = v.size() > 0;
  for (size_t i = 0; i < v.size() && a.count < cc::msmf::Acks::kMax; ++i) {
    cc::msmf::Ack& e = a.entries[a.count++];
    const std::string& d = v[i][0].str();
    for (int k = 0; k < 6; ++k) e.deviceId[k] = static_cast<uint8_t>(std::strtoul(d.substr(2 * k, 2).c_str(), nullptr, 16));
    e.epoch = v[i][1].u64();
    e.seq = static_cast<uint32_t>(v[i][2].u64());
  }
  a.rows = a.count;
  return a;
}

const char* const kLogNames[sf::kFiles] = {"plays.jnl",   "plays.1.jnl", "plays.2.jnl", "plays.3.jnl", "plays.4.jnl",
                                           "plays.5.jnl", "plays.6.jnl", "plays.7.jnl", "plays.8.jnl", "plays.9.jnl"};

// The boot's scan as the journal reads the log: each file in its order,
// whole slots to the cap; `unread` the card's faults (-1 an open that
// failed, else the slots read before a read failed).
void scanLog(sf::Fold& f, const Files& in, const std::map<std::string, int>& unread) {
  for (int i = 0; i < sf::kFiles; ++i) {
    auto it = in.find(kLogNames[i]);
    if (it == in.end()) continue;
    uint32_t slots = static_cast<uint32_t>(std::min<size_t>(it->second.size(), pl::kFileBytes) / pl::kSlot);
    auto u = unread.find(kLogNames[i]);
    if (u != unread.end()) slots = u->second < 0 ? 0 : std::min<uint32_t>(slots, static_cast<uint32_t>(u->second));
    for (uint32_t k = 0; k < slots; ++k) {
      pl::Slot s;
      if (pl::decode(reinterpret_cast<const uint8_t*>(it->second.data()) + k * pl::kSlot, pl::kSlot, &s) ==
          pl::Verdict::Ok)
        f.bootSlot(s, i);
    }
    if (u != unread.end()) f.partial();
  }
}

// 2.20.2's trim as the journal judges a file (every play acknowledged, one
// at least, a play last, nothing it can't judge), and 5.5's: every play
// slot at or under the card's MARK.
bool trimmed(sf::Fold& f, const Bytes& b, const cc::msmf::Acks& acks) {
  if (b.size() > pl::kFileBytes) return false;
  bool anyPlay = false, allAcked = true, lastIsPlay = false, covered = true;
  for (size_t k = 0; k + pl::kSlot <= b.size(); k += pl::kSlot) {
    pl::Slot s;
    const pl::Verdict v = pl::decode(reinterpret_cast<const uint8_t*>(b.data()) + k, pl::kSlot, &s);
    if (v == pl::Verdict::Newer || v == pl::Verdict::Kind) return false;
    if (v != pl::Verdict::Ok) continue;
    lastIsPlay = s.kind == pl::kKindPlay;
    if (s.kind != pl::kKindPlay) continue;
    anyPlay = true;
    if (!pl::acknowledged(s, &acks)) allAcked = false;
    if (!f.covered(s)) covered = false;
  }
  return anyPlay && allAcked && lastIsPlay && covered;
}

Files cardOf(const minijson::Value& where) {
  Files out;
  for (const auto& m : where.members) out[m.first] = cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first);
  return out;
}

// ---------------------------------------------------------------------------
// A model of statsref's fold, save and file, kept plain (std::map): the
// random tests' reference.
// ---------------------------------------------------------------------------
struct Model {
  sf::Head head;
  uint32_t generation = 0;
  std::map<std::pair<std::array<uint8_t, 6>, uint64_t>, uint32_t> mark;
  std::map<uint32_t, std::array<uint32_t, 2>> hour;              // plays, listenedMs
  std::map<uint32_t, std::array<uint32_t, 7>> days;              // plays, skips, djP, djS, ms, shP, shS
  std::map<std::pair<uint32_t, uint64_t>, uint32_t> rcnt;
  std::map<uint64_t, sf::Track> trkd;
  uint32_t used = 0;
  bool anyUsed = false;
  int firstPlayDay = -1;

  static uint64_t sat(uint64_t v, uint64_t top) { return v > top ? top : v; }

  void fold(const pl::Slot& s, uint32_t rnd) {
    const bool play = sf::isPlay(s), skip = sf::isSkip(s), dated = s.startedAt >= kMinMs && s.startedAt <= 4102444799999ull;
    const int src = s.source == 3 ? 2 : s.source == 2 ? 1 : 0;
    head.slots += 1;
    head.listenedMs += s.playedMs;
    head.src[src].listenedMs += s.playedMs;
    if (play) {
      head.plays += 1;
      head.src[src].plays += 1;
      if (!dated) head.undated += 1;
    }
    if (skip) {
      head.skips += 1;
      head.src[src].skips += 1;
    }
    if (dated) {
      auto& hr = hour[static_cast<uint32_t>((s.startedAt - kMinMs) / 3600000)];
      hr[0] = static_cast<uint32_t>(sat(hr[0] + play, 255));
      hr[1] = static_cast<uint32_t>(sat(static_cast<uint64_t>(hr[1]) + s.playedMs, 0xFFFFFFFFull));
      const uint32_t day = static_cast<uint32_t>((s.startedAt - kMinMs) / 86400000);
      auto& d = days[day];
      d[0] = static_cast<uint32_t>(sat(d[0] + play, 0xFFFF));
      d[1] = static_cast<uint32_t>(sat(d[1] + skip, 0xFFFF));
      if (src == 2) {
        d[2] = static_cast<uint32_t>(sat(d[2] + play, 255));
        d[3] = static_cast<uint32_t>(sat(d[3] + skip, 255));
      }
      if (src == 1) {
        d[5] = static_cast<uint32_t>(sat(d[5] + play, 255));
        d[6] = static_cast<uint32_t>(sat(d[6] + skip, 255));
      }
      d[4] = static_cast<uint32_t>(sat(static_cast<uint64_t>(d[4]) + s.playedMs, 0xFFFFFFFFull));
      if (play) {
        auto& n = rcnt[{day, s.pathHash}];
        n = static_cast<uint32_t>(sat(n + 1, 0xFFFF));
        if (firstPlayDay < 0 || static_cast<int>(day) < firstPlayDay) firstPlayDay = static_cast<int>(day);
      }
    }
    sf::Track r;
    if (play) {
      const uint32_t t = dated ? static_cast<uint32_t>((s.startedAt - kMinMs) / 1000 + 1) : 0;
      r.plays = 1;
      r.firstAt = r.lastAt = t;
      r.lastOrd = std::min<uint32_t>(rnd, 0xFFFFFF);
      r.src = static_cast<uint8_t>(src);
    } else if (skip) {
      r.skips = 1;
    }
    if (play || skip) {
      r.hash = s.pathHash;
      auto it = trkd.find(s.pathHash);
      if (it == trkd.end()) {
        trkd[s.pathHash] = r;
      } else {
        // 5.2's merge, written out from statsref's rules.
        const sf::Track a = it->second;
        sf::Track m;
        m.hash = a.hash;
        m.plays = static_cast<uint16_t>(sat(a.plays + r.plays, 0xFFFF));
        m.skips = static_cast<uint16_t>(sat(a.skips + r.skips, 0xFFFF));
        auto key = [](uint32_t t) { return t == 0 ? 1ull << 32 : t == 0xFFFFFFFFu ? (1ull << 32) + 1 : t; };
        const auto ka = std::make_pair(key(a.firstAt), a.src), kb = std::make_pair(key(r.firstAt), r.src);
        m.firstAt = ka <= kb ? a.firstAt : r.firstAt;
        m.src = ka <= kb ? a.src : r.src;
        m.lastAt = std::max(a.lastAt, r.lastAt);
        m.lastOrd = std::max(a.lastOrd, r.lastOrd);
        it->second = m;
      }
    }
    std::array<uint8_t, 6> dev;
    std::memcpy(dev.data(), s.deviceId, 6);
    auto& m = mark[{dev, s.epoch}];
    m = std::max(m, s.seq);
    used = anyUsed ? std::max(used, rnd) : rnd;
    anyUsed = true;
  }

  // statsref's save: the round, sinceDay, the retention, the generation.
  void save(uint64_t nowMs, bool hasCommit = false, uint64_t commitSec = 0) {
    const bool known = nowMs >= kMinMs && nowMs <= 4102444799999ull;
    const uint32_t today = known ? static_cast<uint32_t>((nowMs - kMinMs) / 86400000) : 0;
    if (anyUsed) head.round = std::max(head.round, used);
    if (head.sinceDay == 0xFFFF) {
      int first = firstPlayDay;
      if (first >= 0 && known) first = std::min<int>(first, static_cast<int>(today));
      if (first >= 0) {
        head.sinceDay = static_cast<uint16_t>(first);
      } else if (known) {
        head.sinceDay = static_cast<uint16_t>(today);
      } else if (hasCommit && commitSec * 1000 >= kMinMs && commitSec * 1000 <= 4102444799999ull) {
        head.sinceDay = static_cast<uint16_t>((commitSec * 1000 - kMinMs) / 86400000);
        head.flags |= 1;
      }
    }
    while (hour.size() > 840) hour.erase(hour.begin());
    while (days.size() > 400) days.erase(days.begin());
    if (known && !days.empty()) {
      const int64_t ref = std::min<int64_t>(today, days.rbegin()->first);
      for (auto it = rcnt.begin(); it != rcnt.end();)
        it = static_cast<int64_t>(it->first.first) <= ref - 32 ? rcnt.erase(it) : std::next(it);
    }
    while (rcnt.size() > 4096) {
      const uint32_t oldest = rcnt.begin()->first.first;
      while (!rcnt.empty() && rcnt.begin()->first.first == oldest) rcnt.erase(rcnt.begin());
    }
    generation += 1;
  }

  Bytes file() const {
    Bytes h(sf::kHeadBytes, '\0'), mk, hr, dy, rc, tk;
    sf::encodeHead(head, reinterpret_cast<uint8_t*>(&h[0]));
    for (const auto& m : mark) {
      uint8_t r[24] = {};
      std::memcpy(r, m.first.first.data(), 6);
      cc::put64(r + 8, m.first.second);
      cc::put32(r + 16, m.second);
      mk.append(reinterpret_cast<const char*>(r), 24);
    }
    for (const auto& x : hour) {
      uint8_t r[8];
      cc::put32(r, x.first | x.second[0] << 24);
      cc::put32(r + 4, x.second[1]);
      hr.append(reinterpret_cast<const char*>(r), 8);
    }
    for (const auto& x : days) {
      uint8_t r[16] = {};
      cc::put16(r, static_cast<uint16_t>(x.first));
      cc::put16(r + 2, static_cast<uint16_t>(x.second[0]));
      cc::put16(r + 4, static_cast<uint16_t>(x.second[1]));
      r[6] = static_cast<uint8_t>(x.second[2]);
      r[7] = static_cast<uint8_t>(x.second[3]);
      cc::put32(r + 8, x.second[4]);
      r[12] = static_cast<uint8_t>(x.second[5]);
      r[13] = static_cast<uint8_t>(x.second[6]);
      dy.append(reinterpret_cast<const char*>(r), 16);
    }
    for (const auto& x : rcnt) {
      uint8_t r[12];
      cc::put16(r, static_cast<uint16_t>(x.first.first));
      cc::put16(r + 2, static_cast<uint16_t>(x.second));
      cc::put64(r + 4, x.first.second);
      rc.append(reinterpret_cast<const char*>(r), 12);
    }
    for (const auto& x : trkd) {
      uint8_t r[24];
      sf::encodeTrack(x.second, r);
      tk.append(reinterpret_cast<const char*>(r), 24);
    }
    return frame(generation, {{sf::kTypes[0], {0, h}},
                              {sf::kTypes[1], {24, mk}},
                              {sf::kTypes[2], {8, hr}},
                              {sf::kTypes[3], {16, dy}},
                              {sf::kTypes[4], {12, rc}},
                              {sf::kTypes[5], {24, tk}}});
  }
};

// The fold's stats.bin for a save now (nothing compacted).
Bytes saveNow(sf::Fold& f, uint64_t nowMs, bool hasCommit = false, uint64_t commitSec = 0) {
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(nowMs, hasCommit, commitSec, &p));
  const Bytes b = statsBin(f, p);
  f.saved(p);
  return b;
}

// stats.bin's bytes loaded into a fresh fold (generation, HEAD, rows).
void load(sf::Fold& f, const Bytes& b) {
  Files in{{"stats.bin", b}};
  std::vector<sf::Track> base;
  TEST_ASSERT_EQUAL_STRING("loaded", readName(readStats(f, in, &base)));
}

// A deterministic generator.
struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed) {}
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return static_cast<uint32_t>(s >> 11);
  }
  uint32_t below(uint32_t n) { return next() % n; }
};

uint64_t hashOf(uint32_t i) { return cc::fnv1a64(&i, sizeof(i)); }

std::string hex(const Bytes& a, const Bytes& b) {
  // Where two files first differ, for a failure's message.
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  char m[96];
  std::snprintf(m, sizeof(m), "%zu and %zu bytes, first difference at %zu", a.size(), b.size(), i);
  return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// The rules (4.1).
// ---------------------------------------------------------------------------
void test_the_rules_of_4_1() {
  const uint64_t h = 7;
  // The line: min(30 s, durationMs / 2), not rounded; 30 s with durationMs 0.
  TEST_ASSERT_FALSE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 20000, 40001, pl::kStopped)));
  TEST_ASSERT_TRUE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 20001, 40001, pl::kStopped)));
  TEST_ASSERT_TRUE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 30000, 0, pl::kStopped)));
  TEST_ASSERT_FALSE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 29999, 0, pl::kStopped)));
  TEST_ASSERT_TRUE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 30000, 4000000)));
  TEST_ASSERT_TRUE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 20000, 40000)));  // the worked example's 44
  // A skip: outcome 2 under the line; over it, a play.
  TEST_ASSERT_TRUE(sf::isSkip(slot(kDevA, kEpochA, 1, h, 29999, 0, pl::kSkipped)));
  TEST_ASSERT_FALSE(sf::isSkip(slot(kDevA, kEpochA, 1, h, 31000, 100000, pl::kSkipped)));
  for (uint8_t o : {uint8_t{0}, pl::kCompleted, pl::kStopped, uint8_t{4}}) {
    TEST_ASSERT_FALSE(sf::isSkip(slot(kDevA, kEpochA, 1, h, 10000, 200000, o)));
    TEST_ASSERT_FALSE(sf::isPlay(slot(kDevA, kEpochA, 1, h, 10000, 200000, o)));
  }
  // The sources.
  const uint8_t want[] = {sf::kYou, sf::kYou, sf::kShuffle, sf::kAutoDj, sf::kYou, sf::kYou};
  for (uint8_t src = 0; src < 6; ++src)
    TEST_ASSERT_EQUAL_UINT8(want[src], sf::sourceOf(slot(kDevA, kEpochA, 1, h, 1, 1, 1, src)));
  // What counts: kind 1, an epoch, a seq, a deviceId.
  TEST_ASSERT_TRUE(sf::counts(slot(kDevA, kEpochA, 1, h, 1, 1)));
  TEST_ASSERT_FALSE(sf::counts(slot(kDevA, 0, 1, h, 1, 1)));
  TEST_ASSERT_FALSE(sf::counts(slot(kDevA, kEpochA, 0, h, 1, 1)));
  TEST_ASSERT_FALSE(sf::counts(slot(kZeros, kEpochA, 1, h, 1, 1)));
  pl::Slot note = slot(kDevA, kEpochA, 1, h, 1, 1);
  note.kind = pl::kKindDrop;
  TEST_ASSERT_FALSE(sf::counts(note));
  // Dated: 2026-01-01T00:00:00Z to 2099-12-31T23:59:59.999Z.
  TEST_ASSERT_FALSE(sf::dated(0));
  TEST_ASSERT_FALSE(sf::dated(wallclock::kMinMs - 1));
  TEST_ASSERT_TRUE(sf::dated(wallclock::kMinMs));
  TEST_ASSERT_TRUE(sf::dated(wallclock::kMaxMs));
  TEST_ASSERT_FALSE(sf::dated(wallclock::kMaxMs + 1));
  // The worked example (5.13): 2026-11-08T21:40:00Z is day 311, hour
  // 7,485, time 26,948,401.
  const uint64_t x = at(311, 21, 40);
  TEST_ASSERT_EQUAL_UINT32(311, sf::dayOf(x));
  TEST_ASSERT_EQUAL_UINT32(7485, sf::hourOf(x));
  TEST_ASSERT_EQUAL_UINT32(26948401, sf::timeOf(x));
  TEST_ASSERT_EQUAL_UINT32(311, sf::dayOfTime(sf::timeOf(x)));
  TEST_ASSERT_EQUAL_UINT32(0, sf::dayOfTime(sf::timeOf(kMinMs)));
  // The last dated time still fits a u24 hour, a u16 day and a time.
  TEST_ASSERT_TRUE(sf::hourOf(wallclock::kMaxMs) < (1u << 24));
  TEST_ASSERT_TRUE(sf::timeOf(wallclock::kMaxMs) < sf::kNoFirst);
  uint16_t today = 0;
  TEST_ASSERT_FALSE(sf::todayOf(0, &today));
  TEST_ASSERT_TRUE(sf::todayOf(x, &today));
  TEST_ASSERT_EQUAL_UINT16(311, today);
}

void test_head_and_rows_byte_for_byte() {
  // HEAD as the worked example's first boot saved it: decoded and encoded
  // again, the same 128 bytes; the fields where 5.2 puts them.
  const minijson::Value& sc = gIndex[0];
  TEST_ASSERT_EQUAL_STRING("worked-example", sc["name"].c_str());
  const Files out = cardOf(sc["boots"][0]["out"]);
  cc::Why why;
  Section s[6];
  uint32_t gen = 0;
  TEST_ASSERT_TRUE(sections(out.at("stats.bin"), kStatsSpec, &why, s, &gen));
  TEST_ASSERT_EQUAL_UINT32(128, s[0].bytes);
  const sf::Head h = sf::decodeHead(s[0].p);
  uint8_t again[128];
  sf::encodeHead(h, again);
  TEST_ASSERT_EQUAL_MEMORY(s[0].p, again, 128);
  const minijson::Value& want = sc["boots"][0]["stats"]["head"];
  TEST_ASSERT_EQUAL_UINT64(want["plays"].u64(), h.plays);
  TEST_ASSERT_EQUAL_UINT64(want["listenedMs"].u64(), h.listenedMs);
  TEST_ASSERT_EQUAL_UINT64(want["slots"].u64(), h.slots);
  TEST_ASSERT_EQUAL_UINT32(want["autodj"][0].u64(), h.src[sf::kAutoDj].plays);
  TEST_ASSERT_EQUAL_UINT64(want["you"][2].u64(), h.src[sf::kYou].listenedMs);
  TEST_ASSERT_EQUAL_UINT32(want["sinceDay"].u64(), h.sinceDay);
  TEST_ASSERT_EQUAL_UINT32(want["round"].u64(), h.round);
  TEST_ASSERT_EQUAL_UINT32(want["baseCrc"].u64(), h.baseCrc);
  // A track's row: the source in flags' bits 0-1, the rest of them 0 (and
  // ignored when read).
  sf::Track t;
  t.hash = 0x0123456789ABCDEFull;
  t.plays = 0xFFFF;
  t.skips = 3;
  t.firstAt = 26948401;
  t.lastAt = 26985841;
  t.lastOrd = 0xFFFFFF;
  t.src = sf::kAutoDj;
  uint8_t r[24];
  sf::encodeTrack(t, r);
  TEST_ASSERT_EQUAL_HEX8(0x02, r[23]);
  TEST_ASSERT_EQUAL_HEX32(0x02FFFFFF, cc::get32(r + 20));
  r[23] |= 0xFC;
  const sf::Track u = sf::decodeTrack(r);
  TEST_ASSERT_EQUAL_UINT8(sf::kAutoDj, u.src);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFF, u.lastOrd);
  TEST_ASSERT_EQUAL_UINT64(t.hash, u.hash);
}

// ---------------------------------------------------------------------------
// The merge (5.2).
// ---------------------------------------------------------------------------
void test_the_merge_is_order_free() {
  // One track's slots: dated plays of two sources, undated plays of two
  // sources, skips, a stop under the line (no row of its own).
  const uint64_t h = 42;
  std::vector<pl::Slot> s = {
      slot(kDevA, kEpochA, 1, h, 200000, 200000, 1, pl::kSourceAutoDj, at(300, 9)),
      slot(kDevA, kEpochA, 2, h, 200000, 200000, 1, pl::kSourceShuffle, at(299, 9)),
      slot(kDevA, kEpochA, 3, h, 200000, 200000, 1, pl::kSourceAutoDj, 0),
      slot(kDevA, kEpochA, 4, h, 200000, 200000, 1, pl::kSourceShuffle, 0),
      slot(kDevA, kEpochA, 5, h, 5000, 200000, pl::kSkipped, pl::kSourceManual, at(301, 9)),
      slot(kDevA, kEpochA, 6, h, 5000, 200000, pl::kSkipped, pl::kSourceManual, 0),
      slot(kDevA, kEpochA, 7, h, 200000, 200000, 1, pl::kSourceManual, at(299, 9)),  // ties 2's time: you wins
  };
  std::vector<size_t> order(s.size());
  for (size_t i = 0; i < order.size(); ++i) order[i] = i;
  sf::Track first;
  bool any = false;
  int n = 0;
  do {
    sf::Track row;
    bool has = false;
    for (size_t i : order) {
      sf::Track t;
      if (!sf::slotTrack(s[i], 5, &t)) continue;
      row = has ? sf::merge(row, t) : t;
      has = true;
    }
    TEST_ASSERT_TRUE(has);
    if (!any) {
      first = row;
      any = true;
    }
    TEST_ASSERT_EQUAL_UINT16(first.plays, row.plays);
    TEST_ASSERT_EQUAL_UINT16(first.skips, row.skips);
    TEST_ASSERT_EQUAL_UINT32(first.firstAt, row.firstAt);
    TEST_ASSERT_EQUAL_UINT8(first.src, row.src);
    TEST_ASSERT_EQUAL_UINT32(first.lastAt, row.lastAt);
    TEST_ASSERT_EQUAL_UINT32(first.lastOrd, row.lastOrd);
    ++n;
  } while (std::next_permutation(order.begin(), order.end()));
  TEST_ASSERT_EQUAL_INT(5040, n);
  TEST_ASSERT_EQUAL_UINT16(5, first.plays);
  TEST_ASSERT_EQUAL_UINT16(2, first.skips);
  TEST_ASSERT_EQUAL_UINT32(sf::timeOf(at(299, 9)), first.firstAt);
  TEST_ASSERT_EQUAL_UINT8(sf::kYou, first.src);
  TEST_ASSERT_EQUAL_UINT32(sf::timeOf(at(300, 9)), first.lastAt);
  // Two undated plays only: firstAt 0, the smaller source; a skip alone:
  // none; a skip then an undated play: 0.
  sf::Track a, b, k;
  TEST_ASSERT_TRUE(sf::slotTrack(s[2], 1, &a));
  TEST_ASSERT_TRUE(sf::slotTrack(s[3], 1, &b));
  TEST_ASSERT_TRUE(sf::slotTrack(s[5], 1, &k));
  TEST_ASSERT_EQUAL_UINT8(sf::kShuffle, sf::merge(a, b).src);
  TEST_ASSERT_EQUAL_UINT8(sf::kShuffle, sf::merge(b, a).src);
  TEST_ASSERT_EQUAL_UINT32(0, sf::merge(a, b).firstAt);
  TEST_ASSERT_EQUAL_UINT32(sf::kNoFirst, k.firstAt);
  TEST_ASSERT_EQUAL_UINT32(0, k.lastOrd);
  TEST_ASSERT_EQUAL_UINT32(0, sf::merge(k, a).firstAt);
  TEST_ASSERT_EQUAL_UINT8(sf::kAutoDj, sf::merge(k, a).src);
  // Saturating: plays and skips at 0xFFFF; a round past 24 bits.
  sf::Track big = a;
  big.plays = 0xFFFE;
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, sf::merge(big, sf::merge(a, b)).plays);
  sf::Track r;
  TEST_ASSERT_TRUE(sf::slotTrack(s[0], 0x1000000, &r));
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, r.lastOrd);
  // Neither a play nor a skip: no row.
  TEST_ASSERT_FALSE(sf::slotTrack(slot(kDevA, kEpochA, 9, h, 8000, 0, pl::kStopped), 1, &r));
}

// ---------------------------------------------------------------------------
// Every boot of every fixture.
// ---------------------------------------------------------------------------
void test_every_fixture_boot() {
  sf::Fold f(heapAlloc, heapFree);
  TEST_ASSERT_TRUE(f.begin());
  int boots = 0, saves = 0, compactions = 0, trims = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    for (size_t k = 0; k < sc["boots"].size(); ++k) {
      const minijson::Value& b = sc["boots"][k];
      char what[96];
      std::snprintf(what, sizeof(what), "%s, boot %u", sc["name"].c_str(), static_cast<unsigned>(k + 1));
      const Files in = cardOf(b["in"]);
      const Files out = cardOf(b["out"]);
      const cc::msmf::Acks acks = acksOf(b["acks"]);
      std::vector<sf::Track> base;
      uint32_t named = 0;
      const Read read = readStats(f, in, &base, &named);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(b["read"].c_str(), readName(read), what);
      ++boots;
      if (read == Read::Newer || read == Read::Unread) {
        // Stats off: nothing folded, saved or trimmed.
        TEST_ASSERT_EQUAL_MESSAGE(0, b["folded"].u64(), what);
        TEST_ASSERT_FALSE_MESSAGE(b["saved"].boolean(), what);
        TEST_ASSERT_EQUAL_MESSAGE(0, b["trimmed"].size(), what);
        continue;
      }
      if (read == Read::Corrupt) f.clear();  // (renamed stats.bad: counting starts again)
      std::map<std::string, int> unread;
      for (const auto& m : b["unread"].members) unread[m.first] = m.second.isNull() ? -1 : static_cast<int>(m.second.u64());
      TEST_ASSERT_TRUE_MESSAGE(f.beginBoot(acks), what);
      TEST_ASSERT_TRUE(f.reserve(128));
      scanLog(f, in, unread);
      f.endBoot();
      TEST_ASSERT_FALSE_MESSAGE(f.noMemory(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["folded"].u64(), f.folded(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["twice"].u64(), f.twice(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["gap"].u64(), f.gap(), what);
      TEST_ASSERT_EQUAL_MESSAGE(b["partial"].boolean(), f.partialScan(), what);
      // The save: none after a partial scan or on a full card; nothing
      // written when nothing is new.
      bool wrote = false, compacted = false;
      if (!b["saveFails"].boolean()) {
        sf::Fold::Plan p;
        const bool ok = f.plan(b["now"].isNull() ? 0 : b["now"].u64(), !b["commitTime"].isNull(),
                               b["commitTime"].u64(), &p);
        TEST_ASSERT_EQUAL_MESSAGE(!b["partial"].boolean(), ok, what);
        if (ok && f.dirty()) {
          Bytes newBase;
          if (sf::compactionDue(p.rows[sf::kTrkd], p.head.baseRows, b["playing"].boolean(), b["safePoint"].boolean()) &&
              p.head.baseGen + 1 != named) {
            newBase = compact(f, &p, base);
            compacted = true;
            TEST_ASSERT_TRUE_MESSAGE(out.count(baseName(p.head.baseGen)) == 1, what);
            const Bytes& want = out.at(baseName(p.head.baseGen));
            TEST_ASSERT_TRUE_MESSAGE(newBase == want, (std::string(what) + ": the base, " + hex(newBase, want)).c_str());
            ++compactions;
          }
          const Bytes mine = statsBin(f, p);
          const Bytes& want = out.at("stats.bin");
          TEST_ASSERT_TRUE_MESSAGE(mine == want, (std::string(what) + ": stats.bin, " + hex(mine, want)).c_str());
          f.saved(p);
          TEST_ASSERT_FALSE(f.dirty());
          wrote = true;
          ++saves;
        }
      }
      TEST_ASSERT_EQUAL_MESSAGE(b["saved"].boolean(), wrote, what);
      TEST_ASSERT_EQUAL_MESSAGE(b["compacted"].boolean(), compacted, what);
      // The trim: by the card's MARK (the saved one, else the boot's).
      std::set<std::string> want, got;
      for (size_t t = 0; t < b["trimmed"].size(); ++t) want.insert(b["trimmed"][t].str());
      for (const char* n : kLogNames) {
        auto it = in.find(n);
        if (it == in.end() || unread.count(n) || acks.count == 0) continue;
        if (trimmed(f, it->second, acks)) got.insert(n);
      }
      TEST_ASSERT_TRUE_MESSAGE(want == got, what);
      trims += static_cast<int>(got.size());
    }
  }
  printf("fixtures: %d boots, %d saves byte for byte, %d bases, %d files trimmed\n", boots, saves, compactions, trims);
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(22, trims);
}

// ---------------------------------------------------------------------------
// Each slot once, the gap.
// ---------------------------------------------------------------------------
void test_each_slot_once() {
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  auto s = [](uint32_t q) { return slot(kDevA, kEpochA, q, 100 + q, 200000, 200000, 1, 1, at(300, 8)); };
  // plays.jnl: 10-12; plays.1.jnl: 5-9, then 10 and 11 again (a twin's
  // copy); in plays.jnl a seq at or under its last (met again: not twice).
  for (uint32_t q : {10u, 11u, 12u}) f.bootSlot(s(q), 0);
  f.bootSlot(s(11), 0);
  f.bootSlot(s(12), 0);
  for (uint32_t q : {5u, 6u, 7u, 8u, 9u, 10u, 11u}) f.bootSlot(s(q), 1);
  // plays.2.jnl: 6-7 again (inside plays.1.jnl's range), 13 new.
  for (uint32_t q : {6u, 7u, 13u}) f.bootSlot(s(q), 2);
  // Another device's seqs are its own.
  f.bootSlot(slot(kDevB, kEpochB, 6, 1, 200000, 200000), 2);
  f.endBoot();
  TEST_ASSERT_EQUAL_UINT32(10, f.folded());  // A 5-13, B 6
  TEST_ASSERT_EQUAL_UINT32(4, f.twice());    // 10, 11 in plays.1.jnl; 6, 7 in plays.2.jnl
  TEST_ASSERT_EQUAL_UINT32(4, f.head().twice);
  TEST_ASSERT_EQUAL_UINT32(13, f.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(6, f.markOf(kDevB, kEpochB));
  TEST_ASSERT_EQUAL_UINT64(10, f.head().plays);
  // Under the MARK read at the boot: not folded. Slots that don't count:
  // skipped (drop notes, epoch 0, deviceId zeros, seq 0).
  const Bytes b = saveNow(f, at(313, 9));
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  load(g, b);
  TEST_ASSERT_TRUE(g.beginBoot(none));
  for (uint32_t q = 1; q <= 13; ++q) g.bootSlot(s(q), 0);
  pl::Slot note = s(14);
  note.kind = pl::kKindDrop;
  g.bootSlot(note, 0);
  g.bootSlot(slot(kDevA, 0, 14, 1, 200000, 200000), 0);
  g.bootSlot(slot(kZeros, kEpochA, 14, 1, 200000, 200000), 0);
  g.bootSlot(slot(kDevA, kEpochA, 0, 1, 200000, 200000), 0);
  g.bootSlot(s(14), 0);
  g.endBoot();
  TEST_ASSERT_EQUAL_UINT32(1, g.folded());
  TEST_ASSERT_EQUAL_UINT32(14, g.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(0, g.markOf(kZeros, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(0, g.markOf(kDevA, 0));
  TEST_ASSERT_EQUAL_UINT64(11, g.head().plays);
}

void test_the_gap() {
  // MARK (A) 10 on the card; ACKS (A) 50: the log holds 41-60, and 45 a
  // second time in another file; seqs 11-40 are gone: 30.
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  for (uint32_t q = 1; q <= 10; ++q) f.bootSlot(slot(kDevA, kEpochA, q, 5, 200000, 200000, 1, 1, at(300, 8)), 0);
  const Bytes card = saveNow(f, at(300, 20));
  cc::msmf::Acks acks;
  acks.present = true;
  acks.count = acks.rows = 3;
  std::memcpy(acks.entries[0].deviceId, kDevA, 6);
  acks.entries[0].epoch = kEpochA;
  acks.entries[0].seq = 50;
  // A row of a key that never counts (epoch 0), 3 seqs, one held in the
  // log: counted (2) and given a MARK row, as statsref does.
  std::memcpy(acks.entries[1].deviceId, kDevB, 6);
  acks.entries[1].epoch = 0;
  acks.entries[1].seq = 3;
  // A row at or under MARK: nothing.
  std::memcpy(acks.entries[2].deviceId, kDevC, 6);
  acks.entries[2].epoch = kEpochB;
  acks.entries[2].seq = 0;
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  load(g, card);
  TEST_ASSERT_TRUE(g.beginBoot(acks));
  for (uint32_t q = 41; q <= 60; ++q) g.bootSlot(slot(kDevA, kEpochA, q, 6, 200000, 200000, 1, 1, at(310, 8)), 0);
  g.bootSlot(slot(kDevA, kEpochA, 45, 6, 200000, 200000, 1, 1, at(310, 8)), 1);
  g.bootSlot(slot(kDevB, 0, 2, 6, 200000, 200000, 1, 1, at(310, 8)), 1);
  g.bootSlot(slot(kDevB, 0, 2, 6, 200000, 200000, 1, 1, at(310, 8)), 0);  // (the same seq: held once)
  g.endBoot();
  TEST_ASSERT_EQUAL_UINT32(20, g.folded());
  TEST_ASSERT_EQUAL_UINT32(1, g.twice());
  TEST_ASSERT_EQUAL_UINT32(30 + 2, g.gap());
  TEST_ASSERT_EQUAL_UINT32(32, g.head().gap);
  TEST_ASSERT_EQUAL_UINT32(60, g.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(3, g.markOf(kDevB, 0));
  TEST_ASSERT_EQUAL_UINT32(0, g.markOf(kDevC, kEpochB));
  TEST_ASSERT_TRUE(g.dirty());
  // The next boot: nothing new, the gap not counted again.
  const Bytes next = saveNow(g, at(313, 9));
  sf::Fold h;
  TEST_ASSERT_TRUE(h.begin());
  load(h, next);
  TEST_ASSERT_TRUE(h.beginBoot(acks));
  for (uint32_t q = 41; q <= 60; ++q) h.bootSlot(slot(kDevA, kEpochA, q, 6, 200000, 200000, 1, 1, at(310, 8)), 0);
  h.endBoot();
  TEST_ASSERT_EQUAL_UINT32(0, h.gap());
  TEST_ASSERT_EQUAL_UINT32(32, h.head().gap);
  TEST_ASSERT_FALSE(h.dirty());
  // A partial scan counts no gap (the slots it didn't read may hold them),
  // and plans no save.
  sf::Fold p;
  TEST_ASSERT_TRUE(p.begin());
  load(p, card);
  TEST_ASSERT_TRUE(p.beginBoot(acks));
  for (uint32_t q = 41; q <= 45; ++q) p.bootSlot(slot(kDevA, kEpochA, q, 6, 200000, 200000, 1, 1, at(310, 8)), 0);
  p.partial();
  p.endBoot();
  TEST_ASSERT_EQUAL_UINT32(0, p.gap());
  TEST_ASSERT_EQUAL_UINT32(45, p.markOf(kDevA, kEpochA));
  TEST_ASSERT_TRUE(p.partialScan());
  sf::Fold::Plan plan;
  TEST_ASSERT_FALSE(p.plan(at(313, 9), false, 0, &plan));
}

// ---------------------------------------------------------------------------
// Rounds, the live fold, MARK, sinceDay.
// ---------------------------------------------------------------------------
void test_rounds_and_the_live_fold() {
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  cc::msmf::Acks none;
  // A card whose HEAD says round 7.
  TEST_ASSERT_TRUE(f.beginBoot(none));
  f.bootSlot(slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(300, 8)), 0);
  f.endBoot();
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(at(300, 9), false, 0, &p));
  p.head.round = 7;
  const Bytes card = statsBin(f, p);
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  load(g, card);
  TEST_ASSERT_TRUE(g.beginBoot(none));
  g.bootSlot(slot(kDevA, kEpochA, 2, 2, 200000, 200000, 1, 1, 0), 1);  // undated: ordered by its round
  g.endBoot();
  sf::Track t;
  TEST_ASSERT_TRUE(g.track(2, &t));
  TEST_ASSERT_EQUAL_UINT32(8, t.lastOrd);  // the boot's: HEAD.round + 1
  g.liveSlot(slot(kDevA, kEpochA, 3, 3, 200000, 200000, 1, 1, 0));
  TEST_ASSERT_TRUE(g.track(3, &t));
  TEST_ASSERT_EQUAL_UINT32(9, t.lastOrd);  // live: + 2
  // A live slot that doesn't count, or a drop note: nothing.
  g.liveSlot(slot(kDevA, 0, 4, 4, 200000, 200000));
  TEST_ASSERT_FALSE(g.track(4, &t));
  // The save writes the highest round used; a later live fold this session
  // is still + 2 (of HEAD.round as loaded).
  TEST_ASSERT_TRUE(g.plan(at(301, 9), false, 0, &p));
  TEST_ASSERT_EQUAL_UINT32(9, p.head.round);
  g.saved(p);
  g.liveSlot(slot(kDevA, kEpochA, 5, 3, 200000, 200000, 1, 1, 0));
  TEST_ASSERT_TRUE(g.track(3, &t));
  TEST_ASSERT_EQUAL_UINT32(9, t.lastOrd);
  TEST_ASSERT_EQUAL_UINT16(2, t.plays);
  TEST_ASSERT_EQUAL_UINT32(3, g.folded());
  // A round at its 24-bit top saturates.
  f.clear();
  TEST_ASSERT_TRUE(f.beginBoot(none));
  f.endBoot();
  TEST_ASSERT_TRUE(f.plan(at(300, 9), false, 0, &p));
  p.head.round = sf::kMaxRound - 1;
  sf::Fold top;
  TEST_ASSERT_TRUE(top.begin());
  load(top, statsBin(f, p));
  top.liveSlot(slot(kDevA, kEpochA, 9, 9, 200000, 200000));
  TEST_ASSERT_TRUE(top.track(9, &t));
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, t.lastOrd);
}

void test_mark_kept_for_good_and_covered() {
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  // 70 (deviceId, epoch) pairs: MARK's block of 64 made larger (twice).
  for (uint32_t e = 1; e <= 70; ++e) f.bootSlot(slot(kDevA, e, 3, e, 200000, 200000), e % 2);
  f.endBoot();
  TEST_ASSERT_EQUAL_UINT32(70, f.rows(sf::kMark));
  // covered(): the card's MARK, not RAM's, until a save lands.
  const pl::Slot s3 = slot(kDevA, 5, 3, 1, 1, 1), s0 = slot(kDevA, 5, 0, 1, 1, 1);
  TEST_ASSERT_FALSE(f.covered(s3));
  TEST_ASSERT_TRUE(f.covered(s0));  // (seq 0: covered with no row, as acknowledged() reads it)
  TEST_ASSERT_EQUAL_UINT32(3, f.markOf(kDevA, 5));
  const Bytes b = saveNow(f, at(300, 9));
  TEST_ASSERT_TRUE(f.covered(s3));
  TEST_ASSERT_FALSE(f.covered(slot(kDevA, 5, 4, 1, 1, 1)));
  TEST_ASSERT_FALSE(f.covered(slot(kDevB, 5, 1, 1, 1, 1)));
  // A live fold raises RAM's MARK, not the card's.
  f.liveSlot(slot(kDevA, 5, 4, 1, 200000, 200000));
  TEST_ASSERT_EQUAL_UINT32(4, f.markOf(kDevA, 5));
  TEST_ASSERT_FALSE(f.covered(slot(kDevA, 5, 4, 1, 1, 1)));
  // Kept for good: every row comes back from the file and goes into the
  // next, though nothing of the pair is folded again.
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  load(g, b);
  TEST_ASSERT_EQUAL_UINT32(70, g.rows(sf::kMark));
  TEST_ASSERT_TRUE(g.beginBoot(none));
  g.bootSlot(slot(kDevB, kEpochB, 1, 99, 200000, 200000), 0);
  g.endBoot();
  TEST_ASSERT_TRUE(g.covered(s3));
  const Bytes c = saveNow(g, at(301, 9));
  sf::Fold h;
  TEST_ASSERT_TRUE(h.begin());
  load(h, c);
  TEST_ASSERT_EQUAL_UINT32(71, h.rows(sf::kMark));
  for (uint32_t e = 1; e <= 70; ++e) TEST_ASSERT_EQUAL_UINT32(3, h.markOf(kDevA, e));
}

void test_since_day() {
  cc::msmf::Acks none;
  auto sinceOf = [&](const std::vector<pl::Slot>& slots, uint64_t now, bool hasCommit, uint64_t commit,
                     uint16_t* flags) {
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    TEST_ASSERT_TRUE(f.beginBoot(none));
    for (const pl::Slot& s : slots) f.bootSlot(s, 0);
    f.endBoot();
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(now, hasCommit, commit, &p));
    *flags = p.head.flags;
    return p.head.sinceDay;
  };
  uint16_t flags = 0;
  const uint64_t commit = (at(305, 12) / 1000);
  // The earliest dated play: a skip earlier doesn't count, nor an undated play.
  std::vector<pl::Slot> s = {slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(290, 8)),
                             slot(kDevA, kEpochA, 2, 1, 5000, 200000, pl::kSkipped, 1, at(280, 8)),
                             slot(kDevA, kEpochA, 3, 1, 200000, 200000, 1, 1, 0)};
  TEST_ASSERT_EQUAL_UINT16(290, sinceOf(s, at(313, 9), true, commit, &flags));
  TEST_ASSERT_EQUAL_UINT16(0, flags);
  // Not after today (a foreign slot dated ahead).
  std::vector<pl::Slot> ahead = {slot(kDevB, kEpochB, 1, 1, 200000, 200000, 1, 1, at(900, 8))};
  TEST_ASSERT_EQUAL_UINT16(313, sinceOf(ahead, at(313, 9), false, 0, &flags));
  // The clock unset: the slot's day, ahead or not (nothing to cap it by).
  TEST_ASSERT_EQUAL_UINT16(900, sinceOf(ahead, 0, true, commit, &flags));
  // No dated play: today; no today: the root's commitTime, flagged; a
  // commitTime not believed: unset.
  std::vector<pl::Slot> undated = {slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, 0)};
  TEST_ASSERT_EQUAL_UINT16(313, sinceOf(undated, at(313, 9), true, commit, &flags));
  TEST_ASSERT_EQUAL_UINT16(305, sinceOf(undated, 0, true, commit, &flags));
  TEST_ASSERT_EQUAL_UINT16(sf::kFlagSinceCommit, flags);
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, sinceOf(undated, 0, true, 1000, &flags));
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, sinceOf(undated, 0, false, 0, &flags));
  TEST_ASSERT_EQUAL_UINT16(0, flags);
  // Set once: a later save keeps it.
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  TEST_ASSERT_TRUE(f.beginBoot(none));
  f.bootSlot(s[0], 0);
  f.endBoot();
  const Bytes b = saveNow(f, at(313, 9));
  sf::Fold g;
  TEST_ASSERT_TRUE(g.begin());
  load(g, b);
  g.liveSlot(slot(kDevA, kEpochA, 9, 1, 200000, 200000, 1, 1, at(100, 8)));
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(g.plan(at(314, 9), false, 0, &p));
  TEST_ASSERT_EQUAL_UINT16(290, p.head.sinceDay);
}

// ---------------------------------------------------------------------------
// The retention (5.2).
// ---------------------------------------------------------------------------
void test_hour_and_days_keep_the_newest() {
  // 1,500 hours over 600 days, folded in a shuffled order and in a newest-
  // first order: HOUR keeps its newest 840, DAYS its newest 400, whatever
  // the order (5.2), as the model does at the save.
  for (int order = 0; order < 3; ++order) {
    std::vector<pl::Slot> slots;
    for (uint32_t i = 0; i < 1500; ++i) {
      const uint32_t day = 10 + (i * 2) / 5, hour = (i * 7) % 24;
      for (uint32_t k = 0; k < 1 + i % 3; ++k)
        slots.push_back(slot(kDevA, kEpochA, 0, hashOf(i % 97), 20000 + 15000 * k, 200000, 1 + k % 3,
                             static_cast<uint8_t>(k % 4), at(day, hour, k)));
    }
    Rng rng(order + 1);
    if (order == 1) std::reverse(slots.begin(), slots.end());
    if (order == 2)
      for (size_t i = slots.size(); i > 1; --i) std::swap(slots[i - 1], slots[rng.below(static_cast<uint32_t>(i))]);
    for (size_t i = 0; i < slots.size(); ++i) slots[i].seq = static_cast<uint32_t>(i + 1);
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    for (const pl::Slot& s : slots) {
      f.liveSlot(s);
      m.fold(s, 2);
    }
    TEST_ASSERT_EQUAL_UINT32(840, f.rows(sf::kHour));
    TEST_ASSERT_EQUAL_UINT32(400, f.rows(sf::kDays));
    const uint64_t now = at(700, 12);
    const Bytes mine = saveNow(f, now);
    m.save(now);
    TEST_ASSERT_TRUE_MESSAGE(mine == m.file(), hex(mine, m.file()).c_str());
  }
}

void test_rcnt_retention() {
  // 40 days (300-339) of 3 tracks; today 345: RCNT keeps days 308 on (32
  // back from 339, the newest day folded, earlier than today).
  auto build = [](sf::Fold& f, Model& m, uint32_t from, uint32_t days, uint32_t tracks) {
    uint32_t q = 0;
    for (uint32_t d = from; d < from + days; ++d)
      for (uint32_t t = 0; t < tracks; ++t) {
        const pl::Slot s = slot(kDevA, kEpochA, ++q, hashOf(t), 200000, 200000, 1, 1, at(d, 8, t % 60));
        f.liveSlot(s);
        m.fold(s, 2);
      }
  };
  {
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    build(f, m, 300, 40, 3);
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(at(345, 9), false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(308, p.rcntCut);
    TEST_ASSERT_EQUAL_UINT32(32 * 3, p.rows[sf::kRcnt]);
    const Bytes mine = statsBin(f, p);
    f.saved(p);
    m.save(at(345, 9));
    TEST_ASSERT_TRUE(mine == m.file());
    TEST_ASSERT_EQUAL_UINT32(96, f.rows(sf::kRcnt));
    // Today earlier than the newest day (a slot from a fast clock): from today.
    TEST_ASSERT_TRUE(f.plan(at(320, 9), false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(320 - 31, p.rcntCut);
  }
  {
    // The clock unset: nothing by date, then the cap.
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    build(f, m, 100, 40, 3);
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(0, false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(0, p.rcntCut);
    TEST_ASSERT_EQUAL_UINT32(120, p.rows[sf::kRcnt]);
    const Bytes mine = statsBin(f, p);
    m.save(0);
    TEST_ASSERT_TRUE(mine == m.file());
  }
  {
    // Past 4,096 rows: whole days, oldest first (3 days of 2,000: one goes).
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    build(f, m, 200, 3, 2000);
    TEST_ASSERT_EQUAL_UINT32(6000, f.rows(sf::kRcnt));
    TEST_ASSERT_TRUE(f.chunks(sf::kRcnt) >= 5);
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(at(202, 20), false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(201, p.rcntCut);
    TEST_ASSERT_EQUAL_UINT32(4000, p.rows[sf::kRcnt]);
    const Bytes mine = statsBin(f, p);
    f.saved(p);
    m.save(at(202, 20));
    TEST_ASSERT_TRUE(mine == m.file());
    TEST_ASSERT_EQUAL_UINT32(4000, f.rows(sf::kRcnt));
    TEST_ASSERT_EQUAL_UINT32(3, f.chunks(sf::kRcnt));  // (the rest given back)
  }
  {
    // One day of 4,097 tracks goes whole: RCNT empty.
    sf::Fold f;
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    build(f, m, 200, 1, 4097);
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(at(200, 20), false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(0, p.rows[sf::kRcnt]);
    TEST_ASSERT_EQUAL_UINT32(201, p.rcntCut);
    const Bytes mine = statsBin(f, p);
    m.save(at(200, 20));
    TEST_ASSERT_TRUE(mine == m.file());
  }
}

// ---------------------------------------------------------------------------
// Random folds against the model.
// ---------------------------------------------------------------------------
void test_random_folds_against_the_model() {
  const uint8_t* devs[3] = {kDevA, kDevB, kDevC};
  const uint64_t epochs[3] = {kEpochA, kEpochB, 0x77};
  const size_t live0 = gHeap.live;
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    Rng rng(seed * 7919);
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    Model m;
    uint32_t seq[3] = {};
    uint64_t now = at(200, 0);
    Bytes card;
    // Three sessions: a boot's fold (two files, newest first), live folds,
    // a save; the next session loads the card.
    for (int session = 0; session < 3; ++session) {
      if (session > 0) {
        f.clear();
        load(f, card);
      }
      cc::msmf::Acks none;
      TEST_ASSERT_TRUE(f.beginBoot(none));
      const uint32_t base = f.head().round;
      const uint32_t n = 1500 + rng.below(2500);
      std::vector<std::pair<pl::Slot, int>> boot;
      for (uint32_t i = 0; i < n; ++i) {
        const int d = rng.below(3);
        const uint32_t pick = rng.below(10);
        uint64_t start = 0;
        if (pick < 7) start = now - static_cast<uint64_t>(rng.below(60 * 24)) * 3600000ull - rng.below(3600000);
        if (pick == 7) start = at(27000, 1);  // a foreign clock, 2099
        // (pick 8, 9: undated.)
        pl::Slot s = slot(devs[d], epochs[d], ++seq[d], hashOf(rng.below(2600)), rng.below(300000),
                          rng.below(5) ? 1 + rng.below(400000) : 0, static_cast<uint8_t>(rng.below(5)),
                          static_cast<uint8_t>(rng.below(5)), start);
        boot.push_back({s, i < n / 2 ? 1 : 0});
      }
      // The scan reads plays.jnl (the newer half) first.
      for (int file = 0; file <= 1; ++file)
        for (const auto& b : boot)
          if (b.second == file) {
            f.bootSlot(b.first, file);
            m.fold(b.first, base + 1);
          }
      f.endBoot();
      for (uint32_t i = 0; i < 300; ++i) {
        const int d = rng.below(3);
        const pl::Slot s = slot(devs[d], epochs[d], ++seq[d], hashOf(2600 + rng.below(900)), 20000 + rng.below(200000),
                                200000, 1 + rng.below(3), static_cast<uint8_t>(rng.below(4)), now + i * 60000ull);
        f.liveSlot(s);
        m.fold(s, base + 2);
      }
      TEST_ASSERT_FALSE(f.noMemory());
      now += 9 * static_cast<uint64_t>(sf::kDayMs);
      card = saveNow(f, now);
      m.save(now);
      const Bytes want = m.file();
      TEST_ASSERT_TRUE_MESSAGE(card == want, hex(card, want).c_str());
      TEST_ASSERT_EQUAL_UINT32(m.trkd.size(), f.rows(sf::kTrkd));
      m.used = 0;
      m.anyUsed = false;
      m.firstPlayDay = -1;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(live0, gHeap.live);  // (every block given back)
}

// ---------------------------------------------------------------------------
// The memory (5.6).
// ---------------------------------------------------------------------------
void test_the_memory() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  {
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_TRUE(f.begin());
    // HOUR, DAYS and MARK carved once, at their caps.
    TEST_ASSERT_EQUAL_UINT32(3, gHeap.carved.size());
    TEST_ASSERT_EQUAL_UINT32(6720, gHeap.carved[0]);
    TEST_ASSERT_EQUAL_UINT32(6400, gHeap.carved[1]);
    TEST_ASSERT_EQUAL_UINT32(1536, gHeap.carved[2]);
    TEST_ASSERT_EQUAL_UINT32(6720 + 6400 + 1536, f.held());
    f.clear();
    // The load's chunks: what stats.bin needs, and one spare.
    gHeap.carved.clear();
    TEST_ASSERT_TRUE(f.reserveLoad(2000, 700));
    TEST_ASSERT_EQUAL_UINT32(2, f.chunks(sf::kRcnt));  // 1,365 a chunk
    TEST_ASSERT_EQUAL_UINT32(2, f.chunks(sf::kTrkd));  // 682 a chunk
    TEST_ASSERT_TRUE(f.spare());
    for (size_t n : gHeap.carved) TEST_ASSERT_EQUAL_UINT32(sf::kChunkBytes, n);
    TEST_ASSERT_EQUAL_UINT32(5, gHeap.carved.size());
    // The boot's fold: each index a power of two at least twice the rows.
    cc::msmf::Acks none;
    TEST_ASSERT_TRUE(f.beginBoot(none));
    TEST_ASSERT_EQUAL_UINT32(256, f.indexSize(sf::kTrkd));
    // reserve(): the next folds carve nothing; past half full the index
    // is carved again at twice the size and filled from the rows.
    uint32_t q = 0;
    for (int piece = 0; piece < 40; ++piece) {
      TEST_ASSERT_TRUE(f.reserve(128));
      const size_t before = gHeap.carved.size();
      for (int k = 0; k < 128; ++k) {
        ++q;
        f.bootSlot(slot(kDevA, kEpochA, q, hashOf(q), 200000, 200000, 1, 1, at(100 + q / 50, q % 24)), 0);
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, gHeap.carved.size(), "a fold carved after reserve()");
    }
    TEST_ASSERT_EQUAL_UINT32(5120, f.rows(sf::kTrkd));
    TEST_ASSERT_EQUAL_UINT32(16384, f.indexSize(sf::kTrkd));
    TEST_ASSERT_EQUAL_UINT32(16384, f.indexSize(sf::kRcnt));
    TEST_ASSERT_EQUAL_UINT32(8, f.chunks(sf::kTrkd));
    for (size_t n : gHeap.carved)
      TEST_ASSERT_TRUE_MESSAGE(n == sf::kChunkBytes || (n >= 512 && (n & (n - 1)) == 0) || n == 64 * 10 * 8,
                               "a block that is no chunk, index or range");
    const size_t peak = f.peak();
    f.endBoot();
    // The indexes and the ranges freed at the scan's end.
    TEST_ASSERT_EQUAL_UINT32(0, f.indexSize(sf::kTrkd));
    TEST_ASSERT_TRUE(f.held() < peak);
    TEST_ASSERT_EQUAL_UINT32(gHeap.live - live0, f.held());
    // A compaction's save gives TRKD's chunks back (one as the spare).
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(at(300, 9), false, 0, &p));
    p.compact = true;
    f.saved(p);
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks(sf::kTrkd));
    TEST_ASSERT_TRUE(f.spare());
  }
  TEST_ASSERT_EQUAL_UINT32(live0, gHeap.live);
}

void test_no_memory() {
  const size_t live0 = gHeap.live;
  cc::msmf::Acks none;
  std::vector<pl::Slot> slots;
  for (uint32_t q = 1; q <= 3000; ++q)
    slots.push_back(slot(kDevA, kEpochA, q, hashOf(q % 1500), 200000, 200000, 1, 1, at(200 + q / 100, q % 24)));
  // The control: everything carved.
  Bytes want;
  {
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    TEST_ASSERT_TRUE(f.beginBoot(none));
    for (const pl::Slot& s : slots) f.bootSlot(s, 0);
    f.endBoot();
    want = saveNow(f, at(240, 9));
  }
  // No memory for an index: each chunk's order finds the same rows.
  gHeap.reset();
  for (size_t size : {size_t{512}, size_t{1024}, size_t{2048}, size_t{8192}}) {
    gHeap.failSize = size;
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    TEST_ASSERT_TRUE(f.beginBoot(none));
    for (const pl::Slot& s : slots) f.bootSlot(s, 0);
    f.endBoot();
    TEST_ASSERT_FALSE(f.noMemory());
    gHeap.failSize = 0;
    TEST_ASSERT_TRUE(saveNow(f, at(240, 9)) == want);
  }
  // No memory at each carve in turn: either the fold is whole and the same,
  // or it stopped (noMemory) and plans no save. Never a wrong file.
  int stopped = 0, whole = 0;
  for (int n = 0; n < 40; ++n) {
    gHeap.reset();
    gHeap.failAfter = n;
    sf::Fold f(heapAlloc, heapFree);
    const bool began = f.begin();
    if (began) {
      f.clear();
      if (f.beginBoot(none)) {
        for (const pl::Slot& s : slots) f.bootSlot(s, 0);
        f.endBoot();
      }
    }
    gHeap.failAfter = -1;
    sf::Fold::Plan p;
    if (!began || f.noMemory()) {
      ++stopped;
      if (began) TEST_ASSERT_FALSE(f.plan(at(240, 9), false, 0, &p));
      continue;
    }
    ++whole;
    TEST_ASSERT_TRUE(saveNow(f, at(240, 9)) == want);
  }
  TEST_ASSERT_TRUE(stopped > 0 && whole > 0);
  TEST_ASSERT_EQUAL_UINT32(live0, gHeap.live);
}

void test_load_checks_the_order() {
  // Rows not strictly increasing in any section: corrupt (5.4).
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  f.bootSlot(slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(300, 8)), 0);
  f.bootSlot(slot(kDevB, kEpochB, 1, 2, 200000, 200000, 1, 1, at(301, 9)), 0);
  f.endBoot();
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(at(302, 9), false, 0, &p));
  for (int sec = 1; sec < sf::kSecs; ++sec) {
    const Bytes rows = rowsOf(f, p, static_cast<sf::Sec>(sec));
    const uint32_t st = sf::kStrides[sec];
    TEST_ASSERT_EQUAL_UINT32(2 * st, rows.size());
    sf::Fold g;
    TEST_ASSERT_TRUE(g.begin());
    g.clear();
    TEST_ASSERT_TRUE(g.loadRows(static_cast<sf::Sec>(sec), reinterpret_cast<const uint8_t*>(rows.data()), 2, st));
    g.clear();
    const Bytes swapped = rows.substr(st) + rows.substr(0, st);
    TEST_ASSERT_FALSE(g.loadRows(static_cast<sf::Sec>(sec), reinterpret_cast<const uint8_t*>(swapped.data()), 2, st));
    g.clear();
    const Bytes twice = rows.substr(0, st) + rows.substr(0, st);
    TEST_ASSERT_FALSE(g.loadRows(static_cast<sf::Sec>(sec), reinterpret_cast<const uint8_t*>(twice.data()), 2, st));
    // A newer minor's longer rows, by their prefix; a stride under the
    // section's: refused.
    g.clear();
    Bytes longer;
    for (int r = 0; r < 2; ++r) longer += rows.substr(r * st, st) + Bytes(8, '\xee');
    TEST_ASSERT_TRUE(g.loadRows(static_cast<sf::Sec>(sec), reinterpret_cast<const uint8_t*>(longer.data()), 2, st + 8));
    TEST_ASSERT_EQUAL_UINT32(2, g.rows(static_cast<sf::Sec>(sec)));
    g.clear();
    TEST_ASSERT_FALSE(g.loadRows(static_cast<sf::Sec>(sec), reinterpret_cast<const uint8_t*>(rows.data()), 1, st - 4));
  }
}

void test_compaction_rule() {
  // 5.3: nothing playing: over 32 KB, or over a quarter of the base; any
  // delta over no base. Playing: only past 96 KB, at a safe point.
  TEST_ASSERT_TRUE(sf::compactionDue(1, 0, false, false));
  TEST_ASSERT_FALSE(sf::compactionDue(0, 0, false, true));
  TEST_ASSERT_FALSE(sf::compactionDue(1, 0, true, true));
  TEST_ASSERT_FALSE(sf::compactionDue(250, 1000, false, true));
  TEST_ASSERT_TRUE(sf::compactionDue(251, 1000, false, true));
  TEST_ASSERT_TRUE(sf::compactionDue(1366, 100000, false, false));  // 32,784 B
  TEST_ASSERT_FALSE(sf::compactionDue(1365, 100000, false, false));
  TEST_ASSERT_FALSE(sf::compactionDue(4096, 100000, true, true));   // 98,304 B: at 96 KB, not past
  TEST_ASSERT_TRUE(sf::compactionDue(4097, 100000, true, true));
  TEST_ASSERT_FALSE(sf::compactionDue(4097, 100000, true, false));
}

// release(): the stats off for the session: every block given back, and
// begin() and a load carve them again.
void test_release_gives_every_block_back() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  sf::Fold f(heapAlloc, heapFree);
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  TEST_ASSERT_TRUE(f.reserveLoad(2000, 700));
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  for (uint32_t q = 1; q <= 300; ++q)
    f.bootSlot(slot(kDevA, kEpochA, q, hashOf(q), 200000, 200000, 1, 1, at(100, 8)), 0);
  TEST_ASSERT_TRUE(f.held() > 0);
  f.release();
  TEST_ASSERT_EQUAL_size_t(0, f.held());
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  TEST_ASSERT_FALSE(f.booting());
  TEST_ASSERT_FALSE(f.spare());
  TEST_ASSERT_EQUAL_UINT32(0, f.chunks(sf::kRcnt) + f.chunks(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT32(0, f.folded());
  // Again: the same as a fresh fold's.
  TEST_ASSERT_TRUE(f.begin());
  TEST_ASSERT_EQUAL_size_t(6720 + 6400 + 1536, f.held());
  f.release();
  f.release();  // (twice: nothing more)
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// A library update's give-back (docs/LISTENING-STATS.md 6.7): every block
// but MARK's given back, exactly what givesBack() said; covered() and
// markOf() answer as before (the card's MARK and RAM's); nothing while the
// boot's fold runs; begin() carves HOUR and DAYS again (MARK's block kept)
// for the next load.
void test_give_back_keeps_mark_alone() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  sf::Fold f(heapAlloc, heapFree);
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  TEST_ASSERT_TRUE(f.reserveLoad(2000, 700));
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  for (uint32_t q = 1; q <= 300; ++q)
    f.bootSlot(slot(kDevA, kEpochA, q, hashOf(q), 200000, 200000, 1, 1, at(100, 8)), 0);
  TEST_ASSERT_EQUAL_size_t(0, f.givesBack());  // (the boot's fold: its blocks are the scan's)
  TEST_ASSERT_EQUAL_size_t(0, f.giveBack());
  TEST_ASSERT_TRUE(f.booting());
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(0, false, 0, &p));  // (the boot's end; the save landed:)
  f.saved(p);
  f.liveSlot(slot(kDevA, kEpochA, 301, hashOf(301), 200000, 200000, 1, 1, at(100, 9)));
  TEST_ASSERT_TRUE(f.dirty());
  const size_t mark = sf::kMarkBlockRows * sizeof(sf::Fold::MarkRow);
  const size_t gives = f.givesBack();
  TEST_ASSERT_EQUAL_size_t(f.held() - mark, gives);
  TEST_ASSERT_EQUAL_size_t(gives, f.giveBack());
  TEST_ASSERT_EQUAL_size_t(mark, f.held());
  TEST_ASSERT_EQUAL_size_t(live0 + mark, gHeap.live);
  TEST_ASSERT_EQUAL_size_t(0, f.givesBack());
  TEST_ASSERT_EQUAL_size_t(0, f.giveBack());  // (twice: nothing more)
  TEST_ASSERT_FALSE(f.spare());
  TEST_ASSERT_EQUAL_UINT32(0, f.chunks(sf::kRcnt) + f.chunks(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT32(0, f.rows(sf::kHour) + f.rows(sf::kDays) + f.rows(sf::kRcnt) + f.rows(sf::kTrkd));
  // MARK kept: the card's 300 (the save's), RAM's 301.
  TEST_ASSERT_EQUAL_UINT32(1, f.rows(sf::kMark));
  TEST_ASSERT_TRUE(f.covered(slot(kDevA, kEpochA, 300, 1, 1, 1, 1, 1, 0)));
  TEST_ASSERT_FALSE(f.covered(slot(kDevA, kEpochA, 301, 1, 1, 1, 1, 1, 0)));
  TEST_ASSERT_EQUAL_UINT32(301, f.markOf(kDevA, kEpochA));
  uint8_t plays = 0;
  uint32_t ms = 0;
  TEST_ASSERT_FALSE(f.hour(sf::hourOf(at(100, 8)), &plays, &ms));  // (no HOUR held)
  // The next load: HOUR and DAYS again, MARK's block the same.
  TEST_ASSERT_TRUE(f.begin());
  TEST_ASSERT_EQUAL_size_t(6720 + 6400 + mark, f.held());
  f.release();
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// ---------------------------------------------------------------------------
// A full log never synced.
// ---------------------------------------------------------------------------
namespace {

// Both files at the cap: 13,106 slots, 25 plays a day for 524 days, of
// 5,000 tracks.
std::vector<pl::Slot> fullLog(std::set<uint64_t>* hashes) {
  const uint32_t total = 2 * pl::kFileSlots;
  std::vector<pl::Slot> slots;
  Rng rng(2026);
  for (uint32_t q = 1; q <= total; ++q) {
    const uint64_t h = hashOf(rng.below(5000));
    if (hashes) hashes->insert(h);
    slots.push_back(slot(kDevA, kEpochA, q, h, 30000 + rng.below(200000), 200000, 1 + rng.below(3),
                         static_cast<uint8_t>(rng.below(4)), at(q / 25, 8 + (q % 25) / 2, q % 60)));
  }
  return slots;
}

// Its boot's fold, as the glue would: the scan reads plays.jnl (the newer)
// first, reserve() before each piece of 128 slots; and into the model.
void foldFullLog(sf::Fold& f, Model* m, const std::vector<pl::Slot>& slots) {
  cc::msmf::Acks none;
  TEST_ASSERT_TRUE(f.beginBoot(none));
  uint32_t piece = 0;
  for (int file = 0; file <= 1; ++file) {
    const uint32_t from = file == 0 ? pl::kFileSlots : 0;
    for (uint32_t i = from; i < from + pl::kFileSlots; ++i) {
      if (piece++ % 128 == 0) TEST_ASSERT_TRUE(f.reserve(128));
      f.bootSlot(slots[i], file);
      if (m) m->fold(slots[i], 1);
    }
  }
}

}  // namespace

void test_a_full_log_never_synced() {
  // As the glue would: reserve() before each piece of 128 slots.
  const uint32_t total = 2 * pl::kFileSlots;
  std::set<uint64_t> hashes;
  const std::vector<pl::Slot> slots = fullLog(&hashes);
  gHeap.reset();
  sf::Fold f(heapAlloc, heapFree);
  TEST_ASSERT_TRUE(f.begin());
  f.clear();
  TEST_ASSERT_TRUE(f.reserveLoad(0, 0));
  f.resetPeak();
  Model m;
  const auto t0 = std::chrono::steady_clock::now();
  foldFullLog(f, nullptr, slots);
  const size_t rcntPeakRows = f.rows(sf::kRcnt), rcntPeakChunks = f.chunks(sf::kRcnt);
  f.endBoot();
  const auto t1 = std::chrono::steady_clock::now();
  for (int file = 0; file <= 1; ++file) {
    const uint32_t from = file == 0 ? pl::kFileSlots : 0;
    for (uint32_t i = from; i < from + pl::kFileSlots; ++i) m.fold(slots[i], 1);
  }
  TEST_ASSERT_EQUAL_UINT32(total, f.folded());
  TEST_ASSERT_EQUAL_UINT32(hashes.size(), f.rows(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT32(m.rcnt.size(), rcntPeakRows);
  const size_t trkdChunks = f.chunks(sf::kTrkd);
  const Bytes mine = saveNow(f, at(530, 9));
  m.save(at(530, 9));
  TEST_ASSERT_TRUE_MESSAGE(mine == m.file(), hex(mine, m.file()).c_str());
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  printf("full log: %u slots, %u hashes (TRKD %u chunks), RCNT %u rows (%u chunks) at the fold's end; "
         "peak %u bytes held (heap %u), %.2f us a slot on this host; stats.bin %u bytes\n",
         static_cast<unsigned>(total), static_cast<unsigned>(hashes.size()), static_cast<unsigned>(trkdChunks),
         static_cast<unsigned>(rcntPeakRows), static_cast<unsigned>(rcntPeakChunks), static_cast<unsigned>(f.peak()),
         static_cast<unsigned>(gHeap.peak), us / total, static_cast<unsigned>(mine.size()));
  TEST_ASSERT_TRUE(f.peak() < 512 * 1024);
}

// A boot whose save doesn't land (refused, failed, held): RCNT, pruned
// only by a save that lands, is held to its 4,096-row cap by whole days at
// the fold's end (the save's cut without the date's), not a full log's
// 13,000 rows for the session. A later save writes the file statsref's
// would (the model's, which keeps every row till then): live plays dated
// before that cut (a clock gone back) add no RCNT row, which that save
// cuts anyway; with the clock unset (no date cut), and with it set.
void test_a_boot_whose_save_doesnt_land() {
  const std::vector<pl::Slot> slots = fullLog(nullptr);
  size_t heldAfter = 0, heldLanded = 0;
  for (int clock = 0; clock < 2; ++clock) {
    gHeap.reset();
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_TRUE(f.begin());
    f.clear();
    TEST_ASSERT_TRUE(f.reserveLoad(0, 0));
    Model m;
    foldFullLog(f, &m, slots);
    TEST_ASSERT_TRUE(f.rows(sf::kRcnt) > sf::kRcntRows);
    f.endBoot();  // (the boot's save refused: the glue ends the fold)
    TEST_ASSERT_TRUE(f.rows(sf::kRcnt) <= sf::kRcntRows);
    TEST_ASSERT_TRUE(f.rows(sf::kRcnt) > sf::kRcntRows - 25);  // (whole days of at most 25 rows)
    TEST_ASSERT_TRUE(f.chunks(sf::kRcnt) <= 4);
    TEST_ASSERT_TRUE(f.rcntFloor() > 0);
    TEST_ASSERT_TRUE(f.dirty());
    if (clock == 0) heldAfter = f.held();
    // Live plays: one dated before the cut, two after (one a row RCNT
    // holds, one new), one undated.
    const uint32_t floor = f.rcntFloor();
    const uint32_t seq0 = static_cast<uint32_t>(slots.size());
    const pl::Slot live[4] = {
        slot(kDevA, kEpochA, seq0 + 1, slots[0].pathHash, 120000, 200000, 1, 1, at(floor - 3, 9)),
        slot(kDevA, kEpochA, seq0 + 2, slots.back().pathHash, 120000, 200000, 1, 1, slots.back().startedAt + 60000),
        slot(kDevA, kEpochA, seq0 + 3, hashOf(9001), 120000, 200000, 1, 3, at(floor + 2, 10)),
        slot(kDevA, kEpochA, seq0 + 4, hashOf(9002), 120000, 200000, 1, 2, 0)};
    for (const pl::Slot& s : live) {
      f.liveSlot(s);
      m.fold(s, 2);
    }
    const uint64_t now = clock ? at(530, 9) : 0;
    const Bytes mine = saveNow(f, now);
    m.save(now);
    TEST_ASSERT_TRUE_MESSAGE(mine == m.file(), hex(mine, m.file()).c_str());
    TEST_ASSERT_EQUAL_UINT32(0, f.rcntFloor());  // (the save landed: the card's file is RAM's)
    if (clock == 1) heldLanded = f.held();
  }
  printf("a full log's boot whose save doesn't land: %u bytes held after the fold (RCNT at its cap); %u after a "
         "save that lands (the clock set)\n",
         static_cast<unsigned>(heldAfter), static_cast<unsigned>(heldLanded));
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_rules_of_4_1);
  RUN_TEST(test_head_and_rows_byte_for_byte);
  RUN_TEST(test_the_merge_is_order_free);
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_each_slot_once);
  RUN_TEST(test_the_gap);
  RUN_TEST(test_rounds_and_the_live_fold);
  RUN_TEST(test_mark_kept_for_good_and_covered);
  RUN_TEST(test_since_day);
  RUN_TEST(test_hour_and_days_keep_the_newest);
  RUN_TEST(test_rcnt_retention);
  RUN_TEST(test_random_folds_against_the_model);
  RUN_TEST(test_the_memory);
  RUN_TEST(test_no_memory);
  RUN_TEST(test_load_checks_the_order);
  RUN_TEST(test_compaction_rule);
  RUN_TEST(test_a_full_log_never_synced);
  RUN_TEST(test_a_boot_whose_save_doesnt_land);
  RUN_TEST(test_release_gives_every_block_back);
  RUN_TEST(test_give_back_keeps_mark_alone);
  return UNITY_END();
}
