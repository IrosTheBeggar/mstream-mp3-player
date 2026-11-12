// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/PlayHistory.h"

#include <Preferences.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <cstring>

#include "CardContract.h"
#include "NvsLayout.h"
#include "app/AutoDj.h"
#include "app/BoardClock.h"
#include "app/Psram.h"
#include "storage/SectorDisk.h"

namespace {

namespace cc = cardcontract;
using playlog::Journal;
using playlog::Outbox;

// NVS's part (nvslayout's "plays", schema 6): each write one key, at once.
class PrefsNvs final : public Journal::Nvs {
public:
  bool load(Journal::Ids* out) override {
    Preferences p;
    if (!p.begin(nvslayout::kPlaysNamespace, false)) return false;
    nvslayout::PlaysStored s;
    s.haveEpoch = p.isKey(nvslayout::kPlaysEpochKey);
    s.haveSeq = p.isKey(nvslayout::kPlaysSeqKey);
    s.haveBoots = p.isKey(nvslayout::kPlaysBootsKey);
    if (s.haveEpoch) s.epoch = p.getULong64(nvslayout::kPlaysEpochKey, 0);
    if (s.haveSeq) s.seq = p.getUInt(nvslayout::kPlaysSeqKey, 0);
    if (s.haveBoots) s.boots = p.getUInt(nvslayout::kPlaysBootsKey, 0);
    p.end();
    const nvslayout::PlaysIds ids = nvslayout::playsFrom(s);
    out->epoch = ids.epoch;
    out->seq = ids.seq;
    out->boots = ids.boots;
    return true;
  }
  bool storeSeq(uint32_t seq) override { return put(nvslayout::kPlaysSeqKey, seq); }
  bool storeBoots(uint32_t boots) override { return put(nvslayout::kPlaysBootsKey, boots); }
  bool storeEpoch(const Journal::Ids& ids) override {
    Preferences p;
    if (!p.begin(nvslayout::kPlaysNamespace, false)) return false;
    // The seq and the boot count first, the epoch last (a cut between: no
    // epoch, the next boot draws another).
    const bool ok = p.putUInt(nvslayout::kPlaysSeqKey, ids.seq) == sizeof(uint32_t) &&
                    p.putUInt(nvslayout::kPlaysBootsKey, ids.boots) == sizeof(uint32_t) &&
                    p.putULong64(nvslayout::kPlaysEpochKey, ids.epoch) == sizeof(uint64_t);
    p.end();
    return ok;
  }

private:
  static bool put(const char* key, uint32_t v) {
    Preferences p;
    if (!p.begin(nvslayout::kPlaysNamespace, false)) return false;
    const bool ok = p.putUInt(key, v) == sizeof(uint32_t);
    p.end();
    return ok;
  }
};

PrefsNvs prefsNvs;

uint64_t randomEpoch(void*) {
  return static_cast<uint64_t>(esp_random()) << 32 | esp_random();
}

const char* base(const char* p) {
  const char* s = p ? std::strrchr(p, '/') : nullptr;
  return s ? s + 1 : (p ? p : "");
}

const char* outcomeName(uint8_t o) {
  return o == playlog::kCompleted ? "completed" : o == playlog::kSkipped ? "skipped" : "stopped";
}

// The card isn't one to ask now (docs/METADATA.md 3.8): the mount's card is
// out of the slot, none answering or another in it (sectordisk::cardOut(),
// what the player's card hold goes by), or it refuses every write until a
// restart (the guard's writes off). Each ask of a card that is out is a
// mount tried, a second or two of the SD driver's tries with the loop held,
// and a slot's would only fail again.
bool cardAway() {
  return sectordisk::installed() && (sectordisk::cardOut() || sectordisk::guard().writesOff);
}

}  // namespace

