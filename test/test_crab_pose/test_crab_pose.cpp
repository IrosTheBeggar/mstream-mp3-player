// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the dancing crab: the generated art (CrabArt), the mapping
// from beat phase to frames and offsets (CrabPose), the palette blend, and
// the skin cycle. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "CrabPose.h"
#include "DanceSkin.h"

using crab::Pose;

namespace {
// A canvas of palette indices, the box's size, recording each fillRect.
struct Canvas {
  uint8_t px[crab::kBoxH][crab::kBoxW];
  int rects = 0;
  Canvas() { clear(); }
  void clear() {
    std::memset(px, 0, sizeof(px));
    rects = 0;
  }
  void fillRect(int x, int y, int w, int h, int c) {
    ++rects;
    for (int j = y; j < y + h; ++j)
      for (int i = x; i < x + w; ++i)
        if (i >= 0 && i < crab::kBoxW && j >= 0 && j < crab::kBoxH) px[j][i] = static_cast<uint8_t>(c);
  }
};

// The crab's layers only (no ground, shadow or dot), as DanceView blits them.
void render(Canvas& c, const Pose& p) {
  c.clear();
  crab::forEachLayer(p, [&](crab::FrameId f, int x, int y, bool flip, int repeat) {
    crab::blit(c, f, x, y, flip, repeat);
  });
}

uint32_t fnv1a(const Canvas& c) {
  uint32_t h = 0x811C9DC5u;
  const auto* b = &c.px[0][0];
  for (size_t i = 0; i < sizeof(c.px); ++i) h = (h ^ b[i]) * 0x01000193u;
  return h;
}

uint16_t rgb565(const uint8_t c[3]) {
  return static_cast<uint16_t>(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
}

// The two-beat cycle in n steps: (phi, odd) for step k.
float phiOf(int k, int n) { return static_cast<float>(k % n) / n; }
bool oddOf(int k, int n) { return (k / n) % 2 == 1; }
}  // namespace

void setUp() {}
void tearDown() {}

// The generated tables hold what crab.json says: sizes, index ranges, the
// view's own palette slots unused by the art, and the palette slots the stick
// figure's screen shares (ground line, beat dot).
void test_art_tables() {
  const uint8_t sizes[crab::kFrameCount][2] = {{24, 15}, {26, 13}, {22, 16}, {16, 14}, {16, 14}, {16, 14}, {12, 11},
                                               {12, 11}, {4, 1},   {34, 7},  {34, 7},  {34, 7},  {11, 5}};
  int bytes = 0;
  for (int f = 0; f < crab::kFrameCount; ++f) {
    const crab::Frame& fr = crab::kFrames[f];
    TEST_ASSERT_EQUAL_UINT8(sizes[f][0], fr.w);
    TEST_ASSERT_EQUAL_UINT8(sizes[f][1], fr.h);
    bytes += (fr.w + 1) / 2 * fr.h;
    int opaque = 0;
    for (int y = 0; y < fr.h; ++y) {
      for (int x = 0; x < fr.w; ++x) {
        const uint8_t ix = crab::pixel(fr, x, y);
        TEST_ASSERT_TRUE(ix < crab::kShadowIx);  // never the shadow, ground or dot slots
        opaque += ix != 0;
      }
    }
    TEST_ASSERT_TRUE(opaque > 0);
  }
  TEST_ASSERT_EQUAL_INT(1382, bytes);
  TEST_ASSERT_EQUAL_HEX16(0x4208, rgb565(crab::kPalette[crab::kGroundIx]));   // the stick figure's ground grey
  TEST_ASSERT_EQUAL_HEX16(0x07FF, rgb565(crab::kPalette[crab::kBeatDotIx]));  // and its cyan beat dot
  for (int i = 0; i < crab::kPaletteSize; ++i) {
    const bool viewSlot = i == 0 || i >= crab::kShadowIx;
    TEST_ASSERT_EQUAL_INT(viewSlot, std::memcmp(crab::kPalette[i], crab::kPaletteIdle[i], 3) == 0);
  }
  // The arm tile is outline-red-shade-outline, not the eye stalks' orange.
  const crab::Frame& arm = crab::kFrames[crab::kArm];
  TEST_ASSERT_EQUAL_UINT8(1, crab::pixel(arm, 0, 0));
  TEST_ASSERT_EQUAL_UINT8(3, crab::pixel(arm, 1, 0));
  TEST_ASSERT_EQUAL_UINT8(2, crab::pixel(arm, 2, 0));
  TEST_ASSERT_EQUAL_UINT8(1, crab::pixel(arm, 3, 0));
}

// k0..k15 (phase n/8 of the two-beat cycle) as the design rig (Python, in
// crab.json's animation.phases_k0_k15) worked them out.
void test_phases_match_the_design() {
  struct K {
    float dx, dy;
    crab::FrameId body, legs, eyes;
    bool flip;
    int eyeX, eyeY, liftL, liftR;
    bool snapL, snapR;
    int fx;
  };
  using namespace crab;
  const K k[16] = {
      {-6.0000f, -0.0000f, kBodySquash, kLegsSplay, kEyes, false, 0, -1, 9, 1, true, false, -1},
      {-5.9986f, -0.2034f, kBody, kLegs, kEyes, false, 0, 0, 8, 1, true, false, 0},
      {-5.1524f, -4.5325f, kBody, kLegs, kEyesLook, false, 0, 1, 7, 1, false, false, 0},
      {-3.1196f, -7.4090f, kBody, kLegsAir, kEyesLook, false, -1, 1, 5, 1, false, false, 0},
      {-0.4496f, -8.8326f, kBody, kLegsAir, kEyesLook, false, -1, 0, 3, 1, false, false, 0},
      {2.3082f, -8.8036f, kBody, kLegsAir, kEyesLook, false, -1, 0, 1, 1, false, false, 0},
      {4.6047f, -7.3218f, kBody, kLegs, kEyesLook, false, -1, 0, 1, 1, false, false, 0},
      {5.8904f, -4.3873f, kBodyStretch, kLegs, kEyes, false, 0, -1, 1, 2, false, false, 0},
      {6.0000f, -0.0000f, kBodySquash, kLegsSplay, kEyesHappy, false, 0, -1, 1, 9, false, true, 1},
      {5.9986f, -0.2034f, kBody, kLegs, kEyes, false, 0, 0, 1, 8, false, true, 0},
      {5.1524f, -4.5325f, kBody, kLegs, kEyesLook, true, 0, 1, 1, 7, false, false, 0},
      {3.1196f, -7.4090f, kBody, kLegsAir, kEyesLook, true, 1, 1, 1, 5, false, false, 0},
      {0.4496f, -8.8326f, kBody, kLegsAir, kEyesLook, true, 1, 0, 1, 3, false, false, 0},
      {-2.3082f, -8.8036f, kBody, kLegsAir, kEyesLook, true, 1, 0, 1, 1, false, false, 0},
      {-4.6047f, -7.3218f, kBody, kLegs, kEyesLook, true, 1, 0, 1, 1, false, false, 0},
      {-5.8904f, -4.3873f, kBodyStretch, kLegs, kEyes, false, 0, -1, 2, 1, false, false, 0},
  };
  for (int n = 0; n < 16; ++n) {
    const Pose p = dancePose((n % 8) / 8.0f, n >= 8);
    const K& e = k[n];
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, e.dx, p.dx);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, e.dy, p.dy);
    TEST_ASSERT_EQUAL_INT(e.body, p.body);
    TEST_ASSERT_EQUAL_INT(e.legs, p.legs);
    TEST_ASSERT_EQUAL_INT(e.eyes, p.eyes);
    TEST_ASSERT_EQUAL(e.flip, p.eyesFlip);
    TEST_ASSERT_EQUAL_INT(e.eyeX, p.eyeX);
    TEST_ASSERT_EQUAL_INT(e.eyeY, p.eyeY);
    TEST_ASSERT_EQUAL_INT(e.liftL, p.liftL);
    TEST_ASSERT_EQUAL_INT(e.liftR, p.liftR);
    TEST_ASSERT_EQUAL(e.snapL, p.snapL);
    TEST_ASSERT_EQUAL(e.snapR, p.snapR);
    TEST_ASSERT_EQUAL_INT(e.fx, p.fx);
  }
}

