// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the two code characters' cue -> pose mapping
// (CodeCharacters: codePhase, DanceHalf, CrabCharacter::pose,
// ClassicStick::pose) and for DanceMode's frame as lib/core holds it
// (DanceFrame: beatInput, sceneOf): docs/DANCE-SPRITES.md, sections 1.3 and
// 1.4 and Appendix F (F.1's CodeCharacters.h and DanceFrame.h, F.3, F.4's
// test_code_characters list, item by item). Written from the doc alone,
// without reading the implementation.
//
// What it pins:
//   1. codePhase: phi = frac(u), odd = floor(u) & 1, bit for bit;
//   2. hand-built cues: the k-poses equal 0.7.0's frozen poses (Crab::frozen,
//      Dancer::frozen), dancing at w >= 1 is dancePose itself, idle at
//      w <= 0 is idlePose itself, the fade is the blend; the functions read
//      only u, the weight and the idle seconds;
//   3. DanceHalf: the cue's u while it names a beat move, else the last one;
//   4. the real Choreographer under BeatScript's Script: k0-k15 while quiet
//      and while locked, the hold through a restart, Leaving, and bounds over
//      a long script at 24 and 30 fps;
//   5. the harness: DanceMode's frame on the host. beatInput's fold rule,
//      sceneOf, today's weight (a reference crab::Crab and dance::Dancer
//      driven as 0.7.0's render() drove them, against the Choreographer,
//      equal with == on every frame), today's idle poses, and a switch from
//      the crab to the stick at the next whole dance beat.
//
// Every Script frame in this suite goes through beatInput(facts(in), fold)
// before it reaches a Choreographer, and the result is checked equal to the
// Script's own BeatInput, field for field (F.4 item 5).
//
// Run: pio test -e native -f test_code_characters (and native_asan, F.4).
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../support/dance/BeatScript.h"
#include "CodeCharacters.h"
#include "CrabPose.h"
#include "DanceCast.h"
#include "DanceCharacter.h"
#include "DanceChoreo.h"
#include "DanceFrame.h"
#include "DancePose.h"
#include "DanceRate.h"

using beatscript::Script;
using dance::BeatInput;
using dance::ClassicStick;
using dance::CrabCharacter;
using dance::Cue;
using dance::DanceState;
using dance::FrameFacts;
using dance::kAutoBounce;
using dance::kAutoIdle;
using dance::kNoMove;

namespace {

// --- messages ------------------------------------------------------------

char g_ctx[256] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

const char* msg(const char* fmt, ...) {
  static char buf[640];
  int n = std::snprintf(buf, sizeof buf, "%s: ", g_ctx);
  if (n < 0) n = 0;
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + n, sizeof buf - static_cast<size_t>(n), fmt, ap);
  va_end(ap);
  return buf;
}

// The message is formatted only on failure: these run on every frame.
#define CHECK(cond, ...)                              \
  do {                                                \
    if (!(cond)) TEST_FAIL_MESSAGE(msg(__VA_ARGS__)); \
  } while (0)

// --- pose comparisons, field for field, floats with == ---------------------

// The first field where two crab poses differ, or null when they are equal.
const char* crabDiff(const crab::Pose& a, const crab::Pose& b) {
#define F_(x) \
  if (!(a.x == b.x)) return #x;
  F_(dx) F_(dy) F_(bob) F_(body) F_(eyes) F_(legs) F_(eyesFlip) F_(eyeX) F_(eyeY) F_(liftL) F_(liftR) F_(snapL)
  F_(snapR) F_(fx)
#undef F_
  return nullptr;
}

// The first joint where two stick poses differ (headR too), or null.
const char* stickDiff(const dance::Pose& a, const dance::Pose& b) {
#define P_(j) \
  if (!(a.j.x == b.j.x && a.j.y == b.j.y)) return #j;
  P_(head) P_(neck) P_(hip) P_(shoulderL) P_(elbowL) P_(handL) P_(shoulderR) P_(elbowR) P_(handR) P_(kneeL)
  P_(footL) P_(kneeR) P_(footR)
#undef P_
  if (!(a.headR == b.headR)) return "headR";
  return nullptr;
}

void poseFail(const char* who, const char* field, const char* m) {
  static char buf[900];
  std::snprintf(buf, sizeof buf, "%s pose: %s differs; %s", who, field, m);
  TEST_FAIL_MESSAGE(buf);
}
#define CHECK_CRAB(want, got, ...)                  \
  do {                                              \
    const char* d_ = crabDiff((want), (got));       \
    if (d_) poseFail("crab", d_, msg(__VA_ARGS__)); \
  } while (0)
#define CHECK_STICK(want, got, ...)                  \
  do {                                               \
    const char* d_ = stickDiff((want), (got));       \
    if (d_) poseFail("stick", d_, msg(__VA_ARGS__)); \
  } while (0)

bool finite(float v) { return std::isfinite(v); }
bool crabFinite(const crab::Pose& p) { return finite(p.dx) && finite(p.dy) && finite(p.bob); }
bool stickFinite(const dance::Pose& p) {
  const dance::Point pts[] = {p.head,   p.neck,      p.hip,    p.shoulderL, p.elbowL, p.handL, p.shoulderR,
                              p.elbowR, p.handR,     p.kneeL,  p.footL,     p.kneeR,  p.footR};
  for (const dance::Point& q : pts)
    if (!finite(q.x) || !finite(q.y)) return false;
  return finite(p.headR);
}

// --- hand-built cues ---------------------------------------------------------

constexpr int kVariants = 3;

