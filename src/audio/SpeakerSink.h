// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "AmpGate.h"
#include "AudioTap.h"
#include "DeclickReader.h"
#include "PcmRing.h"
#include "audio/AudioShared.h"

// Plays the ring through the Core2's internal speaker. M5.Speaker drives the
// NS4168 amp (switched on through the power chip) and mixes to mono. A pump
// task moves frames from the ring into three rotating buffers queued with
// M5.Speaker.playRaw(); a buffer is refilled only after the speaker task has
// released it. That handshake comes from M5Unified's MP3_with_ESP8266Audio
// example (MIT). The speaker starts on first use, so the amp stays off while
// listening over Bluetooth.
//
// The amp and the I2S go off again 2 s after the pump last queued audio
// (paused, stopped, or the output moved to Bluetooth), and on again before
// the next buffer, with a short settle on zeros (AmpGate, ENERGY.md item 5).
// Both happen on the pump's own task, the only one that queues buffers.
//
// M5.Speaker drops straight to 0 when its queue runs dry, so every stretch of
// audio the pump queues ends at 0: a DeclickReader fades in after a gap, and
// on a short read, a pause or a switch to Bluetooth it appends a ~1.5 ms fade
// from the last frame to 0 (or, paused, fades the next 64 real frames out).
class SpeakerSink {
public:
  static constexpr uint8_t kConsumerId = 2;

  void begin(PcmRing& ring, AudioShared& shared);
  void setVolume(uint8_t percent);

  // What the pump queued (the ring's audio only, not the fades it appends),
  // for the beat tracker. nullptr if its PSRAM couldn't be had.
  const AudioTap* tap() const { return tap_; }
  // How long the end of a queued buffer takes to be heard: the measured wait
  // from the pump filling it to M5.Speaker releasing it (it has mixed it all
  // into the I2S DMA), plus that DMA's depth. 0 until measured.
  uint32_t queueLatencyUs() const { return queueUs_.load(std::memory_order_relaxed); }
  uint32_t dmaLatencyUs() const { return dmaUs_; }

  // Whether the pump copies what it queues to its tap (only while the Dance
  // tab is up). Any task.
  void setTapOn(bool on) {
    if (tap_) tap_->setEnabled(on);
  }

  // Power measurements (the console's Pa): the amp (the NS4168's enable,
  // AXP192 GPIO2) and M5.Speaker's I2S off as soon as the speaker is quiet,
  // or on and held on (zeros: nothing is heard) until an Off. Carried out
  // by the pump once it is idle and every queued buffer has been played, so
  // nothing is cut off. Without these the pump switches it by itself.
  enum class Amp : uint8_t { None, Off, On };
  void requestAmp(Amp a) { ampRequest_.store(a, std::memory_order_relaxed); }
  // A request not carried out yet (the speaker is still playing).
  bool ampPending() const {
    return ampRequest_.load(std::memory_order_relaxed) != Amp::None || ampAsking_.load(std::memory_order_relaxed);
  }
  // Held on by a Pa1.
  bool ampHeld() const { return ampHeld_.load(std::memory_order_relaxed); }
  // The amp's switches so far (free-running), and why the last one was
  // made, for the loop task's log.
  uint32_t ampSwitches() const { return ampSwitches_.load(std::memory_order_acquire); }
  AmpGate::Why ampWhy() const { return ampWhy_.load(std::memory_order_relaxed); }
  // M5.Speaker is running: its task, the I2S and the amp are on.
  static bool ampOn();

private:
  static void taskEntry(void* self);
  static void onBufferReleased(void* self, const void* data, uint8_t channel);
  void pump();
  void takeAmpRequest();
  void quietPass();  // nothing to queue this pass: maybe the amp goes off
  void switchAmp(AmpGate::Do d);

  static constexpr size_t kBuffers = 3;
  static constexpr uint32_t kFrames = 1024;  // ~23 ms at 44.1 kHz
  // Room after a buffer's audio for its fade to 0.
  static constexpr uint32_t kDecayFrames = Declicker::decayFramesFor(Declicker::kDefaultRampFrames);

  PcmRing* ring_ = nullptr;
  AudioShared* shared_ = nullptr;
  int16_t* buffers_[kBuffers] = {};
  std::atomic<bool> busy_[kBuffers] = {{false}, {false}, {false}};
  std::atomic<uint32_t> filledUs_[kBuffers] = {{0}, {0}, {0}};  // when the pump queued each; 0: not timed
  std::atomic<uint32_t> queueUs_{0};  // smoothed fill -> release time (speaker task)
  uint32_t dmaUs_ = 0;
  AudioTap* tap_ = nullptr;  // written only by the pump
  std::atomic<Amp> ampRequest_{Amp::None};  // loop task -> pump
  std::atomic<bool> ampAsking_{false};      // pump: the gate has a request not carried out
  std::atomic<bool> ampHeld_{false};
  std::atomic<uint32_t> ampSwitches_{0};
  std::atomic<AmpGate::Why> ampWhy_{AmpGate::Why::None};
  // Pump task only.
  DeclickReader reader_{kConsumerId};
  AmpGate gate_;
  int lastRate_ = 44100;  // of the last buffer queued
};
