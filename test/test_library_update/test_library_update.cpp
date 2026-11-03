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
// marker's build out of PSRAM at the boot; a failed first walk that must
// not shrink the library (2026-10-09); Auto DJ's: a pick owed while its
// index joins holds the fence, a held Auto DJ never does, its join
// released at the fence runs again after the Live step on the new ids, and
// the memory check has the join given back when the new track table's
// block is short (setGiveBack()). A read the card fails at each point of a boot
// (library.idx's, D's, T's and D's as a build reads them): nothing goes over what is
// on the card, the jobs hold, and the library is whole, or /music walked for the
// session. What a track that ends inside the fence does is test_gapless_player's.
// Run: pio test -e native -f test_library_update
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../support/CutFs.h"
#include "../support/FakeFat.h"
#include "AutoDjJoin.h"
#include "AutoDjTrigger.h"
#include "CardAutoDj.h"
#include "CardContract.h"
#include "CardJobs.h"
#include "CardManifest.h"
#include "CardRoot.h"
#include "CardTags.h"
#include "LibraryBoot.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibraryUpdate.h"
#include "QueueText.h"
#include "ScanScheduler.h"
#include "TagStore.h"
#include "TrackCatalog.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace mpdj = cardcontract::mpdj;
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
    bool damaged() const override { return f_->damaged(); }
    ts::File* f_;
    std::string path_;
  };
  ts::File* open(const char* path, Mode mode) override {
    ts::File* f = in_.open(path, mode);
    return f ? new F(f, path) : nullptr;
  }
  ts::File* openRead(const char* path, bool* cardFailed) override {
    ts::File* f = in_.openRead(path, cardFailed);
    return f ? new F(f, path) : nullptr;
  }
  Entry entry(const char* path) override { return in_.entry(path); }
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
  bool autoDjOwed = false;  // (autodj::decide()'s owed)
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
  // Auto DJ's hooks around the fence (main.cpp's enterFence(), before the
  // index goes; leaveFence(), once the new one is readable).
  std::function<void()> beforeFence, afterLive;

  Device(CutFs& c, TestCard& t) : cut(c), fs(c), card(t), jobs(nullptr, nullptr) {
    index.keepTrackBlock(true);
    // /music's own entry (FatFs answers for the folder: the step asks
    // whether the card is still there).
    if (!cut.dead && !cut.exists("/music")) cut.put("/music", {});
  }

  // What the firmware passes (Library::openRecords()): the boot's waits, or none.
  void (*wait)(uint32_t ms) = nullptr;

  void boot() {
    store.reset(new ts::TagStore(fs, storeConfig()));
    store->openAtBoot(wait);
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
    c.wait = wait;
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
    ui.autoDjOwed = e.autoDjOwed;
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
      if (beforeFence) beforeFence();
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
      if (afterLive) afterLive();
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
// A boot the card fails the root's reads at: nothing is known of the transfer,
// which isn't to say there is none. Taken for none, this would build from D alone
// over a good library.idx, every file T lists under its path.
void test_a_root_the_card_fails_to_read_at_the_boot_keeps_the_library() {
  using libraryboot::Action;
  using libraryboot::Saved;
  CutFs fs;
  TestCard card;
  fill(card);
  commit(fs, card, 2);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.build.transferUsed);
  }
  const std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
  const uint32_t writes = static_cast<uint32_t>(fs.bytesWritten);
  fs.faultPrefix = "/.mstream/";
  fs.faultFrom = 0;
  fs.faultTo = -1;
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.root->readFailed());
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Differs);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_FALSE(d.booted.built);
    TEST_ASSERT_FALSE(d.booted.walked);
    // The tags T gave it, and not a byte written.
    TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
    TEST_ASSERT_EQUAL_STRING("From T", titleOf(d.index, kFlac).c_str());
    TEST_ASSERT_EQUAL_UINT32(writes, static_cast<uint32_t>(fs.bytesWritten));
    TEST_ASSERT_TRUE(idx == fs.bytes(LibraryUpdate::kIndexNames.path));
  }
  // No PSRAM to load it: no library this session, and the jobs held all the same.
  {
    Device d(fs, card);
    g_ceiling = g_live + 64;
    d.boot();
    g_ceiling = SIZE_MAX;
    TEST_ASSERT_TRUE(d.booted.noMemory);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(idx == fs.bytes(LibraryUpdate::kIndexNames.path));
  }
  // T's reads alone: the same, the root naming the card.
  fs.faultPrefix = "/.mstream/tags";
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.root->hasRoot);
    TEST_ASSERT_TRUE(d.root->readFailed());
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_EQUAL_STRING("From T", titleOf(d.index, kFlac).c_str());
    TEST_ASSERT_EQUAL_UINT32(writes, static_cast<uint32_t>(fs.bytesWritten));
  }
  fs.faultPrefix = "/.mstream/";
  // A deferred build waits for a boot that can read it.
  fs.put(LibraryUpdate::kMarker, {});
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.marker);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_FALSE(d.booted.markerRemoved);
    TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
  }
  fs.remove(LibraryUpdate::kMarker);
  // The card answering again, the next boot finds what it left.
  fs.faultFrom = -1;
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.root->present);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Matches);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_TRUE(idx == fs.bytes(LibraryUpdate::kIndexNames.path));
  }
  // No library.idx: no transfer's library to lose. Built from what can be read,
  // saved with no transfer in it, the card jobs running; so loaded next, as a card
  // with no transfer data, as long as the root stays unread.
  fs.faultFrom = 0;
  fs.remove(LibraryUpdate::kIndexNames.path);
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.root->readFailed());
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Missing);
    TEST_ASSERT_TRUE(d.booted.decision.action != Action::Load);
    TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_FALSE(d.booted.build.transferUsed);
  }
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Matches);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
    TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
  }
  // Its deferred build runs, too: it can lose nothing. (From what can be read.)
  fs.put(LibraryUpdate::kMarker, {});
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.decision.action != Action::Load);
    TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
    TEST_ASSERT_FALSE(d.booted.build.transferUsed);
    TEST_ASSERT_TRUE(d.booted.markerRemoved);
  }
  // A boot that can read the root builds from it.
  fs.faultFrom = -1;
  {
    Device d(fs, card);
    d.boot();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Differs);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_TRUE(d.booted.build.transferUsed);
    TEST_ASSERT_EQUAL_STRING("From T", titleOf(d.index, kFlac).c_str());
  }
}

// ---------------------------------------------------------------------------
// A read the card fails of library.idx or of the records: the boot keeps what is on the
// card, and the library it gives is whole, or /music walked for the session (a part from
// the records would cut queue.txt down to itself when the queue is restored: METADATA.md
// 3.2.2, measured on the firmware's own QueueStore).
// ---------------------------------------------------------------------------
namespace {

std::map<std::string, std::vector<uint8_t>> filesOf(const CutFs& fs) {
  std::map<std::string, std::vector<uint8_t>> out;
  for (const std::string& n : fs.names()) out[n] = fs.bytes(n);
  return out;
}

void arm(CutFs& fs, const char* prefix, long from, long to) {
  fs.faultPrefix = prefix;
  fs.faultEvents = 0;
  fs.faultFrom = from;
  fs.faultTo = to;
}

std::vector<uint32_t> g_waits;
void noteWait(uint32_t ms) { g_waits.push_back(ms); }

// A card filled three ways: T and files it doesn't list, read here (a transfer, then a
// PC); every file read here; every file T's. The worst for a build without D is the
// first, and without T the second.
enum class Filled { Mixed, Hand, Transfer };
constexpr uint32_t kMixedExtra = 2;

uint32_t filledCard(CutFs& fs, TestCard& card, Filled how) {
  if (how == Filled::Hand) {
    handFilled(fs, card);
  } else {
    transferCard(fs, card);
  }
  if (how == Filled::Mixed) {
    card.addBytes("Later/01 - g.mp3", corpus("v22.mp3"));
    card.addBytes("Later/02 - h.mp3", corpus("v1_only.mp3"));
  }
  // A session to the end (the walk, the scan, the update step, its save), so the boots
  // here load, with nothing to do.
  {
    Device d(fs, card);
    d.boot();
    d.jobs.askWalk();
    d.drain();
    if (d.store->hasJournals()) d.update("the scan's end");
    d.drain();
  }
  {
    Device d(fs, card);
    d.boot();
    d.jobs.askWalk();
    d.drain();
    if (d.store->hasJournals() || d.booted.softStale) d.update("the scan's end");
  }
  const uint32_t tracks = kAudio + (how == Filled::Mixed ? kMixedExtra : 0);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.ok);
  TEST_ASSERT_TRUE(d.booted.decision.action == libraryboot::Action::Load);
  TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
  TEST_ASSERT_FALSE(d.booted.softStale);
  TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
  TEST_ASSERT_TRUE(d.store->device().present);
  TEST_ASSERT_FALSE(d.store->hasJournals());
  return tracks;
}

// What a boot's reads under `prefix` number, on a copy (one with nothing wrong writes
// nothing, or only what a card can take twice).
long bootEvents(const CutFs& fs, TestCard& card, const char* prefix) {
  CutFs counted = fs.reboot(cutfs::Variant::InOrder);
  arm(counted, prefix, 1000000, -1);
  Device d(counted, card);
  d.boot();
  return counted.faultEvents;
}

// ... and those of the store's open() alone, the boot's first.
long openEvents(const CutFs& fs, const char* prefix) {
  CutFs counted = fs.reboot(cutfs::Variant::InOrder);
  arm(counted, prefix, 1000000, -1);
  ts::TagStore st(counted, storeConfig());
  st.open();
  return counted.faultEvents;
}

