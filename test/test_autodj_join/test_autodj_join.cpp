// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for Auto DJ's join (lib/core/AutoDjJoin; the engine
// design's 3.3 and 3.4, docs/AUTODJ-INDEX.md 11.2): the card's AutoDJ
// index turned into this library's track ids and resident filter words, in
// slices. Every join is checked against a plain reference (a map of every
// library track's full path hash, DJPH entry by entry):
// - the filter word (the BPM's rounding, the key, BPM_ANALYSED, the artist
//   key's low 16 bits) and the run sort against std::sort;
// - the goldens of test/fixtures/card (each description's files as the
//   library, with new files beside them and some gone; the smallest path
//   hash taking a row: NFC before NFD, autodj-tiny's two copies), in
//   default slices and in tiny ones;
// - every MPDJ hardening file that opens: the join's verdict and reason are
//   Table::check(kUseDevice)'s, and the present ones read as their golden;
//   every byte of the small goldens flipped, the same; a table whose open()
//   failed on its counts (a DJRF longer than rowCount among them) refused;
// - files from mpdj::write: gone files (a row's smallest one among them),
//   new files, rows without card files, a hash two card files share; and,
//   with hashes injected, 32-bit prefix collisions in a run and across
//   runs, two library tracks of one 64-bit hash (neither mapped), tracks
//   without a hash; longer minor-version DJRF and DJPH strides; each
//   section's CRC;
// - the slices: at most one read of at most 4 KB, 250 hashes, 100
//   confirms, one run's sort; a loop pass's slices (stepPass()): one card
//   read at most; two joins interleaved; a fence (release()) and an index change
//   (another pointer, none, the same object rebuilt) at every slice
//   boundary, everything freed; every allocation failing in turn; every
//   read failing in turn; 65,534 tracks and 65,535;
// - the same file on the host FatFs model (FatModelFs.h), read in its 4 KB
//   pieces;
// - the memory and the slices at 20,000, 22,000 and 27,000 tracks against
//   the design's figures (printed as [join] lines);
// - the folders' states (each folder's path hashed once): every track's
//   hash trackPathHash()'s, whatever the root's spelling, the paths'
//   length and the folders' depth;
// - the placement in a model of the device's heap (ESP-IDF's TLSF): the
//   blocks' order at a free block's top and without a placement, the holes
//   that take them, every allocation failing in turn; free never under the
//   margin (the device's 2026-10-14 check: 10 KB); the spacers; a first
//   boot's walk carving its buffers during the join, then a library
//   update's track table (LibraryUpdate::roomToBuild()) that fits once the
//   join is given back, as with Auto DJ off; and a library update of the
//   real 19,412-track library, with Auto DJ off and on (the rebuild's own
//   fragmentation, the join after it taking only what no smaller block
//   holds).
// Run: pio test -e native -f test_autodj_join
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/FatModel.h"
#include "../support/FatModelFs.h"
#include "../support/SynthCard.h"
#include "AutoDjJoin.h"
#include "ByteStream.h"
#include "CardAutoDj.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "LibraryUpdate.h"

namespace cc = cardcontract;
namespace mpdj = cardcontract::mpdj;
using autodj::Join;
using Bytes = std::vector<uint8_t>;
using Status = Join::Status;
using Phase = Join::Phase;
using minijson::Value;

namespace {

// ---------------------------------------------------------------------------
// The join's allocator: every block counted; the `failAt`-th fails.
// ---------------------------------------------------------------------------
struct Heap {
  static size_t live, peak, largest, allocs;
  static long failAt;  // 0-based; -1: none
  static std::map<void*, size_t>& blocks() {
    static std::map<void*, size_t> b;
    return b;
  }
  static void reset() {
    live = peak = largest = allocs = 0;
    failAt = -1;
    blocks().clear();
  }
  static void* alloc(size_t n) {
    if (failAt >= 0 && static_cast<long>(allocs) == failAt) {
      ++allocs;
      return nullptr;
    }
    ++allocs;
    void* p = std::malloc(n ? n : 1);
    blocks()[p] = n;
    live += n;
    peak = std::max(peak, live);
    largest = std::max(largest, n);
    return p;
  }
  static void release(void* p) {
    auto it = blocks().find(p);
    TEST_ASSERT_TRUE_MESSAGE(it != blocks().end(), "a block freed twice, or not ours");
    live -= it->second;
    blocks().erase(it);
    std::free(p);
  }
};
size_t Heap::live = 0, Heap::peak = 0, Heap::largest = 0, Heap::allocs = 0;
long Heap::failAt = -1;

// ---------------------------------------------------------------------------
// Files.
// ---------------------------------------------------------------------------
class VecSink : public cc::Sink {
public:
  bool write(uint32_t offset, const void* data, uint32_t n) override {
    if (offset + n > b.size()) b.resize(offset + n);
    if (n) std::memcpy(b.data() + offset, data, n);
    return true;
  }
  Bytes b;
};

// A Source that counts its reads, and fails every read from `failFrom` on.
class CountingSource : public cc::Source {
public:
  explicit CountingSource(cc::Source& in) : in_(in) {}
  uint32_t size() const override { return in_.size(); }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (failFrom >= 0 && static_cast<long>(reads) >= failFrom) {
      ++reads;
      return false;
    }
    ++reads;
    bytes += n;
    most = std::max(most, n);
    return in_.read(offset, out, n);
  }
  uint64_t reads = 0, bytes = 0;
  uint32_t most = 0;  // the longest read
  long failFrom = -1;

private:
  cc::Source& in_;
};

// An MPDJ file in memory, opened through a counting source.
struct File {
  Bytes bytes;
  cc::MemSource mem;
  CountingSource src;
  mpdj::Table table;
  cc::Why open;
  explicit File(Bytes b)
      : bytes(std::move(b)), mem(bytes.data(), static_cast<uint32_t>(bytes.size())), src(mem),
        open(table.open(src)) {}
  bool ok() const { return open == cc::Why::Ok; }
};

Bytes fixture(const std::string& rel) {
  const std::string s = cardfixtures::bytes(rel);
  TEST_ASSERT_FALSE_MESSAGE(s.empty(), rel.c_str());
  return Bytes(s.begin(), s.end());
}

// A description's card paths (relative to /music), rows in its order.
std::vector<std::string> descPaths(const std::string& name) {
  Value d;
  TEST_ASSERT_TRUE_MESSAGE(cardfixtures::json("libraries/" + name + ".json", &d), name.c_str());
  std::vector<std::string> out;
  for (const Value& row : d["rows"].items)
    for (const Value& f : row["files"].items) out.push_back(f["path"].str());
  return out;
}

// A table written by hand (test_card_autodj's writer would compute every
// pair's level): its rows' DJRF values and the DJPH entries as given (in
// the order given: sorted for a valid file), with strides as given (the
// tails filled with 0xA5), and empty slots (every entry unused, the CRCs
// right: the join never reads them).
struct RawTable {
  struct Row {
    uint16_t bpm10 = 0;
    uint8_t camelot = 0, flags = 0;
    uint32_t artistKey = 0;
  };
  std::vector<Row> rows;
  std::vector<std::pair<uint64_t, uint16_t>> paths;
  uint32_t filterStride = mpdj::kFilterStride, pathStride = mpdj::kPathStride;
  uint16_t minor = 0;
  uint32_t k = 4;
  uint32_t extraFilters = 0;  // DJRF records past rowCount (a file open() refuses: Counts)
};

Bytes writeRaw(const RawTable& t) {
  const uint32_t n = static_cast<uint32_t>(t.rows.size());
  const uint32_t p = static_cast<uint32_t>(t.paths.size());
  const uint32_t nf = n + t.extraFilters;
  const uint32_t slotBytes = mpdj::slotBytesFor(t.k);
  const char strs[] = "\0synthetic-embedding\0" "1\0cosine\0";  // offsets 1, 21, 23
  uint8_t th[mpdj::kMinHeaderBytes - cc::kCommonHeaderBytes] = {};
  cc::put32(th, n);
  cc::put32(th + 4, p);
  cc::put16(th + 8, static_cast<uint16_t>(t.k));
  cc::put16(th + 10, static_cast<uint16_t>(slotBytes));
  th[12] = mpdj::kIndexBytes;
  th[13] = mpdj::kScoreCosine;
  th[14] = mpdj::kSelTop;
  th[15] = static_cast<uint8_t>(t.k);
  cc::put32(th + 16, 1790000000);
  for (int i = 0; i < 16; ++i) th[24 + i] = static_cast<uint8_t>(0x30 + i);
  cc::put32(th + 40, 1);
  cc::put32(th + 44, 21);
  cc::put32(th + 48, 23);
  const cc::SectionOut s[4] = {
      cc::SectionOut{mpdj::kDjsl, cc::kSectionRequired, n, slotBytes, n * slotBytes},
      cc::SectionOut{mpdj::kDjrf, cc::kSectionRequired, nf, t.filterStride, nf * t.filterStride},
      cc::SectionOut{mpdj::kDjph, cc::kSectionRequired, p, t.pathStride, p * t.pathStride},
      cc::SectionOut{cc::kStrs, cc::kSectionRequired, 0, 0, sizeof(strs) - 1},
  };
  cc::FileMeta fm;
  fm.magic = cc::kMagicMpdj;
  fm.major = mpdj::kMajor2;
  fm.minor = t.minor;
  fm.generation = 3;
  fm.cardId = 0x5EEDC0DE00000003ull;
  VecSink out;
  cc::ContainerWriter cw;
  TEST_ASSERT_TRUE(cw.begin(out, fm, th, mpdj::headerBytesFor(4, slotBytes), s, 4, sizeof(th)));
  Bytes slot(slotBytes, 0);
  for (uint32_t j = 0; j < t.k; ++j) cc::put16(&slot[2 * j], mpdj::kUnused);
  for (uint32_t r = 0; r < n; ++r) {
    cc::put32(&slot[slotBytes - 4], mpdj::slotCrc(slot.data(), slotBytes, r));
    TEST_ASSERT_TRUE(cw.write(slot.data(), slotBytes));
  }
  Bytes rec(std::max(t.filterStride, t.pathStride), 0xA5);
  for (const RawTable::Row& r : t.rows) {
    std::fill(rec.begin(), rec.end(), 0xA5);
    cc::put16(&rec[0], r.bpm10);
    rec[2] = r.camelot;
    rec[3] = r.flags;
    cc::put32(&rec[4], r.artistKey);
    TEST_ASSERT_TRUE(cw.write(rec.data(), t.filterStride));
  }
  for (uint32_t r = 0; r < t.extraFilters; ++r) {
    std::fill(rec.begin(), rec.end(), 0xA5);
    TEST_ASSERT_TRUE(cw.write(rec.data(), t.filterStride));
  }
  for (const auto& e : t.paths) {
    std::fill(rec.begin(), rec.end(), 0xA5);
    cc::put64(&rec[0], e.first);
    cc::put16(&rec[8], e.second);
    TEST_ASSERT_TRUE(cw.write(rec.data(), t.pathStride));
  }
  TEST_ASSERT_TRUE(cw.write(strs, sizeof(strs) - 1));
  TEST_ASSERT_TRUE(cw.finish());
  return out.b;
}

// ---------------------------------------------------------------------------
// Libraries and hashes.
// ---------------------------------------------------------------------------
std::unique_ptr<LibraryIndex> libraryOf(const std::vector<std::string>& rels) {
  std::unique_ptr<LibraryIndex> idx(new LibraryIndex());
  TEST_ASSERT_TRUE(idx->begin("/music"));
  for (const std::string& r : rels)
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, static_cast<int>(idx->addFile(("/music/" + r).c_str())), r.c_str());
  TEST_ASSERT_TRUE(idx->finish());
  return idx;
}

void rebuild(LibraryIndex& idx, const std::vector<std::string>& rels) {
  idx.clear();
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const std::string& r : rels) idx.addFile(("/music/" + r).c_str());
  TEST_ASSERT_TRUE(idx.finish());
}

uint32_t trackOf(const LibraryIndex& idx, const std::string& rel) {
  const uint32_t t = idx.findTrack(("/music/" + rel).c_str());
  TEST_ASSERT_TRUE_MESSAGE(t != LibraryIndex::kNone, rel.c_str());
  return t;
}

// The full path's FNV-1a 64 (the default hook's pathHash() of the rest is
// the same function: an independent spelling of it).
bool fullHash(const LibraryIndex& idx, uint32_t t, uint64_t* h) {
  char p[Join::kPathBytes];
  if (!idx.trackPath(t, p, sizeof(p))) return false;
  *h = cc::fnv1a64Str(p);
  return true;
}

uint64_t g_hashCalls = 0;

bool countingHash(const LibraryIndex& idx, uint32_t t, char*, size_t, uint64_t* h, void*) {
  ++g_hashCalls;
  return fullHash(idx, t, h);
}

// Hashes given by track id (collisions can't be made from paths).
struct Injected {
  std::vector<uint64_t> hash;
  std::vector<uint8_t> has;
  bool of(uint32_t t, uint64_t* h) const {
    if (t >= has.size() || !has[t]) return false;
    *h = hash[t];
    return true;
  }
};

bool injectedHash(const LibraryIndex&, uint32_t t, char*, size_t, uint64_t* h, void* ctx) {
  ++g_hashCalls;
  return static_cast<const Injected*>(ctx)->of(t, h);
}

// ---------------------------------------------------------------------------
// The reference: every track's hash in a map, DJPH entry by entry.
// ---------------------------------------------------------------------------
using HashOf = std::function<bool(uint32_t, uint64_t*)>;

uint32_t plainWord(const mpdj::Filter& f) {
  uint32_t bpm = static_cast<uint32_t>(std::floor(f.bpm10 / 10.0 + 0.5));
  if (bpm > 255) bpm = 255;
  const uint32_t cam = f.camelot + (f.flags ? 32u : 0u);
  uint32_t art = f.artistKey % 65536u;
  if (art == 0 && f.artistKey != 0) art = 1;
  return bpm + cam * 256u + art * 65536u;
}

struct Expect {
  std::vector<uint16_t> t2r, r2t;
  std::vector<uint32_t> filters;
  uint32_t mapped = 0, withBpm = 0, withKey = 0, rowsMapped = 0, ambiguous = 0;
};

Expect reference(const LibraryIndex& idx, const mpdj::Table& t, const HashOf& hashOf) {
  Expect e;
  const uint32_t n = idx.trackCount(), rows = t.rowCount();
  e.t2r.assign(n, autodj::kNoRow);
  e.r2t.assign(rows, autodj::kNoTrack);
  e.filters.resize(rows);
  for (uint32_t r = 0; r < rows; ++r) {
    mpdj::Filter f;
    TEST_ASSERT_TRUE(t.filter(r, &f));
    e.filters[r] = plainWord(f);
  }
  std::multimap<uint64_t, uint32_t> byHash;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t h;
    if (hashOf(i, &h)) byHash.emplace(h, i);
  }
  const cc::Section& ph = t.container().section(mpdj::kSecPaths);
  for (uint32_t i = 0; i < ph.count; ++i) {
    uint8_t b[mpdj::kPathStride];
    TEST_ASSERT_TRUE(t.container().source()->read(ph.offset + i * ph.stride, b, sizeof(b)));
    const uint64_t h = cc::get64(b);
    const uint16_t row = cc::get16(b + 8);
    const auto range = byHash.equal_range(h);
    const size_t c = static_cast<size_t>(std::distance(range.first, range.second));
    if (c > 1) ++e.ambiguous;
    if (c != 1) continue;
    const uint32_t track = range.first->second;
    e.t2r[track] = row;
    ++e.mapped;
    if (e.filters[row] & 0xFF) ++e.withBpm;
    if ((e.filters[row] >> 8) & 0x1F) ++e.withKey;
    if (e.r2t[row] == autodj::kNoTrack) {
      e.r2t[row] = static_cast<uint16_t>(track);
      e.filters[row] |= 0x8000;
      ++e.rowsMapped;
    }
  }
  return e;
}

