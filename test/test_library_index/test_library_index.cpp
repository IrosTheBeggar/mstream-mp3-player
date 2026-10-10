// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for LibraryIndex (the compact library store, library.idx v6)
// and the synthetic library generator: the path index as it always was, and
// tag records named by Stage A's rules (docs/METADATA.md 5.4).
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ByteStream.h"
#include "CardContract.h"
#include "JumpIndex.h"
#include "LibraryIndex.h"
#include "LibrarySynth.h"
#include "TextFold.h"

namespace {

// Counting allocator: every block the index takes goes through here.
struct Heap {
  static size_t live;
  static size_t peak;  // the most live at once
  static size_t allocs;
  static size_t frees;
  static size_t limit;  // fail allocations once live would pass this
  static std::vector<std::pair<void*, size_t>>& blocks() {
    static std::vector<std::pair<void*, size_t>> b;
    return b;
  }
  // Every size asked since reset() (or since a test cleared it).
  static std::vector<size_t>& sizes() {
    static std::vector<size_t> s;
    return s;
  }
  static void reset() {
    live = peak = allocs = frees = 0;
    limit = SIZE_MAX;
    blocks().clear();
    sizes().clear();
  }
  static void* alloc(size_t n) {
    if (live + n > limit) return nullptr;
    void* p = std::malloc(n ? n : 1);
    blocks().push_back({p, n});
    sizes().push_back(n);
    live += n;
    if (live > peak) peak = live;
    ++allocs;
    return p;
  }
  static void release(void* p) {
    for (size_t i = 0; i < blocks().size(); ++i) {
      if (blocks()[i].first == p) {
        live -= blocks()[i].second;
        blocks().erase(blocks().begin() + static_cast<long>(i));
        ++frees;
        std::free(p);
        return;
      }
    }
    TEST_FAIL_MESSAGE("freed a block the index didn't allocate");
  }
};
size_t Heap::live = 0;
size_t Heap::peak = 0;
size_t Heap::allocs = 0;
size_t Heap::frees = 0;
size_t Heap::limit = SIZE_MAX;

// The SD card's layout (a slice of it) plus the odd cases.
const char* const kFiles[] = {
    "/music/Daft Punk/Discovery/01 - One More Time.mp3",
    "/music/Daft Punk/Discovery/03 - Digital Love.mp3",
    "/music/Daft Punk/Discovery/02 - Aerodynamic.mp3",
    "/music/Daft Punk/Discovery/cover.jpg",  // not audio: counted, and the album's cover
    "/music/Daft Punk/Homework/07 - Around the World.flac",
    "/music/Kanye West/Graduation/06 - Can’T Tell Me Nothing.mp3",
    "/music/Kanye West/Graduation/05 - Good Life Feat. T‐Pain.MP3",
    "/music/air/Moon Safari/01 - La femme d'argent.flac",
    "/music/Aphex Twin/Selected Ambient Works 85-92/01 - Xtal.mp3",
    "/music/Émilie Simon/Végétal/10 - Le voyage de Pénélope.mp3",
    "/music/Kavinsky/OutRun/08 - Nightcall.mp3",
    "/music/Kavinsky/OutRun/CD2/01 - Bonus.mp3",  // deeper: still OutRun
    "/music/Kavinsky/Loose Track.mp3",             // artist level: album ""
    "/music/Root Track.mp3",                       // root: artist "", album ""
    "/music/808 State/Ninety/2001 A Space.mp3",    // 4 digits: no number
    "/elsewhere/x.mp3",                            // outside the root
    "/music/Daft Punk/Discovery/notes.txt",
};
constexpr uint32_t kAudio = 14;

void buildSample(LibraryIndex& idx) {
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const char* f : kFiles) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());
}

std::string title(const LibraryIndex& idx, uint32_t t) {
  uint8_t len;
  const char* s = idx.trackTitle(t, &len);
  return std::string(s, len);
}

uint32_t findArtist(const LibraryIndex& idx, const char* name) {
  for (uint32_t i = 0; i < idx.artistCount(); ++i) {
    if (std::strcmp(idx.artistName(i), name) == 0) return i;
  }
  return LibraryIndex::kNone;
}

uint32_t findAlbum(const LibraryIndex& idx, const char* name) {
  for (uint32_t i = 0; i < idx.albumCount(); ++i) {
    if (std::strcmp(idx.albumName(i), name) == 0) return i;
  }
  return LibraryIndex::kNone;
}

}  // namespace

void setUp() { Heap::reset(); }
void tearDown() {}

void test_counts_and_skips() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music/"));  // a trailing slash is fine
  uint32_t added = 0, skipped = 0, other = 0;
  for (const char* f : kFiles) {
    const auto r = idx.addFile(f);
    if (r == LibraryIndex::Add::Added) ++added;
    if (r == LibraryIndex::Add::Skipped) ++skipped;
    if (r == LibraryIndex::Add::Other) ++other;
  }
  TEST_ASSERT_EQUAL_UINT32(kAudio, added);
  TEST_ASSERT_EQUAL_UINT32(1, skipped);  // outside the root
  TEST_ASSERT_EQUAL_UINT32(2, other);    // cover.jpg, notes.txt
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_TRUE(idx.ready());
  TEST_ASSERT_EQUAL_UINT32(kAudio, idx.trackCount());
  // Daft Punk, Kanye West, air, Aphex Twin, Émilie Simon, Kavinsky, "", 808 State.
  TEST_ASSERT_EQUAL_UINT32(8, idx.artistCount());
  // Discovery, Homework, Graduation, Moon Safari, SAW 85-92, Végétal, OutRun,
  // Kavinsky's "", the root's "", Ninety.
  TEST_ASSERT_EQUAL_UINT32(10, idx.albumCount());
}

void test_titles_numbers_formats() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  bool sawCant = false, sawSpace = false, sawRoot = false;
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    const std::string name = idx.trackFileName(t);
    if (name == "06 - Can’T Tell Me Nothing.mp3") {
      TEST_ASSERT_EQUAL_STRING("Can’T Tell Me Nothing", title(idx, t).c_str());
      TEST_ASSERT_EQUAL_UINT8(6, idx.track(t).number);
      TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Mp3), static_cast<int>(idx.track(t).format));
      sawCant = true;
    }
    if (name == "05 - Good Life Feat. T‐Pain.MP3") {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Mp3), static_cast<int>(idx.track(t).format));
    }
    if (name == "07 - Around the World.flac") {
      TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Flac), static_cast<int>(idx.track(t).format));
    }
    if (name == "2001 A Space.mp3") {
      TEST_ASSERT_EQUAL_UINT8(0, idx.track(t).number);
      TEST_ASSERT_EQUAL_STRING("2001 A Space", title(idx, t).c_str());
      sawSpace = true;
    }
    if (name == "Root Track.mp3") {
      TEST_ASSERT_EQUAL_STRING("", idx.artistName(idx.track(t).artist));
      TEST_ASSERT_EQUAL_STRING("", idx.albumName(idx.track(t).album));
      TEST_ASSERT_EQUAL_UINT32(LibraryIndex::rootFolder(), idx.track(t).folder);
      sawRoot = true;
    }
  }
  TEST_ASSERT_TRUE(sawCant && sawSpace && sawRoot);
}

