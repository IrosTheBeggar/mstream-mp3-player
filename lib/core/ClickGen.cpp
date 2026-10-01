// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "ClickGen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
constexpr float kTwoPi = 6.283185307f;
constexpr float kPi = 3.141592654f;
constexpr float kClickMs = 15.0f;
constexpr float kOffBeatOffset = 0.37f;  // "off" tracks: first beat at 0.37 of a period
constexpr float kUnaccentedDb = -4.0f;
constexpr float kTickLevel = 0.35f;      // of the thump
}  // namespace

bool ClickGen::parse(const std::string& name, Spec* spec) {
  if (name.rfind("click", 0) != 0) return false;
  std::string rest = name.substr(5);
  bool off = false;
  if (rest.size() > 3 && rest.compare(rest.size() - 3, 3, "off") == 0) {
    off = true;
    rest.resize(rest.size() - 3);
  }
  if (rest.empty()) return false;
  for (char c : rest) {
    if (c < '0' || c > '9') return false;
  }
  const int bpm = std::atoi(rest.c_str());
  if (bpm < 30 || bpm > 300) return false;
  if (spec) {
    spec->bpm = static_cast<float>(bpm);
    spec->offsetBeats = off ? kOffBeatOffset : 0.0f;
  }
  return true;
}

void ClickGen::start(uint32_t sampleRate, const Spec& spec, uint32_t durationFrames, float levelDbfs) {
  spec_ = spec;
  rate_ = sampleRate;
  total_ = durationFrames;
  pos_ = 0;
  periodFrames_ = 60.0 * sampleRate / spec.bpm;
  offset_ = static_cast<uint32_t>(std::llround(spec.offsetBeats * periodFrames_));
  nextBeat_ = 0;
  nextFrame_ = beatFrame(0);
  clickLen_ = static_cast<uint32_t>(sampleRate * kClickMs / 1000.0f);
  // Scaled so the accented click peaks at exactly levelDbfs.
  float peak = 0.0f, phase = 0.0f;
  for (uint32_t i = 0; i < clickLen_; ++i) peak = std::max(peak, std::fabs(shape(i, &phase)));
  level_ = std::pow(10.0f, levelDbfs / 20.0f) / (peak > 0.0f ? peak : 1.0f);
  softLevel_ = level_ * std::pow(10.0f, kUnaccentedDb / 20.0f);
  clickPos_ = clickLen_;
  accent_ = false;
  thumpPhase_ = 0.0f;
}

uint32_t ClickGen::beatFrame(uint32_t k) const {
  return offset_ + static_cast<uint32_t>(std::llround(static_cast<double>(k) * 60.0 * rate_ / spec_.bpm));
}

int32_t ClickGen::offsetFromNearestBeat(uint32_t frame) const {
  if (frame <= offset_) return static_cast<int32_t>(frame) - static_cast<int32_t>(offset_);
  const auto k = static_cast<uint32_t>((frame - offset_) / periodFrames_);
  const int64_t before = static_cast<int64_t>(frame) - beatFrame(k);
  const int64_t after = static_cast<int64_t>(frame) - beatFrame(k + 1);
  return static_cast<int32_t>(std::llabs(before) <= std::llabs(after) ? before : after);
}

// Frame i of a click, at full scale 1 before normalising: the thump glides
// 100 -> 60 Hz and decays over ~5 ms, the tick is 2 kHz and gone in ~1 ms.
// A 0.5 ms attack and a raised-cosine window over the whole click keep both
// ends at 0. `phase` is the thump's, carried from frame to frame.
float ClickGen::shape(uint32_t i, float* phase) const {
  const float t = static_cast<float>(i) / static_cast<float>(rate_);
  const float freq = 60.0f + 40.0f * std::exp(-t / 0.004f);
  *phase += kTwoPi * freq / static_cast<float>(rate_);
  const float attack = t < 0.0005f ? t / 0.0005f : 1.0f;
  const float window = 0.5f + 0.5f * std::cos(kPi * static_cast<float>(i) / static_cast<float>(clickLen_));
  const float thump = std::sin(*phase) * std::exp(-t / 0.005f);
  const float tick = kTickLevel * std::sin(kTwoPi * 2000.0f * t) * std::exp(-t / 0.0008f);
  return attack * window * (thump + tick);
}

uint32_t ClickGen::generate(int16_t* out, uint32_t frames) {
  const uint32_t n = frames < total_ - pos_ ? frames : total_ - pos_;
  for (uint32_t i = 0; i < n; ++i, ++pos_) {
    if (pos_ == nextFrame_) {
      accent_ = nextBeat_ % 4 == 0;
      clickPos_ = 0;
      thumpPhase_ = 0.0f;
      nextFrame_ = beatFrame(++nextBeat_);
    }
    int16_t s = 0;
    if (clickPos_ < clickLen_) {
      const float v = shape(clickPos_, &thumpPhase_) * (accent_ ? level_ : softLevel_);
      s = static_cast<int16_t>(std::lround(32767.0f * v));
      ++clickPos_;
    }
    out[2 * i] = s;
    out[2 * i + 1] = s;
  }
  return n;
}
