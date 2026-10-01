// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "CrabPose.h"

#include <algorithm>
#include <cmath>

#include "DancePose.h"  // dance::danceWeight: the same confidence -> weight as the stick figure

namespace crab {
namespace {
static_assert(kBodySquash == kBody + 1 && kBodyStretch == kBody + 2, "kBodyXY is indexed from kBody");

constexpr float kTwoPi = 6.2831853f;
// Idle loop timing (seconds).
constexpr float kBreathS = 2.6f;       // bob = 1.5 sin(2 pi t / 2.6)
constexpr float kBobPx = 1.5f;
constexpr float kGlanceS = 5.3f;       // eyes_look to one side, then the other
constexpr float kBlinkS = 3.7f, kBlinkLen = 0.16f;
constexpr float kTwitchLS = 2.9f, kTwitchRS = 3.3f, kTwitchROffset = 1.4f, kTwitchLen = 0.25f;
// The idle clock wraps here: one frame's hiccup in the idle loop every
// ~17 minutes, while float seconds stay precise to well under a frame.
constexpr float kIdleWrapS = 1000.0f;

float wrap(float x, float m) {
  x = std::fmod(x, m);
  return x < 0.0f ? x + m : x;
}

float smooth(float a, float b, float x) {
  float t = (x - a) / (b - a);
  t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
  return t * t * (3.0f - 2.0f * t);
}

int clamp2(int v) { return v < -2 ? -2 : (v > 2 ? 2 : v); }

// 0..1, 1 on the claw's own beat (v = 0 mod 2): an accelerating rise over the
// last kClawRise of the beat before, an eased fall over kClawFall after.
float clawPulse(float v) {
  v = wrap(v + 1.0f, 2.0f) - 1.0f;  // -1..1, its beat at 0
  if (v >= -kClawRise && v < 0.0f) {
    const float x = 1.0f + v / kClawRise;
    return x * x;
  }
  if (v >= 0.0f && v < kClawFall) {
    const float x = v / kClawFall;
    return 1.0f - x * x * (3.0f - 2.0f * x);
  }
  return 0.0f;
}

// The whole crab's screen offset at cycle position u (beats, wraps at 2).
void bodyMotion(float u, float& dx, float& dy) {
  u = wrap(u, 2.0f);
  const int parity = static_cast<int>(u);
  const float phi = u - static_cast<float>(parity);
  float a = (phi - kDwell) / (1.0f - kDwell);
  if (a < 0.0f) a = 0.0f;
  dy = -kHopPx * 4.0f * a * (1.0f - a);  // 0 on the ground, lands on the next beat
  const float s = smooth(kDwell, 0.92f, phi);
  const float from = parity == 0 ? -1.0f : 1.0f;  // even: left -> right, odd: right -> left
  dx = kShufflePx * (from - 2.0f * from * s);
}
}  // namespace

Pose dancePose(float phi, bool odd) {
  phi -= std::floor(phi);
  const int parity = odd ? 1 : 0;
  const float u = static_cast<float>(parity) + phi;
  Pose p;
  bodyMotion(u, p.dx, p.dy);
  p.body = phi < kSquashEnd ? kBodySquash : (phi >= kStretchStart ? kBodyStretch : kBody);
  // The eye stalks trail: where the body was kEyeLag ago, less where it is.
  float pdx, pdy;
  bodyMotion(u - kEyeLag, pdx, pdy);
  p.eyeX = clamp2(rnd((pdx - p.dx) / kScale));
  p.eyeY = clamp2(rnd((pdy - p.dy) / kScale));
  if (phi < kSquashEnd && odd) {
    p.eyes = kEyesHappy;  // ^ ^ on odd impacts: the face alternates each beat
  } else if (phi >= kLookStart && phi < kLookEnd) {
    p.eyes = kEyesLook;   // toward the travel: right on even beats, left on odd
    p.eyesFlip = odd;
  }
  p.liftL = kClawRest + rnd(kClawLift * clawPulse(u));
  p.liftR = kClawRest + rnd(kClawLift * clawPulse(u - 1.0f));
  p.snapL = !odd && phi < kSnapEnd;
  p.snapR = odd && phi < kSnapEnd;
  p.legs = phi < kSquashEnd ? kLegsSplay : ((phi >= kAirStart && phi < kAirEnd) ? kLegsAir : kLegs);
  p.fx = phi < kFxEnd ? (odd ? 1 : -1) : 0;
  return p;
}

Pose idlePose(float t) {
  Pose p;
  const float breath = std::sin(kTwoPi * t / kBreathS);
  p.bob = kBobPx * breath;
  const float look = std::sin(kTwoPi * t / kGlanceS);
  p.eyeX = look > 0.6f ? 1 : (look < -0.6f ? -1 : 0);
  p.eyeY = breath > 0.3f ? 1 : 0;  // the eyes settle a touch on the out-breath
  p.eyes = std::fmod(t, kBlinkS) < kBlinkLen ? kEyesHappy : (p.eyeX != 0 ? kEyesLook : kEyes);
  p.eyesFlip = p.eyeX < 0;
  p.snapL = std::fmod(t, kTwitchLS) < kTwitchLen;
  p.snapR = std::fmod(t + kTwitchROffset, kTwitchRS) < kTwitchLen;
  p.liftL = kIdleLift + (p.snapL ? 1 : 0);
  p.liftR = kIdleLift + (p.snapR ? 1 : 0);
  return p;
}

Pose blend(const Pose& i, const Pose& d, float w) {
  if (w >= 1.0f) return d;
  if (w <= 0.0f) return i;
  Pose p = w >= 0.5f ? d : i;
  auto L = [w](float a, float b) { return a + (b - a) * w; };
  p.dx = L(i.dx, d.dx);
  p.dy = L(i.dy, d.dy);
  p.bob = L(i.bob, d.bob);
  p.liftL = rnd(L(static_cast<float>(i.liftL), static_cast<float>(d.liftL)));
  p.liftR = rnd(L(static_cast<float>(i.liftR), static_cast<float>(d.liftR)));
  p.eyeX = rnd(L(static_cast<float>(i.eyeX), static_cast<float>(d.eyeX)));
  p.eyeY = rnd(L(static_cast<float>(i.eyeY), static_cast<float>(d.eyeY)));
  return p;
}

int paletteStep(float w) {
  if (!(w > 0.0f)) return 0;
  if (w >= 1.0f) return kPaletteSteps;
  return rnd(w * kPaletteSteps);
}

void paletteAt(int step, uint8_t out[kPaletteSize][3]) {
  step = std::max(0, std::min(kPaletteSteps, step));
  for (int i = 0; i < kPaletteSize; ++i) {
    for (int c = 0; c < 3; ++c) {
      const int a = kPaletteIdle[i][c], b = kPalette[i][c];
      out[i][c] = static_cast<uint8_t>(a + ((b - a) * step + (b >= a ? kPaletteSteps / 2 : -kPaletteSteps / 2)) /
                                               kPaletteSteps);
    }
  }
}

Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

namespace {
Rect clipToBox(Rect r) {
  r.x0 = std::max(r.x0, 0);
  r.y0 = std::max(r.y0, 0);
  r.x1 = std::min(r.x1, kBoxW);
  r.y1 = std::min(r.y1, kBoxH);
  return r;
}
}  // namespace

Rect bounds(const Pose& p) {
  Rect r{kBoxW, kBoxH, 0, 0};
  forEachLayer(p, [&](FrameId f, int x, int y, bool, int repeat) {
    const Frame& fr = kFrames[f];
    r.x0 = std::min(r.x0, x);
    r.y0 = std::min(r.y0, y);
    r.x1 = std::max(r.x1, x + fr.w * kScale);
    r.y1 = std::max(r.y1, y + fr.h * kScale * repeat);
  });
  return clipToBox(r);
}

Rect shadowRect(const Pose& p) {
  const int w = kShadowW - 2 * rnd(kShadowShrink * -p.dy / 2.0f);
  Rect r;
  r.x0 = kOriginX + rnd(p.dx) - w / 2;
  r.x1 = r.x0 + w;
  r.y0 = kShadowY0;
  r.y1 = kShadowY1;
  return r;
}

Rect drawnRect(const Pose& p) { return unite(bounds(p), clipToBox(shadowRect(p))); }

void Crab::reset(float weight) {
  weight_ = std::max(0.0f, std::min(1.0f, weight));
  idle_ = 0.0f;
}

Pose Crab::update(float phi, bool odd, float confidence, bool beat, float dt) {
  if (dt < 0.0f) dt = 0.0f;
  const float target = beat ? dance::danceWeight(confidence) : 0.0f;
  weight_ += (target - weight_) * (1.0f - std::exp(-dt / 0.4f));
  idle_ += dt;
  if (idle_ >= kIdleWrapS) idle_ -= kIdleWrapS;
  if (beat) {
    lastPhi_ = phi;
    lastOdd_ = odd;
  }
  // Only the half that shows is worked out.
  if (weight_ >= 1.0f) return dancePose(lastPhi_, lastOdd_);
  if (weight_ <= 0.0f) return idlePose(idle_);
  return blend(idlePose(idle_), dancePose(lastPhi_, lastOdd_), weight_);
}

}  // namespace crab
