// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"

// The validation walk (docs/METADATA.md 2.10.1, 3.2.3; milestone N5): the
// device's listing of /music, which tells D (/.player/tags.bin) what changed
// on the card since the last walk, and settles which files T's records (the
// transfer's) still describe. Portable, host-tested (test_card_walk), in
// fixed memory whatever the card holds. A job for the card worker, one step
// at a time (3.3.4: a step is one folder's listing, or one doubtful file).
//
// The walk:
//   - lists one folder at a time through the Lister (FatFs's f_readdir on
//     the device, N10: a FILINFO gives the name, the size and the FAT time
//     in the same directory read), one DIR open at a time, into the caller's
//     scratch (64 KB of PSRAM on the device);
//   - sorts each folder into the canonical order (2.6.8): its files by their
//     names' bytes, then its subfolders by name, depth first. A folder whose
//     entries don't fit the scratch is listed in passes, each keeping the
//     smallest names after the last one used, so the walk never emits out
//     of order whatever the folder holds (a 3,000-file folder through a few
//     KB of scratch). The scratch is a stack: a folder's subfolders wait in
//     it while its children are walked, each level keeping room for the
//     levels below it;
//   - sees what the device sees (2.8.2): a name starting with "." is
//     skipped, file or folder; so is a path over 255 bytes ("/music/"
//     included); folders are walked to 8 levels below /music. Audio is
//     LibraryIndex::formatOf()'s, a cover image imageRank()'s;
//   - digests each folder: FNV-1a 64 over its audio and image files'
//     (name, size, FAT time) in name order and its count of other files
//     (FolderDigest). A folder's own FAT time isn't used: FAT doesn't update
//     it when its contents change. A folder whose digest is D's is skipped:
//     nothing is read of D for it and nothing written. That is the normal
//     boot;
//   - merges a changed folder's audio files against D's rows for it (Known),
//     and on the first walk after a commit every folder's against T's
//     records too (Transfer: one near-sequential pass over T, in the same
//     order). A file D has and the card doesn't is Gone; one the card has
//     and D doesn't is Added; one whose row changes is Changed.
//
// T's freshness (2.9 rule 1, 2.3.4, 2.3.5), per audio file T has a record
// for:
//   - a size other than the card file's: T doesn't describe it;
//   - the same size and FAT time (or, on a walk at the same commit, the time
//     shifted by the skew the first walk found): T's (Software);
//   - the same size and another time, or a 0 or an invalid stamp on either
//     side: Doubtful. Its row goes to the sink as it would be without T (the
//     scan reads it, slower, never wrong), and the doubt is kept by the sink
//     (walk.jnl, N4), not in RAM: every file is doubtful when a PC shifted
//     every stamp. On the first walk after a commit, every pair whose sizes
//     match and whose stamps are valid counts in a SkewHistogram (2 KB,
//     exact while at most 256 distinct deltas came, a Misra-Gries summary
//     past that): every file T has a record for, covers included, as 2.3.4
//     says; a pair that isn't an audio file's doubt is kept as a count-only
//     doubt for the recount. At the walk's end the skew is found (with one
//     pass over the doubts first when the summary was needed, so it is
//     2.3.4's whatever the order), then each doubt is settled: its delta is
//     the skew, T's; else a T record with no qfp (0) can't be confirmed;
//     else, when D saved the file's qfp at this size and time, that is
//     compared with T's, no read; else the file's qfp is read (an open and
//     two reads of up to 4 KB) and compared. Equal: T's, and confirmed;
//     else the row without T. A qfp read is saved in the row either way, so
//     a confirmation is paid once per file, not per boot, and a later commit
//     with the same fingerprint is settled without a read.
// A T record flagged UNREADABLE without FROM_API settles only that the file
// is the software's (2.9): its row is the one without T, and the scan reads
// the file (3.3.1). Images: the best cover of a folder is "owned" (its
// FolderRow::imageOwned, 2.14.3's "a cover the ledger lists") when T has a
// record of the same size at its path; the time isn't asked (a listener who
// replaced the software's cover.jpg by one of exactly its size is taken to
// be the software).
//
// The rows the walk gives (FileRow) are what LibraryBuilder reads back as
// D's: the file's size and time as the walk saw them, its status, and the
// confirmation (LibraryBuilder::Row::confirmed), with the skew in the
// Summary for D's header (DHDR). So the builder's rule 1 holds for exactly
// the files the walk found fresh.
//
// What N4's TagStore provides (the interfaces below): Known, D's folders in
// pre-order with each one's digest and owned flag, and each folder's rows in
// name order (DFLD and the records: "a seek to the folder's range"); Sink,
// the walk's output as walk.jnl: the run of the walk in its order, the
// doubts read back after it, then the settled rows. Every folder the walk
// gave a row is one D lists (a folder is given one when it has audio at or
// below it, or D already lists it), so an unchanged card writes nothing. A
// folder with no audio at or below it (a "Scans" folder of images) has no
// row: each walk lists it and finds nothing to say, at the cost of its
// listing and, when T is given, one lookup of its best cover.
//
// Which T to give (N12's glue): on the first walk after a commit, a
// StreamedTransfer (every file is asked, in order: one pass); on a walk at
// the same commit, an IndexedTransfer (only the changed files and the covers
// of the folders merged are asked: a few HIDX lookups; on an unchanged card,
// only the covers of folders D doesn't list).
namespace cardwalk {

using Status = LibraryBuilder::Status;  // D's statuses (3.3.2's DSTA): Scanned, Software, Pending, Unreadable

// Folder levels walked below /music (2.8.2; today's Library.cpp kMaxDepth).
constexpr uint32_t kMaxDepth = 8;

// One directory entry, as FatFs's f_readdir fills a FILINFO.
struct Entry {
  const char* name = nullptr;  // the stored long name in UTF-8, NUL-terminated (the lister's buffer)
  size_t nameLength = 0;
  bool folder = false;
  uint32_t size = 0;     // fsize (a folder's: unused)
  uint32_t fatTime = 0;  // (fdate << 16) | ftime (2.3.4)
};

// The card, read through FatFs on the device (N10): its folders under
// /music, listed, and its files, read for a qfp. Paths are relative to
// /music ("" is /music itself; "Artist/Album"), NUL-terminated at `len`,
// names as the card stores them (2.8.5).
class Lister {
public:
  enum class Open : uint8_t { Ok, Missing, Error };
  enum class Next : uint8_t { Entry, End, Error };
  virtual ~Lister() = default;
  // f_opendir. Missing: no such folder (only /music may be missing: a card
  // with no library); Error: a read failed. One folder open at a time.
  virtual Open openDir(const char* rel, size_t len) = 0;
  // f_readdir: the next entry ("." and ".." never come), End after the last.
  virtual Next next(Entry* out) = 0;
  virtual void closeDir() = 0;
  // f_open for reading (one file at a time, between listings): the file as
  // a Source, nullptr when it can't be opened. Valid until closeFile().
  virtual cardcontract::Source* openFile(const char* rel, size_t len) = 0;
  virtual void closeFile() = 0;
};

// An audio file's row in D, after the walk (3.3.2's DSTA, and the record's
// size, fatTime and qfp).
struct FileRow {
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint64_t qfp = 0;  // the device's qfp of the file at this size and time (2.3.5); 0 unknown
  Status status = Status::Pending;
  bool confirmed = false;  // T's qfp was confirmed against the file at this size and time (2.9 rule 1)
  bool operator==(const FileRow& o) const {
    return size == o.size && fatTime == o.fatTime && qfp == o.qfp && status == o.status && confirmed == o.confirmed;
  }
  bool operator!=(const FileRow& o) const { return !(*this == o); }
};

// A folder's row in D (DFLD): its digest and its other files (the cover
// facts the builder asks for, LibraryBuilder::FolderFactsSource).
struct FolderRow {
  uint64_t digest = 0;
  const char* image = "";  // the best cover image's name (LibraryIndex::imageRank(), ties: the first by name); "" none
  size_t imageLength = 0;
  uint8_t imageRank = LibraryIndex::kNoImage;
  uint32_t imageSize = 0;
  uint32_t imageTime = 0;
  bool imageOwned = false;  // T has a record of its size at its path (the ledger lists it, 2.14.3)
  uint32_t audio = 0;       // audio files
  uint32_t images = 0;      // cover images (.jpg, .jpeg), the best one included
  uint32_t others = 0;      // every other file
};
// The facts LibraryIndex takes (setFolderFacts()): its otherCount counts
// every file that isn't audio, images included.
LibraryIndex::FolderFacts factsOf(const FolderRow& row);

// A folder's digest (3.2.3 step 4): FNV-1a 64 from the basis over each audio
// and image file in name order (its name's bytes, a 0 byte, then its size
// and its FAT time as u32 LE), then the count of its other files (u32 LE).
// Device-internal: a firmware that changes it sees every folder changed once.
class FolderDigest {
public:
  void add(const char* name, size_t len, uint32_t size, uint32_t fatTime);
  void other() { ++others_; }
  uint64_t value() const;

private:
  uint64_t h_ = cardcontract::kFnvBasis;
  uint32_t others_ = 0;
};

// The qfp of a file (2.3.5) read through `src`, whose size must be `size`
// (false: it isn't, or a read failed), through a caller's buffer of any size
// (two reads when it holds 4 KB).
bool fileQfp(cardcontract::Source& src, uint32_t size, uint8_t* buf, uint32_t bufBytes, uint64_t* out);

// D, as the last walk left it (N4's TagStore over tags.bin: DFLD and the
// records). Strings stay valid until the next call of the same function.
struct KnownFolder {
  const char* path = "";  // relative to /music
  size_t pathLength = 0;
  uint64_t digest = 0;
  bool imageOwned = false;
};
struct KnownFile {
  const char* name = "";
  size_t nameLength = 0;
  FileRow row;
};
class Known {
public:
  virtual ~Known() = default;
  // D's folders in pre-order (2.6.8), from the first: false after the last.
  virtual bool nextFolder(KnownFolder* out) = 0;
  // The rows of the folder nextFolder() gave last, by name bytes: false
  // after the last. Rows not asked for are skipped.
  virtual bool nextFile(KnownFile* out) = 0;
  // A read failed: the walk stops (D is then the TagStore's to rebuild).
  virtual bool failed() const { return false; }
};

// T: the valid root's MPTG companion (2.5).
struct TransferRecord {
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint64_t qfp = 0;
  uint16_t flags = 0;
  uint8_t container = 0;
};
class Transfer {
public:
  virtual ~Transfer() = default;
  // T's record of `rel`, asked in canonical order (2.6.8), each path at most
  // once. False: T has none.
  virtual bool find(const char* rel, size_t len, TransferRecord* out) = 0;
  // At the walk's end: true when T read whole and passed every check (a
  // stream's CRCs are known only at its end, 3.4.1).
  virtual bool finish() = 0;
};

// T read front to back (the first walk after a commit: 3.9 MB at 20k, one
// near-sequential pass), checked as it goes (mptg::Walker, 2.4.3).
class StreamedTransfer : public Transfer {
public:
  // `scratch` for the walker's four streams: 8 KB is 3.4.1's budget.
  cardcontract::Why begin(cardcontract::Source& src, uint8_t* scratch, uint32_t scratchBytes);
  bool find(const char* rel, size_t len, TransferRecord* out) override;
  bool finish() override;
  cardcontract::Why why() const { return walker_.why(); }

private:
  bool advance();  // to the next record: false at the end, or on a failure
  cardcontract::mptg::Walker walker_;
  bool begun_ = false;
  bool have_ = false;  // the walker stands on a record not yet passed
  bool done_ = false;
};

// T read at random through its HIDX (a walk at the same commit asks for a
// few changed files): a file a walk already checked whole.
class IndexedTransfer : public Transfer {
public:
  cardcontract::Why begin(cardcontract::Source& src);
  bool find(const char* rel, size_t len, TransferRecord* out) override;
  bool finish() override { return ok_; }

private:
  cardcontract::mptg::File file_;
  bool ok_ = false;
};

// How a file's row reached the sink.
enum class Change : uint8_t {
  Added,     // a file D has no row for
  Changed,   // its row changed
  Doubtful,  // its row until the doubt is settled (the row without T): a doubt() follows
  Settled,   // the settled row of a doubtful file (after the walk)
};

// A pair kept for after the walk.
struct Doubt {
  bool settle = true;     // an audio file whose row waits on it; false: kept only for the skew's recount
  bool hasDelta = false;  // both stamps valid and non-zero: `delta` is W(the card's) - W(T's)
  int64_t delta = 0;
  uint32_t size = 0;      // the card file's
  uint32_t fatTime = 0;
  uint64_t transferQfp = 0;  // T's record's (0 unknown)
  uint64_t deviceQfp = 0;    // D's saved qfp of the file at this size and time (0 unknown)
  Status fallback = Status::Pending;  // the row's status when T doesn't match
};

struct Summary {
  bool firstAfterCommit = false;  // D's walk identity becomes the root's commit
  int32_t skew = 0;               // T's skew (2.3.4) for DHDR; 0 none
  bool changed = false;           // the sink was given rows (a first walk after a commit always changes D's header)
  // A doubt couldn't be settled (its qfp read failed): its row is the one
  // without T, and D must not take this walk's commit, so that the next walk
  // is a first one and asks T again (a walk at the same commit would find
  // the row at the file's size and time and never ask).
  bool unsettled = false;
};

// The walk's output (N4: walk.jnl). False from any call stops the walk.
class Sink {
public:
  enum class Read : uint8_t { Item, End, Error };
  virtual ~Sink() = default;
  // The walk's run, as it goes: file rows and gones in canonical order
  // (2.6.8), folder rows and gones in pre-order, the two kinds interleaved
  // (a folder D doesn't list gets its row once audio turns up at or below
  // it, so maybe after its subfolders' file rows).
  virtual bool folder(const char* rel, size_t len, const FolderRow& row) = 0;
  virtual bool folderGone(const char* rel, size_t len) = 0;
  virtual bool file(const char* rel, size_t len, Change change, const FileRow& row) = 0;
  virtual bool fileGone(const char* rel, size_t len) = 0;
  // The doubts, in canonical order (a settling one right after its file's
  // Doubtful row), read back after the walk: rewindDoubts(), then
  // nextDoubt() until End (`rel` holds kMaxRelPath + 1 bytes).
  virtual bool doubt(const char* rel, size_t len, const Doubt& d) = 0;
  virtual bool rewindDoubts() = 0;
  virtual Read nextDoubt(char* rel, size_t* len, Doubt* d) = 0;
  // The walk is over: its summary. abort() instead when it failed: nothing
  // it gave counts.
  virtual bool finish(const Summary& s) = 0;
  virtual void abort() = 0;
};

// About 7 KB (its stack of folders, the paths, the 2 KB histogram): a heap or
// PSRAM object, never the card worker's stack, like the scratch it's given.
class CardWalk {
public:
  // A folder entry in the scratch: 12 bytes of header and the name with its
  // NUL (at most kMaxRelPath bytes), aligned to 4, and its 4-byte offset.
  static constexpr uint32_t kEntryMax = 4 + ((12 + cardcontract::kMaxRelPath + 1 + 3) & ~3u);
  // The smallest scratch: one entry for each of the nine levels.
  static constexpr uint32_t kMinScratch = (kMaxDepth + 1) * kEntryMax;
  // The device's: 64 KB of PSRAM (/music with 705 children is about 28 KB).
  static constexpr uint32_t kDeviceScratch = 64 * 1024;
  // The qfp reads' buffer, taken from the scratch once the listing is done.
  static constexpr uint32_t kQfpBuffer = cardcontract::kQfpPart;