// A cue with every field set, the weight and idle seconds as given. Variant 0
// looks like a dancing frame's; 1 and 2 put other values in every field the
// pose functions must ignore (the cue's own u included: they read the u
// they are handed, DanceHalf's).
Cue cueWith(double u, float w, float idleSeconds, int variant) {
  Cue c{};
  switch (variant) {
    case 0:
      c.beatMove = 0;
      c.u = u;
      c.beatPos16 = static_cast<uint32_t>(std::floor(u * 786432.0));
      c.k = 0;
      c.beatMirror = false;
      c.idleMove = 1;
      c.idleMs = 0;
      c.idleMirror = false;
      c.energy = 65536;
      c.showDance = true;
      c.flash = false;
      c.spot = 0;
      c.rootX = 160;
      c.rootY = 153;
      c.state = DanceState::Dancing;
      break;
    case 1:
      c.beatMove = kNoMove;
      c.u = u + 2.75;
      c.beatPos16 = 123456789u;
      c.k = 1;
      c.beatMirror = true;
      c.idleMove = kAutoIdle;
      c.idleMs = 98765u;
      c.idleMirror = true;
      c.energy = 39322;
      c.showDance = false;
      c.flash = true;
      c.spot = -1;
      c.rootX = 64;
      c.rootY = 7;
      c.state = DanceState::Leaving;
      break;
    default:
      c.beatMove = kAutoBounce;
      c.u = 0.0;
      c.beatPos16 = 0xFFFFFFFFu;
      c.k = -1;
      c.beatMirror = true;
      c.idleMove = kNoMove;
      c.idleMs = 0xFFFFFFFFu;
      c.idleMirror = false;
      c.energy = 47186;
      c.showDance = true;
      c.flash = true;
      c.spot = 1;
      c.rootX = 256;
      c.rootY = -3;
      c.state = DanceState::Freeze;
      break;
  }
  c.weight = w;
  c.idleSeconds = idleSeconds;
  return c;
}

// --- the frame harness: Script -> FrameFacts -> beatInput ---------------------

// F.4 item 5's facts(in): every field of a Script frame but foldScale.
FrameFacts facts(const BeatInput& in) {
  FrameFacts f{};
  f.heard = in.heard;
  f.grid = in.grid;
  f.trackerBeats = in.trackerBeats;
  f.locked = in.locked;
  f.confidence = in.confidence;
  f.rawBpm = in.rawBpm;
  f.restarts = in.restarts;
  f.acquisitions = in.acquisitions;
  f.dt = in.dt;
  f.freeze = in.freeze;
  return f;
}

// The first BeatInput field where a and b differ (doubles and floats with
// ==), or null.
const char* inputDiff(const BeatInput& a, const BeatInput& b) {
#define I_(x) \
  if (!(a.x == b.x)) return #x;
  I_(heard) I_(grid) I_(locked) I_(confidence) I_(trackerBeats) I_(rawBpm) I_(foldScale) I_(restarts)
  I_(acquisitions) I_(dt) I_(freeze)
#undef I_
  return nullptr;
}

// DanceMode's fold_: reset whenever the restarts change, as
// DanceMode::restart() does; beatInput never resets it.
struct Feed {
  dance::TempoFold fold;
  uint32_t restarts = 0;
  long frames = 0;
  // The Script's frame as DanceMode builds it, checked equal to the Script's.
  BeatInput operator()(const BeatInput& in) {
    const FrameFacts f = facts(in);
    if (f.restarts != restarts) {
      fold.reset();
      restarts = f.restarts;
    }
    const BeatInput b = dance::beatInput(f, fold);
    const char* d = inputDiff(in, b);
    CHECK(d == nullptr, "frame %ld: beatInput(facts(in), fold) differs from the Script's in %s", frames, d);
    ++frames;
    return b;
  }
};

const CrabCharacter& theCrab() {
  static const CrabCharacter c{};
  return c;
}
const ClassicStick& theStick() {
  static const ClassicStick c{};
  return c;
}

// One frame of DanceMode on the host: the Script's input through the Feed,
// the Choreographer's update, then DanceHalf.
struct Frame {
  BeatInput in;
  Cue cue;
  double u;
};
struct Rig {
  Script& s;
  dance::Choreographer ch;
  dance::DanceHalf half;
  Feed feed;
  explicit Rig(Script& script, const dance::Character* c = &theCrab()) : s(script) {
    ch.reset(0);
    ch.setCharacter(c);
    half.reset();
  }
  Frame step() {
    Frame f;
    f.in = feed(s.frame());
    f.cue = ch.update(f.in);
    f.u = half.follow(f.cue);
    return f;
  }
  void seconds(double secs) {
    const long n = std::lround(secs / static_cast<double>(s.dt()));
    for (long i = 0; i < n; ++i) step();
  }
};

// The expected k<n> poses (F.1's k<n> bullet; E.2.7).
crab::Pose crabK(int n) { return crab::Crab::frozen(static_cast<float>(n % 8) / 8.0f, n >= 8); }
dance::Pose stickK(int n) { return dance::Dancer().frozen(static_cast<float>(n % 8) / 8.0f, n >= 8); }

}  // namespace

void setUp() {}
void tearDown() {}

