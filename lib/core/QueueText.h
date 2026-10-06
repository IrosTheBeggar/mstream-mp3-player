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

// Writes a queue a few lines at a time, so a long one (10,000 tracks is
// ~700 KB) doesn't hold the caller's loop. It writes what the queue holds
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
  uint32_t dropped = 0;      // paths the catalog doesn't know any more
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
// Nothing changes in `q` unless the result is ok (and memory allowed).
Restored read(ByteSource& in, const TrackCatalog& catalog, QueueModel& q,
              int32_t (*pickCurrent)(const Header& h, void* ctx) = nullptr, void* ctx = nullptr,
              MemorySink::AllocFn alloc = nullptr, MemorySink::FreeFn release = nullptr);

}  // namespace queuetext
