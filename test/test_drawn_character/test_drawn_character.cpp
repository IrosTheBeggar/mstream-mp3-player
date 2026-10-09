// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for DrawnCharacter and the free functions of Appendix B it
// carries (B.2's eases, B.3's hop and sway, B.4's frameAt, section 8's
// stageX, B.11's extent, fits and start spots): docs/DANCE-SPRITES.md,
// Appendix E.4's test_drawn_character list, item by item. Written from the
// doc alone, without reading the implementation.
//
// - The expected values are the doc's worked examples (B.2-B.4, B.11), the
//   fixture's labels (C.10) and BudgetVectors.h's frame traces, which
//   dancepack.py generated: frameAt must equal dancepack.py's frame_at bit
//   for bit.
// - B.2 and B.3 are also compared with this file's own integer reference,
//   written from B.0's rules (floor division, rhu), at every phase.
// - Packs are exact-size heap copies (packedit::HeapCopy), so ASan reports
//   a read past one.
//
// Run: pio test -e native -f test_drawn_character (and native_asan, E.4).
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "../support/dance/BeatScript.h"
#include "../support/dance/BudgetVectors.h"
#include "../support/dance/FoldVectors.h"
#include "DanceBlit.h"
#include "DanceCharacter.h"
#include "DancePack.h"
#include "DanceStage.h"
#include "DrawnCharacter.h"
#include "GenreFold.h"

using beatscript::CardDancer;
using beatscript::OpenPack;
using packedit::Bytes;

namespace {

char g_ctx[256] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

// --- B.0's integer rules, written again --------------------------------------

int64_t floorDiv(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}
int64_t rhu(int64_t a, int64_t b) { return floorDiv(2 * a + b, 2 * b); }

int64_t refEase(int e, int64_t f) {
  switch (e) {
    case 1: return f;
    case 2: return (f * f) >> 16;
    case 3: return 65536 - (((65536 - f) * (65536 - f)) >> 16);
    case 4: return (f * f * (196608 - 2 * f)) >> 32;
    default: return f < 65536 ? 0 : 65536;
  }
}
int64_t refHop(int64_t phi) {
  if (phi < 7864) return 0;
  const int64_t a = floorDiv((phi - 7864) * 65536, 57672);
  return (4 * a * (65536 - a)) >> 16;
}
int64_t refSway(int64_t b, int64_t phi) {
  const int64_t s = (b % 2 == 0) ? -1 : 1;
  return s * (65536 - 2 * refEase(4, phi));
}

constexpr int kSpotX[3] = {64, 160, 256};

dance::StageRect rect(int x0, int y0, int x1, int y1) {
  dance::StageRect r;
  r.x0 = static_cast<int16_t>(x0);
  r.y0 = static_cast<int16_t>(y0);
  r.x1 = static_cast<int16_t>(x1);
  r.y1 = static_cast<int16_t>(y1);
  return r;
}
bool sameRect(const dance::StageRect& a, const dance::StageRect& b) {
  return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1;
}
bool emptyRect(const dance::StageRect& r) { return r.x0 == 0 && r.y0 == 0 && r.x1 == 0 && r.y1 == 0; }
// B.5's union: an empty one adds nothing.
dance::StageRect unite(const dance::StageRect& a, const dance::StageRect& b) {
  const bool ea = a.x1 <= a.x0 || a.y1 <= a.y0, eb = b.x1 <= b.x0 || b.y1 <= b.y0;
  if (ea && eb) return rect(0, 0, 0, 0);
  if (ea) return b;
  if (eb) return a;
  return rect(std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1));
}

void expectRect(const dance::StageRect& want, const dance::StageRect& got, const char* what) {
  char msg[400];
  std::snprintf(msg, sizeof msg, "%s: %s: want %d,%d-%d,%d, got %d,%d-%d,%d", g_ctx, what, want.x0, want.y0, want.x1,
                want.y1, got.x0, got.y0, got.x1, got.y1);
  TEST_ASSERT_TRUE_MESSAGE(sameRect(want, got), msg);
}

// The fixture as a card pack (E.1's split); kept for the whole run.
CardDancer& fixture() {
  static CardDancer* d = nullptr;
  if (!d) {
    d = new CardDancer();
    TEST_ASSERT_TRUE_MESSAGE(d->load(packedit::testPack()), "the fixture loads as a card pack");
  }
  return *d;
}

int stepStart(const dance::Pack& p, int move, int i) {
  int s = 0;
  for (int k = 0; k < i; ++k) s += p.step(move, k).dur;
  return s;
}
int stepSum(const dance::Pack& p, int move) { return stepStart(p, move, p.moveRecord(move).stepCount); }

}  // namespace

void setUp() {}
void tearDown() {}

// ===========================================================================
// 1. B.2 and B.3.

void test_eases_hop_sway() {
  const int32_t fs[5] = {0, 16384, 32768, 49152, 65536};
  const int32_t table[5][5] = {
      {0, 0, 0, 0, 65536},              // hold
      {0, 16384, 32768, 49152, 65536},  // linear
      {0, 4096, 16384, 36864, 65536},   // in
      {0, 28672, 49152, 61440, 65536},  // out
      {0, 10240, 32768, 55296, 65536},  // inout
  };
  for (int e = 0; e < 5; ++e)
    for (int i = 0; i < 5; ++i) {
      ctx("ease %d at f %d", e, fs[i]);
      TEST_ASSERT_EQUAL_INT32_MESSAGE(table[e][i], dance::ease16(e, fs[i]), g_ctx);
    }
  // Every f against B.2's formulas; a code above 4 acts as hold.
  for (int e = 0; e <= 7; ++e)
    for (int32_t f = 0; f <= 65536; f += (f < 65530 ? 7 : 1)) {
      ctx("ease %d at f %d", e, f);
      TEST_ASSERT_EQUAL_INT32_MESSAGE(static_cast<int32_t>(refEase(e > 4 ? 0 : e, f)), dance::ease16(e, f), g_ctx);
    }
  // B.3's hop table, then every phase.
  const int32_t phis[10] = {0, 7863, 7864, 7865, 7866, 16384, 32768, 36700, 49152, 65535};
  const int32_t hops[10] = {0, 0, 0, 3, 7, 33003, 64317, 65536, 53317, 7};
  for (int i = 0; i < 10; ++i) {
    ctx("hop at phi %d", phis[i]);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(hops[i], dance::hop16(phis[i]), g_ctx);
  }
  for (int32_t phi = 0; phi < 65536; ++phi) {
    if (dance::hop16(phi) != refHop(phi)) {
      ctx("hop at phi %d: want %lld, got %d", phi, static_cast<long long>(refHop(phi)), dance::hop16(phi));
      TEST_FAIL_MESSAGE(g_ctx);
    }
  }
  // B.3's sway values, then a spread of beats and phases.
  TEST_ASSERT_EQUAL_INT32(-65536, dance::sway16(0, 0));
  TEST_ASSERT_EQUAL_INT32(0, dance::sway16(0, 32768));
  TEST_ASSERT_EQUAL_INT32(65534, dance::sway16(0, 65535));
  TEST_ASSERT_EQUAL_INT32(65536, dance::sway16(1, 0));
  const int64_t beats[] = {0, 1, 2, 3, 7, 4095, 4096, int64_t(1) << 40, (int64_t(1) << 40) + 1};
  for (int64_t b : beats)
    for (int32_t phi = 0; phi < 65536; phi += 13) {
      ctx("sway at beat %lld, phi %d", static_cast<long long>(b), phi);
      TEST_ASSERT_EQUAL_INT32_MESSAGE(static_cast<int32_t>(refSway(b, phi)), dance::sway16(b, phi), g_ctx);
    }
}

// ===========================================================================
// 2. B.4's worked example, its hold-travel variant, e16 47186, the clamp
//    example and B.3's 1-beat sway.

