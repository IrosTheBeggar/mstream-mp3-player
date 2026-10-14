// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjDriver.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "NvsLayout.h"

namespace autodj {

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

constexpr uint32_t kBitsetBytes = 65536 / 8;

bool testAndSet(uint8_t* bits, uint32_t i) {
  const uint8_t m = static_cast<uint8_t>(1u << (i & 7));
  const bool was = (bits[i >> 3] & m) != 0;
  bits[i >> 3] = static_cast<uint8_t>(bits[i >> 3] | m);
  return was;
}

uint16_t rowOf(const Maps* m, uint32_t track) {
  return m && m->trackToRow && track < m->tracks ? m->trackToRow[track] : kNoRow;
}

}  // namespace

const char* beganName(Began b) {
  switch (b) {
    case Began::None: return "none";
    case Began::Boot: return "the boot (Auto DJ on)";
    case Began::On: return "switched on";
    case Began::NewQueue: return "a new queue (a Play, a Clear, or the undo of one)";
    case Began::Console: return "the console";
    case Began::FromHere: return "Auto DJ from here";
    case Began::Surprise: return "the Auto DJ button";
    case Began::IndexChanged: return "another index on the card";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// MarkCarry, TimedReader
// ---------------------------------------------------------------------------
void MarkCarry::fenceUp(const QueueModel& q, const Marks& marks) {
  n_ = 0;
  size_ = q.size();
  armed_ = true;
  if (marks.count() == 0) return;
  for (uint32_t p = 0; p < q.size() && n_ < static_cast<uint32_t>(kMarks); ++p)
    if (marks.has(q.keyAt(p))) pos_[n_++] = p;
}

void MarkCarry::remapped(const QueueModel& q, Marks* marks) {
  if (!armed_) return;
  armed_ = false;
  marks->clear();
  if (q.size() != size_) {
    n_ = 0;
    return;
  }
  for (uint32_t i = 0; i < n_; ++i) marks->add(q.keyAt(pos_[i]));
}

cardcontract::mpdj::SlotRead TimedReader::read(uint16_t row, uint8_t* bytes, uint32_t* count, cardcontract::Why* why) {
  const uint64_t t0 = now_ ? now_() : 0;
  const cardcontract::mpdj::SlotRead r = inner_->read(row, bytes, count, why);
  if (now_) us += static_cast<uint32_t>(now_() - t0);
  ++reads;
  return r;
}

// ---------------------------------------------------------------------------
// The settings and the sessions
// ---------------------------------------------------------------------------
bool Driver::begin() { return engine_.begin(cfg_.alloc, cfg_.release); }

void Driver::load(const Settings& s) {
  s_ = s;
  if (!nvslayout::autoDjFloorValid(s_.floorPct)) s_.floorPct = kFloorMin;
  if (s_.on) newSession(Began::Boot, kNoTrackId, nullptr);
}

bool Driver::setOn(bool on) {
  if (s_.on == on) return false;
  s_.on = on;
  failures_.clear();
  retry_ = false;
  pending_ = QueueModel::kNone;
  if (on) newSession(Began::On, kNoTrackId, nullptr);
  return true;
}

bool Driver::setMode(Mode m) {
  if (s_.mode == m) return false;
  s_.mode = m;
  engine_.modeChanged();
  return true;
}

bool Driver::setFloor(uint8_t pct) {
  if (!nvslayout::autoDjFloorValid(pct) || s_.floorPct == pct) return false;
  s_.floorPct = pct;
  return true;
}

bool Driver::setMatch(bool tempo, bool key) {
  if (s_.matchTempo == tempo && s_.matchKey == key) return false;
  s_.matchTempo = tempo;
  s_.matchKey = key;
  return true;
}

void Driver::setSeed(uint32_t seed) {
  seed_ = seed;
  seedSet_ = true;
}

uint32_t Driver::nextSeed() {
  if (seedSet_) {
    seedSet_ = false;
    return seed_;
  }
  if (cfg_.random) return cfg_.random();
  fallback_ ^= fallback_ << 13;
  fallback_ ^= fallback_ >> 17;
  fallback_ ^= fallback_ << 5;
  return fallback_;
}

void Driver::newSession(Began why, uint32_t track, const Maps* maps) {
  sessionSeed_ = nextSeed();
  engine_.newSession(sessionSeed_, track, maps);
  ++sessions_;
  began_ = why;
  pending_ = QueueModel::kNone;
  pendingRow_ = kNoRow;
  watch_.row = kNoRow;  // (another index's, maybe: the next pass with maps says it again)
  retry_ = false;
}

// ---------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------
TriggerIn Driver::triggerIn(const PlaybackController& player, const Index& ix, const Env& env) const {
  TriggerIn in;
  in.on = s_.on;
  in.joining = ix.joining;
  in.cardHeld = env.cardHeld;
  in.restartSet = env.restartSet;
  in.sleepEndOfQueue = env.sleepEndOfQueue;
  in.failurePaused = failures_.paused();
  return fromPlayer(player, in);
}

void Driver::watchCurrent(const QueueModel& q) {
  watch_ = Watch();
  watch_.key = q.currentKey();
  watch_.track = q.currentTrack();
}

Pass Driver::step(PlaybackController& player, const Index& ix, const Env& env) {
  Pass p;
  const QueueModel& q = player.queue();
  if (player.fenced()) {
    // Behind the library update's fence the queue is the build's (released:
    // empty to fromPlayer()); fenceUp() noted what remapped() needs. The
    // trigger's answer is the fence's, not an empty queue's: no pick,
    // nothing owed, the holds as they are.
    p.trigger = decide(triggerIn(player, ix, env));
    if (s_.on) p.trigger.wait = Wait::Fenced;
    trigger_ = p.trigger;
    return p;
  }
  const Maps* maps = ix.maps;
  p.mapsArrived = maps && !mapsSeen_;
  mapsSeen_ = maps != nullptr;

  // 1. A new queue: a new session, seeded by its current track.
  if (!epochSeen_) {
    epochSeen_ = true;
    epoch_ = q.playEpoch();
  } else if (q.playEpoch() != epoch_) {
    epoch_ = q.playEpoch();
    pending_ = QueueModel::kNone;
    if (s_.on) {
      newSession(Began::NewQueue, q.currentTrack(), maps);
      p.began = Began::NewQueue;
    }
    watchCurrent(q);  // (the new queue's current entry: nothing left)
    ends_ = player.naturalEnds();
  }

  // 2. The current entry: the one before left, this one started.
  const uint32_t key = q.currentKey();
  const bool endedNow = player.naturalEnds() != ends_;
  ends_ = player.naturalEnds();
  if (key != watch_.key) {
    const bool natural = endedNow || watch_.ended;
    if (s_.on && watch_.key != QueueModel::kNone && watch_.started) {
      // (Its row: the maps', else the one they last gave: away after a
      // fence until the join is ready again.)
      engine_.rowLeft(maps ? rowOf(maps, watch_.track) : watch_.row, watch_.playedMs, natural);
      if (!natural && watch_.playedMs < kQuickSkipMs) {
        p.quickSkip = true;
        p.leftTrack = watch_.track;
        p.leftMs = watch_.playedMs;
      }
    }
    watchCurrent(q);
  } else if (endedNow) {
    watch_.ended = true;  // (still current: the queue's end, Repeat One's loop)
  }
  if (maps) watch_.row = rowOf(maps, watch_.track);
  const bool ours = entryStart_.update(key, env.trackSeq, env.positionMs);
  if (key != QueueModel::kNone && player.state() == PlayState::Playing) {
    if (!watch_.started) {
      // Started: it steers the pick it may be due right now (D1).
      watch_.started = true;
      p.started = true;
      p.startedTrack = watch_.track;
      p.startedByAutoDj = marks_.has(key);
      if (s_.on) engine_.trackStarted(watch_.track, maps, p.startedByAutoDj);
      if (pending_ == key) pending_ = QueueModel::kNone;
    }
    if (ours && env.positionKnown && env.audioPlaying) {
      watch_.playedMs = std::max(watch_.playedMs, env.positionMs);
      if (!watch_.played && env.positionMs >= kPlayingMs) {
        watch_.played = true;
        p.playing = true;
        if (s_.on) failures_.played();
      }
    }
  }
  // The index joined since this track started: it steers from now.
  if (p.mapsArrived && s_.on && watch_.started && !p.started) {
    engine_.trackStarted(watch_.track, maps, marks_.has(key));
  }
  // Auto DJ's pick taken out before it played: a skip at 0:00.
  if (pending_ != QueueModel::kNone && q.positionOf(pending_) == QueueModel::kNone) {
    if (s_.on) engine_.rowLeft(maps ? rowOf(maps, pendingTrack_) : pendingRow_, 0, false);
    p.removedPick = true;
    p.removedTrack = pendingTrack_;
    ++removedPicks_;
    pending_ = QueueModel::kNone;
  }

  // 3. A track that failed.
  const PlaybackController::Failure& f = player.lastFailure();
  if (f.count != failuresSeen_) {
    failuresSeen_ = f.count;
    p.failed = true;
    p.failedPick = marks_.has(f.key);
    if (s_.on) p.paused = failures_.failed(p.failedPick);
  }

  // 4. The trigger: a pick, or a slot read ahead.
  p.trigger = decide(triggerIn(player, ix, env));
  trigger_ = p.trigger;
  if (s_.on && p.trigger.hold != Hold::None && p.trigger.hold != lastHold_) ++holds_;
  lastHold_ = s_.on ? p.trigger.hold : Hold::None;
  if (p.trigger.pick && !retryWaits(env.nowMs)) {
    pickInto(player, ix, env, p.trigger.cue, &p);
    return p;
  }
  uint16_t row = kNoRow;
  if (readWait_ && static_cast<int32_t>(env.nowMs - readAtMs_) >= 0) readWait_ = false;
  if (s_.on && maps && ix.slots && !ix.joining && !env.listMoving && !env.cardHeld && !readWait_ &&
      engine_.prefetchDue(s_, &row)) {
    TimedReader timed(ix.slots, cfg_.nowUs);
    p.prefetched = true;
    p.prefetchRow = row;
    p.prefetch = engine_.prefetch(timed, row);
    p.prefetchUs = timed.us;
    if (p.prefetch != SlotResult::Ok) {
      readWait_ = true;
      readAtMs_ = env.nowMs + kReadAgainMs;
    }
  }
  return p;
}

void Driver::pickInto(PlaybackController& player, const Index& ix, const Env& env, bool cue, Pass* p) {
  const QueueModel& q = player.queue();
  TimedReader timed(ix.slots, cfg_.nowUs);
  PickIn in;
  in.maps = ix.maps;
  in.slots = ix.slots ? &timed : nullptr;
  in.k = ix.k;
  in.selHead = ix.selHead;
  in.selFloor = ix.selFloor;
  in.playingTrack = q.currentTrack();
  in.libraryTracks = env.libraryTracks;
  const uint64_t t0 = now();
  p->pick = engine_.pick(s_, in);
  p->pickUs = static_cast<uint32_t>(now() - t0);
  p->readUs = timed.us;
  p->picked = true;
  const QueueModel::Edit before = q.undoable();
  if (p->pick.ok && player.addFromAutoDj(p->pick.track, cue)) {
    p->added = true;
    p->key = q.keyAt(q.size() - 1);
    p->cued = cue && player.cuedByAutoDj() && q.currentKey() == p->key;
    marks_.add(p->key);
    pending_ = p->key;
    pendingTrack_ = p->pick.track;
    pendingRow_ = p->pick.row;
    p->undoDropped = before != QueueModel::Edit::None && q.undoable() == QueueModel::Edit::None;
    undoDrops_ += p->undoDropped;
    retry_ = false;
  } else {
    // (The engine joined it to the ring when it picked it: it isn't in the
    // queue, so it doesn't steer the retry.)
    if (p->pick.ok) engine_.pickRefused(p->pick.row);
    p->refused = true;
    ++refusals_;
    retry_ = true;
    retryAtMs_ = env.nowMs + kRetryMs;
  }
  lastPick_ = *p;
  lastPicked_ = true;
}

Pass Driver::pickNow(PlaybackController& player, const Index& ix, const Env& env) {
  Pass p;
  p.trigger = trigger_;
  if (player.fenced()) {
    p.refused = true;
    return p;
  }
  pickInto(player, ix, env, player.endedAtEnd(), &p);
  return p;
}

bool Driver::playFrom(PlaybackController& player, uint32_t track, Began why, const Maps* maps, bool* switchedOn) {
  if (switchedOn) *switchedOn = false;
  if (track == kNoTrackId || !player.playNow(&track, 1, 0)) return false;
  // After the Play: its new queue is this session's (no second one at the
  // next pass), seeded by the track.
  const QueueModel& q = player.queue();
  epochSeen_ = true;
  epoch_ = q.playEpoch();
  watchCurrent(q);
  ends_ = player.naturalEnds();
  if (!s_.on) {
    s_.on = true;
    failures_.clear();
    if (switchedOn) *switchedOn = true;
  }
  newSession(why, track, maps);
  return true;
}

Pick Driver::surprise(const Index& ix) {
  PickIn in;
  in.maps = ix.maps;
  return engine_.surprise(s_, in);
}

// ---------------------------------------------------------------------------
// The fence
// ---------------------------------------------------------------------------
void Driver::fenceUp(const QueueModel& q) {
  carry_.fenceUp(q, marks_);
  fenceSize_ = q.size();
  fenceCurrent_ = q.current();
  fencePending_ = pending_ != QueueModel::kNone ? q.positionOf(pending_) : QueueModel::kNone;
  fenced_ = true;
}

void Driver::remapped(const QueueModel& q) {
  carry_.remapped(q, &marks_);
  engine_.tracksRenumbered();
  if (!fenced_) return;
  fenced_ = false;
  epoch_ = q.playEpoch();  // (a read-back is no new queue)
  if (q.size() == fenceSize_ && fenceCurrent_ >= 0) {
    // The same entries under new keys and ids: the one that was current
    // (the read-back may have moved on from it: a track that ended inside
    // the fence, which the next pass then sees as left by its end). Their
    // rows stay: an index's rows don't change with the library's ids.
    const uint32_t at = static_cast<uint32_t>(fenceCurrent_);
    watch_.key = q.keyAt(at);
    watch_.track = q.trackAt(at);
    if (q.currentKey() == watch_.key) entryStart_.rekey(watch_.key);
    pending_ = fencePending_ != QueueModel::kNone ? q.keyAt(fencePending_) : QueueModel::kNone;
    if (pending_ != QueueModel::kNone) pendingTrack_ = q.trackAt(fencePending_);
  } else {
    // Another queue (a track gone from the card): taken as it is, nothing
    // left or started by the read-back.
    watchCurrent(q);
    pending_ = QueueModel::kNone;
    pendingRow_ = kNoRow;
  }
}

// ---------------------------------------------------------------------------
// The console's dry run
// ---------------------------------------------------------------------------
bool DryRun::begin(const Engine& live, uint32_t picks, uint32_t playingTrack, Engine::AllocFn alloc,
                   Engine::FreeFn release) {
  end();
  alloc_ = alloc ? alloc : defaultAlloc;
  free_ = release ? release : defaultFree;
  if (picks == 0) return false;
  if (picks > kMaxPicks) picks = kMaxPicks;
  rowsSeen_ = static_cast<uint8_t*>(alloc_(kBitsetBytes));
  artistsSeen_ = static_cast<uint8_t*>(alloc_(kBitsetBytes));
  times_ = static_cast<float*>(alloc_(picks * sizeof(float)));
  if (!rowsSeen_ || !artistsSeen_ || !times_ || !copy_.begin(alloc_, free_)) {
    end();
    return false;
  }
  std::memset(rowsSeen_, 0, kBitsetBytes);
  std::memset(artistsSeen_, 0, kBitsetBytes);
  copy_.copySession(live);
  base_ = copy_.stats();
  r_ = Result();
  want_ = picks;
  done_ = 0;
  playing_ = playingTrack;
  readFailed_ = false;
  return true;
}

void DryRun::end() {
  copy_.end();
  if (free_) {
    if (rowsSeen_) free_(rowsSeen_);
    if (artistsSeen_) free_(artistsSeen_);
    if (times_) free_(times_);
  }
  rowsSeen_ = artistsSeen_ = nullptr;
  times_ = nullptr;
  want_ = done_ = 0;
}

bool DryRun::step(const Settings& s, const Index& ix, uint32_t libraryTracks, TimedReader::Clock nowUs) {
  if (!running()) return false;
  // A read ahead first, as the loop makes one a pass between two picks.
  uint16_t row = kNoRow;
  if (!readFailed_ && ix.maps && ix.slots && copy_.prefetchDue(s, &row)) {
    readFailed_ = copy_.prefetch(*ix.slots, row) != SlotResult::Ok;
    const Stats& st = copy_.stats();
    r_.prefetchReads = st.prefetchReads - base_.prefetchReads;
    return true;
  }
  readFailed_ = false;
  TimedReader timed(ix.slots, nowUs);
  PickIn in;
  in.maps = ix.maps;
  in.slots = ix.slots ? &timed : nullptr;
  in.k = ix.k;
  in.selHead = ix.selHead;
  in.selFloor = ix.selFloor;
  in.playingTrack = playing_;
  in.libraryTracks = libraryTracks;
  const uint64_t t0 = nowUs ? nowUs() : 0;
  const Pick pk = copy_.pick(s, in);
  times_[done_] = nowUs ? static_cast<float>(nowUs() - t0) : 0.0f;
  ++done_;
  if (!pk.ok) {
    ++r_.failed;
  } else {
    ++r_.picks;
    playing_ = pk.track;  // treated as played: the next pick follows it
    if (pk.row != kNoRow) {
      if (testAndSet(rowsSeen_, pk.row)) ++r_.repeats;
      const uint16_t a = ix.maps && pk.row < ix.maps->rows ? filterArtist(ix.maps->filters[pk.row]) : 0;
      if (a && !testAndSet(artistsSeen_, a)) ++r_.artists;
    }
  }
  const Stats& st = copy_.stats();
  for (uint32_t c = 0; c < kClasses; ++c) r_.classes[c] = st.classes[c] - base_.classes[c];
  for (uint32_t i = 0; i <= kReadsPerPick; ++i) r_.readsAtPick[i] = st.readsAtPick[i] - base_.readsAtPick[i];
  const int m = static_cast<int>(s.mode);
  r_.tempoBreaks = st.tempoBreaks[m] - base_.tempoBreaks[m];
  r_.keyBreaks = st.keyBreaks[m] - base_.keyBreaks[m];
  r_.imputedTempo = st.imputedTempo - base_.imputedTempo;
  r_.imputedKey = st.imputedKey - base_.imputedKey;
  r_.dry = st.dry - base_.dry;
  r_.random = st.random - base_.random;
  r_.noIndex = st.noIndex - base_.noIndex;
  r_.prefetchReads = st.prefetchReads - base_.prefetchReads;
  r_.leftOut = st.leftOut - base_.leftOut;
  if (done_ == want_) std::sort(times_, times_ + done_);
  return done_ < want_;
}

}  // namespace autodj
