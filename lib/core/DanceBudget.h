// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "DrawnCharacter.h"

// The per-move budget and the reader's two late move checks
// (docs/DANCE-SPRITES.md, sections 6.6-6.7 and 5.7; Appendices B.6, B.11,
// D.2 and E.1).
//
// The budget is one integer simulation: a move played once (and the frame
// that crosses its end) at a fixed pace and energy 1, frame by frame through
// B.4's frameAt, counting the px B.5's dirty rule would push each frame. It
// runs at 24 and 30 fps (frame periods of 42 and 33 ms), at both ends and the
// middle of the paces the move can be danced at (B.7's reach), in each
// orientation it plays in, and for a travel move from each spot it can start
// from (B.11). Each case gives its lower median and its peak; the move's
// median and peak are the largest of those, and its light is green up to
// 16,000 px, amber up to 20,000 px ("may run 25-29 fps with an MP3"), red
// above. It reads boxes only, never pixels. tools/dancepack.py computes the
// same numbers (budget, trace), and BudgetVectors.h pins both.
//
// The reader's last two move rows (A.15) are this file's:
//   - startSpots, `travels from no spot`: a travel move that can start from
//     no spot in any orientation it plays in can never play. It is cheap
//     (the steps read once), so it runs inside Pack::open() as
//     OpenOptions::late, for every pack.
//   - budgetRed, `budget red (<peak> px)`: a move whose light is red and
//     isn't flagged heavy. A move is up to 24 cases and ~2,900 simulated
//     frames (an 8-beat travel move with mirror), tens of ms on the ESP32,
//     so it runs after open(), in slices of frames while the old dancer
//     keeps dancing (BudgetPass::run), and only for card packs; the device
//     never trusts a file's own budget. Built-in packs skip it
//     (test_stick_pack pins them green).
// The loader's order (D.2's split): open() with OpenOptions{0, &startSpots,
// nullptr}; then BudgetPass::run(kBudgetSliceFrames) once a frame (or in a
// loop under a ~2 ms time budget) until it returns false; then
// DrawnCharacter::init() on the pack. The drops arrive in that order:
// open()'s in MOVS order, then the pass's in MOVS order.
//
// The verdict keeps nothing per frame: budgetRed and BudgetPass track only
// the peak. A case's median needs its values, so budgetCase and budget take
// a caller's scratch of kBudgetMaxFrames values (none: median 0).
//
// Each move's steps are read once (readMoveSteps) and placed frame after
// frame with B.4's placeSteps; pos16 advances by 32-bit additions.
//
// Portable, no heap, no floats.
namespace dance {

constexpr uint32_t kGreenPx = 16000, kAmberPx = 20000;  // B.6's lights
// 2 fps x 3 paces x 2 orientations x 2 spots: a travel move starts from at
// most 2 spots an orientation (s + travel must be a spot), any other from 0.
constexpr int kBudgetMaxCases = 24;
constexpr int kBudgetMaxFrames = 364;  // B.6's largest N: a 12 s idle move at 30 fps
// The slice the loader runs a frame (5.7): ~1-2 ms on the ESP32 at 240 MHz
// (estimate, 10.5 times it).
constexpr uint32_t kBudgetSliceFrames = 256;
enum class BudgetLight : uint8_t { Green, Amber, Red };  // 0, 1, 2, as kMoveBudgets' light
BudgetLight budgetLight(uint32_t peak);  // green <= 16,000 < amber <= 20,000 < red

// B.6's cases of MOVS record `move`, in B.6's order (fps, pace, orientation, spot).
struct BudgetCaseSpec { uint8_t fps; uint8_t pace; bool mirrored; int8_t spot; };  // pace 0: idle
int budgetCaseSpecs(const Pack& pack, int move, BudgetCaseSpec out[kBudgetMaxCases]);

// One case frame by frame: n = 0..N, as dancepack.py's trace().
struct BudgetFrame { uint16_t n; uint32_t pos16; Placement place; uint32_t dirty; };  // dirty 0 at n = 0

// A case over steps the caller keeps (BudgetRun's and BudgetPass's engine).
class BudgetCursor {
 public:
  // From frame 0. No frames for an unreadable record or a beat move with pace 0.
  void begin(const MoveSteps& m, const BudgetCaseSpec& spec);
  // The next frame through placeSteps(pack, m, ...), m the steps begin() had.
  bool next(const Pack& pack, const MoveSteps& m, BudgetFrame* out);  // false after frame N

 private:
  BudgetCaseSpec spec_ = {};
  uint64_t add_ = 0;  // pos16's numerator a frame: period * pace * 65536, or period * 65536 (idle)
  uint32_t div_ = 1;  // its denominator: 5000, or 10
  int64_t pos16_ = 0;
  uint64_t rem_ = 0;  // the numerator's remainder, below div_
  int64_t end_ = 0;   // L * 65536
  int64_t n_ = 0;
  StageRect prev_;
  bool done_ = true;
};

class BudgetRun {
 public:
  // The pack must outlive the run.
  BudgetRun(const Pack& pack, int move, const BudgetCaseSpec& spec);
  bool next(BudgetFrame* out);  // false after frame N

 private:
  const Pack* pack_;
  MoveSteps steps_;
  BudgetCursor cursor_;
};

struct BudgetCaseResult { uint16_t frames; uint32_t median, peak; };  // frames: N
// scratch: kBudgetMaxFrames values, for the median; null: median 0.
BudgetCaseResult budgetCase(const Pack& pack, int move, const BudgetCaseSpec& spec, uint32_t* scratch);
struct BudgetResult { uint32_t median, peak; BudgetLight light; uint8_t cases; };
BudgetResult budget(const Pack& pack, int move, uint32_t* scratch);

// D.2, A.15's last two move rows.
// OpenOptions::late: false, *why = TravelsFromNoSpot, for a beat move with travel != 0
// whose startSpotMask is 0 unmirrored and, when it has kMirror, mirrored too. ctx unused.
bool startSpots(void* ctx, const Pack& pack, int move, Reason* why);
// The verdict: true (drop it), *why = BudgetRed with a = the peak, when the move hasn't
// kHeavy and its B.6 light is red. A heavy move returns false without a simulation.
bool budgetRed(const Pack& pack, int move, Reason* why);

// 5.7's budget pass for a card pack, resumable frame by frame: it keeps the
// move it is on, its cases, the case in progress and the running peak
// (~0.9 KB: the loader allocates it in PSRAM).
class BudgetPass {
 public:
  void begin(Pack* pack, Report* report = nullptr);  // from MOVS record 0
  // Up to maxFrames simulated frames (0 counts as 1) of the next kept move's
  // cases; on the call that finishes its last case, budgetRed's verdict,
  // dropping a red move through pack->dropMove (which reports it). A heavy
  // move, or one with no frames, takes no frames. True while it did
  // something; false, doing nothing, once no kept move is left to check.
  bool run(uint32_t maxFrames);
  // run() to the end of one kept move: each true return checked exactly one.
  bool step();
  bool done() const;

 private:
  int nextKept() const;  // the next kept move from next_, or -1
  bool finish();         // the verdict on move_; true

  Pack* pack_ = nullptr;
  Report* report_ = nullptr;
  int next_ = 0;
  int move_ = -1;  // the move being simulated, or -1
  uint8_t cases_ = 0, case_ = 0;
  bool inCase_ = false;
  uint32_t peak_ = 0;
  BudgetCaseSpec specs_[kBudgetMaxCases] = {};
  MoveSteps steps_;
  BudgetCursor cursor_;
};

}  // namespace dance
