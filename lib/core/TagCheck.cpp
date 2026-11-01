// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagCheck.h"

#include "ScanScheduler.h"

namespace tagcheck {

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
using cc::Why;

bool waitOver(bool workerBusy, uint32_t startMs, uint32_t nowMs, uint32_t maxMs) {
  if (!workerBusy) return true;
  const int32_t passed = static_cast<int32_t>(nowMs - startMs);
  return passed >= 0 && static_cast<uint32_t>(passed) >= maxMs;
}

bool pieceWaits(const Pass& p) { return ScanScheduler::listHolds(p.listMoving, p.dark); }

void Turn::opened(const Pass& p) {
  reading_ = true;
  ended_ = false;
  compactionsAt_ = p.compactions;
  remountsAt_ = p.remounts;
}

Turn::Now Turn::pass(const Pass& p, Check& check) {
  if (gaveUp_) return Now::GaveUp;
  // (T's turn: no compaction replaces it, none is waited for.)
  const bool replacing = compacted_ && p.replacing;
  if (reading_) {
    if (!ended_) {
      // (Its answer judged at the next pass, by what the loop says after
      // the last read: `p` was taken before this pass's reads.)
      if (check.step() == Check::Step::Done) ended_ = true;
      return Now::Working;
    }
    reading_ = ended_ = false;
    // Its answer stands: no compaction under way now (none was at the
    // open), none handed meanwhile, and the card not mounted again (the
    // handle's reads would have failed).
    const bool remounted = p.remounts != remountsAt_;
    const bool compacted = compacted_ && p.compactions != compactionsAt_;
    if (!replacing && !compacted && !remounted) return Now::Done;
    remounted_ = remounted;
    if (++rereads_ > kRereads) {
      gaveUp_ = true;
      return Now::GaveUp;
    }
    return Now::Again;
  }
  if (!replacing) {
    waiting_ = false;
    return Now::Open;
  }
  if (!waiting_) {
    waiting_ = true;
    waitMs_ = p.nowMs;
    return Now::Said;
  }
  // (A pass's time before the wait's own is no time passed.)
  const int32_t waited = static_cast<int32_t>(p.nowMs - waitMs_);
  if (waited >= 0 && static_cast<uint32_t>(waited) >= kReplaceWaitMs) {
    gaveUp_ = waitedOut_ = true;
    return Now::GaveUp;
  }
  return Now::Wait;
}

void Gs::begin(uint32_t startMs, uint32_t waitMs, GsIo& io) {
  phase_ = Phase::Wait;
  startMs_ = startMs;
  waitMs_ = waitMs;
  // The worker's step under way first, nothing handed after it.
  io.hold(Hold::Worker);
}

void Gs::step(const Pass& p, GsIo& io) {
  switch (phase_) {
    case Phase::Idle: return;
    case Phase::Wait:
      // The worker's step under way taken in first (as CardTasks::
      // waitIdle()'s wait: waitMs_ at most, then on regardless).
      if (!waitOver(p.workerBusy, startMs_, p.nowMs, waitMs_)) return;
      io.firstLines();
      // The reads: the worker goes on, but for a new compaction.
      io.hold(Hold::Records);
      if (!io.memory()) {
        end(io);
        return;
      }
      // D, its rows by status, its journals: its turn from the next pass.
      turn_.begin(true);
      phase_ = Phase::Device;
      return;
    case Phase::Device:
      if (file(p, true, io)) phase_ = Phase::Journals;
      return;
    case Phase::Journals:
      io.journals();
      phase_ = Phase::Root;
      return;
    case Phase::Root:
      // T: the root, then its tags file's turn from the next pass.
      if (io.root()) {
        turn_.begin(false);
        phase_ = Phase::Transfer;
      } else {
        phase_ = Phase::Plan;
      }
      return;
    case Phase::Transfer:
      if (file(p, false, io)) phase_ = Phase::Plan;
      return;
    case Phase::Plan:
      io.plan();
      end(io);
      return;
  }
}

bool Gs::file(const Pass& p, bool device, GsIo& io) {
  using Now = Turn::Now;
  switch (turn_.pass(p, io.check())) {
    case Now::Wait:
    case Now::Working: return false;
    case Now::Said:
      io.waiting(device);
      return false;
    case Now::Open:
      if (io.open(device)) {
        turn_.opened(p);
        return false;
      }
      io.lines(device, false);  // not there
      return true;
    case Now::Again:
      io.close();
      io.again(device, turn_);
      return false;
    case Now::GaveUp:
      io.close();
      io.notRead(device, turn_);
      return true;
    case Now::Done:
      io.close();
      io.lines(device, true);
      return true;
  }
  return true;
}

void Gs::end(GsIo& io) {
  io.end();
  phase_ = Phase::Idle;
  io.hold(Hold::None);
}

void Check::begin(cc::Source& src, bool device, const Memory& m, const Config& c) {
  src_ = &src;
  m_ = m;
  c_ = c;
  isDevice_ = device;
  why_ = Why::Ok;
  valid_ = false;
  device_ = tagstore::DeviceInfo();
  for (uint32_t& n : rows_) n = 0;
  units_ = steps_ = 0;
  phase_ = device ? Phase::Header : Phase::Begin;
  if (!m_.walker || !m_.file || (device && !m_.rows)) {
    why_ = Why::Io;
    phase_ = Phase::Done;
  }
}

Check::Step Check::step() {
  if (phase_ == Phase::Done) return Step::Done;
  ++steps_;
  sliceAtUs_ = unitAtUs_ = c_.nowUs ? c_.nowUs() : 0;
  for (;;) {
    unit();
    ++units_;
    if (phase_ == Phase::Done) return Step::Done;
    if (!goesOn()) return Step::Working;
  }
}

// After a unit: another one in this step while the last one's time, from
// now, still fits in the slice (CardJobs::sliceGoesOn()'s rule: a slice of
// steady units ends at sliceUs, not a unit past it).
bool Check::goesOn() {
  if (!c_.nowUs || c_.sliceUs == 0) return false;
  const uint64_t now = c_.nowUs();
  const uint64_t unit = now - unitAtUs_;
  unitAtUs_ = now;
  return now - sliceAtUs_ + unit <= c_.sliceUs;
}

// One unit, in the order the one-call check took them (app/TagConsole's
// find(), then status()'s rows).
void Check::unit() {
  switch (phase_) {
    case Phase::Header:
      why_ = tagstore::openDevice(*src_, &device_);
      phase_ = why_ == Why::Ok ? Phase::Begin : Phase::Done;
      return;
    case Phase::Begin:
      why_ = m_.walker->begin(*src_, mptg::kUseHidx, m_.scratch, m_.scratchBytes);
      if (why_ != Why::Ok) {
        phase_ = Phase::Done;
        return;
      }
      m_.walker->setEndPiece(c_.hidxPiece);
      phase_ = Phase::Walk;
      return;
    case Phase::Walk:
      switch (m_.walker->next()) {
        case mptg::Walker::Step::End: phase_ = Phase::Open; return;
        case mptg::Walker::Step::Bad:
          why_ = m_.walker->why();
          phase_ = Phase::Done;
          return;
        default: return;  // a folder, a record, a piece of HIDX
      }
    case Phase::Open:
      why_ = m_.file->open(*src_);
      valid_ = why_ == Why::Ok;
      phase_ = valid_ && isDevice_ ? Phase::RowsBegin : Phase::Done;
      return;
    case Phase::RowsBegin:
      phase_ = m_.rows->begin(*src_, m_.buf, m_.bufBytes) ? Phase::Rows : Phase::Done;
      return;
    case Phase::Rows:
      if (!m_.rows->step(c_.rowsPiece)) return;
      for (int i = 0; i < 4; ++i) rows_[i] = m_.rows->counts()[i];
      phase_ = Phase::Done;
      return;
    case Phase::Done: return;
  }
}

}  // namespace tagcheck
