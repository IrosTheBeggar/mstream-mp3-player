// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContainer.h"
#include "CardContract.h"
#include "CardManifest.h"
#include "TagStore.h"

// What the boot reads of /.mstream (docs/METADATA.md 3.2.2 step 1, 2.5.4,
// 2.12.5; milestone N10): the root that counts and the transfer's tags file
// it names (T), checked as far as the boot checks them, and whether a
// transfer's plan is on the card. A few small reads: each root's frame and
// sections (a few hundred bytes), T's header and directory, the plan's.
// Portable, host-tested (test_card_jobs), through N4's file interface
// (tagstore::Fs: FatFs on the device). The device never writes /.mstream.
//
// T counts as present when the root that wins the election (2.5.4) is
// valid whole and its MPTG companion (2.5.3) opens as an MPTG file whose
// header's generation, fileBytes and headerCrc are the COMP entry's and
// whose source is the software's (2 or 3). Its sections' CRCs are checked
// by whoever streams it (the walk, the builder): a T that fails then is
// absent for that job, which carries on without it.
//
// The root's MPDJ entry (the AutoDJ index, docs/AUTODJ-INDEX.md 7.2) is
// kept as COMP names it, only while T is present: an MPDJ without a valid
// MPTG companion isn't used. The file itself isn't opened here: Auto DJ
// opens it after the library's load and checks it against the entry
// (mpdj::Table::open, msmf::companionMatches). The root's MPWF entry (the
// waveform index, docs/WAVEFORM-INDEX.md 7.2) is kept the same way, and a
// card job opens that file when Now Playing wants a shape (mpwf::Table::open).
//
// A read the card fails is none of these (readFailed()): it says nothing of the
// root, so it must not read as a card with no transfer data. That would cost far
// more than the glitch: the boot would build over library.idx from D alone and
// the scan set out on every file T lists (docs/METADATA.md 3.2.2).
namespace cardroot {

// The library roots (LIBR, 2.8.6) the boot keeps: at most this many, each
// within the contract's path limit. A root with more has the rest left out
// (their files index under /music, as a card with no LIBR would).
constexpr uint32_t kMaxRoots = 16;

struct Root {
  // The root that won the election (2.5.4), checked whole: the card's id
  // and that commit's generation (2.5.2). Kept also when T then fails, or a read of
  // its library roots after the check (the root still names the card: @status cid=,
  // docs/HOST-STATUS.md). False: no root, none valid, or the card failed a read
  // before one was checked whole (readFailed()).
  bool hasRoot = false;
  uint64_t cardId = 0;
  uint32_t generation = 0;
  // T: the root's tags file, checked as above.
  bool present = false;
  tagstore::Identity identity;  // present: the commit (cardId, generation, commitId, T's headerCrc)
  char tagsPath[48] = "";       // "/.mstream/tags-0000002a.bin"
  uint32_t tagsBytes = 0;
  uint32_t tagsRecords = 0;     // T's header's record count (the update step's memory check)
  // Why there is no transfer data (Ok when present). Missing: no root at
  // all, or (hasRoot) the root's T isn't on the card. Io: readFailed().
  cardcontract::Why why = cardcontract::Why::Missing;
  // The card failed a read of a root or of T (a read, the volume's lock, no
  // memory for the file): nothing is known of its transfer, not that it has none.
  // A candidate that fails it holds up the election: it may be the newer one. (A
  // read that failed on a file's own records in the file system isn't this: the
  // card answered, and the file is invalid: Why::FatRecords.)
  bool readFailed() const { return !present && why == cardcontract::Why::Io; }
  // manifest.bin and manifest.tmp are one commit seen twice (a rename cut
  // between its two directory writes): readers use either (2.5.4); the
  // software asks for a disk check.
  bool sameCommitTwice = false;
  // The library roots, relative to /music, in LIBR's order.
  uint32_t rootCount = 0;
  char roots[kMaxRoots][cardcontract::kMaxRelPath + 1] = {};
  // The roots as LibraryBuilder::Config::libraryRoots takes them (pointers
  // into this Root, which must then stay where it is).
  const char* const* rootList() {
    for (uint32_t i = 0; i < rootCount; ++i) rootPtrs_[i] = roots[i];
    return rootCount ? rootPtrs_ : nullptr;
  }
  // A transfer's plan (pending.bin, or a valid pending.tmp when it is
  // missing: 2.12.5): the last transfer didn't finish (the status line).
  bool plan = false;
  // The AutoDJ index the root names (AUTODJ-INDEX.md 7.2): present only
  // when T is and COMP has an MPDJ entry. `entry` is that entry (its
  // generation, fileBytes, headerCrc and key, the selection signature:
  // msmf::companionMatches(entry, table.frame(), 0, table.selectionSig())
  // says whether the file opened is the one named).
  struct AutoDj {
    bool present = false;
    char path[48] = "";  // "/.mstream/autodj-00000029.bin"
    cardcontract::msmf::Comp entry;
  } autoDj;
  // The waveform index the root names (WAVEFORM-INDEX.md 7.2): present only
  // when T is and COMP has a good MPWF row (a bad one costs only itself:
  // Manifest::open). `entry` is that row (its generation, fileBytes and
  // headerCrc: msmf::companionMatches(entry, table.frame(), 0, nullptr) says
  // whether the file opened is the one named).
  struct Waves {
    bool present = false;
    char path[48] = "";  // "/.mstream/waves-0000002a.bin"
    cardcontract::msmf::Comp entry;
  } waves;

private:
  const char* rootPtrs_[kMaxRoots] = {};
};

// Reads the root and the plan through `fs`, `scratch` (at least 512 bytes)
// for the readers' streams. `out` is a few KB: a heap or PSRAM object.
void read(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out);

// read(), and again while the card fails it (readFailed()), with `wait` between:
// most faults are a glitch, and at the boot nothing else is held up. These are
// short, against what the boot decides from the answer. How many reads it took.
constexpr uint32_t kBootWaitsMs[] = {100, 300, 1000};
uint32_t readAtBoot(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out, void (*wait)(uint32_t ms));

// One line for the boot's log ("T tags-0000002a.bin, generation 42 (commit
// 1122334455667788), 3 library roots", "no transfer data (none on the
// card)"; a root whose T failed: "no transfer data (the root's T
// tags-0000002a.bin, generation 42: missing)"; "its root unread (a read of the
// card failed)"; "its T tags-0000002a.bin, generation 42, unread (a read of the
// card failed)").
size_t describe(const Root& r, char* buf, size_t size);

}  // namespace cardroot
