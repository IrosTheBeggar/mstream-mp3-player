// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for TagStore (docs/METADATA.md 3.3.2, 2.12.6; milestone N4):
// D with its device sections against N1's writer byte for byte, the merged
// view's precedence (D, the chunks before the walk, the walk's two runs, the
// chunks after it), the journals' torn tails and stale bases, Rescan and a
// new parser, the cut-rename rule (twins until a disk check), a power cut at
// every write, sync, remove and rename of a session (a rename cut between its
// two directory writes included) in three ways (the steps before it, the
// unsynced writes lost, the cut write torn) with the recovery itself cut
// again, the compaction's memory fixed whatever the journal holds, N2's
// builder fed through the adapters, and the compaction at 20k.
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "../support/CutFs.h"
#include "../support/SynthCard.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "TagStore.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
namespace ts = tagstore;
using cutfs::CutFs;
using cutfs::Variant;
using ts::Status;

namespace {

// ---------------------------------------------------------------------------
// A counting allocator: the store's work memory.
// ---------------------------------------------------------------------------
struct Heap {
  static size_t live, peak;
  static std::map<void*, size_t>& blocks() {
    static std::map<void*, size_t> b;
    return b;
  }
  static void reset() {
    live = peak = 0;
    blocks().clear();
  }
  static void* alloc(size_t n) {
    void* p = std::malloc(n ? n : 1);
    blocks()[p] = n;
    live += n;
    if (live > peak) peak = live;
    return p;
  }
  static void release(void* p) {
    auto it = blocks().find(p);
    if (it == blocks().end()) {
      TEST_FAIL_MESSAGE("freed a block nobody allocated");
      return;
    }
    live -= it->second;
    blocks().erase(it);
    std::free(p);
  }
};
size_t Heap::live = 0;
size_t Heap::peak = 0;

// ---------------------------------------------------------------------------
// The logical state the store must show: per file, its row, record and
// fields; per folder, the walk's facts; D's header.
// ---------------------------------------------------------------------------
struct PathLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return cc::compareFilePaths(a.data(), a.size(), b.data(), b.size()) < 0;
  }
};
struct FolderLess {
  bool operator()(const std::string& a, const std::string& b) const {
    return cc::compareFolderPaths(a.data(), a.size(), b.data(), b.size()) < 0;
  }
};

struct MRow {
  ts::Row row;
  mptg::Record rec;  // folder, name and strings 0
  std::vector<std::string> fields = std::vector<std::string>(cc::kRunFields);
};

bool sameRecord(const mptg::Record& a, const mptg::Record& b) {
  uint8_t x[mptg::kRecsStride], y[mptg::kRecsStride];
  mptg::Record ca = a, cb = b;
  ca.folder = ca.name = ca.strings = 0;
  cb.folder = cb.name = cb.strings = 0;
  mptg::encodeRecord(ca, x);
  mptg::encodeRecord(cb, y);
  return std::memcmp(x, y, sizeof(x)) == 0;
}

bool sameRow(const MRow& a, const MRow& b) {
  return a.row.status == b.row.status && a.row.confirmed == b.row.confirmed && sameRecord(a.rec, b.rec) &&
         a.fields == b.fields;
}

struct State {
  std::map<std::string, MRow, PathLess> files;
  std::map<std::string, ts::FolderFacts, FolderLess> facts;
  ts::DeviceHeader header;  // D's after the next compaction
  bool walkPending = false;
  ts::DeviceHeader walk;     // the walk to merge
  bool walkUnsettled = false;  // a walk's run 1 alone to merge, a doubt in it: D unwalked after it
  bool walkSettles = false;    // the walk to merge has a doubt to settle in its run 1
};

mptg::Record bare(uint32_t size, uint32_t fatTime, uint64_t qfp = 0) {
  mptg::Record r;
  r.size = size;
  r.fatTime = fatTime;
  r.qfp = qfp;
  return r;
}

// A scan result.
struct Scan {
  std::string path;
  Status status = Status::Scanned;
  mptg::Record rec;
  std::vector<std::string> fields = std::vector<std::string>(cc::kRunFields);
};

Scan scanned(const std::string& path, uint32_t size, uint32_t time, uint32_t seed) {
  Scan s;
  s.path = path;
  s.rec.size = size;
  s.rec.fatTime = time;
  s.rec.durationMs = 180000 + seed * 37;
  s.rec.known = mptg::kKnownRules1;
  s.rec.year = static_cast<uint16_t>(1980 + seed % 40);
  s.rec.track = static_cast<uint16_t>(seed % 20 + 1);
  s.rec.container = mptg::kContainerMp3;
  s.rec.qfp = 0x9E3779B97F4A7C15ull * (seed + 1);
  s.fields[cc::kTitle] = "Title " + std::to_string(seed);
  s.fields[cc::kArtist] = "Artist " + std::to_string(seed % 5);
  if (seed % 3 == 0) s.fields[cc::kArtist] += std::string("\x1F") + "Guest " + std::to_string(seed % 7);
  s.fields[cc::kAlbum] = "Album " + std::to_string(seed % 4);
  if (seed % 2) s.fields[cc::kGenre] = "Rock";
  if (seed % 5 == 1) s.fields[cc::kAlbumSort] = "Sort " + std::to_string(seed);
  return s;
}

Scan unreadable(const std::string& path, uint32_t size, uint32_t time) {
  Scan s;
  s.path = path;
  s.status = Status::Unreadable;
  s.rec.size = size;
  s.rec.fatTime = time;
  s.rec.flags = mptg::kUnreadable;
  return s;
}

std::vector<uint8_t>& chunkBuffer() {
  static std::vector<uint8_t> b(96 * 1024);
  return b;
}

bool append(ts::TagStore& st, State& m, const std::vector<Scan>& scans) {
  ts::ChunkBuilder c;
  c.begin(chunkBuffer().data(), static_cast<uint32_t>(chunkBuffer().size()));
  for (const Scan& s : scans) {
    const char* f[cc::kRunFields];
    for (uint32_t k = 0; k < cc::kRunFields; ++k) f[k] = s.fields[k].c_str();
    if (!c.add(s.path.data(), s.path.size(), s.status, s.rec, f)) return false;
  }
  if (!st.append(c)) return false;
  for (const Scan& s : scans) {
    MRow r;
    r.row.status = s.status;
    r.rec = s.rec;
    if (s.status == Status::Unreadable) {
      r.rec.flags |= mptg::kUnreadable;
    } else {
      r.fields = s.fields;
    }
    m.files[s.path] = r;
  }
  return true;
}

// One entry of a walk.
struct WOp {
  enum Kind { File, Gone, Doubt, Folder, FolderGone } kind = File;
  std::string path;
  uint32_t size = 0, time = 0;
  uint64_t qfp = 0;
  Status status = Status::Pending;
  bool confirmed = false;
  ts::Doubt doubt;
  ts::FolderFacts facts;
};
WOp wFile(const std::string& p, uint32_t size, uint32_t time, Status s = Status::Pending, bool confirmed = false,
          uint64_t qfp = 0) {
  WOp o;
  o.kind = WOp::File;
  o.path = p;
  o.size = size;
  o.time = time;
  o.status = s;
  o.confirmed = confirmed;
  o.qfp = qfp;
  return o;
}
WOp wGone(const std::string& p) {
  WOp o;
  o.kind = WOp::Gone;
  o.path = p;
  return o;
}
// A doubt as N5 keeps it: `settle` for an audio file (its row given without
// T first), else only for the skew's recount.
WOp wDoubt(const std::string& p, uint32_t size, uint32_t time, int64_t delta, bool settle = true) {
  WOp o;
  o.kind = WOp::Doubt;
  o.path = p;
  o.doubt.settle = settle;
  o.doubt.hasDelta = true;
  o.doubt.delta = delta;
  o.doubt.size = size;
  o.doubt.fatTime = time;
  o.doubt.transferQfp = 0x1234567890ull + size;
  o.doubt.deviceQfp = settle ? 0 : 7;
  o.doubt.fallback = Status::Pending;
  return o;
}
ts::FolderFacts facts(uint64_t digest, const char* cover = nullptr, bool owned = false) {
  ts::FolderFacts f;
  f.digest = digest;
  f.audio = static_cast<uint32_t>(digest % 13);
  f.others = static_cast<uint32_t>(digest % 7);
  if (cover) {
    f.imageLength = static_cast<uint8_t>(std::strlen(cover));
    std::memcpy(f.image, cover, f.imageLength);
    f.image[f.imageLength] = 0;
    f.images = 1;
    f.imageRank = 1;
    f.imageSize = 40000 + static_cast<uint32_t>(digest % 1000);
    f.imageTime = 0x5D4773D5;
    f.imageOwned = owned;
  }
  return f;
}
WOp wFolder(const std::string& p, const ts::FolderFacts& f) {
  WOp o;
  o.kind = WOp::Folder;
  o.path = p;
  o.facts = f;
  return o;
}
WOp wFolderGone(const std::string& p) {
  WOp o;
  o.kind = WOp::FolderGone;
  o.path = p;
  return o;
}

