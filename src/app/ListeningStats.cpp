// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/ListeningStats.h"

#include <esp_timer.h>

#include <cstring>

#include "LibraryUpdate.h"
#include "WallClock.h"
#include "app/BoardClock.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"

namespace {

namespace cc = cardcontract;
namespace sf = statsfold;
namespace sx = statsfile;
namespace sg = statsglue;

using ul = unsigned long;
using ull = unsigned long long;

// The boot's waits between reads the card failed (nothing else is held up
// yet: setup()).
void bootWait(uint32_t ms) { delay(ms); }

float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }

// "7 h 37 min", "45 min" (4.1's times).
void listened(uint64_t ms, char* out, size_t cap) {
  const uint64_t min = ms / 60000;
  if (min >= 60) {
    snprintf(out, cap, "%llu h %llu min", static_cast<ull>(min / 60), static_cast<ull>(min % 60));
  } else {
    snprintf(out, cap, "%llu min", static_cast<ull>(min));
  }
}

// A UTC day since 2026-01-01 as "2026-11-08".
void dayText(uint32_t day, char* out, size_t cap) {
  const wallclock::Civil c = wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(day) * sf::kDayMs);
  snprintf(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day);
}

// A row's time (seconds since 2026-01-01, plus 1) as "2026-11-08 21:40:00".
void timeText(uint32_t t, char* out, size_t cap) {
  if (t == 0 || t == sf::kNoFirst) {
    snprintf(out, cap, "%s", t == 0 ? "none" : "none (skips only)");
    return;
  }
  wallclock::text(wallclock::toCivil(sf::kMinMs + static_cast<uint64_t>(t - 1) * 1000), out, cap);
}

void devText(const uint8_t d[6], char* out, size_t cap) {
  snprintf(out, cap, "%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5]);
}

const char* srcName(uint8_t s) { return s == sf::kAutoDj ? "Auto DJ" : s == sf::kShuffle ? "shuffle" : "you"; }

const char* settledText(tagstore::Settle s) {
  switch (s) {
    case tagstore::Settle::Clean: return "";
    case tagstore::Settle::Removed: return "; a leftover stats.tmp removed";
    case tagstore::Settle::Promoted: return "; stats.tmp promoted (a cut save's: stats.bin was gone)";
    case tagstore::Settle::Quarantined: return "; stats.tmp shared a chain: renamed to a twin (a disk check frees it)";
    case tagstore::Settle::Failed: return "; stats.tmp couldn't be settled (a remove or a rename the card refused)";
    case tagstore::Settle::Kept: return "; stats.tmp kept (it may be the only copy)";
    case tagstore::Settle::Unread: return "; the card failed a question of stats.tmp";
  }
  return "";
}

const char* refusedText(sg::Refused r) {
  switch (r) {
    case sg::Refused::None: return "";
    case sg::Refused::Off: return "the stats are off this session";
    case sg::Refused::Ending: return "the orderly end flushes (its 3 s are the plays'; the next boot folds again)";
    case sg::Refused::Budget:
      return "under 1 s of the end's flush was left: the plays of the plays.1.jnl this rotation takes out are lost "
             "to the stats";
    case sg::Refused::CardAway: return "the card is away (out of the slot, refusing writes, or another card in it)";
    case sg::Refused::Partial: return "a log file wasn't read whole this session (the next boot folds again)";
    case sg::Refused::Booting: return "the boot's scan is under way";
    case sg::Refused::Fenced: return "the library update's fence is up";
  }
  return "";
}

// (PlayHistory's own: the card isn't one to ask now.)
bool away() {
  if (!sectordisk::installed()) return false;
  const sectordisk::Guard g = sectordisk::guard();
  return sectordisk::cardOut() || g.writesOff || g.foreign;
}

}  // namespace

