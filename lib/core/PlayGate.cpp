#include "PlayGate.h"

const char* PlayGate::stateName(State s) {
  switch (s) {
    case State::Idle: return "idle";
    case State::Waiting: return "waiting";
    case State::Failed: return "failed";
  }
  return "?";
}

PlayGate::Do PlayGate::update(const In& in) {
  using P = BtLink::Phase;
  if (in.play != PlayState::Waiting) {
    // No wait (any more). A failure's notice lasts until the headphones are
    // back or the speaker is the output: then it has nothing more to say.
    if (state_ == State::Failed && (in.linked || !in.onBluetooth)) state_ = State::Idle;
    if (state_ != State::Waiting) return Do::None;
    // (A release ends it in the pass that releases.) It was cancelled,
    // played on the speaker, or stopped: what it asked for is withdrawn.
    state_ = State::Idle;
    return Do::Ended;
  }
  if (!in.onBluetooth) {
    // The output moved without the listener choosing the speaker for this
    // (silent test mode): paused, never on out loud by itself.
    state_ = State::Idle;
    return Do::Cancel;
  }
  if (in.linked) {
    state_ = State::Idle;
    return Do::Release;
  }
  const bool pairScreen = in.link.phase == P::PairScan || in.link.phase == P::Pairing;
  if (in.nothingToFind && !pairScreen) {
    // None paired and nothing looks for any: no wait (it would only time
    // out). Paused; the radio isn't asked for anything (no scan).
    state_ = State::Idle;
    return Do::NotPaired;
  }
  if (state_ != State::Waiting) {
    // A new wait (or Try again): connect now. The failure checks start on
    // the next pass, once the session has been asked again (its old
    // failure would otherwise end this wait at once). A background page
    // under way isn't only followed: its burst may be at its last try, and
    // this one gets the full burst (BtSink::connect() counts that page as
    // try 1 rather than paging on top of it). The Pair screen's scan or
    // pairing isn't stopped for it: followed.
    state_ = State::Waiting;
    sinceMs_ = in.nowMs;
    return pairScreen ? Do::Track : Do::Connect;
  }
  const bool tooLong = static_cast<int32_t>(in.nowMs - sinceMs_) >= static_cast<int32_t>(kBackstopMs);
  if (in.sessionFailed || tooLong) {
    state_ = State::Failed;
    ++failures_;
    return Do::GiveUp;
  }
  return Do::None;
}

PlayGate::Do PlayGate::step(const In& in, PlaybackController& player, BtSession& session) {
  const Do d = update(in);
  switch (d) {
    case Do::Connect: session.connect(in.nowMs); break;
    case Do::Track:
      if (!session.wanted()) session.connect(in.nowMs);
      break;
    case Do::Release: player.release(); break;
    case Do::GiveUp:
      player.cancelWait();
      session.withdraw(true);
      break;
    case Do::Cancel:
    case Do::NotPaired:
      player.cancelWait();
      session.withdraw(false);
      break;
    case Do::Ended: session.withdraw(false); break;
    case Do::None: break;
  }
  return d;
}
