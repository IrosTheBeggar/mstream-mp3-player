// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardRoot.h"
#include "LibraryBoot.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "TagStore.h"

// The library's boot and its update step (docs/METADATA.md 3.2.2, 3.4.2;
// milestone N12), one portable state machine over the card's files
// (tagstore::Fs: FatFs on the device, CutFs in the host tests), host-tested
// like IdlePolicy (test_library_update). N10 built these in pieces on the
// loop (app/Library, app/CardTasks, main.cpp); they are here now:
//
// The boot (boot(), before the UI): library.tmp left by a cut settled
// (2.12.6: whole, it is taken), the build-at-boot marker, library.idx's
// header, then libraryboot::decide()'s table (3.2.2): load it, build from
// the records (the journals compacted first) and save, or (no records at
// all, or records that don't list the card: no T in use and D never walked)
// walk /music through the caller's walk and save; the marker goes once its
// build is saved. The marker's build out of PSRAM on the boot's fresh
// heap, with library.idx matching the card: that index is loaded instead
// (stale, but a library) and the marker removed, and this session's update
// steps don't write it again for a short PSRAM (bootBuildShort(): the next
// boot would only fail the same build).
//
// The update step (3.4.2), asked by the scan's end, a walk that found new
// files (U11), the console (gb, gb!, g0), the boot's soft-stale index:
//
//   Asked     the scan and new walks hold (Out::holdScan); a walk under way
//             goes on to its end (its walk.jnl must be merged first); the
//             journals are compacted (Out::compact: the card worker's
//             compaction step); then, with the worker free, the safe point
//             reached (nothing plays, or the heard track has the pause's
//             length left and a margin, 20-30 s: safeLeftMs(), and no seek
//             came in the last 2 s), the worker's task up
//             (Out::wantWorker: the caller makes it; no internal RAM for its
//             stack, the step waits here, the index untouched, rather than
//             behind a fence no build can start under) and the card
//             answering (/music, and the records opening), the memory check:
//             free PSRAM plus what the step frees at least 1.1 x the build's
//             estimated peak, and the new track table fitting the old one's
//             block or the largest free one. Short (or gb!), the marker is
//             written and the step ends (Do::Deferred: "Library updates at
//             next boot"); the next boot builds before the UI, on a fresh
//             heap. Before the memory check, the records must list the card
//             (recordsListCard(): T in use, or D walked): D with only what
//             the scan read (the first boot's walk failed, 2026-10-09)
//             would build an index of those files alone, so the step ends
//             there (Do::Failed, Step::unlisted: the index untouched, no
//             marker). Else:
//   Fence     Do::Fence, once: the loop puts the fence up and calls
//             fencedUp() in the same pass (steps 1-3: the queue flushed to
//             queue.txt and its memory given back, the old index hidden from
//             every reader on the loop, Thumbs' pools given back); fencedUp()
//             clears the old index (two don't fit at 20k); then the UI's
//             "Updating library" state (its lists count no rows: readable()
//             is nullptr by then). The queue can't be carried (no memory for
//             it; or the card refused queue.txt and its text in PSRAM would
//             take more than the memory check had to spare(): it is held
//             through the build): cantFence(), and the step defers to the
//             boot.
//   Build     Out::build: ScanScheduler hands the card worker the build (one
//             step at priority 1); buildStarted(), stepBuild() on the worker
//             (the merge of T and D, LibraryBuilder; a T that fails its checks
//             restarts it from D alone; no records, the caller's walk), then
//             buildDone() on the loop. A read that fails meanwhile is the
//             card's (pulled, failing), not a file's checks: on D's files, or
//             on T with no whole build from D after it, the step fails
//             (Step::cardGone: nothing walked, nothing saved; the next boot
//             loads the last library.idx); on T with D read whole, the build
//             from D alone stands and is saved as one that left records out
//             (the next boot finds it soft-stale; T isn't marked bad).
//   Live      Do::Live, once: the loop takes the fence down and calls lived()
//             in the same pass (step 5: the queue read back from queue.txt
//             with the new ids, the readers back, Thumbs' pools back).
//   Save      Out::save: the card worker's save of library.idx (aside, then
//             renamed), then the marker removed if there was one;
//             saveStarted(), stepSave() on the worker, saveDone() on the
//             loop: Do::Saved, once, and the step is over.
// From Fence to the save's end Out::updating holds the worker's background
// jobs (the walk, a compaction, the scan): nothing writes tags.bin or the
// journals while the build streams them, and a compaction asked meanwhile
// (gr, the journal's limits) waits for the save. Out::libraryWrite holds
// IdlePolicy's LibraryWrite from the fence to the save's end (the queue's
// memory is the build's: a power-off's flush then would write it empty).
//
// The fence's rule: while fenced() (fencedUp() to lived()), nothing on the
// loop reads the index (readable() is nullptr; the firmware's
// Library::index() says none, its TrackCatalog has none). The build writes
// it on the worker meanwhile; once it is done and lived(), the loop reads
// the finished one, which nothing changes while the save writes it (the
// scan, whose findings clear Pending flags, waits for the save).
//
// A power cut at any stage leaves a card the next boot reads whole: before
// the save, the old library.idx, its hard inputs still the card's, is
// loaded, and its soft inputs (D's headerCrc, the journal's sequence: the
// compaction changed them) make the scan's end rebuild it (the first one
// after the boot's walk, even with nothing to scan); mid-save,
// library.tmp is removed (not whole) or taken (whole: the cut fell between
// the remove and the rename); after the save, before the marker's removal,
// the boot builds once more. A T that failed its checks is left out of every
// build this session (the builder restarts from D alone, before anything is
// shown); the next boot's build meets it again and does the same. A build
// (the boot's or a step's) that met a read error and still built is saved
// with the journal's sequence kJournalsLeftOut, as one that left records
// out: the next boot loads it soft-stale and its scan's end rebuilds it.
//
// Loop task only, but for stepBuild() and stepSave(), which run on the card
// worker while the loop goes on, and touch only the index (behind the fence),
// the card's files and this object's step state.
class LibraryUpdate {
public:
  using AllocFn = LibraryIndex::AllocFn;
  using FreeFn = LibraryIndex::FreeFn;

