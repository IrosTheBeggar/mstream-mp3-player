// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/ListeningStats.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstring>

#include "AutoDjJoin.h"
#include "LibraryUpdate.h"
#include "WallClock.h"
#include "app/BoardClock.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"

// The resident set (docs/LISTENING-STATS.md 5.14): the glue's object alone,
// carved once in setup and never freed. A budget, not a figure the heap
// model pins: R1's yardstick (test_autodj_join) can't tell up to about 4 KB
// from main's own variation; past it, the device re-run decides.
static_assert(sizeof(statsglue::Glue) <= 3584, "the listening stats' resident set is at most 3,584 B (5.14)");

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

size_t psramLargest() { return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM); }
size_t psramFreeBytes() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

// A block the stats hold across passes (the boot's scan's ranges' block, its
// overflow's chunks): at the top of PSRAM's largest free block, the join's
// spacers under it while it is carved, never less than this free meanwhile
// (Auto DJ's kJoinMarginBytes, for the same reason: another task's block may
// come in that window, a track start's 4 KB probe, a seek's 8 KB, the Opus
// packet buffer's 60 KB, the card worker's 4-16 KB). Carved there, a block
// moves no long-lived block carved after it (5.14: the device's round 2).
constexpr size_t kTopMarginBytes = 64 * 1024;
void* topAlloc(size_t bytes) {
  autodj::Spacers sp;
  autodj::takeSpacers(bytes, kTopMarginBytes, psramLargest, psramFreeBytes, psramAlloc, psramFree, &sp);
  void* p = psramAlloc(bytes);
  autodj::giveSpacers(&sp, psramFree);
  return p;
}

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

// A section's type ("TRKD"; 0: stats.bin's frame).
void typeText(uint32_t t, char* out, size_t cap) {
  if (!t) {
    snprintf(out, cap, "its frame");
    return;
  }
  snprintf(out, cap, "%c%c%c%c", static_cast<char>(t), static_cast<char>(t >> 8), static_cast<char>(t >> 16),
           static_cast<char>(t >> 24));
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
    case sg::Refused::Booting: return "the boot's scan (or its save, tried again) is under way";
    case sg::Refused::Fenced: return "the library update's fence is up";
    case sg::Refused::Behind:
      return "this session lacks slots the log holds above the card's MARK (the records full, a save that didn't "
             "land with the boot's overflow, a held stats.tmp lost, or a section damaged): no save this session; the "
             "next boot folds what the log still holds (a rotation meanwhile loses those in plays.1.jnl)";
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
  sg::Memory m;
  m.alloc = psramAlloc;
  m.top = topAlloc;
  m.release = psramFree;
  glue_ = psramNew<sg::Glue>(fs, static_cast<sg::Env&>(*this), m);
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
  waiting_ = p.waiting;
  safePoint_ = LibraryUpdate::safePoint(p.playing, p.waiting, p.trackLeftMs, seekSeen_ ? nowMs - seekAt_ : UINT32_MAX);
  joinWorking_ = p.joinWorking;
  updating_ = p.updating;
}

