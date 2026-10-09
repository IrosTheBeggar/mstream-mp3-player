// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"

// The device's own tag records on the card (docs/METADATA.md 3.3.2, 2.12.6;
// milestone N4): /.player/tags.bin (D) with its device sections, the two
// journals that extend it (tags.jnl, the scan's; walk.jnl, the walk's), the
// streaming compaction that folds them into a new D, and the recovery from a
// power cut at any moment. Portable, host-tested (test_tag_store) through a
// file interface (Fs) whose fake can stop at any write, sync, remove or
// rename, a rename being two directory writes.
//
// The files, all in Config::dir (/.player):
//
//   tags.bin   MPTG v1, source 1 (2.6): one record per audio file the last
//              walk saw, in canonical order (2.6.8), with HIDX; then the
//              device's sections, which other readers skip:
//                DSTA  a 16-byte header (records, ownRecords: the rows that
//                      aren't Software, ownFolders: the FOLD entries on their
//                      paths), then one byte per record: its Row (status in
//                      bits 0-1, bit 2 "T's qfp confirmed at this size");
//                DFLD  an 8-byte header (folders), then per FOLD entry its
//                      FolderFacts (N5's folder row: the digest, the best
//                      cover, the counts of audio, images and other files);
//                DHDR  48 bytes: the commit the last walk compared against,
//                      T's skew it found, the rescan epoch.
//              A Software or Pending row holds the size, the FAT time and the
//              qfp the walk read (N5 saves it: a confirmation is paid once);
//              an Unreadable one the record's UNREADABLE flag; a Scanned one
//              the full record. Every record in D was read by the header's
//              parserVersion at DHDR's epoch: a compaction under another
//              parser, or a Rescan, turns Scanned and Unreadable rows back
//              into Pending.
//   tags.jnl   The scan's chunks (ChunkBuilder): each its records sorted into
//              canonical order, headed by its sequence (1, 2, ...), the
//              headerCrc of the D it extends (0: none) and the parser and
//              epoch that read them, and closed by a CRC-32. A torn last chunk
//              fails its CRC and ends the journal (the next append cuts it
//              off); a journal for another D was already merged and is
//              dropped. So it is a sequence of sorted runs.
//   walk.jnl   The last walk's changes (WalkWriter): a header (the D it
//              extends, the commit it compared against, tags.jnl's last
//              sequence when it began), then at most two sorted runs of CRC'd
//              blocks, each run closed by an End block that carries T's skew:
//              run 1 the walk's File, Gone, Doubtful and Folder entries, run 2
//              the doubtful files' resolutions (its End says whether one
//              couldn't be settled: a qfp read failed). A run without its
//              End is dropped: a walk cut short in run 1 is walked again; cut
//              in run 2, its run 1's rows stand. DHDR takes the walk's commit
//              and skew from a whole walk whose doubts were all settled; a
//              walk that left one unsettled (run 1 alone with a doubt, or a
//              qfp read that failed) leaves DHDR unwalked, so the next boot's
//              walk is a first one: it merges every folder against T and
//              settles the doubts again (a walk at the same commit would skip
//              the folders whose rows run 1 already gave, and never ask T
//              about them).
//   tags.tmp   The compaction's output, renamed over tags.bin.
//   hidx.tmp   The compaction's scratch: HIDX's pairs before their sort.
//   tags.xl1.. Twins (2.12.6): a tags.tmp that shared tags.bin's cluster
//              chain after a cut inside a rename, kept until a disk check.
//
// Which row wins (the merged view, which a compaction writes out and View
// reads without writing): per path, the newest source in time: D, then the
// chunks older than the walk (their sequence at most walk.jnl's watermark),
// then walk.jnl's run 1, its run 2, then the chunks after the walk. A walk
// entry sets a file's row (its status, size, time and qfp; a reading made at
// the same size and time stands unless T now covers the file) or takes it
// away (Gone); a doubt changes no row (the walk gives the doubtful file's row
// without T, and its settled row in run 2: a cut before run 2's End leaves
// the row without T, which the scan reads, slower than the qfp check it
// missed, never wrong); a chunk record sets a Scanned or Unreadable row. The
// card's /music doesn't change while the device runs, so within a session
// the walk's sightings and the scan's agree; across a card pulled and
// changed, the walk of the next boot sees the change against D and its entry
// is newer than the older chunks.
//
// The compaction (3.3.2) is a streaming k-way merge of D, walk.jnl's runs and
// tags.jnl's chunks in two passes: the first counts the new D's sections and
// writes HIDX's pairs to hidx.tmp, the second writes each section at its
// place (FOLD, RECS, STRS's two runs, DSTA, DFLD), then HIDX is sorted in
// passes over hidx.tmp (read kSortRead at a time) in the memory the merge
// gave back and the buffers the second pass left idle (every one but HIDX's
// own section buffer), then DHDR, the
// directory and the header, a sync, and the rename: remove tags.bin (or
// rename it to a twin), rename tags.tmp, remove the journals. Every input is
// read through a small buffer and nothing is read whole: the work memory is
// fixed by Config (about 115 KB at the defaults: workBytes()), whatever the
// files hold. Each section's buffer ends on the file's 4 KB boundaries
// (kWriteAlign below), so the card is written whole sectors at a time
// (METADATA.md 3.3.7). A journal of more than Config::maxChunks chunks
// (this firmware never writes one: append() refuses and asks for a
// compaction) has the rest dropped; their files stay Pending and the scan
// reads them again.
//
// Recovery (open(), at boot, before anything writes): the cut-rename rule of
// 2.12.6 for tags.bin and tags.tmp (settle()), the twins collected, a
// leftover hidx.tmp removed, D's frame and header checked, the journals
// scanned. Every cut point leaves either the old D with its journals or the
// new D (whose journals are then stale), so the merged view is the same; and
// no cut leads to a cluster chain being freed while another entry uses it.
//
// One job at a time (the card worker, 3.3.4): nothing appends, walks or
// compacts while another of these runs, and nothing writes D or the journals
// while a build or a View streams them.
namespace tagstore {

using Status = LibraryBuilder::Status;
using AllocFn = void* (*)(size_t bytes);
using FreeFn = void (*)(void* p);
using cardcontract::Why;
struct Merge;

// ---------------------------------------------------------------------------
// The file interface: FatFs on the device (f_open, f_lseek and f_read or
// f_write, f_sync, f_truncate, f_close, f_unlink, f_rename, FIL.obj.sclust),
// memory on the host. Paths are absolute ("/.player/tags.bin").
// ---------------------------------------------------------------------------
class File : public cardcontract::Source, public cardcontract::Sink {
public:
  // Source: size() and read(offset, ...). Sink: write(offset, ...), which
  // extends the file when it writes past its end (the bytes between are
  // whatever the card held: every writer here fills them).
  virtual bool sync() = 0;                  // f_sync: the data and the directory entry on the card
  virtual bool truncate(uint32_t size) = 0;  // f_truncate at `size` (at most the file's size)
};

// How a write reaches the card (the 2026-10-09 device run, METADATA.md
// 3.3.7): FatFs writes the whole sectors of a write from a sector's start
// straight from the caller's bytes, as many at once as are contiguous; a
// partial sector goes through the FIL's one-sector buffer, the sector read
// first when it is inside the file, and written alone when the write moves
// on. So the store's writers end their buffers on these boundaries of the
// file (TagStore's section cursors), and the firmware's File (CardFat)
// cuts a long write into pieces of at most kWriteAlign that end on them
// (writePiece()): a 4 KB piece from a 4 KB boundary is one 8-sector card
// write inside a cluster of 4 KB or more, where an unaligned one was a
// single-sector write at each end, each read first.
constexpr uint32_t kSectorBytes = 512;
constexpr uint32_t kWriteAlign = 4096;
// HIDX's sort reads hidx.tmp this much at a time (8 sectors a card command;
// a 1 KB run buffer read it 2 at a time: the 2026-10-09 run's compactions,
// B4).
constexpr uint32_t kSortRead = 4096;
// The first piece of a write of `left` bytes at `offset`: up to the next
// kWriteAlign boundary of the file, at most `left`.
inline uint32_t writePiece(uint32_t offset, uint32_t left) {
  const uint32_t edge = kWriteAlign - offset % kWriteAlign;
  return left < edge ? left : edge;
}

class Fs {
public:
  enum class Mode : uint8_t {
    Read,    // an existing file
    Create,  // FA_CREATE_ALWAYS, read and written: a new file, or an existing one emptied (its clusters freed)
    Update,  // an existing file, read and written
  };
  virtual ~Fs() = default;
  // nullptr: missing (Read, Update), or it couldn't be opened.
  virtual File* open(const char* path, Mode mode) = 0;
  // f_close: a written file is synced first. The File is gone either way.
  virtual bool close(File* f) = 0;
  virtual bool exists(const char* path) = 0;
  virtual bool remove(const char* path) = 0;
  // f_rename: fails when `to` exists. On FAT it writes the new directory
  // entry, then removes the old one: a cut between the two leaves both on
  // one cluster chain (2.12.1, "Cut renames").
  virtual bool rename(const char* from, const char* to) = 0;
  // The file's first cluster (FIL.obj.sclust after f_open): two entries that
  // give the same non-zero value share a chain. 0: missing, or empty (no
  // cluster).
  virtual uint32_t firstCluster(const char* path) = 0;
};

// ---------------------------------------------------------------------------
// The cut-rename rule (2.12.6), for any file the device replaces through a
// tmp (tags.bin; library.idx, queue.txt and device.txt keep their own names
// and can use it too). A rename cut between its two directory writes leaves X
// and X.tmp on one chain, and the next X.tmp opened with FA_CREATE_ALWAYS (or
// a remove of either) would free clusters the other still uses. So:
//   - settle(), at boot: X.tmp alone is renamed X when `check` says it is
//     whole (it was synced before X went), removed when it says torn, and
//     left as it is when the check couldn't run (no memory for it: a
//     failed allocation is no verdict on the file); X and X.tmp on
//     different chains: the tmp is a leftover, removed; on one chain: the
//     tmp is renamed to a twin (X.xl1, a rename frees nothing);
//   - prepareTmp(), before X.tmp is created: a leftover the same way;
//   - replace(), the last step of a write: X is removed, or renamed to a twin
//     when it shares a twin's chain; then X.tmp is renamed X;
//   - collectTwins(): a twin is removed once no other entry (X, X.tmp,
//     another twin) shares its first cluster (a disk check copied the chains
//     apart), never before.
// 2.12.6 names two twins, .xl1 and .xl2; repeated cuts without a disk check
// take the next free name up to .xl9, after which the file can't be replaced
// (replace() and prepareTmp() fail) until a disk check frees them.
// ---------------------------------------------------------------------------
struct Names {
  const char* path = nullptr;  // "/.player/tags.bin"
  const char* tmp = nullptr;   // "/.player/tags.tmp"
  const char* stem = nullptr;  // "/.player/tags": the twins are "<stem>.xl1".."<stem>.xl9"
};
constexpr int kMaxTwins = 9;
// "<stem>.xl<i>" (i in 1-9); false when it doesn't fit.
bool twinName(const Names& n, int i, char* out, size_t cap);

class TmpCheck {
public:
  enum class Verdict : uint8_t {
    Torn,     // not a whole file of its kind (or it couldn't be opened)
    Whole,    // a whole file of its kind (its checksums hold)
    Unknown,  // the check couldn't run (no memory for it): no verdict
  };
  virtual ~TmpCheck() = default;
  virtual Verdict check(Fs& fs, const char* tmp) = 0;
};

enum class Settle : uint8_t {
  Clean,        // no tmp
  Removed,      // a leftover tmp, removed
  Promoted,     // X was missing and the tmp whole: renamed X
  Quarantined,  // the tmp shared a chain: renamed to a twin
  Failed,       // a remove or rename failed (or no twin name was free)
  Kept,         // X was missing and the check couldn't run: the tmp left for the next settle()
};
struct Settled {
  Settle what = Settle::Clean;
  uint32_t twins = 0;  // twins left after collectTwins(): the card wants a disk check
};
Settled settle(Fs& fs, const Names& n, TmpCheck* check);
bool prepareTmp(Fs& fs, const Names& n);
bool replace(Fs& fs, const Names& n);
uint32_t collectTwins(Fs& fs, const Names& n);  // the twins left

// ---------------------------------------------------------------------------
// D's device sections (3.3.2; the layout is the device's own).
// ---------------------------------------------------------------------------
constexpr uint32_t kDsta = cardcontract::fourcc("DSTA");
constexpr uint32_t kDfld = cardcontract::fourcc("DFLD");
constexpr uint32_t kDhdr = cardcontract::fourcc("DHDR");
constexpr uint32_t kDstaHeader = 16;
constexpr uint32_t kDfldHeader = 8;
constexpr uint32_t kDhdrBytes = 48;
constexpr uint32_t kFactsFixed = 31;  // a DFLD entry before its cover image's name

// One record's row: its status, and whether T's qfp was confirmed against the
// file at this size (2.9, rule 1).
struct Row {
  Status status = Status::Pending;
  bool confirmed = false;
};
uint8_t encodeRow(const Row& r);
Row decodeRow(uint8_t b);
LibraryBuilder::Row builderRow(const Row& r);

// The commit a walk compared against (2.5.2, 2.10.1): the transfer's
// identity, or present false (no valid root).
struct Identity {
  bool present = false;
  uint64_t cardId = 0;
  uint32_t generation = 0;
  uint64_t commitId = 0;
  uint32_t tagsCrc = 0;  // T's headerCrc
  bool operator==(const Identity& o) const {
    return present == o.present && cardId == o.cardId && generation == o.generation && commitId == o.commitId &&
           tagsCrc == o.tagsCrc;
  }
  bool operator!=(const Identity& o) const { return !(*this == o); }
};

// DHDR.
struct DeviceHeader {
  bool walked = false;  // a walk was merged: `walk` and `skew` are its
  Identity walk;
  int32_t skew = 0;     // T's skew the walk found (2.3.4); 0 none
  uint32_t epoch = 0;   // the rescan epoch
};
void encodeHeader(const DeviceHeader& h, uint8_t out[kDhdrBytes]);
bool decodeHeader(const uint8_t* in, uint32_t n, DeviceHeader* h);

// A folder's row, from the walk (N5's cardwalk::FolderRow, field for field):
// its digest (3.2.3; 0 unknown: the next walk merges the folder's files
// again), its best cover image (2.14.3: its name, rank, size and time, and
// whether T's ledger lists it), and its counts of audio files, cover images
// and other files.
struct FolderFacts {
  uint64_t digest = 0;
  uint32_t imageSize = 0;
  uint32_t imageTime = 0;
  uint32_t audio = 0;
  uint32_t images = 0;  // the best one included
  uint32_t others = 0;  // every other file
  uint8_t imageRank = LibraryIndex::kNoImage;
  bool imageOwned = false;
  uint8_t imageLength = 0;  // 0: no cover
  char image[256] = "";
  bool operator==(const FolderFacts& o) const;
  // LibraryIndex's (as cardwalk::factsOf()): the cover's name (pointing into
  // this), its image count and its other files, images included (saturating).
  LibraryIndex::FolderFacts index() const;
};
// A DFLD entry, or a walk Folder entry's tail: kFactsFixed + imageLength bytes.
uint32_t encodeFacts(const FolderFacts& f, uint8_t* out);

// A pair the walk keeps for after it (N5's cardwalk::Doubt): a file whose
// size is T's and whose time isn't, its row given without T until settled
// (settle), or a pair kept only for the skew's recount (not settle).
struct Doubt {
  bool settle = true;
  bool hasDelta = false;  // both stamps valid and non-zero: `delta` is W(the card's) - W(T's)
  int64_t delta = 0;
  uint32_t size = 0;      // the card file's
  uint32_t fatTime = 0;
  uint64_t transferQfp = 0;
  uint64_t deviceQfp = 0;
  Status fallback = Status::Pending;  // the row's status when T doesn't match
};

// What open() reads of D: the frame and the header (the contract's and
// DHDR, DSTA's header); its sections' CRCs are checked by whoever streams
// them (a compaction, a build, the walk).
struct DeviceInfo {
  bool present = false;
  uint32_t headerCrc = 0;
  uint32_t generation = 0;
  cardcontract::mptg::Info tags;
  DeviceHeader header;
  uint32_t ownRecords = 0;
  uint32_t ownFolders = 0;
};
// The frame (2.4.3 with D's sections required), the MPTG header (source 1,
// the counts), DHDR whole and checked, DSTA's and DFLD's headers.
Why openDevice(cardcontract::Source& src, DeviceInfo* out);

// ---------------------------------------------------------------------------
// Reading D.
// ---------------------------------------------------------------------------
// D in canonical order with its device sections in step: a folder and its
// facts, then its records and their rows (the walk, N5, streams this beside
// the card). Everything 2.4.3 asks, and DSTA's and DFLD's CRCs and shape, is
// checked as it goes; a failure is Step::Bad from then on. About 4.6 KB plus
// what it is given: a heap or PSRAM object.
class DeviceReader {
public:
  enum class Step : uint8_t { Folder, Record, End, Bad };
  // `scratch` (at least kMinScratch): most for the MPTG walker's four
  // streams, the rest for DSTA's and DFLD's. `run`, when given, receives each
  // Scanned record's run. `facts` false: DFLD isn't read (folderFacts() is
  // then empty; its CRC unchecked).
  static constexpr uint32_t kMinScratch = cardcontract::mptg::Walker::kMinScratch + 2 * 64;
  Why begin(cardcontract::Source& src, uint8_t* scratch, uint32_t scratchBytes, cardcontract::RunFields* run = nullptr,
            bool facts = true);
  Step next();
  Why why() const { return why_; }
  const DeviceInfo& info() const { return info_; }
  const cardcontract::mptg::Walker& walker() const { return w_; }
  // After Step::Folder: the walker's folder() and path(); its facts.
  const FolderFacts& folderFacts() const { return facts_; }
  // After Step::Record: the walker's record(), path() and run(); its row.
  const Row& row() const { return row_; }

private:
  Step fail(Why w);
  cardcontract::Source* src_ = nullptr;
  cardcontract::mptg::Walker w_;
  DeviceInfo info_;
  cardcontract::Container c_;
  cardcontract::Stream dsta_, dfld_;
  bool factsOn_ = true;
  FolderFacts facts_;
  Row row_;
  Why why_ = Why::Ok;
};

// D's folders in pre-order with their facts, looked up by path in pre-order
// (a build's or a compaction's folders, each asked once): FOLD, the folder
// names (STRS's first run) and DFLD, each through a stream. About 1.8 KB plus
// its buffers. A folder D doesn't hold, or a D that turns out broken, gives
// no facts.
class FolderCursor {
public:
  // `buf`: split in three (at least 3 x 64 bytes).
  bool begin(cardcontract::Source& src, uint8_t* buf, uint32_t bufBytes);
  // The facts of folder `rel` ("" for /music): true when D holds it. Paths
  // asked in pre-order (compareFolderPaths); an earlier path than the last
  // starts over from folder 0.
  bool find(const char* rel, size_t len, FolderFacts* out);
  // Reads the rest and checks DFLD's CRC (FOLD and STRS are the walker's).
  bool finish();
  bool failed() const { return failed_; }

private:
  bool advance();  // to the next folder
  cardcontract::Source* src_ = nullptr;
  uint8_t* buf_ = nullptr;
  uint32_t bufBytes_ = 0;
  cardcontract::Container c_;
  cardcontract::Stream fold_, names_, dfld_;
  uint32_t split_ = 0;
  uint32_t folders_ = 0;
  uint32_t next_ = 0;  // the next folder's index
  bool have_ = false;  // cur is folder next_ - 1
  bool failed_ = false;
  char path_[cardcontract::kMaxRelPath + 1];
  size_t pathLen_ = 0;
  struct Level {
    uint32_t index;
    uint16_t end;
  };
  Level stack_[cardcontract::mptg::Walker::kMaxDepth + 1];
  uint32_t depth_ = 0;
  FolderFacts cur_;
  bool asked_ = false;  // last_ is the path asked last
  char last_[cardcontract::kMaxRelPath + 1];
  size_t lastLen_ = 0;
};

// LibraryBuilder's inputs from D (N2's interfaces): the rows in record order
// (a build that restarts asks from record 0 again), and the folders' facts.
class BuilderRows : public LibraryBuilder::DeviceRows {
public:
  bool begin(cardcontract::Source& src, uint8_t* buf, uint32_t bufBytes);
  LibraryBuilder::Row row(uint32_t record) override;
  bool ownCounts(uint32_t* records, uint32_t* folders) override;

private:
  cardcontract::Source* src_ = nullptr;
  DeviceInfo info_;
  cardcontract::Section dsta_;
  cardcontract::Stream s_;
  uint8_t* buf_ = nullptr;
  uint32_t bufBytes_ = 0;
  uint32_t next_ = 0;
  bool ok_ = false;
};

class BuilderFacts : public LibraryBuilder::FolderFactsSource {
public:
  bool begin(cardcontract::Source& src, uint8_t* buf, uint32_t bufBytes) { return cursor_.begin(src, buf, bufBytes); }
  bool facts(const char* rel, size_t len, LibraryIndex::FolderFacts* out) override;

private:
  FolderCursor cursor_;
  FolderFacts f_;
};

// ---------------------------------------------------------------------------
// The scan's journal: a chunk of records, built in the caller's buffer
// (PSRAM; 100 records are about 20 KB) and appended by TagStore::append(),
// which sorts it into canonical order (a path added twice keeps its last).
// ---------------------------------------------------------------------------
class ChunkBuilder {
public:
  void begin(uint8_t* buf, uint32_t bytes);
  // One scan result: `rel` relative to /music, `status` Scanned or
  // Unreadable, the record (its size and FAT time the file's as the scan
  // read it; folder, name and strings ignored) and its fields (2.3.6
  // applied: FieldBuilder's). False: no room (append the chunk first), or a
  // path or status that can't be.
  bool add(const char* rel, size_t len, Status status, const cardcontract::mptg::Record& rec,
           const char* const fields[cardcontract::kRunFields]);
  uint32_t count() const { return count_; }
  uint32_t bytes() const { return used_; }  // the entries' bytes
  void clear() { count_ = used_ = 0; }

private:
  friend class TagStore;
  uint8_t* buf_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t used_ = 0;   // entries from the front
  uint32_t count_ = 0;  // their offsets from the back, 4 bytes each
};

// ---------------------------------------------------------------------------
// The walk's journal (N5 writes it through this, behind cardwalk::Sink).
// Three kinds of entries, each kind in its own order and the kinds
// interleaved as the walk gives them (N5 gives a folder's row once audio
// turns up at or below it, maybe after its subfolders' files): file rows and
// gones in canonical order (2.6.8), folder rows and gones in pre-order ("" is
// /music), doubts in canonical order. Each is checked against the one before
// of its kind; one out of order fails the walk (nothing of it counts). A
// doubt changes no row: the walk gives the file's row without T first, and
// its settled row in run 2. Blocks go to the card as the caller's buffer
// fills, each synced; endRun() closes run 1, then run 2, and each closed run
// is the store's at once (readWalk(), a View). Nothing reaches the card when
// the walk found nothing and its identity and skew are D's. A WalkWriter
// left without finish() finishes when it goes.
// ---------------------------------------------------------------------------
enum class EntryKind : uint8_t { Record = 1, File = 2, Gone = 3, Doubt = 4, Folder = 5, FolderGone = 6 };

class WalkWriter {
public:
  ~WalkWriter() {
    if (store_ && run_ <= 2) finish();
  }
  // A file seen at this size and time (`qfp` the device's of it then, 0
  // unknown): Software, T's record covers it (`confirmed` by qfp); Pending,
  // the scan should read it; Scanned or Unreadable, the device's reading of
  // it stands. A reading of the file at this size and time stands for any
  // status but Software (the scan doesn't read it again), and one that isn't
  // there leaves the file Pending.
  bool file(const char* rel, size_t len, uint32_t size, uint32_t fatTime, uint64_t qfp, Status status,
            bool confirmed = false);
  bool gone(const char* rel, size_t len);
  bool doubt(const char* rel, size_t len, const Doubt& d);
  bool folder(const char* rel, size_t len, const FolderFacts& facts);
  bool folderGone(const char* rel, size_t len);
  // Closes the current run (1, then 2), with T's skew as the walk then has
  // it. `unsettled` (run 2): a doubt couldn't be settled (its qfp read
  // failed), so DHDR doesn't take the walk's commit: the next walk asks T
  // again.
  bool endRun(int32_t skew, bool unsettled = false);
  // Done (closes the file; a run left open is dropped, as a cut would).
  bool finish();
  // The walk failed: nothing of it counts (walk.jnl removed).
  void abort();
  bool failed() const { return failed_; }
  uint32_t run() const { return run_; }

private:
  friend class TagStore;
  struct Key {
    bool have = false;
    char path[cardcontract::kMaxRelPath + 1];
    size_t len = 0;
  };
  bool put(EntryKind kind, uint8_t flags, const char* rel, size_t len, const uint8_t* tail, uint32_t tailLen);
  bool flush();  // the block being built, to the card
  bool ensureFile();
  bool writeBlock(uint8_t type, const uint8_t* payload, uint32_t bytes, uint32_t count);
  bool writeEnd(uint32_t run, int32_t skew, uint32_t count, uint32_t flags = 0);
  bool sameAsDevice(int32_t skew) const;
  bool settles_ = false;  // run 1 holds a doubt to settle
  class TagStore* store_ = nullptr;
  File* file_ = nullptr;
  Identity id_;
  uint8_t* buf_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t fill_ = 0;        // the block being built: its payload
  uint32_t blockCount_ = 0;  // its entries
  uint32_t runCount_ = 0;    // the run's entries so far
  uint32_t run_ = 1;
  uint32_t at_ = 0;          // the file's end
  bool failed_ = false;
  bool open_ = false;
  bool deferred_ = false;    // run 1 found nothing new: written only if run 2 finds something
  int32_t skew1_ = 0;
  Key files_, folders_, doubts_;  // the last key of each kind in this run
  uint8_t header_[64];
  uint32_t runStart_[2] = {};
  uint32_t runEnd_[2] = {};
};

// One entry of a walk run, read back.
struct WalkEntry {
  EntryKind kind = EntryKind::File;
  char path[cardcontract::kMaxRelPath + 1] = "";
  size_t pathLen = 0;
  Row row;                         // File
  uint32_t size = 0, fatTime = 0;  // File
  uint64_t qfp = 0;                // File
  Doubt doubt;                     // Doubt
  FolderFacts facts;               // Folder
};

class WalkReader {
public:
  ~WalkReader();
  bool next(WalkEntry* e);  // false: the run's end, or a failed read
  bool failed() const { return failed_; }
  void close();

private:
  friend class TagStore;
  Fs* fs_ = nullptr;
  File* f_ = nullptr;
  cardcontract::Stream s_;
  uint32_t left_ = 0;     // entries left in the block
  bool inBlock_ = false;  // its CRC follows its last entry
  bool done_ = false;
  bool failed_ = false;
};

// ---------------------------------------------------------------------------
// The store.
// ---------------------------------------------------------------------------
class TagStore {
public:
  struct Config {
    const char* dir = "/.player";
    const char* producer = "";      // D's producer string ("mstream-player 0.8.0")
    uint16_t parserVersion = 0;     // the scan's parser (N6)
    uint64_t cardId = 0;            // D's cardId (the root's, when known)
    // The buffers (2026-10-09: 512, 4096 and 512 made every card access of
    // a compaction a single sector: 94 s a compaction at 20k on the device;
    // METADATA.md 3.3.7). The work memory is about 115 KB at these.
    uint32_t runBuffer = 1024;      // each journal run's read buffer
    uint32_t deviceBuffer = 8192;   // D's walker's buffers (its four streams)
    uint32_t writeBuffer = 4096;    // each output section's buffer (whole 4 KB pieces from a 4 KB boundary)
    uint32_t maxChunks = 32;        // chunks a compaction merges (append() refuses more)
    uint32_t compactBytes = 512u * 1024u;  // the journal's size that asks for a compaction
    // A microsecond clock for a compaction's parts (Compacted's ms); nullptr:
    // none (0).
    uint64_t (*nowUs)() = nullptr;
  };