  // The device's own files (2.12.6): library.idx is written aside, then
  // replace()d; the marker is an empty file.
  static const tagstore::Names kIndexNames;
  static constexpr const char* kMarker = "/.player/build.req";

  // ---- the safe point (3.4.2) ----
  // The heard track has the pause's length left at least, and a margin: the
  // last step's pause (its fence, Fence to Live on the loop's clock) when
  // one built this session, else the build's estimate from the index's
  // tracks, whichever is longer; kSafeMarginMs more, at least kSafeLeftMs
  // (3.4.2's first figure, the floor) and at most kSafeCapMs. MEASURED
  // (2026-10-09, N11's 19,410 tracks): the fence up 15.5 s dark and idle,
  // 17.8-18.6 s with the tone playing or the screen lit, so about 1 ms a
  // track while audio plays (kPauseUsPerTrack): about 24 s at 20k, 20 s
  // below about 15,000 tracks. And no seek came in the last kSeekQuietMs.
  // A track that ends inside the fence anyway is safe (3.9: it joins the
  // next, or the next waits, paused): the safe point keeps that rare.
  static constexpr uint32_t kSafeLeftMs = 20000;
  static constexpr uint32_t kSafeMarginMs = 5000;
  static constexpr uint32_t kSafeCapMs = 30000;
  static constexpr uint32_t kPauseUsPerTrack = 1000;
  static constexpr uint32_t kSeekQuietMs = 2000;
  // What the safe point wants left of the heard track for a pause of
  // `pauseMs`: pauseMs + kSafeMarginMs, at least kSafeLeftMs, at most
  // kSafeCapMs (a track shorter than that never makes one: the step waits
  // for the queue to stop, or a longer track).
  static uint32_t safeLeftFor(uint32_t pauseMs);
  // The pause a build of `tracks` tracks is expected to take (the estimate
  // before any step ran this session): kPauseUsPerTrack each.
  static uint32_t pauseFor(uint32_t tracks);
  // Nothing plays (stopped, or paused), or it plays with `needLeftMs` left
  // and no seek for kSeekQuietMs. A play waiting for the headphones isn't a
  // safe point: its start, when they connect, needs the path.
  static bool safePoint(bool playing, bool waiting, uint32_t trackLeftMs, uint32_t sinceSeekMs,
                        uint32_t needLeftMs = kSafeLeftMs);
  // This session's: safeLeftFor() the longer of the last step's measured
  // pause (lastPauseMs()) and pauseFor() the index's tracks now.
  uint32_t safeLeftMs() const;
  // The last update step's pause this session (its fence, from Fence to
  // Live, of a step whose build ran whole); 0: none yet.
  uint32_t lastPauseMs() const { return lastPauseMs_; }

