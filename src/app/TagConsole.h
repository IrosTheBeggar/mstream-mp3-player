// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "LibraryText.h"
#include "TagCheck.h"
#include "TagText.h"
#include "app/Library.h"
#include "storage/LocalStorage.h"

// The console's tag commands (the spike's g: gs, gt, gr, gw, gb, gv;
// tagtext has the words and docs/METADATA.md 3.3.6 the design): what the
// card's tag records say, for the device batches (L0-L5). Run on the loop
// task, reading the card directly (seconds for a 20k card's tags files:
// diagnostics, never the player's path). gs reads them a piece a loop pass
// (loop()); gt and gl in one go.
//
//   gs   the scan's state (the card worker's, when it says), where the
//        index's names came from, D (/.player/tags.bin: its rows by
//        status, its journals), T (the transfer's root and its tags file)
//        and an unfinished transfer's plan. The lines that read no card
//        first, once the worker's step under way is taken in; then D's and
//        T's as their checks end (lib/core TagCheck: D and T checked
//        whole, about kSliceUs of it a pass, the pieces whole, none while
//        a list moves on a lit screen; 12.4 s of the loop in one go on a
//        19,412-record card before, 2026-10-14; 32 s lit and 60 s dark
//        there a piece a pass, paused, 52-54 s and 72-75 s while 1-second
//        tracks played), no new compaction meanwhile (Jobs::hold), and
//        D not read while one is under way (tagcheck::Turn: gs waits for
//        it, and says so), either file read again if the card was mounted
//        again under it; its [console] line after the last (the phases:
//        tagcheck::Gs; the reads and the lines here, its GsIo)
//   gt   one file: its tags read now (TagScan), its records in D and in T,
//        the record the builder would take (LibraryBuilder::choose(), 2.9)
//        and the names the index has for it (not while a compaction is
//        still under way after idle()'s wait: it replaces D)
//   gr, gr!, gw, gb, gv: the card worker's jobs (Rescan tags, walk, build,
//        verify), handed in by whoever runs it (setJobs(): app/CardTasks);
//        without them they say there is none, and change nothing; gb! the
//        update step deferred to the next boot (L4.4), gs0 the worker's
//        figures from now (L3)
//   gc   the PSRAM sector cache under FatFs (storage/SectorDisk): its
//        counts; gc0 off, gc1 on, gc2 on with every hit checked against
//        the card (the device batch's L0 and L1): each prints the counts up
//        to the switch, and they start again from it
//   gl   L0's bench: the card's time per sector, and the opens of a file
//        under /music's 1st, 353rd and 703rd entries uncached, cold and warm;
//        glw also the stock walk (forEachFile) and 3.2.3's walk (CardWalk
//        over FatFs), each uncached and cached (minutes at 20k: playback
//        stopped)
//
// About 35 KB of PSRAM while a command runs (gs: until its last line; the
// readers, the check's Walker, the records found, their buffers), none
// between. Nothing big goes on the stack: the loop task has 8 KB, and a
// read of the card under a command takes about 2 KB of it (the VFS,
// FatFs's 512 B long-name buffer, the SD driver). gs once overflowed it
// with mptg::check()'s 3.9 KB Walker on the stack; each command's own depth
// is the [console] line after it (app/LoopStack; gs's, its pieces' too).
class TagConsole : private tagcheck::GsIo {
public:
  struct Jobs {
    bool (*rescan)(bool everything) = nullptr;
    bool (*walk)() = nullptr;
    bool (*build)() = nullptr;
    bool (*buildAtBoot)() = nullptr;  // gb!: the update step, its memory check made to fail
    bool (*verify)() = nullptr;
    // The scan's state now: the status line's (librarytext), and a line of
    // the worker's own for the console ("" for none).
    void (*state)(librarytext::Status* status, char* line, size_t size) = nullptr;
    // gs's lines of the worker's own (its waits, its steps, its stack).
    void (*report)() = nullptr;
    // gs0: those figures start again.
    void (*resetStats)() = nullptr;
    // Waits for the worker's step under way to finish, before a command
    // reads the card's records itself (gt, gl).
    bool (*idle)() = nullptr;
    // gs, over the passes its reads take (CardTasks::holdForConsole()):
    // Worker while it waits for the step under way (busy() until it's taken
    // in; kWaitMs at most, as idle()'s wait), Records while it reads (no
    // new compaction replaces tags.bin under them), None after its last
    // line.
    void (*hold)(tagcheck::Hold h) = nullptr;
    bool (*busy)() = nullptr;
    // A compaction under way (it replaces tags.bin at its end), and the
    // compactions handed so far (CardTasks::replacing(), compactions()): D
    // isn't read meanwhile, and a check of D one overlapped is read again
    // (tagcheck::Turn; the card's guard's remounts too, either file's:
    // sectordisk::guard()).
    bool (*replacing)() = nullptr;
    uint32_t (*compactions)() = nullptr;
    // A list moves, and nothing is drawn (the screen dark): gs's pieces
    // wait for a list moving on a lit screen only (tagcheck::pieceWaits():
    // the scan's slices yield to it too).
    bool (*listMoving)() = nullptr;
    bool (*dark)() = nullptr;
  };

  // gs's piece of a pass: its units (lib/core TagCheck) until this long
  // has passed, each whole (5.2 KB of the card at most, three reads at
  // most but for the opens' small ones; about 4.8 ms a read through the
  // VFS on the device).
  static constexpr uint32_t kSliceUs = 10000;
  // gs's wait for the worker's step under way, then its first lines
  // regardless (idle()'s 10 s, as before). Its reads wait on while that
  // step is a compaction (tagcheck::Turn: 33-48 s were seen, 2026-10-14).
  static constexpr uint32_t kWaitMs = 10000;

  TagConsole(LocalStorage& storage, Library& library) : storage_(storage), library_(library) {}
  void setJobs(const Jobs& jobs) { jobs_ = jobs; }
  // A parsed g command that is the tags' (Status, Dump, Rescan, RescanAll,
  // Walk, Build, Verify).
  void command(const tagtext::Parsed& p);
  // g's line: where the names came from, and the scan's state.
  void summary();
  // Every loop pass: a piece of gs's reads, while one is under way.
  void loop(uint32_t nowMs);

private:
  struct Work;
  struct Found;
  void status();
  // What gs knows of the loop at the pass (Jobs' hooks, the card's guard).
  tagcheck::Pass pass(uint32_t nowMs) const;
  // gs's reads and lines (tagcheck::GsIo: tagcheck::Gs decides when).
  void hold(tagcheck::Hold h) override;
  void firstLines() override;
  bool memory() override;
  tagcheck::Check& check() override;
  bool open(bool device) override;
  void close() override;
  void lines(bool device, bool read) override;
  void waiting(bool device) override;
  void again(bool device, const tagcheck::Turn& t) override;
  void notRead(bool device, const tagcheck::Turn& t) override;
  void journals() override;
  bool root() override;
  void plan() override;
  void end() override;
  void deviceLines();
  void transferLine();
  void dump(const char* path);
  void cache(uint32_t n);
  void cacheCounts(const char* when);
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
  // gs under way.
  tagcheck::Gs gs_;
  uint32_t gsLeast_ = UINT32_MAX;  // the loop task's stack: the least its pieces left
  Work* gsWork_ = nullptr;
  char gsPath_[64] = "";  // T's
  uint64_t gsCommit_ = 0;
  uint32_t gsGeneration_ = 0;
};
