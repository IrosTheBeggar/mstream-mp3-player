// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the listening stats' map (docs/LISTENING-STATS.md 6, 9.6's
// C11 and C13): the library's path hashes walked in slices
// (lib/core/PathHashWalk, the walk Auto DJ's join takes them by too).
// - The walk: every track's hash is trackPathHash()'s, and none where it
//   has none, whatever the slices' sizes, with the folders' states and
//   without them; a library past kMaxFolders folders takes whole paths; an
//   index rebuilt under the map no longer matches() (StatsMap's).
// - The map (lib/core/StatsMap): the album bits and the wanted hashes' ids
//   against a count made track by track, in slices of every size, orphans
//   and hashes no list wants among them; a 64-bit collision maps neither;
//   an index rebuilt under the walk aborts it, and a new begin() gives the
//   new index's; the wanted set sealed (sorted, each hash once, across
//   chunks); Not played yet over every fixture boot statsref names it for
//   (test/fixtures/card/stats: its library, the boot's card ranked by
//   StatsRank for the played set).
// S3a's V2 (9.7, "S3a: tests"):
// - the album bits over the u40 played set (Map::begin() over a Played40:
//   the full rank's and the played-set-only rank's) equal the u64 set's and
//   statsref's on every fixture boot that names Not played yet, and on
//   every rank scenario (stats/rank.json) over a library made of its played
//   hashes and unplayed ones;
// - call 3 ("5-byte fingerprints"): an injected top-40 collision hides
//   exactly its album and never shows a played one; the ids stay exact;
// - Wanted::rowsCap: one region for fewer rows than a chunk's (the lists'
//   696 rows in 8,352 B), nothing written past it;
// - seal()'s heap sort: the sealed set equal to the insertion sort's (S2's
//   seal(), kept here as the reference) on every fixture's lists, on
//   reversed input and on equal hashes.
// Run: pio test -e native -f test_stats_map
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContainer.h"
#include "CardContract.h"
#include "LibraryIndex.h"
#include "PathHashWalk.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsMap.h"
#include "StatsRank.h"

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sm = statsmap;
namespace sr = statsrank;
namespace sx = statsfile;
using pathhash::Walk;

namespace {

// A file under `parent` whose whole path is `bytes` long.
std::string pathOf(const std::string& parent, size_t bytes) {
  const size_t used = parent.size() + 1 + 4;  // ('/' and ".mp3")
  return parent + "/" + std::string(bytes > used ? bytes - used : 1, 'n') + ".mp3";
}

// The paths test_autodj_join's test_the_folders_hash takes: titles in
// folders, the root's own, a near miss of "/music/", whole paths just under,
// at and over kPathBytes, a long folder, a folder too long, folders deeper
// than LibraryIndex::folderPath() goes.
std::vector<std::string> pathsUnder(const std::string& r) {
  std::vector<std::string> paths;
  char buf[96];
  for (int i = 0; i < 40; ++i) {
    snprintf(buf, sizeof(buf), "/Artist %d/Album %d/%02d - Title.mp3", i % 5, i % 7, i);
    paths.push_back(r + buf);
  }
  paths.push_back(r + "/Loose.mp3");
  paths.push_back(r + "/ic/Under.mp3");
  for (size_t bytes = 509; bytes <= 513; ++bytes) paths.push_back(pathOf(r + "/Long", bytes));
  const std::string wide = r + "/" + std::string(250, 'd') + "/" + std::string(240, 'e');
  paths.push_back(wide + "/x.mp3");
  paths.push_back(r + "/" + std::string(260, 'f') + "/" + std::string(260, 'g') + "/y.mp3");
  std::string deep = r;
  for (int d = 1; d <= 34; ++d) {
    deep += "/d" + std::to_string(d);
    if (d >= 30) paths.push_back(deep + "/z.mp3");
  }
  return paths;
}

struct Seen {
  std::map<uint32_t, uint64_t> hashes;
  uint32_t twice = 0;
  uint32_t folders = 0;  // the walk's folders with a state
};

void see(void* ctx, uint32_t t, uint64_t h) {
  Seen& s = *static_cast<Seen*>(ctx);
  if (!s.hashes.emplace(t, h).second) ++s.twice;
}

// The walk over `idx`, folders' states or none, in slices of `folders` and
// `tracks`; what it handed.
Seen walkAll(const LibraryIndex& idx, bool states, uint32_t folders, uint32_t tracks, Walk* w) {
  const uint32_t n = states ? Walk::foldersFor(idx) : 0;
  std::vector<uint8_t> block(Walk::bytesFor(n) + 1);
  w->begin(&idx, n, n ? block.data() : nullptr);
  Seen s;
  uint32_t slices = 0;
  while (!w->foldersDone()) {
    w->foldersSlice(folders);
    TEST_ASSERT_TRUE(++slices < 100000);
  }
  while (!w->tracksDone()) {
    TEST_ASSERT_TRUE(w->hashSlice(tracks, see, &s));
    TEST_ASSERT_TRUE(++slices < 100000);
  }
  TEST_ASSERT_FALSE(w->hashSlice(tracks, see, &s));  // (nothing left)
  s.folders = w->folders();
  w->end();
  TEST_ASSERT_EQUAL_UINT32(0, w->folders());
  return s;
}

}  // namespace

// Every track's hash is trackPathHash()'s, each track handed once, none
// where it has none: the root "/music" in any case, alone or with more
// after it, other roots; slices of 1, 3 and 250 folders and 1, 7 and 250
// tracks; with the folders' states and without them (the hook's whole
// paths, the join's other way).
void test_every_hash_is_track_path_hash() {
  const char* const roots[] = {"/music", "/MUSIC", "/Music", "/mUsIc/inner", "/musicx", "/sd", "/mus"};
  for (const char* root : roots) {
    LibraryIndex idx;
    TEST_ASSERT_TRUE(idx.begin(root));
    for (const std::string& p : pathsUnder(root)) idx.addFile(p.c_str());
    TEST_ASSERT_TRUE(idx.finish());
    std::map<uint32_t, uint64_t> want;
    char p[Walk::kPathBytes];
    for (uint32_t t = 0; t < idx.trackCount(); ++t) {
      uint64_t h;
      if (pathhash::trackPathHash(idx, t, p, sizeof(p), &h)) want[t] = h;
    }
    TEST_ASSERT_TRUE_MESSAGE(idx.trackCount() - want.size() >= 5, root);  // (the cases with none are there)
    const uint32_t sizes[][2] = {{1, 1}, {3, 7}, {250, 250}};
    for (bool states : {true, false}) {
      for (const auto& sz : sizes) {
        Walk w;
        const Seen s = walkAll(idx, states, sz[0], sz[1], &w);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, s.twice, root);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), s.hashes.size(), root);
        TEST_ASSERT_TRUE_MESSAGE(want == s.hashes, root);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(idx.trackCount(), w.position(), root);  // (every track passed)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(states ? idx.folderCount() : 0, s.folders, root);
      }
    }
    // trackHash() alone, after the folders: each track's, as handed.
    Walk w;
    std::vector<uint8_t> block(Walk::bytesFor(idx.folderCount()));
    w.begin(&idx, idx.folderCount(), block.data());
    while (!w.foldersDone()) w.foldersSlice(250);
    for (uint32_t t = 0; t < idx.trackCount(); ++t) {
      uint64_t h = 0;
      const bool has = w.trackHash(t, &h);
      TEST_ASSERT_EQUAL_MESSAGE(want.count(t) == 1, has, root);
      if (has) TEST_ASSERT_TRUE_MESSAGE(want[t] == h, root);
    }
    printf("[walk] root \"%s\": %u tracks in %u folders, %u hashed, each trackPathHash()'s\n", root,
           (unsigned)idx.trackCount(), (unsigned)idx.folderCount(), (unsigned)want.size());
  }
}

