// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "SeekIndex.h"

#include "TrackSeek.h"
#include "TrimFeed.h"

void SeekIndex::setStorage(Entry* a, Entry* b, uint32_t capacity) {
  std::lock_guard<std::mutex> guard(lock_);
  slots_[0].e = a;
  slots_[1].e = b;
  slots_[0].cap = a ? capacity : 0;
  slots_[1].cap = b ? capacity : 0;
  clear(slots_[0]);
  clear(slots_[1]);
}

void SeekIndex::clear(Slot& s) {
  s.run = Run{};
  s.head = 0;
  s.count = 0;
  s.last = Entry{0, 0, 0};
}

void SeekIndex::reset() {
  std::lock_guard<std::mutex> guard(lock_);
  clear(slots_[0]);
  clear(slots_[1]);
  heard_ = decoding_ = 0;
}

void SeekIndex::beginJoin() {
  std::lock_guard<std::mutex> guard(lock_);
  decoding_ = static_cast<uint8_t>(1 - heard_);
  clear(slots_[decoding_]);
}

void SeekIndex::cut() {
  std::lock_guard<std::mutex> guard(lock_);
  if (decoding_ != heard_) clear(slots_[decoding_]);
  decoding_ = heard_;
}

void SeekIndex::begin(const Run& run) {
  std::lock_guard<std::mutex> guard(lock_);
  Slot& s = slots_[decoding_];
  clear(s);
  s.run = run;
}

void SeekIndex::rebase(uint64_t base, bool exact, bool origin) {
  std::lock_guard<std::mutex> guard(lock_);
  Run& r = slots_[decoding_].run;
  r.base = base;
  r.exact = exact;
  r.origin = r.origin && origin;
}

bool SeekIndex::notePass(const FrameCursor& cursor, int64_t nextSample) {
  Slot& s = slots_[decoding_];  // (only this task changes which slot decodes)
  if (s.run.kind != Kind::Mp3 || s.cap == 0) return false;
  uint32_t byte = 0, in = 0;
  if (!cursor.at(&byte, &in)) return false;
  const int64_t t0 = nextSample - static_cast<int64_t>(in);
  if (t0 > INT32_MAX || t0 < INT32_MIN) return false;  // (a 13.5 h file: the index stops there)
  if (s.count > 0 &&
      (t0 < static_cast<int64_t>(s.last.t0) + static_cast<int64_t>(kEveryFrames) * s.run.spf || byte <= s.last.byte)) {
    return false;
  }
  const uint8_t* frame = cursor.frameBytes();
  Entry e{byte, static_cast<int32_t>(t0), frame ? resumeanchor::frameHash(frame, resumeanchor::kHashBytes) : 0};
  std::lock_guard<std::mutex> guard(lock_);
  if (s.count < s.cap) {
    s.e[(s.head + s.count) % s.cap] = e;
    ++s.count;
  } else {  // full: the oldest goes
    s.e[s.head] = e;
    s.head = (s.head + 1) % s.cap;
  }
  s.last = e;
  return true;
}

void SeekIndex::advance() {
  std::lock_guard<std::mutex> guard(lock_);
  heard_ = decoding_;
}

bool SeekIndex::heardRun(Run* out) const {
  std::lock_guard<std::mutex> guard(lock_);
  const Run& r = slots_[heard_].run;
  if (r.kind == Kind::None) return false;
  *out = r;
  return true;
}

uint64_t SeekIndex::sampleAt(uint64_t base, uint32_t ringFrames, uint32_t rate, uint32_t ringRate) {
  if (ringRate == 0) return base;
  return base + static_cast<uint64_t>(ringFrames) * rate / ringRate;
}

bool SeekIndex::anchorAt(uint32_t gen, uint32_t ringFrames, uint32_t ringRate, ResumeAnchor* out) const {
  std::lock_guard<std::mutex> guard(lock_);
  const Slot& s = slots_[heard_];
  const Run& r = s.run;
  if (r.kind == Kind::None || r.gen != gen || r.rate == 0) return false;
  const uint64_t t = sampleAt(r.base, ringFrames, r.rate, ringRate);
  if (r.kind == Kind::Flac) {
    *out = ResumeAnchor{};
    out->kind = ResumeAnchor::Kind::Flac;
    out->exact = r.exact;
    out->rate = r.rate;
    out->sample = t;
    out->fileSize = r.fileSize;
    out->frameHash = static_cast<uint32_t>(r.totalSamples);
    return true;
  }
  return lookup(s, t, out);
}

