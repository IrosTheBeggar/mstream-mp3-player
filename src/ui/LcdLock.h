// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// How long the LCD holds the SPI bus. The LCD and the microSD card (which
// feeds the decoder) share SPI3 and its mutex, and a push from a PSRAM sprite
// is copied by the CPU with the bus held, so every such hold is time the SD
// card can't read.
//
// LcdLock is a scoped M5.Display.startWrite()/endWrite() that timestamps the
// outermost hold (the one that takes the bus mutex) and adds it to a
// SpiHoldStats. Wrap each small push in one, never a whole frame:
//
//   { LcdLock lock(&stats); sprite.pushSprite(&M5.Display, x, y); }
//
// Loop task only (the one task that draws).
struct SpiHoldStats {
  uint32_t count = 0;     // holds
  uint64_t totalUs = 0;   // time held
  uint32_t maxUs = 0;     // longest single hold
  void reset() { *this = SpiHoldStats{}; }
  void add(uint32_t us) {
    ++count;
    totalUs += us;
    if (us > maxUs) maxUs = us;
  }
  float meanUs() const { return count ? static_cast<float>(totalUs) / count : 0.0f; }
};

class LcdLock {
public:
  explicit LcdLock(SpiHoldStats* stats = nullptr);
  ~LcdLock();
  LcdLock(const LcdLock&) = delete;
  LcdLock& operator=(const LcdLock&) = delete;

  // Microseconds held so far.
  uint32_t heldUs() const;

  // Every hold through an LcdLock since boot (or the last reset), whatever
  // stats it was also given.
  static SpiHoldStats& global();

private:
  SpiHoldStats* stats_;
  int64_t t0_ = 0;
  bool outermost_ = false;
};
