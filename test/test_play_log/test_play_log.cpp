// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for the play log (lib/core PlayLog; docs/METADATA.md
// 2.20, 2.5.6), against tools/playlog.py's fixtures (test/fixtures/card/
// plays, vectors.json's plays group):
// - the slot and the drop note byte for byte; decode()'s verdicts on the
//   reader's cases (damaged, torn, a tail, a newer major, an unknown kind);
// - the goldens: log.json's events (boots, plays, a host's @time, the
//   ACKS of a sync, a flasher's NVS erase) through the Journal over a
//   card in memory give plays/golden/<snapshot>/ byte for byte, and the
//   reference model's ops one for one;
// - the boot rules (vectors' atBoot: NVS behind or ahead of the card, a new
//   epoch, another device, a conflict's two slots, NVS put back behind a
//   trimmed log and ACKS's seq) and the boot's scan in pieces of any size;
//   the seq rule's reservations, ACKS's and the stats' MARK;
// - ACKS (2.5.6) read on its own: the golden root's entries, the
//   hardening files (damaged, out of order, short rows: absent, the root
//   present; a newer minor's longer rows read by their prefix); this
//   board's rows kept first when the section has more than 16;
// - the trim's rules, a cut append's tail, NVS refusing the seq, the seq's
//   last value taking a new epoch;
// - a power cut at every step of appends and two rotations, the card as
//   the next boot finds it three ways (CutFs): no chain freed that another
//   name uses, no slot credited wrongly (every valid slot is one the
//   journal wrote, byte for byte), no seq used twice, the log going on;
//   the twins a cut rename leaves, collected after a disk check; all nine
//   twin names taken: blocked; a rename the card refuses, tried again; a
//   remount settling again;
// - the Tracker (the time played, pauses excluded, a seek's jump not
//   counted, one as long as a pause before it neither, the tail of a
//   completed play, short plays, the queue) and the WriteGate (the gapless
//   window, a start's settle, the ring, underruns, a moving list; a run of
//   7 s tracks, which it never opens for: the ring of 32);
// - a slot the card fails, tried again (Journal::again()): the same seq and
//   bytes, once on the card whether the first try's write landed or not
//   (on FatFs too: the directory's write refused); a rotation a step of
//   which fails ending as one nothing failed, its drop note kept; a
//   plays.1.jnl that can't be read, once and for good; the Outbox: a play
//   the card fails first in line for three tries, nothing behind it
//   before it, after a remount, and the orderly end's flush (the first
//   card failure, the deadline);
// - what an append, a rotation and the boot's scan ask of the card, on the
//   host FatFs model (ChaN's FatFs as the Core2 builds it, the sector
//   cache in front: test_card_io's card): bytes per play and the cap's
//   worst case; a cut append's tail written over; a remount during the
//   boot's scan;
// - the listening stats' hooks (playlog::Folds, a MemFolds; docs/
//   LISTENING-STATS.md 5.5, 5.6): a new epoch read with folds, a scan
//   begun again; the trim waits for the folds' save, and after a failed one
//   takes what the card's MARK covers; a rotation saves first (a twin's
//   rename too, a plays.1.jnl not read whole); the orderly end's flush
//   (the boot's save refused, a rotation's with 1 s left); a partial scan
//   (an open or a read the card failed): partial(), no save; the Outbox's
//   written(). With no Folds every case above runs as it always did.
// Run: pio test -e native -f test_play_log
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/FatModel.h"
#include "../support/FatModelFs.h"
#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "../support/FaultFs.h"
#include "CachedDrive.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardManifest.h"
#include "PlayLog.h"
#include "SectorCache.h"

namespace cc = cardcontract;
namespace msmf = cardcontract::msmf;
namespace pl = playlog;
using Bytes = std::vector<uint8_t>;
using cutfs::CutFs;
using faultfs::FaultFs;
using faultfs::FlakyFs;
using faultfs::Knob;
using minijson::Value;
using pl::Journal;
using pl::Slot;

namespace {

Value gVectors;  // vectors.json
Value gScript;   // plays/log.json
Value gIndex;    // plays/index.json

Bytes fixture(const std::string& rel) {
  const std::string s = cardfixtures::bytes(rel);
  return Bytes(s.begin(), s.end());
}

Bytes hexBytes(const std::string& s) {
  Bytes b;
  for (size_t i = 0; i + 1 < s.size();) {
    if (s[i] == ' ' || s[i] == ':' || s[i] == '-') {
      ++i;
      continue;
    }
    b.push_back(static_cast<uint8_t>(std::stoul(s.substr(i, 2), nullptr, 16)));
    i += 2;
  }
  return b;
}

void deviceOf(const Value& v, uint8_t out[6]) {
  const Bytes b = hexBytes(v.str());
  TEST_ASSERT_EQUAL_size_t(6, b.size());
  std::memcpy(out, b.data(), 6);
}

const uint8_t kDevA[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x01};  // RFC 7042's documentation MACs
const uint8_t kDevB[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, 0x02};

// NVS in memory: always takes a write unless told not to.
struct MemNvs : Journal::Nvs {
  Journal::Ids ids;  // epoch 0: none (erased)
  bool failSeq = false, failBoots = false, failLoad = false;
  uint32_t seqWrites = 0;
  bool load(Journal::Ids* out) override {
    if (failLoad) return false;
    *out = ids;
    return true;
  }
  bool storeSeq(uint32_t s) override {
    if (failSeq) return false;
    ids.seq = s;
    ++seqWrites;
    return true;
  }
  bool storeBoots(uint32_t b) override {
    if (failBoots) return false;
    ids.boots = b;
    return true;
  }
  bool storeEpoch(const Journal::Ids& i) override {
    ids = i;
    return true;
  }
  void erase() { ids = Journal::Ids(); }
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

// The journal's ops, as tools/playlog.py's Device writes them.
void onOp(void* ctx, Journal::Op o, const char* a, const char* b, uint64_t n) {
  auto* v = static_cast<std::vector<std::string>*>(ctx);
  char t[96];
  switch (o) {
    case Journal::Op::Epoch: std::snprintf(t, sizeof(t), "epoch %016llx", static_cast<unsigned long long>(n)); break;
    case Journal::Op::Play: std::snprintf(t, sizeof(t), "play %llu", static_cast<unsigned long long>(n)); break;
    case Journal::Op::Lost: std::snprintf(t, sizeof(t), "lost %llu", static_cast<unsigned long long>(n)); break;
    case Journal::Op::Note: std::snprintf(t, sizeof(t), "note %llu", static_cast<unsigned long long>(n)); break;
    case Journal::Op::Rename: std::snprintf(t, sizeof(t), "rename %s %s", base(a).c_str(), base(b).c_str()); break;
    case Journal::Op::Delete: std::snprintf(t, sizeof(t), "delete %s %s", base(a).c_str(), b); break;
    case Journal::Op::Twin: std::snprintf(t, sizeof(t), "twin %s %s", base(a).c_str(), base(b).c_str()); break;
    case Journal::Op::Blocked: std::snprintf(t, sizeof(t), "blocked %s", base(a).c_str()); break;
    case Journal::Op::Refused: std::snprintf(t, sizeof(t), "refused %s %s", base(a).c_str(), base(b).c_str()); break;
    case Journal::Op::Unread:
      std::snprintf(t, sizeof(t), "unread %s %llu", base(a).c_str(), static_cast<unsigned long long>(n));
      break;
  }
  v->push_back(t);
}

// A device: its NVS, its epochs, the journal over a card (`fs`).
struct Device {
  MemNvs nvs;
  Epochs epochs;
  std::vector<std::string> ops;
  Bytes buf = Bytes(3 * pl::kSlot);  // a few slots a read: the scan's pieces cross reads
  uint8_t id[6];
  uint32_t cap;
  msmf::Acks acks;
  pl::Folds* folds = nullptr;  // the listening stats (none: the journal as without them)
  std::unique_ptr<Journal> j;
  explicit Device(const uint8_t dev[6] = kDevA, uint32_t capSlots = pl::kFileSlots) : cap(capSlots) {
    std::memcpy(id, dev, 6);
  }
  Journal::Config config() {
    Journal::Config c;
    c.capSlots = cap;
    c.buf = buf.data();
    c.bufBytes = static_cast<uint32_t>(buf.size());
    c.random = nextEpoch;
    c.randomCtx = &epochs;
    c.onOp = onOp;
    c.opCtx = &ops;
    c.folds = folds;
    return c;
  }
  // A boot with this card: begin(), then the scan in pieces of `piece`.
  bool boot(tagstore::Fs& fs, uint32_t piece = 5) {
    j.reset(new Journal(fs, nvs, config()));
    if (!j->begin(id, acks)) return false;
    for (int guard = 0; guard < 100000 && !j->scan(piece); ++guard) {
    }
    return j->ready();
  }
};

// A play's own fields (the rest the journal fills).
Slot play(uint64_t pathHash, uint32_t playedMs, uint32_t uptimeMs, uint8_t outcome = pl::kCompleted) {
  Slot s;
  s.outcome = outcome;
  s.source = pl::kSourceManual;
  s.clock = pl::kClockRtc;
  s.startedAt = 1792100000000ull + uptimeMs;
  s.uptimeMs = uptimeMs;
  s.playedMs = playedMs;
  s.durationMs = playedMs;
  s.pathHash = pathHash;
  s.size = 1000;
  s.fatTime = 0x5D4773D5;
  s.generation = 42;
  return s;
}

void setAcks(msmf::Acks& a, std::initializer_list<msmf::Ack> list) {
  a = msmf::Acks();
  for (const msmf::Ack& e : list) a.entries[a.count++] = e;
  a.rows = a.count;
  a.present = true;
}
msmf::Ack ack(const uint8_t dev[6], uint64_t epoch, uint32_t seq) {
  msmf::Ack e;
  std::memcpy(e.deviceId, dev, 6);
  e.epoch = epoch;
  e.seq = seq;
  return e;
}

// The log's files on a card (CutFs's names).
const char* const kLog = pl::kLogPath;
const char* const kPrev = pl::kPrevPath;

// The listening stats as the journal sees them (playlog::Folds), in
// memory: MARK by (deviceId, epoch), the card's (`card`: read at the boot,
// or the last save's) and RAM's (`ram`: with the folds since). A play slot
// is folded once, when its seq is above the card's MARK at scanBegan();
// scanBegan() drops RAM's folds (stats.bin read again). A save copies
// RAM's MARK to the card's, unless the scan was partial, the card fails it
// (failSave), or the glue's own rule (`may`: the orderly end's) says no.
struct MemFolds : pl::Folds {
  using Key = std::pair<std::string, uint64_t>;
  static Key key(const uint8_t dev[6], uint64_t epoch) {
    return Key(std::string(reinterpret_cast<const char*>(dev), 6), epoch);
  }
  std::map<Key, uint32_t> card, ram, atBoot;
  std::set<std::pair<Key, uint32_t>> took;  // the slots folded since scanBegan()
  std::vector<uint32_t> folded;             // their seqs, in order (the live ones too)
  std::vector<int> files;                   // slot()'s `file`, for each slot
  std::vector<std::string> calls;           // began, began again, partial, save boot (no), save rotation (no)
  std::vector<std::string> seen;            // the log's files on the card at each save
  bool partialScan = false;
  bool failSave = false;
  uint32_t saves = 0;                       // saves that landed
  std::function<bool(Why)> may;             // the glue's own word (none: yes)
  const CutFs* on = nullptr;                // the card, for `seen`

  void scanBegan(bool again) override {
    calls.push_back(again ? "began again" : "began");
    ram = atBoot = card;
    took.clear();
    folded.clear();
    files.clear();
    partialScan = false;
  }
  void slot(const Slot& s, int file) override {
    files.push_back(file);
    if (s.kind == pl::kKindPlay) fold(s);
  }
  // A play slot into RAM (the boot's fold, and the live one's: written()).
  void fold(const Slot& s) {
    const Key k = key(s.deviceId, s.epoch);
    auto b = atBoot.find(k);
    if ((b != atBoot.end() && s.seq <= b->second) || !took.insert(std::make_pair(k, s.seq)).second) return;
    folded.push_back(s.seq);
    uint32_t& m = ram[k];
    m = std::max(m, s.seq);
  }
  void partial() override {
    calls.push_back("partial");
    partialScan = true;
  }
  bool covered(const Slot& s) override {
    auto c = card.find(key(s.deviceId, s.epoch));
    return c != card.end() && s.seq <= c->second;
  }
  uint32_t markOf(const uint8_t dev[6], uint64_t epoch) override {
    auto r = ram.find(key(dev, epoch));
    return r == ram.end() ? 0 : r->second;
  }
  bool save(Why why) override {
    std::string what = why == Why::Boot ? "save boot" : "save rotation";
    if (on) {
      std::string names;
      for (const char* n : {kPrev, kLog})
        if (on->dir.count(n)) names += (names.empty() ? "" : " ") + base(n);
      seen.push_back(names);
    }
    const bool ok = !partialScan && !failSave && (!may || may(why));
    calls.push_back(ok ? what : what + " (no)");
    if (!ok) return false;
    card = ram;
    ++saves;
    return true;
  }
  uint32_t cardMark(const uint8_t dev[6], uint64_t epoch) const {
    auto c = card.find(key(dev, epoch));
    return c == card.end() ? 0 : c->second;
  }
};
using Calls = std::vector<std::string>;

// A memory Source over bytes.
struct VecSource : cc::Source {
  Bytes b;
  explicit VecSource(Bytes d) : b(std::move(d)) {}
  uint32_t size() const override { return static_cast<uint32_t>(b.size()); }
  bool read(uint32_t off, void* out, uint32_t n) override {
    if (off > b.size() || n > b.size() - off) return false;
    std::memcpy(out, b.data() + off, n);
    return true;
  }
};

void assertSlotFields(const Value& f, const Slot& s) {
  TEST_ASSERT_EQUAL_UINT8(f["kind"].u64(), s.kind);
  TEST_ASSERT_EQUAL_UINT64(f["epoch"].u64(), s.epoch);
  uint8_t dev[6];
  deviceOf(f["deviceId"], dev);
  TEST_ASSERT_EQUAL_MEMORY(dev, s.deviceId, 6);
  TEST_ASSERT_EQUAL_UINT8(f["clock"].u64(), s.clock);
  TEST_ASSERT_EQUAL_UINT64(f["startedAt"].u64(), s.startedAt);
  TEST_ASSERT_EQUAL_UINT32(f["bootSeq"].u64(), s.bootSeq);
  TEST_ASSERT_EQUAL_UINT32(f["uptimeMs"].u64(), s.uptimeMs);
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// The slot (2.20.3).
// ---------------------------------------------------------------------------
void test_the_slot_and_the_drop_note_byte_for_byte() {
  const Value& v = gVectors["plays"]["slot"];
  const Value& f = v["fields"];
  Slot s;
  s.kind = static_cast<uint8_t>(f["kind"].u64());
  s.outcome = static_cast<uint8_t>(f["outcome"].u64());
  s.source = static_cast<uint8_t>(f["source"].u64());
  s.seq = static_cast<uint32_t>(f["seq"].u64());
  s.pauseCount = static_cast<uint16_t>(f["pauseCount"].u64());
  s.clock = static_cast<uint8_t>(f["clock"].u64());
  s.epoch = f["epoch"].u64();
  deviceOf(f["deviceId"], s.deviceId);
  s.startedAt = f["startedAt"].u64();
  s.bootSeq = static_cast<uint32_t>(f["bootSeq"].u64());
  s.uptimeMs = static_cast<uint32_t>(f["uptimeMs"].u64());
  s.playedMs = static_cast<uint32_t>(f["playedMs"].u64());
  s.durationMs = static_cast<uint32_t>(f["durationMs"].u64());
  s.pathHash = f["pathHash"].u64();
  s.size = static_cast<uint32_t>(f["size"].u64());
  s.fatTime = static_cast<uint32_t>(f["fatTime"].u64());
  s.generation = static_cast<uint32_t>(f["generation"].u64());
  uint8_t out[pl::kSlot];
  pl::encode(s, out);
  const Bytes want = hexBytes(v["bytes"].str());
  TEST_ASSERT_EQUAL_size_t(pl::kSlot, want.size());
  TEST_ASSERT_EQUAL_MEMORY(want.data(), out, pl::kSlot);
  TEST_ASSERT_EQUAL_UINT32(v["crc"].u64(), cc::get32(out + 76));
  Slot back;
  TEST_ASSERT_TRUE(pl::decode(out, pl::kSlot, &back) == pl::Verdict::Ok);
  uint8_t again[pl::kSlot];
  pl::encode(back, again);
  TEST_ASSERT_EQUAL_MEMORY(out, again, pl::kSlot);
  assertSlotFields(f, back);
  TEST_ASSERT_EQUAL_UINT64(s.pathHash, back.pathHash);
  TEST_ASSERT_EQUAL_UINT32(s.fatTime, back.fatTime);

  // The drop note: its play fields written 0 whatever the struct holds.
  const Value& d = gVectors["plays"]["dropNote"];
  Slot n = back;  // (a play's fields set: they must not reach the note)
  n.kind = pl::kKindDrop;
  n.clock = static_cast<uint8_t>(d["fields"]["clock"].u64());
  n.epoch = d["fields"]["epoch"].u64();
  n.startedAt = d["fields"]["startedAt"].u64();
  n.bootSeq = static_cast<uint32_t>(d["fields"]["bootSeq"].u64());
  n.uptimeMs = static_cast<uint32_t>(d["fields"]["uptimeMs"].u64());
  n.playedMs = static_cast<uint32_t>(d["fields"]["dropped"].u64());
  pl::encode(n, out);
  const Bytes note = hexBytes(d["bytes"].str());
  TEST_ASSERT_EQUAL_MEMORY(note.data(), out, pl::kSlot);
  TEST_ASSERT_TRUE(pl::decode(out, pl::kSlot, &back) == pl::Verdict::Ok);
  assertSlotFields(d["fields"], back);
  TEST_ASSERT_EQUAL_UINT32(4, back.playedMs);
  TEST_ASSERT_EQUAL_UINT32(0, back.seq);
}

// decode()'s verdicts, in 2.20.6's order: what the reader's cases count as
// slots, damaged ones, tails, a newer major's and unknown kinds.
void test_the_verdicts_on_the_readers_cases() {
  TEST_ASSERT_TRUE(gIndex.size() >= 14);
  for (size_t c = 0; c < gIndex.size(); ++c) {
    const Value& e = gIndex[c];
    uint32_t slots = 0, damaged = 0, tail = 0, newer = 0, kinds = 0;
    for (const auto& kv : e["files"].members) {
      const Bytes b = fixture("plays/" + kv.second.str());
      TEST_ASSERT_TRUE_MESSAGE(!b.empty(), kv.second.c_str());
      for (size_t off = 0; off < b.size(); off += pl::kSlot) {
        Slot s;
        const uint32_t n = static_cast<uint32_t>(std::min<size_t>(pl::kSlot, b.size() - off));
        switch (pl::decode(b.data() + off, n, &s)) {
          case pl::Verdict::Short: ++tail; continue;
          case pl::Verdict::Magic:
          case pl::Verdict::Crc: ++damaged; break;
          case pl::Verdict::Newer: ++newer; break;
          case pl::Verdict::Kind: ++kinds; break;
          case pl::Verdict::Ok: break;
        }
        ++slots;
      }
    }
    const Value& want = e["counts"];
    const char* name = e["case"].c_str();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["slots"].u64(), slots, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["damaged"].u64(), damaged, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["tail"].u64(), tail, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["newer"].u64(), newer, name);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want["unknownKinds"].u64(), kinds, name);
  }
}

// ---------------------------------------------------------------------------
// The goldens: log.json's events through the Journal.
// ---------------------------------------------------------------------------
void test_the_goldens_from_the_script() {
  const Value& dev = gScript["device"];
  uint8_t id[6];
  deviceOf(dev["deviceId"], id);
  Device d(id, static_cast<uint32_t>(gScript["cap"].u64()));
  for (size_t i = 0; i < dev["epochs"].size(); ++i) d.epochs.list.push_back(dev["epochs"][i].u64());
  CutFs fs;
  uint8_t clock = 0;
  uint32_t snapshots = 0, plays = 0;
  const std::map<std::string, uint8_t> clocks = {{"unset", 0}, {"rtc", 1}, {"host", 2}};
  const Value& events = gScript["events"];
  for (size_t i = 0; i < events.size(); ++i) {
    const Value& e = events[i];
    if (e.has("boot")) {
      clock = clocks.at(e["boot"]["clock"].str());
      TEST_ASSERT_TRUE(d.boot(fs, 2));
    } else if (e.has("setClock")) {
      clock = pl::kClockHost;
    } else if (e.has("eraseNvs")) {
      d.nvs.erase();
    } else if (e.has("acks")) {
      d.acks = msmf::Acks();
      for (size_t k = 0; k < e["acks"].size(); ++k) {
        msmf::Ack a;
        deviceOf(e["acks"][k]["deviceId"], a.deviceId);
        a.epoch = e["acks"][k]["epoch"].u64();
        a.seq = static_cast<uint32_t>(e["acks"][k]["seq"].u64());
        d.acks.entries[d.acks.count++] = a;
      }
      d.acks.rows = d.acks.count;
      d.acks.present = true;
    } else if (e.has("snapshot")) {
      ++snapshots;
      const std::string dir = "plays/golden/" + e["snapshot"].str() + "/";
      std::vector<std::string> names;
      for (const std::string& n : fs.names()) names.push_back(base(n.c_str()));
      std::sort(names.begin(), names.end());
      std::vector<std::string> want;
      for (const char* n : {"plays.1.jnl", "plays.jnl"})
        if (cardfixtures::exists(cardfixtures::dir() + "/" + dir + n)) want.push_back(n);
      TEST_ASSERT_TRUE(names == want);
      for (const std::string& n : want) {
        const Bytes g = fixture(dir + n);
        const Bytes have = fs.bytes("/.player/" + n);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(g.size(), have.size(), (dir + n).c_str());
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(g.data(), have.data(), g.size(), (dir + n).c_str());
      }
    } else {
      const Value& p = e["play"];
      Slot s;
      s.outcome = static_cast<uint8_t>(p["outcome"].u64());
      s.source = static_cast<uint8_t>(p["source"].u64());
      s.clock = clock;
      s.startedAt = clock ? p["startedAt"].u64() : 0;
      s.uptimeMs = static_cast<uint32_t>(p["uptimeMs"].u64());
      s.playedMs = static_cast<uint32_t>(p["playedMs"].u64());
      s.durationMs = static_cast<uint32_t>(p["durationMs"].u64());
      s.pauseCount = static_cast<uint16_t>(p["pauseCount"].u64());
      s.pathHash = p["pathHash"].u64();
      s.size = static_cast<uint32_t>(p["size"].u64());
      s.fatTime = static_cast<uint32_t>(p["fatTime"].u64());
      s.generation = static_cast<uint32_t>(p["generation"].u64());
      TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
      ++plays;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(2, snapshots);
  TEST_ASSERT_EQUAL_UINT32(16, plays);
  TEST_ASSERT_TRUE(fs.violations.empty());
  // The reference model's ops, one for one.
  const Value& ops = gVectors["plays"]["deviceOps"];
  TEST_ASSERT_EQUAL_size_t(ops.size(), d.ops.size());
  for (size_t i = 0; i < ops.size(); ++i) {
    std::string want;
    for (size_t k = 0; k < ops[i].size(); ++k) want += (k ? " " : "") + ops[i][k].str();
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), d.ops[i].c_str(), want.c_str());
  }
}

// ---------------------------------------------------------------------------
// The boot rules (2.20.4).
// ---------------------------------------------------------------------------
void test_the_boot_rules() {
  const Value& at = gVectors["plays"]["atBoot"];
  TEST_ASSERT_EQUAL_size_t(6, at["cases"].size());
  for (size_t c = 0; c < at["cases"].size(); ++c) {
    const Value& e = at["cases"][c];
    CutFs card;
    if (e.has("readCase")) {
      bool found = false;
      for (size_t k = 0; k < gIndex.size(); ++k) {
        if (gIndex[k]["case"].str() != e["readCase"].str()) continue;
        for (const auto& kv : gIndex[k]["files"].members)
          card.put("/.player/" + kv.first, fixture("plays/" + kv.second.str()));
        found = true;
      }
      TEST_ASSERT_TRUE(found);
    } else {
      for (const char* n : {"plays.jnl", "plays.1.jnl"})
        card.put(std::string("/.player/") + n, fixture(at["log"].str() + "/" + n));
    }
    uint8_t id[6];
    deviceOf(e["deviceId"], id);
    for (uint32_t piece : {1u, 3u, 1000u}) {  // the scan in any pieces
      CutFs fs = card;  // (the card as it was, for each piece size)
      Device d(id);
      d.nvs.ids.epoch = e["epoch"].u64();
      d.nvs.ids.seq = static_cast<uint32_t>(e["nvsSeq"].u64());
      d.nvs.ids.boots = static_cast<uint32_t>(e["nvsBoots"].u64());
      // The root's ACKS (the seq rule takes this device and epoch's).
      if (e.has("acks")) {
        for (size_t k = 0; k < e["acks"].size(); ++k) {
          msmf::Ack a;
          deviceOf(e["acks"][k]["deviceId"], a.deviceId);
          a.epoch = e["acks"][k]["epoch"].u64();
          a.seq = static_cast<uint32_t>(e["acks"][k]["seq"].u64());
          d.acks.entries[d.acks.count++] = a;
        }
        d.acks.rows = d.acks.count;
        d.acks.present = true;
      }
      TEST_ASSERT_TRUE(d.boot(fs, piece));
      TEST_ASSERT_FALSE(d.j->newEpoch());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["nextBootSeq"].u64(), d.j->bootSeq(), e["note"].c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["nextBootSeq"].u64(), d.nvs.ids.boots, e["note"].c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(e["nextSeq"].u64(), d.j->lastSeq() + 1, e["note"].c_str());
      // The next play takes it, NVS first.
      Slot s = play(0x1234, 60000, 1000);
      TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
      TEST_ASSERT_EQUAL_UINT32(e["nextSeq"].u64(), s.seq);
      TEST_ASSERT_EQUAL_UINT32(e["nextSeq"].u64(), d.nvs.ids.seq);
      TEST_ASSERT_EQUAL_UINT32(e["nextBootSeq"].u64(), s.bootSeq);
    }
  }
}

// NVS erased (the firmware's recovery, a flasher's install): a new epoch,
// drawn again when the RNG says 0; its seq and boot count from 1; the
// card's slots of the old epoch don't count. A new epoch with no ACKS
// reads nothing at boot.
void test_a_new_epoch() {
  CutFs fs;
  Device d;
  d.epochs.list = {0, 0x1122334455667788ull, 0xF0E1D2C3B4A59687ull};
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_TRUE(d.j->newEpoch());
  TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ull, d.j->epoch());
  TEST_ASSERT_EQUAL_UINT64(0x1122334455667788ull, d.nvs.ids.epoch);
  TEST_ASSERT_EQUAL_UINT32(1, d.j->bootSeq());
  for (uint32_t i = 0; i < 3; ++i) {
    Slot s = play(0x10 + i, 60000, 1000 * i);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(i + 1, s.seq);
  }
  d.nvs.erase();
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_TRUE(d.j->newEpoch());
  TEST_ASSERT_EQUAL_UINT64(0xF0E1D2C3B4A59687ull, d.j->epoch());
  TEST_ASSERT_EQUAL_UINT32(1, d.j->bootSeq());
  TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().slots);  // nothing read: no ACKS, a new epoch
  Slot s = play(0x20, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(1, s.seq);
  TEST_ASSERT_EQUAL_UINT32(4 * pl::kSlot, fs.bytes(kLog).size());
  // The same epoch at the next boot: its boot count 2, the seq on.
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_FALSE(d.j->newEpoch());
  TEST_ASSERT_EQUAL_UINT32(2, d.j->bootSeq());
  TEST_ASSERT_EQUAL_UINT32(1, d.j->lastSeq());
  TEST_ASSERT_EQUAL_UINT32(4, d.j->booted().slots);
}

