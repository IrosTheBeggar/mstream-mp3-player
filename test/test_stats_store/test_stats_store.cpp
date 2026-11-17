// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the listening stats' store as a whole: the proof of
// docs/LISTENING-STATS.md 9.5's C8, as S1's rework (5.14, R5) has it. The
// play log's Journal (lib/core PlayLog) with the real fold (StatsFold: HEAD,
// MARK and the records in its object) and files (StatsFile: each save merges
// the records with stats.bin's sections), joined by the firmware's own glue
// (StatsGlue, which app/ListeningStats wraps: the read before the journal's
// begin(), the save's guards, the end's flush, the live fold at the Outbox's
// written(), the records' save and the boot overflow's tries at a pass the
// write gate lets through, Hs), over a card in memory that can lose its
// power at any step (test/support/CutFs.h) or fail chosen calls
// (test/support/FaultFs.h).
// Each scenario runs the fold, the save, the trim and the reboot, and is
// compared with tools/statsref.py's: the boots of test/fixtures/card/stats
// (index.json) and its sessions (sessions.json: boots, plays and batches of
// them, the records' save and its fence, the end's flush, Hs, two players,
// a 0.14.0 boot, NVS put back, a held tmp, a boot's overflow and its tries,
// a section damaged), file for file, byte for byte, each save's outcome, the
// records and behind after each step. No play lost, none counted twice; the
// undercounts 5.5 and 5.14 name, to the slot.
// - every boot of every fixture through the journal, chained boot to boot;
//   a remount mid-scan, and one after Ready before the first slot;
// - a cut at every write step of the boot's save, its compaction and the
//   trim, three ways, then a boot; a cut at every step of a records' save; a
//   question or a step the card fails, each in turn; a held tmp promoted by
//   the next save, and one a reboot's settle() drops while stats.bin is
//   there;
// - the base faulted at the boot (its open, its header's read): stats off,
//   nothing lost, nothing removed, nothing held;
// - the sessions (every one of statsref's, each step through the firmware's
//   code), the rotation hole, players, NVS and old firmware, firstAt
//   through a rotation's save;
// - firstAt's orders, the retention;
// - a full log never synced (13,106 slots): the overflow's 16 chunks at the
//   top, given back by the boot's save; the peak bytes and the microseconds
//   a slot, printed;
// - the glue's own rules: the card away, the end's flush, Hs's refusals,
//   the log's keys (Hm), a track's row (Ht), the last 7 days (Hd), the
//   records' save before the slot of its pass, its fence and its 96, behind
//   only after a failure or a refusal; the boot overflow's tries; a
//   remount's MARK past the room; the scan and a library update, never both
//   waiting.
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
namespace sg = statsglue;
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

// A heap that counts the glue's blocks (the work memory, the ranges' block,
// the overflow's chunks, MARK's block).
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

const uint8_t* u8(const Bytes& b) { return reinterpret_cast<const uint8_t*>(b.data()); }

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

// A stats.bin's section offset and its rows' count (its directory).
const cc::Section sectionOf(const Bytes& b, sf::Sec sec) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, sx::kStatsSpec, nullptr)));
  return c.section(sec);
}

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

