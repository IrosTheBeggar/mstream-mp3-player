// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsGlue.h"

#include <cstdlib>
#include <cstring>

namespace statsglue {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sx = statsfile;
namespace pl = playlog;

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Boot: return "boot";
    case Kind::Rotation: return "rotation";
    case Kind::Hs: return "Hs";
    case Kind::GiveBack: return "give-back";
  }
  return "?";
}

const char* refusedName(Refused r) {
  switch (r) {
    case Refused::None: return "none";
    case Refused::Off: return "off";
    case Refused::Ending: return "ending";
    case Refused::Budget: return "budget";
    case Refused::CardAway: return "cardAway";
    case Refused::Partial: return "partial";
    case Refused::Booting: return "booting";
    case Refused::Fenced: return "fenced";
    case Refused::GivenBack: return "givenBack";
    case Refused::Behind: return "behind";
  }
  return "?";
}

Glue::Glue(tagstore::Fs& fs, Env& env, sf::Fold::AllocFn alloc, sf::Fold::FreeFn release)
    : fs_(fs), env_(env), alloc_(alloc), free_(release), fold_(alloc, release), store_(fs) {}

uint8_t* Glue::carve() {
  return static_cast<uint8_t*>(alloc_ ? alloc_(sx::kWorkBytes) : std::malloc(sx::kWorkBytes));
}

void Glue::giveBack(uint8_t* p) {
  if (!p) return;
  if (free_) {
    free_(p);
  } else {
    std::free(p);
  }
}

sx::Opened Glue::readNow(sx::Store::WaitFn wait) {
  // (HOUR's, DAYS' and MARK's blocks first: given back with the stats off.)
  uint8_t* w = fold_.begin() ? carve() : nullptr;
  const sx::Opened o = store_.read(fold_, w, wait);  // (no work memory: NoMemory, the stats off)
  giveBack(w);
  // The stats off this session (unread, newer, no memory): nothing held
  // for them (a remount's read carves again).
  if (!sx::statsOn(o.read)) fold_.release();
  return o;
}

const sx::Opened& Glue::begin(const cc::msmf::Acks& acks, sx::Store::WaitFn wait) {
  acks_ = acks;
  first_ = opened_ = readNow(wait);
  return first_;
}

// ---------------------------------------------------------------------------
// The play log's Folds.
// ---------------------------------------------------------------------------
void Glue::scanBegan(bool again) {
  partial_ = false;
  // The boot's fold folds every slot of the log above the card's MARK:
  // nothing is behind after it. (A scan begins again only before the
  // boot's first slot: the ring holds none.)
  behind_ = false;
  ringN_ = ringOver_ = 0;
  takeTries_ = 0;
  logN_ = logMore_ = 0;
  bootPending_ = rereadPending_ = false;
  if (again) {
    if (ending_) {
      drop();
      return;
    }
    if (!dropped_) {
      // A remount: stats.bin read again (one read, no waits), the folds
      // made before it dropped now, or each slot read before it would count
      // twice. A stats.bin unread at the boot is tried again. The read (the
      // work memory, the load's chunks) and the boot's fold's blocks wait
      // for the scan's first reserve(): a pass that may carve (the write
      // gate open, Auto DJ's join not Working: 5.6), never the remount's.
      fold_.clear();
      rereadPending_ = true;
      return;  // (began(): at the read)
    }
    env_.began(true, nullptr);
    return;
  }
  // The boot's fold's blocks (its ranges, ACKS rows, indexes) wait for the
  // scan's first reserve() too: carved in the loop, not in setup() between
  // blocks held for the session (6.7).
  bootPending_ = on();
  env_.began(false, nullptr);
}

void Glue::drop() {
  // 5.4: no read while the end flushes (its 3 s are the plays'); the folds
  // made before are dropped, and none is made or saved after.
  dropped_ = true;
  bootPending_ = rereadPending_ = false;
  fold_.release();
  env_.began(true, nullptr);
}

