// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PathHashWalk.h"

#include <cstring>

#include "CardContract.h"

namespace pathhash {

namespace cc = cardcontract;

bool trackPathHash(const LibraryIndex& index, uint32_t track, char* path, size_t size, uint64_t* hash) {
  const size_t n = index.trackPath(track, path, size);
  if (n == 0) return false;
  static const char kRoot[] = "/music/";
  const size_t k = sizeof(kRoot) - 1;
  bool root = n > k;
  for (size_t i = 0; i < k && root; ++i) {
    const char c = path[i];
    root = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == kRoot[i];
  }
  *hash = root ? cc::pathHash(path + k, n - k) : cc::fnv1a64(path, n);
  return true;
}

void Walk::begin(const LibraryIndex* index) {
  index_ = index;
  if (!folderHash_) folderCount_ = 0;
  nextFolder_ = nextTrack_ = 0;
}

void Walk::end() {
  index_ = nullptr;
  folderHash_ = nullptr;
  folderCount_ = nextFolder_ = 0;
}

void Walk::foldersSlice(uint32_t n) {
  if (n == 0) n = 1;
  if (!folderHash_) return;
  const uint32_t end = folderCount_ - nextFolder_ > n ? nextFolder_ + n : folderCount_;
  uint16_t* const len = lens();
  for (uint32_t f = nextFolder_; f < end; ++f) {
    // trackPathHash()'s rule on the folder's path: a track's is it, '/'
    // and its name, so "/music" (any case) alone or before a '/' starts
    // pathHash()'s (from the root's literal lowercase), any other its bytes'.
    const size_t n2 = index_->folderPath(f, path_, sizeof(path_));
    if (n2 == 0) {
      len[f] = kNoPath;
      continue;
    }
    static const char kRoot[] = "/music";
    bool root = n2 >= 6 && (n2 == 6 || path_[6] == '/');
    for (size_t i = 0; i < 6 && root; ++i) {
      const char c = path_[i];
      root = (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c) == kRoot[i];
    }
    folderHash_[f] = root ? cc::fnv1a64(path_ + 6, n2 - 6, cc::kMusicHash) : cc::fnv1a64(path_, n2);
    len[f] = static_cast<uint16_t>(n2);
  }
  nextFolder_ = end;
}

bool Walk::trackHash(uint32_t t, uint64_t* h) {
  if (!folderHash_) return hash_(*index_, t, path_, sizeof(path_), h, hashCtx_);
  const LibraryIndex::Track& track = index_->track(t);
  const char* name = index_->str(track.name);
  const size_t len = std::strlen(name);
  // (An empty name, which no file has: trackPathHash()'s own way.)
  if (len == 0) return hash_(*index_, t, path_, sizeof(path_), h, hashCtx_);
  if (track.folder >= folderCount_ || lens()[track.folder] == kNoPath) return false;
  // trackPath()'s: the folder's path, '/', the name and a NUL in kPathBytes.
  if (lens()[track.folder] + 1 + len + 1 > kPathBytes) return false;
  *h = cc::fnv1a64(name, len, cc::fnv1a64("/", 1, folderHash_[track.folder]));
  return true;
}

bool Walk::hashSlice(uint32_t n, VisitFn visit, void* ctx) {
  if (n == 0) n = 1;
  const uint32_t tracks = index_ ? index_->trackCount() : 0;
  const uint32_t end = tracks - nextTrack_ > n ? nextTrack_ + n : tracks;
  const bool worked = nextTrack_ < end;
  for (uint32_t t = nextTrack_; t < end; ++t) {
    uint64_t h;
    if (trackHash(t, &h)) visit(ctx, t, h);
  }
  nextTrack_ = end;
  return worked;
}

}  // namespace pathhash
