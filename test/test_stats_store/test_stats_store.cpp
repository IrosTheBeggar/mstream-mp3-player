// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' store as a whole: the proof of
// docs/LISTENING-STATS.md 9.5's C8. The play log's Journal (lib/core PlayLog)
// with the real fold (StatsFold) and files (StatsFile), joined by the
// firmware's own glue (StatsGlue, which app/ListeningStats wraps: C9's read
// before the journal's begin(), the save's guards, the end's flush, the live
// fold at the Outbox's written(), Hs), over a card in memory that can lose
// its power at any step (test/support/CutFs.h) or fail chosen calls
// (test/support/FaultFs.h).
// Each scenario runs the fold, the save, the trim and the reboot, and is
// compared with tools/statsref.py's: the boots of test/fixtures/card/stats
// (index.json) and its sessions (sessions.json: boots, plays, the end's
// flush, Hs, two players, a 0.14.0 boot, NVS put back), file for file, byte
// for byte. No play lost, none counted twice; the undercounts 5.5 names, to
// the slot.
// - every boot of every fixture through the journal, chained boot to boot;
//   a remount mid-scan, and one after Ready before the first slot;
// - a cut at every write step of the boot's save, its compaction and the
//   trim, three ways, then a boot; a question or a step the card fails,
//   each in turn; a held tmp promoted by the next save, and one a reboot's
//   settle() drops while stats.bin is there;
// - the base faulted at the boot (its open, its header's read): stats off,
//   nothing lost, nothing removed;
// - the partial scans (statsref's partial-*), then Hs refused;
// - the sessions: a second player saving (and compacting) between two saves
//   of the first; a rotation over an unsaved plays.1.jnl, the save failing,
//   then a reboot (an undercount, exactly), also over a twin's chain; a
//   rotation in the end's flush with 1 s of its budget left and with less,
//   and one after a partial scan; NVS put back after a trim, and after the
//   log deleted by hand (MARK alone holding the seq); two players
//   alternating; a 0.14.0 boot's trim between (HEAD's gap); a new epoch
//   with no ACKS; firstAt through a rotation's save;
// - firstAt's orders, the retention (the clock unset, a foreign 2099, a
//   clock a year ahead then corrected);
// - a full log never synced (13,106 slots): TRKD's rows the distinct
//   hashes; the peak bytes and the microseconds a slot, printed.
// Run: pio test -e native -f test_stats_store
#include <unity.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "../support/FaultFs.h"
#include "CardContainer.h"
#include "CardManifest.h"
#include "PlayLog.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsGlue.h"

namespace cc = cardcontract;
namespace pl = playlog;
namespace sf = statsfold;
namespace sx = statsfile;
using cutfs::CutFs;
using faultfs::FaultFs;
using faultfs::FlakyFs;
using pl::Journal;
using pl::Slot;

void setUp() {}
void tearDown() {}

namespace {

minijson::Value gIndex;     // stats/index.json: the boots
minijson::Value gSessions;  // stats/sessions.json: the sessions
using Bytes = std::string;
using Files = std::map<std::string, Bytes>;
const std::string kDir = "/.player/";
const char* const kLog = pl::kLogPath;
const char* const kPrev = pl::kPrevPath;

// The caller's memory, aligned as malloc's (PSRAM on the device).
struct Work {
  std::unique_ptr<uint64_t[]> m{new uint64_t[(sx::kWorkBytes + 7) / 8]};
  uint8_t* p() { return reinterpret_cast<uint8_t*>(m.get()); }
};

// The boot's waits, as the hook got them.
std::vector<uint32_t> gWaits;
void waitHook(uint32_t ms) { gWaits.push_back(ms); }

// A heap that counts the Fold's blocks (the full log's peak).
struct Heap {
  std::map<void*, size_t> blocks;
  size_t held = 0, peak = 0;
  uint32_t carves = 0;
};
Heap gHeap;
void* heapAlloc(size_t n) {
  void* p = std::malloc(n);
  if (!p) return nullptr;
  ++gHeap.carves;
  gHeap.blocks[p] = n;
  gHeap.held += n;
  gHeap.peak = std::max(gHeap.peak, gHeap.held);
  return p;
}
void heapFree(void* p) {
  auto it = gHeap.blocks.find(p);
  if (it != gHeap.blocks.end()) {
    gHeap.held -= it->second;
    gHeap.blocks.erase(it);
  }
  std::free(p);
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string hex(const Bytes& a, const Bytes& b) {
  size_t i = 0;
  while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
  char m[96];
  std::snprintf(m, sizeof(m), "%zu and %zu bytes, first difference at %zu", a.size(), b.size(), i);
  return m;
}

// ---------------------------------------------------------------------------
// The card's /.player, and the fixtures'.
// ---------------------------------------------------------------------------
void put(CutFs& card, const Files& files) {
  for (const auto& f : files) card.put(kDir + f.first, std::vector<uint8_t>(f.second.begin(), f.second.end()));
}

Bytes bytesOf(const CutFs& card, const std::string& name) {
  const std::vector<uint8_t> v = card.bytes(kDir + name);
  return Bytes(v.begin(), v.end());
}

Files filesOf(const CutFs& card) {
  Files out;
  for (const std::string& n : card.names())
    if (n.compare(0, kDir.size(), kDir) == 0) out[n.substr(kDir.size())] = bytesOf(card, n.substr(kDir.size()));
  return out;
}

// A fixture's files: each name mapped to the folder under stats/ that holds it.
Files cardOf(const minijson::Value& where) {
  Files out;
  for (const auto& m : where.members) out[m.first] = cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first);
  return out;
}

const minijson::Value& scenario(const char* name) {
  for (size_t i = 0; i < gIndex.size(); ++i)
    if (gIndex[i]["name"].str() == name) return gIndex[i];
  TEST_FAIL_MESSAGE(name);
  return gIndex[0];
}

void sameFiles(const Files& want, const Files& got, const std::string& what) {
  std::string names;
  for (const auto& f : want)
    if (!got.count(f.first)) names += " -" + f.first;
  for (const auto& f : got)
    if (!want.count(f.first)) names += " +" + f.first;
  TEST_ASSERT_TRUE_MESSAGE(names.empty(), (what + ": the card's names differ:" + names).c_str());
  for (const auto& f : want) {
    const Bytes& g = got.at(f.first);
    TEST_ASSERT_TRUE_MESSAGE(g == f.second, (what + ": " + f.first + ", " + hex(g, f.second)).c_str());
  }
}

void deviceOf(const std::string& hexId, uint8_t out[6]) {
  TEST_ASSERT_EQUAL_size_t(12, hexId.size());
  for (int k = 0; k < 6; ++k) out[k] = static_cast<uint8_t>(std::strtoul(hexId.substr(2 * k, 2).c_str(), nullptr, 16));
}

cc::msmf::Acks acksOf(const minijson::Value& v) {
  cc::msmf::Acks a;
  a.present = v.size() > 0;
  for (size_t i = 0; i < v.size() && a.count < cc::msmf::Acks::kMax; ++i) {
    cc::msmf::Ack& e = a.entries[a.count++];
    deviceOf(v[i][0].str(), e.deviceId);
    e.epoch = v[i][1].u64();
    e.seq = static_cast<uint32_t>(v[i][2].u64());
  }
  a.rows = a.count;
  return a;
}

const char* const kLogNames[sf::kFiles] = {"plays.jnl",   "plays.1.jnl", "plays.2.jnl", "plays.3.jnl", "plays.4.jnl",
                                           "plays.5.jnl", "plays.6.jnl", "plays.7.jnl", "plays.8.jnl", "plays.9.jnl"};

// The twins a cut rename of stats.tmp leaves, parted (a disk check) and
// collected, as the next settle() does (2.12.6).
void partTwins(CutFs& card) {
  card.diskCheck();
  tagstore::collectTwins(card, sx::kStatsNames);
}

// ---------------------------------------------------------------------------
// NVS, the epochs a fresh NVS draws.
// ---------------------------------------------------------------------------
struct MemNvs : Journal::Nvs {
  Journal::Ids ids;  // epoch 0: none (erased)
  bool load(Journal::Ids* out) override {
    *out = ids;
    return true;
  }
  bool storeSeq(uint32_t s) override {
    ids.seq = s;
    return true;
  }
  bool storeBoots(uint32_t b) override {
    ids.boots = b;
    return true;
  }
  bool storeEpoch(const Journal::Ids& i) override {
    ids = i;
    return true;
  }
};

struct Epochs {
  std::vector<uint64_t> list;
  size_t at = 0;
};
uint64_t nextEpoch(void* ctx) {
  auto* e = static_cast<Epochs*>(ctx);
  return e->at < e->list.size() ? e->list[e->at++] : 0x0123456789ABCDEFull + e->at++;
}

std::string base(const char* p) {
  if (!p) return "";
  const char* s = std::strrchr(p, '/');
  return s ? s + 1 : p;
}

void onOp(void* ctx, Journal::Op o, const char* a, const char* b, uint64_t n) {
  auto* v = static_cast<std::vector<std::string>*>(ctx);
  char t[96];
  switch (o) {
    case Journal::Op::Delete: std::snprintf(t, sizeof(t), "delete %s %s", base(a).c_str(), b); break;
    case Journal::Op::Twin: std::snprintf(t, sizeof(t), "twin %s %s", base(a).c_str(), base(b).c_str()); break;
    case Journal::Op::Rename: std::snprintf(t, sizeof(t), "rename %s %s", base(a).c_str(), base(b).c_str()); break;
    case Journal::Op::Play: std::snprintf(t, sizeof(t), "play %llu", static_cast<unsigned long long>(n)); break;
    case Journal::Op::Note: std::snprintf(t, sizeof(t), "note %llu", static_cast<unsigned long long>(n)); break;
    default: std::snprintf(t, sizeof(t), "op %d", static_cast<int>(o)); break;
  }
  v->push_back(t);
}

// ---------------------------------------------------------------------------
// The glue: lib/core StatsGlue, the firmware's own (app/ListeningStats wraps
// it: docs/LISTENING-STATS.md 5.4-5.7, 9.5 C9), with a test's Env around it:
// the calls it made, in order; the log's files on the card at each save the
// store was asked for; a full card at a save; the board's clocks.
// ---------------------------------------------------------------------------
struct Glue final : statsglue::Env {
  Glue(tagstore::Fs& fs, CutFs& card, sf::Fold::AllocFn a, sf::Fold::FreeFn f)
      : core(fs, *this, a, f),
        fold(core.fold()),
        store(core.store()),
        first(core.first()),
        opened(core.opened()),
        last(core.last()),
        card_(card) {}
  statsglue::Glue core;
  sf::Fold& fold;
  sx::Store& store;
  const sx::Opened& first;   // the boot's read
  const sx::Opened& opened;  // the last read (a remount's)
  const sx::SaveReport& last;
  cc::msmf::Acks acks;      // the root's
  sx::SaveAsk ask;          // the board's clock, the root's commitTime, the update step's safe point
  bool saveFails = false;   // a full card at a save: each of its steps refused
  bool away = false;        // the card out of the slot, refusing writes, or another card in it
  std::function<uint64_t()> mono;    // the board's monotonic clock
  std::function<void()> beforeSave;  // (a test's: a cut's set-up)
  std::vector<std::string> calls;
  std::vector<std::string> seen;  // the log's files on the card at each boot's or rotation's save the store was asked for

  // PlayHistory::begin(): the read, before the journal's begin().
  sx::Opened read(sx::Store::WaitFn wait) {
    const sx::Opened& o = core.begin(acks, wait);
    TEST_ASSERT_FALSE(o.read == sx::Read::NoMemory);
    return o;
  }
  void reserve(uint32_t slots) { core.reserve(slots); }
  // The journal ready: a boot's fold that asked no save (a partial scan)
  // ends here.
  void ready() { core.ready(); }
  // Outbox::Env::written(): the live fold.
  void written(const Slot& s) { core.written(s); }
  // Hs: the rotation's path (never a compaction).
  bool hs() { return core.hs(false); }
  // The orderly end's flush (app/PlayHistory: the ending flag, the 3 s
  // budget): the boot's save refused; a rotation's with 1 s of it left.
  void ending(uint64_t until) { core.ending(until); }
  void notEnding() { core.notEnding(); }

