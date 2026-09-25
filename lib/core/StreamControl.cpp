#include "StreamControl.h"

void StreamControl::send(Actions& a, Cmd c, State next, uint32_t nowMs) {
  a.send = c;
  state_ = next;
  sentMs_ = nowMs;
}

// Where every call ends: at most one new command, never while one is outstanding.
void StreamControl::evaluate(Actions& a, uint32_t nowMs) {
  if (!linked_ || a.send != Cmd::None) return;
  if (state_ == State::Checking || state_ == State::Starting || state_ == State::Suspending) {
    if (!reached(nowMs, sentMs_ + kAckTimeoutMs)) return;  // still waiting for its answer
    // Unanswered: give up on it. If the stack still holds it, the next
    // command is answered BUSY and the retries back off.
    a.gaveUp = true;
    state_ = audioStarted_ ? State::Started : State::Idle;
  }
  if (state_ == State::Started && !audioStarted_ && reached(nowMs, sentMs_ + kAckTimeoutMs)) {
    // START acknowledged (sentMs_ is when it went out) but the stream never
    // reported STARTED: the ack was a stop_tx's blanket SUCCESS, or the real
    // start failed after it. Start over on the retry schedule.
    a.gaveUp = true;
    state_ = State::Idle;
    ours_ = false;
  }
  if (state_ == State::Idle) {
    if (wanted_ && allowed_ && !heldOff_ && reached(nowMs, nextStartMs_)) {
      send(a, Cmd::CheckReady, State::Checking, nowMs);
      nextStartMs_ = nowMs + retryMs_;
      retryMs_ = retryMs_ * 3 < kMaxRetryMs ? retryMs_ * 3 : kMaxRetryMs;
    }
  } else if (state_ == State::Started) {
    if (!wanted_ && reached(nowMs, unwantedMs_ + kSuspendAfterMs) && reached(nowMs, nextSuspendMs_)) {
      send(a, Cmd::Suspend, State::Suspending, nowMs);
      nextSuspendMs_ = nowMs + kSuspendRetryMs;
    }
  }
}

StreamControl::Actions StreamControl::linkUp(uint32_t nowMs) {
  Actions a;
  if (linked_) return a;
  linked_ = true;
  state_ = State::Idle;
  audioStarted_ = false;
  ours_ = false;
  heldOff_ = false;
  retryMs_ = kFirstRetryMs;
  nextStartMs_ = nowMs;
  nextSuspendMs_ = nowMs;
  if (wanted_) {
    asked_ = true;
    askedMs_ = nowMs;
  } else {
    unwantedMs_ = nowMs;
  }
  evaluate(a, nowMs);
  return a;
}

void StreamControl::linkDown() {
  linked_ = false;
  state_ = State::Idle;
  audioStarted_ = false;
  ours_ = false;
  heldOff_ = false;
}

StreamControl::Actions StreamControl::setWanted(bool wanted, uint32_t nowMs) {
  Actions a;
  if (wanted && !wanted_) {  // a fresh play
    heldOff_ = false;
    retryMs_ = kFirstRetryMs;
    nextStartMs_ = nowMs;
    asked_ = !audioStarted_;
    askedMs_ = nowMs;
  } else if (!wanted && wanted_) {
    unwantedMs_ = nowMs;
    nextSuspendMs_ = nowMs;
    asked_ = false;
  }
  wanted_ = wanted;
  evaluate(a, nowMs);
  return a;
}

StreamControl::Actions StreamControl::setStartAllowed(bool allowed, uint32_t nowMs) {
  Actions a;
  if (allowed && !allowed_) nextStartMs_ = nowMs;  // no need to wait for the retry
  allowed_ = allowed;
  evaluate(a, nowMs);
  return a;
}

StreamControl::Actions StreamControl::ack(Cmd cmd, bool ok, uint32_t nowMs) {
  Actions a;
  if (!linked_) return a;
  // An answer that doesn't match the outstanding command is stray (e.g. to a
  // command given up on) and changes nothing.
  switch (state_) {
    case State::Checking:
      if (cmd != Cmd::CheckReady) break;
      if (ok && wanted_ && allowed_ && !heldOff_) {
        send(a, Cmd::Start, State::Starting, nowMs);
      } else {
        state_ = audioStarted_ ? State::Started : State::Idle;  // retried on schedule
      }
      break;
    case State::Starting:
      if (cmd != Cmd::Start) break;
      if (ok) {
        // Acknowledged before the stream reported STARTED: our stream. After
        // it: the headphones started one themselves, which ESP-IDF suspends.
        ours_ = !audioStarted_;
        state_ = State::Started;
      } else {
        state_ = audioStarted_ ? State::Started : State::Idle;
      }
      break;
    case State::Suspending:
      if (cmd != Cmd::Suspend) break;
      if (ok) {
        state_ = State::Idle;  // the stack acks once transmission has stopped
        audioStarted_ = false;
        ours_ = false;
      } else {
        state_ = audioStarted_ ? State::Started : State::Idle;  // asked again after kSuspendRetryMs
      }
      break;
    case State::Idle:
    case State::Started:
      break;
  }
  evaluate(a, nowMs);
  return a;
}

StreamControl::Actions StreamControl::audioState(bool started, uint32_t nowMs) {
  Actions a;
  if (!linked_) return a;
  if (started && !audioStarted_) {
    audioStarted_ = true;
    switch (state_) {
      case State::Idle:  // started by the headphones
        state_ = State::Started;
        ours_ = false;
        break;
      case State::Checking:  // started by the headphones ahead of our START's answer
      case State::Starting:
        ours_ = false;
        break;
      case State::Started:
        if (ours_) {
          retryMs_ = kFirstRetryMs;
          if (asked_) {
            a.startedAfterMs = static_cast<int32_t>(nowMs - askedMs_);
            asked_ = false;
          }
        }
        break;
      case State::Suspending:
        break;
    }
  } else if (!started && audioStarted_) {
    audioStarted_ = false;
    if (state_ == State::Started) {
      if (ours_) {
        // The headphones stopped the stream we started: don't fight it.
        heldOff_ = true;
        a.remoteSuspend = true;
      }
      // Not ours (e.g. ESP-IDF suspending one the headphones started): if
      // it's wanted, the retry starts ours.
      state_ = State::Idle;
    }
    // Checking/Starting: our command still gets its answer. Suspending: ours.
    ours_ = false;
  }
  evaluate(a, nowMs);
  return a;
}

StreamControl::Actions StreamControl::sendFailed(uint32_t nowMs) {
  Actions a;
  if (state_ == State::Checking || state_ == State::Starting || state_ == State::Suspending) {
    state_ = audioStarted_ ? State::Started : State::Idle;
  }
  evaluate(a, nowMs);  // the schedule already moved on: no immediate resend
  return a;
}

StreamControl::Actions StreamControl::suspendNow(uint32_t nowMs) {
  Actions a;
  if (!wanted_) {
    unwantedMs_ = nowMs - kSuspendAfterMs;
    nextSuspendMs_ = nowMs;
  }
  evaluate(a, nowMs);
  return a;
}

StreamControl::Actions StreamControl::tick(uint32_t nowMs) {
  Actions a;
  evaluate(a, nowMs);
  return a;
}