void test_paths_round_trip() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  std::set<std::string> want;
  for (const char* f : kFiles) {
    const std::string s = f;
    if (s.rfind("/music/", 0) == 0 && (s.find(".mp3") != std::string::npos || s.find(".MP3") != std::string::npos ||
                                       s.find(".flac") != std::string::npos)) {
      want.insert(s);
    }
  }
  std::set<std::string> got;
  char buf[256];
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    TEST_ASSERT_TRUE(idx.trackPath(t, buf, sizeof(buf)) > 0);
    got.insert(buf);
  }
  TEST_ASSERT_TRUE(want == got);
  // Too small a buffer: 0 and "".
  char small[10];
  TEST_ASSERT_EQUAL_UINT32(0, idx.trackPath(0, small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

void test_artist_and_album_views() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  // Artists A-Z: "", 808 State, air, Aphex Twin, Daft Punk, Émilie Simon, Kanye West, Kavinsky.
  const char* order[] = {"", "808 State", "air", "Aphex Twin", "Daft Punk", "Émilie Simon", "Kanye West", "Kavinsky"};
  const LibraryIndex::Span a = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(8, a.count);
  for (uint32_t i = 0; i < a.count; ++i) TEST_ASSERT_EQUAL_STRING(order[i], idx.artistName(a[i]));

  // Daft Punk: Discovery then Homework; Discovery's tracks by number.
  const uint32_t dp = findArtist(idx, "Daft Punk");
  const LibraryIndex::Span albums = idx.albumsOf(dp);
  TEST_ASSERT_EQUAL_UINT32(2, albums.count);
  TEST_ASSERT_EQUAL_STRING("Discovery", idx.albumName(albums[0]));
  TEST_ASSERT_EQUAL_STRING("Homework", idx.albumName(albums[1]));
  const LibraryIndex::Span disc = idx.tracksOfAlbum(albums[0]);
  TEST_ASSERT_EQUAL_UINT32(3, disc.count);
  TEST_ASSERT_EQUAL_STRING("One More Time", title(idx, disc[0]).c_str());
  TEST_ASSERT_EQUAL_STRING("Aerodynamic", title(idx, disc[1]).c_str());
  TEST_ASSERT_EQUAL_STRING("Digital Love", title(idx, disc[2]).c_str());
  // All of Daft Punk: Discovery's 3, then Homework's 1.
  const LibraryIndex::Span all = idx.tracksOfArtist(dp);
  TEST_ASSERT_EQUAL_UINT32(4, all.count);
  TEST_ASSERT_EQUAL_STRING("Around the World", title(idx, all[3]).c_str());

  // Kavinsky: album "" (the loose track) sorts before OutRun, which has the CD2 bonus.
  const uint32_t kav = findArtist(idx, "Kavinsky");
  const LibraryIndex::Span ka = idx.albumsOf(kav);
  TEST_ASSERT_EQUAL_UINT32(2, ka.count);
  TEST_ASSERT_EQUAL_STRING("", idx.albumName(ka[0]));
  TEST_ASSERT_EQUAL_STRING("OutRun", idx.albumName(ka[1]));
  TEST_ASSERT_EQUAL_UINT32(2, idx.tracksOfAlbum(ka[1]).count);
  // The album's folder is OutRun's, not CD2.
  TEST_ASSERT_EQUAL_STRING("OutRun", idx.folderName(idx.album(ka[1]).folder));

  // Albums A-Z.
  const LibraryIndex::Span z = idx.albumsAZ();
  TEST_ASSERT_EQUAL_UINT32(10, z.count);
  for (uint32_t i = 1; i < z.count; ++i) {
    TEST_ASSERT_TRUE(textfold::compareSorted(idx.albumName(z[i - 1]), idx.albumName(z[i])) <= 0);
  }
  TEST_ASSERT_EQUAL_STRING("Végétal", idx.albumName(z[z.count - 1]));
}

void test_folder_tree() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  TEST_ASSERT_EQUAL_STRING("/music", idx.folderName(LibraryIndex::rootFolder()));
  const LibraryIndex::Span top = idx.subfolders(LibraryIndex::rootFolder());
  const char* order[] = {"808 State", "air", "Aphex Twin", "Daft Punk", "Émilie Simon", "Kanye West", "Kavinsky"};
  TEST_ASSERT_EQUAL_UINT32(7, top.count);
  for (uint32_t i = 0; i < top.count; ++i) TEST_ASSERT_EQUAL_STRING(order[i], idx.folderName(top[i]));
  TEST_ASSERT_EQUAL_UINT32(1, idx.filesIn(LibraryIndex::rootFolder()).count);

  // Kavinsky: OutRun as a folder, the loose track as a file.
  const uint32_t kav = top[6];
  TEST_ASSERT_EQUAL_UINT32(1, idx.subfolders(kav).count);
  TEST_ASSERT_EQUAL_UINT32(1, idx.filesIn(kav).count);
  const uint32_t outrun = idx.subfolders(kav)[0];
  TEST_ASSERT_EQUAL_STRING("OutRun", idx.folderName(outrun));
  TEST_ASSERT_EQUAL_UINT32(1, idx.subfolders(outrun).count);  // CD2
  TEST_ASSERT_EQUAL_UINT32(1, idx.filesIn(outrun).count);
  char buf[128];
  idx.folderPath(idx.subfolders(outrun)[0], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky/OutRun/CD2", buf);

  // Discovery's files A-Z by file name (01, 02, 03).
  const uint32_t dp = top[3];
  const LibraryIndex::Span disc = idx.filesIn(idx.subfolders(dp)[0]);
  TEST_ASSERT_EQUAL_UINT32(3, disc.count);
  TEST_ASSERT_EQUAL_STRING("01 - One More Time.mp3", idx.trackFileName(disc[0]));
  TEST_ASSERT_EQUAL_STRING("03 - Digital Love.mp3", idx.trackFileName(disc[2]));
}

// Every file that isn't audio is counted in its folder; the best-named
// image is the folder's cover; folders with no audio under them are left out.
void test_other_files_and_covers() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* const files[] = {
      "/music/Daft Punk/Discovery/back.jpg",
      "/music/Daft Punk/Discovery/01 - One More Time.mp3",
      "/music/Daft Punk/Discovery/cover.jpg",
      "/music/Daft Punk/Discovery/notes.txt",
      "/music/Air/Moon Safari/AlbumArtSmall.jpg",
      "/music/Air/Moon Safari/Folder.JPG",
      "/music/Air/Moon Safari/01 - La femme d'argent.mp3",
      "/music/Air/Moon Safari/front.jpeg",
      "/music/Air/Moon Safari/scan.png",
      "/music/Kavinsky/OutRun/CD1/01 - Prelude.mp3",
      "/music/Kavinsky/OutRun/CD1/Cover.jpg",
      "/music/Kavinsky/OutRun/CD2/01 - Bonus.mp3",
      "/music/Artwork/poster.jpg",  // no audio under it: not a folder in the views
      "/music/Artwork/Sub/x.png",
      "/music/Empty/readme",        // no extension: another "other"
  };
  for (const char* f : files) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());

  const LibraryIndex::Span top = idx.subfolders(LibraryIndex::rootFolder());
  TEST_ASSERT_EQUAL_UINT32(3, top.count);
  TEST_ASSERT_EQUAL_STRING("Air", idx.folderName(top[0]));
  TEST_ASSERT_EQUAL_STRING("Daft Punk", idx.folderName(top[1]));
  TEST_ASSERT_EQUAL_STRING("Kavinsky", idx.folderName(top[2]));

  char buf[128];
  const uint32_t discovery = findAlbum(idx, "Discovery");
  const uint32_t df = idx.album(discovery).folder;
  TEST_ASSERT_EQUAL_UINT16(3, idx.folder(df).otherCount);
  TEST_ASSERT_EQUAL_UINT8(2, idx.folder(df).imageCount);
  TEST_ASSERT_EQUAL_UINT8(0, idx.folder(df).imageRank);
  TEST_ASSERT_EQUAL_UINT32(df, idx.albumCover(discovery));
  TEST_ASSERT_TRUE(idx.imagePath(df, buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_STRING("/music/Daft Punk/Discovery/cover.jpg", buf);
  TEST_ASSERT_EQUAL_UINT32(1, idx.filesIn(df).count);

  // folder.jpg beats front.jpeg and any other .jpg, whatever the order and case.
  const uint32_t moon = findAlbum(idx, "Moon Safari");
  const uint32_t mf = idx.album(moon).folder;
  TEST_ASSERT_EQUAL_UINT8(3, idx.folder(mf).imageCount);
  TEST_ASSERT_EQUAL_UINT16(4, idx.folder(mf).otherCount);
  idx.imagePath(idx.albumCover(moon), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/Air/Moon Safari/Folder.JPG", buf);

  // OutRun's own folder has no image: its first disc's cover.
  const uint32_t outrun = findAlbum(idx, "OutRun");
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, idx.folder(idx.album(outrun).folder).image);
  idx.imagePath(idx.albumCover(outrun), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/Kavinsky/OutRun/CD1/Cover.jpg", buf);
  // No image at all: no path.
  TEST_ASSERT_EQUAL_size_t(0, idx.imagePath(LibraryIndex::rootFolder(), buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_image_rank() {
  auto rank = [](const char* n) { return LibraryIndex::imageRank(n, std::strlen(n)); };
  TEST_ASSERT_EQUAL_UINT8(0, rank("cover.jpg"));
  TEST_ASSERT_EQUAL_UINT8(0, rank("COVER.JPEG"));
  TEST_ASSERT_EQUAL_UINT8(1, rank("Folder.jpg"));
  TEST_ASSERT_EQUAL_UINT8(2, rank("front.jpg"));
  TEST_ASSERT_EQUAL_UINT8(3, rank("AlbumArt_{X}_Large.jpg"));
  TEST_ASSERT_EQUAL_UINT8(3, rank("cover2.jpg"));
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kNoImage, rank("cover.png"));
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kNoImage, rank(".jpg"));
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kNoImage, rank("cover"));
  TEST_ASSERT_EQUAL_UINT8(LibraryIndex::kNoImage, rank("01 - Track.mp3"));
}

// A folder's whole tree is one run: its own files A-Z, then each
// subfolder's tree, A-Z; filesIn() is the start of it.
void test_tree_tracks() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  const uint32_t root = LibraryIndex::rootFolder();
  TEST_ASSERT_EQUAL_UINT32(kAudio, idx.treeTracks(root).count);
  const LibraryIndex::Span top = idx.subfolders(root);
  const uint32_t kav = top[6];
  const LibraryIndex::Span tree = idx.treeTracks(kav);
  TEST_ASSERT_EQUAL_UINT32(3, tree.count);
  TEST_ASSERT_EQUAL_STRING("Loose Track.mp3", idx.trackFileName(tree[0]));
  TEST_ASSERT_EQUAL_STRING("08 - Nightcall.mp3", idx.trackFileName(tree[1]));
  TEST_ASSERT_EQUAL_STRING("01 - Bonus.mp3", idx.trackFileName(tree[2]));
  TEST_ASSERT_EQUAL_PTR(tree.ids, idx.filesIn(kav).ids);
  // No files of its own: an empty filesIn() where its tree starts.
  const uint32_t dp = top[3];
  TEST_ASSERT_EQUAL_STRING("Daft Punk", idx.folderName(dp));
  TEST_ASSERT_EQUAL_UINT32(0, idx.filesIn(dp).count);
  const LibraryIndex::Span dpTree = idx.treeTracks(dp);
  TEST_ASSERT_EQUAL_UINT32(4, dpTree.count);
  TEST_ASSERT_EQUAL_PTR(dpTree.ids, idx.filesIn(idx.subfolders(dp)[0]).ids);
  TEST_ASSERT_EQUAL_STRING("01 - One More Time.mp3", idx.trackFileName(dpTree[0]));
  TEST_ASSERT_EQUAL_STRING("07 - Around the World.flac", idx.trackFileName(dpTree[3]));
  // Every track once, in the root's tree.
  std::set<uint32_t> seen;
  const LibraryIndex::Span all = idx.treeTracks(root);
  for (uint32_t i = 0; i < all.count; ++i) seen.insert(all[i]);
  TEST_ASSERT_EQUAL_UINT32(kAudio, seen.size());
}

void test_buckets() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  using V = LibraryIndex::View;
  // '#': "" and 808 State; A: air, Aphex Twin; D: Daft Punk; E: Émilie; K: Kanye, Kavinsky.
  TEST_ASSERT_EQUAL_UINT32(0, idx.bucketStart(V::Artists, 0));
  TEST_ASSERT_EQUAL_UINT32(2, idx.bucketStart(V::Artists, 1));   // A
  TEST_ASSERT_EQUAL_UINT32(4, idx.bucketStart(V::Artists, 2));   // B: empty, starts at D's
  TEST_ASSERT_EQUAL_UINT32(4, idx.bucketStart(V::Artists, 4));   // D
  TEST_ASSERT_EQUAL_UINT32(5, idx.bucketStart(V::Artists, 5));   // E
  TEST_ASSERT_EQUAL_UINT32(6, idx.bucketStart(V::Artists, 11));  // K
  TEST_ASSERT_EQUAL_UINT32(8, idx.bucketStart(V::Artists, 12));  // L..Z empty
  TEST_ASSERT_EQUAL_UINT32(8, idx.bucketStart(V::Artists, LibraryIndex::kBuckets));
  TEST_ASSERT_EQUAL_INT(5, idx.bucketAt(V::Artists, 5));
  TEST_ASSERT_EQUAL_INT(0, idx.bucketAt(V::Artists, 1));
  // Monotonic for the albums too.
  for (int b = 0; b < LibraryIndex::kBuckets; ++b) {
    TEST_ASSERT_TRUE(idx.bucketStart(V::Albums, b) <= idx.bucketStart(V::Albums, b + 1));
  }
}

void test_memory_comes_from_the_hooks_and_goes_back() {
  {
    LibraryIndex idx(Heap::alloc, Heap::release);
    buildSample(idx);
    const size_t allocsAfterBuild = Heap::allocs;
    // Blocks, not strings: a handful of allocations whatever the size.
    TEST_ASSERT_TRUE(allocsAfterBuild < 40);
    // Queries don't allocate.
    char buf[256];
    for (uint32_t t = 0; t < idx.trackCount(); ++t) idx.trackPath(t, buf, sizeof(buf));
    (void)idx.artistsAZ();
    TEST_ASSERT_EQUAL_size_t(allocsAfterBuild, Heap::allocs);
    // What the index reports is what it holds.
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    idx.clear();
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
    buildSample(idx);  // reusable after clear()
  }
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);  // the destructor frees it all
  TEST_ASSERT_EQUAL_size_t(Heap::allocs, Heap::frees);
}

void test_out_of_memory_is_clean() {
  for (size_t limit : {size_t(0), size_t(20000), size_t(28000), size_t(32000), size_t(40000), size_t(60000)}) {
    Heap::reset();
    Heap::limit = limit;
    {
      LibraryIndex idx(Heap::alloc, Heap::release);
      if (idx.begin("/music", 300)) {
        synth::Spec spec = synth::specFor(300);
        synth::addTracks(idx, spec);
      }
      const bool ok = idx.finish();
      if (!ok) {
        TEST_ASSERT_TRUE(idx.failed());
        TEST_ASSERT_EQUAL_UINT32(0, idx.trackCount());
      }
    }
    TEST_ASSERT_EQUAL_size_t(0, Heap::live);
  }
}

void test_synthetic_10k() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_EQUAL_UINT32(600, spec.artists);
  TEST_ASSERT_EQUAL_UINT32(1500, spec.albums);
  const auto t0 = std::chrono::steady_clock::now();
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  TEST_ASSERT_EQUAL_UINT32(10000, synth::addTracks(idx, spec));
  const auto t1 = std::chrono::steady_clock::now();
  TEST_ASSERT_TRUE(idx.finish());
  const auto t2 = std::chrono::steady_clock::now();
  TEST_ASSERT_EQUAL_UINT32(10000, idx.trackCount());
  TEST_ASSERT_EQUAL_UINT32(600, idx.artistCount());
  TEST_ASSERT_EQUAL_UINT32(1500, idx.albumCount());

  // Every track exactly once through artists -> albums -> tracks, in order.
  std::vector<uint8_t> seen(idx.trackCount(), 0);
  const LibraryIndex::Span artists = idx.artistsAZ();
  for (uint32_t i = 0; i < artists.count; ++i) {
    if (i) TEST_ASSERT_TRUE(textfold::compareSorted(idx.artistName(artists[i - 1]), idx.artistName(artists[i])) < 0);
    const LibraryIndex::Span albums = idx.albumsOf(artists[i]);
    uint32_t artistTracks = 0;
    for (uint32_t j = 0; j < albums.count; ++j) {
      TEST_ASSERT_EQUAL_UINT32(artists[i], idx.album(albums[j]).artist);
      const LibraryIndex::Span tracks = idx.tracksOfAlbum(albums[j]);
      for (uint32_t k = 0; k < tracks.count; ++k) {
        TEST_ASSERT_EQUAL_UINT32(albums[j], idx.track(tracks[k]).album);
        if (k) TEST_ASSERT_TRUE(idx.track(tracks[k - 1]).number < idx.track(tracks[k]).number);
        seen[tracks[k]]++;
      }
      artistTracks += tracks.count;
    }
    TEST_ASSERT_EQUAL_UINT32(artistTracks, idx.tracksOfArtist(artists[i]).count);
  }
  for (uint8_t s : seen) TEST_ASSERT_EQUAL_UINT8(1, s);

  // And once through the folder tree.
  std::fill(seen.begin(), seen.end(), 0);
  std::vector<uint32_t> stack{LibraryIndex::rootFolder()};
  while (!stack.empty()) {
    const uint32_t f = stack.back();
    stack.pop_back();
    const LibraryIndex::Span files = idx.filesIn(f);
    for (uint32_t k = 0; k < files.count; ++k) seen[files[k]]++;
    const LibraryIndex::Span sub = idx.subfolders(f);
    for (uint32_t k = 0; k < sub.count; ++k) stack.push_back(sub[k]);
  }
  for (uint8_t s : seen) TEST_ASSERT_EQUAL_UINT8(1, s);

  // Synthetic paths round-trip.
  char want[320], got[320];
  for (uint32_t i : {0u, 1u, 4u, 4999u, 9999u}) {
    TEST_ASSERT_TRUE(synth::trackPath(spec, i, want, sizeof(want)));
    bool found = false;
    for (uint32_t t = 0; t < idx.trackCount() && !found; ++t) {
      idx.trackPath(t, got, sizeof(got));
      found = std::strcmp(want, got) == 0;
    }
    TEST_ASSERT_TRUE_MESSAGE(found, want);
  }

  // Size: the whole index in well under 100 bytes a track, all of it from the hooks.
  const LibraryIndex::Memory m = idx.memory();
  TEST_ASSERT_EQUAL_size_t(Heap::live, m.total);
  TEST_ASSERT_TRUE(m.total < 100u * 10000u);
  TEST_ASSERT_TRUE(m.buildPeak >= m.total);
  // The reported peak is the allocator's own high-water mark (a block and its
  // doubled replacement, the trim's copies, the sort's temporaries).
  TEST_ASSERT_EQUAL_size_t(Heap::peak, m.buildPeak);
  // Buckets cover the list.
  TEST_ASSERT_EQUAL_UINT32(600, idx.bucketStart(LibraryIndex::View::Artists, LibraryIndex::kBuckets));
  const auto ms = [](auto a, auto b) {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count());
  };
  printf("[index] synthetic 10000: add %ld ms, finish %ld ms, %u bytes (%.1f B/track): strings %u, tracks %u, "
         "artists %u, albums %u, folders %u, views %u; build peak %u\n",
         ms(t0, t1), ms(t1, t2), static_cast<unsigned>(m.total), m.total / 10000.0, static_cast<unsigned>(m.strings),
         static_cast<unsigned>(m.tracks), static_cast<unsigned>(m.artists), static_cast<unsigned>(m.albums),
         static_cast<unsigned>(m.folders), static_cast<unsigned>(m.views), static_cast<unsigned>(m.buildPeak));
}

