// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "LibraryIndex.h"

// A made-up library for measuring the index (and the lists drawn from it) at
// scales the SD card doesn't have: `tracks` files spread over `albums`
// albums of `artists` artists, as "/music/<Artist>/<Album>/NN - <Title>.mp3"
// (every 5th .flac), fed to LibraryIndex::addFile() between the caller's
// begin() and finish(). Deterministic for a seed. Names are built from a word
// list with realistic lengths (artists ~4-30 bytes, albums ~5-40, titles
// ~3-50) and a sprinkling of what real tags have: accents (Pénélope, Café),
// typographic apostrophes and hyphens (Can’t, T‐Pain), names starting with
// digits and "The ". Every artist and every album of an artist has a name of
// its own, so the index ends up with exactly `artists` artists and `albums`
// albums (when albums >= artists and tracks >= albums).
//
// With tags (tagged()): the same library as the files' tag records would
// describe it, the folder and file names then derived from the tags at the
// disagreement rates measured on a real library (docs/METADATA.md, the
// metascan research's section 4 and its database statistics: aggregates,
// no names). The rates are the k*Pct constants below; LibraryBuilder's tests
// write these records into T and D files and build the index from them.
namespace synth {

struct Spec {
  uint32_t tracks = 10000;
  uint32_t artists = 600;
  uint32_t albums = 1500;
  uint32_t seed = 1;
  const char* root = "/music";
};

// The usual shape for n tracks: 6 artists and 15 albums per 100 tracks
// (10,000 -> 600 and 1,500), at least 1 of each.
Spec specFor(uint32_t tracks);
// The measured library's shape: about 19,500 files in 705 artist folders
// and 1,791 album folders, so 3.6 artists and 9.2 albums per 100 tracks.
Spec userShape(uint32_t tracks);

// Adds the files; returns how many addFile() accepted.
uint32_t addTracks(LibraryIndex& index, const Spec& spec);

// The i-th track's path (for tests), into buf; false if it didn't fit.
bool trackPath(const Spec& spec, uint32_t i, char* buf, uint32_t size);

// ---- tags ----
// The measured rates, in percent of tracks unless said (MEASURED on the
// user's library; ESTIMATED where marked).
// The tag title against the file name's title part: the same; the file
// name with the artist in front ("NN - Artist - Title"); the file name
// longer, the title inside it ("Title (Live)"); the same letters in another
// case or punctuation; the tag longer ("Title (Remastered)"); different; the
// rest (0.6 %) no tag title.
constexpr uint32_t kTitleSamePct = 61;
constexpr uint32_t kTitleArtistPct = 26;
constexpr uint32_t kTitleInsidePct = 6;
constexpr uint32_t kTitleCasePct = 4;
constexpr uint32_t kTitleLongerPct = 2;
constexpr uint32_t kTitleOtherPct = 1;
// The album folder against the album tag, in percent of albums: the same;
// the year added; another suffix ("[FLAC]", "(Deluxe Edition)"); a
// different name; the rest (7 %) the same letters in another case.
constexpr uint32_t kAlbumSamePct = 38;
constexpr uint32_t kAlbumYearPct = 35;
constexpr uint32_t kAlbumSuffixPct = 16;
constexpr uint32_t kAlbumOtherPct = 4;
// The artist folder against the album artist, else the artist, in percent
// of artists: the same; case or punctuation (FAT can't store / : ? " * < >
// \ |); different; the rest (8 %) the same.
constexpr uint32_t kArtistCasePct = 9;
constexpr uint32_t kArtistOtherPct = 3;
// Tag presence, in percent of tracks (of albums for the album-level ones).
constexpr uint32_t kTrackPct = 98;         // a track number
constexpr uint32_t kNumberAgreesPct = 98;  // the file name's number is the tag's
constexpr uint32_t kYearPct = 95;
constexpr uint32_t kGenrePct = 79;
constexpr uint32_t kDiscPct = 24;           // a disc number
constexpr uint32_t kMultiDiscPct = 2;       // albums of 2 discs (CD1, CD2 subfolders)
constexpr uint32_t kAlbumArtistPct = 36;    // albums whose tracks carry an album artist
constexpr uint32_t kCompilationPct = 1;     // albums of various artists, flagged
constexpr uint32_t kMultiArtistPct = 4;     // several artist values ("Artist", "Guest")
constexpr uint32_t kPicturePct = 23;        // an embedded picture (a JPEG in 4 of 5)
constexpr uint32_t kSortPct = 2;            // sort tags (ESTIMATED: "1-3 % each")
constexpr uint32_t kNoTagsPerMille = 4;     // a file with no tags at all

// One file of the tagged library: its path and its tag record's fields
// (raw: lists separated by U+001F, 2.3.6 already met), as a reader of the
// file following part 5 would record them.
struct Tagged {
  char path[320];
  char title[160];
  char artist[200];
  char album[160];
  char albumArtist[160];
  char genre[48];
  char artistSort[80];
  char albumSort[160];
  char albumArtistSort[80];
  uint16_t year = 0, track = 0, trackTotal = 0, disc = 0, discTotal = 0;
  uint32_t durationMs = 0;
  uint8_t compilation = 0;  // 0 not said, 1 yes, 2 said no
  bool picture = false;
  bool jpeg = false;
  bool noTags = false;      // read, and carries none (NO_TAGS)
  uint32_t size = 0;
  uint32_t fatTime = 0;
  uint8_t container = 0;    // 1 MP3, 2 FLAC
  uint32_t albumIndex = 0;  // which album it belongs to (0 .. spec.albums - 1)
  bool albumCover = false;  // its album folder has a cover.jpg
};
// The i-th track of the tagged library `spec` describes (its paths are its
// own: the folder names follow the tags). False if it didn't fit.
bool tagged(const Spec& spec, uint32_t i, Tagged* out);
// The album folder of the i-th track, relative to the root ("Artist/Album").
bool albumFolder(const Spec& spec, uint32_t album, char* buf, uint32_t size);

}  // namespace synth
