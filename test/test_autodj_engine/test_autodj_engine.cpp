// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the Auto DJ engine (lib/core/AutoDjEngine; the
// engine design's 1.4 and 2.5-2.9 with the user's decisions D1-D3 and D5,
// and "Loosen before repeating", 2026-10-14).
// First every session of test/fixtures/autodj/sessions.json, made by
// tools/autodj_ref.py (the rule in Python), replayed pick for pick: the
// same row, track, level, class, imputed statuses, breaks, reads and
// counts, every prefetch read and the counters at the end, on the golden
// MPDJ files (read through mpdj::Table) and synthetic tables, with slots
// damaged, failing or broken by a fake reader, the index away and back, and
// a mood filter set, changed and cleared; each session twice, the slots' runs made at the read ahead and at the
// pick. Then the rules one by one on small tables: the defaults (D2: both
// off); the gate at every step and its clamp at selFloor; dry (the 50 best
// sums, ties by row); the ring (5, move-to-end, rows only, D1's joins, the
// quick skip and whether it steered, a pick by track id joining once the
// index is there); the pin and Anchor changes; a seed without a row, and
// none (the pin at the first pick); the lazy key anchor; the cooldowns (50
// rows, 3 artists, artist 0 never cooled, all cooled: repeats); loosen
// before repeating (the device's case: 3 rows above 75 %; Follow the vibe;
// dry and loosened at once); cooldown before music; no BPM or key
// anywhere; empty lists; the read budget and the prefetch; the runs
// following the maps (mapsChanged(), runDue(), moved arrays, arrays filled
// again unannounced); a mood filter (MOOD-FILTERS.md 6.1: the candidates
// inside it, a level alone or no words as none, the runs remade for a
// changed filter or words filled again, a shared row tested by its first
// track; its own ladder, 6.2: a jump keeping the classes, the scan's 64,
// repeats oldest first, a track with no row before them, the only track
// that fits, nothing that fits; the button inside it, 6.3; Stay on seed's
// re-pin, 6.4: in the read budget, no draw before it, a jump when the new
// pin finds nothing, a moved pin gone at a quick skip, a seed's kept); the
// cache keeping the ring and the pin; damaged (a
// damaged pin giving way; the rows a pick's reads damaged, named once),
// failed and broken slots, in hand-made readers and in the hardening
// files; picks by track id; the empty states' button;
// the same seed giving the same session; no allocation in pick() or in a
// jump; the console's dry-run copy.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "AutoDjEngine.h"
#include "AutoDjJoin.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "MoodFilter.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using namespace autodj;
using minijson::Value;

namespace {

// The engine's hooks, counted (no allocation in pick()).
long gAllocs = 0;
long gFrees = 0;
bool gFail = false;
void* countAlloc(size_t n) {
  ++gAllocs;
  return gFail ? nullptr : std::malloc(n);
}
void countFree(void* p) {
  ++gFrees;
  std::free(p);
}

using Entries = std::vector<std::pair<uint16_t, uint8_t>>;  // (row, level), best first

// The tests' SlotReader: a real mpdj::Table (a golden or a hardening file),
// or lists encoded as slots; faults by row as tools/autodj_ref.py's
// SlotSource: damaged (the CRC always fails), flaky (on the first read),
// io (the first read fails, Why::Io), ioAlways (every read does, a sector the card
// keeps failing), broken (a good CRC over a bad list).
class FakeReader : public SlotReader {
public:
  const mpdj::Table* table = nullptr;
  const std::vector<Entries>* lists = nullptr;
  uint32_t k = 0;
  std::set<uint16_t> damaged, flaky, io, ioAlways, broken;
  std::map<uint16_t, int> count;
  int reads = 0;

  mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* n, cc::Why* why) override {
    ++reads;
    const int seen = count[row]++;
    *n = 0;
    if (broken.count(row)) {
      *why = cc::Why::DjSlotOrder;
      return mpdj::SlotRead::Broken;
    }
    if (ioAlways.count(row) || (io.count(row) && seen == 0)) {
      *why = cc::Why::Io;
      return mpdj::SlotRead::Damaged;
    }
    if (damaged.count(row) || (flaky.count(row) && seen == 0)) {
      *why = cc::Why::DjSlotCrc;
      return mpdj::SlotRead::Damaged;
    }
    if (table) return table->slot(row, bytes, nullptr, n, why);
    const Entries& e = (*lists)[row];
    std::memset(bytes, 0, mpdj::slotBytesFor(k));
    for (uint32_t j = 0; j < k; ++j) cc::put16(bytes + 2 * j, j < e.size() ? e[j].first : mpdj::kUnused);
    for (uint32_t j = 0; j < e.size(); ++j) bytes[2 * k + j] = e[j].second;
    *n = static_cast<uint32_t>(e.size());
    *why = cc::Why::Ok;
    return mpdj::SlotRead::Ok;
  }
};

// The join's product, held in vectors.
struct MapsHolder {
  std::vector<uint16_t> t2r, r2t;
  std::vector<uint32_t> f;
  Maps m;
  void set() {
    m.tracks = static_cast<uint32_t>(t2r.size());
    m.rows = static_cast<uint32_t>(r2t.size());
    m.trackToRow = t2r.data();
    m.rowToTrack = r2t.data();
    m.filters = f.data();
  }
};

// Rows 0..n-1 with `lists`; track t is row t, or `tracks` maps tracks to
// rows (kNoRow: a track with no row); the first track of a row is its own.
struct Table {
  std::vector<Entries> lists;
  MapsHolder maps;
  FakeReader reader;
  uint32_t k = 169;
  uint8_t selFloor = 140;

  Table(std::vector<Entries> l, std::vector<uint32_t> filters = {}, std::vector<uint16_t> tracks = {})
      : lists(std::move(l)) {
    const size_t n = lists.size();
    maps.f = filters.empty() ? std::vector<uint32_t>(n, 0) : std::move(filters);
    if (tracks.empty())
      for (size_t i = 0; i < n; ++i) tracks.push_back(static_cast<uint16_t>(i));
    maps.t2r = tracks;
    maps.r2t.assign(n, kNoTrack);
    for (size_t t = 0; t < tracks.size(); ++t)
      if (tracks[t] != kNoRow && maps.r2t[tracks[t]] == kNoTrack) maps.r2t[tracks[t]] = static_cast<uint16_t>(t);
    maps.set();
    for (const Entries& e : lists) k = std::max<uint32_t>(k, static_cast<uint32_t>(e.size()));
    reader.lists = &lists;
    reader.k = k;
  }
  PickIn in(uint32_t playing) {
    PickIn p;
    p.maps = &maps.m;
    p.slots = &reader;
    p.k = static_cast<uint16_t>(k);
    p.selHead = 84;
    p.selFloor = selFloor;
    p.playingTrack = playing;
    p.libraryTracks = maps.m.tracks;
    p.mood = maps.m.mood;
    return p;
  }
};

Entries sorted(Entries e) {
  std::sort(e.begin(), e.end(), [](const std::pair<uint16_t, uint8_t>& a, const std::pair<uint16_t, uint8_t>& b) {
    return a.second != b.second ? a.second > b.second : a.first < b.first;
  });
  return e;
}

// Row `centre` lists every other row at `level` - row (at least 1); the
// others list the centre only.
std::vector<Entries> star(int n, int level = 200) {
  std::vector<Entries> out(n);
  for (int b = 1; b < n; ++b)
    out[0].push_back({static_cast<uint16_t>(b), static_cast<uint8_t>(std::max(level - b, 1))});
  for (int r = 1; r < n; ++r) out[r].push_back({0, static_cast<uint8_t>(level)});
  return out;
}

// Every row lists every other, nearer rows higher.
std::vector<Entries> full(int n) {
  std::vector<Entries> out(n);
  for (int a = 0; a < n; ++a) {
    for (int b = 0; b < n; ++b)
      if (b != a) out[a].push_back({static_cast<uint16_t>(b), static_cast<uint8_t>(250 - std::abs(a - b))});
    out[a] = sorted(out[a]);
  }
  return out;
}

Settings settings(Mode m = Mode::Rolling, uint8_t floor = 55, bool tempo = false, bool key = false) {
  Settings s;
  s.mode = m;
  s.floorPct = floor;
  s.matchTempo = tempo;
  s.matchKey = key;
  return s;
}

std::unique_ptr<Engine> engine() {
  std::unique_ptr<Engine> e(new Engine());
  TEST_ASSERT_TRUE(e->begin(countAlloc, countFree));
  return e;
}

// ---------------------------------------------------------------------------
// The fixture
// ---------------------------------------------------------------------------
bool fixture(Value* out) {
  std::string text;
  return minijson::readFile(cardfixtures::dir() + "/../autodj/sessions.json", &text) && minijson::parse(text, out);
}

std::vector<Entries> hexLists(const Value& slots) {
  std::vector<Entries> out;
  for (const Value& s : slots.items) {
    Entries e;
    const std::string& h = s.str();
    for (size_t i = 0; i + 6 <= h.size(); i += 6)
      e.push_back({static_cast<uint16_t>(std::stoul(h.substr(i, 4), nullptr, 16)),
                   static_cast<uint8_t>(std::stoul(h.substr(i + 4, 2), nullptr, 16))});
    out.push_back(e);
  }
  return out;
}

void setRows(std::set<uint16_t>* out, const Value& rows) {
  out->clear();
  for (const Value& r : rows.items) out->insert(static_cast<uint16_t>(r.u64()));
}

void setFaults(FakeReader* r, const Value& f) {
  setRows(&r->damaged, f["damaged"]);
  setRows(&r->flaky, f["flaky"]);
  setRows(&r->io, f["io"]);
  setRows(&r->broken, f["broken"]);
}

const char* resultName(SlotResult r) {
  switch (r) {
    case SlotResult::Ok: return "ok";
    case SlotResult::Suspect: return "suspect";
    case SlotResult::Damaged: return "damaged";
    case SlotResult::Io: return "io";
    case SlotResult::Broken: return "broken";
  }
  return "?";
}

struct Where {
  std::string session;
  size_t event = 0;
  std::string msg(const char* field) const {
    return session + " event " + std::to_string(event) + ": " + field;
  }
};

void expectField(const Where& w, const Value& expect, const char* key, uint64_t got) {
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(expect[key].u64(0), got, w.msg(key).c_str());
}

void expectPick(const Where& w, const Value& x, const Pick& p) {
  expectField(w, x, "ok", p.ok);
  expectField(w, x, "track", p.track);
  expectField(w, x, "row", p.row);
  expectField(w, x, "level", p.level);
  expectField(w, x, "cl", p.cooldownLevel);
  expectField(w, x, "mr", p.musicRank);
  expectField(w, x, "dry", p.dry);
  expectField(w, x, "loose", p.loosened);
  expectField(w, x, "random", p.random);
  expectField(w, x, "noIndex", p.noIndex);
  expectField(w, x, "impT", p.imputedTempo);
  expectField(w, x, "impK", p.imputedKey);
  expectField(w, x, "tb", p.tempoBreak);
  expectField(w, x, "kb", p.keyBreak);
  expectField(w, x, "reads", p.reads);
  expectField(w, x, "broken", p.broken);
  expectField(w, x, "cand", p.candidates);
  expectField(w, x, "gated", p.gated);
  expectField(w, x, "inClass", p.inClass);
  expectField(w, x, "jump", static_cast<uint64_t>(p.jump));
  expectField(w, x, "repin", p.repinned);
  expectField(w, x, "rep", p.repeated);
  expectField(w, x, "noFit", p.noFit);
}

void expectArray(const Where& w, const Value& x, const char* key, const uint32_t* got, size_t n) {
  TEST_ASSERT_EQUAL_MESSAGE(n, x[key].size(), w.msg(key).c_str());
  for (size_t i = 0; i < n; ++i) TEST_ASSERT_EQUAL_UINT32_MESSAGE(x[key][i].u64(), got[i], w.msg(key).c_str());
}

void expectStats(const Where& w, const Value& x, const Stats& s) {
  expectField(w, x, "picks", s.picks);
  expectField(w, x, "failed", s.failed);
  expectArray(w, x, "readsAtPick", s.readsAtPick, kReadsPerPick + 1);
  expectField(w, x, "prefetchReads", s.prefetchReads);
  expectField(w, x, "leftOut", s.leftOut);
  expectField(w, x, "damaged", s.damaged);
  expectField(w, x, "ioSkips", s.ioSkips);
  expectField(w, x, "broken", s.broken);
  expectField(w, x, "dry", s.dry);
  expectField(w, x, "loosened", s.loosened);
  expectField(w, x, "random", s.random);
  expectField(w, x, "noIndex", s.noIndex);
  expectField(w, x, "imputedTempo", s.imputedTempo);
  expectField(w, x, "imputedKey", s.imputedKey);
  expectArray(w, x, "classes", s.classes, kClasses);
  expectArray(w, x, "modePicks", s.modePicks, 2);
  expectArray(w, x, "tempoBreaks", s.tempoBreaks, 2);
  expectArray(w, x, "keyBreaks", s.keyBreaks, 2);
  expectField(w, x, "quickSkips", s.quickSkips);
  // (Counters added since the first 26 sessions: absent, 0.)
  expectField(w, x, "jumps", s.jumps);
  expectField(w, x, "repins", s.repins);
  expectField(w, x, "moodRepeats", s.moodRepeats);
  expectField(w, x, "noFit", s.noFit);
}

}  // namespace

void setUp() { gFail = false; }
void tearDown() {}

