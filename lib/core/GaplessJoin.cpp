// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "GaplessJoin.h"

void GaplessJoin::setOffer(uint32_t gen, uint32_t after, uint32_t token, const std::string& path, uint32_t hintMs) {
  std::lock_guard<std::mutex> guard(lock_);
  offerSet_ = true;
  offer_.gen = gen;
  offer_.after = after;
  offer_.token = token;
  offer_.hintMs = hintMs;
  offer_.path = token ? path : std::string();
}

bool GaplessJoin::takeAdvance(uint32_t gen, uint32_t readPos, uint32_t* token) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!hasBoundary_ || b_.gen != gen || state_ == State::Cutting) return false;
  // Strictly past B: a frame of the joined track has been read (at B
  // exactly the track before has been read to its end, and a cut to
  // J <= B could still be Done).
  if (static_cast<int32_t>(readPos - b_.heardAt) <= 0) return false;
  heardStart_ = b_.heardAt;
  heardStartMs_ = 0;
  frozen_ = nextFrozen_;
  frozenMs_ = nextLengthMs_;
  nextFrozen_ = false;
  hasBoundary_ = false;
  *token = b_.token;
  return true;
}

uint32_t GaplessJoin::positionMs(uint32_t gen, uint32_t readPos) const {
  std::lock_guard<std::mutex> guard(lock_);
  uint32_t r = readPos;
  if (hasBoundary_ && b_.gen == gen && static_cast<int32_t>(r - b_.heardAt) > 0) r = b_.heardAt;
  return heardStartMs_ + static_cast<uint32_t>(static_cast<uint64_t>(r - heardStart_) * 1000 / kRingRate);
}

uint32_t GaplessJoin::startMs() const {
  std::lock_guard<std::mutex> guard(lock_);
  return heardStartMs_;
}

bool GaplessJoin::frozenLength(uint32_t* ms) const {
  std::lock_guard<std::mutex> guard(lock_);
  if (!frozen_) return false;
  *ms = frozenMs_;
  return true;
}

void GaplessJoin::restart(uint32_t gen, uint32_t startIdx, uint32_t startMs) {
  std::lock_guard<std::mutex> guard(lock_);
  gen_ = gen;
  hasBoundary_ = false;
  nextFrozen_ = false;
  frozen_ = false;
  heardStart_ = startIdx;
  heardStartMs_ = startMs;
  if (offerSet_ && offer_.gen != gen) offerSet_ = false;
}

void GaplessJoin::setStartMs(uint32_t ms) {
  std::lock_guard<std::mutex> guard(lock_);
  heardStartMs_ = ms;
}

GaplessJoin::Answer GaplessJoin::take(uint32_t gen, uint32_t after, Offer* out) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!offerSet_ || offer_.gen != gen || offer_.after != after) return Answer::NoWord;
  if (offer_.token == 0 || offer_.token == lastTaken_) return Answer::Nothing;
  lastTaken_ = offer_.token;
  *out = offer_;
  return Answer::Next;
}

void GaplessJoin::freeze(uint32_t gen, uint32_t lengthMs) {
  std::lock_guard<std::mutex> guard(lock_);
  if (gen != gen_) return;
  if (hasBoundary_) {
    nextFrozen_ = true;
    nextLengthMs_ = lengthMs;
  } else {
    frozen_ = true;
    frozenMs_ = lengthMs;
  }
}

void GaplessJoin::joined(const Boundary& b) {
  std::lock_guard<std::mutex> guard(lock_);
  b_ = b;
  hasBoundary_ = true;
  state_ = State::Pending;
  nextFrozen_ = false;
}

void GaplessJoin::cutCheck(uint32_t gen, bool* pending, bool* cuttable, bool* wanted) const {
  std::lock_guard<std::mutex> guard(lock_);
  *pending = hasBoundary_ && b_.gen == gen;
  *cuttable = *pending && state_ == State::Pending;
  *wanted = *cuttable && offerSet_ && offer_.gen == gen && offer_.after == b_.after && offer_.token != b_.token;
}

bool GaplessJoin::beginCut(uint32_t gen) {
  std::lock_guard<std::mutex> guard(lock_);
  if (!hasBoundary_ || b_.gen != gen || state_ != State::Pending) return false;
  state_ = State::Cutting;
  return true;
}

void GaplessJoin::cutDone() {
  std::lock_guard<std::mutex> guard(lock_);
  hasBoundary_ = false;
  nextFrozen_ = false;
}

void GaplessJoin::cutCrossed() {
  std::lock_guard<std::mutex> guard(lock_);
  if (hasBoundary_ && state_ == State::Cutting) state_ = State::Committed;
}

GaplessJoin::Status GaplessJoin::status() const {
  std::lock_guard<std::mutex> guard(lock_);
  Status s;
  s.offer = offerSet_;
  s.offerNow = offer_;
  s.lastTaken = lastTaken_;
  s.boundary = hasBoundary_;
  s.b = b_;
  s.state = state_;
  s.frozen = frozen_;
  s.frozenMs = frozenMs_;
  s.heardStart = heardStart_;
  return s;
}
