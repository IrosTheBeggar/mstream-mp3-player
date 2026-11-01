// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the Choreographer (DanceChoreo) and B.7's pace rules in
// DanceRules.h: docs/DANCE-SPRITES.md, section 3 and Appendix E (E.2's
// behaviour, E.4's test_dance_choreo list, item by item). Written from the
// doc alone, without reading the implementation.
//
// How it works:
// - Every scenario drives a Twin: two Choreographers built afresh and fed
//   the same calls and inputs, their cues, runs, clocks, latches and prevK
//   compared with == after every update (E.3: identical inputs give
//   identical cues on one build). So every script below runs twice.
// - The Twin also checks E.2.7's rules on every frame that no freeze
//   overrides: the state from the inputs and the run, showDance, the beat
//   light, the energy (e16 of the weight, or the held run's), the weight's
//   per-frame bound, the spot's root, the beat fields against run() (u
//   within the run, beatPos16 = floor(u * 786432), the play's orientation),
//   and the idle fields. At each new run it checks E.2.5: the octave against
//   eligibleK with the prevK it had, the play count and the run's length in
//   whole dance beats, the hard rules (kind, needs, genre, orientation,
//   travel's start spots), the soft rules at level 0, the no-repeat rule
//   below level 2, and forced runs at level 4.
// - Inputs come from beatscript::Script (test/support/dance/BeatScript.h):
//   a scripted tracker at 24 or 30 fps.
// - Statistical checks use picks with no history (the first after a track
//   reset, or the first after a forced run), so their shares are exact:
//   E.4's 3/5, 100/115, 4/5 and 2/3, each over 2,000 draws within 0.05.
//
// Run: pio test -e native -f test_dance_choreo (and native_asan, E.4).
#if defined(DANCE_ASAN) && !defined(__SANITIZE_ADDRESS__)
#error "native_asan: -fsanitize=address didn't reach the compiler (docs/DANCE-SPRITES.md, Appendix D.5)"
#endif

#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "../support/dance/BeatScript.h"
#include "../support/dance/EligibilityVectors.h"
#include "../support/dance/FoldVectors.h"
#include "DanceCharacter.h"
#include "DanceChoreo.h"
#include "DancePose.h"
#include "DanceRules.h"
#include "DanceStage.h"
#include "DrawnCharacter.h"
#include "Fnv1a.h"

using beatscript::CardDancer;
using beatscript::Script;
using beatscript::ScriptCharacter;
using dance::BeatInput;
using dance::Cue;
using dance::DanceState;
using dance::kAutoBounce;
using dance::kAutoIdle;
using dance::kNoMove;
using dance::kNoOctave;
using dance::RunInfo;
using dance::RunStatus;

namespace {

char g_ctx[320] = "";
void ctx(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(g_ctx, sizeof g_ctx, fmt, ap);
  va_end(ap);
}

// A failure message: the scenario's context, then this check's.
const char* msg(const char* fmt, ...) {
  static char buf[640];
  int n = std::snprintf(buf, sizeof buf, "%s: ", g_ctx);
  if (n < 0) n = 0;
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf + n, sizeof buf - static_cast<size_t>(n), fmt, ap);
  va_end(ap);
  return buf;
}

// Checks whose message is formatted only when they fail: the Twin runs them
// on every frame of every scenario.
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) TEST_FAIL_MESSAGE(msg(__VA_ARGS__));     \
  } while (0)
const char* withValues(const char* m, long long want, long long got) {
  static char buf[720];
  std::snprintf(buf, sizeof buf, "%s (want %lld, got %lld)", m, want, got);
  return buf;
}
#define CHECK_EQ(want, got, ...)                                                          \
  do {                                                                                    \
    const long long w_ = static_cast<long long>(want), g_ = static_cast<long long>(got); \
    if (w_ != g_) TEST_FAIL_MESSAGE(withValues(msg(__VA_ARGS__), w_, g_));                \
  } while (0)

const char* stateName(DanceState s) {
  switch (s) {
    case DanceState::Quiet: return "Quiet";
    case DanceState::Listening: return "Listening";
    case DanceState::Entering: return "Entering";
    case DanceState::Dancing: return "Dancing";
    case DanceState::Leaving: return "Leaving";
    case DanceState::Freeze: return "Freeze";
  }
  return "?";
}

constexpr int kSpotX[3] = {64, 160, 256};

// E.2.1's e16(w), written again.
uint32_t refE16(float w) {
  float c = (w - 0.5f) * 2.0f;
  if (c < 0.0f) c = 0.0f;
  if (c > 1.0f) c = 1.0f;
  return static_cast<uint32_t>(39322 + static_cast<int32_t>(std::floor(26214.0 * static_cast<double>(c) + 0.5)));
}

int mod8(int64_t x) { return static_cast<int>(x - 8 * static_cast<int64_t>(std::floor(static_cast<double>(x) / 8.0))); }

// D2: a play's length in half dance beats (E.2.1).
int d2Of(int beats, int k) { return k > 0 ? beats : (k == 0 ? 2 * beats : 4 * beats); }

bool ranged(const dance::MoveLabels& l) { return !(l.paceLo == 0 && l.paceHi == 0); }

// E.2.5's run length.
int expectPlays(const dance::MoveLabels& l, int k) {
  if (l.travel != 0) return 1;
  const int d2 = d2Of(l.beats, k);
  if (l.repeat > 0) {
    int n = l.repeat;
    while ((n * d2) % 2) ++n;
    return n;
  }
  return std::max(1, 8 / d2);
}

bool sameCue(const Cue& a, const Cue& b) {
  return a.beatMove == b.beatMove && a.u == b.u && a.beatPos16 == b.beatPos16 && a.k == b.k &&
         a.beatMirror == b.beatMirror && a.idleMove == b.idleMove && a.idleMs == b.idleMs &&
         a.idleMirror == b.idleMirror && a.idleSeconds == b.idleSeconds && a.weight == b.weight &&
         a.energy == b.energy && a.showDance == b.showDance && a.flash == b.flash && a.spot == b.spot &&
         a.rootX == b.rootX && a.rootY == b.rootY && a.state == b.state;
}
bool sameRun(const RunInfo& a, const RunInfo& b) {
  return a.move == b.move && a.k == b.k && a.plays == b.plays && a.firstMirror == b.firstMirror && a.spot == b.spot &&
         a.phrase == b.phrase && a.level == b.level && a.cut == b.cut && a.status == b.status && a.start == b.start &&
         a.end == b.end && a.uEnd == b.uEnd;
}

// E.2.7's state, from the inputs, the run and the weight.
DanceState expectState(bool heard, bool lockedOk, RunStatus status, float w) {
  if (!heard) return status != RunStatus::None ? DanceState::Freeze : DanceState::Quiet;
  if (status == RunStatus::Running && lockedOk) return DanceState::Dancing;
  if (status != RunStatus::None && !lockedOk) return DanceState::Leaving;
  if (lockedOk && w > 0.5f) return DanceState::Entering;
  return DanceState::Listening;
}

struct Rec {
  BeatInput in;
  Cue cue;
  RunInfo run;
  double clock;
  int t10;
  int8_t prevK;
  const dance::Character* ch;
  bool frozen;
};

struct RunRec {
  size_t frame;     // the Twin's update count at its first frame
  RunInfo first;    // run() at its first frame
  RunInfo last;     // run() at its last frame as the current run
  int8_t prevKBefore;
  int t10;
  int64_t clockFloor;  // floor(danceClock()) at its first frame
};

// Two Choreographers fed the same calls; see the header comment.
class Twin {
 public:
  bool checks = true;   // E.2.7's per-frame rules and E.2.5's per-run rules
  bool keepLog = true;  // every update's Rec in log
  std::vector<Rec> log;
  std::vector<RunRec> runs;
  size_t updates = 0;

  void reset(uint32_t pathHash) {
    a_.reset(pathHash);
    b_.reset(pathHash);
    compareCalls();
    historyCleared_ = true;
    noKeepCheck_ = true;
    mf_ = false;  // reset: every state back but the character (Mf included, E.2.2)
    forced_ = kNoMove;
    trackKeys_.clear();
    injected_.clear();
    pathHash_ = pathHash;
    havePrev_ = false;
    w_ = 0.0f;
    runBroken_ = true;
    syncCharacter();
  }
  void onTrack(uint32_t pathHash, const uint32_t* keys, int count) {
    a_.onTrack(pathHash, keys, count);
    b_.onTrack(pathHash, keys, count);
    trackKeys_.clear();
    for (int i = 0; i < count && i < 16; ++i) trackKeys_.push_back(keys[i]);
    injected_.clear();
    pathHash_ = pathHash;
  }
  void syncPath(uint32_t pathHash) {
    a_.syncPath(pathHash);
    b_.syncPath(pathHash);
    if (pathHash == pathHash_) return;
    trackKeys_.clear();
    injected_.clear();
    pathHash_ = pathHash;
  }
  void setCharacter(const dance::Character* c) {
    a_.setCharacter(c);
    b_.setCharacter(c);
    compareCalls();
    noKeepCheck_ = true;
    if (a_.run().status == RunStatus::None) runBroken_ = true;
    syncCharacter();
  }
  void freeze(int move, uint32_t pos16, bool mirrored) {
    a_.freeze(move, pos16, mirrored);
    b_.freeze(move, pos16, mirrored);
    const dance::Character* c = a_.character();
    mf_ = move == kAutoBounce || move == kAutoIdle || (c && move >= 0 && move < c->moveCount());
  }
  void forceNext(int move) {
    a_.forceNext(move);
    b_.forceNext(move);
    forced_ = move;
  }
  void setSpot(int spot) {
    a_.setSpot(spot);
    b_.setSpot(spot);
    compareCalls();
    runBroken_ = true;  // setSpot ends the run
  }
  void addGenre(uint32_t key) {
    a_.addGenre(key);
    b_.addGenre(key);
    if (injected_.size() < 4) injected_.push_back(key);
  }

  const Cue& update(const BeatInput& in) {
    const int8_t prevKBefore = a_.prevK();
    const bool restarted = !havePrev_ || in.restarts != prev_.in.restarts;
    const Cue& ca = a_.update(in);
    const Cue& cb = b_.update(in);
    ++updates;
    CHECK(sameCue(ca, cb), "update %u: the two cues differ", static_cast<unsigned>(updates));
    compareCalls();
    Rec r;
    r.in = in;
    r.cue = ca;
    r.run = a_.run();
    r.clock = a_.danceClock();
    r.t10 = a_.t10();
    r.prevK = a_.prevK();
    r.ch = a_.character();
    if (r.ch != ch_) {  // a switch inside update (U7, U8): Mf off, history cleared
      mf_ = false;
      forced_ = kNoMove;
      historyCleared_ = true;
      noKeepCheck_ = true;
      ch_ = r.ch;
    }
    if (restarted) {
      historyCleared_ = true;
      noKeepCheck_ = true;
    }
    r.frozen = (in.freeze >= 0 && in.freeze <= 15) || mf_;
    stepWeight(in);
    stepShow(r);
    if (checks) checkFrame(r);
    trackRuns(r, restarted ? kNoOctave : prevKBefore);
    if (keepLog) log.push_back(r);
    prev_ = r;
    havePrev_ = true;
    noKeepCheck_ = false;
    return a_.cue();
  }

  const Cue& cue() const { return a_.cue(); }
  const RunInfo& run() const { return a_.run(); }
  const dance::Character* character() const { return a_.character(); }
  int t10() const { return a_.t10(); }
  int8_t prevK() const { return a_.prevK(); }
  double danceClock() const { return a_.danceClock(); }
  int8_t eligibleNow(int move) const { return a_.eligibleNow(move); }
  dance::Choreographer& a() { return a_; }
  const Rec& last() const { return prev_; }
  int runCount() const { return static_cast<int>(runs.size()); }
  float weight() const { return w_; }         // E.2.3's w, also under a freeze
  float target() const { return target_; }    // its target on the last update
  bool showLatch() const { return show_; }    // E.2.7's latch, as modelled

 private:
  dance::Choreographer a_, b_;
  Rec prev_{};
  bool havePrev_ = false;
  // E.2.3's weight and E.2.7's show latch, written again from the inputs and
  // run(): the cue's weight is overridden under a freeze, the latch not seen.
  float w_ = 0.0f, target_ = 0.0f;
  bool show_ = false;
  double prevRaw_ = -1.0;
  bool runBroken_ = true;  // a call ended the run since the last update
  bool mf_ = false;
  int forced_ = kNoMove;
  bool historyCleared_ = true;
  bool noKeepCheck_ = true;
  const dance::Character* ch_ = nullptr;
  std::vector<uint32_t> trackKeys_, injected_;
  uint32_t pathHash_ = 0;  // the hash the two keep (Choreographer(): as after reset(0))
  // The history E.2.5's no-repeat rule reads, from the runs seen.
  int lastMove_ = kNoMove, lastPlays_ = 0;
  bool lastFirstMirror_ = false;
  int cutExempt_ = kNoMove;

  // U1 and U3: dt 0 unless above 0, at most 1,000 s; a grid with unusable
  // beats or tempo is no grid; confidence clamped to 0..1 (NaN to 0).
  void stepWeight(const BeatInput& in) {
    float dt = in.dt > 0.0f ? in.dt : 0.0f;
    if (!(dt <= 1000.0f)) dt = 1000.0f;
    const bool usable = std::fabs(in.trackerBeats) < 1099511627776.0 && in.rawBpm >= 0.0f && in.rawBpm <= 1000.0f;
    const bool lockedOk = in.heard && in.grid && usable && in.locked;
    float c = in.confidence;
    c = c >= 0.0f ? (c <= 1.0f ? c : 1.0f) : 0.0f;
    target_ = lockedOk ? dance::danceWeight(c) : 0.0f;
    w_ += (target_ - w_) * (1.0f - std::exp(-dt / 0.4f));
  }

  // E.2.7's latch: a new run's first frame sets it to w > 0.5; a running
  // frame where a new play starts sets it to w > 0.5; one crossing any other
  // whole move beat turns it on when w > 0.5; a held run keeps it.
  void stepShow(const Rec& r) {
    const RunInfo& run = r.run;
    const bool fresh = run.status != RunStatus::None &&
                       (runBroken_ || !havePrev_ || prev_.run.status == RunStatus::None ||
                        prev_.run.start != run.start || prev_.run.move != run.move);
    runBroken_ = false;
    if (run.status == RunStatus::None) return;
    if (fresh) {
      show_ = w_ > 0.5f;
      prevRaw_ = -1.0;
    }
    if (run.status != RunStatus::Running) return;
    const double scale = run.k > 0 ? 2.0 : (run.k < 0 ? 0.5 : 1.0);
    double raw = (r.clock - static_cast<double>(run.start)) * scale;
    if (raw > run.uEnd) raw = run.uEnd;
    if (raw < 0.0) raw = 0.0;
    int beats = 1;
    if (r.ch && run.move >= 0 && run.move < r.ch->moveCount() && r.ch->move(run.move).beats > 0)
      beats = r.ch->move(run.move).beats;
    if (prevRaw_ >= 0.0) {
      const double play = std::floor(raw / beats);
      if (play > std::floor(prevRaw_ / beats) && play < run.plays) {
        show_ = w_ > 0.5f;
      } else if (std::floor(raw) > prevRaw_ && w_ > 0.5f) {
        show_ = true;
      }
    }
    prevRaw_ = raw;
  }

  void syncCharacter() {
    if (a_.character() != ch_) {
      ch_ = a_.character();
      mf_ = false;
      forced_ = kNoMove;
      historyCleared_ = true;
    }
  }
  void compareCalls() {
    CHECK(sameCue(a_.cue(), b_.cue()), "the two cues differ");
    CHECK(sameRun(a_.run(), b_.run()), "the two runs differ");
    CHECK(a_.danceClock() == b_.danceClock(), "the two clocks differ");
    CHECK(a_.t10() == b_.t10() && a_.prevK() == b_.prevK(), "the two latches differ");
    CHECK(a_.character() == b_.character(), "the two characters differ");
  }

  bool genreOk(const dance::MoveLabels& l) const {
    if (l.genreCount == 0) return true;
    for (int g = 0; g < l.genreCount; ++g) {
      for (uint32_t k : trackKeys_)
        if (k == l.genreKeys[g]) return true;
      for (uint32_t k : injected_)
        if (k == l.genreKeys[g]) return true;
    }
    return false;
  }

  void checkFrame(const Rec& r) {
    const Cue& c = r.cue;
    const RunInfo& run = r.run;
    // U3: a grid whose beats or tempo can't be used is no grid.
    const bool usable =
        std::fabs(r.in.trackerBeats) < 1099511627776.0 && r.in.rawBpm >= 0.0f && r.in.rawBpm <= 1000.0f;
    const bool heard = r.in.heard, gridOk = heard && r.in.grid && usable, lockedOk = gridOk && r.in.locked;
    const unsigned u = static_cast<unsigned>(updates);
    CHECK(c.spot >= -1 && c.spot <= 1, "update %u: spot %d", u, c.spot);
    CHECK_EQ(kSpotX[c.spot + 1], c.rootX, "update %u: rootX", u);
    CHECK_EQ(153, c.rootY, "update %u: rootY", u);
    CHECK(c.idleSeconds >= 0.0f && c.idleSeconds < 2000.0f, "update %u: idleSeconds", u);
    // E.2.3: the clock stays finite and under 2^52 dance beats whatever the input.
    CHECK(std::fabs(r.clock) < 4503599627370496.0, "update %u: clock %g", u, r.clock);
    if (r.frozen) return;
    if (havePrev_ && !prev_.frozen) {
      const float dt = r.in.dt > 0.0f ? r.in.dt : 0.0f;
      const float bound = 1.0f - std::exp(-dt / 0.4f) + 1e-6f;
      CHECK(std::fabs(c.weight - prev_.cue.weight) <= bound, "update %u: the weight moved %g in one frame", u,
            c.weight - prev_.cue.weight);
    }
    CHECK(c.weight >= 0.0f && c.weight <= 1.0f, "update %u: weight %g", u, c.weight);
    CHECK(c.weight == w_, "update %u: weight %.9g, U3 gives %.9g", u, c.weight, w_);
    CHECK_EQ(run.status != RunStatus::None && show_, c.showDance,
             "update %u: showDance (run status %d, w %g, latch %d)", u, static_cast<int>(run.status), c.weight,
             show_);
    // U8 ends a held run, or a running one without a lock, at w <= 0.5: beat
    // frames at w <= 0.5 come only from the latch under a locked grid.
    if (c.showDance && c.weight <= 0.5f)
      CHECK(lockedOk && run.status == RunStatus::Running, "update %u: beat frames at w %g without a lock", u,
            c.weight);
    const bool flash = gridOk && c.weight > 0.5f && (r.clock - std::floor(r.clock)) < 0.12;
    CHECK_EQ(flash, c.flash, "update %u: flash (clock %.6f, w %g)", u, r.clock, c.weight);
    if (run.status == RunStatus::Held) {
      if (havePrev_ && !prev_.frozen)
        CHECK_EQ(prev_.cue.energy, c.energy, "update %u: a held run's energy", u);
    } else {
      CHECK_EQ(refE16(c.weight), c.energy, "update %u: e16(%g)", u, c.weight);
    }
    const DanceState want = expectState(heard, lockedOk, run.status, c.weight);
    CHECK(want == c.state, "update %u: state %s, want %s (heard %d, grid %d, locked %d, run %d, "
                                                  "w %g)",
                                                  u, stateName(c.state), stateName(want), heard, r.in.grid,
                                                  r.in.locked, static_cast<int>(run.status), c.weight);
    if (run.status != RunStatus::None) {
      CHECK_EQ(run.move, c.beatMove, "update %u: beatMove", u);
      CHECK_EQ(run.k, c.k, "update %u: k", u);
      CHECK_EQ(run.spot, c.spot, "update %u: the running move's start spot", u);
      CHECK(c.u >= 0.0 && c.u <= run.uEnd, "update %u: u %.6f outside 0..%.6f", u, c.u, run.uEnd);
      if (run.status == RunStatus::Held)
        CHECK(c.u == run.uEnd, "update %u: held at u %.6f, uEnd %.6f", u, c.u, run.uEnd);
      CHECK_EQ(static_cast<uint32_t>(std::floor(c.u * 786432.0)), c.beatPos16, "update %u: beatPos16 at u %.9f", u,
               c.u);
      bool mirror = false;
      if (r.ch && run.move >= 0 && run.move < r.ch->moveCount()) {
        const dance::MoveLabels& l = r.ch->move(run.move);
        if (l.flags & dance::kMirror) {
          const int j = std::min(static_cast<int>(std::floor(c.u / l.beats)), run.plays - 1);
          mirror = run.firstMirror != ((j & 1) != 0);
        }
      }
      CHECK_EQ(mirror, c.beatMirror, "update %u: beatMirror at u %.4f", u, c.u);
    } else if (havePrev_ && !noKeepCheck_ && !prev_.frozen && prev_.run.status != RunStatus::None) {
      // None: the beat fields keep their last values (E.2.7).
      CHECK_EQ(prev_.cue.beatMove, c.beatMove, "update %u: an ended run's beatMove", u);
      CHECK(prev_.cue.u == c.u, "update %u: an ended run's u", u);
    }
    if (r.ch) {
      const bool idle = c.idleMove == kAutoIdle ||
                        (c.idleMove >= 0 && c.idleMove < r.ch->moveCount() &&
                         r.ch->move(c.idleMove).kind == dance::MoveKind::Idle && r.ch->move(c.idleMove).needs == 0);
      CHECK(idle, "update %u: idleMove %d isn't an idle move", u, c.idleMove);
      if (c.idleMirror)
        CHECK(c.idleMove >= 0 && (r.ch->move(c.idleMove).flags & dance::kMirror),
              "update %u: a mirrored idle without kMirror", u);
    } else {
      CHECK_EQ(kNoMove, c.idleMove, "update %u: no character, no idle", u);
    }
  }

