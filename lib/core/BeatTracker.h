#pragma once
#include <cstddef>
#include <cstdint>

// Finds the beat in mono audio as it plays, for the dancing figure: tempo,
// where the beats fall, and how sure it is. Portable, allocation only in
// begin() (through a hook, so the firmware can use PSRAM), no locks; about
// 13 ms per second of audio on the Core2, 0.2 ms on a laptop.
//
//   1. Onset signal: the audio is box-averaged down 8x and split at 150 Hz
//      (two biquads) into a low band (kick drums, bass) and the rest; each
//      band's energy is summed per hop of 512 frames (86 Hz at 44.1 kHz).
//      The onset strength is the rise in log energy from one hop to the next
//      (low band, plus half the mid band's, plus a little of the low band's
//      linear rise so the loudest hit stays the beat).
//   2. Tempo: a running autocorrelation of the onset signal (~3 s memory),
//      scored at each candidate tempo (60-200 BPM) at its lag and the next 7
//      multiples (so 4:3 and 3:2 relatives lose), weighted towards 120 BPM,
//      or strongly towards the prior when one is set (a BPM from metadata:
//      it picks the octave).
//   3. Phase: the last ~3 s of onsets folded at that period; the strongest
//      pulse, refined to its onset-weighted centroid (sub-hop).
//   4. A PLL moves each predicted beat by the centroid of the onsets in a
//      window around it (phase and period gains), the period clamped to
//      +-4 % of the tempo it was acquired at. A tempo that keeps scoring
//      clearly better than the one tracked re-acquires.
//   5. Confidence: how far the onsets on the beat stand out from those at an
//      average phase (noise: about 1), how steady the PLL's corrections
//      are, and how clear the tempo peak is. Locked uses hysteresis (on
//      above 0.55 from the second beat, off below 0.3).
//
// Time is counted in the caller's frames: reset() gives the frame index of
// the next sample, and the grid comes back in those units (the firmware uses
// the track frames of the tapped audio). Nothing here reads a clock.
class BeatTracker {
public:
  struct Config {
    uint32_t sampleRate = 44100;
    uint32_t hop = 512;       // frames per onset value; a multiple of decimation
    uint32_t decimation = 8;  // box average before the low-pass
    float lowpassHz = 150.0f;
    float minBpm = 60.0f;     // tempo range searched
    float maxBpm = 200.0f;
    float preferredBpm = 120.0f;
  };

  // The beat grid: beat number `beatIndex` falls at frame beatFrame +
  // beatFrac, and beats follow every periodFrames.
  struct Grid {
    bool valid = false;
    uint32_t beatFrame = 0;
    float beatFrac = 0.0f;
    float periodFrames = 0.0f;
    int32_t beatIndex = 0;
    // Beats from that beat to `frame` + frac (negative before it).
    double beatsAt(uint32_t frame, float frac = 0.0f) const {
      if (!valid || periodFrames <= 0.0f) return 0.0;
      const double d = static_cast<double>(static_cast<int32_t>(frame - beatFrame)) + frac - beatFrac;
      return d / periodFrames;
    }
    // Signed frames from `frame` to the nearest grid beat (frame - beat).
    double offsetFromNearestBeat(uint32_t frame) const;
    // Phase error against a known beat: the nearest grid beat minus
    // `trueBeat` (frames), taken modulo `truePeriod`, so a grid at half or
    // double the true tempo is on the beat too.
    double errorAgainst(double trueBeat, double truePeriod) const;
  };

  BeatTracker() = default;
  ~BeatTracker();
  BeatTracker(const BeatTracker&) = delete;
  BeatTracker& operator=(const BeatTracker&) = delete;

  // Allocates ~9 KB of scratch (onset history, autocorrelation, tempo tables) with `alloc`
  // (freed with `release`; nullptr: malloc/free), so the firmware can put it
  // in PSRAM. False: out of memory.
  bool begin(const Config& config, void* (*alloc)(size_t bytes) = nullptr, void (*release)(void*) = nullptr);
  // Forgets everything: the next sample is frame `originFrame`.
  void reset(uint32_t originFrame);
  // A tempo from metadata, or 0 for none. Weights the tempo search strongly
  // towards it (and its octave); kept across reset().
  void setPrior(float bpm);
  float prior() const { return prior_; }
  // Mono samples, contiguous with what came before (since reset()).
  void process(const int16_t* mono, uint32_t frames);

