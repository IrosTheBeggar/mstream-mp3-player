// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjView.h"

#include <algorithm>

#include "FilterGrid.h"
#include "MoodFilter.h"

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
    case Index::Reading: return "reading";
    case Index::Ready: return "ready";
  }
  return "?";
}

RowState rowState(const State& s) {
  if (!s.on) return RowState::Off;
  switch (s.hold) {
    case autodj::Hold::Failures: return RowState::Paused;
    case autodj::Hold::RepeatOne:
    case autodj::Hold::SleepTimer:
    case autodj::Hold::NoFit: return RowState::Waiting;
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

Ink sheetDetail(const State& s, char* buf, size_t size) {
  if (!buf || size == 0) return Ink::Dim;
  const RowState r = rowState(s);
  if (r == RowState::On && s.moodOn) {
    // What is set after "On · ", whole words in what the frame leaves.
    char what[filtergrid::kSummaryBytes];
    const size_t frame = uistr::trf(buf, size, S::output_autodj_sub_on, "");
    filtergrid::summary(moodfilter::Filter::unpack(s.moods), what, std::min(sizeof(what), size - frame));
    if (what[0]) {
      uistr::trf(buf, size, S::output_autodj_sub_on, what);
    } else {
      uistr::trf(buf, size, rowText(r));  // (not a word's room: the state alone)
    }
    return Ink::Accent;
  }
  uistr::trf(buf, size, rowText(r));
  return r == RowState::Waiting && s.hold == autodj::Hold::NoFit ? Ink::Amber : Ink::Dim;
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

bool rowLive(PageRow r, const State& s) {
  if (r == FilterRow) return s.moodData;
  return r == OnRow || r == StatusRow || s.usable;
}

char* filterValue(const State& s, char* buf, size_t size) {
  if (!buf || size == 0) return buf;
  const moodfilter::Filter f = moodfilter::Filter::unpack(s.moods);
  if (!s.moodData) {
    uistr::trf(buf, size, S::mood_no_data);
  } else if (!f.active()) {
    uistr::trf(buf, size, S::mood_any);
  } else {
    filtergrid::summary(f, buf, size);
  }
  return buf;
}

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

// The coverage once the index is read: BPM and key, and mood with mood data.
void coverage(const State& s, char* l, size_t n) {
  if (s.moodData) {
    uistr::trf(l, n, S::autodj_status_coverage_mood, clampPct(s.bpmPct), clampPct(s.keyPct), clampPct(s.moodPct));
  } else {
    uistr::trf(l, n, S::autodj_status_coverage, clampPct(s.bpmPct), clampPct(s.keyPct));
  }
}

// 1 to 50 tracks fit: fewer than the cooldown, so they repeat.
bool few(const State& s) { return s.fit >= 1 && s.fit <= autodj::kTrackCooldown; }

// A filter on, its count in the index's first line's place: with the index
// read, or with none Auto DJ can use (its picks random inside the filter).
// Found and Reading say what the index does, as with no filter.
bool fitLine(const State& s) {
  return s.moodOn && s.index != Index::Found && s.index != Index::Reading;
}

// The index's lines (the status row's, unheld, with no filter on).
void indexLines(const State& s, char* l1, size_t n1, char* l2, size_t n2) {
  l2[0] = 0;
  switch (s.index) {
    case Index::Ready:
      uistr::trnf(l1, n1, S::autodj_status_ready, s.tracks, uistr::grouped(s.mapped), uistr::grouped(s.tracks));
      coverage(s, l2, n2);
      return;
    case Index::Found:
      uistr::trnf(l1, n1, S::autodj_status_found, s.rows, uistr::grouped(s.rows));
      if (!s.on) uistr::trf(l2, n2, S::autodj_status_found_sub);  // (on: no library yet to join)
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

// With a filter on (fitLine()): how many fit, over the coverage once the
// index is read, else over the index's first line.
void fitLines(const State& s, char* l1, size_t n1, char* l2, size_t n2) {
  if (few(s)) {
    uistr::trnf(l1, n1, S::autodj_status_few, s.fit, s.fit);
  } else {
    uistr::trnf(l1, n1, S::autodj_status_fits, s.fitOf, uistr::grouped(s.fit), uistr::grouped(s.fitOf));
  }
  if (s.index != Index::Ready) {
    char spare[8];
    indexLines(s, l2, n2, spare, sizeof(spare));
  } else if (few(s)) {
    uistr::trf(l2, n2, S::autodj_status_few_sub, clampPct(s.bpmPct), clampPct(s.keyPct));
  } else {
    coverage(s, l2, n2);
  }
}

}  // namespace

void statusLines(const State& s, char* line1, size_t size1, char* line2, size_t size2) {
  if (!line1 || !line2 || size1 == 0 || size2 == 0) return;
  line1[0] = line2[0] = 0;
  const RowState r = rowState(s);
  if (r != RowState::Waiting && r != RowState::Paused) {
    if (fitLine(s)) {
      fitLines(s, line1, size1, line2, size2);
    } else {
      indexLines(s, line1, size1, line2, size2);
    }
    return;
  }
  // Why it adds nothing, over the index's first line.
  const S why = s.hold == autodj::Hold::Failures    ? S::autodj_status_paused
                : s.hold == autodj::Hold::RepeatOne ? S::autodj_status_waits_repeat
                : s.hold == autodj::Hold::NoFit     ? S::autodj_status_waits_fit
                                                    : S::autodj_status_waits_sleep;
  uistr::trf(line1, size1, why);
  if (s.hold == autodj::Hold::NoFit && s.index == Index::Ready) {
    coverage(s, line2, size2);  // (its count is 0: the coverage says what the filter had to go on)
    return;
  }
  char spare[8];
  indexLines(s, line2, size2, spare, sizeof(spare));
}

bool statusAmber(const State& s) {
  switch (rowState(s)) {
    case RowState::Paused: return true;
    case RowState::Waiting: return s.hold == autodj::Hold::NoFit;
    case RowState::On:
    case RowState::Off: break;
  }
  return fitLine(s) && s.fit <= autodj::kTrackCooldown;
}

bool emptyNoFit(const State& s) { return s.moodOn && s.fit == 0; }

bool emptyButton(const State& s, uint32_t libraryTracks) {
  return s.usable && libraryTracks > 0 && !emptyNoFit(s);
}

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
    const autodj::Pick& k = p.pick;
    if (k.jump != autodj::Jump::None || k.repeated || k.repinned) {
      // A mood filter's jump, repeat or re-pin (MOOD-FILTERS.md 6.2, 6.4)
      // says none of these: a jump's anchors had lists, a repeat is for
      // want of tracks that fit, the pin stays or moves onto the playing
      // track. Their own toasts are S5's (4.5).
    } else if (k.noIndex && !k.byIdJoined) {
      if (!randomSaid_) news |= kNewsRandom;
      randomSaid_ = true;
    } else if (k.random && picks_ == 0) {
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