HashOf byPath(const LibraryIndex& idx) {
  return [&idx](uint32_t t, uint64_t* h) { return fullHash(idx, t, h); };
}

HashOf byInjected(const Injected& in) {
  return [&in](uint32_t t, uint64_t* h) { return in.of(t, h); };
}

void assertMaps(const Join& j, const Expect& e, const char* name) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Ready), static_cast<int>(j.status()), name);
  const autodj::Maps* m = j.maps();
  TEST_ASSERT_NOT_NULL_MESSAGE(m, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.t2r.size(), m->tracks, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.r2t.size(), m->rows, name);
  if (m->tracks) {
    TEST_ASSERT_NOT_NULL_MESSAGE(m->trackToRow, name);
    TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(e.t2r.data(), m->trackToRow, m->tracks, name);
  } else {
    TEST_ASSERT_NULL_MESSAGE(m->trackToRow, name);
  }
  TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(e.r2t.data(), m->rowToTrack, m->rows, name);
  TEST_ASSERT_EQUAL_HEX32_ARRAY_MESSAGE(e.filters.data(), m->filters, m->rows, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.mapped, m->mapped, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.withBpm, m->withBpm, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.withKey, m->withKey, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.rowsMapped, j.stats().rowsMapped, name);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(e.ambiguous, j.stats().ambiguous, name);
  for (uint32_t r = 0; r < m->rows; ++r)
    TEST_ASSERT_EQUAL_MESSAGE(m->rowToTrack[r] != autodj::kNoTrack, autodj::filterPresent(m->filters[r]), name);
  // Only the maps are held once Ready.
  const size_t maps = static_cast<size_t>(m->tracks) * 2 + static_cast<size_t>(m->rows) * 6;
  TEST_ASSERT_EQUAL_size_t_MESSAGE(maps, j.heldBytes(), name);
}

// ---------------------------------------------------------------------------
// Running a join, slice by slice.
// ---------------------------------------------------------------------------
struct Slice {
  Phase phase = Phase::None;
  uint64_t reads = 0;
  uint32_t most = 0;
  uint64_t hashes = 0;
};

Status runJoin(Join& j, const LibraryIndex* idx, CountingSource* src = nullptr, std::vector<Slice>* log = nullptr,
               uint32_t maxSlices = 0xFFFFFFFFu) {
  for (uint32_t s = 0; j.status() == Status::Working && s < maxSlices; ++s) {
    Slice sl;
    sl.phase = j.phase();
    const uint64_t r0 = src ? src->reads : 0, h0 = g_hashCalls;
    if (src) src->most = 0;
    j.step(idx);
    sl.reads = src ? src->reads - r0 : 0;
    sl.most = src ? src->most : 0;
    sl.hashes = g_hashCalls - h0;
    if (log) log->push_back(sl);
    TEST_ASSERT_TRUE_MESSAGE(s < 1000000, "a join that never ends");
  }
  return j.status();
}

// A whole join of `f` and `idx` with the given hook and config: Ready, and
// the reference's maps.
void joinAndCheck(File& f, const LibraryIndex& idx, const HashOf& hashOf, const char* name,
                  Join::PathHashFn hook = nullptr, void* ctx = nullptr, const Join::Config* cfg = nullptr) {
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(f.open), name);
  const Expect e = reference(idx, f.table, hashOf);
  Heap::reset();
  {
    Join j(Heap::alloc, Heap::release);
    if (hook) j.setPathHash(hook, ctx);
    if (cfg) j.setConfig(*cfg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, &idx)), name);
    runJoin(j, &idx);
    assertMaps(j, e, name);
    TEST_ASSERT_TRUE_MESSAGE(j.matches(&idx), name);
  }
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, Heap::live, name);
}

const Join::Config kTiny = {2, 1, 1024};  // two pairs a run, one hash a slice, 1 KB pieces

}  // namespace

void setUp() {
  Heap::reset();
  g_hashCalls = 0;
}
void tearDown() {}

// ---------------------------------------------------------------------------
// The filter word.
// ---------------------------------------------------------------------------
void test_filter_word() {
  using autodj::filterWord;
  const uint16_t bpm10s[] = {0, 1, 4, 5, 14, 15, 954, 955, 1204, 1205, 1210, 2544, 2545, 2554, 2555, 9999, 65535};
  const uint8_t bpms[] = {0, 0, 0, 1, 1, 2, 95, 96, 120, 121, 121, 254, 255, 255, 255, 255, 255};
  for (size_t i = 0; i < sizeof(bpm10s) / sizeof(bpm10s[0]); ++i)
    TEST_ASSERT_EQUAL_UINT8(bpms[i], autodj::filterBpm(filterWord(bpm10s[i], 0, 0, 0)));
  for (uint32_t c = 0; c <= 255; ++c) {
    const uint32_t w = filterWord(0, static_cast<uint8_t>(c), 0, 0);
    TEST_ASSERT_EQUAL_UINT8(c <= 24 ? c : 0, autodj::filterCamelot(w));
    TEST_ASSERT_FALSE(autodj::filterAnalysed(w));
    TEST_ASSERT_FALSE(autodj::filterPresent(w));
  }
  // BPM_ANALYSED is flags bit 0; the other bits are ignored.
  TEST_ASSERT_TRUE(autodj::filterAnalysed(filterWord(1200, 8, 1, 0)));
  TEST_ASSERT_TRUE(autodj::filterAnalysed(filterWord(1200, 8, 0xFF, 0)));
  TEST_ASSERT_FALSE(autodj::filterAnalysed(filterWord(1200, 8, 0xFE, 0)));
  TEST_ASSERT_EQUAL_HEX8(8 | autodj::kCamAnalysed, autodj::filterCam(filterWord(1200, 8, 0xFF, 0)));
  // The artist key's low 16 bits; 0 only for no artist.
  TEST_ASSERT_EQUAL_HEX16(0, autodj::filterArtist(filterWord(0, 0, 0, 0)));
  TEST_ASSERT_EQUAL_HEX16(0x5678, autodj::filterArtist(filterWord(0, 0, 0, 0x12345678)));
  TEST_ASSERT_EQUAL_HEX16(1, autodj::filterArtist(filterWord(0, 0, 0, 0x00010000)));
  TEST_ASSERT_EQUAL_HEX16(1, autodj::filterArtist(filterWord(0, 0, 0, 0xFFFF0000)));
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, autodj::filterArtist(filterWord(0, 0, 0, 0x0000FFFF)));
  TEST_ASSERT_EQUAL_HEX16(1, autodj::filterArtist(filterWord(0, 0, 0, 1)));
  // The whole word: autodj-00000029's row 2 (bpm10 1280, 1B, analysed,
  // artistKey B460AFEF).
  TEST_ASSERT_EQUAL_HEX32(128u | (13u | 0x20u) << 8 | 0xAFEFu << 16, filterWord(1280, 13, 1, 0xB460AFEF));
  const uint32_t present = filterWord(1280, 13, 1, 0xB460AFEF) | static_cast<uint32_t>(autodj::kCamPresent) << 8;
  TEST_ASSERT_TRUE(autodj::filterPresent(present));
  TEST_ASSERT_EQUAL_UINT8(13, autodj::filterCamelot(present));
  TEST_ASSERT_TRUE(autodj::filterAnalysed(present));
  TEST_ASSERT_EQUAL_UINT8(128, autodj::filterBpm(present));
  TEST_ASSERT_EQUAL_HEX16(0xAFEF, autodj::filterArtist(present));
}

// ---------------------------------------------------------------------------
// The run sort against std::sort.
// ---------------------------------------------------------------------------
void test_sort_pairs() {
  std::mt19937 rng(20261013);
  const uint32_t sizes[] = {0, 1, 2, 3, 16, 17, 100, 128, 129, 1000, 4096};
  for (uint32_t n : sizes) {
    for (int shape = 0; shape < 7; ++shape) {
      std::vector<uint32_t> k(n);
      std::vector<uint16_t> v(n);
      for (uint32_t i = 0; i < n; ++i) {
        v[i] = static_cast<uint16_t>(i * 7919u % 65536u);  // distinct for n <= 4096
        switch (shape) {
          case 0: k[i] = rng(); break;                         // random
          case 1: k[i] = i; break;                             // sorted
          case 2: k[i] = n - i; break;                         // reversed
          case 3: k[i] = 42; break;                            // one key (the ids decide)
          case 4: k[i] = rng() % 5; break;                     // few keys
          case 5: k[i] = i < n / 2 ? i : rng() % 16; break;    // sorted, then a tail of small keys
          default: k[i] = rng() & 1 ? 0 : 0xFFFFFFFFu; break;  // the two ends
        }
      }
      std::vector<std::pair<uint32_t, uint16_t>> want(n);
      for (uint32_t i = 0; i < n; ++i) want[i] = {k[i], v[i]};
      std::sort(want.begin(), want.end());
      autodj::sortPairs(k.data(), v.data(), n);
      for (uint32_t i = 0; i < n; ++i) {
        TEST_ASSERT_EQUAL_HEX32(want[i].first, k[i]);
        TEST_ASSERT_EQUAL_HEX16(want[i].second, v[i]);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The goldens: each description's card files as the library.
// ---------------------------------------------------------------------------
void test_goldens() {
  const char* names[] = {"autodj-00000029", "autodj-small-top", "autodj-small-default", "autodj-tiny",
                         "autodj-pair",     "autodj-mid",       "autodj-mid-default"};
  const std::vector<std::string> extra = {"New Artist 01/New Album 01/01 - New Title 01.mp3",
                                          "New Artist 01/New Album 01/02 - New Title 02.flac", "Loose New Title.opus"};
  for (const char* name : names) {
    const std::vector<std::string> paths = descPaths(name);
    TEST_ASSERT_TRUE(paths.size() >= 2);
    // Every file there, and new files beside them.
    std::vector<std::string> all = paths;
    all.insert(all.end(), extra.begin(), extra.end());
    // Some gone: every third file (the first of them in a small file).
    std::vector<std::string> some = extra;
    for (size_t i = 0; i < paths.size(); ++i)
      if (i % 3 != 0) some.push_back(paths[i]);
    for (const auto* lib : {&all, &some}) {
      auto idx = libraryOf(*lib);
      File f(fixture(std::string("golden/") + name + ".bin"));
      const std::string label = std::string(name) + (lib == &all ? " (all files)" : " (some gone)");
      joinAndCheck(f, *idx, byPath(*idx), label.c_str());  // the join's own hash
      File g(fixture(std::string("golden/") + name + ".bin"));
      joinAndCheck(g, *idx, byPath(*idx), (label + ", tiny slices").c_str(), countingHash, nullptr, &kTiny);
      // Every file of the description that is in the library has its row.
      Join j;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, idx.get())));
      runJoin(j, idx.get());
      uint32_t present = 0;
      for (const std::string& p : paths) {
        const uint32_t t = idx->findTrack(("/music/" + p).c_str());
        if (t == LibraryIndex::kNone) continue;
        ++present;
        TEST_ASSERT_NOT_EQUAL_MESSAGE(autodj::kNoRow, j.maps()->trackToRow[t], p.c_str());
        TEST_ASSERT_EQUAL_UINT32(f.table.rowOf(cc::pathHash(p.c_str())), j.maps()->trackToRow[t]);
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(present, j.maps()->mapped, label.c_str());
      for (const std::string& p : extra)
        TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, j.maps()->trackToRow[trackOf(*idx, p)]);
    }
  }

  // autodj-00000029 by hand (AUTODJ-INDEX.md 13.4).
  const std::vector<std::string> paths = descPaths("autodj-00000029");
  auto idx = libraryOf(paths);
  File f(fixture("golden/autodj-00000029.bin"));
  Join j;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, idx.get())));
  runJoin(j, idx.get());
  const autodj::Maps& m = *j.maps();
  TEST_ASSERT_EQUAL_UINT32(7, m.tracks);
  TEST_ASSERT_EQUAL_UINT32(6, m.rows);
  TEST_ASSERT_EQUAL_UINT32(7, m.mapped);  // the NFC and the NFD file both
  TEST_ASSERT_EQUAL_UINT32(4, m.withBpm);  // rows 0, 2, 3, 5's files
  TEST_ASSERT_EQUAL_UINT32(4, m.withKey);
  TEST_ASSERT_EQUAL_UINT32(6, j.stats().rowsMapped);
  const std::string nfc = "Generated Artist 02/Sample Album 02/01 - Sample Caf\xC3\xA9.flac";
  const std::string nfd = "Generated Artist 02/Sample Album 02/01 - Sample Cafe\xCC\x81.flac";
  TEST_ASSERT_EQUAL_HEX64(0x6393C560DD800DEBull, cc::pathHash(nfc.c_str()));
  TEST_ASSERT_EQUAL_HEX64(0xECD5A60887B28229ull, cc::pathHash(nfd.c_str()));
  TEST_ASSERT_EQUAL_UINT16(1, m.trackToRow[trackOf(*idx, nfc)]);
  TEST_ASSERT_EQUAL_UINT16(1, m.trackToRow[trackOf(*idx, nfd)]);
  TEST_ASSERT_EQUAL_UINT16(trackOf(*idx, nfc), m.rowToTrack[1]);  // the smaller hash
  const uint32_t P = static_cast<uint32_t>(autodj::kCamPresent) << 8;
  const uint32_t want[6] = {121u | 8u << 8 | 0xA770u << 16 | P,
                            P,  // no BPM, no key, no artist
                            128u | (13u | 0x20u) << 8 | 0xAFEFu << 16 | P,
                            95u | (21u | 0x20u) << 8 | 0xAAD6u << 16 | P,  // bpm10 954
                            0xAAD6u << 16 | P,
                            96u | 21u << 8 | 0xAAD6u << 16 | P};
  TEST_ASSERT_EQUAL_HEX32_ARRAY(want, m.filters, 6);
  const char* rows[6] = {"Generated Artist 01/Sample Album 01/01 - Sample Title 01.mp3", nullptr,
                         "Generated Artist 04/Sample Album 04/03 - Sample Title 04.mp3",
                         "Generated Artist 03/Sample Album 03/01 - Sample Title 03.mp3",
                         "Generated Artist 03/Sample Album 06/07 - SAMPLE TITLE 03.flac",
                         "Generated Artist 03/Sample Album 07/12 - Sample Title 03.opus"};
  for (uint16_t r = 0; r < 6; ++r) {
    if (!rows[r]) continue;
    TEST_ASSERT_EQUAL_UINT16(r, m.trackToRow[trackOf(*idx, rows[r])]);
    TEST_ASSERT_EQUAL_UINT16(trackOf(*idx, rows[r]), m.rowToTrack[r]);
  }

  // autodj-tiny: a row with two copies; the smaller hash is the row's track.
  {
    const std::string live = "Generated Artist 31/Live/01 - Sample Title 31.mp3";
    const std::string copy = "Generated Artist 31/Copy/01 - Sample Title 31.mp3";
    auto tiny = libraryOf(descPaths("autodj-tiny"));
    File t(fixture("golden/autodj-tiny.bin"));
    Join k;
    k.begin(t.table, tiny.get());
    runJoin(k, tiny.get());
    const uint32_t row = k.maps()->trackToRow[trackOf(*tiny, live)];
    TEST_ASSERT_EQUAL_UINT16(row, k.maps()->trackToRow[trackOf(*tiny, copy)]);
    const std::string& first = cc::pathHash(live.c_str()) < cc::pathHash(copy.c_str()) ? live : copy;
    TEST_ASSERT_EQUAL_UINT16(trackOf(*tiny, first), k.maps()->rowToTrack[row]);
    // Its other row's file gone: that row has no track.
    auto alone = libraryOf({live, copy});
    k.begin(t.table, alone.get());
    runJoin(k, alone.get());
    TEST_ASSERT_EQUAL_UINT32(2, k.maps()->mapped);
    TEST_ASSERT_EQUAL_UINT32(1, k.stats().rowsMapped);
    TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, k.maps()->rowToTrack[row == 0 ? 1 : 0]);
    TEST_ASSERT_FALSE(autodj::filterPresent(k.maps()->filters[row == 0 ? 1 : 0]));
  }
}

