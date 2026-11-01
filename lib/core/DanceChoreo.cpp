// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "DanceChoreo.h"

#include <cmath>

#include "DancePose.h"  // danceWeight: the weight is Dancer's, unchanged (3.6)
#include "DanceRules.h"

// The step names (U1-U11, TRACK RESET, HOLD, CUT, END, SWITCH, PICK,
// IDLE-PICK) are Appendix E.2's; each function below says which it is. The
// only floats are E.3's: the weight (Dancer's formula), the idle clock, the
// inputs dt and confidence, and e16's c. The clock and positions are doubles
// using + - * / floor ceil llround (and fabs) only.
namespace dance {

static_assert(sizeof(Choreographer) <= 1024, "E.1, 6.8: the Choreographer lives in DanceMode's .bss budget");

namespace {

constexpr uint32_t kSeedForZero = 0x4D50444Eu;  // D.3's seed, for a zero one
constexpr uint32_t kIdleEnergy = 39322;         // e16 at w <= 0.5 (3.6: 0.6)
constexpr uint32_t kFullEnergy = 65536;
constexpr double kPos16PerBeat = 786432.0;      // 12 ticks x 65536
constexpr uint8_t kClsQuiet = 0, kClsListening = 1;
constexpr int kMaxShifts = 64;  // REPHASE's 1/S shifts (S is 1/2, 1 or 2 in practice)
// E.2.3's input bounds: the values that keep every double far from an int64 or
// uint32 conversion's limits.
constexpr float kMaxDt = 1000.0f;                  // s: one subtraction keeps idleSec in 0..2000
constexpr double kMinFold = 1.0 / 64, kMaxFold = 64.0;   // S
constexpr double kMaxTrackerBeats = 1099511627776.0;  // 2^40
constexpr float kMaxBpm = 1000.0f;
constexpr double kMaxClock = 4503599627370496.0;    // 2^52 dance beats
constexpr double kIdleMsWrap = 2147483648.0;        // 2^31 ms: a continuous idle's ms wrap

// 2.8's auto-bounce as labels: one beat, unranged, no keys, no travel.
constexpr MoveLabels kAutoBounceLabels = {"auto-bounce", MoveKind::Beat, 1, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0,
                                          0, 0, StartAt::Any, 0, nullptr};
constexpr MoveLabels kNoLabels = {};

int8_t clampSpot(int s) { return static_cast<int8_t>(s < -1 ? -1 : (s > 1 ? 1 : s)); }

int64_t mod8(int64_t x) { return ((x % 8) + 8) % 8; }

// E.2.1's e16(w).
uint32_t energyOf(float w) {
  float c = (w - 0.5f) * 2.0f;
  if (!(c > 0.0f)) c = 0.0f;
  if (c > 1.0f) c = 1.0f;
  return kIdleEnergy + static_cast<uint32_t>(static_cast<int32_t>(std::floor(26214.0 * static_cast<double>(c) + 0.5)));
}

Cue firstCue() {
  Cue c = {};
  c.beatMove = kNoMove;
  c.idleMove = kNoMove;
  c.energy = kIdleEnergy;
  c.spot = 0;
  c.rootX = static_cast<int16_t>(spotX(0));
  c.rootY = kStage.groundY;
  c.state = DanceState::Quiet;
  return c;
}

RunInfo firstRun() {
  RunInfo r = {};
  r.move = kNoMove;
  r.status = RunStatus::None;
  return r;
}

bool ranged(const MoveLabels& m) { return !(m.paceLo == 0 && m.paceHi == 0); }
int beatsOf(const MoveLabels& m) { return m.beats > 0 ? m.beats : 1; }
// D2: a play's length in half dance beats, beats * 2^(1 - k).
int d2Of(const MoveLabels& m, int k) { return k > 0 ? beatsOf(m) : (k < 0 ? 4 * beatsOf(m) : 2 * beatsOf(m)); }
uint8_t fromKey(const MoveLabels& m, bool mirrored) { return mirrored ? m.fromTagM : m.fromTag; }
uint8_t toKey(const MoveLabels& m, bool mirrored) { return mirrored ? m.toTagM : m.toTag; }

}  // namespace

// ---- construction and the calls (E.2.8) ------------------------------------

Choreographer::Choreographer() : character_(nullptr), pending_(nullptr), hasPending_(false) { reset(0); }

void Choreographer::reset(uint32_t pathHash) {
  const Character* keep = hasPending_ ? pending_ : character_;  // a pending switch applies at once
  w_ = 0.0f;
  idleSec_ = 0.0f;
  clock_ = 0.0;
  haveClock_ = false;
  lastTB_ = lastTl_ = sinceGrid_ = 0.0;
  lastS_ = 1.0;
  show_ = false;
  acqSeen_ = 0;
  T_ = 0.0;
  t10_ = 0;
  phraseValid_ = false;
  A_ = 0;
  entering_ = false;
  enterAt_ = 0;
  run_ = firstRun();
  leaving_ = false;
  prevU_ = -1.0;
  lastU_ = 0.0;
  heldEnergy_ = kIdleEnergy;
  spot_ = 0;
  lastMove_ = kNoMove;
  lastPlays_ = 0;
  lastFirstMirror_ = false;
  recent_[0] = recent_[1] = recent_[2] = kNoMove;
  prevTo_ = 0;
  cutExempt_ = kNoMove;
  prevK_ = kNoOctave;
  firstRunBias_ = false;
  idleMove_ = kNoMove;
  idleMirror_ = false;
  idleRunMs_ = 0;
  idleCls_ = kClsQuiet;
  idleMsRun_ = 0.0;
  lastIdle_ = kNoMove;
  seed(pathHash);
  character_ = keep;
  pending_ = nullptr;
  hasPending_ = false;
  switchAt_ = 0;
  for (uint32_t& g : genres_) g = 0;
  for (uint32_t& g : injected_) g = 0;
  genreCount_ = injectedCount_ = 0;
  pathHash_ = pathHash;
  restartsSeen_ = 0;
  started_ = false;
  lastAcqIn_ = 0;
  lastShowDance_ = false;
  lastEnergy_ = kIdleEnergy;
  mfOn_ = false;
  mfMove_ = kNoMove;
  mfPos16_ = 0;
  mfMirrored_ = false;
  forced_ = kNoMove;
  base_ = cue_ = firstCue();
}

void Choreographer::onTrack(uint32_t pathHash, const uint32_t* genreKeys, int count) {
  pathHash_ = pathHash;
  genreCount_ = 0;
  if (genreKeys)
    for (int i = 0; i < count && i < kTrackGenres; ++i) genres_[genreCount_++] = genreKeys[i];
  injectedCount_ = 0;
  seed(pathHash_ ^ restartsSeen_ ^ lastAcqIn_);
}

void Choreographer::syncPath(uint32_t pathHash) {
  if (pathHash != pathHash_) onTrack(pathHash, nullptr, 0);
}

void Choreographer::setCharacter(const Character* c) {
  if (c == character_) {  // a pending switch is dropped
    pending_ = nullptr;
    hasPending_ = false;
    return;
  }
  if (!character_ || run_.status != RunStatus::Running || !lastShowDance_) {
    doSwitch(c);  // at once while idle (3.7)
    return;
  }
  // While dancing: at the next whole dance beat, where U7 ends the run. The
  // clock may lie more than a beat behind the run's start (backward nudges),
  // so the content's end is held at 0 or above.
  pending_ = c;
  hasPending_ = true;
  switchAt_ = static_cast<int64_t>(std::floor(clock_)) + 1;
  if (switchAt_ < run_.end) run_.end = switchAt_;
  double u = static_cast<double>(switchAt_ - run_.start) * scaleK();
  if (u < 0.0) u = 0.0;
  if (u < run_.uEnd) run_.uEnd = u;
}

void Choreographer::freeze(int move, uint32_t pos16, bool mirrored) {
  const bool own = character_ && move >= 0 && move < character_->moveCount() && move < kMaxMoves;
  if (own || move == kAutoBounce || move == kAutoIdle) {
    mfOn_ = true;
    mfMove_ = static_cast<int8_t>(move);
    mfPos16_ = pos16;
    mfMirrored_ = mirrored;
  } else {
    mfOn_ = false;
  }
}

void Choreographer::forceNext(int move) {
  // Only a move index can be taken; any other value means the normal pick (E.2.5).
  forced_ = move >= 0 && move < kMaxMoves ? static_cast<int8_t>(move) : kNoMove;
}

void Choreographer::setSpot(int spot) {
  const int8_t s = clampSpot(spot);
  if (run_.status != RunStatus::None) endRun();
  spot_ = s;  // overrides END's travel rule
  entering_ = false;
  // The cue's spot changes at once (3.9's Ms, for screenshots).
  base_.spot = cue_.spot = s;
  base_.rootX = cue_.rootX = static_cast<int16_t>(spotX(s));
}

void Choreographer::addGenre(uint32_t key) {
  if (injectedCount_ < kInjectedGenres) injected_[injectedCount_++] = key;
}

const Cue& Choreographer::cue() const { return cue_; }
const Character* Choreographer::character() const { return character_; }
const RunInfo& Choreographer::run() const { return run_; }
int Choreographer::t10() const { return t10_; }
int8_t Choreographer::prevK() const { return prevK_; }
double Choreographer::danceClock() const { return clock_; }

int8_t Choreographer::eligibleNow(int move) const {
  if (!character_ || move < 0 || move >= character_->moveCount() || move >= kMaxMoves) return kNoOctave;
  const MoveLabels& m = character_->move(move);
  if (m.kind != MoveKind::Beat || m.needs != 0 || !genreOk(move)) return kNoOctave;
  return moveK(move);
}

// ---- helpers -------------------------------------------------------------------

void Choreographer::seed(uint32_t v) { rng_ = v ? v : kSeedForZero; }

uint32_t Choreographer::below(uint32_t n) {
  rng_ ^= rng_ << 13;
  rng_ ^= rng_ >> 17;
  rng_ ^= rng_ << 5;
  return n ? rng_ % n : 0;
}

const MoveLabels& Choreographer::labels(int move) const {
  if (move == kAutoBounce) return kAutoBounceLabels;
  if (character_ && move >= 0 && move < character_->moveCount()) return character_->move(move);
  return kNoLabels;
}

// E.2.5's k_i: with no latched tempo, 0 for an unranged move and none for a ranged one.
int8_t Choreographer::moveK(int i) const {
  const MoveLabels& m = labels(i);
  if (t10_ == 0) return ranged(m) ? kNoOctave : 0;
  return eligibleK(t10_, m.paceLo, m.paceHi, prevK_, m.beats, m.travel);
}

// 2.7: an untagged move, or a tagged one with a key among the track's and the injected.
bool Choreographer::genreOk(int i) const {
  const MoveLabels& m = labels(i);
  if (m.genreCount == 0) return true;
  if (!m.genreKeys) return false;
  for (int g = 0; g < m.genreCount; ++g) {
    for (int t = 0; t < genreCount_; ++t)
      if (genres_[t] == m.genreKeys[g]) return true;
    for (int t = 0; t < injectedCount_; ++t)
      if (injected_[t] == m.genreKeys[g]) return true;
  }
  return false;
}

double Choreographer::scaleK() const { return run_.k > 0 ? 2.0 : (run_.k < 0 ? 0.5 : 1.0); }

// max(0, min(x, uEnd)): 0 or above whatever uEnd is.
double Choreographer::uRaw() const {
  double u = (clock_ - static_cast<double>(run_.start)) * scaleK();
  if (u > run_.uEnd) u = run_.uEnd;
  if (u < 0.0) u = 0.0;
  return u;
}

void Choreographer::clearBeatCue() {
  base_.beatMove = kNoMove;
  base_.u = 0.0;
  base_.beatPos16 = 0;
  base_.k = 0;
  base_.beatMirror = false;
}

// ---- E.2.4's operations ----------------------------------------------------------

// TRACK RESET: history, latch, phrase and beat move cleared.
void Choreographer::trackReset() {
  endRun();
  lastMove_ = kNoMove;
  lastPlays_ = 0;
  recent_[0] = recent_[1] = recent_[2] = kNoMove;
  prevTo_ = 0;
  cutExempt_ = kNoMove;
  prevK_ = kNoOctave;
  T_ = 0.0;
  t10_ = 0;
  phraseValid_ = false;
  haveClock_ = false;
  entering_ = false;
  sinceGrid_ = 0.0;
  clearBeatCue();
  firstRunBias_ = true;
}

// HOLD(u): the run shows its step at u, with the energy it had.
void Choreographer::hold(double u) {
  if (u < run_.uEnd) run_.uEnd = u;
  run_.status = RunStatus::Held;
  heldEnergy_ = lastEnergy_;
}

// CUT(uNow): on to the next whole move beat, held to the next whole dance beat.
void Choreographer::cut(double uNow) {
  const double c = std::ceil(uNow);
  if (c < run_.uEnd) {
    run_.uEnd = c;
    const int64_t e = static_cast<int64_t>(std::ceil(static_cast<double>(run_.start) + c / scaleK()));
    if (e < run_.end) run_.end = e;
  }
}

// END(run).
void Choreographer::endRun() {
  if (run_.status == RunStatus::None) return;
  const double u = run_.status == RunStatus::Running ? uRaw() : run_.uEnd;
  const MoveLabels& m = labels(run_.move);
  const int beats = beatsOf(m);
  if (run_.move >= 0 && m.travel != 0 && u >= beats / 2.0) {
    // A completed travel lands; one abandoned past half way counts as arrived.
    const int sgn = run_.firstMirror ? -1 : 1;
    spot_ = clampSpot(run_.spot + m.travel * sgn);
  }
  if (u >= static_cast<double>(run_.plays) * beats) {
    const int j = run_.plays > 0 ? run_.plays - 1 : 0;
    const bool lastMirror = (m.flags & kMirror) ? (run_.firstMirror != ((j & 1) != 0)) : false;
    prevTo_ = toKey(m, lastMirror);
  } else {
    prevTo_ = 0;  // unknown: a wildcard
  }
  cutExempt_ = run_.cut ? run_.move : kNoMove;
  run_.status = RunStatus::None;
}

// SWITCH(c) (3.7).
void Choreographer::doSwitch(const Character* c) {
  endRun();
  character_ = c;
  pending_ = nullptr;
  hasPending_ = false;
  lastMove_ = kNoMove;
  lastPlays_ = 0;
  recent_[0] = recent_[1] = recent_[2] = kNoMove;
  prevTo_ = 0;
  cutExempt_ = kNoMove;
  forced_ = kNoMove;
  mfOn_ = false;
  if (c && !hasTravel(*c)) spot_ = 0;
  clearBeatCue();
  entering_ = false;
  idleMove_ = kNoMove;
  lastIdle_ = kNoMove;
}

// ---- E.2.5: PICK -------------------------------------------------------------

void Choreographer::startRun(int move, int8_t k, bool mirror, int8_t phrase, uint8_t level, int64_t start) {
  const MoveLabels& m = labels(move);
  const int d2 = d2Of(m, k);
  int plays;
  if (m.travel != 0 && move >= 0) {
    plays = 1;  // a travelling move plays once per run
  } else if (m.repeat > 0) {
    plays = m.repeat;
    if ((plays * d2) % 2 != 0) ++plays;  // repeat * D a whole number of dance beats
  } else {
    plays = 8 / d2 > 1 ? 8 / d2 : 1;  // fill 4 dance beats
  }
  run_.move = static_cast<int8_t>(move);
  run_.k = k;
  run_.plays = static_cast<uint8_t>(plays);
  run_.firstMirror = mirror;
  run_.spot = spot_;
  run_.phrase = phrase;
  run_.level = level;
  run_.cut = false;
  run_.status = RunStatus::Running;
  run_.start = start;
  run_.end = start + static_cast<int64_t>(plays) * d2 / 2;
  run_.uEnd = static_cast<double>(plays) * beatsOf(m);
  leaving_ = false;
  prevU_ = -1.0;
  lastU_ = 0.0;
  show_ = w_ > 0.5f;  // always true: U7 and U9 pick only with w > 0.5
  lastMove_ = static_cast<int8_t>(move);
  lastPlays_ = static_cast<uint8_t>(plays);
  lastFirstMirror_ = mirror;
  recent_[2] = recent_[1];
  recent_[1] = recent_[0];
  recent_[0] = static_cast<int8_t>(move);
  if (move >= 0 && ranged(m)) prevK_ = k;
  cutExempt_ = kNoMove;
  firstRunBias_ = false;
}

void Choreographer::pick(int64_t start) {
  if (!phraseValid_) {
    A_ = start;
    phraseValid_ = true;
  }
  const int p = static_cast<int>(mod8(start - A_));
  const int n = character_ ? (character_->moveCount() < kMaxMoves ? character_->moveCount() : kMaxMoves) : 0;
  const uint8_t spotBit = static_cast<uint8_t>(1u << (spot_ + 1));

  // A forced move (Mm): pace, genre, alignment, place and no-repeat aren't checked.
  if (forced_ != kNoMove) {
    const int f = forced_;
    forced_ = kNoMove;
    if (f >= 0 && f < n && character_->move(f).kind == MoveKind::Beat) {
      const MoveLabels& m = character_->move(f);
      int8_t k = moveK(f);
      if (k == kNoOctave) k = 0;
      uint8_t o = 0;
      if (character_->startSpotMask(f, false) & spotBit) o |= 1;
      if ((m.flags & kMirror) && (character_->startSpotMask(f, true) & spotBit)) o |= 2;
      if (o == 0) o = 1;
      const bool mirror = o == 3 ? below(2) == 1 : o == 2;
      startRun(f, k, mirror, static_cast<int8_t>(p), 4, start);
      return;
    }
  }

  // Each move's octave, its hard filters (never relaxed) and its soft ones.
  int8_t ks[kMaxMoves];
  uint8_t allowed[kMaxMoves];  // bit 0 unmirrored, bit 1 mirrored
  bool hard[kMaxMoves], soft[kMaxMoves];
  for (int i = 0; i < n; ++i) {
    const MoveLabels& m = character_->move(i);
    ks[i] = moveK(i);
    uint8_t o = (m.flags & kMirror) ? 3 : 1;
    if (m.travel != 0) {
      if (!(character_->startSpotMask(i, false) & spotBit)) o &= static_cast<uint8_t>(~1u);
      if (!(character_->startSpotMask(i, true) & spotBit)) o &= static_cast<uint8_t>(~2u);
    }
    allowed[i] = o;
    hard[i] = m.kind == MoveKind::Beat && m.needs == 0 && ks[i] != kNoOctave && genreOk(i) && o != 0;
    soft[i] = false;
    if (!hard[i]) continue;
    const int d2 = d2Of(m, ks[i]);
    const int span = d2 < 8 ? d2 : 8;
    const bool aligned = d2 < 2 || (2 * p) % span == 0;
    bool placeOk = true;
    if (m.flags & kPlaceStart) placeOk = placeOk && p == 0;
    if (m.flags & kPlaceEnd) placeOk = placeOk && ((d2 == 2 && (p == 6 || p == 7)) || (d2 == 4 && p == 6));
    soft[i] = aligned && placeOk && (m.travel == 0 || p == 0 || p == 4);
  }

  // The ladder (2.8): alignment, place and travel's p give way, then no-repeat.
  int8_t entMove[kMaxMoves];
  uint8_t entAllowed[kMaxMoves];
  bool entTwin[kMaxMoves];
  int count = 0, level = 0;
  for (level = 0; level <= 2 && count == 0; ++level) {
    for (int i = 0; i < n; ++i) {
      if (!hard[i] || (level == 0 && !soft[i])) continue;
      bool twin = false;
      uint8_t o = allowed[i];
      if (level < 2 && i == lastMove_ && lastMove_ != cutExempt_) {
        if (lastPlays_ != 1 || !(character_->move(i).flags & kMirror)) continue;
        const uint8_t bit = lastFirstMirror_ ? 1 : 2;  // the other orientation only
        if (!(o & bit)) continue;
        o = bit;
        twin = true;
      }
      entMove[count] = static_cast<int8_t>(i);
      entAllowed[count] = o;
      entTwin[count] = twin;
      ++count;
    }
  }
  --level;  // the level whose entries these are
  if (count == 0) {
    startRun(kAutoBounce, 0, false, static_cast<int8_t>(p), 3, start);  // 2.8: no draws
    return;
  }

  // The weights, integers.
  bool match[kMaxMoves];
  bool anyMatch = false;
  for (int e = 0; e < count; ++e) {
    const MoveLabels& m = character_->move(entMove[e]);
    bool ok = prevTo_ == 0;
    for (int o = 0; o < 2 && !ok; ++o) {
      if (!(entAllowed[e] & (1u << o))) continue;
      const uint8_t from = fromKey(m, o == 1);
      ok = from == 0 || from == prevTo_;
    }
    match[e] = ok;
    anyMatch = anyMatch || ok;
  }
  uint32_t weight[kMaxMoves];
  uint32_t sum = 0;
  for (int e = 0; e < count; ++e) {
    const int i = entMove[e];
    const MoveLabels& m = character_->move(i);
    const uint32_t g = m.genreCount > 0 ? 3 : 1;
    uint32_t b = 1;
    if (m.travel != 0) {
      for (int o = 0; o < 2; ++o) {
        if (!(entAllowed[e] & (1u << o))) continue;
        const int to = spot_ + (o == 1 ? -m.travel : m.travel);
        if ((to < 0 ? -to : to) < (spot_ < 0 ? -spot_ : spot_)) b = firstRunBias_ ? 4 : 2;  // toward the centre
      }
    }
    const uint32_t x = entTwin[e] ? 2 : 1;
    const uint32_t c = (match[e] || !anyMatch) ? 100 : 15;
    const bool recent = recent_[0] == i || recent_[1] == i || recent_[2] == i;
    const uint32_t r = (recent && !entTwin[e]) ? 35 : 100;
    weight[e] = m.weight10 * g * b * x * c * r;
    sum += weight[e];
  }

  // The draws: the entry, then its orientation when two remain.
  uint32_t r = below(sum);
  int chosen = 0;
  for (int e = 0; e < count; ++e) {
    if (r < weight[e]) {
      chosen = e;
      break;
    }
    r -= weight[e];
  }
  const int i = entMove[chosen];
  const MoveLabels& m = character_->move(i);
  uint8_t o = entAllowed[chosen];
  if (prevTo_ != 0 && match[chosen]) {
    for (int b = 0; b < 2; ++b) {
      if (!(o & (1u << b))) continue;
      const uint8_t from = fromKey(m, b == 1);
      if (from != 0 && from != prevTo_) o &= static_cast<uint8_t>(~(1u << b));
    }
  }
  const bool mirror = o == 3 ? below(2) == 1 : o == 2;
  startRun(i, ks[i], mirror, static_cast<int8_t>(p), static_cast<uint8_t>(level), start);
}

// ---- E.2.6: idle runs ----------------------------------------------------------

void Choreographer::idleStep(bool heard, double dt) {
  const uint8_t cls = heard ? kClsListening : kClsQuiet;
  if (!character_) {
    idleMove_ = kNoMove;
    return;
  }
  if (idleMove_ == kNoMove || idleCls_ != cls) {
    idlePick(cls);
    idleMsRun_ = 0.0;
    return;
  }
  idleMsRun_ += dt * 1000.0;
  if (idleRunMs_ == 0) {  // continuous: kept below 2^31 ms (one subtraction: dt <= 1,000 s)
    if (idleMsRun_ >= kIdleMsWrap) idleMsRun_ -= kIdleMsWrap;
  } else if (idleMsRun_ >= static_cast<double>(idleRunMs_)) {
    idleMsRun_ -= static_cast<double>(idleRunMs_);
    if (idleMsRun_ >= static_cast<double>(idleRunMs_)) idleMsRun_ = 0.0;  // a gap longer than the run
    lastIdle_ = idleMove_;
    idlePick(cls);
  }
}

// IDLE-PICK(cls).
void Choreographer::idlePick(uint8_t cls) {
  const uint8_t bit = cls == kClsQuiet ? kWhenQuiet : kWhenListening;
  const int n = character_->moveCount() < kMaxMoves ? character_->moveCount() : kMaxMoves;
  int8_t cands[kMaxMoves];
  int count = 0;
  for (int i = 0; i < n; ++i) {
    const MoveLabels& m = character_->move(i);
    if (m.kind == MoveKind::Idle && m.needs == 0 && (m.when & bit)) cands[count++] = static_cast<int8_t>(i);
  }
  if (count == 0) {  // the other state's idles (2.6)
    for (int i = 0; i < n; ++i) {
      const MoveLabels& m = character_->move(i);
      if (m.kind == MoveKind::Idle && m.needs == 0) cands[count++] = static_cast<int8_t>(i);
    }
  }
  idleCls_ = cls;
  if (count == 0) {  // auto-idle, re-picked every 4 s, no draws
    idleMove_ = kAutoIdle;
    idleMirror_ = false;
    idleRunMs_ = 4000;
    return;
  }
  if (count >= 2) {  // no immediate repeat
    int kept = 0;
    for (int c = 0; c < count; ++c)
      if (cands[c] != lastIdle_) cands[kept++] = cands[c];
    count = kept;
  }
  uint32_t sum = 0;
  for (int c = 0; c < count; ++c) sum += character_->move(cands[c]).weight10;
  uint32_t r = below(sum);
  int chosen = cands[0];
  for (int c = 0; c < count; ++c) {
    const uint32_t w = character_->move(cands[c]).weight10;
    if (r < w) {
      chosen = cands[c];
      break;
    }
    r -= w;
  }
  const MoveLabels& m = character_->move(chosen);
  const uint32_t loops = m.idleMs > 0 ? 1 + below(3) : 1;  // a draw only for a timed idle
  const bool mirrored = (m.flags & kMirror) ? below(2) == 1 : false;
  idleMove_ = static_cast<int8_t>(chosen);
  idleMirror_ = mirrored;
  idleRunMs_ = loops * m.idleMs;  // 0: continuous, never ends
}

// ---- E.2.7: the cue ------------------------------------------------------------

// The show latch at a running frame whose uRaw is raw, before prevU moves on:
// on the run's first frame, and wherever a new play starts, it becomes
// w > 0.5 (so beat frames go off only there: a play, a travel above all,
// once shown, shows to its end); at any other whole move beat it turns on
// when w > 0.5, never off.
void Choreographer::showStep(double raw) {
  const double beats = beatsOf(labels(run_.move));
  if (prevU_ < 0.0) {
    show_ = w_ > 0.5f;
    return;
  }
  const double play = std::floor(raw / beats);
  if (play > std::floor(prevU_ / beats) && play < static_cast<double>(run_.plays)) {
    show_ = w_ > 0.5f;
  } else if (std::floor(raw) > prevU_ && w_ > 0.5f) {
    show_ = true;
  }
}

void Choreographer::buildCue(bool heard, bool gridOk, bool lockedOk, int8_t freezeK) {
  Cue& c = base_;
  if (run_.status != RunStatus::None) {
    double u;
    if (run_.status == RunStatus::Running) {
      const double raw = uRaw();
      const double b = std::floor(raw);
      u = (b > prevU_ && raw - b <= 0.125) ? b : raw;  // 2.2's must-show rule
      showStep(raw);
      prevU_ = raw;
      lastU_ = u;
    } else {
      u = run_.uEnd;
    }
    const MoveLabels& m = labels(run_.move);
    c.beatMove = run_.move;
    c.k = run_.k;
    c.u = u;
    c.beatPos16 = static_cast<uint32_t>(std::floor(u * kPos16PerBeat));
    int64_t j = static_cast<int64_t>(std::floor(u / beatsOf(m)));
    if (j > run_.plays - 1) j = run_.plays - 1;
    if (j < 0) j = 0;
    c.beatMirror = (m.flags & kMirror) ? (run_.firstMirror != ((j & 1) != 0)) : false;
  }
  c.idleMove = idleMove_;
  c.idleMs = static_cast<uint32_t>(std::floor(idleMsRun_));
  c.idleMirror = idleMirror_;
  c.idleSeconds = idleSec_;
  c.weight = w_;
  c.energy = run_.status == RunStatus::Held ? heldEnergy_ : energyOf(w_);
  c.showDance = run_.status != RunStatus::None && show_;
  c.flash = gridOk && w_ > 0.5f && clock_ - std::floor(clock_) < 0.12;
  c.spot = spot_;
  c.rootX = static_cast<int16_t>(spotX(spot_));
  c.rootY = kStage.groundY;
  if (!heard) {
    c.state = run_.status != RunStatus::None ? DanceState::Freeze : DanceState::Quiet;
  } else if (run_.status == RunStatus::Running && lockedOk) {
    c.state = DanceState::Dancing;
  } else if (run_.status != RunStatus::None && !lockedOk) {
    c.state = DanceState::Leaving;
  } else if (lockedOk && w_ > 0.5f) {
    c.state = DanceState::Entering;
  } else {
    c.state = DanceState::Listening;
  }

  // Freezes override the shown cue only (3.8); Mf wins over k<n>.
  cue_ = c;
  if (mfOn_) {
    const bool beat = mfMove_ == kAutoBounce || (mfMove_ >= 0 && labels(mfMove_).kind == MoveKind::Beat &&
                                                 character_ && mfMove_ < character_->moveCount());
    if (beat) {
      cue_.beatMove = mfMove_;
      cue_.beatPos16 = mfPos16_;
      cue_.u = mfPos16_ / kPos16PerBeat;
      cue_.k = 0;
      cue_.beatMirror = mfMirrored_;
      cue_.showDance = true;
      cue_.weight = 1.0f;
      cue_.energy = kFullEnergy;
    } else {
      cue_.idleMove = mfMove_;
      cue_.idleMs = static_cast<uint32_t>((static_cast<uint64_t>(mfPos16_) * 10) >> 16);
      cue_.idleMirror = mfMirrored_;
      cue_.showDance = false;
      cue_.weight = 0.0f;
      cue_.energy = kIdleEnergy;
    }
    cue_.flash = false;
  } else if (freezeK >= 0 && freezeK <= 15) {
    const int f = character_ ? freezeMove(*character_) : kAutoBounce;
    const uint32_t beats = static_cast<uint32_t>(beatsOf(labels(f)));
    cue_.beatMove = static_cast<int8_t>(f);
    cue_.beatPos16 = (static_cast<uint32_t>(freezeK) * 98304u) % (beats * 786432u);
    cue_.u = cue_.beatPos16 / kPos16PerBeat;
    cue_.k = 0;
    cue_.beatMirror = false;
    cue_.showDance = true;
    cue_.weight = 1.0f;
    cue_.energy = kFullEnergy;
    cue_.flash = freezeK % 8 == 0;
  }
}

// ---- E.2.3: update ---------------------------------------------------------------

const Cue& Choreographer::update(const BeatInput& in) {
  // U1. dt: 0 unless above 0 (NaN too), at most kMaxDt (inf and huge values too).
  float dt = in.dt > 0.0f ? in.dt : 0.0f;
  if (!(dt <= kMaxDt)) dt = kMaxDt;

  // U2. Restarts: the Choreographer sees a tracker restart in its input.
  if (!started_ || in.restarts != restartsSeen_) {
    trackReset();
    restartsSeen_ = in.restarts;
    seed(pathHash_ ^ in.restarts ^ in.acquisitions);
    started_ = true;
  }
  lastAcqIn_ = in.acquisitions;

  // U3. The weight (Dancer's formula) and the idle clock. Out-of-contract
  // inputs first: a grid whose beats or tempo can't be used is no grid, a
  // scale outside 1/64..64 counts as 1, and confidence is clamped (NaN to 0).
  const bool heard = in.heard;
  const bool usable = std::fabs(in.trackerBeats) < kMaxTrackerBeats && in.rawBpm >= 0.0f && in.rawBpm <= kMaxBpm;
  const bool gridOk = heard && in.grid && usable;
  const bool lockedOk = gridOk && in.locked;
  const double S = in.foldScale >= kMinFold && in.foldScale <= kMaxFold ? in.foldScale : 1.0;
  const float conf = in.confidence >= 0.0f ? (in.confidence <= 1.0f ? in.confidence : 1.0f) : 0.0f;
  const float target = lockedOk ? danceWeight(conf) : 0.0f;
  w_ += (target - w_) * (1.0f - std::exp(-dt / 0.4f));
  idleSec_ += dt;
  if (idleSec_ >= 2000.0f) idleSec_ -= 2000.0f;

  // U4. The dance clock.
  sinceGrid_ += dt;
  bool far = false;
  if (gridOk) {
    const double tb = in.trackerBeats;
    const double step = tb - lastTB_;
    if (!haveClock_) {
      clock_ = tb * S;
      haveClock_ = true;
    } else if (in.acquisitions == acqSeen_ && S == lastS_ && step >= -0.1 && step <= 1.5) {
      clock_ += step * S;  // a PLL nudge passes through (a change of S re-phases)
    } else {
      // REPHASE: the new grid's dance-beat origin among the 1/S whole-tracker-beat
      // shifts, the one nearest the predicted dance position.
      const double predicted = clock_ + sinceGrid_ * lastTl_ / 60.0;
      const double x = tb * S;
      long long shifts = S < 1.0 ? std::llround(1.0 / S) : 1;
      if (shifts < 1) shifts = 1;
      if (shifts > kMaxShifts) shifts = kMaxShifts;
      double best = 0.0;
      for (long long j = 0; j < shifts; ++j) {
        const double xj = x + static_cast<double>(j) * S;
        const double cj = xj + std::floor(predicted - xj + 0.5);
        if (j == 0 || std::fabs(cj - predicted) < std::fabs(best - predicted)) best = cj;  // a tie keeps the lower j
      }
      const double delta = best - predicted;
      clock_ = best;
      far = std::fabs(delta) > 0.25;
      entering_ = false;
    }
    if (!(std::fabs(clock_) < kMaxClock)) {  // never in practice: starts over, as a far jump
      clock_ = tb * S;
      far = true;
      entering_ = false;
    }
    lastTB_ = tb;
    lastS_ = S;
    lastTl_ = static_cast<double>(in.rawBpm) * S;
    acqSeen_ = in.acquisitions;
    sinceGrid_ = 0.0;
  } else if (heard) {  // no grid, a fresh tracker, a dropped grid, an unusable one
    haveClock_ = false;
    phraseValid_ = false;
    entering_ = false;
  }  // not heard: the clock waits, so a resume continues it
  if (far) phraseValid_ = false;

  // U5. The latch (2.4): a 3 % Schmitt latch on the live folded tempo.
  bool tMoved = false;
  if (lockedOk && in.rawBpm > 0.0f) {
    const double tl = static_cast<double>(in.rawBpm) * S;
    if (t10_ == 0 || std::fabs(tl - T_) > 0.03 * T_) {
      tMoved = t10_ != 0;
      T_ = tl;
      const double t = std::floor(tl * 10.0 + 0.5);
      t10_ = t < 720.0 ? 720 : (t > 1760.0 ? 1760 : static_cast<int>(t));
    }
  }

  // U6. The running run's guards.
  if (run_.status == RunStatus::Running) {
    if (!gridOk || far) {
      hold(lastU_);  // at once: Freeze, Leaving on a dropped grid, or a far jump's re-anchor
    } else {
      double uNow = (clock_ - static_cast<double>(run_.start)) * scaleK();
      if (uNow < 0.0) uNow = 0.0;
      const MoveLabels& m = labels(run_.move);
      if (tMoved && ranged(m) && !paceHolds(t10_, run_.k, m.paceLo, m.paceHi)) {
        cut(uNow);
        run_.cut = true;
      }
      if (!in.locked && !leaving_) {
        cut(uNow);
        leaving_ = true;
      }
    }
  }

  // U7. The run's end.
  if (run_.status == RunStatus::Running && gridOk && clock_ >= static_cast<double>(run_.end)) {
    const int64_t e = run_.end;
    if (hasPending_) {
      endRun();
      doSwitch(pending_);
    } else if (!lockedOk) {
      hold(run_.uEnd);  // Leaving holds that step
    } else {
      endRun();
    }
    // A run starts only while beat frames would show: with w <= 0.5 the
    // dancer idles and comes back through Entering.
    if (run_.status == RunStatus::None && lockedOk && character_ && w_ > 0.5f) {
      pick(e);
      if (run_.status == RunStatus::Running && clock_ >= static_cast<double>(run_.end)) hold(run_.uEnd);
    }
  }

  // U8. Abandoning: idle frames take over.
  if ((run_.status == RunStatus::Held && w_ <= 0.5f) ||
      (run_.status == RunStatus::Running && !lockedOk && w_ <= 0.5f))
    endRun();
  if (hasPending_ && run_.status != RunStatus::Running) doSwitch(pending_);

  // U9. Entering: in on the next whole dance beat, unless the weight is
  // falling through 0.5 (target <= 0.5): then the beat after.
  if (run_.status != RunStatus::Running && lockedOk && w_ > 0.5f && character_) {
    if (!entering_) {
      entering_ = true;
      enterAt_ = static_cast<int64_t>(std::floor(clock_)) + 1;
    }
    if (clock_ >= static_cast<double>(enterAt_)) {
      if (target > 0.5f) {
        if (run_.status == RunStatus::Held) endRun();
        if (!phraseValid_) {
          A_ = enterAt_;
          phraseValid_ = true;
        }
        pick(enterAt_);
        entering_ = false;
      } else {
        enterAt_ = static_cast<int64_t>(std::floor(clock_)) + 1;
      }
    }
  } else {
    entering_ = false;
  }

  // U10. The idle run.
  idleStep(heard, static_cast<double>(dt));

  // U11. The cue.
  const int8_t freezeK = in.freeze >= 0 && in.freeze <= 15 ? in.freeze : -1;
  buildCue(heard, gridOk, lockedOk, freezeK);
  lastShowDance_ = base_.showDance;
  lastEnergy_ = base_.energy;
  return cue_;
}

}  // namespace dance