// Pixel for pixel what the design rig drew (FNV-1a of the box's indices):
// the dance at k0..k15, the idle loop every 0.25 s, and blends.
void test_pixels_match_the_design() {
  const uint32_t dance[16] = {0xAB4960CEu, 0x10B06FF6u, 0x1A0AD775u, 0x853E1AFBu, 0xFA3BF99Du, 0x3E86263Au,
                              0x250C88E8u, 0x55DD9C16u, 0x836147D3u, 0x9ABEFC2Eu, 0xBB18CDC5u, 0x0909C7DBu,
                              0xB0BB2EC9u, 0x34BFDF10u, 0x66842902u, 0xD36FB8B0u};
  const uint32_t idle[16] = {0x0F9550A8u, 0x91A07D7Eu, 0x91A07D7Eu, 0x012549E8u, 0x012549E8u, 0x1EAEEB78u,
                             0x4556E448u, 0x4556E448u, 0x08A7602Bu, 0x48FBEABCu, 0xBCAAAF6Cu, 0x91A07D7Eu,
                             0xA8CBAB5Eu, 0x01A1EE06u, 0x074C7F8Cu, 0xD65C89AFu};
  const uint32_t blended[16] = {0x0F9550A8u, 0xF0FF4CB7u, 0x313446A1u, 0x8EDFD4B2u, 0x70A15961u, 0x0372EF5Fu,
                                0x90F293A9u, 0xAB18D207u, 0x0546EAA7u, 0x615399F5u, 0x62C44A7Cu, 0x2592424Eu,
                                0x64E16A22u, 0x6B81BF80u, 0x75D87FBCu, 0xD36FB8B0u};
  static Canvas c;
  for (int n = 0; n < 16; ++n) {
    render(c, crab::dancePose((n % 8) / 8.0f, n >= 8));
    TEST_ASSERT_EQUAL_HEX32(dance[n], fnv1a(c));
    render(c, crab::idlePose(n * 0.25f));
    TEST_ASSERT_EQUAL_HEX32(idle[n], fnv1a(c));
    const float t = n * 0.3f;
    render(c, crab::blend(crab::idlePose(t), crab::dancePose((n % 8) / 8.0f, (n / 8) % 2 == 1), n / 15.0f));
    TEST_ASSERT_EQUAL_HEX32(blended[n], fnv1a(c));
  }
}

