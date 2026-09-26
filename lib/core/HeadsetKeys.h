#pragma once
#include <cstdint>

#include "PlaybackController.h"

// What a transport key on the Bluetooth headphones (AVRCP passthrough) does
// to the player. Portable, so the rule is host-tested; main.cpp adds the
// Core2 side (which output plays, the prompt suspend after a pause).
//
// The rule: headphone input never starts music that wasn't playing. In-ear
// detection sends PLAY and PAUSE, and a bud being put in, taken out or
// adjusted (a Powerbeats double-press) sends keys nobody meant for us.
//   - PLAY resumes only what was paused. Stopped (after a boot and an
//     automatic reconnect, say), it does nothing.
//   - PAUSE pauses what plays; otherwise nothing.
//   - NEXT/PREV skip while playing. Paused or stopped, they only move to the
//     next or previous track (PlaybackController::cueNext()/cuePrev()): the
//     screen shows it, the player stays paused or stopped, and a later play
//     starts it.
// Play and pause are commands, never a toggle: a repeat must not undo the
// first. The Core2's own buttons and the console are not headphone input and
// keep their toggle and skip-and-play behaviour.
class HeadsetKeys {
public:
  enum class Key : uint8_t { Play, Pause, Next, Prev };
  enum class Action : uint8_t {
    Ignore,  // nothing happens
    Resume,  // paused -> playing (a cued track starts from its beginning)
    Pause,   // playing -> paused
    Skip,    // playing: next()/prev(), which plays the new track
    Cue,     // paused or stopped: cueNext()/cuePrev(), nothing starts
  };

  static Action decide(PlayState state, Key key);
  // decide(), carried out on `player`. Returns what it did.
  static Action apply(PlaybackController& player, Key key);
};
