// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for LibraryUpdate (docs/METADATA.md 3.2.2, 3.4.2; milestone
// N12): the boot's decision with the card's real files (every row of 3.2.2's
// table, library.tmp left by a cut, the build-at-boot marker) and the update
// step pass by pass, as the firmware's loop runs it (like test_idle_policy):
// the scheduler (ScanScheduler), the card worker's jobs (CardJobs) on fake
// FAT trees with tagged files from the corpus, the device's records and
// library.idx on a card that can lose its power at any write (CutFs), a
// worker whose steps take several passes, and the loop's readers (a
// TrackCatalog as the firmware sets it up). The safe point near a track's
// end; a deferral, then the boot that builds; a compaction asked during a
// build; a T found bad at the end of a build; a power cut at every step of
// an update; the fence (no reader sees a half-built index); a card that
// fails while the build reads it; the worker's task before the fence; the
// marker's build out of PSRAM at the boot. What a track that ends inside
// the fence does is test_gapless_player's.
// Run: pio test -e native -f test_library_update
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
#include "LibraryUpdate.h"
#include "ScanScheduler.h"
#include "TagStore.h"
#include "TrackCatalog.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace ts = tagstore;
namespace cj = cardjobs;
using cutfs::CutFs;
using Job = ScanScheduler::Job;
using Do = LibraryUpdate::Do;
using Phase = LibraryUpdate::Phase;

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
  std::string self = __FILE__;
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

// ---- the card's music: FakeFat's tree, real bytes for some files ----
class TestCard : public cj::Card {
public:
  fakefat::Card tree;
  std::map<std::string, std::vector<uint8_t>> bytes;

  void addBytes(const std::string& rel, const std::vector<uint8_t>& b, uint32_t fatTime = kT) {
    bytes[rel] = b;
    tree.addFile(rel, static_cast<uint32_t>(b.size()), fatTime, 1);
  }
  void addNoise(const std::string& rel, uint32_t size, uint32_t seed) {
    bytes.erase(rel);
    tree.addFile(rel, size, kT, seed);
  }
  Open openDir(const char* rel, size_t len) override { return tree.openDir(rel, len); }
  Next next(cardwalk::Entry* out) override { return tree.next(out); }
  void closeDir() override { tree.closeDir(); }
  cc::Source* openFile(const char* rel, size_t len) override {
    const std::string p(rel, len);
    auto it = bytes.find(p);
    if (it == bytes.end()) return tree.openFile(rel, len);
    if (!tree.files.count(p)) return nullptr;
    mem_.reset(new cc::MemSource(it->second.data(), static_cast<uint32_t>(it->second.size())));
    return mem_.get();
  }
  void closeFile() override { mem_.reset(); }
  bool stat(const char* rel, size_t len, uint32_t* size, uint32_t* fatTime) override {
    auto it = tree.files.find(std::string(rel, len));
    if (it == tree.files.end()) return false;
    *size = it->second.size;
    *fatTime = it->second.fatTime;
    return true;
  }

private:
  std::unique_ptr<cc::MemSource> mem_;
};

// Four tagged files (FLAC, Opus, two MP3s), two of noise named .mp3, a
// cover and a text file.
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
const char* const kFlac = "/music/Artist/Album/01 - a.flac";
const char* const kFlacTitle = "Lantern Song";  // its tag (the path names it "a")