void test_sessions_match_the_reference() {
  Value fx;
  TEST_ASSERT_TRUE_MESSAGE(fixture(&fx), "test/fixtures/autodj/sessions.json");
  // The tables: a golden through mpdj::Table, the synthetic one as lists.
  struct Loaded {
    std::string bytes;
    std::unique_ptr<cc::MemSource> src;
    mpdj::Table table;
    std::vector<Entries> lists;
    bool golden = false;
    uint32_t k = 0, selHead = 0, selFloor = 0;
  };
  std::map<std::string, std::unique_ptr<Loaded>> tables;
  for (const auto& t : fx["tables"].members) {
    std::unique_ptr<Loaded> l(new Loaded());
    if (t.second.has("golden")) {
      l->golden = true;
      l->bytes = cardfixtures::bytes("golden/" + t.second["golden"].str());
      TEST_ASSERT_FALSE_MESSAGE(l->bytes.empty(), t.first.c_str());
      l->src.reset(new cc::MemSource(l->bytes.data(), static_cast<uint32_t>(l->bytes.size())));
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(l->table.open(*l->src)));
      l->k = l->table.k();
      l->selHead = l->table.selHead();
      l->selFloor = l->table.selFloor();
    } else {
      l->lists = hexLists(t.second["slots"]);
      TEST_ASSERT_EQUAL(t.second["rows"].u64(), l->lists.size());
      l->k = static_cast<uint32_t>(t.second["k"].u64());
      l->selHead = static_cast<uint32_t>(t.second["selHead"].u64());
      l->selFloor = static_cast<uint32_t>(t.second["selFloor"].u64());
    }
    tables[t.first] = std::move(l);
  }
  size_t sessions = 0, picks = 0, prefetches = 0;
  // Twice: the runs made at the read ahead (prefetch() given the maps and
  // k, as the firmware), then at the pick (prefetch() without them).
  const size_t count = fx["sessions"].size();
  for (size_t run = 0; run < 2 * count; ++run) {
    const bool lazy = run >= count;
    const Value& sv = fx["sessions"][run % count];
    Where w{sv["name"].str() + (lazy ? " (runs at the pick)" : ""), 0};
    const bool indexed = !sv["table"].isNull();
    Loaded* t = indexed ? tables[sv["table"].str()].get() : nullptr;
    TEST_ASSERT_TRUE_MESSAGE(!indexed || t, w.msg("table").c_str());
    MapsHolder mh;
    FakeReader reader;
    std::vector<uint16_t> words;  // the library's mood words (none: no mood data)
    for (const Value& v : sv["moods"].items) words.push_back(static_cast<uint16_t>(v.u64()));
    MoodView mood;
    if (indexed) {
      for (const Value& v : sv["trackToRow"].items) mh.t2r.push_back(static_cast<uint16_t>(v.u64()));
      for (const Value& v : sv["rowToTrack"].items) mh.r2t.push_back(static_cast<uint16_t>(v.u64()));
      for (const Value& v : sv["filters"].items) mh.f.push_back(static_cast<uint32_t>(v.u64()));
      mh.set();
      TEST_ASSERT_EQUAL_MESSAGE(t->golden ? t->table.rowCount() : t->lists.size(), mh.m.rows, w.msg("rows").c_str());
      if (t->golden)
        reader.table = &t->table;
      else
        reader.lists = &t->lists;
      reader.k = t->k;
      setFaults(&reader, sv["faults"]);
    }
    bool live = true;  // the index there (an `index` event takes it away, and back)
    const long allocs = gAllocs;
    Engine e;
    TEST_ASSERT_TRUE(e.begin(countAlloc, countFree));
    const long afterBegin = gAllocs;
    for (const Value& ev : sv["events"].items) {
      const std::string op = ev["op"].str();
      const Maps* maps = indexed && live ? &mh.m : nullptr;
      if (op == "new") {
        e.newSession(static_cast<uint32_t>(ev["seed"].u64()), static_cast<uint32_t>(ev["track"].u64()), maps);
      } else if (op == "start") {
        e.trackStarted(static_cast<uint32_t>(ev["track"].u64()), maps, ev["dj"].u64() != 0);
      } else if (op == "left") {
        e.trackLeft(static_cast<uint32_t>(ev["track"].u64()), maps, static_cast<uint32_t>(ev["ms"].u64()),
                    ev["natural"].u64() != 0);
      } else if (op == "index") {
        live = ev["on"].u64() != 0;
      } else if (op == "mode") {
        e.modeChanged();
      } else if (op == "arrived") {
        TEST_ASSERT_NOT_NULL_MESSAGE(maps, w.msg("arrived without the maps").c_str());
        e.mapsArrived(*maps);
      } else if (op == "reopen") {
        e.fileReopened();
      } else if (op == "renumber") {
        e.tracksRenumbered();
      } else if (op == "faults") {
        setFaults(&reader, ev);
      } else if (op == "filter") {
        // AutoDj::index()'s: the maps' and the pick's.
        mood = MoodView(sv.has("moods") ? words.data() : nullptr, static_cast<uint32_t>(words.size()),
                        static_cast<uint16_t>(ev["q"].u64()));
        mh.m.mood = mood;
      } else if (op == "prefetch") {
        Settings s = settings(ev["m"].u64() ? Mode::Locked : Mode::Rolling);
        uint16_t row = kNoRow;
        const bool due = e.prefetchDue(s, &row);
        expectField(w, ev, "row", due ? row : kNoRow);
        if (due) {
          const SlotResult r = lazy || !t ? e.prefetch(reader, row)
                                          : e.prefetch(reader, row, maps, static_cast<uint16_t>(t->k));
          TEST_ASSERT_EQUAL_STRING_MESSAGE(ev["res"].c_str(), resultName(r), w.msg("res").c_str());
        }
        prefetches += lazy == 0;
      } else if (op == "pick" || op == "surprise") {
        const Value& sj = ev["s"];
        const Settings s = settings(sj["m"].u64() ? Mode::Locked : Mode::Rolling,
                                    static_cast<uint8_t>(sj["f"].u64(55)), sj["t"].u64() != 0, sj["k"].u64() != 0);
        PickIn in;
        in.maps = maps;
        in.mood = mood;
        in.slots = maps ? &reader : nullptr;
        if (t) {
          in.k = static_cast<uint16_t>(t->k);
          in.selHead = static_cast<uint16_t>(t->selHead);
          in.selFloor = static_cast<uint8_t>(t->selFloor);
        }
        in.playingTrack = static_cast<uint32_t>(ev["playing"].u64(kNoTrackId));
        in.libraryTracks = static_cast<uint32_t>(sv["libraryTracks"].u64());
        const int readsBefore = reader.reads;
        const Pick p = op == "pick" ? e.pick(s, in) : e.surprise(s, in);
        expectPick(w, ev["expect"], p);
        TEST_ASSERT_EQUAL_MESSAGE(p.reads, reader.reads - readsBefore, w.msg("reader's reads").c_str());
        TEST_ASSERT_TRUE(p.reads <= kReadsPerPick);
        if (op == "pick" && !lazy) ++picks;
      } else {
        TEST_FAIL_MESSAGE(w.msg(op.c_str()).c_str());
      }
      ++w.event;
    }
    expectStats(w, sv["stats"], e.stats());
    TEST_ASSERT_EQUAL_MESSAGE(afterBegin, gAllocs, w.msg("allocations after begin()").c_str());
    TEST_ASSERT_EQUAL(1, afterBegin - allocs);
    sessions += lazy == 0;
  }
  char line[96];
  std::snprintf(line, sizeof line, "%u sessions, %u picks, %u prefetch passes replayed",
                static_cast<unsigned>(sessions), static_cast<unsigned>(picks), static_cast<unsigned>(prefetches));
  TEST_MESSAGE(line);
  TEST_ASSERT_GREATER_OR_EQUAL(300, picks);
  TEST_ASSERT_GREATER_OR_EQUAL(38, sessions);
}

// D2: BPM continuity and Harmonic mixing off by default (the design's 4.1
// listing said on); the rest as the design: off, Follow the vibe, 55 %.
void test_the_defaults() {
  const Settings s;
  TEST_ASSERT_FALSE(s.on);
  TEST_ASSERT_TRUE(s.mode == Mode::Rolling);
  TEST_ASSERT_EQUAL(kFloorMin, s.floorPct);
  TEST_ASSERT_EQUAL(55, kFloorMin);
  TEST_ASSERT_FALSE(s.matchTempo);
  TEST_ASSERT_FALSE(s.matchKey);
}

// ---------------------------------------------------------------------------
// The gate and dry
// ---------------------------------------------------------------------------
void test_the_gate_at_every_step_and_the_clamp() {
  // Row 0 lists 1-6 at 200 150 145 141 140 139 (each lists 0 back).
  std::vector<Entries> lists(7);
  const uint8_t levels[6] = {200, 150, 145, 141, 140, 139};
  for (int i = 0; i < 6; ++i) lists[0].push_back({static_cast<uint16_t>(i + 1), levels[i]});
  for (int r = 1; r < 7; ++r) lists[r].push_back({0, 200});
  struct Case {
    uint8_t floor, selFloor;
    uint16_t gated;
  };
  const Case cases[] = {{55, 140, 4}, {55, 0, 4}, {30, 140, 5}, {30, 0, 6}, {60, 140, 1}, {65, 140, 1},
                        {70, 140, 1}, {75, 140, 1}, {55, 145, 3}, {100, 0, 0}};
  for (const Case& c : cases) {
    Table t(lists);
    t.selFloor = c.selFloor;
    auto e = engine();
    e->newSession(1, 0, &t.maps.m);
    const Pick p = e->pick(settings(Mode::Rolling, c.floor), t.in(0));
    char msg[48];
    std::snprintf(msg, sizeof msg, "floor %d selFloor %d", c.floor, c.selFloor);
    if (c.gated) {
      TEST_ASSERT_EQUAL_MESSAGE(c.gated, p.gated, msg);
      TEST_ASSERT_FALSE_MESSAGE(p.dry, msg);
      TEST_ASSERT_TRUE(p.level >= floorLevel(c.floor, c.selFloor));
    } else {
      TEST_ASSERT_TRUE_MESSAGE(p.dry, msg);  // 100 %: 255, nothing passes
      TEST_ASSERT_EQUAL(6, p.gated);
    }
    TEST_ASSERT_EQUAL(6, p.candidates);
  }
}

void test_dry_takes_the_50_best_sums() {
  const int n = 70;
  std::vector<Entries> lists(n);
  for (int r = 2; r < n; ++r) {
    lists[0].push_back({static_cast<uint16_t>(r), static_cast<uint8_t>(130 - r % 7)});
    lists[1].push_back({static_cast<uint16_t>(r), static_cast<uint8_t>(100 + r % 5)});
  }
  lists[0] = sorted(lists[0]);
  lists[1] = sorted(lists[1]);
  Table t(lists);
  std::vector<std::pair<int, int>> sums;  // (-sum, row)
  for (int r = 2; r < n; ++r) sums.push_back({-((130 - r % 7) + (100 + r % 5)), r});
  std::sort(sums.begin(), sums.end());
  std::set<int> best;
  for (int i = 0; i < 50; ++i) best.insert(sums[i].second);
  for (uint32_t seed = 0; seed < 40; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    e->trackStarted(1, &t.maps.m, false);
    const Pick p = e->pick(settings(), t.in(1));
    TEST_ASSERT_TRUE(p.dry);
    TEST_ASSERT_EQUAL(68, p.candidates);
    TEST_ASSERT_EQUAL(50, p.gated);
    TEST_ASSERT_TRUE(best.count(p.row));
    TEST_ASSERT_EQUAL(130 - p.row % 7, p.level);
  }
}

// Every sum tied (row 0 lists rows 1-100 at one level below the floor): the
// 50 kept are the 50 lowest rows, the anchors and the playing track's row
// left out first.
void test_dry_ties_go_to_the_lower_rows() {
  const int n = 100;
  std::vector<Entries> lists(n + 1);
  for (int r = 1; r <= n; ++r) {
    lists[0].push_back({static_cast<uint16_t>(r), 100});
    lists[r].push_back({0, 100});
  }
  Table t(lists);
  std::set<uint16_t> seen;
  for (uint32_t seed = 0; seed < 40; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    const Pick p = e->pick(settings(Mode::Locked), t.in(0));
    TEST_ASSERT_TRUE(p.dry);
    TEST_ASSERT_EQUAL(100, p.candidates);
    TEST_ASSERT_EQUAL(50, p.gated);
    TEST_ASSERT_TRUE(p.row >= 1 && p.row <= 50);
    seen.insert(p.row);
    // Follow the vibe from 0 and 1 (1 playing): 2-51.
    auto f = engine();
    f->newSession(seed, 0, &t.maps.m);
    f->trackStarted(1, &t.maps.m, false);
    const Pick q = f->pick(settings(Mode::Rolling, 65), t.in(1));
    TEST_ASSERT_TRUE(q.dry);
    TEST_ASSERT_EQUAL(99, q.candidates);
    TEST_ASSERT_EQUAL(50, q.gated);
    TEST_ASSERT_TRUE(q.row >= 2 && q.row <= 51);
  }
  TEST_ASSERT_TRUE(seen.size() >= 20);  // drawn among all 50
}

// ---------------------------------------------------------------------------
// The anchors
// ---------------------------------------------------------------------------
void test_the_ring_d1_and_the_quick_skip() {
  Table t(star(12));
  const Maps* m = &t.maps.m;
  auto e = engine();
  e->newSession(5, 3, m);
  TEST_ASSERT_EQUAL(1, e->anchors());
  TEST_ASSERT_EQUAL(3, e->anchor(0));
  for (uint32_t tr : {4, 5, 6, 7, 8}) e->trackStarted(tr, m, false);
  const uint16_t five[] = {4, 5, 6, 7, 8};
  TEST_ASSERT_EQUAL(5, e->anchors());  // 5 at most: the seed is out
  for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL(five[i], e->anchor(i));
  TEST_ASSERT_TRUE(e->rowCooled(3));    // ... and still cooled
  e->trackStarted(5, m, false);         // again: to the end
  const uint16_t moved[] = {4, 6, 7, 8, 5};
  for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL(moved[i], e->anchor(i));
  e->trackStarted(3, m, true);          // Auto DJ's (its row cooled: joined when picked): nothing moves
  for (int i = 0; i < 5; ++i) TEST_ASSERT_EQUAL(moved[i], e->anchor(i));
  e->trackLeft(5, m, 29999, false);     // a quick skip
  TEST_ASSERT_EQUAL(4, e->anchors());
  TEST_ASSERT_TRUE(e->rowCooled(5));
  e->trackLeft(8, m, 12000, true);      // a natural end under 30 s: stays
  e->trackLeft(7, m, kQuickSkipMs, false);  // 30 s played: stays
  TEST_ASSERT_EQUAL(4, e->anchors());
  TEST_ASSERT_EQUAL(1, e->stats().quickSkips);
  const Pick p = e->pick(settings(), t.in(8));
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(p.row, e->anchor(e->anchors() - 1));  // the pick joins when picked
  TEST_ASSERT_TRUE(e->rowCooled(p.row));
}

