// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// Mood filters (docs/MOOD-FILTERS.md): mStream's song predicate (mStream
// src/api/mood-filter.js) worked out on the player from the card, with no
// network. Three forms of one song's scores:
//   - the server's row, 8 doubles in MOOD_COLUMNS order (mStream
//     src/db/mood-sync.js), each a score or NULL;
//   - the tail, 8 bytes at 72-79 of its T record (5.3), which a writer
//     makes from the row;
//   - the word, a u16 per track id in library.idx (5.4), which the builder
//     makes from the tail, and which every filter reads.
// Portable, host-tested (test_mood_filter, over test/fixtures/card/
// moods.json); tools/moodfilter.py is the same rules in Python (matches,
// album_counts, album_matches). No allocation, no index.
//
// The tail:
//   72  flags: bit 0 PRESENT; bit 1 sad beat happy; bit 2 aggressive beat
//       relaxed; bits 3-7 are 0
//   73  valence, the larger of happy and sad; 74 energy, the larger of
//       relaxed and aggressive (each 255 if either head is NULL)
//   75  instrumental; 76 acoustic; 77 party; 78 danceable (255 NULL)
//   79  reserved, 0
// A value is min(100, max(0, floor(score*100 + 1e-9))): plain floor is
// wrong at 0.29, 0.57, 0.58. The order bits come from the raw scores (a tie
// is happy, relaxed), and are 0 when either head is NULL. A reader takes a
// tail only with PRESENT set, bits 3-7 and byte 79 zero, and bytes 73-78
// each 0-100 or 255; otherwise the record has no scores. 8 zero bytes: no
// server row.
//
// The word:
//   15 analysed  14 voice NULL  13-11 valence  10-8 energy
//   7-6 instrumental  5-4 acoustic  3-2 party  1-0 danceable
// A bucket is 0 under 30, 1 for 30-49, 2 for 50-69, 3 for 70 and up. A
// scale's field is side<<2 | bucket, side 1 for sad or aggressive; bucket 0
// stores side 0, so 4 means NULL. A NULL tag stores 0, which never holds.
// Word 0: no row (or no valid tail), which no active filter matches.
//
// The filter (4.1): four groups, a group left out does not filter, groups
// are ANDed. Its 11 bits `q`, as NVS keeps Auto DJ's:
//   0-1 valence, 2-3 energy: 0 none, 1 Happy | Relaxed, 2 Neutral,
//       3 Sad | Aggressive
//   4-5 voice: 0 none, 1 Instrumental, 2 Vocals
//   6-8 tags: Tag bits, every one set must hold
//   9-10 level: 0 Standard, 1 Strict, 2 Loose
// Any bit 11-15, voice 3 or level 3 makes the whole of q invalid, and it
// reads as 0. A level alone filters nothing: a filter with no group set is
// inactive, and holds for every track, word 0 included (4.1; mStream adds
// no predicate for it).
//
// A level's bars, on / mirror, are Strict .7/.3, Standard .5/.5, Loose
// .3/.7; in buckets, need / under: Strict 3/1, Standard 2/2, Loose 1/3.
//   Happy, Relaxed      the head >= on and >= the other head (a tie goes
//                       to these): side 0, bucket >= need
//   Sad, Aggressive     the head >= on and > the other: side 1, bucket >=
//                       need
//   Neutral             both heads < mirror: bucket < under
//   Instrumental, tags  the head >= on: bucket >= need
//   Vocals              instrumental < mirror: bucket < under
// A NULL head fails only its own scale. Exact at the three bars for scores
// to 4 places, as the server stores them.
namespace moodfilter {

constexpr uint16_t kAnalysed = 0x8000, kVoiceNull = 0x4000;
constexpr uint8_t kNull = 255;  // a tail byte's NULL
constexpr int kHeads = 8;

// The row's heads, in MOOD_COLUMNS order: tailOf()'s scores.
enum Head : uint8_t {
  kHeadDanceable,
  kHeadHappy,
  kHeadSad,
  kHeadRelaxed,
  kHeadAggressive,
  kHeadParty,
  kHeadAcoustic,
  kHeadInstrumental,
};

enum class Scale : uint8_t { Any, First, Neutral, Second };  // First happy | relaxed, Second sad | aggressive
enum class Voice : uint8_t { Any, Instrumental, Vocals };
enum Tag : uint8_t { kAcoustic = 1, kParty = 2, kDanceable = 4 };
enum class Level : uint8_t { Standard, Strict, Loose };

struct Filter {
  Scale valence = Scale::Any, energy = Scale::Any;
  Voice voice = Voice::Any;
  uint8_t tags = 0;  // Tag bits
  Level level = Level::Standard;

  // A group set. The level alone is not.
  bool active() const;
  uint16_t pack() const;
  // An invalid q (valid() false) is no filter, at Standard.
  static Filter unpack(uint16_t q);
};

bool valid(uint16_t q);

bool tailValid(const uint8_t t[8]);
// The tail's word: 0 when it is not valid (which includes no PRESENT bit).
// An order bit beside a 255 is ignored.
uint16_t wordOfTail(const uint8_t t[8]);
// A writer's tail from the server's row: `scores` in Head order, NaN for a
// NULL head; a null pointer for no row (8 zero bytes).
void tailOf(const double* scores, uint8_t out[8]);

// The song predicate. An inactive filter holds for every word, word 0 and
// any without kAnalysed included; an active one never holds for a word
// without kAnalysed.
bool matches(uint16_t word, const Filter& f);

// An album (4.1): under an active filter it matches when 2*pass > analysed,
// counting only its analysed songs, so one with none never matches; under
// an inactive one every album matches. As moodfilter.py's album_counts and
// album_matches.
struct Tally {
  uint32_t pass = 0, analysed = 0;  // pass: analysed songs that match
  bool unfiltered = false;          // tallied under an inactive filter
};
// Over the tracks `ids` (each below the words' count).
Tally tally(const uint16_t* words, const uint32_t* ids, uint32_t n, const Filter& f);
inline bool majority(const Tally& t) { return t.unfiltered || uint64_t(t.pass) * 2 > t.analysed; }

// The words [0, n) that match (n under an inactive filter); `analysed` (may
// be null) gets those with kAnalysed.
uint32_t count(const uint16_t* words, uint32_t n, const Filter& f, uint32_t* analysed);

}  // namespace moodfilter
