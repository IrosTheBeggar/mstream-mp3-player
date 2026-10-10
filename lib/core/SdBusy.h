// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

// The SD card's busy signal in SPI mode, as the patched SD driver reads it
// (lib/SD/src/sd_diskio.cpp; docs/METADATA.md 3.8). Portable, host-tested
// in test_sd_busy against a bit-level model of the card's DO line.
//
// While a card programs a write (after a block's data response, and after
// a multiple write's Stop Tran token) it holds DO low: every byte read with
// CS low reads 0x00. The line goes high when it's done, as often as not in
// the middle of a byte, so the byte that carries the edge reads 0x01,
// 0x03, 0x07 ... 0x7F. And a card just selected may leave the first bits
// of the first byte to the pull-up before it drives the line (0x80, 0xC0
// ... 0xFE).
//
// arduino-esp32 3.3.12's sdWait() took the first byte that wasn't 0x00 for
// "ready": a pull-up byte, or the Stop Tran's one byte before the busy
// starts. The command that followed went to a busy card, and the response
// loop then took the busy edge for its R1: 0x01 (a write that failed with
// no line), 0x03 or 0x07 ("token error"), 0x0F to 0x7F ("crc error",
// tried again 100 ms later). The device run logged exactly those, after
// walk.jnl's writes (2026-10-09).
namespace sdbusy {

// A byte of the idle line.
constexpr uint8_t kIdle = 0xFF;
// Ready: this many idle bytes in a row. One isn't enough: a pull-up byte
// (the card not driving yet) or the Stop Tran's byte before the busy can
// read 0xFF. The second byte's eight 1s are the card's own.
constexpr uint8_t kReadyBytes = 2;

// The wait: fed every byte read (CS low, MOSI high) until feed() says the
// card is ready (or the caller's timeout).
class Wait {
public:
  bool feed(uint8_t b) {
    run_ = b == kIdle ? static_cast<uint8_t>(run_ + 1) : 0;
    return run_ >= kReadyBytes;
  }
  bool ready() const { return run_ >= kReadyBytes; }

private:
  uint8_t run_ = 0;
};

// The stock driver's rule, for the tests: the first byte not 0x00.
constexpr bool stockReady(uint8_t b) { return b != 0x00; }

// After a multiple write's Stop Tran token the card sends one byte before
// its busy starts (Nbr): read and dropped before the wait, so the wait
// never starts on a byte the busy hasn't reached yet.
constexpr int kStopTranSkip = 1;

// The status check (ff_sd_status(): CMD13 at every FatFs call). An answer
// that isn't 0 makes FatFs mount the volume again (every open file gone:
// the decoder's, the walk's), so one wrong byte mustn't: an R1 with an
// error bit is asked again once. 0xFF (no answer: the driver has marked
// the card failed after its own three tries, or the select timed out) is
// not: a pulled card would only cost another 300 ms.
constexpr int kStatusTries = 2;
// Whether to ask again after try `tried` (from 1) answered `token`.
constexpr bool askStatusAgain(uint8_t token, int tried) {
  return token != 0x00 && token != 0xFF && tried < kStatusTries;
}

// A sector write the driver says failed is written again once, the same
// sectors and bytes (lib/core CachedDrive): the second try's select waits
// out whatever busy the first left.
constexpr int kWriteTries = 2;

}  // namespace sdbusy
