// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' store as a whole: the proof of
// docs/LISTENING-STATS.md 9.5's C8. The play log's Journal (lib/core PlayLog)
// with the real fold (StatsFold) and files (StatsFile), joined by a Folds
// shaped as app/ListeningStats will be (C9: the read before the journal's
// begin(), the save's guards, the end's flush, the live fold at the Outbox's
// written()), over a card in memory that can lose its power at any step
// (test/support/CutFs.h) or fail chosen calls (test/support/FaultFs.h).
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
};
Heap gHeap;
void* heapAlloc(size_t n) {
  void* p = std::malloc(n);
  if (!p) return nullptr;
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
// The glue: the play log's Folds over the real Fold and Store, as
// app/ListeningStats will be (docs/LISTENING-STATS.md 5.4-5.7, 9.5 C9).
// ---------------------------------------------------------------------------
struct Glue final : pl::Folds {
  Glue(tagstore::Fs& fs, CutFs& card, sf::Fold::AllocFn a, sf::Fold::FreeFn f) : fold(a, f), store(fs), card_(card) {}
  sf::Fold fold;
  sx::Store store;
  Work work;
  cc::msmf::Acks acks;      // the root's
  sx::SaveAsk ask;          // the board's clock, the root's commitTime, the update step's safe point
  bool saveFails = false;   // a full card at a save: each of its steps refused
  // The orderly end's flush (app/PlayHistory: the ending flag, the 3 s
  // budget): the boot's save refused; a rotation's with 1 s of it left.
  bool ending = false;
  std::function<uint64_t()> mono;  // the board's monotonic clock
  uint64_t until = 0;              // the flush's deadline
  std::function<void()> beforeSave;  // (a test's: a cut's set-up)
  std::vector<std::string> calls;
  std::vector<std::string> seen;  // the log's files on the card at each save the store was asked for
  sx::Opened first;   // the boot's read
  sx::Opened opened;  // the last read (a remount's)
  sx::SaveReport last;
  bool partialSeen = false;
  bool dropped = false;  // a remount inside the end's flush: the folds dropped, no save

  bool on() const { return store.on() && !dropped; }

  // PlayHistory::begin(): the read, before the journal's begin().
  sx::Opened read(sx::Store::WaitFn wait) {
    TEST_ASSERT_TRUE(fold.begin());
    first = opened = store.read(fold, work.p(), wait);
    return first;
  }
  void reserve(uint32_t slots) {
    if (on() && fold.booting()) fold.reserve(slots);
  }
  void scanBegan(bool again) override {
    calls.push_back(again ? "began again" : "began");
    partialSeen = false;
    if (again) {
      if (ending) {
        // 5.4: no read while the end flushes; the folds dropped, no save.
        dropped = true;
        fold.clear();
        return;
      }
      opened = store.read(fold, work.p(), nullptr);  // a remount's: one read, no waits
    }
    if (on()) fold.beginBoot(acks);  // (false: no memory, the fold says so: no save)
  }
  void slot(const Slot& s, int file) override {
    if (on()) fold.bootSlot(s, file);
  }
  void partial() override {
    calls.push_back("partial");
    partialSeen = true;
    if (on()) fold.partial();
  }
  bool covered(const Slot& s) override { return on() && fold.covered(s); }
  uint32_t markOf(const uint8_t dev[6], uint64_t epoch) override { return on() ? fold.markOf(dev, epoch) : 0; }
  bool save(Why why) override {
    const bool boot = why == Why::Boot;
    const std::string name = boot ? "save boot" : "save rotation";
    last = sx::SaveReport();
    bool ok = false;
    if (!on() || (boot && ending) || (!boot && ending && mono() + 1000 > until)) {
      fold.endBoot();  // (store.save() would have)
    } else {
      std::string names;
      for (const char* n : {kPrev, kLog})
        if (card_.dir.count(n)) names += (names.empty() ? "" : " ") + base(n);
      seen.push_back(names);
      if (beforeSave) beforeSave();
      ok = commit(boot);
    }
    calls.push_back(ok ? name : name + " (no)");
    return ok;
  }
  // The journal ready: a boot's fold that asked no save (a partial scan)
  // ends here.
  void ready() {
    if (fold.booting()) fold.endBoot();
  }
  // Outbox::Env::written(): the live fold.
  void written(const Slot& s) {
    if (on()) fold.liveSlot(s);
  }
  // Hs: the rotation's path (never a compaction); refused during the boot's
  // scan and after a partial scan (the store refuses the rest).
  bool hs() {
    last = sx::SaveReport();
    const bool ok = on() && !fold.booting() && !partialSeen && commit(false);
    calls.push_back(ok ? "hs" : "hs (no)");
    return ok;
  }

private:
  bool commit(bool boot) {
    ask.mayCompact = boot;
    if (saveFails) card_.refuse = true;
    last = store.save(fold, ask, work.p());
    card_.refuse = false;
    return sx::savedOk(last.what);
  }
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
    c.folds = glue.get();
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
    gWaits.clear();
    p.boot(askOf(b));
    const char* w = from == 0 ? "the base's open" : "the base's header";
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
        g->ending = true;
        g->until = p->oenv.now + 3000;
        p->oenv.perStat = 3000 - st["left"].u64();
        p->ended(spec);
        const pl::Outbox::Flushed f = p->out->flush(3000, 128);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, f.written, w);
        g->ending = false;
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
  TEST_ASSERT_EQUAL_UINT32(15, gSessions.size());
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
  return UNITY_END();
}
