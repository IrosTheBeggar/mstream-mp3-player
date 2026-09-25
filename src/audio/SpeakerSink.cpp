#include "audio/SpeakerSink.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>

namespace {
constexpr int kChannel = 0;  // M5.Speaker virtual channel
// Loudest M5.Speaker master volume that can't clip. M5 sums L+R into mono with
// a gain of 2 * magnification(16) * (volume * 255)^2 / 2^36, which is unity at
// volume ~181.7; at 200 it is +1.7 dB and loud masters hard-clip.
constexpr uint8_t kMaxVolume = 181;
constexpr int kTopUpWaitMs = 20;      // wait this long for a full buffer before sending a short one
}  // namespace

void SpeakerSink::begin(PcmRing& ring, AudioShared& shared) {
  ring_ = &ring;
  shared_ = &shared;
  for (auto& b : buffers_) {
    b = static_cast<int16_t*>(heap_caps_malloc(kFrames * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  }

  auto cfg = M5.Speaker.config();
  cfg.sample_rate = 44100;             // most music needs no resampling
  cfg.task_pinned_core = APP_CPU_NUM;  // keep it off the Bluetooth core
  M5.Speaker.config(cfg);
  // Has to be registered before the first playRaw().
  M5.Speaker.setBufferReleaseCallback(this, onBufferReleased);

  xTaskCreatePinnedToCore(taskEntry, "speaker", 3072, this, 3, nullptr, APP_CPU_NUM);
}

void SpeakerSink::setVolume(uint8_t percent) {
  M5.Speaker.setVolume(static_cast<uint8_t>(percent * kMaxVolume / 100));
}

void SpeakerSink::taskEntry(void* self) { static_cast<SpeakerSink*>(self)->pump(); }

void SpeakerSink::onBufferReleased(void* self, const void* data, uint8_t) {
  auto* s = static_cast<SpeakerSink*>(self);
  for (size_t i = 0; i < kBuffers; ++i) {
    if (data == s->buffers_[i]) s->busy_[i] = false;
  }
}

void SpeakerSink::pump() {
  size_t idx = 0;
  int waited = 0;
  for (;;) {
    if (ring_->consumer() != kConsumerId || shared_->paused) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (busy_[idx]) {  // the speaker is still playing this buffer
      vTaskDelay(1);
      continue;
    }
    // Send full buffers; a short one only once the track stops producing.
    if (ring_->size() < kFrames && waited < kTopUpWaitMs) {
      ++waited;
      vTaskDelay(1);
      continue;
    }
    waited = 0;

    const uint32_t n = ring_->read(kConsumerId, buffers_[idx], kFrames);
    if (n == 0) {
      if (shared_->expectingAudio) shared_->underruns++;
      continue;
    }
    // Read the rate after the frames: it is published before a track's first frame.
    const int rate = shared_->rate;
    busy_[idx] = true;  // before playRaw(): the release can only come after it's queued
    if (!M5.Speaker.playRaw(buffers_[idx], n * 2, rate, true, 1, kChannel)) busy_[idx] = false;
    idx = (idx + 1) % kBuffers;
  }
}