// 1. codePhase: phi = (float)(u - floor(u)), odd = floor(u) & 1.
void test_code_phase() {
  ctx("codePhase");
  for (int n = 0; n < 16; ++n) {
    const dance::CodePhase p = dance::codePhase(n / 8.0);
    CHECK(p.phi == static_cast<float>(n % 8) / 8.0f, "u = %d/8: phi %.9g, want %d/8", n, p.phi, n % 8);
    CHECK(p.odd == (n >= 8), "u = %d/8: odd %d", n, p.odd);
  }
  dance::CodePhase p = dance::codePhase(2.0);
  CHECK(p.phi == 0.0f && !p.odd, "u = 2: (%.9g, %d), want (0, false)", p.phi, p.odd);
  p = dance::codePhase(3.999);
  CHECK(p.odd, "u = 3.999: odd");
  CHECK(p.phi == static_cast<float>(3.999 - 3.0), "u = 3.999: phi %.9g", p.phi);
  p = dance::codePhase(1000000.25);
  CHECK(p.phi == 0.25f && !p.odd, "u = 1,000,000.25: (%.9g, %d), want (0.25, false)", p.phi, p.odd);
  p = dance::codePhase(1e-9);
  CHECK(p.phi == static_cast<float>(1e-9) && !p.odd, "u = 1e-9: (%.9g, %d)", p.phi, p.odd);
  p = dance::codePhase(3.999999);
  CHECK(p.odd && p.phi == static_cast<float>(3.999999 - 3.0), "u = 3.999999: (%.9g, %d)", p.phi, p.odd);
  p = dance::codePhase(1.0);
  CHECK(p.phi == 0.0f && p.odd, "u = 1: (%.9g, %d), want (0, true)", p.phi, p.odd);
  p = dance::codePhase(5.5);
  CHECK(p.phi == 0.5f && p.odd, "u = 5.5: (%.9g, %d), want (0.5, true)", p.phi, p.odd);
  p = dance::codePhase(0.0);
  CHECK(p.phi == 0.0f && !p.odd, "u = 0: (%.9g, %d)", p.phi, p.odd);
}

// 2a. The k-poses: weight 1, u = n/8: 0.7.0's frozen poses to the bit.
void test_k_poses_by_hand() {
  for (int v = 0; v < kVariants; ++v) {
    for (int n = 0; n < 16; ++n) {
      ctx("k%d, variant %d", n, v);
      const Cue c = cueWith(n / 8.0, 1.0f, 12.3f, v);
      CHECK_CRAB(crabK(n), CrabCharacter::pose(c, n / 8.0), "want Crab::frozen");
      CHECK_STICK(stickK(n), ClassicStick::pose(c, n / 8.0), "want Dancer::frozen");
      CHECK(CrabCharacter::paletteStep(c) == 32, "paletteStep %d, want 32", CrabCharacter::paletteStep(c));
    }
  }
}

// 2b. Dancing: weight 1, any u: dancePose at codePhase(u), whatever the idle
// clock says.
void test_dancing_by_hand() {
  std::vector<double> us;
  for (int i = 0; i <= 256; ++i) us.push_back(i / 64.0);
  us.push_back(1e-9);
  us.push_back(3.999999);
  const float idles[] = {0.0f, 777.7f};
  for (int v = 0; v < kVariants; ++v) {
    for (float s : idles) {
      for (double u : us) {
        ctx("dancing, u %.9g, idle %.1f, variant %d", u, s, v);
        const Cue c = cueWith(u, 1.0f, s, v);
        const dance::CodePhase p = dance::codePhase(u);
        CHECK_CRAB(crab::dancePose(p.phi, p.odd), CrabCharacter::pose(c, u), "want crab::dancePose");
        CHECK_STICK(dance::dancePose(p.phi, p.odd), ClassicStick::pose(c, u), "want dance::dancePose");
      }
    }
  }
}

// 2c. Idle: weight 0: idlePose at the cue's idle seconds, whatever u is.
void test_idle_by_hand() {
  const float idles[] = {0.0f, 0.033f, 1.7f, 399.99f, 999.97f, 1000.5f, 1999.97f};
  const double us[] = {0.0, 1.37};
  for (int v = 0; v < kVariants; ++v) {
    for (float s : idles) {
      for (double u : us) {
        ctx("idle, u %.2f, idle %.2f, variant %d", u, s, v);
        const Cue c = cueWith(u, 0.0f, s, v);
        CHECK_CRAB(crab::idlePose(s), CrabCharacter::pose(c, u), "want crab::idlePose");
        CHECK_STICK(dance::idlePose(s), ClassicStick::pose(c, u), "want dance::idlePose");
        CHECK(CrabCharacter::paletteStep(c) == 0, "paletteStep %d, want 0", CrabCharacter::paletteStep(c));
      }
    }
  }
}

// 2d. The fade: the blend at the cue's weight; 1.5 is 1 and -0.5 is 0.
void test_fade_by_hand() {
  const float weights[] = {0.001f, 0.2f, 0.4999f, 0.5f, 0.75f, 0.9999f};
  const double us[] = {0.0, 0.3, 0.875, 1.0, 1.61, 2.5, 3.99};
  const float idles[] = {0.0f, 2.1f, 450.25f, 1999.5f};
  for (int v = 0; v < kVariants; ++v) {
    for (float w : weights) {
      for (double u : us) {
        for (float s : idles) {
          ctx("fade, w %.4f, u %.3f, idle %.2f, variant %d", w, u, s, v);
          const Cue c = cueWith(u, w, s, v);
          const dance::CodePhase p = dance::codePhase(u);
          CHECK_CRAB(crab::blend(crab::idlePose(s), crab::dancePose(p.phi, p.odd), w), CrabCharacter::pose(c, u),
                     "want crab::blend");
          CHECK_STICK(dance::blend(dance::idlePose(s), dance::dancePose(p.phi, p.odd), w), ClassicStick::pose(c, u),
                      "want dance::blend");
        }
      }
    }
    for (double u : us) {
      ctx("out-of-range weights, u %.3f, variant %d", u, v);
      const dance::CodePhase p = dance::codePhase(u);
      const Cue hi = cueWith(u, 1.5f, 3.3f, v), lo = cueWith(u, -0.5f, 3.3f, v);
      CHECK_CRAB(crab::dancePose(p.phi, p.odd), CrabCharacter::pose(hi, u), "w 1.5: want dancePose");
      CHECK_STICK(dance::dancePose(p.phi, p.odd), ClassicStick::pose(hi, u), "w 1.5: want dancePose");
      CHECK_CRAB(crab::idlePose(3.3f), CrabCharacter::pose(lo, u), "w -0.5: want idlePose");
      CHECK_STICK(dance::idlePose(3.3f), ClassicStick::pose(lo, u), "w -0.5: want idlePose");
    }
  }
}