void PlayHistory::begin(AutoDj* autoDj) {
  autoDj_ = autoDj;
  if (!library_.records() || !library_.fatfs() || !library_.root()) {
    Serial.println("[plays] no play log: the player's records aren't on a card (the flash, or no card)");
    return;
  }
  buf_ = static_cast<uint8_t*>(psramAlloc(playlog::kReadBytes));
  tracker_ = psramNew<playlog::Tracker>(static_cast<playlog::Tracker::Env&>(*this));
  Journal::Config c;
  c.buf = buf_;
  c.bufBytes = playlog::kReadBytes;
  c.random = randomEpoch;
  c.onOp = onOp;
  c.opCtx = this;
  // The listening stats: stats.bin read (and its chunks loaded) before the
  // journal's begin(), whose scan folds into them (docs/LISTENING-STATS.md
  // 5.4). nullptr: no PSRAM for them, the log as without them.
  c.folds = buf_ && tracker_ ? stats_.begin(*library_.fatfs(), *library_.root()) : nullptr;
  journal_ = buf_ ? psramNew<Journal>(*library_.fatfs(), prefsNvs, c) : nullptr;
  outbox_ = tracker_ && journal_ ? psramNew<Outbox>(*tracker_, *journal_, static_cast<Outbox::Env&>(*this)) : nullptr;
  if (!tracker_ || !journal_ || !outbox_) {
    Serial.println("[plays] no play log: no PSRAM for it");
    return;
  }
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  const cc::msmf::Acks& acks = library_.root()->acks;
  if (!journal_->begin(mac, acks)) {
    Serial.println("[plays] no play log this boot: NVS can't be read or written (\"plays\")");
    return;
  }
  player_.setPlays(tracker_);
  active_ = true;
  // (Library::openRecords() read the root with this board's id: its rows
  // are kept first.)
  char left[64] = "";
  if (acks.rows > acks.count)
    snprintf(left, sizeof(left), "; %lu left out: the plays they name stay",
             static_cast<unsigned long>(acks.rows - acks.count));
  Serial.printf("[plays] the play log: epoch %016llx%s; ACKS %s (%lu entries, %lu of this board's%s); the boot's "
                "scan next\n",
                static_cast<unsigned long long>(journal_->epoch()),
                journal_->newEpoch() ? " (new: NVS had none)" : "",
                acks.present                                   ? "read"
                : library_.root()->readFailed()                ? "unread (the card failed a read of the root)"
                : library_.root()->acksWhy == cc::Why::Missing ? "none"
                : library_.root()->acksWhy == cc::Why::Io      ? "unread (the card failed a read of it)"
                                                               : cc::whyName(library_.root()->acksWhy),
                static_cast<unsigned long>(acks.rows), static_cast<unsigned long>(acks.own), left);
}

uint64_t PlayHistory::monoMs() { return boardclock::monoMs(); }

bool PlayHistory::file(uint32_t key, uint32_t track, playlog::PlayFile* out) {
  if (TrackCatalog::isBuiltin(track)) return false;  // (the built-in tracks aren't logged)
  const TrackCatalog& cat = library_.catalog();
  const size_t n = cat.path(track, out->path, sizeof(out->path));
  static constexpr char kMusic[] = "/music/";
  if (n <= sizeof(kMusic) - 1 || std::strncmp(out->path, kMusic, sizeof(kMusic) - 1) != 0) return false;
  out->pathHash = cc::pathHash(out->path + sizeof(kMusic) - 1);
  const cardroot::Root* root = library_.root();
  out->generation = root && root->hasRoot ? root->generation : 0;
  out->source = autoDj_ && autoDj_->marked(key) ? playlog::kSourceAutoDj
                : player_.shuffle()             ? playlog::kSourceShuffle
                                                : playlog::kSourceManual;
  out->hintMs = cat.durationHintMs(track);
  return true;
}

// The played file as the card has it now: one f_stat, and whose word its
// "no" is (the card read and no such file: the play has no slot; the card
// failing the read: the play waits for another try).
Outbox::Env::File PlayHistory::stat(const char* path, uint32_t* size, uint32_t* fatTime) {
  using Seen = tagstore::Fs::Seen;
  switch (library_.fatfs()->statFile(path, size, fatTime)) {
    case Seen::There: return File::There;
    case Seen::Gone: return File::Gone;
    case Seen::Unknown: break;
  }
  return File::Failed;
}

uint64_t PlayHistory::startedAt(uint64_t startMono, uint8_t* clock) {
  const uint64_t at = boardclock::at(startMono);
  *clock = at ? static_cast<uint8_t>(boardclock::source()) : playlog::kClockUnset;
  return at;
}

