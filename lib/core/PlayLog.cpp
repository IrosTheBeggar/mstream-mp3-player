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

// The journal's questions of the card (which of the log's files are there,
// and on which chains), the first it fails noted: from there the answers
// say nothing ("no such file", "no file shares this chain" may be only the
// read that failed), and nothing is removed, renamed or made on them
// (tagstore's rule for its own files: TagStore.h, the cut-rename rule).
class Journal::Asking {
public:
  explicit Asking(tagstore::Fs& fs) : fs_(fs) {}
  tagstore::Fs::Entry of(const char* path) {
    const tagstore::Fs::Entry e = failed_ ? tagstore::Fs::Entry() : fs_.entry(path);
    failed_ = failed_ || e.cardFailed;
    return e;
  }
  bool failed() const { return failed_; }

private:
  tagstore::Fs& fs_;
  bool failed_ = false;
};

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
  retry_ = false;
  owed_ = 0;
  unreadFails_ = 0;
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
  startScan(false);
  return state_ != State::Off;
}

void Journal::startScan(bool again) {
  closeScan();
  scanFile_ = 0;
  file_ = FileScan();
  trimmable_ = foldedBefore_ = 0;
  booted_.files = booted_.slots = 0;
  booted_.bytes = 0;
  booted_.cardSeq = booted_.cardBoot = 0;
  booted_.partial = 0;
  state_ = State::Scanning;
  if (c_.folds) {
    // (Their folds from a scan before a remount dropped: its slots come
    // again. And the scan reads on a new epoch too: another epoch's slots
    // and other players' are folded.)
    c_.folds->scanBegan(again);
    return;
  }
  // A new epoch has no slot on the card (it was drawn now): with no ACKS
  // to trim by either, there is nothing to read.
  if (newEpoch_ && acks_.count == 0) finishScan();
}

void Journal::partialFile() {
  ++booted_.partial;
  if (c_.folds) c_.folds->partial();
}

void Journal::remounted() {
  if (state_ == State::Off) return;
  closeScan();  // (open across the remount: FatFs refuses it from now on)
  settle();
  if (state_ == State::Ready && appended_) return;
  ++booted_.restarts;
  startScan(true);
}

bool Journal::shared(Asking& a, const char* path) {
  const uint32_t c = a.of(path).firstCluster;
  if (c == 0) return false;
  char other[kNameCap];
  for (int i = 0; i < kNames; ++i)
    if (logName(i, other, sizeof(other)) && std::strcmp(other, path) != 0 && a.of(other).firstCluster == c) return true;
  for (int i = 1; i <= tagstore::kMaxTwins; ++i)
    if (tagstore::twinName(kTwinNames, i, other, sizeof(other)) && std::strcmp(other, path) != 0 &&
        a.of(other).firstCluster == c)
      return true;
  return false;
}

