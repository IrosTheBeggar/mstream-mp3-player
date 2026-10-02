// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "GaplessEngine.h"

namespace {
uint32_t framesToMs(uint32_t frames) {
  return static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000 / GaplessJoin::kRingRate);
}
}  // namespace

void GaplessEngine::begin(uint32_t gen, uint32_t startIdx, uint32_t startMs, bool offers) {
  gen_ = gen;
  token_ = 0;
  decodeStart_ = startIdx;
  startMs_ = startMs;
  offers_ = offers;
  early_ = false;
  committed_ = false;
  flushed_ = false;
  marked_ = false;
  boundaryUp_ = false;
  failedAhead_ = false;
  cutting_ = false;
  phase_ = Phase::Producing;
  book_.restart(gen, startIdx, startMs);
}

void GaplessEngine::idle(uint32_t gen) {
  begin(gen, ring_.writePos(), 0, false);
  phase_ = Phase::Idle;
}

void GaplessEngine::setStartMs(uint32_t ms) {
  startMs_ = ms;
  book_.setStartMs(ms);
}

void GaplessEngine::sourceEnded(bool early) {
  phase_ = Phase::Ending;
  early_ = early;
  committed_ = false;
}

bool GaplessEngine::cutDue() {
  if (cutting_) return true;
  if (!boundaryUp_) return false;
  bool pending = false, cuttable = false, wanted = false;
  book_.cutCheck(gen_, &pending, &cuttable, &wanted);
  if (!pending) {  // the loop took the advance: nothing to cut any more
    boundaryUp_ = false;
    failedAhead_ = false;
    return false;
  }
  return cuttable && (wanted || failedAhead_ || !enabled());
}

uint32_t GaplessEngine::step(uint32_t bufferedMs) {
  if (cutDue()) return stepCut();
  switch (phase_) {
    case Phase::Ending:
      return stepEnding(bufferedMs);
    case Phase::Flushing:
      return stepFlush();
    case Phase::Draining:
      return stepDrain(bufferedMs);
    default:
      return 0;
  }
}

uint32_t GaplessEngine::stepEnding(uint32_t bufferedMs) {
  if (!committed_) {
    // The padding dropped (or, at an early end, what is held goes in: it
    // needs the pass budget the feed counts), then everything into the ring.
    feed_.setBudget(RingFeed::kStageFrames);
    if (!trim_.end(early_)) {
      feed_.commit();
      return kRoomWaitMs;
    }
    if (!feed_.commit()) return kRoomWaitMs;
    tracks_.close();
    committed_ = true;
    marked_ = false;
    endAt_ = ring_.writePos();
    tail_ = feed_.tailFrames();
    // Its exact length: from its first frame to the next stream's first,
    // or (no join) to the end of its tail: the same frame.
    const int32_t frames = static_cast<int32_t>(endAt_ + tail_ - decodeStart_);
    book_.freeze(gen_, startMs_ + framesToMs(frames > 0 ? static_cast<uint32_t>(frames) : 0));
    if (boundaryUp_ && frames <= 0) {
      // A joined track that ended before its first frame (it couldn't be
      // decoded): cut it back out, the track before ends as before.
      bool pending = false, cuttable = false, wanted = false;
      book_.cutCheck(gen_, &pending, &cuttable, &wanted);
      if (cuttable) {
        failedAhead_ = true;
        ++counters_.emptyAhead;
        note(Event::EmptyAhead);
        return 0;
      }
    }
  }
  GaplessJoin::Answer a = GaplessJoin::Answer::Nothing;
  if (canJoin()) a = book_.take(gen_, token_, &offer_);
  if (a == GaplessJoin::Answer::NoWord && bufferedMs > kWaitMinMs) return kWordWaitMs;
  then_ = Then::Drain;
  if (a == GaplessJoin::Answer::Next) {
    uint32_t hz = 0;
    if (!tracks_.probe(offer_, &hz)) {
      tracks_.close();
      ++counters_.failedOpens;
      note(Event::OpenFailed);
    } else {
      offerRate_ = hz;
      if (!flushed_ && feed_.continues(static_cast<int>(hz))) {
        join(true, false);
        return 0;
      }
      then_ = Then::Join;  // after the tail
    }
  }
  phase_ = Phase::Flushing;
  return 0;
}

uint32_t GaplessEngine::stepFlush() {
  if (!feed_.finish()) return kRoomWaitMs;
  if (!flushed_) {
    flushed_ = true;
    endAt_ = ring_.writePos();
    tail_ = 0;
    marked_ = false;
  }
  // The feed with the tail in: what a cut of the next join goes back to.
  if (!marked_ && canJoin()) marked_ = takeMark();
  if (then_ == Then::Join && marked_) {
    feed_.restartStream();
    join(false, false);
    return 0;
  }
  if (then_ == Then::Join) tracks_.close();
  phase_ = Phase::Draining;
  return 0;
}