  // statsglue::Env
  uint64_t monoMs() override { return mono ? mono() : 0; }
  bool cardAway() override { return away; }
  void asking(statsglue::Kind k, sx::SaveAsk* a) override {
    *a = ask;
    if (k != statsglue::Kind::Hs) {
      std::string names;
      for (const char* n : {kPrev, kLog})
        if (card_.dir.count(n)) names += (names.empty() ? "" : " ") + base(n);
      seen.push_back(names);
      if (beforeSave) beforeSave();
    }
    if (saveFails) card_.refuse = true;
  }
  void began(bool again, const sx::Opened*) override { calls.push_back(again ? "began again" : "began"); }
  void partialFile() override { calls.push_back("partial"); }
  void saved(statsglue::Kind k, statsglue::Refused r, const sx::SaveReport& rep) override {
    card_.refuse = false;
    const std::string name = k == statsglue::Kind::Boot       ? "save boot"
                             : k == statsglue::Kind::Rotation ? "save rotation"
                             : k == statsglue::Kind::GiveBack ? "save give-back"
                                                              : "hs";
    calls.push_back(r == statsglue::Refused::None && sx::savedOk(rep.what) ? name : name + " (no)");
  }

private:
  CutFs& card_;
};

// ---------------------------------------------------------------------------
// A player: its NVS, the journal, the glue, the Tracker and the Outbox, over
// a card (`fs`: CutFs, or a card that fails calls over it).
// ---------------------------------------------------------------------------
struct TEnv : pl::Tracker::Env {
  uint64_t now = 0;
  pl::PlayFile next;
  uint64_t monoMs() override { return now; }
  bool file(uint32_t, uint32_t, pl::PlayFile* out) override {
    *out = next;
    return true;
  }
};

struct OEnv : pl::Outbox::Env {
  uint64_t now = 1000000;
  uint64_t perStat = 0;  // ms the played file's question takes
  uint64_t at = 0;       // the next play's startedAt (0: the clock unset)
  uint32_t size = 0, fatTime = 0;
  Glue* glue = nullptr;
  File stat(const char*, uint32_t* s, uint32_t* t) override {
    now += perStat;
    *s = size;
    *t = fatTime;
    return File::There;
  }
  uint64_t startedAt(uint64_t, uint8_t* clock) override {
    *clock = pl::kClockRtc;
    return at;
  }
  uint64_t monoMs() override { return now; }
  void tried(const pl::Outbox::Tried&, const pl::Played&) override {}
  void written(const Slot& s, const pl::Played&) override {
    if (glue) glue->written(s);
  }
};

// A play's own fields (the journal fills seq, epoch, deviceId, bootSeq).
struct PlaySpec {
  uint64_t hash = 0;
  uint32_t played = 0, duration = 0;
  uint8_t outcome = pl::kCompleted, source = pl::kSourceManual;
  uint64_t at = 0;  // startedAt (0: undated)
  uint32_t up = 0;  // the board's monotonic ms at its start (uptimeMs)
  uint32_t size = 0, fatTime = 0, generation = 0;
};

// What a boot is given.
struct BootAsk {
  cc::msmf::Acks acks;
  sx::SaveAsk ask;
  bool saveFails = false;
};

struct Player {
  Player(CutFs& c, const uint8_t dev[6], uint32_t capSlots = pl::kFileSlots) : card(c), fs(&c), cap(capSlots) {
    std::memcpy(id, dev, 6);
  }
  CutFs& card;
  tagstore::Fs* fs;
  uint8_t id[6];
  uint32_t cap;
  MemNvs nvs;
  Epochs epochs;
  std::vector<std::string> ops;
  std::vector<uint8_t> buf = std::vector<uint8_t>(pl::kSlot);  // a slot a read: a read fault ends a file at its slot
  uint32_t piece = 5;
  bool stats = true;  // false: a 0.14.0 boot (no folds)
  sf::Fold::AllocFn alloc = nullptr;
  sf::Fold::FreeFn release = nullptr;
  std::function<void()> afterRead;  // (a test's: between the stats' read and the journal's begin())
  std::unique_ptr<Glue> glue;
  std::unique_ptr<Journal> j;
  TEnv tenv;
  OEnv oenv;
  std::unique_ptr<pl::Tracker> tracker;
  std::unique_ptr<pl::Outbox> out;

  Journal::Config config() {
    Journal::Config c;
    c.capSlots = cap;
    c.buf = buf.data();
    c.bufBytes = static_cast<uint32_t>(buf.size());
    c.random = nextEpoch;
    c.randomCtx = &epochs;
    c.onOp = onOp;
    c.opCtx = &ops;
    c.folds = glue ? &glue->core : nullptr;
    return c;
  }
  // A boot's start (a new session): the stats read, the journal's begin().
  void begin(const BootAsk& b, sx::Store::WaitFn wait = waitHook) {
    out.reset();
    tracker.reset();
    j.reset();
    glue.reset(stats ? new Glue(*fs, card, alloc, release) : nullptr);
    if (glue) {
      glue->acks = b.acks;
      glue->ask = b.ask;
      glue->saveFails = b.saveFails;
      glue->mono = [this] { return oenv.now; };
      glue->read(wait);
    }
    if (afterRead) afterRead();
    ops.clear();
    j.reset(new Journal(*fs, nvs, config()));
    TEST_ASSERT_TRUE(j->begin(id, b.acks));
    tracker.reset(new pl::Tracker(tenv));
    oenv.glue = glue.get();
    out.reset(new pl::Outbox(*tracker, *j, oenv));
  }
  // The scan to its end, a piece a call, the fold's room taken before each.
  bool scan() {
    for (int guard = 0; guard < 1000000; ++guard) {
      if (glue) glue->reserve(piece);
      if (j->scan(piece)) break;
    }
    if (glue) glue->ready();
    return j->ready();
  }
  bool boot(const BootAsk& b, sx::Store::WaitFn wait = waitHook) {
    begin(b, wait);
    return scan();
  }
  std::set<std::string> trimmed() const {
    std::set<std::string> t;
    for (const std::string& o : ops) {
      char name[40], why[40];
      if (std::sscanf(o.c_str(), "delete %39s %39s", name, why) == 2 && std::string(why) == "acknowledged") t.insert(name);
    }
    return t;
  }
  // A play to its end, then (`write`) its slot: through the Tracker and the
  // Outbox as the firmware's are, its fields exactly as `p` gives them.
  void ended(const PlaySpec& p) {
    tenv.next = pl::PlayFile();
    tenv.next.pathHash = p.hash;
    tenv.next.generation = p.generation;
    tenv.next.source = p.source;
    tenv.now = p.up;
    tracker->began(1, 1, 0);
    tenv.now = static_cast<uint64_t>(p.up) + p.played;
    tracker->tick(PlayState::Playing, p.played, true, p.duration);
    tracker->ended(static_cast<PlaybackController::Plays::End>(p.outcome));
    oenv.at = p.at;
    oenv.size = p.size;
    oenv.fatTime = p.fatTime;
  }
  pl::Outbox::Did write(const PlaySpec& p) {
    ended(p);
    TEST_ASSERT_EQUAL_UINT32(1, tracker->pending());
    return out->writeOne().did;
  }
};

// ---------------------------------------------------------------------------
// A fixture's boot through the journal.
// ---------------------------------------------------------------------------
BootAsk askOf(const minijson::Value& b) {
  BootAsk a;
  a.acks = acksOf(b["acks"]);
  a.ask.nowMs = b["now"].isNull() ? 0 : b["now"].u64();
  a.ask.hasCommit = !b["commitTime"].isNull();
  a.ask.commitSec = b["commitTime"].u64();
  a.ask.playing = b["playing"].boolean();
  a.ask.safePoint = b["safePoint"].boolean();
  a.saveFails = b["saveFails"].boolean();
  return a;
}

// The scan's card faults a fixture names (`unread`): a log file's open the
// card fails, or its reads from a slot on; the rest as FaultFs's.
struct ScanFaults : FaultFs {
  using FaultFs::FaultFs;
  std::set<std::string> openFails;
  tagstore::File* openRead(const char* p, bool* cardFailed) override {
    if (openFails.count(p)) {
      *cardFailed = true;
      return nullptr;
    }
    return FaultFs::openRead(p, cardFailed);
  }
  void unread(const minijson::Value& u) {
    openFails.clear();
    badPath.clear();
    bad = faultfs::Knob();
    for (const auto& m : u.members) {
      if (m.second.isNull()) {
        openFails.insert(kDir + m.first);
      } else {
        TEST_ASSERT_TRUE_MESSAGE(badPath.empty(), "one log file's reads fail at a time");
        badPath = kDir + m.first;
        badFrom = static_cast<uint32_t>(m.second.u64()) * pl::kSlot;
        badTo = 0xFFFFFFFFu;
        bad.fail = -1;
      }
    }
  }
  void healed() { unread(minijson::Value()); }
};

// A fixture's device on a card: NVS holds its epoch (new-epoch's is erased,
// and draws it).
void deviceFor(Player& p, const minijson::Value& sc) {
  uint8_t dev[6];
  deviceOf(sc["device"][0].str(), dev);
  std::memcpy(p.id, dev, 6);
  const uint64_t epoch = sc["device"][1].u64();
  if (sc["name"].str() == "new-epoch") {
    p.nvs.ids = Journal::Ids();
    p.epochs.list = {epoch};
  } else {
    p.nvs.ids.epoch = epoch;
  }
}

struct Outcome {
  std::string read, why;
  uint32_t folded = 0, twice = 0, gap = 0, removed = 0;
  bool partial = false, saved = false, compacted = false;
  std::set<std::string> trimmed;
};

Outcome outcomeOf(const Player& p) {
  Outcome o;
  const Glue& g = *p.glue;
  o.read = sx::readName(g.first.read);
  o.why = g.first.why == cc::Why::Ok ? "" : lower(cc::whyName(g.first.why));
  o.folded = g.fold.folded();
  o.twice = g.fold.twice();
  o.gap = g.fold.gap();
  o.partial = p.j->booted().partial > 0;
  o.saved = g.last.what == sx::Saved::Wrote;
  o.compacted = g.last.compacted;
  o.removed = g.first.removed + (g.last.oldBaseRemoved ? 1 : 0);
  o.trimmed = p.trimmed();
  return o;
}

void sameOutcome(const minijson::Value& b, const Outcome& o, const std::string& what) {
  const char* w = what.c_str();
  TEST_ASSERT_EQUAL_STRING_MESSAGE(b["read"].c_str(), o.read.c_str(), w);
  if (!b["why"].isNull()) TEST_ASSERT_EQUAL_STRING_MESSAGE(lower(b["why"].str()).c_str(), o.why.c_str(), w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["folded"].u64(), o.folded, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["twice"].u64(), o.twice, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["gap"].u64(), o.gap, w);
  TEST_ASSERT_EQUAL_MESSAGE(b["partial"].boolean(), o.partial, w);
  TEST_ASSERT_EQUAL_MESSAGE(b["saved"].boolean(), o.saved, w);
  TEST_ASSERT_EQUAL_MESSAGE(b["compacted"].boolean(), o.compacted, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(b["removed"].size(), o.removed, w);
  std::set<std::string> want;
  for (size_t t = 0; t < b["trimmed"].size(); ++t) want.insert(b["trimmed"][t].str());
  TEST_ASSERT_TRUE_MESSAGE(want == o.trimmed, w);
}

// One boot of a fixture over `card` (its files there already), through
// `faults` (over `card`); the outcome, and the player kept for more.
Outcome fixtureBoot(Player& p, ScanFaults& faults, const minijson::Value& b) {
  p.fs = &faults;
  faults.unread(b["unread"]);
  gWaits.clear();
  TEST_ASSERT_TRUE(p.boot(askOf(b)));
  faults.healed();
  return outcomeOf(p);
}

std::string what(const minijson::Value& sc, size_t k, const char* more = "") {
  char w[160];
  std::snprintf(w, sizeof(w), "%s, boot %u%s", sc["name"].c_str(), static_cast<unsigned>(k + 1), more);
  return w;
}

}  // namespace

// ---------------------------------------------------------------------------
// Every boot of every fixture, through the journal.
// ---------------------------------------------------------------------------
void test_every_fixture_through_the_journal() {
  int boots = 0, saves = 0, compactions = 0, trims = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    CutFs card;
    put(card, cardOf(sc["boots"][0]["in"]));
    ScanFaults faults(card);
    Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
    deviceFor(p, sc);
    p.piece = 1 + static_cast<uint32_t>(i % 7);  // (pieces of 1 to 7 slots)
    for (size_t k = 0; k < sc["boots"].size(); ++k) {
      const minijson::Value& b = sc["boots"][k];
      const std::string w = what(sc, k);
      // Chained: the card as the boot before left it, and what the device
      // wrote between (`added`).
      const Files in = cardOf(b["in"]);
      for (size_t a = 0; a < b["added"].size(); ++a) put(card, {{b["added"][a].str(), in.at(b["added"][a].str())}});
      sameFiles(in, filesOf(card), w + " (in)");
      const Outcome o = fixtureBoot(p, faults, b);
      sameOutcome(b, o, w);
      sameFiles(cardOf(b["out"]), filesOf(card), w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w.c_str());
      // The journal's side: the save before the trim, never after a partial scan.
      const Glue& g = *p.glue;
      if (b["partial"].boolean()) {
        std::string calls;
        for (const std::string& c : g.calls) calls += " " + c;
        TEST_ASSERT_TRUE_MESSAGE((g.calls == std::vector<std::string>{"began", "partial"}), (w + ":" + calls).c_str());
      } else {
        TEST_ASSERT_EQUAL_size_t_MESSAGE(2, g.calls.size(), w.c_str());
        TEST_ASSERT_EQUAL_STRING_MESSAGE("began", g.calls[0].c_str(), w.c_str());
        const bool off = !sx::statsOn(g.first.read) || b["saveFails"].boolean();
        TEST_ASSERT_EQUAL_STRING_MESSAGE(off ? "save boot (no)" : "save boot", g.calls[1].c_str(), w.c_str());
      }
      if (sc["name"].str() == "new-epoch") {
        TEST_ASSERT_TRUE(p.j->newEpoch());
        TEST_ASSERT_EQUAL_UINT32(3, p.j->booted().slots);  // (0.14.0's scan read none: a new epoch, no ACKS)
      }
      ++boots;
      saves += o.saved;
      compactions += o.compacted;
      trims += static_cast<int>(o.trimmed.size());
    }
  }
  printf("fixtures through the journal: %d boots, %d saves, %d bases, %d files trimmed\n", boots, saves, compactions,
         trims);
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(22, trims);
}

