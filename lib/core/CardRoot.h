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
namespace cardroot {

// The library roots (LIBR, 2.8.6) the boot keeps: at most this many, each
// within the contract's path limit. A root with more has the rest left out
// (their files index under /music, as a card with no LIBR would).
constexpr uint32_t kMaxRoots = 16;

struct Root {
  // T: the root's tags file, checked as above.
  bool present = false;
  tagstore::Identity identity;  // present: the commit (cardId, generation, commitId, T's headerCrc)
  char tagsPath[48] = "";       // "/.mstream/tags-0000002a.bin"
  uint32_t tagsBytes = 0;
  // Why there is no transfer data (Ok when present; Missing: no root at all).
  cardcontract::Why why = cardcontract::Why::Missing;
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

private:
  const char* rootPtrs_[kMaxRoots] = {};
};

// Reads the root and the plan through `fs`, `scratch` (at least 512 bytes)
// for the readers' streams. `out` is a few KB: a heap or PSRAM object.
void read(tagstore::Fs& fs, uint8_t* scratch, uint32_t scratchBytes, Root* out);

// One line for the boot's log ("T tags-0000002a.bin, generation 42 (commit
// 1122334455667788), 3 library roots", "no transfer data (no root)").
size_t describe(const Root& r, char* buf, size_t size);

}  // namespace cardroot
