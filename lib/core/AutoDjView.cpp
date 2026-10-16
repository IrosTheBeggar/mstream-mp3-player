// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjView.h"

namespace autodjview {

using uistr::S;

const char* indexName(Index i) {
  switch (i) {
    case Index::None: return "none";
    case Index::Bad: return "failed its checks";
    case Index::Unreadable: return "unreadable";
    case Index::TooBig: return "a library too big";
    case Index::NoMemory: return "no memory to join";
    case Index::Found: return "found (joined when on)";
    case Index::Reading: return "joining";
    case Index::Ready: return "ready";
  }
  return "?";
}

RowState rowState(const State& s) {
  if (!s.on) return RowState::Off;
  switch (s.hold) {
    case autodj::Hold::Failures: return RowState::Paused;
    case autodj::Hold::RepeatOne:
    case autodj::Hold::SleepTimer: return RowState::Waiting;
    case autodj::Hold::None: break;
  }
  return RowState::On;
}

S rowText(RowState r) {
  switch (r) {
    case RowState::On: return S::now_playing_autodj_on;
    case RowState::Waiting: return S::now_playing_autodj_waiting;
    case RowState::Paused: return S::now_playing_autodj_paused;
    case RowState::Off: break;
  }
  return S::now_playing_autodj_off;
}

S modeText(autodj::Mode m) { return m == autodj::Mode::Locked ? S::autodj_anchor_locked : S::autodj_anchor_rolling; }

char* rootLine(const State& s, char* buf, size_t size, bool withMode) {
  if (!buf || size == 0) return buf;
  const RowState r = rowState(s);
  if (r == RowState::Off) {
    uistr::trf(buf, size, S::output_autodj_sub_off);
    return buf;
  }
  if (!withMode || !s.usable) {
    uistr::trf(buf, size, rowText(r));
    return buf;
  }
  const char* mode = uistr::tr(modeText(s.mode));
  const S line = r == RowState::Waiting  ? S::output_autodj_sub_waiting
                 : r == RowState::Paused ? S::output_autodj_sub_paused
                                         : S::output_autodj_sub_on;
  uistr::trf(buf, size, line, mode);
  return buf;
}

bool rowLive(PageRow r, const State& s) { return r == OnRow || r == StatusRow || s.usable; }

uint8_t nextFloor(uint8_t pct) {
  for (uint8_t p = autodj::kFloorMin; p <= autodj::kFloorMax; p = static_cast<uint8_t>(p + autodj::kFloorStep)) {
    if (p > pct) return p;
  }
  return autodj::kFloorMin;
}

uint8_t tapFloor(const State& s) { return nextFloor(s.floorShown > s.floorPct ? s.floorShown : s.floorPct); }

uint8_t shownFloor(uint8_t pct, uint32_t selFloor) {
  // floorLevel(q) = (255 q + 99) div 100 >= selFloor  <=>  q >= (100 selFloor - 99) / 255.
  if (selFloor == 0 || autodj::floorLevel(pct, selFloor) == autodj::floorLevel(pct, 0)) return pct;
  const uint32_t need = selFloor * 100 > 99 ? (selFloor * 100 - 99 + 254) / 255 : 0;
  return need > pct ? static_cast<uint8_t>(need > 100 ? 100 : need) : pct;
}

char* strictnessText(uint8_t pct, char* buf, size_t size) {
  if (buf && size) uistr::trf(buf, size, S::autodj_strictness_value, pct);
  return buf;
}

S anchorHint(autodj::Mode m) {
  return m == autodj::Mode::Locked ? S::autodj_anchor_locked_sub : S::autodj_anchor_rolling_sub;
}

namespace {

unsigned clampPct(uint8_t p) { return p > 100 ? 100u : p; }

// The index's lines (the status row's, unheld).
void indexLines(const State& s, char* l1, size_t n1, char* l2, size_t n2) {
  l2[0] = 0;
  switch (s.index) {
    case Index::Ready:
      uistr::trnf(l1, n1, S::autodj_status_ready, s.tracks, uistr::grouped(s.mapped), uistr::grouped(s.tracks));
      uistr::trf(l2, n2, S::autodj_status_coverage, clampPct(s.bpmPct), clampPct(s.keyPct));
      return;
    case Index::Found:
      uistr::trnf(l1, n1, S::autodj_status_found, s.rows, uistr::grouped(s.rows));
      uistr::trf(l2, n2, S::autodj_status_found_sub);
      return;
    case Index::Reading: uistr::trf(l1, n1, S::autodj_status_reading); return;
    case Index::Bad: uistr::trf(l1, n1, S::autodj_status_bad); break;
    case Index::Unreadable: uistr::trf(l1, n1, S::autodj_status_unreadable); break;
    case Index::TooBig: uistr::trf(l1, n1, S::autodj_status_too_big); break;
    case Index::NoMemory: uistr::trf(l1, n1, S::autodj_status_no_memory); break;
    case Index::None: uistr::trf(l1, n1, S::autodj_status_none); break;
  }
  uistr::trf(l2, n2, S::autodj_status_random);
}

}  // namespace

void statusLines(const State& s, char* line1, size_t size1, char* line2, size_t size2) {
  if (!line1 || !line2 || size1 == 0 || size2 == 0) return;
  line1[0] = line2[0] = 0;
  const RowState r = rowState(s);
  if (r != RowState::Waiting && r != RowState::Paused) {
    indexLines(s, line1, size1, line2, size2);
    return;
  }
  // Why it adds nothing, over the index's first line.
  const S why = s.hold == autodj::Hold::Failures    ? S::autodj_status_paused
                : s.hold == autodj::Hold::RepeatOne ? S::autodj_status_waits_repeat
                                                    : S::autodj_status_waits_sleep;
  uistr::trf(line1, size1, why);
  char spare[8];
  indexLines(s, line2, size2, spare, sizeof(spare));
}

bool emptyButton(const State& s, uint32_t libraryTracks) { return s.usable && libraryTracks > 0; }

uint8_t NewsWatch::switchedOn(bool usable) {
  randomSaid_ = !usable;
  return usable ? 0 : kNewsRandom;
}

uint8_t NewsWatch::pass(const autodj::Pass& p, autodj::Mode mode, bool on) {
  uint8_t news = 0;
  if (p.added) ++adds_;
  if (p.cued) news |= kNewsCued;
  if (p.paused && on) news |= kNewsPaused;
  if (p.picked && p.pick.ok && p.added) {  // (a cued pick is appended too)
    if (p.pick.noIndex) {
      if (!randomSaid_) news |= kNewsRandom;
      randomSaid_ = true;
    } else if (p.pick.random && picks_ == 0) {
      news |= mode == autodj::Mode::Locked ? kNewsSeedMoved : kNewsNoNeighbours;
    }
    ++picks_;
  }
  return news;
}

bool newsToast(uint8_t news, Toast* out) {
  if (!out) return false;
  if (news & kNewsPaused) {
    *out = Toast{S::toast_autodj_paused, true};
  } else if (news & kNewsCued) {
    *out = Toast{S::toast_autodj_cued, false};
  } else if (news & kNewsSeedMoved) {
    *out = Toast{S::toast_autodj_seed_moved, false};
  } else if (news & kNewsNoNeighbours) {
    *out = Toast{S::toast_autodj_no_neighbours, false};
  } else if (news & kNewsRandom) {
    *out = Toast{S::toast_autodj_random, false};
  } else {
    return false;
  }
  return true;
}

}  // namespace autodjview