bool Journal::toTwin(Asking& a, const char* path) {
  char t[kNameCap];
  for (int i = 1; i <= tagstore::kMaxTwins; ++i) {
    if (!tagstore::twinName(kTwinNames, i, t, sizeof(t))) continue;
    const bool taken = a.of(t).there;
    if (a.failed()) return false;  // (whether the name is free isn't known)
    if (taken) continue;
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

void Journal::collectTwins(Asking& a) {
  booted_.twins = 0;
  char t[kNameCap];
  for (int i = 1; i <= tagstore::kMaxTwins; ++i) {
    if (!tagstore::twinName(kTwinNames, i, t, sizeof(t))) continue;
    const bool there = a.of(t).there;
    const bool sharesChain = there && shared(a, t);
    // (A question the card failed: the rest uncounted, none removed; the
    // next settle() asks again.)
    if (a.failed()) return;
    if (!there) continue;
    if (!sharesChain && fs_.remove(t)) {
      op(Op::Delete, t, "twin", 0);
    } else {
      ++booted_.twins;
    }
  }
}

bool Journal::settle() {
  blocked_ = unsettled_ = unanswered_ = false;
  Asking a(fs_);
  const tagstore::Fs::Entry log = a.of(kLogPath), prev = a.of(kPrevPath);
  // A rename the card refuses leaves the two on one chain: no slot goes
  // to plays.jnl until one takes (append() tries again first). All nine
  // twin names taken: blocked until a disk check frees one.
  if (!a.failed() && log.there && prev.there && log.firstCluster != 0 && log.firstCluster == prev.firstCluster &&
      !toTwin(a, kLogPath))
    unsettled_ = !blocked_;
  if (!a.failed()) collectTwins(a);
  // A question the card failed: nothing was done on its answer, and which
  // names share a chain isn't known. As for a rename it refused: no slot
  // until a settle() is answered (append() asks again first).
  if (a.failed()) unsettled_ = unanswered_ = true;
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
      bool cardFailed = false;
      scanF_ = fs_.openRead(name, &cardFailed);
      if (!scanF_) {
        // Taken for a missing file, as it always was; an open the card
        // failed is a file not read whole for the folds (its slots may be
        // there).
        if (cardFailed) partialFile();
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
        if (c_.folds) c_.folds->slot(s, scanFile_);
        // The boot rules (2.20.4): every valid slot of this device and
        // epoch, a play's seq, a play's or a drop note's bootSeq.
        if (s.epoch == ids_.epoch && std::memcmp(s.deviceId, dev_, 6) == 0) {
          booted_.cardBoot = std::max(booted_.cardBoot, s.bootSeq);
          if (s.kind == kKindPlay) booted_.cardSeq = std::max(booted_.cardSeq, s.seq);
        }
        // The trim (2.20.2): every play acknowledged, one at least, a play
        // last; with folds, every play at or under the card's MARK too
        // (or saved before the trim: finishScan()).
        file_.lastIsPlay = s.kind == kKindPlay;
        if (s.kind == kKindPlay) {
          file_.anyPlay = true;
          if (!acknowledged(s, &acks_)) file_.allAcked = false;
          if (file_.allFolded && c_.folds && !c_.folds->covered(s)) file_.allFolded = false;
        }
      }
      file_.next += n;
      budget -= n;
    }
    if (failed) {
      // Unreadable from here: kept, and what it holds past here unknown.
      file_.unjudged = true;
      file_.next = file_.slots;
      partialFile();
    }
    if (file_.next < file_.slots) return false;
    fs_.close(scanF_);
    scanF_ = nullptr;
    if (!file_.past && !file_.unjudged && file_.anyPlay && file_.allAcked && file_.lastIsPlay)
      trimmable_ |= 1u << scanFile_;
    if (file_.allFolded) foldedBefore_ |= 1u << scanFile_;
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
  // The folds' save (docs/LISTENING-STATS.md 5.5), after the boot rules and
  // before the trim: no slot is written before Ready, so nothing changes
  // under it. Never after a partial scan (5.6): its MARK would pass slots
  // it never read.
  const bool save = c_.folds && booted_.partial == 0;
  if (!nvs_.storeBoots(boots)) {
    // (No log this boot, and no trim; the fold is whole all the same.)
    if (save) c_.folds->save(Folds::Why::Boot);
    state_ = State::Off;
    return true;
  }
  ids_.boots = bootSeq_ = boots;
  seq_ = std::max(seq_, booted_.cardSeq);
  // With folds a file goes only when the card's MARK covers it: after a
  // save every file the scan read; else (failed, refused, or none after a
  // partial scan) those the MARK read at the boot covered.
  if (c_.folds) {
    const bool saved = save && c_.folds->save(Folds::Why::Boot);
    trimmable_ &= saved ? ~0u : foldedBefore_;
  }
  // The trim, plays.1.jnl to plays.9.jnl, then plays.jnl (the reference's
  // order): a file ACKS acknowledges whole that no twin shares. None
  // without ACKS.
  for (int k = 1; k <= kNames && acks_.count > 0; ++k) {
    const int i = k % kNames;
    if (!(trimmable_ >> i & 1u)) continue;
    char name[kNameCap];
    logName(i, name, sizeof(name));
    // (A question the card fails says nothing of who shares its chain: the
    // file stays, for the next boot's trim.)
    Asking a(fs_);
    const bool sharesChain = shared(a, name);
    if (a.failed() || sharesChain || !fs_.remove(name)) continue;
    ++booted_.trimmed;
    op(Op::Delete, name, "acknowledged", 0);
  }
  state_ = State::Ready;
  return true;
}

bool Journal::unread(uint32_t* lost, bool* uncovered) {
  *lost = 0;
  *uncovered = false;
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
      if (s.kind == kKindPlay && !*uncovered && c_.folds && !c_.folds->covered(s)) *uncovered = true;
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

uint32_t Journal::record(uint8_t out[2 * kSlot], bool note) const {
  uint32_t n = 0;
  if (note) {
    // The drop note: the play's own dates (2.20.3), the count owed.
    Slot play, drop;
    decode(last_, kSlot, &play);
    drop.kind = kKindDrop;
    drop.clock = play.clock;
    drop.epoch = play.epoch;
    std::memcpy(drop.deviceId, play.deviceId, 6);
    drop.startedAt = play.startedAt;
    drop.bootSeq = play.bootSeq;
    drop.uptimeMs = play.uptimeMs;
    drop.playedMs = owed_;
    encode(drop, out);
    n = kSlot;
  }
  std::memcpy(out + n, last_, kSlot);
  return n + kSlot;
}

Journal::Wrote Journal::write(tagstore::File* f, uint32_t at, const uint8_t* rec, uint32_t n, bool note) {
  bool ok = f && f->write(at, rec, n) && f->sync();
  if (f) ok = fs_.close(f) && ok;
  if (!ok) return Wrote::Failed;
  if (note) {
    lastDropped_ = owed_;
    op(Op::Note, nullptr, nullptr, owed_);
    owed_ = 0;
  }
  return Wrote::Ok;
}

Journal::Wrote Journal::rotate() {
  // (A question the card fails, here or below: nothing removed or renamed
  // on it. The slot isn't written and the log is as it was; again() starts
  // the rotation over.)
  Asking a(fs_);
  const bool prevThere = a.of(kPrevPath).there;
  if (a.failed()) return Wrote::Failed;
  if (prevThere) {
    // Step 1: what plays.1.jnl holds that no software read, then it goes
    // (or, a twin on its chain, to a twin name: a delete would free the
    // twin's clusters).
    uint32_t lost = 0;
    bool uncovered = false;
    const bool whole = unread(&lost, &uncovered);
    if (whole) {
      unreadFails_ = 0;
    } else {
      // A read the card fails now may work at the next try: nothing goes
      // on it. One that fails every time (a sector gone bad) would hold
      // every later play back, each paying this read: at the
      // kUnreadTries-th rotation in a row it goes on with what it read.
      if (unreadFails_ < kUnreadTries) ++unreadFails_;
      if (unreadFails_ < kUnreadTries) return Wrote::Failed;
      op(Op::Unread, kPrevPath, nullptr, lost);
    }
    // The folds' save (docs/LISTENING-STATS.md 5.5) before plays.1.jnl
    // leaves the log's names, deleted or renamed to a twin: when it holds a
    // play the card's MARK doesn't cover, or wasn't read whole (what it
    // holds past there unknown). Never after a partial scan (no save this
    // session). The rotation goes on whatever the save comes to: plays are
    // never held back for the stats.
    if (c_.folds && booted_.partial == 0 && (uncovered || !whole)) c_.folds->save(Folds::Why::Rotation);
    const bool sharesChain = shared(a, kPrevPath);
    if (a.failed()) return Wrote::Failed;
    if (!sharesChain) {
      if (!fs_.remove(kPrevPath)) return Wrote::Failed;
      op(Op::Delete, kPrevPath, "rotation", 0);
    } else if (!toTwin(a, kPrevPath)) {
      return blocked_ ? Wrote::Blocked : Wrote::Failed;
    }
    // Its slots are out of the log's names: their count is owed a drop
    // note from here, whatever the next steps come to (a step the card
    // fails is tried again, and the note goes with the slot that lands).
    unreadFails_ = 0;
    owed_ = lost > 0xFFFFFFFFu - owed_ ? 0xFFFFFFFFu : owed_ + lost;
  }
  // Step 2. A rename the card fails may have left both names on one chain
  // (its two directory writes): the cut-rename rule again before the next
  // slot, as at a mount.
  if (!fs_.rename(kLogPath, kPrevPath)) {
    unsettled_ = true;
    return Wrote::Failed;
  }
  op(Op::Rename, kLogPath, kPrevPath, 0);
  // Step 3: the drop note (the play's own dates: 2.20.3), then the play,
  // one write.
  uint8_t rec[2 * kSlot];
  const bool note = owed_ != 0;
  const uint32_t n = record(rec, note);
  lastDropped_ = 0;
  return write(fs_.open(kLogPath, tagstore::Fs::Mode::Create), 0, rec, n, note);
}

Journal::Wrote Journal::place(bool again) {
  if (unsettled_) settle();  // (the rename the card refused, a question it failed: again)
  if (blocked_ || unsettled_) return blocked_ ? Wrote::Blocked : Wrote::Failed;
  uint8_t rec[2 * kSlot];
  tagstore::File* f = fs_.open(kLogPath, tagstore::Fs::Mode::Update);
  if (!f) {
    // No plays.jnl opened: a new one only when the card says there is none.
    // A question it fails says nothing (the open may have been that same
    // fault), and Create would empty a log that is there, every play not
    // yet sent with it.
    const tagstore::Fs::Entry e = fs_.entry(kLogPath);
    if (e.cardFailed || e.there) return Wrote::Failed;
    const bool note = owed_ != 0;
    const uint32_t n = record(rec, note);
    return write(fs_.open(kLogPath, tagstore::Fs::Mode::Create), 0, rec, n, note);
  }
  const uint32_t slots = f->size() / kSlot;
  if (again && slots > 0) {
    // The slot's first try may have landed, only its sync or its close
    // failing: the log's last slot is then these very bytes (and a drop
    // note written with it, the slot before). They go over themselves,
    // where they are, and are synced: one slot, with the sync it was owed.
    uint8_t tail[2 * kSlot];
    const uint32_t k = slots > 1 ? 2 : 1;
    if (!f->read((slots - k) * kSlot, tail, k * kSlot)) {
      fs_.close(f);  // (a read the card fails says nothing of what landed)
      return Wrote::Failed;
    }
    if (std::memcmp(tail + (k - 1) * kSlot, last_, kSlot) == 0) {
      bool note = false;
      if (owed_ != 0 && k == 2) {
        record(rec, true);
        note = std::memcmp(tail, rec, kSlot) == 0;
      }
      const uint32_t n = record(rec, note);
      return write(f, slots * kSlot - n, rec, n, note);
    }
  }
  if (slots + 1 > c_.capSlots) {
    fs_.close(f);
    return rotate();
  }
  // At the slot boundary: a cut append's part slot (under 80 bytes) is
  // written over, so appends stay on slot boundaries. A drop note a failed
  // rotation still owes goes before the slot when both fit under the cap.
  const bool note = owed_ != 0 && slots + 2 <= c_.capSlots;
  const uint32_t n = record(rec, note);
  return write(f, slots * kSlot, rec, n, note);
}

Journal::Wrote Journal::put(bool again) {
  const Wrote w = place(again);
  retry_ = w == Wrote::Failed;
  op(w == Wrote::Ok ? Op::Play : Op::Lost, nullptr, nullptr, cc::get32(last_ + 8));
  return w;
}

Journal::Wrote Journal::append(Slot& play) {
  if (state_ != State::Ready) return Wrote::Off;
  appended_ = true;
  retry_ = false;  // (a slot not tried again by now never is)
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
  // The slot as the card gets it, at this try and at any after it.
  encode(play, last_);
  return put(false);
}

Journal::Wrote Journal::again() {
  if (state_ != State::Ready || !retry_) return Wrote::Off;
  return put(true);
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
    } else {
      // Not playing (paused, waiting): the clock that moves now is no
      // allowance for the position's next step. A seek while paused, or
      // one right after, as long as the pause was, isn't time played; what
      // played up to the pause still is (the step from the last look).
      lastMono_ = now;
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
// The plays waiting, to the card.
// ---------------------------------------------------------------------------
bool Outbox::due() {
  return tries_ == 0 || remounted_ || env_.monoMs() - triedAt_ >= kRetryMs;
}

Outbox::Tried Outbox::done(Tried t, const Played& p) {
  // Written, or dropped for good: the next play's turn.
  if (t.did == Did::Written) {
    ++counts_.written;
  } else {
    ++counts_.dropped;
    if (t.gaveUp) ++counts_.lost;
  }
  env_.tried(t, p);
  if (t.did == Did::Written) env_.written(t.slot, p);  // (before the pop: `p` is the ring's)
  tracker_.pop();
  tries_ = 0;
  taken_ = false;
  remounted_ = false;
  return t;
}

Outbox::Tried Outbox::failed(Tried t, const Played& p) {
  ++tries_;
  triedAt_ = env_.monoMs();
  remounted_ = false;
  t.tries = tries_;
  if (tries_ >= kTries) {
    t.did = Did::Dropped;
    t.gaveUp = true;
    return done(t, p);
  }
  // At the head still: nothing behind it goes to the card before it.
  t.did = Did::Kept;
  env_.tried(t, p);
  return t;
}

Outbox::Tried Outbox::writeOne() {
  Tried t;
  if (!tracker_.pending()) return t;
  const Played& p = tracker_.oldest();
  if (tries_) ++counts_.retries;
  t.tries = static_cast<uint8_t>(tries_ + 1);
  Journal::Wrote w = Journal::Wrote::Off;
  if (!taken_) {
    // No seq taken for it yet: its file as the card has it now.
    uint32_t size = 0, fatTime = 0;
    switch (env_.stat(p.file.path, &size, &fatTime)) {
      case Env::File::Gone:
        t.did = Did::Dropped;
        t.why = Why::Gone;
        return done(t, p);
      case Env::File::Failed:
        t.why = Why::Stat;
        return failed(t, p);
      case Env::File::There:
        break;
    }
    Slot s;
    s.outcome = p.outcome;
    s.source = p.file.source;
    s.pauseCount = p.pauses;
    // Its start: the clock now, less the time since it began (a clock set
    // during the play still dates it); 0 while unset.
    s.startedAt = env_.startedAt(p.startMono, &s.clock);
    if (s.startedAt == 0) s.clock = kClockUnset;
    s.uptimeMs = static_cast<uint32_t>(p.startMono);
    s.playedMs = p.playedMs;
    s.durationMs = p.durationMs;
    s.pathHash = p.file.pathHash;
    s.size = size;
    s.fatTime = fatTime;
    s.generation = p.file.generation;
    w = journal_.append(s);
    slot_ = s;
    // (Failed: the journal keeps the slot as it encoded it, for again().)
    taken_ = w == Journal::Wrote::Failed;
    t.hasSlot = w != Journal::Wrote::Off && w != Journal::Wrote::Nvs;
  } else {
    w = journal_.again();
    t.hasSlot = true;
  }
  t.slot = slot_;
  switch (w) {
    case Journal::Wrote::Ok:
      t.did = Did::Written;
      return done(t, p);
    case Journal::Wrote::Failed:
      t.why = Why::Append;
      return failed(t, p);
    case Journal::Wrote::Nvs:
      t.why = Why::Nvs;
      break;
    case Journal::Wrote::Blocked:
      t.why = Why::Blocked;
      break;
    case Journal::Wrote::Off:
      t.why = Why::Off;
      break;
  }
  t.did = Did::Dropped;
  return done(t, p);
}

Outbox::Flushed Outbox::flush(uint32_t budgetMs, uint32_t scanSlots) {
  Flushed f;
  f.waiting = tracker_.pending();
  const uint64_t until = env_.monoMs() + budgetMs;
  // The boot's scan first (no slot before it: the seq and the boot count
  // are its): what is left of it, while there is time.
  while (tracker_.pending() && !journal_.ready() && !journal_.off()) {
    if (env_.monoMs() >= until) {
      f.stop = Flushed::Stop::Time;
      break;
    }
    journal_.scan(scanSlots);
  }
  while (f.stop == Flushed::Stop::Done && tracker_.pending()) {
    if (env_.monoMs() >= until) {
      f.stop = Flushed::Stop::Time;
      break;
    }
    const Tried t = writeOne();
    if (t.did == Did::Written) {
      ++f.written;
      continue;
    }
    if (t.did == Did::Dropped) ++f.dropped;
    // The card failed it (kept, or dropped at its last try): the plays
    // behind it aren't asked.
    if (t.did == Did::Kept || t.gaveUp) f.stop = Flushed::Stop::Card;
  }
  f.left = tracker_.pending();
  return f;
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
  if (in.listMoving) return Wait::List;
  return Wait::None;
}

const char* WriteGate::waitName(Wait w) {
  switch (w) {
    case Wait::None: return "none";
    case Wait::Start: return "a start";
    case Wait::TrackEnd: return "the gapless window";
    case Wait::Ring: return "the ring";
    case Wait::Underrun: return "an underrun";
    case Wait::List: return "a list moving";
  }
  return "?";
}

}  // namespace playlog
