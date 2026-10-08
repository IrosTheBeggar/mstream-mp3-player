// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for CardWalk (docs/METADATA.md 2.10.1, 3.2.3; milestone N5) on
// fake FAT trees (test/support/FakeFat.h): the canonical order whatever the
// directory order, a 3,000-file folder through a small scratch, what the
// device sees, the digests, and T's freshness: a retag at the same size, a
// renamed folder, a deleted album, every stamp shifted an hour, three files
// shifted, invalid and zero stamps, 2.18's skew vectors and the recount past
// 256 distinct deltas. D is an in-memory store that takes the walk's output
// as N4's TagStore would (MemD below), so each test walks again and checks
// that an unchanged card writes nothing.
// Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../support/FakeFat.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"
#include "CardWalk.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace cw = cardwalk;
using fakefat::leafOf;
using fakefat::parentOf;
using St = cw::Status;

namespace {

constexpr uint32_t kT = 0x5D4773D5;  // 2026-10-07 14:30:42
uint32_t shifted(uint32_t t, int32_t s) { return cc::fatTimeFromWall(cc::fatWallSeconds(t) + s); }

bool isAudio(const std::string& rel) {
  const std::string l = leafOf(rel);
  return LibraryIndex::formatOf(l.data(), l.size()) != LibraryIndex::Format::Unknown;
}
bool isImage(const std::string& rel) {
  const std::string l = leafOf(rel);
  return LibraryIndex::imageRank(l.data(), l.size()) != LibraryIndex::kNoImage;
}

struct FolderLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return cc::compareFolderPaths(a.data(), a.size(), b.data(), b.size()) < 0;
  }
};
struct FileLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return cc::compareFilePaths(a.data(), a.size(), b.data(), b.size()) < 0;
  }
};

// One thing the walk told the sink.
struct Ev {
  char kind;  // 'F' folder row, 'f' folder gone, 'A' 'C' 'D' 'S' a file's row (Added, Changed, Doubtful, Settled), 'g' file gone, 'd' doubt
  std::string rel;
  cw::FileRow row;
  cw::FolderRow folder;
  std::string image;
  cw::Doubt doubt;
};

std::string describe(const Ev& e) {
  char buf[512];
  if (e.kind == 'F')
    std::snprintf(buf, sizeof(buf), "F %s %016llX %s %u/%u/%u %d", e.rel.c_str(),
                  static_cast<unsigned long long>(e.folder.digest), e.image.c_str(), e.folder.audio, e.folder.images,
                  e.folder.others, e.folder.imageOwned);
  else if (e.kind == 'd')
    std::snprintf(buf, sizeof(buf), "d %s %d %d %lld", e.rel.c_str(), e.doubt.settle, e.doubt.hasDelta,
                  static_cast<long long>(e.doubt.delta));
  else
    std::snprintf(buf, sizeof(buf), "%c %s %u %08X %d %d %016llX", e.kind, e.rel.c_str(), e.row.size, e.row.fatTime,
                  static_cast<int>(e.row.status), e.row.confirmed, static_cast<unsigned long long>(e.row.qfp));
  return buf;
}

struct FolderState {
  uint64_t digest = 0;
  bool owned = false;
  std::string image;
  uint32_t audio = 0, images = 0, others = 0;
};

// D in memory: Known over its folders and rows, Sink for the walk's output,
// applied when the walk finishes (as N4 compacts walk.jnl into tags.bin).
// It checks the order the contract asks of each channel.
class MemD : public cw::Known, public cw::Sink {
public:
  std::map<std::string, FolderState, FolderLess> folders;
  std::map<std::string, cw::FileRow, FileLess> files;
  int32_t skew = 0;
  // The last walk's.
  std::vector<Ev> events;
  std::vector<std::pair<std::string, cw::Doubt>> doubts;
  bool finished = false, aborted = false;
  cw::Summary summary;
  bool orderOk = true;
  std::string orderWhy;
  uint32_t rowsRead = 0, foldersRead = 0;
  bool failFolders = false;

  void beginWalk() {
    events.clear();
    doubts.clear();
    finished = aborted = false;
    orderOk = true;
    orderWhy.clear();
    rowsRead = foldersRead = 0;
    list_.clear();
    for (const auto& kv : folders) list_.push_back(kv.first);
    fi_ = 0;
    rows_.clear();
    ri_ = 0;
    lastFile_.clear();
    lastFolder_.clear();
    lastSettled_.clear();
    lastDoubt_.clear();
    haveFile_ = haveFolder_ = haveSettled_ = haveDoubt_ = false;
    settling_ = false;
    di_ = 0;
  }
  std::vector<Ev> of(char kind) const {
    std::vector<Ev> out;
    for (const Ev& e : events)
      if (e.kind == kind) out.push_back(e);
    return out;
  }
  size_t count(char kind) const { return of(kind).size(); }
  const cw::FileRow& row(const std::string& rel) const { return files.at(rel); }

  // ---- Known ----
  bool nextFolder(cw::KnownFolder* out) override {
    if (failFolders) {
      failed_ = true;
      return false;
    }
    if (fi_ >= list_.size()) return false;
    cur_ = list_[fi_++];
    ++foldersRead;
    const FolderState& s = folders.at(cur_);
    out->path = cur_.c_str();
    out->pathLength = cur_.size();
    out->digest = s.digest;
    out->imageOwned = s.owned;
    rows_.clear();
    ri_ = 0;
    // The folder's own files come together, first under it (canonical order).
    for (auto it = files.lower_bound(cur_.empty() ? std::string() : cur_ + "/"); it != files.end(); ++it) {
      if (parentOf(it->first) != cur_) break;
      rows_.push_back({leafOf(it->first), it->second});
    }
    return true;
  }
  bool nextFile(cw::KnownFile* out) override {
    if (ri_ >= rows_.size()) return false;
    const auto& r = rows_[ri_++];
    ++rowsRead;
    out->name = r.first.c_str();
    out->nameLength = r.first.size();
    out->row = r.second;
    return true;
  }
  bool failed() const override { return failed_; }

