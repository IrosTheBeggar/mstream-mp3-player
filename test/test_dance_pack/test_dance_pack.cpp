// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the dance pack reader (DancePack: Pack::open, Pack::scan,
// the reasons, the region views and the accessors) and its pieces (Crc32,
// Fnv1a, GenreFold, DanceRules' reach): docs/DANCE-SPRITES.md, Appendices
// A, B.7, B.8 and D.4, which this file follows item by item. It was written
// from the doc alone, without reading the implementation.
//
// - Every pack and scan buffer handed to the reader is an exact-size heap
//   copy (packedit::HeapCopy), so AddressSanitizer reports a read past the
//   bytes given; the CRCs the tests write come from the doc's own
//   definition (packedit::refCrc32), not from dance::crc32.
// - After every open() and scan() the tests assert regionFaults() == 0 and
//   item 11's properties (verify()).
// - The fuzz (truncations, flips, the structure-aware mutator) is seeded,
//   so a failure names a case that repeats.
// - ReaderVectors.h (D.3) is read in full against step 2's drop lists, and
//   each opened case again against its full list (E.4): DanceBudget's
//   start-spot hook drops `travels from no spot` inside open() (through a
//   counting wrapper), then a BudgetPass run to its end drops `budget red`
//   through dropMove after it, as 5.7's loader does for a card pack. Item 4's
//   stub hook stays, for the hook's own contract.
//
// Run: pio test -e native -f test_dance_pack; under ASan and UBSan with
// pio test -e native_asan (CI) or Appendix D.5's Docker command.
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../support/dance/FoldVectors.h"
#include "../support/dance/PackEdit.h"
#include "Crc32.h"
#include "DanceBlit.h"
#include "DanceBudget.h"
#include "DancePack.h"
#include "DanceRules.h"
#include "Fnv1a.h"
#include "GenreFold.h"

using dance::Why;
using packedit::Bytes;
using packedit::HeapCopy;
using packedit::Xorshift32;

namespace {

// --- context for failure messages --------------------------------------------

char g_ctx[256] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

// --- one reader, its exact-size buffer and its report --------------------------

struct Reader {
  HeapCopy buf;
  dance::Pack pack;
  dance::Report report;
  packedit::Drops drops;
  bool ok = false;
  bool scanned = false;
  uint32_t fileSize = 0;
  char reason[dance::kReasonChars] = "";
  size_t reasonLen = 0;

  // open(copy, size = b.size(), avail): the copy holds min(avail, size) bytes.
  bool open(const Bytes& b, uint32_t avail, const dance::OpenOptions* options = nullptr) {
    const uint32_t size = static_cast<uint32_t>(b.size());
    buf.assign(b, avail < size ? avail : size);
    drops.attach(report);
    ok = pack.open(buf.data(), size, avail, &report, options);
    scanned = false;
    fileSize = size;
    finish();
    return ok;
  }
  bool openAll(const Bytes& b, const dance::OpenOptions* options = nullptr) {
    return open(b, static_cast<uint32_t>(b.size()), options);
  }
  // scan(copy, avail, size): the copy holds b's first min(avail, b.size()) bytes.
  bool scan(const Bytes& b, uint32_t avail, uint32_t size) {
    buf.assign(b, avail);
    drops.attach(report);
    ok = pack.scan(buf.data(), avail, size, &report);
    scanned = true;
    fileSize = size;
    finish();
    return ok;
  }
  // open() and scan() as above, with the bytes placed `shift` bytes into an
  // exact-size block, so the pack starts at an odd address (D.1: a pack can
  // start at any address; every field is read a byte at a time).
  bool openAt(const Bytes& b, size_t shift) {
    const uint32_t size = static_cast<uint32_t>(b.size());
    drops.attach(report);
    ok = pack.open(place(b, size, shift), size, size, &report);
    scanned = false;
    fileSize = size;
    finish();
    return ok;
  }
  bool scanAt(const Bytes& b, uint32_t avail, uint32_t size, size_t shift) {
    drops.attach(report);
    ok = pack.scan(place(b, avail < b.size() ? avail : b.size(), shift), avail, size, &report);
    scanned = true;
    fileSize = size;
    finish();
    return ok;
  }
  void finish() {
    std::memset(reason, 0, sizeof reason);
    reasonLen = dance::formatReason(report.rejected, reason, sizeof reason);
  }

