// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AutoDjTrigger.h"

namespace autodj {

const char* waitName(Wait w) {
  switch (w) {
    case Wait::Due: return "due";
    case Wait::Off: return "off";
    case Wait::Empty: return "the queue is empty";
    case Wait::UpNext: return "tracks up next";
    case Wait::Idle: return "nothing plays";
    case Wait::Fenced: return "the library update's fence";
    case Wait::Card: return "the card is held";
    case Wait::Joining: return "the index joins";
    case Wait::RepeatOne: return "Repeat One";
    case Wait::SleepTimer: return "the sleep timer's end of queue";
    case Wait::Failures: return "picks failed to play";
  }
  return "?";
}

TriggerOut decide(const TriggerIn& in) {
  TriggerOut o;
  if (!in.on) return o;  // (Wait::Off, no hold)
  o.hold = in.failurePaused  ? Hold::Failures
           : in.repeatOne    ? Hold::RepeatOne
           : in.sleepEndOfQueue ? Hold::SleepTimer
                                : Hold::None;
  const bool session = in.play == PlayState::Playing || in.play == PlayState::Waiting;
  const bool finished = !session && in.finished;
  if (in.queueSize == 0) {
    o.wait = Wait::Empty;
  } else if (in.upNext > 0) {
    o.wait = Wait::UpNext;
  } else if (!session && !finished) {
    o.wait = Wait::Idle;
  } else if (in.fenced) {
    o.wait = Wait::Fenced;
  } else if (in.cardHeld || in.restartSet) {
    o.wait = Wait::Card;
  } else if (in.joining) {
    o.wait = Wait::Joining;
  } else if (in.repeatOne) {
    o.wait = Wait::RepeatOne;
  } else if (in.sleepEndOfQueue) {
    o.wait = Wait::SleepTimer;
  } else if (in.failurePaused) {
    o.wait = Wait::Failures;
  } else {
    o.wait = Wait::Due;
  }
  o.pick = o.wait == Wait::Due;
  o.cue = o.pick && finished;
  // Due but for the engine (1-3 and 6-8 hold): no fence until it is made.
  const bool last = in.queueSize > 0 && in.upNext == 0 && (session || finished);
  o.owed = last && in.joining && o.hold == Hold::None;
  return o;
}

TriggerIn fromPlayer(const PlaybackController& player, TriggerIn in) {
  const QueueModel& q = player.queue();
  in.queueSize = q.size();
  in.upNext = q.upNext();
  in.play = player.state();
  in.finished = player.endedAtEnd();
  in.fenced = player.fenced();
  in.repeatOne = player.repeat() == PlaybackController::Repeat::One;
  return in;
}

bool FailurePause::failed(bool byAutoDj) {
  if (!byAutoDj || paused()) return false;
  return ++n_ == kFailedPicks;
}

}  // namespace autodj