// The scan's card faults a fixture names (`unread`): a log file's open the
// card fails, or its reads from a slot on; a replace() that fails (statsref's
// replaceFails: at stats.bin's remove, or with no stats.bin at the tmp's
// rename); the rest as FaultFs's.
struct ScanFaults : FaultFs {
  using FaultFs::FaultFs;
  std::set<std::string> openFails;
  int failBinRemoves = 0, failTmpRenames = 0;
  tagstore::File* openRead(const char* p, bool* cardFailed) override {
    if (openFails.count(p)) {
      *cardFailed = true;
      return nullptr;
    }
    return FaultFs::openRead(p, cardFailed);
  }
  bool remove(const char* p) override {
    if (failBinRemoves > 0 && std::strcmp(p, sx::kStatsBin) == 0) {
      --failBinRemoves;
      return false;
    }
    return FaultFs::remove(p);
  }
  bool rename(const char* from, const char* to) override {
    if (failTmpRenames > 0 && std::strcmp(from, sx::kStatsTmp) == 0 && std::strcmp(to, sx::kStatsBin) == 0) {
      --failTmpRenames;
      return false;
    }
    return FaultFs::rename(from, to);
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

// ---------------------------------------------------------------------------
// The glue: lib/core StatsGlue, the firmware's own (app/ListeningStats wraps
// it: docs/LISTENING-STATS.md 5.4-5.7, 5.14), with a test's Env around it:
// the calls it made, in order, and each save's outcome as statsref names
// them; the log's files on the card at each boot's or rotation's save the
// store was asked for; a full card at a save; a replace() that fails; the
// board's clocks.
// ---------------------------------------------------------------------------
// A save's outcome as statsref's sessions name it.
std::string outcomeName(sg::Refused r, const sx::SaveReport& rep) {
  if (r != sg::Refused::None) return sg::refusedName(r);
  switch (rep.what) {
    case sx::Saved::Wrote: return "wrote";
    case sx::Saved::Nothing: return "nothing";
    case sx::Saved::Held: return "held";
    case sx::Saved::Failed: return "failed";
    case sx::Saved::Changed: return "changed";
    case sx::Saved::Off: return "off";
    case sx::Saved::NoMemory: return "noMemory";
    case sx::Saved::Behind:
    case sx::Saved::Damaged: return "behind";
  }
  return "?";
}

struct Glue final : sg::Env {
  Glue(tagstore::Fs& fs, CutFs& card, const sg::Memory& m, ScanFaults* faults)
      : core(fs, *this, m),
        fold(core.fold()),
        store(core.store()),
        first(core.first()),
        opened(core.opened()),
        last(core.last()),
        card_(card),
        faults_(faults) {}
  sg::Glue core;
  sf::Fold& fold;
  sx::Store& store;
  const sx::Opened& first;   // the boot's read
  const sx::Opened& opened;  // the last read (a remount's)
  const sx::SaveReport& last;
  cc::msmf::Acks acks;      // the root's
  sx::SaveAsk ask;          // the board's clock, the root's commitTime, the update step's safe point
  int saveFails = 0;        // the next saves that fail (a full card at each of their steps); -1: every one
  bool replaceFails = false;  // each save's replace() fails (its tmp held)
  bool away = false;        // the card out of the slot, refusing writes, or another card in it
  std::function<uint64_t()> mono;    // the board's monotonic clock
  std::function<void()> beforeSave;  // (a test's: a cut's set-up)
  std::vector<std::string> calls;
  std::vector<std::pair<sg::Kind, std::string>> saves;  // each save asked and its outcome
  std::vector<std::string> seen;  // the log's files on the card at each boot's or rotation's save the store was asked for

  // PlayHistory::begin(): the read, before the journal's begin().
  sx::Opened read(sx::Store::WaitFn wait) {
    const sx::Opened& o = core.begin(acks, wait);
    return o;
  }
  void reserve(uint32_t slots) { core.reserve(slots); }
  // The journal ready: a boot's fold that asked no save (a partial scan)
  // ends here.
  void ready() { core.ready(); }
  // Outbox::Env::written(): the live fold.
  void written(const Slot& s) { core.written(s); }
  // Hs: never a compaction.
  bool hs() { return core.hs(false); }
  // The orderly end's flush (app/PlayHistory: the ending flag, the 3 s
  // budget): the boot's and the records' save refused; a rotation's with 1 s
  // of it left.
  void ending(uint64_t until) { core.ending(until); }
  void notEnding() { core.notEnding(); }
  // The last save's outcome (statsref's name).
  std::string outcome() const { return outcomeName(core.lastRefused(), core.last()); }

  // statsglue::Env
  uint64_t monoMs() override { return mono ? mono() : 0; }
  bool cardAway() override { return away; }
  void asking(sg::Kind k, sx::SaveAsk* a) override {
    *a = ask;
    if (k == sg::Kind::Boot || k == sg::Kind::Rotation) {
      std::string names;
      for (const char* n : {kPrev, kLog})
        if (card_.dir.count(n)) names += (names.empty() ? "" : " ") + base(n);
      seen.push_back(names);
      if (beforeSave) beforeSave();
    }
    if (saveFails) {
      card_.refuse = true;
      if (saveFails > 0) --saveFails;
    }
    if (replaceFails && faults_) faults_->failBinRemoves = faults_->failTmpRenames = 1;
  }
  void began(bool again, const sx::Opened*) override { calls.push_back(again ? "began again" : "began"); }
  void partialFile() override { calls.push_back("partial"); }
  void saved(sg::Kind k, sg::Refused r, const sx::SaveReport& rep) override {
    card_.refuse = false;
    if (faults_) faults_->failBinRemoves = faults_->failTmpRenames = 0;
    const std::string name = k == sg::Kind::Boot       ? "save boot"
                             : k == sg::Kind::Rotation ? "save rotation"
                             : k == sg::Kind::Records  ? "save records"
                                                       : "hs";
    calls.push_back(r == sg::Refused::None && sx::savedOk(rep.what) ? name : name + " (no)");
    saves.emplace_back(k, outcomeName(r, rep));
  }

private:
  CutFs& card_;
  ScanFaults* faults_;
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
  uint32_t statFails = 0;  // the next questions of the played file the card fails
  Glue* glue = nullptr;
  File stat(const char*, uint32_t* s, uint32_t* t) override {
    now += perStat;
    if (statFails) {
      --statFails;
      return File::Failed;
    }
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
  int saveFails = 0;  // the boot's save and its tries: 0 none, n the first n, -1 every one
  bool replaceFails = false;
};

struct Player {
  Player(CutFs& c, const uint8_t dev[6], uint32_t capSlots = pl::kFileSlots) : card(c), fs(&c), cap(capSlots) {
    std::memcpy(id, dev, 6);
  }
  CutFs& card;
  tagstore::Fs* fs;
  ScanFaults* faults = nullptr;  // (the replace() knobs, when fs is one)
  uint8_t id[6];
  uint32_t cap;
  MemNvs nvs;
  Epochs epochs;
  std::vector<std::string> ops;
  std::vector<uint8_t> buf = std::vector<uint8_t>(pl::kSlot);  // a slot a read: a read fault ends a file at its slot
  uint32_t piece = 5;
  bool stats = true;   // false: a 0.14.0 boot (no folds)
  bool fenced = false;  // a library update's fence is up (the records' save waits until 96; Hs refused)
  sg::Memory mem;      // the glue's blocks (null: malloc)
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
    glue.reset();
    fenced = false;
    glue.reset(stats ? new Glue(*fs, card, mem, faults) : nullptr);
    if (glue) {
      glue->acks = b.acks;
      glue->ask = b.ask;
      glue->saveFails = b.saveFails;
      glue->replaceFails = b.replaceFails;
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
  // The boot overflow's tries (statsglue's: kBootWaitsMs apart, at later
  // passes), the board's clock moved to each; the tries made.
  uint32_t tries() {
    uint32_t n = 0;
    while (glue && glue->core.retrying()) {
      oenv.now = std::max<uint64_t>(oenv.now, glue->core.retryAtMs());
      TEST_ASSERT_TRUE(glue->core.retryDue());
      glue->core.retry();
      ++n;
    }
    return n;
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
  // (saveFails: true, every try; a number n, the first n.)
  const minijson::Value& f = b["saveFails"];
  a.saveFails = f.type == minijson::Value::Bool ? (f.boolean() ? -1 : 0) : static_cast<int>(f.u64());
  a.replaceFails = b["replaceFails"].boolean();
  return a;
}

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
  p.faults = &faults;
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

// A card's stats as a boot reads them (a fold and a store of their own).
struct Read {
  sf::Fold fold;
  sx::Store store;
  sx::Opened o;
  explicit Read(tagstore::Fs& fs) : store(fs) {
    Work work;
    o = store.read(fold, work.p(), nullptr);
  }
};

// stats.bin's rows (its directory), as H reads them.
uint32_t rowsOf(CutFs& card, sf::Sec sec) {
  const Bytes b = bytesOf(card, "stats.bin");
  return b.empty() ? 0 : sectionOf(b, sec).count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Every boot of every fixture, through the journal.
// ---------------------------------------------------------------------------
void test_every_fixture_through_the_journal() {
  int boots = 0, saves = 0, compactions = 0, trims = 0, past = 0;
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
      // The scan's blocks given back by its end (none held but an overflow
      // whose save is tried again).
      TEST_ASSERT_FALSE_MESSAGE(g.core.rangesHeld(), w.c_str());
      if (!g.core.retrying()) TEST_ASSERT_FALSE_MESSAGE(g.core.scanHolds(), w.c_str());
      if (sc["name"].str() == "new-epoch") {
        TEST_ASSERT_TRUE(p.j->newEpoch());
        TEST_ASSERT_EQUAL_UINT32(3, p.j->booted().slots);  // (0.14.0's scan read none: a new epoch, no ACKS)
      }
      ++boots;
      saves += o.saved;
      compactions += o.compacted;
      trims += static_cast<int>(o.trimmed.size());
      past += g.fold.folded() > sf::kRecords;
    }
  }
  printf("fixtures through the journal: %d boots, %d saves, %d bases, %d files trimmed, %d past the records\n", boots,
         saves, compactions, trims, past);
  TEST_ASSERT_EQUAL_INT(34, boots);
  TEST_ASSERT_EQUAL_INT(25, saves);
  TEST_ASSERT_EQUAL_INT(4, compactions);
  TEST_ASSERT_EQUAL_INT(22, trims);
  TEST_ASSERT_EQUAL_INT(2, past);
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
      p.faults = &faults;
      p.piece = 2;
      p.begin(askOf(b));
      p.glue->reserve(1);
      const bool done = p.j->scan(1);  // (one slot read: every fixture's log has more)
      TEST_ASSERT_FALSE_MESSAGE(done, w.c_str());
      p.j->remounted();
      TEST_ASSERT_FALSE_MESSAGE(p.glue->core.scanHolds(), w.c_str());  // (the fold's blocks given back at once)
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
  // Promoted by Hs: the held tmp is the boot's save whole (5.14's Held: the
  // records emptied, HEAD the plan's, the card's MARK the one before until
  // the promotion), and nothing new came since, so Hs writes nothing more.
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
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.tmp") == want.at("stats.bin"));
    TEST_ASSERT_TRUE(p.trimmed().empty());  // (the MARK read at the boot, 41, doesn't cover plays.1.jnl)
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_TRUE(p.glue->last.heldPromoted);
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(p.glue->last.what));
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsTmp));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    // The next boot reads it, folds nothing, and trims what it covers.
    Player q(card, p.id);
    q.nvs = p.nvs;
    q.boot(askOf(b));
    TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(q.glue->first.read));
    TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
    TEST_ASSERT_TRUE((q.trimmed() == std::set<std::string>{"plays.1.jnl"}));
    sameFiles(want, filesOf(card), "a held tmp, promoted");
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
// fold, no save and no trim, and no stats-<n>.bin removed; nothing held for
// the stats but the glue's object. The card answering again, the next boot
// is the fixture's.
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
    p.mem.alloc = heapAlloc;
    p.mem.release = heapFree;
    const size_t held0 = gHeap.held;
    gWaits.clear();
    p.boot(askOf(b));
    const char* w = from == 0 ? "the base's open" : "the base's header";
    // The stats off: nothing held for them (the read's work memory given
    // back; no ranges' block, no chunk).
    TEST_ASSERT_EQUAL_size_t_MESSAGE(held0, gHeap.held, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.glue->fold.chunks(), w);
    TEST_ASSERT_FALSE_MESSAGE(p.glue->core.scanHolds(), w);
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
    // A remount's read, the card whole now: the stats on again, MARK loaded.
    p.j->remounted();
    TEST_ASSERT_TRUE_MESSAGE(p.scan(), w);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("loaded", sx::readName(p.glue->opened.read), w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->core.on(), w);
    TEST_ASSERT_TRUE_MESSAGE(p.glue->fold.marks() > 0, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(held0, gHeap.held, w);
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
  const uint32_t hour1 = rowsOf(card, sf::kHour), days1 = rowsOf(card, sf::kDays);
  const uint32_t rcnt1 = rowsOf(card, sf::kRcnt);
  put(card, {{"plays.jnl", cardOf(ca["boots"][1]["in"]).at("plays.jnl")}});
  fixtureBoot(p, faults, ca["boots"][1]);
  TEST_ASSERT_EQUAL_UINT32(41, hour1);
  TEST_ASSERT_EQUAL_UINT32(41, days1);
  TEST_ASSERT_EQUAL_UINT32(1, rcnt1);
  TEST_ASSERT_EQUAL_UINT32(hour1 + 1, rowsOf(card, sf::kHour));
  TEST_ASSERT_EQUAL_UINT32(days1 + 1, rowsOf(card, sf::kDays));
  TEST_ASSERT_EQUAL_UINT32(2, rowsOf(card, sf::kRcnt));
  sameFiles(cardOf(ca["boots"][1]["out"]), filesOf(card), "clock-ahead");
  // H reads the same rows from the card's directory.
  const sg::Glue::FileLook l = p.glue->core.lookFile();
  TEST_ASSERT_TRUE(l.there && l.frame);
  TEST_ASSERT_EQUAL_UINT32(hour1 + 1, l.rows[sf::kHour]);
  TEST_ASSERT_EQUAL_UINT32(2, l.rows[sf::kRcnt]);
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
// journal's scan. The overflow past the 96 records in 16 chunks, carved
// through the glue's top hook before each piece (app/PlayHistory's 128
// slots), held while the scan runs (a track's start and a library update
// wait meanwhile), given back by the boot's save with the ranges' block;
// TRKD's rows the distinct hashes. The peak bytes and the microseconds a
// slot, printed.
// ---------------------------------------------------------------------------
namespace {
// The slots of a full log never synced (the cap twice: plays.1.jnl the
// older), their distinct hashes and plays.
struct FullLog {
  std::vector<uint8_t> files[2];
  std::set<uint64_t> hashes;
  uint64_t plays = 0;
  FullLog(const uint8_t dev[6], uint64_t epoch) {
    uint64_t x = 2026;
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
  }
};
}  // namespace

void test_a_full_log_never_synced() {
  const uint8_t dev[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
  const uint64_t epoch = 0x1122334455667788ull;
  CutFs card;
  const FullLog log(dev, epoch);
  card.put(kLog, log.files[0]);
  card.put(kPrev, log.files[1]);
  Player p(card, dev);
  p.nvs.ids.epoch = epoch;
  p.nvs.ids.seq = 2 * pl::kFileSlots;
  p.buf.assign(pl::kReadBytes, 0);  // the firmware's: 32 slots a read
  p.piece = 128;                    // app/PlayHistory's piece
  p.mem.alloc = heapAlloc;
  p.mem.release = heapFree;
  gHeap = Heap();
  BootAsk a;
  a.ask.nowMs = sf::kMinMs + 530ull * sf::kDayMs;
  a.ask.playing = true;  // (no compaction: the fold and stats.bin alone)
  a.ask.safePoint = false;
  p.begin(a);
  const size_t atRead = gHeap.held;
  gHeap.peak = gHeap.held;
  // The scan, a piece a call; the holds seen between its pieces.
  bool held = false, waits = false;
  const auto t0 = std::chrono::steady_clock::now();
  for (int guard = 0; guard < 1000000; ++guard) {
    p.glue->reserve(p.piece);
    held = held || p.glue->core.overflowHeld();
    waits = waits || p.glue->core.scanHolds();
    if (p.j->scan(p.piece)) break;
  }
  p.glue->ready();
  const auto t1 = std::chrono::steady_clock::now();
  TEST_ASSERT_TRUE(p.j->ready());
  const Glue& g = *p.glue;
  TEST_ASSERT_TRUE(held && waits);
  TEST_ASSERT_EQUAL_UINT32(2 * pl::kFileSlots, g.fold.folded());
  TEST_ASSERT_EQUAL_UINT64(log.plays, g.fold.head().plays);
  TEST_ASSERT_EQUAL_UINT64(2 * pl::kFileSlots, g.fold.head().slots);
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(g.last.what));
  TEST_ASSERT_EQUAL_UINT32(log.hashes.size(), g.last.rows[sf::kTrkd]);
  TEST_ASSERT_FALSE(g.fold.noMemory());
  // Every block given back by the save: the chunks, the ranges' block, the
  // save's work memory.
  TEST_ASSERT_EQUAL_UINT32(0, g.fold.chunks());
  TEST_ASSERT_EQUAL_UINT32(0, g.fold.records());
  TEST_ASSERT_FALSE(g.core.scanHolds());
  TEST_ASSERT_FALSE(g.core.overflowHeld());
  TEST_ASSERT_EQUAL_size_t(atRead, gHeap.held);
  TEST_ASSERT_EQUAL_size_t(sf::kMaxChunks * sf::kChunkBytes, g.fold.peak());
  // The peak: the 16 chunks, the ranges' block and the save's work memory.
  TEST_ASSERT_TRUE(gHeap.peak <= atRead + g.fold.peak() + g.core.rangesBytes() + sx::kWorkBytes);
  const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
  printf("a full log never synced, through the journal: %u slots, %u hashes (TRKD %u rows), RCNT %u rows after the "
         "save; the overflow's peak %u bytes in %u chunks, the ranges' block %u, the heap's peak %u over the read's "
         "%u; %.2f us a slot on this host (the scan, the fold, the save); stats.bin %u bytes\n",
         static_cast<unsigned>(2 * pl::kFileSlots), static_cast<unsigned>(log.hashes.size()),
         static_cast<unsigned>(g.last.rows[sf::kTrkd]), static_cast<unsigned>(g.last.rows[sf::kRcnt]),
         static_cast<unsigned>(g.fold.peak()), static_cast<unsigned>(sf::kMaxChunks),
         static_cast<unsigned>(g.core.rangesBytes()), static_cast<unsigned>(gHeap.peak - atRead),
         static_cast<unsigned>(atRead), us / (2 * pl::kFileSlots), static_cast<unsigned>(g.last.bytes));
  // Read back by the next boot: the same rows, nothing folded again.
  Player q(card, dev);
  q.nvs = p.nvs;
  q.boot(a);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(q.glue->first.read));
  TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
  TEST_ASSERT_EQUAL_UINT32(log.hashes.size(), rowsOf(card, sf::kTrkd));
  TEST_ASSERT_EQUAL_UINT64(log.plays, q.glue->fold.head().plays);
}

// ---------------------------------------------------------------------------
// The sessions (stats/sessions.json): each step through the firmware's
// code, the card after it compared with statsref's, file for file; each
// save's outcome, the records and behind after each step.
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

// A play's fields: a play step's own (`i` SIZE_MAX), or a batch's column i.
PlaySpec playOf(const minijson::Value& st, size_t i = SIZE_MAX) {
  auto col = [&](const char* k) -> const minijson::Value& { return i == SIZE_MAX ? st[k] : st["plays"][k][i]; };
  PlaySpec p;
  p.hash = col("hash").u64();
  p.played = static_cast<uint32_t>(col("played").u64());
  p.duration = static_cast<uint32_t>(col("duration").u64());
  p.outcome = static_cast<uint8_t>(col("outcome").u64());
  p.source = static_cast<uint8_t>(col("src").u64());
  p.at = col("at").u64();
  p.up = static_cast<uint32_t>(col("up").u64());
  p.size = static_cast<uint32_t>(col("size").u64());
  p.fatTime = static_cast<uint32_t>(col("fatTime").u64());
  p.generation = static_cast<uint32_t>(col("generation").u64());
  return p;
}

// A stats.bin's TRKD rows, by hash.
std::map<uint64_t, sf::Track> tracksOf(const Bytes& b) {
  const cc::Section s = sectionOf(b, sf::kTrkd);
  std::map<uint64_t, sf::Track> out;
  for (uint32_t i = 0; i < s.count; ++i) {
    const sf::Track t = sf::decodeTrack(u8(b) + s.offset + i * s.stride);
    out[t.hash] = t;
  }
  return out;
}

// A section's first byte flipped (XOR 0xFF), the header and the directory
// (its CRC) as they were: statsref's `damage`.
void damage(CutFs& card, const std::string& name, const std::string& section) {
  Bytes b = bytesOf(card, name);
  for (uint32_t s = 0; s < sf::kSecs; ++s)
    if (sf::kTypes[s] == cc::get32(reinterpret_cast<const uint8_t*>(section.c_str()))) {
      const cc::Section sec = sectionOf(b, static_cast<sf::Sec>(s));
      TEST_ASSERT_TRUE(sec.bytes > 0);
      b[sec.offset] = static_cast<char>(b[sec.offset] ^ 0xFF);
    }
  put(card, {{name, b}});
}

const char* factOf(const minijson::Value& v) { return v.isNull() ? "" : v.c_str(); }

// One play of a session's player, as the firmware's loop takes it: at a
// pass the write gate lets through, a records' save due first (before the
// slot; statsglue::turnOf()); the slot (the journal's append: a rotation's
// save first when plays.1.jnl holds a play the card's MARK doesn't cover);
// under a fence, the records' save once they reach 96, whatever the fence.
// In the orderly end's flush (`ending`): the played file's question takes
// what leaves `left` of its 3 s budget at a rotation's save. Each save's
// outcome against the step's facts.
void sessionPlay(Player& p, const PlaySpec& spec, const minijson::Value& now, bool saveFails, bool replaceFails,
                 bool ending, uint64_t left, const minijson::Value& seq, const minijson::Value& save,
                 const minijson::Value& rotationSave, const minijson::Value& recordsSave, const std::string& w) {
  Glue* g = p.glue.get();
  if (g) {
    g->ask = sx::SaveAsk();
    g->ask.nowMs = now.isNull() ? 0 : now.u64();
    g->saveFails = saveFails ? -1 : 0;
    g->replaceFails = replaceFails;
    if (ending) g->ending(p.oenv.now + 3000);
  }
  std::string rec, rot;
  if (g && g->core.recordsDue(p.fenced)) {
    g->core.recordsSave();
    rec = g->outcome();
  }
  const size_t mark = g ? g->saves.size() : 0;
  if (ending) {
    TEST_ASSERT_NOT_NULL(g);
    p.oenv.perStat = 3000 - left;
    p.ended(spec);
    const pl::Outbox::Flushed f = p.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, f.written, w.c_str());
    g->notEnding();
    p.oenv.perStat = 0;
  } else {
    TEST_ASSERT_TRUE_MESSAGE(p.write(spec) == pl::Outbox::Did::Written, w.c_str());
  }
  for (size_t c = mark; g && c < g->saves.size(); ++c)
    if (g->saves[c].first == sg::Kind::Rotation) rot = g->saves[c].second;
  if (g && p.fenced && g->core.recordsDue(true)) {
    g->core.recordsSave();
    rec = g->outcome();
  }
  if (g) {
    g->saveFails = 0;
    g->replaceFails = false;
  }
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(seq.u64(), p.j->lastSeq(), w.c_str());
  TEST_ASSERT_EQUAL_STRING_MESSAGE(factOf(recordsSave), rec.c_str(), (w + ", the records' save").c_str());
  TEST_ASSERT_EQUAL_STRING_MESSAGE(factOf(rotationSave), rot.c_str(), (w + ", the rotation's save").c_str());
  const bool saved = rot == "wrote" || rot == "nothing";
  TEST_ASSERT_EQUAL_MESSAGE(!save.isNull(), !rot.empty(), w.c_str());
  if (!save.isNull()) TEST_ASSERT_EQUAL_MESSAGE(save.boolean(), saved, w.c_str());
  // (When it ran, plays.1.jnl was still on the card: before its delete, or
  // its rename to a twin.)
  if (saved) TEST_ASSERT_EQUAL_STRING_MESSAGE("plays.1.jnl plays.jnl", g->seen.back().c_str(), w.c_str());
}

