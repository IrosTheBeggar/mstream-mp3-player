// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The rules a dance pack's bytes are held to, in one place: the limits of
// the MPDN v1 format (docs/DANCE-SPRITES.md, Appendix A), the bits a move
// record and a step carry, and B.7's reach. tools/dancepack.py enforces the
// same values when it compiles a dancer; the device enforces them again when
// it reads a pack (DancePack), so a pack written by anything else is held to
// exactly what the compiler would have written.
//
// What the numbers mean:
//   - A pack is at most 128 KB (a card pack is loaded whole into the PSRAM
//     arena; two must fit side by side), with at most 16 chunks, 256 frames
//     and 64 moves (so a Pack keeps its moves' kept bits in one uint64_t),
//     32 steps a move and 8 genre tags a move.
//   - Geometry is in screen px: a cell is at most 160 px wide or high at its
//     scale (1-4); the cell's top stands at most 153 px above the ground
//     (stage row 153 of 0-163) and at most 10 px of it hang below; a shadow
//     is at most 160 px wide; a step's key moves the frame at most 48 px.
//   - Strings are counted in UTF-8 bytes, without their NUL.
//   - Beat moves count time in ticks, 1/12 of a move beat, at least 3 a step
//     (a quarter beat); idle (no-BPM) moves in 10 ms units, 200-5,000 ms a
//     step and 500-12,000 ms in all.
//   - A move's pace bounds are 36-200 BPM of its own beats (section 2.4);
//     the paces the engine can actually dance are 36-180 (reach, below).
//
// The record bits keep section 1.1's names. Step 3's DanceCharacter.h
// includes this header instead of declaring them again. There are no stage
// constants here (the ground row, the stage's width): those belong to
// DanceStage.h, and this header must not clash with it or with DancePose's
// kHopPx.
//
// Portable, header-only, no allocation.
namespace dance {

// MPDN v1 (Appendix A)
constexpr uint16_t kReaderVersion = 1;  // A.2: minReader <= this
constexpr uint32_t kMaxPackBytes = 131072;
constexpr int kMaxChunks = 16, kMaxFrames = 256, kMaxMoves = 64, kMaxSteps = 32, kMaxGenreTags = 8;
constexpr int kPalsBytes = 96, kThumbBytes = 800, kThumbSide = 40;
// geometry (A.6), in screen px
constexpr int kMaxScale = 4, kMaxCellPx = 160, kMaxAnchorPx = 153, kMaxBelowGroundPx = 10;
constexpr int kMaxShadowPx = 160, kMaxOffsetPx = 48;
// strings (A.5, A.11), in UTF-8 bytes
constexpr int kMaxIdBytes = 23, kMaxNameBytes = 24, kMaxAuthorBytes = 40, kMaxLicenceBytes = 40;
constexpr int kMaxUrlBytes = 80, kMaxVersionBytes = 16, kMaxKeyBytes = 23, kMaxTagBytes = 63;
// moves (section 2, A.12, A.13)
constexpr int kTicksPerBeat = 12, kMinStepTicks = 3;
constexpr int kMinIdleStep = 20, kMaxIdleStep = 500;     // 10 ms units
constexpr int kMinIdleTotal = 50, kMaxIdleTotal = 1200;  // 10 ms units
constexpr int kMinPace = 36, kMaxPace = 200;
constexpr int kMaxWeight10 = 100, kMaxRepeat = 8, kMaxRepeatBeats = 16;
constexpr int kMaxBounce = 16, kMaxSway = 16, kMaxTravel = 2, kEaseCount = 5, kStartAtCount = 5;
enum : uint8_t { kMirror = 1, kFreeze = 2, kPlaceStart = 4, kPlaceEnd = 8, kHeavy = 16, kSnap = 32 };
enum : uint8_t { kWhenQuiet = 1, kWhenListening = 2 };  // 3 = any
enum : uint8_t { kNeedsSongBpm = 1 };
constexpr uint16_t kFrameMirrored = 0x8000;  // STEP's frame word and GEOM's rest frame: bit 15
constexpr uint8_t kStepGo = 1;               // STEP's flags: bit 0

// B.7's reach: the paces a move can be danced at, [*rlo, *rhi]; false for
// none. `travels` is travel != 0 whatever its value, as dancepack.py reads it.
//
// Each octave k covers its own band of paces: half time (k = -1) 36-88,
// k = 0 72-176, double time (k = +1) 144-180. A move may use:
//   - k = 0, unless it travels with fewer than 2 beats;
//   - k = -1 when it is ranged (lo, hi not both 0) and has at most 4 beats,
//     so no single play is longer than 8 dance beats;
//   - k = +1 when it is ranged and doesn't travel, or travels with at least
//     4 beats (section 8: a travel must last at least 2 dance beats).
// The bands it may use always join into one interval [a, b]. An unranged
// move reaches all of it; a ranged move [lo, hi] reaches [max(lo, a),
// min(hi, b)], or nothing when that is empty (A.15's `range <lo>-<hi>
// unreachable`). Either pointer may be null.
inline bool reach(int lo, int hi, int beats, bool travels, int* rlo, int* rhi) {
  const bool ranged = !(lo == 0 && hi == 0);
  bool any = false;
  int a = 0, b = 0;
  const auto band = [&](int bandLo, int bandHi) {
    a = any && a < bandLo ? a : bandLo;
    b = any && b > bandHi ? b : bandHi;
    any = true;
  };
  if (!(travels && beats < 2)) band(72, 176);
  if (ranged && beats <= 4) band(36, 88);
  if (ranged && (!travels || beats >= 4)) band(144, 180);
  if (!any) return false;
  int l = a, h = b;
  if (ranged) {
    l = lo > a ? lo : a;
    h = hi < b ? hi : b;
    if (l > h) return false;
  }
  if (rlo) *rlo = l;
  if (rhi) *rhi = h;
  return true;
}

}  // namespace dance
