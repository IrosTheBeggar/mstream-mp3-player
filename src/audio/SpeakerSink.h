#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "PcmRing.h"
#include "audio/AudioShared.h"

// Plays the ring through the Core2's internal speaker. M5.Speaker drives the
// NS4168 amp (switched on through the power chip) and mixes to mono. A pump
// task moves frames from the ring into three rotating buffers queued with
// M5.Speaker.playRaw(); a buffer is refilled only after the speaker task has
// released it. That handshake comes from M5Unified's MP3_with_ESP8266Audio
// example (MIT). The speaker starts on first use, so the amp stays off while
// listening over Bluetooth.
class SpeakerSink {
public:
  static constexpr uint8_t kConsumerId = 2;

  void begin(PcmRing& ring, AudioShared& shared);
  void setVolume(uint8_t percent);

private:
  static void taskEntry(void* self);
  static void onBufferReleased(void* self, const void* data, uint8_t channel);
  void pump();

  static constexpr size_t kBuffers = 3;
  static constexpr uint32_t kFrames = 1024;  // ~23 ms at 44.1 kHz

  PcmRing* ring_ = nullptr;
  AudioShared* shared_ = nullptr;
  int16_t* buffers_[kBuffers] = {};
  std::atomic<bool> busy_[kBuffers] = {{false}, {false}, {false}};
};
