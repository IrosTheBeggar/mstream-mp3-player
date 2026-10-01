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
    TEST_ASSERT_TRUE(textfold::compare(idx.albumName(z[i - 1]), idx.albumName(z[i])) <= 0);
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
    if (i) TEST_ASSERT_TRUE(textfold::compare(idx.artistName(artists[i - 1]), idx.artistName(artists[i])) < 0);
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_counts_and_skips);
  RUN_TEST(test_titles_numbers_formats);
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
  RUN_TEST(test_an_empty_library_round_trips);
  return UNITY_END();
}
