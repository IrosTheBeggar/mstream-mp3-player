// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
// Synthetic test audio for the beat tracker's host tests: click trains
// (ClickGen), drum patterns with loud off-beat hi-hats, noise, silence, and
// a way to run a tracker over them and score it against the truth.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "BeatTracker.h"
#include "ClickGen.h"

namespace sig {

constexpr uint32_t kRate = 44100;

inline uint32_t frames(double seconds) { return static_cast<uint32_t>(seconds * kRate); }

// Mono track plus the frames its beats fall on.
struct Track {
  std::vector<int16_t> mono;
  std::vector<double> beats;  // truth, in frames
};

inline int16_t clip(double v) {
  return static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, std::lround(v) * 1.0)));
}

// A ClickGen track at `bpm`, first beat at `offsetBeats` of a period.
inline Track clicks(float bpm, double seconds, float offsetBeats = 0.0f) {
  ClickGen g;
  ClickGen::Spec spec;
  spec.bpm = bpm;
  spec.offsetBeats = offsetBeats;
  const uint32_t n = frames(seconds);
  g.start(kRate, spec, n);
  std::vector<int16_t> stereo(2 * n);
  g.generate(stereo.data(), n);
  Track t;
  t.mono.resize(n);
  for (uint32_t i = 0; i < n; ++i) t.mono[i] = stereo[2 * i];
  for (uint32_t k = 0; g.beatFrame(k) < n; ++k) t.beats.push_back(g.beatFrame(k));
  return t;
}

// Adds white noise at `dbfs` RMS.
inline void addNoise(Track& t, double dbfs, uint32_t seed = 1) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 32768.0 * std::pow(10.0, dbfs / 20.0));
  for (auto& s : t.mono) s = clip(s + gauss(rng));
}

// A kick drum (sine glide 120 -> 50 Hz, ~80 ms) at each beat, and a loud
// hi-hat (high-passed noise burst, ~40 ms) at `hatPhase` of each beat (0.5:
// straight off-beats; 0.66: swung). The hat has next to nothing below the
// tracker's 150 Hz band, so on its own it only checks that the band shuts
// it out. The off-beat content the tracker does hear is optional and sits
// at the same `hatPhase`: a bass note `bassDb` below the kick, and a ghost
// kick (the kick again, a step between) `ghostDb` below it.
inline Track drums(float bpm, double seconds, double hatPhase, double hatDb, double bassDb = -200.0,
                   double ghostDb = -200.0, double firstBeat = 0.0, uint32_t seed = 3) {
  const uint32_t n = frames(seconds);
  Track t;
  std::vector<double> mix(n, 0.0);
  const double period = 60.0 * kRate / bpm;
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 1.0);
  const double kickAmp = 0.35 * 32768;
  for (int k = 0;; ++k) {
    const double at = firstBeat * period + k * period;
    if (at >= n) break;
    t.beats.push_back(at);
    const auto kick = [&](double when, double amp) {
      const auto start = static_cast<uint32_t>(std::lround(when));
      double ph = 0.0;
      for (uint32_t i = 0; i < frames(0.08) && start + i < n; ++i) {
        const double s = static_cast<double>(i) / kRate;
        ph += 2 * M_PI * (50.0 + 70.0 * std::exp(-s / 0.015)) / kRate;
        mix[start + i] += amp * std::sin(ph) * std::exp(-s / 0.03);
      }
    };
    kick(at, kickAmp);
    if (ghostDb > -100) kick(at + hatPhase * period, kickAmp * std::pow(10.0, ghostDb / 20.0));
    // Bass between the beats: a 70 Hz note, 120 ms.
    const auto bassAt = static_cast<uint32_t>(std::lround(at + hatPhase * period));
    const double bassAmp = kickAmp * std::pow(10.0, bassDb / 20.0);
    for (uint32_t i = 0; bassDb > -100 && i < frames(0.12) && bassAt + i < n; ++i) {
      const double s = static_cast<double>(i) / kRate;
      const double env = std::min(1.0, s / 0.004) * std::exp(-s / 0.06);
      mix[bassAt + i] += bassAmp * env * std::sin(2 * M_PI * 70.0 * s);
    }
    // Hi-hat: noise, differentiated twice (a crude high-pass), fast decay.
    const auto hatAt = static_cast<uint32_t>(std::lround(at + hatPhase * period));
    const double hatAmp = 32768 * std::pow(10.0, hatDb / 20.0);
    double x1 = 0, x2 = 0;
    for (uint32_t i = 0; i < frames(0.04) && hatAt + i < n; ++i) {
      const double s = static_cast<double>(i) / kRate;
      const double x = gauss(rng);
      mix[hatAt + i] += hatAmp * 0.25 * (x - 2 * x1 + x2) * std::exp(-s / 0.012);
      x2 = x1;
      x1 = x;
    }
  }
  t.mono.resize(n);
  for (uint32_t i = 0; i < n; ++i) t.mono[i] = clip(mix[i]);
  return t;
}

