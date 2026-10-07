// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for LibraryIndex (the compact library store) and the synthetic
// library generator. Run: pio test -e native
#include <unity.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "ByteStream.h"
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
  static void reset() {
    live = peak = allocs = frees = 0;
    limit = SIZE_MAX;
    blocks().clear();
  }
  static void* alloc(size_t n) {
    if (live + n > limit) return nullptr;
    void* p = std::malloc(n ? n : 1);
    blocks().push_back({p, n});
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
    TEST_ASSERT_EQUAL_UINT32(a.albumsOf(i).count, b.albumsOf(i).count);
    TEST_ASSERT_EQUAL_UINT32(a.tracksOfArtist(i).count, b.tracksOfArtist(i).count);
  }
  for (uint32_t i = 0; i < a.albumCount(); ++i) {
    TEST_ASSERT_EQUAL_UINT32(a.albumsAZ()[i], b.albumsAZ()[i]);
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
  TEST_ASSERT_EQUAL_UINT32(5, version);
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
  for (const uint32_t old : {2u, 3u, 4u}) {
    std::memcpy(bytes.data() + 4, &old, 4);
    MemorySource in(bytes.data(), bytes.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LibraryIndex::Load::Outdated), static_cast<int>(back.load(in, 9)));
    TEST_ASSERT_FALSE(back.ready());
  }
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
  return UNITY_END();
}