int gSessionSteps = 0;

// One session, each step through the players' journals, glues, Trackers
// and Outboxes over one card; the card compared after each step, and the
// step's facts (a boot's read, folds, save, its overflow's tries and trim;
// a play's seq, its records' and rotation's saves; Hs; the records and
// behind after each step). The card's HEAD at the end, read back: every
// slot but `lost`.
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
    p->faults = &faults;
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
    char wb[160];
    std::snprintf(wb, sizeof(wb), "%s, step %u (%s %s)", name.c_str(), static_cast<unsigned>(i + 1), doing.c_str(),
                  st["player"].isNull() ? "" : st["player"].c_str());
    const std::string w = wb;
    Player* p = st["player"].isNull() ? nullptr : players.at(st["player"].str()).get();
    if (doing == "boot") {
      p->stats = st["stats"].isNull() || st["stats"].boolean();
      const BootAsk a = askOf(st);
      faults.unread(st["unread"]);
      gWaits.clear();
      TEST_ASSERT_TRUE_MESSAGE(p->boot(a), w.c_str());
      faults.healed();
      std::set<std::string> want;
      for (size_t t = 0; t < st["trimmed"].size(); ++t) want.insert(st["trimmed"][t].str());
      TEST_ASSERT_TRUE_MESSAGE(want == p->trimmed(), w.c_str());
      if (p->stats) {
        Glue& g = *p->glue;
        Outcome o = outcomeOf(*p);
        // The overflow's tries at later passes (kBootWaitsMs apart, the
        // board's clock moved to each), then what the boot's save came to:
        // any try that wrote, compacted or removed the old base.
        while (g.core.retrying()) {
          p->oenv.now = std::max<uint64_t>(p->oenv.now, g.core.retryAtMs());
          TEST_ASSERT_TRUE_MESSAGE(g.core.retryDue(), w.c_str());
          g.core.retry();
          o.saved = o.saved || g.last.what == sx::Saved::Wrote;
          o.compacted = o.compacted || g.last.compacted;
          o.removed += g.last.oldBaseRemoved ? 1 : 0;
        }
        std::string boot;
        for (const auto& s : g.saves)
          if (s.first == sg::Kind::Boot) boot = s.second;
        sameOutcome(st, o, w);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(factOf(st["bootSave"]), boot.c_str(), w.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["tries"].u64(), g.core.tries(), w.c_str());
        TEST_ASSERT_EQUAL_MESSAGE(st["held"].boolean(), boot == "held", w.c_str());
        TEST_ASSERT_FALSE_MESSAGE(g.core.scanHolds(), w.c_str());  // (the overflow given back: landed, or behind)
        g.saveFails = 0;
        g.replaceFails = false;
      }
    } else if (doing == "play") {
      sessionPlay(*p, playOf(st), st["now"], st["saveFails"].boolean(), st["replaceFails"].boolean(),
                  st["ending"].boolean(), st["left"].u64(), st["seq"], st["save"], st["rotationSave"],
                  st["recordsSave"], w);
    } else if (doing == "plays") {
      const minijson::Value& each = st["each"];
      for (size_t k = 0; k < each["seq"].size(); ++k) {
        const std::string one = w + ", play " + std::to_string(k + 1);
        sessionPlay(*p, playOf(st, k), st["plays"]["now"][k], st["saveFails"].boolean(), false, false, 0,
                    each["seq"][k], each["save"][k], each["rotationSave"][k], each["recordsSave"][k], one);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(each["records"][k].u64(), p->glue ? p->glue->fold.records() : 0,
                                         one.c_str());
      }
    } else if (doing == "hs") {
      Glue* g = p->glue.get();
      std::string rec, hs = "off";
      bool ok = false;
      if (g) {
        g->ask = sx::SaveAsk();
        g->ask.nowMs = st["now"].isNull() ? 0 : st["now"].u64();
        g->saveFails = st["saveFails"].boolean() ? -1 : 0;
        g->replaceFails = st["replaceFails"].boolean();
        // (A pass the write gate lets through: the records' save first,
        // turnOf()'s, then Hs.)
        if (g->core.recordsDue(p->fenced)) {
          g->core.recordsSave();
          rec = g->outcome();
        }
        ok = g->core.hs(p->fenced);
        hs = g->outcome();
        g->saveFails = 0;
        g->replaceFails = false;
      }
      TEST_ASSERT_EQUAL_STRING_MESSAGE(factOf(st["recordsSave"]), rec.c_str(), (w + ", the records' save").c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(factOf(st["hsSave"]), hs.c_str(), (w + ", Hs").c_str());
      TEST_ASSERT_EQUAL_MESSAGE(st["ok"].boolean(), ok, w.c_str());
    } else if (doing == "fence" || doing == "unfence") {
      TEST_ASSERT_TRUE_MESSAGE(p->fenced != (doing == "fence"), w.c_str());
      p->fenced = doing == "fence";
    } else if (doing == "update") {
      // A library update: its fence up for a pass the write gate lets
      // through, then down (LibraryUpdate's), over no play. Nothing of the
      // stats moves: the scan done, nothing held for it to wait for
      // (Wait::Stats: scanHolds()); a records' save due waits for the fence
      // (until 96: none of statsref's updates comes with 96 records), so
      // turnOf() gives none in it, and it is due again after it.
      TEST_ASSERT_FALSE_MESSAGE(p->fenced, w.c_str());
      if (Glue* g = p->glue.get()) {
        TEST_ASSERT_FALSE_MESSAGE(g->core.scanHolds(), w.c_str());
        const bool due = g->core.recordsDue(false);
        p->fenced = true;
        TEST_ASSERT_TRUE_MESSAGE(sg::turnOf(g->core.saveDue(true), 0, false) == sg::Turn::Nothing, w.c_str());
        p->fenced = false;
        TEST_ASSERT_EQUAL_MESSAGE(due, g->core.recordsDue(false), w.c_str());
      }
    } else if (doing == "remount") {
      p->j->remounted();
      p->out->remounted();
    } else if (doing == "nvs") {
      p->nvs.ids.seq = static_cast<uint32_t>(st["seq"].u64());
    } else if (doing == "erase") {
      p->nvs.ids = Journal::Ids();
    } else if (doing == "delete") {
      TEST_ASSERT_TRUE_MESSAGE(card.remove((kDir + st["name"].str()).c_str()), w.c_str());
    } else if (doing == "link") {
      card.link(kDir + st["a"].str(), kDir + st["b"].str());
    } else if (doing == "damage") {
      damage(card, st["name"].str(), st["section"].str());
    } else {
      TEST_FAIL_MESSAGE(w.c_str());
    }
    if (p && st.has("records"))
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(st["records"].u64(), p->glue ? p->glue->fold.records() : 0, w.c_str());
    if (p && st.has("behind"))
      TEST_ASSERT_EQUAL_MESSAGE(st["behind"].boolean(), p->glue && p->glue->fold.behind(), w.c_str());
    sameFiles(sessionCard(st["out"]), filesOf(card), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w.c_str());
    ++gSessionSteps;
  }
  const Read r(card);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("loaded", sx::readName(r.o.read), name.c_str());
  TEST_ASSERT_EQUAL_UINT64_MESSAGE(sc["slots"].u64() - sc["lost"].u64(), r.fold.head().slots, name.c_str());
  return r.fold.head();
}

}  // namespace

