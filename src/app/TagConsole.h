// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "LibraryText.h"
#include "TagText.h"
#include "app/Library.h"
#include "storage/LocalStorage.h"

// The console's tag commands (the spike's g: gs, gt, gr, gw, gb, gv;
// tagtext has the words and docs/METADATA.md 3.3.6 the design): what the
// card's tag records say, for the device batches (L0-L5). Run on the loop
// task, reading the card directly (seconds for a 20k card's tags files:
// diagnostics, never the player's path).
//
//   gs   the scan's state (the card worker's, when it says), where the
//        index's names came from, D (/.player/tags.bin: its rows by
//        status, its journals), T (the transfer's root and its tags file)
//        and an unfinished transfer's plan
//   gt   one file: its tags read now (TagScan), its records in D and in T,
//        the record the builder would take (LibraryBuilder::choose(), 2.9)
//        and the names the index has for it
//   gr, gr!, gw, gb, gv: the card worker's jobs (Rescan tags, walk, build,
//        verify), handed in by whoever runs it (setJobs(): app/CardTasks);
//        without them they say there is none, and change nothing
//   gc   the PSRAM sector cache under FatFs (storage/SectorDisk): its
//        counts; gc0 off, gc1 on, gc2 on with every hit checked against
//        the card (the device batch's L0 and L1)
//   gl   L0's bench: the card's time per sector, and the opens of a file
//        under /music's 1st, 353rd and 703rd entries uncached, cold and warm;
//        glw also the stock walk (forEachFile) and 3.2.3's walk (CardWalk
//        over FatFs), each uncached and cached (minutes at 20k: playback
//        stopped)
//
// About 40 KB of PSRAM while a command runs (the reader, its buffers, two
// record runs), none between.
class TagConsole {
public:
  struct Jobs {
    bool (*rescan)(bool everything) = nullptr;
    bool (*walk)() = nullptr;
    bool (*build)() = nullptr;
    bool (*verify)() = nullptr;
    // The scan's state now: the status line's (librarytext), and a line of
    // the worker's own for the console ("" for none).
    void (*state)(librarytext::Status* status, char* line, size_t size) = nullptr;
    // gs's lines of the worker's own (its waits, its steps, its stack).
    void (*report)() = nullptr;
    // Waits for the worker's step under way to finish, before a command
    // reads the card's records itself (gs, gt, gl).
    bool (*idle)() = nullptr;
  };

  TagConsole(LocalStorage& storage, Library& library) : storage_(storage), library_(library) {}
  void setJobs(const Jobs& jobs) { jobs_ = jobs; }
  // A parsed g command that is the tags' (Status, Dump, Rescan, RescanAll,
  // Walk, Build, Verify).
  void command(const tagtext::Parsed& p);
  // g's line: where the names came from, and the scan's state.
  void summary();

private:
  struct Work;
  struct Found;
  void status();
  void dump(const char* path);
  void cache(uint32_t n);
  void bench(bool walks);
  void job(const char* what, bool (*fn)());
  void scanLine();
  // `rel`'s record in the tags file at `file` (D when `device`).
  void find(const char* file, const char* rel, bool device, Work& w, Found* out);
  // The transfer's tags file (the root's companion: 2.5.4): its path, and
  // the root's commit.
  bool transferFile(Work& w, char* path, size_t size, uint64_t* commitId, uint32_t* generation);

  LocalStorage& storage_;
  Library& library_;
  Jobs jobs_;
};
