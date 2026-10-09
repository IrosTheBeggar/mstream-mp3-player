// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceBudget.h"

#include <algorithm>

namespace dance {

namespace {

constexpr uint8_t kFps[2] = {24, 30};

// dancerate::periodMs: whole ms, rounded (42 at 24 fps, 33 at 30).
int64_t periodMs(int fps) { return fps > 0 ? (1000 + fps / 2) / fps : 0; }

// B.6's cases of a move whose steps m and record r are read already.
int caseSpecs(const Pack& pack, const MoveSteps& m, const MoveRecord& r, BudgetCaseSpec out[kBudgetMaxCases]) {
  if (!m.ok) return 0;
  uint8_t paces[3];
  int paceCount = 0;
  if (r.kind == 1) {
    paces[paceCount++] = 0;  // an idle move: the one pace 0
  } else {
    int rlo = 0, rhi = 0;
    if (reach(r.paceLo, r.paceHi, r.beats, r.travel != 0, &rlo, &rhi)) {
      // rlo, (rlo + rhi) // 2 and rhi, ascending, a repeat dropped.
      const int want[3] = {rlo, (rlo + rhi) / 2, rhi};
      for (int w : want)
        if (paceCount == 0 || paces[paceCount - 1] != w) paces[paceCount++] = static_cast<uint8_t>(w);
    }
  }
  const bool travels = r.kind == 0 && r.travel != 0;
  const int orients = (r.flags & kMirror) ? 2 : 1;
  uint8_t masks[2] = {0b010, 0b010};  // a move without travel: the centre
  if (travels)
    for (int o = 0; o < orients; ++o) masks[o] = startSpotMask(pack, m, o == 1);
  int n = 0;
  for (uint8_t fps : kFps) {
    for (int p = 0; p < paceCount; ++p) {
      for (int o = 0; o < orients; ++o) {
        for (int s = -1; s <= 1; ++s) {
          if (!(masks[o] & (1u << (s + 1))) || n >= kBudgetMaxCases) continue;
          out[n].fps = fps;
          out[n].pace = paces[p];
          out[n].mirrored = o == 1;
          out[n].spot = static_cast<int8_t>(s);
          ++n;
        }
      }
    }
  }
  return n;
}

// One case over steps read already: N, the lower median (with a scratch) and the peak.
BudgetCaseResult runCase(const Pack& pack, const MoveSteps& m, const BudgetCaseSpec& spec, uint32_t* scratch) {
  BudgetCaseResult res = {0, 0, 0};
  BudgetCursor cursor;
  cursor.begin(m, spec);
  BudgetFrame f;
  uint32_t frames = 0;
  int stored = 0;
  bool first = true;
  while (cursor.next(pack, m, &f)) {
    if (first) {  // frame 0 has no last frame: B.6 counts n = 1..N
      first = false;
      continue;
    }
    ++frames;
    if (f.dirty > res.peak) res.peak = f.dirty;
    if (scratch && stored < kBudgetMaxFrames) scratch[stored++] = f.dirty;
  }
  res.frames = static_cast<uint16_t>(frames);
  if (scratch && stored > 0) {
    // The lower median: element (N - 1) // 2 of the values sorted ascending.
    uint32_t* mid = scratch + (stored - 1) / 2;
    std::nth_element(scratch, mid, scratch + stored);
    res.median = *mid;
  }
  return res;
}

}  // namespace

BudgetLight budgetLight(uint32_t peak) {
  if (peak <= kGreenPx) return BudgetLight::Green;
  if (peak <= kAmberPx) return BudgetLight::Amber;
  return BudgetLight::Red;
}

int budgetCaseSpecs(const Pack& pack, int move, BudgetCaseSpec out[kBudgetMaxCases]) {
  MoveSteps m;
  if (!readMoveSteps(pack, move, &m)) return 0;
  return caseSpecs(pack, m, pack.moveRecord(move), out);
}

// ---- one case ------------------------------------------------------------------

void BudgetCursor::begin(const MoveSteps& m, const BudgetCaseSpec& spec) {
  spec_ = spec;
  pos16_ = 0;
  rem_ = 0;
  n_ = 0;
  prev_ = StageRect();
  done_ = true;
  if (!m.ok) return;                         // a record that doesn't read: no frames
  if (m.kind == 0 && spec.pace == 0) return;  // a beat move needs a pace
  const int64_t period = periodMs(spec.fps);
  if (period <= 0) return;
  // B.6: frame n is at n * period ms; a pace of P BPM is P/5000 ticks a ms,
  // so pos16(n) = n * period * P * 65536 // 5000 (idle: n * period * 65536 // 10).
  add_ = static_cast<uint64_t>(period) * 65536u * (m.kind == 0 ? spec.pace : 1u);
  div_ = m.kind == 0 ? 5000u : 10u;
  end_ = static_cast<int64_t>(m.total) * 65536;
  done_ = false;
}

bool BudgetCursor::next(const Pack& pack, const MoveSteps& m, BudgetFrame* out) {
  if (done_) return false;
  const int64_t pos16 = pos16_;
  const Placement place = placeSteps(pack, m, pos16, spec_.mirrored, spec_.spot, 65536);
  BudgetFrame f;
  f.n = static_cast<uint16_t>(n_);
  f.pos16 = static_cast<uint32_t>(pos16);
  f.place = place;
  f.dirty = n_ == 0 ? 0 : dirtyPx(&prev_, place.rect);
  prev_ = place.rect;
  if (n_ >= 1 && pos16 >= end_) done_ = true;  // one play, plus the frame that crosses its end
  ++n_;
  // pos16(n + 1) from pos16(n): the quotient and remainder of n * add // div,
  // in 32 bits whenever they fit (always at 24 and 30 fps).
  const uint64_t s = rem_ + add_;
  if ((s >> 32) == 0) {
    const uint32_t s32 = static_cast<uint32_t>(s), q = s32 / div_;
    pos16_ += q;
    rem_ = s32 - q * div_;
  } else {
    pos16_ += static_cast<int64_t>(s / div_);
    rem_ = s % div_;
  }
  if (out) *out = f;
  return true;
}

BudgetRun::BudgetRun(const Pack& pack, int move, const BudgetCaseSpec& spec) : pack_(&pack) {
  readMoveSteps(pack, move, &steps_);
  cursor_.begin(steps_, spec);
}

bool BudgetRun::next(BudgetFrame* out) { return cursor_.next(*pack_, steps_, out); }

BudgetCaseResult budgetCase(const Pack& pack, int move, const BudgetCaseSpec& spec, uint32_t* scratch) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);
  return runCase(pack, m, spec, scratch);
}