// A walk run's order, written apart from TagStore's: (folder in pre-order,
// a folder's own entry first, then names; a doubt after its file's row).
// TagStore needs each kind in its order only; this one order keeps all
// three.
bool keyLess(const WOp& a, const WOp& b) {
  auto isFolder = [](const WOp& o) { return o.kind == WOp::Folder || o.kind == WOp::FolderGone; };
  auto split = [&](const WOp& o, std::string* folder, std::string* name) {
    if (isFolder(o)) {
      *folder = o.path;
      name->clear();
      return;
    }
    const size_t k = o.path.rfind('/');
    *folder = k == std::string::npos ? "" : o.path.substr(0, k);
    *name = k == std::string::npos ? o.path : o.path.substr(k + 1);
  };
  std::string fa, na, fb, nb;
  split(a, &fa, &na);
  split(b, &fb, &nb);
  const int c = cc::compareFolderPaths(fa.data(), fa.size(), fb.data(), fb.size());
  if (c) return c < 0;
  const bool af = isFolder(a), bf = isFolder(b);
  if (af != bf) return af;
  const int n = cc::compareNames(na.data(), na.size(), nb.data(), nb.size());
  if (n) return n < 0;
  return a.kind != WOp::Doubt && b.kind == WOp::Doubt;
}

bool write(ts::WalkWriter& w, const WOp& o) {
  switch (o.kind) {
    case WOp::File:
      return w.file(o.path.data(), o.path.size(), o.size, o.time, o.qfp, o.status, o.confirmed);
    case WOp::Gone:
      return w.gone(o.path.data(), o.path.size());
    case WOp::Doubt:
      return w.doubt(o.path.data(), o.path.size(), o.doubt);
    case WOp::Folder:
      return w.folder(o.path.data(), o.path.size(), o.facts);
    case WOp::FolderGone:
      return w.folderGone(o.path.data(), o.path.size());
  }
  return false;
}

void applyWalk(State& m, const std::vector<WOp>& ops) {
  for (const WOp& o : ops) {
    switch (o.kind) {
      case WOp::File: {
        // A reading at the same size and time stands, unless T covers the
        // file now; one the walk names that isn't there: Pending.
        auto it = m.files.find(o.path);
        const Status was = it != m.files.end() ? it->second.row.status : Status::Pending;
        const bool reading = (was == Status::Scanned || was == Status::Unreadable) &&
                             it->second.rec.size == o.size && it->second.rec.fatTime == o.time;
        if (o.status != Status::Software && reading) {
          if (o.qfp) it->second.rec.qfp = o.qfp;
          break;
        }
        const bool named = o.status == Status::Software || o.status == Status::Pending;
        const bool same = it != m.files.end() && it->second.rec.size == o.size && it->second.rec.fatTime == o.time;
        MRow r;
        r.row.status = o.status == Status::Software ? Status::Software : Status::Pending;
        r.row.confirmed = named && o.confirmed;
        r.rec = bare(o.size, o.time, o.qfp ? o.qfp : (same ? it->second.rec.qfp : 0));
        m.files[o.path] = r;
        break;
      }
      case WOp::Gone:
        m.files.erase(o.path);
        break;
      case WOp::Folder:
        m.facts[o.path] = o.facts;
        break;
      case WOp::Doubt:       // read back by the walk; no row changes
      case WOp::FolderGone:  // its files went (Gone); the folder follows them
        break;
    }
  }
}

// When set, walk() leaves here the state after its run 1 alone (a cut
// between the runs' Ends leaves the card so).
State* gMid = nullptr;

bool walk(ts::TagStore& st, State& m, const ts::Identity& id, int32_t skew, std::vector<WOp> run1,
          std::vector<WOp> run2 = {}, uint32_t bufBytes = 1024) {
  std::sort(run1.begin(), run1.end(), keyLess);
  std::sort(run2.begin(), run2.end(), keyLess);
  std::vector<uint8_t> buf(bufBytes);
  ts::WalkWriter w;
  if (!st.beginWalk(id, buf.data(), bufBytes, &w)) return false;
  bool ok = true;
  for (const WOp& o : run1) ok = ok && write(w, o);
  ok = ok && w.endRun(skew);
  for (const WOp& o : run2) ok = ok && write(w, o);
  ok = ok && w.endRun(skew);
  ok = w.finish() && ok;
  if (!ok) return false;
  applyWalk(m, run1);
  m.walkPending = true;
  m.walk.walked = true;
  m.walk.walk = id;
  m.walk.skew = skew;
  m.walkSettles = false;
  for (const WOp& o : run1) m.walkSettles = m.walkSettles || (o.kind == WOp::Doubt && o.doubt.settle);
  if (gMid && !run2.empty()) {
    *gMid = m;  // run 1 alone: its rows, not its commit (DHDR waits for a whole walk)
    gMid->walkPending = false;
    // With a doubt to settle in it, DHDR unwalked (the next walk asks T
    // again); else as it was.
    for (const WOp& o : run1) gMid->walkUnsettled = gMid->walkUnsettled || (o.kind == WOp::Doubt && o.doubt.settle);
  }
  applyWalk(m, run2);
  m.walkPending = true;
  m.walk.walked = true;
  m.walk.walk = id;
  m.walk.skew = skew;
  return true;
}

// What a compaction does to the logical state: nothing, but D's header,
// the facts of folders no file is under, and a Rescan.
void compacted(State& m, bool rescan, uint16_t parserChanged = 0) {
  if (m.walkPending) {
    m.header.walked = true;
    m.header.walk = m.walk.walk;
    m.header.skew = m.walk.skew;
    m.walkPending = false;
  } else if (m.walkUnsettled) {
    m.header.walked = false;
  }
  m.walkUnsettled = false;
  if (rescan) ++m.header.epoch;
  if (rescan || parserChanged) {
    for (auto& kv : m.files) {
      MRow& r = kv.second;
      if (r.row.status == Status::Scanned || r.row.status == Status::Unreadable) {
        const uint32_t size = r.rec.size, time = r.rec.fatTime;
        const uint64_t qfp = r.rec.qfp;
        r = MRow();
        r.rec = bare(size, time, qfp);
      }
    }
  }
  for (auto it = m.facts.begin(); it != m.facts.end();) {
    bool used = it->first.empty();
    for (const auto& f : m.files) {
      const std::string& p = f.first;
      const std::string& d = it->first;
      if (p.size() > d.size() && p.compare(0, d.size(), d) == 0 && p[d.size()] == '/') used = true;
    }
    if (used)
      ++it;
    else
      it = m.facts.erase(it);
  }
}

// The store's merged view.
std::vector<std::pair<std::string, MRow>> view(ts::TagStore& st) {
  std::vector<std::pair<std::string, MRow>> out;
  ts::TagStore::View v;
  if (!st.openView(&v)) {
    TEST_FAIL_MESSAGE("the view didn't open");
    return out;
  }
  cc::RunFields* rf = new cc::RunFields();
  while (v.next()) {
    MRow r;
    r.row = v.row();
    r.rec = v.record();
    r.rec.folder = r.rec.name = r.rec.strings = 0;
    TEST_ASSERT_TRUE(v.run(rf));
    for (uint32_t k = 0; k < cc::kRunFields; ++k) r.fields[k] = std::string(rf->get(k), rf->len(k));
    out.emplace_back(std::string(v.path(), v.pathLength()), r);
  }
  delete rf;
  TEST_ASSERT_FALSE_MESSAGE(v.failed(), "the view failed");
  return out;
}

bool viewIs(ts::TagStore& st, const State& m, std::string* why = nullptr) {
  const auto v = view(st);
  if (v.size() != m.files.size()) {
    if (why) *why = "count " + std::to_string(v.size()) + " against " + std::to_string(m.files.size());
    return false;
  }
  size_t i = 0;
  for (const auto& kv : m.files) {
    if (v[i].first != kv.first) {
      if (why) *why = "path " + v[i].first + " against " + kv.first;
      return false;
    }
    if (!sameRow(v[i].second, kv.second)) {
      if (why) *why = "row of " + kv.first;
      return false;
    }
    ++i;
  }
  return true;
}

void assertView(ts::TagStore& st, const State& m) {
  std::string why;
  const bool ok = viewIs(st, m, &why);
  TEST_ASSERT_TRUE_MESSAGE(ok, why.c_str());
}

// The folders D must hold (every ancestor of a file, /music always), in
// pre-order.
std::vector<std::string> foldersOf(const State& m) {
  std::map<std::string, int, FolderLess> f;
  f[""] = 0;
  for (const auto& kv : m.files)
    for (size_t k = kv.first.find('/'); k != std::string::npos; k = kv.first.find('/', k + 1)) f[kv.first.substr(0, k)];
  std::vector<std::string> v;
  for (const auto& kv : f) v.push_back(kv.first);
  return v;
}