// 2e. paletteStep is crab::paletteStep of the cue's weight.
void test_palette_step() {
  const float weights[] = {0.0f, 0.03f, 0.5f, 0.97f, 1.0f, 0.2f, 1.5f, -0.5f};
  for (int v = 0; v < kVariants; ++v) {
    for (float w : weights) {
      ctx("paletteStep, w %.3f, variant %d", w, v);
      const Cue c = cueWith(0.75, w, 1.0f, v);
      CHECK(CrabCharacter::paletteStep(c) == crab::paletteStep(w), "paletteStep %d, want %d",
            CrabCharacter::paletteStep(c), crab::paletteStep(w));
    }
  }
}

// 3. DanceHalf: the cue's u while it names a beat move (a run, an ended
// run's fields, a freeze, auto-bounce), otherwise the last such u; 0 before
// any and after reset().
void test_dance_half() {
  ctx("DanceHalf");
  dance::DanceHalf h;
  h.reset();
  CHECK(h.u() == 0.0, "after reset: u %.9g", h.u());
  Cue none = cueWith(0.0, 0.0f, 0.0f, 0);
  none.beatMove = kNoMove;
  none.u = 0.0;
  CHECK(h.follow(none) == 0.0, "kNoMove first: want 0");
  CHECK(h.u() == 0.0, "u() after kNoMove first");
  Cue run = cueWith(1.25, 0.9f, 1.0f, 0);
  run.beatMove = 0;
  run.u = 1.25;
  CHECK(h.follow(run) == 1.25, "beat move 0 at 1.25");
  CHECK(h.u() == 1.25, "u() after 1.25");
  none.u = 0.0;
  CHECK(h.follow(none) == 1.25, "kNoMove after 1.25: held");
  none.u = 3.5;  // a kNoMove cue's u isn't taken either
  CHECK(h.follow(none) == 1.25, "kNoMove with u 3.5: held");
  CHECK(h.u() == 1.25, "u() held");
  Cue frozen = cueWith(0.625, 1.0f, 1.0f, 0);
  frozen.beatMove = 0;
  frozen.u = 0.625;
  frozen.beatPos16 = 491520;
  CHECK(h.follow(frozen) == 0.625, "freeze cue at 0.625");
  Cue bounce = cueWith(2.5, 1.0f, 1.0f, 0);
  bounce.beatMove = kAutoBounce;
  bounce.u = 2.5;
  CHECK(h.follow(bounce) == 2.5, "auto-bounce at 2.5: a beat move");
  CHECK(h.follow(none) == 2.5, "kNoMove after auto-bounce: held");
  h.reset();
  CHECK(h.u() == 0.0, "reset: u %.9g", h.u());
  CHECK(h.follow(none) == 0.0, "kNoMove after reset: 0");
  Cue zero = cueWith(0.0, 1.0f, 1.0f, 0);
  zero.beatMove = 0;
  zero.u = 0.0;
  run.u = 3.0;
  CHECK(h.follow(run) == 3.0, "beat move at 3");
  CHECK(h.follow(zero) == 0.0, "a beat move at u 0 is taken, not held");
}

// 4a. k0-k15 with the real Choreographer, each for 3 frames, while quiet and
// while locked at 120 BPM: Crab::frozen and Dancer::frozen to the bit, the
// beat dot on n % 8 = 0, weight 1. After k is cleared, the weight is the one
// a Choreographer that never saw the freeze has.
void test_k_frames_with_the_choreographer() {
  const int fpsList[] = {30, 24};
  for (int fps : fpsList) {
    Script s(fps);
    s.heard(false);
    dance::Choreographer ch, twin;
    ch.setCharacter(&theCrab());
    twin.setCharacter(&theCrab());
    dance::DanceHalf half;
    half.reset();
    Feed feed;
    auto frame = [&](int n, const char* phase) {
      const BeatInput in = feed(s.frame());
      BeatInput in2 = in;
      in2.freeze = -1;
      const Cue& c = ch.update(in);
      const Cue& t = twin.update(in2);
      const double u = half.follow(c);
      if (n >= 0) {
        ctx("%d fps, %s, k%d", fps, phase, n);
        CHECK(c.beatMove == 0, "beatMove %d, want 0 (shuffle, the freeze move)", c.beatMove);
        CHECK(c.u == n / 8.0, "cue u %.9g", c.u);
        CHECK(c.beatPos16 == static_cast<uint32_t>(n) * 98304u, "beatPos16 %u", c.beatPos16);
        CHECK(u == n / 8.0, "DanceHalf u %.9g", u);
        CHECK(c.weight == 1.0f, "weight %.9g", c.weight);
        CHECK(c.flash == (n % 8 == 0), "flash %d", c.flash);
        CHECK_CRAB(crabK(n), CrabCharacter::pose(c, u), "want Crab::frozen");
        CHECK_STICK(stickK(n), ClassicStick::pose(c, u), "want Dancer::frozen");
        CHECK(CrabCharacter::paletteStep(c) == 32, "paletteStep %d", CrabCharacter::paletteStep(c));
      } else {
        ctx("%d fps, %s, after k", fps, phase);
        CHECK(c.weight == t.weight, "weight %.9g, the unfrozen twin's %.9g", c.weight, t.weight);
      }
    };
    // Quiet: nothing heard since dance-on.
    for (int i = 0; i < 3; ++i) frame(-1, "quiet");
    for (int n = 0; n < 16; ++n) {
      s.freezeK(n);
      for (int i = 0; i < 3; ++i) frame(n, "quiet");
    }
    s.freezeK(-1);
    for (int i = 0; i < 30; ++i) frame(-1, "quiet");
    // Locked at 120 BPM, dancing.
    s.heard(true).acquire(120.0, s.trackTime() + 0.1);
    for (int i = 0; i < fps * 4; ++i) frame(-1, "locked");
    ctx("%d fps, locked, before k", fps);
    CHECK(ch.cue().state == DanceState::Dancing, "dancing before the k sheet");
    for (int n = 0; n < 16; ++n) {
      s.freezeK(n);
      for (int i = 0; i < 3; ++i) frame(n, "locked");
    }
    s.freezeK(-1);
    for (int i = 0; i < fps * 2; ++i) frame(-1, "locked");
    // And through a fade: unlocked while frozen, then unfrozen.
    s.freezeK(8);
    for (int i = 0; i < 3; ++i) frame(8, "locked");
    s.locked(false);
    for (int i = 0; i < fps; ++i) frame(8, "unlocked");
    s.freezeK(-1);
    for (int i = 0; i < fps; ++i) frame(-1, "unlocked");
  }
}

