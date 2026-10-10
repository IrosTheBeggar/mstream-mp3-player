// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardWalk.h"
#include "TagStore.h"

// N5's walk (lib/core/CardWalk) over N4's store (lib/core/TagStore): D as
// the walk reads it (cardwalk::Known), and walk.jnl as the walk writes it
// (cardwalk::Sink). Portable, host-tested (test_tag_store_walk: CardWalk on
// a fake FAT tree, through these, into tags.bin). CardJobs (N10) gives
// them to the walk:
//
//   KnownD d;   d.begin(store, scratch, 16384);   (CardJobs' kKnownScratch)
//   WalkSink s; s.begin(store, root's identity, buf, 4096, readBuf, 512);
//   cardwalk::CardWalk::Config c{lister, &d, transfer, &s, firstAfterCommit,
//                                store.device().header.skew, ...};
//
// The store must have no walk left to merge (TagStore::beginWalk(): compact
// first), and nothing else may append, walk or compact until the walk is
// over (one job at a time, 3.3.4).
namespace tagstore {

// D's folders in pre-order with their digests and owned flags, and each
// folder's rows by name, streamed from tags.bin (a DeviceReader). A store
// with no D gives none (every file the walk finds is Added). About 5.3 KB:
// a heap or PSRAM object, like the scratch it is given.
class KnownD : public cardwalk::Known {
public:
  ~KnownD() override { close(); }
  // `scratch` at least DeviceReader::kMinScratch; the walker's four streams
  // take most of it, so at 16 KB (the card worker's) their refills are
  // several sectors each, not one (2026-10-09). False: D couldn't be opened
  // (failed() then says so).
  bool begin(TagStore& store, uint8_t* scratch, uint32_t scratchBytes);
  void close();
  bool nextFolder(cardwalk::KnownFolder* out) override;
  bool nextFile(cardwalk::KnownFile* out) override;
  bool failed() const override { return failed_; }

private:
  Fs* fs_ = nullptr;
  File* f_ = nullptr;
  DeviceReader r_;
  bool have_ = false;  // D is there
  bool held_ = false;  // a step read ahead (the next folder, or the end)
  DeviceReader::Step heldStep_ = DeviceReader::Step::End;
  bool done_ = false;
  bool failed_ = false;
  char folder_[cardcontract::kMaxRelPath + 1] = "";
  char name_[cardcontract::kMaxRelPath + 1] = "";
};

// The walk's output into walk.jnl: its rows, gones and doubts in run 1;
// rewindDoubts() closes run 1 (with D's skew, or none on a new commit, until
// the walk has its own) and reads its doubts back; the settled rows go to
// run 2; finish() closes the run (with the summary's skew, and whether a
// doubt was left unsettled: D then stays unwalked) and the file; abort()
// removes it. About 1.5 KB plus the buffers it is given.
class WalkSink : public cardwalk::Sink {
public:
  // Begins the walk's journal against `walk` (the root's identity, or none).
  // `buf` buffers its blocks (at least 1 KB), `readBuf` reads the doubts back
  // (at least 64 bytes). False: the store has a walk left to merge.
  bool begin(TagStore& store, const Identity& walk, uint8_t* buf, uint32_t bytes, uint8_t* readBuf,
             uint32_t readBytes);
  bool folder(const char* rel, size_t len, const cardwalk::FolderRow& row) override;
  bool folderGone(const char* rel, size_t len) override;
  bool file(const char* rel, size_t len, cardwalk::Change change, const cardwalk::FileRow& row) override;
  bool fileGone(const char* rel, size_t len) override;
  bool doubt(const char* rel, size_t len, const cardwalk::Doubt& d) override;
  bool rewindDoubts() override;
  Read nextDoubt(char* rel, size_t* len, cardwalk::Doubt* d) override;
  bool finish(const cardwalk::Summary& s) override;
  void abort() override;

private:
  TagStore* store_ = nullptr;
  Identity id_;
  WalkWriter w_;
  WalkReader r_;
  uint8_t* readBuf_ = nullptr;
  uint32_t readBytes_ = 0;
  uint32_t doubts_ = 0;  // doubts written
  bool reading_ = false;
};

// N5's folder row as D keeps it (DFLD).
FolderFacts folderFacts(const cardwalk::FolderRow& row);

}  // namespace tagstore