// The whole library, as its records give it; or /music walked for the session, every
// track by its path (this rig's walk lists every file: the firmware's can't be told
// whole, and QueueStore::restore() keeps what it leaves out); never a part of the records.
void assertWhole(const Device& d, uint32_t tracks, const char* msg) {
  TEST_ASSERT_TRUE_MESSAGE(d.booted.ok, msg);
  TEST_ASSERT_TRUE_MESSAGE(d.index.ready(), msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, d.index.trackCount(), msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(d.booted.sessionWalked() ? tracks : 0, pendingIn(d.index), msg);
}

// The queue's file against a boot's library, as QueueStore::restore() reads it: no track
// of it is dropped.
void assertQueueKept(const Device& d, uint32_t tracks, const char* msg) {
  if (!d.index.ready()) return;
  std::string text = "mstream-queue 1 ";
  text += std::to_string(tracks) + " 0 1\n";
  for (uint32_t t = 0; t < tracks; ++t) {
    char path[300];
    d.index.trackPath(t, path, sizeof(path));
    text += path;
    text += '\n';
  }
  MemorySource src(text.data(), text.size());
  QueueModel q(testAlloc, testFree);
  TrackCatalog catalog;
  catalog.setIndex(const_cast<LibraryIndex*>(&d.index));
  const queuetext::Restored r = queuetext::read(src, catalog, q, nullptr, nullptr, testAlloc, testFree);
  TEST_ASSERT_TRUE_MESSAGE(r.ok, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.dropped, msg);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, r.entries, msg);
}

}  // namespace

// library.idx's reads, each in its turn (the settling's questions, the marker's entry is
// another prefix's, the header's, the load's): nothing goes over it, the records give
// the same library for the session, and the jobs hold.
void test_a_read_of_library_idx_the_card_fails_builds_nothing_over_it() {
  using libraryboot::Action;
  using libraryboot::Saved;
  for (Filled how : {Filled::Mixed, Filled::Hand, Filled::Transfer}) {
    CutFs fs;
    TestCard card;
    const uint32_t tracks = filledCard(fs, card, how);
    const auto files = filesOf(fs);
    const long events = bootEvents(fs, card, "/.player/library");
    TEST_ASSERT_GREATER_THAN_INT32(3, events);
    for (long k = 0; k < events; ++k) {
      char msg[64];
      snprintf(msg, sizeof(msg), "read %ld of %ld", k, events);
      for (bool lasting : {false, true}) {
        arm(fs, "/.player/library", k, lasting ? -1 : k + 1);
        const uint64_t writes = fs.bytesWritten;
        {
          Device d(fs, card);
          d.boot();
          fs.faultFrom = -1;
          TEST_ASSERT_TRUE_MESSAGE(d.booted.saved == Saved::Unread, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.action == Action::Build, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.sessionOnly, msg);
          assertWhole(d, tracks, msg);
          assertQueueKept(d, tracks, msg);
          TEST_ASSERT_FALSE_MESSAGE(d.booted.walked, msg);
          TEST_ASSERT_EQUAL_UINT64_MESSAGE(writes, fs.bytesWritten, msg);
          TEST_ASSERT_TRUE_MESSAGE(filesOf(fs) == files, msg);
        }
        // The next boot, the card answering, finds it all.
        Device next(fs, card);
        next.boot();
        TEST_ASSERT_TRUE_MESSAGE(next.booted.decision.action == Action::Load, msg);
        TEST_ASSERT_FALSE_MESSAGE(next.booted.decision.holdJobs, msg);
        TEST_ASSERT_FALSE_MESSAGE(next.booted.softStale, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, next.index.trackCount(), msg);
      }
    }
    // The marker's entry, whether it is there or not: "no marker" may be only the read
    // that failed, and a deferred build goes unseen. The same.
    for (bool marked : {false, true}) {
      CutFs at = fs.reboot(cutfs::Variant::InOrder);
      if (marked) at.put(LibraryUpdate::kMarker, {});
      const auto before = filesOf(at);
      arm(at, LibraryUpdate::kMarker, 0, 1);
      Device d(at, card);
      d.boot();
      at.faultFrom = -1;
      TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
      TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
      TEST_ASSERT_TRUE(d.booted.decision.sessionOnly);
      TEST_ASSERT_TRUE(filesOf(at) == before);
    }
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
}

// Most faults are a glitch: read again by the boot's waits, each read and open in its
// turn, it loads, with nothing held. Each failing: the four reads, then the hold.
void test_the_boot_reads_library_idx_again_while_the_card_fails() {
  using libraryboot::Action;
  using libraryboot::Saved;
  CutFs fs;
  TestCard card;
  const uint32_t tracks = filledCard(fs, card, Filled::Transfer);
  const auto files = filesOf(fs);
  const long events = bootEvents(fs, card, "/.player/library");
  for (long k = 0; k < events; ++k) {
    char msg[64];
    snprintf(msg, sizeof(msg), "read %ld of %ld", k, events);
    g_waits.clear();
    arm(fs, "/.player/library", k, k + 1);
    Device d(fs, card);
    d.wait = noteWait;
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_EQUAL_size_t_MESSAGE(1, g_waits.size(), msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(ts::kBootWaitsMs[0], g_waits[0], msg);
    TEST_ASSERT_TRUE_MESSAGE(d.booted.saved == Saved::Matches, msg);
    TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.action == Action::Load, msg);
    TEST_ASSERT_FALSE_MESSAGE(d.booted.decision.holdJobs, msg);
    TEST_ASSERT_TRUE_MESSAGE(d.booted.ok, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, d.index.trackCount(), msg);
  }
  // Each failing: header and load both give up after their four reads at most.
  g_waits.clear();
  arm(fs, "/.player/library", 0, -1);
  {
    Device d(fs, card);
    d.wait = noteWait;
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_EQUAL_size_t(3, g_waits.size());
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.ok);
  }
  TEST_ASSERT_TRUE(filesOf(fs) == files);
  // No library.idx, or a whole one: its word, the first time, no wait.
  g_waits.clear();
  {
    Device d(fs, card);
    d.wait = noteWait;
    d.boot();
    TEST_ASSERT_EQUAL_size_t(0, g_waits.size());
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Load);
  }
  CutFs noIdx = fs.reboot(cutfs::Variant::InOrder);
  noIdx.remove(LibraryUpdate::kIndexNames.path);
  {
    Device d(noIdx, card);
    d.wait = noteWait;
    d.boot();
    TEST_ASSERT_EQUAL_size_t(0, g_waits.size());
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Missing);
  }
}

// library.idx unread with nothing whole to build from: /music walked for the session, by
// its paths, nothing written. The next boot asks again.
void test_library_idx_unread_and_nothing_whole_to_build_from_is_walked_for_the_session() {
  using libraryboot::Action;
  using libraryboot::Saved;
  // The root unread too.
  {
    CutFs fs;
    TestCard card;
    filledCard(fs, card, Filled::Transfer);
    const auto files = filesOf(fs);
    g_failRead = [](const std::string& path) { return path == LibraryUpdate::kIndexNames.path; };
    arm(fs, "/.mstream/", 0, -1);
    Device d(fs, card);
    d.boot();
    g_failRead = nullptr;
    fs.faultFrom = -1;
    TEST_ASSERT_TRUE(d.root->readFailed());
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, kAudio, "the root unread too");
    TEST_ASSERT_TRUE(filesOf(fs) == files);
  }
  // No D: T alone leaves out what it doesn't list.
  {
    CutFs fs;
    TestCard card;
    fill(card);
    commit(fs, card, 1);
    { Device first(fs, card); first.boot(); }
    TEST_ASSERT_FALSE(fs.exists("/.player/tags.bin"));
    const auto files = filesOf(fs);
    arm(fs, LibraryUpdate::kIndexNames.path, 0, -1);
    Device d(fs, card);
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, kAudio, "no D");
    TEST_ASSERT_TRUE(filesOf(fs) == files);
  }
  // D that doesn't list the card (no T, the scan's files alone: its first walk failed).
  {
    CutFs fs;
    TestCard card;
    fill(card);
    {
      Device first(fs, card);
      first.boot();
      TEST_ASSERT_TRUE(first.booted.walked);
    }
    {
      ts::TagStore st(fs, storeConfig());
      st.open();
      uint8_t buf[2048];
      ts::ChunkBuilder chunk;
      chunk.begin(buf, sizeof(buf));
      mptg::Record rec;
      rec.size = 7;
      const char* f[cc::kRunFields] = {};
      for (auto& x : f) x = "";
      TEST_ASSERT_TRUE(chunk.add("Artist/Album/01 - a.flac", 24, ts::Status::Scanned, rec, f));
      TEST_ASSERT_TRUE(st.append(chunk));
      TEST_ASSERT_TRUE(st.compact().ok);
      TEST_ASSERT_TRUE(st.device().present);
      TEST_ASSERT_FALSE(st.device().header.walked);
    }
    const auto files = filesOf(fs);
    arm(fs, LibraryUpdate::kIndexNames.path, 0, -1);
    Device d(fs, card);
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, kAudio, "D not listing the card");
    TEST_ASSERT_TRUE(filesOf(fs) == files);
  }
}

