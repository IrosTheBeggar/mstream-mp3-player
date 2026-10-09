// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// What the card worker's writers and readers ask of the card (docs/
// METADATA.md 3.3.7, 3.4.2; the 2026-10-09 device run), counted on the host
// FatFs model (test/support/FatModel.h: ChaN's FatFs R0.15 as the Core2
// builds it, the 256-sector SectorCache and the firmware's CachedDrive in
// front) over a 32 GB FAT32 card with 16 KB clusters, through a file like
// src/storage/CardFat's (FatModelFs.h). The device run measured every one
// of a compaction's card writes a single sector, each read first, and
// library.idx's save half single-sector writes: the cost is card commands,
// not megabytes, so these tests count commands.
// - A 20k scan in the user's shape, its compactions as the firmware runs
//   them (100 files a chunk, a compaction when the journal asks): most card
//   writes are whole 4 KB pieces (8 sectors) from a 4 KB boundary, a few
//   single sectors (each section's ends), no write over 8 sectors (the
//   decoder waits for FatFs's lock at most one piece), three syncs a
//   compaction; the reads past the cache bounded too.
// - The same scan (6,000 files) with 2026-10-08's buffer sizes (512 B,
//   4 KB D, 512 B), CardFat's unaligned pieces, and with odd sizes: the
//   bytes of every D the same (alignment moves only where a flush ends);
//   with the old sizes every write a single sector, five times the writes
//   at least. (The section cursors end even 512 B buffers on a sector now:
//   6c2a928's partial-sector reads before each write aren't in this
//   count; the r3 investigation's host model of 6c2a928's code has them,
//   METADATA.md 3.3.7.)
// - library.idx's save at 20k (LibraryIndex::save() through an unbuffered
//   sink, as LibraryUpdate's): 4 KB pieces from the file's 4 KB boundaries,
//   few single-sector writes; unaligned (before) about half.
// - The validation walk's KnownD over a Scanned D at the card worker's
//   scratch (CardJobs' kKnownScratch), and the update step's build from it:
//   cc::Stream's refills are whole sectors from a sector's start, few
//   single-sector reads.
// - A Rescan's compaction at 20k and the next (no chunks: the merge's small
//   arena), HIDX's sort in the buffers the second pass left idle and 4 KB
//   reads: 7 passes, not 15, about 3,000 card reads fewer (B4 of the
//   2026-10-09 device run).
// Run: pio test -e native -f test_card_io
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../support/FatModel.h"
#include "../support/FatModelFs.h"
#include "../support/SynthCard.h"
#include "ByteStream.h"
#include "CachedDrive.h"
#include "CardContract.h"
#include "CardJobs.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "SectorCache.h"
#include "TagStore.h"
#include "TagStoreWalk.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace ts = tagstore;
using fatmodel::drive;
using fatmodel::RamDisk;
using ts::Status;

