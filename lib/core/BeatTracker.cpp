// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "BeatTracker.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
constexpr double kPi = 3.14159265358979;

// Onset signal.
constexpr float kLevelSeconds = 1.5f;   // the level a rise must reach to count in full
constexpr float kMeanSeconds = 3.0f;    // the onset signal's mean (removed before correlating)
constexpr float kFloorDbfs = -50.0f;    // quieter than this counts as silence
constexpr float kDcHz = 5.0f;
constexpr float kMidWeight = 0.5f;      // the mid band's onsets, relative to the low band's
constexpr float kLinearWeight = 0.05f;  // the low band's linear rise (units of its level), added

// Tempo.
constexpr float kAcfSeconds = 3.0f;     // autocorrelation memory
constexpr float kBpmStep = 1.005f;      // candidate spacing (0.5 %)
// A tempo scores with the autocorrelation at its lag and the next 7
// multiples: a steady beat correlates at every one of them (beats, bars),
// a 4:3 or 3:2 relative (dotted notes, triplets, swing) only at some.
constexpr int kHarmonics = 8;
constexpr float kHarmonicWeight[kHarmonics + 1] = {0.0f, 1.0f, 1.0f, 0.5f, 1.0f, 0.5f, 0.5f, 0.5f, 1.0f};
constexpr uint32_t kEstimateEveryHops = 8;
constexpr float kMinSecondsForTempo = 1.5f;  // less: noise can look periodic for a moment
constexpr float kPreferenceOctaves = 1.0f;  // width of the pull towards preferredBpm
constexpr float kPriorOctaves = 0.25f;      // ... and towards a prior
constexpr float kAcquireClarity = 0.15f;
constexpr float kSupport = 0.3f;        // own-lag correlation a candidate needs, of the strongest
constexpr int kStableToAcquire = 3;     // estimates within 2 % in a row
constexpr int kBetterToSwitch = 10;     // estimates (~1 s) clearly better than the tracked tempo
constexpr float kBetterRatio = 1.25f;

// Phase and PLL.
constexpr float kAcquireSeconds = 3.0f;
constexpr uint32_t kPhaseBins = 64;
constexpr double kWindow = 0.2;         // PLL window: +-0.2 beat, flat to +-0.1
constexpr double kFlat = 0.1;
constexpr double kPhaseGain = 0.35;
constexpr double kPeriodGain = 0.06;
constexpr double kPeriodClamp = 0.04;

// Confidence.
constexpr float kConfSeconds = 3.0f;
constexpr float kSalienceLow = 2.2f;    // onsets on the beat vs an average phase: noise ~1
constexpr float kSalienceHigh = 3.5f;
constexpr float kJitterLow = 0.03f;     // RMS PLL correction, in periods
constexpr float kJitterHigh = 0.08f;
constexpr float kClarityFull = 0.2f;
constexpr float kLockOn = 0.55f;
constexpr float kLockOff = 0.3f;
constexpr int kWeakBeatsToDrop = 12;
constexpr int kBeatsToLock = 2;         // PLL beats after acquiring before it may lock
constexpr float kMissJitter = 0.1f;     // a beat with no onset counts as this far off

float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// Leaky-average coefficient for a time constant, per step at `rate` steps/s.
float alphaFor(float seconds, double rate) { return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * rate))); }

void lowpass(float fs, float hz, float q, float* b0, float* b1, float* b2, float* a1, float* a2) {
  const double w0 = 2.0 * kPi * hz / fs;
  const double alpha = std::sin(w0) / (2.0 * q);
  const double c = std::cos(w0);
  const double a0 = 1.0 + alpha;
  *b0 = static_cast<float>((1.0 - c) / 2.0 / a0);
  *b1 = static_cast<float>((1.0 - c) / a0);
  *b2 = *b0;
  *a1 = static_cast<float>(-2.0 * c / a0);
  *a2 = static_cast<float>((1.0 - alpha) / a0);
}
}  // namespace

double BeatTracker::Grid::offsetFromNearestBeat(uint32_t frame) const {
  if (!valid || periodFrames <= 0.0f) return 0.0;
  const double beats = beatsAt(frame);
  return (beats - std::floor(beats + 0.5)) * periodFrames;
}

double BeatTracker::Grid::errorAgainst(double trueBeat, double truePeriod) const {
  if (!valid || truePeriod <= 0.0) return 0.0;
  const double whole = std::floor(trueBeat);
  double d = -(offsetFromNearestBeat(static_cast<uint32_t>(static_cast<int64_t>(whole))) + (trueBeat - whole));
  d -= truePeriod * std::floor(d / truePeriod + 0.5);
  return d;
}