// Contact and the extremes on the beat: on the ground and squashed at phi 0,
// at a side extreme (alternating), the beat's claw at its top and shut, the
// spark over it; the other claw down.
void test_contact_and_extremes_on_the_beat() {
  for (int odd = 0; odd < 2; ++odd) {
    const Pose p = crab::dancePose(0.0f, odd == 1);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.dy);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, odd ? crab::kShufflePx : -crab::kShufflePx, p.dx);
    TEST_ASSERT_EQUAL_INT(crab::kBodySquash, p.body);
    TEST_ASSERT_EQUAL_INT(crab::kLegsSplay, p.legs);
    const int beatLift = odd ? p.liftR : p.liftL, otherLift = odd ? p.liftL : p.liftR;
    TEST_ASSERT_EQUAL_INT(crab::kClawRest + crab::kClawLift, beatLift);
    TEST_ASSERT_EQUAL_INT(crab::kClawRest, otherLift);
    TEST_ASSERT_TRUE(odd ? p.snapR && !p.snapL : p.snapL && !p.snapR);
    TEST_ASSERT_EQUAL_INT(odd ? 1 : -1, p.fx);
    // No lift is ever higher than the one on the beat.
    for (int k = 0; k < 400; ++k) {
      const Pose q = crab::dancePose(phiOf(k, 200), oddOf(k, 200));
      TEST_ASSERT_TRUE(std::max(q.liftL, q.liftR) <= beatLift);
    }
  }
  // The squash, the snap and the spark are short: gone by 0.16 beat.
  const Pose later = crab::dancePose(0.17f, false);
  TEST_ASSERT_EQUAL_INT(crab::kBody, later.body);
  TEST_ASSERT_FALSE(later.snapL || later.snapR);
  TEST_ASSERT_EQUAL_INT(0, later.fx);
}