// A pick made by track id while the index was away (no maps: absent, or a
// join after NoMemory) steers once it plays with the index there (D1): its
// row joins at its start. A pick made with the index joined when picked:
// its start moves nothing.
void test_a_pick_by_id_steers_once_the_index_is_there() {
  Table t(star(10));
  const Maps* m = &t.maps.m;
  auto e = engine();
  e->newSession(3, 4);  // no maps: the seed's row unknown
  TEST_ASSERT_EQUAL(0, e->anchors());
  PickIn away;
  away.playingTrack = 4;
  away.libraryTracks = m->tracks;
  const Pick p = e->pick(settings(), away);
  TEST_ASSERT_TRUE(p.ok && p.noIndex);
  TEST_ASSERT_EQUAL_HEX16(kNoRow, p.row);
  e->trackStarted(p.track, m, true);  // the index is back when it starts
  TEST_ASSERT_EQUAL(1, e->anchors());
  TEST_ASSERT_EQUAL(t.maps.t2r[p.track], e->anchor(0));
  TEST_ASSERT_TRUE(e->rowCooled(e->anchor(0)));
  const Pick q = e->pick(settings(), t.in(p.track));
  TEST_ASSERT_TRUE(q.ok);
  TEST_ASSERT_FALSE(q.random);  // it anchors this pick
  e->trackStarted(7, m, false);
  const uint16_t ring[] = {e->anchor(0), e->anchor(1), e->anchor(2)};
  TEST_ASSERT_EQUAL(3, e->anchors());
  TEST_ASSERT_EQUAL(q.row, ring[1]);
  e->trackStarted(q.track, m, true);  // joined when picked: stays where it is
  TEST_ASSERT_EQUAL(3, e->anchors());
  for (uint8_t i = 0; i < 3; ++i) TEST_ASSERT_EQUAL(ring[i], e->anchor(i));
}

void test_tracks_without_rows_and_a_seed_without_a_row() {
  // Tracks 6 and 7 have no row.
  Table t(full(6), {}, {0, 1, 2, 3, 4, 5, kNoRow, kNoRow});
  const Maps* m = &t.maps.m;
  auto e = engine();
  e->newSession(9, 6, m);
  TEST_ASSERT_TRUE(e->seeded());
  TEST_ASSERT_EQUAL(0, e->anchors());
  TEST_ASSERT_EQUAL_HEX16(kNoRow, e->pin());
  const Pick p = e->pick(settings(Mode::Locked), t.in(6));  // Stay on seed: the first pick is random
  TEST_ASSERT_TRUE(p.ok && p.random && !p.noIndex);
  TEST_ASSERT_EQUAL(0, p.reads);
  TEST_ASSERT_EQUAL(p.row, e->pin());  // ... and becomes the seed
  e->trackStarted(7, m, false);        // a track with no row never anchors
  TEST_ASSERT_EQUAL(1, e->anchors());
  const Pick q = e->pick(settings(Mode::Locked), t.in(7));
  TEST_ASSERT_FALSE(q.random);
  TEST_ASSERT_EQUAL(1, q.reads);  // the pin's slot
}

void test_the_pin_and_anchor_changes() {
  Table t(star(10));
  auto e = engine();
  e->newSession(2, 0, &t.maps.m);
  for (int i = 0; i < 4; ++i) {
    const Pick p = e->pick(settings(Mode::Locked), t.in(0));
    TEST_ASSERT_EQUAL(0, e->pin());
    TEST_ASSERT_NOT_EQUAL(0, p.row);
    TEST_ASSERT_EQUAL(i == 0 ? 1 : 0, p.reads);  // the pin's one slot, then cached
  }
  e->modeChanged();
  TEST_ASSERT_EQUAL_HEX16(kNoRow, e->pin());
  const Pick p = e->pick(settings(Mode::Locked), t.in(4));  // the playing track becomes the pin
  TEST_ASSERT_EQUAL(4, e->pin());
  TEST_ASSERT_EQUAL(0, p.row);  // row 4 lists only the centre
  uint16_t row = 0;
  TEST_ASSERT_FALSE(e->prefetchDue(settings(Mode::Locked), &row));  // read at that pick
}

void test_the_seed_from_the_first_track() {
  Table t(star(8));
  auto e = engine();
  e->newSession(4);
  TEST_ASSERT_FALSE(e->seeded());
  e->pick(settings(Mode::Locked), t.in(0));  // the playing track at the first pick
  TEST_ASSERT_TRUE(e->seeded());
  TEST_ASSERT_EQUAL(0, e->pin());
  // Switched on with tracks still to play (no seed track): the first track
  // to start anchors the ring but is no pin; Stay on seed's pin is the
  // playing track at the first pick (the design's 2.2, as the mobile app
  // and the terminal).
  e->newSession(4);
  e->trackStarted(3, &t.maps.m, false);
  TEST_ASSERT_TRUE(e->seeded());
  TEST_ASSERT_EQUAL_HEX16(kNoRow, e->pin());
  TEST_ASSERT_EQUAL(1, e->anchors());
  TEST_ASSERT_EQUAL(3, e->anchor(0));
  e->trackStarted(5, &t.maps.m, false);
  const Pick p = e->pick(settings(Mode::Locked), t.in(5));
  TEST_ASSERT_EQUAL(5, e->pin());
  TEST_ASSERT_EQUAL(1, p.reads);  // the pin's slot alone
  TEST_ASSERT_EQUAL(0, p.row);    // row 5 lists only the centre
}

void test_the_key_anchor_is_lazy() {
  std::vector<uint32_t> f;
  for (int r = 0; r < 10; ++r) f.push_back(filterWord(1200, r < 2 ? 0 : 1 + r, 0, 0));
  Table t(star(10), f);
  auto e = engine();
  e->newSession(6, 0, &t.maps.m);
  e->pick(settings(Mode::Locked, 55, false, false), t.in(0));
  TEST_ASSERT_EQUAL(0, e->keyAnchor());  // off: never taken
  const Pick p = e->pick(settings(Mode::Locked, 55, false, true), t.in(1));  // turned on mid-session
  TEST_ASSERT_EQUAL(filterCamelot(f[p.row]), e->keyAnchor());  // the playing track had none: the pick's
  TEST_ASSERT_FALSE(p.keyBreak);                                  // no anchor at the pick: never a break
  const uint8_t kept = e->keyAnchor();
  e->pick(settings(Mode::Locked, 55, false, false), t.in(5));
  TEST_ASSERT_EQUAL(kept, e->keyAnchor());  // off keeps it
  e->newSession(7, 0, &t.maps.m);
  e->pick(settings(Mode::Locked, 55, false, true), t.in(6));
  TEST_ASSERT_EQUAL(7, e->keyAnchor());  // the playing track's first
}

// ---------------------------------------------------------------------------
// Cooldowns and classes
// ---------------------------------------------------------------------------
void test_the_cooldowns() {
  const int n = 80;
  std::vector<uint32_t> f;
  for (int r = 0; r < n; ++r) f.push_back(filterWord(0, 0, 0, static_cast<uint32_t>(r % 4)));  // artists 0 (none), 1-3
  Table t(star(n), f);
  auto e = engine();
  e->newSession(8, 0, &t.maps.m);
  std::vector<uint16_t> picks;
  for (int i = 0; i < 79; ++i) {
    const Pick p = e->pick(settings(Mode::Locked), t.in(picks.empty() ? 0 : t.maps.r2t[picks.back()]));
    TEST_ASSERT_TRUE(p.ok);
    picks.push_back(p.row);
    TEST_ASSERT_TRUE(p.cooldownLevel < 2);  // 79 rows, 50 cooled at most: never a repeat
  }
  TEST_ASSERT_EQUAL(50, e->cooledRows());
  TEST_ASSERT_EQUAL(3, e->cooledArtists());
  for (uint8_t a = 0; a < e->cooledArtists(); ++a) TEST_ASSERT_NOT_EQUAL(0, e->cooledArtist(a));  // 0: never
  for (int i = 0; i < 49; ++i)
    for (int j = i + 1; j < 50; ++j) TEST_ASSERT_NOT_EQUAL(picks[i], picks[j]);
  // Artist 0 never cooled: while fresh rows of artist 0 are left, a pick with
  // cooled artists all around is level 0.
  auto e2 = engine();
  e2->newSession(9, 0, &t.maps.m);
  for (int i = 0; i < 12; ++i) {
    const Pick p = e2->pick(settings(Mode::Locked), t.in(0));
    if (p.cooldownLevel == 0 && f[p.row] >> 16) {
      // a fresh artist: not among the 3 before this pick
      bool cooledBefore = false;
      for (uint8_t a = 0; a + 1 < e2->cooledArtists(); ++a) cooledBefore |= e2->cooledArtist(a) == (f[p.row] >> 16);
      TEST_ASSERT_FALSE(cooledBefore);
    }
    if (i < 6) TEST_ASSERT_NOT_EQUAL(2, p.cooldownLevel);
  }
}

void test_every_candidate_cooled_gives_repeats() {
  // 6 rows: the seed's slot lists 5; after 5 picks every one is cooled.
  Table t(star(6));
  auto e = engine();
  e->newSession(3, 0, &t.maps.m);
  std::set<uint16_t> seen;
  for (int i = 0; i < 5; ++i) {
    const Pick p = e->pick(settings(Mode::Locked), t.in(0));
    TEST_ASSERT_EQUAL(0, p.cooldownLevel);
    seen.insert(p.row);
  }
  TEST_ASSERT_EQUAL(5, seen.size());
  for (int i = 0; i < 5; ++i) {
    const Pick p = e->pick(settings(Mode::Locked), t.in(0));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL(2, p.cooldownLevel);  // a repeat, never silence
    TEST_ASSERT_EQUAL(5, p.inClass);
  }
}

// Loosen before repeating (the user's decision, 2026-10-14), the device's
// case: Stay on seed at 75 % from a seed listing 3 rows above the floor
// (level 192) and 57 below. The old rule repeated the 3 (on the device:
// 197 of 200 picks cooled, 3 artists); now, once they are cooled, the 50
// nearest rows outside the track cooldown, so no pick repeats inside the
// cooldown while 60 rows are listed; the 3 come back as they leave it.
void test_loosen_before_repeating_the_device_case() {
  const int n = 61;
  std::vector<Entries> lists(n);
  std::vector<uint32_t> f(n);
  auto level = [](int r) { return static_cast<uint8_t>(r <= 3 ? 202 - 2 * r : 195 - r); };  // 200 198 196, 191..135
  for (int r = 1; r < n; ++r) {
    lists[0].push_back({static_cast<uint16_t>(r), level(r)});
    lists[r].push_back({0, 200});
    f[r] = filterWord(1200, static_cast<uint8_t>(1 + r % 24), 0, static_cast<uint32_t>(1 + r % 12));
  }
  Table t(lists, f);
  for (bool matching : {false, true}) {
    auto e = engine();
    e->newSession(0x717feda5, 0, &t.maps.m);
    const Settings s = settings(Mode::Locked, 75, matching, matching);
    uint32_t playing = 0, loosened = 0;
    std::set<uint16_t> seen;
    for (int i = 0; i < 200; ++i) {
      uint16_t row;
      if (e->prefetchDue(s, &row)) e->prefetch(t.reader, row, &t.maps.m, static_cast<uint16_t>(t.k));
      std::vector<uint16_t> fresh, freshGated;  // before the pick, in row order (the level falls with the row)
      for (uint16_t r = 1; r < n; ++r) {
        if (e->rowCooled(r) || r == playing) continue;
        fresh.push_back(r);
        if (r <= 3) freshGated.push_back(r);
      }
      const Pick p = e->pick(s, t.in(playing));
      TEST_ASSERT_TRUE(p.ok && !p.random && !p.dry);
      TEST_ASSERT_TRUE(p.cooldownLevel < 2);  // never a repeat inside the cooldown
      if (freshGated.empty()) {
        TEST_ASSERT_TRUE(p.loosened);
        TEST_ASSERT_EQUAL(std::min<size_t>(50, fresh.size()), p.gated);
        TEST_ASSERT_TRUE(p.level < 192);
        TEST_ASSERT_TRUE(p.row <= fresh[std::min<size_t>(50, fresh.size()) - 1]);  // among the 50 nearest fresh
        ++loosened;
      } else {
        TEST_ASSERT_FALSE(p.loosened);
        TEST_ASSERT_TRUE(std::find(freshGated.begin(), freshGated.end(), p.row) != freshGated.end());
      }
      if (i < 3) TEST_ASSERT_FALSE(p.loosened);
      seen.insert(p.row);
      playing = p.track;
    }
    TEST_ASSERT_TRUE(loosened >= 180 && loosened <= 194);  // the 3 above the floor come back every 50 picks
    TEST_ASSERT_EQUAL(60, seen.size());
    TEST_ASSERT_EQUAL(loosened, e->stats().loosened);
  }
}

// The same starvation in Follow the vibe: rows 0-7 list each other above
// every step and the rest below 75 %, the others list rows 0-7 above it.
// From row 0 at 75 % the clique's rows are cooled after 8 picks while the
// anchors list nothing else above the floor: loosened, never a repeat.
void test_loosen_in_follow_the_vibe() {
  const int n = 70;
  std::vector<Entries> lists(n);
  for (int a = 0; a < n; ++a) {
    for (int b = 0; b < n; ++b) {
      if (a == b) continue;
      int lv;
      if (a < 8 && b < 8) lv = 230 - 2 * std::abs(a - b);
      else if (a < 8) lv = 150 + (a * 11 + b * 7) % 40;
      else if (b < 8) lv = 195 + (a + b) % 5;
      else lv = 145 + (a * 13 + b * 17) % 46;
      lists[a].push_back({static_cast<uint16_t>(b), static_cast<uint8_t>(lv)});
    }
    lists[a] = sorted(lists[a]);
  }
  Table t(lists);
  auto e = engine();
  e->newSession(33, 0, &t.maps.m);
  uint32_t playing = 0, loosened = 0;
  for (int i = 0; i < 90; ++i) {
    uint16_t row;
    if (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row, &t.maps.m, static_cast<uint16_t>(t.k));
    const Pick p = e->pick(settings(Mode::Rolling, 75), t.in(playing));
    TEST_ASSERT_TRUE(p.ok && !p.random);
    TEST_ASSERT_TRUE(p.cooldownLevel < 2);
    if (p.loosened) {
      ++loosened;
      TEST_ASSERT_TRUE(p.level < 192 && p.row >= 8);
    }
    if (i < 7) TEST_ASSERT_TRUE(!p.loosened && p.row < 8);
    playing = p.track;
  }
  TEST_ASSERT_TRUE(loosened >= 40);
}