 private:
  std::unique_ptr<uint8_t[]> block;  // openAt's and scanAt's: shift filler bytes, then the copy
  const uint8_t* place(const Bytes& b, size_t n, size_t shift) {
    block.reset(new uint8_t[shift + n]);
    std::memset(block.get(), 0xA5, shift);
    if (n) std::memcpy(block.get() + shift, b.data(), n);
    return block.get() + shift;
  }
};

const dance::OpenOptions* skipCrc() {
  static dance::OpenOptions o;
  o.flags = dance::kSkipCrc;
  return &o;
}

// --- zeroed results ------------------------------------------------------------

bool zeroGeom(const dance::PackGeom& g) {
  return !g.scale && !g.faces && !g.cellW && !g.cellH && !g.anchorX && !g.anchorY && !g.shadowW && !g.frameCount &&
         !g.restFrame;
}
bool zeroBox(const dance::FrameBox& b) { return !b.x0 && !b.y0 && !b.w && !b.h; }
bool zeroFrame(const dance::PackFrame& f) { return zeroBox(f.box) && f.data == nullptr && f.len == 0; }
bool zeroStep(const dance::StepRecord& s) {
  return !s.frame && !s.mirrored && !s.dur && !s.dx && !s.dy && !s.ease && !s.go;
}
bool sameStep(const dance::StepRecord& a, const dance::StepRecord& b) {
  return a.frame == b.frame && a.mirrored == b.mirrored && a.dur == b.dur && a.dx == b.dx && a.dy == b.dy &&
         a.ease == b.ease && a.go == b.go;
}
bool sameMove(const dance::MoveRecord& a, const dance::MoveRecord& b) {
  return a.name == b.name && a.kind == b.kind && a.beats == b.beats && a.paceLo == b.paceLo && a.paceHi == b.paceHi &&
         a.weight10 == b.weight10 && a.when == b.when && a.flags == b.flags && a.repeat == b.repeat &&
         a.needs == b.needs && a.travel == b.travel && a.startAt == b.startAt && a.travelEase == b.travelEase &&
         a.bounce == b.bounce && a.sway == b.sway && a.fromKey == b.fromKey && a.toKey == b.toKey &&
         a.fromKeyM == b.fromKeyM && a.toKeyM == b.toKeyM && a.genreFirst == b.genreFirst &&
         a.genreCount == b.genreCount && a.stepCount == b.stepCount && a.stepFirst == b.stepFirst &&
         a.songLo == b.songLo && a.songHi == b.songHi;
}
bool zeroMove(const dance::MoveRecord& m) {
  return !m.name && !m.kind && !m.beats && !m.paceLo && !m.paceHi && !m.weight10 && !m.when && !m.flags && !m.repeat &&
         !m.needs && !m.travel && !m.startAt && !m.travelEase && !m.bounce && !m.sway && !m.fromKey && !m.toKey &&
         !m.fromKeyM && !m.toKeyM && !m.genreFirst && !m.genreCount && !m.stepCount && !m.stepFirst && !m.songLo &&
         !m.songHi;
}

const int kOddIndices[] = {-1, INT_MIN, INT_MAX, 256, 1000};

// The State Open accessors, out of that state: 0, null, false, zeroed.
void expectNoOpenAccessors(const dance::Pack& p) {
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.frameCount(), g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.poseKeyCount(), g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.moveCount(), g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.keptMoves(), g_ctx);
  for (int i : {-1, 0, 1, 63, 64, 255, 256, INT_MAX, INT_MIN}) {
    TEST_ASSERT_TRUE_MESSAGE(zeroBox(p.box(i)), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroFrame(p.frame(i)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.poseKey(i), g_ctx);
    TEST_ASSERT_FALSE_MESSAGE(p.moveKept(i), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroMove(p.moveRecord(i)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.moveName(i), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(i, 0)), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(0, i)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.genreTag(i, 0), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.genreTag(0, i), g_ctx);
  }
}

// State None: every accessor null or 0 (D.1).
void expectNoneState(const dance::Pack& p) {
  TEST_ASSERT_TRUE_MESSAGE(p.state() == dance::Pack::State::None, g_ctx);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.size(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.id(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.name(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.author(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.licence(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.url(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.version(), g_ctx);
  TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, p.declaredMoves(), g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(zeroGeom(p.geom()), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.palette(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.idlePalette(), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.thumb(), g_ctx);
  expectNoOpenAccessors(p);
}

// A.5's strings as the accessors give them: NUL-terminated, valid, in limits.
void expectMetaStrings(const dance::Pack& p) {
  const char* s[6] = {p.id(), p.name(), p.author(), p.licence(), p.url(), p.version()};
  const size_t lo[6] = {1, 1, 1, 1, 0, 0};
  const size_t hi[6] = {23, 24, 40, 40, 80, 16};
  for (int i = 0; i < 6; ++i) {
    TEST_ASSERT_NOT_NULL_MESSAGE(s[i], g_ctx);
    const size_t n = std::strlen(s[i]);
    TEST_ASSERT_TRUE_MESSAGE(n >= lo[i] && n <= hi[i], g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(dance::validText(s[i], n), g_ctx);
  }
  for (const char* c = p.id(); *c; ++c)
    TEST_ASSERT_TRUE_MESSAGE((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || (*c == '-' && c != p.id()),
                             g_ctx);
  const dance::PackGeom g = p.geom();
  TEST_ASSERT_TRUE_MESSAGE(g.scale >= 1 && g.scale <= 4, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.faces <= 2, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.cellW >= 1 && g.cellW * g.scale <= 160, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.cellH >= 1 && g.cellH * g.scale <= 160, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.anchorX < g.cellW, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.anchorY >= 1 && g.anchorY <= g.cellH, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.anchorY * g.scale <= 153 && (g.cellH - g.anchorY) * g.scale <= 10, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.shadowW <= 160, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(g.frameCount >= 1 && g.frameCount <= 256, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE((g.restFrame & 0x7FFF) < g.frameCount, g_ctx);
  TEST_ASSERT_NOT_NULL_MESSAGE(p.palette(), g_ctx);
  TEST_ASSERT_NOT_NULL_MESSAGE(p.idlePalette(), g_ctx);
  TEST_ASSERT_NOT_NULL_MESSAGE(p.thumb(), g_ctx);
}

std::vector<uint8_t> gridOf(const dance::Pack& p, int frame, bool* ok = nullptr) {
  const dance::PackGeom g = p.geom();
  std::vector<uint8_t> grid(static_cast<size_t>(g.cellW) * g.cellH, 0xEE);
  const bool good = dance::decodeGrid(p.frame(frame), g.cellW, g.cellH, grid.data());
  if (ok) *ok = good;
  return grid;
}

// One blit of f at a seeded scale, orientation and position into a 40x50
// canvas with a canary border: the border stays intact and only the box's
// rect changes.
void blitIntoCanary(const dance::PackFrame& f, Xorshift32& rng) {
  uint16_t lut[16];
  for (int k = 0; k < 16; ++k) lut[k] = static_cast<uint16_t>(0x7700 | k);
  packedit::CanaryCanvas cc(40, 50, 4);
  const int s = 1 + static_cast<int>(rng.below(4));
  const bool mirrored = rng.below(2) != 0;
  const int bw = f.box.w * s, bh = f.box.h * s;
  const int left = -bw - 2 + static_cast<int>(rng.below(static_cast<uint32_t>(bw + 44)));
  const int top = -bh - 2 + static_cast<int>(rng.below(static_cast<uint32_t>(bh + 54)));
  dance::blit(cc.canvas(), f, left, top, s, mirrored, lut);
  TEST_ASSERT_TRUE_MESSAGE(cc.borderIntact(), g_ctx);
  for (int y = 0; y < cc.h; ++y)
    for (int x = 0; x < cc.w; ++x)
      if (cc.at(x, y) != 0x1234)
        TEST_ASSERT_TRUE_MESSAGE(x >= left && x < left + bw && y >= top && y < top + bh, g_ctx);
}

// Item 11: what holds after every open() and scan().
void verify(Reader& r, Xorshift32* rng) {
  const dance::Pack& p = r.pack;
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, p.regionFaults(), g_ctx);
  TEST_ASSERT_FALSE_MESSAGE(r.drops.outOfOrder, g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(r.report.dropped, r.drops.calls, g_ctx);
  if (!r.ok) {
    TEST_ASSERT_TRUE_MESSAGE(r.report.rejected.why != Why::None, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(r.reasonLen > 0 && r.reasonLen < dance::kReasonChars, g_ctx);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(r.reasonLen, std::strlen(r.reason), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.report.dropped, g_ctx);
    expectNoneState(p);
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(r.report.rejected.why == Why::None, g_ctx);
  TEST_ASSERT_EQUAL_size_t_MESSAGE(0, r.reasonLen, g_ctx);
  TEST_ASSERT_EQUAL_UINT32_MESSAGE(r.fileSize, p.size(), g_ctx);
  expectMetaStrings(p);
  if (r.scanned) {
    TEST_ASSERT_TRUE_MESSAGE(p.state() == dance::Pack::State::Scanned, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.report.dropped, g_ctx);
    expectNoOpenAccessors(p);
    return;
  }
  TEST_ASSERT_TRUE_MESSAGE(p.state() == dance::Pack::State::Open, g_ctx);
  const dance::PackGeom g = p.geom();
  const int frames = p.frameCount();
  TEST_ASSERT_EQUAL_INT_MESSAGE(g.frameCount, frames, g_ctx);

  // Frames: boxes in their cells, the dry run passing, a quarter blitted.
  for (int i = 0; i < frames; ++i) {
    const dance::FrameBox b = p.box(i);
    const dance::PackFrame f = p.frame(i);
    TEST_ASSERT_TRUE_MESSAGE(b.x0 == f.box.x0 && b.y0 == f.box.y0 && b.w == f.box.w && b.h == f.box.h, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(b.x0 + b.w <= g.cellW && b.y0 + b.h <= g.cellH, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE((b.w == 0) == (b.h == 0) && (b.w == 0) == (f.len == 0), g_ctx);
    if (f.len) TEST_ASSERT_NOT_NULL_MESSAGE(f.data, g_ctx);
    bool good = false;
    gridOf(p, i, &good);
    TEST_ASSERT_TRUE_MESSAGE(good, g_ctx);
    if (rng && rng->below(4) == 0) blitIntoCanary(f, *rng);
  }
  if (frames > 0) TEST_ASSERT_TRUE_MESSAGE(p.box(g.restFrame & 0x7FFF).w > 0, g_ctx);

  // Pose keys 1..P.
  const int P = p.poseKeyCount();
  TEST_ASSERT_TRUE_MESSAGE(P >= 0 && P <= 255, g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.poseKey(0), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.poseKey(P + 1), g_ctx);
  for (int id = 1; id <= P; ++id) {
    const char* k = p.poseKey(id);
    TEST_ASSERT_NOT_NULL_MESSAGE(k, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(k) >= 1 && std::strlen(k) <= 23, g_ctx);
  }

  // Moves: the kept ones whole, every one readable whatever the arguments.
  const int moves = p.moveCount();
  TEST_ASSERT_TRUE_MESSAGE(moves >= 0 && moves <= 64, g_ctx);
  TEST_ASSERT_EQUAL_INT_MESSAGE(moves - r.report.dropped, p.keptMoves(), g_ctx);
  int notKept = 0;
  for (int m = 0; m < moves; ++m) {
    const dance::MoveRecord rec = p.moveRecord(m);
    const char* name = p.moveName(m);
    for (int k = 0; k < 34; ++k) (void)p.step(m, k);
    for (int t = 0; t < 10; ++t) (void)p.genreTag(m, t);
    // steps(): every step at once, each as step() reads it, the record too.
    {
      dance::StepRecord all[34];
      dance::MoveRecord got;
      const int n = p.steps(m, all, 34, &got);
      TEST_ASSERT_EQUAL_INT_MESSAGE(rec.stepCount < 34 ? rec.stepCount : 34, n, g_ctx);  // a dropped record may have more
      TEST_ASSERT_TRUE_MESSAGE(sameMove(got, rec), g_ctx);
      for (int k = 0; k < n; ++k) TEST_ASSERT_TRUE_MESSAGE(sameStep(all[k], p.step(m, k)), g_ctx);
      dance::StepRecord few[3];
      const int n3 = p.steps(m, few, 3);
      TEST_ASSERT_EQUAL_INT_MESSAGE(rec.stepCount < 3 ? rec.stepCount : 3, n3, g_ctx);
      for (int k = 0; k < n3; ++k) TEST_ASSERT_TRUE_MESSAGE(sameStep(few[k], p.step(m, k)), g_ctx);
      dance::MoveRecord only;
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.steps(m, nullptr, 34, &only), g_ctx);
      TEST_ASSERT_TRUE_MESSAGE(sameMove(only, rec), g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.steps(m, all, 0), g_ctx);
    }
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(m, -1)), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(m, rec.stepCount)), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(m, INT_MAX)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.genreTag(m, -1), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.genreTag(m, rec.genreCount), g_ctx);
    if (!p.moveKept(m)) {
      ++notKept;
      // D.1: moveName is null exactly when the name fails `bad name`, for
      // any record: a move dropped by a later row has passed it.
      const char* text = nullptr;
      for (int k = 0; k < r.drops.calls && k < packedit::Drops::kMax; ++k)
        if (r.drops.move[k] == m) text = r.drops.text[k];
      TEST_ASSERT_NOT_NULL_MESSAGE(text, g_ctx);
      if (std::strcmp(text, "bad name") == 0) {
        TEST_ASSERT_NULL_MESSAGE(name, g_ctx);
      } else {
        TEST_ASSERT_NOT_NULL_MESSAGE(name, g_ctx);
        TEST_ASSERT_TRUE_MESSAGE(std::strlen(name) >= 1 && std::strlen(name) <= 23, g_ctx);
      }
      continue;
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(name, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(std::strlen(name) >= 1 && std::strlen(name) <= 23, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(rec.kind <= 1 && rec.needs == 0, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(rec.stepCount >= 1 && rec.stepCount <= 32, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(rec.fromKey <= P && rec.toKey <= P && rec.fromKeyM <= P && rec.toKeyM <= P, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(rec.genreCount <= 8, g_ctx);
    for (int k = 0; k < rec.stepCount; ++k) {
      const dance::StepRecord s = p.step(m, k);
      TEST_ASSERT_TRUE_MESSAGE(s.frame < frames, g_ctx);
      TEST_ASSERT_TRUE_MESSAGE(s.dur > 0 && s.ease <= 4, g_ctx);
    }
    for (int t = 0; t < rec.genreCount; ++t) {
      const char* tag = p.genreTag(m, t);
      TEST_ASSERT_NOT_NULL_MESSAGE(tag, g_ctx);
      uint32_t key = 0;
      TEST_ASSERT_TRUE_MESSAGE(dance::genreKey(tag, &key), g_ctx);
    }
  }
  TEST_ASSERT_EQUAL_INT_MESSAGE(r.report.dropped, notKept, g_ctx);

  // Out-of-range arguments: zeroed, null or false.
  for (int i : kOddIndices) {
    TEST_ASSERT_TRUE_MESSAGE(zeroBox(p.box(i)), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroFrame(p.frame(i)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.poseKey(i == 256 ? P + 1 : i), g_ctx);
    TEST_ASSERT_FALSE_MESSAGE(p.moveKept(i), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroMove(p.moveRecord(i)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.moveName(i), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(i, 0)), g_ctx);
    TEST_ASSERT_NULL_MESSAGE(p.genreTag(i, 0), g_ctx);
    dance::StepRecord none[2];
    dance::MoveRecord noRec;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, p.steps(i, none, 2, &noRec), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(zeroMove(noRec), g_ctx);
  }
  TEST_ASSERT_TRUE_MESSAGE(zeroBox(p.box(frames)), g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(zeroFrame(p.frame(frames)), g_ctx);
  TEST_ASSERT_FALSE_MESSAGE(p.moveKept(moves), g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(zeroMove(p.moveRecord(moves)), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.moveName(moves), g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(zeroStep(p.step(moves, 0)), g_ctx);
  TEST_ASSERT_NULL_MESSAGE(p.genreTag(moves, 0), g_ctx);
}

// --- D.3's digest ----------------------------------------------------------------

struct Digest {
  uint32_t h = 0x811C9DC5u;
  void bytes(const void* p, size_t n) { h = packedit::refFnv1a32(p, n, h); }
  void u8(uint32_t v) {
    const uint8_t b = static_cast<uint8_t>(v);
    bytes(&b, 1);
  }
  void u16(uint32_t v) {
    const uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    bytes(b, 2);
  }
  void u32(uint32_t v) {
    const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 24)};
    bytes(b, 4);
  }
  void str(const char* s) {
    const size_t n = s ? std::strlen(s) : 0;
    u8(static_cast<uint32_t>(n));
    if (n) bytes(s, n);
  }
};

uint32_t frameHash(const dance::Pack& p, int i) {
  const std::vector<uint8_t> grid = gridOf(p, i);
  return packedit::refFnv1a32(grid.data(), grid.size());
}

// D.3's table: META, GEOM, PALS and THMB; then, for an opened pack, the
// frames, the pose keys and the moves that `listed` (the case's step-2 drop
// list) doesn't name.
uint32_t digestOf(const dance::Pack& p, bool scanned, const std::vector<bool>& listed) {
  Digest d;
  d.str(p.id());
  d.str(p.name());
  d.str(p.author());
  d.str(p.licence());
  d.str(p.url());
  d.str(p.version());
  d.u8(p.declaredMoves());
  const dance::PackGeom g = p.geom();
  for (uint32_t v : {g.scale, g.faces, g.cellW, g.cellH, g.anchorX, g.anchorY, g.shadowW}) d.u8(v);
  d.u16(g.frameCount);
  d.u16(g.restFrame);
  d.bytes(p.palette(), 48);
  d.bytes(p.idlePalette(), 48);
  d.bytes(p.thumb(), 800);
  if (scanned) return d.h;
  for (int i = 0; i < p.frameCount(); ++i) {
    const dance::FrameBox b = p.box(i);
    for (uint32_t v : {b.x0, b.y0, b.w, b.h}) d.u8(v);
    d.u32(frameHash(p, i));
  }
  const int P = p.poseKeyCount();
  d.u8(static_cast<uint32_t>(P));
  for (int id = 1; id <= P; ++id) d.str(p.poseKey(id));
  d.u8(static_cast<uint32_t>(p.moveCount()));
  for (int m = 0; m < p.moveCount(); ++m) {
    if (m < static_cast<int>(listed.size()) && listed[m]) continue;
    const dance::MoveRecord r = p.moveRecord(m);
    d.u8(static_cast<uint32_t>(m));
    d.str(p.moveName(m));
    for (uint32_t v : {r.kind, r.beats, r.paceLo, r.paceHi, r.weight10, r.when, r.flags, r.repeat, r.needs})
      d.u8(v);
    d.u8(static_cast<uint8_t>(r.travel));
    for (uint32_t v : {r.startAt, r.travelEase, r.bounce, r.sway}) d.u8(v);
    for (uint32_t id : {r.fromKey, r.toKey, r.fromKeyM, r.toKeyM}) d.str(id ? p.poseKey(static_cast<int>(id)) : nullptr);
    d.u8(r.genreCount);
    for (int t = 0; t < r.genreCount; ++t) {
      uint32_t key = 0;
      dance::genreKey(p.genreTag(m, t), &key);
      d.u32(key);
    }
    d.u8(r.songLo);
    d.u8(r.songHi);
    d.u8(r.stepCount);
    for (int k = 0; k < r.stepCount; ++k) {
      const dance::StepRecord s = p.step(m, k);
      d.u16(s.frame | (s.mirrored ? 0x8000u : 0u));
      d.u16(s.dur);
      d.u8(static_cast<uint8_t>(s.dx));
      d.u8(static_cast<uint8_t>(s.dy));
      d.u8(s.ease);
      d.u8(s.go ? 1 : 0);
    }
  }
  return d.h;
}

// --- hand-built cases ---------------------------------------------------------------

Xorshift32 g_rng(0x50414B53u);

void expectRejected(const char* want, const Bytes& data, const dance::OpenOptions* o = nullptr) {
  Reader r;
  ctx("expected \"%s\" (%u bytes)", want, static_cast<unsigned>(data.size()));
  TEST_ASSERT_FALSE_MESSAGE(r.openAll(data, o), g_ctx);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want, r.reason, g_ctx);
  verify(r, &g_rng);
}

// `want`: the drops as "move:text" joined with "|"; "" for none.
void expectDrops(const char* want, const Bytes& data, const dance::OpenOptions* o = nullptr) {
  Reader r;
  ctx("expected drops \"%s\"", want);
  const bool ok = r.openAll(data, o);
  if (!ok) ctx("expected drops \"%s\", rejected: %s", want, r.reason);
  TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
  TEST_ASSERT_EQUAL_STRING_MESSAGE(want, r.drops.str().c_str(), g_ctx);
  verify(r, &g_rng);
}

struct Edit {
  uint32_t off;
  int width;
  int32_t value;
};
Bytes edited(Bytes d, const std::vector<Edit>& edits) {
  for (const Edit& e : edits) packedit::putN(d, e.off, e.width, static_cast<uint32_t>(e.value));
  packedit::fixCrc(d);
  return d;
}
std::vector<Edit> plus(std::vector<Edit> a, const std::vector<Edit>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

dance::Reason reasonOf(Why w, uint32_t a = 0, uint32_t b = 0, const char* tag = nullptr, const char* tag2 = nullptr) {
  dance::Reason r;
  r.why = w;
  r.a = a;
  r.b = b;
  if (tag) std::memcpy(r.tag, tag, 4);
  if (tag2) std::memcpy(r.tag2, tag2, 4);
  return r;
}

// An ASCII-only B.8, written again for the long-tag test.
std::string refFoldAscii(const std::string& tag) {
  std::string t;
  for (char c : tag) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '&' || c == '+') t += " and ";
    else t += c;
  }
  std::vector<std::string> words;
  std::string w;
  for (char c : t) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      w += c;
    } else if (!w.empty()) {
      words.push_back(w);
      w.clear();
    }
  }
  if (!w.empty()) words.push_back(w);
  std::string key;
  for (size_t i = 0; i < words.size(); ++i)
    key += (words[i] == "n" && i > 0 && i + 1 < words.size()) ? std::string("and") : words[i];
  return key;
}

}  // namespace

void setUp() {}
void tearDown() {}

// ===========================================================================
// 1. The pieces.

void test_crc32() {
  const uint8_t* nine = reinterpret_cast<const uint8_t*>("123456789");
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, dance::crc32(nine, 9));
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, packedit::refCrc32(nine, 9));
  TEST_ASSERT_EQUAL_HEX32(0u, dance::crc32(nine, 0));
  TEST_ASSERT_EQUAL_HEX32(0x12345678u, dance::crc32(nine, 0, 0x12345678u));
  for (size_t cut = 0; cut <= 9; ++cut)
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, dance::crc32(nine + cut, 9 - cut, dance::crc32(nine, cut)));
  // A.17's and the fixture's stored CRCs, and random runs against the reference.
  const Bytes dot = packedit::dotPack();
  TEST_ASSERT_EQUAL_HEX32(0x5BF1E285u, dance::crc32(dot.data() + 16, dot.size() - 16));
  TEST_ASSERT_EQUAL_HEX32(packedit::get32(packedit::testPack(), 12),
                          dance::crc32(dance::kTestPack + 16, dance::kTestPackSize - 16));
  Xorshift32 rng(0x43524333u);
  for (int i = 0; i < 200; ++i) {
    const size_t n = 1 + rng.below(600);
    Bytes b(n);
    for (auto& x : b) x = static_cast<uint8_t>(rng.next());
    HeapCopy c(b.data(), n);
    const size_t cut = rng.below(static_cast<uint32_t>(n + 1));
    const uint32_t want = packedit::refCrc32(b.data(), n);
    TEST_ASSERT_EQUAL_HEX32(want, dance::crc32(c.data(), n));
    TEST_ASSERT_EQUAL_HEX32(want, dance::crc32(c.data() + cut, n - cut, dance::crc32(c.data(), cut)));
  }
}

void test_fnv1a32() {
  TEST_ASSERT_EQUAL_HEX32(0x811C9DC5u, dance::kFnvBasis);
  TEST_ASSERT_EQUAL_HEX32(0x811C9DC5u, dance::fnv1a32("", 0));
  TEST_ASSERT_EQUAL_HEX32(0xE40C292Cu, dance::fnv1a32("a", 1));
  TEST_ASSERT_EQUAL_HEX32(0xE40C292Cu, packedit::refFnv1a32("a", 1));
  TEST_ASSERT_EQUAL_HEX32(0x1234u, dance::fnv1a32("abc", 0, 0x1234u));
  TEST_ASSERT_EQUAL_HEX32(dance::fnv1a32("ab", 2), dance::fnv1a32("b", 1, dance::fnv1a32("a", 1)));
  TEST_ASSERT_EQUAL_HEX32(0x18857B21u, dance::fnv1a32("\0\0\0\0\0\0\0\0\0\1\1\0\0\1\1\0", 16));
}

void test_fold_vectors() {
  for (int i = 0; i < dancevec::kFoldCount; ++i) {
    const dancevec::Fold& f = dancevec::kFold[i];
    ctx("fold case %d \"%s\"", i, f.tag);
    char out[96];
    std::memset(out, 0x55, sizeof out);
    const size_t n = dance::foldGenre(f.tag, out, sizeof out);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(std::strlen(f.key), n, g_ctx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(f.key, out, g_ctx);
    uint32_t key = 0xDEADBEEFu;
    const bool has = dance::genreKey(f.tag, &key);
    if (f.key[0] == 0) {
      TEST_ASSERT_FALSE_MESSAGE(has, g_ctx);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xDEADBEEFu, key, g_ctx);  // untouched
    } else {
      TEST_ASSERT_TRUE_MESSAGE(has, g_ctx);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(f.hash, key, g_ctx);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(packedit::refFnv1a32(f.key, std::strlen(f.key)), key, g_ctx);
    }
  }
  // A key cut by a small cap: the whole length returned, cap - 1 bytes written.
  const char* brasil = "M\xC3\xBAsica Popular Brasileira";
  char out[16];
  std::memset(out, 0x55, sizeof out);
  TEST_ASSERT_EQUAL_size_t(23, dance::foldGenre(brasil, out, 8));
  TEST_ASSERT_EQUAL_STRING("musicap", out);
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[8]));
  std::memset(out, 0x55, sizeof out);
  TEST_ASSERT_EQUAL_size_t(23, dance::foldGenre(brasil, out, 1));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[1]));
  std::memset(out, 0x55, sizeof out);
  TEST_ASSERT_EQUAL_size_t(23, dance::foldGenre(brasil, out, 0));  // nothing written (as snprintf)
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[0]));
  // A 1,000-byte tag: genreKey streams it, with no buffer limit.
  std::string tag;
  while (tag.size() < 1000) tag += "Drum n Bass & R+B, 90s ";
  tag.resize(1000);
  const std::string want = refFoldAscii(tag);
  TEST_ASSERT_TRUE(want.size() > 600);
  std::vector<char> big(2048, 0x55);
  TEST_ASSERT_EQUAL_size_t(want.size(), dance::foldGenre(tag.c_str(), big.data(), big.size()));
  TEST_ASSERT_EQUAL_STRING(want.c_str(), big.data());
  TEST_ASSERT_EQUAL_size_t(want.size(), dance::foldGenre(tag.c_str(), out, sizeof out));
  TEST_ASSERT_EQUAL_STRING(want.substr(0, 15).c_str(), out);
  uint32_t key = 0;
  TEST_ASSERT_TRUE(dance::genreKey(tag.c_str(), &key));
  TEST_ASSERT_EQUAL_HEX32(packedit::refFnv1a32(want.data(), want.size()), key);
}

// D.1: a pack's tags are valid UTF-8, but a malformed byte elsewhere (a
// track's genre, later) folds as textfold::decode reads it: U+FFFD, one byte
// at a time, whose Full replacement is '?' (it isn't in dancepack.py's
// TEXTFOLD), so it separates words like any other dropped character. The
// keys are dancepack.py's fold_genre of the same tag with U+FFFD in place of
// each malformed byte.
void test_fold_malformed() {
  struct Case {
    const char* tag;
    const char* key;
    uint32_t hash;
  };
  static const Case kCases[] = {
      {"a\xFFn\xFF" "b", "aandb", 0x21CFBE1Du},              // a lone 0xFF on each side of "n"
      {"rock\x80roll", "rockroll", 0x211AB187u},             // a lone continuation byte
      {"drum\x80n\xC3" "bass", "drumandbass", 0xE6318A55u},  // C3 without its continuation
      {"hip\xC3", "hip", 0xFED2B25Eu},                       // a sequence cut by the tag's end
      {"hip\xE2\x80", "hip", 0xFED2B25Eu},
      {"\xC0\x80n\xED\xA0\x80", "n", 0xEB0C3431u},           // overlong and surrogate: U+FFFD a byte
      {"\xFF", "", 0},
      {"\xF5\x80\x80\x80", "", 0},
  };
  for (const Case& c : kCases) {
    ctx("malformed fold \"%s\"", c.key);
    char out[32];
    std::memset(out, 0x55, sizeof out);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(std::strlen(c.key), dance::foldGenre(c.tag, out, sizeof out), g_ctx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.key, out, g_ctx);
    uint32_t key = 0xDEADBEEFu;
    TEST_ASSERT_EQUAL_MESSAGE(c.key[0] != 0, dance::genreKey(c.tag, &key), g_ctx);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.key[0] ? c.hash : 0xDEADBEEFu, key, g_ctx);
  }
}

void test_valid_text() {
  struct Case {
    const char* s;
    size_t n;
    bool valid;
  };
  static const Case kCases[] = {
      {"", 0, true},
      {"abc", 3, true},
      {" ~", 2, true},
      {"\xC3\xA9", 2, true},              // U+00E9
      {"\xC2\xA0", 2, true},              // U+00A0: past the C1 controls
      {"\xE2\x80\x99", 3, true},          // U+2019
      {"\xED\x9F\xBF", 3, true},          // U+D7FF
      {"\xEE\x80\x80", 3, true},          // U+E000
      {"\xEF\xBF\xBF", 3, true},          // U+FFFF (Python's decode accepts it)
      {"\xF0\x9F\x98\x80", 4, true},      // U+1F600
      {"\xF4\x8F\xBF\xBF", 4, true},      // U+10FFFF
      {"T\xC3\xABst", 5, true},
      {"\x80", 1, false},                 // a lone continuation byte
      {"a\xBF", 2, false},
      {"\xC0\x80", 2, false},             // overlong
      {"\xC1\xBF", 2, false},
      {"\xE0\x80\x80", 3, false},
      {"\xE0\x9F\xBF", 3, false},
      {"\xF0\x80\x80\x80", 4, false},
      {"\xF0\x8F\xBF\xBF", 4, false},
      {"\xED\xA0\x80", 3, false},         // a surrogate
      {"\xED\xBF\xBF", 3, false},
      {"\xF4\x90\x80\x80", 4, false},     // past U+10FFFF
      {"\xF5\x80\x80\x80", 4, false},
      {"\xF5", 1, false},
      {"\xFE", 1, false},
      {"\xFF", 1, false},
      {"\xC3", 1, false},                 // cut by the string's end
      {"ab\xE2\x80", 4, false},
      {"\xF0\x9F\x98", 3, false},
      {"\xC3(", 2, false},                // a lead byte without its continuation
      {"\xE2\x28\xA1", 3, false},
      {"\xE2\x82(", 3, false},            // a good second byte, then a bad third
      {"\xF0\x9F\x98(", 4, false},        // ... or a bad fourth
      {"\xF0\x9F(\x80", 4, false},
      {"\xC2\x80", 2, false},             // U+0080, a C1 control
      {"\xC2\x9F", 2, false},             // U+009F
      {"\x7F", 1, false},                 // DEL
      {"\x01", 1, false},
      {"\x09", 1, false},
      {"\x1F", 1, false},
      {"\x1B[31m", 5, false},
      {"a\0b", 3, false},                 // a NUL inside
  };
  for (const Case& c : kCases) {
    ctx("validText \"%s\" (%u bytes)", c.s, static_cast<unsigned>(c.n));
    HeapCopy h(reinterpret_cast<const uint8_t*>(c.s), c.n);
    TEST_ASSERT_EQUAL_MESSAGE(c.valid, dance::validText(reinterpret_cast<const char*>(h.data()), c.n), g_ctx);
  }
}

void test_reach() {
  struct Case {
    int lo, hi, beats;
    bool travels, some;
    int rlo, rhi;
  };
  static const Case kCases[] = {
      {0, 0, 1, false, true, 72, 176},  {0, 0, 2, false, true, 72, 176},   {0, 0, 4, false, true, 72, 176},
      {0, 0, 8, false, true, 72, 176},  {0, 0, 1, true, false, 0, 0},      {0, 0, 2, true, true, 72, 176},
      {40, 80, 1, true, true, 40, 80},  {90, 130, 4, false, true, 90, 130}, {36, 60, 4, false, true, 36, 60},
      {36, 60, 8, false, false, 0, 0},  {185, 200, 2, false, false, 0, 0}, {178, 200, 2, true, false, 0, 0},
      {100, 200, 4, true, true, 100, 180}, {40, 200, 8, false, true, 72, 180}, {36, 200, 2, false, true, 36, 180},
      {170, 180, 2, true, true, 170, 176}, {180, 180, 1, false, true, 180, 180}, {36, 36, 4, false, true, 36, 36},
  };
  for (const Case& c : kCases) {
    ctx("reach %d-%d, %d beats, travels %d", c.lo, c.hi, c.beats, c.travels);
    int rlo = -1, rhi = -1;
    TEST_ASSERT_EQUAL_MESSAGE(c.some, dance::reach(c.lo, c.hi, c.beats, c.travels, &rlo, &rhi), g_ctx);
    if (c.some) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(c.rlo, rlo, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(c.rhi, rhi, g_ctx);
    }
  }
}

void test_format_reason() {
  struct Case {
    Why why;
    uint32_t a, b;
    const char* tag;
    const char* tag2;
    const char* text;
  };
  const uint32_t m3 = static_cast<uint32_t>(-3), m49 = static_cast<uint32_t>(-49);
  const uint32_t m128 = static_cast<uint32_t>(-128);
  const Case kCases[] = {
      {Why::None, 0, 0, nullptr, nullptr, ""},
      {Why::ShortRead, 4294967295u, 4294967295u, nullptr, nullptr, "short read (4294967295 of 4294967295 bytes)"},
      {Why::ShortRead, 0, 1156, nullptr, nullptr, "short read (0 of 1156 bytes)"},
      {Why::TooShort, 0, 0, nullptr, nullptr, "too short"},
      {Why::NotMpdn, 0, 0, nullptr, nullptr, "not an MPDN pack"},
      {Why::NeedsNewerPlayer, 0, 0, nullptr, nullptr, "needs a newer player"},
      {Why::SizeMismatch, 1160, 1156, nullptr, nullptr, "size 1160 isn't the file's 1156"},
      {Why::TooBig, 131108, 0, nullptr, nullptr, "too big (131108 bytes)"},
      {Why::BadChecksum, 0, 0, nullptr, nullptr, "bad checksum"},
      {Why::BadChunkCount, 17, 0, nullptr, nullptr, "bad chunk count 17"},
      {Why::BadChunkCount, 0, 0, nullptr, nullptr, "bad chunk count 0"},
      {Why::HeaderUnreadable, 0, 0, nullptr, nullptr, "header unreadable"},
      {Why::ChunkMisaligned, 0, 0, "META", nullptr, "chunk META: misaligned"},
      {Why::ChunkOutOfBounds, 0, 0, "M\x01T\xFF", nullptr, "chunk M?T?: out of bounds"},
      {Why::ChunkOutOfBounds, 0, 0, " ~\x1F\x7F", nullptr, "chunk  ~??: out of bounds"},
      {Why::ChunkTwice, 0, 0, "GEOM", nullptr, "chunk GEOM twice"},
      {Why::ChunksOverlap, 0, 0, "META", "GEOM", "chunks META and GEOM overlap"},
      {Why::ChunksOverlap, 0, 0, "\x80" "ABC", "x\0yz", "chunks ?ABC and x?yz overlap"},
      {Why::NoChunk, 0, 0, "STEP", nullptr, "no STEP chunk"},
      {Why::MetaBad, 0, 0, nullptr, nullptr, "META: bad id"},
      {Why::MetaBad, 1, 0, nullptr, nullptr, "META: bad name"},
      {Why::MetaBad, 2, 0, nullptr, nullptr, "META: bad author"},
      {Why::MetaBad, 3, 0, nullptr, nullptr, "META: bad licence"},
      {Why::MetaBad, 4, 0, nullptr, nullptr, "META: bad url"},
      {Why::MetaBad, 5, 0, nullptr, nullptr, "META: bad version"},
      {Why::MetaBad, 6, 0, nullptr, nullptr, "META: bad count"},
      {Why::GeomTooShort, 0, 0, nullptr, nullptr, "GEOM: too short"},
      {Why::GeomBad, 0, 0, nullptr, nullptr, "GEOM: bad scale"},
      {Why::GeomBad, 1, 0, nullptr, nullptr, "GEOM: bad faces"},
      {Why::GeomBad, 2, 0, nullptr, nullptr, "GEOM: bad cellW"},
      {Why::GeomBad, 3, 0, nullptr, nullptr, "GEOM: bad cellH"},
      {Why::GeomBad, 4, 0, nullptr, nullptr, "GEOM: bad anchorX"},
      {Why::GeomBad, 5, 0, nullptr, nullptr, "GEOM: bad anchorY"},
      {Why::GeomBad, 6, 0, nullptr, nullptr, "GEOM: bad shadowW"},
      {Why::GeomBad, 7, 0, nullptr, nullptr, "GEOM: bad frame count"},
      {Why::GeomBad, 8, 0, nullptr, nullptr, "GEOM: bad rest frame"},
      {Why::PalsSize, 95, 0, nullptr, nullptr, "PALS: 95 bytes, not 96"},
      {Why::ThmbSize, 799, 0, nullptr, nullptr, "THMB: 799 bytes, not 800"},
      {Why::FtabSize, 11, 1, nullptr, nullptr, "FTAB: 11 bytes for 1 frames"},
      {Why::FrameBadBox, 0, 0, nullptr, nullptr, "frame 0: bad box"},
      {Why::FrameDataOutOfBounds, 3, 0, nullptr, nullptr, "frame 3: data out of bounds"},
      {Why::FrameBadData, 255, 0, nullptr, nullptr, "frame 255: bad data"},
      {Why::RestFrameEmpty, 0, 0, nullptr, nullptr, "the rest frame is empty"},
      {Why::StrsTooShort, 0, 0, nullptr, nullptr, "STRS: too short"},
      {Why::StrsBadPoseKey, 2, 0, nullptr, nullptr, "STRS: bad pose key 2"},
      {Why::MovsTooShort, 0, 0, nullptr, nullptr, "MOVS: too short"},
      {Why::MovsCount, 65, 0, nullptr, nullptr, "MOVS: 65 moves"},
      {Why::MovsRecordLength, 31, 0, nullptr, nullptr, "MOVS: record length 31"},
      {Why::StepSize, 7, 0, nullptr, nullptr, "STEP: 7 bytes"},
      {Why::BadName, 0, 0, nullptr, nullptr, "bad name"},
      {Why::BadKind, 0, 0, nullptr, nullptr, "bad kind"},
      {Why::BadSteps, 0, 0, nullptr, nullptr, "bad steps"},
      {Why::FrameOutOfRange, 1, 0, nullptr, nullptr, "frame 1 out of range"},
      {Why::Beats, 3, 0, nullptr, nullptr, "3 beats"},
      {Why::StepTicks, 1, 2, nullptr, nullptr, "step 1: 2 ticks"},
      {Why::TickSum, 13, 12, nullptr, nullptr, "13 of 12 ticks"},
      {Why::StepMs, 1, 5010, nullptr, nullptr, "step 1: 5010 ms"},
      {Why::IdleTotal, 12010, 0, nullptr, nullptr, "12010 ms in all"},
      {Why::BeatFields, 0, 0, nullptr, nullptr, "beat fields on a no-BPM move"},
      {Why::BadWhen, 0, 0, nullptr, nullptr, "bad when"},
      {Why::Pace, 120, 100, nullptr, nullptr, "pace 120-100"},
      {Why::RangeUnreachable, 185, 200, nullptr, nullptr, "range 185-200 unreachable"},
      {Why::Weight, 101, 0, nullptr, nullptr, "weight 101"},
      {Why::Repeat, 9, 0, nullptr, nullptr, "repeat 9"},
      {Why::Bounce, 17, 0, nullptr, nullptr, "bounce 17"},
      {Why::Sway, 17, 0, nullptr, nullptr, "sway 17"},
      {Why::Travel, m3, 0, nullptr, nullptr, "travel -3"},
      {Why::Travel, 3, 0, nullptr, nullptr, "travel 3"},
      {Why::BadStartAt, 0, 0, nullptr, nullptr, "bad start_at"},
      {Why::BadEase, 0, 0, nullptr, nullptr, "bad ease"},
      {Why::BadPlace, 0, 0, nullptr, nullptr, "bad place"},
      {Why::Offset, 0, m49, nullptr, nullptr, "offset 0,-49"},
      {Why::Offset, 49, 0, nullptr, nullptr, "offset 49,0"},
      {Why::Offset, m128, 127, nullptr, nullptr, "offset -128,127"},
      {Why::BadPoseKey, 0, 0, nullptr, nullptr, "bad pose key"},
      {Why::BadGenre, 0, 0, nullptr, nullptr, "bad genre"},
      {Why::SongBpm, 0, 0, nullptr, nullptr, "song_bpm needs track BPM data: skipped in this version"},
      {Why::Needs, 0xA0, 0, nullptr, nullptr, "needs 0xA0"},
      {Why::Needs, 0x02, 0, nullptr, nullptr, "needs 0x02"},
      {Why::Needs, 0xFE, 0, nullptr, nullptr, "needs 0xFE"},
      {Why::TravelsFromNoSpot, 0, 0, nullptr, nullptr, "travels from no spot"},
      {Why::BudgetRed, 25600, 0, nullptr, nullptr, "budget red (25600 px)"},
  };
  const int whys = static_cast<int>(Why::BudgetRed) + 1;
  std::vector<bool> seen(whys, false);
  for (const Case& c : kCases) {
    ctx("formatReason \"%s\"", c.text);
    const dance::Reason r = reasonOf(c.why, c.a, c.b, c.tag, c.tag2);
    char out[dance::kReasonChars];
    std::memset(out, 0x55, sizeof out);
    const size_t n = dance::formatReason(r, out, sizeof out);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(c.text, out, g_ctx);
    TEST_ASSERT_EQUAL_size_t_MESSAGE(std::strlen(c.text), n, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(n < dance::kReasonChars, g_ctx);
    seen[static_cast<int>(c.why)] = true;
  }
  for (int w = 0; w < whys; ++w) {
    ctx("formatReason: no case for Why %d", w);
    TEST_ASSERT_TRUE_MESSAGE(seen[w], g_ctx);
  }
  // Cut at caps 0, 1 and 5; the whole length returned, as snprintf.
  const dance::Reason r = reasonOf(Why::SizeMismatch, 1160, 1156);
  char out[16];
  std::memset(out, 0x55, sizeof out);
  TEST_ASSERT_EQUAL_size_t(31, dance::formatReason(r, out, 0));
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[0]));
  TEST_ASSERT_EQUAL_size_t(31, dance::formatReason(r, nullptr, 0));
  TEST_ASSERT_EQUAL_size_t(31, dance::formatReason(r, out, 1));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[1]));
  std::memset(out, 0x55, sizeof out);
  TEST_ASSERT_EQUAL_size_t(31, dance::formatReason(r, out, 5));
  TEST_ASSERT_EQUAL_STRING("size", out);
  TEST_ASSERT_EQUAL_UINT8(0x55, static_cast<uint8_t>(out[5]));
}

void test_sizes() {
  // D.1: a Pack and a Report are small, on every target.
  TEST_ASSERT_TRUE(sizeof(dance::Pack) <= 192);
  TEST_ASSERT_TRUE(sizeof(dance::Report) <= 48);
  TEST_ASSERT_EQUAL_size_t(64, dance::kReasonChars);
  TEST_ASSERT_EQUAL_UINT32(131072u, dance::kMaxPackBytes);
  TEST_ASSERT_EQUAL_INT(1, dance::kReaderVersion);
  // A Pack is a view: a copy shares the bytes.
  Reader r;
  TEST_ASSERT_TRUE(r.openAll(packedit::dotPack()));
  const dance::Pack copy = r.pack;
  TEST_ASSERT_TRUE(copy.id() == r.pack.id());
  TEST_ASSERT_TRUE(copy.frame(0).data == r.pack.frame(0).data);
  // clear(): back to State None, no faults.
  r.pack.clear();
  ctx("after clear()");
  TEST_ASSERT_EQUAL_UINT16(0, r.pack.regionFaults());
  expectNoneState(r.pack);
}

// ===========================================================================
// 2. The fixture.

void test_fixture_header() {
  Reader r;
  ctx("the fixture");
  TEST_ASSERT_TRUE(r.openAll(packedit::testPack()));
  verify(r, &g_rng);
  const dance::Pack& p = r.pack;
  TEST_ASSERT_EQUAL_UINT32(dance::kTestPackSize, p.size());
  TEST_ASSERT_EQUAL_STRING("test", p.id());
  TEST_ASSERT_EQUAL_STRING("T\xC3\xABst", p.name());
  TEST_ASSERT_EQUAL_STRING("IrosTheBeggar", p.author());
  TEST_ASSERT_EQUAL_STRING("GPL-3.0-or-later", p.licence());
  TEST_ASSERT_EQUAL_STRING("https://github.com/IrosTheBeggar/mstream-mp3-player", p.url());
  TEST_ASSERT_EQUAL_STRING("1.0", p.version());
  TEST_ASSERT_EQUAL_UINT8(14, p.declaredMoves());
  const dance::PackGeom g = p.geom();
  TEST_ASSERT_EQUAL_UINT8(4, g.scale);
  TEST_ASSERT_EQUAL_UINT8(1, g.faces);
  TEST_ASSERT_EQUAL_UINT8(24, g.cellW);
  TEST_ASSERT_EQUAL_UINT8(32, g.cellH);
  TEST_ASSERT_EQUAL_UINT8(12, g.anchorX);
  TEST_ASSERT_EQUAL_UINT8(30, g.anchorY);
  TEST_ASSERT_EQUAL_UINT8(35, g.shadowW);
  TEST_ASSERT_EQUAL_UINT16(25, g.frameCount);
  TEST_ASSERT_EQUAL_UINT16(0, g.restFrame);
  TEST_ASSERT_EQUAL_INT(25, p.frameCount());
  const uint8_t dancePal[48] = {0, 0, 0, 0xf0, 0xf0, 0xf8, 0xa8, 0xac, 0xc0, 0xf4, 0x72, 0xb6, 0xff, 0xd8, 0x4a};
  const uint8_t idlePal[48] = {0, 0, 0, 0xb0, 0xb0, 0xb8, 0x70, 0x74, 0x84, 0x9a, 0x5a, 0x7c, 0xa8, 0x90, 0x50};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(dancePal, p.palette(), 48);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(idlePal, p.idlePalette(), 48);
  // The palettes and the thumbnail point at the pack's own PALS and THMB.
  const Bytes t = packedit::testPack();
  const packedit::Entry pals = packedit::entry(t, "PALS"), thmb = packedit::entry(t, "THMB");
  TEST_ASSERT_TRUE(p.palette() == r.buf.data() + pals.off);
  TEST_ASSERT_TRUE(p.idlePalette() == r.buf.data() + pals.off + 48);
  TEST_ASSERT_TRUE(p.thumb() == r.buf.data() + thmb.off);
  // A.8's thumbnail of the rest frame (box 7,3 11x27: m = 1, at (14, 6)).
  const std::vector<uint8_t> rest = gridOf(p, 0);
  uint8_t want[800] = {};
  const dance::FrameBox b = p.box(0);
  TEST_ASSERT_TRUE(b.x0 == 7 && b.y0 == 3 && b.w == 11 && b.h == 27);
  for (int y = 0; y < b.h; ++y)
    for (int x = 0; x < b.w; ++x) {
      const int tx = (40 - b.w) / 2 + x, ty = (40 - b.h) / 2 + y;
      const uint8_t v = rest[(b.y0 + y) * 24 + b.x0 + x];
      want[ty * 20 + tx / 2] |= static_cast<uint8_t>((tx & 1) ? v : v << 4);
    }
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, p.thumb(), 800);
  // The pose keys (C.10): stand, left, right (the twin), low.
  TEST_ASSERT_EQUAL_INT(4, p.poseKeyCount());
  TEST_ASSERT_EQUAL_STRING("stand", p.poseKey(1));
  TEST_ASSERT_EQUAL_STRING("left", p.poseKey(2));
  TEST_ASSERT_EQUAL_STRING("right", p.poseKey(3));
  TEST_ASSERT_EQUAL_STRING("low", p.poseKey(4));
  TEST_ASSERT_NULL(p.poseKey(0));
  TEST_ASSERT_NULL(p.poseKey(5));
  // One drop: future, for its needs bit.
  TEST_ASSERT_EQUAL_STRING("8:song_bpm needs track BPM data: skipped in this version", r.drops.str().c_str());
  TEST_ASSERT_EQUAL_UINT8(1, r.report.dropped);
  TEST_ASSERT_EQUAL_INT(14, p.moveCount());
  TEST_ASSERT_EQUAL_INT(13, p.keptMoves());
  for (int m = 0; m < 14; ++m) TEST_ASSERT_EQUAL(m != 8, p.moveKept(m));
  // Every frame decodes to its A.16 hash.
  for (int i = 0; i < 25; ++i) {
    ctx("fixture frame %d", i);
    bool ok = false;
    const std::vector<uint8_t> grid = gridOf(p, i, &ok);
    TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(dance::kTestPackFrameHash[i], packedit::refFnv1a32(grid.data(), grid.size()),
                                    g_ctx);
  }
  // Frame 8 (B4) is the blank one.
  TEST_ASSERT_TRUE(zeroBox(p.box(8)));
  TEST_ASSERT_EQUAL_UINT16(0, p.frame(8).len);
}