  // ---- Sink ----
  bool folder(const char* rel, size_t len, const cw::FolderRow& row) override {
    folderOrder(rel, len);
    Ev e{'F', std::string(rel, len), {}, row, std::string(row.image, row.imageLength), {}};
    events.push_back(e);
    return true;
  }
  bool folderGone(const char* rel, size_t len) override {
    folderOrder(rel, len);
    events.push_back(Ev{'f', std::string(rel, len), {}, {}, {}, {}});
    return true;
  }
  bool file(const char* rel, size_t len, cw::Change change, const cw::FileRow& row) override {
    const std::string p(rel, len);
    if (std::strlen(rel) != len) bad("a path not NUL-terminated at its length");
    char k = 'A';
    if (change == cw::Change::Changed) k = 'C';
    if (change == cw::Change::Doubtful) k = 'D';
    if (change == cw::Change::Settled) k = 'S';
    if (k == 'S') {
      settling_ = true;
      if (haveSettled_ && !FileLess()(lastSettled_, p)) bad("settled rows out of order: " + p);
      lastSettled_ = p;
      haveSettled_ = true;
    } else {
      fileOrder(p);
    }
    events.push_back(Ev{k, p, row, {}, {}, {}});
    return true;
  }
  bool fileGone(const char* rel, size_t len) override {
    fileOrder(std::string(rel, len));
    events.push_back(Ev{'g', std::string(rel, len), {}, {}, {}, {}});
    return true;
  }
  bool doubt(const char* rel, size_t len, const cw::Doubt& d) override {
    const std::string p(rel, len);
    if (settling_) bad("a doubt after the walk");
    if (haveDoubt_ && !FileLess()(lastDoubt_, p)) bad("doubts out of order: " + p);
    if (d.settle && (events.empty() || events.back().kind != 'D' || events.back().rel != p))
      bad("a settling doubt not right after its Doubtful row: " + p);
    lastDoubt_ = p;
    haveDoubt_ = true;
    doubts.push_back({p, d});
    Ev e{'d', p, {}, {}, {}, d};
    events.push_back(e);
    return true;
  }
  bool rewindDoubts() override {
    di_ = 0;
    return true;
  }
  Read nextDoubt(char* rel, size_t* len, cw::Doubt* d) override {
    if (di_ >= doubts.size()) return Read::End;
    const auto& x = doubts[di_++];
    std::memcpy(rel, x.first.data(), x.first.size());
    *len = x.first.size();
    *d = x.second;
    return Read::Item;
  }
  bool finish(const cw::Summary& s) override {
    summary = s;
    finished = true;
    for (const Ev& e : events) {
      switch (e.kind) {
        case 'F': {
          FolderState f;
          f.digest = e.folder.digest;
          f.owned = e.folder.imageOwned;
          f.image = e.image;
          f.audio = e.folder.audio;
          f.images = e.folder.images;
          f.others = e.folder.others;
          folders[e.rel] = f;
          break;
        }
        case 'f':
          folders.erase(e.rel);
          break;
        case 'g':
          files.erase(e.rel);
          break;
        case 'd':
          break;
        default:
          files[e.rel] = e.row;
      }
    }
    skew = s.skew;
    return true;
  }
  void abort() override { aborted = true; }

private:
  void bad(const std::string& why) {
    if (orderOk) orderWhy = why;
    orderOk = false;
  }
  void fileOrder(const std::string& p) {
    if (settling_) bad("a walk row after the settled ones: " + p);
    if (haveFile_ && !FileLess()(lastFile_, p)) bad("file rows out of order: " + lastFile_ + " then " + p);
    lastFile_ = p;
    haveFile_ = true;
  }
  void folderOrder(const char* rel, size_t len) {
    const std::string p(rel, len);
    if (haveFolder_ && !FolderLess()(lastFolder_, p)) bad("folder rows out of order: " + lastFolder_ + " then " + p);
    lastFolder_ = p;
    haveFolder_ = true;
  }
  std::vector<std::string> list_;
  size_t fi_ = 0;
  std::string cur_;
  std::vector<std::pair<std::string, cw::FileRow>> rows_;
  size_t ri_ = 0;
  bool failed_ = false;
  std::string lastFile_, lastFolder_, lastSettled_, lastDoubt_;
  bool haveFile_ = false, haveFolder_ = false, haveSettled_ = false, haveDoubt_ = false;
  bool settling_ = false;
  size_t di_ = 0;
};

// ---- T ----
class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > bytes.size()) bytes.resize(offset + n);
    if (n) std::memcpy(bytes.data() + offset, data, n);
    return true;
  }
  std::vector<uint8_t> bytes;
};

struct TRec {
  std::string rel;
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint64_t qfp = 0;
  uint16_t flags = 0;
};

std::vector<uint8_t> writeT(const std::vector<TRec>& rs) {
  std::vector<mptg::RecordIn> in(rs.size());
  for (size_t i = 0; i < rs.size(); ++i) {
    const bool audio = isAudio(rs[i].rel);
    in[i].path = rs[i].rel.c_str();
    in[i].rec.size = rs[i].size;
    in[i].rec.fatTime = rs[i].fatTime;
    in[i].rec.qfp = rs[i].qfp;
    in[i].rec.flags = rs[i].flags;
    in[i].rec.container = audio ? mptg::kContainerMp3 : mptg::kContainerNotAudio;
    in[i].rec.known = audio && !(rs[i].flags & mptg::kUnreadable) ? mptg::kKnownRules1 : 0;
    in[i].fields[cc::kTitle] = audio && !(rs[i].flags & mptg::kUnreadable) ? "T" : nullptr;
  }
  mptg::Meta meta;
  meta.source = mptg::kSourceTransfer;
  meta.generation = 3;
  meta.cardId = 0x1234;
  meta.producer = "test";
  VecSink out;
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mptg::write(out, meta, in.data(), in.size(), nullptr, 0, nullptr, 0, nullptr, &error),
                           error ? error : "write");
  return out.bytes;
}

// The software's listing of the card as it is now: a record for each audio
// file and image (the ones `keep` accepts).
template <typename Keep>
std::vector<TRec> transferOf(const fakefat::Card& card, Keep keep) {
  std::vector<TRec> rs;
  for (const auto& kv : card.files) {
    if (!isAudio(kv.first) && !isImage(kv.first)) continue;
    if (!keep(kv.first)) continue;
    rs.push_back(TRec{kv.first, kv.second.size, kv.second.fatTime, card.qfpOf(kv.first), 0});
  }
  return rs;
}
std::vector<TRec> transferOf(const fakefat::Card& card) {
  return transferOf(card, [](const std::string&) { return true; });
}

// ---- one walk ----
struct WalkOpts {
  const std::vector<uint8_t>* t = nullptr;
  bool first = false;
  bool indexed = false;
  uint32_t scratch = cw::CardWalk::kDeviceScratch;
  bool noKnown = false;
};

cw::CardWalk::Result walk(fakefat::Card& card, MemD& d, const WalkOpts& o = WalkOpts()) {
  d.beginWalk();
  std::vector<uint8_t> scratch(o.scratch);
  std::vector<uint8_t> tScratch(8192);
  cc::MemSource src(o.t ? o.t->data() : nullptr, o.t ? static_cast<uint32_t>(o.t->size()) : 0);
  auto streamed = std::make_unique<cw::StreamedTransfer>();
  auto indexed = std::make_unique<cw::IndexedTransfer>();
  cw::Transfer* tr = nullptr;
  if (o.t) {
    if (o.indexed) {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::Ok), static_cast<int>(indexed->begin(src)));
      tr = indexed.get();
    } else {
      streamed->begin(src, tScratch.data(), static_cast<uint32_t>(tScratch.size()));
      tr = streamed.get();
    }
  }
  cw::CardWalk::Config c;
  c.lister = &card;
  c.known = o.noKnown ? nullptr : &d;
  c.transfer = tr;
  c.sink = &d;
  c.firstAfterCommit = o.first;
  c.skew = d.skew;
  c.scratch = scratch.data();
  c.scratchBytes = o.scratch;
  auto w = std::make_unique<cw::CardWalk>();
  TEST_ASSERT_TRUE(w->begin(c));
  w->run();
  TEST_ASSERT_TRUE_MESSAGE(d.orderOk, d.orderWhy.c_str());
  TEST_ASSERT_TRUE(card.maxOpen <= 1);  // one folder open at a time
  card.maxOpen = 0;
  return w->result();
}

void assertDone(const cw::CardWalk::Result& r) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::None), static_cast<int>(r.error));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::State::Done), static_cast<int>(r.state));
}

// A second walk at the same commit finds nothing to do: nothing written,
// no D row read, no qfp read. (A folder D doesn't list, with no audio at or
// below it, is merged again each time: it has no rows and gives none.)
void assertQuiet(fakefat::Card& card, MemD& d, const std::vector<uint8_t>* t = nullptr) {
  WalkOpts o;
  o.t = t;
  o.indexed = t != nullptr;
  const auto r = walk(card, d, o);
  assertDone(r);
  std::string got;
  for (size_t i = 0; i < d.events.size() && i < 4; ++i) got += describe(d.events[i]) + " | ";
  if (!d.events.empty()) TEST_FAIL_MESSAGE(got.c_str());
  TEST_ASSERT_FALSE(r.summary.changed);
  TEST_ASSERT_EQUAL_UINT32(0, d.rowsRead);
  TEST_ASSERT_EQUAL_UINT32(0, r.qfpReads);
}

std::string eventsText(const MemD& d) {
  std::string s;
  for (const Ev& e : d.events) s += describe(e) + "\n";
  return s;
}

// An album: n tracks and a cover, under Artist/Album.
void album(fakefat::Card& card, const std::string& folder, int n, uint32_t time = kT, uint32_t seed = 100) {
  for (int i = 1; i <= n; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "/%02d - Track %d.mp3", i, i);
    card.addFile(folder + name, 300000 + static_cast<uint32_t>(i) * 1013, time, seed + static_cast<uint32_t>(i));
  }
  card.addFile(folder + "/cover.jpg", 40000, time, seed);
}

