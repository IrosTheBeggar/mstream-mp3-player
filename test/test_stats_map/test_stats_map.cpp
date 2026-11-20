// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the listening stats' map (docs/LISTENING-STATS.md 6, 9.6's
// C11 and C13): the library's path hashes walked in slices
// (lib/core/PathHashWalk, the walk Auto DJ's join takes them by too).
// - The walk: every track's hash is trackPathHash()'s, and none where it
//   has none, whatever the slices' sizes, with the folders' states and
//   without them; a library past kMaxFolders folders takes whole paths; an
//   index rebuilt under the walk no longer matches().
// Run: pio test -e native -f test_stats_map
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "CardContract.h"
#include "LibraryIndex.h"
#include "PathHashWalk.h"

namespace cc = cardcontract;
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
    TEST_ASSERT_TRUE(w->matches(&idx));
    w->foldersSlice(folders);
    TEST_ASSERT_TRUE(++slices < 100000);
  }
  while (!w->tracksDone()) {
    TEST_ASSERT_TRUE(w->matches(&idx));
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
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), w.hashed(), root);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(idx.trackCount() - want.size(), w.unhashed(), root);
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

// matches(): the index begin() was given, as it was (rebuilt from the same
// files: the same stamp, the same ids, still); rebuilt with a file more
// (another track count and stamp), another object, none: no longer.
void test_matches() {
  LibraryIndex idx, other;
  for (LibraryIndex* x : {&idx, &other}) {
    TEST_ASSERT_TRUE(x->begin("/music"));
    x->addFile("/music/A/a.mp3");
    x->addFile("/music/B/b.mp3");
    TEST_ASSERT_TRUE(x->finish());
  }
  Walk w;
  TEST_ASSERT_FALSE(w.matches(&idx));  // (nothing begun)
  w.begin(&idx, 0, nullptr);
  TEST_ASSERT_TRUE(w.matches(&idx));
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
  w.end();
  TEST_ASSERT_FALSE(w.matches(&idx));
}

void setUp() {}
void tearDown() {}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_every_hash_is_track_path_hash);
  RUN_TEST(test_the_rule);
  RUN_TEST(test_past_the_folders_limit);
  RUN_TEST(test_matches);
  return UNITY_END();
}