  // ---- the memory check (3.4.2; ESTIMATED, L4 measures) ----
  struct Room {
    size_t psramFree = 0;     // free PSRAM now
    size_t psramLargest = 0;  // its largest free block
    size_t indexBytes = 0;    // the index's (memory().total): the step frees it
    size_t trackBlock = 0;    // its track table's block (memory().tracks): kept across the rebuild
    size_t alsoFreed = 0;     // what else the step frees: the queue's entries, Thumbs' pools
    size_t tableBytes = 0;    // the new track table: the builder's reservation (trackSlots())
  };
  enum class Short : uint8_t {
    None,
    Room,   // free PSRAM, with what the step frees, under 1.1 x the estimated peak
    Table,  // the new track table fits neither the old one's block nor the largest free one
    Carry,  // no memory to carry the queue across the build (cantFence())
  };
  struct Verdict {
    Short shortOf = Short::None;
    size_t room = 0;  // what the step has: free, the index's and what it frees
    size_t peak = 0;  // the build's estimated peak: the index and an eighth, the builder's 96 KB
  };
  // The build's peak is the index it replaces and an eighth more (3.4.4
  // measured 1.99 MB for 1.78 MB at 20k), the builder's own (about 58 KB)
  // and its buffers. The track table is one block, the build's biggest: the
  // old one's block is kept across the rebuild (LibraryIndex::
  // keepTrackBlock()) and taken again when the new table fits in it; else
  // the new one needs a free block of its size and a sixteenth more
  // (ESP-IDF's TLSF rounds a request up to its next size class, a
  // thirty-second, before it searches).
  static Verdict roomToBuild(const Room& r);
  // What `r` leaves to spare for the build: the most that may be held
  // through it besides what the check counted, with roomToBuild() still
  // passing (the room over 1.1 x the peak; and when the new track table
  // doesn't fit the old one's block, the largest free block over the
  // table's need, taken as if the held bytes came out of it). 0: none.
  static size_t spareOf(const Room& r);

  struct Config {
    tagstore::Fs* fs = nullptr;          // the card (its library.idx, the marker, T, D)
    tagstore::TagStore* store = nullptr;  // D, open()ed (its recovery done)
    cardroot::Root* root = nullptr;      // the transfer's root (cardroot::read()); not present: none
    LibraryIndex* index = nullptr;       // the one index (its hooks the firmware's PSRAM)
    AllocFn alloc = nullptr;             // the builder's and the build's buffers; nullptr: malloc
    FreeFn release = nullptr;
    const char* musicRoot = "/music";
    // No records at all: /music walked into `index` (its begin() to its
    // finish(), every file Pending: LibraryIndex::kAddPending). The
    // firmware's VFS walk; nullptr: none (no records, no library).
    bool (*walk)(LibraryIndex& index, void* ctx) = nullptr;
    void* walkCtx = nullptr;
    // A microsecond clock for the timings (esp_timer); nullptr: none (0).
    uint64_t (*nowUs)() = nullptr;
  };