// The hop: on the ground for the dwell, a single arc peaking mid-beat, and
// back on the ground exactly on the next beat.
void test_hop_arc() {
  float top = 0.0f, at = -1.0f;
  for (int i = 0; i < 1000; ++i) {
    const float phi = i / 1000.0f;
    const Pose p = crab::dancePose(phi, false);
    if (phi < crab::kDwell) TEST_ASSERT_EQUAL_FLOAT(0.0f, p.dy);
    if (phi > crab::kDwell + 0.01f) TEST_ASSERT_TRUE(p.dy < 0.0f);
    TEST_ASSERT_TRUE(p.dy >= -crab::kHopPx - 1e-4f);
    if (p.dy < top) {
      top = p.dy;
      at = phi;
    }
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -crab::kHopPx, top);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.56f, at);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.0f, crab::dancePose(0.999f, false).dy);
}

// Anticipation before the beat: the stretched body, and the next beat's claw
// already rising (accelerating) into it.
void test_anticipation() {
  for (int odd = 0; odd < 2; ++odd) {
    const Pose p = crab::dancePose(0.9f, odd == 1);
    TEST_ASSERT_EQUAL_INT(crab::kBodyStretch, p.body);
    TEST_ASSERT_EQUAL_INT(crab::kBody, crab::dancePose(0.8f, odd == 1).body);
    // The next beat is the other parity's: its claw rises over the last 0.22.
    int last = -100;
    for (int i = 0; i <= 20; ++i) {
      const float phi = 0.78f + 0.2199f * i / 20.0f;
      const Pose q = crab::dancePose(phi, odd == 1);
      const int next = odd ? q.liftL : q.liftR;
      TEST_ASSERT_TRUE(next >= last);
      last = next;
    }
    TEST_ASSERT_TRUE(last >= crab::kClawRest + crab::kClawLift - 1);
  }
}

// Continuous across the beats: the end of one beat meets the start of the
// next (no jump in the offsets, at most a pixel in a claw).
void test_continuous_across_beats() {
  for (int odd = 0; odd < 2; ++odd) {
    const Pose end = crab::dancePose(0.9999f, odd == 1);
    const Pose start = crab::dancePose(0.0f, odd == 0);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, start.dx, end.dx);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, start.dy, end.dy);
    TEST_ASSERT_TRUE(std::abs(start.liftL - end.liftL) <= 1);
    TEST_ASSERT_TRUE(std::abs(start.liftR - end.liftR) <= 1);
  }
  // And frame to frame at 30 fps and 160 BPM (the fastest dance tempo):
  // offsets move a few px at most.
  const float step = 160.0f / 60.0f / 30.0f;
  Pose last = crab::dancePose(0.0f, false);
  for (int i = 1; i < 200; ++i) {
    const float u = std::fmod(i * step, 2.0f);
    const Pose p = crab::dancePose(u - std::floor(u), u >= 1.0f);
    TEST_ASSERT_TRUE(std::fabs(p.dx - last.dx) < 3.5f);
    TEST_ASSERT_TRUE(std::fabs(p.dy - last.dy) < 4.5f);
    last = p;
  }
}

