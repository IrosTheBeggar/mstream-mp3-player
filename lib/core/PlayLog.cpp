// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PlayLog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace playlog {

namespace cc = cardcontract;

// ---------------------------------------------------------------------------
// The slot (2.20.3).
// ---------------------------------------------------------------------------
void encode(const Slot& s, uint8_t out[kSlot]) {
  std::memset(out, 0, kSlot);
  cc::put32(out, kMagic);
  out[4] = kMajor;
  out[5] = s.kind;
  const bool play = s.kind == kKindPlay;  // (any other kind: a drop note's fields, as the reference writes it)
  if (play) {
    out[6] = s.outcome;
    out[7] = s.source;
    cc::put32(out + 8, s.seq);
    cc::put16(out + 12, s.pauseCount);
  }
  out[14] = s.clock;
  cc::put64(out + 16, s.epoch);
  std::memcpy(out + 24, s.deviceId, 6);
  cc::put64(out + 32, s.startedAt);
  cc::put32(out + 40, s.bootSeq);
  cc::put32(out + 44, s.uptimeMs);
  cc::put32(out + 48, s.playedMs);
  if (play) {
    cc::put32(out + 52, s.durationMs);
    cc::put64(out + 56, s.pathHash);
    cc::put32(out + 64, s.size);
    cc::put32(out + 68, s.fatTime);
    cc::put32(out + 72, s.generation);
  }
  cc::put32(out + 76, cc::crc32(out, kSlot - 4));
}

Verdict decode(const uint8_t* b, uint32_t n, Slot* out) {
  if (n < kSlot) return Verdict::Short;
  if (cc::get32(b) != kMagic) return Verdict::Magic;
  if (b[4] != kMajor) return Verdict::Newer;
  if (cc::crc32(b, kSlot - 4) != cc::get32(b + 76)) return Verdict::Crc;
  Slot s;
  s.kind = b[5];
  if (s.kind != kKindPlay && s.kind != kKindDrop) {
    *out = s;
    return Verdict::Kind;
  }
  s.outcome = b[6];
  s.source = b[7];
  s.seq = cc::get32(b + 8);
  s.pauseCount = cc::get16(b + 12);
  s.clock = b[14];
  s.epoch = cc::get64(b + 16);
  std::memcpy(s.deviceId, b + 24, 6);
  s.startedAt = cc::get64(b + 32);
  s.bootSeq = cc::get32(b + 40);
  s.uptimeMs = cc::get32(b + 44);
  s.playedMs = cc::get32(b + 48);
  s.durationMs = cc::get32(b + 52);
  s.pathHash = cc::get64(b + 56);
  s.size = cc::get32(b + 64);
  s.fatTime = cc::get32(b + 68);
  s.generation = cc::get32(b + 72);
  *out = s;
  return Verdict::Ok;
}

bool acknowledged(const Slot& s, const cc::msmf::Acks* acks) {
  return s.seq <= (acks ? acks->seqOf(s.deviceId, s.epoch) : 0u);
}

bool logName(int i, char* out, size_t cap) {
  int k = -1;
  if (i == 0) k = std::snprintf(out, cap, "%s", kLogPath);
  else if (i > 0 && i < kNames) k = std::snprintf(out, cap, "/.player/plays.%d.jnl", i);
  return k > 0 && static_cast<size_t>(k) < cap;
}

// ---------------------------------------------------------------------------
// The device's files.
// ---------------------------------------------------------------------------
namespace {
constexpr size_t kNameCap = 32;
}  // namespace

bool Journal::newEpochNow() {
  uint64_t e = 0;
  for (int tries = 0; tries < 8 && e == 0; ++tries) e = c_.random(c_.randomCtx);
  if (e == 0) return false;
  Ids n;
  n.epoch = e;
  if (!nvs_.storeEpoch(n)) return false;
  ids_ = n;
  seq_ = 0;
  newEpoch_ = true;
  op(Op::Epoch, nullptr, nullptr, e);
  return true;
}