// Discs in subfolders of the album folder: one album, disc after disc.
void test_multi_disc_album() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      "/music/Pink Floyd/The Wall/CD2/02 - Hey You.mp3",
      "/music/Pink Floyd/The Wall/CD1/01 - In the Flesh.mp3",
      "/music/Pink Floyd/The Wall/CD2/01 - Is There Anybody Out There.mp3",
      "/music/Pink Floyd/The Wall/CD1/02 - The Thin Ice.mp3",
      "/music/Pink Floyd/The Wall/00 - Booklet Intro.mp3",  // in the album folder itself: first
      "/music/Pink Floyd/The Wall/CD1/03 - Another Brick.mp3",
  };
  for (const char* f : files) TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile(f)));
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(1, idx.albumCount());
  const LibraryIndex::Span t = idx.tracksOfAlbum(0);
  const char* want[] = {"Booklet Intro", "In the Flesh", "The Thin Ice", "Another Brick", "Is There Anybody Out There",
                        "Hey You"};
  TEST_ASSERT_EQUAL_UINT32(6, t.count);
  for (uint32_t i = 0; i < t.count; ++i) TEST_ASSERT_EQUAL_STRING(want[i], title(idx, t[i]).c_str());
  // The whole-library view agrees.
  for (uint32_t i = 0; i < t.count; ++i) TEST_ASSERT_EQUAL_UINT32(t[i], idx.allTracks()[i]);
  TEST_ASSERT_EQUAL_size_t(Heap::peak, idx.memory().buildPeak);
}

void test_rebuild_and_expect_hint() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  for (uint32_t n : {77u, 2000u}) {
    const synth::Spec spec = synth::specFor(n);
    TEST_ASSERT_TRUE(idx.begin(spec.root, n));
    TEST_ASSERT_EQUAL_UINT32(n, synth::addTracks(idx, spec));
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_EQUAL_UINT32(n, idx.trackCount());
    TEST_ASSERT_EQUAL_UINT32(spec.artists, idx.artistCount());
    TEST_ASSERT_EQUAL_UINT32(spec.albums, idx.albumCount());
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
  }
}

// ---- finding a track by its path ----

void test_find_track_by_path() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  char buf[256];
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    TEST_ASSERT_TRUE(idx.trackPath(t, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_UINT32(t, idx.findTrack(buf));
  }
  const char* none[] = {
      "/music/Daft Punk/Discovery/cover.jpg",               // not indexed
      "/music/daft punk/Discovery/01 - One More Time.mp3",  // case matters (it's a path)
      "/music/Daft Punk/Discovery",                         // a folder
      "/music/Daft Punk/Discovery/",
      "/music",
      "/musicx/Root Track.mp3",
      "/elsewhere/x.mp3",
      "relative.mp3",
      "",
  };
  for (const char* p : none) TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, idx.findTrack(p));
  // Doubled slashes are skipped, as addFile() skips them.
  TEST_ASSERT_TRUE(idx.findTrack("/music/Kavinsky//OutRun/08 - Nightcall.mp3") != LibraryIndex::kNone);
  LibraryIndex empty(Heap::alloc, Heap::release);
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, empty.findTrack("/music/Root Track.mp3"));
}

void test_find_track_in_a_big_library() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  const synth::Spec spec = synth::specFor(2000);
  TEST_ASSERT_TRUE(idx.begin(spec.root, 2000));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  char buf[256];
  for (uint32_t t = 0; t < idx.trackCount(); t += 7) {
    idx.trackPath(t, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(t, idx.findTrack(buf));
  }
}

// ---- the cache ----

namespace {
// Everything a reader can see: the counts, every view, every path.
void expectSameIndex(const LibraryIndex& a, const LibraryIndex& b) {
  TEST_ASSERT_TRUE(b.ready());
  TEST_ASSERT_EQUAL_UINT32(a.trackCount(), b.trackCount());
  TEST_ASSERT_EQUAL_UINT32(a.artistCount(), b.artistCount());
  TEST_ASSERT_EQUAL_UINT32(a.albumCount(), b.albumCount());
  TEST_ASSERT_EQUAL_UINT32(a.folderCount(), b.folderCount());
  char pa[256], pb[256];
  for (uint32_t t = 0; t < a.trackCount(); ++t) {
    a.trackPath(t, pa, sizeof(pa));
    b.trackPath(t, pb, sizeof(pb));
    TEST_ASSERT_EQUAL_STRING(pa, pb);
    TEST_ASSERT_EQUAL_STRING(title(a, t).c_str(), title(b, t).c_str());
    TEST_ASSERT_EQUAL_UINT8(a.track(t).number, b.track(t).number);
    TEST_ASSERT_EQUAL_UINT8(a.track(t).disc, b.track(t).disc);
    TEST_ASSERT_EQUAL_UINT32(a.allTracks()[t], b.allTracks()[t]);
    TEST_ASSERT_EQUAL_UINT32(t, b.findTrack(pa));
  }
  for (uint32_t i = 0; i < a.artistCount(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(a.artistsAZ()[i], b.artistsAZ()[i]);
    TEST_ASSERT_EQUAL_STRING(a.artistName(i), b.artistName(i));
    TEST_ASSERT_EQUAL_STRING(a.artistSortKey(i), b.artistSortKey(i));
    TEST_ASSERT_EQUAL_UINT32(a.albumsOf(i).count, b.albumsOf(i).count);
    TEST_ASSERT_EQUAL_UINT32(a.tracksOfArtist(i).count, b.tracksOfArtist(i).count);
  }
  for (uint32_t i = 0; i < a.albumCount(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(a.albumsAZ()[i], b.albumsAZ()[i]);
    TEST_ASSERT_EQUAL_STRING(a.albumSortKey(i), b.albumSortKey(i));
    TEST_ASSERT_EQUAL_UINT32(a.tracksOfAlbum(i).count, b.tracksOfAlbum(i).count);
  }
  for (uint32_t f = 0; f < a.folderCount(); ++f) {
    a.folderPath(f, pa, sizeof(pa));
    b.folderPath(f, pb, sizeof(pb));
    TEST_ASSERT_EQUAL_STRING(pa, pb);
    TEST_ASSERT_EQUAL_UINT32(a.subfolders(f).count, b.subfolders(f).count);
    TEST_ASSERT_EQUAL_UINT32(a.filesIn(f).count, b.filesIn(f).count);
    TEST_ASSERT_EQUAL_UINT32(a.treeTracks(f).count, b.treeTracks(f).count);
    TEST_ASSERT_EQUAL_UINT32(a.folder(f).otherCount, b.folder(f).otherCount);
    a.imagePath(f, pa, sizeof(pa));
    b.imagePath(f, pb, sizeof(pb));
    TEST_ASSERT_EQUAL_STRING(pa, pb);
  }
  for (int k = 0; k <= LibraryIndex::kBuckets; ++k) {
    TEST_ASSERT_EQUAL_UINT32(a.bucketStart(LibraryIndex::View::Artists, k),
                             b.bucketStart(LibraryIndex::View::Artists, k));
    TEST_ASSERT_EQUAL_UINT32(a.bucketStart(LibraryIndex::View::Albums, k), b.bucketStart(LibraryIndex::View::Albums, k));
  }
  TEST_ASSERT_EQUAL_size_t(a.memory().total, b.memory().total);
}
}  // namespace

void test_save_and_load_round_trip() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 0x1234567890ABCDEFull));
  const size_t before = Heap::live;
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size(), 100);  // reads split into pieces
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded),
                        static_cast<int>(back.load(in, 0x1234567890ABCDEFull)));
  expectSameIndex(idx, back);
  // Blocks of exactly the saved size, and no build peak beyond them.
  TEST_ASSERT_EQUAL_size_t(Heap::live - before, back.memory().total);
  TEST_ASSERT_EQUAL_size_t(back.memory().total, back.memory().buildPeak);
  // It saves again byte for byte.
  MemorySink again;
  TEST_ASSERT_TRUE(back.save(again, 0x1234567890ABCDEFull));
  TEST_ASSERT_EQUAL_size_t(file.size(), again.size());
  TEST_ASSERT_EQUAL_MEMORY(file.data(), again.data(), file.size());
}

void test_save_and_load_a_big_library() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  const synth::Spec spec = synth::specFor(10000);
  TEST_ASSERT_TRUE(idx.begin(spec.root));
  synth::addTracks(idx, spec);
  TEST_ASSERT_TRUE(idx.finish());
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 7));
  // The file is the index's blocks plus a small header.
  TEST_ASSERT_TRUE(file.size() >= idx.memory().total && file.size() < idx.memory().total + 512);
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size(), 4096);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 7)));
  expectSameIndex(idx, back);
  printf("[index] cache of 10000 tracks: %u bytes (the index holds %u)\n", static_cast<unsigned>(file.size()),
         static_cast<unsigned>(idx.memory().total));
}

void test_load_rejects_stale_and_damaged_files() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 42));
  LibraryIndex back(Heap::alloc, Heap::release);
  auto load = [&](const std::vector<uint8_t>& bytes, uint64_t sig) {
    MemorySource in(bytes.data(), bytes.size());
    return static_cast<int>(back.load(in, sig));
  };
  const std::vector<uint8_t> good(file.data(), file.data() + file.size());
  const size_t emptyHeap = Heap::live;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Stale), load(good, 43));
  TEST_ASSERT_FALSE(back.ready());
  // Cut short anywhere.
  for (size_t cut : {size_t(0), size_t(3), size_t(100), good.size() / 2, good.size() - 1}) {
    const std::vector<uint8_t> shortFile(good.begin(), good.begin() + static_cast<long>(cut));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), load(shortFile, 42));
    TEST_ASSERT_FALSE(back.ready());
    TEST_ASSERT_EQUAL_size_t(emptyHeap, Heap::live);  // whatever it took went back
  }
  // A flipped byte in the payload: the checksum catches it.
  std::vector<uint8_t> flipped = good;
  flipped[good.size() / 2] ^= 0x20;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), load(flipped, 42));
  // Not an index at all.
  std::vector<uint8_t> junk(good.size(), 0x5A);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), load(junk, 42));
  // Out of memory while loading: clean, and says so.
  Heap::limit = Heap::live + 64;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::NoMemory), load(good, 42));
  TEST_ASSERT_EQUAL_size_t(emptyHeap, Heap::live);
  Heap::limit = SIZE_MAX;
  // And the good file still loads after all that.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), load(good, 42));
  expectSameIndex(idx, back);
  // An index that isn't ready has nothing to save.
  LibraryIndex none(Heap::alloc, Heap::release);
  MemorySink nothing;
  TEST_ASSERT_FALSE(none.save(nothing, 1));
}

// .opus files are tracks (docs/OPUS.md), whatever the case of the
// extension; .ogg and .oga stay other files (counted in their folder, as
// a text file is), as does an .opus with no name.
void test_opus_files_are_tracks() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Added), static_cast<int>(idx.addFile("/music/Band/Record/01 - Opener.opus")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Added), static_cast<int>(idx.addFile("/music/Band/Record/02 - Second Song.OPUS")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Added), static_cast<int>(idx.addFile("/music/Band/Record/03 - Closer.mp3")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Other), static_cast<int>(idx.addFile("/music/Band/Record/04 - Vorbis.ogg")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Other), static_cast<int>(idx.addFile("/music/Band/Record/05 - Vorbis.oga")));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Add::Other), static_cast<int>(idx.addFile("/music/Band/Record/.opus")));
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(3, idx.trackCount());
  const uint32_t album = findAlbum(idx, "Record");
  TEST_ASSERT_NOT_EQUAL(LibraryIndex::kNone, album);
  const LibraryIndex::Span t = idx.tracksOfAlbum(album);
  TEST_ASSERT_EQUAL_UINT32(3, t.count);
  TEST_ASSERT_EQUAL_STRING("01 - Opener.opus", idx.trackFileName(t[0]));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Opus), static_cast<int>(idx.track(t[0]).format));
  TEST_ASSERT_EQUAL_STRING("Opener", title(idx, t[0]).c_str());
  TEST_ASSERT_EQUAL_UINT8(1, idx.track(t[0]).number);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Opus), static_cast<int>(idx.track(t[1]).format));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Mp3), static_cast<int>(idx.track(t[2]).format));
  TEST_ASSERT_EQUAL_UINT16(3, idx.folder(idx.album(album).folder).otherCount);
  // The cache: a version-2 file (the one 0.6.0 wrote, in which an .opus
  // was an other file) is Outdated, not Corrupt, and loads nothing; the
  // version written now loads, formats and all.
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 7));
  std::vector<uint8_t> bytes(file.data(), file.data() + file.size());
  LibraryIndex back(Heap::alloc, Heap::release);
  {
    MemorySource in(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 7)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Format::Opus), static_cast<int>(back.track(t[0]).format));
  }
  std::vector<uint8_t> v2 = bytes;
  v2[4] = 2;  // the version word (little-endian, after the magic)
  {
    MemorySource in(v2.data(), v2.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), static_cast<int>(back.load(in, 7)));
    TEST_ASSERT_FALSE(back.ready());
  }
  std::vector<uint8_t> v9 = bytes;
  v9[4] = 9;  // a version from the future: foreign
  {
    MemorySource in(v9.data(), v9.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), static_cast<int>(back.load(in, 7)));
  }
}