// ---- the transfer ----
std::vector<uint8_t> transferOf(const TestCard& c, uint32_t gen) {
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

std::vector<uint8_t> manifestFor(const std::vector<uint8_t>& t, uint32_t gen, uint64_t commitId) {
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
  struct V : cc::Sink {
    bool write(uint32_t offset, const void* data, uint32_t n) override {
      if (offset + n > bytes.size()) bytes.resize(offset + n);
      if (n) std::memcpy(bytes.data() + offset, data, n);
      return true;
    }
    std::vector<uint8_t> bytes;
  } out;
  uint32_t fileBytes = 0, headerCrc = 0;
  const char* error = nullptr;
  if (!cc::msmf::write(out, m, &fileBytes, &headerCrc, &error)) TEST_FAIL_MESSAGE(error);
  return out.bytes;
}

std::string tagsName(uint32_t gen) {
  char n[32];
  cc::msmf::companionName(cc::kMagicMptg, gen, n, sizeof(n));
  return std::string("/.mstream/") + n;
}

// A transfer committed to the card (T and its root).
void commit(CutFs& fs, const TestCard& card, uint32_t gen) {
  const std::vector<uint8_t> t = transferOf(card, gen);
  fs.put(tagsName(gen), t);
  fs.put("/.mstream/manifest.bin", manifestFor(t, gen, 0xC0FFEE00ull + gen));
}

// T with one letter of a record's title changed (STRS): the frame and the
// header still check (the root takes it, its identity unchanged), the
// section's CRC doesn't, which a reader learns at T's end.
void spoilStrings(CutFs& fs, uint32_t gen) {
  std::vector<uint8_t> t = fs.bytes(tagsName(gen));
  const char* title = "From T";
  auto at = std::search(t.begin(), t.end(), title, title + 6);
  TEST_ASSERT_TRUE(at != t.end());
  at[1] = 'q';
  fs.put(tagsName(gen), t);
}

// ---- the card's files, with an eye on every read (the fence test) ----
std::function<void()> g_onRead;
std::function<void()> g_onAlloc;
// A read of `path` that fails (a contact glitch): true fails it.
std::function<bool(const std::string& path)> g_failRead;

class SpyFs : public ts::Fs {
public:
  explicit SpyFs(CutFs& inner) : in_(inner) {}
  class F : public ts::File {
  public:
    F(ts::File* f, const char* path) : f_(f), path_(path) {}
    uint32_t size() const override { return f_->size(); }
    bool read(uint32_t offset, void* out, uint32_t n) override {
      if (g_onRead) g_onRead();
      if (g_failRead && g_failRead(path_)) return false;
      return f_->read(offset, out, n);
    }
    bool write(uint32_t offset, const void* data, uint32_t n) override { return f_->write(offset, data, n); }
    bool sync() override { return f_->sync(); }
    bool truncate(uint32_t size) override { return f_->truncate(size); }
    ts::File* f_;
    std::string path_;
  };
  ts::File* open(const char* path, Mode mode) override {
    ts::File* f = in_.open(path, mode);
    return f ? new F(f, path) : nullptr;
  }
  bool close(ts::File* file) override {
    F* f = static_cast<F*>(file);
    const bool ok = in_.close(f->f_);
    delete f;
    return ok;
  }
  bool exists(const char* path) override { return in_.exists(path); }
  bool remove(const char* path) override { return in_.remove(path); }
  bool rename(const char* from, const char* to) override { return in_.rename(from, to); }
  uint32_t firstCluster(const char* path) override { return in_.firstCluster(path); }

private:
  CutFs& in_;
};

// The index's memory (the firmware's PSRAM), refused past a ceiling when
// one is set, its allocations seen by the fence test.
size_t g_ceiling = SIZE_MAX;
size_t g_live = 0;
std::map<void*, size_t> g_blocks;
void* testAlloc(size_t n) {
  if (g_onAlloc) g_onAlloc();
  if (g_live + n > g_ceiling) return nullptr;
  void* p = std::malloc(n ? n : 1);
  g_blocks[p] = n;
  g_live += n;
  return p;
}
void testFree(void* p) {
  if (!p) return;
  auto it = g_blocks.find(p);
  if (it != g_blocks.end()) {
    g_live -= it->second;
    g_blocks.erase(it);
  }
  std::free(p);
}

// No records at all: /music walked into the index, every file Pending (the
// firmware's VFS walk).
int g_walks = 0;
bool walkTree(LibraryIndex& idx, void* ctx) {
  const TestCard& card = *static_cast<const TestCard*>(ctx);
  ++g_walks;
  idx.clear();
  if (!idx.begin("/music", 0)) return false;
  for (const auto& kv : card.tree.files) idx.addFile(("/music/" + kv.first).c_str(), LibraryIndex::kAddPending);
  return idx.finish();
}

ts::TagStore::Config storeConfig() {
  ts::TagStore::Config c;
  c.producer = "mstream-player test";
  c.parserVersion = tagscan::kParserVersion;
  return c;
}

std::string titleOf(const LibraryIndex& idx, const char* path) {
  const uint32_t t = idx.findTrack(path);
  if (t == LibraryIndex::kNone) return "<none>";
  uint8_t n = 0;
  const char* s = idx.trackTitle(t, &n);
  return std::string(s, n);
}

uint32_t pendingIn(const LibraryIndex& idx) {
  uint32_t n = 0;
  for (uint32_t t = 0; t < idx.trackCount(); ++t) n += (idx.track(t).flags & LibraryIndex::kTrackPending) ? 1 : 0;
  return n;
}

// What the loop gives the update step besides the card: the player, the
// memory.
struct Env {
  bool playing = false;
  bool waiting = false;
  uint32_t trackLeftMs = 0;
  uint32_t seekSeq = 0;
  size_t psramFree = 64u << 20;
  size_t psramLargest = 64u << 20;
  size_t alsoFreed = 0;
};

// One boot of the device: the store over the card (its recovery done), the
// root, the index, the update step, the jobs, the scheduler, the worker, and
// the loop's readers (the catalog as main.cpp sets it up around the fence).
struct Device {
  CutFs& cut;
  SpyFs fs;
  TestCard& card;
  std::unique_ptr<ts::TagStore> store;
  std::unique_ptr<cardroot::Root> root;
  LibraryIndex index{testAlloc, testFree};
  std::unique_ptr<LibraryUpdate> upd;
  LibraryUpdate::Booted booted;
  cj::Jobs jobs;
  ScanScheduler sched;
  uint32_t now = 1000;
  // The worker: one step at a time, each `passes` loop passes long (the
  // build `buildPasses`): the step runs when it is handed, and is taken in
  // when its passes are over, as CardWorker's poll() does.
  Job running = Job::None;
  int left = 0;
  int buildPasses = 5;
  std::vector<Job> steps;
  bool lastCompactFailed = false;
  // The loop's readers.
  TrackCatalog catalog;
  TrackCatalog::Held held;
  std::vector<Do> acts;
  LibraryUpdate::Out lastOut;
  bool failFence = false;  // the loop can't put the fence up (no PSRAM to carry the queue)
  // The worker's task (CardWorker): there, or made when the step wants it
  // (Out::wantWorker) unless there's no internal RAM for its stack.
  bool workerUp = true;
  bool canMakeWorker = true;

  Device(CutFs& c, TestCard& t) : cut(c), fs(c), card(t), jobs(nullptr, nullptr) {
    index.keepTrackBlock(true);
    // /music's own entry (FatFs answers for the folder: the step asks
    // whether the card is still there).
    if (!cut.dead && !cut.exists("/music")) cut.put("/music", {});
  }

  void boot() {
    store.reset(new ts::TagStore(fs, storeConfig()));
    store->open();
    root.reset(new cardroot::Root());
    std::vector<uint8_t> scratch(1024);
    cardroot::read(fs, scratch.data(), static_cast<uint32_t>(scratch.size()), root.get());
    LibraryUpdate::Config c;
    c.fs = &fs;
    c.store = store.get();
    c.root = root.get();
    c.index = &index;
    c.alloc = testAlloc;
    c.release = testFree;
    c.walk = walkTree;
    c.walkCtx = &card;
    upd.reset(new LibraryUpdate(c));
    booted = upd->boot();
    catalog.setIndex(&index);
    beginJobs();
  }
  void beginJobs() {
    cj::Config c;
    c.store = store.get();
    c.card = &card;
    c.fs = &fs;
    const bool useT = root->present && !upd->transferBad();
    c.transferPath = useT ? root->tagsPath : nullptr;
    c.root = useT ? root->identity : ts::Identity();
    c.rowsPerStep = 3;
    jobs.begin(c);
  }

  // One loop pass, as app/CardTasks and main.cpp's stepCard() run it.
  void pass(const Env& e = Env()) {
    now += 20;
    if (running != Job::None && --left <= 0) taken();
    LibraryUpdate::In ui;
    ui.nowMs = now;
    ui.workerFree = running == Job::None;
    ui.workerUp = workerUp;
    ui.walking = jobs.walking();
    ui.journals = store->hasJournals() || jobs.chunkPending();
    ui.compactFailed = lastCompactFailed;
    ui.playing = e.playing;
    ui.waiting = e.waiting;
    ui.trackLeftMs = e.trackLeftMs;
    ui.seekSeq = e.seekSeq;
    ui.psramFree = e.psramFree;
    ui.psramLargest = e.psramLargest;
    ui.alsoFreed = e.alsoFreed;
    const LibraryUpdate::Out uo = upd->update(ui);
    lastOut = uo;
    if (uo.wantWorker && canMakeWorker) workerUp = true;  // (CardWorker::ensure())
    if (uo.act != Do::None) acts.push_back(uo.act);
    if (uo.act == Do::Fence && failFence) {
      upd->cantFence();
    } else if (uo.act == Do::Fence) {
      // Steps 1-3 (the queue's would go here): the playing track's names
      // kept, the catalog's index gone, then the old index cleared.
      const uint32_t playing = index.ready() && index.trackCount() ? index.findTrack(kFlac) : LibraryIndex::kNone;
      catalog.take(playing, &held);
      catalog.setHeld(&held);
      catalog.setIndex(nullptr);  // (Library::fence())
      upd->fencedUp();
      TEST_ASSERT_NULL(upd->readable());
    } else if (uo.act == Do::Live) {
      TEST_ASSERT_NULL(upd->readable());  // (until lived())
      upd->lived();
      catalog.setIndex(upd->readable());
      catalog.setHeld(nullptr);
      jobs.libraryRebuilt();
      jobs.markRecords();
    }
    ScanScheduler::In in;
    in.nowMs = now;
    in.running = running;
    if (running == Job::None) {
      in.walk = (!uo.holdScan || jobs.walking()) && jobs.walkWork();
      const bool before = uo.compact;
      in.compact = (jobs.compactWork() || before) && !(before && lastCompactFailed);
      in.restPending = !uo.holdScan && !in.walk && !in.compact && jobs.restWork();
    }
    in.build = uo.build;
    in.save = uo.save;
    in.updating = uo.updating;
    in.playing = e.playing;
    const ScanScheduler::Out o = sched.update(in);
    if (o.job != Job::None) start(o.job);
  }
  void start(Job j) {
    TEST_ASSERT_TRUE(workerUp || j != Job::Build);  // the build is handed only to a task that is there
    running = j;
    left = j == Job::Build ? buildPasses : 1;
    steps.push_back(j);
    switch (j) {
      case Job::Build:
        upd->buildStarted();
        upd->stepBuild();
        break;
      case Job::Save:
        upd->saveStarted();
        upd->stepSave();
        break;
      case Job::Compact:
      case Job::Walk:
      case Job::Scan:
        TEST_ASSERT_TRUE(jobs.prepare(j, ScanScheduler::Source::Rest, nullptr, 0, now));
        jobs.step();
        break;
      default: TEST_FAIL_MESSAGE("a job this rig doesn't run");
    }
  }
  void taken() {
    const Job j = running;
    running = Job::None;
    switch (j) {
      case Job::Build: upd->buildDone(); break;
      case Job::Save: upd->saveDone(); break;
      case Job::Compact: {
        const cj::Done& d = jobs.finish();
        lastCompactFailed = !d.compacted;
        break;
      }
      default: jobs.finish(); break;
    }
  }
  // Passes until `until`, at most `max`.
  void runUntil(const std::function<bool()>& until, const Env& e = Env(), int max = 100000) {
    for (int i = 0; i < max; ++i) {
      if (until()) return;
      pass(e);
    }
    TEST_FAIL_MESSAGE("never got there");
  }
  // The update step asked and run to its end (Do::Saved, Deferred or Failed).
  void update(const char* why = "test", const Env& e = Env(), bool defer = false) {
    upd->ask(why, defer);
    const size_t from = acts.size();
    runUntil(
        [&] {
          for (size_t i = from; i < acts.size(); ++i)
            if (acts[i] == Do::Saved || acts[i] == Do::Deferred || acts[i] == Do::Failed) return true;
          return false;
        },
        e);
    // (the pass that said so ran no step: the worker is free)
  }
  // The background work (the walk, compactions, the scan) to its end.
  void drain(const Env& e = Env()) {
    runUntil([&] { return running == Job::None && !jobs.walkWork() && !jobs.compactWork() && !jobs.restWork(); }, e);
  }
  bool did(Do a) const { return std::find(acts.begin(), acts.end(), a) != acts.end(); }
};

}  // namespace

