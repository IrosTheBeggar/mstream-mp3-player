#pragma once
#include <cstddef>
#include <cstdint>

// The music library as one compact index: the future single store the
// browsing UI and the queue read from. Portable, host-tested.
//
// Everything lives in a few flat blocks from an allocator hook (the firmware
// points it at PSRAM): one string arena (every name once, NUL-terminated),
// fixed-size records for tracks, artists, albums and folders, and sorted
// views as arrays of 32-bit ids. There is no allocation per string or per
// record: blocks grow by doubling while building and are trimmed to size by
// finish(). Queries never allocate.
//
// Built from file paths, the way the SD card is laid out today:
//   <root>/Artist/Album/NN - Title.ext   (deeper folders stay in that album)
//   <root>/Artist/NN - Title.ext         (album "", the artist's loose tracks)
//   <root>/NN - Title.ext                (artist "", album "")
// The artist and album names are the folder names (their strings are the
// folders' own, not copies). The track number and title come from the file
// name: leading digits, then separators (" - ", ". ", "_"), then the title
// up to the extension. Only .mp3 and .flac are indexed.
//
// Views (all sorted with textfold::compare: case- and accent-insensitive,
// symbols and digits before letters):
//   artistsAZ()           every artist
//   albumsAZ()            every album (ties: by artist)
//   albumsOf(artist)      an artist's albums, A-Z
//   tracksOfAlbum(album)  an album's tracks folder by folder (its own files,
//                         then disc subfolders A-Z), each by number, then name
//   tracksOfArtist(artist) the artist's albums' tracks, album after album
//   subfolders(folder)    a folder's folders, A-Z
//   filesIn(folder)       a folder's audio files, A-Z
// plus the A-Z rail's buckets for the two long lists ('#', A..Z).
//
// Not thread-safe: build and read on one task (the firmware's loop task).
class LibraryIndex {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  static constexpr int kBuckets = 27;  // '#', 'A'..'Z'

  enum class Format : uint8_t { Unknown = 0, Mp3, Flac };
  enum class Add : uint8_t { Added, Skipped, NoMemory };
  enum class View : uint8_t { Artists, Albums };

  // String offsets are into the arena (str()); ids index the record tables.
  struct Track {             // 24 bytes
    uint32_t name;           // file name, "06 - Title.mp3"
    uint32_t title;          // the title: `titleLen` bytes at this offset (inside the name)
    uint32_t folder;
    uint32_t album;
    uint32_t artist;
    uint8_t titleLen;
    uint8_t number;          // 0: none
    Format format;
    uint8_t reserved;
  };
  struct Artist {            // 20 bytes; first*: positions in albumsOf/tracksOf views
    uint32_t name;
    uint32_t firstAlbum, albumCount;
    uint32_t firstTrack, trackCount;
  };
  struct Album {             // 20 bytes
    uint32_t name;
    uint32_t artist;
    uint32_t folder;         // the album's folder (depth 2), or the track's folder
    uint32_t firstTrack, trackCount;
  };
  struct Folder {            // 24 bytes; folder 0 is the root
    uint32_t name;           // the root's is its whole path, "/music"
    uint32_t parent;         // kNone for the root
    uint32_t firstFolder, folderCount;
    uint32_t firstFile, fileCount;
  };

  // A run of ids inside a view.
  struct Span {
    const uint32_t* ids = nullptr;
    uint32_t count = 0;
    uint32_t operator[](uint32_t i) const { return ids[i]; }
  };

  // Where the bytes are (for the measurements). buildPeak: the most the
  // build held at once, every block from the hooks counted while it is held:
  // hash tables, growth slack, a block and its replacement while one is
  // copied into the other, the sort's temporaries.
  struct Memory {
    size_t strings = 0, tracks = 0, artists = 0, albums = 0, folders = 0, views = 0;
    size_t total = 0;
    size_t buildPeak = 0;
  };