// D as N1's writer makes it from the logical state, the device's sections
// built here from the state.
std::vector<uint8_t> expectedDevice(const State& m, const ts::TagStore& st) {
  std::vector<mptg::RecordIn> recs;
  for (const auto& kv : m.files) {
    mptg::RecordIn r;
    r.path = kv.first.c_str();
    r.rec = kv.second.rec;
    for (uint32_t k = 0; k < cc::kRunFields; ++k) {
      r.fields[k] = kv.second.fields[k].c_str();
    }
    recs.push_back(r);
  }
  // DSTA
  const std::vector<std::string> folders = foldersOf(m);
  uint32_t ownRecords = 0;
  std::map<std::string, bool, FolderLess> own;
  for (const auto& kv : m.files) {
    if (kv.second.row.status == Status::Software) continue;
    ++ownRecords;
    own[""] = true;
    for (size_t k = kv.first.find('/'); k != std::string::npos; k = kv.first.find('/', k + 1))
      own[kv.first.substr(0, k)] = true;
  }
  std::vector<uint8_t> dsta(ts::kDstaHeader);
  cc::put32(dsta.data(), static_cast<uint32_t>(m.files.size()));
  cc::put32(dsta.data() + 4, ownRecords);
  cc::put32(dsta.data() + 8, static_cast<uint32_t>(own.size()));
  for (const auto& kv : m.files) dsta.push_back(ts::encodeRow(kv.second.row));
  // DFLD
  std::vector<uint8_t> dfld(ts::kDfldHeader);
  cc::put32(dfld.data(), static_cast<uint32_t>(folders.size()));
  for (const std::string& f : folders) {
    ts::FolderFacts x;
    auto it = m.facts.find(f);
    if (it != m.facts.end()) x = it->second;
    uint8_t b[ts::kFactsFixed + 256];
    const uint32_t n = ts::encodeFacts(x, b);
    dfld.insert(dfld.end(), b, b + n);
  }
  std::vector<uint8_t> dhdr(ts::kDhdrBytes);
  ts::encodeHeader(m.header, dhdr.data());
  mptg::ExtraSection extra[3];
  extra[0].type = ts::kDsta;
  extra[0].data = dsta.data();
  extra[0].bytes = static_cast<uint32_t>(dsta.size());
  extra[1].type = ts::kDfld;
  extra[1].data = dfld.data();
  extra[1].bytes = static_cast<uint32_t>(dfld.size());
  extra[2].type = ts::kDhdr;
  extra[2].data = dhdr.data();
  extra[2].bytes = static_cast<uint32_t>(dhdr.size());
  mptg::Meta meta;
  meta.generation = st.device().generation;
  meta.cardId = st.config().cardId;
  meta.source = mptg::kSourceDevice;
  meta.parserVersion = st.config().parserVersion;
  meta.producer = st.config().producer;
  synthcard::VecSink out;
  const char* error = nullptr;
  if (!mptg::write(out, meta, recs.data(), recs.size(), nullptr, 0, extra, 3, nullptr, &error)) {
    TEST_FAIL_MESSAGE(error);
  }
  return out.bytes;
}

// D on the card: N1's checks with HIDX, the device's sections in step, the
// facts, and the bytes N1's writer makes.
void assertDevice(CutFs& fs, ts::TagStore& st, const State& m) {
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  TEST_ASSERT_TRUE_MESSAGE(!d.empty(), "no tags.bin");
  cc::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> scratch(8192);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::check(src, mptg::kUseAll, scratch.data(), 8192)));
  ts::DeviceReader* r = new ts::DeviceReader();
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(r->begin(src, scratch.data(), 8192)));
  const std::vector<std::string> folders = foldersOf(m);
  size_t f = 0, rec = 0;
  for (;;) {
    const ts::DeviceReader::Step s = r->next();
    if (s == ts::DeviceReader::Step::Folder) {
      TEST_ASSERT_TRUE(f < folders.size());
      TEST_ASSERT_EQUAL_STRING(folders[f].c_str(), r->walker().path());
      ts::FolderFacts x;
      auto it = m.facts.find(folders[f]);
      if (it != m.facts.end()) x = it->second;
      TEST_ASSERT_TRUE_MESSAGE(r->folderFacts() == x, folders[f].c_str());
      ++f;
    } else if (s == ts::DeviceReader::Step::Record) {
      ++rec;
    } else {
      TEST_ASSERT_TRUE_MESSAGE(s == ts::DeviceReader::Step::End, cc::whyName(r->why()));
      break;
    }
  }
  delete r;
  TEST_ASSERT_EQUAL_size_t(folders.size(), f);
  TEST_ASSERT_EQUAL_size_t(m.files.size(), rec);
  const std::vector<uint8_t> want = expectedDevice(m, st);
  TEST_ASSERT_EQUAL_size_t(want.size(), d.size());
  TEST_ASSERT_TRUE_MESSAGE(want == d, "tags.bin isn't what N1's writer makes of the same rows");
}

ts::TagStore::Config config() {
  ts::TagStore::Config c;
  c.producer = "mstream-player 0.8.0";
  c.parserVersion = 3;
  c.cardId = 0x5EEDC0DE0A1B2C3Dull;
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

constexpr uint32_t kT = 0x5D4773D5;  // 2026-10-07 14:30:42

}  // namespace

void setUp() { Heap::reset(); }
void tearDown() {}

// ---------------------------------------------------------------------------
// The encodings.
// ---------------------------------------------------------------------------
void test_rows_header_and_facts() {
  for (int s = 0; s < 4; ++s)
    for (int c = 0; c < 2; ++c) {
      ts::Row r{static_cast<Status>(s), c == 1};
      const ts::Row d = ts::decodeRow(ts::encodeRow(r));
      TEST_ASSERT_TRUE(d.status == r.status && d.confirmed == r.confirmed);
    }
  TEST_ASSERT_EQUAL_UINT8(0, ts::encodeRow(ts::Row{Status::Scanned, false}));
  TEST_ASSERT_EQUAL_UINT8(1 | 4, ts::encodeRow(ts::Row{Status::Software, true}));
  ts::DeviceHeader h;
  h.walked = true;
  h.walk = commit(7);
  h.skew = -3600;
  h.epoch = 4;
  uint8_t b[ts::kDhdrBytes];
  ts::encodeHeader(h, b);
  ts::DeviceHeader g;
  TEST_ASSERT_TRUE(ts::decodeHeader(b, ts::kDhdrBytes, &g));
  TEST_ASSERT_TRUE(g.walked && g.walk == h.walk && g.skew == -3600 && g.epoch == 4);
  b[0] = 2;  // a newer DHDR: D absent to this firmware
  TEST_ASSERT_FALSE(ts::decodeHeader(b, ts::kDhdrBytes, &g));
  const ts::FolderFacts f = facts(0x1234, "cover.jpg", true);
  uint8_t e[ts::kFactsFixed + 256];
  TEST_ASSERT_EQUAL_UINT32(ts::kFactsFixed + 9, ts::encodeFacts(f, e));
  const LibraryIndex::FolderFacts i = f.index();
  TEST_ASSERT_EQUAL_STRING("cover.jpg", i.image);
  TEST_ASSERT_TRUE(i.imageOwned);
  TEST_ASSERT_EQUAL_UINT8(1, i.imageCount);
  TEST_ASSERT_EQUAL_UINT16(1 + 0x1234 % 7, i.otherCount);  // images included, as cardwalk::factsOf()
  ts::FolderFacts many;
  many.images = 300;
  many.others = 70000;
  TEST_ASSERT_EQUAL_UINT8(255, many.index().imageCount);
  TEST_ASSERT_EQUAL_UINT16(65535, many.index().otherCount);
  TEST_ASSERT_NULL(ts::FolderFacts().index().image);
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kNoImage, ts::FolderFacts().imageRank);
}

// ---------------------------------------------------------------------------
// D, written by the compaction, is what N1's writer makes of the same rows,
// and the view shows each source's rows by its rules.
// ---------------------------------------------------------------------------
void test_compaction_writes_what_the_writer_writes() {
  CutFs fs;
  ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
  st.open();
  State m;
  // An empty card: D with /music alone.
  ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  compacted(m, false);
  assertDevice(fs, st, m);
  TEST_ASSERT_EQUAL_UINT32(0, c.records);
  // The first walk: names whose canonical order isn't their bytes' (2.18).
  const std::vector<WOp> run1 = {
      wFile("A/x.mp3", 1000, kT),        wFile("A B/y.mp3", 1001, kT),       wFile("A-/z.mp3", 1002, kT),
      wFile("B/Album/01.mp3", 2000, kT), wFile("B/Album/02.mp3", 2001, kT, Status::Software),
      wFile("B/Album/CD1/01.flac", 3000, kT, Status::Pending, false, 0xFEEDu), wFile("a/lower.mp3", 4000, kT),
      wFile("\xC3\x89/e.opus", 5000, kT),
      wFile("loose.mp3", 6000, kT),      wFolder("", facts(1)),               wFolder("B/Album", facts(2, "cover.jpg")),
      wFolder("A", facts(3, "folder.jpg", true)), wFolder("Empty", facts(4, "x.jpg")),
  };
  TEST_ASSERT_TRUE(walk(st, m, commit(1), 3600, run1));
  TEST_ASSERT_TRUE(st.hasWalk());
  assertView(st, m);
  c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  TEST_ASSERT_TRUE(c.walkMerged);
  compacted(m, false);
  TEST_ASSERT_FALSE(m.facts.count("Empty"));  // no file under it: not in D
  assertDevice(fs, st, m);
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
  TEST_ASSERT_FALSE(fs.exists("/.player/hidx.tmp"));
  TEST_ASSERT_TRUE(st.device().header.walked);
  TEST_ASSERT_TRUE(st.device().header.walk == commit(1));
  TEST_ASSERT_EQUAL_INT32(3600, st.device().header.skew);
  // DSTA's header: every row but the Software one is D's own; their folders.
  TEST_ASSERT_EQUAL_UINT32(8, st.device().ownRecords);
  // Scans, one unreadable; the D they extend; then again over D.
  TEST_ASSERT_TRUE(append(st, m,
                          {scanned("A/x.mp3", 1000, kT, 1), scanned("B/Album/01.mp3", 2000, kT, 2),
                           unreadable("loose.mp3", 6000, kT), scanned("\xC3\x89/e.opus", 5000, kT, 3)}));
  TEST_ASSERT_TRUE(append(st, m, {scanned("A B/y.mp3", 1001, kT, 4), scanned("A/x.mp3", 1000, kT, 5)}));
  TEST_ASSERT_EQUAL_UINT32(2, st.chunkCount());
  TEST_ASSERT_EQUAL_UINT32(2, st.journalSeq());
  assertView(st, m);
  c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  TEST_ASSERT_EQUAL_UINT32(2, c.chunksMerged);
  compacted(m, false);
  assertDevice(fs, st, m);
  TEST_ASSERT_EQUAL_UINT32(0, st.journalSeq());
  TEST_ASSERT_FALSE(fs.exists(st.journalPath()));
  // A compaction with nothing new writes the same rows again.
  c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  compacted(m, false);
  assertDevice(fs, st, m);
  assertView(st, m);
}