playlog::Folds* ListeningStats::begin(tagstore::Fs& fs, const cardroot::Root& root) {
  fs_ = &fs;
  hasCommit_ = root.hasRoot && root.commitTime != 0;
  commitSec_ = root.commitTime;
  glue_ = psramNew<sg::Glue>(fs, static_cast<sg::Env&>(*this), psramAlloc, psramFree);
  if (!glue_) {
    Serial.println("[listen] no listening stats this boot: no PSRAM for them (the play log trims as 0.14.0's)");
    return nullptr;
  }
  const int64_t t0 = esp_timer_get_time();
  const sx::Opened& o = glue_->begin(root.acks, bootWait);
  logRead(o, "at the boot", msSince(t0));
  return glue_;
}

// ---------------------------------------------------------------------------
// The passes.
// ---------------------------------------------------------------------------
void ListeningStats::pass(const Pass& p, uint32_t nowMs) {
  if (!glue_) return;
  if (!seeksKnown_ || p.seeks != seeks_) {
    if (seeksKnown_) {
      seekSeen_ = true;
      seekAt_ = nowMs;
    }
    seeksKnown_ = true;
    seeks_ = p.seeks;
  }
  if (seekSeen_ && nowMs - seekAt_ >= LibraryUpdate::kSeekQuietMs) seekSeen_ = false;
  playing_ = p.playing;
  safePoint_ = LibraryUpdate::safePoint(p.playing, p.waiting, p.trackLeftMs, seekSeen_ ? nowMs - seekAt_ : UINT32_MAX);
  // The spare chunk again, where it may be carved (5.6).
  if (!p.joinWorking && !glue_->booting()) glue_->keepSpare();
  joinWorking_ = p.joinWorking;
}

bool ListeningStats::mayScan(uint32_t slots) {
  if (!glue_ || !glue_->on() || !glue_->booting()) return true;
  // The boot's fold carves only in a pass where Auto DJ's join isn't
  // Working: otherwise the scan waits this pass (5.6).
  if (joinWorking_) {
    ++scanWaits_;
    return false;
  }
  if (!glue_->reserve(slots)) ++reserveFailed_;
  return true;
}

void ListeningStats::ready(uint32_t passes, int64_t scanUs) {
  if (!glue_ || readyLogged_) return;
  readyLogged_ = true;
  glue_->ready();
  const sf::Fold& f = glue_->fold();
  if (!glue_->on()) {
    Serial.printf("[listen] the boot's scan done (%lu passes, %.0f ms on the loop): the stats are OFF this session "
                  "(%s): nothing folded, saved or trimmed by them\n",
                  static_cast<ul>(passes), static_cast<float>(scanUs) / 1000.0f,
                  glue_->dropped() ? "the folds dropped at the end" : sx::readName(glue_->opened().read));
    return;
  }
  char more[200] = "";
  if (glue_->partialSeen()) {
    snprintf(more, sizeof(more), "; a log file not read whole: NO save this session, the trim by the MARK read at "
                                 "the boot, the next boot folds again");
  } else if (f.noMemory()) {
    snprintf(more, sizeof(more), "; the fold ran OUT OF MEMORY: no save this session, the trim by the MARK read at "
                                 "the boot");
  }
  Serial.printf("[listen] the boot's fold: %lu slots folded (%lu met twice, gap %lu) in the scan's %lu passes (%.0f ms "
                "on the loop%s%s); RAM %lu B held, %lu B at the peak; MARK %lu rows, TRKD %lu, RCNT %lu%s\n",
                static_cast<ul>(f.folded()), static_cast<ul>(f.twice()), static_cast<ul>(f.gap()),
                static_cast<ul>(passes), static_cast<float>(scanUs) / 1000.0f,
                scanWaits_ ? ", some waiting for Auto DJ's join" : "",
                reserveFailed_ ? ", some pieces' room not taken first" : "", static_cast<ul>(f.held()),
                static_cast<ul>(f.peak()), static_cast<ul>(f.rows(sf::kMark)), static_cast<ul>(f.rows(sf::kTrkd)),
                static_cast<ul>(f.rows(sf::kRcnt)), more);
}

void ListeningStats::written(const playlog::Slot& s) {
  if (glue_) glue_->written(s);
}