// ---------------------------------------------------------------------------
// The hardening files: the join's verdict is Table::check(kUseDevice)'s.
// ---------------------------------------------------------------------------
void test_hardening() {
  Value index;
  TEST_ASSERT_TRUE(cardfixtures::json("hardening/index.json", &index));
  auto idx = libraryOf(descPaths("autodj-00000029"));
  int opened = 0, absent = 0, present = 0;
  for (const Value& e : index.items) {
    if (e["format"].str() != "MPDJ") continue;
    const std::string name = e["file"].str();
    File f(fixture("hardening/" + name));
    if (!f.ok()) {
      // The join never begins on a file that doesn't open.
      TEST_ASSERT_EQUAL_STRING_MESSAGE("absent", e["expect"].c_str(), name.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(f.open), name.c_str());
      continue;
    }
    ++opened;
    uint8_t scratch[1024];
    const cc::Why device = f.table.check(mpdj::Table::kUseDevice, scratch, sizeof(scratch));
    Join j(Heap::alloc, Heap::release);
    Heap::reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, idx.get())),
                                  name.c_str());
    runJoin(j, idx.get());
    if (device == cc::Why::Ok) {
      ++present;
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Ready), static_cast<int>(j.status()), name.c_str());
      // A slot's fault is the pick's to find (8.5), not the join's.
      const std::string why = e["why"].str();
      TEST_ASSERT_TRUE_MESSAGE(e["expect"].str() == "present" || why.compare(0, 6, "djSlot") == 0, name.c_str());
      // A present file reads as its golden: the golden's reference maps.
      const std::string same = e["same"].str();
      if (!same.empty()) {
        File g(fixture("golden/" + same));
        assertMaps(j, reference(*idx, g.table, byPath(*idx)), name.c_str());
      }
      if (name == "mpdj-paths-empty.bin") {
        TEST_ASSERT_EQUAL_UINT32(0, j.maps()->mapped);
        for (uint32_t r = 0; r < j.maps()->rows; ++r)
          TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, j.maps()->rowToTrack[r]);
        for (uint32_t t = 0; t < j.maps()->tracks; ++t)
          TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, j.maps()->trackToRow[t]);
      }
    } else {
      ++absent;
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Absent), static_cast<int>(j.status()), name.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(cc::whyName(device), cc::whyName(j.why()), name.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(e["why"].c_str(), cc::whyName(j.why()), name.c_str());
      TEST_ASSERT_NULL(j.maps());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, Heap::live, name.c_str());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(0, j.heldBytes(), name.c_str());
    }
  }
  // The STRS and DJPH faults (7), the present files (8) and the slot
  // faults a pick finds (7) all open.
  TEST_ASSERT_EQUAL_INT(7, absent);
  TEST_ASSERT_EQUAL_INT(15, present);
  TEST_ASSERT_EQUAL_INT(22, opened);
}

// A Table whose open() failed after its frame (Why::Counts) keeps its
// rowCount and its sections: begin() on it (a caller that ignored the
// verdict) is refused, not trusted. A DJRF longer than rowCount would
// otherwise write past the filters.
void test_a_table_whose_open_failed() {
  auto idx = libraryOf(descPaths("autodj-00000029"));
  auto refused = [&](File& f, const char* label) {
    TEST_ASSERT_EQUAL_STRING_MESSAGE("counts", cc::whyName(f.open), label);
    TEST_ASSERT_TRUE_MESSAGE(f.table.container().source() && f.table.rowCount() > 0, label);
    Join j(Heap::alloc, Heap::release);
    Heap::reset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Absent), static_cast<int>(j.begin(f.table, idx.get())),
                                  label);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("counts", cc::whyName(j.why()), label);
    TEST_ASSERT_NULL(j.maps());
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, Heap::allocs, label);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Absent), static_cast<int>(j.step(idx.get())), label);
  };
  for (const char* name : {"mpdj-row-count.bin", "mpdj-djrf-count.bin"}) {
    File f(fixture(std::string("hardening/") + name));
    refused(f, name);
  }
  RawTable t;
  t.rows.resize(4);
  t.extraFilters = 3;  // DJRF of 7 records for 4 rows
  File f(writeRaw(t));
  TEST_ASSERT_EQUAL_UINT32(4, f.table.rowCount());
  TEST_ASSERT_EQUAL_UINT32(7, f.table.container().section(mpdj::kSecFilters).count);
  refused(f, "DJRF of 7 for 4 rows");
}

// ---------------------------------------------------------------------------
// Every byte of the small goldens flipped: the join's verdict is the
// device check's, and a file that stays present maps the same.
// ---------------------------------------------------------------------------
void test_flipped_bytes() {
  const char* names[] = {"autodj-00000029", "autodj-small-top", "autodj-small-default", "autodj-tiny",
                         "autodj-pair"};
  int flips = 0, kept = 0;
  for (const char* name : names) {
    auto idx = libraryOf(descPaths(name));
    const Bytes golden = fixture(std::string("golden/") + name + ".bin");
    File g(golden);
    const Expect e = reference(*idx, g.table, byPath(*idx));
    for (size_t at = 0; at < golden.size(); ++at) {
      Bytes b = golden;
      b[at] ^= 0x5A;
      File f(b);
      if (!f.ok()) continue;  // the header and the directory: open()'s
      ++flips;
      uint8_t scratch[1024];
      const cc::Why device = f.table.check(mpdj::Table::kUseDevice, scratch, sizeof(scratch));
      Join j;
      j.setConfig(kTiny);
      j.begin(f.table, idx.get());
      runJoin(j, idx.get());
      char label[96];
      std::snprintf(label, sizeof(label), "%s, byte %u", name, static_cast<unsigned>(at));
      if (device == cc::Why::Ok) {
        ++kept;
        assertMaps(j, e, label);  // a slot's byte, a gap's, the header's padding
      } else {
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Absent), static_cast<int>(j.status()), label);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cc::whyName(device), cc::whyName(j.why()), label);
      }
    }
  }
  printf("[join] flipped bytes: %d opened, %d still present (slots, gaps, padding)\n", flips, kept);
  TEST_ASSERT_TRUE(flips > kept);
}

// ---------------------------------------------------------------------------
// mpdj::write's files with real paths: gone files, new files, a row's
// smallest file gone, rows without card files.
// ---------------------------------------------------------------------------
namespace {

struct WriteRow {
  uint8_t id;
  std::vector<float> e;
  std::vector<uint64_t> paths;
  uint16_t bpm10 = 0;
  uint8_t camelot = 0, flags = 0;
  uint32_t artistKey = 0;
};

Bytes writeTable(const std::vector<WriteRow>& rows) {
  std::vector<mpdj::RowIn> in(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    in[i].hash[0] = rows[i].id;
    in[i].hash[15] = static_cast<uint8_t>(i);
    in[i].paths = rows[i].paths.data();
    in[i].pathCount = static_cast<uint32_t>(rows[i].paths.size());
    in[i].bpm10 = rows[i].bpm10;
    in[i].camelot = rows[i].camelot;
    in[i].flags = rows[i].flags;
    in[i].artistKey = rows[i].artistKey;
    in[i].embedding = rows[i].e.data();
  }
  mpdj::TableIn t;
  t.generation = 3;
  t.k = 4;
  t.selKind = mpdj::kSelTop;
  t.modelId = "synthetic-embedding";
  t.modelVersion = "1";
  t.license = "CC0-1.0";
  t.dim = 2;
  t.rows = in.data();
  t.rowCount = static_cast<uint32_t>(in.size());
  VecSink out;
  uint8_t sig[16];
  const char* error = nullptr;
  TEST_ASSERT_TRUE_MESSAGE(mpdj::write(out, t, sig, nullptr, nullptr, &error), error ? error : "");
  return out.b;
}

std::vector<float> unit(int i) {
  const double a = 0.1 * i;
  return {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
}

}  // namespace

void test_written_files() {
  char buf[96];
  auto name = [&](int artist, int album, int n) {
    std::snprintf(buf, sizeof(buf), "Generated Artist %02d/Sample Album %02d/%02d - Sample Title %02d.mp3", artist,
                  album, n, n);
    return std::string(buf);
  };
  // Row A: one file. B: three copies. C: two copies, its only files gone.
  // D: a copy in another album. E: no card file the library has.
  const std::string a = name(41, 1, 1);
  const std::vector<std::string> b = {name(42, 1, 2), name(42, 2, 2), name(42, 3, 2)};
  const std::vector<std::string> c = {name(43, 1, 3), name(43, 2, 3)};
  const std::vector<std::string> d = {name(44, 1, 4), name(44, 9, 4)};
  const std::string gone = name(45, 1, 5);
  auto hashes = [](const std::vector<std::string>& v) {
    std::vector<uint64_t> h;
    for (const auto& s : v) h.push_back(cc::pathHash(s.c_str()));
    return h;
  };
  std::vector<WriteRow> rows(5);
  rows[0] = {10, unit(0), hashes({a}), 1205, 8, 1, 0xB460A770};
  rows[1] = {20, unit(1), hashes(b), 0, 24, 0, 0x00010000};
  rows[2] = {30, unit(2), hashes(c), 900, 0, 0, 0};
  rows[3] = {40, unit(3), hashes(d), 2600, 25, 1, 7};
  rows[4] = {50, unit(4), hashes({gone}), 1000, 3, 0, 9};
  const Bytes file = writeTable(rows);

  // B's smallest file gone, and one of D's: each row's track is its
  // smallest present file.
  std::vector<std::string> bySize = b;
  std::sort(bySize.begin(), bySize.end(), [](const std::string& x, const std::string& y) {
    return cc::pathHash(x.c_str()) < cc::pathHash(y.c_str());
  });
  const std::vector<std::string> lib = {
      a, bySize[1], bySize[2], d[1], "New Artist 02/New Album 02/01 - New.mp3", "Loose.flac"};
  auto idx = libraryOf(lib);
  File f(file);
  joinAndCheck(f, *idx, byPath(*idx), "written");
  joinAndCheck(f, *idx, byPath(*idx), "written, tiny slices", countingHash, nullptr, &kTiny);
  Join j;
  j.begin(f.table, idx.get());
  runJoin(j, idx.get());
  const autodj::Maps& m = *j.maps();
  // Rows are in canonical-hash order: the ids rise, so row i is rows[i].
  TEST_ASSERT_EQUAL_UINT16(0, m.trackToRow[trackOf(*idx, a)]);
  TEST_ASSERT_EQUAL_UINT16(1, m.trackToRow[trackOf(*idx, bySize[1])]);
  TEST_ASSERT_EQUAL_UINT16(1, m.trackToRow[trackOf(*idx, bySize[2])]);
  TEST_ASSERT_EQUAL_UINT16(trackOf(*idx, bySize[1]), m.rowToTrack[1]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, m.rowToTrack[2]);
  TEST_ASSERT_EQUAL_UINT16(trackOf(*idx, d[1]), m.rowToTrack[3]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, m.rowToTrack[4]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[trackOf(*idx, "Loose.flac")]);
  TEST_ASSERT_EQUAL_UINT32(4, m.mapped);  // A's, B's two, D's one
  TEST_ASSERT_EQUAL_UINT32(3, j.stats().rowsMapped);
  TEST_ASSERT_EQUAL_UINT32(2, m.withBpm);  // A's and D's: B's two have none
  TEST_ASSERT_EQUAL_UINT32(3, m.withKey);  // A's, B's two (24); D's 25 reads as none
  TEST_ASSERT_EQUAL_UINT8(121, autodj::filterBpm(m.filters[0]));
  TEST_ASSERT_EQUAL_UINT8(255, autodj::filterBpm(m.filters[3]));
  TEST_ASSERT_EQUAL_UINT8(0, autodj::filterCamelot(m.filters[3]));
  TEST_ASSERT_EQUAL_HEX16(1, autodj::filterArtist(m.filters[1]));
  TEST_ASSERT_EQUAL_UINT8(90, autodj::filterBpm(m.filters[2]));
  TEST_ASSERT_FALSE(autodj::filterPresent(m.filters[2]));

  // An empty library: nothing maps, the filters are read all the same.
  auto none = libraryOf({});
  File g(file);
  joinAndCheck(g, *none, byPath(*none), "an empty library");
  j.begin(g.table, none.get());
  runJoin(j, none.get());
  TEST_ASSERT_EQUAL_UINT32(0, j.maps()->tracks);
  TEST_ASSERT_NULL(j.maps()->trackToRow);
  TEST_ASSERT_EQUAL_UINT8(121, autodj::filterBpm(j.maps()->filters[0]));
}

// ---------------------------------------------------------------------------
// Hashes injected: collisions on 32 and 64 bits, in a run and across runs.
// ---------------------------------------------------------------------------
void test_injected_hashes() {
  // 24 library tracks (the files' names don't matter: their hashes are
  // given), runs of 4 pairs (so 6 runs) or one run of 4,096.
  std::vector<std::string> files;
  char buf[64];
  for (int i = 0; i < 24; ++i) {
    std::snprintf(buf, sizeof(buf), "Generated Artist 50/Sample Album 50/%02d - Sample Title.mp3", i);
    files.push_back(buf);
  }
  auto idx = libraryOf(files);
  Injected in;
  in.hash.assign(24, 0);
  in.has.assign(24, 1);
  auto H = [](uint32_t hi, uint32_t lo) { return static_cast<uint64_t>(hi) << 32 | lo; };
  const uint32_t P = 0x80000000u;
  // Tracks 0-3: one prefix, four low halves (a run's worth); tracks 4 and
  // 20 the same prefix again (other runs), 4 one of DJPH's hashes.
  for (uint32_t t = 0; t < 4; ++t) in.hash[t] = H(P, 0x100 + t);
  in.hash[4] = H(P, 0x200);
  in.hash[20] = H(P, 0x300);
  // Tracks 5 and 17: one full hash (a 64-bit collision of two library
  // paths): DJPH's entry maps neither.
  in.hash[5] = in.hash[17] = H(0x12345678u, 0x9ABCDEF0u);
  // Tracks 6 and 7: the ends of the prefixes.
  in.hash[6] = H(0, 0);
  in.hash[7] = H(0xFFFFFFFFu, 0xFFFFFFFFu);
  // Track 8: a hash DJPH leaves out (two card files share it).
  in.hash[8] = H(0x55555555u, 1);
  // Track 9: no hash (its path didn't fit).
  in.has[9] = 0;
  // Tracks 10-15: a row each, in reverse order of their ids' hashes.
  for (uint32_t t = 10; t < 16; ++t) in.hash[t] = H(0x40000000u - t, t);
  // 16, 18, 19, 21-23: hashes no DJPH entry has, prefixes among the others.
  in.hash[16] = H(P, 0x150);
  in.hash[18] = H(0x12345678u, 0x9ABCDEF1u);
  in.hash[19] = H(0x40000000u - 10, 11);
  for (uint32_t t = 21; t < 24; ++t) in.hash[t] = H(t, t);

  std::vector<WriteRow> rows(8);
  rows[0] = {1, unit(0), {in.hash[0], in.hash[2]}, 1200, 1, 0, 1};       // two of one prefix
  rows[1] = {2, unit(1), {in.hash[4], H(P, 0x250)}, 0, 0, 0, 0};         // across runs; a file not here
  rows[2] = {3, unit(2), {in.hash[5]}, 1300, 2, 0, 2};                   // the 64-bit collision
  rows[3] = {4, unit(3), {in.hash[6], in.hash[7]}, 1400, 3, 1, 3};       // the ends
  rows[4] = {5, unit(4), {in.hash[8]}, 1500, 4, 0, 4};                   // shared with row 5's file
  rows[5] = {6, unit(5), {in.hash[8], in.hash[3]}, 1600, 5, 0, 5};
  rows[6] = {7, unit(6), {}, 1700, 6, 0, 6};
  rows[7] = {8, unit(7), {in.hash[1]}, 0, 7, 0, 7};
  for (uint32_t t = 10; t < 16; ++t) rows[6].paths.push_back(in.hash[t]);
  const Bytes file = writeTable(rows);
  File f(file);
  TEST_ASSERT_EQUAL_UINT32(15, f.table.pathCount());  // 17 card files, the shared hash's two left out

  const Join::Config cfgs[] = {{4, 3, 1024}, {4096, 1000, 4096}, {1, 1, 1024}, {7, 24, 1024}};
  for (const Join::Config& cfg : cfgs) {
    g_hashCalls = 0;
    joinAndCheck(f, *idx, byInjected(in), "injected", injectedHash, &in, &cfg);
  }
  Join j;
  j.setPathHash(injectedHash, &in);
  j.setConfig(cfgs[0]);
  j.begin(f.table, idx.get());
  runJoin(j, idx.get());
  const autodj::Maps& m = *j.maps();
  TEST_ASSERT_EQUAL_UINT16(0, m.trackToRow[0]);
  TEST_ASSERT_EQUAL_UINT16(0, m.trackToRow[2]);
  TEST_ASSERT_EQUAL_UINT16(7, m.trackToRow[1]);
  TEST_ASSERT_EQUAL_UINT16(5, m.trackToRow[3]);
  TEST_ASSERT_EQUAL_UINT16(1, m.trackToRow[4]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[5]);   // ambiguous
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[17]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, m.rowToTrack[2]);
  TEST_ASSERT_EQUAL_UINT16(3, m.trackToRow[6]);
  TEST_ASSERT_EQUAL_UINT16(3, m.trackToRow[7]);
  TEST_ASSERT_EQUAL_UINT16(6, m.rowToTrack[3]);               // hash 0 is the smallest
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[8]);   // left out of DJPH
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoTrack, m.rowToTrack[4]);
  TEST_ASSERT_EQUAL_UINT16(3, m.rowToTrack[5]);
  TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[9]);   // no hash
  for (uint32_t t = 10; t < 16; ++t) TEST_ASSERT_EQUAL_UINT16(6, m.trackToRow[t]);
  TEST_ASSERT_EQUAL_UINT16(15, m.rowToTrack[6]);              // the smallest hash: 0x40000000 - 15
  for (uint32_t t : {16u, 18u, 19u, 21u, 22u, 23u}) TEST_ASSERT_EQUAL_HEX16(autodj::kNoRow, m.trackToRow[t]);
  TEST_ASSERT_EQUAL_UINT32(1, j.stats().ambiguous);
  TEST_ASSERT_EQUAL_UINT32(1, j.stats().unhashed);
  TEST_ASSERT_EQUAL_UINT32(23, j.stats().hashed);
  TEST_ASSERT_EQUAL_UINT32(15, j.stats().paths);
  // Prefix P: 6 entries, each confirming the 7 tracks of that prefix (5
  // of them someone's); 0x12345678: tracks 5, 17 (both the entry's) and
  // 18; tracks 10 and 19 under track 10's entry; one each for the rest.
  TEST_ASSERT_EQUAL_UINT32(6 * 7 + 3 + 2 + 2 + 5, j.stats().confirms);
  TEST_ASSERT_EQUAL_UINT32(6 * 7 - 5 + 1 + 1, j.stats().prefixOnly);
  TEST_ASSERT_EQUAL_UINT32(13, m.mapped);
  TEST_ASSERT_EQUAL_UINT32(6, j.stats().rowsMapped);
}