// ---------------------------------------------------------------------------
// Precedence (3.3.2): per path the newest in time: D, the chunks before the
// walk, its run 1, its run 2, the chunks after it.
// ---------------------------------------------------------------------------
void test_merge_precedence() {
  CutFs fs;
  ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
  st.open();
  State m;
  TEST_ASSERT_TRUE(walk(st, m, ts::Identity(), 0,
                        {wFile("a/1.mp3", 10, kT), wFile("a/2.mp3", 20, kT), wFile("a/3.mp3", 30, kT),
                         wFile("a/4.mp3", 40, kT), wFile("a/5.mp3", 50, kT)}));
  TEST_ASSERT_TRUE(st.compact().ok);
  compacted(m, false);
  // Chunks before the walk.
  TEST_ASSERT_TRUE(append(st, m, {scanned("a/1.mp3", 10, kT, 1), scanned("a/2.mp3", 20, kT, 2)}));
  // The walk: 1 changed on a PC (Pending again), 2 gone, 3 doubtful (its row
  // without T first, as the walk gives it) then confirmed in run 2, 6 new, 7
  // doubtful and left so (T's qfp didn't match), a cover's doubt kept only
  // for the skew's recount.
  TEST_ASSERT_TRUE(walk(st, m, commit(2), 0,
                        {wFile("a/1.mp3", 11, kT), wGone("a/2.mp3"), wFile("a/3.mp3", 30, kT + 2),
                         wDoubt("a/3.mp3", 30, kT + 2, 4), wFile("a/6.mp3", 60, kT), wFile("a/7.mp3", 70, kT + 2),
                         wDoubt("a/7.mp3", 70, kT + 2, 4), wDoubt("a/cover.jpg", 900, kT + 2, 4, false)},
                        {wFile("a/3.mp3", 30, kT + 2, Status::Software, true, 0x99),
                         wFile("a/7.mp3", 70, kT + 2, Status::Pending, false, 0x77)}));
  // Chunks after it: 1 scanned at its new size; 4 scanned.
  TEST_ASSERT_TRUE(append(st, m, {scanned("a/1.mp3", 11, kT, 7), scanned("a/4.mp3", 40, kT, 8)}));
  assertView(st, m);
  const auto v = view(st);
  TEST_ASSERT_EQUAL_size_t(6, v.size());  // 1, 3, 4, 5, 6, 7
  TEST_ASSERT_EQUAL_STRING("a/1.mp3", v[0].first.c_str());
  TEST_ASSERT_TRUE(v[0].second.row.status == Status::Scanned);
  TEST_ASSERT_EQUAL_UINT32(11, v[0].second.rec.size);
  TEST_ASSERT_EQUAL_STRING("Title 7", v[0].second.fields[cc::kTitle].c_str());
  TEST_ASSERT_TRUE(v[1].second.row.status == Status::Software && v[1].second.row.confirmed);
  TEST_ASSERT_EQUAL_UINT64(0x99, v[1].second.rec.qfp);
  TEST_ASSERT_TRUE(v[5].second.row.status == Status::Pending);  // 7: settled without T, its qfp kept
  TEST_ASSERT_EQUAL_UINT64(0x77, v[5].second.rec.qfp);
  // The walk's runs read back.
  std::vector<uint8_t> buf(256);
  ts::WalkReader r;
  TEST_ASSERT_TRUE(st.readWalk(1, buf.data(), 256, &r));
  ts::WalkEntry e;
  uint32_t doubts = 0, settle = 0, n = 0;
  while (r.next(&e)) {
    ++n;
    if (e.kind == ts::EntryKind::Doubt) {
      ++doubts;
      settle += e.doubt.settle ? 1 : 0;
      TEST_ASSERT_TRUE(e.doubt.hasDelta);
      TEST_ASSERT_EQUAL_INT64(4, e.doubt.delta);
      TEST_ASSERT_EQUAL_UINT32(kT + 2, e.doubt.fatTime);
      TEST_ASSERT_EQUAL_UINT64(0x1234567890ull + e.doubt.size, e.doubt.transferQfp);
      TEST_ASSERT_TRUE(e.doubt.fallback == Status::Pending);
    }
  }
  TEST_ASSERT_FALSE(r.failed());
  TEST_ASSERT_EQUAL_UINT32(8, n);
  TEST_ASSERT_EQUAL_UINT32(3, doubts);
  TEST_ASSERT_EQUAL_UINT32(2, settle);
  TEST_ASSERT_TRUE(st.readWalk(2, buf.data(), 256, &r));
  TEST_ASSERT_TRUE(r.next(&e));
  TEST_ASSERT_TRUE(e.kind == ts::EntryKind::File && e.row.confirmed);
  TEST_ASSERT_EQUAL_UINT64(0x99, e.qfp);
  TEST_ASSERT_TRUE(r.next(&e));
  TEST_ASSERT_FALSE(r.next(&e));
  r.close();
  // Into D: the same rows.
  ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  compacted(m, false);
  assertDevice(fs, st, m);
  assertView(st, m);
  // A second walk needs the first merged; a walk with nothing new writes nothing.
  std::vector<uint8_t> wb(1024);
  ts::WalkWriter w;
  TEST_ASSERT_TRUE(st.beginWalk(commit(2), wb.data(), 1024, &w));
  TEST_ASSERT_TRUE(w.endRun(0));
  TEST_ASSERT_TRUE(w.endRun(0));
  TEST_ASSERT_TRUE(w.finish());
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
  TEST_ASSERT_FALSE(st.hasWalk());
  // Another skew is something new.
  TEST_ASSERT_TRUE(st.beginWalk(commit(2), wb.data(), 1024, &w));
  TEST_ASSERT_TRUE(w.endRun(-3600));
  TEST_ASSERT_TRUE(w.endRun(-3600));
  TEST_ASSERT_TRUE(w.finish());
  TEST_ASSERT_TRUE(st.hasWalk());
  TEST_ASSERT_FALSE(st.beginWalk(commit(2), wb.data(), 1024, &w));  // the last walk is still to merge
  TEST_ASSERT_TRUE(st.compact().ok);
  m.header.skew = -3600;
  TEST_ASSERT_EQUAL_INT32(-3600, st.device().header.skew);
  // Out of order: the run is refused, nothing of it counts.
  TEST_ASSERT_TRUE(st.beginWalk(commit(3), wb.data(), 1024, &w));
  TEST_ASSERT_TRUE(w.file("b/2.mp3", 7, 1, 1, 0, Status::Pending));
  TEST_ASSERT_FALSE(w.file("b/1.mp3", 7, 1, 1, 0, Status::Pending));
  TEST_ASSERT_FALSE(w.endRun(0));
  w.finish();
  TEST_ASSERT_FALSE(st.hasWalk());
  // A path that can't be is refused (the walk goes on); a folder entry may
  // be /music itself; each kind keeps its own order (a folder's row after its
  // subfolder's files, as the walk gives a folder's row once audio turns up
  // below it).
  TEST_ASSERT_TRUE(st.beginWalk(commit(3), wb.data(), 1024, &w));
  TEST_ASSERT_FALSE(w.file("c/../x.mp3", 10, 1, 1, 0, Status::Pending));
  TEST_ASSERT_TRUE(w.file("n/s/1.mp3", 9, 1, kT, 0, Status::Pending));
  TEST_ASSERT_TRUE(w.folder("", 0, facts(9)));
  TEST_ASSERT_TRUE(w.folder("n", 1, facts(10, "folder.jpg")));
  TEST_ASSERT_TRUE(w.folder("n/s", 3, facts(11)));
  TEST_ASSERT_TRUE(w.file("n/t/1.mp3", 9, 2, kT, 0, Status::Pending));
  TEST_ASSERT_TRUE(w.doubt("n/s/1.mp3", 9, ts::Doubt()));
  TEST_ASSERT_FALSE(w.gone("", 0));
  TEST_ASSERT_TRUE(w.endRun(0) && w.endRun(0) && w.finish());
  applyWalk(m, {wFile("n/s/1.mp3", 1, kT), wFile("n/t/1.mp3", 2, kT), wFolder("", facts(9)),
                wFolder("n", facts(10, "folder.jpg")), wFolder("n/s", facts(11))});
  m.walkPending = true;
  m.walk.walked = true;
  m.walk.walk = commit(3);
  m.walk.skew = 0;
  assertView(st, m);
  TEST_ASSERT_TRUE(st.compact().ok);
  compacted(m, false);
  assertDevice(fs, st, m);
  // A failed walk counts for nothing, its runs written or not.
  TEST_ASSERT_TRUE(st.beginWalk(commit(4), wb.data(), 1024, &w));
  for (int i = 0; i < 60; ++i) {
    char p[32];
    std::snprintf(p, sizeof(p), "z/%02d.mp3", i);
    TEST_ASSERT_TRUE(w.file(p, std::strlen(p), 1, kT, 0, Status::Pending));
  }
  TEST_ASSERT_TRUE(w.endRun(0));
  TEST_ASSERT_TRUE(st.hasWalk());
  w.abort();
  TEST_ASSERT_FALSE(st.hasWalk());
  TEST_ASSERT_FALSE(fs.exists(st.walkPath()));
  assertView(st, m);
}

