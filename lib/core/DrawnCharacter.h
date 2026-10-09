// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceCharacter.h"
#include "DancePack.h"
#include "DanceStage.h"

// A drawn dancer: where a dance pack's move puts its frame on the stage, and
// the Character view of an opened pack (docs/DANCE-SPRITES.md, sections 1.2,
// 2.2-2.3, 6.3-6.4 and 8; Appendices B.2-B.5, B.11 and E.1).
//
// The free functions are tools/dancepack.py's, bit for bit, in integers only:
//   - ease16, hop16, sway16 (B.2, B.3): the eases in Q16 (65536 is 1.0),
//     the bounce's hop (the crab's curve: 0.12 beat on the ground, then a
//     parabola landing on the next beat) and the sway (its extremes on the
//     beats).
//   - frameAt (B.4): for a move at pos16 (Q16 ticks since its run started,
//     never wrapped, for a beat move; Q16 10 ms units for an idle move), in
//     an orientation, from a start spot, at an energy e16 (39322-65536,
//     section 3.6): the step showing and its frame, whether that frame is
//     drawn mirrored, the root (spot + travel + the eased key, sway and hop,
//     scaled by the energy and rounded half up; the orientation mirrors the
//     key, not the sway), the frame's box clamped onto the stage, the shadow
//     (2 px narrower per px of hop) and the character rect, their union
//     (B.5).
//   - extent, fits and startSpotMask (B.11): a bound on the columns a move
//     can draw relative to its root, whether it fits on the stage at a spot,
//     and the spots a travel move can start from (its destination exists,
//     it fits at both ends, and start_at allows it).
// They read only what B uses: the geometry, the frames' boxes and the move's
// record and steps, never pixels. They work on any MOVS record whose steps
// read (1-32 steps inside STEP, durations summing above 0), kept or not; on
// any other record frameAt places frame 0 unmirrored at the spot with no box
// or shadow, extent says none and startSpotMask allows every spot. Where
// dancepack.py raises, they clamp: a negative pos16 counts as 0, a spot or a
// travel's destination outside -1..1 is clamped, and a travel whose `go`
// steps sum to 0 counts as done. The B.0 rules hold throughout: int64_t, and
// floor division (an arithmetic shift) wherever a value can be negative.
//
// DrawnCharacter is the Character of a pack opened by Pack::open (with
// DanceBudget's start-spot hook, and for a card pack after the budget pass):
// its moves are the pack's kept moves at init(), in MOVS order, labelled from
// their records; packMove(i) maps a move back to its MOVS index. It keeps a
// copy of the Pack (a view of the caller's bytes, which must outlive it), the
// labels, the folded genre keys and each move's start spots, and nothing per
// frame. Its frameAt also places the two engine moves on the rest frame:
// kAutoBounce (2.8: one beat, hopping 4 native px) and kAutoIdle (2.6:
// swaying +-1 native px over 4 s). It is at most 6 KB (a static_assert);
// step 4 allocates it in PSRAM.
//
// Portable, no heap, no floats.
namespace dance {

// B.2: ease code 0-4 at f (0..65536). A code above 4 acts as hold.
int32_t ease16(int ease, int32_t f);
// B.3: the hop at phase phi (0..65535) of a move beat; the sway at move beat b (>= 0,
// counted since the run started) and phase phi, the same in either orientation.
int32_t hop16(int32_t phi);
int32_t sway16(int64_t b, int32_t phi);
// B.4 step 8 (section 8's stageX): the stage x of a travel from spot `from` to spot `to`
// (each clamped to -1..1) at progress p16 (0..65536) through `ease`:
// spotX(from) + rhu((spotX(to) - spotX(from)) * ease16(ease, p16), 65536).
int stageX(int from, int to, int32_t p16, int ease);

struct Placement {     // B.4's output
  uint16_t frame;      // the shown step's frame index (bits 0-14)
  bool mirrored;       // drawn mirrored: the step's bit 15 XOR the orientation
  int16_t rootX, rootY;
  StageRect box;       // the frame's box after the clamp; {0, 0, 0, 0} for a blank frame
  StageRect shadow;    // {0, 0, 0, 0} for none
  StageRect rect;      // the character rect: union(box, shadow)
};

// B.4 for MOVS record `move` of an open pack, kept or not.
Placement frameAt(const Pack& pack, int move, int64_t pos16, bool mirrored, int spot,
                  int32_t e16 = 65536);
// B.11. extent: false (lo, hi untouched) when every box is empty.
bool extent(const Pack& pack, int move, bool mirrored, int* lo, int* hi);
bool fits(const Pack& pack, int move, bool mirrored, int spot);
uint8_t startSpotMask(const Pack& pack, int move, bool mirrored);  // bit s + 1 per start spot

// What B.4 reads of a record, read once: frameAt above is readMoveSteps, then
// placeSteps. The budget (DanceBudget) reads a move once and places it frame
// after frame, and DrawnCharacter places its engine moves through it. `ok` is
// false when the record's steps don't read (1-32 steps inside STEP, durations
// summing above 0): placeSteps then gives the unreadable record's placement.
// readMoveSteps decodes the record once (Pack::steps) and precomputes what
// B.4 step 8 sums every frame (the `go` steps' durations), so placeSteps
// does no O(steps) loop: a binary search finds the step, and its divisions
// are 32-bit whenever the values fit (always for a record A.15 keeps).
struct MoveSteps {
  bool ok = false;
  uint8_t kind = 0, beats = 0, flags = 0, bounce = 0, sway = 0, travelEase = 0, startAt = 0;
  int8_t travel = 0;
  uint8_t count = 0;   // steps
  bool anyGo = false;  // some step has `go`; otherwise every step travels (A.13)
  int32_t total = 0;   // their durations' sum, L
  int32_t goTotal = 0;  // Gt: the travelling steps' durations
  StepRecord step[kMaxSteps] = {};
  int32_t start[kMaxSteps] = {};    // s_i
  int32_t goStart[kMaxSteps] = {};  // the travelling steps' durations before step i
};
bool readMoveSteps(const Pack& pack, int move, MoveSteps* out);
Placement placeSteps(const Pack& pack, const MoveSteps& m, int64_t pos16, bool mirrored, int spot,
                     int32_t e16 = 65536);
// B.11's start spots of steps already read (startSpotMask above, without the read).
uint8_t startSpotMask(const Pack& pack, const MoveSteps& m, bool mirrored);

class DrawnCharacter final : public Character {
 public:
  DrawnCharacter();
  // A copy is another view of the same bytes, its labels pointing at its own keys.
  DrawnCharacter(const DrawnCharacter& other);
  DrawnCharacter& operator=(const DrawnCharacter& other);
  // The view of an open pack (State::Open): its kept moves at this moment, in MOVS
  // order. False, and an empty view, otherwise. The pack's bytes must outlive the view.
  bool init(const Pack& pack, Origin origin, bool inCycle);
  const CharacterInfo& info() const override;
  int moveCount() const override;
  const MoveLabels& move(int i) const override;
  uint8_t startSpotMask(int i, bool mirrored) const override;  // B.11's
  int packMove(int i) const;   // move i's MOVS index; -1 out of range
  const Pack& pack() const;
  // B.4 for move i, or for the engine moves kAutoBounce and kAutoIdle (below).
  Placement frameAt(int i, int64_t pos16, bool mirrored, int spot, int32_t e16 = 65536) const;

 private:
  Pack pack_;
  CharacterInfo info_;
  uint8_t count_ = 0;
  int8_t packMove_[kMaxMoves];
  uint8_t spots_[kMaxMoves];  // bits 0-2: startSpotMask unmirrored; bits 4-6: mirrored
  MoveLabels labels_[kMaxMoves];
  uint32_t keys_[kMaxMoves * kMaxGenreTags];  // labels_[i].genreKeys point in here
};

}  // namespace dance