void ListeningStats::ending(uint64_t untilMs) {
  if (glue_) glue_->ending(untilMs);
}

void ListeningStats::runHs(bool fenced) {
  hsWaiting_ = false;
  if (glue_) glue_->hs(fenced);  // (its line: saved())
}

// ---------------------------------------------------------------------------
// statsglue::Env
// ---------------------------------------------------------------------------
uint64_t ListeningStats::monoMs() { return boardclock::monoMs(); }

bool ListeningStats::cardAway() { return away(); }

void ListeningStats::asking(sg::Kind k, sx::SaveAsk* ask) {
  (void)k;
  ask->nowMs = boardclock::now();  // (0 while the clock is unset)
  ask->hasCommit = hasCommit_;
  ask->commitSec = commitSec_;
  ask->playing = playing_;
  ask->safePoint = safePoint_;
  saveT0_ = esp_timer_get_time();
}

void ListeningStats::began(bool again, const sx::Opened* read) {
  readyLogged_ = false;
  if (!again) return;  // (the boot's read: begin()'s line)
  if (!read) {
    Serial.println("[listen] the card mounted again while the end flushes: stats.bin NOT read again; the folds "
                   "dropped, nothing more saved this session (the next boot folds again)");
    return;
  }
  logRead(*read, "again, the card mounted again (one read)", 0.0f);
}

void ListeningStats::partialFile() {
  Serial.println("[listen] a log file wasn't read whole (the card failed an open or a read): no save this session; "
                 "the trim by the MARK read at the boot, the next boot folds again");
}

void ListeningStats::saved(sg::Kind k, sg::Refused r, const sx::SaveReport& rep) {
  const char* kind = sg::kindName(k);
  const char* where = k == sg::Kind::Rotation ? ", before plays.1.jnl leaves the log" : "";
  if (r != sg::Refused::None) {
    lastMs_ = 0.0f;
    const char* off = r == sg::Refused::Off ? (glue_ && glue_->dropped() ? " (the folds dropped at the end)" : "") : "";
    Serial.printf("[listen] NOT saved (%s%s): %s%s\n", kind, where, refusedText(r), off);
    return;
  }
  const float ms = msSince(saveT0_);
  lastMs_ = ms;
  switch (rep.what) {
    case sx::Saved::Wrote: {
      char base[120] = "";
      if (rep.compacted) {
        snprintf(base, sizeof(base), "; compacted: stats-%08lx.bin, %lu rows, %lu bytes%s",
                 static_cast<ul>(glue_ ? glue_->fold().head().baseGen : 0), static_cast<ul>(rep.baseRows),
                 static_cast<ul>(rep.baseBytes), rep.oldBaseRemoved ? ", the old base removed" : "");
      } else if (rep.compactFailed) {
        snprintf(base, sizeof(base), "; the compaction FAILED (the base as it was: the next boot's save tries again)");
      } else if (rep.compactSkipped) {
        snprintf(base, sizeof(base), "; the compaction waits for the next boot (its name is the one stats.bin names)");
      }
      Serial.printf("[listen] saved (%s%s): stats.bin generation %lu, %lu bytes, in %.0f ms%s%s; %lu slots folded "
                    "this session\n",
                    kind, where, static_cast<ul>(rep.generation), static_cast<ul>(rep.bytes), ms,
                    rep.heldPromoted ? "; a held stats.tmp promoted first" : "", base,
                    static_cast<ul>(glue_ ? glue_->fold().folded() : 0));
      return;
    }
    case sx::Saved::Nothing:
      Serial.printf("[listen] saved (%s%s): nothing new since the card's stats.bin (nothing written; %.0f ms)\n", kind,
                    where, ms);
      return;
    case sx::Saved::Held:
      Serial.printf("[listen] NOT saved (%s%s): stats.tmp written whole (%lu bytes) but the card refused its rename: "
                    "held, promoted first by the next save (%.0f ms)\n",
                    kind, where, static_cast<ul>(rep.bytes), ms);
      return;
    case sx::Saved::Failed:
      Serial.printf("[listen] NOT saved (%s%s): the card failed its %s step (%.0f ms): the card's stats.bin as it was; "
                    "the trim follows its MARK\n",
                    kind, where, rep.step, ms);
      return;
    case sx::Saved::Changed:
      Serial.printf("[listen] NOT saved (%s%s): the card's stats.bin isn't the one this session read or wrote "
                    "(another player's, or deleted on a computer): no save this session, the next boot reads the "
                    "card's\n",
                    kind, where);
      return;
    case sx::Saved::NoMemory:
      Serial.printf("[listen] NOT saved (%s%s): no PSRAM for the save's %u B of work memory\n", kind, where,
                    static_cast<unsigned>(sx::kWorkBytes));
      return;
    case sx::Saved::Off:
      Serial.printf("[listen] NOT saved (%s%s): %s\n", kind, where,
                    glue_ && glue_->fold().noMemory() ? "the fold ran out of memory this session"
                                                      : "the stats are off this session");
      return;
  }
}