void test_fixture_moves() {
  // C.10's table, with A.14's frames (TestPack.h's comments name each
  // frame's cells) and B.1's ticks.
  struct Want {
    const char* name;
    uint8_t kind, beats, lo, hi, weight10, when, flags, repeat, needs;
    int8_t travel;
    uint8_t startAt, travelEase, bounce, sway, from, to, fromM, toM, genres, steps;
    uint16_t stepFirst;
    uint8_t songLo, songHi;
  };
  static const Want kMoves[14] = {
      {"bounce", 0, 2, 0, 0, 10, 0, 2, 0, 0, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0},
      {"pulse", 0, 1, 0, 0, 10, 0, 32, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 4, 4, 0, 0},
      {"step-touch", 0, 2, 0, 0, 10, 0, 1, 0, 0, 0, 0, 0, 0, 2, 1, 1, 1, 1, 0, 4, 8, 0, 0},
      {"point", 0, 4, 90, 130, 3, 0, 5, 2, 0, 0, 0, 0, 0, 0, 1, 2, 1, 3, 0, 4, 12, 0, 0},
      {"slow", 0, 4, 36, 60, 25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 16, 0, 0},
      {"fast", 0, 1, 150, 180, 10, 0, 8, 0, 0, 0, 0, 0, 0, 0, 4, 1, 4, 1, 0, 3, 19, 0, 0},
      {"walk", 0, 4, 90, 140, 10, 0, 1, 0, 0, 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 4, 22, 0, 0},
      {"nod", 0, 1, 70, 100, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 3, 26, 0, 0},
      {"future", 0, 2, 0, 0, 10, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 29, 160, 180},
      {"big", 0, 2, 100, 160, 10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 31, 0, 0},
      {"heavy", 0, 2, 0, 0, 10, 0, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 33, 0, 0},
      {"sway", 1, 0, 0, 0, 10, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 35, 0, 0},
      {"breathe", 1, 0, 0, 0, 10, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 37, 0, 0},
      {"look", 1, 0, 0, 0, 10, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 40, 0, 0},
  };
  struct S {
    uint16_t frame;
    bool mirrored;
    uint16_t dur;
    int8_t dx, dy;
    uint8_t ease;
    bool go;
  };
  static const S kSteps[43] = {
      // bounce: A1 B1 C1 D1, 6 ticks each
      {0, 0, 6, 0, 0, 0, 0}, {1, 0, 6, 0, 0, 0, 0}, {0, 0, 6, 0, 0, 0, 0}, {2, 0, 6, 0, 0, 0, 0},
      // pulse: A1 B1 D1 B1, 1/4 0.25 1/4 0.26 (snapped), out
      {0, 0, 3, 0, 0, 3, 0}, {1, 0, 3, 0, -1, 3, 0}, {2, 0, 3, 0, -2, 3, 0}, {1, 0, 3, 0, -1, 3, 0},
      // step-touch: A2 B2 A2 B2 (B2 = A2 mirrored), linear out in hold
      {3, 0, 6, 0, 0, 1, 0}, {3, 1, 6, -1, 0, 3, 0}, {3, 0, 6, 0, 0, 2, 0}, {3, 1, 6, 1, 0, 0, 0},
      // point: A3 B3 C3 B3, 1/2 1/2 1 2, inout
      {4, 0, 6, 0, 0, 4, 0}, {5, 0, 6, 2, -2, 4, 0}, {6, 0, 12, 4, -3, 4, 0}, {5, 0, 24, 2, -2, 4, 0},
      // slow: A4, the blank B4, C4
      {7, 0, 16, 0, 0, 0, 0}, {8, 0, 16, 0, 0, 0, 0}, {9, 0, 16, 0, 0, 0, 0},
      // fast: A5 B5 C5, 1/4 1/2 1/4
      {10, 0, 3, 0, 0, 0, 0}, {11, 0, 6, 0, 0, 0, 0}, {12, 0, 3, 0, 0, 0, 0},
      // walk: A6-D6, go on steps 2-3
      {13, 0, 12, 0, 0, 0, 0}, {14, 0, 12, 0, 0, 0, 1}, {15, 0, 12, 0, 0, 0, 1}, {16, 0, 12, 0, 0, 0, 0},
      // nod: A7 (= A1) B7 A7, triplets
      {0, 0, 4, 0, 0, 0, 0}, {17, 0, 4, 0, 0, 0, 0}, {0, 0, 4, 0, 0, 0, 0},
      // future: A1 B1
      {0, 0, 12, 0, 0, 0, 0}, {1, 0, 12, 0, 0, 0, 0},
      // big, heavy: A8 A8
      {18, 0, 12, -4, 0, 0, 0}, {18, 0, 12, 4, 0, 0, 0}, {18, 0, 12, -12, 0, 0, 0}, {18, 0, 12, 12, 0, 0, 0},
      // sway: A9 (= A1) B9, 2s 2s, inout
      {0, 0, 200, -2, 0, 4, 0}, {19, 0, 200, 2, 0, 4, 0},
      // breathe: A10 B10 C10, 1.1s each
      {20, 0, 110, 0, 0, 0, 0}, {21, 0, 110, 0, 0, 0, 0}, {22, 0, 110, 0, 0, 0, 0},
      // look: A11 B11 A11, 500ms 1s 1.5s
      {23, 0, 50, 0, 0, 0, 0}, {24, 0, 100, 0, 0, 0, 0}, {23, 0, 150, 0, 0, 0, 0},
  };
  Reader r;
  ctx("the fixture");
  TEST_ASSERT_TRUE(r.openAll(packedit::testPack()));
  const dance::Pack& p = r.pack;
  int stepIndex = 0;
  for (int m = 0; m < 14; ++m) {
    const Want& w = kMoves[m];
    const dance::MoveRecord rec = p.moveRecord(m);
    ctx("fixture move %d (%s)", m, w.name);
    if (m == 8) {
      TEST_ASSERT_NULL_MESSAGE(p.genreTag(m, 0), g_ctx);
    }
    TEST_ASSERT_EQUAL_STRING_MESSAGE(w.name, p.moveName(m), g_ctx);
    const uint32_t got[] = {rec.kind,    rec.beats,      rec.paceLo, rec.paceHi, rec.weight10, rec.when,
                            rec.flags,   rec.repeat,     rec.needs,  static_cast<uint8_t>(rec.travel),
                            rec.startAt, rec.travelEase, rec.bounce, rec.sway,   rec.fromKey,  rec.toKey,
                            rec.fromKeyM, rec.toKeyM,    rec.genreCount, rec.stepCount, rec.stepFirst,
                            rec.songLo,  rec.songHi};
    const uint32_t want[] = {w.kind,    w.beats,      w.lo,     w.hi,   w.weight10, w.when,  w.flags,
                             w.repeat,  w.needs,      static_cast<uint8_t>(w.travel), w.startAt, w.travelEase,
                             w.bounce,  w.sway,       w.from,   w.to,   w.fromM,    w.toM,   w.genres,
                             w.steps,   w.stepFirst,  w.songLo, w.songHi};
    TEST_ASSERT_EQUAL_UINT32_ARRAY_MESSAGE(want, got, 23, g_ctx);
    for (int k = 0; k < w.steps; ++k, ++stepIndex) {
      const S& s = kSteps[stepIndex];
      const dance::StepRecord got1 = p.step(m, k);
      ctx("fixture move %d (%s) step %d", m, w.name, k + 1);
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(s.frame, got1.frame, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(s.mirrored, got1.mirrored, g_ctx);
      TEST_ASSERT_EQUAL_UINT16_MESSAGE(s.dur, got1.dur, g_ctx);
      TEST_ASSERT_EQUAL_INT8_MESSAGE(s.dx, got1.dx, g_ctx);
      TEST_ASSERT_EQUAL_INT8_MESSAGE(s.dy, got1.dy, g_ctx);
      TEST_ASSERT_EQUAL_UINT8_MESSAGE(s.ease, got1.ease, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(s.go, got1.go, g_ctx);
    }
  }
  TEST_ASSERT_EQUAL_INT(43, stepIndex);
  // nod's tags, in manifest order, and their keys (FoldVectors' hashes).
  TEST_ASSERT_EQUAL_STRING("hip hop", p.genreTag(7, 0));
  TEST_ASSERT_EQUAL_STRING("R&B", p.genreTag(7, 1));
  TEST_ASSERT_EQUAL_STRING("drum n bass", p.genreTag(7, 2));
  TEST_ASSERT_NULL(p.genreTag(7, 3));
  const uint32_t keys[3] = {0x5DE39FA5u, 0x1BDE7F5Cu, 0xE6318A55u};
  for (int t = 0; t < 3; ++t) {
    uint32_t key = 0;
    TEST_ASSERT_TRUE(dance::genreKey(p.genreTag(7, t), &key));
    TEST_ASSERT_EQUAL_HEX32(keys[t], key);
  }
  // The names point into STRS, zero copy.
  const packedit::Entry strs = packedit::entry(packedit::testPack(), "STRS");
  const uint8_t* lo = r.buf.data() + strs.off;
  const uint8_t* hi = lo + strs.size;
  for (int m = 0; m < 14; ++m) {
    const uint8_t* n = reinterpret_cast<const uint8_t*>(p.moveName(m));
    TEST_ASSERT_TRUE(n > lo && n < hi);
  }
}

void test_dot_pack() {
  const Bytes dot = packedit::dotPack();
  TEST_ASSERT_EQUAL_size_t(1156, dot.size());
  TEST_ASSERT_EQUAL_HEX32(0x5BF1E285u, packedit::get32(dot, 12));
  // A.17's header and directory dump.
  const Bytes dump = packedit::hex(
      "4d 50 44 4e 01 00 01 00 84 04 00 00 85 e2 f1 5b 09 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00"
      "4d 45 54 41 8c 00 00 00 1d 00 00 00 47 45 4f 4d ac 00 00 00 0c 00 00 00 50 41 4c 53 b8 00 00 00"
      "60 00 00 00 54 48 4d 42 18 01 00 00 20 03 00 00 46 54 41 42 38 04 00 00 0c 00 00 00 53 54 52 53"
      "44 04 00 00 09 00 00 00 4d 4f 56 53 50 04 00 00 24 00 00 00 53 54 45 50 74 04 00 00 08 00 00 00"
      "46 44 41 54 7c 04 00 00 06 00 00 00");
  TEST_ASSERT_EQUAL_size_t(140, dump.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(dump.data(), dot.data(), 140);
  TEST_ASSERT_EQUAL_UINT32(1156u, dancevec::kDotPackSize);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(dancevec::kDotPack, dot.data(), 1156);
  Reader r;
  ctx("the dot");
  TEST_ASSERT_TRUE(r.openAll(dot));
  verify(r, &g_rng);
  TEST_ASSERT_EQUAL_STRING("dot", r.pack.id());
  TEST_ASSERT_EQUAL_STRING("Dot", r.pack.name());
  TEST_ASSERT_EQUAL_STRING("Ana", r.pack.author());
  TEST_ASSERT_EQUAL_STRING("CC0-1.0", r.pack.licence());
  TEST_ASSERT_EQUAL_STRING("", r.pack.url());
  TEST_ASSERT_EQUAL_STRING("", r.pack.version());
  TEST_ASSERT_EQUAL_UINT8(1, r.pack.declaredMoves());
  TEST_ASSERT_EQUAL_STRING("", r.drops.str().c_str());
  TEST_ASSERT_EQUAL_HEX32(0x18857B21u, frameHash(r.pack, 0));
  TEST_ASSERT_EQUAL_STRING("hop", r.pack.moveName(0));
  TEST_ASSERT_EQUAL_INT(0, r.pack.poseKeyCount());
  const dance::MoveRecord m = r.pack.moveRecord(0);
  TEST_ASSERT_EQUAL_UINT16(4, m.name);
  TEST_ASSERT_EQUAL_UINT8(1, m.beats);
  TEST_ASSERT_EQUAL_UINT8(10, m.weight10);
  TEST_ASSERT_EQUAL_UINT8(2, m.bounce);
  TEST_ASSERT_EQUAL_UINT8(1, m.stepCount);
  TEST_ASSERT_EQUAL_UINT16(12, r.pack.step(0, 0).dur);
}

// ===========================================================================
// 3. Hand-built packs: test_dancepack.py's ReadPackTest lists, transcribed.

void test_rejections() {
  using namespace packedit;
  const Bytes d = dotPack();
  Bytes zeroCrc = d;
  put32(zeroCrc, 12, 0);
  Bytes mpdx = d;
  std::memcpy(mpdx.data(), "MPDX", 4);  // the CRC doesn't cover the magic
  Bytes hop = chunkOf(d, "MOVS");
  hop.erase(hop.begin(), hop.begin() + 4);
  Bytes movs65 = {65, 32, 0, 0};
  for (int i = 0; i < 65; ++i) movs65 = cat(movs65, hop);
  const Bytes big = dotWith({{"XTRA", Bytes(131072, 0)}});
  const std::string tooBig = "too big (" + std::to_string(big.size()) + " bytes)";
  struct Case {
    std::string want;
    Bytes data;
  };
  const std::vector<Case> cases = {
      {"too short", head(d, 31)},
      {"not an MPDN pack", mpdx},
      {"needs a newer player", patch16(d, 6, 2)},
      {"size 1160 isn't the file's 1156", patch32(d, 8, 1160)},
      {"bad checksum", zeroCrc},
      {"bad chunk count 0", patch16(d, 16, 0)},
      {"bad chunk count 17", patch16(d, 16, 17)},
      {"chunk META: misaligned", patch32(d, 36, 141)},
      {"chunk FDAT: out of bounds", patch32(d, 136, 100)},
      {"chunk GEOM twice", patchRaw(d, 116, "GEOM")},
      {"chunks META and GEOM overlap", patch32(d, 48, 168)},
      {"no STEP chunk", patchRaw(d, 116, "STEX")},
      {"chunk META: out of bounds", patch32(d, 40, 1017)},
      {"META: bad id", patchRaw(d, 141, "DOT")},
      {"META: bad name", patch8(d, 145, 0)},
      {"META: bad count", patch32(d, 40, 28)},
      {"GEOM: too short", patch32(d, 52, 11)},
      {"GEOM: bad scale", patch8(d, 172, 5)},
      {"GEOM: bad faces", patch8(d, 173, 3)},
      {"GEOM: bad cellW", patch8(d, 174, 0)},
      {"GEOM: bad cellH", patch8(d, 175, 161)},
      {"GEOM: bad anchorX", patch8(d, 176, 4)},
      {"GEOM: bad anchorY", patch8(d, 177, 0)},
      {"GEOM: bad shadowW", patch8(d, 178, 161)},
      {"GEOM: bad frame count", patch16(d, 180, 0)},
      {"GEOM: bad rest frame", patch16(d, 182, 1)},
      {"PALS: 95 bytes, not 96", patch32(d, 64, 95)},
      {"THMB: 799 bytes, not 800", patch32(d, 76, 799)},
      {"FTAB: 11 bytes for 1 frames", patch32(d, 88, 11)},
      {"frame 0: bad box", patch8(d, kDotFtab, 3)},
      {"frame 0: data out of bounds", patch16(d, kDotFtab + 8, 7)},
      {"frame 0: bad data", patch8(d, kDotFdat + 2, 0x21)},
      {"frame 0: bad data", patch16(d, kDotFtab + 8, 5)},  // the last token past len
      {"the rest frame is empty", patchRaw(d, kDotFtab, Bytes(12, 0).data(), 12)},
      {"STRS: too short", patch32(d, 100, 3)},
      {"MOVS: too short", patch32(d, 112, 3)},
      {"MOVS: record length 31", patch8(d, 1105, 31)},
      {"STEP: 7 bytes", patch32(d, 124, 7)},
      // A.3's bounds in an order that can't wrap in 32-bit arithmetic: off <= total first.
      {"chunk STEP: out of bounds", patch32(patch32(d, 120, 0xFFFFFFF0u), 124, 0)},
      {"chunk STEP: out of bounds", patch32(patch32(d, 120, 0xFFFFFFF0u), 124, 0x20)},
      {"chunk META: out of bounds", patch32(d, 36, 32)},  // inside the directory
      // A.9's likewise: an FTAB off near 2^32 whose 32-bit sum with len wraps to 2.
      {"frame 0: data out of bounds", patch16(patch32(d, kDotFtab + 4, 0xFFFFFFFCu), kDotFtab + 8, 6)},
      // A.10: skip + the runs <= w on every row, a row with no tokens too.
      {"frame 0: bad data",
       dotWith({{"FDAT", hex("09 00 00 01 11")}, {"FTAB", hex("01 02 02 02 00 00 00 00 05 00 00 00")}})},
      // A.15's note: anchorY's two stage limits read as anchorY.
      {"GEOM: bad anchorY", patch8(d, 175, 15)},  // 11 rows under the ground
      {"GEOM: bad anchorY", patch8(patch8(patch8(d, 172, 4), 175, 40), 177, 39)},  // 156 > 153
      // The overlap names the earlier entry, then the later; missing tags in A.4's order.
      {"chunks STRS and MOVS overlap", patch32(d, 96, 1104)},
      {"no META chunk", patchRaw(patchRaw(d, 32, "XXXX"), 44, "YYYY")},
      {"no GEOM chunk", patchRaw(patchRaw(d, 44, "XXXX"), 116, "YYYY")},
      // Frame indices count from 0.
      {"frame 1: bad box",
       dotWith({{"GEOM", hex("01 00 04 04 02 04 00 00 02 00 00 00")},
                {"FTAB", hex("01 02 02 02 00 00 00 00 06 00 00 00 03 00 02 01 00 00 00 00 00 00 00 00")}})},
      // A.6: cellW and cellH are 1 .. 160 // scale.
      {"GEOM: bad cellH", patch8(d, 175, 0)},
      // A.12: the MOVS head in order: the count before the records' size.
      {"MOVS: 65 moves", patch8(d, 1104, 65)},
      {"MOVS: too short", patch8(d, 1104, 2)},
      // A.1: a string ends in a NUL: the id's replaced by "x".
      {"META: bad id", patch8(d, 144, 0x78)},
      {"MOVS: 65 moves", dotWith({{"MOVS", movs65}})},
      // P = 2: "hop" reads as key 1, key 2 runs past the end (ids from 1).
      {"STRS: bad pose key 2", patch8(d, kDotStrs, 2)},
      {tooBig, big},
  };
  for (const Case& c : cases) expectRejected(c.want.c_str(), c.data);
}

void test_order() {
  using namespace packedit;
  const Bytes d = dotPack();
  expectRejected("needs a newer player", patch32(patch16(d, 6, 2), 8, 1160));
  expectRejected("GEOM: bad scale", patch8(patch8(d, 172, 5), 173, 3));
  // A.15's first rows in order: the magic before minReader before the size.
  expectRejected("not an MPDN pack", patch32(patch16(patchRaw(d, 0, "MPDx"), 6, 2), 8, 1));
  // The CRC before the chunk count.
  Bytes c = patch16(d, 16, 0);
  put32(c, 12, 0);
  expectRejected("bad checksum", c);
  // META before GEOM before PALS before THMB before FTAB.
  expectRejected("META: bad id", patch32(patch8(patchRaw(d, 141, "DOT"), 172, 5), 64, 95));
  expectRejected("PALS: 95 bytes, not 96", patch32(patch32(d, 64, 95), 76, 799));
  // Adjacent A.15 rows that both fail: the first one's reason.
  expectRejected("frame 0: bad box", patch32(patch8(d, kDotFtab + 2, 5), kDotFtab + 4, 100));  // then out of bounds
  expectRejected("MOVS: too short", patch32(patch32(d, 112, 3), 124, 7));                       // then STEP: 7 bytes
  expectRejected("MOVS: 65 moves", patch8(patch8(d, 1104, 65), 1105, 31));                       // then record length 31
  expectDrops("0:bounce 17", patch8(patch8(d, kDotMov + 14, 17), kDotMov + 15, 17));               // then sway 17
  expectDrops("0:song_bpm needs track BPM data: skipped in this version", patch8(d, kDotMov + 10, 3));  // then needs
}

void test_accepted() {
  using namespace packedit;
  const Bytes d = dotPack();
  expectDrops("", d);
  {
    Bytes z = d;
    put32(z, 12, 0);
    Reader r;
    ctx("kSkipCrc on a zeroed CRC");
    TEST_ASSERT_TRUE_MESSAGE(r.openAll(z, skipCrc()), g_ctx);
    TEST_ASSERT_EQUAL_STRING("dot", r.pack.id());
    verify(r, &g_rng);
  }
  expectDrops("", dotWith({{"XTRA", Bytes{'a', 'b', 'c'}}}));
  expectDrops("", dotWith({{"STAG", Bytes(8, 'x')}, {"SPOT", Bytes()}}));
  // A zero-size chunk overlaps nothing, even inside another chunk.
  {
    const Bytes data = dotWith({{"ZERO", Bytes()}});
    const size_t e = 32 + 12 * 9;
    TEST_ASSERT_EQUAL_MEMORY("ZERO", &data[e], 4);
    expectDrops("", patch32(data, e + 4, entry(data, "FTAB").off + 4));
  }
  // format, flags and reserved bytes are ignored; a longer GEOM, META bytes past the count too.
  expectDrops("", patch16(d, 4, 7));
  expectDrops("", patch32(patch16(d, 18, 0xFFFF), 20, 0xFFFFFFFFu));
  expectDrops("", dotWith({{"GEOM", cat(chunkOf(d, "GEOM"), Bytes{'m', 'o', 'r', 'e'})},
                           {"META", cat(chunkOf(d, "META"), Bytes{'!'})}}));
  // A longer MOVS record: the reader takes the first 32 B.
  {
    Bytes rec = chunkOf(d, "MOVS");
    rec.erase(rec.begin(), rec.begin() + 4);
    expectDrops("", dotWith({{"MOVS", cat(cat(Bytes{1, 40, 0, 0}, rec), Bytes(8, 0xFF))}}));
  }
  // A genre run at a valid string (the name, "hop").
  expectDrops("", edited(d, {{kDotMov + 20, 2, 4}, {kDotMov + 22, 1, 1}}));
  // The rest frame's bit 15 is masked off (A.6).
  expectDrops("", patch16(d, 182, 0x8000));
  // An idle move's genre test reads genreCount only.
  const std::vector<Edit> idle = {
      {kDotMov + 2, 1, 1}, {kDotMov + 3, 1, 0}, {kDotMov + 14, 1, 0}, {kDotMov + 7, 1, 3}, {kDotStep + 2, 2, 60}};
  expectDrops("", edited(d, idle));
  expectDrops("", edited(d, plus(idle, {{kDotMov + 20, 2, 4}})));
  // Every reserved byte or bit is ignored.
  expectDrops("", patch8(d, 179, 5));
  expectDrops("", patch16(d, kDotFtab + 10, 0xFFFF));
  expectDrops("", patchRaw(d, kDotStrs + 1, "\x01\x02\x03"));
  expectDrops("", patch16(d, 1106, 0xFFFF));
  expectDrops("", patch32(d, kDotMov + 28, 0xFFFFFFFFu));
  expectDrops("", patch8(d, kDotMov + 8, 0xC0));
  expectDrops("", patch8(d, kDotStep + 7, 0xFE));
}

void test_accepted_token_sequences() {
  using namespace packedit;
  // The dot as a 4x4 grid.
  const uint8_t dot[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0};
  struct Case {
    uint8_t box[4];
    const char* rows;
  };
  const Case cases[] = {
      {{0, 2, 4, 2}, "00 02 00 11  00 02 00 11"},  // a leading gap, a loose box
      {{1, 2, 3, 2}, "00 02 11 00  00 02 11 00"},  // a trailing gap
      {{1, 2, 2, 2}, "00 02 01 01  00 02 01 01"},  // 2 px split 1 + 1
  };
  for (const Case& c : cases) {
    const Bytes data = hex(c.rows);
    Bytes ftab(c.box, c.box + 4);
    ftab.resize(12, 0);
    put16(ftab, 8, static_cast<uint32_t>(data.size()));
    Reader r;
    ctx("token sequence %s", c.rows);
    TEST_ASSERT_TRUE_MESSAGE(r.openAll(dotWith({{"FTAB", ftab}, {"FDAT", data}})), g_ctx);
    verify(r, &g_rng);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r.drops.str().c_str(), g_ctx);
    const std::vector<uint8_t> grid = gridOf(r.pack, 0);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(dot, grid.data(), 16, g_ctx);
  }
  // skip = w, n = 0: a blank row.
  Reader r;
  ctx("a blank row");
  TEST_ASSERT_TRUE_MESSAGE(r.openAll(dotWith({{"FTAB", hex("01 02 02 02 00 00 00 00 05 00 00 00")},
                                              {"FDAT", hex("02 00 00 01 11")}})),
                           g_ctx);
  verify(r, &g_rng);
  const uint8_t want[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(want, gridOf(r.pack, 0).data(), 16);
}

void test_drops() {
  using namespace packedit;
  const Bytes d = dotPack();
  const uint32_t mv = kDotMov, st = kDotStep;
  const std::vector<Edit> idle = {{mv + 2, 1, 1}, {mv + 3, 1, 0}, {mv + 14, 1, 0}};
  const std::vector<Edit> idleOk = plus(idle, {{st + 2, 2, 60}, {mv + 7, 1, 3}});
  struct Case {
    const char* want;
    std::vector<Edit> edits;
  };
  const std::vector<Case> cases = {
      {"bad name", {{mv, 2, 3}}},
      {"bad name", {{mv, 2, 1090}}},
      {"bad kind", {{mv + 2, 1, 2}}},
      {"bad kind", {{mv + 2, 1, 2}, {mv + 3, 1, 3}}},  // the first check wins
      {"bad steps", {{mv + 23, 1, 0}}},
      {"bad steps", {{mv + 24, 2, 1}}},
      {"bad steps", {{mv + 23, 1, 33}}},
      {"frame 1 out of range", {{st, 2, 1}}},
      {"3 beats", {{mv + 3, 1, 3}}},
      {"step 1: 2 ticks", {{st + 2, 2, 2}}},
      {"13 of 12 ticks", {{st + 2, 2, 13}}},
      {"step 1: 100 ms", plus(idle, {{st + 2, 2, 10}, {mv + 7, 1, 3}})},
      {"step 1: 5010 ms", plus(idle, {{st + 2, 2, 501}, {mv + 7, 1, 3}})},
      {"400 ms in all", plus(idle, {{st + 2, 2, 40}, {mv + 7, 1, 3}})},
      {"beat fields on a no-BPM move", {{mv + 2, 1, 1}, {st + 2, 2, 60}, {mv + 7, 1, 3}}},
      {"bad when", plus(idle, {{st + 2, 2, 60}})},
      {"bad when", plus(idle, {{st + 2, 2, 60}, {mv + 7, 1, 4}})},
      {"bad when", {{mv + 7, 1, 1}}},
      {"pace 30-100", {{mv + 4, 1, 30}, {mv + 5, 1, 100}}},
      {"pace 0-100", {{mv + 4, 1, 0}, {mv + 5, 1, 100}}},
      {"pace 120-100", {{mv + 4, 1, 120}, {mv + 5, 1, 100}}},
      {"pace 36-201", {{mv + 4, 1, 36}, {mv + 5, 1, 201}}},
      {"range 185-200 unreachable", {{mv + 4, 1, 185}, {mv + 5, 1, 200}}},
      {"weight 0", {{mv + 6, 1, 0}}},
      {"weight 101", {{mv + 6, 1, 101}}},
      {"repeat 9", {{mv + 9, 1, 9}}},
      {"repeat 4", {{mv + 3, 1, 8}, {st + 2, 2, 96}, {mv + 9, 1, 4}}},
      {"bounce 17", {{mv + 14, 1, 17}}},
      {"sway 17", {{mv + 15, 1, 17}}},
      {"travel 1", {{mv + 11, 1, 1}}},
      {"travel 3", {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 11, 1, 3}}},
      {"travel -3", {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 11, 1, -3}}},
      {"bad start_at", {{mv + 12, 1, 5}}},
      {"bad ease", {{mv + 13, 1, 5}}},
      {"bad ease", {{st + 6, 1, 5}}},
      {"bad place", {{mv + 8, 1, 12}}},
      {"offset 49,0", {{st + 4, 1, 49}}},
      {"offset 0,-49", {{st + 5, 1, -49}}},
      {"bad pose key", {{mv + 16, 1, 1}}},
      {"bad pose key", {{mv + 19, 1, 1}}},
      {"bad genre", {{mv + 22, 1, 9}}},
      {"bad genre", {{mv + 20, 2, 0}, {mv + 22, 1, 1}}},
      {"bad genre", {{mv + 20, 2, 4}, {mv + 22, 1, 2}}},  // the run's second tag is past the end
      {"song_bpm needs track BPM data: skipped in this version", {{mv + 10, 1, 1}, {mv + 26, 1, 160}, {mv + 27, 1, 180}}},
      {"needs 0x02", {{mv + 10, 1, 2}}},
      {"needs 0xA0", {{mv + 10, 1, 0xA0}}},  // uppercase hex digits
      // Each beat field on a no-BPM move.
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 8, 1, 4}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 8, 1, 2}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 20, 2, 4}, {mv + 22, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 11, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 15, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 9, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 4, 1, 90}, {mv + 5, 1, 100}})},
      // A.11: o + 2 + n <= size: the name's NUL in STRS's padding is past its end.
      {"bad name", {{100, 4, 8}}},
      // A.1/A.11's order: o + 2 <= size before the length byte at o is read.
      {"bad name", {{mv, 2, 8}}},
      {"bad name", {{mv, 2, 9}}},
      {"bad name", {{mv, 2, 0xFFFF}}},
      {"bad genre", {{mv + 20, 2, 9}, {mv + 22, 1, 1}}},
      {"bad genre", {{mv + 20, 2, 0xFFFF}, {mv + 22, 1, 1}}},
      // Bit 0 with others: song_bpm's row comes first.
      {"song_bpm needs track BPM data: skipped in this version", {{mv + 10, 1, 3}}},
      // B.7: any travel, negative too, takes the +1 octave from a 2-beat move.
      {"range 178-180 unreachable", {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 11, 1, -2}, {mv + 4, 1, 178}, {mv + 5, 1, 180}}},
      {"range 178-180 unreachable", {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 11, 1, 2}, {mv + 4, 1, 178}, {mv + 5, 1, 180}}},
      {"range 178-200 unreachable", {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 11, 1, 1}, {mv + 4, 1, 178}, {mv + 5, 1, 200}}},
      // Pace 35: one below A.15's 36.
      {"pace 35-100", {{mv + 4, 1, 35}, {mv + 5, 1, 100}}},
      // Each beat field alone on a no-BPM move.
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 3, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 4, 1, 90}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 5, 1, 100}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 14, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 22, 1, 1}})},
      {"beat fields on a no-BPM move", plus(idleOk, {{mv + 8, 1, 8}})},
  };
  for (const Case& c : cases) expectDrops((std::string("0:") + c.want).c_str(), edited(d, c.edits));
  // ... and kept at the edges: pace 36, and 178-200 when the move doesn't travel.
  expectDrops("", edited(d, {{mv + 4, 1, 36}, {mv + 5, 1, 100}}));
  expectDrops("", edited(d, {{mv + 3, 1, 2}, {st + 2, 2, 24}, {mv + 4, 1, 178}, {mv + 5, 1, 200}}));
}

