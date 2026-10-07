// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Library.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>

#include "app/Psram.h"
#include "storage/FileStream.h"

namespace {

constexpr const char* kRoot = "/music";
constexpr int kMaxDepth = 8;

uint32_t internalFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t psramFreeNow() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }
float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }

// The walk's signature: FNV-1a 64 over every path in walk order, each ended
// with '\n'. Seeded with what else shapes the index (the root, the depth).
struct PathHash {
  uint64_t h = 14695981039346656037ull;
  uint32_t files = 0;
  PathHash() {
    add(kRoot);
    mix(static_cast<uint8_t>(kMaxDepth));
    files = 0;
  }
  void mix(uint8_t b) {
    h ^= b;
    h *= 1099511628211ull;
  }
  void add(const char* s) {
    while (*s) mix(static_cast<uint8_t>(*s++));
    mix('\n');
    ++files;
  }
};

void onHash(const char* path, void* ctx) { static_cast<PathHash*>(ctx)->add(path); }

struct BuildCtx {
  LibraryIndex* index = nullptr;
  PathHash hash;
  uint32_t added = 0;
  uint64_t addUs = 0;
  uint32_t minFree = UINT32_MAX;
};

void onBuild(const char* path, void* p) {
  auto& c = *static_cast<BuildCtx*>(p);
  c.hash.add(path);
  const int64_t t0 = esp_timer_get_time();
  if (c.index->addFile(path) == LibraryIndex::Add::Added) ++c.added;
  c.addUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
  c.minFree = std::min(c.minFree, internalFree());
}

}  // namespace

Library::Library(LocalStorage& storage) : storage_(storage) {}

bool Library::ensureIndex() {
  if (!index_) index_ = psramNew<LibraryIndex>(psramAlloc, psramFree);
  if (!index_) Serial.println("[lib] no PSRAM for the library index");
  catalog_.setIndex(index_);
  return index_ != nullptr;
}

void Library::cachePath(char* buf, size_t size, bool temp) {
  snprintf(buf, size, "%s/%s", storage_.stateDir(), temp ? "library.tmp" : "library.idx");
}

Library::Walk Library::signatureWalk() {
  Walk w;
  PathHash hash;
  const int64_t t0 = esp_timer_get_time();
  w.files = storage_.forEachFile(onHash, &hash, kMaxDepth);
  w.ms = msSince(t0);
  w.signature = hash.h;
  return w;
}

bool Library::loadCache(uint64_t signature) {
  char path[48];
  cachePath(path, sizeof(path), false);
  File f = storage_.fs().open(path, FILE_READ);
  if (!f) {
    stats_.cacheNote = "no cache yet";
    return false;
  }
  FileSource src(f);
  const int64_t t0 = esp_timer_get_time();
  const LibraryIndex::Load r = index_->load(src, signature);
  f.close();
  stats_.loadMs = msSince(t0);
  switch (r) {
    case LibraryIndex::Load::Loaded:
      stats_.fromCache = true;
      return true;
    case LibraryIndex::Load::Stale: stats_.cacheNote = "the card changed"; break;
    case LibraryIndex::Load::Outdated: stats_.cacheNote = "the cache is an older version's: rebuilt once"; break;
    case LibraryIndex::Load::Corrupt: stats_.cacheNote = "the cache was unreadable"; break;
    case LibraryIndex::Load::NoMemory: stats_.cacheNote = "no PSRAM to load the cache"; break;
  }
  return false;
}

bool Library::build(uint64_t* signature) {
  index_->clear();
  BuildCtx ctx;
  ctx.index = index_;
  ctx.minFree = internalFree();
  const int64_t t0 = esp_timer_get_time();
  // The files seen by the signature walk (0: unknown) size the blocks up
  // front: a lower build peak than growing them.
  index_->begin(kRoot, stats_.walk.files);
  storage_.forEachFile(onBuild, &ctx, kMaxDepth);
  const int64_t t1 = esp_timer_get_time();
  const bool ok = index_->finish();
  stats_.addMs = ctx.addUs / 1000.0f;
  stats_.buildWalkMs = static_cast<float>(t1 - t0) / 1000.0f - stats_.addMs;
  stats_.finishMs = msSince(t1);
  stats_.added = ctx.added;
  stats_.internalMinDuring = std::min(ctx.minFree, internalFree());
  if (stats_.walk.files == 0) stats_.walk.files = ctx.hash.files;  // a rebuild has no signature walk
  *signature = ctx.hash.h;
  if (!ok) Serial.println("[lib] index build FAILED (out of PSRAM)");
  return ok;
}

void Library::saveCache(uint64_t signature) {
  char tmp[48], path[48];
  cachePath(tmp, sizeof(tmp), true);
  cachePath(path, sizeof(path), false);
  fs::FS& fs = storage_.fs();
  const int64_t t0 = esp_timer_get_time();
  File f = fs.open(tmp, FILE_WRITE);
  bool ok = static_cast<bool>(f);
  if (ok) {
    FileSink sink(f);
    ok = index_->save(sink, signature);
    f.close();
  }
  // Written aside, then swapped in: a cut-off write never leaves a cache
  // that looks whole (and load() checks its sum anyway).
  if (ok) {
    fs.remove(path);
    ok = fs.rename(tmp, path);
  } else {
    fs.remove(tmp);
  }
  stats_.saveMs = msSince(t0);
  if (!ok) Serial.printf("[lib] couldn't save the index cache to %s\n", path);
}