  // New runs: E.2.5's rules, then the history for the next one.
  void trackRuns(const Rec& r, int8_t prevKBefore) {
    const RunInfo& run = r.run;
    if (!runs.empty() && run.status != RunStatus::None && run.start == runs.back().first.start &&
        run.move == runs.back().first.move)
      runs.back().last = run;
    const bool isNew = run.status != RunStatus::None &&
                       (runs.empty() || run.start != runs.back().first.start || run.move != runs.back().first.move ||
                        !havePrev_ || prev_.run.status == RunStatus::None);
    if (!isNew) return;
    // The history the pick read: cleared by a track reset or a switch;
    // otherwise the last run's.
    if (!runs.empty() && !historyCleared_) {
      const RunInfo& p = runs.back().last;
      lastMove_ = p.move;
      lastPlays_ = p.plays;
      lastFirstMirror_ = p.firstMirror;
      cutExempt_ = p.cut ? p.move : kNoMove;
    } else {
      lastMove_ = kNoMove;
      lastPlays_ = 0;
      lastFirstMirror_ = false;
      cutExempt_ = kNoMove;
    }
    historyCleared_ = false;
    RunRec rr;
    rr.frame = updates;
    rr.first = run;
    rr.last = run;
    rr.prevKBefore = prevKBefore;
    rr.t10 = r.t10;
    rr.clockFloor = static_cast<int64_t>(std::floor(r.clock));
    runs.push_back(rr);
    const int forced = forced_;
    forced_ = kNoMove;
    if (!checks) return;
    const unsigned u = static_cast<unsigned>(updates);
    CHECK(run.phrase >= 0 && run.phrase <= 7, "update %u: phrase %d", u, run.phrase);
    CHECK(run.level <= 4, "update %u: level %d", u, run.level);
    CHECK(!(run.cut), "update %u: a new run is cut", u);
    CHECK(run.k >= -1 && run.k <= 1, "update %u: k %d", u, run.k);
    CHECK_EQ(r.cue.spot, run.spot, "update %u: a run starts at the current spot", u);
    CHECK(r.clock >= static_cast<double>(run.start), "update %u: a run starting ahead", u);
    CHECK(r.t10 > 0, "update %u: a pick without a latched tempo", u);
    CHECK(w_ > 0.5f, "update %u: a run started at w %g (U7 and U9 pick only above 0.5)", u, w_);
    if (run.move == kAutoBounce) {
      CHECK_EQ(3, run.level, "update %u: auto-bounce's level", u);
      CHECK_EQ(0, run.k, "update %u: auto-bounce's k", u);
      CHECK_EQ(4, run.plays, "update %u: auto-bounce's plays", u);
      CHECK(!(run.firstMirror), "update %u: auto-bounce mirrored", u);
      CHECK(run.end - run.start == 4 && run.uEnd == 4.0, "update %u: auto-bounce's length", u);
      CHECK_EQ(prevKBefore, r.prevK, "update %u: auto-bounce keeps prevK", u);
      CHECK(forced == kNoMove || !isBeatMove(r.ch, forced), "update %u: auto-bounce instead of the forced move", u);
      return;
    }
    CHECK((r.ch) != nullptr, "update %u: a run without a character", u);
    CHECK(run.move >= 0 && run.move < r.ch->moveCount(), "update %u: move %d", u, run.move);
    const dance::MoveLabels& l = r.ch->move(run.move);
    CHECK(l.kind == dance::MoveKind::Beat, "update %u: an idle move as a beat run", u);
    const int d2 = d2Of(l.beats, run.k);
    CHECK_EQ(expectPlays(l, run.k), run.plays, "update %u: %s's plays at k %d", u, l.name, run.k);
    CHECK_EQ(0, (run.plays * d2) % 2, "update %u: not whole dance beats", u);
    CHECK(run.end - run.start == run.plays * d2 / 2, "update %u: %s runs %lld dance beats, want %d", u, l.name,
                                 static_cast<long long>(run.end - run.start), run.plays * d2 / 2);
    CHECK(run.uEnd == static_cast<double>(run.plays * l.beats), "update %u: uEnd", u);
    const bool o = run.firstMirror;
    const int spotBit = 1 << (run.spot + 1);
    if (isBeatMove(r.ch, forced)) {
      // Mm (E.2.5): the forced move at level 4, k its eligibleK or 0.
      CHECK_EQ(forced, run.move, "update %u: the forced move", u);
      CHECK_EQ(4, run.level, "update %u: a forced run's level", u);
      const int8_t k = ranged(l) ? dance::eligibleK(r.t10, l.paceLo, l.paceHi, prevKBefore, l.beats, l.travel) : 0;
      CHECK_EQ(k == kNoOctave ? 0 : k, run.k, "update %u: a forced run's k", u);
      if (o) {
        CHECK((l.flags & dance::kMirror) && (r.ch->startSpotMask(run.move, true) & spotBit),
              "update %u: a forced run mirrored", u);
      }
    } else {
      CHECK(run.level <= 2, "update %u: level %d without a forced move", u, run.level);
      CHECK_EQ(0, l.needs, "update %u: a move with a needs bit", u);
      CHECK(genreOk(l), "update %u: %s's genres don't match", u, l.name);
      const int8_t k = ranged(l) ? dance::eligibleK(r.t10, l.paceLo, l.paceHi, prevKBefore, l.beats, l.travel) : 0;
      CHECK(k != kNoOctave, "update %u: %s isn't eligible at t10 %d", u, l.name, r.t10);
      CHECK_EQ(k, run.k, "update %u: %s's k (t10 %d, prevK %d)", u, l.name, r.t10, prevKBefore);
      // allowed(i): the orientation, and a travel move's start spot.
      if (o) CHECK(l.flags & dance::kMirror, "update %u: mirrored without kMirror", u);
      if (l.travel != 0)
        CHECK(r.ch->startSpotMask(run.move, o) & spotBit, "update %u: %s travels from spot %d", u, l.name, run.spot);
      const int p = run.phrase;
      if (run.level == 0) {
        const bool aligned = d2 < 2 || (2 * p) % std::min(d2, 8) == 0;
        CHECK(aligned, "update %u: %s (D2 %d) at p %d, level 0", u, l.name, d2, p);
        if (l.flags & dance::kPlaceStart) CHECK_EQ(0, p, "update %u: place = start", u);
        if (l.flags & dance::kPlaceEnd)
          CHECK((d2 == 2 && (p == 6 || p == 7)) || (d2 == 4 && p == 6), "update %u: place = end at D2 %d, p %d", u,
                d2, p);
        if (l.travel != 0) CHECK(p == 0 || p == 4, "update %u: travel at p %d", u, p);
      }
      if (run.level <= 1 && run.move == lastMove_) {
        const bool twin = lastPlays_ == 1 && (l.flags & dance::kMirror) && o == !lastFirstMirror_;
        CHECK(run.move == cutExempt_ || twin, "update %u: %s repeated at level %d (last plays %d)", u, l.name,
              run.level,
                                     lastPlays_);
      }
    }
    // prevK: the k of the last ranged run.
    CHECK_EQ(ranged(l) ? run.k : prevKBefore, r.prevK, "update %u: prevK after the pick", u);
  }

  static bool isBeatMove(const dance::Character* c, int i) {
    return c && i >= 0 && i < c->moveCount() && c->move(i).kind == dance::MoveKind::Beat;
  }
};

// --- driving -------------------------------------------------------------------

// One frame.
const Cue& step(Twin& ch, Script& s) { return ch.update(s.frame()); }
// Frames for `seconds` of the script's clock.
void feed(Twin& ch, Script& s, double seconds) {
  const long n = std::lround(seconds / static_cast<double>(s.dt()));
  for (long i = 0; i < n; ++i) step(ch, s);
}
// Frames until pred() holds after one (true), or maxFrames (false).
bool until(Twin& ch, Script& s, const std::function<bool()>& pred, int maxFrames) {
  for (int i = 0; i < maxFrames; ++i) {
    step(ch, s);
    if (pred()) return true;
  }
  return false;
}
// Frames until a new run starts (true), or maxFrames.
bool untilNewRun(Twin& ch, Script& s, int maxFrames = 2000) {
  const int n = ch.runCount();
  return until(ch, s, [&] { return ch.runCount() > n; }, maxFrames);
}

uint32_t pathHashOf(int i) {
  char path[48];
  std::snprintf(path, sizeof path, "/music/track-%05d.mp3", i);
  return packedit::refFnv1a32(path, std::strlen(path));
}

// The fixture as a card pack, kept for the whole run.
CardDancer& fixture() {
  static CardDancer* d = nullptr;
  if (!d) {
    d = new CardDancer();
    TEST_ASSERT_TRUE_MESSAGE(d->load(packedit::testPack()), "the fixture loads as a card pack");
  }
  return *d;
}

constexpr uint32_t kHipHop = 0x5DE39FA5u, kRandB = 0x1BDE7F5Cu, kDnB = 0xE6318A55u;

}  // namespace

void setUp() {}
void tearDown() {}

// ===========================================================================
// 1. B.7 in C++: eligibleK and paceHolds against EligibilityVectors.h and
//    B.7's tables.

void test_b7_in_cpp() {
  using namespace dancevec;
  TEST_ASSERT_EQUAL_INT(127, kNoOctave);
  TEST_ASSERT_EQUAL_INT(kNoK, kNoOctave);
  TEST_ASSERT_EQUAL_INT(3696, kEligibilityCount);
  for (int i = 0; i < kEligibilityCount; ++i) {
    const Eligibility& e = kEligibility[i];
    const int8_t k = dance::eligibleK(e.t10, e.lo, e.hi, e.prevK, e.beats, e.travel);
    if (k != e.k) {
      ctx("kEligibility[%d]: t10 %u, %u-%u, prevK %d, %u beats, travel %d: want %d, got %d", i, e.t10, e.lo, e.hi,
          e.prevK, e.beats, e.travel, e.k, k);
      TEST_FAIL_MESSAGE(g_ctx);
    }
  }
  TEST_ASSERT_EQUAL_INT(585, kPaceHoldsCount);
  for (int i = 0; i < kPaceHoldsCount; ++i) {
    const PaceHolds& p = kPaceHolds[i];
    const bool holds = dance::paceHolds(p.t10, p.k, p.lo, p.hi);
    if (holds != (p.holds != 0)) {
      ctx("kPaceHolds[%d]: t10 %u, k %d, %u-%u: want %u", i, p.t10, p.k, p.lo, p.hi, p.holds);
      TEST_FAIL_MESSAGE(g_ctx);
    }
  }
  // B.7's eligible_k table.
  struct Row {
    int t10, lo, hi;
    int8_t prevK;
    int beats, travel;
    int8_t k;
  };
  const Row rows[] = {
      {1200, 90, 130, kNoOctave, 4, 0, 0},
      {870, 150, 180, kNoOctave, 1, 0, 1},
      {870, 150, 180, kNoOctave, 8, 0, 1},
      {1740, 36, 60, kNoOctave, 4, 0, kNoOctave},
      {1200, 36, 200, 1, 2, 0, 0},
      {1200, 36, 200, -1, 2, 0, -1},
      {950, 170, 180, kNoOctave, 2, 0, kNoOctave},
      {900, 170, 180, kNoOctave, 2, 0, 1},
      {1760, 0, 0, 1, 2, 0, 0},
      {1200, 60, 100, kNoOctave, 4, 0, -1},
      {1200, 60, 100, kNoOctave, 8, 0, kNoOctave},
      {800, 72, 180, 1, 2, 1, 0},
      {720, 100, 150, kNoOctave, 2, 1, kNoOctave},
      {720, 100, 150, kNoOctave, 4, 1, 1},
  };
  for (const Row& r : rows) {
    ctx("B.7: t10 %d, %d-%d, prevK %d, %d beats, travel %d", r.t10, r.lo, r.hi, r.prevK, r.beats, r.travel);
    TEST_ASSERT_EQUAL_INT_MESSAGE(r.k, dance::eligibleK(r.t10, r.lo, r.hi, r.prevK, r.beats, r.travel), g_ctx);
  }
  // prevK outside -1..1 counts as none; travel is "nonzero".
  TEST_ASSERT_EQUAL_INT(0, dance::eligibleK(1200, 36, 200, 5, 2, 0));
  TEST_ASSERT_EQUAL_INT(0, dance::eligibleK(1200, 36, 200, -2, 2, 0));
  TEST_ASSERT_EQUAL_INT(-1, dance::eligibleK(1200, 36, 200, -1, 2, 0));
  TEST_ASSERT_EQUAL_INT(-1, dance::eligibleK(1200, 60, 100, 2, 4, 0));
  TEST_ASSERT_EQUAL_INT(kNoOctave, dance::eligibleK(720, 100, 150, kNoOctave, 2, -1));
  TEST_ASSERT_EQUAL_INT(1, dance::eligibleK(720, 100, 150, kNoOctave, 4, -2));
  // pace_holds' examples: 100-100 at k = 0; 170-180 at k = +1, never past 180.
  TEST_ASSERT_TRUE(dance::paceHolds(970, 0, 100, 100));
  TEST_ASSERT_TRUE(dance::paceHolds(1030, 0, 100, 100));
  TEST_ASSERT_FALSE(dance::paceHolds(969, 0, 100, 100));
  TEST_ASSERT_FALSE(dance::paceHolds(1031, 0, 100, 100));
  TEST_ASSERT_TRUE(dance::paceHolds(900, 1, 170, 180));
  TEST_ASSERT_FALSE(dance::paceHolds(901, 1, 170, 180));
  TEST_ASSERT_FALSE(dance::paceHolds(927, 1, 170, 180));
  // k outside -1..1: false for a ranged move, true for an unranged one.
  const int8_t badKs[3] = {2, -2, kNoOctave};
  for (int8_t k : badKs) {
    TEST_ASSERT_FALSE(dance::paceHolds(1200, k, 36, 200));
    TEST_ASSERT_TRUE(dance::paceHolds(1200, k, 0, 0));
  }
  // B.7's reach table (DanceRules.h, step 2), here too.
  struct Reach {
    int lo, hi, beats, travel;
    bool any;
    int rlo, rhi;
  };
  const Reach reaches[] = {
      {0, 0, 1, 0, true, 72, 176},    {0, 0, 2, 0, true, 72, 176},   {0, 0, 4, 0, true, 72, 176},
      {0, 0, 8, 0, true, 72, 176},    {0, 0, 1, 1, false, 0, 0},     {40, 80, 1, 1, true, 40, 80},
      {90, 130, 4, 0, true, 90, 130}, {36, 60, 4, 0, true, 36, 60},  {36, 60, 8, 0, false, 0, 0},
      {185, 200, 2, 0, false, 0, 0},  {178, 200, 2, 1, false, 0, 0}, {100, 200, 4, 1, true, 100, 180},
      {40, 200, 8, 0, true, 72, 180},
  };
  for (const Reach& r : reaches) {
    int lo = -1, hi = -1;
    ctx("reach %d-%d, %d beats, travel %d", r.lo, r.hi, r.beats, r.travel);
    TEST_ASSERT_EQUAL_MESSAGE(r.any, dance::reach(r.lo, r.hi, r.beats, r.travel != 0, &lo, &hi), g_ctx);
    if (r.any) {
      TEST_ASSERT_EQUAL_INT_MESSAGE(r.rlo, lo, g_ctx);
      TEST_ASSERT_EQUAL_INT_MESSAGE(r.rhi, hi, g_ctx);
    }
  }
}

// ===========================================================================
// 2. The latch (2.4, U5).

namespace {
// A re-acquire at `bpm` in phase with the current grid: tracker beat K, the
// last whole one, keeps its time.
void reacquireInPhase(Script& s, double bpm) {
  const double K = std::floor(s.beatsAt(s.trackTime()));
  s.acquire(bpm, s.timeOfBeat(K), static_cast<int32_t>(K));
}

BeatInput handInput(float rawBpm) {
  BeatInput in{};
  in.heard = true;
  in.grid = true;
  in.locked = true;
  in.confidence = 0.8f;
  in.trackerBeats = 0.0;
  in.rawBpm = rawBpm;
  in.foldScale = 1.0;
  in.restarts = 0;
  in.acquisitions = 1;
  in.dt = 0.0f;
  in.freeze = -1;
  return in;
}
}  // namespace

void test_latch() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  {
    ctx("before the first lock");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.locked(false);
    s.acquire(100.0, 0.0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.t10(), g_ctx);
    for (int i = 0; i < 30; ++i) {
      step(ch, s);
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.t10(), g_ctx);  // an unlocked grid never latches
    }
    s.locked(true);
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1000, ch.t10(), g_ctx);  // the first locked frame
  }
  const struct {
    float bpm;
    int t10;
  } moves[] = {{103.0f, 1000}, {103.5f, 1035}, {97.0f, 1000}, {96.9f, 969}};
  for (const auto& m : moves) {
    ctx("from T = 100 to %.1f", m.bpm);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(100.0, 0.0);
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1000, ch.t10(), g_ctx);  // T latched on the first locked frame
    feed(ch, s, 1.0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1000, ch.t10(), g_ctx);
    reacquireInPhase(s, m.bpm);
    for (int i = 0; i < 45; ++i) {
      step(ch, s);
      TEST_ASSERT_EQUAL_INT_MESSAGE(m.t10, ch.t10(), g_ctx);
    }
  }
  {
    ctx("an unlocked grid, then a restart");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(100.0, 0.0);
    feed(ch, s, 1.0);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1000, ch.t10(), g_ctx);
    s.locked(false);
    reacquireInPhase(s, 120.0);
    for (int i = 0; i < 45; ++i) {
      step(ch, s);
      TEST_ASSERT_EQUAL_INT_MESSAGE(1000, ch.t10(), g_ctx);
    }
    s.locked(true);
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1200, ch.t10(), g_ctx);
    s.restart();
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.t10(), g_ctx);  // the track reset clears it
    feed(ch, s, 0.5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.t10(), g_ctx);
    s.acquire(110.0, s.trackTime());
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1100, ch.t10(), g_ctx);
  }
  // U5's clamp to 720..1760 (inputs whose fold leaves the band), and the
  // rounding floor(Tl * 10 + 0.5).
  const struct {
    float bpm;
    int t10;
  } clamps[] = {{200.0f, 1760}, {60.0f, 720}, {95.75f, 958}, {95.74f, 957}};
  for (const auto& w : clamps) {
    ctx("rawBpm %.2f at S = 1", w.bpm);
    Twin ch;
    ch.setCharacter(&c);
    ch.update(handInput(w.bpm));
    TEST_ASSERT_EQUAL_INT_MESSAGE(w.t10, ch.t10(), g_ctx);
  }
}

// ===========================================================================
// 3. Pace in the pick.

void test_pace_in_the_pick() {
  {
    // Unranged moves at k 0; ranged ones at eligibleK's k, prevK first (the
    // Twin checks every run's k against eligibleK with the prevK it had).
    ScriptCharacter c;
    const int X = c.beatMove("x", 2, 50, 60);   // only half time at T 120
    const int Y = c.beatMove("y", 2, 36, 200);  // 0 or -1: prevK decides
    const int U = c.beatMove("u", 1, 0, 0);
    ctx("prevK at T 120");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 90.0);
    int yAfterHalf = 0, xRuns = 0, uRuns = 0;
    for (const RunRec& r : ch.runs) {
      if (r.first.move == U) {
        ++uRuns;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.first.k, g_ctx);
      }
      if (r.first.move == X) {
        ++xRuns;
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, r.first.k, g_ctx);
      }
      if (r.first.move == Y && r.prevKBefore == -1) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, r.first.k, g_ctx);  // the previous ranged move's octave fits first
        ++yAfterHalf;
      }
    }
    TEST_ASSERT_TRUE_MESSAGE(xRuns > 0 && uRuns > 0 && yAfterHalf > 0, g_ctx);
  }
  {
    // The 180 cap for a running move (10.3): a 170-180 1-beat move danced
    // at +1 from t10 880, then a re-acquire at 90.7 BPM within 1/4 beat:
    // t10 907, 2T = 181.4 is inside the move's 3 % but past 180: cut.
    ScriptCharacter c;
    const int M = c.beatMove("fast", 1, 170, 180);
    ctx("the 180 cap");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(88.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(880, ch.t10(), g_ctx);
    const RunInfo r0 = ch.run();
    TEST_ASSERT_EQUAL_INT_MESSAGE(M, r0.move, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r0.k, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(8, r0.plays, g_ctx);  // 1-beat at +1: 8 plays, 4 dance beats
    TEST_ASSERT_TRUE_MESSAGE(r0.end - r0.start == 4, g_ctx);
    // At S = 1 a dance beat is a tracker beat: re-acquire with beat 0 on
    // dance beat start + 1, one frame after it.
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.danceClock() >= r0.start + 1.0; }, 200), g_ctx);
    s.acquire(90.7, s.timeOfBeat(static_cast<double>(r0.start + 1)));
    step(ch, s);
    TEST_ASSERT_EQUAL_INT_MESSAGE(907, ch.t10(), g_ctx);
    const RunInfo r1 = ch.run();
    TEST_ASSERT_TRUE_MESSAGE(r1.status == RunStatus::Running && r1.start == r0.start && r1.move == M, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(r1.cut, g_ctx);
    const double uNow = std::max(0.0, (ch.danceClock() - static_cast<double>(r1.start)) * 2.0);
    CHECK(r1.uEnd == std::ceil(uNow), "uEnd %.4f, u then %.4f", r1.uEnd, uNow);
    const int64_t wantEnd =
        std::min<int64_t>(r0.end, static_cast<int64_t>(std::ceil(static_cast<double>(r1.start) + r1.uEnd / 2.0)));
    CHECK(r1.end == wantEnd, "end %lld, want %lld", static_cast<long long>(r1.end),
                                                    static_cast<long long>(wantEnd));
    // The cue's u holds at uEnd until the run's end, never past.
    bool reached = false;
    for (int i = 0; i < 200; ++i) {
      step(ch, s);
      if (ch.run().start != r1.start) break;
      CHECK(ch.cue().u <= r1.uEnd, "u %.4f past %.4f", ch.cue().u, r1.uEnd);
      if (ch.danceClock() >= static_cast<double>(r1.start) + r1.uEnd / 2.0) {
        CHECK(ch.cue().u == r1.uEnd, "held at u %.4f", ch.cue().u);
        reached = true;
      }
      TEST_ASSERT_TRUE_MESSAGE(ch.danceClock() < static_cast<double>(r1.end), g_ctx);
    }
    TEST_ASSERT_TRUE_MESSAGE(reached, g_ctx);
    // Then the picker chooses again: the move fits no octave at t10 907.
    TEST_ASSERT_EQUAL_INT_MESSAGE(kAutoBounce, ch.run().move, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(ch.run().start == r1.end, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kNoOctave, ch.eligibleNow(M), g_ctx);
  }
  {
    // Half time never for an 8-beat move: a lone 8-beat 36-60 move at T 120.
    ScriptCharacter c;
    const int M = c.beatMove("long", 8, 36, 60);
    ctx("an 8-beat move at half time");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kAutoBounce, ch.run().move, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, ch.run().level, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(kNoOctave, ch.eligibleNow(M), g_ctx);
    feed(ch, s, 20.0);
    for (const RunRec& r : ch.runs) TEST_ASSERT_EQUAL_INT_MESSAGE(kAutoBounce, r.first.move, g_ctx);
  }
}

// ===========================================================================
// 4. An I Wonder trace (95.7 -> 191.4 -> 95.7 re-acquires while locked).

namespace {
void iWonder(const dance::Character& c, bool backOnMidpoint) {
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  const double beat = 60.0 / 95.7;
  s.acquire(95.7, 0.0);
  int phase = 0;
  bool first = true, flipBack = false, waiting = false;
  int64_t enterAt = 0;
  double lastClock = 0.0;
  int held = 0;
  while (s.trackTime() < 60.0) {
    if (phase == 0 && s.trackTime() >= 20.0) {
      // Beat 0 on a midpoint between the 95.7 beats: REPHASE's j = 1 matches.
      const double K = std::floor(s.trackTime() / beat - 0.5);
      s.acquire(191.4, (K + 0.5) * beat);
      phase = 1;
    } else if (phase == 1 && s.trackTime() >= 40.0) {
      const double half = backOnMidpoint ? 0.5 : 0.0;
      const double K = std::floor(s.trackTime() / beat - half);
      s.acquire(95.7, (K + half) * beat);
      phase = 2;
      flipBack = true;
    }
    const BeatInput in = s.frame();
    ch.update(in);
    ctx("I Wonder (%s, %s) at %.3f s", c.info().id, backOnMidpoint ? "back on a midpoint" : "back on a beat",
        s.trackTime());
    TEST_ASSERT_EQUAL_INT_MESSAGE(957, ch.t10(), g_ctx);
    const double inc = ch.danceClock() - lastClock;
    const double want = static_cast<double>(in.dt) * static_cast<double>(in.rawBpm) * in.foldScale / 60.0;
    const RunInfo& r = ch.run();
    if (flipBack && backOnMidpoint) {
      // S = 1 has one shift: delta is 1/2, far. The run is held, and the
      // next pick comes at floor(clock) + 1 with the phrase re-anchored.
      CHECK(std::fabs(std::fabs(inc - want) - 0.5) <= 1e-6, "the clock moved %.9f", inc - want);
      TEST_ASSERT_TRUE_MESSAGE(r.status == RunStatus::Held, g_ctx);
      TEST_ASSERT_TRUE_MESSAGE(ch.cue().state == DanceState::Entering, g_ctx);
      enterAt = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
      waiting = true;
    } else if (!first) {
      CHECK(std::fabs(inc - want) <= 1e-6, "the clock moved %.9f, want %.9f", inc, want);
    }
    if (waiting && r.status == RunStatus::Running) {
      CHECK(r.start == enterAt, "the re-pick at %lld, want %lld",
                                                       static_cast<long long>(r.start),
                                                       static_cast<long long>(enterAt));
      TEST_ASSERT_EQUAL_INT_MESSAGE(0, r.phrase, g_ctx);
      waiting = false;
    }
    if (waiting) {
      ++held;
      TEST_ASSERT_TRUE_MESSAGE(r.status == RunStatus::Held, g_ctx);
    } else {
      TEST_ASSERT_TRUE_MESSAGE(r.status != RunStatus::Held, g_ctx);
      TEST_ASSERT_FALSE_MESSAGE(r.cut && r.status != RunStatus::None, g_ctx);
    }
    if (r.status == RunStatus::Running && r.move >= 0) {
      const dance::MoveLabels& l = c.move(r.move);
      if (ranged(l))
        CHECK(dance::paceHolds(957, r.k, l.paceLo, l.paceHi), "%s at k %d: the pace past its 3 %%", l.name, r.k);
    }
    lastClock = ch.danceClock();
    first = false;
    flipBack = false;
  }
  TEST_ASSERT_FALSE_MESSAGE(waiting, g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(ch.runCount() >= 10, g_ctx);
  if (backOnMidpoint) TEST_ASSERT_TRUE_MESSAGE(held >= 1 && held <= 31, g_ctx);
}
}  // namespace

void test_i_wonder() {
  ScriptCharacter c;
  c.beatMove("groove", 2, 90, 100);
  iWonder(c, false);
  iWonder(fixture().character(), false);
  iWonder(c, true);
  iWonder(fixture().character(), true);
}

// ===========================================================================
// 5. A 4:3 trace: the running move cut at its next whole move beat.

void test_four_three() {
  ScriptCharacter c;
  const int A = c.beatMove("a", 2, 100, 130);
  const int B = c.beatMove("b", 2, 140, 170);
  for (int late = 1; late <= 2; ++late) {
    ctx("4:3, %d frame(s) after the beat", late);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    // At 120 BPM only A dances, its later runs at level 2 (no-repeat gives way).
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(A, ch.run().move, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.run().level, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(A, ch.run().move, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, ch.run().level, g_ctx);
    const RunInfo r0 = ch.run();
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.danceClock() >= r0.start + 1.0; }, 200), g_ctx);
    if (late == 2) step(ch, s);
    // Beat 0 of the 160 BPM grid on that dance beat (S = 1).
    s.acquire(160.0, s.timeOfBeat(static_cast<double>(r0.start + 1)));
    step(ch, s);
    const RunInfo r1 = ch.run();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1600, ch.t10(), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(r1.status == RunStatus::Running && r1.start == r0.start, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(r1.cut, g_ctx);
    const double uNow = std::max(0.0, ch.danceClock() - static_cast<double>(r1.start));
    // Mid-play: u past 1, in A's first play (u < 2); |delta| <= 1/4, so not held.
    CHECK(uNow > 1.0 && uNow < 2.0, "u %.4f", uNow);
    CHECK(r1.uEnd == std::ceil(uNow), "uEnd %.4f, u %.4f", r1.uEnd, uNow);
    TEST_ASSERT_TRUE_MESSAGE(r1.end == r1.start + static_cast<int64_t>(r1.uEnd), g_ctx);  // k 0: u reaches it there
    for (int i = 0; i < 200; ++i) {
      step(ch, s);
      if (ch.run().start != r1.start) break;
      CHECK(ch.cue().u <= r1.uEnd, "u %.4f past %.4f", ch.cue().u, r1.uEnd);
    }
    const RunInfo r2 = ch.run();
    TEST_ASSERT_EQUAL_INT_MESSAGE(B, r2.move, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, r2.k, g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(r2.start == r1.end, g_ctx);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, ch.prevK(), g_ctx);
  }
}

