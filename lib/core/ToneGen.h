#pragma once
#include <cmath>
#include <cstdint>

// Stereo sine generator for the built-in test tracks ("tone:440" etc.). They
// need no files, so Bluetooth and the speaker can be tested before any decoder
// is involved. The phase carries over between generate() calls, so the output
// doesn't depend on how the caller chunks it.
class ToneGen {
public:
  enum class Channels { Both, LeftOnly };

  void start(uint32_t sampleRate, float freqHz, float levelDbfs, Channels channels,
             uint32_t durationFrames) {
    rate_ = sampleRate;
    step_ = kTwoPi * freqHz / static_cast<float>(sampleRate);
    phase_ = 0.0f;
    amplitude_ = 32767.0f * std::pow(10.0f, levelDbfs / 20.0f);
    channels_ = channels;
    remaining_ = durationFrames;
  }

  // Writes up to `frames` interleaved stereo frames; returns how many (0 once
  // the tone's duration has been produced).
  uint32_t generate(int16_t* out, uint32_t frames) {
    const uint32_t n = frames < remaining_ ? frames : remaining_;
    for (uint32_t i = 0; i < n; ++i) {
      const auto s = static_cast<int16_t>(std::lround(amplitude_ * std::sin(phase_)));
      out[2 * i] = s;
      out[2 * i + 1] = channels_ == Channels::Both ? s : 0;
      phase_ += step_;
      if (phase_ >= kTwoPi) phase_ -= kTwoPi;
    }
    remaining_ -= n;
    return n;
  }

  bool done() const { return remaining_ == 0; }
  uint32_t sampleRate() const { return rate_; }

private:
  static constexpr float kTwoPi = 6.283185307f;

  uint32_t rate_ = 44100;
  float step_ = 0.0f;
  float phase_ = 0.0f;
  float amplitude_ = 0.0f;
  Channels channels_ = Channels::Both;
  uint32_t remaining_ = 0;
};