bool Journal::begin(const uint8_t deviceId[6], const cc::msmf::Acks& acks) {
  closeScan();
  state_ = State::Off;
  booted_ = Booted();
  blocked_ = newEpoch_ = false;
  lastDropped_ = 0;
  std::memcpy(dev_, deviceId, 6);
  acks_ = acks;
  static const uint8_t kNoDevice[6] = {};
  if (!c_.buf || c_.bufBytes < kSlot || !c_.random || c_.capSlots == 0 || std::memcmp(dev_, kNoDevice, 6) == 0)
    return false;
  if (!nvs_.load(&ids_)) return false;
  seq_ = ids_.seq;
  if (ids_.epoch == 0 && !newEpochNow()) return false;
  bootsAtMount_ = ids_.boots;
  appended_ = false;
  settle();
  startScan();
  return state_ != State::Off;
}

void Journal::startScan() {
  closeScan();
  scanFile_ = 0;
  file_ = FileScan();
  trimmable_ = 0;
  booted_.files = booted_.slots = 0;
  booted_.bytes = 0;
  booted_.cardSeq = booted_.cardBoot = 0;
  state_ = State::Scanning;
  // A new epoch has no slot on the card (it was drawn now): with no ACKS
  // to trim by either, there is nothing to read.
  if (newEpoch_ && acks_.count == 0) finishScan();
}

void Journal::remounted() {
  if (state_ == State::Off) return;
  closeScan();  // (open across the remount: FatFs refuses it from now on)
  settle();
  if (state_ == State::Ready && appended_) return;
  ++booted_.restarts;
  startScan();
}

bool Journal::shared(const char* path) {
  const uint32_t c = fs_.firstCluster(path);
  if (c == 0) return false;
  char other[kNameCap];
  for (int i = 0; i < kNames; ++i)
    if (logName(i, other, sizeof(other)) && std::strcmp(other, path) != 0 && fs_.firstCluster(other) == c) return true;
  for (int i = 1; i <= tagstore::kMaxTwins; ++i)
    if (tagstore::twinName(kTwinNames, i, other, sizeof(other)) && std::strcmp(other, path) != 0 &&
        fs_.firstCluster(other) == c)
      return true;
  return false;
}

bool Journal::toTwin(const char* path) {
  char t[kNameCap];
  for (int i = 1; i <= tagstore::kMaxTwins; ++i) {
    if (!tagstore::twinName(kTwinNames, i, t, sizeof(t)) || exists(t)) continue;
    if (!fs_.rename(path, t)) {
      op(Op::Refused, path, t, 0);
      return false;
    }
    op(Op::Twin, path, t, 0);
    return true;
  }
  blocked_ = true;
  op(Op::Blocked, path, nullptr, 0);
  return false;
}

void Journal::collectTwins() {
  booted_.twins = 0;
  char t[kNameCap];
  for (int i = 1; i <= tagstore::kMaxTwins; ++i) {
    if (!tagstore::twinName(kTwinNames, i, t, sizeof(t)) || !exists(t)) continue;
    if (!shared(t) && fs_.remove(t)) {
      op(Op::Delete, t, "twin", 0);
    } else {
      ++booted_.twins;
    }
  }
}

bool Journal::settle() {
  blocked_ = unsettled_ = false;
  if (exists(kLogPath) && exists(kPrevPath)) {
    const uint32_t c = fs_.firstCluster(kLogPath);
    // A rename the card refuses leaves the two on one chain: no slot goes
    // to plays.jnl until one takes (append() tries again first). All nine
    // twin names taken: blocked until a disk check frees one.
    if (c != 0 && c == fs_.firstCluster(kPrevPath) && !toTwin(kLogPath)) unsettled_ = !blocked_;
  }
  collectTwins();
  return !blocked_ && !unsettled_;
}

