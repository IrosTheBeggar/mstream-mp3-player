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

// NVS's part (nvslayout's "plays", schema 5): each write one key, at once.
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
  journal_ = buf_ ? psramNew<Journal>(*library_.fatfs(), prefsNvs, c) : nullptr;
  if (!tracker_ || !journal_) {
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
                acks.present                                  ? "read"
                : library_.root()->acksWhy == cc::Why::Missing ? "none"
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

void PlayHistory::loop(const Look& look, const playlog::WriteGate::In& gate, bool cardHeld) {
  if (!active_) return;
  tracker_->tick(look.state, look.positionMs, look.positionKnown, look.positionKnown ? look.durationMs : 0);
  const playlog::WriteGate::Wait w = gate_.update(gate);  // (every pass: it follows the starts)
  if (cardHeld || w != playlog::WriteGate::Wait::None) return;
  if (!journal_->ready()) {
    if (journal_->off()) return;
    ++scanPasses_;
    if (!journal_->scan(kScanSlotsPerPass)) return;
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
                  journal_->unsettled() ? "; plays.jnl shares plays.1.jnl's chain and the card refused its rename: "
                                          "tried again at each play"
                                        : "");
    return;
  }
  if (tracker_->pending()) writeOne();
}

void PlayHistory::writeOne() {
  const playlog::Played& p = tracker_->oldest();
  playlog::Slot s;
  uint32_t size = 0, fatTime = 0;
  const int64_t t0 = esp_timer_get_time();
  if (!library_.fatfs()->stat(p.file.path, &size, &fatTime)) {
    ++lost_;
    Serial.printf("[plays] a play of %s NOT written: the file isn't there\n", base(p.file.path));
    tracker_->pop();
    return;
  }
  s.outcome = p.outcome;
  s.source = p.file.source;
  s.pauseCount = p.pauses;
  // Its start: the clock now, less the time since it began (a clock set
  // during the play still dates it); 0 while unset.
  s.startedAt = boardclock::at(p.startMono);
  s.clock = s.startedAt ? static_cast<uint8_t>(boardclock::source()) : playlog::kClockUnset;
  s.uptimeMs = static_cast<uint32_t>(p.startMono);
  s.playedMs = p.playedMs;
  s.durationMs = p.durationMs;
  s.pathHash = p.file.pathHash;
  s.size = size;
  s.fatTime = fatTime;
  s.generation = p.file.generation;
  const Journal::Wrote w = journal_->append(s);
  const float ms = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
  tracker_->pop();
  if (w == Journal::Wrote::Ok) {
    ++written_;
    Serial.printf("[plays] seq %lu: %s, %lu:%02lu played, %u pause%s%s (written in %.1f ms)\n",
                  static_cast<unsigned long>(s.seq), outcomeName(s.outcome),
                  static_cast<unsigned long>(s.playedMs / 60000), static_cast<unsigned long>(s.playedMs / 1000 % 60),
                  static_cast<unsigned>(s.pauseCount), s.pauseCount == 1 ? "" : "s",
                  s.clock == playlog::kClockUnset ? "; the clock unset" : "", ms);
    return;
  }
  ++lost_;
  Serial.printf("[plays] seq %lu NOT written (%s; %.1f ms)\n", static_cast<unsigned long>(s.seq),
                w == Journal::Wrote::Nvs       ? "NVS didn't take the seq"
                : w == Journal::Wrote::Blocked ? "blocked: no twin name free, a disk check frees one"
                : w == Journal::Wrote::Off     ? "no log"
                : journal_->unsettled()        ? "the card refused the rename that parts plays.jnl from plays.1.jnl"
                                               : "the card refused it",
                ms);
}

void PlayHistory::flush(const char* why) {
  if (!active_) return;
  player_.endPlay();  // (the open play, stopped)
  const uint32_t waiting = tracker_->pending();
  if (sectordisk::installed() && sectordisk::guard().foreign) {
    if (waiting)
      Serial.printf("[plays] before the %s: %lu play%s NOT written (another card is in the slot)\n", why,
                    static_cast<unsigned long>(waiting), waiting == 1 ? "" : "s");
    return;
  }
  if (!waiting) return;
  const int64_t t0 = esp_timer_get_time();
  while (!journal_->ready() && !journal_->off()) journal_->scan(kScanSlotsPerPass);
  while (tracker_->pending()) writeOne();
  Serial.printf("[plays] before the %s: %lu play%s written in %.0f ms\n", why, static_cast<unsigned long>(waiting),
                waiting == 1 ? "" : "s", static_cast<float>(esp_timer_get_time() - t0) / 1000.0f);
}

void PlayHistory::remounted() {
  if (!active_) return;
  const bool wasReady = journal_->ready();
  journal_->remounted();
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
    case Journal::Op::Play:
    case Journal::Op::Lost:
      break;  // (writeOne()'s line)
  }
}
