// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// When the list under a rail scrub (a finger dragging the A-Z rail's thumb)
// is drawn again. Every position a scrub asks for is a jump: the band's
// whole 168 lines are drawn again (50-78 ms on the device with nothing
// playing, 80-110 ms with an MP3), far too slow to follow a finger at the
// frame rate, and while it draws no touch is sampled (ui/ListView). So the
// rail alone (the thumb and its letter: a frame of a few ms) follows the
// finger on every frame, and the band catches up:
//
//   - at once when the finger has rested for settleMs (the position asked
//     for unchanged that long): the listener stopped to look;
//   - otherwise no sooner than cadenceMs after the last band draw ENDED,
//     so a slow draw never has the next one due before the loop has
//     sampled the finger again.
//
// Portable (the caller's millis()), host-tested in test_scrub_pacer.
class ScrubPacer {
public:
  struct Config {
    uint32_t cadenceMs = 60;  // from the end of a band draw to the next
    uint32_t settleMs = 40;   // a finger still this long: the band at once
  };

  ScrubPacer() = default;
  explicit ScrubPacer(const Config& c) : config_(c) {}
  void setConfig(const Config& c) { config_ = c; }
  const Config& config() const { return config_; }

  // The finger landed on the rail: nothing is owed, and the first position
  // it asks for is due at once.
  void begin(uint32_t nowMs);
  // The scrub asks for a position the band doesn't show.
  void targetChanged(uint32_t nowMs);
  // The band was drawn at the latest position; `endMs` is when that ended.
  void bandDrawn(uint32_t endMs);
  // A position is owed and the finger rested, or the cadence allows.
  bool bandDue(uint32_t nowMs) const;
  // A position is owed (asked for and not drawn yet).
  bool owed() const { return owed_; }

private:
  Config config_;
  bool owed_ = false;
  uint32_t changedMs_ = 0;  // the last targetChanged()
  uint32_t drawnMs_ = 0;    // the last bandDrawn() (or begin()'s stand-in)
};