bool SeekIndex::find(uint32_t pathHash, uint32_t fileSize, uint64_t t, ResumeAnchor* out) const {
  std::lock_guard<std::mutex> guard(lock_);
  for (int k = 0; k < 2; ++k) {
    const Slot& s = slots_[k == 0 ? heard_ : 1 - heard_];
    if (s.run.kind != Kind::Mp3 || s.run.pathHash != pathHash || s.run.fileSize != fileSize) continue;
    if (lookup(s, t, out)) return true;
  }
  return false;
}

bool SeekIndex::lookup(const Slot& s, uint64_t t, ResumeAnchor* out) {
  const Run& r = s.run;
  if (r.kind != Kind::Mp3 || r.spf == 0 || t > static_cast<uint64_t>(INT32_MAX)) return false;
  const auto ti = static_cast<int64_t>(t);
  // The landing: the frame with the largest t0 at or before t, of the
  // entries (in time order: a binary search) and the origin.
  int64_t landIdx = -1;  // in the entries
  uint32_t lo = 0, hi = s.count;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    if (static_cast<int64_t>(at(s, mid).t0) <= ti) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  landIdx = static_cast<int64_t>(lo) - 1;
  uint32_t landByte = 0, landHash = 0;
  int64_t landT0 = 0;
  bool onOrigin = false;
  if (landIdx >= 0 && !(r.origin && r.originT0 > at(s, static_cast<uint32_t>(landIdx)).t0 && r.originT0 <= ti)) {
    const Entry& e = at(s, static_cast<uint32_t>(landIdx));
    landByte = e.byte;
    landT0 = e.t0;
    landHash = e.hash;
  } else if (r.origin && r.originT0 <= ti) {
    landByte = r.originByte;
    landT0 = r.originT0;
    landHash = r.originHash;
    onOrigin = true;
    landIdx = -1;
  } else {
    return false;
  }
  if (ti - landT0 > static_cast<int64_t>(kMaxSkipFrames) * r.spf) return false;
  // The preroll (trackseek::kPrerollBytes): the latest entry at least the
  // history frames + 1 before it and 1 KB before the history frames, whose
  // bytes aren't known here: at most kMaxHistoryBytes. Else the origin's.
  bool found = false;
  uint32_t preroll = 0;
  const int64_t frames = static_cast<int64_t>(trackseek::historyFrames(r.spf) + 1) * r.spf;
  for (int64_t j = landIdx - 1; j >= 0 && landIdx - j <= 64; --j) {
    const Entry& e = at(s, static_cast<uint32_t>(j));
    if (landByte - e.byte >= kMaxPrerollSpan) break;
    if (landT0 - e.t0 >= frames && e.byte + trackseek::kPrerollBytes + trackseek::kMaxHistoryBytes <= landByte) {
      preroll = e.byte;
      found = true;
      break;
    }
  }
  if (!found) {
    if (!r.origin || (r.originByte > landByte && !onOrigin)) return false;
    preroll = r.originPreroll;
    if (landByte - preroll >= kMaxPrerollSpan) return false;
  }
  *out = ResumeAnchor{};
  out->kind = ResumeAnchor::Kind::Mp3;
  out->exact = r.exact;
  out->rate = r.rate;
  out->sample = t;
  out->fileSize = r.fileSize;
  out->prerollByte = preroll;
  out->frameByte = landByte;
  out->skip = static_cast<uint32_t>(ti - landT0);
  out->frameHash = landHash;
  return true;
}

uint32_t SeekIndex::entries(int slot) const {
  std::lock_guard<std::mutex> guard(lock_);
  return slots_[slot & 1].count;
}

int SeekIndex::heardSlot() const {
  std::lock_guard<std::mutex> guard(lock_);
  return heard_;
}

int SeekIndex::decodingSlot() const {
  std::lock_guard<std::mutex> guard(lock_);
  return decoding_;
}

SeekRecorder::Settled SeekRecorder::afterPass(const TrimFeed& trim, const FrameCursor& cursor) {
  if (!index_) return Settled::None;
  Settled ev = Settled::None;
  if (landingDue_) {
    const TrimFeed::Landing l = trim.landing();
    if (l == TrimFeed::Landing::Waiting) return Settled::None;
    landingDue_ = false;
    if (l == TrimFeed::Landing::NextFrame && trim.lateBy() > 0) {
      // The landing frame was lost: the start is a frame later, still on
      // the timeline.
      base_ += trim.lateBy();
      index_->rebase(static_cast<uint64_t>(base_), exact_, true);
      ev = Settled::Late;
    } else if (l == TrimFeed::Landing::Elsewhere) {
      exact_ = false;
      index_->rebase(static_cast<uint64_t>(base_), false, false);
      ev = Settled::Elsewhere;
    }
  }
  if (trim.landed()) index_->notePass(cursor, base_ + static_cast<int64_t>(trim.kept()));
  return ev;
}