void setUp() {}
// (A test that failed mid-way leaves no hook behind for the next.)
void tearDown() {
  g_onRead = nullptr;
  g_onAlloc = nullptr;
  g_failRead = nullptr;
  g_ceiling = SIZE_MAX;
}

namespace {

// A hand-filled card, booted once (its walk: every track by its path,
// Pending), then walked and scanned in the background: the journal holds
// every file's reading, and the loaded index none of it.
void handFilled(CutFs& fs, TestCard& card) {
  fill(card);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.ok);
  TEST_ASSERT_TRUE(d.booted.walked);
  d.jobs.askWalk();
  d.drain();
  TEST_ASSERT_TRUE(d.store->hasJournals() || d.store->device().present);
}

// A transfer card: committed, booted once (built from T), then walked and
// drained (D's rows for T's files).
void transferCard(CutFs& fs, TestCard& card) {
  fill(card);
  commit(fs, card, 1);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.build.transferUsed);
  d.jobs.askWalk();
  d.drain();
}

}  // namespace

// ---------------------------------------------------------------------------
// The pure parts: the safe point and the memory check (3.4.2).
// ---------------------------------------------------------------------------
void test_the_safe_point_and_the_memory_check() {
  using U = LibraryUpdate;
  // Nothing plays: safe; a wait for the headphones isn't (its start needs a path).
  TEST_ASSERT_TRUE(U::safePoint(false, false, 0, 0));
  TEST_ASSERT_FALSE(U::safePoint(false, true, 999999, UINT32_MAX));
  // Playing: 20 s left at least, and no seek in the last 2 s.
  TEST_ASSERT_TRUE(U::safePoint(true, false, 20000, U::kSeekQuietMs));
  TEST_ASSERT_FALSE(U::safePoint(true, false, 19999, UINT32_MAX));
  TEST_ASSERT_FALSE(U::safePoint(true, false, 0, UINT32_MAX));  // a length not known
  TEST_ASSERT_FALSE(U::safePoint(true, false, 200000, U::kSeekQuietMs - 1));

  // The room: free + the index + what else goes, at least 1.1 x the peak.
  U::Room r;
  r.indexBytes = 1000000;
  r.trackBlock = 200000;
  r.tableBytes = 200000;
  const size_t peak = 1000000 + 125000 + 96 * 1024;
  TEST_ASSERT_EQUAL_size_t(peak, U::roomToBuild(r).peak);
  const size_t need = (peak * 11 + 9) / 10;  // the smallest room that passes
  r.psramFree = need - r.indexBytes;
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::None);
  r.psramFree -= 1;
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::Room);
  r.alsoFreed = 1;  // the queue's, Thumbs' pools: counted
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::None);
  // The table: in the old one's block, or the largest free one with a
  // sixteenth to spare.
  r.psramFree = 64u << 20;
  r.tableBytes = 200001;
  r.psramLargest = 200001 + 200001 / 16 - 1;
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::Table);
  r.psramLargest += 1;
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::None);
  r.psramLargest = 0;
  r.tableBytes = 200000;  // fits the old block
  TEST_ASSERT_TRUE(U::roomToBuild(r).shortOf == U::Short::None);

  // What the step can spare (the queue's text in PSRAM held through the
  // build, when the card can't take queue.txt): held, the check still
  // passes; a byte more, it doesn't.
  U::Room s;
  s.indexBytes = 1000000;
  s.trackBlock = 200000;
  s.tableBytes = 200000;
  s.psramFree = need - s.indexBytes;  // the check's edge: nothing to spare
  TEST_ASSERT_EQUAL_size_t(0, U::spareOf(s));
  s.psramFree += 5000;
  TEST_ASSERT_EQUAL_size_t(5000, U::spareOf(s));
  U::Room held = s;
  held.psramFree -= U::spareOf(s);
  TEST_ASSERT_TRUE(U::roomToBuild(held).shortOf == U::Short::None);
  held.psramFree -= 1;
  TEST_ASSERT_TRUE(U::roomToBuild(held).shortOf == U::Short::Room);
  s.psramFree -= 6000;  // short already
  TEST_ASSERT_EQUAL_size_t(0, U::spareOf(s));
  // The table in a free block of its own: what that block can lose, taken
  // as if the held bytes came out of it.
  s.psramFree = 64u << 20;
  s.tableBytes = 300000;
  s.psramLargest = 300000 + 300000 / 16 + 700;
  TEST_ASSERT_EQUAL_size_t(700, U::spareOf(s));
  held = s;
  held.psramFree -= 700;
  held.psramLargest -= 700;
  TEST_ASSERT_TRUE(U::roomToBuild(held).shortOf == U::Short::None);
  held.psramLargest -= 1;
  TEST_ASSERT_TRUE(U::roomToBuild(held).shortOf == U::Short::Table);
}