BeatTracker::~BeatTracker() { freeBuffers(); }

void BeatTracker::freeBuffers() {
  if (!release_) return;
  release_(hist_);
  release_(acf_);
  release_(candLag_);
  release_(candWeight_);
  hist_ = acf_ = candLag_ = candWeight_ = nullptr;
}

bool BeatTracker::begin(const Config& config, void* (*alloc)(size_t), void (*release)(void*)) {
  if (hist_) return true;  // once
  cfg_ = config;
  if (!alloc) alloc = std::malloc;
  release_ = release ? release : std::free;
  hopRate_ = static_cast<double>(cfg_.sampleRate) / cfg_.hop;
  hopLen_ = cfg_.hop / cfg_.decimation;
  // kHarmonics times the longest period: each tempo is scored at its multiples.
  maxLag_ = static_cast<uint32_t>(std::ceil(kHarmonics * lagOf(cfg_.minBpm))) + 2;
  const auto acquireHops = static_cast<uint32_t>(kAcquireSeconds * hopRate_) + 1;
  uint32_t histLen = 1;
  while (histLen < maxLag_ + 1 || histLen < acquireHops + 1) histLen <<= 1;
  histMask_ = histLen - 1;

  candidates_ = static_cast<uint32_t>(std::log(cfg_.maxBpm / cfg_.minBpm) / std::log(kBpmStep)) + 1;
  hist_ = static_cast<float*>(alloc(histLen * sizeof(float)));
  acf_ = static_cast<float*>(alloc((maxLag_ + 1) * sizeof(float)));
  candLag_ = static_cast<float*>(alloc(candidates_ * sizeof(float)));
  candWeight_ = static_cast<float*>(alloc(candidates_ * sizeof(float)));
  if (!hist_ || !acf_ || !candLag_ || !candWeight_) {
    freeBuffers();
    return false;
  }
  fillCandidates();
  acfDecay_ = 1.0f - alphaFor(kAcfSeconds, hopRate_);
  // Per-hop smoothing, worked out once (exp() in double is slow on the ESP32).
  levelAlpha_ = alphaFor(kLevelSeconds, hopRate_);
  meanAlpha_ = alphaFor(kMeanSeconds, hopRate_);
  fadeDecay_ = 1.0f - alphaFor(1.0f, hopRate_);
  confDecay_ = 1.0f - alphaFor(kConfSeconds, hopRate_);

  const float fs = static_cast<float>(cfg_.sampleRate) / cfg_.decimation;
  // Fourth-order Butterworth as two biquads.
  lowpass(fs, cfg_.lowpassHz, 0.5412f, &lp1_.b0, &lp1_.b1, &lp1_.b2, &lp1_.a1, &lp1_.a2);
  lowpass(fs, cfg_.lowpassHz, 1.3066f, &lp2_.b0, &lp2_.b1, &lp2_.b2, &lp2_.a1, &lp2_.a2);
  const float floorAmp = std::pow(10.0f, kFloorDbfs / 20.0f);
  energyFloor_ = static_cast<float>(hopLen_) * floorAmp * floorAmp;
  reset(0);
  return true;
}

void BeatTracker::reset(uint32_t originFrame) {
  origin_ = originFrame;
  fed_ = 0;
  decimSum_ = 0;
  decimCount_ = 0;
  hopFill_ = 0;
  hopEnergy_ = midEnergy_ = 0.0f;
  dcX_ = dcY_ = 0.0f;
  lp1_.z1 = lp1_.z2 = lp2_.z1 = lp2_.z2 = 0.0f;
  hops_ = 0;
  prevEnergy_ = prevMid_ = 0.0f;
  level_ = midLevel_ = 0.0f;
  onsetMean_ = 0.0f;
  if (hist_) std::memset(hist_, 0, (histMask_ + 1) * sizeof(float));
  if (acf_) std::memset(acf_, 0, (maxLag_ + 1) * sizeof(float));
  estBpm_ = 0.0f;
  clarity_ = 0.0f;
  stableEstimates_ = 0;
  betterEstimates_ = 0;
  acquired_ = false;
  beat_ = period_ = acqPeriod_ = 0.0;
  beatIndex_ = 0;
  winSum_ = winMoment_ = 0.0f;
  beatEnergyAvg_ = 0.0f;
  for (float& b : phaseHist_) b = 0.0f;
  jitter_ = 0.0f;
  conf_ = 0.0f;
  locked_ = false;
  beatsSinceAcquire_ = 0;
  weakBeats_ = 0;
  lockAt_ = -1;
}