// ---------------------------------------------------------------------------
// The lines.
// ---------------------------------------------------------------------------
void ListeningStats::logRead(const sx::Opened& o, const char* when, float ms) {
  char took[32] = "";
  if (ms > 0.0f) snprintf(took, sizeof(took), " in %.0f ms", ms);
  char reads[48] = "";
  if (o.reads > 1 || o.baseReads > 1)
    snprintf(reads, sizeof(reads), " (%lu reads, the base %lu)", static_cast<ul>(o.reads), static_cast<ul>(o.baseReads));
  char twins[64] = "";
  if (o.settled.twins)
    snprintf(twins, sizeof(twins), "; %lu twin%s left (a disk check frees them)", static_cast<ul>(o.settled.twins),
             o.settled.twins == 1 ? "" : "s");
  const char* held = o.heldPromoted ? "; the stats.tmp this session held promoted first" : "";
  switch (o.read) {
    case sx::Read::Loaded: {
      const sf::Head& h = glue_->fold().head();
      char base[160];
      if (o.baseLost) {
        snprintf(base, sizeof(base), "; its base LOST (gone, or another file): the per-track rows before it are "
                                     "gone, the totals stay");
      } else if (h.baseGen) {
        snprintf(base, sizeof(base), "; its base stats-%08lx.bin, %lu rows, %lu bytes, checked whole%s",
                 static_cast<ul>(h.baseGen), static_cast<ul>(h.baseRows), static_cast<ul>(o.baseBytes),
                 o.removed ? " (an old one removed)" : "");
      } else {
        snprintf(base, sizeof(base), "; no base yet");
      }
      Serial.printf("[listen] stats.bin read %s: generation %lu, %lu bytes%s%s%s%s%s: %llu plays, %lu MARK rows; "
                    "counting goes on above MARK\n",
                    when, static_cast<ul>(o.generation), static_cast<ul>(o.bytes), base, took, reads,
                    settledText(o.settled.what), held, static_cast<ull>(h.plays),
                    static_cast<ul>(glue_->fold().rows(sf::kMark)));
      break;
    }
    case sx::Read::Absent:
      Serial.printf("[listen] stats.bin read %s: none on the card%s%s%s%s: a fresh start, every slot on the card is "
                    "folded\n",
                    when, took, settledText(o.settled.what), twins, held);
      return;
    case sx::Read::Corrupt:
      Serial.printf("[listen] stats.bin read %s: CORRUPT (%s, read twice%s): %s; counting starts again from the log\n",
                    when, cc::whyName(o.why), took,
                    o.quarantined ? "renamed stats.bad" : "set aside");
      return;
    case sx::Read::Newer:
      Serial.printf("[listen] stats.bin read %s: a NEWER stats firmware's (%s%s): left alone; the stats are OFF this "
                    "session (no fold, no save, no trim)\n",
                    when, cc::whyName(o.why), took);
      return;
    case sx::Read::Unread:
      Serial.printf("[listen] stats.bin read %s: UNREAD (%s%s, %s%s%s): the stats are OFF this session, no fold, no "
                    "save, no trim%s\n",
                    when, o.inBase ? "its base: " : "", cc::whyName(o.why),
                    o.why == cc::Why::FatRecords ? "the file system's records damaged" : "the card failed a read",
                    took, reads, std::strcmp(when, "at the boot") == 0 ? " (tried again at a remount)" : "");
      return;
    case sx::Read::NoMemory:
      Serial.printf("[listen] stats.bin NOT read %s: no PSRAM for the fold or the read: the stats are OFF this session "
                    "(no fold, no save, no trim)\n",
                    when);
      return;
  }
  if (o.settled.twins) Serial.printf("[listen] stats.bin's twins:%s\n", twins);
}

