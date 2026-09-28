#include "IdlePolicy.h"

#include <cstdio>

namespace {
constexpr uint32_t kChoiceMs[IdlePolicy::kChoices] = {10u * 60000u, 20u * 60000u, 60u * 60000u, 0};
constexpr const char* kChoiceLabel[IdlePolicy::kChoices] = {"10 min", "20 min", "60 min", "Never"};

int clampChoice(int c) { return c < 0 || c >= IdlePolicy::kChoices ? IdlePolicy::kDefaultChoice : c; }
}  // namespace

uint32_t IdlePolicy::choiceMs(int choice) { return kChoiceMs[clampChoice(choice)]; }

const char* IdlePolicy::choiceLabel(int choice) { return kChoiceLabel[clampChoice(choice)]; }

void IdlePolicy::begin(int choice, uint32_t nowMs) {
  choice_ = clampChoice(choice);
  testMs_ = 0;
  phase_ = Phase::Blocked;
  blocker_ = Blocker::None;
  sinceMs_ = nowMs;
}

void IdlePolicy::restart(uint32_t nowMs) {
  // Re-evaluated at the next update(): Counting from then if nothing blocks.
  // (A warning up ends there: its phase is gone.)
  if (phase_ != Phase::Off) phase_ = Phase::Blocked;
  sinceMs_ = nowMs;
}

void IdlePolicy::setChoice(int choice, uint32_t nowMs) {
  choice_ = clampChoice(choice);
  restart(nowMs);
}

void IdlePolicy::setTestMs(uint32_t ms, uint32_t nowMs) {
  testMs_ = ms;
  restart(nowMs);
}

uint32_t IdlePolicy::lengthMs() const { return testMs_ ? testMs_ : choiceMs(choice_); }

IdlePolicy::Blocker IdlePolicy::blockerOf(const In& in) const {
  if (lengthMs() == 0) return Blocker::Never;
  if (in.play == PlayState::Playing) return Blocker::Playing;
  if (in.play == PlayState::Waiting) return Blocker::Waiting;
  if (in.usb) return Blocker::Usb;
  if (in.pairing) return Blocker::Pairing;
  if (in.queueWrite) return Blocker::QueueWrite;
  if (in.busy) return Blocker::Busy;
  return Blocker::None;
}

IdlePolicy::Out IdlePolicy::update(const In& in) {
  Out o;
  const uint32_t now = in.nowMs;
  blocker_ = blockerOf(in);
  switch (phase_) {
    case Phase::Off:
      return o;  // the power is going: nothing more
    case Phase::Releasing:
      if (blocker_ != Blocker::None || in.input) {
        o.cancelled = true;
        phase_ = blocker_ != Blocker::None ? Phase::Blocked : Phase::Counting;
        sinceMs_ = now;
        return o;
      }
      if (!in.linked || now - releaseMs_ >= kReleaseWaitMs) {
        phase_ = Phase::Off;
        o.powerOff = true;
      }
      return o;
    default:
      break;
  }
  const bool warning = phase_ == Phase::Warning;
  if (blocker_ != Blocker::None) {
    o.warnEnd = warning;
    phase_ = Phase::Blocked;
    sinceMs_ = now;
    return o;
  }
  if (phase_ == Phase::Blocked || in.input) {
    // Idle from now (it was blocked until now, or someone is there).
    o.warnEnd = warning;
    phase_ = Phase::Counting;
    sinceMs_ = now;
  }
  const uint32_t len = lengthMs();
  const uint32_t idle = now - sinceMs_;
  if (idle >= len) {
    // (A warning up goes with the power; the screen goes dark with it.)
    phase_ = Phase::Releasing;
    releaseMs_ = now;
    o.shutdown = true;
    return o;
  }
  if (phase_ == Phase::Counting && len - idle <= kWarnMs) {
    phase_ = Phase::Warning;
    o.warn = true;
    o.warnEnd = false;
  }
  return o;
}

void IdlePolicy::cancel(uint32_t nowMs) {
  phase_ = Phase::Blocked;
  sinceMs_ = nowMs;
}

uint32_t IdlePolicy::msLeft(uint32_t nowMs) const {
  if (phase_ != Phase::Counting && phase_ != Phase::Warning) return 0;
  const uint32_t len = lengthMs(), idle = nowMs - sinceMs_;
  return idle >= len ? 0 : len - idle;
}

uint32_t IdlePolicy::warnSeconds(uint32_t nowMs) const {
  if (phase_ != Phase::Warning) return 0;
  const uint32_t left = msLeft(nowMs);
  const uint32_t s = (left + 999) / 1000;
  return s < 1 ? 1 : s > kWarnMs / 1000 ? kWarnMs / 1000 : s;
}

const char* IdlePolicy::phaseName(Phase p) {
  switch (p) {
    case Phase::Blocked: return "blocked";
    case Phase::Counting: return "counting";
    case Phase::Warning: return "warning";
    case Phase::Releasing: return "releasing";
    case Phase::Off: return "off";
  }
  return "?";
}

const char* IdlePolicy::blockerName(Blocker b) {
  switch (b) {
    case Blocker::None: return "nothing";
    case Blocker::Never: return "set to Never";
    case Blocker::Playing: return "playing";
    case Blocker::Waiting: return "a play waits for the headphones";
    case Blocker::Usb: return "on USB power";
    case Blocker::Pairing: return "pairing";
    case Blocker::QueueWrite: return "the queue is being saved";
    case Blocker::Busy: return "a screen of its own is up";
  }
  return "?";
}

void IdlePolicy::warnText(uint32_t seconds, char* buf, size_t size) {
  snprintf(buf, size, "Turning off in %lu s", static_cast<unsigned long>(seconds));
}

void IdlePolicy::offText(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = (ms + 500) / 1000;
  if (s < 60) {
    snprintf(buf, size, "Turned off after %lu s idle", static_cast<unsigned long>(s));
    return;
  }
  const uint32_t min = (s + 30) / 60;
  snprintf(buf, size, "Turned off after %lu minute%s idle", static_cast<unsigned long>(min), min == 1 ? "" : "s");
}