void BeatTracker::setPrior(float bpm) {
  prior_ = bpm > 0.0f ? bpm : 0.0f;
  fillCandidates();
}

float BeatTracker::bpm() const { return acquired_ && period_ > 0.0 ? static_cast<float>(lagOf(1.0f) / period_) : 0.0f; }

BeatTracker::Grid BeatTracker::grid() const {
  Grid g;
  if (!acquired_) return g;
  const double at = beat_ * cfg_.hop - kOnsetDelayFrames;
  const double whole = std::floor(at);
  g.valid = true;
  g.beatFrame = origin_ + static_cast<uint32_t>(static_cast<int64_t>(whole));
  g.beatFrac = static_cast<float>(at - whole);
  g.periodFrames = static_cast<float>(period_ * cfg_.hop);
  g.beatIndex = beatIndex_;
  return g;
}

void BeatTracker::process(const int16_t* mono, uint32_t frames) {
  if (!hist_) return;
  const float scale = 1.0f / (32768.0f * static_cast<float>(cfg_.decimation));
  const float dcPole = 1.0f - 2.0f * static_cast<float>(kPi) * kDcHz * cfg_.decimation / cfg_.sampleRate;
  for (uint32_t i = 0; i < frames; ++i) {
    decimSum_ += mono[i];
    if (++decimCount_ < cfg_.decimation) continue;
    const float x = static_cast<float>(decimSum_) * scale;
    decimSum_ = 0;
    decimCount_ = 0;
    const float dc = x - dcX_ + dcPole * dcY_;  // DC blocker
    dcX_ = x;
    dcY_ = dc;
    const float y = lp2_.run(lp1_.run(dc));
    const float mid = dc - y;  // above the low band, up to the decimated Nyquist
    hopEnergy_ += y * y;
    midEnergy_ += mid * mid;
    if (++hopFill_ == hopLen_) {
      onHop(hopEnergy_, midEnergy_);
      hopEnergy_ = midEnergy_ = 0.0f;
      hopFill_ = 0;
    }
  }
  fed_ += frames;
}

// The rise in log energy from one hop to the next. The first hop of a rise
// counts most, so a kick with a slow body still marks its attack (a linear
// energy flux put real kicks and basslines 20-40 ms late). A rise in a band
// far below its recent level (a quiet tail, near silence) counts less.
float BeatTracker::rise(float energy, float prev, float level) const {
  const float a = std::log(energy + energyFloor_), b = std::log(prev + energyFloor_);
  if (a <= b) return 0.0f;
  const float gate = energy / (level + energyFloor_);
  return (a - b) * (gate < 1.0f ? gate : 1.0f);
}

void BeatTracker::onHop(float energy, float midEnergy) {
  level_ += (energy - level_) * levelAlpha_;
  midLevel_ += (midEnergy - midLevel_) * levelAlpha_;
  // Mostly the low band (kick drums, bass); the mid band adds snares and
  // chords, which carry the beat in music with soft kicks. A log rise hardly
  // tells a kick from a quieter note, so a little of the linear rise (in
  // units of the level) keeps the loudest hit the beat: off-beat bass and
  // ghost kicks as loud together as the kick don't pull the grid to them.
  const float linear = energy > prevEnergy_ ? (energy - prevEnergy_) / (level_ + energyFloor_) : 0.0f;
  const float onset = rise(energy, prevEnergy_, level_) + kLinearWeight * linear +
                      kMidWeight * rise(midEnergy, prevMid_, midLevel_);
  prevEnergy_ = energy;
  prevMid_ = midEnergy;
  hist_[hops_ & histMask_] = onset;
  onsetMean_ += (onset - onsetMean_) * meanAlpha_;
  updateAcf(onset - onsetMean_);

  const double t = hops_;
  const float above = onset > onsetMean_ ? onset - onsetMean_ : 0.0f;
  if (acquired_) {
    updateConfidence(t, above);
    pll(t, above);
  } else {
    conf_ *= fadeDecay_;  // nothing tracked: fades out
  }
  ++hops_;
  if (hops_ % kEstimateEveryHops == 0) estimateTempo();
}