void PlayHistory::loop(const Look& look, const playlog::WriteGate::In& gate, bool cardHeld) {
  if (!active_) return;
  tracker_->tick(look.state, look.positionMs, look.positionKnown, look.positionKnown ? look.durationMs : 0);
  // The stats' look at this pass: the update step's safe point (a boot's
  // save compacts only at one), the spare chunk where Auto DJ's join isn't
  // Working.
  ListeningStats::Pass sp;
  sp.playing = look.state == PlayState::Playing;
  sp.waiting = look.state == PlayState::Waiting || look.restartAsked;
  sp.trackLeftMs = gate.trackLeftMs;
  sp.seeks = player_.seeks();
  sp.joinWorking = autoDj_ && autoDj_->joinWorking();
  sp.joinSettled = !autoDj_ || autoDj_->joinSettled();
  sp.updating = library_.update() && library_.update()->asked();
  stats_.pass(sp, gate.nowMs);
  const playlog::WriteGate::Wait w = gate_.update(gate);  // (every pass: it follows the starts)
  // (This pass's verdict, for the library update step's give-back save:
  // statsSaveWaits(), main.cpp's stepCard() after this.)
  gateOpen_ = !cardHeld && w == playlog::WriteGate::Wait::None;
  if (!gateOpen_) return;
  // The stats' blocks again after a library update (docs/LISTENING-STATS.md
  // 6.7): the update over, Auto DJ's rejoin done or none to come, the join
  // not Working; stats.bin read once (not from a card that is away: out of
  // the slot, refusing writes, another card). Nothing else of the card's
  // this pass.
  if (stats_.takeBackDue()) {
    stats_.takeBack();
    return;
  }
  // (Nothing is asked of a card that is away: the scan and the plays wait
  // for the remount, remounted(), or the end's flush(). A journal Off, NVS
  // refusing the boot count, writes no slot: Hs still runs below.)
  if (!journal_->ready() && !journal_->off()) {
    if (cardAway()) return;
    // The fold's room for this piece first; none while Auto DJ's join is
    // Working: the scan waits that pass.
    if (!stats_.mayScan(kScanSlotsPerPass)) return;
    ++scanPasses_;
    const int64_t t0 = esp_timer_get_time();
    const bool done = journal_->scan(kScanSlotsPerPass);
    scanUs_ += esp_timer_get_time() - t0;
    if (!done) return;
  }
  if (!booted_) {
    booted_ = true;
    const Journal::Booted& b = journal_->booted();
    Serial.printf("[plays] %s: boot %lu of this epoch, the next seq %lu; the scan read %lu slots of %lu files (%lu "
                  "passes%s)%s; %lu file%s trimmed (the transfer has its plays)%s%s\n",
                  journal_->off() ? "OFF (NVS didn't take the boot count)" : "ready",
                  static_cast<unsigned long>(journal_->bootSeq()), static_cast<unsigned long>(journal_->lastSeq() + 1),
                  static_cast<unsigned long>(b.slots), static_cast<unsigned long>(b.files),
                  static_cast<unsigned long>(scanPasses_), b.restarts ? "; begun again after a remount" : "",
                  b.cardSeq ? "" : " (none of this epoch's)", static_cast<unsigned long>(b.trimmed),
                  b.trimmed == 1 ? "" : "s",
                  b.twins ? "; a cut rename's twins on the card: a disk check frees them" : "",
                  journal_->unanswered()  ? "; the card failed a question of the log's files (which are there, on "
                                            "which chains): asked again at each play"
                  : journal_->unsettled() ? "; plays.jnl shares plays.1.jnl's chain and the card refused its rename: "
                                            "tried again at each play"
                                          : "");
    stats_.ready(scanPasses_, scanUs_);
    return;
  }
  // One thing of the card's a pass (statsglue::turnOf()). One slot a pass:
  // a play the card failed waits its 5 s (or the remount) at the head, and
  // nothing behind it goes before it. Hs (the console's listening stats
  // save), now, as the real saves are, only at a pass with no slot due: the
  // plays the gate held back are written first, each folded live
  // (written()), so its save holds them (S1's device round 1: Hs at the
  // pass the gate opened saved "nothing new" with 4 plays waiting).
  switch (statsglue::turnOf(slotsDue(), stats_.hsWaiting())) {
    case statsglue::Turn::Slot:
      tryT0_ = esp_timer_get_time();
      outbox_->writeOne();
      break;
    case statsglue::Turn::Hs: stats_.runHs(library_.update() && library_.update()->fenced()); break;
    case statsglue::Turn::Nothing: break;
  }
}

uint32_t PlayHistory::slotsDue() {
  if (!active_ || !journal_->ready() || !tracker_->pending() || cardAway() || !outbox_->due()) return 0;
  return tracker_->pending();
}

