// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' fold (lib/core StatsFold;
// docs/LISTENING-STATS.md 4.1, 5.2, 5.6, and S1's rework, 5.14: the
// records, MARK and HEAD in the object, the card's rows merged at each
// save), against tools/statsref.py's fixtures (test/fixtures/card/stats/
// index.json) and a port of its fold:
// - the rules of 4.1 (the line at an odd durationMs and at 0, the
//   outcomes, the sources, dated's ends) and the worked example's hour,
//   day and time; HEAD and a track's row byte for byte;
// - the merge (5.2's ⊕) order-free: every order of the same slots gives
//   the same row, but for lastOrd; each section's rows as the file has
//   them, their order and their ⊕ (compareRows, addRow), saturating;
// - every boot of every fixture: stats.bin's HEAD and MARK loaded into the
//   fold, its other sections kept as a reference table (the card's rows,
//   StatsFile's to stream), the boot's fold into the records (and its
//   overflow's chunks) over the log's files in the scan's order (a partial
//   scan as the fixture says), the counts (folded, twice, gap), the
//   records merged with the table through the cursors (the card's rows ⊕
//   the records', the retention after, a compaction on the merged count)
//   equal to statsref's stats.bin and base byte for byte, and the trim the
//   card's MARK allows, file for file;
// - each slot once (5.6's ranges in the caller's block: a slot met again
//   in its own file, one met in another file's range, counted twice), the
//   gap (an ACKS row of a key that never counts, a partial scan's none),
//   the fold rounds (the boot's +1, live +2), MARK kept for good (the
//   card's beside RAM's), sinceDay's four sources;
// - random sessions (boots with and without an overflow, live folds, the
//   records' save at kSaveAt, saves held then promoted) against the port of
//   statsref's whole aggregate: each save's file byte for byte, merged from
//   the card's last one;
// - the resident set: the records' kSaveAt (due), kRecords - 1 and kRecords
//   (kept), the one past live (behind: not folded, the records dropped), the
//   boot's overflow past kRecords in chunks of 819 (16,384 B), a live
//   slot in an overflow's room, reserve() leaving the folds nothing to
//   carve, the chunks given back (a save landed or held, behind, clear(),
//   the fold's end); MARK's 16 rows and the extra block (8 rows of room, a
//   remount's read past it), Held's records emptied with the card's MARK
//   kept till promoted; no memory at each carve (the fold stops, nothing
//   is saved: never a wrong file);
// - a full log never synced (13,106 slots): 16 chunks, the peak with the
//   ranges' block (at most 273 KB as the device carves them), the file
//   statsref writes, the us a slot and the save's merge, printed;
// - S3a's getters (9.7's V3): sinceDayNow() (set, unset, the first save's
//   day) and liveRound() (a live fold's round).
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

// A play of A's at seq q: track q % 50, dated day 300.
pl::Slot playA(uint32_t q, uint64_t startedAt = 0) {
  return slot(kDevA, kEpochA, q, 1000 + q % 50, 200000, 200000, pl::kCompleted, pl::kSourceManual,
              startedAt ? startedAt : at(300, q % 24, q % 60));
}

// ---------------------------------------------------------------------------
// A heap that counts: every block's size, the live bytes, and a carve that
// fails on demand.
// ---------------------------------------------------------------------------
struct Heap {
  std::map<void*, size_t> blocks;
  std::vector<size_t> carved;  // every carve's size, in order
  size_t live = 0, peak = 0;
  int asked = 0;               // carves asked since reset()
  int failAt = -1;             // the carve that fails (-1: none)
  bool failFrom = false;       // ... and every one after it
  size_t refuse = 0;           // a size every carve of fails (0: none)
  void reset() {
    carved.clear();
    peak = live;
    asked = 0;
    failAt = -1;
    failFrom = false;
    refuse = 0;
  }
};
Heap gHeap;

void* heapAlloc(size_t n) {
  const int k = gHeap.asked++;
  if (gHeap.failAt >= 0 && (k == gHeap.failAt || (gHeap.failFrom && k > gHeap.failAt))) return nullptr;
  if (n == gHeap.refuse) return nullptr;
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

// The caller's ranges' block for one boot's fold (the glue carves it at the
// top; 8-byte aligned): kept until the fold's end.
struct Ranges {
  std::vector<uint64_t> mem;
  size_t bytes = 0;
  bool begin(sf::Fold& f, const cc::msmf::Acks& acks) {
    bytes = f.bootBytes(acks);
    mem.assign((bytes + 7) / 8, 0xA5A5A5A5A5A5A5A5ull);  // (not zeroed: beginBoot's to do)
    return f.beginBoot(acks, mem.data(), bytes);
  }
};

const cc::msmf::Acks kNoAcks;

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
using Rows = std::vector<Bytes>;  // a section's rows, each its stride, in order

const uint8_t* u8(const Bytes& b) { return reinterpret_cast<const uint8_t*>(b.data()); }
uint8_t* u8(Bytes& b) { return reinterpret_cast<uint8_t*>(&b[0]); }

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

uint32_t headerCrc(const Bytes& b) { return cc::get32(u8(b) + 32); }

std::string baseName(uint32_t gen) {
  char n[32];
  std::snprintf(n, sizeof(n), "stats-%08x.bin", static_cast<unsigned>(gen));
  return n;
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
    out[i] = {u8(b) + s.offset, s.bytes, s.count, s.stride};
  }
  *generation = c.header().generation;
  return true;
}

// ---------------------------------------------------------------------------
// The card: stats.bin (HEAD and MARK the fold's after the read; its other
// sections the reference table a save merges the records with, as StatsFile
// streams them) and the base it names.
// ---------------------------------------------------------------------------
struct Card {
  Bytes bin;                    // stats.bin ("": none, or renamed stats.bad)
  std::vector<sf::Track> base;  // the base's rows
  uint32_t named = 0;           // the base stats.bin names, whole or lost (no compaction under it: 5.3)
};

// A section's rows by their first minor's prefix (a newer minor's longer
// rows: 5.12), as statsref reads them.
Rows rowsOf(const Section& s, sf::Sec sec) {
  Rows out;
  for (uint32_t i = 0; i < s.count; ++i)
    out.push_back(Bytes(reinterpret_cast<const char*>(s.p) + static_cast<size_t>(i) * s.stride, sf::kStrides[sec]));
  return out;
}

void table(const Bytes& bin, Rows out[sf::kSecs]) {
  for (int i = 0; i < sf::kSecs; ++i) out[i].clear();
  if (bin.empty()) return;
  cc::Why why;
  Section s[6];
  uint32_t gen = 0;
  TEST_ASSERT_TRUE(sections(bin, kStatsSpec, &why, s, &gen));
  for (int i = sf::kHour; i < sf::kSecs; ++i) out[i] = rowsOf(s[i], static_cast<sf::Sec>(i));
}

// The read (5.4) as a boot does it, small (StatsFile is the firmware's):
// HEAD and MARK into the fold (MARK's block when the card's rows and 8 more
// pass the object's 16), the base whole.
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

// The boot's read's MARK block for each fold (the glue's, carved in setup
// and kept for the session).
std::map<const sf::Fold*, std::vector<uint32_t>> gMarkBlocks;

Read readStats(sf::Fold& f, const Files& in, Card* card) {
  f.clear();
  *card = Card();
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
  if (s[sf::kMark].count + sf::kMarkRoom > f.markCap()) {
    std::vector<uint32_t>& block = gMarkBlocks[&f];
    block.assign(sf::Fold::markBlockBytes(s[sf::kMark].count) / 4, 0);
    TEST_ASSERT_TRUE(f.markBlock(block.data(), block.size() * 4));
  }
  f.loadGeneration(gen);
  f.loadHead(s[0].p);
  if (!f.loadMark(s[sf::kMark].p, s[sf::kMark].count, s[sf::kMark].stride)) {
    f.clear();
    return Read::Corrupt;
  }
  // (The other sections' order: StatsFile checks it as it streams them.)
  for (int i = sf::kHour; i < sf::kSecs; ++i) {
    const Rows r = rowsOf(s[i], static_cast<sf::Sec>(i));
    for (size_t k = 1; k < r.size(); ++k)
      if (sf::compareRows(static_cast<sf::Sec>(i), u8(r[k - 1]), u8(r[k])) >= 0) {
        f.clear();
        return Read::Corrupt;
      }
  }
  card->bin = it->second;
  const sf::Head h = f.head();
  card->named = h.baseGen;
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
        card->base.push_back(sf::decodeTrack(t.p + static_cast<size_t>(i) * t.stride));
        if (i && card->base[i].hash <= card->base[i - 1].hash) return Read::Unread;
      }
    }
  }
  return Read::Loaded;
}

// ---------------------------------------------------------------------------
// The save as StatsFile streams it, small: each section's card rows merged
// with the records' (the cursor: a two-stream merge by compareRows, equal
// keys ⊕'d by addRow), the retention after (statsref's retain()), a
// compaction on the merged TRKD's count, HEAD and MARK from the plan.
// ---------------------------------------------------------------------------
Rows merged(sf::Fold& f, sf::Sec sec, const Rows& card) {
  const uint32_t st = sf::kStrides[sec];
  sf::Fold::Cursor c(f, sec);
  Rows out;
  uint8_t r[sf::kTrackBytes];
  Bytes last;
  bool more = c.next(r);
  if (more) last.assign(reinterpret_cast<const char*>(r), st);
  size_t i = 0;
  while (more || i < card.size()) {
    const int k = !more ? 1 : i == card.size() ? -1 : sf::compareRows(sec, r, u8(card[i]));
    if (k < 0) {
      out.push_back(Bytes(reinterpret_cast<const char*>(r), st));
    } else if (k > 0) {
      out.push_back(card[i++]);
    } else {
      Bytes a = card[i++];
      sf::addRow(sec, u8(a), r);
      out.push_back(a);
    }
    if (k <= 0) {
      more = c.next(r);
      // The cursor's rows strictly increasing: each key once.
      if (more) {
        TEST_ASSERT_TRUE_MESSAGE(sf::compareRows(sec, u8(last), r) < 0, "the cursor gave a key twice or out of order");
        last.assign(reinterpret_cast<const char*>(r), st);
      }
    }
  }
  return out;
}

uint16_t dayOfRow(sf::Sec sec, const Bytes& r) { return sec == sf::kDays || sec == sf::kRcnt ? cc::get16(u8(r)) : 0; }

// statsref's retain(): HOUR's and DAYS' newest 840 and 400; RCNT's 32 days
// back from the earlier of today and DAYS' newest day (nothing by date with
// the clock unset), then whole days, oldest first, past 4,096 rows.
void retain(Rows rows[sf::kSecs], uint64_t nowMs) {
  Rows& hr = rows[sf::kHour];
  Rows& dy = rows[sf::kDays];
  Rows& rc = rows[sf::kRcnt];
  if (hr.size() > sf::kHourRows) hr.erase(hr.begin(), hr.end() - sf::kHourRows);
  if (dy.size() > sf::kDaysRows) dy.erase(dy.begin(), dy.end() - sf::kDaysRows);
  uint16_t today = 0;
  if (sf::todayOf(nowMs, &today) && !dy.empty()) {
    const int64_t ref = std::min<int64_t>(today, dayOfRow(sf::kDays, dy.back()));
    rc.erase(std::remove_if(rc.begin(), rc.end(),
                            [&](const Bytes& r) { return dayOfRow(sf::kRcnt, r) <= ref - static_cast<int64_t>(sf::kRcntDays); }),
             rc.end());
  }
  while (rc.size() > sf::kRcntRows) {
    const uint16_t oldest = dayOfRow(sf::kRcnt, rc.front());
    size_t k = 0;
    while (k < rc.size() && dayOfRow(sf::kRcnt, rc[k]) == oldest) ++k;
    rc.erase(rc.begin(), rc.begin() + k);
  }
}