// A marker stays through a session's build, and a journal is folded first: its records
// are in D then, and the build is the whole library.
void test_a_session_build_keeps_the_marker_and_folds_the_journals() {
  using libraryboot::Saved;
  CutFs fs;
  TestCard card;
  const uint32_t tracks = filledCard(fs, card, Filled::Mixed);
  // A chunk the next boot finds in tags.jnl.
  {
    ts::TagStore st(fs, storeConfig());
    st.open();
    uint8_t buf[2048];
    ts::ChunkBuilder chunk;
    chunk.begin(buf, sizeof(buf));
    uint32_t size = 0, time = 0;
    TEST_ASSERT_TRUE(card.stat("Later/02 - h.mp3", 16, &size, &time));
    mptg::Record rec;
    rec.size = size;
    rec.fatTime = time;
    rec.known = mptg::kKnownRules1;
    rec.container = mptg::kContainerMp3;
    const char* f[cc::kRunFields] = {};
    for (auto& x : f) x = "";
    f[cc::kTitle] = "From the journal";
    TEST_ASSERT_TRUE(chunk.add("Later/02 - h.mp3", 16, ts::Status::Scanned, rec, f));
    TEST_ASSERT_TRUE(st.append(chunk));
  }
  fs.put(LibraryUpdate::kMarker, {});
  const std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
  arm(fs, LibraryUpdate::kIndexNames.path, 0, -1);
  {
    Device d(fs, card);
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.compacted);
    TEST_ASSERT_TRUE(d.booted.compaction.ok);
    TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
    TEST_ASSERT_EQUAL_STRING("From the journal", titleOf(d.index, "/music/Later/02 - h.mp3").c_str());
    TEST_ASSERT_FALSE(d.booted.markerRemoved);
    TEST_ASSERT_TRUE(fs.exists(LibraryUpdate::kMarker));
    TEST_ASSERT_TRUE(idx == fs.bytes(LibraryUpdate::kIndexNames.path));
  }
  // A read of D failing as the session's build reads it: not the whole library, so the
  // walk, nothing saved.
  CutFs later = fs.reboot(cutfs::Variant::InOrder);
  long reads = 0;
  arm(later, LibraryUpdate::kIndexNames.path, 0, -1);
  g_failRead = [&](const std::string& path) { return path == "/.player/tags.bin" && ++reads == 20; };
  {
    Device d(later, card);
    d.boot();
    later.faultFrom = -1;
    TEST_ASSERT_GREATER_OR_EQUAL_INT32(20, reads);
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, tracks, "a read in the session's build");
    TEST_ASSERT_TRUE(idx == later.bytes(LibraryUpdate::kIndexNames.path));
  }
  g_failRead = nullptr;
  // A journal that can't be folded in (a card that takes no writes): tags.bin alone
  // would leave out what it holds, so the walk.
  CutFs refusing = fs.reboot(cutfs::Variant::InOrder);
  {
    ts::TagStore st(refusing, storeConfig());
    st.open();
    uint8_t buf[2048];
    ts::ChunkBuilder chunk;
    chunk.begin(buf, sizeof(buf));
    uint32_t size = 0, time = 0;
    TEST_ASSERT_TRUE(card.stat("Later/01 - g.mp3", 16, &size, &time));
    mptg::Record rec;
    rec.size = size;
    rec.fatTime = time;
    rec.known = mptg::kKnownRules1;
    rec.container = mptg::kContainerMp3;
    const char* f[cc::kRunFields] = {};
    for (auto& x : f) x = "";
    TEST_ASSERT_TRUE(chunk.add("Later/01 - g.mp3", 16, ts::Status::Scanned, rec, f));
    TEST_ASSERT_TRUE(st.append(chunk));
  }
  refusing.refuse = true;
  arm(refusing, LibraryUpdate::kIndexNames.path, 0, -1);
  {
    Device d(refusing, card);
    d.boot();
    refusing.faultFrom = -1;
    refusing.refuse = false;
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.sessionOnly);
    TEST_ASSERT_TRUE(d.booted.compacted);
    TEST_ASSERT_FALSE(d.booted.compaction.ok);
    TEST_ASSERT_FALSE(d.booted.compaction.readFailed);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, tracks, "a journal left");
  }
  // D's own bytes failing a check mid-build, under T: T alone would be a part of the card.
  // With no T, nothing usable. The walk both ways, once, with nothing saved.
  for (const bool withT : {true, false}) {
    CutFs bad;
    TestCard same;
    filledCard(bad, same, withT ? Filled::Mixed : Filled::Hand);
    std::vector<uint8_t> dBytes = bad.bytes("/.player/tags.bin");
    cc::MemSource src(dBytes.data(), static_cast<uint32_t>(dBytes.size()));
    cc::Container c;
    mptg::Info info;
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(c, src, &info)));
    dBytes[c.section(mptg::kSecRecs).offset + 13] ^= 0x10;
    bad.put("/.player/tags.bin", dBytes);
    const std::vector<uint8_t> kept = bad.bytes(LibraryUpdate::kIndexNames.path);
    const int walks = g_walks;
    arm(bad, LibraryUpdate::kIndexNames.path, 0, -1);
    Device d(bad, same);
    d.boot();
    bad.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.sessionOnly);
    TEST_ASSERT_FALSE(d.booted.build.deviceUsed);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    TEST_ASSERT_EQUAL_INT(walks + 1, g_walks);
    TEST_ASSERT_TRUE(kept == bad.bytes(LibraryUpdate::kIndexNames.path));
  }
  // T's own checks failing at its end, D whole: D alone isn't the library either, so the
  // walk, by no fault of the card's.
  {
    CutFs bad;
    TestCard same;
    const uint32_t tracksThere = filledCard(bad, same, Filled::Mixed);
    spoilStrings(bad, 1);
    arm(bad, LibraryUpdate::kIndexNames.path, 0, -1);
    Device d(bad, same);
    d.boot();
    bad.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_FALSE(d.booted.build.transferUsed);
    TEST_ASSERT_TRUE(d.booted.build.deviceUsed);
    TEST_ASSERT_FALSE(d.booted.readErrors);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, tracksThere, "T's checks");
  }
  // D's folder facts found damaged at the last read of them, its rows whole: a read that
  // failed, the file's fault: a build that stands on any other boot, but not the library
  // this one stands in for. The walk.
  {
    CutFs bad;
    TestCard same;
    const uint32_t tracksThere = filledCard(bad, same, Filled::Mixed);
    arm(bad, LibraryUpdate::kIndexNames.path, 0, -1);
    long total = 0;
    {
      CutFs dry = bad.reboot(cutfs::Variant::InOrder);
      arm(dry, LibraryUpdate::kIndexNames.path, 0, -1);
      g_failRead = [&](const std::string& path) {
        total += path == "/.player/tags.bin";
        return false;
      };
      Device d(dry, same);
      d.boot();
      g_failRead = nullptr;
      TEST_ASSERT_TRUE(d.booted.built);
    }
    long seen = 0;
    g_failRead = [&](const std::string& path) {
      if (path == "/.player/tags.bin" && ++seen == total) bad.damagedPrefix = path;
      return false;
    };
    Device d(bad, same);
    d.boot();
    g_failRead = nullptr;
    bad.faultFrom = -1;
    bad.damagedPrefix.clear();
    TEST_ASSERT_TRUE(d.booted.readErrors);
    TEST_ASSERT_TRUE(d.booted.build.deviceUsed);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, tracksThere, "D's facts");
  }
  // T's own records found damaged at its last read in the session's build, D whole: the
  // build from D alone that it restarts as is another library than the one it stands
  // in for, so the walk. (No fault of the card's: T's next boot says what it says.)
  const std::string t = tagsName(1);
  long tTotal = 0;
  {
    CutFs dry = fs.reboot(cutfs::Variant::InOrder);
    arm(dry, LibraryUpdate::kIndexNames.path, 0, -1);
    g_failRead = [&](const std::string& path) {
      tTotal += path == t;
      return false;
    };
    Device d(dry, card);
    d.boot();
    g_failRead = nullptr;
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_TRUE(d.booted.build.transferUsed);
  }
  CutFs split = fs.reboot(cutfs::Variant::InOrder);
  long tSeen = 0;
  g_failRead = [&](const std::string& path) {
    if (path == t && ++tSeen == tTotal) split.damagedPrefix = t;
    return false;
  };
  arm(split, LibraryUpdate::kIndexNames.path, 0, -1);
  {
    Device d(split, card);
    d.boot();
    g_failRead = nullptr;
    split.faultFrom = -1;
    split.damagedPrefix.clear();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Unread);
    TEST_ASSERT_TRUE(d.booted.decision.sessionOnly);
    TEST_ASSERT_TRUE(d.booted.readErrors);
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    assertWhole(d, tracks, "T damaged");
  }
}

// A file's own fault, every read answered: a boot that held on to it would hold for ever.
void test_a_damaged_or_cut_library_idx_is_its_own_fault() {
  using libraryboot::Action;
  using libraryboot::Saved;
  for (int kind = 0; kind < 3; ++kind) {
    CutFs fs;
    TestCard card;
    const uint32_t tracks = filledCard(fs, card, Filled::Transfer);
    std::vector<uint8_t> bytes = fs.bytes(LibraryUpdate::kIndexNames.path);
    if (kind == 0) fs.damagedPrefix = LibraryUpdate::kIndexNames.path;
    if (kind == 1) fs.put(LibraryUpdate::kIndexNames.path, std::vector<uint8_t>(bytes.begin(), bytes.begin() + 16));
    if (kind == 2) {
      bytes[bytes.size() / 2] ^= 0xFF;
      fs.put(LibraryUpdate::kIndexNames.path, bytes);
    }
    Device d(fs, card);
    d.boot();
    fs.damagedPrefix.clear();
    TEST_ASSERT_TRUE(d.booted.saved == Saved::Corrupt);
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Build);
    TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
    TEST_ASSERT_FALSE(d.booted.decision.sessionOnly);
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
  }
}

