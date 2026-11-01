// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceChoreo.h"
#include "DancePose.h"  // TempoFold
#include "DanceRate.h"  // dancerate::Scene

// DanceMode's frame between what it measures and what the Choreographer
// reads (docs/DANCE-SPRITES.md, Appendices E.1 and F.1), here so the host
// tests run the same steps as the device.
//
// Each frame DanceMode::render() gathers the facts: whether audio is heard at
// the moment the frame will be on the LCD, whether the tracker's grid applies
// to it and where on that grid it is, the tracker's lock, confidence and
// tempo, its restart and acquisition counts, dt, and the console's k<n>.
// Gathering them stays in src/ (the tap reader, the computer's heard clock,
// the output's latency, esp_timer); what they become is here:
//   - beatInput: the Choreographer's BeatInput (E.1's table). Every field is
//     copied; a k outside 0..15 is none; and S, the dance tempo's octave
//     scale, is the TempoFold applied to the tracker's tempo only on frames
//     with a grid, as 0.7.0's render() applied it only inside `if (beat)`.
//     Without a grid the fold's last scale goes in and the fold doesn't move.
//     It never resets the fold: DanceMode::restart() and hostRestart() do,
//     as before.
//   - sceneOf: DanceRate's scene (the next frame's rate) from the cue. Its
//     `frozen` is k<n> only: an Mf freeze shows through the cue's weight (1 on
//     a beat move, dancing; 0 on an idle one).
// The facts are gathered every frame, frozen or not: the Choreographer runs
// on underneath a freeze (E.2.7).
//
// Portable, no heap; floats where BeatInput has them.
namespace dance {

struct FrameFacts {        // what DanceMode::render() reads for a frame, before the Choreographer
  bool heard;              // audio is heard at the moment shown
  bool grid;               // today's `beat`: heard, the grid valid and not fresh, the epoch matching
  double trackerBeats;     // today's `beats` with grid; 0 without
  bool locked;             // tracker_.locked()
  float confidence;        // tracker_.confidence()
  float rawBpm;            // tracker_.bpm()
  uint32_t restarts;       // resets_
  uint32_t acquisitions;   // tracker_.acquisitions()
  float dt;                // today's dt
  int8_t freeze;           // frozen_: -1, or k<n>'s 0..15
};

// E.1's table: every field copied, freeze = f.freeze when 0..15 else -1, and foldScale =
// f.grid ? fold.apply(f.rawBpm) : fold.scale(): the fold applied only on frames with a
// grid, as today's render() applies it only inside `if (beat)`. It never resets the
// fold: DanceMode::restart() and hostRestart() still do, as today.
BeatInput beatInput(const FrameFacts& f, TempoFold& fold);
// DanceRate's scene for a frame drawn from cue c: frozen = f.freeze in 0..15 (k<n>; an Mf
// freeze shows through the cue's weight: 1 on a beat move, 0 on an idle one), beat =
// f.grid, locked = f.locked, weight = c.weight.
dancerate::Scene sceneOf(const FrameFacts& f, const Cue& c);

}  // namespace dance
