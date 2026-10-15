// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "AutoDjEngine.h"
#include "AutoDjTrigger.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueView.h"
#include "SleepTimer.h"  // EntryStart

// Auto DJ's loop step: what the firmware's glue (src/app/AutoDj) does with
// the player, the engine and the trigger every loop pass (the engine
// design's 2.1-2.5, 2.12 and 2.14; its milestone M4). Portable, so the host
// tests drive it with a real PlaybackController over a real index
// (test_autodj_driver); the glue owns the file, the join, NVS, the log and
// the console, and hands this the index as it is each pass (Index).
//
// One pass (step()), behind the library update's fence nothing but the
// trigger's answer (Wait::Fenced: the queue there is the build's), in this
// order:
//   1. A new queue (QueueModel::playEpoch(): every Play, Shuffle all, a
//      Clear, the undo of either, wherever it was asked: the Library, the
//      Queue's Clear, the toast's Undo, the console's qa/qb/qp/qx/qu, the
//      power lab) starts a new session seeded by the new current track
//      (none: an empty queue). No caller of Play or Clear says so itself.
//   2. The current entry. One that changed: the entry before leaves the
//      session's ring when it started and went in its first kQuickSkipMs
//      (by the backend's position) not by its end
//      (PlaybackController::naturalEnds(): a skip, a jump, a removal; the
//      engine's trackLeft(); by the row it had while the maps are away,
//      as after a fence until the join is ready again). One that starts
//      (current, and the player
//      Playing: before the trigger, so the pick it is due makes follows
//      it) steers from now (trackStarted(), D1: Auto DJ's own picks joined
//      when picked). One that plays (the backend on it, its position this
//      entry's: EntryStart, kPlayingMs in) ends the failure pause. A pick
//      of Auto DJ's taken out of the queue before it started leaves the
//      ring too (trackLeft(.., 0, false): a skip at 0:00; it stays in the
//      cooldowns, 2.12).
//   3. A track that failed (PlaybackController::lastFailure()): one of
//      Auto DJ's (a DJ-marked entry) counts towards the failure pause.
//   4. The trigger (autodj::decide()): due, one pick appended
//      (PlaybackController::addFromAutoDj(): no undo of its own; cued at a
//      finished queue), its entry DJ-marked; the undo it dropped, if any,
//      said (Pass::undoDropped: Ui::queueUndoDropped()). Not due, one slot
//      read ahead when one is (not while a list moves or the card is held;
//      kReadAgainMs after one that failed; its run made with it), else a
//      cached slot's run made again for new maps (Engine::runDue(), no
//      read; not while a list moves). A pick that gave no track, or
//      one the queue refused (out of the ring again: Engine::pickRefused()),
//      is tried again after kRetryMs. While the decoder fills the ring from
//      empty (Env::refilling: a start or a skip, until it holds 500 ms) a
//      due pick waits a pass or two and nothing is read ahead: the decode
//      task runs above the loop on its core then, flat out, and a pick
//      timed across it measured the decoder (108 ms on the device,
//      2026-10-13), as a read ahead would wait behind its reads. It isn't
//      owed (a few hundred ms; a gapless join is no refill).
// The DJ marks (2.14) are the keys of the last kMarks entries Auto DJ
// added (RAM only: not across a reboot); across a library rebuild's
// read-back (fresh keys) they follow their positions (MarkCarry).
//
// Switching on starts a session with no seed track (2.1: no pin; the
// first track to start anchors it, or the playing one at the first pick),
// and clears the failure pause; off ends it (what it added stays). After a
// boot (load()) a session starts the same way: the restored queue waits
// for Play. The index can arrive after a session began (the join after a
// boot or a fence, or after "Auto DJ from here" and the empty states'
// button switched Auto DJ on: they don't wait for it): the track that
// plays then steers from then on, and a seed track the session got
// without the maps is Stay on seed's pin (Engine::mapsArrived()).
//
// Not thread-safe: the firmware's loop task owns it.
namespace autodj {

constexpr int kMarks = 64;
using Marks = queueview::KeyRing<kMarks>;
// A track plays (the failure pause is over: 2.3.2's "starts
// successfully") once this much of it played.
constexpr uint32_t kPlayingMs = 500;
// A pick that gave no track, or one the queue refused: tried again after.
constexpr uint32_t kRetryMs = 5000;
// A slot read ahead that failed (the card pulled, a CRC): the next one
// after (a pick still reads what it needs).
constexpr uint32_t kReadAgainMs = 2000;
// The empty states' Auto DJ button before the index is joined (Auto DJ
// off: it joins only while on): library tracks drawn, each looked up in
// the index by its path (mpdj::Table::rowOf(): a binary search of DJPH,
// about 15 small reads), until one has a row.
constexpr uint8_t kSurpriseTries = 8;

// The DJ marks across a library rebuild (2.14): enterFence() notes where
// the marked entries are; after the read-back (fresh keys), the entries at
// those positions are marked when the queue has the size it had (a track
// gone from the card shifts them: then none are).
class MarkCarry {
public:
  void fenceUp(const QueueModel& q, const Marks& marks);
  void remapped(const QueueModel& q, Marks* marks);
  uint32_t noted() const { return n_; }

private:
  uint32_t pos_[kMarks] = {};
  uint32_t n_ = 0;
  uint32_t size_ = 0;
  bool armed_ = false;
};

// A SlotReader that times the reads it forwards (Pick's read µs; the
// console's D).
class TimedReader : public SlotReader {
public:
  using Clock = uint64_t (*)();
  TimedReader(SlotReader* inner, Clock nowUs) : inner_(inner), now_(nowUs) {}
  cardcontract::mpdj::SlotRead read(uint16_t row, uint8_t* bytes, uint32_t* count, cardcontract::Why* why) override;
  uint32_t us = 0;
  uint32_t reads = 0;

private:
  SlotReader* inner_;
  Clock now_;
};

// What the glue knows of the index this pass.
struct Index {
  const Maps* maps = nullptr;   // the join's (Ready); null: picks by track id
  SlotReader* slots = nullptr;  // null with no index
  uint16_t k = 0, selHead = 0;
  uint8_t selFloor = 0;
  bool joining = false;         // the file reopens, or the join runs (the trigger's)
};

// The rest of the pass's inputs.
struct Env {
  uint32_t nowMs = 0;
  uint32_t libraryTracks = 0;  // picks by id draw below it
  // The backend (its track may still be the entry before's: EntryStart).
  bool audioPlaying = false;   // IAudioBackend::isPlaying()
  bool positionKnown = true;   // ... positionKnown()
  uint32_t trackSeq = 0;       // its starts and gapless advances
  uint32_t positionMs = 0;
  // The rest of the firmware (the trigger's).
  bool cardHeld = false;
  bool restartSet = false;
  bool sleepEndOfQueue = false;
  bool listMoving = false;     // no read ahead while a list moves
  // The decoder fills the ring from empty after a start or a skip (below
  // the 500 ms its pacer runs flat out to): a due pick waits, nothing is
  // read ahead (the class comment's step 4).
  bool refilling = false;
};

// Why a session began (the [dj] lines).
enum class Began : uint8_t { None, Boot, On, NewQueue, Console, FromHere, Surprise, IndexChanged };
const char* beganName(Began b);

// What a quick skip changed, and why (the "[dj] a quick skip" line's
// reason: docs/AUTODJ.md 3.2). The row is the maps', else the one they gave
// while the track was current (away after a fence until the join is ready
// again); unknown when they never did.
enum class SkipWhy : uint8_t {
  Steered,    // it was in the ring and left it
  NoRow,      // the index has no row for it: it never steered
  NotAnchor,  // its row wasn't among the anchors (it started before the session): nothing changes
  NotJoined,  // its row unknown: the index joins and wasn't joined while it was current
  NoIndex,    // its row unknown: no index joined (none usable, the join away or short of memory)
};
// "no longer steers", "it has no row in the index: it never steered", "it
// wasn't among the anchors: nothing changes", "the index isn't joined yet:
// it never steered", "no index is joined: it never steered".
const char* skipWhyText(SkipWhy w);

// What one pass did, for the glue's log lines and the UI.
struct Pass {
  TriggerOut trigger;
  Began began = Began::None;  // a session began (Driver::sessionSeed() its seed)
  bool mapsArrived = false;   // the index is joined since the last pass
  // The current entry.
  bool started = false;       // a track started (current, Playing): it steers
  uint32_t startedTrack = kNoTrackId;
  bool startedByAutoDj = false;
  bool playing = false;       // ... and plays (kPlayingMs of it heard): the failure pause over
  bool quickSkip = false;     // the entry before went in its first 30 s, not by its end
  uint32_t leftTrack = kNoTrackId;
  uint16_t leftRow = kNoRow;  // ... its row (kNoRow: none, or unknown before a join: leftWhy says)
  bool leftSteered = false;   // ... and it left the ring (it steered until now; Engine::rowLeft())
  SkipWhy leftWhy = SkipWhy::NoRow;  // ... the line's reason (skipWhyText())
  uint32_t leftMs = 0;
  bool removedPick = false;   // Auto DJ's pick taken out before it played
  uint32_t removedTrack = kNoTrackId;
  // A track that failed.
  bool failed = false;
  bool failedPick = false;    // ... one of Auto DJ's
  bool paused = false;        // ... and Auto DJ is paused by it (the toast, once)
  // The pick.
  bool refillWait = false;    // due, and waits for the decoder's refill (Env::refilling)
  bool picked = false;        // pick() was asked (`pick` says what it gave)
  Pick pick;
  bool added = false;         // ... and appended
  bool cued = false;          // ... and cued (a finished queue)
  uint32_t key = QueueModel::kNone;  // its entry
  bool refused = false;       // no track, or the queue refused it: tried again after kRetryMs
  uint32_t pickUs = 0, readUs = 0;   // the pick's time, and its reads' share of it
  bool undoDropped = false;   // the listener's undo went with it
  // A slot read ahead.
  bool prefetched = false;
  uint16_t prefetchRow = kNoRow;
  SlotResult prefetch = SlotResult::Ok;
  uint32_t prefetchUs = 0;
  // ... or, none due, a cached slot's run made again for the maps
  // (Engine::runDue(): the join ready again since it was made).
  bool runMade = false;
  uint16_t runRow = kNoRow;
};

class Driver {
public:
  using Random = uint32_t (*)();
  using Clock = uint64_t (*)();
  struct Config {
    Engine::AllocFn alloc = nullptr;  // the engine's memory (the firmware's PSRAM); null: malloc
    Engine::FreeFn release = nullptr;
    Random random = nullptr;          // a session's seed (esp_random()); null: a fixed sequence
    Clock nowUs = nullptr;            // the picks' times (esp_timer); null: none
  };