// ---------------------------------------------------------------------------
// The boot (3.2.2): every row of the table, with the card's real files.
// ---------------------------------------------------------------------------
void test_every_row_of_the_boot() {
  using libraryboot::Action;
  using libraryboot::Saved;
  CutFs fs;
  TestCard card;
  // No records, nothing saved (a card-reader card, this firmware's first
  // boot): /music walked, every file Pending, saved with no transfer.
  fill(card);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Missing);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.walked);
    TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
    TEST_ASSERT_EQUAL_UINT32(kAudio, pendingIn(d.index));
    TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kIndexNames.path));
    // The walk and the scan in the background: D, and the journal.
    d.jobs.askWalk();
    d.drain();
  }
  // v6, matching, no marker: loaded; the scan went on since its build (the
  // journal): soft-stale, rebuilt at the scan's end.
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Matches);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_TRUE(d.booted.softStale);
    TEST_ASSERT_EQUAL_STRING("a", titleOf(d.index, kFlac).c_str());  // (its path's name)
    // The scan's end's update step: the tags in.
    d.update("the scan's end");
    TEST_ASSERT_TRUE(d.upd->last().built && d.upd->last().saved);
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
    TEST_ASSERT_EQUAL_UINT32(0, pendingIn(d.index));
  }
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_FALSE(d.booted.softStale);
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
  // v6, matching, the marker set (a build deferred): built from the
  // records, the journals compacted first, the marker gone after.
  fs.put(LibraryUpdate::kMarker, {});
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.marker);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Matches);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_TRUE(d.booted.built);
    TEST_ASSERT_TRUE(d.booted.markerRemoved);
    TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
  // v6, another identity (a transfer happened): built from T and D.
  commit(fs, card, 2);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Differs);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_TRUE(d.booted.build.transferUsed);
    TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  }
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);  // matching now
  }
  // ... /.mstream gone: another identity again; from D alone.
  fs.remove("/.mstream/manifest.bin");
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Differs);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_FALSE(d.booted.build.transferUsed);
    TEST_ASSERT_TRUE(d.booted.build.deviceUsed);
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
  // An older version (v5), unreadable, missing: with records, built.
  for (int k = 0; k < 3; ++k) {
    std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
    if (k == 0) idx[4] = 5;                       // the version word: 5
    if (k == 1) idx.assign(idx.size(), 0x5A);     // not an index
    if (k == 2) {
      fs.remove(LibraryUpdate::kIndexNames.path);
    } else {
      fs.put(LibraryUpdate::kIndexNames.path, idx);
    }
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.saved == (k == 0 ? Saved::Outdated : k == 1 ? Saved::Corrupt : Saved::Missing));
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_TRUE(d.booted.built);
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
  // No records (D gone, no transfer): an older version, unreadable or none
  // walks /music.
  for (const char* p : {"/.player/tags.bin", "/.player/tags.jnl", "/.player/walk.jnl"})
    if (fs.exists(p)) fs.remove(p);
  for (int k = 0; k < 3; ++k) {
    std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
    if (k == 0) idx[4] = 5;
    if (k == 1) idx.assign(idx.size(), 0x5A);
    if (k == 2) {
      fs.remove(LibraryUpdate::kIndexNames.path);
    } else {
      fs.put(LibraryUpdate::kIndexNames.path, idx);
    }
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.walked);
    TEST_ASSERT_EQUAL_UINT32(kAudio, pendingIn(d.index));
  }
  // NoMemory: an index that can't be loaded for PSRAM: no library (the
  // built-in tracks still play).
  {
    Device d(fs, card);
    g_ceiling = g_live + 64;
    d.boot();
    g_ceiling = SIZE_MAX;
    TEST_ASSERT_FALSE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.noMemory);
    TEST_ASSERT_FALSE(d.index.ready());
  }
}

