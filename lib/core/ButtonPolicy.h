// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "ButtonGesture.h"
#include "InputEvent.h"

// What the three touch buttons under the screen do: the same on every
// screen, in every mode and overlay (tab bar spec §4), so the screen never
// spends pixels labelling them.
//
//   A  click: previous track        hold: volume down 5 %, again every 200 ms
//   B  click: play / pause          hold: switch the output (Bluetooth <-> speaker)
//   C  click: next track            hold: volume up 5 %, again every 200 ms
//
// (A's previous track is PlaybackController's prev: past a track's first
// 3 s the same track from 0:00, paused still paused.)
//
// Holds act at 500 ms (the user's presses: clicks 17-143 ms, holds from
// 509 ms). Switching TO the speaker always pauses first, so a slow press on
// B meant as a pause never moves the music out loud; B plays it again.
// Switching to Bluetooth keeps playing or paused as it was (the audio stays
// on the speaker until the headphones are up).
//
// A B click that would start the speaker right after the screen woke from
// off, before anyone touched the glass, is refused (Transport::
// startRefused(): the pocket rule, docs/ENERGY.md item 2).
//
// Every hold leaves a Feedback for the screen's HUD (the volume, or where
// the output went and whether that paused). Portable: the Transport is
// main.cpp's.
class ButtonPolicy {
public:
  static constexpr int kButtonA = 0, kButtonB = 1, kButtonC = 2;
  static constexpr uint32_t kHoldMs = 500;
  static constexpr uint32_t kVolumeRepeatMs = 200;
  static constexpr int kVolumeStep = 5;

  // The gesture settings for each button: A and C repeat their hold.
  static ButtonGesture::Config gestureFor(int button);

  class Transport {
  public:
    virtual void prev() = 0;
    virtual void next() = 0;
    virtual void playPause() = 0;
    virtual bool playing() const = 0;
    virtual void pause() = 0;
    // The active output's volume, by `delta` percent; volume() is where it
    // is headed (the HUD shows it).
    virtual void stepVolume(int delta) = 0;
    virtual int volume() const = 0;
    // Bluetooth is the output, or on its way to it (asked for, not linked
    // yet): a B hold then goes to the speaker.
    virtual bool onBluetooth() const = 0;
    // The audio plays through Bluetooth now (not only asked for): going to
    // the speaker pauses only then. Music still on the speaker while the
    // headphones connect plays on when a B hold cancels them (as the
    // Output tab's Speaker row does).
    virtual bool audioOnBluetooth() const { return onBluetooth(); }
    // Nothing to play (the queue is empty, no card): clicks do nothing,
    // and say so (handle() returns false: the input layer's "inert" buzz,
    // not the tap tick).
    virtual bool idle() const { return false; }
    // A play started now would sound on the speaker while nobody may be
    // looking (the screen woke from off and no touch on the glass since:
    // ScreenPower::unattended(), a pocket's contacts): a B click that
    // would start playing does nothing, and says so (the inert buzz). A
    // pause, the volume and a B hold still act.
    virtual bool startRefused() const { return false; }
    // Switches to the other output; false if it can't (silent test mode,
    // no headphones paired).
    virtual bool switchOutput() = 0;

  protected:
    ~Transport() = default;
  };

  enum class Hud : uint8_t { None, Volume, Output };
  struct Feedback {
    Hud kind = Hud::None;
    int volume = 0;            // Volume: the level after the step
    bool toBluetooth = false;  // Output: where it went (or would have)
    bool paused = false;       // Output: going to the speaker paused the music
    bool refused = false;      // Output: the switch was refused
    uint32_t ms = 0;
    uint32_t seq = 0;          // counts feedback: a HUD redraws when it changes
  };

  // Acts on a button event (Click, Hold, Repeat; Press and HoldEnd do
  // nothing). True if it did something (a click with nothing to play
  // doesn't).
  bool handle(const InputEvent& e, Transport& t);
  const Feedback& feedback() const { return feedback_; }

private:
  void note(Feedback f, uint32_t ms);

  Feedback feedback_;
};