// The seq goes to NVS before the slot: one NVS refuses is no slot (and
// the next play takes the next seq: a gap, never a seq twice). NVS that
// can't be read: no log. The seq's last value takes a new epoch, its boot
// count from 1 (2.20.4).
void test_nvs_first_and_the_last_seq() {
  CutFs fs;
  Device d;
  TEST_ASSERT_TRUE(d.boot(fs));
  Slot s = play(1, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  d.nvs.failSeq = true;
  s = play(2, 60000, 2000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Nvs);
  TEST_ASSERT_EQUAL_UINT32(pl::kSlot, fs.bytes(kLog).size());
  d.nvs.failSeq = false;
  s = play(3, 60000, 3000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(2, s.seq);
  // The last seq.
  d.nvs.ids.seq = 0xFFFFFFFEu;
  const uint64_t old = d.nvs.ids.epoch;
  TEST_ASSERT_TRUE(d.boot(fs));
  s = play(4, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu, s.seq);
  s = play(5, 60000, 2000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_TRUE(s.epoch != old);
  TEST_ASSERT_EQUAL_UINT64(d.nvs.ids.epoch, s.epoch);
  TEST_ASSERT_EQUAL_UINT32(1, s.seq);
  TEST_ASSERT_EQUAL_UINT32(1, s.bootSeq);
  // NVS that can't be read; one that won't take the boot count.
  Device off;
  off.nvs.failLoad = true;
  TEST_ASSERT_FALSE(off.boot(fs));
  TEST_ASSERT_TRUE(off.j->off());
  s = play(6, 60000, 1000);
  TEST_ASSERT_TRUE(off.j->append(s) == Journal::Wrote::Off);
  Device noBoots;
  noBoots.nvs.ids.epoch = 7;
  noBoots.nvs.failBoots = true;
  TEST_ASSERT_FALSE(noBoots.boot(fs));
  TEST_ASSERT_TRUE(noBoots.j->append(s) == Journal::Wrote::Off);
}

// A cut append's part slot is written over by the next append, which goes
// at the slot boundary (2.20.2: every append lands on one).
void test_a_tail_is_cut_off() {
  CutFs fs;
  Device d(kDevA, 4);
  TEST_ASSERT_TRUE(d.boot(fs));
  for (uint32_t i = 0; i < 2; ++i) {
    Slot s = play(i, 60000, 1000 * i);
    d.j->append(s);
  }
  Bytes b = fs.bytes(kLog);
  b.insert(b.end(), 37, 0x5A);
  fs.put(kLog, b);
  Slot s = play(9, 60000, 9000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  b = fs.bytes(kLog);
  TEST_ASSERT_EQUAL_size_t(3 * pl::kSlot, b.size());
  Slot back;
  TEST_ASSERT_TRUE(pl::decode(b.data() + 2 * pl::kSlot, pl::kSlot, &back) == pl::Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT32(3, back.seq);
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// ---------------------------------------------------------------------------
// ACKS (2.5.6).
// ---------------------------------------------------------------------------
void test_acks_read_on_their_own() {
  const Value& v = gVectors["plays"]["acks"];
  std::vector<uint8_t> scratch(32);
  auto read = [&](const Bytes& file, msmf::Acks* out) {
    VecSource src(file);
    msmf::Manifest m;
    std::vector<uint8_t> s(512);
    TEST_ASSERT_TRUE(m.open(src, s.data(), static_cast<uint32_t>(s.size())) == cc::Why::Ok);  // the root reads
    return msmf::readAcks(src, scratch.data(), static_cast<uint32_t>(scratch.size()), out);
  };
  auto same = [&](const Value& want, const msmf::Acks& a) {
    TEST_ASSERT_EQUAL_UINT32(want.size(), a.count);
    TEST_ASSERT_EQUAL_UINT32(want.size(), a.rows);
    for (size_t i = 0; i < want.size(); ++i) {
      uint8_t dev[6];
      deviceOf(want[i]["deviceId"], dev);
      TEST_ASSERT_EQUAL_MEMORY(dev, a.entries[i].deviceId, 6);
      TEST_ASSERT_EQUAL_UINT64(want[i]["epoch"].u64(), a.entries[i].epoch);
      TEST_ASSERT_EQUAL_UINT32(want[i]["seq"].u64(), a.entries[i].seq);
      TEST_ASSERT_EQUAL_UINT32(want[i]["seq"].u64(), a.seqOf(dev, want[i]["epoch"].u64()));
    }
  };
  msmf::Acks a;
  TEST_ASSERT_TRUE(read(fixture("golden/" + v["golden"]["file"].str()), &a) == cc::Why::Ok);
  TEST_ASSERT_TRUE(a.present);
  same(v["golden"]["entries"], a);
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(kDevB, 0x1122334455667788ull));
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(kDevA, 0x0A0B0C0D0E0F1011ull));
  // The stored entry's bytes.
  const Bytes stored = hexBytes(v["entry"]["stored"].str());
  TEST_ASSERT_EQUAL_size_t(msmf::kAcksStride, stored.size());
  // The hardening files: each failure makes ACKS absent, the root present.
  const Value& h = v["hardening"];
  TEST_ASSERT_EQUAL_size_t(4, h.size());
  for (size_t i = 0; i < h.size(); ++i) {
    const cc::Why w = read(fixture("hardening/" + h[i]["file"].str()), &a);
    if (h[i]["acks"].isNull()) {
      TEST_ASSERT_TRUE_MESSAGE(w != cc::Why::Ok && w != cc::Why::Missing, h[i]["file"].c_str());
      TEST_ASSERT_FALSE(a.present);
      TEST_ASSERT_EQUAL_UINT32(0, a.count);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(w == cc::Why::Ok, h[i]["file"].c_str());
      same(h[i]["acks"], a);
    }
  }
  TEST_ASSERT_TRUE(read(fixture("hardening/msmf-acks-damaged.bin"), &a) == cc::Why::SectionCrc);
  TEST_ASSERT_TRUE(read(fixture("hardening/msmf-acks-order.bin"), &a) == cc::Why::AcksOrder);
  TEST_ASSERT_TRUE(read(fixture("hardening/msmf-acks-short.bin"), &a) == cc::Why::Shape);
  // A root with none.
  TEST_ASSERT_TRUE(read(fixture("golden/manifest.bin"), &a) == cc::Why::Missing);
  TEST_ASSERT_FALSE(a.present);
}

// The rows kept (msmf::readAcks()'s `own`, the root the boot reads with the
// board's id): a section of more than 16, other boards' sorting before this
// one's, keeps this board's, which its log trims by; without `own`, the
// first 16, none of them this board's (its log never trimmed). Of this
// board's own rows past 16, the first 16.
void test_acks_keep_this_boards_rows_first() {
  const Bytes golden = fixture("golden/" + gVectors["plays"]["acks"]["golden"]["file"].str());
  // The golden root with its ACKS section's rows replaced by `rows` (placed
  // at the file's end; readAcks() checks only the section).
  auto withRows = [&](const std::vector<msmf::Ack>& rows) {
    Bytes b = golden;
    const uint32_t headerBytes = cc::get32(b.data() + 8), sections = cc::get32(b.data() + 12);
    size_t entry = 0;
    for (uint32_t i = 0; i < sections; ++i)
      if (cc::get32(b.data() + headerBytes + i * cc::kDirEntryBytes) == msmf::kAcks)
        entry = headerBytes + i * cc::kDirEntryBytes;
    TEST_ASSERT_TRUE(entry != 0);
    const uint32_t at = static_cast<uint32_t>(b.size());
    for (const msmf::Ack& r : rows) {
      uint8_t row[msmf::kAcksStride] = {};
      std::memcpy(row, r.deviceId, 6);
      cc::put64(row + 8, r.epoch);
      cc::put32(row + 16, r.seq);
      b.insert(b.end(), row, row + sizeof(row));
    }
    const uint32_t bytes = static_cast<uint32_t>(rows.size()) * msmf::kAcksStride;
    cc::put32(b.data() + entry + 8, at);
    cc::put32(b.data() + entry + 12, bytes);
    cc::put32(b.data() + entry + 16, static_cast<uint32_t>(rows.size()));
    cc::put32(b.data() + entry + 20, msmf::kAcksStride);
    cc::put32(b.data() + entry + 24, cc::crc32(b.data() + at, bytes));
    return b;
  };
  auto board = [](uint8_t last) {
    msmf::Ack a;
    const uint8_t id[6] = {0x00, 0x00, 0x5E, 0x00, 0x53, last};  // RFC 7042's documentation range
    std::memcpy(a.deviceId, id, 6);
    return a;
  };
  auto row = [&](uint8_t last, uint64_t epoch, uint32_t seq) {
    msmf::Ack a = board(last);
    a.epoch = epoch;
    a.seq = seq;
    return a;
  };
  const msmf::Ack mine = board(0xF0);
  std::vector<uint8_t> scratch(32);
  auto read = [&](const Bytes& file, msmf::Acks* out, const uint8_t* own) {
    VecSource src(file);
    return msmf::readAcks(src, scratch.data(), static_cast<uint32_t>(scratch.size()), out, own);
  };
  // 18 other boards before this one, its two epochs, one board after.
  std::vector<msmf::Ack> rows;
  for (uint8_t k = 1; k <= 18; ++k) rows.push_back(row(k, 0x10 + k, 5));
  rows.push_back(row(0xF0, 0x1111, 7));
  rows.push_back(row(0xF0, 0x2222, 9));
  rows.push_back(row(0xF1, 0x3333, 4));
  const Bytes file = withRows(rows);
  msmf::Acks a;
  TEST_ASSERT_TRUE(read(file, &a, nullptr) == cc::Why::Ok);
  TEST_ASSERT_EQUAL_UINT32(21, a.rows);
  TEST_ASSERT_EQUAL_UINT32(msmf::Acks::kMax, a.count);
  TEST_ASSERT_EQUAL_UINT32(0, a.own);
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(mine.deviceId, 0x2222));
  TEST_ASSERT_TRUE(read(file, &a, mine.deviceId) == cc::Why::Ok);
  TEST_ASSERT_EQUAL_UINT32(21, a.rows);
  TEST_ASSERT_EQUAL_UINT32(msmf::Acks::kMax, a.count);
  TEST_ASSERT_EQUAL_UINT32(2, a.own);
  TEST_ASSERT_EQUAL_UINT32(7, a.seqOf(mine.deviceId, 0x1111));
  TEST_ASSERT_EQUAL_UINT32(9, a.seqOf(mine.deviceId, 0x2222));
  // The others kept: the first 14 (the 15th and 16th gave their places).
  TEST_ASSERT_EQUAL_UINT32(5, a.seqOf(board(1).deviceId, 0x11));
  TEST_ASSERT_EQUAL_UINT32(5, a.seqOf(board(14).deviceId, 0x10 + 14));
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(board(15).deviceId, 0x10 + 15));
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(board(0xF1).deviceId, 0x3333));
  // At most 16 or fewer rows: as the section has them, `own` or not.
  TEST_ASSERT_TRUE(read(withRows({row(1, 1, 1), row(0xF0, 2, 2), row(0xF1, 3, 3)}), &a, mine.deviceId) == cc::Why::Ok);
  TEST_ASSERT_EQUAL_UINT32(3, a.count);
  TEST_ASSERT_EQUAL_UINT32(1, a.own);
  TEST_ASSERT_EQUAL_MEMORY(board(1).deviceId, a.entries[0].deviceId, 6);
  TEST_ASSERT_EQUAL_MEMORY(mine.deviceId, a.entries[1].deviceId, 6);
  TEST_ASSERT_EQUAL_MEMORY(board(0xF1).deviceId, a.entries[2].deviceId, 6);
  // This board's own rows past 16: its first 16.
  std::vector<msmf::Ack> many;
  for (uint32_t k = 1; k <= 20; ++k) many.push_back(row(0xF0, k, k));
  TEST_ASSERT_TRUE(read(withRows(many), &a, mine.deviceId) == cc::Why::Ok);
  TEST_ASSERT_EQUAL_UINT32(20, a.own);
  TEST_ASSERT_EQUAL_UINT32(16, a.seqOf(mine.deviceId, 16));
  TEST_ASSERT_EQUAL_UINT32(0, a.seqOf(mine.deviceId, 17));
  // The log: this board's plays of epoch 0x2222, seqs 1-3, all acknowledged
  // by the section's row: trimmed with `own`'s rows, kept without.
  Bytes log;
  for (uint32_t seq = 1; seq <= 3; ++seq) {
    Slot s = play(seq, 60000, 1000 * seq);
    s.seq = seq;
    s.epoch = 0x2222;
    std::memcpy(s.deviceId, mine.deviceId, 6);
    s.bootSeq = 1;
    uint8_t b[pl::kSlot];
    pl::encode(s, b);
    log.insert(log.end(), b, b + pl::kSlot);
  }
  for (const bool own : {false, true}) {
    CutFs fs;
    fs.put(kLog, log);
    Device d(mine.deviceId);
    d.nvs.ids.epoch = 0x2222;
    d.nvs.ids.seq = 3;
    TEST_ASSERT_TRUE(read(file, &d.acks, own ? mine.deviceId : nullptr) == cc::Why::Ok);
    TEST_ASSERT_TRUE(d.boot(fs));
    TEST_ASSERT_EQUAL_UINT32(own ? 1 : 0, d.j->booted().trimmed);
    TEST_ASSERT_EQUAL(own ? 0 : 1, static_cast<int>(fs.dir.count(kLog)));
  }
}

// ---------------------------------------------------------------------------
// The trim (2.20.2).
// ---------------------------------------------------------------------------
void test_the_trims_rules() {
  const uint64_t e = 0x1122334455667788ull;
  auto slotOf = [&](const uint8_t dev[6], uint32_t seq, uint8_t kind = pl::kKindPlay, uint32_t dropped = 0) {
    Slot s = play(seq, 60000, 1000 * seq);
    s.kind = kind;
    s.seq = kind == pl::kKindPlay ? seq : 0;
    s.epoch = e;
    std::memcpy(s.deviceId, dev, 6);
    s.bootSeq = 1;
    if (kind != pl::kKindPlay) s.playedMs = dropped;
    Bytes b(pl::kSlot);
    pl::encode(s, b.data());
    return b;
  };
  auto cat = [](std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
    return b;
  };
  struct Case {
    const char* what;
    Bytes file;
    bool gone;
  };
  Bytes newer = slotOf(kDevA, 2);
  newer[4] = 2;  // (a newer major's: never judged)
  Bytes kind9 = slotOf(kDevA, 2, 9);
  Bytes torn = slotOf(kDevA, 2);
  torn[30] ^= 1;
  const Case cases[] = {
      {"every play acknowledged", cat({slotOf(kDevA, 1), slotOf(kDevA, 2)}), true},
      {"a damaged slot among them", cat({slotOf(kDevA, 1), torn, slotOf(kDevA, 3)}), true},
      {"a play past the mark", cat({slotOf(kDevA, 1), slotOf(kDevA, 4)}), false},
      {"another device's play ACKS doesn't name", cat({slotOf(kDevA, 1), slotOf(kDevB, 1)}), false},
      {"a drop note no play follows", cat({slotOf(kDevA, 1), slotOf(kDevA, 0, pl::kKindDrop, 3)}), false},
      {"a drop note read (a play after it)", cat({slotOf(kDevA, 0, pl::kKindDrop, 3), slotOf(kDevA, 1)}), true},
      {"a newer major's slot", cat({slotOf(kDevA, 1), newer}), false},
      {"a slot of a kind this firmware doesn't know", cat({slotOf(kDevA, 1), kind9}), false},
      {"no play at all", cat({slotOf(kDevA, 0, pl::kKindDrop, 3)}), false},
      {"longer than a reader reads", [&] {
         Bytes b = slotOf(kDevA, 1);
         b.resize(pl::kFileBytes + pl::kSlot, 0);
         return b;
       }(),
       false},
  };
  for (const Case& c : cases) {
    for (int file = 0; file < 2; ++file) {
      CutFs fs;
      fs.put(file ? kPrev : kLog, c.file);
      Device d;
      d.nvs.ids.epoch = 0x77;  // (another epoch of this device: none of these slots are its)
      setAcks(d.acks, {ack(kDevA, e, 3)});
      TEST_ASSERT_TRUE(d.boot(fs, 7));
      TEST_ASSERT_EQUAL_MESSAGE(!c.gone, fs.dir.count(file ? kPrev : kLog) == 1, c.what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.gone ? 1 : 0, d.j->booted().trimmed, c.what);
    }
  }
  // No ACKS: nothing goes, whatever the slots (a seq 0 reads as
  // acknowledged by any ACKS).
  CutFs fs;
  fs.put(kLog, cat({slotOf(kDevA, 1)}));
  Device d;
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
  TEST_ASSERT_TRUE(fs.dir.count(kLog) == 1);
}

// ---------------------------------------------------------------------------
// The rotation and power cuts (2.20.2, 2.20.5).
// ---------------------------------------------------------------------------
namespace {

// Every valid slot on the card (the log's names and the twins): each must
// be one the journal wrote, byte for byte; plays of one key never differ.
void assertNothingCreditedWrongly(const CutFs& fs, const std::map<std::string, Bytes>& written, const char* what) {
  std::map<std::string, Bytes> seen;
  for (const std::string& name : fs.names()) {
    const Bytes b = fs.bytes(name);
    for (size_t off = 0; off + pl::kSlot <= b.size(); off += pl::kSlot) {
      Slot s;
      if (pl::decode(b.data() + off, pl::kSlot, &s) != pl::Verdict::Ok || s.kind != pl::kKindPlay) continue;
      char k[48];
      std::snprintf(k, sizeof(k), "%016llx-%u", static_cast<unsigned long long>(s.epoch), static_cast<unsigned>(s.seq));
      const Bytes slot(b.begin() + static_cast<long>(off), b.begin() + static_cast<long>(off) + pl::kSlot);
      auto w = written.find(k);
      TEST_ASSERT_TRUE_MESSAGE(w != written.end(), what);
      TEST_ASSERT_TRUE_MESSAGE(w->second == slot, what);
      auto it = seen.find(k);
      if (it != seen.end()) TEST_ASSERT_TRUE_MESSAGE(it->second == slot, what);
      seen[k] = slot;
    }
  }
}

std::string keyOf(const Slot& s) {
  char k[48];
  std::snprintf(k, sizeof(k), "%016llx-%u", static_cast<unsigned long long>(s.epoch), static_cast<unsigned>(s.seq));
  return k;
}

}  // namespace

void test_a_cut_at_every_step() {
  const cutfs::Variant variants[] = {cutfs::Variant::InOrder, cutfs::Variant::LoseUnsynced, cutfs::Variant::Torn};
  uint32_t runs = 0, twinsMade = 0;
  for (cutfs::Variant v : variants) {
    for (long cut = 1; cut < 80; ++cut) {
      CutFs fs;
      Device d(kDevA, 3);
      std::map<std::string, Bytes> written;
      uint32_t up = 0;
      auto playOne = [&](Device& dev, tagstore::Fs& card) {
        Slot s = play(0x1000 + up, 60000, up += 1000);
        const Journal::Wrote w = dev.j->append(s);
        if (s.seq) {  // (the seq taken: whatever reached the card is this slot)
          Bytes b(pl::kSlot);
          pl::encode(s, b.data());
          written[keyOf(s)] = b;
        }
        (void)card;
        return w;
      };
      TEST_ASSERT_TRUE(d.boot(fs));
      for (int i = 0; i < 4; ++i) TEST_ASSERT_TRUE(playOne(d, fs) == Journal::Wrote::Ok);
      const uint32_t before = d.j->lastSeq();
      fs.cutAt = fs.steps + cut;
      for (int i = 0; i < 8 && !fs.dead; ++i) playOne(d, fs);
      if (!fs.dead) continue;  // (past the last step)
      ++runs;
      char what[64];
      std::snprintf(what, sizeof(what), "variant %d, cut at step %ld", static_cast<int>(v), cut);
      TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), what);
      CutFs next = fs.reboot(v);
      TEST_ASSERT_TRUE_MESSAGE(d.boot(next), what);
      TEST_ASSERT_TRUE_MESSAGE(next.violations.empty(), what);
      assertNothingCreditedWrongly(next, written, what);
      // The log goes on: no seq used twice, every new slot on a slot boundary.
      const uint32_t last = d.j->lastSeq();
      TEST_ASSERT_TRUE_MESSAGE(last >= before, what);
      if (next.dir.count("/.player/plays.xl1")) ++twinsMade;
      for (int i = 0; i < 7; ++i) {
        const Journal::Wrote w = playOne(d, next);
        TEST_ASSERT_TRUE_MESSAGE(w == Journal::Wrote::Ok, what);
      }
      TEST_ASSERT_TRUE_MESSAGE(next.violations.empty(), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, next.bytes(kLog).size() % pl::kSlot, what);
      assertNothingCreditedWrongly(next, written, what);
      // A disk check copies the chains apart: the next boot collects the twins.
      next.diskCheck();
      TEST_ASSERT_TRUE_MESSAGE(d.boot(next), what);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, d.j->booted().twins, what);
      for (const std::string& n : next.names()) TEST_ASSERT_TRUE_MESSAGE(n.find(".xl") == std::string::npos, what);
      assertNothingCreditedWrongly(next, written, what);
    }
  }
  TEST_ASSERT_TRUE(runs >= 60);
  TEST_ASSERT_TRUE(twinsMade > 0);  // (a cut inside a rotation's rename left twins)
  printf("[play log] power cuts: %u runs (3 ways), %u left a cut rename's twins\n", runs, twinsMade);
}

