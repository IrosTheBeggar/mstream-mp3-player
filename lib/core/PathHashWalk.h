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
//            doesn't fit kPathBytes) passed over (position() counts both).
// With no block (none given, or a library of more than kMaxFolders
// folders) every track's whole path is hashed through the hook
// (trackPathHash() by default), and Folders has nothing to do.
//
// The walk holds the index begin() was given and reads it at each slice:
// the caller checks, before each one, that it is still the one it began on
// (Join::matches(), StatsMap's own: the same object, ready, the same tracks
// and build stamp); a walk over an index that changed or went reads freed
// memory. It is a member of Auto DJ's join, so it keeps nothing the join
// already kept (the join's object is the same size as before the walk: it
// sits in PSRAM from setup, where a few bytes more can move a size class).
namespace pathhash {

// The path hash of the track's path as the index writes it: "/music/" (any
// case) and the rest is cardcontract::pathHash()'s (the root's literal
// lowercase), any other path its bytes' FNV-1a 64. Through `path` (`size`
// bytes). False: no path (it doesn't fit).
bool trackPathHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash);

class Walk {
public:
  // A track's hash when its folder has no state (the default:
  // trackPathHash()); Auto DJ's join passes its tests' own (injected
  // hashes), and then keeps no folders' states.
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
  bool hashIsDefault() const { return hash_ == defaultHash; }

  // The folders' states: `folders` of them wanted (0: none, each track's
  // whole path), their block given (bytesFor(folders) of it; the caller's,
  // which may take it after wanting it: Auto DJ's join places its blocks
  // first). dropFolders(): none.
  void wantFolders(uint32_t folders) {
    folderCount_ = folders;
    folderHash_ = nullptr;
  }
  void giveBlock(void* block) { folderHash_ = static_cast<uint64_t*>(block); }
  void dropFolders() { wantFolders(0); }
  uint32_t folders() const { return folderCount_; }  // wanted (0: whole paths)
  void* block() const { return folderHash_; }

  // Starts over `index` (ready): Folders over the states wanted, if their
  // block was given (else none: whole paths), then Hash.
  void begin(const LibraryIndex* index);
  // As wantFolders(folders), giveBlock(block), begin(index).
  void begin(const LibraryIndex* index, uint32_t folders, void* block) {
    wantFolders(block ? folders : 0);
    giveBlock(folders ? block : nullptr);
    begin(index);
  }
  // Forgets the index and the folders' states (a walk released or aborted;
  // the block stays the caller's).
  void end();

  // Folders: up to `n` folders hashed (at least 1). Whether the phase is
  // done: foldersDone().
  void foldersSlice(uint32_t n);
  bool foldersDone() const { return nextFolder_ >= folderCount_ || !folderHash_; }
  // Hash: up to `n` tracks (at least 1), each one with a hash handed to
  // `visit`. False: no track was left (the phase was done already).
  bool hashSlice(uint32_t n, VisitFn visit, void* ctx);
  bool tracksDone() const { return !index_ || nextTrack_ >= index_->trackCount(); }
  // The tracks the Hash phase has passed (handed or not).
  uint32_t position() const { return nextTrack_; }

  // A track's hash: from its folder's state, else the hook's. False: none.
  bool trackHash(uint32_t track, uint64_t* hash);

private:
  static bool defaultHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash, void*) {
    return trackPathHash(index, track, path, size, hash);
  }
  // Each folder's path length after the hashes (kNoPath: it has none:
  // deeper than LibraryIndex::folderPath() goes, or longer than
  // kPathBytes).
  uint16_t* lens() const { return reinterpret_cast<uint16_t*>(folderHash_ + folderCount_); }
  static constexpr uint16_t kNoPath = 0xFFFF;

  const LibraryIndex* index_ = nullptr;
  // Each folder's path hash so far, then its path's length, in one block;
  // nullptr: the hook's hash.
  uint64_t* folderHash_ = nullptr;
  uint32_t folderCount_ = 0;
  uint32_t nextFolder_ = 0;
  uint32_t nextTrack_ = 0;
  HashFn hash_ = defaultHash;
  void* hashCtx_ = nullptr;
  char path_[kPathBytes] = {};
};

}  // namespace pathhash
