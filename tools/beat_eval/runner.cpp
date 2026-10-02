// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// The beat tracker's evaluation runner (docs/BEAT-TRACKER-EVAL.md): the
// firmware's own BeatTracker (lib/core, compiled unchanged) fed the way the
// Core2 feeds it, and everything it reports written out for the scorer
// (tools/beat_eval/score.py). Built with the host compiler by
// `tools/beat_eval/beat_eval.py build`; not part of the firmware or the
// native tests.
//
//   runner --pcm track.s16 [--start S] [--seconds S] [--prior BPM] [--via-hops]
//   runner --synth "click:120:60,silence:5,click:96:30" [--noise DBFS] [--seed N]
//   runner --synth "drums:124:20:0.75:-6:-4:-4:0.2"   (the host tests' drum pattern)
//   runner --bench [--seconds S] [--repeat N]
//   runner --pcm track.s16 --time-only [--start S] [--seconds S] [--repeat N]
//
// --pcm is mono int16 at 44.1 kHz on the device's timeline ((L + R) >> 1,
// as AudioTap mixes it). The tracker is reset to the start frame (the track
// frame, as DanceMode::restart() does) and fed one 512-frame hop at a time:
// the result doesn't depend on the chunking (test_beat_tracker), and a hop
// at a time lets every state the grid passes through be seen.
//
// Output, one record per line (times in seconds of the input's timeline):
//   meta key=value ...
//   T t bpm est clarity conf locked valid dominance hits pulse steady weight
//                                               every --every hops (default 8);
//                                               weight: the dance weight shown
//                                               (dance::danceWeight while locked)
//   B t period bpm conf locked index            each grid beat, as it is reached
//   R t                                         truth (synthetic input only)
//   end key=value ...
// A grid beat is written when the audio fed reaches it: its time is the
// prediction the grid held then (set at the previous beat's close, ~0.8
// beat ahead), the state is the tracker's at that hop. A pending beat that a
// re-acquire replaces before its time is never written: it was never shown.
// CPU: a second pass over the same audio in 2048-frame chunks (the
// firmware's scratch size), nothing polled, timed with the steady clock;
// --time-only does only that, best of --repeat, and --bench the same on the
// boot bench's click120. --hops-out also writes the front end's hop
// energies (float32 low, mid) for tools/beat_eval/diagnose.py.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "BeatTracker.h"
#include "ClickGen.h"
#include "DancePose.h"
#include "HopFrontEnd.h"
#include "Signals.h"  // test/test_beat_tracker: the host tests' drum pattern, exactly

namespace {

constexpr uint32_t kRate = 44100;
constexpr uint32_t kChunk = 2048;  // DanceMode's kScratchFrames

struct Options {
  std::string pcm;
  std::string synth;
  std::string hopsOut;  // the front end's hop energies, float32 (low, mid) pairs
  bool bench = false;
  bool timeOnly = false;
  double start = 0.0;
  double seconds = 0.0;  // 0: to the end
  float prior = 0.0f;
  bool viaHops = false;
  double noiseDbfs = -999.0;
  uint32_t seed = 1;
  int every = 8;
  int repeat = 7;
};

[[noreturn]] void usage(const char* why) {
  std::fprintf(stderr, "runner: %s\n", why);
  std::fprintf(stderr,
               "usage: runner --pcm FILE [--start S] [--seconds S] [--prior BPM] [--via-hops] [--every N]\n"
               "              [--hops-out FILE]\n"
               "       runner --synth SPEC [--noise DBFS] [--seed N] [--prior BPM] [--via-hops]\n"
               "       runner --bench [--seconds S] [--repeat N]\n"
               "       runner --pcm FILE --time-only [--start S] [--seconds S] [--repeat N]\n"
               "SPEC: comma-separated click:BPM[off][:SECONDS[:OFFSET_BEATS]] | silence:SECONDS | noise:DBFS:SECONDS\n"
               "      | ambient:DBFS:SECONDS[:SEED] | drums:BPM:SECONDS:PHASE:HAT_DB[:BASS_DB[:GHOST_DB[:FIRST_BEAT]]]\n");
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const auto next = [&]() -> const char* {
      if (i + 1 >= argc) usage(("missing value for " + a).c_str());
      return argv[++i];
    };
    if (a == "--pcm") o.pcm = next();
    else if (a == "--synth") o.synth = next();
    else if (a == "--bench") o.bench = true;
    else if (a == "--time-only") o.timeOnly = true;
    else if (a == "--start") o.start = std::atof(next());
    else if (a == "--seconds") o.seconds = std::atof(next());
    else if (a == "--prior") o.prior = static_cast<float>(std::atof(next()));
    else if (a == "--via-hops") o.viaHops = true;
    else if (a == "--hops-out") o.hopsOut = next();
    else if (a == "--noise") o.noiseDbfs = std::atof(next());
    else if (a == "--seed") o.seed = static_cast<uint32_t>(std::atoi(next()));
    else if (a == "--every") o.every = std::max(1, std::atoi(next()));
    else if (a == "--repeat") o.repeat = std::max(1, std::atoi(next()));
    else usage(("unknown option " + a).c_str());
  }
  if (static_cast<int>(!o.pcm.empty()) + static_cast<int>(!o.synth.empty()) + static_cast<int>(o.bench) != 1) {
    usage("give exactly one of --pcm, --synth, --bench");
  }
  return o;
}

