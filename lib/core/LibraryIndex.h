// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ByteStream.h"

// The music library as one compact index: the single store the browsing UI,
// the queue and the player read from (they hold its track ids). Portable,
// host-tested.
//
// Everything lives in a few flat blocks from an allocator hook (the firmware
// points it at PSRAM): the strings (every name once, NUL-terminated, in
// chunks of 64 KB, so the strings never need an estimate, a doubling or a
// copy), fixed-size records for tracks, artists, albums and folders, and
// sorted views as arrays of 32-bit ids. There is no allocation per string or
// per record: the record tables are sized from the inputs' counts (or grow
// by doubling when they aren't known) and are trimmed to size by finish(),
// in place when the allocator can shrink a block. Queries never allocate.
//
// Built from file paths, the way the SD card is laid out today:
//   <root>/Artist/Album/NN - Title.ext   (deeper folders stay in that album)
//   <root>/Artist/NN - Title.ext         (the artist's loose tracks: album "")
//   <root>/NN - Title.ext                (artist "", album "")
// where the root is /music, or the longest library root (the transfer's
// LIBR, docs/METADATA.md 2.8.6) that contains the file. Albums are keyed by
// their folder, artists by their folder's name. The disc, the track number
// and the title come from the file name, read with the other names in its
// folder when the build finishes (trackname::Folder): "06 - Title", "1-01
// Title" (disc 1), "101 Title" (disc 1), "Artist - 03 - Title", and a
// title's leading "Artist - " dropped when it is the folder's artist. The
// title is then a slice of the file name, up to the extension. Only .mp3,
// .flac and .opus are tracks (.ogg and .oga aren't: an Ogg file of another
// codec would only fail at its open, docs/OPUS.md), and their names are read
// alike, whatever the extension. Every other file is counted in its folder
// (the Folders view hides them but says how many: "14 audio files, 1
// other"), and the folder's cover image is picked from them by name:
// cover.jpg, then folder.jpg, then front.jpg, then any other .jpg
// (imageRank()). A folder with no audio anywhere under it (artwork alone,
// say) is left out of the folder views.
//
// A file can come with its tag record (addRecord(): the transfer's or the
// device's, chosen per file by LibraryBuilder, docs/METADATA.md 2.9). Then
// Stage A's rules (5.4) name it: the record's title, number, disc and length,
// each field the record lacks from the path as above; its artists' display
// join as the track's artist. Each album (still its folder) elects, over its
// tracks' records, its name (the most common album value; ties to the
// smallest by bytes; none: the folder's name), its year (the most common;
// ties to the earliest), its artist line (the most common album-artist
// display, else "Various Artists" when a track says compilation, else the
// most common track-artist display, else the artist folder) and its discs
// (the highest disc number). Each artist (still its folder) shows the most
// common album-artist display (else track-artist display) of its tracks
// when textfold::sameName() matches it to the folder's name, else the
// folder's name. The votes count as the records arrive, over each album's
// and each artist's run of tracks (the builder adds files in the card's
// canonical order, so a folder's whole tree is one run), in fixed scratch: an
// album votes on its first 512 tracks. A file without a record indexes
// exactly as before, and an index with no records is today's to the byte
// (apart from the version and the record sizes).
//
// Views (all sorted with textfold::compare: case- and accent-insensitive,
// symbols and digits before letters; the artists and albums by their
// textfold::sortName() of their sort tag when they have one, else of their
// name, so "The Lantern Choir" sorts under L):
//   artistsAZ()           every artist
//   albumsAZ()            every album (ties: by artist)
//   albumsOf(artist)      an artist's albums, newest first (by year; no year
//                         last, then A-Z: an index without years is A-Z)
//   tracksOfAlbum(album)  an album's tracks. With records: by disc (none is
//                         1), then folder by folder (its own files, then disc
//                         subfolders A-Z), then number (none last, as
//                         mStream's track order), then name. Without: folder
//                         by folder, then disc, number (none first), name
//   tracksOfArtist(artist) the artist's albums' tracks, album after album
//   subfolders(folder)    a folder's folders with audio under them, A-Z
//   filesIn(folder)       a folder's audio files, A-Z
//   treeTracks(folder)    every audio file under a folder: its own files A-Z,
//                         then each subfolder's tree, A-Z (depth first)
// plus the A-Z rail's buckets for the two long lists ('#', A..Z), by the
// sort keys' first letters.
//
// save() writes the finished index as one file (library.idx v6: its blocks
// as they are, a header with what it was built from, a checksum) and load()
// reads it back into blocks of exactly that size, with no sorting and no
// build peak: the firmware's cache on the card. findTrack() turns a path
// back into an id (the queue is saved as paths, so it survives a rebuild
// that renumbers the tracks).
//
// Not thread-safe: build and read on one task.
class LibraryIndex {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  // Shrinks a block to `bytes` (on the firmware heap_caps_realloc to a
  // smaller size, which splits the block in place): the block, moved or
  // not, or nullptr to keep it as it was. With no hook a trim copies the
  // block, and the copy counts in the build peak.
  using ShrinkFn = void* (*)(void* p, size_t bytes);

  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  static constexpr int kBuckets = 27;  // '#', 'A'..'Z'
  // Stage A's rules (docs/METADATA.md 5.4): a saved index of other rules
  // is Outdated, so a change of the votes or the orders rebuilds it.
  static constexpr uint16_t kRulesVersion = 1;
  // The strings' chunks: an offset is its chunk's number, then its place.
  static constexpr uint32_t kChunkBits = 16;
  static constexpr uint32_t kChunkBytes = 1u << kChunkBits;
  static constexpr uint32_t kMaxChunks = 128;  // 8 MB of strings
  // An album's vote counts its first this many tracks (5.4).
  static constexpr uint32_t kVoteTracks = 512;
  // docs/METADATA.md 7.1, U9 (b) (open; the proposed default): a cover image
  // the listener added (one the transfer's ledger doesn't list) in the
  // album's folder or its first track's beats the transfer's thumbnail
  // (2.14.3). false: the thumbnail wins whenever T has one.
  static constexpr bool kHandCoverBeatsThumbnail = true;