// The cut-rename rule's twins (2.20.2): plays.jnl and plays.1.jnl on one
// chain at a mount; the rotations while a twin shares plays.1.jnl rename it
// to the next twin, never delete it; all nine twin names taken: blocked,
// no slot written (lost, counted) until a disk check frees one.
void test_twins_and_the_blocked_log() {
  CutFs fs;
  Device d(kDevA, 2);
  TEST_ASSERT_TRUE(d.boot(fs));
  for (int i = 0; i < 3; ++i) {
    Slot s = play(i, 60000, 1000 * (i + 1));
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  // A cut inside the next rotation's rename: both names on plays.jnl's chain.
  fs.remove(kPrev);
  fs.link(kLog, kPrev);
  d.ops.clear();
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_EQUAL_STRING("twin plays.jnl plays.xl1", d.ops.at(0).c_str());
  TEST_ASSERT_TRUE(fs.shared(kPrev, "/.player/plays.xl1"));
  TEST_ASSERT_FALSE(fs.dir.count(kLog) == 1);
  // Plays go on into a new plays.jnl; its rotation renames plays.1.jnl to
  // xl2 (xl1 shares its chain), and xl1 stays.
  for (int i = 0; i < 3; ++i) {
    Slot s = play(10 + i, 60000, 9000 + 1000 * i);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  TEST_ASSERT_TRUE(fs.dir.count("/.player/plays.xl1") == 1);
  TEST_ASSERT_TRUE(fs.dir.count("/.player/plays.xl2") == 1);
  TEST_ASSERT_TRUE(fs.violations.empty());
  // All nine twin names taken, and plays.jnl on plays.1.jnl's chain again.
  for (int i = 3; i <= 9; ++i) fs.put("/.player/plays.xl" + std::to_string(i), Bytes(80, 0));
  fs.remove(kPrev);
  fs.link(kLog, kPrev);
  for (int i = 3; i <= 9; ++i) fs.link(kLog, "/.player/plays.xl" + std::to_string(i));
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_TRUE(d.j->blocked());
  Slot s = play(99, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Blocked);
  TEST_ASSERT_EQUAL_STRING("lost 7", d.ops.back().c_str());
  TEST_ASSERT_TRUE(fs.violations.empty());
  // The disk check: every twin collected, the log going on.
  fs.diskCheck();
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_FALSE(d.j->blocked());
  TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().twins);
  s = play(100, 60000, 2000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(8, s.seq);  // (7 was taken: NVS has it)
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// The cut-rename rule's rename refused by the card (not all nine twin
// names taken): the log isn't blocked. No slot goes to plays.jnl while it
// shares plays.1.jnl's chain (the play is lost, plays.1.jnl untouched);
// each append tries the rename again, and once the card takes it the plays
// go on in a new plays.jnl. And a remount settles again: a log blocked
// before a disk check goes on after it with the same card back.
void test_a_refused_rename_is_tried_again() {
  CutFs fs;
  Device d(kDevA, 2);
  TEST_ASSERT_TRUE(d.boot(fs));
  for (int i = 0; i < 3; ++i) {
    Slot s = play(i, 60000, 1000 * (i + 1));
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  fs.remove(kPrev);
  fs.link(kLog, kPrev);
  const Bytes prev = fs.bytes(kPrev);
  fs.refuse = true;  // (a card that takes no write for now: the rename refused)
  d.ops.clear();
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_FALSE(d.ops.empty());
  TEST_ASSERT_EQUAL_STRING("refused plays.jnl plays.xl1", d.ops.at(0).c_str());
  TEST_ASSERT_FALSE(d.j->blocked());
  TEST_ASSERT_TRUE(d.j->unsettled());
  Slot s = play(50, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
  TEST_ASSERT_EQUAL_STRING("refused plays.jnl plays.xl1", d.ops.at(d.ops.size() - 2).c_str());
  TEST_ASSERT_EQUAL_STRING("lost 4", d.ops.back().c_str());
  TEST_ASSERT_TRUE(fs.bytes(kPrev) == prev);
  fs.refuse = false;
  s = play(51, 60000, 2000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(5, s.seq);
  TEST_ASSERT_FALSE(d.j->unsettled());
  TEST_ASSERT_TRUE(fs.shared(kPrev, "/.player/plays.xl1"));
  TEST_ASSERT_EQUAL_size_t(pl::kSlot, fs.bytes(kLog).size());
  TEST_ASSERT_TRUE(fs.bytes(kPrev) == prev);
  TEST_ASSERT_TRUE(fs.violations.empty());

  // Blocked: plays.jnl on plays.1.jnl's chain, all nine twin names taken.
  fs.dir.erase(kPrev);  // (by hand: nothing freed)
  fs.link(kLog, kPrev);
  for (int i = 1; i <= 9; ++i) fs.link(kLog, "/.player/plays.xl" + std::to_string(i));
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_TRUE(d.j->blocked());
  s = play(52, 60000, 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Blocked);
  // The card to a computer for a disk check, and back: remounted().
  fs.diskCheck();
  d.j->remounted();
  TEST_ASSERT_FALSE(d.j->blocked());
  TEST_ASSERT_TRUE(d.j->ready());  // (a slot was asked for: the boot's scan stands)
  TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().restarts);
  for (const std::string& n : fs.names()) TEST_ASSERT_TRUE(n.find(".xl") == std::string::npos);
  s = play(53, 60000, 2000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(7, s.seq);
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// A rotation counts what plays.1.jnl holds that no software read: its
// plays ACKS doesn't acknowledge and the drop notes no read play follows,
// saturating; the note takes the play's dates.
void test_the_drop_note() {
  CutFs fs;
  Device d(kDevA, 2);
  d.epochs.list = {0xAAull};
  TEST_ASSERT_TRUE(d.boot(fs));
  for (int i = 0; i < 4; ++i) {
    Slot s = play(i, 60000, 1000 * (i + 1));
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  TEST_ASSERT_EQUAL_UINT32(0, d.j->lastDropped());  // (the first rotation had no plays.1.jnl)
  // ACKS for seq 1: plays.1.jnl holds 1 and 2 when 5 comes: 2 unread.
  setAcks(d.acks, {ack(kDevA, 0xAA, 1)});
  TEST_ASSERT_TRUE(d.boot(fs));
  Slot s = play(9, 60000, 77000);
  s.clock = pl::kClockHost;
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(1, d.j->lastDropped());  // (plays.1.jnl held 1 and 2: 2 unread)
  const Bytes b = fs.bytes(kLog);
  TEST_ASSERT_EQUAL_size_t(2 * pl::kSlot, b.size());
  Slot note;
  TEST_ASSERT_TRUE(pl::decode(b.data(), pl::kSlot, &note) == pl::Verdict::Ok);
  TEST_ASSERT_EQUAL_UINT8(pl::kKindDrop, note.kind);
  TEST_ASSERT_EQUAL_UINT64(s.startedAt, note.startedAt);
  TEST_ASSERT_EQUAL_UINT32(77000, note.uptimeMs);
  TEST_ASSERT_EQUAL_UINT8(pl::kClockHost, note.clock);
  TEST_ASSERT_EQUAL_UINT32(s.bootSeq, note.bootSeq);
  TEST_ASSERT_EQUAL_UINT32(d.j->lastDropped(), note.playedMs);
}

// What a rotation counts in plays.1.jnl (2.20.2, step 1): a drop note that
// an acknowledged play follows was read (its count goes no further), one
// no such play follows wasn't (its count goes on); unacknowledged plays
// each count; the sum saturates at 2^32 - 1.
void test_what_a_rotation_counts() {
  const uint64_t e = 0xABCDull;
  auto slot = [&](uint32_t seq, uint32_t dropped = 0) {
    Slot s = play(seq, 60000, 1000 * seq);
    s.kind = dropped ? pl::kKindDrop : pl::kKindPlay;
    s.seq = dropped ? 0 : seq;
    s.epoch = e;
    std::memcpy(s.deviceId, kDevA, 6);
    s.bootSeq = 1;
    if (dropped) s.playedMs = dropped;
    Bytes b(pl::kSlot);
    pl::encode(s, b.data());
    return b;
  };
  auto cat = [](std::initializer_list<Bytes> parts) {
    Bytes b;
    for (const Bytes& p : parts) b.insert(b.end(), p.begin(), p.end());
    return b;
  };
  struct Case {
    const char* what;
    Bytes prev;
    bool acks;
    uint32_t dropped;
  };
  Bytes newer = slot(7);
  newer[4] = 2;
  const Case cases[] = {
      {"a note read (an acknowledged play after it), a play unread", cat({slot(0, 3), slot(1), slot(2)}), true, 1},
      {"a note after the acknowledged play: unread, its count goes on", cat({slot(1), slot(0, 5)}), true, 5},
      {"no ACKS: the note and both plays", cat({slot(0, 3), slot(1), slot(2)}), false, 5},
      {"everything read: no note (a newer major's slot keeps the file)", cat({slot(0, 3), slot(1), newer}), true, 0},
      {"saturating", cat({slot(0, 0xFFFFFFFFu), slot(2)}), true, 0xFFFFFFFFu},
  };
  for (const Case& c : cases) {
    CutFs fs;
    fs.put(kPrev, c.prev);
    fs.put(kLog, cat({slot(3), slot(4)}));
    Device d(kDevA, 2);
    d.nvs.ids.epoch = e;
    d.nvs.ids.seq = 4;
    if (c.acks) setAcks(d.acks, {ack(kDevA, e, 1)});
    TEST_ASSERT_TRUE_MESSAGE(d.boot(fs), c.what);
    TEST_ASSERT_TRUE_MESSAGE(fs.dir.count(kPrev) == 1, c.what);  // (not trimmed: something in it is unread)
    Slot s = play(5, 60000, 5000);
    TEST_ASSERT_TRUE_MESSAGE(d.j->append(s) == Journal::Wrote::Ok, c.what);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.dropped, d.j->lastDropped(), c.what);
    TEST_ASSERT_EQUAL_size_t_MESSAGE((c.dropped ? 2 : 1) * pl::kSlot, fs.bytes(kLog).size(), c.what);
  }
}

// ---------------------------------------------------------------------------
// The Tracker.
// ---------------------------------------------------------------------------
namespace {
struct Env : pl::Tracker::Env {
  uint64_t now = 1000;
  bool card = true;
  uint32_t calls = 0;
  uint64_t monoMs() override { return now; }
  bool file(uint32_t key, uint32_t track, pl::PlayFile* out) override {
    ++calls;
    out->pathHash = 0x1000 + track;
    out->generation = 42;
    out->source = key == 7 ? pl::kSourceAutoDj : pl::kSourceManual;
    out->hintMs = 200000;
    std::snprintf(out->path, sizeof(out->path), "/music/t%u.mp3", static_cast<unsigned>(track));
    return card;
  }
};
using End = PlaybackController::Plays::End;
}  // namespace

void test_the_tracker() {
  Env env;
  pl::Tracker t(env);
  // A play from 0:00: 10 s played in passes of 20 ms, a pause of a
  // minute, 5 s more; a seek forward of 60 s isn't played time; then it
  // ends at its end (the tail since the last look counts).
  t.began(3, 5, 0);
  uint32_t pos = 0;
  auto run = [&](uint32_t ms) {
    for (uint32_t k = 0; k < ms / 20; ++k) {
      env.now += 20;
      pos += 20;
      t.tick(PlayState::Playing, pos, true, 180000);
    }
  };
  run(10000);
  env.now += 20;
  t.tick(PlayState::Paused, pos, true, 180000);
  for (int k = 0; k < 100; ++k) {
    env.now += 600;
    t.tick(PlayState::Paused, pos, true, 180000);
  }
  run(5000);
  pos += 60000;  // a seek: the start taken up at once (a fake backend)
  env.now += 20;
  t.tick(PlayState::Playing, pos, true, 180000);
  // An async start: unknown, then the backend's position on the old track
  // never counts.
  t.tick(PlayState::Playing, 999999, false, 180000);
  env.now += 20;
  t.tick(PlayState::Playing, pos, true, 180000);
  run(1000);
  pos = 180000 - 300;
  env.now += 20;
  t.tick(PlayState::Playing, pos, true, 180000);  // (a jump: not counted)
  TEST_ASSERT_EQUAL_UINT32(0, t.pending());
  t.ended(End::Completed);
  TEST_ASSERT_EQUAL_UINT32(1, t.pending());
  const pl::Played& p = t.oldest();
  TEST_ASSERT_EQUAL_UINT32(10000 + 5000 + 1000 + 300, p.playedMs);
  TEST_ASSERT_EQUAL_UINT16(1, p.pauses);
  TEST_ASSERT_EQUAL_UINT8(pl::kCompleted, p.outcome);
  TEST_ASSERT_EQUAL_UINT32(180000, p.durationMs);
  TEST_ASSERT_EQUAL_UINT64(1000, p.startMono);
  TEST_ASSERT_EQUAL_UINT64(0x1005, p.file.pathHash);
  TEST_ASSERT_EQUAL_STRING("/music/t5.mp3", p.file.path);
  t.pop();
  TEST_ASSERT_EQUAL_UINT32(0, t.pending());
  // Under a second: left out. Not a card file: left out. A Skipped one
  // from a resume point; the length the backend never said: the hint.
  t.began(4, 6, 0);
  pos = 0;
  run(900);
  t.ended(End::Skipped);
  t.began(7, 8, 150000);
  pos = 150000;
  for (int k = 0; k < 100; ++k) {
    env.now += 20;
    pos += 20;
    t.tick(PlayState::Playing, pos, true, 0);
  }
  t.ended(End::Skipped);
  env.card = false;
  t.began(9, 9, 0);
  pos = 0;
  run(5000);
  t.ended(End::Stopped);
  env.card = true;
  TEST_ASSERT_EQUAL_UINT32(1, t.pending());
  TEST_ASSERT_EQUAL_UINT8(pl::kSkipped, t.oldest().outcome);
  TEST_ASSERT_EQUAL_UINT8(pl::kSourceAutoDj, t.oldest().file.source);
  TEST_ASSERT_EQUAL_UINT32(2000, t.oldest().playedMs);
  TEST_ASSERT_EQUAL_UINT32(200000, t.oldest().durationMs);
  TEST_ASSERT_EQUAL_UINT32(1, t.counts().short_);
  TEST_ASSERT_EQUAL_UINT32(1, t.counts().notLogged);
  // A begin while one is open ends it first (Stopped); an end with none
  // open is nothing.
  t.ended(End::Stopped);
  t.began(1, 1, 0);
  pos = 0;
  run(2000);
  t.began(2, 2, 0);
  TEST_ASSERT_EQUAL_UINT32(2, t.pending());
  // The queue's room: the newest past it is lost, counted.
  pos = 0;
  run(2000);
  t.ended(End::Completed);
  for (uint32_t i = 0; i < pl::Tracker::kPending; ++i) {
    t.began(1, 1, 0);
    pos = 0;
    run(2000);
    t.ended(End::Completed);
  }
  TEST_ASSERT_EQUAL_UINT32(pl::Tracker::kPending, t.pending());
  TEST_ASSERT_EQUAL_UINT32(3, t.counts().overflow);
  // The ring's size, and what it costs (the Tracker is in PSRAM).
  TEST_ASSERT_EQUAL_UINT32(32, pl::Tracker::kPending);
  TEST_ASSERT_TRUE(sizeof(pl::Played) <= 320);
}

// A pause's time is no allowance for the position's next step: a seek while
// paused, or taken up as it plays on, as long as the pause was, isn't time
// played (it was: the clock had moved that far since the last look that
// counted). A plain pause still counts the audio either side of it.
void test_a_seek_after_a_pause_is_not_played() {
  Env env;
  pl::Tracker t(env);
  uint32_t pos = 0;
  auto run = [&](uint32_t ms) {
    for (uint32_t k = 0; k < ms / 20; ++k) {
      env.now += 20;
      pos += 20;
      t.tick(PlayState::Playing, pos, true, 600000);
    }
  };
  auto paused = [&](uint32_t ms) {
    for (uint32_t k = 0; k < ms / 600; ++k) {
      env.now += 600;
      t.tick(PlayState::Paused, pos, true, 600000);
    }
  };
  // 10 s played, a minute paused, 50 s forward while paused, 5 s played:
  // 15 s, less the look that saw the jump (as any seek's).
  t.began(1, 5, 0);
  run(10000);
  env.now += 20;
  t.tick(PlayState::Paused, pos, true, 600000);
  paused(60000);
  pos += 50000;
  paused(1200);
  run(5000);
  t.ended(End::Stopped);
  TEST_ASSERT_EQUAL_UINT32(10000 + 5000 - 20, t.oldest().playedMs);
  TEST_ASSERT_EQUAL_UINT16(1, t.oldest().pauses);
  t.pop();
  // The seek asked while paused and seen only as it plays on: the same.
  t.began(1, 5, 0);
  pos = 0;
  run(10000);
  env.now += 20;
  t.tick(PlayState::Paused, pos, true, 600000);
  paused(60000);
  pos += 50000;
  run(5000);
  t.ended(End::Stopped);
  TEST_ASSERT_EQUAL_UINT32(10000 + 5000 - 20, t.oldest().playedMs);
  t.pop();
  // A seek back while paused: nothing for it, and on from there.
  t.began(1, 5, 100000);
  pos = 100000;
  run(2000);
  env.now += 20;
  t.tick(PlayState::Paused, pos, true, 600000);
  paused(30000);
  pos = 30000;
  paused(600);
  run(1000);
  t.ended(End::Stopped);
  TEST_ASSERT_EQUAL_UINT32(2000 + 1000 - 20, t.oldest().playedMs);
  t.pop();
  // A plain pause and resume: the audio either side, and what played on
  // between the last look and the pause (a pass's worth).
  t.began(1, 5, 0);
  pos = 0;
  run(4000);
  pos += 10;
  env.now += 20;
  t.tick(PlayState::Paused, pos, true, 600000);
  paused(30000);
  run(3000);
  t.ended(End::Stopped);
  TEST_ASSERT_EQUAL_UINT32(4000 + 10 + 3000, t.oldest().playedMs);
  TEST_ASSERT_EQUAL_UINT16(1, t.oldest().pauses);
}

// Tracks of 7 s or less, one after the other, never open the gate (a
// start's 2 s, a track's last 5): their plays wait in the ring, 32 of them
// (8, before, was an album of interludes); the one past it is lost,
// counted. The gate opens once they stop.
void test_short_tracks_wait_for_the_gate() {
  using W = pl::WriteGate::Wait;
  Env env;
  pl::Tracker t(env);
  pl::WriteGate g;
  pl::WriteGate::In in;
  in.nowMs = 10000;
  in.playing = true;
  in.ringMs = 1400;
  in.ringCapacityMs = 1490;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  uint32_t opened = 0;
  for (uint32_t track = 0; track < pl::Tracker::kPending + 1; ++track) {
    t.began(1, track + 1, 0);
    ++in.trackSeq;  // its start (a join's too)
    for (uint32_t ms = 20; ms <= 7000; ms += 20) {
      env.now += 20;
      in.nowMs += 20;
      in.trackLeftMs = std::max<uint32_t>(1, 7000 - ms);
      t.tick(PlayState::Playing, ms, true, 7000);
      if (g.update(in) == W::None) ++opened;
    }
    t.ended(End::Completed);
  }
  TEST_ASSERT_EQUAL_UINT32(0, opened);
  TEST_ASSERT_EQUAL_UINT32(pl::Tracker::kPending, t.pending());
  TEST_ASSERT_EQUAL_UINT32(1, t.counts().overflow);
  TEST_ASSERT_EQUAL_UINT64(0x1001, t.oldest().file.pathHash);  // (the newest is the one lost)
  // Paused after the last of them: open.
  in.playing = false;
  in.trackLeftMs = 0;
  in.nowMs += 20;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
}

// ---------------------------------------------------------------------------
// The WriteGate.
// ---------------------------------------------------------------------------
void test_the_write_gate() {
  using W = pl::WriteGate::Wait;
  pl::WriteGate g;
  pl::WriteGate::In in;
  in.nowMs = 10000;
  in.playing = true;
  in.ringMs = 1400;
  in.ringCapacityMs = 1490;
  in.trackLeftMs = 120000;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  // A join (or a start): 2 s, and the decoder's next start taken up.
  in.trackSeq = 1;
  TEST_ASSERT_TRUE(g.update(in) == W::Start);
  in.nowMs += 1999;
  TEST_ASSERT_TRUE(g.update(in) == W::Start);
  in.nowMs += 1;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  in.seeking = true;
  TEST_ASSERT_TRUE(g.update(in) == W::Start);
  in.playing = false;  // (paused: a start not taken up holds nothing)
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  in.playing = true;
  in.seeking = false;
  // The gapless window: the heard track's last 5 s, the decoder at its end.
  in.trackLeftMs = 4999;
  TEST_ASSERT_TRUE(g.update(in) == W::TrackEnd);
  in.trackLeftMs = 0;  // (not known)
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  in.decoderAtEnd = true;
  TEST_ASSERT_TRUE(g.update(in) == W::TrackEnd);
  in.decoderAtEnd = false;
  in.ringMs = 700;
  TEST_ASSERT_TRUE(g.update(in) == W::Ring);
  in.playing = false;
  TEST_ASSERT_TRUE(g.update(in) == W::None);  // (paused: the ring is no matter)
  in.playing = true;
  in.ringMs = 1490;
  in.underruns = 1;
  TEST_ASSERT_TRUE(g.update(in) == W::Underrun);
  in.nowMs += 4999;
  TEST_ASSERT_TRUE(g.update(in) == W::Underrun);
  in.nowMs += 1;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  TEST_ASSERT_EQUAL_STRING("the gapless window", pl::WriteGate::waitName(W::TrackEnd));
}

// A list that moves holds the write (a fling, a finger on the A-Z rail: the
// loop that writes draws its frames, and the card worker waits too), playing
// or paused, for as long as it moves, and not a pass longer; what the audio
// holds it for is counted from its own moment all the same.
void test_the_write_gate_waits_for_a_moving_list() {
  using W = pl::WriteGate::Wait;
  pl::WriteGate g;
  pl::WriteGate::In in;
  in.nowMs = 10000;
  in.playing = true;
  in.ringMs = 1400;
  in.ringCapacityMs = 1490;
  in.trackLeftMs = 120000;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  in.listMoving = true;
  TEST_ASSERT_TRUE(g.update(in) == W::List);
  in.nowMs += 600000;  // (no limit: the finger stays on the rail)
  TEST_ASSERT_TRUE(g.update(in) == W::List);
  in.listMoving = false;
  TEST_ASSERT_TRUE(g.update(in) == W::None);  // the pass it rests
  // Paused or stopped too: the audio doesn't mind, the list's frames do.
  in.playing = false;
  in.listMoving = true;
  TEST_ASSERT_TRUE(g.update(in) == W::List);
  in.listMoving = false;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  in.playing = true;
  // A join under the moving list: its settle from the join, not from the
  // list's rest.
  in.listMoving = true;
  in.trackSeq = 1;
  TEST_ASSERT_TRUE(g.update(in) == W::Start);  // (the audio's word first)
  in.nowMs += pl::WriteGate::kSettleMs;
  TEST_ASSERT_TRUE(g.update(in) == W::List);
  in.listMoving = false;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  // An underrun under it the same: 5 s from the underrun.
  in.listMoving = true;
  in.underruns = 1;
  TEST_ASSERT_TRUE(g.update(in) == W::Underrun);
  in.nowMs += pl::WriteGate::kUnderrunMs;
  TEST_ASSERT_TRUE(g.update(in) == W::List);
  in.listMoving = false;
  TEST_ASSERT_TRUE(g.update(in) == W::None);
  TEST_ASSERT_EQUAL_STRING("a list moving", pl::WriteGate::waitName(W::List));
}

// ---------------------------------------------------------------------------
// What the log asks of the card, on the host FatFs model.
// ---------------------------------------------------------------------------
namespace {
// test_card_io's card: 32 GB, FAT32, 16 KB clusters, the sector cache and
// the firmware's wrapper in front.
struct Card {
  fatmodel::RamDisk disk{62333952u};
  SectorCache cache;
  fatmodel::Slot slot{0};
  CachedDrive wrapper{cache, slot};
  uint8_t scratch[fatmodel::kSS];
  FATFS fs;
  bool ok = false;
  Card() {
    if (!cache.begin(SectorCache::kDefaultEntries)) return;
    wrapper.setScratch(scratch);
    fatmodel::drive(0).disk = &disk;
    fatmodel::drive(0).wrapper = &wrapper;
    if (fatmodel::format(0, 16384) != FR_OK || fatmodel::mount(0, &fs) != FR_OK) return;
    wrapper.remember(disk.sectors());
    f_mkdir("0:/.player");
    f_mkdir("0:/music");
    ok = true;
  }
  ~Card() {
    f_mount(nullptr, "0:", 0);
    fatmodel::drive(0).disk = nullptr;
    fatmodel::drive(0).wrapper = nullptr;
    fatmodel::drive(0).cache = nullptr;
    fatmodel::drive(0).noInit = false;
    fatmodel::drive(0).asked = fatmodel::Drive::Asked();
  }
};
}  // namespace

void test_what_the_card_is_asked() {
  std::unique_ptr<Card> card(new Card());
  TEST_ASSERT_TRUE(card->ok);
  fatmodel::ModelFs fs;
  Device d;
  d.buf.assign(pl::kReadBytes, 0);  // the firmware's: 32 slots, 5 whole sectors a read
  TEST_ASSERT_TRUE(d.boot(fs));
  // The plays of a file, then the rotation, then one more.
  const fatmodel::RamDisk::Counts c0 = card->disk.counts;
  uint32_t up = 0;
  uint64_t mostWrites = 0, mostReads = 0;
  for (uint32_t i = 0; i < pl::kFileSlots; ++i) {
    const fatmodel::RamDisk::Counts a = card->disk.counts;
    Slot s = play(i, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    mostWrites = std::max<uint64_t>(mostWrites, card->disk.counts.writes - a.writes);
    mostReads = std::max<uint64_t>(mostReads, card->disk.counts.reads - a.reads);
  }
  const fatmodel::RamDisk::Counts c1 = card->disk.counts;
  const double writesPer = static_cast<double>(c1.writes - c0.writes) / pl::kFileSlots;
  const double sectorsPer = static_cast<double>(c1.writeSectors - c0.writeSectors) / pl::kFileSlots;
  const double readsPer = static_cast<double>(c1.reads - c0.reads) / pl::kFileSlots;
  printf("[play log] an append: %.2f card writes (%.2f sectors), %.2f reads past the cache on average; at most %llu "
         "writes and %llu reads; 80 bytes of the log a play\n",
         writesPer, sectorsPer, readsPer, static_cast<unsigned long long>(mostWrites),
         static_cast<unsigned long long>(mostReads));
  TEST_ASSERT_TRUE(writesPer <= 3.0);
  TEST_ASSERT_TRUE(mostWrites <= 6);
  // The rotation's: plays.1.jnl doesn't exist yet (no count), then a full
  // one (its 524,240 bytes read once for the count).
  for (int r = 0; r < 2; ++r) {
    fatmodel::RamDisk::Counts a = card->disk.counts;
    for (uint32_t i = 0; i < (r ? pl::kFileSlots : 1); ++i) {
      a = card->disk.counts;  // (the last append's: the rotation's)
      Slot s = play(i, 60000, up += 1000);
      TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    }
    const fatmodel::RamDisk::Counts b = card->disk.counts;
    if (r) TEST_ASSERT_TRUE(b.readSectors - a.readSectors >= pl::kFileBytes / 512);
    printf("[play log] %s: %llu card writes, %llu reads (%llu sectors) past the cache\n",
           r ? "a rotation with plays.1.jnl full (its 524,240 bytes counted)" : "the first rotation (no plays.1.jnl)",
           static_cast<unsigned long long>(b.writes - a.writes), static_cast<unsigned long long>(b.reads - a.reads),
           static_cast<unsigned long long>(b.readSectors - a.readSectors));
  }
  TEST_ASSERT_EQUAL_UINT32(pl::kFileSlots * 2 + 1, d.j->lastSeq());
  TEST_ASSERT_EQUAL_UINT32(pl::kFileSlots, d.j->lastDropped());  // (no software read any)
  // The boot's scan over the cap's worst case: both files full.
  for (uint32_t i = 0; i + 2 < pl::kFileSlots; ++i) {
    Slot s = play(i, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  const uint32_t last = d.j->lastSeq();
  const fatmodel::RamDisk::Counts a = card->disk.counts;
  TEST_ASSERT_TRUE(d.boot(fs, pl::kReadBytes / pl::kSlot));
  const fatmodel::RamDisk::Counts b = card->disk.counts;
  printf("[play log] the boot's scan, both files full: %llu bytes read (%u slots), %llu card reads (%llu sectors)\n",
         static_cast<unsigned long long>(d.j->booted().bytes), static_cast<unsigned>(d.j->booted().slots),
         static_cast<unsigned long long>(b.reads - a.reads), static_cast<unsigned long long>(b.readSectors - a.readSectors));
  TEST_ASSERT_EQUAL_UINT64(2ull * pl::kFileBytes, d.j->booted().bytes);
  TEST_ASSERT_EQUAL_UINT32(last, d.j->booted().cardSeq);
}

// A cut append's part slot on FatFs itself (test_a_tail_is_cut_off's rule,
// which CutFs models): f_lseek back to the slot boundary inside the file,
// then f_write past its end, leaves the file a whole number of slots, the
// slot whole, as a truncate first would. Tails of 37 bytes (across a
// sector's end), 79 and 1.
void test_a_tail_on_the_fatfs_card() {
  std::unique_ptr<Card> card(new Card());
  TEST_ASSERT_TRUE(card->ok);
  fatmodel::ModelFs fs;
  Device d(kDevA, 16);
  TEST_ASSERT_TRUE(d.boot(fs));
  uint32_t up = 0;
  for (int i = 0; i < 6; ++i) {
    Slot s = play(i, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  for (const uint32_t tail : {37u, 79u, 1u}) {
    tagstore::File* f = fs.open(kLog, tagstore::Fs::Mode::Update);
    TEST_ASSERT_NOT_NULL(f);
    const uint32_t size = f->size();
    TEST_ASSERT_EQUAL_UINT32(0, size % pl::kSlot);
    const Bytes part(tail, 0x5A);
    TEST_ASSERT_TRUE(f->write(size, part.data(), tail));
    TEST_ASSERT_TRUE(fs.close(f));
    Slot s = play(100 + tail, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    f = fs.open(kLog, tagstore::Fs::Mode::Read);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(size + pl::kSlot, f->size());
    uint8_t have[pl::kSlot], want[pl::kSlot];
    TEST_ASSERT_TRUE(f->read(size, have, pl::kSlot));
    fs.close(f);
    pl::encode(s, want);
    TEST_ASSERT_EQUAL_MEMORY(want, have, pl::kSlot);
  }
  // The next boot's scan reads every slot.
  TEST_ASSERT_TRUE(d.boot(fs));
  TEST_ASSERT_EQUAL_UINT32(9, d.j->booted().slots);
  TEST_ASSERT_EQUAL_UINT32(9, d.j->booted().cardSeq);
}

// The same card mounted again during the boot (the card guard's remount),
// on FatFs itself, with NVS behind the card (an older NVS image put back:
// the boot rules need every slot): a file the scan holds open across the
// remount reads nothing more, so left so the scan misses the rest of it;
// remounted() begins the scan again. The card out for the whole scan (every
// open refused: done with nothing) and back before the boot's first slot:
// the scan again. A remount after a whole scan, before any slot: the same
// boot count, not one more. After a slot: the cut-rename rule only.
void test_a_remount_and_the_boots_scan() {
  std::unique_ptr<Card> card(new Card());
  TEST_ASSERT_TRUE(card->ok);
  fatmodel::ModelFs fs;
  Device d(kDevA, 40);
  TEST_ASSERT_TRUE(d.boot(fs));
  uint32_t up = 0;
  for (int i = 0; i < 60; ++i) {  // plays.1.jnl seqs 1-40, plays.jnl 41-60, all of boot 1
    Slot s = play(i, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  auto bootBehind = [&](uint32_t piece) {
    d.nvs.ids.seq = 0;
    d.nvs.ids.boots = 0;
    d.j.reset(new Journal(fs, d.nvs, d.config()));
    TEST_ASSERT_TRUE(d.j->begin(d.id, d.acks));
    return d.j->scan(piece);
  };
  auto remount = [&] {
    fatmodel::drive(0).noInit = true;  // pulled and put back: FatFs mounts again at its next call
    TEST_ASSERT_TRUE(fs.exists(kPrev));  // (that call)
  };
  auto finish = [&] {
    for (int guard = 0; guard < 1000 && !d.j->scan(5); ++guard) {
    }
    TEST_ASSERT_TRUE(d.j->ready());
  };
  // Left so: plays.jnl (the scan's first file) open, 5 of its 20 slots read.
  TEST_ASSERT_FALSE(bootBehind(5));
  remount();
  finish();
  TEST_ASSERT_EQUAL_UINT32(45, d.j->lastSeq());  // (46 next: a seq used twice)
  // remounted().
  TEST_ASSERT_FALSE(bootBehind(5));
  remount();
  d.j->remounted();
  TEST_ASSERT_FALSE(d.j->ready());
  finish();
  TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().restarts);
  TEST_ASSERT_EQUAL_UINT32(60, d.j->booted().slots);
  TEST_ASSERT_EQUAL_UINT32(60, d.j->booted().cardSeq);
  TEST_ASSERT_EQUAL_UINT32(60, d.j->lastSeq());
  TEST_ASSERT_EQUAL_UINT32(2, d.j->bootSeq());
  // The card out for the whole scan, then back.
  TEST_ASSERT_FALSE(bootBehind(0));
  fatmodel::RamDisk* disk = fatmodel::drive(0).disk;
  fatmodel::drive(0).disk = nullptr;
  finish();
  TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().slots);
  TEST_ASSERT_EQUAL_UINT32(0, d.j->lastSeq());
  fatmodel::drive(0).disk = disk;
  remount();
  d.j->remounted();
  finish();
  TEST_ASSERT_EQUAL_UINT32(60, d.j->lastSeq());
  TEST_ASSERT_EQUAL_UINT32(2, d.j->bootSeq());
  TEST_ASSERT_EQUAL_UINT32(2, d.nvs.ids.boots);
  // Again, the scan whole: the same count.
  remount();
  d.j->remounted();
  finish();
  TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().restarts);
  TEST_ASSERT_EQUAL_UINT32(2, d.j->bootSeq());
  TEST_ASSERT_EQUAL_UINT32(2, d.nvs.ids.boots);
  // After a slot: no scan again.
  Slot s = play(100, 60000, up += 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(61, s.seq);
  TEST_ASSERT_EQUAL_UINT32(2, s.bootSeq);
  remount();
  d.j->remounted();
  TEST_ASSERT_TRUE(d.j->ready());
  TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().restarts);
  s = play(101, 60000, up += 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(62, s.seq);
}

// ---------------------------------------------------------------------------
// A question or an open the card fails (docs/METADATA.md 3.8, 3.10).
// ---------------------------------------------------------------------------

// A slot's open the card fails says nothing of plays.jnl. The question that
// follows (is there one?) decides whether a new one is made, and one the card
// fails decides nothing: Create would empty a log that is there, every play
// not yet sent with it. (FatFs's f_stat says "no" for a read that failed
// too, so exists() can't be asked that.)
void test_an_open_the_card_fails_never_empties_the_log() {
  FlakyFs fs;
  Device d;
  TEST_ASSERT_TRUE(d.boot(fs));
  for (int i = 0; i < 3; ++i) {
    Slot s = play(i, 60000, 1000 * (i + 1));
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  const Bytes three = fs.bytes(kLog);
  TEST_ASSERT_EQUAL_size_t(3 * pl::kSlot, three.size());
  // The open and the question after it both fail; the card is back for
  // whatever comes next.
  fs.failing(0, 2);
  Slot s = play(10, 60000, 9000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
  TEST_ASSERT_EQUAL_STRING("lost 4", d.ops.back().c_str());
  TEST_ASSERT_TRUE(fs.bytes(kLog) == three);
  // The open alone fails, the question says the log is there: not written,
  // not emptied.
  fs.failing(0, 1);
  s = play(11, 60000, 10000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
  TEST_ASSERT_TRUE(fs.bytes(kLog) == three);
  // The card answers: the log goes on where it was (the seqs the lost plays
  // took aren't used again).
  fs.failing(-1, -1);
  s = play(12, 60000, 11000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(6, s.seq);
  TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, fs.bytes(kLog).size());
  const Bytes four = fs.bytes(kLog);
  TEST_ASSERT_TRUE(Bytes(four.begin(), four.begin() + three.size()) == three);
  // No plays.jnl, and the question fails: none is made (it may be there);
  // answered, it is.
  FlakyFs none;
  Device e;
  TEST_ASSERT_TRUE(e.boot(none));
  none.failing(0, 2);
  s = play(1, 60000, 1000);
  TEST_ASSERT_TRUE(e.j->append(s) == Journal::Wrote::Failed);
  TEST_ASSERT_TRUE(none.dir.count(kLog) == 0);
  none.failing(-1, -1);
  s = play(2, 60000, 2000);
  TEST_ASSERT_TRUE(e.j->append(s) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_size_t(pl::kSlot, none.bytes(kLog).size());
}

// The questions the cut-rename rule, the trim and the rotation ask (which of
// the log's files are there, on which chains), each failed in its turn:
// nothing is removed or renamed on an answer the card didn't give, so no
// chain is freed while another name uses it. A rule left unanswered holds
// the slots as a refused rename does, until the card answers.
void test_a_question_the_card_fails_frees_no_chain() {
  const uint64_t epoch = 0x1122334455667788ull;
  auto slotOf = [&](uint32_t seq) {
    Slot s = play(seq, 60000, 1000 * seq);
    s.seq = seq;
    s.epoch = epoch;
    std::memcpy(s.deviceId, kDevA, 6);
    s.bootSeq = 1;
    Bytes b(pl::kSlot);
    pl::encode(s, b.data());
    return b;
  };
  auto two = [&](uint32_t a, uint32_t b) {
    Bytes out = slotOf(a);
    const Bytes more = slotOf(b);
    out.insert(out.end(), more.begin(), more.end());
    return out;
  };
  const char* const kTwin1 = "/.player/plays.xl1";
  const char* const kTwin2 = "/.player/plays.xl2";
  // plays.1.jnl, its every play acknowledged, on one chain with a twin (an
  // earlier cut rename's); a twin on a chain of its own; plays.jnl, full at a
  // cap of 2, its plays past ACKS's mark.
  auto card = [&](FlakyFs& fs) {
    fs.put(kPrev, two(1, 2));
    fs.link(kPrev, kTwin1);
    fs.put(kTwin2, slotOf(9));
    fs.put(kLog, two(3, 4));
  };
  auto device = [&](Device& d) {
    d.nvs.ids.epoch = epoch;
    d.nvs.ids.seq = 4;
    d.nvs.ids.boots = 1;
    setAcks(d.acks, {ack(kDevA, epoch, 2)});
  };
  auto message = [](const FlakyFs& fs, const char* what, long k, long n) {
    char msg[200];
    std::snprintf(msg, sizeof(msg), "%s %ld of %ld: %s", what, k, n,
                  fs.violations.empty() ? "no chain freed" : fs.violations[0].c_str());
    return std::string(msg);
  };

  // ---- the boot: the rule, the scan's opens, the trim ----
  long events = 0;
  {
    FlakyFs fs;
    card(fs);
    Device d(kDevA, 2);
    device(d);
    fs.failing(1000000, -1);  // (counted, none failed)
    TEST_ASSERT_TRUE(d.boot(fs));
    events = fs.events;
    // Every answer given: the twin on its own chain collected, plays.1.jnl
    // kept though ACKS has all of it (its twin shares its chain).
    TEST_ASSERT_TRUE(fs.dir.count(kTwin2) == 0);
    TEST_ASSERT_TRUE(fs.dir.count(kPrev) == 1);
    TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().twins);
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
    TEST_ASSERT_FALSE(d.j->unsettled());
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
  TEST_ASSERT_GREATER_THAN_INT32(10, events);
  int unanswered = 0;
  for (long k = 0; k < events; ++k) {
    FlakyFs fs;
    card(fs);
    Device d(kDevA, 2);
    device(d);
    fs.failing(k, k + 1);
    d.boot(fs);
    const std::string msg = message(fs, "the boot's question", k, events);
    TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.bytes(kPrev) == two(1, 2), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.bytes(kTwin1) == two(1, 2), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.bytes(kLog) == two(3, 4), msg.c_str());
    TEST_ASSERT_EQUAL_MESSAGE(d.j->unanswered(), d.j->unsettled(), msg.c_str());
    unanswered += d.j->unanswered();
    // The card answering again (the next boot, or a remount): as if nothing
    // had failed.
    fs.failing(-1, -1);
    TEST_ASSERT_TRUE_MESSAGE(d.boot(fs), msg.c_str());
    TEST_ASSERT_FALSE_MESSAGE(d.j->unsettled(), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.dir.count(kTwin2) == 0, msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.dir.count(kPrev) == 1 && fs.dir.count(kTwin1) == 1, msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg.c_str());
  }
  TEST_ASSERT_GREATER_THAN_INT(0, unanswered);

  // ---- a rule left unanswered: no slot until the card answers ----
  {
    FlakyFs fs;
    card(fs);
    Device d(kDevA, 9);
    device(d);
    fs.failing(0, -1);  // (every question, for now)
    d.boot(fs);
    TEST_ASSERT_TRUE(d.j->unanswered());
    TEST_ASSERT_FALSE(d.j->blocked());
    Slot s = play(20, 60000, 20000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_STRING("lost 5", d.ops.back().c_str());
    TEST_ASSERT_TRUE(fs.bytes(kLog) == two(3, 4));
    TEST_ASSERT_TRUE(fs.dir.count(kTwin2) == 1);
    fs.failing(-1, -1);
    s = play(21, 60000, 21000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(6, s.seq);
    TEST_ASSERT_FALSE(d.j->unanswered());
    TEST_ASSERT_TRUE(fs.dir.count(kTwin2) == 0);  // (the rule again, at that append: collected)
    TEST_ASSERT_TRUE(fs.violations.empty());
  }

  // ---- the rotation (a cap of 2: the next play rotates) ----
  long asked = 0;
  for (long k = -1; k < asked || k < 0; ++k) {
    FlakyFs fs;
    card(fs);
    Device d(kDevA, 2);
    device(d);
    TEST_ASSERT_TRUE(d.boot(fs));
    const std::map<std::string, int> before = fs.dir;
    fs.failing(k < 0 ? 1000000 : k, k < 0 ? -1 : k + 1);
    Slot s = play(30, 60000, 30000);
    const Journal::Wrote w = d.j->append(s);
    if (k < 0) {
      // Every answer given: plays.1.jnl to a twin name (its twin shares its
      // chain: a delete would free it), plays.jnl after it, a new plays.jnl.
      asked = fs.events;
      TEST_ASSERT_TRUE(w == Journal::Wrote::Ok);
      TEST_ASSERT_TRUE(fs.shared(kTwin1, kTwin2));
      TEST_ASSERT_TRUE(fs.bytes(kPrev) == two(3, 4));
      TEST_ASSERT_EQUAL_size_t(pl::kSlot, fs.bytes(kLog).size());
      TEST_ASSERT_TRUE(fs.violations.empty());
      TEST_ASSERT_GREATER_THAN_INT32(3, asked);
      continue;
    }
    const std::string msg = message(fs, "the rotation's question", k, asked);
    TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(w == Journal::Wrote::Failed, msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.dir == before, msg.c_str());  // (nothing renamed or removed)
    TEST_ASSERT_TRUE_MESSAGE(fs.bytes(kLog) == two(3, 4), msg.c_str());
    // The card answers: the next play rotates as it would have.
    fs.failing(-1, -1);
    s = play(31, 60000, 31000);
    TEST_ASSERT_TRUE_MESSAGE(d.j->append(s) == Journal::Wrote::Ok, msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.shared(kTwin1, kTwin2), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.bytes(kPrev) == two(3, 4), msg.c_str());
    TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg.c_str());
  }
}

// ---------------------------------------------------------------------------
// A slot the card fails, tried again (docs/METADATA.md 3.10): the journal's
// again(), the rotation's count kept for its note, a plays.1.jnl it can't
// read, and the Outbox (the play kept first in line, three tries, the
// orderly end's flush).
// ---------------------------------------------------------------------------
namespace {

std::vector<Slot> slotsOf(const Bytes& b) {
  std::vector<Slot> v;
  for (size_t at = 0; at + pl::kSlot <= b.size(); at += pl::kSlot) {
    Slot s;
    TEST_ASSERT_TRUE(pl::decode(b.data() + at, pl::kSlot, &s) == pl::Verdict::Ok);
    v.push_back(s);
  }
  return v;
}

// A card with plays.1.jnl (seqs 1-4) and plays.jnl (5-8) under a cap of 4,
// no ACKS: the next append rotates, and its note says 4.
struct Rotating {
  CutFs card;
  FaultFs fs{card};
  Device d{kDevA, 4};
  uint32_t up = 0;
  Rotating() {
    TEST_ASSERT_TRUE(d.boot(fs));
    for (int i = 0; i < 8; ++i) TEST_ASSERT_TRUE(add() == Journal::Wrote::Ok);
    d.ops.clear();
  }
  Journal::Wrote add() {
    up += 1000;
    Slot s = play(up / 1000, 60000, up);
    return d.j->append(s);
  }
};

}  // namespace

// The journal's again(): the slot the card failed, the same seq and bytes,
// once on the card however the first try ended.
void test_a_failed_slot_is_tried_again() {
  // The card refuses the write (full, or gone for a moment): nothing on it;
  // tried again, the slot is there, its seq the one NVS took at first.
  {
    CutFs card;
    FaultFs fs(card);
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Off);  // nothing to try again
    Slot a = play(1, 60000, 1000);
    TEST_ASSERT_TRUE(d.j->append(a) == Journal::Wrote::Ok);
    TEST_ASSERT_FALSE(d.j->owes());
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Off);  // (it was written)
    card.refuse = true;
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(d.j->owes());
    TEST_ASSERT_EQUAL_UINT32(2, b.seq);
    TEST_ASSERT_EQUAL_STRING("lost 2", d.ops.back().c_str());
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Failed);  // refused still
    TEST_ASSERT_TRUE(d.j->owes());
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());
    card.refuse = false;
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_STRING("play 2", d.ops.back().c_str());
    TEST_ASSERT_FALSE(d.j->owes());
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Off);
    TEST_ASSERT_EQUAL_UINT32(2, d.nvs.seqWrites);  // (one seq a play: none for the tries)
    uint8_t want[pl::kSlot];
    pl::encode(b, want);
    const Bytes log = card.bytes(kLog);
    TEST_ASSERT_EQUAL_size_t(2 * pl::kSlot, log.size());
    TEST_ASSERT_EQUAL_MEMORY(want, log.data() + pl::kSlot, pl::kSlot);
    // A slot not tried again is left behind by the next: its seq isn't
    // used again, and again() is the new one's alone.
    card.refuse = true;
    Slot c = play(3, 60000, 3000);
    TEST_ASSERT_TRUE(d.j->append(c) == Journal::Wrote::Failed);
    card.refuse = false;
    Slot e = play(4, 60000, 4000);
    TEST_ASSERT_TRUE(d.j->append(e) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(4, e.seq);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Off);
    TEST_ASSERT_EQUAL_size_t(3 * pl::kSlot, card.bytes(kLog).size());
    TEST_ASSERT_TRUE(card.violations.empty());
  }
  // The write landed and only its sync and its close failed (the card took
  // the data and refused the rest): the slot is the log's last, unsynced.
  // Tried again it goes over itself, where it is, and is synced: one slot,
  // and a power cut after it keeps it. With one slot before it, and none.
  for (int before = 0; before <= 1; ++before) {
    CutFs card;
    FaultFs fs(card);
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    if (before) {
      Slot a = play(1, 60000, 1000);
      TEST_ASSERT_TRUE(d.j->append(a) == Journal::Wrote::Ok);
    }
    fs.noSync.fail = 1;
    fs.noClose.fail = 1;
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    const size_t whole = (before + 1) * pl::kSlot;
    TEST_ASSERT_EQUAL_size_t(whole, card.bytes(kLog).size());  // (landed)
    TEST_ASSERT_EQUAL_size_t(before * pl::kSlot, card.reboot(cutfs::Variant::LoseUnsynced).bytes(kLog).size());
    const uint32_t wrote = fs.writes;
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(wrote + 1, fs.writes);  // (over itself: its sync's)
    const Bytes log = card.bytes(kLog);
    TEST_ASSERT_EQUAL_size_t(whole, log.size());  // one slot, not two
    uint8_t want[pl::kSlot];
    pl::encode(b, want);
    TEST_ASSERT_EQUAL_MEMORY(want, log.data() + before * pl::kSlot, pl::kSlot);
    TEST_ASSERT_TRUE(card.reboot(cutfs::Variant::LoseUnsynced).bytes(kLog) == log);
    Slot c = play(3, 60000, 3000);
    TEST_ASSERT_TRUE(d.j->append(c) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_size_t(whole + pl::kSlot, card.bytes(kLog).size());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(before) + 2, c.seq);
  }
  // The write itself said no and landed all the same (its answer lost): the
  // same, one slot.
  {
    CutFs card;
    FaultFs fs(card);
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    fs.lands = true;
    fs.noWrite.fail = 1;
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());
  }
  // A read the card fails on the way (was it written?) answers nothing:
  // not written at that try, nor a second slot at the next.
  {
    CutFs card;
    FaultFs fs(card);
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    fs.lands = true;
    fs.noWrite.fail = 1;
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    fs.badPath = kLog;
    fs.badTo = 0xFFFFFFFFu;
    fs.bad.fail = 1;
    const uint32_t wrote = fs.writes;
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_UINT32(wrote, fs.writes);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());
  }
  // The open and the question after it (2.20's rule, above): tried again,
  // the log goes on with that slot.
  {
    FlakyFs fs;
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    Slot a = play(1, 60000, 1000);
    TEST_ASSERT_TRUE(d.j->append(a) == Journal::Wrote::Ok);
    fs.failing(0, 2);
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    fs.failing(-1, -1);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    const std::vector<Slot> s = slotsOf(fs.bytes(kLog));
    TEST_ASSERT_EQUAL_size_t(2, s.size());
    TEST_ASSERT_EQUAL_UINT32(2, s[1].seq);
  }
  // The same card mounted again between the tries: the rule again, and the
  // slot.
  {
    CutFs card;
    FaultFs fs(card);
    Device d;
    TEST_ASSERT_TRUE(d.boot(fs));
    card.dead = true;  // pulled
    Slot b = play(2, 60000, 2000);
    TEST_ASSERT_TRUE(d.j->append(b) == Journal::Wrote::Failed);
    card.dead = false;  // put back
    d.j->remounted();
    TEST_ASSERT_TRUE(d.j->ready());  // (a slot was asked for: no scan again)
    TEST_ASSERT_TRUE(d.j->owes());
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());
  }
}

// On FatFs itself: the slot's data sector written, the directory's write
// refused (f_sync fails with the entry's new size still in FatFs's window).
// Tried again, the slot is found where it landed and its sync done: after a
// mount the card holds each slot once.
void test_a_failed_sync_on_the_fatfs_card() {
  std::unique_ptr<Card> card(new Card());
  TEST_ASSERT_TRUE(card->ok);
  fatmodel::ModelFs fs;
  Device d;
  TEST_ASSERT_TRUE(d.boot(fs));
  uint32_t up = 0;
  for (int i = 0; i < 3; ++i) {
    Slot s = play(i, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
  }
  // Where plays.jnl's directory entry is.
  auto* f = static_cast<fatmodel::ModelFile*>(fs.open(kLog, tagstore::Fs::Mode::Read));
  TEST_ASSERT_NOT_NULL(f);
  const uint32_t dirSector = static_cast<uint32_t>(f->fil.dir_sect);
  fs.close(f);
  fatmodel::Drive& drv = fatmodel::drive(0);
  drv.noAnswerWriteLba = dirSector;
  drv.errorWriteLba = true;  // (refused, the card still there)
  Slot s = play(10, 60000, up += 1000);
  TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
  TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Failed);  // refused still
  drv.noAnswerWriteLba = UINT32_MAX;
  drv.errorWriteLba = false;
  TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(4, s.seq);
  Slot t = play(11, 60000, up += 1000);
  TEST_ASSERT_TRUE(d.j->append(t) == Journal::Wrote::Ok);
  TEST_ASSERT_EQUAL_UINT32(5, t.seq);
  // As the card holds it (mounted again: nothing of FatFs's memory).
  drv.noInit = true;
  TEST_ASSERT_TRUE(fs.exists(kLog));
  tagstore::File* r = fs.open(kLog, tagstore::Fs::Mode::Read);
  TEST_ASSERT_NOT_NULL(r);
  TEST_ASSERT_EQUAL_UINT32(5 * pl::kSlot, r->size());
  Bytes log(r->size());
  TEST_ASSERT_TRUE(r->read(0, log.data(), r->size()));
  fs.close(r);
  const std::vector<Slot> all = slotsOf(log);
  TEST_ASSERT_EQUAL_size_t(5, all.size());
  for (uint32_t i = 0; i < 5; ++i) TEST_ASSERT_EQUAL_UINT32(i + 1, all[i].seq);
  uint8_t want[pl::kSlot];
  pl::encode(s, want);
  TEST_ASSERT_EQUAL_MEMORY(want, log.data() + 3 * pl::kSlot, pl::kSlot);
}

// A rotation a step of which the card fails: tried again, the card ends as
// one the card never failed (byte for byte), its drop note not lost with
// the step.
void test_a_rotation_tried_again() {
  // The card nothing failed.
  Rotating clean;
  TEST_ASSERT_TRUE(clean.add() == Journal::Wrote::Ok);
  const Bytes wantLog = clean.card.bytes(kLog), wantPrev = clean.card.bytes(kPrev);
  TEST_ASSERT_EQUAL_size_t(2 * pl::kSlot, wantLog.size());
  TEST_ASSERT_EQUAL_UINT32(4, clean.d.j->lastDropped());
  const std::vector<std::string> wantOps = {"delete plays.1.jnl rotation", "rename plays.jnl plays.1.jnl", "note 4",
                                            "play 9"};
  TEST_ASSERT_TRUE(clean.d.ops == wantOps);
  auto same = [&](Rotating& r) {
    TEST_ASSERT_TRUE(r.card.bytes(kLog) == wantLog);
    TEST_ASSERT_TRUE(r.card.bytes(kPrev) == wantPrev);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());
    TEST_ASSERT_EQUAL_UINT32(0, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.card.violations.empty());
    TEST_ASSERT_TRUE(r.card.reboot(cutfs::Variant::LoseUnsynced).bytes(kLog) == wantLog);
    // ... and the log goes on.
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_size_t(3 * pl::kSlot, r.card.bytes(kLog).size());
  };
  // Step 1's question, its delete: nothing done; again from its start (the
  // count once, not twice).
  {
    Rotating r;
    r.fs.noEntry.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kPrev).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    same(r);
  }
  {
    Rotating r;
    r.fs.noRemove.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_UINT32(0, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    same(r);
  }
  // Step 2's rename refused, plays.1.jnl gone already: its count is owed,
  // and the note goes with the slot when it lands.
  {
    Rotating r;
    r.fs.noRename.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.card.dir.count(kPrev) == 0);
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kLog).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    const std::vector<std::string> ops = {"delete plays.1.jnl rotation", "lost 9", "rename plays.jnl plays.1.jnl",
                                          "note 4", "play 9"};
    TEST_ASSERT_TRUE(r.d.ops == ops);
    same(r);
  }
  // ... cut between its two directory writes (both names on one chain): the
  // cut-rename rule before the slot, as at a mount; plays.jnl's slots stay
  // (under plays.1.jnl and a twin), none is counted dropped for it, no
  // chain freed.
  {
    Rotating r;
    r.fs.halfRename.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.card.shared(kLog, kPrev));
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE(r.card.bytes(kLog) == wantLog);
    TEST_ASSERT_TRUE(r.card.bytes(kPrev) == wantPrev);
    TEST_ASSERT_TRUE(r.card.shared(kPrev, "/.player/plays.xl1"));
    TEST_ASSERT_TRUE(r.card.violations.empty());
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());
  }
  // Step 3: the new plays.jnl's open refused; made and its write refused;
  // written and its sync and close refused (the note and the slot there,
  // unsynced: over themselves, no second note, no second slot).
  {
    Rotating r;
    r.fs.noCreate.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.card.dir.count(kLog) == 0);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    same(r);
  }
  {
    Rotating r;
    r.fs.noWrite.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(0, r.card.bytes(kLog).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    same(r);
  }
  {
    Rotating r;
    r.fs.noSync.fail = 1;
    r.fs.noClose.fail = 1;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.card.bytes(kLog) == wantLog);  // (landed)
    TEST_ASSERT_EQUAL_size_t(0, r.card.reboot(cutfs::Variant::LoseUnsynced).bytes(kLog).size());
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    same(r);
  }
  // Given up on (the caller's three tries): the count stays owed, and the
  // next play's slot carries the note, with that play's dates.
  {
    Rotating r;
    r.fs.noRename.fail = 3;
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->owedDrops());
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Ok);  // seq 10
    const std::vector<Slot> s = slotsOf(r.card.bytes(kLog));
    TEST_ASSERT_EQUAL_size_t(2, s.size());
    TEST_ASSERT_EQUAL_UINT8(pl::kKindDrop, s[0].kind);
    TEST_ASSERT_EQUAL_UINT32(4, s[0].playedMs);
    TEST_ASSERT_EQUAL_UINT32(10, s[1].seq);
    TEST_ASSERT_EQUAL_UINT32(s[1].uptimeMs, s[0].uptimeMs);
    TEST_ASSERT_EQUAL_UINT64(s[1].startedAt, s[0].startedAt);
    TEST_ASSERT_EQUAL_UINT32(0, r.d.j->owedDrops());
  }
}

// plays.1.jnl with a stretch the card can't read (FP3: the log wedged for
// good once plays.jnl was full, each play paying the read and lost). A read
// that fails once: nothing goes on it, and tried again the rotation counts
// every slot. One that fails every time: at the third rotation in a row the
// file goes all the same, its note counting what could be read, and the
// log goes on.
void test_a_plays1_that_cant_be_read() {
  // (The device's buffer here: 3 slots a read, so plays.1.jnl's 4 are two
  // reads, the second its last slot.)
  auto bad = [](Rotating& r, long times) {
    r.fs.badPath = kPrev;
    r.fs.badFrom = 3 * pl::kSlot;
    r.fs.badTo = 4 * pl::kSlot;
    r.fs.bad.fail = times;
  };
  {
    Rotating r;
    bad(r, 1);
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kPrev).size());  // nothing removed
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kLog).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());  // (none dropped unseen)
    const std::vector<std::string> ops = {"lost 9", "delete plays.1.jnl rotation", "rename plays.jnl plays.1.jnl",
                                          "note 4", "play 9"};
    TEST_ASSERT_TRUE(r.d.ops == ops);
  }
  {
    Rotating r;
    bad(r, -1);
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kPrev).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);  // the third: on with what it read
    const std::vector<std::string> ops = {"lost 9",   "lost 9", "unread plays.1.jnl 3", "delete plays.1.jnl rotation",
                                          "rename plays.jnl plays.1.jnl", "note 3", "play 9"};
    TEST_ASSERT_TRUE(r.d.ops == ops);
    TEST_ASSERT_EQUAL_UINT32(3, r.d.j->lastDropped());
    std::vector<Slot> s = slotsOf(r.card.bytes(kLog));
    TEST_ASSERT_EQUAL_size_t(2, s.size());
    TEST_ASSERT_EQUAL_UINT32(9, s[1].seq);
    TEST_ASSERT_TRUE(r.card.violations.empty());
    // The log goes on: its next rotation (a plays.1.jnl that reads) is as
    // ever, at its first try.
    for (int i = 0; i < 2; ++i) TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Ok);
    r.fs.badPath.clear();
    r.d.ops.clear();
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Ok);  // seq 12: rotates
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());
    TEST_ASSERT_EQUAL_STRING("delete plays.1.jnl rotation", r.d.ops.front().c_str());
  }
  // Three in a row, not three in all: one that reads between starts the
  // count again. (Three plays' rotations here, as a caller that gave each
  // up after two tries.)
  {
    Rotating r;
    bad(r, 2);
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);  // it read
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());
    for (const std::string& o : r.d.ops) TEST_ASSERT_TRUE(o.compare(0, 6, "unread") != 0);
  }
  // ... a read that works starts it again whatever the rotation then comes
  // to: two that failed, one that read (its delete refused), one that
  // failed: that one is the first of its row, and nothing goes on it.
  {
    Rotating r;
    bad(r, 2);
    TEST_ASSERT_TRUE(r.add() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    r.fs.noRemove.fail = 1;
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);  // (read whole; the delete refused)
    bad(r, 1);
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_EQUAL_size_t(4 * pl::kSlot, r.card.bytes(kPrev).size());
    TEST_ASSERT_TRUE(r.d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(4, r.d.j->lastDropped());
    for (const std::string& o : r.d.ops) TEST_ASSERT_TRUE(o.compare(0, 6, "unread") != 0);
  }
  // Its open refused each time (the same fault, before any slot is read):
  // the third goes on, counting none.
  {
    struct OpenFails : FaultFs {
      using FaultFs::FaultFs;
      long fails = -1;
      tagstore::File* open(const char* p, Mode m) override {
        if (m == Mode::Read && std::string(p) == kPrev && fails != 0) {
          if (fails > 0) --fails;
          return nullptr;
        }
        return FaultFs::open(p, m);
      }
    };
    CutFs card;
    OpenFails fs(card);
    Device d(kDevA, 4);
    TEST_ASSERT_TRUE(d.boot(fs));
    uint32_t up = 0;
    for (int i = 0; i < 8; ++i) {
      Slot s = play(i, 60000, up += 1000);
      TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    }
    d.ops.clear();
    Slot s = play(9, 60000, up += 1000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(0, d.j->lastDropped());
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, card.bytes(kLog).size());  // (no note: nothing seen)
    TEST_ASSERT_EQUAL_STRING("unread plays.1.jnl 0", d.ops[2].c_str());
  }
}

