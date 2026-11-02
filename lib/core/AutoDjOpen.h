// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "CardContainer.h"

// What a failed open of the card's AutoDJ index comes to (docs/AUTODJ.md 6.6).
// The glue (app/AutoDj) opens the file at the boot, at a remount and on the
// console's Dc. A file that isn't there, or fails a check, stays so until a
// remount changes it. A fault of the card says nothing of the file. A read that
// gets no answer marks the card failed, so FatFs mounts the volume again at its
// next call once the card answers, and that remount reopens the file. A read
// that fails any other way (a select timeout, an R1 error, no data token, a CRC
// mismatch) sets nothing, nor do the volume's lock or no PSRAM for the FIL, so
// one such glitch at the boot would leave picks at random for the whole session.
//
// So a fault of the card is tried again. The tries are on the loop task, as the
// first is, where a card that doesn't answer costs about a second of the driver's
// retries each: so there are few of them, the first soon, since most faults are
// a glitch. After the last the file is off until a remount or a boot, as any
// other failure.
//
// Portable, host-tested (test_autodj_open).
namespace autodj {

// What a try came to. Open: open, and checked against the root's entry.
// NotThere: FatFs says the file the root names isn't on the card. Checks: its
// frame, its header or the entry's check failed. Card: the card's fault, nothing
// said of the file: a read that failed, the volume's lock timing out, no PSRAM
// for the FIL.
enum class OpenTry : uint8_t { Open, NotThere, Checks, Card };

// The try of a file that opened, from Table::open()'s and the entry's verdict.
constexpr OpenTry openTryOf(cardcontract::Why why) {
  return why == cardcontract::Why::Ok   ? OpenTry::Open
         : why == cardcontract::Why::Io ? OpenTry::Card
                                        : OpenTry::Checks;
}

class OpenTries {
public:
  // The tries after the first, and the wait before each (0 the first of them).
  static constexpr uint8_t kAgain = 3;
  static constexpr uint32_t waitMs(uint8_t again) { return again == 0 ? 2000 : again == 1 ? 10000 : 30000; }

  // An open from outside (the boot, a remount, Dc): from its first try.
  void reset() { used_ = 0; }

  // A try that failed. True: it is tried again, when due() says. False: the file
  // is off until a remount or a boot.
  bool failed(OpenTry t, uint32_t nowMs) {
    if (t != OpenTry::Card || used_ >= kAgain) return false;
    atMs_ = nowMs + waitMs(used_);
    ++used_;
    return true;
  }

  // The wait of the last true is over. (Signed: `nowMs` may be the pass's, a
  // little older than failed()'s; and it holds across millis()'s wrap.)
  bool due(uint32_t nowMs) const { return static_cast<int32_t>(nowMs - atMs_) >= 0; }
  // Of kAgain, those asked for since reset(), and the last one's wait.
  uint8_t used() const { return used_; }
  uint32_t lastWaitMs() const { return used_ ? waitMs(static_cast<uint8_t>(used_ - 1)) : 0; }

private:
  uint8_t used_ = 0;
  uint32_t atMs_ = 0;
};

}  // namespace autodj