  enum class Format : uint8_t { Unknown = 0, Mp3, Flac, Opus };
  // Added: a track. Other: a file that isn't audio, counted in its folder
  // (and a candidate for its cover). Skipped: outside the root, or no name.
  enum class Add : uint8_t { Added, Skipped, NoMemory, Other };
  // imageRank(): how good a cover a file name makes, 0 best; kNoImage: none.
  static constexpr uint8_t kNoImage = 0xFF;
  enum class View : uint8_t { Artists, Albums };
  // load(): Stale means a good file built from other hard inputs (the card
  // or the transfer changed: a rebuild follows); Outdated one saved by an
  // earlier version of the index or other rules (versions 1-5 among them:
  // a record's meaning changed), so a rebuild follows; Corrupt a short,
  // damaged or foreign one (a newer version too).
  enum class Load : uint8_t { Loaded, Stale, Corrupt, NoMemory, Outdated };

  // Track::flags.
  static constexpr uint8_t kSourceMask = 0x03;  // where its names come from:
  static constexpr uint8_t kFromPath = 0;       //   the path alone
  static constexpr uint8_t kFromDevice = 1;     //   the device's record (D)
  static constexpr uint8_t kFromTransfer = 2;   //   the transfer's record (T)
  static constexpr uint8_t kTrackJpeg = 0x04;   // its record locates an embedded JPEG (2.14.3, step 3)
  static constexpr uint8_t kTrackCompilation = 0x08;  // its record says compilation
  static constexpr uint8_t kTrackPending = 0x10;      // no record: the scan should read it
  static constexpr uint8_t kTagTitle = 0x20;    // the title is the record's
  static constexpr uint8_t kTagNumber = 0x40;   // the number is the record's
  static constexpr uint8_t kTagDisc = 0x80;     // the disc is the record's
  // Album::flags.
  static constexpr uint8_t kLoose = 0x01;          // an artist folder's (or a root's) own tracks: named ""
  // /.mstream/thumbs has its cover, and no hand-added image beats it (2.14.3).
  static constexpr uint8_t kTransferThumb = 0x02;
  static constexpr uint8_t kTagged = 0x04;         // a track has a record: its names and order are Stage A's
  // addFile() options.
  static constexpr uint8_t kAddPending = 0x01;  // a track the scan should read (kTrackPending)
  // A file the transfer's ledger lists: its image doesn't beat a transfer
  // thumbnail.
  static constexpr uint8_t kAddOwned = 0x02;