void test_drops_rebuilt() {
  using namespace packedit;
  const Bytes d = dotPack();
  const Bytes movs = chunkOf(d, "MOVS");
  // A tag that is a valid string but folds to nothing is a bad genre; "ab" is fine.
  struct Tag {
    const char* tag;
    const char* want;
  };
  for (const Tag& t : {Tag{"?!", "0:bad genre"}, Tag{"ab", ""}}) {
    Bytes strs = chunkOf(d, "STRS");
    strs.push_back(static_cast<uint8_t>(std::strlen(t.tag)));
    strs.insert(strs.end(), t.tag, t.tag + std::strlen(t.tag));
    strs.push_back(0);
    Bytes m = movs;
    put16(m, 4 + 20, 9);
    put8(m, 4 + 22, 1);
    expectDrops(t.want, dotWith({{"STRS", strs}, {"MOVS", m}}));
  }
  // An idle move's steps sum to at most 12,000 ms: 400 + 400 + 401 units is 12,010.
  Bytes m = movs;
  for (int k : {2, 3, 4, 5}) put8(m, 4 + k, k == 2 ? 1 : 0);  // kind idle, beats 0, no pace
  put8(m, 4 + 7, 3);                                          // when any
  put8(m, 4 + 14, 0);                                         // bounce 0
  put8(m, 4 + 23, 3);                                         // 3 steps
  struct Durs {
    uint16_t d[3];
    const char* want;
  };
  for (const Durs& c : {Durs{{400, 400, 401}, "0:12010 ms in all"}, Durs{{400, 400, 400}, ""}}) {
    Bytes step;
    for (uint16_t dur : c.d) step = cat(step, Bytes{0, 0, static_cast<uint8_t>(dur), static_cast<uint8_t>(dur >> 8), 0, 0, 0, 0});
    expectDrops(c.want, dotWith({{"MOVS", m}, {"STEP", step}}));
  }
}

