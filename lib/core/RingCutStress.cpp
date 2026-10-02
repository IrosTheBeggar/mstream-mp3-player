// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "RingCutStress.h"

#include <algorithm>

RingCutStress::RingCutStress(PcmRing& ring, uint8_t reader, uint32_t seed, uint32_t tracks)
    : ring_(ring),
      reader_(reader),
      total_(tracks < kMaxTracks - 2 ? tracks : kMaxTracks - 2),
      prng_(seed),
      cut_((kMaxTracks + 7) / 8, 0),
      crng_(seed ^ 0x9E3779B9u),
      seen_((kMaxTracks + 7) / 8, 0) {
  len_ = 1 + static_cast<int>(prng_() % 300);
}

bool RingCutStress::write(int track, int to) {
  if (from_ >= to) return true;
  int16_t f[32 * 2];
  const int n = std::min(to - from_, 1 + static_cast<int>(prng_() % 32));
  for (int k = 0; k < n; ++k) {
    f[2 * k] = static_cast<int16_t>(track);
    f[2 * k + 1] = static_cast<int16_t>(from_ + k);
  }
  from_ += static_cast<int>(ring_.write(f, static_cast<uint32_t>(n)));
  return from_ >= to;
}

bool RingCutStress::produce() {
  switch (step_) {
    case Step::Rest:  // the rest of the current track
      if (!write(track_, len_)) return true;
      ++written_;
      if (static_cast<uint32_t>(track_) >= total_) {
        step_ = Step::Done;
        return false;
      }
      j_ = ring_.writePos();
      // The next one, part of it decoded ahead.
      ++track_;
      len_ = 1 + static_cast<int>(prng_() % 300);
      part_ = 1 + static_cast<int>(prng_() % len_);
      from_ = 0;
      step_ = Step::Ahead;
      return true;
    case Step::Ahead:
      if (!write(track_, part_)) return true;
      step_ = prng_() % 2 == 0 ? Step::Decide : Step::Rest;
      return true;
    case Step::Decide:
      switch (ring_.cutBack(j_)) {
        case PcmRing::Cut::Pending:
          ++retries_;
          return true;  // (the fence stays up: again next step)
        case PcmRing::Cut::Done:
          ++cuts_;
          cut_[track_ / 8] |= static_cast<uint8_t>(1u << (track_ % 8));
          ++track_;  // another track in its place, from its start
          len_ = 1 + static_cast<int>(prng_() % 300);
          from_ = 0;
          break;
        case PcmRing::Cut::Crossed:
          ++tooLate_;
          break;
      }
      step_ = Step::Rest;
      return true;
    case Step::Done:
    default:
      return false;
  }
}

uint32_t RingCutStress::consume(uint32_t maxFrames) {
  int16_t buf[400 * 2];
  if (maxFrames > 400) maxFrames = 400;
  const uint32_t want = crng_() % 3 == 0 || maxFrames == 0 ? 0 : 1 + crng_() % maxFrames;
  const uint32_t got = ring_.read(reader_, buf, want);
  for (uint32_t i = 0; i < got; ++i) {
    const int tr = buf[2 * i], idx = buf[2 * i + 1];
    if (tr == seenTrack_) {
      if (idx != seenIndex_ + 1) ++errors_;  // a gap or a repeat inside a track
    } else {
      if (tr < seenTrack_ || idx != 0 || tr <= 0 || tr >= static_cast<int>(kMaxTracks)) ++errors_;
      if (tr > 0 && tr < static_cast<int>(kMaxTracks)) seen_[tr / 8] |= static_cast<uint8_t>(1u << (tr % 8));
      seenTrack_ = tr;
    }
    seenIndex_ = idx;
  }
  frames_ += got;
  return got;
}

RingCutStress::Result RingCutStress::result() const {
  Result r;
  r.tracks = written_;
  r.cuts = cuts_;
  r.tooLate = tooLate_;
  r.retries = retries_;
  r.frames = frames_;
  r.errors = errors_;
  for (size_t i = 0; i < cut_.size(); ++i) {
    uint8_t both = cut_[i] & seen_[i];
    while (both) {
      r.cutHeard += both & 1u;
      both >>= 1;
    }
  }
  return r;
}