  Driver() = default;
  explicit Driver(const Config& c) : cfg_(c) {}
  // The engine's memory (one block of about 19 KB); false: none (picks by
  // id only).
  bool begin();

  // ---- the settings (saved by the glue) ----
  const Settings& settings() const { return s_; }
  // The saved settings at boot. On: a session begins (no seed track).
  void load(const Settings& s);
  // True when it changed. On: a new session, no seed track and no pin
  // (2.1), the failure pause and a pick's retry cleared; off: the session
  // ends (the tracks it added stay).
  bool setOn(bool on);
  // Anchor (Follow the vibe, Stay on seed): the pin dropped (2.2).
  bool setMode(Mode m);
  // 55-75 by 5 (nvslayout::autoDjFloorValid()); false: unchanged.
  bool setFloor(uint8_t pct);
  bool setMatch(bool tempo, bool key);
  // The next session's seed (the console's Ds; reproducible sessions).
  void setSeed(uint32_t seed);
  bool seedSet() const { return seedSet_; }
  // A new session now (on or off), seeded by `track` (kNoTrackId: none).
  void newSession(Began why, uint32_t track, const Maps* maps);

  // ---- the loop ----
  Pass step(PlaybackController& player, const Index& ix, const Env& env);
  // The console's Dp: one pick appended now, whatever the trigger says
  // (cued at a finished queue). Nothing behind the fence (refused).
  Pass pickNow(PlaybackController& player, const Index& ix, const Env& env);
  // "Auto DJ from here" (`why` FromHere) and the empty states' button
  // (Surprise): `track` played as a one-track Play, Auto DJ on, a new
  // session seeded by it (after the Play: it can't use the seed up). False:
  // the Play was refused (fenced, no memory). *switchedOn: it was off.
  bool playFrom(PlaybackController& player, uint32_t track, Began why, const Maps* maps, bool* switchedOn);
  // The empty states' button's track: a random row that maps to a track
  // (not ok without an index).
  Pick surprise(const Index& ix);
  // ... before the index is joined: up to kSurpriseTries library tracks
  // (below `libraryTracks`) drawn, the first that `hasRow` says has a row
  // (the firmware: its path hash in the index's DJPH); kNoTrackId: none.
  // `*tries` (or nullptr): how many were looked up.
  uint32_t surpriseTrack(uint32_t libraryTracks, bool (*hasRow)(uint32_t track, void* ctx), void* ctx,
                         uint8_t* tries = nullptr);

