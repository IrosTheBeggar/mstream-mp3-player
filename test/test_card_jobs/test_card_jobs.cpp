// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for N10's portable glue (docs/METADATA.md 3.2.2, 3.2.3, 3.3;
// milestone N10): the boot's decision (LibraryBoot), the transfer's root as
// the boot reads it (CardRoot), and the card worker's jobs (CardJobs): the
// validation walk, the compaction and the scan, one step at a time as the
// firmware's card worker runs them, on fake FAT trees (test/support/
// FakeFat.h) with real tagged files from the tag corpus, and the device's
// records on a fake card (test/support/CutFs.h), then built into the index
// as the boot would (LibraryBuilder). Sessions are boots: a new TagStore
// over the same card, open()ed. The slices (2026-10-09): N11's synthetic
// card walked a folder a step, several steps a hand-off on a fake clock;
// the scan's rest a few files a hand-off; the loop's cut.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/CutFs.h"
#include "../support/FakeFat.h"
#include "CardContract.h"
#include "CardJobs.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "CardTags.h"
#include "LibraryBoot.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "TagStore.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace ts = tagstore;
namespace cj = cardjobs;
using cutfs::CutFs;
using Job = ScanScheduler::Job;
using Src = ScanScheduler::Source;

void setUp() {}
void tearDown() {}

namespace {

constexpr uint32_t kT = 0x5D4773D5;  // 2026-10-07 14:30:42
constexpr uint64_t kCardId = 0x5EEDC0DE0A1B2C3Dull;

// ---- the corpus ----
std::string corpusDir() {
  const char* tries[] = {"test/fixtures/tags", "../test/fixtures/tags", "../../test/fixtures/tags"};
  for (const char* t : tries) {
    const std::string p = std::string(t) + "/expected.json";
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
      std::fclose(f);
      return t;
    }
  }
  std::string self = __FILE__;  // .../test/test_card_jobs/test_card_jobs.cpp
  for (int up = 0; up < 2; ++up) self = self.substr(0, self.find_last_of("/\\"));
  return self + "/fixtures/tags";
}

std::vector<uint8_t> corpus(const char* name) {
  const std::string p = corpusDir() + "/" + name;
  std::vector<uint8_t> b;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    uint8_t buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
  }
  TEST_ASSERT_TRUE_MESSAGE(!b.empty(), p.c_str());
  return b;
}

// The worker's clock for the slices (Config::nowUs): a listing and a file's
// open cost what a test sets (TestCard::listUs, openUs).
uint64_t g_us = 0;
uint64_t fakeUs() { return g_us; }

// ---- the card: FakeFat's tree, real bytes for some files ----
class TestCard : public cj::Card {
public:
  fakefat::Card tree;
  std::map<std::string, std::vector<uint8_t>> bytes;
  std::map<std::string, uint32_t> reads;  // files opened, by path
  uint32_t stats = 0;
  uint32_t listUs = 0, openUs = 0;
  // The loop meanwhile (a cut): at each listing, at each file's open.
  std::function<void()> onList, onOpen;

  void addBytes(const std::string& rel, const std::vector<uint8_t>& b, uint32_t fatTime = kT) {
    bytes[rel] = b;
    tree.addFile(rel, static_cast<uint32_t>(b.size()), fatTime, 1);
  }
  void addNoise(const std::string& rel, uint32_t size, uint32_t seed, uint32_t fatTime = kT) {
    bytes.erase(rel);
    tree.addFile(rel, size, fatTime, seed);
  }

  Open openDir(const char* rel, size_t len) override {
    g_us += listUs;
    if (onList) onList();
    return tree.openDir(rel, len);
  }
  Next next(cardwalk::Entry* out) override { return tree.next(out); }
  void closeDir() override { tree.closeDir(); }
  cc::Source* openFile(const char* rel, size_t len) override {
    const std::string p(rel, len);
    g_us += openUs;
    if (onOpen) onOpen();
    ++reads[p];
    auto it = bytes.find(p);
    if (it == bytes.end()) return tree.openFile(rel, len);
    if (!tree.files.count(p)) return nullptr;
    mem_.reset(new cc::MemSource(it->second.data(), static_cast<uint32_t>(it->second.size())));
    return mem_.get();
  }
  void closeFile() override { mem_.reset(); }
  bool stat(const char* rel, size_t len, uint32_t* size, uint32_t* fatTime) override {
    ++stats;
    auto it = tree.files.find(std::string(rel, len));
    if (it == tree.files.end()) return false;
    *size = it->second.size;
    *fatTime = it->second.fatTime;
    return true;
  }

private:
  std::unique_ptr<cc::MemSource> mem_;
};

// A card of four tagged files (FLAC, Opus and two MP3s from the corpus), two
// files of noise named .mp3 (TagScan: no tag, or not an MP3), a cover and a
// text file.
void fill(TestCard& c) {
  c.addBytes("Artist/Album/01 - a.flac", corpus("flac_full.flac"));
  c.addBytes("Artist/Album/02 - b.opus", corpus("opus_basic.opus"));
  c.addBytes("Artist/Other/01 - c.mp3", corpus("v22.mp3"));
  c.addBytes("Band/Live/01 - d.mp3", corpus("v1_only.mp3"));
  c.addNoise("Band/Live/02 - e.mp3", 9000, 7);
  c.addNoise("Loose/03 - f.mp3", 7000, 8);
  c.addNoise("Artist/Album/cover.jpg", 4000, 9);
  c.addNoise("Artist/Album/notes.txt", 100, 10);
}
constexpr uint32_t kAudio = 6;

ts::TagStore::Config storeConfig() {
  ts::TagStore::Config c;
  c.producer = "mstream-player test";
  c.parserVersion = tagscan::kParserVersion;
  return c;
}

// A boot: the store over the card, its recovery done.
struct Session {
  CutFs& fs;
  TestCard& card;
  ts::TagStore store;
  cj::Jobs jobs;
  uint32_t now = 1000;
  Session(CutFs& f, TestCard& c, const ts::TagStore::Config& sc = storeConfig()) : fs(f), card(c), store(f, sc) {
    store.open();
  }
  // The index's files (Config::indexed): what the walk's news leaves out.
  std::set<std::string> indexed;
  bool useIndexed = false;
  uint32_t sliceUs = 0;  // a slice on the fake clock (0: one unit a step)
  static bool isIndexed(const char* rel, size_t len, void* ctx) {
    return static_cast<Session*>(ctx)->indexed.count(std::string(rel, len)) > 0;
  }
  void begin(const char* tPath = nullptr, const ts::Identity& root = ts::Identity(), uint32_t chunkFiles = 100) {
    cj::Config c;
    if (useIndexed) {
      c.indexed = isIndexed;
      c.indexedCtx = this;
    }
    c.store = &store;
    c.card = &card;
    c.fs = &fs;
    c.transferPath = tPath;
    c.root = root;
    c.chunkFiles = chunkFiles;
    c.rowsPerStep = 3;  // small: the rest spans steps
    if (sliceUs) {
      c.sliceUs = sliceUs;
      c.nowUs = fakeUs;
    }
    jobs.begin(c);
  }
  // One step of `job`, as the worker takes it.
  const cj::Done& run(Job job, Src src = Src::Rest, const char* rel = nullptr) {
    TEST_ASSERT_TRUE(jobs.prepare(job, src, rel, rel ? std::strlen(rel) : 0, now));
    jobs.step();
    now += 10;
    return jobs.finish();
  }
  // The jobs in the scheduler's order (the walk, a compaction, the scan's
  // rest) until none has work; every step's Done to `seen`.
  uint32_t drain(const std::function<void(const cj::Done&)>& seen = nullptr) {
    uint32_t steps = 0;
    for (int guard = 0; guard < 100000; ++guard) {
      const Job job = jobs.walkWork() ? Job::Walk : jobs.compactWork() ? Job::Compact : jobs.restWork() ? Job::Scan : Job::None;
      if (job == Job::None) return steps;
      const cj::Done& d = run(job);
      ++steps;
      if (seen) seen(d);
    }
    TEST_FAIL_MESSAGE("the jobs never ran out of work");
    return steps;
  }
};

