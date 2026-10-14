// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "AutoDjRng.h"

// The Auto DJ engine's tempo and key preferences (the engine design's 1.4;
// the mobile app's request, mStream's waterfall for one pick): what BPM
// continuity and Harmonic mixing make of one candidate, as a status each
// and one music rank, and D3's rule for a candidate with no BPM or no key.
// Portable, host-tested (test_autodj_tiers); tools/autodj_ref.py is the
// same rule in Python. The values come from a row's filter word (its BPM
// rounded half up, its Camelot code: AutoDjJoin.h).
//
// Tempo, against the playing track's BPM `ref` (re-read every pick; 0, or
// BPM continuity off: Na for every candidate): Tight within 8 of ref, of
// half of it rounded up (Dart's round()) or of double it; Wide within 10 of
// one of them; Wrong otherwise; Unknown with no BPM. Key, against the
// session's Camelot anchor (Harmonic mixing off: Na): Good one of the
// anchor's 6 codes (the same number and one either side, 12 next to 1, in
// either letter; before there is an anchor any known key, the mobile app's
// requireMusicalKey); Wrong otherwise; Unknown with no key.
//
// The music rank (0 best): the waterfall step that first admits the
// candidate (random.js 1005-1066: tight + key, wide + key, tight, wide,
// unrestricted), then its classifyRow tier inside that step (422-450):
//   both on        Good  Unknown  Wrong
//     Tight         0      2       3
//     Wide          1      4       4
//     Unknown       5      6       7
//     Wrong         7      7       7
//   tempo only: Tight 0, Wide 1, Unknown 2, Wrong 3
//   key only:   Good 0, Unknown 1, Wrong 2
//   neither (or both Na): 0
// One difference from mStream, the design's: before a key anchor exists a
// known key is Good, so an Unknown tempo with a known key ranks 5 where
// mStream's "na" key gives it 6 (test_autodj_tiers checks every other cell
// against a port of the waterfall).
//
// D3, the user's decision (2026-10-13): a candidate with no BPM gets a
// tempo status drawn with the odds of this pick's gated candidates that
// have one, one below(nTight + nWide + nWrong) each (u < nTight Tight, u <
// nTight + nWide Wide, else Wrong), in the candidates' order; then every
// candidate with no key the same way (Good / Wrong). With no known value
// among them the status stays Unknown and ranks as above.
namespace autodj {

enum class Tempo : uint8_t { Na, Tight, Wide, Wrong, Unknown };
enum class Key : uint8_t { Na, Good, Wrong, Unknown };

constexpr int kTightBpm = 8;
constexpr int kWideBpm = 10;
constexpr uint8_t kRanks = 8;  // music ranks 0-7; a class is cooldown level x 8 + rank

// The gate's level (docs/AUTODJ-INDEX.md 11.4) for the slider at `percent`
// (hundredths, above 100 read as 100): (255 p + 99) div 100, at least
// `selFloor`.
uint8_t floorLevel(uint32_t percent, uint32_t selFloor);

// ---- statuses ----
struct Window {
  int lo = 0, hi = 0;
};
// The three windows of `ref` at `tolerance`: ref, half (rounded up), double.
void windows(int ref, int tolerance, Window out[3]);
Tempo tempoStatus(uint8_t bpm, uint8_t ref);
// `code` is one of `anchor`'s 6 codes (both 1-24).
bool camelotNear(uint8_t anchor, uint8_t code);
Key keyStatus(uint8_t code, uint8_t anchor, bool on);

// The rank table above as one lookup, [tempo][key] in the enums' order.
inline uint8_t musicRank(Tempo t, Key k) {
  static constexpr uint8_t kRank[5][4] = {
      // Na Good Wrong Unknown
      {0, 0, 2, 1},  // Na: key only
      {0, 0, 3, 2},  // Tight
      {1, 1, 4, 4},  // Wide
      {3, 7, 7, 7},  // Wrong
      {2, 5, 7, 6},  // Unknown
  };
  return kRank[static_cast<int>(t)][static_cast<int>(k)];
}

// tempoStatus(bpm, ref) for every bpm 0-255, as Tempo values: the windows
// filled in (wide, then tight over them), the rest Wrong, 0 Unknown; all Na
// for ref 0. About 120 stores: the engine's table for one pick.
void tempoStatuses(uint8_t ref, uint8_t out[256]);

// keyStatus() against one pick's `anchor` as a mask of its good codes (the
// engine's hot loop); a filter word's 5-bit code above 24 is none
// (Unknown), as everywhere.
class KeyRef {
public:
  KeyRef(uint8_t anchor, bool on);
  bool on() const { return on_; }
  Key status(uint8_t code) const {
    if (!on_) return Key::Na;
    if (code > 24 || !((kKnown >> code) & 1u)) return Key::Unknown;
    return (good_ >> code) & 1u ? Key::Good : Key::Wrong;
  }

private:
  static constexpr uint32_t kKnown = 0x1FFFFFEu;  // codes 1-24
  uint32_t good_ = 0;
  bool on_;
};

// ---- D3 ----
struct TempoOdds {
  uint32_t tight = 0, wide = 0, wrong = 0;
  void add(Tempo t) {
    if (t == Tempo::Tight) ++tight;
    else if (t == Tempo::Wide) ++wide;
    else if (t == Tempo::Wrong) ++wrong;
  }
  uint32_t known() const { return tight + wide + wrong; }
  // One draw (known() above 0).
  Tempo draw(Rng& rng) const {
    const uint32_t u = rng.below(known());
    return u < tight ? Tempo::Tight : u < tight + wide ? Tempo::Wide : Tempo::Wrong;
  }
};
struct KeyOdds {
  uint32_t good = 0, wrong = 0;
  void add(Key k) {
    if (k == Key::Good) ++good;
    else if (k == Key::Wrong) ++wrong;
  }
  uint32_t known() const { return good + wrong; }
  Key draw(Rng& rng) const { return rng.below(known()) < good ? Key::Good : Key::Wrong; }
};

}  // namespace autodj