void Glue::settle() {
  // Given back to a library update: nothing read or carved until
  // takeBack() (a remount's read waits for it too).
  if (given_) return;
  if (rereadPending_) {
    rereadPending_ = false;
    ++rereads_;
    opened_ = readNow(nullptr);
    bootPending_ = on();
    env_.began(true, &opened_);
  }
  if (bootPending_) {
    bootPending_ = false;
    fold_.beginBoot(acks_);  // (false: no memory: the fold says so, and no save follows)
  }
}

void Glue::ending(uint64_t untilMs) {
  ending_ = true;
  until_ = untilMs;
  // A remount's read not made yet: none now (5.4), the folds dropped.
  if (rereadPending_) drop();
}

void Glue::noteLog(const pl::Slot& s) {
  if (s.kind != pl::kKindPlay || s.seq == 0) return;
  for (uint32_t i = 0; i < logN_; ++i) {
    LogKey& k = log_[i];
    if (k.epoch == s.epoch && std::memcmp(k.dev, s.deviceId, 6) == 0) {
      if (s.seq > k.seq) k.seq = s.seq;
      ++k.slots;
      return;
    }
  }
  if (logN_ == kLogKeys) {
    ++logMore_;
    return;
  }
  LogKey& k = log_[logN_++];
  std::memcpy(k.dev, s.deviceId, 6);
  k.epoch = s.epoch;
  k.seq = s.seq;
  k.slots = 1;
}

// (settle(): the scan's first reserve() made the read and began the boot's
// fold; a scan the end's flush finishes, or a test's, asks no reserve().)
void Glue::slot(const pl::Slot& s, int file) {
  settle();
  noteLog(s);
  if (on()) fold_.bootSlot(s, file);
}

void Glue::partial() {
  settle();
  partial_ = true;
  env_.partialFile();
  if (on()) fold_.partial();
}

bool Glue::covered(const pl::Slot& s) {
  settle();
  return on() && fold_.covered(s);
}

uint32_t Glue::markOf(const uint8_t deviceId[6], uint64_t epoch) {
  settle();
  return on() ? fold_.markOf(deviceId, epoch) : 0;
}

bool Glue::save(Why why) {
  settle();
  const Kind k = why == Why::Boot ? Kind::Boot : Kind::Rotation;
  if (!on()) return refuse(k, Refused::Off);
  // (A rotation while given back: no fold to save; it deletes what the
  // card's MARK doesn't cover, 5.5's hole. After a give-back whose save
  // landed, the card's MARK covers every play of plays.1.jnl.)
  if (given_) return refuse(k, Refused::GivenBack);
  if (ending_ && k == Kind::Boot) return refuse(k, Refused::Ending);
  if (ending_ && env_.monoMs() + kEndSaveMs > until_) return refuse(k, Refused::Budget);
  if (env_.cardAway()) return refuse(k, Refused::CardAway);
  if (partial_) return refuse(k, Refused::Partial);
  if (behind_) return refuse(k, Refused::Behind);
  return commit(k);
}

bool Glue::hs(bool fenced) {
  if (fenced) return refuse(Kind::Hs, Refused::Fenced);
  if (!on() && !rereadPending_) return refuse(Kind::Hs, Refused::Off);
  if (given_) return refuse(Kind::Hs, Refused::GivenBack);
  if (booting()) return refuse(Kind::Hs, Refused::Booting);
  if (partial_) return refuse(Kind::Hs, Refused::Partial);
  if (behind_) return refuse(Kind::Hs, Refused::Behind);
  if (env_.cardAway()) return refuse(Kind::Hs, Refused::CardAway);
  return commit(Kind::Hs);
}