// What D (and its journals) says of each file: View's rows.
std::map<std::string, ts::Row> rows(ts::TagStore& st) {
  std::map<std::string, ts::Row> out;
  ts::TagStore::View v;
  TEST_ASSERT_TRUE(st.openView(&v));
  while (v.next()) out[std::string(v.path(), v.pathLength())] = v.row();
  TEST_ASSERT_FALSE(v.failed());
  return out;
}

// The index the boot builds from the records (T at `tPath`, D compacted).
LibraryBuilder::Result build(LibraryIndex& index, CutFs& fs, ts::TagStore& st, const char* tPath = nullptr,
                             bool tLists = false) {
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  const std::vector<uint8_t> t = tPath ? fs.bytes(tPath) : std::vector<uint8_t>();
  cc::MemSource dsrc(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource rsrc(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource fsrc(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource tsrc(t.data(), static_cast<uint32_t>(t.size()));
  std::vector<uint8_t> rb(256), fb(768);
  ts::BuilderRows brows;
  ts::BuilderFacts facts;
  LibraryBuilder::Config bc;
  if (!d.empty()) {
    TEST_ASSERT_TRUE(brows.begin(rsrc, rb.data(), 256));
    TEST_ASSERT_TRUE(facts.begin(fsrc, fb.data(), 768));
    bc.device = &dsrc;
    bc.rows = &brows;
    bc.facts = &facts;
  }
  if (tPath) bc.transfer = &tsrc;
  bc.skew = st.device().header.skew;
  bc.transferLists = tLists;
  LibraryBuilder b;
  return b.build(index, bc);
}

std::string titleOf(const LibraryIndex& idx, const char* path) {
  const uint32_t t = idx.findTrack(path);
  if (t == LibraryIndex::kNone) return "<none>";
  uint8_t n = 0;
  const char* s = idx.trackTitle(t, &n);
  return std::string(s, n);
}

// ---- the transfer ----
// T over the card's audio files (sizes, times and qfps as the card has
// them: fresh), source 2, generation `gen`.
std::vector<uint8_t> transferOf(const TestCard& c, uint32_t gen, uint64_t badQfpFor = 0) {
  std::vector<std::string> rels;
  for (const auto& kv : c.tree.files) {
    const std::string leaf = fakefat::leafOf(kv.first);
    if (LibraryIndex::formatOf(leaf.data(), leaf.size()) != LibraryIndex::Format::Unknown) rels.push_back(kv.first);
  }
  std::vector<mptg::RecordIn> in(rels.size());
  for (size_t i = 0; i < rels.size(); ++i) {
    const fakefat::File& f = c.tree.files.at(rels[i]);
    in[i].path = rels[i].c_str();
    in[i].rec.size = f.size;
    in[i].rec.fatTime = f.fatTime;
    auto b = c.bytes.find(rels[i]);
    if (b != c.bytes.end()) {
      const cc::QfpRanges r = cc::qfpRanges(f.size);
      in[i].rec.qfp = cc::qfp(f.size, b->second.data(), r.tailBytes ? b->second.data() + r.tailOffset : nullptr);
    } else {
      in[i].rec.qfp = c.tree.qfpOf(rels[i]);
    }
    if (badQfpFor && cc::pathHash(rels[i].c_str()) == badQfpFor) in[i].rec.qfp ^= 1;
    in[i].rec.container = mptg::kContainerMp3;
    in[i].rec.known = mptg::kKnownRules1;
    in[i].fields[cc::kTitle] = "From T";
  }
  mptg::Meta meta;
  meta.generation = gen;
  meta.cardId = kCardId;
  meta.source = mptg::kSourceTransfer;
  meta.producer = "mstream-terminal test";
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

struct Written {
  std::vector<uint8_t> bytes;
  uint32_t fileBytes = 0, headerCrc = 0;
};

// A root naming `t` (generation `gen`), with `roots` as LIBR.
Written manifestFor(const std::vector<uint8_t>& t, uint32_t gen, uint64_t commitId,
                    const std::vector<const char*>& roots = {}) {
  cc::msmf::CompIn comp;
  comp.kind = cc::kMagicMptg;
  comp.generation = gen;
  comp.fileBytes = static_cast<uint32_t>(t.size());
  cc::MemSource src(t.data(), static_cast<uint32_t>(t.size()));
  cc::Container c;
  mptg::Info info;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(c, src, &info)));
  comp.headerCrc = info.frame.headerCrc;
  cc::msmf::ManifestIn m;
  m.generation = gen;
  m.cardId = kCardId;
  m.flags = cc::msmf::kFinal;
  m.commitId = commitId;
  m.producer = "mstream-terminal test";
  m.comps = &comp;
  m.compCount = 1;
  m.roots = roots.empty() ? nullptr : roots.data();
  m.rootCount = static_cast<uint32_t>(roots.size());
  struct V : cc::Sink {
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      if (offset + n > bytes.size()) bytes.resize(offset + n);
      if (n) std::memcpy(bytes.data() + offset, data, n);
      return true;
    }
    std::vector<uint8_t> bytes;
  } out;
  Written w;
  const char* error = nullptr;
  if (!cc::msmf::write(out, m, &w.fileBytes, &w.headerCrc, &error)) TEST_FAIL_MESSAGE(error);
  w.bytes = out.bytes;
  return w;
}

std::string tagsName(uint32_t gen) {
  char n[32];
  cc::msmf::companionName(cc::kMagicMptg, gen, n, sizeof(n));
  return std::string("/.mstream/") + n;
}

// A transfer committed to the card: T and its root; the root's identity.
ts::Identity commit(CutFs& fs, const TestCard& card, uint32_t gen, uint64_t badQfpFor = 0) {
  const std::vector<uint8_t> t = transferOf(card, gen, badQfpFor);
  fs.put(tagsName(gen), t);
  fs.put("/.mstream/manifest.bin", manifestFor(t, gen, 0xC0FFEE00ull + gen).bytes);
  std::vector<uint8_t> scratch(1024);
  cardroot::Root r;
  cardroot::read(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), &r);
  TEST_ASSERT_TRUE(r.present);
  return r.identity;
}

}  // namespace

