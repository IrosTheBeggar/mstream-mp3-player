// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DanceCharacter.h"
#include "DanceStage.h"

// The choreographer: which move the dancer plays, where in it, and from which
// spot, frame by frame, from what the beat tracker says (docs/DANCE-SPRITES.md,
// section 3; Appendix E.2 is the exact behaviour, E.3 its determinism).
//
// Each frame DanceMode hands it a BeatInput (is audio heard, is there a beat
// grid, is it locked, the tracker's beat count and tempo) and gets back a Cue:
// the beat move and its position u in move beats, the idle move and its
// milliseconds, the dance weight and energy, whether beat or idle frames show,
// the beat light, and the spot. It owns the dance weight, the idle clock and
// the spot, so a character switch keeps all three.
//
// How it times the dance:
//   - The dance clock counts dance beats: the tracker's beats times the
//     TempoFold scale S (DancePose.h), so it dances at the tracked tempo
//     folded into 72-176 BPM. Runs start and end on its whole beats, and
//     the pseudo-phrase is p = (dance beats - A) mod 8 (3.3).
//   - A move is danced at octave k (-1 half time, 0, +1 double time), chosen
//     by B.7's eligibleK so its pace T * 2^k lands in its range; T is the
//     latched dance tempo, moving only when the live one is 3 % away (2.4).
//     A run's position is u = (clock - start) * 2^k move beats, which is
//     section 3.5's u += delta(trackerBeats) * S * 2^k with S recomputed at
//     every acquisition, so an octave flip of the tracker (I Wonder's 95.7 and
//     191 BPM) leaves the pace alone.
//   - A re-acquire, a jump or a change of S (the fold can flip on a PLL's
//     drift alone, near 72 or 176) re-phases: the new grid's dance beats are
//     chosen among the 1/S whole-tracker-beat shifts, the one nearest the
//     predicted position; within 1/4 beat the run carries on, otherwise it
//     holds and the dancer comes in again on the next beat. A running move
//     whose pace leaves its range by more than 3 % is cut at its next whole
//     move beat (2.5).
//   - The first frame at or after each whole move beat, at most 1/8 beat
//     late, draws exactly that beat (2.2's must-show rule), so every hit
//     shows from its start at 24 and 30 fps (for a render under 1/4 beat
//     late, which a 3-tick step still covers).
// Which frames show (E.2.7): a run starts only with w > 0.5 (Entering also
// waits for the target weight to be above 0.5), and its beat frames then
// show at least to the end of the play; they go off only where a play
// starts and come back on at a whole move beat. So a weight wobbling around
// 0.5 never flickers the dancer, and a travel, once seen, is seen to its
// end.
// What it picks (3.4, 2.8): among the eligible beat moves (pace, genre,
// travel feasibility: never relaxed), those aligned to the phrase, respecting
// `place` and not repeating the last run; then alignment gives way, then
// no-repeat; then the engine's auto-bounce. Weights are integers (weight x
// genre boost x travel bias toward the centre x mirror twin x continuity of
// pose keys x recency), drawn with xorshift32 seeded from the track's path
// hash, the tracker's restarts and its acquisitions. Idle moves play in runs
// of 1-3 loops while nothing is heard (quiet) or no beat is locked
// (listening), never the same twice in a row.
//
// Determinism (E.3): identical calls give identical cues on one build. The
// integers (t10, the phrase, run boundaries, weights, draws) are exact; the
// clock and positions are doubles using only + - * / floor ceil llround; the
// weight is Dancer's float formula.
//
// Out-of-contract inputs (E.2.3) are made safe before use, so no input can
// poison the state or reach an undefined conversion: dt is 0 unless above 0
// (NaN included) and at most 1,000 s (inf included); confidence is clamped to
// 0..1 (NaN to 0); a scale S outside 1/64..64 counts as 1; a frame whose
// trackerBeats isn't below 2^40 in size (NaN, inf) or whose rawBpm isn't in
// 0..1000 has no grid; and a clock that would reach 2^52 dance beats starts
// over as a far jump.
//
// No heap, nothing allocated per frame; at most 1 KB (a static_assert), held
// in DanceMode. It keeps pointers to the characters, never copies: a
// character must outlive its use, a pending switch included.
namespace dance {

struct BeatInput {        // section 3.1: one a frame, from DanceMode::render()
  bool heard;             // audio is heard now
  bool grid;              // the tracker's grid applies to what is heard (meaningful with heard)
  bool locked;            // BeatTracker::locked()
  float confidence;       // BeatTracker::confidence(), 0..1
  double trackerBeats;    // the tracker's beat count at the moment shown (with grid)
  float rawBpm;           // BeatTracker::bpm(): 0 when none
  double foldScale;       // S: TempoFold::apply(rawBpm), a power of two (1/2, 1 or 2)
  uint32_t restarts;      // tracker restarts so far (DanceMode's resets_)
  uint32_t acquisitions;  // BeatTracker::acquisitions()
  float dt;               // seconds since the last frame; 0 on the first
  int8_t freeze;          // the console's k<n>: -1 none, 0..15
};

enum class DanceState : uint8_t { Quiet, Listening, Entering, Dancing, Leaving, Freeze };

struct Cue {              // section 3.1, plus beatPos16
  int8_t beatMove;        // the run's move, kAutoBounce, or kNoMove (none since a reset or switch)
  double u;               // move beats since its run started, as drawn
  uint32_t beatPos16;     // B.4's pos16 for beatMove: floor(u * 786432), exact under a freeze
  int8_t k;               // the run's octave, -1..+1
  bool beatMirror;        // this play's orientation
  int8_t idleMove;        // the idle run's move, kAutoIdle, or kNoMove (no character)
  uint32_t idleMs;        // ms since the idle run started (B.4: pos16 = idleMs * 65536 // 10)
  bool idleMirror;
  float idleSeconds;      // the idle clock, 0..2000 s (code characters' continuous idles)
  float weight;           // w: the dance weight
  uint32_t energy;        // e16: 39322..65536 (3.6), for B.4's e16
  bool showDance;         // beat frames show; otherwise idle frames
  bool flash;             // the beat light's cyan
  int8_t spot;            // the current spot, -1..+1: the running move's start spot
  int16_t rootX, rootY;   // spotX(spot), kStage.groundY: the spot's anchor on the stage
  DanceState state;
};

enum class RunStatus : uint8_t { None, Running, Held };

struct RunInfo {          // the current beat run (status None: the last one's fields)
  int8_t move;            // a move, kAutoBounce, or kNoMove
  int8_t k;
  uint8_t plays;
  bool firstMirror;
  int8_t spot;            // its start spot
  int8_t phrase;          // p at its start, 0..7
  uint8_t level;          // the ladder level that picked it: 0-2, 3 auto-bounce, 4 forced (Dm)
  bool cut;               // its pace failed (2.5)
  RunStatus status;
  int64_t start, end;     // dance-clock beats: where it started, where it ends (E)
  double uEnd;            // move beats where its content ends (plays * beats, or a cut point)
};

class Choreographer {
 public:
  Choreographer();                     // as after reset(0), with no character
  void reset(uint32_t pathHash);       // dance on (E.2.8): everything back but the character
  void onTrack(uint32_t pathHash, const uint32_t* genreKeys, int count);  // every track change
  void setCharacter(const Character* c);                   // 3.7; null: none
  void freeze(int move, uint32_t pos16, bool mirrored);    // Df; kNoMove unfreezes
  void forceNext(int move);            // Dm: the next beat run
  void setSpot(int spot);              // Ds
  void addGenre(uint32_t key);         // Dg: a genre key for this track
  const Cue& update(const BeatInput& in);                  // once a frame
  const Cue& cue() const;              // the last update's
  const Character* character() const;  // the current one, not a pending switch
  const RunInfo& run() const;
  int t10() const;                     // the latched T in tenths, 0 = none yet
  int8_t prevK() const;                // kNoOctave = none
  double danceClock() const;           // E.2's clock
  int8_t eligibleNow(int move) const;  // Dl: the k move would get now, or kNoOctave

