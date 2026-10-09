// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Test-only helpers for step 3's suites (test_dance_choreo,
// test_drawn_character and test_dance_budget): docs/DANCE-SPRITES.md,
// Appendix E.4. Header-only; it may use std::vector and floats because
// nothing here reaches the firmware. Written from the doc alone, without
// reading the implementation.
//
// - Script: one BeatInput a frame at a fixed frame rate, from a scripted
//   tracker. The track time tau advances by the frame's dt only while audio
//   is heard (a pause stands it still); a frame's trackerBeats is index +
//   (tau - beatAt) * bpm / 60 (plus any nudges) while a grid applies, else
//   0; rawBpm is (float) bpm; foldScale is a dance::TempoFold's apply(rawBpm)
//   on frames with a grid and its scale() otherwise, as DanceMode keeps
//   fold_. Calls change what the NEXT frame() reports. The first acquire()
//   starts locked at confidence 0.8 unless locked() or confidence() were
//   called before it.
// - ScriptCharacter: a hand-built dance::Character. beatMove() and
//   idleMove() fill every other label with its default (0, null,
//   StartAt::Any, weight10 10); labels() edits one; setSpots() overrides
//   startSpotMask for a move (otherwise Character's default).
// - CardDancer: kTestPack (or a patched copy) opened as a card pack is
//   (E.1's split): Pack::open with DanceBudget's start-spot hook, a
//   BudgetPass run to its end, then the DrawnCharacter view.
// - miniPack(): an MPDN pack built from a geometry, frame boxes (each a
//   solid box of palette index 1) and move records, for the doc's worked
//   examples (B.3, B.4, B.11).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "DanceBudget.h"
#include "DanceCharacter.h"
#include "DanceChoreo.h"
#include "DancePack.h"
#include "DancePose.h"
#include "DanceRate.h"
#include "DrawnCharacter.h"
#include "PackEdit.h"

namespace beatscript {

// --- the scripted tracker ---------------------------------------------------

class Script {
 public:
  explicit Script(int fps)
      : fps_(fps), dt_(static_cast<float>(dancerate::periodMs(static_cast<uint32_t>(fps))) / 1000.0f) {}

  // From the next frame on:
  Script& heard(bool on) {
    heard_ = on;
    return *this;
  }
  Script& grid(bool on) {
    gridOn_ = on;
    return *this;
  }
  Script& locked(bool on) {
    locked_ = on;
    lockedSet_ = true;
    return *this;
  }
  Script& confidence(float c) {
    confidence_ = c;
    confidenceSet_ = true;
    return *this;
  }
  // A new grid: beat `index` at track time beatAt (s); acquisitions + 1; grid on.
  Script& acquire(double bpm, double beatAt, int32_t index = 0) {
    bpm_ = bpm;
    beatAt_ = beatAt;
    index_ = index;
    shift_ = 0.0;
    ++acquisitions_;
    gridOn_ = true;
    haveGrid_ = true;
    if (!acquiredOnce_) {
      acquiredOnce_ = true;
      if (!lockedSet_) locked_ = true;
      if (!confidenceSet_) confidence_ = 0.8f;
    }
    return *this;
  }
  // The grid moved by `beats` tracker beats, same acquisition.
  Script& nudge(double beats) {
    shift_ += beats;
    return *this;
  }
  // The tempo drifts to `bpm` from now on, same acquisition: the beat count
  // runs on without a jump (a PLL's slow drift), only its rate changes.
  Script& drift(double bpm) {
    const double b = beatsAt(tau_);
    bpm_ = bpm;
    beatAt_ = tau_;
    index_ = 0;
    shift_ = b;
    return *this;
  }
  // restarts + 1, grid off (the tracker has none until the next acquire),
  // the TempoFold reset: DanceMode::restart.
  Script& restart() {
    ++restarts_;
    gridOn_ = false;
    haveGrid_ = false;
    fold_.reset();
    return *this;
  }
  Script& freezeK(int n) {
    freeze_ = static_cast<int8_t>(n);
    return *this;
  }