// test_dancepack.py's test_limits (D.3's limit anchors), rebuilt: each limit
// on both sides of it with well-formed values, a string exactly at its limit
// and one byte past it (the mutator only patches a length byte in place,
// which moves the NUL), each count at its cap, and what the accessors read
// from the packs at the caps.
namespace limits {

using packedit::Bytes;

Bytes str8(const std::string& s) {
  Bytes b{static_cast<uint8_t>(s.size())};
  b.insert(b.end(), s.begin(), s.end());
  b.push_back(0);
  return b;
}
Bytes hopRecord() {
  Bytes m = packedit::chunkOf(packedit::dotPack(), "MOVS");
  return Bytes(m.begin() + 4, m.begin() + 36);
}
Bytes record(const std::vector<Edit>& edits) {  // hop's record, fields patched (offsets in the record)
  Bytes r = hopRecord();
  for (const Edit& e : edits) packedit::putN(r, e.off, e.width, static_cast<uint32_t>(e.value));
  return r;
}
Bytes movs(const std::vector<Bytes>& records, uint8_t rlen = 32) {
  Bytes out = {static_cast<uint8_t>(records.size()), rlen, 0, 0};
  for (const Bytes& r : records) {
    out = packedit::cat(out, r);
    out.resize(out.size() + (rlen - 32), 0xFF);
  }
  return out;
}
Bytes steps(const std::vector<uint16_t>& durs) {
  Bytes out;
  for (uint16_t d : durs) out = packedit::cat(out, Bytes{0, 0, static_cast<uint8_t>(d), static_cast<uint8_t>(d >> 8), 0, 0, 0, 0});
  return out;
}
Bytes meta(int field, const std::string& value) {  // A.17's META with one string replaced
  const std::string f[6] = {"dot", "Dot", "Ana", "CC0-1.0", "", ""};
  Bytes out;
  for (int i = 0; i < 6; ++i) out = packedit::cat(out, str8(i == field ? value : f[i]));
  out.push_back(1);
  return out;
}
const Bytes kStrsHead = {0, 0, 0, 0, 3, 'h', 'o', 'p', 0};  // P = 0, "hop" at 4

}  // namespace limits

void test_limits() {
  using namespace packedit;
  using limits::meta;
  using limits::movs;
  using limits::record;
  using limits::steps;
  using limits::str8;
  const Bytes d = dotPack();
  const Bytes& head = limits::kStrsHead;
  // A.5: each META string at its limit (multi-byte where A.1 allows it), then one byte past it.
  const char* fields[6] = {"id", "name", "author", "licence", "url", "version"};
  const size_t hi[6] = {23, 24, 40, 40, 80, 16};
  for (int f = 0; f < 6; ++f) {
    const std::string ok = f == 0 ? std::string(23, 'a') : std::string(hi[f] - 3, 'a') + "\xC3\xA9" "a";
    TEST_ASSERT_EQUAL_size_t(hi[f], ok.size());
    expectDrops("", dotWith({{"META", meta(f, ok)}}));
    expectRejected((std::string("META: bad ") + fields[f]).c_str(), dotWith({{"META", meta(f, ok + "a")}}));
  }
  expectRejected("META: bad id", dotWith({{"META", meta(0, "-dot")}}));  // no leading -
  // A.1: a good second byte, then a bad third or fourth.
  expectRejected("META: bad name", patchRaw(d, 146, "\xE2\x82("));
  expectRejected("META: bad licence", patchRaw(d, 156, "\xF0\x9F\x98("));
  expectRejected("META: bad licence", patchRaw(d, 156, "\xF0\x9F(\x80"));
  // A.3: two unknown tags sharing three bytes.
  expectDrops("", dotWith({{"XTRA", Bytes{'a'}}, {"XTRB", Bytes{'b'}}}));
  // A.6: anchorY * scale <= 153 and (cellH - anchorY) * scale <= 10, at each limit and past it.
  struct G {
    uint8_t scale, cellH, anchorY;
    bool ok;
  };
  for (const G& g : {G{1, 153, 153, true}, G{1, 154, 154, false}, G{2, 76, 76, true}, G{2, 77, 77, false},
                     G{3, 51, 51, true}, G{3, 52, 52, false}, G{1, 14, 4, true}, G{2, 9, 4, true}, G{2, 10, 4, false}}) {
    const Bytes data = edited(d, {{172, 1, g.scale}, {175, 1, g.cellH}, {177, 1, g.anchorY}});
    if (g.ok) expectDrops("", data);
    else expectRejected("GEOM: bad anchorY", data);
  }
  // A.10: skip = w + 1 with no tokens; a last row head of one byte at FDAT's end (verify()
  // asserts no read past FDAT's view).
  expectRejected("frame 0: bad data", patchRaw(d, kDotFdat, hex("03 00 00 02 01 01").data(), 6));
  expectRejected("frame 0: bad data", dotWith({{"FTAB", hex("01 02 02 02 00 00 00 00 04 00 00 00")},
                                               {"FDAT", hex("00 01 11 00")}}));
  // A.11: pose keys and names of 23 bytes and of 24, and with a leading -; a name below offset 4.
  struct K {
    std::string key;
    bool ok;
  };
  for (const K& k : {K{std::string(23, 'k'), true}, K{std::string(24, 'k'), false}, K{"-x", false}}) {
    const Bytes pose = cat(Bytes{1, 0, 0, 0}, str8(k.key));
    const Bytes data = dotWith({{"STRS", cat(pose, str8("hop"))},
                                {"MOVS", movs({record({{0, 2, static_cast<int32_t>(pose.size())}})})}});
    if (k.ok) expectDrops("", data);
    else expectRejected("STRS: bad pose key 1", data);
  }
  expectDrops("", dotWith({{"STRS", cat(Bytes{0, 0, 0, 0}, str8(std::string(23, 'n')))}}));
  expectDrops("0:bad name", dotWith({{"STRS", cat(Bytes{0, 0, 0, 0}, str8(std::string(24, 'n')))}}));
  expectDrops("0:bad name", dotWith({{"STRS", cat(Bytes{0, 0, 0, 0}, str8("-hop"))}}));
  expectDrops("0:bad name", dotWith({{"STRS", hex("00 00 00 01 61 00")}, {"MOVS", movs({record({{0, 2, 3}})})}}));
  // Genre tags of 63 bytes and of 64 (B.8 copies a tag into 64 B on the stack), at STRS offset 9.
  struct T {
    std::string tag;
    const char* want;
  };
  std::string e31;
  for (int i = 0; i < 31; ++i) e31 += "\xC3\xA9";
  const T tags[] = {{std::string(63, 'a'), ""}, {e31 + "a", ""}, {"ZZ", ""}, {std::string(64, 'a'), "0:bad genre"},
                    {e31 + "\xC3\xA9", "0:bad genre"}, {std::string(62, 'a') + "\xC3\xA9", "0:bad genre"}};
  for (const T& t : tags) {
    const Bytes data = dotWith({{"STRS", cat(head, str8(t.tag))}, {"MOVS", movs({record({{20, 2, 9}, {22, 1, 1}})})}});
    expectDrops(t.want, data);
    if (t.want[0]) continue;
    Reader r;
    TEST_ASSERT_TRUE(r.openAll(data));
    TEST_ASSERT_EQUAL_STRING(t.tag.c_str(), r.pack.genreTag(0, 0));
    uint32_t key = 0;
    TEST_ASSERT_TRUE(dance::genreKey(r.pack.genreTag(0, 0), &key));
  }
  // 8 good tags, then 9; a tag past STRS offset 255.
  Bytes nine;
  for (char c = 'a'; c <= 'i'; ++c) nine = cat(nine, str8(std::string(1, c)));
  const Bytes eight = dotWith({{"STRS", cat(head, nine)}, {"MOVS", movs({record({{20, 2, 9}, {22, 1, 8}})})}});
  expectDrops("", eight);
  expectDrops("0:bad genre", dotWith({{"STRS", cat(head, nine)}, {"MOVS", movs({record({{20, 2, 9}, {22, 1, 9}})})}}));
  Bytes far = head;
  far.resize(260, 0);
  const Bytes rock = dotWith({{"STRS", cat(far, str8("rock"))}, {"MOVS", movs({record({{20, 2, 260}, {22, 1, 1}})})}});
  expectDrops("", rock);
  // A.12: 64 moves; three records of 40 bytes, each different.
  const Bytes many = dotWith({{"MOVS", movs(std::vector<Bytes>(64, limits::hopRecord()))}});
  expectDrops("", many);
  const Bytes three =
      dotWith({{"STRS", cat(head, str8("two"))},
               {"STEP", cat(steps({12}), hex("00 00 0c 00 01 00 02 00"))},
               {"MOVS", movs({limits::hopRecord(), record({{0, 2, 9}, {6, 1, 20}, {14, 1, 3}, {24, 2, 1}}),
                              record({{9, 1, 1}, {14, 1, 5}, {15, 1, 2}})},
                             40)}});
  expectDrops("", three);
  // A.13: 32 steps and 33, beat (8 beats of 3-tick steps) and idle (200 ms steps).
  const std::vector<Edit> beat8 = {{3, 1, 8}}, idle = {{2, 1, 1}, {3, 1, 0}, {7, 1, 3}, {14, 1, 0}};
  struct S {
    const std::vector<Edit>* edits;
    uint16_t dur;
  };
  Bytes beat32;
  for (const S& s : {S{&beat8, 3}, S{&idle, 20}}) {
    const Bytes ok = dotWith({{"STEP", steps(std::vector<uint16_t>(32, s.dur))}, {"MOVS", movs({record(plus(*s.edits, {{23, 1, 32}}))})}});
    expectDrops("", ok);
    if (s.dur == 3) beat32 = ok;
    expectDrops("0:bad steps", dotWith({{"STEP", steps(std::vector<uint16_t>(33, s.dur))},
                                        {"MOVS", movs({record(plus(*s.edits, {{23, 1, 33}}))})}}));
  }
  // Idle steps of 190 and 200 ms; idle totals of 490 and 500 ms.
  struct I {
    std::vector<uint16_t> durs;
    const char* want;
  };
  for (const I& i : {I{{19, 20, 20}, "0:step 1: 190 ms"}, I{{20, 20, 20}, ""}, I{{20, 29}, "0:490 ms in all"}, I{{20, 30}, ""}}) {
    expectDrops(i.want, dotWith({{"STEP", steps(i.durs)},
                                 {"MOVS", movs({record(plus(idle, {{23, 1, static_cast<int32_t>(i.durs.size())}}))})}}));
  }

  // What the accessors read at the caps.
  {
    Reader r;
    ctx("limits: 64 moves");
    TEST_ASSERT_TRUE(r.openAll(many));
    TEST_ASSERT_EQUAL_INT(64, r.pack.moveCount());
    TEST_ASSERT_EQUAL_INT(64, r.pack.keptMoves());
    TEST_ASSERT_TRUE(r.pack.moveKept(63));
    TEST_ASSERT_FALSE(r.pack.moveKept(64));
    TEST_ASSERT_EQUAL_STRING("hop", r.pack.moveName(63));
    verify(r, &g_rng);
  }
  {
    Reader r;
    ctx("limits: 32 steps of 3 ticks");
    TEST_ASSERT_TRUE(r.openAll(beat32));
    TEST_ASSERT_EQUAL_UINT16(3, r.pack.step(0, 31).dur);
    TEST_ASSERT_TRUE(zeroStep(r.pack.step(0, 32)));
  }
  {
    Reader r;
    ctx("limits: 8 tags");
    TEST_ASSERT_TRUE(r.openAll(eight));
    TEST_ASSERT_EQUAL_STRING("a", r.pack.genreTag(0, 0));
    TEST_ASSERT_EQUAL_STRING("h", r.pack.genreTag(0, 7));
    TEST_ASSERT_NULL(r.pack.genreTag(0, 8));
  }
  {
    Reader r;
    ctx("limits: a tag at STRS offset 260");
    TEST_ASSERT_TRUE(r.openAll(rock));
    TEST_ASSERT_EQUAL_UINT16(260, r.pack.moveRecord(0).genreFirst);
    TEST_ASSERT_EQUAL_STRING("rock", r.pack.genreTag(0, 0));
  }
  {
    Reader r;
    ctx("limits: three records of 40 bytes");
    TEST_ASSERT_TRUE(r.openAll(three));
    TEST_ASSERT_EQUAL_INT(3, r.pack.moveCount());
    const char* names[3] = {"hop", "two", "hop"};
    const uint8_t weights[3] = {10, 20, 10}, bounces[3] = {2, 3, 5}, sways[3] = {0, 0, 2}, repeats[3] = {0, 0, 1};
    for (int m = 0; m < 3; ++m) {
      const dance::MoveRecord rec = r.pack.moveRecord(m);
      TEST_ASSERT_EQUAL_STRING(names[m], r.pack.moveName(m));
      TEST_ASSERT_EQUAL_UINT8(weights[m], rec.weight10);
      TEST_ASSERT_EQUAL_UINT8(bounces[m], rec.bounce);
      TEST_ASSERT_EQUAL_UINT8(sways[m], rec.sway);
      TEST_ASSERT_EQUAL_UINT8(repeats[m], rec.repeat);
    }
    const dance::StepRecord s = r.pack.step(1, 0);
    TEST_ASSERT_EQUAL_INT8(1, s.dx);
    TEST_ASSERT_EQUAL_UINT8(2, s.ease);
    TEST_ASSERT_EQUAL_INT8(0, r.pack.step(0, 0).dx);
  }
  // A frame past FDAT offset 65,535: the dot's rows, read at their 32-bit offset.
  {
    Bytes fdat(65540, 0);
    fdat = cat(fdat, hex("00 01 11 00 01 11"));
    const Bytes data = dotWith({{"FTAB", hex("01 02 02 02 04 00 01 00 06 00 00 00")}, {"FDAT", fdat}});
    Reader r;
    ctx("limits: a frame at FDAT offset 65,540");
    TEST_ASSERT_TRUE(r.openAll(data));
    verify(r, &g_rng);
    TEST_ASSERT_TRUE(r.pack.frame(0).data == r.buf.data() + entry(data, "FDAT").off + 65540);
    TEST_ASSERT_EQUAL_HEX32(0x18857B21u, frameHash(r.pack, 0));
  }
}