LibraryBuilder::Choice choose(const TRec& t, const cw::FileRow& row, int32_t skew) {
  LibraryBuilder::Seen ts, ds;
  ts.present = ds.present = true;
  ts.size = t.size;
  ts.fatTime = t.fatTime;
  ts.flags = t.flags;
  ds.size = row.size;
  ds.fatTime = row.fatTime;
  LibraryBuilder::Row r;
  r.status = row.status;
  r.confirmed = row.confirmed;
  return LibraryBuilder::choose(ts, ds, r, true, skew, false);
}

// Every row of D against T through the builder's rule 1: T is chosen for
// exactly the rows the walk made Software.
void assertBuilderAgrees(const MemD& d, const std::vector<TRec>& t) {
  std::map<std::string, TRec> byPath;
  for (const TRec& r : t) byPath[r.rel] = r;
  for (const auto& kv : d.files) {
    auto it = byPath.find(kv.first);
    if (it == byPath.end()) {
      TEST_ASSERT_TRUE_MESSAGE(kv.second.status != St::Software, kv.first.c_str());
      continue;
    }
    const bool tPicked = choose(it->second, kv.second, d.skew).pick == LibraryBuilder::Pick::Transfer;
    TEST_ASSERT_EQUAL_MESSAGE(kv.second.status == St::Software, tPicked, kv.first.c_str());
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

// ---- the helpers ----

void test_digest_and_qfp_helpers() {
  // Pinned (computed apart from this code: FNV-1a 64 over the bytes 3.2.3
  // names).
  cw::FolderDigest empty;
  TEST_ASSERT_EQUAL_HEX64(0x4D25767F9DCE13F5ull, empty.value());
  cw::FolderDigest d;
  d.add("01 - A.mp3", 10, 1000, kT);
  d.add("cover.jpg", 9, 500, kT);
  d.other();
  d.other();
  TEST_ASSERT_EQUAL_HEX64(0x73DE00588F27D114ull, d.value());

  // 2.18's qfp vectors through a file read in pieces of any size.
  struct V {
    uint32_t size;
    uint64_t qfp;
  } vs[] = {{0, 0xA8C7F832281A39C5ull},    {100, 0xB708DC48BA0A842Dull},  {4096, 0x636A94FE9C19DC15ull},
            {4097, 0xA76BF84EC13A75BCull}, {5000, 0xC8E651224ADA889Cull}, {8192, 0xA9383C4532F6F525ull},
            {8193, 0xD70E2B23544A7444ull}, {10000, 0xF17B194EF7F5F338ull}};
  uint8_t buf[4096];
  for (const V& v : vs) {
    for (uint32_t piece : {4096u, 1000u, 7u, 1u}) {
      fakefat::FileSource src(v.size, 0);
      uint64_t q = 0;
      TEST_ASSERT_TRUE(cw::fileQfp(src, v.size, buf, piece, &q));
      TEST_ASSERT_EQUAL_HEX64(v.qfp, q);
      if (piece == 4096u) TEST_ASSERT_TRUE(src.reads <= 2);  // two reads at most
    }
  }
  fakefat::FileSource changed(5000, 0);
  uint64_t q;
  TEST_ASSERT_FALSE(cw::fileQfp(changed, 5001, buf, sizeof(buf), &q));  // the file isn't the size listed

  // The facts LibraryIndex takes count images among its other files.
  cw::FolderRow row;
  row.image = "cover.jpg";
  row.imageLength = 9;
  row.images = 2;
  row.others = 3;
  row.imageOwned = true;
  const LibraryIndex::FolderFacts f = cw::factsOf(row);
  TEST_ASSERT_EQUAL_STRING("cover.jpg", f.image);
  TEST_ASSERT_EQUAL_UINT8(2, f.imageCount);
  TEST_ASSERT_EQUAL_UINT16(5, f.otherCount);
  TEST_ASSERT_TRUE(f.imageOwned);
  TEST_ASSERT_NULL(cw::factsOf(cw::FolderRow()).image);

  // The smallest scratch.
  fakefat::Card card;
  MemD dd;
  std::vector<uint8_t> small(cw::CardWalk::kMinScratch - 1);
  cw::CardWalk w;
  cw::CardWalk::Config c;
  c.lister = &card;
  c.sink = &dd;
  c.scratch = small.data();
  c.scratchBytes = static_cast<uint32_t>(small.size());
  TEST_ASSERT_FALSE(w.begin(c));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::Config), static_cast<int>(w.result().error));
}

// ---- the order, the passes, what the device sees ----

void test_first_walk_lists_in_canonical_order() {
  fakefat::Card card;
  // 2.18's sibling names, made in a scrambled order, and files beside folders.
  for (const char* n : {"É", "a", "B", "A-", "A B", "A"}) card.addFile(std::string(n) + "/x.mp3", 1000, kT);
  card.addFile("A/z.mp3", 1001, kT);
  card.addFile("A/B/y.mp3", 1002, kT);
  card.addFile("A/0.flac", 1003, kT);
  card.addFile("top.opus", 1004, kT);
  card.addFile("Ab.mp3", 1005, kT);
  MemD d;
  const auto r = walk(card, d);
  assertDone(r);
  const char* want[] = {"Ab.mp3", "top.opus", "A/0.flac", "A/x.mp3", "A/z.mp3", "A/B/y.mp3",
                        "A B/x.mp3", "A-/x.mp3", "B/x.mp3", "a/x.mp3", "É/x.mp3"};
  const auto added = d.of('A');
  TEST_ASSERT_EQUAL_size_t(11, added.size());
  for (size_t i = 0; i < added.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(want[i], added[i].rel.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(added[i].row.status));
  }
  // A row for each folder with audio at or below it, /music first.
  const char* folders[] = {"", "A", "A/B", "A B", "A-", "B", "a", "É"};
  const auto rows = d.of('F');
  TEST_ASSERT_EQUAL_size_t(8, rows.size());
  for (size_t i = 0; i < rows.size(); ++i) TEST_ASSERT_EQUAL_STRING(folders[i], rows[i].rel.c_str());
  TEST_ASSERT_EQUAL_UINT32(3, rows[1].folder.audio);
  TEST_ASSERT_EQUAL_UINT32(11, r.audio);
  TEST_ASSERT_TRUE(r.summary.changed);
  TEST_ASSERT_TRUE(d.finished);
  assertQuiet(card, d);

  // Another directory order: still nothing to do (the digests are over the
  // sorted entries).
  card.shuffle(7);
  assertQuiet(card, d);
}

// A tree of made-up names in several directory orders and scratch sizes: the
// same walk, to the byte.
void test_shuffled_order_gives_the_same_walk() {
  fakefat::Card card;
  uint32_t x = 12345;
  auto rnd = [&] {
    x = x * 1103515245u + 12345u;
    return x >> 8;
  };
  const char* parts[] = {"A", "a", "B", "Z z", "é", "Ünïcode", "01", "1", "x-y", "The Band", "Mix", "~"};
  for (int i = 0; i < 400; ++i) {
    std::string path;
    const int depth = 1 + static_cast<int>(rnd() % 4);
    for (int k = 0; k < depth; ++k) path += std::string(parts[rnd() % 12]) + std::to_string(rnd() % 3) + "/";
    const char* exts[] = {".mp3", ".flac", ".opus", ".jpg", ".txt", ".MP3"};
    path += "f" + std::to_string(rnd() % 50) + exts[rnd() % 6];
    card.addFile(path, 1000 + rnd() % 100000, kT + (rnd() % 7) * 2, rnd());
  }
  MemD ref;
  const auto r0 = walk(card, ref);
  assertDone(r0);
  const std::string want = eventsText(ref);
  TEST_ASSERT_TRUE(ref.count('A') > 100);
  for (uint32_t shuffle : {1u, 2u, 99u}) {
    for (uint32_t scratch : {cw::CardWalk::kMinScratch, 3000u, 5000u, 20000u, 1u << 20}) {
      card.shuffle(shuffle);
      MemD d;
      const auto r = walk(card, d, WalkOpts{nullptr, false, false, scratch});
      assertDone(r);
      TEST_ASSERT_EQUAL_STRING(want.c_str(), eventsText(d).c_str());
      TEST_ASSERT_EQUAL_UINT32(r0.audio, r.audio);
      TEST_ASSERT_EQUAL_UINT32(r0.images, r.images);
      TEST_ASSERT_EQUAL_UINT32(r0.others, r.others);
    }
    assertQuiet(card, ref);
  }
}