void test_an_empty_library_round_trips() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  TEST_ASSERT_TRUE(idx.finish());
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 5));
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 5)));
  TEST_ASSERT_EQUAL_UINT32(0, back.trackCount());
  TEST_ASSERT_EQUAL_UINT32(1, back.folderCount());
  TEST_ASSERT_EQUAL_STRING("/music", back.folderName(0));
}

// ---- names (made-up artists and titles, in the shapes measured on a real
// library; test_track_name has the rules one by one) ----

namespace {
std::vector<std::string> albumTitles(const LibraryIndex& idx, const char* album) {
  std::vector<std::string> out;
  const LibraryIndex::Span t = idx.tracksOfAlbum(findAlbum(idx, album));
  for (uint32_t i = 0; i < t.count; ++i) {
    const LibraryIndex::Track& r = idx.track(t[i]);
    out.push_back(std::to_string(r.disc) + "/" + std::to_string(r.number) + " " + title(idx, t[i]));
  }
  return out;
}

void expectTitles(const LibraryIndex& idx, const char* album, const std::vector<std::string>& want) {
  const std::vector<std::string> got = albumTitles(idx, album);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(want.size(), got.size(), album);
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}
}  // namespace

// "The Lantern Choir" sorts under L, its name shown as it is; the Folders
// view keeps the folders' own order.
void test_articles_sort_past_the() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      "/music/Theory of Rain/Wet/01 - Drizzle.mp3",
      "/music/The Lantern Choir/The Long Road/01 - Mile One.mp3",
      "/music/Lantern/Long Division/01 - Remainder.mp3",
      "/music/Le Ciel Bleu/A Paper Kite/01 - Crescent.mp3",
      "/music/Brass Arcade/The Zebra Years/01 - Stripes.mp3",
      "/music/Brass Arcade/Middle/01 - Centre.mp3",
      "/music/Brass Arcade/Alpha/01 - First.mp3",
  };
  for (const char* f : files) TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile(f)));
  TEST_ASSERT_TRUE(idx.finish());
  const char* artists[] = {"Brass Arcade", "Le Ciel Bleu", "Lantern", "The Lantern Choir", "Theory of Rain"};
  const LibraryIndex::Span a = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(5, a.count);
  for (uint32_t i = 0; i < a.count; ++i) TEST_ASSERT_EQUAL_STRING(artists[i], idx.artistName(a[i]));
  // The rail: B, C, L L, T; "The Lantern Choir" is an L.
  using V = LibraryIndex::View;
  TEST_ASSERT_EQUAL_UINT32(0, idx.bucketStart(V::Artists, textfold::bucketOf('B')));
  TEST_ASSERT_EQUAL_UINT32(1, idx.bucketStart(V::Artists, textfold::bucketOf('C')));
  TEST_ASSERT_EQUAL_UINT32(2, idx.bucketStart(V::Artists, textfold::bucketOf('L')));
  TEST_ASSERT_EQUAL_UINT32(4, idx.bucketStart(V::Artists, textfold::bucketOf('M')));
  TEST_ASSERT_EQUAL_UINT32(4, idx.bucketStart(V::Artists, textfold::bucketOf('T')));
  TEST_ASSERT_EQUAL_UINT32(5, idx.bucketStart(V::Artists, textfold::bucketOf('U')));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('L'), idx.bucketAt(V::Artists, 3));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('C'), idx.bucketAt(V::Artists, 1));
  // Albums: "A Paper Kite" stays under A ("A" is no article here), "The
  // Long Road" goes after "Long Division", "The Zebra Years" under Z.
  const char* albums[] = {"A Paper Kite", "Alpha", "Long Division", "The Long Road", "Middle", "Wet", "The Zebra Years"};
  const LibraryIndex::Span z = idx.albumsAZ();
  TEST_ASSERT_EQUAL_UINT32(7, z.count);
  for (uint32_t i = 0; i < z.count; ++i) TEST_ASSERT_EQUAL_STRING(albums[i], idx.albumName(z[i]));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('L'), idx.bucketAt(V::Albums, 3));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('Z'), idx.bucketAt(V::Albums, 6));
  // An artist's albums too.
  const LibraryIndex::Span of = idx.albumsOf(findArtist(idx, "Brass Arcade"));
  TEST_ASSERT_EQUAL_UINT32(3, of.count);
  TEST_ASSERT_EQUAL_STRING("Alpha", idx.albumName(of[0]));
  TEST_ASSERT_EQUAL_STRING("Middle", idx.albumName(of[1]));
  TEST_ASSERT_EQUAL_STRING("The Zebra Years", idx.albumName(of[2]));
  // The Folders view: the names as they are.
  const char* folders[] = {"Brass Arcade", "Lantern", "Le Ciel Bleu", "The Lantern Choir", "Theory of Rain"};
  const LibraryIndex::Span top = idx.subfolders(LibraryIndex::rootFolder());
  TEST_ASSERT_EQUAL_UINT32(5, top.count);
  for (uint32_t i = 0; i < top.count; ++i) TEST_ASSERT_EQUAL_STRING(folders[i], idx.folderName(top[i]));
  // Every path still found (findTrack searches the folders' order).
  char buf[256];
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    idx.trackPath(t, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(t, idx.findTrack(buf));
  }
}

// Discs and numbers from the names, read folder by folder; an album plays
// disc by disc.
void test_disc_track_names_order_an_album() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      // "1-01": disc then number; the number written again goes (half
      // the folder's names write it so).
      "/music/Glass Orchard/Night Shift/2-01 01-Return.mp3",
      "/music/Glass Orchard/Night Shift/1-02. Second.mp3",
      "/music/Glass Orchard/Night Shift/2-02 - Glass Orchard - Last.flac",
      "/music/Glass Orchard/Night Shift/1-01 01-Opening.mp3",
      "/music/Glass Orchard/Night Shift/cover.jpg",
      // "101": discs 1-3 (301 had no number before: it played first).
      "/music/Glass Orchard/Box/301 - Encore.mp3",
      "/music/Glass Orchard/Box/201 - Middle.mp3",
      "/music/Glass Orchard/Box/101 - Start.mp3",
      "/music/Glass Orchard/Box/102 - Next.mp3",
      // "CD1 - 01 - Title": the restarted numbers stay apart.
      "/music/Glass Orchard/Live/CD2 - 01 - Encore.mp3",
      "/music/Glass Orchard/Live/CD1 - 02 - Song.mp3",
      "/music/Glass Orchard/Live/CD1 - 01 - Hello.mp3",
      // "Artist - Album - NN Title", a guest on one.
      "/music/Glass Orchard/Demos/Glass Orchard - Demos - 10 Ten.mp3",
      "/music/Glass Orchard/Demos/Glass Orchard - Demos - 09 Nine.mp3",
      "/music/Glass Orchard/Demos/Glass Orchard feat. Mira Lune - Demos - 02 Two.mp3",
      // Disc subfolders: folder by folder, as before.
      "/music/Glass Orchard/Double/CD2/01 - Glass Orchard - Back.mp3",
      "/music/Glass Orchard/Double/CD1/02 - Front Two.mp3",
      "/music/Glass Orchard/Double/CD1/01 - Front One.mp3",
      // A lone "1-02": the plain rule.
      "/music/Glass Orchard/Single/1-02 Lonely.mp3",
  };
  for (const char* f : files) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());
  expectTitles(idx, "Night Shift", {"1/1 Opening", "1/2 Second", "2/1 Return", "2/2 Last"});
  expectTitles(idx, "Box", {"1/1 Start", "1/2 Next", "2/1 Middle", "3/1 Encore"});
  expectTitles(idx, "Live", {"1/1 Hello", "1/2 Song", "2/1 Encore"});
  expectTitles(idx, "Demos", {"0/2 Two", "0/9 Nine", "0/10 Ten"});
  expectTitles(idx, "Double", {"0/1 Front One", "0/2 Front Two", "0/1 Back"});
  expectTitles(idx, "Single", {"0/1 02 Lonely"});
  TEST_ASSERT_EQUAL_UINT32(18, idx.tracksOfArtist(findArtist(idx, "Glass Orchard")).count);
  // The Folders view still lists the files by their names, and the paths
  // are the files'.
  const uint32_t ns = idx.album(findAlbum(idx, "Night Shift")).folder;
  const LibraryIndex::Span f = idx.filesIn(ns);
  TEST_ASSERT_EQUAL_UINT32(4, f.count);
  TEST_ASSERT_EQUAL_STRING("1-01 01-Opening.mp3", idx.trackFileName(f[0]));
  TEST_ASSERT_EQUAL_STRING("2-02 - Glass Orchard - Last.flac", idx.trackFileName(f[3]));
  char buf[256];
  idx.trackPath(f[3], buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/Glass Orchard/Night Shift/2-02 - Glass Orchard - Last.flac", buf);
  TEST_ASSERT_EQUAL_UINT32(f[3], idx.findTrack(buf));
  TEST_ASSERT_EQUAL_size_t(Heap::peak, idx.memory().buildPeak);
}

// The artist off the titles, only when it is the folder's; a compilation's
// names stay whole; years stay in titles.
void test_titles_lose_the_folders_artist() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      "/music/The Lantern Choir/Hymns/01 - Lantern Choir - Mile One.mp3",
      "/music/The Lantern Choir/Hymns/02 - The Lantern Choir - Mile Two.mp3",
      "/music/The Lantern Choir/Hymns/03 - Mile Three - Live.mp3",
      "/music/The Lantern Choir/Hymns/04 - Other Act - Duet.mp3",
      "/music/The Lantern Choir/Hymns/05 - 1999 Remix.mp3",
      "/music/The Lantern Choir/Lantern Choir - Loose One.mp3",  // in the artist's own folder
      "/music/Various Artists/Mixtape/01 - Act One - Song.mp3",
      "/music/Various Artists/Mixtape/02 - Act Two - Tune.mp3",
      "/music/Various Artists/Mixtape/Act Three - 03 - Air.mp3",
      "/music/Glass Orchard - Opening.mp3",  // the top of /music: no artist folder
      "/music/R_K Unit/Signals/01 - R-K Unit - Static.mp3",
      "/music/R_K Unit/Signals/1999 - Party Song.mp3",
      "/music/R_K Unit/Signals/2001 - Odyssey.mp3",
  };
  for (const char* f : files) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());
  expectTitles(idx, "Hymns", {"0/1 Mile One", "0/2 Mile Two", "0/3 Mile Three - Live", "0/4 Other Act - Duet",
                              "0/5 1999 Remix"});
  expectTitles(idx, "Mixtape", {"0/0 Act Three - 03 - Air", "0/1 Act One - Song", "0/2 Act Two - Tune"});
  expectTitles(idx, "Signals", {"0/0 1999 - Party Song", "0/0 2001 - Odyssey", "0/1 Static"});
  bool sawLoose = false, sawTop = false;
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    const std::string name = idx.trackFileName(t);
    if (name == "Lantern Choir - Loose One.mp3") {
      TEST_ASSERT_EQUAL_STRING("Loose One", title(idx, t).c_str());
      sawLoose = true;
    }
    if (name == "Glass Orchard - Opening.mp3") {
      TEST_ASSERT_EQUAL_STRING("Glass Orchard - Opening", title(idx, t).c_str());
      sawTop = true;
    }
  }
  TEST_ASSERT_TRUE(sawLoose && sawTop);
}

// "Artist - Album - NN Title" whose album or work part differs from name
// to name: each album, work or disc keeps its tracks together.
void test_prefixes_that_differ_keep_their_order() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      // A disc part at the album part's end: disc, then number.
      "/music/Glass Orchard/Night Shift/Glass Orchard - Night Shift (Disc 2) - 02 Last.mp3",
      "/music/Glass Orchard/Night Shift/Glass Orchard - Night Shift (Disc 1) - 01 Opening.mp3",
      "/music/Glass Orchard/Night Shift/Glass Orchard - Night Shift (Disc 2) - 01 Return.mp3",
      "/music/Glass Orchard/Night Shift/Glass Orchard - Night Shift (Disc 1) - 02 Second.mp3",
      // Two works, each numbered from 1: the file names' order.
      "/music/Composer/Suites/Composer - Glass Suite No. 2 - 1. Evening.mp3",
      "/music/Composer/Suites/Composer - Glass Suite No. 1 - 2. Noon.mp3",
      "/music/Composer/Suites/Composer - Glass Suite No. 1 - 1. Morning.mp3",
      "/music/Composer/Suites/Composer - Glass Suite No. 2 - 2. Night.mp3",
      // Two EPs in the artist's own folder.
      "/music/Lantern Choir/Lantern Choir - Second EP - 01 Iron Kite.mp3",
      "/music/Lantern Choir/Lantern Choir - First EP - 02 Glass Road.mp3",
      "/music/Lantern Choir/Lantern Choir - First EP - 01 Paper Kite.mp3",
  };
  for (const char* f : files) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());
  expectTitles(idx, "Night Shift", {"1/1 Opening", "1/2 Second", "2/1 Return", "2/2 Last"});
  expectTitles(idx, "Suites",
               {"0/0 Glass Suite No. 1 - 1. Morning", "0/0 Glass Suite No. 1 - 2. Noon",
                "0/0 Glass Suite No. 2 - 1. Evening", "0/0 Glass Suite No. 2 - 2. Night"});
  const LibraryIndex::Span t = idx.tracksOfArtist(findArtist(idx, "Lantern Choir"));
  const char* eps[] = {"First EP - 01 Paper Kite", "First EP - 02 Glass Road", "Second EP - 01 Iron Kite"};
  TEST_ASSERT_EQUAL_UINT32(3, t.count);
  for (uint32_t i = 0; i < t.count; ++i) {
    TEST_ASSERT_EQUAL_STRING(eps[i], title(idx, t[i]).c_str());
    TEST_ASSERT_EQUAL_UINT8(0, idx.track(t[i]).number);
  }
}