namespace {

// The Outbox's side of the firmware: the played file's f_stat (its answers
// scripted), the board's clocks, each try's word.
struct OutEnv : pl::Outbox::Env {
  uint64_t now = 100000;
  uint64_t perStat = 0;  // ms a stat takes (a slow card)
  uint64_t perLook = 0;  // ms between two looks at the clock
  uint64_t wall = 1792100000000ull;  // the wall clock at mono 0 (0: unset)
  std::vector<File> stats;  // the next answers (then There)
  uint32_t statCalls = 0;
  std::vector<std::string> lines;
  File stat(const char* path, uint32_t* size, uint32_t* fatTime) override {
    (void)path;
    now += perStat;
    const File f = statCalls < stats.size() ? stats[statCalls] : File::There;
    ++statCalls;
    if (f == File::There) {
      *size = 1000;
      *fatTime = 0x5D4773D5;
    }
    return f;
  }
  uint64_t startedAt(uint64_t startMono, uint8_t* clock) override {
    *clock = wall ? pl::kClockRtc : 77;  // (unset: the Outbox says so, whatever this says)
    return wall ? wall + startMono : 0;
  }
  uint64_t monoMs() override {
    now += perLook;
    return now;
  }
  void tried(const pl::Outbox::Tried& t, const pl::Played& p) override {
    using Did = pl::Outbox::Did;
    static const char* const kWhy[] = {"", " gone", " off", " nvs", " blocked", " stat", " append"};
    std::string line = t.did == Did::Written ? "written" : t.did == Did::Kept ? "kept" : t.gaveUp ? "lost" : "dropped";
    line += kWhy[static_cast<int>(t.why)];
    line += " " + std::to_string(t.tries) + ": t" + std::to_string(p.file.pathHash - 0x1000);
    if (t.hasSlot) line += " seq " + std::to_string(t.slot.seq);
    lines.push_back(line);
  }
  // written(): what it was told (the slot as the card has it, the play
  // still the ring's oldest: `tracker`), and the live fold (`folds`).
  const pl::Tracker* tracker = nullptr;
  MemFolds* folds = nullptr;
  std::vector<Slot> wrote;
  std::vector<std::string> wroteLines;
  void written(const Slot& s, const pl::Played& p) override {
    wrote.push_back(s);
    std::string line = "seq " + std::to_string(s.seq) + ": t" + std::to_string(p.file.pathHash - 0x1000);
    if (tracker) line += &tracker->oldest() == &p ? ", the oldest of " + std::to_string(tracker->pending()) : ", gone";
    wroteLines.push_back(line);
    if (folds) folds->fold(s);
  }
};

// A device with plays waiting: the journal over a card that fails on
// order, the Tracker, the Outbox.
struct Box {
  CutFs card;
  FaultFs fs{card};
  Device d;
  Env tenv;
  pl::Tracker tracker{tenv};
  OutEnv env;
  std::unique_ptr<pl::Outbox> out;
  explicit Box(uint32_t cap = pl::kFileSlots, MemFolds* folds = nullptr) : d(kDevA, cap) {
    d.folds = folds;
    env.folds = folds;
    env.tracker = &tracker;
    TEST_ASSERT_TRUE(d.boot(fs));
    out.reset(new pl::Outbox(tracker, *d.j, env));
  }
  // A play of track `track`, 2 s of it: waiting for its slot.
  void played(uint32_t track) {
    tracker.began(1, track, 0);
    for (uint32_t k = 1; k <= 100; ++k) {
      tenv.now += 20;
      tracker.tick(PlayState::Playing, k * 20, true, 180000);
    }
    tracker.ended(End::Completed);
  }
  std::vector<Slot> slots() { return slotsOf(card.bytes(kLog)); }
};

using Did = pl::Outbox::Did;
using Why = pl::Outbox::Why;
using OutFile = pl::Outbox::Env::File;

}  // namespace

