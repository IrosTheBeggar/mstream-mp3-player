// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for N5's walk through N4's store (lib/core/TagStoreWalk): the
// real CardWalk on a fake FAT tree (test/support/FakeFat.h), reading D
// through KnownD and writing walk.jnl through WalkSink, compacted into
// tags.bin: a first walk, an unchanged card that writes nothing, a card
// changed on a PC, a transfer whose every stamp a PC shifted an hour (the
// doubts written, read back and settled by the skew, which DHDR keeps), N2's
// builder over the result, and a power cut at every step of that walk and
// its compaction, after which the next boot's walk reaches the same D; a
// doubt at the same commit settled by its qfp, cut at every step of its walk
// or its read failing, asked again by the next boot's walk.
// Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../support/CutFs.h"
#include "../support/FakeFat.h"
#include "CardContract.h"
#include "CardTags.h"
#include "CardWalk.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "TagStore.h"
#include "TagStoreWalk.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace cw = cardwalk;
namespace ts = tagstore;
using cutfs::CutFs;
using cutfs::Variant;
using ts::Status;

namespace {

constexpr uint32_t kT = 0x5D4773D5;  // 2026-10-07 14:30:42
uint32_t shifted(uint32_t t, int32_t s) { return cc::fatTimeFromWall(cc::fatWallSeconds(t) + s); }

bool isAudio(const std::string& rel) {
  const std::string l = fakefat::leafOf(rel);
  return LibraryIndex::formatOf(l.data(), l.size()) != LibraryIndex::Format::Unknown;
}

ts::TagStore::Config config() {
  ts::TagStore::Config c;
  c.producer = "mstream-player 0.8.0";
  c.parserVersion = 3;
  return c;
}

ts::Identity commit(uint32_t gen) {
  ts::Identity id;
  id.present = true;
  id.cardId = 0x5EEDC0DE0A1B2C3Dull;
  id.generation = gen;
  id.commitId = 0xC0FFEE00ull + gen;
  id.tagsCrc = 0xABCD0000u + gen;
  return id;
}

// The card's audio: 12 files in the user's shape, covers, a text file, a
// folder of images alone.
void fill(fakefat::Card& c) {
  uint32_t seed = 10;
  for (const char* a : {"Artist/Album", "Artist/Album 2/CD1", "Band/Live", "Other"}) {
    for (int k = 1; k <= 3; ++k) {
      char rel[96];
      std::snprintf(rel, sizeof(rel), "%s/%02d - Song.mp3", a, k);
      c.addFile(rel, 3000000 + seed * 1000, kT, seed);
      ++seed;
    }
  }
  c.addFile("Artist/Album/cover.jpg", 40000, kT, 3);
  c.addFile("Artist/Album/notes.txt", 100, kT, 4);
  c.addFile("Band/Live/folder.jpg", 50000, kT, 5);
  c.addFile("Scans/back.jpg", 60000, kT, 6);
}

// T: the card's files as a transfer recorded them, every stamp `shift`
// seconds behind the card's (a PC's time zone).
std::vector<uint8_t> transferOf(const fakefat::Card& c, int32_t shift) {
  std::vector<std::string> rels;
  for (const auto& kv : c.files)
    if (kv.first.rfind("Scans/", 0) != 0) rels.push_back(kv.first);
  std::vector<mptg::RecordIn> in(rels.size());
  for (size_t i = 0; i < rels.size(); ++i) {
    const fakefat::File& f = c.files.at(rels[i]);
    const bool audio = isAudio(rels[i]);
    in[i].path = rels[i].c_str();
    in[i].rec.size = f.size;
    in[i].rec.fatTime = shifted(f.fatTime, -shift);
    in[i].rec.qfp = c.qfpOf(rels[i]);
    in[i].rec.container = audio ? mptg::kContainerMp3 : mptg::kContainerNotAudio;
    in[i].rec.known = audio ? mptg::kKnownRules1 : 0;
    in[i].fields[cc::kTitle] = audio ? "T" : nullptr;
  }
  mptg::Meta meta;
  meta.generation = 7;
  meta.cardId = 0x5EEDC0DE0A1B2C3Dull;
  meta.source = mptg::kSourceTransfer;
  meta.producer = "mstream-terminal 0.13.0";
  struct V : cc::Sink {
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      if (offset + n > bytes.size()) bytes.resize(offset + n);
      if (n) std::memcpy(bytes.data() + offset, data, n);
      return true;
    }
    std::vector<uint8_t> bytes;
  } out;
  const char* error = nullptr;
  if (!mptg::write(out, meta, in.data(), in.size(), nullptr, 0, nullptr, 0, nullptr, &error)) TEST_FAIL_MESSAGE(error);
  return out.bytes;
}