// An .opus file's name is read as an .mp3's or a .flac's is, in the one
// pass over its folder: the disc-track and "Artist - NN - Title" shapes
// count the folder's names whatever their extensions, the folder's artist
// comes off a title, and an artist of .opus files sorts past its "The".
void test_opus_names_are_read_like_the_others() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* files[] = {
      // Disc and number, in a folder of .opus files alone (either case).
      "/music/Glass Orchard/Night Shift/2-01 Return.opus",
      "/music/Glass Orchard/Night Shift/1-02 Second.OPUS",
      "/music/Glass Orchard/Night Shift/1-01 Opening.opus",
      // "101 Title": the folder rule counts the three formats together.
      "/music/Glass Orchard/Box/201 - Middle.flac",
      "/music/Glass Orchard/Box/102 - Next.mp3",
      "/music/Glass Orchard/Box/101 - Start.opus",
      // "Artist - NN - Title" across the formats; the artist off a title.
      "/music/Glass Orchard/Demos/Glass Orchard - 03 - Three.opus",
      "/music/Glass Orchard/Demos/Glass Orchard - 01 - One.mp3",
      "/music/Glass Orchard/Demos/Glass Orchard - 02 - Two.flac",
      "/music/Glass Orchard/Live/02 - Other Act - Duet.opus",
      "/music/Glass Orchard/Live/01 - Glass Orchard - Mile One.opus",
      // An artist of .opus files sorts past its "The"; an .ogg beside them
      // is an other file.
      "/music/The Lantern Choir/Hymns/01 - Mile One.opus",
      "/music/The Lantern Choir/Hymns/02 - Mile Two.ogg",
      "/music/Lantern/Long Division/01 - Remainder.opus",
  };
  for (const char* f : files) idx.addFile(f);
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(13, idx.trackCount());
  expectTitles(idx, "Night Shift", {"1/1 Opening", "1/2 Second", "2/1 Return"});
  expectTitles(idx, "Box", {"1/1 Start", "1/2 Next", "2/1 Middle"});
  expectTitles(idx, "Demos", {"0/1 One", "0/2 Two", "0/3 Three"});
  expectTitles(idx, "Live", {"0/1 Mile One", "0/2 Other Act - Duet"});
  // The formats stay the extensions'.
  using F = LibraryIndex::Format;
  const LibraryIndex::Span box = idx.tracksOfAlbum(findAlbum(idx, "Box"));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(F::Opus), static_cast<int>(idx.track(box[0]).format));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(F::Mp3), static_cast<int>(idx.track(box[1]).format));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(F::Flac), static_cast<int>(idx.track(box[2]).format));
  const LibraryIndex::Span ns = idx.tracksOfAlbum(findAlbum(idx, "Night Shift"));
  for (uint32_t i = 0; i < ns.count; ++i) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(F::Opus), static_cast<int>(idx.track(ns[i]).format));
  }
  TEST_ASSERT_EQUAL_STRING("1-02 Second.OPUS", idx.trackFileName(ns[1]));
  // The artists: Glass Orchard, Lantern, The Lantern Choir (an L).
  const char* artists[] = {"Glass Orchard", "Lantern", "The Lantern Choir"};
  const LibraryIndex::Span a = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(3, a.count);
  for (uint32_t i = 0; i < a.count; ++i) TEST_ASSERT_EQUAL_STRING(artists[i], idx.artistName(a[i]));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('L'), idx.bucketAt(LibraryIndex::View::Artists, 2));
  TEST_ASSERT_EQUAL_UINT16(1, idx.folder(idx.album(findAlbum(idx, "Hymns")).folder).otherCount);
  // Saved and loaded: the names, discs and formats come back.
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 11));
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 11)));
  expectSameIndex(idx, back);
  expectTitles(back, "Night Shift", {"1/1 Opening", "1/2 Second", "2/1 Return"});
  for (uint32_t t = 0; t < idx.trackCount(); ++t) {
    TEST_ASSERT_EQUAL_INT(static_cast<int>(idx.track(t).format), static_cast<int>(back.track(t).format));
  }
}

// A cache saved before the names were read this way (version 2: 0.6.0's),
// by feature/opus before the merge (version 3: the same records, .opus
// tracks and the old names) or by dev before it (version 4: the names, no
// .opus tracks) is Outdated, so the Library builds the index again and
// saves version 5.
void test_an_older_cache_version_is_rebuilt() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addFile("/music/Glass Orchard/Night Shift/1-01 Opening.mp3");
  idx.addFile("/music/Glass Orchard/Night Shift/1-02 Second.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 9));
  std::vector<uint8_t> bytes(file.data(), file.data() + file.size());
  uint32_t version;
  std::memcpy(&version, bytes.data() + 4, 4);  // the header's second word
  TEST_ASSERT_EQUAL_UINT32(6, version);
  LibraryIndex back(Heap::alloc, Heap::release);
  {
    MemorySource in(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 9)));
    expectSameIndex(idx, back);
    const uint32_t first = back.tracksOfAlbum(0)[0];
    TEST_ASSERT_EQUAL_STRING("Opening", title(back, first).c_str());
    TEST_ASSERT_EQUAL_UINT8(1, back.track(first).disc);
    TEST_ASSERT_EQUAL_UINT8(1, back.track(first).number);
  }
  for (const uint32_t old : {1u, 2u, 3u, 4u, 5u}) {
    std::memcpy(bytes.data() + 4, &old, 4);
    MemorySource in(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), static_cast<int>(back.load(in, 9)));
    TEST_ASSERT_FALSE(back.ready());
  }
}

// ---- tag records (library.idx v6; docs/METADATA.md 5.4, Stage A) ----

namespace {

// A record's view with the fields a test gives (the rest absent).
struct View {
  LibraryIndex::TagView v;
  View& title(const char* s) { return set(&v.title, &v.titleLen, s); }
  View& artist(const char* s) { return set(&v.artist, &v.artistLen, s); }
  View& album(const char* s) { return set(&v.album, &v.albumLen, s); }
  View& albumArtist(const char* s) { return set(&v.albumArtist, &v.albumArtistLen, s); }
  View& albumSort(const char* s) { return set(&v.albumSort, &v.albumSortLen, s); }
  View& artistSort(const char* s) { return set(&v.artistSort, &v.artistSortLen, s); }
  View& year(uint16_t y) {
    v.year = y;
    return *this;
  }
  View& track(uint16_t t) {
    v.track = t;
    return *this;
  }
  View& disc(uint16_t d) {
    v.disc = d;
    return *this;
  }
  View& compilation() {
    v.compilation = 1;
    return *this;
  }
  View& ms(uint32_t d) {
    v.durationMs = d;
    return *this;
  }
  View& set(const char** p, size_t* n, const char* s) {
    *p = s;
    *n = std::strlen(s);
    return *this;
  }
};

uint32_t trackAt(const LibraryIndex& idx, const char* path) { return idx.findTrack(path); }

std::vector<std::string> albumOrder(const LibraryIndex& idx, uint32_t album) {
  std::vector<std::string> out;
  const LibraryIndex::Span t = idx.tracksOfAlbum(album);
  for (uint32_t i = 0; i < t.count; ++i) out.push_back(idx.trackFileName(t[i]));
  return out;
}

void expectOrder(const LibraryIndex& idx, uint32_t album, const std::vector<std::string>& want) {
  const std::vector<std::string> got = albumOrder(idx, album);
  TEST_ASSERT_EQUAL_UINT32(want.size(), got.size());
  for (size_t i = 0; i < want.size(); ++i) TEST_ASSERT_EQUAL_STRING(want[i].c_str(), got[i].c_str());
}

}  // namespace

// A folder album named by its tracks' records: the most common album value
// (not the folder's "(2019)"), year and artist line; titles a slice of the
// file name when they're inside it; the track artist shown only where it
// differs from the line; a missing number last; a file with no record named
// from its path in the same album.
void test_records_name_an_album() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  const char* dir = "/music/Glass Orchard/Night Shift (2019)/";
  auto add = [&](const char* name, const View& v) {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addRecord((std::string(dir) + name).c_str(), v.v)));
  };
  add("02 - Second.mp3", View().title("Second Song").artist("Glass Orchard").album("Night Shift").year(2019).track(2));
  add("01 - Opening.mp3", View().title("Opening").artist("Glass Orchard").album("Night Shift").year(2019).track(1).ms(184600));
  add("03 - Third.flac",
      View().title("Third").artist("Glass Orchard\x1FMira Lune").album("Night Shift (Live)").year(2018).track(3));
  TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile((std::string(dir) + "Bonus.mp3").c_str())));
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(1, idx.albumCount());
  TEST_ASSERT_EQUAL_STRING("Night Shift", idx.albumName(0));
  TEST_ASSERT_EQUAL_UINT16(2019, idx.album(0).year);
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", idx.albumArtistLine(0));
  TEST_ASSERT_EQUAL_UINT8(1, idx.album(0).discs);
  TEST_ASSERT_TRUE(idx.album(0).flags & LibraryIndex::kTagged);
  TEST_ASSERT_FALSE(idx.album(0).flags & LibraryIndex::kLoose);
  // By number, the one with none (the record-less Bonus) last.
  expectOrder(idx, 0, {"01 - Opening.mp3", "02 - Second.mp3", "03 - Third.flac", "Bonus.mp3"});
  const uint32_t opening = trackAt(idx, "/music/Glass Orchard/Night Shift (2019)/01 - Opening.mp3");
  const uint32_t second = trackAt(idx, "/music/Glass Orchard/Night Shift (2019)/02 - Second.mp3");
  const uint32_t third = trackAt(idx, "/music/Glass Orchard/Night Shift (2019)/03 - Third.flac");
  const uint32_t bonus = trackAt(idx, "/music/Glass Orchard/Night Shift (2019)/Bonus.mp3");
  TEST_ASSERT_EQUAL_STRING("Opening", title(idx, opening).c_str());
  // Inside the file name: its title is a slice of it.
  TEST_ASSERT_TRUE(idx.track(opening).title >= idx.track(opening).name &&
                   idx.track(opening).title < idx.track(opening).name + 20);
  TEST_ASSERT_EQUAL_STRING("Second Song", title(idx, second).c_str());
  TEST_ASSERT_EQUAL_UINT16(2, idx.track(second).number);
  TEST_ASSERT_EQUAL_STRING("Bonus", title(idx, bonus).c_str());
  TEST_ASSERT_EQUAL_UINT16(0, idx.track(bonus).number);
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromPath, idx.track(bonus).flags & LibraryIndex::kSourceMask);
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kFromDevice, idx.track(opening).flags & LibraryIndex::kSourceMask);
  TEST_ASSERT_EQUAL_UINT16(185, idx.track(opening).durationS);
  TEST_ASSERT_EQUAL_UINT16(0, idx.track(bonus).durationS);
  TEST_ASSERT_EQUAL_UINT32(LibraryIndex::kNone, idx.track(opening).trackArtist);
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", idx.trackArtistName(opening));
  TEST_ASSERT_EQUAL_STRING("Glass Orchard, Mira Lune", idx.trackArtistName(third));
  TEST_ASSERT_EQUAL_STRING("Glass Orchard", idx.trackArtistName(bonus));
  // The Folders view and the paths stay the files'.
  TEST_ASSERT_EQUAL_STRING("Night Shift (2019)", idx.folderName(idx.album(0).folder));
  char buf[200];
  idx.trackPath(second, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/Glass Orchard/Night Shift (2019)/02 - Second.mp3", buf);
  // Saved and loaded: the same.
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 3));
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size(), 77);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 3)));
  expectSameIndex(idx, back);
  TEST_ASSERT_EQUAL_STRING("Night Shift", back.albumName(0));
  TEST_ASSERT_EQUAL_STRING("Glass Orchard, Mira Lune", back.trackArtistName(third));
  TEST_ASSERT_EQUAL_UINT64(idx.buildStamp(), back.buildStamp());
}

