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
// Run: pio test -e native -f test_stats_map
#include <unity.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "../support/CardFixtures.h"
#include "../support/CutFs.h"
#include "CardContract.h"
#include "LibraryIndex.h"
#include "PathHashWalk.h"
#include "StatsFile.h"
#include "StatsMap.h"
#include "StatsRank.h"

namespace cc = cardcontract;
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
// stays clear in albumsAZ() order: the boot's `notPlayed`.
void test_not_played_yet_as_statsref_has_it() {
  minijson::Value lib, index;
  TEST_ASSERT_TRUE(cardfixtures::json("stats/library.json", &lib));
  TEST_ASSERT_TRUE(cardfixtures::json("stats/index.json", &index));
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
  for (size_t i = 0; i < index.size(); ++i) {
    const minijson::Value& bootsV = index[i]["boots"];
    for (size_t b = 0; b < bootsV.size(); ++b) {
      const minijson::Value& e = bootsV[b];
      if (!e.has("lists") || !e["lists"].has("notPlayed")) continue;
      const std::string what = index[i]["name"].str() + " boot " + std::to_string(b + 1);
      cutfs::CutFs card;
      for (const auto& m : e["out"].members) {
        const std::string bytes = cardfixtures::bytes("stats/" + m.second.str() + "/" + m.first);
        card.put("/.player/" + m.first, std::vector<uint8_t>(bytes.begin(), bytes.end()));
      }
      statsfold::Fold fold;
      sx::Store store(card);
      Block work(sx::kWorkBytes);
      TEST_ASSERT_TRUE_MESSAGE(sx::statsOn(store.read(fold, work.p(), nullptr).read), what.c_str());
      sr::Input in;
      in.present = store.cardCopy().present;
      in.generation = store.cardCopy().generation;
      in.headerCrc = store.cardCopy().headerCrc;
      in.baseGen = fold.head().baseGen;
      in.baseRows = fold.head().baseRows;
      in.baseCrc = fold.head().baseCrc;
      Block lists(sr::Ranker::kBytes), win(sr::Windows::bytesFor(4096));
      PlayedSet played({});
      Block chunk(sr::kPlayedChunkBytes);
      played.p.chunk[0] = reinterpret_cast<uint64_t*>(chunk.p());
      played.p.chunks = 1;
      sr::Memory mem;
      mem.work = work.p();
      mem.lists = lists.p();
      mem.windows = win.p();
      mem.windowRows = 4096;
      mem.played = &played.p;
      sr::Ranker r;
      TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", sr::outcomeName(sr::rank(card, in, mem, r).outcome), what.c_str());
      Block bits(sm::albumBytes(idx.albumCount()));
      Block fold2(Walk::bytesFor(Walk::foldersFor(idx)));
      sm::Map m;
      m.begin(&idx, &played.p, nullptr, bits.p(), fold2.p());
      TEST_ASSERT_EQUAL_INT(st(sm::Map::Status::Ready), st(runMap(m, &idx)));
      std::vector<std::string> got;
      const LibraryIndex::Span az = idx.albumsAZ();
      for (uint32_t k = 0; k < az.count; ++k)
        if (!m.albumPlayed(az[k])) got.push_back(idx.albumName(az[k]));
      const minijson::Value& want = e["lists"]["notPlayed"];
      TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), got.size(), what.c_str());
      for (size_t k = 0; k < got.size(); ++k)
        TEST_ASSERT_EQUAL_STRING_MESSAGE(want[k].c_str(), got[k].c_str(), what.c_str());
      ++boots;
    }
  }
  TEST_ASSERT_TRUE(boots >= 25);
  printf("[map] Not played yet as statsref has it: %u boots over its %u albums\n", static_cast<unsigned>(boots),
         static_cast<unsigned>(idx.albumCount()));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
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
  return UNITY_END();
}