// ===========================================================================
// 6. States: quiet, listening, the idle fallback, auto-idle, idle runs.

void test_quiet() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  const int Q = c.idleMove("q", 1000, dance::kWhenQuiet);
  const int L = c.idleMove("l", 1000, dance::kWhenListening);
  ctx("quiet");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  // Nothing heard (paused, stopped, Pk0): Quiet, a quiet idle.
  s.heard(false);
  for (int i = 0; i < 10; ++i) {
    step(ch, s);
    CHECK(ch.cue().state == DanceState::Quiet, "state %s", stateName(ch.cue().state));
    CHECK_EQ(Q, ch.cue().idleMove, "the quiet idle");
    CHECK(!ch.cue().showDance, "showDance");
  }
  // Heard, no grid: Listening; the class change re-picks at once.
  s.heard(true);
  step(ch, s);
  CHECK(ch.cue().state == DanceState::Listening, "state %s", stateName(ch.cue().state));
  CHECK_EQ(L, ch.cue().idleMove, "the listening idle");
  CHECK_EQ(0, ch.cue().idleMs, "a new idle run");
  // Locked: dancing.
  s.acquire(120.0, s.trackTime());
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  feed(ch, s, 1.0);
  CHECK(ch.cue().state == DanceState::Dancing, "state %s", stateName(ch.cue().state));
  CHECK(ch.cue().showDance, "dancing shows beat frames");
  // A pause: Freeze at once, the step and the energy held; quiet idles take
  // over at w <= 0.5 (within 0.3 s).
  const Cue before = ch.cue();
  const RunInfo runBefore = ch.run();
  s.heard(false);
  step(ch, s);
  CHECK(ch.cue().state == DanceState::Freeze, "state %s", stateName(ch.cue().state));
  CHECK(ch.run().status == RunStatus::Held && ch.run().start == runBefore.start, "the run held");
  CHECK(ch.cue().u == before.u, "u %.6f, before %.6f", ch.cue().u, before.u);
  CHECK_EQ(before.energy, ch.cue().energy, "the energy held");
  CHECK_EQ(Q, ch.cue().idleMove, "the quiet idle picked at the pause");
  CHECK(ch.cue().showDance, "the held step shows while w > 0.5");
  int frames = 1;
  while (ch.cue().weight > 0.5f && frames < 100) {
    CHECK(ch.cue().state == DanceState::Freeze, "state %s", stateName(ch.cue().state));
    CHECK(ch.cue().u == before.u, "held at u %.6f", ch.cue().u);
    step(ch, s);
    ++frames;
  }
  CHECK(frames * 0.033 <= 0.3 + 1e-9, "%d frames to w <= 0.5", frames);
  CHECK(ch.run().status == RunStatus::None, "abandoned at w <= 0.5");
  CHECK(ch.cue().state == DanceState::Quiet, "state %s", stateName(ch.cue().state));
  CHECK(!ch.cue().showDance, "quiet idle frames");
  CHECK_EQ(Q, ch.cue().idleMove, "the quiet idle");
  feed(ch, s, 3.0);
  CHECK(ch.cue().state == DanceState::Quiet, "state %s", stateName(ch.cue().state));
}

void test_listening() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  const int L = c.idleMove("l", 1000, dance::kWhenListening);
  {
    ctx("listening: no grid, then a fresh grid not yet locked");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    for (int i = 0; i < 60; ++i) {
      step(ch, s);
      CHECK(ch.cue().state == DanceState::Listening, "state %s", stateName(ch.cue().state));
      CHECK_EQ(L, ch.cue().idleMove, "the listening idle");
      CHECK(ch.run().status == RunStatus::None, "no run");
    }
    s.locked(false);
    s.acquire(120.0, s.trackTime());
    for (int i = 0; i < 90; ++i) {
      step(ch, s);
      CHECK(ch.cue().state == DanceState::Listening, "state %s", stateName(ch.cue().state));
      CHECK(ch.run().status == RunStatus::None, "no run without a lock");
      CHECK(ch.cue().weight == 0.0f, "w %g", ch.cue().weight);
    }
  }
  {
    ctx("listening: a grid dropped while heard");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 1.0);
    const Cue before = ch.cue();
    s.grid(false);
    step(ch, s);
    // Held at once (U6: without a grid), Leaving while w > 0.5.
    CHECK(ch.run().status == RunStatus::Held, "held");
    CHECK(ch.cue().state == DanceState::Leaving, "state %s", stateName(ch.cue().state));
    CHECK(ch.cue().u == before.u, "the step held");
    int frames = 1;
    while (ch.cue().weight > 0.5f && frames < 100) {
      step(ch, s);
      ++frames;
    }
    CHECK(ch.run().status == RunStatus::None, "abandoned");
    CHECK(ch.cue().state == DanceState::Listening, "state %s", stateName(ch.cue().state));
    CHECK_EQ(L, ch.cue().idleMove, "the listening idle");
    // The grid back: the clock starts afresh and the phrase is re-anchored.
    s.grid(true);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(0, ch.run().phrase, "the phrase re-anchored after a dropped grid");
  }
}

void test_idle_fallbacks() {
  {
    ctx("only a quiet idle: used while listening");
    ScriptCharacter c;
    const int Q = c.idleMove("q", 1000, dance::kWhenQuiet);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    for (int i = 0; i < 90; ++i) {
      step(ch, s);
      CHECK(ch.cue().state == DanceState::Listening, "state");
      CHECK_EQ(Q, ch.cue().idleMove, "the other state's idle");
    }
  }
  {
    ctx("only a listening idle: used while quiet");
    ScriptCharacter c;
    const int L = c.idleMove("l", 1000, dance::kWhenListening);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.heard(false);
    for (int i = 0; i < 90; ++i) {
      step(ch, s);
      CHECK_EQ(L, ch.cue().idleMove, "the other state's idle");
    }
  }
  {
    ctx("no idles: auto-idle in runs of 4,000 ms");
    ScriptCharacter c;
    c.beatMove("m", 1, 0, 0);
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    uint32_t last = 0;
    int wraps = 0;
    for (int i = 0; i < 30 * 21; ++i) {
      step(ch, s);
      const Cue& q = ch.cue();
      CHECK_EQ(kAutoIdle, q.idleMove, "auto-idle");
      CHECK(!q.idleMirror, "auto-idle unmirrored");
      CHECK(q.idleMs < 4000, "idleMs %u", q.idleMs);
      if (i > 0 && q.idleMs < last) {
        ++wraps;
        CHECK(std::abs(static_cast<int>(last) + 33 - static_cast<int>(q.idleMs) - 4000) <= 1,
              "a run of %d ms", static_cast<int>(last) + 33 - static_cast<int>(q.idleMs));
      } else if (i > 0) {
        CHECK(q.idleMs - last >= 32 && q.idleMs - last <= 34, "idleMs advanced %u", q.idleMs - last);
      }
      last = q.idleMs;
    }
    CHECK(wraps >= 4 && wraps <= 6, "%d runs ended in 21 s", wraps);
    // The same while dancing (the idle run advances under beat frames).
    s.acquire(120.0, s.trackTime());
    for (int i = 0; i < 30 * 10; ++i) {
      step(ch, s);
      CHECK_EQ(kAutoIdle, ch.cue().idleMove, "auto-idle while dancing");
      CHECK(ch.cue().idleMs < 4000, "idleMs %u", ch.cue().idleMs);
    }
  }
  {
    ctx("no character: no idle");
    Twin ch;
    Script s(30);
    for (int i = 0; i < 10; ++i) {
      step(ch, s);
      CHECK_EQ(kNoMove, ch.cue().idleMove, "no idle");
      CHECK(ch.character() == nullptr, "no character");
    }
  }
}

void test_idle_runs() {
  ScriptCharacter c;
  const int A = c.idleMove("a", 1000, dance::kWhenListening);
  const int B = c.idleMove("b", 1500, dance::kWhenListening, 30);
  c.beatMove("m", 1, 0, 0);
  ctx("idle runs of 1-3 loops");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  step(ch, s);
  int move = ch.cue().idleMove;
  uint32_t last = ch.cue().idleMs;
  CHECK(move == A || move == B, "an idle of the class");
  int loops[4] = {0, 0, 0, 0}, runs = 0;
  for (int i = 0; i < 30 * 180; ++i) {
    step(ch, s);
    const Cue& q = ch.cue();
    const bool repick = q.idleMs < last || q.idleMove != move;
    if (repick) {
      // A new pick after loops * idleMs, the overshoot carried over.
      const int ms = static_cast<int>(last) + 33 - static_cast<int>(q.idleMs);
      const int idleMs = c.move(move).idleMs;
      const int n = (ms + idleMs / 2) / idleMs;
      CHECK(n >= 1 && n <= 3 && std::abs(ms - n * idleMs) <= 1, "a run of %d ms of a %d ms idle", ms, idleMs);
      if (n >= 1 && n <= 3) ++loops[n];
      CHECK(q.idleMove != move, "the same idle twice in a row");
      CHECK(q.idleMs < 40, "the overshoot %u", q.idleMs);
      move = q.idleMove;
      ++runs;
    } else {
      CHECK(q.idleMs - last >= 32 && q.idleMs - last <= 34, "idleMs advanced %u", q.idleMs - last);
    }
    CHECK(q.idleMove == A || q.idleMove == B, "an idle of the class");
    last = q.idleMs;
  }
  CHECK(runs >= 50, "%d idle runs", runs);
  CHECK(loops[1] > 0 && loops[2] > 0 && loops[3] > 0, "loops seen: %d %d %d", loops[1], loops[2], loops[3]);
}

// ===========================================================================
// 7. Genre: a filter, never relaxed.

void test_genre() {
  ScriptCharacter c;
  dance::MoveLabels t = ScriptCharacter::defaults("tagged");
  t.beats = 1;
  const uint32_t tag = kHipHop;
  t.genreCount = 1;
  t.genreKeys = &tag;
  const int T = c.add(t);
  c.beatMove("u1", 1, 0, 0);
  c.beatMove("u2", 1, 0, 0);
  {
    ctx("genre: dormant with no track genres, and with a key that doesn't match");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 40.0);
    CHECK_EQ(kNoOctave, ch.eligibleNow(T), "dormant");
    const uint32_t other = kRandB;
    ch.onTrack(pathHashOf(1), &other, 1);
    feed(ch, s, 40.0);
    CHECK_EQ(kNoOctave, ch.eligibleNow(T), "not matching");
    for (const RunRec& r : ch.runs) CHECK(r.first.move != T, "a tagged move picked without a match");
    CHECK(ch.runCount() >= 30, "%d runs", ch.runCount());
    // A matching key, injected (Mg) or the track's: eligible, and picked.
    ch.addGenre(kHipHop);
    CHECK_EQ(0, ch.eligibleNow(T), "matched through Mg");
    const int before = ch.runCount();
    feed(ch, s, 40.0);
    int tagged = 0;
    for (int i = before; i < ch.runCount(); ++i) tagged += ch.runs[i].first.move == T;
    CHECK(tagged > 0, "the matched tagged move never picked");
    ch.onTrack(pathHashOf(2), nullptr, 0);  // clears Mg's key
    CHECK_EQ(kNoOctave, ch.eligibleNow(T), "Mg cleared by onTrack");
    ch.onTrack(pathHashOf(3), &tag, 1);
    CHECK_EQ(0, ch.eligibleNow(T), "matched through the track's genres");
  }
  {
    // The x3: the first pick after each of 2,000 restarts (no history), one
    // matched tagged and two untagged 1-beat moves of equal weight: 3/5.
    ctx("genre: the x3 boost");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.onTrack(pathHashOf(100 + i), &tag, 1);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      hits += ch.run().move == T;
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 0.6) <= 0.05, "the tagged move's share %.3f, want 3/5", share);
  }
  {
    // The ladder: only unmatched tagged moves leaves nothing; auto-bounce.
    ctx("genre: the ladder ends in auto-bounce");
    ScriptCharacter only;
    dance::MoveLabels a = ScriptCharacter::defaults("rnb");
    a.beats = 2;
    const uint32_t ka = kRandB, kb = kDnB;
    a.genreCount = 1;
    a.genreKeys = &ka;
    only.add(a);
    dance::MoveLabels b = ScriptCharacter::defaults("dnb");
    b.beats = 1;
    b.genreCount = 1;
    b.genreKeys = &kb;
    only.add(b);
    Twin ch;
    ch.setCharacter(&only);
    ch.onTrack(pathHashOf(4), &tag, 1);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 20.0);
    CHECK(ch.runCount() >= 3, "%d runs", ch.runCount());
    for (const RunRec& r : ch.runs) {
      CHECK_EQ(kAutoBounce, r.first.move, "auto-bounce");
      CHECK_EQ(3, r.first.level, "level 3");
    }
  }
}

// ===========================================================================
// 8. Picking: no-repeat, runs, mirror alternation, from/to, alignment, place.

void test_no_repeat_and_twins() {
  ScriptCharacter c;
  c.beatMove("a", 1, 0, 0);
  c.beatMove("b", 1, 0, 0);
  c.beatMove("c", 1, 0, 0);
  const int M = c.beatMove("m4", 4, 0, 0);
  c.labels(M).flags = dance::kMirror;
  ctx("no-repeat and twins");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(ch, s, 240.0);
  int twins = 0;
  for (size_t i = 1; i < ch.runs.size(); ++i) {
    const RunInfo& p = ch.runs[i - 1].last;
    const RunInfo& r = ch.runs[i].first;
    CHECK(r.level < 2, "run %u at level %d while other moves exist", static_cast<unsigned>(i), r.level);
    if (r.move == p.move) {
      // Only a single play of a mirrorable move, followed by its twin.
      CHECK(r.move == M && p.plays == 1 && r.firstMirror != p.firstMirror, "run %u repeats move %d",
            static_cast<unsigned>(i), r.move);
      ++twins;
    }
  }
  CHECK(ch.runs.size() >= 100, "%u runs", static_cast<unsigned>(ch.runs.size()));
  CHECK(twins > 0, "no twin entry in %u runs", static_cast<unsigned>(ch.runs.size()));
}

void test_plays() {
  ScriptCharacter c;
  const int R8 = c.beatMove("r8", 1, 170, 180);  // +1 at T 88: 8 plays
  const int R4 = c.beatMove("r4", 1, 170, 180);  // repeat 3 at +1: 4
  c.labels(R4).repeat = 3;
  const int U = c.beatMove("u", 1, 0, 0);    // k 0: 4 plays
  const int V = c.beatMove("v", 2, 0, 0);    // repeat 1: 1 play
  c.labels(V).repeat = 1;
  const int W = c.beatMove("w", 4, 0, 0);    // 1 play
  const int MM = c.beatMove("mm", 1, 0, 0);  // mirrorable, 4 plays: M, M', M, M'
  c.labels(MM).flags = dance::kMirror;
  const int Tr = c.beatMove("tr", 2, 0, 0);  // travel: 1 play
  c.labels(Tr).travel = 1;
  ctx("plays at T 88");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(88.0, 0.0);
  int seen[7] = {0, 0, 0, 0, 0, 0, 0};
  for (int it = 0; it < 40; ++it) {
    ch.setSpot(-1);  // keeps the travel move possible
    feed(ch, s, 8.0);
  }
  for (const RunRec& rr : ch.runs) {
    const RunInfo& r = rr.first;
    if (r.move == R8) {
      CHECK(r.k == 1 && r.plays == 8, "r8: k %d, %d plays", r.k, r.plays);
      ++seen[0];
    } else if (r.move == R4) {
      CHECK(r.k == 1 && r.plays == 4, "r4: k %d, %d plays", r.k, r.plays);
      ++seen[1];
    } else if (r.move == U) {
      CHECK(r.k == 0 && r.plays == 4, "u: k %d, %d plays", r.k, r.plays);
      ++seen[2];
    } else if (r.move == V) {
      CHECK(r.plays == 1 && r.end - r.start == 2, "v: %d plays", r.plays);
      ++seen[3];
    } else if (r.move == W) {
      CHECK(r.plays == 1 && r.end - r.start == 4, "w: %d plays", r.plays);
      ++seen[4];
    } else if (r.move == MM) {
      CHECK(r.plays == 4, "mm: %d plays", r.plays);
      ++seen[5];
    } else if (r.move == Tr) {
      CHECK(r.plays == 1 && r.end - r.start == 2, "tr: %d plays", r.plays);
      ++seen[6];
    }
  }
  for (int i = 0; i < 7; ++i) CHECK(seen[i] > 0, "move %d never ran", i);
  // beatMirror alternating play by play in a mirrorable run (also checked
  // on every frame by the Twin): each of mm's runs shows both orientations.
  int alternating = 0;
  for (size_t i = 1; i < ch.log.size(); ++i) {
    const Rec& a = ch.log[i - 1];
    const Rec& b = ch.log[i];
    if (b.run.status == RunStatus::Running && b.run.move == MM && a.run.start == b.run.start &&
        a.run.move == MM && a.cue.beatMirror != b.cue.beatMirror) {
      const int ja = std::min(static_cast<int>(std::floor(a.cue.u)), 3);
      const int jb = std::min(static_cast<int>(std::floor(b.cue.u)), 3);
      CHECK(jb == ja + 1, "the orientation flips at a play's start (u %.3f -> %.3f)", a.cue.u, b.cue.u);
      ++alternating;
    }
  }
  CHECK(alternating >= seen[5], "%d flips in %d mm runs", alternating, seen[5]);
}

namespace {
// The first pick after a forced run of X (to = k1) that completed, over n
// restarts: how often it is `want`.
double shareAfterForced(const ScriptCharacter& c, int X, int want, int n, int seed) {
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  int hits = 0;
  for (int i = 0; i < n; ++i) {
    ch.onTrack(pathHashOf(seed + i), nullptr, 0);
    ch.forceNext(X);
    s.restart();
    s.acquire(120.0, s.trackTime());
    if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
    CHECK(ch.run().move == X && ch.run().level == 4, "restart %d: the forced run", i);
    const RunInfo forced = ch.run();
    if (!untilNewRun(ch, s, 400)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after the forced run", i));
    CHECK(ch.run().start == forced.end, "restart %d: the next run starts where the forced one ended", i);
    hits += ch.run().move == want;
  }
  return static_cast<double>(hits) / n;
}
}  // namespace

void test_from_to() {
  // X (to = k1), then Y (from = k1) and Z (from = k2): equal 1-beat moves;
  // Y keeps full weight, Z gets x0.15: 100/115.
  ScriptCharacter c;
  const int X = c.beatMove("x", 1, 0, 0);
  c.labels(X).toTag = 1;
  c.labels(X).toTagM = 1;
  const int Y = c.beatMove("y", 1, 0, 0);
  c.labels(Y).fromTag = 1;
  c.labels(Y).fromTagM = 1;
  const int Z = c.beatMove("z", 1, 0, 0);
  c.labels(Z).fromTag = 2;
  c.labels(Z).fromTagM = 2;
  ctx("from/to: 100/115");
  const double share = shareAfterForced(c, X, Y, 2000, 5000);
  // 0.02: the seeds are fixed, so the share is exact; C = 10 or 20 misses it.
  CHECK(std::fabs(share - 100.0 / 115.0) <= 0.02, "Y's share %.4f, want 100/115", share);
  // A key of 0 matches anything: Z without a `from` keeps full weight: 1/2.
  ScriptCharacter c0;
  const int X0 = c0.beatMove("x", 1, 0, 0);
  c0.labels(X0).toTag = 1;
  c0.labels(X0).toTagM = 1;
  const int Y0 = c0.beatMove("y", 1, 0, 0);
  c0.labels(Y0).fromTag = 1;
  c0.labels(Y0).fromTagM = 1;
  c0.beatMove("z", 1, 0, 0);
  ctx("from/to: a key of 0 matches anything");
  const double half = shareAfterForced(c0, X0, Y0, 2000, 9000);
  CHECK(std::fabs(half - 0.5) <= 0.05, "Y's share %.3f, want 1/2", half);
}

// E.2.5's other weights, on picks whose history is known exactly: recency
// (x0.35 for a move in the last 3 runs), the mirror twin (x2, exempt from
// recency) and continuity when no entry matches (all at 100).
void test_weights() {
  {
    // X forced, then Y forced: the next pick between X (recent) and Z.
    ScriptCharacter c;
    const int X = c.beatMove("x", 1, 0, 0);
    const int Y = c.beatMove("y", 1, 0, 0);
    c.beatMove("z", 1, 0, 0);
    ctx("recency: 35/135");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.onTrack(pathHashOf(50000 + i), nullptr, 0);
      ch.forceNext(X);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      ch.forceNext(Y);
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no Y", i));
      CHECK(ch.run().move == Y && ch.run().level == 4, "restart %d: Y forced", i);
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after Y", i));
      CHECK(ch.run().move != Y, "Y left out");
      hits += ch.run().move == X;
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 35.0 / 135.0) <= 0.05, "X's share %.3f, want 35/135", share);
  }
  {
    // A single play of a mirrorable move: its twin at x2 against N: 2/3.
    ScriptCharacter c;
    const int M = c.beatMove("m", 4, 0, 0);
    c.labels(M).flags = dance::kMirror;
    c.beatMove("n", 1, 0, 0);
    ctx("the mirror twin: 2/3");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.onTrack(pathHashOf(60000 + i), nullptr, 0);
      ch.forceNext(M);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      const RunInfo m = ch.run();
      CHECK(m.move == M && m.plays == 1, "restart %d: M forced, one play", i);
      if (!untilNewRun(ch, s, 300)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after M", i));
      if (ch.run().move == M) {
        CHECK(ch.run().firstMirror != m.firstMirror, "restart %d: the twin is the other orientation", i);
        ++hits;
      }
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 2.0 / 3.0) <= 0.05, "the twin's share %.3f, want 2/3", share);
  }
  {
    // prevTo set, but no entry's `from` matches it: C is 100 for all.
    ScriptCharacter c;
    const int X = c.beatMove("x", 1, 0, 0);
    c.labels(X).toTag = 1;
    c.labels(X).toTagM = 1;
    const int Y = c.beatMove("y", 1, 0, 0);
    c.labels(Y).fromTag = 2;
    c.labels(Y).fromTagM = 2;
    const int Z = c.beatMove("z", 1, 0, 0);
    c.labels(Z).fromTag = 3;
    c.labels(Z).fromTagM = 3;
    ctx("from/to with no match: 1/2");
    const double share = shareAfterForced(c, X, Y, 2000, 70000);
    CHECK(std::fabs(share - 0.5) <= 0.05, "Y's share %.3f, want 1/2", share);
    (void)Z;
  }
}