// Every session, each step through the firmware's code.
void test_every_session() {
  gSessionSteps = 0;
  for (size_t i = 0; i < gSessions.size(); ++i) runSession(gSessions[i]);
  printf("sessions: %u, %d steps, the card compared after each\n", static_cast<unsigned>(gSessions.size()),
         gSessionSteps);
  TEST_ASSERT_EQUAL_UINT32(36, gSessions.size());
  TEST_ASSERT_EQUAL_INT(290, gSessionSteps);
}

// A rotation over a plays.1.jnl no save covers (the boot's save failed): its
// save first, before the delete or the rename to a twin; failing too, the
// plays in it are lost to the stats (5.5, 13: the undercount), exactly. So
// are they in a rotation in the end's flush with under 1 s of its budget
// left. And 5.14's: a session behind its log (the records' saves failing to
// 96, a full log whose boot save failed all four tries) loses the plays of
// the plays.1.jnl a rotation takes out, and so does one whose rotations'
// saves failed before it was behind (their plays were in the records
// alone). Each session's slots pinned, so a fixture regenerated smaller
// shows here.
void test_the_rotation_hole() {
  struct Case {
    const char* name;
    uint64_t slots, lost;
  };
  const Case cases[] = {{"rotation-saves", 9, 0},
                        {"rotation-fails", 9, 4},
                        {"rotation-twin-saves", 9, 0},
                        {"rotation-twin-fails", 9, 4},
                        {"flush-in-time", 9, 0},
                        {"flush-late", 9, 4},
                        {"partial-rotation", 13, 4},
                        {"records-behind-rotation", 129, 64},
                        {"records-fail-rotations", 99, 77},
                        {"boot-overflow-full-rotation", 257, 128},
                        {"boot-overflow-full-rotation-retry", 257, 0}};
  for (const auto& c : cases) {
    const minijson::Value& sc = session(c.name);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(c.lost, sc["lost"].u64(), c.name);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(c.slots, sc["slots"].u64(), c.name);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(c.slots - c.lost, runSession(sc).slots, c.name);
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
// (Hm); a track's row, base ⊕ stats.bin's TRKD ⊕ the records (Ht); stats.bin's
// header and rows (H); the last 7 days (Hd).
// ---------------------------------------------------------------------------
namespace {

const char* refused(const Player& p) { return sg::refusedName(p.glue->core.lastRefused()); }

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
  // save doesn't compact, and the merge is the same).
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
    TEST_ASSERT_EQUAL_UINT32(7, p.glue->fold.records());
    TEST_ASSERT_TRUE(p.trimmed().empty());
    sameFiles(in, filesOf(card), "the card away");
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("cardAway", refused(p));
    p.glue->away = false;
    TEST_ASSERT_FALSE(p.glue->core.hs(true));
    TEST_ASSERT_EQUAL_STRING("fenced", refused(p));
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    TEST_ASSERT_EQUAL_STRING("Hs", sg::kindName(p.glue->core.lastKind()));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == want.at("stats.bin"));
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
    TEST_ASSERT_TRUE(p.glue->hs());  // (nothing new: nothing written)
    TEST_ASSERT_EQUAL_STRING("nothing new", sx::savedName(p.glue->last.what));
    TEST_ASSERT_EQUAL_UINT32(1, p.glue->core.savesWritten());
    // stats.bin's header and directory, as H reads them.
    const sg::Glue::FileLook l = p.glue->core.lookFile();
    TEST_ASSERT_TRUE(l.there);
    TEST_ASSERT_FALSE(l.unread);
    TEST_ASSERT_TRUE(l.frame);
    TEST_ASSERT_EQUAL_UINT32(sx::kMagic, l.magic);
    TEST_ASSERT_EQUAL_UINT32(1, l.major);
    TEST_ASSERT_EQUAL_UINT32(0, l.minor);
    TEST_ASSERT_EQUAL_UINT32(want.at("stats.bin").size(), l.bytes);
    TEST_ASSERT_EQUAL_UINT32(p.glue->store.cardCopy().generation, l.generation);
    for (uint32_t s = 0; s < sf::kSecs; ++s)
      TEST_ASSERT_EQUAL_UINT32(sectionOf(want.at("stats.bin"), static_cast<sf::Sec>(s)).count, l.rows[s]);
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
  // leave 3 s and 0.5 s: only this pins the edge. The records' save there:
  // refused (the flush's plays only take records).
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
    p.glue->ending(p.oenv.now + sg::kEndSaveMs - 1);
    TEST_ASSERT_FALSE(p.glue->core.save(pl::Folds::Why::Rotation));
    TEST_ASSERT_EQUAL_STRING("budget", refused(p));
    TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == before);
    TEST_ASSERT_FALSE(p.glue->core.recordsSave());
    TEST_ASSERT_EQUAL_STRING("ending", refused(p));
    p.glue->ending(p.oenv.now + sg::kEndSaveMs);
    TEST_ASSERT_TRUE(p.glue->core.save(pl::Folds::Why::Rotation));
    TEST_ASSERT_EQUAL_STRING("none", refused(p));
    TEST_ASSERT_EQUAL_STRING("rotation", sg::kindName(p.glue->core.lastKind()));
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
        if (pl::decode(u8(f.second) + at, pl::kSlot, &s) != pl::Verdict::Ok) continue;
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
    const sg::Glue& g = p.glue->core;
    TEST_ASSERT_TRUE(log.size() >= 2 && log.size() <= sg::kLogKeys);
    TEST_ASSERT_EQUAL_UINT32(log.size(), g.logKeys());
    TEST_ASSERT_EQUAL_UINT32(0, g.logKeysMore());
    for (uint32_t i = 0; i < g.logKeys(); ++i) {
      const sg::LogKey& k = g.logKey(i);
      const auto it = log.find({std::string(reinterpret_cast<const char*>(k.dev), 6), k.epoch});
      TEST_ASSERT_TRUE(it != log.end());
      TEST_ASSERT_EQUAL_UINT32(it->second.first, k.seq);
      TEST_ASSERT_EQUAL_UINT32(it->second.second, k.slots);
    }
    TEST_ASSERT_TRUE(g.fold().marks() >= 2);
    for (uint32_t i = 0; i < g.fold().marks(); ++i) {
      const sf::Fold::MarkRow* m = g.fold().markRow(i);
      TEST_ASSERT_NOT_NULL(m);
      TEST_ASSERT_EQUAL_UINT32(m->seq, g.fold().markOf(m->dev, m->epoch()));
      TEST_ASSERT_EQUAL_UINT32(m->seq, m->card);  // (saved: the card's MARK is RAM's)
    }
    TEST_ASSERT_NULL(g.fold().markRow(g.fold().marks()));
  }
  // A track's row, base ⊕ delta (Ht): compaction's boot leaves every row in
  // its new base (TRKD empty); a live play of one of them merges with it, a
  // record (the delta); saved by Hs, the delta is stats.bin's TRKD row.
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
    const auto row = [&](uint32_t i) { return sf::decodeTrack(u8(base) + sx::kBaseRowsAt + i * sf::kTrackBytes); };
    for (uint32_t i : {0u, rows / 2, rows - 1}) {
      const sg::TrackLook t = p.glue->core.track(row(i).hash);
      TEST_ASSERT_TRUE(t.base == sg::TrackLook::Base::Found);
      TEST_ASSERT_FALSE(t.delta);
      TEST_ASSERT_FALSE(t.unread);
      sameTrack(row(i), t.row, "a base row");
      TEST_ASSERT_TRUE(t.reads >= 1);
    }
    const sg::TrackLook none = p.glue->core.track(0x0123456789abcdefull);
    TEST_ASSERT_TRUE(none.base == sg::TrackLook::Base::NotThere);
    TEST_ASSERT_FALSE(none.delta);
    TEST_ASSERT_EQUAL_UINT64(0, none.row.hash);
    // A live play of the first row's track: base ⊕ the record.
    PlaySpec ps;
    ps.hash = row(0).hash;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + 300ull * sf::kDayMs;
    ps.up = 5000;
    TEST_ASSERT_TRUE(p.write(ps) == pl::Outbox::Did::Written);
    const sg::TrackLook t = p.glue->core.track(ps.hash);
    TEST_ASSERT_TRUE(t.base == sg::TrackLook::Base::Found);
    TEST_ASSERT_TRUE(t.delta);
    TEST_ASSERT_EQUAL_UINT32(1, t.deltaRow.plays);
    sameTrack(sf::merge(row(0), t.deltaRow), t.row, "base and a record");
    TEST_ASSERT_EQUAL_UINT32(row(0).plays + 1u, t.row.plays);
    // Saved: the same row, now stats.bin's TRKD ⊕ the base.
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
    TEST_ASSERT_EQUAL_UINT32(1, rowsOf(card, sf::kTrkd));
    const sg::TrackLook u = p.glue->core.track(ps.hash);
    TEST_ASSERT_TRUE(u.delta);
    sameTrack(t.row, u.row, "base and stats.bin's TRKD");
  }
  // The last 7 days (Hd): stats.bin's DAYS and HOUR merged with the
  // records, the days the card's rows give, a record dated today added.
  {
    CutFs card;
    put(card, in);
    Player p(card, dev);
    deviceFor(p, sc);
    TEST_ASSERT_TRUE(p.boot(askOf(b)));
    const Bytes bin = bytesOf(card, "stats.bin");
    const cc::Section days = sectionOf(bin, sf::kDays);
    TEST_ASSERT_TRUE(days.count > 0);
    const uint16_t last = cc::get16(u8(bin) + days.offset + (days.count - 1) * days.stride);
    sg::DaysLook d = p.glue->core.lookDays(last);
    TEST_ASSERT_TRUE(d.read);
    TEST_ASSERT_TRUE(d.d[6].any);
    TEST_ASSERT_EQUAL_UINT16(last, d.d[6].day);
    TEST_ASSERT_TRUE(std::memcmp(d.d[6].row, u8(bin) + days.offset + (days.count - 1) * days.stride, 16) == 0);
    const uint16_t plays = cc::get16(d.d[6].row + 2);
    TEST_ASSERT_TRUE(d.d[6].hours > 0);
    PlaySpec ps;
    ps.hash = 0x0123456789abcdefull;
    ps.played = 120000;
    ps.duration = 200000;
    ps.at = sf::kMinMs + static_cast<uint64_t>(last) * sf::kDayMs + 3 * sf::kHourMs;
    ps.up = 5000;
    TEST_ASSERT_TRUE(p.write(ps) == pl::Outbox::Did::Written);
    d = p.glue->core.lookDays(last);
    TEST_ASSERT_EQUAL_UINT16(plays + 1, cc::get16(d.d[6].row + 2));
  }
}

