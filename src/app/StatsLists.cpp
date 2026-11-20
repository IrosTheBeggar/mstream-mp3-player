// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/StatsLists.h"

#include <esp_timer.h>

#include <cstring>
#include <new>

#include "CardContract.h"
#include "PathHashWalk.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "WallClock.h"
#include "app/BoardClock.h"

namespace {

namespace sf = statsfold;
namespace sx = statsfile;
namespace sr = statsrank;
namespace sm = statsmap;
namespace sg = statsglue;

using ul = unsigned long;
using ull = unsigned long long;

// The walk's slices a pass, as Auto DJ's join's (kJoinPassUs: 6.2's "up to
// 8 ms a pass"), and the times an index that changed under it begins it again.
constexpr int64_t kWalkPassUs = 8000;
constexpr uint8_t kRestarts = 3;
constexpr size_t kAlign = 16;

size_t up(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }

struct PassClock {
  int64_t endUs;
};
bool passTimeLeft(void* ctx) { return esp_timer_get_time() < static_cast<PassClock*>(ctx)->endUs; }

const char* shelfName(uint8_t shelf) {
  switch (shelf) {
    case 0: return "Most played";
    case 1: return "New finds";
    case 2: return "Old favourites";
    case 3: return "Not played yet";
    default: return "Recently played";
  }
}
const char* windowName(uint8_t w) { return w == 0 ? "7 days" : w == 1 ? "30 days" : "All time"; }

// "Hl0 2 (Most played, All time)", "Hl1 (New finds)".
void listLabel(uint8_t shelf, uint8_t window, char* out, size_t cap) {
  if (shelf == 0) {
    snprintf(out, cap, "Hl0%u (%s, %s)", static_cast<unsigned>(window), shelfName(0), windowName(window));
  } else {
    snprintf(out, cap, "Hl%u (%s)", static_cast<unsigned>(shelf), shelfName(shelf));
  }
}

sr::List listOf(uint8_t shelf, uint8_t window) {
  switch (shelf) {
    case 0: return window == 0 ? sr::kMost7 : window == 1 ? sr::kMost30 : sr::kMostAll;
    case 1: return sr::kNewFinds;
    case 2: return sr::kOldFavourites;
    default: return sr::kRecent;
  }
}

const char* closeText(autodj::Close c) {
  switch (c) {
    case autodj::Close::Closed: return "the free block above it closed by caps";
    case autodj::Close::Nothing: return "nothing above it to close";
    case autodj::Close::NoSpacers: return "no spacers placed: nothing closed";
    case autodj::Close::NotOnTop: return "in a smaller free block: nothing closed";
    case autodj::Close::Margin: return "the free block above it LEFT OPEN (the 64 KB margin)";
    case autodj::Close::Missed: return "the free block above it LEFT OPEN (a cap landed elsewhere)";
    case autodj::Close::NoCap: return "the free block above it LEFT OPEN (no PSRAM for a cap)";
    case autodj::Close::Lost: return "LEFT OPEN (the walk didn't meet it)";
    case autodj::Close::Left: return "the free block above it partly LEFT OPEN";
    case autodj::Close::Room: return "the caps given back (a decoder's room)";
    case autodj::Close::Plain: return "carved under spacers, nothing closed";
    default: return "?";
  }
}

// A row's time (seconds since 2026-01-01, plus 1) as "2026-11-08 21:40", as
// Ht prints it; a UTC day as "2026-11-08".
void timeText(uint32_t t, char* out, size_t cap) {
  if (t == 0 || t == sf::kNoFirst) {
    snprintf(out, cap, "%s", t == 0 ? "undated" : "none");
    return;
  }
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(t - 1) * 1000);
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.hour, c.minute);
}
void dayText(uint32_t day, char* out, size_t cap) {
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
  snprintf(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}

const char* srcName(uint8_t s) { return s == sf::kAutoDj ? "Auto DJ" : s == sf::kShuffle ? "shuffle" : "you"; }

}  // namespace

