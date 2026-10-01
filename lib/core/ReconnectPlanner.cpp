// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ReconnectPlanner.h"

void ReconnectPlanner::start(Why, bool remembered, bool canScan, uint32_t nowMs) {
  sinceMs_ = nowMs;
  step_ = 0;
  pages_ = 0;
  if (!remembered) {
    // Nothing to page. No name to look for (a release build): quiet,
    // connectable only; the Pair screen is the way to headphones.
    phase_ = canScan ? Phase::Scan : Phase::Resting;
    return;
  }
  phase_ = Phase::Burst;
  // A page already on its way (the background's, the boot's) is the
  // burst's first try, not one to page on top of.
  if (pageOnItsWay(nowMs)) pages_ = 1;
}

void ReconnectPlanner::pageMade(uint32_t nowMs) {
  paged_ = true;
  pageOpen_ = true;
  lastPageMs_ = nowMs;
  if (phase_ == Phase::Burst && pages_ < kBurstPages) ++pages_;
}

void ReconnectPlanner::pageEnded(uint32_t) { pageOpen_ = false; }

bool ReconnectPlanner::pageOnItsWay(uint32_t nowMs) const {
  return paged_ && pageOpen_ && !elapsed(nowMs, lastPageMs_, kPageMs);
}

uint32_t ReconnectPlanner::nextPageInMs(uint32_t nowMs) const {
  if (phase_ != Phase::Backoff || !paged_) return 0;
  const int32_t left = static_cast<int32_t>(backoffMs()) - static_cast<int32_t>(nowMs - lastPageMs_);
  return left > 0 ? static_cast<uint32_t>(left) : 0u;
}

ReconnectPlanner::Do ReconnectPlanner::step(const In& in) {
  if (in.linked) {
    phase_ = Phase::Idle;
    return Do::Nothing;
  }
  // The library's scan by name (the state is ours to set; the stack's own
  // flag may lag behind a stop).
  const bool scanning = in.state == Lib::Discovering;
  switch (phase_) {
    case Phase::Idle:
      return Do::Nothing;

    case Phase::Resting:
      // Connectable only: a scan by name still running is stopped.
      return scanning ? Do::StopScan : Do::Nothing;

    case Phase::Scan:
      if (in.remembered) {
        // Headphones to page now: the scan found them and the connection
        // failed (the library connects to what it finds), or they were
        // paired meanwhile. No inquiry while remembered.
        if (scanning) return Do::StopScan;
        if (in.state != Lib::Unconnected) return Do::Nothing;  // still connecting to what it found
        start(Why::Ask, true, in.canScan, in.nowMs);
        return step(in);
      }
      if (!in.canScan || elapsed(in.nowMs, sinceMs_, kScanForMs)) {
        phase_ = Phase::Resting;
        return scanning ? Do::StopScan : Do::Nothing;
      }
      // (The library runs round after round while DISCOVERING.)
      if (in.state == Lib::Unconnected && !in.discoveryActive) return Do::Scan;
      return Do::Nothing;

    case Phase::Burst:
    case Phase::Backoff:
      break;
  }

  // Burst, Backoff: remembered headphones are paged, never scanned for.
  if (!in.remembered) {
    // Forgotten meanwhile: a scan by name instead, or Resting with no
    // name to look for.
    phase_ = in.canScan ? Phase::Scan : Phase::Resting;
    sinceMs_ = in.nowMs;
    return step(in);
  }
  if (scanning) return Do::StopScan;
  // A connection under way (the boot's page, the library's own), or a page
  // of ours: wait for its answer.
  if (in.state == Lib::Connecting || in.state == Lib::Other) return Do::Nothing;
  if (pageOnItsWay(in.nowMs)) return Do::Nothing;

  if (phase_ == Phase::Burst) {
    if (pages_ < kBurstPages) {
      if (pages_ == 0 || !paged_ || elapsed(in.nowMs, lastPageMs_, kBurstGapMs)) return Do::Page;
      return Do::Nothing;
    }
    // The burst's pages have all had their answer: back off.
    phase_ = Phase::Backoff;
    step_ = 0;
  }
  // Nobody around, or long enough: stop paging, stay connectable.
  if (in.quiet || elapsed(in.nowMs, sinceMs_, kGiveUpMs)) {
    phase_ = Phase::Resting;
    return Do::Nothing;
  }
  if (!paged_ || elapsed(in.nowMs, lastPageMs_, backoffMs())) {
    if (step_ < kBackoffSteps) ++step_;
    return Do::Page;
  }
  return Do::Nothing;
}

const char* ReconnectPlanner::phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Burst: return "burst";
    case Phase::Backoff: return "backoff";
    case Phase::Resting: return "resting";
    case Phase::Scan: return "scan";
  }
  return "?";
}

const char* ReconnectPlanner::whyName(Why w) {
  switch (w) {
    case Why::Boot: return "boot";
    case Why::Drop: return "the headphones dropped";
    case Why::Ask: return "asked for";
  }
  return "?";
}

// ---- RadioMeter ----

void RadioMeter::sample(uint32_t nowMs, bool busy) {
  if (!started_) {
    started_ = true;
    lastMs_ = nowMs;
    minuteStartMs_ = nowMs;
    lastBusy_ = busy;
    return;
  }
  const int32_t d = static_cast<int32_t>(nowMs - lastMs_);
  uint32_t dt = d > 0 ? static_cast<uint32_t>(d) : 0u;
  if (dt > kMaxStepMs) dt = kMaxStepMs;
  countedMs_ += dt;
  if (lastBusy_) busyMs_ += dt;
  lastMs_ = nowMs;
  lastBusy_ = busy;
  if (static_cast<int32_t>(nowMs - minuteStartMs_) >= static_cast<int32_t>(kMinuteMs)) {
    lastPercent_ = countedMs_ ? static_cast<int>((busyMs_ * 100u + countedMs_ / 2) / countedMs_) : 0;
    minuteStartMs_ = nowMs;
    countedMs_ = 0;
    busyMs_ = 0;
  }
}