 private:
  static constexpr int kTrackGenres = 16, kInjectedGenres = 4;

  // E.2.4's operations and E.2.5-E.2.7's steps.
  void trackReset();
  void hold(double u);
  void cut(double uNow);
  void endRun();
  void doSwitch(const Character* c);
  void pick(int64_t start);
  void startRun(int move, int8_t k, bool mirror, int8_t phrase, uint8_t level, int64_t start);
  void idleStep(bool heard, double dt);
  void idlePick(uint8_t cls);
  void buildCue(bool heard, bool gridOk, bool lockedOk, int8_t freezeK);
  void showStep(double raw);  // E.2.7's show latch at a running frame
  void clearBeatCue();
  void seed(uint32_t v);
  uint32_t below(uint32_t n);
  int8_t moveK(int i) const;  // E.2.5's k_i
  bool genreOk(int i) const;
  const MoveLabels& labels(int move) const;  // a character's move, or auto-bounce's
  double uRaw() const;        // the running run's clamped position
  double scaleK() const;      // 2^k of the run

  // E.2.2's state.
  float w_;
  float idleSec_;
  double clock_;
  double lastTB_, lastS_, lastTl_, sinceGrid_;
  double T_;
  int64_t A_, enterAt_, switchAt_;
  RunInfo run_;
  double prevU_, lastU_;
  uint32_t heldEnergy_;
  int t10_;
  uint32_t acqSeen_, rng_, pathHash_, restartsSeen_, lastAcqIn_, lastEnergy_, dfPos16_;
  double idleMsRun_;  // the idle run's ms
  uint32_t idleRunMs_;
  const Character* character_;
  const Character* pending_;
  uint32_t genres_[kTrackGenres];
  uint32_t injected_[kInjectedGenres];
  Cue base_;  // the cue before any freeze: its beat fields persist while no run exists
  Cue cue_;   // the cue shown
  bool haveClock_, phraseValid_, entering_, leaving_, hasPending_, started_, lastShowDance_, show_;
  bool firstRunBias_, lastFirstMirror_, idleMirror_, dfOn_, dfMirrored_;
  int8_t spot_, lastMove_, prevK_, cutExempt_, idleMove_, lastIdle_, dfMove_, forced_;
  int8_t recent_[3];
  uint8_t lastPlays_, prevTo_, idleCls_, genreCount_, injectedCount_;
};

}  // namespace dance