  // nullptr hooks: malloc/free.
  explicit LibraryIndex(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~LibraryIndex();
  LibraryIndex(const LibraryIndex&) = delete;
  LibraryIndex& operator=(const LibraryIndex&) = delete;

  // ---- building ----
  // Drops what's there and starts a build of the files under `root`.
  // `expectTracks` (0: unknown) sizes the blocks up front.
  bool begin(const char* root = "/music", uint32_t expectTracks = 0);
  // One file, by its full path.
  Add addFile(const char* path);
  // Sorts, fills the views and trims the blocks. False: out of memory (the
  // index is then empty).
  bool finish();
  void clear();
  bool ready() const { return ready_; }
  bool failed() const { return failed_; }

  // ---- reading (after finish()) ----
  uint32_t trackCount() const { return trackN_; }
  uint32_t artistCount() const { return artistN_; }
  uint32_t albumCount() const { return albumN_; }
  uint32_t folderCount() const { return folderN_; }

  const Track& track(uint32_t id) const { return tracks_[id]; }
  const Artist& artist(uint32_t id) const { return artists_[id]; }
  const Album& album(uint32_t id) const { return albums_[id]; }
  const Folder& folder(uint32_t id) const { return folders_[id]; }
  const char* str(uint32_t offset) const { return arena_ + offset; }
  const char* artistName(uint32_t id) const { return str(artists_[id].name); }
  const char* albumName(uint32_t id) const { return str(albums_[id].name); }
  const char* folderName(uint32_t id) const { return str(folders_[id].name); }
  const char* trackFileName(uint32_t id) const { return str(tracks_[id].name); }
  // The title is not NUL-terminated (it sits inside the file name).
  const char* trackTitle(uint32_t id, uint8_t* len) const {
    *len = tracks_[id].titleLen;
    return str(tracks_[id].title);
  }

  // "<root>/Artist/Album/06 - Title.mp3" into buf. Returns the length, or 0
  // if it didn't fit (buf then holds "").
  size_t trackPath(uint32_t id, char* buf, size_t size) const;
  size_t folderPath(uint32_t id, char* buf, size_t size) const;

  Span artistsAZ() const { return {artistsAZ_, artistN_}; }
  Span albumsAZ() const { return {albumsAZ_, albumN_}; }
  Span albumsOf(uint32_t artist) const {
    return {albumsByArtist_ + artists_[artist].firstAlbum, artists_[artist].albumCount};
  }
  Span tracksOfAlbum(uint32_t album) const {
    return {tracksByAlbum_ + albums_[album].firstTrack, albums_[album].trackCount};
  }
  Span tracksOfArtist(uint32_t artist) const {
    return {tracksByAlbum_ + artists_[artist].firstTrack, artists_[artist].trackCount};
  }
  // Every track, artist after artist, album after album (the tracks view).
  Span allTracks() const { return {tracksByAlbum_, trackN_}; }
  Span subfolders(uint32_t folder) const {
    return {folderChildren_ + folders_[folder].firstFolder, folders_[folder].folderCount};
  }
  Span filesIn(uint32_t folder) const {
    return {folderFiles_ + folders_[folder].firstFile, folders_[folder].fileCount};
  }
  static constexpr uint32_t rootFolder() { return 0; }

  // A-Z rail: the position in artistsAZ()/albumsAZ() of the first entry in
  // bucket b (0 '#', 1..26 A..Z); bucketStart(v, kBuckets) is the count. A
  // bucket without entries starts where the next one does.
  uint32_t bucketStart(View view, int bucket) const;
  // The bucket of the entry at `position` in that view.
  int bucketAt(View view, uint32_t position) const;

  Memory memory() const;

private:
  template <typename T>
  struct Block {  // a growable array from the hooks
    T* data = nullptr;
    uint32_t size = 0;
    uint32_t cap = 0;
  };
  struct Table {  // open addressing: id + 1 per slot (0 empty), with the hash
    uint32_t* ids = nullptr;
    uint32_t* hashes = nullptr;
    uint32_t cap = 0;
    uint32_t size = 0;
  };

  void* alloc(size_t bytes);
  void release(void* p, size_t bytes);
  template <typename T>
  bool reserve(Block<T>& b, uint32_t n);
  template <typename T>
  bool push(Block<T>& b, const T& v);
  template <typename T>
  void trim(Block<T>& b);
  template <typename T>
  void drop(Block<T>& b);
  template <typename T>
  size_t bytes(const Block<T>& b) const { return static_cast<size_t>(b.cap) * sizeof(T); }
  bool tableInit(Table& t, uint32_t cap);
  void tableDrop(Table& t);
  template <typename Eq>
  uint32_t tableFind(const Table& t, uint32_t hash, Eq eq) const;
  bool tableInsert(Table& t, uint32_t hash, uint32_t id);
  uint32_t intern(const char* s, size_t len);  // kNone: no memory
  uint32_t folderChild(uint32_t parent, const char* name, size_t len);
  uint32_t artistFor(uint32_t nameOffset);
  uint32_t albumFor(uint32_t artist, uint32_t nameOffset, uint32_t folder);
  bool buildViews();
  void resetViews();

  AllocFn allocFn_;
  FreeFn freeFn_;

  // Build state.
  Block<char> arenaB_;
  Block<Track> tracksB_;
  Block<Artist> artistsB_;
  Block<Album> albumsB_;
  Block<Folder> foldersB_;
  Table folderTable_, artistTable_, albumTable_;
  uint32_t rootLen_ = 0;
  uint32_t emptyName_ = kNone;  // "" in the arena
  bool building_ = false;
  bool failed_ = false;
  bool ready_ = false;
  size_t held_ = 0;       // bytes from the hooks now
  size_t buildPeak_ = 0;  // the most held_ since clear()

  // Views (after finish()), in blocks from the hooks.
  uint32_t* viewsBlock_ = nullptr;  // every view in one allocation
  size_t viewsBytes_ = 0;
  uint32_t* artistsAZ_ = nullptr;
  uint32_t* albumsAZ_ = nullptr;
  uint32_t* albumsByArtist_ = nullptr;
  uint32_t* tracksByAlbum_ = nullptr;
  uint32_t* folderChildren_ = nullptr;
  uint32_t* folderFiles_ = nullptr;
  uint32_t artistBuckets_[kBuckets + 1] = {};
  uint32_t albumBuckets_[kBuckets + 1] = {};

  // Read side: pointers into the blocks, fixed after finish().
  const char* arena_ = nullptr;
  const Track* tracks_ = nullptr;
  const Artist* artists_ = nullptr;
  const Album* albums_ = nullptr;
  const Folder* folders_ = nullptr;
  uint32_t trackN_ = 0, artistN_ = 0, albumN_ = 0, folderN_ = 0;
};