// The block's head: the ask's state, then its blocks.
struct StatsLists::Ask {
  // The worker's (published by state_: written before kRankQueued /
  // kTrackQueued, read back after kDone).
  sr::Input in;
  sr::Memory mem;
  sr::Files files;
  sr::Played played;
  sr::Ranker ranker;
  sr::Report report;
  sr::TrackRows track;
  uint64_t hash = 0;
  int64_t queuedUs = 0, startUs = 0, endUs = 0;
  uint8_t priority = 0;
  // The loop's.
  int64_t copyUs = 0;      // the records' copy (Fold::Cursor), on the loop
  uint32_t trkdRecords = 0, rcntRecords = 0;
  statsfold::Track recordRow;  // Hp: the records' row
  bool recordFound = false;
  sm::Map map;
  sm::Wanted wanted;
  uint8_t* bits = nullptr;
  void* folders = nullptr;
  uint32_t foldersCap = 0, albumsCap = 0;  // what the folders' states and the bits hold
  uint32_t passes = 0;
  int64_t walkT0 = 0, walkUs = 0, walkMaxUs = 0;
  size_t parts[8] = {};  // the regions' bytes (Hr's line)
};

void StatsLists::begin(sg::Glue* glue, tagstore::Fs* fs, const Hold& hold, PrintTrackFn print) {
  glue_ = glue;
  fs_ = fs;
  hold_ = hold;
  print_ = print;
}

const char* StatsLists::name() const {
  switch (kind_) {
    case kRank: return "Hr";
    case kList: return "Hl";
    case kWalkAsk: return "Hw";
    case kTrack: return "Hp";
    default: return "H";
  }
}

void StatsLists::command(const char* arg) {
  const uint8_t s = state_.load();
  if (s != kIdle && s != kAsked) {
    Serial.printf("[listen] %s under way: one ask at a time\n", name());
    return;
  }
  Kind k = kNone;
  switch (arg[0]) {
    case 'r': k = kRank; break;
    case 'w': k = kWalkAsk; break;
    case 'p': k = kTrack; break;
    case 'l': {
      const char c = arg[1];
      if (c < '0' || c > '4') {
        Serial.println("[listen] Hl<shelf>[window]: shelf 0 Most played, 1 New finds, 2 Old favourites, 3 Not "
                       "played yet, 4 Recently played; Most played's window 0 7 days, 1 30 days, 2 All time (the "
                       "default)");
        return;
      }
      shelf_ = static_cast<uint8_t>(c - '0');
      window_ = arg[2] >= '0' && arg[2] <= '2' ? static_cast<uint8_t>(arg[2] - '0') : 2;
      k = kList;
      break;
    }
    default: return;
  }
  if (s == kAsked) Serial.printf("[listen] %s replaced by the new ask (it hadn't begun)\n", name());
  kind_ = k;
  again_ = false;
  restarts_ = 0;
  waitLogged_ = 0;
  askedMs_ = millis();
  state_.store(kAsked);
  Serial.printf("[listen] %s: at the next pass the write gate lets through, the boot's scan done, no library update "
                "asked%s; its memory given back when it ends (nothing held between asks)\n",
                name(), k == kTrack ? "" : ", Auto DJ's join neither working nor due");
}

bool StatsLists::turnWanted(const Gates& g) const {
  if (state_.load() != kAsked || !glue_) return false;
  // Each gate that holds it, said once (a bit each).
  uint32_t why = 0;
  if (!glue_->on()) why = 1;
  else if (glue_->booting() || glue_->scanHolds()) why = 2;
  else if (glue_->store().held()) why = 4;
  else if (g.updateAsked || !g.index) why = 8;
  else if (g.cardHeld || g.cardAway) why = 16;
  else if (kind_ != kTrack && g.joinPending) why = 32;
  else if ((kind_ == kList || kind_ == kWalkAsk) && !g.index->ready()) why = 8;
  if (why == 0) return true;
  StatsLists* self = const_cast<StatsLists*>(this);
  if (!(waitLogged_ & why)) {
    self->waitLogged_ |= why;
    Serial.printf("[listen] %s waits: %s\n", name(),
                  why == 1    ? "the stats are off this session"
                  : why == 2  ? "the boot's scan (its fold, its save)"
                  : why == 4  ? "a save's tmp held (Hs or the next save promotes it first)"
                  : why == 8  ? "the library update (or the index isn't readable)"
                  : why == 16 ? "the card (away, or held)"
                              : "Auto DJ's join (working, or due)");
  }
  return false;
}