// The artist line's chain (5.4): the album-artist display, else "Various
// Artists" for a compilation, else the most common track-artist display,
// else the artist folder. Ties go to the smallest bytes, years to the
// earliest; an artist's own loose tracks keep "" and no year.
void test_album_votes() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  auto add = [&](const char* path, const View& v) {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addRecord(path, v.v)));
  };
  add("/music/Act/One/01 - a.mp3", View().artist("Guest").albumArtist("Act Band").album("B Side").year(2004));
  add("/music/Act/One/02 - b.mp3", View().artist("Act Band").album("A Side").year(2002));
  add("/music/Act/Two/01 - a.mp3", View().artist("Somebody").compilation().album("Mix"));
  add("/music/Act/Two/02 - b.mp3", View().artist("Else").album("Mix"));
  add("/music/Act/Three/01 - a.mp3", View().artist("Act\x1F" "Act").album("Three"));
  add("/music/Act/Three/02 - b.mp3", View().artist("Other"));
  add("/music/Act/Three/03 - c.mp3", View().artist("Act"));
  idx.addFile("/music/Act/Four/01 - a.mp3");
  add("/music/Act/Loose.mp3", View().album("Some Album").year(1999));
  TEST_ASSERT_TRUE(idx.finish());
  auto albumOf = [&](const char* path) { return idx.track(trackAt(idx, path)).album; };
  const uint32_t one = albumOf("/music/Act/One/01 - a.mp3");
  TEST_ASSERT_EQUAL_STRING("A Side", idx.albumName(one));  // one vote each: the smallest bytes
  TEST_ASSERT_EQUAL_UINT16(2002, idx.album(one).year);     // one each: the earliest
  TEST_ASSERT_EQUAL_STRING("Act Band", idx.albumArtistLine(one));
  TEST_ASSERT_EQUAL_STRING("Guest", idx.trackArtistName(trackAt(idx, "/music/Act/One/01 - a.mp3")));
  const uint32_t two = albumOf("/music/Act/Two/01 - a.mp3");
  TEST_ASSERT_EQUAL_STRING("Various Artists", idx.albumArtistLine(two));
  TEST_ASSERT_EQUAL_STRING("Somebody", idx.trackArtistName(trackAt(idx, "/music/Act/Two/01 - a.mp3")));
  const uint32_t three = albumOf("/music/Act/Three/01 - a.mp3");
  TEST_ASSERT_EQUAL_STRING("Act", idx.albumArtistLine(three));  // "Act" (twice: the display dedups) against "Other"
  TEST_ASSERT_EQUAL_STRING("Three", idx.albumName(three));
  const uint32_t four = albumOf("/music/Act/Four/01 - a.mp3");
  TEST_ASSERT_EQUAL_STRING("Four", idx.albumName(four));
  TEST_ASSERT_EQUAL_STRING("Act", idx.albumArtistLine(four));  // the folder artist
  TEST_ASSERT_FALSE(idx.album(four).flags & LibraryIndex::kTagged);
  const uint32_t loose = albumOf("/music/Act/Loose.mp3");
  TEST_ASSERT_EQUAL_STRING("", idx.albumName(loose));
  TEST_ASSERT_TRUE(idx.album(loose).flags & LibraryIndex::kLoose);
  TEST_ASSERT_EQUAL_UINT16(0, idx.album(loose).year);
  // The artist: "Act" and "Act Band" tie (two tracks each); the smaller
  // bytes win, and match the folder.
  TEST_ASSERT_EQUAL_STRING("Act", idx.artistName(idx.album(one).artist));
}

// An album votes on its first 512 tracks: 300 say "Y" first, then 300 "X";
// counted whole it would be a tie, which "X" wins by its bytes.
void test_album_votes_on_its_first_512_tracks() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music", 600));
  char path[96];
  for (int i = 0; i < 600; ++i) {
    snprintf(path, sizeof(path), "/music/Big/Box/%03d - t.mp3", i + 1);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addRecord(path, View().album(i < 300 ? "Y" : "X").v)));
  }
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(1, idx.albumCount());
  TEST_ASSERT_EQUAL_STRING("Y", idx.albumName(0));
  TEST_ASSERT_EQUAL_UINT32(600, idx.tracksOfAlbum(0).count);
}

// The artist folder shows its tracks' spelling when textfold::sameName()
// matches it to the folder's (case, accents, a FAT-illegal character, "The");
// another name leaves the folder's.
void test_artist_display_names() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addRecord("/music/R_K Unit/Live/01 - a.mp3", View().albumArtist("R/K Unit").artist("R/K Unit feat. Guest").v);
  idx.addRecord("/music/R_K Unit/Live/02 - b.mp3", View().albumArtist("R/K Unit").v);
  idx.addRecord("/music/lantern choir/Hymns/01 - a.mp3", View().artist("The Lantern Choir").v);
  idx.addRecord("/music/Unsorted/Mix/01 - a.mp3", View().artist("Someone Else").v);
  idx.addFile("/music/Path Only/Album/01 - a.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  const char* artists[] = {"The Lantern Choir", "Path Only", "R/K Unit", "Unsorted"};
  const LibraryIndex::Span a = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(4, a.count);
  for (uint32_t i = 0; i < a.count; ++i) TEST_ASSERT_EQUAL_STRING(artists[i], idx.artistName(a[i]));
  // The Folders view keeps the folders' names.
  const LibraryIndex::Span top = idx.subfolders(LibraryIndex::rootFolder());
  TEST_ASSERT_EQUAL_STRING("lantern choir", idx.folderName(top[0]));
  TEST_ASSERT_EQUAL_STRING("R_K Unit", idx.folderName(top[2]));
  // The rail follows the names shown: "The Lantern Choir" under L.
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('L'), idx.bucketAt(LibraryIndex::View::Artists, 0));
}

// Inside an album with records: by disc (none is 1), then folder, then
// number (none last). An artist's albums newest first, no year last; sort
// tags order the A-Z lists and their rails.
void test_stage_a_orders() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addRecord("/music/Band/Set/a.mp3", View().disc(2).track(1).v);
  idx.addRecord("/music/Band/Set/b.mp3", View().disc(1).track(2).v);
  idx.addRecord("/music/Band/Set/c.mp3", View().disc(1).track(1).v);
  idx.addRecord("/music/Band/Set/d.mp3", View().disc(1).v);
  idx.addRecord("/music/Band/Set/CD3/01 - e.mp3", View().disc(3).track(1).v);
  idx.addRecord("/music/Band/Old/01 - x.mp3", View().year(1999).v);
  idx.addRecord("/music/Band/New/01 - x.mp3", View().year(2011).v);
  idx.addRecord("/music/Band/Zulu/01 - x.mp3", View().album("Zulu").albumSort("Alpha").v);
  idx.addRecord("/music/Choir Lantern/Hymns/01 - x.mp3", View().artist("Choir Lantern").artistSort("Lantern Choir").v);
  TEST_ASSERT_TRUE(idx.finish());
  const uint32_t set = idx.track(trackAt(idx, "/music/Band/Set/a.mp3")).album;
  expectOrder(idx, set, {"c.mp3", "b.mp3", "d.mp3", "a.mp3", "01 - e.mp3"});
  TEST_ASSERT_EQUAL_UINT8(3, idx.album(set).discs);
  const LibraryIndex::Span of = idx.albumsOf(idx.album(set).artist);
  TEST_ASSERT_EQUAL_UINT32(4, of.count);
  TEST_ASSERT_EQUAL_STRING("New", idx.albumName(of[0]));
  TEST_ASSERT_EQUAL_STRING("Old", idx.albumName(of[1]));
  TEST_ASSERT_EQUAL_STRING("Zulu", idx.albumName(of[2]));  // no year: by sort key, "Alpha" before "Set"
  TEST_ASSERT_EQUAL_STRING("Set", idx.albumName(of[3]));
  // Albums A-Z: "Zulu" under A, by its sort tag.
  const LibraryIndex::Span az = idx.albumsAZ();
  TEST_ASSERT_EQUAL_STRING("Zulu", idx.albumName(az[0]));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('A'), idx.bucketAt(LibraryIndex::View::Albums, 0));
  TEST_ASSERT_EQUAL_UINT32(0, idx.bucketStart(LibraryIndex::View::Albums, textfold::bucketOf('A')));
  // Artists: "Choir Lantern" sorts as "Lantern Choir", after "Band".
  const LibraryIndex::Span ar = idx.artistsAZ();
  TEST_ASSERT_EQUAL_STRING("Band", idx.artistName(ar[0]));
  TEST_ASSERT_EQUAL_STRING("Choir Lantern", idx.artistName(ar[1]));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('L'), idx.bucketAt(LibraryIndex::View::Artists, 1));
}

namespace {

// The rail's name of a row, as LibraryPage::railName() gives it: the sort
// name of the entry's sort key.
struct RailOf {
  const LibraryIndex* idx;
  LibraryIndex::View view;
};
const char* railNameOf(void* ctx, uint32_t row) {
  const RailOf& r = *static_cast<const RailOf*>(ctx);
  return r.view == LibraryIndex::View::Artists ? textfold::sortName(r.idx->artistSortKey(r.idx->artistsAZ()[row]))
                                               : textfold::sortName(r.idx->albumSortKey(r.idx->albumsAZ()[row]));
}

// Every row's rail letter (the UI's) is the bucket the index put it in.
void expectRailAgrees(const LibraryIndex& idx, LibraryIndex::View view) {
  RailOf ctx{&idx, view};
  const uint32_t n = view == LibraryIndex::View::Artists ? idx.artistCount() : idx.albumCount();
  for (uint32_t row = 0; row < n; ++row)
    TEST_ASSERT_EQUAL_INT(idx.bucketAt(view, row), textfold::bucketOf(textfold::railKey(railNameOf(&ctx, row))));
}

}  // namespace

// The sort keys (review of N2): what the A-Z lists sort by is kept with the
// views, and saved, so the rail, a row's letter and the jump grid key on it
// as the index's buckets do. Daniel Bowery tagged "Bowery, Daniel" sorts among
// the B's, and the jump grid finds him there, at both of its levels (keyed
// on the name shown, B would end at him and D would find none). A sort tag
// of whitespace alone is no sort tag (5.4's orderName takes the sort tag's
// nameKey only when it isn't empty), and whitespace around one is dropped.
void test_sort_keys_are_kept_and_blank_ones_ignored() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  for (const char* a : {"Amber Fold", "Bartleby Pines", "Basalt", "Bayou Nine", "Brine Choir", "Burrow", "Copper Lane"})
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile((std::string("/music/") + a + "/Album/01 - x.mp3").c_str())));
  idx.addRecord("/music/Daniel Bowery/Night Ferry/01 - x.mp3",
                View().artist("Daniel Bowery").artistSort("Bowery, Daniel").album("Night Ferry").v);
  // A lone tab is stored as one space (2.3.6): not a sort tag.
  idx.addRecord("/music/Zephyr Kite/Zebra/01 - x.mp3",
                View().artist("Zephyr Kite").artistSort(" ").album("Zebra").albumSort(" ").v);
  idx.addRecord("/music/Zephyr Kite/Yonder/01 - x.mp3",
                View().artist("Zephyr Kite").album("Yonder").albumSort("  Alpha\t").v);
  TEST_ASSERT_TRUE(idx.finish());

  const char* artists[] = {"Amber Fold",  "Bartleby Pines", "Basalt",      "Bayou Nine", "Daniel Bowery",
                           "Brine Choir", "Burrow",         "Copper Lane", "Zephyr Kite"};
  const LibraryIndex::Span az = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(9, az.count);
  for (uint32_t i = 0; i < az.count; ++i) TEST_ASSERT_EQUAL_STRING(artists[i], idx.artistName(az[i]));
  TEST_ASSERT_EQUAL_STRING("Bowery, Daniel", idx.artistSortKey(az[4]));
  TEST_ASSERT_EQUAL_STRING("Zephyr Kite", idx.artistSortKey(az[8]));  // not " ": not first, under '#'
  TEST_ASSERT_EQUAL_STRING("Amber Fold", idx.artistSortKey(az[0]));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('B'), idx.bucketAt(LibraryIndex::View::Artists, 4));
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('Z'), idx.bucketAt(LibraryIndex::View::Artists, 8));
  const char* albums[] = {"Album", "Album", "Album",  "Album",       "Album",
                          "Album", "Album", "Yonder", "Night Ferry", "Zebra"};
  const LibraryIndex::Span bz = idx.albumsAZ();
  TEST_ASSERT_EQUAL_UINT32(10, bz.count);
  for (uint32_t i = 0; i < bz.count; ++i) TEST_ASSERT_EQUAL_STRING(albums[i], idx.albumName(bz[i]));
  TEST_ASSERT_EQUAL_STRING("Alpha", idx.albumSortKey(bz[7]));  // "  Alpha\t": trimmed, under A
  TEST_ASSERT_EQUAL_STRING("Zebra", idx.albumSortKey(bz[9]));  // " ": its name
  TEST_ASSERT_EQUAL_INT(textfold::bucketOf('A'), idx.bucketAt(LibraryIndex::View::Albums, 7));

  // Saved and loaded: the keys come back, and the UI's letters, the jump
  // grid and the buckets agree.
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, 5));
  LibraryIndex back(Heap::alloc, Heap::release);
  MemorySource in(file.data(), file.size());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(back.load(in, 5)));
  expectSameIndex(idx, back);
  for (const LibraryIndex* x : {static_cast<const LibraryIndex*>(&idx), static_cast<const LibraryIndex*>(&back)}) {
    expectRailAgrees(*x, LibraryIndex::View::Artists);
    expectRailAgrees(*x, LibraryIndex::View::Albums);
    RailOf ctx{x, LibraryIndex::View::Artists};
    int32_t first[jump::kCells], end[jump::kCells];
    jump::letters(x->artistCount(), railNameOf, &ctx, first, end);
    const int b = textfold::bucketOf('B'), c = textfold::bucketOf('C'), d = textfold::bucketOf('D');
    TEST_ASSERT_EQUAL_INT32(1, first[b]);
    TEST_ASSERT_EQUAL_INT32(7, end[b]);  // Bartleby Pines to Burrow, Bowery among them
    TEST_ASSERT_EQUAL_INT32(static_cast<int32_t>(x->bucketStart(LibraryIndex::View::Artists, c)), first[c]);
    TEST_ASSERT_EQUAL_INT32(-1, first[d]);
    int32_t second[jump::kCells];
    jump::seconds(1, 7, railNameOf, &ctx, second);
    TEST_ASSERT_EQUAL_INT32(4, second['o' - 'a' + 1]);  // "Bo": Bowery
    TEST_ASSERT_EQUAL_INT32(5, second['r' - 'a' + 1]);  // "Br": Brine Choir
  }
}

