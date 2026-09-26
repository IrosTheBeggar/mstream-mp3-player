#include "ScrollGovernor.h"

ScrollGovernor::Level ScrollGovernor::fromRing(uint32_t ringMs, uint32_t margin) const {
  if (ringMs < config_.pauseBelowMs + margin) return Level::Paused;
  if (ringMs < config_.wholeRowsBelowMs + margin) return Level::WholeRows;
  if (ringMs < config_.reducedBelowMs + margin) return Level::Reduced;
  return Level::Normal;
}

ScrollGovernor::Budget ScrollGovernor::budget() const {
  Budget b;
  b.level = level_;
  switch (level_) {
    case Level::Normal: b.frameMs = config_.normalFrameMs; break;
    case Level::Reduced: b.frameMs = config_.reducedFrameMs; break;
    case Level::WholeRows:
      b.frameMs = config_.wholeRowFrameMs;
      b.wholeRows = true;
      break;
    case Level::Paused:
      b.frameMs = config_.wholeRowFrameMs;
      b.wholeRows = true;
      b.draw = false;
      break;
  }
  return b;
}

ScrollGovernor::Budget ScrollGovernor::update(uint32_t nowMs, uint32_t ringMs, uint32_t underruns,
                                              bool audioActive) {
  const bool newUnderrun = primed_ && underruns != lastUnderruns_;
  lastUnderruns_ = underruns;
  primed_ = true;
  if (!config_.enabled || !audioActive) {
    level_ = Level::Normal;
    return budget();
  }
  Level now = fromRing(ringMs, 0);
  if (newUnderrun && now < Level::Reduced) now = Level::Reduced;
  if (now > level_) {
    level_ = now;
    lastBadMs_ = nowMs;
  } else if (now == level_ && now != Level::Normal) {
    lastBadMs_ = nowMs;  // still at this level: the recovery clock waits
  } else if (now < level_) {
    // Better: only with the margin, and after holdMs of it; one level at a time.
    const Level withMargin = newUnderrun ? Level::Reduced : fromRing(ringMs, config_.recoverMarginMs);
    if (withMargin >= level_) {
      lastBadMs_ = nowMs;
    } else if (nowMs - lastBadMs_ >= config_.holdMs) {
      level_ = static_cast<Level>(static_cast<uint8_t>(level_) - 1);
      lastBadMs_ = nowMs;
    }
  }
  return budget();
}

const char* ScrollGovernor::name(Level l) {
  switch (l) {
    case Level::Reduced: return "reduced";
    case Level::WholeRows: return "whole-rows";
    case Level::Paused: return "paused";
    default: return "normal";
  }
}
