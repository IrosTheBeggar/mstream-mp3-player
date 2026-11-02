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
// next call once the card answers. A read that fails any other way (a select
// timeout, an R1 error, no data token, a CRC mismatch) sets nothing, nor do the
// volume's lock or no PSRAM for the FIL, so one such glitch at the boot would leave
// picks at random for the whole session.
//
// So a fault of the card is tried again. The tries are on the loop task, as the
// first is, where a card that doesn't answer costs about a second of the driver's
// retries each: so there are few of them, the first soon, since most faults are
// a glitch. After the last the file is off until a remount or a boot, as any
// other failure.
//
// The remount a read with no answer brings is that read's own echo, whoever's next
// FatFs call makes it (another reader of the root's, too). Taken for news of the
// card it would start the tries over and read at once, get no answer again, and so
// on at every pass, seconds of the loop each. So while the last try failed on the
// card's account a remount neither reads nor starts the count over until kEchoMs
// after it (remounted()); then it reads once, the count where it was. A card put
// back sooner waits for the next try, or, the tries over, the next remount or boot.
// A reader of the root makes that call itself, right after a read it failed
// (FatFs::echoNow()): left to the next caller, however late, the echo would come as
// news, a read and the next caller's after it, and a decoder's open it would cut.
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
  // A remount this soon after a try the card failed is taken for that try's echo.
  static constexpr uint32_t kEchoMs = 30000;

  // An open from outside (the boot, Dc), or one that worked: from its first try.
  void reset() {
    used_ = 0;
    cardFailed_ = false;
  }

  // A try that failed. True: it is tried again, when due() says. False: the file
  // is off until a remount or a boot.
  bool failed(OpenTry t, uint32_t nowMs) {
    cardFailed_ = t == OpenTry::Card;
    if (!cardFailed_) return false;
    failedAtMs_ = nowMs;
    if (used_ >= kAgain) return false;
    atMs_ = nowMs + waitMs(used_);
    ++used_;
    return true;
  }

  // FatFs mounted the card again. True: ask the card now. The last try was no fault
  // of the card's: a new count. False: that try's echo, and its wait stands. (Signed,
  // as due(), for a pass's older now. Far negative: past 2^31 ms, weeks on, no echo.)
  bool remounted(uint32_t nowMs) {
    if (!cardFailed_) {
      used_ = 0;
      return true;
    }
    const int32_t age = static_cast<int32_t>(nowMs - failedAtMs_);
    const int32_t echo = static_cast<int32_t>(kEchoMs);
    return age >= echo || age < -echo;
  }

  // The wait of the last true is over. (Signed: `nowMs` may be the pass's, a
  // little older than failed()'s; and it holds across millis()'s wrap.)
  bool due(uint32_t nowMs) const { return static_cast<int32_t>(nowMs - atMs_) >= 0; }
  // Of kAgain, those asked for since reset(), and the last one's wait.
  uint8_t used() const { return used_; }
  uint32_t lastWaitMs() const { return used_ ? waitMs(static_cast<uint8_t>(used_ - 1)) : 0; }

private:
  uint8_t used_ = 0;
  bool cardFailed_ = false;
  uint32_t atMs_ = 0;
  uint32_t failedAtMs_ = 0;
};

}  // namespace autodj