// A 3,000-file folder and a 400-folder folder through a scratch that holds
// a fraction of either: listed in passes, never out of order.
void test_big_folders_through_a_small_scratch() {
  fakefat::Card card;
  uint32_t x = 777;
  auto rnd = [&] {
    x = x * 1664525u + 1013904223u;
    return x >> 8;
  };
  // Made in a random order, so each pass must pick the smallest names.
  std::vector<int> order(3000);
  for (int i = 0; i < 3000; ++i) order[static_cast<size_t>(i)] = i;
  for (int i = 2999; i > 0; --i) std::swap(order[static_cast<size_t>(i)], order[rnd() % static_cast<uint32_t>(i + 1)]);
  for (int i : order) {
    char name[96];
    std::snprintf(name, sizeof(name), "Singles/%04d %.*s.mp3", i, 1 + i % 40, "a song with a rather long name, really");
    card.addFile(name, 5000 + static_cast<uint32_t>(i), kT, static_cast<uint32_t>(i) + 1);
  }
  card.addFile("Singles/cover.jpg", 777, kT);
  card.addFile("Singles/notes.txt", 10, kT);
  for (int i = 399; i >= 0; --i) {
    char name[64];
    std::snprintf(name, sizeof(name), "Many/%03d/%d.flac", i, i);
    card.addFile(name, 9000 + static_cast<uint32_t>(i), kT);
  }
  MemD ref;
  const auto big = walk(card, ref, WalkOpts{nullptr, false, false, 1u << 20});
  assertDone(big);
  TEST_ASSERT_EQUAL_UINT32(3400, big.audio);
  TEST_ASSERT_EQUAL_UINT32(big.folders, big.listings);  // each folder fit: one listing each
  const std::string want = eventsText(ref);
  for (uint32_t scratch : {cw::CardWalk::kMinScratch, 4096u, 8192u}) {
    MemD d;
    card.opened.clear();
    const auto r = walk(card, d, WalkOpts{nullptr, false, false, scratch});
    assertDone(r);
    TEST_ASSERT_EQUAL_STRING(want.c_str(), eventsText(d).c_str());
    TEST_ASSERT_TRUE(r.listings > r.folders + 10);  // passes
    // A small scratch at 20k-scale stays fixed: the walk asked for nothing more.
    if (scratch == 8192u) {
      // Singles: its digest in passes, then again to merge (D didn't list it).
      uint32_t singles = 0;
      for (const auto& o : card.opened) singles += o == "Singles";
      TEST_ASSERT_TRUE(singles >= 4);
      // Then nothing changed: digests in passes, no D row read.
      card.shuffle(3);
      WalkOpts o;
      o.scratch = scratch;
      const auto q = walk(card, d, o);
      assertDone(q);
      TEST_ASSERT_EQUAL_size_t(0, d.events.size());
      TEST_ASSERT_EQUAL_UINT32(0, d.rowsRead);
      // One track changed size: one row, found through the passes.
      char name[96];
      std::snprintf(name, sizeof(name), "Singles/%04d %.*s.mp3", 1234, 1 + 1234 % 40,
                    "a song with a rather long name, really");
      TEST_ASSERT_EQUAL_size_t(1, card.files.count(name));
      card.addFile(name, 1, kT + 2, 9);
      const auto c = walk(card, d, o);
      assertDone(c);
      TEST_ASSERT_EQUAL_size_t(2, d.events.size());  // its row, and the folder's new digest
      TEST_ASSERT_EQUAL_size_t(1, d.count('C'));
      TEST_ASSERT_EQUAL_STRING(name, d.of('C')[0].rel.c_str());
      TEST_ASSERT_EQUAL_size_t(1, d.count('F'));
      TEST_ASSERT_EQUAL_UINT32(3000, d.rowsRead);
      card.shuffle(0);
    }
  }

  // The first walk after a commit merges every folder anyway: a big one is
  // digested and merged in the same passes, T asked in order. A hand-added
  // "a scan.jpg" is the best cover until cover.jpg (T's) comes, passes later.
  card.addFile("Singles/a scan.jpg", 5555, kT);
  auto recs = transferOf(card, [](const std::string& p) { return p != "Singles/a scan.jpg"; });
  const auto t = writeT(recs);
  int moved = 0;
  for (auto& kv : card.files)
    if (isAudio(kv.first) && kv.first.compare(0, 8, "Singles/") == 0 && (kv.second.size % 997) == 0 && moved < 3) {
      kv.second.fatTime = shifted(kT, 3600);  // three shifted: no skew, three reads
      ++moved;
    }
  TEST_ASSERT_EQUAL_INT(3, moved);
  MemD known;
  assertDone(walk(card, known, WalkOpts{nullptr, false, false, 1u << 20}));
  std::string wantT;
  for (uint32_t scratch : {1u << 20, cw::CardWalk::kMinScratch, 8192u, cw::CardWalk::kDeviceScratch}) {
    for (bool dKnows : {true, false}) {
      MemD d;
      if (dKnows) {
        d.folders = known.folders;
        d.files = known.files;
      }
      card.opened.clear();
      const auto r = walk(card, d, WalkOpts{&t, true, false, scratch});
      assertDone(r);
      TEST_ASSERT_EQUAL_UINT32(3, r.qfpReads);
      TEST_ASSERT_EQUAL_UINT32(3, r.byQfp);
      TEST_ASSERT_TRUE(d.folders.at("Singles").owned);
      TEST_ASSERT_EQUAL_STRING("cover.jpg", d.folders.at("Singles").image.c_str());
      for (const auto& kv : d.files) TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(kv.second.status));
      assertBuilderAgrees(d, recs);
      if (dKnows) {
        // The rows that changed are the same, whatever the scratch.
        const std::string got = eventsText(d);
        if (wantT.empty()) wantT = got;
        TEST_ASSERT_EQUAL_STRING(wantT.c_str(), got.c_str());
      }
      uint32_t singles = 0;
      for (const auto& o : card.opened) singles += o == "Singles";
      if (scratch == cw::CardWalk::kDeviceScratch) TEST_ASSERT_EQUAL_UINT32(3, singles);  // 3.2.3's three passes
    }
  }
}

// Names up to the path limit, eight levels deep, many siblings at each level:
// through the least scratch each level still lists (the levels above keep
// their subfolders while it does), and the walk is the same as with plenty.
void test_long_names_and_deep_folders_through_the_least_scratch() {
  fakefat::Card card;
  uint32_t x = 4242;
  auto rnd = [&] {
    x = x * 1103515245u + 12345u;
    return x >> 8;
  };
  // A name of about `len` bytes, ASCII and two-byte é, never cut inside one.
  auto name = [&](size_t len) {
    std::string s(1, static_cast<char>('A' + rnd() % 26));
    while (s.size() + 2 <= len) s += (rnd() % 5 == 0) ? "\xC3\xA9" : std::string(1, static_cast<char>('a' + rnd() % 26));
    return s;
  };
  std::vector<std::string> level = {""};
  for (int depth = 1; depth <= 8; ++depth) {
    std::vector<std::string> next;
    for (const std::string& parent : level) {
      const size_t used = parent.empty() ? 0 : parent.size() + 1;
      const size_t room = cc::kMaxRelPath - used;
      if (room < 12) continue;
      const int siblings = depth <= 2 ? 6 : 2;
      for (int k = 0; k < siblings; ++k) {
        const size_t want = 4 + rnd() % (depth == 8 ? room - 8 : std::min<size_t>(room / 3, 60));
        const std::string folder = (parent.empty() ? "" : parent + "/") + name(want);
        const size_t fileRoom = cc::kMaxRelPath - folder.size() - 1;
        if (fileRoom < 6) continue;
        card.addFile(folder + "/" + name(std::min<size_t>(fileRoom - 4, 2 + rnd() % 200)) + ".mp3", 1000 + rnd() % 9000,
                     kT);
        next.push_back(folder);
      }
    }
    level = std::move(next);
  }
  TEST_ASSERT_TRUE(card.files.size() > 300);
  size_t longest = 0;
  for (const auto& kv : card.files) longest = std::max(longest, kv.first.size());
  TEST_ASSERT_TRUE(longest > 200);
  MemD ref;
  const auto r0 = walk(card, ref, WalkOpts{nullptr, false, false, 1u << 20});
  assertDone(r0);
  TEST_ASSERT_EQUAL_UINT32(card.files.size(), r0.audio);
  const std::string want = eventsText(ref);
  for (uint32_t scratch : {cw::CardWalk::kMinScratch, cw::CardWalk::kMinScratch + 300, 4096u}) {
    for (uint32_t shuffle : {0u, 5u}) {
      card.shuffle(shuffle);
      MemD d;
      const auto r = walk(card, d, WalkOpts{nullptr, false, false, scratch});
      assertDone(r);
      TEST_ASSERT_EQUAL_STRING(want.c_str(), eventsText(d).c_str());
    }
  }
}

