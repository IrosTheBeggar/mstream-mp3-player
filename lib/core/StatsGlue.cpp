// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "StatsGlue.h"

#include <cstdlib>
#include <cstring>
#include <new>

namespace statsglue {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sx = statsfile;
namespace pl = playlog;

namespace {

void* mallocAlloc(size_t n) { return std::malloc(n); }
void mallocFree(void* p) { std::free(p); }

Memory normalized(const Memory& m) {
  Memory out = m;
  if (!out.alloc) out.alloc = mallocAlloc;
  if (!out.top) out.top = out.alloc;
  if (!out.release) out.release = mallocFree;
  return out;
}

// The ranges' block holds ACKS's rows (a u64 each): carved 4 B over and
// aligned to 8 here (PSRAM's heap gives 4).
constexpr size_t kRangesAlign = alignof(sf::Fold::AckTrack);

const cc::msmf::Acks kNoAcks{};

}  // namespace

const char* kindName(Kind k) {
  switch (k) {
    case Kind::Boot: return "boot";
    case Kind::Rotation: return "rotation";
    case Kind::Hs: return "Hs";
    case Kind::Records: return "records";
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
    case Refused::Behind: return "behind";
  }
  return "?";
}

Turn turnOf(bool saveDue, uint32_t slotsDue, bool hsAsked) {
  if (saveDue) return Turn::Save;
  if (slotsDue) return Turn::Slot;
  return hsAsked ? Turn::Hs : Turn::Nothing;
}

Glue::Glue(tagstore::Fs& fs, Env& env, const Memory& memory)
    : fs_(fs),
      env_(env),
      mem_(normalized(memory)),
      fold_(mem_.top, mem_.release),
      store_(fs, mem_.alloc, mem_.release),
      acks_(&kNoAcks) {}

Glue::~Glue() {
  if (ranges_) release(ranges_);
}

uint8_t* Glue::carve() { return static_cast<uint8_t*>(mem_.alloc(sx::kWorkBytes)); }

void Glue::release(void* p) {
  if (p) mem_.release(p);
}

sx::Opened Glue::readNow(sx::Store::WaitFn wait) {
  // (The work memory for this call alone; MARK's block, when the card's
  // MARK needs one, the store's: carved by the boot's read, kept for the
  // session.)
  uint8_t* w = carve();
  const sx::Opened o = store_.read(fold_, w, wait);  // (no work memory: NoMemory, the stats off)
  release(w);
  return o;
}

const sx::Opened& Glue::begin(const cc::msmf::Acks& acks, sx::Store::WaitFn wait) {
  acks_ = &acks;
  first_ = opened_ = readNow(wait);
  return first_;
}

// ---------------------------------------------------------------------------
// The play log's Folds.
// ---------------------------------------------------------------------------
void Glue::scanBegan(bool again) {
  partial_ = false;
  logN_ = logMore_ = 0;
  bootPending_ = rereadPending_ = retrying_ = false;
  tries_ = 0;
  recordsTried_ = 0;
  rangesBytes_ = 0;
  // (A remount mid-scan: the fold begun before it gives its ranges' block
  // back now.)
  endBoot();
  if (again) {
    if (ending_) {
      drop();
      return;
    }
    if (!dropped_) {
      // A remount: stats.bin read again (one read, no waits), the folds
      // made before it dropped now (the records and an overflow's chunks),
      // or each slot read before it would count twice. A stats.bin unread at
      // the boot is tried again. The read and the boot's fold's ranges' block
      // wait for the scan's first reserve(): a pass that may carve (the write
      // gate open, Auto DJ's join not Working, no library update asked: 5.6,
      // 5.14), never the remount's.
      fold_.clear();
      rereadPending_ = true;
      return;  // (began(): at the read)
    }
    env_.began(true, nullptr);
    return;
  }
  // The boot's fold's ranges' block waits for the scan's first reserve()
  // too: carved in the loop, not in setup() (5.14).
  bootPending_ = on();
  env_.began(false, nullptr);
}

void Glue::drop() {
  // 5.4: no read while the end flushes (its 3 s are the plays'); the folds
  // made before are dropped, and none is made or saved after.
  dropped_ = true;
  bootPending_ = rereadPending_ = retrying_ = false;
  endBoot();
  fold_.clear();
  env_.began(true, nullptr);
}

void Glue::settle() {
  if (rereadPending_) {
    rereadPending_ = false;
    ++rereads_;
    opened_ = readNow(nullptr);
    bootPending_ = on();
    env_.began(true, &opened_);
  }
  if (bootPending_) {
    bootPending_ = false;
    beginBoot();
  }
}

void Glue::beginBoot() {
  // The ranges (80 B a MARK row) and ACKS's rows above MARK (112 B each):
  // 3,072 B with 16 of each, at the top (5.14). None: the fold out of
  // memory (it says so), and nothing is saved.
  const size_t need = fold_.bootBytes(*acks_);
  void* m = mem_.top(need + kRangesAlign - 4);
  uint8_t* at = static_cast<uint8_t*>(m);
  if (at && reinterpret_cast<uintptr_t>(at) % kRangesAlign) at += kRangesAlign - reinterpret_cast<uintptr_t>(at) % kRangesAlign;
  if (!fold_.beginBoot(*acks_, at, m ? need : 0)) {
    release(m);
    return;
  }
  ranges_ = m;
  rangesBytes_ = need + kRangesAlign - 4;
}

void Glue::endBoot() {
  fold_.endBoot();  // (the ranges' block no longer used)
  if (ranges_) release(ranges_);
  ranges_ = nullptr;
}

void Glue::ending(uint64_t untilMs) {
  ending_ = true;
  until_ = untilMs;
  // A remount's read not made yet: none now (5.4), the folds dropped.
  if (rereadPending_) drop();
  // An overflow waiting for its save's next try: none in the end's flush
  // (its 3 s are the plays'). Behind: the next boot folds the log above the
  // card's MARK.
  if (retrying_) {
    retrying_ = false;
    fold_.fallBehind();
  }
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

// (Each save: refusal(), then commit() or refuse(), from the caller's own
// frame: one frame fewer under the store's save, the loop's deepest path.)
bool Glue::save(Why why) {
  settle();
  const Kind k = why == Why::Rotation ? Kind::Rotation : Kind::Boot;
  if (k == Kind::Boot) {
    // The boot's: its first try (afterBootTry(): an overflow's next tries).
    retrying_ = false;
    tries_ = 1;
  }
  const Refused r = refusal(k);
  const bool ok = r == Refused::None ? commit(k) : refuse(k, r);
  if (k == Kind::Boot) afterBootTry();
  return ok;
}

// The session's own rules (Hs has its own order): the refusal, or None.
// (A call that returns before the save's: not a frame under it, the loop's
// deepest path, docs/LISTENING-STATS.md 11.)
Refused Glue::refusal(Kind k) {
  if (!on()) return Refused::Off;
  if (ending_ && (k == Kind::Boot || k == Kind::Records)) return Refused::Ending;
  if (ending_ && env_.monoMs() + kEndSaveMs > until_) return Refused::Budget;
  if (env_.cardAway()) return Refused::CardAway;
  if (partial_) return Refused::Partial;
  if (fold_.behind()) return Refused::Behind;
  return Refused::None;
}

bool Glue::refuse(Kind k, Refused r) {
  // The boot's fold ends here whatever (the store's save would have ended
  // it); Hs never ends one (it is refused while one runs).
  if (k != Kind::Hs) endBoot();
  if (k == Kind::Records) recordsTried_ = fold_.records();  // (again after a later live record)
  ++asked_;
  last_ = sx::SaveReport();
  lastKind_ = k;
  lastRefused_ = r;
  env_.saved(k, r, last_);
  return false;
}

bool Glue::commit(Kind k) {
  // The boot's fold ended and its ranges' block given back first: the
  // save's work memory comes after it (5.14).
  endBoot();
  env_.asking(k, &ask_);
  ask_.mayCompact = k == Kind::Boot;  // (a rotation's, the records' and Hs never compact: 5.3)
  // A tmp the store holds, promoted first, in a call of its own: its
  // frames (replace()'s, a twin's rename) never under the save's (the
  // loop's stack: docs/LISTENING-STATS.md 11).
  const sx::Store::Held held = store_.promote(fold_);
  uint8_t* w = carve();
  // (No work memory: NoMemory, or Nothing with nothing new. The report made
  // straight in last_: no copy of it on this frame.)
  last_.~SaveReport();
  new (&last_) sx::SaveReport(store_.save(fold_, ask_, w, held));
  release(w);
  ++asked_;
  if (last_.what == sx::Saved::Wrote) ++written_;
  lastKind_ = k;
  // The store's Behind and Damaged are the session's Behind (5.14): no save
  // until the next boot; the report says which (a damaged section's type).
  lastRefused_ = last_.what == sx::Saved::Behind || last_.what == sx::Saved::Damaged ? Refused::Behind : Refused::None;
  // (A save of any kind that took the records empties them: the records'
  // save is due again from kSaveAt new ones.)
  if (sx::savedOk(last_.what) || last_.what == sx::Saved::Held) {
    recordsTried_ = 0;
  } else if (k == Kind::Records) {
    recordsTried_ = fold_.records();
  }
  env_.saved(k, lastRefused_, last_);
  return sx::savedOk(last_.what);
}

// After a try of the boot's save: an overflow (records past kRecords, in
// the chunks) is given back by a save that lands or is held (the Fold's
// saved()/held()). Else a try that didn't land (a step the card failed, a
// read it failed, no work memory, the card away) is made again at a later
// pass, kBootWaitsMs after this one, kBootTries in all, a track's start
// and an asked library update waiting meanwhile; a refusal (the end's
// flush, a changed stats.bin, the stats off, behind) or the last try's
// failure leaves the session behind (statsref's boot overflow: the next
// boot folds again; 5.14's rotation-loss hole for a full log).
void Glue::afterBootTry() {
  retrying_ = false;
  if (fold_.records() <= sf::kRecords || fold_.behind()) return;
  const bool again = lastRefused_ == Refused::CardAway ||
                     (lastRefused_ == Refused::None &&
                      (last_.what == sx::Saved::Failed || last_.what == sx::Saved::NoMemory));
  if (again && tries_ < kBootTries) {
    retrying_ = true;
    retryAt_ = env_.monoMs() + tagstore::kBootWaitsMs[tries_ - 1];
    return;
  }
  fold_.fallBehind();
}

bool Glue::retryDue() {
  // (Given back meanwhile: a rotation's save that landed holds them; or the
  // records full, behind.)
  if (retrying_ && (fold_.records() <= sf::kRecords || fold_.behind())) retrying_ = false;
  return retrying_ && !ending_ && env_.monoMs() >= retryAt_;
}

bool Glue::retry() {
  if (!retryDue()) return false;
  ++tries_;
  const Refused r = refusal(Kind::Boot);
  const bool ok = r == Refused::None ? commit(Kind::Boot) : refuse(Kind::Boot, r);
  afterBootTry();
  return ok;
}

bool Glue::recordsDue(bool fenced) {
  if (!on() || booting() || !fold_.due()) return false;
  // (One that didn't land: again after a later live record.)
  if (recordsTried_ && fold_.records() <= recordsTried_) return false;
  if (fenced && fold_.records() < sf::kRecords) return false;
  return !env_.cardAway();
}

bool Glue::recordsSave() {
  const Refused r = refusal(Kind::Records);
  return r == Refused::None ? commit(Kind::Records) : refuse(Kind::Records, r);
}

bool Glue::saveNow(bool fenced) {
  if (retryDue()) return retry();
  return recordsDue(fenced) && recordsSave();
}

bool Glue::hs(bool fenced) {
  if (fenced) return refuse(Kind::Hs, Refused::Fenced);
  if (!on() && !rereadPending_) return refuse(Kind::Hs, Refused::Off);
  if (booting()) return refuse(Kind::Hs, Refused::Booting);
  if (partial_) return refuse(Kind::Hs, Refused::Partial);
  if (fold_.behind()) return refuse(Kind::Hs, Refused::Behind);
  if (env_.cardAway()) return refuse(Kind::Hs, Refused::CardAway);
  return commit(Kind::Hs);
}

// ---------------------------------------------------------------------------
// The rest of the session.
// ---------------------------------------------------------------------------
ScanWait Glue::scanWaits(bool updateAsked, bool joinWorking) const {
  if (!wantsRoom()) return ScanWait::None;
  if (updateAsked && (rereadPending_ || bootPending_)) return ScanWait::Update;
  return joinWorking ? ScanWait::Join : ScanWait::None;
}

bool Glue::reserve(uint32_t slots) {
  settle();
  return !(on() && fold_.booting()) || fold_.reserve(slots);
}

void Glue::ready() {
  settle();
  endBoot();
}

void Glue::written(const pl::Slot& s) {
  // (A live slot while a boot's overflow waits for its save: the last
  // chunk's room. The 97th with no room: behind.)
  if (on()) fold_.liveSlot(s);
}

// ---------------------------------------------------------------------------
// The console's looks.
// ---------------------------------------------------------------------------
// One section of stats.bin merged with the records' rows (as a save's
// writing pass would merge them, before its cuts), each row to `fn`. False:
// stats.bin couldn't be read (the card failed, no work memory, a check).
bool Glue::lookRows(sf::Sec sec, RowFn fn, void* ctx) {
  uint8_t* work = carve();
  if (!work) return false;
  sx::Scratch* sc = new (work) sx::Scratch;
  uint8_t* r1 = work + sx::kScratchBytes + sx::kBufBytes;
  bool cardFailed = false;
  tagstore::File* f = fs_.openRead(sx::kStatsBin, &cardFailed);
  bool ok = f || (!cardFailed && fs_.look(sx::kStatsBin) == tagstore::Fs::Seen::Gone);
  tagstore::ReadWatch src(f);
  if (f && sc->frame.open(src, sx::kStatsSpec, nullptr) != cc::Why::Ok) ok = false;
  if (ok) {
    sc->card.begin(f ? &src : nullptr, f ? sc->frame.section(sec) : cc::Section(), sec, r1, sx::kReadBytes);
    sf::Fold::Cursor* cur = new (sc->cursor) sf::Fold::Cursor(fold_, sec);
    uint8_t* c = sc->row[0];
    uint8_t* r = sc->row[1];
    uint8_t* out = sc->row[2];
    bool hc = sc->card.next(c), hr = cur->next(r);
    while (hc || hr) {
      const int k = !hc ? 1 : !hr ? -1 : sf::compareRows(sec, c, r);
      std::memcpy(out, k <= 0 ? c : r, sf::kStrides[sec]);
      if (k == 0) sf::addRow(sec, out, r);
      fn(ctx, out);
      if (k <= 0) hc = sc->card.next(c);
      if (k >= 0) hr = cur->next(r);
    }
    ok = sc->card.end() == cc::Why::Ok;
  }
  if (f) fs_.close(f);
  release(work);
  return ok;
}

TrackLook Glue::track(uint64_t hash) {
  TrackLook t;
  struct Find {
    uint64_t hash;
    TrackLook* t;
  } find{hash, &t};
  t.unread = !lookRows(
      sf::kTrkd,
      [](void* ctx, const uint8_t* row) {
        Find* f = static_cast<Find*>(ctx);
        if (cc::get64(row) != f->hash) return;
        f->t->deltaRow = sf::decodeTrack(row);
        f->t->delta = true;
      },
      &find);
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
    // Its directory: each section's rows (a work memory for the frame).
    uint8_t* work = carve();
    if (work) {
      sx::Scratch* sc = new (work) sx::Scratch;
      tagstore::ReadWatch src(f);
      if (sc->frame.open(src, sx::kStatsSpec, nullptr) == cc::Why::Ok) {
        l.frame = true;
        for (uint32_t i = 0; i < sf::kSecs; ++i) l.rows[i] = sc->frame.section(i).count;
      }
      release(work);
    }
  } else {
    l.unread = true;
  }
  fs_.close(f);
  return l;
}

DaysLook Glue::lookDays(uint16_t today) {
  DaysLook d;
  const uint16_t from = today >= 6 ? static_cast<uint16_t>(today - 6) : 0;
  for (uint32_t i = 0; i < 7; ++i) d.d[i].day = static_cast<uint16_t>(today >= 6 - i ? today - (6 - i) : 0);
  struct At {
    DaysLook* d;
    uint16_t from, today;
  } at{&d, from, today};
  const bool days = lookRows(
      sf::kDays,
      [](void* ctx, const uint8_t* row) {
        At* a = static_cast<At*>(ctx);
        const uint16_t day = cc::get16(row);
        if (day < a->from || day > a->today) return;
        DaysLook::Day& e = a->d->d[6 - (a->today - day)];
        e.any = true;
        std::memcpy(e.row, row, sizeof(e.row));
      },
      &at);
  const bool hours = lookRows(
      sf::kHour,
      [](void* ctx, const uint8_t* row) {
        At* a = static_cast<At*>(ctx);
        const uint32_t w = cc::get32(row);
        const uint32_t day = (w & sf::kMaxRound) / 24;
        if (day < a->from || day > a->today) return;
        DaysLook::Day& e = a->d->d[6 - (a->today - day)];
        e.hourPlays += w >> 24;
        e.hourMs += cc::get32(row + 4);
        ++e.hours;
      },
      &at);
  d.read = days && hours;
  return d;
}

}  // namespace statsglue