// The default rule spelled out: "/music/" and the rest is pathHash()'s,
// FNV-1a 64 of the root's literal lowercase and the path as it is.
void test_the_rule() {
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/MUSIC"));
  idx.addFile("/MUSIC/Artist/Album/01 Song.flac");
  TEST_ASSERT_TRUE(idx.finish());
  Walk w;
  const Seen s = walkAll(idx, true, 250, 250, &w);
  TEST_ASSERT_EQUAL_UINT32(1, s.hashes.size());
  TEST_ASSERT_TRUE(cc::fnv1a64Str("/music/Artist/Album/01 Song.flac") == s.hashes.begin()->second);
  TEST_ASSERT_TRUE(cc::pathHash("Artist/Album/01 Song.flac") == s.hashes.begin()->second);
}

// More folders than kMaxFolders: no states (foldersFor() 0), each track's
// whole path, every one hashed.
void test_past_the_folders_limit() {
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  char buf[64];
  for (uint32_t i = 0; i <= Walk::kMaxFolders; ++i) {
    snprintf(buf, sizeof(buf), "/music/f%u/t.mp3", (unsigned)i);
    idx.addFile(buf);
  }
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_TRUE(idx.folderCount() > Walk::kMaxFolders);
  TEST_ASSERT_EQUAL_UINT32(0, Walk::foldersFor(idx));
  Walk w;
  const Seen s = walkAll(idx, true, 250, 1000, &w);
  TEST_ASSERT_EQUAL_UINT32(0, s.folders);
  TEST_ASSERT_EQUAL_UINT32(idx.trackCount(), s.hashes.size());
  char p[Walk::kPathBytes];
  for (const auto& kv : s.hashes) {
    uint64_t h;
    TEST_ASSERT_TRUE(pathhash::trackPathHash(idx, kv.first, p, sizeof(p), &h));
    TEST_ASSERT_TRUE(h == kv.second);
  }
}

