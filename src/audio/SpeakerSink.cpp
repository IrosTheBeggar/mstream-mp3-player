#include "audio/SpeakerSink.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

namespace {
constexpr int kChannel = 0;  // M5.Speaker virtual channel
// Loudest M5.Speaker master volume that can't clip. M5 sums L+R into mono with
// a gain of 2 * magnification(16) * (volume * 255)^2 / 2^36, which is unity at
// volume ~181.7; at 200 it is +1.7 dB and loud masters hard-clip.
constexpr uint8_t kMaxVolume = 181;
constexpr int kTopUpWaitMs = 20;      // wait this long for a full buffer before sending a short one
// What the pump queued, for the beat tracker: ~0.74 s, 64 KB of PSRAM.
constexpr uint32_t kTapFrames = 32768;
}  // namespace

void SpeakerSink::begin(PcmRing& ring, AudioShared& shared) {
  ring_ = &ring;
  shared_ = &shared;
  reader_.bind(ring);
  for (auto& b : buffers_) {
    b = static_cast<int16_t*>(
        heap_caps_malloc((kFrames + kDecayFrames) * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  }

  auto* tapBuffer = static_cast<int16_t*>(heap_caps_malloc(kTapFrames * sizeof(int16_t), MALLOC_CAP_SPIRAM));
  if (tapBuffer) tap_ = new AudioTap(tapBuffer, kTapFrames);

  auto cfg = M5.Speaker.config();
  cfg.sample_rate = 44100;             // most music needs no resampling
  cfg.task_pinned_core = APP_CPU_NUM;  // keep it off the Bluetooth core
  M5.Speaker.config(cfg);
  // The I2S DMA holds this much after M5.Speaker has mixed a buffer in.
  dmaUs_ = static_cast<uint32_t>(static_cast<uint64_t>(cfg.dma_buf_len) * cfg.dma_buf_count * 1000000u /
                                 cfg.sample_rate);
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
    if (data != s->buffers_[i]) continue;
    // Its wait in M5.Speaker's queue, smoothed (1/8 per buffer), for the
    // dancing figure's timing. The speaker task is the only writer.
    const uint32_t filled = s->filledUs_[i].exchange(0, std::memory_order_relaxed);
    if (filled != 0) {
      const auto waited = static_cast<int32_t>(static_cast<uint32_t>(esp_timer_get_time()) - filled);
      const auto q = static_cast<int32_t>(s->queueUs_.load(std::memory_order_relaxed));
      if (waited > 0 && waited < 1000000) {
        s->queueUs_.store(q == 0 ? waited : q + (waited - q) / 8, std::memory_order_relaxed);
      }
    }
    s->busy_[i] = false;
  }
}

void SpeakerSink::pump() {
  size_t idx = 0;
  int waited = 0;
  for (;;) {
    const bool paused = shared_->paused;
    const bool playing = ring_->consumer() == kConsumerId && !paused;
    // Idle once paused or handed to Bluetooth, and the fade-out is queued.
    if (!playing && reader_.silent()) {
      waited = 0;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (busy_[idx]) {  // the speaker is still playing this buffer
      vTaskDelay(1);
      continue;
    }
    // Send full buffers; a short one only once the track stops producing.
    if (playing && ring_->size() < kFrames && waited < kTopUpWaitMs) {
      ++waited;
      vTaskDelay(1);
      continue;
    }
    waited = 0;

    // Up to kFrames of the ring's audio (faded in after a gap or skip); if it
    // came up short, paused or lost the ring, then a fade to 0 after it.
    const DeclickReader::Result r = reader_.pull(buffers_[idx], kFrames, !paused);
    if (playing && r.read < r.wanted && shared_->expectingAudio) shared_->underruns++;
    if (r.total == 0) {
      // Also yields when the ring was busy: the task holding its lock
      // (setConsumer on the loop task, discardAll on the decode task) runs
      // on this core at a lower priority and must get to finish.
      vTaskDelay(1);
      continue;
    }
    // Read the rate after the frames: it is published before a track's first
    // frame. A fade-only buffer keeps the rate of the audio it ends.
    if (r.read > 0) lastRate_ = shared_->rate;
    // A copy of the real audio for the beat tracker (never the fade after
    // it), and when this buffer was filled, to time its release.
    const auto nowUs = static_cast<uint32_t>(esp_timer_get_time());
    if (r.read > 0 && tap_) tap_->write(buffers_[idx], r.read, r.read, r.epoch, r.position, nowUs);
    filledUs_[idx].store(r.read == kFrames ? (nowUs | 1u) : 0u, std::memory_order_relaxed);
    busy_[idx] = true;  // before playRaw(): the release can only come after it's queued
    if (!M5.Speaker.playRaw(buffers_[idx], r.total * 2, lastRate_, true, 1, kChannel)) {
      busy_[idx] = false;
    }
    idx = (idx + 1) % kBuffers;
  }
}