  struct Opened {
    Settled settled;           // tags.tmp's fate (2.12.6); Kept: tmpKept()
    Why deviceWhy = Why::Ok;   // D's frame: Ok, or why it is absent
    uint32_t chunks = 0;       // tags.jnl's chunks that extend D
    bool journalTorn = false;  // a torn or foreign tail, cut off at the next append
    bool journalStale = false; // tags.jnl extends another D: dropped
    bool walk = false;         // walk.jnl has a complete run that extends D
    bool walkStale = false;    // walk.jnl for another D, or cut short: dropped
  };

  struct Compacted {
    bool ok = false;
    const char* error = nullptr;     // why not
    Why deviceWhy = Why::Ok;         // D failed its checks and was left out
    uint32_t records = 0, folders = 0;
    uint32_t chunksMerged = 0;
    uint32_t chunksDropped = 0;      // past maxChunks
    bool walkMerged = false;
    bool rescanned = false;          // Scanned rows turned Pending (another parser or epoch)
    size_t workBytes = 0;            // the memory it took from the hooks
    uint32_t hidxPasses = 0;
    // Where its time went (Config::nowUs; 0 without one): the first pass
    // (the counts, HIDX's pairs to hidx.tmp), the second (the sections
    // written), HIDX's sort, and the whole (with the rest: the opens, DHDR,
    // the header, the sync, the rename).
    uint32_t pass1Ms = 0, pass2Ms = 0, sortMs = 0, totalMs = 0;
  };

