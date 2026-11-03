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
// either way (3.2.3), unless the decision holds the card jobs (holdJobs).
//
// A read the card fails says nothing of its file, so these never decide on one as they
// would on a file that's missing or damaged. Such a boot keeps what is on the card: it
// saves no library and holds the card jobs. It shows library.idx if it can, or the same
// library built for the session, or /music walked for the session, by its paths. Portable,
// host-tested (test_card_jobs).
namespace libraryboot {

// library.idx as the boot found it.
enum class Saved : uint8_t {
  Missing,   // no file (and no whole library.tmp to take its place)
  Corrupt,   // unreadable, foreign, or a newer firmware's (peek() or load())
  Outdated,  // versions 1-5, or an older rulesVersion
  Matches,   // v6, built against the card's transfer identity (matches())
  Differs,   // v6, another identity: a transfer happened, or /.mstream went or was damaged
  Unread,    // the card failed a read of it (or of library.tmp, alone): nothing is known of it
};

struct In {
  Saved saved = Saved::Missing;
  bool marker = false;    // /.player/build.req: a build was deferred to this boot (3.4.2)
  bool transfer = false;  // T is present (cardroot::Root::present)
  bool device = false;    // D is present (TagStore::device().present)
  // The card failed the root's reads (cardroot::Root::readFailed()): nothing is
  // known of the transfer. Against no root, a Differs is an index built from a
  // transfer, and a Matches one with no transfer in it.
  bool rootUnread = false;
  // The card failed a read of D or its journals (TagStore::readFailed()): nothing is
  // known of the device's records.
  bool deviceUnread = false;
  // D is there and a build from the records lists the card (LibraryUpdate::
  // walkListsCard()): what Saved::Unread's build needs to be the library it stands in for.
  bool recordsList = false;
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
  // No walk, no scan and no update step this session (CardTasks::begin()): they would
  // go on without records they can't read, and replace them. A marker stays too.
  bool holdJobs = false;
  // What the build or the walk gives is this session's: nothing saved, library.idx and a
  // marker as they are. A build stands only if it is the whole library (T and D read
  // whole, no journal left); else the walk.
  bool sessionOnly = false;
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

const char* savedName(Saved s, bool rootUnread = false);
const char* actionName(Action a);

}  // namespace libraryboot