void test_strings_read_inside_strs() {
  using namespace packedit;
  // STRS moved to the end of the file and grown to 12 B ("hop" at 4, "x" at
  // 9), so a byte past STRS is past the data too: ASan catches a read there.
  std::vector<Chunk> parts = dotChunks();
  Bytes strs, movs;
  std::vector<Chunk> order;
  for (const Chunk& c : parts) {
    if (c.tag == "STRS") strs = cat(c.data, Bytes{1, 'x', 0});
    else order.push_back(c);
    if (c.tag == "MOVS") movs = c.data;
  }
  struct Case {
    uint16_t name, gFirst;
    uint8_t gCount;
    const char* want;
  };
  const Case cases[] = {
      {4, 0, 0, ""}, {9, 0, 0, ""}, {12, 0, 0, "0:bad name"}, {0xFFFF, 0, 0, "0:bad name"},
      {4, 9, 1, ""}, {4, 9, 2, "0:bad genre"}, {4, 12, 1, "0:bad genre"},
  };
  for (const Case& c : cases) {
    Bytes m = movs;
    put16(m, 4, c.name);
    put16(m, 4 + 20, c.gFirst);
    put8(m, 4 + 22, c.gCount);
    std::vector<Chunk> chunks;
    for (const Chunk& ch : order) chunks.push_back(ch.tag == "MOVS" ? Chunk{"MOVS", m} : ch);
    chunks.push_back({"STRS", strs});
    const Bytes data = assemble(chunks);
    const Entry e = entry(data, "STRS");
    TEST_ASSERT_EQUAL_size_t(32 + 12 * 8, e.at);
    TEST_ASSERT_EQUAL_size_t(data.size(), e.off + e.size);
    TEST_ASSERT_EQUAL_UINT32(12, e.size);
    expectDrops(c.want, data);
  }
}

// test_dancepack.py's test_card_budget_and_spots, built by hand: scale 4,
// 40x40 cells, anchor 20,38; frame 0 (the rest, B1) a 4x8 block at 18,30,
// frame 1 (A1) the whole cell. `held` is red (kept by heavy); `walk`
// travels 2 from -1. The bytes needn't be the compiler's, only the moves.
Bytes cardPack(uint8_t heldFlags, uint8_t walkStartAt) {
  using namespace packedit;
  Bytes meta = hex("04 63 61 72 64 00 04 43 61 72 64 00 01 41 00 03 4d 49 54 00 00 00 00 00 02");
  Bytes pals(96, 0);
  pals[3] = 0xFF;
  pals[6] = pals[7] = pals[8] = 0xFF;
  Bytes ftab = hex("12 1e 04 08 00 00 00 00 18 00 00 00  00 00 28 28 18 00 00 00 c8 00 00 00");
  Bytes strs = hex("00 00 00 00 04 68 65 6c 64 00 04 77 61 6c 6b 00");
  Bytes rec(32, 0), movs = {2, 32, 0, 0};
  put16(rec, 0, 4);
  rec[3] = 2;
  rec[6] = 10;
  rec[8] = heldFlags;
  rec[23] = 2;
  movs = cat(movs, rec);
  rec = Bytes(32, 0);
  put16(rec, 0, 10);
  rec[3] = 2;
  rec[6] = 10;
  rec[11] = 2;
  rec[12] = walkStartAt;
  rec[13] = 4;
  rec[23] = 2;
  rec[24] = 2;
  movs = cat(movs, rec);
  Bytes step = hex("01 00 0c 00 00 00 00 00 01 00 0c 00 00 00 00 00 00 00 0c 00 00 00 00 00 00 00 0c 00 00 00 00 00");
  Bytes fdat;
  for (int y = 0; y < 8; ++y) fdat = cat(fdat, hex("00 01 32"));
  for (int y = 0; y < 40; ++y) fdat = cat(fdat, hex("00 03 f1 f1 71"));
  return assemble({{"META", meta},
                   {"GEOM", hex("04 00 28 28 14 26 00 00 02 00 00 00")},
                   {"PALS", pals},
                   {"THMB", Bytes(800, 0)},
                   {"FTAB", ftab},
                   {"STRS", strs},
                   {"MOVS", movs},
                   {"STEP", step},
                   {"FDAT", fdat}});
}

void test_step3_rows_wait_for_step3() {
  using namespace packedit;
  // D.2: without a hook, a move that fails only `travels from no spot` or
  // `budget red` is kept and not reported.
  expectDrops("", cardPack(16, 0));
  expectDrops("", cardPack(0, 0));   // red without heavy: step 3's budget row
  expectDrops("", cardPack(16, 1));  // start_at centre leaves walk no spot: step 3's
  // test_spots_only_in_orientations_it_plays: the fixture's walk without
  // its mirror flag, from the centre, keyed 10 native px right everywhere.
  Bytes t = testPack();
  const uint32_t rec = entry(t, "MOVS").off + 4 + 32 * 6;
  const uint32_t step = entry(t, "STEP").off;
  TEST_ASSERT_EQUAL_UINT8(1, t[rec + 8] & 1);
  t[rec + 8] = static_cast<uint8_t>(t[rec + 8] & ~1);
  t[rec + 12] = 1;
  const uint32_t first = get16(t, rec + 24);
  for (uint32_t k = 0; k < t[rec + 23]; ++k) t[step + 8 * (first + k) + 4] = 10;
  fixCrc(t);
  expectDrops("8:song_bpm needs track BPM data: skipped in this version", t);
}

// ===========================================================================
// 4. The hook.

struct HookLog {
  int calls = 0;
  int move[64];
  bool good[64];
};

bool stubLate(void* ctx, const dance::Pack& pack, int move, dance::Reason* why) {
  HookLog& log = *static_cast<HookLog*>(ctx);
  if (log.calls < 64) {
    const dance::MoveRecord rec = pack.moveRecord(move);
    bool good = pack.state() == dance::Pack::State::Open && !pack.moveKept(move) && rec.stepCount > 0 &&
                pack.moveName(move) != nullptr && pack.frameCount() == 25;
    for (int k = 0; k < rec.stepCount; ++k) good = good && pack.step(move, k).dur > 0;
    log.move[log.calls] = move;
    log.good[log.calls] = good;
  }
  ++log.calls;
  if (move == 2) {
    why->why = Why::TravelsFromNoSpot;
    return false;
  }
  if (move == 9) {
    why->why = Why::BudgetRed;
    why->a = 12345;
    return false;
  }
  return true;
}

void test_late_check_hook() {
  HookLog log;
  dance::OpenOptions o;
  o.late = &stubLate;
  o.lateCtx = &log;
  Reader r;
  ctx("the fixture with a stub hook");
  TEST_ASSERT_TRUE(r.openAll(packedit::testPack(), &o));
  verify(r, &g_rng);
  const int want[] = {0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13};
  TEST_ASSERT_EQUAL_INT(13, log.calls);
  for (int i = 0; i < 13; ++i) {
    TEST_ASSERT_EQUAL_INT(want[i], log.move[i]);
    TEST_ASSERT_TRUE(log.good[i]);
  }
  TEST_ASSERT_EQUAL_STRING(
      "2:travels from no spot|8:song_bpm needs track BPM data: skipped in this version|9:budget red (12345 px)",
      r.drops.str().c_str());
  TEST_ASSERT_EQUAL_UINT8(3, r.report.dropped);
  TEST_ASSERT_EQUAL_INT(11, r.pack.keptMoves());
  TEST_ASSERT_FALSE(r.pack.moveKept(2));
  TEST_ASSERT_FALSE(r.pack.moveKept(8));
  TEST_ASSERT_FALSE(r.pack.moveKept(9));
  TEST_ASSERT_TRUE(r.pack.moveKept(3));
  // A hook that keeps everything: the same as no hook.
  struct Keep {
    static bool all(void*, const dance::Pack&, int, dance::Reason*) { return true; }
  };
  o.late = &Keep::all;
  o.lateCtx = nullptr;
  expectDrops("8:song_bpm needs track BPM data: skipped in this version", packedit::testPack(), &o);
}

// D.2: dropMove, the budget's drop after open(), one kept move a frame (5.7).
void test_drop_move() {
  const char* song = "8:song_bpm needs track BPM data: skipped in this version";
  const dance::Reason red = reasonOf(Why::BudgetRed, 25600);
  Reader r;
  ctx("dropMove on the fixture");
  TEST_ASSERT_TRUE(r.openAll(packedit::testPack()));
  r.drops.lastMove = -1;  // dropMove's drops come after open()'s
  // A move not kept, or out of range: false, and nothing changes.
  for (int m : {8, -1, 14, 63, 64, INT_MAX, INT_MIN}) TEST_ASSERT_FALSE(r.pack.dropMove(m, red, &r.report));
  TEST_ASSERT_EQUAL_UINT8(1, r.report.dropped);
  TEST_ASSERT_EQUAL_STRING(song, r.drops.str().c_str());
  TEST_ASSERT_EQUAL_INT(13, r.pack.keptMoves());
  // Two kept moves, in MOVS order: dropped and reported after open()'s drop.
  TEST_ASSERT_TRUE(r.pack.dropMove(9, red, &r.report));
  TEST_ASSERT_TRUE(r.pack.dropMove(10, reasonOf(Why::BudgetRed, 20001), &r.report));
  TEST_ASSERT_FALSE(r.pack.dropMove(9, red, &r.report));  // once
  TEST_ASSERT_EQUAL_STRING((std::string(song) + "|9:budget red (25600 px)|10:budget red (20001 px)").c_str(),
                           r.drops.str().c_str());
  TEST_ASSERT_EQUAL_UINT8(3, r.report.dropped);
  TEST_ASSERT_EQUAL_INT(11, r.pack.keptMoves());
  TEST_ASSERT_FALSE(r.pack.moveKept(9));
  TEST_ASSERT_FALSE(r.pack.moveKept(10));
  TEST_ASSERT_TRUE(r.pack.moveKept(11));
  TEST_ASSERT_EQUAL_STRING("big", r.pack.moveName(9));  // the record stays readable
  verify(r, &g_rng);
  // Without a report: the kept bit and the count only.
  TEST_ASSERT_TRUE(r.pack.dropMove(0, red));
  TEST_ASSERT_FALSE(r.pack.moveKept(0));
  TEST_ASSERT_EQUAL_INT(10, r.pack.keptMoves());
  TEST_ASSERT_EQUAL_UINT8(3, r.report.dropped);
  // Out of State Open: false.
  TEST_ASSERT_TRUE(r.scan(packedit::testPack(), 2048, static_cast<uint32_t>(packedit::testPack().size())));
  TEST_ASSERT_FALSE(r.pack.dropMove(1, red, &r.report));
  r.pack.clear();
  TEST_ASSERT_FALSE(r.pack.dropMove(1, red, &r.report));
  TEST_ASSERT_EQUAL_INT(0, r.drops.calls);
  // The kept mask's top bit: move 63 of 64.
  const Bytes many = packedit::dotWith({{"MOVS", limits::movs(std::vector<Bytes>(64, limits::hopRecord()))}});
  ctx("dropMove on 64 moves");
  TEST_ASSERT_TRUE(r.openAll(many));
  TEST_ASSERT_TRUE(r.pack.dropMove(63, red, &r.report));
  TEST_ASSERT_FALSE(r.pack.moveKept(63));
  TEST_ASSERT_TRUE(r.pack.moveKept(62));
  TEST_ASSERT_EQUAL_INT(63, r.pack.keptMoves());
  TEST_ASSERT_EQUAL_STRING("63:budget red (25600 px)", r.drops.str().c_str());
  verify(r, &g_rng);
}

// ===========================================================================
// 5. Short reads.

void test_short_reads() {
  using namespace packedit;
  for (const Bytes& base : {dotPack(), testPack()}) {
    const uint32_t size = static_cast<uint32_t>(base.size());
    for (uint32_t avail : {0u, 1u, 31u, 32u, 140u, size / 2, size - 1}) {
      Reader r;
      const std::string want =
          "short read (" + std::to_string(avail) + " of " + std::to_string(size) + " bytes)";
      ctx("%s", want.c_str());
      TEST_ASSERT_FALSE_MESSAGE(r.open(base, avail), g_ctx);
      if (avail == 0) TEST_ASSERT_NULL(r.buf.data());
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.reason, g_ctx);
      verify(r, &g_rng);
    }
  }
  // Before any other check: garbage bytes, a short read all the same.
  Reader r;
  ctx("garbage, short");
  TEST_ASSERT_FALSE(r.open(Bytes(64, 0xFF), 10));
  TEST_ASSERT_EQUAL_STRING("short read (10 of 64 bytes)", r.reason);
  verify(r, &g_rng);
  // avail above size: only data[0, size) is read (the copy holds just those).
  ctx("avail above size");
  TEST_ASSERT_TRUE(r.open(dotPack(), 1156 + 100));
  verify(r, &g_rng);
}

// ===========================================================================
// 6. Scan mode, 5.5's rows in their order.

void expectScan(const char* want, const Bytes& b, uint32_t avail, uint32_t size) {
  Reader r;
  if (want) ctx("scan: expected \"%s\" (avail %u, size %u)", want, avail, size);
  else ctx("scan: expected a pass (avail %u, size %u)", avail, size);
  const bool ok = r.scan(b, avail, size);
  if (want) {
    TEST_ASSERT_FALSE_MESSAGE(ok, g_ctx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want, r.reason, g_ctx);
  } else {
    TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
  }
  verify(r, &g_rng);
}

void test_scan_rows() {
  using namespace packedit;
  const Bytes d = dotPack();
  const char* hu = "header unreadable";
  // Row 1: the header without the CRC.
  expectScan("too short", d, 0, 1156);
  expectScan("too short", d, 31, 1156);
  Bytes mpdx = d;
  std::memcpy(mpdx.data(), "MPDX", 4);
  expectScan("not an MPDN pack", mpdx, 2048, 1156);
  expectScan("needs a newer player", patch16(d, 6, 2), 2048, 1156);
  expectScan("size 1160 isn't the file's 1156", patch32(d, 8, 1160), 2048, 1156);
  Bytes longer = d;
  longer.resize(2000, 0);  // the header still says 1156
  expectScan("size 1156 isn't the file's 2000", longer, 2000, 2000);
  expectScan("too big (131076 bytes)", patch32(d, 8, 131076), 1156, 131076);
  expectScan("bad chunk count 0", patch16(d, 16, 0), 1156, 1156);
  expectScan("bad chunk count 17", patch16(d, 16, 17), 1156, 1156);
  Bytes tiny = head(d, 100);
  put32(tiny, 8, 100);
  expectScan("bad chunk count 9", tiny, 100, 100);  // the directory past the end
  // Row 2: the directory within avail.
  expectScan(hu, d, 139, 1156);
  // Row 3: A.3 against the total size, whatever avail holds.
  expectScan("chunk FDAT: out of bounds", patch32(d, 136, 100), 1080, 1156);
  expectScan("chunk META: misaligned", patch32(d, 36, 141), 2048, 1156);
  expectScan("chunk GEOM twice", patchRaw(d, 116, "GEOM"), 2048, 1156);
  expectScan("chunks META and GEOM overlap", patch32(d, 48, 168), 2048, 1156);
  // Row 4: the nine required tags.
  expectScan("no STEP chunk", patchRaw(d, 116, "STEX"), 2048, 1156);
  expectScan("no GEOM chunk", patchRaw(d, 44, "GEOX"), 2048, 1156);
  // Row 5: META, GEOM, PALS and THMB within avail (off, then only the end).
  for (uint32_t avail : {140u, 168u, 171u, 183u, 279u, 280u, 1079u}) expectScan(hu, d, avail, 1156);
  expectScan(nullptr, d, 1080, 1156);
  // ... before a bad META (row 5 before row 6).
  const Bytes badId = patchRaw(d, 141, "DOT");
  expectScan(hu, badId, 1079, 1156);
  expectScan("META: bad id", badId, 1080, 1156);
  // Row 6: META.
  expectScan("META: bad name", patch8(d, 145, 0), 1080, 1156);
  expectScan("META: bad count", patch32(d, 40, 28), 1080, 1156);
  // Row 7: GEOM, in table order; the rest frame below the frame count.
  expectScan("GEOM: too short", patch32(d, 52, 11), 2048, 1156);
  expectScan("GEOM: bad scale", patch8(patch8(d, 172, 5), 173, 3), 2048, 1156);
  expectScan("GEOM: bad rest frame", patch16(d, 182, 1), 2048, 1156);
  expectScan("GEOM: bad anchorY", patch8(d, 175, 15), 2048, 1156);
  // Row 8: PALS and THMB sizes.
  expectScan("PALS: 95 bytes, not 96", patch32(d, 64, 95), 2048, 1156);
  expectScan("THMB: 799 bytes, not 800", patch32(d, 76, 799), 2048, 1156);
  // The orders between rows.
  expectScan("META: bad id", patch8(badId, 172, 5), 2048, 1156);
  expectScan("GEOM: bad scale", patch32(patch8(d, 172, 5), 64, 95), 2048, 1156);
  expectScan("needs a newer player", patch16(patch16(d, 6, 2), 16, 0), 2048, 1156);
  // Only open() checks the CRC, FTAB, the frames, STRS, MOVS and STEP.
  Bytes zeroCrc = d;
  put32(zeroCrc, 12, 0);
  expectScan(nullptr, zeroCrc, 2048, 1156);
  expectScan(nullptr, patch32(d, 88, 11), 2048, 1156);
  expectScan(nullptr, patch8(d, 1104, 65), 2048, 1156);
  expectScan(nullptr, patch32(d, 124, 7), 2048, 1156);
  expectScan(nullptr, patch32(d, 100, 3), 2048, 1156);
  expectScan(nullptr, patch8(d, kDotFdat + 2, 0x21), 2048, 1156);
  const Bytes emptyRest = patchRaw(d, kDotFtab, Bytes(12, 0).data(), 12);
  expectScan(nullptr, emptyRest, 2048, 1156);  // whether the rest's box is empty needs FTAB
  expectRejected("the rest frame is empty", emptyRest);
  // An avail above size is cut to size.
  Bytes extra = d;
  extra.resize(1256, 0xEE);
  expectScan(nullptr, extra, 1256, 1156);
  {
    Reader r;
    ctx("scan: avail above size");
    TEST_ASSERT_TRUE(r.scan(extra, 1256, 1156));
    TEST_ASSERT_EQUAL_UINT32(1156, r.pack.size());
    TEST_ASSERT_EQUAL_STRING("dot", r.pack.id());
    TEST_ASSERT_EQUAL_STRING("", r.pack.url());
  }
}

