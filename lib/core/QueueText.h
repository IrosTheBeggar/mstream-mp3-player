// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "ByteStream.h"
#include "QueueModel.h"
#include "TrackCatalog.h"

// The queue as text, one track path a line, so it outlives a reboot and a
// library rebuild (track ids change when the library does, paths don't):
//
//   mstream-queue 1 <entries> <current> <generation>
//   /music/Daft Punk/Discovery/01 - One More Time.mp3
//   tone:click120
//
// `current` is the current entry's line (0-based, -1: none); `generation`
// is the saver's own number, which tells whether a position saved elsewhere
// (NVS on the firmware) belongs to this file. Portable, host-tested.
//
// Version 2 is a shuffled queue (docs/QUEUE-MODES.md section 2.9), and is
// written only then: the lines in play order, each the entry's rank (its
// place in the own order) in decimal, one space, then the path:
//
//   mstream-queue 2 <entries> <current> <generation>
//   17 /music/Daft Punk/Discovery/01 - One More Time.mp3
//   4 tone:click120
//
// Not shuffled, the file is version 1, byte for byte as before this: a
// unit that never shuffles never changes format, and older firmware still
// reads it. An id the catalog doesn't know writes "17 " (no path), dropped
// at the read like version 1's empty line. A version 2 line that doesn't
// start "<digits> ", or whose rank is past 4294967295, means the file isn't
// whole (as a bad header): the queue is left alone. Older firmware reads
// a version 2 file as not a queue file at all (the queue lost once).
namespace queuetext {

struct Header {
  uint32_t entries = 0;
  int32_t current = -1;
  uint32_t generation = 0;
  bool shuffled = false;  // version 2
};

// Writes a queue a few lines at a time, so a long one (a full queue, 5,000
// tracks, is ~350 KB) doesn't hold the caller's loop. It writes what the queue holds
// at each step: begin() notes the queue's contentVersion(), and a step
// after the queue changed returns Changed (start again later).
class Writer {
public:
  enum class Step : uint8_t { More, Done, Changed, Failed };

  void begin(const QueueModel& q, uint32_t generation);
  // Up to `maxLines` more lines (the header counts as one).
  Step step(const QueueModel& q, const TrackCatalog& catalog, ByteSink& out, uint32_t maxLines);
  uint32_t written() const { return next_; }  // entries so far

private:
  uint32_t version_ = 0;
  uint32_t generation_ = 0;
  uint32_t next_ = 0;
  bool headerDone_ = false;
};

// The whole queue in one go (a small one, or a buffer in memory).
bool write(const QueueModel& q, const TrackCatalog& catalog, uint32_t generation, ByteSink& out);

struct Restored {
  bool ok = false;           // a queue file (false: none, or not one)
  uint32_t lines = 0;        // entries in the file
  uint32_t entries = 0;      // in the queue now
  uint32_t dropped = 0;      // paths the catalog doesn't know any more (of the lines read in)
  // Lines left out by the queue's cap (QueueModel::kMaxEntries): a file
  // longer than the queue holds (an older firmware's whole-library queue),
  // read in from line `first` only. lines = entries + dropped + capped.
  uint32_t capped = 0;
  uint32_t first = 0;
  bool currentKept = false;  // the current line's track is still there
  Header header;
};

// Reads a queue file into `q` (assign(): fresh keys, no undo; version 2:
// shuffled, with the ranks of the lines that survived). Paths the
// catalog doesn't know are dropped; if the current one is among them, the
// next line that survived is current (the last one if none after it did).
// `current` >= 0 overrides the file's own current line (a position saved
// since, for this generation). Reads the header first and calls
// `pickCurrent(header, ctx)` for that override; nullptr: the file's.
// A file of more lines than the queue holds (QueueModel::kMaxEntries:
// written before the cap) is read in as QueueModel::window() of its lines
// and that current line: its first 5,000, or from the current line on
// when that is past them (every line is still checked: a file that isn't
// whole is left alone as ever). The writer never writes more: the queue
// never holds more.
// Nothing changes in `q` unless the result is ok (and memory allowed).
// Memory, from `alloc`/`release` (nullptr: malloc/free): the surviving
// ids and (version 2) their ranks, each a block of exactly the lines read
// in (the header's count, at most the cap), 4 bytes a line, held until
// assign() has copied them; a file with more lines than its header says
// isn't whole either.
Restored read(ByteSource& in, const TrackCatalog& catalog, QueueModel& q,
              int32_t (*pickCurrent)(const Header& h, void* ctx) = nullptr, void* ctx = nullptr,
              MemorySink::AllocFn alloc = nullptr, MemorySink::FreeFn release = nullptr);

}  // namespace queuetext