namespace {
struct B4Row {
  int tick;
  // unmirrored from spot -1
  int f0, m0, rx0, ry0;
  dance::StageRect box0, shadow0;
  // mirrored from spot +1
  int f1, m1, rx1, ry1;
  dance::StageRect box1, shadow1;
};
const dance::StageRect kNone = {};
}  // namespace

void test_b4_worked_example() {
  OpenPack op;
  TEST_ASSERT_TRUE(op.open(beatscript::b4Pack()));
  TEST_ASSERT_EQUAL_INT(0, op.report.dropped);
  const dance::Pack& p = op.pack;
  const B4Row rows[] = {
      {0, 0, 0, 62, 153, rect(58, 141, 66, 153), rect(57, 152, 67, 155),
       0, 1, 258, 153, rect(256, 141, 264, 153), rect(253, 152, 263, 155)},
      {3, 0, 0, 65, 149, rect(61, 137, 69, 149), rect(64, 152, 66, 155),
       0, 1, 255, 149, rect(253, 137, 261, 149), rect(254, 152, 256, 155)},
      {6, 0, 0, 69, 146, rect(65, 134, 73, 146), kNone,
       0, 1, 252, 146, rect(250, 134, 258, 146), kNone},
      {12, 0, 1, 72, 149, rect(70, 137, 78, 149), rect(71, 152, 73, 155),
       0, 0, 248, 149, rect(244, 137, 252, 149), rect(247, 152, 249, 155)},
      {15, 0, 1, 86, 147, rect(84, 135, 92, 147), kNone,
       0, 0, 234, 147, rect(230, 135, 238, 147), kNone},
      {18, 0, 1, 117, 146, rect(115, 134, 123, 146), kNone,
       0, 0, 204, 146, rect(200, 134, 208, 146), kNone},
      {24, 0, 0, 158, 153, rect(154, 141, 162, 153), rect(153, 152, 163, 155),
       0, 1, 162, 153, rect(160, 141, 168, 153), rect(157, 152, 167, 155)},
  };
  for (const B4Row& r : rows) {
    for (int side = 0; side < 2; ++side) {
      const bool mirrored = side == 1;
      const int spot = mirrored ? 1 : -1;
      ctx("B.4 tick %d, %s from spot %d", r.tick, mirrored ? "mirrored" : "unmirrored", spot);
      const dance::Placement pl = dance::frameAt(p, 0, int64_t(r.tick) * 65536, mirrored, spot);
      TEST_ASSERT_EQUAL_INT_MESSAGE(mirrored ? r.f1 : r.f0, pl.frame, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(mirrored ? r.m1 : r.m0, pl.mirrored ? 1 : 0, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(mirrored ? r.rx1 : r.rx0, pl.rootX, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(mirrored ? r.ry1 : r.ry0, pl.rootY, g_ctx);
      const dance::StageRect& box = mirrored ? r.box1 : r.box0;
      const dance::StageRect& sh = mirrored ? r.shadow1 : r.shadow0;
      expectRect(box, pl.box, "box");
      expectRect(sh, pl.shadow, "shadow");
      expectRect(unite(box, sh), pl.rect, "rect");
    }
  }
  // travelEase hold: the root lands at tick 24.
  const int ticks[5] = {0, 12, 23, 24, 30};
  const int rootX[5] = {62, 72, 63, 158, 165};
  for (int i = 0; i < 5; ++i) {
    ctx("B.4 with travelEase hold, tick %d", ticks[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(rootX[i], dance::frameAt(p, 1, int64_t(ticks[i]) * 65536, false, -1).rootX, g_ctx);
  }
  // At e16 = 47186 (0.72) and tick 15, unmirrored: travel isn't scaled.
  ctx("B.4 at e16 47186, tick 15");
  const dance::Placement e72 = dance::frameAt(p, 0, 15 * 65536, false, -1, 47186);
  TEST_ASSERT_EQUAL_INT_MESSAGE(84, e72.rootX, g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(149, e72.rootY, g_ctx);
  expectRect(rect(82, 137, 90, 149), e72.box, "box");
  expectRect(rect(83, 152, 85, 155), e72.shadow, "shadow");
  // B.3's 1-beat sway: at scale 2 with sway 2 from the centre spot.
  const int swayTicks[5] = {0, 6, 12, 18, 24};
  const int swayX[5] = {156, 160, 164, 160, 156};
  for (int i = 0; i < 5; ++i) {
    ctx("B.3's sway at tick %d", swayTicks[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE(swayX[i], dance::frameAt(p, 2, int64_t(swayTicks[i]) * 65536, false, 0).rootX,
                                  g_ctx);
  }
}

void test_clamp_example() {
  OpenPack op;
  TEST_ASSERT_TRUE(op.open(beatscript::clampPack()));
  ctx("the clamp example, unmirrored");
  const dance::Placement u = dance::frameAt(op.pack, 0, 0, false, -1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(64, u.rootX, g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(153, u.rootY, g_ctx);
  expectRect(rect(0, 29, 128, 153), u.box, "box");
  expectRect(rect(44, 152, 84, 155), u.shadow, "shadow");
  expectRect(rect(0, 29, 128, 155), u.rect, "rect");
  ctx("the clamp example, mirrored");
  const dance::Placement m = dance::frameAt(op.pack, 0, 0, true, -1);
  TEST_ASSERT_EQUAL_INT_MESSAGE(80, m.rootX, g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(153, m.rootY, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(m.mirrored, g_ctx);
  expectRect(rect(18, 29, 146, 153), m.box, "box");
  // B.5: sw = 40 (no hop), x0 = rootX - 20.
  expectRect(rect(60, 152, 100, 155), m.shadow, "shadow");
  // Pushed left at the right edge too: mirrored from spot +1 the key is +16
  // px, root 272, left 272 - 62 = 210, right 338: 18 px in.
  ctx("the clamp example, mirrored from spot +1");
  const dance::Placement r = dance::frameAt(op.pack, 0, 0, true, 1);
  expectRect(rect(192, 29, 320, 153), r.box, "box");
  TEST_ASSERT_EQUAL_INT_MESSAGE(254, r.rootX, g_ctx);
  expectRect(rect(234, 152, 274, 155), r.shadow, "shadow");
}

// ===========================================================================
// 3. Step lookup at exact tick edges, and the wrap.

void test_step_lookup() {
  OpenPack op;
  TEST_ASSERT_TRUE(op.open(packedit::testPack()));
  const dance::Pack& p = op.pack;
  for (int m = 0; m < p.moveCount(); ++m) {
    const int n = p.moveRecord(m).stepCount;
    const int L = stepSum(p, m);
    for (int o = 0; o < 2; ++o) {
      const bool mirrored = o == 1;
      auto expectStep = [&](int64_t pos16, int i) {
        const dance::StepRecord s = p.step(m, i);
        ctx("move %d (%s), %s, pos16 %lld: step %d", m, p.moveName(m), mirrored ? "mirrored" : "unmirrored",
            static_cast<long long>(pos16), i);
        const dance::Placement pl = dance::frameAt(p, m, pos16, mirrored, 0);
        TEST_ASSERT_EQUAL_INT_MESSAGE(s.frame, pl.frame, g_ctx);
        TEST_ASSERT_EQUAL_MESSAGE(s.mirrored != mirrored, pl.mirrored, g_ctx);
      };
      expectStep(0, 0);
      for (int i = 1; i < n; ++i) {
        const int64_t s = int64_t(stepStart(p, m, i)) * 65536;
        expectStep(s - 1, i - 1);
        expectStep(s, i);
      }
      expectStep(int64_t(L) * 65536 - 1, n - 1);
      expectStep(int64_t(L) * 65536, 0);              // the wrap back to step 0
      expectStep(int64_t(L) * 65536 * 3 + 1, 0);      // and on, never wrapped by the caller
      if (n > 1) expectStep(int64_t(L) * 65536 + int64_t(stepStart(p, m, 1)) * 65536, 1);
    }
  }
  // pos16 < 0 counts as 0 (E.1: where dancepack.py raises, the C++ clamps).
  ctx("pos16 -1 is 0");
  const dance::Placement a = dance::frameAt(p, 2, -1, false, 0), b = dance::frameAt(p, 2, 0, false, 0);
  TEST_ASSERT_EQUAL_INT(b.frame, a.frame);
  TEST_ASSERT_EQUAL(b.mirrored, a.mirrored);
  TEST_ASSERT_EQUAL_INT(b.rootX, a.rootX);
  TEST_ASSERT_EQUAL_INT(b.rootY, a.rootY);
}

// ===========================================================================
// 4. Mirror symmetry: B.11's mirrored offsets, L' = scale - R and R' = scale - L.

void test_mirror_symmetry() {
  OpenPack op;
  TEST_ASSERT_TRUE(op.open(packedit::testPack()));
  const dance::Pack& p = op.pack;
  const dance::PackGeom g = p.geom();
  const int scale = g.scale;
  // Each frame's box offset from the root, seen drawn unmirrored and mirrored.
  std::vector<int> seen0(p.frameCount(), 0), seen1(p.frameCount(), 0);
  std::vector<int> L0(p.frameCount(), 0), R0(p.frameCount(), 0), L1(p.frameCount(), 0), R1(p.frameCount(), 0);
  for (int m = 0; m < p.moveCount(); ++m) {
    for (int i = 0; i < p.moveRecord(m).stepCount; ++i) {
      for (int o = 0; o < 2; ++o) {
        const dance::Placement pl = dance::frameAt(p, m, int64_t(stepStart(p, m, i)) * 65536, o == 1, 0);
        const dance::FrameBox b = p.box(pl.frame);
        if (b.w == 0) {
          ctx("move %d step %d: a blank frame has no box", m, i);
          TEST_ASSERT_TRUE_MESSAGE(emptyRect(pl.box), g_ctx);
          continue;
        }
        const int L = pl.box.x0 - pl.rootX, R = pl.box.x1 - pl.rootX;
        ctx("move %d step %d, %s: frame %d drawn %s", m, i, o ? "mirrored" : "unmirrored", pl.frame,
            pl.mirrored ? "mirrored" : "unmirrored");
        // B.4 step 10, then B.11's L and R.
        const int wantL = pl.mirrored ? (g.anchorX - b.x0 - b.w + 1) * scale : (b.x0 - g.anchorX) * scale;
        TEST_ASSERT_EQUAL_INT_MESSAGE(wantL, L, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(wantL + b.w * scale, R, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(pl.rootY + (b.y0 - g.anchorY) * scale, pl.box.y0, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(pl.box.y0 + b.h * scale, pl.box.y1, g_ctx);
        if (pl.mirrored) {
          seen1[pl.frame] = 1;
          L1[pl.frame] = L;
          R1[pl.frame] = R;
        } else {
          seen0[pl.frame] = 1;
          L0[pl.frame] = L;
          R0[pl.frame] = R;
        }
      }
    }
  }
  int pairs = 0;
  for (int f = 0; f < p.frameCount(); ++f) {
    if (!seen0[f] || !seen1[f]) continue;
    ctx("frame %d: L' = scale - R and R' = scale - L", f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(scale - R0[f], L1[f], g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(scale - L0[f], R1[f], g_ctx);
    ++pairs;
  }
  // Every drawn frame (frame 8 is the blank B4) seen both ways round.
  TEST_ASSERT_EQUAL_INT(p.frameCount() - 1, pairs);
}

// ===========================================================================
// 5. The bounds hold every pixel.

void test_bounds_hold_every_pixel() {
  CardDancer& d = fixture();
  const dance::Pack& p = d.pack;
  const dance::PackGeom g = p.geom();
  packedit::CanaryCanvas cv(320, 164, 6, 0xA5A5, 0x0000);
  uint16_t lut[16];
  for (int i = 0; i < 16; ++i) lut[i] = static_cast<uint16_t>(0x8000 | i);
  int blits = 0;
  for (int m = 0; m < p.moveCount(); ++m) {
    if (!p.moveKept(m)) continue;
    const bool travels = p.moveRecord(m).travel != 0;
    const int L = stepSum(p, m);
    for (int o = 0; o < 2; ++o) {
      int lo = 0, hi = 0;
      const bool hasExtent = dance::extent(p, m, o == 1, &lo, &hi);
      for (int spot = -1; spot <= 1; ++spot) {
        for (int32_t e16 : {39322, 65536}) {
          for (int t = 0; t <= 2 * L; t += 7) {
            const dance::Placement pl = dance::frameAt(p, m, int64_t(t) * 65536, o == 1, spot, e16);
            ctx("move %s, %s, spot %d, e16 %d, tick %d", p.moveName(m), o ? "mirrored" : "unmirrored", spot, e16, t);
            const dance::FrameBox fb = p.box(pl.frame);
            if (fb.w == 0) {
              TEST_ASSERT_TRUE_MESSAGE(emptyRect(pl.box), g_ctx);
              continue;
            }
            const dance::StageRect& b = pl.box;
            TEST_ASSERT_TRUE_MESSAGE(b.x0 >= 0 && b.y0 >= 0 && b.x1 <= 320 && b.y1 <= 164, g_ctx);
            TEST_ASSERT_EQUAL_INT_MESSAGE(fb.w * g.scale, b.x1 - b.x0, g_ctx);
            TEST_ASSERT_EQUAL_INT_MESSAGE(fb.h * g.scale, b.y1 - b.y0, g_ctx);
            dance::blit(cv.canvas(), p.frame(pl.frame), b.x0, b.y0, g.scale, pl.mirrored, lut);
            ++blits;
            int inside = 0;
            for (int y = 0; y < 164; ++y)
              for (int x = 0; x < 320; ++x) {
                const uint16_t v = cv.at(x, y);
                if (v == 0) continue;
                const bool in = x >= b.x0 && x < b.x1 && y >= b.y0 && y < b.y1;
                if (!in) {
                  ctx("move %s, %s, spot %d, e16 %d, tick %d: pixel %d,%d written outside the box %d,%d-%d,%d",
                      p.moveName(m), o ? "mirrored" : "unmirrored", spot, e16, t, x, y, b.x0, b.y0, b.x1, b.y1);
                  TEST_FAIL_MESSAGE(g_ctx);
                }
                ++inside;
                cv.at(x, y) = 0;
              }
            TEST_ASSERT_TRUE_MESSAGE(inside > 0, g_ctx);
            TEST_ASSERT_TRUE_MESSAGE(cv.borderIntact(), g_ctx);
            if (!travels && spot == 0) {
              TEST_ASSERT_TRUE_MESSAGE(hasExtent, g_ctx);
              TEST_ASSERT_TRUE_MESSAGE(b.x0 - 160 >= lo && b.x1 - 160 <= hi, g_ctx);
            }
          }
        }
      }
    }
  }
  TEST_ASSERT_TRUE(blits > 2000);
}

// ===========================================================================
// 6. stageX.

void test_stage_x() {
  for (int e = 0; e < 5; ++e)
    for (int from = -1; from <= 1; ++from)
      for (int to = -1; to <= 1; ++to) {
        ctx("stageX(%d, %d) with ease %d", from, to, e);
        TEST_ASSERT_EQUAL_INT_MESSAGE(kSpotX[from + 1], dance::stageX(from, to, 0, e), g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(kSpotX[to + 1], dance::stageX(from, to, 65536, e), g_ctx);
        for (int32_t p16 = 0; p16 <= 65536; p16 += 97) {
          const int64_t want =
              kSpotX[from + 1] + rhu(int64_t(kSpotX[to + 1] - kSpotX[from + 1]) * refEase(e, p16), 65536);
          ctx("stageX(%d, %d, %d) with ease %d", from, to, p16, e);
          TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(want), dance::stageX(from, to, p16, e), g_ctx);
        }
      }
  // A leftward travel rounds half up through a floor (B.0): -96 * 1000 /
  // 65536 is -1.46, so 159 (a truncating (a + 2^15) / 2^16 would give 160);
  // -1.5 rounds up to -1; -2.001 to -2; and +1.5 to +2.
  TEST_ASSERT_EQUAL_INT(159, dance::stageX(0, -1, 1000, 1));
  TEST_ASSERT_EQUAL_INT(159, dance::stageX(0, -1, 1024, 1));
  TEST_ASSERT_EQUAL_INT(158, dance::stageX(0, -1, 1366, 1));
  TEST_ASSERT_EQUAL_INT(162, dance::stageX(0, 1, 1024, 1));
  // Spots past +-1 are clamped.
  TEST_ASSERT_EQUAL_INT(64, dance::stageX(-5, 7, 0, 4));
  TEST_ASSERT_EQUAL_INT(256, dance::stageX(-5, 7, 65536, 4));
  TEST_ASSERT_EQUAL_INT(256, dance::stageX(2, -3, 0, 1));
  TEST_ASSERT_EQUAL_INT(64, dance::stageX(2, -3, 65536, 1));
}

// ===========================================================================
// 7. B.11: extent, fits and start spots.

void test_extent_fits_start_spots() {
  {
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(beatscript::b4Pack()));
    int lo = 0, hi = 0;
    TEST_ASSERT_TRUE(dance::extent(op.pack, 0, false, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(-6, lo);
    TEST_ASSERT_EQUAL_INT(14, hi);
    TEST_ASSERT_EQUAL_HEX8(0x03, dance::startSpotMask(op.pack, 0, false));  // spots -1 and 0
    // Mirrored, the extent is (scale - hi, scale - lo) = (-12, 8): spots 0 and +1.
    TEST_ASSERT_TRUE(dance::extent(op.pack, 0, true, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(-12, lo);
    TEST_ASSERT_EQUAL_INT(8, hi);
    TEST_ASSERT_EQUAL_HEX8(0x06, dance::startSpotMask(op.pack, 0, true));
    // A move without travel: 0b111.
    TEST_ASSERT_EQUAL_HEX8(0x07, dance::startSpotMask(op.pack, 2, false));
  }
  {
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(beatscript::wrapPack()));
    int lo = 0, hi = 0;
    TEST_ASSERT_TRUE(dance::extent(op.pack, 0, false, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(-36, lo);
    TEST_ASSERT_EQUAL_INT(16, hi);
  }
  {
    // A move whose boxes are all empty: no extent (lo, hi untouched), fits anywhere.
    beatscript::MiniMove m;
    m.name = "blank";
    m.beats = 2;
    m.travel = 1;
    m.travelEase = 4;
    m.steps = {{1, false, 24, 5, 0, 0, false}};
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(beatscript::miniPack({2, 8, 8, 4, 8, 0}, {{2, 2, 4, 6}, {0, 0, 0, 0}}, {m})));
    int lo = 1234, hi = 5678;
    TEST_ASSERT_FALSE(dance::extent(op.pack, 0, false, &lo, &hi));
    TEST_ASSERT_EQUAL_INT(1234, lo);
    TEST_ASSERT_EQUAL_INT(5678, hi);
    for (int s = -1; s <= 1; ++s) TEST_ASSERT_TRUE(dance::fits(op.pack, 0, false, s));
    TEST_ASSERT_EQUAL_HEX8(0x03, dance::startSpotMask(op.pack, 0, false));
    TEST_ASSERT_EQUAL_HEX8(0x06, dance::startSpotMask(op.pack, 0, true));
  }
  OpenPack op;
  TEST_ASSERT_TRUE(op.open(packedit::testPack()));
  const dance::Pack& p = op.pack;
  using namespace beatscript;
  // walk: travel +1, start_at side: from -1 unmirrored, from +1 mirrored.
  TEST_ASSERT_EQUAL_HEX8(0x01, dance::startSpotMask(p, kWalk, false));
  TEST_ASSERT_EQUAL_HEX8(0x04, dance::startSpotMask(p, kWalk, true));
  for (int m = 0; m < p.moveCount(); ++m) {
    if (m == kWalk) continue;
    ctx("move %d: no travel, every spot", m);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x07, dance::startSpotMask(p, m, false), g_ctx);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x07, dance::startSpotMask(p, m, true), g_ctx);
  }
  int lo = 0, hi = 0;
  TEST_ASSERT_TRUE(dance::extent(p, kHeavy, false, &lo, &hi));
  TEST_ASSERT_EQUAL_INT(-96, lo);
  TEST_ASSERT_EQUAL_INT(96, hi);
  TEST_ASSERT_TRUE(dance::extent(p, kHeavy, true, &lo, &hi));
  TEST_ASSERT_EQUAL_INT(-92, lo);
  TEST_ASSERT_EQUAL_INT(100, hi);
  TEST_ASSERT_TRUE(dance::extent(p, kBig, false, &lo, &hi));
  TEST_ASSERT_EQUAL_INT(-64, lo);
  TEST_ASSERT_EQUAL_INT(64, hi);
  TEST_ASSERT_TRUE(dance::extent(p, kBig, true, &lo, &hi));
  TEST_ASSERT_EQUAL_INT(-60, lo);
  TEST_ASSERT_EQUAL_INT(68, hi);
  for (int o = 0; o < 2; ++o) {
    TEST_ASSERT_FALSE(dance::fits(p, kHeavy, o == 1, -1));
    TEST_ASSERT_TRUE(dance::fits(p, kHeavy, o == 1, 0));
    TEST_ASSERT_FALSE(dance::fits(p, kHeavy, o == 1, 1));
  }
  TEST_ASSERT_TRUE(dance::fits(p, kBig, false, -1));
  TEST_ASSERT_TRUE(dance::fits(p, kBig, false, 0));
  TEST_ASSERT_TRUE(dance::fits(p, kBig, false, 1));
  TEST_ASSERT_TRUE(dance::fits(p, kBig, true, -1));
  TEST_ASSERT_TRUE(dance::fits(p, kBig, true, 0));
  TEST_ASSERT_FALSE(dance::fits(p, kBig, true, 1));
  // Every move's fit agrees with its extent (B.11's fits), every spot.
  for (int m = 0; m < p.moveCount(); ++m)
    for (int o = 0; o < 2; ++o) {
      int l = 0, h = 0;
      const bool has = dance::extent(p, m, o == 1, &l, &h);
      for (int s = -1; s <= 1; ++s) {
        ctx("move %d, %s, spot %d", m, o ? "mirrored" : "unmirrored", s);
        const bool want = !has || (kSpotX[s + 1] + l >= 0 && kSpotX[s + 1] + h <= 320);
        TEST_ASSERT_EQUAL_MESSAGE(want, dance::fits(p, m, o == 1, s), g_ctx);
      }
    }
}

// ===========================================================================
// 8. The view of the fixture.

void test_view_of_the_fixture() {
  using namespace beatscript;
  {
    // Not open: init false, an empty view.
    dance::Pack none;
    std::unique_ptr<dance::DrawnCharacter> v(new dance::DrawnCharacter());
    TEST_ASSERT_FALSE(v->init(none, dance::Origin::Card, true));
    TEST_ASSERT_EQUAL_INT(0, v->moveCount());
    TEST_ASSERT_EQUAL_INT(-1, v->packMove(0));
    // Scanned, not open: the same.
    OpenPack sp;
    const Bytes t = packedit::testPack();
    sp.bytes.assign(t.data(), 2048);
    TEST_ASSERT_TRUE(sp.pack.scan(sp.bytes.data(), 2048, static_cast<uint32_t>(t.size())));
    TEST_ASSERT_FALSE(v->init(sp.pack, dance::Origin::Card, true));
    TEST_ASSERT_EQUAL_INT(0, v->moveCount());
  }
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  const dance::Pack& p = d.pack;
  TEST_ASSERT_EQUAL_INT(13, c.moveCount());
  for (int i = 0; i < 13; ++i) TEST_ASSERT_EQUAL_INT(i < 8 ? i : i + 1, c.packMove(i));
  TEST_ASSERT_EQUAL_INT(-1, c.packMove(13));
  TEST_ASSERT_EQUAL_INT(-1, c.packMove(-1));
  TEST_ASSERT_EQUAL_INT(14, c.pack().moveCount());
  TEST_ASSERT_EQUAL_PTR(p.name(), c.pack().name());  // a view: the same bytes
  // The labels C.10 fixes.
  struct Want {
    const char* name;
    dance::MoveKind kind;
    int beats, lo, hi, weight10, when, flags, repeat, travel;
    dance::StartAt startAt;
    int from, to, fromM, toM, idleMs, genres;
  };
  using dance::MoveKind;
  using dance::StartAt;
  const Want want[13] = {
      {"bounce", MoveKind::Beat, 2, 0, 0, 10, 0, dance::kFreeze, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 0},
      {"pulse", MoveKind::Beat, 1, 0, 0, 10, 0, dance::kSnap, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 0},
      {"step-touch", MoveKind::Beat, 2, 0, 0, 10, 0, dance::kMirror, 0, 0, StartAt::Any, 1, 1, 1, 1, 0, 0},
      {"point", MoveKind::Beat, 4, 90, 130, 3, 0, dance::kMirror | dance::kPlaceStart, 2, 0, StartAt::Any, 1, 2, 1, 3,
       0, 0},
      {"slow", MoveKind::Beat, 4, 36, 60, 25, 0, 0, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 0},
      {"fast", MoveKind::Beat, 1, 150, 180, 10, 0, dance::kPlaceEnd, 0, 0, StartAt::Any, 4, 1, 4, 1, 0, 0},
      {"walk", MoveKind::Beat, 4, 90, 140, 10, 0, dance::kMirror, 0, 1, StartAt::Side, 0, 0, 0, 0, 0, 0},
      {"nod", MoveKind::Beat, 1, 70, 100, 10, 0, 0, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 3},
      {"big", MoveKind::Beat, 2, 100, 160, 10, 0, 0, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 0},
      {"heavy", MoveKind::Beat, 2, 0, 0, 10, 0, dance::kHeavy, 0, 0, StartAt::Any, 0, 0, 0, 0, 0, 0},
      {"sway", MoveKind::Idle, 0, 0, 0, 10, 3, 0, 0, 0, StartAt::Any, 0, 0, 0, 0, 4000, 0},
      {"breathe", MoveKind::Idle, 0, 0, 0, 10, 1, 0, 0, 0, StartAt::Any, 0, 0, 0, 0, 3300, 0},
      {"look", MoveKind::Idle, 0, 0, 0, 10, 2, dance::kMirror, 0, 0, StartAt::Any, 0, 0, 0, 0, 3000, 0},
  };
  for (int i = 0; i < 13; ++i) {
    const dance::MoveLabels& l = c.move(i);
    const Want& w = want[i];
    const dance::MoveRecord r = p.moveRecord(c.packMove(i));
    ctx("view move %d (%s)", i, w.name);
    TEST_ASSERT_NOT_NULL_MESSAGE(l.name, g_ctx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(w.name, l.name, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(l.kind == w.kind, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.beats, l.beats, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.idleMs, l.idleMs, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.lo, l.paceLo, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.hi, l.paceHi, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.weight10, l.weight10, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.when, l.when, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.flags, l.flags, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.repeat, l.repeat, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, l.needs, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.travel, l.travel, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(l.startAt == w.startAt, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.from, l.fromTag, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.to, l.toTag, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.fromM, l.fromTagM, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.toM, l.toTagM, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.genres, l.genreCount, g_ctx);
    // The record agrees (the labels are the record's fields, E.1).
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.flags, l.flags, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.fromKey, l.fromTag, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.toKeyM, l.toTagM, g_ctx);
    if (l.genreCount == 0) TEST_ASSERT_NULL_MESSAGE(l.genreKeys, g_ctx);
    // startSpotMask is B.11's over the pack.
    for (int o = 0; o < 2; ++o)
      TEST_ASSERT_EQUAL_HEX8_MESSAGE(dance::startSpotMask(p, c.packMove(i), o == 1), c.startSpotMask(i, o == 1), g_ctx);
  }
  // nod's genre keys: FoldVectors' hashes for `hip hop`, `R&B` and `drum n bass`.
  const dance::MoveLabels& nod = c.move(kNod);
  TEST_ASSERT_NOT_NULL(nod.genreKeys);
  TEST_ASSERT_EQUAL_HEX32(0x5DE39FA5u, nod.genreKeys[0]);
  TEST_ASSERT_EQUAL_HEX32(0x1BDE7F5Cu, nod.genreKeys[1]);
  TEST_ASSERT_EQUAL_HEX32(0xE6318A55u, nod.genreKeys[2]);
  // The info.
  const dance::CharacterInfo& info = c.info();
  TEST_ASSERT_EQUAL_STRING("test", info.id);
  TEST_ASSERT_EQUAL_STRING("T\xC3\xABst", info.name);
  TEST_ASSERT_EQUAL_STRING(p.author(), info.author);
  TEST_ASSERT_EQUAL_STRING("IrosTheBeggar", info.author);
  TEST_ASSERT_EQUAL_STRING("GPL-3.0-or-later", info.licence);
  TEST_ASSERT_NOT_NULL(info.url);
  TEST_ASSERT_EQUAL_STRING(p.url(), info.url);
  TEST_ASSERT_TRUE(info.origin == dance::Origin::Card);
  TEST_ASSERT_TRUE(info.inCycle);
  TEST_ASSERT_EQUAL_INT(1, info.ignoredMoves);
  TEST_ASSERT_EQUAL_INT(0, dance::freezeMove(c));
  TEST_ASSERT_TRUE(dance::hasTravel(c));
  // Origin and inCycle as given.
  {
    std::unique_ptr<dance::DrawnCharacter> v(new dance::DrawnCharacter());
    TEST_ASSERT_TRUE(v->init(p, dance::Origin::BuiltIn, false));
    TEST_ASSERT_TRUE(v->info().origin == dance::Origin::BuiltIn);
    TEST_ASSERT_FALSE(v->info().inCycle);
    TEST_ASSERT_EQUAL_INT(13, v->moveCount());
  }
  // DrawnCharacter::frameAt is the free frameAt through packMove.
  for (int i = 0; i < c.moveCount(); ++i)
    for (int o = 0; o < 2; ++o)
      for (int spot = -1; spot <= 1; ++spot)
        for (int t = 0; t < 120; t += 5) {
          const dance::Placement a = c.frameAt(i, int64_t(t) * 65536 + 777, o == 1, spot, 50000);
          const dance::Placement b = dance::frameAt(p, c.packMove(i), int64_t(t) * 65536 + 777, o == 1, spot, 50000);
          ctx("view move %d, %d, spot %d, tick %d", i, o, spot, t);
          TEST_ASSERT_EQUAL_INT_MESSAGE(b.frame, a.frame, g_ctx);
          TEST_ASSERT_EQUAL_MESSAGE(b.mirrored, a.mirrored, g_ctx);
          TEST_ASSERT_EQUAL_INT_MESSAGE(b.rootX, a.rootX, g_ctx);
          TEST_ASSERT_EQUAL_INT_MESSAGE(b.rootY, a.rootY, g_ctx);
          expectRect(b.box, a.box, "box");
          expectRect(b.shadow, a.shadow, "shadow");
          expectRect(b.rect, a.rect, "rect");
        }
  // The view is a snapshot: a dropMove after init isn't seen.
  {
    CardDancer later;
    TEST_ASSERT_TRUE(later.load(packedit::testPack()));
    TEST_ASSERT_TRUE(later.pack.dropMove(kPulse, dance::Reason()));
    TEST_ASSERT_EQUAL_INT(13, later.character().moveCount());
    TEST_ASSERT_EQUAL_STRING("pulse", later.character().move(1).name);
  }
}

// ===========================================================================
// 9. The engine moves.

void test_engine_moves() {
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  const dance::PackGeom g = d.pack.geom();
  const int scale = g.scale;
  const int rest = g.restFrame & 0x7FFF;
  for (int spot = -1; spot <= 1; ++spot) {
    const int sx = kSpotX[spot + 1];
    // Auto-bounce: the rest frame, 1 beat, bounce 4 native px, no sway or key.
    ctx("auto-bounce at pos16 0, spot %d", spot);
    const dance::Placement b0 = c.frameAt(dance::kAutoBounce, 0, false, spot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(rest, b0.frame, g_ctx);
    TEST_ASSERT_FALSE_MESSAGE(b0.mirrored, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx, b0.rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(153, b0.rootY, g_ctx);
    for (int64_t pos16 = 0; pos16 < 3 * 786432; pos16 += 4099) {
      for (int32_t e16 : {39322, 52429, 65536}) {
        const int64_t phi = (pos16 % 786432) / 12;
        const int64_t y16 = -4 * refHop(phi);
        const int Y = static_cast<int>(rhu(int64_t(scale) * e16 * y16, int64_t(1) << 32));
        ctx("auto-bounce at pos16 %lld, e16 %d, spot %d", static_cast<long long>(pos16), e16, spot);
        const dance::Placement pl = c.frameAt(dance::kAutoBounce, pos16, false, spot, e16);
        TEST_ASSERT_EQUAL_INT_MESSAGE(rest, pl.frame, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(sx, pl.rootX, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(153 + Y, pl.rootY, g_ctx);
        // B.5's shadow narrows by 2 px per px of hop.
        const int sw = g.shadowW - 2 * (Y < 0 ? -Y : 0);
        if (sw <= 0) {
          TEST_ASSERT_TRUE_MESSAGE(emptyRect(pl.shadow), g_ctx);
        } else {
          TEST_ASSERT_EQUAL_INT_MESSAGE(sw, pl.shadow.x1 - pl.shadow.x0, g_ctx);
          TEST_ASSERT_EQUAL_INT_MESSAGE(sx - sw / 2, pl.shadow.x0, g_ctx);
        }
      }
    }
    // The hop's peak: phi 36700 (0.56 beat) at 4 native px, scale 4: 16 px up.
    TEST_ASSERT_EQUAL_INT(137, c.frameAt(dance::kAutoBounce, 36700 * 12, false, spot).rootY);
    // Auto-idle: the rest frame swaying +-1 native px over 4 s.
    ctx("auto-idle, spot %d", spot);
    const dance::Placement i0 = c.frameAt(dance::kAutoIdle, 0, false, spot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(rest, i0.frame, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx - scale, i0.rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(153, i0.rootY, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx, c.frameAt(dance::kAutoIdle, int64_t(100) * 65536, false, spot).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx + scale, c.frameAt(dance::kAutoIdle, int64_t(200) * 65536, false, spot).rootX,
                                  g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx, c.frameAt(dance::kAutoIdle, int64_t(300) * 65536, false, spot).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx - scale, c.frameAt(dance::kAutoIdle, int64_t(400) * 65536, false, spot).rootX,
                                  g_ctx);
    // Mirrored, the key is negated.
    TEST_ASSERT_EQUAL_INT_MESSAGE(sx + scale, c.frameAt(dance::kAutoIdle, 0, true, spot).rootX, g_ctx);
    for (int64_t ms = 0; ms <= 8000; ms += 10) {
      const int64_t pos16 = ms * 65536 / 10;
      const int64_t q = pos16 % (400 * 65536);
      const int step = q < 200 * 65536 ? 0 : 1;
      const int64_t f = (q - step * 200 * 65536) / 200;
      const int64_t kx = (step ? 1 : -1) * 65536 + (step ? -2 : 2) * refEase(4, f);
      const int X = static_cast<int>(rhu(int64_t(scale) * 65536 * kx, int64_t(1) << 32));
      ctx("auto-idle at %lld ms, spot %d", static_cast<long long>(ms), spot);
      TEST_ASSERT_EQUAL_INT_MESSAGE(sx + X, c.frameAt(dance::kAutoIdle, pos16, false, spot).rootX, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(153, c.frameAt(dance::kAutoIdle, pos16, false, spot).rootY, g_ctx);
    }
  }
  // An index that is neither a move nor an engine move: the unreadable
  // record's placement (frame 0, unmirrored, the spot's root, no box).
  for (int i : {13, 64, 1000, -1, -4, -100}) {
    ctx("view index %d", i);
    const dance::Placement pl = c.frameAt(i, 12345, true, 1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, pl.frame, g_ctx);
    TEST_ASSERT_FALSE_MESSAGE(pl.mirrored, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(256, pl.rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(153, pl.rootY, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(emptyRect(pl.box) && emptyRect(pl.shadow) && emptyRect(pl.rect), g_ctx);
  }
  // A record index past MOVS through the free frameAt: the same.
  const dance::Placement far = dance::frameAt(d.pack, 99, 0, false, -1);
  TEST_ASSERT_EQUAL_INT(64, far.rootX);
  TEST_ASSERT_TRUE(emptyRect(far.rect));
}

// Travel always completes its full distance, whatever the energy (3.6).
void test_travel_ignores_energy() {
  CardDancer& d = fixture();
  const dance::Pack& p = d.pack;
  const int L = stepSum(p, beatscript::kWalk);
  for (int32_t e16 : {39322, 47186, 65536}) {
    ctx("walk at e16 %d", e16);
    // walk has no keys and no sway: the root is the travel alone.
    const int64_t end = int64_t(L) * 65536;
    TEST_ASSERT_EQUAL_INT_MESSAGE(64, dance::frameAt(p, beatscript::kWalk, 0, false, -1, e16).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(160, dance::frameAt(p, beatscript::kWalk, end - 1, false, -1, e16).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(160, dance::frameAt(p, beatscript::kWalk, end, false, -1, e16).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(160, dance::frameAt(p, beatscript::kWalk, end, true, 1, e16).rootX, g_ctx);
    // The destination is clamped to -1..1: from +1 unmirrored it stays at +1.
    TEST_ASSERT_EQUAL_INT_MESSAGE(256, dance::frameAt(p, beatscript::kWalk, end, false, 1, e16).rootX, g_ctx);
  }
  // Half way through the go steps (2-3 of 4, 12 ticks each, linear): 48 px.
  TEST_ASSERT_EQUAL_INT(112, dance::frameAt(p, beatscript::kWalk, int64_t(24) * 65536, false, -1, 39322).rootX);
}

// ===========================================================================
// 10. BudgetVectors' traces through the free frameAt and the view.

void test_budget_vector_traces() {
  CardDancer& d = fixture();
  const dance::Pack& p = d.pack;
  const dance::DrawnCharacter& c = d.character();
  using namespace dancevec;
  TEST_ASSERT_EQUAL_INT(14, kTraceCasesCount);
  int frames = 0;
  for (int t = 0; t < kTraceCasesCount; ++t) {
    const TraceCase& tc = kTraceCases[t];
    const int view = d.viewIndex(tc.move);
    for (int n = 0; n < tc.count; ++n) {
      const TraceFrame& f = kTrace[tc.first + n];
      for (int via = 0; via < 2; ++via) {
        if (via == 1 && view < 0) continue;  // `future` isn't in the view
        ctx("trace of move %u, frame %u (pos16 %u), %s", tc.move, f.n, f.pos16, via ? "the view" : "the free frameAt");
        const dance::Placement pl = via ? c.frameAt(view, f.pos16, tc.mirrored != 0, tc.spot)
                                        : dance::frameAt(p, tc.move, f.pos16, tc.mirrored != 0, tc.spot);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(f.frame, pl.frame | (pl.mirrored ? 0x8000 : 0), g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(f.rootX, pl.rootX, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(f.rootY, pl.rootY, g_ctx);
        expectRect(rect(f.x0, f.y0, f.x1, f.y1), pl.rect, "rect");
        ++frames;
      }
    }
    TEST_ASSERT_EQUAL_INT(tc.move == beatscript::kFuture ? -1 : 0, view < 0 ? -1 : 0);
  }
  TEST_ASSERT_TRUE(frames > kTraceCount);
}

// ===========================================================================
// Size (E.1).

// Added in integration: E.1's note on DrawnCharacter's copies. A copy (the
// copy constructor or assignment) is another view of the same bytes whose
// labels' genreKeys point at its own keys, so it outlives the original.
void test_view_copies() {
  const dance::DrawnCharacter& c = fixture().character();
  std::unique_ptr<dance::DrawnCharacter> a(new dance::DrawnCharacter(c));
  std::unique_ptr<dance::DrawnCharacter> b(new dance::DrawnCharacter());
  *b = *a;
  const dance::DrawnCharacter& same = *b;
  *b = same;  // self-assignment changes nothing
  dance::DrawnCharacter* views[2] = {a.get(), b.get()};
  for (dance::DrawnCharacter* v : views) {
    ctx("a copy of the fixture's view");
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.moveCount(), v->moveCount(), g_ctx);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(c.pack().name(), v->pack().name(), g_ctx);  // the same bytes
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.info().id, v->info().id, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.info().ignoredMoves, v->info().ignoredMoves, g_ctx);
    for (int i = 0; i < c.moveCount(); ++i) {
      const dance::MoveLabels& w = c.move(i);
      const dance::MoveLabels& l = v->move(i);
      ctx("a copy's move %d", i);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(w.name, l.name, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(c.packMove(i), v->packMove(i), g_ctx);
      for (int o = 0; o < 2; ++o)
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(c.startSpotMask(i, o == 1), v->startSpotMask(i, o == 1), g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(w.genreCount, l.genreCount, g_ctx);
      if (l.genreCount == 0) {
        TEST_ASSERT_NULL_MESSAGE(l.genreKeys, g_ctx);
        continue;
      }
      const char* lo = reinterpret_cast<const char*>(v);
      const char* at = reinterpret_cast<const char*>(l.genreKeys);
      TEST_ASSERT_TRUE_MESSAGE(at >= lo && at < lo + sizeof(dance::DrawnCharacter), g_ctx);  // its own keys
      for (int g = 0; g < l.genreCount; ++g) TEST_ASSERT_EQUAL_HEX32_MESSAGE(w.genreKeys[g], l.genreKeys[g], g_ctx);
    }
  }
  // The original gone (heap-freed: ASan would report a key read through it).
  a.reset();
  const dance::MoveLabels& nod = b->move(beatscript::kNod);
  TEST_ASSERT_EQUAL_INT(3, nod.genreCount);
  TEST_ASSERT_EQUAL_HEX32(0x5DE39FA5u, nod.genreKeys[0]);
  TEST_ASSERT_EQUAL_HEX32(0x1BDE7F5Cu, nod.genreKeys[1]);
  TEST_ASSERT_EQUAL_HEX32(0xE6318A55u, nod.genreKeys[2]);
}

// ===========================================================================
// 12. Added in review: rules the fixture's data didn't pin.

// A.13 and B.4 step 8: a travel move with no `go` step travels during every
// step (the compiler's default); flagging every step changes nothing; with
// `go` on the second step only, the root waits through the first.
void test_travel_without_go() {
  const auto build = [](bool go1, bool go2) {
    beatscript::MiniMove m;
    m.name = "t";
    m.beats = 2;
    m.travel = 1;
    m.travelEase = 1;  // linear
    m.steps = {{0, false, 12, 0, 0, 0, go1}, {0, false, 12, 0, 0, 0, go2}};
    return beatscript::miniPack({2, 8, 8, 4, 8, 10}, {{2, 2, 4, 6}}, {m});
  };
  OpenPack none, all, second;
  TEST_ASSERT_TRUE(none.open(build(false, false)));
  TEST_ASSERT_TRUE(all.open(build(true, true)));
  TEST_ASSERT_TRUE(second.open(build(false, true)));
  const int noGo[5] = {64, 88, 112, 136, 160}, goSecond[5] = {64, 64, 64, 112, 160};
  for (int k = 0; k < 5; ++k) {
    const int64_t pos16 = int64_t(6 * k) * 65536;
    ctx("tick %d", 6 * k);
    TEST_ASSERT_EQUAL_INT_MESSAGE(noGo[k], dance::frameAt(none.pack, 0, pos16, false, -1).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(256 - (noGo[k] - 64), dance::frameAt(none.pack, 0, pos16, true, 1).rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(goSecond[k], dance::frameAt(second.pack, 0, pos16, false, -1).rootX, g_ctx);
  }
  for (int64_t pos16 = 0; pos16 <= 30 * 65536; pos16 += 4097)
    for (int o = 0; o < 2; ++o) {
      ctx("no go flag against every step flagged, pos16 %lld", static_cast<long long>(pos16));
      TEST_ASSERT_EQUAL_INT_MESSAGE(dance::frameAt(all.pack, 0, pos16, o == 1, o ? 1 : -1).rootX,
                                    dance::frameAt(none.pack, 0, pos16, o == 1, o ? 1 : -1).rootX, g_ctx);
    }
}

// B.4 steps 11-12 at the stage's edges: the shadow is clipped to x 0-320 at
// spots -1 and +1 (B.5), and a frame lifted above the stage's top is pushed
// down, its root with it.
void test_edge_clamps() {
  {
    beatscript::MiniMove m;
    m.name = "w";
    m.beats = 1;
    m.steps = {{0, false, 12, 0, 0, 0, false}};
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(beatscript::miniPack({1, 8, 8, 4, 8, 160}, {{2, 2, 4, 6}}, {m})));
    ctx("a 160 px shadow at spot -1");
    const dance::Placement l = dance::frameAt(op.pack, 0, 0, false, -1);
    TEST_ASSERT_EQUAL_INT_MESSAGE(64, l.rootX, g_ctx);
    expectRect(rect(62, 147, 66, 153), l.box, "box");
    expectRect(rect(0, 152, 144, 155), l.shadow, "shadow");  // x0 64 - 80 = -16, clipped
    expectRect(rect(0, 147, 144, 155), l.rect, "rect");
    ctx("a 160 px shadow at spot +1");
    const dance::Placement r = dance::frameAt(op.pack, 0, 0, false, 1);
    expectRect(rect(176, 152, 320, 155), r.shadow, "shadow");  // x1 336, clipped
    ctx("a 160 px shadow at spot 0");
    expectRect(rect(80, 152, 240, 155), dance::frameAt(op.pack, 0, 0, true, 0).shadow, "shadow");
  }
  {
    beatscript::MiniMove up;
    up.name = "up";
    up.beats = 1;
    up.steps = {{0, false, 12, 0, -40, 0, false}};
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(beatscript::miniPack({1, 8, 120, 4, 120, 10}, {{2, 0, 4, 120}}, {up})));
    ctx("a 120-row frame keyed 40 px up");
    const dance::Placement p = dance::frameAt(op.pack, 0, 0, false, 0);
    // Y = -40: root 113, top -7, so pushed 7 down: root 120, box from row 0.
    TEST_ASSERT_EQUAL_INT_MESSAGE(160, p.rootX, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(120, p.rootY, g_ctx);
    expectRect(rect(158, 0, 162, 120), p.box, "box");
    TEST_ASSERT_TRUE_MESSAGE(emptyRect(p.shadow), g_ctx);  // 10 - 2 * 40 px of hop
  }
}

// The view beyond the fixture's data: more than 13 kept moves (up to 64), a
// mirrored `from` key unlike the unmirrored one, and a rest frame stored
// with bit 15 (the engine moves draw it mirrored).
void test_view_beyond_the_fixture() {
  for (int count : {20, 64}) {
    std::vector<beatscript::MiniMove> moves;
    for (int i = 0; i < count; ++i) {
      beatscript::MiniMove m;
      char name[8];
      std::snprintf(name, sizeof name, "m%02d", i);
      m.name = name;
      m.beats = 1;
      m.steps = {{0, false, 12, 0, 0, 0, false}};
      moves.push_back(m);
    }
    ctx("a view of %d moves", count);
    OpenPack op;
    TEST_ASSERT_TRUE_MESSAGE(op.open(beatscript::miniPack({2, 8, 8, 4, 8, 10}, {{2, 2, 4, 6}}, moves)), g_ctx);
    std::unique_ptr<dance::DrawnCharacter> v(new dance::DrawnCharacter());
    TEST_ASSERT_TRUE_MESSAGE(v->init(op.pack, dance::Origin::Card, true), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(count, v->moveCount(), g_ctx);
    for (int i = 0; i < count; ++i) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(i, v->packMove(i), g_ctx);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(moves[i].name.c_str(), v->move(i).name, g_ctx);
    }
  }
  {
    // point's stored keys 1 2 1 3, with fromKeyM patched to 2.
    Bytes t = packedit::testPack();
    const uint32_t rec = packedit::entry(t, "MOVS").off + 4 + 32 * beatscript::kPoint;
    TEST_ASSERT_EQUAL_UINT32(1, packedit::get8(t, rec + 18));
    packedit::put8(t, rec + 18, 2);
    packedit::fixCrc(t);
    CardDancer d;
    TEST_ASSERT_TRUE(d.load(t));
    const dance::MoveLabels& l = d.character().move(beatscript::kPoint);
    TEST_ASSERT_EQUAL_INT(1, l.fromTag);
    TEST_ASSERT_EQUAL_INT(2, l.fromTagM);
    TEST_ASSERT_EQUAL_INT(2, l.toTag);
    TEST_ASSERT_EQUAL_INT(3, l.toTagM);
  }
  {
    // GEOM's rest frame with bit 15 (A.6 allows it; the compiler writes 0).
    Bytes t = packedit::testPack();
    const uint32_t geom = packedit::entry(t, "GEOM").off;
    const uint32_t rest = packedit::get16(t, geom + 10);
    TEST_ASSERT_EQUAL_UINT32(0, rest & 0x8000u);
    packedit::put16(t, geom + 10, rest | 0x8000u);
    packedit::fixCrc(t);
    CardDancer d;
    TEST_ASSERT_TRUE(d.load(t));
    const dance::DrawnCharacter& c = d.character();
    for (int o = 0; o < 2; ++o) {
      ctx("the engine moves on a mirrored rest frame, orientation %d", o);
      const dance::Placement b = c.frameAt(dance::kAutoBounce, 0, o == 1, 0);
      const dance::Placement i = c.frameAt(dance::kAutoIdle, 0, o == 1, 0);
      TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(rest), b.frame, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(o == 0, b.mirrored, g_ctx);  // the step's bit 15 XOR the orientation
      TEST_ASSERT_EQUAL_MESSAGE(o == 0, i.mirrored, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(160 + (o ? 4 : -4), i.rootX, g_ctx);  // the key's sign: the orientation's
    }
  }
}

// COST-2: readMoveSteps decodes the record once (Pack::steps); its steps,
// starts and travelling sums equal step-by-step reads, on every record of
// the fixture and the hand-built packs.
void test_move_steps_read_once() {
  std::vector<Bytes> packs = {packedit::testPack(), beatscript::b4Pack(), beatscript::clampPack(),
                              beatscript::wrapPack()};
  for (size_t k = 0; k < packs.size(); ++k) {
    OpenPack op;
    TEST_ASSERT_TRUE(op.open(packs[k]));
    const dance::Pack& p = op.pack;
    for (int m = -1; m <= p.moveCount(); ++m) {
      ctx("pack %u, move %d", static_cast<unsigned>(k), m);
      dance::MoveSteps s;
      const bool ok = dance::readMoveSteps(p, m, &s);
      const dance::MoveRecord r = p.moveRecord(m);
      TEST_ASSERT_EQUAL_MESSAGE(m >= 0 && m < p.moveCount(), ok, g_ctx);
      if (!ok) continue;
      TEST_ASSERT_EQUAL_INT_MESSAGE(r.stepCount, s.count, g_ctx);
      int32_t total = 0, go = 0;
      bool anyGo = false;
      for (int i = 0; i < r.stepCount; ++i) anyGo = anyGo || p.step(m, i).go;
      for (int i = 0; i < r.stepCount; ++i) {
        const dance::StepRecord st = p.step(m, i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st.frame, s.step[i].frame, g_ctx);
        TEST_ASSERT_EQUAL_MESSAGE(st.mirrored, s.step[i].mirrored, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st.dur, s.step[i].dur, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st.dx, s.step[i].dx, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st.dy, s.step[i].dy, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(st.ease, s.step[i].ease, g_ctx);
        TEST_ASSERT_EQUAL_MESSAGE(st.go, s.step[i].go, g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(total, s.start[i], g_ctx);
        TEST_ASSERT_EQUAL_INT_MESSAGE(go, s.goStart[i], g_ctx);
        total += st.dur;
        if (!anyGo || st.go) go += st.dur;
      }
      TEST_ASSERT_EQUAL_INT_MESSAGE(total, s.total, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(go, s.goTotal, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(anyGo, s.anyGo, g_ctx);
    }
  }
}

void test_size() {
  char msg[80];
  std::snprintf(msg, sizeof msg, "sizeof(DrawnCharacter) = %u", static_cast<unsigned>(sizeof(dance::DrawnCharacter)));
  TEST_MESSAGE(msg);
  TEST_ASSERT_TRUE_MESSAGE(sizeof(dance::DrawnCharacter) <= 6144, msg);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_eases_hop_sway);
  RUN_TEST(test_b4_worked_example);
  RUN_TEST(test_clamp_example);
  RUN_TEST(test_step_lookup);
  RUN_TEST(test_mirror_symmetry);
  RUN_TEST(test_bounds_hold_every_pixel);
  RUN_TEST(test_stage_x);
  RUN_TEST(test_extent_fits_start_spots);
  RUN_TEST(test_view_of_the_fixture);
  RUN_TEST(test_engine_moves);
  RUN_TEST(test_travel_ignores_energy);
  RUN_TEST(test_budget_vector_traces);
  RUN_TEST(test_view_copies);
  RUN_TEST(test_travel_without_go);
  RUN_TEST(test_edge_clamps);
  RUN_TEST(test_view_beyond_the_fixture);
  RUN_TEST(test_move_steps_read_once);
  RUN_TEST(test_size);
  return UNITY_END();
}