void StatsLists::refuse(const char* why) {
  Serial.printf("[listen] %s refused: %s\n", name(), why);
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::turn(const Gates& g) {
  if (!turnWanted(g)) return;
  const int64_t t0 = esp_timer_get_time();
  sf::Fold& fold = glue_->fold();
  const sx::Store::Copy& copy = glue_->store().cardCopy();
  // stats.bin's directory now (its TRKD's rows: the played set's room), the
  // copy the session stands on.
  uint32_t trkdRows = 0;
  if (copy.present) {
    const sg::Glue::FileLook l = glue_->lookFile();
    if (!l.there || !l.frame || l.generation != copy.generation) {
      refuse(l.unread ? "the card failed a read of stats.bin" : "stats.bin isn't the copy this session stands on");
      return;
    }
    trkdRows = l.rows[sf::kTrkd];
  }
  if (kind_ == kTrack && g.playingHash == 0) {
    refuse("nothing playing");
    return;
  }
  const sf::Head& h = fold.head();
  const bool rank = kind_ != kTrack;
  const bool walk = kind_ == kList || kind_ == kWalkAsk;
  const bool playedSet = kind_ == kWalkAsk || (kind_ == kList && shelf_ == 3);
  const uint32_t records = fold.records();
  // The regions.
  size_t parts[8] = {};
  parts[0] = up(sizeof(Ask));
  parts[1] = up(sx::kWorkBytes);
  if (rank) {
    parts[2] = up(sr::Ranker::kBytes);
    parts[3] = up(sr::Windows::bytesFor(sf::kRcntRows + records));
    parts[4] = up(static_cast<size_t>(records) * (sf::kTrackBytes + sf::kStrides[sf::kRcnt]));
  }
  uint32_t playedChunks = 0;
  if (playedSet) {
    playedChunks = sr::playedChunks(h.baseRows + trkdRows + records);
    if (playedChunks > sr::kMaxPlayedChunks) playedChunks = sr::kMaxPlayedChunks;
    parts[5] = static_cast<size_t>(playedChunks) * sr::kPlayedChunkBytes;
  }
  uint32_t folders = 0, wantedRows = 0;
  if (walk) {
    folders = pathhash::Walk::foldersFor(*g.index);
    parts[6] = up(pathhash::Walk::bytesFor(folders)) + up(sm::albumBytes(g.index->albumCount()));
    wantedRows = kind_ == kWalkAsk ? sr::Ranker::kEntries
                 : shelf_ == 3     ? 0
                                   : sr::rowsOf(listOf(shelf_, window_));
    parts[7] = static_cast<size_t>(sm::wantedChunks(wantedRows)) * sm::kChunkBytes;
  }
  size_t bytes = 0;
  for (size_t p : parts) bytes += p;
  autodj::CloseReport cr;
  void* p = hold_.take ? hold_.take(bytes, &cr) : nullptr;
  if (!p) {
    char why[128];
    snprintf(why, sizeof(why), "no PSRAM for its %lu B block (%s)", static_cast<ul>(bytes),
             cr.why == autodj::Close::NoRoom ? "with it held no free block takes a decoder's buffers"
                                             : "no free block takes it");
    refuse(why);
    return;
  }
  block_ = new (p) Ask;
  bytes_ = bytes;
  Ask& a = *block_;
  for (int i = 0; i < 8; ++i) a.parts[i] = parts[i];
  uint8_t* at = static_cast<uint8_t*>(p) + parts[0];
  a.mem.work = at;
  at += parts[1];
  // The rank's input: the copy the session stands on, its base, today.
  a.in.present = copy.present;
  a.in.generation = copy.generation;
  a.in.headerCrc = copy.headerCrc;
  a.in.baseGen = h.baseGen;
  a.in.baseRows = h.baseRows;
  a.in.baseCrc = h.baseCrc;
  uint16_t today = 0;
  a.in.hasToday = sf::todayOf(boardclock::now(), &today);
  a.in.today = today;
  if (rank) {
    a.mem.lists = at;
    at += parts[2];
    a.mem.windows = at;
    a.mem.windowRows = sf::kRcntRows + records;
    at += parts[3];
    // The records' copy (5.14's "The screens"): TRKD's rows, then RCNT's,
    // each sorted and a key's records ⊕'d (Fold::Cursor, in this call: no
    // fold while it is in use).
    const int64_t c0 = esp_timer_get_time();
    uint8_t* recs = at;
    {
      sf::Fold::Cursor cur(fold, sf::kTrkd);
      while (a.trkdRecords < records && cur.next(recs + static_cast<size_t>(a.trkdRecords) * sf::kTrackBytes))
        ++a.trkdRecords;
    }
    uint8_t* rc = recs + static_cast<size_t>(records) * sf::kTrackBytes;
    {
      sf::Fold::Cursor cur(fold, sf::kRcnt);
      while (a.rcntRecords < records &&
             cur.next(rc + static_cast<size_t>(a.rcntRecords) * sf::kStrides[sf::kRcnt]))
        ++a.rcntRecords;
    }
    a.copyUs = esp_timer_get_time() - c0;
    a.in.trkd = recs;
    a.in.trkdRows = a.trkdRecords;
    a.in.rcnt = rc;
    a.in.rcntRows = a.rcntRecords;
    at += parts[4];
    if (playedSet) {
      for (uint32_t i = 0; i < playedChunks; ++i)
        a.played.chunk[i] = reinterpret_cast<uint64_t*>(at + static_cast<size_t>(i) * sr::kPlayedChunkBytes);
      a.played.chunks = playedChunks;
      a.mem.played = &a.played;
    }
    at += parts[5];
    if (walk) {
      a.folders = folders ? at : nullptr;
      a.foldersCap = folders;
      a.bits = at + up(pathhash::Walk::bytesFor(folders));
      a.albumsCap = g.index->albumCount();
      at += parts[6];
      for (uint32_t i = 0; i < sm::wantedChunks(wantedRows); ++i)
        a.wanted.chunk[a.wanted.chunks++] = at + static_cast<size_t>(i) * sm::kChunkBytes;
    }
  } else {
    // Hp: the records' row (the loop's: Fold::track()).
    a.hash = g.playingHash;
    a.recordFound = fold.track(a.hash, &a.recordRow);
  }
  // The files, opened here (a FatFile each: PSRAM; the worker allocates
  // nothing).
  const sr::Outcome o = sr::open(*fs_, a.in, &a.files);
  if (o != sr::Outcome::Ok) {
    char why[96];
    snprintf(why, sizeof(why), "its files didn't open (%s)", sr::outcomeName(o));
    refuse(why);
    return;
  }
  a.queuedUs = esp_timer_get_time();
  rank_ = rank;
  Serial.printf("[listen] %s: its block %lu B at 0x%08lx (%s, %u cap%s); %s queued for the card worker (%.1f ms on "
                "the loop)\n",
                name(), static_cast<ul>(bytes), static_cast<ul>(reinterpret_cast<uintptr_t>(p)), closeText(cr.why),
                static_cast<unsigned>(cr.caps), cr.caps == 1 ? "" : "s", rank ? "the rank" : "the track's read",
                msSince(t0));
  state_.store(rank ? kRankQueued : kTrackQueued);
}

bool StatsLists::start(CardWorker& worker, uint8_t priority, bool rank, uint32_t nowMs) {
  const uint8_t s = state_.load();
  if (!block_ || (rank ? s != kRankQueued : s != kTrackQueued)) return false;
  block_->priority = priority;
  state_.store(kRunning);
  if (!worker.start(CardWorker::Job::Stats, priority, entry, this, nowMs)) {
    state_.store(rank ? kRankQueued : kTrackQueued);  // (asked again at the next pass)
    return false;
  }
  return true;
}

void StatsLists::entry(void* self) {
  StatsLists& s = *static_cast<StatsLists*>(self);
  Ask& a = *s.block_;
  a.startUs = esp_timer_get_time();
  if (s.rank_) {
    a.report = sr::rank(a.files, a.in, a.mem, a.ranker);
  } else {
    a.track = sr::trackRows(a.files, a.in, a.hash, a.mem.work);
  }
  a.endUs = esp_timer_get_time();
  s.state_.store(kDone);
}

void StatsLists::giveBack() {
  if (!block_) return;
  if (fs_) sr::close(*fs_, &block_->files);
  block_->~Ask();
  if (hold_.give) hold_.give(block_);
  block_ = nullptr;
  bytes_ = 0;
}

void StatsLists::loop(const Gates& g) {
  if (state_.load() != kDone) return;
  Ask& a = *block_;
  sr::close(*fs_, &a.files);  // (on the loop, where they were opened)
  if (rank_) {
    finishRank(g);
  } else {
    finishTrack();
  }
}

void StatsLists::finishRank(const Gates& g) {
  Ask& a = *block_;
  const sr::Report& r = a.report;
  const sr::Ranker& k = a.ranker;
  if (r.outcome != sr::Outcome::Ok) {
    char why[96];
    snprintf(why, sizeof(why), "the rank came to %s on the card worker", sr::outcomeName(r.outcome));
    refuse(why);
    return;
  }
  if (kind_ == kRank) {
    Serial.printf("[listen] Hr: ranked in %.1f ms on the card worker (priority %u; queued %.1f ms before it); the "
                  "records' copy %lu us on the loop; read: the base %lu rows, TRKD %lu, RCNT %lu (%lu in the 30 days), "
                  "the records' %lu, %llu B in all; %lu tracks, %lu played%s\n",
                  (a.endUs - a.startUs) / 1000.0f, static_cast<unsigned>(a.priority),
                  (a.startUs - a.queuedUs) / 1000.0f, static_cast<ul>(a.copyUs), static_cast<ul>(r.baseRows),
                  static_cast<ul>(r.trkdRows), static_cast<ul>(r.rcntRows), static_cast<ul>(r.windowRows),
                  static_cast<ul>(r.recordRows), static_cast<ull>(r.bytesRead), static_cast<ul>(r.tracks),
                  static_cast<ul>(r.played), a.in.hasToday ? "" : "; the clock unset: no windows");
    char lists[160];
    if (k.hasToday()) {
      snprintf(lists, sizeof(lists), "most played 7 days %lu, 30 days %lu, All time %lu; new finds %lu; old favourites "
                                     "%lu; recently played %lu",
               static_cast<ul>(k.size(sr::kMost7)), static_cast<ul>(k.size(sr::kMost30)),
               static_cast<ul>(k.size(sr::kMostAll)), static_cast<ul>(k.size(sr::kNewFinds)),
               static_cast<ul>(k.size(sr::kOldFavourites)), static_cast<ul>(k.size(sr::kRecent)));
    } else {
      snprintf(lists, sizeof(lists), "most played All time %lu; recently played %lu (the windows, new finds and old "
                                     "favourites need the clock)",
               static_cast<ul>(k.size(sr::kMostAll)), static_cast<ul>(k.size(sr::kRecent)));
    }
    Serial.printf("[listen] Hr: the lists (each its cap and 16 at most): %s; its block %lu B (work %lu, lists %lu, "
                  "the windows' rows %lu, the records' copy %lu), given back\n",
                  lists, static_cast<ul>(bytes_), static_cast<ul>(a.parts[1]), static_cast<ul>(a.parts[2]),
                  static_cast<ul>(a.parts[3]), static_cast<ul>(a.parts[4]));
    giveBack();
    kind_ = kNone;
    state_.store(kIdle);
    return;
  }
  // Hl, Hw: the wanted set from the lists, then the walk.
  if (!g.index || !g.index->ready() || g.index->albumCount() > a.albumsCap ||
      pathhash::Walk::foldersFor(*g.index) > a.foldersCap) {
    refuse("the library's index changed before its walk");
    return;
  }
  if (kind_ == kWalkAsk) {
    for (int l = 0; l < sr::kLists; ++l)
      for (uint32_t i = 0; i < k.size(static_cast<sr::List>(l)); ++i) a.wanted.add(k.at(static_cast<sr::List>(l), i).hash);
  } else if (shelf_ != 3) {
    const sr::List l = listOf(shelf_, window_);
    for (uint32_t i = 0; i < k.size(l); ++i) a.wanted.add(k.at(l, i).hash);
  }
  a.wanted.seal();
  if (a.mem.played && a.played.lost) {
    Serial.printf("[listen] %s: %lu played hashes past the played set's room (not counted as played)\n", name(),
                  static_cast<ul>(a.played.lost));
  }
  a.walkT0 = esp_timer_get_time();
  a.map.begin(g.index, a.mem.played, a.wanted.chunks ? &a.wanted : nullptr, a.mem.played ? a.bits : nullptr,
              a.folders);
  state_.store(kWalk);
}

void StatsLists::finishTrack() {
  Ask& a = *block_;
  const sr::TrackRows& t = a.track;
  const float ms = (a.endUs - a.startUs) / 1000.0f;
  if (t.outcome != sr::Outcome::Ok) {
    char why[96];
    snprintf(why, sizeof(why), "the read came to %s on the card worker", sr::outcomeName(t.outcome));
    refuse(why);
    return;
  }
  // As Ht holds it: the base's row ⊕ (TRKD's ⊕ the records').
  sg::TrackLook look;
  look.base = a.in.baseGen == 0 ? sg::TrackLook::Base::None
              : t.base          ? sg::TrackLook::Base::Found
                                : sg::TrackLook::Base::NotThere;
  look.baseRow = t.baseRow;
  look.reads = t.reads;
  if (t.delta && a.recordFound) {
    look.deltaRow = sf::merge(t.deltaRow, a.recordRow);
  } else if (t.delta) {
    look.deltaRow = t.deltaRow;
  } else if (a.recordFound) {
    look.deltaRow = a.recordRow;
  }
  look.delta = t.delta || a.recordFound;
  if (t.base && look.delta) {
    look.row = sf::merge(look.baseRow, look.deltaRow);
  } else if (t.base) {
    look.row = look.baseRow;
  } else if (look.delta) {
    look.row = look.deltaRow;
  } else {
    look.row.hash = 0;
  }
  if (print_) print_("Hp", a.hash, look, ms);
  Serial.printf("[listen] Hp: read on the card worker in %.1f ms (priority %u; queued %.1f ms before it): %lu base "
                "rows read (a binary search), TRKD's %lu rows passed; its block %lu B given back\n",
                ms, static_cast<unsigned>(a.priority), (a.startUs - a.queuedUs) / 1000.0f, static_cast<ul>(t.reads),
                static_cast<ul>(t.trkdRows), static_cast<ul>(bytes_));
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::stepMap(const Gates& g) {
  if (state_.load() != kWalk) return;
  Ask& a = *block_;
  // Never with the join's slices in the same pass (6.2: two walks in one
  // pass), a list moving, the card held, the UI not up. (Not held by an
  // update asked: the update waits for this block, statsWait, so its fence
  // never comes under the walk, and waiting for it here would hold both.)
  if (g.joinWorking || g.listMoving || g.cardHeld || !g.uiStarted || !g.index) return;
  const int64_t t0 = esp_timer_get_time();
  PassClock clock{t0 + kWalkPassUs};
  const sm::Map::Status s = a.map.stepPass(g.index, passTimeLeft, &clock);
  const int64_t us = esp_timer_get_time() - t0;
  ++a.passes;
  a.walkUs += us;
  if (us > a.walkMaxUs) a.walkMaxUs = us;
  if (s == sm::Map::Status::Working) return;
  if (s == sm::Map::Status::Aborted) {
    if (++restarts_ > kRestarts || !g.index || !g.index->ready()) {
      refuse("the library's index changed under its walk, again and again");
      return;
    }
    Serial.printf("[listen] %s: the library's index changed under its walk: begun again (%u of %u)\n", name(),
                  static_cast<unsigned>(restarts_), static_cast<unsigned>(kRestarts));
    // (Its blocks were sized for the index it began on.)
    if (pathhash::Walk::foldersFor(*g.index) > a.foldersCap || g.index->albumCount() > a.albumsCap) {
      refuse("the library's new index is larger than its block");
      return;
    }
    a.map.begin(g.index, a.mem.played, a.wanted.chunks ? &a.wanted : nullptr, a.mem.played ? a.bits : nullptr,
                a.folders);
    return;
  }
  if (kind_ == kWalkAsk) {
    printWalk(g);
  } else {
    printLists(g);
  }
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::printWalk(const Gates& g) {
  Ask& a = *block_;
  const sm::Map::Stats& m = a.map.stats();
  const uint32_t played = a.played.n;
  const uint32_t matched = m.playedTracks > played ? played : m.playedTracks;
  const uint32_t albums = g.index->albumCount();
  Serial.printf("[listen] Hw: walked %lu tracks (%lu without a path hash) in %lu passes, %.0f ms from its start (%.1f ms "
                "of slices, the longest pass %.1f ms; %u restarts; the folders' states %s); the played set %lu hashes: "
                "%lu matched to a track, %lu not (orphans); albums %lu played of %lu (not played yet %lu); the lists' "
                "%lu hashes: %lu mapped, %lu collisions; the rank %.1f ms on the card worker; its block %lu B given "
                "back\n",
                static_cast<ul>(m.tracks), static_cast<ul>(m.unhashed), static_cast<ul>(a.passes),
                (esp_timer_get_time() - a.walkT0) / 1000.0f, a.walkUs / 1000.0f, a.walkMaxUs / 1000.0f,
                static_cast<unsigned>(restarts_), a.folders ? "kept" : "none (whole paths)", static_cast<ul>(played),
                static_cast<ul>(matched), static_cast<ul>(played - matched), static_cast<ul>(m.albumsPlayed),
                static_cast<ul>(albums), static_cast<ul>(albums - m.albumsPlayed), static_cast<ul>(a.wanted.n),
                static_cast<ul>(m.wantedMapped), static_cast<ul>(m.collisions), (a.endUs - a.startUs) / 1000.0f,
                static_cast<ul>(bytes_));
}

void StatsLists::printLists(const Gates& g) {
  Ask& a = *block_;
  const LibraryIndex& idx = *g.index;
  if (shelf_ == 3) {
    // Not played yet: the albums of albumsAZ() with no track played.
    const LibraryIndex::Span az = idx.albumsAZ();
    uint32_t n = 0;
    for (uint32_t i = 0; i < az.count; ++i) n += a.map.albumPlayed(az[i]) ? 0 : 1;
    Serial.printf("[listen] Hl3 (Not played yet): %lu albums of %lu (walked in %lu passes, %.1f ms of slices)\n",
                  static_cast<ul>(n), static_cast<ul>(az.count), static_cast<ul>(a.passes), a.walkUs / 1000.0f);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < az.count && shown < 300; ++i) {
      if (a.map.albumPlayed(az[i])) continue;
      ++shown;
      Serial.printf("  %3lu. #%lu %.80s\n", static_cast<ul>(shown), static_cast<ul>(az[i]), idx.albumName(az[i]));
    }
    if (n > shown) Serial.printf("  ... and %lu more\n", static_cast<ul>(n - shown));
    return;
  }
  const sr::List l = listOf(shelf_, window_);
  const sr::Ranker& k = a.ranker;
  char label[48];
  listLabel(shelf_, window_, label, sizeof(label));
  if (!k.offered(l)) {
    Serial.printf("[listen] %s: needs the board's clock (unset)\n", label);
    return;
  }
  uint32_t orphans = 0;
  for (uint32_t i = 0; i < k.size(l); ++i) orphans += a.wanted.idOf(k.at(l, i).hash) == sm::kNoId ? 1 : 0;
  Serial.printf("[listen] %s: %lu rows (the cap %lu and 16 more), %lu with no track of this library (orphans, or a "
                "hash two tracks share); walked in %lu passes, %.1f ms of slices\n",
                label, static_cast<ul>(k.size(l)), static_cast<ul>(sr::kCaps[l]), static_cast<ul>(orphans),
                static_cast<ul>(a.passes), a.walkUs / 1000.0f);
  for (uint32_t i = 0; i < k.size(l); ++i) {
    const sr::Entry& e = k.at(l, i);
    const uint32_t id = a.wanted.idOf(e.hash);
    char what[64];
    char t[24];
    switch (l) {
      case sr::kMost7:
      case sr::kMost30:
        dayText(e.at, t, sizeof(t));
        snprintf(what, sizeof(what), "%lu plays, the last on %s", static_cast<ul>(e.count), t);
        break;
      case sr::kNewFinds:
        timeText(e.at, t, sizeof(t));
        snprintf(what, sizeof(what), "%lu plays, found %s (%s)", static_cast<ul>(e.count), t, srcName(e.src));
        break;
      case sr::kRecent:
        timeText(e.at, t, sizeof(t));
        if (e.at) {
          snprintf(what, sizeof(what), "%lu plays, last %s", static_cast<ul>(e.count), t);
        } else {
          snprintf(what, sizeof(what), "%lu plays, undated (round %lu)", static_cast<ul>(e.count), static_cast<ul>(e.ord));
        }
        break;
      default:
        timeText(e.at, t, sizeof(t));
        snprintf(what, sizeof(what), "%lu plays, last %s", static_cast<ul>(e.count), t);
        break;
    }
    if (id == sm::kNoId) {
      Serial.printf("  %3lu. %016llx  -      %s  (no track: left out of the shelf)\n", static_cast<ul>(i + 1),
                    static_cast<ull>(e.hash), what);
    } else {
      uint8_t len = 0;
      const char* title = idx.trackTitle(id, &len);
      Serial.printf("  %3lu. %016llx  #%-5lu %s  %.*s\n", static_cast<ul>(i + 1), static_cast<ull>(e.hash),
                    static_cast<ul>(id), what, static_cast<int>(len), title);
    }
  }
}

bool StatsLists::waitWorker(uint32_t maxMs) {
  const uint32_t t0 = millis();
  while (state_.load() == kRunning && millis() - t0 < maxMs) delay(5);
  return state_.load() != kRunning;
}

size_t StatsLists::drop() {
  if (!block_) return 0;
  // (A worker's step under way: its end first, the files it reads closed
  // after; the fence never comes while the block is held, holds() keeping
  // the update waiting: this is the guard.)
  if (!waitWorker(5000)) return 0;
  const size_t n = bytes_;
  giveBack();
  if (kind_ != kNone) {
    again_ = true;
    restarts_ = 0;
    waitLogged_ = 0;
    state_.store(kAsked);
    Serial.printf("[listen] %s: its block given back at the library update's fence (%lu B): begun again after it\n",
                  name(), static_cast<ul>(n));
  }
  return n;
}

void StatsLists::ending(uint32_t maxMs) {
  if (!waitWorker(maxMs)) return;  // (still reading: the end comes all the same, nothing saved over it)
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}
