// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "LibraryIndex.h"

// What the Library's rows and headers, Now Playing and the catalog say about
// the index's entries (docs/METADATA.md 5.4 and 3.6; milestone N9): the
// names Stage A elected from the tags, with today's path names where no
// record says better, so an index with no records (no tags.bin, no
// transfer) reads as before. Portable, host-tested (test_ui_library).
//
// Stage A keeps the folder entities: an artist is its folder, an album its
// folder, and Go to artist and Go to album go there. What changes is what
// they are called: an album's elected name, year and artist line, a
// track's own artist where it differs from its album's line, an artist
// folder's elected spelling (LibraryIndex's votes).
namespace librarytext {

// " · ": between the facts of a subtitle ("Artist · 2001 · 14 tracks").
inline constexpr const char* kDot = " \xC2\xB7 ";

// An album's artist line as shown: the line its tracks' records elected
// (the album-artist display, "Various Artists", the track-artist display)
// when it has records (LibraryIndex::kTagged), else its artist's name (the
// folder's, or the spelling the artist's other albums elected for it). ""
// when there is none (the files right under /music).
const char* albumArtist(const LibraryIndex& index, uint32_t album);
// A track's own artist: its record's display join, where it differs from
// its album's line (the builder drops it where it is the same); nullptr
// otherwise. The track rows' subtitle.
const char* ownArtist(const LibraryIndex& index, uint32_t track);
// A track's artist as shown: ownArtist(), else albumArtist() of its album.
const char* trackArtist(const LibraryIndex& index, uint32_t track);

// "3 albums, 41 tracks" (an artist row, the Artist page's header).
size_t artistCounts(const LibraryIndex& index, uint32_t artist, char* buf, size_t size);
// "14 tracks", "1 track".
size_t trackCount(uint32_t n, char* buf, size_t size);

// An album row's subtitle. AZ (the root's Albums): its artist line and its
// year ("Artist · 2001"; uitext::kNoArtistFolder when there is no line).
// OfArtist (an artist's albums, newest first): its year and its tracks
// ("2001 · 14 tracks", or "14 tracks").
enum class AlbumPlace : uint8_t { AZ, OfArtist };
size_t albumSub(const LibraryIndex& index, uint32_t album, AlbumPlace place, char* buf, size_t size);
// The Album page's header line: "Artist · 2001 · 14 tracks" (the parts it
// has).
size_t albumHeader(const LibraryIndex& index, uint32_t album, char* buf, size_t size);
// A track row's subtitle: its own artist (ownArtist()), and with `album`
// (an artist's All tracks) its album's name after it ("Guest · Album", or
// "Album"; uitext::kLooseTracks for the loose tracks). "" when there is
// nothing to say.
size_t trackSub(const LibraryIndex& index, uint32_t track, bool album, char* buf, size_t size);
// The album's name, or uitext::kLooseTracks.
const char* albumShown(const LibraryIndex& index, uint32_t album);
// The artist's name, or uitext::kNoArtistFolder.
const char* artistShown(const LibraryIndex& index, uint32_t artist);

// What the A-Z rail, a row's letter and the jump grid key an Artists or
// Albums row by: textfold::sortName() of its sort key (its elected sort
// tag, else its name), as the index's buckets do ("The Lantern Choir"
// under L, "Bowery, Daniel" under B).
const char* railName(const LibraryIndex& index, LibraryIndex::View view, uint32_t id);

// "Disc 2" (uitext::kDisc).
size_t discText(uint32_t disc, char* buf, size_t size);

// An album's disc dividers (5.4: "Disc 2" rows only when it has more than
// one disc): the Album page's rows are its tracks (tracksOfAlbum()) with a
// divider before each disc's first track, "Disc 1" included. An album
// whose discs don't run in order (a path-only album with "1-01" names in
// several folders, sorted folder by folder) has a divider at each change,
// up to kMax; past that, none at all. About 400 bytes: the page's.
class Discs {
public:
  static constexpr uint32_t kMax = 64;
  struct Row {
    bool divider = false;
    uint16_t disc = 0;   // a divider's disc (a missing one is 1)
    uint32_t track = 0;  // a track's place in tracksOfAlbum(); a divider: the first track after it
  };
  // Reads the album's tracks once (`album` kNone, or an index not ready:
  // none).
  void set(const LibraryIndex& index, uint32_t album);
  void clear() { n_ = 0; }
  uint32_t dividers() const { return n_; }
  // The rows of an album of `tracks` tracks.
  uint32_t rows(uint32_t tracks) const { return tracks + n_; }
  Row at(uint32_t row) const;
  // The row of the track at place `track`.
  uint32_t rowOf(uint32_t track) const;

private:
  uint32_t start_[kMax] = {};
  uint16_t disc_[kMax] = {};
  uint32_t n_ = 0;
};

// ---- the scan's texts (3.3.6; the scan's glue shows them) ----
// The Library's status line while the card worker is busy, and the toasts
// at its start and end. The counts grouped ("Reading tags 1,234 /
// 19,410").
struct Status {
  enum class Phase : uint8_t {
    Idle,        // nothing to say: no line
    Checking,    // the validation walk
    Reading,     // the scan: `done` of `total`
    Updating,    // the update step (3.4.2)
    Unfinished,  // a transfer's plan is on the card (pending.bin, 2.12.5)
  };
  Phase phase = Phase::Idle;
  uint32_t done = 0;
  uint32_t total = 0;
};
size_t statusText(const Status& s, char* buf, size_t size);
// "Found 12 new tracks", "Found 1 new track" (the walk's toast).
size_t foundText(uint32_t n, char* buf, size_t size);

// ---- the Output tab's Library row (3.3.6, N12) ----
// Where the index's tracks' names come from: the transfer's records, the
// device's own reading, or none (their paths).
struct Sources {
  uint32_t transfer = 0;
  uint32_t device = 0;
  uint32_t none = 0;
  uint32_t total() const { return transfer + device + none; }
};
Sources sourcesOf(const LibraryIndex& index);
// "Library: 19,410 tracks" ("Library: no tracks"); `form` 1, its short
// form ("19,410 tracks") when the long one doesn't fit.
size_t libraryRowTitle(uint32_t tracks, char* buf, size_t size, int form = 0);
// The row's second line in `form` 0 (long: "18,000 from the transfer, 1,400
// read here, 10 without tags"), 1 (short: "18,000 transfer, 1,400 here, 10
// none") or 2 ("99% tagged"; rounded down, 100% only when all are); the
// parts with a count only; none at all with a record: "names from the
// files". The caller takes the longest that fits.
constexpr int kSourceForms = 3;
size_t sourcesText(const Sources& s, int form, char* buf, size_t size);

}  // namespace librarytext