  struct Config {
    Lister* lister = nullptr;
    Known* known = nullptr;        // D; nullptr: the device knows nothing yet (every file Added)
    Transfer* transfer = nullptr;  // T; nullptr: no valid transfer data
    Sink* sink = nullptr;
    // D's walk identity isn't the root's (a transfer happened, or /.mstream
    // went): every folder is merged, against T when there is one (else D's
    // Software rows turn Pending), and the skew is found again.
    bool firstAfterCommit = false;
    int32_t skew = 0;  // T's skew as D's header has it: a walk at the same commit uses it
    uint8_t* scratch = nullptr;  // at least kMinScratch (kDeviceScratch on the device)
    uint32_t scratchBytes = 0;
  };

  enum class State : uint8_t { Idle, Walking, Settling, Done, Failed };
  enum class Error : uint8_t {
    None,
    Config,    // begin()'s: a missing interface, or the scratch under kMinScratch
    Card,      // a listing failed (the card went, or a read)
    Known,     // D couldn't be read
    Sink,      // the sink refused (the card is full, a write failed)
    Transfer,  // T failed a check as it streamed: walk again without it
  };

  struct Result {
    State state = State::Idle;
    Error error = Error::None;
    Summary summary;
    uint32_t steps = 0;
    uint32_t listings = 0;  // folder passes (a folder that fits takes one)
    uint32_t folders = 0;   // walked
    uint32_t audio = 0, images = 0, others = 0;  // files seen
    uint32_t foldersMerged = 0;   // read against D (changed, new, or the first walk after a commit)
    uint32_t folderRows = 0;      // folder rows given
    uint32_t foldersGone = 0;
    uint32_t added = 0, changed = 0, gone = 0;
    uint32_t doubtful = 0;        // audio files
    uint32_t countOnly = 0;       // doubts kept for the recount only
    uint32_t bySkew = 0;          // doubtful files the skew settled as T's
    uint32_t byDeviceQfp = 0;     // ... D's saved qfp, without a read
    uint32_t byQfp = 0;           // ... a qfp read
    uint32_t notTransfer = 0;     // doubtful files that aren't T's
    uint32_t qfpReads = 0;
    uint32_t qfpFailed = 0;       // a file that couldn't be read (its row without T; summary.unsettled)
    bool recounted = false;       // the histogram needed the pass over the doubts
  };