// A remount in the boot's scan (after its first piece): the folds made
// before it dropped, stats.bin read again (one read), the scan again from
// its start: each slot once. And one after Ready before the first slot: the
// scan and the read again, the save again with nothing new (nothing
// written), nothing more trimmed.
void test_a_remount_mid_scan_and_after_ready() {
  int runs = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& sc = gIndex[i];
    for (size_t k = 0; k < sc["boots"].size(); ++k) {
      const minijson::Value& b = sc["boots"][k];
      const std::string w = what(sc, k, ", a remount mid-scan");
      CutFs card;
      put(card, cardOf(b["in"]));
      ScanFaults faults(card);
      faults.unread(b["unread"]);
      Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
      deviceFor(p, sc);
      p.fs = &faults;
      p.piece = 2;
      p.begin(askOf(b));
      p.glue->reserve(1);
      const bool done = p.j->scan(1);  // (one slot read: every fixture's log has more)
      TEST_ASSERT_FALSE_MESSAGE(done, w.c_str());
      p.j->remounted();
      TEST_ASSERT_TRUE_MESSAGE(p.scan(), w.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE("began again", p.glue->calls[1].c_str(), w.c_str());
      Outcome o = outcomeOf(p);
      // (The verdict the boot's read gave; the remount's read, once, sees
      // what it left: corrupt's stats.bad renamed, absent now.)
      o.removed = p.glue->first.removed + (p.glue->last.oldBaseRemoved ? 1 : 0);
      sameOutcome(b, o, w);
      sameFiles(cardOf(b["out"]), filesOf(card), w);
      // After Ready, before the first slot.
      const long steps = card.steps;
      p.j->remounted();
      TEST_ASSERT_TRUE_MESSAGE(p.scan(), w.c_str());
      const std::string again = what(sc, k, ", a remount after Ready");
      // (Nothing saved before it, a partial scan's or a full card's: the
      // same slots folded again, and again no save.)
      const bool unsaved = b["partial"].boolean() || b["saveFails"].boolean();
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(unsaved ? b["folded"].u64() : 0, p.glue->fold.folded(), again.c_str());
      TEST_ASSERT_EQUAL_INT32_MESSAGE(steps, card.steps, again.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, p.j->booted().restarts, again.c_str());
      if (!unsaved && sx::statsOn(p.glue->opened.read))
        TEST_ASSERT_EQUAL_STRING_MESSAGE("save boot", p.glue->calls.back().c_str(), again.c_str());
      sameFiles(cardOf(b["out"]), filesOf(card), again);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), again.c_str());
      ++runs;
    }
  }
  TEST_ASSERT_EQUAL_INT(34, runs);
}

// ---------------------------------------------------------------------------
// A cut at every write step of the boot's save, its compaction and the trim.
// ---------------------------------------------------------------------------
namespace {

// The boots cut: a save, a compaction (over the old base; over a stale
// base after stats.bin corrupt; after a base lost, the boot that doesn't
// compact under the name stats.bin names, then the one that does), the trim
// of one file and of two. "deleted": corrupt's card with stats.bin deleted
// (and no stats.bad): a fresh start whose compaction replaces the stale
// stats-00000001.bin; its want is corrupt's but stats.bad.
struct CutCase {
  const char* name;
  int boot;
  bool deleted;
};
const CutCase kCuts[] = {{"worked-example", 0, false}, {"rules", 0, false},  {"compaction", 0, false},
                         {"corrupt", 0, false},        {"corrupt", 0, true}, {"base-other", 0, false},
                         {"base-other", 1, false},     {"two-devices", 0, false}, {"gap", 0, false},
                         {"partial-tail", 1, false}};

Files cutIn(const CutCase& c, Files* want) {
  const minijson::Value& b = scenario(c.name)["boots"][c.boot];
  Files in = cardOf(b["in"]);
  *want = cardOf(b["out"]);
  if (c.deleted) {
    in.erase("stats.bin");
    in.erase("stats.bad");
    want->erase("stats.bad");
  }
  return in;
}

}  // namespace

void test_a_cut_at_every_step_of_the_save_and_the_trim() {
  int runs = 0;
  for (const CutCase& c : kCuts) {
    const minijson::Value& sc = scenario(c.name);
    const minijson::Value& b = sc["boots"][c.boot];
    Files want;
    const Files in = cutIn(c, &want);
    int steps = 0;
    for (long k = 1;; ++k) {
      bool cut = false;
      for (cutfs::Variant v : {cutfs::Variant::InOrder, cutfs::Variant::LoseUnsynced, cutfs::Variant::Torn}) {
        CutFs card;
        put(card, in);
        Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
        deviceFor(p, sc);
        p.begin(askOf(b));
        p.glue->beforeSave = [&card, k] { card.cutAt = card.steps + k; };
        p.scan();
        if (!card.dead) break;
        cut = true;
        char w[160];
        std::snprintf(w, sizeof(w), "%s %d%s, cut at the save's step %ld, way %d", c.name, c.boot + 1,
                      c.deleted ? " (deleted)" : "", k, static_cast<int>(v));
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
        CutFs next = card.reboot(v);
        Player q(next, p.id);
        q.nvs = p.nvs;
        q.boot(askOf(b));
        // The old commit or the new one, never unread; a base lost only
        // where the boot's own read lost it (base-other).
        const std::string read = sx::readName(q.glue->first.read);
        TEST_ASSERT_TRUE_MESSAGE(read == "loaded" || read == "absent", w);
        TEST_ASSERT_TRUE_MESSAGE(!q.glue->first.baseLost || (b["stats"]["head"]["flags"].u64() & sf::kFlagBaseLost), w);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, next.violations.size(), w);
        // A rename cut between its writes leaves stats.tmp on stats.bin's
        // chain: settle() made it a twin, kept until a disk check parts them.
        for (int t = 1; t <= tagstore::kMaxTwins; ++t) {
          char twin[sx::kNameCap];
          TEST_ASSERT_TRUE(tagstore::twinName(sx::kStatsNames, t, twin, sizeof(twin)));
          if (next.dir.count(twin)) TEST_ASSERT_TRUE_MESSAGE(next.shared(twin, sx::kStatsBin), w);
        }
        partTwins(next);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, next.violations.size(), w);
        sameFiles(want, filesOf(next), w);
        ++runs;
      }
      if (!cut) break;
      ++steps;
    }
    printf("%s %d%s: the save's and the trim's %d steps cut three ways\n", c.name, c.boot + 1,
           c.deleted ? " (stats.bin deleted)" : "", steps);
    TEST_ASSERT_TRUE(steps >= 5);
  }
  TEST_ASSERT_TRUE(runs > 0);
}

// ---------------------------------------------------------------------------
// A question or a step the card fails, each in its turn, through a boot;
// then the card answers, and a boot leaves the uncut boot's card.
// ---------------------------------------------------------------------------
namespace {
// The card after a boot that failed a step, then a boot: the uncut boot's
// card; or, where the step was its compaction's (the save went on
// uncompacted, and the next boot, with nothing new, doesn't compact),
// the card a boot that couldn't compact leaves (compaction-playing's).
void sameOrUncompacted(const char* name, const Files& want, const Files& got, const char* w) {
  if (std::string(name) == "compaction" && !got.count("stats-00000002.bin")) {
    sameFiles(cardOf(scenario("compaction-playing")["boots"][0]["out"]), got, std::string(w) + " (uncompacted)");
    return;
  }
  sameFiles(want, got, w);
}
}  // namespace

void test_the_card_fails_a_question_or_a_step() {
  int runs = 0;
  for (const CutCase& c : {CutCase{"worked-example", 0, false}, CutCase{"compaction", 0, false},
                           CutCase{"two-devices", 0, false}, CutCase{"base-neighbours", 0, false}}) {
    const minijson::Value& sc = scenario(c.name);
    const minijson::Value& b = sc["boots"][c.boot];
    Files want;
    const Files in = cutIn(c, &want);
    // Each question (an entry, an open to read) the card fails once.
    long events = 0;
    for (long k = -1; k < events; ++k) {
      FlakyFs card;
      put(card, in);
      card.failing(k < 0 ? 1000000 : k, k < 0 ? -1 : k + 1);
      Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
      deviceFor(p, sc);
      p.boot(askOf(b));
      if (k < 0) {
        events = card.events;
        sameFiles(want, filesOf(card), c.name);
        continue;
      }
      char w[160];
      std::snprintf(w, sizeof(w), "%s %d, question %ld of %ld failed", c.name, c.boot + 1, k, events);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
      card.failing(-1, -1);
      Player q(card, p.id);
      q.nvs = p.nvs;
      q.boot(askOf(b));
      partTwins(card);
      sameOrUncompacted(c.name, want, filesOf(card), w);
      ++runs;
    }
    TEST_ASSERT_TRUE(events > 10);
    // Each step a knob names, failed at its n-th call.
    using KnobOf = faultfs::Knob FaultFs::*;
    const std::pair<const char*, KnobOf> knobs[] = {
        {"write", &FaultFs::noWrite},   {"sync", &FaultFs::noSync},     {"close", &FaultFs::noClose},
        {"create", &FaultFs::noCreate}, {"rename", &FaultFs::noRename}, {"half a rename", &FaultFs::halfRename},
        {"remove", &FaultFs::noRemove}, {"entry", &FaultFs::noEntry}};
    for (const auto& kn : knobs) {
      for (long n = 0;; ++n) {
        CutFs card;
        put(card, in);
        FaultFs fs(card);
        (fs.*kn.second).skip = n;
        (fs.*kn.second).fail = 1;
        Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
        deviceFor(p, sc);
        p.fs = &fs;
        p.boot(askOf(b));
        if ((fs.*kn.second).fail == 1) break;  // (no n-th call)
        char w[160];
        std::snprintf(w, sizeof(w), "%s %d, its %s %ld failed", c.name, c.boot + 1, kn.first, n);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
        Player q(card, p.id);
        q.nvs = p.nvs;
        q.boot(askOf(b));
        partTwins(card);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
        sameOrUncompacted(c.name, want, filesOf(card), w);
        ++runs;
      }
    }
  }
  printf("a question or a step failed, then a boot: %d runs\n", runs);
}

// A tmp written whole whose replace() failed is held: the next save in the
// session promotes it first (its rename the card refused: stats.bin removed,
// the tmp the only copy), and a reboot's settle() drops one while stats.bin
// is there (its remove refused), the next boot folding again.
void test_a_held_tmp() {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  const Files in = cardOf(b["in"]), want = cardOf(b["out"]);
  // Promoted by Hs, the rotation's path: the same stats, the next
  // generation (the held tmp a save that landed).
  {
    CutFs card;
    put(card, in);
    FaultFs fs(card);
    Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
    deviceFor(p, sc);
    p.fs = &fs;
    p.begin(askOf(b));
    p.glue->beforeSave = [&fs] { fs.noRename.fail = 1; };
    p.scan();
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(p.glue->last.what));
    TEST_ASSERT_TRUE(p.glue->store.held());
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.tmp") == want.at("stats.bin"));
    TEST_ASSERT_TRUE(p.trimmed().empty());  // (the MARK read at the boot, 41, doesn't cover plays.1.jnl)
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_TRUE(p.glue->last.heldPromoted);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    const Bytes bin = bytesOf(card, "stats.bin");
    const Bytes w = want.at("stats.bin");
    TEST_ASSERT_EQUAL_UINT32(cc::get32(reinterpret_cast<const uint8_t*>(w.data()) + 20) + 1,
                             cc::get32(reinterpret_cast<const uint8_t*>(bin.data()) + 20));
    TEST_ASSERT_TRUE(bin.substr(40) == w.substr(40));  // (all but the header: the generation and its CRC)
    // The next boot reads it, folds nothing, and trims what it covers.
    Player q(card, p.id);
    q.nvs = p.nvs;
    q.boot(askOf(b));
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(q.glue->first.read));
    TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
    TEST_ASSERT_TRUE((q.trimmed() == std::set<std::string>{"plays.1.jnl"}));
    Files got = filesOf(card);
    got["stats.bin"] = w;
    sameFiles(want, got, "a held tmp, promoted");
  }
  // Dropped by a reboot's settle() while stats.bin is there: the next boot
  // folds 42-48 again, and the card is the uncut boot's.
  {
    CutFs card;
    put(card, in);
    FaultFs fs(card);
    Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
    deviceFor(p, sc);
    p.fs = &fs;
    p.begin(askOf(b));
    p.glue->beforeSave = [&fs] { fs.noRemove.fail = 1; };
    p.scan();
    TEST_ASSERT_EQUAL_STRING("held", sx::savedName(p.glue->last.what));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == in.at("stats.bin"));
    TEST_ASSERT_TRUE(card.dir.count(sx::kStatsTmp) == 1);
    CutFs next = card.reboot(cutfs::Variant::InOrder);
    Player q(next, p.id);
    q.nvs = p.nvs;
    q.boot(askOf(b));
    TEST_ASSERT_TRUE(q.glue->first.settled.what == tagstore::Settle::Removed);
    TEST_ASSERT_EQUAL_UINT32(7, q.glue->fold.folded());
    sameFiles(want, filesOf(next), "a held tmp, dropped by a reboot");
  }
}

