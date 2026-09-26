#pragma once
#include <cmath>
#include <cstdint>

#include "CrabArt.h"

// The dancing crab: which layer frames to draw and where, as pure functions
// of where the music is in the beat, the same shape as DancePose. The art
// (palette, frames, the rig's geometry) is CrabArt, generated from
// tools/art/crab.json. Nothing here draws: forEachLayer() and blit() hand
// integer rectangles of one palette index to any canvas with fillRect(), and
// ui/DanceView maps the indices to RGB565 for its sprite.
//
// A two-beat cycle, u = parity + phi. Contact and the extremes land ON the
// beat (phi = 0):
//   - On the ground and squashed (body_squash, legs_splay, open grin) for
//     phi < 0.12, then a ballistic hop of 9 screen px that peaks at phi 0.56
//     and lands hard exactly on the next beat.
//   - A side-step of +-6 px: the even beat travels left to right, the odd one
//     right to left, so the crab is at a side extreme on every beat.
//   - Anticipation: body_stretch with an "o" mouth from phi 0.84.
//   - The left claw punches up on even beats, the right on odd ones: at its
//     top and snapped shut (claw_closed) on the beat, with a white and gold
//     spark over it while squashed.
//   - The eye stalks trail the body by 0.12 beat (the lagging secondary
//     element); mid-air the pupils look where the crab travels, and on odd
//     beats the face squints (^ ^) on impact.
// With no beat (low confidence) it blends over ~0.4 s into an idle loop:
// breathing with the feet planted, claws tucked and twitching, glances,
// blinks, and a dimmed palette.
namespace crab {

struct Pose {
  float dx = 0.0f, dy = 0.0f;  // screen px, the whole crab (side-step, hop)
  float bob = 0.0f;            // screen px, every layer but the legs (idle breathing)
  FrameId body = kBody, eyes = kEyes, legs = kLegs;
  bool eyesFlip = false;       // eyes_look mirrored: looking left
  int eyeX = 0, eyeY = 0;      // native px, the eye layer against the body
  int liftL = 0, liftR = 0;    // native px, claws up (negative: tucked down)
  bool snapL = false, snapR = false;  // claw_closed instead of claw_open
  int fx = 0;                  // snap spark: 0 none, -1 over the left claw, +1 over the right
};

// Dance tuning (screen px for the whole crab, native px for parts, beats for
// phases). Exposed for the tests.
constexpr float kHopPx = 9.0f;        // hop height (screen px)
constexpr float kDwell = 0.12f;       // on the ground for phi < kDwell
constexpr float kShufflePx = 6.0f;    // side-step, either side of the centre (screen px)
constexpr int kClawLift = 8;          // a claw's punch (native px)
constexpr int kClawRest = 1;          // a claw's lift between punches
constexpr int kIdleLift = -3;         // idle: claws tucked low
constexpr float kClawRise = 0.22f;    // beats of rise before its beat
constexpr float kClawFall = 0.7f;     // beats of fall after it
constexpr float kEyeLag = 0.12f;      // beats the eye stalks trail the body
constexpr float kSquashEnd = 0.12f;   // body_squash / legs_splay for phi < this
constexpr float kStretchStart = 0.84f;  // body_stretch from here to the beat
constexpr float kSnapEnd = 0.16f;     // the beat's claw is shut until here
constexpr float kFxEnd = 0.12f;       // the spark shows until here
constexpr float kLookStart = 0.2f, kLookEnd = 0.8f;  // eyes_look, toward the travel
constexpr float kAirStart = 0.3f, kAirEnd = 0.7f;    // legs_air (tucked) mid-hop

// On the beat: phi in [0, 1) since the last beat, `odd` that beat's parity.
Pose dancePose(float phi, bool odd);
// The idle loop, `seconds` into it.
Pose idlePose(float seconds);
// w: 0 idle .. 1 dancing. Offsets and lifts lerp; the frames (and flips,
// snaps, spark) switch at w = 0.5.
Pose blend(const Pose& idle, const Pose& dance, float w);

// The palette for a dance weight: kPaletteIdle at 0 .. kPalette at 1, per
// channel, in kPaletteSteps + 1 steps (so the view only reloads it when the
// step changes). Entries the view draws itself (shadow, ground, beat dot) are
// the same in both.
constexpr int kPaletteSteps = 32;
int paletteStep(float w);
void paletteAt(int step, uint8_t out[kPaletteSize][3]);

// Box pixels, [x0, x1) x [y0, y1).
struct Rect {
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  bool empty() const { return x1 <= x0 || y1 <= y0; }
};
Rect unite(const Rect& a, const Rect& b);
// Everything the crab's layers cover, clipped to the box.
Rect bounds(const Pose& p);
// The contact shadow (kShadowIx), drawn over the ground line before the crab:
// kShadowW px wide on the ground, narrower the higher the hop. Not clipped.
Rect shadowRect(const Pose& p);
// What a frame of this pose draws (layers and shadow), clipped to the box:
// the dirty rectangle is this now united with the last frame's.
Rect drawnRect(const Pose& p);

// Calls draw(FrameId, x, y, flip, repeat) back to front. x, y: the scaled
// frame's top-left in box pixels; repeat: the frame stacked that many times
// downwards (the arms, a one-row tile; 1 for everything else).
template <class Draw>
void forEachLayer(const Pose& p, Draw&& draw);

// Draws a layer into anything with fillRect(x, y, w, h, colour), where the
// colour is the palette index: kScale x kScale blocks, index 0 left alone
// (transparent). A run of one index in a row is one rectangle, and so is the
// same run repeated in the rows below it (~210 rectangles for a whole crab).
template <class Canvas>
void blit(Canvas& c, FrameId id, int x0, int y0, bool flip, int repeat = 1);

// The state around the pure functions, as dance::Dancer: an idle clock and a
// dance weight that follows the tracker's confidence (0 below 0.3, 1 above
// 0.7) over ~0.4 s. Without a beat, the dance half holds its last pose while
// it fades.
class Crab {
public:
  // One frame, dt seconds after the last. `beat`: phi/odd are meaningful.
  Pose update(float phi, bool odd, float confidence, bool beat, float dt);
  // A fixed pose for screenshots: full dance weight, no smoothing.
  static Pose frozen(float phi, bool odd) { return dancePose(phi, odd); }
  float weight() const { return weight_; }
  // `weight`: start there (a skin switch hands over the other skin's weight).
  void reset(float weight = 0.0f);

private:
  float weight_ = 0.0f;
  float idle_ = 0.0f;  // seconds of idle clock
  float lastPhi_ = 0.0f;
  bool lastOdd_ = false;
};

// ---- templates

// Round half up, as the Python rig the crab was designed with (the tests hold
// the port to it pixel for pixel).
inline int rnd(float x) { return static_cast<int>(std::floor(x + 0.5f)); }

template <class Draw>
void forEachLayer(const Pose& p, Draw&& draw) {
  const int ox = kOriginX + rnd(p.dx), oy = kOriginY + rnd(p.dy), bob = rnd(p.bob);
  auto at = [&](FrameId f, int x, int y, bool flip, int repeat) {
    const bool leg = f == kLegs || f == kLegsSplay || f == kLegsAir;  // the feet stay put
    draw(f, ox + x * kScale, oy + y * kScale + (leg ? 0 : bob), flip, repeat);
  };
  const int bi = p.body - kBody;
  const int bx = kBodyXY[bi][0], by = kBodyXY[bi][1];
  at(p.legs, kLegsX, kLegsY, false, 1);
  const int ty = by - kClawBelowTop;  // a claw's top at lift 0
  for (int side = 0; side < 2; ++side) {  // right, then left (mirrored)
    const bool left = side == 1;
    const int cy = ty - (left ? p.liftL : p.liftR);
    const int cx = left ? -kClawX - kClawW : kClawX;
    const int ax = left ? cx + kClawW - kArmCol - kArmW : cx + kArmCol;
    // The arm: from under the pincer down behind the body.
    const int rows = by + kArmEnd - (cy + kClawH) + 1;
    if (rows > 0) at(kArm, ax, cy + kClawH, false, rows);
    at((left ? p.snapL : p.snapR) ? kClawClosed : kClawOpen, cx, cy, left, 1);
  }
  at(p.eyes, kEyesDx + p.eyeX, by - kEyesAbove + p.eyeY, p.eyesFlip, 1);
  at(p.body, bx, by, false, 1);
  if (p.fx != 0) {
    const int lift = p.fx < 0 ? p.liftL : p.liftR;
    const int cx = p.fx < 0 ? -kClawX - kClawW : kClawX;
    at(kFxSnap, cx + (kClawW - kFxW) / 2, ty - lift - kFxAbove, false, 1);
  }
}

template <class Canvas>
void blit(Canvas& c, FrameId id, int x0, int y0, bool flip, int repeat) {
  const Frame& f = kFrames[id];
  const int w = f.w, h = f.h;
  auto px = [&](int x, int y) { return pixel(f, flip ? w - 1 - x : x, y); };
  // The run of `ix` at [x, x + n) in row y, and nothing more of it either side.
  auto sameRun = [&](int x, int n, int y, uint8_t ix) {
    if ((x > 0 && px(x - 1, y) == ix) || (x + n < w && px(x + n, y) == ix)) return false;
    for (int k = 0; k < n; ++k)
      if (px(x + k, y) != ix) return false;
    return true;
  };
  for (int j = 0; j < h; ++j) {
    int i = 0;
    while (i < w) {
      const uint8_t ix = px(i, j);
      int n = 1;
      while (i + n < w && px(i + n, j) == ix) ++n;
      // A run the row above repeats exactly was drawn with it, as one taller
      // rectangle; otherwise this one reaches down as far as it repeats.
      if (ix != 0 && !(j > 0 && sameRun(i, n, j - 1, ix))) {
        int rows = 1;
        while (j + rows < h && sameRun(i, n, j + rows, ix)) ++rows;
        const int height = h == 1 ? kScale * repeat : kScale * rows;  // a one-row tile stacks
        c.fillRect(x0 + i * kScale, y0 + j * kScale, n * kScale, height, static_cast<int>(ix));
      }
      i += n;
    }
  }
}

}  // namespace crab