bool ListeningStats::mayScan(uint32_t slots) {
  if (!glue_) return true;
  switch (glue_->scanWaits(updating_, joinWorking_)) {
    case sg::ScanWait::Update:
      // A library update asked and not over, the boot's fold not begun:
      // neither it nor a remount's read begins behind its memory check or
      // during its build (the plays wait in the Tracker). Once begun, the
      // update waits for the scan's blocks instead (Wait::Stats).
      ++updateWaits_;
      return false;
    case sg::ScanWait::Join:
      // The boot's fold carves only in a pass where Auto DJ's join isn't
      // Working (5.6); so does a remount's read of stats.bin.
      ++scanWaits_;
      return false;
    case sg::ScanWait::None: break;
  }
  if (glue_->wantsRoom() && !glue_->reserve(slots)) ++reserveFailed_;
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
  char more[240] = "";
  if (glue_->partialSeen()) {
    snprintf(more, sizeof(more), "; a log file not read whole: NO save this session, the trim by the MARK read at "
                                 "the boot, the next boot folds again%s",
             f.behind() ? " (its overflow dropped: behind)" : "");
  } else if (f.noMemory()) {
    snprintf(more, sizeof(more), "; the fold ran OUT OF MEMORY: no save this session, the trim by the MARK read at "
                                 "the boot");
  } else if (glue_->retrying()) {
    const uint64_t now = boardclock::monoMs(), at = glue_->retryAtMs();
    snprintf(more, sizeof(more), "; its save didn't land: the overflow held (%lu chunks), tried again in %lu ms (try "
                                 "%lu of %lu), a track's start and a library update waiting meanwhile",
             static_cast<ul>(f.chunks()), static_cast<ul>(at > now ? at - now : 0),
             static_cast<ul>(glue_->tries() + 1), static_cast<ul>(sg::kBootTries));
  } else if (f.behind()) {
    snprintf(more, sizeof(more), "; BEHIND its log (%s): no save this session",
             glue_->lastRefused() != sg::Refused::None ? sg::refusedName(glue_->lastRefused()) : "its save didn't land");
  }
  Serial.printf("[listen] the boot's fold: %lu slots folded (%lu met twice, gap %lu) in the scan's %lu passes (%.0f ms "
                "on the loop%s%s%s); the ranges' block %lu B; the overflow %lu B at the peak (%lu chunks now); "
                "%lu records, MARK %lu rows%s\n",
                static_cast<ul>(f.folded()), static_cast<ul>(f.twice()), static_cast<ul>(f.gap()),
                static_cast<ul>(passes), static_cast<float>(scanUs) / 1000.0f,
                scanWaits_ ? ", some waiting for Auto DJ's join" : "",
                updateWaits_ ? ", its first waiting for a library update" : "",
                reserveFailed_ ? ", some pieces' room not taken first" : "", static_cast<ul>(glue_->rangesBytes()),
                static_cast<ul>(f.peak()), static_cast<ul>(f.chunks()), static_cast<ul>(f.records()),
                static_cast<ul>(f.marks()), more);
}

void ListeningStats::written(const playlog::Slot& s) {
  if (glue_) glue_->written(s);
}

void ListeningStats::ending(uint64_t untilMs) {
  if (glue_) glue_->ending(untilMs);
}

void ListeningStats::runSave(bool fenced) {
  if (!glue_) return;
  const bool retry = glue_->retryDue();
  glue_->saveNow(fenced);  // (its line: saved())
  if (!retry) return;
  if (glue_->retrying()) {
    const uint64_t now = boardclock::monoMs(), at = glue_->retryAtMs();
    Serial.printf("[listen] the boot's overflow held: its save tried again in %lu ms (try %lu of %lu)\n",
                  static_cast<ul>(at > now ? at - now : 0), static_cast<ul>(glue_->tries() + 1),
                  static_cast<ul>(sg::kBootTries));
  } else if (glue_->fold().behind()) {
    Serial.printf("[listen] the boot's overflow given back after %lu tries of its save: the session is BEHIND its "
                  "log (no save this session; the next boot folds what the log still holds, a rotation meanwhile "
                  "loses those in plays.1.jnl)\n",
                  static_cast<ul>(glue_->tries()));
  }
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
  // (A play waiting, for the headphones or the stats' overflow, or a
  // restart asked, is playing for a compaction: its start, or the restart,
  // mustn't wait behind one. Not a safe point either:
  // LibraryUpdate::safePoint().)
  ask->playing = playing_ || waiting_;
  ask->safePoint = safePoint_;
  saveT0_ = esp_timer_get_time();
}

