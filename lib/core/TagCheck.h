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
// apart, a gapless join taken 980 ms late; docs/METADATA.md 3.8).
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
// and its wait for the worker's step under way first (waitOver()).
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
// compaction (it replaces tags.bin under the reads) and the update step
// stays as asked (its compaction, its fence); covers, the walk and the scan
// go on. None after its last line.
enum class Hold : uint8_t { None, Worker, Records };

// gs's wait for the worker's step under way before its reads: over once
// that step is taken in (`workerBusy` false), or `maxMs` after `startMs`
// (then on regardless, as CardTasks::waitIdle()'s wait). `nowMs` earlier
// than `startMs` (the pass's time, taken before the console ran gs in that
// pass) is no time passed, not a wrapped 49 days.
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

}  // namespace tagcheck