bool Journal::scan(uint32_t maxSlots) {
  if (state_ != State::Scanning) return true;
  const uint32_t per = c_.bufBytes / kSlot;
  uint32_t budget = maxSlots;
  while (scanFile_ < kNames) {
    if (budget == 0) return false;
    // A file stays open from its first piece to its last (FatFs seeks on
    // from where it is, not from the chain's start).
    if (!scanF_) {
      char name[kNameCap];
      logName(scanFile_, name, sizeof(name));
      scanF_ = fs_.open(name, tagstore::Fs::Mode::Read);
      if (!scanF_) {
        file_ = FileScan();
        ++scanFile_;
        continue;
      }
      const uint32_t size = scanF_->size();
      file_ = FileScan();
      file_.past = size > kFileBytes;
      file_.slots = std::min(size, kFileBytes) / kSlot;
      ++booted_.files;
    }
    tagstore::File* f = scanF_;
    bool failed = false;
    while (budget > 0 && file_.next < file_.slots) {
      const uint32_t n = std::min(std::min(per, budget), file_.slots - file_.next);
      if (!f->read(file_.next * kSlot, c_.buf, n * kSlot)) {
        failed = true;
        break;
      }
      booted_.bytes += static_cast<uint64_t>(n) * kSlot;
      booted_.slots += n;
      for (uint32_t k = 0; k < n; ++k) {
        Slot s;
        const Verdict v = decode(c_.buf + k * kSlot, kSlot, &s);
        if (v == Verdict::Newer || v == Verdict::Kind) {
          file_.unjudged = true;  // a slot this firmware can't judge: never trimmed
          continue;
        }
        if (v != Verdict::Ok) continue;  // (damaged: skipped, 2.20.6)
        // The boot rules (2.20.4): every valid slot of this device and
        // epoch, a play's seq, a play's or a drop note's bootSeq.
        if (s.epoch == ids_.epoch && std::memcmp(s.deviceId, dev_, 6) == 0) {
          booted_.cardBoot = std::max(booted_.cardBoot, s.bootSeq);
          if (s.kind == kKindPlay) booted_.cardSeq = std::max(booted_.cardSeq, s.seq);
        }
        // The trim (2.20.2): every play acknowledged, one at least, a play last.
        file_.lastIsPlay = s.kind == kKindPlay;
        if (s.kind == kKindPlay) {
          file_.anyPlay = true;
          if (!acknowledged(s, &acks_)) file_.allAcked = false;
        }
      }
      file_.next += n;
      budget -= n;
    }
    if (failed) {
      // Unreadable from here: kept, and what it holds past here unknown.
      file_.unjudged = true;
      file_.next = file_.slots;
    }
    if (file_.next < file_.slots) return false;
    fs_.close(scanF_);
    scanF_ = nullptr;
    if (!file_.past && !file_.unjudged && file_.anyPlay && file_.allAcked && file_.lastIsPlay)
      trimmable_ |= 1u << scanFile_;
    file_ = FileScan();
    ++scanFile_;
  }
  return finishScan();
}

bool Journal::finishScan() {
  // 2.20.4: NVS's or the card's, whichever is higher, and one more; the
  // boot count stored before any slot uses it (NVS's at the mount: a scan
  // begun again comes to the same count, or a higher one).
  const uint32_t boots = std::max(bootsAtMount_, booted_.cardBoot) + 1;
  if (!nvs_.storeBoots(boots)) {
    state_ = State::Off;
    return true;
  }
  ids_.boots = bootSeq_ = boots;
  seq_ = std::max(seq_, booted_.cardSeq);
  // The trim, plays.1.jnl to plays.9.jnl, then plays.jnl (the reference's
  // order): a file ACKS acknowledges whole that no twin shares. None
  // without ACKS.
  for (int k = 1; k <= kNames && acks_.count > 0; ++k) {
    const int i = k % kNames;
    if (!(trimmable_ >> i & 1u)) continue;
    char name[kNameCap];
    logName(i, name, sizeof(name));
    if (shared(name) || !fs_.remove(name)) continue;
    ++booted_.trimmed;
    op(Op::Delete, name, "acknowledged", 0);
  }
  state_ = State::Ready;
  return true;
}

bool Journal::unread(uint32_t* lost) {
  tagstore::File* f = fs_.open(kPrevPath, tagstore::Fs::Mode::Read);
  if (!f) return false;
  const uint32_t slots = std::min(f->size(), kFileBytes) / kSlot;
  const uint32_t per = c_.bufBytes / kSlot;
  uint64_t gone = 0, pending = 0;
  bool ok = true;
  for (uint32_t at = 0; at < slots && ok;) {
    const uint32_t n = std::min(per, slots - at);
    ok = f->read(at * kSlot, c_.buf, n * kSlot);
    for (uint32_t k = 0; ok && k < n; ++k) {
      Slot s;
      if (decode(c_.buf + k * kSlot, kSlot, &s) != Verdict::Ok) continue;
      if (s.kind == kKindDrop) {
        pending += s.playedMs;  // a note the software hasn't read: its count goes on
      } else if (acknowledged(s, &acks_)) {
        pending = 0;  // read: the notes before it were too
      } else {
        ++gone;
      }
    }
    at += n;
  }
  fs_.close(f);
  *lost = static_cast<uint32_t>(std::min<uint64_t>(gone + pending, 0xFFFFFFFFull));
  return ok;
}