void test_what_the_device_sees() {
  fakefat::Card card;
  card.addFile("Artist/Album/01.mp3", 1000, kT);
  card.addFile("Artist/Album/02.MP3", 1000, kT);  // any case
  card.addFile("Artist/Album/.03.mp3", 1000, kT);  // hidden
  card.addFile("Artist/.git/x.mp3", 1000, kT);     // a hidden folder
  card.addFile("Artist/Album/04.ogg", 1000, kT);   // not listed: an Ogg of any codec (2.8.2)
  card.addFile("Artist/Album/front.JPEG", 1000, kT);
  // Eight folder levels below /music are walked, the ninth isn't.
  card.addFile("1/2/3/4/5/6/7/8/deep.mp3", 1000, kT);
  card.addFile("1/2/3/4/5/6/7/8/9/deeper.mp3", 1000, kT);
  // "/music/" and the path: 255 bytes are seen, 256 aren't.
  const std::string ok = "L/" + std::string(242, 'k') + ".mp3";    // 7 + 248 = 255
  const std::string over = "L/" + std::string(243, 'o') + ".mp3";  // 256
  card.addFile(ok, 1000, kT);
  card.addFile(over, 1000, kT);
  MemD d;
  const auto r = walk(card, d);
  assertDone(r);
  std::vector<std::string> added;
  for (const Ev& e : d.of('A')) added.push_back(e.rel);
  const std::vector<std::string> want = {"1/2/3/4/5/6/7/8/deep.mp3", "Artist/Album/01.mp3", "Artist/Album/02.MP3", ok};
  TEST_ASSERT_EQUAL_size_t(want.size(), added.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), added[i].c_str());
  TEST_ASSERT_EQUAL_UINT32(1, r.images);
  TEST_ASSERT_EQUAL_UINT32(1, r.others);  // the .ogg
  for (const auto& o : card.opened) TEST_ASSERT_TRUE(o.find(".git") == std::string::npos && o.find("/9") == std::string::npos);
  // The album's row: front.JPEG its cover, the .ogg its other file.
  for (const Ev& e : d.of('F')) {
    if (e.rel != "Artist/Album") continue;
    TEST_ASSERT_EQUAL_STRING("front.JPEG", e.image.c_str());
    TEST_ASSERT_EQUAL_UINT32(2, e.folder.audio);
    TEST_ASSERT_EQUAL_UINT32(1, e.folder.others);
  }

  // No /music at all: D's folders are gone, and the walk is fine.
  card.noMusic = true;
  const auto gone = walk(card, d);
  assertDone(gone);
  TEST_ASSERT_EQUAL_size_t(4, d.count('g'));
  TEST_ASSERT_TRUE(d.files.empty());
  TEST_ASSERT_EQUAL_size_t(1, d.folders.size());  // /music itself, empty
  TEST_ASSERT_EQUAL_size_t(1, d.folders.count(""));
}

// ---- covers and folder rows ----

void test_folder_rows_and_covers() {
  fakefat::Card card;
  album(card, "Artist/Album", 3);
  card.addFile("Artist/Album/folder.jpg", 5000, kT);  // cover.jpg ranks first
  card.addFile("Artist/Album/booklet.pdf", 5000, kT);
  card.addFile("Artist/Album/scan 2.jpg", 5000, kT);
  // Images and no audio anywhere below: no row (the index leaves it out).
  card.addFile("Artist/Album/Scans/a.jpg", 100, kT);
  card.addFile("Artist/Album/Scans/b.jpg", 100, kT);
  // An artist with album subfolders only: its row comes, before theirs.
  card.addFile("New/One/CD1/01.flac", 2000, kT);
  // The software wrote cover.jpg; the listener added Hand's front.jpg.
  album(card, "Other/Hand", 2);
  card.remove("Other/Hand/cover.jpg");
  card.addFile("Other/Hand/front.jpg", 3000, kT);
  const auto t = writeT(transferOf(card, [](const std::string& p) { return p.find("Hand/front") == std::string::npos; }));
  MemD d;
  WalkOpts o;
  o.t = &t;
  o.first = true;
  const auto r = walk(card, d, o);
  assertDone(r);
  std::map<std::string, Ev> rows;
  std::vector<std::string> order;
  for (const Ev& e : d.of('F')) {
    rows[e.rel] = e;
    order.push_back(e.rel);
  }
  TEST_ASSERT_EQUAL_size_t(0, rows.count("Artist/Album/Scans"));
  const std::vector<std::string> want = {"",        "Artist",      "Artist/Album", "New",
                                         "New/One", "New/One/CD1", "Other",        "Other/Hand"};
  TEST_ASSERT_EQUAL_size_t(want.size(), order.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), order[i].c_str());
  const Ev& a = rows["Artist/Album"];
  TEST_ASSERT_EQUAL_STRING("cover.jpg", a.image.c_str());
  TEST_ASSERT_EQUAL_UINT8(0, a.folder.imageRank);
  TEST_ASSERT_EQUAL_UINT32(40000, a.folder.imageSize);
  TEST_ASSERT_TRUE(a.folder.imageOwned);  // T lists it, at its size
  TEST_ASSERT_EQUAL_UINT32(3, a.folder.audio);
  TEST_ASSERT_EQUAL_UINT32(3, a.folder.images);
  TEST_ASSERT_EQUAL_UINT32(1, a.folder.others);
  const Ev& h = rows["Other/Hand"];
  TEST_ASSERT_EQUAL_STRING("front.jpg", h.image.c_str());
  TEST_ASSERT_FALSE(h.folder.imageOwned);  // a hand-added cover (2.14.3)
  TEST_ASSERT_EQUAL_STRING("", rows["New/One"].image.c_str());
  assertQuiet(card, d, &t);

  // The listener swaps the software's cover for another: no longer owned.
  card.addFile("Artist/Album/cover.jpg", 41234, kT + 2);
  const auto r2 = walk(card, d, WalkOpts{&t});
  assertDone(r2);
  TEST_ASSERT_EQUAL_size_t(1, d.events.size());
  TEST_ASSERT_EQUAL_STRING("Artist/Album", d.events[0].rel.c_str());
  TEST_ASSERT_FALSE(d.events[0].folder.imageOwned);
  TEST_ASSERT_TRUE(d.rowsRead > 0);  // its rows were read for the merge
  // The audio is unchanged, so not one track went to T or the sink.
  TEST_ASSERT_EQUAL_size_t(0, d.count('C') + d.count('D'));
}

// ---- T's freshness ----