namespace {

// The run's card: 62,333,952 sectors (32 GB), FAT32, 16 KB clusters, the
// cache and the firmware's wrapper in front, /music's files taking the
// card's first clusters (a stand-in of 32 MB), /.player after them.
struct Card {
  RamDisk disk{62333952u};
  SectorCache cache;
  fatmodel::Slot slot{0};
  CachedDrive wrapper{cache, slot};
  uint8_t scratch[fatmodel::kSS];
  FATFS fs;
  bool ok = false;
  Card() {
    if (!cache.begin(SectorCache::kDefaultEntries)) return;
    wrapper.setScratch(scratch);
    drive(0).disk = &disk;
    drive(0).wrapper = &wrapper;
    if (fatmodel::format(0, 16384) != FR_OK || fatmodel::mount(0, &fs) != FR_OK) return;
    wrapper.remember(disk.sectors());
    f_mkdir("0:/.player");
    f_mkdir("0:/music");
    FIL f;
    std::vector<uint8_t> blk(16384, 0xA5);
    if (f_open(&f, "0:/music/filler.bin", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return;
    UINT put = 0;
    for (uint32_t i = 0; i < 2048; ++i) f_write(&f, blk.data(), 16384, &put);
    f_close(&f);
    disk.counts.mostWriteSectors = 0;  // (the format's and the filler's)
    ok = true;
  }
  ~Card() {
    f_mount(nullptr, "0:", 0);
    drive(0).disk = nullptr;
    drive(0).wrapper = nullptr;
    drive(0).cache = nullptr;
    drive(0).noInit = false;
    drive(0).asked = fatmodel::Drive::Asked();
  }
};

// What a stretch of work asked of the card (past the cache) and of FatFs.
struct Took {
  uint64_t writes = 0, singleWrites = 0, writeSectors = 0;
  uint64_t reads = 0, singleReads = 0, readSectors = 0;
  uint64_t syncs = 0, backSeeks = 0;
  uint32_t mostWriteSectors = 0;
};
struct Probe {
  const Card& card;
  const fatmodel::ModelFs& fs;
  RamDisk::Counts c0;
  fatmodel::FileCounts f0;
  Probe(const Card& c, const fatmodel::ModelFs& f) : card(c), fs(f), c0(c.disk.counts), f0(f.counts) {}
  Took took() const {
    const RamDisk::Counts& c = card.disk.counts;
    Took t;
    t.writes = c.writes - c0.writes;
    t.singleWrites = c.singleWrites - c0.singleWrites;
    t.writeSectors = c.writeSectors - c0.writeSectors;
    t.reads = c.reads - c0.reads;
    t.singleReads = c.singleReads - c0.singleReads;
    t.readSectors = c.readSectors - c0.readSectors;
    t.syncs = fs.counts.syncs - f0.syncs;
    t.backSeeks = fs.counts.backSeeks - f0.backSeeks;
    return t;
  }
};

void print(const char* what, const Took& t) {
  printf("[card io] %s: %llu card writes (%llu of one sector, %llu sectors), %llu reads past the cache (%llu of one "
         "sector, %llu sectors), %llu syncs, %llu seeks back\n",
         what, (unsigned long long)t.writes, (unsigned long long)t.singleWrites, (unsigned long long)t.writeSectors,
         (unsigned long long)t.reads, (unsigned long long)t.singleReads, (unsigned long long)t.readSectors,
         (unsigned long long)t.syncs, (unsigned long long)t.backSeeks);
}

uint64_t fileHash(const char* path, uint32_t* bytes) {
  FIL f;
  uint64_t h = 1469598103934665603ull;
  *bytes = 0;
  if (f_open(&f, path, FA_READ) != FR_OK) return 0;
  std::vector<uint8_t> b(4096);
  UINT got = 0;
  while (f_read(&f, b.data(), 4096, &got) == FR_OK && got) {
    for (UINT i = 0; i < got; ++i) h = (h ^ b[i]) * 1099511628211ull;
    *bytes += got;
  }
  f_close(&f);
  return h;
}

// The host's clock for a compaction's parts (the firmware's esp_timer).
uint64_t hostUs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

ts::TagStore::Config config() {
  ts::TagStore::Config c;  // the firmware's buffers: its defaults (src/app/Library.cpp)
  c.producer = "mstream-player 0.8.0";
  c.parserVersion = 1;
  c.nowUs = hostUs;
  return c;
}

// 2026-10-08's buffers, before the device run.
ts::TagStore::Config oldConfig() {
  ts::TagStore::Config c = config();
  c.runBuffer = 512;
  c.deviceBuffer = 4096;
  c.writeBuffer = 512;
  return c;
}

struct Compaction {
  Took took;
  uint32_t dBytes = 0;
  uint64_t dHash = 0;
  uint32_t hidxPasses = 0;
  ts::TagStore::Compacted parts;  // its times (the host's)
};

// A fresh card's first walk (every file Pending, every folder's facts),
// then its scan in canonical order (CardJobs: a chunk every 100 files, a
// compaction when the journal asks, and one at the scan's end), as the
// firmware's jobs write them.
bool scanCard(fatmodel::ModelFs& fs, const Card& card, const ts::TagStore::Config& cfg, uint32_t tracks,
              std::vector<Compaction>* out) {
  const synthcard::Card sc = synthcard::make(synth::userShape(tracks));
  ts::TagStore st(fs, cfg);
  st.open();
  std::vector<std::string> folders;
  for (const auto& f : sc.files) {
    std::string d = f.rel;
    for (size_t k; (k = d.rfind('/')) != std::string::npos;) {
      d.resize(k);
      folders.push_back(d);
    }
  }
  folders.push_back("");
  std::sort(folders.begin(), folders.end(), [](const std::string& a, const std::string& b) {
    return cc::compareFolderPaths(a.data(), a.size(), b.data(), b.size()) < 0;
  });
  folders.erase(std::unique(folders.begin(), folders.end()), folders.end());
  std::vector<const synthcard::File*> order;
  for (const auto& f : sc.files) order.push_back(&f);
  std::sort(order.begin(), order.end(), [](const synthcard::File* a, const synthcard::File* b) {
    return cc::compareFilePaths(a->rel.data(), a->rel.size(), b->rel.data(), b->rel.size()) < 0;
  });
  {
    std::vector<uint8_t> wbuf(4096);
    ts::WalkWriter w;
    if (!st.beginWalk(ts::Identity(), wbuf.data(), static_cast<uint32_t>(wbuf.size()), &w)) return false;
    for (const std::string& d : folders) {
      ts::FolderFacts ff;
      ff.digest = cc::fnv1a64(d.data(), d.size()) | 1;
      ff.audio = 8;
      ff.images = 1;
      ff.others = 1;
      std::strcpy(ff.image, "cover.jpg");
      ff.imageLength = 9;
      ff.imageRank = 1;
      if (!w.folder(d.data(), d.size(), ff)) return false;
    }
    for (const synthcard::File* f : order)
      if (!w.file(f->rel.data(), f->rel.size(), f->size, f->fatTime, 0, Status::Pending)) return false;
    if (!w.endRun(0) || !w.endRun(0) || !w.finish()) return false;
  }
  std::vector<uint8_t> chunkBuf(32 * 1024);
  ts::ChunkBuilder cb;
  cb.begin(chunkBuf.data(), static_cast<uint32_t>(chunkBuf.size()));
  for (size_t i = 0; i < order.size(); ++i) {
    const mptg::RecordIn r = synthcard::recordOf(*order[i], true, 0);
    if (!cb.add(r.path, std::strlen(r.path), Status::Scanned, r.rec, r.fields)) return false;
    const bool last = i + 1 == order.size();
    if (cb.count() < 100 && cb.bytes() < 16384 && !last) continue;
    if (!st.append(cb)) return false;
    if (!st.wantsCompaction() && !last) continue;
    const Probe p(card, fs);
    const ts::TagStore::Compacted c = st.compact();
    if (!c.ok) return false;
    Compaction k;
    k.took = p.took();
    k.parts = c;
    k.hidxPasses = c.hidxPasses;
    k.dHash = fileHash("0:/.player/tags.bin", &k.dBytes);
    out->push_back(k);
  }
  return true;
}

// LibraryUpdate's FileOut: each block where the last ended, unbuffered.
class FileOut : public ByteSink {
public:
  explicit FileOut(ts::File& f) : f_(f) {}
  bool write(const void* data, size_t n) override {
    if (!f_.write(at_, data, static_cast<uint32_t>(n))) return false;
    at_ += static_cast<uint32_t>(n);
    return true;
  }

private:
  ts::File& f_;
  uint32_t at_ = 0;
};

// What a stretch of FatFs work at most for a single-sector write share.
bool mostlyWhole(const Took& t, double singleShare) {
  return t.writes > 0 && static_cast<double>(t.singleWrites) <= singleShare * static_cast<double>(t.writes);
}

}  // namespace

void setUp() {}
void tearDown() {}

// The firmware's compactions at 20k (N11's card, 19,410 files): eight, each
// writing whole pieces.
void test_compactions_write_whole_pieces() {
  Card card;
  TEST_ASSERT_TRUE(card.ok);
  fatmodel::ModelFs fs;
  std::vector<Compaction> ks;
  const auto t0 = std::chrono::steady_clock::now();
  TEST_ASSERT_TRUE(scanCard(fs, card, config(), 19410, &ks));
  TEST_ASSERT_EQUAL_UINT32(8, ks.size());
  Took all;
  for (size_t i = 0; i < ks.size(); ++i) {
    const Took& t = ks[i].took;
    char what[96];
    snprintf(what, sizeof(what), "compaction %u (D %u B, HIDX %u passes)", (unsigned)(i + 1), ks[i].dBytes,
             ks[i].hidxPasses);
    print(what, t);
    // Its parts' times (TagStore::Config::nowUs; the host's, the device's
    // line gives the Core2's): each counted, within the whole.
    const ts::TagStore::Compacted& c = ks[i].parts;
    printf("[card io]   its time on the host: %u ms (the first pass %u, the second %u, HIDX's sort %u)\n",
           (unsigned)c.totalMs, (unsigned)c.pass1Ms, (unsigned)c.pass2Ms, (unsigned)c.sortMs);
    TEST_ASSERT_TRUE_MESSAGE(c.pass1Ms + c.pass2Ms + c.sortMs <= c.totalMs + 3, what);
    // Whole 4 KB pieces: a single-sector write is a section's first or last
    // piece (or FatFs's FAT and directory sectors), not every write.
    TEST_ASSERT_TRUE_MESSAGE(mostlyWhole(t, 0.15), what);
    // The written sectors in pieces of 8 or so: at most a quarter as many
    // writes as sectors (before: one write a sector).
    TEST_ASSERT_TRUE_MESSAGE(t.writes * 4 <= t.writeSectors, what);
    // Three syncs: the output's, its close, hidx.tmp's close.
    TEST_ASSERT_TRUE_MESSAGE(t.syncs <= 3, what);
    // The reads: D twice through the walker's 2 KB streams and the folder
    // cursor's, the journal's 32 runs through 1 KB each, hidx.tmp's passes.
    // Bounded by D's sectors: about 2.8 reads a sector of D at 20k (it was
    // about 3.6 before, every one a single sector).
    TEST_ASSERT_TRUE_MESSAGE(t.reads <= 4u * (ks[i].dBytes / 512u + 1000u), what);
    // HIDX's sort in the buffers the second pass left idle too, hidx.tmp
    // read 4 KB at a time: 4 passes at 19,410 pairs (5-6 in the arena alone,
    // 1 KB at a time: about 1,000 more reads a compaction, B4).
    TEST_ASSERT_TRUE_MESSAGE(ks[i].hidxPasses <= 4, what);
    all.writes += t.writes;
    all.singleWrites += t.singleWrites;
    all.writeSectors += t.writeSectors;
    all.reads += t.reads;
    all.singleReads += t.singleReads;
    all.readSectors += t.readSectors;
    all.syncs += t.syncs;
    if (t.mostWriteSectors > all.mostWriteSectors) all.mostWriteSectors = t.mostWriteSectors;
  }
  print("the scan's compactions", all);
  // No card write over 8 sectors: the decoder waits for FatFs's lock one
  // 4 KB piece at most.
  TEST_ASSERT_TRUE(card.disk.counts.mostWriteSectors <= 8);
  // The last compaction of a 20k scan: D about 3.75 MB.
  TEST_ASSERT_TRUE(ks.back().dBytes > 3500000u && ks.back().dBytes < 4000000u);
  printf("[card io] host time %.0f ms\n",
         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

// The same scan (smaller) with 2026-10-08's buffer sizes and CardFat's
// unaligned pieces, and with odd sizes: every D byte for byte the same; the
// writes many more with the old sizes.
void test_aligned_buffers_write_the_same_bytes() {
  std::vector<Compaction> before, after;
  {
    Card card;
    TEST_ASSERT_TRUE(card.ok);
    fatmodel::ModelFs fs;
    fs.alignPieces = false;
    TEST_ASSERT_TRUE(scanCard(fs, card, oldConfig(), 6000, &before));
  }
  {
    Card card;
    TEST_ASSERT_TRUE(card.ok);
    fatmodel::ModelFs fs;
    TEST_ASSERT_TRUE(scanCard(fs, card, config(), 6000, &after));
  }
  // Buffers of other sizes (a section's flushes ending elsewhere).
  std::vector<Compaction> odd;
  {
    Card card;
    TEST_ASSERT_TRUE(card.ok);
    fatmodel::ModelFs fs;
    ts::TagStore::Config c = config();
    c.writeBuffer = 6000;
    c.runBuffer = 700;
    c.deviceBuffer = 5000;
    TEST_ASSERT_TRUE(scanCard(fs, card, c, 6000, &odd));
  }
  TEST_ASSERT_EQUAL_UINT32(before.size(), after.size());
  TEST_ASSERT_EQUAL_UINT32(before.size(), odd.size());
  TEST_ASSERT_TRUE(before.size() >= 2);
  Took b, a;
  for (size_t i = 0; i < before.size(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(before[i].dBytes, after[i].dBytes);
    TEST_ASSERT_TRUE(before[i].dHash == after[i].dHash);
    TEST_ASSERT_EQUAL_UINT32(before[i].dBytes, odd[i].dBytes);
    TEST_ASSERT_TRUE(before[i].dHash == odd[i].dHash);
    b.writes += before[i].took.writes;
    b.singleWrites += before[i].took.singleWrites;
    b.reads += before[i].took.reads;
    b.singleReads += before[i].took.singleReads;
    a.writes += after[i].took.writes;
    a.singleWrites += after[i].took.singleWrites;
    a.reads += after[i].took.reads;
    a.singleReads += after[i].took.singleReads;
  }
  print("6k: 2026-10-08's buffer sizes, unaligned pieces", b);
  print("6k: now", a);
  TEST_ASSERT_TRUE(b.singleWrites * 10 >= b.writes * 9);  // before: nearly every write a single sector
  TEST_ASSERT_TRUE(a.writes * 5 <= b.writes);             // now: a fifth of the writes at most
  TEST_ASSERT_TRUE(a.reads * 3 <= b.reads * 2);           // and two thirds of the reads at most
}

// library.idx's save at 20k (LibraryUpdate::save(): the index straight to
// library.tmp through an unbuffered sink, then 2.12.6's replace()).
void test_save_writes_whole_pieces() {
  LibraryIndex idx;
  const synth::Spec spec = synth::userShape(19410);
  idx.begin(spec.root);
  synth::addTracks(idx, spec);
  idx.finish();
  TEST_ASSERT_TRUE(idx.ready());
  const ts::Names names{"/.player/library.idx", "/.player/library.tmp", "/.player/library"};
  Took took[2];
  uint64_t hash[2] = {};
  uint32_t bytes[2] = {};
  for (int aligned = 0; aligned < 2; ++aligned) {
    Card card;
    TEST_ASSERT_TRUE(card.ok);
    fatmodel::ModelFs fs;
    fs.alignPieces = aligned == 1;
    const Probe p(card, fs);
    TEST_ASSERT_TRUE(ts::prepareTmp(fs, names));
    ts::File* f = fs.open(names.tmp, ts::Fs::Mode::Create);
    TEST_ASSERT_NOT_NULL(f);
    {
      FileOut out(*f);
      LibraryIndex::Inputs in;
      TEST_ASSERT_TRUE(idx.save(out, in));
    }
    TEST_ASSERT_TRUE(fs.close(f));
    TEST_ASSERT_TRUE(ts::replace(fs, names));
    took[aligned] = p.took();
    hash[aligned] = fileHash("0:/.player/library.idx", &bytes[aligned]);
    TEST_ASSERT_TRUE(card.disk.counts.mostWriteSectors <= 8);
  }
  print("library.idx's save, unaligned pieces (before)", took[0]);
  print("library.idx's save", took[1]);
  printf("[card io] library.idx %u B\n", bytes[1]);
  TEST_ASSERT_EQUAL_UINT32(bytes[0], bytes[1]);
  TEST_ASSERT_TRUE(hash[0] == hash[1]);
  TEST_ASSERT_TRUE(bytes[1] > 1000000u);
  TEST_ASSERT_TRUE(took[0].singleWrites * 3 >= took[0].writes);  // before: about half single sectors
  TEST_ASSERT_TRUE(mostlyWhole(took[1], 0.10));                  // now: under a tenth
  TEST_ASSERT_TRUE(took[1].writes * 3 <= took[0].writes * 2);     // and fewer writes
  // About one card write a 4 KB piece, with FatFs's FAT and directory.
  TEST_ASSERT_TRUE(took[1].writes <= bytes[1] / 4096u + 64u);
}

// The validation walk's D (KnownD) over a Scanned D at the card worker's
// scratch, and the update step's build from it (LibraryUpdate's three opens
// of D: the merge, DSTA's rows, DFLD's facts): their streams refill whole
// sectors, several at a time.
void test_walk_and_build_read_in_runs() {
  Card card;
  TEST_ASSERT_TRUE(card.ok);
  fatmodel::ModelFs fs;
  std::vector<Compaction> ks;
  TEST_ASSERT_TRUE(scanCard(fs, card, config(), 19410, &ks));
  ts::TagStore st(fs, config());
  st.open();
  Took took[2];
  const uint32_t scratch[2] = {4096, cardjobs::Jobs::kKnownScratch};
  for (int k = 0; k < 2; ++k) {
    card.cache.clear();  // cold, as at a boot's walk
    std::vector<uint8_t> buf(scratch[k]);
    const Probe p(card, fs);
    ts::KnownD d;
    TEST_ASSERT_TRUE(d.begin(st, buf.data(), scratch[k]));
    cardwalk::KnownFolder kf;
    cardwalk::KnownFile kfile;
    uint32_t folders = 0, files = 0;
    while (d.nextFolder(&kf)) {
      ++folders;
      while (d.nextFile(&kfile)) ++files;
    }
    TEST_ASSERT_FALSE(d.failed());
    TEST_ASSERT_EQUAL_UINT32(19410, files);
    d.close();
    took[k] = p.took();
    char what[64];
    snprintf(what, sizeof(what), "KnownD over D, %u B scratch", scratch[k]);
    print(what, took[k]);
  }
  // Before: the whole of D in single-sector reads. Now most of its sectors
  // come in multi-sector reads, in a fraction of the card commands.
  TEST_ASSERT_TRUE(took[0].singleReads * 10 >= took[0].reads * 9);
  TEST_ASSERT_TRUE(took[1].reads * 4 <= took[0].reads);
  TEST_ASSERT_TRUE(took[1].singleReads * 4 <= took[1].reads);

  // The build: LibraryUpdate::buildIndex()'s wiring, with its buffers.
  card.cache.clear();
  const Probe p(card, fs);
  ts::File* d = fs.open(st.devicePath(), ts::Fs::Mode::Read);
  ts::File* rowsF = fs.open(st.devicePath(), ts::Fs::Mode::Read);
  ts::File* factsF = fs.open(st.devicePath(), ts::Fs::Mode::Read);
  TEST_ASSERT_TRUE(d && rowsF && factsF);
  std::vector<uint8_t> bufs(1024 + 3072);
  ts::BuilderRows rows;
  ts::BuilderFacts facts;
  TEST_ASSERT_TRUE(rows.begin(*rowsF, bufs.data(), 1024));
  TEST_ASSERT_TRUE(facts.begin(*factsF, bufs.data() + 1024, 3072));
  LibraryIndex index;
  LibraryBuilder builder;
  LibraryBuilder::Config bc;
  bc.root = "/music";
  bc.device = d;
  bc.rows = &rows;
  bc.facts = &facts;
  const LibraryBuilder::Result r = builder.build(index, bc);
  TEST_ASSERT_TRUE(r.built);
  TEST_ASSERT_EQUAL_UINT32(19410, r.fromDevice);
  for (ts::File* f : {d, rowsF, factsF}) fs.close(f);
  const Took b = p.took();
  print("the build from D", b);
  // D once (its sections through the walker's 2 KB streams, DSTA, DFLD and
  // the folders' names again): whole sectors, few single-sector reads.
  TEST_ASSERT_TRUE(b.singleReads * 10 <= b.reads);
  TEST_ASSERT_TRUE(b.readSectors <= 8000);
}

// A compaction with no chunks at 20k: a Rescan's (every Scanned row turned
// Pending), as `gr` asks, and the one right after (nothing to merge). Its
// arena is the merge's small one (D and two walk runs), where HIDX's sort
// took 15 passes over hidx.tmp in 1 KB reads (the 2026-10-09 run's B4: the
// Rescan's compaction 20.8 s on the device); the sort has the buffers the
// second pass left idle too now, and reads 4 KB at a time.
void test_a_rescans_compaction() {
  Card card;
  TEST_ASSERT_TRUE(card.ok);
  fatmodel::ModelFs fs;
  std::vector<Compaction> ks;
  TEST_ASSERT_TRUE(scanCard(fs, card, config(), 19410, &ks));
  for (int k = 0; k < 2; ++k) {
    ts::TagStore st(fs, config());
    st.open();
    card.cache.clear();
    const Probe p(card, fs);
    const ts::TagStore::Compacted c = st.compact(k == 0);
    TEST_ASSERT_TRUE(c.ok);
    TEST_ASSERT_EQUAL(k == 0, c.rescanned);
    TEST_ASSERT_EQUAL_UINT32(0, c.chunksMerged);
    uint32_t bytes = 0;
    fileHash("0:/.player/tags.bin", &bytes);
    const Took t = p.took();
    char what[128];
    snprintf(what, sizeof(what), "%s compaction, no chunks (D %u B, HIDX %u passes, %u B of work memory)",
             k == 0 ? "a Rescan's" : "the next", bytes, c.hidxPasses, (unsigned)c.workBytes);
    print(what, t);
    printf("[card io]   its time on the host: %u ms (the first pass %u, the second %u, HIDX's sort %u)\n",
           (unsigned)c.totalMs, (unsigned)c.pass1Ms, (unsigned)c.pass2Ms, (unsigned)c.sortMs);
    // 15 passes before (about 3,400 reads of 2 sectors for hidx.tmp alone).
    TEST_ASSERT_TRUE_MESSAGE(c.hidxPasses <= 8, what);
    // The reads: D twice (2.46 MB of Pending rows) and hidx.tmp's passes:
    // 9,002 reads (25,531 sectors) before.
    TEST_ASSERT_TRUE_MESSAGE(t.reads <= 6500, what);
    TEST_ASSERT_TRUE_MESSAGE(t.readSectors <= 23000, what);
    TEST_ASSERT_TRUE_MESSAGE(mostlyWhole(t, 0.15), what);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_compactions_write_whole_pieces);
  RUN_TEST(test_aligned_buffers_write_the_same_bytes);
  RUN_TEST(test_save_writes_whole_pieces);
  RUN_TEST(test_walk_and_build_read_in_runs);
  RUN_TEST(test_a_rescans_compaction);
  return UNITY_END();
}