// The eyes: they trail the body (the secondary element), look where the crab
// travels mid-air (right on even beats, left on odd), and squint on odd impacts.
void test_eyes_trail_and_look() {
  for (int odd = 0; odd < 2; ++odd) {
    const float travel = odd ? -1.0f : 1.0f;  // screen x direction of this beat's step
    for (int i = 0; i < 100; ++i) {
      const float phi = i / 100.0f;
      const Pose p = crab::dancePose(phi, odd == 1);
      TEST_ASSERT_TRUE(std::abs(p.eyeX) <= 2 && std::abs(p.eyeY) <= 2);
      TEST_ASSERT_TRUE(p.eyeX * travel <= 0.0f);  // never ahead of the body
      if (phi >= crab::kLookStart && phi < crab::kLookEnd) {
        TEST_ASSERT_EQUAL_INT(crab::kEyesLook, p.eyes);
        TEST_ASSERT_EQUAL(odd == 1, p.eyesFlip);  // flipped = looking left
      }
    }
  }
  TEST_ASSERT_EQUAL_INT(crab::kEyesHappy, crab::dancePose(0.05f, true).eyes);
  TEST_ASSERT_EQUAL_INT(crab::kEyes, crab::dancePose(0.05f, false).eyes);
}

// Every pose stays inside the 120 x 150 box (so the dirty rectangle never
// clips the crab), above the ground line; the shadow tracks the hop.
void test_bounds_and_shadow() {
  static Canvas c;
  auto check = [&](const Pose& p) {
    const crab::Rect r = crab::bounds(p);
    TEST_ASSERT_TRUE(r.x0 >= 0 && r.x1 <= crab::kBoxW && r.y0 >= 20 && r.y1 <= crab::kOriginY);
    // bounds() holds every pixel drawn.
    render(c, p);
    for (int y = 0; y < crab::kBoxH; ++y)
      for (int x = 0; x < crab::kBoxW; ++x)
        if (c.px[y][x]) TEST_ASSERT_TRUE(x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1);
    // The frame's dirty rectangle holds the shadow too.
    const crab::Rect s = crab::shadowRect(p), d = crab::drawnRect(p);
    TEST_ASSERT_TRUE(s.x0 >= 0 && s.x1 <= crab::kBoxW);
    TEST_ASSERT_TRUE(d.x0 <= s.x0 && d.x1 >= s.x1 && d.y0 <= r.y0 && d.y1 == crab::kShadowY1);
  };
  for (int k = 0; k < 240; ++k) check(crab::dancePose(phiOf(k, 120), oddOf(k, 120)));
  for (int k = 0; k < 160; ++k) check(crab::idlePose(k * 0.05f));
  const crab::Rect ground = crab::shadowRect(crab::dancePose(0.0f, false));
  TEST_ASSERT_EQUAL_INT(crab::kShadowW, ground.x1 - ground.x0);
  TEST_ASSERT_EQUAL_INT(crab::kOriginX - 6 - crab::kShadowW / 2, ground.x0);  // under the crab at dx -6
  const crab::Rect top = crab::shadowRect(crab::dancePose(0.56f, false));
  TEST_ASSERT_EQUAL_INT(crab::kShadowW - 18, top.x1 - top.x0);  // 2 px narrower per px of the 9 px hop
}

