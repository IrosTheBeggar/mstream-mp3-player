// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryIndex.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "CardContract.h"
#include "NameKey.h"
#include "TextFold.h"
#include "TrackName.h"

static_assert(sizeof(LibraryIndex::Track) == 32, "library.idx v6: a track is 32 bytes");
static_assert(sizeof(LibraryIndex::Artist) == 20, "library.idx v6: an artist is 20 bytes");
static_assert(sizeof(LibraryIndex::Album) == 28, "library.idx v6: an album is 28 bytes");
static_assert(sizeof(LibraryIndex::Folder) == 36, "library.idx v6: a folder is 36 bytes");

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

uint32_t fnv(const char* s, size_t len, uint32_t seed) {
  uint32_t h = 2166136261u ^ (seed * 0x9E3779B1u);
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<unsigned char>(s[i]);
    h *= 16777619u;
  }
  return h ? h : 1;
}

bool sameName(const char* stored, const char* name, size_t len) {
  return std::memcmp(stored, name, len) == 0 && stored[len] == 0;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool extIs(const char* ext, size_t len, const char* want) {
  if (std::strlen(want) != len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (lower(ext[i]) != want[i]) return false;
  }
  return true;
}

// A case-insensitive match of the first `len` bytes of `s` with `want`.
bool nameIs(const char* s, size_t len, const char* want) {
  if (std::strlen(want) != len) return false;
  for (size_t i = 0; i < len; ++i) {
    if (lower(s[i]) != want[i]) return false;
  }
  return true;
}

LibraryIndex::Folder newFolder(uint32_t name, uint32_t parent) {
  LibraryIndex::Folder f{};
  f.name = name;
  f.parent = parent;
  f.image = LibraryIndex::kNone;
  f.imageRank = LibraryIndex::kNoImage;
  return f;
}

uint32_t pow2AtLeast(uint32_t n) {
  uint32_t p = 64;
  while (p < n) p <<= 1;
  return p;
}

// A table's slots for n entries, under its 70 % fill.
uint32_t tableSlots(uint32_t n) { return pow2AtLeast(n / 7 * 10 + 16); }

// Bytes order, the shorter first on a common prefix: the tie rule of the
// votes (mStream's "smallest").
bool bytesLess(const char* a, const char* b) { return std::strcmp(a, b) < 0; }

// The first id in `ids` (sorted by textfold::compare, then id) whose name
// is `name`, or kNone. compare() is a total order (names that fold alike
// fall back to their bytes), so the equal run is the name itself.
template <typename NameOf>
uint32_t findByName(const uint32_t* ids, uint32_t n, const char* name, NameOf nameOf) {
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (textfold::compare(nameOf(ids[mid]), name) < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < n && std::strcmp(nameOf(ids[lo]), name) == 0 ? ids[lo] : LibraryIndex::kNone;
}

// The disc, number and title of every track that its record didn't give:
// each folder's files read together (trackname::Folder), so a shape counts
// only when the folder is written that way, with its artist folder's name.
// `byFolder` has every folder's own files in one run.
template <typename Str>
void readNames(Str str, LibraryIndex::Track* tracks, const uint32_t* artistFolderName, const uint32_t* byFolder,
               uint32_t n) {
  auto stemOf = [&](const LibraryIndex::Track& t, size_t* len) {
    const char* name = str(t.name);
    const char* dot = std::strrchr(name, '.');
    *len = dot && dot != name ? static_cast<size_t>(dot - name) : std::strlen(name);
    return name;
  };
  for (uint32_t i = 0; i < n;) {
    const uint32_t folder = tracks[byFolder[i]].folder;
    trackname::Folder names(str(artistFolderName[tracks[byFolder[i]].artist]));  // a folder has one artist
    uint32_t j = i;
    for (; j < n && tracks[byFolder[j]].folder == folder; ++j) {
      size_t len;
      const char* stem = stemOf(tracks[byFolder[j]], &len);
      names.add(stem, len);
    }
    for (uint32_t k = i; k < j; ++k) {
      LibraryIndex::Track& t = tracks[byFolder[k]];
      size_t len;
      const char* stem = stemOf(t, &len);
      const trackname::Name r = names.read(stem, len);
      if (!(t.flags & LibraryIndex::kTagDisc)) t.disc = r.disc;
      if (!(t.flags & LibraryIndex::kTagNumber)) t.number = r.number;
      if (!(t.flags & LibraryIndex::kTagTitle)) {
        const size_t titleLen = len - r.titleAt;
        t.title = t.name + r.titleAt;
        t.titleLen = static_cast<uint8_t>(titleLen > 255 ? 255 : titleLen);
      }
    }
    i = j;
  }
}

// ---- the cache file ----
// Little-endian 32-bit words: the header, then the blocks as they are in
// memory (the strings, the four record tables, the views), then an FNV-1a
// sum of everything before it. Same compiler, same layout: a record size
// that changed makes the file Corrupt, and the version is bumped when a
// record's meaning changes.
constexpr uint32_t kMagic = 0x494C504Du;  // "MPLI"
// 2: folders count their other files and pick a cover. 3: .opus files are
// tracks (Format::Opus; a version-2 cache, built from the same paths, would
// keep them as other files, since the signature hashes only the paths).
// 4: names read with their folder (discs, "Artist - 03 - Title", the
// artist off titles), and artists and albums sorted past "The". 3 and 4
// were written by two branches (feature/opus and dev) with the same record
// sizes and path signature, each refusing the other's; 5 is their merge,
// both at once. 6: tag records (docs/METADATA.md 3.4.3): 32-byte tracks
// (a title of their own, the record's artist, a 16-bit number, the length,
// the source), 28-byte albums (the elected artist line, year, discs,
// flags), the strings in 64 KB chunks, the inputs in the header. A cache of
// any earlier version is Outdated (load()) and the Library rebuilds it once.
constexpr uint32_t kVersion = 6;
// The header: magic, version, record sizes, rules; the inputs (walk
// signature 2, cardId 2, generation, commitId 2, T's headerCrc, flags, D's
// headerCrc, the journal's sequence); the build stamp (2); the string bytes
// and the four counts; then the two rails' buckets.
constexpr int kCountWords = 22;
constexpr int kHeaderWords = kCountWords + 2 * (LibraryIndex::kBuckets + 1);
constexpr uint32_t kMaxRecords = 1u << 22;  // a damaged header must not ask for gigabytes

uint32_t recordSizes() {
  return static_cast<uint32_t>(sizeof(LibraryIndex::Track)) |
         static_cast<uint32_t>(sizeof(LibraryIndex::Artist)) << 8 |
         static_cast<uint32_t>(sizeof(LibraryIndex::Album)) << 16 |
         static_cast<uint32_t>(sizeof(LibraryIndex::Folder)) << 24;
}

struct Fnv32 {
  uint32_t h = 2166136261u;
  void add(const void* data, size_t n) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) {
      h ^= p[i];
      h *= 16777619u;
    }
  }
};

class SummingSink : public ByteSink {
public:
  explicit SummingSink(ByteSink& out) : out_(out) {}
  bool write(const void* data, size_t n) override {
    if (n == 0) return true;
    sum.add(data, n);
    return out_.write(data, n);
  }
  Fnv32 sum;

private:
  ByteSink& out_;
};

class SummingSource : public ByteSource {
public:
  explicit SummingSource(ByteSource& in) : in_(in) {}
  size_t read(void* data, size_t n) override {
    const size_t got = in_.read(data, n);
    sum.add(data, got);
    return got;
  }
  Fnv32 sum;

private:
  ByteSource& in_;
};

}  // namespace

// The votes of the album run and the artist run being added (5.4): every
// distinct string of the album run (album values, album-artist displays,
// track-artist displays) once, with its counts; the years; the artist run's
// candidates. Fixed, from the hooks while records are added (about 42 KB),
// cleared run by run through the lists of the slots used.
struct LibraryIndex::Votes {
  static constexpr uint32_t kSlots = 1024;        // at most 3/4 used: 768 distinct strings
  static constexpr uint32_t kArtistSlots = 512;   // at most 3/4 used
  static constexpr uint32_t kYears = 64;
  struct Entry {
    uint64_t hash;
    uint32_t off;
    uint32_t sort;  // an album value's first sort tag (albumSort)
    uint16_t album, albumArtist, trackArtist;
    uint16_t used;
  };
  struct ArtistEntry {
    uint64_t hash;
    uint32_t off;
    uint32_t sort;  // the candidate's first sort tag (albumArtistSort, else artistSort)
    uint32_t count;
    uint32_t used;
  };
  Entry slot[kSlots];
  uint16_t usedList[kSlots];
  uint32_t used;
  ArtistEntry aslot[kArtistSlots];
  uint16_t aUsedList[kArtistSlots];
  uint32_t aUsed;
  uint16_t year[kYears];
  uint32_t yearCount[kYears];
  uint32_t years;
  uint32_t voters;   // tracks of the album run that voted (at most kVoteTracks)
  uint32_t records;  // tracks of the album run with a record
  bool compilation;
  char join[2][namekey::kDisplayMax + 1];
};

LibraryIndex::LibraryIndex(AllocFn alloc, FreeFn release, ShrinkFn shrink)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree), shrinkFn_(shrink) {}

LibraryIndex::~LibraryIndex() {
  keepTracks_ = false;
  clear();
}