// library.tmp alone, a cut between the remove and the rename: its check's read the card
// fails is no verdict. It stays, the boot holds, and the next one takes it.
void test_library_tmp_outlives_a_read_the_card_fails() {
  using libraryboot::Action;
  using libraryboot::Saved;
  CutFs fs;
  TestCard card;
  const uint32_t tracks = filledCard(fs, card, Filled::Transfer);
  const std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
  fs.remove(LibraryUpdate::kIndexNames.path);
  fs.put(LibraryUpdate::kIndexNames.tmp, idx);
  const long events = bootEvents(fs, card, "/.player/library");
  TEST_ASSERT_GREATER_THAN_INT32(3, events);
  for (long k = 0; k < events; ++k) {
    char msg[64];
    snprintf(msg, sizeof(msg), "read %ld of %ld", k, events);
    CutFs once = fs.reboot(cutfs::Variant::InOrder);
    arm(once, "/.player/library", k, k + 1);
    {
      Device d(once, card);
      d.boot();
      once.faultFrom = -1;
      TEST_ASSERT_TRUE_MESSAGE(d.booted.saved == Saved::Unread, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
      TEST_ASSERT_TRUE_MESSAGE(once.bytes(LibraryUpdate::kIndexNames.tmp) == idx ||
                                   once.bytes(LibraryUpdate::kIndexNames.path) == idx,
                               msg);
    }
    Device next(once, card);
    next.boot();
    TEST_ASSERT_TRUE_MESSAGE(next.booted.decision.action == Action::Load, msg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, next.index.trackCount(), msg);
    TEST_ASSERT_TRUE_MESSAGE(once.bytes(LibraryUpdate::kIndexNames.path) == idx, msg);
    TEST_ASSERT_FALSE_MESSAGE(once.exists(LibraryUpdate::kIndexNames.tmp), msg);
  }
}

// D's and its journals' reads, each in its turn: nothing goes over them, whichever
// library.idx the boot finds. One that says it is a library is loaded, whole; none to
// load, /music walked for the session. No byte written, the jobs held, and the next boot
// finds it all.
void test_a_read_of_the_records_the_card_fails_keeps_them_and_the_library() {
  using libraryboot::Action;
  using libraryboot::Saved;
  for (Filled how : {Filled::Mixed, Filled::Hand, Filled::Transfer}) {
    CutFs fs;
    TestCard card;
    const uint32_t tracks = filledCard(fs, card, how);
    const std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
    enum Row { Same, Other, Marker, Gone, Spoilt, Older };
    for (Row row : {Same, Other, Marker, Gone, Spoilt, Older}) {
      CutFs at = fs.reboot(cutfs::Variant::InOrder);
      if (row == Other) {
        if (how == Filled::Hand) continue;  // (no root, no other identity)
        commit(at, card, 2);
      }
      if (row == Marker) at.put(LibraryUpdate::kMarker, {});
      if (row == Gone) at.remove(LibraryUpdate::kIndexNames.path);
      if (row == Spoilt) {
        std::vector<uint8_t> bad = idx;
        bad[bad.size() / 2] ^= 0xFF;
        at.put(LibraryUpdate::kIndexNames.path, bad);
      }
      if (row == Older) {
        std::vector<uint8_t> old = idx;
        old[4] = 5;  // (the version, little-endian)
        at.put(LibraryUpdate::kIndexNames.path, old);
      }
      const bool loads = row == Same || row == Other || row == Marker;
      const auto files = filesOf(at);
      const long events = openEvents(at, "/.player/tags");
      TEST_ASSERT_GREATER_THAN_INT32(4, events);
      for (long k = 0; k < events; ++k) {
        char msg[80];
        snprintf(msg, sizeof(msg), "filled %d, row %d, read %ld of %ld", static_cast<int>(how), row, k, events);
        for (bool lasting : {false, true}) {
          arm(at, "/.player/tags", k, lasting ? -1 : k + 1);
          const int walks = g_walks;
          {
            Device d(at, card);
            d.boot();
            at.faultFrom = -1;
            TEST_ASSERT_TRUE_MESSAGE(d.store->readFailed(), msg);
            TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
            TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.action == (loads ? Action::Load : Action::Walk), msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(loads ? walks : walks + 1, g_walks, msg);
            TEST_ASSERT_EQUAL_INT_MESSAGE(!loads, d.booted.sessionWalked(), msg);
            assertWhole(d, tracks, msg);
            assertQueueKept(d, tracks, msg);
            TEST_ASSERT_FALSE_MESSAGE(d.booted.markerRemoved, msg);
            TEST_ASSERT_TRUE_MESSAGE(filesOf(at) == files, msg);
          }
        }
      }
      // The card answering: as a boot with nothing wrong.
      Device next(at, card);
      next.boot();
      TEST_ASSERT_FALSE(next.booted.decision.holdJobs);
      TEST_ASSERT_TRUE(next.booted.ok);
      TEST_ASSERT_TRUE(next.store->device().present);
      TEST_ASSERT_TRUE(at.violations.empty());
    }
  }
}

// T's and D's reads as a boot's build reads them (a new transfer, a deferred build): a
// library.idx that says it is one comes back, nothing saved, the marker kept, the jobs
// held. T or D damaged is the file's fault, the build stands.
void test_a_read_the_card_fails_in_a_boots_build_goes_over_nothing() {
  using libraryboot::Action;
  using libraryboot::Saved;
  for (const char* prefix : {"/.mstream/tags-00000002", "/.player/tags.bin"}) {
    CutFs fs;
    TestCard card;
    const uint32_t tracks = filledCard(fs, card, Filled::Mixed);
    commit(fs, card, 2);
    const auto files = filesOf(fs);
    // The reads of a clean build, past what the boot's root and open read.
    CutFs counted = fs.reboot(cutfs::Variant::InOrder);
    arm(counted, prefix, 1000000, -1);
    long head = 0;
    {
      // (The store's and the root's reads come first; then the build's, counted here.)
      ts::TagStore st(counted, storeConfig());
      st.open();
      std::unique_ptr<cardroot::Root> r(new cardroot::Root());
      std::vector<uint8_t> scratch(1024);
      cardroot::read(counted, scratch.data(), 1024, r.get());
      head = counted.faultEvents;
    }
    counted.faultEvents = 0;
    {
      Device d(counted, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.ok);
      TEST_ASSERT_TRUE(d.booted.built);
    }
    const long all = counted.faultEvents;
    TEST_ASSERT_GREATER_THAN_INT32(head, all);
    int kept = 0;
    for (long k = head; k < all; ++k) {
      char msg[96];
      snprintf(msg, sizeof(msg), "%s, read %ld of %ld", prefix, k, all);
      CutFs once = fs.reboot(cutfs::Variant::InOrder);
      arm(once, prefix, k, k + 1);
      Device d(once, card);
      d.boot();
      once.faultFrom = -1;
      if (!d.booted.keptSaved) continue;  // (the last reads are the reload's, of a saved build)
      ++kept;
      TEST_ASSERT_TRUE_MESSAGE(d.booted.ok, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.readErrors, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.action == Action::Load, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, d.index.trackCount(), msg);
      assertQueueKept(d, tracks, msg);
      TEST_ASSERT_TRUE_MESSAGE(filesOf(once) == files, msg);
    }
    TEST_ASSERT_GREATER_THAN_INT(0, kept);
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
  // A deferred build, each of D's opens and reads as it reads them: the same, and the
  // marker stays for a boot that can read them.
  {
    CutFs fs;
    TestCard card;
    const uint32_t tracks = filledCard(fs, card, Filled::Mixed);
    fs.put(LibraryUpdate::kMarker, {});
    const auto files = filesOf(fs);
    const long head = openEvents(fs, "/.player/tags.bin");
    for (long k = head; k < head + 12; ++k) {
      char msg[48];
      snprintf(msg, sizeof(msg), "D's read %ld", k);
      arm(fs, "/.player/tags.bin", k, k + 1);
      Device d(fs, card);
      d.boot();
      fs.faultFrom = -1;
      TEST_ASSERT_TRUE_MESSAGE(d.booted.marker, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.keptSaved, msg);
      TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
      TEST_ASSERT_FALSE_MESSAGE(d.booted.markerRemoved, msg);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(tracks, d.index.trackCount(), msg);
      TEST_ASSERT_TRUE_MESSAGE(filesOf(fs) == files, msg);
    }
    // With a journal, the compaction's read of D failing: no build, the same.
    {
      ts::TagStore st(fs, storeConfig());
      st.open();
      uint8_t buf[2048];
      ts::ChunkBuilder chunk;
      chunk.begin(buf, sizeof(buf));
      uint32_t size = 0, time = 0;
      TEST_ASSERT_TRUE(card.stat("Later/02 - h.mp3", 16, &size, &time));
      mptg::Record rec;
      rec.size = size;
      rec.fatTime = time;
      const char* f[cc::kRunFields] = {};
      for (auto& x : f) x = "";
      TEST_ASSERT_TRUE(chunk.add("Later/02 - h.mp3", 16, ts::Status::Scanned, rec, f));
      TEST_ASSERT_TRUE(st.append(chunk));
    }
    const auto journalled = filesOf(fs);
    // (One read: D whole for a build after it, from tags.bin alone, which would leave
    // the journal out, and save that over library.idx.)
    const long opens = openEvents(fs, "/.player/tags.bin");
    arm(fs, "/.player/tags.bin", opens + 1, opens + 2);
    Device d(fs, card);
    d.boot();
    fs.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.compacted);
    TEST_ASSERT_TRUE(d.booted.compaction.readFailed);
    TEST_ASSERT_TRUE(d.booted.keptSaved);
    TEST_ASSERT_FALSE(d.booted.built);
    TEST_ASSERT_TRUE(filesOf(fs) == journalled);
  }
  // Nothing to keep (library.idx missing, of an older version, damaged): a part of a
  // library is no better. /music walked for the session, nothing saved, the jobs held, and
  // the next boot builds.
  for (const char* prefix : {"/.mstream/tags-00000001", "/.player/tags.bin"}) {
    for (int row = 0; row < 3; ++row) {
      CutFs at;
      TestCard card;
      const uint32_t tracks = filledCard(at, card, Filled::Mixed);
      std::vector<uint8_t> idx = at.bytes(LibraryUpdate::kIndexNames.path);
      if (row == 0) at.remove(LibraryUpdate::kIndexNames.path);
      if (row == 1) {
        idx[4] = 5;  // (the version, little-endian)
        at.put(LibraryUpdate::kIndexNames.path, idx);
      }
      if (row == 2) {
        idx[idx.size() / 2] ^= 0xFF;
        at.put(LibraryUpdate::kIndexNames.path, idx);
      }
      const auto files = filesOf(at);
      // (The store's and the root's reads come first.)
      CutFs counted = at.reboot(cutfs::Variant::InOrder);
      arm(counted, prefix, 1000000, -1);
      long head = 0;
      {
        ts::TagStore st(counted, storeConfig());
        st.open();
        std::unique_ptr<cardroot::Root> r(new cardroot::Root());
        std::vector<uint8_t> scratch(1024);
        cardroot::read(counted, scratch.data(), 1024, r.get());
        head = counted.faultEvents;
      }
      for (long k = head; k < head + 8; ++k) {
        char msg[96];
        snprintf(msg, sizeof(msg), "%s, row %d, read %ld", prefix, row, k);
        const int walks = g_walks;
        arm(at, prefix, k, k + 1);
        {
          Device d(at, card);
          d.boot();
          at.faultFrom = -1;
          TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.action == Action::Walk, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.decision.holdJobs, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.readErrors, msg);
          TEST_ASSERT_TRUE_MESSAGE(d.booted.sessionWalked(), msg);
          TEST_ASSERT_EQUAL_INT_MESSAGE(walks + 1, g_walks, msg);
          assertWhole(d, tracks, msg);
          TEST_ASSERT_TRUE_MESSAGE(filesOf(at) == files, msg);
        }
      }
      Device next(at, card);
      next.boot();
      TEST_ASSERT_TRUE(next.booted.ok);
      TEST_ASSERT_FALSE(next.booted.decision.holdJobs);
      TEST_ASSERT_EQUAL_UINT32(tracks, next.index.trackCount());
    }
  }
  // No T, D's opens for the build all failing: not "no records", which walks and saves.
  {
    CutFs at;
    TestCard card;
    const uint32_t tracks = filledCard(at, card, Filled::Hand);
    at.remove(LibraryUpdate::kIndexNames.path);
    arm(at, "/.player/tags.bin", openEvents(at, "/.player/tags.bin"), -1);
    Device d(at, card);
    d.boot();
    at.faultFrom = -1;
    TEST_ASSERT_TRUE(d.booted.sessionWalked());
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_FALSE(at.exists(LibraryUpdate::kIndexNames.path));
    assertWhole(d, tracks, "D's opens");
  }
  // The compaction's reload failing (it replaced D, then a read of the new one): D reads
  // as none, only because the card failed the read. A build then is T alone.
  for (bool withIdx : {true, false}) {
    CutFs at;
    TestCard card;
    const uint32_t tracks = filledCard(at, card, Filled::Mixed);
    {
      ts::TagStore st(at, storeConfig());
      st.open();
      uint8_t buf[2048];
      ts::ChunkBuilder chunk;
      chunk.begin(buf, sizeof(buf));
      uint32_t size = 0, time = 0;
      TEST_ASSERT_TRUE(card.stat("Later/02 - h.mp3", 16, &size, &time));
      mptg::Record rec;
      rec.size = size;
      rec.fatTime = time;
      const char* f[cc::kRunFields] = {};
      for (auto& x : f) x = "";
      TEST_ASSERT_TRUE(chunk.add("Later/02 - h.mp3", 16, ts::Status::Scanned, rec, f));
      TEST_ASSERT_TRUE(st.append(chunk));
    }
    if (withIdx) {
      at.put(LibraryUpdate::kMarker, {});
    } else {
      at.remove(LibraryUpdate::kIndexNames.path);
    }
    const std::vector<uint8_t> idx = at.bytes(LibraryUpdate::kIndexNames.path);
    // The last read of tags.bin a clean boot's compaction makes, and on.
    CutFs counted = at.reboot(cutfs::Variant::InOrder);
    long reloadAt = 0;
    {
      const long opens = openEvents(at, "/.player/tags.bin");
      arm(counted, "/.player/tags.bin", 1000000, -1);
      ts::TagStore st(counted, storeConfig());
      st.open();
      TEST_ASSERT_TRUE(st.compact().ok);
      reloadAt = counted.faultEvents - 1;
      (void)opens;
    }
    arm(at, "/.player/tags.bin", reloadAt, -1);
    {
      Device d(at, card);
      d.boot();
      at.faultFrom = -1;
      TEST_ASSERT_TRUE(d.booted.compacted);
      TEST_ASSERT_FALSE(d.booted.compaction.readFailed);
      TEST_ASSERT_TRUE(d.store->readFailed());
      TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
      TEST_ASSERT_FALSE(d.booted.markerRemoved);
      TEST_ASSERT_TRUE(idx == at.bytes(LibraryUpdate::kIndexNames.path));
      assertWhole(d, tracks, "the reload");
      TEST_ASSERT_EQUAL_INT(!withIdx, d.booted.sessionWalked());
    }
  }
  // D's own records damaged, found as a new transfer's build reads it (its open read
  // whole): its fault. Built without it, saved, nothing held, for ever or otherwise.
  {
    CutFs at;
    TestCard card;
    filledCard(at, card, Filled::Mixed);
    commit(at, card, 2);
    {
      CutFs dry = at.reboot(cutfs::Variant::InOrder);
      Device d(dry, card);
      d.boot();
      TEST_ASSERT_TRUE(d.booted.build.deviceUsed);
    }
    const long atOpen = openEvents(at, "/.player/tags.bin") - 1;  // (less its openRead)
    long seen = 0;
    g_failRead = [&](const std::string& path) {
      if (path == "/.player/tags.bin" && ++seen == atOpen + 1) at.damagedPrefix = path;
      return false;
    };
    Device d(at, card);
    d.boot();
    g_failRead = nullptr;
    at.damagedPrefix.clear();
    TEST_ASSERT_TRUE(d.booted.readErrors);
    TEST_ASSERT_FALSE(d.booted.keptSaved);
    TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.ok);
    TEST_ASSERT_FALSE(d.booted.build.deviceUsed);
    TEST_ASSERT_FALSE(d.booted.sessionWalked());
  }
  // T's own records damaged: its fault. Built without it, saved, nothing held.
  CutFs fs;
  TestCard card;
  filledCard(fs, card, Filled::Transfer);
  commit(fs, card, 2);
  fs.damagedPrefix = tagsName(2);
  Device d(fs, card);
  d.boot();
  fs.damagedPrefix.clear();
  TEST_ASSERT_FALSE(d.root->readFailed());
  TEST_ASSERT_FALSE(d.booted.keptSaved);
  TEST_ASSERT_FALSE(d.booted.decision.holdJobs);
  TEST_ASSERT_TRUE(d.booted.ok);
}