  // Starts a walk. False (Error::Config): a missing lister or sink, or the
  // scratch too small. The interfaces and the scratch must outlive it.
  bool begin(const Config& config);
  // One step: at most one folder listing (a folder that fits, or one pass of
  // a big one), or a few doubts with at most one qfp read. Returns the state
  // after it.
  State step();
  // Steps to the end.
  State run();
  const Result& result() const { return r_; }
  State state() const { return r_.state; }

private:
  // One folder on the walk's stack.
  enum class Phase : uint8_t { Begin, Digest, Merge, Dirs };
  struct Frame {
    uint16_t pathLength = 0;
    uint8_t depth = 0;
    Phase phase = Phase::Begin;
    uint32_t lo = 0, hi = 0;  // its part of the scratch
    // The subfolders waiting (Phase::Dirs): offsets at lo, entries at [dirBottom, hi).
    uint32_t dirs = 0, nextDir = 0, dirBottom = 0;
    bool moreDirs = false;  // subfolders after these: list again
    // D's view of it.
    bool known = false;
    uint64_t knownDigest = 0;
    bool knownOwned = false;
    // Its files.
    FolderDigest digest;
    bool merge = false;
    bool mergeFromStart = false;  // a big folder's merge passes start over
    bool fused = false;           // ... or digest it as they go (merged whatever its digest)
    uint32_t audio = 0, images = 0, others = 0;
    uint8_t imageRank = LibraryIndex::kNoImage;
    uint32_t imageSize = 0, imageTime = 0;
    bool imageOwned = false;
    uint16_t imageLength = 0;
    char image[cardcontract::kMaxRelPath + 1] = "";
    bool rowPending = false;  // D doesn't list it and no audio is at or below it yet: its row waits
    bool rowGiven = false;
  };
  // A pass's bound: an entry's place in the canonical order.
  struct Key {
    bool folder = false;
    uint16_t length = 0;
    char name[cardcontract::kMaxRelPath + 1] = "";
  };
  enum class SettlePhase : uint8_t { Recount, Settle };