// ---------------------------------------------------------------------------
// The console's H.
// ---------------------------------------------------------------------------
void ListeningStats::command(const char* arg) {
  if (!glue_) {
    Serial.println("[listen] no listening stats this boot (no play log, or no PSRAM for them)");
    return;
  }
  switch (arg[0]) {
    case 0: printState(); return;
    case 'd': printDays(); return;
    case 'm': printMarks(); return;
    case 't': printTrack(arg + 1); return;
    case 's':
      if (glue_->booting()) {
        Serial.printf("[listen] Hs refused: %s\n", refusedText(sg::Refused::Booting));
        return;
      }
      hsWaiting_ = true;
      Serial.println("[listen] Hs: a save at the next pass the write gate lets through");
      return;
    default:
      Serial.println("[listen] H: the state; Hd the last 7 UTC days; Hm MARK, ACKS and the log; Ht<16 hex> a track's "
                     "row; Hs a save now");
      return;
  }
}

void ListeningStats::printState() {
  statsglue::Glue& g = *glue_;
  const sf::Fold& f = g.fold();
  const sx::Opened& o = g.opened();
  const char* phase = g.dropped()                 ? "OFF (the folds dropped at the end)"
                      : !g.on()                   ? "OFF"
                      : g.booting()               ? "folding (the boot's scan)"
                      : g.partialSeen()           ? "on; a log file not read whole: no save this session"
                      : g.store().changed()       ? "on; the card's stats.bin changed: no save this session"
                      : f.noMemory()              ? "on; the fold ran out of memory: no save this session"
                                                  : "on";
  Serial.printf("[listen] H: %s; read %s (%s%s), %lu read again since\n", phase, sx::readName(o.read),
                o.inBase ? "its base: " : "", cc::whyName(o.why), static_cast<ul>(g.rereads()));
  const statsglue::Glue::FileLook l = g.lookFile();
  if (l.there && !l.unread) {
    Serial.printf("[listen] stats.bin on the card: generation %lu (the saves), %lu bytes, %s %u.%u; RAM stands on "
                  "%s generation %lu%s\n",
                  static_cast<ul>(l.generation), static_cast<ul>(l.bytes), l.magic == sx::kMagic ? "MPST" : "NOT MPST",
                  static_cast<unsigned>(l.major), static_cast<unsigned>(l.minor),
                  g.store().cardCopy().present ? "its" : "none,", static_cast<ul>(g.store().cardCopy().generation),
                  g.store().held() ? "; a stats.tmp held" : "");
  } else {
    Serial.printf("[listen] stats.bin on the card: %s\n", l.unread ? "unread (the card failed the question)" : "none");
  }
  const sf::Head& h = f.head();
  char lis[32], you[32], shuf[32], dj[32];
  listened(h.listenedMs, lis, sizeof(lis));
  listened(h.src[sf::kYou].listenedMs, you, sizeof(you));
  listened(h.src[sf::kShuffle].listenedMs, shuf, sizeof(shuf));
  listened(h.src[sf::kAutoDj].listenedMs, dj, sizeof(dj));
  Serial.printf("[listen] HEAD: %llu plays, %llu skips, %s listened (%llu ms), %llu slots; you %lu/%lu/%s, shuffle "
                "%lu/%lu/%s, Auto DJ %lu/%lu/%s (plays/skips/time); %lu undated; round %lu; twice %lu, gap %lu; flags "
                "%u\n",
                static_cast<ull>(h.plays), static_cast<ull>(h.skips), lis, static_cast<ull>(h.listenedMs),
                static_cast<ull>(h.slots), static_cast<ul>(h.src[sf::kYou].plays), static_cast<ul>(h.src[sf::kYou].skips),
                you, static_cast<ul>(h.src[sf::kShuffle].plays), static_cast<ul>(h.src[sf::kShuffle].skips), shuf,
                static_cast<ul>(h.src[sf::kAutoDj].plays), static_cast<ul>(h.src[sf::kAutoDj].skips), dj,
                static_cast<ul>(h.undated), static_cast<ul>(h.round), static_cast<ul>(h.twice), static_cast<ul>(h.gap),
                static_cast<unsigned>(h.flags));
  uint16_t today = 0;
  const bool known = sf::todayOf(boardclock::now(), &today);
  char todayText[16] = "unknown";
  if (known) dayText(today, todayText, sizeof(todayText));
  if (h.sinceDay == sf::kUnsetDay) {
    Serial.printf("[listen] counting since: not set yet (a save that knows a date sets it); today %s\n", todayText);
  } else {
    char since[16];
    dayText(h.sinceDay, since, sizeof(since));
    const long days = known ? static_cast<long>(today) - static_cast<long>(h.sinceDay) + 1 : 0;
    Serial.printf("[listen] counting since %s (day %u%s); today %s: %ld counting day%s\n", since,
                  static_cast<unsigned>(h.sinceDay),
                  (h.flags & sf::kFlagSinceCommit) ? ", from the sync's commitTime" : "", todayText,
                  days < 1 ? 1L : days, days == 1 ? "" : "s");
  }
  Serial.printf("[listen] rows: MARK %lu (%lu B), HOUR %lu (%lu B), DAYS %lu (%lu B), RCNT %lu (%lu B), TRKD %lu (%lu "
                "B); the base %s%08lx, %lu rows%s\n",
                static_cast<ul>(f.rows(sf::kMark)), static_cast<ul>(f.rows(sf::kMark) * sf::kStrides[sf::kMark]),
                static_cast<ul>(f.rows(sf::kHour)), static_cast<ul>(f.rows(sf::kHour) * sf::kStrides[sf::kHour]),
                static_cast<ul>(f.rows(sf::kDays)), static_cast<ul>(f.rows(sf::kDays) * sf::kStrides[sf::kDays]),
                static_cast<ul>(f.rows(sf::kRcnt)), static_cast<ul>(f.rows(sf::kRcnt) * sf::kStrides[sf::kRcnt]),
                static_cast<ul>(f.rows(sf::kTrkd)), static_cast<ul>(f.rows(sf::kTrkd) * sf::kStrides[sf::kTrkd]),
                h.baseGen ? "stats-" : "none ", static_cast<ul>(h.baseGen), static_cast<ul>(h.baseRows),
                (h.flags & sf::kFlagBaseLost) ? " (a base was lost once)" : "");
  Serial.printf("[listen] RAM: %lu B held (the fold's blocks; TRKD %lu chunks, RCNT %lu), %lu B at the peak; the spare "
                "chunk %s; this session %lu folded, %lu met twice, gap %lu\n",
                static_cast<ul>(f.held()), static_cast<ul>(f.chunks(sf::kTrkd)), static_cast<ul>(f.chunks(sf::kRcnt)),
                static_cast<ul>(f.peak()), f.spare() ? "held" : "not held", static_cast<ul>(f.folded()),
                static_cast<ul>(f.twice()), static_cast<ul>(f.gap()));
  if (!g.anySave()) {
    Serial.println("[listen] the last save: none asked yet this session");
    return;
  }
  const sx::SaveReport& r = g.last();
  if (g.lastRefused() != sg::Refused::None) {
    Serial.printf("[listen] the last save: %s, refused (%s); %lu written this session\n", sg::kindName(g.lastKind()),
                  refusedText(g.lastRefused()), static_cast<ul>(g.savesWritten()));
    return;
  }
  Serial.printf("[listen] the last save: %s, %s%s%s, %lu bytes, generation %lu, %.0f ms%s; %lu written this session\n",
                sg::kindName(g.lastKind()), sx::savedName(r.what), r.step[0] ? " at " : "", r.step,
                static_cast<ul>(r.bytes), static_cast<ul>(r.generation), lastMs_, r.compacted ? ", compacted" : "",
                static_cast<ul>(g.savesWritten()));
}