// ---------------------------------------------------------------------------
// The boot's decision: every row of 3.2.2's table.
// ---------------------------------------------------------------------------
void test_the_boots_decision() {
  using libraryboot::Action;
  using libraryboot::Saved;
  auto at = [](Saved s, bool marker, bool t, bool d) {
    libraryboot::In in;
    in.saved = s;
    in.marker = marker;
    in.transfer = t;
    in.device = d;
    return libraryboot::decide(in);
  };
  // v6, the inputs the card's, the marker: built now, the marker removed.
  for (int r = 0; r < 4; ++r) {
    const libraryboot::Decision d = at(Saved::Matches, true, r & 1, r & 2);
    TEST_ASSERT_TRUE(d.action == Action::Build);
    TEST_ASSERT_TRUE(d.compactFirst);
    TEST_ASSERT_TRUE(d.removeMarker);
  }
  // ... no marker: loaded (no walk, no build), records or not.
  for (int r = 0; r < 4; ++r) TEST_ASSERT_TRUE(at(Saved::Matches, false, r & 1, r & 2).action == Action::Load);
  // Another identity, an older version, unreadable or none: built from the
  // records when there are any (T, or D alone), else /music walked.
  for (Saved s : {Saved::Differs, Saved::Outdated, Saved::Corrupt, Saved::Missing}) {
    for (int r = 0; r < 4; ++r) {
      const bool t = r & 1, dv = r & 2;
      for (bool marker : {false, true}) {
        const libraryboot::Decision d = at(s, marker, t, dv);
        TEST_ASSERT_TRUE(d.action == (t || dv ? Action::Build : Action::Walk));
        TEST_ASSERT_EQUAL(marker, d.removeMarker);
        TEST_ASSERT_TRUE(d.why[0] != 0);
      }
    }
  }

  // matches(): the identity, zeros for none; no path signature; T's use not asked.
  ts::Identity root;
  root.present = true;
  root.cardId = kCardId;
  root.generation = 4;
  root.commitId = 0x77;
  root.tagsCrc = 0x99;
  LibraryIndex::Inputs in = libraryboot::inputsOf(root, true, 5, 6);
  TEST_ASSERT_TRUE(in.transfer);
  TEST_ASSERT_EQUAL_UINT32(5, in.deviceCrc);
  TEST_ASSERT_EQUAL_UINT32(6, in.journalSeq);
  TEST_ASSERT_TRUE(libraryboot::matches(in, root));
  // The soft inputs: equal, the index is current; the scan went on, or the
  // build left the journals out (their compaction refused: N10's review),
  // rebuilt at the scan's end. The hard ones don't care.
  TEST_ASSERT_FALSE(libraryboot::softStale(in, 5, 6));
  TEST_ASSERT_TRUE(libraryboot::softStale(in, 5, 7));
  TEST_ASSERT_TRUE(libraryboot::softStale(in, 4, 6));
  const LibraryIndex::Inputs left = libraryboot::inputsOf(root, true, 5, 6, true);
  TEST_ASSERT_EQUAL_UINT32(libraryboot::kJournalsLeftOut, left.journalSeq);
  TEST_ASSERT_TRUE(libraryboot::matches(left, root));
  for (uint32_t seq : {0u, 1u, 6u, 1000000u}) TEST_ASSERT_TRUE(libraryboot::softStale(left, 5, seq));
  in.transfer = false;  // T failed its checks at that build: the same T fails again
  TEST_ASSERT_TRUE(libraryboot::matches(in, root));
  LibraryIndex::Inputs walked = in;
  walked.walkSignature = 1;  // an older firmware's path walk
  TEST_ASSERT_FALSE(libraryboot::matches(walked, root));
  for (int k = 0; k < 4; ++k) {
    ts::Identity other = root;
    if (k == 0) other.cardId ^= 1;
    if (k == 1) ++other.generation;
    if (k == 2) other.commitId ^= 1;
    if (k == 3) other.tagsCrc ^= 1;
    TEST_ASSERT_FALSE(libraryboot::matches(in, other));
  }
  // No transfer data: an index built without T matches; one with T doesn't.
  const ts::Identity none;
  TEST_ASSERT_TRUE(libraryboot::matches(libraryboot::inputsOf(none, true, 1, 2), none));
  TEST_ASSERT_FALSE(libraryboot::inputsOf(none, true, 1, 2).transfer);
  TEST_ASSERT_FALSE(libraryboot::matches(libraryboot::inputsOf(root, true, 1, 2), none));
  TEST_ASSERT_FALSE(libraryboot::matches(libraryboot::inputsOf(none, false, 1, 2), root));
  // savedOf(): peek()'s answer turned into the table's column.
  TEST_ASSERT_TRUE(libraryboot::savedOf(LibraryIndex::Load::Loaded, in, root) == Saved::Matches);
  TEST_ASSERT_TRUE(libraryboot::savedOf(LibraryIndex::Load::Loaded, in, none) == Saved::Differs);
  TEST_ASSERT_TRUE(libraryboot::savedOf(LibraryIndex::Load::Outdated, in, root) == Saved::Outdated);
  TEST_ASSERT_TRUE(libraryboot::savedOf(LibraryIndex::Load::Corrupt, in, root) == Saved::Corrupt);
  TEST_ASSERT_TRUE(libraryboot::savedOf(LibraryIndex::Load::NoMemory, in, root) == Saved::Corrupt);
  for (int s = 0; s < 5; ++s) TEST_ASSERT_TRUE(libraryboot::savedName(static_cast<Saved>(s))[0] != '?');
  for (int a = 0; a < 3; ++a) TEST_ASSERT_TRUE(libraryboot::actionName(static_cast<Action>(a))[0] != '?');
}

// ---------------------------------------------------------------------------
// The root as the boot reads it (2.5.4): the election, T's frame and header
// against COMP, LIBR, the plan; anything that fails is "no transfer data".
// ---------------------------------------------------------------------------
void test_the_root() {
  CutFs fs;
  TestCard card;
  fill(card);
  std::vector<uint8_t> scratch(512);
  auto read = [&]() {
    std::unique_ptr<cardroot::Root> r(new cardroot::Root());
    cardroot::read(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), r.get());
    return r;
  };
  {
    auto r = read();
    TEST_ASSERT_FALSE(r->present);
    TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
    TEST_ASSERT_FALSE(r->plan);
    char line[160];
    cardroot::describe(*r, line, sizeof(line));
    TEST_ASSERT_EQUAL_STRING("no transfer data (none on the card)", line);
  }
  // A commit: T and the root naming it, two library roots.
  const std::vector<uint8_t> t5 = transferOf(card, 5);
  fs.put(tagsName(5), t5);
  fs.put("/.mstream/manifest.bin", manifestFor(t5, 5, 0xABC, {"Lib B", "Lib A"}).bytes);
  {
    auto r = read();
    TEST_ASSERT_TRUE(r->present);
    TEST_ASSERT_TRUE(r->why == cc::Why::Ok);
    TEST_ASSERT_TRUE(r->identity.present);
    TEST_ASSERT_EQUAL_UINT64(kCardId, r->identity.cardId);
    TEST_ASSERT_EQUAL_UINT32(5, r->identity.generation);
    TEST_ASSERT_EQUAL_UINT64(0xABC, r->identity.commitId);
    cc::MemSource src(t5.data(), static_cast<uint32_t>(t5.size()));
    cc::Container c;
    mptg::Info info;
    mptg::openFile(c, src, &info);
    TEST_ASSERT_EQUAL_HEX32(info.frame.headerCrc, r->identity.tagsCrc);
    TEST_ASSERT_EQUAL_STRING(tagsName(5).c_str(), r->tagsPath);
    TEST_ASSERT_EQUAL_UINT32(t5.size(), r->tagsBytes);
    TEST_ASSERT_EQUAL_UINT32(kAudio, r->tagsRecords);  // T's header's count (the update step's memory check)
    TEST_ASSERT_EQUAL_UINT32(2, r->rootCount);
    TEST_ASSERT_EQUAL_STRING("Lib A", r->roots[0]);  // LIBR is sorted
    TEST_ASSERT_EQUAL_STRING("Lib B", r->rootList()[1]);
    char line[200];
    cardroot::describe(*r, line, sizeof(line));
    TEST_ASSERT_TRUE(std::strstr(line, "tags-00000005.bin") != nullptr);
  }
  // A newer manifest.tmp wins the election; its T isn't on the card: absent.
  const std::vector<uint8_t> t6 = transferOf(card, 6);
  fs.put("/.mstream/manifest.tmp", manifestFor(t6, 6, 0xDEF).bytes);
  {
    auto r = read();
    TEST_ASSERT_FALSE(r->present);
    TEST_ASSERT_TRUE(r->why == cc::Why::Missing);
  }
  fs.put(tagsName(6), t6);
  {
    auto r = read();
    TEST_ASSERT_TRUE(r->present);
    TEST_ASSERT_EQUAL_UINT32(6, r->identity.generation);
    TEST_ASSERT_EQUAL_UINT32(0, r->rootCount);
  }
  // T replaced by another file of the same name (another generation inside,
  // or a header that isn't the entry's): not the root's.
  fs.put(tagsName(6), transferOf(card, 7));
  TEST_ASSERT_TRUE(read()->why == cc::Why::Comp);
  std::vector<uint8_t> broken = t6;
  broken[20] ^= 1;  // the header's CRC fails
  fs.put(tagsName(6), broken);
  {
    auto r = read();
    TEST_ASSERT_FALSE(r->present);
    TEST_ASSERT_TRUE(r->why != cc::Why::Ok);
  }
  // The .tmp damaged: the .bin's commit counts again.
  std::vector<uint8_t> m6 = fs.bytes("/.mstream/manifest.tmp");
  m6[m6.size() - 1] ^= 0xFF;
  fs.put("/.mstream/manifest.tmp", m6);
  {
    auto r = read();
    TEST_ASSERT_TRUE(r->present);
    TEST_ASSERT_EQUAL_UINT32(5, r->identity.generation);
  }
  // A plan on the card: the last transfer didn't finish.
  fs.put("/.mstream/pending.bin", std::vector<uint8_t>{1, 2, 3});
  TEST_ASSERT_TRUE(read()->plan);
}

