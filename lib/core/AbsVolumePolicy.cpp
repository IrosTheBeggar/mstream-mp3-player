#include "AbsVolumePolicy.h"

namespace {
uint8_t clampPercent(uint8_t p) { return p > 100 ? 100 : p; }
int absDiff(int a, int b) { return a > b ? a - b : b - a; }
}  // namespace

AbsVolumePolicy::AbsVolumePolicy(uint8_t percent) : percent_(clampPercent(percent)) {
  gain_ = targetGain();
}

uint16_t AbsVolumePolicy::targetGain() const {
  if (mode_ == Mode::Absolute) return vol::kHeadroomQ15;
  return vol::mulQ15(vol::kHeadroomQ15, vol::softwareVolumeQ15(percent_));
}

void AbsVolumePolicy::retarget(Actions& a, bool snap) {
  const uint16_t t = targetGain();
  if (!snap && t == gain_) return;
  gain_ = t;
  a.gainChanged = true;
  a.snap = snap;
  a.lift = false;
}

void AbsVolumePolicy::setMode(Actions& a, Mode m) {
  if (mode_ == m) return;
  mode_ = m;
  a.modeChanged = true;
}

void AbsVolumePolicy::setPercent(Actions& a, uint8_t percent) {
  if (percent == percent_) return;
  percent_ = percent;
  a.volumeChanged = true;
}

void AbsVolumePolicy::send(Actions& a, uint32_t nowMs) {
  const uint8_t absolute = vol::percentToAbs(percent_);
  a.sendAbsolute = true;
  a.absolute = absolute;
  for (int i = 0; i + 1 < kSentHistory; ++i) sent_[i] = sent_[i + 1];
  sent_[kSentHistory - 1] = {absolute, -1, nowMs, true};
}

// Their ACCEPTs answer our commands in order: the oldest one not answered yet.
void AbsVolumePolicy::noteAccepted(uint8_t absolute) {
  for (Sent& s : sent_) {
    if (s.valid && s.accepted < 0) {
      s.accepted = absolute;
      return;
    }
  }
}

// An echo matches a command of ours they haven't confirmed by a notification
// yet. It (and anything older) is used up; anything else is one of their own
// changes, after which every notification shows their real level.
bool AbsVolumePolicy::takeEcho(uint8_t absolute, uint32_t nowMs) {
  for (int i = kSentHistory - 1; i >= 0; --i) {
    const Sent& s = sent_[i];
    if (!s.valid || nowMs - s.ms > kEchoWindowMs) continue;
    if (absDiff(absolute, s.absolute) <= kEchoTolerance || (s.accepted >= 0 && absDiff(absolute, s.accepted) <= 1)) {
      for (int j = 0; j <= i; ++j) sent_[j].valid = false;
      return true;
    }
  }
  clearSent();
  return false;
}

void AbsVolumePolicy::clearSent() {
  for (Sent& s : sent_) s.valid = false;
}

void AbsVolumePolicy::maybeProbe(Actions& a, uint32_t nowMs) {
  // Only before anything has been heard on this link: the probe steps the
  // headphones' own volume, from a level we can't know.
  if (!linked_ || !capable_ || probed_ || !canHandOver() || mode_ != Mode::Software) return;
  probed_ = true;
  probeStartMs_ = nowMs;
  setMode(a, Mode::Probing);  // gain unchanged: still attenuating
  send(a, nowMs);
}

// Only while nothing has been heard on this link (canHandOver()): the level
// the first stream fades in to is not a rise over anything the listener heard.
void AbsVolumePolicy::handOver(Actions& a) {
  keepSending_ = false;
  setMode(a, Mode::Absolute);
  gain_ = targetGain();
  a.gainChanged = true;
  a.snap = false;
  a.lift = true;
}