// ---------------------------------------------------------------------------
// The journal: a torn last chunk ends it and is cut off at the next append;
// a journal of another D is dropped; a path added twice keeps its last.
// ---------------------------------------------------------------------------
void test_journal_tails_and_bases() {
  CutFs fs;
  State m;
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    st.open();
    TEST_ASSERT_TRUE(walk(st, m, ts::Identity(), 0, {wFile("x/1.mp3", 1, kT), wFile("x/2.mp3", 2, kT)}));
    TEST_ASSERT_TRUE(st.compact().ok);
    compacted(m, false);
    TEST_ASSERT_TRUE(append(st, m, {scanned("x/1.mp3", 1, kT, 1)}));
    TEST_ASSERT_TRUE(append(st, m, {scanned("x/2.mp3", 2, kT, 2), scanned("x/2.mp3", 2, kT, 3)}));
    assertView(st, m);
    TEST_ASSERT_EQUAL_STRING("Title 3", m.files["x/2.mp3"].fields[cc::kTitle].c_str());
  }
  // Tear the last chunk: its CRC fails, the journal ends before it.
  std::vector<uint8_t> j = fs.bytes("/.player/tags.jnl");
  const size_t whole = j.size();
  j.resize(whole - 5);
  fs.put("/.player/tags.jnl", j);
  State before = m;
  before.files["x/2.mp3"] = MRow();
  before.files["x/2.mp3"].rec = bare(2, kT);
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    const ts::TagStore::Opened o = st.open();
    TEST_ASSERT_EQUAL_UINT32(1, o.chunks);
    TEST_ASSERT_TRUE(o.journalTorn);
    assertView(st, before);
    // The next append cuts the torn tail off first, and takes its sequence.
    State again = before;
    TEST_ASSERT_TRUE(append(st, again, {scanned("x/2.mp3", 2, kT, 3)}));
    TEST_ASSERT_EQUAL_UINT32(2, st.journalSeq());
    assertView(st, again);
  }
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    const ts::TagStore::Opened o = st.open();
    TEST_ASSERT_EQUAL_UINT32(2, o.chunks);
    TEST_ASSERT_FALSE(o.journalTorn);
    assertView(st, m);
    // A flipped byte inside the first chunk: the journal ends before it.
    std::vector<uint8_t> k = fs.bytes("/.player/tags.jnl");
    k[40] ^= 1;
    fs.put("/.player/tags.jnl", k);
  }
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    const ts::TagStore::Opened o = st.open();
    TEST_ASSERT_EQUAL_UINT32(0, o.chunks);
    TEST_ASSERT_TRUE(o.journalTorn);
    State none = m;
    for (auto& kv : none.files) {
      kv.second = MRow();
      kv.second.rec = bare(kv.first == "x/1.mp3" ? 1 : 2, kT);
    }
    assertView(st, none);
    // A fresh journal replaces it whole.
    State again = none;
    TEST_ASSERT_TRUE(append(st, again, {scanned("x/1.mp3", 1, kT, 9)}));
    TEST_ASSERT_EQUAL_UINT32(1, st.journalSeq());
    assertView(st, again);
    // A compaction moves D on: the old journal's chunks, if left, are another D's.
    const std::vector<uint8_t> stale = fs.bytes("/.player/tags.jnl");
    TEST_ASSERT_TRUE(st.compact().ok);
    fs.put("/.player/tags.jnl", stale);
    ts::TagStore st2(fs, config(), Heap::alloc, Heap::release);
    const ts::TagStore::Opened o2 = st2.open();
    TEST_ASSERT_TRUE(o2.journalStale);
    TEST_ASSERT_EQUAL_UINT32(0, o2.chunks);
    assertView(st2, again);
  }
  // A walk.jnl cut short (no End) or of another D is dropped.
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    st.open();
    State s;
    std::vector<uint8_t> buf(1024);
    ts::WalkWriter w;
    TEST_ASSERT_TRUE(st.beginWalk(commit(4), buf.data(), 1024, &w));
    for (int i = 0; i < 60; ++i) {  // more than a block: some reach the card
      char p[32];
      std::snprintf(p, sizeof(p), "x/3_%02d.mp3", i);
      TEST_ASSERT_TRUE(w.file(p, std::strlen(p), 3, kT, 0, Status::Pending));
    }
    TEST_ASSERT_TRUE(fs.exists(st.walkPath()));
    TEST_ASSERT_TRUE(w.finish());  // run 1 never ended
    ts::TagStore st2(fs, config(), Heap::alloc, Heap::release);
    const ts::TagStore::Opened o = st2.open();
    TEST_ASSERT_TRUE(o.walkStale);
    TEST_ASSERT_FALSE(st2.hasWalk());
  }
}

// ---------------------------------------------------------------------------
// A D whose frame holds but whose records don't (a flipped bit in RECS,
// known only at the first pass's end): left out (3.4.1). The journals alone
// make the new D, unwalked, so the next walk compares the whole card.
// ---------------------------------------------------------------------------
void test_a_bad_device_is_left_out() {
  CutFs fs;
  State m;
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    st.open();
    TEST_ASSERT_TRUE(walk(st, m, commit(1), 0,
                          {wFile("q/1.mp3", 1, kT), wFile("q/2.mp3", 2, kT), wFolder("q", facts(3, "cover.jpg"))}));
    TEST_ASSERT_TRUE(st.compact().ok);
    compacted(m, false);
    TEST_ASSERT_TRUE(append(st, m, {scanned("q/2.mp3", 2, kT, 5)}));
  }
  std::vector<uint8_t> d = fs.bytes("/.player/tags.bin");
  cc::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  cc::Container c;
  mptg::Info info;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::openFile(c, src, &info)));
  d[c.section(mptg::kSecRecs).offset + 13] ^= 0x10;  // record 0's size
  fs.put("/.player/tags.bin", d);
  ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(st.open().deviceWhy));  // the frame holds
  const ts::TagStore::Compacted r = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(r.ok, r.error);
  TEST_ASSERT_EQUAL_STRING("sectionCrc", cc::whyName(r.deviceWhy));
  State only;
  only.files["q/2.mp3"] = m.files["q/2.mp3"];
  assertView(st, only);
  assertDevice(fs, st, only);
  TEST_ASSERT_FALSE(st.device().header.walked);
}

// ---------------------------------------------------------------------------
// Rescan (3.3.6) and a new parser: the device's own readings turn Pending.
// ---------------------------------------------------------------------------
void test_rescan_and_new_parser() {
  CutFs fs;
  State m;
  {
    ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
    st.open();
    TEST_ASSERT_TRUE(walk(st, m, ts::Identity(), 0,
                          {wFile("r/1.mp3", 1, kT), wFile("r/2.mp3", 2, kT),
                           wFile("r/3.mp3", 3, kT, Status::Software)}));
    TEST_ASSERT_TRUE(st.compact().ok);
    compacted(m, false);
    TEST_ASSERT_TRUE(append(st, m, {scanned("r/1.mp3", 1, kT, 1), unreadable("r/2.mp3", 2, kT)}));
    TEST_ASSERT_TRUE(st.compact().ok);
    compacted(m, false);
    // A chunk read before the Rescan is of the old epoch.
    TEST_ASSERT_TRUE(append(st, m, {scanned("r/1.mp3", 1, kT, 2)}));
    ts::TagStore::Compacted c = st.compact(true);
    TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
    TEST_ASSERT_TRUE(c.rescanned);
    compacted(m, true);
    TEST_ASSERT_EQUAL_UINT32(1, st.device().header.epoch);
    assertDevice(fs, st, m);
    for (const auto& kv : m.files) TEST_ASSERT_TRUE(kv.second.row.status != Status::Scanned);
    // Read again at the new epoch: kept.
    TEST_ASSERT_TRUE(append(st, m, {scanned("r/1.mp3", 1, kT, 3)}));
    TEST_ASSERT_TRUE(st.compact().ok);
    compacted(m, false);
    assertDevice(fs, st, m);
  }
  // A firmware with another parser: D asks for a compaction, which turns
  // the old readings Pending.
  ts::TagStore::Config c4 = config();
  c4.parserVersion = 4;
  ts::TagStore st(fs, c4, Heap::alloc, Heap::release);
  st.open();
  TEST_ASSERT_TRUE(st.wantsCompaction());
  const ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE(c.ok && c.rescanned);
  compacted(m, false, 4);
  assertDevice(fs, st, m);
  TEST_ASSERT_FALSE(st.wantsCompaction());
}

// ---------------------------------------------------------------------------
// The cut-rename rule (2.12.6).
// ---------------------------------------------------------------------------
namespace {

class Whole : public ts::TmpCheck {
public:
  explicit Whole(bool w) : w_(w) {}
  bool whole(ts::Fs&, const char*) override { return w_; }

private:
  bool w_;
};

}  // namespace