// ---------------------------------------------------------------------------
// Longer records (a later minor), and each section's CRC.
// ---------------------------------------------------------------------------
void test_strides_and_crcs() {
  std::vector<std::string> files;
  char buf[96];
  RawTable raw;
  for (int i = 0; i < 300; ++i) {
    std::snprintf(buf, sizeof(buf), "Generated Artist %02d/Sample Album %02d/%02d - Sample Title %03d.mp3", i % 7,
                  i % 11, i % 13, i);
    files.push_back(buf);
    RawTable::Row r;
    r.bpm10 = static_cast<uint16_t>(i % 5 ? 600 + 7 * i : 0);
    r.camelot = static_cast<uint8_t>(i % 26);
    r.flags = static_cast<uint8_t>(i % 3 ? 0xFE : 0x01);
    r.artistKey = 0x9E3779B9u * static_cast<uint32_t>(i);
    raw.rows.push_back(r);
    if (i % 4) raw.paths.emplace_back(cc::pathHash(files.back().c_str()), static_cast<uint16_t>(299 - i));
  }
  std::sort(raw.paths.begin(), raw.paths.end());
  auto idx = libraryOf(files);
  File base(writeRaw(raw));
  joinAndCheck(base, *idx, byPath(*idx), "raw");
  Join k;
  k.begin(base.table, idx.get());
  runJoin(k, idx.get());
  const autodj::Maps& want = *k.maps();
  TEST_ASSERT_EQUAL_UINT32(225, want.mapped);

  const uint32_t strides[][2] = {{12, 12}, {16, 10}, {8, 16}, {24, 40}, {4100, 10}, {8, 5000}};
  for (const auto& s : strides) {
    RawTable r = raw;
    r.filterStride = s[0];
    r.pathStride = s[1];
    r.minor = 1;
    for (const Join::Config& cfg : {Join::Config(), kTiny}) {
      File f(writeRaw(r));
      char label[64];
      std::snprintf(label, sizeof(label), "strides %u/%u", static_cast<unsigned>(s[0]), static_cast<unsigned>(s[1]));
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(f.open), label);
      Join j;
      j.setConfig(cfg);
      j.begin(f.table, idx.get());
      std::vector<Slice> log;
      runJoin(j, idx.get(), &f.src, &log);
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Ready), static_cast<int>(j.status()), label);
      TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(want.trackToRow, j.maps()->trackToRow, want.tracks, label);
      TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(want.rowToTrack, j.maps()->rowToTrack, want.rows, label);
      TEST_ASSERT_EQUAL_HEX32_ARRAY_MESSAGE(want.filters, j.maps()->filters, want.rows, label);
      // A record longer than a piece takes a slice of its own; the others
      // read at most one piece a slice.
      const uint32_t piece = cfg.sliceBytes;
      for (const Slice& sl : log) {
        TEST_ASSERT_TRUE_MESSAGE(sl.most <= piece, label);
        if (s[0] <= piece && s[1] <= piece) TEST_ASSERT_TRUE_MESSAGE(sl.reads <= 1, label);
      }
    }
  }

  // Each section's CRC: a byte of each record's tail (no other check
  // reads it), then a string's byte.
  RawTable r = raw;
  r.filterStride = 12;
  r.pathStride = 12;
  const Bytes good = writeRaw(r);
  File g(good);
  const uint32_t at[3] = {g.table.container().section(mpdj::kSecFilters).offset + 12 * 150 + 10,
                          g.table.container().section(mpdj::kSecPaths).offset + 12 * 100 + 11,
                          g.table.container().section(mpdj::kSecStrs).offset + 5};
  const Phase phases[3] = {Phase::Filters, Phase::Paths, Phase::Strings};
  for (int i = 0; i < 3; ++i) {
    Bytes b = good;
    b[at[i]] ^= 1;
    File f(b);
    TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(f.open));
    Heap::reset();
    Join j(Heap::alloc, Heap::release);
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Absent), static_cast<int>(j.status()));
    TEST_ASSERT_EQUAL_STRING("sectionCrc", cc::whyName(j.why()));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(phases[i]), static_cast<int>(j.phase()));
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
    uint8_t scratch[1024];
    const cc::Why device = f.table.check(mpdj::Table::kUseDevice, scratch, sizeof(scratch));
    TEST_ASSERT_EQUAL_STRING("sectionCrc", cc::whyName(device));
  }
  // DJPH's order and rows, under good CRCs.
  for (int fault = 0; fault < 3; ++fault) {
    RawTable bad = raw;
    if (fault == 0) std::swap(bad.paths[10], bad.paths[11]);
    if (fault == 1) bad.paths[50].first = bad.paths[49].first;
    if (fault == 2) bad.paths[200].second = 300;  // rowCount
    File f(writeRaw(bad));
    Join j;
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Absent), static_cast<int>(j.status()));
    TEST_ASSERT_EQUAL_STRING("djPaths", cc::whyName(j.why()));
  }
}

// ---------------------------------------------------------------------------
// The slices: what one reads and hashes, the phases' order.
// ---------------------------------------------------------------------------
namespace {

// A library of `tracks` synthetic tracks, and a table whose rows are its
// tracks in a scrambled order, one card file each (every 50th row's a file
// the library doesn't have: a row without a track), BPM and key on most.
struct Scale {
  std::unique_ptr<LibraryIndex> idx;
  Bytes file;
  uint32_t rows = 0;
};

Scale scale(uint32_t tracks, uint32_t k = 169) {
  Scale s;
  s.idx.reset(new LibraryIndex());
  const synth::Spec spec = synth::userShape(tracks);
  TEST_ASSERT_TRUE(s.idx->begin(spec.root, tracks));
  TEST_ASSERT_EQUAL_UINT32(tracks, synth::addTracks(*s.idx, spec));
  TEST_ASSERT_TRUE(s.idx->finish());
  const uint32_t n = s.idx->trackCount();
  std::mt19937 rng(tracks);
  std::vector<uint32_t> order(n);
  for (uint32_t i = 0; i < n; ++i) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  RawTable raw;
  raw.k = k;
  raw.rows.resize(n);
  for (uint32_t r = 0; r < n; ++r) {
    RawTable::Row& row = raw.rows[r];
    row.bpm10 = rng() % 100 < 95 ? static_cast<uint16_t>(600 + rng() % 1200) : 0;
    row.camelot = rng() % 100 < 97 ? static_cast<uint8_t>(1 + rng() % 24) : 0;
    row.flags = rng() & 1;
    row.artistKey = rng();
    uint64_t h;
    TEST_ASSERT_TRUE(fullHash(*s.idx, order[r], &h));
    if (r % 50 == 49) h ^= 0x8000000000000000ull;  // a file the library doesn't have
    raw.paths.emplace_back(h, static_cast<uint16_t>(r));
  }
  std::sort(raw.paths.begin(), raw.paths.end());
  s.file = writeRaw(raw);
  s.rows = n;
  return s;
}

}  // namespace

void test_slice_budget() {
  Scale s = scale(6000, 4);
  File f(s.file);
  const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
  Join j;
  j.setPathHash(countingHash, nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, s.idx.get())));
  // begin() reads nothing.
  f.src.reads = 0;
  std::vector<Slice> log;
  g_hashCalls = 0;
  runJoin(j, s.idx.get(), &f.src, &log);
  assertMaps(j, e, "6000");
  uint32_t per[8] = {}, reading[8] = {};
  const Phase order[] = {Phase::Hash, Phase::Sort, Phase::Strings, Phase::Filters, Phase::Paths};
  size_t at = 0;
  for (const Slice& sl : log) {
    // The phases come in order, each in one stretch.
    while (at < 5 && order[at] != sl.phase) ++at;
    TEST_ASSERT_TRUE(at < 5);
    ++per[static_cast<int>(sl.phase)];
    reading[static_cast<int>(sl.phase)] += static_cast<uint32_t>(sl.reads);
    TEST_ASSERT_TRUE(sl.reads <= 1);
    TEST_ASSERT_TRUE(sl.most <= 4096);
    if (sl.phase == Phase::Hash) TEST_ASSERT_TRUE(sl.hashes <= 250);
    if (sl.phase == Phase::Hash || sl.phase == Phase::Sort) TEST_ASSERT_EQUAL_UINT64(0, sl.reads);
    // Paths' confirms: 100 a slice (an entry's own may pass it by a few).
    if (sl.phase != Phase::Hash) TEST_ASSERT_TRUE(sl.hashes <= 100 + 4);
  }
  const cc::Section& rf = f.table.container().section(mpdj::kSecFilters);
  const cc::Section& ph = f.table.container().section(mpdj::kSecPaths);
  TEST_ASSERT_EQUAL_UINT32(24, per[static_cast<int>(Phase::Hash)]);
  TEST_ASSERT_EQUAL_UINT32(2, per[static_cast<int>(Phase::Sort)]);  // runs of 4,096 and 1,904
  TEST_ASSERT_EQUAL_UINT32(1, per[static_cast<int>(Phase::Strings)]);
  TEST_ASSERT_TRUE(reading[static_cast<int>(Phase::Filters)] <= rf.bytes / 4096 + 2);
  TEST_ASSERT_TRUE(reading[static_cast<int>(Phase::Paths)] <= ph.bytes / 4096 + 2);
  // Paths' slices that go on in the buffer (the confirms' limit) read nothing.
  TEST_ASSERT_TRUE(per[static_cast<int>(Phase::Paths)] > reading[static_cast<int>(Phase::Paths)]);
  // Every slice that reads, reads a piece: the reads are the pieces, and the
  // join's own count.
  uint64_t readingSlices = 0;
  for (const Slice& sl : log) readingSlices += sl.reads;
  TEST_ASSERT_EQUAL_UINT64(readingSlices, f.src.reads);
  TEST_ASSERT_EQUAL_UINT64(f.src.reads, j.stats().reads);
  // Ready: a step does nothing more.
  const uint64_t reads = f.src.reads;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Ready), static_cast<int>(j.step(s.idx.get())));
  TEST_ASSERT_EQUAL_UINT64(reads, f.src.reads);
  TEST_ASSERT_TRUE(j.matches(s.idx.get()));
  TEST_ASSERT_FALSE(j.matches(nullptr));
  printf("[join] 6000 tracks, default slices: %u slices (hash %u, sort %u, strings %u, filters %u, paths %u of "
         "which %u read), %u reads\n",
         static_cast<unsigned>(log.size()), per[1], per[2], per[3], per[4], per[5], reading[5],
         static_cast<unsigned>(f.src.reads));

  // Two joins, a slice each in turn: each its own.
  Scale t = scale(1500, 4);
  File g(t.file);
  const Expect e2 = reference(*t.idx, g.table, byPath(*t.idx));
  Join a, b;
  a.setConfig(kTiny);
  a.begin(f.table, s.idx.get());
  b.begin(g.table, t.idx.get());
  while (a.status() == Status::Working || b.status() == Status::Working) {
    a.step(s.idx.get());
    b.step(t.idx.get());
  }
  assertMaps(a, e, "interleaved a");
  assertMaps(b, e2, "interleaved b");
}

// ---------------------------------------------------------------------------
// A fence and an index change at every slice boundary.
// ---------------------------------------------------------------------------
namespace {

struct Small {
  std::vector<std::string> files;
  Bytes file;
};

Small small() {
  Small s;
  char buf[96];
  std::vector<WriteRow> rows;
  for (int i = 0; i < 60; ++i) {
    std::snprintf(buf, sizeof(buf), "Generated Artist %02d/Sample Album %02d/%02d - Sample Title.mp3", i % 5, i % 9, i);
    const std::string p = buf;
    if (i % 7) s.files.push_back(p);
    WriteRow r{static_cast<uint8_t>(i + 1), unit(i), {cc::pathHash(p.c_str()), cc::pathHash((p + ".copy.mp3").c_str())},
               static_cast<uint16_t>(700 + 10 * i), static_cast<uint8_t>(i % 25), static_cast<uint8_t>(i & 1),
               static_cast<uint32_t>(i)};
    if (i % 3 == 0) s.files.push_back(p + ".copy.mp3");
    rows.push_back(r);
  }
  for (int i = 0; i < 20; ++i) {
    std::snprintf(buf, sizeof(buf), "New Artist/New Album/%02d - New.mp3", i);
    s.files.push_back(buf);
  }
  s.file = writeTable(rows);
  return s;
}

const Join::Config kSmallSlices = {16, 7, 1024};

}  // namespace