BudgetResult budget(const Pack& pack, int move, uint32_t* scratch) {
  BudgetResult res = {0, 0, BudgetLight::Green, 0};
  MoveSteps m;
  BudgetCaseSpec specs[kBudgetMaxCases];
  const int n = readMoveSteps(pack, move, &m) ? caseSpecs(pack, m, pack.moveRecord(move), specs) : 0;
  for (int i = 0; i < n; ++i) {
    const BudgetCaseResult c = runCase(pack, m, specs[i], scratch);
    if (c.median > res.median) res.median = c.median;
    if (c.peak > res.peak) res.peak = c.peak;
  }
  res.light = budgetLight(res.peak);
  res.cases = static_cast<uint8_t>(n);
  return res;
}

// ---- the verdicts ----------------------------------------------------------------

bool startSpots(void* /*ctx*/, const Pack& pack, int move, Reason* why) {
  MoveSteps m;
  readMoveSteps(pack, move, &m);  // unread: kind 0, travel 0, so kept (its start spots are all three)
  if (m.kind != 0 || m.travel == 0) return true;
  if (startSpotMask(pack, m, false) != 0) return true;
  if ((m.flags & kMirror) && startSpotMask(pack, m, true) != 0) return true;
  if (why) {
    *why = Reason();
    why->why = Why::TravelsFromNoSpot;
  }
  return false;
}

bool budgetRed(const Pack& pack, int move, Reason* why) {
  if (pack.state() != Pack::State::Open || move < 0 || move >= pack.moveCount()) return false;
  const MoveRecord r = pack.moveRecord(move);
  if (r.flags & kHeavy) return false;  // kept, whatever its light: no simulation
  const BudgetResult b = budget(pack, move, nullptr);
  if (b.light != BudgetLight::Red) return false;
  if (why) {
    *why = Reason();
    why->why = Why::BudgetRed;
    why->a = b.peak;
  }
  return true;
}

// ---- the pass ----------------------------------------------------------------------

void BudgetPass::begin(Pack* pack, Report* report) {
  pack_ = pack;
  report_ = report;
  next_ = 0;
  move_ = -1;
  cases_ = case_ = 0;
  inCase_ = false;
  peak_ = 0;
}

int BudgetPass::nextKept() const {
  if (!pack_ || pack_->state() != Pack::State::Open) return -1;
  for (int m = next_; m < pack_->moveCount(); ++m)
    if (pack_->moveKept(m)) return m;
  return -1;
}

bool BudgetPass::finish() {
  const int m = move_;
  move_ = -1;
  inCase_ = false;
  if (budgetLight(peak_) == BudgetLight::Red) {  // budgetRed's verdict and reason
    Reason why;
    why.why = Why::BudgetRed;
    why.a = peak_;
    pack_->dropMove(m, why, report_);
  }
  return true;
}

bool BudgetPass::run(uint32_t maxFrames) {
  if (!pack_ || pack_->state() != Pack::State::Open) {
    move_ = -1;
    return false;
  }
  if (maxFrames < 1) maxFrames = 1;
  if (move_ < 0) {
    const int m = nextKept();
    if (m < 0) return false;
    next_ = m + 1;
    const MoveRecord r = pack_->moveRecord(m);
    if (r.flags & kHeavy) return true;  // kept, whatever its light: no simulation
    move_ = m;
    peak_ = 0;
    case_ = 0;
    inCase_ = false;
    cases_ = static_cast<uint8_t>(readMoveSteps(*pack_, m, &steps_) ? caseSpecs(*pack_, steps_, r, specs_) : 0);
  }
  uint32_t frames = 0;
  for (;;) {
    if (!inCase_) {
      if (case_ >= cases_) return finish();
      cursor_.begin(steps_, specs_[case_]);
      inCase_ = true;
    }
    if (frames >= maxFrames) return true;
    BudgetFrame f;
    if (!cursor_.next(*pack_, steps_, &f)) {
      inCase_ = false;
      ++case_;
      continue;
    }
    ++frames;
    if (f.dirty > peak_) peak_ = f.dirty;  // frame 0's dirty is 0
  }
}

bool BudgetPass::step() { return run(UINT32_MAX); }

bool BudgetPass::done() const {
  if (!pack_ || pack_->state() != Pack::State::Open) return true;
  return move_ < 0 && nextKept() < 0;
}

}  // namespace dance
