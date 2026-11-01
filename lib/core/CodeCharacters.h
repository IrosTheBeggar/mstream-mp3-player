// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CrabPose.h"
#include "DanceChoreo.h"  // Cue, and DanceCharacter.h
#include "DancePose.h"

// The code characters: today's two dancers behind the Choreographer's
// character interface (docs/DANCE-SPRITES.md, sections 1.3-1.4 and
// Appendix F.1). Their art stays code: the crab is CrabPose's pixel art and
// the classic stick figure DancePose's joints, both pure functions of where
// the music is in the beat. What is new is only the mapping from the
// Choreographer's Cue to those functions, here in lib/core so the host tests
// pin it and step 5's renderers call the same functions.
//
// Each declares two moves, so the Choreographer can treat it like a drawn
// dancer:
//   - move 0, the beat move (the crab's `shuffle`, the stick's `bounce`): 2
//     move beats, any pace, flagged freeze (so k<n> freezes it), from and to
//     pose key 1 (`stand`, its own mirror twin);
//   - move 1, the idle (`idle`, `sway`): continuous (idleMs 0), shown when
//     nothing is heard and when no beat is locked.
// Neither travels, so the Choreographer keeps them at the centre spot.
//
// The pose a cue gives, in three parts:
//   - codePhase(u): the dance half's place in its two-beat cycle. phi is
//     frac(u) and odd is floor(u) & 1, the move beat inside the run, so the
//     claw (or the arm) that punches no longer follows the tracker's beat
//     index and doesn't flip on a re-acquire.
//   - DanceHalf: the u the dance half is drawn at, kept from frame to frame.
//     While the cue names a beat move it is the cue's u; when the cue names
//     none (a track reset, a switch: E.2.4 clears the beat fields) it keeps
//     the last one, so the dance half holds its last pose while the weight
//     fades, as Crab::update and Dancer::update held lastPhi and lastOdd.
//     Before the first run after dance-on it rests at 0, the hit, so the
//     crab crouches into the hit and comes in on the beat.
//   - pose(c, u): the idle and the dance half blended by the cue's weight,
//     the idle at the cue's idleSeconds (the Choreographer's idle clock,
//     which runs on through pauses and switches; never idleMs). At weight 1
//     and above it is exactly the dance pose, at 0 and below exactly the
//     idle pose, so k<n> (weight 1, u = n/8) gives Crab::frozen and
//     Dancer::frozen to the bit; the stick's w >= 1 branch matters, because
//     dance::blend's a + (b - a) can miss b in the last bit.
// The functions read only u, the weight and idleSeconds, and ignore the rest
// of the cue (showDance, the energy, the spot, the idle move, mirroring), as
// a code character may (E.1).
//
// The characters themselves hold nothing but their vtable: their labels and
// infos are constexpr tables in flash. DanceMode allocates them in PSRAM.
// Portable, no heap; floats only where CrabPose and DancePose already use
// them (the poses, the weight, the idle seconds).
namespace dance {

// A code character's dance half at move-beat position u (u >= 0, as every cue's):
// 1.3's phi = frac(u) and odd = floor(u) & 1, odd counting move beats from the run's
// start. phi = (float)(u - floor(u)); odd = ((int64_t)floor(u) & 1) != 0.
struct CodePhase {
  float phi;
  bool odd;
};
CodePhase codePhase(double u);

// Where a code character's dance half is drawn, kept from frame to frame: the cue's u
// while the cue names a beat move (beatMove != kNoMove: a run, a run that ended (its
// fields stay, E.2.7), or a freeze); otherwise the last such u, 0 before any. So the
// dance half holds its last pose while the weight fades, through a track reset and a
// switch too, which set the cue's beat fields to kNoMove (E.2.4), as Crab::update and
// Dancer::update hold lastPhi and lastOdd today.
class DanceHalf {
 public:
  double follow(const Cue& c);  // this frame's u (and remembers it)
  double u() const { return u_; }  // the last follow()'s
  void reset() { u_ = 0.0; }       // u = 0 (dance on)

 private:
  double u_ = 0.0;
};

// The crab (CrabPose, CrabArt): id `crab`, the first-boot dancer.
class CrabCharacter final : public Character {
 public:
  const CharacterInfo& info() const override;
  int moveCount() const override;                // 2
  const MoveLabels& move(int i) const override;  // 0 shuffle, 1 idle
  // crab::blend(crab::idlePose(c.idleSeconds), crab::dancePose(p.phi, p.odd), c.weight),
  // p = codePhase(u).
  static crab::Pose pose(const Cue& c, double u);
  static int paletteStep(const Cue& c);          // crab::paletteStep(c.weight)
};

// Today's stick figure (DancePose), id `stick` (step 7: `stick-classic`, out of the tap
// cycle, when the drawn stick figure takes the id).
class ClassicStick final : public Character {
 public:
  const CharacterInfo& info() const override;
  int moveCount() const override;                // 2
  const MoveLabels& move(int i) const override;  // 0 bounce, 1 sway
  // p = codePhase(u), the default dance::Box (DanceView's): c.weight >= 1:
  // dance::dancePose(p.phi, p.odd); c.weight <= 0: dance::idlePose(c.idleSeconds);
  // otherwise dance::blend(dance::idlePose(c.idleSeconds), dance::dancePose(p.phi, p.odd), c.weight).
  static Pose pose(const Cue& c, double u);
};

}  // namespace dance
