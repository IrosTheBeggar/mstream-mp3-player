// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/StatsLists.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstring>
#include <new>

#include "CardContract.h"
#include "PathHashWalk.h"
#include "StatsFile.h"
#include "StatsFold.h"
#include "StatsView.h"
#include "WallClock.h"
#include "app/BoardClock.h"
#include "app/ListeningStats.h"

namespace {

namespace sf = statsfold;
namespace sx = statsfile;
namespace sr = statsrank;
namespace sm = statsmap;
namespace sg = statsglue;
namespace sv = statsvisit;
using Wait = ScanScheduler::Wait;

using ul = unsigned long;
using ull = unsigned long long;

// The walk's slices a pass, as Auto DJ's join's (kJoinPassUs: 6.2's "up to
// 8 ms a pass"), and the times an index that changed under it begins it again.
constexpr int64_t kWalkPassUs = 8000;
constexpr uint8_t kRestarts = 3;
// A step queued this long and not taken by the card worker (none this boot,
// or held behind the boot's walk, a compaction and the yields) is given up:
// its block back, a line.
constexpr uint32_t kQueuedMs = 30000;
constexpr size_t kAlign = 16;

// S3a (docs/LISTENING-STATS.md 9.7, "The lit hold", the owner's default (a)):
// the lit cap's default (Hc0/Hc1 change it for the session, RAM) and the
// visit's summed hold it is set against. On: run 3a-1 measured every lit
// Full and Lists visit with it off holding its block past 3 s (up to
// 6,104 ms a take), and under 2.4 s with it on; the owner's "Cap on".
constexpr bool kLitCap = true;
constexpr uint32_t kLitCapMs = 3000;
// A visit's walk while lit (7.10): 100 tracks a slice, up to 4 ms a pass;
// dark (or capped) S2's: 250 tracks, up to kWalkPassUs.
constexpr int64_t kLitWalkPassUs = 4000;
constexpr uint32_t kLitSlice = 100;
constexpr uint32_t kDarkSlice = 250;
// A visit's walk that skips passes this long gives its block back (a lit
// list moving, the card held: 9.7's hold bounds).
constexpr uint32_t kWalkSkipMs = 1000;

size_t up(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }
size_t psramLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
size_t psramFreeBytes() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

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

// A list as the snapshot's lines name it.
const char* listText(sr::List l) {
  switch (l) {
    case sr::kMost7: return "most played 7 days";
    case sr::kMost30: return "30 days";
    case sr::kMostAll: return "All time";
    case sr::kNewFinds: return "new finds";
    case sr::kOldFavourites: return "old favourites";
    default: return "recently played";
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
    case autodj::Close::Full: return "the hold full: not tracked, nothing closed";
    case autodj::Close::NoRoom: return "given back: no room for a decoder's buffers";
    case autodj::Close::NoBlock: return "no free block takes it";
    default: return "?";
  }
}

// S2's gate bits (statsvisit::kWhy...), as the waits' lines name them.
const char* gateName(uint8_t why) {
  return why == sv::kWhyOff      ? "the stats are off this session"
         : why == sv::kWhyBoot   ? "the boot's scan (its fold, its save)"
         : why == sv::kWhyTmp    ? "a save's tmp held (Hs or the next save promotes it first)"
         : why == sv::kWhyUpdate ? "the library update (or the index isn't readable)"
         : why == sv::kWhyCard   ? "the card (away, or held)"
         : why == sv::kWhyJoin   ? "Auto DJ's join (working, or due)"
         : why == sv::kWhyWorker ? "the card worker's walk or compaction (a rank comes after them)"
                                 : "the card worker's tag scan (its memory held)";
}

// Why the Dance tab's block wasn't lent for a snapshot of `need` bytes
// (statshold::borrow() refused: statsvisit::loanOk()).
const char* noLoanText(size_t need) {
  return need > sv::kBound       ? "past the Dance tab's 4,096 B block (more than 18,432 albums)"
         : statshold::danceUp() ? "the Dance tab is up (its tracker reads into the block; a page asks again once the "
                                  "tab is down, a console ask doesn't)"
                                : "the Dance tab's block wasn't made at the boot (no PSRAM for the dancer)";
}

// The waits an ask met before its take: S2's gate bits and the Rank lane's
// (a bit a ScanScheduler::Wait), "none" for none. (`stalled`: kWhyWorker
// was the 30 s worker give-up's hold, not the walk's.)
void waitsText(uint8_t gates, uint16_t lane, bool stalled, char* out, size_t cap) {
  size_t n = 0;
  out[0] = 0;
  static constexpr uint8_t kShort[] = {sv::kWhyBoot, sv::kWhyTmp,    sv::kWhyUpdate, sv::kWhyCard,
                                       sv::kWhyJoin, sv::kWhyWorker, sv::kWhyRest};
  static constexpr const char* kShortName[] = {"the boot's scan", "a save's tmp",           "the library update",
                                               "the card",        "Auto DJ's join",         "the card worker's walk",
                                               "the card worker's tag scan"};
  static_assert(sizeof(kShort) == sizeof(kShortName) / sizeof(kShortName[0]), "a name a gate bit");
  for (size_t i = 0; i < sizeof(kShort); ++i) {
    if (!(gates & kShort[i]) || n + 2 >= cap) continue;
    const char* what = stalled && kShort[i] == sv::kWhyWorker ? "the card worker's give-up" : kShortName[i];
    n += static_cast<size_t>(snprintf(out + n, cap - n, "%s%s", n ? ", " : "", what));
  }
  for (int w = 0; w < ScanScheduler::kWaits; ++w) {
    if (!(lane & (1u << w)) || n + 2 >= cap) continue;
    n += static_cast<size_t>(
        snprintf(out + n, cap - n, "%s%s", n ? ", " : "", ScanScheduler::waitName(static_cast<Wait>(w))));
  }
  if (n == 0) snprintf(out, cap, "none");
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
  if (day == sf::kUnsetDay) {
    snprintf(out, cap, "none");
    return;
  }
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
  snprintf(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}
// Recently played's `when` (minutes since 2026-01-01, or the codes: a dated
// snapshot's rows with no dated play, and every row of an undated one, a
// dated row of an earlier session's among them: the owner's call,
// 2026-11-24).
void whenText(uint32_t when, char* out, size_t cap) {
  if (when == sv::kWhenSession || when == sv::kWhenEarlier) {
    snprintf(out, cap, "%s", when == sv::kWhenSession ? "this session (no time)" : "earlier (no time)");
    return;
  }
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(when) * 60000);
  snprintf(out, cap, "%04d-%02d-%02d %02d:%02d", c.year, c.month, c.day, c.hour, c.minute);
}
// "7 h 37 min", "45 min" (4.1's times).
void listenedText(uint32_t ms, char* out, size_t cap) {
  const uint32_t min = ms / 60000;
  if (min >= 60) {
    snprintf(out, cap, "%lu h %lu min", static_cast<ul>(min / 60), static_cast<ul>(min % 60));
  } else {
    snprintf(out, cap, "%lu min", static_cast<ul>(min));
  }
}

const char* srcName(uint8_t s) { return s == sf::kAutoDj ? "Auto DJ" : s == sf::kShuffle ? "shuffle" : "you"; }

// Whether Not played yet may open (call 4, O5: statsview::notPlayedOpen()
// decides the shelf; this is the visit's grade): dated, 30 counting days or
// more; with no clock, perhaps (DAYS' span, the ask's report, decides it),
// so the visit asks for its played set too. The rule is lib/core's
// (statsvisit::notPlayedMayOpen(), host-tested), the shelf's too, so the
// two can't disagree.
bool notPlayedMayOpen(const sf::Fold& fold) {
  const uint64_t now = boardclock::now();
  uint16_t today = 0;
  if (!sf::todayOf(now, &today)) return sv::notPlayedMayOpen(false, 0, sf::kUnsetDay);
  return sv::notPlayedMayOpen(true, today, fold.sinceDayNow(now));
}

}  // namespace

// The block's head: the ask's state, then its blocks.
struct StatsLists::Ask {
  // The worker's (published by state_: written before kRankQueued /
  // kTrackQueued, read back after kDone).
  sr::Input in;
  sr::Memory mem;
  sr::Files files;
  sr::Played played;
  sr::Played40 played40;  // S3a's asks' (a visit's Full grade, the lean ask)
  sr::Ranker ranker;
  sr::Report report;
  sr::TrackRows track;
  uint64_t hash = 0;
  int64_t queuedUs = 0, startUs = 0, endUs = 0;
  uint8_t priority = 0;
  // The loop's.
  int64_t copyUs = 0;      // the records' copy (Fold::Cursor), on the loop
  uint32_t trkdRecords = 0, rcntRecords = 0, daysRecords = 0;
  statsfold::Track recordRow;  // Hp: the records' row
  bool recordFound = false;
  uint8_t* records = nullptr;  // the records' copy: 40 TRKD rows' room, then 40 RCNT rows' (and S3a's 40 DAYS rows')
  sm::Map map;
  sm::Wanted wanted;
  uint8_t* bits = nullptr;
  void* folders = nullptr;
  uint32_t foldersCap = 0, albumsCap = 0;  // what the folders' states and the bits hold
  uint32_t passes = 0;
  int64_t walkT0 = 0, walkUs = 0, walkMaxUs = 0;
  size_t parts[8] = {};  // the regions' bytes (Hr's line)
  size_t bytes = 0;      // the block's
  // S3a's visit and lean asks: what the steps after the take keep of the
  // grade's layout (statsvisit::layout(): its regions' offsets from the
  // block's start, 0 none; the whole Layout, 92 B, rode in every grade's
  // block), the hold and the loop's pieces (run 3a-2: each under 35 ms, so
  // u32 microseconds).
  struct Took {
    uint32_t wantedAt = 0, foldersAt = 0, bitsAt = 0;
    uint16_t wantedRows = 0;
    sv::Grade grade = sv::Grade::Lists;
    bool dated = false;   // window room (the clock set at the take)
    bool played = false;  // a u40 set (Full, Lean, NoFolders)
  };
  Took y;
  int64_t takeUs = 0;                // the take: the hold's start
  uint32_t turnUs = 0, lookUs = 0;   // the turn on the loop (stats.bin's directory among it)
  uint32_t handUs = 0, openUs = 0;   // the hand-off on the loop (the records' copy, the opens)
  uint32_t finishUs = 0, sealUs = 0, writeUs = 0, resolveUs = 0, notPlayedUs = 0;
  uint32_t walkLastMs = 0;           // the walk's last pass with a slice (Gates::nowMs)
  bool litTake = false, litRank = false, litWalk = false;
  bool set = false;                 // the walk keeps the album bits (the u40 set whole)
};
static_assert(sr::Ranker::kEntries <= 0xFFFF, "Took::wantedRows is a u16");

void StatsLists::begin(sg::Glue* glue, tagstore::Fs* fs) {
  // The head of every S3a grade's block (statsvisit::layout()'s `head`),
  // which the heap model's S3 grades read as statsvisit::kAskHeadBytes (the
  // host can't build src/app): a change to Ask moves every grade byte for
  // byte, so the model's constant and its pinned grades move with it.
  static_assert(sizeof(Ask) == sv::kAskHeadBytes,
                "StatsLists::Ask changed: statsvisit::kAskHeadBytes and test/test_autodj_join's pinned S3 grades");
  // The object (internal RAM's .bss): S3a's 32 B over S2's at most (9.7);
  // the tag scan's gate a bit beside the walk's (ahead_), no byte more.
  static_assert(sizeof(StatsLists) == 44, "StatsLists' .bss: 44 B (9.7's cap)");
  glue_ = glue;
  fs_ = fs;
  if (kLitCap) flags_ = static_cast<uint8_t>(flags_ | kCapOn);
}