// ---------------------------------------------------------------------------
// A card filled by hand: the walk finds every file, the scan reads each
// once, the chunk goes to tags.jnl, a compaction makes D, and the build
// names the tracks from their tags. The next boot's walk and scan find
// nothing to do; a hand-copied album at the boot after is walked and read
// alone (U11's new files).
// ---------------------------------------------------------------------------
void test_a_hand_filled_card() {
  CutFs fs;
  TestCard card;
  fill(card);
  {
    Session s(fs, card);
    s.begin();
    TEST_ASSERT_TRUE(s.jobs.restWork() == false);  // no D, no journal: nothing to scan until a walk
    s.jobs.askWalk();
    TEST_ASSERT_TRUE(s.jobs.walkWork());
    uint32_t walksEnded = 0, read = 0, appended = 0;
    std::map<std::string, int> seen;
    cardwalk::CardWalk::Result walk;
    s.drain([&](const cj::Done& d) {
      if (d.walkEnded) {
        ++walksEnded;
        walk = d.walk;
      }
      if (d.read) {
        ++read;
        ++seen[d.rel];
        TEST_ASSERT_TRUE(s.jobs.record() != nullptr);
      }
      if (d.appended) ++appended;
    });
    TEST_ASSERT_EQUAL_UINT32(1, walksEnded);
    TEST_ASSERT_TRUE(walk.state == cardwalk::CardWalk::State::Done);
    TEST_ASSERT_EQUAL_UINT32(kAudio, walk.added);
    TEST_ASSERT_TRUE(walk.summary.changed);
    TEST_ASSERT_EQUAL_UINT32(kAudio, read);
    for (const auto& kv : seen) TEST_ASSERT_EQUAL_INT_MESSAGE(1, kv.second, kv.first.c_str());
    TEST_ASSERT_TRUE(appended >= 1);
    TEST_ASSERT_FALSE(s.jobs.chunkPending());
    TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned);
    // Every audio file has its reading in the journal.
    for (const auto& kv : rows(s.store)) {
      TEST_ASSERT_TRUE_MESSAGE(kv.second.status == ts::Status::Scanned || kv.second.status == ts::Status::Unreadable,
                               kv.first.c_str());
    }
    // Before a build: a compaction (the update step's).
    s.jobs.askCompact();
    TEST_ASSERT_TRUE(s.jobs.compactWork());
    s.drain();
    TEST_ASSERT_FALSE(s.store.hasJournals());
    LibraryIndex idx;
    const LibraryBuilder::Result br = build(idx, fs, s.store);
    TEST_ASSERT_TRUE(br.built);
    TEST_ASSERT_EQUAL_UINT32(kAudio, idx.trackCount());
    TEST_ASSERT_EQUAL_UINT32(0, br.pending);
    TEST_ASSERT_EQUAL_STRING("Lantern Song", titleOf(idx, "/music/Artist/Album/01 - a.flac").c_str());
    TEST_ASSERT_EQUAL_STRING("Opus Track", titleOf(idx, "/music/Artist/Album/02 - b.opus").c_str());
    TEST_ASSERT_EQUAL_STRING("Old Format", titleOf(idx, "/music/Artist/Other/01 - c.mp3").c_str());
    TEST_ASSERT_EQUAL_STRING("Plain Old Tag", titleOf(idx, "/music/Band/Live/01 - d.mp3").c_str());
  }
  // The next boot: the same card. The walk changes nothing; the scan's one
  // pass of the View finds nothing Pending.
  card.reads.clear();
  {
    Session s(fs, card);
    s.begin();
    s.jobs.askWalk();
    uint32_t read = 0;
    bool changed = true;
    s.drain([&](const cj::Done& d) {
      if (d.read) ++read;
      if (d.walkEnded) changed = d.walk.summary.changed;
    });
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_EQUAL_UINT32(0, read);
    TEST_ASSERT_TRUE(card.reads.empty());  // no file opened at all
    TEST_ASSERT_FALSE(s.jobs.restWork());
  }
  // A hand-copied album: walked and read alone.
  card.addBytes("New/Album/01 - g.mp3", corpus("ape_v1.mp3"));
  card.reads.clear();
  {
    Session s(fs, card);
    s.begin();
    s.jobs.askWalk();
    std::vector<std::string> read;
    uint32_t added = 0;
    s.drain([&](const cj::Done& d) {
      if (d.read) read.push_back(d.rel);
      if (d.walkEnded) added = d.walk.added;
    });
    TEST_ASSERT_EQUAL_UINT32(1, added);
    TEST_ASSERT_EQUAL_UINT32(1, read.size());
    TEST_ASSERT_EQUAL_STRING("New/Album/01 - g.mp3", read[0].c_str());
    s.jobs.askCompact();
    s.drain();
    LibraryIndex idx;
    TEST_ASSERT_TRUE(build(idx, fs, s.store).built);
    TEST_ASSERT_EQUAL_UINT32(kAudio + 1, idx.trackCount());
    TEST_ASSERT_EQUAL_STRING("From ID3v1", titleOf(idx, "/music/New/Album/01 - g.mp3").c_str());
  }
}

// ---------------------------------------------------------------------------
// The walk's news is the files new to the index (U11's count and the
// toast's): an index walked from /music, or built from T, lists files
// before D has them.
// ---------------------------------------------------------------------------
void test_new_to_the_index() {
  CutFs fs;
  TestCard card;
  fill(card);
  Session s(fs, card);
  s.useIndexed = true;
  // The boot walked /music into the index: every file there by its path.
  for (const auto& kv : card.tree.files) s.indexed.insert(kv.first);
  card.addBytes("Hand/Copied/01 - h.flac", corpus("flac_full.flac"));
  s.begin();
  s.jobs.askWalk();
  cj::Done d;
  while (s.jobs.walkWork()) d = s.run(Job::Walk);
  TEST_ASSERT_TRUE(d.walkEnded);
  TEST_ASSERT_EQUAL_UINT32(kAudio + 1, d.walk.added);  // all of them new to D
  TEST_ASSERT_EQUAL_UINT32(1, d.newToIndex);           // one new to the index
  // No predicate: every added file is news.
  CutFs fs2;
  Session t(fs2, card);
  t.begin();
  t.jobs.askWalk();
  while (t.jobs.walkWork()) d = t.run(Job::Walk);
  TEST_ASSERT_EQUAL_UINT32(kAudio + 1, d.newToIndex);
}

// ---------------------------------------------------------------------------
// The loop's sources (the playing track, the queue, the Library tab): a file
// the loop names is read at its f_stat size and time, at once, and the
// rest, whose View hasn't passed it, doesn't read it again.
// ---------------------------------------------------------------------------
void test_the_loops_sources() {
  CutFs fs;
  TestCard card;
  fill(card);
  Session s(fs, card);
  s.begin();
  s.jobs.askWalk();
  while (s.jobs.walkWork()) s.run(Job::Walk);
  // The rest begins (its View open, the first rows passed)...
  const cj::Done& first = s.run(Job::Scan);
  TEST_ASSERT_TRUE(first.read);
  const std::string firstRel = first.rel;
  // ... then the playing track, by its path.
  const char* playing = "Loose/03 - f.mp3";
  TEST_ASSERT_TRUE(firstRel != playing);
  const uint32_t statsBefore = card.stats;
  const cj::Done& d = s.run(Job::Scan, Src::Playing, playing);
  TEST_ASSERT_TRUE(d.handled);
  TEST_ASSERT_TRUE(d.read);
  TEST_ASSERT_TRUE(d.source == Src::Playing);
  TEST_ASSERT_EQUAL_STRING(playing, d.rel);
  TEST_ASSERT_EQUAL_UINT32(statsBefore + 1, card.stats);
  // The rest to its end: each file read once.
  s.drain();
  for (const auto& kv : card.reads) TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, kv.second, kv.first.c_str());
  TEST_ASSERT_EQUAL_UINT32(kAudio, card.reads.size());
  // A file the loop names that is gone: handled, not read; nothing recorded.
  const cj::Done& gone = s.run(Job::Scan, Src::QueueNext, "Nowhere/x.mp3");
  TEST_ASSERT_TRUE(gone.handled);
  TEST_ASSERT_FALSE(gone.read);
  // Its reading in the journal at the card's size and time.
  s.jobs.askCompact();
  s.drain();
  ts::TagStore::View v;
  TEST_ASSERT_TRUE(s.store.openView(&v));
  bool found = false;
  while (v.next()) {
    if (std::string(v.path(), v.pathLength()) != playing) continue;
    found = true;
    TEST_ASSERT_EQUAL_UINT32(card.tree.files.at(playing).size, v.record().size);
    TEST_ASSERT_EQUAL_UINT32(kT, v.record().fatTime);
  }
  TEST_ASSERT_TRUE(found);
}