  // The next frame's input (dt 0 on the first).
  dance::BeatInput frame() {
    const float dt = first_ ? 0.0f : dt_;
    first_ = false;
    if (heard_) tau_ += static_cast<double>(dt);
    dance::BeatInput in{};
    in.heard = heard_;
    const bool g = heard_ && gridOn_ && haveGrid_;
    in.grid = g;
    in.locked = locked_;
    in.confidence = confidence_;
    in.rawBpm = haveGrid_ ? static_cast<float>(bpm_) : 0.0f;
    in.foldScale = g ? fold_.apply(in.rawBpm) : fold_.scale();
    in.trackerBeats = g ? beatsAt(tau_) : 0.0;
    in.restarts = restarts_;
    in.acquisitions = acquisitions_;
    in.dt = dt;
    in.freeze = freeze_;
    ++frames_;
    return in;
  }
  std::vector<dance::BeatInput> seconds(double s) {
    std::vector<dance::BeatInput> out;
    const long n = std::lround(s / static_cast<double>(dt_));
    for (long i = 0; i < n; ++i) out.push_back(frame());
    return out;
  }

  double trackTime() const { return tau_; }
  // The script's own trackerBeats * S at tau (S: the fold's scale now).
  double danceBeats() const { return haveGrid_ ? beatsAt(tau_) * fold_.scale() : 0.0; }
  // The tracker beat at track time t, and the track time of tracker beat b,
  // on the current grid.
  double beatsAt(double t) const { return index_ + shift_ + (t - beatAt_) * bpm_ / 60.0; }
  double timeOfBeat(double b) const { return beatAt_ + (b - index_ - shift_) * 60.0 / bpm_; }
  double bpm() const { return bpm_; }
  double scale() const { return fold_.scale(); }
  float dt() const { return dt_; }
  int fps() const { return fps_; }
  long frames() const { return frames_; }
  uint32_t restarts() const { return restarts_; }
  uint32_t acquisitions() const { return acquisitions_; }

 private:
  int fps_;
  float dt_;
  bool first_ = true;
  double tau_ = 0.0;
  bool heard_ = true;
  bool gridOn_ = false;
  bool haveGrid_ = false;
  bool locked_ = false;
  bool lockedSet_ = false;
  float confidence_ = 0.0f;
  bool confidenceSet_ = false;
  bool acquiredOnce_ = false;
  double bpm_ = 0.0, beatAt_ = 0.0, shift_ = 0.0;
  int32_t index_ = 0;
  uint32_t restarts_ = 0, acquisitions_ = 0;
  int8_t freeze_ = -1;
  long frames_ = 0;
  dance::TempoFold fold_;
};

// --- a hand-built character ---------------------------------------------------

class ScriptCharacter final : public dance::Character {
 public:
  static constexpr int kMax = 64;

  ScriptCharacter() {
    info_.id = "script";
    info_.name = "Script";
    info_.author = "test";
    info_.licence = "GPL-3.0-or-later";
    info_.url = "";
    info_.origin = dance::Origin::BuiltIn;
    info_.inCycle = true;
    info_.ignoredMoves = 0;
  }
  ScriptCharacter(const ScriptCharacter&) = delete;
  ScriptCharacter& operator=(const ScriptCharacter&) = delete;

  const dance::CharacterInfo& info() const override { return info_; }
  int moveCount() const override { return count_; }
  const dance::MoveLabels& move(int i) const override { return moves_[i]; }
  uint8_t startSpotMask(int i, bool mirrored) const override {
    if (i >= 0 && i < count_ && spotsSet_[i]) return spots_[i][mirrored ? 1 : 0];
    return dance::Character::startSpotMask(i, mirrored);
  }