// Every block goes through these two, which count what is held: the build
// peak is the most held at any moment, a block and its replacement included.
void* LibraryIndex::alloc(size_t bytes) {
  void* p = allocFn_(bytes);
  if (p) {
    held_ += bytes;
    if (held_ > buildPeak_) buildPeak_ = held_;
  }
  return p;
}

void LibraryIndex::release(void* p, size_t bytes) {
  if (!p) return;
  freeFn_(p);
  held_ -= bytes;
}

template <typename T>
bool LibraryIndex::reserve(Block<T>& b, uint32_t n) {
  if (n <= b.cap) return true;
  // An empty block too small for what comes (the kept track block):
  // freed first, then asked at the size wanted, not doubled.
  if (b.size == 0 && b.data) drop(b);
  uint32_t cap = b.cap ? b.cap * 2 : 16;
  if (cap < n) cap = n;
  T* p = static_cast<T*>(alloc(static_cast<size_t>(cap) * sizeof(T)));
  if (!p) {
    failed_ = true;
    return false;
  }
  if (b.size) std::memcpy(p, b.data, static_cast<size_t>(b.size) * sizeof(T));
  release(b.data, bytes(b));
  b.data = p;
  b.cap = cap;
  return true;
}

template <typename T>
bool LibraryIndex::push(Block<T>& b, const T& v) {
  if (!reserve(b, b.size + 1)) return false;
  b.data[b.size++] = v;
  return true;
}

// To its size: in place through the shrink hook, else by a copy (kept as
// it is when the copy can't be had).
template <typename T>
void LibraryIndex::trim(Block<T>& b) {
  if (b.cap == b.size) return;
  if (b.size == 0) {
    drop(b);
    return;
  }
  const size_t want = static_cast<size_t>(b.size) * sizeof(T);
  if (shrinkFn_) {
    void* p = shrinkFn_(b.data, want);
    if (p) {
      held_ -= bytes(b) - want;
      b.data = static_cast<T*>(p);
      b.cap = b.size;
      return;
    }
  }
  T* p = static_cast<T*>(alloc(want));
  if (!p) return;  // keep the slack rather than fail
  std::memcpy(p, b.data, want);
  release(b.data, bytes(b));
  b.data = p;
  b.cap = b.size;
}

template <typename T>
void LibraryIndex::drop(Block<T>& b) {
  release(b.data, bytes(b));
  b = Block<T>{};
}

bool LibraryIndex::tableInit(Table& t, uint32_t cap) {
  auto* ids = static_cast<uint32_t*>(alloc(cap * sizeof(uint32_t)));
  auto* hashes = static_cast<uint32_t*>(alloc(cap * sizeof(uint32_t)));
  if (!ids || !hashes) {
    release(ids, cap * sizeof(uint32_t));
    release(hashes, cap * sizeof(uint32_t));
    failed_ = true;
    return false;
  }
  std::memset(ids, 0, cap * sizeof(uint32_t));
  tableDrop(t);
  t.ids = ids;
  t.hashes = hashes;
  t.cap = cap;
  t.size = 0;
  return true;
}

void LibraryIndex::tableDrop(Table& t) {
  release(t.ids, t.cap * sizeof(uint32_t));
  release(t.hashes, t.cap * sizeof(uint32_t));
  t = Table{};
}

template <typename Eq>
uint32_t LibraryIndex::tableFind(const Table& t, uint32_t hash, Eq eq) const {
  if (!t.cap) return kNone;
  const uint32_t mask = t.cap - 1;
  for (uint32_t i = hash & mask;; i = (i + 1) & mask) {
    const uint32_t slot = t.ids[i];
    if (slot == 0) return kNone;
    if (t.hashes[i] == hash && eq(slot - 1)) return slot - 1;
  }
}

bool LibraryIndex::tableInsert(Table& t, uint32_t hash, uint32_t id) {
  if ((t.size + 1) * 10 > t.cap * 7) {  // over 70 % full: double
    Table bigger;
    if (!tableInit(bigger, t.cap ? t.cap * 2 : 64)) return false;
    for (uint32_t i = 0; i < t.cap; ++i) {
      if (!t.ids[i]) continue;
      const uint32_t mask = bigger.cap - 1;
      uint32_t j = t.hashes[i] & mask;
      while (bigger.ids[j]) j = (j + 1) & mask;
      bigger.ids[j] = t.ids[i];
      bigger.hashes[j] = t.hashes[i];
      ++bigger.size;
    }
    tableDrop(t);
    t = bigger;
  }
  const uint32_t mask = t.cap - 1;
  uint32_t i = hash & mask;
  while (t.ids[i]) i = (i + 1) & mask;
  t.ids[i] = id + 1;
  t.hashes[i] = hash;
  ++t.size;
  return true;
}

// ---- the strings ----

uint32_t LibraryIndex::arenaBytes() const { return chunks_ ? (chunks_ - 1) * kChunkBytes + lastUsed_ : 0; }

size_t LibraryIndex::arenaHeld() const {
  if (whole_) return wholeBytes_;
  return chunks_ ? static_cast<size_t>(chunks_ - 1) * kChunkBytes + lastCap_ : 0;
}

void LibraryIndex::dropArena() {
  if (whole_) {
    release(whole_, wholeBytes_);
  } else {
    for (uint32_t i = 0; i < chunks_; ++i) release(chunk_[i], i + 1 < chunks_ ? kChunkBytes : lastCap_);
  }
  std::memset(chunk_, 0, sizeof(chunk_));
  chunks_ = lastUsed_ = lastCap_ = 0;
  whole_ = nullptr;
  wholeBytes_ = 0;
}

// A string's copy, NUL-terminated, in the last chunk. The first chunk grows
// by doubling up to kChunkBytes (a small library never takes a whole one);
// then each full chunk is closed, its tail zeroed (saved as it is), and a
// new one taken: no string moves once it has its offset.
uint32_t LibraryIndex::intern(const char* s, size_t len) {
  const size_t need = len + 1;
  if (need > kChunkBytes) {
    failed_ = true;
    return kNone;
  }
  if (chunks_ == 0) {
    uint32_t cap = lastCap_ ? lastCap_ : 1024;  // startBuild() leaves the first chunk's size here
    if (cap < need) cap = static_cast<uint32_t>(need);
    if (cap > kChunkBytes) cap = kChunkBytes;
    char* p = static_cast<char*>(alloc(cap));
    if (!p) {
      failed_ = true;
      return kNone;
    }
    chunk_[0] = p;
    chunks_ = 1;
    lastCap_ = cap;
    lastUsed_ = 0;
  }
  if (lastUsed_ + need > lastCap_ && lastCap_ < kChunkBytes) {
    uint32_t cap = lastCap_ * 2;
    if (cap < lastUsed_ + need) cap = static_cast<uint32_t>(lastUsed_ + need);
    if (cap > kChunkBytes) cap = kChunkBytes;
    char* p = static_cast<char*>(alloc(cap));
    if (!p) {
      failed_ = true;
      return kNone;
    }
    std::memcpy(p, chunk_[chunks_ - 1], lastUsed_);
    release(chunk_[chunks_ - 1], lastCap_);
    chunk_[chunks_ - 1] = p;
    lastCap_ = cap;
  }
  if (lastUsed_ + need > lastCap_) {
    if (chunks_ == kMaxChunks) {
      failed_ = true;
      return kNone;
    }
    char* p = static_cast<char*>(alloc(kChunkBytes));
    if (!p) {
      failed_ = true;
      return kNone;
    }
    std::memset(chunk_[chunks_ - 1] + lastUsed_, 0, lastCap_ - lastUsed_);
    chunk_[chunks_++] = p;
    lastCap_ = kChunkBytes;
    lastUsed_ = 0;
  }
  const uint32_t off = (chunks_ - 1) << kChunkBits | lastUsed_;
  char* dst = chunk_[chunks_ - 1] + lastUsed_;
  std::memcpy(dst, s, len);
  dst[len] = 0;
  lastUsed_ += static_cast<uint32_t>(need);
  return off;
}

// The last chunk to what it holds.
void LibraryIndex::trimArena() {
  if (whole_ || chunks_ == 0 || lastCap_ == lastUsed_ || lastUsed_ == 0) return;
  char* last = chunk_[chunks_ - 1];
  if (shrinkFn_) {
    void* p = shrinkFn_(last, lastUsed_);
    if (p) {
      held_ -= lastCap_ - lastUsed_;
      chunk_[chunks_ - 1] = static_cast<char*>(p);
      lastCap_ = lastUsed_;
      return;
    }
  }
  char* p = static_cast<char*>(alloc(lastUsed_));
  if (!p) return;
  std::memcpy(p, last, lastUsed_);
  release(last, lastCap_);
  chunk_[chunks_ - 1] = p;
  lastCap_ = lastUsed_;
}

// ---- building ----

void LibraryIndex::resetViews() {
  release(viewsBlock_, viewsBytes_);
  viewsBlock_ = nullptr;
  viewsBytes_ = 0;
  artistsAZ_ = albumsAZ_ = albumsByArtist_ = tracksByAlbum_ = folderChildren_ = folderTree_ = nullptr;
  artistSortKeys_ = albumSortKeys_ = nullptr;
  std::memset(artistBuckets_, 0, sizeof(artistBuckets_));
  std::memset(albumBuckets_, 0, sizeof(albumBuckets_));
}

void LibraryIndex::dropVotes() {
  if (votes_) release(votes_, sizeof(Votes));
  votes_ = nullptr;
}

