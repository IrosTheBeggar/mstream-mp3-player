#pragma once
#include "hal/IAudioBackend.h"

// Simulated audio backend for Wokwi (and anywhere without real I2S hardware).
// It advances a virtual playback head based on wall-clock time so the UI's
// progress bar and auto-advance behave realistically — it just makes no sound.
//
// Swap this for a real I2sAudioBackend (decode -> PCM5102A over I2S) on the
// physical board; PlaybackController is unaware of which one it's driving.
class SimAudioBackend : public IAudioBackend {
public:
  bool play(const std::string& path, uint32_t durationHintMs) override {
    (void)path;
    durMs_ = durationHintMs ? durationHintMs : 180000;  // default 3:00 if unknown
    posMs_ = 0;
    playing_ = true;
    paused_ = false;
    finished_ = false;
    lastMs_ = 0;
    return true;
  }

  void pause() override { paused_ = true; }
  void resume() override { paused_ = false; lastMs_ = 0; }

  void stop() override {
    playing_ = false;
    paused_ = false;
    finished_ = false;
    posMs_ = 0;
  }

  void loop(uint32_t nowMs) override {
    if (playing_ && !paused_) {
      if (lastMs_ == 0) lastMs_ = nowMs;          // re-sync after start/resume
      posMs_ += (nowMs - lastMs_);
      lastMs_ = nowMs;
      if (posMs_ >= durMs_) {
        posMs_ = durMs_;
        playing_ = false;
        finished_ = true;
      }
    } else {
      lastMs_ = nowMs;
    }
  }

  bool isPlaying() const override { return playing_ && !paused_; }
  uint32_t positionMs() const override { return posMs_; }
  bool finished() const override { return finished_; }

private:
  uint32_t durMs_ = 0;
  uint32_t posMs_ = 0;
  uint32_t lastMs_ = 0;
  bool playing_ = false;
  bool paused_ = false;
  bool finished_ = false;
};