Journal::Wrote Journal::rotate(const uint8_t slot[kSlot], const Slot& play) {
  uint32_t lost = 0;
  if (exists(kPrevPath)) {
    // Step 1: what plays.1.jnl holds that no software read, then it goes
    // (or, a twin on its chain, to a twin name: a delete would free the
    // twin's clusters).
    if (!unread(&lost)) return Wrote::Failed;
    if (!shared(kPrevPath)) {
      if (!fs_.remove(kPrevPath)) return Wrote::Failed;
      op(Op::Delete, kPrevPath, "rotation", 0);
    } else if (!toTwin(kPrevPath)) {
      return blocked_ ? Wrote::Blocked : Wrote::Failed;
    }
  }
  // Step 2.
  if (!fs_.rename(kLogPath, kPrevPath)) return Wrote::Failed;
  op(Op::Rename, kLogPath, kPrevPath, 0);
  // Step 3: the drop note (the play's own dates: 2.20.3), then the play,
  // one write.
  uint8_t out[2 * kSlot];
  uint32_t n = 0;
  lastDropped_ = 0;
  if (lost) {
    Slot note;
    note.kind = kKindDrop;
    note.clock = play.clock;
    note.epoch = play.epoch;
    std::memcpy(note.deviceId, play.deviceId, 6);
    note.startedAt = play.startedAt;
    note.bootSeq = play.bootSeq;
    note.uptimeMs = play.uptimeMs;
    note.playedMs = lost;
    encode(note, out);
    n = kSlot;
    lastDropped_ = lost;
    op(Op::Note, nullptr, nullptr, lost);
  }
  std::memcpy(out + n, slot, kSlot);
  n += kSlot;
  tagstore::File* f = fs_.open(kLogPath, tagstore::Fs::Mode::Create);
  bool ok = f && f->write(0, out, n) && f->sync();
  if (f) ok = fs_.close(f) && ok;
  return ok ? Wrote::Ok : Wrote::Failed;
}

Journal::Wrote Journal::append(Slot& play) {
  if (state_ != State::Ready) return Wrote::Off;
  appended_ = true;
  if (seq_ == 0xFFFFFFFFu) {
    // 2.20.4: a seq past 2^32 - 1 takes a new epoch, whose first boot this is.
    if (!newEpochNow() || !nvs_.storeBoots(1)) return Wrote::Nvs;
    ids_.boots = bootSeq_ = 1;
  }
  // The seq in NVS before the slot that uses it (2.20.4): one never used
  // twice, whatever becomes of the append.
  const uint32_t seq = seq_ + 1;
  if (!nvs_.storeSeq(seq)) return Wrote::Nvs;
  seq_ = ids_.seq = seq;
  play.kind = kKindPlay;
  play.seq = seq;
  play.epoch = ids_.epoch;
  std::memcpy(play.deviceId, dev_, 6);
  play.bootSeq = bootSeq_;
  if (unsettled_) settle();  // (the rename the card refused: again)
  if (blocked_ || unsettled_) {
    op(Op::Lost, nullptr, nullptr, seq);
    return blocked_ ? Wrote::Blocked : Wrote::Failed;
  }
  uint8_t raw[kSlot];
  encode(play, raw);
  Wrote w = Wrote::Failed;
  if (tagstore::File* f = fs_.open(kLogPath, tagstore::Fs::Mode::Update)) {
    const uint32_t slots = f->size() / kSlot;
    if (slots + 1 > c_.capSlots) {
      fs_.close(f);
      w = rotate(raw, play);
    } else {
      // At the slot boundary: a cut append's part slot (under 80 bytes) is
      // written over, so appends stay on slot boundaries.
      bool ok = f->write(slots * kSlot, raw, kSlot) && f->sync();
      ok = fs_.close(f) && ok;
      w = ok ? Wrote::Ok : Wrote::Failed;
    }
  } else if (!exists(kLogPath)) {
    tagstore::File* n = fs_.open(kLogPath, tagstore::Fs::Mode::Create);
    bool ok = n && n->write(0, raw, kSlot) && n->sync();
    if (n) ok = fs_.close(n) && ok;
    w = ok ? Wrote::Ok : Wrote::Failed;
  }
  op(w == Wrote::Ok ? Op::Play : Op::Lost, nullptr, nullptr, seq);
  return w;
}