Bytes join(const Rows& rows) {
  Bytes out;
  for (const Bytes& r : rows) out += r;
  return out;
}

struct SaveAsk {
  uint64_t nowMs = 0;
  bool hasCommit = false;
  uint64_t commitSec = 0;
  bool playing = true;  // (with safePoint false: never a compaction)
  bool safePoint = false;
};

struct SaveOut {
  Bytes bin, base;
  bool compacted = false;
  uint32_t mergedTrkd = 0;
  sf::Fold::Plan plan;
};

// The save's bytes; the fold unchanged (the caller says saved() or held()).
SaveOut saveOf(sf::Fold& f, const Card& card, const SaveAsk& ask) {
  SaveOut o;
  TEST_ASSERT_TRUE(f.plan(ask.nowMs, ask.hasCommit, ask.commitSec, &o.plan));
  sf::Fold::Plan& p = o.plan;
  Rows t[sf::kSecs];
  table(card.bin, t);
  Rows m[sf::kSecs];
  for (int i = sf::kHour; i < sf::kSecs; ++i) m[i] = merged(f, static_cast<sf::Sec>(i), t[i]);
  retain(m, ask.nowMs);
  o.mergedTrkd = static_cast<uint32_t>(m[sf::kTrkd].size());
  // 5.3, on the merged count: base ⊕ TRKD into stats-<gen+1>.bin.
  if (sf::compactionDue(o.mergedTrkd, p.head.baseRows, ask.playing, ask.safePoint) && p.head.baseGen + 1 != card.named) {
    Rows rows;
    size_t i = 0, k = 0;
    uint8_t r[sf::kTrackBytes];
    while (i < card.base.size() || k < m[sf::kTrkd].size()) {
      sf::Track a;
      if (k == m[sf::kTrkd].size() || (i < card.base.size() && card.base[i].hash < cc::get64(u8(m[sf::kTrkd][k])))) {
        a = card.base[i++];
      } else if (i == card.base.size() || cc::get64(u8(m[sf::kTrkd][k])) < card.base[i].hash) {
        a = sf::decodeTrack(u8(m[sf::kTrkd][k++]));
      } else {
        a = sf::merge(card.base[i++], sf::decodeTrack(u8(m[sf::kTrkd][k++])));
      }
      sf::encodeTrack(a, r);
      rows.push_back(Bytes(reinterpret_cast<const char*>(r), sf::kTrackBytes));
    }
    const uint32_t gen = p.head.baseGen + 1;
    o.base = frame(gen, {{sf::kTrks, {24, join(rows)}}});
    p.head.baseGen = gen;
    p.head.baseRows = static_cast<uint32_t>(rows.size());
    p.head.baseCrc = headerCrc(o.base);
    p.compact = true;
    o.compacted = true;
    m[sf::kTrkd].clear();
  }
  Bytes head(sf::kHeadBytes, '\0'), mark(static_cast<size_t>(p.rows[sf::kMark]) * 24, '\0');
  sf::encodeHead(p.head, u8(head));
  uint32_t got = 0;
  while (got < p.rows[sf::kMark]) {
    const uint32_t k = f.markRows(got, u8(mark) + static_cast<size_t>(got) * 24, 5);  // (in pieces)
    TEST_ASSERT_TRUE(k > 0);
    got += k;
  }
  TEST_ASSERT_EQUAL_UINT32(0, f.markRows(got, u8(head), 1));
  o.bin = frame(p.generation, {{sf::kTypes[0], {0, head}},
                               {sf::kTypes[1], {24, mark}},
                               {sf::kTypes[2], {8, join(m[sf::kHour])}},
                               {sf::kTypes[3], {16, join(m[sf::kDays])}},
                               {sf::kTypes[4], {12, join(m[sf::kRcnt])}},
                               {sf::kTypes[5], {24, join(m[sf::kTrkd])}}});
  return o;
}

// A save that lands: the card is the new file (and base), RAM saved().
Bytes saveNow(sf::Fold& f, Card& card, uint64_t nowMs) {
  SaveAsk ask;
  ask.nowMs = nowMs;
  SaveOut o = saveOf(f, card, ask);
  f.saved(o.plan);
  card.bin = o.bin;
  card.named = o.plan.head.baseGen;
  return o.bin;
}