// The buffers a walk takes (PSRAM on the device).
struct Buffers {
  std::vector<uint8_t> scratch = std::vector<uint8_t>(cw::CardWalk::kDeviceScratch);
  std::vector<uint8_t> known = std::vector<uint8_t>(4096);
  std::vector<uint8_t> journal = std::vector<uint8_t>(4096);
  std::vector<uint8_t> doubts = std::vector<uint8_t>(512);
  std::vector<uint8_t> transfer = std::vector<uint8_t>(8192);
};

// One walk of the card (`card`, a fakefat::Card or a lister over one) into
// `st` against `t` (nullptr: no transfer data) at `id`, as CardJobs runs it:
// the first walk after a commit when D's identity isn't the root's (or D is
// unwalked), D's skew otherwise.
cw::CardWalk::Result walk(ts::TagStore& st, cw::Lister& card, const std::vector<uint8_t>* t,
                          const ts::Identity& id) {
  Buffers b;
  auto known = std::make_unique<ts::KnownD>();
  auto sink = std::make_unique<ts::WalkSink>();
  auto w = std::make_unique<cw::CardWalk>();
  cw::CardWalk::Result r;
  if (!known->begin(st, b.known.data(), 4096) ||
      !sink->begin(st, id, b.journal.data(), 4096, b.doubts.data(), 512)) {
    r.state = cw::CardWalk::State::Failed;
    return r;
  }
  const ts::DeviceInfo& d = st.device();
  const bool first = !(d.present && d.header.walked && d.header.walk == id);
  cc::MemSource src(t ? t->data() : nullptr, t ? static_cast<uint32_t>(t->size()) : 0);
  cw::StreamedTransfer streamed;
  cw::IndexedTransfer indexed;
  cw::CardWalk::Config c;
  c.lister = &card;
  c.known = known.get();
  c.sink = sink.get();
  c.firstAfterCommit = first;
  c.skew = first ? 0 : d.header.skew;
  c.scratch = b.scratch.data();
  c.scratchBytes = static_cast<uint32_t>(b.scratch.size());
  if (t) {
    if (first) {
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(streamed.begin(src, b.transfer.data(), 8192)));
      c.transfer = &streamed;
    } else {
      TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(indexed.begin(src)));
      c.transfer = &indexed;
    }
  }
  TEST_ASSERT_TRUE(w->begin(c));
  w->run();
  return w->result();
}

struct Seen {
  Status status;
  bool confirmed;
  uint32_t size, fatTime;
  uint64_t qfp;
};
std::map<std::string, Seen> view(ts::TagStore& st) {
  std::map<std::string, Seen> out;
  ts::TagStore::View v;
  TEST_ASSERT_TRUE(st.openView(&v));
  while (v.next())
    out[std::string(v.path(), v.pathLength())] =
        Seen{v.row().status, v.row().confirmed, v.record().size, v.record().fatTime, v.record().qfp};
  TEST_ASSERT_FALSE(v.failed());
  return out;
}

// Every audio file of the card, at its size and time, with `status`.
void assertRows(ts::TagStore& st, const fakefat::Card& card, Status status) {
  const auto v = view(st);
  size_t audio = 0;
  for (const auto& kv : card.files) {
    if (!isAudio(kv.first)) continue;
    ++audio;
    auto it = v.find(kv.first);
    TEST_ASSERT_TRUE_MESSAGE(it != v.end(), kv.first.c_str());
    TEST_ASSERT_EQUAL_UINT32(kv.second.size, it->second.size);
    TEST_ASSERT_EQUAL_UINT32(kv.second.fatTime, it->second.fatTime);
    TEST_ASSERT_TRUE_MESSAGE(it->second.status == status, kv.first.c_str());
  }
  TEST_ASSERT_EQUAL_size_t(audio, v.size());
}