// The map's matches(): the index begin() was given, as it was (rebuilt
// from the same files: the same stamp, the same ids, still), Ready too;
// rebuilt with a file more (another track count and stamp), another
// object, none, released: no longer.
void test_matches() {
  LibraryIndex idx, other;
  for (LibraryIndex* x : {&idx, &other}) {
    TEST_ASSERT_TRUE(x->begin("/music"));
    x->addFile("/music/A/a.mp3");
    x->addFile("/music/B/b.mp3");
    TEST_ASSERT_TRUE(x->finish());
  }
  sm::Map w;
  TEST_ASSERT_FALSE(w.matches(&idx));  // (nothing begun)
  w.begin(&idx, nullptr, nullptr, nullptr, nullptr);
  TEST_ASSERT_TRUE(w.matches(&idx));
  while (w.status() == sm::Map::Status::Working) w.step(&idx);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(sm::Map::Status::Ready), static_cast<int>(w.status()));
  TEST_ASSERT_TRUE(w.matches(&idx));  // (Ready: still the index it walked)
  TEST_ASSERT_FALSE(w.matches(&other));
  TEST_ASSERT_FALSE(w.matches(nullptr));
  idx.clear();
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addFile("/music/A/a.mp3");
  idx.addFile("/music/B/b.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_TRUE(w.matches(&idx));  // (the same files: the same ids)
  idx.clear();
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addFile("/music/A/a.mp3");
  idx.addFile("/music/A/b.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_FALSE(w.matches(&idx));  // (the same object and count, another stamp)
  idx.clear();
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addFile("/music/A/a.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_FALSE(w.matches(&idx));  // (another count)
  w.release();
  TEST_ASSERT_FALSE(w.matches(&idx));
}

// ---------------------------------------------------------------------------
// The map
// ---------------------------------------------------------------------------
namespace {

// A block aligned as malloc's.
struct Block {
  std::unique_ptr<uint64_t[]> m;
  explicit Block(size_t bytes) : m(new uint64_t[(bytes + 7) / 8 + 1]) {}
  uint8_t* p() { return reinterpret_cast<uint8_t*>(m.get()); }
};

// A played set over `hashes` (sorted here), in chunks.
struct PlayedSet {
  std::vector<std::unique_ptr<Block>> chunks;
  sr::Played p;
  explicit PlayedSet(std::vector<uint64_t> hashes) {
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    for (uint32_t i = 0; i < sr::playedChunks(static_cast<uint32_t>(hashes.size())); ++i) {
      chunks.emplace_back(new Block(sr::kPlayedChunkBytes));
      p.chunk[i] = reinterpret_cast<uint64_t*>(chunks.back()->p());
    }
    p.chunks = static_cast<uint32_t>(chunks.size());
    for (uint64_t h : hashes) {
      p.chunk[p.n / sr::kPlayedChunk][p.n % sr::kPlayedChunk] = h;
      ++p.n;
    }
  }
};

// The u40 set over `hashes` (sorted here, each once, as the u64 set's), as
// the rank writes it: each hash's top 40 bits, 5 B a row, little-endian.
struct Played40Set {
  std::unique_ptr<Block> rows;
  sr::Played40 p;
  explicit Played40Set(std::vector<uint64_t> hashes) {
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    p.cap = static_cast<uint32_t>(hashes.size());
    rows.reset(new Block(sr::Played40::bytesFor(p.cap)));
    p.rows = rows->p();
    for (uint64_t h : hashes) {
      uint8_t* r = p.rows + static_cast<size_t>(p.n++) * sr::Played40::kRowBytes;
      const uint64_t key = sr::Played40::keyOf(h);
      cc::put32(r, static_cast<uint32_t>(key));
      r[4] = static_cast<uint8_t>(key >> 32);
    }
  }
};

// S2's form: whole chunks of kChunkRows.
struct WantedSet {
  std::vector<std::unique_ptr<Block>> chunks;
  sm::Wanted w;
  explicit WantedSet(uint32_t rows) {
    for (uint32_t i = 0; i < sm::wantedChunks(rows); ++i) more();
  }
  void more() {
    chunks.emplace_back(new Block(sm::kChunkBytes));
    w.chunk[w.chunks++] = chunks.back()->p();
  }
};

std::vector<std::string> libraryPaths(int albums, int perAlbum) {
  std::vector<std::string> v;
  char b[96];
  for (int a = 0; a < albums; ++a)
    for (int t = 0; t < perAlbum + a % 3; ++t) {
      snprintf(b, sizeof(b), "/music/Artist %d/Album %02d/%02d Song %d.mp3", a % 7, a, t + 1, t);
      v.push_back(b);
    }
  return v;
}

std::unique_ptr<LibraryIndex> libraryOf(const std::vector<std::string>& paths) {
  std::unique_ptr<LibraryIndex> idx(new LibraryIndex());
  TEST_ASSERT_TRUE(idx->begin("/music"));
  for (const std::string& p : paths) idx->addFile(p.c_str());
  TEST_ASSERT_TRUE(idx->finish());
  return idx;
}

uint64_t hashOf(const LibraryIndex& idx, uint32_t t) {
  char p[Walk::kPathBytes];
  uint64_t h = 0;
  TEST_ASSERT_TRUE(pathhash::trackPathHash(idx, t, p, sizeof(p), &h));
  return h;
}

sm::Map::Status runMap(sm::Map& m, const LibraryIndex* idx) {
  for (int i = 0; i < 1000000 && m.status() == sm::Map::Status::Working; ++i) m.step(idx);
  return m.status();
}

int st(sm::Map::Status s) { return static_cast<int>(s); }

// Track 3's hash is track 5's: a 64-bit collision, made.
bool collide(const LibraryIndex& idx, uint32_t t, char* path, size_t size, uint64_t* h, void*) {
  return pathhash::trackPathHash(idx, t == 3 ? 5 : t, path, size, h);
}

// The hashes `ctx` (a track → hash map) gives, trackPathHash()'s for the
// rest: hashes injected (the hook: no folders' states).
bool injected(const LibraryIndex& idx, uint32_t t, char* path, size_t size, uint64_t* h, void* ctx) {
  const auto& given = *static_cast<const std::map<uint32_t, uint64_t>*>(ctx);
  const auto it = given.find(t);
  if (it == given.end()) return pathhash::trackPathHash(idx, t, path, size, h);
  *h = it->second;
  return true;
}

// The album bits after a Ready walk, a character an album ('1': played).
std::string bitsOf(const sm::Map& m) {
  std::string s;
  for (uint32_t a = 0; a < m.albums(); ++a) s += m.albumPlayed(a) ? '1' : '0';
  return s;
}

// ---------------------------------------------------------------------------
// The fixtures' cards, ranked (S3a: the u40 set and the lists of every card)
// ---------------------------------------------------------------------------
minijson::Value gIndex;
minijson::Value gRank;
const std::string kDir = "/.player/";
using Files = std::vector<std::pair<std::string, std::string>>;  // under /.player/: name, bytes

int todayOf(const minijson::Value& v) { return v.isNull() ? -1 : static_cast<int>(v.u64()); }

// A frame's section bytes.
std::string sectionOf(const std::string& b, const cc::FormatSpec& spec, uint32_t i) {
  cc::MemSource src(b.data(), static_cast<uint32_t>(b.size()));
  cc::Container c;
  TEST_ASSERT_EQUAL_STRING("ok", cc::whyName(c.open(src, spec, nullptr)));
  return b.substr(c.section(i).offset, c.section(i).bytes);
}

// A fixture boot's card as the boot leaves it (index.json's `out`).
Files bootFiles(const minijson::Value& e) {
  Files f;
  for (const auto& m : e["out"].members)
    f.emplace_back(m.first, cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first));
  return f;
}

// A rank scenario's card (rank.json's `in`): stats.bin and the base its
// HEAD names.
Files scenarioFiles(const minijson::Value& sc) {
  const std::string dir = "stats/" + sc["in"].str() + "/";
  Files f;
  f.emplace_back("stats.bin", cardfixtures::bytes(dir + "stats.bin"));
  TEST_ASSERT_TRUE(!f[0].second.empty());
  const std::string head = sectionOf(f[0].second, sx::kStatsSpec, sf::kHead);
  const sf::Head h = sf::decodeHead(reinterpret_cast<const uint8_t*>(head.data()));
  if (h.baseGen) {
    char n[sx::kNameCap];
    TEST_ASSERT_TRUE(sx::baseName(h.baseGen, n, sizeof(n)));
    const std::string name = std::string(n).substr(kDir.size());
    f.emplace_back(name, cardfixtures::bytes(dir + name));
    TEST_ASSERT_TRUE(!f.back().second.empty());
  }
  return f;
}

// Which played set a rank fills: S2's u64 set (Hw's), the u40 set (a
// visit's Full grade), or the u40 set with no lists (the played-set-only
// rank: the lean Not played yet ask's).
enum class Set : uint8_t { U64, U40, Lean };

// A card read as the boot reads it and ranked on `today` (-1: the clock
// unset, no window room carved), every block given room enough.
struct Ranked {
  cutfs::CutFs card;
  sf::Fold fold;
  Block work{sx::kWorkBytes};
  Block lists{sr::Ranker::kBytes};
  std::unique_ptr<Block> windows;
  std::vector<std::unique_ptr<Block>> chunks;
  std::unique_ptr<Block> rows40;
  sr::Played played;
  sr::Played40 played40;
  sr::Ranker ranker;
  sr::Report rep;
};

std::unique_ptr<Ranked> rankOf(const Files& files, int today, Set set, const std::string& what) {
  std::unique_ptr<Ranked> r(new Ranked());
  for (const auto& f : files) r->card.put(kDir + f.first, std::vector<uint8_t>(f.second.begin(), f.second.end()));
  sx::Store store(r->card);
  TEST_ASSERT_TRUE_MESSAGE(sx::statsOn(store.read(r->fold, r->work.p(), nullptr).read), what.c_str());
  sr::Input in;
  in.present = store.cardCopy().present;
  in.generation = store.cardCopy().generation;
  in.headerCrc = store.cardCopy().headerCrc;
  in.baseGen = r->fold.head().baseGen;
  in.baseRows = r->fold.head().baseRows;
  in.baseCrc = r->fold.head().baseCrc;
  in.hasToday = today >= 0;
  in.today = static_cast<uint16_t>(today >= 0 ? today : 0);
  in.liveRound = r->fold.liveRound();  // (as the firmware's prepare(): a session that boots on the card)
  const uint32_t rows = in.baseRows + 4096;  // (the played rows at most: TRKD's under it)
  sr::Memory mem;
  mem.work = r->work.p();
  if (set != Set::Lean) {
    mem.lists = r->lists.p();
    if (in.hasToday) {
      r->windows.reset(new Block(sr::Windows::bytesFor(4096 + 64)));
      mem.windows = r->windows->p();
      mem.windowRows = 4096 + 64;
    }
  }
  if (set == Set::U64) {
    for (uint32_t i = 0; i < sr::playedChunks(rows); ++i) {
      r->chunks.emplace_back(new Block(sr::kPlayedChunkBytes));
      r->played.chunk[i] = reinterpret_cast<uint64_t*>(r->chunks.back()->p());
    }
    r->played.chunks = static_cast<uint32_t>(r->chunks.size());
    mem.played = &r->played;
  } else {
    r->rows40.reset(new Block(sr::Played40::bytesFor(rows)));
    r->played40.rows = r->rows40->p();
    r->played40.cap = rows;
    mem.played40 = &r->played40;
  }
  r->rep = sr::rank(r->card, in, mem, r->ranker);
  TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(r->rep.outcome), what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, set == Set::U64 ? r->played.lost : r->played40.lost, what.c_str());
  return r;
}

// statsref's u40 keys (rank.json's played40: 5 B a key, little-endian, in
// hex).
std::vector<uint64_t> keysOf(const std::string& hex) {
  std::vector<uint64_t> keys;
  TEST_ASSERT_EQUAL_UINT32(0, hex.size() % 10);
  for (size_t i = 0; i < hex.size(); i += 10) {
    uint64_t k = 0;
    for (int b = 4; b >= 0; --b) k = k << 8 | std::stoull(hex.substr(i + 2 * static_cast<size_t>(b), 2), nullptr, 16);
    keys.push_back(k);
  }
  return keys;
}

}  // namespace

