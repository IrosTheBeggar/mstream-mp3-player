// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <random>
#include <vector>

#include "PcmRing.h"

// A stress test of PcmRing::cutBack() against a reader on another task
// (docs/GAPLESS.md section 5.1). The producer writes tracks of frames
// tagged (track, index), writes part of the next one past each track's
// end (J), and at random takes it back out (cutBack(J), tried again while
// Pending) and writes another track in its place. The reader checks that
// every track starts at its first frame and runs on without a gap or a
// repeat, the tracks in order; at the end no frame of a cut track may have
// been read. The host test (test_pcm_ring) runs it on two threads; the
// console's Gx on the ESP32's two cores, whose memory model is the one
// that matters (the fence and the reading mark are a Dekker pair).
//
// Step by step, never blocking: produce() does one write or one cut
// attempt, consume() one read, so either side can run in a task that
// yields. The two sides share nothing but the ring until result().
class RingCutStress {
public:
  static constexpr uint32_t kMaxTracks = 32000;  // tags are int16

  RingCutStress(PcmRing& ring, uint8_t reader, uint32_t seed, uint32_t tracks);

  // ---- the producer's task ----
  // One step; false once every track is written.
  bool produce();
  // ---- the reader's task ----
  // One read of up to `maxFrames` (a random amount below it, often none);
  // the frames read.
  uint32_t consume(uint32_t maxFrames);

  // ---- after both have stopped (the ring read empty) ----
  struct Result {
    uint32_t tracks = 0;    // written whole
    uint32_t cuts = 0;      // Done
    uint32_t tooLate = 0;   // Crossed
    uint32_t retries = 0;   // Pending
    uint64_t frames = 0;    // read
    uint32_t errors = 0;    // a gap, a repeat, a track not from its start, out of order
    uint32_t cutHeard = 0;  // tracks cut whose frames were read anyway
    bool ok() const { return errors == 0 && cutHeard == 0; }
  };
  Result result() const;

private:
  enum class Step : uint8_t { Rest, Ahead, Decide, Done };
  bool write(int track, int to);

  PcmRing& ring_;
  const uint8_t reader_;
  const uint32_t total_;

  // The producer's.
  std::mt19937 prng_;
  Step step_ = Step::Rest;
  int track_ = 1;       // writing this one
  int from_ = 0;        // ... from this frame
  int len_ = 0;         // ... of this many
  int part_ = 0;        // the next one: this much decoded ahead
  uint32_t j_ = 0;      // J: the current track's end
  std::vector<uint8_t> cut_;
  uint32_t written_ = 0, cuts_ = 0, tooLate_ = 0, retries_ = 0;

  // The reader's.
  std::mt19937 crng_;
  int seenTrack_ = 0;
  int seenIndex_ = -1;
  std::vector<uint8_t> seen_;
  uint64_t frames_ = 0;
  uint32_t errors_ = 0;
};