void test_alignment_and_place() {
  ScriptCharacter c;
  const int S1 = c.beatMove("start", 1, 0, 0);
  c.labels(S1).flags = dance::kPlaceStart;
  const int E1 = c.beatMove("end1", 1, 0, 0);
  c.labels(E1).flags = dance::kPlaceEnd;
  c.labels(E1).repeat = 1;
  const int E2 = c.beatMove("end2", 2, 0, 0);
  c.labels(E2).flags = dance::kPlaceEnd;
  c.labels(E2).repeat = 1;
  const int U1 = c.beatMove("one", 1, 0, 0);
  c.labels(U1).repeat = 1;
  const int U2 = c.beatMove("two", 2, 0, 0);
  c.labels(U2).repeat = 1;
  c.beatMove("four", 4, 0, 0);
  c.beatMove("eight", 8, 0, 0);
  ctx("alignment and place");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(ch, s, 400.0);
  int starts = 0, ends1 = 0, ends2 = 0, level1 = 0, e1p6 = 0, e1p7 = 0, e2p6 = 0;
  for (const RunRec& rr : ch.runs) {
    const RunInfo& r = rr.first;
    if (r.level == 1) ++level1;
    if (r.level != 0) continue;  // the Twin checked level 0's alignment and place
    if (r.move == S1) ++starts;
    if (r.move == E1) ++ends1;
    if (r.move == E2) ++ends2;
    e1p6 += r.move == E1 && r.phrase == 6;
    e1p7 += r.move == E1 && r.phrase == 7;
    e2p6 += r.move == E2 && r.phrase == 6;
  }
  CHECK(starts > 0 && ends1 > 0 && ends2 > 0, "place runs at level 0: start %d, end1 %d, end2 %d", starts, ends1,
        ends2);
  // place = end allows D 1 at p 6 and at p 7, and D 2 at p 6 (E.2.5).
  CHECK(e1p6 > 0 && e1p7 > 0 && e2p6 > 0, "end1 at p 6 %d, at p 7 %d; end2 at p 6 %d", e1p6, e1p7, e2p6);
  CHECK(ch.runCount() >= 100, "%d runs", ch.runCount());
  (void)level1;
}

void test_ladder_levels() {
  {
    // Level 1 when only alignment blocks: F (1 beat, repeat 1) forced runs
    // one dance beat, so the next pick is at p 1, where the 4-beat A isn't
    // aligned and F is left out by no-repeat.
    ScriptCharacter c;
    const int F = c.beatMove("f", 1, 0, 0);
    c.labels(F).repeat = 1;
    const int A = c.beatMove("a", 4, 0, 0);
    ctx("level 1");
    Twin ch;
    ch.setCharacter(&c);
    ch.forceNext(F);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().move == F && ch.run().level == 4 && ch.run().phrase == 0, "the forced run");
    CHECK(ch.run().end - ch.run().start == 1, "one dance beat");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(A, ch.run().move, "A");
    CHECK_EQ(1, ch.run().phrase, "p 1");
    CHECK_EQ(1, ch.run().level, "alignment gave way");
  }
  {
    // Level 2 when only no-repeat blocks: a lone move.
    ScriptCharacter c;
    const int M = c.beatMove("m", 1, 0, 0);
    ctx("level 2");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().move == M && ch.run().level == 0, "the first run");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().move == M && ch.run().level == 2, "no-repeat gave way");
  }
}

// ===========================================================================
// 9. The draw, exactly.

void test_draw_exactly() {
  for (int mirror = 0; mirror < 2; ++mirror) {
    ScriptCharacter c;
    const int M0 = c.beatMove("m0", 1, 0, 0, 10);
    const int M1 = c.beatMove("m1", 1, 0, 0, 30);
    if (mirror) {
      c.labels(M0).flags = dance::kMirror;
      c.labels(M1).flags = dance::kMirror;
    }
    for (int i = 0; i < 300; ++i) {
      // pathHash 1 with the first update's restarts 0 and acquisitions 1
      // makes the seed 0: 0x4D50444E instead.
      const uint32_t h = i == 0 ? 1u : pathHashOf(20000 + i);
      ctx("the first draw, pathHash 0x%08X, %s", h, mirror ? "mirrorable" : "unmirrored");
      Twin ch;
      ch.setCharacter(&c);
      ch.reset(h);
      Script s(30);
      s.acquire(120.0, 0.0);
      const BeatInput first = s.frame();
      ch.update(first);
      TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
      uint32_t seed = h ^ first.restarts ^ first.acquisitions;
      if (seed == 0) seed = 0x4D50444Eu;
      if (i == 0) TEST_ASSERT_EQUAL_HEX32(0x4D50444Eu, seed);
      packedit::Xorshift32 x(seed);
      const uint32_t r = x.below(10 * 100 * 100 + 30 * 100 * 100);
      CHECK_EQ(r < 10 * 100 * 100 ? M0 : M1, ch.run().move, "the entry drawn (r %u)", r);
      const bool o = mirror ? x.below(2) == 1 : false;
      CHECK_EQ(o, ch.run().firstMirror, "the orientation drawn");
    }
  }
}

// ===========================================================================
// 10. Travel.

void test_travel_on_the_fixture() {
  using namespace beatscript;
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  const int walk = d.viewIndex(kWalk);
  TEST_ASSERT_EQUAL_INT(kWalk, walk);  // the view keeps MOVS 0-7 in place
  {
    ctx("walk on the fixture");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    int fromLeft = 0, fromRight = 0;
    for (int it = 0; it < 160; ++it) {
      ch.setSpot(it % 2 ? 1 : -1);
      for (int k = 0; k < 3; ++k) {
        if (!untilNewRun(ch, s, 400)) TEST_FAIL_MESSAGE(msg("no run"));
        const RunInfo r = ch.run();
        if (r.move != walk) continue;
        CHECK(r.spot != 0, "walk from the centre");
        CHECK(r.firstMirror == (r.spot == 1), "walk from %d %s", r.spot, r.firstMirror ? "mirrored" : "unmirrored");
        if (r.level == 0) CHECK(r.phrase == 0 || r.phrase == 4, "walk at p %d", r.phrase);
        CHECK_EQ(1, r.plays, "a travel move plays once");
        (r.spot < 0 ? fromLeft : fromRight)++;
        // When its run ends the dancer stands on its destination, the centre.
        if (!untilNewRun(ch, s, 400)) TEST_FAIL_MESSAGE(msg("no run after walk"));
        CHECK_EQ(0, ch.cue().spot, "walk lands on the centre");
        break;
      }
    }
    CHECK(fromLeft > 0 && fromRight > 0, "walk from -1 %d times, from +1 %d times", fromLeft, fromRight);
  }
  {
    ctx("walk forced from -1, then idles");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    ch.setSpot(-1);
    ch.forceNext(walk);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    const RunInfo r = ch.run();
    CHECK(r.move == walk && !r.firstMirror && r.spot == -1, "walk from -1, unmirrored");
    while (ch.run().start == r.start) {
      CHECK_EQ(-1, ch.cue().spot, "the start spot while it runs");
      step(ch, s);
    }
    CHECK_EQ(0, ch.cue().spot, "arrived");
    s.heard(false);
    for (int i = 0; i < 150; ++i) {
      step(ch, s);
      CHECK_EQ(0, ch.cue().spot, "the spot kept through idles");
    }
    CHECK(!ch.cue().showDance, "idles");
    s.heard(true);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(0, ch.run().spot, "dancing again from the centre");
    // From spot 0 walk is never eligible (no start spot there): forced, it
    // falls back to unmirrored.
  }
}

void test_travel_bias() {
  ScriptCharacter c;
  const int T = c.beatMove("t", 2, 0, 0);
  c.labels(T).travel = 1;
  c.beatMove("p", 2, 0, 0);
  {
    // The first pick after each restart (p 0, first-run bias): x4.
    ctx("the centre bias, first run: 4/5");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.setSpot(-1);
      ch.onTrack(pathHashOf(30000 + i), nullptr, 0);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      CHECK_EQ(0, ch.run().phrase, "p 0");
      CHECK_EQ(-1, ch.run().spot, "from -1");
      hits += ch.run().move == T;
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 0.8) <= 0.02, "T's share %.4f, want 4/5", share);  // x5 would miss
  }
  {
    // F forced first uses up the first-run bias; setSpot(-1) in its fourth
    // dance beat; the next pick (p 4, F left out) is T at x2: 2/3.
    ScriptCharacter c3;
    const int T3 = c3.beatMove("t", 2, 0, 0);
    c3.labels(T3).travel = 1;
    c3.beatMove("p", 2, 0, 0);
    const int F = c3.beatMove("f", 1, 0, 0);
    ctx("the centre bias, later runs: 2/3");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c3);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.setSpot(-1);
      ch.onTrack(pathHashOf(40000 + i), nullptr, 0);
      ch.forceNext(F);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      const RunInfo f = ch.run();
      CHECK(f.move == F && f.plays == 4 && f.end - f.start == 4, "restart %d: F forced, 4 plays", i);
      if (!until(ch, s, [&] { return ch.danceClock() >= static_cast<double>(f.start + 3); }, 200))
        TEST_FAIL_MESSAGE(msg("restart %d: F's fourth beat", i));
      ch.setSpot(-1);
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after F", i));
      CHECK_EQ(4, ch.run().phrase, "p 4");
      CHECK(ch.run().start == f.start + 4, "restart %d: the pick at F's fourth beat's end", i);
      CHECK(ch.run().move != F, "F left out");
      hits += ch.run().move == T3;
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 2.0 / 3.0) <= 0.02, "T's share %.4f, want 2/3", share);
  }
  {
    // D >= 2: a 2-beat travel move never runs at k = +1, even after a
    // ranged run at +1 (prevK +1).
    ScriptCharacter c2;
    const int R = c2.beatMove("r", 1, 170, 180);
    const int V = c2.beatMove("v", 2, 72, 180);
    c2.labels(V).travel = 1;
    ctx("D >= 2");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c2);
    Script s(30);
    s.acquire(88.0, 0.0);
    int vRuns = 0, vAfterPlus = 0, rRuns = 0;
    for (int it = 0; it < 30; ++it) {
      ch.setSpot(-1);
      feed(ch, s, 6.0);
    }
    for (const RunRec& rr : ch.runs) {
      if (rr.first.move == R) ++rRuns;
      if (rr.first.move != V) continue;
      ++vRuns;
      CHECK_EQ(0, rr.first.k, "the travel move at k %d", rr.first.k);
      vAfterPlus += rr.prevKBefore == 1;
    }
    CHECK(vRuns > 0 && rRuns > 0 && vAfterPlus > 0, "runs: v %d (after +1: %d), r %d", vRuns, vAfterPlus, rRuns);
  }
}

// END's travel rule (E.2.4): a travel run that ends early lands on its
// destination when it got past half way (u >= beats / 2), otherwise it stays
// at its start spot; setSpot overrides it.
void test_travel_abandoned() {
  ScriptCharacter c;
  const int T = c.beatMove("t", 2, 0, 0);  // k 0: one play of 2 dance beats, u 0..2
  c.labels(T).travel = 1;
  struct Case {
    double at;  // dance beats into the run when it ends
    int how;    // 0 a long pause, 1 a track reset, 2 setSpot(+1)
    int spot;   // the spot after
  };
  // how 3: the lock lost at u 0.85-0.92, so CUT holds it at u 1.0 = beats / 2
  // exactly, and U8 ends it there: it counts as arrived.
  const Case cases[] = {{0.6, 0, -1}, {1.3, 0, 0}, {0.6, 1, -1}, {1.3, 1, 0},
                        {1.3, 2, 1},  {0.6, 2, 1}, {0.85, 3, 0}};
  for (const Case& k : cases) {
    ctx("a travel run ended %.2f beats in (%s)", k.at,
        k.how == 0 ? "pause" : (k.how == 1 ? "restart" : (k.how == 2 ? "setSpot" : "lost lock")));
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    ch.setSpot(-1);
    ch.forceNext(T);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    const RunInfo r = ch.run();
    CHECK(r.move == T && r.spot == -1 && !r.firstMirror, "t from -1");
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.danceClock() >= r.start + k.at; }, 200), g_ctx);
    if (k.how == 0) {
      s.heard(false);
      feed(ch, s, 1.0);
      CHECK(ch.run().status == RunStatus::None, "abandoned");
    } else if (k.how == 1) {
      s.restart();
      step(ch, s);
    } else if (k.how == 3) {
      s.locked(false);
      step(ch, s);
      CHECK(ch.run().uEnd == 1.0, "cut to u 1 (uEnd %.3f)", ch.run().uEnd);
      feed(ch, s, 2.0);
      CHECK(ch.run().status == RunStatus::None, "abandoned at u 1");
    } else {
      ch.setSpot(1);
      step(ch, s);
    }
    CHECK_EQ(k.spot, ch.cue().spot, "the spot after");
  }
}

// ===========================================================================
// 11. Determinism (every scenario runs on a Twin), a different seed, and a
//     gapless join.

void test_determinism() {
  CardDancer& d = fixture();
  std::vector<std::pair<int, bool>> seq[2];
  for (int i = 0; i < 2; ++i) {
    ctx("the fixture with pathHash %d", i + 1);
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&d.character());
    ch.reset(static_cast<uint32_t>(i + 1));
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 90.0);
    for (const RunRec& r : ch.runs) seq[i].push_back({r.first.move, r.first.firstMirror});
    CHECK(seq[i].size() >= 30, "%u runs", static_cast<unsigned>(seq[i].size()));
  }
  TEST_ASSERT_TRUE_MESSAGE(seq[0] != seq[1], "a different pathHash gives the same runs");
}

void test_gapless_join() {
  ScriptCharacter c;
  c.beatMove("a", 1, 0, 0);
  c.beatMove("b", 1, 0, 0);
  const int B2 = c.beatMove("b2", 2, 0, 0);
  c.labels(B2).repeat = 1;
  const int C1 = c.beatMove("c1", 1, 0, 0);
  c.labels(C1).repeat = 1;
  c.beatMove("r", 2, 100, 130);
  dance::MoveLabels g = ScriptCharacter::defaults("hip");
  g.beats = 1;
  const uint32_t kh = kHipHop, kr = kRandB;
  g.genreCount = 1;
  g.genreKeys = &kh;
  const int G1 = c.add(g);
  g.name = "rnb";
  g.genreKeys = &kr;
  const int G2 = c.add(g);
  // Three Twins fed the same: A joins onto a new track with the genre hip
  // hop, B doesn't join, C joins with the same genre and another path.
  Twin A, B, C;
  for (Twin* t : {&A, &B, &C}) {
    t->keepLog = false;
    t->setCharacter(&c);
    t->addGenre(kRandB);
  }
  Script s(30);
  s.acquire(120.0, 0.0);
  for (int i = 0; i < 30 * 30; ++i) {
    const BeatInput in = s.frame();
    A.update(in);
    B.update(in);
    C.update(in);
  }
  // Mid-run.
  ctx("a gapless join");
  for (int i = 0; i < 300 && !(A.run().status == RunStatus::Running && A.danceClock() > A.run().start + 0.5); ++i) {
    const BeatInput in = s.frame();
    A.update(in);
    B.update(in);
    C.update(in);
  }
  const RunInfo before = A.run();
  const int t10 = A.t10();
  CHECK(before.status == RunStatus::Running, "mid-run");
  CHECK_EQ(0, A.eligibleNow(G2), "Mg's genre before the join");
  CHECK_EQ(kNoOctave, A.eligibleNow(G1), "hip hop before the join");
  const int64_t anchor = before.start - before.phrase;  // A, mod 8
  A.onTrack(pathHashOf(7), &kh, 1);
  C.onTrack(pathHashOf(8), &kh, 1);
  CHECK(sameRun(before, A.run()), "the run kept by the call");
  CHECK_EQ(0, A.eligibleNow(G1), "the new track's genre");
  CHECK_EQ(kNoOctave, A.eligibleNow(G2), "Mg's genre cleared");
  CHECK_EQ(0, B.eligibleNow(G2), "B keeps Mg's genre");
  const size_t runsA = A.runs.size(), runsB = B.runs.size(), runsC = C.runs.size();
  for (int i = 0; i < 30 * 60; ++i) {
    const BeatInput in = s.frame();
    A.update(in);
    B.update(in);
    C.update(in);
    if (A.run().start == before.start) {
      CHECK(A.run().move == before.move && A.run().end == before.end && A.run().status != RunStatus::None,
            "the joined run goes on");
    }
    CHECK_EQ(t10, A.t10(), "t10 kept");
  }
  std::vector<int> la, lb, lc;
  for (size_t i = runsA; i < A.runs.size(); ++i) {
    la.push_back(A.runs[i].first.move);
    CHECK_EQ(mod8(A.runs[i].first.start - anchor), A.runs[i].first.phrase, "the phrase count goes on");
  }
  for (size_t i = runsB; i < B.runs.size(); ++i) lb.push_back(B.runs[i].first.move);
  for (size_t i = runsC; i < C.runs.size(); ++i) lc.push_back(C.runs[i].first.move);
  CHECK(la.size() >= 20, "%u runs after the join", static_cast<unsigned>(la.size()));
  CHECK(la != lb, "the join didn't change the later picks");
  CHECK(la != lc, "the new seed didn't change the later picks");
  int hip = 0;
  for (int m : la) {
    CHECK(m != G2, "Mg's genre after the join");
    hip += m == G1;
  }
  CHECK(hip > 0, "the new track's genre never danced");
}

// ===========================================================================
// 12. Discontinuities.

void test_nudges_and_jumps() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  c.beatMove("n", 2, 0, 0);
  {
    ctx("a nudge of -0.05 beat passes through");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.5);
    const RunInfo r = ch.run();
    const double before = ch.danceClock();
    s.nudge(-0.05);
    step(ch, s);
    CHECK(ch.run().status == RunStatus::Running && ch.run().start == r.start, "no hold");
    CHECK(std::fabs(ch.danceClock() - (before + 0.033f * 2.0 - 0.05)) < 1e-6, "the clock moved %.9f",
          ch.danceClock() - before);
    CHECK(std::fabs(ch.danceClock() - s.danceBeats()) < 1e-6, "the clock follows the grid");
  }
  {
    ctx("a nudge of -0.2: a jump within 1/4, u held at 0 across its start");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 1.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);  // a run's first frame
    const RunInfo r = ch.run();
    const double before = ch.danceClock();
    CHECK(before - r.start < 0.1, "just started");
    s.nudge(-0.2);
    step(ch, s);
    const double predicted = before + static_cast<double>(s.dt()) * 120.0 / 60.0;
    CHECK(std::fabs(ch.danceClock() - (predicted - 0.2)) < 1e-6, "the clock %.9f, the prediction %.9f",
          ch.danceClock(), predicted);
    CHECK(ch.run().status == RunStatus::Running && ch.run().start == r.start && ch.run().end == r.end,
          "the run goes on");
    CHECK(ch.cue().u == 0.0, "u %.6f held at 0", ch.cue().u);
    CHECK(ch.cue().state == DanceState::Dancing, "dancing");
    // It goes on and ends where it was to end.
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().start == r.end, "the next run at %lld", static_cast<long long>(ch.run().start));
  }
  {
    ctx("a jump of 2 whole tracker beats at S = 1: delta 0");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.5);
    const RunInfo r = ch.run();
    const double before = ch.danceClock();
    s.nudge(2.0);
    step(ch, s);
    CHECK(std::fabs(ch.danceClock() - (before + static_cast<double>(s.dt()) * 2.0)) < 1e-6, "the clock moved %.9f",
          ch.danceClock() - before);
    CHECK(sameRun(r, ch.run()), "nothing changes");
    const RunInfo r2 = ch.run();
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().start == r2.end, "the next run where the last ended");
    CHECK_EQ(mod8(r2.end - (r2.start - r2.phrase)), ch.run().phrase, "the phrase goes on");
  }
  {
    ctx("a re-acquire 1/2 beat off: held, then re-anchored");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.7);
    const Cue last = ch.cue();
    const double K = std::floor(s.beatsAt(s.trackTime()));
    s.acquire(120.0, s.timeOfBeat(K + 0.5));
    step(ch, s);
    CHECK(ch.run().status == RunStatus::Held, "held");
    CHECK(ch.cue().u == last.u, "held at once");
    CHECK(ch.cue().state == DanceState::Entering, "state %s", stateName(ch.cue().state));
    const int64_t E = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().start == E, "the pick at %lld, want %lld", static_cast<long long>(ch.run().start),
          static_cast<long long>(E));
    CHECK_EQ(0, ch.run().phrase, "re-anchored");
  }
}

void test_leaving() {
  ScriptCharacter c;
  c.beatMove("m", 2, 0, 0);
  c.beatMove("n", 4, 0, 0);
  const int L = c.idleMove("l", 1000, dance::kWhenListening);
  {
    ctx("Leaving: the lock lost mid-run");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.3);
    const RunInfo r0 = ch.run();
    s.locked(false);
    step(ch, s);
    const RunInfo r1 = ch.run();
    const double uNow = std::max(0.0, ch.danceClock() - static_cast<double>(r0.start));  // k 0
    CHECK(ch.cue().state == DanceState::Leaving, "state %s", stateName(ch.cue().state));
    CHECK(r1.start == r0.start && r1.status == RunStatus::Running, "plays on");
    CHECK(!r1.cut, "a lost lock isn't a pace cut");
    CHECK(r1.uEnd == std::min(r0.uEnd, std::ceil(uNow)), "uEnd %.4f, u %.4f", r1.uEnd, uNow);
    CHECK(r1.end == std::min<int64_t>(r0.end, r0.start + static_cast<int64_t>(std::ceil(uNow))), "end");
    int frames = 1;
    while (ch.cue().weight > 0.5f && frames < 100) {
      CHECK(ch.cue().showDance, "the held step shows while w > 0.5");
      CHECK(ch.cue().u <= r1.uEnd, "u past the cut");
      step(ch, s);
      ++frames;
    }
    CHECK(ch.run().status == RunStatus::None, "idles once w <= 0.5");
    CHECK(!ch.cue().showDance, "idle frames");
    CHECK(ch.cue().state == DanceState::Listening, "state %s", stateName(ch.cue().state));
    CHECK_EQ(L, ch.cue().idleMove, "the listening idle");
  }
  {
    ctx("Leaving: the lock back after two frames");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.3);
    const RunInfo r0 = ch.run();
    s.locked(false);
    step(ch, s);
    step(ch, s);
    const RunInfo cut = ch.run();
    CHECK(cut.start == r0.start, "the same run");
    s.locked(true);
    for (int i = 0; i < 300 && ch.run().start == r0.start; ++i) {
      step(ch, s);
      CHECK(ch.run().start != r0.start || ch.run().end == cut.end, "the cut end kept");
    }
    CHECK(ch.run().start == cut.end, "the new pick at %lld, want %lld", static_cast<long long>(ch.run().start),
          static_cast<long long>(cut.end));
    CHECK(ch.run().status == RunStatus::Running && ch.cue().state == DanceState::Dancing, "dancing goes on");
  }
}

