// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TagCheck.h"

namespace tagcheck {

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;
using cc::Why;

bool waitOver(bool workerBusy, uint32_t startMs, uint32_t nowMs, uint32_t maxMs) {
  if (!workerBusy) return true;
  const int32_t passed = static_cast<int32_t>(nowMs - startMs);
  return passed >= 0 && static_cast<uint32_t>(passed) >= maxMs;
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