// 4b. The hold: a restart while dancing leaves the dance half where it was
// until the next run, and the crab draws the blend of that pose.
namespace {
// One restart: dancing at 120 BPM for `at` s, a restart, 2 s without a grid,
// then a re-lock; true when the held u isn't a whole move beat.
bool holdCase(int fps, double at) {
  Script s(fps);
  Rig r(s);
  s.acquire(120.0, 0.05);
  r.seconds(at);
  const Frame last = r.step();
  ctx("%d fps, restart at %.2f s, before the restart", fps, at);
  CHECK(last.cue.beatMove == 0, "dancing shuffle before the restart (beatMove %d)", last.cue.beatMove);
  const double held = last.u;
  CHECK(held == last.cue.u, "DanceHalf follows a running cue");
  s.restart();
  bool ran = false;
  int heldFrames = 0;
  for (int i = 0; i < fps * 10; ++i) {
    if (i == fps * 2) s.acquire(120.0, s.trackTime() + 0.2);  // the tracker locks again after 2 s
    const Frame f = r.step();
    ctx("%d fps, restart at %.2f s, frame %d after the restart", fps, at, i);
    if (i == 0) CHECK(f.cue.beatMove == kNoMove, "the restart frame's beatMove %d, want kNoMove", f.cue.beatMove);
    if (f.cue.beatMove != kNoMove) {
      ran = true;
      break;
    }
    CHECK(r.half.u() == held, "u %.9g, want the held %.9g", r.half.u(), held);
    CHECK(f.u == held, "follow() %.9g, want the held %.9g", f.u, held);
    const dance::CodePhase p = dance::codePhase(held);
    CHECK_CRAB(crab::blend(crab::idlePose(f.cue.idleSeconds), crab::dancePose(p.phi, p.odd), f.cue.weight),
               CrabCharacter::pose(f.cue, r.half.u()), "want the held blend");
    ++heldFrames;
  }
  ctx("%d fps, restart at %.2f s", fps, at);
  CHECK(ran, "a new run within 10 s of the restart");
  CHECK(heldFrames >= fps * 2, "held for %d frames, want the 2 s without a grid at least", heldFrames);
  return held != std::floor(held);
}
}  // namespace

void test_hold_through_a_restart() {
  const int fpsList[] = {30, 24};
  const double restartAt[] = {8.0, 8.4, 9.13};
  int midBeat = 0;
  for (int fps : fpsList)
    for (double at : restartAt)
      if (holdCase(fps, at)) ++midBeat;
  ctx("the hold");
  CHECK(midBeat > 0, "some restart falls between whole move beats");
}

// 4c. Leaving: unlocked mid-run, the dance half never goes back until a new
// run starts, and once the run has ended (Listening) it stays put.
void test_leaving() {
  const int fpsList[] = {30, 24};
  const double unlockAt[] = {6.0, 6.3, 6.55, 7.1};
  for (int fps : fpsList) {
    for (double at : unlockAt) {
      Script s(fps);
      Rig r(s);
      s.acquire(120.0, 0.05);
      r.seconds(at);
      ctx("%d fps, unlock at %.2f s", fps, at);
      CHECK(r.ch.run().status == dance::RunStatus::Running, "mid-run at the unlock");
      const int64_t start = r.ch.run().start;
      s.locked(false);
      double prev = r.half.u();
      bool listening = false;
      double settled = 0.0;
      for (int i = 0; i < fps * 5; ++i) {
        const Frame f = r.step();
        ctx("%d fps, unlock at %.2f s, frame %d", fps, at, i);
        CHECK(!(r.ch.run().status == dance::RunStatus::Running && r.ch.run().start != start),
              "no new run while unlocked");
        CHECK(f.u >= prev, "u went back from %.9g to %.9g", prev, f.u);
        prev = f.u;
        if (listening) {
          CHECK(f.u == settled, "u %.9g moved after the run ended (%.9g)", f.u, settled);
        } else if (f.cue.state == DanceState::Listening) {
          listening = true;
          settled = f.u;
        }
      }
      ctx("%d fps, unlock at %.2f s", fps, at);
      CHECK(listening, "Listening within 5 s of the unlock");
    }
  }
}