void test_fence_at_every_boundary() {
  const Small s = small();
  auto idx = libraryOf(s.files);
  File f(s.file);
  const Expect e = reference(*idx, f.table, byPath(*idx));
  Heap::reset();
  Join j(Heap::alloc, Heap::release);
  j.setConfig(kSmallSlices);
  j.begin(f.table, idx.get());
  std::vector<Slice> log;
  runJoin(j, idx.get(), nullptr, &log);
  assertMaps(j, e, "whole");
  const size_t slices = log.size();
  TEST_ASSERT_TRUE(slices > 20);
  for (size_t k = 0; k <= slices; ++k) {
    j.release();
    Heap::reset();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, idx.get())));
    runJoin(j, idx.get(), nullptr, nullptr, static_cast<uint32_t>(k));
    const size_t held = j.heldBytes();
    TEST_ASSERT_EQUAL_size_t(Heap::live, held);
    TEST_ASSERT_TRUE(held > 0);
    TEST_ASSERT_EQUAL_size_t(held, j.release());  // the fence's alsoFreed
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
    TEST_ASSERT_EQUAL_size_t(0, j.heldBytes());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Idle), static_cast<int>(j.status()));
    TEST_ASSERT_NULL(j.maps());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Idle), static_cast<int>(j.step(idx.get())));
    TEST_ASSERT_EQUAL_size_t(0, j.release());
    // After the fence: the join again, whole.
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
    assertMaps(j, e, "after a fence");
  }
  j.release();
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
}

void test_index_change_at_every_boundary() {
  const Small s = small();
  auto idx = libraryOf(s.files);
  auto other = libraryOf(s.files);  // the same files, another object
  std::vector<std::string> fewer = s.files;
  fewer.pop_back();
  // As many tracks, one named otherwise: only the build stamp tells.
  std::vector<std::string> renamed = s.files;
  renamed.back() = "New Artist/New Album/99 - Renamed.mp3";
  {
    auto r = libraryOf(renamed);
    TEST_ASSERT_EQUAL_UINT32(idx->trackCount(), r->trackCount());
    TEST_ASSERT_FALSE(idx->buildStamp() == r->buildStamp());
  }
  File f(s.file);
  const Expect e = reference(*idx, f.table, byPath(*idx));
  Heap::reset();
  Join j(Heap::alloc, Heap::release);
  j.setConfig(kSmallSlices);
  // No ready index: nothing begins.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Aborted), static_cast<int>(j.begin(f.table, nullptr)));
  LibraryIndex empty;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Aborted), static_cast<int>(j.begin(f.table, &empty)));
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
  j.begin(f.table, idx.get());
  std::vector<Slice> log;
  runJoin(j, idx.get(), nullptr, &log);
  const size_t slices = log.size();
  for (size_t k = 0; k < slices; ++k) {
    for (int how = 0; how < 5; ++how) {
      auto mine = libraryOf(s.files);  // rebuilt in place below
      j.release();
      Heap::reset();
      j.begin(f.table, mine.get());
      runJoin(j, mine.get(), nullptr, nullptr, static_cast<uint32_t>(k));
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.status()));
      const LibraryIndex* now = mine.get();
      if (how == 0) now = other.get();  // another index (equal, but not this one)
      if (how == 1) now = nullptr;      // behind the fence
      if (how == 2) rebuild(*mine, fewer);  // the same object, another library
      if (how == 3) rebuild(*mine, s.files);  // the same object, the same library: the same ids
      if (how == 4) rebuild(*mine, renamed);  // the same object and count, another library
      const Status st = j.step(now);
      if (how == 3) {
        runJoin(j, mine.get());
        assertMaps(j, reference(*mine, f.table, byPath(*mine)), "rebuilt the same");
        continue;
      }
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Aborted), static_cast<int>(st));
      TEST_ASSERT_EQUAL_size_t(0, Heap::live);
      TEST_ASSERT_EQUAL_size_t(0, j.heldBytes());
      TEST_ASSERT_NULL(j.maps());
      // It stays aborted, reading nothing, whatever it is given.
      const uint64_t reads = f.src.reads;
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Aborted), static_cast<int>(j.step(mine.get())));
      TEST_ASSERT_EQUAL_UINT64(reads, f.src.reads);
      // A new join on the index as it is now.
      j.begin(f.table, mine.get());
      runJoin(j, mine.get());
      assertMaps(j, reference(*mine, f.table, byPath(*mine)), "joined again");
    }
  }
  // Once Ready, the maps say which index they are for.
  j.begin(f.table, idx.get());
  runJoin(j, idx.get());
  assertMaps(j, e, "ready");
  TEST_ASSERT_TRUE(j.matches(idx.get()));
  TEST_ASSERT_FALSE(j.matches(other.get()));
  rebuild(*idx, fewer);
  TEST_ASSERT_FALSE(j.matches(idx.get()));
  j.release();
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
}

// ---------------------------------------------------------------------------
// Every allocation failing, and every read.
// ---------------------------------------------------------------------------
void test_no_memory_and_read_errors() {
  const Small s = small();
  auto idx = libraryOf(s.files);
  File f(s.file);
  const Expect e = reference(*idx, f.table, byPath(*idx));
  Heap::reset();
  {
    Join j(Heap::alloc, Heap::release);
    j.setConfig(kSmallSlices);
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
    assertMaps(j, e, "whole");
  }
  const size_t allocs = Heap::allocs;
  // The filters, rowToTrack, trackToRow, the runs' table, the runs, the
  // buffer, the folders' states.
  TEST_ASSERT_EQUAL_size_t(6 + (idx->trackCount() + 15) / 16, allocs);
  for (size_t k = 0; k < allocs; ++k) {
    Heap::reset();
    Heap::failAt = static_cast<long>(k);
    Join j(Heap::alloc, Heap::release);
    j.setConfig(kSmallSlices);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::NoMemory), static_cast<int>(j.begin(f.table, idx.get())));
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
    TEST_ASSERT_EQUAL_size_t(0, j.heldBytes());
    TEST_ASSERT_NULL(j.maps());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::NoMemory), static_cast<int>(j.step(idx.get())));
    Heap::failAt = -1;
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
    assertMaps(j, e, "after no memory");
  }

  // Reads: the join's, after open().
  f.src.reads = 0;
  {
    Join j;
    j.setConfig(kSmallSlices);
    j.begin(f.table, idx.get());
    runJoin(j, idx.get());
  }
  const uint64_t reads = f.src.reads;
  TEST_ASSERT_TRUE(reads >= 4);  // STRS, DJRF, DJPH's two pieces
  for (uint64_t k = 0; k <= reads; ++k) {
    File g(s.file);
    g.src.reads = 0;
    g.src.failFrom = static_cast<long>(k);
    Heap::reset();
    Join j(Heap::alloc, Heap::release);
    j.setConfig(kSmallSlices);
    j.begin(g.table, idx.get());
    runJoin(j, idx.get());
    if (k == reads) {
      assertMaps(j, e, "every read made");
      continue;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::ReadError), static_cast<int>(j.status()));
    TEST_ASSERT_EQUAL_STRING("io", cc::whyName(j.why()));
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
    TEST_ASSERT_NULL(j.maps());
  }
}

// ---------------------------------------------------------------------------
// The library's size: 65,534 tracks join, 65,535 don't.
// ---------------------------------------------------------------------------
void test_library_limit() {
  // One folder of tracks, named by number: the cheapest big index.
  auto build = [](uint32_t n) {
    std::unique_ptr<LibraryIndex> idx(new LibraryIndex());
    TEST_ASSERT_TRUE(idx->begin("/music", n));
    char p[64];
    for (uint32_t i = 0; i < n; ++i) {
      std::snprintf(p, sizeof(p), "/music/Generated Artist/Sample Album/%05u.mp3", static_cast<unsigned>(i));
      idx->addFile(p);
    }
    TEST_ASSERT_TRUE(idx->finish());
    TEST_ASSERT_EQUAL_UINT32(n, idx->trackCount());
    return idx;
  };
  RawTable raw;
  raw.rows.resize(3);
  raw.paths = {{cc::pathHash("Generated Artist/Sample Album/00007.mp3"), 0},
               {cc::pathHash("Generated Artist/Sample Album/65533.mp3"), 2}};
  std::sort(raw.paths.begin(), raw.paths.end());
  File f(writeRaw(raw));
  auto big = build(65535);
  Heap::reset();
  Join j(Heap::alloc, Heap::release);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::TooBig), static_cast<int>(j.begin(f.table, big.get())));
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
  big = build(65534);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, big.get())));
  runJoin(j, big.get());
  const uint32_t a = trackOf(*big, "Generated Artist/Sample Album/00007.mp3");
  const uint32_t b = trackOf(*big, "Generated Artist/Sample Album/65533.mp3");
  TEST_ASSERT_EQUAL_UINT16(0, j.maps()->trackToRow[a]);
  TEST_ASSERT_EQUAL_UINT16(2, j.maps()->trackToRow[b]);
  TEST_ASSERT_EQUAL_UINT16(b, j.maps()->rowToTrack[2]);
  TEST_ASSERT_EQUAL_UINT32(2, j.maps()->mapped);
  // Every block under 256 KB: the largest trackToRow's.
  TEST_ASSERT_EQUAL_size_t(65534u * 2u, Heap::largest);
  TEST_ASSERT_EQUAL_size_t(Heap::largest, j.stats().largestBlock);
  TEST_ASSERT_TRUE(Heap::largest < 256u * 1024u);
  printf("[join] 65,534 tracks: peak %u B, largest block %u B (trackToRow), %u slices\n",
         static_cast<unsigned>(j.stats().peakBytes), static_cast<unsigned>(j.stats().largestBlock),
         static_cast<unsigned>(j.stats().slices));
}

// ---------------------------------------------------------------------------
// The same file on the host FatFs model.
// ---------------------------------------------------------------------------
void test_fat_model() {
  using fatmodel::drive;
  Scale s = scale(3000);
  File mem(s.file);
  const Expect e = reference(*s.idx, mem.table, byPath(*s.idx));

  fatmodel::RamDisk disk(8u * 1024u * 1024u);  // 4 GB
  drive(0).disk = &disk;
  FATFS fs;
  TEST_ASSERT_EQUAL_INT(FR_OK, fatmodel::format(0, 16384));
  TEST_ASSERT_EQUAL_INT(FR_OK, fatmodel::mount(0, &fs));
  TEST_ASSERT_EQUAL_INT(FR_OK, f_mkdir("0:/.mstream"));
  {
    FIL out;
    TEST_ASSERT_EQUAL_INT(FR_OK, f_open(&out, "0:/.mstream/autodj-00000003.bin", FA_WRITE | FA_CREATE_ALWAYS));
    UINT put = 0;
    TEST_ASSERT_EQUAL_INT(FR_OK, f_write(&out, s.file.data(), static_cast<UINT>(s.file.size()), &put));
    TEST_ASSERT_EQUAL_UINT32(s.file.size(), put);
    TEST_ASSERT_EQUAL_INT(FR_OK, f_close(&out));
  }
  fatmodel::ModelFs mfs;
  tagstore::File* file = mfs.open("/.mstream/autodj-00000003.bin", tagstore::Fs::Mode::Read);
  TEST_ASSERT_NOT_NULL(file);
  CountingSource src(*file);
  mpdj::Table table;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(table.open(src)));
  Join j;
  j.begin(table, s.idx.get());
  src.reads = 0;
  drive(0).asked = fatmodel::Drive::Asked();
  uint64_t most = 0, singles = 0, sectors = 0, mostDisk = 0;
  uint32_t slices = 0;
  while (j.status() == Status::Working) {
    const fatmodel::Drive::Asked a0 = drive(0).asked;
    const uint64_t r0 = src.reads;
    src.most = 0;
    j.step(s.idx.get());
    ++slices;
    const fatmodel::Drive::Asked& a = drive(0).asked;
    TEST_ASSERT_TRUE(src.reads - r0 <= 1);
    TEST_ASSERT_TRUE(src.most <= 4096);
    most = std::max<uint64_t>(most, src.reads - r0);
    mostDisk = std::max<uint64_t>(mostDisk, a.reads - a0.reads);
    singles += a.singleReads - a0.singleReads;
    sectors += a.readSectors - a0.readSectors;
  }
  assertMaps(j, e, "the FatFs model");
  // A piece is whole sectors from a sector boundary: FatFs reads them
  // straight into the buffer, one command a cluster they touch (two at
  // most); the single sectors are a section's last, partial one and the
  // FAT's as the chain is followed.
  TEST_ASSERT_TRUE(mostDisk <= 4);
  printf("[join] FatFs model, 3,000 tracks: %u slices, %u pieces (at most 1 a slice), %u card reads of %u sectors "
         "(%u single), at most %u a slice\n",
         static_cast<unsigned>(slices), static_cast<unsigned>(src.reads), static_cast<unsigned>(drive(0).asked.reads),
         static_cast<unsigned>(sectors), static_cast<unsigned>(singles), static_cast<unsigned>(mostDisk));
  mfs.close(file);
  f_mount(nullptr, "0:", 0);
  drive(0).disk = nullptr;
  drive(0).asked = fatmodel::Drive::Asked();
}

// ---------------------------------------------------------------------------
// The memory and the slices at 20,000 tracks (and 22,000, 27,000) against
// the design's 3.3 and 3.4: maps 40 + 40 KB and filters 80 KB kept; the
// runs 120 KB and a 4 KB buffer while it runs; every block at most the
// filters' 80 KB; about 90 short slices of 4 KB reads.
// ---------------------------------------------------------------------------
void test_memory_at_scale() {
  const uint32_t sizes[] = {20000, 22000, 27000};
  for (uint32_t n : sizes) {
    Scale s = scale(n);
    File f(s.file);
    const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
    Heap::reset();
    Join j(Heap::alloc, Heap::release);
    j.setPathHash(countingHash, nullptr);
    j.begin(f.table, s.idx.get());
    // The runs: 6 bytes a pair, in blocks of 4,096 pairs (24,576 B) and
    // the rest.
    size_t full = 0, rest = 0;
    for (const auto& b : Heap::blocks()) {
      full += b.second == 4096u * 6u;
      rest += b.second == (n % 4096u) * 6u;
    }
    TEST_ASSERT_EQUAL_size_t(n / 4096, full);
    TEST_ASSERT_EQUAL_size_t(n % 4096 ? 1 : 0, rest);
    f.src.reads = 0;
    std::vector<Slice> log;
    std::vector<double> ms;
    g_hashCalls = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (j.status() == Status::Working) {
      const auto a = std::chrono::steady_clock::now();
      Slice sl;
      sl.phase = j.phase();
      const uint64_t r0 = f.src.reads, h0 = g_hashCalls;
      f.src.most = 0;
      j.step(s.idx.get());
      sl.reads = f.src.reads - r0;
      sl.most = f.src.most;
      sl.hashes = g_hashCalls - h0;
      log.push_back(sl);
      ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
    }
    const double total = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    char label[32];
    std::snprintf(label, sizeof(label), "%u tracks", static_cast<unsigned>(n));
    assertMaps(j, e, label);
    const size_t kept = static_cast<size_t>(n) * 2 + static_cast<size_t>(s.rows) * 6;
    const size_t runs = static_cast<size_t>(n) * 6;
    TEST_ASSERT_EQUAL_size_t(kept, j.heldBytes());
    TEST_ASSERT_EQUAL_size_t(kept, Heap::live);
    // The peak: the kept maps, the runs, the buffer and the runs' table.
    TEST_ASSERT_TRUE(Heap::peak >= kept + runs + 4096);
    TEST_ASSERT_TRUE(Heap::peak <= kept + runs + 4096 + 512);
    TEST_ASSERT_EQUAL_size_t(Heap::peak, j.stats().peakBytes);
    TEST_ASSERT_EQUAL_size_t(static_cast<size_t>(s.rows) * 4, Heap::largest);  // the filters
    if (n <= 20000) TEST_ASSERT_TRUE(Heap::largest <= 80u * 1024u);
    TEST_ASSERT_TRUE(Heap::largest < 256u * 1024u);
    uint32_t per[8] = {};
    uint64_t mostHash = 0, mostConfirm = 0;
    double mostMs = 0;
    for (size_t i = 0; i < log.size(); ++i) {
      ++per[static_cast<int>(log[i].phase)];
      TEST_ASSERT_TRUE(log[i].reads <= 1);
      TEST_ASSERT_TRUE(log[i].most <= 4096);
      if (log[i].phase == Phase::Hash) mostHash = std::max(mostHash, log[i].hashes);
      if (log[i].phase == Phase::Paths) mostConfirm = std::max(mostConfirm, log[i].hashes);
      mostMs = std::max(mostMs, ms[i]);
    }
    TEST_ASSERT_TRUE(mostHash <= 250);
    TEST_ASSERT_TRUE(mostConfirm <= 100 + 4);
    const uint32_t joinBytes = f.table.container().section(mpdj::kSecFilters).bytes +
                               f.table.container().section(mpdj::kSecPaths).bytes +
                               f.table.container().section(mpdj::kSecStrs).bytes;
    printf("[join] %u tracks, %u rows: peak %u B (kept %u: maps %u + %u, filters %u; runs %u in %u blocks of at "
           "most 24,576; buffer 4,096), largest block %u B; %u slices (hash %u, sort %u, strings %u, filters %u, "
           "paths %u), %u reads of at most 4,096 B over %u B; at most %u hashes and %u confirms a slice; host %.1f ms, "
           "slowest slice %.2f ms\n",
           static_cast<unsigned>(n), static_cast<unsigned>(s.rows), static_cast<unsigned>(Heap::peak),
           static_cast<unsigned>(kept), static_cast<unsigned>(n * 2), static_cast<unsigned>(s.rows * 2),
           static_cast<unsigned>(s.rows * 4), static_cast<unsigned>(runs), static_cast<unsigned>((n + 4095) / 4096),
           static_cast<unsigned>(Heap::largest), static_cast<unsigned>(log.size()), per[1], per[2], per[3], per[4],
           per[5], static_cast<unsigned>(f.src.reads), static_cast<unsigned>(joinBytes),
           static_cast<unsigned>(mostHash), static_cast<unsigned>(mostConfirm), total, mostMs);
    // The slices: ceil(n / 250) hashing, a run's sort each, one STRS,
    // and one 4 KB piece a reading slice for DJRF and DJPH.
    TEST_ASSERT_EQUAL_UINT32((n + 249) / 250, per[static_cast<int>(Phase::Hash)]);
    TEST_ASSERT_EQUAL_UINT32((n + 4095) / 4096, per[static_cast<int>(Phase::Sort)]);
    TEST_ASSERT_TRUE(f.src.reads <= joinBytes / 4096 + 4);
  }
}

