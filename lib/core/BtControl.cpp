#include "BtControl.h"

// Carries out what StreamControl decided. Only this issues media control, so
// there is never more than one command outstanding.
void BtControl::act(const StreamControl::Actions& a, uint32_t nowMs) {
  if (a.gaveUp) io_.commandGaveUp();
  if (a.send != StreamControl::Cmd::None && !io_.mediaCtrl(a.send)) {
    act(stream_.sendFailed(nowMs), nowMs);  // sends nothing before the schedule's next slot
  }
  if (a.remoteSuspend) io_.remoteSuspend();
  if (a.startedAfterMs >= 0) io_.streamStarted(static_cast<uint32_t>(a.startedAfterMs));
}

void BtControl::apply(const AbsVolumePolicy::Actions& a) {
  if (a.gainChanged) {
    io_.setGain(policy_.gainQ15(), a.lift ? GainMove::Lift : a.snap ? GainMove::Snap : GainMove::Ramp);
  }
  if (a.sendAbsolute && avrcUp_) io_.sendAbsoluteVolume(a.absolute);
  if (a.volumeChanged) io_.volumeChanged();
  if (a.modeChanged) io_.volumeModeChanged(policy_.mode(), a.probeUnanswered);
}

// Brings the two into line after any event: media that may flow makes a
// handover on this link a late one (or sends a late probe's command once it
// stops), and a probe (or capabilities still on their way) holds a new
// stream back. Each step can enable the other once.
void BtControl::settle(uint32_t nowMs) {
  for (int i = 0; i < 4; ++i) {
    bool changed = false;
    const bool active = stream_.active();
    if (active != policyActive_) {
      policyActive_ = active;
      apply(policy_.streamActive(active, nowMs));
      changed = true;
    }
    const bool ready = policy_.audioReady(nowMs);
    if (ready != gateOpen_) {
      gateOpen_ = ready;
      act(stream_.setStartAllowed(ready, nowMs), nowMs);
      changed = true;
    }
    if (!changed) break;
  }
  io_.publish();
}

void BtControl::linkUp(uint32_t nowMs) {
  if (stream_.linked()) return;  // a repeated report
  apply(policy_.linkUp(nowMs));  // the safe software level first
  gateOpen_ = policy_.audioReady(nowMs);  // before the stream may start
  act(stream_.setStartAllowed(gateOpen_, nowMs), nowMs);
  act(stream_.linkUp(nowMs), nowMs);
  settle(nowMs);
}

void BtControl::linkDown(uint32_t nowMs) {
  stream_.linkDown();
  policyActive_ = false;
  apply(policy_.linkDown());
  settle(nowMs);
}

void BtControl::avrcpUp(uint32_t nowMs) {
  avrcUp_ = true;
  policy_.avrcpUp(nowMs);
  settle(nowMs);
}

void BtControl::avrcpDown(uint32_t nowMs) {
  avrcUp_ = false;
  apply(policy_.avrcpDown());
  settle(nowMs);
}

void BtControl::capabilities(bool volumeChange, uint32_t nowMs) {
  if (!avrcUp_) {  // they only come over a connected AVRCP
    avrcUp_ = true;
    policy_.avrcpUp(nowMs);
  }
  apply(policy_.capabilities(volumeChange, nowMs));
  settle(nowMs);
}

void BtControl::accepted(uint8_t absolute, uint32_t nowMs) {
  apply(policy_.accepted(absolute, nowMs));
  settle(nowMs);
}

void BtControl::headsetChanged(uint8_t absolute, uint32_t nowMs) {
  apply(policy_.headsetChanged(absolute, nowMs));
  settle(nowMs);
}

void BtControl::setVolume(uint8_t percent, uint32_t nowMs) {
  apply(policy_.userSet(percent, nowMs));
  settle(nowMs);
}

void BtControl::stepVolume(int delta, uint32_t nowMs) {
  int percent = policy_.percent() + delta;
  percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  setVolume(static_cast<uint8_t>(percent), nowMs);
}

void BtControl::setHeadroom(uint16_t q15, uint32_t nowMs) {
  apply(policy_.setHeadroom(q15));
  settle(nowMs);
}

void BtControl::setWanted(bool wanted, uint32_t nowMs) {
  act(stream_.setWanted(wanted, nowMs), nowMs);
  settle(nowMs);
}

void BtControl::suspendNow(uint32_t nowMs) {
  act(stream_.suspendNow(nowMs), nowMs);
  settle(nowMs);
}

void BtControl::mediaAck(StreamControl::Cmd cmd, bool ok, uint32_t nowMs) {
  act(stream_.ack(cmd, ok, nowMs), nowMs);
  settle(nowMs);
}

void BtControl::audioState(bool started, uint32_t nowMs) {
  act(stream_.audioState(started, nowMs), nowMs);
  settle(nowMs);
}

void BtControl::tick(uint32_t nowMs) {
  apply(policy_.tick(nowMs));
  act(stream_.tick(nowMs), nowMs);
  settle(nowMs);
}
