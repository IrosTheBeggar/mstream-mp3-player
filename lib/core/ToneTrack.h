// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>
#include <string>

#include "ClickGen.h"

// What a built-in track's path asks the backend to make (ToneGen, ClickGen):
//   "tone:440", "tone:1000"   a sine at that frequency, -18 dBFS, 30 s
//   "tone:left"               440 Hz in the left channel only
//   "tone:silence"            an hour of zeros (power measurements)
//   "tone:click120[off]"      60 s of clicks with a known beat (ClickGen)
// A sine or the silence may end in "@<rate>" ("tone:1000@48000",
// "tone:silence@96000"): made at that rate and converted to 44.1 kHz like
// a file (the converter's test tracks, docs/RESAMPLER.md section 6). The
// parser takes any rate; whether it plays is the converter's to say. The
// click tracks stay at 44.1 kHz: the dancer's truth is in 44.1 kHz frames.
// Portable, host-tested.
struct ToneTrack {
  enum class Kind : uint8_t { Sine, Silence, Clicks };

  static constexpr uint32_t kRate = 44100;  // without "@<rate>"
  static constexpr uint32_t kSineSeconds = 30;
  static constexpr uint32_t kSilenceSeconds = 3600;
  static constexpr uint32_t kClickSeconds = 60;

  Kind kind = Kind::Sine;
  float hz = 0.0f;        // Sine
  bool leftOnly = false;  // Sine: "tone:left"
  ClickGen::Spec click;   // Clicks
  uint32_t rate = kRate;  // the rate it is made at
  uint32_t seconds = 0;   // its length

  // `path` with its "tone:". False: not a built-in track (an unknown name,
  // a sine at or above the rate's Nyquist, a malformed or zero rate).
  static bool parse(const std::string& path, ToneTrack* out);
};
