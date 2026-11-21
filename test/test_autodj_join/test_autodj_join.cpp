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
// - the placement in a model of the device's heap (ESP-IDF's TLSF, pinned to
//   the device's logged numbers): the blocks' order at a free block's top and
//   without a placement, the holes that take them, every allocation failing
//   in turn; free never under the margin (the device's 2026-10-14 check:
//   10 KB); the spacers, wherever the largest free block is in its size class
//   (the device's 2026-11-02 rejoin: NO PSRAM with 815 KB free), another
//   task's block taking the room above them; a first
//   boot's walk carving its buffers during the join, then a library
//   update's track table (LibraryUpdate::roomToBuild()) that fits once the
//   join is given back, as with Auto DJ off; and a library update of the
//   real 19,412-track library, with Auto DJ off and on (the rebuild's own
//   fragmentation, the join after it taking only what no smaller block
//   holds), then the same built from T with the mood words' block
//   (MOOD-FILTERS.md 9.5's pass rule); and with the listening stats
//   (docs/LISTENING-STATS.md 5.14, S1's rework, its R6: the firmware's glue
//   on a card in memory, its object held from setup, the boot's scan's
//   blocks at the top until its save, each save's work memory and files in
//   its own call) under the same rule, joined within the control's class;
// - the listening stats across five library updates (docs/LISTENING-STATS.md
//   5.14's yardstick, its R1 and R6): the decoders' blocks as the device asks
//   for them, held as a history has them, joined after each update against
//   main's run in the same history, beside main's own variation; the rework
//   fails no step the control doesn't, and the shapes its waits exist for
//   (each wait left out) do;
// - the stats pages' asks (docs/LISTENING-STATS.md 9.7, S3a's V5): a visit
//   (Full, then Lists) and Not played yet's (Lean, then No folders), each
//   grade statsvisit::layout()'s block with the firmware's head, through the
//   hold after the boot's join and after each update's rejoin, with and
//   without the Opus kept, each grade taken or refused pinned and each ask
//   leaving no trace; a reduced-room shape at 286,708 B, the snapshot
//   elsewhere or out of the largest free block; and A4, the snapshot held
//   from each visit to the next fence (512, 3,860 and 4,096 B), as ways of
//   the yardstick, failing no step the control doesn't; 9.7's fallback for
//   run 3a-4 (the snapshot freed before each decoder's open and each join's
//   begin), every figure the visits alone's to the byte, with its cost (how
//   often a page loses its lists, how long a snapshot is held); A4's moves
//   traced through the model heap; and a top placement for comparison.
// Run: pio test -e native -f test_autodj_join
// The yardstick's variants (5.14's R1), outside the suite, the case alone, in
// a build folder of their own (from Git Bash in the worktree):
//   PLATFORMIO_BUILD_FLAGS="-DSTATS_WIDE" PLATFORMIO_BUILD_DIR="$PWD/.pio/build-yard" \
//     pio test -e native -f test_autodj_join -v
// with -DSTATS_OLD_DECODERS, and both, the same way.
#include <unity.h>

#include <algorithm>
#include <array>
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
#include "../support/CutFs.h"
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
#include "PlayLog.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsGlue.h"
#include "StatsMap.h"
#include "StatsRank.h"
#include "StatsVisit.h"

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

// The listening stats' hold over the model heap (app/ListeningStats.cpp's
// topAlloc() and topRelease(), the model's hooks): one a process, emptied
// wherever the model heap is reset (and before each case), so that a case a
// failure cut short (Unity's longjmp: no destructor ran) leaves no stale
// entry for the next.
autodj::TopHold g_modelTop;

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

// The player at a take (the firmware's ListeningStats::pass(), the pass
// before the scan's piece): a track playing, a start waiting. The room's
// rule reads only `playing` (statsglue::decoderRoom()).
struct TopPlayer {
  bool playing = false, waiting = false;
};
std::function<TopPlayer()> g_topPlayer;  // (topAlloc()'s room; null: stopped)

void setUp() {
  Heap::reset();
  g_hashCalls = 0;
  g_modelTop = autodj::TopHold();
  g_topPlayer = nullptr;
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

// A heap that serves a request as the framework's does (ESP-IDF 5.5's
// components/heap with CONFIG_HEAP_POISONING_LIGHT: tlsf_control_functions.h,
// and tlsf_malloc(), tlsf_fit_size() and the poisoning's wrappers as libheap.a
// has them): 12 B of poisoning on every request; 4-byte alignment and a 4-byte
// header a block; free lists by size class, 32 classes a power of two above
// 128 B; a request rounded up to its class's next boundary (mapping_search(),
// which writes the rounded size back: it is what block_prepare_used() carves,
// not only what the search asks for), then the head of the first non-empty
// list at or above it, carved from that block's low end (block_trim_free());
// a freed block merges with its free neighbours and goes to its list's head;
// realloc to a smaller size trims in place to the request, not rounded
// (block_trim_used()). largest() answers as heap_caps_get_largest_free_block()
// does: the largest request that succeeds, the largest free block rounded down
// to its class (tlsf_fit_size()) less the 12 B, which says nothing of where in
// its class the block is; rawLargest() is the block itself. freeBytes() answers
// as heap_caps_get_free_size() does: a block's bytes and header off at each
// allocation and back at each free, less the 12 B. Real memory (the join writes
// its maps, LibraryIndex its strings). Its free bytes' lowest since mark(); the
// `failAt`-th allocation since reset() fails.
//
// A run's carves and frees in order, for a trace (the yardstick's A4 trace,
// S3a's V5): each with the run's place (the update step, -1 the boot; its
// point, a TracePoint) and whether it is the stats pages' snapshot's own;
// the blocks in use by offset (each its carve's index), and the heap at each
// update's figure. ModelHeap records into it while ModelHeap::trace points
// at one (reset() leaves it).
struct HeapTrace {
  struct Ev {
    size_t at = 0;     // the block's offset
    size_t n = 0;      // the request (0: a free)
    size_t block = 0;  // the block's size (a free: before it merges)
    size_t from = 0;   // (a carve) the free block it came out of
    size_t free = 0, largest = 0;  // free and the largest free block (raw) before it
    int step = -1, point = 0;
    bool snap = false;
  };
  struct Block {
    size_t at = 0, size = 0;
    bool used = false;
    size_t carve = SIZE_MAX;  // (used) its carve's index in ev; SIZE_MAX: before the trace
  };
  std::vector<Ev> ev;
  std::map<size_t, size_t> live;
  std::vector<std::vector<Block>> figures;  // [update]: the heap at its figure (Updates::joined's)
  std::vector<size_t> figureAt;             // ... ev's size then
  int step = -1, point = 0;
  bool snap = false;
  void carved(size_t at, size_t n, size_t block, size_t from, size_t free0, size_t largest0) {
    Ev e;
    e.at = at;
    e.n = n;
    e.block = block;
    e.from = from;
    e.free = free0;
    e.largest = largest0;
    e.step = step;
    e.point = point;
    e.snap = snap;
    live[at] = ev.size();
    ev.push_back(e);
  }
  void freed(size_t at, size_t block, size_t free0, size_t largest0) {
    Ev e;
    e.at = at;
    e.block = block;
    e.free = free0;
    e.largest = largest0;
    e.step = step;
    e.point = point;
    e.snap = snap;
    ev.push_back(e);
    live.erase(at);
  }
};

// A trace's points: where in libraryUpdates() a carve or a free was made.
enum TracePoint : int {
  kTpBoot,      // the boot, up to its join
  kTpBootJoin,  // the boot's join
  kTpFlacOpen,  // an update step's FLAC opening, before its fence
  kTpSave,      // the fence pass's records' save
  kTpFence,     // enterFence()
  kTpBuild,     // the build
  kTpLive,      // leaveFence(): the queue, the durations, Thumbs' pools
  kTpRejoin,    // the rejoin
  kTpVisit,     // the stats pages' visit
  kTpFlacEnd,   // the FLAC's end
  kTpDc,        // the Dc: the join again
  kTpPlays,     // the plays written, their records' save
  kTpOpusOpen,  // the Opus opening
  kTpEnd,       // the run's end
};
const char* tracePointName(int p) {
  static const char* const names[] = {"the boot",       "the boot's join", "the FLAC's open", "the records' save",
                                      "the fence",      "the build",       "the Live",        "the rejoin",
                                      "the visit",      "the FLAC's end",  "the Dc's join",   "the plays' save",
                                      "the Opus's open", "the run's end"};
  return p >= 0 && p <= kTpEnd ? names[p] : "?";
}

struct ModelHeap {
  struct Block {
    size_t size;  // usable bytes: the block's footprint is kHeader more
    bool used;
  };
  static constexpr size_t kAlign = 4, kHeader = 4, kMin = 12, kSplit = 16, kSmall = 128, kPoison = 12;
  static constexpr int kSlLog2 = 5;
  static inline std::vector<uint8_t> mem;
  static inline std::map<size_t, Block> blocks;  // footprint's offset: the block
  static inline std::map<std::pair<int, int>, std::vector<size_t>> lists;  // (fl, sl): offsets, the head last
  static inline size_t freeNow = 0, low = 0, allocs = 0;
  static inline long failAt = -1;
  // The carves and frees while `rec` is set, to replay them in a control (the
  // stats' read at PlayHistory::begin()'s place): an alloc of `n` bytes at the
  // block's offset, or (n 0) the release of the block at that offset.
  struct Event {
    size_t at, n;
  };
  static inline std::vector<Event>* rec = nullptr;
  static inline HeapTrace* trace = nullptr;

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
  // The block a request of n bytes takes: its size rounded up to its class.
  static size_t carved(size_t n) {
    const size_t a = std::max(kMin, (n + kPoison + kAlign - 1) & ~(kAlign - 1));
    if (a < kSmall) return a;
    const size_t round = static_cast<size_t>(1) << (fls(a) - kSlLog2);
    return (a + round - 1) & ~(round - 1);
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
    g_modelTop = autodj::TopHold();
    blocks[0] = {bytes - kHeader, false};
    list(0);
    freeNow = low = bytes - kHeader - kPoison;
    allocs = 0;
    failAt = -1;
  }
  static void* alloc(size_t n) {
    if (failAt >= 0 && static_cast<long>(allocs++) == failAt) return nullptr;
    if (n == 0) return nullptr;
    const size_t free0 = freeNow, largest0 = trace ? rawLargest() : 0;
    const size_t a = carved(n);
    auto it = lists.lower_bound(classOf(a));
    while (it != lists.end() && it->second.empty()) ++it;
    if (it == lists.end()) return nullptr;
    const size_t off = it->second.back();
    unlist(off);
    Block& b = blocks[off];
    const size_t from = b.size;
    if (b.size >= kSplit + a) {
      blocks[off + kHeader + a] = {b.size - a - kHeader, false};
      list(off + kHeader + a);
      b.size = a;
    }
    freeNow -= b.size + kHeader;
    b.used = true;
    low = std::min(low, freeNow);
    if (rec) rec->push_back({off, n});
    if (trace) trace->carved(off, n, b.size, from, free0, largest0);
    return mem.data() + off + kHeader;
  }
  static size_t offsetOf(const void* p) {
    return static_cast<size_t>(static_cast<const uint8_t*>(p) - mem.data()) - kHeader;
  }
  static void release(void* p) {
    if (!p) return;
    auto it = blocks.find(offsetOf(p));
    TEST_ASSERT_TRUE_MESSAGE(it != blocks.end() && it->second.used, "not a block of the model's");
    if (rec) rec->push_back({it->first, 0});
    if (trace) trace->freed(it->first, it->second.size, freeNow, rawLargest());
    it->second.used = false;
    freeNow += it->second.size + kHeader;
    auto next = std::next(it);
    if (next != blocks.end() && !next->second.used) {
      unlist(next->first);
      it->second.size += kHeader + next->second.size;
      blocks.erase(next);
    }
    if (it != blocks.begin()) {
      auto prev = std::prev(it);
      if (!prev->second.used) {
        unlist(prev->first);
        prev->second.size += kHeader + it->second.size;
        blocks.erase(it);
        it = prev;
      }
    }
    list(it->first);
  }
  static void* shrink(void* p, size_t n) {
    auto it = blocks.find(offsetOf(p));
    TEST_ASSERT_TRUE_MESSAGE(it != blocks.end() && it->second.used, "not a block of the model's");
    const size_t a = std::max(kMin, (n + kPoison + kAlign - 1) & ~(kAlign - 1));
    if (a > it->second.size) return nullptr;
    if (it->second.size >= kSplit + a) {
      const size_t tail = it->first + kHeader + a;
      blocks[tail] = {it->second.size - a - kHeader, true};
      it->second.size = a;
      std::vector<Event>* r = rec;  // (the tail is no carve of the caller's)
      rec = nullptr;
      release(mem.data() + tail + kHeader);
      rec = r;
    }
    return p;
  }
  static size_t rawLargest() {
    size_t m = 0;
    for (const auto& b : blocks)
      if (!b.second.used) m = std::max(m, b.second.size);
    return m;
  }
  static size_t largest() {
    const size_t m = rawLargest();
    if (m < kSmall) return m > kPoison ? m - kPoison : 0;
    return (m & ~((static_cast<size_t>(1) << (fls(m) - kSlLog2)) - 1)) - kPoison;
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

// The heap now into the trace's figures (an update's figure).
void traceFigure(HeapTrace* t) {
  std::vector<HeapTrace::Block> v;
  for (const auto& b : ModelHeap::blocks) {
    HeapTrace::Block k;
    k.at = b.first;
    k.size = b.second.size;
    k.used = b.second.used;
    const auto it = t->live.find(b.first);
    if (k.used && it != t->live.end()) k.carve = it->second;
    v.push_back(k);
  }
  t->figures.push_back(v);
  t->figureAt.push_back(t->ev.size());
}

constexpr size_t kMargin = 64 * 1024;  // the firmware's (app/AutoDj.cpp's kJoinMarginBytes)

// autodj::AboveFn over the model heap, through the firmware's own walk state
// (autodj::WalkAbove: app/ListeningStats.cpp's walkStep() is a line over it,
// heap_caps_walk() handing it each block by TLSF's pointer, malloc's less
// the poisoning's 8 B): the model's blocks in address order, each by its
// footprint's address (the model's pointer less its 4 B header, so a cap
// that takes T's head returns the walk's capAt here too), one heap, no bound.
uint32_t g_aboveCalls = 0;
void (*g_aboveHook)(uint32_t call) = nullptr;  // (a test's: another task's carve before the walk)
void walkModel(autodj::WalkAbove& w) {
  for (const auto& b : ModelHeap::blocks)
    if (!w.step(0, reinterpret_cast<uintptr_t>(ModelHeap::mem.data() + b.first), b.second.size, b.second.used)) return;
}
void modelAbove(const void* block, const void* spacer, autodj::Above* out) {
  if (g_aboveHook) g_aboveHook(g_aboveCalls);
  ++g_aboveCalls;
  autodj::WalkAbove w(block, spacer, ModelHeap::kHeader, 0, out);
  walkModel(w);
}

// The model's hold hooks (the firmware's kTop: its margin, the poisoning's 12 B).
const autodj::TopHold::Hooks kModelTop = {ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                          ModelHeap::release, modelAbove,           kMargin,
                                          ModelHeap::kPoison};

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

// A heap of free blocks that exactly hold requests of `holes` bytes, each before
// a used block, then a free block of `top` bytes exactly.
void holesThenTop(const std::vector<size_t>& holes, size_t top) {
  size_t taken = 0;
  for (size_t hole : holes) taken += ModelHeap::carved(hole) + ModelHeap::carved(64) + 2 * ModelHeap::kHeader;
  ModelHeap::reset(taken + top + ModelHeap::kHeader);
  std::vector<void*> fill;
  for (size_t hole : holes) {
    fill.push_back(ModelHeap::alloc(hole));
    TEST_ASSERT_NOT_NULL(ModelHeap::alloc(64));
  }
  for (void* p : fill) ModelHeap::release(p);
  TEST_ASSERT_EQUAL_size_t(top, ModelHeap::rawLargest());
}

}  // namespace

// The model against the device's own numbers, none of them the placement's
// arithmetic: what a request takes, what is left, what largest() says of it.
// (A model that carved the request itself, as this one did until 2026-11-02,
// couldn't see the rejoin's failure.)
void test_the_model_heap_against_the_device() {
  // Every rejoin that failed on 2026-11-02 (three builds, the spare card's
  // 19,410 rows and 19,412 tracks): the largest free block said 417,780 B, the
  // spacer asked 336,690 B, what was left said 73,716 B and the filters' 77,640 B
  // were refused, with 570,140 B taken: the two maps, the runs, the folders'
  // states, the buffer and the runs' table in the holes, and the spacer.
  const size_t holed[] = {38820, 38824, 24576, 24576, 24576, 24576, 18168, 24770, 4096, 100};
  size_t inHoles = 0;
  for (size_t b : holed) inHoles += ModelHeap::carved(b) + ModelHeap::kHeader;
  TEST_ASSERT_EQUAL_size_t(226072, inHoles);
  TEST_ASSERT_EQUAL_size_t(344064, ModelHeap::carved(336690));
  TEST_ASSERT_EQUAL_size_t(570140, inHoles + ModelHeap::carved(336690) + ModelHeap::kHeader);
  TEST_ASSERT_EQUAL_size_t(77824, ModelHeap::carved(77640));
  for (size_t over : {size_t(4), size_t(1000), size_t(2048)}) {
    ModelHeap::reset(417792 + over + ModelHeap::kHeader);
    TEST_ASSERT_EQUAL_size_t(417780, ModelHeap::largest());
    const size_t free0 = ModelHeap::freeBytes();
    TEST_ASSERT_NOT_NULL(ModelHeap::alloc(336690));
    TEST_ASSERT_EQUAL_size_t(free0 - 344068, ModelHeap::freeBytes());
    TEST_ASSERT_EQUAL_size_t(73716, ModelHeap::largest());
    TEST_ASSERT_NULL(ModelHeap::alloc(77640));
  }
  // The same session's Dc a little later, which joined: 303,092 B, a spacer of
  // 222,002 B, PSRAM free at least 129,332 B of 658,516 B, and 225,268 B said
  // of the free part once the spacer was given back.
  TEST_ASSERT_EQUAL_size_t(129332, 658516 - inHoles - (ModelHeap::carved(222002) + ModelHeap::kHeader) -
                                       (ModelHeap::carved(77640) + ModelHeap::kHeader));
  ModelHeap::reset(303104 + 4 + ModelHeap::kHeader);
  TEST_ASSERT_EQUAL_size_t(303092, ModelHeap::largest());
  void* spacer = ModelHeap::alloc(222002);
  TEST_ASSERT_NOT_NULL(spacer);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(77640));
  ModelHeap::release(spacer);
  TEST_ASSERT_EQUAL_size_t(225268, ModelHeap::largest());
  // 2026-10-15 (docs/AUTODJ.md 7.4), a rejoin that worked: 409,588 B once given
  // back, 335,860 B while joined.
  ModelHeap::reset(409600 + 6000 + ModelHeap::kHeader);
  TEST_ASSERT_EQUAL_size_t(409588, ModelHeap::largest());
  spacer = ModelHeap::alloc(328498);
  TEST_ASSERT_NOT_NULL(spacer);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(77640));
  ModelHeap::release(spacer);
  TEST_ASSERT_EQUAL_size_t(335860, ModelHeap::largest());
}

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
  // (Its free block on a class's floor: largest() tells of every block taken
  // from it. test_the_rejoin_anywhere_in_a_class() has the rest of the class.)
  const size_t heap = (1u << 20) + ModelHeap::kHeader;
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
    TEST_ASSERT_EQUAL_size_t(heap - ModelHeap::kHeader, ModelHeap::rawLargest());
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
  // Every allocation failing in turn: a block's, NoMemory, nothing held; but
  // one under the spacers (they hold the most of the heap) or a spacer's, the
  // blocks where the heap puts them (a join as any other); the heap whole again
  // either way.
  int noMemory = 0, unplaced = 0, topShort = 0;
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
      topShort += j.stats().topShort ? 1 : 0;
      TEST_ASSERT_FALSE(j.stats().placed && j.stats().topShort);
      runJoin(j, s.idx.get());
      assertMaps(j, e, "an allocation failing");
      j.release();
    }
    TEST_ASSERT_EQUAL_size_t(heap - ModelHeap::kHeader, ModelHeap::rawLargest());
    if (!failed) {
      TEST_ASSERT_TRUE(j.stats().placed || st != Status::Working);
      break;
    }
  }
  TEST_ASSERT_TRUE(noMemory >= 3);  // (the runs' table, the probes, the kept blocks once the spacers are gone)
  TEST_ASSERT_TRUE(unplaced >= 1);  // (a spacer's)
  TEST_ASSERT_TRUE(topShort >= 1);  // (a block's under the spacers)
}

// The spacers never leave less than the margin free (the device's 2026-10-14
// check: free fell to 10 KB at every join), whatever the heap's shape, each
// block's rounding and the spacer's own counted: the join's own blocks only
// take what they hold, rounded. The maps taken after the spacers keep most of
// it at 20,000 tracks whatever the margin.
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
        const size_t peak = j.stats().peakBytes;
        const size_t own = peak + peak / 32 + 1024;
        const size_t floor = std::min(margin, free0 > own ? free0 - own : 0);
        TEST_ASSERT_TRUE_MESSAGE(ModelHeap::low >= floor, what);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(ModelHeap::low, j.stats().lowestFree, what);
        // The small library's maps (48 KB) under the margin: without it the
        // spacers leave little more than them.
        if (lib && margin == 0 && c.holes == 0) TEST_ASSERT_TRUE_MESSAGE(ModelHeap::low < 64 * 1024, what);
        printf("[join] %s: free %u B, its lowest while placing %u B (%s)\n", what, (unsigned)free0,
               (unsigned)ModelHeap::low, j.stats().placed ? "placed" : "where the heap put it");
      }
    }
    // Every size of heap around where the margin bites: the spacer's own
    // rounding, a thirty-second of it, can't take it under.
    for (size_t heap = 340000; heap < 760000; heap += 1537) {
      ModelHeap::reset(heap);
      const size_t free0 = ModelHeap::freeBytes();
      ModelHeap::mark();
      Join j(ModelHeap::alloc, ModelHeap::release);
      j.setPlacement(modelPlacement());
      if (j.begin(f.table, &idx) != Status::Working) continue;
      const size_t peak = j.stats().peakBytes;
      const size_t own = peak + peak / 32 + 1024;
      char what[64];
      snprintf(what, sizeof(what), "%u tracks, a heap of %u B", (unsigned)idx.trackCount(), (unsigned)heap);
      TEST_ASSERT_TRUE_MESSAGE(ModelHeap::low >= std::min(kMargin, free0 > own ? free0 - own : 0), what);
    }
  }
  // The kept blocks in holes they just fit (the runs' table in one of its own),
  // so every transient one goes under the spacers with the margin to keep beside
  // them: their rounding counts.
  const size_t rows = big.rows;
  for (size_t top = 240000; top < 420000; top += 257) {
    holesThenTop({512, rows * 4, rows * 2, rows * 2}, top);
    Join j(ModelHeap::alloc, ModelHeap::release);
    j.setPlacement(modelPlacement());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(bf.table, big.idx.get())));
    char what[64];
    snprintf(what, sizeof(what), "the kept blocks in holes, a free block of %u B", (unsigned)top);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3, j.stats().keptElsewhere, what);
    TEST_ASSERT_TRUE_MESSAGE(j.stats().placed, what);
    TEST_ASSERT_TRUE_MESSAGE(j.stats().lowestFree >= kMargin, what);
  }
}

// The rejoin after a library update on the device (2026-11-02: gb on a fresh
// boot, "[heap] FAILED: 77640 B", largest block 73716 B with 815 KB free, picks
// at random until a reboot): holes that take the two maps and the transient
// blocks, the filters for the top of the largest free block. largest() only
// says which size class that block is in, and the spacer takes its request
// rounded up to its own: the filters' room must hold wherever in its class the
// block is, in each of its classes.
void test_the_rejoin_anywhere_in_a_class() {
  for (uint32_t tracks : {uint32_t(19412), uint32_t(27000), uint32_t(8000)}) {
    Scale s = scale(tracks, 4);
    File f(s.file);
    const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
    const size_t filters = static_cast<size_t>(s.rows) * 4;
    const size_t hole = std::max(static_cast<size_t>(s.rows) * 2 + 8192, size_t(30000));
    for (size_t floor : {size_t(303104), size_t(344064), size_t(409600), size_t(417792), size_t(524288), size_t(655360)}) {
      const size_t cls = floor >= 524288 ? 16384 : 8192;
      for (size_t over = 0; over < cls; over += cls / 32) {
        const size_t top = floor + over;
        holesThenTop(std::vector<size_t>(12, hole), top);
        char what[96];
        snprintf(what, sizeof(what), "%u tracks, the largest free block %u B", (unsigned)tracks, (unsigned)top);
        Join j(ModelHeap::alloc, ModelHeap::release);
        j.setPlacement(modelPlacement());
        TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, s.idx.get())),
                                      what);
        const Join::Stats& st = j.stats();
        TEST_ASSERT_TRUE_MESSAGE(st.placed, what);
        TEST_ASSERT_FALSE_MESSAGE(st.topShort, what);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(2, st.keptElsewhere, what);
        TEST_ASSERT_EQUAL_size_t_MESSAGE(filters, st.topBytes, what);
        TEST_ASSERT_TRUE_MESSAGE(st.lowestFree >= kMargin, what);
        // At the block's top: the free part below them is the most of it, all
        // but their room, over by at most a class of the spacer's and one of
        // the block's (a closer spacer lands in a hole, and is given back).
        TEST_ASSERT_TRUE_MESSAGE(ModelHeap::rawLargest() + filters <= top, what);
        TEST_ASSERT_TRUE_MESSAGE(ModelHeap::rawLargest() + filters + filters / 32 + 1024 + top / 16 + 8 >= top, what);
        if (over == 0) {
          runJoin(j, s.idx.get());
          assertMaps(j, e, what);
        }
        j.release();
        TEST_ASSERT_EQUAL_size_t_MESSAGE(top, ModelHeap::rawLargest(), what);
      }
    }
  }
}

// What the spacers leave, wherever the free block is in its class and whichever
// blocks go above them: they all fit there, close to the top (a list of blocks
// is what the transient ones and the kept ones make, each rounded up).
void test_the_room_above_the_spacers() {
  const std::vector<std::vector<size_t>> lists = {
      {77640},
      {77640, 38820, 38824},
      {24576, 24576, 24576, 24576, 18168, 24770, 4096, 77640, 38820, 38824},
      {24576, 11430, 8000, 4096, 24004, 12002, 12002},
      {1000},
  };
  int spaced = 0, unspaced = 0;
  for (const std::vector<size_t>& list : lists) {
    size_t bytes = 0;
    for (size_t b : list) bytes += b;
    const size_t need = bytes + bytes / 32 + 1024;
    for (size_t top = bytes + 1024; top < 1300000; top += 3331) {
      ModelHeap::reset(top + ModelHeap::kHeader);
      autodj::Spacers sp;
      char what[96];
      snprintf(what, sizeof(what), "%u B of blocks, a free block of %u B", (unsigned)bytes, (unsigned)top);
      if (!autodj::takeSpacers(bytes, 0, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc, ModelHeap::release,
                               &sp)) {
        // None only with no room for one: the block is about what they need.
        TEST_ASSERT_TRUE_MESSAGE(top < need + 4096 + 256 + top / 32, what);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, sp.n, what);
        ++unspaced;
        continue;
      }
      ++spaced;
      const size_t room = ModelHeap::rawLargest();
      TEST_ASSERT_TRUE_MESSAGE(room >= need, what);
      const uint8_t* highest = nullptr;
      for (int i = 0; i < sp.n; ++i) highest = std::max(highest, static_cast<const uint8_t*>(sp.p[i]));
      for (size_t b : list) {
        const uint8_t* p = static_cast<const uint8_t*>(ModelHeap::alloc(b));
        TEST_ASSERT_NOT_NULL_MESSAGE(p, what);
        TEST_ASSERT_TRUE_MESSAGE(p > highest, what);
      }
      // Closed in: a class of the block's and a few KB of spacer not worth it.
      TEST_ASSERT_TRUE_MESSAGE(room <= need + room / 32 + 4096 + 256, what);
      TEST_ASSERT_TRUE_MESSAGE(sp.n <= 3, what);
    }
  }
  TEST_ASSERT_TRUE(spaced > 1000);
  TEST_ASSERT_TRUE(unspaced >= 1);
}

// Another task's block in the milliseconds of begin() (a track's start: the
// Opus packet buffer's 60 KB, which here takes the room left above the spacers
// when it is the closest fit): the placement falls short, not the join.
namespace {

long g_allocCalls = 0, g_otherAt = -1;
void* g_other = nullptr;
void* allocBesideAnother(size_t n) {
  if (g_allocCalls++ == g_otherAt) g_other = ModelHeap::alloc(60 * 1024);
  return ModelHeap::alloc(n);
}

}  // namespace

void test_another_task_s_block_meanwhile() {
  Scale s = scale(19412, 4);
  File f(s.file);
  const Expect e = reference(*s.idx, f.table, byPath(*s.idx));
  const size_t hole = static_cast<size_t>(s.rows) * 2 + 8192;
  int shortOf = 0;
  for (long k = 0;; ++k) {
    holesThenTop(std::vector<size_t>(12, hole), 417792 + 1000);
    g_allocCalls = 0;
    g_otherAt = k;
    g_other = nullptr;
    Join j(allocBesideAnother, ModelHeap::release);
    j.setPlacement(modelPlacement());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(j.begin(f.table, s.idx.get())));
    shortOf += j.stats().topShort ? 1 : 0;
    TEST_ASSERT_FALSE(j.stats().placed && j.stats().topShort);
    const bool taken = g_allocCalls > k;
    runJoin(j, s.idx.get());
    assertMaps(j, e, "another task's block");
    j.release();
    if (g_other) ModelHeap::release(g_other);
    TEST_ASSERT_EQUAL_size_t(417792 + 1000, ModelHeap::rawLargest());
    if (!taken) break;
  }
  TEST_ASSERT_TRUE(shortOf >= 1);
}

// ---------------------------------------------------------------------------
// The listening stats' hold (docs/LISTENING-STATS.md 5.14, fix A): a block
// held at the top of the largest free block, the free block the spacers left
// above it (T) closed by caps, given back bottom-up.
// ---------------------------------------------------------------------------
namespace {

// A heap whose last free block is exactly `f` bytes, right above a block of
// `a` bytes in use (with a block of 64 B in use above it when `withB`: the
// tail trimmed off a third block, exact, not rounded); the block's pointer.
void* gapAbove(size_t f, bool withB) {
  const size_t ca = ModelHeap::carved(512);
  if (!withB) {
    ModelHeap::reset(ca + 2 * ModelHeap::kHeader + f);
    return ModelHeap::alloc(512);
  }
  const size_t cx = ModelHeap::carved(f + 16), cb = ModelHeap::carved(64);
  ModelHeap::reset(3 * ModelHeap::kHeader + ca + cx + cb);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(512));
  void* x = ModelHeap::alloc(f + 16);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(64));
  // Trimmed to a' (block_trim_used(): the request's own size, 4-aligned):
  // the tail is cx - a' - 4 = f.
  const size_t keep = cx - f - ModelHeap::kHeader;
  TEST_ASSERT_NOT_NULL(ModelHeap::shrink(x, keep - ModelHeap::kPoison));
  return x;
}

autodj::CloseHooks modelClose() {
  autodj::CloseHooks c;
  c.above = modelAbove;
  c.alloc = ModelHeap::alloc;
  c.release = ModelHeap::release;
  c.overhead = ModelHeap::kPoison;
  return c;
}

using Map = std::map<size_t, ModelHeap::Block>;
bool sameMap(const Map& a, const Map& b) {
  if (a.size() != b.size()) return false;
  for (auto i = a.begin(), j = b.begin(); i != a.end(); ++i, ++j)
    if (i->first != j->first || i->second.size != j->second.size || i->second.used != j->second.used) return false;
  return true;
}

// The boot's heap in small (5 holes of 1,000 B under the free top: the
// setup's, smaller than the ranges' block) .
void bootHeap() { holesThenTop(std::vector<size_t>(5, 1000), 834836); }

// The free block right above the model block at `p` (0: none).
size_t freeAboveOf(const void* p) {
  auto it = ModelHeap::blocks.find(ModelHeap::offsetOf(p));
  auto next = std::next(it);
  return next != ModelHeap::blocks.end() && !next->second.used ? next->second.size : 0;
}

}  // namespace