// stats.bin's bytes loaded into a fold, as a boot's read.
void load(sf::Fold& f, const Bytes& b, Card* card) {
  Files in{{"stats.bin", b}};
  TEST_ASSERT_EQUAL_STRING("loaded", readName(readStats(f, in, card)));
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
// whole slots to the cap, reserve() before each piece of 128 slots (the
// glue's); `unread` the card's faults (-1 an open that failed, else the
// slots read before a read failed).
void scanLog(sf::Fold& f, const Files& in, const std::map<std::string, int>& unread) {
  uint32_t piece = 0;
  for (int i = 0; i < sf::kFiles; ++i) {
    auto it = in.find(kLogNames[i]);
    if (it == in.end()) continue;
    uint32_t slots = static_cast<uint32_t>(std::min<size_t>(it->second.size(), pl::kFileBytes) / pl::kSlot);
    auto u = unread.find(kLogNames[i]);
    if (u != unread.end()) slots = u->second < 0 ? 0 : std::min<uint32_t>(slots, static_cast<uint32_t>(u->second));
    for (uint32_t k = 0; k < slots; ++k) {
      if (piece++ % 128 == 0) TEST_ASSERT_TRUE(f.reserve(128));
      pl::Slot s;
      if (pl::decode(u8(it->second) + k * pl::kSlot, pl::kSlot, &s) == pl::Verdict::Ok) f.bootSlot(s, i);
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
    const pl::Verdict v = pl::decode(u8(b) + k, pl::kSlot, &s);
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
// A port of statsref's whole aggregate (Stats.fold, Session.save, write_
// stats), kept plain (std::map): the random sessions' reference.
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

  // A new session (statsref's next Session): its round and first day.
  void session() {
    used = 0;
    anyUsed = false;
    firstPlayDay = -1;
  }

  Bytes file() const {
    Bytes h(sf::kHeadBytes, '\0'), mk, hr, dy, rc, tk;
    sf::encodeHead(head, u8(h));
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

// One boot's fold of `slots` (each in its file, in the scan's order),
// reserve() before each piece of 128 (its slots: a full log's last piece
// holds 50, and 128 would pass the records and 16 chunks, 13,144); and into
// the model at the boot's round.
void bootFold(sf::Fold& f, Ranges& rg, const std::vector<std::pair<pl::Slot, int>>& slots, Model* m,
              const cc::msmf::Acks& acks = kNoAcks) {
  TEST_ASSERT_TRUE(rg.begin(f, acks));
  const uint32_t round = f.head().round + 1;
  uint32_t piece = 0;
  for (const auto& s : slots) {
    if (piece % 128 == 0)
      TEST_ASSERT_TRUE(f.reserve(std::min<uint32_t>(128, static_cast<uint32_t>(slots.size()) - piece)));
    ++piece;
    f.bootSlot(s.first, s.second);
    if (m) m->fold(s.first, round);
  }
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
// The merge (5.2), and each section's rows: their order and ⊕.
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
  // The records' rows the same: every order of the slots above folded live,
  // the cursor's TRKD row is the merge's (round: HEAD.round 4 + 2).
  sf::Fold f;
  uint8_t head[sf::kHeadBytes];
  sf::Head h0;
  h0.round = 4;
  sf::encodeHead(h0, head);
  std::vector<size_t> pick = {6, 0, 5, 3, 1, 4, 2};
  for (int round = 0; round < 3; ++round) {
    f.clear();
    f.loadHead(head);
    for (size_t i : pick) TEST_ASSERT_TRUE(f.liveSlot(s[i]));
    sf::Fold::Cursor c(f, sf::kTrkd);
    uint8_t row[24];
    TEST_ASSERT_TRUE(c.next(row));
    TEST_ASSERT_FALSE(c.next(row));
    const sf::Track got = sf::decodeTrack(row);
    TEST_ASSERT_EQUAL_UINT16(5, got.plays);
    TEST_ASSERT_EQUAL_UINT16(2, got.skips);
    TEST_ASSERT_EQUAL_UINT32(first.firstAt, got.firstAt);
    TEST_ASSERT_EQUAL_UINT8(first.src, got.src);
    TEST_ASSERT_EQUAL_UINT32(first.lastAt, got.lastAt);
    TEST_ASSERT_EQUAL_UINT32(6, got.lastOrd);
    sf::Track viaTrack;
    TEST_ASSERT_TRUE(f.track(h, &viaTrack));
    uint8_t again[24];
    sf::encodeTrack(viaTrack, again);
    TEST_ASSERT_EQUAL_MEMORY(row, again, 24);
    std::reverse(pick.begin(), pick.end());
    if (round == 1) std::rotate(pick.begin(), pick.begin() + 3, pick.end());
  }
}

void test_the_rows_order_and_merge() {
  // HOUR: the hour (bits 0-23) orders; plays (bits 24-31) u8 and ms u32
  // saturate.
  uint8_t a[24] = {}, b[24] = {};
  cc::put32(a, 7485 | 250u << 24);
  cc::put32(a + 4, 0xFFFFFF00u);
  cc::put32(b, 7485 | 10u << 24);
  cc::put32(b + 4, 0x200);
  TEST_ASSERT_EQUAL_INT(0, sf::compareRows(sf::kHour, a, b));  // (plays aren't the key)
  sf::addRow(sf::kHour, a, b);
  TEST_ASSERT_EQUAL_HEX32(7485 | 255u << 24, cc::get32(a));
  TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFu, cc::get32(a + 4));
  cc::put32(b, 7486);
  TEST_ASSERT_EQUAL_INT(-1, sf::compareRows(sf::kHour, a, b));
  TEST_ASSERT_EQUAL_INT(1, sf::compareRows(sf::kHour, b, a));
  // DAYS: the day; plays and skips u16, the sources' u8, ms u32.
  std::memset(a, 0, 24);
  std::memset(b, 0, 24);
  cc::put16(a, 311);
  cc::put16(a + 2, 0xFFF0);
  cc::put16(a + 4, 3);
  a[6] = 200;
  a[7] = 1;
  cc::put32(a + 8, 1000);
  a[12] = 254;
  a[13] = 0;
  std::memcpy(b, a, 16);
  sf::addRow(sf::kDays, a, b);
  TEST_ASSERT_EQUAL_UINT16(311, cc::get16(a));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, cc::get16(a + 2));
  TEST_ASSERT_EQUAL_UINT16(6, cc::get16(a + 4));
  TEST_ASSERT_EQUAL_UINT8(255, a[6]);
  TEST_ASSERT_EQUAL_UINT8(2, a[7]);
  TEST_ASSERT_EQUAL_UINT32(2000, cc::get32(a + 8));
  TEST_ASSERT_EQUAL_UINT8(255, a[12]);
  TEST_ASSERT_EQUAL_UINT8(0, a[14] | a[15]);
  cc::put16(b, 310);
  TEST_ASSERT_EQUAL_INT(1, sf::compareRows(sf::kDays, a, b));
  // RCNT: the day, then the hash (a u64, not its bytes); plays u16.
  std::memset(a, 0, 24);
  std::memset(b, 0, 24);
  cc::put16(a, 300);
  cc::put16(a + 2, 0xFFFE);
  cc::put64(a + 4, 0x0100000000000000ull);
  cc::put16(b, 300);
  cc::put16(b + 2, 5);
  cc::put64(b + 4, 0x00000000000000FFull);
  TEST_ASSERT_EQUAL_INT(1, sf::compareRows(sf::kRcnt, a, b));
  cc::put16(b, 301);
  TEST_ASSERT_EQUAL_INT(-1, sf::compareRows(sf::kRcnt, a, b));
  cc::put64(b + 4, 0x0100000000000000ull);
  cc::put16(b, 300);
  TEST_ASSERT_EQUAL_INT(0, sf::compareRows(sf::kRcnt, a, b));
  sf::addRow(sf::kRcnt, a, b);
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, cc::get16(a + 2));
  // TRKD: the hash; ⊕ is merge().
  sf::Track x, y;
  TEST_ASSERT_TRUE(sf::slotTrack(slot(kDevA, kEpochA, 1, 9, 200000, 200000, 1, pl::kSourceAutoDj, at(300, 9)), 3, &x));
  TEST_ASSERT_TRUE(sf::slotTrack(slot(kDevA, kEpochA, 2, 9, 200000, 200000, 1, pl::kSourceShuffle, at(299, 9)), 5, &y));
  sf::encodeTrack(x, a);
  sf::encodeTrack(y, b);
  TEST_ASSERT_EQUAL_INT(0, sf::compareRows(sf::kTrkd, a, b));
  sf::addRow(sf::kTrkd, a, b);
  uint8_t want[24];
  sf::encodeTrack(sf::merge(x, y), want);
  TEST_ASSERT_EQUAL_MEMORY(want, a, 24);
  y.hash = 10;
  sf::encodeTrack(y, b);
  TEST_ASSERT_EQUAL_INT(-1, sf::compareRows(sf::kTrkd, a, b));
  // MARK: the deviceId's bytes, then the epoch (a u64).
  std::memset(a, 0, 24);
  std::memset(b, 0, 24);
  std::memcpy(a, kDevB, 6);
  std::memcpy(b, kDevA, 6);
  cc::put64(a + 8, 1);
  cc::put64(b + 8, 0x100);
  TEST_ASSERT_EQUAL_INT(1, sf::compareRows(sf::kMark, a, b));
  std::memcpy(b, kDevB, 6);
  TEST_ASSERT_EQUAL_INT(-1, sf::compareRows(sf::kMark, a, b));
}

// ---------------------------------------------------------------------------
// Every boot of every fixture.
// ---------------------------------------------------------------------------
void test_every_fixture_boot() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  int boots = 0, saves = 0, compactions = 0, trims = 0, overflows = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    for (size_t k = 0; k < sc["boots"].size(); ++k) {
      const minijson::Value& b = sc["boots"][k];
      char what[96];
      std::snprintf(what, sizeof(what), "%s, boot %u", sc["name"].c_str(), static_cast<unsigned>(k + 1));
      sf::Fold f(heapAlloc, heapFree);
      const Files in = cardOf(b["in"]);
      const Files out = cardOf(b["out"]);
      const cc::msmf::Acks acks = acksOf(b["acks"]);
      Card card;
      const Read read = readStats(f, in, &card);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(b["read"].c_str(), readName(read), what);
      ++boots;
      if (read == Read::Newer || read == Read::Unread) {
        // Stats off: nothing folded, saved or trimmed.
        TEST_ASSERT_EQUAL_MESSAGE(0, b["folded"].u64(), what);
        TEST_ASSERT_FALSE_MESSAGE(b["saved"].boolean(), what);
        TEST_ASSERT_EQUAL_MESSAGE(0, b["trimmed"].size(), what);
        continue;
      }
      if (read == Read::Corrupt) {
        f.clear();  // (renamed stats.bad: counting starts again)
        card = Card();
      }
      std::map<std::string, int> unread;
      for (const auto& m : b["unread"].members) unread[m.first] = m.second.isNull() ? -1 : static_cast<int>(m.second.u64());
      Ranges rg;
      TEST_ASSERT_TRUE_MESSAGE(rg.begin(f, acks), what);
      TEST_ASSERT_TRUE_MESSAGE(rg.bytes <= 3072, what);
      scanLog(f, in, unread);
      f.endBoot();
      TEST_ASSERT_FALSE_MESSAGE(f.noMemory(), what);
      TEST_ASSERT_FALSE_MESSAGE(f.behind(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["folded"].u64(), f.folded(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["twice"].u64(), f.twice(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["gap"].u64(), f.gap(), what);
      TEST_ASSERT_EQUAL_MESSAGE(b["partial"].boolean(), f.partialScan(), what);
      // One record a slot folded: past 96, the chunks of 819 they fill.
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(f.folded(), f.records(), what);
      const uint32_t over = f.records() > sf::kRecords ? f.records() - sf::kRecords : 0;
      TEST_ASSERT_EQUAL_UINT32_MESSAGE((over + sf::kChunkRecords - 1) / sf::kChunkRecords, f.chunks(), what);
      if (over) ++overflows;
      // The save: none after a partial scan or on a full card; nothing
      // written when nothing is new.
      bool wrote = false, compacted = false;
      if (!b["saveFails"].boolean()) {
        sf::Fold::Plan p;
        const bool ok = f.plan(0, false, 0, &p);
        TEST_ASSERT_EQUAL_MESSAGE(!b["partial"].boolean(), ok, what);
        if (ok && f.dirty()) {
          SaveAsk ask;
          ask.nowMs = b["now"].isNull() ? 0 : b["now"].u64();
          ask.hasCommit = !b["commitTime"].isNull();
          ask.commitSec = b["commitTime"].u64();
          ask.playing = b["playing"].boolean();
          ask.safePoint = b["safePoint"].boolean();
          const SaveOut o = saveOf(f, card, ask);
          if (o.compacted) {
            compacted = true;
            const std::string name = baseName(o.plan.head.baseGen);
            TEST_ASSERT_TRUE_MESSAGE(out.count(name) == 1, what);
            TEST_ASSERT_TRUE_MESSAGE(o.base == out.at(name), (std::string(what) + ": the base, " + hex(o.base, out.at(name))).c_str());
            ++compactions;
          }
          const Bytes& want = out.at("stats.bin");
          TEST_ASSERT_TRUE_MESSAGE(o.bin == want, (std::string(what) + ": stats.bin, " + hex(o.bin, want)).c_str());
          f.saved(o.plan);
          TEST_ASSERT_FALSE(f.dirty());
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, f.records(), what);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, f.chunks(), what);
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
  printf("fixtures: %d boots, %d saves merged byte for byte, %d bases, %d files trimmed, %d boots past the records\n",
         boots, saves, compactions, trims, overflows);
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(22, trims);
  TEST_ASSERT_EQUAL_INT(2, overflows);  // (retention's 902, rcnt-cap's 4,128)
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// ---------------------------------------------------------------------------
// Each slot once, the gap.
// ---------------------------------------------------------------------------
void test_each_slot_once() {
  sf::Fold f;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
  TEST_ASSERT_EQUAL_size_t(sf::kMarkRows * sf::kFiles * sizeof(sf::Fold::Range), rg.bytes);  // 1,280 B: no ACKS
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
  TEST_ASSERT_EQUAL_UINT32(10, f.records());
  TEST_ASSERT_EQUAL_UINT32(4, f.twice());    // 10, 11 in plays.1.jnl; 6, 7 in plays.2.jnl
  TEST_ASSERT_EQUAL_UINT32(4, f.head().twice);
  TEST_ASSERT_EQUAL_UINT32(13, f.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(6, f.markOf(kDevB, kEpochB));
  TEST_ASSERT_EQUAL_UINT64(10, f.head().plays);
  // A boot's fold no longer folds once ended.
  f.bootSlot(s(14), 0);
  TEST_ASSERT_EQUAL_UINT32(10, f.folded());
  // Under the MARK read at the boot: not folded. Slots that don't count:
  // skipped (drop notes, epoch 0, deviceId zeros, seq 0).
  Card card;
  const Bytes b = saveNow(f, card, at(313, 9));
  sf::Fold g;
  load(g, b, &card);
  Ranges rg2;
  TEST_ASSERT_TRUE(rg2.begin(g, kNoAcks));
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
  TEST_ASSERT_EQUAL_UINT32(1, g.records());
  TEST_ASSERT_EQUAL_UINT32(14, g.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(0, g.markOf(kZeros, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(0, g.markOf(kDevA, 0));
  TEST_ASSERT_EQUAL_UINT64(11, g.head().plays);
}

void test_the_gap() {
  // MARK (A) 10 on the card; ACKS (A) 50: the log holds 41-60, and 45 a
  // second time in another file; seqs 11-40 are gone: 30.
  sf::Fold f;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
  for (uint32_t q = 1; q <= 10; ++q) f.bootSlot(slot(kDevA, kEpochA, q, 5, 200000, 200000, 1, 1, at(300, 8)), 0);
  Card base;
  const Bytes card = saveNow(f, base, at(300, 20));
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
  Card c;
  load(g, card, &c);
  Ranges rg2;
  TEST_ASSERT_TRUE(rg2.begin(g, acks));
  TEST_ASSERT_EQUAL_size_t(1280 + 2 * sizeof(sf::Fold::AckTrack), rg2.bytes);  // (two ACKS rows above MARK)
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
  const Bytes next = saveNow(g, c, at(313, 9));
  sf::Fold h;
  Card d;
  load(h, next, &d);
  Ranges rg3;
  TEST_ASSERT_TRUE(rg3.begin(h, acks));
  for (uint32_t q = 41; q <= 60; ++q) h.bootSlot(slot(kDevA, kEpochA, q, 6, 200000, 200000, 1, 1, at(310, 8)), 0);
  h.endBoot();
  TEST_ASSERT_EQUAL_UINT32(0, h.gap());
  TEST_ASSERT_EQUAL_UINT32(32, h.head().gap);
  TEST_ASSERT_FALSE(h.dirty());
  // Only the gap new (MARK raised, no slot): dirty, no record, and the
  // save writes the card's rows again with MARK's and HEAD's.
  sf::Fold only;
  Card e;
  load(only, card, &e);
  Ranges rg4;
  TEST_ASSERT_TRUE(rg4.begin(only, acks));
  only.endBoot();
  TEST_ASSERT_TRUE(only.dirty());
  TEST_ASSERT_EQUAL_UINT32(0, only.records());
  TEST_ASSERT_EQUAL_UINT32(43, only.gap());  // (A's 40, B's 3: nothing held)
  // A partial scan counts no gap (the slots it didn't read may hold them),
  // and plans no save.
  sf::Fold p;
  Card pc;
  load(p, card, &pc);
  Ranges rg5;
  TEST_ASSERT_TRUE(rg5.begin(p, acks));
  for (uint32_t q = 41; q <= 45; ++q) p.bootSlot(slot(kDevA, kEpochA, q, 6, 200000, 200000, 1, 1, at(310, 8)), 0);
  p.partial();
  p.endBoot();
  TEST_ASSERT_EQUAL_UINT32(0, p.gap());
  TEST_ASSERT_EQUAL_UINT32(45, p.markOf(kDevA, kEpochA));
  TEST_ASSERT_TRUE(p.partialScan());
  TEST_ASSERT_FALSE(p.behind());  // (5 records: kept; only an overflow no save may take is dropped)
  sf::Fold::Plan plan;
  TEST_ASSERT_FALSE(p.plan(at(313, 9), false, 0, &plan));
}

// ---------------------------------------------------------------------------
// Rounds, the live fold, MARK, sinceDay.
// ---------------------------------------------------------------------------
void test_rounds_and_the_live_fold() {
  // A card whose HEAD says round 7.
  sf::Fold f;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
  f.bootSlot(slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(300, 8)), 0);
  f.endBoot();
  {
    sf::Fold::Plan p;
    TEST_ASSERT_TRUE(f.plan(at(300, 9), false, 0, &p));
    TEST_ASSERT_EQUAL_UINT32(1, p.head.round);  // (the boot's: HEAD.round 0 + 1)
  }
  uint8_t head[sf::kHeadBytes];
  sf::Head h7 = f.head();
  h7.round = 7;
  sf::encodeHead(h7, head);
  sf::Fold g;
  g.loadHead(head);
  Ranges rg2;
  TEST_ASSERT_TRUE(rg2.begin(g, kNoAcks));
  g.bootSlot(slot(kDevA, kEpochA, 2, 2, 200000, 200000, 1, 1, 0), 1);  // undated: ordered by its round
  g.endBoot();
  sf::Track t;
  TEST_ASSERT_TRUE(g.track(2, &t));
  TEST_ASSERT_EQUAL_UINT32(8, t.lastOrd);  // the boot's: HEAD.round + 1
  TEST_ASSERT_TRUE(g.liveSlot(slot(kDevA, kEpochA, 3, 3, 200000, 200000, 1, 1, 0)));
  TEST_ASSERT_TRUE(g.track(3, &t));
  TEST_ASSERT_EQUAL_UINT32(9, t.lastOrd);  // live: + 2
  // A live slot that doesn't count, or a drop note: nothing.
  TEST_ASSERT_FALSE(g.liveSlot(slot(kDevA, 0, 4, 4, 200000, 200000)));
  pl::Slot note = slot(kDevA, kEpochA, 4, 4, 200000, 200000);
  note.kind = pl::kKindDrop;
  TEST_ASSERT_FALSE(g.liveSlot(note));
  TEST_ASSERT_FALSE(g.track(4, &t));
  TEST_ASSERT_EQUAL_UINT32(2, g.records());
  // The save writes the highest round used; a later live fold this session
  // is still + 2 (of HEAD.round as loaded).
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(g.plan(at(301, 9), false, 0, &p));
  TEST_ASSERT_EQUAL_UINT32(9, p.head.round);
  g.saved(p);
  TEST_ASSERT_FALSE(g.track(3, &t));  // (the records emptied: the card holds them)
  TEST_ASSERT_TRUE(g.liveSlot(slot(kDevA, kEpochA, 5, 3, 200000, 200000, 1, 1, 0)));
  TEST_ASSERT_TRUE(g.track(3, &t));
  TEST_ASSERT_EQUAL_UINT32(9, t.lastOrd);
  TEST_ASSERT_EQUAL_UINT16(1, t.plays);
  TEST_ASSERT_EQUAL_UINT32(3, g.folded());
  // A round at its 24-bit top saturates.
  sf::Head top;
  top.round = sf::kMaxRound - 1;
  sf::encodeHead(top, head);
  sf::Fold z;
  z.loadHead(head);
  TEST_ASSERT_TRUE(z.liveSlot(slot(kDevA, kEpochA, 9, 9, 200000, 200000)));
  TEST_ASSERT_TRUE(z.track(9, &t));
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, t.lastOrd);
  TEST_ASSERT_TRUE(z.plan(0, false, 0, &p));
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, p.head.round);
}

void test_since_day() {
  auto sinceOf = [&](const std::vector<pl::Slot>& slots, uint64_t now, bool hasCommit, uint64_t commit,
                     uint16_t* flags) {
    sf::Fold f;
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
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
  // Set once: a later save keeps it. Unset at a save (no date), the
  // session's earliest dated play still sets it at a later one, though
  // that save emptied the records (the day is the object's, not theirs).
  sf::Fold f;
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
  f.bootSlot(s[0], 0);
  f.endBoot();
  Card card;
  const Bytes b = saveNow(f, card, at(313, 9));
  sf::Fold g;
  Card c;
  load(g, b, &c);
  TEST_ASSERT_TRUE(g.liveSlot(slot(kDevA, kEpochA, 9, 1, 200000, 200000, 1, 1, at(100, 8))));
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(g.plan(at(314, 9), false, 0, &p));
  TEST_ASSERT_EQUAL_UINT16(290, p.head.sinceDay);
  sf::Fold u;
  TEST_ASSERT_TRUE(u.liveSlot(slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(200, 8))));
  TEST_ASSERT_TRUE(u.liveSlot(slot(kDevA, kEpochA, 2, 1, 200000, 200000, 1, 1, 0)));
  TEST_ASSERT_TRUE(u.plan(0, false, 0, &p));
  TEST_ASSERT_EQUAL_UINT16(200, p.head.sinceDay);  // (the clock unset: the play's day)
}

// S3a's getters (9.7's V3): sinceDayNow(), the entry line's day and the
// snapshot's (4.2 as decided: HEAD's sinceDay, else the day the first save
// will set from what the fold holds), and liveRound(), the round a live fold
// gives its record (Recently played's "This session": 7.6).
void test_since_day_now_and_live_round() {
  // Unset: nothing folded, no HEAD's.
  sf::Fold f;
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, f.sinceDayNow());
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, f.sinceDayNow(0));
  TEST_ASSERT_EQUAL_UINT32(2, f.liveRound());  // (HEAD's round 0: a live fold's + 2)
  // The first save's day: the earliest dated play folded (a skip earlier
  // and an undated play don't count), as plan() sets it, then HEAD's.
  Ranges rg;
  TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
  f.bootSlot(slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(290, 8)), 0);
  f.bootSlot(slot(kDevA, kEpochA, 2, 1, 5000, 200000, pl::kSkipped, 1, at(280, 8)), 0);
  f.bootSlot(slot(kDevA, kEpochA, 3, 1, 200000, 200000, 1, 1, 0), 0);
  f.endBoot();
  TEST_ASSERT_EQUAL_UINT16(290, f.sinceDayNow());
  TEST_ASSERT_EQUAL_UINT16(290, f.sinceDayNow(at(313, 9)));
  Card card;
  const Bytes b = saveNow(f, card, at(313, 9));
  TEST_ASSERT_EQUAL_UINT16(290, f.head().sinceDay);
  TEST_ASSERT_EQUAL_UINT16(290, f.sinceDayNow());
  // Set: HEAD's, whatever is folded after (an earlier play, the clock).
  sf::Fold g;
  Card c;
  load(g, b, &c);
  TEST_ASSERT_EQUAL_UINT16(290, g.sinceDayNow());
  TEST_ASSERT_TRUE(g.liveSlot(slot(kDevA, kEpochA, 9, 1, 200000, 200000, 1, 1, at(100, 8))));
  TEST_ASSERT_EQUAL_UINT16(290, g.sinceDayNow());
  TEST_ASSERT_EQUAL_UINT16(290, g.sinceDayNow(at(50, 1)));
  // Unset at the load, each case as the first save will set it: a play
  // dated ahead of today capped at today (with the clock; the play's day
  // without); no dated play: today, else unset (the root's commitTime is
  // plan()'s alone: it isn't the fold's).
  auto firstSave = [](const std::vector<pl::Slot>& slots, uint64_t now, uint16_t* plannedOut) {
    sf::Fold h;
    for (const pl::Slot& one : slots) TEST_ASSERT_TRUE(h.liveSlot(one));
    const uint16_t day = h.sinceDayNow(now);
    sf::Fold::Plan made;
    TEST_ASSERT_TRUE(h.plan(now, false, 0, &made));
    *plannedOut = made.head.sinceDay;
    return day;
  };
  uint16_t planned = 0;
  const std::vector<pl::Slot> ahead = {slot(kDevB, kEpochB, 1, 1, 200000, 200000, 1, 1, at(900, 8))};
  TEST_ASSERT_EQUAL_UINT16(313, firstSave(ahead, at(313, 9), &planned));
  TEST_ASSERT_EQUAL_UINT16(313, planned);
  TEST_ASSERT_EQUAL_UINT16(900, firstSave(ahead, 0, &planned));
  TEST_ASSERT_EQUAL_UINT16(900, planned);
  const std::vector<pl::Slot> undated = {slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, 0)};
  TEST_ASSERT_EQUAL_UINT16(313, firstSave(undated, at(313, 9), &planned));
  TEST_ASSERT_EQUAL_UINT16(313, planned);
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, firstSave(undated, 0, &planned));
  TEST_ASSERT_EQUAL_UINT16(sf::kUnsetDay, planned);
  const std::vector<pl::Slot> two = {slot(kDevA, kEpochA, 1, 1, 200000, 200000, 1, 1, at(305, 8)),
                                     slot(kDevA, kEpochA, 2, 1, 200000, 200000, 1, 1, at(301, 8))};
  TEST_ASSERT_EQUAL_UINT16(301, firstSave(two, at(313, 9), &planned));
  TEST_ASSERT_EQUAL_UINT16(301, planned);
  // liveRound(): HEAD's round as loaded + 2, the lastOrd a live fold
  // gives; a save raising HEAD's round leaves it (the session's rounds are
  // of HEAD as loaded); saturating at the 24-bit top.
  uint8_t head[sf::kHeadBytes];
  sf::Head h7;
  h7.round = 7;
  sf::encodeHead(h7, head);
  sf::Fold r;
  r.loadHead(head);
  TEST_ASSERT_EQUAL_UINT32(9, r.liveRound());
  TEST_ASSERT_TRUE(r.liveSlot(slot(kDevA, kEpochA, 3, 3, 200000, 200000, 1, 1, 0)));
  sf::Track t;
  TEST_ASSERT_TRUE(r.track(3, &t));
  TEST_ASSERT_EQUAL_UINT32(r.liveRound(), t.lastOrd);
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(r.plan(at(301, 9), false, 0, &p));
  TEST_ASSERT_EQUAL_UINT32(9, p.head.round);
  r.saved(p);
  TEST_ASSERT_EQUAL_UINT32(9, r.head().round);
  TEST_ASSERT_EQUAL_UINT32(9, r.liveRound());
  sf::Head top;
  top.round = sf::kMaxRound - 1;
  sf::encodeHead(top, head);
  sf::Fold z;
  z.loadHead(head);
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, z.liveRound());
  top.round = sf::kMaxRound;
  sf::encodeHead(top, head);
  z.loadHead(head);
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxRound, z.liveRound());
  TEST_ASSERT_TRUE(z.liveSlot(slot(kDevA, kEpochA, 9, 9, 200000, 200000)));
  TEST_ASSERT_TRUE(z.track(9, &t));
  TEST_ASSERT_EQUAL_UINT32(z.liveRound(), t.lastOrd);
}