  explicit LibraryUpdate(const Config& c);
  const Config& config() const { return c_; }

  // ---- the boot (3.2.2) ----
  struct Booted {
    bool ok = false;                  // the index is ready
    bool noMemory = false;            // it couldn't be loaded or built for PSRAM: no library
    tagstore::Settled tmpSettled;     // library.tmp's fate (2.12.6): Promoted, the cut fell mid-save
    bool marker = false;              // the build-at-boot marker was there
    libraryboot::Saved saved = libraryboot::Saved::Missing;
    libraryboot::Decision decision;
    bool softStale = false;           // loaded, and the scan went on since its build
    bool built = false;               // by the builder (LibraryBuilder::Result below)
    bool walked = false;              // /music walked (no records)
    LibraryBuilder::Result build;
    tagstore::TagStore::Compacted compaction;  // the journals' (when there were any)
    bool compacted = false;           // a compaction ran
    bool journalsLeft = false;        // it failed: built from tags.bin alone (3.8)
    bool saveFailed = false;
    bool markerRemoved = false;
    // A read of T or D failed as the build read them (the card?): what was
    // built is the session's, saved as one that left records out (the next
    // boot loads it soft-stale, and its scan's end rebuilds it).
    bool readErrors = false;
    // The marker's build ran out of PSRAM (on this fresh heap) and
    // library.idx matches the card: loaded instead, the marker removed
    // (bootBuildShort()).
    bool loadedShort = false;
    // The timings (Config::nowUs).
    float peekMs = 0, loadMs = 0, compactMs = 0, buildMs = 0, saveMs = 0;
  };
  Booted boot();

  // ---- the update step (3.4.2) ----
  enum class Phase : uint8_t { Idle, Asked, Fence, Build, Building, Live, Save, Saving };
  // What the loop carries out this pass (each once).
  enum class Do : uint8_t {
    None,
    Deferred,  // the memory check failed (or gb! asked it to): the marker written (Step::markerWritten), the step over
    Failed,    // the card didn't answer (/music, or the records the boot opened): the step over, the index untouched
    Fence,     // steps 1-3: put the fence up, then fenced()
    Live,      // step 5: take it down (the queue read back, the readers back), then lived()
    Saved,     // step 6 ended (lastSave() says how): the step is over
  };
  // What the step waits for before its fence (the console).
  enum class Wait : uint8_t { None, Walk, Compaction, Worker, SafePoint };

  struct In {
    uint32_t nowMs = 0;
    bool workerFree = true;     // no step under way or waiting to be taken in
    bool workerUp = true;       // the worker's task is there (CardWorker::alive()): the build can start at once
    bool walking = false;       // a walk under way (between its steps): its walk.jnl must be merged first
    bool journals = false;      // D has journals, or the scan's chunk waits for tags.jnl
    bool compactFailed = false;  // the last compaction failed (a full card, a walk being written): built from tags.bin alone
    // The safe point.
    bool playing = false;       // the player is Playing
    bool waiting = false;       // ... Waiting for the headphones
    uint32_t trackLeftMs = 0;   // what is left of the heard track (0: not known)
    uint32_t seekSeq = 0;       // the seeks asked for, free-running (PlaybackController::seeks())
    // The memory check: free PSRAM, its largest block, what the step frees
    // besides the index (the queue's entries, Thumbs' pools).
    size_t psramFree = 0;
    size_t psramLargest = 0;
    size_t alsoFreed = 0;
  };
  struct Out {
    Do act = Do::None;
    Wait wait = Wait::None;
    bool holdScan = false;      // asked: the scan and new walks wait (a walk under way goes on)
    bool compact = false;       // a compaction is wanted before the build (the journals)
    // The step waits only for the worker's task (In::workerUp): the caller
    // makes it now (CardWorker::ensure()), and keeps it until the build is
    // handed. Its fence goes up once the task is there, never before.
    bool wantWorker = false;
    bool build = false;         // ScanScheduler::In::build
    bool save = false;          // ScanScheduler::In::save
    bool updating = false;      // ScanScheduler::In::updating: the fence to the save's end
    bool libraryWrite = false;  // IdlePolicy's LibraryWrite: the fence to the save's end
    bool fenced = false;        // the fence is up (Fence to Live): nothing on the loop reads the index
  };