// U1: the cap rule against the model heap (ESP-IDF 5.5's TLSF), every free
// size from 12 B to 64 KB in 4 B steps and 2,000 more to 1 MB, each built
// exactly (a heap's last block, or a block's trimmed tail under one in use):
// closed by at most 3 caps, each at the address the walk gave, nothing a
// request can take left above (0 B, or a 12 B block), the caps' footprints
// and the rest's T's; freed, the heap as it was. The counts are capcheck's
// (the spec's every size to 4 MB, its rules the model's).
void test_caps_close_any_free_block() {
  TEST_ASSERT_EQUAL_size_t(57332, autodj::capRequest(58212, 12));
  TEST_ASSERT_EQUAL_size_t(852, autodj::capRequest(864, 12));
  TEST_ASSERT_EQUAL_size_t(1236, autodj::capRequest(1276, 12));
  TEST_ASSERT_EQUAL_size_t(12, autodj::capRequest(24, 12));
  TEST_ASSERT_EQUAL_size_t(4, autodj::capRequest(16, 12));
  TEST_ASSERT_EQUAL_size_t(0, autodj::capRequest(12, 12));
  TEST_ASSERT_EQUAL_size_t(12, autodj::capRequest(12, 0));  // (no poisoning: TLSF's least, 12 B)
  std::map<int, uint32_t> caps;
  uint32_t rest12 = 0, rest0 = 0;
  auto one = [&](size_t f, bool withB) {
    void* a = gapAbove(f, withB);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_size_t(f, freeAboveOf(a));
    const Map before = ModelHeap::blocks;
    autodj::Caps c;
    autodj::CloseReport r;
    const autodj::Close why = autodj::closeAbove(a, nullptr, modelClose(), &c, &r);
    char w[64];
    std::snprintf(w, sizeof(w), "a free block of %u B%s", (unsigned)f, withB ? " under a block" : "");
    TEST_ASSERT_TRUE_MESSAGE(why == (f < 16 ? autodj::Close::Nothing : autodj::Close::Closed), w);
    TEST_ASSERT_TRUE_MESSAGE(c.n <= autodj::Caps::kMax, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(c.n, r.caps, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(f, r.aboveBytes, w);
    // Each cap right above the one before (the block, then each cap).
    const void* at = a;
    size_t footprints = 0;
    for (int k = 0; k < c.n; ++k) {
      auto it = ModelHeap::blocks.find(ModelHeap::offsetOf(at));
      TEST_ASSERT_EQUAL_size_t_MESSAGE(it->first + ModelHeap::kHeader + it->second.size, ModelHeap::offsetOf(c.p[k]),
                                       w);
      footprints += ModelHeap::blocks[ModelHeap::offsetOf(c.p[k])].size + ModelHeap::kHeader;
      at = c.p[k];
    }
    TEST_ASSERT_EQUAL_size_t_MESSAGE(footprints, r.capBytes, w);
    const size_t rest = freeAboveOf(at);
    TEST_ASSERT_TRUE_MESSAGE(rest == 0 || rest == 12, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(f + ModelHeap::kHeader, footprints + (rest ? rest + ModelHeap::kHeader : 0), w);
    if (withB) TEST_ASSERT_TRUE_MESSAGE(rest || ModelHeap::blocks.rbegin()->first == ModelHeap::offsetOf(at) +
                                                       ModelHeap::kHeader + ModelHeap::blocks[ModelHeap::offsetOf(at)].size,
                                        w);
    for (int k = 0; k < c.n; ++k) ModelHeap::release(c.p[k]);
    TEST_ASSERT_TRUE_MESSAGE(sameMap(before, ModelHeap::blocks), w);
    return std::make_pair(static_cast<int>(c.n), rest);
  };
  for (size_t f = 12; f <= 65536; f += 4) {
    const auto o = one(f, (f / 4) % 2 == 1);
    ++caps[o.first];
    ++(o.second ? rest12 : rest0);
  }
  printf("[hold] the caps, every free block of 12 B to 64 KB: 0 caps %u, 1 cap %u, 2 caps %u; 12 B left %u, 0 B %u\n",
         (unsigned)caps[0], (unsigned)caps[1], (unsigned)caps[2], (unsigned)rest12, (unsigned)rest0);
  // capcheck's counts over the same sizes.
  TEST_ASSERT_EQUAL_UINT32(1, caps[0]);
  TEST_ASSERT_EQUAL_UINT32(1213, caps[1]);
  TEST_ASSERT_EQUAL_UINT32(15168, caps[2]);
  TEST_ASSERT_EQUAL_UINT32(0, caps[3]);
  TEST_ASSERT_EQUAL_UINT32(193, rest12);
  TEST_ASSERT_EQUAL_UINT32(16189, rest0);
  // To 1 MB: three caps (the first need for them is at 66,584 B).
  std::mt19937 rng(58212);
  std::map<int, uint32_t> big;
  for (int i = 0; i < 2000; ++i) {
    const size_t f = 12 + 4 * (rng() % ((1u << 20) / 4));
    ++big[one(f, i % 2 == 1).first];
  }
  TEST_ASSERT_EQUAL_INT(2, one(66580, false).first);
  TEST_ASSERT_EQUAL_INT(3, one(66584, false).first);
  printf("[hold] the caps, 2,000 free blocks to 1 MB: 1 cap %u, 2 caps %u, 3 caps %u\n", (unsigned)big[1],
         (unsigned)big[2], (unsigned)big[3]);
  TEST_ASSERT_TRUE(big[3] > 0 && big[2] > 0);
  TEST_ASSERT_EQUAL_UINT32(0, big[4]);
}

// U2: the boot's take (the ranges' block): T closed (nothing free between it
// and the heap's end), then the first frame's canvas and a queue each where a
// heap that never had the take puts them; given back, that heap's blocks.
// Without the caps (no walk: fe61db9's topAlloc()) the sprite lands in T.
void test_the_hold_closes_t() {
  auto tail = [](bool take, bool caps) {
    bootHeap();
    autodj::TopHold hold;
    autodj::TopHold::Hooks h = kModelTop;
    if (!caps) h.above = nullptr;
    autodj::CloseReport r;
    void* block = take ? hold.take(h, 1284, &r) : nullptr;
    if (take) {
      TEST_ASSERT_NOT_NULL(block);
      TEST_ASSERT_TRUE(r.why == (caps ? autodj::Close::Closed : autodj::Close::Nothing));
      if (caps) {
        // Nothing a request can take between the block and the heap's end.
        for (auto it = ModelHeap::blocks.find(ModelHeap::offsetOf(block)); it != ModelHeap::blocks.end(); ++it)
          TEST_ASSERT_TRUE(it->second.used || it->second.size < 16);
        printf("[hold] the boot's take: the ranges' block %u B carved, the %u B above it closed by %u caps (%u B), "
               "%u walks\n",
               (unsigned)r.blockBytes, (unsigned)r.aboveBytes, (unsigned)r.caps, (unsigned)r.capBytes,
               (unsigned)r.walks);
        // (the caps' footprints T's, or T's but a 12 B block no request takes)
        TEST_ASSERT_TRUE(r.capBytes == r.aboveBytes + 4 || r.capBytes + 16 == r.aboveBytes + 4);
      }
    }
    std::vector<size_t> offs;
    for (size_t n : {352, 19208, 1100, 1180}) offs.push_back(ModelHeap::offsetOf(ModelHeap::alloc(n)));
    hold.give(h, block);
    TEST_ASSERT_EQUAL_INT(0, hold.held());
    return std::make_pair(offs, Map(ModelHeap::blocks));
  };
  const auto plain = tail(false, false), fixed = tail(true, true), open = tail(true, false);
  TEST_ASSERT_TRUE(fixed.first == plain.first);
  TEST_ASSERT_TRUE(sameMap(plain.second, fixed.second));
  // As built: the sprite (and the queue) in T, the maps apart.
  TEST_ASSERT_FALSE(open.first == plain.first);
  TEST_ASSERT_FALSE(sameMap(plain.second, open.second));
}

// U3: a cap that lands elsewhere: another block heads T's own list when the
// cap is asked (a filler takes T's head, a block of the same class is
// carved from the big block with a guard above it, the filler given back
// (T again), then that block given back: it heads the list now). The cap
// comes back there: Missed, given back at once, no cap held, T still free;
// the block given back, the heap as before the take with the guard.
namespace {
int g_capArmed = 0, g_capSkip = 0;  // (skip: the caps carved as asked before the misled one)
void* g_y = nullptr;
void* g_guard = nullptr;
void* allocMisled(size_t n) {
  if (g_capArmed == 2 && g_capSkip > 0) {
    --g_capSkip;
  } else if (g_capArmed == 2) {
    g_capArmed = 3;
    void* filler = ModelHeap::alloc(n);  // T's head
    g_y = ModelHeap::alloc(n);           // T's list empty: the big block's
    g_guard = ModelHeap::alloc(4096);    // (fits no hole: right above it)
    ModelHeap::release(filler);          // T again, at its list's head
    ModelHeap::release(g_y);             // ... and this one, the head now
  }
  if (g_capArmed == 1 && n == 1284) g_capArmed = 2;  // (the block: the next carve is the cap's)
  return ModelHeap::alloc(n);
}
}  // namespace

void test_a_cap_that_lands_elsewhere() {
  bootHeap();
  const Map start = ModelHeap::blocks;
  autodj::TopHold hold;
  autodj::TopHold::Hooks h = kModelTop;
  h.alloc = allocMisled;
  g_capArmed = 1;
  autodj::CloseReport r;
  void* block = hold.take(h, 1284, &r);
  TEST_ASSERT_EQUAL_INT(3, g_capArmed);
  TEST_ASSERT_NOT_NULL(block);
  TEST_ASSERT_TRUE(r.why == autodj::Close::Missed);
  TEST_ASSERT_EQUAL_UINT32(0, r.caps);
  TEST_ASSERT_TRUE(r.got == g_y);  // (the model's pointers: T's head is the walk's capAt)
  TEST_ASSERT_TRUE(r.want != g_y);
  TEST_ASSERT_EQUAL_size_t(ModelHeap::offsetOf(block) + ModelHeap::kHeader +
                               ModelHeap::blocks[ModelHeap::offsetOf(block)].size,
                           ModelHeap::offsetOf(r.want));
  TEST_ASSERT_EQUAL_size_t(r.aboveBytes, freeAboveOf(block));          // T still free, whole
  TEST_ASSERT_FALSE(ModelHeap::blocks[ModelHeap::offsetOf(g_y)].used);  // the cap given back at once
  hold.give(h, block);
  ModelHeap::release(g_guard);
  TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  g_capArmed = 0;
  // The same at the second cap (T takes two here): the first held at T's
  // head, the second back at Y: Missed, one cap kept, the miss given back at
  // once, what the first left of T still free; given back (the block, then
  // its one cap), the heap as before with the guard. (The setup's holes
  // 600 B here, a class under the rest the first cap leaves of T, about
  // 1,000 B: bootHeap's 1,000 B holes, carved 1,012 B, would serve Y before
  // the big block does.)
  {
    holesThenTop(std::vector<size_t>(5, 600), 834836);
    const Map start2 = ModelHeap::blocks;
    autodj::TopHold hold2;
    g_capArmed = 1;
    g_capSkip = 1;
    autodj::CloseReport r2;
    void* b2 = hold2.take(h, 1284, &r2);
    TEST_ASSERT_EQUAL_INT(3, g_capArmed);
    TEST_ASSERT_TRUE(r2.why == autodj::Close::Missed);
    TEST_ASSERT_EQUAL_UINT32(1, r2.caps);
    TEST_ASSERT_TRUE(r2.got == g_y);
    TEST_ASSERT_FALSE(ModelHeap::blocks[ModelHeap::offsetOf(g_y)].used);  // the miss given back at once
    // The cap at T's head, right above the block; the rest of T above it, free.
    auto it = std::next(ModelHeap::blocks.find(ModelHeap::offsetOf(b2)));
    TEST_ASSERT_TRUE(it->second.used);
    void* cap = ModelHeap::mem.data() + it->first + ModelHeap::kHeader;
    TEST_ASSERT_TRUE(r2.want != g_y);
    TEST_ASSERT_EQUAL_size_t(r2.aboveBytes - r2.capBytes, freeAboveOf(cap));
    TEST_ASSERT_EQUAL_size_t(ModelHeap::offsetOf(cap) + ModelHeap::kHeader + it->second.size,
                             ModelHeap::offsetOf(r2.want));
    TEST_ASSERT_EQUAL_INT(1, hold2.held());
    printf("[hold] a cap missing at the second: T %u B, 1 cap (%u B) kept, %u B of T left open\n",
           (unsigned)r2.aboveBytes, (unsigned)r2.capBytes, (unsigned)freeAboveOf(cap));
    hold2.give(h, b2);
    TEST_ASSERT_EQUAL_INT(0, hold2.held());
    ModelHeap::release(g_guard);
    TEST_ASSERT_TRUE(sameMap(start2, ModelHeap::blocks));
    g_capArmed = g_capSkip = 0;
  }
}

// U4 (hazard 4): another task's 4 KB probe (4,224 B as carved) before each
// carve of a take in turn (the spacers', the block's, each cap's), and before
// the first walk: every cap held at the address the walk gave, inside T; the
// take's outcome by where the probe came; given back with the probe freed,
// the heap as it was. A 60 KB Opus packet buffer before the first cap: T
// closed when it is under the buffer's class (62,464 B), else the buffer
// takes T and the cap misses.
namespace {
long g_besideCalls = 0, g_besideAt = -1;
size_t g_besideBytes = 4096;
void* g_beside = nullptr;
void* allocBeside(size_t n) {
  if (g_besideCalls++ == g_besideAt) g_beside = ModelHeap::alloc(g_besideBytes);
  return ModelHeap::alloc(n);
}
void probeBeforeWalk(uint32_t call) {
  if (call == 0) g_beside = ModelHeap::alloc(4096);
}
// Another task's 512 B carve right after the g_raceAt-th alloc call's (counted in g_besideCalls).
long g_raceAt = -1;
void* g_race = nullptr;
void* allocRacing(size_t n) {
  void* p = ModelHeap::alloc(n);
  if (g_besideCalls++ == g_raceAt) g_race = ModelHeap::alloc(512);
  return p;
}
long g_failCall = -1;
void* allocFailingAt(size_t n) {  // (the g_failCall-th alloc call, counted in g_besideCalls, finds no memory)
  if (g_besideCalls++ == g_failCall) return nullptr;
  return ModelHeap::alloc(n);
}
// A walk that gives up after one block (the firmware's visit bound, met
// before the block: none of these heaps' blocks is the first).
void boundedAbove(const void* block, const void* spacer, autodj::Above* out) {
  autodj::WalkAbove w(block, spacer, ModelHeap::kHeader, 1, out);
  walkModel(w);
}
}  // namespace

void test_another_task_s_carve_before_the_cap() {
  TEST_ASSERT_EQUAL_size_t(4224, ModelHeap::carved(4096));  // (a probe, and a FatFile's 4,148 B too)
  TEST_ASSERT_EQUAL_size_t(4224, ModelHeap::carved(4148));  // (PsramFs's FatFile, kFatFileBytes)
  // Where the carves of a take come: its alloc calls in order.
  bootHeap();
  const Map start = ModelHeap::blocks;
  autodj::TopHold::Hooks h = kModelTop;
  h.alloc = allocBeside;
  g_besideCalls = 0;
  g_besideAt = -1;
  autodj::TopHold free0;
  autodj::CloseReport r0;
  void* b0 = free0.take(h, 1284, &r0);
  const long calls = g_besideCalls;
  const int spacers = static_cast<int>(calls) - 1 - r0.caps;  // (the spacers', the block's, the caps')
  TEST_ASSERT_TRUE(r0.why == autodj::Close::Closed);
  free0.give(h, b0);
  TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  printf("[hold] a take's carves: %d spacers, the block, %u caps; T %u B\n", spacers, (unsigned)r0.caps,
         (unsigned)r0.aboveBytes);
  for (long k = 0; k < calls; ++k) {
    bootHeap();
    g_besideCalls = 0;
    g_besideAt = k;
    g_beside = nullptr;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = hold.take(h, 1284, &r);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_NOT_NULL(g_beside);
    char w[48];
    std::snprintf(w, sizeof(w), "the probe before carve %ld", k);
    // Every cap held inside the free block T was right above the block.
    const size_t t0 = ModelHeap::offsetOf(block) + ModelHeap::kHeader + ModelHeap::blocks[ModelHeap::offsetOf(block)].size;
    TEST_ASSERT_TRUE_MESSAGE(r.why == autodj::Close::Closed || r.why == autodj::Close::NotOnTop ||
                                 r.why == autodj::Close::Missed || r.why == autodj::Close::Nothing,
                             w);
    if (r.why == autodj::Close::Closed) {
      // Nothing a request can take between the block and the heap's end.
      for (auto it = ModelHeap::blocks.find(ModelHeap::offsetOf(block)); it != ModelHeap::blocks.end(); ++it)
        TEST_ASSERT_TRUE_MESSAGE(it->second.used || it->second.size < 16, w);
    }
    printf("[hold] the probe before a take's carve %ld (%s): %s, %u caps\n", k,
           k < spacers ? "a spacer's" : k == spacers ? "the block's" : "a cap's", autodj::closeName(r.why),
           (unsigned)r.caps);
    if (k == 0) {
      TEST_ASSERT_TRUE_MESSAGE(r.why == autodj::Close::Closed, w);  // (the probe in the big block's bottom)
    } else if (k < spacers) {
    } else if (k == spacers) {
      // In R's low end under the block: the block isn't on the spacers' top.
      TEST_ASSERT_TRUE_MESSAGE(r.why == autodj::Close::NotOnTop, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.caps, w);
    } else if (k == spacers + 1) {
      // At the first cap: the probe takes T's head, the cap's class is
      // T's floor and T is under it now: the cap comes from the big block.
      TEST_ASSERT_TRUE_MESSAGE(r.why == autodj::Close::Missed, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.caps, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(t0, ModelHeap::offsetOf(r.want), w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(t0, ModelHeap::offsetOf(g_beside), w);
      TEST_ASSERT_TRUE_MESSAGE(r.got < block, w);
    } else {
      // At a later cap: the caps before it held; the probe in the big
      // block; the rest closed.
      TEST_ASSERT_TRUE_MESSAGE(r.why == autodj::Close::Closed, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(r0.caps, r.caps, w);
    }
    hold.give(h, block);
    ModelHeap::release(g_beside);
    TEST_ASSERT_TRUE_MESSAGE(sameMap(start, ModelHeap::blocks), w);
  }
  // Between the give-back of the spacers and the first walk: a block in use
  // above (the probe, 4,224 B): Nothing, T's rest left open.
  {
    bootHeap();
    autodj::TopHold hold;
    autodj::TopHold::Hooks hw = kModelTop;
    g_beside = nullptr;
    g_aboveCalls = 0;
    g_aboveHook = probeBeforeWalk;
    autodj::CloseReport r;
    void* block = hold.take(hw, 1284, &r);
    g_aboveHook = nullptr;
    TEST_ASSERT_TRUE(r.why == autodj::Close::Nothing);
    TEST_ASSERT_EQUAL_size_t(4224, r.usedAbove);
    TEST_ASSERT_EQUAL_UINT32(0, r.caps);
    hold.give(hw, block);
    ModelHeap::release(g_beside);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // The Opus packet buffer (61,440 B: 62,464 B carved) before the first cap.
  {
    bootHeap();
    g_besideCalls = 0;
    g_besideAt = spacers + 1;
    g_besideBytes = 61440;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = hold.take(h, 1284, &r);
    g_besideBytes = 4096;
    const bool fits = r0.aboveBytes >= ModelHeap::carved(61440);
    printf("[hold] the Opus packet buffer before the first cap: T %u B, the buffer %u B carved: %s\n",
           (unsigned)r0.aboveBytes, (unsigned)ModelHeap::carved(61440), autodj::closeName(r.why));
    TEST_ASSERT_TRUE(r.why == (fits ? autodj::Close::Missed : autodj::Close::Closed));
    hold.give(h, block);
    ModelHeap::release(g_beside);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // No memory for the second cap (another task's carve took the room, or
  // the heap refuses): NoCap, the first cap kept at T's head, the rest of T
  // left open; the entry held part-capped, and given back (the block, then
  // its one cap) the heap as it was.
  {
    TEST_ASSERT_EQUAL_UINT32(2, r0.caps);  // (T takes two here)
    bootHeap();
    g_besideCalls = 0;
    g_besideAt = -1;
    g_failCall = spacers + 2;  // (the spacers', the block's, cap 1's, then cap 2's)
    autodj::TopHold::Hooks hf = kModelTop;
    hf.alloc = allocFailingAt;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = hold.take(hf, 1284, &r);
    g_failCall = -1;
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_TRUE(r.why == autodj::Close::NoCap);
    TEST_ASSERT_EQUAL_UINT32(1, r.caps);
    TEST_ASSERT_NULL(r.got);
    auto it = std::next(ModelHeap::blocks.find(ModelHeap::offsetOf(block)));
    TEST_ASSERT_TRUE(it->second.used);  // (cap 1, right above the block: T's head)
    void* cap = ModelHeap::mem.data() + it->first + ModelHeap::kHeader;
    TEST_ASSERT_EQUAL_size_t(r0.aboveBytes, r.aboveBytes);
    TEST_ASSERT_EQUAL_size_t(it->second.size + ModelHeap::kHeader, r.capBytes);
    TEST_ASSERT_TRUE(freeAboveOf(cap) > 0);
    TEST_ASSERT_EQUAL_size_t(r.aboveBytes - r.capBytes, freeAboveOf(cap));
    TEST_ASSERT_EQUAL_INT(1, hold.held());
    hold.give(hf, block);
    TEST_ASSERT_EQUAL_INT(0, hold.held());
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // Another task's carve right after the first cap's, before its walk (a
  // 512 B block: the cap leaves T's rest, about 1,000 B, whose list comes
  // before the 1,012 B holes', so it takes the rest's head): the walk meets
  // it in use above the cap, the caps' footprints short of T's: Left (not
  // Closed), one cap kept, the rest above that block open; given back (the
  // block, its cap), the heap as it was with that block.
  {
    bootHeap();
    g_besideCalls = 0;
    g_raceAt = spacers + 1;  // (cap 1's carve)
    g_race = nullptr;
    autodj::TopHold::Hooks hr = kModelTop;
    hr.alloc = allocRacing;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = hold.take(hr, 1284, &r);
    g_raceAt = -1;
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_NOT_NULL(g_race);
    printf("[hold] another task's block at the rest's head before the walk: %s, %u cap (%u B) of T's %u B, %u B "
           "left open above that block\n",
           autodj::closeName(r.why), (unsigned)r.caps, (unsigned)r.capBytes, (unsigned)r.aboveBytes,
           (unsigned)freeAboveOf(g_race));
    TEST_ASSERT_TRUE(r.why == autodj::Close::Left);
    TEST_ASSERT_EQUAL_UINT32(1, r.caps);
    TEST_ASSERT_TRUE(r.capBytes < r.aboveBytes + 4);
    auto it = std::next(ModelHeap::blocks.find(ModelHeap::offsetOf(block)));
    void* cap = ModelHeap::mem.data() + it->first + ModelHeap::kHeader;
    TEST_ASSERT_TRUE(it->second.used);
    TEST_ASSERT_EQUAL_size_t(ModelHeap::offsetOf(cap) + ModelHeap::kHeader + it->second.size,
                             ModelHeap::offsetOf(g_race));  // (right above the cap: the rest's head)
    TEST_ASSERT_TRUE(freeAboveOf(g_race) > 0);
    TEST_ASSERT_EQUAL_INT(1, hold.held());
    hold.give(hr, block);
    TEST_ASSERT_EQUAL_INT(0, hold.held());
    ModelHeap::release(g_race);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // The walk that doesn't meet the block (its bound reached first): Lost, no
  // cap, the block held (tracked), and given back freed.
  {
    bootHeap();
    autodj::TopHold::Hooks hb = kModelTop;
    hb.above = boundedAbove;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = hold.take(hb, 1284, &r);
    TEST_ASSERT_NOT_NULL(block);
    TEST_ASSERT_TRUE(r.why == autodj::Close::Lost);
    TEST_ASSERT_TRUE(r.bounded);
    TEST_ASSERT_EQUAL_UINT32(0, r.caps);
    TEST_ASSERT_EQUAL_UINT32(1, r.walks);
    TEST_ASSERT_EQUAL_INT(1, hold.held());
    TEST_ASSERT_TRUE(ModelHeap::blocks[ModelHeap::offsetOf(block)].used);
    hold.give(hb, block);
    TEST_ASSERT_EQUAL_INT(0, hold.held());
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
}

// U5: the hold's blocks given back bottom-up: the ranges' block, then the
// arena under it; the ranges' given first waits (nothing freed); the arena's
// frees the arena, its caps, the ranges' block, its caps, in rising
// addresses, never a free block above a block still held; then the heap as
// before. Given in the order taken back, each at once; a third take while
// two are held untracked (Full) and freed at once; nullptr and a block not
// taken here.
namespace {
std::vector<size_t> g_freed;
std::vector<size_t> g_holdRegion;  // the hold's blocks and caps (offsets), held
bool g_giving = false;             // (a give-back's frees watched; a take's spacers aren't)
void releaseWatched(void* p) {
  ModelHeap::release(p);
  if (!g_giving) return;
  g_freed.push_back(ModelHeap::offsetOf(p));
  // No free block a request can take above the lowest block still held.
  size_t lowest = SIZE_MAX;
  for (size_t o : g_holdRegion) {
    auto it = ModelHeap::blocks.find(o);
    if (it != ModelHeap::blocks.end() && it->second.used) lowest = std::min(lowest, o);
  }
  if (lowest == SIZE_MAX) return;
  for (auto it = ModelHeap::blocks.upper_bound(lowest); it != ModelHeap::blocks.end(); ++it)
    TEST_ASSERT_TRUE_MESSAGE(it->second.used || it->second.size < 16, "a free block opened above a held one");
}
}  // namespace

void test_the_hold_given_back_bottom_up() {
  bootHeap();
  const Map start = ModelHeap::blocks;
  autodj::TopHold::Hooks h = kModelTop;
  autodj::TopHold hold;
  autodj::CloseReport rr, ra;
  auto give = [&](void* p) {
    g_giving = true;
    hold.give(h, p);
    g_giving = false;
  };
  void* ranges = hold.take(h, 1284, &rr);
  void* arena = hold.take(h, statsfold::kArenaBytes, &ra);
  TEST_ASSERT_TRUE(rr.why == autodj::Close::Closed);
  TEST_ASSERT_TRUE(ra.why == autodj::Close::Closed);
  TEST_ASSERT_EQUAL_INT(2, hold.held());
  TEST_ASSERT_TRUE(arena < ranges);
  printf("[hold] the ranges' block (%u B above it, %u caps) and the arena under it (%u B above it, %u caps): %u B "
         "free held\n",
         (unsigned)rr.aboveBytes, (unsigned)rr.caps, (unsigned)ra.aboveBytes, (unsigned)ra.caps,
         (unsigned)ModelHeap::freeBytes());
  g_holdRegion.clear();
  for (auto it = ModelHeap::blocks.find(ModelHeap::offsetOf(arena)); it != ModelHeap::blocks.end(); ++it)
    if (it->second.used) g_holdRegion.push_back(it->first);
  TEST_ASSERT_EQUAL_size_t(2 + rr.caps + ra.caps, g_holdRegion.size());  // (nothing else up there)
  h.release = releaseWatched;
  g_freed.clear();
  const Map held = ModelHeap::blocks;
  give(ranges);  // (commit()'s endBoot: the arena still held)
  TEST_ASSERT_EQUAL_INT(2, hold.held());
  TEST_ASSERT_EQUAL_size_t(0, g_freed.size());
  TEST_ASSERT_TRUE(sameMap(held, ModelHeap::blocks));
  give(arena);  // (the save landed: the arena, then the ranges' block waiting for it)
  TEST_ASSERT_EQUAL_INT(0, hold.held());
  TEST_ASSERT_TRUE(g_freed == g_holdRegion);  // (in rising addresses: the arena, its caps, the ranges', its caps)
  TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  // In the order taken back: each at once.
  ranges = hold.take(h, 1284, &rr);
  arena = hold.take(h, statsfold::kArenaBytes, &ra);
  g_freed.clear();
  give(arena);
  TEST_ASSERT_EQUAL_INT(1, hold.held());
  TEST_ASSERT_EQUAL_size_t(1 + ra.caps, g_freed.size());
  // A third take while two are held: Full, untracked, freed at once.
  void* again = hold.take(h, statsfold::kArenaBytes, &ra);
  void* third = hold.take(h, 1000, &ra);
  TEST_ASSERT_TRUE(ra.why == autodj::Close::Full);
  TEST_ASSERT_EQUAL_INT(2, hold.held());
  g_freed.clear();
  give(third);
  TEST_ASSERT_EQUAL_size_t(1, g_freed.size());
  give(nullptr);
  void* other = ModelHeap::alloc(100);
  give(other);  // (not a top block: freed at once)
  TEST_ASSERT_EQUAL_size_t(2, g_freed.size());
  TEST_ASSERT_EQUAL_INT(2, hold.held());
  give(again);
  give(ranges);
  TEST_ASSERT_EQUAL_INT(0, hold.held());
  TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
}

// U6 (rule 2): a block a smaller free block took (a hole: here 8 KB, in a
// smaller class than what the spacers leave) isn't on the spacers' top: no
// cap, so a block carved next goes into the hole's rest, as on main.
void test_a_block_in_a_hole_gets_no_cap() {
  auto run = [](bool take) {
    holesThenTop({8000}, 834836);
    const size_t hole = ModelHeap::freeBlocks(4096).front().first;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* block = take ? hold.take(kModelTop, 1284, &r) : nullptr;
    if (take) {
      TEST_ASSERT_TRUE(r.why == autodj::Close::NotOnTop);
      TEST_ASSERT_EQUAL_UINT32(0, r.caps);
      TEST_ASSERT_EQUAL_size_t(hole, ModelHeap::offsetOf(block));
    }
    void* next = ModelHeap::alloc(6000);
    const size_t at = ModelHeap::offsetOf(next);
    hold.give(kModelTop, block);
    TEST_ASSERT_TRUE(at >= hole && at < hole + ModelHeap::carved(8000));  // (in the hole, as main's)
    return at - hole;
  };
  printf("[hold] a block in a hole: the next 6,000 B at the hole's +%u B (main's +%u B)\n", (unsigned)run(true),
         (unsigned)run(false));
}

// U7 (rule 3): the caps never leave under the margin free (keepFree and a
// KB): one byte short of it, left open; at it, closed. And a take keeps
// keepFree free with its block carved, whatever the block (the arena's
// 262 KB included: the spacers count its carve, as the join counts its own).
namespace {
size_t g_freeNow = 0;
size_t freeIs() { return g_freeNow; }
}  // namespace

void test_the_caps_keep_the_margin() {
  for (size_t f : {1276, 58212, 60268}) {
    for (int over = 0; over < 2; ++over) {
      void* a = gapAbove(f, false);
      autodj::CloseHooks c = modelClose();
      c.freeBytes = freeIs;
      c.keepFree = kMargin;
      g_freeNow = f + 4 + kMargin + 1024 - (over ? 0 : 1);
      autodj::Caps caps;
      autodj::CloseReport r;
      const autodj::Close why = autodj::closeAbove(a, nullptr, c, &caps, &r);
      TEST_ASSERT_TRUE(why == (over ? autodj::Close::Closed : autodj::Close::Margin));
      TEST_ASSERT_EQUAL_UINT32(over ? caps.n : 0, r.caps);
      if (!over) TEST_ASSERT_EQUAL_size_t(f, freeAboveOf(a));
    }
  }
  for (size_t n : {size_t{1284}, size_t{3076}, statsfold::kArenaBytes}) {
    bootHeap();
    autodj::TopHold hold;
    autodj::CloseReport r;
    ModelHeap::mark();
    void* p = hold.take(kModelTop, n, &r);
    printf("[hold] a take of %u B: the lowest free %u B (keepFree %u B), %s, %u caps\n", (unsigned)n,
           (unsigned)ModelHeap::low, (unsigned)kMargin, autodj::closeName(r.why), (unsigned)r.caps);
    TEST_ASSERT_TRUE(ModelHeap::low >= kMargin);
    TEST_ASSERT_TRUE(r.why == autodj::Close::Closed);
    hold.give(kModelTop, p);
  }
}

// U8: a heap too full for a spacer: the block where the heap puts it, and
// nothing closed (the rest of the largest block above it never capped);
// and no walk (no `above` hook): nothing closed either.
void test_no_spacers_no_cap() {
  ModelHeap::reset(kMargin + 1024 + 4096);
  autodj::TopHold hold;
  autodj::CloseReport r;
  void* p = hold.take(kModelTop, 1284, &r);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_TRUE(r.why == autodj::Close::NoSpacers);
  TEST_ASSERT_EQUAL_UINT32(0, r.caps);
  TEST_ASSERT_EQUAL_size_t(0, ModelHeap::offsetOf(p));
  TEST_ASSERT_TRUE(freeAboveOf(p) > 16);
  TEST_ASSERT_EQUAL_INT(1, hold.held());
  hold.give(kModelTop, p);
  TEST_ASSERT_EQUAL_INT(0, hold.held());
  // No memory for the block: nothing held.
  TEST_ASSERT_NULL(hold.take(kModelTop, 1u << 20, &r));
  TEST_ASSERT_TRUE(r.why == autodj::Close::NoBlock);
  TEST_ASSERT_EQUAL_INT(0, hold.held());
  bootHeap();
  autodj::TopHold::Hooks h = kModelTop;
  h.above = nullptr;
  p = hold.take(h, 1284, &r);
  TEST_ASSERT_TRUE(r.why == autodj::Close::Nothing);
  TEST_ASSERT_EQUAL_UINT32(0, r.walks);
  hold.give(h, p);
}

// U9 (rule 3 after the first cap): another task frees the block right above
// T between two caps (a decoder's, the card worker's, Bluetooth's). It
// merges into what the first cap left, heads its list at the next walk's
// capAt, so the next cap would land there (the address check passes) and
// take the freed block's room with no margin asked. Left: the first cap
// kept, the rest open, free never under the margin; the caps given back,
// the heap as the free left it.
namespace {
void* g_freedAbove = nullptr;
void freeAboveAtSecondWalk(uint32_t call) {
  if (call == 1 && g_freedAbove) {
    ModelHeap::release(g_freedAbove);
    g_freedAbove = nullptr;
  }
}
// [A 512 B][X][T: exactly f][Y: y B in use][Z: z B free]: X's pointer (the
// block under T), Y's into *yOut.
void* gapUnderUsed(size_t f, size_t y, size_t z, void** yOut) {
  const size_t ca = ModelHeap::carved(512), cx = ModelHeap::carved(f + 16), cy = ModelHeap::carved(y);
  ModelHeap::reset(4 * ModelHeap::kHeader + ca + cx + cy + z);
  TEST_ASSERT_NOT_NULL(ModelHeap::alloc(512));
  void* x = ModelHeap::alloc(f + 16);
  *yOut = ModelHeap::alloc(y);
  TEST_ASSERT_NOT_NULL(*yOut);
  TEST_ASSERT_EQUAL_size_t(z, ModelHeap::rawLargest());
  TEST_ASSERT_TRUE(ModelHeap::shrink(x, cx - f - ModelHeap::kHeader - ModelHeap::kPoison) == x);
  TEST_ASSERT_EQUAL_size_t(f, freeAboveOf(x));
  return x;
}
}  // namespace

void test_a_block_freed_into_t_between_caps() {
  const size_t f = 58212, y = 100 * 1024, z = 80000;
  // The heap as the free leaves it, no caps: the reference.
  void* y0 = nullptr;
  gapUnderUsed(f, y, z, &y0);
  ModelHeap::release(y0);
  const Map after = ModelHeap::blocks;
  void* a = gapUnderUsed(f, y, z, &g_freedAbove);
  autodj::CloseHooks c = modelClose();
  c.freeBytes = ModelHeap::freeBytes;
  c.keepFree = kMargin;
  TEST_ASSERT_TRUE(ModelHeap::freeBytes() >= f + 4 + kMargin + 1024);  // (the first cap's margin holds)
  ModelHeap::mark();
  g_aboveCalls = 0;
  g_aboveHook = freeAboveAtSecondWalk;
  autodj::Caps caps;
  autodj::CloseReport r;
  const autodj::Close why = autodj::closeAbove(a, nullptr, c, &caps, &r);
  g_aboveHook = nullptr;
  printf("[hold] a block freed into T between caps: %s, %u cap%s (%u B), the lowest free %u B (the margin %u B)\n",
         autodj::closeName(why), (unsigned)caps.n, caps.n == 1 ? "" : "s", (unsigned)r.capBytes,
         (unsigned)ModelHeap::low, (unsigned)(kMargin + 1024));
  TEST_ASSERT_NULL(g_freedAbove);  // (freed before the second walk)
  TEST_ASSERT_TRUE(why == autodj::Close::Left);
  TEST_ASSERT_EQUAL_UINT32(1, caps.n);
  TEST_ASSERT_EQUAL_UINT32(1, r.caps);
  TEST_ASSERT_EQUAL_size_t(f, r.aboveBytes);
  TEST_ASSERT_TRUE(ModelHeap::low >= kMargin + 1024);
  for (int k = 0; k < caps.n; ++k) ModelHeap::release(caps.p[k]);
  TEST_ASSERT_TRUE(sameMap(after, ModelHeap::blocks));
  // Without the free: closed by two caps, as U1 has it.
  a = gapUnderUsed(f, y, z, &y0);
  autodj::Caps c2;
  autodj::CloseReport r2;
  TEST_ASSERT_TRUE(autodj::closeAbove(a, nullptr, c, &c2, &r2) == autodj::Close::Closed);
  TEST_ASSERT_EQUAL_UINT32(2, c2.n);
}

// U13 (the recheck, hazard 5): a decoder's room. When one may open in the
// hold (the firmware's: the ranges' block at every take, the arena while a
// track plays; U14-U16) a take asks room for the Opus's buffers
// (statsglue::kDecoderRoomBytes): in a roomy heap the
// caps stay (Closed); where the caps would leave no free block for them,
// every cap the hold holds is given back (Room: T open again, the older
// block's too), and a block that can yield (the arena) is given back too
// when even that isn't enough (NoRoom: nullptr, nothing more held). Without
// the room the same heap keeps the caps, and the Opus doesn't open.
void test_a_decoder_s_room() {
  const size_t room = statsglue::kDecoderRoomBytes;
  auto opusOpens = [] {
    std::vector<void*> got;
    for (size_t n : {65307, 61440, 1276})
      if (void* p = ModelHeap::alloc(n)) got.push_back(p);
    const bool all = got.size() == 3;
    for (void* p : got) ModelHeap::release(p);
    return all;
  };
  // Roomy (the boot's heap): the room asked changes nothing.
  {
    bootHeap();
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* p = hold.take(kModelTop, 1284, &r, true, room);
    TEST_ASSERT_TRUE(r.why == autodj::Close::Closed);
    TEST_ASSERT_EQUAL_UINT32(0, r.uncapped);
    TEST_ASSERT_TRUE(opusOpens());
    hold.give(kModelTop, p);
  }
  // One free block of 180 KB: the ranges' caps would leave about 110 KB.
  for (bool yield : {false, true}) {
    holesThenTop({}, 180 * 1024);
    const Map start = ModelHeap::blocks;
    autodj::TopHold hold;
    autodj::CloseReport r;
    void* p = hold.take(kModelTop, 1284, &r, true, room, yield);
    printf("[hold] a decoder's room, one free block of 184,320 B, the ranges' block%s: %s, %u caps given back, the "
           "largest free block %u B\n",
           yield ? " (may yield)" : "", autodj::closeName(r.why), (unsigned)r.uncapped, (unsigned)ModelHeap::largest());
    if (yield) {
      // (the caps gone, still short: the block given back too)
      TEST_ASSERT_NULL(p);
      TEST_ASSERT_TRUE(r.why == autodj::Close::NoRoom);
      TEST_ASSERT_EQUAL_INT(0, hold.held());
      TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
      continue;
    }
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(r.why == autodj::Close::Room);
    TEST_ASSERT_TRUE(r.caps > 0 && r.uncapped == r.caps);
    TEST_ASSERT_EQUAL_size_t(r.aboveBytes, freeAboveOf(p));  // (T open again, whole)
    TEST_ASSERT_TRUE(opusOpens());                            // (in T and under the block)
    TEST_ASSERT_EQUAL_INT(1, hold.held());
    hold.give(kModelTop, p);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // A full log's hold, one free block of 480 KB: the ranges' block (closed:
  // room left), then the arena. Without the room both stay capped and the
  // Opus finds no room; with it, every cap is given back (the ranges' too),
  // and, that short as well, the arena is given back: its chunks would come
  // one by one. The ranges' block stays held, open.
  for (bool asked : {false, true}) {
    holesThenTop({}, 480 * 1024);
    const Map start = ModelHeap::blocks;
    autodj::TopHold hold;
    autodj::CloseReport rr, ra;
    void* ranges = hold.take(kModelTop, 1284, &rr, true, asked ? room : 0);
    TEST_ASSERT_TRUE(rr.why == autodj::Close::Closed);
    void* arena = hold.take(kModelTop, statsfold::kArenaBytes, &ra, true, asked ? room : 0, true);
    printf("[hold] a decoder's room, one free block of 491,520 B, the ranges' block (%u caps) then the arena, the "
           "room %s: %s, %u caps given back; the largest free block %u B: the Opus %s\n",
           (unsigned)rr.caps, asked ? "asked" : "not asked", autodj::closeName(ra.why), (unsigned)ra.uncapped,
           (unsigned)ModelHeap::largest(), opusOpens() ? "opens" : "DOESN'T OPEN");
    if (!asked) {
      TEST_ASSERT_TRUE(ra.why == autodj::Close::Closed);
      TEST_ASSERT_FALSE(opusOpens());  // (the hazard: the caps keep the margin, not its room)
      hold.give(kModelTop, ranges);
      hold.give(kModelTop, arena);
    } else {
      TEST_ASSERT_NULL(arena);
      TEST_ASSERT_TRUE(ra.why == autodj::Close::NoRoom);
      TEST_ASSERT_EQUAL_UINT32(rr.caps + ra.caps, ra.uncapped);  // (the ranges' caps given back too)
      TEST_ASSERT_EQUAL_INT(1, hold.held());
      TEST_ASSERT_EQUAL_size_t(rr.aboveBytes, freeAboveOf(ranges));  // (its T open again)
      TEST_ASSERT_TRUE(opusOpens());
      hold.give(kModelTop, ranges);
    }
    TEST_ASSERT_EQUAL_INT(0, hold.held());
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
}

// U10: no free block takes the block (no 262 KB one for the overflow's
// arena: after a library update, mid-session): NoBlock, said without a
// carve (the firmware's failed heap_caps_malloc prints "[heap] FAILED" and
// counts), nothing held, the heap as it was. largest() is exact: a block
// that would just take it is carved.
namespace {
long g_carves = 0;
void* allocCounted(size_t n) {
  ++g_carves;
  return ModelHeap::alloc(n);
}
}  // namespace

void test_no_block_asks_no_carve() {
  autodj::TopHold::Hooks h = kModelTop;
  h.alloc = allocCounted;
  for (size_t top : {size_t{200000}, size_t{262140}, size_t{262144}}) {
    holesThenTop(std::vector<size_t>(3, 100000), top);
    const Map start = ModelHeap::blocks;
    const size_t largest = ModelHeap::largest();
    const bool fits = largest >= statsfold::kArenaBytes;
    autodj::TopHold hold;
    autodj::CloseReport r;
    g_carves = 0;
    void* p = hold.take(h, statsfold::kArenaBytes, &r);
    printf("[hold] the arena (%u B) with the largest free block %u B (largest() %u B): %s, %ld carves\n",
           (unsigned)statsfold::kArenaBytes, (unsigned)top, (unsigned)largest, autodj::closeName(r.why), g_carves);
    if (!fits) {
      TEST_ASSERT_NULL(p);
      TEST_ASSERT_TRUE(r.why == autodj::Close::NoBlock);
      TEST_ASSERT_EQUAL_INT32(0, g_carves);
      TEST_ASSERT_EQUAL_INT(0, hold.held());
      TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
      TEST_ASSERT_NULL(ModelHeap::alloc(statsfold::kArenaBytes));  // (the heap itself refuses it)
      continue;
    }
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(g_carves > 0);
    TEST_ASSERT_EQUAL_INT(1, hold.held());
    hold.give(h, p);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
}

// U11: the walk's decisions (autodj::WalkAbove, the firmware's walkStep()
// a line over it) on synthetic walks as heap_caps_walk() hands them: the
// blocks by the walker's pointer (the alloc's less `head`: 8 B with light
// poisoning, 0 without), heap by heap. The block mid-heap with a free block
// above it (its cap's pointer that block's + head); the block last in its
// heap, the next heap's first block next (nothing above); the free block
// under it holding the spacer, not holding it, or in use (onSpacers); the
// block's pointer met free (not found); the bound before the block (Lost)
// and at it (found).
void test_the_walk_above() {
  struct B {
    intptr_t heap;
    uintptr_t at;
    size_t size;
    bool used;
  };
  const intptr_t h1 = 0x3f800000, h2 = 0x3fa00000;
  // Heap 1: free 1,000 B, used 2,000 B, free 5,000 B, used 100 B (its last);
  // heap 2: free 7,000 B, used 64 B. Each block's walker pointer is the one
  // before it's + its size + 4 (the size field).
  std::vector<B> walk;
  uintptr_t at = static_cast<uintptr_t>(h1);
  for (const auto& b : std::vector<std::pair<size_t, bool>>{{1000, false}, {2000, true}, {5000, false}, {100, true}}) {
    walk.push_back({h1, at, b.first, b.second});
    at += b.first + 4;
  }
  at = static_cast<uintptr_t>(h2);
  for (const auto& b : std::vector<std::pair<size_t, bool>>{{7000, false}, {64, true}}) {
    walk.push_back({h2, at, b.first, b.second});
    at += b.first + 4;
  }
  for (size_t head : {size_t{8}, size_t{0}}) {
    auto run = [&](const std::vector<B>& bs, size_t i, const void* spacer, uint32_t bound, uint32_t* steps) {
      autodj::Above a;
      autodj::WalkAbove w(reinterpret_cast<const void*>(bs[i].at + head), spacer, head, bound, &a);
      *steps = 0;
      for (const B& b : bs) {
        ++*steps;
        if (!w.step(b.heap, b.at, b.size, b.used)) break;
      }
      if (w.done) {
        const autodj::Above before = a;
        TEST_ASSERT_FALSE(w.step(bs[0].heap, bs[0].at, bs[0].size, bs[0].used));  // (done: the rest ignored)
        TEST_ASSERT_EQUAL_UINT32(before.visits, a.visits);
      }
      return a;
    };
    char m[48];
    std::snprintf(m, sizeof(m), "head %u", (unsigned)head);
    uint32_t steps = 0;
    // Mid-heap, a free block above: its size, its cap at its pointer + head.
    const void* inPrev = reinterpret_cast<const void*>(walk[0].at + 100 + head);  // (a spacer given back into it)
    autodj::Above a = run(walk, 1, inPrev, 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.bounded && a.onSpacers && a.free, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(2000, a.blockBytes, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(5000, a.bytes, m);
    TEST_ASSERT_TRUE_MESSAGE(a.capAt == reinterpret_cast<void*>(walk[2].at + head), m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, a.visits, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, steps, m);  // (it stops at the block above)
    // The spacer at the free block's own pointer (its first byte): on it.
    a = run(walk, 1, reinterpret_cast<const void*>(walk[0].at + head), 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.onSpacers, m);
    // The spacer not in the free block under it (above the block), or none.
    a = run(walk, 1, reinterpret_cast<const void*>(walk[2].at + head), 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.onSpacers, m);
    a = run(walk, 1, nullptr, 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.onSpacers && a.free, m);
    // The block under it in use: not on the spacers, the spacer's address
    // in it or not.
    std::vector<B> usedUnder = walk;
    usedUnder[0].used = true;
    a = run(usedUnder, 1, reinterpret_cast<const void*>(walk[0].at + 100 + head), 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.onSpacers && a.free, m);
    // The block last in its heap (the spacer in the free block under it: on
    // it): the next heap's first block is no block above it (free false,
    // 0 B, no cap).
    a = run(walk, 3, reinterpret_cast<const void*>(walk[2].at + 4000 + head), 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && a.onSpacers && !a.free && !a.capAt, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, a.bytes, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(100, a.blockBytes, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, steps, m);
    // The walk's last block: nothing above it either.
    a = run(walk, 5, nullptr, 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.free && !a.capAt, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(0, a.bytes, m);
    // A heap's first block, the free block before it the last of another
    // heap (holding the spacer's address): not on it.
    std::vector<B> crossing = walk;
    crossing[3].used = false;  // (heap 1's last block free; heap 2's first in use)
    crossing[4].used = true;
    a = run(crossing, 4, reinterpret_cast<const void*>(crossing[3].at + head), 0, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.onSpacers, m);
    // The block's pointer met free: not found, every block looked at.
    a = run(walk, 0, nullptr, 0, &steps);
    TEST_ASSERT_FALSE_MESSAGE(a.found || a.bounded || a.free, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(walk.size()), a.visits, m);
    // The bound: reached before the block (block 2 of heap 1 is the 2nd
    // looked at: bound 1 gives up), at it (bound 2: found), and past it the
    // block above still read (the bound is only before the block).
    a = run(walk, 1, nullptr, 1, &steps);
    TEST_ASSERT_TRUE_MESSAGE(!a.found && a.bounded && !a.free, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, a.visits, m);
    a = run(walk, 1, nullptr, 2, &steps);
    TEST_ASSERT_TRUE_MESSAGE(a.found && !a.bounded && a.free, m);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(5000, a.bytes, m);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, a.visits, m);
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
    // Off: the walk's blocks from the largest block's low end (largest()
    // only tells of its class, a thirty-second of it).
    const size_t walked = 4 * (ModelHeap::carved(sh.walk) + ModelHeap::kHeader);
    TEST_ASSERT_TRUE(off.givenBack + walked <= off.largestAtJoin + off.largestAtJoin / 32);
    TEST_ASSERT_TRUE(off.givenBack + walked + off.givenBack / 32 >= off.largestAtJoin);
    // At the top: the maps take the block's top (less than 24 KB above
    // them), the walk its bottom, and the join given back leaves the very
    // block Auto DJ off leaves.
    TEST_ASSERT_TRUE(placed.largestJoined <= off.givenBack + off.givenBack / 32 - placed.joinBytes);
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
  const size_t top = ModelHeap::rawLargest();
  TEST_ASSERT_TRUE(top >= 300000 + 300000 / 32 + 1024);
  TEST_ASSERT_TRUE(top <= 300000 + 300000 / 32 + 1024 + top / 32 + 4096 + 256);
  autodj::giveSpacers(&sp, ModelHeap::release);
  TEST_ASSERT_EQUAL_INT(0, sp.n);
  TEST_ASSERT_EQUAL_size_t(1000000 - ModelHeap::kHeader, ModelHeap::rawLargest());
  TEST_ASSERT_FALSE(autodj::takeSpacers(990000, 0, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                        ModelHeap::release, &sp));
  TEST_ASSERT_EQUAL_INT(0, sp.n);
  TEST_ASSERT_EQUAL_size_t(0, autodj::spacerFor(300000 + 300000 / 32 + 1024 + 4096, 300000));
  TEST_ASSERT_TRUE(autodj::takeSpacers(300000, 600000, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc,
                                       ModelHeap::release, &sp));
  TEST_ASSERT_TRUE(ModelHeap::freeBytes() >= 600000);
  TEST_ASSERT_TRUE(ModelHeap::rawLargest() >= 600000 - 64);  // (what keepFree leaves, at the block's top)
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
  size_t joined = 0;           // ... joined again (and the listening stats' records' save made)
  size_t joinOnly = 0;         // ... the join given back (the memory check's give-back)
  size_t givenBack = 0;        // ... the same (the stats give nothing back since 5.14's rework)
  size_t topKept = 0;          // the kept bytes the join put at the top
  size_t bootLowest = SIZE_MAX;  // the lowest free while the boot's join placed its blocks
  bool opusFirst = false;        // a history's Opus at the boot opened before the boot's join
  size_t lowest = SIZE_MAX;      // ... while the joins after the update did
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

// A run from T as well as D (D's rows then Software rows: MOOD-FILTERS.md
// 9.5 C1, 9.6 C6), its records with their mood tails or not: with them,
// library.idx and each build hold the words' block (2 B a track), the
// load's last block and, in a build, taken at the first track with a word.
struct MoodRun {
  cc::Source* transfer = nullptr;
  bool moods = false;  // T's records carry mood tails: the index has words
};

// The listening stats (docs/LISTENING-STATS.md 5.14: S1's rework, a fixed
// resident set; its R6): the firmware's own glue (statsglue::Glue, its Fold
// and its store) on a card in memory, every PSRAM block it carves through
// the model heap as the firmware's hooks carve them (app/ListeningStats.cpp):
// - the glue's object is internal RAM's on the device (.bss, at most
//   2,048 B: 5.14's "Moved to internal RAM", after the device's FLAC row),
//   outside the model heap; at PlayHistory::begin()'s place (setup(), after
//   Auto DJ's glue) its read (Glue::begin(), the boot's, with its waits):
//   the work memory and a FatFile for each file open, given back in the
//   call, and for a card with more than 8 MARK rows MARK's block, kept for
//   the session where the heap puts it (Memory::keep: the firmware's
//   psramAlloc, 5.14's fix G; the internal-RAM review: internal RAM holds
//   the object alone, whatever the card);
// - the boot's scan, from its first reserve() (the fold begun): the ranges'
//   block and a log's overflow arena (all 16 chunks' room) at the top of the
//   largest free block through the join's spacers keeping kMargin free with
//   the block carved, the free block they left above each closed by caps
//   (Memory::top and topRelease: the firmware's topAlloc() and topRelease(),
//   an autodj::TopHold, 5.14's fix A), held across the scan's passes until
//   the boot's save gives them back, bottom-up (the ranges' block, given
//   back first by commit(), waits for the arena: freed once it lands, or
//   after its fourth try); the arena only for a fold past the 40 records
//   (the firmware's reserve() asks no look-ahead:
//   statsglue::kReserveAhead; the fold carves it at the 41st);
//   the ranges' block 1,284 B with no ACKS row above the card's MARK, 3,076 B
//   with 16 (StatsRun::ackRows: a card a 0.14.0 player synced first);
// - each save's work memory (Memory::alloc) and its FatFiles (PsramFs), in
//   the store's own order of opens and closes, given back in its call: the
//   boot's (a compaction's two bases too), the records' (24 plays written
//   live) in each update's fence pass and after it, one at 40 records in
//   update 0's fence.
// The card the boot finds (StatsCard) is written by the same glue in
// earlier sessions, outside the model heap.
constexpr size_t kFatFileBytes = 4148;  // FatFs::open()'s psramNew<FatFile>(): a FIL with its own 4,096 B window
constexpr uint64_t kStatsNowMs = statsfold::kMinMs + 600ull * statsfold::kDayMs;  // the board's clock at the saves
const uint8_t kStatsDev[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
constexpr uint64_t kStatsEpoch = 0x1122334455667788ull;

uint64_t statsHash(uint32_t track) { return cc::fnv1a64(&track, sizeof(track)); }

// A completed manual play of `track` (its path hash statsHash()'s): slot
// `seq` of player `dev` (a (deviceId, epoch) each: a MARK row each).
playlog::Slot statsSlot(uint32_t seq, uint32_t track, uint64_t startedAt, uint32_t dev = 0) {
  playlog::Slot s;
  s.kind = playlog::kKindPlay;
  s.outcome = playlog::kCompleted;
  s.source = playlog::kSourceManual;
  s.clock = playlog::kClockRtc;
  s.seq = seq;
  s.epoch = kStatsEpoch + dev;
  std::memcpy(s.deviceId, kStatsDev, 6);
  s.deviceId[5] = static_cast<uint8_t>(kStatsDev[5] + dev);
  s.startedAt = startedAt;
  s.playedMs = s.durationMs = 200000;
  s.pathHash = statsHash(track);
  return s;
}

// The card a boot finds: stats.bin and its base as plays of `played` tracks
// left them (`trkd` of them in stats.bin's TRKD, the rest compacted into the
// base), each session written by the glue (malloc's memory); `marks`
// players' MARK rows; the log above the card's MARK from player 0's `seq`.
struct StatsCard {
  cutfs::CutFs card;
  uint32_t played = 0, trkd = 0, marks = 1;
  std::vector<uint32_t> seq;  // each player's next seq (above the card's MARK)
  uint32_t sessions = 0;
};

struct PlainStatsEnv final : statsglue::Env {
  statsfile::SaveAsk ask;
  uint64_t monoMs() override { return 0; }
  void asking(statsglue::Kind, statsfile::SaveAsk* a) override { *a = ask; }
};

// An earlier session: its boot folds one play each of tracks [t0, t0 + n),
// dated over `days` days from `day`, by `players` players in turn; its save
// compacts (nothing playing) or doesn't (a track playing, no safe point).
void statsSession(StatsCard& c, uint32_t n, uint32_t t0, uint32_t day, uint32_t days, uint32_t players,
                  bool compact) {
  namespace sf = statsfold;
  PlainStatsEnv env;
  env.ask.nowMs = kStatsNowMs;
  env.ask.playing = !compact;
  env.ask.safePoint = compact;
  const cc::msmf::Acks acks{};
  statsglue::Glue g(c.card, env);
  g.begin(acks, [](uint32_t) {});
  TEST_ASSERT_TRUE(g.on());
  g.scanBegan(false);
  for (uint32_t i = 0; i < n; ++i) {
    if (i % 128 == 0) TEST_ASSERT_TRUE(g.reserve(statsglue::kReserveAhead));
    const uint32_t d = i % players;
    const uint64_t at = sf::kMinMs + (day + static_cast<uint64_t>(i) * days / n) * sf::kDayMs + (i % 24) * sf::kHourMs;
    g.slot(statsSlot(c.seq[d]++, t0 + i, at, d), 0);
  }
  TEST_ASSERT_TRUE(g.save(playlog::Folds::Why::Boot));
  TEST_ASSERT_TRUE(g.last().what == statsfile::Saved::Wrote);
  TEST_ASSERT_EQUAL(compact, g.last().compacted);
  g.ready();
  ++c.sessions;
}

StatsCard statsCard(uint32_t played, uint32_t trkd, uint32_t marks = 1) {
  StatsCard c;
  c.played = played;
  c.trkd = trkd;
  c.marks = marks;
  c.seq.assign(marks, 1);
  // (A boot adds at most 8 players' rows to the card's MARK: the first
  // sessions' 8 at most, the last one's the rest.)
  uint32_t t = 0, day = 300;
  while (t + trkd < played) {
    const uint32_t n = std::min<uint32_t>(8000, played - trkd - t);
    statsSession(c, n, t, day, 100, std::min<uint32_t>(marks, statsfold::kMarkRoom), true);
    t += n;
    day += 100;
  }
  if (trkd) statsSession(c, trkd, t, day, 60, marks, false);
  return c;
}

// A card past the object's 16 MARK rows: the compacted sessions' 8 players,
// then TRKD's plays in sessions of 8 players more each (a boot adds at most
// kMarkRoom rows to the card's MARK).
StatsCard statsCardRows(uint32_t played, uint32_t trkd, uint32_t marks) {
  StatsCard c;
  c.played = played;
  c.trkd = trkd;
  c.marks = marks;
  c.seq.assign(marks, 1);
  uint32_t t = 0, day = 300;
  while (t + trkd < played) {
    const uint32_t n = std::min<uint32_t>(8000, played - trkd - t);
    statsSession(c, n, t, day, 100, std::min<uint32_t>(marks, statsfold::kMarkRoom), true);
    t += n;
    day += 100;
  }
  const uint32_t parts = (marks - 1) / statsfold::kMarkRoom;
  for (uint32_t k = 0; k < parts; ++k) {
    const uint32_t n = trkd / parts + (k < trkd % parts ? 1 : 0);
    statsSession(c, n, t, day, 60, std::min<uint32_t>(marks, statsfold::kMarkRoom * (k + 2)), false);
    t += n;
    day += 60;
  }
  return c;
}

// The card through the firmware's FatFs (src/storage/CardFat.cpp): a
// FatFile carved for each file open, before the open (given back when it
// fails), given back at its close; entry() an open and a close. (f_stat's
// FILINFO, carved and freed with nothing between, leaves no trace in this
// heap: not modelled.)
class PsramFs final : public tagstore::Fs {
public:
  explicit PsramFs(cutfs::CutFs& card) : card_(card) {}
  tagstore::File* open(const char* path, Mode mode) override {
    void* b = ModelHeap::alloc(kFatFileBytes);
    if (!b) return nullptr;
    return kept(card_.open(path, mode), b);
  }
  tagstore::File* openRead(const char* path, bool* cardFailed) override {
    void* b = ModelHeap::alloc(kFatFileBytes);
    if (!b) {
      *cardFailed = true;
      return nullptr;
    }
    return kept(card_.openRead(path, cardFailed), b);
  }
  Entry entry(const char* path) override {
    Entry e;
    tagstore::File* f = openRead(path, &e.cardFailed);
    if (f) {
      e.there = true;
      e.firstCluster = card_.firstCluster(path);
      close(f);
    }
    return e;
  }
  Seen look(const char* path) override { return card_.look(path); }
  bool close(tagstore::File* f) override {
    auto it = files_.find(f);
    TEST_ASSERT_TRUE_MESSAGE(it != files_.end(), "a file PsramFs didn't open");
    void* b = it->second;
    files_.erase(it);
    const bool ok = card_.close(f);
    ModelHeap::release(b);
    return ok;
  }
  bool exists(const char* path) override { return card_.exists(path); }
  bool remove(const char* path) override { return card_.remove(path); }
  bool rename(const char* from, const char* to) override { return card_.rename(from, to); }
  uint32_t firstCluster(const char* path) override { return card_.firstCluster(path); }
  size_t openNow() const { return files_.size(); }

private:
  tagstore::File* kept(tagstore::File* f, void* b) {
    if (!f) {
      ModelHeap::release(b);
      return nullptr;
    }
    files_[f] = b;
    return f;
  }
  cutfs::CutFs& card_;
  std::map<tagstore::File*, void*> files_;
};

// The glue's Env in a run: the board's clock (the overflow's tries), a
// decoder open (compactionDue()'s `playing`), the card refusing every write
// for the next `fails` saves; each save's outcome.
struct StatsEnv final : statsglue::Env {
  cutfs::CutFs* card = nullptr;
  uint64_t mono = 0;
  bool playing = false;
  int fails = 0;
  uint32_t saves = 0, wrote = 0, compacted = 0;
  std::function<void()> onAsking;  // (a carve in the save's window: after endBoot, before its work memory)
  uint64_t monoMs() override { return mono; }
  void asking(statsglue::Kind, statsfile::SaveAsk* a) override {
    if (onAsking) {
      onAsking();
      onAsking = nullptr;
    }
    a->nowMs = kStatsNowMs;
    a->hasCommit = false;
    a->playing = playing;
    a->safePoint = true;
    if (fails > 0) {
      card->refuse = true;
      --fails;
    }
  }
  void saved(statsglue::Kind, statsglue::Refused r, const statsfile::SaveReport& rep) override {
    card->refuse = false;
    ++saves;
    if (r == statsglue::Refused::None && rep.what == statsfile::Saved::Wrote) ++wrote;
    if (rep.compacted) ++compacted;
  }
};

// The rework's stats in a run of libraryUpdates(), and what they came to.
struct StatsRun {
  enum class Scan {
    ScanFirst,     // the boot's scan, then the boot's join (waiting while the scan's blocks are held)
    JoinFirst,     // the boot's join, then the scan
    AfterUpdate,   // a library update asked before the scan's first piece: the scan after update 0's rejoin
    ThroughBuild,  // (no update waiting for the scan's blocks) the scan after the boot's join, its blocks
                   // held through update 0's build, its save after it
    InFence,       // (no scan waiting for an asked update) the scan begun in update 0's fence, its save after
                   // the build
  };
  const StatsCard* card = nullptr;
  uint32_t slots = 30;      // the boot's log above the card's MARK: a usual boot's (at most 39: the last
                            // session's records), 300 (an overflow chunk), a full log's 13,106 (16 chunks)
  Scan when = Scan::ScanFirst;
  bool startWaits = true;   // a start at the boot waits while overflow chunks are held (false: the shape the
                            // wait exists for, the Opus opening under them)
  int bootFails = 0;        // the boot's save's tries that fail (the card refusing writes); kBootTries: behind
  int fenceSave = 0;        // a records' save at 40 records in update 0's fence: 1 before its build, 2 after it
  bool stepSaves = true;    // the records' save (24 plays written) in each update's fence pass and after it
  uint32_t ackRows = 0;     // the root's ACKS rows above the card's MARK (statsAcks(): at most 16, the ranges'
                            // block then at its largest, 3,076 B)
  uint32_t startAt = 0;     // (startWaits false) a start at the boot asked after this many of the scan's pieces
                            // of 128 slots (0: after its last piece)
  // The hold's cases (5.14, fix A + G): main's own long-lived carves inside the boot's hold or around it, at the
  // same points in the stats' run and in its twin without the stats (statsOff: main's run, the same pieces and the
  // same carves, no glue); what the hold leaves is compared between the two.
  static constexpr uint32_t kNever = UINT32_MAX, kInSave = UINT32_MAX - 1, kBetweenTries = UINT32_MAX - 2,
                            kBeforeJoin = UINT32_MAX - 3;
  bool statsOff = false;     // main's twin
  bool scanFile = false;     // the play log's FatFile (PlayLog's open: 4,148 B), one a log file, held while the scan
                             // reads it (main's too), carved after that piece's reserve()
  uint32_t uiAt = kNever;    // the first frame (NowPlayingPage's cover: the M5Canvas object 352 B and its 98 x 98
                             // sprite 19,208 B, kept; PanelScan's 4,096 B, freed at once): after the scan's piece k,
                             // in the boot save's window (kInSave: commit() asks the Env after endBoot, before its
                             // work memory), before the save's second try (kBetweenTries), or after the hold, before
                             // the boot's join (kBeforeJoin: main's order); kNever: none (the yardstick's runs)
  uint32_t queueAt = kNever; // a start from the console, a tap or a headphone key after the scan's piece k: the
                             // queue's blocks (1,100 + 1,180 B, kept) and an MP3's 4 KB probe (freed at once; none
                             // while overflow chunks hold a start)
  bool remount = false;      // after the boot's join, a FLAC playing: a remount's re-read (scanBegan(true), the same
                             // log again, all under the card's MARK), the next track's FLAC opened after its first
                             // piece (the old one's blocks freed first), its save, ready(); then the Dc
  bool takeHoles = false;    // (remount) every free block under 64 KB taken by a kept block first (both twins): the
                             // ranges' block then sits on the spacers' top
  bool nextOpus = false;     // (remount) an MP3 playing, not a FLAC (no PSRAM held: its start's 4 KB probe freed at
                             // once), the next track the session's first Opus, opening after the re-read's piece
                             // `nextAt` (never held: OutputHold holds a start, not a track that follows one playing):
                             // its start's probe, then its three buffers (kOpusAsks), kept for the session
                             // (OpusGenerator's, made at the first .opus); one that finds no room: those carved given
                             // back (allocBuffers()), the track skipped (nextShort)
  uint32_t nextAt = 0;
  size_t less = 0;           // (remount) the session's blocks this much more before it (a block kept: less free at
                             // the remount, a larger library or a library update's growth)
  bool fe61db9 = false;      // fe61db9's hooks (fe61db9Top(): the spacers keeping 64 KB alone, no caps, no arena: the
                             // chunks one by one; each block given back at once)
  uint32_t nextShort = 0;    // (nextOpus) the Opus's open found no room
  const std::vector<ModelHeap::Event>* replayRead = nullptr;  // (statsOff) the stats' read replayed at its place,
                                                              // its blocks never freed kept (MARK's)
  std::vector<ModelHeap::Event> readEvents;  // the boot's read's carves and frees (the stats' run)
  // What the hold left: at the boot's join (`boot`) and at the remount's Dc (`again`).
  struct Seen {
    std::vector<size_t> kept;  // the offsets of the blocks main carved in the hold and keeps (queue, canvas)
    size_t largest = 0;        // rawLargest() after the hold, before the join
    std::vector<std::pair<size_t, size_t>> free64;  // ... freeBlocks(64 KB)
    Layout layout;             // the join's blocks
    bool placed = false;
    uint32_t keptElsewhere = 0, runsElsewhere = 0;
    size_t topBytes = 0, topKept = 0;
    size_t joined = 0;         // largest() once joined
    int held = -1;             // the top blocks the hold still held after it (TopHold::held())
  } boot, again;
  int heldAtEnd = -1;        // ... at the run's end, the glue destroyed
  size_t topAbove = 0;       // the free block right above the last block the stats held at a top (0: none)
  // What it came to.
  size_t markBytes = 0;     // MARK's block (the boot's read's: kept for the session)
  size_t markHeld = 0;      // what the boot's read left held in this heap: MARK's block's footprint (0: none)
  size_t rangesBytes = 0;   // the ranges' block the boot's fold carved
  size_t chunkBytes = 0;    // the overflow's bytes held at the scan's end (Fold::held())
  bool arena = false;       // ... its chunks in the arena (fix A), not each a carve of its own
  uint32_t againChunks = 0; // (remount) the re-read's overflow chunks at its scan's end
  bool againArena = false;  // ... in the arena, carved mid-session (false: the chunks one by one, or none)
  uint32_t chunks = 0, tries = 0, saves = 0, wrote = 0, compacted = 0, reserveFailed = 0;
  bool behind = false;
  // S2 (C13; the owner's "A: nothing held", 2026-11-20): a console ask (Hr,
  // Hl, Hw) after the boot's join and after each update's rejoin and plays:
  // its one block at the top through the hold (the decoder's room asked; it
  // yields), stats.bin's and the base's FatFiles opened beside it (the
  // loop's), held through the rank's step and the walk, then everything
  // given back. Nothing of it between asks.
  // S3a (9.7's V5, the model's S3 half): the stats pages' asks, Visit (its
  // grades Full, then Lists) and NotPlayed (Lean, then No folders), as Hv
  // and Hn ask (app/StatsLists' turnVisit()), after the boot's join and after
  // each update's rejoin (the Live and the rejoin done; before a history's
  // FLAC ends, its Dc and the plays): stats.bin's directory read, a visit's
  // snapshot carved plain before the take, the grades tried in order through
  // the hold, each statsvisit::layout()'s block with the firmware's head.
  enum class Ask : uint8_t { None, Rank, Walk, Visit, NotPlayed };  // Hr; Hw (Hl3's blocks, the largest an
                                                                    // S2 ask carves); Hv; Hn
  Ask ask = Ask::None;
  bool askOpus = false;      // the session's first Opus opens while the ask holds its block (a start, the next track)
  size_t askBytes = 0;       // its block, as app/StatsLists sizes it for this card and library
  uint32_t asks = 0, askRefused = 0;
  bool askTraceless = true;  // after each give-back: free, the largest free block and the free blocks of 64 KB or
                             // more as before its take
  size_t askLowestLargest = SIZE_MAX;  // the largest free block while an ask held its block
  size_t askLowestFree = SIZE_MAX;     // ... free
  uint32_t askOpusShort = 0;           // (askOpus) the Opus's carves that found no room
  // A4 (9.7's "the held snapshot", call 1's condition): the visit's snapshot
  // held as a page holds it (Hu's), `bytes` of it (0: the library's,
  // statsvisit::bytesFor()), carved plain (psramAlloc: the model heap's
  // alloc) before each visit's take, held through the step (a history's
  // FLAC end, its Dc, the records' save, its Opus or the next step's FLAC
  // opening, the next step's records' save) and freed at the next fence
  // (StatsLists::drop(): after Auto DJ's join, before the old index), and at
  // the run's end. None: Hv's, freed at the landing (a console ask holds
  // nothing). Top (a placement for comparison, not the firmware's): carved
  // as the visit's grades are (statshold::take(): at the top of the largest
  // free block under the hold's spacers, the free block above it closed by
  // caps, a decoder's room asked; it yields), in the same hold, its caps
  // held with it, given back at the fence. `fallback` (9.7's "If 3a-4
  // fails"): freed besides before each long-lived carve made while it is
  // held, a decoder's open (the FLAC's, the Opus's) or a join's begin (the
  // Dc), and not carved again until the next visit (the page attached again
  // at 0 rows with Try again: nothing asks by itself).
  enum class Carve : uint8_t { None, Plain, Top };
  struct Page {
    size_t bytes = 0;
    Carve carve = Carve::None;
    bool fallback = false;
  } page;
  // What the page's snapshot came to: freed by the fallback before a FLAC's
  // open, an Opus's open, a join's begin (letGo[]); the model's points each
  // held snapshot lived through (each decoder's open, the FLAC's end, the
  // Dc, each records' save and the step's figure passed with it held; the
  // fence or the run's end frees it), and the visits whose snapshot was
  // still held at their step's figure (the boot's: its join's figure).
  enum { kLetGoFlac, kLetGoOpus, kLetGoJoin };
  uint32_t letGo[3] = {0, 0, 0};
  uint32_t heldPoints = 0, heldAtFigure = 0;
  std::map<std::string, uint32_t> topWhy;  // (Top) the snapshot's takes, by CloseReport::why (closeName())
  uint32_t topOutOfL = 0;                  // ... carved out of the largest free block
  // Each S3 ask's turn.
  struct Turn {
    uint32_t rows = 0;     // the u40 set's rows: HEAD's baseRows + TRKD's (stats.bin's directory) + the records'
    size_t bytes[2] = {};  // the grades' blocks in the order tried (Full, Lists; Lean, No folders), tried or not
    int took = -1;         // the grade taken (0, 1); -1: every grade refused
    size_t largest = 0;    // largest() at the ask, before its snapshot
    bool snapCarved = false;  // (a visit) its snapshot carved by this ask
    bool outOfL = false;      // ... out of the largest free block
  };
  std::vector<Turn> turns;
  uint32_t pageFreed = 0;  // (page) the snapshot freed at a fence or the run's end, held
};

// The firmware's sizeof(StatsLists::Ask) (src/app/StatsLists.cpp; the host
// build can't see src/app): 1,600 B, as the xtensa gdb reads it on both of
// S3a's V4 ELFs (core2 and core2-dio, at 6ea01b0: print
// sizeof('StatsLists::Ask')), and as Hv's take line prints it. The head of
// every grade's block (statsvisit::layout()), which moves with it byte for
// byte: 112 B over 9.7's estimate (1,488 B), each grade 112 B over its table.
constexpr size_t kS3AskHead = 1600;

// The grades an S3 ask tries, in order (StatsLists::turnVisit(); the model's
// clock is set, its card counts days from day 300 to the board's 600: Not
// played yet may open, so a visit asks Full first).
void s3Grades(StatsRun::Ask a, statsvisit::Grade out[2]) {
  const bool visit = a == StatsRun::Ask::Visit;
  out[0] = visit ? statsvisit::Grade::Full : statsvisit::Grade::Lean;
  out[1] = visit ? statsvisit::Grade::Lists : statsvisit::Grade::NoFolders;
}

// Each S3 ask's grade taken, in order ("Full, Lists, refused").
std::string tookOf(const StatsRun& r) {
  statsvisit::Grade g[2];
  s3Grades(r.ask, g);
  std::string s;
  for (const StatsRun::Turn& t : r.turns) {
    if (!s.empty()) s += ", ";
    s += t.took < 0 ? "refused" : statsvisit::gradeName(g[t.took]);
  }
  return s;
}

// An ask's block as app/StatsLists::turn() sizes it (its head, the firmware's
// struct Ask, about 4.4 KB on the device: the ranker, the map and the walk's
// 512 B path buffer among it; taken here as 4,608 B), for a card of
// `played` hashes (base and TRKD) with 40 records, over a library of
// `folders` folders and `albums` albums.
size_t s2AskBytes(StatsRun::Ask a, uint32_t played, uint32_t folders, uint32_t albums) {
  auto up = [](size_t n) { return (n + 15) & ~static_cast<size_t>(15); };
  const uint32_t records = statsfold::kRecords;
  size_t n = up(4608) + up(statsfile::kWorkBytes) + up(statsrank::Ranker::kBytes) +
             up(statsrank::Windows::bytesFor(statsfold::kRcntRows + records)) +
             up(static_cast<size_t>(records) * (statsfold::kTrackBytes + 12));
  if (a == StatsRun::Ask::Walk) {
    n += static_cast<size_t>(statsrank::playedChunks(played + records)) * statsrank::kPlayedChunkBytes;
    n += up(pathhash::Walk::bytesFor(folders)) + up(statsmap::albumBytes(albums));
    n += static_cast<size_t>(statsmap::wantedChunks(statsrank::Ranker::kEntries)) * statsmap::kChunkBytes;
  }
  return n;
}

// The decoders' requests while a track plays (docs/LISTENING-STATS.md 5.14,
// the yardstick), each held from the track's open to its end.
#ifdef STATS_OLD_DECODERS
// The shape the rework was first drawn on ("old"): the Opus's carved sizes
// asked again (the model rounds them once more), a FLAC as one 64 KB block
// and 16 KB.
const std::vector<size_t> kOpusAsks = {65536, 62464};
const std::vector<size_t> kFlacAsks = {65536, 16384};
#else
// The device's ("dev", the measure): the Opus's three
// (src/audio/OpusGenerator.cpp:50-52; the logs' "[opus] buffers: 128023 B"),
// in that order; libFLAC's (ESP8266Audio's, FLAC__ALIGN_MALLOC_DATA, the 64
// Kbit bit reader of a non-ESP8266 build) for a stereo 4,096-sample FLAC: its
// private struct (about 6 KB), the 8,192 B bit reader (bitreader.c:106, 274),
// then each channel's output and residual (stream_decoder.c:1316-1325).
const std::vector<size_t> kOpusAsks = {65307, 61440, 1276};
const std::vector<size_t> kFlacAsks = {6000, 8192, 16400, 16415, 16400, 16415};
#endif

// An update history (5.14's yardstick): the Opus kept from one point on, a
// FLAC playing through one update, a Dc (Auto DJ's join again) after each
// update or not; no memory check's give-back (run L had none). Each update's
// joined: the largest free block once its rejoin, the FLAC's end, the Dc, a
// save and the Opus's open (when it opens there) are done.
struct Updates {
  static constexpr int kNever = -2;
  int opusAt = kNever;  // -1: opened after the boot's join (and the boot's fold); k: after update k; kNever
  int flacStep = -1;    // carved before update k's fence, given back after its rejoin; -1: no FLAC
  bool dc = false;
  size_t queueExtra = 0;  // the queue's block this much longer: main's own variation (the control)
  bool opusFirst = false;  // (opusAt -1, no stats) the Opus opened before the boot's join, not after it
  std::vector<size_t> joined;
  uint32_t carveFails = 0, joinFails = 0;  // a decoder's block, or a join, that found no room
};

// The root's ACKS on a card a 0.14.0 player synced before this player's
// first stats boot: `rows` rows above the card's MARK, each 112 B in the
// ranges' block (at most Acks::kMax, 16: the block 80 x 16 + 112 x 16 =
// 3,072 B, 3,076 B as carved). The first is player 0's, its seq inside the
// log the boot folds (from `seq0`: no gap); the rest are players the log
// doesn't hold (a gap of 5 each, a MARK row each at the fold's end: with
// the card's one row, 16 in all, the object's room).
cc::msmf::Acks statsAcks(uint32_t rows, uint32_t seq0) {
  cc::msmf::Acks a;
  a.present = rows > 0;
  a.rows = a.count = rows;
  for (uint32_t i = 0; i < rows; ++i) {
    cc::msmf::Ack& e = a.entries[i];
    if (i == 0) {
      std::memcpy(e.deviceId, kStatsDev, 6);
      e.epoch = kStatsEpoch;
      e.seq = seq0 + 9;
      a.own = 1;
      continue;
    }
    const uint8_t dev[6] = {0x02, 0x00, 0x00, 0x00, 0x00, static_cast<uint8_t>(i)};
    std::memcpy(e.deviceId, dev, 6);
    e.epoch = 0x0BADC0FFEE000000ull + i;
    e.seq = 5;
  }
  return a;
}

// app/ListeningStats.cpp's topAlloc() and topRelease() over the model heap
// (autodj::TopHold, its hooks the model's: modelAbove() walks it): a block
// held across passes at the top of the largest free block, the join's
// spacers under it while it is carved (kMargin kept free with it carved),
// the free block they left above it closed by caps; given back bottom-up.
// The overflow's chunks one by one (no room for its arena) as fe61db9
// carved them: the hold's caps given back first, kMargin alone kept,
// nothing closed. A decoder's room (the recheck; its trigger since the
// final review): the hold's caps given back when the largest free block
// wouldn't take the Opus's buffers, and the arena too (its chunks then one
// by one). The rule is statsglue::decoderRoom(), the one the firmware's
// topAlloc() calls (U16): the ranges' block asks it at every take (no
// start is held while it alone is); the arena and a chunk only while a
// track plays (g_topPlayer's `playing`: the firmware's s_trackPlaying),
// since every other start waits while they are held.
// g_topAbove: the free block the last take found above its block (T, as
// fe61db9's left it open); g_topClose: what that take did.
size_t g_topAbove = 0;
autodj::Close g_topClose = autodj::Close::Nothing;
void* fe61db9Top(size_t n);
// fe61db9's hooks for the glue (it had no arena: the chunks one by one).
void* fe61db9NoArena(size_t n) { return n == statsfold::kArenaBytes ? nullptr : fe61db9Top(n); }
void* topAlloc(size_t n) {
  const bool chunk = n == statsfold::kChunkBytes;
  if (chunk) g_modelTop.uncap(kModelTop);
  autodj::CloseReport r;
  const bool playing = g_topPlayer && g_topPlayer().playing;
  const size_t room = statsglue::decoderRoom(n, playing);
  void* p = g_modelTop.take(kModelTop, n, &r, !chunk, room, n == statsfold::kArenaBytes);
  g_topAbove = r.aboveBytes;
  g_topClose = r.why;
  return p;
}
void topRelease(void* p) { g_modelTop.give(kModelTop, p); }

// Every free block under 64 KB taken by a kept block (into `kept`), each with
// requests of its own size class's floor (the cap's rule), so it is taken
// whole: what is left is under 16 B.
void takeHolesInto(std::vector<void*>* kept) {
  for (int guard = 0; guard < 10000; ++guard) {
    size_t f = 0;
    for (const auto& b : ModelHeap::blocks)
      if (!b.second.used && b.second.size >= 16 && b.second.size < 64 * 1024) f = b.second.size;
    if (!f) return;
    const size_t req = f < ModelHeap::kSmall
                           ? f - ModelHeap::kPoison
                           : (f & ~((static_cast<size_t>(1) << (ModelHeap::fls(f) - ModelHeap::kSlLog2)) - 1)) -
                                 ModelHeap::kPoison;
    void* p = ModelHeap::alloc(req);
    TEST_ASSERT_NOT_NULL(p);
    kept->push_back(p);
  }
  TEST_FAIL_MESSAGE("the holes never all taken");
}

// A record of carves and frees (ModelHeap::rec's) replayed: each block kept
// that the record never frees.
void replayCarves(const std::vector<ModelHeap::Event>& ev, std::vector<void*>* kept) {
  std::map<size_t, void*> live;
  for (const ModelHeap::Event& e : ev) {
    if (e.n) {
      void* p = ModelHeap::alloc(e.n);
      TEST_ASSERT_NOT_NULL(p);
      live[e.at] = p;
      continue;
    }
    auto it = live.find(e.at);
    TEST_ASSERT_TRUE_MESSAGE(it != live.end(), "a release the record never carved");
    ModelHeap::release(it->second);
    live.erase(it);
  }
  for (const auto& l : live) kept->push_back(l.second);
}

UpdateRun libraryUpdates(Dj dj, const std::vector<uint8_t>& saved, cc::Source& records, const synthcard::Card& card,
                         int steps, const MoodRun& mood = MoodRun(), StatsRun* stats = nullptr,
                         Updates* hist = nullptr) {
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
  TEST_ASSERT_EQUAL(mood.moods, index.hasMoods());
  std::vector<void*> queue, thumbs;
  const size_t queueBytes = 5120 + (hist ? hist->queueExtra : 0);
  take({queueBytes}, &queue);
  g_durations = ModelHeap::alloc(index.trackCount() * 2);
  take({3680, 3584, 15432, 4148}, nullptr);  // Auto DJ's glue, its engine, the open file
  // PlayHistory::begin(): the stats' read (the work memory and the files
  // given back in the call). The glue's object is internal RAM's; MARK's
  // block (a card with more than 8 MARK rows) stays in this heap, at the
  // top of its largest free block, and nothing else of the stats' does.
  const cc::msmf::Acks acks = stats ? statsAcks(stats->ackRows, stats->card->seq[0]) : cc::msmf::Acks{};  // the root's
  std::unique_ptr<cutfs::CutFs> statsCard;
  std::unique_ptr<PsramFs> statsFs;
  StatsEnv statsEnv;
  std::unique_ptr<statsglue::Glue> glue;
  uint32_t seq = 0, track = 0;
  // The hold's cases: main's blocks carved in the hold (or around it) and
  // kept for the run.
  std::vector<void*> holdKept;
  if (stats && stats->statsOff) {
    // Main's twin: no glue; the stats' read's carves replayed (a control
    // with MARK's block where the read leaves it), or nothing.
    if (stats->replayRead) replayCarves(*stats->replayRead, &holdKept);
    seq = stats->card->seq[0];
  } else if (stats) {
    statsCard.reset(new cutfs::CutFs(stats->card->card.reboot(cutfs::Variant::InOrder)));
    statsFs.reset(new PsramFs(*statsCard));
    statsEnv.card = statsCard.get();
    statsglue::Memory m;
    m.alloc = ModelHeap::alloc;
    m.top = stats->fe61db9 ? fe61db9NoArena : topAlloc;
    m.topRelease = stats->fe61db9 ? ModelHeap::release : topRelease;
    m.release = ModelHeap::release;
    m.keep = stats->fe61db9 ? fe61db9Top : ModelHeap::alloc;  // (fix G: MARK's block where any session block goes)
    m.keepRelease = ModelHeap::release;
    glue.reset(new statsglue::Glue(*statsFs, statsEnv, m));
    const size_t free0 = ModelHeap::freeBytes();
    stats->readEvents.clear();
    ModelHeap::rec = &stats->readEvents;
    const statsfile::Opened& o = glue->begin(acks, [](uint32_t) {});
    ModelHeap::rec = nullptr;
    TEST_ASSERT_TRUE(o.read == statsfile::Read::Loaded);
    TEST_ASSERT_TRUE(glue->on());
    TEST_ASSERT_EQUAL_size_t(0, statsFs->openNow());
    stats->markBytes = glue->store().markBytes();
    stats->markHeld = free0 - ModelHeap::freeBytes();
    seq = stats->card->seq[0];
  }
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
  // A trace's place (ModelHeap::trace: the yardstick's A4 trace).
  HeapTrace* const trace = ModelHeap::trace;
  auto tracePoint = [&](int p) {
    if (trace) trace->point = p;
  };
  // A4 (StatsRun::page): the stats pages' held snapshot (S3a's s3Ask(),
  // below). Freed at a fence (StatsLists::drop(): after Auto DJ's join,
  // before the old index), at the run's end, and with page.fallback before
  // each decoder's open and each join's begin made while it is held (9.7's
  // fallback: statsRelease() from the player's open and the join's begin).
  void* page = nullptr;
  auto freePage = [&] {
    if (!page) return;
    if (trace) trace->snap = true;
    if (stats->page.carve == StatsRun::Carve::Top) {
      g_modelTop.give(kModelTop, page);
    } else {
      ModelHeap::release(page);
    }
    if (trace) trace->snap = false;
    page = nullptr;
    ++stats->pageFreed;
  };
  auto letGo = [&](int kind) {
    if (!page || !stats->page.fallback) return;
    freePage();
    ++stats->letGo[kind];
  };
  // A point of the step passed (StatsRun::heldPoints): with the snapshot
  // held, one more.
  auto heldPoint = [&] {
    if (page) ++stats->heldPoints;
  };
  auto joinNow = [&](size_t* lowest) {
    if (dj == Dj::Off) return;
    letGo(StatsRun::kLetGoJoin);  // (9.7's fallback: before the join's begin)
    ModelHeap::mark();
    if (hist) {
      // (a history counts a join that can't place, and goes on)
      if (join.begin(table, &index) != Status::Working) {
        ++hist->joinFails;
        return;
      }
    } else {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Working), static_cast<int>(join.begin(table, &index)));
    }
    *lowest = std::min(*lowest, ModelHeap::low);
    u.topKept = join.stats().topKept;
    runJoin(join, &index);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(Status::Ready), static_cast<int>(join.status()));
  };
  // The decoders' blocks (a history's).
  std::vector<void*> opusBlocks, flacBlocks;
  auto carveInto = [&](const std::vector<size_t>& asks, std::vector<void*>* into) {
    const bool opus = &asks == &kOpusAsks;
    letGo(opus ? StatsRun::kLetGoOpus : StatsRun::kLetGoFlac);  // (9.7's fallback: before the player's open)
    const int was = trace ? trace->point : 0;
    tracePoint(opus ? kTpOpusOpen : kTpFlacOpen);
    for (size_t n : asks) {
      void* p = ModelHeap::alloc(n);
      if (!p) {
        TEST_ASSERT_NOT_NULL_MESSAGE(hist, "a decoder's block with no room");
        ++hist->carveFails;
        continue;
      }
      into->push_back(p);
    }
    tracePoint(was);
    heldPoint();
  };
  const bool opusAtBoot = hist && hist->opusAt == -1;
  bool opusOpen = false, opusWaits = false;
  // A start the listener asks at the boot (opusAt -1): held while the boot's
  // fold holds overflow chunks (main.cpp's OutputHold: overflowHeld()).
  auto bootStart = [&] {
    if (!opusAtBoot || opusOpen) return;
    opusWaits = glue && stats->startWaits && glue->overflowHeld();
    if (opusWaits) return;
    carveInto(kOpusAsks, &opusBlocks);
    opusOpen = true;
  };
  // The stats' part of the boot's scan (a reserve() before each piece of
  // 128, the firmware's look-ahead, then its slots: the ranges' block and the
  // overflow's arena at the top), the log above the card's MARK in the
  // scan's order (plays.jnl, the newest, then plays.1.jnl); a start asked
  // after piece `startAt` (no start waiting: the Opus opening there, the
  // later chunks carved around it). Then finishScan(): the boot's save, its
  // overflow's tries at later passes, the board's waits apart (the join, an
  // asked update and a start waiting meanwhile: nothing else carves), and
  // the journal ready. From then on plays are written.
  bool statsReady = false;
  // The hold's cases: main's own carves (StatsRun::uiAt, queueAt), kept for
  // the run, their offsets into `keptInto` while the hold is recorded.
  std::vector<size_t>* keptInto = nullptr;
  bool mp3 = false;       // an MP3 playing (it holds no PSRAM: its next track's decoder may open all the same)
  bool mp3Waits = false;  // ... its start waiting (overflow chunks held), played once the boot's save gave them back
  auto keep = [&](size_t n) {
    void* p = ModelHeap::alloc(n);
    TEST_ASSERT_NOT_NULL(p);
    holdKept.push_back(p);
    if (keptInto) keptInto->push_back(ModelHeap::offsetOf(p));
  };
  auto carveUi = [&] {
    keep(352);    // NowPlayingPage's M5Canvas
    keep(19208);  // its 98 x 98 sprite
    ModelHeap::release(ModelHeap::alloc(4096));  // PanelScan's
  };
  auto carveQueue = [&] {
    keep(1100);  // QueueModel's entries
    keep(1180);  // ... and its undo
    // An MP3's start: its 4 KB probe (none while overflow chunks hold it:
    // the start waits).
    mp3Waits = glue && stats->startWaits && glue->overflowHeld();
    if (mp3Waits) return;
    mp3 = true;
    ModelHeap::release(ModelHeap::alloc(4096));
  };
  const uint32_t bootSeq = seq;  // (a remount scans the same log again)
  auto statsScan = [&](bool again) {
    namespace sf = statsfold;
    const uint32_t n = stats->slots;
    const uint32_t newer = std::min(n, playlog::kFileSlots);  // plays.jnl's; the rest plays.1.jnl's
    const uint32_t seq0 = again ? bootSeq : seq;
    g_topAbove = 0;
    if (glue) glue->scanBegan(again);
    uint32_t pieces = 0, done = 0;
    void* logFile = nullptr;  // (scanFile) the play log's FatFile
    int fileOf = -1;
    auto afterPiece = [&](uint32_t k) {
      if (again) {
        if (stats->nextOpus && k == stats->nextAt) {
          // The next track the session's first Opus (the MP3 gives nothing
          // back): its start's probe, then its buffers, all or none.
          ModelHeap::release(ModelHeap::alloc(4096));
          std::vector<void*> got;
          for (size_t ask : kOpusAsks)
            if (void* p = ModelHeap::alloc(ask)) got.push_back(p);
          if (got.size() == kOpusAsks.size()) {
            opusBlocks = got;
          } else {
            ++stats->nextShort;
            for (void* p : got) ModelHeap::release(p);
          }
          return;
        }
        // The next track's FLAC opens (the old one's blocks freed first).
        if (k == 0 && !flacBlocks.empty()) {
          for (void* p : flacBlocks) ModelHeap::release(p);
          flacBlocks.clear();
          carveInto(kFlacAsks, &flacBlocks);
        }
        return;
      }
      if (stats->queueAt == k) carveQueue();
      if (stats->uiAt == k) carveUi();
    };
    auto fold = [&](uint32_t i, int f) {
      if (stats->startAt && pieces == stats->startAt * 128) bootStart();
      if (pieces++ % 128 == 0 && glue && !glue->reserve(statsglue::kReserveAhead)) ++stats->reserveFailed;
      if (stats->scanFile && fileOf != f) {
        if (logFile) ModelHeap::release(logFile);
        logFile = ModelHeap::alloc(kFatFileBytes);
        fileOf = f;
      }
      const uint64_t at = sf::kMinMs + (570 + static_cast<uint64_t>(i) * 29 / n) * sf::kDayMs + (i % 24) * sf::kHourMs;
      if (glue) glue->slot(statsSlot(seq0 + i, i % stats->card->played, at), f);
      if (++done == n && logFile) {
        ModelHeap::release(logFile);  // (the last file read whole: closed in its piece)
        logFile = nullptr;
      }
      if (pieces % 128 == 0) afterPiece(pieces / 128 - 1);
    };
    for (uint32_t i = n - newer; i < n; ++i) fold(i, 0);
    for (uint32_t i = 0; i < n - newer; ++i) fold(i, 1);
    if (pieces % 128) afterPiece(pieces / 128);
    if (!again) seq += n;
    if (!glue) return;
    stats->topAbove = g_topAbove;
    if (again) {
      stats->againChunks = glue->fold().chunks();
      stats->againArena = glue->fold().inArena();
      return;
    }
    stats->rangesBytes = glue->rangesBytes();
    stats->chunks = glue->fold().chunks();
    stats->chunkBytes = glue->fold().held();
    stats->arena = glue->fold().inArena();
  };
  auto playing = [&] { return !opusBlocks.empty() || !flacBlocks.empty(); };
  // S2's ask (StatsRun::ask): the firmware's order (app/StatsLists): its
  // block at the top, the two files, the worker's step and the walk (no
  // carve: their memory is the block's), the files closed, the block given
  // back. Main's twin (statsOff) carves nothing; the rest of the run is the
  // same. With askOpus the session's first Opus opens inside the hold, kept.
  auto statsAsk = [&] {
    if (!stats || stats->ask == StatsRun::Ask::None || stats->statsOff) return;
    if (stats->ask == StatsRun::Ask::Visit || stats->ask == StatsRun::Ask::NotPlayed) return;  // (S3's: s3Ask())
    stats->askBytes = s2AskBytes(stats->ask, stats->card->played, index.folderCount(), index.albumCount());
    const size_t free0 = ModelHeap::freeBytes(), largest0 = ModelHeap::rawLargest();
    const std::vector<std::pair<size_t, size_t>> free64 = ModelHeap::freeBlocks(64 * 1024);
    ++stats->asks;
    autodj::CloseReport r;
    void* p = g_modelTop.take(kModelTop, stats->askBytes, &r, true, statsglue::kDecoderRoomBytes, true);
    if (!p) {
      ++stats->askRefused;
      if (ModelHeap::freeBytes() != free0 || ModelHeap::rawLargest() != largest0) stats->askTraceless = false;
      return;
    }
    void* bin = ModelHeap::alloc(kFatFileBytes);
    void* base = ModelHeap::alloc(kFatFileBytes);
    TEST_ASSERT_NOT_NULL(bin);
    TEST_ASSERT_NOT_NULL(base);
    stats->askLowestLargest = std::min(stats->askLowestLargest, ModelHeap::largest());
    stats->askLowestFree = std::min(stats->askLowestFree, ModelHeap::freeBytes());
    bool opened = false;
    if (stats->askOpus && opusBlocks.empty()) {
      std::vector<void*> got;
      for (size_t a : kOpusAsks)
        if (void* q = ModelHeap::alloc(a)) got.push_back(q);
      if (got.size() == kOpusAsks.size()) {
        opusBlocks = got;
        opened = true;
      } else {
        ++stats->askOpusShort;
        for (void* q : got) ModelHeap::release(q);
      }
    }
    ModelHeap::release(base);
    ModelHeap::release(bin);
    g_modelTop.give(kModelTop, p);
    TEST_ASSERT_EQUAL_INT(0, g_modelTop.held());
    // (An Opus opened in the hold keeps its blocks: no trace to ask of it.)
    if (!opened && (ModelHeap::freeBytes() != free0 || ModelHeap::rawLargest() != largest0 ||
                    ModelHeap::freeBlocks(64 * 1024) != free64))
      stats->askTraceless = false;
  };
  // S3a's asks (StatsRun::Ask Visit, NotPlayed; docs/LISTENING-STATS.md
  // 9.7): app/StatsLists' turnVisit() over the model heap. stats.bin's
  // directory read for a grade with a played set (Glue::lookFile(): its
  // FatFile and a work memory carved and given back), the u40 rows from it;
  // a visit's snapshot carved plain if none is held (A4's page, held: its
  // own); then the grades in order through the hold (the decoder's room
  // asked; it yields), each statsvisit::layout()'s block with the
  // firmware's head (kS3AskHead), over this index's folders and albums,
  // dated (the work memory the host's: 32 B over the device's where it sets
  // a grade's room, Lists and No folders; 5.14's R4). Every grade refused:
  // the snapshot freed if this ask carved it. Taken: the rank's two
  // FatFiles beside it (the hand-off: stats.bin and the base), the rank and
  // the walk (no carve: their memory is the block's), the files closed, the
  // block given back; then Hv's snapshot freed (a console ask holds
  // nothing), a page's kept (Hu's). Main's twin (statsOff) carves nothing.
  // Each ask's traces: the take's (free, the largest free block, the free
  // blocks of 64 KB or more as after the snapshot's carve) and, with no
  // page kept, the whole ask's (as before it). A4's page (StatsRun::page,
  // `page` above): carved plain, or (Top) through the hold as the grades
  // are, refused there when it finds no room (the visit's "No room": no
  // grade asked).
  auto s3Ask = [&] {
    if (!stats || stats->statsOff) return;
    const bool visit = stats->ask == StatsRun::Ask::Visit;
    if (!visit && stats->ask != StatsRun::Ask::NotPlayed) return;
    namespace sv = statsvisit;
    const size_t free0 = ModelHeap::freeBytes(), largest0 = ModelHeap::rawLargest();
    const std::vector<std::pair<size_t, size_t>> free64 = ModelHeap::freeBlocks(64 * 1024);
    ++stats->asks;
    StatsRun::Turn t;
    t.largest = ModelHeap::largest();
    const statsglue::Glue::FileLook l = glue->lookFile();
    TEST_ASSERT_TRUE(l.there && l.frame);
    t.rows = glue->fold().head().baseRows + l.rows[statsfold::kTrkd] + statsfold::kRecords;
    const uint32_t albums = index.albumCount();
    const size_t foldersBytes = pathhash::Walk::bytesFor(pathhash::Walk::foldersFor(index));
    void* snap = page;
    const bool top = stats->page.carve == StatsRun::Carve::Top;
    tracePoint(kTpVisit);
    if (visit && !snap) {
      // Out of the largest free block, or elsewhere (TLSF's good fit: the
      // smallest free class that holds it).
      size_t lAt = 0, lBytes = 0;
      for (const auto& b : ModelHeap::blocks)
        if (!b.second.used && b.second.size > lBytes) {
          lAt = b.first;
          lBytes = b.second.size;
        }
      const size_t snapBytes = stats->page.bytes ? stats->page.bytes : sv::bytesFor(albums);
      if (trace) trace->snap = true;
      if (top) {
        autodj::CloseReport r;
        snap = g_modelTop.take(kModelTop, snapBytes, &r, true, statsglue::kDecoderRoomBytes, true);
        ++stats->topWhy[autodj::closeName(r.why)];
      } else {
        snap = ModelHeap::alloc(snapBytes);
      }
      if (trace) trace->snap = false;
      if (!snap) {
        // (Top) no room for it with a decoder's: the visit's "No room", no
        // grade asked.
        TEST_ASSERT_TRUE(top);
        ++stats->askRefused;
        stats->turns.push_back(t);
        return;
      }
      t.snapCarved = true;
      t.outOfL = ModelHeap::offsetOf(snap) >= lAt && ModelHeap::offsetOf(snap) < lAt + lBytes + ModelHeap::kHeader;
      if (top && t.outOfL) ++stats->topOutOfL;
    }
    const size_t freeT = ModelHeap::freeBytes(), largestT = ModelHeap::rawLargest();
    const std::vector<std::pair<size_t, size_t>> free64T = ModelHeap::freeBlocks(64 * 1024);
    sv::Grade grades[2];
    s3Grades(stats->ask, grades);
    for (int i = 0; i < 2; ++i)
      t.bytes[i] = sv::layout(grades[i], kS3AskHead, t.rows, foldersBytes, albums, true).bytes;
    void* p = nullptr;
    for (int i = 0; i < 2 && !p; ++i) {
      autodj::CloseReport r;
      p = g_modelTop.take(kModelTop, t.bytes[i], &r, true, statsglue::kDecoderRoomBytes, true);
      if (p) {
        t.took = i;
      } else if (ModelHeap::freeBytes() != freeT || ModelHeap::rawLargest() != largestT) {
        stats->askTraceless = false;  // (a refused take leaves no trace)
      }
    }
    bool opened = false;
    if (p) {
      void* bin = ModelHeap::alloc(kFatFileBytes);
      void* base = ModelHeap::alloc(kFatFileBytes);
      TEST_ASSERT_NOT_NULL(bin);
      TEST_ASSERT_NOT_NULL(base);
      stats->askLowestLargest = std::min(stats->askLowestLargest, ModelHeap::largest());
      stats->askLowestFree = std::min(stats->askLowestFree, ModelHeap::freeBytes());
      if (stats->askOpus && opusBlocks.empty()) {
        std::vector<void*> got;
        for (size_t a : kOpusAsks)
          if (void* q = ModelHeap::alloc(a)) got.push_back(q);
        if (got.size() == kOpusAsks.size()) {
          opusBlocks = got;
          opened = true;
        } else {
          ++stats->askOpusShort;
          for (void* q : got) ModelHeap::release(q);
        }
      }
      ModelHeap::release(base);
      ModelHeap::release(bin);
      g_modelTop.give(kModelTop, p);
      TEST_ASSERT_EQUAL_INT(top && snap ? 1 : 0, g_modelTop.held());  // ((Top) the snapshot's, still held)
      if (!opened && (ModelHeap::freeBytes() != freeT || ModelHeap::rawLargest() != largestT ||
                      ModelHeap::freeBlocks(64 * 1024) != free64T))
        stats->askTraceless = false;
    } else {
      ++stats->askRefused;
    }
    // The snapshot: a page's kept (one held before this ask, whatever it
    // came to; one it carved, if it took a grade); else freed (every grade
    // refused, this ask's carve; or Hv's).
    const bool held = snap && stats->page.carve != StatsRun::Carve::None && (p || !t.snapCarved);
    if (snap && !held) {
      if (trace) trace->snap = true;
      if (top) {
        g_modelTop.give(kModelTop, snap);
      } else {
        ModelHeap::release(snap);
      }
      if (trace) trace->snap = false;
    }
    page = held ? snap : nullptr;
    if (!opened && !held &&
        (ModelHeap::freeBytes() != free0 || ModelHeap::rawLargest() != largest0 ||
         ModelHeap::freeBlocks(64 * 1024) != free64))
      stats->askTraceless = false;
    stats->turns.push_back(t);
  };
  // The player at a take (the firmware's ListeningStats::pass()): a track
  // playing (an MP3, an Opus or a FLAC), a start waiting.
  g_topPlayer = [&] {
    TopPlayer s;
    s.playing = mp3 || playing();
    s.waiting = mp3Waits || opusWaits;
    return s;
  };
  auto statsBootSave = [&](bool boot) {
    const bool ui = boot && (stats->uiAt == StatsRun::kInSave || stats->uiAt == StatsRun::kBetweenTries);
    if (!glue) {
      // Main's twin: no save; the same carve at its place.
      if (ui) carveUi();
      statsReady = true;
      return;
    }
    statsEnv.playing = playing();
    statsEnv.fails = boot ? stats->bootFails : 0;
    if (ui && stats->uiAt == StatsRun::kInSave) statsEnv.onAsking = carveUi;
    glue->save(playlog::Folds::Why::Boot);
    statsEnv.onAsking = nullptr;
    bool tried = false;
    while (glue->retrying()) {
      if (ui && !tried && stats->uiAt == StatsRun::kBetweenTries) carveUi();
      tried = true;
      statsEnv.mono = glue->retryAtMs();
      TEST_ASSERT_TRUE(glue->saveDue(false));
      glue->saveNow(false);
    }
    statsEnv.fails = 0;
    glue->ready();
    TEST_ASSERT_FALSE(glue->scanHolds());
    TEST_ASSERT_EQUAL_INT(0, g_modelTop.held());  // (the hold's blocks and caps all given back)
    TEST_ASSERT_EQUAL_size_t(0, statsFs->openNow());
    statsReady = true;
    // (An MP3's start the chunks held plays from the pass after: main.cpp's
    // stepPlayGate.)
    if (mp3Waits) {
      mp3Waits = false;
      mp3 = true;
    }
    if (!boot) return;
    stats->tries = glue->tries();
    stats->behind = glue->fold().behind();
  };
  // What the hold left (StatsRun::Seen): after it, and once joined.
  auto seenHold = [&](StatsRun::Seen* s) {
    s->largest = ModelHeap::rawLargest();
    s->free64 = ModelHeap::freeBlocks(64 * 1024);
    s->held = g_modelTop.held();
  };
  auto seenJoin = [&](StatsRun::Seen* s) {
    s->joined = ModelHeap::largest();
    if (dj == Dj::Off) return;
    s->layout = layoutOf(join);
    const Join::Stats& js = join.stats();
    s->placed = js.placed;
    s->keptElsewhere = js.keptElsewhere;
    s->runsElsewhere = js.runsElsewhere;
    s->topBytes = js.topBytes;
    s->topKept = js.topKept;
  };
  // `n` plays written (a pass the play log's write gate lets through each),
  // a save due first at each such pass (statsglue::turnOf()): the records'
  // save at 24 records, or at 40 behind an update's fence (`fenced`).
  auto statsPlays = [&](uint32_t n, bool fenced) {
    namespace sf = statsfold;
    if (!glue) return;
    auto due = [&] {
      if (!glue->saveDue(fenced)) return;
      statsEnv.playing = playing();
      glue->saveNow(fenced);
      TEST_ASSERT_EQUAL_size_t(0, statsFs->openNow());
    };
    for (uint32_t i = 0; i < n; ++i) {
      due();
      glue->written(statsSlot(seq++, track++ % stats->card->played, sf::kMinMs + 599ull * sf::kDayMs + i * 60000));
    }
    due();
  };
  using Scan = StatsRun::Scan;
  const Scan when = stats ? stats->when : Scan::JoinFirst;
  if (stats && when == Scan::ScanFirst) {
    keptInto = &stats->boot.kept;
    statsScan(false);
    bootStart();  // (a start asked during the scan: under its blocks, unless overflow chunks hold it)
    statsBootSave(true);
    keptInto = nullptr;
    // A start the chunks held: released at the pass after the save gave them
    // back (main.cpp's stepPlayGate), before that pass's stepAutoDj begins
    // the boot's join. So the Opus opens before the join, as main's with the
    // Opus first.
    bootStart();
  }
  if (hist && hist->opusFirst) bootStart();
  u.opusFirst = opusOpen;
  if (stats && stats->uiAt == StatsRun::kBeforeJoin) carveUi();  // (main's order: the first frame, then the join)
  if (stats) seenHold(&stats->boot);
  tracePoint(kTpBootJoin);
  joinNow(&u.bootLowest);
  if (stats && statsReady) statsAsk();  // (S2: an ask once the boot's join is Ready)
  if (stats && statsReady) s3Ask();     // (S3a: the same)
  u.bootJoined = ModelHeap::largest();
  heldPoint();  // (the boot's figure)
  if (page) ++stats->heldAtFigure;
  tracePoint(kTpBoot);
  u.bootTopKept = u.topKept;
  if (stats) seenJoin(&stats->boot);
  if (stats && when == Scan::JoinFirst) {
    statsScan(false);
    bootStart();
    statsBootSave(true);
  }
  if (stats && when == Scan::ThroughBuild) {
    statsScan(false);
    bootStart();
  }
  bootStart();  // (after the boot's join: main's; or after the overflow's arena went)
  if (stats && stats->remount) {
    // A FLAC playing (opened after the boot's join), then a remount mid-
    // session: the stats read stats.bin again and fold the log again (all
    // under the card's MARK) holding the ranges' block, the next track's
    // FLAC opening in that hold; then the Dc. (nextOpus: an MP3 playing,
    // the next track an Opus.)
    if (stats->less) keep(stats->less);
    if (stats->nextOpus) {
      ModelHeap::release(ModelHeap::alloc(4096));  // (the MP3's start: its probe)
      mp3 = true;
    } else {
      carveInto(kFlacAsks, &flacBlocks);
    }
    if (stats->takeHoles) takeHolesInto(&holdKept);
    keptInto = &stats->again.kept;
    statsScan(true);
    statsBootSave(false);
    keptInto = nullptr;
    seenHold(&stats->again);
    join.release();
    joinNow(&u.lowest);
    seenJoin(&stats->again);
  }
  for (int step = 0; step < steps; ++step) {
    if (trace) trace->step = step;
    if (hist && hist->flacStep == step) carveInto(kFlacAsks, &flacBlocks);
    // The update step's pass that raises the fence: the records' save, due
    // there, before it (the save waits while the fence is up).
    tracePoint(kTpSave);
    if (stats && statsReady && stats->stepSaves) {
      statsPlays(statsfold::kSaveAt, false);
      heldPoint();
    }
    // enterFence(): the queue, Thumbs' pools, the join, then the old index.
    tracePoint(kTpFence);
    for (void* p : queue) ModelHeap::release(p);
    for (void* p : thumbs) ModelHeap::release(p);
    queue.clear();
    thumbs.clear();
    join.release();
    if (stats) freePage();  // (S3a: StatsLists::drop(), the stats pages' snapshot)
    index.clear();  // (the words go with it)
    if (stats && when == Scan::InFence && step == 0) {
      statsScan(false);
      bootStart();
    }
    // The plays written behind the fence: the records' save at 40, whatever
    // the fence (before the build, or after it).
    if (stats && stats->fenceSave == 1 && step == 0) statsPlays(statsfold::kRecords, true);
    u.fence = ModelHeap::freeBlocks(64 * 1024);
    // The build, from D's records (or T's, D's rows Software rows).
    tracePoint(kTpBuild);
    LibraryBuilder b(ModelHeap::alloc, ModelHeap::release);
    synthcard::Facts facts(card, false);
    synthcard::SameRows rows(mood.transfer ? LibraryBuilder::Status::Software : LibraryBuilder::Status::Scanned, false,
                             true);
    LibraryBuilder::Config c;
    c.transfer = mood.transfer;
    c.device = &records;
    c.rows = &rows;
    c.facts = &facts;
    const LibraryBuilder::Result r = b.build(index, c);
    TEST_ASSERT_TRUE(r.built);
    if (stats && (when == Scan::ThroughBuild || when == Scan::InFence) && step == 0) {
      statsBootSave(true);
      bootStart();
    }
    if (stats && stats->fenceSave == 2 && step == 0) statsPlays(statsfold::kRecords, true);
    if (mood.transfer) {
      TEST_ASSERT_TRUE(r.transferUsed);  // T's records, not a restart from D
      TEST_ASSERT_EQUAL_UINT32(card.files.size(), r.fromTransfer);
    }
    TEST_ASSERT_EQUAL(mood.moods, index.hasMoods());
    TEST_ASSERT_EQUAL(mood.moods, r.withMoods > 0);
    // leaveFence(): the queue read back, the durations, Thumbs' pools; then
    // the join (the next pass).
    tracePoint(kTpLive);
    take({queueBytes}, &queue);
    ModelHeap::release(g_durations);
    g_durations = ModelHeap::alloc(index.trackCount() * 2);
    take({204800, 256, 256, 110592, 24, 24}, &thumbs);
    u.largest = ModelHeap::largest();
    tracePoint(kTpRejoin);
    joinNow(&u.lowest);
    // S3a: the stats pages' ask once the Live and the rejoin are done (a
    // page's snapshot carved before its take, held to the next fence).
    if (stats && statsReady) s3Ask();
    // An update asked before the boot's scan's first piece: the scan once
    // it is over (the scan's pieces waiting for the rejoin too).
    if (stats && when == Scan::AfterUpdate && step == 0) {
      statsScan(false);
      bootStart();
      statsBootSave(true);
    }
    if (hist) {
      // The FLAC ends; the Dc.
      if (hist->flacStep == step) {
        tracePoint(kTpFlacEnd);
        for (void* p : flacBlocks) ModelHeap::release(p);
        flacBlocks.clear();
        heldPoint();
      }
      if (hist->dc && dj != Dj::Off) {
        tracePoint(kTpDc);
        join.release();
        joinNow(&u.lowest);
        heldPoint();
      }
    }
    // Plays, and the records' save they bring due.
    tracePoint(kTpPlays);
    if (stats && statsReady && stats->stepSaves) {
      statsPlays(statsfold::kSaveAt, false);
      heldPoint();
    }
    // S2: an ask once the rejoin is Ready and the update over.
    if (stats && statsReady) statsAsk();
    if (hist) {
      if (hist->opusAt == step) carveInto(kOpusAsks, &opusBlocks);
      hist->joined.push_back(ModelHeap::largest());
      heldPoint();  // (the step's figure)
      if (page) ++stats->heldAtFigure;
      if (trace) traceFigure(trace);
    }
    u.joined = ModelHeap::largest();
    if (hist) continue;  // (no memory check's give-back: run L had none)
    // The next update's memory check, short on its table's block: the join
    // given back (setGiveBack()).
    join.release();
    u.joinOnly = ModelHeap::largest();
    u.givenBack = ModelHeap::largest();
    joinNow(&u.lowest);
  }
  tracePoint(kTpEnd);
  join.release();
  index.clear();
  if (stats) freePage();  // (the orderly end: StatsLists::ending())
  for (void* p : opusBlocks) ModelHeap::release(p);
  for (void* p : flacBlocks) ModelHeap::release(p);
  if (stats) TEST_ASSERT_TRUE_MESSAGE(statsReady, "the boot's scan never ended");
  if (glue) {
    stats->saves = statsEnv.saves;
    stats->wrote = statsEnv.wrote;
    stats->compacted = statsEnv.compacted;
    glue.reset();  // (MARK's block given back with its store)
  }
  if (stats) stats->heldAtEnd = g_modelTop.held();
  g_topPlayer = nullptr;
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
  auto savedFrom = [&](cc::Source* transfer, cc::Source& device) {
    LibraryIndex idx;
    LibraryBuilder b;
    synthcard::Facts facts(card, false);
    synthcard::SameRows rows(transfer ? LibraryBuilder::Status::Software : LibraryBuilder::Status::Scanned, false,
                             true);
    LibraryBuilder::Config c;
    c.transfer = transfer;
    c.device = &device;
    c.rows = &rows;
    c.facts = &facts;
    const LibraryBuilder::Result r = b.build(idx, c);
    TEST_ASSERT_TRUE(r.built);
    TEST_ASSERT_EQUAL_UINT32(tracks, idx.trackCount());
    if (transfer) {
      TEST_ASSERT_TRUE(r.transferUsed);  // T's records, not a restart from D
      TEST_ASSERT_EQUAL_UINT32(tracks, r.fromTransfer);
    }
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    return std::vector<uint8_t>(sink.data(), sink.data() + sink.size());
  };
  const std::vector<uint8_t> saved = savedFrom(nullptr, records);
  const UpdateRun off = libraryUpdates(Dj::Off, saved, records, card, 2);
  const UpdateRun on = libraryUpdates(Dj::Placed, saved, records, card, 2);
  const UpdateRun heaps = libraryUpdates(Dj::WhereThe, saved, records, card, 2);
  printf("[join] a library update, 19,412 tracks, the model heap: the largest free block at the boot %u B; after the "
         "update %u B with Auto DJ off (main's own rebuild). Auto DJ on: joined at the boot %u B, free at least %u B "
         "while placing; after the update %u B, joined %u B (%u B of it at the top), given back %u B; free at least "
         "%u B while placing. Its blocks where the heap puts them: joined %u B, given back %u B\n",
         (unsigned)off.bootLargest, (unsigned)off.largest, (unsigned)on.bootJoined, (unsigned)on.bootLowest,
         (unsigned)on.largest, (unsigned)on.joined, (unsigned)on.topKept, (unsigned)on.givenBack, (unsigned)on.lowest,
         (unsigned)heaps.joined, (unsigned)heaps.givenBack);
  // The rebuild itself fragments PSRAM, Auto DJ or not.
  TEST_ASSERT_TRUE(off.largest + 100000 < off.bootLargest);
  // The fence's frees leave the heap as Auto DJ off has it, and the build
  // and the Live step do the same with it.
  TEST_ASSERT_TRUE(on.fence == off.fence);
  TEST_ASSERT_TRUE(heaps.fence == off.fence);
  TEST_ASSERT_EQUAL_size_t(off.largest, on.largest);
  // Joined again: only what no smaller free block holds at the top (here none
  // of it: the model's rebuild leaves a hole for the filters, the device's
  // didn't: test_the_rejoin_anywhere_in_a_class()); given back, off's block.
  TEST_ASSERT_TRUE(on.topKept <= static_cast<size_t>(tracks - 2) * 4);
  TEST_ASSERT_TRUE(on.joined + on.topKept + 24 * 1024 >= off.largest);
  TEST_ASSERT_EQUAL_size_t(off.largest, on.givenBack);
  TEST_ASSERT_TRUE(on.lowest >= kMargin);
  TEST_ASSERT_TRUE(on.bootLowest >= kMargin);
  // Its blocks where the heap puts them: 9.5's pass rule holds as well (the
  // margin kept; after the update, and given back, off's block).
  TEST_ASSERT_TRUE(heaps.lowest >= kMargin);
  TEST_ASSERT_TRUE(heaps.bootLowest >= kMargin);
  TEST_ASSERT_EQUAL_size_t(off.largest, heaps.largest);
  TEST_ASSERT_EQUAL_size_t(off.largest, heaps.givenBack);
  // At the boot (one free block): everything at the top, as before.
  TEST_ASSERT_EQUAL_size_t(155284, on.bootTopKept);
  TEST_ASSERT_TRUE(on.bootJoined + on.bootTopKept + 24 * 1024 >= on.bootLargest);

  // The same with the mood words (MOOD-FILTERS.md 9.5, 9.6 C6, against the
  // run above as the control): built from T, its records with their mood
  // tails (synthcard::kMoodPct of the tracks), D's rows Software rows; the
  // words' block the index's own, loaded at the boot (the file's last
  // block) and taken again by each build at its first track with a word.
  synthcard::Write tw;
  tw.moods = true;
  const std::vector<uint8_t> t = synthcard::tagsFile(card, tw);
  const std::vector<uint8_t> plain = synthcard::tagsFile(card, synthcard::Write());  // (no tails: no block)
  synthcard::Write sw;
  sw.source = cc::mptg::kSourceDevice;
  sw.fullRecords = false;
  const std::vector<uint8_t> ds = synthcard::tagsFile(card, sw);
  cc::MemSource transfer(t.data(), static_cast<uint32_t>(t.size()));
  cc::MemSource plainT(plain.data(), static_cast<uint32_t>(plain.size()));
  cc::MemSource rowsOnly(ds.data(), static_cast<uint32_t>(ds.size()));
  const std::vector<uint8_t> savedT = savedFrom(&transfer, rowsOnly);
  const std::vector<uint8_t> savedPlain = savedFrom(&plainT, rowsOnly);
  TEST_ASSERT_EQUAL_size_t(savedPlain.size() + tracks * 2, savedT.size());
  MoodRun mood;
  mood.transfer = &transfer;
  mood.moods = true;
  MoodRun fromT;
  fromT.transfer = &plainT;
  const UpdateRun moodOff = libraryUpdates(Dj::Off, savedT, rowsOnly, card, 2, mood);
  const UpdateRun moodOn = libraryUpdates(Dj::Placed, savedT, rowsOnly, card, 2, mood);
  const UpdateRun moodHeaps = libraryUpdates(Dj::WhereThe, savedT, rowsOnly, card, 2, mood);
  const UpdateRun tOn = libraryUpdates(Dj::Placed, savedPlain, rowsOnly, card, 2, fromT);  // (T, no block)
  printf("[join] the same with the mood words (a block of %u B), from T: the largest free block at the boot %u B; after "
         "the update %u B with Auto DJ off. Auto DJ on: joined at the boot %u B (%u B of it at the top), free at "
         "least %u B while placing; after the update %u B, joined %u B (%u B of it at the top), given back %u B; free "
         "at least %u B while placing. Its blocks where the heap puts them: joined %u B, given back %u B. Joined "
         "against the control's %u B: %+ld B (from T without the block: joined %u B, %+ld B)\n",
         (unsigned)(tracks * 2), (unsigned)moodOff.bootLargest, (unsigned)moodOff.largest, (unsigned)moodOn.bootJoined,
         (unsigned)moodOn.bootTopKept, (unsigned)moodOn.bootLowest, (unsigned)moodOn.largest, (unsigned)moodOn.joined,
         (unsigned)moodOn.topKept, (unsigned)moodOn.givenBack, (unsigned)moodOn.lowest, (unsigned)moodHeaps.joined,
         (unsigned)moodHeaps.givenBack, (unsigned)on.joined, (long)moodOn.joined - (long)on.joined,
         (unsigned)tOn.joined, (long)tOn.joined - (long)on.joined);
  // The pass rule (Ready: joinNow(), each join): the margin kept, at the
  // boot and after the update; the fence's frees, the build and the Live
  // step leave the heap as Auto DJ off has it; given back, off's block; at
  // the boot, everything at the top.
  TEST_ASSERT_TRUE(moodOn.lowest >= kMargin);
  TEST_ASSERT_TRUE(moodOn.bootLowest >= kMargin);
  TEST_ASSERT_TRUE(moodOn.fence == moodOff.fence);
  TEST_ASSERT_TRUE(moodHeaps.fence == moodOff.fence);
  TEST_ASSERT_EQUAL_size_t(moodOff.largest, moodOn.largest);
  TEST_ASSERT_EQUAL_size_t(moodOff.largest, moodOn.givenBack);
  TEST_ASSERT_EQUAL_size_t(155284, moodOn.bootTopKept);
  // The same rule where the heap puts the join's blocks (no top to keep).
  TEST_ASSERT_TRUE(moodHeaps.lowest >= kMargin);
  TEST_ASSERT_TRUE(moodHeaps.bootLowest >= kMargin);
  TEST_ASSERT_EQUAL_size_t(moodOff.largest, moodHeaps.largest);
  TEST_ASSERT_EQUAL_size_t(moodOff.largest, moodHeaps.givenBack);
  // No redesign forced (9.5): joined falls by no more than 120 KB, and not
  // under 245,764 B where the control is above it (the device's 335,860 B
  // less FLAC's 155,636 B, docs/AUTODJ.md 14.3, plus 65,540 B for Opus's
  // page buffer, docs/OPUS.md 1).
  TEST_ASSERT_TRUE(moodOn.joined + 122880 >= on.joined);
  TEST_ASSERT_TRUE(moodOn.joined >= 245764 || on.joined < 245764);
  // This shape, pinned: joined again as the control is, none of it at the
  // top (S1's stand-in, the block taken before the build's first record,
  // measured the same, its lowest after the update 84,104 B). The words
  // kept across clear() (rejected, 9.5), tried on this shape with the
  // stand-in, pass the rule above but not this: joined 278,516 B (-81,920
  // B), 77,640 B of it at the top.
  TEST_ASSERT_EQUAL_size_t(on.joined, moodOn.joined);
  TEST_ASSERT_EQUAL_size_t(0, moodOn.topKept);

}

// The listening stats in the same model (docs/LISTENING-STATS.md 5.14, S1's
// rework, its R6; 6.7's case), against the run without them as the control
// (the 19,412-track library, two update steps): the firmware's glue on a
// card of P = 2,214 played tracks (stats.bin's TRKD 1,000 rows, the rest in
// the base) or P = 19,478 (TRKD at the forced compaction's 4,096 rows, the
// base 15,382), its read at PlayHistory::begin()'s place (its object
// internal RAM's, outside the model), its boot's
// scan folding a log of 300 slots (one overflow chunk) or a full one never
// synced (13,106 slots: 16 chunks), the boot's save landing, landing at its
// second try, or failing all four (behind: the chunks given back, no save
// after); the scan first (the boot's join waiting for its blocks), the join
// first, or the scan after the first update (asked before its first
// piece); and a card with 12 MARK rows (MARK's block, carved by the boot's
// read in setup where the heap puts it and kept: PSRAM's on the device, the
// internal-RAM review, 5.14's fix G). Plays are written at each update's fence
// pass and after it, each 24 bringing the records' save. The mood plan's pass rule (9.5),
// with Auto DJ placed and where the heap puts its blocks: the margin kept
// at the boot and after the update; the fence's frees, the build and the
// Live step leave the heap as Auto DJ off has it (with the same stats); the
// join given back, off's block; joined within 122,880 B of the control's
// and at or over 245,764 B where the control is. And 5.14's own, stricter:
// joined the control's, or under it by no more than the class of the
// control's figure (ModelHeap::largest()'s rounding: 8,192 B at 360,436 B,
// half the device's 16,368 B of room over the line after a FLAC update,
// MOOD-FILTERS Run D, which the give-back's case held it to); the boot's
// join the control's the same way. Nothing per track is held, so P changes
// nothing but the saves' compactions. Every figure pinned.
void test_the_listening_stats_in_a_model_heap() {
  const uint32_t tracks = 19412;
  const synthcard::Card card = synthcard::make(synth::userShape(tracks));
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
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
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
  }
  const UpdateRun off = libraryUpdates(Dj::Off, saved, records, card, 2);
  const UpdateRun on = libraryUpdates(Dj::Placed, saved, records, card, 2);
  const StatsCard small = statsCard(2214, 1000), big = statsCard(19478, 4096), players = statsCard(2214, 1000, 12);
  printf("[join] the listening stats' cards: P = 2,214 (%u sessions), P = 19,478 (%u), 12 players' MARK rows (%u)\n",
         (unsigned)small.sessions, (unsigned)big.sessions, (unsigned)players.sessions);
  using Scan = StatsRun::Scan;
  struct Shape {
    const StatsCard* card;
    uint32_t slots;
    Scan when;
    int bootFails;
    // Pinned (MEASURED-HOST): MARK's block, the ranges' block and the
    // overflow's chunks at the scan's end (in the arena: 262,080 B held
    // whatever their count, 5.14's fix A), the boot save's tries, the saves
    // landed and asked, the compactions, behind; Auto DJ placed: the boot's
    // join and joined after the second update.
    size_t mark, ranges;
    uint32_t chunks;
    uint32_t tries, wrote, saves, compacted;
    bool behind;
    size_t bootJoined, joined;
  };
  const uint32_t full = 2 * playlog::kFileSlots;
  const int all = static_cast<int>(statsglue::kBootTries);
  const Shape shapes[] = {
      {&small, 300, Scan::ScanFirst, 0, 0, 1284, 1, 1, 5, 5, 1, false, 589812, 360436},
      {&small, full, Scan::ScanFirst, 0, 0, 1284, 16, 1, 5, 5, 1, false, 589812, 360436},
      {&big, 300, Scan::ScanFirst, 0, 0, 1284, 1, 1, 5, 5, 1, false, 589812, 360436},
      {&big, full, Scan::ScanFirst, 0, 0, 1284, 16, 1, 5, 5, 1, false, 589812, 360436},
      {&small, full, Scan::JoinFirst, 0, 0, 1284, 16, 1, 5, 5, 1, false, 589812, 360436},
      {&big, full, Scan::JoinFirst, 0, 0, 1284, 16, 1, 5, 5, 1, false, 589812, 360436},
      {&small, full, Scan::ScanFirst, 1, 0, 1284, 16, 2, 5, 6, 1, false, 589812, 360436},
      {&big, full, Scan::ScanFirst, 1, 0, 1284, 16, 2, 5, 6, 1, false, 589812, 360436},
      {&small, full, Scan::ScanFirst, all, 0, 1284, 16, 4, 0, 4, 0, true, 589812, 360436},
      {&big, full, Scan::ScanFirst, all, 0, 1284, 16, 4, 0, 4, 0, true, 589812, 360436},
      {&small, full, Scan::AfterUpdate, 0, 0, 1284, 16, 1, 4, 4, 1, false, 589812, 360436},
      {&big, full, Scan::AfterUpdate, 0, 0, 1284, 16, 1, 4, 4, 1, false, 589812, 360436},
      {&players, 30, Scan::ScanFirst, 0, 560, 1604, 0, 1, 5, 5, 1, false, 589812, 360436},
  };
  auto classOf = [](size_t largest) {
    return static_cast<size_t>(1) << (ModelHeap::fls(largest + ModelHeap::kPoison) - ModelHeap::kSlLog2);
  };
  const char* whenName[] = {"the scan first", "the join first", "the scan after the first update", "", ""};
  std::string broken;
  for (const Shape& sh : shapes) {
    StatsRun sr;
    sr.card = sh.card;
    sr.slots = sh.slots;
    sr.when = sh.when;
    sr.bootFails = sh.bootFails;
    StatsRun srOff = sr, srHeaps = sr;
    const UpdateRun statsOff = libraryUpdates(Dj::Off, saved, records, card, 2, MoodRun(), &srOff);
    const UpdateRun statsOn = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &sr);
    const UpdateRun statsHeaps = libraryUpdates(Dj::WhereThe, saved, records, card, 2, MoodRun(), &srHeaps);
    char w[160];
    std::snprintf(w, sizeof(w), "P = %u, %u MARK rows, %u slots, %s, the boot's save %s", (unsigned)sh.card->played,
                  (unsigned)sh.card->marks, (unsigned)sh.slots, whenName[static_cast<int>(sh.when)],
                  sh.bootFails == 0 ? "landing" : sh.bootFails == 1 ? "landing at its second try" : "failing every try");
    const bool rule = statsOn.lowest >= kMargin && statsOn.bootLowest >= kMargin && statsHeaps.lowest >= kMargin &&
                      statsHeaps.bootLowest >= kMargin && statsOn.fence == statsOff.fence &&
                      statsHeaps.fence == statsOff.fence && statsOn.largest == statsOff.largest &&
                      statsOn.givenBack == statsOff.largest && statsHeaps.largest == statsOff.largest &&
                      statsHeaps.givenBack == statsOff.largest && statsOn.joined + 122880 >= on.joined &&
                      (statsOn.joined >= 245764 || on.joined < 245764);
    const bool inClass = statsOn.joined + classOf(on.joined) >= on.joined &&
                         statsOn.bootJoined + classOf(on.bootJoined) >= on.bootJoined;
    printf("[join] with the listening stats (%s): MARK's block %u B (%u B held in the heap), the ranges' block %u B, "
           "the overflow %u B in %u chunks at the scan's end; the boot's save %u tries%s, %u of %u saves landed (%u compacting). Auto DJ off: "
           "after the update %u B (the control's %u B). Auto DJ on: the boot's join %u B (the control's %u B), free "
           "at least %u B while placing; after the update %u B, joined %u B (%u B of it at the top), the join given "
           "back %u B; free at least %u B while placing; the fence as off's: %s. Where the heap puts them: joined %u "
           "B, given back %u B. Joined against the control's %u B: %+ld B: the pass rule %s; within the control's "
           "class (%u B): %s\n",
           w, (unsigned)sr.markBytes, (unsigned)sr.markHeld, (unsigned)sr.rangesBytes, (unsigned)sr.chunkBytes, (unsigned)sr.chunks,
           (unsigned)sr.tries, sr.behind ? ", behind" : "", (unsigned)sr.wrote, (unsigned)sr.saves,
           (unsigned)sr.compacted, (unsigned)statsOff.largest, (unsigned)off.largest, (unsigned)statsOn.bootJoined,
           (unsigned)on.bootJoined, (unsigned)statsOn.bootLowest, (unsigned)statsOn.largest, (unsigned)statsOn.joined,
           (unsigned)statsOn.topKept, (unsigned)statsOn.givenBack, (unsigned)statsOn.lowest,
           statsOn.fence == statsOff.fence ? "yes" : "no", (unsigned)statsHeaps.joined, (unsigned)statsHeaps.givenBack,
           (unsigned)on.joined, (long)statsOn.joined - (long)on.joined, rule ? "holds" : "FAILS",
           (unsigned)classOf(on.joined), inClass ? "yes" : "NO");
    // The same stats whatever Auto DJ does.
    for (const StatsRun* o : {&srOff, &srHeaps}) {
      TEST_ASSERT_EQUAL_size_t_MESSAGE(sr.rangesBytes, o->rangesBytes, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(sr.chunkBytes, o->chunkBytes, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sr.wrote, o->wrote, w);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sr.saves, o->saves, w);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, sr.reserveFailed, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.mark, sr.markBytes, w);
    // MARK's block, a card past 8 rows, in this heap (PSRAM on the device:
    // Memory::keep, the firmware's psramAlloc, 5.14's fix G; the internal-RAM
    // review: internal RAM holds the object alone, whatever the card), one
    // footprint; none for 8 rows or fewer.
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.mark ? ModelHeap::carved(sh.mark) + ModelHeap::kHeader : 0, sr.markHeld, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.ranges, sr.rangesBytes, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.chunks, sr.chunks, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.chunks ? statsfold::kArenaBytes : 0, sr.chunkBytes, w);
    TEST_ASSERT_EQUAL_MESSAGE(sh.chunks > 0, sr.arena, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.tries, sr.tries, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.wrote, sr.wrote, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.saves, sr.saves, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.compacted, sr.compacted, w);
    TEST_ASSERT_TRUE_MESSAGE(sh.behind == sr.behind, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.bootJoined, statsOn.bootJoined, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.joined, statsOn.joined, w);
    // The mood plan's rule, whole, for every shape.
    TEST_ASSERT_TRUE_MESSAGE(statsOn.lowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(statsOn.bootLowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(statsHeaps.lowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(statsHeaps.bootLowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(statsOn.fence == statsOff.fence, w);
    TEST_ASSERT_TRUE_MESSAGE(statsHeaps.fence == statsOff.fence, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(statsOff.largest, statsOn.largest, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(statsOff.largest, statsOn.givenBack, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(statsOff.largest, statsHeaps.largest, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(statsOff.largest, statsHeaps.givenBack, w);
    // Given back, the update's room is Auto DJ off's without the stats.
    TEST_ASSERT_EQUAL_size_t_MESSAGE(off.largest, statsOff.largest, w);
    TEST_ASSERT_TRUE_MESSAGE(rule, w);
    if (!inClass) {
      char c[256];
      std::snprintf(c, sizeof(c), "%s%s (joined %+ld B, the boot's join %+ld B)", broken.empty() ? "" : "; ", w,
                    (long)statsOn.joined - (long)on.joined, (long)statsOn.bootJoined - (long)on.bootJoined);
      broken += c;
    }
  }
  if (!broken.empty()) {
    const std::string m = "joined under the control's by more than its class (5.14's R6): " + broken;
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

// S2's id-keyed half (docs/LISTENING-STATS.md 6.7, 9.6's C13), as the
// owner's "A: nothing held" (2026-11-20) has it: no block of the lists, the
// ids or the album bits between asks; each console ask (Hr, Hl, Hw) one
// block at the top through the hold (5.14's fix A), the decoder's room asked
// (it yields: the ask refused), the rank's two FatFiles beside it, all given
// back at its end. At P = 2,214 and P = 19,478, Hr's block and Hw's (the
// played set, the folders' states, the bits and the wanted set: the largest
// an ask carves), an ask after the boot's join and after each update's rejoin
// and plays, against the twin with no ask: every figure of the run the
// twin's (the boot's join, the fence's free blocks, joined, given back), each
// ask leaving free, the largest free block and the free blocks of 64 KB or
// more as they were (traceless), the mood plan's rule whole; the asks refused
// for room pinned (MODELLED). And the session's first Opus opening inside an
// ask's hold: it finds its room (the decoder's room the take asks).
void test_an_s2_ask_in_a_model_heap() {
  const uint32_t tracks = 19412;
  const synthcard::Card card = synthcard::make(synth::userShape(tracks));
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
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
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
  }
  const UpdateRun on = libraryUpdates(Dj::Placed, saved, records, card, 2);
  const StatsCard small = statsCard(2214, 1000), big = statsCard(19478, 4096);
  using Ask = StatsRun::Ask;
  struct Shape {
    const StatsCard* card;
    Ask ask;
    bool opus;
    // Pinned (MODELLED): the asks made (the boot's and one after each of the
    // two updates) and those refused for room. Hw's block at P = 19,478
    // (289,948 B: ten chunks of the played set) is refused after each
    // update: with it held no free block would take a decoder's buffers
    // (joined 360,436 B here; on the device 262-295 KB after an update). So
    // is P = 2,214's once the session's first Opus holds its 128 KB from
    // the first ask's hold on. Hr's (83,872 B) always fits.
    uint32_t asks, refused;
  };
  const Shape shapes[] = {
      {&small, Ask::Rank, false, 3, 0}, {&small, Ask::Walk, false, 3, 0}, {&big, Ask::Rank, false, 3, 0},
      {&big, Ask::Walk, false, 3, 2},   {&small, Ask::Walk, true, 3, 2},  {&big, Ask::Walk, true, 3, 2},
  };
  for (const Shape& sh : shapes) {
    StatsRun with, twin;
    with.card = twin.card = sh.card;
    with.slots = twin.slots = 30;
    with.ask = sh.ask;
    with.askOpus = sh.opus;
    const UpdateRun a = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &with);
    const UpdateRun b = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &twin);
    char w[160];
    std::snprintf(w, sizeof(w), "P = %u, %s%s", (unsigned)sh.card->played, sh.ask == Ask::Rank ? "Hr" : "Hw",
                  sh.opus ? ", the Opus opening in its hold" : "");
    printf("[join] an S2 ask (%s): its block %u B; %u asks, %u refused for room; while held the largest free block "
           "at least %u B, free at least %u B; traceless: %s; the boot's join %u B (the twin's %u B), joined %u B "
           "(the twin's %u B, Auto DJ on with no stats %u B)%s\n",
           w, (unsigned)with.askBytes, (unsigned)with.asks, (unsigned)with.askRefused,
           (unsigned)(with.askLowestLargest == SIZE_MAX ? 0 : with.askLowestLargest),
           (unsigned)(with.askLowestFree == SIZE_MAX ? 0 : with.askLowestFree), with.askTraceless ? "yes" : "NO",
           (unsigned)a.bootJoined, (unsigned)b.bootJoined, (unsigned)a.joined, (unsigned)b.joined, (unsigned)on.joined,
           sh.opus ? (with.askOpusShort ? "; the Opus found NO room" : "; the Opus opened") : "");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.asks, with.asks, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.refused, with.askRefused, w);
    TEST_ASSERT_TRUE_MESSAGE(with.askTraceless, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, with.askOpusShort, w);
    // The decoder's room kept while held: the largest free block takes the
    // Opus's three buffers (their carves, a thirty-second and 1 KB over).
    if (with.askLowestLargest != SIZE_MAX)
      TEST_ASSERT_TRUE_MESSAGE(with.askLowestLargest >= statsglue::kDecoderRoomBytes, w);
    // Free never under the hold's margin while held.
    if (with.askLowestFree != SIZE_MAX) TEST_ASSERT_TRUE_MESSAGE(with.askLowestFree >= kMargin, w);
    // Nothing between asks: the run's figures the twin's (with the Opus
    // opened in the hold, the twin opens none: only the rule).
    if (!sh.opus) {
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.bootJoined, a.bootJoined, w);
      TEST_ASSERT_TRUE_MESSAGE(b.fence == a.fence, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.largest, a.largest, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.joined, a.joined, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.givenBack, a.givenBack, w);
    }
    TEST_ASSERT_TRUE_MESSAGE(a.lowest >= kMargin && a.bootLowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(a.joined + 122880 >= on.joined && (a.joined >= 245764 || on.joined < 245764), w);
  }
}

// S3a's half (docs/LISTENING-STATS.md 9.7's V5: "the grades" and "nothing
// held"): the stats pages' asks as the console's twins make them, a visit
// (Hv: its snapshot carved plain before the take, then Full, then Lists;
// the snapshot freed at its landing) and Not played yet's (Hn: Lean, then
// No folders; no snapshot), each grade statsvisit::layout()'s block with
// the firmware's head (kS3AskHead) over this library's folders and albums
// and the card's u40 rows as stats.bin's directory gives them at the turn,
// through the hold (the decoder's room asked: it yields). At P = 2,214 and
// at P = 19,478 (the base's rows, with TRKD's 1,365: 9.7's column), an ask
// after the boot's join and after each update's rejoin, with and without
// the session's first Opus opening in the first taken ask's hold (kept from
// there on), against the twin with no ask. Each grade taken or refused
// pinned (MODELLED); every ask leaving the heap as it found it
// (test_an_s2_ask_in_a_model_heap()'s rule: free, the largest free block
// and the free blocks of 64 KB or more as before it; a refused take as
// before its take); the decoder's room and the margin kept while held. And
// nothing held (the control): without the Opus every figure of the run the
// twin's (the boot's join, the fence's free blocks, the update's largest,
// joined, given back): the asks alone, their snapshot with them, move
// nothing.
void test_an_s3_ask_in_a_model_heap() {
  const uint32_t tracks = 19412;
  const synthcard::Card card = synthcard::make(synth::userShape(tracks));
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
  std::vector<uint8_t> saved;
  uint32_t folders = 0, albums = 0;
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
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
    folders = pathhash::Walk::foldersFor(idx);
    albums = idx.albumCount();
  }
  printf("[join] the S3 asks' library: %u tracks, %u folders (the folders' states %u B), %u albums (the snapshot %u "
         "B); the spare's 2,478 and 1,706 (24,780 B, 3,860 B)\n",
         (unsigned)tracks, (unsigned)folders, (unsigned)pathhash::Walk::bytesFor(folders), (unsigned)albums,
         (unsigned)statsvisit::bytesFor(albums));
  const UpdateRun on = libraryUpdates(Dj::Placed, saved, records, card, 2);
  // P = 19,478 as 9.7's column has it: the base's 19,478 rows and TRKD's
  // 1,365 more (each play here a track of its own: 20,843 tracks played).
  const StatsCard small = statsCard(2214, 1000), big = statsCard(19478 + 1365, 1365);
  using Ask = StatsRun::Ask;
  struct Shape {
    const StatsCard* card;
    Ask ask;
    bool opus;
    // Pinned (MODELLED): each ask's grade taken ("refused": every grade), the
    // boot's, then after each of the two updates; the boot's ask's u40 rows
    // and its grades' blocks, in the order tried. This library's 2,562
    // folders and 1,785 albums make Full and Lean 848 B over 9.7's spare
    // column with the same rows and head (139,376 and 131,024 B at P =
    // 19,478); Lists and No folders are the work memory's (the host's, 32 B
    // over the device's). Every ask fits after the boot's join (589,812 B)
    // and after each update (360,436 B). With the Opus kept (249,844 B after
    // each update): at P = 19,478 Full is refused, the visit taking Lists,
    // and so are the lean ask and No folders (9.7's fit table's 249,844 B
    // row); P = 2,214's every grade fits.
    const char* took;
    uint32_t rows;
    size_t bytes[2];
  };
  const Shape shapes[] = {
      {&small, Ask::Visit, false, "Full, Full, Full", 2254, {85440, 81504}},
      {&small, Ask::NotPlayed, false, "Lean, Lean, Lean", 2254, {38736, 25328}},
      {&big, Ask::Visit, false, "Full, Full, Full", 20883, {140224, 81504}},
      {&big, Ask::NotPlayed, false, "Lean, Lean, Lean", 20883, {131872, 118464}},
      {&small, Ask::Visit, true, "Full, Full, Full", 2254, {85440, 81504}},
      {&small, Ask::NotPlayed, true, "Lean, Lean, Lean", 2254, {38736, 25328}},
      {&big, Ask::Visit, true, "Full, Lists, Lists", 20883, {140224, 81504}},
      {&big, Ask::NotPlayed, true, "Lean, refused, refused", 20883, {131872, 118464}},
  };
  // The twins: the same stats, no ask.
  StatsRun twinSmall, twinBig;
  twinSmall.card = &small;
  twinBig.card = &big;
  twinSmall.slots = twinBig.slots = 30;
  const UpdateRun bSmall = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &twinSmall);
  const UpdateRun bBig = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &twinBig);
  for (const Shape& sh : shapes) {
    StatsRun with;
    with.card = sh.card;
    with.slots = 30;
    with.ask = sh.ask;
    with.askOpus = sh.opus;
    const UpdateRun a = libraryUpdates(Dj::Placed, saved, records, card, 2, MoodRun(), &with);
    const UpdateRun& b = sh.card == &small ? bSmall : bBig;
    char w[160];
    std::snprintf(w, sizeof(w), "P = %u%s, %s%s", (unsigned)(sh.card == &small ? 2214 : 19478),
                  sh.card == &small ? "" : " (TRKD 1,365)",
                  sh.ask == Ask::Visit ? "Hv (Full, Lists)" : "Hn (Lean, No folders)",
                  sh.opus ? ", the Opus opening in its hold" : "");
    const std::string took = tookOf(with);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(3, with.turns.size(), w);
    const StatsRun::Turn& t0 = with.turns[0];
    std::string turns;
    for (const StatsRun::Turn& t : with.turns) {
      char tb[160];
      std::snprintf(tb, sizeof(tb), "%s%u rows, %u / %u B at a largest free block of %u B%s", turns.empty() ? "" : "; ",
                    (unsigned)t.rows, (unsigned)t.bytes[0], (unsigned)t.bytes[1], (unsigned)t.largest,
                    t.snapCarved ? (t.outOfL ? ", the snapshot out of it" : ", the snapshot elsewhere") : "");
      turns += tb;
    }
    printf("[join] an S3 ask (%s): %s; %u asks, %u refused for room; the turns: %s; while held the largest free block "
           "at least %u B, free at least %u B; traceless: %s; the boot's join %u B (the twin's %u B), joined %u B (the "
           "twin's %u B, Auto DJ on with no stats %u B)%s\n",
           w, took.c_str(), (unsigned)with.asks, (unsigned)with.askRefused, turns.c_str(),
           (unsigned)(with.askLowestLargest == SIZE_MAX ? 0 : with.askLowestLargest),
           (unsigned)(with.askLowestFree == SIZE_MAX ? 0 : with.askLowestFree), with.askTraceless ? "yes" : "NO",
           (unsigned)a.bootJoined, (unsigned)b.bootJoined, (unsigned)a.joined, (unsigned)b.joined, (unsigned)on.joined,
           sh.opus ? (with.askOpusShort ? "; the Opus found NO room" : "; the Opus opened") : "");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(sh.took, took.c_str(), w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sh.rows, t0.rows, w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.bytes[0], t0.bytes[0], w);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(sh.bytes[1], t0.bytes[1], w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, with.asks, w);
    TEST_ASSERT_TRUE_MESSAGE(with.askTraceless, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, with.askOpusShort, w);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, with.pageFreed, w);  // (nothing held: Hv's snapshot freed at its landing)
    // The decoder's room kept while held; free never under the hold's margin.
    if (with.askLowestLargest != SIZE_MAX)
      TEST_ASSERT_TRUE_MESSAGE(with.askLowestLargest >= statsglue::kDecoderRoomBytes, w);
    if (with.askLowestFree != SIZE_MAX) TEST_ASSERT_TRUE_MESSAGE(with.askLowestFree >= kMargin, w);
    // Nothing held: the run's figures the twin's (with the Opus opened in
    // the hold, the twin opens none: only the rule).
    if (!sh.opus) {
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.bootJoined, a.bootJoined, w);
      TEST_ASSERT_TRUE_MESSAGE(b.fence == a.fence, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.largest, a.largest, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.joined, a.joined, w);
      TEST_ASSERT_EQUAL_size_t_MESSAGE(b.givenBack, a.givenBack, w);
    }
    TEST_ASSERT_TRUE_MESSAGE(a.lowest >= kMargin && a.bootLowest >= kMargin, w);
    TEST_ASSERT_TRUE_MESSAGE(a.joined + 122880 >= on.joined && (a.joined >= 245764 || on.joined < 245764), w);
  }
}

// 9.7's reduced-room shape (V5; its fit table's 286,708 B row, the device's
// worst after an update: after the stopped update, and after Dc; MEASURED):
// PSRAM's largest free block reporting 286,708 B (largest(): its class floor
// less the poisoning's 12 B), the rest of free in blocks under the
// snapshot's class (64 of 3,000 B: the margin the take keeps free with its
// block carved, as the device's 1-2 MB free leaves it), and one free block
// of the snapshot's carve (it goes there: "elsewhere") or none (it comes out
// of the largest: "out of L"). 9.7's parts: the spare's 2,478 folders and
// 1,706 albums (the snapshot 3,860 B), P = 19,478 with TRKD 1,365 (20,883
// u40 rows) and TRKD's cap (23,614), the head the firmware's (kS3AskHead),
// the work memory the host's (32 B over the device's where it sets a
// grade's room). The visit's snapshot carved first (plain), then each grade
// asked alone through the hold with it held, as 9.7's fit table gives each
// grade's cell both ways: Full and Lists (Hv's), Lean and No folders (Hn's,
// whatever the visit took). Every ask leaves the heap as it found it, so
// the page's chains read from them: the visit (Full, then Lists) and, after
// a visit that took Lists, Not played yet's Try again (the lean ask: Lean,
// then No folders) with the snapshot held. Each grade's margin is the
// model's: the largest free block with its block held (a take without the
// decoder's room asked, given back) less the room the take asks
// (carvedAtMost(the Opus's buffers): 133,047 B), beside T (the free block
// left above it, which the caps close). The raw block at its class's floor
// (286,720 B, the least room the report allows: a snapshot out of it then
// reads one class less, as 9.7's fit assumes) is pinned grade by grade,
// each margin and T; across the class (every 64 B to its top) each grade
// taken and the page's outcomes are counted, pinned (MODELLED): the near
// cells 9.7's rule can't call, which run 3a-3 decides on the device.
// The model against 9.7's rule (B at most (L - 133,047 - T) x 32/33, T
// about 5.4 KB): at P = 19,478 Full is taken with the snapshot elsewhere
// (+6,205 B, T 4,088 B), as the rule has it, and out of L too (+2,109 B,
// T 1,584 B), which the rule refuses (135,824 B at most against 139,376 B).
// Out of L the raw block is 282,812 B: two spacers (131,072 and 6,784 B,
// the second closing in) leave the block's 143,360 B carve (139,376 B
// rounded to its 4,096 B class) with T 1,584 B above it, and given back
// they are a free block of 137,860 B under it, reported 135,156 B: 2,109 B
// over 133,047 B. The rule's T is 5.4 KB and its 32/33 a whole
// thirty-second lost to the class floor; here the floor takes 2,692 B. So
// at P = 19,478 the visit takes Full at every raw size of the class, either
// way, and the Lists-then-lean chain 9.7 gives for this cell never comes in
// a page. Asked alone (Hn's), the lean ask is taken there either way at
// every raw size (+14,397 / +10,301 B at the floor, T 8,184 / 5,296 B), as
// 9.7's Lean column has it (12,856 / 4,912 B) and 13's fallback relies on.
// Lists and No folders are taken in every cell at every raw size, as 9.7
// has them (No folders at TRKD's cap +14,397 / +10,301 B, 9.7's 11,784 /
// 3,840 B). At TRKD's cap Full is refused either way (-6,083 and -10,179
// B), as the rule has it; the lean ask is taken with the snapshot elsewhere
// (+2,109 B: a 135,168 B spacer under the block's 147,456 B carve, T 4,088
// B), which the rule refuses (143,768 B at most against 144,688 B), and
// refused out of L (-1,987 B), No folders then taken (+10,301 B). That
// refusal is 9.7's outcome at the floor, but not its cell: 9.7 has it
// 8,752 B short, past one class of L (not near); the model has it near,
// taken at 66 raw sizes of the class's 128 (No folders at the other 62).
void test_an_s3_ask_in_a_reduced_room() {
  namespace sv = statsvisit;
  const uint32_t albums = 1706;
  const size_t foldersBytes = pathhash::Walk::bytesFor(2478);
  const size_t snapBytes = sv::bytesFor(albums);
  TEST_ASSERT_EQUAL_size_t(3860, snapBytes);
  const uint32_t pRows = 19478 + 1365 + 40, capRows = 19478 + 4096 + 40;
  constexpr size_t kL = 286708;
  const size_t floorRaw = kL + ModelHeap::kPoison;  // 286,720 B
  const size_t room = statsglue::kDecoderRoomBytes + statsglue::kDecoderRoomBytes / 32 + 1024;  // carvedAtMost()
  TEST_ASSERT_EQUAL_size_t(133047, room);
  struct Try {
    long margin = 0;   // the largest free block with the block held, less `room`
    size_t above = 0;  // T
    bool taken = false;
  };
  struct Out {
    bool outOfL = false;
    Try grade[4];  // Full, Lists, Lean, No folders: each asked alone, the snapshot held
    int visitTook = -1, leanTook = -1;  // a page's: the visit's grade, its Try again's (-1: refused, or not asked)
  };
  auto tryGrade = [&](size_t bytes) {
    Try t;
    autodj::CloseReport r;
    const size_t f = ModelHeap::freeBytes(), lg = ModelHeap::rawLargest();
    const Map start = ModelHeap::blocks;
    void* q = g_modelTop.take(kModelTop, bytes, &r, true, 0, true);  // (the probe: no decoder's room asked)
    TEST_ASSERT_NOT_NULL(q);
    t.margin = static_cast<long>(ModelHeap::largest()) - static_cast<long>(room);
    t.above = r.aboveBytes;
    g_modelTop.give(kModelTop, q);
    TEST_ASSERT_EQUAL_size_t(f, ModelHeap::freeBytes());
    TEST_ASSERT_EQUAL_size_t(lg, ModelHeap::rawLargest());
    void* p = g_modelTop.take(kModelTop, bytes, &r, true, statsglue::kDecoderRoomBytes, true);
    t.taken = p != nullptr;
    TEST_ASSERT_EQUAL(t.margin >= 0, t.taken);  // (the take's own check: the largest free block against `room`)
    if (p) g_modelTop.give(kModelTop, p);
    TEST_ASSERT_EQUAL_INT(0, g_modelTop.held());
    TEST_ASSERT_EQUAL_size_t(f, ModelHeap::freeBytes());  // (taken or refused: no trace)
    TEST_ASSERT_EQUAL_size_t(lg, ModelHeap::rawLargest());
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));  // (so the next grade's ask meets the same heap)
    return t;
  };
  const sv::Grade grades[4] = {sv::Grade::Full, sv::Grade::Lists, sv::Grade::Lean, sv::Grade::NoFolders};
  auto run = [&](size_t raw, bool elsewhere, uint32_t rows) {
    Out o;
    std::vector<size_t> holes(64, 2988);  // (3,000 B blocks: under the snapshot's 3,904 B class)
    if (elsewhere) holes.insert(holes.begin(), snapBytes);
    holesThenTop(holes, raw);
    TEST_ASSERT_EQUAL_size_t(kL, ModelHeap::largest());
    size_t lAt = 0;
    for (const auto& b : ModelHeap::blocks)
      if (!b.second.used && b.second.size == raw) lAt = b.first;
    void* snap = ModelHeap::alloc(snapBytes);
    TEST_ASSERT_NOT_NULL(snap);
    o.outOfL = ModelHeap::offsetOf(snap) == lAt;
    TEST_ASSERT_EQUAL(!elsewhere, o.outOfL);
    for (int i = 0; i < 4; ++i)
      o.grade[i] = tryGrade(sv::layout(grades[i], kS3AskHead, rows, foldersBytes, albums, true).bytes);
    // A page's chains: Full, then Lists; after Lists, the lean ask (Lean,
    // then No folders). Each refused ask left no trace, so the next one met
    // the heap this one did.
    o.visitTook = o.grade[0].taken ? 0 : o.grade[1].taken ? 1 : -1;
    if (o.visitTook == 1) o.leanTook = o.grade[2].taken ? 0 : o.grade[3].taken ? 1 : -1;
    ModelHeap::release(snap);
    return o;
  };
  auto said = [](const Out& o) {
    static const char* const names[] = {"Full", "Lists", "Lean", "No folders"};
    std::string s;
    char b[160];
    for (int i = 0; i < 4; ++i) {
      std::snprintf(b, sizeof(b), "%s%s %s (%+ld B, T %u B)", i == 0 ? "" : i == 2 ? "; Hn's: " : ", ", names[i],
                    o.grade[i].taken ? "taken" : "refused", o.grade[i].margin, (unsigned)o.grade[i].above);
      s += b;
    }
    std::snprintf(b, sizeof(b), "; a page's visit: %s%s%s", o.visitTook < 0 ? "refused" : names[o.visitTook],
                  o.visitTook == 1 ? ", then its lean ask: " : "",
                  o.visitTook != 1 ? "" : o.leanTook < 0 ? "refused" : names[2 + o.leanTook]);
    s += b;
    return s;
  };
  const char* rowsName[] = {"P = 19,478 with TRKD 1,365 (20,883 rows)", "TRKD's cap (23,614 rows)"};
  const uint32_t rowsOf[] = {pRows, capRows};
  // Pinned (MODELLED), [the rows][the snapshot elsewhere, out of L]: each
  // grade's margin and T at the class's floor (Full, Lists, Lean, No
  // folders), a page's grades there (the visit's; its lean ask's, -1: not
  // asked, or refused), and across the class each grade taken, and a page's
  // outcomes: Full; Lists, then Lean; Lists, then No folders; else.
  struct Pin {
    long margin[4];
    uint32_t above[4];
    int visitTook, leanTook;
    uint32_t taken[4], page[4];
  };
  const Pin pinned[2][2] = {
      {{{6205, 67645, 14397, 26685}, {4088, 4088, 8184, 8184}, 0, -1, {128, 128, 128, 128}, {128, 0, 0, 0}},
       {{2109, 63549, 10301, 22589}, {1584, 3376, 5296, 4656}, 0, -1, {128, 128, 128, 128}, {128, 0, 0, 0}}},
      {{{-6083, 67645, 2109, 14397}, {4088, 4088, 4088, 4088}, 1, 0, {0, 128, 128, 128}, {0, 128, 0, 0}},
       {{-10179, 63549, -1987, 10301}, {3504, 3376, 2992, 2352}, 1, 1, {0, 128, 66, 128}, {0, 66, 62, 0}}},
  };
  for (int k = 0; k < 2; ++k) {
    printf("[join] the reduced room, %s: Full %u B, Lists %u B, Lean %u B, No folders %u B\n", rowsName[k],
           (unsigned)sv::layout(sv::Grade::Full, kS3AskHead, rowsOf[k], foldersBytes, albums, true).bytes,
           (unsigned)sv::layout(sv::Grade::Lists, kS3AskHead, rowsOf[k], foldersBytes, albums, true).bytes,
           (unsigned)sv::layout(sv::Grade::Lean, kS3AskHead, rowsOf[k], foldersBytes, albums, true).bytes,
           (unsigned)sv::layout(sv::Grade::NoFolders, kS3AskHead, rowsOf[k], foldersBytes, albums, true).bytes);
    for (bool elsewhere : {true, false}) {
      const Pin& pin = pinned[k][elsewhere ? 0 : 1];
      const Out o = run(floorRaw, elsewhere, rowsOf[k]);
      printf("[join] the reduced room (L %u B, the raw block %u B), %s, the snapshot %s: %s\n", (unsigned)kL,
             (unsigned)floorRaw, rowsName[k], elsewhere ? "elsewhere" : "out of L", said(o).c_str());
      char w[160];
      std::snprintf(w, sizeof(w), "the reduced room, %s, the snapshot %s", rowsName[k],
                    elsewhere ? "elsewhere" : "out of L");
      for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(pin.margin[i], o.grade[i].margin, w);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pin.above[i], o.grade[i].above, w);
      }
      TEST_ASSERT_EQUAL_INT_MESSAGE(pin.visitTook, o.visitTook, w);
      TEST_ASSERT_EQUAL_INT_MESSAGE(pin.leanTook, o.leanTook, w);
      // Across the class: each grade taken, and the grade a page's asks came
      // to, counted.
      uint32_t n = 0, taken[4] = {0, 0, 0, 0}, page[4] = {0, 0, 0, 0};
      for (size_t raw = floorRaw; raw < floorRaw + 8192; raw += 64) {
        const Out c = run(raw, elsewhere, rowsOf[k]);
        ++n;
        for (int i = 0; i < 4; ++i) taken[i] += c.grade[i].taken ? 1 : 0;
        ++page[c.visitTook == 0 ? 0 : c.leanTook == 0 ? 1 : c.leanTook == 1 ? 2 : 3];
      }
      printf("[join] the reduced room across L's class (%u raw blocks, %u to %u B), %s, the snapshot %s: taken: "
             "Full %u, Lists %u, Lean %u, No folders %u; a page's: Full %u; Lists, then Lean %u; Lists, then No "
             "folders %u; else %u\n",
             (unsigned)n, (unsigned)floorRaw, (unsigned)(floorRaw + 8192 - 64), rowsName[k],
             elsewhere ? "elsewhere" : "out of L", (unsigned)taken[0], (unsigned)taken[1], (unsigned)taken[2],
             (unsigned)taken[3], (unsigned)page[0], (unsigned)page[1], (unsigned)page[2], (unsigned)page[3]);
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(128, n, w);
      for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pin.taken[i], taken[i], w);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(pin.page[i], page[i], w);
      }
    }
  }
}

// The boot's hold (5.14, fix A + G; the device's B1 runs, 2026-11-17): what
// the stats hold across passes (the ranges' block, a full log's overflow)
// sits at the top of the largest free block, and main's own long-lived
// carves made while it is held (the first frame's cover canvas, a queue
// started from the console, a tap or a headphone key) must go where main
// puts them, so that once the hold ends PSRAM's layout is main's. Each case
// against its twin without the stats (statsOff: the same pieces, the same
// carves at the same points, no glue): (a) the blocks main carved in the
// hold at main's offsets; (b) after the hold, the largest free block and the
// free blocks of 64 KB or more main's; (c) the boot's join (the Dc's after
// a remount) main's: its blocks' offsets, its placement's figures, the
// largest free block once joined. Y1 I-3m (the usual log, a queue, a start's
// probe and the first frame after the scan's first piece); Y2 the first
// frame alone in the hold; Y3 I-1 (a full log: the queue after piece 3, the
// first frame after piece 70, about the 11th chunk, the start waiting); Y4
// the first frame in the boot save's window (the ranges' block given back
// by commit(), the overflow held); Y5 a remount's re-read mid-session (a
// FLAC's next track opening in its hold; the holds under 64 KB taken first,
// so the ranges' block sits on the spacers: a free block of 4 KB or more
// above it, the case's T); Y6 a card past 8 MARK rows (12, and 40: MARK's
// block kept for the session from the boot's read), against a control that
// replays the read's carves (MARK's block where the read leaves it), and
// against bare main within 5.14's one class; Y7 the first frame between the
// boot save's tries.
void test_the_hold_leaves_main_s_layout() {
  const uint32_t tracks = 19412;
  const synthcard::Card card = synthcard::make(synth::userShape(tracks));
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
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
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
  }
  const StatsCard sc = statsCard(2214, 1000), rows12 = statsCard(2214, 1000, 12),
                  rows40 = statsCardRows(2214, 1000, 40);
  const uint32_t full = 2 * playlog::kFileSlots;
  auto classOf = [](size_t largest) {
    return static_cast<size_t>(1) << (ModelHeap::fls(largest + ModelHeap::kPoison) - ModelHeap::kSlLog2);
  };
  // One case: the stats' run and its twin; what differs, said.
  std::string bad;
  auto run = [&](const char* name, StatsRun on, bool replay, bool remount, StatsRun* outOn = nullptr,
                 StatsRun* outOff = nullptr) {
    on.scanFile = true;
    StatsRun off = on;
    off.statsOff = true;
    const UpdateRun uOn = libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &on);
    if (replay) off.replayRead = &on.readEvents;
    const UpdateRun uOff = libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &off);
    (void)uOn;
    (void)uOff;
    const StatsRun::Seen& a = remount ? on.again : on.boot;
    const StatsRun::Seen& m = remount ? off.again : off.boot;
    std::string why;
    auto differ = [&](const char* what, bool same) {
      if (same) return;
      why += why.empty() ? "" : ", ";
      why += what;
    };
    differ("the hold's kept blocks' offsets", a.kept == m.kept);
    differ("the largest free block after the hold", a.largest == m.largest);
    differ("the free blocks of 64 KB or more after the hold", a.free64 == m.free64);
    differ("the join's blocks' offsets", a.layout.filters == m.layout.filters &&
                                              a.layout.rowToTrack == m.layout.rowToTrack &&
                                              a.layout.trackToRow == m.layout.trackToRow);
    differ("the join's placement", a.placed == m.placed && a.keptElsewhere == m.keptElsewhere &&
                                       a.runsElsewhere == m.runsElsewhere && a.topBytes == m.topBytes &&
                                       a.topKept == m.topKept);
    differ("the largest once joined", a.joined == m.joined);
    differ("the hold's top blocks given back", a.held == 0 && on.heldAtEnd == 0);
    printf("[hold] %s: after the hold %u B (main's %u B), %u free blocks of 64 KB+ (main's %u); joined %u B (main's %u "
           "B, %+ld B), filters at %u (main's %u), %u B at the top (main's %u), kept elsewhere %u (%u), transient "
           "elsewhere %u (%u); the ranges' block %u B, the overflow %u chunks; the stats' top left %u B free above it: "
           "%s%s\n",
           name, (unsigned)a.largest, (unsigned)m.largest, (unsigned)a.free64.size(), (unsigned)m.free64.size(),
           (unsigned)a.joined, (unsigned)m.joined, (long)a.joined - (long)m.joined, (unsigned)a.layout.filters,
           (unsigned)m.layout.filters, (unsigned)a.topBytes, (unsigned)m.topBytes, (unsigned)a.keptElsewhere,
           (unsigned)m.keptElsewhere, (unsigned)a.runsElsewhere, (unsigned)m.runsElsewhere, (unsigned)on.rangesBytes,
           (unsigned)on.chunks, (unsigned)on.topAbove, why.empty() ? "main's" : "NOT main's: ", why.c_str());
    if (!why.empty()) bad += (bad.empty() ? std::string() : std::string("; ")) + name + ": " + why;
    if (outOn) *outOn = on;
    if (outOff) *outOff = off;
  };
  StatsRun usual;
  usual.card = &sc;
  StatsRun fullLog = usual;
  fullLog.slots = full;

  StatsRun y1 = usual;  // Y1: I-3m
  y1.queueAt = 0;
  y1.uiAt = 0;
  run("Y1 I-3m (the usual log; a queue, a start's probe and the first frame after the first piece)", y1, false, false);
  StatsRun y2 = usual;  // Y2
  y2.uiAt = 0;
  run("Y2 the first frame alone in the hold", y2, false, false);
  StatsRun y3 = fullLog;  // Y3: I-1
  y3.queueAt = 3;
  y3.uiAt = 70;
  run("Y3 I-1 (a full log; the queue after piece 3, the first frame after piece 70, the start waiting)", y3, false,
      false);
  StatsRun y4 = fullLog;  // Y4
  y4.uiAt = StatsRun::kInSave;
  run("Y4 a full log, the first frame in the boot save's window", y4, false, false);
  StatsRun y5 = usual;  // Y5: a remount mid-session
  y5.uiAt = StatsRun::kBeforeJoin;  // (main's order at the boot: its hold is I-2's)
  y5.remount = true;
  y5.takeHoles = true;
  StatsRun y5on;
  run("Y5 a remount's re-read mid-session, a FLAC's next track in its hold, the holes taken", y5, false, true, &y5on);
  // (the case's T: the free block the ranges' block left above it at its top)
  TEST_ASSERT_TRUE_MESSAGE(y5on.topAbove >= 4096, "Y5: the ranges' block left no T above it");
  {
    // The same without the holes taken: the ranges' block in a hole, within
    // a class of main's.
    StatsRun y5h = y5;
    y5h.takeHoles = false;
    StatsRun a, m;
    const std::string before = bad;
    run("Y5' the same, the holes not taken (the ranges' block in a hole)", y5h, false, true, &a, &m);
    bad = before;  // (printed; asserted within a class)
    TEST_ASSERT_TRUE_MESSAGE(a.again.joined + classOf(m.again.joined) >= m.again.joined, "Y5': under main's class");
  }
  // Y5b: a remount whose re-read folds an overflow mid-session, under Auto
  // DJ's join (a session behind: the boot save's four tries failed, so the
  // card's MARK is the boot's and the re-read folds the full log again): the
  // ranges' block and the arena, a FLAC's next track opening in that hold;
  // the holes taken and not. Without a free block of 262 KB mid-session the
  // arena falls back to the chunks one by one (fe61db9's layout: the spec's
  // open question 5, an owner's call): said, and not held to main's then.
  for (bool holes : {true, false}) {
    StatsRun y5b = fullLog;
    y5b.uiAt = StatsRun::kBeforeJoin;
    y5b.bootFails = static_cast<int>(statsglue::kBootTries);
    y5b.remount = true;
    y5b.takeHoles = holes;
    char n5[160];
    std::snprintf(n5, sizeof(n5), "Y5b a remount's re-read of a full log mid-session (the boot behind), the holes %s",
                  holes ? "taken" : "not taken");
    StatsRun a, m;
    const std::string before = bad;
    run(n5, y5b, false, true, &a, &m);
    printf("[hold] %s: the boot %s; the re-read's overflow %u chunks, %s\n", n5, a.behind ? "behind" : "NOT behind",
           (unsigned)a.againChunks,
           a.againArena ? "in the arena (carved mid-session)"
                        : "one by one (the arena given back for a decoder's room, a FLAC playing, or no free block "
                          "of 262 KB mid-session: fe61db9's layout)");
    TEST_ASSERT_TRUE_MESSAGE(a.behind, n5);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(statsfold::kMaxChunks, a.againChunks, n5);
    if (!a.againArena) bad = before;  // (the fallback: printed, the owner's call)
  }
  // Y5d (the recheck, hazard 5): Y5b with an MP3 playing (it holds no
  // PSRAM) and the next track the session's first Opus, opening in the
  // re-read's hold (never held: OutputHold holds a start, not a track that
  // follows one playing) after its first piece (the arena held: every
  // chunk's room) or its last (fe61db9's 16 chunks held too), its three
  // buffers kept for the session; with the session's blocks 0 to 224 KB more
  // (less free at the remount: a larger library, a library update's growth),
  // the holes taken and not. Against main's twin and fe61db9's hooks: the
  // Opus never short where fe61db9's hold left it room (the caps keep the
  // margin free, not a decoder's blocks: topAlloc()'s room gives them back,
  // and the arena, when the largest free block wouldn't take them), never on
  // main; at the model's own session (no more blocks) main's layout.
  {
    uint32_t cases = 0, mains = 0, fixShort = 0, oldShort = 0, yielded = 0, worseN = 0, noRoomBoth = 0;
    std::string worse;
    for (uint32_t at : {0u, 102u}) {
      for (bool holes : {true, false}) {
        for (size_t kb = 0; kb <= 224; kb += 16) {
          StatsRun y = fullLog;
          y.uiAt = StatsRun::kBeforeJoin;
          y.bootFails = static_cast<int>(statsglue::kBootTries);
          y.remount = true;
          y.takeHoles = holes;
          y.nextOpus = true;
          y.nextAt = at;
          y.less = kb * 1024;
          y.scanFile = true;
          StatsRun a = y, m = y, o = y;
          m.statsOff = true;
          o.fe61db9 = true;
          libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &a);
          libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &m);
          libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &o);
          char w[112];
          std::snprintf(w, sizeof(w), "Y5d the Opus after piece %u, the holes %s, %u KB less free", (unsigned)at,
                        holes ? "taken" : "not taken", (unsigned)kb);
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, m.nextShort, w);  // (main: no hold)
          TEST_ASSERT_TRUE_MESSAGE(a.behind, w);
          // (The re-read folds what fe61db9's did: the whole log wherever it did; short of room for its
          // chunks, both out of memory.)
          TEST_ASSERT_TRUE_MESSAGE(o.againChunks < statsfold::kMaxChunks || a.againChunks == statsfold::kMaxChunks, w);
          noRoomBoth += a.againChunks < statsfold::kMaxChunks ? 1 : 0;
          ++cases;
          fixShort += a.nextShort;
          oldShort += o.nextShort;
          yielded += a.againArena ? 0 : 1;
          const bool same = !a.nextShort && a.again.largest == m.again.largest && a.again.free64 == m.again.free64 &&
                            a.again.joined == m.again.joined && a.again.layout.filters == m.again.layout.filters &&
                            a.again.held == 0 && a.heldAtEnd == 0;
          mains += same ? 1 : 0;
          if (a.nextShort && !o.nextShort) {
            ++worseN;
            worse += std::string(worse.empty() ? "" : "; ") + w;
          }
          if (kb == 0) {
            printf("[hold] %s: the Opus %s (fe61db9's %s); after the hold %u B (main's %u B, fe61db9's %u B), joined "
                   "%u B (main's %u B, fe61db9's %u B); the re-read's overflow %s: %s\n",
                   w, a.nextShort ? "SHORT" : "opened", o.nextShort ? "SHORT" : "opened", (unsigned)a.again.largest,
                   (unsigned)m.again.largest, (unsigned)o.again.largest, (unsigned)a.again.joined,
                   (unsigned)m.again.joined, (unsigned)o.again.joined,
                   a.againArena ? "in the arena" : "one by one (the arena given back)", same ? "main's" : "NOT main's");
            TEST_ASSERT_TRUE_MESSAGE(same, w);  // (the model's session: the room kept with the caps held)
          }
        }
      }
    }
    printf("[hold] Y5d, %u cases (the Opus after the first piece or the last, the holes taken or not, 0-224 KB less "
           "free): the Opus short %u (fe61db9's hooks %u; main's 0), short where fe61db9's opened %u; main's layout "
           "%u; the arena given back for the room %u; the re-read short of room for its chunks %u (fe61db9's too)\n",
           (unsigned)cases, (unsigned)fixShort, (unsigned)oldShort, (unsigned)worseN, (unsigned)mains,
           (unsigned)yielded, (unsigned)noRoomBoth);
    TEST_ASSERT_TRUE_MESSAGE(worse.empty(), ("the Opus short where fe61db9's hooks left it room: " + worse).c_str());
    TEST_ASSERT_TRUE(oldShort > 0);  // (the case reaches where fe61db9's hold leaves no room either)
  }
  // Y5c: a remount on a card past 8 MARK rows (40): the re-read carves no
  // MARK block (the boot's covers its rows and 8 more), its hold the
  // ranges' block alone (bigger: MARK's rows' ranges); against a control
  // replaying the boot's read (printed), and against bare main within 5.14's
  // one class.
  {
    StatsRun y5c = usual;
    y5c.card = &rows40;
    y5c.uiAt = StatsRun::kBeforeJoin;
    y5c.remount = true;
    y5c.takeHoles = true;
    StatsRun a;
    const std::string before = bad;
    run("Y5c a remount on a card of 40 MARK rows, against the read's carves", y5c, true, true, &a);
    bad = before;  // (printed; held to bare main's class below)
    StatsRun bare = y5c;
    bare.statsOff = true;
    bare.scanFile = true;
    libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &bare);
    const bool inClass = a.again.joined + classOf(bare.again.joined) >= bare.again.joined;
    printf("[hold] Y5c 40 MARK rows (MARK's block %u B, the ranges' block %u B) against bare main after the remount: "
           "joined %u B (bare main's %u B, %+ld B): %s\n",
           (unsigned)a.markBytes, (unsigned)a.rangesBytes, (unsigned)a.again.joined, (unsigned)bare.again.joined,
           (long)a.again.joined - (long)bare.again.joined, inClass ? "within its class" : "UNDER ITS CLASS");
    if (!inClass) bad += std::string(bad.empty() ? "" : "; ") + "Y5c 40 MARK rows: under bare main's class";
  }
  // Y6: a card past 8 MARK rows.
  for (const StatsCard* c : {&rows12, &rows40}) {
    StatsRun y6 = usual;
    y6.card = c;
    y6.uiAt = 0;
    char n6[128];
    std::snprintf(n6, sizeof(n6), "Y6 %u MARK rows (MARK's block kept for the session), against the read's carves",
                  (unsigned)c->marks);
    StatsRun a;
    run(n6, y6, true, false, &a);
    // Against bare main (no stats, no read): 5.14's rule, a class at most.
    StatsRun bare = y6;
    bare.statsOff = true;
    bare.scanFile = true;
    libraryUpdates(Dj::Placed, saved, records, card, 0, MoodRun(), &bare);
    const bool inClass = a.boot.joined + classOf(bare.boot.joined) >= bare.boot.joined;
    printf("[hold] Y6 %u MARK rows (MARK's block %u B) against bare main: joined %u B (bare main's %u B, %+ld B): "
           "%s\n",
           (unsigned)c->marks, (unsigned)a.markBytes, (unsigned)a.boot.joined, (unsigned)bare.boot.joined,
           (long)a.boot.joined - (long)bare.boot.joined, inClass ? "within its class" : "UNDER ITS CLASS");
    if (!inClass) bad += std::string(bad.empty() ? "" : "; ") + n6 + ": under bare main's class";
  }
  StatsRun y7 = fullLog;  // Y7
  y7.bootFails = 1;
  y7.uiAt = StatsRun::kBetweenTries;
  run("Y7 a full log, the first frame between the boot save's tries", y7, false, false);
  if (!bad.empty()) {
    const std::string m = "the hold leaves PSRAM not main's: " + bad;
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

// The overflow's fallback (5.14, fix A, hazard 7): no free block of
// 262 KB for the arena (a heap a library update cut up, mid-session), so
// the chunks are carved one by one, and the fold must fold what fe61db9's
// did. A full log (16 chunks) through the glue on random heaps of that
// kind (the lifecycle review's: a largest free block of 150-255 KB, 1-6
// holes of 4-89 KB under it) and on the review's own (the largest
// 188,416 B, holes of 43, 6, 30, 27 and 55 KB): fe61db9's hooks (the
// spacers keeping 64 KB, no caps, given back at once), the firmware's
// (app/ListeningStats' topAlloc(): the hold's caps given back at the first
// chunk, the chunks carved as fe61db9 carved them), and the fix as first
// built (8cd3708: every take closed, the chunks' too, the ranges' caps held
// through the fallback). Wherever fe61db9's carves 16 chunks and saves, the
// firmware's does, and gives every top block back; the first build's lost
// some (the review's 8 of 600 full logs).
namespace {

void* fe61db9Top(size_t n) {
  autodj::Spacers sp;
  autodj::takeSpacers(n, kMargin, ModelHeap::largest, ModelHeap::freeBytes, ModelHeap::alloc, ModelHeap::release, &sp);
  void* p = ModelHeap::alloc(n);
  autodj::giveSpacers(&sp, ModelHeap::release);
  return p;
}
void* firstBuiltTop(size_t n) {
  autodj::CloseReport r;
  return g_modelTop.take(kModelTop, n, &r);
}

struct FallbackRun {
  uint32_t chunks = 0, wrote = 0;
  bool arena = false, noMemory = false, behind = false;
  int held = -1;
  bool saved() const { return chunks == statsfold::kMaxChunks && !noMemory && !behind && wrote > 0 && held == 0; }
};

FallbackRun foldFullLog(const StatsCard& sc, const std::vector<size_t>& holes, size_t big, void* (*top)(size_t),
                        void (*release)(void*)) {
  namespace sf = statsfold;
  holesThenTop(holes, big);  // (g_modelTop reset with the heap)
  cutfs::CutFs card = sc.card.reboot(cutfs::Variant::InOrder);
  StatsEnv env;
  env.card = &card;
  statsglue::Memory m;
  m.alloc = ModelHeap::alloc;
  m.release = ModelHeap::release;
  m.top = top;
  m.topRelease = release;
  m.keep = ModelHeap::alloc;
  m.keepRelease = ModelHeap::release;
  FallbackRun out;
  {
    statsglue::Glue g(card, env, m);
    const cc::msmf::Acks acks{};
    TEST_ASSERT_TRUE(g.begin(acks, [](uint32_t) {}).read == statsfile::Read::Loaded);
    g.scanBegan(false);
    const uint32_t n = 2 * playlog::kFileSlots, newer = playlog::kFileSlots, seq0 = sc.seq[0];
    uint32_t pieces = 0;
    auto fold = [&](uint32_t i, int f) {
      if (pieces++ % 128 == 0) g.reserve(statsglue::kReserveAhead);
      const uint64_t at = sf::kMinMs + (570 + static_cast<uint64_t>(i) * 29 / n) * sf::kDayMs + (i % 24) * sf::kHourMs;
      g.slot(statsSlot(seq0 + i, i % sc.played, at), f);
    };
    for (uint32_t i = n - newer; i < n; ++i) fold(i, 0);
    for (uint32_t i = 0; i < n - newer; ++i) fold(i, 1);
    out.chunks = g.fold().chunks();
    out.arena = g.fold().inArena();
    out.noMemory = g.fold().noMemory();
    g.save(playlog::Folds::Why::Boot);
    while (g.retrying()) {
      env.mono = g.retryAtMs();
      g.saveNow(false);
    }
    g.ready();
    out.behind = g.fold().behind();
    out.wrote = env.wrote;
    TEST_ASSERT_FALSE(g.scanHolds());
  }
  out.held = g_modelTop.held();
  return out;
}

}  // namespace

void test_the_arena_s_fallback_as_before() {
  const StatsCard sc = statsCard(2214, 1000);
  uint32_t layouts = 0, full = 0, firmwareLost = 0, firstLost = 0;
  std::string lost;
  auto one = [&](const std::vector<size_t>& holes, size_t big, bool say) {
    const FallbackRun o = foldFullLog(sc, holes, big, fe61db9Top, ModelHeap::release);
    const FallbackRun f = foldFullLog(sc, holes, big, topAlloc, topRelease);
    const FallbackRun b = foldFullLog(sc, holes, big, firstBuiltTop, topRelease);
    TEST_ASSERT_FALSE(o.arena || f.arena || b.arena);  // (no room for it: the fallback)
    ++layouts;
    size_t free = big;
    for (size_t h : holes) free += ModelHeap::carved(h);
    if (say)
      printf("[hold] the arena's fallback, the largest %u B, %u holes, %u B free: fe61db9's %u chunks (%s), the "
             "firmware's %u (%s), the first build's %u (%s)\n",
             (unsigned)big, (unsigned)holes.size(), (unsigned)free, (unsigned)o.chunks, o.saved() ? "saved" : "NOT saved",
             (unsigned)f.chunks, f.saved() ? "saved" : "NOT saved", (unsigned)b.chunks, b.saved() ? "saved" : "NOT saved");
    TEST_ASSERT_EQUAL_INT(0, f.held);
    TEST_ASSERT_EQUAL_INT(0, b.held);
    if (!o.saved()) return;
    ++full;
    if (!f.saved()) {
      ++firmwareLost;
      char w[96];
      std::snprintf(w, sizeof(w), "%slargest %u B, %u holes (%u chunks)", lost.empty() ? "" : "; ", (unsigned)big,
                    (unsigned)holes.size(), (unsigned)f.chunks);
      lost += w;
    }
    if (!b.saved()) ++firstLost;
  };
  one({43 * 1024, 6 * 1024, 30 * 1024, 27 * 1024, 55 * 1024}, 188416, true);
  std::mt19937 rng(11);
  for (int t = 0; t < 120; ++t) {
    const size_t big = (150 + rng() % 106) * 1024;
    std::vector<size_t> holes(1 + rng() % 6);
    for (size_t& h : holes) h = (4 + rng() % 86) * 1024;
    one(holes, big, false);
  }
  printf("[hold] the arena's fallback, %u heaps (a full log saved by fe61db9's hooks in %u): the firmware's lost %u, "
         "the first build's %u\n",
         (unsigned)layouts, (unsigned)full, (unsigned)firmwareLost, (unsigned)firstLost);
  TEST_ASSERT_TRUE(full > 0);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, firmwareLost, lost.c_str());
  TEST_ASSERT_TRUE(firstLost > 0);  // (the case sees what the review found)
}

namespace {

// The session's first Opus at its open (OpusGenerator::allocBuffers(): the
// page, the packet and a frame, in that order, kept for the session): the
// first of the three no free block takes (-1: all three carved). Each one
// carved is given back.
int opusShortAt() {
  static const size_t kAsks[3] = {65307, 61440, 1276};
  std::vector<void*> got;
  int at = -1;
  for (int i = 0; i < 3 && at < 0; ++i) {
    if (void* p = ModelHeap::alloc(kAsks[i])) {
      got.push_back(p);
    } else {
      at = i;
    }
  }
  for (void* p : got) ModelHeap::release(p);
  return at;
}

TopPlayer topPlayer(bool playing, bool waiting) {
  TopPlayer s;
  s.playing = playing;
  s.waiting = waiting;
  return s;
}

}  // namespace

// U14 (the final review: the room's trigger, a start waiting). The arena
// asks a decoder's room only while a track plays. A start that waits at
// its take (the headphones not linked yet, a restart asked) can't open a
// decoder while the arena is held: main.cpp's OutputHold holds every start
// that isn't from Playing while overflow chunks are held, PlayGate's
// statsHold keeps it waiting, and the arena and its chunks are given back
// in one call. So the room asked for it could only give the caps back
// (Room) or the arena (NoRoom: the chunks one by one) for nothing. One free
// block of 480 KB: the ranges' block (Closed: the room is there), then the
// arena with a start waiting: kept, its T closed, the caps held, though
// with them held the Opus has no room; once both are given back it opens.
// With a track playing the same take still asks (its next track opens in
// the hold): NoRoom, the ranges' T open again. Through the glue a full log
// folds in the arena with the start waiting.
void test_a_waiting_start_asks_no_room() {
  const size_t big = 480 * 1024;
  for (bool playing : {false, true}) {
    holesThenTop({}, big);
    const Map start = ModelHeap::blocks;
    g_topPlayer = [playing] { return topPlayer(playing, !playing); };
    void* ranges = topAlloc(1284);
    TEST_ASSERT_NOT_NULL(ranges);
    TEST_ASSERT_TRUE(g_topClose == autodj::Close::Closed);
    const size_t rangesAbove = g_topAbove;
    void* arena = topAlloc(statsfold::kArenaBytes);
    const autodj::Close why = g_topClose;
    const int shortAt = opusShortAt();
    printf("[hold] U14 one free block of %u B, %s: the ranges' block closed, the arena %s (%s), %d top blocks held; "
           "the largest free block %u B: the Opus %s\n",
           (unsigned)big, playing ? "a track playing" : "a start waiting", arena ? "kept" : "GIVEN BACK",
           autodj::closeName(why), g_modelTop.held(), (unsigned)ModelHeap::largest(),
           shortAt < 0 ? "opens" : "has no room (yet)");
    if (!playing) {
      TEST_ASSERT_NOT_NULL(arena);
      TEST_ASSERT_TRUE(why == autodj::Close::Closed);  // (no Room, no NoRoom: no room asked)
      TEST_ASSERT_EQUAL_INT(2, g_modelTop.held());
      TEST_ASSERT_EQUAL_size_t(0, freeAboveOf(ranges));  // (the ranges' caps held)
      TEST_ASSERT_TRUE(shortAt >= 0);                    // (the start waits: no decoder opens now)
      // The save's order: the ranges' block given back first (it waits for
      // the arena), then the arena: both freed with their caps at once.
      topRelease(ranges);
      TEST_ASSERT_EQUAL_INT(2, g_modelTop.held());
      topRelease(arena);
      TEST_ASSERT_EQUAL_INT(0, g_modelTop.held());
      TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
      TEST_ASSERT_EQUAL_INT(-1, opusShortAt());  // (the start released: it opens)
      continue;
    }
    TEST_ASSERT_NULL(arena);
    TEST_ASSERT_TRUE(why == autodj::Close::NoRoom);
    TEST_ASSERT_EQUAL_INT(1, g_modelTop.held());
    TEST_ASSERT_EQUAL_size_t(rangesAbove, freeAboveOf(ranges));  // (its T open again)
    TEST_ASSERT_EQUAL_INT(-1, shortAt);
    topRelease(ranges);
    TEST_ASSERT_TRUE(sameMap(start, ModelHeap::blocks));
  }
  // Through the glue (a full log, 16 chunks): the start waiting, the
  // overflow in the arena; a track playing, its chunks one by one. Both
  // fold the whole log and save it.
  const StatsCard sc = statsCard(2214, 1000);
  for (bool playing : {false, true}) {
    g_topPlayer = [playing] { return topPlayer(playing, !playing); };
    const FallbackRun f = foldFullLog(sc, {}, big, topAlloc, topRelease);
    printf("[hold] U14 a full log in one free block of %u B, %s: %u chunks, %s, %s\n", (unsigned)big,
           playing ? "a track playing" : "a start waiting", (unsigned)f.chunks,
           f.arena ? "in the arena" : "one by one (the arena given back for the room)",
           f.saved() ? "saved" : "NOT saved");
    TEST_ASSERT_EQUAL(!playing, f.arena);
    TEST_ASSERT_TRUE(f.saved());
  }
}

// U15 (the final review: the room's trigger, a start in the ranges' hold).
// The ranges' block asks a decoder's room at every take, whatever the
// player. While it alone is held (before the overflow's first chunk: a
// usual log, or a full log's first 40 records) OutputHold holds no start,
// so a Play from a stop or a pause made after the take opens its decoder
// with the block's caps held. (The arena is given back mid-scan only by its
// own NoRoom, which gives the ranges' caps back first; its chunks then hold
// every start.) One free block of 130-320 KB in 64 B steps (PSRAM
// short: a remount's re-read mid-session), the player stopped at the take,
// the ranges' request the model's smallest and largest (1,284 B: no ACKS
// row; 3,076 B: 16), then the session's first Opus: it opens wherever
// fe61db9's hooks left it room, the take giving the caps back (Room) where
// they would leave none. The take without the room (a stopped player's
// before this fix) leaves the Opus short in a band of about 139-201 KB
// where fe61db9's opened it: the page under the block, then the packet's
// 61,440 B (in a few, the frame's 1,276 B) finding no free block. Two
// heaps leave it short with the room too (3,076 B at 133,120 and 133,184
// B): the take leaves T open for the margin (Margin), but fix A's spacers
// put the block 4 KB lower than fe61db9's, and the packet finds no block.
// That is fix A's placement, not the room's; the count is pinned (2 at
// 3,076 B, none at 1,284 B), so a change that widens it fails here.
void test_the_ranges_take_asks_the_room() {
  g_topPlayer = [] { return topPlayer(false, false); };
  for (size_t req : {size_t{1284}, size_t{3076}}) {
    uint32_t heaps = 0, rooms = 0, worse = 0, margins = 0, before = 0;
    uint32_t shortAt[3] = {0, 0, 0};  // (without the room: the page, the packet or the frame the one short)
    size_t roomLo = 0, roomHi = 0, beforeLo = 0, beforeHi = 0;
    std::string worseAt;
    for (size_t top = 130 * 1024; top <= 320 * 1024; top += 64) {
      ++heaps;
      // fe61db9's hooks: the spacers keeping 64 KB, nothing closed.
      holesThenTop({}, top);
      void* o = fe61db9Top(req);
      TEST_ASSERT_NOT_NULL(o);
      const bool oldOpens = opusShortAt() < 0;
      ModelHeap::release(o);
      // The firmware's: the player stopped, the room asked.
      holesThenTop({}, top);
      void* p = topAlloc(req);
      TEST_ASSERT_NOT_NULL(p);
      const autodj::Close why = g_topClose;
      const int fixShort = opusShortAt();
      topRelease(p);
      TEST_ASSERT_EQUAL_INT(0, g_modelTop.held());
      // The same take without the room (94cdf0c's for a stopped player).
      holesThenTop({}, top);
      autodj::TopHold h;
      autodj::CloseReport r;
      void* q = h.take(kModelTop, req, &r, true, 0, false);
      TEST_ASSERT_NOT_NULL(q);
      const int beforeShort = opusShortAt();
      h.give(kModelTop, q);
      if (why == autodj::Close::Room) {
        ++rooms;
        if (!roomLo) roomLo = top;
        roomHi = top;
        TEST_ASSERT_EQUAL_INT(-1, fixShort);  // (all three carved: T open again)
      }
      if (oldOpens && beforeShort >= 0) {
        ++before;
        if (!beforeLo) beforeLo = top;
        beforeHi = top;
        ++shortAt[beforeShort];
      }
      if (oldOpens && fixShort >= 0) {
        // (A take that left T open for the margin: fix A's spacers, not
        // the room; the same without it.)
        if (why == autodj::Close::Margin && beforeShort >= 0) {
          ++margins;
          continue;
        }
        if (++worse > 6) continue;  // (the first six said)
        char w[96];
        std::snprintf(w, sizeof(w), "%s%u B (%s)", worseAt.empty() ? "" : ", ", (unsigned)top, autodj::closeName(why));
        worseAt += w;
      }
    }
    printf("[hold] U15 the ranges' request %u B, %u heaps of one free block (130-320 KB), the player stopped: Room in "
           "%u (%u-%u B), the Opus short where fe61db9's opened %u (a margin's take, T left open, %u); without the "
           "room %u (%u-%u B; the one short the page's 65,307 B in %u, the packet's 61,440 B in %u, the frame's "
           "1,276 B in %u)\n",
           (unsigned)req, (unsigned)heaps, (unsigned)rooms, (unsigned)roomLo, (unsigned)roomHi, (unsigned)worse,
           (unsigned)margins, (unsigned)before, (unsigned)beforeLo, (unsigned)beforeHi, (unsigned)shortAt[0],
           (unsigned)shortAt[1], (unsigned)shortAt[2]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, worse, worseAt.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(req == 3076 ? 2 : 0, margins,
                                     "the Opus short where fe61db9's opened, a margin's take: fix A's placement moved");
    TEST_ASSERT_TRUE(rooms > 0);
    TEST_ASSERT_TRUE(before > 0);  // (the case reaches the band the review found)
  }
}

// U16 (the final review's second round): the room's rule itself,
// statsglue::decoderRoom(), which the firmware's topAlloc() calls as the
// model's does (U14 and U15 run it through the model). Every ranges'
// request asks the Opus's room, whatever the player: the fold's need, a
// multiple of 16 B, and 4 B (the block's alignment), never the arena's
// size or a chunk's. The arena and a chunk ask it only while a track plays
// (the pass's `playing`): a start waiting, or none, asks nothing.
void test_the_room_s_rule() {
  const size_t room = statsglue::kDecoderRoomBytes;
  for (const TopPlayer& s : {topPlayer(true, false), topPlayer(false, true), topPlayer(false, false)}) {
    uint32_t asked = 0, ranges = 0;
    for (size_t need = 16; need <= 512 * 1024; need += 16) {
      ++ranges;
      if (statsglue::decoderRoom(need + 4, s.playing) == room) ++asked;
    }
    const size_t arena = statsglue::decoderRoom(statsfold::kArenaBytes, s.playing);
    const size_t chunk = statsglue::decoderRoom(statsfold::kChunkBytes, s.playing);
    printf("[hold] U16 %s: the ranges' requests (20 B to 512 KB) ask %u B in %u of %u; the arena %u B, a chunk %u B\n",
           s.playing ? "a track playing" : s.waiting ? "a start waiting" : "stopped", (unsigned)room,
           (unsigned)asked, (unsigned)ranges, (unsigned)arena, (unsigned)chunk);
    TEST_ASSERT_EQUAL_UINT32(ranges, asked);
    TEST_ASSERT_EQUAL_size_t(s.playing ? room : 0, arena);
    TEST_ASSERT_EQUAL_size_t(s.playing ? room : 0, chunk);
  }
}

namespace {

// 5.14's yardstick, its histories: the comparison's H1-H4, H5-H8, K1-K4 and
// D, round 2's own sequence on the device (the Opus kept before the first
// update, the MP3 update, the Opus update, the FLAC playing through the
// third, a Dc after each). STATS_WIDE: every combination (the Opus never,
// before update 0, or after update 0-3; a FLAC through none or one of
// updates 0-4; a Dc or not), 72 histories, the named among them.
struct History {
  std::string name;
  const char* set;  // the named set it belongs to ("": none)
  int opusAt, flacStep;
  bool dc;
};

std::vector<History> yardstickHistories() {
  const int never = Updates::kNever;
  const std::vector<History> named = {
      {"H1", "H1-H4", never, -1, false}, {"H2", "H1-H4", -1, 3, true},   {"H3", "H1-H4", 2, -1, true},
      {"H4", "H1-H4", 0, -1, true},      {"H5", "H5-H8", -1, -1, false}, {"H6", "H5-H8", 1, 0, true},
      {"H7", "H5-H8", never, 1, true},   {"H8", "H5-H8", 3, 4, true},    {"K1", "K1-K4", never, 4, true},
      {"K2", "K1-K4", never, 4, false},  {"K3", "K1-K4", -1, 2, false},  {"K4", "K1-K4", -1, 4, true},
      {"D", "D", -1, 2, true},
  };
#ifdef STATS_WIDE
  std::vector<History> wide;
  for (int o : {never, -1, 0, 1, 2, 3})
    for (int f : {-1, 0, 1, 2, 3, 4})
      for (bool dc : {true, false}) {
        History h{"", "", o, f, dc};
        for (const History& n : named)
          if (n.opusAt == o && n.flacStep == f && n.dc == dc) h = n;
        char b[48];
        std::snprintf(b, sizeof(b), "opus %d flac %d dc %d", o, f, dc ? 1 : 0);
        h.name = h.name.empty() ? std::string(b) : h.name + " (" + b + ")";
        wide.push_back(h);
      }
  return wide;
#else
  return named;
#endif
}

// The mood plan's rule (9.5), per update: joined at or over 245,764 B where
// main's is, and within 122,880 B of main's.
constexpr size_t kLine = 245764, kWithin = 122880;
bool stepFails(size_t mainJoined, size_t joined) {
  return (mainJoined >= kLine && joined < kLine) || joined + kWithin < mainJoined;
}

// One kind of run over every history: main's (no stats), main's own
// variation (its queue block longer), or the stats' (a StatsRun).
struct Yard {
  std::string name;
  size_t queueExtra = 0;
  const StatsRun* stats = nullptr;
  bool rework = false;  // the rework as built (the rule applies), not a shape a wait exists for
  bool opusFirst = false;  // (main's) a start at the boot opening the Opus before the boot's join
  std::vector<std::vector<size_t>> joined;  // [history][update]
  std::vector<size_t> bootJoined;
  std::vector<bool> opened;  // [history]: the Opus at the boot opened before the boot's join
  std::vector<StatsRun> runs;  // [history]: what the stats came to
  uint32_t carveFails = 0, joinFails = 0;
  std::vector<std::pair<size_t, int>> fails;  // (history, update) failing the rule
};

// What largest() reports for a free block of `raw` bytes: its class floor
// less the poisoning's 12 B.
size_t reportedOf(size_t raw) {
  if (raw < ModelHeap::kSmall) return raw > ModelHeap::kPoison ? raw - ModelHeap::kPoison : 0;
  return (raw & ~((static_cast<size_t>(1) << (ModelHeap::fls(raw) - ModelHeap::kSlLog2)) - 1)) - ModelHeap::kPoison;
}

// "update 3's the Dc's join", "the boot's the visit".
std::string placeOf(const HeapTrace::Ev& e) {
  char b[80];
  if (e.step < 0) {
    std::snprintf(b, sizeof(b), "the boot, %s", tracePointName(e.point));
  } else {
    std::snprintf(b, sizeof(b), "update %d, %s", e.step, tracePointName(e.point));
  }
  return b;
}

// The yardstick's A4 trace (S3a's V5; printed, not pinned): history `h`
// run with `held` (A4's page) and with its snapshot, the same size, freed
// at its landing (Hv's: nothing held), every carve and free traced
// (HeapTrace), beside the visits alone (`visits`: the library's
// snapshot). At update `step`'s figure: the figure each way; where each
// snapshot held then landed (the free block it came out of); the first
// carve that landed elsewhere than with nothing held (the one a held
// snapshot displaced) and where it went; and the largest free block both
// ways, with what sits, the snapshot held, inside the largest free block
// that nothing held leaves: why it reports a class less.
void traceHeldMove(const char* label, const std::vector<uint8_t>& saved, cc::Source& records,
                   const synthcard::Card& card, const StatsRun& visits, const StatsRun& held, const History& h,
                   int step) {
  HeapTrace tr[2];
  Updates up[3];
  for (int k = 0; k < 3; ++k) {
    up[k].opusAt = h.opusAt;
    up[k].flacStep = h.flacStep;
    up[k].dc = h.dc;
    StatsRun sr = k == 1 ? held : visits;
    if (k == 0) sr.page.bytes = held.page.bytes;  // (carve None: freed at its landing)
    ModelHeap::trace = k < 2 ? &tr[k] : nullptr;
    libraryUpdates(Dj::Placed, saved, records, card, 5, MoodRun(), &sr, &up[k]);
    ModelHeap::trace = nullptr;
    if (k < 2) TEST_ASSERT_EQUAL_size_t(5, tr[k].figures.size());
  }
  const size_t s = static_cast<size_t>(step);
  const size_t j0 = up[0].joined[s], j1 = up[1].joined[s];
  printf("[stats-yard] A4's move traced | %s, %s, update %d's figure: %u B held, %u B freed at its landing (%+lld B), "
         "the visits alone's %u B%s\n",
         label, h.name.c_str(), step, (unsigned)j1, (unsigned)j0,
         static_cast<long long>(j1) - static_cast<long long>(j0), (unsigned)up[2].joined[s],
         j0 == j1 ? ": not moved in this build" : "");
  if (j0 == j1) return;
  const size_t end1 = tr[1].figureAt[s];
  // The snapshots carved before the figure (held: freed after it, or never).
  for (size_t e = 0; e < end1; ++e) {
    const HeapTrace::Ev& c = tr[1].ev[e];
    if (!c.snap || !c.n) continue;
    size_t freedAt = SIZE_MAX;
    for (size_t f = e + 1; f < tr[1].ev.size() && freedAt == SIZE_MAX; ++f)
      if (tr[1].ev[f].snap && !tr[1].ev[f].n && tr[1].ev[f].at == c.at) freedAt = f;
    if (c.step < step - 1) continue;  // (the two last visits')
    printf("[stats-yard] A4's move traced |   the snapshot carved at %s: a %u B block at %u, out of a free block of "
           "%u B (its rest %u B left free), %s\n",
           placeOf(c).c_str(), (unsigned)c.block, (unsigned)c.at, (unsigned)c.from,
           (unsigned)(c.from > c.block ? c.from - c.block - ModelHeap::kHeader : 0),
           freedAt == SIZE_MAX ? "held to the run's end"
           : freedAt < end1    ? ("freed before the figure, at " + placeOf(tr[1].ev[freedAt])).c_str()
                               : ("held at the figure, freed at " + placeOf(tr[1].ev[freedAt])).c_str());
  }
  // The carves that landed elsewhere (the snapshot's own left out), the
  // two runs' events side by side while they ask the same: the first, the
  // first that outlived its call (given back at a later point, or never:
  // a decoder's, a join's, the Live's), and the first still held at the
  // figure. Where the runs first ask otherwise (a spacer more or less, a
  // smaller one), the decisive point: each run's free and largest free
  // block there, and its next requests.
  std::vector<bool> atFigure(tr[1].ev.size(), false);
  for (const HeapTrace::Block& x : tr[1].figures[s])
    if (x.used && x.carve != SIZE_MAX) atFigure[x.carve] = true;
  std::vector<size_t> freedAt(tr[1].ev.size(), SIZE_MAX);  // a carve's free (tr[1]'s index)
  {
    std::map<size_t, size_t> open;
    for (size_t e = 0; e < tr[1].ev.size(); ++e) {
      const HeapTrace::Ev& d = tr[1].ev[e];
      if (d.n) {
        open[d.at] = e;
        continue;
      }
      const auto it = open.find(d.at);
      if (it == open.end()) continue;
      freedAt[it->second] = e;
      open.erase(it);
    }
  }
  auto outlives = [&](size_t e) {
    const size_t f = freedAt[e];
    return f == SIZE_MAX || tr[1].ev[f].step != tr[1].ev[e].step || tr[1].ev[f].point != tr[1].ev[e].point;
  };
  size_t a = 0, b = 0, alike = 0, elsewhere = 0;
  size_t first[2] = {SIZE_MAX, SIZE_MAX}, kept[2] = {SIZE_MAX, SIZE_MAX}, asks[2] = {SIZE_MAX, SIZE_MAX},
         lasting[2] = {SIZE_MAX, SIZE_MAX};
  auto past = [](const HeapTrace& t, size_t* i) {
    while (*i < t.ev.size() && t.ev[*i].snap) ++*i;
  };
  for (;;) {
    past(tr[0], &a);
    past(tr[1], &b);
    if (a >= tr[0].figureAt[s] || b >= tr[1].figureAt[s]) break;
    const HeapTrace::Ev& e0 = tr[0].ev[a];
    const HeapTrace::Ev& e1 = tr[1].ev[b];
    if (e0.n != e1.n) {
      asks[0] = a;
      asks[1] = b;
      break;
    }
    if (e0.n && e0.at != e1.at) {
      ++elsewhere;
      if (first[0] == SIZE_MAX) {
        first[0] = a;
        first[1] = b;
      }
      if (kept[0] == SIZE_MAX && atFigure[b]) {
        kept[0] = a;
        kept[1] = b;
      }
      if (lasting[0] == SIZE_MAX && outlives(b)) {
        lasting[0] = a;
        lasting[1] = b;
      }
    } else if (first[0] == SIZE_MAX) {
      ++alike;
    }
    ++a;
    ++b;
  }
  // The snapshot held at tr[1]'s event `at` (its carve before it, its free
  // after it): the free block it came out of, [at, at + from + header).
  auto holeAt = [&](size_t at, size_t* lo, size_t* hi) {
    size_t sAt = SIZE_MAX, sFrom = 0;
    for (size_t e = 0; e < at; ++e) {
      const HeapTrace::Ev& c = tr[1].ev[e];
      if (!c.snap) continue;
      if (c.n) {
        sAt = c.at;
        sFrom = c.from;
      } else if (c.at == sAt) {
        sAt = SIZE_MAX;
      }
    }
    *lo = sAt;
    *hi = sAt == SIZE_MAX ? 0 : sAt + sFrom + ModelHeap::kHeader;
  };
  auto said = [&](const char* what, const size_t at[2]) {
    const HeapTrace::Ev& e0 = tr[0].ev[at[0]];
    const HeapTrace::Ev& e1 = tr[1].ev[at[1]];
    size_t lo = 0, hi = 0;
    holeAt(at[1], &lo, &hi);
    const bool inHole = e0.at >= lo && e0.at < hi;
    const size_t f = freedAt[at[1]];
    const std::string until = atFigure[at[1]] ? std::string("held at the figure")
                              : f == SIZE_MAX ? std::string("never given back")
                              : !outlives(at[1]) ? std::string("given back in its call")
                                                 : "given back at " + placeOf(tr[1].ev[f]);
    printf("[stats-yard] A4's move traced |   %s: %s, a request of %u B (a %u B block): with nothing held at %u, out "
           "of a free block of %u B%s; with the snapshot held at %u, out of a free block of %u B (%s)\n",
           what, placeOf(e1).c_str(), (unsigned)e0.n, (unsigned)e0.block, (unsigned)e0.at, (unsigned)e0.from,
           inHole ? " (the one the held snapshot came out of)" : "", (unsigned)e1.at, (unsigned)e1.from,
           until.c_str());
  };
  printf("[stats-yard] A4's move traced |   %u carves and frees alike, then %u carves that landed elsewhere%s%s\n",
         (unsigned)alike, (unsigned)elsewhere, asks[0] == SIZE_MAX ? " before the figure" : ", up to ",
         asks[0] == SIZE_MAX ? "" : placeOf(tr[1].ev[asks[1]]).c_str());
  if (first[0] != SIZE_MAX) said("the first carve that landed elsewhere", first);
  if (lasting[0] != SIZE_MAX && lasting[0] != first[0]) said("the first of them that outlived its call", lasting);
  if (kept[0] != SIZE_MAX && kept[0] != first[0] && kept[0] != lasting[0])
    said("the first of them held at the figure", kept);
  if (asks[0] != SIZE_MAX) {
    std::string next[2];
    for (int k = 0; k < 2; ++k) {
      uint32_t c = 0;
      for (size_t e = asks[k]; e < tr[k].ev.size() && c < 4; ++e) {
        const HeapTrace::Ev& d = tr[k].ev[e];
        if (d.snap || !d.n) continue;
        next[k] += (c++ ? ", " : "") + std::to_string(d.n);
      }
    }
    const HeapTrace::Ev& e0 = tr[0].ev[asks[0]];
    const HeapTrace::Ev& e1 = tr[1].ev[asks[1]];
    printf("[stats-yard] A4's move traced |   the runs first ask otherwise at %s: with nothing held (free %u B, the "
           "largest free block %u B) its requests %s B; with the snapshot held (free %u B, %+lld B; the largest %u "
           "B) %s B\n",
           placeOf(e1).c_str(), (unsigned)e0.free, (unsigned)e0.largest, next[0].c_str(), (unsigned)e1.free,
           static_cast<long long>(e1.free) - static_cast<long long>(e0.free), (unsigned)e1.largest,
           next[1].c_str());
  }
  // The largest free block at the figure, both ways.
  HeapTrace::Block big[2];
  for (int k = 0; k < 2; ++k)
    for (const HeapTrace::Block& x : tr[k].figures[s])
      if (!x.used && x.size > big[k].size) big[k] = x;
  printf("[stats-yard] A4's move traced |   at the figure the largest free block: with nothing held %u B at %u "
         "(reported %u B: its class from %u B); with the snapshot held %u B at %u (reported %u B)\n",
         (unsigned)big[0].size, (unsigned)big[0].at, (unsigned)reportedOf(big[0].size),
         (unsigned)(reportedOf(big[0].size) + ModelHeap::kPoison), (unsigned)big[1].size, (unsigned)big[1].at,
         (unsigned)reportedOf(big[1].size));
  // What sits there, the snapshot held: the blocks in use inside the
  // largest free block that nothing held leaves, each with its carve's
  // place and where the same carve (that place's same request, as many
  // before it) is with nothing held.
  auto twinOf = [&](size_t idx) {
    const HeapTrace::Ev& c = tr[1].ev[idx];
    uint32_t nth = 0;
    for (size_t e = 0; e < idx; ++e) {
      const HeapTrace::Ev& d = tr[1].ev[e];
      if (!d.snap && d.n == c.n && d.step == c.step && d.point == c.point) ++nth;
    }
    for (size_t e = 0; e < tr[0].ev.size(); ++e) {
      const HeapTrace::Ev& d = tr[0].ev[e];
      if (d.snap || d.n != c.n || d.step != c.step || d.point != c.point) continue;
      if (nth == 0) return e;
      --nth;
    }
    return SIZE_MAX;
  };
  const size_t lo = big[0].at, hi = big[0].at + big[0].size + ModelHeap::kHeader;
  std::string in;
  uint32_t n = 0;
  size_t bytes = 0;
  for (const HeapTrace::Block& x : tr[1].figures[s]) {
    if (!x.used || x.at + x.size + ModelHeap::kHeader <= lo || x.at >= hi) continue;
    ++n;
    bytes += x.size + ModelHeap::kHeader;
    if (n > 4) continue;
    char t[240];
    if (x.carve == SIZE_MAX) {
      std::snprintf(t, sizeof(t), "%s%u B at %u", in.empty() ? "" : "; ", (unsigned)x.size, (unsigned)x.at);
    } else {
      const HeapTrace::Ev& c = tr[1].ev[x.carve];
      const size_t w = c.snap ? SIZE_MAX : twinOf(x.carve);
      char was[80] = "";
      if (w != SIZE_MAX) {
        bool there = false;
        for (const HeapTrace::Block& y : tr[0].figures[s]) there = there || (y.used && y.carve == w);
        std::snprintf(was, sizeof(was), "; with nothing held at %u%s", (unsigned)tr[0].ev[w].at,
                      there ? "" : ", given back by the figure");
      }
      std::snprintf(t, sizeof(t), "%s%u B at %u (a request of %u B at %s%s%s)", in.empty() ? "" : "; ",
                    (unsigned)x.size, (unsigned)x.at, (unsigned)c.n, placeOf(c).c_str(),
                    c.snap ? ", the snapshot's" : "", was);
    }
    in += t;
  }
  printf("[stats-yard] A4's move traced |   in that block with the snapshot held: %u block%s in use (%u B with "
         "headers)%s%s%s\n",
         (unsigned)n, n == 1 ? "" : "s", (unsigned)bytes, n ? ": " : "", in.c_str(), n > 4 ? "; ..." : "");
}

}  // namespace

// 5.14's yardstick (R1 of its build plan; R6 made it the rework's): the
// multi-update model. The 19,412-track library, 5 update steps, each
// history's decoders as the device asks for their blocks
// (STATS_OLD_DECODERS: as the design was first drawn), a Dc after each step
// where the history has one, no memory check's give-back; joined per step
// against main's run (the stats off) in the same history, the mood plan's
// rule at each step, and the boot's join equal to main's (to main's with
// the Opus opening before it, where a start at the boot opened it before
// the boot's join: the order's own cost). Beside them: main with its queue
// block 2,048 B longer (main's own variation, the control), and main with
// a start at the boot before its join; the rework as built (the firmware's
// glue, StatsRun, on a card of P = 2,214 played tracks). With the scan
// first (the boot's join waiting for its blocks), a start at the boot is
// asked during the scan: it opens the Opus under the scan's blocks, or,
// held while a full log's overflow chunks are, at the pass after the boot's
// save, before that pass begins the boot's join (main.cpp's stepPlayGate
// before stepAutoDj): either way before the boot's join, so its reference
// is main's with the Opus first. The scan first: a usual boot (30 records:
// the last session's; no chunk, so nothing holds its start), the same with
// the root's ACKS 16 rows above the card's MARK (the ranges' block at its
// largest, 3,076 B), a records' save at 40 in update 0's fence before or
// after its build, a full log never synced (16 overflow chunks, a start
// waiting), the same with ACKS's 16 rows, its boot's save failing all four
// tries (behind). The join
// first, or the scan after update 0 (asked before its first piece): a usual
// boot, a full log. And the rework without one of its waits at a time, the
// shapes they exist for: a full log's overflow held through update 0's
// build, or carved in its fence, which must break the rule; the Opus
// opening under a full log's overflow, the scan first, after its last
// piece, its first, its 50th, and after its last with ACKS's 16 rows: the
// start's wait, reported. The suite's build (the device's decoders, the
// named histories): every figure pinned, the control failing no step, nor
// the rework. The variants (STATS_OLD_DECODERS, STATS_WIDE; this case
// alone, outside the suite: PLATFORMIO_BUILD_FLAGS and a build folder of
// their own): every step the rework fails must be one the control fails
// too; their figures go in the commit message. All three pass since the
// resident set left PSRAM (the plan's 5.14, "Moved to internal RAM"); with
// the 3,584 B block, STATS_OLD_DECODERS had a usual boot's three ways with
// the scan first one class under the boot's join (450,548 B against
// 466,932 B), the rework review's 4 and 11.
// S3a's V5 (docs/LISTENING-STATS.md 9.7) adds its ways here, so the wide
// build runs them (this case alone is registered there): on a usual boot,
// the scan first, a visit after the boot's join and after each update's
// Live and rejoin (Hv: nothing held), and A4, the snapshot held as a page
// holds it at 512, 3,860 and 4,096 B (carved before each visit's take, held
// through the history's FLAC end, its Dc, the records' saves and the next
// decoder's opens, freed at the next fence). Each scored by stepFails() and
// the rule above (none may fail a step the control doesn't: the 13 named
// histories' 65 steps, STATS_WIDE's 360), with a score line of its own
// ("A4's score"). The model's histories have no memory check (run L had
// none), so alsoFreed's count of the snapshot at the fence isn't modelled:
// it is freed there, after Auto DJ's join, before the old index.
// V5's 3a-4 fallback (9.7, "If 3a-4 fails"): A4 at 3,860 and 4,096 B with
// the snapshot freed besides before each long-lived carve made while it is
// held (a decoder's open, the FLAC's or the Opus's; a join's begin, the Dc)
// and not carved again until the next visit: every figure the visits
// alone's to the byte in every build (run 3a-4's rule), with the fallback's
// frees counted per history (how often a page would lose its lists) and the
// model's points each snapshot lives through ("3a-4's fallback"). Each held
// way's figures against the visits alone ("moved"). A placement for
// comparison, printed, not the rule's: the snapshot at the top through the
// hold, as the grades are, held as A4 holds it ("the top"). And A4's moves
// traced through the model heap ("A4's move traced": where the snapshot
// landed, the first carve that landed elsewhere, what sits in the largest
// free block): the 3,860 B way at D's update 4 and at its worst in the wide
// build, and the 4,096 B way at its worst there.
void test_the_listening_stats_across_updates() {
  const uint32_t tracks = 19412;
  const synthcard::Card card = synthcard::make(synth::userShape(tracks));
  synthcard::Write dw;
  dw.source = cc::mptg::kSourceDevice;
  const std::vector<uint8_t> d = synthcard::tagsFile(card, dw);
  cc::MemSource records(d.data(), static_cast<uint32_t>(d.size()));
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
    MemorySink sink;
    LibraryIndex::Inputs in;
    in.cardId = 1;
    TEST_ASSERT_TRUE(idx.save(sink, in));
    saved.assign(sink.data(), sink.data() + sink.size());
  }

  // The decoders' requests against the device's own figures.
  size_t opusAsked = 0;
  for (size_t n : kOpusAsks) opusAsked += n;
  ModelHeap::reset(1024 * 1024);
  const size_t before = ModelHeap::freeBytes();
  std::vector<void*> flac;
  for (size_t n : kFlacAsks) flac.push_back(ModelHeap::alloc(n));
  const size_t flacTaken = before - ModelHeap::freeBytes();
  for (void* p : flac) ModelHeap::release(p);
  printf("[stats-yard] the decoders (%s): the Opus asks %u B in %u blocks, a FLAC takes %u B of free PSRAM in %u\n",
#ifdef STATS_OLD_DECODERS
         "the old shape",
#else
         "the device's",
#endif
         (unsigned)opusAsked, (unsigned)kOpusAsks.size(), (unsigned)flacTaken, (unsigned)kFlacAsks.size());
#ifndef STATS_OLD_DECODERS
  TEST_ASSERT_EQUAL_size_t(128023, opusAsked);  // "[opus] buffers: 128023 B"
  // The control's FLAC (mainrun.log: 511,460 B free after qx, 429,340 B
  // while it played): 82,120 B, within the private struct's class step
  // (about 6 KB: 128 B) of the model's.
  TEST_ASSERT_EQUAL_size_t(82072, flacTaken);
  TEST_ASSERT_TRUE(flacTaken <= 82120 && 82120 - flacTaken < 128);
#endif

  // The kinds of run.
  const StatsCard sc = statsCard(2214, 1000);
  using Scan = StatsRun::Scan;
  const uint32_t full = 2 * playlog::kFileSlots;
  StatsRun usual;
  usual.card = &sc;
  StatsRun usualAcks = usual, usualJoin = usual, fenceBefore = usual, fenceAfter = usual;
  usualAcks.ackRows = cc::msmf::Acks::kMax;
  usualJoin.when = Scan::JoinFirst;
  fenceBefore.fenceSave = 1;
  fenceAfter.fenceSave = 2;
  StatsRun fullLog = usual;
  fullLog.slots = full;
  StatsRun fullJoin = fullLog, fullFails = fullLog, fullAcks = fullLog, fullAfter = fullLog, through = fullLog,
           inFence = fullLog, under = fullLog;
  fullAcks.ackRows = cc::msmf::Acks::kMax;
  fullJoin.when = Scan::JoinFirst;
  fullFails.bootFails = static_cast<int>(statsglue::kBootTries);
  fullAfter.when = Scan::AfterUpdate;
  through.when = Scan::ThroughBuild;
  inFence.when = Scan::InFence;
  under.startWaits = false;
  StatsRun underFirst = under, underMid = under, underAcks = under;
  underFirst.startAt = 1;
  underMid.startAt = 50;
  underAcks.ackRows = cc::msmf::Acks::kMax;
  // S3a's V5 (docs/LISTENING-STATS.md 9.7): a visit (Full, then Lists) after
  // the boot's join and after each update's Live and rejoin, before the
  // history's FLAC ends, its Dc and the plays: with nothing held (Hv: its
  // snapshot freed at its landing), and A4, the snapshot held as a page
  // holds it (Hu's: carved plain before the visit's take, held through the
  // step, freed at the next fence) at 512, 3,860 (the spare's) and 4,096 B
  // (the bound).
  StatsRun visits = usual;
  visits.ask = StatsRun::Ask::Visit;
  StatsRun a4Small = visits, a4 = visits, a4Bound = visits;
  a4Small.page = {512, StatsRun::Carve::Plain};
  a4.page = {3860, StatsRun::Carve::Plain};
  a4Bound.page = {4096, StatsRun::Carve::Plain};
  // 9.7's fallback ("If 3a-4 fails"): A4's snapshot freed besides before
  // each decoder's open and each join's begin made while it is held, at
  // 3,860 and 4,096 B. And a placement for comparison (not the firmware's):
  // the snapshot carved at the top through the hold (statshold::take(), as
  // the grades are: under the spacers, its caps closing the free block
  // above it, held with it), held as A4 holds it.
  StatsRun fallback = a4, fallbackBound = a4Bound, onTop = a4, onTopBound = a4Bound;
  fallback.page.fallback = fallbackBound.page.fallback = true;
  onTop.page.carve = onTopBound.page.carve = StatsRun::Carve::Top;
  std::vector<Yard> yards = {
      {"main"},
      {"main's queue 2,048 B longer", 2048},
      {"main, a start at the boot opening the Opus before its join", 0, nullptr, false, true},
      // The scan first: a start at the boot opens the Opus before the boot's join.
      {"the rework: a usual boot (30 records), the scan first", 0, &usual, true},
      {"the rework: a usual boot, the scan first, the root's ACKS 16 rows above MARK (the ranges' block 3,076 B)", 0,
       &usualAcks, true},
      {"the rework: a records' save at 40 in update 0's fence, before its build", 0, &fenceBefore, true},
      {"the rework: a records' save at 40 in update 0's fence, after its build", 0, &fenceAfter, true},
      {"the rework: a full log (16 overflow chunks), the scan first, a start waiting", 0, &fullLog, true},
      {"the rework: a full log, its boot's save failing all four tries (behind)", 0, &fullFails, true},
      {"the rework: a full log, the scan first, a start waiting, the root's ACKS 16 rows above MARK", 0, &fullAcks,
       true},
      // The join first, or the scan after update 0: the Opus after the boot's join.
      {"the rework: a usual boot, the join first", 0, &usualJoin, true},
      {"the rework: a full log, the join first, a start waiting", 0, &fullJoin, true},
      {"the rework: a full log, the scan after update 0 (asked before its first piece)", 0, &fullAfter, true},
      // The shapes the waits exist for.
      {"no update waiting for the scan: a full log's overflow held through update 0's build", 0, &through},
      {"no scan waiting for the update: a full log's overflow carved in update 0's fence", 0, &inFence},
      {"no start waiting: the Opus opening under a full log's overflow after the scan's last piece", 0, &under},
      {"no start waiting: the Opus opening after a full log's first piece (the later chunks around it)", 0,
       &underFirst},
      {"no start waiting: the Opus opening after a full log's 50th piece", 0, &underMid},
      {"no start waiting: the Opus opening under a full log's overflow, the root's ACKS 16 rows above MARK", 0,
       &underAcks},
      // S3a's V5: the stats pages' visit (a usual boot, the scan first).
      {"S3a: a visit after the boot's join and each update's rejoin, nothing held (Hv)", 0, &visits, true},
      {"A4: the snapshot held, 512 B plain, carved before each visit's take, freed at the next fence", 0, &a4Small,
       true},
      {"A4: the snapshot held, 3,860 B plain, carved before each visit's take, freed at the next fence", 0, &a4,
       true},
      {"A4: the snapshot held, 4,096 B plain, carved before each visit's take, freed at the next fence", 0, &a4Bound,
       true},
      // 9.7's fallback, then the top for comparison (printed, not the rule's).
      {"3a-4's fallback: the snapshot held, 3,860 B plain, freed before each decoder's open and join's begin", 0,
       &fallback, true},
      {"3a-4's fallback: the snapshot held, 4,096 B plain, freed before each decoder's open and join's begin", 0,
       &fallbackBound, true},
      {"for comparison: the snapshot held, 3,860 B at the top through the hold, freed at the next fence", 0, &onTop},
      {"for comparison: the snapshot held, 4,096 B at the top through the hold, freed at the next fence", 0,
       &onTopBound},
  };
  enum { kMain, kControl, kMainFirst, kRework, kReworkAcks };
  const size_t kFenceBefore = 5, kFenceAfter = 6, kFullLog = 7, kFullFails = 8, kFullAcks = 9, kReworkJoin = 10,
               kFullJoin = 11, kFullAfter = 12, kThrough = 13, kFence = 14, kUnder = 15, kUnderFirst = 16,
               kUnderMid = 17, kUnderAcks = 18, kVisits = 19, kA4Small = 20, kA4 = 21, kA4Bound = 22,
               kFallback = 23, kFallbackBound = 24, kTop = 25, kTopBound = 26;
  TEST_ASSERT_EQUAL_size_t(27, yards.size());

  const std::vector<History> hs = yardstickHistories();
  const auto t0 = std::chrono::steady_clock::now();
  for (Yard& y : yards) {
    for (const History& h : hs) {
      Updates up;
      up.opusAt = h.opusAt;
      up.flacStep = h.flacStep;
      up.dc = h.dc;
      up.queueExtra = y.queueExtra;
      up.opusFirst = y.opusFirst;
      StatsRun sr = y.stats ? *y.stats : StatsRun();
      const UpdateRun u = libraryUpdates(Dj::Placed, saved, records, card, 5, MoodRun(), y.stats ? &sr : nullptr, &up);
      TEST_ASSERT_EQUAL_size_t(5, up.joined.size());
      y.joined.push_back(up.joined);
      y.bootJoined.push_back(u.bootJoined);
      y.opened.push_back(u.opusFirst);
      y.runs.push_back(sr);
      y.carveFails += up.carveFails;
      y.joinFails += up.joinFails;
    }
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  // What each kind came to against main: per history, per named set, in all.
  struct Score {
    long long sum = 0, worst = 0;
    int n = 0, fails = 0;
    std::string where;
  };
  const Yard& mainRun = yards[kMain];
  for (size_t yi = 1; yi < yards.size(); ++yi) {
    Yard& y = yards[yi];
    std::map<std::string, Score> sets;
    Score all;
    for (size_t i = 0; i < hs.size(); ++i) {
      printf("[stats-yard] %s | %s: boot %u/%u", hs[i].name.c_str(), y.name.c_str(), (unsigned)y.bootJoined[i],
             (unsigned)mainRun.bootJoined[i]);
      for (size_t k = 0; k < 5; ++k) {
        const size_t m = mainRun.joined[i][k], j = y.joined[i][k];
        const long long dlt = static_cast<long long>(j) - static_cast<long long>(m);
        const bool f = stepFails(m, j);
        printf(" %u: %u/%u (%+lld)%s", (unsigned)k, (unsigned)j, (unsigned)m, dlt, f ? " FAILS" : "");
        for (Score* sc2 : {&all, hs[i].set[0] ? &sets[hs[i].set] : nullptr}) {
          if (!sc2) continue;
          sc2->sum += dlt;
          sc2->worst = std::min(sc2->worst, dlt);
          ++sc2->n;
          if (f) {
            ++sc2->fails;
            char b[64];
            std::snprintf(b, sizeof(b), "%s%s %u (%+lld B)", sc2->where.empty() ? "" : ", ", hs[i].name.c_str(),
                          (unsigned)k, dlt);
            sc2->where += b;
          }
        }
        if (f) y.fails.emplace_back(i, static_cast<int>(k));
      }
      if (y.stats) {
        const StatsRun& r = y.runs[i];
        printf(" | ranges %u B, %u chunks, %u tries%s, %u of %u saves landed (%u compacting)",
               (unsigned)r.rangesBytes, (unsigned)r.chunks, (unsigned)r.tries, r.behind ? " (behind)" : "",
               (unsigned)r.wrote, (unsigned)r.saves, (unsigned)r.compacted);
        if (!r.turns.empty()) {
          uint32_t out = 0;
          for (const StatsRun::Turn& t : r.turns) out += t.outOfL ? 1 : 0;
          printf(" | the visits: %s; the snapshot %s, out of the largest free block %u of %u; traceless: %s",
                 tookOf(r).c_str(),
                 r.page.carve == StatsRun::Carve::None ? "freed at each landing"
                 : r.page.fallback                     ? "held to the next fence or long-lived carve"
                                                       : "held to the next fence",
                 (unsigned)out, (unsigned)r.turns.size(), r.askTraceless ? "yes" : "NO");
          if (r.page.carve != StatsRun::Carve::None)
            printf("; held through %u of the model's points, at %u of its figures", (unsigned)r.heldPoints,
                   (unsigned)r.heldAtFigure);
          if (r.page.fallback)
            printf("; let go before a FLAC's open %u, an Opus's open %u, a join's begin %u", (unsigned)r.letGo[0],
                   (unsigned)r.letGo[1], (unsigned)r.letGo[2]);
          for (const auto& w : r.topWhy) printf("; its take %s %u", w.first.c_str(), (unsigned)w.second);
        }
      }
      printf("\n");
    }
    for (const char* set : {"H1-H4", "H5-H8", "K1-K4", "D"})
      printf("[stats-yard] %s | %s: mean %+.0f B, worst %+lld B, %d of %d failing%s%s\n", set, y.name.c_str(),
             sets[set].n ? static_cast<double>(sets[set].sum) / sets[set].n : 0.0, sets[set].worst, sets[set].fails,
             sets[set].n, sets[set].where.empty() ? "" : ": ", sets[set].where.c_str());
    printf("[stats-yard] all %u histories | %s: mean %+.0f B, worst %+lld B, %d of %d failing; the boot's join %u B "
           "(main's %u B); decoder blocks without room %u, joins without room %u\n",
           (unsigned)hs.size(), y.name.c_str(), all.n ? static_cast<double>(all.sum) / all.n : 0.0, all.worst,
           all.fails, all.n, (unsigned)y.bootJoined[0], (unsigned)mainRun.bootJoined[0], (unsigned)y.carveFails,
           (unsigned)y.joinFails);
  }
  printf("[stats-yard] %u histories, %u kinds of run, 5 updates each: %.1f s\n", (unsigned)hs.size(),
         (unsigned)yards.size(), secs);

  // Main's, the control's and the rework's runs: the decoders' blocks and
  // every join placed (a shape the waits exist for may leave a decoder no
  // room: with the old shapes a full log's overflow held through the build
  // does); the stats' saves each landed (but a boot's whose tries all fail:
  // behind, none after), their scan's blocks the same in every history.
  const Yard& control = yards[kControl];
  for (const Yard& y : yards) {
    if (y.stats && !y.rework) continue;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, y.carveFails, y.name.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, y.joinFails, y.name.c_str());
    if (!y.stats) continue;
    for (size_t i = 0; i < hs.size(); ++i) {
      const StatsRun& r = y.runs[i];
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.reserveFailed, y.name.c_str());
      TEST_ASSERT_EQUAL_size_t_MESSAGE(y.runs[0].rangesBytes, r.rangesBytes, y.name.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(y.runs[0].chunks, r.chunks, y.name.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(y.runs[0].saves, r.saves, y.name.c_str());
      if (y.stats->bootFails) {
        TEST_ASSERT_TRUE_MESSAGE(r.behind, y.name.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(statsglue::kBootTries, r.tries, y.name.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.wrote, y.name.c_str());
      } else {
        TEST_ASSERT_FALSE_MESSAGE(r.behind, y.name.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.tries, y.name.c_str());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.saves, r.wrote, y.name.c_str());
      }
    }
  }
  // The rule (5.14): the boot's join main's in every history (main's with
  // the Opus opening before it where a start at the boot opened it before
  // the boot's join: the order's cost, not the stats'), and every step the
  // stats fail one the control fails too (none, in the suite's build). What
  // breaks it, for each kind of run.
  const Yard& mainFirst = yards[kMainFirst];
  auto breaks = [&](const Yard& y) {
    std::string b;
    for (size_t i = 0; i < hs.size(); ++i) {
      const Yard& ref = y.opened[i] ? mainFirst : mainRun;
      if (y.bootJoined[i] == ref.bootJoined[i]) continue;
      char t[200];
      std::snprintf(t, sizeof(t), "%s%s's boot's join %u B (%s %u B)", b.empty() ? "" : ", ", hs[i].name.c_str(),
                    (unsigned)y.bootJoined[i], y.opened[i] ? "main's with the Opus first" : "main's",
                    (unsigned)ref.bootJoined[i]);
      b += t;
    }
    for (const auto& f : y.fails) {
      if (std::find(control.fails.begin(), control.fails.end(), f) != control.fails.end()) continue;
      const size_t m = mainRun.joined[f.first][f.second], j = y.joined[f.first][f.second];
      char t[160];
      std::snprintf(t, sizeof(t), "%s%s's step %d %u B (main's %u B, %+lld B)", b.empty() ? "" : ", ",
                    hs[f.first].name.c_str(), f.second, (unsigned)j, (unsigned)m,
                    static_cast<long long>(j) - static_cast<long long>(m));
      b += t;
    }
    return b;
  };
  std::string broken;
  for (size_t yi = 1; yi < yards.size(); ++yi) {
    const Yard& y = yards[yi];
    const std::string b = breaks(y);
    printf("[stats-yard] the rule | %s: %s%s\n", y.name.c_str(), b.empty() ? "holds" : "broken: ", b.c_str());
    if (y.rework && !b.empty()) broken += (broken.empty() ? std::string() : std::string("; ")) + y.name + ": " + b;
  }
  for (size_t i = 0; i < hs.size(); ++i) TEST_ASSERT_EQUAL_size_t(mainRun.bootJoined[i], control.bootJoined[i]);
  // A4 (9.7's V5; call 1's condition, a pre-check: the device decides, to
  // the byte, run 3a-4): the held snapshot scored by stepFails() over these
  // histories (the suite's 13 named, 65 steps; STATS_WIDE's 72, 360), each
  // way failing no step the control (main's queue 2,048 B longer) doesn't
  // (the rule above, which fails the case); the visits alone the same. Each
  // run: a visit after the boot's join and after each of the 5 updates, no
  // ask leaving a trace, and every snapshot an A4 way kept freed at a fence
  // or the run's end (none held by Hv).
  for (const size_t yi : {kVisits, kA4Small, kA4, kA4Bound}) {
    const Yard& y = yards[yi];
    uint32_t notControl = 0, taken = 0, turns = 0, out = 0;
    for (const auto& f : y.fails)
      if (std::find(control.fails.begin(), control.fails.end(), f) == control.fails.end()) ++notControl;
    for (size_t i = 0; i < hs.size(); ++i) {
      const StatsRun& r = y.runs[i];
      TEST_ASSERT_EQUAL_size_t_MESSAGE(6, r.turns.size(), y.name.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.askTraceless, y.name.c_str());
      uint32_t kept = 0;
      for (const StatsRun::Turn& t : r.turns) {
        TEST_ASSERT_TRUE_MESSAGE(t.snapCarved, y.name.c_str());  // (each freed before the next: carved again)
        kept += t.took >= 0 ? 1 : 0;
        out += t.outOfL ? 1 : 0;
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.page.carve == StatsRun::Carve::Plain ? kept : 0, r.pageFreed, y.name.c_str());
      taken += kept;
      turns += static_cast<uint32_t>(r.turns.size());
    }
    printf("[stats-yard] A4's score | %s: %u of %u steps failing (the control %u), %u the control doesn't fail: %s; "
           "the visits took a grade %u of %u times, the snapshot out of the largest free block %u times\n",
           y.name.c_str(), (unsigned)y.fails.size(), (unsigned)(hs.size() * 5), (unsigned)control.fails.size(),
           (unsigned)notControl, notControl ? "FAILS" : "passes", (unsigned)taken, (unsigned)turns, (unsigned)out);
    // The snapshot never came out of the largest free block (a smaller free
    // class always took it), in every history of every build.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, out, y.name.c_str());
  }
  auto same = [&](size_t a, size_t b) {
    for (size_t i = 0; i < hs.size(); ++i) {
      const std::string w = yards[b].name + ", " + hs[i].name;
      TEST_ASSERT_EQUAL_size_t_MESSAGE(yards[a].bootJoined[i], yards[b].bootJoined[i], w.c_str());
      TEST_ASSERT_TRUE_MESSAGE(yards[a].joined[i] == yards[b].joined[i], w.c_str());
    }
  };
  // S3a's V5, nothing held (the control): the visits alone (Hv: the snapshot
  // carved before each take and freed at its landing) give the rework's
  // figures to the byte in every history of every build: no ask leaves a
  // trace.
  same(kRework, kVisits);
  // What each held snapshot moved: its run's figures (the boot's join's and
  // each update's) against the visits alone's, history by history, and the
  // steps it fails that the control doesn't. Printed for A4, 9.7's fallback
  // and the top (V5's 3a-4 fallback).
  struct Moves {
    uint32_t n = 0, under = 0, over = 0, notControl = 0;
    long long least = 0, most = 0;  // the largest move under, and over
    std::string leastAt, mostAt;
  };
  auto movesOf = [&](size_t yi) {
    Moves m;
    const Yard& y = yards[yi];
    for (size_t i = 0; i < hs.size(); ++i) {
      for (int k = -1; k < 5; ++k) {
        const size_t v = k < 0 ? yards[kVisits].bootJoined[i] : yards[kVisits].joined[i][static_cast<size_t>(k)];
        const size_t j = k < 0 ? y.bootJoined[i] : y.joined[i][static_cast<size_t>(k)];
        if (j == v) continue;
        const long long dlt = static_cast<long long>(j) - static_cast<long long>(v);
        ++m.n;
        char b[120];
        std::snprintf(b, sizeof(b), "%s %s (%u B against %u B)", hs[i].name.c_str(),
                      k < 0 ? "boot" : std::to_string(k).c_str(), (unsigned)j, (unsigned)v);
        if (dlt < 0) {
          ++m.under;
          if (dlt < m.least) {
            m.least = dlt;
            m.leastAt = b;
          }
        } else {
          ++m.over;
          if (dlt > m.most) {
            m.most = dlt;
            m.mostAt = b;
          }
        }
      }
    }
    for (const auto& f : y.fails)
      if (std::find(control.fails.begin(), control.fails.end(), f) == control.fails.end()) ++m.notControl;
    printf("[stats-yard] moved | %s: %u of %u figures (the boot's join and 5 updates, %u histories) not the visits "
           "alone's: %u under (at most %+lld B%s%s), %u over (at most %+lld B%s%s); %u of %u steps failing (the "
           "control %u), %u the control doesn't\n",
           y.name.c_str(), (unsigned)m.n, (unsigned)(hs.size() * 6), (unsigned)hs.size(), (unsigned)m.under, m.least,
           m.under ? ": " : "", m.leastAt.c_str(), (unsigned)m.over, m.most, m.over ? ": " : "", m.mostAt.c_str(),
           (unsigned)y.fails.size(), (unsigned)(hs.size() * 5), (unsigned)control.fails.size(),
           (unsigned)m.notControl);
    return m;
  };
  for (const size_t yi : {kA4Small, kA4, kA4Bound}) movesOf(yi);
  // 9.7's fallback ("If 3a-4 fails"; V5's 3a-4 fallback): the snapshot
  // freed before each decoder's open and each join's begin made while it is
  // held (statsRelease() from the player's open and the join's begin), not
  // carved again until the next visit. Every figure the visits alone's to
  // the byte (nothing held: run 3a-4's rule), in every history of every
  // build; the visits' grades the same, each held snapshot freed once, none
  // out of the largest free block, no ask leaving a trace. Its cost,
  // printed: how often a page loses its lists (the fallback's frees), and
  // how long a snapshot is held, in the model's points (each decoder's
  // open, the FLAC's end, the Dc, each records' save and the step's figure
  // it lives through), against A4 at the same size (to the next fence).
  auto pointsOf = [&](size_t yi, uint32_t* atFigure) {
    uint32_t points = 0, kept = 0;
    *atFigure = 0;
    for (const StatsRun& r : yards[yi].runs) {
      points += r.heldPoints;
      kept += r.pageFreed;
      *atFigure += r.heldAtFigure;
    }
    return kept ? static_cast<double>(points) / kept : 0.0;
  };
  for (const size_t yi : {kFallback, kFallbackBound}) {
    const Yard& y = yards[yi];
    const size_t a4yi = yi == kFallback ? kA4 : kA4Bound;
    same(kVisits, yi);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, movesOf(yi).n, y.name.c_str());
    uint32_t fired[3] = {0, 0, 0}, kept = 0, histories = 0, most = 0;
    std::string mostAt;
    for (size_t i = 0; i < hs.size(); ++i) {
      const StatsRun& r = y.runs[i];
      const std::string w = y.name + ", " + hs[i].name;
      TEST_ASSERT_EQUAL_size_t_MESSAGE(6, r.turns.size(), w.c_str());
      TEST_ASSERT_TRUE_MESSAGE(r.askTraceless, w.c_str());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(tookOf(yards[kVisits].runs[i]).c_str(), tookOf(r).c_str(), w.c_str());
      uint32_t took = 0;
      for (const StatsRun::Turn& t : r.turns) {
        TEST_ASSERT_TRUE_MESSAGE(t.snapCarved, w.c_str());
        TEST_ASSERT_FALSE_MESSAGE(t.outOfL, w.c_str());
        took += t.took >= 0 ? 1 : 0;
      }
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(took, r.pageFreed, w.c_str());  // (each kept snapshot freed once)
      const uint32_t f = r.letGo[0] + r.letGo[1] + r.letGo[2];
      TEST_ASSERT_TRUE_MESSAGE(f <= took, w.c_str());
      for (int k = 0; k < 3; ++k) fired[k] += r.letGo[k];
      kept += took;
      histories += f ? 1 : 0;
      if (f > most) {
        most = f;
        mostAt = hs[i].name;
      } else if (f == most && f) {
        mostAt += mostAt.size() < 80 ? ", " + hs[i].name : std::string();
      }
    }
    uint32_t atFigure = 0, a4AtFigure = 0;
    const double held = pointsOf(yi, &atFigure), a4Held = pointsOf(a4yi, &a4AtFigure);
    const uint32_t all = fired[0] + fired[1] + fired[2];
    printf("[stats-yard] 3a-4's fallback | %s: every figure the visits alone's, to the byte (%u histories, the boot's "
           "join and 5 updates each); the snapshot freed by the fallback %u times of the %u kept after a visit "
           "(before a FLAC's open %u, an Opus's open %u, a join's begin %u), in %u of %u histories, %.2f a history, "
           "at most %u (%s); kept to the next fence or the run's end %u times; held through %.2f of the model's "
           "points on average (A4 at the same size %.2f), at its step's figure %u times (A4 %u)\n",
           y.name.c_str(), (unsigned)hs.size(), (unsigned)all, (unsigned)kept, (unsigned)fired[0],
           (unsigned)fired[1], (unsigned)fired[2], (unsigned)histories, (unsigned)hs.size(),
           static_cast<double>(all) / hs.size(), (unsigned)most, mostAt.c_str(), (unsigned)(kept - all), held, a4Held,
           (unsigned)atFigure, (unsigned)a4AtFigure);
  }
  // Its frees and holds are the history's and the visits' grades',
  // whatever the snapshot's size: 4,096 B's are 3,860 B's, history by
  // history.
  for (size_t i = 0; i < hs.size(); ++i) {
    const StatsRun& p = yards[kFallback].runs[i];
    const StatsRun& q = yards[kFallbackBound].runs[i];
    for (int k = 0; k < 3; ++k) TEST_ASSERT_EQUAL_UINT32_MESSAGE(p.letGo[k], q.letGo[k], hs[i].name.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(p.heldPoints, q.heldPoints, hs[i].name.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(p.heldAtFigure, q.heldAtFigure, hs[i].name.c_str());
  }
  // A placement for comparison (printed, not pinned, not the rule's): the
  // snapshot at the top through the hold, held as A4 holds it. Its moves
  // against the visits alone, beside A4's above, and its takes.
  uint32_t visitsTaken = 0;
  for (const StatsRun& r : yards[kVisits].runs)
    for (const StatsRun::Turn& t : r.turns) visitsTaken += t.took >= 0 ? 1 : 0;
  for (const size_t yi : {kTop, kTopBound}) {
    const Yard& y = yards[yi];
    movesOf(yi);
    std::map<std::string, uint32_t> why;
    uint32_t refused = 0, outOfL = 0, took = 0, turns = 0;
    for (const StatsRun& r : y.runs) {
      for (const auto& w : r.topWhy) why[w.first] += w.second;
      outOfL += r.topOutOfL;
      for (const StatsRun::Turn& t : r.turns) {
        ++turns;
        refused += t.snapCarved ? 0 : 1;
        took += t.took >= 0 ? 1 : 0;
      }
    }
    std::string ws;
    for (const auto& w : why) ws += (ws.empty() ? "" : ", ") + w.first + " " + std::to_string(w.second);
    printf("[stats-yard] the top | %s: the snapshot's takes: %s; out of the largest free block %u of %u, refused "
           "(no room with a decoder's) %u; the visits took a grade %u times of %u (the visits alone %u)\n",
           y.name.c_str(), ws.c_str(), (unsigned)outOfL, (unsigned)turns, (unsigned)refused, (unsigned)took,
           (unsigned)turns, (unsigned)visitsTaken);
  }
  // A4's moves traced (printed): the 3,860 B way's suite step (D's update
  // 4), its worst in the wide build ("opus 2 flac 2 dc 0", update 4) and
  // the worst of every way there (4,096 B, "opus 3 flac 3 dc 1", update 4).
  traceHeldMove("A4 at 3,860 B", saved, records, card, visits, a4, History{"D", "D", -1, 2, true}, 4);
  traceHeldMove("A4 at 3,860 B", saved, records, card, visits, a4, History{"opus 2 flac 2 dc 0", "", 2, 2, false}, 4);
  traceHeldMove("A4 at 4,096 B", saved, records, card, visits, a4Bound, History{"opus 3 flac 3 dc 1", "", 3, 3, true},
                4);
  // The ranges' block: 1,284 B with no ACKS row above MARK, 3,076 B with 16
  // (the largest the docs budget), in every history.
  TEST_ASSERT_EQUAL_size_t(1284, yards[kRework].runs[0].rangesBytes);
  TEST_ASSERT_EQUAL_size_t(3076, yards[kReworkAcks].runs[0].rangesBytes);
  TEST_ASSERT_EQUAL_size_t(3076, yards[kFullAcks].runs[0].rangesBytes);
  TEST_ASSERT_EQUAL_size_t(3076, yards[kUnderAcks].runs[0].rangesBytes);
  TEST_ASSERT_EQUAL_UINT32(0, yards[kRework].runs[0].chunks);  // (a usual boot: no chunk, nothing holds its start)
  // A start at the boot asked during the scan first opens the Opus before
  // the boot's join (a history with it at the boot), the join first after.
  for (size_t i = 0; i < hs.size(); ++i) {
    TEST_ASSERT_EQUAL(hs[i].opusAt == -1, yards[kRework].opened[i]);
    TEST_ASSERT_FALSE(yards[kReworkJoin].opened[i]);
  }
  // The shapes the waits exist for: a full log's overflow held through
  // update 0's build, or carved in its fence, breaks it in every build. The
  // Opus opening under the scan's blocks (no start waiting) is reported,
  // and pinned in the suite's build.
  TEST_ASSERT_FALSE_MESSAGE(breaks(yards[kThrough]).empty(), yards[kThrough].name.c_str());
  TEST_ASSERT_FALSE_MESSAGE(breaks(yards[kFence]).empty(), yards[kFence].name.c_str());

  (void)kUnderFirst;  // (the variants report them)
  (void)kUnderMid;
  (void)kFenceBefore;
  (void)kFenceAfter;
  (void)kFullLog;
  (void)kFullFails;
  (void)kFullJoin;
  (void)kFullAfter;
#if !defined(STATS_OLD_DECODERS) && !defined(STATS_WIDE)
  // The suite's build: every figure pinned (MEASURED-HOST), the boot's join
  // and joined after each of the 5 updates, the histories in hs's order:
  // main's, the control's, main's with the Opus opening before the boot's
  // join; the rework's usual boot with the scan first (a start at the boot
  // opening the Opus during the scan, under the ranges' block: nothing holds
  // it), the same with ACKS's 16 rows above MARK (the ranges' block at its
  // largest), and with the join first (the Opus opening after the boot's
  // join, before its save: under the ranges' block too); and three shapes
  // (held through update 0's build, carved in its fence, the Opus under a
  // full log's overflow with ACKS's 16 rows above MARK). Since the resident
  // set moved to internal RAM (5.14) the stats hold nothing in PSRAM between
  // calls, and where no stats block is live when a decoder opens, the
  // rework's figures are main's to the byte: a full log's ways (the start
  // waiting for its chunks), the scan first, failing its boot's save, with
  // ACKS's rows: main's with the Opus first; the join first, the scan after
  // update 0: main's. A usual boot's start opens the Opus under the ranges'
  // block (1,284 B, 3,076 B with ACKS's rows; given back at the boot's save):
  // since fix A (the free block above the hold's blocks closed by caps, the
  // overflow in one arena) main's with the Opus first to the byte with the
  // scan first (K3, K4 and D's step 4 differed by -20,480 to -45,056 B
  // before it), and with the join first H5's, K3's, K4's and D's steps 3-4
  // differ as before (-110,592 to +106,496 B), main's own variation's size
  // (the control's -61,440 B at K3's and D's step 3); no step fails. The
  // records' saves at 40 in update 0's fence give the usual boot's figures,
  // and so does the Opus under a full log's overflow, opening after the
  // scan's last piece, its first or its 50th, with ACKS's rows too.
  struct Pin {
    size_t boot, joined[5];
  };
  const Pin pins[][13] = {
      {  // main
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {249844, 249844, 360436, 180212, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {589812, {249844, 249844, 360436, 270324, 360436}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {589812, {249844, 249844, 180212, 311284, 270324}}, {589812, {249844, 249844, 360436, 270324, 180212}},
          {589812, {249844, 249844, 180212, 311284, 270324}},
      },
      {  // the control
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {249844, 249844, 360436, 180212, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {589812, {249844, 249844, 360436, 270324, 360436}}, {589812, {249844, 294900, 286708, 204788, 176116}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {589812, {249844, 249844, 180212, 249844, 270324}}, {589812, {249844, 249844, 360436, 270324, 180212}},
          {589812, {249844, 249844, 180212, 249844, 270324}},
      },
      {  // main, the Opus first
          {589812, {360436, 360436, 360436, 360436, 360436}}, {466932, {249844, 249844, 360436, 116724, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {466932, {249844, 249844, 360436, 270324, 249844}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {466932, {249844, 249844, 180212, 249844, 270324}}, {466932, {249844, 249844, 360436, 270324, 294900}},
          {466932, {249844, 249844, 180212, 249844, 270324}},
      },
      {  // the rework, a usual boot, the scan first (a start at the boot opening the Opus under the ranges' block): main's with the Opus first
          {589812, {360436, 360436, 360436, 360436, 360436}}, {466932, {249844, 249844, 360436, 116724, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {466932, {249844, 249844, 360436, 270324, 249844}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {466932, {249844, 249844, 180212, 249844, 270324}}, {466932, {249844, 249844, 360436, 270324, 294900}},
          {466932, {249844, 249844, 180212, 249844, 270324}},
      },
      {  // the same, the root's ACKS 16 rows above MARK (the ranges' block 3,076 B): main's with the Opus first
          {589812, {360436, 360436, 360436, 360436, 360436}}, {466932, {249844, 249844, 360436, 116724, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {466932, {249844, 249844, 360436, 270324, 249844}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {466932, {249844, 249844, 180212, 249844, 270324}}, {466932, {249844, 249844, 360436, 270324, 294900}},
          {466932, {249844, 249844, 180212, 249844, 270324}},
      },
      {  // the rework, a usual boot, the join first (the Opus under the ranges' block, after the boot's join)
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {249844, 249844, 360436, 180212, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {589812, {249844, 249844, 360436, 270324, 249844}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {589812, {249844, 249844, 180212, 249844, 270324}}, {589812, {249844, 249844, 360436, 270324, 286708}},
          {589812, {249844, 249844, 180212, 249844, 270324}},
      },
      {  // held through the build
          {589812, {466932, 360436, 360436, 360436, 360436}}, {589812, {352244, 241652, 241652, 131060, 241652}},
          {589812, {466932, 360436, 294900, 278516, 278516}}, {589812, {335860, 249844, 249844, 249844, 249844}},
          {589812, {352244, 241652, 241652, 241652, 241652}}, {589812, {278516, 294900, 278516, 172020, 106484}},
          {589812, {466932, 286708, 360436, 360436, 360436}}, {589812, {466932, 360436, 360436, 360436, 112628}},
          {589812, {466932, 360436, 360436, 360436, 360436}}, {589812, {466932, 360436, 360436, 360436, 278516}},
          {589812, {352244, 241652, 131060, 241652, 241652}}, {589812, {352244, 241652, 241652, 241652, 131060}},
          {589812, {352244, 241652, 131060, 241652, 241652}},
      },
      {  // carved in the fence
          {589812, {319476, 360436, 360436, 360436, 360436}}, {589812, {180212, 249844, 360436, 180212, 270324}},
          {589812, {319476, 360436, 360436, 270324, 249844}}, {589812, {258036, 180212, 249844, 249844, 249844}},
          {589812, {180212, 249844, 360436, 270324, 360436}}, {589812, {237556, 294900, 286708, 204788, 180212}},
          {589812, {319476, 360436, 360436, 360436, 360436}}, {589812, {319476, 360436, 360436, 294900, 221172}},
          {589812, {319476, 360436, 360436, 360436, 360436}}, {589812, {319476, 360436, 360436, 360436, 360436}},
          {589812, {180212, 249844, 180212, 311284, 270324}}, {589812, {180212, 249844, 360436, 270324, 180212}},
          {589812, {180212, 249844, 180212, 311284, 270324}},
      },
      {  // the Opus under a full log's overflow with ACKS's 16 rows above MARK: main's with the Opus first
          {589812, {360436, 360436, 360436, 360436, 360436}}, {466932, {249844, 249844, 360436, 116724, 270324}},
          {589812, {360436, 360436, 360436, 270324, 249844}}, {589812, {294900, 172020, 172020, 106484, 172020}},
          {466932, {249844, 249844, 360436, 270324, 249844}}, {589812, {249844, 294900, 286708, 204788, 180212}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 294900, 221172}},
          {589812, {360436, 360436, 360436, 360436, 360436}}, {589812, {360436, 360436, 360436, 360436, 360436}},
          {466932, {249844, 249844, 180212, 249844, 270324}}, {466932, {249844, 249844, 360436, 270324, 294900}},
          {466932, {249844, 249844, 180212, 249844, 270324}},
      },
  };
  const size_t pinned[] = {kMain, kControl, kMainFirst, kRework, kReworkAcks, kReworkJoin, kThrough, kFence, kUnderAcks};
  TEST_ASSERT_EQUAL_size_t(sizeof(pins) / sizeof(pins[0]), sizeof(pinned) / sizeof(pinned[0]));
  TEST_ASSERT_EQUAL_size_t(13, hs.size());
  for (size_t p = 0; p < sizeof(pinned) / sizeof(pinned[0]); ++p) {
    const Yard& y = yards[pinned[p]];
    for (size_t i = 0; i < hs.size(); ++i) {
      const std::string w = y.name + ", " + hs[i].name;
      TEST_ASSERT_EQUAL_size_t_MESSAGE(pins[p][i].boot, y.bootJoined[i], w.c_str());
      for (size_t k = 0; k < 5; ++k) TEST_ASSERT_EQUAL_size_t_MESSAGE(pins[p][i].joined[k], y.joined[i][k], w.c_str());
    }
  }
  // No stats block live when the Opus opens: main's, to the byte.
  for (const size_t yi : {kFullLog, kFullFails, kFullAcks}) same(kMainFirst, yi);
  for (const size_t yi : {kFullJoin, kFullAfter}) same(kMain, yi);
  // The usual boot's figures: the records' saves at 40 in update 0's fence,
  // and the Opus under a full log's overflow with no ACKS row.
  for (const size_t yi : {kFenceBefore, kFenceAfter, kUnder, kUnderFirst, kUnderMid}) same(kRework, yi);
  // The control fails no step here, so the rule for the stats is every
  // step. Held through the build fails 16 (H2 1, 2, 4; H3 3; H5 1-4; K3 1,
  // 3, 4; K4 1-3; D 1, 3, 4: 15 before fix A), carved in the fence 6 (step 0
  // of H2, H5, H6, K3, K4, D: H6's step 2 alone before it): the hold's
  // caps and the arena held through an update's build fragment it more,
  // and the waits that keep them apart stay load-bearing. Since fix A the
  // Opus under a full log's overflow gives main's figures with the Opus
  // first wherever the start comes, the ranges' block at its largest
  // (ACKS's 16 rows) too (before it: its boot's join 401,396 B against
  // 466,932 B in H2, H5, K3, K4 and D): nothing it carves meanwhile lands in
  // a free block the hold left open. The start's wait is kept (main.cpp's
  // OutputHold: the device's decoders, not the model's, and the hold's
  // peak).
  TEST_ASSERT_EQUAL_size_t(0, control.fails.size());
  TEST_ASSERT_EQUAL_size_t(0, mainFirst.fails.size());
  TEST_ASSERT_EQUAL_size_t(16, yards[kThrough].fails.size());
  TEST_ASSERT_EQUAL_size_t(6, yards[kFence].fails.size());
  TEST_ASSERT_EQUAL_size_t(0, yards[kUnderAcks].fails.size());
  for (const size_t yi : {kUnder, kUnderFirst, kUnderMid, kUnderAcks}) TEST_ASSERT_TRUE(breaks(yards[yi]).empty());
  // Fix A: a usual boot's ways with the scan first (its start opening the
  // Opus under the ranges' block) and the Opus under a full log's overflow
  // with ACKS's rows are main's with the Opus first to the byte; the join
  // first as before (main's own variation's size at H5 4, K3 3, K4 4, D 3).
  for (const size_t yi : {size_t{kRework}, size_t{kReworkAcks}, kUnderAcks}) same(kMainFirst, yi);
  // S3a's V5. The visits alone give the rework's figures to the byte (above,
  // in every build). A4, the snapshot held from each visit to the next
  // fence, gives them too but at these steps, none failing (MEASURED-HOST;
  // the model, a pre-check: run 3a-4 decides on the device, to the byte, and
  // would fail a moved figure). 3,860 B: D's step 4 one class under (262,132
  // B against 270,324 B); 4,096 B: H3's step 3, K4's step 3 and D's step 4
  // the same; 512 B: H4's steps 3 and 4 above main's (172,020 B against
  // 106,484 B; 278,516 B against 172,020 B), and its last visit takes Full
  // where the visits alone find no room. The visits took a grade 68 times of
  // 78 (Hv, A4 at 3,860 and 4,096 B), 69 (512 B): refused (no decoder's room
  // with the block held) where a FLAC still held its blocks at the visit
  // (H2, K3, K4, D) or the kept Opus left too little (H4, H6), and Lists
  // after H8's step 4.
  struct Moved {
    size_t yard;
    const char* history;
    int step;
    size_t joined;
  };
  const Moved moved[] = {
      {kA4Small, "H4", 3, 172020}, {kA4Small, "H4", 4, 278516}, {kA4, "D", 4, 262132},
      {kA4Bound, "H3", 3, 262132}, {kA4Bound, "K4", 3, 262132}, {kA4Bound, "D", 4, 262132},
  };
  for (const size_t yi : {kA4Small, kA4, kA4Bound}) {
    const Yard& y = yards[yi];
    for (size_t i = 0; i < hs.size(); ++i) {
      const std::string w = y.name + ", " + hs[i].name;
      TEST_ASSERT_EQUAL_size_t_MESSAGE(yards[kVisits].bootJoined[i], y.bootJoined[i], w.c_str());
      for (int k = 0; k < 5; ++k) {
        size_t want = yards[kVisits].joined[i][static_cast<size_t>(k)];
        for (const Moved& m : moved)
          if (m.yard == yi && hs[i].name == m.history && m.step == k) want = m.joined;
        TEST_ASSERT_EQUAL_size_t_MESSAGE(want, y.joined[i][static_cast<size_t>(k)], w.c_str());
      }
      const bool h4 = yi == kA4Small && hs[i].name == "H4";
      TEST_ASSERT_EQUAL_STRING_MESSAGE(h4 ? "Full, Full, refused, refused, refused, Full"
                                          : tookOf(yards[kVisits].runs[i]).c_str(),
                                       tookOf(y.runs[i]).c_str(), w.c_str());
    }
  }
  uint32_t visitsTook = 0;
  for (size_t i = 0; i < hs.size(); ++i)
    for (const StatsRun::Turn& t : yards[kVisits].runs[i].turns) visitsTook += t.took >= 0 ? 1 : 0;
  TEST_ASSERT_EQUAL_UINT32(68, visitsTook);
  TEST_ASSERT_EQUAL_STRING("Full, Full, Full, Full, Full, Lists", tookOf(yards[kVisits].runs[7]).c_str());  // H8
  // V5's 3a-4 fallback, history by history (MEASURED-HOST; 3,860 B's, the
  // 4,096 B way's the same, above): its frees before a FLAC's open, an
  // Opus's open and a join's begin, the model's points its snapshots lived
  // through and the figures (the boot's and 5 updates') they were held at.
  // 39 frees of the 68 snapshots kept (3 at a FLAC's open, 36 at a join's
  // begin, none at an Opus's: no named history opens it after an update
  // without a Dc first), in 11 of the 13 histories; 81 points (1.19 a
  // snapshot, A4's 3.46), 32 figures (A4's 68). Where a Dc follows each
  // update (H2-H4, H6-H8, K1, K4, D) every update's snapshot it kept goes
  // at that Dc, held through the FLAC's end at most; a page keeps its
  // lists to the next fence in H1, H5, K2 and K3 (no Dc), but for the
  // snapshot a FLAC's open takes in K2 and K3, and the boot's visit's to
  // update 0's fence everywhere but H6 (its FLAC opening there).
  struct Fell {
    uint32_t flac, opus, join, points, figures;
  };
  const Fell fell[13] = {
      {0, 0, 0, 16, 6}, {0, 0, 4, 2, 1}, {0, 0, 5, 2, 1}, {0, 0, 1, 2, 1}, {0, 0, 0, 16, 6},
      {1, 0, 3, 2, 1},  {0, 0, 5, 3, 1}, {0, 0, 5, 3, 1}, {0, 0, 5, 3, 1}, {1, 0, 0, 16, 6},
      {1, 0, 0, 12, 5}, {0, 0, 4, 2, 1}, {0, 0, 4, 2, 1},
  };
  for (size_t i = 0; i < hs.size(); ++i) {
    const StatsRun& r = yards[kFallback].runs[i];
    const std::string w = yards[kFallback].name + ", " + hs[i].name;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fell[i].flac, r.letGo[StatsRun::kLetGoFlac], w.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fell[i].opus, r.letGo[StatsRun::kLetGoOpus], w.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fell[i].join, r.letGo[StatsRun::kLetGoJoin], w.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fell[i].points, r.heldPoints, w.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(fell[i].figures, r.heldAtFigure, w.c_str());
  }
#endif
  if (!broken.empty()) {
    const std::string m = "the rework breaks 5.14's yardstick: " + broken;
    TEST_FAIL_MESSAGE(m.c_str());
  }
}

int main(int, char**) {
  UNITY_BEGIN();
#if !defined(STATS_OLD_DECODERS) && !defined(STATS_WIDE)  // (the yardstick's variants run it alone)
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
  RUN_TEST(test_the_model_heap_against_the_device);
  RUN_TEST(test_the_placement_s_order);
  RUN_TEST(test_free_never_under_the_margin);
  RUN_TEST(test_the_rejoin_anywhere_in_a_class);
  RUN_TEST(test_the_room_above_the_spacers);
  RUN_TEST(test_another_task_s_block_meanwhile);
  RUN_TEST(test_caps_close_any_free_block);
  RUN_TEST(test_the_hold_closes_t);
  RUN_TEST(test_a_cap_that_lands_elsewhere);
  RUN_TEST(test_another_task_s_carve_before_the_cap);
  RUN_TEST(test_the_hold_given_back_bottom_up);
  RUN_TEST(test_a_block_in_a_hole_gets_no_cap);
  RUN_TEST(test_the_caps_keep_the_margin);
  RUN_TEST(test_no_spacers_no_cap);
  RUN_TEST(test_a_block_freed_into_t_between_caps);
  RUN_TEST(test_no_block_asks_no_carve);
  RUN_TEST(test_a_decoder_s_room);
  RUN_TEST(test_the_walk_above);
  RUN_TEST(test_the_placement_in_a_model_heap);
  RUN_TEST(test_a_library_update_in_a_model_heap);
  RUN_TEST(test_the_listening_stats_in_a_model_heap);
  RUN_TEST(test_an_s2_ask_in_a_model_heap);
  RUN_TEST(test_an_s3_ask_in_a_model_heap);
  RUN_TEST(test_an_s3_ask_in_a_reduced_room);
  RUN_TEST(test_the_hold_leaves_main_s_layout);
  RUN_TEST(test_the_arena_s_fallback_as_before);
  RUN_TEST(test_a_waiting_start_asks_no_room);
  RUN_TEST(test_the_ranges_take_asks_the_room);
  RUN_TEST(test_the_room_s_rule);
#endif
  RUN_TEST(test_the_listening_stats_across_updates);
  return UNITY_END();
}
