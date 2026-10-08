// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ScanScheduler.h"

namespace {
// `a` after `b`, as millis() goes (wrap-safe within 24 days).
bool after(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }
}  // namespace

void ScanScheduler::Window::open(uint32_t nowMs, uint32_t forMs) {
  if (forMs == 0) return;  // (a Config of 0: no window)
  const uint32_t end = nowMs + forMs;
  if (!on || after(end, until)) until = end;
  on = true;
}

void ScanScheduler::Window::expire(uint32_t nowMs) {
  if (on && !after(until, nowMs)) on = false;
}

ScanScheduler::Out ScanScheduler::update(const In& in) {
  const uint32_t now = in.nowMs;
  // The time since the last pass went to what it waited for then.
  if (primed_ && lastWait_ != Wait::None) waited_[static_cast<int>(lastWait_)] += now - lastNowMs_;
  lastNowMs_ = now;
  Out o;
  observe(in, o);
  decide(in, o);
  lastWait_ = o.wait;
  return o;
}

void ScanScheduler::observe(const In& in, Out& o) {
  const uint32_t now = in.nowMs;
  if (!primed_) {
    // The counters as they are: what came before isn't news.
    lastUnderruns_ = in.underruns;
    lastTrackSeq_ = in.trackSeq;
    lastSeekSeq_ = in.seekSeq;
    lastBtSeq_ = in.btSeq;
    primed_ = true;
  }
  input_.expire(now);
  underrun_.expire(now);
  pass_.expire(now);
  track_.expire(now);
  seek_.expire(now);
  bt_.expire(now);

  if (in.input) input_.open(now, config_.inputQuietMs);
  if (in.underruns != lastUnderruns_) underrun_.open(now, config_.underrunBackoffMs);
  if (in.decodePassUs > config_.passLimitUs) pass_.open(now, config_.passBackoffMs);
  // From the decoder's end of file until the next track has been heard for
  // trackSettleMs (its token changes when it begins to be heard).
  if (in.decoderAtEnd || in.trackSeq != lastTrackSeq_) track_.open(now, config_.trackSettleMs);
  if (in.seeking || in.seekSeq != lastSeekSeq_) seek_.open(now, config_.seekSettleMs);
  if (in.btSetup || in.btSeq != lastBtSeq_) bt_.open(now, config_.btSettleMs);
  lastUnderruns_ = in.underruns;
  lastTrackSeq_ = in.trackSeq;
  lastSeekSeq_ = in.seekSeq;
  lastBtSeq_ = in.btSeq;

  // The battery floor, with its hysteresis (U13). A reading not known keeps
  // the state.
  const bool before = batteryLow_;
  if (in.usb) {
    batteryLow_ = false;
  } else if (in.battery >= 0) {
    if (in.battery < config_.floorPct) {
      batteryLow_ = true;
    } else if (in.battery > config_.resumeAbovePct) {
      batteryLow_ = false;
    }
  }
  o.batteryHeld = !before && batteryLow_;
  o.batteryReleased = before && !batteryLow_;
}

ScanScheduler::Wait ScanScheduler::yieldOf(const In& in) const {
  if (input_.on) return Wait::Input;
  if (in.playing && in.ringCapacityMs > 0 &&
      static_cast<uint64_t>(in.ringMs) * 100u < static_cast<uint64_t>(in.ringCapacityMs) * config_.ringMinPct) {
    return Wait::Ring;
  }
  if (underrun_.on) return Wait::Underrun;
  if (pass_.on) return Wait::DecodePass;
  if (track_.on) return Wait::TrackChange;
  if (seek_.on) return Wait::Seek;
  if (bt_.on) return Wait::Bluetooth;
  return Wait::None;
}

void ScanScheduler::decide(const In& in, Out& o) const {
  if (in.running != Job::None) {
    // One step at a time: it finishes first. A cover under way drops below
    // the loop while a list moves (Thumbs' rule).
    o.priority = priorityOf(in.running, in.listMoving);
    o.wait = Wait::Step;
    return;
  }
  if (in.build) {
    o.job = Job::Build;
    o.priority = priorityOf(Job::Build, in.listMoving);
    return;
  }
  const Source src = sourceOf(in);
  const bool background = in.walk || in.compact || src != Source::None || in.djCheck;
  if (!in.cover && !in.save && !background) return;  // nothing to do
  if (in.listMoving) {
    o.wait = Wait::List;
    return;
  }
  if (in.cover) {
    o.job = Job::Cover;
    o.priority = priorityOf(Job::Cover, false);
    return;
  }
  Job next = Job::None;
  if (in.save) {
    next = Job::Save;
  } else if (in.updating) {
    o.wait = Wait::Updating;  // (only background work is left: `background` holds)
    return;
  } else if (in.walk) {
    next = Job::Walk;
  } else if (in.compact) {
    next = Job::Compact;
  } else if (src != Source::None) {
    next = Job::Scan;
  } else {
    next = Job::DjCheck;
  }
  if ((next == Job::Scan || next == Job::DjCheck) && batteryLow_) {
    o.wait = Wait::Battery;
    return;
  }
  const Wait w = yieldOf(in);
  if (w != Wait::None) {
    o.wait = w;
    return;
  }
  o.job = next;
  o.source = next == Job::Scan ? src : Source::None;
  o.priority = priorityOf(next, false);
}

