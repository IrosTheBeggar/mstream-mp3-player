// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "CardContract.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "MoodFilter.h"

// The console's tag commands (docs/METADATA.md 3.3.6; milestone N9): what
// `g` takes, and the lines it prints about a tag record and the library's
// sources. Portable, host-tested (test_ui_library); the firmware's side is
// app/TagConsole, which reads the card, and app/Library (gm).
//
//   g             the library's report, where its names came from, and its
//                 mood words
//   g0            walk /music and build again (as before)
//   g<n>          a synthetic library of n tracks for the labs (as before)
//   gs            the scan's status: the device's records, the transfer's,
//                 the index's sources; gs0 starts the card worker's figures
//                 again (the waits, the steps, its stack, internal RAM: L3's
//                 per-condition figures)
//   gt</music/..> one file: its tags read now, its records in D and T (T's
//                 mood tail as its bytes stand, valid or not, and its word),
//                 which one the builder takes (2.9), and the index's names
//                 and mood word
//   gm            the index's mood words (docs/MOOD-FILTERS.md 5.4, 9.3):
//                 the block, the tracks with a word, the coverage
//   gm<hex>       the tracks and the albums filter q lets through (5.4's 11
//                 bits, 1-4 hex digits: gm127 is Sad, Relaxed, Vocals,
//                 Danceable at Standard), and each pass's time; a q that
//                 isn't a filter (bits 11-15, voice 3, level 3) is refused
//   gr            Rescan tags: the device's own records read again
//   gr!           ... the transfer's files' too (a diagnostic)
//   gw, gb, gv    walk now, build now, verify T's files (the card worker's
//                 jobs: N10, N12); gb! the update step with its memory check
//                 made to fail: the deferral to the next boot (L4.4)
//   gc            the PSRAM sector cache under FatFs (N10): its counts; gc0
//                 off, gc1 on, gc2 on with every hit checked against the
//                 card (the device batch's L0 and L1, 6.3)
//   gl            L0's bench: the card's time per sector, the opens of a
//                 file under /music's 1st, 353rd and 703rd entries,
//                 uncached and cached;
//                 glw also the stock walk of /music, uncached and cached
//   gw?           the waveform index (app/Waves): the file, the track wanted,
//                 the record and the counters. gw alone is the walk, above.
//   gw=<99 digits> a shape with no card, for a test of Now Playing's bar:
//                 each column's height, 0-9. On whatever plays, until gw-.
//   gw#<480 hex>  the same from 240 bars, through the reduction (wave::reduce())
//   gw-           drops that shape
namespace tagtext {

enum class Command : uint8_t {
  Report,     // g
  Rebuild,    // g0
  Synthetic,  // g<n>: `n`
  Status,     // gs; gs0: `n` 1 (the worker's figures start again)
  Dump,       // gt<path>: `path`
  Rescan,     // gr
  RescanAll,  // gr!
  Walk,       // gw
  Build,      // gb; gb!: `n` 1 (deferred to the next boot)
  Verify,     // gv
  Cache,      // gc, gc0, gc1, gc2: `n` the switch (kCacheReport, 0, 1, 2)
  Bench,      // gl, glw: `n` 1 with the walks
  Wave,       // gw?, gw-, gw=<...>, gw#<...>: `path` from the letter after the w
  Moods,      // gm; gm<hex>: `n` the filter's q (moodfilter::valid())
  Bad,        // anything else (g<n> out of 1-50,000 included)
};
// Cache's `n` for gc alone: the counts, nothing switched.
constexpr uint32_t kCacheReport = 9;
// Moods' `n` for gm alone: the words' figures, no filter (above any q).
constexpr uint32_t kMoodsReport = 0x10000;
struct Parsed {
  Command command = Command::Bad;
  uint32_t n = 0;            // Synthetic's; Cache's, Bench's, Status's, Build's and Moods'
  const char* path = "";     // Dump's: the rest of the argument, from its first non-space; Wave's: as above
};
// `arg`: what followed the 'g', trimmed (nullptr: "").
Parsed parse(const char* arg);
// The help line (the console's 'g' entry).
extern const char* const kHelp;
// gw='s text into `count` values: exactly that many digits 0-9. gw#'s into
// `count` bytes: exactly twice that many hex digits, of either case. False:
// anything else (`out` then holds what came before the fault).
bool digitsIn(const char* text, uint8_t* out, size_t count);
bool hexIn(const char* text, uint8_t* out, size_t count);

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

// ---- moods (gm, and g's and gt's lines: docs/MOOD-FILTERS.md 5.3, 5.4, 9.3) ----
// The words as tools/synthtransfer.py --check reads them off the card, so
// the two can be set side by side (its lines, the numbers grouped).

// The index's mood words: their block, the tracks with one.
struct Moods {
  uint32_t tracks = 0;
  uint32_t withWord = 0;  // moodTracks()
  size_t blockBytes = 0;  // memory().moods: 0 when no track has a word
};
Moods countMoods(const LibraryIndex& index);
// "17,859 of 19,412 tracks with a word (92.0 %), block 38,824 B"; with no
// block "no mood data: none of 19,412 tracks has a word, block 0 B".
size_t moodsText(const Moods& m, char* buf, size_t size);
// Filter q's groups and level, as --check's filter_name(): "valence sad;
// energy relaxed; voice vocals; tags danceable at Standard (need 2, under
// 2)"; a level alone "no group at Strict (need 3, under 1): filters
// nothing". An invalid q reads as 0 (Filter::unpack()).
size_t filterText(uint16_t q, char* buf, size_t size);

// gm<hex>'s two passes, as --check --mood-filter counts them (4.1). A track
// fits by moodfilter::count() over the words. An album is one of
// albumsAZ(), as the Albums list shows them (its folder, deeper folders in
// it, or an artist's or the root's loose tracks), and fits by
// moodfilter::tally() over its tracksOfAlbum() and majority(). Without
// words every track's is 0: only an inactive filter lets anything through.
struct TrackFits {
  uint32_t tracks = 0, fit = 0;
  uint32_t analysed = 0;  // the tracks with kAnalysed
};
TrackFits fittingTracks(const LibraryIndex& index, const moodfilter::Filter& f);
// One album: its tally into `t` (may be null), and whether it fits.
bool albumFits(const LibraryIndex& index, uint32_t album, const moodfilter::Filter& f,
               moodfilter::Tally* t = nullptr);
struct AlbumFits {
  uint32_t albums = 0, fit = 0;
  uint32_t analysed = 0;  // the albums with an analysed track
};
AlbumFits fittingAlbums(const LibraryIndex& index, const moodfilter::Filter& f);

// A record's mood tail (bytes 72-79, 5.3) as its bytes stand, and what a
// reader made of it (`taken`: File::moodTail()'s verdict): "03 3A 46 1D 31
// FF 46 00, taken: 0xB313: ..." (wordText()); 8 zero bytes "...: no server
// row (word 0)"; anything else "...: REFUSED ...". nullptr `tail`: the
// record has none (its RECS stride under 80): "none (...)".
size_t tailText(const uint8_t* tail, bool taken, char* buf, size_t size);
// A word in its fields, each a bucket (0 under 30, 1 for 30-49, 2 for
// 50-69, 3 for 70 and up) and a scale's side: "0xB313: valence sad 2,
// energy relaxed 3, instrumental 0, acoustic 1, party 0, danceable 3";
// "0x0000: no row".
size_t wordText(uint16_t word, char* buf, size_t size);

}  // namespace tagtext