// ---------------------------------------------------------------------------
// library.tmp left by a cut (2.12.6): whole with library.idx gone (the cut
// fell between the remove and the rename), it is library.idx; a torn one
// next to library.idx is removed.
// ---------------------------------------------------------------------------
void test_library_tmp_after_a_cut() {
  CutFs fs;
  TestCard card;
  fill(card);
  {
    Device d(fs, card);
    d.boot();
  }
  const std::vector<uint8_t> whole = fs.bytes(LibraryUpdate::kIndexNames.path);
  fs.remove(LibraryUpdate::kIndexNames.path);
  fs.put(LibraryUpdate::kIndexNames.tmp, whole);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.tmpSettled.what == ts::Settle::Promoted);
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kIndexNames.tmp));
  }
  std::vector<uint8_t> torn(whole.begin(), whole.begin() + whole.size() / 2);
  fs.put(LibraryUpdate::kIndexNames.tmp, torn);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.tmpSettled.what == ts::Settle::Removed);
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kIndexNames.tmp));
  }
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// ---------------------------------------------------------------------------
// A deferral (the memory check fails, or gb! asks for one), then the boot
// that builds, on a fresh heap, before the UI.
// ---------------------------------------------------------------------------
void test_a_deferral_then_a_boot_that_builds() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  for (int forced = 0; forced < 2; ++forced) {
    {
      Device d(fs, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
      const uint32_t tracks = d.index.trackCount();
      Env e;
      if (!forced) e.psramFree = e.psramLargest = 1024;  // short
      d.update(forced ? "gb!" : "the scan's end", e, forced != 0);
      TEST_ASSERT_TRUE(d.did(Do::Deferred));
      TEST_ASSERT_FALSE(d.did(Do::Fence));
      TEST_ASSERT_TRUE(d.upd->last().markerWritten);
      TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
      TEST_ASSERT_EQUAL(forced != 0, d.upd->deferForced());
      TEST_ASSERT_TRUE(forced ? d.upd->verdict().shortOf == LibraryUpdate::Short::None
                              : d.upd->verdict().shortOf == LibraryUpdate::Short::Room);
      // The index as it was, the step over: nothing held.
      TEST_ASSERT_TRUE(d.index.ready());
      TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
      TEST_ASSERT_TRUE(d.upd->phase() == Phase::Idle);
      d.pass();
      TEST_ASSERT_FALSE(d.lastOut.holdScan || d.lastOut.updating || d.lastOut.libraryWrite);
      TEST_ASSERT_EQUAL_UINT32(1, d.upd->deferrals());
      // (the compaction it asked for first ran: the journals are in D)
      TEST_ASSERT_FALSE(d.store->hasJournals());
    }
    // The next boot: the marker builds before the UI, and goes.
    {
      Device d(fs, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.marker);
      TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Build);
      TEST_ASSERT_TRUE(d.booted.markerRemoved);
      TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
      TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
    }
    {
      Device d(fs, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
      TEST_ASSERT_FALSE(d.booted.softStale);
    }
    // (Spoil it again for the next round: the scan's records are in, so a
    // Rescan makes them news.)
    Device d(fs, card);
    d.boot();
    d.jobs.askCompact(true);
    d.jobs.askRest();
    d.drain();
  }
  // No PSRAM to carry the queue across (the loop can't put the fence up):
  // deferred to the boot the same way, the index as it was.
  {
    Device d(fs, card);
    d.boot();
    const uint32_t tracks = d.index.trackCount();
    d.failFence = true;
    d.update("no carry");
    TEST_ASSERT_TRUE(d.did(Do::Fence));
    TEST_ASSERT_TRUE(d.did(Do::Deferred));
    TEST_ASSERT_TRUE(d.upd->verdict().shortOf == LibraryUpdate::Short::Carry);
    TEST_ASSERT_FALSE(d.upd->deferForced());
    TEST_ASSERT_TRUE(d.upd->last().markerWritten);
    TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
    TEST_ASSERT_TRUE(d.index.ready());
    TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
    TEST_ASSERT_NOT_NULL(d.upd->readable());
    d.pass();
    TEST_ASSERT_FALSE(d.lastOut.updating || d.lastOut.libraryWrite || d.lastOut.holdScan);
    fs.remove(LibraryUpdate::kMarker);
  }
  // A deferral, then memory: the same session's next step builds, and the
  // marker goes with its save.
  {
    Device d(fs, card);
    d.boot();
    Env shortOf;
    shortOf.psramFree = shortOf.psramLargest = 1024;
    d.update("short", shortOf);
    TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
    d.update("again");
    TEST_ASSERT_TRUE(d.upd->last().saved);
    TEST_ASSERT_TRUE(d.upd->last().markerRemoved);
    TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
  }
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// ---------------------------------------------------------------------------
// The safe point near a track's end: with 20 s left the step starts; with
// less it waits for the next track (its length known), and 2 s after a seek.
// A wait for the headphones holds it too.
// ---------------------------------------------------------------------------
void test_a_track_end_near_the_safe_point() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  d.upd->ask("the scan's end");
  Env e;
  e.playing = true;
  e.trackLeftMs = 19999;
  // The compaction first (it doesn't wait for the safe point: it writes
  // nothing the decoder reads).
  d.runUntil([&] { return !d.store->hasJournals() && d.running == Job::None; }, e);
  for (int i = 0; i < 50; ++i) {
    d.pass(e);
    TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
    TEST_ASSERT_TRUE(d.lastOut.holdScan);
    TEST_ASSERT_FALSE(d.lastOut.updating);
    e.trackLeftMs -= 20;  // it plays on
  }
  // Its end, then the next track's first moments: no length known yet (0).
  e.trackLeftMs = 0;
  d.pass(e);
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  // A seek in the next track: 2 s more.
  e.trackLeftMs = 180000;
  e.seekSeq = 1;
  d.pass(e);
  const uint32_t seekAt = d.now;
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  d.runUntil([&] { return d.did(Do::Fence); }, e);
  TEST_ASSERT_TRUE(d.now - seekAt >= LibraryUpdate::kSeekQuietMs);
  TEST_ASSERT_TRUE(d.now - seekAt <= LibraryUpdate::kSeekQuietMs + 40);
  d.runUntil([&] { return d.did(Do::Saved); }, e);
  TEST_ASSERT_TRUE(d.upd->last().saved);
  // A play waiting for the headphones: never a safe point.
  Device w(fs, card);
  w.boot();
  w.upd->ask("gb");
  Env wait;
  wait.waiting = true;
  for (int i = 0; i < 200; ++i) w.pass(wait);
  TEST_ASSERT_FALSE(w.did(Do::Fence));
  TEST_ASSERT_TRUE(w.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  w.runUntil([&] { return w.did(Do::Saved); });  // paused: it goes
}

// ---------------------------------------------------------------------------
// A compaction asked during a build (gr, the journal's limits) waits for
// the save: nothing writes tags.bin or the journals while the build streams
// them. The worker's steps, in order: the compaction the step asked for,
// the build, the save, then the one asked meanwhile.
// ---------------------------------------------------------------------------
void test_a_compaction_asked_during_a_build() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  d.buildPasses = 40;
  d.upd->ask("the scan's end");
  d.runUntil([&] { return d.running == Job::Build; });
  TEST_ASSERT_TRUE(std::count(d.steps.begin(), d.steps.end(), Job::Compact) == 1);
  // gr: a Rescan's compaction and the rest after it; and a walk (gw).
  d.jobs.askCompact(true);
  d.jobs.askRest();
  d.jobs.askWalk();
  const size_t at = d.steps.size();
  d.runUntil([&] { return d.did(Do::Saved); });
  // From the build to the save's end, nothing but the build and the save
  // (the pass that takes the save in may start what waited).
  TEST_ASSERT_TRUE(d.steps.size() > at);
  TEST_ASSERT_TRUE(d.steps[at] == Job::Save);
  TEST_ASSERT_TRUE(d.steps.size() <= at + 2);
  d.drain();
  // After it: the walk (it waits for no compaction), the compaction, the scan.
  std::vector<Job> after(d.steps.begin() + static_cast<long>(at) + 1, d.steps.end());
  TEST_ASSERT_TRUE(std::find(after.begin(), after.end(), Job::Compact) != after.end());
  TEST_ASSERT_TRUE(std::find(after.begin(), after.end(), Job::Scan) != after.end());
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// ---------------------------------------------------------------------------
// A T found bad at the end of a build (a section's CRC, which only its end
// shows): the build restarts from D alone, before anything is shown, and T
// is left out for the session; the inputs saved say T wasn't used, and the
// next boot loads that index (the same T fails the same way).
// ---------------------------------------------------------------------------
void test_a_bad_transfer_found_at_the_end_of_a_build() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  {
    Device d(fs, card);
    d.boot();
    d.update("the scan's end");
  }
  // A transfer: the boot builds from T.
  commit(fs, card, 7);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.build.transferUsed);
    TEST_ASSERT_EQUAL_STRING("From T", titleOf(d.index, kFlac).c_str());
  }
  // T's strings spoiled behind its header: the same identity, so the boot
  // loads; the next update step's build finds it at T's end.
  spoilStrings(fs, 7);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_TRUE(d.root->present);
    d.update("gb");
    const LibraryUpdate::Step& s = d.upd->last();
    TEST_ASSERT_TRUE(s.built);
    TEST_ASSERT_TRUE(s.build.restarted);
    TEST_ASSERT_FALSE(s.build.transferUsed);
    TEST_ASSERT_TRUE(s.build.transferWhy != cc::Why::Ok);
    TEST_ASSERT_TRUE(s.build.deviceUsed);
    TEST_ASSERT_TRUE(d.upd->transferBad());
    TEST_ASSERT_FALSE(s.inputs.transfer);
    TEST_ASSERT_TRUE(s.saved);
    // From D alone: the device's own reading of the file.
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
    TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
    // The session's next build leaves T out from its start.
    d.update("again");
    TEST_ASSERT_FALSE(d.upd->last().build.restarted);
    TEST_ASSERT_FALSE(d.upd->last().build.transferUsed);
  }
  // The next boot: the index matches (whether T was used isn't asked).
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
  // A boot that builds meets it the same way (the marker): from D alone.
  fs.put(LibraryUpdate::kMarker, {});
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.built);
    TEST_ASSERT_TRUE(d.booted.build.restarted);
    TEST_ASSERT_TRUE(d.upd->transferBad());
    TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
  }
}

