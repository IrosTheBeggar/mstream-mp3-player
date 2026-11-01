// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CodeCharacters.h"

#include <cmath>

namespace dance {
namespace {

// The labels (F.1's table), field for field. Move 0 is the beat move: 2 beats, any
// pace, weight 1.0, flagged freeze, from and to pose key 1 (`stand`, its own twin; code
// characters have no key names). Move 1 is the continuous idle, for quiet and
// listening alike. Neither travels, so startSpotMask is Character's default (0b111),
// freezeMove is 0 and hasTravel is false.
constexpr uint8_t kStand = 1;
constexpr uint8_t kAnyIdle = kWhenQuiet | kWhenListening;

constexpr CharacterInfo kCrabInfo = {"crab", "Crab", "IrosTheBeggar", "GPL-3.0-or-later", "",
                                     Origin::BuiltIn, true, 0};
constexpr MoveLabels kCrabMoves[2] = {
    {"shuffle", MoveKind::Beat, 2, 0, 0, 0, 10, 0, kFreeze, 0, kStand, kStand, kStand, kStand, 0, 0, StartAt::Any,
     0, nullptr},
    {"idle", MoveKind::Idle, 0, 0, 0, 0, 10, kAnyIdle, 0, 0, 0, 0, 0, 0, 0, 0, StartAt::Any, 0, nullptr},
};

constexpr CharacterInfo kStickInfo = {"stick", "Stick", "IrosTheBeggar", "GPL-3.0-or-later", "",
                                      Origin::BuiltIn, true, 0};
constexpr MoveLabels kStickMoves[2] = {
    {"bounce", MoveKind::Beat, 2, 0, 0, 0, 10, 0, kFreeze, 0, kStand, kStand, kStand, kStand, 0, 0, StartAt::Any,
     0, nullptr},
    {"sway", MoveKind::Idle, 0, 0, 0, 0, 10, kAnyIdle, 0, 0, 0, 0, 0, 0, 0, 0, StartAt::Any, 0, nullptr},
};

// move(i) outside 0..1 is outside E.1's contract; it gives move 0 rather than read
// past the table.
int moveIndex(int i) { return i == 1 ? 1 : 0; }

}  // namespace

CodePhase codePhase(double u) {
  const double b = std::floor(u);
  CodePhase p;
  p.phi = static_cast<float>(u - b);
  // floor(u) & 1: the remainder of a whole number by 2 is exact, so this is
  // ((int64_t)floor(u) & 1) != 0 wherever that cast is defined, and defined for any
  // double besides (the cast isn't past 2^63, which a cue's u never reaches).
  p.odd = std::fmod(b, 2.0) != 0.0;
  return p;
}

double DanceHalf::follow(const Cue& c) {
  if (c.beatMove != kNoMove) u_ = c.u;
  return u_;
}

// ---- the crab

const CharacterInfo& CrabCharacter::info() const { return kCrabInfo; }
int CrabCharacter::moveCount() const { return 2; }
const MoveLabels& CrabCharacter::move(int i) const { return kCrabMoves[moveIndex(i)]; }

crab::Pose CrabCharacter::pose(const Cue& c, double u) {
  const CodePhase p = codePhase(u);
  // crab::blend returns the dance pose at w >= 1 and the idle pose at w <= 0, so only
  // the half that shows is worked out (as Crab::update did); between, the blend.
  if (c.weight >= 1.0f) return crab::dancePose(p.phi, p.odd);
  if (c.weight <= 0.0f) return crab::idlePose(c.idleSeconds);
  return crab::blend(crab::idlePose(c.idleSeconds), crab::dancePose(p.phi, p.odd), c.weight);
}

int CrabCharacter::paletteStep(const Cue& c) { return crab::paletteStep(c.weight); }

// ---- the classic stick figure

const CharacterInfo& ClassicStick::info() const { return kStickInfo; }
int ClassicStick::moveCount() const { return 2; }
const MoveLabels& ClassicStick::move(int i) const { return kStickMoves[moveIndex(i)]; }

Pose ClassicStick::pose(const Cue& c, double u) {
  const CodePhase p = codePhase(u);
  // dance::blend lerps every joint, so at t = 1 it may miss the dance pose in the last
  // bit: the ends are the poses themselves (k<n> is Dancer::frozen exactly).
  if (c.weight >= 1.0f) return dancePose(p.phi, p.odd);
  if (c.weight <= 0.0f) return idlePose(c.idleSeconds);
  return blend(idlePose(c.idleSeconds), dancePose(p.phi, p.odd), c.weight);
}

}  // namespace dance
