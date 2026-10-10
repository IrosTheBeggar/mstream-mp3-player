// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "LibraryIndex.h"
#include "TagStore.h"

// The boot's decision (docs/METADATA.md 3.2.2's table; milestone N10): what
// becomes of /.player/library.idx, given its header (LibraryIndex::peek())
// and what the card holds (the transfer's root, T: cardroot::read(); the
// device's records, D: TagStore::open()). The boot never walks the card to
// decide: a matching index is loaded, a card with records builds from them
// (LibraryBuilder), and only a card with no records at all walks /music
// into a path-named index. The validation walk follows in the background
// either way (3.2.3). Portable, host-tested (test_card_jobs).
namespace libraryboot {

// library.idx as the boot found it.
enum class Saved : uint8_t {
  Missing,   // no file (and no whole library.tmp to take its place)
  Corrupt,   // unreadable, foreign, or a newer firmware's (peek() or load())
  Outdated,  // versions 1-5, or an older rulesVersion
  Matches,   // v6, built against the card's transfer identity (matches())
  Differs,   // v6, another identity: a transfer happened, or /.mstream went or was damaged
};

struct In {
  Saved saved = Saved::Missing;
  bool marker = false;    // /.player/build.req: a build was deferred to this boot (3.4.2)
  bool transfer = false;  // T is present (cardroot::Root::present)
  bool device = false;    // D is present (TagStore::device().present)
};

enum class Action : uint8_t {
  Load,   // load library.idx: no walk, no build
  Build,  // build from the records (T and D) behind the boot screen, then save
  Walk,   // walk /music now into a path-named index (no records at all), then save
};

struct Decision {
  Action action = Action::Walk;
  bool compactFirst = false;  // the journals are compacted before the build (D always is, 3.4.1)
  bool removeMarker = false;  // the marker goes once the build is saved
  const char* why = "";       // for the boot's log
};

Decision decide(const In& in);

// A saved index's hard inputs are the card's: the transfer's identity
// (cardId, generation, commitId, T's headerCrc; zeros for none) and no path
// signature (this boot never walks to decide). Whether T was read whole
// and used isn't asked: the same identity is the same T, and one that
// failed its checks at the build fails them again.
bool matches(const LibraryIndex::Inputs& saved, const tagstore::Identity& root);

// What a build saves in library.idx's header (3.4.3): the root's identity,
// whether T was used, D's headerCrc and the journal's last sequence (the
// soft inputs: the scan going on since makes them differ). `journalsLeft`:
// the build read tags.bin while the journals (tags.jnl, walk.jnl) were
// left on the card, their compaction refused (a full card, a walk being
// written: N10's review): the sequence saved is kJournalsLeftOut, which no
// store has, so the next boot finds the index soft-stale and rebuilds it at
// its scan's end, with the records this build didn't read.
constexpr uint32_t kJournalsLeftOut = 0xFFFFFFFFu;
LibraryIndex::Inputs inputsOf(const tagstore::Identity& root, bool transferUsed, uint32_t deviceCrc,
                              uint32_t journalSeq, bool journalsLeft = false);

// A loaded index's soft inputs against the store's now: differing, the
// scan went on since its build (or the build left records out), and the
// scan's end rebuilds it (Library::softStale()).
bool softStale(const LibraryIndex::Inputs& saved, uint32_t deviceCrc, uint32_t journalSeq);

// The Saved state for a peek() (`load` its result) and its inputs.
Saved savedOf(LibraryIndex::Load load, const LibraryIndex::Inputs& saved, const tagstore::Identity& root);

const char* savedName(Saved s);
const char* actionName(Action a);

}  // namespace libraryboot