bool Library::begin() {
  if (!storage_.available()) {
    Serial.println("[lib] no storage: no library (the built-in tracks still play)");
    return false;
  }
  if (!ensureIndex()) return false;
  stats_ = Stats{};
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  stats_.walk = signatureWalk();
  bool ok = loadCache(stats_.walk.signature);
  if (!ok) {
    uint64_t signature = 0;
    ok = build(&signature);
    if (ok) saveCache(signature);
  }
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  if (stats_.internalMinDuring == 0) stats_.internalMinDuring = internalFree();
  if (stats_.fromCache) {
    Serial.printf("[lib] %s /music: %lu files walked in %.0f ms, index loaded from the cache in %.0f ms\n",
                  storage_.name(), (unsigned long)stats_.walk.files, stats_.walk.ms, stats_.loadMs);
  } else {
    Serial.printf("[lib] %s /music: %lu files walked in %.0f ms; built (%s): walk %.0f ms, add %.0f ms, finish "
                  "%.0f ms; cache saved in %.0f ms\n",
                  storage_.name(), (unsigned long)stats_.walk.files, stats_.walk.ms, stats_.cacheNote,
                  stats_.buildWalkMs, stats_.addMs, stats_.finishMs, stats_.saveMs);
  }
  if (ok) {
    Serial.printf("[lib] %lu tracks, %lu artists, %lu albums; %u B of PSRAM; internal RAM %+ld B\n",
                  (unsigned long)index_->trackCount(), (unsigned long)index_->artistCount(),
                  (unsigned long)index_->albumCount(), (unsigned)index_->memory().total,
                  (long)stats_.internalDelta);
  }
  return ok;
}

bool Library::rebuild() {
  if (!storage_.available() || !ensureIndex()) return false;
  stats_ = Stats{};
  stats_.cacheNote = "asked for";
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  uint64_t signature = 0;
  const bool ok = build(&signature);
  if (ok) saveCache(signature);
  stats_.walk.signature = signature;
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  return ok;
}

void Library::report() const {
  if (!index_ || !index_->ready()) {
    Serial.println("[index] library: empty (no card, or the build failed; g0 rebuilds it)");
    return;
  }
  const LibraryIndex::Memory m = index_->memory();
  const uint32_t t = index_->trackCount();
  Serial.printf("[index] library, %s /music: %lu tracks, %lu artists, %lu albums, %lu folders\n", storage_.name(),
                (unsigned long)t, (unsigned long)index_->artistCount(), (unsigned long)index_->albumCount(),
                (unsigned long)index_->folderCount());
  if (stats_.fromCache) {
    Serial.printf("[index] time: signature walk %.1f ms (%lu files), cache load %.1f ms\n", stats_.walk.ms,
                  (unsigned long)stats_.walk.files, stats_.loadMs);
  } else {
    Serial.printf("[index] time: signature walk %.1f ms; built (%s): walk %.1f ms (the file system), add %.1f ms, "
                  "finish %.1f ms (sort + views + trim), cache save %.1f ms\n",
                  stats_.walk.ms, stats_.cacheNote, stats_.buildWalkMs, stats_.addMs, stats_.finishMs, stats_.saveMs);
  }
  Serial.printf("[index] PSRAM: %u B held (%.1f B/track): strings %u, tracks %u, artists %u, albums %u, folders %u, "
                "views %u; build peak %u B; PSRAM free fell %ld B\n",
                (unsigned)m.total, t ? static_cast<float>(m.total) / t : 0.0f, (unsigned)m.strings,
                (unsigned)m.tracks, (unsigned)m.artists, (unsigned)m.albums, (unsigned)m.folders, (unsigned)m.views,
                (unsigned)m.buildPeak, (long)stats_.psramUsed);
  Serial.printf("[index] internal RAM: free %lu B before, %+ld B after, lowest %lu B during; the index object is in "
                "PSRAM (%u B)\n",
                (unsigned long)stats_.internalFreeBefore, (long)stats_.internalDelta,
                (unsigned long)stats_.internalMinDuring, (unsigned)sizeof(LibraryIndex));
  const LibraryIndex::Span a = index_->artistsAZ();
  Serial.print("[index] artists A-Z:");
  for (uint32_t i = 0; i < a.count && i < 4; ++i) Serial.printf(" \"%s\"", index_->artistName(a[i]));
  Serial.printf("%s; rail buckets:", a.count > 4 ? " ..." : "");
  for (int b = 0; b < LibraryIndex::kBuckets; ++b) {
    const uint32_t n = index_->bucketStart(LibraryIndex::View::Artists, b + 1) -
                       index_->bucketStart(LibraryIndex::View::Artists, b);
    if (n) Serial.printf(" %c%lu", b == 0 ? '#' : 'A' + b - 1, (unsigned long)n);
  }
  Serial.println();
}
