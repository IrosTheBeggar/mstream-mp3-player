// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The dance stage: where the dancer stands and which pixels a frame dirties
// (docs/DANCE-SPRITES.md, sections 6.1 and 8, Appendices B.4, B.5 and E.1).
//
// The stage is one RGB565 canvas, 320x164, at screen (0, 36): the whole
// width above the Dance tab's 40 px rail. Its ground is stage row 153 (the
// screen's 189, where both of today's dancers draw their ground line), and
// it has three spots, the anchor x a dancer stands at: 64, 160 and 256, 96 px
// apart, so a 128 px character just fits at either edge spot. Travel is
// counted in spots, never in px, so packs survive later stage changes.
//
// What lives here from step 3 is only what the engine's integer algorithms
// need, and what tools/dancepack.py computes the same way:
//   - StageGeom and kStage: B.4 places a frame on kStage (its width, height,
//     ground row and spots); kLegacyStage is section 6.1's old 120x150 box,
//     for step 5's pixel-parity check, and nothing in step 3 reads it.
//   - StageRect and B.5's rules: a rect is half-open, [x0, x1) x [y0, y1) in
//     stage px, empty when x1 <= x0 or y1 <= y0, and {0, 0, 0, 0} is how
//     "none" is written (a blank frame's box, a shadow narrowed away). The
//     union of two rects is their bounding rect, an empty one adding
//     nothing.
//   - The dirty rule (6.3, B.5): this frame's character rect B and the last
//     one's A are pushed as their union U, unless U is more than 1.3x the
//     sum of the two (a switch across spots, a fast travel), when they stay
//     two rects. The budget (B.6) counts the px that rule pushes.
//
// Step 5 adds the hold plan, inStage() and DanceView's needs beside these,
// without changing them. Header-only, portable, no allocation.
namespace dance {

// The dance stage (section 6.1): where it sits on the screen, its ground row and its
// three spots' anchor x, all in px. Stage coordinates run from the stage's top-left.
struct StageGeom {
  int16_t x, y, w, h;   // the stage on the screen
  int16_t groundY;      // the ground row; standing feet's last row is groundY - 1
  int16_t spotX[3];     // spots -1, 0, +1
};
constexpr StageGeom kStage = {0, 36, 320, 164, 153, {64, 160, 256}};       // final: B.4's stage
constexpr StageGeom kLegacyStage = {100, 44, 120, 150, 145, {60, 60, 60}};  // step 5's parity rect

// kStage.spotX[spot + 1], spot clamped to -1..1.
inline int spotX(int spot) {
  const int s = spot < -1 ? -1 : (spot > 1 ? 1 : spot);
  return kStage.spotX[s + 1];
}

// B.5's rect: half-open [x0, x1) x [y0, y1) in stage px, empty when x1 <= x0 or
// y1 <= y0. {0, 0, 0, 0} is how "none" is written.
struct StageRect { int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

inline bool rectEmpty(const StageRect& r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }

// 0 when empty.
inline int32_t rectArea(const StageRect& r) {
  if (rectEmpty(r)) return 0;
  return (static_cast<int32_t>(r.x1) - r.x0) * (static_cast<int32_t>(r.y1) - r.y0);
}

// The bounding rect; an empty one adds nothing; two empty ones give {0, 0, 0, 0}.
inline StageRect rectUnion(const StageRect& a, const StageRect& b) {
  const bool ea = rectEmpty(a), eb = rectEmpty(b);
  if (ea && eb) return StageRect{};
  if (ea) return b;
  if (eb) return a;
  StageRect u;
  u.x0 = a.x0 < b.x0 ? a.x0 : b.x0;
  u.y0 = a.y0 < b.y0 ? a.y0 : b.y0;
  u.x1 = a.x1 > b.x1 ? a.x1 : b.x1;
  u.y1 = a.y1 > b.y1 ? a.y1 : b.y1;
  return u;
}

// B.5's dirty rule: the px a frame dirties, A the last frame's character rect (null:
// no last frame, which dirties area(now)) and B this frame's. U = union(A, B); when
// 10 * area(U) > 13 * (area(A) + area(B)) (in 64 bits) they stay two rects,
// area(A) + area(B); otherwise area(U). As dancepack.py's dirty_px.
inline uint32_t dirtyPx(const StageRect* last, const StageRect& now) {
  const StageRect none;
  const StageRect& a = last ? *last : none;
  const int64_t areaA = rectArea(a), areaB = rectArea(now);
  const int64_t areaU = rectArea(rectUnion(a, now));
  if (10 * areaU > 13 * (areaA + areaB)) return static_cast<uint32_t>(areaA + areaB);
  return static_cast<uint32_t>(areaU);
}

}  // namespace dance