// ---------------------------------------------------------------------------
// MARK: the object's 16 rows and the extra block.
// ---------------------------------------------------------------------------
void test_mark_rows_and_the_extra_block() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  // The object's 16: 16 (deviceId, epoch) pairs fold; a 17th is the fold
  // out of memory (no save), MARK and covered() as they were.
  {
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_EQUAL_UINT32(sf::kMarkRows, f.markCap());
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    for (uint32_t e = 1; e <= 16; ++e) f.bootSlot(slot(kDevA, e, 3, e, 200000, 200000), 0);
    TEST_ASSERT_FALSE(f.noMemory());
    f.bootSlot(slot(kDevA, 17, 3, 17, 200000, 200000), 0);
    TEST_ASSERT_TRUE(f.noMemory());
    TEST_ASSERT_EQUAL_UINT32(16, f.folded());
    f.endBoot();
    sf::Fold::Plan p;
    TEST_ASSERT_FALSE(f.plan(at(300, 9), false, 0, &p));
    TEST_ASSERT_FALSE(f.liveSlot(slot(kDevA, 1, 4, 1, 200000, 200000)));
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carved.size());  // (MARK and the records: the object's)
  }
  // MARK kept for good over sessions. A card with 8 rows fits the
  // object with 8 rows of room; one with 9 needs the block (17 rows).
  Card card;
  Bytes eight, nine, seventeen;
  {
    sf::Fold f;
    for (uint32_t e = 1; e <= 8; ++e) TEST_ASSERT_TRUE(f.liveSlot(slot(kDevA, e, 3, e, 200000, 200000)));
    eight = saveNow(f, card, at(300, 9));
    TEST_ASSERT_TRUE(f.liveSlot(slot(kDevA, 9, 3, 9, 200000, 200000)));
    nine = saveNow(f, card, at(300, 9));
  }
  {
    sf::Fold f;
    Card c;
    load(f, eight, &c);
    TEST_ASSERT_EQUAL_UINT32(sf::kMarkRows, f.markCap());  // (no block: 8 + 8 fit)
    TEST_ASSERT_EQUAL_UINT32(8, f.marks());
    for (uint32_t e = 101; e <= 108; ++e) TEST_ASSERT_TRUE(f.liveSlot(slot(kDevB, e, 1, e, 200000, 200000)));
    TEST_ASSERT_FALSE(f.liveSlot(slot(kDevB, 109, 1, 109, 200000, 200000)));
    TEST_ASSERT_TRUE(f.noMemory());
  }
  {
    sf::Fold f;
    Card c;
    load(f, nine, &c);
    TEST_ASSERT_EQUAL_UINT32(17, f.markCap());  // its 9 rows and 8 more
    TEST_ASSERT_EQUAL_size_t(17 * 28, sf::Fold::markBlockBytes(9));
    TEST_ASSERT_EQUAL_UINT32(9, f.marks());
    for (uint32_t e = 1; e <= 9; ++e) {
      TEST_ASSERT_EQUAL_UINT32(3, f.markOf(kDevA, e));
      TEST_ASSERT_TRUE(f.covered(slot(kDevA, e, 3, 1, 1, 1)));
      TEST_ASSERT_FALSE(f.covered(slot(kDevA, e, 4, 1, 1, 1)));
    }
    // A boot's 8 new pairs; then the 9th: no memory.
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    TEST_ASSERT_EQUAL_size_t(17 * sf::kFiles * sizeof(sf::Fold::Range), rg.bytes);  // (the ranges: a row each)
    for (uint32_t e = 101; e <= 108; ++e) f.bootSlot(slot(kDevB, e, 1, e, 200000, 200000), 1);
    TEST_ASSERT_FALSE(f.noMemory());
    f.endBoot();
    TEST_ASSERT_EQUAL_UINT32(17, f.marks());
    seventeen = saveNow(f, c, at(301, 9));
  }
  // A remount's read past the room (another player wrote the card): no
  // block carved, MARK's rows don't load: no memory (the stats off).
  {
    sf::Fold f;
    std::vector<uint32_t> small(sf::Fold::markBlockBytes(1) / 4);  // a block of 9 rows from an earlier read
    TEST_ASSERT_TRUE(f.markBlock(small.data(), small.size() * 4));
    TEST_ASSERT_EQUAL_UINT32(9, f.markCap());
    cc::Why why;
    Section s[6];
    uint32_t gen = 0;
    TEST_ASSERT_TRUE(sections(seventeen, kStatsSpec, &why, s, &gen));
    TEST_ASSERT_EQUAL_UINT32(17, s[sf::kMark].count);
    f.clear();
    TEST_ASSERT_FALSE(f.loadMark(s[sf::kMark].p, s[sf::kMark].count, s[sf::kMark].stride));
    TEST_ASSERT_TRUE(f.noMemory());
    // The block given and taken back: rows copied in, the object's after.
    f.clear();
    TEST_ASSERT_TRUE(f.loadMark(s[sf::kMark].p, 9, s[sf::kMark].stride));
    std::vector<uint32_t> big(sf::Fold::markBlockBytes(17) / 4);
    TEST_ASSERT_FALSE(f.markBlock(big.data(), 8 * 28));  // (smaller than the 9 rows held)
    TEST_ASSERT_TRUE(f.markBlock(big.data(), big.size() * 4));
    TEST_ASSERT_EQUAL_UINT32(25, f.markCap());
    TEST_ASSERT_EQUAL_UINT32(9, f.marks());
    TEST_ASSERT_EQUAL_UINT32(3, f.markOf(kDevA, 9));
    TEST_ASSERT_TRUE(f.loadMark(s[sf::kMark].p + 9 * 24, 8, s[sf::kMark].stride));
    TEST_ASSERT_EQUAL_UINT32(17, f.marks());
    f.dropMarkBlock();
    TEST_ASSERT_EQUAL_UINT32(sf::kMarkRows, f.markCap());
    TEST_ASSERT_EQUAL_UINT32(0, f.marks());
  }
  // covered(): the card's MARK, not RAM's, until a save lands; a slot
  // whose pair has no row only at seq 0.
  {
    sf::Fold f;
    Card c;
    load(f, eight, &c);
    const pl::Slot s3 = slot(kDevA, 5, 3, 1, 1, 1), s0 = slot(kDevA, 50, 0, 1, 1, 1);
    TEST_ASSERT_TRUE(f.covered(s3));
    TEST_ASSERT_TRUE(f.covered(s0));
    TEST_ASSERT_FALSE(f.covered(slot(kDevB, 5, 1, 1, 1, 1)));
    TEST_ASSERT_TRUE(f.liveSlot(slot(kDevA, 5, 4, 1, 200000, 200000)));
    TEST_ASSERT_EQUAL_UINT32(4, f.markOf(kDevA, 5));
    TEST_ASSERT_FALSE(f.covered(slot(kDevA, 5, 4, 1, 1, 1)));
    saveNow(f, c, at(302, 9));
    TEST_ASSERT_TRUE(f.covered(slot(kDevA, 5, 4, 1, 1, 1)));
    // Hm's look: every row, the card's beside RAM's.
    uint32_t seen = 0;
    for (uint32_t i = 0; const sf::Fold::MarkRow* m = f.markRow(i); ++i) {
      TEST_ASSERT_EQUAL_UINT32(m->seq, m->card);
      ++seen;
    }
    TEST_ASSERT_EQUAL_UINT32(8, seen);
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// ---------------------------------------------------------------------------
// Random sessions against the port of statsref's whole aggregate.
// ---------------------------------------------------------------------------
void test_random_sessions_against_the_model() {
  const uint8_t* devs[3] = {kDevA, kDevB, kDevC};
  const uint64_t epochs[3] = {kEpochA, kEpochB, 0x77};
  gHeap.reset();
  const size_t live0 = gHeap.live;
  int saves = 0, helds = 0, overflows = 0;
  for (uint32_t seed = 1; seed <= 6; ++seed) {
    Rng rng(seed * 7919);
    Model m;
    Card card;
    uint32_t seq[3] = {};
    uint64_t now = at(200, 0);
    auto randomSlot = [&](uint32_t trackFrom, uint32_t tracks) {
      const int d = static_cast<int>(rng.below(3));
      const uint32_t pick = rng.below(10);
      uint64_t start = 0;
      if (pick < 7) start = now - static_cast<uint64_t>(rng.below(60 * 24)) * 3600000ull - rng.below(3600000);
      if (pick == 7) start = at(27000, 1);  // a foreign clock, 2099
      // (pick 8, 9: undated.)
      return slot(devs[d], epochs[d], ++seq[d], hashOf(trackFrom + rng.below(tracks)), rng.below(300000),
                  rng.below(5) ? 1 + rng.below(400000) : 0, static_cast<uint8_t>(rng.below(5)),
                  static_cast<uint8_t>(rng.below(5)), start);
    };
    // Each save's file: merged from the card's last one, statsref's byte for
    // byte. A held one (1 in 4 of the records' saves): the same bytes in a
    // tmp, promoted at the next save.
    Bytes heldBin;
    for (int session = 0; session < 4; ++session) {
      sf::Fold f(heapAlloc, heapFree);
      if (!card.bin.empty()) {
        const Bytes bin = card.bin;
        load(f, bin, &card);
      }
      m.session();
      const uint32_t base = f.head().round;
      // The boot's fold: two files, the newer read first; 1 in 3 past the
      // records (an overflow of up to five chunks).
      const uint32_t n = rng.below(3) == 0 ? 900 + rng.below(3000) : rng.below(sf::kRecords - 1);
      std::vector<std::pair<pl::Slot, int>> boot;
      for (uint32_t i = 0; i < n; ++i) boot.push_back({randomSlot(0, 2600), i < n / 2 ? 1 : 0});
      std::stable_sort(boot.begin(), boot.end(), [](const std::pair<pl::Slot, int>& a, const std::pair<pl::Slot, int>& b) {
        return a.second < b.second;
      });
      Ranges rg;
      bootFold(f, rg, boot, &m);
      f.endBoot();
      TEST_ASSERT_EQUAL_UINT32(n, f.records());
      if (n > sf::kRecords) ++overflows;
      auto landOrHold = [&](bool mayHold) {
        if (f.heldPending()) {
          card.bin = heldBin;  // (the held tmp is stats.bin now)
          f.promoted();
        }
        if (!f.dirty()) return;  // (nothing new: the card holds it all)
        SaveAsk ask;
        ask.nowMs = now;
        const SaveOut o = saveOf(f, card, ask);
        m.save(now);
        const Bytes want = m.file();
        TEST_ASSERT_TRUE_MESSAGE(o.bin == want, hex(o.bin, want).c_str());
        if (mayHold && rng.below(4) == 0) {
          f.held(o.plan);
          heldBin = o.bin;
          ++helds;
        } else {
          f.saved(o.plan);
          card.bin = o.bin;
        }
        TEST_ASSERT_EQUAL_UINT32(0, f.records());
        TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
        ++saves;
      };
      if (f.dirty()) landOrHold(false);
      // Live plays, the records' save each time it is due; the session's
      // end saves what is left (the next session reads the card).
      const uint32_t plays = 40 + rng.below(200);
      for (uint32_t i = 0; i < plays; ++i) {
        const pl::Slot s = randomSlot(2600, 900);
        TEST_ASSERT_TRUE(f.liveSlot(s));
        m.fold(s, base + 2);
        if (f.due()) {
          now += rng.below(3) * static_cast<uint64_t>(sf::kDayMs);
          landOrHold(true);
        }
      }
      now += 9 * static_cast<uint64_t>(sf::kDayMs);
      if (f.dirty() || f.heldPending()) landOrHold(false);
      TEST_ASSERT_FALSE(f.heldPending());
      TEST_ASSERT_FALSE(f.noMemory());
      TEST_ASSERT_FALSE(f.behind());
    }
  }
  printf("random sessions: %d saves merged byte for byte (%d held, then promoted), %d boots past the records\n", saves,
         helds, overflows);
  TEST_ASSERT_TRUE(helds > 0 && overflows > 0);
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);  // (every chunk given back)
}

