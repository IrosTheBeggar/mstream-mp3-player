// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// Which of the Dance tab's panels to draw (ui/DancePage), and what each last
// showed. A panel is drawn when its value changes, at most twice a second, or
// when the page asks for all of them.
//
// A toast's end asks for the panels in its row alone (`topOnly`: the beat and
// the dancer, y 44-99). The others weren't drawn, so their values stay as they
// were: what changed meanwhile is still different at the next pass and drawn
// then. Remembering them regardless left an old title on the screen until the
// next track.
//
// Portable, no heap.
namespace dancepanels {

struct Values {
  int bpmX10 = -1;               // the tempo in tenths, 0: none
  int lock = -1;                 // 0 no beat, 1 listening, 2 locked
  int conf = -1;                 // the confidence, in percent
  int skin = -1;                 // the cast entry chosen last
  uint32_t track = 0xFFFFFFFFu;  // AppState::trackId
  int progress = -1;             // the progress line's width, in px
  int host = -1;                 // 1: a computer drives the dancer (the USB visualizer)
};

struct Draw {
  bool beat;      // the left panel's tempo
  bool lock;      // its locked / listening / no beat pill
  bool conf;      // its confidence
  bool dancer;    // the right panel
  bool track;     // the bottom: the title and artist, or the computer's texts
  bool progress;  // the 2 px line under them
};

// What to draw for `now`, with `shown` updated to match. `all`: every panel in
// scope; `topOnly`: only the beat and the dancer are in scope.
inline Draw plan(Values& shown, const Values& now, bool all, bool topOnly = false) {
  const bool rest = !topOnly;
  Draw d;
  d.beat = all || now.bpmX10 / 10 != shown.bpmX10 / 10;
  d.lock = rest && (all || now.lock != shown.lock);
  d.conf = rest && (all || now.conf != shown.conf);
  d.dancer = all || now.skin != shown.skin;
  d.track = rest && (all || now.track != shown.track || now.host != shown.host);
  d.progress = rest && (all || now.progress != shown.progress || now.host != shown.host);
  shown.bpmX10 = now.bpmX10;
  shown.skin = now.skin;
  if (rest) {
    shown.lock = now.lock;
    shown.conf = now.conf;
    shown.track = now.track;
    shown.progress = now.progress;
    shown.host = now.host;
  }
  return d;
}

}  // namespace dancepanels