  // String offsets are into the strings (str()); ids index the record tables.
  struct Track {             // 32 bytes
    uint32_t name;           // file name, "06 - Title.mp3"
    uint32_t title;          // the title: `titleLen` bytes at this offset (inside the name, or a string of its own)
    uint32_t folder;
    uint32_t album;
    uint32_t artist;         // the folder artist (Go to, notePlaying)
    uint32_t trackArtist;    // the record's artist display; kNone: none, or the album's artistLine
    uint8_t titleLen;
    Format format;
    uint8_t disc;            // the record's, else the file name's ("2-03 Title"); 0: none
    uint8_t flags;           // kSourceMask, kTrack*, kTag*
    uint16_t number;         // 0: none
    uint16_t durationS;      // the record's length, rounded; 0: unknown
  };
  struct Artist {            // 20 bytes; first*: positions in albumsOf/tracksOf views
    uint32_t name;           // the display name (the folder's, or its tracks' elected spelling)
    uint32_t firstAlbum, albumCount;
    uint32_t firstTrack, trackCount;
  };
  struct Album {             // 28 bytes
    uint32_t name;           // the display name ("" for kLoose)
    uint32_t artist;
    uint32_t folder;         // the album's folder (depth 2 below its root), or the track's folder
    uint32_t firstTrack, trackCount;
    uint32_t artistLine;     // the elected artist line (the artist's name when nothing says otherwise)
    uint16_t year;           // the elected year; 0: none
    uint8_t discs;           // the highest disc number (at least 1)
    uint8_t flags;           // kLoose, kTransferThumb, kTagged
  };
  struct Folder {            // 36 bytes; folder 0 is the root
    uint32_t name;           // the root's is its whole path, "/music"
    uint32_t parent;         // kNone for the root
    uint32_t firstFolder, folderCount;  // in subfolders(): those with audio under them
    uint32_t firstFile, fileCount;      // its own audio files; firstFile also starts its tree's
    uint32_t treeCount;      // audio files in it and in every folder under it
    uint32_t image;          // its cover image's file name (string offset), kNone: none
    uint16_t otherCount;     // its files that aren't audio (saturating)
    uint8_t imageRank;       // imageRank() of `image`; kNoImage: none
    uint8_t imageCount;      // its .jpg/.jpeg files (saturating)
  };

  // A run of ids inside a view.
  struct Span {
    const uint32_t* ids = nullptr;
    uint32_t count = 0;
    uint32_t operator[](uint32_t i) const { return ids[i]; }
  };

  // Where the bytes are (for the measurements). buildPeak: the most the
  // build held at once, every block from the hooks counted while it is held:
  // hash tables, the votes' scratch, growth slack, a block and its
  // replacement while one is copied into the other, the sort's temporaries.
  struct Memory {
    size_t strings = 0, tracks = 0, artists = 0, albums = 0, folders = 0, views = 0;
    size_t total = 0;
    size_t buildPeak = 0;
  };

  // The blocks' sizes up front, from the inputs' headers (3.4.2: begin()
  // from the counts, no doubling). Upper bounds are fine: finish() trims. 0:
  // unknown (estimated from `tracks`). A count that turns out too small
  // costs a doubling, never correctness.
  struct Sizing {
    uint32_t tracks = 0;
    uint32_t folders = 0;
    uint32_t albums = 0;
    uint32_t artists = 0;
    uint32_t firstChunk = 0;  // the first chunk of strings (at most kChunkBytes; it grows to it)
  };