  // ---- the library update's fence ----
  // enterFence(), before the queue goes: the marks' and the entries'
  // positions noted.
  void fenceUp(const QueueModel& q);
  // leaveFence(), after the read-back: the marks, the current entry and
  // the pick up next under their new keys (the queue the same size; else
  // forgotten), the by-id cooldown cleared (new ids).
  void remapped(const QueueModel& q);

  // ---- what it holds ----
  Engine& engine() { return engine_; }
  const Engine& engine() const { return engine_; }
  const Marks& marks() const { return marks_; }
  const FailurePause& failurePause() const { return failures_; }
  const TriggerOut& lastTrigger() const { return trigger_; }
  bool owed() const { return trigger_.owed; }
  uint32_t sessions() const { return sessions_; }
  uint32_t sessionSeed() const { return sessionSeed_; }
  Began sessionBegan() const { return began_; }
  // Holds begun (Repeat One, the sleep timer, the failure pause), while on.
  uint32_t holds() const { return holds_; }
  uint32_t refusals() const { return refusals_; }
  uint32_t undoDrops() const { return undoDrops_; }
  uint32_t removedPicks() const { return removedPicks_; }
  // Due picks that waited for a refill (once per refill), while on.
  uint32_t refillWaits() const { return refillWaits_; }
  // A pick's retry waits (until `nowMs` reaches it).
  bool retryWaits(uint32_t nowMs) const { return retry_ && static_cast<int32_t>(nowMs - retryAtMs_) < 0; }
  // The last pick (Pass's), for the console.
  bool lastPick(Pass* out) const {
    if (!lastPicked_) return false;
    *out = lastPick_;
    return true;
  }

private:
  struct Watch {
    uint32_t key = QueueModel::kNone;
    uint32_t track = kNoTrackId;
    uint16_t row = kNoRow;  // its row, as the maps last said (they may be away when it leaves)
    bool rowKnown = false;  // ... the maps said it (else it is unknown: never joined while current)
    bool started = false;  // current while Playing: it steers
    bool played = false;   // kPlayingMs of it heard
    bool ended = false;    // it played to its end and is still current (the queue's end, Repeat One)
    uint32_t playedMs = 0;
  };
  uint32_t nextSeed();
  // 32 random bits: the hook's, else the fixed sequence (not the next
  // session's seed: that one stays set).
  uint32_t random32();
  uint64_t now() const { return cfg_.nowUs ? cfg_.nowUs() : 0; }
  TriggerIn triggerIn(const PlaybackController& player, const Index& ix, const Env& env) const;
  void pickInto(PlaybackController& player, const Index& ix, const Env& env, bool cue, Pass* p);
  void watchCurrent(const QueueModel& q);