AbsVolumePolicy::Actions AbsVolumePolicy::linkUp(uint32_t nowMs) {
  Actions a;
  if (linked_) return a;  // a repeated report must not snap or re-probe
  linked_ = true;
  linkUpMs_ = nowMs;
  heard_ = false;
  streaming_ = false;
  probed_ = false;
  keepSending_ = false;
  headsetAbs_ = -1;
  clearSent();
  if (percent_ > kMaxLinkUpPercent) {
    percent_ = kMaxLinkUpPercent;
    a.volumeChanged = true;
  }
  setMode(a, Mode::Software);
  retarget(a, /*snap=*/true);
  maybeProbe(a, nowMs);  // capabilities may have come first (or AVRCP stayed up)
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::linkDown() {
  Actions a;
  linked_ = false;
  // capable_ and capsKnown_ stay: AVRCP may outlive the A2DP link, and then
  // no new capabilities arrive for the next one.
  probed_ = false;
  keepSending_ = false;
  streaming_ = false;
  heard_ = false;
  headsetAbs_ = -1;
  clearSent();
  setMode(a, Mode::Software);
  retarget(a, /*snap=*/true);  // no audio flows now; be at the safe level for the next link
  return a;
}

void AbsVolumePolicy::avrcpUp(uint32_t nowMs) {
  if (avrcUp_) return;
  avrcUp_ = true;
  avrcUpMs_ = nowMs;
}

AbsVolumePolicy::Actions AbsVolumePolicy::capabilities(bool volumeChange, uint32_t nowMs) {
  Actions a;
  capsKnown_ = true;
  capable_ = volumeChange;
  maybeProbe(a, nowMs);
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::streamActive(bool active, uint32_t nowMs) {
  Actions a;
  streaming_ = active;
  if (active && linked_) heard_ = true;
  maybeProbe(a, nowMs);
  return a;
}

bool AbsVolumePolicy::audioReady(uint32_t nowMs) const {
  if (!linked_) return true;
  if (mode_ == Mode::Probing) return false;
  if (heard_ || capsKnown_) return true;
  if (nowMs - linkUpMs_ < kCapsWaitMs) return false;
  // AVRCP is up and its capabilities are on their way: worth a little longer,
  // since a link that has played can't hand the volume over any more.
  return !(avrcUp_ && nowMs - avrcUpMs_ < kCapsReplyWaitMs);
}

AbsVolumePolicy::Actions AbsVolumePolicy::avrcpDown() {
  Actions a;
  avrcUp_ = false;
  capsKnown_ = false;
  capable_ = false;
  probed_ = false;  // a new AVRCP connection gets its own probe, if nothing was heard yet
  keepSending_ = false;
  headsetAbs_ = -1;
  clearSent();
  if (mode_ != Mode::Software) {
    setMode(a, Mode::Software);
    retarget(a, /*snap=*/false);  // down, quickly
  }
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::accepted(uint8_t absolute, uint32_t nowMs) {
  Actions a;
  absolute &= 0x7F;
  headsetAbs_ = absolute;
  // Only an answer to something sent on this link counts (not a stale one).
  if (!linked_ || !probed_) return a;
  noteAccepted(absolute);
  switch (mode_) {
    case Mode::Absolute:
      break;  // a step of ours, applied; the UI already shows it
    case Mode::Probing:
      if (canHandOver()) {
        handOver(a);  // the UI keeps what the user chose
      } else {
        // Capable, but media has flowed since the probe went out (a stream
        // the headphones started): not on this link. They follow our
        // commands, so keep sending them.
        setMode(a, Mode::Software);
        keepSending_ = true;
      }
      break;
    case Mode::Software:
      // An answer after the probe timed out. Nothing heard yet: as if in
      // time. Otherwise it changes nothing: our gain never rises on a link
      // that has played.
      if (keepSending_ && canHandOver()) {
        handOver(a);
        // Later commands may still be on their way; make sure they end on ours.
        if (absolute != vol::percentToAbs(percent_)) send(a, nowMs);
      }
      break;
  }
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::headsetChanged(uint8_t absolute, uint32_t nowMs) {
  Actions a;
  absolute &= 0x7F;
  headsetAbs_ = absolute;
  if (!linked_) return a;
  const bool echo = takeEcho(absolute, nowMs);
  const bool waiting = mode_ == Mode::Probing || (mode_ == Mode::Software && keepSending_);
  if (mode_ == Mode::Absolute) {
    if (!echo) setPercent(a, vol::absToPercent(absolute));  // their buttons: the UI follows
  } else if (waiting && canHandOver()) {
    // Nothing heard yet: they render the volume themselves.
    if (echo || absolute <= vol::percentToAbs(percent_)) {
      handOver(a);
      if (!echo) setPercent(a, vol::absToPercent(absolute));
    } else {
      // Louder than we asked for: ask again (the link-up cap holds).
      if (mode_ == Mode::Probing) probeStartMs_ = nowMs;
      send(a, nowMs);
    }
  } else {
    // Media has flowed at a level that includes theirs, unknown until now:
    // neither our gain nor the UI volume follows it. The listener set it on
    // the headphones; if it isn't ours, stop sending ours.
    if (!echo) keepSending_ = false;
    if (mode_ == Mode::Probing) {
      setMode(a, Mode::Software);
      keepSending_ = echo;
    }
  }
  return a;  // never sends a change of theirs back
}

AbsVolumePolicy::Actions AbsVolumePolicy::userSet(uint8_t percent, uint32_t nowMs) {
  Actions a;
  percent = clampPercent(percent);
  const bool changed = percent != percent_;
  percent_ = percent;
  switch (mode_) {
    case Mode::Software:
      retarget(a, /*snap=*/false);
      // After an unanswered probe: they may apply it without saying so.
      // Stacked with our gain that is quieter, never louder, than asked.
      if (keepSending_ && changed) send(a, nowMs);
      break;
    case Mode::Probing:
      retarget(a, /*snap=*/false);
      if (changed) {
        probeStartMs_ = nowMs;  // give the new command its own time to be answered
        send(a, nowMs);
      }
      break;
    case Mode::Absolute:
      send(a, nowMs);  // even unchanged: re-syncs headphones that drifted
      break;
  }
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::tick(uint32_t nowMs) {
  Actions a;
  if (mode_ == Mode::Probing && nowMs - probeStartMs_ >= kProbeTimeoutMs) {
    setMode(a, Mode::Software);  // gain is already the software level
    keepSending_ = true;
    a.probeUnanswered = true;
  }
  return a;
}