void test_pause_and_resume() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  const int M2 = c.beatMove("one", 1, 0, 0);
  c.labels(M2).repeat = 1;  // one dance beat: phrases off 0 and 4
  c.beatMove("two", 2, 0, 0);
  for (int pause = 0; pause < 2; ++pause) {
    ctx("pause and resume (%s)", pause ? "long" : "short");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 6.0);
    const RunInfo r0 = ch.run();
    const Cue last = ch.cue();
    const int64_t anchor = r0.start - r0.phrase;
    s.heard(false);
    step(ch, s);
    CHECK(ch.cue().state == DanceState::Freeze, "state %s", stateName(ch.cue().state));
    CHECK(ch.cue().u == last.u, "u held at once");
    CHECK_EQ(last.energy, ch.cue().energy, "the energy held");
    feed(ch, s, pause ? 3.0 : 0.15);
    if (pause) CHECK(ch.run().status == RunStatus::None, "abandoned during a long pause");
    s.heard(true);
    // Entering: the next pick at the next whole beat after w > 0.5, the
    // phrase continuing from before the pause.
    int64_t E = -1;
    for (int i = 0; i < 300; ++i) {
      step(ch, s);
      if (E < 0 && ch.cue().state == DanceState::Entering) E = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
      if (ch.run().status == RunStatus::Running && ch.run().start != r0.start) break;
    }
    CHECK(E >= 0, "no Entering");
    CHECK(ch.run().start == E, "the pick at %lld, want %lld", static_cast<long long>(ch.run().start),
          static_cast<long long>(E));
    CHECK_EQ(mod8(E - anchor), ch.run().phrase, "the phrase continues");
  }
}

void test_track_reset() {
  ScriptCharacter c;
  c.beatMove("r", 2, 100, 130);
  c.beatMove("s", 2, 90, 140);
  ctx("a track reset");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(ch, s, 4.0);
  CHECK_EQ(0, ch.prevK(), "prevK before");
  CHECK_EQ(1200, ch.t10(), "t10 before");
  const Cue before = ch.cue();
  s.restart();
  step(ch, s);
  const Cue& q = ch.cue();
  CHECK_EQ(0, ch.t10(), "t10 cleared");
  CHECK_EQ(kNoOctave, ch.prevK(), "prevK cleared");
  CHECK(ch.run().status == RunStatus::None, "the run cleared");
  CHECK_EQ(kNoMove, q.beatMove, "beatMove");
  CHECK(q.u == 0.0 && q.beatPos16 == 0 && q.k == 0 && !q.beatMirror, "the beat fields cleared");
  CHECK_EQ(before.spot, q.spot, "the spot");
  CHECK(std::fabs(q.idleSeconds - (before.idleSeconds + s.dt())) < 1e-4, "the idle clock goes on");
  CHECK(std::fabs(q.weight - before.weight) <= 1.0f / 12.0f, "the weight goes on");
  CHECK(q.weight > 0.5f, "no jump in the weight");
  // The next lock dances from a fresh phrase, the latch set anew.
  s.acquire(125.0, s.trackTime());
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  CHECK_EQ(1250, ch.t10(), "the new latch");
  CHECK_EQ(0, ch.run().phrase, "a fresh phrase");
}

void test_entering() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  c.idleMove("l", 1000, dance::kWhenListening);
  for (int fps : {30, 24}) {
    ctx("Entering at %d fps", fps);
    Twin ch;
    ch.setCharacter(&c);
    Script s(fps);
    feed(ch, s, 1.0);
    CHECK(ch.cue().state == DanceState::Listening, "Listening first");
    s.acquire(117.0, s.trackTime() + 0.13);
    int64_t E = -1;
    for (int i = 0; i < 200; ++i) {
      step(ch, s);
      const Cue& q = ch.cue();
      if (E < 0 && q.state == DanceState::Entering) E = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
      if (E < 0) continue;
      if (ch.danceClock() < static_cast<double>(E)) {
        CHECK(q.state == DanceState::Entering && !q.showDance, "idle frames while Entering");
        CHECK(ch.run().status == RunStatus::None, "no run yet");
      } else {
        CHECK(ch.run().status == RunStatus::Running, "the pick at the beat");
        CHECK(ch.run().start == E, "start %lld, want %lld", static_cast<long long>(ch.run().start),
              static_cast<long long>(E));
        CHECK_EQ(0, ch.run().phrase, "phrase 0");
        CHECK(q.showDance && q.state == DanceState::Dancing, "on the beat");
        CHECK(q.u == 0.0, "drawn from u 0 (u %.4f)", q.u);
        break;
      }
    }
    CHECK(E >= 0 && ch.run().status == RunStatus::Running, "danced");
  }
}

// ===========================================================================
// 13. Timing: every hit shown on the first render at or after its beat.

namespace {
int timingAt(int fps, int P) {
  ScriptCharacter c;
  const int M = c.beatMove("m", 1, P, P);
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(fps);
  int k;
  if (P >= 80 && P <= 160) {
    s.acquire(P, 0.0);
    k = 0;
  } else if (P >= 40 && P < 80) {
    s.acquire(2.0 * P, 0.0);
    k = -1;
  } else if (P > 160) {
    s.acquire(P / 2.0, 0.0);
    k = 1;
  } else {
    // 36-39: T = 2P (72-78) reached from an 80 BPM grid, so the TempoFold
    // keeps S = 1. That grid is unlocked, or the 3 % latch would keep T at
    // 80 for 78 (P 39).
    s.locked(false);
    s.acquire(80.0, 0.0);
    feed(ch, s, 0.5);
    s.locked(true);
    s.acquire(2.0 * P, s.trackTime());
    k = -1;
  }
  const double scale = k > 0 ? 2.0 : (k == 0 ? 1.0 : 0.5);
  double prevClock = ch.danceClock();
  int hits = 0;
  for (int i = 0; i < fps * 40 && hits < 12; ++i) {
    step(ch, s);
    CHECK(s.scale() == 1.0, "S %.2f", s.scale());
    const RunInfo& r = ch.run();
    const double clock = ch.danceClock();
    if (r.status == RunStatus::Running && r.move == M) {
      CHECK_EQ(k, r.k, "the octave");
      const double now = (clock - static_cast<double>(r.start)) * scale;
      const double before = (prevClock - static_cast<double>(r.start)) * scale;
      for (int64_t b = static_cast<int64_t>(std::floor(before)) + 1; b <= static_cast<int64_t>(std::floor(now)); ++b) {
        if (b < 0) continue;
        const uint64_t pos = ch.cue().beatPos16;
        CHECK(pos >= static_cast<uint64_t>(12 * b * 65536) && pos < static_cast<uint64_t>((12 * b + 3) * 65536),
              "beat %lld: pos16 %llu, true u %.5f (u %.5f)", static_cast<long long>(b),
              static_cast<unsigned long long>(pos), now, ch.cue().u);
        if (now - static_cast<double>(b) <= 0.125)
          CHECK(ch.cue().u == static_cast<double>(b), "beat %lld: u %.6f, true u %.6f", static_cast<long long>(b),
                ch.cue().u, now);
        ++hits;
      }
    }
    prevClock = clock;
  }
  return hits;
}
}  // namespace

void test_timing() {
  for (int fps : {24, 30}) {
    for (int P = 36; P <= 180; ++P) {
      ctx("timing at %d fps, pace %d", fps, P);
      const int hits = timingAt(fps, P);
      CHECK(hits >= 12, "%d hits", hits);
    }
  }
}

// ===========================================================================
// 14. Weight and energy.

void test_weight_and_energy() {
  // E.2.1's e16 at the doc's points (this file's reference; the Twin checks
  // every cue's energy against it).
  TEST_ASSERT_EQUAL_UINT32(39322, refE16(0.0f));
  TEST_ASSERT_EQUAL_UINT32(39322, refE16(0.5f));
  TEST_ASSERT_EQUAL_UINT32(47186, refE16(0.65f));
  TEST_ASSERT_EQUAL_UINT32(52429, refE16(0.75f));
  TEST_ASSERT_EQUAL_UINT32(65536, refE16(1.0f));
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  for (int fps : {30, 24}) {
    ctx("the weight at %d fps", fps);
    Twin ch;
    ch.setCharacter(&c);
    Script s(fps);
    s.confidence(0.5f);
    s.acquire(120.0, 0.0);
    const float bound = fps == 30 ? 1.0f / 12.0f : 0.1f;
    float w = 0.0f;
    bool saw[3] = {false, false, false};
    int flashes = 0;
    while (s.trackTime() < 4.0) {
      step(ch, s);
      const Cue& q = ch.cue();
      CHECK(std::fabs(q.weight - w) <= bound, "the weight moved %g", q.weight - w);
      if (s.trackTime() >= 2.0) CHECK(q.weight > 0.95f, "w %g after %.2f s", q.weight, s.trackTime());
      if (q.energy > 39322 && q.energy < 47186) saw[0] = true;
      if (q.energy > 47186 && q.energy < 65536) saw[1] = true;
      if (q.energy == 65536 || q.energy > 65000) saw[2] = true;
      flashes += q.flash;
      if (q.flash) CHECK(q.weight > 0.5f, "a flash at w %g", q.weight);
      w = q.weight;
    }
    CHECK(saw[0] && saw[1] && saw[2], "the energy's fade");
    CHECK(flashes > 0, "no beat light");
    // A lost lock: under 0.5 within 0.3 s.
    s.locked(false);
    int frames = 0;
    while (ch.cue().weight >= 0.5f && frames < 100) {
      step(ch, s);
      ++frames;
      CHECK(std::fabs(ch.cue().weight - w) <= bound, "the weight moved %g", ch.cue().weight - w);
      w = ch.cue().weight;
    }
    CHECK(frames * s.dt() <= 0.3f + 1e-6f, "%d frames to fall under 0.5", frames);
    CHECK(!ch.cue().flash, "no beat light at w <= 0.5");
  }
}

// ===========================================================================
// 15. Switching characters.

void test_switching() {
  ScriptCharacter c1, c2, c3;
  const int P1 = c1.beatMove("p1", 1, 0, 0);
  const int T1 = c1.beatMove("t1", 2, 0, 0);
  c1.labels(T1).travel = 1;
  c1.idleMove("i1", 1000, 3);
  c2.beatMove("x2", 8, 36, 40);  // never eligible at 120: the idle sits at index 3
  const int P2 = c2.beatMove("p2", 1, 0, 0);
  const int T2 = c2.beatMove("t2", 2, 0, 0);
  c2.labels(T2).travel = 1;
  const int I2 = c2.idleMove("i2", 1000, 3);
  const int Q3 = c3.beatMove("q3", 2, 0, 0);
  c3.idleMove("i3", 1000, 3);
  {
    ctx("switching while idle: at once");
    Twin ch;
    ch.setCharacter(&c1);
    Script s(30);
    feed(ch, s, 1.0);
    CHECK_EQ(2, ch.cue().idleMove, "c1's idle");
    ch.setCharacter(&c2);
    CHECK(ch.character() == &c2, "at once");
    step(ch, s);
    CHECK_EQ(I2, ch.cue().idleMove, "c2's idle on the next update");
    ch.setCharacter(nullptr);
    CHECK(ch.character() == nullptr, "no character");
    step(ch, s);
    CHECK_EQ(kNoMove, ch.cue().idleMove, "no idle");
  }
  {
    ctx("switching while dancing: at the next whole dance beat");
    Twin ch;
    ch.setCharacter(&c1);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    ch.setSpot(1);  // t1 can't start from +1: only p1 dances there
    feed(ch, s, 2.2);
    CHECK(ch.cue().showDance && ch.run().move == P1 && ch.cue().spot == 1, "dancing p1 at +1");
    const double C = ch.danceClock();
    const int64_t at = static_cast<int64_t>(std::floor(C)) + 1;
    const RunInfo old = ch.run();
    const Cue lastCue = ch.cue();
    ch.setCharacter(&c2);
    CHECK(ch.character() == &c1, "pending");
    CHECK(ch.run().end <= at, "the run cut to the switch");
    float w = lastCue.weight, idle = lastCue.idleSeconds;
    for (int i = 0; i < 100 && ch.character() == &c1; ++i) {
      step(ch, s);
      const Cue& q = ch.cue();
      if (ch.character() == &c1) {
        CHECK(ch.danceClock() < static_cast<double>(at), "still c1 at the beat");
        CHECK_EQ(old.move, q.beatMove, "the old run's frames");
        CHECK(ch.run().start == old.start, "the old run");
      }
      CHECK(std::fabs(q.weight - w) <= 1.0f / 12.0f, "w moved");
      CHECK(std::fabs(q.idleSeconds - (idle + s.dt())) < 1e-4, "the idle clock");
      w = q.weight;
      idle = q.idleSeconds;
    }
    CHECK(ch.character() == &c2, "switched");
    CHECK(ch.danceClock() >= static_cast<double>(at), "at the beat");
    CHECK(ch.run().status == RunStatus::Running && ch.run().start == at, "the new character's first run at %lld",
          static_cast<long long>(at));
    CHECK_EQ(P2, ch.run().move, "c2's move");
    CHECK_EQ(1, ch.cue().spot, "a travelling character keeps the spot");
    CHECK_EQ(I2, ch.cue().idleMove, "c2's idle");
    // To a character without travel: the centre.
    feed(ch, s, 1.3);
    const int64_t at3 = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
    ch.setCharacter(&c3);
    for (int i = 0; i < 100 && ch.character() != &c3; ++i) step(ch, s);
    CHECK(ch.character() == &c3, "switched to c3");
    CHECK(ch.run().start == at3, "c3's first run at the beat");
    CHECK_EQ(Q3, ch.run().move, "c3's move");
    CHECK_EQ(0, ch.cue().spot, "a character without travel moves dances at the centre");
    CHECK_EQ(0, ch.run().spot, "from the centre");
  }
  (void)T1;
}

// ===========================================================================
// 16. Freezes: k0-k15, the freeze chain, Mf.

void test_freeze_k() {
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  for (int locked = 0; locked < 2; ++locked) {
    ctx("k<n> on the fixture (%s)", locked ? "dancing" : "idle");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    if (locked) s.acquire(120.0, 0.0);
    feed(ch, s, 2.0);
    for (int n = 0; n < 16; ++n) {
      s.freezeK(n);
      step(ch, s);
      const Cue& q = ch.cue();
      CHECK_EQ(0, q.beatMove, "k%d: bounce", n);
      CHECK_EQ(static_cast<uint32_t>(n) * 98304u, q.beatPos16, "k%d: beatPos16", n);
      CHECK(q.u == n / 8.0, "k%d: u %.6f", n, q.u);
      CHECK(q.showDance, "k%d: showDance", n);
      CHECK(q.weight == 1.0f, "k%d: weight", n);
      CHECK_EQ(65536, q.energy, "k%d: energy", n);
      CHECK_EQ(n % 8 == 0, q.flash, "k%d: flash", n);
      CHECK_EQ(0, q.k, "k%d: k", n);
      CHECK(!q.beatMirror, "k%d: unmirrored", n);
    }
    s.freezeK(16);  // out of range counts as off
    step(ch, s);
    s.freezeK(-1);
    feed(ch, s, 1.0);
  }
  {
    // A 1-beat freeze move: k9 gives u = 1/8 (the wrap the chain avoids).
    ScriptCharacter c1;
    c1.beatMove("x", 2, 0, 0);
    const int F = c1.beatMove("f", 1, 0, 0);
    c1.labels(F).flags = dance::kFreeze;
    ctx("k9 on a 1-beat freeze move");
    Twin ch;
    ch.setCharacter(&c1);
    Script s(30);
    s.freezeK(9);
    step(ch, s);
    CHECK_EQ(F, ch.cue().beatMove, "the flagged move");
    CHECK_EQ(98304, ch.cue().beatPos16, "beatPos16");
    CHECK(ch.cue().u == 0.125, "u %.6f", ch.cue().u);
    CHECK(!ch.cue().flash, "k9 doesn't flash");
  }
  {
    ctx("k<n> with no character: auto-bounce, 1 beat");
    Twin ch;
    Script s(30);
    s.freezeK(3);
    step(ch, s);
    CHECK_EQ(kAutoBounce, ch.cue().beatMove, "auto-bounce");
    CHECK_EQ(294912, ch.cue().beatPos16, "3 * 98304 mod 786432");
    s.freezeK(9);
    step(ch, s);
    CHECK_EQ(98304, ch.cue().beatPos16, "9 * 98304 mod 786432");
    CHECK(!ch.cue().flash, "k9 doesn't flash");
    s.freezeK(8);
    step(ch, s);
    CHECK_EQ(0, ch.cue().beatPos16, "8 * 98304 mod 786432");
    CHECK(ch.cue().flash, "k8 flashes");
  }
}

void test_freeze_chain() {
  {
    ScriptCharacter c;
    c.beatMove("x", 2, 0, 0);
    const int Y = c.beatMove("y", 1, 0, 0);
    c.labels(Y).flags = dance::kFreeze;
    TEST_ASSERT_EQUAL_INT(Y, dance::freezeMove(c));  // flagged
  }
  {
    ScriptCharacter c;
    c.beatMove("r", 2, 90, 130);
    c.beatMove("u1", 1, 0, 0);
    const int U2 = c.beatMove("u2", 2, 0, 0);
    TEST_ASSERT_EQUAL_INT(U2, dance::freezeMove(c));  // the first unranged 2-beat
  }
  {
    ScriptCharacter c;
    c.beatMove("r", 2, 90, 130);
    const int U1 = c.beatMove("u1", 1, 0, 0);
    c.beatMove("u4", 4, 0, 0);
    TEST_ASSERT_EQUAL_INT(U1, dance::freezeMove(c));  // the first unranged
  }
  {
    ScriptCharacter c;
    c.beatMove("r", 2, 90, 130);
    c.idleMove("i", 1000, 3);
    TEST_ASSERT_EQUAL_INT(kAutoBounce, dance::freezeMove(c));
    TEST_ASSERT_FALSE(dance::hasTravel(c));
  }
  {
    // Moves with a needs bit are skipped; an idle move flagged freeze isn't one.
    ScriptCharacter c;
    const int I = c.idleMove("i", 1000, 3);
    c.labels(I).flags = dance::kFreeze;
    const int F = c.beatMove("f", 2, 0, 0);
    c.labels(F).flags = dance::kFreeze;
    c.labels(F).needs = dance::kNeedsSongBpm;
    const int U2 = c.beatMove("u2", 2, 0, 0);
    TEST_ASSERT_EQUAL_INT(U2, dance::freezeMove(c));
    const int T = c.beatMove("t", 2, 0, 0);
    c.labels(T).travel = -1;
    c.labels(T).needs = 2;
    TEST_ASSERT_FALSE(dance::hasTravel(c));  // only moves without a needs bit count
    c.labels(T).needs = 0;
    TEST_ASSERT_TRUE(dance::hasTravel(c));
    // The Choreographer's k<n> plays it.
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.freezeK(2);
    step(ch, s);
    TEST_ASSERT_EQUAL_INT(U2, ch.cue().beatMove);
  }
  // startAtMask (E.1).
  TEST_ASSERT_EQUAL_HEX8(0x07, dance::startAtMask(dance::StartAt::Any, false));
  TEST_ASSERT_EQUAL_HEX8(0x02, dance::startAtMask(dance::StartAt::Centre, true));
  TEST_ASSERT_EQUAL_HEX8(0x05, dance::startAtMask(dance::StartAt::Side, false));
  TEST_ASSERT_EQUAL_HEX8(0x01, dance::startAtMask(dance::StartAt::Left, false));
  TEST_ASSERT_EQUAL_HEX8(0x04, dance::startAtMask(dance::StartAt::Left, true));
  TEST_ASSERT_EQUAL_HEX8(0x04, dance::startAtMask(dance::StartAt::Right, false));
  TEST_ASSERT_EQUAL_HEX8(0x01, dance::startAtMask(dance::StartAt::Right, true));
  TEST_ASSERT_EQUAL_HEX8(0x00, dance::startAtMask(static_cast<dance::StartAt>(5), false));
  // Character's default startSpotMask: no art, only the destination and start_at.
  ScriptCharacter c;
  const int A = c.beatMove("a", 2, 0, 0);
  const int R = c.beatMove("r", 2, 0, 0);
  c.labels(R).travel = 1;
  const int L2 = c.beatMove("l2", 4, 0, 0);
  c.labels(L2).travel = -2;
  const int S = c.beatMove("s", 2, 0, 0);
  c.labels(S).travel = 1;
  c.labels(S).startAt = dance::StartAt::Right;
  const int I = c.idleMove("i", 1000, 3);
  const dance::Character& base = c;
  TEST_ASSERT_EQUAL_HEX8(0x07, base.dance::Character::startSpotMask(A, false));
  TEST_ASSERT_EQUAL_HEX8(0x07, base.dance::Character::startSpotMask(I, true));
  TEST_ASSERT_EQUAL_HEX8(0x03, base.dance::Character::startSpotMask(R, false));  // -1, 0
  TEST_ASSERT_EQUAL_HEX8(0x06, base.dance::Character::startSpotMask(R, true));   // 0, +1
  TEST_ASSERT_EQUAL_HEX8(0x04, base.dance::Character::startSpotMask(L2, false));  // +1 -> -1
  TEST_ASSERT_EQUAL_HEX8(0x01, base.dance::Character::startSpotMask(L2, true));
  TEST_ASSERT_EQUAL_HEX8(0x00, base.dance::Character::startSpotMask(S, false));  // right is +1: no destination
  TEST_ASSERT_EQUAL_HEX8(0x00, base.dance::Character::startSpotMask(S, true));   // right mirrored is -1... -1 - 1
  // setSpots overrides it.
  c.setSpots(R, 0x01, 0x00);
  TEST_ASSERT_EQUAL_HEX8(0x01, c.startSpotMask(R, false));
  TEST_ASSERT_EQUAL_HEX8(0x00, c.startSpotMask(R, true));
}

namespace {
int stepStart(const dance::Pack& p, int move, int i) {
  int s = 0;
  for (int k = 0; k < i; ++k) s += p.step(move, k).dur;
  return s;
}
}  // namespace

void test_df() {
  using namespace beatscript;
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  const int stepTouch = d.viewIndex(kStepTouch), look = d.viewIndex(kLook);
  TEST_ASSERT_EQUAL_INT(12, look);
  ctx("Mf on the fixture");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(ch, s, 2.0);
  // step-touch's third step starts at tick 12 (four 6-tick steps).
  const uint32_t pos = static_cast<uint32_t>(stepStart(d.pack, kStepTouch, 2)) * 65536u;
  TEST_ASSERT_EQUAL_UINT32(12u * 65536u, pos);
  ch.freeze(stepTouch, pos, true);
  step(ch, s);
  {
    const Cue& q = ch.cue();
    CHECK_EQ(stepTouch, q.beatMove, "Mf's move");
    CHECK_EQ(pos, q.beatPos16, "exact pos16");
    CHECK(q.u == pos / 786432.0, "u %.6f", q.u);
    CHECK(q.beatMirror && q.showDance && q.weight == 1.0f && q.energy == 65536 && !q.flash && q.k == 0, "Mf's cue");
  }
  // Mf wins over k.
  s.freezeK(5);
  step(ch, s);
  CHECK_EQ(stepTouch, ch.cue().beatMove, "Mf over k");
  CHECK_EQ(pos, ch.cue().beatPos16, "Mf over k");
  s.freezeK(-1);
  // An idle move: look's second step starts at 50 units (500 ms).
  const uint32_t ipos = static_cast<uint32_t>(stepStart(d.pack, kLook, 1)) * 65536u;
  ch.freeze(look, ipos, true);
  step(ch, s);
  {
    const Cue& q = ch.cue();
    CHECK_EQ(look, q.idleMove, "Mf's idle move");
    CHECK_EQ(500, q.idleMs, "idleMs = 10 * the step start");
    CHECK(q.idleMirror && !q.showDance && q.weight == 0.0f && q.energy == 39322 && !q.flash, "Mf's idle cue");
  }
  // The engine moves.
  ch.freeze(kAutoIdle, 200u * 65536u, false);
  step(ch, s);
  CHECK(ch.cue().idleMove == kAutoIdle && ch.cue().idleMs == 2000 && !ch.cue().showDance, "Mf on auto-idle");
  ch.freeze(kAutoBounce, 98304, false);
  step(ch, s);
  CHECK(ch.cue().beatMove == kAutoBounce && ch.cue().beatPos16 == 98304 && ch.cue().showDance, "Mf on auto-bounce");
  // freeze(kNoMove), or a value that isn't a move, unfreezes.
  ch.freeze(kNoMove, 0, false);
  step(ch, s);
  CHECK(ch.cue().state == DanceState::Dancing && ch.cue().beatMove == ch.run().move, "unfrozen");
  ch.freeze(stepTouch, pos, false);
  step(ch, s);
  CHECK_EQ(pos, ch.cue().beatPos16, "frozen again");
  ch.freeze(99, 0, false);
  step(ch, s);
  CHECK(ch.cue().beatMove == ch.run().move, "99 unfreezes");
  // A switch unfreezes.
  ScriptCharacter other;
  other.beatMove("o", 1, 0, 0);
  ch.freeze(stepTouch, pos, false);
  step(ch, s);
  ch.setCharacter(&other);  // while dancing: at the next whole dance beat
  for (int i = 0; i < 60 && ch.character() != &other; ++i) step(ch, s);
  CHECK(ch.character() == &other, "switched");
  step(ch, s);
  CHECK(ch.cue().beatMove != stepTouch || ch.cue().beatPos16 != pos, "Mf cleared by the switch");
}