  // Every label at its default: a beat move, 0 / null / StartAt::Any, weight10 10.
  static dance::MoveLabels defaults(const char* name) {
    dance::MoveLabels m{};
    m.name = name;
    m.kind = dance::MoveKind::Beat;
    m.weight10 = 10;
    m.startAt = dance::StartAt::Any;
    m.genreCount = 0;
    m.genreKeys = nullptr;
    return m;
  }
  int add(const dance::MoveLabels& m) {
    if (count_ >= kMax) return -1;
    const int i = count_++;
    moves_[i] = m;
    std::snprintf(names_[i], sizeof names_[i], "%s", m.name ? m.name : "");
    moves_[i].name = names_[i];
    const int n = m.genreCount < 8 ? m.genreCount : 8;
    for (int g = 0; g < n; ++g) genres_[i][g] = m.genreKeys[g];
    moves_[i].genreCount = static_cast<uint8_t>(n);
    moves_[i].genreKeys = n ? genres_[i] : nullptr;
    spotsSet_[i] = false;
    return i;
  }
  int beatMove(const char* name, int beats, int lo, int hi, int weight10 = 10) {
    dance::MoveLabels m = defaults(name);
    m.kind = dance::MoveKind::Beat;
    m.beats = static_cast<uint8_t>(beats);
    m.paceLo = static_cast<uint8_t>(lo);
    m.paceHi = static_cast<uint8_t>(hi);
    m.weight10 = static_cast<uint8_t>(weight10);
    return add(m);
  }
  int idleMove(const char* name, int ms, int when, int weight10 = 10) {
    dance::MoveLabels m = defaults(name);
    m.kind = dance::MoveKind::Idle;
    m.beats = 0;
    m.idleMs = static_cast<uint16_t>(ms);
    m.when = static_cast<uint8_t>(when);
    m.weight10 = static_cast<uint8_t>(weight10);
    return add(m);
  }
  dance::MoveLabels& labels(int i) { return moves_[i]; }
  void setGenres(int i, std::initializer_list<uint32_t> keys) {
    int n = 0;
    for (uint32_t k : keys)
      if (n < 8) genres_[i][n++] = k;
    moves_[i].genreCount = static_cast<uint8_t>(n);
    moves_[i].genreKeys = n ? genres_[i] : nullptr;
  }
  void setSpots(int i, uint8_t unmirrored, uint8_t mirrored) {
    spotsSet_[i] = true;
    spots_[i][0] = unmirrored;
    spots_[i][1] = mirrored;
  }
  int find(const char* name) const {
    for (int i = 0; i < count_; ++i)
      if (std::strcmp(names_[i], name) == 0) return i;
    return -1;
  }

 private:
  dance::CharacterInfo info_{};
  int count_ = 0;
  dance::MoveLabels moves_[kMax]{};
  char names_[kMax][24]{};
  uint32_t genres_[kMax][8]{};
  bool spotsSet_[kMax]{};
  uint8_t spots_[kMax][2]{};
};

// --- a card pack, opened as the loader opens one (E.1's split) ----------------

struct CardDancer {
  packedit::HeapCopy bytes;
  dance::Pack pack;
  dance::Report report;
  packedit::Drops drops;
  std::unique_ptr<dance::DrawnCharacter> view{new dance::DrawnCharacter()};
  int passSteps = 0;
  bool opened = false;