  // Asked (again while one is under way: nothing more). `why` is kept for
  // the log (a literal). `deferToBoot` (gb!, L4.4): the memory check made to
  // fail, so the step writes the marker as a short PSRAM would.
  void ask(const char* why, bool deferToBoot = false);
  bool asked() const { return phase_ != Phase::Idle; }
  const char* why() const { return why_; }
  Phase phase() const { return phase_; }
  // The fence is up: from fencedUp() (the old index cleared) to lived().
  // (In the pass that says Do::Fence the loop still reads the old index
  // while it puts the fence up; in the one that says Do::Live it reads the
  // new one after lived().)
  bool fenced() const { return phase_ == Phase::Build || phase_ == Phase::Building || phase_ == Phase::Live; }
  // The index the loop may read: nullptr behind the fence.
  LibraryIndex* readable() const { return fenced() ? nullptr : c_.index; }

  // Every loop pass.
  Out update(const In& in);
  // The loop put the fence up (Do::Fence): the old index is cleared now.
  void fencedUp();
  // The loop couldn't put it up (no memory to carry the queue across, or
  // its text over spare()): the step defers to the boot as a short PSRAM
  // would (the marker; Do::Deferred at the next update()). Nothing was
  // given back: the index is as it was.
  void cantFence();
  // What the last memory check left to spare (spareOf()): the most the
  // fence may hold through the build besides what the check counted (the
  // queue's text in PSRAM, when the card can't take queue.txt).
  size_t spare() const { return spareOf(room_); }
  // The worker took the build (ScanScheduler handed Job::Build).
  void buildStarted();
  // On the card worker: the build. The fence holds; the index is the build's.
  void stepBuild();
  // On the loop, after it: Live.
  void buildDone();
  // The loop took the fence down (Do::Live).
  void lived();
  void saveStarted();
  // On the card worker: library.idx saved (the index unchanged while it is
  // written), then the marker removed if the step's build was the one it
  // asked for.
  void stepSave();
  void saveDone();

  // ---- what happened ----
  struct Step {
    const char* why = "";
    bool built = false;          // the index is ready (else: no library until the next boot or "Try again")
    bool walked = false;         // no records: /music walked
    bool noMemory = false;
    bool cardGone = false;       // a file that opened before the fence didn't on the worker, or a read of it failed
    bool unlisted = false;       // Do::Failed: the records don't list the card (recordsListCard()): the index untouched
    bool readErrors = false;     // a read failed (cardGone, or T's with D read whole: saved as records left out)
    LibraryBuilder::Result build;
    bool journalsLeft = false;   // the compaction before it failed: tags.bin alone
    LibraryIndex::Inputs inputs;  // what the save writes
    bool saved = false;          // library.idx replaced
    bool markerRemoved = false;
    bool markerWritten = false;  // Do::Deferred: the marker is on the card
    bool markerSkipped = false;  // Do::Deferred, short, after a boot whose marker build was short too: none written
    float buildMs = 0, saveMs = 0;
    uint32_t fenceMs = 0;        // the fence's length (the loop's clock: Fence to Live)
  };
  const Step& last() const { return step_; }
  // The memory check's last verdict (Do::Deferred's reason; gb! says so).
  const Verdict& verdict() const { return verdict_; }
  bool deferForced() const { return deferForced_; }
  // T failed its checks this session (a build or the caller's walk found it):
  // every build leaves it out.
  bool transferBad() const { return transferBad_; }
  void setTransferBad() { transferBad_ = true; }
  // This boot's marker build ran out of PSRAM (Booted::loadedShort): a
  // short memory check this session ends the step without the marker
  // (Step::markerSkipped), which would only make the next boot fail the
  // same build. gb! still writes it.
  bool bootBuildShort() const { return bootBuildShort_; }
  // The steps run, and those deferred to the boot (the console).
  uint32_t steps() const { return steps_; }
  uint32_t deferrals() const { return deferrals_; }

