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
  uint8_t* w = carve();
  const sx::Opened o = store_.read(fold_, w, wait);  // (no work memory: NoMemory, the stats off)
  giveBack(w);
  return o;
}

const sx::Opened& Glue::begin(const cc::msmf::Acks& acks, sx::Store::WaitFn wait) {
  acks_ = acks;
  foldReady_ = fold_.begin();
  if (!foldReady_) {
    first_ = sx::Opened();
    first_.read = sx::Read::NoMemory;
    opened_ = first_;
    return first_;
  }
  first_ = opened_ = readNow(wait);
  return first_;
}

// ---------------------------------------------------------------------------
// The play log's Folds.
// ---------------------------------------------------------------------------
void Glue::scanBegan(bool again) {
  partial_ = false;
  logN_ = logMore_ = 0;
  if (again) {
    if (ending_) {
      // 5.4: no read while the end flushes (its 3 s are the plays'); the
      // folds made before are dropped, and none is made or saved after.
      dropped_ = true;
      fold_.clear();
      env_.began(true, nullptr);
      return;
    }
    // A remount: stats.bin read again (one read, no waits), the folds made
    // before it dropped, or each slot read before it would count twice. A
    // stats.bin unread at the boot is tried again here.
    if (foldReady_ && !dropped_) {
      ++rereads_;
      opened_ = readNow(nullptr);
    }
  }
  if (on()) fold_.beginBoot(acks_);  // (false: no memory: the fold says so, and no save follows)
  env_.began(again, again && foldReady_ && !dropped_ ? &opened_ : nullptr);
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

void Glue::slot(const pl::Slot& s, int file) {
  noteLog(s);
  if (on()) fold_.bootSlot(s, file);
}

void Glue::partial() {
  partial_ = true;
  env_.partialFile();
  if (on()) fold_.partial();
}

bool Glue::covered(const pl::Slot& s) { return on() && fold_.covered(s); }

uint32_t Glue::markOf(const uint8_t deviceId[6], uint64_t epoch) { return on() ? fold_.markOf(deviceId, epoch) : 0; }

bool Glue::save(Why why) {
  const Kind k = why == Why::Boot ? Kind::Boot : Kind::Rotation;
  if (!on()) return refuse(k, Refused::Off);
  if (ending_ && k == Kind::Boot) return refuse(k, Refused::Ending);
  if (ending_ && env_.monoMs() + kEndSaveMs > until_) return refuse(k, Refused::Budget);
  if (env_.cardAway()) return refuse(k, Refused::CardAway);
  if (partial_) return refuse(k, Refused::Partial);
  return commit(k);
}

bool Glue::hs(bool fenced) {
  if (fenced) return refuse(Kind::Hs, Refused::Fenced);
  if (!on()) return refuse(Kind::Hs, Refused::Off);
  if (fold_.booting()) return refuse(Kind::Hs, Refused::Booting);
  if (partial_) return refuse(Kind::Hs, Refused::Partial);
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
  last_ = store_.save(fold_, ask_, w);  // (no work memory: NoMemory, or Nothing with nothing new)
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
bool Glue::reserve(uint32_t slots) { return !(on() && fold_.booting()) || fold_.reserve(slots); }

void Glue::ready() {
  if (fold_.booting()) fold_.endBoot();
}

void Glue::written(const pl::Slot& s) {
  if (on()) fold_.liveSlot(s);
}

bool Glue::keepSpare() {
  if (!on() || fold_.booting() || fold_.noMemory() || fold_.spare()) return true;
  return fold_.keepSpare();
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