// The album bits and the ids, against a count made track by track: every
// slice size, with and without the folders' states; orphans played and
// wanted; a hash added twice.
void test_the_map() {
  const auto idx = libraryOf(libraryPaths(40, 5));
  std::mt19937_64 rng(5);
  std::vector<uint64_t> playedHashes;
  std::set<uint32_t> playedAlbums;
  std::map<uint64_t, uint32_t> want;  // a wanted hash: its id (kNoId: an orphan)
  uint32_t playedTracks = 0;
  for (uint32_t t = 0; t < idx->trackCount(); ++t) {
    const uint64_t h = hashOf(*idx, t);
    if (rng() % 4 == 0) {
      playedHashes.push_back(h);
      playedAlbums.insert(idx->track(t).album);
      ++playedTracks;
      if (rng() % 2) want[h] = t;
    } else if (rng() % 9 == 0) {
      want[h] = t;
    }
  }
  for (int k = 0; k < 30; ++k) {
    const uint64_t h = rng() | 1;
    playedHashes.push_back(h);  // an orphan: a play of a file no longer here
    if (k % 2) want[h] = sm::kNoId;
  }
  PlayedSet played(playedHashes);
  const uint32_t cfgs[][2] = {{1, 1}, {3, 7}, {250, 250}, {10000, 10000}};
  for (bool folders : {true, false}) {
    for (const auto& cf : cfgs) {
      WantedSet ws(static_cast<uint32_t>(want.size()) + 1);
      std::vector<uint64_t> add;
      for (const auto& kv : want) add.push_back(kv.first);
      std::shuffle(add.begin(), add.end(), rng);
      add.push_back(add.front());  // (one twice)
      for (uint64_t h : add) TEST_ASSERT_TRUE(ws.w.add(h));
      ws.w.seal();
      TEST_ASSERT_EQUAL_UINT32(want.size(), ws.w.n);
      Block bits(sm::albumBytes(idx->albumCount()));
      Block fold(Walk::bytesFor(Walk::foldersFor(*idx)));
      sm::Map m;
      m.setConfig(sm::Map::Config{cf[0], cf[1]});
      TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Working),
                            st(m.begin(idx.get(), &played.p, &ws.w, bits.p(), folders ? fold.p() : nullptr)));
      TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, idx.get())));
      for (uint32_t a = 0; a < idx->albumCount(); ++a)
        TEST_ASSERT_EQUAL_MESSAGE(playedAlbums.count(a) == 1, m.albumPlayed(a), idx->albumName(a));
      uint32_t mapped = 0;
      for (const auto& kv : want) {
        TEST_ASSERT_EQUAL_UINT32(kv.second, ws.w.idOf(kv.first));
        mapped += kv.second != sm::kNoId ? 1 : 0;
      }
      TEST_ASSERT_EQUAL_UINT32(mapped, m.stats().wantedMapped);
      TEST_ASSERT_EQUAL_UINT32(0, m.stats().collisions);
      TEST_ASSERT_EQUAL_UINT32(playedAlbums.size(), m.stats().albumsPlayed);
      TEST_ASSERT_EQUAL_UINT32(idx->trackCount(), m.stats().tracks);
      TEST_ASSERT_EQUAL_UINT32(playedTracks, m.stats().playedTracks);
      TEST_ASSERT_EQUAL_UINT32(sm::kNoId, ws.w.idOf(12345));  // not wanted
    }
  }
}

// A hash two tracks of the library share maps neither (6.6); a play of it
// sets both albums' bits.
void test_a_collision_maps_neither() {
  const auto idx = libraryOf(libraryPaths(4, 3));
  const uint64_t h5 = hashOf(*idx, 5);
  const uint64_t h1 = hashOf(*idx, 1);
  PlayedSet played({h5});
  WantedSet ws(4);
  TEST_ASSERT_TRUE(ws.w.add(h5));
  TEST_ASSERT_TRUE(ws.w.add(h1));
  ws.w.seal();
  Block bits(sm::albumBytes(idx->albumCount()));
  sm::Map m;
  m.setPathHash(collide, nullptr);
  m.begin(idx.get(), &played.p, &ws.w, bits.p(), nullptr);  // (no folders' states: the hook)
  TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, idx.get())));
  TEST_ASSERT_EQUAL_UINT32(sm::kNoId, ws.w.idOf(h5));
  TEST_ASSERT_EQUAL_UINT32(1, ws.w.idOf(h1));
  TEST_ASSERT_EQUAL_UINT32(1, m.stats().collisions);
  TEST_ASSERT_EQUAL_UINT32(1, m.stats().wantedMapped);
  TEST_ASSERT_TRUE(m.albumPlayed(idx->track(5).album));
  TEST_ASSERT_TRUE(m.albumPlayed(idx->track(3).album));
}

// The index rebuilt under the walk (a file more: another count and stamp)
// after each number of slices: Aborted at the next step while it worked;
// begun again over the new index, that index's map.
void test_an_index_rebuilt_aborts() {
  const std::vector<std::string> paths = libraryPaths(12, 4);
  LibraryIndex idx;
  uint32_t aborted = 0;
  for (int at = 0; at < 40; ++at) {
    idx.clear();
    TEST_ASSERT_TRUE(idx.begin("/music"));
    for (const std::string& p : paths) idx.addFile(p.c_str());
    TEST_ASSERT_TRUE(idx.finish());
    const uint32_t s7 = idx.findTrack(paths[7].c_str());
    PlayedSet played({hashOf(idx, s7)});
    WantedSet ws(2);
    ws.w.add(hashOf(idx, s7));
    ws.w.seal();
    Block bits(sm::albumBytes(idx.albumCount()));
    Block fold(Walk::bytesFor(Walk::foldersFor(idx)));
    sm::Map m;
    m.setConfig(sm::Map::Config{2, 3});
    m.begin(&idx, &played.p, &ws.w, bits.p(), fold.p());
    for (int k = 0; k < at && m.status() == sm::Map::Status::Working; ++k) m.step(&idx);
    const bool working = m.status() == sm::Map::Status::Working;
    idx.clear();
    TEST_ASSERT_TRUE(idx.begin("/music"));
    idx.addFile("/music/Aaa/Aaa/00 First.mp3");
    for (const std::string& p : paths) idx.addFile(p.c_str());
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_EQUAL_INT(st(working ? sm::Map::Status::Aborted : sm::Map::Status::Ready), st(m.step(&idx)));
    aborted += working ? 1 : 0;
    Block bits2(sm::albumBytes(idx.albumCount()));
    Block fold2(Walk::bytesFor(Walk::foldersFor(idx)));
    m.begin(&idx, &played.p, &ws.w, bits2.p(), fold2.p());
    TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, &idx)));
    const uint32_t t7 = idx.findTrack(paths[7].c_str());
    TEST_ASSERT_TRUE(t7 != s7);
    TEST_ASSERT_EQUAL_UINT32(t7, ws.w.idOf(hashOf(idx, t7)));
    TEST_ASSERT_TRUE(m.albumPlayed(idx.track(t7).album));
    TEST_ASSERT_EQUAL_UINT32(1, m.stats().albumsPlayed);
  }
  TEST_ASSERT_TRUE(aborted >= 20);
  // No index: Aborted at once.
  sm::Map m;
  TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Aborted), st(m.begin(nullptr, nullptr, nullptr, nullptr, nullptr)));
}