// ---------------------------------------------------------------------------
// A power cut at every step of an update (the compaction it asks for, the
// save aside, its rename, the marker's removal), each way a card can come
// back (InOrder, LoseUnsynced, Torn): the next boot has a whole library
// every time, either the new one or the old one marked soft-stale, whose
// next update step (the scan's end) gives the same as an uncut one. No
// cluster chain is ever freed while another entry uses it.
// ---------------------------------------------------------------------------
void test_a_power_cut_at_each_stage() {
  CutFs base;
  TestCard card;
  handFilled(base, card);
  const char* const variants[] = {"in order", "unsynced lost", "torn"};
  // With and without a deferral's marker on the card (its removal after the
  // save is a stage).
  for (int marker = 0; marker < 2; ++marker) {
    // An uncut run: how many steps the update takes.
    long total = 0;
    uint32_t old = 0, fresh = 0, promoted = 0;
    {
      CutFs fs = base.reboot(cutfs::Variant::InOrder);
      Device d(fs, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.softStale);
      if (marker) TEST_ASSERT_TRUE(d.upd->writeMarker());
      const long from = fs.steps;
      d.update("the scan's end");
      TEST_ASSERT_TRUE(d.upd->last().saved);
      TEST_ASSERT_EQUAL(marker != 0, d.upd->last().markerRemoved);
      total = fs.steps - from;
      TEST_ASSERT_TRUE(total > 5);
    }
    for (long k = 1; k <= total; ++k) {
      for (int v = 0; v < 3; ++v) {
        CutFs fs = base.reboot(cutfs::Variant::InOrder);
        {
          Device d(fs, card);
          d.boot();
          TEST_ASSERT_TRUE(d.booted.ok);
          if (marker) TEST_ASSERT_TRUE(d.upd->writeMarker());
          fs.cutAt = fs.steps + k;
          d.update("the scan's end");  // (it ends: saved, not saved, or failed)
        }
        char msg[80];
        snprintf(msg, sizeof(msg), "marker %d, cut at step %ld of %ld, %s", marker, k, total, variants[v]);
        CutFs after = fs.reboot(static_cast<cutfs::Variant>(v));
        {
          Device d(after, card);
          d.boot();
          TEST_ASSERT_TRUE_MESSAGE(d.booted.ok, msg);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(kAudio, d.index.trackCount(), msg);
          const bool isFresh = titleOf(d.index, kFlac) == kFlacTitle;
          // Old: its soft inputs say so (the scan's end rebuilds it), or
          // the marker built it at the boot.
          TEST_ASSERT_TRUE_MESSAGE(isFresh || d.booted.softStale, msg);
          ++(isFresh ? fresh : old);
          if (d.booted.tmpSettled.what == ts::Settle::Promoted) ++promoted;
          if (d.booted.softStale) {
            d.update("the scan's end");
            TEST_ASSERT_TRUE_MESSAGE(d.upd->last().saved, msg);
          }
          TEST_ASSERT_EQUAL_STRING_MESSAGE(kFlacTitle, titleOf(d.index, kFlac).c_str(), msg);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, pendingIn(d.index), msg);
        }
        TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty() && after.violations.empty(), msg);
        // And the boot after that: a matching index, not soft-stale, no marker.
        Device again(after, card);
        again.boot();
        TEST_ASSERT_TRUE_MESSAGE(again.booted.decision.action == libraryboot::Action::Load, msg);
        TEST_ASSERT_FALSE_MESSAGE(again.booted.softStale, msg);
        TEST_ASSERT_FALSE_MESSAGE(after.exists(LibraryUpdate::kMarker), msg);
      }
    }
    // Both outcomes came: the old index (a cut before the save's rename;
    // without the marker) and the new one, and a library.tmp taken whole
    // (a cut between the remove and the rename).
    if (!marker) TEST_ASSERT_TRUE(old > 0);
    TEST_ASSERT_TRUE(fresh > 0);
    TEST_ASSERT_TRUE(promoted > 0);
  }
}

// ---------------------------------------------------------------------------
// The fence: from the fence to the build's end no reader on the loop sees
// the index (readable() nullptr, the catalog without one, Now Playing's
// names from the held copy), checked at every read the build makes of the
// card and every block it takes; before and after, they see a whole index.
// ---------------------------------------------------------------------------
void test_the_fence() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.softStale);
  const uint32_t oldId = d.index.findTrack(kFlac);
  TEST_ASSERT_TRUE(oldId != LibraryIndex::kNone);
  char want[128];
  d.catalog.title(oldId, want, sizeof(want));
  TEST_ASSERT_EQUAL_STRING("a", want);
  std::string album = d.catalog.album(oldId);
  uint32_t checks = 0;
  bool inBuild = false;
  auto reader = [&] {
    if (!inBuild) return;
    ++checks;
    TEST_ASSERT_TRUE(d.upd->fenced());
    TEST_ASSERT_NULL(d.upd->readable());
    TEST_ASSERT_NULL(d.catalog.index());
    char t[128];
    TEST_ASSERT_TRUE(d.catalog.title(oldId, t, sizeof(t)) > 0);
    TEST_ASSERT_EQUAL_STRING(want, t);
    TEST_ASSERT_EQUAL_STRING(album.c_str(), d.catalog.album(oldId));
    char p[256];
    TEST_ASSERT_EQUAL_size_t(0, d.catalog.path(oldId, p, sizeof(p)));  // the player starts nothing
    TEST_ASSERT_FALSE(d.catalog.valid(oldId));
  };
  g_onRead = reader;
  g_onAlloc = reader;
  d.upd->ask("the scan's end");
  bool sawFence = false, sawLive = false;
  for (int i = 0; i < 10000 && !d.did(Do::Saved); ++i) {
    // Every pass: what the loop's readers see.
    if (d.upd->fenced()) {
      sawFence = true;
      TEST_ASSERT_NULL(d.upd->readable());
      TEST_ASSERT_NULL(d.catalog.index());
    } else {
      LibraryIndex* r = d.upd->readable();
      TEST_ASSERT_NOT_NULL(r);
      TEST_ASSERT_TRUE(r->ready());
      TEST_ASSERT_EQUAL_UINT32(kAudio, r->trackCount());
      if (d.did(Do::Live)) sawLive = true;
    }
    inBuild = d.upd->phase() == Phase::Build;  // the build is handed (and runs) this pass
    d.pass();
    inBuild = false;
  }
  g_onRead = nullptr;
  g_onAlloc = nullptr;
  TEST_ASSERT_TRUE(sawFence);
  TEST_ASSERT_TRUE(sawLive);
  TEST_ASSERT_TRUE(checks > 20);  // the build read and allocated behind it
  // After: the new index, its names; the held copy gone.
  TEST_ASSERT_EQUAL_PTR(&d.index, d.catalog.index());
  const uint32_t newId = d.index.findTrack(kFlac);
  char t[128];
  d.catalog.title(newId, t, sizeof(t));
  TEST_ASSERT_EQUAL_STRING(kFlacTitle, t);
  TEST_ASSERT_FALSE(d.upd->fenced());
  TEST_ASSERT_TRUE(d.upd->last().fenceMs > 0);
}