void LibraryIndex::clear() {
  dropArena();
  if (keepTracks_ && tracksB_.data) {
    tracksB_.size = 0;  // the block kept for the next build or load
  } else {
    drop(tracksB_);
  }
  drop(artistsB_);
  drop(albumsB_);
  drop(foldersB_);
  drop(folderAlbum_);
  drop(folderMarks_);
  drop(artistKey_);
  drop(artistSort_);
  drop(albumSort_);
  tableDrop(folderTable_);
  tableDrop(artistTable_);
  dropVotes();
  resetViews();
  tracks_ = nullptr;
  artists_ = nullptr;
  albums_ = nullptr;
  folders_ = nullptr;
  trackN_ = artistN_ = albumN_ = folderN_ = 0;
  runAlbum_ = runArtist_ = kNone;
  runFirst_ = 0;
  roots_ = nullptr;
  rootCount_ = 0;
  thumbs_ = nullptr;
  thumbN_ = 0;
  rootLen_ = 0;
  emptyName_ = various_ = kNone;
  inputs_ = Inputs{};
  buildStamp_ = 0;
  building_ = false;
  failed_ = false;
  ready_ = false;
  buildPeak_ = held_;  // 0: everything is released above (but a kept track block)
}

bool LibraryIndex::begin(const char* root, uint32_t expectTracks) {
  // Today's estimates from the track count alone.
  const uint32_t n = expectTracks ? expectTracks : 256;
  Sizing s;
  s.tracks = n;
  s.artists = n / 16 + 16;
  s.albums = n / 6 + 16;
  s.folders = n / 5 + 16;
  s.firstChunk = n * 40 + 1024;
  return startBuild(s, root);
}

bool LibraryIndex::begin(const Sizing& sizing, const char* root, const char* const* libraryRoots, uint32_t rootCount) {
  Sizing s = sizing;
  const uint32_t n = s.tracks ? s.tracks : 256;
  s.tracks = n;
  if (!s.folders) s.folders = n / 5 + 16;
  if (!s.albums) s.albums = s.folders;
  if (!s.artists) s.artists = std::min(s.folders, n) + 1;
  if (!s.firstChunk) s.firstChunk = n * 40 + 1024;
  if (!startBuild(s, root)) return false;
  roots_ = libraryRoots;
  rootCount_ = libraryRoots ? rootCount : 0;
  return true;
}

bool LibraryIndex::startBuild(const Sizing& s, const char* root) {
  clear();
  building_ = true;
  size_t rootLen = std::strlen(root);
  while (rootLen > 1 && root[rootLen - 1] == '/') --rootLen;
  lastCap_ = s.firstChunk > kChunkBytes ? kChunkBytes : s.firstChunk;  // intern() takes the first chunk this size
  const bool ok = reserve(tracksB_, s.tracks) && reserve(artistsB_, s.artists) && reserve(albumsB_, s.albums) &&
                  reserve(foldersB_, s.folders) && reserve(folderAlbum_, s.folders) &&
                  reserve(folderMarks_, s.folders) && reserve(artistKey_, s.artists) &&
                  reserve(artistSort_, s.artists) && reserve(albumSort_, s.albums) &&
                  tableInit(folderTable_, tableSlots(s.folders)) && tableInit(artistTable_, tableSlots(s.artists));
  if (!ok) {
    failed_ = true;
    return false;
  }
  const uint32_t rootName = intern(root, rootLen);
  emptyName_ = intern("", 0);
  if (rootName == kNone || emptyName_ == kNone) return false;
  rootLen_ = static_cast<uint32_t>(rootLen);
  return push(foldersB_, newFolder(rootName, kNone)) && push(folderAlbum_, kNone) && push(folderMarks_, uint8_t{0});
}

uint32_t LibraryIndex::folderChild(uint32_t parent, const char* name, size_t len) {
  const uint32_t h = fnv(name, len, parent + 1);
  const uint32_t found = tableFind(folderTable_, h, [&](uint32_t id) {
    return foldersB_.data[id].parent == parent && sameName(at(foldersB_.data[id].name), name, len);
  });
  if (found != kNone) return found;
  const uint32_t off = intern(name, len);
  if (off == kNone) return kNone;
  const uint32_t id = foldersB_.size;
  if (!push(foldersB_, newFolder(off, parent)) || !push(folderAlbum_, kNone) || !push(folderMarks_, uint8_t{0}))
    return kNone;
  if (!tableInsert(folderTable_, h, id)) return kNone;
  return id;
}

uint32_t LibraryIndex::artistFor(uint32_t nameOffset) {
  const char* name = at(nameOffset);
  const size_t len = std::strlen(name);
  const uint32_t h = fnv(name, len, 0);
  const uint32_t found =
      tableFind(artistTable_, h, [&](uint32_t id) { return sameName(at(artistKey_.data[id]), name, len); });
  if (found != kNone) return found;
  const uint32_t id = artistsB_.size;
  if (!push(artistsB_, Artist{nameOffset, 0, 0, 0, 0}) || !push(artistKey_, nameOffset) || !push(artistSort_, kNone))
    return kNone;
  if (!tableInsert(artistTable_, h, id)) return kNone;
  return id;
}

uint32_t LibraryIndex::albumFor(uint32_t folder, uint32_t artist, bool loose) {
  const uint32_t found = folderAlbum_.data[folder];
  if (found != kNone) return found;
  const uint32_t id = albumsB_.size;
  Album a{};
  a.name = loose ? emptyName_ : foldersB_.data[folder].name;
  a.artist = artist;
  a.folder = folder;
  a.artistLine = artistKey_.data[artist];
  a.flags = loose ? kLoose : 0;
  if (!push(albumsB_, a) || !push(albumSort_, kNone)) return kNone;
  folderAlbum_.data[folder] = id;
  return id;
}

uint32_t LibraryIndex::folderOf(const char* path, const char* lastSlash, uint32_t* artistFolder,
                                uint32_t* albumFolder) {
  // The file's root: the longest library root that holds it (its depth in
  // names), else the root itself (depth 0).
  const char* rel = path + rootLen_ + 1;
  const size_t relLen = lastSlash > rel ? static_cast<size_t>(lastSlash - rel) : 0;  // its folder's path
  int rootDepth = 0;
  size_t rootBytes = 0;
  for (uint32_t i = 0; i < rootCount_; ++i) {
    const char* r = roots_[i];
    const size_t rl = r ? std::strlen(r) : 0;
    if (rl == 0 || rl > relLen || rl <= rootBytes) continue;
    if (std::memcmp(rel, r, rl) != 0 || (rl < relLen && rel[rl] != '/')) continue;
    int depth = 1;
    for (size_t k = 0; k < rl; ++k) depth += r[k] == '/';
    rootDepth = depth;
    rootBytes = rl;
  }
  uint32_t folder = 0;
  int depth = 0;
  *artistFolder = *albumFolder = kNone;
  for (const char* p = rel; p < lastSlash;) {
    const char* q = static_cast<const char*>(std::memchr(p, '/', static_cast<size_t>(lastSlash - p)));
    if (!q) q = lastSlash;
    if (q > p) {
      folder = folderChild(folder, p, static_cast<size_t>(q - p));
      if (folder == kNone) return kNone;
      ++depth;
      if (depth == rootDepth + 1) *artistFolder = folder;
      if (depth == rootDepth + 2) *albumFolder = folder;
    }
    p = q + 1;
  }
  return folder;
}

LibraryIndex::Format LibraryIndex::formatOf(const char* leaf, size_t leafLen) {
  const char* dot = nullptr;
  for (size_t i = leafLen; i-- > 0;) {
    if (leaf[i] == '.') {
      dot = leaf + i;
      break;
    }
  }
  if (!dot || dot == leaf) return Format::Unknown;
  const size_t extLen = leafLen - static_cast<size_t>(dot + 1 - leaf);
  if (extIs(dot + 1, extLen, "mp3")) return Format::Mp3;
  if (extIs(dot + 1, extLen, "flac")) return Format::Flac;
  if (extIs(dot + 1, extLen, "opus")) return Format::Opus;
  return Format::Unknown;
}

uint8_t LibraryIndex::imageRank(const char* name, size_t len) {
  const char* dot = nullptr;
  for (size_t i = len; i-- > 0;) {
    if (name[i] == '.') {
      dot = name + i;
      break;
    }
  }
  if (!dot || dot == name) return kNoImage;
  const size_t stem = static_cast<size_t>(dot - name);
  const size_t extLen = len - stem - 1;
  if (!extIs(dot + 1, extLen, "jpg") && !extIs(dot + 1, extLen, "jpeg")) return kNoImage;
  if (nameIs(name, stem, "cover")) return 0;
  if (nameIs(name, stem, "folder")) return 1;
  if (nameIs(name, stem, "front")) return 2;
  return 3;
}

namespace {
constexpr uint8_t kMarkImageOwned = 1;  // the folder's image is a file the transfer's ledger lists
}  // namespace