ScanScheduler::Source ScanScheduler::sourceOf(const In& in) {
  if (in.playingPending) return Source::Playing;
  if (in.queueNextPending) return Source::QueueNext;
  if (in.queueSoonPending) return Source::QueueSoon;
  if (in.shownPending) return Source::Shown;
  if (in.restPending) return Source::Rest;
  return Source::None;
}

uint8_t ScanScheduler::priorityOf(Job job, bool listMoving) {
  switch (job) {
    case Job::Build: return kHighPriority;
    case Job::Cover: return listMoving ? kLowPriority : kHighPriority;
    default: return kLowPriority;
  }
}

void ScanScheduler::stepDone(Job job, uint32_t ms) {
  const int j = static_cast<int>(job);
  if (j <= 0 || j >= kJobs) return;
  ++steps_[j];
  stepMs_[j] += ms;
  if (ms > stepMaxMs_[j]) stepMaxMs_[j] = ms;
}

uint64_t ScanScheduler::waitedMs(Wait w) const {
  const int i = static_cast<int>(w);
  return i >= 0 && i < kWaits ? waited_[i] : 0;
}

uint32_t ScanScheduler::steps(Job job) const {
  const int j = static_cast<int>(job);
  return j >= 0 && j < kJobs ? steps_[j] : 0;
}

uint32_t ScanScheduler::meanStepMs(Job job) const {
  const int j = static_cast<int>(job);
  if (j < 0 || j >= kJobs || steps_[j] == 0) return 0;
  return static_cast<uint32_t>((stepMs_[j] + steps_[j] / 2) / steps_[j]);
}

uint32_t ScanScheduler::maxStepMs(Job job) const {
  const int j = static_cast<int>(job);
  return j >= 0 && j < kJobs ? stepMaxMs_[j] : 0;
}

uint32_t ScanScheduler::scanMsPerFile() const {
  const uint32_t mean = meanStepMs(Job::Scan);
  return steps(Job::Scan) > 0 ? (mean > 0 ? mean : 1) : kEstimateMsPerFile;
}

void ScanScheduler::resetStats() {
  for (uint64_t& w : waited_) w = 0;
  for (int j = 0; j < kJobs; ++j) {
    steps_[j] = 0;
    stepMs_[j] = 0;
    stepMaxMs_[j] = 0;
  }
}

uint32_t ScanScheduler::scanEstimateMs(uint32_t files, uint32_t msPerFile) {
  const uint64_t ms = static_cast<uint64_t>(files) * msPerFile;
  return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
}

bool ScanScheduler::buildAfterWalk(uint32_t added, uint32_t toScan, uint32_t msPerFile) {
  return added >= kBuildNowAdded || scanEstimateMs(toScan, msPerFile) > kBuildNowScanMs;
}

const char* ScanScheduler::jobName(Job j) {
  switch (j) {
    case Job::None: return "none";
    case Job::Cover: return "cover";
    case Job::Build: return "build";
    case Job::Save: return "save";
    case Job::Walk: return "walk";
    case Job::Compact: return "compaction";
    case Job::Scan: return "scan";
    case Job::DjCheck: return "DJ check";
  }
  return "?";
}

const char* ScanScheduler::sourceName(Source s) {
  switch (s) {
    case Source::None: return "none";
    case Source::Playing: return "the playing track";
    case Source::QueueNext: return "the queue's next";
    case Source::QueueSoon: return "the queue";
    case Source::Shown: return "the Library tab";
    case Source::Rest: return "the rest";
  }
  return "?";
}

const char* ScanScheduler::waitName(Wait w) {
  switch (w) {
    case Wait::None: return "nothing";
    case Wait::Step: return "a step under way";
    case Wait::List: return "a list moving";
    case Wait::Updating: return "the library update";
    case Wait::Battery: return "the battery (below the floor)";
    case Wait::Input: return "input";
    case Wait::Ring: return "the ring below half";
    case Wait::Underrun: return "an underrun";
    case Wait::DecodePass: return "a long decode pass";
    case Wait::TrackChange: return "a track change";
    case Wait::Seek: return "a seek";
    case Wait::Bluetooth: return "Bluetooth";
  }
  return "?";
}