void test_transfer_rows_and_the_builder() {
  fakefat::Card card;
  album(card, "Artist/Album", 5);
  card.addFile("Hand/Made/01.mp3", 7000, kT);  // the listener's own
  auto recs = transferOf(card, [](const std::string& p) { return p.find("Hand") == std::string::npos; });
  // T knows Track 5 at another size (the listener replaced it).
  for (TRec& tr : recs)
    if (tr.rel == "Artist/Album/05 - Track 5.mp3") tr.size += 1;
  const auto t = writeT(recs);
  MemD d;
  WalkOpts o;
  o.t = &t;
  o.first = true;
  const auto r = walk(card, d, o);
  assertDone(r);
  for (int i = 1; i <= 4; ++i) {
    char p[64];
    std::snprintf(p, sizeof(p), "Artist/Album/%02d - Track %d.mp3", i, i);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(p).status));
  }
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(d.row("Artist/Album/05 - Track 5.mp3").status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(d.row("Hand/Made/01.mp3").status));
  TEST_ASSERT_EQUAL_UINT32(0, r.doubtful);
  TEST_ASSERT_EQUAL_INT32(0, r.summary.skew);
  TEST_ASSERT_TRUE(r.summary.firstAfterCommit);
  assertBuilderAgrees(d, recs);
  assertQuiet(card, d, &t);

  // The same at the same commit through T's index.
  WalkOpts oi;
  oi.t = &t;
  oi.indexed = true;
  const auto ri = walk(card, d, oi);
  assertDone(ri);
  TEST_ASSERT_EQUAL_size_t(0, d.events.size());

  // /.mstream went away: a first walk with no T turns the Software rows
  // Pending; the device's own rows stay.
  d.files["Hand/Made/01.mp3"].status = St::Scanned;
  WalkOpts gone;
  gone.first = true;
  const auto rg = walk(card, d, gone);
  assertDone(rg);
  TEST_ASSERT_EQUAL_size_t(4, d.count('C'));
  for (const auto& kv : d.files)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(kv.first == "Hand/Made/01.mp3" ? St::Scanned : St::Pending),
                          static_cast<int>(kv.second.status));
}

// A file the software couldn't read names nothing: the scan reads it.
void test_unreadable_transfer_record() {
  fakefat::Card card;
  card.addFile("A/B/bad.mp3", 1000, kT, 5);
  card.addFile("A/B/api.mp3", 2000, kT, 6);
  auto recs = transferOf(card);
  recs[0].flags = mptg::kUnreadable | mptg::kFromApi;  // api.mp3: filled from mStream's API
  recs[1].flags = mptg::kUnreadable;
  const auto t = writeT(recs);
  MemD d;
  const auto r = walk(card, d, WalkOpts{&t, true});
  assertDone(r);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(d.row("A/B/bad.mp3").status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row("A/B/api.mp3").status));
  assertBuilderAgrees(d, recs);
}

// A retag that kept the size: caught by the time and the fingerprint. One
// that kept the time too isn't (2.3.5's limit; Rescan tags, U8).
void test_retag_at_the_same_size() {
  fakefat::Card card;
  album(card, "Artist/Album", 6);
  const auto recs = transferOf(card);
  const auto t = writeT(recs);
  MemD d;
  assertDone(walk(card, d, WalkOpts{&t, true}));
  const std::string p = "Artist/Album/02 - Track 2.mp3";
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(p).status));

  // Retagged on a PC: new bytes in its head, the same size, a new time.
  card.at(p).seed = 4242;
  card.at(p).fatTime = shifted(kT, 86400 * 3 + 64);
  const auto r = walk(card, d, WalkOpts{&t});
  assertDone(r);
  TEST_ASSERT_EQUAL_UINT32(1, r.foldersMerged);
  TEST_ASSERT_EQUAL_UINT32(1, r.doubtful);
  TEST_ASSERT_EQUAL_UINT32(1, r.qfpReads);
  TEST_ASSERT_EQUAL_UINT32(1, r.notTransfer);
  const cw::FileRow& row = d.row(p);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(row.status));
  TEST_ASSERT_FALSE(row.confirmed);
  TEST_ASSERT_EQUAL_HEX64(card.qfpOf(p), row.qfp);  // the read is kept
  TEST_ASSERT_EQUAL_size_t(1, d.count('D'));
  TEST_ASSERT_EQUAL_size_t(1, d.count('S'));
  TEST_ASSERT_EQUAL_size_t(1, d.count('F'));  // the folder's digest
  assertBuilderAgrees(d, recs);
  assertQuiet(card, d, &t);

  // A retag that kept the size and the time: the walk can't see it.
  const std::string q = "Artist/Album/03 - Track 3.mp3";
  card.at(q).seed = 999;
  assertQuiet(card, d, &t);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(q).status));
}

void test_renamed_folder() {
  fakefat::Card card;
  album(card, "Artist/Album", 4);
  album(card, "Artist/Second", 2, kT, 300);
  const auto recs = transferOf(card);
  const auto t = writeT(recs);
  MemD d;
  assertDone(walk(card, d, WalkOpts{&t, true}));
  card.renameFolder("Artist/Album", "Artist/Album (Deluxe)");
  const auto r = walk(card, d, WalkOpts{&t});
  assertDone(r);
  TEST_ASSERT_EQUAL_size_t(1, d.count('f'));
  TEST_ASSERT_EQUAL_STRING("Artist/Album", d.of('f')[0].rel.c_str());
  TEST_ASSERT_EQUAL_size_t(4, d.count('g'));
  TEST_ASSERT_EQUAL_size_t(4, d.count('A'));
  for (const Ev& e : d.of('A')) {
    TEST_ASSERT_EQUAL_STRING("Artist/Album (Deluxe)", parentOf(e.rel).c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(e.row.status));  // T names the old paths
  }
  TEST_ASSERT_EQUAL_size_t(1, d.count('F'));
  TEST_ASSERT_EQUAL_STRING("Artist/Album (Deluxe)", d.of('F')[0].rel.c_str());
  TEST_ASSERT_EQUAL_size_t(0, d.count('C'));
  TEST_ASSERT_EQUAL_UINT32(0, r.qfpReads);
  // The pre-order: the new name ("Album (" is 41 20) sorts before the old's
  // successor, "Second": its rows came between the gone ones and nothing else.
  assertQuiet(card, d, &t);
}

void test_deleted_album() {
  fakefat::Card card;
  album(card, "Artist/Album", 4);
  album(card, "Artist/Gone", 3, kT, 500);
  card.addFile("Artist/Gone/Bonus/01.mp3", 1000, kT);
  album(card, "Zed/Last", 1, kT, 700);
  const auto t = writeT(transferOf(card));
  MemD d;
  assertDone(walk(card, d, WalkOpts{&t, true}));
  const size_t before = d.files.size();
  card.remove("Artist/Gone");
  const auto r = walk(card, d, WalkOpts{&t});
  assertDone(r);
  TEST_ASSERT_EQUAL_size_t(2, d.count('f'));  // the album and its subfolder
  TEST_ASSERT_EQUAL_size_t(4, d.count('g'));
  TEST_ASSERT_EQUAL_size_t(6, d.events.size());  // nothing else: Artist's own digest didn't change
  TEST_ASSERT_EQUAL_size_t(before - 4, d.files.size());
  TEST_ASSERT_EQUAL_UINT32(0, r.foldersMerged);
  assertQuiet(card, d, &t);

  // The last folders of the card gone: found at the walk's end.
  card.remove("Zed");
  const auto z = walk(card, d, WalkOpts{&t});
  assertDone(z);
  TEST_ASSERT_EQUAL_size_t(2, d.count('f'));
  TEST_ASSERT_EQUAL_size_t(1, d.count('g'));
}