const char* StatsLists::name() const {
  switch (kind_) {
    case kRank: return "Hr";
    case kList: return "Hl";
    case kWalkAsk: return "Hw";
    case kTrack: return "Hp";
    case kVisit:
      return (flags_ & kFromUi) ? "the visit's ask" : (flags_ & kHu) ? "Hu" : (flags_ & kForced) ? "Hv1" : "Hv";
    case kLean: return (flags_ & kFromUi) ? "Not played yet's ask" : (flags_ & kForced) ? "Hn1" : "Hn";
    default: return "H";
  }
}

void StatsLists::command(const char* arg) {
  // S3a's letters that ask nothing.
  switch (arg[0]) {
    case 'k':
      if (!snap_) {
        Serial.println("[listen] Hk: no snapshot held (Hu holds one; a stats page's visit)");
      } else {
        // (Printed from the next pass, with the index: as the TX buffer's
        // room allows, stepHk().)
        hkPart_ = 1;
        hkRow_ = 0;
      }
      return;
    case 'c':
      if (arg[1] == '0' || arg[1] == '1') {
        flags_ = static_cast<uint8_t>(arg[1] == '1' ? flags_ | kCapOn : flags_ & ~kCapOn);
      }
      Serial.printf("[listen] Hc: the lit cap %s this session (its default %s, kLitCap): %s\n",
                    (flags_ & kCapOn) ? "ON" : "off", kLitCap ? "on" : "off",
                    (flags_ & kCapOn) ? "the stats pages' asks and Hv, Hn, Hu rank at priority 1 and walk with the "
                                        "dark's passes, lit or not, and a visit's block is given back once its holds "
                                        "reach 3 s in all (Capped)"
                                      : "lit asks rank at priority 0, and a lit walk takes 4 ms a pass");
      return;
    case 'u':
      if (arg[1] != '0') break;
      // Hu0: the held fake visit's snapshot released (its ask too, if one
      // is asked again after a fence). Hu's alone (9.3): a page's, or one an
      // ask under way borrowed, is kept.
      if (kind_ == kVisit && !(flags_ & kFromUi) && (flags_ & kHu) && state_.load() != kIdle) {
        stop(Outcome::Cancelled, "Hu0");
      }
      if (!snap_) {
        Serial.println("[listen] Hu0: no snapshot held");
      } else if (!(flags_ & kFake)) {
        Serial.printf("[listen] Hu0: the snapshot held (%lu B at 0x%08lx) isn't Hu's (%s): kept\n",
                      static_cast<ul>(snapshotBytes()), static_cast<ul>(reinterpret_cast<uintptr_t>(snap_)),
                      snap_->madeUp() ? "a made-up view's, uiSx: uiSx0 ends it"
                                      : "a stats page's, or an ask's under way");
      } else {
        freeSnap("Hu0");
      }
      flags_ = static_cast<uint8_t>(flags_ & ~kFake);
      return;
    default: break;
  }
  const uint8_t s = state_.load();
  if (s != kIdle && (flags_ & kFromUi)) {
    Serial.printf("[listen] H%c: %s under way: one ask at a time\n", arg[0], name());
    return;
  }
  if (s != kIdle && s != kAsked) {
    Serial.printf("[listen] %s under way: one ask at a time\n", name());
    return;
  }
  Kind k = kNone;
  uint8_t flags = 0;
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
    case 'v':
      k = kVisit;
      if (arg[1] == '1') flags = kForced;
      break;
    case 'n':
      k = kLean;
      if (arg[1] == '1') flags = kForced;
      break;
    case 'u':
      k = kVisit;
      flags = kHu;
      break;
    default: return;
  }
  if (s == kAsked && visiting()) {
    // (In Asked it holds no block; but a console visit given back after its
    // take, putBack(), still holds the snapshot it borrowed: released with
    // it, as no ask would own it any more.)
    const bool carved = (flags_ & kCarved) && !(flags_ & kFake) && snap_;
    Serial.printf("[listen] %s replaced by the new ask (in Asked, holding no block%s)\n", name(),
                  carved ? "; the snapshot it borrowed released" : "");
    if (carved) freeSnap("its console ask replaced (a console ask holds nothing)");
  } else if (s == kAsked) {
    Serial.printf("[listen] %s replaced by the new ask (it hadn't begun)\n", name());
  }
  if (k == kVisit || k == kLean) {
    beginAsk(k, flags, millis());
    Serial.printf("[listen] %s: %s, as a stats page asks: at the next pass the write gate lets through, the boot's "
                  "scan done, no library update asked, Auto DJ's join neither working nor due, no card worker's walk "
                  "or compaction, no tag scan holding its memory (not waited for while the battery floor pauses the "
                  "scan: the take's decoder-room check then), and the Rank lane's waits clear (none taken while one "
                  "holds); its block given back when it ends%s\n",
                  name(),
                  k == kLean        ? (flags ? "Not played yet's lean ask, without the folders' states (forced)"
                                             : "Not played yet's lean ask (the u40 set; without the folders' states if "
                                               "refused)")
                  : flags & kHu     ? "a held fake visit (Full, else Lists): its snapshot (the Dance tab's block, "
                                      "lent) kept as a page keeps it, until Hu0, the Dance tab coming up (not asked "
                                      "again) or a fence (then asked again after the Live and the rejoin)"
                  : flags & kForced ? "a visit's ask with the Lists grade forced"
                                    : "a visit's ask (Full when Not played yet may open, else Lists; Full refused, "
                                      "Lists)",
                  k == kVisit && !(flags & kHu) ? "; its snapshot (the Dance tab's block, lent: refused while that "
                                                  "tab is up) released after it (a console ask holds nothing) unless "
                                                  "one was held before"
                                                : "");
    return;
  }
  kind_ = k;
  flags_ = static_cast<uint8_t>(flags_ & (kCapOn | kFake | kCapped));
  restarts_ = 0;
  waitLogged_ = 0;
  state_.store(kAsked);
  Serial.printf("[listen] %s: at the next pass the write gate lets through, the boot's scan done, no library update "
                "asked%s; its memory given back when it ends (nothing held between asks)\n",
                name(), k == kTrack ? "" : ", Auto DJ's join neither working nor due");
}

uint8_t StatsLists::gateOf(const Gates& g) const {
  // The index only for the asks that walk; a library update asked for every
  // ask (the index goes behind its fence; and the update waits for an ask's
  // block).
  const bool walk = kind_ == kList || kind_ == kWalkAsk || visiting();
  // (S3a's asks: a remount's read of stats.bin to make isn't the stats off,
  // but the boot's scan's wait: on() is false until it settles. S2's line
  // as S2 had it.)
  if (!glue_->on() && !(visiting() && glue_->rereadPending())) return sv::kWhyOff;
  if (glue_->booting() || glue_->scanHolds()) return sv::kWhyBoot;
  if (glue_->store().held()) return sv::kWhyTmp;
  if (g.updateAsked || (walk && (!g.index || !g.index->ready()))) return sv::kWhyUpdate;
  if (g.cardHeld || g.cardAway) return sv::kWhyCard;
  if (kind_ != kTrack && g.joinPending) return sv::kWhyJoin;
  if (kind_ != kTrack && (ahead_ & kAheadWalk)) return sv::kWhyWorker;
  // S3a's asks: the tag scan while it holds its memory (its work memory and
  // any View: 90-124 KB of PSRAM no gate counts; the owner's "Only while
  // held", 2026-11-21; CardTasks' word, sv::restHolds(), which leaves out
  // memory no step gives back, CardJobs::scanStuck(), and the battery
  // floor's pause, the owner's "Don't wait at the floor", 2026-11-22: then
  // the take's own decoder-room check guards playback). S2's asks don't
  // wait for it.
  if (visiting() && (ahead_ & kAheadRest)) return sv::kWhyRest;
  // S3a's asks after the 30 s worker give-up (kStalled): in Asked, holding
  // nothing, sv::kStallMs from it (queuedMs_, which no pass moves while
  // stalled: loop()'s sv::waitStartMoves()), then taken again, the stall
  // cleared at the take; not at once, for a worker that never comes (its
  // task unmade, or held): the take, 30 s queued, the give-up, the take
  // again, the block held all the while and the page Working.
  if (visiting() && sv::stallHolds((flags_ & kStalled) != 0, g.nowMs, queuedMs_)) return sv::kWhyWorker;
  return 0;
}

const char* StatsLists::whyName(uint8_t why) const {
  return why == sv::kWhyWorker && (flags_ & kStalled)
             ? "the card worker (no step of this ask's in 30 s: taken again 30 s after)"
             : gateName(why);
}

Wait StatsLists::laneWait(const Gates& g) const {
  const Wait w = static_cast<Wait>(rankWait_);
  return w == Wait::None && g.input ? Wait::Input : w;
}

bool StatsLists::turnWanted(const Gates& g) const {
  if (state_.load() != kAsked || !glue_) return false;
  const uint8_t why = gateOf(g);
  if (!visiting()) {
    // S2's asks: each gate that holds it said once (a bit each), here, at
    // the passes PlayHistory's turn reaches (S2's lines, as S2 said them).
    if (why && !(waitLogged_ & why)) {
      const_cast<StatsLists*>(this)->waitLogged_ = static_cast<uint8_t>(waitLogged_ | why);
      Serial.printf("[listen] %s waits: %s\n", name(), gateName(why));
    }
    return why == 0;
  }
  // S3a's asks (their waits said at loop(), every pass): none while the
  // Rank lane waits (9.7's pre-turn gate), this pass's input among them
  // (CardTasks' word has it only after this turn: a tap's or a console
  // line's own pass never carves, reads stats.bin's directory or takes).
  return why == 0 && laneWait(g) == Wait::None;
}