// 4d. Bounds over a long script: lock, unlock, pause, resume, restart,
// re-lock, a nudge, a re-acquire: u in [0, 4] (shuffle's and bounce's two
// plays of two beats), every pose field finite.
void test_bounds() {
  const int fpsList[] = {24, 30};
  const dance::Character* chars[] = {&theCrab(), &theStick()};
  for (int fps : fpsList) {
    for (const dance::Character* who : chars) {
      Script s(fps);
      Rig r(s, who);
      long n = 0;
      auto run = [&](double secs) {
        const long frames = std::lround(secs / static_cast<double>(s.dt()));
        for (long i = 0; i < frames; ++i, ++n) {
          const Frame f = r.step();
          ctx("%d fps, %s, frame %ld", fps, who->info().id, n);
          CHECK(f.u >= 0.0 && f.u <= 4.0, "u %.9g outside [0, 4]", f.u);
          CHECK(r.half.u() == f.u, "u() %.9g, follow() %.9g", r.half.u(), f.u);
          CHECK(crabFinite(CrabCharacter::pose(f.cue, f.u)), "a crab pose field isn't finite");
          CHECK(stickFinite(ClassicStick::pose(f.cue, f.u)), "a stick pose field isn't finite");
        }
      };
      s.heard(false);
      run(2.0);
      s.heard(true).acquire(120.0, 0.1);
      run(10.0);
      s.locked(false);
      run(4.0);
      s.locked(true);
      run(6.0);
      s.heard(false);
      run(4.0);
      s.heard(true);
      run(6.0);
      s.restart();
      run(2.0);
      s.acquire(96.0, s.trackTime() + 0.3);
      run(8.0);
      s.nudge(0.3);
      run(4.0);
      s.acquire(150.0, s.trackTime() + 0.17);
      run(6.0);
      s.locked(false);
      run(4.0);
      s.heard(false);
      run(4.0);
    }
  }
}

// 5a. beatInput: every field copied, freeze normalised, the fold applied
// only on grid frames and never reset.
void test_beat_input() {
  ctx("beatInput");
  FrameFacts f{};
  f.heard = true;
  f.grid = true;
  f.trackerBeats = 1234.5678;
  f.locked = true;
  f.confidence = 0.4375f;
  f.rawBpm = 123.25f;
  f.restarts = 7;
  f.acquisitions = 0xFFFFFFF0u;
  f.dt = 0.033f;
  f.freeze = 5;
  dance::TempoFold fold;
  BeatInput b = dance::beatInput(f, fold);
  CHECK(b.heard && b.grid && b.locked, "flags");
  CHECK(b.trackerBeats == 1234.5678, "trackerBeats %.9g", b.trackerBeats);
  CHECK(b.confidence == 0.4375f, "confidence");
  CHECK(b.rawBpm == 123.25f, "rawBpm");
  CHECK(b.restarts == 7u && b.acquisitions == 0xFFFFFFF0u, "restarts %u, acquisitions %u", b.restarts,
        b.acquisitions);
  CHECK(b.dt == 0.033f, "dt");
  CHECK(b.freeze == 5, "freeze %d", b.freeze);
  CHECK(b.foldScale == 1.0, "foldScale %.9g, want 1 (123.25 BPM)", b.foldScale);

  // Inconsistent facts are copied as they are: beatInput judges nothing.
  FrameFacts g = f;
  g.heard = false;
  g.grid = true;
  g.locked = false;
  g.trackerBeats = -3.5;
  g.confidence = 1.5f;
  g.rawBpm = 0.0f;
  g.dt = 0.0f;
  g.restarts = 0;
  g.acquisitions = 0;
  dance::TempoFold fold2;
  b = dance::beatInput(g, fold2);
  CHECK(!b.heard && b.grid && !b.locked, "flags copied as given");
  CHECK(b.trackerBeats == -3.5 && b.confidence == 1.5f && b.rawBpm == 0.0f && b.dt == 0.0f, "values copied as given");
  CHECK(b.restarts == 0u && b.acquisitions == 0u, "counters");

  // freeze: 0..15 kept, anything else -1.
  for (int v = -128; v <= 127; ++v) {
    FrameFacts h = f;
    h.freeze = static_cast<int8_t>(v);
    dance::TempoFold fl;
    const BeatInput o = dance::beatInput(h, fl);
    const int want = (v >= 0 && v <= 15) ? v : -1;
    CHECK(o.freeze == want, "freeze %d gives %d, want %d", v, o.freeze, want);
  }

  // The fold: apply() on grid frames, scale() otherwise, and a frame without a
  // grid leaves it alone. A fresh fold that saw 150 BPM would keep 170 at
  // scale 1; one that never saw it folds 170 to 85 (scale 1/2).
  {
    dance::TempoFold a, ref;
    FrameFacts ng = f;
    ng.grid = false;
    ng.rawBpm = 150.0f;
    const double before = a.scale();
    b = dance::beatInput(ng, a);
    CHECK(b.foldScale == before, "no grid: foldScale %.9g, want scale() %.9g", b.foldScale, before);
    CHECK(a.scale() == before, "no grid: the fold changed (%.9g -> %.9g)", before, a.scale());
    FrameFacts gr = f;
    gr.grid = true;
    gr.rawBpm = 170.0f;
    b = dance::beatInput(gr, a);
    const double want = ref.apply(170.0f);
    CHECK(want == 0.5, "the reference fold: 170 BPM from fresh is 1/2 (%.9g)", want);
    CHECK(b.foldScale == want, "grid after a no-grid frame: foldScale %.9g, want %.9g", b.foldScale, want);
    CHECK(a.scale() == ref.scale(), "the fold after it %.9g, want %.9g", a.scale(), ref.scale());
    // A no-grid frame with a tempo that would refold (300 BPM) changes nothing.
    ng.rawBpm = 300.0f;
    b = dance::beatInput(ng, a);
    CHECK(b.foldScale == 0.5 && a.scale() == 0.5, "no grid at 300 BPM: foldScale %.9g, scale %.9g", b.foldScale,
          a.scale());
    // It never resets the fold: a frame with new restarts keeps the scale
    // (150 at 1/2 is 75, inside 72-176); a reset fold would give 1.
    gr.rawBpm = 150.0f;
    gr.restarts = f.restarts + 1;
    b = dance::beatInput(gr, a);
    CHECK(b.foldScale == 0.5, "restarts changed: foldScale %.9g, want 0.5 (no reset)", b.foldScale);
    // And refolds past the band on a grid frame: 60 at 1/2 is 30, so 2 (120).
    gr.rawBpm = 60.0f;
    b = dance::beatInput(gr, a);
    dance::TempoFold r2;
    r2.apply(170.0f);
    r2.apply(150.0f);
    CHECK(b.foldScale == r2.apply(60.0f), "60 BPM: foldScale %.9g", b.foldScale);
  }

  // Over a whole Script (the Feed checks every frame against the Script's own
  // BeatInput, foldScale included): tempos that refold, a restart, a pause.
  const int fpsList[] = {24, 30};
  for (int fps : fpsList) {
    Script s(fps);
    Feed feed;
    auto run = [&](double secs) {
      const long n = std::lround(secs / static_cast<double>(s.dt()));
      for (long i = 0; i < n; ++i) feed(s.frame());
    };
    run(1.0);
    s.acquire(174.0, 0.2);
    run(3.0);
    s.drift(181.0);
    run(3.0);
    s.heard(false);
    run(1.0);
    s.heard(true);
    run(1.0);
    s.restart();
    run(1.0);
    s.acquire(70.0, s.trackTime() + 0.4);
    run(3.0);
    s.freezeK(3);
    run(1.0);
    s.freezeK(-1);
    s.grid(false);
    run(1.0);
    s.grid(true);
    run(1.0);
  }
}