// ---------------------------------------------------------------------------
// The base faulted at the boot: its open, its header's read (each for good,
// through the boot's waits): Unread, never a lost base; stats off, so no
// fold, no save and no trim, and no stats-<n>.bin removed. The card
// answering again, the next boot is the fixture's.
// ---------------------------------------------------------------------------
void test_the_base_faulted_at_the_boot() {
  const minijson::Value& sc = scenario("base-neighbours");
  const minijson::Value& b = sc["boots"][0];
  const Files in = cardOf(b["in"]);
  for (long from : {0L, 1L}) {
    CutFs card;
    put(card, in);
    card.faultPrefix = "/.player/stats-00000001.bin";
    card.faultFrom = from;
    Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
    deviceFor(p, sc);
    p.alloc = heapAlloc;
    p.release = heapFree;
    const size_t held0 = gHeap.held;
    gWaits.clear();
    p.boot(askOf(b));
    const char* w = from == 0 ? "the base's open" : "the base's header";
    // The stats off: every block of the fold given back (stats.bin's chunks
    // and the spare were carved before its base failed), nothing held for
    // the session.
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, p.glue->fold.held(), w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(held0, gHeap.held, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.glue->fold.chunks(sf::kTrkd) + p.glue->fold.chunks(sf::kRcnt), w);
    TEST_ASSERT_FALSE_MESSAGE(p.glue->fold.spare(), w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("unread", sx::readName(p.glue->first.read), w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("io", cc::whyName(p.glue->first.why), w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->first.inBase, w);
    TEST_ASSERT_FALSE_MESSAGE(p.glue->first.baseLost, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, gWaits.size(), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.glue->first.removed, w);
    TEST_ASSERT_TRUE_MESSAGE((p.glue->calls == std::vector<std::string>{"began", "save boot (no)"}), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.glue->fold.folded(), w);
    TEST_ASSERT_TRUE_MESSAGE(p.trimmed().empty(), w);  // (ACKS covers both files)
    card.faultFrom = -1;
    sameFiles(in, filesOf(card), w);
    Player q(card, p.id);
    q.nvs = p.nvs;
    q.boot(askOf(b));
    sameOutcome(b, outcomeOf(q), w);
    sameFiles(cardOf(b["out"]), filesOf(card), w);
    // A remount's read, the card whole now: the fold's blocks carved again.
    p.j->remounted();
    TEST_ASSERT_TRUE_MESSAGE(p.scan(), w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("loaded", sx::readName(p.glue->opened.read), w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->core.on(), w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->fold.held() > 0, w);
  }
}

// ---------------------------------------------------------------------------
// firstAt's orders and the retention, through the journal (the fixtures'
// own checks are test_every_fixture_through_the_journal's; these say what
// they prove).
// ---------------------------------------------------------------------------
void test_first_at_orders_and_the_retention() {
  // The same seven slots read in both orders: one stats.bin.
  TEST_ASSERT_TRUE(cardOf(scenario("first-at-a")["boots"][0]["out"]).at("stats.bin") ==
                   cardOf(scenario("first-at-b")["boots"][0]["out"]).at("stats.bin"));
  // A clock a year ahead at one save, then corrected: HOUR and DAYS keep
  // every row (boot 2 adds its play's), RCNT pays.
  const minijson::Value& ca = scenario("clock-ahead");
  CutFs card;
  put(card, cardOf(ca["boots"][0]["in"]));
  ScanFaults faults(card);
  Player p(card, (const uint8_t*)"\0\0\0\0\0\1");
  deviceFor(p, ca);
  fixtureBoot(p, faults, ca["boots"][0]);
  const uint32_t hour1 = p.glue->fold.rows(sf::kHour), days1 = p.glue->fold.rows(sf::kDays);
  const uint32_t rcnt1 = p.glue->fold.rows(sf::kRcnt);
  put(card, {{"plays.jnl", cardOf(ca["boots"][1]["in"]).at("plays.jnl")}});
  fixtureBoot(p, faults, ca["boots"][1]);
  TEST_ASSERT_EQUAL_UINT32(41, hour1);
  TEST_ASSERT_EQUAL_UINT32(41, days1);
  TEST_ASSERT_EQUAL_UINT32(1, rcnt1);
  TEST_ASSERT_EQUAL_UINT32(hour1 + 1, p.glue->fold.rows(sf::kHour));
  TEST_ASSERT_EQUAL_UINT32(days1 + 1, p.glue->fold.rows(sf::kDays));
  TEST_ASSERT_EQUAL_UINT32(2, p.glue->fold.rows(sf::kRcnt));
  sameFiles(cardOf(ca["boots"][1]["out"]), filesOf(card), "clock-ahead");
  // A save with the clock unset; a foreign slot dated 2099 (sinceDay not
  // after today, nothing pruned by it).
  TEST_ASSERT_TRUE(scenario("no-clock")["boots"][0]["now"].isNull());
  const minijson::Value& ret = scenario("retention")["boots"][0];
  TEST_ASSERT_EQUAL_UINT64(840, ret["stats"]["rows"]["hour"].u64());
  TEST_ASSERT_EQUAL_UINT64(400, ret["stats"]["rows"]["days"].u64());
  TEST_ASSERT_TRUE(ret["stats"]["head"]["sinceDay"].u64() <= ret["today"].u64());
}

// ---------------------------------------------------------------------------
// A full log never synced: both files at the cap, 13,106 slots, through the
// journal's scan; TRKD's rows the distinct hashes. The peak of the fold's
// blocks and the microseconds a slot (the scan and the fold), printed.
// ---------------------------------------------------------------------------
void test_a_full_log_never_synced() {
  const uint8_t dev[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
  const uint64_t epoch = 0x1122334455667788ull;
  CutFs card;
  std::set<uint64_t> hashes;
  uint64_t x = 2026;
  uint64_t plays = 0;
  std::vector<uint8_t> files[2];
  for (uint32_t q = 1; q <= 2 * pl::kFileSlots; ++q) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    Slot s;
    s.kind = pl::kKindPlay;
    s.outcome = static_cast<uint8_t>(1 + x % 3);
    s.source = static_cast<uint8_t>(x % 4);
    s.clock = pl::kClockRtc;
    s.seq = q;
    s.epoch = epoch;
    std::memcpy(s.deviceId, dev, 6);
    s.startedAt = sf::kMinMs + static_cast<uint64_t>(q / 25) * sf::kDayMs + (q % 25) * sf::kHourMs / 2;
    s.bootSeq = 1 + q / 100;
    s.uptimeMs = q * 1000;
    s.playedMs = 30000 + static_cast<uint32_t>(x % 200000);
    s.durationMs = 200000;
    const uint32_t track = static_cast<uint32_t>((x >> 20) % 5000);
    s.pathHash = cc::fnv1a64(&track, sizeof(track));
    hashes.insert(s.pathHash);
    plays += sf::isPlay(s);
    uint8_t b[pl::kSlot];
    pl::encode(s, b);
    std::vector<uint8_t>& f = files[q <= pl::kFileSlots ? 1 : 0];  // plays.1.jnl the older
    f.insert(f.end(), b, b + pl::kSlot);
  }
  card.put(kLog, files[0]);
  card.put(kPrev, files[1]);
  Player p(card, dev);
  p.nvs.ids.epoch = epoch;
  p.nvs.ids.seq = 2 * pl::kFileSlots;
  p.buf.assign(pl::kReadBytes, 0);  // the firmware's: 32 slots a read
  p.piece = 128;                    // app/PlayHistory's piece
  p.alloc = heapAlloc;
  p.release = heapFree;
  gHeap = Heap();
  BootAsk a;
  a.ask.nowMs = sf::kMinMs + 530ull * sf::kDayMs;
  a.ask.playing = true;  // (no compaction: the fold and stats.bin alone)
  a.ask.safePoint = false;
  p.begin(a);
  const size_t atRead = gHeap.held;
  gHeap.peak = gHeap.held;
  p.glue->fold.resetPeak();
  const auto t0 = std::chrono::steady_clock::now();
  TEST_ASSERT_TRUE(p.scan());
  const auto t1 = std::chrono::steady_clock::now();
  const Glue& g = *p.glue;
  TEST_ASSERT_EQUAL_UINT32(2 * pl::kFileSlots, g.fold.folded());
  TEST_ASSERT_EQUAL_UINT32(hashes.size(), g.fold.rows(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT64(plays, g.fold.head().plays);
  TEST_ASSERT_EQUAL_UINT64(2 * pl::kFileSlots, g.fold.head().slots);
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(g.last.what));
  TEST_ASSERT_FALSE(g.fold.noMemory());
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  printf("a full log never synced, through the journal: %u slots, %u hashes (TRKD %u rows), RCNT %u rows after the "
         "save; the fold's peak %u bytes (the heap's %u, %u at the read); %.2f us a slot on this host (the scan, the "
         "fold, the save); stats.bin %u bytes\n",
         static_cast<unsigned>(2 * pl::kFileSlots), static_cast<unsigned>(hashes.size()),
         static_cast<unsigned>(g.fold.rows(sf::kTrkd)), static_cast<unsigned>(g.fold.rows(sf::kRcnt)),
         static_cast<unsigned>(g.fold.peak()), static_cast<unsigned>(gHeap.peak), static_cast<unsigned>(atRead),
         us / (2 * pl::kFileSlots), static_cast<unsigned>(g.last.bytes));
  TEST_ASSERT_TRUE(g.fold.peak() < 512 * 1024);
  // Read back by the next boot: the same rows, nothing folded again.
  Player q(card, dev);
  q.nvs = p.nvs;
  q.boot(a);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(q.glue->first.read));
  TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
  TEST_ASSERT_EQUAL_UINT32(hashes.size(), q.glue->fold.rows(sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT64(plays, q.glue->fold.head().plays);
}

// ---------------------------------------------------------------------------
// The sessions (stats/sessions.json): each step through the firmware's
// code, the card after it compared with statsref's, file for file.
// ---------------------------------------------------------------------------
namespace {

Files sessionCard(const minijson::Value& where) {
  Files out;
  for (const auto& m : where.members)
    out[m.first] = cardfixtures::bytes("stats/sessions/" + m.second.str() + "/" + m.first);
  return out;
}

const minijson::Value& session(const char* name) {
  for (size_t i = 0; i < gSessions.size(); ++i)
    if (gSessions[i]["name"].str() == name) return gSessions[i];
  TEST_FAIL_MESSAGE(name);
  return gSessions[0];
}

PlaySpec playOf(const minijson::Value& st) {
  PlaySpec p;
  p.hash = st["hash"].u64();
  p.played = static_cast<uint32_t>(st["played"].u64());
  p.duration = static_cast<uint32_t>(st["duration"].u64());
  p.outcome = static_cast<uint8_t>(st["outcome"].u64());
  p.source = static_cast<uint8_t>(st["src"].u64());
  p.at = st["at"].u64();
  p.up = static_cast<uint32_t>(st["up"].u64());
  p.size = static_cast<uint32_t>(st["size"].u64());
  p.fatTime = static_cast<uint32_t>(st["fatTime"].u64());
  p.generation = static_cast<uint32_t>(st["generation"].u64());
  return p;
}

// A stats.bin's TRKD rows, by hash.
std::map<uint64_t, sf::Track> tracksOf(const Bytes& b) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, sx::kStatsSpec, nullptr)));
  const cc::Section& s = c.section(sf::kTrkd);
  std::map<uint64_t, sf::Track> out;
  for (uint32_t i = 0; i < s.count; ++i) {
    const sf::Track t = sf::decodeTrack(reinterpret_cast<const uint8_t*>(b.data()) + s.offset + i * s.stride);
    out[t.hash] = t;
  }
  return out;
}

// One session, each step through the players' journals, glues, Trackers
// and Outboxes over one card; the card compared after each step, and the
// step's facts (a boot's read, folds, save and trim; a play's seq and its
// rotation's save; Hs). The card's HEAD at the end, read back: every slot
// but `lost`.
sf::Head runSession(const minijson::Value& sc) {
  const std::string name = sc["name"].str();
  const uint32_t cap = static_cast<uint32_t>(sc["cap"].u64());
  CutFs card;
  put(card, sessionCard(sc["in"]));
  ScanFaults faults(card);
  std::map<std::string, std::unique_ptr<Player>> players;
  for (const auto& m : sc["players"].members) {
    uint8_t dev[6];
    deviceOf(m.second["device"].str(), dev);
    std::unique_ptr<Player> p(new Player(card, dev, cap));
    p->fs = &faults;
    for (size_t e = 0; e < m.second["epochs"].size(); ++e) p->epochs.list.push_back(m.second["epochs"][e].u64());
    const minijson::Value& nvs = m.second["nvs"];
    if (!nvs.isNull()) {
      p->nvs.ids.epoch = nvs[0].u64();
      p->nvs.ids.seq = static_cast<uint32_t>(nvs[1].u64());
      p->nvs.ids.boots = static_cast<uint32_t>(nvs[2].u64());
    }
    players[m.first] = std::move(p);
  }
  for (size_t i = 0; i < sc["steps"].size(); ++i) {
    const minijson::Value& st = sc["steps"][i];
    const std::string doing = st["do"].str();
    char w[160];
    std::snprintf(w, sizeof(w), "%s, step %u (%s %s)", name.c_str(), static_cast<unsigned>(i + 1), doing.c_str(),
                  st["player"].isNull() ? "" : st["player"].c_str());
    Player* p = st["player"].isNull() ? nullptr : players.at(st["player"].str()).get();
    if (doing == "boot") {
      p->stats = st["stats"].isNull() || st["stats"].boolean();
      BootAsk a;
      a.acks = acksOf(st["acks"]);
      a.ask.nowMs = st["now"].isNull() ? 0 : st["now"].u64();
      a.ask.playing = st["playing"].boolean();
      a.ask.safePoint = st["safePoint"].boolean();
      a.saveFails = st["saveFails"].boolean();
      faults.unread(st["unread"]);
      gWaits.clear();
      TEST_ASSERT_TRUE_MESSAGE(p->boot(a), w);
      faults.healed();
      std::set<std::string> want;
      for (size_t t = 0; t < st["trimmed"].size(); ++t) want.insert(st["trimmed"][t].str());
      TEST_ASSERT_TRUE_MESSAGE(want == p->trimmed(), w);
      if (p->stats) sameOutcome(st, outcomeOf(*p), w);
    } else if (doing == "play") {
      const PlaySpec spec = playOf(st);
      Glue* g = p->glue.get();
      const size_t calls = g ? g->calls.size() : 0;
      if (g) {
        if (!st["now"].isNull()) g->ask.nowMs = st["now"].u64();
        g->saveFails = st["saveFails"].boolean();
      }
      if (st["ending"].boolean()) {
        // The orderly end's flush and its 3 s budget: the played file's
        // question takes what leaves `left` of it at the rotation's save.
        TEST_ASSERT_NOT_NULL(g);
        g->ending(p->oenv.now + 3000);
        p->oenv.perStat = 3000 - st["left"].u64();
        p->ended(spec);
        const pl::Outbox::Flushed f = p->out->flush(3000, 128);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, f.written, w);
        g->notEnding();
        p->oenv.perStat = 0;
      } else {
        TEST_ASSERT_TRUE_MESSAGE(p->write(spec) == pl::Outbox::Did::Written, w);
      }
      if (g) g->saveFails = false;
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["seq"].u64(), p->j->lastSeq(), w);
      // The rotation's save: asked or not, and what it came to; when it
      // ran, plays.1.jnl was still on the card (before its delete, or its
      // rename to a twin).
      std::string save;
      for (size_t c = calls; g && c < g->calls.size(); ++c) save = g->calls[c];
      const char* want = st["save"].isNull() ? "" : st["save"].boolean() ? "save rotation" : "save rotation (no)";
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want, save.c_str(), w);
      if (save == "save rotation") TEST_ASSERT_EQUAL_STRING_MESSAGE("plays.1.jnl plays.jnl", g->seen.back().c_str(), w);
    } else if (doing == "hs") {
      p->glue->ask.nowMs = st["now"].u64();
      TEST_ASSERT_EQUAL_MESSAGE(st["ok"].boolean(), p->glue->hs(), w);
    } else if (doing == "giveback") {
      // A library update's give-back (6.7): the save first when the fold
      // holds plays the card's stats.bin doesn't, then the blocks given
      // back, MARK's kept.
      Glue* g = p->glue.get();
      g->ask.nowMs = st["now"].u64();
      g->saveFails = st["saveFails"].boolean();
      const size_t calls = g->calls.size();
      const size_t held = g->fold.held();
      const size_t bytes = g->core.giveBack();
      g->saveFails = false;
      std::string save;
      for (size_t c = calls; c < g->calls.size(); ++c) save = g->calls[c];
      const char* want = st["saved"].isNull() ? "" : st["saved"].boolean() ? "save give-back" : "save give-back (no)";
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want, save.c_str(), w);
      TEST_ASSERT_EQUAL_MESSAGE(st["given"].boolean(), g->core.givenBack(), w);
      const statsglue::GaveBack& gb = g->core.gaveBack();
      TEST_ASSERT_EQUAL_size_t_MESSAGE(bytes, gb.bytes, w);
      TEST_ASSERT_TRUE_MESSAGE(bytes > 0 && gb.kept > 0, w);  // (MARK's block, kept)
      TEST_ASSERT_EQUAL_size_t_MESSAGE(gb.kept, g->fold.held(), w);
      TEST_ASSERT_TRUE_MESSAGE(bytes + gb.kept >= held, w);  // (a save's retention may have given some back first)
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, g->fold.chunks(sf::kTrkd) + g->fold.chunks(sf::kRcnt), w);
      TEST_ASSERT_FALSE_MESSAGE(g->fold.spare(), w);
    } else if (doing == "takeback") {
      Glue* g = p->glue.get();
      const statsglue::TookBack& t = g->core.takeBack();
      TEST_ASSERT_TRUE_MESSAGE(t.read, w);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(st["read"].c_str(), sx::readName(g->opened.read), w);
      TEST_ASSERT_EQUAL_MESSAGE(st["same"].boolean(), t.same, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["folded"].u64(), t.folded, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["covered"].u64(), t.covered, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["over"].u64(), t.over, w);
      TEST_ASSERT_EQUAL_MESSAGE(st["behind"].boolean(), g->core.behind(), w);
      TEST_ASSERT_FALSE_MESSAGE(g->core.givenBack(), w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, g->core.ringCount(), w);
    } else if (doing == "remount") {
      p->j->remounted();
      p->out->remounted();
    } else if (doing == "nvs") {
      p->nvs.ids.seq = static_cast<uint32_t>(st["seq"].u64());
    } else if (doing == "erase") {
      p->nvs.ids = Journal::Ids();
    } else if (doing == "delete") {
      TEST_ASSERT_TRUE_MESSAGE(card.remove((kDir + st["name"].str()).c_str()), w);
    } else if (doing == "link") {
      card.link(kDir + st["a"].str(), kDir + st["b"].str());
    } else {
      TEST_FAIL_MESSAGE(w);
    }
    sameFiles(sessionCard(st["out"]), filesOf(card), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
  }
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  sx::Store store(card);
  Work work;
  TEST_ASSERT_EQUAL_STRING_MESSAGE("loaded", sx::readName(store.read(f, work.p(), nullptr).read), name.c_str());
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(sc["slots"].u64() - sc["lost"].u64(), f.head().slots, name.c_str());
  return f.head();
}

}  // namespace