// ---------------------------------------------------------------------------
// The chunk: appended every chunkFiles files; a full journal (maxChunks)
// asks for a compaction, after which the kept chunk goes; nothing is lost.
// ---------------------------------------------------------------------------
void test_chunks_and_a_full_journal() {
  CutFs fs;
  TestCard card;
  fill(card);
  ts::TagStore::Config sc = storeConfig();
  sc.maxChunks = 2;
  Session s(fs, card, sc);
  s.begin(nullptr, ts::Identity(), 1);
  s.jobs.askWalk();
  while (s.jobs.walkWork()) s.run(Job::Walk);
  // The scan alone (the scheduler would compact as soon as the journal asks):
  // a chunk a file, the third refused by the full journal and kept.
  uint32_t appended = 0, failed = 0;
  while (s.jobs.restWork()) {
    const cj::Done& d = s.run(Job::Scan);
    if (d.appended) ++appended;
    if (d.appendFailed) ++failed;
  }
  TEST_ASSERT_EQUAL_UINT32(2, appended);
  TEST_ASSERT_TRUE(failed >= 1);
  TEST_ASSERT_TRUE(s.jobs.chunkPending());
  TEST_ASSERT_TRUE(s.jobs.compactWork());
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned);
  // The compaction, then the kept chunk: nothing lost.
  uint32_t compactions = 0;
  s.drain([&](const cj::Done& d) {
    if (d.compacted) ++compactions;
  });
  TEST_ASSERT_TRUE(compactions >= 1);
  TEST_ASSERT_FALSE(s.jobs.chunkPending());
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned);  // nothing read twice
  uint32_t recorded = 0;
  for (const auto& kv : rows(s.store)) {
    TEST_ASSERT_TRUE_MESSAGE(kv.second.status != ts::Status::Pending, kv.first.c_str());
    ++recorded;
  }
  TEST_ASSERT_EQUAL_UINT32(kAudio, recorded);
  // Through the scheduler's order the journal never refuses: a compaction
  // comes as soon as it asks.
  CutFs fs2;
  Session s2(fs2, card, sc);
  s2.begin(nullptr, ts::Identity(), 1);
  s2.jobs.askWalk();
  uint32_t refused = 0;
  s2.drain([&](const cj::Done& d) {
    if (d.appendFailed) ++refused;
  });
  TEST_ASSERT_EQUAL_UINT32(0, refused);
  TEST_ASSERT_EQUAL_UINT32(kAudio, s2.jobs.counts().scanned);
}

// ---------------------------------------------------------------------------
// The scan's rules: a file changed since the walk (another size) or gone is
// skipped, not recorded; the next walk sees it changed and the scan reads it.
// A Rescan (a compaction at the next epoch) reads every device file again.
// ---------------------------------------------------------------------------
void test_changed_files_and_a_rescan() {
  CutFs fs;
  TestCard card;
  fill(card);
  {
    Session s(fs, card);
    s.begin();
    s.jobs.askWalk();
    while (s.jobs.walkWork()) s.run(Job::Walk);
    card.tree.at("Band/Live/02 - e.mp3").size += 512;  // changed on a PC after the walk
    card.tree.remove("Loose/03 - f.mp3");
    s.drain();
    TEST_ASSERT_EQUAL_UINT32(kAudio - 2, s.jobs.counts().scanned);
    TEST_ASSERT_EQUAL_UINT32(2, s.jobs.counts().skipped);
    const auto r = rows(s.store);
    TEST_ASSERT_TRUE(r.at("Band/Live/02 - e.mp3").status == ts::Status::Pending);
    TEST_ASSERT_TRUE(r.at("Loose/03 - f.mp3").status == ts::Status::Pending);
  }
  {
    Session s(fs, card);
    s.begin();
    s.jobs.askWalk();
    uint32_t changed = 0, gone = 0;
    std::vector<std::string> read;
    s.drain([&](const cj::Done& d) {
      if (d.walkEnded) {
        changed = d.walk.changed;
        gone = d.walk.gone;
      }
      if (d.read) read.push_back(d.rel);
    });
    TEST_ASSERT_EQUAL_UINT32(1, changed);
    TEST_ASSERT_EQUAL_UINT32(1, gone);
    TEST_ASSERT_EQUAL_UINT32(1, read.size());
    TEST_ASSERT_EQUAL_STRING("Band/Live/02 - e.mp3", read[0].c_str());
    // A Rescan: every device reading Pending again, read again.
    s.jobs.askCompact(true);
    s.jobs.askRest();
    const uint32_t before = s.jobs.counts().scanned;
    s.drain();
    TEST_ASSERT_EQUAL_UINT32(kAudio - 1, s.jobs.counts().scanned - before);
    for (const auto& kv : rows(s.store)) TEST_ASSERT_TRUE(kv.second.status != ts::Status::Pending);
  }
}

// ---------------------------------------------------------------------------
// A card filled by the transfer: the first walk after the commit streams T,
// every file fresh, nothing for the scan; the build names them from T.
// Verify (gv) reads each software file's qfp against T's; Rescan everything
// (gr!) reads every file anyway, a diagnostic.
// ---------------------------------------------------------------------------
void test_a_transfer_card() {
  CutFs fs;
  TestCard card;
  fill(card);
  const uint64_t bad = cc::pathHash("Band/Live/01 - d.mp3");
  const ts::Identity id = commit(fs, card, 3, bad);
  const std::string tPath = tagsName(3);
  Session s(fs, card);
  s.begin(tPath.c_str(), id);
  s.jobs.askWalk();
  uint32_t read = 0;
  cardwalk::CardWalk::Result walk;
  s.drain([&](const cj::Done& d) {
    if (d.read) ++read;
    if (d.walkEnded) walk = d.walk;
  });
  TEST_ASSERT_TRUE(walk.state == cardwalk::CardWalk::State::Done);
  TEST_ASSERT_TRUE(walk.summary.firstAfterCommit);
  TEST_ASSERT_EQUAL_UINT32(0, read);
  for (const auto& kv : rows(s.store)) TEST_ASSERT_TRUE(kv.second.status == ts::Status::Software);
  s.jobs.askCompact();
  s.drain();
  TEST_ASSERT_TRUE(s.store.device().header.walked);
  TEST_ASSERT_TRUE(s.store.device().header.walk == id);
  LibraryIndex idx;
  const LibraryBuilder::Result br = build(idx, fs, s.store, tPath.c_str());
  TEST_ASSERT_TRUE(br.built);
  TEST_ASSERT_EQUAL_UINT32(kAudio, br.fromTransfer);
  TEST_ASSERT_EQUAL_STRING("From T", titleOf(idx, "/music/Artist/Album/01 - a.flac").c_str());

  // Verify: every software file's qfp against T's; the one T got wrong differs.
  s.jobs.askRest(cj::Mode::Verify);
  bool ended = false;
  s.drain([&](const cj::Done& d) {
    if (d.verifyEnded) ended = true;
  });
  TEST_ASSERT_TRUE(ended);
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.verified().checked);
  TEST_ASSERT_EQUAL_UINT32(kAudio - 1, s.jobs.verified().equal);
  TEST_ASSERT_EQUAL_UINT32(1, s.jobs.verified().differ);
  TEST_ASSERT_TRUE(s.jobs.mode() == cj::Mode::Normal);

  // Verify in slices (the dark's 60 ms; the card's clock still): one slice
  // verifies every file and reaches the rest's end, so the mode is Normal
  // again by finish(). Each file says it was verified, not read for its
  // tags (CardTasks counted them as the scan's: "Reading tags" went up).
  s.sliceUs = 60000;
  s.begin(tPath.c_str(), id);  // (again: the slice's clock)
  s.jobs.askRest(cj::Mode::Verify);
  const cj::Done& v = s.run(Job::Scan);
  TEST_ASSERT_TRUE(v.verifyEnded);
  TEST_ASSERT_TRUE(s.jobs.mode() == cj::Mode::Normal);
  TEST_ASSERT_EQUAL_UINT32(kAudio, v.files);
  for (uint32_t i = 0; i < v.files; ++i) {
    const cj::FileDone f = s.jobs.file(i);
    TEST_ASSERT_TRUE(f.verified);
    TEST_ASSERT_TRUE(f.read);
  }
  TEST_ASSERT_TRUE(v.verified);
  TEST_ASSERT_EQUAL_UINT32(2 * kAudio, s.jobs.verified().checked);

  // Rescan everything: the transfer's files read too (one slice: read, not
  // verified).
  const uint32_t before = s.jobs.counts().scanned;
  s.jobs.askRest(cj::Mode::All);
  const cj::Done& a = s.run(Job::Scan);
  TEST_ASSERT_EQUAL_UINT32(kAudio, a.files);
  for (uint32_t i = 0; i < a.files; ++i) TEST_ASSERT_FALSE(s.jobs.file(i).verified);
  TEST_ASSERT_FALSE(a.verified);
  s.drain();
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned - before);
  TEST_ASSERT_TRUE(s.jobs.mode() == cj::Mode::Normal);
  s.sliceUs = 0;
}