void BeatTracker::updateAcf(float centred) {
  // Lag 0 is the energy the others are compared with.
  const uint32_t k = hops_;
  const uint32_t lags = k < maxLag_ ? k : maxLag_;
  for (uint32_t lag = 0; lag <= lags; ++lag) {
    acf_[lag] = acf_[lag] * acfDecay_ + centred * (onsetAt(k - lag) - onsetMean_);
  }
}

float BeatTracker::acfAt(float lag) const {
  if (lag < 0.0f || lag >= static_cast<float>(maxLag_)) return 0.0f;
  const auto i = static_cast<uint32_t>(lag);
  const float f = lag - static_cast<float>(i);
  return acf_[i] * (1.0f - f) + acf_[i + 1] * f;
}

// A tempo scores with its own lag and its multiples (kHarmonicWeight): a
// steady beat correlates at all of them, so a tempo whose double is empty
// (the double of the real tempo) loses, and so does a 4:3 or 3:2 relative.
float BeatTracker::scoreAtLag(float lag) const {
  float s = 0.0f;
  for (int k = 1; k <= kHarmonics; ++k) s += kHarmonicWeight[k] * acfAt(k * lag);
  return s;
}

float BeatTracker::candBpm(uint32_t i) const { return cfg_.minBpm * std::pow(kBpmStep, static_cast<float>(i)); }

float BeatTracker::candValue(uint32_t i, float strongest) const {
  const float lag = candLag_[i];
  const float s = acfAt(lag) >= kSupport * strongest ? scoreAtLag(lag) : 0.0f;
  return (s > 0.0f ? s : 0.0f) * candWeight_[i];
}

void BeatTracker::fillCandidates() {
  if (!candLag_) return;
  for (uint32_t i = 0; i < candidates_; ++i) {
    const float bpm = candBpm(i);
    candLag_[i] = static_cast<float>(lagOf(bpm));
    candWeight_[i] = preference(bpm);
  }
}

float BeatTracker::preference(float bpm) const {
  const bool prior = prior_ > 0.0f;
  const float centre = prior ? prior_ : cfg_.preferredBpm;
  const float x = std::log2(bpm / centre) / (prior ? kPriorOctaves : kPreferenceOctaves);
  return std::exp(-0.5f * x * x);
}

void BeatTracker::estimateTempo() {
  if (hops_ < kMinSecondsForTempo * hopRate_ || acf_[0] <= 1e-9f) {
    clarity_ = 0.0f;
    return;
  }
  // Candidates spaced evenly in log tempo (tables from begin()/setPrior()).
  // Only a tempo the audio supports (its own lag correlates, not just its
  // double) may win: a prior can pick a slower octave of a steady beat, but
  // can't make up beats between them.
  float strongest = 0.0f;
  for (uint32_t i = 0; i < candidates_; ++i) {
    const float a = acfAt(candLag_[i]);
    if (a > strongest) strongest = a;
  }
  float best = -1.0f, before = 0.0f, after = 0.0f;
  uint32_t bestIdx = 0;
  for (uint32_t i = 0; i < candidates_; ++i) {
    const float lag = candLag_[i];
    const float s = acfAt(lag) >= kSupport * strongest ? scoreAtLag(lag) : 0.0f;
    const float v = (s > 0.0f ? s : 0.0f) * candWeight_[i];
    if (v > best) {
      best = v;
      bestIdx = i;
    }
  }
  if (bestIdx > 0) before = candValue(bestIdx - 1, strongest);
  if (bestIdx + 1 < candidates_) after = candValue(bestIdx + 1, strongest);
  const float bestBpm = candBpm(bestIdx);
  if (best <= 0.0f) {
    clarity_ = 0.0f;
    stableEstimates_ = 0;
    return;
  }
  // Parabolic refinement between the neighbouring candidates.
  const float denom = before - 2.0f * best + after;
  float shift = denom < 0.0f ? 0.5f * (before - after) / denom : 0.0f;
  if (shift > 0.5f) shift = 0.5f;
  if (shift < -0.5f) shift = -0.5f;
  const float est = bestBpm * std::pow(kBpmStep, shift);
  // A perfectly periodic onset signal scores the sum of the weights times its
  // energy. Only the multiples the history already reaches count, so a clear
  // beat can be acquired before 8 periods have been heard.
  const double estLag = lagOf(est);
  float weights = kHarmonicWeight[1];
  for (int k = 2; k <= kHarmonics && k * estLag <= 0.8 * hops_; ++k) weights += kHarmonicWeight[k];
  clarity_ = clamp01(scoreAtLag(static_cast<float>(estLag)) / (weights * acf_[0]));
  stableEstimates_ = estBpm_ > 0.0f && std::fabs(est / estBpm_ - 1.0f) < 0.02f ? stableEstimates_ + 1 : 0;
  estBpm_ = est;

  if (!acquired_) {
    if (stableEstimates_ >= kStableToAcquire && clarity_ >= kAcquireClarity) acquire(lagOf(est));
    return;
  }
  // Tracked tempo vs the pick: switch once the pick is clearly better for a while.
  const float tracked = bpm();
  const float trackedScore = scoreAtLag(static_cast<float>(period_)) * preference(tracked);  // rare: exp is fine
  const bool differs = std::fabs(est / tracked - 1.0f) > static_cast<float>(kPeriodClamp);
  betterEstimates_ = differs && best > kBetterRatio * trackedScore ? betterEstimates_ + 1 : 0;
  // Sooner once the tracked beat has stopped fitting (a tempo change).
  const int needed = locked_ ? kBetterToSwitch : kStableToAcquire;
  if (betterEstimates_ >= needed && clarity_ >= kAcquireClarity) acquire(lagOf(est));
}