// A file that isn't audio: counted in its folder, and its cover if it's the
// best-named image there so far.
LibraryIndex::Add LibraryIndex::addOther(const char* path, const char* lastSlash, const char* leaf, size_t leafLen,
                                         bool owned) {
  uint32_t artistFolder, albumFolder;
  const uint32_t folder = folderOf(path, lastSlash, &artistFolder, &albumFolder);
  if (folder == kNone) return Add::NoMemory;
  Folder& f = foldersB_.data[folder];
  if (f.otherCount < 0xFFFF) ++f.otherCount;
  const uint8_t rank = imageRank(leaf, leafLen);
  if (rank == kNoImage) return Add::Other;
  if (f.imageCount < 0xFF) ++f.imageCount;
  if (rank < f.imageRank) {
    const uint32_t name = intern(leaf, leafLen);
    if (name == kNone) return Add::NoMemory;
    Folder& g = foldersB_.data[folder];  // intern() doesn't move folders, but be plain about it
    g.image = name;
    g.imageRank = rank;
    folderMarks_.data[folder] =
        static_cast<uint8_t>((folderMarks_.data[folder] & ~kMarkImageOwned) | (owned ? kMarkImageOwned : 0));
  }
  return Add::Other;
}

LibraryIndex::Add LibraryIndex::placeTrack(const char* path, Format* format, uint32_t* folder, uint32_t* artist,
                                           uint32_t* album, uint32_t* name, const char** lastSlashOut) {
  if (!building_ || !path) return Add::Skipped;
  if (failed_) return Add::NoMemory;
  const char* root = at(foldersB_.data[0].name);
  if (std::strncmp(path, root, rootLen_) != 0 || path[rootLen_] != '/') return Add::Skipped;
  const char* lastSlash = std::strrchr(path, '/');
  const char* leaf = lastSlash + 1;
  const size_t leafLen = std::strlen(leaf);
  if (leafLen == 0) return Add::Skipped;
  *lastSlashOut = lastSlash;
  *format = formatOf(leaf, leafLen);
  if (*format == Format::Unknown) return Add::Other;

  // The folders, creating the ones not seen yet.
  uint32_t artistFolder, albumFolder;
  *folder = folderOf(path, lastSlash, &artistFolder, &albumFolder);
  if (*folder == kNone) return Add::NoMemory;
  *artist = artistFor(artistFolder != kNone ? foldersB_.data[artistFolder].name : emptyName_);
  if (*artist == kNone) return Add::NoMemory;
  const bool loose = albumFolder == kNone;
  *album = albumFor(loose ? *folder : albumFolder, *artist, loose);
  if (*album == kNone) return Add::NoMemory;
  if (!enterRuns(*album, *artist)) return Add::NoMemory;
  *name = intern(leaf, leafLen);
  if (*name == kNone) return Add::NoMemory;
  return Add::Added;
}

LibraryIndex::Add LibraryIndex::addFile(const char* path, uint8_t options) {
  Format format;
  uint32_t folder, artist, album, name;
  const char* lastSlash = nullptr;
  const Add r = placeTrack(path, &format, &folder, &artist, &album, &name, &lastSlash);
  if (r == Add::Other) {
    const char* leaf = lastSlash + 1;
    return addOther(path, lastSlash, leaf, std::strlen(leaf), (options & kAddOwned) != 0);
  }
  if (r != Add::Added) return r;
  // No number and the whole stem for a title until finish() reads the
  // names, each with its folder's (readNames()).
  const char* leaf = lastSlash + 1;
  const char* dot = std::strrchr(leaf, '.');
  const size_t stem = static_cast<size_t>(dot - leaf);
  Track t{};
  t.name = name;
  t.title = name;
  t.titleLen = static_cast<uint8_t>(stem > 255 ? 255 : stem);
  t.folder = folder;
  t.album = album;
  t.artist = artist;
  t.trackArtist = kNone;
  t.format = format;
  t.flags = (options & kAddPending) ? kTrackPending : 0;
  if (!push(tracksB_, t)) return Add::NoMemory;
  return Add::Added;
}

// ---- the votes ----

bool LibraryIndex::votesReady() {
  if (votes_) return true;
  void* p = alloc(sizeof(Votes));
  if (!p) {
    failed_ = true;
    return false;
  }
  std::memset(p, 0, sizeof(Votes));
  votes_ = static_cast<Votes*>(p);
  return true;
}

namespace {

// The run's entry for a string: found (`same` on its offset), or taken
// (*fresh: its offset and counts are the caller's to fill, at once, before
// any other lookup); nullptr when the table is full.
template <typename Entry, typename Same>
Entry* runSlot(Entry* slots, uint32_t cap, uint16_t* usedList, uint32_t* used, uint64_t hash, Same same, bool* fresh) {
  *fresh = false;
  const uint32_t mask = cap - 1;
  for (uint32_t i = static_cast<uint32_t>(hash) & mask;; i = (i + 1) & mask) {
    Entry& e = slots[i];
    if (!e.used) {
      if ((*used + 1) * 4 > cap * 3) return nullptr;
      e.used = 1;
      e.hash = hash;
      usedList[(*used)++] = static_cast<uint16_t>(i);
      *fresh = true;
      return &e;
    }
    if (e.hash == hash && same(e.off)) return &e;
  }
}

// A sort tag as the lists use it (5.4's orderName takes the nameKey of the
// sort tag only when that key isn't empty, else the name): without White_Space
// at either end, and none (*len 0) when nothing else is left. 2.3.6 stores a
// lone tab as one space, a valid value, which would otherwise sort its artist
// or album first, under '#'.
const char* sortTagOf(const char* s, size_t n, size_t* len) {
  *len = 0;
  return s && n ? namekey::trim(s, n, len) : nullptr;
}

}  // namespace

bool LibraryIndex::enterRuns(uint32_t album, uint32_t artist) {
  if (album != runAlbum_) {
    closeAlbumRun();
    runAlbum_ = album;
    runFirst_ = tracksB_.size;
  }
  if (artist != runArtist_) {
    closeArtistRun();
    runArtist_ = artist;
  }
  return !failed_;
}

void LibraryIndex::closeAlbumRun() {
  if (runAlbum_ == kNone) return;
  const uint32_t id = runAlbum_;
  runAlbum_ = kNone;
  if (!votes_) return;
  Votes& v = *votes_;
  if (v.records > 0 && !failed_) {
    Album& a = albumsB_.data[id];
    a.flags |= kTagged;
    const bool loose = (a.flags & kLoose) != 0;
    // The best entry by one of its counts, ties to the smallest bytes.
    auto best = [&](uint16_t Votes::Entry::*count) -> const Votes::Entry* {
      const Votes::Entry* b = nullptr;
      for (uint32_t k = 0; k < v.used; ++k) {
        const Votes::Entry& e = v.slot[v.usedList[k]];
        if (!(e.*count)) continue;
        if (!b || e.*count > b->*count || (e.*count == b->*count && bytesLess(at(e.off), at(b->off)))) b = &e;
      }
      return b;
    };
    if (!loose) {
      if (const Votes::Entry* n = best(&Votes::Entry::album)) {
        a.name = n->off;
        albumSort_.data[id] = n->sort;
      }
      uint32_t bestYear = 0, bestCount = 0;
      for (uint32_t k = 0; k < v.years; ++k) {
        if (v.yearCount[k] > bestCount || (v.yearCount[k] == bestCount && v.year[k] < bestYear)) {
          bestYear = v.year[k];
          bestCount = v.yearCount[k];
        }
      }
      a.year = static_cast<uint16_t>(bestYear);
    }
    if (const Votes::Entry* aa = best(&Votes::Entry::albumArtist)) {
      a.artistLine = aa->off;
    } else if (v.compilation) {
      if (various_ == kNone) various_ = intern("Various Artists", 15);
      if (various_ != kNone) a.artistLine = various_;
    } else if (const Votes::Entry* ta = best(&Votes::Entry::trackArtist)) {
      a.artistLine = ta->off;
    }
    // A track whose artist is the line shows none of its own.
    const char* line = at(a.artistLine);
    for (uint32_t t = runFirst_; t < tracksB_.size; ++t) {
      Track& tr = tracksB_.data[t];
      if (tr.album == id && tr.trackArtist != kNone && std::strcmp(at(tr.trackArtist), line) == 0)
        tr.trackArtist = kNone;
    }
  }
  for (uint32_t k = 0; k < v.used; ++k) v.slot[v.usedList[k]] = Votes::Entry{};
  v.used = v.years = v.voters = v.records = 0;
  v.compilation = false;
}

void LibraryIndex::closeArtistRun() {
  if (runArtist_ == kNone) return;
  const uint32_t id = runArtist_;
  runArtist_ = kNone;
  if (!votes_) return;
  Votes& v = *votes_;
  const Votes::ArtistEntry* b = nullptr;
  for (uint32_t k = 0; k < v.aUsed; ++k) {
    const Votes::ArtistEntry& e = v.aslot[v.aUsedList[k]];
    if (!b || e.count > b->count || (e.count == b->count && bytesLess(at(e.off), at(b->off)))) b = &e;
  }
  const char* folder = at(artistKey_.data[id]);
  if (b && folder[0]) {
    const char* w = at(b->off);
    if (textfold::sameName(w, std::strlen(w), folder, std::strlen(folder))) {
      artistsB_.data[id].name = b->off;
      artistSort_.data[id] = b->sort;
    }
  }
  for (uint32_t k = 0; k < v.aUsed; ++k) v.aslot[v.aUsedList[k]] = Votes::ArtistEntry{};
  v.aUsed = 0;
}