  // What the index was built from (3.4.3), saved in its header. The hard
  // inputs differ: load() says Stale and the caller builds again; the soft
  // ones differ (the scan went on): the index is used, and rebuilt at the
  // scan's end (the caller compares inputs()).
  struct Inputs {
    // Hard.
    uint64_t walkSignature = 0;  // today's path walk (0 when the boot doesn't walk)
    uint64_t cardId = 0;         // the transfer's identity (2.5.2, 2.10.1); zeros: no transfer data
    uint32_t generation = 0;
    uint64_t commitId = 0;
    uint32_t tagsCrc = 0;        // T's headerCrc
    bool transfer = false;       // T was read whole and used
    // Soft.
    uint32_t deviceCrc = 0;      // D's headerCrc
    uint32_t journalSeq = 0;     // the last tags.jnl chunk merged
    bool sameHard(const Inputs& o) const {
      return walkSignature == o.walkSignature && cardId == o.cardId && generation == o.generation &&
             commitId == o.commitId && tagsCrc == o.tagsCrc && transfer == o.transfer;
    }
    bool sameSoft(const Inputs& o) const { return deviceCrc == o.deviceCrc && journalSeq == o.journalSeq; }
  };

  // One file's chosen record, as addRecord() reads it: the fields its
  // producer looked for (`known`, 2.6.4) and found; null or empty is absent,
  // and an absent field comes from the path. Lists are their values
  // separated by U+001F (2.3.6).
  struct TagView {
    uint8_t source = kFromDevice;  // kFromDevice or kFromTransfer
    const char* title = nullptr;
    size_t titleLen = 0;
    const char* artist = nullptr;  // a list
    size_t artistLen = 0;
    const char* album = nullptr;
    size_t albumLen = 0;
    const char* albumArtist = nullptr;  // a list
    size_t albumArtistLen = 0;
    const char* artistSort = nullptr;
    size_t artistSortLen = 0;
    const char* albumSort = nullptr;
    size_t albumSortLen = 0;
    const char* albumArtistSort = nullptr;
    size_t albumArtistSortLen = 0;
    uint16_t year = 0;   // 0: absent
    uint16_t track = 0;
    uint16_t disc = 0;
    uint32_t durationMs = 0;
    uint8_t compilation = 0;  // 0 not said, 1 yes, 2 said no
    bool jpeg = false;        // an embedded JPEG picture is located
  };

  // A folder's files that aren't audio, as the walk saw them (D's DFLD;
  // the builder's facts): its best image by imageRank(), how many images
  // and other files it has, and whether the transfer's ledger lists that
  // image (a hand-added cover beats a transfer thumbnail, 2.14.3).
  struct FolderFacts {
    const char* image = nullptr;  // the best image's file name; nullptr: none
    uint8_t imageCount = 0;
    uint16_t otherCount = 0;
    bool imageOwned = false;
  };

  // nullptr hooks: malloc/free, and trims by a copy.
  explicit LibraryIndex(AllocFn alloc = nullptr, FreeFn release = nullptr, ShrinkFn shrink = nullptr);
  ~LibraryIndex();
  LibraryIndex(const LibraryIndex&) = delete;
  LibraryIndex& operator=(const LibraryIndex&) = delete;