bool Glue::refuse(Kind k, Refused r) {
  // The boot's fold ends here whatever (the store's save would have ended
  // it); Hs never ends one (it is refused while one runs).
  if (k != Kind::Hs) fold_.endBoot();
  ++asked_;
  last_ = sx::SaveReport();
  lastKind_ = k;
  lastRefused_ = r;
  env_.saved(k, r, last_);
  return false;
}

bool Glue::commit(Kind k) {
  env_.asking(k, &ask_);
  ask_.mayCompact = k == Kind::Boot;  // (a rotation's and Hs never compact: 5.3)
  uint8_t* w = carve();
  // A tmp the store holds, promoted first, in a call of its own: its
  // frames (replace()'s, a twin's rename) never under the save's (the
  // loop's stack: docs/LISTENING-STATS.md 11).
  // (No work memory: NoMemory, or Nothing with nothing new.)
  last_ = store_.save(fold_, ask_, w, store_.promote(fold_));
  giveBack(w);
  ++asked_;
  if (last_.what == sx::Saved::Wrote) ++written_;
  lastKind_ = k;
  lastRefused_ = Refused::None;
  env_.saved(k, Refused::None, last_);
  return sx::savedOk(last_.what);
}

// ---------------------------------------------------------------------------
// The rest of the session.
// ---------------------------------------------------------------------------
bool Glue::reserve(uint32_t slots) {
  if (given_) return false;  // (the scan waits for takeBack(): app/ListeningStats::mayScan())
  settle();
  return !(on() && fold_.booting()) || fold_.reserve(slots);
}

void Glue::ready() {
  settle();
  if (fold_.booting()) fold_.endBoot();
}

void Glue::written(const pl::Slot& s) {
  if (given_) {
    // No fold while given back: the ring, folded after the re-read; one
    // past it is the next boot's (and no save this session: takeBack()).
    // (Given back with the stats on, or a remount's read to make, when no
    // slot is written; and still so between the take-back's reads the
    // card failed, the stats off then.)
    if (!sf::counts(s)) return;
    if (ringN_ < kRing) {
      ring_[ringN_++] = s;
    } else {
      ++ringOver_;
    }
    return;
  }
  if (!on()) return;
  fold_.liveSlot(s);
}

bool Glue::keepSpare() {
  if (!on() || given_ || booting() || fold_.noMemory() || fold_.spare()) return true;
  return fold_.keepSpare();
}

// ---------------------------------------------------------------------------
// A library update's give-back (6.7).
// ---------------------------------------------------------------------------
// (Before the boot's fold begins, bootPending_, the fold holds what the
// boot's read loaded, nothing folded: given back, the scan waits for
// takeBack() and folds after it, its blocks carved then. A remount's read
// still to make, rereadPending_: the folds dropped, the stats off until
// it; what the fold still holds is given back, and the read waits for
// takeBack() too. The boot's fold under way: its blocks are the scan's.)
size_t Glue::resident() const {
  return !given_ && !ending_ && !fold_.booting() && (on() || rereadPending_) ? fold_.givesBack() : 0;
}

bool Glue::updateWaits(bool gateOpen, uint32_t slotsQueued) const {
  if (!resident()) return false;
  // A play written now is folded live (the stats on, the boot's fold done):
  // the give-back's save holds it. (Before the boot's fold, or with a
  // remount's read to make, the journal isn't ready: none is queued.)
  if (slotsQueued && on() && !booting()) return true;
  return !gateOpen && fold_.dirty();
}

Turn turnOf(uint32_t slotsDue, bool hsAsked) {
  if (slotsDue) return Turn::Slot;
  return hsAsked ? Turn::Hs : Turn::Nothing;
}