LibraryIndex::Add LibraryIndex::addRecord(const char* path, const TagView& tv) {
  Format format;
  uint32_t folder, artist, album, name;
  const char* lastSlash = nullptr;
  const Add r = placeTrack(path, &format, &folder, &artist, &album, &name, &lastSlash);
  if (r == Add::Other) {
    const char* leaf = lastSlash + 1;
    return addOther(path, lastSlash, leaf, std::strlen(leaf), false);
  }
  if (r != Add::Added) return r;
  if (!votesReady()) return Add::NoMemory;
  Votes& v = *votes_;
  const char* leaf = lastSlash + 1;
  const size_t leafLen = std::strlen(leaf);
  const char* dot = std::strrchr(leaf, '.');
  const size_t stem = static_cast<size_t>(dot - leaf);
  const bool loose = (albumsB_.data[album].flags & kLoose) != 0;
  const uint32_t artistFolderName = artistKey_.data[artist];
  const uint32_t albumFolderName = foldersB_.data[albumsB_.data[album].folder].name;

  Track t{};
  t.name = name;
  t.title = name;
  t.titleLen = static_cast<uint8_t>(stem > 255 ? 255 : stem);
  t.folder = folder;
  t.album = album;
  t.artist = artist;
  t.trackArtist = kNone;
  t.format = format;
  t.flags = tv.source & kSourceMask;

  // The title: a slice of the file name when it is inside it (most are),
  // else a string of its own.
  if (tv.title && tv.titleLen) {
    const size_t len = cardcontract::utf8CutLength(tv.title, tv.titleLen, 255);
    const char* hit = nullptr;
    for (size_t i = 0; len && i + len <= leafLen && !hit; ++i) {
      if (std::memcmp(leaf + i, tv.title, len) == 0) hit = leaf + i;
    }
    const uint32_t off = hit ? name + static_cast<uint32_t>(hit - leaf) : intern(tv.title, len);
    if (off == kNone) return Add::NoMemory;
    t.title = off;
    t.titleLen = static_cast<uint8_t>(len);
    t.flags |= kTagTitle;
  }
  if (tv.track) {
    t.number = tv.track;
    t.flags |= kTagNumber;
  }
  if (tv.disc) {
    t.disc = static_cast<uint8_t>(tv.disc > 255 ? 255 : tv.disc);
    t.flags |= kTagDisc;
  }
  const uint32_t secs = (tv.durationMs + 500) / 1000;
  t.durationS = static_cast<uint16_t>(secs > 0xFFFF ? 0xFFFF : secs);
  if (tv.jpeg) t.flags |= kTrackJpeg;
  if (tv.compilation == 1) t.flags |= kTrackCompilation;

  ++v.records;
  const bool vote = v.voters < kVoteTracks;
  if (vote) ++v.voters;
  if (vote && tv.compilation == 1) v.compilation = true;

  // A string of this run, interned once: an offset already in the index
  // when it has those bytes (a folder's name), else its run's copy. Its
  // entry (nullptr when the table is full: then it isn't counted).
  auto runString = [&](const char* s, size_t len, uint32_t reuse, Votes::Entry** entry) -> uint32_t {
    bool fresh = false;
    Votes::Entry* e = runSlot(v.slot, Votes::kSlots, v.usedList, &v.used, cardcontract::fnv1a64(s, len),
                              [&](uint32_t off) { return sameName(at(off), s, len); }, &fresh);
    *entry = e;
    if (e && !fresh) return e->off;
    const uint32_t off = reuse != kNone && sameName(at(reuse), s, len) ? reuse : intern(s, len);
    if (off == kNone) {
      if (e) {  // the last taken: given back
        e->used = 0;
        --v.used;
      }
      *entry = nullptr;
      return kNone;
    }
    if (e) {
      e->off = off;
      e->sort = kNone;
    }
    return off;
  };

  // The track's artists, as shown.
  uint32_t trackArtistOff = kNone, albumArtistOff = kNone;
  Votes::Entry* trackArtistEntry = nullptr;
  Votes::Entry* albumArtistEntry = nullptr;
  if (tv.artist && tv.artistLen) {
    namekey::displayJoin(tv.artist, tv.artistLen, v.join[0], sizeof(v.join[0]));
    const size_t n = std::strlen(v.join[0]);
    if (n) {
      trackArtistOff = runString(v.join[0], n, artistFolderName, &trackArtistEntry);
      if (trackArtistOff == kNone) return Add::NoMemory;
    }
  }
  if (tv.albumArtist && tv.albumArtistLen) {
    namekey::displayJoin(tv.albumArtist, tv.albumArtistLen, v.join[1], sizeof(v.join[1]));
    const size_t n = std::strlen(v.join[1]);
    if (n) {
      albumArtistOff = runString(v.join[1], n, artistFolderName, &albumArtistEntry);
      if (albumArtistOff == kNone) return Add::NoMemory;
    }
  }
  t.trackArtist = trackArtistOff;
  if (vote) {
    if (trackArtistEntry) ++trackArtistEntry->trackArtist;
    if (albumArtistEntry) ++albumArtistEntry->albumArtist;
  }
  // The album's name and year (a loose album keeps "" and no year).
  if (vote && !loose && tv.album && tv.albumLen) {
    Votes::Entry* e = nullptr;
    const uint32_t off = runString(tv.album, tv.albumLen, albumFolderName, &e);
    if (off == kNone) return Add::NoMemory;
    if (e) {
      ++e->album;
      size_t sortLen = 0;
      const char* sortTag = sortTagOf(tv.albumSort, tv.albumSortLen, &sortLen);
      if (e->sort == kNone && sortLen) {
        e->sort = intern(sortTag, sortLen);
        if (e->sort == kNone) return Add::NoMemory;
      }
    }
  }
  if (vote && !loose && tv.year) {
    uint32_t k = 0;
    while (k < v.years && v.year[k] != tv.year) ++k;
    if (k == v.years && v.years < Votes::kYears) v.year[v.years++] = tv.year;
    if (k < v.years) ++v.yearCount[k];
  }
  // The artist's: its album-artist display, else its track-artist display.
  const uint32_t candidate = albumArtistOff != kNone ? albumArtistOff : trackArtistOff;
  if (candidate != kNone && at(artistFolderName)[0]) {
    const char* s = at(candidate);
    const size_t len = std::strlen(s);
    const uint64_t h = cardcontract::fnv1a64(s, len);
    bool fresh = false;
    Votes::ArtistEntry* e = runSlot(v.aslot, Votes::kArtistSlots, v.aUsedList, &v.aUsed, h,
                                    [&](uint32_t off) { return sameName(at(off), s, len); }, &fresh);
    if (e) {
      if (fresh) {
        e->off = candidate;
        e->sort = kNone;
      }
      ++e->count;
      size_t sortLen = 0;
      const char* sortTag = albumArtistOff != kNone ? sortTagOf(tv.albumArtistSort, tv.albumArtistSortLen, &sortLen)
                                                    : sortTagOf(tv.artistSort, tv.artistSortLen, &sortLen);
      if (e->sort == kNone && sortLen) {
        e->sort = intern(sortTag, sortLen);
        if (e->sort == kNone) return Add::NoMemory;
      }
    }
  }
  if (!push(tracksB_, t)) return Add::NoMemory;
  return Add::Added;
}

bool LibraryIndex::setFolderFacts(const char* folderPath, const FolderFacts& facts) {
  if (!building_ || !folderPath) return false;
  if (failed_) return false;
  const char* root = at(foldersB_.data[0].name);
  if (std::strncmp(folderPath, root, rootLen_) != 0) return false;
  uint32_t folder = 0;
  if (folderPath[rootLen_] == '/') {
    const char* p = folderPath + rootLen_ + 1;
    const char* end = p + std::strlen(p);
    while (p < end) {
      const char* q = static_cast<const char*>(std::memchr(p, '/', static_cast<size_t>(end - p)));
      if (!q) q = end;
      if (q > p) {
        folder = folderChild(folder, p, static_cast<size_t>(q - p));
        if (folder == kNone) return false;
      }
      p = q + 1;
    }
  } else if (folderPath[rootLen_] != 0) {
    return false;
  }
  uint32_t image = kNone;
  uint8_t rank = kNoImage;
  if (facts.image) {
    const size_t len = std::strlen(facts.image);
    rank = imageRank(facts.image, len);
    if (rank != kNoImage) {
      image = intern(facts.image, len);
      if (image == kNone) return false;
    }
  }
  Folder& f = foldersB_.data[folder];
  f.image = image;
  f.imageRank = image == kNone ? kNoImage : rank;
  f.imageCount = facts.imageCount;
  f.otherCount = facts.otherCount;
  folderMarks_.data[folder] = static_cast<uint8_t>((folderMarks_.data[folder] & ~kMarkImageOwned) |
                                                   (image != kNone && facts.imageOwned ? kMarkImageOwned : 0));
  return true;
}

void LibraryIndex::setThumbFolders(const uint64_t* sortedHashes, uint32_t n) {
  thumbs_ = sortedHashes;
  thumbN_ = sortedHashes ? n : 0;
}

// The path hash (2.3.3) of a folder of the build: FNV-1a 64 of the root's
// path, then "/" and each name.
uint64_t LibraryIndex::folderHash(uint32_t folder) const {
  uint32_t depth = 0;
  for (uint32_t f = folder; f != 0; f = foldersB_.data[f].parent) ++depth;
  uint64_t h = cardcontract::fnv1a64Str(at(foldersB_.data[0].name));
  for (uint32_t level = depth; level > 0; --level) {
    uint32_t f = folder;
    for (uint32_t k = 1; k < level; ++k) f = foldersB_.data[f].parent;
    h = cardcontract::fnv1a64Str(at(foldersB_.data[f].name), cardcontract::fnv1a64("/", 1, h));
  }
  return h;
}