// Every session, each step through the firmware's code.
void test_every_session() {
  int steps = 0;
  for (size_t i = 0; i < gSessions.size(); ++i) {
    runSession(gSessions[i]);
    steps += static_cast<int>(gSessions[i]["steps"].size());
  }
  printf("sessions: %u, %d steps, the card compared after each\n", static_cast<unsigned>(gSessions.size()), steps);
  TEST_ASSERT_EQUAL_UINT32(23, gSessions.size());
}

// A rotation over a plays.1.jnl no save covers (the boot's save failed): its
// save first, before the delete or the rename to a twin; failing too, the
// plays in it are lost to the stats (5.5, 13: the undercount), exactly. So
// are they in a rotation in the end's flush with under 1 s of its budget
// left.
void test_the_rotation_hole() {
  const std::pair<const char*, uint64_t> cases[] = {{"rotation-saves", 0},      {"rotation-fails", 4},
                                                    {"rotation-twin-saves", 0}, {"rotation-twin-fails", 4},
                                                    {"flush-in-time", 0},       {"flush-late", 4},
                                                    {"partial-rotation", 4}};
  for (const auto& c : cases) {
    const minijson::Value& sc = session(c.first);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(c.second, sc["lost"].u64(), c.first);
    const uint64_t slots = sc["slots"].u64();
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(std::string(c.first) == "partial-rotation" ? 13 : 9, slots, c.first);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(slots - c.second, runSession(sc).slots, c.first);
  }
}

// A second player saving (and compacting) between two saves of the first:
// the first's next save refused (the card's copy changed under it); its
// next boot folds what neither saved, each slot once. NVS put back after a
// trim: the next play's seq above ACKS's and MARK's (C5). Two players
// alternating. A 0.14.0 boot's trim between: HEAD's gap. A new epoch with
// no ACKS mid-history. A partial scan, a live play, Hs refused.
void test_players_nvs_and_old_firmware() {
  const sf::Head two = runSession(session("second-player"));
  TEST_ASSERT_EQUAL_UINT64(6, two.slots);
  TEST_ASSERT_EQUAL_UINT32(1, two.baseGen);  // (B's compaction)
  const minijson::Value& hs = session("second-player")["steps"][10];
  TEST_ASSERT_EQUAL_STRING("hs", hs["do"].c_str());
  TEST_ASSERT_FALSE(hs["ok"].boolean());
  TEST_ASSERT_EQUAL_UINT64(5, runSession(session("nvs-back")).slots);
  TEST_ASSERT_EQUAL_UINT64(5, session("nvs-back")["steps"][8]["seq"].u64());  // (not 2)
  // The log deleted by hand: MARK alone keeps the seq above what it covers.
  const minijson::Value& del = session("log-deleted");
  TEST_ASSERT_EQUAL_UINT64(5, runSession(del).slots);
  TEST_ASSERT_EQUAL_UINT64(5, del["steps"][9]["seq"].u64());  // (not 2)
  TEST_ASSERT_EQUAL_UINT64(6, runSession(session("alternating")).slots);
  const sf::Head old = runSession(session("old-firmware"));
  TEST_ASSERT_EQUAL_UINT32(3, old.gap);
  TEST_ASSERT_EQUAL_UINT64(3, old.slots);
  TEST_ASSERT_EQUAL_UINT64(4, runSession(session("new-epoch-live")).slots);
  const minijson::Value& ph = session("partial-hs");
  TEST_ASSERT_EQUAL_STRING("hs", ph["steps"][2]["do"].c_str());
  TEST_ASSERT_FALSE(ph["steps"][2]["ok"].boolean());
  TEST_ASSERT_EQUAL_UINT64(15, runSession(ph).slots);
}

// firstAt through a rotation's save: the live plays' rows are a boot fold's
// (first-at-a's: the same plays) but lastOrd, which goes by order: the rows
// whose latest play was live at HEAD.round + 2, the one whose latest play
// was the boot's at + 1.
void test_first_at_through_a_rotations_save() {
  const minijson::Value& sc = session("first-at-live");
  runSession(sc);
  Bytes saved;
  for (size_t i = 0; i < sc["steps"].size(); ++i)
    if (sc["steps"][i]["save"].boolean()) saved = sessionCard(sc["steps"][i]["out"]).at("stats.bin");
  TEST_ASSERT_FALSE(saved.empty());
  const std::map<uint64_t, sf::Track> live = tracksOf(saved);
  const std::map<uint64_t, sf::Track> boot = tracksOf(cardOf(scenario("first-at-a")["boots"][0]["out"]).at("stats.bin"));
  TEST_ASSERT_EQUAL_size_t(3, boot.size());
  int liveLatest = 0;
  for (const auto& r : boot) {
    TEST_ASSERT_TRUE(live.count(r.first));
    const sf::Track& a = r.second;
    const sf::Track& b = live.at(r.first);
    TEST_ASSERT_EQUAL_UINT16(a.plays, b.plays);
    TEST_ASSERT_EQUAL_UINT16(a.skips, b.skips);
    TEST_ASSERT_EQUAL_UINT32(a.firstAt, b.firstAt);
    TEST_ASSERT_EQUAL_UINT32(a.lastAt, b.lastAt);
    TEST_ASSERT_EQUAL_UINT8(a.src, b.src);
    TEST_ASSERT_EQUAL_UINT32(1, a.lastOrd);  // (one boot's round)
    TEST_ASSERT_TRUE(b.lastOrd == 1 || b.lastOrd == 2);
    liveLatest += b.lastOrd == 2;
  }
  TEST_ASSERT_EQUAL_INT(2, liveLatest);  // Harbour Lights and Tidewater: their latest play A's, live
  // North Pier (its row the one with a skip): its latest play B's, folded at the boot.
  for (const auto& r : boot)
    if (r.second.skips) TEST_ASSERT_EQUAL_UINT32(1, live.at(r.first).lastOrd);
}

// ---------------------------------------------------------------------------
// The glue's own rules (StatsGlue: app/ListeningStats's), beyond what the
// fixtures and the sessions run through it: the card away or another card in
// the slot (no save; the trim by the MARK read at the boot); Hs refused with
// the card away, behind the update's fence and during the boot's scan; the
// boot's save while the end flushes, and a remount inside the end's flush (no
// read, the folds dropped: nothing covered, nothing saved); the log's keys
// (Hm); a track's row, base ⊕ delta (Ht); stats.bin's header (H).
// ---------------------------------------------------------------------------
namespace {

const char* refused(const Player& p) { return statsglue::refusedName(p.glue->core.lastRefused()); }

void sameTrack(const sf::Track& a, const sf::Track& b, const char* w) {
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(a.hash, b.hash, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.plays, b.plays, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.skips, b.skips, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.firstAt, b.firstAt, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.lastAt, b.lastAt, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.lastOrd, b.lastOrd, w);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(a.src, b.src, w);
}

}  // namespace