// Hs after a partial scan says why (the store would refuse it too: the
// fold's plan), and Hs never compacts, not even where a boot's save would
// (no base yet, nothing playing).
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
    // (A boot's save now would compact: no base, nothing playing, a delta.)
    TEST_ASSERT_TRUE(sf::compactionDue(rowsOf(card, sf::kTrkd) + 1, 0, false, true));
    TEST_ASSERT_TRUE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
    TEST_ASSERT_FALSE(p.glue->last.compacted);
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.head().baseGen);
    for (const auto& f : filesOf(card)) TEST_ASSERT_FALSE_MESSAGE(f.first.compare(0, 6, "stats-") == 0, f.first.c_str());
  }
}

// The boot's fold's ranges' block, and a remount's read of stats.bin (its
// work memory), wait for the scan's first reserve(): a pass that may carve
// (5.6, 5.14: the join not Working, no library update asked, the write gate
// open), never setup()'s, nor the remount's own pass (the card guard's). A
// scan that asks no reserve() (the end's flush finishing it) begins the fold
// at its first call. A remount's read still to make when the end flushes is
// never made: the folds dropped.
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
    p.mem.alloc = heapAlloc;
    p.mem.release = heapFree;
    uint32_t afterRead = 0;
    size_t heldAfterRead = 0;
    p.afterRead = [&] {
      afterRead = gHeap.carves;
      heldAfterRead = gHeap.held;
    };
    p.begin(askOf(b));
    // The journal's begin() (scanBegan()): nothing carved for the stats, and
    // nothing held after the read (its work memory given back).
    TEST_ASSERT_EQUAL_UINT32(afterRead, gHeap.carves);
    TEST_ASSERT_TRUE(p.glue->core.booting());
    TEST_ASSERT_TRUE(p.glue->core.wantsRoom());
    TEST_ASSERT_TRUE(p.glue->core.bootPending());
    TEST_ASSERT_FALSE(p.glue->fold.booting());
    TEST_ASSERT_FALSE(p.glue->core.rangesHeld());
    TEST_ASSERT_FALSE(p.glue->core.scanHolds());
    TEST_ASSERT_FALSE(p.glue->hs());
    TEST_ASSERT_EQUAL_STRING("booting", refused(p));
    p.glue->reserve(1);
    TEST_ASSERT_TRUE(p.glue->fold.booting());
    TEST_ASSERT_TRUE(p.glue->core.rangesHeld());
    TEST_ASSERT_TRUE(p.glue->core.scanHolds());
    TEST_ASSERT_FALSE(p.glue->core.overflowHeld());  // (7 slots: no chunk)
    TEST_ASSERT_EQUAL_UINT32(afterRead + 1, gHeap.carves);
    TEST_ASSERT_EQUAL_size_t(heldAfterRead + p.glue->core.rangesBytes(), gHeap.held);
    TEST_ASSERT_TRUE(p.scan());
    sameOutcome(b, outcomeOf(p), "the boot's fold at its first reserve()");
    sameFiles(want, filesOf(card), "the boot's fold at its first reserve()");
    TEST_ASSERT_FALSE(p.glue->core.wantsRoom());
    TEST_ASSERT_FALSE(p.glue->core.scanHolds());
    TEST_ASSERT_EQUAL_size_t(heldAfterRead, gHeap.held);  // (the ranges' block and the save's work memory back)
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
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
    TEST_ASSERT_TRUE(p.scan());  // (the flush finishing the scan: no read)
    TEST_ASSERT_EQUAL_UINT32(0, p.glue->core.rereads());
    TEST_ASSERT_EQUAL_STRING("save boot (no)", p.glue->calls.back().c_str());
    TEST_ASSERT_EQUAL_STRING("off", refused(p));
    TEST_ASSERT_TRUE(p.trimmed().empty());
    sameFiles(want, filesOf(card), "a remount's read when the end flushes");
  }
}

