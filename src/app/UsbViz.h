// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include <functional>
#include <utility>

#include "HostLine.h"
#include "HostLink.h"
#include "app/DanceMode.h"

// The USB visualizer on the Core2 (docs/USB-VISUALIZER.md): while a
// computer plays music, it sends the beat tracker's hop energies and its
// heard clock down the USB cable as '@' lines, and the Dance tab's dancer
// dances to them. HostLink (lib/core) decides what each line means; this
// carries it out, on the loop task:
//
//   Enter    (@hello ... viz): the dancer in host mode, then main.cpp's
//            enter(): the player paused (never resumed by itself; marked
//            the computer's, so the headphones' Play doesn't either), a test
//            track the console started stopped, the screen woken, the Dance
//            tab shown. While on, main.cpp keeps the screen lit, rests the
//            headphones' background search and counts it as busy for the
//            idle power-off.
//   Exit     (@bye, 3 s of nothing, USB unplugged, a touch outside the
//            dancer or a button: userEnded(), the Dance tab gone): the dancer
//            back on the Core2's own audio; the player stays paused, the
//            tab stays on Dance.
//   Epoch, Prior, Hop, Clock, Log: to the dancer (DanceMode's host*()).
//
// Each reply goes out with one printf. Logged: a line on entry and one on
// exit; nothing per line (the [dance] line every 5 s has the counters).
class UsbViz {
public:
  struct Hooks {
    // Why host mode can't start now (HostLink::Busy::None: it can).
    std::function<HostLink::Busy()> busy;
    // Entering: pause, wake, show the Dance tab. True: something was
    // playing (or a test track) and is paused (stopped) now.
    std::function<bool()> enter;
    // The Dance tab went away some other way than an exit (another screen
    // took the display, the console's d). (Not the screen being dark: the
    // dancer stops then and comes back with the wake.)
    std::function<bool()> danceGone;
  };

  UsbViz(DanceMode& dance, Hooks hooks) : dance_(dance), hooks_(std::move(hooks)) {}

  // The firmware version @ok names.
  void begin(const char* fw) { link_.begin(fw); }
  // From the console: a complete line (`line`, writable) or one that wasn't
  // (`line` nullptr: Bad, Long, Restart).
  void onLine(char* line, HostLine::Byte kind);
  // Every loop pass, after the UI's: the 3 s timeout, USB power gone, the
  // Dance tab gone some other way.
  void loop(uint32_t nowMs, bool usbPower);
  // The Core2's own input ends host mode: a touch outside the dancer, a
  // button or the PWR key, the headphones' play key.
  void userEnded(HostLink::Why why);
  bool active() const { return link_.active(); }

private:
  void apply(const HostLink::Out& o, uint32_t nowUs);

  DanceMode& dance_;
  Hooks hooks_;
  HostLink link_;
  HostLink::Busy lastRefusal_ = HostLink::Busy::None;  // logged once per reason
};