  // Pack::open with the start-spot hook (D.2), a BudgetPass run to its end,
  // then the view. False when the pack is rejected or the view refuses it.
  bool load(const packedit::Bytes& b, dance::Origin origin = dance::Origin::Card, bool inCycle = true) {
    bytes.assign(b);
    drops.attach(report);
    dance::OpenOptions o;
    o.late = &dance::startSpots;
    o.lateCtx = nullptr;
    const uint32_t n = static_cast<uint32_t>(b.size());
    opened = pack.open(bytes.data(), n, n, &report, &o);
    if (!opened) return false;
    drops.lastMove = -1;  // the pass's drops come after open()'s
    dance::BudgetPass pass;
    pass.begin(&pack, &report);
    passSteps = 0;
    while (pass.step()) ++passSteps;
    return view->init(pack, origin, inCycle);
  }
  const dance::DrawnCharacter& character() const { return *view; }
  // The view's index of MOVS record m, or -1.
  int viewIndex(int m) const {
    for (int i = 0; i < view->moveCount(); ++i)
      if (view->packMove(i) == m) return i;
    return -1;
  }
};

// The fixture's moves (C.10), by MOVS index.
enum FixtureMove : int {
  kBounce = 0, kPulse, kStepTouch, kPoint, kSlow, kFast, kWalk, kNod, kFuture, kBig, kHeavy, kSway, kBreathe, kLook,
  kFixtureMoves
};

// --- hand-built packs -------------------------------------------------------------

struct MiniStep {
  uint16_t frame;
  bool mirrored;
  uint16_t dur;
  int8_t dx, dy;
  uint8_t ease;
  bool go;
};
struct MiniMove {
  std::string name;
  uint8_t kind = 0, beats = 1, paceLo = 0, paceHi = 0, weight10 = 10, when = 0, flags = 0, repeat = 0, needs = 0;
  int8_t travel = 0;
  uint8_t startAt = 0, travelEase = 0, bounce = 0, sway = 0;
  std::vector<MiniStep> steps;
};
struct MiniGeom {
  uint8_t scale, cellW, cellH, anchorX, anchorY, shadowW;
};

inline void putString(packedit::Bytes& b, const std::string& s) {
  b.push_back(static_cast<uint8_t>(s.size()));
  b.insert(b.end(), s.begin(), s.end());
  b.push_back(0);
}

// A pack with A.4's nine chunks: META (id "mini"), GEOM from g (faces none,
// rest frame 0), PALS (index 1 red), a blank THMB, one frame per box (each
// row of a box: skip 0, then tokens of index 1 covering its w px), no pose
// keys, and the moves' records and steps.
inline packedit::Bytes miniPack(const MiniGeom& g, const std::vector<dance::FrameBox>& boxes,
                                const std::vector<MiniMove>& moves) {
  using packedit::Bytes;
  Bytes meta;
  putString(meta, "mini");
  putString(meta, "Mini");
  putString(meta, "Ana");
  putString(meta, "CC0-1.0");
  putString(meta, "");
  putString(meta, "");
  meta.push_back(static_cast<uint8_t>(moves.size()));
  Bytes geom = {g.scale, 0, g.cellW, g.cellH, g.anchorX, g.anchorY, g.shadowW, 0, 0, 0, 0, 0};
  packedit::put16(geom, 8, static_cast<uint32_t>(boxes.size()));
  Bytes pals(96, 0);
  pals[3] = 0xFF;
  pals[51] = 0x74;
  pals[52] = 0x1B;
  pals[53] = 0x1B;
  Bytes ftab, fdat;
  for (const dance::FrameBox& f : boxes) {
    Bytes rec(12, 0);
    rec[0] = f.x0;
    rec[1] = f.y0;
    rec[2] = f.w;
    rec[3] = f.h;
    if (f.w) {
      const size_t off = fdat.size();
      for (int y = 0; y < f.h; ++y) {
        int left = f.w;
        Bytes row = {0, 0};
        while (left > 0) {
          const int run = left > 16 ? 16 : left;
          row.push_back(static_cast<uint8_t>((run - 1) << 4 | 1));
          ++row[1];
          left -= run;
        }
        fdat.insert(fdat.end(), row.begin(), row.end());
      }
      packedit::put32(rec, 4, static_cast<uint32_t>(off));
      packedit::put16(rec, 8, static_cast<uint32_t>(fdat.size() - off));
    }
    ftab.insert(ftab.end(), rec.begin(), rec.end());
  }
  Bytes strs = {0, 0, 0, 0};
  std::vector<uint16_t> nameAt;
  for (const MiniMove& m : moves) {
    nameAt.push_back(static_cast<uint16_t>(strs.size()));
    putString(strs, m.name);
  }
  Bytes movs = {static_cast<uint8_t>(moves.size()), 32, 0, 0};
  Bytes step;
  uint16_t first = 0;
  for (size_t i = 0; i < moves.size(); ++i) {
    const MiniMove& m = moves[i];
    Bytes r(32, 0);
    packedit::put16(r, 0, nameAt[i]);
    r[2] = m.kind;
    r[3] = m.beats;
    r[4] = m.paceLo;
    r[5] = m.paceHi;
    r[6] = m.weight10;
    r[7] = m.when;
    r[8] = m.flags;
    r[9] = m.repeat;
    r[10] = m.needs;
    r[11] = static_cast<uint8_t>(m.travel);
    r[12] = m.startAt;
    r[13] = m.travelEase;
    r[14] = m.bounce;
    r[15] = m.sway;
    r[23] = static_cast<uint8_t>(m.steps.size());
    packedit::put16(r, 24, first);
    movs.insert(movs.end(), r.begin(), r.end());
    for (const MiniStep& s : m.steps) {
      Bytes sr(8, 0);
      packedit::put16(sr, 0, static_cast<uint32_t>(s.frame | (s.mirrored ? 0x8000u : 0u)));
      packedit::put16(sr, 2, s.dur);
      sr[4] = static_cast<uint8_t>(s.dx);
      sr[5] = static_cast<uint8_t>(s.dy);
      sr[6] = s.ease;
      sr[7] = s.go ? 1 : 0;
      step.insert(step.end(), sr.begin(), sr.end());
    }
    first = static_cast<uint16_t>(first + m.steps.size());
  }
  return packedit::assemble({{"META", meta},
                             {"GEOM", geom},
                             {"PALS", pals},
                             {"THMB", Bytes(800, 0)},
                             {"FTAB", ftab},
                             {"STRS", strs},
                             {"MOVS", movs},
                             {"STEP", step},
                             {"FDAT", fdat}});
}

// B.4's worked example (scale 2, cell 8x8, anchor 4,8, shadow 10, one frame
// with box (2, 2, 4, 6)), with three moves:
//   0: B.4's 2-beat move: bounce 2, sway 1, travel 1, travelEase inout; step 1
//      frame 0, 12 ticks, key 0,0, ease out; step 2 frame 0 with bit 15, 12
//      ticks, key 3,-2, ease in, `go`;
//   1: the same with travelEase hold (B.4's variant);
//   2: B.3's 1-beat move with sway 2: one step, frame 0, 12 ticks, key 0,0.
inline packedit::Bytes b4Pack() {
  MiniMove m;
  m.name = "b4";
  m.beats = 2;
  m.bounce = 2;
  m.sway = 1;
  m.travel = 1;
  m.travelEase = 4;
  m.steps = {{0, false, 12, 0, 0, 3, false}, {0, true, 12, 3, -2, 2, true}};
  MiniMove hold = m;
  hold.name = "b4-hold";
  hold.travelEase = 0;
  MiniMove sway;
  sway.name = "sway";
  sway.beats = 1;
  sway.sway = 2;
  sway.steps = {{0, false, 12, 0, 0, 0, false}};
  return miniPack({2, 8, 8, 4, 8, 10}, {{2, 2, 4, 6}}, {m, hold, sway});
}

// B.4's clamp example: scale 2, cell 64x76, anchor 32,72, shadow 40, one
// frame with box (0, 10, 64, 62), a 1-beat move of one step keyed -8,0.
inline packedit::Bytes clampPack() {
  MiniMove m;
  m.name = "clamp";
  m.beats = 1;
  m.steps = {{0, false, 12, -8, 0, 0, false}};
  return miniPack({2, 64, 76, 32, 72, 40}, {{0, 10, 64, 62}}, {m});
}

// B.11's wrap example: scale 2, cell 16x8, anchor 8,8, frames with boxes
// (7, 0, 2, 8) and (0, 0, 16, 8); step 1 frame 0 keyed -10,0, step 2 frame 1
// keyed 0,0, both linear (a 2-beat move, 12 ticks each).
inline packedit::Bytes wrapPack() {
  MiniMove m;
  m.name = "wrap";
  m.beats = 2;
  m.steps = {{0, false, 12, -10, 0, 1, false}, {1, false, 12, 0, 0, 1, false}};
  return miniPack({2, 16, 8, 8, 8, 0}, {{7, 0, 2, 8}, {0, 0, 16, 8}}, {m});
}

// A pack opened from an exact-size heap copy, without a hook.
struct OpenPack {
  packedit::HeapCopy bytes;
  dance::Pack pack;
  dance::Report report;
  packedit::Drops drops;
  bool open(const packedit::Bytes& b, const dance::OpenOptions* o = nullptr) {
    bytes.assign(b);
    drops.attach(report);
    const uint32_t n = static_cast<uint32_t>(b.size());
    return pack.open(bytes.data(), n, n, &report, o);
  }
};

}  // namespace beatscript