// Dry and loosened at once: the anchor lists 60 rows, all below the gate
// (dry: the 50 best sums, rows 1-50 here); the listener's tracks, each
// skipped at once, have cooled exactly those 50, and the track playing has
// no row: the 50 best sums outside the track cooldown, rows 51-60.
void test_dry_and_loosened() {
  const int n = 61;
  std::vector<Entries> lists(n);
  for (int r = 1; r < n; ++r) {
    lists[0].push_back({static_cast<uint16_t>(r), static_cast<uint8_t>(139 - (r - 1) / 2)});
    lists[r].push_back({0, 139});
  }
  std::vector<uint16_t> tracks;
  for (int r = 0; r < n; ++r) tracks.push_back(static_cast<uint16_t>(r));
  tracks.push_back(kNoRow);  // track 61: no row
  Table t(lists, {}, tracks);
  const Maps* m = &t.maps.m;
  for (uint32_t seed = 0; seed < 20; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, m);
    for (uint32_t r = 1; r <= 50; ++r) {
      e->trackStarted(r, m, false);
      TEST_ASSERT_TRUE(e->trackLeft(r, m, 1000, false));
    }
    e->trackStarted(61, m, false);
    TEST_ASSERT_EQUAL(1, e->anchors());
    const Pick p = e->pick(settings(), t.in(61));
    TEST_ASSERT_TRUE(p.ok && p.dry && p.loosened);
    TEST_ASSERT_EQUAL(60, p.candidates);
    TEST_ASSERT_EQUAL(10, p.gated);
    TEST_ASSERT_TRUE(p.row >= 51 && p.row <= 60);
    TEST_ASSERT_EQUAL(0, p.cooldownLevel);
  }
}

// A cached slot's run belongs to the maps it was made from. mapsChanged()
// (the join ready again) makes runDue() name each cached anchor once, and
// makeRun() remake it without a read; maps whose arrays moved are noticed
// alone; arrays filled again in place, unannounced, still never give a row
// with no track (the pick remakes its runs and draws again from the same
// stream: the pick a fresh engine makes).
void test_runs_follow_the_maps() {
  Table t(full(30));
  const Maps* m = &t.maps.m;
  const uint16_t k = static_cast<uint16_t>(t.k);
  auto e = engine();
  e->newSession(5, 0, m);
  for (uint32_t tr = 1; tr <= 4; ++tr) e->trackStarted(tr, m, false);
  uint16_t row = 0;
  while (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row, m, k);
  TEST_ASSERT_FALSE(e->runDue(settings(), *m, k, &row));  // made at the read ahead
  e->mapsChanged();
  int remade = 0;
  const int reads = t.reader.reads;
  while (e->runDue(settings(), *m, k, &row)) {
    e->makeRun(row, *m, k);
    ++remade;
  }
  TEST_ASSERT_EQUAL(5, remade);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);  // no read
  TEST_ASSERT_TRUE(e->runDue(settings(), *m, static_cast<uint16_t>(k - 1), &row));  // another k
  TEST_ASSERT_EQUAL(4, row);                                                     // newest first
  // Every row but the anchors and row 20 loses its track.
  std::vector<uint16_t> r2t = t.maps.r2t, t2r = t.maps.t2r;
  for (uint16_t r = 5; r < 30; ++r) {
    if (r == 20) continue;
    t2r[t.maps.r2t[r]] = kNoRow;
    r2t[r] = kNoTrack;
  }
  // Moved arrays (another join's blocks): noticed without mapsChanged().
  MapsHolder moved;
  moved.t2r = t2r;
  moved.r2t = r2t;
  moved.f = t.maps.f;
  moved.set();
  PickIn in = t.in(4);
  in.maps = &moved.m;
  for (uint32_t seed = 0; seed < 10; ++seed) {
    auto c = engine();
    c->copySession(*e);
    c->newSession(seed, 0, m);
    for (uint32_t tr = 1; tr <= 4; ++tr) c->trackStarted(tr, m, false);
    const Pick p = c->pick(settings(), in);
    TEST_ASSERT_EQUAL(20, p.row);
    TEST_ASSERT_EQUAL(0, p.reads);
  }
  // The same arrays filled again in place, no call: the stale runs list 25
  // rows, 24 of them without a track now; a draw of one of those is drawn
  // again from fresh runs (row 20), and a draw of row 20 itself stands. An
  // engine told (mapsChanged()) counts the one candidate from the start.
  t.maps.r2t = r2t;
  t.maps.t2r = t2r;
  t.maps.set();
  TEST_ASSERT_TRUE(t.maps.m.rowToTrack == m->rowToTrack);
  int redrawn = 0;
  for (uint32_t seed = 0; seed < 20; ++seed) {
    auto stale = engine();
    stale->copySession(*e);
    stale->newSession(seed, 0, m);
    for (uint32_t tr = 1; tr <= 4; ++tr) stale->trackStarted(tr, m, false);
    auto told = engine();
    told->copySession(*stale);
    told->mapsChanged();
    const Pick a = stale->pick(settings(), t.in(4));
    const Pick b = told->pick(settings(), t.in(4));
    TEST_ASSERT_EQUAL(20, a.row);
    TEST_ASSERT_EQUAL(t.maps.r2t[20], a.track);
    TEST_ASSERT_EQUAL(20, b.row);
    TEST_ASSERT_EQUAL(1, b.candidates);
    redrawn += a.candidates == 1;
  }
  TEST_ASSERT_TRUE(redrawn >= 10);  // 24 in 25 draws
}

// ---------------------------------------------------------------------------
// A mood filter (docs/MOOD-FILTERS.md 6.1)
// ---------------------------------------------------------------------------
namespace {

// A track's word from its scores, as the library builder takes it (the
// writer's tail, then the word): the valence's heads given, the rest even.
uint16_t wordOf(double happy, double sad) {
  const double scores[moodfilter::kHeads] = {0.5, happy, sad, 0.5, 0.5, 0.5, 0.5, 0.1};
  uint8_t tail[8];
  moodfilter::tailOf(scores, tail);
  return moodfilter::wordOfTail(tail);
}
const uint16_t kHappy = wordOf(0.8, 0.1);
const uint16_t kSad = wordOf(0.1, 0.8);

uint16_t qOf(moodfilter::Scale valence, moodfilter::Level level = moodfilter::Level::Standard) {
  moodfilter::Filter f;
  f.valence = valence;
  f.level = level;
  return f.pack();
}
const uint16_t kHappyQ = qOf(moodfilter::Scale::First);
const uint16_t kSadQ = qOf(moodfilter::Scale::Second);

}  // namespace

// Row 0 lists rows 1-9; the even tracks are happy, the odd sad: under
// Happy only rows 2, 4, 6, 8 are candidates, and the picks are those.
void test_a_filter_keeps_the_candidates_inside_the_mood() {
  Table t(star(10));
  std::vector<uint16_t> words;
  for (int tr = 0; tr < 10; ++tr) words.push_back(tr % 2 == 0 ? kHappy : kSad);
  t.maps.m.mood = MoodView(words.data(), static_cast<uint32_t>(words.size()), kHappyQ);
  TEST_ASSERT_TRUE(t.maps.m.mood.on());
  auto e = engine();
  e->newSession(9, 0, &t.maps.m);
  std::set<uint16_t> seen;
  for (int i = 0; i < 4; ++i) {
    const Pick p = e->pick(settings(), t.in(0));
    TEST_ASSERT_TRUE(p.ok && !p.random);
    TEST_ASSERT_EQUAL(0, p.row % 2);
    TEST_ASSERT_EQUAL(4 - i, p.candidates);  // (the ring's picks are anchors)
    seen.insert(p.row);
  }
  TEST_ASSERT_EQUAL(4, seen.size());
  // A track past the words passes no active filter; every track passes an inactive one.
  TEST_ASSERT_FALSE(MoodView(words.data(), 10, kHappyQ).passes(10));
  TEST_ASSERT_TRUE(MoodView(words.data(), 10, 0).passes(10));
}

// A level alone, an invalid q (voice 3) or no words: off, the same picks as
// no filter from the same stream, and the same print (the runs stay).
void test_a_level_alone_is_no_filter() {
  Table t(full(12));
  std::vector<uint16_t> words;
  for (int tr = 0; tr < 12; ++tr) words.push_back(tr % 3 ? kHappy : kSad);
  const uint16_t strict = qOf(moodfilter::Scale::Any, moodfilter::Level::Strict);
  const MoodView views[] = {MoodView(), MoodView(words.data(), 12, strict), MoodView(words.data(), 12, 0x30),
                            MoodView(nullptr, 0, kHappyQ)};
  std::vector<uint16_t> first;
  for (const MoodView& v : views) {
    TEST_ASSERT_FALSE(v.on());
    t.maps.m.mood = v;
    auto e = engine();
    e->newSession(21, 0, &t.maps.m);
    std::vector<uint16_t> rows;
    uint32_t playing = 0;
    for (int i = 0; i < 10; ++i) {
      const Pick p = e->pick(settings(), t.in(playing));
      rows.push_back(p.row);
      rows.push_back(p.candidates);
      playing = p.track;
    }
    if (first.empty()) first = rows;
    TEST_ASSERT_TRUE(rows == first);
  }
  TEST_ASSERT_EQUAL_HEX16(0, MoodView(words.data(), 12, 0x30).q);  // read as 0
  // By id the same: no word read, no draw added.
  std::vector<uint32_t> ids[2];
  for (int i = 0; i < 2; ++i) {
    auto e = engine();
    e->newSession(21, 2);
    PickIn in;
    in.mood = i ? MoodView(words.data(), 12, qOf(moodfilter::Scale::Any, moodfilter::Level::Loose)) : MoodView();
    in.playingTrack = 2;
    in.libraryTracks = 12;
    for (int n = 0; n < 14; ++n) ids[i].push_back(e->pick(settings(), in).track);
  }
  TEST_ASSERT_TRUE(ids[0] == ids[1]);
  // Runs made with no filter stay fresh under a level alone.
  t.maps.m.mood = MoodView();
  const Maps* m = &t.maps.m;
  const uint16_t k = static_cast<uint16_t>(t.k);
  auto e = engine();
  e->newSession(5, 0, m);
  for (uint32_t tr = 1; tr <= 4; ++tr) e->trackStarted(tr, m, false);
  uint16_t row = 0;
  while (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row, m, k);
  TEST_ASSERT_FALSE(e->runDue(settings(), *m, k, &row));
  t.maps.m.mood = views[1];
  TEST_ASSERT_FALSE(e->runDue(settings(), *m, k, &row));
}

// A changed filter makes the cached runs stale (its q is in their maps'
// print): runDue() names each anchor and makeRun() remakes it without a
// read, or the pick does. The words filled again in place under the same
// q are the maps' arrays filled again: mapsChanged() says so.
void test_a_filter_changed_remakes_the_runs() {
  Table t(full(30));
  std::vector<uint16_t> words;
  for (int tr = 0; tr < 30; ++tr) words.push_back(tr % 2 == 0 ? kHappy : kSad);
  Maps* m = &t.maps.m;
  m->mood = MoodView(words.data(), 30, kHappyQ);
  const uint16_t k = static_cast<uint16_t>(t.k);
  auto e = engine();
  e->newSession(5, 0, m);
  for (uint32_t tr = 1; tr <= 4; ++tr) e->trackStarted(tr, m, false);
  uint16_t row = 0;
  while (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row, m, k);
  TEST_ASSERT_FALSE(e->runDue(settings(), *m, k, &row));
  auto c = engine();
  c->copySession(*e);
  Pick p = c->pick(settings(), t.in(4));
  TEST_ASSERT_EQUAL(0, p.track % 2);
  TEST_ASSERT_EQUAL(0, p.reads);
  // Sad now: every cached run remade, no read.
  m->mood = MoodView(words.data(), 30, kSadQ);
  int remade = 0;
  const int reads = t.reader.reads;
  while (e->runDue(settings(), *m, k, &row)) {
    e->makeRun(row, *m, k);
    ++remade;
  }
  TEST_ASSERT_EQUAL(5, remade);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);
  c = engine();
  c->copySession(*e);
  p = c->pick(settings(), t.in(4));
  TEST_ASSERT_EQUAL(1, p.track % 2);
  // ... or at the pick, from runs still Sad's, after the filter went back.
  m->mood = MoodView(words.data(), 30, kHappyQ);
  for (uint32_t seed = 0; seed < 10; ++seed) {
    c = engine();
    c->copySession(*e);
    c->newSession(seed, 0, m);
    for (uint32_t tr = 1; tr <= 4; ++tr) c->trackStarted(tr, m, false);
    p = c->pick(settings(), t.in(4));
    TEST_ASSERT_EQUAL(0, p.track % 2);
    TEST_ASSERT_EQUAL(0, p.reads);
  }
  // Off: the runs hold every row again.
  m->mood = MoodView();
  TEST_ASSERT_TRUE(e->runDue(settings(), *m, k, &row));
  while (e->runDue(settings(), *m, k, &row)) e->makeRun(row, *m, k);
  c = engine();
  c->copySession(*e);
  TEST_ASSERT_EQUAL(25, c->pick(settings(), t.in(4)).candidates);
  // The same words' block filled again (a library rebuilt there), the same
  // q: the print can't tell, mapsChanged() does.
  m->mood = MoodView(words.data(), 30, kHappyQ);
  while (e->runDue(settings(), *m, k, &row)) e->makeRun(row, *m, k);
  for (uint16_t& w : words) w = w == kHappy ? kSad : kHappy;
  TEST_ASSERT_FALSE(e->runDue(settings(), *m, k, &row));
  e->mapsChanged();
  remade = 0;
  while (e->runDue(settings(), *m, k, &row)) {
    e->makeRun(row, *m, k);
    ++remade;
  }
  TEST_ASSERT_EQUAL(5, remade);
  for (uint32_t seed = 0; seed < 10; ++seed) {
    c = engine();
    c->copySession(*e);
    c->newSession(seed, 0, m);
    for (uint32_t tr = 1; tr <= 4; ++tr) c->trackStarted(tr, m, false);
    p = c->pick(settings(), t.in(4));
    TEST_ASSERT_EQUAL(1, p.track % 2);  // the odd tracks are happy now
  }
}

// Tracks 2 and 9 share row 2, whose track (rowToTrack's, the one a pick of
// it plays) is 2: sad, so under Happy the row is no candidate though track 9
// is happy (MOOD-FILTERS.md 6.3: testing the track alone could leak).
void test_a_shared_row_is_tested_by_its_first_track() {
  Table t(star(6), {}, {0, 1, 2, 3, 4, 5, kNoRow, kNoRow, kNoRow, 2});
  TEST_ASSERT_EQUAL(2, t.maps.r2t[2]);
  const std::vector<uint16_t> words = {kHappy, kSad, kSad, kHappy, kSad, kHappy, 0, 0, 0, kHappy};
  t.maps.m.mood = MoodView(words.data(), static_cast<uint32_t>(words.size()), kHappyQ);
  TEST_ASSERT_TRUE(t.maps.m.mood.passes(9) && !t.maps.m.mood.passes(2));
  for (uint32_t seed = 0; seed < 8; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    const Pick p = e->pick(settings(), t.in(0));
    TEST_ASSERT_EQUAL(2, p.candidates);  // rows 3 and 5
    TEST_ASSERT_TRUE(p.row == 3 || p.row == 5);
  }
  // The scan the same (6.3): row 0 lists only row 1, sad, so the pick
  // jumps; track 9 is a hit, but its row's track fails.
  t.lists[0] = {{1, 200}};
  for (uint32_t seed = 0; seed < 12; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    const Pick p = e->pick(settings(), t.in(0));
    TEST_ASSERT_TRUE(p.jump == Jump::NoCandidate);
    TEST_ASSERT_EQUAL(2, p.candidates);
    TEST_ASSERT_TRUE(p.row == 3 || p.row == 5);
  }
}