// What a try came to (the Outbox's word, the play still first in line).
void PlayHistory::tried(const Outbox::Tried& t, const playlog::Played& p) {
  using Did = Outbox::Did;
  using Why = Outbox::Why;
  const float ms = static_cast<float>(esp_timer_get_time() - tryT0_) / 1000.0f;
  const playlog::Slot& s = t.slot;
  char nth[24] = "";
  if (t.tries > 1) snprintf(nth, sizeof(nth), ", at its try %u", static_cast<unsigned>(t.tries));
  if (t.did == Did::Written) {
    Serial.printf("[plays] seq %lu: %s, %lu:%02lu played, %u pause%s%s (written in %.1f ms%s)\n",
                  static_cast<unsigned long>(s.seq), outcomeName(s.outcome),
                  static_cast<unsigned long>(s.playedMs / 60000), static_cast<unsigned long>(s.playedMs / 1000 % 60),
                  static_cast<unsigned>(s.pauseCount), s.pauseCount == 1 ? "" : "s",
                  s.clock == playlog::kClockUnset ? "; the clock unset" : "", ms, nth);
    tryT0_ = esp_timer_get_time();  // (a flush's next try: its own time)
    return;
  }
  // The card's failures: which question it was.
  const char* card = t.why == Why::Stat          ? "the card failed a read on the way to its file"
                     : journal_->unanswered()    ? "the card failed a question of the log's files"
                     : journal_->unsettled()     ? "the card refused the rename that parts plays.jnl from plays.1.jnl"
                                                 : "the card refused it";
  // (A seq was taken for it, or it is named by its file.)
  char what[96];
  if (t.hasSlot) {
    snprintf(what, sizeof(what), "seq %lu", static_cast<unsigned long>(s.seq));
  } else {
    snprintf(what, sizeof(what), "a play of %.64s", base(p.file.path));
  }
  if (t.did == Did::Kept) {
    Serial.printf("[plays] %s not written yet (%s; %.1f ms): tried again in %lu s or when the card mounts again%s "
                  "(try %u of %u); the plays behind it wait\n",
                  what, card, ms, static_cast<unsigned long>(Outbox::kRetryMs / 1000),
                  t.hasSlot ? ", the same slot" : "", static_cast<unsigned>(t.tries),
                  static_cast<unsigned>(Outbox::kTries));
    tryT0_ = esp_timer_get_time();
    return;
  }
  if (t.gaveUp) {
    Serial.printf("[plays] %s NOT written: the card failed it %u times (the last: %s; %.1f ms): the play is lost\n",
                  what, static_cast<unsigned>(t.tries), card, ms);
    tryT0_ = esp_timer_get_time();
    return;
  }
  if (t.why == Why::Gone) {
    Serial.printf("[plays] %s NOT written: the file isn't there\n", what);
  } else {
    Serial.printf("[plays] %s NOT written (%s; %.1f ms)\n", what,
                  t.why == Why::Nvs       ? "NVS didn't take the seq"
                  : t.why == Why::Blocked ? "blocked: no twin name free, a disk check frees one"
                                          : "no log",
                  ms);
  }
  tryT0_ = esp_timer_get_time();  // (a flush's next try: its own time)
}

void PlayHistory::written(const playlog::Slot& s, const playlog::Played& p) {
  (void)p;
  stats_.written(s);
}