// The wanted set: sealed sorted and each hash once, across chunks; full.
void test_the_wanted_set() {
  WantedSet ws(3000);
  TEST_ASSERT_EQUAL_UINT32(3, ws.w.chunks);
  std::mt19937_64 rng(11);
  std::vector<uint64_t> v;
  for (int i = 0; i < 2600; ++i) v.push_back(rng() % 2000);  // (duplicates among them)
  for (uint64_t h : v) TEST_ASSERT_TRUE(ws.w.add(h));
  ws.w.seal();
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  TEST_ASSERT_EQUAL_UINT32(v.size(), ws.w.n);
  for (uint32_t i = 0; i < ws.w.n; ++i) {
    TEST_ASSERT_TRUE(ws.w.hashAt(i) == v[i]);
    TEST_ASSERT_EQUAL_UINT32(sm::kNoId, ws.w.idAt(i));
    TEST_ASSERT_EQUAL_INT64(i, ws.w.find(v[i]));
  }
  TEST_ASSERT_EQUAL_INT64(-1, ws.w.find(5000));
  WantedSet one(1);
  for (uint32_t i = 0; i < sm::kChunkRows; ++i) TEST_ASSERT_TRUE(one.w.add(i));
  TEST_ASSERT_FALSE(one.w.add(99999));
}


// Not played yet over statsref's fixtures: the library (stats/library.json)
// indexed, each boot's card ranked for the played set, the albums whose bit
// stays clear in albumsAZ() order: the boot's `notPlayed`. Since S3a's V2
// each card is ranked three ways on the boot's day, S2's u64 set (Hw's),
// the u40 set (a visit's Full grade) and the played-set-only rank's u40 set
// (the lean ask's), and walked over each (Map::begin() over a Played40 for
// the two): the album bits equal, each statsref's.
void test_not_played_yet_as_statsref_has_it() {
  minijson::Value lib;
  TEST_ASSERT_TRUE(cardfixtures::json("stats/library.json", &lib));
  LibraryIndex idx;
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (size_t a = 0; a < lib.size(); ++a)
    for (size_t t = 0; t < lib[a]["tracks"].size(); ++t) idx.addFile(("/music/" + lib[a]["tracks"][t]["path"].str()).c_str());
  TEST_ASSERT_TRUE(idx.finish());
  for (size_t a = 0; a < lib.size(); ++a)
    for (size_t t = 0; t < lib[a]["tracks"].size(); ++t) {
      const minijson::Value& tr = lib[a]["tracks"][t];
      const uint32_t id = idx.findTrack(("/music/" + tr["path"].str()).c_str());
      TEST_ASSERT_TRUE(id != LibraryIndex::kNone);
      TEST_ASSERT_TRUE(tr["hash"].u64() == hashOf(idx, id));  // statsref's hashes
    }
  uint32_t boots = 0;
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& bootsV = gIndex[i]["boots"];
    for (size_t b = 0; b < bootsV.size(); ++b) {
      const minijson::Value& e = bootsV[b];
      if (!e.has("lists") || !e["lists"].has("notPlayed")) continue;
      const std::string what = gIndex[i]["name"].str() + " boot " + std::to_string(b + 1);
      const Files files = bootFiles(e);
      std::string bits64;
      for (Set set : {Set::U64, Set::U40, Set::Lean}) {
        const std::string how = what + (set == Set::U64 ? " (u64)" : set == Set::U40 ? " (u40)" : " (u40, lean)");
        const std::unique_ptr<Ranked> r = rankOf(files, todayOf(e["today"]), set, how);
        Block bits(sm::albumBytes(idx.albumCount()));
        Block folders(Walk::bytesFor(Walk::foldersFor(idx)));
        sm::Map m;
        if (set == Set::U64) {
          m.begin(&idx, &r->played, nullptr, bits.p(), folders.p());
        } else {
          TEST_ASSERT_EQUAL_UINT32_MESSAGE(r->rep.played, r->played40.n, how.c_str());
          m.begin(&idx, &r->played40, nullptr, bits.p(), folders.p());
        }
        TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, &idx)));
        std::vector<std::string> got;
        const LibraryIndex::Span az = idx.albumsAZ();
        for (uint32_t k = 0; k < az.count; ++k)
          if (!m.albumPlayed(az[k])) got.push_back(idx.albumName(az[k]));
        const minijson::Value& want = e["lists"]["notPlayed"];
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), got.size(), how.c_str());
        for (size_t k = 0; k < got.size(); ++k)
          TEST_ASSERT_EQUAL_STRING_MESSAGE(want[k].c_str(), got[k].c_str(), how.c_str());
        if (set == Set::U64) {
          bits64 = bitsOf(m);
        } else {
          TEST_ASSERT_EQUAL_STRING_MESSAGE(bits64.c_str(), bitsOf(m).c_str(), how.c_str());
        }
      }
      ++boots;
    }
  }
  TEST_ASSERT_TRUE(boots >= 25);
  printf("[map] Not played yet as statsref has it: %u boots over its %u albums, from the u64 set, the u40 set and "
         "the lean rank's u40 set alike\n",
         static_cast<unsigned>(boots), static_cast<unsigned>(idx.albumCount()));
}

