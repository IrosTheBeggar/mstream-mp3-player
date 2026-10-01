// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// When the speaker's amp may be off (ENERGY.md item 5): the NS4168's enable
// (AXP192 GPIO2) and M5.Speaker's I2S, which M5.Speaker::end() switches off
// together and begin() on again. On, they cost ~5.3 USB mA (measured), and
// before this they stayed on from the first speaker use until reboot,
// through all later Bluetooth listening.
//
// The speaker pump asks this on every pass, and does what it says:
//
// - Off (Stop) 2 s after the pump last queued audio, once every buffer it
//   queued has been played (M5.Speaker has released them all: end() must
//   not drop one). Paused, stopped, the queue's end, or the output moved to
//   Bluetooth are all the same here: nothing was queued for 2 s.
// - On (Start) before the next buffer is queued, if it is off. The pump
//   then lets the amp settle on the I2S's zeros before the audio, whose
//   start the DeclickReader fades in (it is always after a gap).
//
// The console's Pa (power measurements): Pa0 switches it off as soon as
// the speaker is quiet (no 2 s wait); Pa1 switches it on (the I2S clocks
// zeros into it: nothing is heard) and holds it on, through later playing
// and pausing, until Pa0.
//
// One task only (the pump). Times are millis(), wrapping.
class AmpGate {
public:
  static constexpr uint32_t kQuietMs = 2000;

  enum class Ask : uint8_t { None, Off, On };  // Pa0 / Pa1
  enum class Do : uint8_t { Nothing, Start, Stop };
  // Of the last Start or Stop: Quiet (a Stop, 2 s without audio), Playing (a
  // Start, audio to queue), Asked (the console's Pa).
  enum class Why : uint8_t { None, Quiet, Playing, Asked };

  // A console request, carried out by a later quiet() (the pump is idle).
  void ask(Ask a) { ask_ = a; }
  // Asked and not carried out yet.
  bool asking() const { return ask_ != Ask::None; }
  // Pa1 holds it on.
  bool held() const { return held_; }

  // The pump has a buffer of audio to queue; `running`: M5.Speaker is on.
  // Start: begin it first.
  Do beforeQueue(bool running) {
    if (running) return Do::Nothing;
    why_ = Why::Playing;
    return Do::Start;
  }
  // The pump queued a buffer at `nowMs`.
  void queued(uint32_t nowMs) {
    lastAudioMs_ = nowMs;
    known_ = true;
  }
  // A pass with nothing to queue. `drained`: every queued buffer has been
  // released.
  Do quiet(uint32_t nowMs, bool running, bool drained);

  // Why the last Start or Stop was asked for (for the log).
  Why why() const { return why_; }

private:
  Ask ask_ = Ask::None;
  bool held_ = false;
  bool known_ = false;        // lastAudioMs_ is the start of this quiet spell
  uint32_t lastAudioMs_ = 0;  // the last buffer queued, or the quiet spell's first pass
  Why why_ = Why::None;
};