uint32_t GaplessEngine::stepDrain(uint32_t bufferedMs) {
  if (ring_.size() == 0) {
    phase_ = Phase::Ended;
    return 0;
  }
  // A late word, while there is time for an open before the ring runs dry.
  if (canJoin() && marked_ && bufferedMs >= kWaitMinMs && book_.take(gen_, token_, &offer_) == GaplessJoin::Answer::Next) {
    uint32_t hz = 0;
    if (!tracks_.probe(offer_, &hz)) {
      tracks_.close();
      ++counters_.failedOpens;
      note(Event::OpenFailed, true);
      return kRoomWaitMs;
    }
    offerRate_ = hz;
    feed_.restartStream();
    join(false, true);
    return 0;
  }
  return kRoomWaitMs;
}

uint32_t GaplessEngine::stepCut() {
  if (!cutting_) {
    if (!book_.beginCut(gen_)) return 0;  // taken meanwhile: cutDue() sees it next time
    cutting_ = true;
  }
  switch (ring_.cutBack(cutAt_)) {
    case PcmRing::Cut::Pending:
      ++counters_.retries;
      return kCutRetryMs;
    case PcmRing::Cut::Crossed:
      book_.cutCrossed();
      cutting_ = false;
      failedAhead_ = false;
      ++counters_.tooLate;
      note(Event::CutTooLate);
      return 0;
    case PcmRing::Cut::Done:
      break;
  }
  // Back at the end of the track before, as if the cut track had never
  // been decoded: its decoder closed, the feed as it was at J, the
  // boundary gone before anything more is written.
  const uint32_t cutToken = token_;
  tracks_.close();
  feed_.rewind(*marks_[prev_.mark]);
  trim_.disarm();
  book_.cutDone();
  token_ = prev_.token;
  decodeStart_ = prev_.start;
  startMs_ = prev_.startMs;
  endAt_ = prev_.endAt;
  tail_ = prev_.tail;
  flushed_ = prev_.flushed;
  mark_ = prev_.mark;
  marked_ = true;
  committed_ = true;
  early_ = false;
  boundaryUp_ = false;
  failedAhead_ = false;
  cutting_ = false;
  phase_ = Phase::Ending;
  ++counters_.cuts;
  Note n;
  n.event = Event::Cut;
  n.token = cutToken;
  n.cutAt = cutAt_;
  n.ringFrames = cutAt_ - ring_.readPos();
  tracks_.note(n);
  return 0;
}

void GaplessEngine::join(bool continuous, bool late) {
  if (continuous && !takeMark()) {  // (no mark: no join)
    tracks_.close();
    then_ = Then::Drain;
    phase_ = Phase::Flushing;
    return;
  }
  const uint32_t heardAt = endAt_ + (continuous ? tail_ : 0);
  if (!tracks_.start()) {
    tracks_.close();
    ++counters_.failedOpens;
    note(Event::OpenFailed, late);
    if (!continuous) feed_.rewind(*marks_[mark_]);  // the track before with its tail in, as it was
    then_ = Then::Drain;
    phase_ = continuous ? Phase::Flushing : Phase::Draining;
    return;
  }
  prev_.token = token_;
  prev_.start = decodeStart_;
  prev_.startMs = startMs_;
  prev_.endAt = endAt_;
  prev_.tail = tail_;
  prev_.flushed = flushed_;
  prev_.mark = mark_;
  GaplessJoin::Boundary b;
  b.gen = gen_;
  b.after = token_;
  b.token = offer_.token;
  b.cutAt = endAt_;
  b.heardAt = heardAt;
  book_.joined(b);  // before the joined track's first frame
  cutAt_ = endAt_;
  token_ = offer_.token;
  decodeStart_ = heardAt;
  startMs_ = 0;
  committed_ = false;
  flushed_ = false;
  marked_ = false;
  early_ = false;
  boundaryUp_ = true;
  failedAhead_ = false;
  phase_ = Phase::Producing;
  if (continuous) {
    ++counters_.joins;
  } else {
    ++counters_.resets;
  }
  if (late) ++counters_.late;
  note(continuous ? Event::Joined : Event::JoinedReset, late);
}

bool GaplessEngine::takeMark() {
  // Not the mark a waiting boundary would be cut back to.
  const uint8_t slot = boundaryUp_ ? static_cast<uint8_t>(1 - prev_.mark) : 0;
  if (!marks_[slot] || !feed_.mark(marks_[slot])) return false;
  mark_ = slot;
  return true;
}

void GaplessEngine::note(Event e, bool late) {
  Note n;
  n.event = e;
  n.token = e == Event::OpenFailed ? offer_.token : token_;
  n.cutAt = boundaryUp_ ? cutAt_ : endAt_;
  n.heardAt = decodeStart_;
  n.ringFrames = endAt_ - ring_.readPos();
  n.rate = offerRate_;
  n.late = late;
  tracks_.note(n);
}