void test_freezes_leave_the_state_machine() {
  // While locked with w <= 0.5 the dancer idles: the run in progress ends
  // and none follows (U7 picks only above 0.5). k<n> shows beat frames, but
  // the state machine's showDance stays false, so a switch applies at once.
  ScriptCharacter c1, c2;
  c1.beatMove("m", 1, 0, 0);
  c2.beatMove("n", 1, 0, 0);
  ctx("a freeze over a faded dancer");
  Twin ch;
  ch.setCharacter(&c1);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(ch, s, 2.0);
  s.confidence(0.15f);  // danceWeight 0.02: the weight fades while locked
  feed(ch, s, 3.0);     // past the end of the run in progress (4 dance beats, 2 s)
  CHECK(ch.cue().weight <= 0.5f, "faded: w %g", ch.cue().weight);
  CHECK(ch.run().status == RunStatus::None, "no run follows at w <= 0.5");
  CHECK(ch.cue().state == DanceState::Listening && !ch.cue().showDance, "Listening, idle frames");
  const double clock = ch.danceClock();
  s.freezeK(4);
  step(ch, s);
  CHECK(ch.cue().showDance, "the freeze shows beat frames");
  feed(ch, s, 1.0);
  // 31 frames at 120 BPM: 2 dance beats a second.
  CHECK(std::fabs(ch.danceClock() - clock - 62.0 * static_cast<double>(s.dt())) < 1e-6,
        "the clock runs on under the freeze");
  CHECK(ch.run().status == RunStatus::None, "and still no run underneath");
  CHECK(ch.cue().state == DanceState::Listening, "the state underneath");
  ch.setCharacter(&c2);
  CHECK(ch.character() == &c2, "at once: the unfrozen showDance was false");
}

// ===========================================================================
// 17. The console hooks: forceNext, setSpot, addGenre, eligibleNow.

void test_console_hooks() {
  ScriptCharacter c;
  const int U = c.beatMove("u", 1, 0, 0);
  const int R = c.beatMove("r", 2, 170, 180);  // never eligible at 120
  {
    ctx("forceNext");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(U, ch.run().move, "u");
    const RunInfo r = ch.run();
    ch.forceNext(R);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().start == r.end, "taken at the next pick");
    CHECK_EQ(R, ch.run().move, "the forced move");
    CHECK_EQ(4, ch.run().level, "level 4");
    CHECK_EQ(0, ch.run().k, "k 0 when not eligible");
    CHECK_EQ(kNoOctave, ch.eligibleNow(R), "still not eligible");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(U, ch.run().move, "then the normal pick");
    // An idle move forced: the normal pick.
    ScriptCharacter ci;
    const int I = ci.idleMove("i", 1000, 3);
    ci.beatMove("b", 1, 0, 0);
    Twin c2;
    c2.setCharacter(&ci);
    c2.forceNext(I);
    Script s2(30);
    s2.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(c2, s2), g_ctx);
    CHECK(c2.run().level != 4, "an idle move isn't forced");
  }
  {
    ctx("setSpot");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.5);
    ch.setSpot(1);
    CHECK(ch.run().status == RunStatus::None, "the run ended");
    step(ch, s);
    CHECK_EQ(1, ch.cue().spot, "the spot");
    CHECK_EQ(256, ch.cue().rootX, "its root");
    ch.setSpot(-7);
    step(ch, s);
    CHECK_EQ(-1, ch.cue().spot, "clamped");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(-1, ch.run().spot, "dancing resumes there");
  }
  {
    ctx("addGenre: up to 4, cleared by onTrack");
    ScriptCharacter g;
    int tagged[5];
    uint32_t keys[5];
    for (int i = 0; i < 5; ++i) {
      char name[8];
      std::snprintf(name, sizeof name, "g%d", i);
      keys[i] = dancevec::kFold[i * 3].hash;
      dance::MoveLabels l = ScriptCharacter::defaults(name);
      l.beats = 1;
      l.genreCount = 1;
      l.genreKeys = &keys[i];
      tagged[i] = g.add(l);
    }
    Twin ch;
    ch.setCharacter(&g);
    for (int i = 0; i < 5; ++i) ch.addGenre(keys[i]);
    for (int i = 0; i < 5; ++i)
      CHECK_EQ(i < 4 ? 0 : kNoOctave, ch.eligibleNow(tagged[i]), "tag %d", i);
    ch.onTrack(pathHashOf(1), nullptr, 0);
    for (int i = 0; i < 5; ++i) CHECK_EQ(kNoOctave, ch.eligibleNow(tagged[i]), "tag %d after onTrack", i);
    // The track's genres: the first 16 count.
    uint32_t track[17];
    for (int i = 0; i < 17; ++i) track[i] = 0x1000u + static_cast<uint32_t>(i);
    track[15] = keys[0];
    track[16] = keys[1];
    ch.onTrack(pathHashOf(2), track, 17);
    CHECK_EQ(0, ch.eligibleNow(tagged[0]), "the 16th track genre");
    CHECK_EQ(kNoOctave, ch.eligibleNow(tagged[1]), "the 17th track genre");
  }
  {
    ctx("eligibleNow against eligibleK");
    ScriptCharacter e;
    const int A = e.beatMove("any", 2, 0, 0);
    const int H = e.beatMove("half", 4, 50, 60);
    const int Z = e.beatMove("zero", 2, 90, 130);
    const int D = e.beatMove("double", 1, 150, 180);
    const int N = e.beatMove("needs", 2, 0, 0);
    e.labels(N).needs = dance::kNeedsSongBpm;
    const int I = e.idleMove("idle", 1000, 3);
    const int T = e.beatMove("tagged", 1, 0, 0);
    const uint32_t k = kHipHop;
    e.setGenres(T, {k});
    Twin ch;
    ch.setCharacter(&e);
    // t10 = 0: 0 for an unranged move, kNoOctave for a ranged one.
    CHECK_EQ(0, ch.eligibleNow(A), "unranged before a lock");
    CHECK_EQ(kNoOctave, ch.eligibleNow(Z), "ranged before a lock");
    for (double bpm : {88.0, 100.0, 120.0, 150.0}) {
      Script s(30);
      s.acquire(bpm, 0.0);
      for (int i = 0; i < 300; ++i) {
        step(ch, s);
        for (int m = 0; m < e.moveCount(); ++m) {
          const dance::MoveLabels& l = e.move(m);
          int8_t want;
          if (l.kind != dance::MoveKind::Beat || l.needs || l.genreCount) want = kNoOctave;
          else want = dance::eligibleK(ch.t10(), l.paceLo, l.paceHi, ch.prevK(), l.beats, l.travel);
          CHECK_EQ(want, ch.eligibleNow(m), "move %s at t10 %d, prevK %d", l.name, ch.t10(), ch.prevK());
        }
      }
      ch.reset(0);
    }
    for (int m : {-1, e.moveCount(), 64, 1000}) CHECK_EQ(kNoOctave, ch.eligibleNow(m), "move %d", m);
    CHECK_EQ(kNoOctave, ch.eligibleNow(I), "an idle move");
    CHECK_EQ(kNoOctave, ch.eligibleNow(N), "a needs bit");
    (void)H;
    (void)D;
  }
}

// ===========================================================================
// E.2.8's calls, and the size.

void test_calls() {
  {
    ctx("before the first update");
    dance::Choreographer ch;
    const Cue& q = ch.cue();
    CHECK(q.beatMove == kNoMove && q.u == 0.0 && q.beatPos16 == 0 && q.k == 0 && !q.beatMirror, "the beat fields");
    CHECK(q.idleMove == kNoMove && q.idleMs == 0 && !q.idleMirror && q.idleSeconds == 0.0f, "the idle fields");
    CHECK(q.weight == 0.0f && q.energy == 39322 && !q.showDance && !q.flash, "weight, energy, showDance, flash");
    CHECK(q.spot == 0 && q.rootX == 160 && q.rootY == 153 && q.state == DanceState::Quiet, "spot, root, state");
    const RunInfo& r = ch.run();
    CHECK(r.move == kNoMove && r.k == 0 && r.plays == 0 && !r.firstMirror && r.spot == 0 && r.phrase == 0 &&
              r.level == 0 && !r.cut && r.status == RunStatus::None && r.start == 0 && r.end == 0 && r.uEnd == 0.0,
          "run()");
    CHECK(ch.character() == nullptr && ch.t10() == 0 && ch.prevK() == kNoOctave && ch.danceClock() == 0.0,
          "no character, no latch, no clock");
  }
  ScriptCharacter c1, c2, c3;
  c1.beatMove("a1", 1, 0, 0);
  c2.beatMove("a2", 1, 0, 0);
  c3.beatMove("a3", 1, 0, 0);
  {
    ctx("reset: a pending switch becomes the character at once");
    Twin ch;
    ch.setCharacter(&c1);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 2.0);
    ch.setCharacter(&c2);
    CHECK(ch.character() == &c1, "pending");
    ch.reset(1234);
    CHECK(ch.character() == &c2, "the pending switch at once");
    const Cue& q = ch.cue();
    CHECK(q.beatMove == kNoMove && q.weight == 0.0f && q.state == DanceState::Quiet && q.idleMove == kNoMove,
          "cue() back to its first values");
    CHECK(ch.run().status == RunStatus::None && ch.run().move == kNoMove, "run() too");
    CHECK(ch.t10() == 0 && ch.prevK() == kNoOctave && ch.danceClock() == 0.0, "the latch and the clock");
    // Dancing again from scratch.
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(0, ch.run().phrase, "a fresh phrase");
  }
  {
    ctx("setCharacter: the current one drops a pending switch; a second replaces it");
    Twin ch;
    ch.setCharacter(&c1);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 2.0);
    ch.setCharacter(&c2);
    ch.setCharacter(&c1);
    feed(ch, s, 2.0);
    CHECK(ch.character() == &c1, "the pending switch dropped");
    ch.setCharacter(&c2);
    ch.setCharacter(&c3);
    feed(ch, s, 1.0);
    CHECK(ch.character() == &c3, "the second call replaced the pending character");
  }
}

// ===========================================================================
// Added in integration: E.2's rules that E.4's list doesn't name.

// U7: a stall longer than the run picked at E (a forward jump of up to 1.5
// tracker beats passes as a nudge, U4) holds that run at its end at once;
// Entering then replaces it at the next whole dance beat (U9), the phrase
// continuing.
void test_stall_holds_the_new_run() {
  ScriptCharacter c;
  const int M = c.beatMove("one", 1, 0, 0);
  c.labels(M).repeat = 1;  // one play: a run of 1 dance beat at k 0
  ctx("a stall past the new run's end");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  feed(ch, s, 2.0);
  // The last frame before a run's end (a frame is 0.066 dance beats at 120).
  TEST_ASSERT_TRUE_MESSAGE(until(ch, s,
                                 [&] {
                                   return ch.run().status == RunStatus::Running &&
                                          ch.danceClock() + 0.07 >= static_cast<double>(ch.run().end);
                                 },
                                 100),
                           g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(ch.cue().weight > 0.5f, g_ctx);
  const int64_t E = ch.run().end;
  const int8_t p0 = ch.run().phrase;
  const int64_t s0 = ch.run().start;
  s.nudge(1.4);  // this frame's step: 1.4 + 0.066 tracker beats, the same acquisition
  step(ch, s);
  const double C = ch.danceClock();
  CHECK(C >= static_cast<double>(E) + 1.0 && C < static_cast<double>(E) + 1.5, "the clock at %.4f, E %lld", C,
        static_cast<long long>(E));
  const RunInfo r1 = ch.run();
  CHECK(r1.status == RunStatus::Held, "the new run held (status %d)", static_cast<int>(r1.status));
  CHECK_EQ(M, r1.move, "the new run's move");
  CHECK(r1.start == E && r1.end == E + 1, "the new run at E (start %lld, end %lld)",
        static_cast<long long>(r1.start), static_cast<long long>(r1.end));
  CHECK(r1.uEnd == 1.0 && ch.cue().u == 1.0 && ch.cue().beatPos16 == 786432u, "held at its end (u %.4f)",
        ch.cue().u);
  CHECK_EQ(mod8(p0 + (E - s0)), r1.phrase, "its phrase");
  CHECK(ch.cue().showDance && ch.cue().state == DanceState::Entering, "the held step shows, Entering");
  // Entering: the next pick at floor(clock) + 1, the phrase continuing.
  const int64_t enterAt = static_cast<int64_t>(std::floor(C)) + 1;
  for (int i = 0; i < 100 && ch.run().status != RunStatus::Running; ++i) {
    step(ch, s);
    if (ch.run().status == RunStatus::Running) break;
    CHECK(ch.run().status == RunStatus::Held && ch.cue().u == 1.0, "still held");
    CHECK(ch.danceClock() < static_cast<double>(enterAt), "held past the beat");
  }
  const RunInfo r2 = ch.run();
  CHECK(r2.status == RunStatus::Running && r2.start == enterAt, "the re-pick at %lld (got %lld)",
        static_cast<long long>(enterAt), static_cast<long long>(r2.start));
  CHECK_EQ(mod8(r1.phrase + (enterAt - E)), r2.phrase, "the phrase continues");
  CHECK(ch.cue().u == 0.0 && ch.cue().state == DanceState::Dancing, "dancing from u 0");
}

// U8: a switch pending when the run stops running (a pause holds it at
// once, U6) is made on that frame; SWITCH ends the held run, and the new
// character's quiet idle is picked in that update's U10.
void test_pending_switch_on_pause() {
  ScriptCharacter c1, c2;
  c1.beatMove("p1", 1, 0, 0);
  c1.idleMove("i1", 1000, 3);
  c2.beatMove("p2", 1, 0, 0);
  const int I2 = c2.idleMove("i2", 1000, 3);
  ctx("a pending switch on a pause");
  Twin ch;
  ch.setCharacter(&c1);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  feed(ch, s, 1.5);
  CHECK(ch.cue().showDance && ch.run().status == RunStatus::Running, "dancing");
  ch.setCharacter(&c2);
  CHECK(ch.character() == &c1, "pending");
  s.heard(false);
  step(ch, s);
  CHECK(ch.character() == &c2, "switched on the pause's frame");
  CHECK(ch.run().status == RunStatus::None, "the held run ended by the switch");
  CHECK_EQ(kNoMove, ch.cue().beatMove, "the beat fields cleared");
  CHECK(!ch.cue().showDance && ch.cue().state == DanceState::Quiet, "quiet idles");
  CHECK_EQ(I2, ch.cue().idleMove, "the new character's idle, this update");
}

// U1 and U3: dt above 0 only (0 for a negative one or a NaN); the idle clock
// wraps at 2,000 s (E.2.2), exactly.
void test_dt_and_the_idle_clock() {
  ctx("the idle clock");
  Twin ch;
  BeatInput in{};
  in.heard = false;
  in.foldScale = 1.0;
  in.freeze = -1;
  in.dt = 333.25f;
  const float want[8] = {333.25f, 666.5f, 999.75f, 1333.0f, 1666.25f, 1999.5f, 332.75f, 666.0f};
  for (int i = 0; i < 8; ++i) {
    ch.update(in);
    CHECK(ch.cue().idleSeconds == want[i], "frame %d: idleSeconds %.4f, want %.4f", i, ch.cue().idleSeconds,
          want[i]);
  }
  in.dt = -1.0f;
  ch.update(in);
  CHECK(ch.cue().idleSeconds == 666.0f, "a negative dt counts as 0");
  in.dt = std::nanf("");
  ch.update(in);
  CHECK(ch.cue().idleSeconds == 666.0f, "a NaN dt counts as 0");
  Twin w;
  in.dt = 500.0f;
  for (int i = 0; i < 4; ++i) w.update(in);
  CHECK(w.cue().idleSeconds == 0.0f, "2,000 s exactly wraps to 0 (got %.4f)", w.cue().idleSeconds);
  // A NaN dt leaves the weight alone too, locked or not.
  ScriptCharacter c;
  c.beatMove("u", 1, 0, 0);
  Twin d;
  d.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  feed(d, s, 0.5);
  BeatInput q = s.frame();
  const float w0 = d.cue().weight;
  q.dt = std::nanf("");
  d.update(q);
  CHECK(d.cue().weight == w0, "the weight with a NaN dt");
}

// E.2.8 setSpot (Ms, for screenshots): the cue's spot and root change at
// once, before the next update (E.4 item 17: "the cue's spot at once").
void test_set_spot_at_once() {
  ScriptCharacter c;
  c.beatMove("u", 1, 0, 0);
  ctx("setSpot at once");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  feed(ch, s, 0.5);
  ch.setSpot(1);
  CHECK(ch.cue().spot == 1 && ch.cue().rootX == 256 && ch.cue().rootY == 153, "spot %d, root %d,%d",
        ch.cue().spot, ch.cue().rootX, ch.cue().rootY);
  ch.setSpot(-5);
  CHECK(ch.cue().spot == -1 && ch.cue().rootX == 64, "clamped: spot %d, root %d", ch.cue().spot, ch.cue().rootX);
  dance::Choreographer raw;  // before any update
  raw.setSpot(1);
  CHECK(raw.cue().spot == 1 && raw.cue().rootX == 256, "before the first update");
}

// 2.5, E.2.5: the cut move is exempt from no-repeat (it carries on at its
// new octave); a run that isn't cut gets no exemption.
void test_cut_move_carries_on() {
  ScriptCharacter c;
  const int A = c.beatMove("a", 2, 60, 125);  // k 0 at T 120; at T 140 only k -1 (pace 70)
  ctx("the cut move carries on at its new octave");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  CHECK(ch.run().move == A && ch.run().k == 0 && ch.run().level == 2, "a lone move repeats at level 2");
  const RunInfo r0 = ch.run();
  TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.danceClock() >= r0.start + 1.0; }, 200), g_ctx);
  s.acquire(140.0, s.timeOfBeat(static_cast<double>(r0.start + 1)));  // within 1/4 beat
  step(ch, s);
  CHECK_EQ(1400, ch.t10(), "t10");
  CHECK(ch.run().status == RunStatus::Running && ch.run().start == r0.start && ch.run().cut, "A cut");
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  const RunInfo r1 = ch.run();
  CHECK_EQ(A, r1.move, "A again");
  CHECK_EQ(-1, r1.k, "at its new octave");
  const bool aligned = (2 * r1.phrase) % 8 == 0;  // D2 8 at k -1
  CHECK_EQ(aligned ? 0 : 1, r1.level, "no-repeat didn't give way (phrase %d)", r1.phrase);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  CHECK(ch.run().move == A && ch.run().level == 2, "after a run that wasn't cut, level 2 again");
}

// E.2.5's second draw: when the chosen entry matched prevTo, its orientations
// keep only those whose `from` is 0 or prevTo (here one, so no draw).
void test_orientation_keeps_the_match() {
  for (int want = 1; want <= 2; ++want) {
    ScriptCharacter c;
    const int X = c.beatMove("x", 1, 0, 0);
    c.labels(X).toTag = static_cast<uint8_t>(want);
    c.labels(X).toTagM = static_cast<uint8_t>(want);
    const int Y = c.beatMove("y", 1, 0, 0);
    c.labels(Y).flags = dance::kMirror;
    c.labels(Y).fromTag = 1;   // unmirrored from k1
    c.labels(Y).fromTagM = 2;  // mirrored from k2
    ctx("Y's orientation after a run ending on k%d", want);
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    for (int i = 0; i < 200; ++i) {
      ch.onTrack(pathHashOf(130000 + 1000 * want + i), nullptr, 0);
      ch.forceNext(X);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      CHECK(ch.run().move == X && ch.run().level == 4, "restart %d: X forced", i);
      if (!untilNewRun(ch, s, 400)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after X", i));
      CHECK_EQ(Y, ch.run().move, "restart %d: Y", i);
      CHECK_EQ(want == 2, ch.run().firstMirror, "restart %d: Y's orientation", i);
    }
  }
}

// END: a run that didn't complete leaves prevTo 0 (unknown: a wildcard), so
// after X (to k1) is ended early, Y (from k1) and Z (from k2) share 1/2,
// not 100/115.
void test_incomplete_run_leaves_no_key() {
  ScriptCharacter c;
  const int X = c.beatMove("x", 1, 0, 0);
  c.labels(X).toTag = 1;
  c.labels(X).toTagM = 1;
  const int Y = c.beatMove("y", 1, 0, 0);
  c.labels(Y).fromTag = 1;
  c.labels(Y).fromTagM = 1;
  const int Z = c.beatMove("z", 1, 0, 0);
  c.labels(Z).fromTag = 2;
  c.labels(Z).fromTagM = 2;
  ctx("an unfinished run: 1/2");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  int hits = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    ch.onTrack(pathHashOf(140000 + i), nullptr, 0);
    ch.forceNext(X);
    s.restart();
    s.acquire(120.0, s.trackTime());
    if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
    const RunInfo x = ch.run();
    CHECK(x.move == X && x.end - x.start == 4, "restart %d: X forced, 4 dance beats", i);
    if (!until(ch, s, [&] { return ch.danceClock() >= static_cast<double>(x.start) + 1.5; }, 100))
      TEST_FAIL_MESSAGE(msg("restart %d: X's middle", i));
    ch.setSpot(0);  // ends X half way
    if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after X", i));
    CHECK(ch.run().move == Y || ch.run().move == Z, "restart %d: Y or Z", i);
    hits += ch.run().move == Y;
  }
  const double share = static_cast<double>(hits) / n;
  CHECK(std::fabs(share - 0.5) <= 0.05, "Y's share %.3f, want 1/2", share);
}

// SWITCH keeps prevK (it is about the tempo): the new character's first
// ranged run tries the old one's octave first.
void test_switch_keeps_prev_k() {
  ScriptCharacter c1, c2;
  const int H = c1.beatMove("half", 2, 50, 60);  // only k -1 at T 120
  const int Y = c2.beatMove("y", 2, 36, 200);    // k -1 or 0: prevK decides
  ctx("SWITCH keeps prevK");
  Twin ch;
  ch.setCharacter(&c1);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  CHECK(ch.run().move == H && ch.run().k == -1 && ch.prevK() == -1, "half time");
  ch.setCharacter(&c2);
  TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.character() == &c2; }, 200), g_ctx);
  CHECK(ch.run().status == RunStatus::Running && ch.run().move == Y, "c2's first run, at the switch");
  CHECK_EQ(-1, ch.run().k, "the previous ranged run's octave first");
  CHECK_EQ(-1, ch.prevK(), "prevK");
}

// U4: a re-acquire (REPHASE) sets entering = false, so a far one during
// Entering starts it over: the pick comes at the new grid's floor(clock) + 1,
// never at the old grid's beat it has jumped past.
void test_rephase_restarts_entering() {
  ScriptCharacter c;
  c.beatMove("u", 1, 0, 0);
  bool hit = false;
  for (int trial = 0; trial < 8 && !hit; ++trial) {
    ctx("a far re-acquire during Entering, trial %d", trial);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.locked(false);
    s.acquire(120.0, 0.0);
    feed(ch, s, 1.0 + 0.1 * trial);  // an unlocked grid: Listening, the clock running
    s.locked(true);
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.cue().state == DanceState::Entering; }, 60), g_ctx);
    const int64_t enterAt = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
    bool found = false;
    for (int i = 0; i < 30 && ch.cue().state == DanceState::Entering; ++i) {
      const double left = static_cast<double>(enterAt) - ch.danceClock();
      if (left > 0.07 && left <= 0.25) {
        found = true;
        break;
      }
      step(ch, s);
    }
    if (!found) continue;
    s.acquire(120.0, -0.175);  // the new grid 0.35 beat ahead of the old one: far
    step(ch, s);
    const double C = ch.danceClock();
    CHECK(C > static_cast<double>(enterAt), "the clock past the old beat (%.4f, %lld)", C,
          static_cast<long long>(enterAt));
    CHECK(ch.run().status == RunStatus::None && ch.cue().state == DanceState::Entering, "entering again");
    const int64_t again = static_cast<int64_t>(std::floor(C)) + 1;
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s, 60), g_ctx);
    CHECK(ch.run().start == again, "the pick at %lld, want %lld", static_cast<long long>(ch.run().start),
          static_cast<long long>(again));
    CHECK_EQ(0, ch.run().phrase, "the phrase re-anchored there");
    hit = true;
  }
  TEST_ASSERT_TRUE_MESSAGE(hit, "no trial had a frame 0.07-0.25 beat before Entering's beat");
}