// ---------------------------------------------------------------------------
// A loop pass's slices (stepPass()): as many as its clock allows, never a
// second card read; the join is the slice-by-slice one.
// ---------------------------------------------------------------------------
namespace {

struct SliceBudget {
  uint32_t left = 0;  // slices the pass may still take after this one
};
bool slicesLeft(void* ctx) {
  SliceBudget* b = static_cast<SliceBudget*>(ctx);
  if (b->left == 0) return false;
  --b->left;
  return true;
}

}  // namespace

void test_a_pass_of_slices() {
  Scale s = scale(6001, 4);
  File f(s.file);
  const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
  // The slices one at a time: the reference count.
  uint32_t slices = 0;
  {
    Join j;
    j.begin(f.table, s.idx.get());
    runJoin(j, s.idx.get());
    slices = j.stats().slices;
    assertMaps(j, e, "slice by slice");
  }
  // A pass with time for everything: one card read a pass ends it.
  {
    Join j;
    j.begin(f.table, s.idx.get());
    f.src.reads = 0;
    uint32_t passes = 0, noRead = 0;
    while (j.status() == Status::Working) {
      const uint64_t r0 = f.src.reads;
      const uint32_t s0 = j.stats().slices;
      j.stepPass(s.idx.get(), nullptr, nullptr);
      ++passes;
      TEST_ASSERT_TRUE(f.src.reads - r0 <= 1);
      TEST_ASSERT_TRUE(j.stats().slices > s0);
      if (f.src.reads == r0) ++noRead;
    }
    assertMaps(j, e, "a pass up to a read");
    TEST_ASSERT_EQUAL_UINT32(slices, j.stats().slices);
    // Every pass reads but the last, if its slices found the rest in the
    // buffer (the hashing and the sorts ride along with STRS's read).
    TEST_ASSERT_TRUE(noRead <= 1);
    TEST_ASSERT_EQUAL_UINT32(f.src.reads + noRead, passes);
    TEST_ASSERT_TRUE(passes * 2 < slices);
    printf("[join] 6,001 tracks: %u slices in %u passes of one read at most (%u reads)\n", (unsigned)slices,
           (unsigned)passes, (unsigned)f.src.reads);
  }
  // A pass's clock for three slices: the folders and the hashing take a
  // third of the passes, a read still ends one early.
  {
    Join j;
    j.begin(f.table, s.idx.get());
    f.src.reads = 0;
    uint32_t passes = 0, hashPasses = 0;
    while (j.status() == Status::Working) {
      const uint64_t r0 = f.src.reads;
      const uint32_t s0 = j.stats().slices;
      const bool hashing = j.phase() == Phase::Folders || j.phase() == Phase::Hash;
      SliceBudget b{2};
      j.stepPass(s.idx.get(), slicesLeft, &b);
      ++passes;
      hashPasses += hashing ? 1 : 0;
      TEST_ASSERT_TRUE(f.src.reads - r0 <= 1);
      TEST_ASSERT_TRUE(j.stats().slices - s0 <= 3);
    }
    assertMaps(j, e, "three slices a pass");
    TEST_ASSERT_EQUAL_UINT32(slices, j.stats().slices);
    // 25 slices of 250 hashes, after a slice of 250 folders each.
    const uint32_t folderSlices = (s.idx->folderCount() + 249) / 250;
    TEST_ASSERT_NOT_EQUAL(0, folderSlices);
    TEST_ASSERT_EQUAL_UINT32(s.idx->folderCount(), j.stats().folders);
    TEST_ASSERT_EQUAL_UINT32((folderSlices + 25 + 2) / 3, hashPasses);
  }
}

// ---------------------------------------------------------------------------
// The Folders phase: each folder's path hashed once, a track's hash going on
// from its folder's.
// ---------------------------------------------------------------------------
namespace {

bool viaTrackPath(const LibraryIndex& idx, uint32_t t, char* path, size_t size, uint64_t* h, void*) {
  return autodj::trackPathHash(idx, t, path, size, h);
}

// A file under `parent` whose whole path is `bytes` long.
std::string pathOf(const std::string& parent, size_t bytes) {
  const size_t used = parent.size() + 1 + 4;  // ('/' and ".mp3")
  return parent + "/" + std::string(bytes > used ? bytes - used : 1, 'n') + ".mp3";
}

}  // namespace

// Every track's hash is trackPathHash()'s, and none where it has none: the
// root "/music" in any case, alone or with more after it, other roots;
// whole paths just under, at and over kPathBytes; a folder whose own path
// is too long; folders deeper than LibraryIndex::folderPath() goes. The
// join with the folders' states maps as the hook's (each track's whole
// path) does, a table of the tracks' own hashes mapping every one with one.
void test_the_folders_hash() {
  const char* const roots[] = {"/music", "/MUSIC", "/Music", "/mUsIc/inner", "/musicx", "/sd", "/mus"};
  for (const char* root : roots) {
    const std::string r = root;
    std::vector<std::string> paths;
    char buf[96];
    for (int i = 0; i < 40; ++i) {
      snprintf(buf, sizeof(buf), "/Artist %d/Album %d/%02d - Title.mp3", i % 5, i % 7, i);
      paths.push_back(r + buf);
    }
    paths.push_back(r + "/Loose.mp3");      // the root's own
    paths.push_back(r + "/ic/Under.mp3");   // ("/mus" and "/ic": no "/music/")
    for (size_t bytes = 509; bytes <= 513; ++bytes) paths.push_back(pathOf(r + "/Long", bytes));
    const std::string wide = r + "/" + std::string(250, 'd') + "/" + std::string(240, 'e');
    paths.push_back(wide + "/x.mp3");                                // a long folder, the track too long
    paths.push_back(r + "/" + std::string(260, 'f') + "/" + std::string(260, 'g') + "/y.mp3");  // the folder too
    std::string deep = r;
    for (int d = 1; d <= 34; ++d) {
      deep += "/d" + std::to_string(d);
      if (d >= 30) paths.push_back(deep + "/z.mp3");  // depth 31 to 35 with the root
    }
    LibraryIndex idx;
    TEST_ASSERT_TRUE(idx.begin(root));
    for (const std::string& p : paths) idx.addFile(p.c_str());
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_TRUE_MESSAGE(idx.trackCount() > 40, root);
    // The table: every track with a hash, a row each, and a file of none.
    RawTable raw;
    uint32_t hashed = 0;
    char p[Join::kPathBytes];
    for (uint32_t t = 0; t < idx.trackCount(); ++t) {
      uint64_t h;
      if (!autodj::trackPathHash(idx, t, p, sizeof(p), &h)) continue;
      raw.rows.push_back(RawTable::Row{static_cast<uint16_t>(600 + t), 1, 0, t + 1});
      raw.paths.emplace_back(h, static_cast<uint16_t>(hashed++));
    }
    raw.rows.push_back(RawTable::Row{700, 2, 0, 9});
    raw.paths.emplace_back(0x0123456789ABCDEFull, static_cast<uint16_t>(hashed));
    std::sort(raw.paths.begin(), raw.paths.end());
    File f(writeRaw(raw));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", cc::whyName(f.open), root);
    Join folders, hook;
    hook.setPathHash(viaTrackPath, nullptr);
    folders.setConfig(Join::Config{4096, 7, 1024, 3, 5});  // (small slices: every boundary of the phase)
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(folders.begin(f.table, &idx)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Folders), static_cast<int>(folders.phase()));
    hook.begin(f.table, &idx);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Hash), static_cast<int>(hook.phase()));
    runJoin(folders, &idx);
    runJoin(hook, &idx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Ready), static_cast<int>(folders.status()), root);
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Ready), static_cast<int>(hook.status()), root);
    const autodj::Maps& a = *folders.maps();
    const autodj::Maps& b = *hook.maps();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(idx.folderCount(), folders.stats().folders, root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, hook.stats().folders, root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(hashed, a.mapped, root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(hashed, b.mapped, root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(idx.trackCount() - hashed, folders.stats().unhashed, root);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(hook.stats().unhashed, folders.stats().unhashed, root);
    TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(b.trackToRow, a.trackToRow, a.tracks, root);
    TEST_ASSERT_EQUAL_HEX16_ARRAY_MESSAGE(b.rowToTrack, a.rowToTrack, a.rows, root);
    TEST_ASSERT_EQUAL_HEX32_ARRAY_MESSAGE(b.filters, a.filters, a.rows, root);
    // (The cases without a hash are there: the long paths, the deep folders.)
    TEST_ASSERT_TRUE_MESSAGE(folders.stats().unhashed >= 5, root);
    printf("[join] the folders' states, root \"%s\": %u tracks in %u folders, %u hashed, the same maps as each "
           "track's whole path\n",
           root, (unsigned)idx.trackCount(), (unsigned)idx.folderCount(), (unsigned)hashed);
  }
  // A library of more folders than kMaxFolders: each track's whole path.
  {
    LibraryIndex idx;
    TEST_ASSERT_TRUE(idx.begin("/music"));
    char buf[64];
    for (uint32_t i = 0; i <= Join::kMaxFolders; ++i) {
      snprintf(buf, sizeof(buf), "/music/f%u/t.mp3", (unsigned)i);
      idx.addFile(buf);
    }
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_TRUE(idx.folderCount() > Join::kMaxFolders);
    RawTable raw;
    raw.rows.push_back(RawTable::Row{700, 2, 0, 9});
    raw.paths.emplace_back(1, 0);
    File f(writeRaw(raw));
    Join j;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, &idx)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Phase::Hash), static_cast<int>(j.phase()));
    runJoin(j, &idx);
    TEST_ASSERT_EQUAL_UINT32(0, j.stats().folders);
    TEST_ASSERT_EQUAL_UINT32(idx.trackCount(), j.stats().hashed);
  }
}

// ---------------------------------------------------------------------------
// The placement (setPlacement()) in a model of the device's heap.
// ---------------------------------------------------------------------------
namespace {

// A heap that serves a request as ESP-IDF's TLSF does (components/heap/
// tlsf, the IDF's heap for PSRAM): 4-byte alignment and a 4-byte header a
// block; free lists by size class, 32 classes a power of two above 128 B;
// a request rounded up to its class's next boundary (mapping_search()),
// then the head of the first non-empty list at or above it, carved from
// that block's low end (block_trim_free()); a freed block merges with its
// free neighbours and goes to its list's head; realloc to a smaller size
// trims in place (block_trim_used()). Real memory (the join writes its
// maps, LibraryIndex its strings). Its free bytes counted as it goes, their
// lowest since mark(); the `failAt`-th allocation since reset() fails.
struct ModelHeap {
  struct Block {
    size_t size;  // usable bytes: the block's footprint is kHeader more
    bool used;
  };
  static constexpr size_t kAlign = 4, kHeader = 4, kMin = 12, kSplit = 16, kSmall = 128;
  static constexpr int kSlLog2 = 5;
  static inline std::vector<uint8_t> mem;
  static inline std::map<size_t, Block> blocks;  // footprint's offset: the block
  static inline std::map<std::pair<int, int>, std::vector<size_t>> lists;  // (fl, sl): offsets, the head last
  static inline size_t freeNow = 0, low = 0, allocs = 0;
  static inline long failAt = -1;