size_t Glue::giveBack() {
  // (gaveBack(): the last that gave back.)
  if (given_ || ending_ || fold_.booting() || (!on() && !rereadPending_)) return 0;
  gave_ = GaveBack();
  // A fold holding plays the card's stats.bin doesn't: saved first, the
  // rotation's path (never a compaction), with a save's guards. Whatever it
  // comes to, the blocks go. A save that doesn't land leaves RAM behind its
  // log after the re-read (Refused::Behind): the next boot folds those
  // plays only while the log still holds them, and a rotation later this
  // session deletes those in plays.1.jnl (5.5's hole), where a save held
  // through would have landed.
  if (fold_.dirty()) {
    gave_.saveAsked = true;
    if (env_.cardAway()) {
      refuse(Kind::GiveBack, Refused::CardAway);
    } else if (partial_) {
      refuse(Kind::GiveBack, Refused::Partial);
    } else if (behind_) {
      refuse(Kind::GiveBack, Refused::Behind);
    } else {
      commit(Kind::GiveBack);
    }
    gave_.refused = lastRefused_;
  }
  // The copy on the card that holds every slot RAM holds now, the re-read's
  // judge: the card's copy (nothing new, or the save landed), or this
  // save's tmp, held (its replace() failed: the read promotes it first).
  // None: the save didn't land, or the fold ran out of memory (plays it
  // never folded are in no copy).
  holds_ = false;
  if (!fold_.noMemory()) {
    if (!fold_.dirty()) {
      holding_ = store_.cardCopy();
      holds_ = true;
    } else if (gave_.saveAsked && lastRefused_ == Refused::None && last_.what == sx::Saved::Held) {
      holding_ = store_.heldCopy();
      holds_ = true;
    }
  }
  gave_.covers = holds_;
  gave_.markRows = fold_.rows(sf::kMark);
  gave_.bytes = fold_.giveBack();
  gave_.kept = fold_.held();
  given_ = true;
  ringN_ = ringOver_ = 0;
  ++giveBacks_;
  return gave_.bytes;
}

void Glue::foldRing() {
  for (uint32_t i = 0; i < ringN_; ++i) {
    const pl::Slot& s = ring_[i];
    // (Another player folded it, the card in it meanwhile: its stats.bin
    // holds it.)
    if (fold_.covered(s)) {
      ++took_.covered;
      continue;
    }
    fold_.liveSlot(s);
    ++took_.folded;
  }
  ringN_ = ringOver_ = 0;
}

bool Glue::takeBackDue() const { return given_ && !ending_ && (takeTries_ == 0 || env_.monoMs() >= takeAtMs_); }

const TookBack& Glue::takeBack() {
  // (No read while the end flushes: 5.4.)
  if (!given_ || ending_) return took_;
  took_ = TookBack();
  given_ = false;
  if (rereadPending_) {
    // A remount's scan begun again meanwhile (Ready, before the boot's
    // first slot: the ring holds none): its first reserve() reads
    // stats.bin, and its boot's fold folds the log again.
    took_.rescan = true;
    ringN_ = ringOver_ = 0;
    takeTries_ = 0;
    return took_;
  }
  ++rereads_;
  opened_ = readNow(nullptr);
  ++takeTries_;
  took_.read = true;
  took_.tries = takeTries_;
  took_.on = sx::statsOn(opened_.read);
  took_.over = ringOver_;
  if (!took_.on) {
    if (sx::cardFault(opened_) && takeTries_ < kTakeBackReads) {
      // A read the card failed: the boot's would be made again after
      // kBootWaitsMs (5.4), so this one is too, at a later pass, never a
      // wait in the loop. Still given back meanwhile: the ring kept, no
      // save, no fold (readNow() gave every block back; the next read
      // carves them again).
      given_ = true;
      took_.again = true;
      takeAtMs_ = env_.monoMs() + tagstore::kBootWaitsMs[takeTries_ - 1];
      return took_;
    }
    // Off for the session, as a remount's read leaves them (5.4): the
    // ring's plays are the next boot's, and a rotation later this session
    // deletes those in plays.1.jnl with no save (5.5's hole).
    took_.dropped = ringN_;
    ringN_ = ringOver_ = 0;
    takeTries_ = 0;
    bootPending_ = false;  // (no boot's fold with the stats off)
    return took_;
  }
  takeTries_ = 0;
  const bool present = opened_.read == sx::Read::Loaded;
  took_.same = holds_ && present == holding_.present &&
               (!present || (opened_.generation == holding_.generation && opened_.headerCrc == holding_.headerCrc));
  foldRing();
  // RAM lacks plays the log holds above the card's MARK (a save that
  // didn't land, a fold out of memory, another stats.bin, a play past the
  // ring): a later save would raise MARK past them unfolded. None this
  // session: the next boot folds those the log still holds, and a
  // rotation later this session deletes those in plays.1.jnl (its save
  // refused: 5.5's hole). Not when the boot's fold is still to begin
  // (given back before it): it folds every slot of the log above the
  // MARK this read found, and no play was written before it (the ring
  // holds none).
  if (!bootPending_ && (!took_.same || took_.over)) behind_ = true;
  took_.behind = behind_;
  return took_;
}