void test_scan_fixture() {
  // The fixture's first 2,048 bytes give what open() gives.
  const Bytes t = packedit::testPack();
  Reader s, o;
  ctx("scan of the fixture");
  TEST_ASSERT_TRUE(s.scan(t, 2048, static_cast<uint32_t>(t.size())));
  verify(s, &g_rng);
  TEST_ASSERT_TRUE(o.openAll(t));
  TEST_ASSERT_TRUE(s.pack.state() == dance::Pack::State::Scanned);
  TEST_ASSERT_EQUAL_UINT32(t.size(), s.pack.size());
  TEST_ASSERT_EQUAL_STRING(o.pack.id(), s.pack.id());
  TEST_ASSERT_EQUAL_STRING(o.pack.name(), s.pack.name());
  TEST_ASSERT_EQUAL_STRING(o.pack.author(), s.pack.author());
  TEST_ASSERT_EQUAL_STRING(o.pack.licence(), s.pack.licence());
  TEST_ASSERT_EQUAL_STRING(o.pack.url(), s.pack.url());
  TEST_ASSERT_EQUAL_STRING(o.pack.version(), s.pack.version());
  TEST_ASSERT_EQUAL_UINT8(o.pack.declaredMoves(), s.pack.declaredMoves());
  const dance::PackGeom a = s.pack.geom(), b = o.pack.geom();
  const uint32_t ga[] = {a.scale, a.faces, a.cellW, a.cellH, a.anchorX, a.anchorY, a.shadowW, a.frameCount, a.restFrame};
  const uint32_t gb[] = {b.scale, b.faces, b.cellW, b.cellH, b.anchorX, b.anchorY, b.shadowW, b.frameCount, b.restFrame};
  TEST_ASSERT_EQUAL_UINT32_ARRAY(gb, ga, 9);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(o.pack.palette(), s.pack.palette(), 48);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(o.pack.idlePalette(), s.pack.idlePalette(), 48);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(o.pack.thumb(), s.pack.thumb(), 800);
  // The scan's strings point into its own buffer.
  TEST_ASSERT_TRUE(reinterpret_cast<const uint8_t*>(s.pack.id()) >= s.buf.data() &&
                   reinterpret_cast<const uint8_t*>(s.pack.id()) < s.buf.data() + 2048);
  // Exactly the first 1,284 bytes (A.4's worst case) are enough here too.
  Reader q;
  TEST_ASSERT_TRUE(q.scan(t, 1284, static_cast<uint32_t>(t.size())));
  verify(q, &g_rng);
}

// ===========================================================================
// 7-9. The fuzz.

// The bytes no reader row reads (A.15's note on reserved bytes, padding and
// the unchecked PALS and THMB): a flip there must change nothing.
std::vector<bool> ignoredBytes(const Bytes& b) {
  std::vector<bool> ig(b.size(), true);
  auto mark = [&](size_t from, size_t to, bool v) {
    for (size_t i = from; i < to && i < ig.size(); ++i) ig[i] = v;
  };
  mark(0, 18, false);
  const auto dir = packedit::directory(b);
  mark(32, 32 + 12 * dir.size(), false);
  for (const auto& e : dir) mark(e.off, e.off + e.size, false);
  const packedit::Entry pals = packedit::entry(b, "PALS"), thmb = packedit::entry(b, "THMB");
  const packedit::Entry geom = packedit::entry(b, "GEOM"), ftab = packedit::entry(b, "FTAB");
  const packedit::Entry strs = packedit::entry(b, "STRS"), movs = packedit::entry(b, "MOVS");
  mark(pals.off, pals.off + pals.size, true);
  mark(thmb.off, thmb.off + thmb.size, true);
  mark(geom.off + 7, geom.off + 8, true);
  for (uint32_t f = 0; f < ftab.size / 12; ++f) mark(ftab.off + 12 * f + 10, ftab.off + 12 * f + 12, true);
  mark(strs.off + 1, strs.off + 4, true);
  mark(movs.off + 2, movs.off + 4, true);
  const uint32_t count = b[movs.off], recLen = b[movs.off + 1];
  for (uint32_t m = 0; m < count; ++m) {
    const uint32_t rec = movs.off + 4 + recLen * m;
    mark(rec + 28, rec + recLen, true);
  }
  return ig;
}

void test_truncations() {
  using namespace packedit;
  for (const Bytes& base : {dotPack(), testPack()}) {
    Reader full;
    TEST_ASSERT_TRUE(full.openAll(base));
    const std::string baseDrops = full.drops.str();
    const uint32_t lastEnd = lastChunkEnd(base);
    TEST_ASSERT_TRUE(lastEnd <= base.size() && lastEnd + 4 > base.size());
    Reader r;
    for (uint32_t len = 0; len <= base.size(); ++len) {
      Bytes b = head(base, len);
      fixSize(b);
      fixCrc(b);
      ctx("truncated to %u of %u bytes", len, static_cast<unsigned>(base.size()));
      const bool ok = r.openAll(b);
      if (len < 32) TEST_ASSERT_EQUAL_STRING_MESSAGE("too short", r.reason, g_ctx);
      TEST_ASSERT_EQUAL_MESSAGE(len >= lastEnd, ok, g_ctx);
      if (ok) TEST_ASSERT_EQUAL_STRING_MESSAGE(baseDrops.c_str(), r.drops.str().c_str(), g_ctx);
      verify(r, &g_rng);
    }
  }
}

void test_flips() {
  using namespace packedit;
  const Bytes bases[2] = {dotPack(), testPack()};
  std::string baseDrops[2];
  std::vector<bool> ignored[2];
  for (int k = 0; k < 2; ++k) {
    Reader full;
    TEST_ASSERT_TRUE(full.openAll(bases[k]));
    baseDrops[k] = full.drops.str();
    ignored[k] = ignoredBytes(bases[k]);
  }
  Xorshift32 rng(0x464C4950u);
  Reader r;
  int accepted = 0, quiet = 0;
  for (int i = 0; i < 10000; ++i) {
    const int k = i & 1;
    Bytes b = bases[k];
    const uint32_t off = 16 + rng.below(static_cast<uint32_t>(b.size() - 16));
    const uint32_t x = 1 + rng.below(255);
    b[off] = static_cast<uint8_t>(b[off] ^ x);
    fixCrc(b);
    ctx("flip %d: base %d, byte %u ^ 0x%02X", i, k, off, x);
    const bool ok = r.openAll(b);
    accepted += ok;
    if (ignored[k][off]) {
      ++quiet;
      TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(baseDrops[k].c_str(), r.drops.str().c_str(), g_ctx);
    }
    verify(r, &g_rng);
  }
  TEST_ASSERT_TRUE(accepted > 1000);
  TEST_ASSERT_TRUE(quiet > 1000);
}

// The structure of a pristine base, for the mutator's in-place edits.
struct Layout {
  Bytes base;
  uint32_t off[9] = {}, size[9] = {};
  std::vector<uint32_t> metaStr;  // META's six length bytes (absolute)
  uint32_t metaCount = 0;
  std::vector<uint32_t> strsStr;  // every STRS string's length byte (absolute)
  std::vector<uint32_t> fStart, fLen, ftabOff, ftabLen;
  int frames = 0, moves = 0, steps = 0, poseKeys = 0;
  uint8_t cellW = 0, cellH = 0;
  std::vector<uint32_t> u32pool;  // every directory off, size and end, and the edges
};

enum { kMeta, kGeom, kPals, kThmb, kFtab, kStrs, kMovs, kStep, kFdat };

Layout layoutOf(const Bytes& b) {
  using namespace packedit;
  Layout L;
  L.base = b;
  for (const Entry& e : directory(b))
    for (int t = 0; t < 9; ++t)
      if (e.tag == kTags[t]) {
        L.off[t] = e.off;
        L.size[t] = e.size;
      }
  uint32_t p = L.off[kMeta];
  for (int i = 0; i < 6; ++i) {
    L.metaStr.push_back(p);
    p += 2 + b[p];
  }
  L.metaCount = p;
  const uint32_t g = L.off[kGeom];
  L.cellW = b[g + 2];
  L.cellH = b[g + 3];
  L.frames = static_cast<int>(get16(b, g + 8));
  for (int f = 0; f < L.frames; ++f) {
    const uint32_t rec = L.off[kFtab] + 12 * f;
    L.ftabOff.push_back(get32(b, rec + 4));
    L.ftabLen.push_back(get16(b, rec + 8));
    L.fStart.push_back(L.off[kFdat] + get32(b, rec + 4));
    L.fLen.push_back(get16(b, rec + 8));
  }
  L.poseKeys = b[L.off[kStrs]];
  for (uint32_t q = L.off[kStrs] + 4; q < L.off[kStrs] + L.size[kStrs]; q += 2 + b[q]) L.strsStr.push_back(q);
  L.moves = b[L.off[kMovs]];
  L.steps = static_cast<int>(L.size[kStep] / 8);
  const uint32_t len = static_cast<uint32_t>(b.size());
  L.u32pool = {0, 1, 4, 32, 140, 144, len, len - 4, len + 4, 131072, 131073, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u,
               0xFFFFFFFCu, 0xFFFFFFFFu};
  for (int t = 0; t < 9; ++t) {
    L.u32pool.push_back(L.off[t]);
    L.u32pool.push_back(L.size[t]);
    L.u32pool.push_back(L.off[t] + L.size[t]);
  }
  return L;
}

uint32_t pick(Xorshift32& r, const std::vector<uint32_t>& pool) { return pool[r.below(static_cast<uint32_t>(pool.size()))]; }

const std::vector<Bytes>& utf8Edges() {
  static const std::vector<Bytes> edges = {
      {0x80}, {0xC0, 0x80}, {0xC1, 0xBF}, {0xE0, 0x80, 0x80}, {0xF0, 0x80, 0x80, 0x80}, {0xED, 0xA0, 0x80},
      {0xF4, 0x90, 0x80, 0x80}, {0xF5}, {0xC3}, {0xE2, 0x80}, {0xC2, 0x80}, {0x7F}, {0xC3, 0xA9},
      {0xE2, 0x80, 0x99}, {0xF0, 0x9F, 0x98, 0x80}, {0x00}, {0x01}, {'A'}, {'-'}, {'_'}, {'z'}, {'0'}, {' '},
      {0xE2, 0x82, 'A'}, {0xF0, 0x9F, 0x98, 'A'}};
  return edges;
}

void writeSeq(Bytes& b, uint32_t pos, const Bytes& seq) {
  for (size_t i = 0; i < seq.size() && pos + i < b.size(); ++i) b[pos + i] = seq[i];
}

// One of D.3's structure-aware edits, in place.
void mutate(Bytes& b, const Layout& L, Xorshift32& r) {
  using namespace packedit;
  const Bytes& o = L.base;
  auto near = [](uint32_t v) { return std::vector<uint32_t>{v - 1, v + 1, v ^ 0x80u}; };
  auto with = [](std::vector<uint32_t> a, const std::vector<uint32_t>& c) {
    a.insert(a.end(), c.begin(), c.end());
    return a;
  };
  switch (r.below(22)) {
    case 0: {  // the header's format, minReader, chunk count and flags
      const uint32_t pos = std::vector<uint32_t>{4, 6, 16, 16, 18}[r.below(5)];
      put16(b, pos, pick(r, with({0, 1, 2, 8, 9, 10, 16, 17, 0xFF, 0xFFFF}, near(get16(o, pos)))));
      break;
    }
    case 1:  // the total size
      put32(b, 8, pick(r, with(L.u32pool, near(static_cast<uint32_t>(o.size())))));
      break;
    case 2: {  // a directory tag: another required one, an unknown one, a duplicate
      const uint32_t e = 32 + 12 * r.below(9);
      static const char* const other[] = {"XTRA", "STAG", "SPOT", "meta", "\0\0\0\0", "\xFF\x01MS"};
      const char* tag = r.below(2) ? kTags[r.below(9)] : other[r.below(6)];
      std::memcpy(&b[e], tag, 4);
      break;
    }
    case 3: {  // a directory offset
      const uint32_t e = 32 + 12 * r.below(9);
      put32(b, e + 4, pick(r, with(L.u32pool, {get32(o, e + 4) - 4, get32(o, e + 4) + 4, get32(o, e + 4) + 1})));
      break;
    }
    case 4: {  // a directory size
      const uint32_t e = 32 + 12 * r.below(9);
      const uint32_t off = get32(o, e + 4), len = static_cast<uint32_t>(o.size());
      put32(b, e + 8, pick(r, with(L.u32pool, with(near(get32(o, e + 8)), {0, len - off, len - off + 1, 11, 12}))));
      break;
    }
    case 5: {  // a META length byte
      const uint32_t pos = L.metaStr[r.below(6)];
      put8(b, pos, pick(r, with({0, 1, 16, 17, 23, 24, 25, 40, 41, 80, 81, 255, L.size[kMeta]}, near(o[pos]))));
      break;
    }
    case 6: {  // META string bytes: the UTF-8 edges
      const uint32_t pos = L.metaStr[r.below(6)];
      const uint32_t n = o[pos];
      writeSeq(b, pos + 1 + (n ? r.below(n) : 0), utf8Edges()[r.below(static_cast<uint32_t>(utf8Edges().size()))]);
      break;
    }
    case 7:  // the declared move count
      put8(b, L.metaCount, pick(r, {0, 1, 63, 64, 65, 255}));
      break;
    case 8: {  // a GEOM field
      const uint32_t k = r.below(12), pos = L.off[kGeom] + k;
      const uint32_t fc = static_cast<uint32_t>(L.frames);
      if (k == 8 || k == 10)
        put16(b, pos, pick(r, with({0, 1, fc - 1, fc, fc + 1, 256, 257, 0x7FFF, 0x8000, 0x8001, 0xFFFF},
                                   {get16(o, pos) | 0x8000u})));
      else
        put8(b, pos, pick(r, with({0, 1, 2, 3, 4, 5, 10, 12, 38, 40, 80, 152, 153, 154, 160, 161, 255}, near(o[pos]))));
      break;
    }
    case 9: {  // PALS's or THMB's size
      const uint32_t e = 32 + 12 * (r.below(2) ? kPals : kThmb);
      put32(b, e + 8, pick(r, {0, 95, 96, 97, 799, 800, 801, 1284}));
      break;
    }
    case 10: {  // an FTAB box byte
      const uint32_t pos = L.off[kFtab] + 12 * r.below(L.frames) + r.below(4);
      put8(b, pos, pick(r, with({0, 1, L.cellW, L.cellW + 1u, L.cellH, L.cellH + 1u, 160, 161, 255}, near(o[pos]))));
      break;
    }
    case 11: {  // an FTAB data offset or length
      const uint32_t f = r.below(L.frames), rec = L.off[kFtab] + 12 * f, fd = L.size[kFdat];
      const uint32_t g = r.below(L.frames);
      if (r.below(2))
        put32(b, rec + 4, pick(r, with({0, 1, fd, fd - 1, fd + 1, 0xFFFFFFF0u, 0xFFFFFFFCu, L.ftabOff[g]}, near(L.ftabOff[f]))));
      else
        put16(b, rec + 8, pick(r, with({0, 1, fd, fd + 1, 0xFFFF, L.ftabLen[g]}, near(L.ftabLen[f]))));
      break;
    }
    case 12:
    case 13: {  // FDAT bytes: row heads and tokens
      const uint32_t f = r.below(L.frames);
      if (!L.fLen[f]) break;
      const uint32_t pos = L.fStart[f] + r.below(L.fLen[f]);
      const uint32_t w = o[L.off[kFtab] + 12 * f + 2];
      put8(b, pos, pick(r, with({0, 1, 0x0F, 0x10, 0xF0, 0xF1, 0xFF, w, w + 1, r.next() & 0xFF}, near(o[pos]))));
      break;
    }
    case 14: {  // STRS's P
      const uint32_t P = static_cast<uint32_t>(L.poseKeys);
      put8(b, L.off[kStrs], pick(r, {0, 1, P - 1, P + 1, P + 2, 255}));
      break;
    }
    case 15: {  // a STRS length byte
      if (L.strsStr.empty()) break;
      const uint32_t pos = L.strsStr[r.below(static_cast<uint32_t>(L.strsStr.size()))];
      put8(b, pos, pick(r, with({0, 1, 22, 23, 24, 62, 63, 64, 255}, near(o[pos]))));
      break;
    }
    case 16: {  // STRS string bytes
      if (L.strsStr.empty()) break;
      const uint32_t pos = L.strsStr[r.below(static_cast<uint32_t>(L.strsStr.size()))];
      const uint32_t n = o[pos];
      writeSeq(b, pos + 1 + (n ? r.below(n) : 0), utf8Edges()[r.below(static_cast<uint32_t>(utf8Edges().size()))]);
      break;
    }
    case 17: {  // the MOVS head
      const uint32_t m = static_cast<uint32_t>(L.moves);
      if (r.below(2)) put8(b, L.off[kMovs], pick(r, {0, 1, m - 1, m + 1, 64, 65, 255}));
      else put8(b, L.off[kMovs] + 1, pick(r, {0, 1, 31, 32, 33, 64, 255}));
      break;
    }
    case 18:
    case 19:
    case 20: {  // a MOVS record field: the name, steps, genres and keys most
      if (!L.moves) break;
      struct F {
        uint8_t off, width;
      };
      static const F fields[] = {{0, 2},  {0, 2},  {2, 1},  {3, 1},  {4, 1},  {5, 1},  {6, 1},  {7, 1},
                                 {8, 1},  {9, 1},  {10, 1}, {11, 1}, {12, 1}, {13, 1}, {14, 1}, {15, 1},
                                 {16, 1}, {16, 1}, {17, 1}, {17, 1}, {18, 1}, {19, 1}, {20, 2}, {20, 2},
                                 {22, 1}, {22, 1}, {23, 1}, {23, 1}, {24, 2}, {24, 2}, {26, 1}, {27, 1}};
      const F f = fields[r.below(sizeof fields / sizeof fields[0])];
      const uint32_t pos = L.off[kMovs] + 4 + 32 * r.below(L.moves) + f.off;
      const uint32_t P = static_cast<uint32_t>(L.poseKeys), S = L.size[kStrs], n = static_cast<uint32_t>(L.steps);
      if (f.width == 1) {
        put8(b, pos, pick(r, with({0, 1, 2, 3, 4, 5, 8, 9, 12, 16, 17, 32, 33, 35, 36, 64, 99, 100, 101, 127, 128,
                                   180, 181, 200, 201, 255, P, P + 1},
                                  near(o[pos]))));
      } else {
        std::vector<uint32_t> pool = {0, 1, 2, 3, 4, 5, 8, 9, S - 2, S - 1, S, S + 1, n - 1, n, n + 1, 0x7FFF, 0x8000, 0xFFFF};
        for (uint32_t s : L.strsStr) pool.push_back(s - L.off[kStrs]);
        put16(b, pos, pick(r, with(pool, near(get16(o, pos)))));
      }
      break;
    }
    case 21: {  // a STEP field
      if (!L.steps) break;
      const uint32_t s = L.off[kStep] + 8 * r.below(L.steps);
      const uint32_t fc = static_cast<uint32_t>(L.frames);
      switch (r.below(6)) {
        case 0:
          put16(b, s, pick(r, {0, 1, fc - 1, fc, fc + 1, 0x7FFF, 0x8000, 0x8000 | fc, get16(o, s) ^ 0x8000u, 0xFFFF}));
          break;
        case 1:
          put16(b, s + 2, pick(r, with({0, 1, 2, 3, 4, 12, 13, 19, 20, 500, 501, 0xFFFF}, near(get16(o, s + 2)))));
          break;
        case 2:
        case 3:
          put8(b, s + 4 + r.below(2), pick(r, {0, 1, 0xFF, 12, 13, 0xF4, 0xF3, 48, 49, 0xD0, 0xCF, 127, 128}));
          break;
        case 4:
          put8(b, s + 6, pick(r, {0, 4, 5, 255}));
          break;
        default:
          put8(b, s + 7, pick(r, {0, 1, 2, 0xFF}));
          break;
      }
      break;
    }
    default:
      break;
  }
}