// No walk for the session to make, or no memory for it: no library, nothing written.
void test_a_session_walk_that_cant_run_is_no_library() {
  using libraryboot::Action;
  CutFs fs;
  TestCard card;
  filledCard(fs, card, Filled::Transfer);
  const auto files = filesOf(fs);
  arm(fs, "/.player/", 0, -1);
  {
    Device d(fs, card);
    g_ceiling = g_live + 64;
    d.boot();
    g_ceiling = SIZE_MAX;
    TEST_ASSERT_TRUE(d.booted.decision.action == Action::Walk);
    TEST_ASSERT_TRUE(d.booted.decision.holdJobs);
    TEST_ASSERT_TRUE(d.booted.noMemory);
    TEST_ASSERT_FALSE(d.booted.ok);
    TEST_ASSERT_FALSE(d.index.ready());
  }
  fs.faultFrom = -1;
  TEST_ASSERT_TRUE(filesOf(fs) == files);
}

// The update step, after a compaction the card failed a read of D in: the build would
// read the same D, behind the fence, and leave no library until the next boot. It ends
// before its fence, the library as it was.
void test_a_read_of_d_the_card_fails_in_the_compaction_ends_the_step_before_its_fence() {
  CutFs fs;
  TestCard card;
  const uint32_t tracks = filledCard(fs, card, Filled::Hand);
  {
    ts::TagStore st(fs, storeConfig());
    st.open();
    uint8_t buf[2048];
    ts::ChunkBuilder chunk;
    chunk.begin(buf, sizeof(buf));
    uint32_t size = 0, time = 0;
    TEST_ASSERT_TRUE(card.stat("Loose/03 - f.mp3", 16, &size, &time));
    mptg::Record rec;
    rec.size = size;
    rec.fatTime = time;
    const char* f[cc::kRunFields] = {};
    for (auto& x : f) x = "";
    f[cc::kTitle] = "From the journal";
    TEST_ASSERT_TRUE(chunk.add("Loose/03 - f.mp3", 16, ts::Status::Scanned, rec, f));
    TEST_ASSERT_TRUE(st.append(chunk));
  }
  const std::vector<uint8_t> d0 = fs.bytes("/.player/tags.bin");
  const std::vector<uint8_t> idx = fs.bytes(LibraryUpdate::kIndexNames.path);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.ok);
  TEST_ASSERT_TRUE(d.store->hasJournals());
  arm(fs, "/.player/tags.bin", 2, -1);
  d.update("the scan's end");
  fs.faultFrom = -1;
  TEST_ASSERT_TRUE(d.did(Do::Failed));
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_TRUE(d.index.ready());
  TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());
  TEST_ASSERT_TRUE(d0 == fs.bytes("/.player/tags.bin"));
  TEST_ASSERT_TRUE(idx == fs.bytes(LibraryUpdate::kIndexNames.path));
  TEST_ASSERT_TRUE(d.store->hasJournals());
  // The compaction whole and the read of the new D failing (its reload): the records
  // read as none in memory, not on the card. A step on that would build from T alone.
  CutFs reload = fs.reboot(cutfs::Variant::InOrder);
  arm(reload, "/.player/tags.bin", 1000000, -1);
  long lastReads = 0;
  {
    ts::TagStore st(reload, storeConfig());
    st.open();
    reload.faultEvents = 0;
    TEST_ASSERT_TRUE(st.compact().ok);
    lastReads = reload.faultEvents;
  }
  CutFs at = fs.reboot(cutfs::Variant::InOrder);
  Device m(at, card);
  m.boot();
  TEST_ASSERT_TRUE(m.booted.ok);
  arm(at, "/.player/tags.bin", lastReads - 1, -1);  // (the reload's last read, and on)
  const ts::TagStore::Compacted c = m.store->compact();
  TEST_ASSERT_FALSE(c.ok);
  TEST_ASSERT_FALSE(c.readFailed);
  TEST_ASSERT_TRUE(m.store->readFailed());
  m.update("the scan's end");
  at.faultFrom = -1;
  TEST_ASSERT_TRUE(m.did(Do::Failed));
  TEST_ASSERT_FALSE(m.did(Do::Fence));
  TEST_ASSERT_TRUE(idx == at.bytes(LibraryUpdate::kIndexNames.path));
  TEST_ASSERT_EQUAL_UINT32(tracks, m.index.trackCount());
}

