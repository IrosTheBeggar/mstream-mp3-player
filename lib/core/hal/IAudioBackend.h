#pragma once
#include <cstdint>
#include <string>

// HAL seam for audio. The portable PlaybackController drives transport through
// this interface and never touches I2S or a codec directly.
//
//   Sim build (Wokwi):  SimAudioBackend advances a virtual playback head so the
//                       UI works, but produces no sound (no I2S in simulation).
//   Real build:         a future I2sAudioBackend decodes MP3/FLAC and streams
//                       PCM to the PCM5102A over I2S. See docs/ARCHITECTURE.md
//                       for the planned finer-grained IPcmSink seam that lets
//                       the *decode* pipeline run in Wokwi too.
class IAudioBackend {
public:
  virtual ~IAudioBackend() = default;

  // Begin playing `path`. `durationHintMs` is used by the sim backend; the real
  // backend learns true duration/EOF from the decoder and may ignore the hint.
  virtual bool play(const std::string& path, uint32_t durationHintMs) = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;

  // Pump the decode/output pipeline. Called every firmware loop with the
  // current monotonic time in ms.
  virtual void loop(uint32_t nowMs) = 0;

  virtual bool isPlaying() const = 0;     // playing and not paused
  virtual uint32_t positionMs() const = 0;
  virtual bool finished() const = 0;      // reached end of the current track
};
