// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "FrameCursor.h"
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
// The generator's first word on its format since arm() goes straight to
// the feed, frames held or not: everything it handed over before it is in
// that format already. MP3's generator says its rate and channels only
// after its first decoded frame, so a start after a seek (a skip of 1)
// already holds frames when it does. (Taken for a change, that word used to
// turn the end trim off for every seek and resume start.)
//
// A rate or channel change in the middle of a track (setRate()/
// setChannels() with frames held) waits: the held frames go into the feed
// at the old format first, then the change, and the end trim is off for
// the rest of that track (the padding is the end's, at whatever rate; a
// file that changes its rate isn't LAME's). MP3's generator makes such a
// call once and ignores its result, so it can't be refused.
//
// A start by a plan (docs/SEEK.md section 4.2: a resume point's anchor, a
// seek): armAt(). The decoder was handed a preroll frame before the
// landing frame, so that the landing frame decodes bit for bit (its bit
// reservoir and the filterbank's history come from the frames before it).
// Until the cursor (FrameCursor: the generator's own state) says the
// landing frame, every sample is dropped: the generator's lead, the
// preroll frames. Then `skip` more, then as arm(0, hold). The landing frame
// lost (bad data, a reservoir the preroll didn't cover): the cursor says
// the frame after it, and the start lands a frame late (lateBy()); any
// other frame: it lands there, inexact (Elsewhere). Once landed, nothing
// changes for the rest of the track: the cursor isn't asked again.
//
// kept() counts the samples taken after the start's skip (into the feed or
// the hold), also with nothing to trim: SeekIndex's clock (the trimmed
// timeline's sample of the next one is the run's base + kept()).
//
// One task (the decode task). ~100 B; the FIFO is the caller's (PSRAM).
class TrimFeed {
public:
  // The most an MP3's padding can hold back: its 12-bit field.
  static constexpr uint32_t kMaxHold = 4095;

  // A planned start's landing (armAt()).
  enum class Landing : uint8_t {
    None,       // arm(): no landing (from the top, or a FLAC)
    Waiting,    // dropping until the cursor says the landing frame
    Exact,      // landed on the landing frame
    NextFrame,  // on the frame after it (the landing frame was lost): lateBy() later
    Elsewhere,  // on another frame: the plan's timeline is lost
  };

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
  // A start by a plan (see the class): every frame dropped until `cursor`
  // says the frame at `landByte` (`landLength` bytes, `spf` samples), then
  // `skip` more, then as arm(0, hold). The cursor must outlive the landing.
  void armAt(const FrameCursor* cursor, uint32_t landByte, uint32_t landLength, uint32_t spf, uint32_t skip,
             uint32_t hold);
  // Nothing trimmed: every frame straight into the feed.
  void disarm();
  // Whether consume() has anything to do (else RingOutput calls the feed
  // straight, one branch per frame).
  bool active() const { return active_; }

  // ---- the generator's side (RingOutput) ----
  bool consume(const int16_t sample[2]) {
    if (!active_) {
      if (!feed_.consume(sample)) return false;
      ++kept_;
      return true;
    }
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

  // ---- a planned start (armAt()) and the run index ----
  // How the last planned start landed (kept through end() and disarm(),
  // for the console; arm() clears it).
  Landing landing() const { return landing_; }
  // NextFrame: samples later than planned (the start moves by that).
  uint32_t lateBy() const { return lateBy_; }
  // Past the landing and the start's skip: kept() counts from the start.
  bool landed() const { return landing_ != Landing::Waiting && skip_ == 0; }
  // Samples taken since the start's skip (into the feed or the hold).
  uint32_t kept() const { return kept_; }

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
  // The landing phase: true once the cursor says the landing frame (or
  // after it): this sample is the first past the preroll.
  bool land();
  void updateActive() {
    active_ = landing_ == Landing::Waiting || skip_ > 0 || hold_ > 0 || count_ > 0 || pending_;
  }

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
  bool firstWord_ = false;  // since arm(), no frame consumed after a rate was said
  bool rateSaid_ = false;   // since arm()
  uint64_t skipped_ = 0;
  uint64_t dropped_ = 0;
  uint32_t kept_ = 0;
  // A planned start (armAt()).
  const FrameCursor* cursor_ = nullptr;
  uint32_t landByte_ = 0;
  uint32_t landLength_ = 0;
  uint32_t spf_ = 0;
  uint32_t landSkip_ = 0;
  uint32_t lateBy_ = 0;
  Landing landing_ = Landing::None;
};
