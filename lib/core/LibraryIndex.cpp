// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryIndex.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "TextFold.h"

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

bool isSeparator(char c) { return c == ' ' || c == '-' || c == '.' || c == '_'; }

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

// ---- the cache file ----
// Little-endian 32-bit words: the header, then the blocks as they are in
// memory (the arena, the four record tables, the views), then an FNV-1a sum
// of everything before it. Same compiler, same layout: a record size that
// changed makes the file Corrupt, and the version is bumped when a record's
// meaning changes.
constexpr uint32_t kMagic = 0x494C504Du;  // "MPLI"
constexpr uint32_t kVersion = 2;  // 2: folders count their other files and pick a cover
constexpr int kCountWords = 10;           // magic .. folders
constexpr int kHeaderWords = kCountWords + 2 * (LibraryIndex::kBuckets + 1);
constexpr uint32_t kMaxRecords = 1u << 22;  // a damaged header must not ask for gigabytes
constexpr uint32_t kMaxArena = 256u << 20;

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

LibraryIndex::LibraryIndex(AllocFn alloc, FreeFn release)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree) {}

LibraryIndex::~LibraryIndex() { clear(); }

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

template <typename T>
void LibraryIndex::trim(Block<T>& b) {
  if (b.cap == b.size) return;
  if (b.size == 0) {
    drop(b);
    return;
  }
  T* p = static_cast<T*>(alloc(static_cast<size_t>(b.size) * sizeof(T)));
  if (!p) return;  // keep the slack rather than fail
  std::memcpy(p, b.data, static_cast<size_t>(b.size) * sizeof(T));
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

void LibraryIndex::resetViews() {
  release(viewsBlock_, viewsBytes_);
  viewsBlock_ = nullptr;
  viewsBytes_ = 0;
  artistsAZ_ = albumsAZ_ = albumsByArtist_ = tracksByAlbum_ = folderChildren_ = folderTree_ = nullptr;
  std::memset(artistBuckets_, 0, sizeof(artistBuckets_));
  std::memset(albumBuckets_, 0, sizeof(albumBuckets_));
}

void LibraryIndex::clear() {
  drop(arenaB_);
  drop(tracksB_);
  drop(artistsB_);
  drop(albumsB_);
  drop(foldersB_);
  tableDrop(folderTable_);
  tableDrop(artistTable_);
  tableDrop(albumTable_);
  resetViews();
  arena_ = nullptr;
  tracks_ = nullptr;
  artists_ = nullptr;
  albums_ = nullptr;
  folders_ = nullptr;
  trackN_ = artistN_ = albumN_ = folderN_ = 0;
  rootLen_ = 0;
  emptyName_ = kNone;
  building_ = false;
  failed_ = false;
  ready_ = false;
  buildPeak_ = held_;  // 0: everything is released above
}

uint32_t LibraryIndex::intern(const char* s, size_t len) {
  if (!reserve(arenaB_, arenaB_.size + static_cast<uint32_t>(len) + 1)) return kNone;
  const uint32_t off = arenaB_.size;
  std::memcpy(arenaB_.data + off, s, len);
  arenaB_.data[off + len] = 0;
  arenaB_.size += static_cast<uint32_t>(len) + 1;
  return off;
}

bool LibraryIndex::begin(const char* root, uint32_t expectTracks) {
  clear();
  building_ = true;
  size_t rootLen = std::strlen(root);
  while (rootLen > 1 && root[rootLen - 1] == '/') --rootLen;
  const uint32_t n = expectTracks ? expectTracks : 256;
  const bool ok = reserve(arenaB_, n * 40 + 1024) && reserve(tracksB_, n) && reserve(artistsB_, n / 16 + 16) &&
                  reserve(albumsB_, n / 6 + 16) && reserve(foldersB_, n / 5 + 16) &&
                  tableInit(folderTable_, pow2AtLeast(n / 5 * 2)) && tableInit(artistTable_, pow2AtLeast(n / 16 * 2)) &&
                  tableInit(albumTable_, pow2AtLeast(n / 6 * 2));
  if (!ok) {
    failed_ = true;
    return false;
  }
  const uint32_t rootName = intern(root, rootLen);
  emptyName_ = intern("", 0);
  if (rootName == kNone || emptyName_ == kNone) return false;
  rootLen_ = static_cast<uint32_t>(rootLen);
  return push(foldersB_, newFolder(rootName, kNone));
}

uint32_t LibraryIndex::folderChild(uint32_t parent, const char* name, size_t len) {
  const uint32_t h = fnv(name, len, parent + 1);
  const uint32_t found = tableFind(folderTable_, h, [&](uint32_t id) {
    return foldersB_.data[id].parent == parent && sameName(arenaB_.data + foldersB_.data[id].name, name, len);
  });
  if (found != kNone) return found;
  const uint32_t off = intern(name, len);
  if (off == kNone) return kNone;
  const uint32_t id = foldersB_.size;
  if (!push(foldersB_, newFolder(off, parent))) return kNone;
  if (!tableInsert(folderTable_, h, id)) return kNone;
  return id;
}

uint32_t LibraryIndex::artistFor(uint32_t nameOffset) {
  const char* name = arenaB_.data + nameOffset;
  const size_t len = std::strlen(name);
  const uint32_t h = fnv(name, len, 0);
  const uint32_t found = tableFind(artistTable_, h, [&](uint32_t id) {
    return sameName(arenaB_.data + artistsB_.data[id].name, arenaB_.data + nameOffset, len);
  });
  if (found != kNone) return found;
  const uint32_t id = artistsB_.size;
  if (!push(artistsB_, Artist{nameOffset, 0, 0, 0, 0})) return kNone;
  if (!tableInsert(artistTable_, h, id)) return kNone;
  return id;
}

uint32_t LibraryIndex::albumFor(uint32_t artist, uint32_t nameOffset, uint32_t folder) {
  const char* name = arenaB_.data + nameOffset;
  const size_t len = std::strlen(name);
  const uint32_t h = fnv(name, len, artist + 0x51ED27u);
  const uint32_t found = tableFind(albumTable_, h, [&](uint32_t id) {
    return albumsB_.data[id].artist == artist &&
           sameName(arenaB_.data + albumsB_.data[id].name, arenaB_.data + nameOffset, len);
  });
  if (found != kNone) return found;
  const uint32_t id = albumsB_.size;
  if (!push(albumsB_, Album{nameOffset, artist, folder, 0, 0})) return kNone;
  if (!tableInsert(albumTable_, h, id)) return kNone;
  return id;
}

uint32_t LibraryIndex::folderOf(const char* path, const char* lastSlash, int* depth, uint32_t* artistFolder,
                                uint32_t* albumFolder) {
  uint32_t folder = 0;
  *depth = 0;
  *artistFolder = *albumFolder = kNone;
  for (const char* p = path + rootLen_ + 1; p < lastSlash;) {
    const char* q = static_cast<const char*>(std::memchr(p, '/', static_cast<size_t>(lastSlash - p)));
    if (!q) q = lastSlash;
    if (q > p) {
      folder = folderChild(folder, p, static_cast<size_t>(q - p));
      if (folder == kNone) return kNone;
      ++*depth;
      if (*depth == 1) *artistFolder = folder;
      if (*depth == 2) *albumFolder = folder;
    }
    p = q + 1;
  }
  return folder;
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

// A file that isn't audio: counted in its folder, and its cover if it's the
// best-named image there so far.
LibraryIndex::Add LibraryIndex::addOther(const char* path, const char* lastSlash, const char* leaf, size_t leafLen) {
  int depth;
  uint32_t artistFolder, albumFolder;
  const uint32_t folder = folderOf(path, lastSlash, &depth, &artistFolder, &albumFolder);
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
  }
  return Add::Other;
}

LibraryIndex::Add LibraryIndex::addFile(const char* path) {
  if (!building_ || !path) return Add::Skipped;
  if (failed_) return Add::NoMemory;
  const char* root = arenaB_.data + foldersB_.data[0].name;
  if (std::strncmp(path, root, rootLen_) != 0 || path[rootLen_] != '/') return Add::Skipped;
  const char* lastSlash = std::strrchr(path, '/');
  const char* leaf = lastSlash + 1;
  const size_t leafLen = std::strlen(leaf);
  if (leafLen == 0) return Add::Skipped;
  const char* dot = std::strrchr(leaf, '.');
  Format format = Format::Unknown;
  if (dot && dot != leaf) {
    const size_t extLen = leafLen - static_cast<size_t>(dot + 1 - leaf);
    if (extIs(dot + 1, extLen, "mp3")) {
      format = Format::Mp3;
    } else if (extIs(dot + 1, extLen, "flac")) {
      format = Format::Flac;
    }
  }
  if (format == Format::Unknown) return addOther(path, lastSlash, leaf, leafLen);

  // The folders, creating the ones not seen yet.
  int depth;
  uint32_t artistFolder, albumFolder;
  const uint32_t folder = folderOf(path, lastSlash, &depth, &artistFolder, &albumFolder);
  if (folder == kNone) return Add::NoMemory;

  const uint32_t artist = artistFor(depth >= 1 ? foldersB_.data[artistFolder].name : emptyName_);
  if (artist == kNone) return Add::NoMemory;
  const uint32_t album =
      albumFor(artist, depth >= 2 ? foldersB_.data[albumFolder].name : emptyName_, depth >= 2 ? albumFolder : folder);
  if (album == kNone) return Add::NoMemory;
  const uint32_t name = intern(leaf, leafLen);
  if (name == kNone) return Add::NoMemory;

  // "06 - Title.mp3": number 6, title "Title". Up to 3 leading digits count
  // as the number when a separator follows them.
  const size_t stem = static_cast<size_t>(dot - leaf);
  size_t i = 0;
  uint32_t number = 0;
  while (i < stem && i < 4 && leaf[i] >= '0' && leaf[i] <= '9') number = number * 10 + (leaf[i++] - '0');
  size_t titleAt = 0;
  if (i > 0 && i <= 3 && i < stem && isSeparator(leaf[i]) && number <= 255) {
    titleAt = i;
    while (titleAt < stem && isSeparator(leaf[titleAt])) ++titleAt;
    if (titleAt == stem) titleAt = 0;  // "06.mp3": the whole stem is the title
  } else {
    number = 0;
  }
  size_t titleLen = stem - titleAt;
  if (titleLen > 255) titleLen = 255;

  Track t{};
  t.name = name;
  t.title = name + static_cast<uint32_t>(titleAt);
  t.titleLen = static_cast<uint8_t>(titleLen);
  t.folder = folder;
  t.album = album;
  t.artist = artist;
  t.number = static_cast<uint8_t>(number);
  t.format = format;
  if (!push(tracksB_, t)) return Add::NoMemory;
  return Add::Added;
}

bool LibraryIndex::buildViews() {
  const uint32_t nT = tracksB_.size, nA = artistsB_.size, nB = albumsB_.size, nF = foldersB_.size;
  const size_t words = static_cast<size_t>(nA) + nB + nB + nT + nF + nT;
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
  artistsAZ_ = viewsBlock_;
  albumsAZ_ = artistsAZ_ + nA;
  albumsByArtist_ = albumsAZ_ + nB;
  tracksByAlbum_ = albumsByArtist_ + nB;
  folderChildren_ = tracksByAlbum_ + nT;
  folderTree_ = folderChildren_ + nF;

  const char* s = arenaB_.data;
  Track* tracks = tracksB_.data;
  Artist* artists = artistsB_.data;
  Album* albums = albumsB_.data;
  Folder* folders = foldersB_.data;

  // Artists A-Z.
  for (uint32_t i = 0; i < nA; ++i) artistsAZ_[i] = i;
  std::sort(artistsAZ_, artistsAZ_ + nA, [&](uint32_t a, uint32_t b) {
    const int c = textfold::compare(s + artists[a].name, s + artists[b].name);
    return c != 0 ? c < 0 : a < b;
  });
  for (uint32_t i = 0; i < nA; ++i) artistRank[artistsAZ_[i]] = i;

  // Albums A-Z (ties: by artist), and by artist.
  for (uint32_t i = 0; i < nB; ++i) albumsAZ_[i] = albumsByArtist_[i] = i;
  std::sort(albumsAZ_, albumsAZ_ + nB, [&](uint32_t a, uint32_t b) {
    const int c = textfold::compare(s + albums[a].name, s + albums[b].name);
    if (c != 0) return c < 0;
    const uint32_t ra = artistRank[albums[a].artist], rb = artistRank[albums[b].artist];
    return ra != rb ? ra < rb : a < b;
  });
  std::sort(albumsByArtist_, albumsByArtist_ + nB, [&](uint32_t a, uint32_t b) {
    const uint32_t ra = artistRank[albums[a].artist], rb = artistRank[albums[b].artist];
    if (ra != rb) return ra < rb;
    const int c = textfold::compare(s + albums[a].name, s + albums[b].name);
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
    const int c = textfold::compare(s + folders[a].name, s + folders[b].name);
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

  // Tracks: album after album (so artist after artist); inside an album,
  // folder by folder (discs in their own subfolders stay apart), then by
  // number, then name.
  for (uint32_t i = 0; i < nT; ++i) tracksByAlbum_[i] = i;
  std::sort(tracksByAlbum_, tracksByAlbum_ + nT, [&](uint32_t a, uint32_t b) {
    const Track& x = tracks[a];
    const Track& y = tracks[b];
    if (x.album != y.album) return albumPos[x.album] < albumPos[y.album];
    if (x.folder != y.folder) return folderRank[x.folder] < folderRank[y.folder];
    if (x.number != y.number) return x.number < y.number;
    const int c = textfold::compare(s + x.name, s + y.name);
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

  // The folder tree's tracks: folder by folder in that walk, each folder's
  // files A-Z. So a folder's own files (filesIn) and its whole tree
  // (treeTracks) are runs that start at the same place.
  for (uint32_t i = 0; i < nT; ++i) folderTree_[i] = i;
  std::sort(folderTree_, folderTree_ + nT, [&](uint32_t a, uint32_t b) {
    const uint32_t ra = folderRank[tracks[a].folder], rb = folderRank[tracks[b].folder];
    if (ra != rb) return ra < rb;
    const int c = textfold::compare(s + tracks[a].name, s + tracks[b].name);
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

  // A-Z buckets: the keys are in order, since compare() sorts '#' first.
  auto buckets = [&](const uint32_t* view, uint32_t n, uint32_t* out, auto nameOf) {
    int next = 0;
    for (uint32_t i = 0; i < n; ++i) {
      const int b = textfold::bucketOf(textfold::railKey(nameOf(view[i])));
      while (next <= b) out[next++] = i;
    }
    while (next <= kBuckets) out[next++] = n;
  };
  buckets(artistsAZ_, nA, artistBuckets_, [&](uint32_t id) { return s + artists[id].name; });
  buckets(albumsAZ_, nB, albumBuckets_, [&](uint32_t id) { return s + albums[id].name; });

  release(temp, tempBytes);
  return true;
}

bool LibraryIndex::finish() {
  if (!building_) return ready_;
  building_ = false;
  if (!failed_ && !buildViews()) failed_ = true;
  tableDrop(folderTable_);
  tableDrop(artistTable_);
  tableDrop(albumTable_);
  if (failed_) {
    const size_t peak = buildPeak_;
    clear();
    failed_ = true;
    buildPeak_ = peak;
    return false;
  }
  trim(arenaB_);
  trim(tracksB_);
  trim(artistsB_);
  trim(albumsB_);
  trim(foldersB_);
  arena_ = arenaB_.data;
  tracks_ = tracksB_.data;
  artists_ = artistsB_.data;
  albums_ = albumsB_.data;
  folders_ = foldersB_.data;
  trackN_ = tracksB_.size;
  artistN_ = artistsB_.size;
  albumN_ = albumsB_.size;
  folderN_ = foldersB_.size;
  ready_ = true;
  return true;
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
  if (view == View::Artists) {
    if (position >= artistN_) return kBuckets - 1;
    return textfold::bucketOf(textfold::railKey(artistName(artistsAZ_[position])));
  }
  if (position >= albumN_) return kBuckets - 1;
  return textfold::bucketOf(textfold::railKey(albumName(albumsAZ_[position])));
}

LibraryIndex::Memory LibraryIndex::memory() const {
  Memory m;
  m.strings = bytes(arenaB_);
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

bool LibraryIndex::save(ByteSink& out, uint64_t signature) const {
  if (!ready_) return false;
  uint32_t h[kHeaderWords] = {};
  h[0] = kMagic;
  h[1] = kVersion;
  h[2] = recordSizes();
  h[3] = static_cast<uint32_t>(signature);
  h[4] = static_cast<uint32_t>(signature >> 32);
  h[5] = arenaB_.size;
  h[6] = trackN_;
  h[7] = artistN_;
  h[8] = albumN_;
  h[9] = folderN_;
  std::memcpy(h + kCountWords, artistBuckets_, sizeof(artistBuckets_));
  std::memcpy(h + kCountWords + kBuckets + 1, albumBuckets_, sizeof(albumBuckets_));
  SummingSink s(out);
  const bool ok = s.write(h, sizeof(h)) && s.write(arena_, arenaB_.size) &&
                  s.write(tracks_, static_cast<size_t>(trackN_) * sizeof(Track)) &&
                  s.write(artists_, static_cast<size_t>(artistN_) * sizeof(Artist)) &&
                  s.write(albums_, static_cast<size_t>(albumN_) * sizeof(Album)) &&
                  s.write(folders_, static_cast<size_t>(folderN_) * sizeof(Folder)) &&
                  s.write(viewsBlock_, viewsBytes_);
  const uint32_t sum = s.sum.h;
  return ok && out.write(&sum, sizeof(sum));
}

template <typename T>
bool LibraryIndex::readBlock(ByteSource& in, Block<T>& b, uint32_t n) {
  if (n == 0) return true;
  b.data = static_cast<T*>(alloc(static_cast<size_t>(n) * sizeof(T)));
  if (!b.data) return false;
  b.cap = b.size = n;
  return readFully(in, b.data, static_cast<size_t>(n) * sizeof(T));
}

LibraryIndex::Load LibraryIndex::load(ByteSource& in, uint64_t signature) {
  clear();
  SummingSource s(in);
  uint32_t h[kHeaderWords];
  if (!readFully(s, h, sizeof(h)) || h[0] != kMagic || h[1] != kVersion || h[2] != recordSizes()) {
    return Load::Corrupt;
  }
  if ((static_cast<uint64_t>(h[4]) << 32 | h[3]) != signature) return Load::Stale;
  const uint32_t arenaBytes = h[5], nT = h[6], nA = h[7], nB = h[8], nF = h[9];
  if (arenaBytes == 0 || arenaBytes > kMaxArena || nT > kMaxRecords || nA > kMaxRecords || nB > kMaxRecords ||
      nF == 0 || nF > kMaxRecords) {
    return Load::Corrupt;
  }
  const size_t words = static_cast<size_t>(nA) + nB + nB + nT + nF + nT;
  const size_t viewsBytes = (words ? words : 1) * sizeof(uint32_t);

  // Into blocks of exactly the saved size; on any failure clear() gives
  // every block back.
  auto fail = [&](Load why) {
    clear();
    return why;
  };
  bool noMemory = false;
  auto block = [&](auto& b, uint32_t n) {
    if (readBlock(s, b, n)) return true;
    noMemory = !b.data;
    return false;
  };
  if (!block(arenaB_, arenaBytes) || !block(tracksB_, nT) || !block(artistsB_, nA) || !block(albumsB_, nB) ||
      !block(foldersB_, nF)) {
    return fail(noMemory ? Load::NoMemory : Load::Corrupt);
  }
  viewsBlock_ = static_cast<uint32_t*>(alloc(viewsBytes));
  if (!viewsBlock_) return fail(Load::NoMemory);
  viewsBytes_ = viewsBytes;
  if (!readFully(s, viewsBlock_, viewsBytes)) return fail(Load::Corrupt);
  const uint32_t want = s.sum.h;
  uint32_t sum = 0;
  if (!readFully(in, &sum, sizeof(sum)) || sum != want || arenaB_.data[arenaBytes - 1] != 0) {
    return fail(Load::Corrupt);
  }

  std::memcpy(artistBuckets_, h + kCountWords, sizeof(artistBuckets_));
  std::memcpy(albumBuckets_, h + kCountWords + kBuckets + 1, sizeof(albumBuckets_));
  artistsAZ_ = viewsBlock_;
  albumsAZ_ = artistsAZ_ + nA;
  albumsByArtist_ = albumsAZ_ + nB;
  tracksByAlbum_ = albumsByArtist_ + nB;
  folderChildren_ = tracksByAlbum_ + nT;
  folderTree_ = folderChildren_ + nF;
  arena_ = arenaB_.data;
  tracks_ = tracksB_.data;
  artists_ = artistsB_.data;
  albums_ = albumsB_.data;
  folders_ = foldersB_.data;
  trackN_ = nT;
  artistN_ = nA;
  albumN_ = nB;
  folderN_ = nF;
  ready_ = true;
  return Load::Loaded;
}
