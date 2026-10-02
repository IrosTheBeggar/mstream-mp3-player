// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "RingFeed.h"

// Gapless trimming (docs/GAPLESS.md section 4.4), in front of RingFeed, at
// the track's own rate (before the converter):
// - the start: `skip` frames are taken and dropped: the generator's lead
//   (ESP8266Audio's {0,0}), and for an MP3 with a LAME tag its encoder
//   delay plus libmad's 529;
// - the end: the last `hold` frames are kept back in a FIFO (an MP3's
//   padding less 529). A frame goes into the feed only once `hold` newer
//   ones have come; at the end of the file (end()) what is still held is
//   the padding, dropped. A hold, not a count to the end: it works from
//   wherever the decoder started (a resume point, a TOC seek) and isn't
//   fooled by frames lost to bad data in the middle.
//
// It keeps RingFeed's refusal contract: when the feed refuses the frame
// that has to go in first, consume() refuses the new one and changes
// nothing, so the generator offers the same sample again. Skipped and held
// frames don't count against the feed's pass budget (at most 4,095 + 530
// frames at a track's start, once).
//
// A rate or channel change in the middle of a track (setRate()/
// setChannels() with frames held) waits: the held frames go into the feed
// at the old format first, then the change, and the end trim is off for
// the rest of that track (the padding is the end's, at whatever rate; a
// file that changes its rate isn't LAME's). MP3's generator makes such a
// call once and ignores its result, so it can't be refused.
//
// One task (the decode task). ~48 B; the FIFO is the caller's (PSRAM).
class TrimFeed {
public:
  // The most an MP3's padding can hold back: its 12-bit field.
  static constexpr uint32_t kMaxHold = 4095;

  explicit TrimFeed(RingFeed& feed) : feed_(feed) {}

  // The FIFO: `capacity` frames, interleaved stereo (the firmware's is in
  // PSRAM, kMaxHold frames). nullptr: no end trim at all (holds are 0).
  void setHoldBuffer(int16_t* frames, uint32_t capacity) {
    buf_ = frames;
    cap_ = frames ? capacity : 0;
  }

  // A track starts (its decoder hands frames over from now): drop the first
  // `skip`, keep the last `hold` back (clamped to the FIFO). Forgets
  // anything held for the track before.
  void arm(uint32_t skip, uint32_t hold);
  // Nothing trimmed: every frame straight into the feed.
  void disarm();
  // Whether consume() has anything to do (else RingOutput calls the feed
  // straight, one branch per frame).
  bool active() const { return active_; }

  // ---- the generator's side (RingOutput) ----
  bool consume(const int16_t sample[2]) {
    if (!active_) return feed_.consume(sample);
    return consumeTrimmed(sample);
  }
  bool setRate(int hz);
  void setChannels(int channels);

  // ---- the decode task ----
  // The track's source ended. `early` false (its natural end): what is held
  // is the padding: dropped. `early` true (a decode error, a file cut short:
  // the held frames are real audio): they go into the feed, which may be
  // full, or the pass's budget spent (RingFeed::setBudget() first): false:
  // call again. A format change still waiting is real audio too: flushed
  // the same way. True: done, disarmed.
  bool end(bool early);

  // ---- for the console (G) ----
  uint32_t skipLeft() const { return skip_; }
  uint32_t holding() const { return count_; }
  uint32_t holdArmed() const { return hold_; }
  uint64_t skipped() const { return skipped_; }  // since arm()
  uint64_t dropped() const { return dropped_; }  // since arm(): the held frames at the end

private:
  bool consumeTrimmed(const int16_t sample[2]);
  // The held frames into the feed, oldest first; false: refused (the ring
  // is full), the rest still held.
  bool releaseHeld();
  // A waiting format change: the held frames first, then the change.
  bool applyPending();
  void updateActive() { active_ = skip_ > 0 || hold_ > 0 || count_ > 0 || pending_; }

  RingFeed& feed_;
  int16_t* buf_ = nullptr;
  uint32_t cap_ = 0;
  uint32_t skip_ = 0;
  uint32_t hold_ = 0;   // the FIFO's length for this track
  uint32_t head_ = 0;   // the oldest held frame
  uint32_t count_ = 0;  // frames held
  int lastHz_ = 0;      // the format the generator last said
  int lastChannels_ = 0;
  int pendingHz_ = 0;   // a change waiting for the held frames (0: none)
  int pendingChannels_ = 0;
  bool pending_ = false;
  bool active_ = false;
  uint64_t skipped_ = 0;
  uint64_t dropped_ = 0;
};