  TagStore(Fs& fs, const Config& config, AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~TagStore();
  TagStore(const TagStore&) = delete;
  TagStore& operator=(const TagStore&) = delete;

  // Boot (3.2.2, before anything writes the card): the cut-rename rule for
  // tags.tmp, the twins, hidx.tmp, D's frame, the journals.
  Opened open();

  // ---- what's there ----
  const DeviceInfo& device() const { return dev_; }
  // The soft inputs of library.idx (3.4.3): D's headerCrc and the journal's
  // last sequence (0 when compacted).
  uint32_t deviceCrc() const { return dev_.present ? dev_.headerCrc : 0; }
  uint32_t journalSeq() const { return chunkN_ ? chunks_[chunkN_ - 1].seq : 0; }
  uint32_t chunkCount() const { return chunkN_; }
  uint32_t journalBytes() const { return jnlEnd_; }
  bool hasWalk() const { return walkRuns_ > 0; }
  // The walk to merge leaves a doubt unsettled (its run 1 alone, or a qfp
  // read that failed): the compaction leaves DHDR unwalked.
  bool walkUnsettled() const { return walkRuns_ > 0 && walkUnsettled_; }
  bool hasJournals() const { return chunkN_ > 0 || walkRuns_ > 0; }
  // The journal asks for a compaction (Config::compactBytes, or maxChunks
  // reached), or D was read by another parser.
  bool wantsCompaction() const;
  // Twins on the card: it wants a disk check on a PC (the console and the
  // Library row say so).
  uint32_t twins() const { return twins_; }
  // open() found tags.tmp alone (a cut between replace()'s remove and its
  // rename, or in a first D's write) and had no memory to check it: it is
  // left as it is, D is absent this session, and compact() refuses (its
  // prepareTmp() would remove the tmp) until an open() checks it.
  bool tmpKept() const { return tmpKept_; }
  const Config& config() const { return cfg_; }
  Fs& fs() { return fs_; }
  // The paths.
  const char* devicePath() const { return binPath_; }
  const char* journalPath() const { return jnlPath_; }
  const char* walkPath() const { return walkPath_; }

  // ---- the scan ----
  // Sorts `chunk`, appends it to tags.jnl (synced) and clears it. False
  // (`chunk` kept): the journal holds maxChunks already (compact first), a
  // walk is being written (one job at a time), or a write failed.
  bool append(ChunkBuilder& chunk);

  // ---- the walk ----
  // Starts walk.jnl against `walk` (the root's identity, or none), entries
  // buffered in `buf` (at least 1 KB). False when a walk is still to be
  // merged (compact first).
  bool beginWalk(const Identity& walk, uint8_t* buf, uint32_t bytes, WalkWriter* out);
  // Reads walk run 1 or 2 back (complete runs only), through `buf`.
  bool readWalk(uint32_t run, uint8_t* buf, uint32_t bytes, WalkReader* out);

  // ---- compaction (3.3.2) ----
  // Folds the journals into a new D. `rescan`: a Rescan (3.3.6), the next
  // epoch: every Scanned and Unreadable row turns Pending.
  Compacted compact(bool rescan = false);
  // The memory compact() takes from the hooks (one block, given back), at
  // most; a View takes less.
  size_t workBytes() const;

  // ---- the merged view: D and the journals as one (the scan's to-do, the
  // console). Rows in canonical order; Gone files skipped. ----
  class View {
  public:
    ~View();
    bool next();
    const char* path() const;
    size_t pathLength() const;
    const Row& row() const;
    const cardcontract::mptg::Record& record() const;
    // The row's fields (its record's run; none for a Pending or Software
    // row), once per row, before next().
    bool run(cardcontract::RunFields* out);
    bool failed() const;

  private:
    friend class TagStore;
    Merge* m_ = nullptr;
    TagStore* store_ = nullptr;
    void* mem_ = nullptr;
    uint8_t* runBuf_ = nullptr;
  };
  // Opens `v` over the store as it is (its memory from the hooks, as a
  // compaction's merge). False: no memory, or a file that can't be read.
  bool openView(View* v);

private:
  friend class WalkWriter;
  struct Chunk {
    uint32_t offset;  // the chunk's header
    uint32_t bytes;   // header, payload and CRC
    uint32_t seq;
    uint32_t count;
    uint16_t parser;
    uint16_t rules;
    uint32_t epoch;
  };
  void scanJournal(Opened* o);
  void scanWalk(Opened* o);
  bool loadDevice();
  bool beginMerge(Merge& m, uint8_t* arena, size_t arenaBytes, bool useBin, uint32_t epoch);
  void release(void* p);
  void walkDone(uint32_t runs, const uint32_t* start, const uint32_t* end, int32_t skew, const Identity& id,
                uint32_t seq, bool unsettled);

  Fs& fs_;
  Config cfg_;
  AllocFn alloc_;
  FreeFn free_;
  char binPath_[64], tmpPath_[64], stemPath_[64], jnlPath_[64], walkPath_[64], hidxPath_[64];
  Names names_;
  DeviceInfo dev_;
  uint32_t twins_ = 0;
  bool tmpKept_ = false;  // open(): tags.tmp alone, unchecked (Settle::Kept)
  // tags.jnl
  Chunk* chunks_ = nullptr;  // maxChunks of them
  uint32_t chunkN_ = 0;
  uint32_t extraChunks_ = 0;  // valid chunks past maxChunks (another firmware's): dropped by the compaction
  uint32_t jnlEnd_ = 0;       // the valid chunks' end
  bool jnlTail_ = false;      // bytes past jnlEnd_ to cut off
  bool jnlForeign_ = false;   // the file holds no chunk of this D: emptied at the next append
  // walk.jnl
  uint32_t walkRuns_ = 0;  // complete runs
  uint32_t walkStart_[2] = {}, walkEnd_[2] = {};
  uint32_t walkSeq_ = 0;  // tags.jnl's last sequence when the walk began
  Identity walkId_;
  int32_t walkSkew_ = 0;
  bool walkUnsettled_ = false;  // the walk merged so far leaves a doubt unsettled (run 1 alone with one, or run 2's End)
  bool walking_ = false;
};

}  // namespace tagstore
