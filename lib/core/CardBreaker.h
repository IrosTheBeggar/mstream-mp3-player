// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The card's breaker (docs/METADATA.md 3.8): what stops a restart at every boot, whatever
// brings it. A few words in RTC slow memory (the firmware's RTC_NOINIT_ATTR: they outlive
// esp_restart, not a power-on, hence the reset reason, the magic and the check). Portable,
// host-tested (test_card_breaker).
namespace cardbreaker {

constexpr uint32_t kMagic = 0x4D43424Bu;
// The guard's restarts in a row after which a boot holds the card jobs.
constexpr uint32_t kHoldAfter = 2;
// A session this long with no restart of the guard's is calm: the count starts again.
constexpr uint32_t kCalmMs = 30u * 60u * 1000u;

struct Word {
  uint32_t magic;
  uint32_t restarts;
  uint32_t check;
};

inline uint32_t checkOf(const Word& w) { return w.magic ^ w.restarts ^ 0xA5A55A5Au; }
inline void seal(Word& w) {
  w.magic = kMagic;
  w.check = checkOf(w);
}
inline bool valid(const Word& w) { return w.magic == kMagic && w.check == checkOf(w); }

// At each boot, once. `afterRestart`: the reset was esp_restart()'s (anything else, the
// words say nothing). True: this boot holds the card jobs. The hold is this boot's alone:
// the count starts again, so a power-on, or any restart from a held session, asks again.
inline bool atBoot(Word& w, bool afterRestart, uint32_t* found = nullptr) {
  const uint32_t n = afterRestart && valid(w) ? w.restarts : 0;
  if (found) *found = n;
  const bool hold = n >= kHoldAfter;
  w.restarts = hold ? 0 : n;
  seal(w);
  return hold;
}
// Right before a restart of the guard's that says nothing of another card.
inline void noteRestart(Word& w) {
  if (!valid(w)) w.restarts = 0;
  ++w.restarts;
  seal(w);
}
// Another card, its identity read: the count isn't its.  kCalmMs of a session: as well.
inline void clear(Word& w) {
  w.restarts = 0;
  seal(w);
}

}  // namespace cardbreaker