// ---------------------------------------------------------------------------
// A T that fails its checks as the walk streams it (a section's CRC): the
// walk is walked again without it, T absent for the session, and the scan
// reads every file itself (slower, never wrong).
// ---------------------------------------------------------------------------
void test_a_bad_transfer_is_walked_without() {
  CutFs fs;
  TestCard card;
  fill(card);
  const ts::Identity id = commit(fs, card, 4);
  const std::string tPath = tagsName(4);
  // A letter of a record's title changed (STRS): the frame and the header
  // still check (the root takes it), the section's CRC doesn't, which the
  // walk learns at T's end.
  std::vector<uint8_t> t = fs.bytes(tPath);
  const char* title = "From T";
  auto at = std::search(t.begin(), t.end(), title, title + 6);
  TEST_ASSERT_TRUE(at != t.end());
  at[1] = 'q';
  fs.put(tPath, t);
  std::vector<uint8_t> scratch(512);
  cardroot::Root r;
  cardroot::read(fs, scratch.data(), 512, &r);
  TEST_ASSERT_TRUE(r.present);
  Session s(fs, card);
  s.begin(tPath.c_str(), id);
  s.jobs.askWalk();
  uint32_t walks = 0, retried = 0;
  s.drain([&](const cj::Done& d) {
    if (d.walkEnded) ++walks;
    if (d.walkRetried) ++retried;
  });
  TEST_ASSERT_EQUAL_UINT32(2, walks);
  TEST_ASSERT_EQUAL_UINT32(1, retried);
  TEST_ASSERT_TRUE(s.jobs.transferBad());
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned);
  s.jobs.askCompact();
  s.drain();
  TEST_ASSERT_FALSE(s.store.device().header.walk.present);  // walked against no transfer data
}

// ---------------------------------------------------------------------------
// A card that refuses writes (full), or is pulled (N10's review): nothing
// is tried again pass after pass. A walk left to merge whose compaction
// fails isn't handed again (the jobs run out of work); a chunk the card
// refused is offered again from the loop only after Config::retryMs; a
// read whose record never reached tags.jnl is no news for the update
// step; a rest whose View can't be read says so.
// ---------------------------------------------------------------------------
void test_a_card_that_refuses() {
  CutFs fs;
  TestCard card;
  fill(card);
  {
    // A session cut after its walk: walk.jnl written, never merged.
    Session s(fs, card);
    s.begin();
    s.jobs.askWalk();
    while (s.jobs.walkWork()) s.run(Job::Walk);
    TEST_ASSERT_TRUE(s.store.hasWalk());
  }
  {
    // The next boot, the card full: the boot's walk needs that walk merged,
    // and the compaction fails. Once: not handed again pass after pass
    // (each a LibraryWrite holding the idle power-off), and the walk (and
    // the scan after it, the firmware's order) waits for the next boot.
    Session s(fs, card);
    s.begin();
    fs.refuse = true;
    s.jobs.askWalk();
    TEST_ASSERT_FALSE(s.jobs.walkWork());
    TEST_ASSERT_TRUE(s.jobs.compactWork());
    TEST_ASSERT_FALSE(s.run(Job::Compact).compacted);
    for (int pass = 0; pass < 100; ++pass) {
      TEST_ASSERT_FALSE(s.jobs.compactWork());
      TEST_ASSERT_FALSE(s.jobs.walkWork());
    }
    TEST_ASSERT_EQUAL_UINT32(1, s.jobs.counts().compactionsFailed);
    // Asked again (the update step's, gr): it runs.
    s.jobs.askCompact();
    TEST_ASSERT_TRUE(s.jobs.compactWork());
    TEST_ASSERT_FALSE(s.run(Job::Compact).compacted);
    TEST_ASSERT_FALSE(s.jobs.compactWork());
    TEST_ASSERT_EQUAL_UINT32(2, s.jobs.counts().compactionsFailed);

    // The playing track, read: its record waits in the chunk. News for the
    // update step while the journal may still take it...
    const cj::Done& d = s.run(Job::Scan, Src::Playing, "Artist/Album/01 - a.flac");
    TEST_ASSERT_TRUE(d.read);
    TEST_ASSERT_TRUE(s.jobs.chunkPending());
    TEST_ASSERT_TRUE(s.jobs.newRecords());
    // ... the loop's flush: refused, kept; no news any more (an update now
    // would build the same index and find the track Pending again).
    const uint32_t opens = fs.opens;
    TEST_ASSERT_FALSE(s.jobs.idleFlush(s.now));
    TEST_ASSERT_EQUAL_UINT32(1, s.jobs.counts().appendFailures);
    TEST_ASSERT_TRUE(s.jobs.chunkPending());
    TEST_ASSERT_FALSE(s.jobs.newRecords());
    // Not tried again pass after pass: each try is an open and a write.
    const uint32_t opensAfterTry = fs.opens;
    TEST_ASSERT_TRUE(opensAfterTry >= opens);
    for (uint32_t k = 1; k < 30; ++k) TEST_ASSERT_FALSE(s.jobs.idleFlush(s.now + k * 1000));
    TEST_ASSERT_EQUAL_UINT32(1, s.jobs.counts().appendFailures);
    TEST_ASSERT_EQUAL_UINT32(opensAfterTry, fs.opens);
    // After Config::retryMs: tried again (refused again).
    TEST_ASSERT_FALSE(s.jobs.idleFlush(s.now + 30000));
    TEST_ASSERT_EQUAL_UINT32(2, s.jobs.counts().appendFailures);
    // The card takes writes again: the next try writes it, and that is news.
    fs.refuse = false;
    TEST_ASSERT_FALSE(s.jobs.idleFlush(s.now + 30000 + 29999));
    TEST_ASSERT_TRUE(s.jobs.idleFlush(s.now + 60000));
    TEST_ASSERT_FALSE(s.jobs.chunkPending());
    TEST_ASSERT_EQUAL_UINT32(1, s.jobs.counts().recorded);
    TEST_ASSERT_TRUE(s.jobs.newRecords());
    s.jobs.markRecords();  // the update step
    TEST_ASSERT_FALSE(s.jobs.newRecords());
  }
  {
    // A rest that ends because the card can't be read says so; one that
    // reaches D's end doesn't.
    Session s(fs, card);
    s.begin();
    bool ended = false, failed = true;
    while (s.jobs.restWork()) {
      const cj::Done& d = s.run(Job::Scan);
      if (d.restEnded) {
        ended = true;
        failed = d.restFailed;
      }
    }
    TEST_ASSERT_TRUE(ended);
    TEST_ASSERT_FALSE(failed);
    s.jobs.askRest();
    fs.dead = true;  // pulled
    const cj::Done& d = s.run(Job::Scan);
    TEST_ASSERT_TRUE(d.restEnded);
    TEST_ASSERT_TRUE(d.restFailed);
    TEST_ASSERT_FALSE(s.jobs.restWork());
    fs.dead = false;
  }
}