  static int fls(size_t v) {
    int r = -1;
    for (; v; v >>= 1) ++r;
    return r;
  }
  static std::pair<int, int> classOf(size_t size) {
    if (size < kSmall) return {0, static_cast<int>(size / (kSmall / 32))};
    const int f = fls(size);
    return {f - (kSlLog2 + 1), static_cast<int>((size >> (f - kSlLog2)) ^ (1u << kSlLog2))};
  }
  static void list(size_t off) { lists[classOf(blocks[off].size)].push_back(off); }
  static void unlist(size_t off) {
    std::vector<size_t>& l = lists[classOf(blocks[off].size)];
    l.erase(std::find(l.begin(), l.end(), off));
  }
  static void reset(size_t bytes) {
    mem.assign(bytes, 0);
    blocks.clear();
    lists.clear();
    blocks[0] = {bytes - kHeader, false};
    list(0);
    freeNow = low = bytes - kHeader;
    allocs = 0;
    failAt = -1;
  }
  static void* alloc(size_t n) {
    if (failAt >= 0 && static_cast<long>(allocs++) == failAt) return nullptr;
    if (n == 0) return nullptr;
    const size_t a = std::max(kMin, (n + kAlign - 1) & ~(kAlign - 1));
    const size_t want = a < kSmall ? a : a + (static_cast<size_t>(1) << (fls(a) - kSlLog2)) - 1;
    auto it = lists.lower_bound(classOf(want));
    while (it != lists.end() && it->second.empty()) ++it;
    if (it == lists.end()) return nullptr;
    const size_t off = it->second.back();
    unlist(off);
    Block& b = blocks[off];
    if (b.size >= kSplit + a) {
      blocks[off + kHeader + a] = {b.size - a - kHeader, false};
      list(off + kHeader + a);
      b.size = a;
      freeNow -= a + kHeader;
    } else {
      freeNow -= b.size;
    }
    b.used = true;
    low = std::min(low, freeNow);
    return mem.data() + off + kHeader;
  }
  static size_t offsetOf(const void* p) {
    return static_cast<size_t>(static_cast<const uint8_t*>(p) - mem.data()) - kHeader;
  }
  static void release(void* p) {
    if (!p) return;
    auto it = blocks.find(offsetOf(p));
    TEST_ASSERT_TRUE_MESSAGE(it != blocks.end() && it->second.used, "not a block of the model's");
    it->second.used = false;
    freeNow += it->second.size;
    auto next = std::next(it);
    if (next != blocks.end() && !next->second.used) {
      unlist(next->first);
      it->second.size += kHeader + next->second.size;
      freeNow += kHeader;
      blocks.erase(next);
    }
    if (it != blocks.begin()) {
      auto prev = std::prev(it);
      if (!prev->second.used) {
        unlist(prev->first);
        prev->second.size += kHeader + it->second.size;
        freeNow += kHeader;
        blocks.erase(it);
        it = prev;
      }
    }
    list(it->first);
  }
  static void* shrink(void* p, size_t n) {
    auto it = blocks.find(offsetOf(p));
    TEST_ASSERT_TRUE_MESSAGE(it != blocks.end() && it->second.used, "not a block of the model's");
    const size_t a = std::max(kMin, (n + kAlign - 1) & ~(kAlign - 1));
    if (a > it->second.size) return nullptr;
    if (it->second.size >= kSplit + a) {
      const size_t tail = it->first + kHeader + a;
      blocks[tail] = {it->second.size - a - kHeader, true};
      it->second.size = a;
      release(mem.data() + tail + kHeader);
    }
    return p;
  }
  static size_t largest() {
    size_t m = 0;
    for (const auto& b : blocks)
      if (!b.second.used) m = std::max(m, b.second.size);
    return m;
  }
  static size_t freeBytes() { return freeNow; }
  static void mark() { low = freeNow; }
  // The free blocks of `min` bytes or more, by offset.
  static std::vector<std::pair<size_t, size_t>> freeBlocks(size_t min) {
    std::vector<std::pair<size_t, size_t>> v;
    for (const auto& b : blocks)
      if (!b.second.used && b.second.size >= min) v.emplace_back(b.first, b.second.size);
    return v;
  }
};

constexpr size_t kMargin = 64 * 1024;  // the firmware's (app/AutoDj.cpp's kJoinMarginBytes)

Join::Placement modelPlacement(size_t margin = kMargin) {
  Join::Placement p;
  p.largest = ModelHeap::largest;
  p.freeBytes = ModelHeap::freeBytes;
  p.margin = margin;
  return p;
}

// begin()'s blocks in the order they are taken, without a placement.
std::string g_order;
uint32_t g_rows = 0, g_tracks = 0, g_folders = 0;
void* orderAlloc(size_t n) {
  void* p = Heap::alloc(n);
  if (!p) return nullptr;
  char c = '?';
  if (n == static_cast<size_t>(g_rows) * 4) {
    c = 'F';  // the filters
  } else if (n == static_cast<size_t>(g_rows) * 2 || n == static_cast<size_t>(g_tracks) * 2) {
    c = 'M';  // rowToTrack, trackToRow
  } else if (n == 4096) {
    c = 'B';  // the stream's buffer
  } else if (n % 6 == 0 && n >= 6 * 1000) {
    c = 'R';  // a run
  } else if (n == static_cast<size_t>(g_folders) * 10) {
    c = 'D';  // the folders' states
  } else if (n < 1024) {
    c = 'T';  // the runs' table
  }
  g_order += c;
  return p;
}

// Where a join's blocks are in the model heap (offsets of their footprints).
struct Layout {
  size_t filters = 0, rowToTrack = 0, trackToRow = 0;
};
Layout layoutOf(const Join& j) {
  const autodj::Maps* m = j.maps();
  Layout l;
  l.filters = ModelHeap::offsetOf(m->filters);
  l.rowToTrack = ModelHeap::offsetOf(m->rowToTrack);
  if (m->trackToRow) l.trackToRow = ModelHeap::offsetOf(m->trackToRow);
  return l;
}

}  // namespace

// Without a placement the kept blocks come first, as ever. In a heap of one
// free block every block goes to its top: the runs, then the maps above
// them, the filters lowest, a few KB under the heap's end; the runs' hole
// joins the free part below once the join is done, and giving it back
// leaves the one block. A library whose maps are under the margin: the
// rest of the margin above them. Holes that hold the maps and the runs take
// them (the largest block untouched); every allocation failing in turn
// leaves the heap as it was.
void test_the_placement_s_order() {
  Scale s = scale(6001, 4);  // (6,001: no map's size a multiple of 6)
  File f(s.file);
  const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
  g_rows = s.rows;
  g_tracks = s.idx->trackCount();
  g_folders = s.idx->folderCount();
  {
    Heap::reset();
    g_order.clear();
    Join j(orderAlloc, Heap::release);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, s.idx.get())));
    TEST_ASSERT_EQUAL_STRING("FMMTRRBD", g_order.c_str());
    TEST_ASSERT_FALSE(j.stats().placed);
    runJoin(j, s.idx.get());
    assertMaps(j, e, "no placement");
  }
  // One free block: everything at its top, at 20,000 tracks (the maps over
  // the margin) and at 6,001 (under it).
  Scale big = scale(20000, 4);
  File bf(big.file);
  const Expect be = reference(*big.idx, bf.table, byPath(*big.idx));
  const size_t heap = 1u << 20;
  for (int lib = 0; lib < 2; ++lib) {
    const LibraryIndex& idx = lib ? *s.idx : *big.idx;
    File& file = lib ? f : bf;
    const uint32_t rows = file.table.rowCount(), tracks = idx.trackCount();
    const size_t kept = static_cast<size_t>(rows) * 6 + static_cast<size_t>(tracks) * 2;
    // (the runs, the buffer, the folders' states)
    const size_t folders = idx.folderCount() <= Join::kMaxFolders ? static_cast<size_t>(idx.folderCount()) * 10 : 0;
    const size_t transient = static_cast<size_t>(tracks) * 6 + 4096 + folders;
    ModelHeap::reset(heap);
    Join j(ModelHeap::alloc, ModelHeap::release);
    j.setPlacement(modelPlacement());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(file.table, &idx)));
    const Join::Stats& st = j.stats();
    TEST_ASSERT_TRUE(st.placed);
    TEST_ASSERT_EQUAL_UINT8(0, st.keptElsewhere);
    TEST_ASSERT_EQUAL_UINT32(0, st.runsElsewhere);
    TEST_ASSERT_EQUAL_size_t(kept + transient, st.topBytes);
    TEST_ASSERT_EQUAL_size_t(kept, st.topKept);
    TEST_ASSERT_TRUE(st.lowestFree >= kMargin);
    TEST_ASSERT_EQUAL_size_t(ModelHeap::low, st.lowestFree);
    // The runs below the filters, the free part below them.
    const size_t above = kept >= kMargin ? 0 : kMargin - kept;  // (the margin the maps don't make)
    TEST_ASSERT_EQUAL_size_t(1, ModelHeap::freeBlocks(above + 32 * 1024).size());
    runJoin(j, &idx);
    assertMaps(j, lib ? e : be, "one block");
    const Layout l = layoutOf(j);
    TEST_ASSERT_TRUE(l.filters < l.rowToTrack && l.rowToTrack < l.trackToRow);
    const size_t end = l.trackToRow + ModelHeap::kHeader + static_cast<size_t>(tracks) * 2;
    TEST_ASSERT_TRUE(end <= heap && heap - end < above + 24 * 1024);
    // Done: the runs' hole joined the free part below the maps.
    const auto below = ModelHeap::freeBlocks(above + 32 * 1024);
    TEST_ASSERT_EQUAL_size_t(1, below.size());
    TEST_ASSERT_EQUAL_size_t(l.filters, below[0].first + ModelHeap::kHeader + below[0].second);
    j.release();
    TEST_ASSERT_EQUAL_size_t(heap - ModelHeap::kHeader, ModelHeap::largest());
  }
  // Holes that hold the maps and the transient blocks (32 KB, the
  // filters' 24 KB among them): the largest block keeps what it had.
  ModelHeap::reset(heap);
  std::vector<void*> fill;
  for (int i = 0; i < 12; ++i) {
    fill.push_back(ModelHeap::alloc(32 * 1024));
    fill.push_back(ModelHeap::alloc(64));
  }
  for (int i = 0; i < 12; i += 2) ModelHeap::release(fill[i]);
  {
    const size_t largest = ModelHeap::largest();
    Join j(ModelHeap::alloc, ModelHeap::release);
    j.setPlacement(modelPlacement());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, s.idx.get())));
    const Join::Stats& st = j.stats();
    TEST_ASSERT_EQUAL_UINT8(3, st.keptElsewhere);
    TEST_ASSERT_EQUAL_size_t(0, st.topKept);
    TEST_ASSERT_EQUAL_UINT32(4, st.runsElsewhere);  // (two runs, the folders' states, the buffer)
    TEST_ASSERT_EQUAL_size_t(0, st.topBytes);
    TEST_ASSERT_FALSE(st.placed);  // (no spacer: nothing for the top)
    TEST_ASSERT_EQUAL_size_t(largest, ModelHeap::largest());
    runJoin(j, s.idx.get());
    assertMaps(j, e, "holes");
    TEST_ASSERT_EQUAL_size_t(largest, ModelHeap::largest());
  }
  // Every allocation failing in turn: a block's, NoMemory, nothing held; a
  // spacer's, the blocks where the heap puts them (a join as any other);
  // the heap whole again either way.
  int noMemory = 0, unplaced = 0;
  for (long k = 0;; ++k) {
    ModelHeap::reset(heap);
    ModelHeap::failAt = k;
    Join j(ModelHeap::alloc, ModelHeap::release);
    j.setPlacement(modelPlacement());
    const Status st = j.begin(f.table, s.idx.get());
    const bool failed = ModelHeap::allocs > static_cast<size_t>(k);  // (the k-th was asked for)
    if (st == Status::NoMemory) {
      ++noMemory;
      TEST_ASSERT_EQUAL_size_t(0, j.heldBytes());
    } else {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(st));
      unplaced += failed && !j.stats().placed ? 1 : 0;
      runJoin(j, s.idx.get());
      assertMaps(j, e, "an allocation failing");
      j.release();
    }
    TEST_ASSERT_EQUAL_size_t(heap - ModelHeap::kHeader, ModelHeap::largest());
    if (!failed) {
      TEST_ASSERT_TRUE(j.stats().placed || st != Status::Working);
      break;
    }
  }
  TEST_ASSERT_TRUE(noMemory >= 6);  // (the runs' table, the probes, the blocks on top)
  TEST_ASSERT_TRUE(unplaced >= 1);  // (a spacer's)
}

// The spacers never leave less than the margin free (the device's 2026-10-14
// check: free fell to 10 KB at every join), whatever the heap's shape: the
// join's own blocks only take what they hold. The maps taken after the
// spacers keep most of it at 20,000 tracks whatever the margin.
void test_free_never_under_the_margin() {
  Scale big = scale(20000, 4);
  Scale small = scale(6001, 4);
  File bf(big.file), sf(small.file);
  struct Case {
    size_t heap, hole, holes;
  };
  const Case cases[] = {{1000000, 0, 0}, {800000, 4000, 8}, {1500000, 30000, 8}, {700000, 20000, 4}, {420000, 0, 0}};
  for (int lib = 0; lib < 2; ++lib) {
    const LibraryIndex& idx = lib ? *small.idx : *big.idx;
    File& f = lib ? sf : bf;
    for (const Case& c : cases) {
      for (size_t margin : {size_t(128 * 1024), kMargin, size_t(0)}) {
        ModelHeap::reset(c.heap);
        std::vector<void*> fill;
        for (size_t i = 0; i < 2 * c.holes; ++i) {
          fill.push_back(ModelHeap::alloc(c.hole));
          fill.push_back(ModelHeap::alloc(64));
        }
        for (size_t i = 0; i < fill.size(); i += 4) ModelHeap::release(fill[i]);
        const size_t free0 = ModelHeap::freeBytes();
        ModelHeap::mark();
        Join j(ModelHeap::alloc, ModelHeap::release);
        j.setPlacement(modelPlacement(margin));
        const Status st = j.begin(f.table, &idx);
        char what[112];
        snprintf(what, sizeof(what), "%u tracks, a heap of %u B, %u holes of %u B, margin %u B",
                 (unsigned)idx.trackCount(), (unsigned)c.heap, (unsigned)c.holes, (unsigned)c.hole, (unsigned)margin);
        if (st != Status::Working) {
          TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::NoMemory), static_cast<int>(st), what);
          continue;
        }
        // What the join holds is its own; the spacers keep the margin beside it.
        const size_t own = j.stats().peakBytes + 64;
        const size_t floor = std::min(margin, free0 > own ? free0 - own : 0);
        TEST_ASSERT_TRUE_MESSAGE(ModelHeap::low + 64 >= floor, what);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(ModelHeap::low, j.stats().lowestFree, what);
        // The small library's maps (48 KB) under the margin: without it the
        // spacers leave little more than them.
        if (lib && margin == 0 && c.holes == 0) TEST_ASSERT_TRUE_MESSAGE(ModelHeap::low < 64 * 1024, what);
        printf("[join] %s: free %u B, its lowest while placing %u B (%s)\n", what, (unsigned)free0,
               (unsigned)ModelHeap::low, j.stats().placed ? "placed" : "where the heap put it");
      }
    }
  }
}

// The placement under a model of the device's heap (problem 3 of the
// device checks, 2026-10-13): the first boot of the 19k card joined at
// once, the card's walk carved its buffers while the join ran, and a
// library update then needed a track table a little bigger than the old
// one's block, so a free block of 17/16 of it. With the join's blocks
// where the heap put them (low in the largest block) giving them back
// doesn't help; at that block's top, giving them back makes the block Auto
// DJ off would have left, and the update's memory check passes as it
// would have.
namespace {

enum class Dj { Off, Placed, WhereThe };
struct Boot {
  size_t largestAtJoin = 0;  // before the join's blocks
  size_t largestJoined = 0;  // the join ready, the walk's blocks taken
  size_t givenBack = 0;      // ... and the join given back
  size_t joinBytes = 0;
  size_t lowest = 0;         // the lowest free while begin() placed its blocks
  bool placed = false;
};

// The boot's heap: the library's blocks, then smaller ones with 8 holes of
// `hole` bytes between them, then the free tail; the walk's 4 blocks of
// `walk` bytes.
struct Shape {
  size_t heap, hole, walk;
};

Boot firstBoot(Dj dj, const Shape& sh, File& f, const LibraryIndex& idx, const Expect& e) {
  Boot b;
  ModelHeap::reset(sh.heap);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(640000));
  std::vector<void*> small;
  for (int i = 0; i < 16; ++i) small.push_back(ModelHeap::alloc(sh.hole));
  for (int i = 0; i < 16; i += 2) ModelHeap::release(small[i]);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(20000));
  b.largestAtJoin = ModelHeap::largest();
  Join j(ModelHeap::alloc, ModelHeap::release);
  if (dj == Dj::Placed) j.setPlacement(modelPlacement());
  if (dj != Dj::Off) {
    ModelHeap::mark();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, &idx)));
    b.lowest = ModelHeap::low;
    b.placed = j.stats().placed;
    runJoin(j, &idx, nullptr, nullptr, 20);  // (the walk begins 2 s after the UI's first frame)
  }
  // The card's walk and scan: four blocks, while the join runs.
  for (int i = 0; i < 4; ++i) TEST_ASSERT_NOT_NULL(ModelHeap::alloc(sh.walk));
  if (dj != Dj::Off) {
    runJoin(j, &idx);
    assertMaps(j, e, "the model heap");
    b.joinBytes = j.heldBytes();
  }
  b.largestJoined = ModelHeap::largest();
  j.release();
  b.givenBack = ModelHeap::largest();
  return b;
}

}  // namespace