// The boot's readers read in order, whatever failed: the card, with its whole /.player
// or everything failing, keeps it all, and shows the whole library, or /music walked.
void test_every_read_failing_leaves_a_whole_library() {
  for (Filled how : {Filled::Mixed, Filled::Hand, Filled::Transfer}) {
    for (const char* prefix : {"/.player/", "/.mstream/", "/"}) {
      CutFs fs;
      TestCard card;
      const uint32_t tracks = filledCard(fs, card, how);
      const auto files = filesOf(fs);
      const long events = bootEvents(fs, card, prefix);
      for (long k = 0; k < events; ++k) {
        char msg[80];
        snprintf(msg, sizeof(msg), "filled %d, %s, read %ld of %ld", static_cast<int>(how), prefix, k, events);
        for (bool lasting : {false, true}) {
          arm(fs, prefix, k, lasting ? -1 : k + 1);
          Device d(fs, card);
          d.boot();
          fs.faultFrom = -1;
          // (A plan's own read is the status line's, and leaves the root whole.)
          assertWhole(d, tracks, msg);
          assertQueueKept(d, tracks, msg);
          TEST_ASSERT_TRUE_MESSAGE(filesOf(fs) == files, msg);
          TEST_ASSERT_TRUE_MESSAGE(fs.violations.empty(), msg);
        }
      }
    }
  }
}

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
// The safe point follows the measured pause (3.4.2; the 2026-10-09 device
// run, B2: the fence up 17.8-18.6 s with audio at 20k, over the 16 s that
// should grow it). The pure rule: the pause and kSafeMarginMs, at least
// 20 s, at most kSafeCapMs; before any step, the estimate from the tracks
// (about 24 s at N11's 19,410, the floor below about 15,000). Then on the
// rig: a step whose fence lasts 22 s moves the next one's safe point to
// 27 s: with 26 s left of the track it waits, with 27 s it goes; a failed
// build's fence doesn't count.
// ---------------------------------------------------------------------------
void test_the_safe_point_follows_the_pause() {
  using U = LibraryUpdate;
  TEST_ASSERT_EQUAL_UINT32(20000, U::safeLeftFor(0));
  TEST_ASSERT_EQUAL_UINT32(20000, U::safeLeftFor(15000));
  TEST_ASSERT_EQUAL_UINT32(20001, U::safeLeftFor(15001));
  TEST_ASSERT_EQUAL_UINT32(23600, U::safeLeftFor(18600));  // the lit fence of 6c2a928's and 062bd1f's gb
  TEST_ASSERT_EQUAL_UINT32(30000, U::safeLeftFor(25000));
  TEST_ASSERT_EQUAL_UINT32(30000, U::safeLeftFor(UINT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(19410, U::pauseFor(19410));
  TEST_ASSERT_EQUAL_UINT32(24410, U::safeLeftFor(U::pauseFor(19410)));
  TEST_ASSERT_EQUAL_UINT32(20000, U::safeLeftFor(U::pauseFor(2000)));
  TEST_ASSERT_TRUE(U::safePoint(true, false, 24410, U::kSeekQuietMs, 24410));
  TEST_ASSERT_FALSE(U::safePoint(true, false, 24409, U::kSeekQuietMs, 24410));
  TEST_ASSERT_TRUE(U::safePoint(false, false, 0, 0, 24410));  // nothing plays: safe whatever it wants

  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  // Before any step: the estimate (six tracks: the floor).
  TEST_ASSERT_EQUAL_UINT32(0, d.upd->lastPauseMs());
  TEST_ASSERT_EQUAL_UINT32(U::kSafeLeftMs, d.upd->safeLeftMs());
  // A build of 1,100 passes of 20 ms: the fence up about 22 s.
  d.buildPasses = 1100;
  d.update("the scan's end");
  TEST_ASSERT_TRUE(d.upd->last().saved);
  const uint32_t pause = d.upd->lastPauseMs();
  TEST_ASSERT_EQUAL_UINT32(d.upd->last().fenceMs, pause);
  TEST_ASSERT_TRUE(pause >= 22000 && pause <= 22100);
  const uint32_t want = pause + U::kSafeMarginMs;
  TEST_ASSERT_EQUAL_UINT32(want, d.upd->safeLeftMs());
  // The next step: a track with a second less than that left waits; then
  // the next track, long enough, goes.
  d.buildPasses = 5;
  d.upd->ask("gb");
  Env e;
  e.playing = true;
  e.trackLeftMs = want - 1000;
  for (int i = 0; i < 40; ++i) {
    d.pass(e);
    TEST_ASSERT_FALSE(d.lastOut.fenced);
  }
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  e.trackLeftMs = want;
  const size_t from = d.acts.size();
  d.runUntil([&] { return std::find(d.acts.begin() + static_cast<long>(from), d.acts.end(), Do::Saved) != d.acts.end(); },
             e);
  // That fence was short (5 passes): the safe point comes back down, to the floor.
  TEST_ASSERT_TRUE(d.upd->lastPauseMs() < 1000);
  TEST_ASSERT_EQUAL_UINT32(U::kSafeLeftMs, d.upd->safeLeftMs());
  // A build that fails (out of PSRAM) leaves the last measure as it was.
  d.buildPasses = 1100;
  d.update("gb");
  const uint32_t measured = d.upd->lastPauseMs();
  TEST_ASSERT_TRUE(measured >= 22000);
  g_ceiling = g_live;  // nothing more for the build
  d.buildPasses = 5;
  d.update("gb");
  g_ceiling = SIZE_MAX;
  TEST_ASSERT_FALSE(d.upd->last().built);
  TEST_ASSERT_EQUAL_UINT32(measured, d.upd->lastPauseMs());
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

// ---------------------------------------------------------------------------
// A failed first walk (the device, 2026-10-08: a walk.jnl write the card
// refused, CardWalk's Error::Sink). The scan then read the loop's sources,
// the update step at its end built from D, which held those files alone,
// and 19,410 tracks became 204, saved. Here (the inverse of the
// investigation's host test, where 6 tracks became 2): the update step
// refuses while the card's records don't list it (no T, D never walked),
// before its fence and its memory check (no marker), and the index keeps
// every track; a boot that would build from such a D walks /music instead;
// once a walk lists the card, the update builds every track, the scanned
// ones with their tags.
// ---------------------------------------------------------------------------
void test_a_failed_first_walk_keeps_the_library() {
  CutFs fs;
  TestCard card;
  fill(card);
  Device d(fs, card);
  d.boot();
  TEST_ASSERT_TRUE(d.booted.walked);  // no records: /music walked, every track by its path
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  TEST_ASSERT_TRUE(d.upd->recordsListCard());  // none at all: a build walks /music
  TEST_ASSERT_FALSE(d.upd->walkListsCard());   // ... but no walk listed the card
  // The boot's walk, its walk.jnl write refused.
  d.jobs.askWalk();
  fs.refuse = true;
  d.runUntil([&] { return d.running == Job::None && !d.jobs.walkWork() && !d.jobs.walking(); });
  fs.refuse = false;
  TEST_ASSERT_EQUAL_UINT32(1, d.jobs.counts().walksFailed);
  TEST_ASSERT_FALSE(d.store->hasWalk());
  TEST_ASSERT_FALSE(d.jobs.restWork());  // D none: no rest; the loop's sources only
  // Right after the failure the records are still none, which a build could
  // take (recordsListCard()); CardTasks' line and its hold go by
  // walkListsCard(), false from here to the walk that lists the card, so the
  // failure's line says "no update step until a walk lists the card" on
  // this first boot too (2026-10-09's review: it didn't).
  TEST_ASSERT_TRUE(d.upd->recordsListCard());
  TEST_ASSERT_FALSE(d.upd->walkListsCard());
  // The loop's sources: the playing track and the queue's next.
  const char* const read[] = {"Artist/Album/01 - a.flac", "Artist/Album/02 - b.opus"};
  for (const char* rel : read) {
    TEST_ASSERT_TRUE(d.jobs.prepare(Job::Scan, ScanScheduler::Source::Playing, rel, std::strlen(rel), d.now));
    d.jobs.step();
    TEST_ASSERT_TRUE(d.jobs.finish().read);
  }
  TEST_ASSERT_TRUE(d.jobs.flushChunk());
  TEST_ASSERT_TRUE(d.jobs.newRecords());  // the scan's end would ask (CardTasks doesn't, after a failed walk)
  TEST_ASSERT_FALSE(d.upd->recordsListCard());  // the journal holds the two, nothing listed the card
  TEST_ASSERT_FALSE(d.upd->walkListsCard());
  // The update step (the scan's end; gb): its compaction makes D of the two
  // records alone; then it refuses, before its fence.
  d.update("the scan's end");
  TEST_ASSERT_TRUE(d.did(Do::Failed));
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_TRUE(d.upd->last().unlisted);
  TEST_ASSERT_FALSE(d.upd->last().cardGone);
  TEST_ASSERT_TRUE(d.store->device().present);
  TEST_ASSERT_FALSE(d.store->device().header.walked);
  TEST_ASSERT_TRUE(d.index.ready());
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());  // every track kept
  // gb! the same: no marker (the next boot would build those two alone).
  d.acts.clear();
  d.update("gb!", Env(), true);
  TEST_ASSERT_TRUE(d.did(Do::Failed));
  TEST_ASSERT_FALSE(d.did(Do::Deferred));
  TEST_ASSERT_TRUE(d.upd->last().unlisted);
  TEST_ASSERT_FALSE(fs.exists(LibraryUpdate::kMarker));
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  // A boot that must build (library.idx gone) from such a D: /music walked.
  {
    CutFs copy = fs;
    copy.remove(LibraryUpdate::kIndexNames.path);
    Device b(copy, card);
    b.boot();
    TEST_ASSERT_TRUE(b.booted.ok);
    TEST_ASSERT_TRUE(b.booted.walked);
    TEST_ASSERT_EQUAL_UINT32(kAudio, b.index.trackCount());
    TEST_ASSERT_EQUAL_UINT32(kAudio, pendingIn(b.index));
  }
  // The walk again (CardTasks' retry, or the next boot's): it lists the
  // card, and the update step builds every track, the two read with their
  // tags, the rest's scan with theirs.
  d.jobs.askWalk();
  d.drain();
  TEST_ASSERT_EQUAL_UINT32(1, d.jobs.counts().walksFailed);
  TEST_ASSERT_TRUE(d.upd->recordsListCard());
  TEST_ASSERT_TRUE(d.upd->walkListsCard());  // (a walk to merge)
  d.acts.clear();
  d.update("the scan's end");
  TEST_ASSERT_TRUE(d.did(Do::Saved));
  TEST_ASSERT_FALSE(d.upd->last().unlisted);
  TEST_ASSERT_TRUE(d.store->device().header.walked);
  TEST_ASSERT_TRUE(d.upd->walkListsCard());  // (D walked)
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());
  TEST_ASSERT_EQUAL_UINT32(0, pendingIn(d.index));
  TEST_ASSERT_EQUAL_STRING(kFlacTitle, titleOf(d.index, kFlac).c_str());
}

// ---------------------------------------------------------------------------
// Auto DJ and the update step (the engine design's 2.3, 3.2 and 3.3).
// ---------------------------------------------------------------------------
namespace {

// A pick due as the queue's last entry plays, Auto DJ on; its index joining.
autodj::TriggerIn owedPick() {
  autodj::TriggerIn in;
  in.on = true;
  in.joining = true;
  in.queueSize = 3;
  in.upNext = 0;
  in.play = PlayState::Playing;
  return in;
}

class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

// An AutoDJ index of the card's files `rels` (row i is rels[i]), as the
// transfer software would write it.
std::vector<uint8_t> autoDjIndexOf(const std::vector<std::string>& rels) {
  const size_t n = rels.size();
  std::vector<uint64_t> paths(n);
  std::vector<float> e(2 * n);
  std::vector<mpdj::RowIn> rows(n);
  for (size_t i = 0; i < n; ++i) {
    paths[i] = cc::pathHash(rels[i].c_str());
    e[2 * i] = static_cast<float>(std::cos(0.4 * static_cast<double>(i)));
    e[2 * i + 1] = static_cast<float>(std::sin(0.4 * static_cast<double>(i)));
    rows[i].hash[0] = static_cast<uint8_t>(i + 1);  // (canonical-hash order: row i)
    rows[i].paths = &paths[i];
    rows[i].pathCount = 1;
    rows[i].bpm10 = static_cast<uint16_t>(1000 + 50 * i);
    rows[i].embedding = &e[2 * i];
  }
  mpdj::TableIn t;
  t.generation = 2;
  t.cardId = kCardId;
  t.k = 4;
  t.selKind = mpdj::kSelTop;
  t.modelId = "synthetic-embedding";
  t.modelVersion = "1";
  t.license = "CC0-1.0";
  t.dim = 2;
  t.rows = rows.data();
  t.rowCount = static_cast<uint32_t>(n);
  VecSink out;
  uint8_t sig[16];
  const char* error = nullptr;
  if (!mpdj::write(out, t, sig, nullptr, nullptr, &error)) TEST_FAIL_MESSAGE(error);
  return out.bytes;
}

}  // namespace

// A pick owed while Auto DJ's index joins (autodj::decide()'s owed) keeps
// the fence off (Wait::AutoDj), after the safe point; once it is made the
// step goes on. Behind the fence it changes nothing. A held Auto DJ (Repeat
// One, the sleep timer's End of queue, its failure pause) owes none, its
// join running or not: it never blocks a step.
void test_auto_dj_owed_holds_the_fence() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  d.upd->ask("the scan's end");
  Env e;
  e.playing = true;
  e.trackLeftMs = 180000;
  e.autoDjOwed = autodj::decide(owedPick()).owed;
  TEST_ASSERT_TRUE(e.autoDjOwed);
  // The compaction first (nothing of the fence's), then the wait.
  d.runUntil([&] { return !d.store->hasJournals() && d.running == Job::None; }, e);
  for (int i = 0; i < 200; ++i) {
    d.pass(e);
    TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::AutoDj);
    TEST_ASSERT_TRUE(d.lastOut.holdScan);
    TEST_ASSERT_FALSE(d.lastOut.fenced);
    TEST_ASSERT_FALSE(d.lastOut.updating);
  }
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  // The safe point is asked first: a track near its end is that wait.
  e.trackLeftMs = 1000;
  d.pass(e);
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::SafePoint);
  e.trackLeftMs = 180000;
  d.pass(e);
  TEST_ASSERT_TRUE(d.lastOut.wait == LibraryUpdate::Wait::AutoDj);
  // The pick made (the last entry has a next): the fence goes up; owed
  // again behind it, nothing changes.
  e.autoDjOwed = false;
  d.runUntil([&] { return d.did(Do::Fence); }, e);
  e.autoDjOwed = true;
  d.runUntil([&] { return d.did(Do::Saved); }, e);
  TEST_ASSERT_TRUE(d.upd->last().saved);
  // Held: no pick owed, the step goes on whatever the join does.
  const std::function<void(autodj::TriggerIn&)> holds[] = {
      [](autodj::TriggerIn& in) { in.repeatOne = true; },
      [](autodj::TriggerIn& in) { in.sleepEndOfQueue = true; },
      [](autodj::TriggerIn& in) { in.failurePaused = true; },
  };
  for (const auto& hold : holds) {
    autodj::TriggerIn in = owedPick();
    hold(in);
    const autodj::TriggerOut o = autodj::decide(in);
    TEST_ASSERT_TRUE(o.hold != autodj::Hold::None);
    Env h;
    h.playing = true;
    h.trackLeftMs = 180000;
    h.autoDjOwed = o.owed;
    TEST_ASSERT_FALSE(h.autoDjOwed);
    d.acts.clear();
    d.update("gb", h);
    TEST_ASSERT_TRUE(d.did(Do::Saved));
  }
}

