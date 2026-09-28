#include "HeadsetKeys.h"

HeadsetKeys::Action HeadsetKeys::decide(PlayState state, Key key, bool pausedByTimer) {
  switch (key) {
    case Key::Play:
      return state == PlayState::Paused && !pausedByTimer ? Action::Resume : Action::Ignore;
    case Key::Pause:
      return state == PlayState::Playing ? Action::Pause : Action::Ignore;
    case Key::Next:
    case Key::Prev:
      return state == PlayState::Playing ? Action::Skip : Action::Cue;
  }
  return Action::Ignore;
}

bool HeadsetKeys::isInput(Action a, bool pausedByTimer) {
  switch (a) {
    case Action::Resume:
    case Action::Pause:
    case Action::Skip: return true;
    case Action::Cue: return !pausedByTimer;
    case Action::Ignore: return false;
  }
  return false;
}

HeadsetKeys::Action HeadsetKeys::apply(PlaybackController& player, Key key) {
  const Action a = decide(player.state(), key, player.pausedByTimer());
  const bool forward = key == Key::Next;
  switch (a) {
    case Action::Resume:
    case Action::Pause:
      player.togglePlayPause();  // only ever from the state decide() checked
      break;
    case Action::Skip:
      forward ? player.next() : player.prev();
      break;
    case Action::Cue:
      forward ? player.cueNext() : player.cuePrev();
      break;
    case Action::Ignore:
      break;
  }
  return a;
}