  // ---- building ----
  // Drops what's there and starts a build of the files under `root`.
  // `expectTracks` (0: unknown) sizes the blocks up front (the other counts
  // estimated from it).
  bool begin(const char* root = "/music", uint32_t expectTracks = 0);
  // The same with the counts, and the library roots (LIBR: folders
  // relative to `root`, any order; their strings outlive finish()).
  bool begin(const Sizing& sizing, const char* root = "/music", const char* const* libraryRoots = nullptr,
             uint32_t rootCount = 0);
  // One file, by its full path, named from its path (kAddPending marks a
  // track the scan should read; kAddOwned an image the transfer owns).
  Add addFile(const char* path, uint8_t options = 0);
  // One audio file and its chosen record (a path that isn't audio is
  // addFile()'s).
  Add addRecord(const char* path, const TagView& tags);
  // A folder's other files (`folderPath` absolute, "/music/Artist/Album";
  // the root itself too). False: out of memory.
  bool setFolderFacts(const char* folderPath, const FolderFacts& facts);
  // The album folders that have a transfer thumbnail (FOLD's THUMB in T):
  // their path hashes (2.3.3), sorted. Read by finish(); the array outlives
  // it.
  void setThumbFolders(const uint64_t* sortedHashes, uint32_t n);
  // Elects, sorts, fills the views and trims the blocks. False: out of
  // memory (the index is then empty).
  bool finish();
  // Empties the index and gives every block back (but the kept track
  // block: keepTrackBlock()).
  void clear();
  // The update step's rebuild (3.4.2, N10's review): clear() keeps the
  // track table's block, and the next build (begin()'s Sizing) or load
  // takes it again when the new table fits in it (else it is freed first
  // and the new one asked at its size), so a rebuild of about the same
  // library needs no second free block of the table's size. The heap's
  // largest free block can't stand in for it: ESP-IDF's TLSF rounds a
  // request up to its next size class before it searches, so a block the
  // table's own size isn't found for a table that size. memory().tracks is
  // the kept block's bytes. The destructor frees it.
  void keepTrackBlock(bool on) { keepTracks_ = on; }
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
  const char* str(uint32_t offset) const { return chunk_[offset >> kChunkBits] + (offset & (kChunkBytes - 1)); }
  const char* artistName(uint32_t id) const { return str(artists_[id].name); }
  const char* albumName(uint32_t id) const { return str(albums_[id].name); }
  const char* albumArtistLine(uint32_t id) const { return str(albums_[id].artistLine); }
  const char* folderName(uint32_t id) const { return str(folders_[id].name); }
  const char* trackFileName(uint32_t id) const { return str(tracks_[id].name); }
  // The title is not NUL-terminated (it may sit inside the file name).
  const char* trackTitle(uint32_t id, uint8_t* len) const {
    *len = tracks_[id].titleLen;
    return str(tracks_[id].title);
  }
  // The track's artist as shown: its record's display, else its album's
  // artist line.
  const char* trackArtistName(uint32_t id) const {
    const uint32_t a = tracks_[id].trackArtist;
    return a != kNone ? str(a) : albumArtistLine(tracks_[id].album);
  }

  // "<root>/Artist/Album/06 - Title.mp3" into buf. Returns the length, or 0
  // if it didn't fit (buf then holds "").
  size_t trackPath(uint32_t id, char* buf, size_t size) const;
  size_t folderPath(uint32_t id, char* buf, size_t size) const;
  // The track at `path` ("<root>/Artist/Album/06 - Title.mp3", as
  // trackPath() writes it), or kNone. A binary search per folder level: no
  // allocation, O(depth x log n).
  uint32_t findTrack(const char* path) const;

