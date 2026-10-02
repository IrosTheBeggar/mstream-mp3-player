// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "HostClock.h"

#include <algorithm>
#include <cmath>

namespace {
// Signed microseconds from `from` to `to` (the counter wraps).
double secondsBetween(uint32_t from, uint32_t to) { return static_cast<int32_t>(to - from) * 1e-6; }
}  // namespace

void HostClock::start(uint32_t rate) {
  rate_ = rate ? rate : 44100;
  first_ = count_ = 0;
  playing_ = false;
  have_ = false;
  anchor_ = 0.0;
  anchorUs_ = 0;
  slew_ = 0.0f;
  any_ = false;
  snaps_ = 0;
}

void HostClock::dropOld(uint32_t nowUs) {
  while (count_ > 0 && static_cast<int32_t>(nowUs - samples_[first_].us) > static_cast<int32_t>(kWindowUs)) {
    first_ = (first_ + 1) % kMaxSamples;
    --count_;
  }
}

double HostClock::edgeAt(uint32_t atUs) const {
  double edge = 0.0;
  for (int i = 0; i < count_; ++i) {
    const Sample& s = samples_[(first_ + i) % kMaxSamples];
    const double e = s.heard + rate_ * secondsBetween(s.us, atUs);
    if (i == 0 || e > edge) edge = e;
  }
  return edge;
}

double HostClock::clockAt(uint32_t atUs) const {
  const double t = secondsBetween(anchorUs_, atUs);
  const double hold = kSlewHoldUs * 1e-6;
  // The slew for at most kSlewHoldUs after its sample, then the plain rate.
  if (t <= hold) return anchor_ + rate_ * (1.0 + slew_) * t;
  return anchor_ + rate_ * (1.0 + slew_) * hold + rate_ * (t - hold);
}

void HostClock::sample(uint32_t nowUs, int32_t heard, bool playing) {
  lastUs_ = nowUs;
  any_ = true;
  if (!playing) {
    // Paused (or stopped, starved): invalid at once; the next play snaps.
    playing_ = false;
    have_ = false;
    first_ = count_ = 0;
    slew_ = 0.0f;
    return;
  }
  playing_ = true;
  if (count_ == kMaxSamples) {  // full: the oldest goes
    first_ = (first_ + 1) % kMaxSamples;
    --count_;
  }
  samples_[(first_ + count_) % kMaxSamples] = Sample{nowUs, heard};
  ++count_;
  dropOld(nowUs);
  const double edge = edgeAt(nowUs);
  const double err = have_ ? edge - clockAt(nowUs) : 0.0;
  if (!have_ || std::fabs(err) > rate_ * (kSnapUs * 1e-6)) {
    anchor_ = edge;
    slew_ = 0.0f;
    have_ = true;
    ++snaps_;
  } else {
    anchor_ = clockAt(nowUs);
    const double s = err / (rate_ * (kSlewUs * 1e-6));
    slew_ = static_cast<float>(std::max(-static_cast<double>(kMaxSlew), std::min(static_cast<double>(kMaxSlew), s)));
  }
  anchorUs_ = nowUs;
}

bool HostClock::valid(uint32_t nowUs) const {
  return playing_ && have_ && static_cast<int32_t>(nowUs - lastUs_) < static_cast<int32_t>(kStaleUs);
}

double HostClock::frameAt(uint32_t atUs) const { return have_ ? clockAt(atUs) : 0.0; }

HostClock::Heard HostClock::at(uint32_t nowUs, int32_t aheadUs) const {
  Heard h;
  if (!valid(nowUs)) return h;
  const double f = clockAt(nowUs + static_cast<uint32_t>(aheadUs));
  const double whole = std::floor(f);
  h.valid = true;
  h.frame = static_cast<int32_t>(static_cast<int64_t>(whole));
  h.frac = static_cast<float>(f - whole);
  return h;
}

HostClock::Stats HostClock::stats(uint32_t nowUs) const {
  Stats s;
  s.valid = valid(nowUs);
  s.snaps = snaps_;
  s.slew = slew_;
  s.samples = count_;
  s.ageMs = any_ ? static_cast<uint32_t>(std::max<int32_t>(0, static_cast<int32_t>(nowUs - lastUs_)) / 1000) : 0;
  if (count_ > 0) {
    // Each sample's distance behind the window's leading edge, in ms.
    float behind[kMaxSamples];
    const double edge = edgeAt(nowUs);
    for (int i = 0; i < count_; ++i) {
      const Sample& x = samples_[(first_ + i) % kMaxSamples];
      behind[i] = static_cast<float>((edge - (x.heard + rate_ * secondsBetween(x.us, nowUs))) * 1000.0 / rate_);
    }
    std::sort(behind, behind + count_);
    s.spreadMs = behind[std::min(count_ - 1, static_cast<int>(std::floor(0.95 * (count_ - 1) + 0.5)))];
  }
  return s;
}