  // The listing (the scratch).
  bool fill(Frame& f, const Key* after);
  void place(const Entry& e, uint8_t kind);
  void evictMax();
  void compact();
  int compareAt(uint32_t a, uint32_t b) const;
  int compareEntry(const Entry& e, const Key& k) const;
  int compareEntryAt(const Entry& e, uint32_t at) const;
  void keyOf(uint32_t at, Key* out) const;
  uint32_t firstFolder() const;  // the batch's first subfolder (its count when none)
  bool filesComplete() const;    // the batch holds every file of the folder not yet passed
  void keepDirs(Frame& f);
  // The walk.
  void stepFolder();
  void enterChild(Frame& parent);
  void decideMerge(Frame& f);
  void digestFiles(Frame& f, uint32_t end);
  void digestOne(Frame& f, uint32_t at);
  bool mergeFiles(Frame& f, uint32_t end);
  bool mergeOne(Frame& f, uint32_t at, const KnownFile* d);
  bool endFiles(Frame& f);
  bool giveRow(uint32_t level);
  void finishWalk();
  // D.
  void advanceKnown();
  bool nextKnownFile();
  bool syncKnown(Frame& f);
  bool goneFolder();
  bool beginRows(Frame& f);
  bool goneRow(const Frame& f);
  // The sink.
  bool emitFile(const char* rel, size_t len, Change change, const FileRow& row);
  bool emitDoubt(const char* rel, size_t len, const Doubt& d);
  // After the walk.
  void stepSettle();
  bool settleOne(const Doubt& d, size_t len, bool* read);
  void fail(Error e);
  size_t filePath(const Frame& f, const char* name, size_t len);
  const char* nameAt(uint32_t at) const { return reinterpret_cast<const char*>(base_ + at + 12); }
  uint16_t nameLengthAt(uint32_t at) const;
  uint32_t* offsets(uint32_t lo) { return reinterpret_cast<uint32_t*>(base_ + lo); }