// A lister over a card whose file reads can fail (a card's read error).
class FlakyLister : public cw::Lister {
public:
  explicit FlakyLister(fakefat::Card& c) : card_(c) {}
  Open openDir(const char* rel, size_t len) override { return card_.openDir(rel, len); }
  Next next(cw::Entry* out) override { return card_.next(out); }
  void closeDir() override { card_.closeDir(); }
  cc::Source* openFile(const char* rel, size_t len) override {
    return failFiles ? nullptr : card_.openFile(rel, len);
  }
  void closeFile() override { card_.closeFile(); }
  bool failFiles = false;

private:
  fakefat::Card& card_;
};

Seen rowOf(ts::TagStore& st, const std::string& rel) {
  const auto v = view(st);
  auto it = v.find(rel);
  TEST_ASSERT_TRUE_MESSAGE(it != v.end(), rel.c_str());
  return it->second;
}

ts::FolderFacts factsIn(CutFs& fs, ts::TagStore& st, const char* rel) {
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  cc::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> buf(768);
  ts::FolderCursor fc;
  ts::FolderFacts f;
  TEST_ASSERT_TRUE(fc.begin(src, buf.data(), 768));
  TEST_ASSERT_TRUE_MESSAGE(fc.find(rel, std::strlen(rel), &f), rel);
  return f;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// A card with no transfer: the first walk, an unchanged one, a changed one.
// ---------------------------------------------------------------------------
void test_walks_into_the_store() {
  CutFs fs;
  ts::TagStore st(fs, config());
  st.open();
  fakefat::Card card;
  fill(card);
  card.shuffle(7);
  // The first walk: every audio file Added, Pending; D made from it.
  cw::CardWalk::Result r = walk(st, card, nullptr, ts::Identity());
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(12, r.added);
  TEST_ASSERT_TRUE(st.hasWalk());
  assertRows(st, card, Status::Pending);
  ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  TEST_ASSERT_EQUAL_UINT32(12, c.records);
  assertRows(st, card, Status::Pending);
  TEST_ASSERT_TRUE(st.device().header.walked);
  const ts::FolderFacts album = factsIn(fs, st, "Artist/Album");
  TEST_ASSERT_TRUE(album.digest != 0);
  TEST_ASSERT_EQUAL_STRING("cover.jpg", album.image);
  TEST_ASSERT_EQUAL_UINT32(3, album.audio);
  TEST_ASSERT_EQUAL_UINT32(1, album.images);
  TEST_ASSERT_EQUAL_UINT32(1, album.others);
  TEST_ASSERT_EQUAL_STRING("folder.jpg", factsIn(fs, st, "Band/Live").image);
  // Unchanged: every folder D lists has D's digest (Scans, which it doesn't,
  // is listed and says nothing); nothing reaches the card.
  r = walk(st, card, nullptr, ts::Identity());
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(1, r.foldersMerged);
  TEST_ASSERT_EQUAL_UINT32(0, r.added + r.changed + r.gone + r.folderRows);
  TEST_ASSERT_FALSE(st.hasWalk());
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
  // Changed on a PC: an album added, a file retagged (its time), a folder
  // removed.
  card.addFile("New/Album/01 - Song.mp3", 4000000, kT, 99);
  card.at("Band/Live/02 - Song.mp3").fatTime = shifted(kT, 600);
  card.remove("Other");
  r = walk(st, card, nullptr, ts::Identity());
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(1, r.added);
  TEST_ASSERT_EQUAL_UINT32(1, r.changed);
  TEST_ASSERT_EQUAL_UINT32(3, r.gone);
  assertRows(st, card, Status::Pending);
  c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  assertRows(st, card, Status::Pending);
  // And again: nothing.
  r = walk(st, card, nullptr, ts::Identity());
  TEST_ASSERT_EQUAL_UINT32(0, r.added + r.changed + r.gone + r.folderRows);
  TEST_ASSERT_FALSE(st.hasWalk());
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
}

// ---------------------------------------------------------------------------
// A transfer whose stamps a PC shifted an hour: every file doubtful, the
// doubts read back from walk.jnl and settled by the skew; DHDR keeps it, so
// the next walk at the same commit writes nothing; N2's builder takes T's
// records for every file.
// ---------------------------------------------------------------------------
void test_a_transfer_and_its_skew() {
  CutFs fs;
  ts::TagStore st(fs, config());
  st.open();
  fakefat::Card card;
  fill(card);
  const std::vector<uint8_t> t = transferOf(card, 3600);
  cw::CardWalk::Result r = walk(st, card, &t, commit(1));
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(12, r.doubtful);
  TEST_ASSERT_EQUAL_UINT32(12, r.bySkew);
  TEST_ASSERT_EQUAL_INT32(3600, r.summary.skew);
  assertRows(st, card, Status::Software);
  ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  assertRows(st, card, Status::Software);
  TEST_ASSERT_TRUE(st.device().header.walked);
  TEST_ASSERT_TRUE(st.device().header.walk == commit(1));
  TEST_ASSERT_EQUAL_INT32(3600, st.device().header.skew);
  TEST_ASSERT_TRUE(factsIn(fs, st, "Artist/Album").imageOwned);  // T lists its cover
  // The same commit again: the skew from DHDR, nothing new.
  r = walk(st, card, &t, commit(1));
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(0, r.added + r.changed + r.gone + r.folderRows + r.doubtful);
  TEST_ASSERT_FALSE(st.hasWalk());
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
  // N2's builder: T fresh for every file by the skew.
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  cc::MemSource tsrc(t.data(), static_cast<uint32_t>(t.size()));
  cc::MemSource dsrc(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource rsrc(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource fsrc(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> rb(256), fb(768);
  ts::BuilderRows rows;
  ts::BuilderFacts facts;
  TEST_ASSERT_TRUE(rows.begin(rsrc, rb.data(), 256));
  TEST_ASSERT_TRUE(facts.begin(fsrc, fb.data(), 768));
  LibraryIndex index;
  LibraryBuilder b;
  LibraryBuilder::Config bc;
  bc.transfer = &tsrc;
  bc.device = &dsrc;
  bc.rows = &rows;
  bc.facts = &facts;
  bc.skew = st.device().header.skew;
  const LibraryBuilder::Result res = b.build(index, bc);
  TEST_ASSERT_TRUE(res.built);
  TEST_ASSERT_EQUAL_UINT32(12, res.fromTransfer);
  TEST_ASSERT_EQUAL_UINT32(0, res.pending);
}

// ---------------------------------------------------------------------------
// A power cut at every step of that walk and its compaction (the doubts'
// run 1, the settled rows' run 2, the rename): the boot after it finds the
// card's files at their sizes and times, Pending or T's, D whole; the next
// boot's walk and compaction reach the D the uncut ones made.
// ---------------------------------------------------------------------------
void test_a_cut_walk_is_walked_again() {
  fakefat::Card card;
  fill(card);
  const std::vector<uint8_t> t = transferOf(card, 3600);
  // The uncut run, for its steps and its D.
  long steps = 0;
  std::vector<uint8_t> want;
  {
    CutFs fs;
    ts::TagStore st(fs, config());
    st.open();
    TEST_ASSERT_TRUE(walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done);
    TEST_ASSERT_TRUE(st.compact().ok);
    steps = fs.steps;
    want = fs.bytes(st.devicePath());
  }
  printf("[tagstore+walk] a first walk after a transfer and its compaction: %ld steps\n", steps);
  uint32_t boots = 0, runOneAlone = 0;
  for (long k = 1; k <= steps; ++k) {
    CutFs fs;
    fs.cutAt = k;
    {
      ts::TagStore st(fs, config());
      st.open();
      if (walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done && !fs.dead) st.compact();
    }
    TEST_ASSERT_TRUE(fs.dead);
    TEST_ASSERT_EQUAL_UINT32(0, fs.openNow);  // every file closed, the one whose write the cut failed too
    TEST_ASSERT_TRUE(fs.violations.empty());
    for (Variant v : {Variant::InOrder, Variant::LoseUnsynced, Variant::Torn}) {
      CutFs boot = fs.reboot(v);
      ts::TagStore st(boot, config());
      st.open();
      char label[64];
      std::snprintf(label, sizeof(label), "cut at %ld, variant %d", k, static_cast<int>(v));
      // What the boot finds: the card's files, at their sizes and times.
      const auto seen = view(st);
      if (!seen.empty()) {
        size_t software = 0;
        for (const auto& kv : card.files) {
          if (!isAudio(kv.first)) continue;
          auto it = seen.find(kv.first);
          TEST_ASSERT_TRUE_MESSAGE(it != seen.end(), label);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(kv.second.fatTime, it->second.fatTime, label);
          TEST_ASSERT_TRUE_MESSAGE(it->second.status == Status::Pending || it->second.status == Status::Software,
                                   label);
          software += it->second.status == Status::Software ? 1 : 0;
        }
        if (software == 0 && st.hasWalk()) ++runOneAlone;
      }
      // The next boot's work: the journals in, the walk, its compaction.
      if (st.hasJournals() || !st.device().present) TEST_ASSERT_TRUE_MESSAGE(st.compact().ok, label);
      TEST_ASSERT_TRUE_MESSAGE(walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done, label);
      TEST_ASSERT_TRUE_MESSAGE(st.compact().ok, label);
      assertRows(st, card, Status::Software);
      TEST_ASSERT_TRUE_MESSAGE(st.device().header.walk == commit(1), label);
      TEST_ASSERT_EQUAL_INT32_MESSAGE(3600, st.device().header.skew, label);
      // D as the uncut run made it, but for its generation and header CRC.
      std::vector<uint8_t> got = boot.bytes(st.devicePath());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(want.size(), got.size(), label);
      for (size_t i : {20u, 21u, 22u, 23u, 32u, 33u, 34u, 35u}) got[i] = want[i];
      TEST_ASSERT_TRUE_MESSAGE(got == want, label);
      TEST_ASSERT_TRUE_MESSAGE(boot.violations.empty(), label);
      ++boots;
    }
  }
  printf("[tagstore+walk] %u boots checked; %u found run 1 alone\n", boots, runOneAlone);
}

// ---------------------------------------------------------------------------
// Doubts left unsettled are asked again. A PC rewrote a file's time without
// changing its bytes: the walk at the same commit finds it doubtful and
// settles it by its qfp (T's, confirmed). Cut after run 1 (the file's row
// without T) and before run 2 (its settled row), or with the qfp read
// failing, D is left unwalked: the next boot's walk is a first one, asks T
// about every file, reads the qfp and settles it. (Kept at the commit, that
// walk found the row at the file's size and time, the folder's digest D's,
// and never asked T again: the file stayed Pending until the next commit.)
// ---------------------------------------------------------------------------
namespace {
const char* const kTouched = "Artist/Album/02 - Song.mp3";

// D after a first walk against T (stamps equal), then the touch.
CutFs walkedThenTouched(fakefat::Card& card, const std::vector<uint8_t>& t) {
  CutFs fs;
  ts::TagStore st(fs, config());
  st.open();
  TEST_ASSERT_TRUE(walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done);
  TEST_ASSERT_TRUE(st.compact().ok);
  assertRows(st, card, Status::Software);
  card.at(kTouched).fatTime = shifted(kT, 7 * 60 + 4);
  return fs;
}

void assertSettled(ts::TagStore& st, const fakefat::Card& card, const char* label) {
  const Seen s = rowOf(st, kTouched);
  TEST_ASSERT_TRUE_MESSAGE(s.status == Status::Software && s.confirmed, label);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(card.files.at(kTouched).fatTime, s.fatTime, label);
  TEST_ASSERT_TRUE_MESSAGE(st.device().header.walked && st.device().header.walk == commit(1), label);
  assertRows(st, card, Status::Software);
}
}  // namespace

void test_a_cut_same_commit_walk_settles_again() {
  fakefat::Card card;
  fill(card);
  const std::vector<uint8_t> t = transferOf(card, 0);
  const CutFs base = walkedThenTouched(card, t);
  // Uncut: the walk at the same commit settles the file by its qfp.
  long steps = 0;
  {
    CutFs fs = base;
    ts::TagStore st(fs, config());
    st.open();
    const long before = fs.steps;
    const cw::CardWalk::Result r = walk(st, card, &t, commit(1));
    TEST_ASSERT_FALSE(r.summary.firstAfterCommit);
    TEST_ASSERT_EQUAL_UINT32(1, r.doubtful);
    TEST_ASSERT_EQUAL_UINT32(1, r.byQfp);
    TEST_ASSERT_TRUE(st.compact().ok);
    steps = fs.steps - before;
    assertSettled(st, card, "uncut");
  }
  // Cut at every step of that walk and its compaction.
  uint32_t boots = 0, unsettled = 0;
  for (long k = 1; k <= steps; ++k) {
    CutFs fs = base;
    fs.cutAt = fs.steps + k;
    {
      ts::TagStore st(fs, config());
      st.open();
      if (walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done && !fs.dead) st.compact();
    }
    TEST_ASSERT_TRUE(fs.dead);
    TEST_ASSERT_EQUAL_UINT32(0, fs.openNow);  // every file closed, the one whose write the cut failed too
    for (Variant v : {Variant::InOrder, Variant::LoseUnsynced, Variant::Torn}) {
      CutFs boot = fs.reboot(v);
      ts::TagStore st(boot, config());
      st.open();
      char label[64];
      std::snprintf(label, sizeof(label), "cut at %ld, variant %d", k, static_cast<int>(v));
      if (st.walkUnsettled()) ++unsettled;
      if (st.hasJournals() || !st.device().present) TEST_ASSERT_TRUE_MESSAGE(st.compact().ok, label);
      TEST_ASSERT_TRUE_MESSAGE(walk(st, card, &t, commit(1)).state == cw::CardWalk::State::Done, label);
      if (st.hasJournals()) TEST_ASSERT_TRUE_MESSAGE(st.compact().ok, label);
      assertSettled(st, card, label);
      // And the boot after that: nothing new.
      const cw::CardWalk::Result r = walk(st, card, &t, commit(1));
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.doubtful + r.added + r.changed + r.qfpReads, label);
      TEST_ASSERT_FALSE_MESSAGE(st.hasWalk(), label);
      TEST_ASSERT_TRUE_MESSAGE(boot.violations.empty(), label);
      ++boots;
    }
  }
  printf("[tagstore+walk] a same-commit walk with a doubt, cut at each of its %ld steps: %u boots, %u found its run 1 "
         "alone\n",
         steps, boots, unsettled);
  TEST_ASSERT_GREATER_THAN(0, unsettled);
}

void test_a_failed_qfp_read_is_asked_again() {
  fakefat::Card card;
  fill(card);
  const std::vector<uint8_t> t = transferOf(card, 0);
  CutFs fs = walkedThenTouched(card, t);
  ts::TagStore st(fs, config());
  st.open();
  FlakyLister flaky(card);
  flaky.failFiles = true;
  // The read fails: the row without T (Pending), the walk unsettled, D
  // unwalked.
  cw::CardWalk::Result r = walk(st, flaky, &t, commit(1));
  TEST_ASSERT_TRUE(r.state == cw::CardWalk::State::Done);
  TEST_ASSERT_EQUAL_UINT32(1, r.qfpFailed);
  TEST_ASSERT_TRUE(r.summary.unsettled);
  TEST_ASSERT_TRUE(st.walkUnsettled());
  {
    ts::TagStore again(fs, config());  // a boot here: run 2's End says it
    again.open();
    TEST_ASSERT_TRUE(again.hasWalk());
    TEST_ASSERT_TRUE(again.walkUnsettled());
  }
  TEST_ASSERT_TRUE(st.compact().ok);
  TEST_ASSERT_TRUE(rowOf(st, kTouched).status == Status::Pending);
  TEST_ASSERT_FALSE(st.device().header.walked);
  // It reads again: a first walk, against T, settles it.
  flaky.failFiles = false;
  r = walk(st, flaky, &t, commit(1));
  TEST_ASSERT_TRUE(r.summary.firstAfterCommit);
  TEST_ASSERT_EQUAL_UINT32(1, r.byQfp);
  TEST_ASSERT_FALSE(r.summary.unsettled);
  TEST_ASSERT_TRUE(st.compact().ok);
  assertSettled(st, card, "after the read that worked");
  r = walk(st, flaky, &t, commit(1));
  TEST_ASSERT_EQUAL_UINT32(0, r.doubtful + r.qfpReads);
  TEST_ASSERT_FALSE(st.hasWalk());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_walks_into_the_store);
  RUN_TEST(test_a_transfer_and_its_skew);
  RUN_TEST(test_a_cut_walk_is_walked_again);
  RUN_TEST(test_a_cut_same_commit_walk_settles_again);
  RUN_TEST(test_a_failed_qfp_read_is_asked_again);
  return UNITY_END();
}