void test_the_glues_own_rules() {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  const Files in = cardOf(b["in"]), want = cardOf(b["out"]);
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  // The card away at the boot's save: refused, nothing written, and the trim
  // by the MARK read at the boot (41: plays.1.jnl's 30-45 aren't all under
  // it, so nothing goes). Hs then: refused while the card is away and behind
  // the fence; with the card back, the boot's stats.bin (worked-example's
  // save doesn't compact, and the plan is the same).
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    p.begin(askOf(b));
    p.glue->away = true;
    TEST_ASSERT_TRUE(p.scan());
    TEST_ASSERT_TRUE((p.glue->calls == std::vector<std::string>{"began", "save boot (no)"}));
    TEST_ASSERT_EQUAL_STRING("cardAway", refused(p));
    TEST_ASSERT_EQUAL_UINT32(7, p.glue->fold.folded());
    TEST_ASSERT_TRUE(p.trimmed().empty());
    sameFiles(in, filesOf(card), "the card away");
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("cardAway", refused(p));
    p.glue->away = false;
    TEST_ASSERT_FALSE(p.glue->core.hs(true));
    TEST_ASSERT_EQUAL_STRING("fenced", refused(p));
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    TEST_ASSERT_EQUAL_STRING("Hs", statsglue::kindName(p.glue->core.lastKind()));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    TEST_ASSERT_TRUE(p.glue->hs());  // (nothing new: nothing written)
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(p.glue->last.what));
    TEST_ASSERT_EQUAL_UINT32(1, p.glue->core.savesWritten());
    // stats.bin's header, as H reads it.
    const statsglue::Glue::FileLook l = p.glue->core.lookFile();
    TEST_ASSERT_TRUE(l.there);
    TEST_ASSERT_FALSE(l.unread);
    TEST_ASSERT_EQUAL_UINT32(sx::kMagic, l.magic);
    TEST_ASSERT_EQUAL_UINT32(1, l.major);
    TEST_ASSERT_EQUAL_UINT32(0, l.minor);
    TEST_ASSERT_EQUAL_UINT32(want.at("stats.bin").size(), l.bytes);
    TEST_ASSERT_EQUAL_UINT32(p.glue->store.cardCopy().generation, l.generation);
  }
  // Hs during the boot's scan: refused, and the boot goes on as the
  // fixture's.
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    p.begin(askOf(b));
    p.glue->reserve(1);
    TEST_ASSERT_FALSE(p.j->scan(1));
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("booting", refused(p));
    TEST_ASSERT_TRUE(p.scan());
    sameOutcome(b, outcomeOf(p), "Hs during the boot's scan");
    sameFiles(want, filesOf(card), "Hs during the boot's scan");
  }
  // The end's flush finishing the boot's scan: its save refused (the 3 s
  // are the plays'), the trim by the MARK read at the boot. Then a remount
  // inside the flush: no read, the folds dropped; the scan again folds
  // nothing, saves nothing, and covers nothing (nothing trimmed).
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    p.begin(askOf(b));
    p.glue->ending(1000000);
    TEST_ASSERT_TRUE(p.scan());
    TEST_ASSERT_TRUE((p.glue->calls == std::vector<std::string>{"began", "save boot (no)"}));
    TEST_ASSERT_EQUAL_STRING("ending", refused(p));
    TEST_ASSERT_EQUAL_UINT32(7, p.glue->fold.folded());
    TEST_ASSERT_TRUE(p.glue->core.on());
    sameFiles(in, filesOf(card), "the boot's save while ending");
    p.j->remounted();  // (Ready, no slot yet: the scan again)
    TEST_ASSERT_TRUE(p.glue->core.dropped());
    TEST_ASSERT_FALSE(p.glue->core.on());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.folded());
    TEST_ASSERT_TRUE(p.scan());
    TEST_ASSERT_TRUE(
        (p.glue->calls == std::vector<std::string>{"began", "save boot (no)", "began again", "save boot (no)"}));
    TEST_ASSERT_EQUAL_STRING("off", refused(p));
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.folded());
    TEST_ASSERT_TRUE(p.trimmed().empty());
    sameFiles(in, filesOf(card), "a remount inside the end's flush");
    p.glue->notEnding();
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("off", refused(p));
  }
  // A rotation's save in the end's flush, at the edge of its budget
  // (kEndSaveMs, 5.5): a millisecond under 1 s left, refused (nothing
  // written); 1 s left, saved. The sessions' flush-in-time and flush-late
  // leave 3 s and 0.5 s: only this pins the edge.
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    TEST_ASSERT_TRUE(p.boot(askOf(b)));
    PlaySpec ps;
    ps.hash = 0x0123456789abcdefull;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + 313ull * sf::kDayMs;
    ps.up = 5000;
    TEST_ASSERT_TRUE(p.write(ps) == pl::Outbox::Did::Written);
    TEST_ASSERT_TRUE(p.glue->fold.dirty());
    const Bytes before = bytesOf(card, "stats.bin");
    p.glue->ending(p.oenv.now + statsglue::kEndSaveMs - 1);
    TEST_ASSERT_FALSE(p.glue->core.save(pl::Folds::Why::Rotation));
    TEST_ASSERT_EQUAL_STRING("budget", refused(p));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == before);
    p.glue->ending(p.oenv.now + statsglue::kEndSaveMs);
    TEST_ASSERT_TRUE(p.glue->core.save(pl::Folds::Why::Rotation));
    TEST_ASSERT_EQUAL_STRING("none", refused(p));
    TEST_ASSERT_EQUAL_STRING("rotation", statsglue::kindName(p.glue->core.lastKind()));
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    TEST_ASSERT_FALSE(bytesOf(card, "stats.bin") == before);
    // (The boot's save in the end's flush: refused whatever is left.)
    p.glue->ending(p.oenv.now + 60000);
    TEST_ASSERT_FALSE(p.glue->core.save(pl::Folds::Why::Boot));
    TEST_ASSERT_EQUAL_STRING("ending", refused(p));
    p.glue->notEnding();
  }
  // The log's keys (Hm): each (deviceId, epoch) the boot's scan met in the
  // log's play slots, its highest seq and its slots; MARK's rows as Hm lists
  // them.
  {
    const minijson::Value& td = scenario("two-devices");
    const minijson::Value& tb = td["boots"][0];
    const Files tin = cardOf(tb["in"]);
    std::map<std::pair<std::string, uint64_t>, std::pair<uint32_t, uint32_t>> log;
    for (const auto& f : tin) {
      if (f.first.compare(0, 6, "plays.") != 0) continue;
      for (size_t at = 0; at + pl::kSlot <= f.second.size(); at += pl::kSlot) {
        Slot s;
        if (pl::decode(reinterpret_cast<const uint8_t*>(f.second.data()) + at, pl::kSlot, &s) != pl::Verdict::Ok)
          continue;
        if (s.kind != pl::kKindPlay || s.seq == 0) continue;
        auto& e = log[{std::string(reinterpret_cast<const char*>(s.deviceId), 6), s.epoch}];
        e.first = std::max(e.first, s.seq);
        ++e.second;
      }
    }
    CutFs card;
    put(card, tin);
    Player p(card, dev);
    deviceFor(p, td);
    TEST_ASSERT_TRUE(p.boot(askOf(tb)));
    const statsglue::Glue& g = p.glue->core;
    TEST_ASSERT_TRUE(log.size() >= 2);
    TEST_ASSERT_EQUAL_UINT32(log.size(), g.logKeys());
    TEST_ASSERT_EQUAL_UINT32(0, g.logKeysMore());
    for (uint32_t i = 0; i < g.logKeys(); ++i) {
      const statsglue::LogKey& k = g.logKey(i);
      const auto it = log.find({std::string(reinterpret_cast<const char*>(k.dev), 6), k.epoch});
      TEST_ASSERT_TRUE(it != log.end());
      TEST_ASSERT_EQUAL_UINT32(it->second.first, k.seq);
      TEST_ASSERT_EQUAL_UINT32(it->second.second, k.slots);
    }
    TEST_ASSERT_TRUE(g.fold().rows(sf::kMark) >= 2);
    for (uint32_t i = 0; i < g.fold().rows(sf::kMark); ++i) {
      const sf::Fold::MarkRow* m = g.fold().markRow(i);
      TEST_ASSERT_NOT_NULL(m);
      TEST_ASSERT_EQUAL_UINT32(m->seq, g.fold().markOf(m->dev, m->epoch));
      TEST_ASSERT_EQUAL_UINT32(m->seq, m->card);  // (saved: the card's MARK is RAM's)
    }
    TEST_ASSERT_NULL(g.fold().markRow(g.fold().rows(sf::kMark)));
  }
  // A track's row, base ⊕ delta (Ht): compaction's boot leaves every row in
  // its new base (TRKD empty); a live play of one of them merges with it.
  {
    const minijson::Value& cs = scenario("compaction");
    const minijson::Value& cb = cs["boots"][0];
    CutFs card;
    put(card, cardOf(cb["in"]));
    Player p(card, dev);
    deviceFor(p, cs);
    TEST_ASSERT_TRUE(p.boot(askOf(cb)));
    TEST_ASSERT_TRUE(p.glue->last.compacted);
    char name[sx::kNameCap];
    TEST_ASSERT_TRUE(sx::baseName(p.glue->fold.head().baseGen, name, sizeof(name)));
    const Bytes base = bytesOf(card, std::string(name).substr(kDir.size()));
    const uint32_t rows = p.glue->fold.head().baseRows;
    TEST_ASSERT_TRUE(rows >= 2);
    TEST_ASSERT_EQUAL_UINT32(sx::kBaseRowsAt + rows * sf::kTrackBytes, base.size());
    const auto row = [&](uint32_t i) {
      return sf::decodeTrack(reinterpret_cast<const uint8_t*>(base.data()) + sx::kBaseRowsAt + i * sf::kTrackBytes);
    };
    for (uint32_t i : {0u, rows / 2, rows - 1}) {
      const statsglue::TrackLook t = p.glue->core.track(row(i).hash);
      TEST_ASSERT_TRUE(t.base == statsglue::TrackLook::Base::Found);
      TEST_ASSERT_FALSE(t.delta);
      sameTrack(row(i), t.row, "a base row");
      TEST_ASSERT_TRUE(t.reads >= 1);
    }
    const statsglue::TrackLook none = p.glue->core.track(0x0123456789abcdefull);
    TEST_ASSERT_TRUE(none.base == statsglue::TrackLook::Base::NotThere);
    TEST_ASSERT_FALSE(none.delta);
    TEST_ASSERT_EQUAL_UINT64(0, none.row.hash);
    // A live play of the first row's track: base ⊕ delta.
    PlaySpec ps;
    ps.hash = row(0).hash;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + 300ull * sf::kDayMs;
    ps.up = 5000;
    TEST_ASSERT_TRUE(p.write(ps) == pl::Outbox::Did::Written);
    const statsglue::TrackLook t = p.glue->core.track(ps.hash);
    TEST_ASSERT_TRUE(t.base == statsglue::TrackLook::Base::Found);
    TEST_ASSERT_TRUE(t.delta);
    TEST_ASSERT_EQUAL_UINT32(1, t.deltaRow.plays);
    sameTrack(sf::merge(row(0), t.deltaRow), t.row, "base and delta");
    TEST_ASSERT_EQUAL_UINT32(row(0).plays + 1u, t.row.plays);
  }
}

// Hs after a partial scan says why (the store would refuse it too: the
// fold's plan), and Hs never compacts, not even where a boot's save would
// (no base yet, nothing playing): the rotation's path.
void test_hs_says_why_and_never_compacts() {
  {
    const minijson::Value& sc = scenario("partial-tail");
    const minijson::Value& b = sc["boots"][0];
    CutFs card;
    put(card, cardOf(b["in"]));
    ScanFaults faults(card);
    Player p(card, reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1"));
    deviceFor(p, sc);
    sameOutcome(b, fixtureBoot(p, faults, b), "partial-tail, boot 1");
    TEST_ASSERT_TRUE(p.glue->core.partialSeen());
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("partial", refused(p));
    sameFiles(cardOf(b["out"]), filesOf(card), "partial-tail, Hs");
  }
  {
    const minijson::Value& sc = scenario("first-at-a");
    const minijson::Value& b = sc["boots"][0];
    CutFs card;
    put(card, cardOf(b["in"]));
    ScanFaults faults(card);
    Player p(card, reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1"));
    deviceFor(p, sc);
    sameOutcome(b, fixtureBoot(p, faults, b), "first-at-a");
    TEST_ASSERT_FALSE(p.glue->last.compacted);
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.head().baseGen);
    PlaySpec ps;
    ps.hash = 0x0123456789abcdefull;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + 310ull * sf::kDayMs;
    ps.up = 5000;
    TEST_ASSERT_TRUE(p.write(ps) == pl::Outbox::Did::Written);
    p.glue->ask.playing = false;
    p.glue->ask.safePoint = true;
    // (A boot's save now would compact: no base, nothing playing.)
    TEST_ASSERT_TRUE(sf::compactionDue(p.glue->fold.rows(sf::kTrkd), 0, false, true));
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    TEST_ASSERT_FALSE(p.glue->last.compacted);
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.head().baseGen);
    for (const auto& f : filesOf(card)) TEST_ASSERT_FALSE_MESSAGE(f.first.compare(0, 6, "stats-") == 0, f.first.c_str());
  }
}

