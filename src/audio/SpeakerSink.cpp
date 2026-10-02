// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

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
// The amp's enable comes up only once M5.Speaker's I2S is set up and
// clocking zeros (kClockFirstMs after begin()), never while its pins are
// being reconfigured; then the amp gets kAmpSettleMs of zeros before the
// first buffer, so its start-up can't cut into the fade-in (tune by ear,
// ENERGY.md step 3).
constexpr uint32_t kClockFirstMs = 5;
constexpr uint32_t kAmpSettleMs = 20;

// The NS4168's enable: AXP192 GPIO2 (AXP2101 ALDO3 on later Core2s), as
// M5Unified's own callback switches it.
void ampEnable(bool on) {
  switch (M5.Power.getType()) {
    case m5::Power_Class::pmic_axp192: M5.Power.Axp192.setGPIO2(on); break;
    case m5::Power_Class::pmic_axp2101: M5.Power.Axp2101.setALDO3(on ? 3300 : 0); break;
    default: break;
  }
}

// M5.Speaker's enable callback, replacing M5Unified's (which raises the
// enable at the start of begin(), before the I2S is set up: the amp was
// live while the port was uninstalled and its clock pins reconfigured).
// begin() leaves it alone (the pump raises it once the clock runs);
// end() still drops it first, before the task and the I2S stop.
bool onSpeakerEnable(void*, bool enabled) {
  if (!enabled) ampEnable(false);
  return true;
}

// Speaker_Class::setCallback() is protected (M5Unified::begin() sets its
// own): reached through a pointer to it, taken where it is accessible.
struct SpeakerCallbackAccess : m5::Speaker_Class {
  using Setter = void (m5::Speaker_Class::*)(void*, bool (*)(void*, bool));
  static Setter setter() { return &SpeakerCallbackAccess::setCallback; }
};
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
  if (tapBuffer) {
    tap_ = new AudioTap(tapBuffer, kTapFrames);
    tap_->setEnabled(false);  // until the Dance tab is up (DanceMode)
  }

  auto cfg = M5.Speaker.config();
  cfg.sample_rate = AudioShared::kRingRate;  // the ring's: every track is converted to it
  cfg.task_pinned_core = APP_CPU_NUM;  // keep it off the Bluetooth core
  M5.Speaker.config(cfg);
  // The I2S DMA holds this much after M5.Speaker has mixed a buffer in.
  dmaUs_ = static_cast<uint32_t>(static_cast<uint64_t>(cfg.dma_buf_len) * cfg.dma_buf_count * 1000000u /
                                 cfg.sample_rate);
  // Has to be registered before the first playRaw().
  M5.Speaker.setBufferReleaseCallback(this, onBufferReleased);
  // The amp's enable is the pump's to raise (switchAmp), after the clock.
  (M5.Speaker.*SpeakerCallbackAccess::setter())(nullptr, onSpeakerEnable);

  xTaskCreatePinnedToCore(taskEntry, "speaker", 3072, this, 3, nullptr, APP_CPU_NUM);
}

void SpeakerSink::setVolume(uint8_t percent) {
  M5.Speaker.setVolume(static_cast<uint8_t>(percent * kMaxVolume / 100));
}

void SpeakerSink::taskEntry(void* self) { static_cast<SpeakerSink*>(self)->pump(); }

bool SpeakerSink::ampOn() { return M5.Speaker.isRunning(); }

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

// A Pa request from the loop task, to the gate. ampAsking_ goes up before
// the request comes down, so ampPending() never reads false in between.
void SpeakerSink::takeAmpRequest() {
  const Amp a = ampRequest_.load(std::memory_order_relaxed);
  if (a == Amp::None) return;
  gate_.ask(a == Amp::On ? AmpGate::Ask::On : AmpGate::Ask::Off);
  ampAsking_.store(true, std::memory_order_relaxed);
  Amp expected = a;
  ampRequest_.compare_exchange_strong(expected, Amp::None, std::memory_order_relaxed);
}

