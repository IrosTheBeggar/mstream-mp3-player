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

  // Begin playing `path`, `startMs` into it (0: from its start; a resume
  // point: the backend decides where that really lands, and positionMs()
  // counts from there). `durationHintMs`: its length as known elsewhere (a
  // resume point's, as the backend had it), 0: none; a hint for placing a
  // start part of the way in (a VBR MP3 without a table of contents): the
  // backend learns the real end of the track from the decoder.
  virtual bool play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) = 0;
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;

  // Called every firmware loop with the current monotonic time in ms.
  virtual void loop(uint32_t nowMs) = 0;

  virtual bool isPlaying() const = 0;     // playing and not paused
  virtual uint32_t positionMs() const = 0;
  // positionMs() is the last play()'s track: false while that start hasn't
  // been taken up yet (a backend that starts asynchronously may still count
  // the track before). PlaybackController's prev reads the position only
  // then; before, it goes by where it asked that play to start.
  virtual bool positionKnown() const { return true; }
  // The current track's length (0: not known).
  virtual uint32_t durationMs() const { return 0; }
  virtual bool finished() const = 0;      // reached end of the current track
  // The current track can't be played (missing file, unsupported format, or a
  // sample rate the active output can't take).
  virtual bool failed() const { return false; }
};
