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
//       them. "2-04 04-Title": the number written again is dropped, when
//       '-', '_' or '.' joins it to the title and at least half of the
//       folder's disc-track names repeat theirs ("1-10 10 Paper Kites"
//       keeps its title).
//   "101 Title", "204-title"
//       disc 2, number 4: three digits, the same folder rule, and each
//       disc's numbers start at 0 or 1 ("365 Steps" next to "500 Stairs"
//       reads as the plain rule: 365 is no disc-track number).
//   "Artist - 03 - Title", "Artist - Album - 01 Title", "CD2 - 04 - Title"
//       number 3, title "Title": the first " - " followed by one to three
//       digits (at most 255) and a separator, not a number that runs on
//       ("Artist - 1-800 Lines", "Artist - 24-7"). A disc comes from the
//       end of the part before it: "CD2", "Album - Disc 2", "Album (Disc
//       2)", "Album [CD 2]". Read only when that part
//       - starts with the folder's artist ("Artist feat. Guest" counts; an
//         artist named with digits too: "10 Lanterns - 01 - Title") and the
//         rest of it, past the artist and a disc, is the same in every such
//         name of the folder, so two albums or two works numbered from 1
//         ("Artist - Suite No. 1 - 1. Overture", "... No. 2 - 1.
//         Overture") keep their file names' order; or
//       - is a disc part alone ("CD2"), or is the same, past a disc, in
//         every name of the folder;
//       and the numbers look like track numbers: " - " follows the number,
//       or the folder has two such names or more, at least half of it,
//       their numbers all differ (copies with the same name aside), and
//       the lowest is 0 or 1 or one is written "0N". So "Artist - 99
//       Lanterns" among "Artist - Title" names, or a single's "Artist - 7
//       Kites" next to "Artist - 7 Kites (Live)", keep their titles.
// Then a title that starts with the folder's artist and " - " loses that
// part ("03 - Artist - Title" -> "Title"), when it is the same name as
// textfold::sameName() compares them (case, accents, the punctuation a FAT
// name can't hold, a leading "The"; an artist "A - B" too), and something
// follows. A title "Other Artist - Title" (a compilation's) is left whole:
// the index has no track artist to keep it in.
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
// " - 03"), firstLen (its first " - " part), keyLen (the prefix without a
// disc part at its end), disc (from that disc part, else 0), dash (" - "
// right after the number) and padded (the number written "0N").
// `digitLead`: a name that starts with a digit may have the shape too
// (Folder allows it when the digits are its artist's: "10 Lanterns - 01").
struct Prefixed {
  Name name;
  uint16_t prefixLen = 0;
  uint16_t firstLen = 0;
  uint16_t keyLen = 0;
  bool dash = false;
  bool padded = false;
};
bool prefixed(const char* stem, size_t len, Prefixed* out, bool digitLead = false);
// "Artist - Title" with `artist` (NUL-terminated) that name: where "Title"
// starts; 0 when it doesn't start with the artist. The artist may hold
// " - " itself ("A - B - Title" with the artist "A - B").
size_t afterArtist(const char* title, size_t len, const char* artist);

// One folder's names: add() them all, then read() each. It keeps pointers
// into the names it was given and to `artist`: they outlive it.
class Folder {
public:
  // `artist`: the folder's artist (its artist folder's name; "" at the top).
  explicit Folder(const char* artist);
  void add(const char* stem, size_t len);
  Name read(const char* stem, size_t len) const;

  bool discTracks() const { return discTrack_ >= 2 && discTrack_ == lead_; }
  bool hundredsRule() const;
  bool samePrefix() const { return files_ >= 2 && prefixed_ == files_ && samePrefix_; }

private:
  // Rule 3's shape as this folder reads it: prefixed(), a digit lead only
  // when the digits are the artist's. *artistEnd: where the artist ends in
  // the prefix (at one of its " - "s, or its end); 0 when it doesn't start
  // with the artist.
  bool shape(const char* stem, size_t len, Prefixed* p, size_t* artistEnd) const;
  // Whether rule 3's numbers look like track numbers.
  bool numbered(const Prefixed& p) const;

  const char* artist_;
  uint32_t files_ = 0;
  uint32_t lead_ = 0;  // names that start with a digit (not an artist's)
  uint32_t discTrack_ = 0;
  uint32_t repeats_ = 0;  // disc-track names with the number written again
  uint32_t hundreds_ = 0;
  uint32_t prefixed_ = 0;
  uint8_t lowest_[10] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};  // per disc: its lowest number
  // Rule 3's names: their lowest number, one written "0N", a number seen
  // twice on a disc (seen_: per number, a bit per disc 0..7, the higher
  // discs on bit 7).
  uint8_t prefixLowest_ = 0xFF;
  bool padded_ = false;
  bool numberTwice_ = false;
  uint8_t seen_[256] = {};
  const char* lastStem_ = nullptr;  // the name before (a copy has the same)
  size_t lastLen_ = 0;
  const char* firstKey_ = nullptr;  // the first prefixed name's key (inside its name)
  size_t firstKeyLen_ = 0;
  bool samePrefix_ = true;
  const char* firstRest_ = nullptr;  // the first artist-led name's rest, past the artist
  size_t firstRestLen_ = 0;
  bool sameRest_ = true;
};

}  // namespace trackname