  Config cfg_;
  Engine engine_;
  Settings s_;
  FailurePause failures_;
  Marks marks_;
  MarkCarry carry_;
  EntryStart entryStart_;
  Watch watch_;
  TriggerOut trigger_;
  Hold lastHold_ = Hold::None;
  bool epochSeen_ = false;
  uint32_t epoch_ = 0;
  uint32_t ends_ = 0;
  uint32_t failuresSeen_ = 0;
  bool mapsSeen_ = false;
  uint32_t pending_ = QueueModel::kNone;  // Auto DJ's pick not played yet: its key
  uint32_t pendingTrack_ = kNoTrackId;
  uint16_t pendingRow_ = kNoRow;          // ... and its row (the maps may be away when it goes)
  bool retry_ = false;
  uint32_t retryAtMs_ = 0;
  bool readWait_ = false;  // a read ahead failed: none until readAtMs_
  uint32_t readAtMs_ = 0;
  // The fence's notes (positions).
  uint32_t fenceSize_ = 0;
  int32_t fenceCurrent_ = -1;
  uint32_t fencePending_ = QueueModel::kNone;
  bool fenced_ = false;
  // Sessions and seeds.
  bool seedSet_ = false;
  uint32_t seed_ = 0;
  uint32_t fallback_ = 0x2545F491u;  // (no Random: xorshift32, so the host tests repeat)
  uint32_t sessions_ = 0;
  uint32_t sessionSeed_ = 0;
  Began began_ = Began::None;
  // Counters (the console's D).
  uint32_t holds_ = 0, refusals_ = 0, undoDrops_ = 0, removedPicks_ = 0, refillWaits_ = 0;
  bool refillWaiting_ = false;  // the last pass's due pick waited for a refill
  bool lastPicked_ = false;
  Pass lastPick_;
};

// The console's Dn: dry-run picks on a copy of the session (the live one
// untouched), one loop pass each (a slot read ahead when one is due, as
// the loop's, else a pick), each treated as played (the next pick follows
// it). Its tallies: the classes, the breaks, the repeats (a row this run
// picked before), the artists, the reads and the times.
class DryRun {
public:
  static constexpr uint32_t kMaxPicks = 2000;
  struct Result {
    uint32_t picks = 0, failed = 0;
    uint32_t classes[kClasses] = {};
    uint32_t tempoBreaks = 0, keyBreaks = 0;
    uint32_t imputedTempo = 0, imputedKey = 0;
    uint32_t dry = 0, random = 0, noIndex = 0;
    uint32_t loosened = 0;  // picks whose pool was loosened (Pick::loosened)
    uint32_t repeats = 0;   // rows this run had picked before
    uint32_t artists = 0;   // distinct artist keys (0 not counted)
    uint32_t readsAtPick[kReadsPerPick + 1] = {};
    uint32_t prefetchReads = 0;
    uint32_t leftOut = 0;
  };
  DryRun() = default;
  ~DryRun() { end(); }
  DryRun(const DryRun&) = delete;
  DryRun& operator=(const DryRun&) = delete;

