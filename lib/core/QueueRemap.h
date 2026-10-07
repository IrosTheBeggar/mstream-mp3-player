// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ByteStream.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueSaver.h"
#include "QueueText.h"
#include "TrackCatalog.h"

// The queue carried across a library rebuild, through the queue file on
// the card (docs/METADATA.md section 3.4.2, milestone N3). A rebuild gives
// every library track a new id, and paths don't change, so the queue goes
// across as its paths:
//
//   1. flush: QueueStore::flushNow(), so queue.txt holds the queue as it is
//      now (a write under way finished, an edit not yet written written),
//      its lines the queue's positions;
//   2. free: the queue's entries and undo snapshot given back
//      (QueueModel::release()), so the rebuild has their memory: a
//      whole-library queue of 20,000 is 240-480 KB;
//   3. rebuild: the caller's (Library::rebuild() today; the update step's
//      build in N12);
//   4. re-read: queuetext::read() of queue.txt, its two blocks exactly the
//      header's line count (80 KB at 20,000, 160 KB shuffled), into a
//      block of exactly the queue's size (QueueModel::assign()).
//
// Until this, the remap wrote the queue as text into a PSRAM buffer that
// doubled as it grew, while the old index was still held: about 1.5 MB of
// text at 20,000 lines, 3 MB at the moment of a doubling. It ran out at
// about 15,000 entries (the metascan research, section 6.2). Now nothing
// the size of the queue is held across the rebuild: its peak is the
// re-read's, after the rebuild, about 0.32 MB at 20,000 (0.4 MB shuffled).
//
// What comes across: the entries whose tracks are still there, in the same
// order (shuffled: the same play order with the same ranks, gaps where
// tracks went, and the mode); the current entry, or if its track is gone
// the next one that stayed (the last one if none after it did), as at
// boot; a start point that waited (the resume point after a boot, the
// console's qs) when the current entry is still the same file (its path,
// not its line, says so); nothing of the undo (the keys are new: an undo
// can't name these entries). The saver is told what the card now holds:
// the file is the queue unless tracks were dropped (then it's written
// again, with the next generation, and the resume point follows the
// entry's new line).
//
// The file can hold more than the queue (QueueSaver::keptFile()): after a
// boot or a rebuild that found no library, which left its library tracks
// out, or a queue that couldn't come across a rebuild. Its lines are then
// not the queue's positions, so the read starts at the file's own line,
// as the next boot would (the position saved for it, which the queue's
// moves meanwhile didn't touch): "Try again" after a boot with no library
// brings the whole queue back where it was.
//
// A card that can't take the file (none, full, a write that failed): the
// queue is carried as that text in memory instead, sized exactly (two
// passes over the queue), as before this but without the doubling; past
// what memory holds, the queue is cleared. A rebuild that leaves no
// library (out of memory, a card that went away) isn't the listener's
// queue changing: what survives (the built-in tracks) stays, the file
// isn't rewritten (it keeps the library's tracks for the next boot, or the
// next remap), and playback stops if its track is gone. A queue that can't
// come back (cleared) keeps the mode, and the file keeps the last queue
// saved (the next remap brings it back from its own line).
//
// Portable, host-tested (test_queue). Loop task only: nothing else may
// touch the queue, the saver or the index while it runs.
namespace queueremap {

// What the remap needs of the firmware (app/QueueStore: the card, the
// caller's rebuild).
class Card {
public:
  // Everything saved now (QueueStore::flushNow()). True: the queue file
  // holds the queue as it is now; false: it doesn't (no card, a write that
  // failed).
  virtual bool flush() = 0;
  // The queue file, open for reading; nullptr: it can't be (the card went
  // away during the rebuild).
  virtual ByteSource* openFile() = 0;
  virtual void closeFile() = 0;
  // The rebuild itself; what it returns is the result's `rebuilt`.
  virtual bool rebuild() = 0;

protected:
  ~Card() = default;
};

// How the queue was sent across the rebuild.
enum class Via : uint8_t {
  File,    // the queue file (the rule)
  Memory,  // the card couldn't take it: as text in memory, sized exactly
  None,    // neither: no card for the file and no memory for the text
};

struct Result {
  bool rebuilt = false;       // what Card::rebuild() returned
  Via via = Via::None;
  // The read back: ok, the queue came across (lines, entries, dropped,
  // currentKept: here, the current entry is the same file as before, by
  // its path's hash, as a resume point is); not ok, it was cleared
  // (Via::None; the file couldn't be opened or read whole after the
  // rebuild; no memory for the read).
  queuetext::Restored read;
  bool noLibrary = false;     // the rebuild left no library (its tracks dropped, the file kept)
  bool startCarried = false;  // a start point waited and was set again
  size_t freedBytes = 0;      // what the queue gave back for the rebuild (entries and snapshot)
  size_t textBytes = 0;       // Via::Memory: the text's size
};

// The remap: flush, free, `card.rebuild()`, re-read. `alloc`/`release`:
// the hooks for the re-read's blocks and the fallback text (the firmware's
// PSRAM; nullptr: malloc/free).
Result run(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog, Card& card,
           uint32_t nowMs, MemorySink::AllocFn alloc = nullptr, MemorySink::FreeFn release = nullptr);

}  // namespace queueremap