  // ---- shared with the firmware (the boot, the console) ----
  // The track table the build would reserve (roomToBuild()'s tableBytes):
  // as the build configures the builder; no records, the index's own size.
  uint32_t trackSlots() const;
  // Writes the marker (an empty file). False: the card wouldn't take it.
  bool writeMarker();
  // Whether the card answers for a build now: /music, and the records the
  // boot found (T while it isn't bad, D) opening.
  bool cardAnswers();
  // Whether a build from the card's records, after the compaction an update
  // step makes first, would list every file on it: T in use (it lists the
  // card until the walk at its commit); or a walk to merge, or D walked (a
  // walk listed the card into it); or no records at all (the build walks
  // /music). False: no T, and D (or the journal it would be compacted
  // from) holds only what the scan read: the first boot's walk failed, or
  // hasn't merged. A build then would drop every other file (2026-10-09:
  // 19,410 tracks to 204), so the update step refuses (Step::unlisted).
  bool recordsListCard() const;
  // Whether a walk listed the card into its records, or T does: T in use, a
  // walk to merge, or D walked. recordsListCard() less its "no records at
  // all" (the build walks /music): on a fresh card whose first walk failed
  // that holds only until the scan's first chunk reaches the journal. So
  // after a failed walk CardTasks asks no update step, and says so, until
  // this is true (a walk that lists the card asks).
  bool walkListsCard() const;

private:
  // The build of the records (T and D), or the walk (none): into the index.
  // `update`: the update step's (its files opened on the worker).
  // `readErrors`: a read of T or D failed (whatever came of it).
  bool buildIndex(bool update, LibraryBuilder::Result* r, bool* walked, bool* noMemory, bool* cardGone,
                  bool* readErrors);
  bool records() const;
  // No T in use and D there but never walked: a build lists only its rows.
  bool deviceUnlisted() const;
  void compactFirst(tagstore::TagStore::Compacted* c, bool* ran);
  bool save(const LibraryIndex::Inputs& inputs);
  float msSince(uint64_t t0) const;
  uint64_t now() const { return c_.nowUs ? c_.nowUs() : 0; }

  Config c_;
  Phase phase_ = Phase::Idle;
  const char* why_ = "";
  bool deferAsked_ = false;
  bool deferForced_ = false;
  bool transferBad_ = false;
  bool bootBuildShort_ = false;
  bool markerSet_ = false;      // the marker is on the card (the boot's, or a deferral's)
  bool primed_ = false;
  uint32_t lastSeekSeq_ = 0;
  uint32_t lastSeekMs_ = 0;
  bool seekSeen_ = false;       // a seek since the start (lastSeekMs_ means something)
  uint32_t fenceAtMs_ = 0;
  uint32_t lastPauseMs_ = 0;    // the last step's fence (a whole build's): the safe point's measure
  bool liveDue_ = false;        // the build ended: Do::Live
  bool savedDue_ = false;       // the save ended: Do::Saved
  bool deferredDue_ = false;    // cantFence(): Do::Deferred
  Room room_;                   // the last memory check's inputs (spare())
  Verdict verdict_;
  Step step_;
  uint32_t steps_ = 0, deferrals_ = 0;
};