// The rank scenarios (stats/rank.json, up to 781 played hashes) have no
// library, so each is given one: 5 tracks an album; every fifth album with
// no played track; the others' tracks taking the scenario's played hashes
// (the u64 rank's, ascending; those left over are orphans) or unplayed
// ones (random, none sharing a played hash's top 40 bits), injected by the
// hook (no folders' states). The album bits over the u40 set, the full
// rank's and the lean rank's, equal the u64 set's, statsref's (an album
// played when one of its tracks' top 40 bits is a key of rank.json's
// played40) and a count made track by track.
void test_the_u40_album_bits_on_the_rank_scenarios() {
  std::mt19937_64 rng(40);
  for (size_t i = 0; i < gRank.size(); ++i) {
    const minijson::Value& sc = gRank[i];
    const std::string what = sc["name"].str();
    const Files files = scenarioFiles(sc);
    const int today = todayOf(sc["today"]);
    const std::unique_ptr<Ranked> r64 = rankOf(files, today, Set::U64, what + " (u64)");
    const std::unique_ptr<Ranked> r40 = rankOf(files, today, Set::U40, what + " (u40)");
    const std::unique_ptr<Ranked> lean = rankOf(files, today, Set::Lean, what + " (u40, lean)");
    std::vector<uint64_t> played;
    for (uint32_t k = 0; k < r64->played.n; ++k) played.push_back(r64->played.at(k));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sc["rows"]["played"].u64(), played.size(), what.c_str());
    const std::vector<uint64_t> refKeys = keysOf(sc["played40"].str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(played.size(), refKeys.size(), what.c_str());
    const std::set<uint64_t> exact(played.begin(), played.end());
    const std::set<uint64_t> keys(refKeys.begin(), refKeys.end());
    // The library: twice the played hashes' tracks, and 400 more.
    const uint32_t tracks = 2 * static_cast<uint32_t>(played.size()) + 400;
    std::vector<std::string> paths;
    char b[96];
    for (uint32_t t = 0; t < tracks; ++t) {
      snprintf(b, sizeof(b), "/music/Artist %u/Album %04u/%02u.mp3", static_cast<unsigned>(t / 5 % 9),
               static_cast<unsigned>(t / 5), static_cast<unsigned>(t % 5));
      paths.push_back(b);
    }
    const auto idx = libraryOf(paths);
    std::map<uint32_t, uint64_t> given;
    std::vector<uint8_t> exactPlayed(idx->albumCount(), 0), refPlayed(idx->albumCount(), 0);
    size_t next = 0;
    for (uint32_t t = 0; t < idx->trackCount(); ++t) {
      const uint32_t a = idx->track(t).album;
      uint64_t h = 0;
      if (a % 5 != 0 && next < played.size() && rng() % 4 != 0) {
        h = played[next++];
      } else {
        do {
          h = rng();
        } while (keys.count(sr::Played40::keyOf(h)) != 0);
      }
      given[t] = h;
      if (exact.count(h)) exactPlayed[a] = 1;
      if (keys.count(sr::Played40::keyOf(h))) refPlayed[a] = 1;
    }
    uint32_t albumsPlayed = 0;
    for (uint32_t a = 0; a < idx->albumCount(); ++a) {
      TEST_ASSERT_EQUAL_MESSAGE(exactPlayed[a], refPlayed[a], what.c_str());  // (no top-40 collision here)
      albumsPlayed += exactPlayed[a];
    }
    TEST_ASSERT_TRUE_MESSAGE(albumsPlayed > 0 && albumsPlayed < idx->albumCount(), what.c_str());
    std::string bits64;
    for (Set set : {Set::U64, Set::U40, Set::Lean}) {
      const std::string how = what + (set == Set::U64 ? " (u64)" : set == Set::U40 ? " (u40)" : " (u40, lean)");
      Block bits(sm::albumBytes(idx->albumCount()));
      sm::Map m;
      m.setPathHash(injected, &given);
      if (set == Set::U64) {
        m.begin(idx.get(), &r64->played, nullptr, bits.p(), nullptr);
      } else {
        const sr::Played40& p40 = set == Set::U40 ? r40->played40 : lean->played40;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(played.size(), p40.n, how.c_str());
        m.begin(idx.get(), &p40, nullptr, bits.p(), nullptr);
      }
      TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, idx.get())));
      for (uint32_t a = 0; a < idx->albumCount(); ++a)
        TEST_ASSERT_EQUAL_MESSAGE(exactPlayed[a] != 0, m.albumPlayed(a), how.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(albumsPlayed, m.stats().albumsPlayed, how.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, m.stats().playedTracks, how.c_str());
      if (set == Set::U64) {
        bits64 = bitsOf(m);
      } else {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bits64.c_str(), bitsOf(m).c_str(), how.c_str());
      }
    }
    printf("[map] %s: %u played hashes, %u of them on %u tracks; %u of %u albums played, from the u64 set, the u40 "
           "set and the lean rank's alike\n",
           what.c_str(), static_cast<unsigned>(played.size()), static_cast<unsigned>(next),
           static_cast<unsigned>(idx->trackCount()), static_cast<unsigned>(albumsPlayed),
           static_cast<unsigned>(idx->albumCount()));
  }
}

// Call 3 ("5-byte fingerprints", 4.3): the u40 set matches a hash by its
// top 40 bits. An unplayed track whose hash shares them with a played one
// (bit 23 apart: the highest the key leaves out) reads as played, so its
// album, with no played track, is hidden from Not played yet: that album
// alone. Such a track inside a played album changes nothing; one apart in
// bit 24 (the lowest the key holds) hides nothing; every album with a
// played track keeps its bit (a played album is never shown). The u64 set
// (Hw's) shows the album; the wanted set's ids stay exact either way.
void test_a_top_40_collision_hides_exactly_its_album() {
  const auto idx = libraryOf(libraryPaths(12, 4));
  TEST_ASSERT_EQUAL_UINT32(12, idx->albumCount());
  std::vector<std::vector<uint32_t>> of(idx->albumCount());  // each album's tracks
  for (uint32_t t = 0; t < idx->trackCount(); ++t) of[idx->track(t).album].push_back(t);
  // Played: the first track of every third album (0, 3, 6, 9).
  std::vector<uint64_t> playedHashes;
  for (uint32_t a = 0; a < idx->albumCount(); a += 3) playedHashes.push_back(hashOf(*idx, of[a][0]));
  TEST_ASSERT_EQUAL_UINT32(4, playedHashes.size());
  const uint32_t x = of[1][1];  // album 1 (none played): album 0's played hash, bit 23 apart
  const uint32_t y = of[3][1];  // album 3 (played): album 6's, its low 24 bits apart
  const uint32_t z = of[2][0];  // album 2 (none played): album 9's, bit 24 apart
  std::map<uint32_t, uint64_t> given;
  given[x] = playedHashes[0] ^ (1ull << 23);
  given[y] = playedHashes[2] ^ 0xFFFFFFull;
  given[z] = playedHashes[3] ^ (1ull << 24);
  TEST_ASSERT_TRUE(sr::Played40::keyOf(given[x]) == sr::Played40::keyOf(playedHashes[0]));
  TEST_ASSERT_TRUE(sr::Played40::keyOf(given[y]) == sr::Played40::keyOf(playedHashes[2]));
  TEST_ASSERT_TRUE(sr::Played40::keyOf(given[z]) != sr::Played40::keyOf(playedHashes[3]));
  const PlayedSet p64(playedHashes);
  const Played40Set p40(playedHashes);
  std::vector<std::string> shown[2];  // Not played yet: the u64 set's, the u40 set's
  for (int u40 = 0; u40 < 2; ++u40) {
    const char* how = u40 ? "u40" : "u64";
    WantedSet ws(8);
    for (uint64_t h : {playedHashes[0], given[x], given[z]}) TEST_ASSERT_TRUE(ws.w.add(h));
    ws.w.seal();
    Block bits(sm::albumBytes(idx->albumCount()));
    sm::Map m;
    m.setPathHash(injected, &given);
    if (u40) {
      m.begin(idx.get(), &p40.p, &ws.w, bits.p(), nullptr);
    } else {
      m.begin(idx.get(), &p64.p, &ws.w, bits.p(), nullptr);
    }
    TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, idx.get())));
    for (uint32_t a = 0; a < idx->albumCount(); ++a) {
      const bool hasPlay = a % 3 == 0;
      if (hasPlay) TEST_ASSERT_TRUE_MESSAGE(m.albumPlayed(a), how);  // never shown
      TEST_ASSERT_EQUAL_MESSAGE(hasPlay || (u40 && a == 1), m.albumPlayed(a), how);
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(u40 ? 5 : 4, m.stats().albumsPlayed, how);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(u40 ? 6 : 4, m.stats().playedTracks, how);  // (x and y read as played)
    // The ids are the wanted set's, exact: x's hash is x's, not album 0's.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(of[0][0], ws.w.idOf(playedHashes[0]), how);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(x, ws.w.idOf(given[x]), how);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(z, ws.w.idOf(given[z]), how);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3, m.stats().wantedMapped, how);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, m.stats().collisions, how);
    const LibraryIndex::Span az = idx->albumsAZ();
    for (uint32_t k = 0; k < az.count; ++k)
      if (!m.albumPlayed(az[k])) shown[u40].push_back(idx->albumName(az[k]));
  }
  // The u40 set's list is the u64 set's less album 1.
  TEST_ASSERT_EQUAL_UINT32(shown[0].size() - 1, shown[1].size());
  std::vector<std::string> less = shown[0];
  const auto hidden = std::find(less.begin(), less.end(), std::string(idx->albumName(1)));
  TEST_ASSERT_TRUE(hidden != less.end());
  less.erase(hidden);
  TEST_ASSERT_TRUE(less == shown[1]);
}

