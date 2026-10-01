// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <string>

// Click tracks with a known beat grid, for testing the beat tracker on the
// device: the built-in tracks "tone:click90", "tone:click120", ...,
// "tone:click120off" (see parse()). Each click is ~15 ms: a thump gliding from
// 100 to 60 Hz plus a short 2 kHz tick, the downbeat (every 4th beat)
// accented at about -12 dBFS, the others ~4 dB softer. Stereo, both channels
// the same.
//
// Beat k starts at exactly offset + round(k * 60 / bpm * rate) frames, so the
// firmware can compare what the tracker finds with the truth, in track frames.
// Output doesn't depend on how the caller chunks it.
class ClickGen {
public:
  struct Spec {
    float bpm = 120.0f;
    float offsetBeats = 0.0f;  // the first beat, as a fraction of a period
  };

  // "click120" -> 120 BPM from frame 0; "click120off" -> first beat at 0.37 of
  // a period. The name without "tone:". False: not a click track.
  static bool parse(const std::string& name, Spec* spec);

  void start(uint32_t sampleRate, const Spec& spec, uint32_t durationFrames, float levelDbfs = -12.0f);
  // Up to `frames` interleaved stereo frames; 0 once the duration is produced.
  uint32_t generate(int16_t* out, uint32_t frames);
  bool done() const { return pos_ >= total_; }

  // ---- ground truth ----
  const Spec& spec() const { return spec_; }
  uint32_t sampleRate() const { return rate_; }
  uint32_t beatFrame(uint32_t k) const;
  // Signed distance in frames from `frame` to the nearest beat (frame - beat).
  int32_t offsetFromNearestBeat(uint32_t frame) const;

private:
  float shape(uint32_t i, float* phase) const;

  Spec spec_;
  uint32_t rate_ = 44100;
  uint32_t total_ = 0;
  uint32_t pos_ = 0;
  uint32_t offset_ = 0;     // frames before beat 0
  double periodFrames_ = 0;
  float level_ = 0.25f;     // gain of an accented click's shape
  float softLevel_ = 0.16f; // the others'
  // The click being played.
  uint32_t nextBeat_ = 0;   // index of the next beat to start
  uint32_t nextFrame_ = 0;  // its frame
  uint32_t clickPos_ = 0;   // frames into the current click, or clickLen_ when none
  uint32_t clickLen_ = 0;
  bool accent_ = false;
  float thumpPhase_ = 0.0f;
};