// ---------------------------------------------------------------------------
// The step's order and holds, pass by pass: the scan and new walks hold from
// the ask; a walk under way ends first; the compaction; then the fence, the
// build at priority 1, Live, the save; LibraryWrite and the worker's hold
// from the fence to the save's end. A card that doesn't answer fails the
// step before the fence, the index untouched.
// ---------------------------------------------------------------------------
void test_the_steps_order_and_a_card_that_doesnt_answer() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  // A walk under way when the step is asked (a hand-copied album: it has
  // news, walk.jnl): it goes on to its end, and is merged before the build.
  card.addBytes("New/Album/01 - g.mp3", corpus("ape_v1.mp3"));
  d.jobs.askWalk();
  d.runUntil([&] { return d.jobs.walking(); });
  d.upd->ask("gb");
  d.pass();
  TEST_ASSERT_TRUE(d.lastOut.holdScan);
  TEST_ASSERT_FALSE(d.lastOut.libraryWrite);
  std::vector<Phase> phases;
  for (int i = 0; i < 10000 && !d.did(Do::Saved); ++i) {
    const Phase before = d.upd->phase();
    if (phases.empty() || phases.back() != before) phases.push_back(before);
    const size_t acts = d.acts.size();
    d.pass();
    const bool fencePass = d.acts.size() > acts && d.acts.back() == Do::Fence;
    // LibraryWrite and the worker's hold: from the fence to the save's end.
    const bool held = fencePass || (before != Phase::Asked && before != Phase::Idle);
    const bool savedPass = d.acts.size() > acts && d.acts.back() == Do::Saved;
    if (!savedPass) {
      TEST_ASSERT_EQUAL(held, d.lastOut.libraryWrite);
      TEST_ASSERT_EQUAL(held, d.lastOut.updating);
    }
    TEST_ASSERT_TRUE(d.lastOut.holdScan || held || savedPass);
  }
  // (Fence and Live last within their passes: the loop carries them out at once.)
  const std::vector<Phase> want = {Phase::Asked, Phase::Build, Phase::Building, Phase::Save, Phase::Saving};
  TEST_ASSERT_EQUAL_size_t(want.size(), phases.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_TRUE(phases[i] == want[i]);
  // The walk ended before the compaction, the compaction before the build.
  long lastWalk = -1, compact = -1, build = -1;
  for (size_t i = 0; i < d.steps.size(); ++i) {
    if (d.steps[i] == Job::Walk) lastWalk = static_cast<long>(i);
    if (d.steps[i] == Job::Compact && build < 0) compact = static_cast<long>(i);
    if (d.steps[i] == Job::Build && build < 0) build = static_cast<long>(i);
  }
  TEST_ASSERT_TRUE(lastWalk >= 0 && compact > lastWalk && build > compact);
  TEST_ASSERT_EQUAL_UINT8(ScanScheduler::kHighPriority, ScanScheduler::priorityOf(Job::Build, false));

  // The card pulled while on: the step fails before its fence.
  const uint32_t tracks = d.index.trackCount();
  d.cut.dead = true;
  d.acts.clear();
  d.update("gb");
  TEST_ASSERT_TRUE(d.did(Do::Failed));
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_TRUE(d.upd->last().cardGone);
  TEST_ASSERT_TRUE(d.index.ready());
  TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
  d.cut.dead = false;
}

// ---------------------------------------------------------------------------
// A card that fails while the build reads it (pulled, or a contact glitch),
// after its opens worked (the card answered at the ask and on the worker):
// the card's trouble, not "no records". Nothing is walked (an absent /music
// would give an empty library and "Library updated"; a glitch the card came
// back from, a path-named one saved over library.idx) or saved, T isn't
// marked bad, and the next boot loads the last library.idx. A read of T
// failing while D reads whole: the build from D alone stands, saved as one
// that left records out: the next boot finds it soft-stale, and its update
// step reads T again.
// ---------------------------------------------------------------------------
void test_a_card_that_fails_mid_build() {
  for (int withT = 0; withT < 2; ++withT) {
    for (int how = 0; how < 2; ++how) {  // 0: pulled (every read after fails); 1: one read of D fails
      char msg[64];
      snprintf(msg, sizeof(msg), "%s, %s", withT ? "T and D" : "D", how ? "a glitch" : "pulled");
      CutFs fs;
      TestCard card;
      if (withT) {
        transferCard(fs, card);
      } else {
        handFilled(fs, card);
      }
      const std::vector<uint8_t> saved = fs.bytes(LibraryUpdate::kIndexNames.path);
      std::string title;
      {
        Device d(fs, card);
        d.boot();
        TEST_ASSERT_TRUE_MESSAGE(d.booted.ok, msg);
        title = titleOf(d.index, kFlac);
        const int walks = g_walks;
        int reads = 0;
        g_onRead = [&] {
          if (how == 0 && d.upd->phase() == Phase::Building && ++reads == 3) fs.dead = true;
        };
        int deviceReads = 0;
        g_failRead = [&](const std::string& path) {
          return how == 1 && d.upd->phase() == Phase::Building && path == "/.player/tags.bin" && ++deviceReads == 4;
        };
        d.upd->ask("the scan's end");
        d.runUntil([&] { return d.did(Do::Live); });
        g_onRead = nullptr;
        g_failRead = nullptr;
        fs.dead = false;  // (put back)
        d.runUntil([&] { return d.did(Do::Saved); });
        const LibraryUpdate::Step& s = d.upd->last();
        TEST_ASSERT_TRUE_MESSAGE(s.cardGone, msg);
        TEST_ASSERT_TRUE_MESSAGE(s.readErrors, msg);
        TEST_ASSERT_FALSE_MESSAGE(s.built, msg);
        TEST_ASSERT_FALSE_MESSAGE(s.walked, msg);
        TEST_ASSERT_EQUAL_INT_MESSAGE(walks, g_walks, msg);
        TEST_ASSERT_FALSE_MESSAGE(s.saved, msg);
        TEST_ASSERT_TRUE_MESSAGE(std::find(d.steps.begin(), d.steps.end(), Job::Save) == d.steps.end(), msg);
        TEST_ASSERT_FALSE_MESSAGE(d.upd->transferBad(), msg);
        TEST_ASSERT_FALSE_MESSAGE(d.index.ready(), msg);  // no library until the next boot
        TEST_ASSERT_TRUE_MESSAGE(d.upd->phase() == Phase::Idle, msg);
      }
      // library.idx as it was: the next boot loads it.
      TEST_ASSERT_TRUE_MESSAGE(fs.bytes(LibraryUpdate::kIndexNames.path) == saved, msg);
      Device again(fs, card);
      again.boot();
      TEST_ASSERT_TRUE_MESSAGE(again.booted.decision.action == libraryboot::Action::Load, msg);
      TEST_ASSERT_TRUE_MESSAGE(again.booted.ok, msg);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(title.c_str(), titleOf(again.index, kFlac).c_str(), msg);
      TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg);
    }
  }
  // One read of T fails, D reads whole: built from D alone, T not marked
  // bad, saved as records left out; the next boot loads it soft-stale and
  // its update step reads T again.
  CutFs fs;
  TestCard card;
  transferCard(fs, card);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    int tReads = 0;
    g_failRead = [&](const std::string& path) {
      return d.upd->phase() == Phase::Building && path == tagsName(1) && ++tReads == 3;
    };
    d.update("gb");
    g_failRead = nullptr;
    const LibraryUpdate::Step& s = d.upd->last();
    TEST_ASSERT_TRUE(s.built);
    TEST_ASSERT_TRUE(s.readErrors);
    TEST_ASSERT_FALSE(s.cardGone);
    TEST_ASSERT_FALSE(s.build.transferUsed);
    TEST_ASSERT_TRUE(s.build.transferWhy == cc::Why::Io);
    TEST_ASSERT_TRUE(s.build.deviceUsed);
    TEST_ASSERT_FALSE(d.upd->transferBad());
    TEST_ASSERT_TRUE(s.saved);
    TEST_ASSERT_EQUAL_UINT32(libraryboot::kJournalsLeftOut, s.inputs.journalSeq);
    TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  }
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    TEST_ASSERT_TRUE(d.booted.softStale);
    d.update("the scan's end");
    TEST_ASSERT_TRUE(d.upd->last().build.transferUsed);
    TEST_ASSERT_FALSE(d.upd->last().readErrors);
    TEST_ASSERT_EQUAL_STRING("From T", titleOf(d.index, kFlac).c_str());
  }
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_FALSE(d.booted.softStale);
  }
  TEST_ASSERT_TRUE(fs.violations.empty());
}