int16_t clip(double v) {
  const double r = std::nearbyint(v);
  return static_cast<int16_t>(r < -32768.0 ? -32768.0 : (r > 32767.0 ? 32767.0 : r));
}

// A ClickGen track, mixed to mono as AudioTap does ((L + R) >> 1).
void appendClicks(std::vector<int16_t>* mono, std::vector<double>* truth, float bpm, double seconds,
                  float offsetBeats) {
  ClickGen g;
  ClickGen::Spec spec;
  spec.bpm = bpm;
  spec.offsetBeats = offsetBeats;
  const auto n = static_cast<uint32_t>(seconds * kRate);
  g.start(kRate, spec, n);
  std::vector<int16_t> stereo(2 * static_cast<size_t>(n));
  g.generate(stereo.data(), n);
  const size_t base = mono->size();
  mono->resize(base + n);
  for (uint32_t i = 0; i < n; ++i) {
    (*mono)[base + i] = static_cast<int16_t>((static_cast<int32_t>(stereo[2 * i]) + stereo[2 * i + 1]) >> 1);
  }
  for (uint32_t k = 0; g.beatFrame(k) < n; ++k) truth->push_back(static_cast<double>(base + g.beatFrame(k)));
}

void addNoise(int16_t* s, size_t n, double dbfs, std::mt19937* rng) {
  std::normal_distribution<double> gauss(0.0, 32768.0 * std::pow(10.0, dbfs / 20.0));
  for (size_t i = 0; i < n; ++i) s[i] = clip(s[i] + gauss(*rng));
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t from = 0;
  for (;;) {
    const size_t at = s.find(sep, from);
    out.push_back(s.substr(from, at == std::string::npos ? std::string::npos : at - from));
    if (at == std::string::npos) return out;
    from = at + 1;
  }
}

void buildSynth(const Options& o, std::vector<int16_t>* mono, std::vector<double>* truth) {
  std::mt19937 rng(o.seed);
  for (const std::string& seg : split(o.synth, ',')) {
    const std::vector<std::string> f = split(seg, ':');
    if (f[0] == "click" && f.size() >= 2) {
      std::string bpmText = f[1];
      float offset = 0.0f;
      if (bpmText.size() > 3 && bpmText.compare(bpmText.size() - 3, 3, "off") == 0) {
        ClickGen::Spec spec;  // "120off": ClickGen's own offset, as the built-in track
        ClickGen::parse("click" + bpmText, &spec);
        offset = spec.offsetBeats;
        bpmText.resize(bpmText.size() - 3);
      }
      const double secs = f.size() >= 3 ? std::atof(f[2].c_str()) : 60.0;
      if (f.size() >= 4) offset = static_cast<float>(std::atof(f[3].c_str()));
      appendClicks(mono, truth, static_cast<float>(std::atof(bpmText.c_str())), secs, offset);
    } else if (f[0] == "drums" && f.size() >= 5) {  // sig::drums(): kick on the beat, hat/bass/ghost at PHASE
      const auto num = [&](size_t i, double dflt) { return f.size() > i ? std::atof(f[i].c_str()) : dflt; };
      const sig::Track d = sig::drums(static_cast<float>(num(1, 120)), num(2, 20), num(3, 0.5), num(4, -6), num(5, -200),
                                      num(6, -200), num(7, 0.0));
      const size_t base = mono->size();
      mono->insert(mono->end(), d.mono.begin(), d.mono.end());
      for (double b : d.beats) truth->push_back(static_cast<double>(base) + b);
    } else if (f[0] == "silence" && f.size() == 2) {
      mono->resize(mono->size() + static_cast<size_t>(std::atof(f[1].c_str()) * kRate), 0);
    } else if (f[0] == "ambient" && f.size() >= 3) {  // low-passed noise with a slow swell (test_beat_tracker's)
      const size_t base = mono->size();
      const auto n = static_cast<size_t>(std::atof(f[2].c_str()) * kRate);
      mono->resize(base + n, 0);
      std::mt19937 arng(f.size() >= 4 ? static_cast<uint32_t>(std::atoi(f[3].c_str())) : 5u);
      std::normal_distribution<double> gauss(0.0, 1.0);
      double y1 = 0, y2 = 0;
      const double a = std::exp(-2 * 3.14159265358979 * 300.0 / kRate);
      const double amp = 32768 * std::pow(10.0, std::atof(f[1].c_str()) / 20.0) * 12.0;
      for (size_t i = 0; i < n; ++i) {
        y1 = a * y1 + (1 - a) * gauss(arng);
        y2 = a * y2 + (1 - a) * y1;
        const double swell = 0.6 + 0.4 * std::sin(2 * 3.14159265358979 * 0.13 * static_cast<double>(i) / kRate);
        (*mono)[base + i] = clip(amp * swell * y2);
      }
    } else if (f[0] == "noise" && f.size() == 3) {
      const size_t base = mono->size();
      const auto n = static_cast<size_t>(std::atof(f[2].c_str()) * kRate);
      mono->resize(base + n, 0);
      addNoise(mono->data() + base, n, std::atof(f[1].c_str()), &rng);
    } else {
      usage(("bad synth segment: " + seg).c_str());
    }
  }
  if (o.noiseDbfs > -200.0) addNoise(mono->data(), mono->size(), o.noiseDbfs, &rng);
}