// ---------------------------------------------------------------------------
// A play.
// ---------------------------------------------------------------------------
void Tracker::began(uint32_t key, uint32_t track, uint32_t fromMs) {
  if (open_) ended(End::Stopped);  // (the controller ends one first)
  open_ = true;
  cur_ = Played();
  logged_ = env_.file(key, track, &cur_.file);
  cur_.startMono = env_.monoMs();
  // Counted from where it was asked to start; a start the backend hasn't
  // taken up yet isn't read (tick(): positionKnown()).
  havePos_ = true;
  lastPos_ = fromMs;
  lastMono_ = cur_.startMono;
}

void Tracker::ended(End how) {
  if (!open_) return;
  open_ = false;
  ++counts_.ended;
  if (!logged_) {
    ++counts_.notLogged;
    return;
  }
  cur_.outcome = static_cast<uint8_t>(how);
  // At its end: what its last look didn't see of it (a pass's worth).
  if (how == End::Completed && havePos_ && cur_.durationMs > lastPos_ && cur_.durationMs - lastPos_ <= kTailMs)
    cur_.playedMs += cur_.durationMs - lastPos_;
  if (cur_.durationMs == 0) cur_.durationMs = cur_.file.hintMs;
  if (cur_.playedMs < kMinPlayedMs) {
    ++counts_.short_;
    return;
  }
  if (count_ == kPending) {
    ++counts_.overflow;
    return;
  }
  ring_[(head_ + count_) % kPending] = cur_;
  ++count_;
  ++counts_.queued;
}

void Tracker::tick(PlayState state, uint32_t positionMs, bool positionKnown, uint32_t durationMs) {
  const uint64_t now = env_.monoMs();
  const bool playing = state == PlayState::Playing;
  if (open_) {
    if (wasPlaying_ && state == PlayState::Paused && cur_.pauses < 0xFFFF) ++cur_.pauses;
    if (!positionKnown) {
      havePos_ = false;  // a start not taken up: its position may be the track before's
    } else if (playing) {
      // The audio played: the position's step, when it is no more than the
      // board's clock moved (a seek's jump, a step back, aren't).
      if (havePos_ && positionMs >= lastPos_ && positionMs - lastPos_ <= now - lastMono_ + kSlackMs)
        cur_.playedMs += positionMs - lastPos_;
      havePos_ = true;
      lastPos_ = positionMs;
      lastMono_ = now;
      if (durationMs) cur_.durationMs = durationMs;
    }
  }
  wasPlaying_ = playing;
}

void Tracker::pop() {
  if (count_ == 0) return;
  head_ = (head_ + 1) % kPending;
  --count_;
}

// ---------------------------------------------------------------------------
// The gate.
// ---------------------------------------------------------------------------
WriteGate::Wait WriteGate::update(const In& in) {
  if (!primed_) {
    primed_ = true;
    trackSeq_ = in.trackSeq;
    underruns_ = in.underruns;
  }
  if (in.trackSeq != trackSeq_) {
    trackSeq_ = in.trackSeq;
    settling_ = true;
    settleFrom_ = in.nowMs;
  }
  if (in.underruns != underruns_) {
    underruns_ = in.underruns;
    underrun_ = true;
    underrunAt_ = in.nowMs;
  }
  if (settling_ && in.nowMs - settleFrom_ >= kSettleMs) settling_ = false;
  if (underrun_ && in.nowMs - underrunAt_ >= kUnderrunMs) underrun_ = false;
  if (settling_ || (in.playing && in.seeking)) return Wait::Start;
  if (in.playing && (in.decoderAtEnd || (in.trackLeftMs != 0 && in.trackLeftMs < kEndMs))) return Wait::TrackEnd;
  if (in.playing && in.ringCapacityMs != 0 && in.ringMs * 2 < in.ringCapacityMs) return Wait::Ring;
  if (underrun_) return Wait::Underrun;
  return Wait::None;
}

const char* WriteGate::waitName(Wait w) {
  switch (w) {
    case Wait::None: return "none";
    case Wait::Start: return "a start";
    case Wait::TrackEnd: return "the gapless window";
    case Wait::Ring: return "the ring";
    case Wait::Underrun: return "an underrun";
  }
  return "?";
}

}  // namespace playlog