// Phase from the recent onsets folded at the period: the strongest pulse,
// then the onset-weighted centroid around it. Also seeds the confidence
// from the same history, so a clear beat can lock at its first PLL beat.
bool BeatTracker::acquire(double periodHops) {
  const auto window = static_cast<uint32_t>(kAcquireSeconds * hopRate_);
  const uint32_t n = hops_ < window ? hops_ : window;
  if (n < 2 || periodHops <= 1.0) return false;
  const uint32_t first = hops_ - n;
  float bins[kPhaseBins] = {};
  for (uint32_t j = first; j < hops_; ++j) {
    const float o = onsetAt(j) - onsetMean_;
    if (o <= 0.0f) continue;
    const double pos = std::fmod(static_cast<double>(j), periodHops) / periodHops * kPhaseBins;
    const auto b = static_cast<uint32_t>(pos);
    const auto f = static_cast<float>(pos - b);
    bins[b % kPhaseBins] += o * (1.0f - f);
    bins[(b + 1) % kPhaseBins] += o * f;
  }
  uint32_t peak = 0;
  float peakValue = -1.0f;
  for (uint32_t b = 0; b < kPhaseBins; ++b) {
    const float v = bins[(b + kPhaseBins - 1) % kPhaseBins] + 2.0f * bins[b] + bins[(b + 1) % kPhaseBins];
    if (v > peakValue) {
      peakValue = v;
      peak = b;
    }
  }
  const double coarse = periodHops * peak / kPhaseBins;
  double sum = 0.0, moment = 0.0;
  for (uint32_t j = first; j < hops_; ++j) {
    const float o = onsetAt(j) - onsetMean_;
    if (o <= 0.0f) continue;
    double d = std::fmod(static_cast<double>(j) - coarse, periodHops);
    if (d < 0.0) d += periodHops;
    if (d >= periodHops / 2) d -= periodHops;
    if (std::fabs(d) <= periodHops / 8) {
      sum += o;
      moment += o * d;
    }
  }
  const double phase = coarse + (sum > 0.0 ? moment / sum : 0.0);

  // The first beat whose whole PLL window is still ahead.
  const double ahead = static_cast<double>(hops_) + kWindow * periodHops;
  beat_ = phase + std::ceil((ahead - phase) / periodHops) * periodHops;
  period_ = acqPeriod_ = periodHops;
  beatIndex_ = 0;
  winSum_ = winMoment_ = 0.0f;
  acquired_ = true;
  beatsSinceAcquire_ = 0;
  betterEstimates_ = 0;
  weakBeats_ = 0;

  // Confidence seeded from the last 2 s against this grid.
  for (float& b : phaseHist_) b = 0.0f;
  float windowEnergy = 0.0f;
  const auto seedHops = static_cast<uint32_t>(2.0 * hopRate_);
  for (uint32_t j = hops_ - (n < seedHops ? n : seedHops); j < hops_; ++j) {
    const float o = onsetAt(j) - onsetMean_;
    updateConfidence(j, o > 0.0f ? o : 0.0f);
  }
  // The typical onset energy of a beat window, for telling hits from misses.
  const double beatsInWindow = n / periodHops;
  for (uint32_t j = first; j < hops_; ++j) {
    const float o = onsetAt(j) - onsetMean_;
    if (o <= 0.0f) continue;
    double d = std::fmod(static_cast<double>(j) - phase, periodHops);
    if (d < 0.0) d += periodHops;
    if (d >= periodHops / 2) d -= periodHops;
    if (std::fabs(d) <= kWindow * periodHops) windowEnergy += o;
  }
  beatEnergyAvg_ = beatsInWindow > 0.0 ? static_cast<float>(windowEnergy / beatsInWindow) : 0.0f;
  jitter_ = 0.02f * 0.02f;
  conf_ = rawConfidence();
  return true;
}