// 5b. sceneOf: frozen is k<n> only, beat the grid, locked as given, the
// weight the cue's.
void test_scene_of() {
  ctx("sceneOf");
  const int8_t freezes[] = {-128, -2, -1, 0, 1, 7, 8, 15, 16, 127};
  const float weights[] = {0.0f, 0.25f, 0.5f, 0.5000001f, 1.0f};
  for (int8_t fz : freezes) {
    for (int grid = 0; grid < 2; ++grid) {
      for (int locked = 0; locked < 2; ++locked) {
        for (float w : weights) {
          FrameFacts f{};
          f.heard = true;
          f.grid = grid != 0;
          f.locked = locked != 0;
          f.freeze = fz;
          f.rawBpm = 120.0f;
          f.confidence = 0.7f;
          Cue c = cueWith(0.5, w, 1.0f, (grid + locked) % kVariants);
          const dancerate::Scene sc = dance::sceneOf(f, c);
          CHECK(sc.frozen == (fz >= 0 && fz <= 15), "freeze %d: frozen %d", fz, sc.frozen);
          CHECK(sc.beat == (grid != 0), "beat %d, grid %d", sc.beat, grid);
          CHECK(sc.locked == (locked != 0), "locked %d", sc.locked);
          CHECK(sc.weight == w, "weight %.9g, want %.9g", sc.weight, w);
        }
      }
    }
  }
}

namespace {

// The reference dancers, driven as 0.7.0's render() drove them.
struct Reference {
  crab::Crab crab;
  dance::Dancer dancer;
  dance::TempoFold fold;
  uint32_t restarts = 0;
  crab::Pose crabPose;
  dance::Pose stickPose;
  void update(const BeatInput& in) {
    if (in.restarts != restarts) {
      fold.reset();
      restarts = in.restarts;
    }
    dance::Step step;
    if (in.grid) step = dance::danceStep(in.trackerBeats, in.rawBpm, fold.apply(in.rawBpm));
    const bool beat = in.grid && in.locked;
    crabPose = crab.update(step.phi, step.odd, in.confidence, beat, in.dt);
    stickPose = dancer.update(step.phi, step.odd, in.confidence, beat, in.dt);
  }
};

}  // namespace

// 5c. Today's weight and today's idle: the Choreographer's weight equals
// Crab::update's and Dancer::update's on every frame, so DanceRate's mode is
// 0.7.0's; over the first 4 s (no lock, both weights exactly 0) the code
// characters' poses are the reference dancers' own.
void test_todays_weight_and_idle() {
  const int fpsList[] = {30, 24};
  for (int fps : fpsList) {
    Script s(fps);
    Rig r(s);
    Reference ref;
    double t = 0.0;
    int lockedFrames = 0, dancingFrames = 0;
    auto run = [&](double secs, const char* phase, bool idlePhase) {
      const long n = std::lround(secs / static_cast<double>(s.dt()));
      for (long i = 0; i < n; ++i) {
        const Frame f = r.step();
        ref.update(f.in);
        t += static_cast<double>(f.in.dt);
        ctx("%d fps, %s, t %.3f s", fps, phase, t);
        CHECK(f.cue.weight == ref.crab.weight(), "weight %.9g, Crab's %.9g", f.cue.weight, ref.crab.weight());
        CHECK(f.cue.weight == ref.dancer.weight(), "weight %.9g, Dancer's %.9g", f.cue.weight, ref.dancer.weight());
        const FrameFacts fa = facts(f.in);
        const dancerate::Scene sc = dance::sceneOf(fa, f.cue);
        CHECK(!sc.frozen && sc.beat == f.in.grid && sc.locked == f.in.locked && sc.weight == f.cue.weight,
              "sceneOf {%d, %d, %d, %.9g}", sc.frozen, sc.beat, sc.locked, sc.weight);
        dancerate::Scene old;
        old.frozen = false;
        old.beat = f.in.grid;
        old.locked = f.in.locked;
        old.weight = ref.crab.weight();
        CHECK(dancerate::mode(sc) == dancerate::mode(old), "mode %s, 0.7.0's %s",
              dancerate::modeName(dancerate::mode(sc)), dancerate::modeName(dancerate::mode(old)));
        if (f.in.grid && f.in.locked) ++lockedFrames;
        if (dancerate::mode(sc) == dancerate::Mode::Dancing) ++dancingFrames;
        if (idlePhase) {  // today's idle: the first 4 s, no lock yet
          CHECK(f.cue.weight == 0.0f, "weight %.9g in the first 4 s", f.cue.weight);
          CHECK_CRAB(ref.crabPose, CrabCharacter::pose(f.cue, f.u), "want Crab::update's");
          CHECK_STICK(ref.stickPose, ClassicStick::pose(f.cue, f.u), "want Dancer::update's");
        }
      }
    };
    s.heard(false);
    run(2.0, "quiet", true);
    s.heard(true).locked(false).confidence(0.3f).acquire(120.0, 0.25);
    run(2.0, "unlocked grid at 0.3", true);
    s.locked(true).confidence(0.6f);
    run(10.0, "locked at 0.6", false);
    s.confidence(0.2f);
    run(3.0, "locked at 0.2", false);
    s.heard(false);
    run(2.0, "paused", false);
    s.heard(true);
    run(3.0, "resumed", false);
    s.restart();
    run(0.5, "restarted", false);
    s.confidence(0.6f).acquire(120.0, s.trackTime() + 0.15);
    run(5.0, "locked again", false);
    s.locked(false);
    run(2.0, "unlocked", false);
    ctx("%d fps", fps);
    CHECK(lockedFrames > fps * 10, "the script locks (%d frames)", lockedFrames);
    CHECK(dancingFrames > fps * 10, "the scene dances (%d frames)", dancingFrames);
  }
}