// Row 0 lists rows 1-4, of which only row 1 is happy; rows 5-8, happy, are
// listed by none. Row 5 shares row 0's artist; at 120 BPM row 6 is tight,
// 7 wide, 8 wrong.
std::unique_ptr<Table> jumpTable(std::vector<uint16_t>* words) {
  std::vector<Entries> lists = {{{1, 200}, {2, 199}, {3, 198}, {4, 197}}};
  for (int r = 1; r <= 8; ++r) lists.push_back({{0, 200}});
  const uint16_t bpm[] = {1200, 1200, 1200, 1200, 1200, 1200, 1220, 1300, 1600};
  const uint32_t art[] = {9, 1, 2, 3, 4, 9, 6, 7, 8};
  std::vector<uint32_t> f;
  for (int r = 0; r < 9; ++r) f.push_back(filterWord(bpm[r], 0, 0, art[r]));
  std::unique_ptr<Table> t(new Table(lists, f));
  *words = {kSad, kHappy, kSad, kSad, kSad, kHappy, kHappy, kHappy, kHappy};
  t->maps.m.mood = MoodView(words->data(), 9, kHappyQ);
  return t;
}

// A jump keeps the classes (MOOD-FILTERS.md 6.3): with row 1 an anchor,
// nothing listed is inside the mood (Jump::NoCandidate); with row 1 cooled
// and out of the ring, all of it is cooled (Jump::AllCooled), and no
// neighbour repeats. Either way the scan's rows, the artist cooldown and the
// tempo's tiers: row 6, alone in the best class.
void test_a_jump_keeps_the_classes() {
  std::vector<uint16_t> words;
  auto t = jumpTable(&words);
  const Maps* m = &t->maps.m;
  const Settings tempo = settings(Mode::Rolling, 55, true, false);
  for (uint32_t seed = 0; seed < 10; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, m);
    e->trackStarted(1, m, false);
    Pick p = e->pick(tempo, t->in(1));
    TEST_ASSERT_TRUE(p.ok && p.random && !p.noIndex && p.jump == Jump::NoCandidate);
    TEST_ASSERT_EQUAL(6, p.row);
    TEST_ASSERT_EQUAL(0, p.cooldownLevel);
    TEST_ASSERT_EQUAL(0, p.musicRank);
    TEST_ASSERT_EQUAL(4, p.candidates);  // rows 5-8
    TEST_ASSERT_EQUAL(1, p.inClass);
    TEST_ASSERT_EQUAL(0, p.level);
    e = engine();
    e->newSession(seed, 0, m);
    e->trackStarted(1, m, false);
    TEST_ASSERT_TRUE(e->trackLeft(1, m, 1000, false));
    e->trackStarted(0, m, false);
    p = e->pick(tempo, t->in(0));
    TEST_ASSERT_TRUE(p.jump == Jump::AllCooled);
    TEST_ASSERT_EQUAL(6, p.row);
    TEST_ASSERT_FALSE(p.dry || p.loosened || p.repeated);
    TEST_ASSERT_EQUAL(5, p.candidates);  // row 1 too, cooled: its class last
    TEST_ASSERT_EQUAL(1, e->stats().jumps);
  }
  // No filter: that pick repeats a neighbour (cooldown level 2), as before.
  t->maps.m.mood = MoodView(words.data(), 9, 0);
  auto e = engine();
  e->newSession(3, 0, m);
  for (uint32_t tr = 1; tr <= 4; ++tr) {
    e->trackStarted(tr, m, false);
    e->trackLeft(tr, m, 1000, false);
  }
  e->trackStarted(0, m, false);
  const Pick p = e->pick(tempo, t->in(0));
  TEST_ASSERT_TRUE(p.jump == Jump::None && !p.random);
  TEST_ASSERT_EQUAL(2, p.cooldownLevel);
  TEST_ASSERT_EQUAL(0, e->stats().jumps);
}

// 200 happy rows that no list holds: every jump's sample is 64 of them,
// the later ones taking a place at below(n) < 64.
void test_the_scan_keeps_64() {
  std::vector<Entries> lists = {{{1, 200}}};
  for (int r = 1; r <= 201; ++r) lists.push_back({{0, 200}});
  Table t(lists);
  std::vector<uint16_t> words(202, kHappy);
  words[0] = words[1] = kSad;
  t.maps.m.mood = MoodView(words.data(), 202, kHappyQ);
  std::set<uint16_t> rows;
  for (uint32_t seed = 0; seed < 300; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    const Pick p = e->pick(settings(), t.in(0));
    TEST_ASSERT_TRUE(p.jump == Jump::NoCandidate);
    TEST_ASSERT_EQUAL(64, p.candidates);
    TEST_ASSERT_EQUAL(64, p.inClass);
    rows.insert(p.row);
  }
  TEST_ASSERT_TRUE(rows.size() > 150);
  TEST_ASSERT_TRUE(*rows.begin() < 66 && *rows.rbegin() > 66);
}

// Tracks 1-4 fit. Once each is picked they are the anchors or cooled, and
// the picks repeat them in the order they were first picked: the track
// cooldown's oldest first, not playing (6.2, 8).
void test_repeats_oldest_first() {
  Table t(full(8));
  const std::vector<uint16_t> words = {kSad, kHappy, kHappy, kHappy, kHappy, kSad, kSad, kSad};
  t.maps.m.mood = MoodView(words.data(), 8, kHappyQ);
  auto e = engine();
  e->newSession(17, 0, &t.maps.m);
  uint32_t playing = 0;
  std::vector<uint16_t> rows;
  for (int i = 0; i < 12; ++i) {
    const Pick p = e->pick(settings(), t.in(playing));
    rows.push_back(p.row);
    if (i >= 4) {
      TEST_ASSERT_TRUE(p.repeated && p.random && !p.noIndex);
      TEST_ASSERT_EQUAL(2, p.cooldownLevel);
    }
    playing = p.track;
  }
  std::vector<uint16_t> first(rows.begin(), rows.begin() + 4);
  std::sort(first.begin(), first.end());
  TEST_ASSERT_TRUE(first == std::vector<uint16_t>({1, 2, 3, 4}));
  for (int i = 4; i < 12; ++i) TEST_ASSERT_EQUAL(rows[i - 4], rows[i]);
  TEST_ASSERT_EQUAL(8, e->stats().moodRepeats);
  TEST_ASSERT_EQUAL(8, e->stats().classes[2 * kRanks]);
}

// Rows 1-2 fit and are cooled; track 6, with no row, fits: it goes first,
// by id (Jump::NoRow), then the repeats.
void test_a_track_with_no_row_before_repeats() {
  Table t(full(5), {}, {0, 1, 2, 3, 4, kNoRow, kNoRow});
  const std::vector<uint16_t> words = {kSad, kHappy, kHappy, kSad, kSad, kSad, kHappy};
  t.maps.m.mood = MoodView(words.data(), 7, kHappyQ);
  auto e = engine();
  e->newSession(4, 0, &t.maps.m);
  uint32_t playing = 0;
  std::vector<Pick> got;
  for (int i = 0; i < 5; ++i) {
    const Pick p = e->pick(settings(), t.in(playing));
    got.push_back(p);
    playing = p.track;
    e->trackStarted(p.track, &t.maps.m, true);
  }
  TEST_ASSERT_TRUE(got[0].track != got[1].track && got[0].track + got[1].track == 3);  // 1 and 2
  TEST_ASSERT_EQUAL(6, got[2].track);
  TEST_ASSERT_EQUAL_HEX16(kNoRow, got[2].row);
  TEST_ASSERT_TRUE(got[2].jump == Jump::NoRow && got[2].noIndex && !got[2].repeated);
  TEST_ASSERT_TRUE(got[3].repeated && got[4].repeated && got[3].row != kNoRow);
}

// Track 3 alone fits, and plays: it again, by id (6.2, 9), with the index or
// without; nothing fits: no pick.
void test_the_only_track_repeats() {
  Table t(full(6));
  const std::vector<uint16_t> words = {kSad, kSad, kSad, kHappy, kSad, kSad};
  moodfilter::Filter none;
  none.valence = moodfilter::Scale::First;
  none.energy = moodfilter::Scale::Second;
  none.voice = moodfilter::Voice::Instrumental;
  for (int indexed = 1; indexed >= 0; --indexed) {
    t.maps.m.mood = MoodView(words.data(), 6, kHappyQ);
    PickIn in = t.in(3);
    if (!indexed) {
      in.maps = nullptr;
      in.slots = nullptr;
    }
    auto e = engine();
    e->newSession(8, 3, indexed ? &t.maps.m : nullptr);
    for (int i = 0; i < 3; ++i) {
      const Pick p = e->pick(settings(), in);
      TEST_ASSERT_TRUE(p.ok && p.repeated && p.noIndex);
      TEST_ASSERT_EQUAL(3, p.track);
      TEST_ASSERT_EQUAL(indexed ? 3 : kNoRow, p.row);
    }
    t.maps.m.mood = in.mood = MoodView(words.data(), 6, none.pack());
    const Pick p = e->pick(settings(), in);
    TEST_ASSERT_TRUE(!p.ok && p.noFit);
    TEST_ASSERT_EQUAL_UINT32(kNoTrackId, p.track);
    TEST_ASSERT_EQUAL(1, e->stats().noFit);
    TEST_ASSERT_EQUAL(1, e->stats().failed);
    TEST_ASSERT_EQUAL(3, e->stats().picks);
  }
}

// Row 0, the seed, lists rows 1-2, sad; row 3, happy, lists rows 4-5,
// happy; row 6, happy, is listed by none (the scan's). Under Happy, Stay on
// seed from row 0 with the listener on row 3 re-pins onto it (MOOD-FILTERS.md
// 6.4).
std::unique_ptr<Table> repinTable(std::vector<uint16_t>* words) {
  std::vector<Entries> lists = {{{1, 200}, {2, 199}}, {{0, 200}}, {{0, 200}}, {{4, 200}, {5, 199}},
                                {{3, 200}},           {{3, 200}}, {{0, 200}}, {{0, 200}}};
  std::unique_ptr<Table> t(new Table(lists));
  *words = {kSad, kSad, kSad, kHappy, kHappy, kHappy, kHappy, kSad};
  t->maps.m.mood = MoodView(words->data(), 8, kHappyQ);
  return t;
}

// The second round shares the first's reads (kReadsPerPick), and round 1
// draws nothing before the re-pin; past the budget, the scan, the moved pin
// read ahead; the second round finding nothing jumps, both marked.
void test_the_repin_stays_in_the_budget() {
  const Settings lock = settings(Mode::Locked);
  std::vector<uint16_t> words;
  // Row 0's slot, then row 3's: two reads; row 0's read ahead, one; both, none.
  for (int ahead = 0; ahead <= 2; ++ahead) {
    auto t = repinTable(&words);
    const Maps* m = &t->maps.m;
    auto e = engine();
    e->newSession(5, 0, m);
    if (ahead == 2) e->prefetch(t->reader, 3, m, static_cast<uint16_t>(t->k));
    if (ahead) e->prefetch(t->reader, 0, m, static_cast<uint16_t>(t->k));
    e->trackStarted(3, m, false);
    const Pick p = e->pick(lock, t->in(3));
    TEST_ASSERT_TRUE(p.ok && p.repinned && p.jump == Jump::None && !p.random);
    TEST_ASSERT_EQUAL(2 - ahead, p.reads);
    TEST_ASSERT_TRUE(p.row == 4 || p.row == 5);
    TEST_ASSERT_EQUAL(3, e->pin());
    TEST_ASSERT_TRUE(e->pinMoved());
    TEST_ASSERT_EQUAL(1, e->stats().repins);
    TEST_ASSERT_EQUAL(0, e->stats().jumps);
  }
  // No draw before it: the pick a session pinned on row 3 from the start
  // makes, from the same stream.
  for (uint32_t seed = 0; seed < 8; ++seed) {
    auto t = repinTable(&words);
    const Maps* m = &t->maps.m;
    auto e = engine();
    e->newSession(seed, 0, m);
    e->trackStarted(3, m, false);
    auto twin = engine();
    twin->newSession(seed, 3, m);
    TEST_ASSERT_EQUAL(twin->pick(lock, t->in(3)).row, e->pick(lock, t->in(3)).row);
  }
  // Two reads of row 0's (its CRC failed once): none left for the new pin's,
  // so the scan (random, no jump); the moved pin is the next read ahead.
  {
    auto t = repinTable(&words);
    t->reader.flaky = {0};
    const Maps* m = &t->maps.m;
    auto e = engine();
    e->newSession(5, 0, m);
    e->trackStarted(3, m, false);
    const Pick p = e->pick(lock, t->in(3));
    TEST_ASSERT_TRUE(p.ok && p.repinned && p.random && p.jump == Jump::None);
    TEST_ASSERT_EQUAL(2, p.reads);
    TEST_ASSERT_EQUAL(1, e->stats().leftOut);
    TEST_ASSERT_TRUE(p.row >= 4 && p.row <= 6);
    uint16_t row = kNoRow;
    TEST_ASSERT_TRUE(e->prefetchDue(lock, &row));
    TEST_ASSERT_EQUAL(3, row);
  }
  // Nothing inside the mood in the new pin's list either: the jump.
  {
    auto t = repinTable(&words);
    t->lists[3] = {{1, 200}, {2, 199}};
    const Maps* m = &t->maps.m;
    auto e = engine();
    e->newSession(5, 0, m);
    e->trackStarted(3, m, false);
    const Pick p = e->pick(lock, t->in(3));
    TEST_ASSERT_TRUE(p.ok && p.repinned && p.random && p.jump == Jump::NoCandidate);
    TEST_ASSERT_EQUAL(3, p.candidates);  // rows 4-6
    TEST_ASSERT_EQUAL(1, e->stats().repins);
    TEST_ASSERT_EQUAL(1, e->stats().jumps);
  }
}

