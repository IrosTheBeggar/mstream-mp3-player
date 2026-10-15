// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CardContainer.h"
#include "CardTags.h"
#include "TagStore.h"

// A tags file checked whole, a bounded piece at a time: the console's gs
// (app/TagConsole runs a step each loop pass). What gs did in one call on
// the loop task, for D (/.player/tags.bin) and T (the transfer's tags file):
//   - D's frame and header (tagstore::openDevice());
//   - the walk mptg::check() makes, HIDX included (mptg::Walker, its end's
//     HIDX check in pieces: Walker::setEndPiece());
//   - mptg::File::open() (T's line reads its info());
//   - D's rows by status (tagstore::RowTally: DSTA's CRC as they pass).
// The same answers (why the file is absent, or Ok; D's header and rows);
// only the reads are spread. In one call it held the loop 12.4 s on a
// synthetic 19,412-record card (2026-10-14: the 5 s [stats] line 16.9 s
// apart, a gapless join taken 980 ms late; docs/METADATA.md 3.8). A piece a
// pass, gs took 32 s there with the screen lit and 60 s dark, paused, and
// 52-54 s lit, 72-75 s dark while 1-second tracks played (MEASURED the same
// day, gs's command to its D line, all 19,412 records Scanned: the loop's
// 1-5 ms and 20 ms sleeps between pieces, the decoder's reads, the
// worker's steps; fewer Scanned rows read faster).
//
// A step runs units until Config::sliceUs has passed on Config::nowUs, each
// unit whole, the next one started only while the last one's time still
// fits (CardJobs' slices); no clock, or a slice of 0: one unit a step (the
// host tests'). A unit: D's header; the walk's begin; one walker step (a
// folder, or a record with its name and run: a refill of a stream or two);
// kHidxPiece of HIDX's entries at the walk's end; File::open(); RowTally's
// begin; kRowsPiece of DSTA's rows. At 19,412 records (test_tag_check,
// where the one call read 3.7 MB at once) a unit reads 5.2 KB at most, in
// three reads at most, but for the four opens (D's header, the walk's
// begin, File::open(), the rows' begin): 27 to 39 reads each, most of them
// small ones of the header and the section table, within 1 to 11 of the
// card's sectors. A Source read through the VFS took about 4.8 ms on the
// device (2,555 reads in 12.4 s, most of them a stream's 1.6 KB refill).
// Portable; the memory is the caller's (PSRAM on the device), the reads
// its Source's. Also what gs holds of the card worker meanwhile (Hold),
// its wait for the worker's step under way first (waitOver()), what it
// knows of the loop each pass (Pass: no piece while a list moves on a lit
// screen, pieceWaits()), each file's turn (Turn: never read while a
// compaction replaces it, read again if one did or the card was mounted
// again), and gs itself over the passes (Gs: app/TagConsole does its reads
// and prints its lines, GsIo).
namespace tagcheck {

// HIDX's entries a unit at the walk's end: 3 KB, about two refills of the
// walker's HIDX stream (8 KB of scratch in five streams).
constexpr uint32_t kHidxPiece = 256;
// DSTA's rows a unit: a byte each, 2 KB (at most one refill of a 4 KB
// buffer).
constexpr uint32_t kRowsPiece = 2048;

// What gs holds of the card worker while it runs (app/CardTasks::
// holdForConsole()). Worker while it waits for the step under way: nothing
// is handed, so that step is the last before its reads, and the scheduler's
// and the update step's waits stay as they were when gs came (its scan line
// says what it said when gs blocked the loop). Records while it reads: no
// new compaction (it replaces tags.bin under the reads; one already under
// way is waited out: Turn) and the update step stays as asked (its
// compaction, its fence); covers, the walk and the scan go on. None after
// its last line.
enum class Hold : uint8_t { None, Worker, Records };

// gs's wait for the worker's step under way before its first lines: over
// once that step is taken in (`workerBusy` false), or `maxMs` after
// `startMs` (then on regardless, as CardTasks::waitIdle()'s wait; a
// compaction still under way then is waited out before D's reads: Turn).
// `nowMs` earlier than `startMs` (the pass's time, taken before the
// console ran gs in that pass) is no time passed, not a wrapped 49 days.
bool waitOver(bool workerBusy, uint32_t startMs, uint32_t nowMs, uint32_t maxMs);

struct Memory {
  cardcontract::mptg::Walker* walker = nullptr;  // 3.9 KB
  cardcontract::mptg::File* file = nullptr;      // opened at the walk's end: the caller reads info()
  tagstore::RowTally* rows = nullptr;            // D's
  uint8_t* scratch = nullptr;                    // the walker's streams (Walker::begin())
  uint32_t scratchBytes = 0;
  uint8_t* buf = nullptr;                        // the rows' stream
  uint32_t bufBytes = 0;
};

struct Config {
  uint32_t sliceUs = 0;
  uint64_t (*nowUs)() = nullptr;
  // 0: all in one unit (the one call's reads).
  uint32_t hidxPiece = kHidxPiece;
  uint32_t rowsPiece = kRowsPiece;
};

class Check {
public:
  enum class Step : uint8_t { Working, Done };
  // `device`: D (its header first, its rows once the file is valid).
  void begin(cardcontract::Source& src, bool device, const Memory& m, const Config& c = Config());
  // One step (a slice of units). Done: the answers below are final.
  Step step();
  bool done() const { return phase_ == Phase::Done; }
  // Ok, or why the file is absent (its header, the walk, File::open()).
  cardcontract::Why why() const { return why_; }
  bool valid() const { return valid_; }
  // D's header (openDevice()'s); D's rows by LibraryBuilder::Status, all 0
  // for T, for a D that isn't valid, or when DSTA fails.
  const tagstore::DeviceInfo& device() const { return device_; }
  const uint32_t* rows() const { return rows_; }
  uint32_t units() const { return units_; }
  uint32_t steps() const { return steps_; }

private:
  enum class Phase : uint8_t { Header, Begin, Walk, Open, RowsBegin, Rows, Done };
  void unit();
  bool goesOn();