void ListeningStats::printDays() {
  statsglue::Glue& g = *glue_;
  uint16_t today = 0;
  if (!sf::todayOf(boardclock::now(), &today)) {
    Serial.println("[listen] Hd: the board's clock is unset (or not believed): no today, so no last 7 days");
    return;
  }
  uint32_t plays = 0, skips = 0;
  uint64_t ms = 0;
  for (int k = 6; k >= 0; --k) {
    if (today < k) continue;
    const uint16_t d = static_cast<uint16_t>(today - k);
    char date[16];
    dayText(d, date, sizeof(date));
    uint8_t row[16];
    uint32_t hourPlays = 0, hours = 0;
    uint64_t hourMs = 0;
    for (uint32_t hr = 0; hr < 24; ++hr) {
      uint8_t p = 0;
      uint32_t m = 0;
      if (g.fold().hour(static_cast<uint32_t>(d) * 24 + hr, &p, &m)) {
        hourPlays += p;
        hourMs += m;
        ++hours;
      }
    }
    if (!g.fold().day(d, row)) {
      Serial.printf("[listen] %s (day %u): nothing%s\n", date, static_cast<unsigned>(d), hours ? " (HOUR has rows!)" : "");
      continue;
    }
    const uint16_t dp = cc::get16(row + 2), ds = cc::get16(row + 4);
    const uint8_t djp = row[6], djs = row[7], shp = row[12], shs = row[13];
    const uint32_t dms = cc::get32(row + 8);
    char lis[32];
    listened(dms, lis, sizeof(lis));
    Serial.printf("[listen] %s (day %u): %u plays, %u skips, %s (%lu ms); you %u/%u, shuffle %u/%u, Auto DJ %u/%u; "
                  "HOUR: %lu plays in %lu hours, %llu ms\n",
                  date, static_cast<unsigned>(d), static_cast<unsigned>(dp), static_cast<unsigned>(ds), lis,
                  static_cast<ul>(dms), static_cast<unsigned>(dp - djp - shp), static_cast<unsigned>(ds - djs - shs),
                  static_cast<unsigned>(shp), static_cast<unsigned>(shs), static_cast<unsigned>(djp),
                  static_cast<unsigned>(djs), static_cast<ul>(hourPlays), static_cast<ul>(hours),
                  static_cast<ull>(hourMs));
    plays += dp;
    skips += ds;
    ms += dms;
  }
  char lis[32];
  listened(ms, lis, sizeof(lis));
  Serial.printf("[listen] the last 7 UTC days: %lu plays, %lu skips, %s listened\n", static_cast<ul>(plays),
                static_cast<ul>(skips), lis);
}