// A moved pin goes at a quick skip of it (by its track, or by the row the
// loop step remembers while the maps are away) and at an Anchor change, its
// mark with it; the next Stay on seed pick takes the playing track's row. A
// natural end, or 30 s played, keeps it; so does a skip whose row is unknown.
void test_a_moved_pin_goes_at_a_quick_skip() {
  const Settings lock = settings(Mode::Locked);
  std::vector<uint16_t> words;
  auto t = repinTable(&words);
  const Maps* m = &t->maps.m;
  enum Leave { Quick, Natural, Played, Anchor, ByRow, Unknown };
  for (int leave = Quick; leave <= Unknown; ++leave) {
    auto e = engine();
    e->newSession(5, 0, m);
    e->trackStarted(3, m, false);
    const Pick p = e->pick(lock, t->in(3));
    TEST_ASSERT_TRUE(p.repinned && e->pinMoved());
    TEST_ASSERT_EQUAL(3, e->pin());
    if (leave == Anchor) {
      e->modeChanged();
    } else if (leave == ByRow) {
      e->rowLeft(3, 5000, false);
    } else {
      e->trackLeft(3, leave == Unknown ? nullptr : m, leave == Played ? kQuickSkipMs : 5000, leave == Natural);
    }
    const bool gone = leave == Quick || leave == Anchor || leave == ByRow;
    TEST_ASSERT_EQUAL_MESSAGE(gone ? kNoRow : 3, e->pin(), "the pin");
    TEST_ASSERT_EQUAL_MESSAGE(!gone, e->pinMoved(), "its mark");
    e->trackStarted(p.track, m, true);
    e->pick(lock, t->in(p.track));
    TEST_ASSERT_EQUAL(gone ? p.row : 3, e->pin());
    TEST_ASSERT_EQUAL(!gone, e->pinMoved());
  }
}

// The seed's pin, and one a pick took from the playing track, stay at a
// quick skip. A pin moves only in Stay on seed (Follow the vibe with row 0
// its only anchor jumps), only under a filter (none: row 0's own list, as
// today), and never onto a row outside the mood, the pin itself, or a
// damaged row.
void test_a_seed_pin_stays() {
  const Settings lock = settings(Mode::Locked);
  std::vector<uint16_t> words;
  auto t = repinTable(&words);
  const Maps* m = &t->maps.m;
  auto e = engine();
  e->newSession(5, 0, m);
  TEST_ASSERT_TRUE(e->trackLeft(0, m, 5000, false));  // out of the ring, still the pin
  TEST_ASSERT_EQUAL(0, e->pin());
  TEST_ASSERT_FALSE(e->pinMoved());
  e->modeChanged();
  e->pick(lock, t->in(6));  // the pin from the playing track
  TEST_ASSERT_EQUAL(6, e->pin());
  e->trackLeft(6, m, 5000, false);
  TEST_ASSERT_EQUAL(6, e->pin());
  enum Case { Rolling, Off, Outside, OnPin, Damaged };
  for (int c = Rolling; c <= Damaged; ++c) {
    auto u = repinTable(&words);
    const Maps* um = &u->maps.m;
    if (c == Off) u->maps.m.mood = MoodView(words.data(), 8, 0);
    auto f = engine();
    f->newSession(5, 0, um);
    const uint32_t playing = c == Outside ? 7 : c == OnPin ? 0 : 3;
    if (c == Damaged) {
      u->reader.damaged = {3};
      f->prefetch(u->reader, 3);
      f->prefetch(u->reader, 3);
      TEST_ASSERT_TRUE(f->rowDamaged(3));
    }
    if (c != OnPin && c != Rolling) f->trackStarted(playing, um, false);
    const Pick p = f->pick(c == Rolling ? settings() : lock, u->in(playing));
    TEST_ASSERT_FALSE(p.repinned);
    TEST_ASSERT_EQUAL(0, f->pin());
    TEST_ASSERT_FALSE(f->pinMoved());
    TEST_ASSERT_TRUE(p.jump == (c == Off ? Jump::None : Jump::NoCandidate));
    TEST_ASSERT_TRUE(c == Off ? p.row == 1 || p.row == 2 : p.row >= 3 && p.row <= 6);
    TEST_ASSERT_EQUAL(0, f->stats().repins);
  }
}

// trackLeft() says whether the track stopped steering: a row in the ring
// leaves it; a track with no row (the console's '[dj] a quick skip' line,
// 2026-10-13) or one already out never steered. Each quick skip counts.
void test_a_quick_skip_says_whether_it_steered() {
  Table t(star(8), {}, {0, 1, 2, 3, 4, 5, 6, 7, kNoRow});  // track 8: no row
  const Maps* m = &t.maps.m;
  auto e = engine();
  e->newSession(1, 0, m);
  e->trackStarted(3, m, false);
  TEST_ASSERT_TRUE(e->trackLeft(3, m, 2000, false));
  TEST_ASSERT_FALSE(e->trackLeft(3, m, 2000, false));  // out of the ring already
  e->trackStarted(8, m, false);
  TEST_ASSERT_FALSE(e->trackLeft(8, m, 2500, false));  // no row
  TEST_ASSERT_FALSE(e->rowLeft(kNoRow, 2500, false));
  TEST_ASSERT_EQUAL(4, e->stats().quickSkips);
  TEST_ASSERT_FALSE(e->trackLeft(0, m, 1000, true));          // a natural end
  TEST_ASSERT_FALSE(e->trackLeft(0, m, kQuickSkipMs, false));  // played 30 s
  TEST_ASSERT_EQUAL(1, e->anchors());
  TEST_ASSERT_EQUAL(4, e->stats().quickSkips);
}

void test_cooldown_before_music() {
  // Row 1 matches the tempo but is cooled; row 2 is fresh and wrong: row 2.
  std::vector<Entries> lists = {{{1, 200}, {2, 199}}, {{0, 200}}, {{0, 199}}, {{0, 100}}};
  Table t(lists, {filterWord(1200, 0, 0, 0), filterWord(1200, 0, 0, 0), filterWord(900, 0, 0, 0),
                  filterWord(1200, 0, 0, 0)});
  auto e = engine();
  e->newSession(1, 0, &t.maps.m);
  e->trackStarted(1, &t.maps.m, false);
  e->trackStarted(3, &t.maps.m, false);
  e->trackStarted(0, &t.maps.m, false);
  const Pick p = e->pick(settings(Mode::Locked, 55, true, false), t.in(3));
  TEST_ASSERT_EQUAL(2, p.row);
  TEST_ASSERT_EQUAL(0, p.cooldownLevel);
  TEST_ASSERT_EQUAL(3, p.musicRank);
  TEST_ASSERT_TRUE(p.tempoBreak);
}

void test_no_bpm_or_key_anywhere() {
  Table t(full(12));  // every filter word 0
  auto e = engine();
  e->newSession(4, 0, &t.maps.m);
  for (int i = 0; i < 6; ++i) {
    const Pick p = e->pick(settings(Mode::Locked, 55, true, true), t.in(0));
    // The playing track has no BPM: tempo Na; no key known: Unknown ranks 1.
    TEST_ASSERT_EQUAL(1, p.musicRank);
    TEST_ASSERT_FALSE(p.imputedTempo || p.imputedKey || p.tempoBreak || p.keyBreak);
  }
  TEST_ASSERT_EQUAL(0, e->keyAnchor());
}

void test_imputation_gives_missing_values_their_share() {
  // The seed lists 60 rows: 40 with a tight BPM and a good key... and 20
  // with neither. Over many sessions a row with no data is picked about as
  // often as one with data (D3), never starved.
  const int n = 61;
  std::vector<Entries> lists(n);
  std::vector<uint32_t> f(n);
  f[0] = filterWord(1200, 5, 0, 0);
  for (int r = 1; r < n; ++r) {
    lists[0].push_back({static_cast<uint16_t>(r), 220});
    lists[r].push_back({0, 220});
    f[r] = r <= 40 ? filterWord(1200, 5, 0, 0) : 0;
  }
  Table t(lists, f);
  int missing = 0;
  for (uint32_t seed = 0; seed < 600; ++seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    const Pick p = e->pick(settings(Mode::Locked, 55, true, true), t.in(0));
    TEST_ASSERT_EQUAL(0, p.musicRank);
    if (p.row > 40) {
      ++missing;
      TEST_ASSERT_TRUE(p.imputedTempo && p.imputedKey);
    }
  }
  TEST_ASSERT_INT_WITHIN(60, 200, missing);  // a third of the rows: about 200 of 600
}

void test_empty_lists() {
  // Row 0's slot is empty (c = 0), row 1's lists only row 0: a session of
  // them has no candidate: random picks, never silence.
  std::vector<Entries> lists = {{}, {{0, 230}}, {{0, 100}}, {{1, 90}}, {}};
  Table t(lists);
  auto e = engine();
  e->newSession(1, 0, &t.maps.m);
  e->trackStarted(1, &t.maps.m, false);
  const Pick p = e->pick(settings(), t.in(1));
  TEST_ASSERT_TRUE(p.ok && p.random && !p.noIndex);
  TEST_ASSERT_EQUAL(2, p.reads);
  TEST_ASSERT_NOT_EQUAL(0, p.row);
  TEST_ASSERT_NOT_EQUAL(1, p.row);
}

// ---------------------------------------------------------------------------
// Reads
// ---------------------------------------------------------------------------
void test_the_read_budget_and_prefetch() {
  Table t(star(12));
  const Maps* m = &t.maps.m;
  auto e = engine();
  e->newSession(1, 0, m);
  for (uint32_t tr : {1, 2, 3, 4}) e->trackStarted(tr, m, false);
  uint16_t row = 0;
  TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
  TEST_ASSERT_EQUAL(4, row);  // newest first
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Ok), static_cast<int>(e->prefetch(t.reader, 4)));
  TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
  TEST_ASSERT_EQUAL(3, row);
  const Pick p = e->pick(settings(), t.in(4));
  TEST_ASSERT_EQUAL(2, p.reads);  // 3 and 2; 1 and 0 left out
  TEST_ASSERT_EQUAL(2, e->stats().leftOut);
  TEST_ASSERT_EQUAL(3, t.reader.reads);
  TEST_ASSERT_EQUAL(1, e->stats().readsAtPick[2]);
  TEST_ASSERT_EQUAL(1, e->stats().prefetchReads);
  // A run of skips: one prefetch read a pass, never more than 2 at a pick.
  for (uint32_t tr = 5; tr < 11; ++tr) {
    e->trackStarted(tr, m, false);
    const int before = t.reader.reads;
    if (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row);
    TEST_ASSERT_TRUE(t.reader.reads - before <= 1);
    const Pick q = e->pick(settings(), t.in(tr));
    TEST_ASSERT_TRUE(q.reads <= kReadsPerPick);
  }
}

// The ring's rows and the pin are never evicted, even when one of them is
// the least recently used slot: the least recently used other slot is.
void test_the_cache_keeps_the_ring_and_the_pin() {
  Table t(full(20));
  const Maps* m = &t.maps.m;
  uint16_t row;
  // Follow the vibe, no pin: rows 1-4 in the ring and read first (the
  // oldest slots); 5 and 6 each started, read and quickly skipped (out of
  // the ring: the newest slots). The cache is full.
  auto e = engine();
  e->newSession(1);
  for (uint32_t tr = 1; tr <= 6; ++tr) {
    e->trackStarted(tr, m, false);
    TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
    TEST_ASSERT_EQUAL(tr, row);
    TEST_ASSERT_TRUE(e->prefetch(t.reader, row) == SlotResult::Ok);
    if (tr >= 5) e->trackLeft(tr, m, 5000, false);
  }
  TEST_ASSERT_EQUAL_HEX16(kNoRow, e->pin());
  TEST_ASSERT_EQUAL(4, e->anchors());
  for (uint16_t r = 1; r <= 6; ++r) TEST_ASSERT_TRUE(e->cached(r));
  // Track 7's read takes row 5's slot (the older of the two outside the
  // ring), not row 1's (the oldest of all, in the ring).
  e->trackStarted(7, m, false);
  TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
  TEST_ASSERT_EQUAL(7, row);
  e->prefetch(t.reader, row);
  TEST_ASSERT_FALSE(e->cached(5));
  TEST_ASSERT_TRUE(e->cached(6));
  for (uint8_t i = 0; i < e->anchors(); ++i) TEST_ASSERT_TRUE(e->cached(e->anchor(i)));
  TEST_ASSERT_FALSE(e->prefetchDue(settings(), &row));
  int reads = t.reader.reads;
  TEST_ASSERT_EQUAL(0, e->pick(settings(), t.in(7)).reads);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);
  // The pin outside the ring, the oldest slot of all: kept. The seed (row
  // 0) is read first; 1-6 start, 0 then 1 leave the ring; 6's read takes
  // row 1's slot.
  auto f = engine();
  f->newSession(2, 0, m);
  TEST_ASSERT_TRUE(f->prefetchDue(settings(Mode::Locked), &row));
  TEST_ASSERT_EQUAL(0, row);
  f->prefetch(t.reader, row);
  for (uint32_t tr = 1; tr <= 6; ++tr) {
    f->trackStarted(tr, m, false);
    TEST_ASSERT_TRUE(f->prefetchDue(settings(), &row));
    TEST_ASSERT_EQUAL(tr, row);
    f->prefetch(t.reader, row);
    TEST_ASSERT_FALSE(f->prefetchDue(settings(), &row));
  }
  TEST_ASSERT_EQUAL(0, f->pin());
  for (uint8_t i = 0; i < f->anchors(); ++i) TEST_ASSERT_NOT_EQUAL(0, f->anchor(i));  // out of the ring
  TEST_ASSERT_TRUE(f->cached(0));
  TEST_ASSERT_FALSE(f->cached(1));
  reads = t.reader.reads;
  TEST_ASSERT_EQUAL(0, f->pick(settings(Mode::Locked), t.in(6)).reads);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);
}