bool readPcm(const std::string& path, std::vector<int16_t>* mono) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  mono->resize(static_cast<size_t>(bytes) / 2);
  const size_t got = std::fread(mono->data(), 2, mono->size(), f);
  std::fclose(f);
  mono->resize(got);
  return true;
}

// Feeds `n` frames the way the firmware does: process(), or (--via-hops)
// a HopFrontEnd of its own whose hops go through feedHop(), as the USB
// visualizer's computer sends them.
struct Feeder {
  BeatTracker* tracker = nullptr;
  HopFrontEnd fe;
  bool viaHops = false;
  void begin(BeatTracker* t, bool hops) {
    tracker = t;
    viaHops = hops;
    const BeatTracker::Config& c = t->config();
    fe.begin(c.sampleRate, c.hop, c.decimation, c.lowpassHz);
  }
  void reset() { fe.reset(); }
  void feed(const int16_t* mono, uint32_t n) {
    if (viaHops) {
      fe.process(mono, n, [this](float low, float mid) { tracker->feedHop(low, mid); });
    } else {
      tracker->process(mono, n);
    }
  }
};

double timedPass(BeatTracker* tracker, Feeder* feeder, const int16_t* mono, uint32_t n, uint32_t origin) {
  tracker->reset(origin);
  feeder->reset();
  const auto t0 = std::chrono::steady_clock::now();
  for (uint32_t at = 0; at < n; at += kChunk) feeder->feed(mono + at, std::min(kChunk, n - at));
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

int bench(const Options& o) {
  std::vector<int16_t> mono;
  std::vector<double> truth;
  appendClicks(&mono, &truth, 120.0f, o.seconds > 0.0 ? o.seconds : 60.0, 0.0f);
  BeatTracker tracker;
  if (!tracker.begin(BeatTracker::Config{})) return 1;
  Feeder feeder;
  feeder.begin(&tracker, false);
  const auto n = static_cast<uint32_t>(mono.size());
  double best = 1e300;
  for (int r = 0; r < o.repeat; ++r) best = std::min(best, timedPass(&tracker, &feeder, mono.data(), n, 0));
  const double audioSeconds = static_cast<double>(n) / kRate;
  std::printf("bench click120 seconds=%.1f repeat=%d best_ns_per_audio_s=%.0f locked=%d bpm=%.3f\n", audioSeconds,
              o.repeat, best / audioSeconds, tracker.locked() ? 1 : 0, tracker.bpm());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options o = parse(argc, argv);
  if (o.bench) return bench(o);

  std::vector<int16_t> mono;
  std::vector<double> truth;
  if (!o.pcm.empty()) {
    if (!readPcm(o.pcm, &mono)) usage(("can't read " + o.pcm).c_str());
  } else {
    buildSynth(o, &mono, &truth);
  }
  const auto total = static_cast<uint32_t>(mono.size());
  const auto startFrame = std::min(total, static_cast<uint32_t>(std::llround(o.start * kRate)));
  uint32_t endFrame = total;
  if (o.seconds > 0.0) endFrame = std::min(total, startFrame + static_cast<uint32_t>(std::llround(o.seconds * kRate)));
  const uint32_t n = endFrame - startFrame;
  const int16_t* audio = mono.data() + startFrame;

  BeatTracker tracker;
  if (!tracker.begin(BeatTracker::Config{})) {
    std::fprintf(stderr, "runner: begin() failed\n");
    return 1;
  }
  tracker.setPrior(o.prior);
  Feeder feeder;
  feeder.begin(&tracker, o.viaHops);
  if (o.timeOnly) {  // CPU only: the best of --repeat timed passes, nothing written but that
    double best = 1e300;
    for (int r = 0; r < o.repeat; ++r) best = std::min(best, timedPass(&tracker, &feeder, audio, n, startFrame));
    std::printf("time frames=%u repeat=%d best_ns_per_audio_s=%.0f\n", n, o.repeat,
                n ? best / (static_cast<double>(n) / kRate) : 0.0);
    return 0;
  }
  const uint32_t hop = tracker.config().hop;

  std::printf("meta rate=%u hop=%u frames=%u start_frame=%u prior=%.3f via_hops=%d every=%d\n", kRate, hop, n,
              startFrame, o.prior, o.viaHops ? 1 : 0, o.every);
  for (double t : truth) {
    if (t >= startFrame && t < endFrame) std::printf("R %.6f\n", t / kRate);
  }

  tracker.reset(startFrame);
  feeder.reset();
  // --hops-out: the same HopFrontEnd the tracker runs, its energies written out
  // (for tools/beat_eval/diagnose.py; the tracker's own onsets are private).
  FILE* hopsFile = o.hopsOut.empty() ? nullptr : std::fopen(o.hopsOut.c_str(), "wb");
  HopFrontEnd dumpFe;
  if (hopsFile) {
    const BeatTracker::Config& c = tracker.config();
    dumpFe.begin(c.sampleRate, c.hop, c.decimation, c.lowpassHz);
  }
  bool pendingKnown = false, pendingShown = false;
  double pending = 0.0;  // the grid's pending beat, in frames of the input
  uint32_t hops = 0;
  int64_t lockedHops = 0;
  for (uint32_t at = 0; at + hop <= n; at += hop) {
    feeder.feed(audio + at, hop);
    if (hopsFile) {
      dumpFe.process(audio + at, hop, [hopsFile](float low, float mid) {
        const float e[2] = {low, mid};
        std::fwrite(e, sizeof(float), 2, hopsFile);
      });
    }
    ++hops;
    const double fedTo = static_cast<double>(startFrame) + at + hop;  // frames of the input fed so far
    const BeatTracker::Grid g = tracker.grid();
    if (g.valid) {
      const double beat =
          static_cast<double>(static_cast<int32_t>(g.beatFrame - startFrame)) + startFrame + g.beatFrac;
      if (!pendingKnown || beat != pending) {
        pending = beat;
        pendingKnown = true;
        pendingShown = false;
      }
      if (!pendingShown && pending <= fedTo) {
        std::printf("B %.6f %.6f %.4f %.4f %d %ld\n", pending / kRate, g.periodFrames / kRate, tracker.bpm(),
                    tracker.confidence(), tracker.locked() ? 1 : 0, static_cast<long>(g.beatIndex));
        pendingShown = true;
      }
    } else {
      pendingKnown = false;
    }
    if (tracker.locked()) ++lockedHops;
    if (hops % static_cast<uint32_t>(o.every) == 0) {
      const BeatTracker::Factors f = tracker.factors();
      // What the dancer is shown (DanceMode: the weight's target, danced only while locked).
      const float weight = tracker.locked() ? dance::danceWeight(tracker.confidence()) : 0.0f;
      std::printf("T %.4f %.4f %.4f %.4f %.4f %d %d %.3f %.3f %.3f %.3f %.3f\n", fedTo / kRate, tracker.bpm(),
                  tracker.estimatedBpm(), tracker.tempoClarity(), tracker.confidence(), tracker.locked() ? 1 : 0,
                  g.valid ? 1 : 0, f.dominance, f.hitRate, f.pulse, f.steady, weight);
    }
  }
  if (hopsFile) std::fclose(hopsFile);
  const int32_t lockFrames = tracker.framesToLock();

  // CPU: the same audio again, timed, in the firmware's chunk size.
  const double ns = timedPass(&tracker, &feeder, audio, n, startFrame);
  const double audioSeconds = static_cast<double>(n) / kRate;
  std::printf("end hops=%u lock_s=%.4f locked_share=%.4f ns_per_audio_s=%.0f\n", hops,
              lockFrames < 0 ? -1.0 : static_cast<double>(lockFrames) / kRate,
              hops ? static_cast<double>(lockedHops) / hops : 0.0, audioSeconds > 0 ? ns / audioSeconds : 0.0);
  return 0;
}