// A play the card fails stays first in line and is tried again, the same
// slot, three tries in all; nothing behind it goes before it.
void test_the_outbox_tries_again() {
  // The played file's f_stat fails once (a read on the way to it): kept,
  // no seq taken; 5 s later, written. The play behind it after it.
  {
    Box b;
    b.played(5);
    b.played(6);
    TEST_ASSERT_TRUE(b.out->due());
    b.env.stats = {OutFile::Failed};
    pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Kept && t.why == Why::Stat);
    TEST_ASSERT_EQUAL_UINT8(1, t.tries);
    TEST_ASSERT_FALSE(t.hasSlot);
    TEST_ASSERT_EQUAL_UINT32(2, b.tracker.pending());
    TEST_ASSERT_EQUAL_UINT32(0, b.d.nvs.seqWrites);
    TEST_ASSERT_TRUE(b.card.dir.count(kLog) == 0);
    TEST_ASSERT_FALSE(b.out->due());
    b.env.now += pl::Outbox::kRetryMs - 1;
    TEST_ASSERT_FALSE(b.out->due());
    b.env.now += 1;
    TEST_ASSERT_TRUE(b.out->due());
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT8(2, t.tries);
    TEST_ASSERT_EQUAL_UINT32(1, t.slot.seq);
    TEST_ASSERT_TRUE(b.out->due());
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT8(1, t.tries);
    const std::vector<Slot> s = b.slots();
    TEST_ASSERT_EQUAL_size_t(2, s.size());
    TEST_ASSERT_EQUAL_UINT64(0x1005, s[0].pathHash);
    TEST_ASSERT_EQUAL_UINT32(1, s[0].seq);
    TEST_ASSERT_EQUAL_UINT64(0x1006, s[1].pathHash);
    TEST_ASSERT_EQUAL_UINT32(2, s[1].seq);
    TEST_ASSERT_EQUAL_UINT32(1000, s[0].size);
    TEST_ASSERT_EQUAL_UINT8(pl::kClockRtc, s[0].clock);
    TEST_ASSERT_EQUAL_UINT32(2000, s[0].playedMs);
    const std::vector<std::string> lines = {"kept stat 1: t5", "written 2: t5 seq 1", "written 1: t6 seq 2"};
    TEST_ASSERT_TRUE(b.env.lines == lines);
    TEST_ASSERT_EQUAL_UINT32(2, b.out->counts().written);
    TEST_ASSERT_EQUAL_UINT32(1, b.out->counts().retries);
    TEST_ASSERT_EQUAL_UINT32(0, b.out->counts().dropped);
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Nothing);
    // The clock unset at the write: no start, and the slot's clock byte
    // says so whatever the clock's source reads.
    b.env.wall = 0;
    b.played(7);
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    TEST_ASSERT_EQUAL_UINT64(0, b.slots()[2].startedAt);
    TEST_ASSERT_EQUAL_UINT8(pl::kClockUnset, b.slots()[2].clock);
  }
  // The card refuses the slot's write once: kept, its seq taken; tried
  // again, one slot, that seq, and the bytes of the first try (a clock set
  // between the two doesn't date it again).
  {
    Box b;
    b.played(5);
    b.played(6);
    b.card.refuse = true;
    pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Kept && t.why == Why::Append);
    TEST_ASSERT_TRUE(t.hasSlot);
    TEST_ASSERT_EQUAL_UINT32(1, t.slot.seq);
    const Slot first = t.slot;
    TEST_ASSERT_EQUAL_UINT32(1, b.d.nvs.seqWrites);
    TEST_ASSERT_EQUAL_UINT32(2, b.tracker.pending());
    b.card.refuse = false;
    b.env.wall += 3600000;  // a @time between the tries
    b.env.now += pl::Outbox::kRetryMs;
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT8(2, t.tries);
    TEST_ASSERT_EQUAL_UINT32(1, t.slot.seq);
    TEST_ASSERT_EQUAL_UINT32(1, b.d.nvs.seqWrites);  // (no second seq)
    TEST_ASSERT_EQUAL_UINT32(1, b.env.statCalls);    // (nor a second look at the file)
    uint8_t want[pl::kSlot];
    pl::encode(first, want);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, b.card.bytes(kLog).size());
    TEST_ASSERT_EQUAL_MEMORY(want, b.card.bytes(kLog).data(), pl::kSlot);
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(2, b.slots()[1].seq);
    TEST_ASSERT_EQUAL_UINT64(first.startedAt + 3600000 + 2000, b.slots()[1].startedAt);  // (the next play's: the clock now)
  }
  // The write landed and its sync failed: the try after it finds the slot,
  // one on the card.
  {
    Box b;
    b.played(5);
    b.fs.noSync.fail = 1;
    b.fs.noClose.fail = 1;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, b.card.bytes(kLog).size());
    b.env.now += pl::Outbox::kRetryMs;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, b.card.bytes(kLog).size());
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, b.card.reboot(cutfs::Variant::LoseUnsynced).bytes(kLog).size());
  }
  // Three failures: dropped, counted lost; the play behind it gets the next
  // seq (the lost one's is never on the card, and never used again).
  {
    Box b;
    b.played(5);
    b.played(6);
    b.card.refuse = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    b.env.now += pl::Outbox::kRetryMs;
    pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Kept);
    TEST_ASSERT_EQUAL_UINT8(2, t.tries);
    TEST_ASSERT_EQUAL_UINT8(2, b.out->tries());
    TEST_ASSERT_FALSE(b.out->due());
    b.env.now += pl::Outbox::kRetryMs;
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.gaveUp && t.why == Why::Append);
    TEST_ASSERT_EQUAL_UINT8(3, t.tries);
    TEST_ASSERT_EQUAL_UINT32(1, b.tracker.pending());
    TEST_ASSERT_EQUAL_UINT32(1, b.out->counts().lost);
    TEST_ASSERT_EQUAL_UINT32(1, b.out->counts().dropped);
    TEST_ASSERT_EQUAL_UINT8(0, b.out->tries());
    TEST_ASSERT_TRUE(b.out->due());  // (the next play's turn, at once)
    b.card.refuse = false;
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(2, t.slot.seq);
    const std::vector<Slot> s = b.slots();
    TEST_ASSERT_EQUAL_size_t(1, s.size());
    TEST_ASSERT_EQUAL_UINT64(0x1006, s[0].pathHash);
    const std::vector<std::string> lines = {"kept append 1: t5 seq 1", "kept append 2: t5 seq 1",
                                            "lost append 3: t5 seq 1", "written 1: t6 seq 2"};
    TEST_ASSERT_TRUE(b.env.lines == lines);
  }
  // ... the f_stat's three: lost with no seq taken (the next play's is 1).
  // A try of each kind counts.
  {
    Box b;
    b.played(5);
    b.played(6);
    b.env.stats = {OutFile::Failed, OutFile::Failed, OutFile::Failed};
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    b.env.now += pl::Outbox::kRetryMs;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    b.env.now += pl::Outbox::kRetryMs;
    const pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.gaveUp && t.why == Why::Stat && !t.hasSlot);
    TEST_ASSERT_EQUAL_UINT32(0, b.d.nvs.seqWrites);
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(1, b.slots()[0].seq);
    Box c;
    c.played(5);
    c.env.stats = {OutFile::Failed};
    TEST_ASSERT_TRUE(c.out->writeOne().did == Did::Kept);  // the f_stat
    c.card.refuse = true;
    c.env.now += pl::Outbox::kRetryMs;
    TEST_ASSERT_TRUE(c.out->writeOne().did == Did::Kept);  // the append
    c.env.now += pl::Outbox::kRetryMs;
    const pl::Outbox::Tried u = c.out->writeOne();  // the same slot again
    TEST_ASSERT_TRUE(u.did == Did::Dropped && u.gaveUp && u.why == Why::Append);
    TEST_ASSERT_EQUAL_UINT32(1, c.d.nvs.seqWrites);
  }
  // Nothing behind a play that waits goes before it: whoever asks in its
  // 5 s (the flush does) tries the head, and the card's order is the
  // plays'.
  {
    Box b;
    for (uint32_t t = 1; t <= 4; ++t) b.played(t);
    b.card.refuse = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    TEST_ASSERT_EQUAL_UINT64(0x1001, b.tracker.oldest().file.pathHash);
    TEST_ASSERT_FALSE(b.out->due());
    b.card.refuse = false;
    while (b.tracker.pending()) {
      TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    }
    const std::vector<Slot> s = b.slots();
    TEST_ASSERT_EQUAL_size_t(4, s.size());
    for (uint32_t i = 0; i < 4; ++i) {
      TEST_ASSERT_EQUAL_UINT64(0x1001 + i, s[i].pathHash);
      TEST_ASSERT_EQUAL_UINT32(i + 1, s[i].seq);
    }
  }
  // What another try wouldn't change is dropped at once, as before: the
  // file not on the card any more, NVS refusing the seq, a blocked log, no
  // log.
  {
    Box b;
    b.played(5);
    b.played(6);
    b.played(7);
    b.env.stats = {OutFile::Gone};
    pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.why == Why::Gone && !t.gaveUp);
    TEST_ASSERT_EQUAL_UINT32(2, b.tracker.pending());
    TEST_ASSERT_TRUE(b.out->due());
    b.d.nvs.failSeq = true;
    t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.why == Why::Nvs && !t.hasSlot);
    b.d.nvs.failSeq = false;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(0, b.out->counts().lost);
    TEST_ASSERT_EQUAL_UINT32(2, b.out->counts().dropped);
    // No log this boot (NVS unreadable at the boot).
    CutFs card;
    Device d;
    d.nvs.failLoad = true;
    TEST_ASSERT_FALSE(d.boot(card));
    Env tenv;
    pl::Tracker tracker(tenv);
    OutEnv env;
    pl::Outbox out(tracker, *d.j, env);
    tracker.began(1, 5, 0);
    for (uint32_t k = 1; k <= 100; ++k) {
      tenv.now += 20;
      tracker.tick(PlayState::Playing, k * 20, true, 180000);
    }
    tracker.ended(End::Completed);
    t = out.writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.why == Why::Off);
    // All nine twin names taken: blocked.
    Box blocked(4);
    for (int i = 0; i < 4; ++i) {
      blocked.played(1);
      TEST_ASSERT_TRUE(blocked.out->writeOne().did == Did::Written);
    }
    blocked.card.link(kLog, kPrev);
    for (int i = 1; i <= 9; ++i) blocked.card.put("/.player/plays.xl" + std::to_string(i), Bytes(pl::kSlot, 0x11));
    blocked.d.j->remounted();
    blocked.played(2);
    t = blocked.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Dropped && t.why == Why::Blocked && !t.gaveUp);
  }
}