// The boot's fold's blocks (its ranges, ACKS rows, indexes), and a
// remount's read of stats.bin (its work memory, the load's chunks), wait
// for the scan's first reserve(): a pass that may carve (5.6: the join not
// Working, the write gate open), never setup()'s, nor the remount's own
// pass (the card guard's). A scan that asks no reserve() (the end's flush
// finishing it) begins the fold at its first call. A remount's read still
// to make when the end flushes is never made: the folds dropped.
void test_the_folds_blocks_wait_for_a_reserve() {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  const Files in = cardOf(b["in"]), want = cardOf(b["out"]);
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    p.alloc = heapAlloc;
    p.release = heapFree;
    uint32_t afterRead = 0;
    p.afterRead = [&afterRead] { afterRead = gHeap.carves; };
    p.begin(askOf(b));
    // The journal's begin() (scanBegan()): nothing carved for the stats.
    TEST_ASSERT_EQUAL_UINT32(afterRead, gHeap.carves);
    TEST_ASSERT_TRUE(p.glue->core.booting());
    TEST_ASSERT_TRUE(p.glue->core.wantsRoom());
    TEST_ASSERT_FALSE(p.glue->fold.booting());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.indexSize(sf::kTrkd));
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("booting", refused(p));
    p.glue->reserve(1);
    TEST_ASSERT_TRUE(p.glue->fold.booting());
    TEST_ASSERT_TRUE(p.glue->fold.indexSize(sf::kTrkd) > 0);
    TEST_ASSERT_TRUE(gHeap.carves > afterRead);
    TEST_ASSERT_TRUE(p.scan());
    sameOutcome(b, outcomeOf(p), "the boot's fold at its first reserve()");
    sameFiles(want, filesOf(card), "the boot's fold at its first reserve()");
    TEST_ASSERT_FALSE(p.glue->core.wantsRoom());
    // A remount after Ready, before the first slot: the folds dropped at
    // once; no read, no carve, nothing covered until the scan's next piece.
    const uint64_t read0 = card.bytesRead;
    const uint32_t carves0 = gHeap.carves;
    const size_t calls0 = p.glue->calls.size();
    p.j->remounted();
    TEST_ASSERT_TRUE(p.glue->core.rereadPending());
    TEST_ASSERT_TRUE(p.glue->core.wantsRoom());
    TEST_ASSERT_TRUE(p.glue->core.booting());
    TEST_ASSERT_FALSE(p.glue->core.on());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_UINT64(read0, card.bytesRead);
    TEST_ASSERT_EQUAL_UINT32(carves0, gHeap.carves);
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.folded());
    TEST_ASSERT_EQUAL_size_t(calls0, p.glue->calls.size());  // (began(): at the read)
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("booting", refused(p));
    p.glue->reserve(1);
    TEST_ASSERT_EQUAL_UINT32(1, p.glue->core.rereads());
    TEST_ASSERT_TRUE(card.bytesRead > read0);
    TEST_ASSERT_TRUE(gHeap.carves > carves0);
    TEST_ASSERT_TRUE(p.glue->core.on());
    TEST_ASSERT_TRUE(p.glue->fold.booting());
    TEST_ASSERT_EQUAL_STRING("began again", p.glue->calls.back().c_str());
    TEST_ASSERT_TRUE(p.scan());
    TEST_ASSERT_EQUAL_STRING("save boot", p.glue->calls.back().c_str());
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(p.glue->last.what));
    sameFiles(want, filesOf(card), "a remount after Ready");
  }
  // A scan no reserve() came before: the fold begins at its first slot.
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    p.begin(askOf(b));
    int pieces = 0;
    while (!p.j->scan(3)) ++pieces;
    TEST_ASSERT_TRUE(pieces > 0);
    p.glue->ready();
    sameOutcome(b, outcomeOf(p), "a scan with no reserve()");
    sameFiles(want, filesOf(card), "a scan with no reserve()");
  }
  // A remount's read still to make when the end flushes: never made.
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    TEST_ASSERT_TRUE(p.boot(askOf(b)));
    p.ops.clear();  // (the boot's trim)
    p.j->remounted();
    p.glue->ending(1000000);
    TEST_ASSERT_TRUE(p.glue->core.dropped());
    TEST_ASSERT_FALSE(p.glue->core.on());
    TEST_ASSERT_FALSE(p.glue->core.rereadPending());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_STRING("began again", p.glue->calls.back().c_str());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->fold.held());
    TEST_ASSERT_TRUE(p.scan());  // (the flush finishing the scan: no read)
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_STRING("save boot (no)", p.glue->calls.back().c_str());
    TEST_ASSERT_EQUAL_STRING("off", refused(p));
    TEST_ASSERT_TRUE(p.trimmed().empty());
    sameFiles(want, filesOf(card), "a remount's read when the end flushes");
  }
}

// A fold out of memory from the boot's first fold (its ranges couldn't be
// carved) and no work memory for the save either (the same short heap): no
// save, and the trim by the MARK read at the boot (5.5), never as after a
// save. worked-example's whole boot trims plays.1.jnl (ACKS 45 covers it);
// its MARK, 41, doesn't cover plays 42-45, never folded: it stays. Over a
// loaded stats.bin and over none (no MARK: nothing covered).
namespace {
bool gShortHeap = false;  // the heap has nothing left
void* shortAlloc(size_t n) { return gShortHeap ? nullptr : std::malloc(n); }
void shortFree(void* p) { std::free(p); }
}  // namespace

void test_a_fold_out_of_memory_trims_by_the_boots_mark() {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  TEST_ASSERT_EQUAL_size_t(1, b["trimmed"].size());  // (the whole boot's)
  for (int absent = 0; absent < 2; ++absent) {
    const char* w = absent ? "no stats.bin" : "stats.bin loaded";
    Files in = cardOf(b["in"]);
    if (absent) in.erase("stats.bin");
    CutFs card;
    put(card, in);
    Player p(card, reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1"));
    deviceFor(p, sc);
    p.alloc = shortAlloc;
    p.release = shortFree;
    p.afterRead = [] { gShortHeap = true; };
    p.begin(askOf(b));
    TEST_ASSERT_TRUE_MESSAGE(p.glue->core.on(), w);
    const bool ready = p.scan();
    gShortHeap = false;
    TEST_ASSERT_TRUE_MESSAGE(ready, w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->fold.noMemory(), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.glue->fold.folded(), w);
    std::string calls;
    for (const std::string& c : p.glue->calls) calls += " " + c;
    TEST_ASSERT_TRUE_MESSAGE((p.glue->calls == std::vector<std::string>{"began", "save boot (no)"}),
                             (std::string(w) + ":" + calls).c_str());
    TEST_ASSERT_FALSE_MESSAGE(sx::savedOk(p.glue->last.what), w);
    TEST_ASSERT_TRUE_MESSAGE(p.trimmed().empty(), w);
    sameFiles(in, filesOf(card), w);
  }
}

// ---------------------------------------------------------------------------
// A library update's give-back (docs/LISTENING-STATS.md 6.7, the owner's
// decision of 2026-11-11): the sessions' update-* steps (statsref's
// giveback and takeback), pinned here; then what only the firmware has: a
// cut at every step of the give-back's save, the re-read failing, a
// remount's scan begun again while given back, a held tmp, and the glue's
// own rules around it.
// ---------------------------------------------------------------------------
namespace {

// worked-example's boot (its save lands: MARK 48), then a live play (49):
// the fold holds a play the card's stats.bin doesn't.
struct Worked {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  Files in = cardOf(b["in"]);
  static PlaySpec play(uint32_t i) {
    PlaySpec ps;
    ps.hash = 0x0123456789abcdefull + i;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + 313ull * sf::kDayMs + i * 60000ull;
    ps.up = 5000 + i * 300000;
    return ps;
  }
  BootAsk ask() const {
    BootAsk a = askOf(b);
    a.ask.playing = true;  // (no compaction: the counts compared, not a base)
    a.ask.safePoint = false;
    return a;
  }
  void boot(Player& p) const {
    deviceFor(p, sc);
    TEST_ASSERT_TRUE(p.boot(ask()));
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  }
};

// The counts a card's stats hold, read as a boot reads them: HEAD (but its
// round), MARK, and TRKD's rows (but lastOrd: a live play's round is
// HEAD.round + 2, a boot's + 1).
struct Counts {
  uint64_t plays = 0, skips = 0, slots = 0, listenedMs = 0;
  std::map<std::pair<std::string, uint64_t>, uint32_t> mark;
  std::map<uint64_t, std::vector<uint32_t>> tracks;
  bool operator==(const Counts& o) const {
    return plays == o.plays && skips == o.skips && slots == o.slots && listenedMs == o.listenedMs && mark == o.mark &&
           tracks == o.tracks;
  }
};
Counts countsOf(CutFs& card) {
  sf::Fold f;
  TEST_ASSERT_TRUE(f.begin());
  sx::Store store(card);
  Work work;
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(store.read(f, work.p(), nullptr).read));
  Counts c;
  c.plays = f.head().plays;
  c.skips = f.head().skips;
  c.slots = f.head().slots;
  c.listenedMs = f.head().listenedMs;
  for (uint32_t i = 0; i < f.rows(sf::kMark); ++i) {
    const sf::Fold::MarkRow* m = f.markRow(i);
    c.mark[{std::string(reinterpret_cast<const char*>(m->dev), 6), m->epoch}] = m->seq;
  }
  for (const auto& t : tracksOf(bytesOf(card, "stats.bin")))
    c.tracks[t.first] = {t.second.plays, t.second.skips, t.second.firstAt, t.second.lastAt, t.second.src};
  return c;
}

}  // namespace