// Idle: the feet planted, claws tucked (twitching by a pixel), breathing,
// glances both ways and blinks, all within their cycles.
void test_idle_loop() {
  bool blink = false, lookL = false, lookR = false, twitchL = false, twitchR = false;
  float bobLo = 0.0f, bobHi = 0.0f;
  for (int i = 0; i < 600; ++i) {  // 12 s at 50 Hz
    const Pose p = crab::idlePose(i * 0.02f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.dx);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, p.dy);
    TEST_ASSERT_EQUAL_INT(crab::kLegs, p.legs);
    TEST_ASSERT_EQUAL_INT(crab::kBody, p.body);
    TEST_ASSERT_EQUAL_INT(0, p.fx);
    TEST_ASSERT_TRUE(p.liftL <= crab::kIdleLift + 1 && p.liftR <= crab::kIdleLift + 1);
    TEST_ASSERT_EQUAL(p.liftL == crab::kIdleLift + 1, p.snapL);
    TEST_ASSERT_EQUAL(p.liftR == crab::kIdleLift + 1, p.snapR);
    blink |= p.eyes == crab::kEyesHappy;
    lookL |= p.eyes == crab::kEyesLook && p.eyesFlip && p.eyeX < 0;
    lookR |= p.eyes == crab::kEyesLook && !p.eyesFlip && p.eyeX > 0;
    twitchL |= p.snapL;
    twitchR |= p.snapR;
    bobLo = std::min(bobLo, p.bob);
    bobHi = std::max(bobHi, p.bob);
  }
  TEST_ASSERT_TRUE(blink && lookL && lookR && twitchL && twitchR);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -1.5f, bobLo);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.5f, bobHi);
  // The legs don't bob: only the rest of the crab breathes.
  Pose up = crab::idlePose(2.6f / 4.0f);  // bob +1.5 -> rounds to 2 px
  int legY = -1, bodyY = -1, legY0 = -1, bodyY0 = -1;
  crab::forEachLayer(up, [&](crab::FrameId f, int, int y, bool, int) {
    if (f == crab::kLegs) legY = y;
    if (f == crab::kBody) bodyY = y;
  });
  crab::forEachLayer(crab::idlePose(0.0f), [&](crab::FrameId f, int, int y, bool, int) {
    if (f == crab::kLegs) legY0 = y;
    if (f == crab::kBody) bodyY0 = y;
  });
  TEST_ASSERT_EQUAL_INT(legY0, legY);
  TEST_ASSERT_EQUAL_INT(bodyY0 + 2, bodyY);
}

// Blend: the ends are the poses themselves, offsets lerp, frames switch at
// w = 0.5.
void test_blend() {
  const Pose i = crab::idlePose(1.0f), d = crab::dancePose(0.5f, false);
  const Pose b0 = crab::blend(i, d, 0.0f), b1 = crab::blend(i, d, 1.0f);
  TEST_ASSERT_EQUAL_FLOAT(i.bob, b0.bob);
  TEST_ASSERT_EQUAL_INT(i.liftL, b0.liftL);
  TEST_ASSERT_EQUAL_FLOAT(d.dy, b1.dy);
  TEST_ASSERT_EQUAL_INT(d.legs, b1.legs);
  const Pose lo = crab::blend(i, d, 0.49f), hi = crab::blend(i, d, 0.51f);
  TEST_ASSERT_EQUAL_INT(i.legs, lo.legs);
  TEST_ASSERT_EQUAL_INT(d.legs, hi.legs);
  TEST_ASSERT_EQUAL_INT(d.eyes, hi.eyes);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.25f * d.dy, crab::blend(i, d, 0.25f).dy);
  float last = 1.0f;
  for (int k = 0; k <= 20; ++k) {
    const float dy = crab::blend(i, d, k / 20.0f).dy;
    TEST_ASSERT_TRUE(dy <= last);  // d.dy < 0: monotonic towards the dance
    last = dy;
  }
}