// ---------------------------------------------------------------------------
// The resident set: the records, the overflow.
// ---------------------------------------------------------------------------
void test_the_records_boundaries() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  constexpr uint32_t S = sf::kSaveAt, R = sf::kRecords;
  static_assert(S == 24 && R == 40, "the records (5.14: 40 in internal RAM, the save from 24)");
  // Live: due from S; R - 1 and R kept; the one past them not folded, behind.
  {
    sf::Fold f(heapAlloc, heapFree);
    for (uint32_t q = 1; q <= S - 1; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    TEST_ASSERT_FALSE(f.due());
    TEST_ASSERT_TRUE(f.liveSlot(playA(S)));
    TEST_ASSERT_TRUE(f.due());
    for (uint32_t q = S + 1; q <= R - 1; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    TEST_ASSERT_EQUAL_UINT32(R - 1, f.records());
    TEST_ASSERT_TRUE(f.liveSlot(playA(R)));
    TEST_ASSERT_EQUAL_UINT32(R, f.records());
    const sf::Head before = f.head();
    TEST_ASSERT_FALSE(f.liveSlot(playA(R + 1)));
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_EQUAL_UINT32(0, f.records());  // (no save may take them: the next boot folds the log again)
    TEST_ASSERT_FALSE(f.due());
    TEST_ASSERT_EQUAL_UINT64(before.plays, f.head().plays);
    TEST_ASSERT_EQUAL_UINT32(R, f.markOf(kDevA, kEpochA));
    TEST_ASSERT_EQUAL_UINT32(R, f.folded());
    TEST_ASSERT_FALSE(f.liveSlot(playA(R + 2)));
    sf::Fold::Plan p;
    TEST_ASSERT_FALSE(f.plan(at(300, 23), false, 0, &p));
    TEST_ASSERT_FALSE(f.covered(playA(1)));  // (the card's MARK: none)
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carved.size());  // (live records never carve)
  }
  // A save empties them: R more fit.
  {
    sf::Fold f(heapAlloc, heapFree);
    Card card;
    for (uint32_t q = 1; q <= S; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    saveNow(f, card, at(300, 23));
    TEST_ASSERT_EQUAL_UINT32(0, f.records());
    TEST_ASSERT_FALSE(f.due());
    for (uint32_t q = S + 1; q <= S + R; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    TEST_ASSERT_FALSE(f.liveSlot(playA(S + R + 1)));
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_TRUE(f.covered(playA(S)));
    TEST_ASSERT_FALSE(f.covered(playA(S + 1)));
  }
  // The boot's fold past R: chunks of 819 records, as it needs them, in one
  // arena (all 16 chunks' room: 5.14's fix A) carved at the first.
  constexpr uint32_t C = sf::kChunkRecords;
  for (uint32_t n : {R, R + 1, R + C, R + C + 1, R + 2 * C, R + 2 * C + 1}) {
    gHeap.reset();
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    for (uint32_t q = 1; q <= n; ++q) {
      if ((q - 1) % 128 == 0) TEST_ASSERT_TRUE(f.reserve(128));
      f.bootSlot(playA(q), 0);
    }
    f.endBoot();
    const uint32_t want = n <= sf::kRecords ? 0 : (n - sf::kRecords + sf::kChunkRecords - 1) / sf::kChunkRecords;
    char what[48];
    std::snprintf(what, sizeof(what), "%u slots", static_cast<unsigned>(n));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(n, f.records(), what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, f.chunks(), what);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(want ? sf::kArenaBytes : 0, f.held(), what);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(live0 + (want ? sf::kArenaBytes : 0), gHeap.live, what);
    // (one carve, the arena, whatever the chunks: reserve()'s room took it at
    // the first piece, and endBoot() gave it back when no record needed it)
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, gHeap.carved.size(), what);
    for (size_t b : gHeap.carved) TEST_ASSERT_EQUAL_size_t_MESSAGE(sf::kArenaBytes, b, what);
    TEST_ASSERT_TRUE_MESSAGE(f.due(), what);
  }
  // Without reserve(): the fold carves the chunk it needs itself.
  {
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    for (uint32_t q = 1; q <= R + 1; ++q) f.bootSlot(playA(q), 0);
    TEST_ASSERT_EQUAL_UINT32(1, f.chunks());
    TEST_ASSERT_FALSE(f.noMemory());
    // An overflow waiting for its save (the boot's tries): a live slot
    // goes in the last chunk's room; past it, behind.
    f.endBoot();
    for (uint32_t q = R + 2; q <= sf::kRecords + sf::kChunkRecords; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    TEST_ASSERT_EQUAL_UINT32(1, f.chunks());
    TEST_ASSERT_FALSE(f.liveSlot(playA(sf::kRecords + sf::kChunkRecords + 1)));
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
    TEST_ASSERT_EQUAL_size_t(0, f.held());
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// 5.14's Held: the records emptied (the tmp holds them), RAM's HEAD the
// plan's, the card's MARK the one before until the tmp is promoted; then
// the next save merges only the records taken since onto the held copy.
void test_held_and_promoted() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  {
    sf::Fold f(heapAlloc, heapFree);
    Model m;
    Card card;
    std::vector<std::pair<pl::Slot, int>> boot;
    for (uint32_t q = 1; q <= 200; ++q) boot.push_back({playA(q), 0});
    Ranges rg;
    bootFold(f, rg, boot, &m);
    f.endBoot();
    TEST_ASSERT_EQUAL_UINT32(1, f.chunks());
    // The boot's save held: the chunk given back, nothing more to save.
    SaveAsk ask;
    ask.nowMs = at(300, 23);
    SaveOut o = saveOf(f, card, ask);
    m.save(ask.nowMs);
    TEST_ASSERT_TRUE(o.bin == m.file());
    f.held(o.plan);
    TEST_ASSERT_TRUE(f.heldPending());
    TEST_ASSERT_FALSE(f.dirty());
    TEST_ASSERT_EQUAL_UINT32(0, f.records());
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
    TEST_ASSERT_EQUAL_UINT32(o.plan.generation, f.generation());
    TEST_ASSERT_EQUAL_UINT32(o.plan.head.sinceDay, f.head().sinceDay);
    TEST_ASSERT_FALSE(f.covered(playA(1)));  // (the card's MARK: none until promoted)
    TEST_ASSERT_EQUAL_UINT32(200, f.markOf(kDevA, kEpochA));
    // Live plays meanwhile: RAM's MARK rises, the held tmp's doesn't.
    for (uint32_t q = 201; q <= 203; ++q) {
      TEST_ASSERT_TRUE(f.liveSlot(playA(q, at(301, 9, q % 60))));
      m.fold(playA(q, at(301, 9, q % 60)), 2);
    }
    TEST_ASSERT_EQUAL_UINT32(3, f.records());
    f.promoted();
    card.bin = o.bin;
    TEST_ASSERT_FALSE(f.heldPending());
    TEST_ASSERT_TRUE(f.covered(playA(200)));
    TEST_ASSERT_FALSE(f.covered(playA(201)));  // (the held tmp's MARK, not RAM's)
    TEST_ASSERT_EQUAL_UINT32(203, f.markOf(kDevA, kEpochA));
    // The next save: the three onto the held copy, none counted twice.
    const Bytes b = saveNow(f, card, at(301, 23));
    m.save(at(301, 23));
    TEST_ASSERT_TRUE_MESSAGE(b == m.file(), hex(b, m.file()).c_str());
    TEST_ASSERT_TRUE(f.covered(playA(203)));
    f.promoted();  // (nothing held: nothing)
    TEST_ASSERT_TRUE(f.covered(playA(203)));
  }
  // The tmp lost before it is promoted: behind, the card's MARK the one
  // before (the log above it keeps those slots for the next boot).
  {
    sf::Fold f(heapAlloc, heapFree);
    Card card;
    for (uint32_t q = 1; q <= 10; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    saveNow(f, card, at(300, 23));
    for (uint32_t q = 11; q <= 15; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    SaveAsk ask;
    ask.nowMs = at(300, 23);
    f.held(saveOf(f, card, ask).plan);
    TEST_ASSERT_TRUE(f.liveSlot(playA(16)));
    f.fallBehind();
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_EQUAL_UINT32(0, f.records());
    TEST_ASSERT_TRUE(f.covered(playA(10)));
    TEST_ASSERT_FALSE(f.covered(playA(11)));
    TEST_ASSERT_FALSE(f.liveSlot(playA(17)));
    sf::Fold::Plan p;
    TEST_ASSERT_FALSE(f.plan(at(300, 23), false, 0, &p));
  }
  // A held compaction: RAM's HEAD names the new base, so the save after the
  // promotion writes a TRKD of the newer records over it (the old delta in
  // the base, not lost).
  {
    sf::Fold f(heapAlloc, heapFree);
    Card card;
    for (uint32_t q = 1; q <= 30; ++q) TEST_ASSERT_TRUE(f.liveSlot(playA(q)));
    SaveAsk ask;
    ask.nowMs = at(300, 23);
    ask.playing = false;  // (any delta over no base: due)
    const SaveOut o = saveOf(f, card, ask);
    TEST_ASSERT_TRUE(o.compacted);
    f.held(o.plan);
    TEST_ASSERT_EQUAL_UINT32(1, f.head().baseGen);
    TEST_ASSERT_EQUAL_UINT32(30, f.head().baseRows);
    TEST_ASSERT_EQUAL_UINT32(headerCrc(o.base), f.head().baseCrc);
    TEST_ASSERT_TRUE(f.liveSlot(playA(31)));   // (a new track, 1031)
    TEST_ASSERT_TRUE(f.liveSlot(playA(101)));  // (one the base has, 1001)
    f.promoted();
    card.bin = o.bin;
    card.named = 1;
    ask.playing = true;
    const SaveOut next = saveOf(f, card, ask);
    TEST_ASSERT_FALSE(next.compacted);
    TEST_ASSERT_EQUAL_UINT32(1, next.plan.head.baseGen);
    TEST_ASSERT_EQUAL_UINT32(2, next.mergedTrkd);  // (TRKD: the two newer rows alone)
    f.saved(next.plan);
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// A boot's overflow kept while its save is tried again (the glue's four
// tries), a live slot in its room meanwhile; given back when one lands, or
// at the fourth failure (behind).
void test_an_overflow_kept_till_a_save_lands() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  std::vector<std::pair<pl::Slot, int>> boot;
  Rng rng(77);
  for (uint32_t q = 1; q <= 2000; ++q)
    boot.push_back({slot(kDevA, kEpochA, q, hashOf(rng.below(700)), 30000 + rng.below(200000), 200000,
                         static_cast<uint8_t>(1 + rng.below(3)), static_cast<uint8_t>(rng.below(4)), at(250 + q / 40, q % 24)),
                    q <= 1000 ? 1 : 0});
  std::stable_sort(boot.begin(), boot.end(),
                   [](const std::pair<pl::Slot, int>& a, const std::pair<pl::Slot, int>& b) { return a.second < b.second; });
  for (int lands = 0; lands < 2; ++lands) {
    sf::Fold f(heapAlloc, heapFree);
    Model m;
    Card card;
    Ranges rg;
    bootFold(f, rg, boot, &m);
    f.endBoot();
    TEST_ASSERT_EQUAL_UINT32(3, f.chunks());  // (1,904 past the records)
    SaveAsk ask;
    ask.nowMs = at(300, 9);
    // Three tries fail (nothing changes), a live play meanwhile.
    for (int t = 0; t < 3; ++t) saveOf(f, card, ask);
    const pl::Slot s = slot(kDevA, kEpochA, 2001, hashOf(5), 200000, 200000, 1, 1, at(300, 8));
    TEST_ASSERT_TRUE(f.liveSlot(s));
    m.fold(s, 2);
    TEST_ASSERT_EQUAL_UINT32(2001, f.records());
    TEST_ASSERT_EQUAL_UINT32(3, f.chunks());
    if (lands) {
      const Bytes b = saveNow(f, card, ask.nowMs);
      m.save(ask.nowMs);
      TEST_ASSERT_TRUE_MESSAGE(b == m.file(), hex(b, m.file()).c_str());
      TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
      TEST_ASSERT_FALSE(f.behind());
      TEST_ASSERT_TRUE(f.covered(s));
    } else {
      f.fallBehind();  // (the fourth failed)
      TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
      TEST_ASSERT_EQUAL_UINT32(0, f.records());
      TEST_ASSERT_FALSE(f.covered(boot[0].first));  // (the boot's MARK: the log keeps every slot)
    }
    TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  }
  // A partial scan's overflow, or a fold out of memory's: no save may take
  // it, so the fold's end drops it (behind); one within the records is kept.
  {
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    bootFold(f, rg, boot, nullptr);
    f.partial();
    f.endBoot();
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
    TEST_ASSERT_EQUAL_UINT32(0, f.records());
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

void test_the_memory() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  printf("the fold's object: %u bytes on this host (the records' %u, MARK's %u)\n",
         static_cast<unsigned>(sizeof(sf::Fold)), static_cast<unsigned>(sf::kRecords * sizeof(sf::Fold::Record)),
         static_cast<unsigned>(sf::kMarkRows * sizeof(sf::Fold::MarkRow)));
  // The resident set (5.14: 2,048 B of internal RAM on the device with the
  // glue's and the store's state): the records (40: 800 B), MARK's 16 rows
  // (448 B), HEAD and the rest.
  TEST_ASSERT_TRUE(sizeof(sf::Fold) <= 800 + 448 + 512);
  {
    sf::Fold f(heapAlloc, heapFree);
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carved.size());  // (nothing carved but the overflow)
    TEST_ASSERT_TRUE(f.reserve(10000));                 // (outside a boot's fold: nothing)
    TEST_ASSERT_EQUAL_UINT32(0, gHeap.carved.size());
    // reserve(): the next folds carve nothing.
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    uint32_t q = 0;
    for (int piece = 0; piece < 24; ++piece) {
      TEST_ASSERT_TRUE(f.reserve(128));
      const size_t before = gHeap.carved.size();
      for (int k = 0; k < 128; ++k) {
        ++q;
        f.bootSlot(slot(kDevA, kEpochA, q, hashOf(q), 200000, 200000, 1, 1, at(100 + q / 50, q % 24)), 0);
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, gHeap.carved.size(), "a fold carved after reserve()");
    }
    TEST_ASSERT_EQUAL_UINT32(3072, f.records());
    TEST_ASSERT_EQUAL_UINT32(4, f.chunks());  // (2,976 past the records: three and a bit)
    TEST_ASSERT_EQUAL_UINT32(1, gHeap.carved.size());  // (one arena for every chunk)
    for (size_t n : gHeap.carved) TEST_ASSERT_EQUAL_size_t(sf::kArenaBytes, n);
    TEST_ASSERT_EQUAL_size_t(sf::kArenaBytes, f.held());
    TEST_ASSERT_EQUAL_size_t(f.held(), f.peak());
    f.endBoot();
    TEST_ASSERT_EQUAL_UINT32(4, f.chunks());  // (every one holds records)
    // clear(): every chunk given back (a scan begun again).
    f.clear();
    TEST_ASSERT_EQUAL_size_t(0, f.held());
    TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
    // At most 16: reserve() past them says no memory, and so does the fold.
    Ranges rg2;
    TEST_ASSERT_TRUE(rg2.begin(f, kNoAcks));
    TEST_ASSERT_FALSE(f.reserve(sf::kRecords + sf::kMaxChunks * sf::kChunkRecords + 1));
    TEST_ASSERT_EQUAL_UINT32(sf::kMaxChunks, f.chunks());
    TEST_ASSERT_TRUE(f.reserve(sf::kRecords + sf::kMaxChunks * sf::kChunkRecords));
    const uint32_t most = sf::kRecords + sf::kMaxChunks * sf::kChunkRecords;
    for (uint32_t k = 1; k <= most; ++k) f.bootSlot(slot(kDevA, kEpochA, k, k, 200000, 200000), 0);
    TEST_ASSERT_FALSE(f.noMemory());
    f.bootSlot(slot(kDevA, kEpochA, most + 1, 1, 200000, 200000), 0);
    TEST_ASSERT_TRUE(f.noMemory());
    TEST_ASSERT_EQUAL_UINT32(most, f.records());
    f.endBoot();  // (no save may take it: dropped)
    TEST_ASSERT_TRUE(f.behind());
    TEST_ASSERT_EQUAL_size_t(0, f.held());
    TEST_ASSERT_FALSE(f.liveSlot(playA(1)));
  }
  // The destructor gives back what is held.
  {
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    TEST_ASSERT_TRUE(f.reserve(1000));
    TEST_ASSERT_EQUAL_UINT32(2, f.chunks());
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  // endBoot() with room no record needs (reserve()'s look-ahead): the chunks
  // in use drop to those records need, and the arena stays whole (it can't
  // give part of itself back) until none is: the save, behind, clear().
  {
    gHeap.reset();
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
    TEST_ASSERT_TRUE(f.reserve(sf::kRecords + 2 * sf::kChunkRecords));
    TEST_ASSERT_EQUAL_UINT32(2, f.chunks());
    for (uint32_t k = 1; k <= sf::kRecords + 1; ++k) f.bootSlot(slot(kDevA, kEpochA, k, k, 200000, 200000), 0);
    f.endBoot();
    TEST_ASSERT_EQUAL_UINT32(1, f.chunks());
    TEST_ASSERT_EQUAL_size_t(sf::kArenaBytes, f.held());
    TEST_ASSERT_EQUAL_size_t(live0 + sf::kArenaBytes, gHeap.live);
    f.fallBehind();
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
    TEST_ASSERT_EQUAL_size_t(0, f.held());
    TEST_ASSERT_EQUAL_UINT32(1, gHeap.carved.size());
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// No memory at each point: the ranges' block, MARK, each chunk. The fold
// stops (noMemory()) and nothing is saved, or it is whole and its file the
// same: never a wrong file.
void test_no_memory() {
  gHeap.reset();
  const size_t live0 = gHeap.live;
  std::vector<std::pair<pl::Slot, int>> slots;
  for (uint32_t q = 1; q <= 3000; ++q)
    slots.push_back({slot(kDevA, kEpochA, q, hashOf(q % 1500), 200000, 200000, 1, 1, at(200 + q / 100, q % 24)), 0});
  SaveAsk ask;
  ask.nowMs = at(240, 9);
  // The control: everything carved.
  Bytes want;
  {
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    bootFold(f, rg, slots, nullptr);
    f.endBoot();
    Card card;
    want = saveNow(f, card, ask.nowMs);
  }
  // No ranges' block, or one too small: no boot's fold, no save.
  {
    sf::Fold f;
    TEST_ASSERT_FALSE(f.beginBoot(kNoAcks, nullptr, 0));
    TEST_ASSERT_TRUE(f.noMemory());
    f.bootSlot(slots[0].first, 0);
    TEST_ASSERT_EQUAL_UINT32(0, f.folded());
    sf::Fold::Plan p;
    TEST_ASSERT_FALSE(f.plan(ask.nowMs, false, 0, &p));
    sf::Fold g;
    std::vector<uint64_t> mem(100);
    TEST_ASSERT_FALSE(g.beginBoot(kNoAcks, mem.data(), g.bootBytes(kNoAcks) - 1));
    TEST_ASSERT_TRUE(g.noMemory());
  }
  // The arena's carve failing (reserve()'s: the fold then carves its own at
  // the record past the 40), every carve failing (the fold stops there), and
  // carves after the arena's failing (none is asked: the arena holds every
  // chunk, so nothing fails; the chunks failing one by one are the next
  // case's, with no room for the arena).
  int stopped = 0, whole = 0;
  for (int from = 0; from < 2; ++from) {
    for (int n = 0; n < 5; ++n) {
      gHeap.reset();
      gHeap.failAt = n;
      gHeap.failFrom = from != 0;
      sf::Fold f(heapAlloc, heapFree);
      Ranges rg;
      uint32_t piece = 0;
      TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
      for (const auto& s : slots) {
        if (piece++ % 128 == 0) f.reserve(128);
        f.bootSlot(s.first, s.second);
      }
      gHeap.failAt = -1;
      // (n 0: the arena's carve fails, and more are asked; past it, the
      // arena's the one carve: nothing failed)
      if (n == 0) TEST_ASSERT_TRUE(gHeap.asked > 1);
      else TEST_ASSERT_EQUAL_INT(1, gHeap.asked);
      f.endBoot();
      sf::Fold::Plan p;
      if (f.noMemory()) {
        ++stopped;
        TEST_ASSERT_FALSE(f.plan(ask.nowMs, false, 0, &p));
        // An overflow dropped (no save may take it: behind); within the
        // records, kept and never saved. No chunk held either way.
        TEST_ASSERT_TRUE(f.behind() || f.records() == sf::kRecords);
        TEST_ASSERT_EQUAL_size_t(0, f.held());
        TEST_ASSERT_FALSE(f.covered(slots[0].first));
        continue;
      }
      ++whole;
      Card card;
      TEST_ASSERT_TRUE(saveNow(f, card, ask.nowMs) == want);
    }
  }
  TEST_ASSERT_TRUE(stopped > 0 && whole > 0);
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  // No room for the arena (no free block of 262,080 B: a heap a library
  // update cut up): each chunk a carve of its own (16,384 B), as before the
  // arena, and the file the same.
  {
    gHeap.reset();
    gHeap.refuse = sf::kArenaBytes;
    sf::Fold f(heapAlloc, heapFree);
    Ranges rg;
    bootFold(f, rg, slots, nullptr);
    f.endBoot();
    TEST_ASSERT_FALSE(f.noMemory());
    TEST_ASSERT_FALSE(f.inArena());
    TEST_ASSERT_EQUAL_UINT32(4, f.chunks());
    TEST_ASSERT_EQUAL_size_t(4 * sf::kChunkBytes, f.held());
    TEST_ASSERT_EQUAL_size_t(4 * sf::kChunkBytes, f.peak());
    TEST_ASSERT_EQUAL_UINT32(4, gHeap.carved.size());
    for (size_t b : gHeap.carved) TEST_ASSERT_EQUAL_size_t(sf::kChunkBytes, b);
    Card card;
    TEST_ASSERT_TRUE(saveNow(f, card, ask.nowMs) == want);
    TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
    TEST_ASSERT_EQUAL_size_t(0, f.held());
    gHeap.refuse = 0;
  }
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  // No room for the arena, and a chunk's own carve failing in turn (each
  // carve after the refused arena: chunk 0 to 3, and past the last), alone
  // or with every one after it: some chunks held, then no memory, the
  // overflow dropped (behind) and each chunk given back on its own (no
  // arena); or whole, and its file the same.
  int mid = 0, recovered = 0;
  stopped = whole = 0;
  for (int from = 0; from < 2; ++from) {
    for (int n = 1; n <= 7; ++n) {
      gHeap.reset();
      gHeap.refuse = sf::kArenaBytes;
      gHeap.failAt = n;
      gHeap.failFrom = from != 0;
      sf::Fold f(heapAlloc, heapFree);
      Ranges rg;
      uint32_t piece = 0, most = 0;
      TEST_ASSERT_TRUE(rg.begin(f, kNoAcks));
      for (const auto& s : slots) {
        if (piece++ % 128 == 0) f.reserve(128);
        f.bootSlot(s.first, s.second);
        most = std::max(most, f.chunks());
      }
      const bool failed = gHeap.asked > n;  // (the n-th carve asked: it failed)
      gHeap.failAt = -1;
      gHeap.refuse = 0;
      TEST_ASSERT_FALSE(f.inArena());
      f.endBoot();
      sf::Fold::Plan p;
      if (f.noMemory()) {
        TEST_ASSERT_TRUE(failed);
        ++stopped;
        if (most > 0) ++mid;
        TEST_ASSERT_FALSE(f.plan(ask.nowMs, false, 0, &p));
        TEST_ASSERT_TRUE(f.behind() || f.records() == sf::kRecords);
        TEST_ASSERT_EQUAL_size_t(0, f.held());
        TEST_ASSERT_EQUAL_UINT32(0, f.chunks());
        TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);  // (each chunk given back)
        continue;
      }
      // (Whole with a carve failed: reserve()'s look-ahead, which stops
      // nothing; the slot that needs the chunk carves it again.)
      if (failed) ++recovered;
      ++whole;
      Card card;
      TEST_ASSERT_TRUE(saveNow(f, card, ask.nowMs) == want);
    }
  }
  printf("[fold] no room for the arena, a chunk's carve failing: stopped %d (mid-overflow %d), whole %d (%d after a "
         "look-ahead's carve failed)\n",
         stopped, mid, whole, recovered);
  TEST_ASSERT_TRUE(mid > 0 && whole > 0);
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
}

// ---------------------------------------------------------------------------
// A full log never synced.
// ---------------------------------------------------------------------------
namespace {

// Both files at the cap: 13,106 slots, 25 plays a day for 524 days, of
// 5,000 tracks; the scan reads plays.jnl (the newer half) first.
std::vector<std::pair<pl::Slot, int>> fullLog() {
  const uint32_t total = 2 * pl::kFileSlots;
  std::vector<pl::Slot> slots;
  Rng rng(2026);
  for (uint32_t q = 1; q <= total; ++q) {
    const uint64_t h = hashOf(rng.below(5000));
    slots.push_back(slot(kDevA, kEpochA, q, h, 30000 + rng.below(200000), 200000, 1 + rng.below(3),
                         static_cast<uint8_t>(rng.below(4)), at(q / 25, 8 + (q % 25) / 2, q % 60)));
  }
  std::vector<std::pair<pl::Slot, int>> out;
  for (uint32_t i = pl::kFileSlots; i < total; ++i) out.push_back({slots[i], 0});
  for (uint32_t i = 0; i < pl::kFileSlots; ++i) out.push_back({slots[i], 1});
  return out;
}

}  // namespace

void test_a_full_log_never_synced() {
  const std::vector<std::pair<pl::Slot, int>> log = fullLog();
  gHeap.reset();
  const size_t live0 = gHeap.live;
  sf::Fold f(heapAlloc, heapFree);
  Model m;
  // The root's 16 ACKS rows (the ranges' block at its 3,072 B): A's own,
  // every seq in the log, and 15 others of 5 seqs the log doesn't hold
  // (MARK at its 16 rows).
  cc::msmf::Acks acks;
  acks.present = true;
  std::memcpy(acks.entries[0].deviceId, kDevA, 6);
  acks.entries[0].epoch = kEpochA;
  acks.entries[0].seq = 2 * pl::kFileSlots;
  for (uint32_t i = 1; i < cc::msmf::Acks::kMax; ++i) {
    std::memcpy(acks.entries[i].deviceId, kDevB, 6);
    acks.entries[i].epoch = 100 + i;
    acks.entries[i].seq = 5;
  }
  acks.count = acks.rows = cc::msmf::Acks::kMax;
  Ranges rg;
  const auto t0 = std::chrono::steady_clock::now();
  bootFold(f, rg, log, nullptr, acks);
  f.endBoot();
  const auto t1 = std::chrono::steady_clock::now();
  for (const auto& s : log) m.fold(s.first, 1);
  TEST_ASSERT_EQUAL_size_t(3072, rg.bytes);
  TEST_ASSERT_EQUAL_UINT32(log.size(), f.folded());
  TEST_ASSERT_EQUAL_UINT32(log.size(), f.records());
  TEST_ASSERT_EQUAL_UINT32(sf::kMaxChunks, f.chunks());  // 40 + 16 x 819 = 13,144
  TEST_ASSERT_EQUAL_size_t(sf::kArenaBytes, f.peak());
  // As the device carves them (5.14, fix A: the arena 262,144 B and its 4 B
  // header, its 16 chunks packed) with the ranges' block: at most 273 KB.
  const size_t device = size_t{262148} + rg.bytes;
  TEST_ASSERT_TRUE(device <= 273472);
  // The save: the records (17 runs) merged into an empty card.
  Card card;
  SaveAsk ask;
  ask.nowMs = at(530, 9);
  const auto t2 = std::chrono::steady_clock::now();
  const SaveOut o = saveOf(f, card, ask);
  const auto t3 = std::chrono::steady_clock::now();
  f.saved(o.plan);
  m.save(ask.nowMs);
  // (the gap: 15 rows of 5 seqs, none held)
  TEST_ASSERT_EQUAL_UINT32(75, f.gap());
  TEST_ASSERT_EQUAL_UINT32(sf::kMarkRows, f.marks());
  m.head.gap = 75;
  for (uint32_t i = 1; i < cc::msmf::Acks::kMax; ++i) {
    std::array<uint8_t, 6> dev;
    std::memcpy(dev.data(), kDevB, 6);
    m.mark[{dev, 100 + i}] = 5;
  }
  TEST_ASSERT_TRUE_MESSAGE(o.bin == m.file(), hex(o.bin, m.file()).c_str());
  TEST_ASSERT_EQUAL_size_t(live0, gHeap.live);
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  const double ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
  printf("full log: %u slots, %u chunks (the arena's %u bytes held, %u as the device carves it with the ranges' %u); "
         "%.2f us a slot on this host; the save's merge %.1f ms (the test's own reader and writer with it); "
         "stats.bin %u bytes\n",
         static_cast<unsigned>(log.size()), static_cast<unsigned>(sf::kMaxChunks), static_cast<unsigned>(f.peak()),
         static_cast<unsigned>(device), static_cast<unsigned>(rg.bytes), us / log.size(), ms,
         static_cast<unsigned>(o.bin.size()));
}

// ---------------------------------------------------------------------------
// The load, compaction's rule.
// ---------------------------------------------------------------------------
void test_load_checks_the_order() {
  // MARK's rows not strictly increasing: corrupt (5.4). (The other
  // sections' StatsFile checks as it streams them, by compareRows.)
  uint8_t rows[2 * 24] = {};
  std::memcpy(rows, kDevA, 6);
  cc::put64(rows + 8, kEpochA);
  cc::put32(rows + 16, 9);
  std::memcpy(rows + 24, kDevB, 6);
  cc::put64(rows + 32, 1);
  cc::put32(rows + 40, 3);
  sf::Fold f;
  TEST_ASSERT_TRUE(f.loadMark(rows, 2, 24));
  TEST_ASSERT_EQUAL_UINT32(9, f.markOf(kDevA, kEpochA));
  TEST_ASSERT_EQUAL_UINT32(3, f.markOf(kDevB, 1));
  TEST_ASSERT_TRUE(f.covered(slot(kDevB, 1, 3, 1, 1, 1)));
  f.clear();
  uint8_t swapped[2 * 24];
  std::memcpy(swapped, rows + 24, 24);
  std::memcpy(swapped + 24, rows, 24);
  TEST_ASSERT_FALSE(f.loadMark(swapped, 2, 24));
  TEST_ASSERT_FALSE(f.noMemory());  // (corrupt, not no memory)
  f.clear();
  uint8_t twice[2 * 24];
  std::memcpy(twice, rows, 24);
  std::memcpy(twice + 24, rows, 24);
  TEST_ASSERT_FALSE(f.loadMark(twice, 2, 24));
  // Batches go on from the last row; a newer minor's longer rows by their
  // prefix; a stride under 24: refused.
  f.clear();
  TEST_ASSERT_TRUE(f.loadMark(rows, 1, 24));
  TEST_ASSERT_TRUE(f.loadMark(rows + 24, 1, 24));
  TEST_ASSERT_FALSE(f.loadMark(rows, 1, 24));
  f.clear();
  uint8_t longer[2 * 32];
  std::memset(longer, 0xEE, sizeof(longer));
  std::memcpy(longer, rows, 24);
  std::memcpy(longer + 32, rows + 24, 24);
  TEST_ASSERT_TRUE(f.loadMark(longer, 2, 32));
  TEST_ASSERT_EQUAL_UINT32(2, f.marks());
  TEST_ASSERT_EQUAL_UINT32(3, f.markOf(kDevB, 1));
  f.clear();
  TEST_ASSERT_FALSE(f.loadMark(rows, 1, 20));
  // The rows MARK writes: the file's, sorted, the reserved bytes 0.
  f.clear();
  TEST_ASSERT_TRUE(f.liveSlot(slot(kDevB, 1, 4, 1, 200000, 200000)));
  TEST_ASSERT_TRUE(f.liveSlot(slot(kDevA, kEpochA, 10, 1, 200000, 200000)));
  sf::Fold::Plan p;
  TEST_ASSERT_TRUE(f.plan(0, false, 0, &p));
  TEST_ASSERT_EQUAL_UINT32(2, p.rows[sf::kMark]);
  uint8_t out[2 * 24];
  std::memset(out, 0xEE, sizeof(out));
  TEST_ASSERT_EQUAL_UINT32(2, f.markRows(0, out, 2));
  cc::put32(rows + 16, 10);
  cc::put32(rows + 40, 4);
  TEST_ASSERT_EQUAL_MEMORY(rows, out, sizeof(out));
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

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_rules_of_4_1);
  RUN_TEST(test_head_and_rows_byte_for_byte);
  RUN_TEST(test_the_merge_is_order_free);
  RUN_TEST(test_the_rows_order_and_merge);
  RUN_TEST(test_every_fixture_boot);
  RUN_TEST(test_each_slot_once);
  RUN_TEST(test_the_gap);
  RUN_TEST(test_rounds_and_the_live_fold);
  RUN_TEST(test_since_day);
  RUN_TEST(test_since_day_now_and_live_round);
  RUN_TEST(test_mark_rows_and_the_extra_block);
  RUN_TEST(test_random_sessions_against_the_model);
  RUN_TEST(test_the_records_boundaries);
  RUN_TEST(test_held_and_promoted);
  RUN_TEST(test_an_overflow_kept_till_a_save_lands);
  RUN_TEST(test_the_memory);
  RUN_TEST(test_no_memory);
  RUN_TEST(test_a_full_log_never_synced);
  RUN_TEST(test_load_checks_the_order);
  RUN_TEST(test_compaction_rule);
  return UNITY_END();
}