void test_cut_rename_rule() {
  const ts::Names n{"/p/x.idx", "/p/x.tmp", "/p/x"};
  char name[64];
  TEST_ASSERT_TRUE(ts::twinName(n, 1, name, sizeof(name)));
  TEST_ASSERT_EQUAL_STRING("/p/x.xl1", name);
  TEST_ASSERT_FALSE(ts::twinName(n, 10, name, sizeof(name)));
  Whole yes(true), no(false);
  {
    // A tmp alone and whole: X's last write, cut between the remove and the rename.
    CutFs fs;
    fs.put("/p/x.tmp", {1, 2, 3});
    const ts::Settled s = ts::settle(fs, n, &yes);
    TEST_ASSERT_TRUE(s.what == ts::Settle::Promoted);
    TEST_ASSERT_TRUE(fs.exists("/p/x.idx") && !fs.exists("/p/x.tmp"));
  }
  {
    // A tmp alone and torn: removed.
    CutFs fs;
    fs.put("/p/x.tmp", {1, 2, 3});
    TEST_ASSERT_TRUE(ts::settle(fs, n, &no).what == ts::Settle::Removed);
    TEST_ASSERT_TRUE(fs.names().empty());
  }
  {
    // Both, on different chains: the tmp is a leftover.
    CutFs fs;
    fs.put("/p/x.idx", {1});
    fs.put("/p/x.tmp", {2});
    TEST_ASSERT_TRUE(ts::settle(fs, n, &yes).what == ts::Settle::Removed);
    TEST_ASSERT_EQUAL_size_t(1, fs.names().size());
  }
  {
    // Both on one chain: a rename cut between its two directory writes.
    // The tmp becomes a twin; the next replacement moves X to another twin
    // instead of freeing the chain; both stay until a disk check.
    CutFs fs;
    fs.put("/p/x.tmp", {7, 7, 7});
    fs.link("/p/x.tmp", "/p/x.idx");
    ts::Settled s = ts::settle(fs, n, &yes);
    TEST_ASSERT_TRUE(s.what == ts::Settle::Quarantined);
    TEST_ASSERT_EQUAL_UINT32(1, s.twins);
    TEST_ASSERT_TRUE(fs.shared("/p/x.idx", "/p/x.xl1"));
    // A new tmp, then the replacement.
    TEST_ASSERT_TRUE(ts::prepareTmp(fs, n));
    ts::File* f = fs.open("/p/x.tmp", ts::Fs::Mode::Create);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(f->write(0, "new", 3) && f->sync());
    fs.close(f);
    TEST_ASSERT_TRUE(ts::replace(fs, n));
    TEST_ASSERT_TRUE(fs.shared("/p/x.xl1", "/p/x.xl2"));
    TEST_ASSERT_EQUAL_UINT32(2, ts::collectTwins(fs, n));
    // The next replacement: X shares nothing now, so it goes.
    TEST_ASSERT_TRUE(ts::prepareTmp(fs, n));
    f = fs.open("/p/x.tmp", ts::Fs::Mode::Create);
    TEST_ASSERT_TRUE(f->write(0, "newer", 5) && f->sync());
    fs.close(f);
    TEST_ASSERT_TRUE(ts::replace(fs, n));
    TEST_ASSERT_EQUAL_UINT32(2, ts::collectTwins(fs, n));
    TEST_ASSERT_TRUE(fs.violations.empty());
    // chkdsk copies the chains apart: the twins go.
    fs.diskCheck();
    TEST_ASSERT_EQUAL_UINT32(0, ts::collectTwins(fs, n));
    TEST_ASSERT_EQUAL_size_t(1, fs.names().size());
    TEST_ASSERT_TRUE(fs.bytes("/p/x.idx") == std::vector<uint8_t>({'n', 'e', 'w', 'e', 'r'}));
  }
  {
    // A tmp that shares a twin's chain (a quarantine whose own rename was
    // cut): renamed to the next twin, never removed.
    CutFs fs;
    fs.put("/p/x.idx", {1});
    fs.put("/p/x.xl1", {9, 9});
    fs.link("/p/x.xl1", "/p/x.tmp");
    TEST_ASSERT_TRUE(ts::settle(fs, n, &yes).what == ts::Settle::Quarantined);
    TEST_ASSERT_TRUE(fs.shared("/p/x.xl1", "/p/x.xl2"));
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
  {
    // Every twin name taken: X can't be replaced until a disk check.
    CutFs fs;
    fs.put("/p/x.idx", {5});
    for (int i = 1; i <= ts::kMaxTwins; ++i) {
      ts::twinName(n, i, name, sizeof(name));
      fs.link("/p/x.idx", name);
    }
    fs.put("/p/x.tmp", {6});
    TEST_ASSERT_FALSE(ts::replace(fs, n));
    TEST_ASSERT_TRUE(fs.exists("/p/x.idx"));
    TEST_ASSERT_TRUE(fs.violations.empty());
  }
  {
    // An empty file has no chain to share: its twin can go.
    CutFs fs;
    fs.put("/p/x.idx", {});
    fs.link("/p/x.idx", "/p/x.xl1");
    TEST_ASSERT_EQUAL_UINT32(0, ts::collectTwins(fs, n));
  }
}

// ---------------------------------------------------------------------------
// A power cut at every step of a session.
// ---------------------------------------------------------------------------
namespace {

struct Ctx {
  CutFs* fs;
  ts::TagStore* st;
  State* m;
};
using StepFn = std::function<bool(Ctx&)>;

bool doCompact(Ctx& c, bool rescan) {
  const ts::TagStore::Compacted r = c.st->compact(rescan);
  if (!r.ok) return false;
  compacted(*c.m, rescan);
  return true;
}

// The session: a first walk into an empty card, scans, a walk with
// changes (doubtful files resolved in run 2) between scans, compactions, a
// Rescan. Small buffers, so each section takes many writes.
std::vector<StepFn> session() {
  std::vector<StepFn> s;
  s.push_back([](Ctx& c) {
    return walk(*c.st, *c.m, commit(1), 3600,
                {wFile("Artist/Album/01 - One.mp3", 3001, kT), wFile("Artist/Album/02 - Two.mp3", 3002, kT),
                 wFile("Artist/Album/03 - Three.mp3", 3003, kT, Status::Software),
                 wFile("Artist/Album B/01.flac", 4001, kT), wFile("Artist/Single.mp3", 5001, kT),
                 wFile("Other/X/a.opus", 6001, kT), wFile("Other/X/b.opus", 6002, kT), wFile("top.mp3", 7001, kT),
                 wFolder("", facts(10)), wFolder("Artist/Album", facts(11, "cover.jpg")),
                 wFolder("Other/X", facts(12, "folder.jpg", true))});
  });
  s.push_back([](Ctx& c) { return doCompact(c, false); });
  s.push_back([](Ctx& c) {
    return append(*c.st, *c.m,
                  {scanned("Artist/Album/01 - One.mp3", 3001, kT, 1), scanned("Artist/Album/02 - Two.mp3", 3002, kT, 2),
                   unreadable("Artist/Single.mp3", 5001, kT)});
  });
  s.push_back([](Ctx& c) {
    return append(*c.st, *c.m,
                  {scanned("Other/X/a.opus", 6001, kT, 3), scanned("Artist/Album/01 - One.mp3", 3001, kT, 4)});
  });
  s.push_back([](Ctx& c) {
    return walk(*c.st, *c.m, commit(2), 3600,
                {wFile("Artist/Album/02 - Two.mp3", 3999, kT + 2), wGone("Other/X/b.opus"),
                 wFile("Artist/Album/03 - Three.mp3", 3003, kT + 4),
                 wDoubt("Artist/Album/03 - Three.mp3", 3003, kT + 4, 2), wFile("New/n.mp3", 8001, kT),
                 wFolder("Artist/Album", facts(21, "cover.jpg")), wFolder("New", facts(22)),
                 wGone("Artist/Single.mp3"), wFolderGone("Artist/Gone")},
                {wFile("Artist/Album/03 - Three.mp3", 3003, kT + 4, Status::Software, true, 0xABCD)});
  });
  s.push_back([](Ctx& c) {
    return append(*c.st, *c.m,
                  {scanned("Artist/Album/02 - Two.mp3", 3999, kT + 2, 5), scanned("New/n.mp3", 8001, kT, 6)});
  });
  s.push_back([](Ctx& c) { return doCompact(c, false); });
  s.push_back([](Ctx& c) { return append(*c.st, *c.m, {scanned("top.mp3", 7001, kT, 7)}); });
  // A walk at the same commit: a file D has Pending that an older chunk read
  // (the reading stands); a scanned file doubtful against T, its row
  // without T the reading, settled the same (T's qfp didn't match); a file
  // T covers now.
  s.push_back([](Ctx& c) {
    return walk(*c.st, *c.m, commit(2), 3600,
                {wFile("top.mp3", 7001, kT),
                 wFile("Artist/Album/01 - One.mp3", 3001, kT, Status::Scanned, false, 0x5151),
                 wDoubt("Artist/Album/01 - One.mp3", 3001, kT, 2),
                 wFile("Artist/Album B/01.flac", 4001, kT, Status::Software)},
                {wFile("Artist/Album/01 - One.mp3", 3001, kT, Status::Scanned, false, 0x5151)});
  });
  s.push_back([](Ctx& c) { return doCompact(c, true); });
  return s;
}

ts::TagStore::Config smallConfig() {
  ts::TagStore::Config c = config();
  c.runBuffer = 64;
  c.writeBuffer = 64;
  c.deviceBuffer = ts::DeviceReader::kMinScratch;
  return c;
}

struct Run {
  std::vector<State> states;   // after each step (states[0]: before any)
  std::vector<long> boundary;  // the fake's step count after each
  std::vector<std::vector<State>> mid;  // a step's states between before and after (a walk's run 1 alone)
};

Run uncut() {
  Run r;
  CutFs fs;
  ts::TagStore st(fs, smallConfig(), Heap::alloc, Heap::release);
  st.open();
  State m;
  r.states.push_back(m);
  r.boundary.push_back(fs.steps);
  r.mid.emplace_back();
  Ctx c{&fs, &st, &m};
  for (auto& step : session()) {
    State mid;
    mid.header.epoch = 0xFFFFFFFFu;  // a mark: walk() overwrites it when its run 2 has entries
    gMid = &mid;
    TEST_ASSERT_TRUE(step(c));
    gMid = nullptr;
    r.states.push_back(m);
    r.boundary.push_back(fs.steps);
    r.mid.emplace_back();
    if (mid.header.epoch != 0xFFFFFFFFu) r.mid.back().push_back(mid);
  }
  TEST_ASSERT_TRUE(fs.violations.empty());
  return r;
}

// After a cut in step `i` and a reboot: the card holds the state before the
// step or after it, readable whole; then the rest of the session reaches
// the end state.
void recoverAndFinish(CutFs& card, const Run& r, size_t i, const char* label) {
  ts::TagStore st(card, smallConfig(), Heap::alloc, Heap::release);
  st.open();
  TEST_ASSERT_TRUE_MESSAGE(card.violations.empty(), label);
  const State& before = r.states[i - 1];
  const State& after = r.states[i];
  std::string w1, w2;
  bool isBefore = viewIs(st, before, &w1);
  const bool isAfter = !isBefore && viewIs(st, after, &w2);
  const State* mid = nullptr;
  if (!isBefore && !isAfter)
    for (const State& x : r.mid[i])
      if (viewIs(st, x)) mid = &x;
  if (!isBefore && !isAfter && !mid) {
    std::string msg = std::string(label) + ": neither before (" + w1 + ") nor after (" + w2 + ")";
    TEST_FAIL_MESSAGE(msg.c_str());
  }
  State m = isBefore ? before : (isAfter ? after : *mid);
  if (mid) isBefore = true;  // step i is done again from there
  // The journals into D: the same rows, D whole.
  if (m.walkPending && !st.hasWalk()) {
    // The cut compaction merged the walk (its walk.jnl left behind, stale).
    // Its run 1 alone with a doubt to settle leaves the same rows when run 2
    // changed none, and D unwalked.
    if (m.walkSettles && !st.device().header.walked) {
      m.header.walked = false;
    } else {
      m.header.walked = true;
      m.header.walk = m.walk.walk;
      m.header.skew = m.walk.skew;
    }
    m.walkPending = false;
  }
  if (m.walkUnsettled && !st.hasWalk()) {
    m.header.walked = false;  // ... its run 1 alone: D unwalked
    m.walkUnsettled = false;
  }
  if (m.walkPending && st.walkUnsettled()) {
    // Run 1 alone is on the card, its rows the whole walk's (run 2 changed
    // none): merged, it leaves D unwalked.
    m.walkPending = false;
    m.walkUnsettled = true;
  }
  if (st.hasJournals() || !st.device().present) {
    ts::TagStore::Compacted c = st.compact();
    TEST_ASSERT_TRUE_MESSAGE(c.ok, label);
    compacted(m, false);
    TEST_ASSERT_TRUE_MESSAGE(viewIs(st, m), label);
  }
  TEST_ASSERT_TRUE_MESSAGE(card.violations.empty(), label);
  // Then the session goes on from where it was.
  Ctx c{&card, &st, &m};
  const auto steps = session();
  for (size_t k = isBefore ? i - 1 : i; k < steps.size(); ++k) TEST_ASSERT_TRUE_MESSAGE(steps[k](c), label);
  std::string why;
  TEST_ASSERT_TRUE_MESSAGE(viewIs(st, r.states.back(), &why), (std::string(label) + ": " + why).c_str());
  if (!st.hasJournals()) assertDevice(card, st, m);
  TEST_ASSERT_TRUE_MESSAGE(card.violations.empty(), label);
}

}  // namespace

void test_power_cut_at_every_step() {
  const Run r = uncut();
  const long total = r.boundary.back();
  printf("[tagstore] the session: %ld steps (writes, syncs, removes, renames' directory writes)\n", total);
  TEST_ASSERT_TRUE(total > 100);
  uint32_t runs = 0, twinsSeen = 0;
  for (long k = 1; k <= total; ++k) {
    // The step of the session the cut falls in.
    size_t i = 1;
    while (r.boundary[i] < k) ++i;
    CutFs fs;
    fs.cutAt = k;
    {
      ts::TagStore st(fs, smallConfig(), Heap::alloc, Heap::release);
      st.open();
      State m;
      Ctx c{&fs, &st, &m};
      const auto steps = session();
      for (size_t s = 0; s < steps.size() && !fs.dead; ++s) steps[s](c);
      TEST_ASSERT_TRUE(fs.dead);
      TEST_ASSERT_TRUE(fs.violations.empty());
    }
    TEST_ASSERT_EQUAL_UINT32(0, fs.openNow);  // every file closed, the one whose write the cut failed too
    for (Variant v : {Variant::InOrder, Variant::LoseUnsynced, Variant::Torn}) {
      CutFs card = fs.reboot(v);
      char label[96];
      std::snprintf(label, sizeof(label), "cut at step %ld (session step %u), variant %d", k, static_cast<unsigned>(i),
                    static_cast<int>(v));
      // What open() finds before it settles anything.
      if (card.shared("/.player/tags.tmp", "/.player/tags.bin")) ++twinsSeen;
      recoverAndFinish(card, r, i, label);
      ++runs;
    }
  }
  printf("[tagstore] %u recoveries checked; %u cuts left tags.bin and tags.tmp on one chain\n", runs, twinsSeen);
  TEST_ASSERT_TRUE_MESSAGE(twinsSeen > 0, "no cut fell inside a rename");
}

// The recovery itself cut: a second power cut at every step of the boot
// after a cut in a compaction's last steps (the renames and removes), then a
// third boot.
void test_power_cut_during_recovery() {
  const Run r = uncut();
  uint32_t runs = 0;
  // Each compaction's tail: from its tags.tmp's sync to its end.
  for (size_t i = 1; i < r.boundary.size(); ++i) {
    const long end = r.boundary[i];
    const long from = std::max(r.boundary[i - 1] + 1, end - 8);
    for (long k = from; k <= end; ++k) {
      CutFs fs;
      fs.cutAt = k;
      {
        ts::TagStore st(fs, smallConfig(), Heap::alloc, Heap::release);
        st.open();
        State m;
        Ctx c{&fs, &st, &m};
        const auto steps = session();
        for (size_t s = 0; s < steps.size() && !fs.dead; ++s) steps[s](c);
      }
      for (Variant v : {Variant::InOrder, Variant::LoseUnsynced}) {
        const CutFs first = fs.reboot(v);
        // The boot's own steps, counted.
        long bootSteps;
        {
          CutFs probe = first;
          ts::TagStore st(probe, smallConfig(), Heap::alloc, Heap::release);
          st.open();
          if (st.hasJournals() || !st.device().present) st.compact();
          bootSteps = probe.steps;
        }
        for (long j = 1; j <= bootSteps; ++j) {
          if (j > 12 && j < bootSteps - 12) continue;  // the settle and the renames, not the middle of the writes
          CutFs second = first;
          second.cutAt = j;
          {
            ts::TagStore st(second, smallConfig(), Heap::alloc, Heap::release);
            st.open();
            if (!second.dead && (st.hasJournals() || !st.device().present)) st.compact();
          }
          TEST_ASSERT_TRUE(second.violations.empty());
          CutFs third = second.reboot(v);
          char label[96];
          std::snprintf(label, sizeof(label), "cut at %ld, then at boot step %ld, variant %d", k, j,
                        static_cast<int>(v));
          recoverAndFinish(third, r, i, label);
          ++runs;
        }
      }
    }
  }
  printf("[tagstore] %u twice-cut recoveries checked\n", runs);
}

// ---------------------------------------------------------------------------
// The compaction's memory: fixed by its config, whatever the journal holds.
// ---------------------------------------------------------------------------
void test_compaction_memory_is_bounded() {
  // A record as big as a record gets: every field at its limit.
  auto big = [](const std::string& path, uint32_t seed) {
    Scan s = scanned(path, 100 + seed, kT, seed);
    for (uint32_t f = 0; f < cc::kRunFields; ++f) {
      const size_t max = cc::fieldMax(f);
      s.fields[f] = std::string(max, static_cast<char>('a' + f));
      if (cc::isListField(f)) {
        // 4 values of 254 bytes and their separators: 1,019 bytes.
        std::string v;
        for (int k = 0; k < 4; ++k) {
          if (k) v += '\x1F';
          v += std::string(254, static_cast<char>('a' + (f + k) % 26));
        }
        s.fields[f] = v;
      }
    }
    return s;
  };
  const ts::TagStore::Config cfg = config();
  size_t peaks[2] = {};
  for (int round = 0; round < 2; ++round) {
    CutFs fs;
    State m;
    {
      // The second round's journal holds 3 x maxChunks chunks, written by a
      // policy that isn't this firmware's.
      ts::TagStore::Config w = cfg;
      w.maxChunks = round ? 3 * cfg.maxChunks : cfg.maxChunks;
      ts::TagStore st(fs, w, Heap::alloc, Heap::release);
      st.open();
      std::vector<WOp> files;
      for (uint32_t i = 0; i < 300; ++i)
        files.push_back(wFile("d" + std::to_string(i % 17) + "/f" + std::to_string(i) + ".mp3", 100 + i, kT));
      TEST_ASSERT_TRUE(walk(st, m, ts::Identity(), 0, files));
      TEST_ASSERT_TRUE(st.compact().ok);
      compacted(m, false);
      for (uint32_t k = 0; k < w.maxChunks; ++k) {
        std::vector<Scan> scans;
        for (uint32_t i = 0; i < 4; ++i) {
          const uint32_t n = (k * 4 + i) % 300;
          scans.push_back(big("d" + std::to_string(n % 17) + "/f" + std::to_string(n) + ".mp3", n));
        }
        TEST_ASSERT_TRUE(append(st, m, scans));
      }
    }
    Heap::reset();
    ts::TagStore st(fs, cfg, Heap::alloc, Heap::release);
    const ts::TagStore::Opened o = st.open();
    TEST_ASSERT_EQUAL_UINT32(cfg.maxChunks, o.chunks);
    TEST_ASSERT_TRUE(st.wantsCompaction());
    if (round) {
      // Past maxChunks: no more appends, and the rest is dropped.
      ts::ChunkBuilder c;
      c.begin(chunkBuffer().data(), 4096);
      const char* f[cc::kRunFields] = {};
      TEST_ASSERT_TRUE(c.add("d0/f0.mp3", 9, Status::Scanned, bare(100, kT), f));
      TEST_ASSERT_FALSE(st.append(c));
    }
    const size_t before = Heap::live;
    const ts::TagStore::Compacted c = st.compact();
    TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
    TEST_ASSERT_EQUAL_UINT32(cfg.maxChunks, c.chunksMerged);
    TEST_ASSERT_EQUAL_UINT32(round ? 2 * cfg.maxChunks : 0, c.chunksDropped);
    peaks[round] = Heap::peak - before;
    TEST_ASSERT_TRUE(peaks[round] <= st.workBytes());
    TEST_ASSERT_EQUAL_size_t(before, Heap::live);  // all given back
    printf("[tagstore] compaction of %u chunks of full records (%u more dropped): work %u bytes (workBytes() %u)\n",
           c.chunksMerged, c.chunksDropped, static_cast<unsigned>(peaks[round]), static_cast<unsigned>(st.workBytes()));
  }
  TEST_ASSERT_EQUAL_size_t(peaks[0], peaks[1]);
  // About 115 KB at the firmware's defaults since 2026-10-09 (3.5; 1 KB run
  // buffers, an 8 KB D, 4 KB section buffers: whole-sector card commands,
  // test_card_io), 66 KB before.
  CutFs fs;
  ts::TagStore::Config d = config();
  const ts::TagStore::Config firmware;
  d.runBuffer = firmware.runBuffer;
  d.deviceBuffer = firmware.deviceBuffer;
  d.writeBuffer = firmware.writeBuffer;
  ts::TagStore st(fs, d);
  printf("[tagstore] the firmware's compaction memory: %u bytes\n", static_cast<unsigned>(st.workBytes()));
  TEST_ASSERT_TRUE(st.workBytes() <= 120u * 1024u);
}

// ---------------------------------------------------------------------------
// N2's builder fed from D through the adapters.
// ---------------------------------------------------------------------------
void test_builder_adapters() {
  CutFs fs;
  ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
  st.open();
  State m;
  TEST_ASSERT_TRUE(walk(st, m, ts::Identity(), 0,
                        {wFile("Band/Record/01 - Song.mp3", 100, kT), wFile("Band/Record/02 - Other.mp3", 101, kT),
                         wFile("Band/Record/03 - Third.mp3", 102, kT, Status::Software),
                         wFolder("Band/Record", facts(5, "cover.jpg"))}));
  TEST_ASSERT_TRUE(append(st, m, {scanned("Band/Record/01 - Song.mp3", 100, kT, 12)}));
  TEST_ASSERT_TRUE(st.compact().ok);
  compacted(m, false);
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  cc::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  cc::MemSource src2(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> rb(256), fb(512);
  ts::BuilderRows rows;
  ts::BuilderFacts fx;
  TEST_ASSERT_TRUE(rows.begin(src, rb.data(), 256));
  TEST_ASSERT_TRUE(fx.begin(src2, fb.data(), 512));
  uint32_t own = 0, ownFolders = 0;
  TEST_ASSERT_TRUE(rows.ownCounts(&own, &ownFolders));
  TEST_ASSERT_EQUAL_UINT32(2, own);
  TEST_ASSERT_EQUAL_UINT32(3, ownFolders);  // /music, Band, Band/Record
  TEST_ASSERT_TRUE(rows.row(0).status == Status::Scanned);
  TEST_ASSERT_TRUE(rows.row(2).status == Status::Software);
  TEST_ASSERT_TRUE(rows.row(1).status == Status::Pending);  // asked again from the start
  LibraryIndex index;
  LibraryBuilder b;
  LibraryBuilder::Config c;
  cc::MemSource dsrc(d.data(), static_cast<uint32_t>(d.size()));
  c.device = &dsrc;
  ts::BuilderRows rows2;
  cc::MemSource src3(d.data(), static_cast<uint32_t>(d.size()));
  TEST_ASSERT_TRUE(rows2.begin(src3, rb.data(), 256));
  c.rows = &rows2;
  c.facts = &fx;
  const LibraryBuilder::Result res = b.build(index, c);
  TEST_ASSERT_TRUE(res.built);
  TEST_ASSERT_EQUAL_UINT32(1, res.fromDevice);  // the scanned one
  TEST_ASSERT_EQUAL_UINT32(2, res.fromPath);    // Pending, and Software with no T
  TEST_ASSERT_EQUAL_UINT32(3, index.trackCount());
  const uint32_t t = index.findTrack("/music/Band/Record/01 - Song.mp3");
  TEST_ASSERT_TRUE(t != LibraryIndex::kNone);
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kFromDevice, index.track(t).flags & LibraryIndex::kSourceMask);
  const LibraryIndex::Folder& f = index.folder(index.track(t).folder);
  TEST_ASSERT_TRUE(f.image != LibraryIndex::kNone);
  TEST_ASSERT_EQUAL_STRING("cover.jpg", index.str(f.image));
}

// ---------------------------------------------------------------------------
// At 20k (the user's shape): a D of full records, a 512 KB journal, the
// compaction's reads, writes and memory.
// ---------------------------------------------------------------------------
void test_20k_compaction() {
  const synthcard::Card card = synthcard::make(synth::userShape(20000));
  TEST_ASSERT_EQUAL_size_t(20000, card.files.size());
  CutFs fs;
  ts::TagStore st(fs, config(), Heap::alloc, Heap::release);
  st.open();
  State m;
  std::vector<WOp> files;
  files.reserve(card.files.size());
  for (const auto& f : card.files) files.push_back(wFile(f.rel, f.size, f.fatTime));
  std::vector<uint8_t> wbuf(16384);
  {
    std::sort(files.begin(), files.end(), keyLess);
    ts::WalkWriter w;
    TEST_ASSERT_TRUE(st.beginWalk(ts::Identity(), wbuf.data(), 16384, &w));
    for (const WOp& o : files) TEST_ASSERT_TRUE(write(w, o));
    TEST_ASSERT_TRUE(w.endRun(0) && w.endRun(0) && w.finish());
  }
  ts::TagStore::Compacted c = st.compact();
  TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
  printf("[tagstore] 20k: D of Pending rows %u bytes\n", static_cast<unsigned>(fs.bytes(st.devicePath()).size()));
  // The scan, 100 files a chunk, compacting when the journal asks.
  auto t0 = std::chrono::steady_clock::now();
  uint32_t compactions = 0;
  uint64_t read = 0, written = 0;
  size_t peak = 0;
  ts::ChunkBuilder cb;
  cb.begin(chunkBuffer().data(), static_cast<uint32_t>(chunkBuffer().size()));
  for (size_t i = 0; i < card.files.size(); ++i) {
    const mptg::RecordIn r = synthcard::recordOf(card.files[i], true, 0);
    TEST_ASSERT_TRUE(cb.add(r.path, std::strlen(r.path), Status::Scanned, r.rec, r.fields));
    if (cb.count() == 100 || i + 1 == card.files.size()) {
      if (!st.append(cb)) TEST_FAIL_MESSAGE("append");
      if (st.wantsCompaction() || i + 1 == card.files.size()) {
        const uint64_t r0 = fs.bytesRead, w0 = fs.bytesWritten;
        Heap::peak = Heap::live;
        const size_t before = Heap::live;
        c = st.compact();
        TEST_ASSERT_TRUE_MESSAGE(c.ok, c.error);
        peak = std::max(peak, Heap::peak - before);
        ++compactions;
        read += fs.bytesRead - r0;
        written += fs.bytesWritten - w0;
        if (i + 1 == card.files.size())
          printf("[tagstore] 20k: the last compaction read %.2f MB, wrote %.2f MB, HIDX in %u passes\n",
                 (fs.bytesRead - r0) / 1e6, (fs.bytesWritten - w0) / 1e6, c.hidxPasses);
      }
    }
  }
  const long ms = static_cast<long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  const size_t dBytes = fs.bytes(st.devicePath()).size();
  printf("[tagstore] 20k scanned: %u compactions, %.1f MB read, %.1f MB written in all; D %u bytes; work %u bytes; "
         "%ld ms on the host\n",
         compactions, read / 1e6, written / 1e6, static_cast<unsigned>(dBytes), static_cast<unsigned>(peak), ms);
  TEST_ASSERT_EQUAL_UINT32(20000, c.records);
  TEST_ASSERT_TRUE(peak <= st.workBytes());
  // 3.3.2: about 4.1 MB with every file Scanned (HIDX included).
  TEST_ASSERT_TRUE(dBytes > 3500000 && dBytes < 4600000);
  // N1's reader takes it whole.
  const std::vector<uint8_t> d = fs.bytes(st.devicePath());
  cc::MemSource src(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> scratch(8192);
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(mptg::check(src, mptg::kUseAll, scratch.data(), 8192)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rows_header_and_facts);
  RUN_TEST(test_compaction_writes_what_the_writer_writes);
  RUN_TEST(test_merge_precedence);
  RUN_TEST(test_journal_tails_and_bases);
  RUN_TEST(test_a_bad_device_is_left_out);
  RUN_TEST(test_rescan_and_new_parser);
  RUN_TEST(test_cut_rename_rule);
  RUN_TEST(test_power_cut_at_every_step);
  RUN_TEST(test_power_cut_during_recovery);
  RUN_TEST(test_compaction_memory_is_bounded);
  RUN_TEST(test_builder_adapters);
  RUN_TEST(test_20k_compaction);
  return UNITY_END();
}