// Artists in a script Full folding can't spell (docs/I18N.md, phase 0;
// made-up names): an artist folder whose tags name it the same way, by its
// letters in Unicode lower case, takes the tags' spelling and their
// ARTISTSORT, as a Latin one does (the gate dropped both before: sameName()
// saw no letter in them). A Latin folder for a Cyrillic tag is another
// name. The names without a sort tag sort under '#' by script (Greek
// before Cyrillic), then by letter, not by length.
void test_other_scripts_take_their_sort_tags() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  idx.addRecord("/music/Кот Лампа/Мост/01 - x.mp3", View().artist("Кот Лампа").artistSort("Kot Lampa").v);
  idx.addRecord("/music/Кот Лампа/Мост/02 - y.mp3", View().artist("Кот Лампа").artistSort("Kot Lampa").v);
  idx.addRecord("/music/Синий Мост/Утро/01 - x.mp3", View().artist("СИНИЙ МОСТ").artistSort("Siniy Most").v);
  idx.addRecord("/music/山川/海/01 - x.mp3", View().artist("山川").artistSort("Yamakawa").v);
  idx.addRecord("/music/Ruby Ferns/Dawn/01 - x.mp3", View().artist("Рубин Папоротник").artistSort("Rubin").v);
  idx.addRecord("/music/Νησί/Ήλιος/01 - x.mp3", View().artist("Νησί").v);
  idx.addRecord("/music/Ая Море/Песни/01 - x.mp3", View().artist("Ая Море").v);
  idx.addRecord("/music/Аквамарин/Песни/01 - x.mp3", View().artist("Аквамарин").v);
  idx.addRecord("/music/Amber Fold/Album/01 - x.mp3", View().artist("Amber Fold").v);
  TEST_ASSERT_TRUE(idx.finish());

  struct Row {
    const char* name;
    const char* key;
    char rail;
  } want[] = {
      {"Νησί", "Νησί", '#'},            // Greek
      {"Аквамарин", "Аквамарин", '#'},  // Cyrillic: А-к before А-я (by length, it came after)
      {"Ая Море", "Ая Море", '#'},
      {"Amber Fold", "Amber Fold", 'A'},
      {"Кот Лампа", "Kot Lampa", 'K'},  // the tags' sort name
      {"Ruby Ferns", "Ruby Ferns", 'R'},  // a Latin folder: its own name, no sort tag
      {"СИНИЙ МОСТ", "Siniy Most", 'S'},  // the tags' spelling (case), and sort name
      {"山川", "Yamakawa", 'Y'},
  };
  const LibraryIndex::Span az = idx.artistsAZ();
  TEST_ASSERT_EQUAL_UINT32(sizeof(want) / sizeof(want[0]), az.count);
  for (uint32_t i = 0; i < az.count; ++i) {
    TEST_ASSERT_EQUAL_STRING(want[i].name, idx.artistName(az[i]));
    TEST_ASSERT_EQUAL_STRING(want[i].key, idx.artistSortKey(az[i]));
    TEST_ASSERT_EQUAL_INT(textfold::bucketOf(want[i].rail), idx.bucketAt(LibraryIndex::View::Artists, i));
  }
  expectRailAgrees(idx, LibraryIndex::View::Artists);
  expectRailAgrees(idx, LibraryIndex::View::Albums);
}

// The library roots (the transfer's LIBR, 2.8.6): a file's artist and album
// are its folders at depths 1 and 2 below the longest root that holds it.
void test_library_roots() {
  const char* roots[] = {"Lib B", "Lib A", "Lib A/Inner"};
  LibraryIndex idx(Heap::alloc, Heap::release);
  LibraryIndex::Sizing s;
  s.tracks = 8;
  TEST_ASSERT_TRUE(idx.begin(s, "/music", roots, 3));
  idx.addFile("/music/Lib A/Artist/Album/01 - x.mp3");
  idx.addFile("/music/Lib A/Artist/Album/CD2/01 - y.mp3");
  idx.addFile("/music/Lib A/Artist/loose.mp3");
  idx.addFile("/music/Lib A/top.mp3");
  idx.addFile("/music/Lib A/Inner/Other/Record/01 - z.mp3");
  idx.addFile("/music/Lib AB/Artist/Album/01 - w.mp3");  // not under "Lib A"
  idx.addFile("/music/root.mp3");
  TEST_ASSERT_TRUE(idx.finish());
  auto artistOf = [&](const char* p) { return std::string(idx.artistName(idx.track(trackAt(idx, p)).artist)); };
  auto albumOf = [&](const char* p) { return std::string(idx.albumName(idx.track(trackAt(idx, p)).album)); };
  TEST_ASSERT_EQUAL_STRING("Artist", artistOf("/music/Lib A/Artist/Album/01 - x.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Album", albumOf("/music/Lib A/Artist/Album/01 - x.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Album", albumOf("/music/Lib A/Artist/Album/CD2/01 - y.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("", albumOf("/music/Lib A/Artist/loose.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("", artistOf("/music/Lib A/top.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Other", artistOf("/music/Lib A/Inner/Other/Record/01 - z.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Record", albumOf("/music/Lib A/Inner/Other/Record/01 - z.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Lib AB", artistOf("/music/Lib AB/Artist/Album/01 - w.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("Artist", albumOf("/music/Lib AB/Artist/Album/01 - w.mp3").c_str());
  TEST_ASSERT_EQUAL_STRING("", artistOf("/music/root.mp3").c_str());
  // The two loose albums of artist "" (the root's and Lib A's) stay apart:
  // albums are their folders.
  TEST_ASSERT_NOT_EQUAL(idx.track(trackAt(idx, "/music/root.mp3")).album,
                        idx.track(trackAt(idx, "/music/Lib A/top.mp3")).album);
  // Without the roots, Lib A is an artist.
  LibraryIndex plain(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(plain.begin("/music"));
  plain.addFile("/music/Lib A/Artist/Album/01 - x.mp3");
  TEST_ASSERT_TRUE(plain.finish());
  TEST_ASSERT_EQUAL_STRING("Lib A", plain.artistName(0));
}

// library.idx v6's header: the hard inputs (the transfer's identity, T used,
// today's walk signature) must be equal to load (else Stale); the soft ones
// (D's CRC, the journal) come back for the caller to compare. Another rules
// version is Outdated.
void test_inputs_of_the_cache() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  LibraryIndex::Inputs in;
  in.cardId = 0x5EEDC0DE0A1B2C3Dull;
  in.generation = 42;
  in.commitId = 0x1122334455667788ull;
  in.tagsCrc = 0xCAFEF00Du;
  in.transfer = true;
  in.deviceCrc = 7;
  in.journalSeq = 9;
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, in));
  std::vector<uint8_t> bytes(file.data(), file.data() + file.size());
  LibraryIndex back(Heap::alloc, Heap::release);
  auto load = [&](const std::vector<uint8_t>& b, const LibraryIndex::Inputs& e) {
    MemorySource src(b.data(), b.size());
    return static_cast<int>(back.load(src, e));
  };
  LibraryIndex::Inputs expect = in;
  expect.deviceCrc = 8;  // the scan went on: soft
  expect.journalSeq = 10;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), load(bytes, expect));
  TEST_ASSERT_TRUE(back.inputs().sameHard(in));
  TEST_ASSERT_EQUAL_UINT32(7, back.inputs().deviceCrc);
  TEST_ASSERT_EQUAL_UINT32(9, back.inputs().journalSeq);
  TEST_ASSERT_FALSE(back.inputs().sameSoft(expect));
  expectSameIndex(idx, back);
  for (int k = 0; k < 6; ++k) {
    LibraryIndex::Inputs other = in;
    switch (k) {
      case 0: other.cardId ^= 1; break;
      case 1: ++other.generation; break;
      case 2: other.commitId ^= 1; break;
      case 3: other.tagsCrc ^= 1; break;
      case 4: other.transfer = false; break;
      default: other.walkSignature = 5; break;
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Stale), load(bytes, other));
    TEST_ASSERT_FALSE(back.ready());
  }
  // Today's path signature alone is another hard input.
  {
    MemorySource src(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Stale), static_cast<int>(back.load(src, uint64_t{0})));
  }
  // Another rules version (the header's fourth word): Outdated.
  std::vector<uint8_t> rules = bytes;
  rules[12] = static_cast<uint8_t>(LibraryIndex::kRulesVersion + 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), load(rules, in));
  // Rules 3 (docs/I18N.md, phase 0: textfold's order and sameName()): an
  // index of rules 2 or 1, as the firmware before it saved, rebuilds once.
  TEST_ASSERT_EQUAL_UINT16(3, LibraryIndex::kRulesVersion);
  for (const uint8_t older : {uint8_t{1}, uint8_t{2}}) {
    rules[12] = older;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), load(rules, in));
  }
  // The build stamp: what a track id means. Two builds of the same files
  // agree; another file changes it.
  LibraryIndex again(Heap::alloc, Heap::release);
  buildSample(again);
  TEST_ASSERT_EQUAL_UINT64(idx.buildStamp(), again.buildStamp());
  LibraryIndex more(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(more.begin("/music"));
  for (const char* f : kFiles) more.addFile(f);
  more.addFile("/music/Zz/Extra/01 - more.mp3");
  TEST_ASSERT_TRUE(more.finish());
  TEST_ASSERT_TRUE(more.buildStamp() != idx.buildStamp());
}

// peek(): the boot reads a saved index's header alone (3.2.2, N10): its
// inputs whatever they are (no Stale here), Outdated for an older version
// or other rules, Corrupt for a short or foreign file; nothing is loaded.
// clearPending(): a track the scan read since the build stops being
// Pending, its other fields and the build stamp as they were.
void test_peek_and_clear_pending() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  buildSample(idx);
  LibraryIndex::Inputs in;
  in.cardId = 0x0102030405060708ull;
  in.generation = 3;
  in.commitId = 0xA0A0B0B0C0C0D0D0ull;
  in.tagsCrc = 0x12345678u;
  in.transfer = true;
  in.deviceCrc = 0xDEADBEEFu;
  in.journalSeq = 4;
  MemorySink file;
  TEST_ASSERT_TRUE(idx.save(file, in));
  std::vector<uint8_t> bytes(file.data(), file.data() + file.size());
  auto peek = [](const std::vector<uint8_t>& b, LibraryIndex::Inputs* out) {
    MemorySource src(b.data(), b.size());
    return static_cast<int>(LibraryIndex::peek(src, out));
  };
  LibraryIndex::Inputs got;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), peek(bytes, &got));
  TEST_ASSERT_TRUE(got.sameHard(in));
  TEST_ASSERT_TRUE(got.sameSoft(in));
  TEST_ASSERT_EQUAL_UINT64(0, got.walkSignature);
  // Only the header is read: a file cut right after it still peeks.
  std::vector<uint8_t> head(bytes.begin(), bytes.begin() + 22 * 4 + 2 * 4 * (LibraryIndex::kBuckets + 1));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), peek(head, &got));
  head.pop_back();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), peek(head, &got));
  std::vector<uint8_t> older = bytes;
  older[4] = 5;  // version 5
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), peek(older, &got));
  std::vector<uint8_t> rules = bytes;
  rules[12] = static_cast<uint8_t>(LibraryIndex::kRulesVersion + 1);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), peek(rules, &got));
  std::vector<uint8_t> foreign = bytes;
  foreign[0] ^= 1;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), peek(foreign, &got));
  std::vector<uint8_t> newer = bytes;
  newer[4] = 7;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), peek(newer, &got));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Corrupt), peek(std::vector<uint8_t>(), &got));

  // clearPending: built Pending, then read by the scan.
  LibraryIndex p(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(p.begin("/music"));
  TEST_ASSERT_TRUE(p.addFile("/music/A/B/01 - x.mp3", LibraryIndex::kAddPending) == LibraryIndex::Add::Added);
  TEST_ASSERT_TRUE(p.addFile("/music/A/B/02 - y.mp3", LibraryIndex::kAddPending) == LibraryIndex::Add::Added);
  TEST_ASSERT_TRUE(p.finish());
  const uint64_t stamp = p.buildStamp();
  const uint32_t t = p.findTrack("/music/A/B/02 - y.mp3");
  TEST_ASSERT_TRUE(t != LibraryIndex::kNone);
  const LibraryIndex::Track before = p.track(t);
  TEST_ASSERT_TRUE(before.flags & LibraryIndex::kTrackPending);
  p.clearPending(t);
  p.clearPending(LibraryIndex::kNone);  // out of range: nothing
  const LibraryIndex::Track after = p.track(t);
  TEST_ASSERT_FALSE(after.flags & LibraryIndex::kTrackPending);
  TEST_ASSERT_EQUAL_UINT8(before.flags & ~LibraryIndex::kTrackPending, after.flags);
  TEST_ASSERT_EQUAL_UINT32(before.title, after.title);
  TEST_ASSERT_EQUAL_UINT16(before.number, after.number);
  TEST_ASSERT_EQUAL_UINT64(stamp, p.buildStamp());
  TEST_ASSERT_TRUE(p.track(p.findTrack("/music/A/B/01 - x.mp3")).flags & LibraryIndex::kTrackPending);
}