  Config c_;
  Result r_;
  uint8_t* base_ = nullptr;  // the scratch, 4-aligned
  uint32_t bytes_ = 0;
  Frame frames_[kMaxDepth + 1];
  uint32_t depth_ = 0;  // frames on the stack
  char path_[cardcontract::kMaxRelPath + 2] = "";   // the current folder, then "/" and a file's name
  char other_[cardcontract::kMaxRelPath + 2] = "";  // a path given to the sink or read back from it
  // The current pass: offsets at [lo_, lo_ + 4n_), entries from hi_ down to bottom_.
  uint32_t lo_ = 0, hi_ = 0, cap_ = 0, n_ = 0, bottom_ = 0, live_ = 0;
  bool truncated_ = false;
  Key ceiling_;  // the smallest key the pass dropped (when truncated_)
  Key after_;    // where the next pass starts
  // D.
  bool dStarted_ = false;   // its first folder was asked for
  bool dHave_ = false;      // a folder from Known waits
  bool dUsed_ = false;      // ... and the walk matched it
  bool dFileHave_ = false;  // a row of it waits (dFile_)
  char dPath_[cardcontract::kMaxRelPath + 1] = "";
  size_t dLength_ = 0;
  uint64_t dDigest_ = 0;
  bool dOwned_ = false;
  KnownFile dFile_;
  char dName_[cardcontract::kMaxRelPath + 1] = "";
  // T's skew, and the doubts after the walk.
  cardcontract::SkewHistogram skew_;
  SettlePhase settle_ = SettlePhase::Recount;
  bool settleBegun_ = false;
};

}  // namespace cardwalk
