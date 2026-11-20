// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"

// A library's track path hashes (AUTODJ-INDEX.md 2.3: FNV-1a 64 of
// "/music/..."), walked in slices: Auto DJ's join and the listening stats'
// map take every track's hash this one way (docs/LISTENING-STATS.md 6.2).
// Portable, host-tested (test_stats_map; test_autodj_join through the
// join).
//
// Two phases, each in slices the caller sizes:
//   Folders  each library folder's path hashed once, with its length (10
//            bytes a folder, in one block the caller gives: bytesFor()).
//            FNV-1a 64 is sequential, so a track's hash goes on from its
//            folder's with "/" and its name: the hashing is about 3.3 times
//            faster than each track's whole path (the 2026-10-14 device
//            check's join was CPU-bound).
//   Hash     each track's hash, from its folder's state, handed to the
//            caller's visitor with its id; a track without one (its path
//            doesn't fit kPathBytes) counted, not handed.
// With no block (none given, or a library of more than kMaxFolders
// folders) every track's whole path is hashed through the hook
// (trackPathHash() by default), and Folders has nothing to do.
//
// The walk holds no index: begin() is given it, and each slice reads it as
// it was then. The caller checks matches() (or its own check) before each
// slice: a walk over an index that changed or went reads freed memory.
namespace pathhash {

// The path hash of the track's path as the index writes it: "/music/" (any
// case) and the rest is cardcontract::pathHash()'s (the root's literal
// lowercase), any other path its bytes' FNV-1a 64. Through `path` (`size`
// bytes). False: no path (it doesn't fit).
bool trackPathHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash);

class Walk {
public:
  // A track's hash when its folder has no state (the default:
  // trackPathHash()); Auto DJ's join passes its own (the tests' injected
  // hashes).
  using HashFn = bool (*)(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash,
                          void* ctx);
  // The Hash phase's visitor: a track and its hash.
  using VisitFn = void (*)(void* ctx, uint32_t track, uint64_t hash);

  static constexpr size_t kPathBytes = 512;  // the path buffer: trackPath()'s and folderPath()'s
  // The folders' states up to this many folders (an 80 KB block).
  static constexpr uint32_t kMaxFolders = 8192;
  static constexpr size_t kFolderBytes = sizeof(uint64_t) + sizeof(uint16_t);  // 10 B a folder

  // The block that holds `folders` folders' states.
  static size_t bytesFor(uint32_t folders) { return static_cast<size_t>(folders) * kFolderBytes; }
  // The folders' states `index` takes: its folderCount(), or 0 past
  // kMaxFolders (each track's whole path then).
  static uint32_t foldersFor(const LibraryIndex& index) {
    return index.folderCount() <= kMaxFolders ? index.folderCount() : 0;
  }

  Walk() = default;
  Walk(const Walk&) = delete;
  Walk& operator=(const Walk&) = delete;

  // The hook for a track whose folder has no state; nullptr: trackPathHash().
  void setHash(HashFn fn, void* ctx) {
    hash_ = fn ? fn : defaultHash;
    hashCtx_ = ctx;
  }

  // Starts over `index` (ready), with `folders` folders' states in `block`
  // (bytesFor(folders) of it: foldersFor(*index), or 0; nullptr or 0: none,
  // each track's whole path). The block stays the caller's.
  void begin(const LibraryIndex* index, uint32_t folders, void* block);
  // Forgets the index and the block (a walk released or aborted).
  void end();

  // Folders: up to `n` folders hashed (at least 1). Whether the phase is
  // done: foldersDone().
  void foldersSlice(uint32_t n);
  bool foldersDone() const { return nextFolder_ >= folderCount_; }
  // Hash: up to `n` tracks (at least 1), each one with a hash handed to
  // `visit`. False: no track was left (the phase was done already).
  bool hashSlice(uint32_t n, VisitFn visit, void* ctx);
  bool tracksDone() const { return nextTrack_ >= tracks_; }

  // A track's hash: from its folder's state, else the hook's. False: none.
  bool trackHash(uint32_t track, uint64_t* hash);

  // `index` is the one begin() was given, still as it was: the same object,
  // ready, the same tracks and build stamp (Join::matches()' test).
  bool matches(const LibraryIndex* index) const {
    return index_ && index == index_ && index->ready() && index->trackCount() == tracks_ &&
           index->buildStamp() == stamp_;
  }

  uint32_t folders() const { return folderCount_; }  // folders with a state (0: whole paths)
  uint32_t tracks() const { return tracks_; }
  uint32_t hashed() const { return hashed_; }      // tracks handed to the visitor
  uint32_t unhashed() const { return unhashed_; }  // tracks without a path hash

private:
  static bool defaultHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash, void*) {
    return trackPathHash(index, track, path, size, hash);
  }

  // A folder's path has none (deeper than LibraryIndex::folderPath() goes,
  // or longer than kPathBytes).
  static constexpr uint16_t kNoPath = 0xFFFF;

  HashFn hash_ = defaultHash;
  void* hashCtx_ = nullptr;
  const LibraryIndex* index_ = nullptr;
  uint32_t tracks_ = 0;
  uint64_t stamp_ = 0;
  // Each folder's path hash so far and its path's length, in one block;
  // nullptr: the hook's hash.
  uint64_t* folderHash_ = nullptr;
  uint16_t* folderLen_ = nullptr;
  uint32_t folderCount_ = 0;
  uint32_t nextFolder_ = 0;
  uint32_t nextTrack_ = 0;
  uint32_t hashed_ = 0;
  uint32_t unhashed_ = 0;
  char path_[kPathBytes] = {};
};

}  // namespace pathhash