// Wanted::rowsCap (S3a, 9.7's grades): the wanted set in one region (the
// visit's: the lists' 696 rows, 8,352 B, in A's tail), for fewer rows than
// a chunk's 1,365 and past them (whole chunks over the region, contiguous):
// full at its rows, sealed in place, nothing written past it. rowsCap 0:
// whole chunks (S2's); it only ever lowers the chunks' cap.
void test_the_wanted_rows_cap() {
  TEST_ASSERT_EQUAL_UINT32(696, sr::Ranker::kEntries);
  TEST_ASSERT_EQUAL_UINT32(8352, sm::Wanted::bytesFor(sr::Ranker::kEntries));
  std::mt19937_64 rng(696);
  const uint32_t sizes[] = {1, 2, 674, 696, sm::kChunkRows - 1, sm::kChunkRows, sm::kChunkRows + 1, 3000};
  for (uint32_t rows : sizes) {
    const std::string what = std::to_string(rows) + " rows";
    const size_t bytes = sm::Wanted::bytesFor(rows);
    TEST_ASSERT_EQUAL_UINT32(static_cast<size_t>(rows) * sm::kRowBytes, bytes);
    std::vector<uint8_t> mem(bytes + 64, 0xA5);
    sm::Wanted w;
    TEST_ASSERT_TRUE_MESSAGE(w.region(mem.data(), rows), what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows, w.rowsCap, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sm::wantedChunks(rows), w.chunks, what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(rows, w.cap(), what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, w.n, what.c_str());
    std::vector<uint64_t> v;
    for (uint32_t i = 0; i < rows; ++i) {
      const uint64_t h = i % 7 == 6 ? v[rng() % v.size()] : rng();  // (some twice)
      v.push_back(h);
      TEST_ASSERT_TRUE_MESSAGE(w.add(h), what.c_str());
      TEST_ASSERT_TRUE_MESSAGE(w.row(i) == mem.data() + static_cast<size_t>(i) * sm::kRowBytes, what.c_str());
    }
    TEST_ASSERT_FALSE_MESSAGE(w.add(1), what.c_str());  // full at its rows
    w.seal();
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(v.size(), w.n, what.c_str());
    for (uint32_t i = 0; i < w.n; ++i) {
      TEST_ASSERT_TRUE_MESSAGE(w.hashAt(i) == v[i], what.c_str());
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(sm::kNoId, w.idAt(i), what.c_str());
      TEST_ASSERT_EQUAL_INT64_MESSAGE(i, w.find(v[i]), what.c_str());
    }
    for (size_t k = bytes; k < mem.size(); ++k) TEST_ASSERT_EQUAL_HEX8_MESSAGE(0xA5, mem[k], what.c_str());
  }
  // No rows: empty. Rows with no region, or past every chunk: refused, empty.
  std::vector<uint8_t> some(64);
  sm::Wanted none;
  TEST_ASSERT_TRUE(none.region(some.data(), 0));
  TEST_ASSERT_EQUAL_UINT32(0, none.cap());
  TEST_ASSERT_FALSE(none.add(1));
  TEST_ASSERT_FALSE(none.region(nullptr, 5));
  TEST_ASSERT_EQUAL_UINT32(0, none.cap());
  TEST_ASSERT_FALSE(none.region(some.data(), sm::kMaxChunks * sm::kChunkRows + 1));
  TEST_ASSERT_EQUAL_UINT32(0, none.cap());
  TEST_ASSERT_EQUAL_UINT32(0, none.chunks);
  // S2's chunks: rowsCap 0, whole chunks; rowsCap lowers the cap, never
  // raises it past the chunks'.
  WantedSet ws(3000);
  TEST_ASSERT_EQUAL_UINT32(0, ws.w.rowsCap);
  TEST_ASSERT_EQUAL_UINT32(3 * sm::kChunkRows, ws.w.cap());
  ws.w.rowsCap = 100;
  TEST_ASSERT_EQUAL_UINT32(100, ws.w.cap());
  ws.w.rowsCap = 4 * sm::kChunkRows;
  TEST_ASSERT_EQUAL_UINT32(3 * sm::kChunkRows, ws.w.cap());
}

namespace {

// S2's seal() (lib/core/StatsMap.cpp until S3a's V2), kept as the
// reference: an insertion sort over the chunked rows, then each hash once,
// every id kNoId.
void insertionSeal(sm::Wanted& w) {
  for (uint32_t i = 1; i < w.n; ++i) {
    const uint64_t h = w.hashAt(i);
    uint32_t j = i;
    while (j > 0 && w.hashAt(j - 1) > h) {
      std::memcpy(w.row(j), w.row(j - 1), sm::kRowBytes);
      --j;
    }
    cc::put64(w.row(j), h);
    cc::put32(w.row(j) + 8, sm::kNoId);
  }
  uint32_t out = 0;
  for (uint32_t i = 0; i < w.n; ++i) {
    if (out > 0 && w.hashAt(out - 1) == w.hashAt(i)) continue;
    if (out != i) std::memcpy(w.row(out), w.row(i), sm::kRowBytes);
    w.setId(out++, sm::kNoId);
  }
  w.n = out;
}

// A sealed set's rows, byte for byte.
std::string rowBytes(const sm::Wanted& w) {
  std::string s;
  for (uint32_t i = 0; i < w.n; ++i) s.append(reinterpret_cast<const char*>(w.row(i)), sm::kRowBytes);
  return s;
}

// Every list's hashes in rank order, as finishRank() adds them (S2's Hw).
std::vector<uint64_t> listHashes(const sr::Ranker& k) {
  std::vector<uint64_t> v;
  for (int l = 0; l < sr::kLists; ++l)
    for (uint32_t i = 0; i < k.size(static_cast<sr::List>(l)); ++i) v.push_back(k.at(static_cast<sr::List>(l), i).hash);
  return v;
}

// `hashes` added in their order, each row's id then set to its place
// (seal() gives every row kNoId), and sealed three ways: seal() over S2's
// whole chunks and over one region of rowsCap rows (S3a's), and the
// insertion sort over whole chunks. The three byte-equal; sorted, each hash
// once. `us`: the host's µs of seal() and of the insertion sort.
void sameSeal(const std::vector<uint64_t>& hashes, const std::string& what, long long* us = nullptr) {
  const uint32_t rows = static_cast<uint32_t>(hashes.size());
  WantedSet heap(rows), ref(rows);
  std::vector<uint8_t> mem(sm::Wanted::bytesFor(rows) + 16, 0x5C);
  sm::Wanted one;
  TEST_ASSERT_TRUE_MESSAGE(one.region(mem.data(), rows), what.c_str());
  for (sm::Wanted* w : {&heap.w, &ref.w, &one}) {
    for (uint64_t h : hashes) TEST_ASSERT_TRUE_MESSAGE(w->add(h), what.c_str());
    for (uint32_t i = 0; i < w->n; ++i) w->setId(i, i);
  }
  const auto t0 = std::chrono::steady_clock::now();
  heap.w.seal();
  const auto t1 = std::chrono::steady_clock::now();
  insertionSeal(ref.w);
  const auto t2 = std::chrono::steady_clock::now();
  one.seal();
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.w.n, heap.w.n, what.c_str());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(ref.w.n, one.n, what.c_str());
  TEST_ASSERT_TRUE_MESSAGE(rowBytes(ref.w) == rowBytes(heap.w), what.c_str());
  TEST_ASSERT_TRUE_MESSAGE(rowBytes(ref.w) == rowBytes(one), what.c_str());
  for (size_t k = sm::Wanted::bytesFor(rows); k < mem.size(); ++k)
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x5C, mem[k], what.c_str());
  std::vector<uint64_t> v(hashes);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(v.size(), heap.w.n, what.c_str());
  for (uint32_t i = 0; i < heap.w.n; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(heap.w.hashAt(i) == v[i], what.c_str());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(sm::kNoId, heap.w.idAt(i), what.c_str());
  }
  if (us) {
    us[0] = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    us[1] = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
  }
}

}  // namespace

