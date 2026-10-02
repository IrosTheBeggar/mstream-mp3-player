// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "TrimFeed.h"

void TrimFeed::arm(uint32_t skip, uint32_t hold) {
  skip_ = skip;
  hold_ = hold < cap_ ? hold : cap_;
  head_ = 0;
  count_ = 0;
  lastHz_ = 0;
  lastChannels_ = 0;
  pending_ = false;
  pendingHz_ = 0;
  pendingChannels_ = 0;
  skipped_ = 0;
  dropped_ = 0;
  kept_ = 0;
  firstWord_ = true;
  rateSaid_ = false;
  cursor_ = nullptr;
  landing_ = Landing::None;
  lateBy_ = 0;
  updateActive();
}

void TrimFeed::armAt(const FrameCursor* cursor, uint32_t landByte, uint32_t landLength, uint32_t spf, uint32_t skip,
                     uint32_t hold) {
  arm(0, hold);
  cursor_ = cursor;
  landByte_ = landByte;
  landLength_ = landLength;
  spf_ = spf;
  landSkip_ = skip;
  landing_ = Landing::Waiting;
  updateActive();
}

bool TrimFeed::land() {
  uint32_t byte = 0, in = 0;
  if (cursor_ && !cursor_->at(&byte, &in)) return false;  // the lead: no frame yet
  if (cursor_ && byte < landByte_) return false;           // a preroll frame
  if (cursor_ && byte == landByte_ && in <= landSkip_) {
    landing_ = Landing::Exact;
    skip_ = landSkip_ - in;  // (in is 0: a frame's first sample is always offered)
  } else if (cursor_ && byte == landByte_ + landLength_ && in == 0) {
    // The landing frame was lost: its samples come from this one on.
    landing_ = Landing::NextFrame;
    if (landSkip_ >= spf_) {
      skip_ = landSkip_ - spf_;  // still exactly there
    } else {
      lateBy_ = spf_ - landSkip_;
      skip_ = 0;
    }
  } else {
    landing_ = Landing::Elsewhere;  // a resync somewhere else: here, inexact
    skip_ = 0;
  }
  cursor_ = nullptr;
  updateActive();
  return true;
}

void TrimFeed::disarm() {
  if (landing_ == Landing::Waiting) landing_ = Landing::None;  // (how a start landed stays, for the console)
  cursor_ = nullptr;
  skip_ = 0;
  hold_ = 0;
  head_ = 0;
  count_ = 0;
  pending_ = false;
  pendingHz_ = 0;
  pendingChannels_ = 0;
  active_ = false;
}

bool TrimFeed::consumeTrimmed(const int16_t sample[2]) {
  if (rateSaid_) firstWord_ = false;  // the generator's first word is behind us
  if (landing_ == Landing::Waiting && !land()) {
    ++skipped_;  // the lead, a preroll frame: dropped
    return true;
  }
  if (pending_ && !applyPending()) return false;
  if (skip_ > 0) {
    --skip_;
    ++skipped_;
    if (skip_ == 0) updateActive();
    return true;
  }
  if (hold_ == 0) {
    if (!feed_.consume(sample)) return false;
    ++kept_;
    return true;
  }
  if (count_ < hold_) {  // the FIFO fills
    uint32_t at = head_ + count_;
    if (at >= hold_) at -= hold_;
    buf_[2 * at] = sample[0];
    buf_[2 * at + 1] = sample[1];
    ++count_;
    ++kept_;
    return true;
  }
  // Full: the oldest goes into the feed first; refused, nothing changes.
  int16_t* oldest = buf_ + 2 * head_;
  if (!feed_.consume(oldest)) return false;
  oldest[0] = sample[0];  // the newest in its place
  oldest[1] = sample[1];
  if (++head_ == hold_) head_ = 0;
  ++kept_;
  return true;
}

bool TrimFeed::releaseHeld() {
  while (count_ > 0) {
    if (!feed_.consume(buf_ + 2 * head_)) return false;
    if (++head_ == hold_) head_ = 0;
    --count_;
  }
  head_ = 0;
  return true;
}

bool TrimFeed::applyPending() {
  if (!releaseHeld()) return false;
  hold_ = 0;  // no end trim for the rest of this track
  pending_ = false;
  if (pendingHz_ > 0) feed_.setRate(pendingHz_);
  if (pendingChannels_ > 0) feed_.setChannels(pendingChannels_);
  pendingHz_ = 0;
  pendingChannels_ = 0;
  updateActive();
  return true;
}

bool TrimFeed::setRate(int hz) {
  if (hz <= 0) return feed_.setRate(hz);  // (ignored there)
  // The generator's first word: the format of everything since arm(),
  // held frames included (MP3's comes after its first frame).
  if (firstWord_ && !pending_) {
    rateSaid_ = true;
    lastHz_ = hz;
    return feed_.setRate(hz);
  }
  // A change with frames held, or another while one waits: after them.
  if (pending_ || (count_ > 0 && hz != lastHz_)) {
    lastHz_ = hz;
    pendingHz_ = hz;
    pending_ = true;
    active_ = true;
    return true;  // (MP3's generator ignores it either way)
  }
  lastHz_ = hz;
  return feed_.setRate(hz);
}

void TrimFeed::setChannels(int channels) {
  if (firstWord_ && !pending_) {  // as setRate()'s first word (MP3's comes with it)
    lastChannels_ = channels;
    feed_.setChannels(channels);
    return;
  }
  if (pending_ || (count_ > 0 && channels != lastChannels_)) {
    lastChannels_ = channels;
    pendingChannels_ = channels;
    pending_ = true;
    active_ = true;
    return;
  }
  lastChannels_ = channels;
  feed_.setChannels(channels);
}

bool TrimFeed::end(bool early) {
  if (early || pending_) {
    if (!releaseHeld()) return false;
  } else {
    dropped_ += count_;
  }
  disarm();
  return true;
}