// Low-passed noise with a slow swell: wind, pads, an ambient intro.
inline Track ambient(double seconds, double dbfs, uint32_t seed = 5) {
  const uint32_t n = frames(seconds);
  Track t;
  t.mono.resize(n);
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 1.0);
  double y1 = 0, y2 = 0;
  const double a = std::exp(-2 * M_PI * 300.0 / kRate);
  const double amp = 32768 * std::pow(10.0, dbfs / 20.0) * 12.0;
  for (uint32_t i = 0; i < n; ++i) {
    y1 = a * y1 + (1 - a) * gauss(rng);
    y2 = a * y2 + (1 - a) * y1;
    const double swell = 0.6 + 0.4 * std::sin(2 * M_PI * 0.13 * i / kRate);
    t.mono[i] = clip(amp * swell * y2);
  }
  return t;
}

// Joins b after a (b's beats shifted).
inline Track concat(const Track& a, const Track& b) {
  Track t = a;
  const double shift = static_cast<double>(a.mono.size());
  t.mono.insert(t.mono.end(), b.mono.begin(), b.mono.end());
  for (double x : b.beats) t.beats.push_back(x + shift);
  return t;
}

struct Result {
  double lockSeconds = -1;        // first time locked (-1: never)
  std::vector<double> errorsMs;   // |phase error| at each true beat after `scoreFrom`
  std::vector<double> signedMs;
  int lockedBeats = 0, scoredBeats = 0;
  double meanConfidence = 0;
  double lockedFraction = 0;      // of the chunks
  float bpm = 0;
};

// Feeds `t` in chunks like the firmware's tap reader and scores the grid at
// every true beat from `scoreFrom` seconds on: the grid as it stands when
// the audio up to that beat has been fed (a prediction: the PLL only moves a
// beat after its window).
inline Result run(BeatTracker& bt, const Track& t, double scoreFrom, uint32_t chunk = 1024) {
  Result r;
  size_t nextBeat = 0;
  double confSum = 0;
  int chunks = 0, lockedChunks = 0;
  for (uint32_t at = 0; at < t.mono.size(); at += chunk) {
    const uint32_t n = std::min<uint32_t>(chunk, static_cast<uint32_t>(t.mono.size()) - at);
    // Score the beats in this chunk against the grid from before it.
    while (nextBeat < t.beats.size() && t.beats[nextBeat] < at + n) {
      const double beat = t.beats[nextBeat++];
      if (beat < scoreFrom * kRate) continue;
      ++r.scoredBeats;
      if (!bt.locked()) continue;
      ++r.lockedBeats;
      const double truePeriod = nextBeat < t.beats.size() ? t.beats[nextBeat] - beat : beat - t.beats[nextBeat - 2];
      const double d = bt.grid().errorAgainst(beat, truePeriod);
      const double e = d * 1000.0 / kRate;
      r.signedMs.push_back(e);
      r.errorsMs.push_back(std::fabs(e));
    }
    bt.process(t.mono.data() + at, n);
    if (bt.locked() && r.lockSeconds < 0) r.lockSeconds = static_cast<double>(at + n) / kRate;
    confSum += bt.confidence();
    ++chunks;
    if (bt.locked()) ++lockedChunks;
  }
  r.meanConfidence = chunks ? confSum / chunks : 0;
  r.lockedFraction = chunks ? static_cast<double>(lockedChunks) / chunks : 0;
  r.bpm = bt.bpm();
  return r;
}

inline double percentile(std::vector<double> v, double p) {
  if (v.empty()) return 1e9;
  std::sort(v.begin(), v.end());
  const auto i = static_cast<size_t>(std::min<double>(v.size() - 1, std::floor(p * (v.size() - 1) + 0.5)));
  return v[i];
}

}  // namespace sig
