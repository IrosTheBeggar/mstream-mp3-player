#pragma once
#include <cstdint>
#include <string>

// HAL seam for audio. The portable PlaybackController drives transport through
// this interface and never touches a decoder or an output directly. On the
// Core2 it is Core2AudioBackend (decode task -> PCM ring -> Bluetooth or the
// internal speaker); host tests use a fake.
class IAudioBackend {
public:
  virtual ~IAudioBackend() = default;

  // Begin playing `path`. `durationHintMs` may be ignored: the backend learns
  // the real end of the track from the decoder.
  virtual bool play(const std::string& path, uint32_t durationHintMs) = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;

  // Called every firmware loop with the current monotonic time in ms.
  virtual void loop(uint32_t nowMs) = 0;

  virtual bool isPlaying() const = 0;     // playing and not paused
  virtual uint32_t positionMs() const = 0;
  virtual bool finished() const = 0;      // reached end of the current track
  // The current track can't be played (missing file, unsupported format, or a
  // sample rate the active output can't take).
  virtual bool failed() const { return false; }
};