// REPHASE: the 1/4-beat line (|delta| > 0.25 is far: the run held), and a
// tie between two shifts keeping the lower j.
void test_rephase_edges() {
  ScriptCharacter c;
  c.beatMove("four", 4, 0, 0);  // one play of 4 dance beats a run
  const double deltas[4] = {0.24, -0.24, 0.26, -0.26};
  for (double d : deltas) {
    ctx("a re-acquire %+.2f beat off", d);
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.6);
    TEST_ASSERT_TRUE_MESSAGE(ch.run().status == RunStatus::Running, g_ctx);
    const double before = ch.danceClock();
    s.acquire(120.0, -0.5 * d);  // the new grid d tracker beats off the old one (S = 1)
    step(ch, s);
    CHECK(std::fabs(ch.danceClock() - (before + 2.0 * s.dt() + d)) < 1e-6, "the clock took the new phase");
    const bool far = std::fabs(d) > 0.25;
    CHECK(ch.run().status == (far ? RunStatus::Held : RunStatus::Running), "status %d",
          static_cast<int>(ch.run().status));
  }
  // A tie: S = 1/2 has two shifts, x and x + 1/2; predicted 1 lies 1/4 from
  // 0.75 and from 1.25 (exact in binary): the lower j, 0.75, wins.
  for (int tie = 0; tie < 2; ++tie) {
    ctx("REPHASE between two shifts, %s", tie ? "a tie" : "j = 1 nearer");
    Twin ch;
    BeatInput in{};
    in.heard = in.grid = in.locked = true;
    in.confidence = 0.8f;
    in.trackerBeats = 0.0;
    in.rawBpm = 120.0f;
    in.foldScale = 1.0;
    in.acquisitions = 1;
    in.freeze = -1;
    ch.update(in);
    CHECK(ch.danceClock() == 0.0, "the clock at 0");
    in.dt = 0.5f;  // predicted: 0 + 0.5 * 120 / 60 = 1
    in.acquisitions = 2;
    in.rawBpm = 240.0f;
    in.foldScale = 0.5;
    in.trackerBeats = tie ? 1.5 : 1.4;  // x = 0.75 (or 0.7), the other shift 1.25 (or 1.2)
    ch.update(in);
    if (tie)
      CHECK(ch.danceClock() == 0.75, "the lower j: %.6f", ch.danceClock());
    else
      CHECK(std::fabs(ch.danceClock() - 1.2) < 1e-9, "j = 1, 0.2 away: %.6f", ch.danceClock());
  }
}

// onTrack (E.2.8): seed(pathHash ^ restartsSeen ^ lastAcqIn), so the next
// pick's draw is exactly the new seed's first below(), with no idle draws in
// between (auto-idle draws nothing).
void test_on_track_reseeds_exactly() {
  ScriptCharacter c;
  const uint32_t w10[3] = {10, 20, 30};
  c.beatMove("m0", 1, 0, 0, 10);
  c.beatMove("m1", 1, 0, 0, 20);
  c.beatMove("m2", 1, 0, 0, 30);
  for (int i = 0; i < 200; ++i) {
    ctx("onTrack's seed, trial %d", i);
    Twin ch;
    ch.setCharacter(&c);
    ch.reset(pathHashOf(150000 + i));
    Script s(30);
    s.acquire(120.0, 0.0);
    for (int k = 0; k < 3; ++k) TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    feed(ch, s, 0.5);
    TEST_ASSERT_TRUE_MESSAGE(ch.run().status == RunStatus::Running, g_ctx);
    const uint32_t h = pathHashOf(160000 + i);
    ch.onTrack(h, nullptr, 0);
    uint32_t seed = h ^ s.restarts() ^ s.acquisitions();
    if (seed == 0) seed = 0x4D50444Eu;
    // The next pick: every move but the running one (no-repeat), at
    // w10 x 100 (no keys) x 35 for a move in the last three runs.
    const int L = ch.run().move;
    std::vector<int> recent;
    for (size_t k = ch.runs.size(); k-- > 0 && recent.size() < 3;) recent.push_back(ch.runs[k].first.move);
    uint32_t W[3] = {0, 0, 0}, sum = 0;
    for (int m = 0; m < 3; ++m) {
      if (m == L) continue;
      const bool rec = std::find(recent.begin(), recent.end(), m) != recent.end();
      W[m] = w10[m] * 100u * (rec ? 35u : 100u);
      sum += W[m];
    }
    packedit::Xorshift32 x(seed);
    uint32_t r = x.below(sum);
    int want = -1;
    for (int m = 0; m < 3; ++m) {
      if (W[m] == 0) continue;
      if (r < W[m]) {
        want = m;
        break;
      }
      r -= W[m];
    }
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK_EQ(0, ch.run().level, "level 0");
    CHECK_EQ(want, ch.run().move, "the move the new seed draws");
  }
}

// syncPath (E.2.8, F.1): what DanceMode does on any change of the queue's key.
// Another path is onTrack's reseed and clears Mg; the kept path changes nothing.
// The two paths that used to leave FNV("") seeding a whole track are the point:
// a dance turned on with nothing to name, then the queue's first track (a
// restart), and the same behind the library's fence (no restart).
namespace {

// The next n runs' picks, each drawn on its own frame.
std::vector<int> nextPicks(Twin& ch, Script& s, int n) {
  std::vector<int> picks;
  for (int i = 0; i < n; ++i) {
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    picks.push_back(ch.run().move * 16 + ch.run().plays);
  }
  return picks;
}

constexpr int kSyncPicks = 12;

}  // namespace

void test_sync_path() {
  ScriptCharacter c;
  c.beatMove("m0", 1, 0, 0, 10);
  c.beatMove("m1", 1, 0, 0, 20);
  c.beatMove("m2", 1, 0, 0, 30);
  uint32_t tag = kHipHop;
  dance::MoveLabels tagged = ScriptCharacter::defaults("tagged");
  tagged.beats = 1;
  tagged.genreCount = 1;
  tagged.genreKeys = &tag;
  const int T = c.add(tagged);

  // The same path: nothing, whichever frame it lands on.
  for (int i = 0; i < 50; ++i) {
    ctx("syncPath, the kept path: trial %d", i);
    const uint32_t h = pathHashOf(300000 + i);
    Twin a, b;
    a.setCharacter(&c);
    b.setCharacter(&c);
    a.reset(h);
    b.reset(h);
    a.addGenre(kHipHop);
    b.addGenre(kHipHop);
    Script sa(30), sb(30);
    sa.acquire(120.0, 0.0);
    sb.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(a, sa), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(b, sb), g_ctx);
    a.syncPath(h);  // the same file under a new key
    CHECK_EQ(0, a.eligibleNow(T), "Mg's key kept");
    CHECK(nextPicks(a, sa, kSyncPicks) == nextPicks(b, sb, kSyncPicks), "the draws untouched");
    a.syncPath(h);
    CHECK_EQ(0, a.eligibleNow(T), "Mg's key kept, again");
  }

  // Another path, the fence's case: FNV("") kept, the music already danced to
  // and no restart. Exactly onTrack(h, nullptr, 0). Against no call at all, the
  // hash alone differs (no Mg there), and it draws differently on some trial.
  int staleDiffers = 0;
  for (int i = 0; i < 50; ++i) {
    ctx("syncPath, a new path with no restart: trial %d", i);
    const uint32_t h = pathHashOf(310000 + i);
    Twin a, b, stale;
    for (Twin* t : {&a, &b, &stale}) {
      t->setCharacter(&c);
      t->reset(dance::kFnvBasis);
    }
    Script sa(30), sb(30), sc(30);
    sa.acquire(120.0, 0.0);
    sb.acquire(120.0, 0.0);
    sc.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(a, sa), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(b, sb), g_ctx);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(stale, sc), g_ctx);
    a.addGenre(kHipHop);
    a.syncPath(h);
    b.onTrack(h, nullptr, 0);
    CHECK_EQ(kNoOctave, a.eligibleNow(T), "Mg's key cleared");
    const std::vector<int> picks = nextPicks(a, sa, kSyncPicks);
    CHECK(picks == nextPicks(b, sb, kSyncPicks), "as onTrack draws");
    staleDiffers += picks != nextPicks(stale, sc, kSyncPicks);
    a.addGenre(kHipHop);
    a.syncPath(h);  // the same file re-keyed after it: nothing
    CHECK_EQ(0, a.eligibleNow(T), "a key added since is kept");
  }
  CHECK(staleDiffers > 0, "FNV(\"\") and the path never drew differently");

  // Another path, the empty queue's case: the Dance tab up with no track, then
  // the first track's restart. As if the dance had come on with the track.
  staleDiffers = 0;
  for (int i = 0; i < 50; ++i) {
    ctx("syncPath, the first track of an empty queue: trial %d", i);
    const uint32_t h = pathHashOf(320000 + i);
    Twin fixed, want, stale;
    fixed.setCharacter(&c);
    want.setCharacter(&c);
    stale.setCharacter(&c);
    fixed.reset(dance::kFnvBasis);
    want.reset(h);
    stale.reset(dance::kFnvBasis);
    Script sf(30), sw(30), ss(30);
    feed(fixed, sf, 0.5);
    feed(want, sw, 0.5);
    feed(stale, ss, 0.5);
    fixed.addGenre(kHipHop);
    fixed.syncPath(h);
    CHECK_EQ(kNoOctave, fixed.eligibleNow(T), "Mg's key cleared");
    for (Script* s : {&sf, &sw, &ss}) {
      s->restart();
      s->acquire(120.0, s->trackTime());
    }
    const std::vector<int> picks = nextPicks(fixed, sf, kSyncPicks);
    CHECK(picks == nextPicks(want, sw, kSyncPicks), "as a dance that came on with the track");
    staleDiffers += picks != nextPicks(stale, ss, kSyncPicks);
  }
  CHECK(staleDiffers > 0, "FNV(\"\") and the path never drew differently");
}

// U8: a running run without lockedOk is ENDed as soon as w <= 0.5, before
// its cut point: idle frames take over, the state Listening.
void test_lost_lock_fades_mid_run() {
  ScriptCharacter c;
  const int H = c.beatMove("half", 4, 50, 60);  // k -1 at T 120: a move beat is 1 s
  ctx("a lost lock fading before the cut point");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  CHECK(ch.run().move == H && ch.run().k == -1, "half time");
  const int64_t start = ch.run().start;
  auto trueU = [&] { return (ch.danceClock() - static_cast<double>(start)) * 0.5; };
  TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return trueU() > 1.0; }, 200), g_ctx);
  CHECK(trueU() <= 1.05 && ch.cue().weight > 0.9f, "just past move beat 1 (u %.4f, w %g)", trueU(),
        ch.cue().weight);
  s.locked(false);
  step(ch, s);
  CHECK(ch.run().status == RunStatus::Running && ch.run().uEnd == 2.0, "cut to move beat 2");
  int frames = 1;
  while (ch.cue().weight > 0.5f && frames < 30) {
    CHECK(ch.run().status == RunStatus::Running && ch.cue().state == DanceState::Leaving, "Leaving");
    step(ch, s);
    ++frames;
  }
  CHECK(ch.cue().weight <= 0.5f && trueU() < 2.0, "w %g at u %.4f", ch.cue().weight, trueU());
  CHECK(ch.run().status == RunStatus::None, "the run ended at w <= 0.5 (status %d)",
        static_cast<int>(ch.run().status));
  CHECK(ch.cue().state == DanceState::Listening && !ch.cue().showDance, "idle frames, Listening");
}

// E.2.7: a whole move beat is drawn exactly once, on the first frame at or
// after it (b > prevU); a run held at u 0 (a backward jump past its start)
// then draws its true u, not 0 again.
void test_must_show_once() {
  ScriptCharacter c;
  c.beatMove("m", 4, 0, 0);
  ctx("must-show after u held at 0");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  const int64_t start = ch.run().start;
  CHECK(ch.cue().u == 0.0, "the run's first frame at u 0");
  s.nudge(-0.2);  // within 1/4: the run goes on, its u below its start
  step(ch, s);
  CHECK(ch.danceClock() < static_cast<double>(start) && ch.cue().u == 0.0, "held at u 0");
  int checked = 0;
  for (int i = 0; i < 10 && checked == 0; ++i) {
    step(ch, s);
    const double u = ch.danceClock() - static_cast<double>(start);
    if (u <= 0.0) {
      CHECK(ch.cue().u == 0.0, "still at u 0");
      continue;
    }
    CHECK(ch.cue().u == u, "u %.6f drawn as %.6f: beat 0 was drawn already", u, ch.cue().u);
    ++checked;
  }
  CHECK(checked == 1, "a frame past the start");
}

// Section 8's bias is for moves heading toward the centre only: a travel of
// +2 from spot -1 ends at +1, as far out, so it gets no x4 on the first run.
void test_bias_only_toward_the_centre() {
  ScriptCharacter c;
  const int T = c.beatMove("t2", 2, 0, 0);
  c.labels(T).travel = 2;
  c.beatMove("p", 2, 0, 0);
  ctx("no bias across the centre: 1/2");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  int hits = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    ch.setSpot(-1);
    ch.onTrack(pathHashOf(170000 + i), nullptr, 0);
    s.restart();
    s.acquire(120.0, s.trackTime());
    if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
    CHECK(ch.run().phrase == 0 && ch.run().spot == -1, "restart %d: p 0 from -1", i);
    hits += ch.run().move == T;
  }
  const double share = static_cast<double>(hits) / n;
  CHECK(std::fabs(share - 0.5) <= 0.05, "T's share %.3f, want 1/2", share);
}

// TRACK RESET clears the phrase itself (not only through a frame without a
// grid): a restart whose new grid applies on its own frame, its beats
// numbered from 3, starts a fresh phrase at its first pick.
void test_track_reset_clears_the_phrase() {
  ScriptCharacter c;
  c.beatMove("u", 1, 0, 0);
  ctx("a track reset with the new grid on its frame");
  Twin ch;
  ch.setCharacter(&c);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  feed(ch, s, 3.0);
  s.restart();
  s.acquire(120.0, s.trackTime(), 3);
  step(ch, s);
  CHECK(ch.t10() == 1200 && ch.run().status == RunStatus::None, "reset, latched again");
  CHECK(ch.cue().state == DanceState::Entering, "entering on the new grid");
  const int64_t enterAt = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s, 60), g_ctx);
  CHECK(ch.run().start == enterAt, "the first pick at %lld", static_cast<long long>(enterAt));
  CHECK_EQ(0, ch.run().phrase, "a fresh phrase");
}

// ===========================================================================
// Added in review (E.4 item 20): each test fails when its rule is broken.

namespace {
// The drawn root x of a cue on a DrawnCharacter: its beat frame's while beat
// frames show, else its idle frame's (B.4: idle pos16 = idleMs * 65536 // 10).
int drawnRootX(const dance::DrawnCharacter& c, const Cue& q) {
  if (q.showDance)
    return c.frameAt(q.beatMove, q.beatPos16, q.beatMirror, q.spot, static_cast<int32_t>(q.energy)).rootX;
  return c.frameAt(q.idleMove, (static_cast<int64_t>(q.idleMs) * 65536) / 10, q.idleMirror, q.spot).rootX;
}

// The seed U2 gives the first update after reset(h).
uint32_t seedOf(uint32_t h, const BeatInput& first) {
  const uint32_t s = h ^ first.restarts ^ first.acquisitions;
  return s ? s : 0x4D50444Eu;
}
}  // namespace

// U4 (FOLD-DRIFT): S can change without an acquisition when the PLL's drift
// carries the folded tempo past 176 (or under 72) after a re-acquire kept
// the old scale near the edge. That frame re-phases, so the clock's whole
// beats stay on the tracker's (sub)beats: clock - S * trackerBeats stays a
// multiple of min(1, S) on every frame.
void test_fold_drift_rephases() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  const struct {
    double edge, drift, scale;
  } cases[] = {{175.9, 176.3, 0.5}, {72.2, 71.8, 2.0}};
  for (const auto& k : cases) {
    ctx("a drift from %.1f to %.1f BPM, the same acquisition", k.edge, k.drift);
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 4.0);
    reacquireInPhase(s, k.edge);  // inside 72..176: S stays 1
    bool flipped = false;
    int framesAfter = 0;
    uint32_t acq = 0;
    for (int i = 0; i < 30 * 16; ++i) {
      if (i == 30 * 6) {
        acq = s.acquisitions();
        s.drift(k.drift);
      }
      const BeatInput in = s.frame();
      ch.update(in);
      if (!in.grid) continue;
      const double S = in.foldScale;
      const double unit = S < 1.0 ? S : 1.0;
      const double off = (ch.danceClock() - S * in.trackerBeats) / unit;
      CHECK(std::fabs(off - std::floor(off + 0.5)) < 1e-6, "frame %d: the clock %.4f off the beat (S %.2f)", i,
            (off - std::floor(off + 0.5)) * unit, S);
      if (i < 30 * 6) CHECK(S == 1.0, "S %.2f before the drift", S);
      if (i >= 30 * 6) {
        CHECK_EQ(acq, in.acquisitions, "no acquisition");
        flipped = flipped || S == k.scale;
        ++framesAfter;
      }
    }
    CHECK(flipped, "the fold flipped to %.1f", k.scale);
    CHECK(ch.run().status == RunStatus::Running && ch.cue().showDance, "dancing on after the flip");
    CHECK(ch.runs.back().first.start > static_cast<int64_t>(std::floor(ch.danceClock())) - 8, "runs after the flip");
    (void)framesAfter;
  }
}

// TRAVEL-TELEPORT and E.2.7's latch: a travel, once shown, shows to its end
// even when w falls under 0.5 on the way, so the drawn dancer walks to its
// destination and the idle takes over there; and no run starts while w <=
// 0.5 (U7), so no travel plays unseen and moves the idle dancer.
void test_travel_stays_seen() {
  using namespace beatscript;
  CardDancer& d = fixture();
  const dance::DrawnCharacter& c = d.character();
  const int walk = d.viewIndex(kWalk), bounce = d.viewIndex(kBounce);
  {
    ctx("walk from -1, w falling under 0.5 on the way");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    ch.setSpot(-1);
    ch.forceNext(walk);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    const RunInfo r = ch.run();
    CHECK(r.move == walk && r.spot == -1 && !r.firstMirror && r.plays == 1, "walk from -1, one play");
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.danceClock() >= r.start + 0.5; }, 100), g_ctx);
    s.confidence(0.15f);  // danceWeight 0.02
    int prev = drawnRootX(c, ch.cue()), maxJump = 0;
    bool hidden = false, sawLow = false;
    for (int i = 0; i < 30 * 6; ++i) {
      step(ch, s);
      const Cue& q = ch.cue();
      const int x = drawnRootX(c, q);
      maxJump = std::max(maxJump, std::abs(x - prev));
      prev = x;
      if (ch.run().status != RunStatus::None && ch.run().start == r.start) {
        CHECK(q.showDance, "frame %d: the walk hidden at w %g", i, q.weight);
        sawLow = sawLow || q.weight <= 0.5f;
      } else {
        hidden = true;
        CHECK_EQ(0, q.spot, "frame %d: the walk landed", i);
        CHECK(!q.showDance && ch.run().status == RunStatus::None, "frame %d: idles after it (w %g)", i, q.weight);
      }
    }
    CHECK(sawLow && hidden, "the walk ran on at w <= 0.5, then idles");
    CHECK(maxJump <= 48, "the drawn root jumped %d px in a frame", maxJump);
  }
  {
    ctx("no run while w <= 0.5: the spot stays");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    ch.setSpot(-1);
    ch.forceNext(bounce);  // no travel: from -1, then walk could follow
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    CHECK(ch.run().move == bounce && ch.run().spot == -1, "bounce from -1");
    feed(ch, s, 0.3);
    s.confidence(0.15f);
    const int runs = ch.runCount();
    int prev = drawnRootX(c, ch.cue()), maxJump = 0;
    for (int i = 0; i < 30 * 20; ++i) {
      step(ch, s);
      const int x = drawnRootX(c, ch.cue());
      maxJump = std::max(maxJump, std::abs(x - prev));
      prev = x;
      CHECK_EQ(-1, ch.cue().spot, "frame %d: the spot", i);
    }
    CHECK_EQ(runs, ch.runCount(), "no run started at w <= 0.5");
    CHECK(ch.run().status == RunStatus::None && !ch.cue().showDance, "idling");
    CHECK(maxJump <= 48, "the drawn root jumped %d px in a frame", maxJump);
  }
}

// SHOWDANCE-FLICKER: E.2.7's latch. With confidence wobbling around 0.31
// (danceWeight 0.5) beat frames and idle frames each show for at least a
// move beat (less a frame), where `run exists and w > 0.5` flickers for a
// frame or two; and Entering doesn't start a run whose weight is falling
// through 0.5 (U9: the target at 0.5 or below waits for the next beat).
void test_show_latch() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);  // 4 plays of 1 move beat (0.5 s at 120)
  c.idleMove("l", 1000, 3);
  {
    ctx("confidence wobbling around 0.31");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    s.acquire(120.0, 0.0);
    feed(ch, s, 2.0);
    // A slow swing through danceWeight 0.5 with a fast wobble on it: near
    // each crossing w goes over and under 0.5 every few frames.
    int seg = 0, segNaive = 0, minSeg = 1 << 30, naiveShort = 0, toggles = 0, naiveToggles = 0;
    bool show = ch.cue().showDance, naive = show, counting = false;
    for (int i = 0; i < 30 * 120; ++i) {
      const double t = s.trackTime();
      const double tau = 2.0 * 3.14159265358979;
      s.confidence(static_cast<float>(0.31 + 0.012 * std::sin(t * tau / 3.1) + 0.035 * std::sin(t * tau / 0.21)));
      step(ch, s);
      const Cue& q = ch.cue();
      const bool n = ch.run().status != RunStatus::None && q.weight > 0.5f;
      if (q.showDance != show) {
        if (counting) minSeg = std::min(minSeg, seg);  // a whole segment, shown or not
        counting = true;
        ++toggles;
        seg = 0;
        show = q.showDance;
      }
      ++seg;
      if (n != naive) {
        if (segNaive < 5) ++naiveShort;
        ++naiveToggles;
        segNaive = 0;
        naive = n;
      }
      ++segNaive;
    }
    CHECK(naiveShort > 0, "the scenario wobbles: %d short naive segments", naiveShort);
    CHECK(toggles > 4 && toggles < naiveToggles, "%d toggles, %d naive", toggles, naiveToggles);
    CHECK(minSeg >= 14, "frames of one kind for only %d frames (a move beat is 15)", minSeg);
  }
  {
    ctx("Entering with the weight falling through 0.5");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.locked(false);
    s.acquire(120.0, 0.0);
    feed(ch, s, 1.0);
    // Lock half way between beats: w passes 0.5 nine frames (0.6 beat)
    // later, so Entering waits most of a beat, w rising well above 0.5.
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] {
      const double f = ch.danceClock() - std::floor(ch.danceClock());
      return f >= 0.45 && f < 0.52;
    }, 60), g_ctx);
    s.locked(true);
    s.confidence(0.8f);
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.cue().state == DanceState::Entering; }, 60), g_ctx);
    const int64_t enterAt = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
    // Two frames before the beat the confidence drops: w is still above 0.5
    // at the beat, the target far below.
    TEST_ASSERT_TRUE_MESSAGE(
        until(ch, s, [&] { return ch.danceClock() >= static_cast<double>(enterAt) - 0.14; }, 60), g_ctx);
    s.confidence(0.15f);
    bool atBeat = false;
    for (int i = 0; i < 90; ++i) {
      step(ch, s);
      if (!atBeat && ch.danceClock() >= static_cast<double>(enterAt)) {
        atBeat = true;
        CHECK(ch.weight() > 0.5f && ch.target() <= 0.5f, "at the beat: w %g, target %g", ch.weight(), ch.target());
      }
      CHECK(ch.run().status == RunStatus::None && !ch.cue().showDance, "frame %d: no run", i);
    }
    CHECK(atBeat && ch.runCount() == 0, "no pick");
    s.confidence(0.8f);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s, 60), g_ctx);
    CHECK(ch.danceClock() - std::floor(ch.danceClock()) < 0.1 && ch.run().start == static_cast<int64_t>(std::floor(ch.danceClock())),
          "in on a whole beat");
  }
}