void SpeakerSink::switchAmp(AmpGate::Do d) {
  if (d == AmpGate::Do::Nothing) return;
  if (d == AmpGate::Do::Stop) {
    // The amp's enable first, then M5.Speaker's task and the I2S (end()
    // does it in that order), on the zeros that followed the last fade.
    M5.Speaker.end();
  } else {
    // The I2S first (set up, its task clocking zeros), the amp's enable
    // only then, and zeros into the amp before any audio.
    M5.Speaker.begin();
    vTaskDelay(pdMS_TO_TICKS(kClockFirstMs));
    ampEnable(true);
    vTaskDelay(pdMS_TO_TICKS(kAmpSettleMs));
  }
  ampWhy_.store(gate_.why(), std::memory_order_relaxed);
  ampSwitches_.fetch_add(1, std::memory_order_release);
}

void SpeakerSink::quietPass() {
  // end() mustn't drop a buffer still queued: its release would never come
  // and the pump would wait on it for good.
  const bool drained = !busy_[0] && !busy_[1] && !busy_[2];
  switchAmp(gate_.quiet(millis(), M5.Speaker.isRunning(), drained));
  ampAsking_.store(gate_.asking(), std::memory_order_relaxed);
  ampHeld_.store(gate_.held(), std::memory_order_relaxed);
}

void SpeakerSink::pump() {
  size_t idx = 0;
  int waited = 0;
  bool refilling = false;  // the ring was empty when last looked at
  for (;;) {
    takeAmpRequest();
    const bool paused = shared_->paused;
    const bool playing = ring_->consumer() == kConsumerId && !paused;
    // Idle once paused or handed to Bluetooth, and the fade-out is queued.
    if (!playing && reader_.silent()) {
      quietPass();
      waited = 0;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (busy_[idx]) {  // the speaker is still playing this buffer
      vTaskDelay(1);
      continue;
    }
    // Send full buffers; a short one only once the track stops producing.
    // After the ring ran empty (a start, a seek, a resume point, an
    // underrun) the wait begins with the first frames back, not before:
    // the decoder writes 256 at a time (RingFeed's stage), and a wait that
    // ran out while they came in sent a short buffer, faded out, and faded
    // the rest in again 6-17 ms into the start (docs/SEEK.md section 17).
    const uint32_t have = ring_->size();
    if (have == 0) {
      refilling = true;
    } else if (refilling) {
      refilling = false;
      waited = 0;
    }
    if (playing && have < kFrames && waited < kTopUpWaitMs) {
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
      // Nothing to send (stopped with the speaker the output, an underrun):
      // as quiet as paused. Also yields when the ring was busy: the task
      // holding its lock (setConsumer on the loop task, discardAll on the
      // decode task) runs on this core at a lower priority and must get to
      // finish.
      quietPass();
      vTaskDelay(1);
      continue;
    }
    const bool running = M5.Speaker.isRunning();
    if (!running && r.read == 0) {
      // Only a fade to 0 after audio that was never heard (the amp is off):
      // nothing to play.
      continue;
    }
    // The amp on again (after a quiet spell) before any audio reaches it.
    switchAmp(gate_.beforeQueue(running));
    // A copy of the real audio for the beat tracker (never the fade after
    // it; nothing while the Dance tab isn't up), and when this buffer was
    // filled, to time its release.
    const auto nowUs = static_cast<uint32_t>(esp_timer_get_time());
    if (r.read > 0 && tap_) tap_->write(buffers_[idx], r.read, r.read, r.epoch, r.position, nowUs);
    // The sleep timer's fade (ENERGY.md section 3), after the tap (the
    // tracker hears the music) and before playRaw: at most 1, one level
    // for both outputs. Moved only while the speaker is the consumer.
    shared_->fade.process(buffers_[idx], r.total, ring_->consumer() == kConsumerId);
    filledUs_[idx].store(r.read == kFrames ? (nowUs | 1u) : 0u, std::memory_order_relaxed);
    busy_[idx] = true;  // before playRaw(): the release can only come after it's queued
    if (!M5.Speaker.playRaw(buffers_[idx], r.total * 2, AudioShared::kRingRate, true, 1, kChannel)) {
      busy_[idx] = false;
    }
    gate_.queued(millis());
    idx = (idx + 1) % kBuffers;
  }
}