// The views' words: artistsAZ, albumsAZ, albumsByArtist, tracksByAlbum,
// folderChildren, folderTree, then each artist's and each album's sort key.
size_t LibraryIndex::viewWords(uint32_t nT, uint32_t nA, uint32_t nB, uint32_t nF) {
  return static_cast<size_t>(nA) + nB + nB + nT + nF + nT + nA + nB;
}

void LibraryIndex::placeViews(uint32_t nT, uint32_t nA, uint32_t nB, uint32_t nF) {
  artistsAZ_ = viewsBlock_;
  albumsAZ_ = artistsAZ_ + nA;
  albumsByArtist_ = albumsAZ_ + nB;
  tracksByAlbum_ = albumsByArtist_ + nB;
  folderChildren_ = tracksByAlbum_ + nT;
  folderTree_ = folderChildren_ + nF;
  artistSortKeys_ = folderTree_ + nT;
  albumSortKeys_ = artistSortKeys_ + nA;
}

bool LibraryIndex::buildViews() {
  const uint32_t nT = tracksB_.size, nA = artistsB_.size, nB = albumsB_.size, nF = foldersB_.size;
  const size_t words = viewWords(nT, nA, nB, nF);
  viewsBytes_ = (words ? words : 1) * sizeof(uint32_t);
  viewsBlock_ = static_cast<uint32_t*>(alloc(viewsBytes_));
  // Temporary, in one block: an artist's place in A-Z, an album's in
  // albumsByArtist, a folder's in the folder tree (depth first, A-Z), the
  // walk's stack, and where each folder's tracks start in the tree view.
  const size_t tempBytes = (static_cast<size_t>(nA) + nB + 3 * static_cast<size_t>(nF) + 6) * sizeof(uint32_t);
  auto* temp = static_cast<uint32_t*>(alloc(tempBytes));
  if (!viewsBlock_ || !temp) {
    release(temp, tempBytes);
    return false;
  }
  uint32_t* artistRank = temp;
  uint32_t* albumPos = artistRank + nA + 1;
  uint32_t* folderRank = albumPos + nB + 1;
  uint32_t* stack = folderRank + nF + 1;
  uint32_t* rankStart = stack + nF + 1;
  placeViews(nT, nA, nB, nF);

  auto str = [&](uint32_t off) -> const char* { return at(off); };
  Track* tracks = tracksB_.data;
  Artist* artists = artistsB_.data;
  Album* albums = albumsB_.data;
  Folder* folders = foldersB_.data;
  // What the lists sort by: the elected sort tag, else the name. Kept with
  // the views (the UI's rail and jump grid key on it too), the build's own
  // tags dropped after.
  for (uint32_t a = 0; a < nA; ++a) {
    artistSortKeys_[a] = artistSort_.data[a] != kNone ? artistSort_.data[a] : artists[a].name;
  }
  for (uint32_t b = 0; b < nB; ++b) {
    albumSortKeys_[b] = albumSort_.data[b] != kNone ? albumSort_.data[b] : albums[b].name;
  }
  auto artistKey = [&](uint32_t a) { return at(artistSortKeys_[a]); };
  auto albumKey = [&](uint32_t b) { return at(albumSortKeys_[b]); };

  // Artists A-Z, "The Lantern Choir" under L (textfold::sortName()).
  for (uint32_t i = 0; i < nA; ++i) artistsAZ_[i] = i;
  std::sort(artistsAZ_, artistsAZ_ + nA, [&](uint32_t a, uint32_t b) {
    const int c = textfold::compareSorted(artistKey(a), artistKey(b));
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t i = 0; i < nA; ++i) artistRank[artistsAZ_[i]] = i;

  // Albums A-Z (ties: by artist), and by artist, newest first.
  for (uint32_t i = 0; i < nB; ++i) albumsAZ_[i] = albumsByArtist_[i] = i;
  std::sort(albumsAZ_, albumsAZ_ + nB, [&](uint32_t a, uint32_t b) {
    const int c = textfold::compareSorted(albumKey(a), albumKey(b));
    if (c != 0) return c < 0;
    const uint32_t ra = artistRank[albums[a].artist], rb = artistRank[albums[b].artist];
    return ra != rb ? ra < rb : a < b;
  });
  std::sort(albumsByArtist_, albumsByArtist_ + nB, [&](uint32_t a, uint32_t b) {
    const uint32_t ra = artistRank[albums[a].artist], rb = artistRank[albums[b].artist];
    if (ra != rb) return ra < rb;
    const uint16_t ya = albums[a].year, yb = albums[b].year;
    if (ya != yb) return ya != 0 && (yb == 0 || ya > yb);  // newest first, no year last
    const int c = textfold::compareSorted(albumKey(a), albumKey(b));
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t i = 0; i < nA; ++i) artists[i].firstAlbum = artists[i].albumCount = 0;
  for (uint32_t i = 0; i < nB; ++i) {
    const uint32_t id = albumsByArtist_[i];
    albumPos[id] = i;
    Artist& ar = artists[albums[id].artist];
    if (ar.albumCount++ == 0) ar.firstAlbum = i;
  }

  // Folder tree. First what's under each folder: its own audio files, then
  // every folder's added to its parent's (children come after their
  // parents: a folder's id is always above its parent's). A folder with no
  // audio anywhere under it (artwork alone) stays out of the views.
  for (uint32_t i = 0; i < nF; ++i) {
    folders[i].firstFolder = folders[i].folderCount = 0;
    folders[i].firstFile = folders[i].fileCount = folders[i].treeCount = 0;
  }
  for (uint32_t i = 0; i < nT; ++i) ++folders[tracks[i].folder].fileCount;
  for (uint32_t i = 0; i < nF; ++i) folders[i].treeCount = folders[i].fileCount;
  for (uint32_t i = nF; i-- > 1;) folders[folders[i].parent].treeCount += folders[i].treeCount;
  // Each folder's folders, A-Z.
  uint32_t nSub = 0;
  for (uint32_t i = 1; i < nF; ++i) {
    if (folders[i].treeCount) folderChildren_[nSub++] = i;
  }
  for (uint32_t i = nSub; i < nF; ++i) folderChildren_[i] = 0;  // unused: saved as zeros
  std::sort(folderChildren_, folderChildren_ + nSub, [&](uint32_t a, uint32_t b) {
    if (folders[a].parent != folders[b].parent) return folders[a].parent < folders[b].parent;
    const int c = textfold::compare(str(folders[a].name), str(folders[b].name));
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t i = 0; i < nSub; ++i) {
    Folder& parent = folders[folders[folderChildren_[i]].parent];
    if (parent.folderCount++ == 0) parent.firstFolder = i;
  }
  // Each folder's place in a depth-first walk: a folder before its
  // subfolders, those A-Z. So an album's own files come first, then CD1,
  // CD2 and so on; and a folder's whole tree is one run of places.
  for (uint32_t i = 0; i < nF; ++i) folderRank[i] = kNone;
  uint32_t nRanks = 0;
  if (nF) {
    uint32_t sp = 0;
    stack[sp++] = 0;
    while (sp) {
      const uint32_t f = stack[--sp];
      folderRank[f] = nRanks++;
      for (uint32_t k = folders[f].folderCount; k-- > 0;) stack[sp++] = folderChildren_[folders[f].firstFolder + k];
    }
  }

  // The folder tree's tracks: folder by folder in that walk, each folder's
  // files A-Z. So a folder's own files (filesIn) and its whole tree
  // (treeTracks) are runs that start at the same place.
  for (uint32_t i = 0; i < nT; ++i) folderTree_[i] = i;
  std::sort(folderTree_, folderTree_ + nT, [&](uint32_t a, uint32_t b) {
    const uint32_t ra = folderRank[tracks[a].folder], rb = folderRank[tracks[b].folder];
    if (ra != rb) return ra < rb;
    const int c = textfold::compare(str(tracks[a].name), str(tracks[b].name));
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t r = 0; r <= nRanks; ++r) rankStart[r] = kNone;
  for (uint32_t i = 0; i < nT; ++i) {
    const uint32_t r = folderRank[tracks[folderTree_[i]].folder];
    if (rankStart[r] == kNone) rankStart[r] = i;
  }
  rankStart[nRanks] = nT;
  for (uint32_t r = nRanks; r-- > 0;) {
    if (rankStart[r] == kNone) rankStart[r] = rankStart[r + 1];  // no files of its own: where the next one's start
  }
  for (uint32_t i = 0; i < nF; ++i) {
    if (folderRank[i] != kNone) folders[i].firstFile = rankStart[folderRank[i]];
  }

  // The names the records didn't give: each folder's files are one run of
  // the tree's tracks.
  readNames(str, tracks, artistKey_.data, folderTree_, nT);
  // An album's discs: its highest (none counts as 1).
  for (uint32_t i = 0; i < nB; ++i) albums[i].discs = 1;
  for (uint32_t i = 0; i < nT; ++i) {
    Album& al = albums[tracks[i].album];
    if (tracks[i].disc > al.discs) al.discs = tracks[i].disc;
  }

  // Tracks: album after album (so artist after artist). Inside an album
  // with records (5.4): by disc (none counts as 1), then folder by folder,
  // then number (none last), then name. Without: folder by folder (discs in
  // their own subfolders stay apart), then the disc a name gives ("2-03
  // Title"), number (none first), then name, as before.
  for (uint32_t i = 0; i < nT; ++i) tracksByAlbum_[i] = i;
  std::sort(tracksByAlbum_, tracksByAlbum_ + nT, [&](uint32_t a, uint32_t b) {
    const Track& x = tracks[a];
    const Track& y = tracks[b];
    if (x.album != y.album) return albumPos[x.album] < albumPos[y.album];
    if (albums[x.album].flags & kTagged) {
      const uint32_t dx = x.disc ? x.disc : 1, dy = y.disc ? y.disc : 1;
      if (dx != dy) return dx < dy;
      if (x.folder != y.folder) return folderRank[x.folder] < folderRank[y.folder];
      if (x.number != y.number) return x.number != 0 && (y.number == 0 || x.number < y.number);
    } else {
      if (x.folder != y.folder) return folderRank[x.folder] < folderRank[y.folder];
      if (x.disc != y.disc) return x.disc < y.disc;
      if (x.number != y.number) return x.number < y.number;
    }
    const int c = textfold::compare(str(x.name), str(y.name));
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t i = 0; i < nB; ++i) albums[i].firstTrack = albums[i].trackCount = 0;
  for (uint32_t i = 0; i < nA; ++i) artists[i].firstTrack = artists[i].trackCount = 0;
  for (uint32_t i = 0; i < nT; ++i) {
    const Track& t = tracks[tracksByAlbum_[i]];
    Album& al = albums[t.album];
    if (al.trackCount++ == 0) al.firstTrack = i;
    Artist& ar = artists[t.artist];
    if (ar.trackCount++ == 0) ar.firstTrack = i;
  }

  // A-Z buckets: the keys are in order, since compare() sorts '#' first
  // (the sort keys' sort names: "The Lantern Choir" is an L).
  auto buckets = [&](const uint32_t* view, uint32_t n, uint32_t* out, auto keyOf) {
    int next = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const int b = textfold::bucketOf(textfold::railKey(textfold::sortName(keyOf(view[i]))));
      while (next <= b) out[next++] = i;
    }
    while (next <= kBuckets) out[next++] = n;
  };
  buckets(artistsAZ_, nA, artistBuckets_, artistKey);
  buckets(albumsAZ_, nB, albumBuckets_, albumKey);

  // The transfer's thumbnails (2.14.3, step 1): an album folder with THUMB
  // in T, unless its folder or its first track's holds an image the
  // transfer's ledger doesn't list (one the listener added wins).
  if (thumbN_) {
    for (uint32_t i = 0; i < nB; ++i) {
      Album& al = albums[i];
      const uint64_t h = folderHash(al.folder);
      if (!std::binary_search(thumbs_, thumbs_ + thumbN_, h)) continue;
      auto handCover = [&](uint32_t f) {
        return kHandCoverBeatsThumbnail && folders[f].image != kNone && !(folderMarks_.data[f] & kMarkImageOwned);
      };
      if (handCover(al.folder)) continue;
      if (al.trackCount && handCover(tracks[tracksByAlbum_[al.firstTrack]].folder)) continue;
      al.flags |= kTransferThumb;
    }
  }

  release(temp, tempBytes);
  return true;
}

void LibraryIndex::setReadPointers() {
  tracks_ = tracksB_.data;
  artists_ = artistsB_.data;
  albums_ = albumsB_.data;
  folders_ = foldersB_.data;
  trackN_ = tracksB_.size;
  artistN_ = artistsB_.size;
  albumN_ = albumsB_.size;
  folderN_ = foldersB_.size;
}

uint64_t LibraryIndex::computeStamp() const {
  uint64_t h = cardcontract::kFnvBasis;
  for (uint32_t i = 0; i < chunks_; ++i) h = cardcontract::fnv1a64(chunk_[i], i + 1 < chunks_ ? kChunkBytes : lastUsed_, h);
  uint8_t w[8];
  for (uint32_t i = 0; i < foldersB_.size; ++i) {
    cardcontract::put32(w, foldersB_.data[i].name);
    cardcontract::put32(w + 4, foldersB_.data[i].parent);
    h = cardcontract::fnv1a64(w, 8, h);
  }
  for (uint32_t i = 0; i < tracksB_.size; ++i) {
    cardcontract::put32(w, tracksB_.data[i].name);
    cardcontract::put32(w + 4, tracksB_.data[i].folder);
    h = cardcontract::fnv1a64(w, 8, h);
  }
  return h;
}

bool LibraryIndex::finish() {
  if (!building_) return ready_;
  closeAlbumRun();
  closeArtistRun();
  building_ = false;
  dropVotes();
  tableDrop(folderTable_);
  tableDrop(artistTable_);
  if (!failed_ && !buildViews()) failed_ = true;
  drop(folderAlbum_);
  drop(folderMarks_);
  drop(artistKey_);
  drop(artistSort_);
  drop(albumSort_);
  thumbs_ = nullptr;
  thumbN_ = 0;
  roots_ = nullptr;
  rootCount_ = 0;
  if (failed_) {
    const size_t peak = buildPeak_;
    clear();
    failed_ = true;
    buildPeak_ = peak;
    return false;
  }
  trim(tracksB_);
  trim(artistsB_);
  trim(albumsB_);
  trim(foldersB_);
  trimArena();
  setReadPointers();
  buildStamp_ = computeStamp();
  ready_ = true;
  return true;
}

void LibraryIndex::forgetSources() {
  for (uint32_t i = 0; i < tracksB_.size; ++i) tracksB_.data[i].flags &= static_cast<uint8_t>(~kSourceMask);
}

void LibraryIndex::forgetLengths() {
  for (uint32_t i = 0; i < tracksB_.size; ++i) tracksB_.data[i].durationS = 0;
}

size_t LibraryIndex::folderPath(uint32_t id, char* buf, size_t size) const {
  if (size == 0) return 0;
  buf[0] = 0;
  if (!ready_ || id >= folderN_) return 0;
  uint32_t chain[32];
  int depth = 0;
  for (uint32_t f = id; f != kNone; f = folders_[f].parent) {
    if (depth == 32) return 0;
    chain[depth++] = f;
  }
  size_t n = 0;
  for (int i = depth - 1; i >= 0; --i) {
    const char* name = str(folders_[chain[i]].name);
    const size_t len = std::strlen(name);
    const size_t need = len + (i == depth - 1 ? 0 : 1);
    if (n + need + 1 > size) {
      buf[0] = 0;
      return 0;
    }
    if (i != depth - 1) buf[n++] = '/';
    std::memcpy(buf + n, name, len);
    n += len;
  }
  buf[n] = 0;
  return n;
}

size_t LibraryIndex::trackPath(uint32_t id, char* buf, size_t size) const {
  if (size == 0) return 0;
  buf[0] = 0;
  if (!ready_ || id >= trackN_) return 0;
  size_t n = folderPath(tracks_[id].folder, buf, size);
  if (n == 0) return 0;
  const char* name = str(tracks_[id].name);
  const size_t len = std::strlen(name);
  if (n + 1 + len + 1 > size) {
    buf[0] = 0;
    return 0;
  }
  buf[n++] = '/';
  std::memcpy(buf + n, name, len + 1);
  return n + len;
}

uint32_t LibraryIndex::albumCover(uint32_t album) const {
  if (!ready_ || album >= albumN_) return kNone;
  const uint32_t f = albums_[album].folder;
  if (f < folderN_ && folders_[f].image != kNone) return f;
  const Span t = tracksOfAlbum(album);
  if (t.count == 0) return kNone;
  const uint32_t g = tracks_[t[0]].folder;
  return g < folderN_ && folders_[g].image != kNone ? g : kNone;
}

size_t LibraryIndex::imagePath(uint32_t folder, char* buf, size_t size) const {
  if (size == 0) return 0;
  buf[0] = 0;
  if (!ready_ || folder >= folderN_ || folders_[folder].image == kNone) return 0;
  size_t n = folderPath(folder, buf, size);
  if (n == 0) return 0;
  const char* name = str(folders_[folder].image);
  const size_t len = std::strlen(name);
  if (n + 1 + len + 1 > size) {
    buf[0] = 0;
    return 0;
  }
  buf[n++] = '/';
  std::memcpy(buf + n, name, len + 1);
  return n + len;
}

uint32_t LibraryIndex::bucketStart(View view, int bucket) const {
  if (bucket < 0) bucket = 0;
  if (bucket > kBuckets) bucket = kBuckets;
  return view == View::Artists ? artistBuckets_[bucket] : albumBuckets_[bucket];
}

int LibraryIndex::bucketAt(View view, uint32_t position) const {
  const uint32_t* b = view == View::Artists ? artistBuckets_ : albumBuckets_;
  const uint32_t n = view == View::Artists ? artistN_ : albumN_;
  if (position >= n) return kBuckets - 1;
  // The last bucket that starts at or before it (an empty bucket starts
  // where the next one does, so that one is never empty).
  int k = kBuckets - 1;
  while (k > 0 && b[k] > position) --k;
  return k;
}

LibraryIndex::Memory LibraryIndex::memory() const {
  Memory m;
  m.strings = arenaHeld();
  m.tracks = bytes(tracksB_);
  m.artists = bytes(artistsB_);
  m.albums = bytes(albumsB_);
  m.folders = bytes(foldersB_);
  m.views = viewsBytes_;
  m.total = m.strings + m.tracks + m.artists + m.albums + m.folders + m.views;
  m.buildPeak = buildPeak_;
  return m;
}

uint32_t LibraryIndex::findTrack(const char* path) const {
  if (!ready_ || !path) return kNone;
  const char* root = folderName(rootFolder());
  const size_t rootLen = std::strlen(root);
  if (std::strncmp(path, root, rootLen) != 0 || path[rootLen] != '/') return kNone;
  char name[256];
  uint32_t folder = rootFolder();
  for (const char* p = path + rootLen + 1;;) {
    const char* slash = std::strchr(p, '/');
    const size_t len = slash ? static_cast<size_t>(slash - p) : std::strlen(p);
    if (len >= sizeof(name)) return kNone;
    if (slash && len == 0) {  // "a//b": addFile() skips empty names too
      p = slash + 1;
      continue;
    }
    std::memcpy(name, p, len);
    name[len] = 0;
    if (!slash) {
      const Span files = filesIn(folder);
      return findByName(files.ids, files.count, name, [&](uint32_t id) { return trackFileName(id); });
    }
    const Span sub = subfolders(folder);
    folder = findByName(sub.ids, sub.count, name, [&](uint32_t id) { return folderName(id); });
    if (folder == kNone) return kNone;
    p = slash + 1;
  }
}

bool LibraryIndex::save(ByteSink& out, const Inputs& in) const {
  if (!ready_) return false;
  uint32_t h[kHeaderWords] = {};
  h[0] = kMagic;
  h[1] = kVersion;
  h[2] = recordSizes();
  h[3] = kRulesVersion;
  h[4] = static_cast<uint32_t>(in.walkSignature);
  h[5] = static_cast<uint32_t>(in.walkSignature >> 32);
  h[6] = static_cast<uint32_t>(in.cardId);
  h[7] = static_cast<uint32_t>(in.cardId >> 32);
  h[8] = in.generation;
  h[9] = static_cast<uint32_t>(in.commitId);
  h[10] = static_cast<uint32_t>(in.commitId >> 32);
  h[11] = in.tagsCrc;
  h[12] = in.transfer ? 1u : 0u;
  h[13] = in.deviceCrc;
  h[14] = in.journalSeq;
  h[15] = static_cast<uint32_t>(buildStamp_);
  h[16] = static_cast<uint32_t>(buildStamp_ >> 32);
  h[17] = arenaBytes();
  h[18] = trackN_;
  h[19] = artistN_;
  h[20] = albumN_;
  h[21] = folderN_;
  std::memcpy(h + kCountWords, artistBuckets_, sizeof(artistBuckets_));
  std::memcpy(h + kCountWords + kBuckets + 1, albumBuckets_, sizeof(albumBuckets_));
  SummingSink s(out);
  bool ok = s.write(h, sizeof(h));
  for (uint32_t i = 0; ok && i < chunks_; ++i) ok = s.write(chunk_[i], i + 1 < chunks_ ? kChunkBytes : lastUsed_);
  ok = ok && s.write(tracks_, static_cast<size_t>(trackN_) * sizeof(Track)) &&
       s.write(artists_, static_cast<size_t>(artistN_) * sizeof(Artist)) &&
       s.write(albums_, static_cast<size_t>(albumN_) * sizeof(Album)) &&
       s.write(folders_, static_cast<size_t>(folderN_) * sizeof(Folder)) && s.write(viewsBlock_, viewsBytes_);
  const uint32_t sum = s.sum.h;
  return ok && out.write(&sum, sizeof(sum));
}

bool LibraryIndex::save(ByteSink& out, uint64_t signature) const {
  Inputs in;
  in.walkSignature = signature;
  return save(out, in);
}

template <typename T>
bool LibraryIndex::readBlock(ByteSource& in, Block<T>& b, uint32_t n) {
  // A kept block (keepTrackBlock()) is taken when the records fit, else
  // freed first.
  if (b.data && (n == 0 || b.cap < n)) drop(b);
  if (n == 0) return true;
  if (!b.data) {
    b.data = static_cast<T*>(alloc(static_cast<size_t>(n) * sizeof(T)));
    if (!b.data) return false;
    b.cap = n;
  }
  b.size = n;
  if (!readFully(in, b.data, static_cast<size_t>(n) * sizeof(T))) return false;
  trim(b);  // a kept block bigger than the file's table: to its size
  return true;
}

namespace {

// The header (load()'s and peek()'s): the magic and the version first (an
// older version's header is shorter), then the rest, the record sizes, the
// rules and the inputs. Loaded: `h` holds it all.
LibraryIndex::Load readHeader(ByteSource& s, uint32_t* h, LibraryIndex::Inputs* got) {
  using Load = LibraryIndex::Load;
  if (!readFully(s, h, 2 * sizeof(uint32_t)) || h[0] != kMagic) return Load::Corrupt;
  if (h[1] != kVersion) return h[1] >= 1 && h[1] < kVersion ? Load::Outdated : Load::Corrupt;
  if (!readFully(s, h + 2, (kHeaderWords - 2) * sizeof(uint32_t))) return Load::Corrupt;
  if (h[2] != recordSizes()) return Load::Corrupt;
  if (h[3] != LibraryIndex::kRulesVersion) return Load::Outdated;
  got->walkSignature = static_cast<uint64_t>(h[5]) << 32 | h[4];
  got->cardId = static_cast<uint64_t>(h[7]) << 32 | h[6];
  got->generation = h[8];
  got->commitId = static_cast<uint64_t>(h[10]) << 32 | h[9];
  got->tagsCrc = h[11];
  got->transfer = (h[12] & 1u) != 0;
  got->deviceCrc = h[13];
  got->journalSeq = h[14];
  return Load::Loaded;
}

}  // namespace

LibraryIndex::Load LibraryIndex::peek(ByteSource& in, Inputs* out) {
  uint32_t h[kHeaderWords];
  Inputs got;
  const Load r = readHeader(in, h, &got);
  if (r == Load::Loaded && out) *out = got;
  return r;
}

void LibraryIndex::clearPending(uint32_t id) {
  if (id < tracksB_.size) tracksB_.data[id].flags &= static_cast<uint8_t>(~kTrackPending);
}

LibraryIndex::Load LibraryIndex::load(ByteSource& in, const Inputs& expect) {
  clear();
  SummingSource s(in);
  uint32_t h[kHeaderWords];
  Inputs got;
  const Load head = readHeader(s, h, &got);
  if (head != Load::Loaded) return head;
  if (!got.sameHard(expect)) return Load::Stale;
  const uint32_t arenaBytes = h[17], nT = h[18], nA = h[19], nB = h[20], nF = h[21];
  if (arenaBytes == 0 || arenaBytes > kMaxChunks * kChunkBytes || nT > kMaxRecords || nA > kMaxRecords ||
      nB > kMaxRecords || nF == 0 || nF > kMaxRecords) {
    return Load::Corrupt;
  }
  const size_t words = viewWords(nT, nA, nB, nF);
  const size_t viewsBytes = (words ? words : 1) * sizeof(uint32_t);

  // Into blocks of exactly the saved size; on any failure clear() gives
  // every block back.
  auto fail = [&](Load why) {
    clear();
    return why;
  };
  whole_ = static_cast<char*>(alloc(arenaBytes));
  if (!whole_) return fail(Load::NoMemory);
  wholeBytes_ = arenaBytes;
  if (!readFully(s, whole_, arenaBytes)) return fail(Load::Corrupt);
  chunks_ = (arenaBytes + kChunkBytes - 1) / kChunkBytes;
  for (uint32_t i = 0; i < chunks_; ++i) chunk_[i] = whole_ + static_cast<size_t>(i) * kChunkBytes;
  lastUsed_ = lastCap_ = arenaBytes - (chunks_ - 1) * kChunkBytes;
  bool noMemory = false;
  auto block = [&](auto& b, uint32_t n) {
    if (readBlock(s, b, n)) return true;
    noMemory = !b.data;
    return false;
  };
  if (!block(tracksB_, nT) || !block(artistsB_, nA) || !block(albumsB_, nB) || !block(foldersB_, nF)) {
    return fail(noMemory ? Load::NoMemory : Load::Corrupt);
  }
  viewsBlock_ = static_cast<uint32_t*>(alloc(viewsBytes));
  if (!viewsBlock_) return fail(Load::NoMemory);
  viewsBytes_ = viewsBytes;
  if (!readFully(s, viewsBlock_, viewsBytes)) return fail(Load::Corrupt);
  const uint32_t want = s.sum.h;
  uint32_t sum = 0;
  if (!readFully(in, &sum, sizeof(sum)) || sum != want || whole_[arenaBytes - 1] != 0) {
    return fail(Load::Corrupt);
  }

  std::memcpy(artistBuckets_, h + kCountWords, sizeof(artistBuckets_));
  std::memcpy(albumBuckets_, h + kCountWords + kBuckets + 1, sizeof(albumBuckets_));
  placeViews(nT, nA, nB, nF);
  setReadPointers();
  inputs_ = got;
  buildStamp_ = static_cast<uint64_t>(h[16]) << 32 | h[15];
  ready_ = true;
  return Load::Loaded;
}

LibraryIndex::Load LibraryIndex::load(ByteSource& in, uint64_t signature) {
  Inputs expect;
  expect.walkSignature = signature;
  return load(in, expect);
}