// ---------------------------------------------------------------------------
// The slices (2026-10-09). On the device a step of the walk was a third of a
// folder (entering it, listing it, leaving it: two of the three no I/O), and
// a step was handed once a loop pass: N11's 20k card took 7,774 hand-offs,
// each waiting out the loop's 20 ms sleep with the screen dark (2.7 min).
// Now a step is a folder's listing, and a hand-off runs them for up to
// 18 ms on the worker's clock (CardTasks' kSliceUs).
// ---------------------------------------------------------------------------
namespace {

std::string fixturesDir() {
  const char* tries[] = {"test/fixtures", "../test/fixtures", "../../test/fixtures"};
  for (const char* t : tries) {
    const std::string p = std::string(t) + "/synthcard/walk-shape.txt";
    if (FILE* f = std::fopen(p.c_str(), "rb")) {
      std::fclose(f);
      return t;
    }
  }
  std::string self = __FILE__;
  for (int up = 0; up < 2; ++up) self = self.substr(0, self.find_last_of("/\\"));
  return self + "/fixtures";
}

struct Shape {
  uint32_t folders = 0, audio = 0, images = 0, others = 0;
};

// N11's synthetic card as the walk saw it on the device (2026-10-08), its
// folders in pre-order with their files' counts; the names made up here.
Shape loadShape(TestCard& card) {
  const std::string path = fixturesDir() + "/synthcard/walk-shape.txt";
  FILE* f = std::fopen(path.c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, path.c_str());
  Shape sh;
  std::vector<std::string> at;  // the folder at each depth
  char line[128];
  uint32_t seed = 1;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned depth = 0, a = 0, i = 0, o = 0;
    if (line[0] == '#' || std::sscanf(line, "%u %u %u %u", &depth, &a, &i, &o) != 4) continue;
    std::string rel;
    if (depth > 0) {
      TEST_ASSERT_TRUE(depth <= at.size());
      const std::string& parent = at[depth - 1];
      rel = (parent.empty() ? "" : parent + "/") + "f" + std::to_string(sh.folders);
      card.tree.addFolder(rel);
    }
    at.resize(depth + 1);
    at[depth] = rel;
    const std::string pre = rel.empty() ? "" : rel + "/";
    for (unsigned k = 0; k < a; ++k) card.addNoise(pre + "a" + std::to_string(k) + ".mp3", 4000 + k, ++seed);
    for (unsigned k = 0; k < i; ++k) card.addNoise(pre + "c" + std::to_string(k) + ".jpg", 2000 + k, ++seed);
    for (unsigned k = 0; k < o; ++k) card.addNoise(pre + "o" + std::to_string(k) + ".txt", 100 + k, ++seed);
    ++sh.folders;
    sh.audio += a;
    sh.images += i;
    sh.others += o;
  }
  std::fclose(f);
  return sh;
}

}  // namespace

// The walk's steps and hand-offs on N11's card: the first boot's (no D:
// every file added, walk.jnl written), then an unchanged one's (D walked:
// every folder listed, its digest D's, nothing written).
void test_the_synthetic_cards_walk() {
  CutFs fs;
  TestCard card;
  const Shape sh = loadShape(card);
  TEST_ASSERT_EQUAL_UINT32(2591, sh.folders);  // /music included
  TEST_ASSERT_EQUAL_UINT32(19410, sh.audio);   // the device's tracks
  TEST_ASSERT_EQUAL_UINT32(1688, sh.images);
  TEST_ASSERT_EQUAL_UINT32(1570, sh.others);
  card.listUs = 3000;  // a listing: 3 ms on the worker's clock
  for (int boot = 0; boot < 2; ++boot) {
    Session s(fs, card);
    s.sliceUs = 18000;
    s.begin();
    TEST_ASSERT_EQUAL(boot == 1, s.store.device().present && s.store.device().header.walked);
    s.jobs.askWalk();
    uint32_t handOffs = 0, steps = 0;
    uint64_t longest = 0;
    cardwalk::CardWalk::Result r;
    while (s.jobs.walkWork()) {
      const uint64_t t0 = g_us;
      const cj::Done& d = s.run(Job::Walk);
      ++handOffs;
      steps += d.walkSteps;
      TEST_ASSERT_TRUE(d.walkSteps >= 1);
      if (g_us - t0 > longest) longest = g_us - t0;
      if (d.walkEnded) r = d.walk;
    }
    TEST_ASSERT_TRUE(r.state == cardwalk::CardWalk::State::Done);
    TEST_ASSERT_EQUAL_UINT32(sh.folders, r.folders);
    TEST_ASSERT_EQUAL_UINT32(sh.folders, r.listings);  // each fits the 64 KB scratch: one pass
    TEST_ASSERT_EQUAL_UINT32(sh.audio, r.audio);
    TEST_ASSERT_EQUAL_UINT32(sh.images, r.images);
    TEST_ASSERT_EQUAL_UINT32(sh.others, r.others);
    // A step a folder, then the walk's end and the doubts' pass (none):
    // 2,593, where the device took 7,774.
    TEST_ASSERT_EQUAL_UINT32(sh.folders + 2, r.steps);
    TEST_ASSERT_EQUAL_UINT32(r.steps, steps);
    // Six 3 ms listings a slice (a seventh would end at 21 ms): 432
    // hand-offs, none over the slice.
    TEST_ASSERT_TRUE(longest <= 18000);
    TEST_ASSERT_EQUAL_UINT32(432, handOffs);
    if (boot == 0) {
      TEST_ASSERT_EQUAL_UINT32(sh.audio, r.added);
      TEST_ASSERT_TRUE(r.summary.changed);
      s.jobs.askCompact();  // (the update step's, before its build)
      TEST_ASSERT_TRUE(s.run(Job::Compact).compacted);
    } else {
      // (Its folders with no audio at or below them, which D has no row
      // for, are merged each walk, and say nothing.)
      TEST_ASSERT_EQUAL_UINT32(0, r.added + r.changed + r.gone + r.foldersGone);
      TEST_ASSERT_FALSE(r.summary.changed);
      TEST_ASSERT_FALSE(s.store.hasWalk());  // an unchanged card writes nothing
    }
  }
  // The loop cuts a slice (a wait came): it ends after the listing under way.
  Session s(fs, card);
  s.sliceUs = 18000;
  s.begin();
  s.jobs.askWalk();
  s.run(Job::Walk);  // (its first: six folders)
  uint32_t lists = 0;
  card.onList = [&] {
    if (++lists == 2) s.jobs.cutSlice();
  };
  const cj::Done& d = s.run(Job::Walk);
  TEST_ASSERT_EQUAL_UINT32(2, d.walkSteps);
  card.onList = nullptr;
  TEST_ASSERT_EQUAL_UINT32(6, s.run(Job::Walk).walkSteps);  // the next isn't cut
  // No clock: a step a hand-off.
  CutFs fresh;
  Session one(fresh, card);
  one.begin();
  one.jobs.askWalk();
  TEST_ASSERT_EQUAL_UINT32(1, one.run(Job::Walk).walkSteps);
  TEST_ASSERT_EQUAL_UINT32(1, one.run(Job::Walk).walkSteps);
}

