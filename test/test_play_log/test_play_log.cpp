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
//   epoch, another device, a conflict's two slots) and the boot's scan in
//   pieces of any size;
// - ACKS (2.5.6) read on its own: the golden root's entries, the
//   hardening files (damaged, out of order, short rows: absent, the root
//   present; a newer minor's longer rows read by their prefix);
// - the trim's rules, a cut append's tail, NVS refusing the seq, the seq's
//   last value taking a new epoch;
// - a power cut at every step of appends and two rotations, the card as
//   the next boot finds it three ways (CutFs): no chain freed that another
//   name uses, no slot credited wrongly (every valid slot is one the
//   journal wrote, byte for byte), no seq used twice, the log going on;
//   the twins a cut rename leaves, collected after a disk check; all nine
//   twin names taken: blocked;
// - the Tracker (the time played, pauses excluded, a seek's jump not
//   counted, the tail of a completed play, short plays, the queue) and the
//   WriteGate (the gapless window, a start's settle, the ring, underruns);
// - what an append, a rotation and the boot's scan ask of the card, on the
//   host FatFs model (ChaN's FatFs as the Core2 builds it, the sector
//   cache in front: test_card_io's card): bytes per play and the cap's
//   worst case.
// Run: pio test -e native -f test_play_log
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "../support/FatModelFs.h"
#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
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
  TEST_ASSERT_EQUAL_size_t(5, at["cases"].size());
  for (size_t c = 0; c < at["cases"].size(); ++c) {
    const Value& e = at["cases"][c];
    CutFs fs;
    if (e.has("readCase")) {
      bool found = false;
      for (size_t k = 0; k < gIndex.size(); ++k) {
        if (gIndex[k]["case"].str() != e["readCase"].str()) continue;
        for (const auto& kv : gIndex[k]["files"].members) fs.put("/.player/" + kv.first, fixture("plays/" + kv.second.str()));
        found = true;
      }
      TEST_ASSERT_TRUE(found);
    } else {
      for (const char* n : {"plays.jnl", "plays.1.jnl"})
        fs.put(std::string("/.player/") + n, fixture(at["log"].str() + "/" + n));
    }
    uint8_t id[6];
    deviceOf(e["deviceId"], id);
    for (uint32_t piece : {1u, 3u, 1000u}) {  // the scan in any pieces
      Device d(id);
      d.nvs.ids.epoch = e["epoch"].u64();
      d.nvs.ids.seq = static_cast<uint32_t>(e["nvsSeq"].u64());
      d.nvs.ids.boots = static_cast<uint32_t>(e["nvsBoots"].u64());
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
      // (Put the card back as it was for the next piece size.)
      fs.put(kLog, fixture(e.has("readCase") ? std::string("plays/read/conflict-top/plays.jnl")
                                             : at["log"].str() + "/plays.jnl"));
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
  RUN_TEST(test_the_trims_rules);
  RUN_TEST(test_a_cut_at_every_step);
  RUN_TEST(test_twins_and_the_blocked_log);
  RUN_TEST(test_the_drop_note);
  RUN_TEST(test_what_a_rotation_counts);
  RUN_TEST(test_the_tracker);
  RUN_TEST(test_the_write_gate);
  RUN_TEST(test_what_the_card_is_asked);
  RUN_TEST(test_device_txt_says_history);
  return UNITY_END();
}