// The card mounted again while a play waits: tried at once, not in its 5 s.
// Before this boot's first slot the boot's scan runs again first (what it
// read may be partial), and the slot's seq and boot count are the scan's.
void test_the_outbox_after_a_remount() {
  // A slot whose seq was taken: the same one, after the mount.
  {
    Box b;
    b.played(5);
    b.card.dead = true;  // pulled
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    TEST_ASSERT_FALSE(b.out->due());
    b.card.dead = false;  // put back: the guard's remount
    b.d.j->remounted();
    b.out->remounted();
    TEST_ASSERT_TRUE(b.d.j->ready());
    TEST_ASSERT_TRUE(b.out->due());
    const pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(1, t.slot.seq);
    TEST_ASSERT_EQUAL_UINT32(1, b.d.nvs.seqWrites);
    TEST_ASSERT_EQUAL_size_t(pl::kSlot, b.card.bytes(kLog).size());
    // A remount with nothing waiting changes nothing for the next play.
    b.out->remounted();
    b.played(6);
    b.card.refuse = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    TEST_ASSERT_FALSE(b.out->due());  // (that remount was before its try)
  }
  // The f_stat failed with the card out, before the boot's first slot, NVS
  // behind the card (an older NVS put back: the scan's seqs count): the
  // scan again, then the slot, its seq after the card's.
  {
    Box b;
    for (uint32_t t = 1; t <= 7; ++t) {
      b.played(t);
      TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    }
    b.d.nvs.ids.seq = 0;
    TEST_ASSERT_TRUE(b.d.boot(b.fs));  // the next boot
    b.out.reset(new pl::Outbox(b.tracker, *b.d.j, b.env));
    TEST_ASSERT_EQUAL_UINT32(7, b.d.j->lastSeq());
    TEST_ASSERT_EQUAL_UINT32(2, b.d.j->bootSeq());
    b.played(8);
    b.env.stats.assign(b.env.statCalls, OutFile::There);
    b.env.stats.push_back(OutFile::Failed);
    b.card.dead = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    b.card.dead = false;
    b.d.j->remounted();
    b.out->remounted();
    TEST_ASSERT_FALSE(b.d.j->ready());  // the scan again
    TEST_ASSERT_EQUAL_UINT32(1, b.d.j->booted().restarts);
    for (int guard = 0; guard < 1000 && !b.d.j->scan(5); ++guard) {
    }
    TEST_ASSERT_TRUE(b.d.j->ready());
    TEST_ASSERT_TRUE(b.out->due());
    const pl::Outbox::Tried t = b.out->writeOne();
    TEST_ASSERT_TRUE(t.did == Did::Written);
    TEST_ASSERT_EQUAL_UINT32(8, t.slot.seq);
    TEST_ASSERT_EQUAL_UINT32(2, t.slot.bootSeq);
    TEST_ASSERT_EQUAL_size_t(8, b.slots().size());
  }
}