// seal() as a heap sort (S3a's V2: in place over the chunked rows, no
// recursion, as the save's sort is), then the same dedup: the sealed set
// equal to the insertion sort's (S2's seal()) on every fixture's lists
// (each boot's and each rank scenario's, every list's hashes in rank
// order, as finishRank() adds them; rank-big's 696, every list past its cap
// and 16), on reversed input (and reversed with every hash twice), on equal
// hashes (all, and three values across chunks), sorted input, none and one.
void test_seal_is_the_insertion_sort_s() {
  uint32_t cards = 0;
  size_t most = 0;
  long long us[2] = {};
  for (size_t i = 0; i < gIndex.size(); ++i) {
    const minijson::Value& bootsV = gIndex[i]["boots"];
    for (size_t b = 0; b < bootsV.size(); ++b) {
      const minijson::Value& e = bootsV[b];
      if (!e.has("lists")) continue;
      const std::string what = gIndex[i]["name"].str() + " boot " + std::to_string(b + 1);
      const std::unique_ptr<Ranked> r = rankOf(bootFiles(e), todayOf(e["today"]), Set::U64, what);
      sameSeal(listHashes(r->ranker), what);
      ++cards;
    }
  }
  for (size_t i = 0; i < gRank.size(); ++i) {
    const minijson::Value& sc = gRank[i];
    const std::string what = sc["name"].str();
    const std::unique_ptr<Ranked> r = rankOf(scenarioFiles(sc), todayOf(sc["today"]), Set::U64, what);
    const std::vector<uint64_t> v = listHashes(r->ranker);
    long long t[2] = {};
    sameSeal(v, what, t);
    if (v.size() > most) {
      most = v.size();
      us[0] = t[0];
      us[1] = t[1];
    }
    ++cards;
  }
  TEST_ASSERT_TRUE(cards >= 33);
  TEST_ASSERT_EQUAL_UINT32(sr::Ranker::kEntries, most);
  for (uint32_t n : {2u, 3u, 674u, 696u, 2600u}) {
    std::vector<uint64_t> down, twice;
    for (uint32_t k = n; k-- > 0;) {
      const uint64_t h = 0x0123456789ABCDEFull + k * 0x1000001ull;
      down.push_back(h);
      twice.push_back(h);
      twice.push_back(h);
    }
    sameSeal(down, "reversed, " + std::to_string(n));
    sameSeal(twice, "reversed, each twice, " + std::to_string(n));
    std::vector<uint64_t> up(down.rbegin(), down.rend());
    sameSeal(up, "sorted, " + std::to_string(n));
    sameSeal(std::vector<uint64_t>(n, 0xDEADBEEFull), "equal, " + std::to_string(n));
  }
  std::mt19937_64 rng(3);
  const uint64_t three[] = {0, UINT64_MAX, 1ull << 63};
  std::vector<uint64_t> few;
  for (int k = 0; k < 2600; ++k) few.push_back(three[rng() % 3]);
  sameSeal(few, "three values over 2 chunks");
  sameSeal({}, "none");
  sameSeal({42}, "one");
  printf("[map] seal(): the insertion sort's on %u cards' lists, reversed, equal and sorted inputs; for %u rows "
         "%lld us, the insertion sort %lld us, on the host\n",
         static_cast<unsigned>(cards), static_cast<unsigned>(most), us[0], us[1]);
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  if (!cardfixtures::json("stats/index.json", &gIndex) || gIndex.size() == 0 ||
      !cardfixtures::json("stats/rank.json", &gRank) || gRank.size() == 0) {
    printf("the stats fixtures (test/fixtures/card/stats) can't be read\n");
    return 1;
  }
  UNITY_BEGIN();
  RUN_TEST(test_every_hash_is_track_path_hash);
  RUN_TEST(test_the_rule);
  RUN_TEST(test_past_the_folders_limit);
  RUN_TEST(test_matches);
  RUN_TEST(test_the_map);
  RUN_TEST(test_a_collision_maps_neither);
  RUN_TEST(test_an_index_rebuilt_aborts);
  RUN_TEST(test_the_wanted_set);
  RUN_TEST(test_not_played_yet_as_statsref_has_it);
  RUN_TEST(test_the_u40_album_bits_on_the_rank_scenarios);
  RUN_TEST(test_a_top_40_collision_hides_exactly_its_album);
  RUN_TEST(test_the_wanted_rows_cap);
  RUN_TEST(test_seal_is_the_insertion_sort_s);
  return UNITY_END();
}