// A PC that shifted every stamp an hour (2.17 item 5): the skew, no reads.
void test_every_stamp_shifted_an_hour() {
  fakefat::Card card;
  album(card, "Artist/Album", 12);
  album(card, "Artist/Other", 8, kT, 900);
  const auto recs = transferOf(card);
  const auto t = writeT(recs);
  for (auto& kv : card.files) kv.second.fatTime = shifted(kv.second.fatTime, 3600);
  MemD d;
  const auto r = walk(card, d, WalkOpts{&t, true});
  assertDone(r);
  TEST_ASSERT_EQUAL_INT32(3600, r.summary.skew);
  TEST_ASSERT_EQUAL_UINT32(20, r.doubtful);
  TEST_ASSERT_EQUAL_UINT32(20, r.bySkew);
  TEST_ASSERT_EQUAL_UINT32(2, r.countOnly);  // the covers' pairs count too
  TEST_ASSERT_EQUAL_UINT32(0, r.qfpReads);
  TEST_ASSERT_FALSE(r.recounted);
  for (const auto& kv : d.files) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(kv.second.status));
    TEST_ASSERT_FALSE(kv.second.confirmed);
  }
  TEST_ASSERT_EQUAL_INT32(3600, d.skew);
  for (const auto& kv : d.folders)
    if (!kv.second.image.empty()) TEST_ASSERT_TRUE(kv.second.owned);
  assertBuilderAgrees(d, recs);
  // The next boot at the same commit: the skew from D's header, nothing to do.
  assertQuiet(card, d, &t);
  // A file touched again later (its bytes unchanged): at the same commit the
  // skew doesn't explain it, so its qfp is read, once.
  const std::string p = "Artist/Album/03 - Track 3.mp3";
  card.at(p).fatTime = shifted(kT, 7200);
  const auto r2 = walk(card, d, WalkOpts{&t});
  assertDone(r2);
  TEST_ASSERT_EQUAL_UINT32(1, r2.doubtful);
  TEST_ASSERT_EQUAL_UINT32(1, r2.qfpReads);
  TEST_ASSERT_EQUAL_UINT32(1, r2.byQfp);
  TEST_ASSERT_TRUE(d.row(p).confirmed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(p).status));
  assertBuilderAgrees(d, recs);
  assertQuiet(card, d, &t);
}

// Three files shifted (2.17 item 5): no skew; qfp decides, once.
void test_three_files_shifted() {
  fakefat::Card card;
  album(card, "Artist/Album", 30);
  auto recs = transferOf(card);
  const auto t = writeT(recs);
  const char* moved[] = {"Artist/Album/04 - Track 4.mp3", "Artist/Album/17 - Track 17.mp3",
                         "Artist/Album/29 - Track 29.mp3"};
  for (const char* m : moved) card.at(m).fatTime = shifted(kT, 3600);
  card.at(moved[2]).seed = 31337;  // and retagged
  MemD d;
  const auto r = walk(card, d, WalkOpts{&t, true});
  assertDone(r);
  TEST_ASSERT_EQUAL_INT32(0, r.summary.skew);
  TEST_ASSERT_EQUAL_UINT32(3, r.doubtful);
  TEST_ASSERT_EQUAL_UINT32(3, r.qfpReads);
  TEST_ASSERT_EQUAL_UINT32(2, r.byQfp);
  TEST_ASSERT_EQUAL_UINT32(1, r.notTransfer);
  TEST_ASSERT_EQUAL_UINT32(0, r.qfpFailed);
  TEST_ASSERT_FALSE(r.summary.unsettled);  // every doubt settled (a failed read: test_tag_store_walk)
  TEST_ASSERT_TRUE(d.row(moved[0]).confirmed);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(moved[1]).status));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Pending), static_cast<int>(d.row(moved[2]).status));
  assertBuilderAgrees(d, recs);
  // Paid once: the next boot reads nothing.
  assertQuiet(card, d, &t);
  // A new commit whose T still has the old stamps: the device's saved
  // fingerprints settle all three without a read.
  const auto r2 = walk(card, d, WalkOpts{&t, true});
  assertDone(r2);
  TEST_ASSERT_EQUAL_UINT32(3, r2.doubtful);
  TEST_ASSERT_EQUAL_UINT32(0, r2.qfpReads);
  TEST_ASSERT_EQUAL_UINT32(2, r2.byDeviceQfp);
  TEST_ASSERT_EQUAL_UINT32(1, r2.notTransfer);
  TEST_ASSERT_TRUE(d.row(moved[1]).confirmed);
  assertBuilderAgrees(d, recs);
}

// A recorded or observed 0, or an invalid stamp, never matches by time: it is
// left out of the skew's pairs, and qfp decides.
void test_invalid_and_zero_stamps() {
  fakefat::Card card;
  album(card, "Artist/Album", 20);
  auto recs = transferOf(card);
  const std::string zeroT = "Artist/Album/01 - Track 1.mp3";    // T recorded 0
  const std::string badCard = "Artist/Album/02 - Track 2.mp3";  // the card's stamp has month 0
  const std::string zeroBoth = "Artist/Album/03 - Track 3.mp3"; // 0 on both sides
  for (TRec& tr : recs) {
    if (tr.rel == zeroT || tr.rel == zeroBoth) tr.fatTime = 0;
  }
  const auto t = writeT(recs);
  // Every other file shifted an hour: the skew is still found from them.
  for (auto& kv : card.files) kv.second.fatTime = shifted(kv.second.fatTime, 3600);
  card.at(badCard).fatTime = 0x5C0773D5;
  card.at(zeroBoth).fatTime = 0;
  MemD d;
  const auto r = walk(card, d, WalkOpts{&t, true});
  assertDone(r);
  TEST_ASSERT_EQUAL_INT32(3600, r.summary.skew);
  TEST_ASSERT_EQUAL_UINT32(20, r.doubtful);
  TEST_ASSERT_EQUAL_UINT32(17, r.bySkew);
  TEST_ASSERT_EQUAL_UINT32(3, r.qfpReads);
  TEST_ASSERT_EQUAL_UINT32(3, r.byQfp);
  for (const std::string& p : {zeroT, badCard, zeroBoth}) {
    TEST_ASSERT_TRUE(d.row(p).confirmed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(d.row(p).status));
  }
  for (const auto& dd : d.doubts) {
    const bool invalid = dd.first == zeroT || dd.first == badCard || dd.first == zeroBoth;
    TEST_ASSERT_EQUAL(!invalid, dd.second.hasDelta);
  }
  assertBuilderAgrees(d, recs);
  assertQuiet(card, d, &t);
  // A zero-stamped file equal to D's row (0 = 0) is unchanged for the walk.
  TEST_ASSERT_EQUAL_UINT32(0, d.row(zeroBoth).fatTime);
}

// 2.18's skew vectors, through the walk.
void test_skew_vectors_through_the_walk() {
  struct Case {
    int plus, minus, zero, two;
    int32_t skew;
  } cases[] = {
      {20, 0, 0, 0, 3600},   // all at +3,600
      {7, 0, 0, 0, 0},       // fewer than 8
      {10, 0, 20, 0, 0},     // fewer than half
      {0, 0, 0, 20, 0},      // +2: not a multiple of 900
      {10, 10, 0, 0, -3600}, // equal counts and |D|: the negative wins
  };
  for (const Case& k : cases) {
    fakefat::Card card;
    int i = 0;
    auto add = [&](int n, int32_t by) {
      for (int j = 0; j < n; ++j, ++i) {
        char p[32];
        std::snprintf(p, sizeof(p), "A/B/%03d.mp3", i);
        card.addFile(p, 1000 + static_cast<uint32_t>(i), kT, static_cast<uint32_t>(i) + 1);
      }
      return by;
    };
    std::vector<std::pair<int, int32_t>> spans = {{k.plus, 3600}, {k.minus, -3600}, {k.zero, 0}, {k.two, 2}};
    for (auto& s : spans) add(s.first, s.second);
    const auto recs = transferOf(card);
    const auto t = writeT(recs);
    i = 0;
    for (auto& s : spans)
      for (int j = 0; j < s.first; ++j, ++i) {
        char p[32];
        std::snprintf(p, sizeof(p), "A/B/%03d.mp3", i);
        card.at(p).fatTime = shifted(kT, s.second);
      }
    MemD d;
    const auto r = walk(card, d, WalkOpts{&t, true});
    assertDone(r);
    TEST_ASSERT_EQUAL_INT32(k.skew, r.summary.skew);
    const uint32_t shiftedFiles = static_cast<uint32_t>(k.plus + k.minus + k.two);
    TEST_ASSERT_EQUAL_UINT32(shiftedFiles, r.doubtful);
    const uint32_t bySkew = k.skew == 3600 ? k.plus : (k.skew == -3600 ? k.minus : 0);
    TEST_ASSERT_EQUAL_UINT32(bySkew, r.bySkew);
    TEST_ASSERT_EQUAL_UINT32(shiftedFiles - bySkew, r.qfpReads);
    for (const auto& kv : d.files) TEST_ASSERT_EQUAL_INT(static_cast<int>(St::Software), static_cast<int>(kv.second.status));
    assertBuilderAgrees(d, recs);
    assertQuiet(card, d, &t);
  }
}