// A fold out of memory from the boot's first fold (its ranges' block
// couldn't be carved) and no work memory for the save either (the same
// short heap): no save, and the trim by the MARK read at the boot (5.5),
// never as after a save. worked-example's whole boot trims plays.1.jnl
// (ACKS 45 covers it); its MARK, 41, doesn't cover plays 42-45, never
// folded: it stays. Over a loaded stats.bin and over none (no MARK: nothing
// covered).
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
    p.mem.alloc = shortAlloc;
    p.mem.release = shortFree;
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
    TEST_ASSERT_FALSE_MESSAGE(p.glue->core.scanHolds(), w);
    sameFiles(in, filesOf(card), w);
  }
}

// ---------------------------------------------------------------------------
// The saves at a pass the write gate lets through (statsglue::turnOf()):
// the records' save before the slot of its pass, its wait behind a library
// update's fence and its run at 96 whatever the fence, behind only after a
// save asked failed or was refused; Hs after the plays waiting for their
// slots (S1's device round 1).
// ---------------------------------------------------------------------------
namespace {

// worked-example's boot (its save lands: MARK 48), the fold clean.
struct Worked {
  const minijson::Value& sc = scenario("worked-example");
  const minijson::Value& b = sc["boots"][0];
  Files in = cardOf(b["in"]);
  static PlaySpec play(uint32_t i) {
    PlaySpec ps;
    ps.hash = 0x0123456789abcdefull + i % 40;
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
  uint64_t slots() const { return b["stats"]["head"]["slots"].u64(); }
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
  const Read r(card);
  TEST_ASSERT_EQUAL_STRING("loaded", sx::readName(r.o.read));
  Counts c;
  c.plays = r.fold.head().plays;
  c.skips = r.fold.head().skips;
  c.slots = r.fold.head().slots;
  c.listenedMs = r.fold.head().listenedMs;
  for (uint32_t i = 0; i < r.fold.marks(); ++i) {
    const sf::Fold::MarkRow* m = r.fold.markRow(i);
    c.mark[{std::string(reinterpret_cast<const char*>(m->dev), 6), m->epoch()}] = m->seq;
  }
  for (const auto& t : tracksOf(bytesOf(card, "stats.bin")))
    c.tracks[t.first] = {t.second.plays, t.second.skips, t.second.firstAt, t.second.lastAt, t.second.src};
  return c;
}

// The card's stats.bin's MARK for this player's epoch.
uint32_t cardMark(CutFs& card, const Player& p) {
  const Counts c = countsOf(card);
  const auto it = c.mark.find({std::string(reinterpret_cast<const char*>(p.id), 6), p.j->epoch()});
  return it == c.mark.end() ? 0 : it->second;
}

// app/PlayHistory::loop()'s turn at each pass the write gate lets through,
// the boot's scan done (statsglue::turnOf(): one thing of the card's a
// pass), over the real Journal, Tracker, Outbox and glue.
struct Loop {
  explicit Loop(Player& pl) : p(pl) {}
  Player& p;
  bool hsAsked = false;
  // PlayHistory::slotsDue(): the journal ready, the card not away (the
  // test's never is), the Outbox's head due.
  uint32_t slotsDue() { return p.j->ready() && p.tracker->pending() && p.out->due() ? p.tracker->pending() : 0; }
  sg::Turn pass(bool gateOpen = true) {
    if (!gateOpen) return sg::Turn::Nothing;
    const sg::Turn t = sg::turnOf(p.glue->core.saveDue(p.fenced), slotsDue(), hsAsked);
    switch (t) {
      case sg::Turn::Save: p.glue->core.saveNow(p.fenced); break;
      case sg::Turn::Slot: p.out->writeOne(); break;
      case sg::Turn::Hs:
        hsAsked = false;
        p.glue->core.hs(p.fenced);
        break;
      case sg::Turn::Nothing: break;
    }
    return t;
  }
  // A play that ends, then its slot at the next pass.
  void play(uint32_t i) {
    p.ended(Worked::play(i));
    TEST_ASSERT_TRUE(pass() == sg::Turn::Slot);
  }
};

}  // namespace

void test_the_saves_at_the_gate() {
  const Worked wk;
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  CutFs card;
  put(card, wk.in);
  Player p(card, dev);
  wk.boot(p);
  TEST_ASSERT_EQUAL_UINT32(48, cardMark(card, p));
  TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
  Loop l(p);
  uint32_t n = 0;  // the plays written this session
  // Run H: Hs asked while 4 plays wait behind a shut gate: the slots first,
  // one a pass, each folded live; Hs at the pass after them, its save
  // holding them.
  for (uint32_t i = 1; i <= 4; ++i) p.ended(Worked::play(i));
  l.hsAsked = true;
  for (int k = 0; k < 3; ++k) l.pass(false);
  TEST_ASSERT_EQUAL_UINT32(4, p.tracker->pending());
  for (uint32_t k = 1; k <= 4; ++k) {
    TEST_ASSERT_TRUE_MESSAGE(l.pass() == sg::Turn::Slot, "Hs before a play waiting for its slot");
    TEST_ASSERT_EQUAL_UINT32(k, p.glue->fold.records());
  }
  n += 4;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Hs);
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  TEST_ASSERT_EQUAL_UINT32(48 + n, cardMark(card, p));
  TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());

  // The records' save before the slot of its pass: the 64th record written,
  // the save due at the next pass the gate lets through, before the 65th's
  // slot (it holds exactly 64).
  for (uint32_t i = 0; i < 63; ++i) l.play(100 + i);
  n += 63;
  TEST_ASSERT_FALSE(p.glue->core.saveDue(false));
  p.ended(Worked::play(200));
  p.ended(Worked::play(201));
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Slot);
  ++n;
  TEST_ASSERT_EQUAL_UINT32(sf::kSaveAt, p.glue->fold.records());
  TEST_ASSERT_TRUE(p.glue->core.saveDue(false));
  // (Waits while the card is away.)
  p.glue->away = true;
  TEST_ASSERT_FALSE(p.glue->core.saveDue(false));
  p.glue->away = false;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Save);
  TEST_ASSERT_EQUAL_STRING("records", sg::kindName(p.glue->core.lastKind()));
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  TEST_ASSERT_FALSE(p.glue->last.compacted);
  TEST_ASSERT_EQUAL_UINT32(48 + n, cardMark(card, p));
  TEST_ASSERT_EQUAL_UINT32(1, p.tracker->pending());  // (the 65th, after the save)
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Slot);
  ++n;
  TEST_ASSERT_EQUAL_UINT32(1, p.glue->fold.records());

  // A library update's fence: no records' save behind it until the records
  // are full (96), then whatever the fence; Hs refused behind it.
  p.fenced = true;
  for (uint32_t i = 0; i < 95; ++i) {
    TEST_ASSERT_FALSE(p.glue->core.saveDue(true));
    l.play(300 + i);
  }
  n += 95;
  TEST_ASSERT_EQUAL_UINT32(sf::kRecords, p.glue->fold.records());
  l.hsAsked = true;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Save);  // (the save first, whatever the fence)
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  TEST_ASSERT_EQUAL_UINT32(48 + n, cardMark(card, p));
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Hs);
  TEST_ASSERT_EQUAL_STRING("fenced", refused(p));
  TEST_ASSERT_FALSE(p.glue->fold.behind());
  p.fenced = false;

  // A records' save that failed, then a save of another kind that took the
  // records (Hs): the records' save is due again from 64 new ones, not only
  // past the count it failed at.
  for (uint32_t i = 0; i < sf::kSaveAt; ++i) l.play(400 + i);
  p.glue->saveFails = 1;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Save);
  TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(p.glue->last.what));
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Nothing);
  l.hsAsked = true;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Hs);
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
  for (uint32_t i = 0; i < sf::kSaveAt; ++i) l.play(450 + i);
  n += 2 * sf::kSaveAt;
  TEST_ASSERT_TRUE(l.pass() == sg::Turn::Save);
  TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(p.glue->last.what));
  TEST_ASSERT_EQUAL_UINT32(48 + n, cardMark(card, p));

  // The card full from the 64th: tried at the pass after each later live
  // record (never twice for one); at 96 tried once more; the 97th finds the
  // records full: behind (no record, no save, Hs's too), never before.
  for (uint32_t i = 0; i < 63; ++i) l.play(500 + i);
  n += 63;
  p.glue->saveFails = -1;
  uint32_t tried = 0;
  for (uint32_t i = 0; i < sf::kRecords - sf::kSaveAt + 2; ++i) {
    l.play(600 + i);
    ++n;
    if (p.glue->fold.behind()) break;
    TEST_ASSERT_TRUE(l.pass() == sg::Turn::Save);
    TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(p.glue->last.what));
    ++tried;
    TEST_ASSERT_TRUE(l.pass() == sg::Turn::Nothing);  // (no later record: not again)
    TEST_ASSERT_FALSE(p.glue->fold.behind());
  }
  TEST_ASSERT_EQUAL_UINT32(sf::kRecords - sf::kSaveAt + 1, tried);  // (64 to 96)
  TEST_ASSERT_TRUE(p.glue->fold.behind());
  TEST_ASSERT_EQUAL_UINT32(0, p.glue->fold.records());
  TEST_ASSERT_FALSE(p.glue->core.saveDue(false));
  p.glue->saveFails = 0;
  TEST_ASSERT_FALSE(p.glue->hs());
  TEST_ASSERT_EQUAL_STRING("behind", refused(p));
  const uint32_t saved = 48 + n - (sf::kRecords + 1);
  TEST_ASSERT_EQUAL_UINT32(saved, cardMark(card, p));
  // The next boot folds every unsaved play once (the log holds them): the
  // card's stats then hold the whole session, each play once.
  Player q(card, p.id);
  deviceFor(q, wk.sc);
  q.nvs = p.nvs;
  TEST_ASSERT_TRUE(q.boot(wk.ask()));
  TEST_ASSERT_EQUAL_UINT32(sf::kRecords + 1, q.glue->fold.folded());
  TEST_ASSERT_EQUAL_UINT64(wk.slots() + n, q.glue->fold.head().slots);
  TEST_ASSERT_EQUAL_UINT32(48 + n, cardMark(card, q));
}