void test_structure_aware_fuzz() {
  const Layout layouts[2] = {layoutOf(packedit::dotPack()), layoutOf(packedit::testPack())};
  Xorshift32 rng(0x4D555441u);
  Reader r;
  int accepted = 0, dropping = 0;
  for (int i = 0; i < 10000; ++i) {
    const Layout& L = layouts[rng.below(4) == 0 ? 1 : 0];
    Bytes b = L.base;
    const int edits = 1 + static_cast<int>(rng.below(3));
    for (int e = 0; e < edits; ++e) mutate(b, L, rng);
    ctx("mutation %d (%s base, %d edits)", i, &L == &layouts[0] ? "dot" : "test", edits);
    const bool ok = r.openAll(b, skipCrc());
    accepted += ok;
    dropping += ok && r.report.dropped > (&L == &layouts[1] ? 1 : 0);
    verify(r, &g_rng);
  }
  TEST_ASSERT_TRUE(accepted > 1000);
  TEST_ASSERT_TRUE(dropping > 300);
}

// ===========================================================================
// 10. ReaderVectors, in full.

// Step 3's reading (D.2, E.4), with the real code: DanceBudget's start-spot
// hook inside open(), through a counting wrapper that records each call (it
// must come exactly for the moves step 2 keeps, in MOVS order), then a
// BudgetPass run to its end, one kept move a step, as 5.7's loader does for
// a card pack. open()'s drops come in place; the budget's after them, in
// MOVS order; and the digest is unchanged.
struct SpotsWrapper {
  std::vector<int> calls;
  bool open = true;  // the Pack open and the move not yet kept, at every call
};

bool spotsWrapper(void* ctx, const dance::Pack& pack, int move, dance::Reason* why) {
  SpotsWrapper& w = *static_cast<SpotsWrapper*>(ctx);
  w.calls.push_back(move);
  if (pack.state() != dance::Pack::State::Open || pack.moveKept(move)) w.open = false;
  return dance::startSpots(nullptr, pack, move, why);
}

// A drop list's "move:text" pairs; budget: 0 every one, 1 all but `budget
// red`, 2 only `budget red`.
std::string dropList(uint32_t first, uint32_t count, int budget = 0) {
  std::string s;
  for (uint32_t k = 0; k < count; ++k) {
    const dancevec::ReaderDrop& d = dancevec::kReaderDrops[first + k];
    const bool red = std::strncmp(dancevec::kReaderReasons[d.reason], "budget red (", 12) == 0;
    if ((budget == 1 && red) || (budget == 2 && !red)) continue;
    if (!s.empty()) s += "|";
    s += std::to_string(d.move) + ":" + dancevec::kReaderReasons[d.reason];
  }
  return s;
}

void test_reader_vectors() {
  using namespace dancevec;
  TEST_ASSERT_TRUE(kReaderCasesCount > 0);
  Reader r;
  int openAccepted = 0, scanAccepted = 0, lateDrops = 0;
  for (int i = 0; i < kReaderCasesCount; ++i) {
    const ReaderCase& c = kReaderCases[i];
    const Bytes b = packedit::readerCase(i);
    TEST_ASSERT_EQUAL_size_t(c.length, b.size());
    ctx("ReaderVectors case %d (base %u, entry %u, length %u, avail %u)", i, c.base, c.entry, c.length, c.avail);
    bool ok;
    if (c.entry == kScan) ok = r.scan(b, c.avail, c.length);
    else if (c.entry == kOpenShort) ok = r.open(b, c.avail);
    else ok = r.open(b, c.length);
    if (c.reason == kAccepted) {
      if (!ok) ctx("ReaderVectors case %d (entry %u): rejected: %s", i, c.entry, r.reason);
      TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
    } else {
      TEST_ASSERT_FALSE_MESSAGE(ok, g_ctx);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kReaderReasons[c.reason], r.reason, g_ctx);
    }
    verify(r, &g_rng);
    if (!ok) continue;
    std::vector<bool> listed;
    if (c.entry == kScan) {
      ++scanAccepted;
    } else {
      ++openAccepted;
      // Step 2's drop list: the same moves, in order, with the same texts.
      std::string want;
      listed.assign(static_cast<size_t>(r.pack.moveCount()), false);
      for (uint32_t k = 0; k < c.dropCount; ++k) {
        const ReaderDrop& d = kReaderDrops[c.firstDrop + k];
        if (k) want += "|";
        want += std::to_string(d.move) + ":" + kReaderReasons[d.reason];
        if (d.move < listed.size()) listed[d.move] = true;
      }
      TEST_ASSERT_EQUAL_STRING_MESSAGE(want.c_str(), r.drops.str().c_str(), g_ctx);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(dropList(c.firstDrop, c.dropCount).c_str(), want.c_str(), g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(c.dropCount, r.report.dropped, g_ctx);
      for (int m = 0; m < r.pack.moveCount(); ++m) TEST_ASSERT_EQUAL_MESSAGE(!listed[m], r.pack.moveKept(m), g_ctx);
    }
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.digest, digestOf(r.pack, c.entry == kScan, listed), g_ctx);
    if (c.entry != kOpen) continue;
    // Again as a card pack (E.4): the real hook, then the budget pass.
    SpotsWrapper w;
    dance::OpenOptions o;
    o.late = &spotsWrapper;
    o.lateCtx = &w;
    ctx("ReaderVectors case %d with DanceBudget's hook and pass", i);
    TEST_ASSERT_TRUE_MESSAGE(r.open(b, c.length, &o), g_ctx);
    verify(r, &g_rng);
    // Called exactly for step 2's kept moves, in MOVS order.
    std::vector<int> want2;
    for (int m = 0; m < r.pack.moveCount(); ++m)
      if (!listed[m]) want2.push_back(m);
    TEST_ASSERT_TRUE_MESSAGE(w.calls == want2, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(w.open, g_ctx);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(dropList(c.firstFullDrop, c.fullDropCount, 1).c_str(), r.drops.str().c_str(),
                                     g_ctx);
    // The budget, one kept move a step after open() (5.7), through dropMove.
    r.drops.lastMove = -1;  // its drops come after open()'s, in MOVS order among themselves
    std::string order = r.drops.str();
    const std::string red = dropList(c.firstFullDrop, c.fullDropCount, 2);
    const int keptAfterOpen = r.pack.keptMoves();
    dance::BudgetPass pass;
    pass.begin(&r.pack, &r.report);
    int steps = 0;
    while (pass.step()) ++steps;
    TEST_ASSERT_TRUE_MESSAGE(pass.done(), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(keptAfterOpen, steps, g_ctx);  // each step checked one kept move
    if (!red.empty()) order += (order.empty() ? "" : "|") + red;
    verify(r, &g_rng);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(order.c_str(), r.drops.str().c_str(), g_ctx);
    TEST_ASSERT_FALSE_MESSAGE(r.drops.outOfOrder, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(c.fullDropCount, r.report.dropped, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.pack.moveCount() - static_cast<int>(c.fullDropCount), r.pack.keptMoves(), g_ctx);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.digest, digestOf(r.pack, false, listed), g_ctx);
    lateDrops += static_cast<int>(c.fullDropCount - c.dropCount);
  }
  // D.3's coverage: at least 500 opened and 100 scanned packs accepted, and
  // some full lists longer than step 2's (the anchors' two late rows).
  TEST_ASSERT_TRUE(openAccepted >= 500);
  TEST_ASSERT_TRUE(scanAccepted >= 100);
  TEST_ASSERT_TRUE(lateDrops >= 2);
}

// ===========================================================================
// Rules D.4's list doesn't name: added in integration.

bool countingHook(void* ctx, const dance::Pack&, int, dance::Reason*) {
  ++*static_cast<int*>(ctx);
  return true;
}

// D.1's report and state across calls on one Pack and one Report: every
// open() and scan() starts afresh (rejected back to None on success,
// dropped counted from 0, nothing of the previous pack left), a null report
// changes nothing but the reporting, and a rejected pack never reaches the
// late-check hook (D.2: it runs with the Pack open).
void test_reuse() {
  using namespace packedit;
  const Bytes t = testPack(), d = dotPack();
  const uint32_t ts = static_cast<uint32_t>(t.size()), ds = static_cast<uint32_t>(d.size());
  Bytes notMpdn = d;
  notMpdn[0] = 'X';
  const Entry step = entry(t, "STEP");
  const Bytes badStep = patch32(t, step.at + 8, step.size - 1);  // A.15's last pack row
  const std::string stepReason = "STEP: " + std::to_string(step.size - 1) + " bytes";
  HeapCopy tc(t.data(), t.size()), dc(d.data(), d.size()), nc(notMpdn.data(), notMpdn.size()),
      bc(badStep.data(), badStep.size());
  char text[dance::kReasonChars];

  dance::Pack p;
  dance::Report rep;
  Drops drops;
  drops.attach(rep);
  ctx("reuse: the fixture");
  TEST_ASSERT_TRUE(p.open(tc.data(), ts, ts, &rep));
  TEST_ASSERT_EQUAL_UINT8(1, rep.dropped);
  TEST_ASSERT_EQUAL_INT(1, drops.calls);
  TEST_ASSERT_EQUAL_INT(13, p.keptMoves());

  ctx("reuse: a rejection on the same Pack and Report");
  drops.reset();
  TEST_ASSERT_FALSE(p.open(nc.data(), ds, ds, &rep));
  dance::formatReason(rep.rejected, text, sizeof text);
  TEST_ASSERT_EQUAL_STRING("not an MPDN pack", text);
  TEST_ASSERT_EQUAL_UINT8(0, rep.dropped);
  TEST_ASSERT_EQUAL_INT(0, drops.calls);
  TEST_ASSERT_EQUAL_UINT16(0, p.regionFaults());
  expectNoneState(p);

  ctx("reuse: the dot after the rejection");
  TEST_ASSERT_TRUE(p.open(dc.data(), ds, ds, &rep));
  TEST_ASSERT_TRUE(rep.rejected.why == Why::None);
  TEST_ASSERT_EQUAL_UINT8(0, rep.dropped);
  TEST_ASSERT_EQUAL_STRING("dot", p.id());
  TEST_ASSERT_EQUAL_INT(1, p.moveCount());
  TEST_ASSERT_EQUAL_INT(1, p.frameCount());

  ctx("reuse: the fixture again, its drop counted once");
  drops.reset();
  TEST_ASSERT_TRUE(p.open(tc.data(), ts, ts, &rep));
  TEST_ASSERT_EQUAL_UINT8(1, rep.dropped);
  TEST_ASSERT_EQUAL_INT(1, drops.calls);

  ctx("reuse: a scan after an open");
  TEST_ASSERT_TRUE(p.scan(tc.data(), 2048, ts, &rep));
  TEST_ASSERT_TRUE(rep.rejected.why == Why::None);
  TEST_ASSERT_TRUE(p.state() == dance::Pack::State::Scanned);
  expectNoOpenAccessors(p);
  TEST_ASSERT_EQUAL_STRING("test", p.id());

  ctx("reuse: a failed scan after a good one");
  TEST_ASSERT_FALSE(p.scan(tc.data(), 139, ts, &rep));
  dance::formatReason(rep.rejected, text, sizeof text);
  TEST_ASSERT_EQUAL_STRING("header unreadable", text);
  expectNoneState(p);

  ctx("reuse: no report");
  TEST_ASSERT_TRUE(p.open(tc.data(), ts, ts));
  TEST_ASSERT_EQUAL_INT(13, p.keptMoves());
  TEST_ASSERT_FALSE(p.moveKept(8));
  TEST_ASSERT_TRUE(p.moveKept(9));
  HookLog log;
  dance::OpenOptions o;
  o.late = &stubLate;
  o.lateCtx = &log;
  TEST_ASSERT_TRUE(p.open(tc.data(), ts, ts, nullptr, &o));
  TEST_ASSERT_EQUAL_INT(13, log.calls);
  TEST_ASSERT_EQUAL_INT(11, p.keptMoves());
  TEST_ASSERT_FALSE(p.moveKept(2));
  TEST_ASSERT_FALSE(p.moveKept(9));
  TEST_ASSERT_FALSE(p.open(nc.data(), ds, ds));
  expectNoneState(p);
  TEST_ASSERT_TRUE(p.scan(dc.data(), ds, ds));
  TEST_ASSERT_EQUAL_STRING("dot", p.id());

  ctx("reuse: a rejected pack never reaches the hook");
  int calls = 0;
  o.late = &countingHook;
  o.lateCtx = &calls;
  drops.reset();
  TEST_ASSERT_FALSE(p.open(bc.data(), ts, ts, &rep, &o));
  dance::formatReason(rep.rejected, text, sizeof text);
  TEST_ASSERT_EQUAL_STRING(stepReason.c_str(), text);
  TEST_ASSERT_EQUAL_INT(0, calls);
  TEST_ASSERT_EQUAL_INT(0, drops.calls);
  TEST_ASSERT_EQUAL_UINT16(0, p.regionFaults());
  expectNoneState(p);
  TEST_ASSERT_TRUE(p.open(tc.data(), ts, ts, &rep, &o));
  TEST_ASSERT_EQUAL_INT(13, calls);  // every move but 8, which an earlier row dropped
}

// D.1: every multi-byte field is read a byte at a time, never through a cast
// pointer, so a pack can start at any address (under native_asan, UBSan
// checks alignment). Both bases, and every fourth ReaderVectors case, opened
// or scanned 1-3 bytes into an exact-size block, say what they say at
// offset 0: the same reason, drops and digest.
void test_any_address() {
  using namespace dancevec;
  for (const Bytes& base : {packedit::dotPack(), packedit::testPack()}) {
    const uint32_t size = static_cast<uint32_t>(base.size());
    Reader at0, scan0;
    ctx("any address: the base at offset 0");
    TEST_ASSERT_TRUE(at0.openAll(base));
    TEST_ASSERT_TRUE(scan0.scan(base, 2048, size));
    std::vector<bool> listed(static_cast<size_t>(at0.pack.moveCount()), false);
    for (int m = 0; m < at0.pack.moveCount(); ++m) listed[m] = !at0.pack.moveKept(m);
    const uint32_t opened = digestOf(at0.pack, false, listed), scanned = digestOf(scan0.pack, true, {});
    for (size_t shift = 1; shift <= 3; ++shift) {
      Reader r;
      ctx("any address: the base %u bytes in", static_cast<unsigned>(shift));
      TEST_ASSERT_TRUE_MESSAGE(r.openAt(base, shift), g_ctx);
      verify(r, &g_rng);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(at0.drops.str().c_str(), r.drops.str().c_str(), g_ctx);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(opened, digestOf(r.pack, false, listed), g_ctx);
      TEST_ASSERT_TRUE_MESSAGE(r.scanAt(base, 2048, size, shift), g_ctx);
      verify(r, &g_rng);
      TEST_ASSERT_EQUAL_HEX32_MESSAGE(scanned, digestOf(r.pack, true, {}), g_ctx);
    }
  }
  int checked = 0;
  for (int i = 0; i < kReaderCasesCount; i += 4) {
    const ReaderCase& c = kReaderCases[i];
    if (c.entry == kOpenShort) continue;  // rejected before a byte is read
    const Bytes b = packedit::readerCase(i);
    const size_t shift = 1 + static_cast<size_t>(i / 4) % 3;
    ctx("any address: ReaderVectors case %d, %u bytes in", i, static_cast<unsigned>(shift));
    Reader r;
    const bool ok = c.entry == kScan ? r.scanAt(b, c.avail, c.length, shift) : r.openAt(b, shift);
    verify(r, &g_rng);
    ++checked;
    if (c.reason != kAccepted) {
      TEST_ASSERT_FALSE_MESSAGE(ok, g_ctx);
      TEST_ASSERT_EQUAL_STRING_MESSAGE(kReaderReasons[c.reason], r.reason, g_ctx);
      continue;
    }
    TEST_ASSERT_TRUE_MESSAGE(ok, g_ctx);
    std::vector<bool> listed;
    if (c.entry == kOpen) {
      listed.assign(static_cast<size_t>(r.pack.moveCount()), false);
      for (uint32_t k = 0; k < c.dropCount; ++k) {
        const ReaderDrop& d = kReaderDrops[c.firstDrop + k];
        if (d.move < listed.size()) listed[d.move] = true;
      }
      TEST_ASSERT_EQUAL_STRING_MESSAGE(dropList(c.firstDrop, c.dropCount).c_str(), r.drops.str().c_str(), g_ctx);
    }
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(c.digest, digestOf(r.pack, c.entry == kScan, listed), g_ctx);
  }
  TEST_ASSERT_TRUE(checked > 500);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_crc32);
  RUN_TEST(test_fnv1a32);
  RUN_TEST(test_fold_vectors);
  RUN_TEST(test_fold_malformed);
  RUN_TEST(test_valid_text);
  RUN_TEST(test_reach);
  RUN_TEST(test_format_reason);
  RUN_TEST(test_sizes);
  RUN_TEST(test_fixture_header);
  RUN_TEST(test_fixture_moves);
  RUN_TEST(test_dot_pack);
  RUN_TEST(test_rejections);
  RUN_TEST(test_order);
  RUN_TEST(test_accepted);
  RUN_TEST(test_accepted_token_sequences);
  RUN_TEST(test_drops);
  RUN_TEST(test_drops_rebuilt);
  RUN_TEST(test_limits);
  RUN_TEST(test_strings_read_inside_strs);
  RUN_TEST(test_step3_rows_wait_for_step3);
  RUN_TEST(test_late_check_hook);
  RUN_TEST(test_drop_move);
  RUN_TEST(test_short_reads);
  RUN_TEST(test_scan_rows);
  RUN_TEST(test_scan_fixture);
  RUN_TEST(test_truncations);
  RUN_TEST(test_flips);
  RUN_TEST(test_structure_aware_fuzz);
  RUN_TEST(test_reader_vectors);
  RUN_TEST(test_reuse);
  RUN_TEST(test_any_address);
  return UNITY_END();
}