// 5d. The switch: setCharacter(stick) mid-run waits for the next whole dance
// beat; the weight and the idle clock run on through it, and the drawn pose
// follows character() through the cast's renderer.
void test_switch() {
  const int fpsList[] = {30, 24};
  const double callAt[] = {5.0, 5.21, 5.47, 6.13};
  for (int fps : fpsList) {
    for (double at : callAt) {
      Script s(fps);
      Rig r(s);
      dance::DanceCast cast;
      TEST_ASSERT_TRUE(dance::addBuiltIns(cast, theCrab(), theStick()));
      s.acquire(120.0, 0.05);
      r.seconds(at);
      ctx("%d fps, switch at %.2f s", fps, at);
      CHECK(r.ch.run().status == dance::RunStatus::Running && r.ch.cue().showDance, "dancing at the call");
      const double clockAtCall = r.ch.danceClock();
      const double target = std::floor(clockAtCall) + 1.0;
      r.ch.setCharacter(&theStick());
      CHECK(r.ch.character() == &theCrab(), "still the crab right after the call");
      Cue prev = r.ch.cue();
      bool switched = false;
      for (int i = 0; i < fps * 2; ++i) {
        const Frame f = r.step();
        ctx("%d fps, switch at %.2f s, frame %d (clock %.6f, target %.0f)", fps, at, i, r.ch.danceClock(), target);
        CHECK(std::fabs(f.cue.weight - prev.weight) <= 1.0f / 12.0f, "weight %.9g after %.9g", f.cue.weight,
              prev.weight);
        CHECK(f.cue.idleSeconds == prev.idleSeconds + f.in.dt, "idleSeconds %.9g after %.9g + dt %.9g",
              f.cue.idleSeconds, prev.idleSeconds, f.in.dt);
        const bool due = r.ch.danceClock() >= target;
        if (!switched) {
          CHECK(r.ch.character() == (due ? static_cast<const dance::Character*>(&theStick())
                                          : static_cast<const dance::Character*>(&theCrab())),
                "character() is the %s", r.ch.character() == &theStick() ? "stick" : "crab");
          if (due) {
            switched = true;
            // SWITCH, then a fresh pick at the beat (E.2.8, U7): the stick's
            // bounce from its start.
            CHECK(f.cue.beatMove == 0 && f.cue.u == 0.0, "the stick's first run: beatMove %d, u %.9g",
                  f.cue.beatMove, f.cue.u);
          }
        } else {
          CHECK(r.ch.character() == &theStick(), "the stick from the switch on");
        }
        const int e = cast.indexOf(r.ch.character());
        CHECK(e >= 0, "the drawn character is in the cast");
        const dance::Renderer rd = cast.at(e).renderer;
        if (r.ch.character() == &theCrab()) {
          CHECK(rd == dance::Renderer::Crab, "the crab's entry draws with Renderer::Crab");
          CHECK(crabFinite(CrabCharacter::pose(f.cue, f.u)), "crab pose");
        } else {
          CHECK(rd == dance::Renderer::Classic, "the stick's entry draws with Renderer::Classic");
          CHECK(stickFinite(ClassicStick::pose(f.cue, f.u)), "stick pose");
        }
        prev = f.cue;
      }
      ctx("%d fps, switch at %.2f s", fps, at);
      CHECK(switched, "switched within 2 s");
    }
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_code_phase);
  RUN_TEST(test_k_poses_by_hand);
  RUN_TEST(test_dancing_by_hand);
  RUN_TEST(test_idle_by_hand);
  RUN_TEST(test_fade_by_hand);
  RUN_TEST(test_palette_step);
  RUN_TEST(test_dance_half);
  RUN_TEST(test_k_frames_with_the_choreographer);
  RUN_TEST(test_hold_through_a_restart);
  RUN_TEST(test_leaving);
  RUN_TEST(test_bounds);
  RUN_TEST(test_beat_input);
  RUN_TEST(test_scene_of);
  RUN_TEST(test_todays_weight_and_idle);
  RUN_TEST(test_switch);
  return UNITY_END();
}