void test_the_placement_in_a_model_heap() {
  Scale s = scale(20000, 4);
  File f(s.file);
  const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
  // Small holes (the runs come from the largest block), then holes the
  // runs fit (they go there, and the maps still to the top).
  const Shape shapes[] = {{1500000, 4000, 30000}, {1900000, 30000, 35000}};
  Boot off, placed, heaps;
  for (const Shape& sh : shapes) {
    off = firstBoot(Dj::Off, sh, f, *s.idx, e);
    placed = firstBoot(Dj::Placed, sh, f, *s.idx, e);
    heaps = firstBoot(Dj::WhereThe, sh, f, *s.idx, e);
    printf("[join] the model heap, holes of %u B, 20,000 tracks (the join %u B kept): the largest free block %u B at "
           "the join; then with the walk's 4 x %u B: Auto DJ off %u B; joined at the top %u B, given back %u B "
           "(free at least %u B while placing); joined where the heap puts it %u B, given back %u B\n",
           (unsigned)sh.hole, (unsigned)placed.joinBytes, (unsigned)off.largestAtJoin, (unsigned)sh.walk,
           (unsigned)off.givenBack, (unsigned)placed.largestJoined, (unsigned)placed.givenBack,
           (unsigned)placed.lowest, (unsigned)heaps.largestJoined, (unsigned)heaps.givenBack);
    TEST_ASSERT_TRUE(placed.placed);
    TEST_ASSERT_FALSE(heaps.placed);
    TEST_ASSERT_TRUE(placed.lowest >= kMargin);
    // Off: the walk's blocks from the largest block's low end.
    TEST_ASSERT_TRUE(off.givenBack + 4 * sh.walk <= off.largestAtJoin);
    TEST_ASSERT_TRUE(off.givenBack + 4 * sh.walk + 4 * 16 >= off.largestAtJoin);
    // At the top: the maps take the block's top (less than 24 KB above
    // them), the walk its bottom, and the join given back leaves the very
    // block Auto DJ off leaves.
    TEST_ASSERT_TRUE(placed.largestJoined <= off.givenBack - placed.joinBytes);
    TEST_ASSERT_TRUE(placed.largestJoined + placed.joinBytes + 24 * 1024 >= off.givenBack);
    TEST_ASSERT_EQUAL_size_t(off.givenBack, placed.givenBack);
    // Where the heap puts it: the maps and the walk's blocks low in the
    // block; given back, the block is still short by more than the runs.
    TEST_ASSERT_TRUE(heaps.givenBack + 100000 < off.givenBack);
  }

  // The update's memory check (LibraryUpdate::roomToBuild()): a new track
  // table a little over the old one's block, which fits the largest block
  // Auto DJ off leaves, with its sixteenth.
  LibraryUpdate::Room r;
  r.psramFree = 64u << 20;
  r.indexBytes = 1000000;
  r.tableBytes = (off.givenBack - 64) * 16 / 17;
  r.trackBlock = r.tableBytes - 64;
  r.psramLargest = off.givenBack;
  TEST_ASSERT_TRUE(LibraryUpdate::roomToBuild(r).shortOf == LibraryUpdate::Short::None);
  r.psramLargest = placed.largestJoined;  // joined: short (setGiveBack()'s case)
  TEST_ASSERT_TRUE(LibraryUpdate::roomToBuild(r).shortOf == LibraryUpdate::Short::Table);
  r.psramLargest = placed.givenBack;      // given back: as Auto DJ off
  TEST_ASSERT_TRUE(LibraryUpdate::roomToBuild(r).shortOf == LibraryUpdate::Short::None);
  r.psramLargest = heaps.givenBack;       // (unplaced, given back: still short)
  TEST_ASSERT_TRUE(LibraryUpdate::roomToBuild(r).shortOf == LibraryUpdate::Short::Table);

  // The spacers themselves: one from the largest block, one more on what
  // it left; a heap with nothing to space takes none; none leaves less than
  // keepFree free.
  ModelHeap::reset(1000000);
  autodj::Spacers sp;
  TEST_ASSERT_TRUE(autodj::takeSpacers(300000, 0, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                       ModelHeap::release, &sp));
  TEST_ASSERT_TRUE(sp.n >= 1 && sp.n <= autodj::Spacers::kMax);
  const size_t top = ModelHeap::largest();
  TEST_ASSERT_TRUE(top >= 300000 + 300000 / 32);
  TEST_ASSERT_TRUE(top <= 300000 + 300000 / 32 + 1024 + top / 32 + 4096 + 8);
  autodj::giveSpacers(&sp, ModelHeap::release);
  TEST_ASSERT_EQUAL_INT(0, sp.n);
  TEST_ASSERT_EQUAL_size_t(1000000 - ModelHeap::kHeader, ModelHeap::largest());
  TEST_ASSERT_FALSE(autodj::takeSpacers(990000, 0, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                        ModelHeap::release, &sp));
  TEST_ASSERT_EQUAL_INT(0, sp.n);
  TEST_ASSERT_EQUAL_size_t(0, autodj::spacerFor(300000 + 300000 / 32 + 1024 + 4096, 300000));
  TEST_ASSERT_TRUE(autodj::takeSpacers(300000, 600000, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                       ModelHeap::release, &sp));
  TEST_ASSERT_TRUE(ModelHeap::freeBytes() >= 600000);
  TEST_ASSERT_TRUE(ModelHeap::largest() >= 600000 - 64);  // (what keepFree leaves, at the block's top)
  autodj::giveSpacers(&sp, ModelHeap::release);
  TEST_ASSERT_FALSE(autodj::takeSpacers(300000, 999000, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                        ModelHeap::release, &sp));
  TEST_ASSERT_EQUAL_INT(0, sp.n);
}

// A library update in a model of the device's heap (the device's
// 2026-10-14 check: after each gb the join sat low in PSRAM and the largest
// free block was 180,212 B, against 622,580 B before). The real library at
// 19,412 tracks: library.idx loaded at the boot (its strings one block),
// the firmware's other blocks at their measured sizes around it, Thumbs'
// pools among the UI's; then update steps as main.cpp runs them (the
// fence's frees, the build from D's records, the Live step's queue,
// durations and Thumbs' pools, the join again). Auto DJ off: the rebuild
// alone leaves PSRAM more fragmented than the boot (its working blocks and
// 64 KB string chunks across the regions the old index and Thumbs gave
// back). Auto DJ on: the fence's frees leave the heap as off does; the
// join again takes no more of the largest free block than the blocks no
// smaller one holds (the filters), and given back it is off's; free never
// under the margin.
namespace {

struct UpdateRun {
  size_t bootLargest = 0;      // the boot's end, before the join
  size_t bootJoined = 0;       // ... joined
  size_t bootTopKept = 0;      // ... its kept bytes at the top
  std::vector<std::pair<size_t, size_t>> fence;  // the free blocks at the fence (64 KB or more)
  size_t largest = 0;          // after the update, before the join
  size_t joined = 0;           // ... joined again
  size_t givenBack = 0;        // ... and given back
  size_t topKept = 0;          // the kept bytes the join put at the top
  size_t lowest = SIZE_MAX;    // the lowest free while a join placed its blocks
};

// A table whose rows are `idx`'s tracks, scrambled, `rows` of them (one
// card file each; scale()'s values).
Bytes tableOf(const LibraryIndex& idx, uint32_t rows) {
  std::mt19937 rng(rows);
  std::vector<uint32_t> order(idx.trackCount());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::shuffle(order.begin(), order.end(), rng);
  RawTable raw;
  raw.rows.resize(rows);
  for (uint32_t r = 0; r < rows; ++r) {
    RawTable::Row& row = raw.rows[r];
    row.bpm10 = static_cast<uint16_t>(600 + rng() % 1200);
    row.camelot = static_cast<uint8_t>(1 + rng() % 24);
    row.artistKey = rng();
    uint64_t h;
    TEST_ASSERT_TRUE(fullHash(idx, order[r], &h));
    raw.paths.emplace_back(h, static_cast<uint16_t>(r));
  }
  std::sort(raw.paths.begin(), raw.paths.end());
  return writeRaw(raw);
}

void* g_durations = nullptr;

UpdateRun libraryUpdates(Dj dj, const std::vector<uint8_t>& saved, cc::Source& records, const synthcard::Card& card,
                         int steps) {
  UpdateRun u;
  ModelHeap::reset(4089u * 1024u);
  auto take = [](std::initializer_list<size_t> sizes, std::vector<void*>* into) {
    for (size_t s : sizes) {
      void* p = ModelHeap::alloc(s);
      TEST_ASSERT_NOT_NULL(p);
      if (into) into->push_back(p);
    }
  };
  // The boot's stages ([heap] lines): storage, audio, the library, the
  // dance, the UI.
  take({135168, 4148, 32768, 16384, 4096, 4096}, nullptr);
  take({38048, 7776, 4992, 262144, 131072, 65536, 32768, 11648}, nullptr);
  LibraryIndex index(ModelHeap::alloc, ModelHeap::release, ModelHeap::shrink);
  index.keepTrackBlock(true);
  {
    struct Src : ByteSource {
      const std::vector<uint8_t>& b;
      size_t at = 0;
      explicit Src(const std::vector<uint8_t>& v) : b(v) {}
      size_t read(void* out, size_t n) override {
        const size_t k = std::min(n, b.size() - at);
        std::memcpy(out, b.data() + at, k);
        at += k;
        return k;
      }
    } src(saved);
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(index.load(src, in) == LibraryIndex::Load::Loaded);
  }
  std::vector<void*> queue, thumbs;
  take({5120}, &queue);
  g_durations = ModelHeap::alloc(index.trackCount() * 2);
  take({3680, 3584, 15432, 4148}, nullptr);  // Auto DJ's glue, its engine, the open file
  take({40960, 9216}, nullptr);              // the dance
  take({153600, 76800, 65536}, nullptr);     // the UI, Thumbs' pools among its blocks
  take({204800, 256, 256, 110592, 24, 24}, &thumbs);
  take({40960, 32768, 20480, 16384, 12288, 8192}, nullptr);
  for (int i = 0; i < 64; ++i) take({263}, nullptr);
  u.bootLargest = ModelHeap::largest();

  const Bytes file = tableOf(index, index.trackCount() - 2);
  cc::MemSource src(file.data(), static_cast<uint32_t>(file.size()));
  mpdj::Table table;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(table.open(src)));
  Join join(ModelHeap::alloc, ModelHeap::release);
  if (dj == Dj::Placed) join.setPlacement(modelPlacement());
  auto joinNow = [&]() {
    if (dj == Dj::Off) return;
    ModelHeap::mark();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(join.begin(table, &index)));
    u.lowest = std::min(u.lowest, ModelHeap::low);
    u.topKept = join.stats().topKept;
    runJoin(join, &index);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Ready), static_cast<int>(join.status()));
  };
  joinNow();
  u.bootJoined = ModelHeap::largest();
  u.bootTopKept = u.topKept;
  for (int step = 0; step < steps; ++step) {
    // enterFence(): the queue, Thumbs' pools, the join, then the old index.
    for (void* p : queue) ModelHeap::release(p);
    for (void* p : thumbs) ModelHeap::release(p);
    queue.clear();
    thumbs.clear();
    join.release();
    index.clear();
    u.fence = ModelHeap::freeBlocks(64 * 1024);
    // The build, from D's records.
    LibraryBuilder b(ModelHeap::alloc, ModelHeap::release);
    synthcard::Facts facts(card, false);
    synthcard::SameRows rows(LibraryBuilder::Status::Scanned, false, true);
    LibraryBuilder::Config c;
    c.device = &records;
    c.rows = &rows;
    c.facts = &facts;
    TEST_ASSERT_TRUE(b.build(index, c).built);
    // leaveFence(): the queue read back, the durations, Thumbs' pools; then
    // the join (the next pass).
    take({5120}, &queue);
    ModelHeap::release(g_durations);
    g_durations = ModelHeap::alloc(index.trackCount() * 2);
    take({204800, 256, 256, 110592, 24, 24}, &thumbs);
    u.largest = ModelHeap::largest();
    joinNow();
    u.joined = ModelHeap::largest();
    join.release();
    u.givenBack = ModelHeap::largest();
    joinNow();
  }
  join.release();
  index.clear();
  return u;
}

}  // namespace

void test_a_library_update_in_a_model_heap() {
  const uint32_t tracks = 19412;
  const synth::Spec spec = synth::userShape(tracks);
  const synthcard::Card card = synthcard::make(spec);
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
  // library.idx as the last boot saved it.
  std::vector<uint8_t> saved;
  {
    LibraryIndex idx;
    LibraryBuilder b;
    synthcard::Facts facts(card, false);
    synthcard::SameRows rows(LibraryBuilder::Status::Scanned, false, true);
    LibraryBuilder::Config c;
    c.device = &records;
    c.rows = &rows;
    c.facts = &facts;
    TEST_ASSERT_TRUE(b.build(idx, c).built);
    TEST_ASSERT_EQUAL_UINT32(tracks, idx.trackCount());
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
  }
  const UpdateRun off = libraryUpdates(Dj::Off, saved, records, card, 2);
  const UpdateRun on = libraryUpdates(Dj::Placed, saved, records, card, 2);
  const UpdateRun heaps = libraryUpdates(Dj::WhereThe, saved, records, card, 2);
  printf("[join] a library update, 19,412 tracks, the model heap: the largest free block at the boot %u B; after the "
         "update %u B with Auto DJ off (main's own rebuild). Auto DJ on: joined at the boot %u B; after the update "
         "%u B, joined %u B (%u B of it at the top), given back %u B; free at least %u B while placing. Its blocks "
         "where the heap puts them: joined %u B, given back %u B\n",
         (unsigned)off.bootLargest, (unsigned)off.largest, (unsigned)on.bootJoined, (unsigned)on.largest,
         (unsigned)on.joined, (unsigned)on.topKept, (unsigned)on.givenBack, (unsigned)on.lowest,
         (unsigned)heaps.joined, (unsigned)heaps.givenBack);
  // The rebuild itself fragments PSRAM, Auto DJ or not.
  TEST_ASSERT_TRUE(off.largest + 100000 < off.bootLargest);
  // The fence's frees leave the heap as Auto DJ off has it, and the build
  // and the Live step do the same with it.
  TEST_ASSERT_TRUE(on.fence == off.fence);
  TEST_ASSERT_TRUE(heaps.fence == off.fence);
  TEST_ASSERT_EQUAL_size_t(off.largest, on.largest);
  // Joined again: only what no smaller free block holds at the top (the
  // filters, 77,640 B); given back, off's block.
  TEST_ASSERT_EQUAL_size_t(static_cast<size_t>(tracks - 2) * 4, on.topKept);
  TEST_ASSERT_TRUE(on.joined + on.topKept + 24 * 1024 >= off.largest);
  TEST_ASSERT_EQUAL_size_t(off.largest, on.givenBack);
  TEST_ASSERT_TRUE(on.lowest >= kMargin);
  // At the boot (one free block): everything at the top, as before.
  TEST_ASSERT_EQUAL_size_t(155284, on.bootTopKept);
  TEST_ASSERT_TRUE(on.bootJoined + on.bootTopKept + 24 * 1024 >= on.bootLargest);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_filter_word);
  RUN_TEST(test_sort_pairs);
  RUN_TEST(test_goldens);
  RUN_TEST(test_hardening);
  RUN_TEST(test_a_table_whose_open_failed);
  RUN_TEST(test_flipped_bytes);
  RUN_TEST(test_written_files);
  RUN_TEST(test_injected_hashes);
  RUN_TEST(test_strides_and_crcs);
  RUN_TEST(test_slice_budget);
  RUN_TEST(test_fence_at_every_boundary);
  RUN_TEST(test_index_change_at_every_boundary);
  RUN_TEST(test_no_memory_and_read_errors);
  RUN_TEST(test_library_limit);
  RUN_TEST(test_fat_model);
  RUN_TEST(test_memory_at_scale);
  RUN_TEST(test_a_pass_of_slices);
  RUN_TEST(test_the_folders_hash);
  RUN_TEST(test_the_placement_s_order);
  RUN_TEST(test_free_never_under_the_margin);
  RUN_TEST(test_the_placement_in_a_model_heap);
  RUN_TEST(test_a_library_update_in_a_model_heap);
  return UNITY_END();
}
