// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceRules.h"  // the record bits: kMirror ... kSnap, kWhenQuiet, kWhenListening, kNeedsSongBpm

// A dancer, as the choreographer sees it (docs/DANCE-SPRITES.md, section 1.1
// and Appendix E.1): a list of moves with their labels, and nothing about how
// they are drawn. Two classes implement it:
//   - drawn characters (DrawnCharacter): frames from a dance pack, timed to
//     the beat, with the engine adding the motion (offsets, hops, sways,
//     travel);
//   - code characters (step 4: the crab, stick-classic): procedural poses,
//     with one beat move and one continuous idle each.
// The interface doesn't draw. Each class has a renderer in src/ui, which
// reads the Choreographer's Cue (DanceChoreo.h) and draws the move it names,
// so the anti-aliased classic figure's drawing stays out of lib/core.
//
// What a move's labels mean to the picker (sections 2 and 3.4):
//   - kind: a beat move (1, 2, 4 or 8 move beats, timed to the tracked beat)
//     or an idle (no-BPM) move, shown when nothing is heard (`when` quiet) or
//     no beat is locked (listening), 500-12,000 ms a play; a code character's
//     idle may be continuous (idleMs 0).
//   - paceLo..paceHi: the move's danced pace, how many of its own beats go by
//     in a minute (2.4); 0, 0 is any pace. The engine dances it at half, the
//     same or double the dance tempo to land in that range (B.7).
//   - weight10 (0.1-10.0), repeat, the flags (mirror, freeze, place start or
//     end, heavy, snap), the pose keys a move starts and ends on (fromTag,
//     toTag; fromTagM, toTagM in the mirrored orientation, where left and
//     right swap: the pack stores both, A.12), needs (feature bits this
//     engine lacks: such a move is never used), travel in spots (negated
//     when mirrored) with where it may start (startAt), and the folded genre
//     keys it is tied to (B.8; untagged moves play on any track).
// The record bits are DanceRules.h's, shared with the pack reader.
//
// startSpotMask(i, mirrored) answers section 8's travel test, which needs the
// art: the spots a move can start from in an orientation. This default knows
// no art; DrawnCharacter overrides it with B.11's, which adds the fit on the
// stage.
//
// Move indices are 0..moveCount() - 1, at most 64. Three negative values name
// moves that aren't a character's: kNoMove (none), kAutoBounce (2.8: the rest
// frame hopping on every beat, when nothing is eligible) and kAutoIdle (2.6:
// the rest frame swaying, when a character has no idle move).
//
// Header-only, portable, no allocation.
namespace dance {

enum class MoveKind : uint8_t { Beat, Idle };                     // A.12's kind 0, 1
enum class Origin : uint8_t { BuiltIn, Card };
enum class StartAt : uint8_t { Any, Centre, Side, Left, Right };  // A.12's startAt 0-4; mirrored: Left <-> Right
constexpr int8_t kNoMove = -1, kAutoBounce = -2, kAutoIdle = -3;  // move indices that aren't a character's

struct MoveLabels {           // what the picker reads: the same for both classes
  const char* name;
  MoveKind kind;
  uint8_t beats;              // Beat: 1, 2, 4 or 8 move beats; Idle: 0
  uint16_t idleMs;            // Idle: 500..12000, 10 x its steps' sum; 0 = continuous (code characters only); Beat: 0
  uint8_t paceLo, paceHi;     // the danced pace, 36..200; 0, 0 = any (Idle: 0, 0)
  uint8_t weight10;           // 1..100 = 0.1..10.0
  uint8_t when;               // Idle: kWhenQuiet | kWhenListening (3 = any); Beat: 0
  uint8_t flags;              // kMirror, kFreeze, kPlaceStart, kPlaceEnd, kHeavy, kSnap
  uint8_t repeat;             // 0 (auto) .. 8, repeat * beats <= 16
  uint8_t fromTag, toTag;     // pose key ids, 0 = none
  uint8_t fromTagM, toTagM;   // their mirror twins' ids (A.11, A.12), for the mirrored orientation
  uint8_t needs;              // feature bits: any bit set and the picker never uses the move
  int8_t travel;              // -2..+2 spots, negated when mirrored
  StartAt startAt;
  uint8_t genreCount;         // 0..8
  const uint32_t* genreKeys;  // genreCount B.8 keys (genreKey()), never FNV(""); null when 0
};

struct CharacterInfo {
  const char* id; const char* name; const char* author; const char* licence; const char* url;
  Origin origin; bool inCycle; uint8_t ignoredMoves;
};

// B.11's start_at as spots, bit s + 1 for spot s: Any 0b111, Centre 0b010, Side 0b101,
// Left 0b001 (0b100 mirrored), Right 0b100 (0b001 mirrored); 0 for a value above Right.
inline uint8_t startAtMask(StartAt a, bool mirrored) {
  switch (a) {
    case StartAt::Any: return 0b111;
    case StartAt::Centre: return 0b010;
    case StartAt::Side: return 0b101;
    case StartAt::Left: return mirrored ? 0b100 : 0b001;
    case StartAt::Right: return mirrored ? 0b001 : 0b100;
  }
  return 0;
}

class Character {  // the interface doesn't draw: each class has a renderer in src/ui
 public:
  virtual ~Character() = default;
  virtual const CharacterInfo& info() const = 0;
  virtual int moveCount() const = 0;                // 0..64
  virtual const MoveLabels& move(int i) const = 0;  // 0 <= i < moveCount()
  // The spots move i can start from in an orientation, bit s + 1 for spot s: 0b111 for an
  // idle move and for a move without travel. This default knows no art: for a travel
  // move, every s whose s + travel * sgn is a spot (sgn -1 when mirrored) and that
  // startAtMask allows. DrawnCharacter overrides it with B.11's, which adds the fit.
  // (0 for an i out of range.)
  virtual uint8_t startSpotMask(int i, bool mirrored) const {
    if (i < 0 || i >= moveCount()) return 0;
    const MoveLabels& m = move(i);
    if (m.kind != MoveKind::Beat || m.travel == 0) return 0b111;
    const int t = mirrored ? -m.travel : m.travel;
    const uint8_t allowed = startAtMask(m.startAt, mirrored);
    uint8_t mask = 0;
    for (int s = -1; s <= 1; ++s) {
      const int to = s + t;
      if (to >= -1 && to <= 1 && (allowed & (1u << (s + 1)))) mask |= static_cast<uint8_t>(1u << (s + 1));
    }
    return mask;
  }
};

// 2.9's freeze move: the first beat move flagged kFreeze; else the first unranged 2-beat
// beat move; else the first unranged beat move; else kAutoBounce. Moves with a needs
// bit are skipped. (Unranged: paceLo = paceHi = 0.)
inline int freezeMove(const Character& c) {
  const int n = c.moveCount();
  for (int pass = 0; pass < 3; ++pass) {
    for (int i = 0; i < n; ++i) {
      const MoveLabels& m = c.move(i);
      if (m.kind != MoveKind::Beat || m.needs != 0) continue;
      const bool unranged = m.paceLo == 0 && m.paceHi == 0;
      if (pass == 0 && (m.flags & kFreeze)) return i;
      if (pass == 1 && unranged && m.beats == 2) return i;
      if (pass == 2 && unranged) return i;
    }
  }
  return kAutoBounce;
}

// Whether any beat move without a needs bit has travel != 0.
inline bool hasTravel(const Character& c) {
  const int n = c.moveCount();
  for (int i = 0; i < n; ++i) {
    const MoveLabels& m = c.move(i);
    if (m.kind == MoveKind::Beat && m.needs == 0 && m.travel != 0) return true;
  }
  return false;
}

}  // namespace dance