TrackLook Glue::track(uint64_t hash) {
  TrackLook t;
  t.delta = fold_.track(hash, &t.deltaRow);
  const sf::Head& h = fold_.head();
  if (h.baseGen != 0) {
    t.base = TrackLook::Base::Unread;
    char name[sx::kNameCap];
    bool cardFailed = false;
    tagstore::File* f = sx::baseName(h.baseGen, name, sizeof(name)) ? fs_.openRead(name, &cardFailed) : nullptr;
    if (f) {
      uint8_t b[sx::kHeaderBytes];
      const uint64_t want = sx::kBaseRowsAt + static_cast<uint64_t>(h.baseRows) * sf::kTrackBytes;
      // (The file stats.bin names, as far as its frame's magic, generation
      // and size say: the console's look, not the boot's check.)
      if (f->size() == want && f->read(0, b, sizeof(b)) && cc::get32(b) == sx::kMagic &&
          cc::get32(b + 20) == h.baseGen) {
        uint32_t lo = 0, hi = h.baseRows;
        bool failed = false;
        t.base = TrackLook::Base::NotThere;
        while (lo < hi) {
          const uint32_t mid = lo + (hi - lo) / 2;
          uint8_t row[sf::kTrackBytes];
          ++t.reads;
          if (!f->read(sx::kBaseRowsAt + mid * sf::kTrackBytes, row, sizeof(row))) {
            failed = true;
            break;
          }
          const sf::Track r = sf::decodeTrack(row);
          if (r.hash == hash) {
            t.baseRow = r;
            t.base = TrackLook::Base::Found;
            break;
          }
          if (r.hash < hash) {
            lo = mid + 1;
          } else {
            hi = mid;
          }
        }
        if (failed) t.base = TrackLook::Base::Unread;
      }
      fs_.close(f);
    }
  }
  const bool inBase = t.base == TrackLook::Base::Found;
  if (inBase && t.delta) {
    t.row = sf::merge(t.baseRow, t.deltaRow);
  } else if (inBase) {
    t.row = t.baseRow;
  } else if (t.delta) {
    t.row = t.deltaRow;
  } else {
    t.row = sf::Track();
    t.row.hash = 0;
  }
  return t;
}

Glue::FileLook Glue::lookFile() {
  FileLook l;
  bool cardFailed = false;
  tagstore::File* f = fs_.openRead(sx::kStatsBin, &cardFailed);
  if (!f) {
    l.unread = cardFailed;
    return l;
  }
  l.there = true;
  l.bytes = f->size();
  uint8_t b[sx::kHeaderBytes];
  if (f->read(0, b, sizeof(b))) {
    l.magic = cc::get32(b);
    l.major = cc::get16(b + 4);
    l.minor = cc::get16(b + 6);
    l.generation = cc::get32(b + 20);
  } else {
    l.unread = true;
  }
  fs_.close(f);
  return l;
}

}  // namespace statsglue