void test_an_updates_give_back() {
  // The sessions (statsref's), each step through the glue: their facts.
  {
    TEST_ASSERT_EQUAL_UINT64(statsglue::kRing + 3, runSession(session("update-ring-full")).slots);
    const minijson::Value& ur = session("update-rotation-unsaved");
    TEST_ASSERT_EQUAL_UINT64(4, ur["lost"].u64());
    TEST_ASSERT_EQUAL_UINT64(5, runSession(ur).slots);  // (5-9: 1-4 deleted by the rotation while given back)
    TEST_ASSERT_EQUAL_UINT64(9, runSession(session("update-rotation-saved")).slots);
    TEST_ASSERT_EQUAL_UINT64(9, runSession(session("update-ending")).slots);
    TEST_ASSERT_EQUAL_UINT64(6, runSession(session("update-other-player")).slots);
    TEST_ASSERT_EQUAL_UINT64(6, runSession(session("update-save-fails")).slots);
    TEST_ASSERT_EQUAL_UINT64(5, runSession(session("update-plays")).slots);
    TEST_ASSERT_EQUAL_UINT64(3, runSession(session("update-quiet")).slots);
  }
  const Worked wk;
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  // The rules: what is given back and kept; no give-back while the boot's
  // fold runs, twice, with the stats off, or while the end flushes; Hs and
  // the saves refused while given back; the re-read on the same copy.
  {
    CutFs card;
    put(card, wk.in);
    Player p(card, dev);
    deviceFor(p, wk.sc);
    p.begin(wk.ask());
    p.glue->reserve(1);
    TEST_ASSERT_FALSE(p.j->scan(1));
    TEST_ASSERT_EQUAL_size_t(0, p.glue->core.resident());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->core.giveBack());  // (the boot's fold: its blocks are the scan's)
    TEST_ASSERT_FALSE(p.glue->core.givenBack());
    TEST_ASSERT_TRUE(p.scan());
    sameFiles(cardOf(wk.b["out"]), filesOf(card), "a give-back asked during the boot's scan");
    TEST_ASSERT_TRUE(p.write(Worked::play(1)) == pl::Outbox::Did::Written);
    TEST_ASSERT_TRUE(p.glue->fold.dirty());
    const size_t resident = p.glue->core.resident();
    TEST_ASSERT_EQUAL_size_t(p.glue->fold.givesBack(), resident);
    TEST_ASSERT_EQUAL_size_t(p.glue->fold.held() - sf::kMarkBlockRows * sizeof(sf::Fold::MarkRow), resident);
    const Bytes before = bytesOf(card, "stats.bin");
    const size_t calls = p.glue->calls.size();
    const size_t bytes = p.glue->core.giveBack();
    // The save first (49 isn't in stats.bin), the rotation's path.
    TEST_ASSERT_EQUAL_size_t(calls + 1, p.glue->calls.size());
    TEST_ASSERT_EQUAL_STRING("save give-back", p.glue->calls.back().c_str());
    TEST_ASSERT_FALSE(p.glue->last.compacted);
    TEST_ASSERT_FALSE(bytesOf(card, "stats.bin") == before);
    const statsglue::GaveBack& gb = p.glue->core.gaveBack();
    TEST_ASSERT_TRUE(gb.saveAsked && gb.covers);
    TEST_ASSERT_EQUAL_STRING("none", statsglue::refusedName(gb.refused));
    TEST_ASSERT_TRUE(bytes > 0 && bytes <= resident);  // (the save's retention may give a chunk back first)
    TEST_ASSERT_EQUAL_size_t(sf::kMarkBlockRows * sizeof(sf::Fold::MarkRow), gb.kept);
    TEST_ASSERT_EQUAL_size_t(gb.kept, p.glue->fold.held());
    TEST_ASSERT_EQUAL_UINT32(1, gb.markRows);
    TEST_ASSERT_TRUE(p.glue->core.givenBack());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->core.resident());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->core.giveBack());  // (given back already)
    // covered() by the card's MARK (49 now), markOf() RAM's.
    Slot s;
    s.kind = pl::kKindPlay;
    std::memcpy(s.deviceId, p.id, 6);
    s.epoch = p.j->epoch();
    s.seq = 49;
    TEST_ASSERT_TRUE(p.glue->core.covered(s));
    s.seq = 50;
    TEST_ASSERT_FALSE(p.glue->core.covered(s));
    TEST_ASSERT_EQUAL_UINT32(49, p.glue->core.markOf(p.id, p.j->epoch()));
    // Saves refused, Hs too, with their own reason.
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("givenBack", refused(p));
    TEST_ASSERT_FALSE(p.glue->core.save(pl::Folds::Why::Rotation));
    TEST_ASSERT_EQUAL_STRING("givenBack", refused(p));
    // Plays while given back: the ring (nothing carved: the fold holds MARK's block alone).
    TEST_ASSERT_TRUE(p.write(Worked::play(2)) == pl::Outbox::Did::Written);
    TEST_ASSERT_TRUE(p.write(Worked::play(3)) == pl::Outbox::Did::Written);
    TEST_ASSERT_EQUAL_UINT32(2, p.glue->core.ringCount());
    TEST_ASSERT_EQUAL_size_t(gb.kept, p.glue->fold.held());
    TEST_ASSERT_FALSE(p.glue->fold.noMemory());
    // No re-read while the end flushes.
    p.glue->ending(p.oenv.now + 3000);
    p.glue->core.takeBack();
    TEST_ASSERT_TRUE(p.glue->core.givenBack());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->core.giveBack());
    p.glue->notEnding();
    // The re-read: the copy the give-back wrote; 50 and 51 folded.
    const uint32_t rereads = p.glue->core.rereads();
    const statsglue::TookBack& t = p.glue->core.takeBack();
    TEST_ASSERT_TRUE(t.read && t.on && t.same && !t.behind && !t.rescan);
    TEST_ASSERT_EQUAL_UINT32(2, t.folded);
    TEST_ASSERT_EQUAL_UINT32(0, t.covered);
    TEST_ASSERT_EQUAL_UINT32(rereads + 1, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(p.glue->opened.read));
    TEST_ASSERT_EQUAL_UINT32(51, p.glue->core.markOf(p.id, p.j->epoch()));
    TEST_ASSERT_TRUE(p.glue->fold.held() > gb.kept);
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    // The next boot: 42-51, each once (49-51 in stats.bin: nothing folded).
    Player q(card, p.id);
    deviceFor(q, wk.sc);
    q.nvs = p.nvs;
    TEST_ASSERT_TRUE(q.boot(wk.ask()));
    TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
    TEST_ASSERT_EQUAL_UINT64(p.glue->fold.head().slots, q.glue->fold.head().slots);
  }
  // A fold out of memory, the card away at the give-back, a held tmp: the
  // re-read's verdict on the session (behind, or not).
  for (int c = 0; c < 4; ++c) {
    const char* const names[] = {"the card away at the give-back", "the give-back save's tmp refused",
                                 "the give-back save's rename refused (held)", "the boot's fold out of memory"};
    const char* w = names[c];
    CutFs card;
    put(card, wk.in);
    FaultFs fs(card);
    Player p(card, dev);
    p.fs = &fs;
    if (c == 3) {
      // (worked-example's boot with no memory from its fold's first block:
      // nothing folded, no save; 49 written, not folded.)
      deviceFor(p, wk.sc);
      p.alloc = shortAlloc;
      p.release = shortFree;
      p.afterRead = [] { gShortHeap = true; };
      TEST_ASSERT_TRUE_MESSAGE(p.boot(wk.ask()), w);
      gShortHeap = false;
      TEST_ASSERT_TRUE_MESSAGE(p.glue->fold.noMemory(), w);
    } else {
      wk.boot(p);
    }
    TEST_ASSERT_TRUE_MESSAGE(p.write(Worked::play(1)) == pl::Outbox::Did::Written, w);
    if (c == 0) p.glue->away = true;
    if (c == 1) fs.noCreate.fail = 1;
    if (c == 2) fs.noRename.fail = 1;
    TEST_ASSERT_TRUE_MESSAGE(p.glue->core.giveBack() > 0, w);
    p.glue->away = false;
    fs.noCreate = fs.noRename = faultfs::Knob();
    const statsglue::GaveBack& gb = p.glue->core.gaveBack();
    TEST_ASSERT_EQUAL_MESSAGE(c != 3, gb.saveAsked, w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c == 0 ? "cardAway" : "none", statsglue::refusedName(gb.refused), w);
    if (c == 1) TEST_ASSERT_EQUAL_STRING_MESSAGE("failed", sx::savedName(p.glue->last.what), w);
    if (c == 2) TEST_ASSERT_EQUAL_STRING_MESSAGE("held", sx::savedName(p.glue->last.what), w);
    TEST_ASSERT_EQUAL_MESSAGE(c == 2, gb.covers, w);
    TEST_ASSERT_TRUE_MESSAGE(p.write(Worked::play(2)) == pl::Outbox::Did::Written, w);
    const statsglue::TookBack& t = p.glue->core.takeBack();
    TEST_ASSERT_TRUE_MESSAGE(t.read && t.on, w);
    TEST_ASSERT_EQUAL_MESSAGE(c == 2, t.same, w);
    TEST_ASSERT_EQUAL_MESSAGE(c == 2, p.glue->opened.heldPromoted, w);
    TEST_ASSERT_EQUAL_MESSAGE(c != 2, p.glue->core.behind(), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, t.folded, w);
    TEST_ASSERT_FALSE_MESSAGE(p.glue->fold.noMemory(), w);
    TEST_ASSERT_EQUAL_MESSAGE(c == 2, p.glue->hs(), w);
    if (c != 2) TEST_ASSERT_EQUAL_STRING_MESSAGE("behind", refused(p), w);
    TEST_ASSERT_EQUAL_MESSAGE(c == 2, p.glue->core.save(pl::Folds::Why::Rotation), w);
    // The next boot folds what the card's stats.bin doesn't hold, each
    // once: 49-50 (50 too after a held tmp: Hs saved it; and 42-48 after the
    // boot's fold that ran out of memory).
    Player q(card, p.id);
    deviceFor(q, wk.sc);
    q.nvs = p.nvs;
    TEST_ASSERT_TRUE_MESSAGE(q.boot(wk.ask()), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c == 2 ? 0 : c == 3 ? 9 : 2, q.glue->fold.folded(), w);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(wk.b["stats"]["head"]["slots"].u64() + 2, q.glue->fold.head().slots, w);
  }
  // The re-read failing (stats.bin's open, each time): the stats off for
  // the session, every block given back, the ring dropped; nothing trimmed
  // or saved after; the card answering again, the next boot folds it all.
  {
    CutFs card;
    put(card, wk.in);
    Player p(card, dev);
    wk.boot(p);
    TEST_ASSERT_TRUE(p.write(Worked::play(1)) == pl::Outbox::Did::Written);
    TEST_ASSERT_TRUE(p.glue->core.giveBack() > 0);
    TEST_ASSERT_TRUE(p.write(Worked::play(2)) == pl::Outbox::Did::Written);
    card.faultPrefix = sx::kStatsBin;
    card.faultFrom = 0;
    const statsglue::TookBack& t = p.glue->core.takeBack();
    card.faultFrom = -1;
    TEST_ASSERT_TRUE(t.read);
    TEST_ASSERT_FALSE(t.on);
    TEST_ASSERT_EQUAL_UINT32(1, t.dropped);  // (50: the next boot's)
    TEST_ASSERT_EQUAL_UINT32(0, t.folded);
    TEST_ASSERT_EQUAL_STRING("unread", sx::readName(p.glue->opened.read));
    TEST_ASSERT_FALSE(p.glue->core.on());
    TEST_ASSERT_FALSE(p.glue->core.givenBack());
    TEST_ASSERT_EQUAL_size_t(0, p.glue->fold.held());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.ringCount());
    Slot s;
    s.kind = pl::kKindPlay;
    std::memcpy(s.deviceId, p.id, 6);
    s.epoch = p.j->epoch();
    s.seq = 1;
    TEST_ASSERT_FALSE(p.glue->core.covered(s));  // (nothing covered: nothing trimmed)
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("off", refused(p));
    TEST_ASSERT_TRUE(p.write(Worked::play(3)) == pl::Outbox::Did::Written);
    Player q(card, p.id);
    deviceFor(q, wk.sc);
    q.nvs = p.nvs;
    TEST_ASSERT_TRUE(q.boot(wk.ask()));
    TEST_ASSERT_EQUAL_UINT32(2, q.glue->fold.folded());  // (50, 51: 49 the give-back saved)
  }
  // A remount while given back, Ready before the boot's first slot: the
  // scan begun again waits for the re-read (no read, no carve, no reserve),
  // then reads at its first reserve(), its boot's fold the log again.
  {
    CutFs card;
    put(card, wk.in);
    Player p(card, dev);
    p.alloc = heapAlloc;
    p.release = heapFree;
    wk.boot(p);
    p.ops.clear();
    TEST_ASSERT_TRUE(p.glue->core.giveBack() > 0);
    TEST_ASSERT_FALSE(p.glue->core.gaveBack().saveAsked);  // (nothing new)
    const uint64_t read0 = card.bytesRead;
    const uint32_t carves0 = gHeap.carves;
    p.j->remounted();  // (no slot yet: the scan again)
    TEST_ASSERT_TRUE(p.glue->core.rereadPending());
    TEST_ASSERT_TRUE(p.glue->core.wantsRoom());
    TEST_ASSERT_FALSE(p.glue->core.reserve(1));
    TEST_ASSERT_EQUAL_UINT64(read0, card.bytesRead);
    TEST_ASSERT_EQUAL_UINT32(carves0, gHeap.carves);
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("givenBack", refused(p));
    const statsglue::TookBack& t = p.glue->core.takeBack();
    TEST_ASSERT_TRUE(t.rescan);
    TEST_ASSERT_FALSE(t.read);
    TEST_ASSERT_FALSE(p.glue->core.givenBack());
    TEST_ASSERT_EQUAL_UINT64(read0, card.bytesRead);
    TEST_ASSERT_TRUE(p.scan());
    TEST_ASSERT_TRUE((p.glue->calls ==
                      std::vector<std::string>{"began", "save boot", "hs (no)", "began again", "save boot"}));
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(p.glue->last.what));
    TEST_ASSERT_TRUE(p.glue->core.on());
    TEST_ASSERT_TRUE(p.glue->fold.held() > p.glue->core.gaveBack().kept);
    sameFiles(cardOf(wk.b["out"]), filesOf(card), "a remount's scan while given back");
  }
}

// A cut at every write step of the give-back's save, three ways, then a
// boot: the counts the uncut give-back's next boot holds, each play once
// (stats.bin the boot's, the next boot folding 49 again; or the give-back's,
// folding nothing). The power goes with the cut: the session ends there.
void test_a_cut_during_the_give_backs_save() {
  const Worked wk;
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  // The boot, 49 written live, the give-back (its save cut at its step
  // `cutAt`; 0: none); NVS as the session left it.
  auto session = [&](CutFs& card, long cutAt) {
    Player p(card, dev);
    wk.boot(p);
    TEST_ASSERT_TRUE(p.write(Worked::play(1)) == pl::Outbox::Did::Written);
    if (cutAt > 0) p.glue->beforeSave = [&card, cutAt] { card.cutAt = card.steps + cutAt; };
    p.glue->core.giveBack();
    p.glue->beforeSave = nullptr;
    if (!card.dead) TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    return p.nvs;
  };
  auto next = [&](CutFs& card, const MemNvs& nvs, const char* w, uint32_t* folded) {
    Player q(card, dev);
    deviceFor(q, wk.sc);
    q.nvs = nvs;
    TEST_ASSERT_TRUE_MESSAGE(q.boot(wk.ask()), w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("loaded", sx::readName(q.glue->first.read), w);
    *folded = q.glue->fold.folded();
  };
  CutFs ref;
  put(ref, wk.in);
  uint32_t folded = 0;
  next(ref, session(ref, 0), "uncut", &folded);
  TEST_ASSERT_EQUAL_UINT32(0, folded);  // (49 in the give-back's stats.bin)
  const Counts want = countsOf(ref);
  TEST_ASSERT_EQUAL_UINT64(wk.b["stats"]["head"]["slots"].u64() + 1, want.slots);
  int steps = 0, runs = 0, again = 0;
  for (long k = 1;; ++k) {
    bool cut = false;
    for (cutfs::Variant v : {cutfs::Variant::InOrder, cutfs::Variant::LoseUnsynced, cutfs::Variant::Torn}) {
      CutFs card;
      put(card, wk.in);
      const MemNvs n = session(card, k);
      if (!card.dead) break;
      cut = true;
      char w[96];
      std::snprintf(w, sizeof(w), "the give-back's save cut at its step %ld, way %d", k, static_cast<int>(v));
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
      CutFs after = card.reboot(v);
      next(after, n, w, &folded);
      TEST_ASSERT_TRUE_MESSAGE(folded <= 1, w);  // (49 again, or none: the give-back's landed)
      again += static_cast<int>(folded);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, after.violations.size(), w);
      partTwins(after);
      TEST_ASSERT_TRUE_MESSAGE(countsOf(after) == want, w);
      ++runs;
    }
    if (!cut) break;
    ++steps;
  }
  printf("the give-back's save: %d steps cut three ways (%d runs), then a boot: the uncut counts each time (49 "
         "folded again after %d of them)\n",
         steps, runs, again);
  TEST_ASSERT_TRUE(steps >= 5);
  TEST_ASSERT_TRUE(again > 0 && again < runs);
}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/sessions.json", &gSessions) || gSessions.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_every_fixture_through_the_journal);
  RUN_TEST(test_a_remount_mid_scan_and_after_ready);
  RUN_TEST(test_a_cut_at_every_step_of_the_save_and_the_trim);
  RUN_TEST(test_the_card_fails_a_question_or_a_step);
  RUN_TEST(test_a_held_tmp);
  RUN_TEST(test_the_base_faulted_at_the_boot);
  RUN_TEST(test_first_at_orders_and_the_retention);
  RUN_TEST(test_every_session);
  RUN_TEST(test_the_rotation_hole);
  RUN_TEST(test_players_nvs_and_old_firmware);
  RUN_TEST(test_first_at_through_a_rotations_save);
  RUN_TEST(test_a_full_log_never_synced);
  RUN_TEST(test_the_glues_own_rules);
  RUN_TEST(test_hs_says_why_and_never_compacts);
  RUN_TEST(test_a_fold_out_of_memory_trims_by_the_boots_mark);
  RUN_TEST(test_the_folds_blocks_wait_for_a_reserve);
  RUN_TEST(test_an_updates_give_back);
  RUN_TEST(test_a_cut_during_the_give_backs_save);
  return UNITY_END();
}