  cardcontract::Source* src_ = nullptr;
  Memory m_;
  Config c_;
  bool isDevice_ = false;
  Phase phase_ = Phase::Done;
  cardcontract::Why why_ = cardcontract::Why::Ok;
  bool valid_ = false;
  tagstore::DeviceInfo device_;
  uint32_t rows_[4] = {};
  uint32_t units_ = 0, steps_ = 0;
  uint64_t sliceAtUs_ = 0, unitAtUs_ = 0;
};

// What gs knows of the loop at a pass (app/TagConsole, from CardTasks, the
// card's guard and the UI, read at the pass's start).
struct Pass {
  uint32_t nowMs = 0;
  bool workerBusy = false;  // a step under way, not taken in yet (CardWorker::busy())
  // ... and it's a compaction: it writes tags.tmp and puts it in tags.bin's
  // place at its end (CardTasks::replacing()). D's alone: nothing in the
  // firmware writes T (/.mstream).
  bool replacing = false;
  // The compactions handed so far, free-running (CardTasks::compactions()).
  uint32_t compactions = 0;
  // FatFs's mounts of the card again so far, free-running (the card's
  // guard: sectordisk::guard().remounts; a card that stopped answering, or
  // pulled and put back): a file opened before one is gone (its reads
  // fail), D or T.
  uint32_t remounts = 0;
  bool listMoving = false;  // a list moves (ui::Ui::listMoving())
  bool dark = false;        // nothing is drawn: the screen dark (ScanScheduler::In::dark)
};

// No piece of gs this pass: a list moves on a lit screen (ScanScheduler::
// listHolds(), the scan's rule). In the dark none is seen moving, whatever
// its page owes the wake (2026-10-14: a list reloaded in the dark held the
// library update's save 260 s, and gs's pieces used the same gate).
bool pieceWaits(const Pass& p);

// gs waits this long at most for a compaction under way before D's check,
// then leaves D unread (never calling it absent): the compactions of N11's
// card took 33-54 s on 2026-10-14 (a scan's, a Rescan's), and 107 s at the
// longest before 3.3.7's work.
constexpr uint32_t kReplaceWaitMs = 300000;
// A file replaced as gs read it (or its handle gone) is read again, this
// many times at most.
constexpr uint32_t kRereads = 2;

// One file's turn in gs (D's, then T's), a pass at a time: never read while
// a step that replaces it is under way, and its answer kept only if nothing
// replaced it, and the card wasn't mounted again, as it was read. A
// compaction writes tags.tmp and puts it in tags.bin's place at its end;
// gs's wait for the worker's step (waitOver()) ends at kWaitMs, and
// Hold::Records stops only new compactions, so one under way past that
// wait (33-48 s on 2026-10-14) was read under: gs printed "D
// /.player/tags.bin: ABSENT, it fails its checks (string)" 24 ms after
// "[card] compaction done", for a D the next gs read whole. Now: D's turn
// waits for a compaction under way (Said at its first pass: gs says so;
// kReplaceWaitMs at most); and at the pass after the check's last read, a
// compaction handed since the open or under way (D's), or a remount since
// the open (either file's: the open handle's reads failed), and the file is
// read again (Again; kRereads times at most). T's turn starts afresh: no
// compaction replaces it, and D's give-up isn't its.
class Turn {
public:
  enum class Now : uint8_t {
    Wait,     // a compaction under way (D's turn): nothing this pass
    Said,     // ... the wait's first pass: gs says it waits
    Open,     // open the file and begin `check` on it, then opened() (not
              // there: the turn ends with the file's line)
    Working,  // a step of the check was made (or, the pass after its last,
              // none: its answer is judged at the next)
    Again,    // the check ended on a file replaced, or a handle gone, as it
              // was read: its answer doesn't count (close it, say so); read
              // again
    Done,     // the check ended and its answer stands: close it, its lines
    GaveUp,   // not read: a compaction still under way after
              // kReplaceWaitMs, or Again past kRereads (close it; never
              // "absent")
  };
  // A file's turn, nothing waited or given up: D's (`compacted`: a
  // compaction replaces it) or T's (only a remount counts).
  void begin(bool compacted) {
    *this = Turn();
    compacted_ = compacted;
  }
  // A pass of the turn (`check`: the file's, begun after Open).
  Now pass(const Pass& p, Check& check);
  // The file opened and `check` begun on it, at pass `p`.
  void opened(const Pass& p);
  // Why it gave up: the wait ran out (else the rereads).
  bool waitedOut() const { return waitedOut_; }
  uint32_t rereads() const { return rereads_; }
  // The last Again (or the give-up after it) was the card mounted again,
  // not a compaction.
  bool remounted() const { return remounted_; }

private:
  bool compacted_ = true;
  bool reading_ = false;  // the check under way
  bool ended_ = false;    // ... its last read made: judged at the next pass
  bool waiting_ = false;  // a compaction under way since waitMs_
  bool gaveUp_ = false, waitedOut_ = false, remounted_ = false;
  uint32_t waitMs_ = 0;
  uint32_t compactionsAt_ = 0, remountsAt_ = 0;  // the Pass's at the open
  uint32_t rereads_ = 0;
};

// gs's side of the card and the console, a call in a pass's piece
// (app/TagConsole's: the VFS and Serial; test_tag_check's: a card in
// memory). Gs decides; this reads and prints.
class GsIo {
public:
  virtual void hold(Hold h) = 0;  // app/CardTasks::holdForConsole()
  // The lines that read no tags file (the scan's, the worker's, the
  // cache's, the index's names).
  virtual void firstLines() = 0;
  // The reads' memory taken (PSRAM on the device: check() is in it); false,
  // none (its line said so: gs ends).
  virtual bool memory() = 0;
  virtual Check& check() = 0;
  // D's file (`device`) or T's opened and check() begun on it; false, it
  // isn't there.
  virtual bool open(bool device) = 0;
  virtual void close() = 0;  // the file open() opened
  // Its lines: check()'s answers (`read`), or that it isn't there.
  virtual void lines(bool device, bool read) = 0;
  // Turn's Said, Again and GaveUp, a line each (`t`: why). Never "absent".
  virtual void waiting(bool device) = 0;
  virtual void again(bool device, const Turn& t) = 0;
  virtual void notRead(bool device, const Turn& t) = 0;
  virtual void journals() = 0;  // D's journals' line
  // T's root: true, it names a tags file (T's turn next); false, there's
  // none (its line said so).
  virtual bool root() = 0;
  virtual void plan() = 0;  // an unfinished transfer's plan: gs's last line
  virtual void end() = 0;   // the memory given back

protected:
  ~GsIo() = default;
};

// gs over the loop's passes, a phase a piece: its first lines once the
// worker's step under way is taken in (waitOver(), `waitMs`; the worker
// held meanwhile: Hold::Worker), then, under Hold::Records, D's turn, its
// journals, T's root and T's turn, the plan; Hold::None after its last
// line. D's and T's phases take as many pieces as their turns. The caller
// runs a piece a pass while it isn't idle(), none while pieceWaits().
class Gs {
public:
  enum class Phase : uint8_t { Idle, Wait, Device, Journals, Root, Transfer, Plan };
  // A gs from `startMs`, the worker held; its first piece in the same pass
  // (step()).
  void begin(uint32_t startMs, uint32_t waitMs, GsIo& io);
  // A piece (nothing when idle). `p.nowMs` may be earlier than the start
  // (the pass's time, taken before the console ran gs: waitOver()).
  void step(const Pass& p, GsIo& io);
  bool idle() const { return phase_ == Phase::Idle; }
  Phase phase() const { return phase_; }

private:
  // A pass of D's or T's turn: true, it's over (its line out).
  bool file(const Pass& p, bool device, GsIo& io);
  void end(GsIo& io);

  Phase phase_ = Phase::Idle;
  uint32_t startMs_ = 0, waitMs_ = 0;
  Turn turn_;
};

}  // namespace tagcheck