  // A copy of `live`'s session (its rings, its stream, its slot cache and
  // file state) for `picks` picks (1 to kMaxPicks), from `playingTrack`
  // on: its memory from the hooks (the engine's 19 KB, 16 KB of tallies, 4
  // bytes a pick for the times). False: no memory, nothing held.
  bool begin(const Engine& live, uint32_t picks, uint32_t playingTrack, Engine::AllocFn alloc = nullptr,
             Engine::FreeFn release = nullptr);
  // One pass. False: done (or not begun).
  bool step(const Settings& s, const Index& ix, uint32_t libraryTracks, TimedReader::Clock nowUs);
  void end();
  bool running() const { return times_ != nullptr && done_ < want_; }
  bool begun() const { return times_ != nullptr; }
  // The track the next dry pick follows (the last one picked: treated as
  // played; at first the playing track).
  uint32_t following() const { return playing_; }
  uint32_t done() const { return done_; }
  uint32_t wanted() const { return want_; }
  const Result& result() const { return r_; }
  // The picks' times in µs, sorted (done() of them; after the run).
  float* times() { return times_; }

private:
  Engine copy_;
  Engine::AllocFn alloc_ = nullptr;
  Engine::FreeFn free_ = nullptr;
  Result r_;
  Stats base_;
  uint32_t want_ = 0, done_ = 0;
  uint32_t playing_ = kNoTrackId;
  bool readFailed_ = false;         // the last pass's read ahead failed: a pick next
  uint8_t* rowsSeen_ = nullptr;     // 8 KB: a bit a row
  uint8_t* artistsSeen_ = nullptr;  // 8 KB: a bit a 16-bit artist
  float* times_ = nullptr;
};

}  // namespace autodj