// The stateful side: confidence at 0.12 or less stays idle, a clear beat fades in
// over ~0.4 s, a lost beat fades out holding its last pose, reset() hands a
// weight over.
void test_crab_state() {
  crab::Crab c;
  for (int i = 0; i < 90; ++i) c.update(0.3f, false, 0.12f, true, 1.0f / 30.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.weight());
  const Pose idle = c.update(0.3f, false, 0.12f, true, 1.0f / 30.0f);
  TEST_ASSERT_EQUAL_INT(crab::kLegs, idle.legs);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, idle.dy);
  // A clear beat: 1 - e^-1 of the way after 0.4 s, nearly all after 2 s.
  for (int i = 0; i < 12; ++i) c.update(0.5f, false, 0.9f, true, 1.0f / 30.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, 0.632f, c.weight());
  Pose p;
  for (int i = 0; i < 48; ++i) p = c.update(0.5f, false, 0.9f, true, 1.0f / 30.0f);
  TEST_ASSERT_TRUE(c.weight() > 0.99f);
  const Pose ref = crab::dancePose(0.5f, false);
  TEST_ASSERT_EQUAL_INT(ref.legs, p.legs);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, ref.dy, p.dy);
  // The beat is lost: the dance half holds phi 0.5 while it fades.
  p = c.update(0.9f, true, 0.9f, false, 1.0f / 30.0f);
  TEST_ASSERT_TRUE(c.weight() < 0.99f && c.weight() > 0.9f);
  TEST_ASSERT_EQUAL_INT(ref.legs, p.legs);
  for (int i = 0; i < 90; ++i) p = c.update(0.0f, false, 0.9f, false, 1.0f / 30.0f);
  TEST_ASSERT_TRUE(c.weight() < 0.01f);
  TEST_ASSERT_EQUAL_INT(crab::kIdleLift, std::min(p.liftL, p.liftR));
  // Frozen poses are the pure dance.
  const Pose f = crab::Crab::frozen(0.0f, true);
  TEST_ASSERT_EQUAL_INT(crab::kBodySquash, f.body);
  TEST_ASSERT_EQUAL_INT(1, f.fx);
  c.reset(0.7f);
  TEST_ASSERT_EQUAL_FLOAT(0.7f, c.weight());
  c.reset(3.0f);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, c.weight());
  c.reset();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, c.weight());
  // A long idle stays finite (the clock wraps).
  for (int i = 0; i < 40000; ++i) p = c.update(0.0f, false, 0.0f, false, 1.0f / 30.0f);
  TEST_ASSERT_TRUE(std::fabs(p.bob) <= 1.5f);
}

// The palette fades per channel from the idle colours to the dance ones in
// kPaletteSteps steps; the view's own slots never change.
void test_palette_blend() {
  uint8_t pal[crab::kPaletteSize][3];
  crab::paletteAt(0, pal);
  TEST_ASSERT_EQUAL_MEMORY(crab::kPaletteIdle, pal, sizeof(pal));
  crab::paletteAt(crab::kPaletteSteps, pal);
  TEST_ASSERT_EQUAL_MEMORY(crab::kPalette, pal, sizeof(pal));
  crab::paletteAt(crab::kPaletteSteps + 5, pal);  // clamped
  TEST_ASSERT_EQUAL_MEMORY(crab::kPalette, pal, sizeof(pal));
  crab::paletteAt(crab::kPaletteSteps / 2, pal);
  for (int i = 0; i < crab::kPaletteSize; ++i) {
    for (int ch = 0; ch < 3; ++ch) {
      const int mid = (crab::kPaletteIdle[i][ch] + crab::kPalette[i][ch]) / 2;
      TEST_ASSERT_INT_WITHIN(1, mid, pal[i][ch]);
    }
  }
  TEST_ASSERT_EQUAL_INT(0, crab::paletteStep(-1.0f));
  TEST_ASSERT_EQUAL_INT(0, crab::paletteStep(0.0f));
  TEST_ASSERT_EQUAL_INT(crab::kPaletteSteps / 2, crab::paletteStep(0.5f));
  TEST_ASSERT_EQUAL_INT(crab::kPaletteSteps, crab::paletteStep(1.0f));
  TEST_ASSERT_EQUAL_INT(crab::kPaletteSteps, crab::paletteStep(0.999f));
}