// The join and the update step: a join under way when the step is asked
// owes the playing last entry its pick, so the fence waits for it; the pick
// made, the fence goes up and releases the join's maps first (enterFence():
// autoDj.fence(), its bytes the step's to count); after the Live step the
// join runs again on the rebuilt index (leaveFence()), and its maps are the
// new library's: a file added since has its row, every id the new one.
void test_the_join_after_a_live_step() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  const std::vector<std::string> rels = {"Artist/Album/01 - a.flac", "Artist/Album/02 - b.opus",
                                         "Artist/Other/01 - c.mp3",  "Band/Live/01 - d.mp3",
                                         "Band/Live/02 - e.mp3",     "Loose/03 - f.mp3",
                                         "Aardvark Band/Sample/01 - g.mp3"};
  const std::vector<uint8_t> file = autoDjIndexOf(rels);
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpdj::Table table;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(table.open(src)));
  Device d(fs, card);
  d.boot();
  d.update("the scan's end");  // (the index of the files' readings)
  TEST_ASSERT_EQUAL_UINT32(kAudio, d.index.trackCount());

  // Joined on this library: six of the seven rows have a track.
  autodj::Join join;
  TEST_ASSERT_TRUE(join.begin(table, d.upd->readable()) == autodj::Join::Status::Working);
  while (join.status() == autodj::Join::Status::Working) join.step(d.upd->readable());
  TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Ready);
  TEST_ASSERT_EQUAL_UINT32(kAudio, join.maps()->mapped);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, join.maps()->rowToTrack[6]);
  std::vector<uint32_t> before(rels.size(), LibraryIndex::kNone);
  for (size_t i = 0; i < kAudio; ++i) before[i] = d.index.findTrack(("/music/" + rels[i]).c_str());

  // A new file, walked in; the step asked. A join begins (the card came
  // back, say) while the queue's last entry plays: until it is ready, the
  // pick is owed. Its slices are skipped while a list moves (every 8th
  // pass here).
  card.addBytes(rels[6], corpus("ape_v1.mp3"));
  d.jobs.askWalk();
  d.drain();
  d.acts.clear();
  d.upd->ask("a walk's news");
  Env e;
  e.playing = true;
  e.trackLeftMs = 1000;  // (held at the safe point until the join begins)
  d.runUntil([&] { return d.lastOut.wait == LibraryUpdate::Wait::SafePoint; }, e);
  join.setConfig(autodj::Join::Config{4096, 1, 1024});
  TEST_ASSERT_TRUE(join.begin(table, d.upd->readable()) == autodj::Join::Status::Working);
  e.trackLeftMs = 180000;
  size_t freed = 0;
  int owedWaits = 0, rejoins = 0;
  bool picked = false;
  d.beforeFence = [&] {
    TEST_ASSERT_TRUE(picked);  // (no fence while it was owed)
    TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Ready);
    freed = join.release();
  };
  d.afterLive = [&] {
    TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Idle);
    TEST_ASSERT_TRUE(join.begin(table, d.upd->readable()) == autodj::Join::Status::Working);
    ++rejoins;
  };
  for (int i = 0; i < 20000 && !d.did(Do::Saved); ++i) {
    if (i % 8 == 0 && join.status() == autodj::Join::Status::Working) join.step(d.upd->readable());
    // Auto DJ's step: owed while it joins; picked as soon as it is ready.
    autodj::TriggerIn in = owedPick();
    in.joining = join.status() == autodj::Join::Status::Working;
    in.upNext = picked ? 1 : 0;
    if (!in.joining && !picked && autodj::decide(in).pick) picked = true;
    in.upNext = picked ? 1 : 0;
    e.autoDjOwed = autodj::decide(in).owed;
    d.pass(e);
    if (d.lastOut.wait == LibraryUpdate::Wait::AutoDj) ++owedWaits;
  }
  TEST_ASSERT_TRUE(d.did(Do::Saved));
  TEST_ASSERT_TRUE(owedWaits > 10);
  TEST_ASSERT_TRUE(picked);
  TEST_ASSERT_TRUE(freed > 0);
  TEST_ASSERT_EQUAL_INT(1, rejoins);
  // The rejoin, to its end, on the new index.
  while (join.status() == autodj::Join::Status::Working) join.step(d.upd->readable());
  TEST_ASSERT_TRUE(join.status() == autodj::Join::Status::Ready);
  const autodj::Maps& m = *join.maps();
  TEST_ASSERT_EQUAL_UINT32(kAudio + 1, d.index.trackCount());
  TEST_ASSERT_EQUAL_UINT32(kAudio + 1, m.tracks);
  TEST_ASSERT_EQUAL_UINT32(kAudio + 1, m.mapped);
  bool moved = false;
  for (size_t i = 0; i < rels.size(); ++i) {
    const uint32_t t = d.index.findTrack(("/music/" + rels[i]).c_str());
    TEST_ASSERT_TRUE(t != LibraryIndex::kNone);
    TEST_ASSERT_EQUAL_UINT16(i, m.trackToRow[t]);
    TEST_ASSERT_EQUAL_UINT16(t, m.rowToTrack[i]);
    moved = moved || (i < kAudio && t != before[i]);
  }
  TEST_ASSERT_TRUE(moved);  // (the new file took an id before others: the old maps were stale)
  const size_t held = join.heldBytes();
  TEST_ASSERT_EQUAL_size_t(held, join.release());
}

