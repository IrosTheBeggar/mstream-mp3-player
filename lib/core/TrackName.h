// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// What a track's file name says: its disc, its number and its title. Read
// from the name and from the names next to it in its folder: a shape that
// could be something else ("1-800 Title", "1999 - Title") counts only when
// the whole folder is written that way. Portable, host-tested, no
// allocation. LibraryIndex reads every folder this way as it finishes a
// build (the shapes and their counts on a real 19,000-file library are in
// docs/ARCHITECTURE.md, "Library and queue").
//
// The names are file names without their extension (the "stem"):
//   "06 - Title", "06. Title", "06_Title", "(06) Title", "[06] Title"
//       number 6, title "Title": up to three digits (at most 255) and a
//       separator (" -._"). Four digits are no number: "2001 A Space".
//   "1-01 Title", "1-01. Title", "2-03 - Title", "2.04 - Title", "1-5. Title"
//       disc 2, number 3: one or two digits, '-' or '.', one or two
//       digits, a separator. Only in a folder where every name that starts
//       with a digit is written so, and two at least: "1-02 Title" alone,
//       or "09-10 Title" among "01 Title"s, stay as the plain rule reads
//       them. "2-04 04-Title": the number written again is dropped.
//   "101 Title", "204-title"
//       disc 2, number 4: three digits, the same folder rule, and each
//       disc's numbers start at 0 or 1 ("365 Steps" next to "500 Stairs"
//       reads as the plain rule: 365 is no disc-track number).
//   "Artist - 03 - Title", "Artist - Album - 01 Title", "CD2 - 04 - Title"
//       number 3, title "Title" (disc 2 from a "CD2"/"Disc 2"/"Disk 2"
//       part): a name that doesn't start with a digit, the first " - "
//       followed by one to three digits (at most 255) and a separator. Only
//       when the part before it starts with the folder's artist (its first
//       " - " part: "Artist feat. Guest - Album - 02 Title" counts), or is
//       that disc part, or is the same in every name of the folder; and
//       the folder has two such names or more and they are at least half
//       of it, or a '-' follows the number. So "Artist - 99 Lanterns"
//       among "Artist - Title" names keeps its title.
// Then a title that starts with the folder's artist and " - " loses that
// part ("03 - Artist - Title" -> "Title"), when it is the same name as
// textfold::sameName() compares them (case, accents, the punctuation a FAT
// name can't hold, a leading "The"), and something follows. A title
// "Other Artist - Title" (a compilation's) is left whole: the index has no
// track artist to keep it in.
namespace trackname {

// A name as read: its title is the stem from `titleAt` on.
struct Name {
  uint8_t disc = 0;      // 0: none
  uint8_t number = 0;    // 0: none
  uint16_t titleAt = 0;  // 0: the whole stem
};

// The shapes on their own (what one name says without its folder).
// "06 - Title", "(06) Title": false when the name has no number (*out is
// then {0, 0, 0}: the whole stem is the title). "06 - " keeps its number
// and the whole stem as its title, "00 - Title" a number 0.
bool plain(const char* stem, size_t len, Name* out);
// "1-01 Title" and its kin; *trackDigits (optional): where the track's
// digits are, and how many (for the number written again).
bool discTrack(const char* stem, size_t len, Name* out, uint16_t* trackAt = nullptr, uint8_t* trackLen = nullptr);
// "101 Title".
bool hundreds(const char* stem, size_t len, Name* out);
// "Artist - 03 - Title": the number and title, prefixLen (the text before
// " - 03"), firstLen (its first " - " part), disc (from a disc part, else
// 0) and dash (a '-' in the separators after the number).
struct Prefixed {
  Name name;
  uint16_t prefixLen = 0;
  uint16_t firstLen = 0;
  bool dash = false;
};
bool prefixed(const char* stem, size_t len, Prefixed* out);
// "Artist - Title" with `artist` (NUL-terminated) that name: where "Title"
// starts; 0 when it doesn't start with the artist.
size_t afterArtist(const char* title, size_t len, const char* artist);

// One folder's names: add() them all, then read() each. It keeps a pointer
// into the first name it was given: the names outlive it.
class Folder {
public:
  void add(const char* stem, size_t len);
  // `artist`: the folder's artist (its artist folder's name; "" at the top).
  Name read(const char* stem, size_t len, const char* artist) const;

  bool discTracks() const { return discTrack_ >= 2 && discTrack_ == lead_; }
  bool hundredsRule() const;
  bool samePrefix() const { return files_ >= 2 && prefixed_ == files_ && samePrefix_; }

private:
  uint32_t files_ = 0;
  uint32_t lead_ = 0;  // names that start with a digit
  uint32_t discTrack_ = 0;
  uint32_t hundreds_ = 0;
  uint32_t prefixed_ = 0;
  uint8_t lowest_[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // per disc: its lowest number
  const char* firstPrefix_ = nullptr;  // the first prefixed name's prefix (inside its name)
  size_t firstPrefixLen_ = 0;
  bool samePrefix_ = true;
};

}  // namespace trackname
