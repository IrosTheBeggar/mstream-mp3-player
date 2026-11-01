// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RtcSync.h"

namespace rtcsync {

using wallclock::Clock;
using wallclock::Source;

int32_t phaseFrom(bool have, int32_t stored) {
  if (!have || stored <= -kMaxPhase || stored >= kMaxPhase) return kNoPhase;
  return stored;
}

Sample sample(bool answered, const uint8_t (&regs)[7]) {
  Sample s;
  s.answered = answered;
  if (!answered) return s;
  const wallclock::Reading r = wallclock::fromRegisters(regs);
  s.voltLow = r.voltLow;
  uint64_t ms = 0;
  s.believed = wallclock::judge(r, &ms) == wallclock::Boot::Ok;
  if (s.believed) s.second = ms;
  return s;
}

void Sync::post(const Event& e) {
  event_ = e;
  pending_ = true;
}

bool Sync::takeEvent(Event* out) {
  if (!pending_) return false;
  *out = event_;
  pending_ = false;
  return true;
}

void Sync::startCatch(Purpose p, uint64_t mono, bool started) {
  mode_ = Mode::Catch;
  purpose_ = p;
  started_ = started;
  start_ = mono;
  polled_ = false;
  have_ = false;
  checked_ = false;
  best_ = Bracket();
}

void Sync::boot(const Clock& clock, int32_t phase) {
  *this = Sync();
  phase_ = phaseFrom(phase != kNoPhase, phase);
  // (The window from the first pass: setup() takes seconds before the loop.)
  if (clock.source() == Source::Rtc) startCatch(Purpose::Boot, 0, false);
}

void Sync::plan(uint64_t unixMs, uint64_t mono) {
  mode_ = Mode::Wait;
  best_ = Bracket();
  uint64_t next = (unixMs + 999) / 1000 * 1000;
  waits_ = 0;
  if (next > wallclock::kMaxMs) {  // (2100: past the chip's calendar)
    next = unixMs;
    waits_ = kWaits;
  }
  writeAt_ = mono + (next - unixMs);
  attempt_ = 0;
  forgotten_ = false;
}

Do Sync::step(uint64_t mono, Clock& clock, uint64_t* second) {
  switch (mode_) {
    case Mode::Idle:
    case Mode::Writing:
      return Do::Nothing;
    case Mode::Wait: {
      if (mono < writeAt_) return Do::Nothing;
      if (!forgotten_) return Do::Forget;  // (before the chip changes: never an older write's phase)
      const uint64_t t = clock.at(mono);
      if (t == 0) {  // (never: a @time set it first)
        mode_ = Mode::Idle;
        return Do::Nothing;
      }
      const uint32_t late = static_cast<uint32_t>(t % 1000);
      if (late > kLateMs && waits_ < kWaits) {
        ++waits_;
        writeAt_ = mono + (1000 - late);
        return Do::Nothing;
      }
      uint64_t w = t - late;
      if (w > wallclock::kMaxMs) w = wallclock::kMaxMs / 1000 * 1000;  // (2099's last)
      written_ = w;
      lateMs_ = late;
      ++attempt_;
      mode_ = Mode::Writing;
      *second = w;
      return Do::Write;
    }
    case Mode::Catch:
      if (!started_) {
        started_ = true;
        start_ = mono;
      }
      if (mono - start_ >= kCatchMs) {
        if (purpose_ == Purpose::Check && !checked_) {
          refuse(Event::Why::Unread, Sample(), mono, clock);
        } else {
          finish(Event::Why::Unanswered, clock);
        }
        return Do::Nothing;
      }
      if (polled_ && mono - lastPoll_ < kPollMs) return Do::Nothing;
      return Do::Read;
  }
  return Do::Nothing;
}

void Sync::forgot() { forgotten_ = true; }

void Sync::wrote(uint64_t mono, bool ok, const Clock& clock) {
  if (mode_ != Mode::Writing) return;
  writtenMono_ = mono;
  if (!ok) {
    refuse(Event::Why::WriteFailed, Sample(), mono, clock);
    return;
  }
  // Read back at once (the same pass), then its next tick.
  startCatch(Purpose::Check, mono, true);
}

void Sync::refuse(Event::Why why, const Sample& back, uint64_t mono, const Clock& clock) {
  Event e;
  e.kind = Event::Kind::Refused;
  e.why = why;
  e.second = written_;
  e.lateMs = lateMs_;
  e.attempt = attempt_;
  e.back = back;
  if (attempt_ < kAttempts) {
    // Again on the clock's next second.
    e.again = true;
    mode_ = Mode::Wait;
    writeAt_ = mono + (1000 - clock.at(mono) % 1000);
    waits_ = 0;
  } else {
    mode_ = Mode::Idle;
  }
  post(e);
}

void Sync::read(uint64_t mono, const Sample& s, Clock& clock) {
  if (mode_ != Mode::Catch) return;
  polled_ = true;
  lastPoll_ = mono;
  if (purpose_ == Purpose::Check && !checked_) {
    if (!s.answered) return;  // (the next pass reads again)
    // The second written, or one the chip counted since (a tick between
    // the write and this read).
    const uint64_t since = mono - writtenMono_;
    if (s.voltLow) {
      refuse(Event::Why::Flag, s, mono, clock);
      return;
    }
    if (!s.believed) {
      refuse(Event::Why::NotTime, s, mono, clock);
      return;
    }
    if (s.second < written_ || s.second > written_ + 1000 + since / 1000 * 1000) {
      refuse(Event::Why::Wrong, s, mono, clock);
      return;
    }
    checked_ = true;
  }
  if (!s.answered || !s.believed) return;
  if (!have_) {
    have_ = true;
    last_ = s.second;
    firstMono_ = lastMono_ = mono;
    return;
  }
  if (s.second == last_) {
    lastMono_ = mono;
    if (mono - firstMono_ >= (purpose_ == Purpose::Check ? kStillAfterWriteMs : kStillMs)) {
      finish(Event::Why::Still, clock);
    }
    return;
  }
  if (s.second == last_ + 1000) {
    // The tick came after the last read of the old second and by this one.
    Bracket b;
    b.valid = true;
    b.gap = mono - lastMono_;
    b.tick = lastMono_ + b.gap / 2;
    b.second = s.second;
    if (!best_.valid || b.gap < best_.gap) best_ = b;
    last_ = s.second;
    firstMono_ = lastMono_ = mono;
    if (b.gap <= kCloseMs) finish(Event::Why::None, clock);
    return;
  }
  // More than one tick since the last read (reads lost for over a second),
  // or a second that can't follow it (a misread, a write elsewhere): from
  // here again; a bracket from before kept only in the first case.
  if (!(s.second > last_ && s.second - last_ <= mono - lastMono_ + 1000)) best_ = Bracket();
  last_ = s.second;
  firstMono_ = lastMono_ = mono;
}

void Sync::finish(Event::Why why, Clock& clock) {
  mode_ = Mode::Idle;
  Event e;
  if (purpose_ == Purpose::Boot) {
    if (clock.source() != Source::Rtc) return;  // (a @time since: plan() took over)
    if (!best_.valid) {
      e.kind = Event::Kind::Missed;
      e.why = why == Event::Why::None ? Event::Why::Unanswered : why;
      post(e);
      return;
    }
    const int32_t add = phase_ == kNoPhase ? 0 : phase_;
    const uint64_t t = static_cast<uint64_t>(static_cast<int64_t>(best_.second) + add);
    // The boot took the second it read as its start: the true time at its
    // reading was `add` to `add` + 1 s later than that (within the bracket).
    const int64_t shift = static_cast<int64_t>(t) - static_cast<int64_t>(clock.at(best_.tick));
    const int64_t slack = static_cast<int64_t>((best_.gap + 1) / 2 + kPollMs);
    if (shift < add - slack || shift >= add + 1000 + slack || !clock.refine(t, best_.tick)) {
      e.kind = Event::Kind::Missed;
      e.why = Event::Why::Disagreed;
      e.second = best_.second;
      post(e);
      return;
    }
    e.kind = Event::Kind::Caught;
    e.second = best_.second;
    e.time = t;
    e.shiftMs = shift;
    e.withinMs = static_cast<uint32_t>((best_.gap + 1) / 2);
    e.phaseMs = phase_;
    post(e);
    return;
  }
  // A @time's check: the phase against the clock the @time set.
  e.second = written_;
  e.lateMs = lateMs_;
  if (!best_.valid) {
    e.kind = Event::Kind::Untimed;
    e.why = why == Event::Why::None ? Event::Why::Unanswered : why;
    post(e);
    return;
  }
  const int64_t phase = static_cast<int64_t>(clock.at(best_.tick)) - static_cast<int64_t>(best_.second);
  if (phase <= -kMaxPhase || phase >= kMaxPhase) {  // (never: the read-back was the second written)
    e.kind = Event::Kind::Untimed;
    e.why = Event::Why::Wrong;
    post(e);
    return;
  }
  e.kind = Event::Kind::Taken;
  e.phaseMs = static_cast<int32_t>(phase);
  e.withinMs = static_cast<uint32_t>((best_.gap + 1) / 2);
  post(e);
}

}  // namespace rtcsync