  float bpm() const;                    // tempo tracked, 0 when none
  float confidence() const { return conf_; }  // 0..1
  bool locked() const { return locked_; }
  Grid grid() const;
  // Frames fed since reset().
  uint32_t framesSinceReset() const { return fed_; }
  // Frames fed from reset() until it first locked, or -1.
  int32_t framesToLock() const { return lockAt_; }
  // Last tempo estimate (the autocorrelation's pick) and its clarity, for logs.
  float estimatedBpm() const { return estBpm_; }
  float tempoClarity() const { return clarity_; }
  const Config& config() const { return cfg_; }

  // The onset signal's delay: an onset in the audio shows up this many frames
  // later in the energy rise (filters, hop quantisation). On the kick-like
  // click tracks the mean error stays within about +-2 ms, so 0.
  static constexpr float kOnsetDelayFrames = 0.0f;

private:
  struct Biquad {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1 = 0, z2 = 0;
    float run(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };

  void onHop(float energy, float midEnergy);
  float rise(float energy, float prev, float level) const;
  void updateAcf(float centred);
  void estimateTempo();
  float scoreAtLag(float lag) const;
  float acfAt(float lag) const;
  float preference(float bpm) const;
  float candBpm(uint32_t i) const;
  float candValue(uint32_t i, float strongest) const;
  void fillCandidates();
  void freeBuffers();
  bool acquire(double periodHops);
  void pll(double t, float onset);
  void closeBeat();
  void updateConfidence(double t, float onset);
  float rawConfidence() const;
  float onsetAt(uint32_t hop) const { return hist_[hop & histMask_]; }
  double lagOf(float bpm) const { return 60.0 * hopRate_ / bpm; }

  Config cfg_;
  void (*release_)(void*) = nullptr;
  double hopRate_ = 86.13;
  uint32_t maxLag_ = 0;
  uint32_t histMask_ = 0;
  float* hist_ = nullptr;  // onset strength per hop, ring
  float* acf_ = nullptr;   // [0..maxLag_]
  uint32_t candidates_ = 0;         // tempo candidates, minBpm..maxBpm in 0.5 % steps
  float* candLag_ = nullptr;        // their lags in hops
  float* candWeight_ = nullptr;     // their preference weights (prior or preferredBpm)
  float acfDecay_ = 0.0f;
  float levelAlpha_ = 0.0f, meanAlpha_ = 0.0f;  // per-hop smoothing (begin())
  float fadeDecay_ = 1.0f, confDecay_ = 1.0f;
  float prior_ = 0.0f;

  // Per sample.
  uint32_t origin_ = 0;
  uint32_t fed_ = 0;
  int32_t decimSum_ = 0;
  uint32_t decimCount_ = 0;
  uint32_t hopFill_ = 0;       // decimated samples in the hop so far
  uint32_t hopLen_ = 64;       // decimated samples per hop
  float hopEnergy_ = 0.0f;
  float midEnergy_ = 0.0f;     // the rest of the decimated band, above the low-pass
  float dcX_ = 0.0f, dcY_ = 0.0f;
  Biquad lp1_, lp2_;

  // Per hop.
  uint32_t hops_ = 0;          // onset values so far
  float prevEnergy_ = 0.0f, prevMid_ = 0.0f;
  float level_ = 0.0f, midLevel_ = 0.0f;  // running mean energy per hop, per band
  float onsetMean_ = 0.0f;     // running mean onset strength
  float energyFloor_ = 0.0f;

  // Tempo.
  float estBpm_ = 0.0f;
  float clarity_ = 0.0f;       // score of the pick / zero-lag energy
  int stableEstimates_ = 0;    // consecutive estimates close to each other
  int betterEstimates_ = 0;    // consecutive estimates clearly better than the tracked tempo

  // Grid, in hops since reset (the onset at hop j stands for time j).
  bool acquired_ = false;
  double beat_ = 0.0;          // the pending (next) beat
  double period_ = 0.0;
  double acqPeriod_ = 0.0;     // what the PLL is clamped around
  int32_t beatIndex_ = 0;
  float winSum_ = 0.0f, winMoment_ = 0.0f;
  float beatEnergyAvg_ = 0.0f; // typical onset energy per beat window

  // Confidence.
  static constexpr int kSalienceBins = 16;
  float phaseHist_[kSalienceBins] = {};  // leaky onset energy per phase of the beat
  float jitter_ = 0.0f;        // leaky mean of (correction / period)^2
  float conf_ = 0.0f;
  bool locked_ = false;
  int beatsSinceAcquire_ = 0;
  int weakBeats_ = 0;          // consecutive beats at low confidence
  int32_t lockAt_ = -1;
};