// NUM-1, NUM-3, NUM-5: inputs outside the contract never poison the state.
// One bad frame in a dance, then normal input: everything finite and in
// range on every frame (the Twin checks the cue), and the dancer dances
// again; under native_asan with float-cast-overflow, no conversion is
// undefined.
void test_hostile_inputs() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  c.idleMove("l", 1000, 3);
  const double inf = std::numeric_limits<double>::infinity(), nan = std::numeric_limits<double>::quiet_NaN();
  const float finf = std::numeric_limits<float>::infinity(), fnan = std::numeric_limits<float>::quiet_NaN();
  struct Bad {
    const char* what;
    int field;  // 0 confidence, 1 dt, 2 trackerBeats, 3 foldScale, 4 rawBpm
    double v;
  };
  const Bad bads[] = {{"confidence NaN", 0, nan},   {"confidence inf", 0, inf},   {"confidence -inf", 0, -inf},
                      {"confidence 5", 0, 5.0},     {"dt inf", 1, inf},           {"dt NaN", 1, nan},
                      {"dt 1e30", 1, 1e30},         {"dt 2049", 1, 2049.0},       {"dt -1", 1, -1.0},
                      {"beats NaN", 2, nan},        {"beats inf", 2, inf},        {"beats 1e300", 2, 1e300},
                      {"beats -2^41", 2, -2199023255552.0}, {"scale inf", 3, inf}, {"scale NaN", 3, nan},
                      {"scale 1e300", 3, 1e300},    {"scale 1e-300", 3, 1e-300},  {"scale -1", 3, -1.0},
                      {"bpm inf", 4, inf},          {"bpm NaN", 4, nan},          {"bpm 3e38", 4, 3e38},
                      {"bpm -5", 4, -5.0}};
  for (const Bad& b : bads) {
    for (int rest = 0; rest < 2; ++rest) {  // then normal input; or a restart and a new grid first
      ctx("one frame of %s%s", b.what, rest ? ", then a restart" : "");
      Twin ch;
      ch.keepLog = false;
      ch.setCharacter(&c);
      Script s(30);
      s.acquire(120.0, 0.0);
      feed(ch, s, 10.0);
      BeatInput in = s.frame();
      switch (b.field) {
        case 0: in.confidence = std::isnan(b.v) ? fnan : (std::isinf(b.v) ? (b.v > 0 ? finf : -finf) : static_cast<float>(b.v)); break;
        case 1: in.dt = std::isnan(b.v) ? fnan : (std::isinf(b.v) ? finf : static_cast<float>(b.v)); break;
        case 2: in.trackerBeats = b.v; break;
        case 3: in.foldScale = b.v; break;
        default: in.rawBpm = std::isnan(b.v) ? fnan : (std::isinf(b.v) ? finf : static_cast<float>(b.v)); break;
      }
      ch.update(in);
      if (rest) {
        s.restart();
        feed(ch, s, 0.2);
        s.acquire(120.0, s.trackTime());
      }
      int dancing = 0;
      for (int i = 0; i < 30 * 10; ++i) {
        step(ch, s);
        const Cue& q = ch.cue();
        CHECK(std::isfinite(ch.danceClock()) && std::fabs(ch.danceClock()) < 4503599627370496.0, "frame %d: clock", i);
        CHECK(std::isfinite(q.u) && q.u >= 0.0, "frame %d: u %g", i, q.u);
        CHECK(q.idleMs < 40000u, "frame %d: idleMs %u", i, q.idleMs);
        dancing += q.state == DanceState::Dancing && q.showDance;
      }
      CHECK(dancing > 30 * 5, "dancing again: %d frames of 300", dancing);
      CHECK(ch.weight() > 0.95f, "w %g", ch.weight());
    }
  }
  {
    // NUM-5(a): a continuous idle (idleMs 0) for 50+ days in 1,000 s frames
    // (U1's cap): its ms stay below 2^31, wrapping there.
    ScriptCharacter cc;
    cc.idleMove("c", 0, 3);
    ctx("a continuous idle for 58 days");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&cc);
    Script s(30);
    s.heard(false);
    uint32_t last = 0;
    int wraps = 0;
    for (int i = 0; i < 5000; ++i) {
      BeatInput in = s.frame();
      in.dt = i == 0 ? 0.0f : 1000.0f;
      ch.update(in);
      const uint32_t ms = ch.cue().idleMs;
      CHECK(ms < 2147483648u, "frame %d: idleMs %u", i, ms);
      if (ms < last) ++wraps;
      last = ms;
    }
    CHECK(wraps >= 2, "%d wraps", wraps);
  }
  {
    // A frame gap far longer than a timed idle run: one re-pick, and the new
    // run starts at 0 rather than re-picking every frame until its ms run down.
    ScriptCharacter ct;
    ct.idleMove("a", 1000, 3);
    ct.idleMove("b", 1500, 3);
    ctx("a timed idle after a gap of 1e30 s");
    Twin ch;
    ch.setCharacter(&ct);
    Script s(30);
    feed(ch, s, 1.0);
    BeatInput in = s.frame();
    in.dt = 1e30f;
    ch.update(in);
    const int after = ch.cue().idleMove;
    CHECK_EQ(0, ch.cue().idleMs, "the new run starts at 0");
    for (int i = 0; i < 20; ++i) {
      step(ch, s);
      CHECK_EQ(after, ch.cue().idleMove, "frame %d: the same idle run", i);
    }
  }
}

// NUM-4: a switch while dancing when backward nudges have taken the clock
// more than a beat behind the run's start: switchAt lies below the start,
// and the run's content end is held at 0, not made negative.
void test_switch_behind_the_start() {
  ScriptCharacter c1, c2;
  c1.beatMove("m", 4, 0, 0);
  const int N = c2.beatMove("n", 1, 0, 0);
  ctx("setCharacter with the clock behind the run's start");
  Twin ch;
  ch.setCharacter(&c1);
  Script s(30);
  s.acquire(120.0, 0.0);
  TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
  const RunInfo r = ch.run();
  for (int i = 0; i < 40; ++i) {  // -0.15 a frame against +0.067: a net -0.083, inside the nudge window
    s.nudge(-0.15);
    step(ch, s);
  }
  CHECK(ch.run().status == RunStatus::Running && ch.run().start == r.start, "the run goes on");
  CHECK(ch.danceClock() < static_cast<double>(r.start) - 1.0, "clock %.3f, start %lld", ch.danceClock(),
        static_cast<long long>(r.start));
  CHECK(ch.cue().showDance, "shown");
  const int64_t at = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
  ch.setCharacter(&c2);
  CHECK(ch.character() == &c1, "pending");
  CHECK(ch.run().uEnd == 0.0 && ch.run().end == at, "uEnd %.3f, end %lld", ch.run().uEnd,
        static_cast<long long>(ch.run().end));
  for (int i = 0; i < 60 && ch.character() == &c1; ++i) {
    step(ch, s);
    CHECK(ch.cue().u == 0.0 && ch.cue().beatPos16 == 0, "u %g, pos16 %u", ch.cue().u, ch.cue().beatPos16);
  }
  CHECK(ch.character() == &c2, "switched");
  CHECK(ch.run().move == N && ch.run().start == at, "c2's first run at %lld", static_cast<long long>(at));
}

// M1 and M16 (idles): E.2.6's draws, exactly, on the first update after
// reset(h): the entry (one draw, also for a single candidate), the loops
// (timed idles only), then the orientation (kMirror only).
void test_idle_draws_exactly() {
  {
    ScriptCharacter c;
    const int A = c.idleMove("a", 1000, dance::kWhenListening);
    c.labels(A).flags = dance::kMirror;
    c.idleMove("b", 1500, dance::kWhenListening, 30);
    int mirrored = 0, aRuns = 0;
    for (int i = 0; i < 300; ++i) {
      const uint32_t h = pathHashOf(210000 + i);
      ctx("the first idle pick, pathHash 0x%08X", h);
      Twin ch;
      ch.setCharacter(&c);
      ch.reset(h);
      Script s(30);
      const BeatInput first = s.frame();
      ch.update(first);
      packedit::Xorshift32 x(seedOf(h, first));
      const uint32_t r = x.below(10 + 30);
      const int want = r < 10 ? A : 1;
      CHECK_EQ(want, ch.cue().idleMove, "the idle drawn");
      x.below(3);  // the loops: a timed idle
      const bool o = want == A ? x.below(2) == 1 : false;
      CHECK_EQ(o, ch.cue().idleMirror, "the orientation drawn");
      if (want == A) {
        ++aRuns;
        mirrored += o;
      }
    }
    CHECK(aRuns > 30 && mirrored > 0 && mirrored < aRuns, "a kMirror idle in both orientations: %d of %d", mirrored,
          aRuns);
  }
  {
    // A continuous kMirror idle re-picked on every class change: one entry
    // draw and one orientation draw each, no loops draw.
    ScriptCharacter c;
    const int C = c.idleMove("c", 0, 3);
    c.labels(C).flags = dance::kMirror;
    for (int i = 0; i < 50; ++i) {
      const uint32_t h = pathHashOf(220000 + i);
      ctx("a continuous mirrored idle, pathHash 0x%08X", h);
      Twin ch;
      ch.setCharacter(&c);
      ch.reset(h);
      Script s(30);
      const BeatInput first = s.frame();
      ch.update(first);
      packedit::Xorshift32 x(seedOf(h, first));
      for (int k = 0; k < 12; ++k) {
        if (k > 0) {
          s.heard(k % 2 == 0);
          step(ch, s);
        }
        x.below(10);
        CHECK_EQ(C, ch.cue().idleMove, "the idle");
        CHECK_EQ(x.below(2) == 1, ch.cue().idleMirror, "class change %d: the orientation drawn", k);
      }
    }
  }
}

// M16 (beats): E.2.5's draws over two consecutive picks: the entry draw also
// with one entry, and the orientation draw only when two orientations remain.
void test_beat_draws_exactly() {
  ScriptCharacter c;
  const int U = c.beatMove("u", 1, 0, 0);  // 4 plays: never a twin
  const int M = c.beatMove("m", 1, 0, 0);
  c.labels(M).flags = dance::kMirror;
  int seen[2] = {0, 0};
  for (int i = 0; i < 300; ++i) {
    const uint32_t h = pathHashOf(230000 + i);
    ctx("two picks, pathHash 0x%08X", h);
    Twin ch;
    ch.setCharacter(&c);
    ch.reset(h);
    Script s(30);
    s.acquire(120.0, 0.0);
    const BeatInput first = s.frame();
    ch.update(first);
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    packedit::Xorshift32 x(seedOf(h, first));
    // First pick: two entries at 10 x 100 x 100 each.
    const int one = x.below(200000) < 100000 ? U : M;
    const bool o1 = one == M ? x.below(2) == 1 : false;
    CHECK(ch.run().move == one && ch.run().firstMirror == o1, "the first pick");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
    // Second: the other move alone (no-repeat), one entry, still one draw.
    const int two = one == U ? M : U;
    x.below(100000);
    const bool o2 = two == M ? x.below(2) == 1 : false;
    CHECK(ch.run().move == two && ch.run().firstMirror == o2 && ch.run().level == 0, "the second pick");
    ++seen[one == U];
  }
  CHECK(seen[0] > 0 && seen[1] > 0, "both orders: %d %d", seen[0], seen[1]);
}

// M2: prevTo is the last play's `to`: a mirrorable X alternates, so after an
// even number of plays its end key is the other orientation's.
void test_prev_to_follows_the_last_play() {
  for (int repeat : {0, 3}) {  // 4 plays (last mirrored opposite), 3 plays (last as the first)
    ScriptCharacter c;
    const int X = c.beatMove("x", 1, 0, 0);
    c.labels(X).flags = dance::kMirror;
    c.labels(X).repeat = static_cast<uint8_t>(repeat);
    c.labels(X).toTag = 1;
    c.labels(X).toTagM = 2;
    const int Y = c.beatMove("y", 1, 0, 0);
    c.labels(Y).fromTag = c.labels(Y).fromTagM = 1;
    const int Z = c.beatMove("z", 1, 0, 0);
    c.labels(Z).fromTag = c.labels(Z).fromTagM = 2;
    ctx("prevTo after %s plays", repeat ? "3" : "4");
    Twin ch;
    ch.keepLog = false;
    ch.setCharacter(&c);
    Script s(30);
    int hits = 0;
    const int n = 2000;
    for (int i = 0; i < n; ++i) {
      ch.onTrack(pathHashOf(240000 + 5000 * repeat + i), nullptr, 0);
      ch.forceNext(X);
      s.restart();
      s.acquire(120.0, s.trackTime());
      if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
      const RunInfo x = ch.run();
      CHECK(x.move == X && x.plays == (repeat ? 3 : 4), "restart %d: X forced", i);
      if (!untilNewRun(ch, s, 400)) TEST_FAIL_MESSAGE(msg("restart %d: no pick after X", i));
      const bool lastMirror = x.firstMirror != (((x.plays - 1) & 1) != 0);
      const int favoured = lastMirror ? Z : Y;  // toTagM 2 matches Z, toTag 1 Y
      hits += ch.run().move == favoured;
    }
    const double share = static_cast<double>(hits) / n;
    CHECK(std::fabs(share - 100.0 / 115.0) <= 0.03, "the last play's key won %.3f, want 100/115", share);
  }
}

// M3: a switch clears a forced move (E.2.4), at once and pending.
void test_switch_clears_forced() {
  ScriptCharacter c1, c2;
  c1.beatMove("a", 1, 0, 0);
  const int F = c1.beatMove("f", 1, 0, 0);
  c2.beatMove("p", 1, 0, 0);
  c2.beatMove("q", 1, 0, 0);
  c2.beatMove("r", 1, 0, 0);
  for (int trial = 0; trial < 5; ++trial) {
    {
      ctx("forceNext, then a switch while idle (trial %d)", trial);
      Twin ch;
      ch.setCharacter(&c1);
      ch.reset(pathHashOf(250000 + trial));
      ch.forceNext(F);
      ch.setCharacter(&c2);
      Script s(30);
      s.acquire(120.0, 0.0);
      TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
      CHECK(ch.character() == &c2 && ch.run().level <= 2, "c2's first run at level %d", ch.run().level);
    }
    {
      ctx("forceNext, then a pending switch (trial %d)", trial);
      Twin ch;
      ch.setCharacter(&c1);
      ch.reset(pathHashOf(260000 + trial));
      Script s(30);
      s.acquire(120.0, 0.0);
      TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s), g_ctx);
      feed(ch, s, 0.2);
      ch.forceNext(F);
      ch.setCharacter(&c2);
      CHECK(ch.character() == &c1, "pending");
      for (int i = 0; i < 60 && ch.character() == &c1; ++i) step(ch, s);
      CHECK(ch.character() == &c2 && ch.run().status == RunStatus::Running, "switched");
      CHECK(ch.run().level <= 2, "c2's first run at level %d", ch.run().level);
    }
  }
}

// M4: U9's threshold is w > 0.5: a steady weight of 0.579 (confidence 0.33)
// enters at floor(clock) + 1 after its first frame above 0.5; a steady
// 0.461 (confidence 0.30) never does.
void test_entering_threshold() {
  ScriptCharacter c;
  c.beatMove("m", 1, 0, 0);
  {
    ctx("confidence 0.33: w 0.579");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.confidence(0.33f);
    s.acquire(120.0, 0.0);
    TEST_ASSERT_TRUE_MESSAGE(until(ch, s, [&] { return ch.weight() > 0.5f; }, 120), g_ctx);
    const int64_t enterAt = static_cast<int64_t>(std::floor(ch.danceClock())) + 1;
    CHECK(ch.cue().state == DanceState::Entering, "Entering");
    TEST_ASSERT_TRUE_MESSAGE(untilNewRun(ch, s, 40), g_ctx);
    CHECK(ch.run().start == enterAt, "picked at %lld, want %lld", static_cast<long long>(ch.run().start),
          static_cast<long long>(enterAt));
    feed(ch, s, 5.0);
    CHECK(ch.weight() > 0.57f && ch.weight() < 0.59f && ch.cue().showDance, "dancing at w %g", ch.weight());
  }
  {
    ctx("confidence 0.30: w 0.461");
    Twin ch;
    ch.setCharacter(&c);
    Script s(30);
    s.confidence(0.30f);
    s.acquire(120.0, 0.0);
    feed(ch, s, 20.0);
    CHECK(ch.runCount() == 0 && ch.cue().state == DanceState::Listening, "never enters (w %g)", ch.weight());
  }
}

// M5: the idle class filter, over many seeds: quiet idles only while quiet,
// listening idles only while listening (each class has one, the other two).
void test_idle_classes_over_seeds() {
  ScriptCharacter c;
  const int Q = c.idleMove("q", 1000, dance::kWhenQuiet);
  const int L = c.idleMove("l", 1000, dance::kWhenListening);
  const int Q2 = c.idleMove("q2", 1200, dance::kWhenQuiet);
  for (int i = 0; i < 300; ++i) {
    const uint32_t h = pathHashOf(270000 + i);
    ctx("the classes, pathHash 0x%08X", h);
    Twin ch;
    ch.setCharacter(&c);
    ch.reset(h);
    Script s(30);
    s.heard(false);
    step(ch, s);
    CHECK(ch.cue().idleMove == Q || ch.cue().idleMove == Q2, "a quiet idle while quiet (%d)", ch.cue().idleMove);
    s.heard(true);
    step(ch, s);
    CHECK_EQ(L, ch.cue().idleMove, "the listening idle while listening");
    s.heard(false);
    step(ch, s);
    CHECK(ch.cue().idleMove == Q || ch.cue().idleMove == Q2, "a quiet idle again (%d)", ch.cue().idleMove);
  }
}

// M7: the centre bias for a mirrored travel: from spot +1 a kMirror travel
// can only go mirrored, toward 0, and gets x4 on the first run.
void test_mirrored_travel_bias() {
  ScriptCharacter c;
  const int T = c.beatMove("t", 2, 0, 0);
  c.labels(T).travel = 1;
  c.labels(T).flags = dance::kMirror;
  c.beatMove("p", 2, 0, 0);
  ctx("the centre bias from +1, mirrored: 4/5");
  Twin ch;
  ch.keepLog = false;
  ch.setCharacter(&c);
  Script s(30);
  int hits = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    ch.setSpot(1);
    ch.onTrack(pathHashOf(280000 + i), nullptr, 0);
    s.restart();
    s.acquire(120.0, s.trackTime());
    if (!untilNewRun(ch, s, 200)) TEST_FAIL_MESSAGE(msg("restart %d: no pick", i));
    CHECK(ch.run().phrase == 0 && ch.run().spot == 1, "restart %d: p 0 from +1", i);
    if (ch.run().move == T) {
      CHECK(ch.run().firstMirror, "restart %d: T mirrored", i);
      ++hits;
    }
  }
  const double share = static_cast<double>(hits) / n;
  CHECK(std::fabs(share - 0.8) <= 0.03, "T's share %.4f, want 4/5", share);
}

// M11: the must-show rule's 1/8: a frame 0.2 past a whole move beat keeps
// its true u; one exactly 0.125 past draws the beat.
void test_must_show_eighth() {
  ScriptCharacter c;
  c.beatMove("m", 4, 0, 0);  // one play of 4 move beats at k 0
  ctx("the must-show rule's 1/8");
  Twin ch;
  ch.setCharacter(&c);
  BeatInput in{};
  in.heard = in.grid = in.locked = true;
  in.confidence = 0.8f;
  in.rawBpm = 120.0f;
  in.foldScale = 1.0;
  in.acquisitions = 1;
  in.freeze = -1;
  in.dt = 0.0f;
  double tb = 0.0;
  for (int i = 0; i < 120 && ch.run().status != RunStatus::Running; ++i) {
    in.trackerBeats = tb;
    ch.update(in);
    in.dt = 1.0f / 30.0f;
    tb += 1.0 / 15.0;
  }
  TEST_ASSERT_TRUE_MESSAGE(ch.run().status == RunStatus::Running, g_ctx);
  const double start = static_cast<double>(ch.run().start);
  const double at[] = {0.9, 1.2, 1.9, 2.125, 3.0};
  for (double a : at) {
    in.trackerBeats = start + a;
    ch.update(in);
    const double u = ch.cue().u;
    if (a == 1.2) CHECK(std::fabs(u - 1.2) < 1e-9, "0.2 late: u %.6f, the true u", u);
    if (a == 2.125) CHECK(u == 2.0, "1/8 late: u %.6f, the beat", u);
    if (a == 3.0) CHECK(u == 3.0, "on the beat: u %.6f", u);
  }
}

void test_size() {
  char m[80];
  std::snprintf(m, sizeof m, "sizeof(Choreographer) = %u", static_cast<unsigned>(sizeof(dance::Choreographer)));
  TEST_MESSAGE(m);
  TEST_ASSERT_TRUE_MESSAGE(sizeof(dance::Choreographer) <= 1024, m);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_b7_in_cpp);
  RUN_TEST(test_latch);
  RUN_TEST(test_pace_in_the_pick);
  RUN_TEST(test_i_wonder);
  RUN_TEST(test_four_three);
  RUN_TEST(test_quiet);
  RUN_TEST(test_listening);
  RUN_TEST(test_idle_fallbacks);
  RUN_TEST(test_idle_runs);
  RUN_TEST(test_genre);
  RUN_TEST(test_no_repeat_and_twins);
  RUN_TEST(test_plays);
  RUN_TEST(test_from_to);
  RUN_TEST(test_weights);
  RUN_TEST(test_alignment_and_place);
  RUN_TEST(test_ladder_levels);
  RUN_TEST(test_draw_exactly);
  RUN_TEST(test_travel_on_the_fixture);
  RUN_TEST(test_travel_bias);
  RUN_TEST(test_travel_abandoned);
  RUN_TEST(test_determinism);
  RUN_TEST(test_gapless_join);
  RUN_TEST(test_nudges_and_jumps);
  RUN_TEST(test_leaving);
  RUN_TEST(test_pause_and_resume);
  RUN_TEST(test_track_reset);
  RUN_TEST(test_entering);
  RUN_TEST(test_timing);
  RUN_TEST(test_weight_and_energy);
  RUN_TEST(test_switching);
  RUN_TEST(test_freeze_k);
  RUN_TEST(test_freeze_chain);
  RUN_TEST(test_df);
  RUN_TEST(test_freezes_leave_the_state_machine);
  RUN_TEST(test_console_hooks);
  RUN_TEST(test_calls);
  RUN_TEST(test_stall_holds_the_new_run);
  RUN_TEST(test_pending_switch_on_pause);
  RUN_TEST(test_dt_and_the_idle_clock);
  RUN_TEST(test_set_spot_at_once);
  RUN_TEST(test_cut_move_carries_on);
  RUN_TEST(test_orientation_keeps_the_match);
  RUN_TEST(test_incomplete_run_leaves_no_key);
  RUN_TEST(test_switch_keeps_prev_k);
  RUN_TEST(test_rephase_restarts_entering);
  RUN_TEST(test_rephase_edges);
  RUN_TEST(test_on_track_reseeds_exactly);
  RUN_TEST(test_sync_path);
  RUN_TEST(test_lost_lock_fades_mid_run);
  RUN_TEST(test_must_show_once);
  RUN_TEST(test_bias_only_toward_the_centre);
  RUN_TEST(test_track_reset_clears_the_phrase);
  RUN_TEST(test_fold_drift_rephases);
  RUN_TEST(test_travel_stays_seen);
  RUN_TEST(test_show_latch);
  RUN_TEST(test_hostile_inputs);
  RUN_TEST(test_switch_behind_the_start);
  RUN_TEST(test_idle_draws_exactly);
  RUN_TEST(test_beat_draws_exactly);
  RUN_TEST(test_prev_to_follows_the_last_play);
  RUN_TEST(test_switch_clears_forced);
  RUN_TEST(test_entering_threshold);
  RUN_TEST(test_idle_classes_over_seeds);
  RUN_TEST(test_mirrored_travel_bias);
  RUN_TEST(test_must_show_eighth);
  RUN_TEST(test_size);
  return UNITY_END();
}