// The memory check and Auto DJ's join (LibraryUpdate::setGiveBack(), the
// device checks' first boot of 2026-10-13): a file walked in makes the new
// track table a little bigger than the old one's block, and the largest
// free block doesn't hold it while the join is held at that block's top.
// The check has the join given back (its bytes leave alsoFreed for the free
// count, PSRAM measured again) and the step goes on as with Auto DJ off;
// still short, it defers. Never asked when the table fits (the old block,
// or the largest free one), when the room is short (the join is counted in
// it already), when gb! defers, or more than once a step.
namespace {

struct GiveBack {
  int calls = 0;
  size_t give = 0;          // the join's bytes (0: none held)
  size_t freeGain = SIZE_MAX;  // the free bytes measured again grow by this (SIZE_MAX: the bytes given)
  size_t largestAfter = 0;  // PSRAM's largest free block once they are back
  size_t freeSeen = 0, largestSeen = 0;
};

size_t giveBackHook(void* ctx, size_t* psramFree, size_t* psramLargest) {
  GiveBack* g = static_cast<GiveBack*>(ctx);
  ++g->calls;
  g->freeSeen = *psramFree;
  g->largestSeen = *psramLargest;
  if (!g->give) return 0;
  *psramFree += g->freeGain == SIZE_MAX ? g->give : g->freeGain;
  *psramLargest = g->largestAfter;
  const size_t given = g->give;
  g->give = 0;  // (given back: none held until it joins again)
  return given;
}

}  // namespace

void test_the_memory_check_has_auto_djs_join_given_back() {
  CutFs fs;
  TestCard card;
  handFilled(fs, card);
  Device d(fs, card);
  d.boot();
  d.update("the scan's end");  // (the index of the files' readings)
  const uint32_t tracks = d.index.trackCount();
  // A file walked in: the next build's table (after the step's
  // compaction) is a track bigger than the old one's block (trimmed to its
  // tracks), so it needs a free block of its own.
  card.addBytes("Aardvark Band/Sample/01 - g.mp3", corpus("ape_v1.mp3"));
  d.jobs.askWalk();
  d.drain();
  GiveBack g;
  d.upd->setGiveBack(giveBackHook, &g);
  Env e;
  e.psramLargest = 16;  // the join at the largest block's top (a 7-track table: a few hundred bytes)
  e.alsoFreed = 155284;   // (its bytes, in what the step frees)

  // Nothing to give back (Auto DJ off, or not joined): deferred.
  d.update("nothing held", e);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_TRUE(d.upd->verdict().shortOf == LibraryUpdate::Short::Table);
  TEST_ASSERT_EQUAL_INT(1, g.calls);
  TEST_ASSERT_EQUAL_size_t(16, g.largestSeen);
  TEST_ASSERT_EQUAL_size_t(0, d.upd->last().givenBack);
  // Given back, but the block still short (the join's blocks not next to
  // it): deferred, the hook asked once.
  d.acts.clear();
  g = GiveBack();
  g.give = 155284;
  g.largestAfter = 16;
  d.update("still short", e);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_EQUAL_INT(1, g.calls);
  TEST_ASSERT_TRUE(d.upd->verdict().shortOf == LibraryUpdate::Short::Table);
  TEST_ASSERT_EQUAL_size_t(155284, d.upd->last().givenBack);
  TEST_ASSERT_EQUAL_size_t(16, d.upd->last().largestBefore);
  // The room short (free PSRAM, the join already counted): not asked.
  d.acts.clear();
  g = GiveBack();
  g.give = 155284;
  g.largestAfter = 64u << 20;
  Env room = e;
  room.psramFree = 1024;
  room.alsoFreed = 0;
  d.update("short of room", room);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_TRUE(d.upd->verdict().shortOf == LibraryUpdate::Short::Room);
  TEST_ASSERT_EQUAL_INT(0, g.calls);
  // gb!: deferred as asked, not asked.
  d.acts.clear();
  d.update("gb!", e, true);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_TRUE(d.upd->deferForced());
  TEST_ASSERT_EQUAL_INT(0, g.calls);
  TEST_ASSERT_EQUAL_UINT32(tracks, d.index.trackCount());  // (the index as it was every time)
  TEST_ASSERT_EQUAL_UINT32(4, d.upd->deferrals());
  // The free bytes after the give-back are what the heap says then: the
  // join's bytes leave alsoFreed whatever it says, never counted twice.
  // Here the heap shows none of them free (taken again meanwhile): the
  // room falls short by them and the step defers (counted twice, it would
  // pass, a build 155 KB short of the check).
  d.acts.clear();
  g = GiveBack();
  g.give = 155284;
  g.freeGain = 0;
  g.largestAfter = 64u << 20;
  Env tight = e;
  tight.psramFree = 0;
  d.update("given back, no freer", tight);
  TEST_ASSERT_TRUE(d.did(Do::Deferred));
  TEST_ASSERT_FALSE(d.did(Do::Fence));
  TEST_ASSERT_EQUAL_INT(1, g.calls);
  TEST_ASSERT_EQUAL_size_t(155284, d.upd->last().givenBack);
  const LibraryUpdate::Verdict v = d.upd->verdict();
  TEST_ASSERT_TRUE(v.shortOf == LibraryUpdate::Short::Room);
  TEST_ASSERT_TRUE(static_cast<uint64_t>(v.room) * 10 < static_cast<uint64_t>(v.peak) * 11);
  TEST_ASSERT_TRUE(static_cast<uint64_t>(v.room + 155284) * 10 >= static_cast<uint64_t>(v.peak) * 11);
  TEST_ASSERT_EQUAL_UINT32(5, d.upd->deferrals());
  // Given back next to the block: the step goes on as with Auto DJ off,
  // the join's bytes out of alsoFreed (they're free now), and builds.
  d.acts.clear();
  g = GiveBack();
  g.give = 155284;
  g.largestAfter = 64u << 20;
  d.update("given back", e);
  TEST_ASSERT_TRUE(d.did(Do::Fence));
  TEST_ASSERT_TRUE(d.did(Do::Saved));
  TEST_ASSERT_EQUAL_INT(1, g.calls);
  TEST_ASSERT_EQUAL_size_t(64u << 20, g.freeSeen);
  TEST_ASSERT_EQUAL_size_t(155284, d.upd->last().givenBack);
  TEST_ASSERT_TRUE(d.upd->verdict().shortOf == LibraryUpdate::Short::None);
  TEST_ASSERT_TRUE(d.upd->last().saved);
  TEST_ASSERT_TRUE(d.upd->last().markerRemoved);  // (the deferrals' marker)
  TEST_ASSERT_EQUAL_UINT32(tracks + 1, d.index.trackCount());
  // The table fits now (the old block is the new one's size): never asked,
  // however small the largest free block.
  d.acts.clear();
  g = GiveBack();
  g.give = 155284;
  e.psramLargest = 16;
  d.update("fits", e);
  TEST_ASSERT_TRUE(d.did(Do::Saved));
  TEST_ASSERT_EQUAL_INT(0, g.calls);
  TEST_ASSERT_EQUAL_size_t(0, d.upd->last().givenBack);
  TEST_ASSERT_TRUE(fs.violations.empty());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_safe_point_and_the_memory_check);
  RUN_TEST(test_every_row_of_the_boot);
  RUN_TEST(test_a_root_the_card_fails_to_read_at_the_boot_keeps_the_library);
  RUN_TEST(test_a_read_of_library_idx_the_card_fails_builds_nothing_over_it);
  RUN_TEST(test_the_boot_reads_library_idx_again_while_the_card_fails);
  RUN_TEST(test_library_idx_unread_and_nothing_whole_to_build_from_is_walked_for_the_session);
  RUN_TEST(test_a_session_build_keeps_the_marker_and_folds_the_journals);
  RUN_TEST(test_a_damaged_or_cut_library_idx_is_its_own_fault);
  RUN_TEST(test_library_tmp_outlives_a_read_the_card_fails);
  RUN_TEST(test_a_read_of_the_records_the_card_fails_keeps_them_and_the_library);
  RUN_TEST(test_a_read_the_card_fails_in_a_boots_build_goes_over_nothing);
  RUN_TEST(test_a_session_walk_that_cant_run_is_no_library);
  RUN_TEST(test_a_read_of_d_the_card_fails_in_the_compaction_ends_the_step_before_its_fence);
  RUN_TEST(test_every_read_failing_leaves_a_whole_library);
  RUN_TEST(test_library_tmp_after_a_cut);
  RUN_TEST(test_a_deferral_then_a_boot_that_builds);
  RUN_TEST(test_a_track_end_near_the_safe_point);
  RUN_TEST(test_the_safe_point_follows_the_pause);
  RUN_TEST(test_a_compaction_asked_during_a_build);
  RUN_TEST(test_a_bad_transfer_found_at_the_end_of_a_build);
  RUN_TEST(test_a_power_cut_at_each_stage);
  RUN_TEST(test_the_fence);
  RUN_TEST(test_the_steps_order_and_a_card_that_doesnt_answer);
  RUN_TEST(test_a_card_that_fails_mid_build);
  RUN_TEST(test_the_worker_s_task_before_the_fence);
  RUN_TEST(test_the_marker_s_build_out_of_psram);
  RUN_TEST(test_a_failed_first_walk_keeps_the_library);
  RUN_TEST(test_auto_dj_owed_holds_the_fence);
  RUN_TEST(test_the_join_after_a_live_step);
  RUN_TEST(test_the_memory_check_has_auto_djs_join_given_back);
  return UNITY_END();
}