// ---------------------------------------------------------------------------
// The worker's task before the fence: it ends 3 s after its last step (a
// long wait for the safe point), and the build needs it made again (a 6 KB
// stack in internal RAM). The step asks for it (Out::wantWorker) and puts
// its fence up only once it is there: with no internal RAM for it, the step
// waits, the index as it was and nothing held but the scan; once it can be
// made, the step goes on.
// ---------------------------------------------------------------------------
void test_the_worker_s_task_before_the_fence() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  const uint32_t tracks = d.index.trackCount();
  d.workerUp = false;
  d.canMakeWorker = false;
  d.upd->ask("the scan's end");
  for (int i = 0; i < 300; ++i) d.pass();
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_TRUE(d.upd->phase() == Phase::Asked);
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::Worker);
  TEST_ASSERT_TRUE(d.lastOut.wantWorker);
  TEST_ASSERT_FALSE(d.lastOut.libraryWrite || d.lastOut.updating || d.lastOut.fenced);
  TEST_ASSERT_NOT_NULL(d.upd->readable());
  TEST_ASSERT_TRUE(d.index.ready());
  TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
  // (Not asked while it waits for the safe point: the task isn't held then.)
  Env playing;
  playing.playing = true;
  playing.trackLeftMs = 5000;
  d.pass(playing);
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  TEST_ASSERT_FALSE(d.lastOut.wantWorker);
  // Internal RAM for it: made, then the fence, the build, the save.
  d.canMakeWorker = true;
  d.runUntil([&] { return d.did(Do::Saved); });
  TEST_ASSERT_TRUE(d.upd->last().saved);
  TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
}

// ---------------------------------------------------------------------------
// The build-at-boot marker's build out of PSRAM on the boot's fresh heap,
// library.idx matching the card: that index is loaded instead (stale, but a
// library; before, every boot ended with none), the marker removed, and the
// session's short memory checks don't write it again (the next boot would
// fail the same build); gb! still does.
// ---------------------------------------------------------------------------
void test_the_marker_s_build_out_of_psram() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  size_t loadBytes = 0;
  std::string title;
  {
    Device d(fs, card);
    const size_t base = g_live;
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
    loadBytes = g_live - base;
    title = titleOf(d.index, kFlac);
  }
  fs.put(LibraryUpdate::kMarker, {});
  Device d(fs, card);
  // Room for the load, not for the builder's own memory.
  g_ceiling = g_live + loadBytes + 1024;
  d.boot();
  g_ceiling = SIZE_MAX;
  TEST_ASSERT_TRUE(d.booted.marker);
  TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Build);
  TEST_ASSERT_TRUE(d.booted.ok);
  TEST_ASSERT_FALSE(d.booted.noMemory);
  TEST_ASSERT_TRUE(d.booted.loadedShort);
  TEST_ASSERT_TRUE(d.booted.softStale);  // (the marker's compaction ran: the journal is in D now)
  TEST_ASSERT_TRUE(d.booted.markerRemoved);
  TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
  TEST_ASSERT_TRUE(d.upd->bootBuildShort());
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  TEST_ASSERT_EQUAL_STRING(title.c_str(), titleOf(d.index, kFlac).c_str());
  // This session: short again, no marker; gb! writes it.
  Env shortOf;
  shortOf.psramFree = shortOf.psramLargest = 1024;
  d.update("the scan's end", shortOf);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_TRUE(d.upd->last().markerSkipped);
  TEST_ASSERT_FALSE(d.upd->last().markerWritten);
  TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
  TEST_ASSERT_TRUE(d.index.ready());
  d.update("gb!", Env(), true);
  TEST_ASSERT_TRUE(d.upd->last().markerWritten);
  TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
  // A boot with room: the marker's build, as ever.
  Device again(fs, card);
  again.boot();
  TEST_ASSERT_TRUE(again.booted.built);
  TEST_ASSERT_FALSE(again.booted.loadedShort);
  TEST_ASSERT_TRUE(again.booted.markerRemoved);
  TEST_ASSERT_FALSE(again.upd->bootBuildShort());
  TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(again.index, kFlac).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_safe_point_and_the_memory_check);
  RUN_TEST(test_every_row_of_the_boot);
  RUN_TEST(test_library_tmp_after_a_cut);
  RUN_TEST(test_a_deferral_then_a_boot_that_builds);
  RUN_TEST(test_a_track_end_near_the_safe_point);
  RUN_TEST(test_a_compaction_asked_during_a_build);
  RUN_TEST(test_a_bad_transfer_found_at_the_end_of_a_build);
  RUN_TEST(test_a_power_cut_at_each_stage);
  RUN_TEST(test_the_fence);
  RUN_TEST(test_the_steps_order_and_a_card_that_doesnt_answer);
  RUN_TEST(test_a_card_that_fails_mid_build);
  RUN_TEST(test_the_worker_s_task_before_the_fence);
  RUN_TEST(test_the_marker_s_build_out_of_psram);
  return UNITY_END();
}
