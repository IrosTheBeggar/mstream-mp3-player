// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "CardContract.h"
#include "CardWalk.h"
#include "ScanScheduler.h"
#include "TagScan.h"
#include "TagStore.h"
#include "TagStoreWalk.h"

// The card worker's background jobs (docs/METADATA.md 3.2.3, 3.3.1-3.3.6;
// milestone N10): the validation walk, the compaction of D's journals and
// the tag scan, over N4-N6 (TagStore, CardWalk, TagScan), one step at a
// time. Portable, host-tested (test_card_jobs) on FakeFat's trees and
// CutFs; the firmware runs it on the card worker (app/CardTasks), the loop
// handing each step as ScanScheduler says.
//
// One step at a time, never two at once: the loop calls prepare() (the
// step's memory, the file it names), the worker step(), then the loop
// finish() (what the step did). Between finish() and the next prepare()
// the loop may read the store and flush the scan's chunk; while a step runs
// it touches nothing here but cutSlice().
//
// A handed step of the walk or of the scan's rest is a slice (2026-10-09):
// its units (a CardWalk step; a file of the rest) one after another until
// the slice (Config::sliceUs, setSlice()) has passed on Config::nowUs, each
// unit whole, the next one started only when the last one's time still
// fits (so a slice ends before the loop's pass that hands the next: 3.3.9).
// The loop's cutSlice() ends it after the unit under way (a wait came:
// input, the ring, a track change...). No clock or a slice of 0: one unit a
// step (the host tests' default). A file a loop source names is a step of
// its own.
//
// The jobs:
//   - The walk (3.2.3): CardWalk over KnownD and WalkSink (TagStoreWalk),
//     against T streamed on the first walk after a commit (D's walk
//     identity isn't the root's) and through its HIDX otherwise. A unit is
//     a folder listing (or one pass of a big folder), or the walk's end, or
//     a few doubts with at most one qfp read; the first step also opens its
//     files. A T that
//     fails its checks as it streams fails the walk (CardWalk's
//     Error::Transfer): it is walked again without T, and T counts as
//     absent for the rest of the session (the builder would restart
//     without it too). A walk needs the last one merged first (TagStore's
//     rule): compactWork() asks for that compaction before walkWork().
//   - A compaction (3.3.2): one step, TagStore::compact() (a Rescan's when
//     asked: the next epoch, every Scanned and Unreadable row Pending).
//   - The scan (3.3.1, 3.3.3): a file a unit. Its file comes from the
//     loop (the playing track, the queue, the Library tab: Pending tracks
//     of the index, prepare()'s path: one file, the step) or from the rest
//     of D's to-do, the Pending rows of TagStore's merged view (a View, kept
//     open across steps, skipping what the scan read since it opened: a
//     slice of files, at most kSliceFiles, each in file()). The file is
//     opened, read by TagScan through a 4 KB buffer, and its record added
//     to the journal's chunk (Scanned, or Unreadable when it isn't a file
//     of its kind); the chunk goes to tags.jnl every chunkFiles files or
//     chunkMs after its first, and before any other job's step (N7's rule:
//     the walk's and the compaction's first step flush it). A read that
//     fails isn't recorded: the file is read again at the next boot. A file
//     whose size isn't the walk's any more was changed since: left for the
//     next walk. The rows are taken at their size and FAT time: D's row's
//     for the rest (the walk's sight of it), f_stat's for a file the loop
//     names.
//
// When the card refuses (full, or pulled: there is no card-detect), nothing
// is tried again pass after pass (N10's review): a compaction that failed
// isn't handed again until it is asked again (askCompact(), the update
// step, the next boot), the walk's merge included; the loop's flush of a
// chunk the card refused waits Config::retryMs (idleFlush()). What the
// scan read counts for the update step only once it reached tags.jnl
// (newRecords()): reads the card couldn't take don't make an update that
// would find them Pending again.
//
// Memory, from the hooks (PSRAM on the device), only while a job has
// work: the walk about 98 KB (CardWalk's 64 KB scratch, D's and the
// journal's buffers, T's streams), the scan about 93 KB (TagScan's
// Scanner, its buffer, the chunk, the run for the overlay, the read set,
// a slice's list of files) plus the View's merge memory, the compaction
// TagStore's own.
namespace cardjobs {

using Job = ScanScheduler::Job;
using Source = ScanScheduler::Source;
using AllocFn = void* (*)(size_t bytes);
using FreeFn = void (*)(void* p);

// The card as the jobs read it: CardWalk's lister (the folders, the files
// for a qfp or a scan) and a file's size and FAT time (f_stat on the
// device), for a file the loop names.
class Card : public cardwalk::Lister {
public:
  virtual bool stat(const char* rel, size_t len, uint32_t* size, uint32_t* fatTime) = 0;
};

struct Config {
  tagstore::TagStore* store = nullptr;  // D, open()ed (its recovery done)
  Card* card = nullptr;
  tagstore::Fs* fs = nullptr;           // T's file (the store's file system)
  const char* transferPath = nullptr;   // T: the root's tags file (cardroot); nullptr: none
  tagstore::Identity root;              // the root's identity (present false: none)
  AllocFn alloc = nullptr;              // nullptr: malloc / free
  FreeFn release = nullptr;
  // The scan's chunk (3.3.2): appended every this many files, or this long
  // after its first, or once it holds chunkBytes of records.
  uint32_t chunkFiles = 100;
  uint32_t chunkMs = 5000;
  uint32_t chunkBytes = 32 * 1024;
  // The files the scan read that an open View hasn't passed yet (the
  // playing track's, the queue's): its read set's slots, a power of two.
  uint32_t readSlots = 4096;
  // A step of the rest looks at most this many of the View's rows.
  uint32_t rowsPerStep = 512;
  // A chunk the card refused is offered again from the loop (idleFlush())
  // after this long, not every pass: each try is a FatFs open (on a pulled
  // card about 1 s of the SD driver's retries) and a write.
  uint32_t retryMs = 30000;
  // The slice (above): a handed step of the walk or of the scan's rest runs
  // its units until this long has passed on `nowUs` (the worker's clock,
  // esp_timer on the device). 0, or no clock: one unit a step. The loop may
  // change it between steps (setSlice(): longer while the screen is dark).
  uint32_t sliceUs = 0;
  uint64_t (*nowUs)() = nullptr;
  // Whether the index lists a file (`rel` relative to /music), asked on the
  // worker for each file the walk adds to D: the walk's news is the files
  // new to the index (Done::newToIndex), not those new to D (an index
  // built from T, or walked from /music, lists them before D does).
  // nullptr: every added file is new.
  bool (*indexed)(const char* rel, size_t len, void* ctx) = nullptr;
  void* indexedCtx = nullptr;
};

// What the scan's rest reads: the Pending rows (the scan); every audio row
// (Rescan everything, the console's gr!: a diagnostic, the transfer's files
// read too); the Software rows' qfp against T's (Verify, gv: nothing
// written).
enum class Mode : uint8_t { Normal, All, Verify };

// Verify's counts (gv).
struct Verified {
  uint32_t checked = 0;   // Software rows asked
  uint32_t equal = 0;     // the file's qfp is T's
  uint32_t differ = 0;    // ... isn't
  uint32_t noQfp = 0;     // T's record has none (0), or T has no record of it
  uint32_t failed = 0;    // the file couldn't be read
};

// The most files a slice of the scan's rest takes (its list for the loop,
// Jobs::file(), is in the scan's PSRAM: about 8 KB).
constexpr uint32_t kSliceFiles = 32;

// A file a scan step took (Jobs::file()): what the loop learns of it.
struct FileDone {
  const char* rel = "";  // relative to /music
  size_t relLength = 0;
  bool read = false;       // read (its record in the chunk unless `readError`), else skipped
  bool readError = false;
  // Verify's (gv): its qfp checked against T's (`read`: the file could be
  // read), no record. The step's own: a slice that verifies the last files
  // and reaches the rest's end has Mode::Normal again by finish().
  bool verified = false;
  tagscan::Result result = tagscan::Result::Ok;
};

// What the last step did (finish()). The scan's fields below `handled` are
// the step's last file's; file() has each of a slice's.
struct Done {
  Job job = Job::None;
  // ---- the walk ----
  uint32_t walkSteps = 0;  // CardWalk's steps in this one (a slice's)
  bool walkEnded = false;  // Done or Failed (`walk` says which)
  bool walkRetried = false;  // T failed its checks: the walk starts again without it
  cardwalk::CardWalk::Result walk;
  uint32_t newToIndex = 0;   // of walk.added, the files the index doesn't list (Config::indexed)
  // ---- a compaction ----
  bool compacted = false;
  tagstore::TagStore::Compacted compaction;
  // ---- the scan ----
  uint32_t files = 0;    // files taken this step (file(0) to file(files - 1)): a loop source's 1, a slice's up to kSliceFiles
  bool handled = false;  // a file was taken: read, or skipped (`read` says which)
  bool read = false;     // ... read, its record in the chunk (Scanned or Unreadable) unless `readError`
  bool readError = false;
  bool verified = false;  // ... verified (gv: FileDone::verified)
  Source source = Source::None;
  char rel[cardcontract::kMaxRelPath + 1] = "";
  size_t relLength = 0;
  tagscan::Result result = tagscan::Result::Ok;
  uint32_t reads = 0;  // TagScan's source reads for it
  bool appended = false;      // a chunk went to tags.jnl this step
  bool appendFailed = false;  // it couldn't (a compaction is asked; the chunk kept)
  bool restEnded = false;     // the View reached its end (the scan's rest is done)
  bool restFailed = false;    // ... because D or a journal couldn't be read (the card?), or no memory
  bool verifyEnded = false;   // ... in Verify (verified() has the counts)
};

class Jobs {
public:
  explicit Jobs(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~Jobs();
  Jobs(const Jobs&) = delete;
  Jobs& operator=(const Jobs&) = delete;

  // The store, the card, T. Call again after a remount (nothing may be
  // under way): what the rest has to do is looked at again, the View
  // closed, a Verify or Rescan's mode dropped; the counts, a chunk waiting
  // and the records' mark (newRecords()) are kept.
  void begin(const Config& c);

  // ---- what there is to do (the loop, between steps) ----
  // The validation walk (the boot's, 2 s after the UI; the console's gw and
  // g0). Asked again while one runs: it runs again after.
  void askWalk();
  // A compaction (before the update step; the console). `rescan`: a Rescan
  // (gr), the next epoch.
  void askCompact(bool rescan = false);
  // The scan's rest in `mode` (Normal: the Pending rows, as after a walk;
  // All: gr!; Verify: gv). It starts over from D's first row; a slice
  // under way ends after its unit (cutSlice()).
  void askRest(Mode mode = Mode::Normal);
  // ScanScheduler::In's flags.
  bool walkWork() const;     // a walk is asked and can start (no walk left to merge)
  bool compactWork() const;  // asked, wanted by the journal, or before a walk
  bool restWork() const;     // the View may have more (unknown until it ends)
  bool walking() const { return walk_ != nullptr; }  // a walk is under way (between its steps)
  Mode mode() const { return mode_; }
  // A chunk waits for tags.jnl.
  bool chunkPending() const;

  // ---- one step ----
  // The step ScanScheduler handed: its memory (false: none; nothing to
  // step), and for a scan from the loop's sources (`source` not Rest) the
  // file, relative to /music. `nowMs`: the chunk's clock.
  bool prepare(Job job, Source source, const char* rel, size_t len, uint32_t nowMs);
  // On the worker: the step (a slice: above).
  void step();
  // From the loop while a step runs: the slice ends after the unit under
  // way (ScanScheduler's Out::cut: a wait came, or a list moves).
  void cutSlice() { cut_.store(true, std::memory_order_relaxed); }
  // The slice from the next step on (Config::sliceUs); the loop, between
  // steps only.
  void setSlice(uint32_t us) { c_.sliceUs = us; }
  // After it (the loop): what it did. The scan's record (record()), its
  // files (file()) and the walk's result are valid until the next prepare().
  const Done& finish();
  // The record the last scan step read last (its fields: viewOf()): the
  // step's last file's (Done::rel).
  const tagscan::Record* record() const;
  // The scan step's file `i` (below Done::files).
  FileDone file(uint32_t i) const;

  // ---- the loop, between steps ----
  // The chunk to tags.jnl now (the idle power-off's shutdown, before the
  // update step). False: it couldn't be written (kept).
  bool flushChunk();
  // The same for a chunk that waited (the loop, with the worker free): not
  // tried again within Config::retryMs of a failed append. False: not
  // written (kept).
  bool idleFlush(uint32_t nowMs);
  // Records reached tags.jnl since markRecords(), or a chunk waits that the
  // journal may still take (no append refused since): the scan's end has
  // something for the update step.
  bool newRecords() const;
  // The update step ran (built, or failed, or deferred to the boot): what
  // the journal had is the index's, or the next boot's.
  void markRecords() { recordedMark_ = counts_.recorded; }
  // Gives back what no job needs (the scan's memory once its chunk is out
  // and its View closed; called when the scheduler has no scan to hand).
  void trim();
  // Right before the update step's build (the loop, the worker free): the
  // View closed (the build compacts D first, which replaces tags.bin) and
  // the chunk out.
  void prepareUpdate();
  // The index was built again (the update step): the scan's read set and
  // View start over (rows read since are Scanned in D now).
  void libraryRebuilt();

  // ---- for the console and the status line ----
  struct Counts {
    uint32_t walks = 0, walkSteps = 0, walksFailed = 0;
    uint32_t compactions = 0, compactionsFailed = 0;
    uint32_t scanned = 0;      // files read (since boot)
    uint32_t unreadable = 0;   // ... of those, Unreadable
    uint32_t partial = 0;      // ... Partial (the read budget)
    uint32_t readErrors = 0;   // reads that failed (read again next boot)
    uint32_t skipped = 0;      // changed since the walk, gone, or not audio
    uint32_t chunks = 0;       // appended
    uint32_t recorded = 0;     // records in them
    uint32_t appendFailures = 0;
    uint32_t viewsOpened = 0;
    uint32_t rowsLooked = 0;   // the View's rows passed
  };
  const Counts& counts() const { return counts_; }
  const Verified& verified() const { return verified_; }
  bool transferBad() const { return transferBad_; }

private:
  struct WalkWork;
  struct ScanWork;
  void stepWalk();
  void stepCompact();
  void stepScan();
  // The slice: its start, and after each unit whether another fits.
  void sliceBegin();
  bool sliceGoesOn();
  void scanRestOne();
  void noteFile();
  bool scanFile(const char* rel, size_t len, uint32_t size, uint32_t fatTime, uint64_t qfp);
  bool verifyFile(const char* rel, size_t len, uint32_t size);
  bool nextRow(char* rel, size_t* len, uint32_t* size, uint32_t* fatTime, uint64_t* qfp, bool* ended);
  bool ensureScan();
  void closeView();
  void endWalk();
  void* alloc(size_t n);
  void release(void* p);
  bool readSetHas(uint64_t h) const;
  void readSetAdd(uint64_t h);
  void readSetClear();
  bool appendChunk();

  AllocFn alloc_;
  FreeFn free_;
  Config c_;
  WalkWork* walk_ = nullptr;
  ScanWork* scan_ = nullptr;
  bool walkAsked_ = false;
  bool compactAsked_ = false;
  bool rescanAsked_ = false;
  bool appendBlocked_ = false;  // the journal is full: a compaction first
  bool compactFailed_ = false;  // the last one failed: not handed again (the journal's, the walk's) until a new ask
  bool appendFailed_ = false;   // the last append failed (at appendFailedMs_): idleFlush() waits
  uint32_t appendFailedMs_ = 0;
  uint32_t recordedMark_ = 0;
  bool restDone_ = false;
  bool restartRest_ = false;  // the View starts over at the next scan step (a walk, a compaction, askRest())
  bool transferBad_ = false;
  Mode mode_ = Mode::Normal;
  // The prepared step.
  Job job_ = Job::None;
  Source source_ = Source::None;
  char rel_[cardcontract::kMaxRelPath + 1] = "";
  size_t relLength_ = 0;
  uint32_t nowMs_ = 0;
  // The slice under way: its start and its last unit's, on Config::nowUs.
  uint64_t sliceAtUs_ = 0, unitAtUs_ = 0;
  std::atomic<bool> cut_{false};
  Done done_;
  Counts counts_;
  Verified verified_;
};

}  // namespace cardjobs