// A folder's facts (D's DFLD: its best image, its counts) set its cover as
// the files would have; an owned image doesn't beat a transfer thumbnail, a
// hand-added one does (2.14.3).
void test_folder_facts_and_thumbnails() {
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music"));
  LibraryIndex::FolderFacts own;
  own.image = "cover.jpg";
  own.imageCount = 2;
  own.otherCount = 3;
  own.imageOwned = true;
  LibraryIndex::FolderFacts hand = own;
  hand.image = "Folder.JPG";
  hand.imageOwned = false;
  TEST_ASSERT_TRUE(idx.setFolderFacts("/music/A/One", own));
  idx.addRecord("/music/A/One/01 - x.mp3", View().title("x").v);
  TEST_ASSERT_TRUE(idx.setFolderFacts("/music/A/Two", hand));
  idx.addRecord("/music/A/Two/01 - x.mp3", View().title("x").v);
  idx.addRecord("/music/A/Three/CD1/01 - x.mp3", View().title("x").v);
  TEST_ASSERT_TRUE(idx.setFolderFacts("/music/A/Three/CD1", hand));
  idx.addRecord("/music/A/Four/01 - x.mp3", View().title("x").v);
  TEST_ASSERT_FALSE(idx.setFolderFacts("/elsewhere", own));
  const uint64_t thumbs[] = {cardcontract::fnv1a64Str("/music/A/One"), cardcontract::fnv1a64Str("/music/A/Two"),
                             cardcontract::fnv1a64Str("/music/A/Three"), cardcontract::fnv1a64Str("/music/A/Four")};
  std::vector<uint64_t> sorted(thumbs, thumbs + 4);
  std::sort(sorted.begin(), sorted.end());
  idx.setThumbFolders(sorted.data(), 4);
  TEST_ASSERT_TRUE(idx.finish());
  auto albumOf = [&](const char* p) { return idx.track(trackAt(idx, p)).album; };
  const uint32_t one = albumOf("/music/A/One/01 - x.mp3"), two = albumOf("/music/A/Two/01 - x.mp3");
  const uint32_t three = albumOf("/music/A/Three/CD1/01 - x.mp3"), four = albumOf("/music/A/Four/01 - x.mp3");
  char buf[128];
  idx.imagePath(idx.albumCover(one), buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("/music/A/One/cover.jpg", buf);
  TEST_ASSERT_EQUAL_UINT16(3, idx.folder(idx.album(one).folder).otherCount);
  TEST_ASSERT_EQUAL_UINT8(2, idx.folder(idx.album(one).folder).imageCount);
  TEST_ASSERT_EQUAL_UINT8(1, idx.folder(idx.album(two).folder).imageRank);
  TEST_ASSERT_TRUE(idx.album(one).flags & LibraryIndex::kTransferThumb);    // its image is the transfer's
  TEST_ASSERT_FALSE(idx.album(two).flags & LibraryIndex::kTransferThumb);   // the listener's folder.jpg wins
  TEST_ASSERT_FALSE(idx.album(three).flags & LibraryIndex::kTransferThumb); // so does one in its first disc
  TEST_ASSERT_TRUE(idx.album(four).flags & LibraryIndex::kTransferThumb);   // nothing beats it
}

// A heap that shrinks blocks in place (the firmware's heap_caps_realloc)
// trims with no copy: the same index, and a peak no higher.
namespace {
void* shrinkInPlace(void* p, size_t n) {
  for (auto& b : Heap::blocks()) {
    if (b.first == p) {
      Heap::live -= b.second - n;
      b.second = n;
      return p;
    }
  }
  return nullptr;
}
}  // namespace

void test_trims_in_place() {
  const synth::Spec spec = synth::specFor(5000);
  MemorySink a, b;
  size_t copyPeak, shrinkPeak;
  {
    Heap::reset();
    LibraryIndex idx(Heap::alloc, Heap::release);
    TEST_ASSERT_TRUE(idx.begin(spec.root, 6000));  // too many: the trims have work
    synth::addTracks(idx, spec);
    TEST_ASSERT_TRUE(idx.finish());
    copyPeak = idx.memory().buildPeak;
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    idx.save(a, 1);
  }
  {
    Heap::reset();
    LibraryIndex idx(Heap::alloc, Heap::release, shrinkInPlace);
    TEST_ASSERT_TRUE(idx.begin(spec.root, 6000));
    synth::addTracks(idx, spec);
    TEST_ASSERT_TRUE(idx.finish());
    shrinkPeak = idx.memory().buildPeak;
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    TEST_ASSERT_EQUAL_size_t(Heap::peak, shrinkPeak);
    idx.save(b, 1);
  }
  TEST_ASSERT_TRUE(shrinkPeak <= copyPeak);
  TEST_ASSERT_EQUAL_size_t(a.size(), b.size());
  TEST_ASSERT_EQUAL_MEMORY(a.data(), b.data(), a.size());
}

// The update step's rebuild (keepTrackBlock(), N10's review): clear()
// keeps the track table's block, and a build or a load that fits in it
// takes it again (no second block of its size: the heap's largest free
// block needn't hold the table), the index the same bytes; one that
// doesn't fit frees it first and asks at its size (not doubled); a smaller
// one is trimmed to its size; the destructor frees it.
void test_a_kept_track_block() {
  const synth::Spec spec = synth::specFor(3000);
  constexpr size_t kTrack = sizeof(LibraryIndex::Track);
  MemorySink plain;
  {
    LibraryIndex idx(Heap::alloc, Heap::release, shrinkInPlace);
    TEST_ASSERT_TRUE(idx.begin(spec.root, 3000));
    synth::addTracks(idx, spec);
    TEST_ASSERT_TRUE(idx.finish());
    TEST_ASSERT_TRUE(idx.save(plain, 1));
  }
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);
  Heap::reset();
  {
    LibraryIndex idx(Heap::alloc, Heap::release, shrinkInPlace);
    idx.keepTrackBlock(true);
    auto build = [&](const synth::Spec& sp, uint32_t n) {
      TEST_ASSERT_TRUE(idx.begin(sp.root, n));
      synth::addTracks(idx, sp);
      TEST_ASSERT_TRUE(idx.finish());
    };
    auto asked = [](size_t bytes) { return std::count(Heap::sizes().begin(), Heap::sizes().end(), bytes); };
    build(spec, 3000);
    const size_t table = 3000 * kTrack;
    TEST_ASSERT_EQUAL_size_t(table, idx.memory().tracks);
    idx.clear();
    TEST_ASSERT_FALSE(idx.ready());
    TEST_ASSERT_EQUAL_UINT32(0, idx.trackCount());
    TEST_ASSERT_EQUAL_size_t(table, Heap::live);  // the kept block alone
    TEST_ASSERT_EQUAL_size_t(table, idx.memory().tracks);
    // The same library again: the block taken, none asked of its size.
    Heap::sizes().clear();
    build(spec, 3000);
    TEST_ASSERT_EQUAL(0, asked(table));
    MemorySink again;
    TEST_ASSERT_TRUE(idx.save(again, 1));
    TEST_ASSERT_EQUAL_size_t(plain.size(), again.size());
    TEST_ASSERT_EQUAL_MEMORY(plain.data(), again.data(), plain.size());
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    // A load of the same size takes it too.
    Heap::sizes().clear();
    MemorySource in(plain.data(), plain.size(), 4096);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(idx.load(in, 1)));
    TEST_ASSERT_EQUAL(0, asked(table));
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    TEST_ASSERT_EQUAL_UINT32(3000, idx.trackCount());
    // A bigger library: the kept block freed first, the new one asked at
    // its size (not twice the old).
    const synth::Spec bigger = synth::specFor(4000);
    idx.clear();
    Heap::sizes().clear();
    build(bigger, 4000);
    TEST_ASSERT_EQUAL(1, asked(4000 * kTrack));
    TEST_ASSERT_EQUAL(0, asked(6000 * kTrack));
    TEST_ASSERT_EQUAL_UINT32(4000, idx.trackCount());
    // A smaller one: taken, trimmed to its size by finish().
    const synth::Spec smaller = synth::specFor(2000);
    idx.clear();
    Heap::sizes().clear();
    build(smaller, 2000);
    TEST_ASSERT_EQUAL(0, asked(2000 * kTrack));
    TEST_ASSERT_EQUAL_size_t(2000 * kTrack, idx.memory().tracks);
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    // A load bigger than the kept block: freed first, then exactly the file's.
    idx.clear();
    Heap::sizes().clear();
    MemorySource in2(plain.data(), plain.size(), 4096);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Loaded), static_cast<int>(idx.load(in2, 1)));
    TEST_ASSERT_EQUAL(1, asked(table));
    TEST_ASSERT_EQUAL_size_t(Heap::live, idx.memory().total);
    // A failed load keeps the block for the next try, and nothing else.
    idx.clear();
    MemorySource cut(plain.data(), plain.size() / 2, 4096);
    TEST_ASSERT_TRUE(idx.load(cut, 1) != LibraryIndex::Load::Loaded);
    TEST_ASSERT_EQUAL_size_t(idx.memory().tracks, Heap::live);
  }
  TEST_ASSERT_EQUAL_size_t(0, Heap::live);  // the destructor frees the kept block
  TEST_ASSERT_EQUAL_size_t(Heap::allocs, Heap::frees);
}

// The tagged synthetic library: deterministic, its rates as the constants
// say, its folders unique (one album and one artist each).
void test_tagged_synthetic_library() {
  const synth::Spec spec = synth::userShape(5000);
  TEST_ASSERT_EQUAL_UINT32(180, spec.artists);
  TEST_ASSERT_EQUAL_UINT32(460, spec.albums);
  synth::Tagged t, u;
  uint32_t sameTitle = 0, withRecord = 0, albumArtist = 0, years = 0, discs = 0;
  LibraryIndex idx(Heap::alloc, Heap::release);
  TEST_ASSERT_TRUE(idx.begin("/music", spec.tracks));
  for (uint32_t i = 0; i < spec.tracks; ++i) {
    TEST_ASSERT_TRUE(synth::tagged(spec, i, &t));
    TEST_ASSERT_TRUE(synth::tagged(spec, i, &u));
    TEST_ASSERT_EQUAL_STRING(t.path, u.path);
    TEST_ASSERT_EQUAL_STRING(t.title, u.title);
    TEST_ASSERT_EQUAL_STRING(t.artist, u.artist);
    TEST_ASSERT_EQUAL_STRING(t.album, u.album);
    TEST_ASSERT_EQUAL_UINT32(t.fatTime, u.fatTime);
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(idx.addFile(t.path)));
    if (t.noTags) continue;
    ++withRecord;
    const char* leaf = std::strrchr(t.path, '/') + 1;
    const char* dot = std::strrchr(leaf, '.');
    const std::string stem(leaf, static_cast<size_t>(dot - leaf));
    sameTitle += stem == t.title || (stem.size() > 5 && stem.substr(5) == t.title);
    albumArtist += t.albumArtist[0] != 0;
    years += t.year != 0;
    discs += t.disc != 0;
  }
  TEST_ASSERT_TRUE(idx.finish());
  TEST_ASSERT_EQUAL_UINT32(spec.tracks, idx.trackCount());
  TEST_ASSERT_EQUAL_UINT32(spec.albums, idx.albumCount());
  TEST_ASSERT_EQUAL_UINT32(spec.artists, idx.artistCount());
  auto near = [&](uint32_t got, uint32_t pctWant) {
    const double p = 100.0 * got / withRecord;
    return p > pctWant - 6.0 && p < pctWant + 6.0;
  };
  TEST_ASSERT_TRUE(near(sameTitle, synth::kTitleSamePct));
  TEST_ASSERT_TRUE(near(albumArtist, synth::kAlbumArtistPct));
  TEST_ASSERT_TRUE(near(years, synth::kYearPct));
  TEST_ASSERT_TRUE(near(discs, synth::kDiscPct));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_counts_and_skips);
  RUN_TEST(test_titles_numbers_formats);
  RUN_TEST(test_articles_sort_past_the);
  RUN_TEST(test_disc_track_names_order_an_album);
  RUN_TEST(test_titles_lose_the_folders_artist);
  RUN_TEST(test_prefixes_that_differ_keep_their_order);
  RUN_TEST(test_paths_round_trip);
  RUN_TEST(test_artist_and_album_views);
  RUN_TEST(test_folder_tree);
  RUN_TEST(test_other_files_and_covers);
  RUN_TEST(test_image_rank);
  RUN_TEST(test_tree_tracks);
  RUN_TEST(test_buckets);
  RUN_TEST(test_memory_comes_from_the_hooks_and_goes_back);
  RUN_TEST(test_out_of_memory_is_clean);
  RUN_TEST(test_synthetic_10k);
  RUN_TEST(test_multi_disc_album);
  RUN_TEST(test_rebuild_and_expect_hint);
  RUN_TEST(test_find_track_by_path);
  RUN_TEST(test_find_track_in_a_big_library);
  RUN_TEST(test_save_and_load_round_trip);
  RUN_TEST(test_save_and_load_a_big_library);
  RUN_TEST(test_load_rejects_stale_and_damaged_files);
  RUN_TEST(test_opus_files_are_tracks);
  RUN_TEST(test_an_empty_library_round_trips);
  RUN_TEST(test_opus_names_are_read_like_the_others);
  RUN_TEST(test_an_older_cache_version_is_rebuilt);
  RUN_TEST(test_records_name_an_album);
  RUN_TEST(test_album_votes);
  RUN_TEST(test_album_votes_on_its_first_512_tracks);
  RUN_TEST(test_artist_display_names);
  RUN_TEST(test_stage_a_orders);
  RUN_TEST(test_sort_keys_are_kept_and_blank_ones_ignored);
  RUN_TEST(test_other_scripts_take_their_sort_tags);
  RUN_TEST(test_library_roots);
  RUN_TEST(test_inputs_of_the_cache);
  RUN_TEST(test_peek_and_clear_pending);
  RUN_TEST(test_folder_facts_and_thumbnails);
  RUN_TEST(test_trims_in_place);
  RUN_TEST(test_a_kept_track_block);
  RUN_TEST(test_tagged_synthetic_library);
  return UNITY_END();
}