void test_damaged_failed_and_the_ring_of_8() {
  Table t(full(30));
  const Maps* m = &t.maps.m;
  t.reader.damaged = {3};
  t.reader.io = {2};
  t.reader.flaky = {1};
  auto e = engine();
  e->newSession(1, 0, m);
  for (uint32_t tr : {1, 2, 3}) e->trackStarted(tr, m, false);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Suspect), static_cast<int>(e->prefetch(t.reader, 3)));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Damaged), static_cast<int>(e->prefetch(t.reader, 3)));
  TEST_ASSERT_TRUE(e->rowDamaged(3));
  uint16_t row = 0;
  TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
  TEST_ASSERT_EQUAL(2, row);  // never re-read
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(e->prefetch(t.reader, 2)));
  TEST_ASSERT_FALSE(e->rowDamaged(2));  // Io: skipped, not in the ring for one
  TEST_ASSERT_EQUAL(1, e->damagedRows());
  Pick p = e->pick(settings(), t.in(3));
  TEST_ASSERT_EQUAL(2, p.reads);  // 2 (good now), then 1 (flaky: suspect)
  TEST_ASSERT_FALSE(p.random);
  p = e->pick(settings(), t.in(p.track));
  TEST_ASSERT_TRUE(e->cached(1));  // its second read is good
  TEST_ASSERT_EQUAL(1, e->stats().damaged);
  TEST_ASSERT_EQUAL(1, e->stats().ioSkips);
  // The ring holds the last 8.
  for (uint16_t r = 10; r < 19; ++r) {
    t.reader.damaged.insert(r);
    e->prefetch(t.reader, r);
    e->prefetch(t.reader, r);
  }
  TEST_ASSERT_EQUAL(8, e->damagedRows());
  TEST_ASSERT_FALSE(e->rowDamaged(3));
  TEST_ASSERT_FALSE(e->rowDamaged(10));
  TEST_ASSERT_TRUE(e->rowDamaged(18));
  e->fileReopened();  // cleared on reopen
  TEST_ASSERT_EQUAL(0, e->damagedRows());
  TEST_ASSERT_FALSE(e->cached(1));
}

void test_every_anchor_damaged_gives_a_random_pick() {
  Table t(full(12));
  t.reader.damaged = {0};
  auto e = engine();
  e->newSession(1, 0, &t.maps.m);
  const Pick p = e->pick(settings(Mode::Locked), t.in(0));
  TEST_ASSERT_TRUE(p.ok && p.random);
  TEST_ASSERT_EQUAL(2, p.reads);  // read, re-read: damaged
  TEST_ASSERT_TRUE(e->rowDamaged(0));
  TEST_ASSERT_NOT_EQUAL(0, p.row);
  // Stay on seed: the damaged pin gives way to that random pick's row, as a
  // seed without a row does; the session isn't random from then on.
  TEST_ASSERT_EQUAL(p.row, e->pin());
  const Pick q = e->pick(settings(Mode::Locked), t.in(p.track));
  TEST_ASSERT_EQUAL(1, q.reads);  // the new pin's; row 0, in the ring, never read again
  TEST_ASSERT_FALSE(q.random);
  TEST_ASSERT_EQUAL(2, t.reader.count[0]);
  // Follow the vibe keeps no pin of its own: its random pick anchors the next.
  auto f = engine();
  f->newSession(1, 0, &t.maps.m);
  const Pick r = f->pick(settings(), t.in(0));
  TEST_ASSERT_TRUE(r.random);
  TEST_ASSERT_EQUAL(0, f->pin());  // (only Stay on seed moves it)
  TEST_ASSERT_FALSE(f->pick(settings(), t.in(r.track)).random);
}

// A pick names the rows its reads put in the damaged ring (the console's
// "read at the pick, row R: DAMAGED" line), each once: a row suspect since
// a read ahead whose CRC fails again at the pick (1 read: the device check
// of 2026-10-15), a row read twice in one pick. Not a suspect the budget
// leaves, a row a read ahead damaged (its line is the read ahead's), a
// read that failed (Io), or a damaged row later (never read again).
void test_a_pick_names_the_rows_it_found_damaged() {
  Table t(full(30));
  const Maps* m = &t.maps.m;
  t.reader.damaged = {3, 4};
  auto e = engine();
  e->newSession(1, 0, m);
  for (uint32_t tr : {3, 4}) e->trackStarted(tr, m, false);  // the ring: 0, 3, 4
  uint16_t row = 0;
  TEST_ASSERT_TRUE(e->prefetchDue(settings(), &row));
  TEST_ASSERT_EQUAL(4, row);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Suspect), static_cast<int>(e->prefetch(t.reader, 4)));
  // Row 4 fails again (1 read: damaged, named), row 3 once (suspect: the
  // budget's second read), row 0 left out: a random pick.
  const Pick p = e->pick(settings(), t.in(4));
  TEST_ASSERT_TRUE(p.ok && p.random);
  TEST_ASSERT_EQUAL(2, p.reads);
  TEST_ASSERT_EQUAL(1, p.damaged);
  TEST_ASSERT_EQUAL(4, p.damagedRow[0]);
  TEST_ASSERT_TRUE(e->rowDamaged(4));
  TEST_ASSERT_FALSE(e->rowDamaged(3));
  // The random pick's row joined the ring (the ring: 0, 3, 4, its row),
  // so it anchors now, read first (1 read, ok); row 4 is skipped
  // (damaged); row 3, suspect since, fails again: named (the second
  // read); row 0 left out again.
  const Pick q = e->pick(settings(), t.in(p.track));
  TEST_ASSERT_FALSE(q.random);
  TEST_ASSERT_EQUAL(2, q.reads);
  TEST_ASSERT_EQUAL(1, q.damaged);
  TEST_ASSERT_EQUAL(3, q.damagedRow[0]);
  TEST_ASSERT_EQUAL(2, e->stats().damaged);
  TEST_ASSERT_EQUAL(1, t.reader.count[p.row]);
  TEST_ASSERT_EQUAL(0, t.reader.count[0]);
  // Named once: rows 3 and 4 aren't read again.
  const int reads3 = t.reader.count[3], reads4 = t.reader.count[4];
  const Pick r = e->pick(settings(), t.in(q.track));
  TEST_ASSERT_EQUAL(0, r.damaged);
  TEST_ASSERT_EQUAL(reads3, t.reader.count[3]);
  TEST_ASSERT_EQUAL(reads4, t.reader.count[4]);
  // Read twice in one pick: named.
  t.reader.damaged = {5};
  auto f = engine();
  f->newSession(1, 5, m);
  const Pick s = f->pick(settings(), t.in(5));
  TEST_ASSERT_TRUE(s.random);
  TEST_ASSERT_EQUAL(2, s.reads);
  TEST_ASSERT_EQUAL(1, s.damaged);
  TEST_ASSERT_EQUAL(5, s.damagedRow[0]);
  // Damaged by two reads ahead: the pick reads nothing and names nothing.
  t.reader.damaged = {6};
  auto g = engine();
  g->newSession(1, 6, m);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Suspect), static_cast<int>(g->prefetch(t.reader, 6)));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Damaged), static_cast<int>(g->prefetch(t.reader, 6)));
  const Pick u = g->pick(settings(), t.in(6));
  TEST_ASSERT_TRUE(u.random);
  TEST_ASSERT_EQUAL(0, u.reads);
  TEST_ASSERT_EQUAL(0, u.damaged);
  // A read that failed (the card pulled): skipped, not damaged for one.
  Table w(full(30));
  w.reader.io = {7};
  auto h = engine();
  h->newSession(1, 7, &w.maps.m);
  const Pick v = h->pick(settings(), w.in(7));
  TEST_ASSERT_EQUAL(1, v.reads);
  TEST_ASSERT_EQUAL(0, v.damaged);
  TEST_ASSERT_EQUAL(1, h->stats().ioSkips);
  TEST_ASSERT_FALSE(h->rowDamaged(7));
}

// A read the card keeps failing: the second in a row on the row puts it in the ring (each costs the loop
// the driver's retries, and a read ahead would ask every 2 s): the pin gives way, and a pick reads it no
// more. One read that works between starts over; a remount or Dc (fileReopened()) asks again.
void test_a_row_whose_read_keeps_failing() {
  Table t(full(30));
  const Maps* m = &t.maps.m;
  t.reader.ioAlways = {5};
  auto e = engine();
  e->newSession(1, 5, m);
  const Settings stay = settings(Mode::Locked);
  uint16_t row = 0;
  TEST_ASSERT_TRUE(e->prefetchDue(stay, &row));
  TEST_ASSERT_EQUAL(5, row);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(e->prefetch(t.reader, 5)));
  TEST_ASSERT_FALSE(e->rowDamaged(5));
  TEST_ASSERT_TRUE(e->prefetchDue(stay, &row));
  TEST_ASSERT_EQUAL(5, row);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::IoTwice), static_cast<int>(e->prefetch(t.reader, 5)));
  TEST_ASSERT_TRUE(e->rowDamaged(5));
  TEST_ASSERT_FALSE(e->prefetchDue(stay, &row));
  TEST_ASSERT_EQUAL(2, e->stats().ioSkips);
  TEST_ASSERT_EQUAL(1, e->stats().damaged);
  const int reads = t.reader.reads;
  const Pick p = e->pick(stay, t.in(5));
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);  // the pin gave way: nothing read of it
  // A remount: asked again, from the start.
  e->fileReopened();
  TEST_ASSERT_FALSE(e->rowDamaged(5));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(e->prefetch(t.reader, 5)));
  TEST_ASSERT_FALSE(e->rowDamaged(5));
  // One read that works between: the count starts over.
  t.reader.ioAlways = {};
  t.reader.io = {7};
  auto g = engine();
  g->newSession(1, 7, m);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(g->prefetch(t.reader, 7)));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Ok), static_cast<int>(g->prefetch(t.reader, 7)));
  TEST_ASSERT_FALSE(g->rowDamaged(7));
  // (prefetch() reads whatever the cache holds: no reopen needed to read it again.)
  t.reader.ioAlways = {7};
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(g->prefetch(t.reader, 7)));
  TEST_ASSERT_FALSE(g->rowDamaged(7));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::IoTwice), static_cast<int>(g->prefetch(t.reader, 7)));
  TEST_ASSERT_TRUE(g->rowDamaged(7));
}

void test_a_remount_between_two_failed_reads_starts_over() {
  Table t(full(30));
  t.reader.ioAlways = {7};
  auto h = engine();
  h->newSession(1, 7, &t.maps.m);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(h->prefetch(t.reader, 7)));
  h->fileReopened();
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(h->prefetch(t.reader, 7)));
  TEST_ASSERT_FALSE(h->rowDamaged(7));
}

void test_a_session_copy_carries_a_failed_read() {
  Table t(full(30));
  t.reader.ioAlways = {7};
  auto i = engine();
  i->newSession(1, 7, &t.maps.m);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(i->prefetch(t.reader, 7)));
  auto c = engine();
  c->copySession(*i);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::IoTwice), static_cast<int>(c->prefetch(t.reader, 7)));
  TEST_ASSERT_TRUE(c->rowDamaged(7));
  TEST_ASSERT_FALSE(i->rowDamaged(7));
}

void test_another_rows_good_read_leaves_a_failed_one() {
  Table t(full(30));
  t.reader.ioAlways = {7};
  auto j = engine();
  j->newSession(1, 7, &t.maps.m);
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Io), static_cast<int>(j->prefetch(t.reader, 7)));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::Ok), static_cast<int>(j->prefetch(t.reader, 8)));
  TEST_ASSERT_EQUAL(static_cast<int>(SlotResult::IoTwice), static_cast<int>(j->prefetch(t.reader, 7)));
  TEST_ASSERT_TRUE(j->rowDamaged(7));
}

// The row's second failure in a row inside a pick: the ring has it, the pick names it for the console's
// line, and the same pick goes on with the others.
void test_a_pick_names_a_row_whose_read_failed_twice() {
  Table t(full(30));
  t.reader.ioAlways = {5};
  auto e = engine();
  e->newSession(1, 5, &t.maps.m);
  const Settings stay = settings(Mode::Locked);
  Pick p = e->pick(stay, t.in(5));
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(1, p.reads);
  TEST_ASSERT_FALSE(e->rowDamaged(5));
  TEST_ASSERT_EQUAL(0, p.damaged);
  p = e->pick(stay, t.in(5));
  TEST_ASSERT_TRUE(p.ok);
  TEST_ASSERT_EQUAL(1, p.reads);
  TEST_ASSERT_TRUE(e->rowDamaged(5));
  TEST_ASSERT_EQUAL(1, p.damaged);
  TEST_ASSERT_EQUAL(5, p.damagedRow[0]);
}

void test_a_broken_slot() {
  Table t(full(12));
  t.reader.broken = {5};
  auto e = engine();
  e->newSession(1, 5, &t.maps.m);
  Pick p = e->pick(settings(), t.in(5));
  TEST_ASSERT_TRUE(p.ok && p.broken && p.random);
  TEST_ASSERT_TRUE(e->broken());
  uint16_t row;
  TEST_ASSERT_FALSE(e->prefetchDue(settings(), &row));
  const int reads = t.reader.reads;
  p = e->pick(settings(), t.in(p.track));
  TEST_ASSERT_TRUE(p.broken && p.random);
  TEST_ASSERT_EQUAL(reads, t.reader.reads);  // absent: no more reads
  e->fileReopened();
  t.reader.broken.clear();
  p = e->pick(settings(), t.in(p.track));
  TEST_ASSERT_FALSE(p.broken || p.random);
}

// The hardening files, read through mpdj::Table: a slot whose CRC fails
// (the device's view of mpdj-slot-crc) and a slot whose levels rise.
void test_damaged_and_broken_in_real_files() {
  struct File {
    std::string bytes;
    std::unique_ptr<cc::MemSource> src;
    mpdj::Table table;
  };
  auto open = [](const char* name, File* f) {
    f->bytes = cardfixtures::bytes(std::string("hardening/") + name);
    TEST_ASSERT_FALSE_MESSAGE(f->bytes.empty(), name);
    f->src.reset(new cc::MemSource(f->bytes.data(), static_cast<uint32_t>(f->bytes.size())));
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f->table.open(*f->src)));
  };
  // Slot 2's CRC fails: damaged; the others still anchor.
  File d;
  open("mpdj-uses-device-slot-crc.bin", &d);
  Table t(std::vector<Entries>(d.table.rowCount()));
  t.reader.table = &d.table;
  t.k = d.table.k();
  t.selFloor = static_cast<uint8_t>(d.table.selFloor());
  auto e = engine();
  e->newSession(1, 2, &t.maps.m);
  Pick p = e->pick(settings(Mode::Locked, 55), t.in(2));
  TEST_ASSERT_TRUE(e->rowDamaged(2));
  TEST_ASSERT_TRUE(p.random && !p.broken);
  e->newSession(1, 0, &t.maps.m);
  e->trackStarted(2, &t.maps.m, false);  // in the ring, but damaged: skipped
  p = e->pick(settings(Mode::Rolling, 55), t.in(2));
  TEST_ASSERT_FALSE(p.random);
  TEST_ASSERT_EQUAL(1, p.reads);
  // Slot 0's levels rise under a good CRC: broken, the file absent.
  File b;
  open("mpdj-slot-level-rises.bin", &b);
  Table u(std::vector<Entries>(b.table.rowCount()));
  u.reader.table = &b.table;
  u.k = b.table.k();
  auto f = engine();
  f->newSession(1, 0, &u.maps.m);
  p = f->pick(settings(), u.in(0));
  TEST_ASSERT_TRUE(p.broken && p.random && p.ok);
  TEST_ASSERT_TRUE(f->broken());
}