  Span artistsAZ() const { return {artistsAZ_, artistN_}; }
  Span albumsAZ() const { return {albumsAZ_, albumN_}; }
  // What artistsAZ() and albumsAZ() sort an entry by (textfold::
  // compareSorted() of these): its elected sort tag, else its name. Saved
  // with the views, so a loaded index has them too. The A-Z rail, the row's
  // letter and the jump grid key on textfold::sortName() of it, as the
  // buckets do: "Bowery, Daniel" puts Daniel Bowery among the B's.
  const char* artistSortKey(uint32_t id) const { return str(artistSortKeys_[id]); }
  const char* albumSortKey(uint32_t id) const { return str(albumSortKeys_[id]); }
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
    return {folderTree_ + folders_[folder].firstFile, folders_[folder].fileCount};
  }
  Span treeTracks(uint32_t folder) const {
    return {folderTree_ + folders_[folder].firstFile, folders_[folder].treeCount};
  }
  static constexpr uint32_t rootFolder() { return 0; }

  // A file name's format by its extension (any case): .mp3, .flac, .opus;
  // Unknown for anything else (not a track).
  static Format formatOf(const char* name, size_t len);

  // ---- covers ----
  // The rank of a file name as a cover: 0 cover.jpg, 1 folder.jpg, 2
  // front.jpg, 3 any other .jpg or .jpeg (case-insensitive); kNoImage for
  // anything else (PNGs included: the device decodes JPEG only).
  static uint8_t imageRank(const char* name, size_t len);
  // The folder whose image is `album`'s cover: the album's folder, else
  // its first track's (a disc subfolder); kNone if neither has one.
  uint32_t albumCover(uint32_t album) const;
  // "<folder path>/<its image>" into buf; 0 (and "") if it has none or it
  // didn't fit.
  size_t imagePath(uint32_t folder, char* buf, size_t size) const;

  // A-Z rail: the position in artistsAZ()/albumsAZ() of the first entry in
  // bucket b (0 '#', 1..26 A..Z); bucketStart(v, kBuckets) is the count. A
  // bucket without entries starts where the next one does.
  uint32_t bucketStart(View view, int bucket) const;
  // The bucket of the entry at `position` in that view.
  int bucketAt(View view, uint32_t position) const;

  Memory memory() const;
  // What a loaded file says it was built from (a build's are given to
  // save(): zeros here).
  const Inputs& inputs() const { return inputs_; }
  // FNV-1a 64 of what a track id means (the strings, the folders' names
  // and parents, the tracks' names and folders): equal stamps, equal ids.
  // For a queue saved by ids (3.2.5's fast path, later).
  uint64_t buildStamp() const { return buildStamp_; }
  // Clears every track's source (kSourceMask): two builds of the same files,
  // one from the transfer's records and one from the device's, are then
  // the same bytes (the conformance test, 2.17 item 5) when the two
  // records' lengths fall in the same whole second.
  void forgetSources();
  // Clears every track's length (durationS): the two producers' lengths of
  // one file may differ by up to 100 ms (2.17 item 3: the software's reader
  // doesn't trim an MP3's encoder delay, the device's does), so its length
  // in whole seconds may differ by one. With forgetSources(), the two
  // builds are then the same bytes whatever the lengths (the test compares
  // the lengths apart).
  void forgetLengths();
  // The scan has read track `id` since this index was built (N10): it is
  // no longer kTrackPending, so the scan's sources that read the index
  // (the playing track, the queue, the Library tab's page) don't ask for it
  // again. Nothing else changes (not the build stamp: the ids stay); the
  // next build takes its record.
  void clearPending(uint32_t id);

  // ---- the cache ----
  // Writes the finished index with what it was built from. False: not
  // ready, or the sink failed.
  bool save(ByteSink& out, const Inputs& inputs) const;
  // Replaces what's here with a saved index: Loaded when its hard inputs
  // are `expect`'s (inputs() then has its soft ones). Anything but Loaded
  // leaves the index empty. Every block comes from the hooks, sized
  // exactly.
  Load load(ByteSource& in, const Inputs& expect);
  // Today's path walk alone: the inputs are `signature`, the rest zero.
  bool save(ByteSink& out, uint64_t signature) const;
  Load load(ByteSource& in, uint64_t signature);
  // A saved index's header alone (the boot's decision, 3.2.2: a few hundred
  // bytes, nothing loaded): Loaded with its inputs in `out` (its sum isn't
  // checked: load() does that), Outdated (an earlier version or other
  // rules) or Corrupt (short, foreign, other record sizes, a newer
  // version). Stale and NoMemory never come from here.
  static Load peek(ByteSource& in, Inputs* out);

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
  struct Votes;  // an album's and an artist's run of votes (LibraryIndex.cpp)

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
  bool readBlock(ByteSource& in, Block<T>& b, uint32_t n);  // exactly n records
  template <typename T>
  size_t bytes(const Block<T>& b) const { return static_cast<size_t>(b.cap) * sizeof(T); }
  bool tableInit(Table& t, uint32_t cap);
  void tableDrop(Table& t);
  template <typename Eq>
  uint32_t tableFind(const Table& t, uint32_t hash, Eq eq) const;
  bool tableInsert(Table& t, uint32_t hash, uint32_t id);

  // The strings.
  char* at(uint32_t offset) const { return chunk_[offset >> kChunkBits] + (offset & (kChunkBytes - 1)); }
  uint32_t intern(const char* s, size_t len);  // kNone: no memory
  uint32_t arenaBytes() const;                 // the strings' logical size
  size_t arenaHeld() const;                    // the bytes their blocks hold
  void dropArena();
  void trimArena();

  bool startBuild(const Sizing& s, const char* root);
  uint32_t folderChild(uint32_t parent, const char* name, size_t len);
  // The folders of `path` up to `lastSlash`, created when new; kNone: no
  // memory. The artist's (depth 1 below its root) and album's (depth 2)
  // folders too (kNone when the file is shallower).
  uint32_t folderOf(const char* path, const char* lastSlash, uint32_t* artistFolder, uint32_t* albumFolder);
  Add addOther(const char* path, const char* lastSlash, const char* leaf, size_t leafLen, bool owned);
  // A track's folder, artist and album, created when new.
  Add placeTrack(const char* path, Format* format, uint32_t* folder, uint32_t* artist, uint32_t* album,
                 uint32_t* name, const char** lastSlash);
  uint32_t artistFor(uint32_t nameOffset);
  uint32_t albumFor(uint32_t folder, uint32_t artist, bool loose);
  // The votes: a new track's album and artist runs (closing the ones it
  // leaves), and closing them.
  bool enterRuns(uint32_t album, uint32_t artist);
  void closeAlbumRun();
  void closeArtistRun();
  bool votesReady();
  void dropVotes();
  uint64_t folderHash(uint32_t folder) const;
  static size_t viewWords(uint32_t nT, uint32_t nA, uint32_t nB, uint32_t nF);
  void placeViews(uint32_t nT, uint32_t nA, uint32_t nB, uint32_t nF);  // the views' pointers into viewsBlock_
  bool buildViews();
  void resetViews();
  void setReadPointers();
  uint64_t computeStamp() const;

  AllocFn allocFn_;
  FreeFn freeFn_;
  ShrinkFn shrinkFn_;

  // The strings: chunk i holds offsets [i << kChunkBits, ...). A built
  // index owns each chunk (the last one `lastCap_` bytes, the others
  // kChunkBytes); a loaded one owns one block (`whole_`) the chunks point
  // into.
  char* chunk_[kMaxChunks] = {};
  uint32_t chunks_ = 0;
  uint32_t lastUsed_ = 0;
  uint32_t lastCap_ = 0;
  char* whole_ = nullptr;
  size_t wholeBytes_ = 0;

  // Build state.
  Block<Track> tracksB_;
  Block<Artist> artistsB_;
  Block<Album> albumsB_;
  Block<Folder> foldersB_;
  Table folderTable_, artistTable_;
  // Held while building: each folder's album (kNone: none yet) and marks,
  // each artist's folder name (the table's key: its shown name may become
  // the elected one) and sort tag, each album's sort tag.
  Block<uint32_t> folderAlbum_;
  Block<uint8_t> folderMarks_;
  Block<uint32_t> artistKey_;
  Block<uint32_t> artistSort_;
  Block<uint32_t> albumSort_;
  Votes* votes_ = nullptr;
  uint32_t runAlbum_ = kNone, runArtist_ = kNone, runFirst_ = 0;
  const char* const* roots_ = nullptr;  // LIBR, relative to the root
  uint32_t rootCount_ = 0;
  const uint64_t* thumbs_ = nullptr;
  uint32_t thumbN_ = 0;
  uint32_t rootLen_ = 0;
  uint32_t emptyName_ = kNone;  // "" in the strings
  uint32_t various_ = kNone;    // "Various Artists", when an album needs it
  bool building_ = false;
  bool failed_ = false;
  bool ready_ = false;
  bool keepTracks_ = false;  // keepTrackBlock()
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
  uint32_t* folderTree_ = nullptr;   // tracks by folder: depth first, each folder's files A-Z
  uint32_t* artistSortKeys_ = nullptr;  // per artist: its sort key's string (artistSortKey())
  uint32_t* albumSortKeys_ = nullptr;   // per album
  uint32_t artistBuckets_[kBuckets + 1] = {};
  uint32_t albumBuckets_[kBuckets + 1] = {};
  Inputs inputs_;
  uint64_t buildStamp_ = 0;

  // Read side: pointers into the blocks, fixed after finish().
  const Track* tracks_ = nullptr;
  const Artist* artists_ = nullptr;
  const Album* albums_ = nullptr;
  const Folder* folders_ = nullptr;
  uint32_t trackN_ = 0, artistN_ = 0, albumN_ = 0, folderN_ = 0;
};