// More than 256 distinct deltas: the histogram is a summary, and the pass
// over the doubts counts the skew exactly (2.3.4: the order doesn't matter).
void test_recount_past_256_distinct_deltas() {
  for (bool hourFirst : {true, false}) {
    fakefat::Card card;
    // 300 tracks shifted an hour, and 300 covers each shifted its own way:
    // the hour is exactly half the pairs, which passes; the summary's count
    // for it is short by the slots it lost, which wouldn't.
    for (int i = 0; i < 300; ++i) {
      char p[48];
      std::snprintf(p, sizeof(p), "%s/T/%03d.mp3", hourFirst ? "A" : "B", i);
      card.addFile(p, 1000 + static_cast<uint32_t>(i), kT, static_cast<uint32_t>(i) + 1);
      std::snprintf(p, sizeof(p), "%s/C/%03d.jpg", hourFirst ? "B" : "A", i);
      card.addFile(p, 2000 + static_cast<uint32_t>(i), kT, static_cast<uint32_t>(i) + 5000);
    }
    const auto recs = transferOf(card);
    const auto t = writeT(recs);
    int k = 0;
    for (auto& kv : card.files) {
      if (isAudio(kv.first))
        kv.second.fatTime = shifted(kT, 3600);
      else
        kv.second.fatTime = shifted(kT, 2 * (++k));  // 300 distinct deltas
    }
    MemD d;
    const auto r = walk(card, d, WalkOpts{&t, true});
    assertDone(r);
    TEST_ASSERT_TRUE(r.recounted);
    TEST_ASSERT_EQUAL_INT32(3600, r.summary.skew);
    TEST_ASSERT_EQUAL_UINT32(300, r.bySkew);
    TEST_ASSERT_EQUAL_UINT32(300, r.countOnly);
    TEST_ASSERT_EQUAL_UINT32(0, r.qfpReads);
    // The hour first: without the pass the summary alone would find none
    // (never another skew). Covers first, it would; the pass agrees.
    cc::SkewHistogram h;
    for (const auto& dd : d.doubts) {
      const int64_t delta = dd.second.delta;
      h.add(kT, cc::fatTimeFromWall(cc::fatWallSeconds(kT) + delta));
    }
    TEST_ASSERT_TRUE(h.needsRecount());
    TEST_ASSERT_EQUAL_INT32(hourFirst ? 0 : 3600, h.skew());
  }
}

// ---- failures and steps ----

void test_failures_leave_d_as_it_was() {
  fakefat::Card card;
  album(card, "Artist/Album", 5);
  album(card, "Artist/More", 5, kT, 50);
  auto t = writeT(transferOf(card));
  MemD d;
  assertDone(walk(card, d, WalkOpts{&t, true}));
  const auto filesBefore = d.files;
  for (auto& kv : card.files) kv.second.fatTime = shifted(kv.second.fatTime, 7200);

  // A listing that fails: the walk stops, and nothing it gave counts.
  card.failOpenAt = static_cast<int>(card.opens) + 3;
  auto r = walk(card, d, WalkOpts{&t});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::Card), static_cast<int>(r.error));
  TEST_ASSERT_TRUE(d.aborted);
  TEST_ASSERT_FALSE(d.finished);
  card.failOpenAt = -1;
  card.failNextAt = static_cast<int>(card.nexts) + 5;
  r = walk(card, d, WalkOpts{&t});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::Card), static_cast<int>(r.error));
  card.failNextAt = -1;

  // T damaged (a flipped bit in a record): known only at its end. The walk
  // fails, to be run again without it.
  {
    cc::MemSource s(t.data(), static_cast<uint32_t>(t.size()));
    cc::Container ct;
    mptg::Info info;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::Ok), static_cast<int>(mptg::openFile(ct, s, &info)));
    t[ct.section(mptg::kSecRecs).offset + 26] ^= 0x10;  // a qfp
  }
  r = walk(card, d, WalkOpts{&t, true});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::Transfer), static_cast<int>(r.error));
  TEST_ASSERT_TRUE(d.aborted);

  // D unreadable.
  d.failFolders = true;
  r = walk(card, d, WalkOpts{&t});
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cw::CardWalk::Error::Known), static_cast<int>(r.error));
  d.failFolders = false;

  TEST_ASSERT_TRUE(filesBefore == d.files);
}

// One folder listing, or one qfp read, per step.
void test_steps_are_small() {
  fakefat::Card card;
  for (int a = 0; a < 6; ++a) album(card, "Artist " + std::to_string(a) + "/Album", 4, kT, 10u * a + 1);
  const auto t = writeT(transferOf(card));
  for (auto& kv : card.files) kv.second.fatTime = shifted(kv.second.fatTime, 2);  // no skew: every track read
  MemD d;
  d.beginWalk();
  std::vector<uint8_t> scratch(cw::CardWalk::kMinScratch);
  std::vector<uint8_t> tScratch(1024);
  cc::MemSource src(t.data(), static_cast<uint32_t>(t.size()));
  cw::StreamedTransfer st;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(cc::Why::Ok), static_cast<int>(st.begin(src, tScratch.data(), 1024)));
  cw::CardWalk::Config c;
  c.lister = &card;
  c.known = &d;
  c.transfer = &st;
  c.sink = &d;
  c.firstAfterCommit = true;
  c.scratch = scratch.data();
  c.scratchBytes = static_cast<uint32_t>(scratch.size());
  auto w = std::make_unique<cw::CardWalk>();
  TEST_ASSERT_TRUE(w->begin(c));
  uint32_t steps = 0;
  for (;;) {
    const uint32_t opens = card.opens, reads = card.fileOpens;
    const auto s = w->step();
    ++steps;
    TEST_ASSERT_TRUE(card.opens - opens <= 1);
    TEST_ASSERT_TRUE(card.fileOpens - reads <= 1);
    if (s == cw::CardWalk::State::Done || s == cw::CardWalk::State::Failed) break;
    TEST_ASSERT_TRUE(steps < 10000);
  }
  assertDone(w->result());
  TEST_ASSERT_EQUAL_UINT32(24, w->result().qfpReads);
  TEST_ASSERT_EQUAL_UINT32(steps, w->result().steps);
  TEST_ASSERT_TRUE(d.orderOk);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_digest_and_qfp_helpers);
  RUN_TEST(test_first_walk_lists_in_canonical_order);
  RUN_TEST(test_shuffled_order_gives_the_same_walk);
  RUN_TEST(test_big_folders_through_a_small_scratch);
  RUN_TEST(test_long_names_and_deep_folders_through_the_least_scratch);
  RUN_TEST(test_what_the_device_sees);
  RUN_TEST(test_folder_rows_and_covers);
  RUN_TEST(test_transfer_rows_and_the_builder);
  RUN_TEST(test_unreadable_transfer_record);
  RUN_TEST(test_retag_at_the_same_size);
  RUN_TEST(test_renamed_folder);
  RUN_TEST(test_deleted_album);
  RUN_TEST(test_every_stamp_shifted_an_hour);
  RUN_TEST(test_three_files_shifted);
  RUN_TEST(test_invalid_and_zero_stamps);
  RUN_TEST(test_skew_vectors_through_the_walk);
  RUN_TEST(test_recount_past_256_distinct_deltas);
  RUN_TEST(test_failures_leave_d_as_it_was);
  RUN_TEST(test_steps_are_small);
  return UNITY_END();
}