// ---------------------------------------------------------------------------
// No index, the button, determinism, memory
// ---------------------------------------------------------------------------
void test_no_index_by_track_id() {
  auto e = engine();
  e->newSession(3, 2);
  PickIn in;
  in.playingTrack = 2;
  in.libraryTracks = 40;
  std::set<uint32_t> got;
  for (int i = 0; i < 9; ++i) {
    const Pick p = e->pick(settings(), in);
    TEST_ASSERT_TRUE(p.ok && p.noIndex && p.random);
    TEST_ASSERT_EQUAL_HEX16(kNoRow, p.row);
    TEST_ASSERT_NOT_EQUAL(2, p.track);
    got.insert(p.track);
  }
  TEST_ASSERT_EQUAL(9, got.size());  // the cooldown by id
  TEST_ASSERT_TRUE(e->idCooled(*got.begin()));
  e->tracksRenumbered();
  TEST_ASSERT_EQUAL(0, e->cooledIds());
  in.playingTrack = 0;
  in.libraryTracks = 1;
  const Pick none = e->pick(settings(), in);  // the only track is the playing one
  TEST_ASSERT_FALSE(none.ok);
  TEST_ASSERT_EQUAL(1, e->stats().failed);
  in.libraryTracks = 0;
  TEST_ASSERT_FALSE(e->pick(settings(), in).ok);
}

void test_surprise() {
  Table t(star(6), {}, {0, 2, 4});
  auto e = engine();
  e->newSession(5);
  for (int i = 0; i < 20; ++i) {
    const Pick p = e->surprise(settings(), t.in(kNoTrackId));
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_TRUE(p.row == 0 || p.row == 2 || p.row == 4);
    TEST_ASSERT_EQUAL(p.row, t.maps.t2r[p.track]);
  }
  TEST_ASSERT_FALSE(e->seeded());  // the session untouched
  TEST_ASSERT_FALSE(e->surprise(settings(), PickIn()).ok);
}

// Under Happy the button's row is inside the mood, from the scan with no
// exclusions (MOOD-FILTERS.md 6.3): the session's rows and the playing one's
// too. Row 2 is tested by its first track, sad, though track 9 shares it and
// is happy; track 8, with no row, never comes. Nothing inside: no track. A
// level alone, or no words: the draws of no filter, from the same stream.
void test_the_button_inside_the_filter() {
  Table t(star(8), {}, {0, 1, 2, 3, 4, 5, 6, 7, kNoRow, 2});
  const std::vector<uint16_t> words = {kHappy, kSad, kSad, kHappy, kSad, kHappy, kHappy, kSad, kHappy, kHappy};
  t.maps.m.mood = MoodView(words.data(), 10, kHappyQ);
  auto e = engine();
  e->newSession(6, 0, &t.maps.m);
  std::set<uint16_t> rows;
  for (int i = 0; i < 60; ++i) {
    const Pick p = e->surprise(settings(), t.in(0));
    TEST_ASSERT_TRUE(p.ok && p.random);
    TEST_ASSERT_EQUAL(4, p.candidates);
    TEST_ASSERT_EQUAL(p.row, t.maps.t2r[p.track]);
    rows.insert(p.row);
  }
  TEST_ASSERT_TRUE(rows == std::set<uint16_t>({0, 3, 5, 6}));
  TEST_ASSERT_EQUAL(1, e->anchors());  // the session untouched
  TEST_ASSERT_EQUAL(1, e->cooledRows());
  TEST_ASSERT_EQUAL(0, e->stats().picks);
  moodfilter::Filter none;
  none.valence = moodfilter::Scale::First;
  none.energy = moodfilter::Scale::Second;
  none.voice = moodfilter::Voice::Instrumental;
  t.maps.m.mood = MoodView(words.data(), 10, none.pack());
  Pick p = e->surprise(settings(), t.in(0));
  TEST_ASSERT_FALSE(p.ok);
  TEST_ASSERT_EQUAL(0, p.candidates);
  TEST_ASSERT_EQUAL_UINT32(kNoTrackId, p.track);
  // Off: today's draws (kSurpriseDraws at most), the stream where it was.
  const uint16_t loose = qOf(moodfilter::Scale::Any, moodfilter::Level::Loose);
  std::vector<std::vector<uint16_t>> got;
  for (const MoodView& mood : {MoodView(), MoodView(words.data(), 10, loose), MoodView(nullptr, 0, kHappyQ)}) {
    t.maps.m.mood = mood;
    auto f = engine();
    f->newSession(6);
    std::vector<uint16_t> r;
    for (int i = 0; i < 20; ++i) r.push_back(f->surprise(settings(), t.in(0)).row);
    r.push_back(f->pick(settings(), t.in(0)).row);
    got.push_back(r);
  }
  TEST_ASSERT_TRUE(got[1] == got[0] && got[2] == got[0]);
  // No scratch (a failed begin()): none inside the mood, today's without one.
  gFail = true;
  Engine bare;
  TEST_ASSERT_FALSE(bare.begin(countAlloc, countFree));
  gFail = false;
  bare.newSession(6);
  t.maps.m.mood = MoodView(words.data(), 10, kHappyQ);
  TEST_ASSERT_FALSE(bare.surprise(settings(), t.in(0)).ok);
  t.maps.m.mood = MoodView();
  TEST_ASSERT_TRUE(bare.surprise(settings(), t.in(0)).ok);
}

void test_the_same_seed_gives_the_same_session() {
  std::vector<uint32_t> f;
  for (int r = 0; r < 40; ++r) f.push_back(filterWord(static_cast<uint16_t>(900 + 13 * r), (r * 7) % 25, 0, r % 6));
  Table t(full(40), f);
  auto run = [&](uint32_t seed) {
    auto e = engine();
    e->newSession(seed, 0, &t.maps.m);
    std::vector<uint16_t> rows;
    uint32_t playing = 0;
    for (int i = 0; i < 30; ++i) {
      const Pick p = e->pick(settings(i % 2 ? Mode::Locked : Mode::Rolling, 55 + 5 * (i % 5), true, true),
                             t.in(playing));
      rows.push_back(p.row);
      playing = p.track;
    }
    return rows;
  };
  TEST_ASSERT_TRUE(run(77) == run(77));
  TEST_ASSERT_FALSE(run(77) == run(78));
}

void test_no_allocation_in_pick() {
  Table t(full(60));
  auto e = engine();
  const long allocs = gAllocs;
  e->newSession(1, 0, &t.maps.m);
  uint32_t playing = 0;
  uint16_t row;
  for (int i = 0; i < 100; ++i) {
    if (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row);
    const Pick p = e->pick(settings(Mode::Rolling, 55, true, true), t.in(playing));
    playing = p.track;
    e->trackStarted(playing, &t.maps.m, true);
    e->surprise(settings(), t.in(playing));
  }
  TEST_ASSERT_EQUAL(allocs, gAllocs);
  TEST_ASSERT_TRUE(e->heldBytes() > 0 && e->heldBytes() < 20 * 1024);
  const long frees = gFrees;
  e.reset();
  TEST_ASSERT_EQUAL(frees + 1, gFrees);  // begin()'s one block
}

// The mood's ladder allocates nothing: jumps, the scan, a track with no
// row, repeats, picks by id and nothing that fits (6.3).
void test_no_allocation_in_a_jump() {
  std::vector<uint16_t> tracks;
  for (uint16_t r = 0; r < 40; ++r) tracks.push_back(r);
  for (int i = 0; i < 5; ++i) tracks.push_back(kNoRow);
  Table t(full(40), {}, tracks);
  std::vector<uint16_t> words(45, kSad);
  for (int tr = 0; tr < 40; tr += 5) words[tr] = kHappy;
  words[42] = kHappy;
  t.maps.m.mood = MoodView(words.data(), 45, kHappyQ);
  auto e = engine();
  const long allocs = gAllocs;
  e->newSession(1, 0, &t.maps.m);
  uint32_t playing = 0;
  uint16_t row;
  bool noRow = false;
  for (int i = 0; i < 60; ++i) {
    if (e->prefetchDue(settings(), &row)) e->prefetch(t.reader, row, &t.maps.m, static_cast<uint16_t>(t.k));
    const Pick p = e->pick(settings(Mode::Rolling, 55, true, true), t.in(playing));
    TEST_ASSERT_TRUE(p.ok && words[p.track] == kHappy);
    noRow = noRow || p.jump == Jump::NoRow;
    playing = p.track;
    e->trackStarted(playing, &t.maps.m, true);
  }
  PickIn byId = t.in(playing);
  byId.maps = nullptr;
  byId.slots = nullptr;
  for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(e->pick(settings(), byId).ok);
  moodfilter::Filter none;
  none.valence = moodfilter::Scale::First;
  none.voice = moodfilter::Voice::Instrumental;
  t.maps.m.mood = MoodView(words.data(), 45, none.pack());
  TEST_ASSERT_TRUE(e->pick(settings(), t.in(playing)).noFit);
  TEST_ASSERT_EQUAL(allocs, gAllocs);
  TEST_ASSERT_TRUE(noRow);
  TEST_ASSERT_TRUE(e->stats().jumps > 0 && e->stats().moodRepeats > 0);
  TEST_ASSERT_EQUAL(1, e->stats().noFit);
}

void test_a_failed_begin_picks_by_id() {
  gFail = true;
  Engine e;
  TEST_ASSERT_FALSE(e.begin(countAlloc, countFree));
  gFail = false;
  TEST_ASSERT_EQUAL(0, e.heldBytes());
  Table t(full(10));
  e.newSession(1, 0, &t.maps.m);
  const Pick p = e.pick(settings(), t.in(0));
  TEST_ASSERT_TRUE(p.ok && p.noIndex);
  uint16_t row;
  TEST_ASSERT_FALSE(e.prefetchDue(settings(), &row));
}

void test_copy_session_for_dry_runs() {
  Table t(full(40));
  auto live = engine();
  live->newSession(11, 0, &t.maps.m);
  uint32_t playing = 0;
  for (int i = 0; i < 5; ++i) playing = live->pick(settings(), t.in(playing)).track;
  auto dry = engine();
  dry->copySession(*live);
  auto again = engine();
  again->copySession(*live);
  const int reads = t.reader.reads;
  for (int i = 0; i < 10; ++i) {
    const Pick a = dry->pick(settings(), t.in(playing));
    const Pick b = again->pick(settings(), t.in(playing));
    TEST_ASSERT_EQUAL(a.row, b.row);
    playing = a.track;
  }
  TEST_ASSERT_TRUE(t.reader.reads > reads);
  // The live session is where it was: its next pick is the copies' first.
  auto third = engine();
  third->copySession(*live);
  TEST_ASSERT_EQUAL(third->pick(settings(), t.in(0)).row, live->pick(settings(), t.in(0)).row);
  TEST_ASSERT_EQUAL(5 + 1, live->stats().picks);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sessions_match_the_reference);
  RUN_TEST(test_the_defaults);
  RUN_TEST(test_the_gate_at_every_step_and_the_clamp);
  RUN_TEST(test_dry_takes_the_50_best_sums);
  RUN_TEST(test_dry_ties_go_to_the_lower_rows);
  RUN_TEST(test_the_ring_d1_and_the_quick_skip);
  RUN_TEST(test_a_pick_by_id_steers_once_the_index_is_there);
  RUN_TEST(test_tracks_without_rows_and_a_seed_without_a_row);
  RUN_TEST(test_the_pin_and_anchor_changes);
  RUN_TEST(test_the_seed_from_the_first_track);
  RUN_TEST(test_the_key_anchor_is_lazy);
  RUN_TEST(test_the_cooldowns);
  RUN_TEST(test_every_candidate_cooled_gives_repeats);
  RUN_TEST(test_loosen_before_repeating_the_device_case);
  RUN_TEST(test_loosen_in_follow_the_vibe);
  RUN_TEST(test_dry_and_loosened);
  RUN_TEST(test_runs_follow_the_maps);
  RUN_TEST(test_a_filter_keeps_the_candidates_inside_the_mood);
  RUN_TEST(test_a_level_alone_is_no_filter);
  RUN_TEST(test_a_filter_changed_remakes_the_runs);
  RUN_TEST(test_a_shared_row_is_tested_by_its_first_track);
  RUN_TEST(test_a_jump_keeps_the_classes);
  RUN_TEST(test_the_scan_keeps_64);
  RUN_TEST(test_repeats_oldest_first);
  RUN_TEST(test_a_track_with_no_row_before_repeats);
  RUN_TEST(test_the_only_track_repeats);
  RUN_TEST(test_the_repin_stays_in_the_budget);
  RUN_TEST(test_a_moved_pin_goes_at_a_quick_skip);
  RUN_TEST(test_a_seed_pin_stays);
  RUN_TEST(test_a_quick_skip_says_whether_it_steered);
  RUN_TEST(test_cooldown_before_music);
  RUN_TEST(test_no_bpm_or_key_anywhere);
  RUN_TEST(test_imputation_gives_missing_values_their_share);
  RUN_TEST(test_empty_lists);
  RUN_TEST(test_the_read_budget_and_prefetch);
  RUN_TEST(test_the_cache_keeps_the_ring_and_the_pin);
  RUN_TEST(test_damaged_failed_and_the_ring_of_8);
  RUN_TEST(test_every_anchor_damaged_gives_a_random_pick);
  RUN_TEST(test_a_pick_names_the_rows_it_found_damaged);
  RUN_TEST(test_a_row_whose_read_keeps_failing);
  RUN_TEST(test_a_remount_between_two_failed_reads_starts_over);
  RUN_TEST(test_a_session_copy_carries_a_failed_read);
  RUN_TEST(test_another_rows_good_read_leaves_a_failed_one);
  RUN_TEST(test_a_pick_names_a_row_whose_read_failed_twice);
  RUN_TEST(test_a_broken_slot);
  RUN_TEST(test_damaged_and_broken_in_real_files);
  RUN_TEST(test_no_index_by_track_id);
  RUN_TEST(test_surprise);
  RUN_TEST(test_the_button_inside_the_filter);
  RUN_TEST(test_the_same_seed_gives_the_same_session);
  RUN_TEST(test_no_allocation_in_pick);
  RUN_TEST(test_no_allocation_in_a_jump);
  RUN_TEST(test_a_failed_begin_picks_by_id);
  RUN_TEST(test_copy_session_for_dry_runs);
  return UNITY_END();
}
