// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"

// The console's tag commands (docs/METADATA.md 3.3.6; milestone N9): what
// `g` takes, and the lines it prints about a tag record and the library's
// sources. Portable, host-tested (test_ui_library); the firmware's side is
// app/TagConsole, which reads the card.
//
//   g             the library's report, and where its names came from
//   g0            walk /music and build again (as before)
//   g<n>          a synthetic library of n tracks for the labs (as before)
//   gs            the scan's status: the device's records, the transfer's,
//                 the index's sources
//   gt</music/..> one file: its tags read now, its records in D and T,
//                 which one the builder takes (2.9), and the index's names
//   gr            Rescan tags: the device's own records read again
//   gr!           ... the transfer's files' too (a diagnostic)
//   gw, gb, gv    walk now, build now, verify T's files (the card worker's
//                 jobs: N10, N12)
//   gc            the PSRAM sector cache under FatFs (N10): its counts; gc0
//                 off, gc1 on, gc2 on with every hit checked against the
//                 card (the device batch's L0 and L1, 6.3)
//   gl            L0's bench: the card's time per sector, the opens of a
//                 file under /music's 1st, 353rd and 703rd entries,
//                 uncached and cached;
//                 glw also the stock walk of /music, uncached and cached
namespace tagtext {

enum class Command : uint8_t {
  Report,     // g
  Rebuild,    // g0
  Synthetic,  // g<n>: `n`
  Status,     // gs
  Dump,       // gt<path>: `path`
  Rescan,     // gr
  RescanAll,  // gr!
  Walk,       // gw
  Build,      // gb
  Verify,     // gv
  Cache,      // gc, gc0, gc1, gc2: `n` the switch (kCacheReport, 0, 1, 2)
  Bench,      // gl, glw: `n` 1 with the walks
  Bad,        // anything else (g<n> out of 1-50,000 included)
};
// Cache's `n` for gc alone: the counts, nothing switched.
constexpr uint32_t kCacheReport = 9;
struct Parsed {
  Command command = Command::Bad;
  uint32_t n = 0;            // Synthetic's; Cache's and Bench's
  const char* path = "";     // Dump's: the rest of the argument, from its first non-space
};
// `arg`: what followed the 'g', trimmed (nullptr: "").
Parsed parse(const char* arg);
// The help line (the console's 'g' entry).
extern const char* const kHelp;

// A line at a time (the firmware's Serial, a test's vector).
using LineFn = void (*)(void* ctx, const char* line);

// One record, field by field: what its producer looked for (`known`) and
// found, two spaces in. The fields of its run (`run`, nullptr: none),
// lists as their values joined with " | "; then the numbers, the
// ReplayGain, the picture's anchor and the flags. An UNREADABLE record is
// one line.
void dumpRecord(const cardcontract::mptg::Record& r, const cardcontract::RunFields* run, LineFn out, void* ctx);

// "Scanned", "Software", "Pending", "Unreadable".
const char* statusName(LibraryBuilder::Status s);
// "the transfer's record", "the device's record", "the path".
const char* pickName(LibraryBuilder::Pick p);
// "2026-10-01 12:34:56" (FatFs's date and time, 2-second steps); "none" for 0.
size_t fatTimeText(uint32_t fatTime, char* buf, size_t size);
// "3:41.250"; "unknown" for 0.
size_t lengthText(uint32_t ms, char* buf, size_t size);
// "1A".."12A", "1B".."12B" (2.6.4); "" for none.
const char* camelotName(uint8_t camelot);
// A record's flags in words ("NO_TAGS, TRUNCATED"); "" for none.
size_t flagsText(uint16_t flags, char* buf, size_t size);

// Where the index's names came from (Track::flags' source, kTrackPending).
struct Sources {
  uint32_t tracks = 0;
  uint32_t transfer = 0;  // the transfer's records
  uint32_t device = 0;    // the device's
  uint32_t path = 0;      // their paths alone
  uint32_t pending = 0;   // of those, the files the scan should read
};
Sources countSources(const LibraryIndex& index);
// "77 tracks: 77 named by their paths", "19,410 tracks: 19,000 from the
// transfer's records, 400 from the device's, 10 by their paths (10 for
// the scan)".
size_t sourcesText(const Sources& s, char* buf, size_t size);

}  // namespace tagtext