void ListeningStats::printMarks() {
  statsglue::Glue& g = *glue_;
  const sf::Fold& f = g.fold();
  const cc::msmf::Acks& a = g.acks();
  char dev[16];
  // MARK's rows, each beside ACKS's row and the log's highest seq.
  for (uint32_t i = 0; i < f.rows(sf::kMark); ++i) {
    const sf::Fold::MarkRow* m = f.markRow(i);
    if (!m) break;
    devText(m->dev, dev, sizeof(dev));
    char ack[24] = "none", log[40] = "none";
    for (uint32_t k = 0; k < a.count; ++k)
      if (a.entries[k].epoch == m->epoch && std::memcmp(a.entries[k].deviceId, m->dev, 6) == 0)
        snprintf(ack, sizeof(ack), "%lu", static_cast<ul>(a.entries[k].seq));
    for (uint32_t k = 0; k < g.logKeys(); ++k) {
      const sg::LogKey& lk = g.logKey(k);
      if (lk.epoch == m->epoch && std::memcmp(lk.dev, m->dev, 6) == 0)
        snprintf(log, sizeof(log), "%lu (%lu slots)", static_cast<ul>(lk.seq), static_cast<ul>(lk.slots));
    }
    Serial.printf("[listen] MARK %s epoch %016llx: %lu (the card's %lu); ACKS %s; the log %s\n", dev,
                  static_cast<ull>(m->epoch), static_cast<ul>(m->seq), static_cast<ul>(m->card), ack, log);
  }
  // ACKS's rows and the log's keys with no MARK row.
  for (uint32_t k = 0; k < a.count; ++k) {
    if (f.markOf(a.entries[k].deviceId, a.entries[k].epoch)) continue;
    devText(a.entries[k].deviceId, dev, sizeof(dev));
    Serial.printf("[listen] ACKS %s epoch %016llx: %lu; no MARK row\n", dev, static_cast<ull>(a.entries[k].epoch),
                  static_cast<ul>(a.entries[k].seq));
  }
  for (uint32_t k = 0; k < g.logKeys(); ++k) {
    const sg::LogKey& lk = g.logKey(k);
    if (f.markOf(lk.dev, lk.epoch)) continue;
    devText(lk.dev, dev, sizeof(dev));
    Serial.printf("[listen] the log %s epoch %016llx: %lu (%lu slots); no MARK row\n", dev, static_cast<ull>(lk.epoch),
                  static_cast<ul>(lk.seq), static_cast<ul>(lk.slots));
  }
  Serial.printf("[listen] Hm: MARK %lu rows, ACKS %lu (%lu on the card), the log's %lu keys%s (the boot's scan); "
                "HEAD's twice %lu, gap %lu; this boot %lu met twice, gap %lu\n",
                static_cast<ul>(f.rows(sf::kMark)), static_cast<ul>(a.count), static_cast<ul>(a.rows),
                static_cast<ul>(g.logKeys()), g.logKeysMore() ? " and more" : "", static_cast<ul>(f.head().twice),
                static_cast<ul>(f.head().gap), static_cast<ul>(f.twice()), static_cast<ul>(f.gap()));
}

