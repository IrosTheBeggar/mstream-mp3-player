// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "LibraryBoot.h"

namespace libraryboot {

Decision decide(const In& in) {
  Decision d;
  const bool records = in.transfer || in.device;
  if (in.saved == Saved::Matches) {
    if (in.marker) {
      // A build was deferred (the update step's memory check, 3.4.2): it
      // runs now, on a fresh heap, before the UI.
      d.action = Action::Build;
      d.compactFirst = true;
      d.removeMarker = true;
      d.why = "a build was deferred to this boot";
      return d;
    }
    d.action = Action::Load;
    d.why = "its inputs are the card's";
    return d;
  }
  d.removeMarker = in.marker;  // whatever is built now is what it asked for
  if (records) {
    d.action = Action::Build;
    d.compactFirst = true;
    switch (in.saved) {
      case Saved::Differs: d.why = "another transfer's, or an older firmware's walk"; break;
      case Saved::Outdated: d.why = "an older version's"; break;
      case Saved::Corrupt: d.why = "unreadable"; break;
      default: d.why = "none yet"; break;
    }
    return d;
  }
  // No records at all: a card-reader card, or this firmware's first boot.
  d.action = Action::Walk;
  switch (in.saved) {
    case Saved::Differs: d.why = "another transfer's or an older firmware's, and no records"; break;
    case Saved::Outdated: d.why = "an older version's, and no records"; break;
    case Saved::Corrupt: d.why = "unreadable, and no records"; break;
    default: d.why = "none yet, and no records"; break;
  }
  return d;
}

bool matches(const LibraryIndex::Inputs& saved, const tagstore::Identity& root) {
  const uint64_t cardId = root.present ? root.cardId : 0;
  const uint32_t generation = root.present ? root.generation : 0;
  const uint64_t commitId = root.present ? root.commitId : 0;
  const uint32_t tagsCrc = root.present ? root.tagsCrc : 0;
  return saved.walkSignature == 0 && saved.cardId == cardId && saved.generation == generation &&
         saved.commitId == commitId && saved.tagsCrc == tagsCrc;
}

LibraryIndex::Inputs inputsOf(const tagstore::Identity& root, bool transferUsed, uint32_t deviceCrc,
                              uint32_t journalSeq, bool journalsLeft) {
  LibraryIndex::Inputs in;
  if (root.present) {
    in.cardId = root.cardId;
    in.generation = root.generation;
    in.commitId = root.commitId;
    in.tagsCrc = root.tagsCrc;
  }
  in.transfer = root.present && transferUsed;
  in.deviceCrc = deviceCrc;
  in.journalSeq = journalsLeft ? kJournalsLeftOut : journalSeq;
  return in;
}

bool softStale(const LibraryIndex::Inputs& saved, uint32_t deviceCrc, uint32_t journalSeq) {
  return saved.deviceCrc != deviceCrc || saved.journalSeq != journalSeq;
}

Saved savedOf(LibraryIndex::Load load, const LibraryIndex::Inputs& saved, const tagstore::Identity& root) {
  switch (load) {
    case LibraryIndex::Load::Loaded: return matches(saved, root) ? Saved::Matches : Saved::Differs;
    case LibraryIndex::Load::Outdated: return Saved::Outdated;
    case LibraryIndex::Load::Stale: return Saved::Differs;
    default: return Saved::Corrupt;
  }
}

const char* savedName(Saved s) {
  switch (s) {
    case Saved::Missing: return "missing";
    case Saved::Corrupt: return "unreadable";
    case Saved::Outdated: return "outdated";
    case Saved::Matches: return "matches the card";
    case Saved::Differs: return "built for other card data";
  }
  return "?";
}

const char* actionName(Action a) {
  switch (a) {
    case Action::Load: return "load";
    case Action::Build: return "build from the records";
    case Action::Walk: return "walk /music";
  }
  return "?";
}

}  // namespace libraryboot
