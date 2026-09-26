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
    "/music/Daft Punk/Discovery/cover.jpg",  // not audio: skipped
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
  uint32_t added = 0, skipped = 0;
  for (const char* f : kFiles) {
    const auto r = idx.addFile(f);
    if (r == LibraryIndex::Add::Added) ++added;
    if (r == LibraryIndex::Add::Skipped) ++skipped;
  }
  TEST_ASSERT_EQUAL_UINT32(kAudio, added);
  TEST_ASSERT_EQUAL_UINT32(3, skipped);
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_counts_and_skips);
  RUN_TEST(test_titles_numbers_formats);
  RUN_TEST(test_paths_round_trip);
  RUN_TEST(test_artist_and_album_views);
  RUN_TEST(test_folder_tree);
  RUN_TEST(test_buckets);
  RUN_TEST(test_memory_comes_from_the_hooks_and_goes_back);
  RUN_TEST(test_out_of_memory_is_clean);
  RUN_TEST(test_synthetic_10k);
  RUN_TEST(test_multi_disc_album);
  RUN_TEST(test_rebuild_and_expect_hint);
  return UNITY_END();
}