// The three signs of a real beat, each 0..1, multiplied: onsets gathered on
// the beat (the grid's phase holds several times the onset energy of an
// average phase; noise gives about 1, and eighths or sixteenths between the
// beats still leave the beat standing out), small PLL corrections, and a
// clear tempo peak.
float BeatTracker::rawConfidence() const {
  float sum = 0.0f;
  for (float b : phaseHist_) sum += b;
  const float salience = sum > 1e-9f ? phaseHist_[0] * kSalienceBins / sum : 0.0f;
  const float grid = clamp01((salience - kSalienceLow) / (kSalienceHigh - kSalienceLow));
  const float steady = clamp01((kJitterHigh - std::sqrt(jitter_)) / (kJitterHigh - kJitterLow));
  const float clear = clamp01(clarity_ / kClarityFull);
  return grid * steady * clear;
}

// Leaky onset energy per phase of the beat, in kSalienceBins bins (the first
// centred on the beat).
void BeatTracker::updateConfidence(double t, float onset) {
  const float decay = confDecay_;
  double ph = (t - beat_) / period_;
  ph -= std::floor(ph);
  const int bin = static_cast<int>((ph + 0.5 / kSalienceBins) * kSalienceBins) % kSalienceBins;
  for (float& b : phaseHist_) b *= decay;
  phaseHist_[bin] += onset;
}

void BeatTracker::pll(double t, float onset) {
  for (int pass = 0; pass < 2; ++pass) {
    const double d = t - beat_;
    const double edge = kWindow * period_;
    if (d < -edge) return;
    if (d <= edge) {
      const double flat = kFlat * period_;
      const double a = std::fabs(d);
      const float w =
          a <= flat ? 1.0f : 0.5f + 0.5f * std::cos(static_cast<float>(kPi * (a - flat) / (edge - flat)));
      winSum_ += static_cast<float>(onset * w);
      winMoment_ += static_cast<float>(onset * w * d);
      return;
    }
    closeBeat();  // this hop is past the window: it may belong to the next one
  }
}

void BeatTracker::closeBeat() {
  const bool hit = winSum_ > 1e-4f && winSum_ > 0.25f * beatEnergyAvg_;
  const double e = hit ? winMoment_ / winSum_ : 0.0;
  beatEnergyAvg_ += (winSum_ - beatEnergyAvg_) * 0.2f;
  const float rel = hit ? static_cast<float>(e / period_) : kMissJitter;
  jitter_ += (rel * rel - jitter_) * 0.3f;

  period_ += kPeriodGain * e;
  const double lo = acqPeriod_ * (1.0 - kPeriodClamp), hi = acqPeriod_ * (1.0 + kPeriodClamp);
  if (period_ < lo) period_ = lo;
  if (period_ > hi) period_ = hi;
  beat_ += kPhaseGain * e + period_;
  ++beatIndex_;
  winSum_ = winMoment_ = 0.0f;
  ++beatsSinceAcquire_;

 const float raw = rawConfidence();
  conf_ += (raw - conf_) * (raw < conf_ ? 0.6f : 0.4f);  // falls faster than it rises
  if (!locked_ && conf_ >= kLockOn && beatsSinceAcquire_ >= kBeatsToLock) {
    locked_ = true;
    if (lockAt_ < 0) lockAt_ = static_cast<int32_t>((hops_ + 1) * cfg_.hop);
  } else if (locked_ && conf_ < kLockOff) {
    locked_ = false;
  }
  weakBeats_ = conf_ < 0.2f ? weakBeats_ + 1 : 0;
  if (weakBeats_ >= kWeakBeatsToDrop) {  // lost it: look for the tempo again
    acquired_ = false;
    locked_ = false;
    stableEstimates_ = 0;
  }
}