void PlayHistory::flush(const char* why) {
  if (!active_) return;
  player_.endPlay();  // (the open play, stopped)
  const uint32_t waiting = tracker_->pending();
  if (!waiting) return;
  // The listening stats from here: no read at a remount (the folds dropped),
  // no boot's save; a rotation's only with 1 s of the budget left, once the
  // flush starts (5.4, 5.5).
  stats_.ending(0);
  // A card that takes no write, FatFs refusing them all (another card in the
  // slot, writes off), or none in the slot: nothing written. The mount's
  // card out as far as the last mount knew is asked once first (FatFs's
  // next mount: put back, it answers, and the rule and the scan are a
  // remount's); out still, it isn't asked again, a play at a time.
  const char* cant = nullptr;
  if (sectordisk::installed()) {
    if (sectordisk::cardOut() && !sectordisk::guard().foreign) {
      library_.fatfs()->echoNow();
      if (!sectordisk::cardOut()) remounted();
    }
    const sectordisk::Guard g = sectordisk::guard();
    cant = g.foreign               ? "another card is in the slot"
           : sectordisk::cardOut() ? "no card answers"
           : g.writesOff           ? "the card takes no more writes this session"
                                   : nullptr;
  }
  if (cant) {
    Serial.printf("[plays] before the %s: %lu play%s NOT written (%s)\n", why, static_cast<unsigned long>(waiting),
                  waiting == 1 ? "" : "s", cant);
    return;
  }
  // Each play once, in order, to the first the card fails or the deadline:
  // the end doesn't wait on a card gone bad.
  const int64_t t0 = esp_timer_get_time();
  tryT0_ = t0;
  stats_.ending(monoMs() + kFlushBudgetMs);  // (the Outbox's own deadline: its monoMs() is this)
  const Outbox::Flushed f = outbox_->flush(kFlushBudgetMs, kScanSlotsPerPass);
  const float ms = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
  char left[160] = "";
  if (f.left) {
    snprintf(left, sizeof(left), "; %lu NOT written: %s", static_cast<unsigned long>(f.left),
             f.stop == Outbox::Flushed::Stop::Card
                 ? "the card failed one, the rest weren't asked"
                 : !journal_->ready() ? "the boot's scan wasn't done in the flush's time"
                                      : "the flush's time ran out (a slow card)");
  }
  char dropped[48] = "";
  if (f.dropped) snprintf(dropped, sizeof(dropped), ", %lu dropped", static_cast<unsigned long>(f.dropped));
  Serial.printf("[plays] before the %s: %lu of %lu play%s written%s in %.0f ms%s\n", why,
                static_cast<unsigned long>(f.written), static_cast<unsigned long>(f.waiting), f.waiting == 1 ? "" : "s",
                dropped, ms, left);
}

void PlayHistory::remounted() {
  if (!active_) return;
  const bool wasReady = journal_->ready();
  journal_->remounted();
  outbox_->remounted();  // (a play the card failed: tried again now, not in its 5 s)
  if (journal_->ready() || journal_->off()) return;
  booted_ = false;  // (its line again at the scan's end)
  Serial.printf("[plays] the card mounted again %s: the boot's scan begins again (what it read may be partial)\n",
                wasReady ? "before this boot's first slot" : "during the boot's scan");
}

void PlayHistory::onOp(void* ctx, playlog::Journal::Op op, const char* a, const char* b, uint64_t n) {
  (void)ctx;
  switch (op) {
    case Journal::Op::Epoch:
      Serial.printf("[plays] a new epoch, %016llx: NVS had none (a first boot, an erase)\n",
                    static_cast<unsigned long long>(n));
      break;
    case Journal::Op::Rename: Serial.printf("[plays] the rotation: %s is now %s\n", base(a), base(b)); break;
    case Journal::Op::Delete:
      Serial.printf("[plays] %s deleted (%s)\n", base(a),
                    std::strcmp(b, "acknowledged") == 0 ? "the transfer has all its plays"
                    : std::strcmp(b, "twin") == 0       ? "a twin no other file shares any more"
                                                         : "the rotation");
      break;
    case Journal::Op::Note:
      Serial.printf("[plays] the rotation dropped %llu play%s no transfer had read: a drop note says so\n",
                    static_cast<unsigned long long>(n), n == 1 ? "" : "s");
      break;
    case Journal::Op::Twin:
      Serial.printf("[plays] %s and plays.1.jnl share a chain (a cut rename): %s renamed %s\n", base(a), base(a),
                    base(b));
      break;
    case Journal::Op::Blocked:
      Serial.printf("[plays] BLOCKED: %s shares a chain and all nine twin names are taken: no play is written "
                    "until a disk check on a computer\n",
                    base(a));
      break;
    case Journal::Op::Refused:
      Serial.printf("[plays] the card refused to rename %s %s (a cut rename's shared chain): no play is written "
                    "until a rename takes (tried again at each play)\n",
                    base(a), base(b));
      break;
    case Journal::Op::Unread:
      Serial.printf("[plays] the rotation couldn't read %s whole in %u tries (a sector the card fails each time): "
                    "it goes all the same, so the log goes on; its drop note counts the %llu play%s it could see\n",
                    base(a), static_cast<unsigned>(Journal::kUnreadTries), static_cast<unsigned long long>(n),
                    n == 1 ? "" : "s");
      break;
    case Journal::Op::Play:
    case Journal::Op::Lost:
      break;  // (the try's line: tried())
  }
}