// The orderly end's flush: each play once, in order; over at the first the
// card fails (the rest not asked) and at its deadline.
void test_the_outbox_flush() {
  using Stop = pl::Outbox::Flushed::Stop;
  // A healthy card: all of them.
  {
    Box b;
    for (uint32_t t = 1; t <= 5; ++t) b.played(t);
    const pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(5, f.waiting);
    TEST_ASSERT_EQUAL_UINT32(5, f.written);
    TEST_ASSERT_EQUAL_UINT32(0, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
    TEST_ASSERT_EQUAL_size_t(5, b.slots().size());
    const pl::Outbox::Flushed none = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(0, none.waiting);
    TEST_ASSERT_TRUE(none.stop == Stop::Done);
  }
  // The ring full (32), a rotation among them: all written.
  {
    Box b(20);
    for (uint32_t t = 1; t <= pl::Tracker::kPending; ++t) b.played(t);
    TEST_ASSERT_EQUAL_UINT32(pl::Tracker::kPending, b.tracker.pending());
    const pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(pl::Tracker::kPending, f.written);
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
    TEST_ASSERT_EQUAL_size_t(20, slotsOf(b.card.bytes(kPrev)).size());
    TEST_ASSERT_EQUAL_size_t(12, b.slots().size());
  }
  // The card fails the third: two written, the third kept (tried once), the
  // two behind it not asked.
  {
    Box b;
    for (uint32_t t = 1; t <= 5; ++t) b.played(t);
    b.env.stats = {OutFile::There, OutFile::There, OutFile::Failed};
    const pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(2, f.written);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
    TEST_ASSERT_EQUAL_UINT32(3, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Card);
    TEST_ASSERT_EQUAL_UINT32(3, b.env.statCalls);
    TEST_ASSERT_EQUAL_size_t(2, b.slots().size());
  }
  // A play in its 5 s wait gets its try all the same; its third failure
  // loses it, and ends the flush too.
  {
    Box b;
    for (uint32_t t = 1; t <= 3; ++t) b.played(t);
    b.card.refuse = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    TEST_ASSERT_FALSE(b.out->due());
    b.card.refuse = false;
    pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(3, f.written);
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
    for (uint32_t t = 4; t <= 6; ++t) b.played(t);
    b.card.refuse = true;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    b.env.now += pl::Outbox::kRetryMs;
    TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
    f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(0, f.written);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL_UINT32(2, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Card);
    TEST_ASSERT_EQUAL_UINT32(1, b.out->counts().lost);
  }
  // What is dropped for good on the way doesn't end it.
  {
    Box b;
    for (uint32_t t = 1; t <= 3; ++t) b.played(t);
    b.env.stats = {OutFile::There, OutFile::Gone};
    const pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(2, f.written);
    TEST_ASSERT_EQUAL_UINT32(1, f.dropped);
    TEST_ASSERT_EQUAL_UINT32(0, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
  }
  // A card that answers, a second a play: the deadline, three written.
  {
    Box b;
    for (uint32_t t = 1; t <= 5; ++t) b.played(t);
    b.env.perStat = 1000;
    const uint64_t t0 = b.env.now;
    const pl::Outbox::Flushed f = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(3, f.written);
    TEST_ASSERT_EQUAL_UINT32(2, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Time);
    TEST_ASSERT_EQUAL_UINT64(3000, b.env.now - t0);
  }
  // The boot's scan not done (a restart right after the boot): what is left
  // of it first, then the plays; out of time in it, nothing is written and
  // no play is dropped for a log that isn't ready.
  {
    Box b(40);
    for (uint32_t t = 1; t <= 60; ++t) {
      b.played(t % 7 + 1);
      TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    }
    b.d.j.reset(new Journal(b.fs, b.d.nvs, b.d.config()));
    TEST_ASSERT_TRUE(b.d.j->begin(b.d.id, b.d.acks));
    b.out.reset(new pl::Outbox(b.tracker, *b.d.j, b.env));
    TEST_ASSERT_FALSE(b.d.j->ready());
    b.played(1);
    b.played(2);
    pl::Outbox::Flushed f = b.out->flush(3000, 8);
    TEST_ASSERT_TRUE(b.d.j->ready());
    TEST_ASSERT_EQUAL_UINT32(2, f.written);
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
    TEST_ASSERT_EQUAL_UINT32(62, b.d.j->lastSeq());
    b.d.j.reset(new Journal(b.fs, b.d.nvs, b.d.config()));
    TEST_ASSERT_TRUE(b.d.j->begin(b.d.id, b.d.acks));
    b.out.reset(new pl::Outbox(b.tracker, *b.d.j, b.env));
    b.played(3);
    b.env.perLook = 1000;  // a second a piece
    f = b.out->flush(3000, 8);
    TEST_ASSERT_FALSE(b.d.j->ready());
    TEST_ASSERT_EQUAL_UINT32(0, f.written);
    TEST_ASSERT_EQUAL_UINT32(0, f.dropped);
    TEST_ASSERT_EQUAL_UINT32(1, f.left);
    TEST_ASSERT_TRUE(f.stop == Stop::Time);
    TEST_ASSERT_EQUAL_UINT32(1, b.tracker.pending());
    // With nothing waiting the scan isn't the flush's to finish.
    b.tracker.pop();
    b.env.perLook = 0;
    f = b.out->flush(3000, 8);
    TEST_ASSERT_FALSE(b.d.j->ready());
    TEST_ASSERT_TRUE(f.stop == Stop::Done);
  }
}

// ---------------------------------------------------------------------------
// The listening stats' hooks (playlog::Folds; docs/LISTENING-STATS.md 5.5,
// 5.6), through a MemFolds.
// ---------------------------------------------------------------------------
namespace {

// A card kDevA wrote under a cap of 4 in one boot: plays.1.jnl seqs 1-4,
// plays.jnl 5-8, no drop note; and the NVS it left.
struct Written {
  CutFs card;
  Journal::Ids ids;
  Written() {
    Device w(kDevA, 4);
    TEST_ASSERT_TRUE(w.boot(card));
    for (uint32_t i = 1; i <= 8; ++i) {
      Slot s = play(i, 60000, 1000 * i);
      TEST_ASSERT_TRUE(w.j->append(s) == Journal::Wrote::Ok);
    }
    ids = w.nvs.ids;
  }
  // The card as the next boot finds it, a copy.
  CutFs copy() const { return card.reboot(cutfs::Variant::InOrder); }
};

// A boot of kDevA over `fs` with that NVS, `folds`, and ACKS up to
// `acked` (0: no ACKS); the scan in pieces of 3.
void bootWith(Device& d, tagstore::Fs& fs, const Written& w, MemFolds* folds, uint32_t acked) {
  d.nvs.ids = w.ids;
  d.folds = folds;
  if (acked) setAcks(d.acks, {ack(kDevA, w.ids.epoch, acked)});
  TEST_ASSERT_TRUE(d.boot(fs, 3));
}

// A card whose opens of one file to read fail (openRead(): the card's
// fault); its other calls as FaultFs's.
struct ScanFaults : FaultFs {
  using FaultFs::FaultFs;
  std::string openFails;
  tagstore::File* openRead(const char* p, bool* cardFailed) override {
    if (openFails == p) {
      *cardFailed = true;
      return nullptr;
    }
    return FaultFs::openRead(p, cardFailed);
  }
};

// A file whose records on the way to it are damaged (FatFs's FR_INT_ERR at
// its open): openRead() says no file, not the card's fault; look() says
// Unknown (it may well be there). `looks`: the look()s asked.
struct DamagedOpen : FaultFs {
  using FaultFs::FaultFs;
  std::string path;
  uint32_t looks = 0;
  tagstore::File* openRead(const char* p, bool* cardFailed) override {
    if (path == p) {
      *cardFailed = false;
      return nullptr;
    }
    return FaultFs::openRead(p, cardFailed);
  }
  Seen look(const char* p) override {
    ++looks;
    return path == p ? Seen::Unknown : FaultFs::look(p);
  }
};

std::vector<uint32_t> seqs(std::initializer_list<uint32_t> l) { return std::vector<uint32_t>(l); }

}  // namespace

// With folds the boot's scan reads on a new epoch with no ACKS: the old
// epoch's slots are folded (an NVS erase would leave them, and other
// players', never folded). The boot rules are the new epoch's still. A
// scan begun again (a remount) tells the folds so, and each slot comes
// once more; after Ready, before the first slot, the save runs again.
void test_a_new_epoch_is_read_with_folds() {
  const Written w;
  for (int withFolds = 0; withFolds < 2; ++withFolds) {
    CutFs fs = w.copy();
    MemFolds f;
    Device d;
    d.epochs.list = {0xF0E1D2C3B4A59687ull};  // (NVS erased: this is drawn)
    d.folds = withFolds ? &f : nullptr;
    TEST_ASSERT_TRUE(d.boot(fs));
    TEST_ASSERT_TRUE(d.j->newEpoch());
    TEST_ASSERT_EQUAL_UINT32(1, d.j->bootSeq());
    if (!withFolds) {
      TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().slots);  // (test_a_new_epoch: nothing read)
      continue;
    }
    TEST_ASSERT_EQUAL_UINT32(8, d.j->booted().slots);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE((f.files == std::vector<int>{0, 0, 0, 0, 1, 1, 1, 1}));  // plays.jnl first
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
    Slot s = play(9, 60000, 9000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(1, s.seq);  // the new epoch's
    TEST_ASSERT_EQUAL_UINT64(0xF0E1D2C3B4A59687ull, s.epoch);
  }
  // A remount in the scan: its folds dropped, the scan again from its
  // start, each slot folded once.
  CutFs fs = w.copy();
  MemFolds f;
  Device d;
  d.epochs.list = {0xF0E1D2C3B4A59687ull};
  d.folds = &f;
  d.j.reset(new Journal(fs, d.nvs, d.config()));
  TEST_ASSERT_TRUE(d.j->begin(d.id, d.acks));
  TEST_ASSERT_FALSE(d.j->scan(5));  // plays.jnl's four, plays.1.jnl's first
  TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1}));
  auto finish = [&] {
    d.j->remounted();
    for (int guard = 0; guard < 100 && !d.j->scan(5); ++guard) {
    }
    TEST_ASSERT_TRUE(d.j->ready());
  };
  finish();
  TEST_ASSERT_TRUE((f.calls == Calls{"began", "began again", "save boot"}));
  TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2, 3, 4}));
  // After Ready, before the first slot: the scan again, and its save
  // (nothing new: all under the card's MARK now).
  finish();
  TEST_ASSERT_TRUE((f.calls == Calls{"began", "began again", "save boot", "began again", "save boot"}));
  TEST_ASSERT_TRUE(f.folded.empty());
  TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().restarts);
}

// With folds a file goes only when the card's MARK covers it, so the boot's
// save comes first, both files still on the card; a save that fails trims
// what the MARK read at the boot covers (the next test). NVS refusing the
// boot count: no log, no trim, and the save all the same (the fold is
// whole).
void test_the_trim_waits_for_the_folds() {
  const Written w;
  {
    CutFs fs = w.copy();
    Device d;
    bootWith(d, fs, w, nullptr, 8);  // (no folds: today's trim)
    TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().trimmed);
  }
  {
    CutFs fs = w.copy();
    MemFolds f;
    f.on = &fs;
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE((f.seen == Calls{"plays.1.jnl plays.jnl"}));  // the save before the trim
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
    TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().trimmed);
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.1.jnl acknowledged", "delete plays.jnl acknowledged"}));
  }
  {
    // A fresh stats.bin (MARK none) that the card won't take: nothing goes.
    CutFs fs = w.copy();
    MemFolds f;
    f.failSave = true;
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)"}));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
    TEST_ASSERT_TRUE(fs.dir.count(kPrev) == 1 && fs.dir.count(kLog) == 1);
  }
  {
    CutFs fs = w.copy();
    MemFolds f;
    Device d;
    d.nvs.ids = w.ids;
    d.nvs.failBoots = true;
    d.folds = &f;
    setAcks(d.acks, {ack(kDevA, w.ids.epoch, 8)});
    TEST_ASSERT_FALSE(d.boot(fs));
    TEST_ASSERT_TRUE(d.j->off());
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
  }
  {
    // No ACKS: the save, no trim, as ever.
    CutFs fs = w.copy();
    MemFolds f;
    Device d;
    bootWith(d, fs, w, &f, 0);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
  }
}

// The card's MARK at 4 (an earlier boot saved seqs 1-4) and this boot's save
// failing (a full card): plays.1.jnl goes, which the MARK covers; plays.jnl
// waits. The next boot folds 5-8 again (the first's folds never reached the
// card) and, its save landing, trims it. Stats off (no MARK known: covered()
// says no, the save is refused): nothing goes.
void test_a_failed_save_trims_what_the_card_covers() {
  const Written w;
  const MemFolds::Key k = MemFolds::key(kDevA, w.ids.epoch);
  CutFs fs = w.copy();
  MemFolds f;
  f.card[k] = 4;
  f.failSave = true;
  {
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8}));
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.1.jnl acknowledged"}));
    TEST_ASSERT_TRUE(fs.dir.count(kPrev) == 0 && fs.dir.count(kLog) == 1);
    TEST_ASSERT_EQUAL_UINT32(4, f.cardMark(kDevA, w.ids.epoch));
  }
  f.failSave = false;
  f.calls.clear();
  {
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8}));
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.jnl acknowledged"}));
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
  }
  {
    struct Off : MemFolds {
      bool covered(const Slot&) override { return false; }
      bool save(Why) override { return false; }
    } off;
    CutFs fs2 = w.copy();
    Device d;
    bootWith(d, fs2, w, &off, 8);
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
  }
}