void ListeningStats::printTrack(const char* hex) {
  uint64_t h = 0;
  int n = 0;
  for (; hex[n]; ++n) {
    const char c = hex[n];
    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (v < 0 || n == 16) {
      n = -1;
      break;
    }
    h = h << 4 | static_cast<uint64_t>(v);
  }
  if (n != 16) {
    Serial.println("[listen] Ht<16 hex digits>: a track's path hash (METADATA 2.3.3)");
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  const sg::TrackLook t = glue_->track(h);
  const float ms = msSince(t0);
  const char* base = t.base == sg::TrackLook::Base::None       ? "no base"
                     : t.base == sg::TrackLook::Base::Found    ? "in the base"
                     : t.base == sg::TrackLook::Base::NotThere ? "not in the base"
                                                               : "the base UNREAD";
  if (t.row.hash == 0) {
    Serial.printf("[listen] track %016llx: no row (%s, not in TRKD; %lu base reads, %.1f ms)\n", static_cast<ull>(h),
                  base, static_cast<ul>(t.reads), ms);
    return;
  }
  char first[24], last[24];
  timeText(t.row.firstAt, first, sizeof(first));
  timeText(t.row.lastAt, last, sizeof(last));
  Serial.printf("[listen] track %016llx: %u plays, %u skips; first %s (%s), last %s; lastOrd %lu (%s%s; %lu base reads, "
                "%.1f ms)\n",
                static_cast<ull>(h), static_cast<unsigned>(t.row.plays), static_cast<unsigned>(t.row.skips), first,
                srcName(t.row.src), last, static_cast<ul>(t.row.lastOrd), base, t.delta ? ", and in TRKD" : "",
                static_cast<ul>(t.reads), ms);
}