void StatsLists::refuse(const char* why) {
  Serial.printf("[listen] %s refused: %s\n", name(), why);
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::turn(const Gates& g) {
  if (!turnWanted(g)) return;
  if (visiting()) {
    turnVisit(g);
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  const sx::Store::Copy& copy = glue_->store().cardCopy();
  const bool rank = kind_ != kTrack;
  const bool walk = kind_ == kList || kind_ == kWalkAsk;
  const bool playedSet = kind_ == kWalkAsk || (kind_ == kList && shelf_ == 3);
  // A list the clock unset can't give (Most played's 7 and 30 days, New
  // finds, Old favourites): said now, nothing carved.
  uint16_t today = 0;
  if (kind_ == kList && shelf_ != 3 && shelf_ != 4 && !(shelf_ == 0 && window_ == 2) &&
      !sf::todayOf(boardclock::now(), &today)) {
    char label[48];
    listLabel(shelf_, window_, label, sizeof(label));
    Serial.printf("[listen] %s: needs the board's clock (unset): nothing ranked\n", label);
    kind_ = kNone;
    state_.store(kIdle);
    return;
  }
  if (kind_ == kTrack && g.playingHash == 0) {
    refuse("nothing playing");
    return;
  }
  // stats.bin's directory now (its TRKD's rows: the played set's room).
  uint32_t trkdRows = 0;
  if (copy.present && playedSet) {
    const sg::Glue::FileLook l = glue_->lookFile();
    if (!l.there || !l.frame || l.generation != copy.generation) {
      refuse(l.unread ? "the card failed a read of stats.bin" : "stats.bin isn't the copy this session stands on");
      return;
    }
    trkdRows = l.rows[sf::kTrkd];
  }
  const sf::Head& h = glue_->fold().head();
  // The regions. The records' room is all 40 (kRecords): the copy is made
  // at the hand-off (prepare()), when more may have been folded; a records'
  // save meanwhile moves them into TRKD, which the played set's count holds.
  const uint32_t records = sf::kRecords;
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
  void* p = statshold::take(bytes, &cr);
  if (!p) {
    char why[128];
    snprintf(why, sizeof(why), "no PSRAM for its %lu B block (%s)", static_cast<ul>(bytes),
             cr.why == autodj::Close::NoRoom ? "with it held no free block takes a decoder's buffers"
                                             : "no free block takes it");
    refuse(why);
    return;
  }
  block_ = new (p) Ask;
  Ask& a = *block_;
  a.bytes = bytes;
  for (int i = 0; i < 8; ++i) a.parts[i] = parts[i];
  uint8_t* at = static_cast<uint8_t*>(p) + parts[0];
  a.mem.work = at;
  at += parts[1];
  if (rank) {
    a.mem.lists = at;
    at += parts[2];
    a.mem.windows = at;
    a.mem.windowRows = sf::kRcntRows + records;
    at += parts[3];
    a.records = at;
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
    a.hash = g.playingHash;
  }
  rank_ = rank;
  queuedMs_ = g.nowMs;
  a.queuedUs = esp_timer_get_time();
  Serial.printf("[listen] %s: its block %lu B at 0x%08lx (%s, %u cap%s); %s queued for the card worker (%.1f ms on "
                "the loop)\n",
                name(), static_cast<ul>(bytes), static_cast<ul>(reinterpret_cast<uintptr_t>(p)), closeText(cr.why),
                static_cast<unsigned>(cr.caps), cr.caps == 1 ? "" : "s", rank ? "the rank" : "the track's read",
                msSince(t0));
  state_.store(rank ? kRankQueued : kTrackQueued);
}

// S3a's turn (9.7: "the snapshot at the turn", "the grades"): a visit's
// snapshot lent first if none is held, then each grade through the hold
// in order; every grade refused, the snapshot this ask borrowed released, No
// room. On the loop, at PlayHistory's turn (the write gate's pass), the
// Rank lane's waits clear.
void StatsLists::turnVisit(const Gates& g) {
  const int64_t t0 = esp_timer_get_time();
  const LibraryIndex& idx = *g.index;  // (gateOf(): ready)
  const bool lean = kind_ == kLean;
  const uint32_t tracks = idx.trackCount(), albums = idx.albumCount();
  if (tracks > sv::kMaxTracks || albums > sv::kMaxAlbums) {
    endVisit(Outcome::NoRoom, "the library is past the snapshot's u16 ids (65,534 tracks): no ask");
    return;
  }
  // Not played yet's lean ask writes its count into the held snapshot at
  // its landing: a page's needs one landed (the console's counts without).
  if (lean && (flags_ & kFromUi) && !snapshot()) {
    endVisit(Outcome::Cancelled, "no landed snapshot to write its count into (a visit's ask first)");
    return;
  }
  // The grades, in order (9.7's grade table).
  sv::Grade grades[2] = {sv::Grade::Lists, sv::Grade::Lists};
  int n = 0;
  const bool forced = (flags_ & kForced) != 0;
  if (lean) {
    if (!forced) grades[n++] = sv::Grade::Lean;
    grades[n++] = sv::Grade::NoFolders;
  } else {
    if (!forced && notPlayedMayOpen(glue_->fold())) grades[n++] = sv::Grade::Full;
    grades[n++] = sv::Grade::Lists;
  }
  // A visit with no snapshot held whose loan would be refused (below):
  // ended before stats.bin's directory is read (a card read and a work
  // memory's carve for an ask that can't go on). borrow() only looks.
  const size_t need = sv::bytesFor(albums);
  if (!lean && !snap_ && !statshold::borrow(need)) {
    Serial.printf("[listen] %s: no block for its snapshot's %lu B: %s\n", name(), static_cast<ul>(need),
                  noLoanText(need));
    endVisit(Outcome::NoBlock, "No room");
    return;
  }
  // stats.bin's directory now, for a grade with a played set (its TRKD's
  // rows: the u40 set's room, HEAD's baseRows + TRKD's + the 40 records',
  // as S2 counts them).
  const sx::Store::Copy& copy = glue_->store().cardCopy();
  uint32_t trkdRows = 0;
  int64_t lookUs = 0;
  if (grades[0] != sv::Grade::Lists && copy.present) {
    const int64_t l0 = esp_timer_get_time();
    const sg::Glue::FileLook l = glue_->lookFile();
    lookUs = esp_timer_get_time() - l0;
    if (!l.there || !l.frame || l.generation != copy.generation) {
      endVisit(l.unread ? Outcome::Io : Outcome::Changed,
               l.unread ? "the card failed a read of stats.bin" : "stats.bin isn't the copy this session stands on");
      return;
    }
    trkdRows = l.rows[sf::kTrkd];
  }
  const uint32_t rows = glue_->fold().head().baseRows + trkdRows + sf::kRecords;
  uint16_t today = 0;
  const bool dated = sf::todayOf(boardclock::now(), &today);
  const uint32_t folders = pathhash::Walk::foldersFor(idx);
  const size_t foldersBytes = pathhash::Walk::bytesFor(folders);
  // A visit's snapshot, before the take: the Dance tab's 4,096 B block,
  // lent (the owner's "Borrow Dance's block", 2026-11-21: carved at the
  // boot for the tab's tracker and kept, so nothing is carved here and
  // PSRAM stays as without the stats). Refused while the tab is up (its
  // tracker reads into it), past 18,432 albums (more than the block), or
  // with no block (no PSRAM for the dancer at the boot): No room. Its line
  // gives the block's address and PSRAM's free beside it, unchanged. (Each
  // line printed as it comes: the loop's stack, 11. Their time, a full TX
  // buffer's wait among it, is left out of the turn's: lineUs; a held
  // snapshot's release among them, by freeSnap()'s word.)
  int64_t lineUs = 0;
  if (!lean) {
    // (The loan is the block whole: short only past 18,432 albums, where
    // no loan is made, the held one released first.)
    if (snap_ && snapshotBytes() < need) {
      lineUs += freeSnap("short of the library's albums now (past the Dance tab's 4,096 B block: 18,432 albums)");
    }
    if (snap_) {
      // (A made-up view's among them: a console Hv or Hu under uiSx<n>.
      // This ask's write() (finishVisit()) clears the mark, so the real
      // snapshot replaces it; the pages read it once the view ends:
      // Ui::statsSnapshot(). An ask that ends before its write (every grade
      // refused, the rank's fault, the card's) leaves it as it was, still
      // the view's: it didn't borrow it, so nothing frees it. So the line
      // says what is to come, not what is done.)
      const int64_t p0 = esp_timer_get_time();
      Serial.printf("[listen] %s: the snapshot held at 0x%08lx (%lu B of the Dance tab's block): rewritten whole%s\n",
                    name(), static_cast<ul>(reinterpret_cast<uintptr_t>(snap_)), static_cast<ul>(need),
                    snap_->madeUp() ? " (a made-up view's, uiSx: replaced by this ask's real one at its write; "
                                      "kept, still the view's, if the ask ends before)"
                                    : "");
      lineUs += esp_timer_get_time() - p0;
    } else {
      const size_t f0 = psramFreeBytes();
      void* s = statshold::borrow(need);
      const size_t f1 = psramFreeBytes();
      if (!s) {
        Serial.printf("[listen] %s: no block for its snapshot's %lu B: %s\n", name(), static_cast<ul>(need),
                      noLoanText(need));
        endVisit(Outcome::NoBlock, "No room");
        return;
      }
      // (Its header zeroed: no magic, nothing served until write(). The
      // block holds the tab's last audio, or an earlier snapshot.)
      std::memset(s, 0, sv::kHeaderBytes);
      snap_ = sv::Snapshot::at(s);
      flags_ = static_cast<uint8_t>(flags_ | kCarved);
      const int64_t p0 = esp_timer_get_time();
      Serial.printf("[listen] %s: the snapshot %lu B in the Dance tab's block at 0x%08lx (%lu B): lent, not carved "
                    "(PSRAM's free %lu B before, %lu B after)\n",
                    name(), static_cast<ul>(need), static_cast<ul>(reinterpret_cast<uintptr_t>(s)),
                    static_cast<ul>(sv::kBound), static_cast<ul>(f0), static_cast<ul>(f1));
      lineUs += esp_timer_get_time() - p0;
    }
    // (Hu's: the snapshot it borrowed kept after its ask, as a page keeps
    // it. One held before it, a page's, stays the page's: Hu0 and the
    // fence's re-ask leave it alone. Over its own, kFake is kept from
    // beginAsk().)
    if ((flags_ & kHu) && (flags_ & kCarved)) flags_ = static_cast<uint8_t>(flags_ | kFake);
  }
  // The grades through the hold (statshold::take(): a decoder's room asked;
  // NoBlock asks the largest free block first, NoRoom gives the block back:
  // a refused take leaves no trace).
  autodj::CloseReport cr;
  void* p = nullptr;
  sv::Layout y;
  for (int i = 0; i < n && !p; ++i) {
    y = sv::layout(grades[i], sizeof(Ask), rows, foldersBytes, albums, dated);
    cr = autodj::CloseReport();
    const size_t largest = psramLargest();
    p = statshold::take(y.bytes, &cr);
    if (p) break;
    const int64_t p0 = esp_timer_get_time();
    Serial.printf("[listen] %s: %s refused: its %lu B (%s; the largest free block %lu B)\n", name(),
                  sv::gradeName(grades[i]), static_cast<ul>(y.bytes),
                  cr.why == autodj::Close::NoRoom ? "with it held no free block takes a decoder's buffers"
                                                  : "no free block takes it",
                  static_cast<ul>(largest));
    lineUs += esp_timer_get_time() - p0;
  }
  if (!p) {
    if ((flags_ & kCarved) && snap_) freeSnap("every grade refused (this ask borrowed it)");
    // (A made-up view's held, not borrowed by this ask: kept and still
    // served under the view, as the turn's line said it may be.)
    endVisit(cr.why == autodj::Close::NoBlock ? Outcome::NoBlock : Outcome::NoRoom,
             snap_ && snap_->madeUp() ? "every grade refused: No room (the made-up view's snapshot kept, still served "
                                        "under it)"
                                      : "every grade refused: No room");
    return;
  }
  block_ = new (p) Ask;
  Ask& a = *block_;
  a.y.wantedAt = static_cast<uint32_t>(y.wanted.at);
  a.y.wantedRows = static_cast<uint16_t>(y.wantedRows);
  a.y.foldersAt = y.folders.bytes ? static_cast<uint32_t>(y.folders.at) : 0;
  a.y.bitsAt = y.bits.bytes ? static_cast<uint32_t>(y.bits.at) : 0;
  a.y.grade = y.grade;
  a.y.dated = y.dated;
  a.y.played = y.played.bytes != 0;
  a.bytes = y.bytes;
  uint8_t* b = static_cast<uint8_t*>(p);
  a.mem.work = b + y.work.at;
  if (y.ranker.bytes) a.mem.lists = b + y.ranker.at;
  if (y.windows.bytes) {
    a.mem.windows = b + y.windows.at;
    a.mem.windowRows = y.windowRows;
  }
  a.records = b + y.records.at;
  if (y.played.bytes) {
    a.played40.rows = b + y.played.at;
    a.played40.cap = y.playedRows;
    a.mem.played40 = &a.played40;
  }
  a.foldersCap = folders;
  a.albumsCap = albums;
  a.litTake = g.lit;
  a.lookUs = static_cast<uint32_t>(lookUs);
  rank_ = true;
  queuedMs_ = g.nowMs;
  a.takeUs = a.queuedUs = esp_timer_get_time();
  a.turnUs = static_cast<uint32_t>(a.takeUs - t0 - lineUs);
  char waits[128];
  waitsText(waitLogged_, rankWaits_, (flags_ & kStalled) != 0, waits, sizeof(waits));
  flags_ = static_cast<uint8_t>(flags_ & ~kStalled);  // (taken: a later give-back's wait is its own)
  Serial.printf("[listen] %s: %s taken at %lu ms: its block %lu B at 0x%08lx (sizeof(Ask) %lu B; %lu u40 rows, %lu "
                "folders, %lu albums, %s; the free block above it %lu B: %s, %u cap%s); waits met before it: %s; the "
                "screen %s; the turn %lu us on the loop (stats.bin's directory %lu us; its lines' %lu us left out): "
                "the rank queued for the card worker\n",
                name(), sv::gradeName(y.grade), static_cast<ul>(g.nowMs), static_cast<ul>(y.bytes),
                static_cast<ul>(reinterpret_cast<uintptr_t>(p)), static_cast<ul>(sizeof(Ask)),
                static_cast<ul>(y.playedRows), static_cast<ul>(folders), static_cast<ul>(albums),
                dated ? "dated" : "undated: no window room", static_cast<ul>(cr.aboveBytes), closeText(cr.why),
                static_cast<unsigned>(cr.caps), cr.caps == 1 ? "" : "s", waits, g.lit ? "lit" : "dark",
                static_cast<ul>(a.turnUs), static_cast<ul>(a.lookUs), static_cast<ul>(lineUs));
  state_.store(kRankQueued);
}

// At the hand-off, on the loop (CardTasks' pass), with the worker free: the
// rank's input as the session stands now (the card's copy, its base,
// today), the records' copy (5.14's "The screens": TRKD's rows, then
// RCNT's, each sorted and a key's records ⊕'d, Fold::Cursor in this call:
// no fold while it is in use; S3a's visit DAYS' after them, its lean ask
// TRKD's alone), and stats.bin and the base opened (a FatFile each: PSRAM;
// the worker allocates nothing). From here until the step is taken in, no
// stats save and no slot is made (workerOwned()). False: the files didn't
// open (refused, or for S3a's asks ended Unread).
bool StatsLists::prepare() {
  Ask& a = *block_;
  sf::Fold& fold = glue_->fold();
  const sx::Store::Copy& copy = glue_->store().cardCopy();
  const sf::Head& h = fold.head();
  a.in = sr::Input();
  a.in.present = copy.present;
  a.in.generation = copy.generation;
  a.in.headerCrc = copy.headerCrc;
  a.in.baseGen = h.baseGen;
  a.in.baseRows = h.baseRows;
  a.in.baseCrc = h.baseCrc;
  uint16_t today = 0;
  a.in.hasToday = sf::todayOf(boardclock::now(), &today);
  a.in.today = today;
  // (A visit's layout made undated has no window room: it ranks undated,
  // should the clock be set since its turn.)
  if (visiting() && !a.y.dated) a.in.hasToday = false;
  // The round this session's live plays have: ranked with no clock,
  // Recently played's rows of it first (This session: the owner's call,
  // 2026-11-24), and the copy-out's This session by the same round.
  a.in.liveRound = fold.liveRound();
  if (rank_) {
    const uint32_t records = sf::kRecords;
    const bool lists = !visiting() || a.mem.lists;  // (the lean ask's played-set-only rank: TRKD's alone)
    const int64_t c0 = esp_timer_get_time();
    uint8_t* recs = a.records;
    a.trkdRecords = a.rcntRecords = a.daysRecords = 0;
    {
      sf::Fold::Cursor cur(fold, sf::kTrkd);
      while (a.trkdRecords < records && cur.next(recs + static_cast<size_t>(a.trkdRecords) * sf::kTrackBytes))
        ++a.trkdRecords;
    }
    uint8_t* rc = recs + static_cast<size_t>(records) * sf::kTrackBytes;
    if (lists) {
      sf::Fold::Cursor cur(fold, sf::kRcnt);
      while (a.rcntRecords < records &&
             cur.next(rc + static_cast<size_t>(a.rcntRecords) * sf::kStrides[sf::kRcnt]))
        ++a.rcntRecords;
    }
    uint8_t* dy = rc + static_cast<size_t>(records) * sf::kStrides[sf::kRcnt];
    if (visiting() && lists) {
      sf::Fold::Cursor cur(fold, sf::kDays);
      while (a.daysRecords < records &&
             cur.next(dy + static_cast<size_t>(a.daysRecords) * sf::kStrides[sf::kDays]))
        ++a.daysRecords;
    }
    a.copyUs = esp_timer_get_time() - c0;
    a.in.trkd = recs;
    a.in.trkdRows = a.trkdRecords;
    if (lists) {
      a.in.rcnt = rc;
      a.in.rcntRows = a.rcntRecords;
    }
    if (a.daysRecords) {
      a.in.days = dy;
      a.in.daysRows = a.daysRecords;
    }
  } else {
    a.recordFound = fold.track(a.hash, &a.recordRow);
  }
  const int64_t o0 = esp_timer_get_time();
  const sr::Outcome o = sr::open(*fs_, a.in, &a.files);
  a.openUs = static_cast<uint32_t>(esp_timer_get_time() - o0);
  if (o == sr::Outcome::Ok) return true;
  char why[96];
  snprintf(why, sizeof(why), "its files didn't open (%s)", sr::outcomeName(o));
  if (visiting()) {
    endVisit(sv::outcomeOf(o), why);
  } else {
    refuse(why);
  }
  return false;
}

uint8_t StatsLists::rankPriority(uint8_t p) const {
  return visiting() && (flags_ & kCapOn) ? ScanScheduler::kHighPriority : p;
}

bool StatsLists::start(CardWorker& worker, uint8_t priority, bool rank, uint32_t nowMs, bool dark) {
  const uint8_t s = state_.load();
  if (!block_ || (rank ? s != kRankQueued : s != kTrackQueued)) return false;
  // The same checks as the turn's: the session's card still the one to read.
  if (!glue_->on() || glue_->store().held() || glue_->booting()) {
    if (visiting()) {
      putBack("the stats' state changed while it waited for the card worker", nowMs);
    } else {
      refuse("the stats' state changed while it waited for the card worker");
    }
    return false;
  }
  // (No task, and its spawn not due (its stack refused within kRetryMs):
  // start() would refuse; the records' copy and the opens not made for it
  // every pass.)
  if (!worker.mayStart(nowMs)) return false;
  const int64_t h0 = esp_timer_get_time();
  if (!prepare()) return false;
  // The lit cap: a visit's rank at priority 1, lit or not (9.7).
  priority = rankPriority(priority);
  block_->priority = priority;
  block_->litRank = !dark;
  block_->handUs = static_cast<uint32_t>(esp_timer_get_time() - h0);
  state_.store(kRunning);
  if (!worker.start(CardWorker::Job::Stats, priority, entry, this, nowMs)) {
    sr::close(*fs_, &block_->files);  // (opened again at the next hand-off)
    state_.store(rank ? kRankQueued : kTrackQueued);
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
  statshold::give(block_);
  block_ = nullptr;
}

void StatsLists::loop(const Gates& g) {
  if (!glue_) return;
  const uint32_t now = g.nowMs;
  const Wait lane = laneWait(g);  // (this pass's input in it)
  if (hkPart_) stepHk(g);
  // The snapshot's releases that come with no navigation (9.7's "the snapshot's
  // life"): the stats off; the card away or held (an S3a ask under way
  // dropped with it: No card, the page's Try again once it is back); a
  // remount whose index isn't the snapshot's key (not while a visit's ask
  // is under way: it rewrites the snapshot over the index it took). Each
  // gives the pages their phase, whatever the last ask came to (landed,
  // its snapshot released here; refused, unread, capped; or one waiting behind
  // a console ask): No card (16), or Idle with the stats off (1); a release
  // moves the version (freeSnap()). (A remount's read of stats.bin to make
  // isn't the stats off: an S3a ask waits for it as for the boot's scan,
  // gateOf().)
  const bool cardGone = g.cardHeld || g.cardAway;
  const bool off = !glue_->on() && !glue_->rereadPending();
  if (off || cardGone) {
    const Outcome o = cardGone ? Outcome::CardAway : Outcome::Cancelled;
    if (visiting() && state_.load() != kIdle) {
      stop(o, cardGone ? "the card away or held: dropped" : "the stats are off this session: dropped");
    }
    if (uiWant_ || outcome_ != Outcome::None || (flags_ & kCapped) || snap_) {
      uiWant_ = 0;
      outcome_ = o;  // (an ask still on the card worker: the same, its result dropped at its end)
      flags_ = static_cast<uint8_t>(flags_ & ~kCapped);
    }
    if (snap_) freeSnap(cardGone ? "the card away or held" : "the stats are off this session");
  } else if (snap_ && !(kind_ == kVisit && state_.load() != kIdle) && g.index && g.index->ready() &&
             snap_->written() && !snap_->matches(g.index->buildStamp(), g.index->trackCount())) {
    // (As at the card's return: No card, the page's Try again. A page's
    // lean ask under way, whose count it would hold, ends with it; a
    // console Hn counts from its walk's bits.)
    if (kind_ == kLean && (flags_ & kFromUi) && state_.load() != kIdle) {
      stop(Outcome::CardAway, "its snapshot's key isn't the library's index any more: dropped");
    } else if (state_.load() == kIdle || !visiting()) {
      outcome_ = Outcome::CardAway;
      flags_ = static_cast<uint8_t>(flags_ & ~kCapped);
    }
    freeSnap("the library's index isn't the snapshot's key (a remount, or another build)");
  }
  uint8_t s = state_.load();
  if (s == kRankQueued || s == kTrackQueued) {
    if (visiting()) {
      // S3a's hold bounds (9.7): a block never queues behind a wait.
      const uint8_t why = gateOf(g);
      const Wait w = lane;
      if (capReached()) {
        capVisit();
      } else if (why || w != Wait::None) {
        char text[112];
        snprintf(text, sizeof(text), "a wait came before the hand-off: %s",
                 why ? gateName(why) : ScanScheduler::waitName(w));
        putBack(text, now);
      } else if (now - queuedMs_ >= kQueuedMs) {
        // (Held in Asked 30 s from here, holding nothing: gateOf()'s
        // kStalled. Nothing else holds it, so its turn would take it again
        // in this pass.)
        putBack("the card worker didn't take it in 30 s (none, or busy all that time): taken again 30 s from now",
                now);
        if (state_.load() == kAsked) flags_ = static_cast<uint8_t>(flags_ | kStalled);
      }
    } else if (now - queuedMs_ >= kQueuedMs) {
      refuse("the card worker didn't take it in 30 s (none, or busy all that time)");
      return;
    }
    s = state_.load();
  }
  uint32_t heldMs = 0;  // (an S3a ask in Asked: its waits' hold to this pass, for the phase's line)
  if (s == kAsked && visiting()) {
    // S3a's asks: each gate that holds it, said once (a bit each), every
    // pass (S2's say theirs at turnWanted()); the Rank lane's waits too (a
    // bit a wait), and the wait's start (phaseOf()'s waitedMs: Working,
    // then Waiting from 3 s) is the last pass nothing held it. Not while
    // stalled (sv::waitStartMoves()): the start is the give-up's until the
    // take, so the pass whose gate the stall's 30 s no longer holds leaves
    // it, and this pass's turn (the same nowMs) takes the ask; moved here,
    // the turn's gateOf() would hold it 30 s more, every 30 s for good.
    const uint8_t why = gateOf(g);
    why_ = why;
    if (why && !(waitLogged_ & why)) {
      waitLogged_ = static_cast<uint8_t>(waitLogged_ | why);
      Serial.printf("[listen] %s waits: %s\n", name(), whyName(why));
    }
    const uint16_t bit = static_cast<uint16_t>(1u << static_cast<unsigned>(lane));
    if (!why && lane != Wait::None && !(rankWaits_ & bit)) {
      rankWaits_ = static_cast<uint16_t>(rankWaits_ | bit);
      Serial.printf("[listen] %s waits: %s (the Rank lane's: no block taken while it holds)\n", name(),
                    ScanScheduler::waitName(lane));
    }
    heldMs = sv::elapsedMs(now, queuedMs_);  // (0 for a start stamped after this pass's now)
    if (sv::waitStartMoves(why, lane, (flags_ & kStalled) != 0)) queuedMs_ = now;
  }
  if (s == kDone) {
    Ask& a = *block_;
    sr::close(*fs_, &a.files);  // (on the loop, where they were opened)
    if (visiting()) {
      // Stopped while on the worker (cancel(), the card): its result
      // dropped; the lit cap reached: given back at the step's end.
      if (outcome_ != Outcome::None) {
        endVisit(outcome_, "its rank's result dropped (stopped while on the card worker)");
      } else if (capReached()) {
        capVisit();
      } else {
        finishVisit(g);
      }
    } else if (rank_) {
      finishRank(g);
    } else {
      finishTrack();
    }
  } else if (s == kWalk && visiting() && capReached()) {
    block_->map.release();
    capVisit();
  }
  // A UI ask that waited behind a console ask (or for a cancelled one's
  // step to end) begins.
  if (state_.load() == kIdle && uiWant_) {
    const Kind k = uiWant_ == 1 ? kVisit : kLean;
    uiWant_ = 0;
    beginAsk(k, kFromUi, now);
  }
  // Each phase change moves the version (the pages' news()), and is said
  // (the device runs read the phase: 3a-1's Waiting under the swipes, 3a-6's
  // while the boot's walk holds it).
  const Phase p = phaseAt(now, lane);
  if (static_cast<uint8_t>(p) != lastPhase_) {
    lastPhase_ = static_cast<uint8_t>(p);
    ++version_;
    sayPhase(p, now, lane, heldMs);
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
                  lists, static_cast<ul>(a.bytes), static_cast<ul>(a.parts[1]), static_cast<ul>(a.parts[2]),
                  static_cast<ul>(a.parts[3]), static_cast<ul>(a.parts[4]));
    giveBack();
    kind_ = kNone;
    state_.store(kIdle);
    return;
  }
  // Hl, Hw: the wanted set from the lists, then the walk. (Its blocks were
  // sized for the index at the turn: one of more albums, or of more folders
  // where it keeps their states, isn't walked.)
  if (!g.index || !g.index->ready() || g.index->albumCount() > a.albumsCap ||
      (a.folders && pathhash::Walk::foldersFor(*g.index) > a.foldersCap)) {
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

// S3a's finishRank() (9.7): the wanted set built in the block's tail from
// every list and sealed (V2's heap sort), statsvisit::write() into the
// snapshot (the copy-out, before the Ranker's room is reused), then the
// folders' states and the album bits at the block's head over the dead
// Ranker, and the walk begun over the u40 set. The lean ask: the walk over
// its u40 set alone. A u40 set that lost a hash to its room counts no Not
// played yet: a short set would show played albums as not played.
void StatsLists::finishVisit(const Gates& g) {
  Ask& a = *block_;
  const sr::Report& r = a.report;
  if (r.outcome != sr::Outcome::Ok) {
    char why[96];
    snprintf(why, sizeof(why), "the rank came to %s on the card worker", sr::outcomeName(r.outcome));
    endVisit(sv::outcomeOf(r.outcome), why);
    return;
  }
  // The index its block was sized for (a remount's may not be): asked again.
  if (!g.index || !g.index->ready() || g.index->albumCount() != a.albumsCap ||
      pathhash::Walk::foldersFor(*g.index) > a.foldersCap || g.index->trackCount() > sv::kMaxTracks) {
    if (++restarts_ > kRestarts) {
      endVisit(Outcome::Cancelled, "the library's index changed before its walk, again and again");
      return;
    }
    putBack("the library's index changed before its walk", g.nowMs);
    return;
  }
  const bool lean = kind_ == kLean;
  uint8_t* b = reinterpret_cast<uint8_t*>(block_);
  const int64_t f0 = esp_timer_get_time();
  a.set = a.y.played && a.played40.lost == 0;
  if (a.y.played && a.played40.lost) {
    Serial.printf("[listen] %s: the u40 set lost %lu played hashes past its room (%lu rows): no Not played yet from "
                  "it (a short set would show played albums as not played)\n",
                  name(), static_cast<ul>(a.played40.lost), static_cast<ul>(a.played40.cap));
    if (lean) {
      endVisit(Outcome::Room, "Not played yet not counted (its u40 set short)");
      return;
    }
  }
  if (!lean) {
    if (!snap_) {
      endVisit(Outcome::Cancelled,
               "its snapshot released meanwhile (the Dance tab came up, or the fence): nothing written");
      return;
    }
    // The wanted set: every list's hashes (the ids the walk maps), in one
    // region of the Ranker's rows (Wanted::rowsCap) at A's tail.
    const sr::Ranker& k = a.ranker;
    a.wanted.region(b + a.y.wantedAt, a.y.wantedRows);
    for (int li = 0; li < sr::kLists; ++li) {
      const sr::List l = static_cast<sr::List>(li);
      for (uint32_t i = 0; i < k.size(l); ++i) a.wanted.add(k.at(l, i).hash);
    }
    const int64_t s0 = esp_timer_get_time();
    a.wanted.seal();
    const int64_t w0 = esp_timer_get_time();
    a.sealUs = static_cast<uint32_t>(w0 - s0);
    const LibraryIndex& idx = *g.index;
    const uint64_t nowMs = boardclock::now();
    uint16_t today = 0;
    sv::Context ctx;
    ctx.grade = a.set ? sv::Grade::Full : sv::Grade::Lists;  // (what it serves: no Not played yet from a short set)
    ctx.buildStamp = idx.buildStamp();
    ctx.trackCount = idx.trackCount();
    ctx.albums = idx.albumCount();
    ctx.time = a.in.hasToday && sf::todayOf(nowMs, &today) ? sf::timeOf(nowMs) : 0;
    ctx.liveRound = a.in.liveRound;  // (prepare()'s Fold::liveRound(): the one the rank marked This session by)
    ctx.sinceDay = glue_->fold().sinceDayNow(nowMs);
    ctx.window = snap_->written() ? snap_->window() : sv::kAllTime;  // (a re-ask keeps the page's window)
    ctx.version = version_;
    // A visit asked again over a served snapshot (a re-ask reuses it):
    // write() unserves it, its rows wanted-set indices until the landing,
    // so the version moves, as at a release (the phase stays Working).
    const bool served = snapshot() != nullptr;
    // (An Hk past its header ends: the block written over is another
    // snapshot's from here, as freeSnap() says for a release.)
    if (hkPart_ >= 2) hkPart_ = kHkCut;
    const bool written = sv::write(a.ranker, a.wanted, r, ctx, snap_, snapshotBytes());
    a.writeUs = static_cast<uint32_t>(esp_timer_get_time() - w0);
    if (served && !snapshot()) ++version_;
    if (!written) {
      // (A backstop: turnVisit() releases a held snapshot short of the
      // index's albums, the albums' check above puts the ask back, and the
      // remount's key check (loop()) releases one over another index.)
      endVisit(Outcome::NoRoom, "its snapshot is short of the library's albums");
      return;
    }
  }
  a.folders = a.y.foldersAt ? b + a.y.foldersAt : nullptr;
  a.bits = a.set ? b + a.y.bitsAt : nullptr;
  a.map.begin(g.index, a.set ? &a.played40 : nullptr, lean ? nullptr : &a.wanted, a.bits, a.folders);
  a.finishUs = static_cast<uint32_t>(esp_timer_get_time() - f0);
  a.litWalk = g.lit;
  a.walkT0 = esp_timer_get_time();
  a.walkLastMs = g.nowMs;
  a.passes = 0;
  a.walkUs = a.walkMaxUs = 0;
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
  ListeningStats::printTrackLook("Hp", a.hash, look, ms);
  Serial.printf("[listen] Hp: read on the card worker in %.1f ms (priority %u; queued %.1f ms before it): %lu base "
                "rows read (a binary search), TRKD's %lu rows passed; its block %lu B given back\n",
                ms, static_cast<unsigned>(a.priority), (a.startUs - a.queuedUs) / 1000.0f, static_cast<ul>(t.reads),
                static_cast<ul>(t.trkdRows), static_cast<ul>(a.bytes));
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::stepMap(const Gates& g) {
  if (state_.load() != kWalk) return;
  if (visiting()) {
    stepVisit(g);
    return;
  }
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
    if ((a.folders && pathhash::Walk::foldersFor(*g.index) > a.foldersCap) || g.index->albumCount() > a.albumsCap) {
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

// S3a's walk (9.7, 7.10): lit, 100 tracks a slice, up to 4 ms a pass, none
// in a pass that handled input; dark, S2's (250 tracks, up to 8 ms); with
// the lit cap on, lit as dark, still none in a pass that handled input.
// Skipping passes for 1 s (a list moving, the card held, the join, input)
// gives the block back (map.release()), the ask in Asked again; an index
// that changes under it, the same (asked again over the new one: the
// snapshot's key is the turn's).
void StatsLists::stepVisit(const Gates& g) {
  Ask& a = *block_;
  const bool capOn = (flags_ & kCapOn) != 0;
  const bool litPace = g.lit && !capOn;
  const bool inputHeld = (g.lit || capOn) && g.input;
  if (g.joinWorking || g.listMoving || g.cardHeld || !g.uiStarted || !g.index || inputHeld) {
    if (g.nowMs - a.walkLastMs >= kWalkSkipMs) {
      a.map.release();
      putBack(g.listMoving  ? "its walk held 1 s by a list moving"
              : g.cardHeld  ? "its walk held 1 s by the card's guard"
              : inputHeld   ? "its walk held 1 s by input"
              : g.joinWorking ? "its walk held 1 s by Auto DJ's join"
                            : "its walk held 1 s (the UI, or the index, not up)",
              g.nowMs);
    }
    return;
  }
  a.walkLastMs = g.nowMs;
  sm::Map::Config c;
  c.foldersPerSlice = c.tracksPerSlice = litPace ? kLitSlice : kDarkSlice;
  a.map.setConfig(c);
  const int64_t t0 = esp_timer_get_time();
  PassClock clock{t0 + (litPace ? kLitWalkPassUs : kWalkPassUs)};
  const sm::Map::Status s = a.map.stepPass(g.index, passTimeLeft, &clock);
  const int64_t us = esp_timer_get_time() - t0;
  ++a.passes;
  a.walkUs += us;
  if (us > a.walkMaxUs) a.walkMaxUs = us;
  if (s == sm::Map::Status::Working) return;
  if (s == sm::Map::Status::Aborted) {
    if (++restarts_ > kRestarts) {
      endVisit(Outcome::Cancelled, "the library's index changed under its walk, again and again");
      return;
    }
    putBack("the library's index changed under its walk", g.nowMs);
    return;
  }
  landVisit(g);
}

// The walk done: resolve() (each row's track id; orphans and collisions
// out) and notPlayed() (the bits, the directory, the count) into the
// snapshot, on the loop, and the block given back in this pass. The lean
// ask's count only now into the held snapshot (its lists served all along);
// the console's with none held, counted from the walk's bits.
void StatsLists::landVisit(const Gates& g) {
  Ask& a = *block_;
  const LibraryIndex& idx = *g.index;
  const bool lean = kind_ == kLean;
  const float walkMs = msSince(a.walkT0);
  uint32_t count = 0;
  bool counted = false;
  const char* note = "";
  if (!lean) {
    if (!snap_) {
      endVisit(Outcome::Cancelled,
               "its snapshot released meanwhile (the Dance tab came up, or the fence): nothing written");
      return;
    }
    const int64_t r0 = esp_timer_get_time();
    sv::resolve(a.wanted, snap_);
    a.resolveUs = static_cast<uint32_t>(esp_timer_get_time() - r0);
    if (a.set) {
      const int64_t n0 = esp_timer_get_time();
      counted = sv::notPlayed(a.map, idx, snap_);
      a.notPlayedUs = static_cast<uint32_t>(esp_timer_get_time() - n0);
      count = snap_->notPlayedCount();
      if (!counted) note = " (the snapshot's key isn't the index's: not written)";
    }
  } else if (snap_ && snap_->resolved() && !snap_->madeUp()) {
    const int64_t n0 = esp_timer_get_time();
    counted = sv::notPlayed(a.map, idx, snap_);
    a.notPlayedUs = static_cast<uint32_t>(esp_timer_get_time() - n0);
    count = snap_->notPlayedCount();
    if (!counted) note = " (the held snapshot's key isn't the index's: its count not written)";
  } else if (!(flags_ & kFromUi)) {
    // (Over a made-up view's, uiSx's, nothing real is written into it: a
    // console Hn's count from the walk's bits, as with none held. A page's
    // lean ask never comes under a view: the pages ask nothing then.)
    const LibraryIndex::Span az = idx.albumsAZ();
    for (uint32_t i = 0; i < az.count; ++i) count += a.map.albumPlayed(az[i]) ? 0 : 1;
    counted = true;
    note = snap_ && snap_->madeUp() ? " (a made-up view's held: counted from the walk's bits, not written)"
                                    : " (no snapshot held: counted from the walk's bits)";
  } else {
    note = " (no snapshot held any more: dropped)";
  }
  const sm::Map::Stats& m = a.map.stats();
  const uint32_t hold = holdMs();
  const uint32_t sum = holdSumMs_ + hold > 0xFFFF ? 0xFFFF : holdSumMs_ + hold;
  char np[96];
  if (counted) {
    snprintf(np, sizeof(np), "%lu albums of %lu%s", static_cast<ul>(count), static_cast<ul>(idx.albumCount()), note);
  } else {
    // (A visit with its u40 set whole that notPlayed() refused: its note.)
    snprintf(np, sizeof(np), "not counted%s",
             lean || a.set ? note
             : a.y.played  ? " (its u40 set short)"
                           : " (Lists: no played set)");
  }
  Serial.printf("[listen] %s: landed (%s; sizeof(Ask) %lu B, its block %lu B): held %lu ms (the screen %s at the "
                "take, %s at the rank, %s at the walk; the visit's holds %lu ms%s); the rank at priority %u: queued "
                "%.1f ms, ranked %.1f ms, walked %.0f ms (%lu tracks in %lu passes, %.1f ms of slices, the longest "
                "%.1f ms; %u restarts; the folders' states %s); on the loop: the turn %lu us (stats.bin's directory "
                "%lu us), the hand-off %lu us (the records' copy %lu us, the opens %lu us), finishRank() %lu us (the "
                "seal %lu us, write %lu us), resolve %lu us, notPlayed %lu us; not played yet: %s\n",
                name(), sv::gradeName(a.y.grade), static_cast<ul>(sizeof(Ask)), static_cast<ul>(a.bytes),
                static_cast<ul>(hold), a.litTake ? "lit" : "dark", a.litRank ? "lit" : "dark",
                a.litWalk ? "lit" : "dark", static_cast<ul>(sum), sum >= kLitCapMs ? ", past the 3 s cap" : "",
                static_cast<unsigned>(a.priority), (a.startUs - a.queuedUs) / 1000.0f,
                (a.endUs - a.startUs) / 1000.0f, walkMs, static_cast<ul>(m.tracks), static_cast<ul>(a.passes),
                a.walkUs / 1000.0f, a.walkMaxUs / 1000.0f, static_cast<unsigned>(restarts_),
                a.folders ? "kept" : "none (whole paths)", static_cast<ul>(a.turnUs), static_cast<ul>(a.lookUs),
                static_cast<ul>(a.handUs), static_cast<ul>(a.copyUs), static_cast<ul>(a.openUs),
                static_cast<ul>(a.finishUs), static_cast<ul>(a.sealUs), static_cast<ul>(a.writeUs),
                static_cast<ul>(a.resolveUs), static_cast<ul>(a.notPlayedUs), np);
  // (The console's visit prints its snapshot, then releases it if it borrowed
  // it: endVisit(). A lean ask's snapshot, Hu's and a page's stay.)
  const bool landed = !lean || counted;
  if (landed) ++version_;
  if (!lean && !(flags_ & kFromUi)) printSnapshot(g, name());
  endVisit(landed ? Outcome::Ok : Outcome::Cancelled, nullptr);
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
                static_cast<ul>(a.bytes));
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
          snprintf(what, sizeof(what), "%lu plays, last %s%s", static_cast<ul>(e.count), t,
                   e.session ? " (this session)" : "");
        } else {
          snprintf(what, sizeof(what), "%lu plays, undated (round %lu%s)", static_cast<ul>(e.count),
                   static_cast<ul>(e.ord), e.session ? ", this session" : "");
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

// ---------------------------------------------------------------------------
// S3a: the stats pages' asks (9.7's API), the hold, the snapshot.
// ---------------------------------------------------------------------------
void StatsLists::beginAsk(Kind k, uint8_t flags, uint32_t nowMs) {
  kind_ = k;
  // (A page's ask over Hu's snapshot takes it over: no longer Hu's, so
  // neither Hu0 nor the fence's re-ask of Hu acts on the page's.)
  const uint8_t kept = (flags & kFromUi) ? kCapOn : static_cast<uint8_t>(kCapOn | kFake);
  flags_ = static_cast<uint8_t>((flags_ & kept) | flags);
  outcome_ = Outcome::None;
  why_ = 0;
  restarts_ = 0;
  waitLogged_ = 0;
  rankWaits_ = 0;
  holdSumMs_ = 0;  // (a new visit: its holds summed from here)
  queuedMs_ = nowMs;
  state_.store(kAsked);
}

bool StatsLists::ask(UiAsk a, bool cardGone) {
  const char* what = a == UiAsk::Visit ? "the visit's ask" : "Not played yet's ask";
  // (A remount's read of stats.bin to make isn't the stats off: the ask
  // waits for it in Asked, as for the boot's scan, gateOf(); a Try again
  // after the card's return isn't dropped.)
  if (!glue_ || (!glue_->on() && !glue_->rereadPending())) {
    why_ = sv::kWhyOff;
    Serial.printf("[listen] %s: dropped: the stats are off this session\n", what);
    return false;
  }
  if (cardGone) {
    release("the card away or held");
    outcome_ = Outcome::CardAway;
    Serial.printf("[listen] %s: dropped: the card away or held (No card; Try again once it is back)\n", what);
    return false;
  }
  if (a == UiAsk::NotPlayed && !snapshot()) {
    Serial.printf("[listen] %s: dropped: no landed snapshot to write its count into (a visit's ask first)\n", what);
    return false;
  }
  const Kind k = a == UiAsk::Visit ? kVisit : kLean;
  const uint8_t want = a == UiAsk::Visit ? 1 : 2;
  const uint8_t s = state_.load();
  if (s != kIdle && !(flags_ & kFromUi)) {
    // One slot (6.8 as decided): behind the console's ask under way.
    // (outcome_ kept: a console ask stopped on the card worker keeps its
    // stop, so its result is still dropped at its step's end; phaseAt()
    // gives Working while uiWant_, and beginAsk() clears it.)
    uiWant_ = want;
    flags_ = static_cast<uint8_t>(flags_ & ~kCapped);
    Serial.printf("[listen] %s: waits behind %s (one ask at a time)\n", what, name());
    return true;
  }
  if (s != kIdle) {
    if (kind_ == k && outcome_ == Outcome::None) return true;  // (the same, under way)
    stop(Outcome::Cancelled, "cancelled: another ask of the stats pages");
    if (state_.load() != kIdle) {
      uiWant_ = want;  // (begun once its rank's step ends)
      return true;
    }
  }
  uiWant_ = 0;
  beginAsk(k, kFromUi, millis());
  Serial.printf("[listen] %s: asked (%s): its turn once S2's gates and the Rank lane's waits clear\n", what,
                k == kVisit ? "Full when Not played yet may open, else Lists" : "the lean ask, then no folders");
  return true;
}

void StatsLists::cancel() {
  uiWant_ = 0;
  if (!(flags_ & kFromUi) || !visiting() || state_.load() == kIdle) return;
  stop(Outcome::Cancelled, "cancelled");
}

void StatsLists::release(const char* why) {
  cancel();
  freeSnap(why);
}

// S3b's made-up views (9.3's uiSx<n>, look only: StatsLists.h). The
// selector, the console and the pages' guards are the Ui's (S12; D16:
// nothing asks while one is on, and its rises write it again); this is the
// write and its release.
bool StatsLists::madeUp(uint8_t n, const LibraryIndex* index) {
  const unsigned view = n;
  if (n == 0) {
    if (snap_ && snap_->madeUp()) freeSnap("uiSx0: the made-up view ended");
    return true;
  }
  const bool ready = index && index->ready();
  statsview::MadeUpView v;
  if (!statsview::madeUpView(n, ready ? index->buildStamp() : 0, ready ? index->trackCount() : 0,
                             ready ? index->albumCount() : 0, &v)) {
    Serial.printf("[listen] uiSx%u: no such made-up view (1-9; 0 the card's own)\n", view);
    return false;
  }
  // A console visit or lean ask under way writes into the held snapshot (its
  // rows, its count) on the loop, and would write into the view's: it ends
  // here, its result dropped, as the Dance tab's coming up ends one (the
  // block's other claimant; stop(): on the card worker, at its step's end,
  // its rank's block alone touched meanwhile). So every false below for 1-9
  // leaves nothing held, as UiHost::statsMadeUp() says, and no view stands
  // on a snapshot that isn't its own.
  if (visiting() && state_.load() != kIdle && !(flags_ & kFromUi)) {
    stop(Outcome::Cancelled, "a made-up view (uiSx) put on: dropped (it would write into the view's snapshot)");
  }
  // Whatever is held released first (a page's ask cancelled; a page's
  // snapshot, Hu's, a console ask's or another view's), so a view's look
  // never stands on a real snapshot: the views with none (2, 5-9) read their
  // phase, not Served. (release()'s own lines.)
  release("a made-up view (uiSx)");
  if (!v.snapshot) return true;
  if (!ready) {
    Serial.printf("[listen] uiSx%u: no made-up snapshot: the library's index isn't ready (behind the update's "
                  "fence)\n",
                  view);
    return false;
  }
  // The Dance tab's block, lent as a visit's (refused while that tab is up,
  // past 18,432 albums, or with no block), written whole as a landed one
  // reads: nothing ranked or read, the live index's key (the key check in
  // loop() keeps it), kFlagMadeUp in its header.
  const size_t need = sv::bytesFor(index->albumCount());
  void* s = statshold::borrow(need);
  if (!s) {
    Serial.printf("[listen] uiSx%u: no block for the made-up snapshot's %lu B: %s\n", view, static_cast<ul>(need),
                  noLoanText(need));
    return false;
  }
  sv::Snapshot* snap = sv::Snapshot::at(s);
  if (!sv::writeMadeUp(v.spec, snap, sv::kBound)) {
    Serial.printf("[listen] uiSx%u: the made-up snapshot refused (a library past 65,534 tracks or 65,535 "
                  "albums)\n",
                  view);
    return false;
  }
  snap_ = snap;
  ++version_;  // (what snapshot() serves changed: the pages' news())
  Serial.printf("[listen] uiSx%u: a made-up snapshot, look only (nothing ranked or read), %lu B in the Dance tab's "
                "block at 0x%08lx, lent: served over the library's index (%lu tracks, %lu albums)\n",
                view, static_cast<ul>(need), static_cast<ul>(reinterpret_cast<uintptr_t>(s)),
                static_cast<ul>(index->trackCount()), static_cast<ul>(index->albumCount()));
  return true;
}

StatsLists::Phase StatsLists::phase() const { return phaseAt(millis(), static_cast<Wait>(rankWait_)); }

StatsLists::Phase StatsLists::phaseAt(uint32_t nowMs, Wait lane) const {
  if (uiWant_) return Phase::Working;  // (behind a console ask, or a stopped one's step)
  const uint8_t s = state_.load();
  const bool capped = (flags_ & kCapped) != 0;
  // (Ready only over a snapshot served: one released since its landing
  // reads Idle, or No card where the card or a remount released it.)
  const bool served = snapshot() != nullptr;
  if (s == kIdle || !visiting()) {
    // None made (or none ended yet): the caller's Idle.
    if (outcome_ == Outcome::None && !capped) return Phase::Idle;
    return sv::phaseOf(0, outcome_, Wait::None, 0, capped, served);
  }
  if (outcome_ != Outcome::None) {
    return sv::phaseOf(0, outcome_, Wait::None, 0, false, served);  // (stopped on the worker)
  }
  if (s != kAsked) return Phase::Working;  // taken, ranking, walking
  return sv::phaseOf(why_, Outcome::None, lane, sv::elapsedMs(nowMs, queuedMs_), false, served);
}

void StatsLists::sayPhase(Phase p, uint32_t nowMs, Wait lane, uint32_t heldMs) const {
  const uint8_t s = state_.load();
  char what[160];
  if (uiWant_) {
    snprintf(what, sizeof(what), "a stats page's ask waits behind %s", name());
  } else if (visiting() && s == kAsked) {
    if (why_ || lane != Wait::None) {
      snprintf(what, sizeof(what), "%s in Asked, held %lu ms by %s%s, holding nothing", name(),
               static_cast<ul>(heldMs), why_ ? whyName(why_) : ScanScheduler::waitName(lane),
               why_ ? "" : " (the Rank lane's)");
    } else {
      snprintf(what, sizeof(what), "%s in Asked, its waits clear after %lu ms held: its turn at the next pass the "
               "write gate lets through", name(), static_cast<ul>(heldMs));
    }
  } else if (visiting() && s != kIdle) {
    snprintf(what, sizeof(what), "%s under way (its block taken: queued, ranking or walking)%s", name(),
             outcome_ != Outcome::None ? "; stopped: its rank's result dropped at its step's end" : "");
  } else {
    snprintf(what, sizeof(what), "the last ask came to %s%s; %s", sv::outcomeName(outcome_),
             (flags_ & kCapped) ? " (capped)" : "",
             snapshot() ? "its snapshot served" : snap_ ? "a snapshot held, not served" : "no snapshot held");
  }
  Serial.printf("[listen] the stats pages' phase: %s (version %u, at %lu ms): %s\n", sv::phaseName(p),
                static_cast<unsigned>(version_), static_cast<ul>(nowMs), what);
}

bool StatsLists::news() {
  const uint8_t v = static_cast<uint8_t>(version_);
  if (v == newsSeen_) return false;
  newsSeen_ = v;
  return true;
}

uint32_t StatsLists::holdMs() const {
  return block_ && block_->takeUs ? static_cast<uint32_t>((esp_timer_get_time() - block_->takeUs) / 1000) : 0;
}

bool StatsLists::capReached() const {
  return (flags_ & kCapOn) && visiting() && holdSumMs_ + holdMs() >= kLitCapMs;
}

void StatsLists::addHold(uint32_t ms) {
  const uint32_t before = holdSumMs_;
  const uint32_t sum = before + ms > 0xFFFF ? 0xFFFF : before + ms;
  holdSumMs_ = static_cast<uint16_t>(sum);
  // A visit whose holds pass kLitCapMs is counted and said (9.7's lit hold).
  if (before < kLitCapMs && sum >= kLitCapMs) {
    if (pastCap_ < 0xFF) ++pastCap_;
    Serial.printf("[listen] %s: the visit's holds %lu ms: past the 3 s cap (%u visit%s this session; the cap %s)\n",
                  name(), static_cast<ul>(sum), static_cast<unsigned>(pastCap_), pastCap_ == 1 ? "" : "s",
                  (flags_ & kCapOn) ? "on" : "off");
  }
}

// (Out of line: its buffers and printf's frame in no caller's.)
__attribute__((noinline)) void StatsLists::sayTake() const {
  if (!block_ || !visiting() || !block_->takeUs) return;
  const Ask& a = *block_;
  const int64_t now = esp_timer_get_time();
  char rank[112], walk[112];
  if (a.endUs) {
    snprintf(rank, sizeof(rank), "the rank at priority %u (the screen %s at its start): queued %.1f ms, ranked %.1f ms",
             static_cast<unsigned>(a.priority), a.litRank ? "lit" : "dark", (a.startUs - a.queuedUs) / 1000.0f,
             (a.endUs - a.startUs) / 1000.0f);
  } else {
    // (Never handed; or handed and its worker's start refused, back in
    // the queue: its priority and screen then the hand-off's.)
    snprintf(rank, sizeof(rank), "not ranked: queued %.1f ms%s", (now - a.queuedUs) / 1000.0f,
             a.handUs ? " (its hand-off made, the worker's start refused)" : "");
  }
  if (a.walkT0) {
    snprintf(walk, sizeof(walk), "walked %.0f ms (the screen %s at its start; %lu passes, %.1f ms of slices, the "
             "longest %.1f ms)",
             (now - a.walkT0) / 1000.0f, a.litWalk ? "lit" : "dark", static_cast<ul>(a.passes), a.walkUs / 1000.0f,
             a.walkMaxUs / 1000.0f);
  } else {
    snprintf(walk, sizeof(walk), "no walk");
  }
  Serial.printf("[listen] %s: the take's figures (not landed): held %lu ms, the screen %s at the take; %s; %s; on "
                "the loop: the turn %lu us (stats.bin's directory %lu us), the hand-off %lu us (the records' copy %lu "
                "us, the opens %lu us), finishRank() %lu us (the seal %lu us, write %lu us)\n",
                name(), static_cast<ul>(holdMs()), a.litTake ? "lit" : "dark", rank, walk, static_cast<ul>(a.turnUs),
                static_cast<ul>(a.lookUs), static_cast<ul>(a.handUs), static_cast<ul>(a.copyUs),
                static_cast<ul>(a.openUs), static_cast<ul>(a.finishUs), static_cast<ul>(a.sealUs),
                static_cast<ul>(a.writeUs));
}

void StatsLists::putBack(const char* why, uint32_t nowMs) {
  const uint32_t hold = holdMs();
  addHold(hold);
  Serial.printf("[listen] %s: its block given back after %lu ms held (%s): back in Asked, no count (the visit's "
                "holds %lu ms)\n",
                name(), static_cast<ul>(hold), why, static_cast<ul>(holdSumMs_));
  sayTake();
  giveBack();
  // (The lit cap's backstop: no ask by itself once its holds reach it.)
  if ((flags_ & kCapOn) && holdSumMs_ >= kLitCapMs) {
    capVisit();
    return;
  }
  waitLogged_ = 0;
  rankWaits_ = 0;
  queuedMs_ = nowMs;
  state_.store(kAsked);
}

void StatsLists::endVisit(Outcome o, const char* why) {
  if (block_) {
    const uint32_t hold = holdMs();
    addHold(hold);
    if (why) {
      Serial.printf("[listen] %s: %s: its block %lu B given back after %lu ms held (the visit's holds %lu ms)\n",
                    name(), why, static_cast<ul>(block_->bytes), static_cast<ul>(hold),
                    static_cast<ul>(holdSumMs_));
      sayTake();  // (a landing's are on its own line: why is null there)
    }
  } else if (why) {
    Serial.printf("[listen] %s: %s\n", name(), why);
  }
  giveBack();
  outcome_ = o;
  // A console ask holds nothing: the snapshot it borrowed released (Hu's kept,
  // as a page's).
  if (!(flags_ & (kFromUi | kFake)) && (flags_ & kCarved) && snap_) freeSnap("a console ask holds nothing");
  kind_ = kNone;
  state_.store(kIdle);
}

void StatsLists::capVisit() {
  const uint32_t sum = holdSumMs_ + holdMs();
  Serial.printf("[listen] %s: CAPPED: the visit's holds reached %lu ms (kLitCapMs %lu, the lit cap on): Capped, "
                "\"Waiting for the card...\" with Try again; nothing asked by itself\n",
                name(), static_cast<ul>(sum > 0xFFFF ? 0xFFFF : sum), static_cast<ul>(kLitCapMs));
  endVisit(Outcome::None, "given back at the lit cap");
  flags_ = static_cast<uint8_t>(flags_ | kCapped);
}

void StatsLists::stop(Outcome o, const char* why) {
  switch (state_.load()) {
    case kIdle: return;
    case kAsked:
    case kRankQueued:
    case kTrackQueued: endVisit(o, why); return;
    case kWalk:
      block_->map.release();
      endVisit(o, why);
      return;
    default:
      // On the card worker: a step is never cut; its result dropped at its
      // end (loop(), kDone).
      if (outcome_ == Outcome::None) {
        outcome_ = o;
        Serial.printf("[listen] %s: %s: its rank's result dropped when its step on the card worker ends\n", name(),
                      why);
      }
      return;
  }
}

uint32_t StatsLists::freeSnap(const char* why) {
  if (!snap_) return 0;
  const size_t n = snapshotBytes();
  void* p = snap_;
  // (A made-up view's, uiSx<n>, released here as any other: the pages'
  // falls, the fence, and loop()'s the stats off, the card, another key.
  // Its view's next rise writes it again: S3b's S12.)
  const bool madeUpOne = snap_->madeUp();
  // (The pointer dropped: the block is the Dance tab's, lent, never the
  // stats' to free.)
  snap_ = nullptr;
  // An Hk past its header ends at its next step: what it would print from
  // here is another snapshot's, if one is written into the block in this
  // call (a made-up view's, uiSx<n> or a rise's: S3b's review, finding 2).
  if (hkPart_ >= 2) hkPart_ = kHkCut;
  flags_ = static_cast<uint8_t>(flags_ & ~(kFake | kCarved));
  // (What snapshot() serves changed: the pages' news(), whether or not the
  // phase moves with it.)
  ++version_;
  const int64_t p0 = esp_timer_get_time();
  Serial.printf("[listen] the stats pages' %ssnapshot released (%lu B at 0x%08lx, the Dance tab's block: nothing "
                "freed): %s\n",
                madeUpOne ? "made-up (uiSx) " : "", static_cast<ul>(n),
                static_cast<ul>(reinterpret_cast<uintptr_t>(p)), why);
  return static_cast<uint32_t>(esp_timer_get_time() - p0);
}

// Hk's first parts, and a console visit's landing: the held snapshot's
// header as a page reads it (its figures; Hk's rows follow, stepHk()).
// `piece`: 0 all; 1 the key and the rows' counts, 2 the 7 days, 3 DAYS'
// span and Not played yet's count (each under kHkPieceRoom).
__attribute__((noinline)) bool StatsLists::printSnapshot(const Gates& g, const char* who, uint8_t piece) {
  if (!snap_) {
    Serial.printf("[listen] %s: no snapshot held\n", who);
    return false;
  }
  const sv::Snapshot& s = *snap_;
  const ul at = static_cast<ul>(reinterpret_cast<uintptr_t>(snap_));
  if (!s.written()) {
    Serial.printf("[listen] %s: the snapshot %lu B at 0x%08lx: not written (its ask under way, or one that didn't "
                  "land)\n",
                  who, static_cast<ul>(snapshotBytes()), at);
    return false;
  }
  if (piece <= 1) {
    const bool mine = g.index && g.index->ready() && s.matches(g.index->buildStamp(), g.index->trackCount());
    char asked[24], today[16], since[16];
    timeText(s.time(), asked, sizeof(asked));
    dayText(s.today(), today, sizeof(today));
    dayText(s.sinceDay(), since, sizeof(since));
    // (A made-up view's says so: its figures are a recipe's, nothing ranked
    // or read, never the card's plays: 9.3's uiSx<n>, S3b's S12. The line
    // stays inside kHkPieceRoom with the label.)
    Serial.printf("[listen] %s: the snapshot %lu B at 0x%08lx (its block %lu B; %s, %s, %s%s): key %016llx, %lu "
                  "tracks, %lu albums%s; asked %s (today %s, the live round %lu); counting since %s; Most played's "
                  "window %s; version %lu\n",
                  who, static_cast<ul>(s.bytes()), at, static_cast<ul>(snapshotBytes()), sv::gradeName(s.grade()),
                  s.dated() ? "dated" : "undated", s.resolved() ? "landed" : "NOT landed",
                  s.madeUp() ? "; MADE UP by uiSx, look only: not the card's plays" : "",
                  static_cast<ull>(s.buildStamp()), static_cast<ul>(s.trackCount()),
                  static_cast<ul>(s.albums()), g.index && !mine ? " (NOT this index's)" : "", asked, today,
                  static_cast<ul>(s.liveRound()), since, windowName(s.window()), static_cast<ul>(s.version()));
    Serial.printf("[listen] %s: rows (of every row offered): most played 7 days %lu (%lu), 30 days %lu (%lu), All "
                  "time %lu (%lu); new finds %lu (%lu); old favourites %lu (%lu); recently played %lu (%lu)\n",
                  who, static_cast<ul>(s.rows(sr::kMost7)), static_cast<ul>(s.total(sr::kMost7)),
                  static_cast<ul>(s.rows(sr::kMost30)), static_cast<ul>(s.total(sr::kMost30)),
                  static_cast<ul>(s.rows(sr::kMostAll)), static_cast<ul>(s.total(sr::kMostAll)),
                  static_cast<ul>(s.rows(sr::kNewFinds)), static_cast<ul>(s.total(sr::kNewFinds)),
                  static_cast<ul>(s.rows(sr::kOldFavourites)), static_cast<ul>(s.total(sr::kOldFavourites)),
                  static_cast<ul>(s.rows(sr::kRecent)), static_cast<ul>(s.total(sr::kRecent)));
  }
  if (piece == 1) return true;
  for (uint32_t i = 0; piece != 3 && i < sr::kWeekDays; ++i) {
    const sr::Day d = s.day(i);
    char day[16], lis[24];
    if (s.dated() && s.today() != sf::kUnsetDay && s.today() >= sr::kWeekDays - 1 - i) {
      dayText(s.today() - (sr::kWeekDays - 1 - i), day, sizeof(day));
    } else {
      snprintf(day, sizeof(day), "day %lu", static_cast<ul>(i + 1));
    }
    listenedText(d.listenedMs, lis, sizeof(lis));
    Serial.printf("[listen] %s: the 7 days, %s: %s, %u plays (Auto DJ's %u, %u skips)\n", who, day, lis,
                  static_cast<unsigned>(d.plays), static_cast<unsigned>(d.autoDjPlays),
                  static_cast<unsigned>(d.autoDjSkips));
  }
  if (piece == 2) return true;
  char first[16], newest[16], rcnt[16];
  dayText(s.firstDay(), first, sizeof(first));
  dayText(s.newestDay(), newest, sizeof(newest));
  dayText(s.rcntFirstDay(), rcnt, sizeof(rcnt));
  Serial.printf("[listen] %s: DAYS' first dated day %s, newest %s (a span of %lu days); RCNT's first %s; not played "
                "yet %s\n",
                who, first, newest, static_cast<ul>(s.spanDays()), rcnt,
                s.hasNotPlayed() ? "" : "not counted (no played set: the Lists grade, or a short set)");
  if (s.hasNotPlayed()) {
    Serial.printf("[listen] %s: not played yet %lu albums of %lu\n", who, static_cast<ul>(s.notPlayedCount()),
                  static_cast<ul>(s.albums()));
  }
  return true;
}

// Hk's next part (hkPart_): the header's three pieces, each list's rows with
// titles (as Hl, while the snapshot is this index's and landed), then Not
// played yet's first albums; each piece, row or album only while the TX
// buffer has room for it (kHkPieceRoom, kHkRowRoom), the rest at later
// passes, so no pass waits on the UART (a list's line and its first row, or
// Not played yet's line and its first album, go together).
__attribute__((noinline)) void StatsLists::stepHk(const Gates& g) {
  const int room = Serial.availableForWrite();
  if (hkPart_ == 1) {
    if (room < kHkPieceRoom) return;
    hkPart_ = printSnapshot(g, "Hk", 1) ? 2 : 0;
    hkRow_ = 0;
    return;
  }
  // (kHkCut: released or written over since the header, another one perhaps
  // written in the same call: a made-up view's, an ask's. S3b's review,
  // finding 2.)
  if (hkPart_ == kHkCut || !snap_ || !snap_->written()) {
    Serial.println("[listen] Hk: the snapshot released or written over, or under an ask's rewrite, since its "
                   "header: the rest not printed");
    hkPart_ = 0;
    return;
  }
  if (hkPart_ <= 3) {
    if (room < kHkPieceRoom) return;
    printSnapshot(g, "Hk", hkPart_);
    ++hkPart_;
    return;
  }
  const sv::Snapshot& s = *snap_;
  const bool mine = g.index && g.index->ready() && s.matches(g.index->buildStamp(), g.index->trackCount());
  const LibraryIndex* idx = mine && s.resolved() ? g.index : nullptr;
  if (hkPart_ < 4 + sr::kLists) {
    const sr::List l = static_cast<sr::List>(hkPart_ - 4);
    const uint32_t rows = s.rows(l);
    if (hkRow_ == 0) {
      if (room < 2 * kHkRowRoom) return;
      Serial.printf("  %s: %lu rows%s\n", listText(l), static_cast<ul>(rows),
                    idx ? "" : " (no titles: not landed, or not this index's)");
    }
    uint32_t i = hkRow_;
    for (; i < rows && (i == 0 || Serial.availableForWrite() >= kHkRowRoom); ++i) {
      const sv::Row r = s.rowAt(l, i);
      char what[48];
      char t[24];
      switch (l) {
        case sr::kNewFinds:
          snprintf(what, sizeof(what), "%u plays (%s)", static_cast<unsigned>(r.count), srcName(r.src));
          break;
        case sr::kOldFavourites:
          dayText(r.lastDay, t, sizeof(t));
          snprintf(what, sizeof(what), "%u plays, last %s", static_cast<unsigned>(r.count), t);
          break;
        case sr::kRecent:
          whenText(r.when, t, sizeof(t));
          snprintf(what, sizeof(what), "%s", t);
          break;
        default: snprintf(what, sizeof(what), "%u plays", static_cast<unsigned>(r.count)); break;
      }
      uint8_t len = 0;
      const char* title = idx && r.id < idx->trackCount() ? idx->trackTitle(r.id, &len) : "";
      Serial.printf("  %3lu. #%-5u %s  %.*s\n", static_cast<ul>(i + 1), static_cast<unsigned>(r.id), what,
                    static_cast<int>(len), title);
    }
    // (A list's rows are its cap and 16 at most, 216: a u8.)
    hkRow_ = i < rows ? static_cast<uint8_t>(i) : 0;
    if (i >= rows) ++hkPart_;
    return;
  }
  // Not played yet: its line, then its first 20 albums (hkRow_ the next).
  const uint32_t n = s.hasNotPlayed() ? s.notPlayedCount() : 0;
  const uint32_t shown = n < 20 ? n : 20;
  if (hkRow_ == 0) {
    if (!s.hasNotPlayed()) {
      hkPart_ = 0;
      return;
    }
    if (room < 2 * kHkRowRoom) return;
    Serial.printf("  not played yet: %lu albums, the first %lu\n", static_cast<ul>(n), static_cast<ul>(shown));
  }
  uint32_t k = hkRow_;
  if (idx) {
    const LibraryIndex::Span az = idx->albumsAZ();
    for (; k < shown && (k == 0 || Serial.availableForWrite() >= kHkRowRoom); ++k) {
      const uint32_t pos = s.albumAt(k);
      if (pos >= az.count) {
        k = shown;
        break;
      }
      Serial.printf("  %3lu. #%lu %.80s\n", static_cast<ul>(k + 1), static_cast<ul>(az[pos]),
                    idx->albumName(az[pos]));
    }
  } else {
    k = shown;  // (no titles: not landed, or not this index's)
  }
  if (k >= shown) {
    hkPart_ = 0;
    hkRow_ = 0;
  } else {
    hkRow_ = static_cast<uint8_t>(k);
  }
}

bool StatsLists::waitWorker(uint32_t maxMs) {
  const uint32_t t0 = millis();
  while (state_.load() == kRunning && millis() - t0 < maxMs) delay(5);
  return state_.load() != kRunning;
}

size_t StatsLists::drop() {
  size_t n = 0;
  size_t gave = 0;  // (the block's bytes, given back here)
  if (block_) {
    // (A worker's step under way: its end first, the files it reads closed
    // after; the fence never comes while the block is held, holds() keeping
    // the update waiting: this is the guard.)
    if (!waitWorker(5000)) {
      Serial.printf("[listen] %s: its card worker's step still reading at the library update's fence: its block "
                    "held through it\n",
                    name());
    } else {
      n = block_->bytes;
      if (visiting()) {
        block_->map.release();
        addHold(holdMs());
        Serial.printf("[listen] %s: its block given back at the library update's fence (%lu B)\n", name(),
                      static_cast<ul>(n));
        sayTake();
      }  // (S2's asks: S2's one line, below.)
      giveBack();
      gave = n;
    }
  }
  // The stats pages' snapshot (9.7's release points: the fence), released (its
  // ids are the old index's): its heap bytes in the fence's line, as the
  // memory check's alsoFreed counted them, which are none (the Dance tab's
  // block, lent).
  const bool fake = (flags_ & kFake) || (kind_ == kVisit && (flags_ & kHu));  // (Hu's snapshot, or its ask)
  if (snap_) {
    n += snapshotHeapBytes();
    freeSnap("the library update's fence");
  }
  if (block_) return n;  // (still read by the worker: the ask as it is)
  uiWant_ = 0;
  if (visiting() && (flags_ & kFromUi)) {
    // (The fence takes the Library tab back to Artists: the pages are left.)
    if (state_.load() != kIdle) endVisit(Outcome::Cancelled, "cancelled at the library update's fence");
  } else if (visiting()) {
    // The console's S3a asks: begun again after the Live.
    restarts_ = 0;
    waitLogged_ = 0;
    rankWaits_ = 0;
    flags_ = static_cast<uint8_t>(flags_ & ~kStalled);
    queuedMs_ = millis();
    state_.store(kAsked);
    Serial.printf("[listen] %s: begun again after the library update's fence\n", name());
  } else if (kind_ != kNone && gave != 0) {
    // S2's asks, as S2 had them: one that held its block begun again after
    // the Live (one line); one in Asked goes on waiting, nothing said.
    restarts_ = 0;
    waitLogged_ = 0;
    state_.store(kAsked);
    Serial.printf("[listen] %s: its block given back at the library update's fence (%lu B): begun again after it\n",
                  name(), static_cast<ul>(gave));
  }
  if (fake) {
    // Hu's held fake visit: asked again after the Live and Auto DJ's rejoin,
    // at the first pass the gates let through (as a visit made after it).
    if (kind_ == kNone) {
      beginAsk(kVisit, kHu, millis());
      Serial.println("[listen] Hu: its visit asked again after the Live and Auto DJ's rejoin (its snapshot lent "
                     "again at its turn)");
    } else if (!(kind_ == kVisit && (flags_ & kHu))) {
      Serial.printf("[listen] Hu: its visit NOT asked again: %s holds the one slot (Hu again after it)\n", name());
    }
  }
  return n;
}

void StatsLists::ending(uint32_t maxMs) {
  // The snapshot first (the orderly end: 9.7's last release point; the worker
  // never reads it).
  uiWant_ = 0;
  freeSnap("the orderly end");
  // (Still reading after `maxMs`: the end goes on. A rotation's save in the
  // end's flush may then replace stats.bin under its reads, which fail their
  // checks: it only reads, and its result is never taken in.)
  if (!waitWorker(maxMs)) return;
  giveBack();
  kind_ = kNone;
  state_.store(kIdle);
}