// The scan's rest a slice of files a hand-off, each file whole and listed
// for the loop (file()); a loop source's file a step of its own; the loop's
// cut after the file under way.
void test_slices_of_the_scan() {
  CutFs fs;
  TestCard card;
  for (int a = 0; a < 5; ++a)
    for (int t = 0; t < 8; ++t)
      card.addNoise("A" + std::to_string(a) + "/Album/" + std::to_string(t) + ".mp3", 3000 + 7 * t, 40 + 8 * a + t);
  constexpr uint32_t kFiles = 40;
  Session s(fs, card);
  s.sliceUs = 18000;
  s.begin();
  s.jobs.askWalk();
  while (s.jobs.walkWork()) s.run(Job::Walk);
  // A file's read costs 8 ms: two a slice (a third would end at 24 ms).
  card.openUs = 8000;
  std::map<std::string, int> seen;
  uint32_t steps = 0;
  while (s.jobs.restWork()) {
    const uint64_t t0 = g_us;
    const cj::Done& d = s.run(Job::Scan);
    ++steps;
    TEST_ASSERT_TRUE(g_us - t0 <= 18000);
    TEST_ASSERT_TRUE(d.files <= 2);
    for (uint32_t i = 0; i < d.files; ++i) {
      const cj::FileDone f = s.jobs.file(i);
      TEST_ASSERT_TRUE(f.read);
      ++seen[std::string(f.rel, f.relLength)];
    }
    if (d.files > 0) {
      // Done's fields are the last file's.
      const cj::FileDone last = s.jobs.file(d.files - 1);
      TEST_ASSERT_EQUAL_STRING(last.rel, d.rel);
      TEST_ASSERT_EQUAL(last.read, d.read);
      TEST_ASSERT_TRUE(d.handled);
    }
    TEST_ASSERT_EQUAL_STRING("", s.jobs.file(d.files).rel);  // (none past them)
  }
  TEST_ASSERT_EQUAL_UINT32(kFiles, seen.size());
  for (const auto& kv : seen) TEST_ASSERT_EQUAL_INT_MESSAGE(1, kv.second, kv.first.c_str());
  TEST_ASSERT_EQUAL_UINT32(kFiles / 2 + 1, steps);  // two a slice, then the rest's end
  TEST_ASSERT_EQUAL_UINT32(kFiles, s.jobs.counts().scanned);
  // 1 ms a file: 18 a slice (a 19th would end at 19 ms); the rest's end in
  // the last.
  card.openUs = 1000;
  s.jobs.askRest(cj::Mode::All);  // (gr!: every file again)
  std::vector<uint32_t> files;
  while (s.jobs.restWork()) files.push_back(s.run(Job::Scan).files);
  TEST_ASSERT_EQUAL_UINT32(3, files.size());
  TEST_ASSERT_EQUAL_UINT32(18, files[0]);
  TEST_ASSERT_EQUAL_UINT32(18, files[1]);
  TEST_ASSERT_EQUAL_UINT32(kFiles - 36, files[2]);
  // The dark's 60 ms slice (setSlice(), between steps): the list holds
  // kSliceFiles, however much of the slice is left.
  s.jobs.setSlice(60000);
  s.jobs.askRest(cj::Mode::All);
  files.clear();
  while (s.jobs.restWork()) files.push_back(s.run(Job::Scan).files);
  TEST_ASSERT_EQUAL_UINT32(2, files.size());
  TEST_ASSERT_EQUAL_UINT32(cj::kSliceFiles, files[0]);
  TEST_ASSERT_EQUAL_UINT32(kFiles - cj::kSliceFiles, files[1]);
  // The loop's cut, during the third file: the slice ends after it.
  s.jobs.askRest(cj::Mode::All);
  uint32_t opened = 0;
  card.onOpen = [&] {
    if (++opened == 3) s.jobs.cutSlice();
  };
  TEST_ASSERT_EQUAL_UINT32(3, s.run(Job::Scan).files);
  card.onOpen = nullptr;
  TEST_ASSERT_EQUAL_UINT32(cj::kSliceFiles, s.run(Job::Scan).files);  // the next isn't cut
  // gr or gv during a slice (askRest()): it ends after its unit too (the rest
  // of it would go on in the new mode from the middle of the View), and the
  // next starts over from D's first row.
  s.jobs.askRest(cj::Mode::All);
  opened = 0;
  card.onOpen = [&] {
    if (++opened == 3) s.jobs.askRest(cj::Mode::All);
  };
  TEST_ASSERT_EQUAL_UINT32(3, s.run(Job::Scan).files);
  card.onOpen = nullptr;
  const cj::Done& again = s.run(Job::Scan);
  TEST_ASSERT_EQUAL_UINT32(cj::kSliceFiles, again.files);
  TEST_ASSERT_EQUAL_STRING("A0/Album/0.mp3", s.jobs.file(0).rel);  // (D's first row)
  // A file the loop names: a step of its own, whatever the slice.
  const cj::Done& d = s.run(Job::Scan, Src::Playing, "A2/Album/5.mp3");
  TEST_ASSERT_EQUAL_UINT32(1, d.files);
  TEST_ASSERT_EQUAL_STRING("A2/Album/5.mp3", s.jobs.file(0).rel);
  TEST_ASSERT_EQUAL_STRING("A2/Album/5.mp3", d.rel);
  card.openUs = 0;
}

// ---------------------------------------------------------------------------
// The jobs begun again after FatFs mounted the same card again (main.cpp's
// stepCardGuard, the card pulled and put back while nothing ran): the
// records this session put in the journal are still news for the update
// step, and an update step's mark stays. (2026-10-09's review: begin() took
// the mark again, so the records a scan appended before the remount asked
// for no update step this session.)
// ---------------------------------------------------------------------------
void test_begun_again_after_a_remount() {
  CutFs fs;
  TestCard card;
  fill(card);
  Session s(fs, card);
  s.begin();
  s.jobs.askWalk();
  s.drain();  // the walk, its merge, the rest: every file read
  TEST_ASSERT_EQUAL_UINT32(kAudio, s.jobs.counts().scanned);
  TEST_ASSERT_TRUE(s.jobs.newRecords());
  s.jobs.markRecords();  // the update step ran
  s.begin();             // the remount
  TEST_ASSERT_FALSE(s.jobs.newRecords());  // not asked again for the same records
  // (The rest looks at the View again and reads nothing: the journal has
  // every file's record.)
  const uint32_t scanned = s.jobs.counts().scanned;
  s.drain();
  TEST_ASSERT_EQUAL_UINT32(scanned, s.jobs.counts().scanned);
  TEST_ASSERT_FALSE(s.jobs.newRecords());
  // The playing track read again, its chunk out; then the remount.
  const cj::Done& d = s.run(Job::Scan, Src::Playing, "Artist/Album/01 - a.flac");
  TEST_ASSERT_TRUE(d.read);
  TEST_ASSERT_TRUE(s.jobs.flushChunk());
  TEST_ASSERT_TRUE(s.jobs.newRecords());
  s.begin();
  TEST_ASSERT_TRUE(s.jobs.newRecords());  // still news
  // A chunk that waits across it: kept, and news.
  s.run(Job::Scan, Src::Playing, "Artist/Album/02 - b.opus");
  TEST_ASSERT_TRUE(s.jobs.chunkPending());
  s.jobs.markRecords();
  s.begin();
  TEST_ASSERT_TRUE(s.jobs.chunkPending());
  TEST_ASSERT_TRUE(s.jobs.newRecords());
  TEST_ASSERT_TRUE(s.jobs.flushChunk());
  s.jobs.markRecords();
  TEST_ASSERT_FALSE(s.jobs.newRecords());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_boots_decision);
  RUN_TEST(test_the_root);
  RUN_TEST(test_a_hand_filled_card);
  RUN_TEST(test_new_to_the_index);
  RUN_TEST(test_the_loops_sources);
  RUN_TEST(test_chunks_and_a_full_journal);
  RUN_TEST(test_changed_files_and_a_rescan);
  RUN_TEST(test_a_transfer_card);
  RUN_TEST(test_a_bad_transfer_is_walked_without);
  RUN_TEST(test_a_card_that_refuses);
  RUN_TEST(test_the_synthetic_cards_walk);
  RUN_TEST(test_slices_of_the_scan);
  RUN_TEST(test_begun_again_after_a_remount);
  return UNITY_END();
}