// A rotation takes plays.1.jnl out of the log's names (deleted, or renamed
// to a twin): when it holds a play the card's MARK doesn't cover, the folds
// are saved first, the file still on the card; the rotation goes on
// whatever the save comes to. After the boot's save the session's first
// rotations save nothing (the file is the one that save covered); a later
// one over this session's plays does. A plays.1.jnl not read whole (a
// scan that was) is saved over too; after a partial scan nothing is.
void test_a_rotation_saves_first() {
  const Written w;
  const uint64_t e = w.ids.epoch;
  auto add = [](Device& d, MemFolds& f, uint32_t seq) {
    Slot s = play(seq, 60000, 1000 * seq);
    const Journal::Wrote r = d.j->append(s);
    if (r == Journal::Wrote::Ok) f.fold(s);  // (the live fold: Outbox::Env::written())
    return r;
  };
  // After a boot whose save failed: 1-4 saved before plays.1.jnl goes.
  {
    CutFs fs = w.copy();
    MemFolds f;
    f.on = &fs;
    f.failSave = true;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    f.failSave = false;
    d.ops.clear();
    TEST_ASSERT_TRUE(add(d, f, 9) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)", "save rotation"}));
    TEST_ASSERT_TRUE((f.seen == Calls{"plays.1.jnl plays.jnl", "plays.1.jnl plays.jnl"}));
    TEST_ASSERT_TRUE(
        (d.ops == Calls{"delete plays.1.jnl rotation", "rename plays.jnl plays.1.jnl", "note 4", "play 9"}));
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, e));  // (9 isn't written yet at the save)
  }
  // The boot's save done: the rotations at 9 and 12 delete what it covers,
  // and save nothing; the one at 15 deletes 9-11, this session's: saved.
  {
    CutFs fs = w.copy();
    MemFolds f;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    for (uint32_t seq = 9; seq <= 14; ++seq) TEST_ASSERT_TRUE(add(d, f, seq) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE(add(d, f, 15) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot", "save rotation"}));
    TEST_ASSERT_EQUAL_UINT32(14, f.cardMark(kDevA, e));
    TEST_ASSERT_EQUAL_size_t(3, std::count(d.ops.begin(), d.ops.end(), std::string("delete plays.1.jnl rotation")));
  }
  // The save fails: the rotation goes on, and 1-4 are in no file the scan
  // reads and in no stats.bin (the undercount 5.5 names).
  {
    CutFs fs = w.copy();
    MemFolds f;
    f.failSave = true;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    d.ops.clear();
    TEST_ASSERT_TRUE(add(d, f, 9) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)", "save rotation (no)"}));
    TEST_ASSERT_EQUAL_STRING("delete plays.1.jnl rotation", d.ops.front().c_str());
    TEST_ASSERT_EQUAL_UINT32(0, f.cardMark(kDevA, e));
  }
  // A twin on plays.1.jnl's chain: the save before its rename to a twin.
  {
    CutFs fs = w.copy();
    fs.link(kPrev, "/.player/plays.xl1");
    MemFolds f;
    f.on = &fs;
    f.failSave = true;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    f.failSave = false;
    d.ops.clear();
    TEST_ASSERT_TRUE(add(d, f, 9) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)", "save rotation"}));
    TEST_ASSERT_EQUAL_STRING("plays.1.jnl plays.jnl", f.seen.back().c_str());
    TEST_ASSERT_TRUE(
        (d.ops == Calls{"twin plays.1.jnl plays.xl2", "rename plays.jnl plays.1.jnl", "note 4", "play 9"}));
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, e));
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
  // plays.1.jnl can't be read whole (its last slot), every play in it
  // covered: the rotation's first two tries leave the log as it was and
  // save nothing; the third goes on, saving first.
  {
    CutFs card = w.copy();
    FaultFs fs(card);
    MemFolds f;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    fs.badPath = kPrev;
    fs.badFrom = 3 * pl::kSlot;
    fs.badTo = 4 * pl::kSlot;
    fs.bad.fail = -1;
    Slot s = play(9, 60000, 9000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Failed);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE(d.j->again() == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot", "save rotation"}));
  }
  // After a partial scan (plays.1.jnl's open failed) no save at all: the
  // rotation deletes 1-4 unsaved.
  {
    CutFs card = w.copy();
    ScanFaults fs(card);
    fs.openFails = kPrev;
    MemFolds f;
    Device d(kDevA, 4);
    bootWith(d, fs, w, &f, 0);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
    d.ops.clear();
    TEST_ASSERT_TRUE(add(d, f, 9) == Journal::Wrote::Ok);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
    TEST_ASSERT_EQUAL_STRING("delete plays.1.jnl rotation", d.ops.front().c_str());
  }
}

// The orderly end's flush (app/PlayHistory: an "ending" flag, the 3 s
// budget), as the glue's rule in MemFolds::may: the boot's save never (the
// budget is the plays'; the next boot folds again), and a rotation's only
// with 1 s of the budget left. The journal asks both in the flush: the
// scan's end (the save, then the trim by what the card covers), and the
// rotation (the save before its delete).
void test_the_ends_flush_and_the_folds() {
  bool ending = false;
  // The boot's scan not done when the end comes: finished in the flush,
  // its save refused, so nothing goes that the card's MARK (none) doesn't
  // cover. The next boot folds every slot, saves and trims.
  {
    const Written w;
    MemFolds f;
    f.may = [&](pl::Folds::Why why) { return !(ending && why == pl::Folds::Why::Boot); };
    Box b;
    b.card = w.copy();
    b.d.nvs.ids = w.ids;
    b.d.folds = &f;
    b.env.folds = &f;
    setAcks(b.d.acks, {ack(kDevA, w.ids.epoch, 8)});
    b.d.j.reset(new Journal(b.fs, b.d.nvs, b.d.config()));
    TEST_ASSERT_TRUE(b.d.j->begin(b.d.id, b.d.acks));
    b.out.reset(new pl::Outbox(b.tracker, *b.d.j, b.env));
    b.played(1);
    b.played(2);
    ending = true;
    const pl::Outbox::Flushed fl = b.out->flush(3000, 2);
    TEST_ASSERT_TRUE(b.d.j->ready());
    TEST_ASSERT_EQUAL_UINT32(2, fl.written);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot (no)"}));
    TEST_ASSERT_EQUAL_UINT32(0, b.d.j->booted().trimmed);
    TEST_ASSERT_TRUE(b.card.dir.count(kPrev) == 1);
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2, 3, 4, 9, 10}));  // (the two live)
    ending = false;
    f.calls.clear();
    Device d;
    bootWith(d, b.card, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 9, 10, 1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT32(10, f.cardMark(kDevA, w.ids.epoch));
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.1.jnl acknowledged"}));  // (plays.jnl: 9 and 10 unacknowledged)
  }
  // A rotation in the flush over plays.1.jnl (1-4, this session's, the
  // card's MARK none): saved with 3 s left; with 0.5 s left (the played
  // file's question took 2.5 s) not, and 1-4 are lost to the stats.
  for (const uint64_t statMs : {0ull, 2500ull}) {
    ending = false;
    MemFolds f;
    Box b(4, &f);
    uint64_t until = 0;
    f.may = [&](pl::Folds::Why why) {
      if (!ending) return true;
      return why == pl::Folds::Why::Rotation && until >= b.env.now + 1000;
    };
    for (uint32_t t = 1; t <= 8; ++t) {
      b.played(t);
      TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
    }
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "save boot"}));  // (the first rotation: no plays.1.jnl)
    b.played(9);
    ending = true;
    b.env.perStat = statMs;
    until = b.env.now + 3000;
    b.d.ops.clear();
    const pl::Outbox::Flushed fl = b.out->flush(3000, 128);
    TEST_ASSERT_EQUAL_UINT32(1, fl.written);
    TEST_ASSERT_EQUAL_STRING("delete plays.1.jnl rotation", b.d.ops.front().c_str());
    TEST_ASSERT_EQUAL_STRING(statMs ? "save rotation (no)" : "save rotation", f.calls.back().c_str());
    TEST_ASSERT_EQUAL_UINT32(statMs ? 0 : 8, f.cardMark(kDevA, b.d.j->epoch()));
  }
}

// A log file the scan can't read whole (an open or a read the card failed)
// is partial(): no save this session; the trim by the MARK read at the boot
// keeps it (as ever: not judged). The next whole boot folds each slot once
// and saves. Without folds the scan is as it was (it counts them).
void test_a_partial_scan_calls_partial() {
  const Written w;
  const MemFolds::Key k = MemFolds::key(kDevA, w.ids.epoch);
  // plays.1.jnl's open fails while plays.jnl reads; the card's MARK at 4.
  {
    CutFs card = w.copy();
    ScanFaults fs(card);
    fs.openFails = kPrev;
    MemFolds f;
    f.card[k] = 4;
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
    TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().partial);
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8}));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);  // plays.jnl not covered, plays.1.jnl not read
    TEST_ASSERT_EQUAL_UINT32(4, f.cardMark(kDevA, w.ids.epoch));
  }
  // A read fault in plays.jnl's tail (its last slot), plays.1.jnl whole:
  // plays.1.jnl goes, which the MARK at the boot covers.
  {
    CutFs card = w.copy();
    FaultFs fs(card);
    fs.badPath = kLog;
    fs.badFrom = 3 * pl::kSlot;
    fs.badTo = 4 * pl::kSlot;
    fs.bad.fail = -1;
    MemFolds f;
    f.card[k] = 4;
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7}));
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.1.jnl acknowledged"}));
    TEST_ASSERT_TRUE(card.dir.count(kLog) == 1);
  }
  // A read fault midway through plays.1.jnl (its last slot), the card's
  // MARK none: nothing goes. Then a whole boot: each slot once, the save,
  // both trimmed.
  {
    CutFs card = w.copy();
    FaultFs fs(card);
    fs.badPath = kPrev;
    fs.badFrom = 3 * pl::kSlot;
    fs.badTo = 4 * pl::kSlot;
    fs.bad.fail = -1;
    MemFolds f;
    {
      Device d;
      bootWith(d, fs, w, &f, 8);
      TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
      TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2}));  // (the scan's read of slots 3-4 failed)
      TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);
    }
    fs.badPath.clear();
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial", "began", "save boot"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().partial);
    TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().trimmed);
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
  }
  // Without folds: plays.1.jnl's open failing is a missing file, as ever.
  {
    CutFs card = w.copy();
    ScanFaults fs(card);
    fs.openFails = kPrev;
    Device d;
    bootWith(d, fs, w, nullptr, 8);
    TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().partial);
    TEST_ASSERT_EQUAL_UINT32(4, d.j->booted().slots);
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.jnl acknowledged"}));
  }
}

// A log file whose open finds its records damaged (no file, the card not at
// fault: FatFs's FR_INT_ERR) is a file not read whole for the folds when
// look() can't say it isn't there (5.6): no save, the trim by the card's
// MARK, and the next boot (its records whole again: a disk check) folds it.
// A MARK saved over plays.jnl's 5-8 would pass plays.1.jnl's 1-4, never
// folded. Without folds it is a missing file, as ever, and no look() asked.
void test_a_damaged_log_file_is_not_read_whole() {
  const Written w;
  {
    CutFs card = w.copy();
    DamagedOpen fs(card);
    fs.path = kPrev;
    MemFolds f;
    {
      Device d;
      bootWith(d, fs, w, &f, 8);
      TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial"}));
      TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().partial);
      TEST_ASSERT_EQUAL_UINT32(1, d.j->booted().files);
      TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8}));
      TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().trimmed);  // plays.jnl not covered, plays.1.jnl not read
      TEST_ASSERT_EQUAL_UINT32(0, f.cardMark(kDevA, w.ids.epoch));
      TEST_ASSERT_TRUE(card.dir.count(kPrev) == 1 && card.dir.count(kLog) == 1);
      // (One look() for each log name the open found nothing at: the other
      // eight are Gone.)
      TEST_ASSERT_EQUAL_UINT32(pl::kNames - 1, fs.looks);
    }
    fs.path.clear();
    Device d;
    bootWith(d, fs, w, &f, 8);
    TEST_ASSERT_TRUE((f.calls == Calls{"began", "partial", "began", "save boot"}));
    TEST_ASSERT_TRUE(f.folded == seqs({5, 6, 7, 8, 1, 2, 3, 4}));
    TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().trimmed);
    TEST_ASSERT_EQUAL_UINT32(8, f.cardMark(kDevA, w.ids.epoch));
  }
  // Without folds: a missing file, as ever, and the card not asked again.
  {
    CutFs card = w.copy();
    DamagedOpen fs(card);
    fs.path = kPrev;
    Device d;
    bootWith(d, fs, w, nullptr, 8);
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().partial);
    TEST_ASSERT_EQUAL_UINT32(4, d.j->booted().slots);
    TEST_ASSERT_EQUAL_UINT32(0, fs.looks);
    TEST_ASSERT_TRUE((d.ops == Calls{"delete plays.jnl acknowledged"}));
  }
}

// Outbox::Env::written(): a Written play only, after tried() and before
// the Tracker lets it go, with its slot as the card has it (a retry's, the
// first try's); never a play kept or dropped.
void test_the_outbox_says_written() {
  Box b;
  for (uint32_t t = 5; t <= 7; ++t) b.played(t);
  b.env.stats = {OutFile::Failed, OutFile::There, OutFile::There, OutFile::Gone};
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);  // (no seq taken)
  TEST_ASSERT_TRUE(b.env.wrote.empty());
  b.env.now += pl::Outbox::kRetryMs;
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Dropped);
  // A slot the card failed, tried again: written() once, at the try that
  // landed.
  b.played(8);
  b.card.refuse = true;
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Kept);
  b.card.refuse = false;
  b.env.now += pl::Outbox::kRetryMs;
  TEST_ASSERT_TRUE(b.out->writeOne().did == Did::Written);
  const Calls want = {"seq 1: t5, the oldest of 3", "seq 2: t6, the oldest of 2", "seq 3: t8, the oldest of 1"};
  TEST_ASSERT_TRUE(b.env.wroteLines == want);
  const std::vector<Slot> card = b.slots();
  TEST_ASSERT_EQUAL_size_t(3, card.size());
  for (size_t i = 0; i < card.size(); ++i) {
    uint8_t told[pl::kSlot], have[pl::kSlot];
    pl::encode(b.env.wrote[i], told);
    pl::encode(card[i], have);
    TEST_ASSERT_EQUAL_MEMORY(have, told, pl::kSlot);
  }
  TEST_ASSERT_EQUAL_UINT32(3, b.out->counts().written);
}

// The seq rule's two reservations (2.20.4; docs/LISTENING-STATS.md 5.6),
// with NVS put back from a backup behind a log already trimmed (the
// house's own end-of-check step): ACKS's seq for this device and epoch,
// and with folds their MARK. A new play takes neither a seq ACKS
// acknowledges (read as sent, it would never be sent) nor one MARK covers
// (never folded). Another device's rows and another epoch's don't count.
void test_the_seq_rules_two_sources() {
  const Written w;
  const uint64_t e = w.ids.epoch;
  Journal::Ids back = w.ids;
  back.seq = 2;
  // ACKS: a sync acknowledged 1-8, and the boot trimmed both files.
  CutFs fs = w.copy();
  {
    Device d;
    bootWith(d, fs, w, nullptr, 8);
    TEST_ASSERT_EQUAL_UINT32(2, d.j->booted().trimmed);
  }
  {
    Device d;
    d.nvs.ids = back;
    setAcks(d.acks, {ack(kDevA, e, 8), ack(kDevA, e + 1, 100), ack(kDevB, e, 100)});
    TEST_ASSERT_TRUE(d.boot(fs));
    TEST_ASSERT_EQUAL_UINT32(0, d.j->booted().cardSeq);
    TEST_ASSERT_EQUAL_UINT32(8, d.j->lastSeq());
    Slot s = play(9, 60000, 9000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(9, s.seq);  // (NVS's and the log's alone: 3)
    TEST_ASSERT_EQUAL_UINT32(9, d.nvs.ids.seq);
  }
  // MARK: the stats hold 1-8 (folded before a rotation deleted 5-8), an
  // older sync's ACKS 4, the log empty: 9 with the folds, 5 without.
  for (int withFolds = 0; withFolds < 2; ++withFolds) {
    CutFs empty;
    MemFolds f;
    f.card[MemFolds::key(kDevA, e)] = 8;
    f.card[MemFolds::key(kDevA, e + 1)] = 100;
    f.card[MemFolds::key(kDevB, e)] = 100;
    Device d;
    d.nvs.ids = back;
    d.folds = withFolds ? &f : nullptr;
    setAcks(d.acks, {ack(kDevA, e, 4)});
    TEST_ASSERT_TRUE(d.boot(empty));
    Slot s = play(9, 60000, 9000);
    TEST_ASSERT_TRUE(d.j->append(s) == Journal::Wrote::Ok);
    TEST_ASSERT_EQUAL_UINT32(withFolds ? 9 : 5, s.seq);
  }
}

// device.txt's history (2.15): the vectors' texts read as the majors they
// list; no key, an empty one, or no file: none. This firmware writes 1.
void test_device_txt_says_history() {
  namespace dt = cc::devicetxt;
  const Value& cases = gVectors["plays"]["deviceTxt"];
  TEST_ASSERT_EQUAL_size_t(5, cases.size());
  for (size_t i = 0; i < cases.size(); ++i) {
    dt::Info info = dt::defaults();
    if (!cases[i]["text"].isNull()) {
      const std::string& t = cases[i]["text"].str();
      TEST_ASSERT_TRUE(dt::parse(t.data(), t.size(), &info));
    }
    uint32_t want = 0;
    for (size_t k = 0; k < cases[i]["history"].size(); ++k) want |= 1u << (cases[i]["history"][k].u64() - 1);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, info.history, cases[i]["text"].c_str());
  }
  TEST_ASSERT_TRUE(dt::readsMajor(dt::current("0.12.0").history, pl::kMajor));
}

int main(int, char**) {
  if (!cardfixtures::json("vectors.json", &gVectors) || !cardfixtures::json("plays/log.json", &gScript) ||
      !cardfixtures::json("plays/index.json", &gIndex)) {
    printf("the card fixtures (test/fixtures/card) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_the_slot_and_the_drop_note_byte_for_byte);
  RUN_TEST(test_the_verdicts_on_the_readers_cases);
  RUN_TEST(test_the_goldens_from_the_script);
  RUN_TEST(test_the_boot_rules);
  RUN_TEST(test_a_new_epoch);
  RUN_TEST(test_nvs_first_and_the_last_seq);
  RUN_TEST(test_a_tail_is_cut_off);
  RUN_TEST(test_acks_read_on_their_own);
  RUN_TEST(test_acks_keep_this_boards_rows_first);
  RUN_TEST(test_the_trims_rules);
  RUN_TEST(test_a_cut_at_every_step);
  RUN_TEST(test_twins_and_the_blocked_log);
  RUN_TEST(test_a_refused_rename_is_tried_again);
  RUN_TEST(test_an_open_the_card_fails_never_empties_the_log);
  RUN_TEST(test_a_question_the_card_fails_frees_no_chain);
  RUN_TEST(test_a_failed_slot_is_tried_again);
  RUN_TEST(test_a_failed_sync_on_the_fatfs_card);
  RUN_TEST(test_a_rotation_tried_again);
  RUN_TEST(test_a_plays1_that_cant_be_read);
  RUN_TEST(test_the_outbox_tries_again);
  RUN_TEST(test_the_outbox_after_a_remount);
  RUN_TEST(test_the_outbox_flush);
  RUN_TEST(test_the_drop_note);
  RUN_TEST(test_what_a_rotation_counts);
  RUN_TEST(test_the_tracker);
  RUN_TEST(test_a_seek_after_a_pause_is_not_played);
  RUN_TEST(test_short_tracks_wait_for_the_gate);
  RUN_TEST(test_the_write_gate);
  RUN_TEST(test_the_write_gate_waits_for_a_moving_list);
  RUN_TEST(test_what_the_card_is_asked);
  RUN_TEST(test_a_tail_on_the_fatfs_card);
  RUN_TEST(test_a_remount_and_the_boots_scan);
  RUN_TEST(test_device_txt_says_history);
  RUN_TEST(test_a_new_epoch_is_read_with_folds);
  RUN_TEST(test_the_trim_waits_for_the_folds);
  RUN_TEST(test_a_failed_save_trims_what_the_card_covers);
  RUN_TEST(test_a_rotation_saves_first);
  RUN_TEST(test_the_ends_flush_and_the_folds);
  RUN_TEST(test_a_partial_scan_calls_partial);
  RUN_TEST(test_a_damaged_log_file_is_not_read_whole);
  RUN_TEST(test_the_outbox_says_written);
  RUN_TEST(test_the_seq_rules_two_sources);
  return UNITY_END();
}