// A cut at every step of a records' save (64 plays written live), then a
// boot: the counts the uncut session leaves, every time (the 64 folded again
// by the boot when the save didn't land, none when it did).
void test_a_cut_during_the_records_save() {
  const Worked wk;
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  auto session = [&](CutFs& card, long cutAt) {
    Player p(card, dev);
    wk.boot(p);
    for (uint32_t i = 0; i < sf::kSaveAt; ++i) TEST_ASSERT_TRUE(p.write(Worked::play(i)) == pl::Outbox::Did::Written);
    TEST_ASSERT_TRUE(p.glue->core.recordsDue(false));
    if (cutAt > 0) card.cutAt = card.steps + cutAt;
    p.glue->core.recordsSave();
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
  TEST_ASSERT_EQUAL_UINT32(0, folded);  // (the 64 in the records' stats.bin)
  const Counts want = countsOf(ref);
  TEST_ASSERT_EQUAL_UINT64(wk.slots() + sf::kSaveAt, want.slots);
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
      std::snprintf(w, sizeof(w), "the records' save cut at its step %ld, way %d", k, static_cast<int>(v));
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, card.violations.size(), w);
      CutFs after = card.reboot(v);
      next(after, n, w, &folded);
      TEST_ASSERT_TRUE_MESSAGE(folded == 0 || folded == sf::kSaveAt, w);
      again += folded ? 1 : 0;
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, after.violations.size(), w);
      partTwins(after);
      TEST_ASSERT_TRUE_MESSAGE(countsOf(after) == want, w);
      ++runs;
    }
    if (!cut) break;
    ++steps;
  }
  printf("the records' save: %d steps cut three ways (%d runs), then a boot: the uncut counts each time (the 64 "
         "folded again after %d of them)\n",
         steps, runs, again);
  TEST_ASSERT_TRUE(steps >= 5);
  TEST_ASSERT_TRUE(again > 0 && again < runs);
}

// ---------------------------------------------------------------------------
// The boot's overflow (300 slots of a 0.14.0 history past the 96 records:
// one chunk) and its save's tries: held through the boot's waits (100, 300
// and 1,000 ms, kBootTries tries in all), a track's start and a library
// update waiting meanwhile (overflowHeld(), scanHolds()); given back when a
// try lands or is held, else after the last (behind); at once by the end's
// flush. A live slot meanwhile goes in the last chunk's room.
// ---------------------------------------------------------------------------
namespace {

struct Overflow {
  const minijson::Value& sc = session("boot-overflow");
  const minijson::Value& st = sc["steps"][0];
  uint8_t dev[6];
  Overflow() { deviceOf(sc["players"]["A"]["device"].str(), dev); }
  void player(Player& p) const {
    const minijson::Value& nvs = sc["players"]["A"]["nvs"];
    p.nvs.ids.epoch = nvs[0].u64();
    p.nvs.ids.seq = static_cast<uint32_t>(nvs[1].u64());
    p.nvs.ids.boots = static_cast<uint32_t>(nvs[2].u64());
    p.piece = 128;
    p.buf.assign(pl::kReadBytes, 0);
    p.mem.alloc = heapAlloc;
    p.mem.release = heapFree;
  }
  BootAsk ask(int saveFails) const {
    BootAsk a = askOf(st);
    a.saveFails = saveFails;
    return a;
  }
};

}  // namespace

void test_the_boot_overflows_tries() {
  const Overflow ov;
  const uint32_t cap = static_cast<uint32_t>(ov.sc["cap"].u64());
  // Every try fails: given back after the fourth, the session behind; the
  // card as it was; the next boot folds the 300 again.
  {
    CutFs card;
    put(card, sessionCard(ov.sc["in"]));
    Player p(card, ov.dev, cap);
    ov.player(p);
    gHeap = Heap();
    p.begin(ov.ask(-1));
    const size_t atRead = gHeap.held;
    bool held = false;
    for (int guard = 0; guard < 100000; ++guard) {
      p.glue->reserve(p.piece);
      held = held || p.glue->core.overflowHeld();
      if (p.j->scan(p.piece)) break;
    }
    p.glue->ready();
    TEST_ASSERT_TRUE(held);
    sg::Glue& g = p.glue->core;
    TEST_ASSERT_EQUAL_UINT32(300, g.fold().folded());
    TEST_ASSERT_EQUAL_UINT32(300, g.fold().records());
    TEST_ASSERT_EQUAL_UINT32(1, g.fold().chunks());
    TEST_ASSERT_TRUE(g.retrying());
    TEST_ASSERT_EQUAL_UINT32(1, g.tries());
    TEST_ASSERT_TRUE(g.overflowHeld() && g.scanHolds());
    TEST_ASSERT_FALSE(g.rangesHeld());  // (the ranges' block given back at the scan's end)
    TEST_ASSERT_EQUAL_size_t(atRead + sf::kChunkBytes, gHeap.held);
    TEST_ASSERT_FALSE(g.hs(false));
    TEST_ASSERT_EQUAL_STRING("booting", refused(p));
    TEST_ASSERT_FALSE(g.recordsDue(false));
    for (uint32_t t = 0; t + 1 < sg::kBootTries; ++t) {
      const uint64_t at = g.retryAtMs();
      TEST_ASSERT_EQUAL_UINT64(p.oenv.now + tagstore::kBootWaitsMs[t], at);
      p.oenv.now = at - 1;
      TEST_ASSERT_FALSE(g.saveDue(false));
      p.oenv.now = at;
      TEST_ASSERT_TRUE(g.saveDue(false));
      TEST_ASSERT_FALSE(g.saveNow(false));
      TEST_ASSERT_EQUAL_UINT32(t + 2, g.tries());
      TEST_ASSERT_EQUAL_STRING("failed", sx::savedName(g.last().what));
    }
    TEST_ASSERT_FALSE(g.retrying());
    TEST_ASSERT_TRUE(g.fold().behind());
    TEST_ASSERT_EQUAL_UINT32(0, g.fold().chunks());
    TEST_ASSERT_FALSE(g.overflowHeld() || g.scanHolds());
    TEST_ASSERT_EQUAL_size_t(atRead, gHeap.held);
    TEST_ASSERT_EQUAL_UINT32(0, card.dir.count(sx::kStatsBin));
    Player q(card, ov.dev, cap);
    q.nvs = p.nvs;
    TEST_ASSERT_TRUE(q.boot(ov.ask(0)));
    TEST_ASSERT_EQUAL_UINT32(300, q.glue->fold.folded());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(q.glue->last.what));
  }
  // The first two fail, the third lands; a live play meanwhile in the last
  // chunk's room, held by it: nothing behind, the next boot folds none.
  {
    CutFs card;
    put(card, sessionCard(ov.sc["in"]));
    Player p(card, ov.dev, cap);
    ov.player(p);
    TEST_ASSERT_TRUE(p.boot(ov.ask(2)));
    sg::Glue& g = p.glue->core;
    TEST_ASSERT_TRUE(g.retrying());
    TEST_ASSERT_TRUE(p.write(Worked::play(7)) == pl::Outbox::Did::Written);
    TEST_ASSERT_EQUAL_UINT32(301, g.fold().records());
    while (g.retrying()) {
      p.oenv.now = g.retryAtMs();
      g.saveNow(false);
    }
    TEST_ASSERT_EQUAL_UINT32(3, g.tries());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(g.last().what));
    TEST_ASSERT_FALSE(g.fold().behind());
    TEST_ASSERT_EQUAL_UINT32(0, g.fold().records());
    TEST_ASSERT_FALSE(g.overflowHeld() || g.scanHolds());
    TEST_ASSERT_EQUAL_UINT32(301, cardMark(card, p));
    Player q(card, ov.dev, cap);
    q.nvs = p.nvs;
    TEST_ASSERT_TRUE(q.boot(ov.ask(0)));
    TEST_ASSERT_EQUAL_UINT32(0, q.glue->fold.folded());
    TEST_ASSERT_EQUAL_UINT64(301, q.glue->fold.head().slots);
  }
  // The orderly end while the tries wait: given back at once, behind.
  {
    CutFs card;
    put(card, sessionCard(ov.sc["in"]));
    Player p(card, ov.dev, cap);
    ov.player(p);
    TEST_ASSERT_TRUE(p.boot(ov.ask(1)));
    sg::Glue& g = p.glue->core;
    TEST_ASSERT_TRUE(g.retrying() && g.overflowHeld());
    g.ending(p.oenv.now + 3000);
    TEST_ASSERT_FALSE(g.retrying());
    TEST_ASSERT_TRUE(g.fold().behind());
    TEST_ASSERT_FALSE(g.overflowHeld() || g.scanHolds());
    p.oenv.now += 10000;
    TEST_ASSERT_FALSE(g.saveDue(false));
  }
}

