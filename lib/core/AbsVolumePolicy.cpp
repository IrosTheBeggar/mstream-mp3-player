// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "AbsVolumePolicy.h"

namespace {
uint8_t clampPercent(uint8_t p) { return p > 100 ? 100 : p; }
int absDiff(int a, int b) { return a > b ? a - b : b - a; }
}  // namespace

AbsVolumePolicy::AbsVolumePolicy(uint8_t percent) : percent_(clampPercent(percent)) {
  gain_ = targetGain();
}

uint16_t AbsVolumePolicy::targetGain() const {
  if (ducked_) return 0;
  if (mode_ == Mode::Absolute) {
    // A late handover's rise waits for the answer to what it sent (see
    // sendHeld()): our gain stays where it was.
    if (riseHeld_) return heldGain_ < headroom_ ? heldGain_ : headroom_;
    return headroom_;
  }
  return vol::mulQ15(headroom_, vol::softwareVolumeQ15(percent_));
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

void AbsVolumePolicy::send(Actions& a, uint32_t nowMs) { sendValue(a, vol::percentToAbs(percent_), nowMs); }

void AbsVolumePolicy::sendValue(Actions& a, uint8_t absolute, uint32_t nowMs, bool again) {
  a.sendAbsolute = true;
  a.absolute = absolute;
  lastSent_ = absolute;
  askedAgain_ = again;
  // The oldest leaves the history: with no ACCEPT yet, it may still be on
  // its way, and its ACCEPT will pair with a later command.
  if (sent_[0].valid && sent_[0].accepted < 0) lostUnanswered_ = true;
  for (int i = 0; i + 1 < kSentHistory; ++i) sent_[i] = sent_[i + 1];
  sent_[kSentHistory - 1] = {absolute, -1, nowMs, true, false};
}

// A command that ends a wait (their level back, ours after a late
// confirmation) while one of ours may still be on its way, maybe louder: a
// new stream waits until it is answered, or `waitMs`, so that whatever was
// on its way has landed before anything is heard (see audioReady()); so
// does a rise of our gain that waits for it (riseHeld_). Its answer names no
// command: an ACCEPT paired with it by order counts only if it is near what
// it asked for, an echo if it matches it or a later command (they land in
// order) and every older command of ours has an ACCEPT (else it can't be
// told from a key of theirs near it, see takeEcho()).
void AbsVolumePolicy::sendHeld(Actions& a, uint8_t absolute, uint32_t nowMs, uint32_t waitMs) {
  for (Sent& s : sent_) s.holds = false;
  sendValue(a, absolute, nowMs);
  sent_[kSentHistory - 1].holds = true;
  held_ = true;
  heldMs_ = nowMs;
  heldForMs_ = waitMs;
}

// How long to wait for the answer to a command sent after a late
// confirmation: as long as they took to answer the one they confirmed (the
// probe went unanswered for kProbeTimeoutMs: these headphones are slow),
// from kProbeTimeoutMs up to kHoldMaxMs.
uint32_t AbsVolumePolicy::lateWaitMs(uint32_t nowMs) const {
  const uint32_t took = nowMs - answeredMs_;
  return took < kProbeTimeoutMs ? kProbeTimeoutMs : (took > kHoldMaxMs ? kHoldMaxMs : took);
}

// The held command is answered, or its time is up: a rise of our gain that
// waited for it goes ahead, at the slow rate (from a dip's silence, or from
// the software level).
void AbsVolumePolicy::releaseHold(Actions& a, uint32_t nowMs) {
  if (held_ && nowMs - heldMs_ >= heldForMs_) {  // (the clock wraps)
    held_ = false;
    for (Sent& s : sent_) s.holds = false;
  }
  if (held_ || !riseHeld_) return;
  riseHeld_ = false;
  endDip();
  retarget(a, /*snap=*/false);
}

// Their ACCEPTs answer our commands in order: the oldest one not answered yet.
// Returns what that command asked for, or -1 if none waits for an answer (a
// stale ACCEPT: more of them than commands we know of).
int AbsVolumePolicy::noteAccepted(uint8_t absolute) {
  for (Sent& s : sent_) {
    if (s.valid && s.accepted < 0) {
      s.accepted = absolute;
      answeredMs_ = s.ms;
      if (s.holds) {
        // Its own answer: what was on its way before it has landed. Not
        // near it: the stack paired another's with it (a command they
        // dropped, a stale answer); the stream waits out the timeout.
        s.holds = false;
        if (absDiff(absolute, s.absolute) <= kEchoTolerance) held_ = false;
      }
      return s.absolute;
    }
  }
  return -1;
}

// An echo matches a command of ours they haven't confirmed by a notification
// yet. It (and anything older) is used up; anything else is one of their own
// changes, after which every notification shows their real level. Unless
// `keepOnMiss`: while a probe waits, a command of ours may still be on its
// way, and its echo (and its ACCEPT, paired by order) must still be
// recognised when it lands.
AbsVolumePolicy::Heard AbsVolumePolicy::takeEcho(uint8_t absolute, uint32_t nowMs, bool keepOnMiss) {
  for (int i = kSentHistory - 1; i >= 0; --i) {
    const Sent& s = sent_[i];
    if (!s.valid || nowMs - s.ms > kEchoWindowMs) continue;
    if (absDiff(absolute, s.absolute) <= kEchoTolerance || (s.accepted >= 0 && absDiff(absolute, s.accepted) <= 1)) {
      // While a command is held, a command older than it with no ACCEPT yet
      // may not have landed, and a key of theirs near it, or near the held
      // one, looks the same as an echo (headphones with fine steps). Not an
      // answer, and nothing is used up: their ACCEPTs still pair with our
      // commands in order, that command still counts as unanswered, its own
      // echo is still recognised, and the hold waits for an answer (see
      // unansweredBeforeHeld()). Nor does the UI follow it: echo or key, the
      // held command (or a later one) lands after it, and they end there,
      // at the level the UI shows. (Followed, the probe's echo arriving
      // before its ACCEPT left the UI at the probe's level once their level
      // back had landed and its echo released the hold.)
      if (held_ && unansweredBeforeHeld(i)) return Heard::Held;
      for (int j = 0; j <= i; ++j) {
        if (sent_[j].valid && sent_[j].holds) held_ = false;  // it, or a later one, landed
      }
      dropSent(i);
      answeredMs_ = s.ms;
      return Heard::Echo;
    }
  }
  if (!keepOnMiss) dropSent(kSentHistory - 1);
  return Heard::Theirs;
}

// While a command is held: using up sent_[0..i] would take a notification
// as the answer to a command older than it that has no ACCEPT yet, or, if
// the held command is among them, release the hold while a command of ours
// that left the history with no ACCEPT (lostUnanswered_) may still be on its
// way. A notification names no command: only an ACCEPT, paired by order,
// shows that one of ours has landed. (Nothing left before it: what remains
// was sent after it, and using that up can't release it.)
bool AbsVolumePolicy::unansweredBeforeHeld(int i) const {
  int held = -1;
  for (int j = 0; j < kSentHistory; ++j) {
    if (sent_[j].valid && sent_[j].holds) held = j;
  }
  if (held < 0) return false;
  for (int j = 0; j <= i && j < held; ++j) {
    if (sent_[j].valid && sent_[j].accepted < 0) return true;
  }
  return i >= held && lostUnanswered_;
}

// What isn't an answer (an ACCEPT or echo above what we ask for, a louder
// notification) is answered with what we ask for again, but not in reply to
// the answer to that very repeat: headphones that can't set our level (they
// round up, or have a floor) would otherwise trade commands and ACCEPTs with
// us for the rest of the link. A new level of theirs (a key) may be answered
// again. And only to bring them down: if what we ask for now is no lower
// than what they just reported (an ACCEPT for an older, lower command of
// ours, with the user's rise since), it would step them up. Returns whether
// it was sent.
bool AbsVolumePolicy::askAgain(Actions& a, uint32_t nowMs, bool newLevel) {
  if (askedAgain_ && !newLevel) return false;
  const uint8_t level = askedLevel();
  if (headsetAbs_ >= 0 && level >= headsetAbs_) return false;
  if (mode_ == Mode::Probing) probeStartMs_ = nowMs;  // its own timeout, within the deadline
  sendValue(a, level, nowMs, /*again=*/true);
  return true;
}

// What we ask them for now: the volume shown, but no more than we last sent
// (Software after an unanswered probe leaves the user's changes to our gain,
// a rise included).
uint8_t AbsVolumePolicy::askedLevel() const {
  const uint8_t ours = vol::percentToAbs(percent_);
  return lastSent_ >= 0 && lastSent_ < ours ? static_cast<uint8_t>(lastSent_) : ours;
}

// Uses up sent_[0..i]. One with no ACCEPT yet may still be on its way (the
// notification that used it up may have been a key of theirs near it), and
// its ACCEPT will pair with a later command: see unansweredBeforeHeld().
void AbsVolumePolicy::dropSent(int i) {
  for (int j = 0; j <= i; ++j) {
    if (sent_[j].valid && sent_[j].accepted < 0) lostUnanswered_ = true;
    sent_[j].valid = false;
  }
}

// A new link or AVRCP connection: nothing of ours is on its way over it.
void AbsVolumePolicy::clearSent() {
  for (Sent& s : sent_) s.valid = false;
  lostUnanswered_ = false;
}

// The probe steps the headphones' own volume, from a level we can't know, at
// once. So it only goes out before anything has been heard on this link, or
// while what they play from us is silent (a late probe dips first).
void AbsVolumePolicy::maybeProbe(Actions& a, uint32_t nowMs) {
  if (!linked_ || !capable_ || probed_ || mode_ != Mode::Software) return;
  probed_ = true;
  setMode(a, Mode::Probing);
  if (canHandOver()) {
    sendProbe(a, nowMs);  // gain unchanged: still attenuating
    return;
  }
  ducked_ = true;
  duckMs_ = nowMs;
  if (percent_ > kMaxLinkUpPercent) setPercent(a, kMaxLinkUpPercent);  // as on a new link
  // Down to silence: faded while media may flow, at once while none does.
  retarget(a, /*snap=*/!streaming_);
  sendPending_ = true;
  sendIfSilent(a, nowMs);  // at once after a pause; else tick() sends it
}

// What the headphones play from us is silent: the dip has had kDuckSettleMs
// to pass ESP-IDF's frame queue and their buffer, or nothing has flowed for
// that long (what they still had from before a stop has played out).
bool AbsVolumePolicy::silentAtHeadphones(uint32_t nowMs) const {
  return nowMs - duckMs_ >= kDuckSettleMs || (!streaming_ && nowMs - stopMs_ >= kDuckSettleMs);
}

void AbsVolumePolicy::sendIfSilent(Actions& a, uint32_t nowMs) {
  if (sendPending_ && silentAtHeadphones(nowMs)) sendProbe(a, nowMs);
}

void AbsVolumePolicy::sendProbe(Actions& a, uint32_t nowMs) {
  sendPending_ = false;
  probeStartMs_ = nowMs;
  probeFirstMs_ = nowMs;
  // A late probe: the silence is snapped as well. If no audio data flowed
  // since the dip (a START out, or a stream the stack hasn't started), the
  // gain stage never saw that target, and the answer's would replace it: a
  // resumed stream would then fade back quickly to the level heard before,
  // at the headphones' new level. A snap is kept until it is applied (a
  // no-op once the fade has reached 0).
  if (ducked_) retarget(a, /*snap=*/true);
  send(a, nowMs);
}

// Only while nothing has been heard on this link (canHandOver()): the level
// the first stream fades in to is not a rise over anything the listener heard.
void AbsVolumePolicy::handOver(Actions& a) {
  awaitingLate_ = false;
  setMode(a, Mode::Absolute);
  gain_ = targetGain();
  a.gainChanged = true;
  a.snap = false;
  a.lift = true;
}

// After audio has flowed: the headphones apply our volume (or less) now, and
// our gain rises to the headroom at the gain stage's own slow rate, from
// silence after a dip or from the software level.
void AbsVolumePolicy::handOverPlaying(Actions& a) {
  awaitingLate_ = false;
  endDip();
  setMode(a, Mode::Absolute);
  retarget(a, /*snap=*/false);
}

// An ACCEPT or an echo of a command of ours after the probe timed out (or an
// echo during one, once a stream the headphones started has flowed): they
// apply what we send. Nothing heard yet: as if in time, and ours goes out if
// it isn't what they have (the user's changes since were not sent); a new
// stream waits for its answer, so that it doesn't land during the first
// one, a rise heard as a step (sendHeld()). Otherwise they end at the last
// command we sent (the probe, or ours again, no louder): the volume shown
// comes down to it if the user rose since (that rise was only our gain's),
// and only a level below what they confirmed is sent (a step down on the
// Core2 since, left to our gain). Our gain rises slowly from the software
// level to the headroom, but only once that is answered: until it lands
// they are at the level they confirmed, above the user's, and our gain is
// what keeps it down. These headphones are slow (the probe went unanswered
// in time), so either wait lasts as long as they took to confirm.
void AbsVolumePolicy::confirmLate(Actions& a, uint8_t absolute, uint32_t nowMs) {
  if (canHandOver()) {
    handOver(a);
    const uint8_t ours = vol::percentToAbs(percent_);
    if (absolute != ours) sendHeld(a, ours, nowMs, lateWaitMs(nowMs));
    return;
  }
  awaitingLate_ = false;
  setMode(a, Mode::Absolute);
  if (lastSent_ >= 0 && vol::percentToAbs(percent_) > lastSent_) {
    setPercent(a, vol::absToPercent(static_cast<uint8_t>(lastSent_)));
  }
  const uint8_t ours = vol::percentToAbs(percent_);
  if (ours < absolute) {
    riseHeld_ = true;
    heldGain_ = gain_;  // the software level they were heard at
    sendHeld(a, ours, nowMs, lateWaitMs(nowMs));
  }
  retarget(a, /*snap=*/false);  // up to the headroom, unless held
}

void AbsVolumePolicy::endDip() {
  ducked_ = false;
  sendPending_ = false;
}

AbsVolumePolicy::Actions AbsVolumePolicy::linkUp(uint32_t nowMs) {
  Actions a;
  if (linked_) return a;  // a repeated report must not snap or re-probe
  linked_ = true;
  linkUpMs_ = nowMs;
  heard_ = false;
  streaming_ = false;
  probed_ = false;
  awaitingLate_ = false;
  held_ = false;
  riseHeld_ = false;
  endDip();
  headsetAbs_ = -1;
  lastSent_ = -1;
  askedAgain_ = false;
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
  awaitingLate_ = false;
  held_ = false;
  riseHeld_ = false;
  endDip();
  streaming_ = false;
  heard_ = false;
  headsetAbs_ = -1;
  lastSent_ = -1;
  askedAgain_ = false;
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
  if (streaming_ && !active) stopMs_ = nowMs;
  streaming_ = active;
  if (active && linked_) heard_ = true;
  sendIfSilent(a, nowMs);
  maybeProbe(a, nowMs);
  return a;
}

bool AbsVolumePolicy::audioReady(uint32_t nowMs) const {
  if (!linked_) return true;
  if (mode_ == Mode::Probing) return false;
  // A command that ended a wait while one of ours may still be on its way:
  // until it is answered, or its timeout (see sendHeld()).
  if (held_ && nowMs - heldMs_ < heldForMs_) return false;
  // A probe that went unanswered before anything was heard: a little longer,
  // in case they apply it late (see kProbeGraceMs).
  if (awaitingLate_ && !heard_ && nowMs - unansweredMs_ < kProbeGraceMs) return false;
  if (heard_ || capsKnown_) return true;
  if (nowMs - linkUpMs_ < kCapsWaitMs) return false;
  // AVRCP is up and its capabilities are on their way: worth a little longer,
  // since on a link that has played the handover takes a dip to silence.
  return !(avrcUp_ && nowMs - avrcUpMs_ < kCapsReplyWaitMs);
}

AbsVolumePolicy::Actions AbsVolumePolicy::avrcpDown() {
  Actions a;
  avrcUp_ = false;
  capsKnown_ = false;
  capable_ = false;
  probed_ = false;  // a new AVRCP connection gets its own probe
  awaitingLate_ = false;
  held_ = false;  // what was on its way went with it
  riseHeld_ = false;
  endDip();
  headsetAbs_ = -1;
  lastSent_ = -1;
  askedAgain_ = false;
  clearSent();
  if (mode_ != Mode::Software) {
    setMode(a, Mode::Software);
    retarget(a, /*snap=*/false);  // down quickly, or from a dip slowly up
  }
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::accepted(uint8_t absolute, uint32_t nowMs) {
  Actions a;
  absolute &= 0x7F;
  headsetAbs_ = absolute;
  // Only an answer to something sent on this link counts (not a stale one).
  if (!linked_ || !probed_) return a;
  const int asked = noteAccepted(absolute);
  // It confirms only if they set no more than the command it answers asked
  // for: else a stale answer (to a command from before this probe), or a
  // level of their own. (An answer to an older command of ours, with a lower
  // one still on its way, does confirm: they apply ours, and the lower one,
  // sent again if need be, lands after it.)
  const bool confirms = asked >= 0 && absolute <= asked + kEchoTolerance;
  switch (mode_) {
    case Mode::Absolute:
      break;  // a step of ours, applied; the UI already shows it
    case Mode::Probing:
      if (sendPending_ || asked < 0) break;  // not an answer to anything of ours out there
      if (!confirms) {
        // Not taken as the answer: ask again (the cap holds), with a new
        // timeout, still within the probe's deadline. Not a second time
        // for the same command: they can't go lower, the probe runs out.
        askAgain(a, nowMs, /*newLevel=*/false);
        break;
      }
      // The UI keeps what the user chose. Media may have flowed since a
      // probe went out before it (a stream the headphones started): they
      // apply ours, so our gain only has to rise, slowly.
      if (canHandOver()) {
        handOver(a);
      } else {
        handOverPlaying(a);
      }
      break;
    case Mode::Software:
      // An answer after the probe timed out: they apply what we sent.
      if (!awaitingLate_ || asked < 0) break;
      if (!confirms) {
        askAgain(a, nowMs, /*newLevel=*/false);  // ours again (at most what we last sent)
        break;
      }
      confirmLate(a, absolute, nowMs);
      break;
  }
  releaseHold(a, nowMs);
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::headsetChanged(uint8_t absolute, uint32_t nowMs) {
  Actions a;
  absolute &= 0x7F;
  const bool newLevel = absolute != headsetAbs_;
  headsetAbs_ = absolute;
  if (!linked_) return a;
  const bool waiting = mode_ == Mode::Probing || (mode_ == Mode::Software && awaitingLate_);
  // Nothing heard yet, or silence during a late probe's dip: they render the
  // volume themselves, no louder than we ask for.
  const bool asking = mode_ != Mode::Absolute && (ducked_ || (waiting && canHandOver()));
  const Heard heard = takeEcho(absolute, nowMs, /*keepOnMiss=*/asking || waiting);
  const bool echo = heard == Heard::Echo;
  // An echo confirms only if no louder than what we ask for now: one of an
  // older command (a lower one still on its way), or a key of theirs that
  // happens to land near one, isn't proof they are at or below ours.
  const bool confirms = echo && absolute <= askedLevel() + kEchoTolerance;
  if (mode_ == Mode::Absolute) {
    // Their buttons: the UI follows. (Not while held near a command of ours
    // still on its way: they end at the held one, see takeEcho().)
    if (heard == Heard::Theirs) setPercent(a, vol::absToPercent(absolute));
  } else if (asking) {
    if (confirms && mode_ == Mode::Software) {
      confirmLate(a, absolute, nowMs);
    } else if (confirms || (!echo && absolute <= vol::percentToAbs(percent_))) {
      // A command of ours may still be on its way (their key, or their
      // answer to the library's registration, crossed it): landing after
      // this, it would step them up to ours. Their own level goes after it,
      // so the last command they get is theirs, and a new stream waits for
      // its answer: ours lands before anything is heard. During a dip our
      // gain waits for it too, at silence: ours lands before it rises.
      const bool sendBack = !echo && !sendPending_;
      if (ducked_ && sendBack) {
        awaitingLate_ = false;
        setMode(a, Mode::Absolute);
        riseHeld_ = true;  // still ducked: releaseHold() ends the dip
      } else if (ducked_) {
        handOverPlaying(a);  // up from silence, slowly; a command still pending is dropped
      } else {
        handOver(a);
      }
      if (!echo) {
        setPercent(a, vol::absToPercent(absolute));
        if (sendBack) sendHeld(a, absolute, nowMs);
      }
    } else if (!sendPending_) {
      // Louder than we asked for: ask again (the link-up cap holds; after an
      // unanswered probe, no more than we last sent). A late probe's command
      // still pending does that anyway.
      askAgain(a, nowMs, /*newLevel=*/!echo && newLevel);
    }
  } else if (confirms && waiting) {
    // Media has flowed and they confirm a command of ours: they apply our
    // volume. Our gain rises slowly from the software level to the headroom
    // (once a step down it sends is answered, see confirmLate()).
    confirmLate(a, absolute, nowMs);
  } else if (waiting) {
    // Media has flowed at a level that includes theirs, unknown until now:
    // neither our gain nor the UI volume follows it. The listener set it on
    // the headphones: stop sending ours. A louder command of ours may still
    // be on its way (the probe, when a stream they started themselves has
    // flowed meanwhile, or a late one): landing after this, it would step
    // them up while audio plays, and stay. Their own level goes after it
    // (or the volume shown, if lower: a step down on the Core2 since was
    // our gain's alone), so the last command they get is no louder than
    // what they have, and a new stream waits for its answer.
    if (lastSent_ > absolute) {
      const uint8_t ours = vol::percentToAbs(percent_);
      sendHeld(a, absolute < ours ? absolute : ours, nowMs);
    }
    awaitingLate_ = false;
    setMode(a, Mode::Software);
  }
  releaseHold(a, nowMs);
  return a;  // a change of theirs is only ever sent back as itself (or less), once (above)
}

AbsVolumePolicy::Actions AbsVolumePolicy::userSet(uint8_t percent, uint32_t nowMs) {
  Actions a;
  percent = clampPercent(percent);
  const bool changed = percent != percent_;
  percent_ = percent;
  switch (mode_) {
    case Mode::Software:
      // Our gain alone, also after an unanswered probe: their level is
      // unknown (they may have kept their own, rejected ours, or apply
      // absolute volume only while streaming), and whatever we send could
      // be the first command they apply, at once, from their own level:
      // louder than asked even for a step down. A late answer to what was
      // sent still hands over (confirmLate()).
      retarget(a, /*snap=*/false);
      break;
    case Mode::Probing:
      retarget(a, /*snap=*/false);  // none during a dip: silence until they answer
      // A late probe's command that hasn't gone out yet carries the new value.
      if (changed && !sendPending_) {
        probeStartMs_ = nowMs;  // its own time to be answered, within the probe's deadline
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
  releaseHold(a, nowMs);
  sendIfSilent(a, nowMs);
  const uint32_t timeout = ducked_ ? kLateProbeTimeoutMs : kProbeTimeoutMs;
  if (mode_ == Mode::Probing && !sendPending_ &&
      (nowMs - probeStartMs_ >= timeout || nowMs - probeFirstMs_ >= kProbeDeadlineFactor * timeout)) {
    setMode(a, Mode::Software);
    awaitingLate_ = true;
    unansweredMs_ = nowMs;
    a.probeUnanswered = true;
    if (ducked_) {
      endDip();
      retarget(a, /*snap=*/false);  // slowly up from silence to the software level
    }  // otherwise the gain is at the software level already
  }
  return a;
}

AbsVolumePolicy::Actions AbsVolumePolicy::setHeadroom(uint16_t q15) {
  Actions a;
  headroom_ = q15 > vol::kUnityQ15 ? vol::kUnityQ15 : q15;
  retarget(a, /*snap=*/false);  // quickly down, slowly up; none during a dip
  return a;
}