// The blitter: integer 3x blocks, runs merged (across rows too), index 0 left
// alone, the mirror exact, a one-row tile stacked.
void test_blit() {
  static Canvas c;
  c.clear();
  std::memset(c.px, 9, sizeof(c.px));  // a background the transparent pixels must keep
  crab::blit(c, crab::kClawOpen, 30, 40, false);
  const crab::Frame& f = crab::kFrames[crab::kClawOpen];
  int runs = 0;
  for (int y = 0; y < f.h; ++y) {
    for (int x = 0; x < f.w; ++x) {
      const uint8_t ix = crab::pixel(f, x, y);
      if (ix != 0 && (x == 0 || crab::pixel(f, x - 1, y) != ix)) ++runs;
      for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) TEST_ASSERT_EQUAL_UINT8(ix ? ix : 9, c.px[40 + 3 * y + j][30 + 3 * x + i]);
    }
  }
  TEST_ASSERT_TRUE(c.rects > 0 && c.rects < runs);  // repeated runs share a rectangle
  static Canvas m;
  m.clear();
  crab::blit(m, crab::kClawOpen, 30, 40, true);
  for (int y = 0; y < f.h * 3; ++y)
    for (int x = 0; x < f.w * 3; ++x)
      TEST_ASSERT_EQUAL_UINT8(c.px[40 + y][30 + x] == 9 ? 0 : c.px[40 + y][30 + x], m.px[40 + y][30 + f.w * 3 - 1 - x]);
  c.clear();
  crab::blit(c, crab::kArm, 10, 10, false, 5);
  TEST_ASSERT_EQUAL_INT(4, c.rects);  // one rectangle per column of the tile
  for (int y = 10; y < 25; ++y) TEST_ASSERT_EQUAL_UINT8(3, c.px[y][13]);
  TEST_ASSERT_EQUAL_UINT8(0, c.px[25][13]);
  // An eye stalk ('kok' for 6 rows) is 3 rectangles, not 18.
  c.clear();
  crab::blit(c, crab::kEyes, 0, 0, false);
  const int eyes = c.rects;
  int eyeRuns = 0;
  const crab::Frame& e = crab::kFrames[crab::kEyes];
  for (int y = 0; y < e.h; ++y)
    for (int x = 0; x < e.w; ++x)
      if (crab::pixel(e, x, y) != 0 && (x == 0 || crab::pixel(e, x - 1, y) != crab::pixel(e, x, y))) ++eyeRuns;
  TEST_ASSERT_TRUE(eyeRuns - eyes >= 2 * 3 * 5);
  // A whole frame of the dance: ~210 rectangles.
  int most = 0;
  for (int k = 0; k < 16; ++k) {
    render(c, crab::dancePose(phiOf(k, 8), oddOf(k, 8)));
    most = std::max(most, c.rects);
  }
  TEST_ASSERT_TRUE(most < 240);
}

void test_skins() {
  TEST_ASSERT_TRUE(dance::kDefaultSkin == dance::Skin::Crab);
  TEST_ASSERT_TRUE(dance::nextSkin(dance::Skin::Crab) == dance::Skin::Stick);
  TEST_ASSERT_TRUE(dance::nextSkin(dance::Skin::Stick) == dance::Skin::Crab);
  TEST_ASSERT_EQUAL_STRING("crab", dance::skinName(dance::Skin::Crab));
  TEST_ASSERT_EQUAL_STRING("stick", dance::skinName(dance::Skin::Stick));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_art_tables);
  RUN_TEST(test_phases_match_the_design);
  RUN_TEST(test_pixels_match_the_design);
  RUN_TEST(test_contact_and_extremes_on_the_beat);
  RUN_TEST(test_hop_arc);
  RUN_TEST(test_anticipation);
  RUN_TEST(test_continuous_across_beats);
  RUN_TEST(test_eyes_trail_and_look);
  RUN_TEST(test_bounds_and_shadow);
  RUN_TEST(test_idle_loop);
  RUN_TEST(test_blend);
  RUN_TEST(test_crab_state);
  RUN_TEST(test_palette_blend);
  RUN_TEST(test_blit);
  RUN_TEST(test_skins);
  return UNITY_END();
}