void ListeningStats::began(bool again, const sx::Opened* read) {
  readyLogged_ = false;
  if (!again) return;  // (the boot's read: begin()'s line)
  if (!read) {
    Serial.println("[listen] the card mounted again, and the end flushes: stats.bin NOT read again; the folds "
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
  const float ms = r == sg::Refused::None || rep.what != sx::Saved::Off ? msSince(saveT0_) : 0.0f;
  lastMs_ = ms;
  if (r == sg::Refused::None || rep.what == sx::Saved::Damaged || rep.what == sx::Saved::Behind) {
    logSaved(k, rep, ms);
    return;
  }
  // (No buffer on this frame: it is under the save's caller, the loop's
  // deepest path, docs/LISTENING-STATS.md 11.)
  const char* off = r == sg::Refused::Off ? (glue_ && glue_->dropped() ? " (the folds dropped at the end)" : "") : "";
  Serial.printf("[listen] NOT saved (%s%s): %s%s\n", sg::kindName(k),
                k == sg::Kind::Rotation ? ", before plays.1.jnl leaves the log" : "", refusedText(r), off);
}

// (A frame of its own, never under the store's save: the loop's stack,
// docs/LISTENING-STATS.md 11.)
__attribute__((noinline)) void ListeningStats::logSaved(sg::Kind k, const sx::SaveReport& rep, float ms) {
  char kind[48];
  if (k == sg::Kind::Rotation) {
    snprintf(kind, sizeof(kind), "rotation, before plays.1.jnl leaves the log");
  } else if (k == sg::Kind::Boot && glue_ && glue_->tries() > 1) {
    snprintf(kind, sizeof(kind), "boot, its try %lu of %lu", static_cast<ul>(glue_->tries()),
             static_cast<ul>(sg::kBootTries));
  } else {
    snprintf(kind, sizeof(kind), "%s", sg::kindName(k));
  }
  switch (rep.what) {
    case sx::Saved::Wrote: {
      char base[96] = "";
      if (rep.compacted) {
        snprintf(base, sizeof(base), "; compacted: stats-%08lx.bin, %lu rows, %lu bytes%s",
                 static_cast<ul>(glue_ ? glue_->fold().head().baseGen : 0), static_cast<ul>(rep.baseRows),
                 static_cast<ul>(rep.baseBytes), rep.oldBaseRemoved ? ", the old base removed" : "");
      } else if (rep.compactFailed) {
        snprintf(base, sizeof(base), "; the compaction FAILED (the base as it was: the next boot's save tries again)");
      } else if (rep.compactSkipped) {
        snprintf(base, sizeof(base), "; the compaction waits for the next boot (its name is the one stats.bin names)");
      }
      Serial.printf("[listen] saved (%s): stats.bin generation %lu, %lu bytes (TRKD %lu rows, RCNT %lu), in %.0f ms%s%s; "
                    "%lu slots folded this session\n",
                    kind, static_cast<ul>(rep.generation), static_cast<ul>(rep.bytes),
                    static_cast<ul>(rep.rows[sf::kTrkd]), static_cast<ul>(rep.rows[sf::kRcnt]), ms,
                    rep.heldPromoted ? "; a held stats.tmp promoted first" : "", base,
                    static_cast<ul>(glue_ ? glue_->fold().folded() : 0));
      return;
    }
    case sx::Saved::Nothing:
      Serial.printf("[listen] saved (%s): nothing new since the card's stats.bin (nothing written%s; %.0f ms)\n", kind,
                    rep.heldPromoted ? "; a held stats.tmp promoted" : "", ms);
      return;
    case sx::Saved::Held:
      Serial.printf("[listen] NOT saved (%s): stats.tmp written whole (%lu bytes) but the card refused its rename: "
                    "held, promoted first by the next save (%.0f ms)\n",
                    kind, static_cast<ul>(rep.bytes), ms);
      return;
    case sx::Saved::Failed:
      Serial.printf("[listen] NOT saved (%s): the card failed its %s step (%.0f ms): the card's stats.bin as it was; "
                    "the trim follows its MARK\n",
                    kind, rep.step, ms);
      return;
    case sx::Saved::Changed:
      Serial.printf("[listen] NOT saved (%s): the card's stats.bin isn't the one this session read or wrote "
                    "(another player's, or deleted on a computer): no save this session, the next boot reads the "
                    "card's\n",
                    kind);
      return;
    case sx::Saved::NoMemory:
      Serial.printf("[listen] NOT saved (%s): no PSRAM for the save's %u B of work memory\n", kind,
                    static_cast<unsigned>(sx::kWorkBytes));
      return;
    case sx::Saved::Off:
      Serial.printf("[listen] NOT saved (%s): %s\n", kind,
                    glue_ && glue_->fold().noMemory() ? "the fold ran out of memory this session"
                                                      : "the stats are off this session");
      return;
    case sx::Saved::Damaged: {
      char sec[16];
      typeText(rep.section, sec, sizeof(sec));
      Serial.printf("[listen] NOT saved (%s): the card's stats.bin is DAMAGED (%s fails its check under the header "
                    "this session stands on; %.0f ms): the session is behind, no save this session; the next boot reads "
                    "it corrupt (stats.bad) and counts again from what the log still holds\n",
                    kind, sec, ms);
      return;
    }
    case sx::Saved::Behind:
      Serial.printf("[listen] NOT saved (%s): %s%s\n", kind,
                    rep.step[0] ? "the stats.tmp a save held was lost before its promotion: " : "",
                    refusedText(sg::Refused::Behind));
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
      char mark[64] = "";
      if (o.markBytes) snprintf(mark, sizeof(mark), " (MARK's block %lu B, kept)", static_cast<ul>(o.markBytes));
      Serial.printf("[listen] stats.bin read %s: generation %lu, %lu bytes%s%s%s%s%s: %llu plays, %lu MARK rows%s; "
                    "counting goes on above MARK\n",
                    when, static_cast<ul>(o.generation), static_cast<ul>(o.bytes), base, took, reads,
                    settledText(o.settled.what), held, static_cast<ull>(h.plays),
                    static_cast<ul>(glue_->fold().marks()), mark);
      break;
    }
    case sx::Read::Absent:
      Serial.printf("[listen] stats.bin read %s: none on the card%s%s%s%s: a fresh start, every slot on the card is "
                    "folded\n",
                    when, took, settledText(o.settled.what), twins, held);
      return;
    case sx::Read::Corrupt:
      Serial.printf("[listen] stats.bin read %s: CORRUPT (%s, read twice%s): %s; counting starts again from the log\n",
                    when, cc::whyName(o.why), took, o.quarantined ? "renamed stats.bad" : "set aside");
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
      Serial.printf("[listen] stats.bin NOT read %s: no PSRAM for the read%s: the stats are OFF this session (no fold, "
                    "no save, no trim)\n",
                    when, o.marks ? " or MARK's rows past the room" : "");
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
      Serial.println("[listen] Hs: a save at the next pass the write gate lets through, after the plays waiting for "
                     "their slots (written first, so the save holds them)");
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
  char tries[96] = "";
  if (g.retrying()) {
    const uint64_t now = boardclock::monoMs(), at = g.retryAtMs();
    snprintf(tries, sizeof(tries), "the boot's save tried again in %lu ms (try %lu of %lu), its overflow held",
             static_cast<ul>(at > now ? at - now : 0), static_cast<ul>(g.tries() + 1), static_cast<ul>(sg::kBootTries));
  }
  const char* phase = g.dropped()           ? "OFF (the folds dropped at the end)"
                      : g.rereadPending()   ? "the card mounted again: stats.bin read at the scan's next piece"
                      : !g.on()             ? "OFF"
                      : g.bootPending()     ? "the boot's fold begins at the scan's next piece"
                      : f.booting()         ? "folding (the boot's scan)"
                      : g.retrying()        ? tries
                      : g.partialSeen()     ? "on; a log file not read whole: no save this session"
                      : g.store().changed() ? "on; the card's stats.bin changed: no save this session"
                      : f.noMemory()        ? "on; the fold ran out of memory: no save this session"
                      : f.behind()          ? "on; BEHIND its log: no save this session (the next boot folds what the "
                                              "log still holds; a rotation meanwhile loses those in plays.1.jnl)"
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
  if (l.frame) {
    Serial.printf("[listen] stats.bin's rows: MARK %lu, HOUR %lu, DAYS %lu, RCNT %lu, TRKD %lu; the base %s%08lx, %lu "
                  "rows%s\n",
                  static_cast<ul>(l.rows[sf::kMark]), static_cast<ul>(l.rows[sf::kHour]),
                  static_cast<ul>(l.rows[sf::kDays]), static_cast<ul>(l.rows[sf::kRcnt]),
                  static_cast<ul>(l.rows[sf::kTrkd]), h.baseGen ? "stats-" : "none ", static_cast<ul>(h.baseGen),
                  static_cast<ul>(h.baseRows), (h.flags & sf::kFlagBaseLost) ? " (a base was lost once)" : "");
  }
  Serial.printf("[listen] RAM: the object %u B (the records %lu of %lu, the save due from %lu; MARK %lu rows of %lu%s), "
                "the overflow %lu chunks (%lu B, %lu B at the peak), the ranges' block %lu B%s; this session %lu "
                "folded, %lu met twice, gap %lu\n",
                static_cast<unsigned>(sizeof(statsglue::Glue)), static_cast<ul>(f.records()),
                static_cast<ul>(sf::kRecords), static_cast<ul>(sf::kSaveAt), static_cast<ul>(f.marks()),
                static_cast<ul>(f.markCap()), g.store().markBytes() ? ", its block beside it" : "",
                static_cast<ul>(f.chunks()), static_cast<ul>(f.held()), static_cast<ul>(f.peak()),
                static_cast<ul>(g.rangesBytes()), g.rangesHeld() ? " (held)" : "", static_cast<ul>(f.folded()),
                static_cast<ul>(f.twice()), static_cast<ul>(f.gap()));
  if (!g.anySave()) {
    Serial.println("[listen] the last save: none asked yet this session");
    return;
  }
  const sx::SaveReport& r = g.last();
  if (g.lastRefused() != sg::Refused::None && r.what != sx::Saved::Damaged && r.what != sx::Saved::Behind) {
    Serial.printf("[listen] the last save: %s, refused (%s); %lu written this session\n", sg::kindName(g.lastKind()),
                  refusedText(g.lastRefused()), static_cast<ul>(g.savesWritten()));
    return;
  }
  if (r.what == sx::Saved::Nothing) {
    // (Nothing written: the report holds no file. The card's copy RAM
    // stands on is the one that holds it all.)
    const sx::Store::Copy& c = g.store().cardCopy();
    char copy[64];
    if (c.present) {
      snprintf(copy, sizeof(copy), "stats.bin generation %lu holds it all", static_cast<ul>(c.generation));
    } else {
      snprintf(copy, sizeof(copy), "no stats.bin on the card");
    }
    Serial.printf("[listen] the last save: %s, nothing new (nothing written; %s), %.0f ms; %lu written this session\n",
                  sg::kindName(g.lastKind()), copy, lastMs_, static_cast<ul>(g.savesWritten()));
    return;
  }
  char sec[24] = "";
  if (r.what == sx::Saved::Damaged) {
    char t[16];
    typeText(r.section, t, sizeof(t));
    snprintf(sec, sizeof(sec), " (%s)", t);
  }
  Serial.printf("[listen] the last save: %s, %s%s%s%s, %lu bytes, generation %lu, %.0f ms%s; %lu written this session\n",
                sg::kindName(g.lastKind()), sx::savedName(r.what), sec, r.step[0] ? " at " : "", r.step,
                static_cast<ul>(r.bytes), static_cast<ul>(r.generation), lastMs_, r.compacted ? ", compacted" : "",
                static_cast<ul>(g.savesWritten()));
}

void ListeningStats::printDays() {
  uint16_t today = 0;
  if (!sf::todayOf(boardclock::now(), &today)) {
    Serial.println("[listen] Hd: the board's clock is unset (or not believed): no today, so no last 7 days");
    return;
  }
  const int64_t t0 = esp_timer_get_time();
  const sg::DaysLook look = glue_->lookDays(today);
  const float ms = msSince(t0);
  uint32_t plays = 0, skips = 0;
  uint64_t total = 0;
  for (uint32_t i = 0; i < 7; ++i) {
    if (today + i < 6) continue;  // (before 2026-01-01)
    const sg::DaysLook::Day& e = look.d[i];
    char date[16];
    dayText(e.day, date, sizeof(date));
    if (!e.any) {
      Serial.printf("[listen] %s (day %u): nothing%s\n", date, static_cast<unsigned>(e.day),
                    e.hours ? " (HOUR has rows!)" : "");
      continue;
    }
    const uint8_t* row = e.row;
    const uint16_t dp = cc::get16(row + 2), ds = cc::get16(row + 4);
    const uint8_t djp = row[6], djs = row[7], shp = row[12], shs = row[13];
    const uint32_t dms = cc::get32(row + 8);
    char lis[32];
    listened(dms, lis, sizeof(lis));
    Serial.printf("[listen] %s (day %u): %u plays, %u skips, %s (%lu ms); you %u/%u, shuffle %u/%u, Auto DJ %u/%u; "
                  "HOUR: %lu plays in %lu hours, %llu ms\n",
                  date, static_cast<unsigned>(e.day), static_cast<unsigned>(dp), static_cast<unsigned>(ds), lis,
                  static_cast<ul>(dms), static_cast<unsigned>(dp - djp - shp), static_cast<unsigned>(ds - djs - shs),
                  static_cast<unsigned>(shp), static_cast<unsigned>(shs), static_cast<unsigned>(djp),
                  static_cast<unsigned>(djs), static_cast<ul>(e.hourPlays), static_cast<ul>(e.hours),
                  static_cast<ull>(e.hourMs));
    plays += dp;
    skips += ds;
    total += dms;
  }
  char lis[32];
  listened(total, lis, sizeof(lis));
  Serial.printf("[listen] the last 7 UTC days: %lu plays, %lu skips, %s listened (stats.bin's DAYS and HOUR with the "
                "%lu records%s; %.0f ms)\n",
                static_cast<ul>(plays), static_cast<ul>(skips), lis, static_cast<ul>(glue_->fold().records()),
                look.read ? "" : "; stats.bin NOT read: the records alone", ms);
}

void ListeningStats::printMarks() {
  statsglue::Glue& g = *glue_;
  const sf::Fold& f = g.fold();
  const cc::msmf::Acks& a = g.acks();
  char dev[16];
  // MARK's rows, each beside ACKS's row and the log's highest seq.
  for (uint32_t i = 0; i < f.marks(); ++i) {
    const sf::Fold::MarkRow* m = f.markRow(i);
    if (!m) break;
    devText(m->dev, dev, sizeof(dev));
    char ack[24] = "none", log[40] = "none";
    for (uint32_t k = 0; k < a.count; ++k)
      if (a.entries[k].epoch == m->epoch() && std::memcmp(a.entries[k].deviceId, m->dev, 6) == 0)
        snprintf(ack, sizeof(ack), "%lu", static_cast<ul>(a.entries[k].seq));
    for (uint32_t k = 0; k < g.logKeys(); ++k) {
      const sg::LogKey& lk = g.logKey(k);
      if (lk.epoch == m->epoch() && std::memcmp(lk.dev, m->dev, 6) == 0)
        snprintf(log, sizeof(log), "%lu (%lu slots)", static_cast<ul>(lk.seq), static_cast<ul>(lk.slots));
    }
    Serial.printf("[listen] MARK %s epoch %016llx: %lu (the card's %lu%s); ACKS %s; the log %s\n", dev,
                  static_cast<ull>(m->epoch()), static_cast<ul>(m->seq), static_cast<ul>(m->card),
                  f.heldPending() ? ", a held stats.tmp's at its promotion" : "", ack, log);
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
  Serial.printf("[listen] Hm: MARK %lu rows (room for %lu), ACKS %lu (%lu on the card), the log's %lu keys%s (the boot's "
                "scan); HEAD's twice %lu, gap %lu; this boot %lu met twice, gap %lu\n",
                static_cast<ul>(f.marks()), static_cast<ul>(f.markCap()), static_cast<ul>(a.count),
                static_cast<ul>(a.rows), static_cast<ul>(g.logKeys()), g.logKeysMore() ? " and more" : "",
                static_cast<ul>(f.head().twice), static_cast<ul>(f.head().gap), static_cast<ul>(f.twice()),
                static_cast<ul>(f.gap()));
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
  const char* delta = t.unread ? ", stats.bin's TRKD UNREAD" : t.delta ? ", and in TRKD or the records" : "";
  if (t.row.hash == 0) {
    Serial.printf("[listen] track %016llx: no row (%s%s; %lu base reads, %.1f ms)\n", static_cast<ull>(h), base,
                  t.unread ? delta : ", not in TRKD or the records", static_cast<ul>(t.reads), ms);
    return;
  }
  char first[24], last[24];
  timeText(t.row.firstAt, first, sizeof(first));
  timeText(t.row.lastAt, last, sizeof(last));
  Serial.printf("[listen] track %016llx: %u plays, %u skips; first %s (%s), last %s; lastOrd %lu (%s%s; %lu base reads, "
                "%.1f ms)\n",
                static_cast<ull>(h), static_cast<unsigned>(t.row.plays), static_cast<unsigned>(t.row.skips), first,
                srcName(t.row.src), last, static_cast<ul>(t.row.lastOrd), base, delta, static_cast<ul>(t.reads), ms);
}