// ---------------------------------------------------------------------------
// MARK's room: the object's 16 rows, or a block for a card with more than 8
// (its rows and 8 more), carved by the boot's read and kept; a remount's
// read whose MARK passes it (another player wrote the card while it was
// out) is a read with no memory: the stats off for the session (no fold, no
// save, no trim), never a carve and never a MARK cut short.
// ---------------------------------------------------------------------------
namespace {

// A play slot of `dev` (its seq 1, epoch `epoch`), dated.
std::vector<uint8_t> slotOf(const uint8_t dev[6], uint64_t epoch, uint32_t i) {
  Slot s;
  s.kind = pl::kKindPlay;
  s.outcome = pl::kCompleted;
  s.source = pl::kSourceManual;
  s.clock = pl::kClockRtc;
  s.seq = 1;
  s.epoch = epoch;
  std::memcpy(s.deviceId, dev, 6);
  s.startedAt = sf::kMinMs + 313ull * sf::kDayMs + i * 60000ull;
  s.bootSeq = 1;
  s.uptimeMs = 1000;
  s.playedMs = 120000;
  s.durationMs = 200000;
  s.pathHash = 0x5eed000000000000ull + i;
  std::vector<uint8_t> b(pl::kSlot);
  pl::encode(s, b.data());
  return b;
}

// The log with the slots of devices [from, to) added (one each).
void addDevices(CutFs& card, uint32_t from, uint32_t to) {
  std::vector<uint8_t> log = card.dir.count(kLog) ? card.bytes(kLog) : std::vector<uint8_t>();
  for (uint32_t d = from; d < to; ++d) {
    const uint8_t dev[6] = {0x02, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(d)};
    const std::vector<uint8_t> s = slotOf(dev, 0x0BADC0FFEE000000ull + d, d);
    log.insert(log.end(), s.begin(), s.end());
  }
  card.put(kLog, log);
}

}  // namespace

void test_marks_room_and_a_remount_past_it() {
  const uint8_t me[6] = {0x02, 0xAA, 0, 0, 0, 1};
  BootAsk a;
  a.ask.nowMs = sf::kMinMs + 314ull * sf::kDayMs;
  a.ask.playing = true;
  a.ask.safePoint = false;
  // 12 players' slots: MARK 12 rows (the object's 16 hold them).
  CutFs card;
  addDevices(card, 0, 12);
  {
    Player x(card, me);
    x.nvs.ids.epoch = 0x77;
    TEST_ASSERT_TRUE(x.boot(a));
    TEST_ASSERT_EQUAL_UINT32(0, x.glue->first.markBytes);
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(x.glue->last.what));
    TEST_ASSERT_EQUAL_UINT32(12, rowsOf(card, sf::kMark));
  }
  // Another card grown to 28 rows, 8 new players a boot: each boot's read
  // carves MARK's block (its rows and 8 more), and the fold fills it.
  CutFs other;
  put(other, filesOf(card));
  for (uint32_t rows = 12; rows < 28; rows += 8) {
    addDevices(other, rows, rows + 8);
    Player y(other, me);
    y.nvs.ids.epoch = 0x77;
    y.mem.alloc = heapAlloc;
    y.mem.release = heapFree;
    TEST_ASSERT_TRUE(y.boot(a));
    TEST_ASSERT_EQUAL_UINT32(rows, y.glue->first.marks);
    TEST_ASSERT_EQUAL_UINT32(sf::Fold::markBlockBytes(rows), y.glue->first.markBytes);
    TEST_ASSERT_EQUAL_UINT32(rows + sf::kMarkRoom, y.glue->fold.markCap());
    TEST_ASSERT_EQUAL_STRING("wrote", sx::savedName(y.glue->last.what));
    TEST_ASSERT_EQUAL_UINT32(rows + 8, rowsOf(other, sf::kMark));
  }
  // A player booting on the first card: its block holds 20 rows. A ninth
  // new pair in one boot is the fold out of memory (no save, no trim by it).
  Player g(card, me);
  g.nvs.ids.epoch = 0x77;
  g.mem.alloc = heapAlloc;
  g.mem.release = heapFree;
  TEST_ASSERT_TRUE(g.boot(a));
  TEST_ASSERT_EQUAL_UINT32(20, g.glue->fold.markCap());
  TEST_ASSERT_TRUE(g.glue->core.on());
  // The card's stats.bin replaced by the other's (28 rows: another player's
  // while it was out), then the card back: a remount's read, one, no carve.
  put(card, {{"stats.bin", bytesOf(other, "stats.bin")}});
  const uint32_t carves = gHeap.carves;
  const size_t held = gHeap.held;
  g.j->remounted();
  TEST_ASSERT_TRUE(g.scan());
  TEST_ASSERT_EQUAL_STRING("noMemory", sx::readName(g.glue->opened.read));
  TEST_ASSERT_EQUAL_UINT32(28, g.glue->opened.marks);
  TEST_ASSERT_FALSE(g.glue->core.on());
  TEST_ASSERT_EQUAL_UINT32(carves + 1, gHeap.carves);  // (the read's work memory alone)
  TEST_ASSERT_EQUAL_size_t(held, gHeap.held);
  TEST_ASSERT_EQUAL_STRING("save boot (no)", g.glue->calls.back().c_str());
  TEST_ASSERT_EQUAL_STRING("off", refused(g));
  TEST_ASSERT_TRUE(g.trimmed().empty());
  TEST_ASSERT_FALSE(g.glue->hs());
  TEST_ASSERT_EQUAL_STRING("off", refused(g));
  TEST_ASSERT_TRUE(bytesOf(card, "stats.bin") == bytesOf(other, "stats.bin"));  // (never a MARK cut short)
}

// ---------------------------------------------------------------------------
// The boot's scan and a library update, never both waiting (5.14): before
// the fold's first piece the scan waits for an asked update (nothing carved
// behind its check or during its build); once begun, the update waits for
// the scan's blocks (scanHolds(): LibraryUpdate's Wait::Stats; test_library_
// update has the step's side) until the boot's save gives them back, and
// the scan goes on. Auto DJ's join: the scan's pieces wait while it works,
// and it doesn't begin while the scan holds blocks.
// ---------------------------------------------------------------------------
void test_the_scan_and_a_library_update() {
  const Worked wk;
  const uint8_t* dev = reinterpret_cast<const uint8_t*>("\0\0\0\0\0\1");
  {
    CutFs card;
    put(card, wk.in);
    Player p(card, dev);
    deviceFor(p, wk.sc);
    p.mem.alloc = heapAlloc;
    p.mem.release = heapFree;
    p.begin(wk.ask());
    sg::Glue& g = p.glue->core;
    const uint32_t carves = gHeap.carves;
    // An update asked before the first piece: the scan waits (as many
    // passes as it is asked), nothing carved, nothing held for it to wait on.
    for (int k = 0; k < 5; ++k) {
      TEST_ASSERT_TRUE(g.scanWaits(true, false) == sg::ScanWait::Update);
      TEST_ASSERT_FALSE(g.scanHolds());
    }
    TEST_ASSERT_TRUE(g.scanWaits(false, true) == sg::ScanWait::Join);
    TEST_ASSERT_EQUAL_UINT32(carves, gHeap.carves);
    // The update over: the first piece takes its room.
    TEST_ASSERT_TRUE(g.scanWaits(false, false) == sg::ScanWait::None);
    g.reserve(1);
    TEST_ASSERT_FALSE(p.j->scan(1));
    TEST_ASSERT_TRUE(g.scanHolds());
    // An update asked now: the scan goes on (only the join holds it), and
    // the update waits for its blocks until the boot's save.
    int pieces = 0;
    for (;;) {
      TEST_ASSERT_TRUE(g.scanWaits(true, false) == sg::ScanWait::None);
      TEST_ASSERT_TRUE(g.scanWaits(true, true) == sg::ScanWait::Join);
      TEST_ASSERT_TRUE(g.scanHolds());
      g.reserve(1);
      ++pieces;
      if (p.j->scan(1)) break;
    }
    p.glue->ready();
    TEST_ASSERT_TRUE(pieces > 3);
    TEST_ASSERT_FALSE(g.scanHolds());
    TEST_ASSERT_TRUE(g.scanWaits(true, true) == sg::ScanWait::None);
    sameFiles(cardOf(wk.b["out"]), filesOf(card), "the scan with an update asked");
    // A remount after Ready: its read pending, the scan's first piece waits
    // for an asked update again.
    p.j->remounted();
    TEST_ASSERT_TRUE(g.scanWaits(true, false) == sg::ScanWait::Update);
    TEST_ASSERT_FALSE(g.scanHolds());
  }
  // An overflow whose save failed once: the update waits through its tries
  // (the scan's blocks), until one lands.
  {
    const Overflow ov;
    const uint32_t cap = static_cast<uint32_t>(ov.sc["cap"].u64());
    CutFs card;
    put(card, sessionCard(ov.sc["in"]));
    Player p(card, ov.dev, cap);
    ov.player(p);
    TEST_ASSERT_TRUE(p.boot(ov.ask(1)));
    sg::Glue& g = p.glue->core;
    TEST_ASSERT_TRUE(g.scanHolds() && g.overflowHeld());
    TEST_ASSERT_TRUE(g.scanWaits(true, false) == sg::ScanWait::None);
    p.oenv.now = g.retryAtMs();
    TEST_ASSERT_TRUE(g.saveNow(false));
    TEST_ASSERT_FALSE(g.scanHolds() || g.overflowHeld());
  }
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
  RUN_TEST(test_the_saves_at_the_gate);
  RUN_TEST(test_a_cut_during_the_records_save);
  RUN_TEST(test_the_boot_overflows_tries);
  RUN_TEST(test_marks_room_and_a_remount_past_it);
  RUN_TEST(test_the_scan_and_a_library_update);
  return UNITY_END();
}
