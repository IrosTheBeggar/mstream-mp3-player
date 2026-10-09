// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for lib/core SdBusy.h, the rules the patched SD driver
// (lib/SD/src/sd_diskio.cpp) waits out the card's busy by (docs/METADATA.md
// 3.8), against a bit-level model of the card's DO line: from CS falling,
// `lead` bits of the pull-up (a card not driving yet, or the byte a card
// sends after a Stop Tran token before its busy), then busy (0) up to the
// bit the card finishes at, then idle (1). The host reads it a byte at a
// time, MSB first, as SPI mode 0 does.
// - The stock wait (the first byte not 0x00) takes a pull-up byte, or the
//   byte before a Stop Tran's busy, for "ready", sends its command to the
//   busy card, and the response loop then reads the busy's edge as an R1:
//   0x01, 0x03 (the device's "token error [13] 0x3"), 0x07, and 0x0F to
//   0x7F (its "crc error"). The test finds each.
// - The patched wait (two bytes of 0xFF in a row) never lets a command
//   start before the card is done, for any lead up to a byte and any
//   busy length; after a Stop Tran its one skipped byte covers a card
//   whose busy starts a byte later.
// - The status check's retry and the write's: what is asked again.
// Run: pio test -e native -f test_sd_busy
#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <set>

#include "SdBusy.h"

void setUp() {}
void tearDown() {}

namespace {

// The card's DO from CS falling: bit t (0 = the first bit clocked).
struct Line {
  long lead;  // bits of the pull-up before the card drives the line
  long done;  // the bit at which the busy ends (<= lead: no busy at all)
  int bit(long t) const { return t < lead ? 1 : t < done ? 0 : 1; }
  uint8_t byteAt(long i) const {
    uint8_t b = 0;
    for (int k = 0; k < 8; ++k) b = static_cast<uint8_t>(b << 1 | bit(8 * i + k));
    return b;
  }
  bool busyAt(long bitIndex) const { return bitIndex >= lead && bitIndex < done; }
};

constexpr long kTimeoutBytes = 100000;

// The byte at which the stock wait returns (-1: its timeout).
long stockWait(const Line& l, long from = 0) {
  for (long i = from; i < from + kTimeoutBytes; ++i)
    if (sdbusy::stockReady(l.byteAt(i))) return i;
  return -1;
}

// The byte at which the patched wait returns (-1: its timeout).
long patchedWait(const Line& l, long from = 0) {
  sdbusy::Wait w;
  for (long i = from; i < from + kTimeoutBytes; ++i)
    if (w.feed(l.byteAt(i))) return i;
  return -1;
}

// The driver's command after a wait that returned at byte `ready`: the 6
// bytes of the command, then the response loop (up to 9 bytes, the first
// with bit 7 clear). A card busy when the command starts ignores it: the
// loop reads the line. Returns the R1 the driver takes (0xFF: none).
uint8_t r1After(const Line& l, long ready, uint8_t answer = 0x00) {
  const long cmd = ready + 1;
  if (!l.busyAt(8 * cmd) && !l.busyAt(8 * cmd + 47)) return answer;  // accepted: its own answer
  for (long i = cmd + 6; i < cmd + 6 + 9; ++i) {
    const uint8_t b = l.byteAt(i);
    if (!(b & 0x80)) return b;
  }
  return 0xFF;
}

}  // namespace

// A card that isn't busy: both waits return at once (the patched one a
// byte later), and the command is answered.
void test_idle_card() {
  const Line l{0, 0};
  TEST_ASSERT_EQUAL(0, stockWait(l));
  TEST_ASSERT_EQUAL(1, patchedWait(l));
  TEST_ASSERT_EQUAL_HEX8(0x00, r1After(l, patchedWait(l)));
}

// The byte that carries the busy's edge: 0x01 ... 0x7F; the wait isn't
// done until a whole byte of 1s follows it.
void test_edge_bytes() {
  for (int k = 1; k < 8; ++k) {
    const Line l{0, 16 + (8 - k)};  // two busy bytes, then k bits of 1s
    const uint8_t edge = l.byteAt(2);
    TEST_ASSERT_EQUAL_HEX8(static_cast<uint8_t>((1u << k) - 1), edge);
    sdbusy::Wait w;
    TEST_ASSERT_FALSE(w.feed(0x00));
    TEST_ASSERT_FALSE(w.feed(0x00));
    TEST_ASSERT_FALSE(w.feed(edge));
    TEST_ASSERT_FALSE(w.feed(0xFF));  // one idle byte: not yet
    TEST_ASSERT_TRUE(w.feed(0xFF));
    TEST_ASSERT_TRUE(w.ready());
  }
  // A busy byte after an idle one starts the count again (the byte before a
  // Stop Tran's busy).
  sdbusy::Wait w;
  TEST_ASSERT_FALSE(w.feed(0xFF));
  TEST_ASSERT_FALSE(w.feed(0x00));
  TEST_ASSERT_FALSE(w.feed(0xFF));
  TEST_ASSERT_TRUE(w.feed(0xFF));
}

// The stock wait against every lead (0-8 bits) and busy length (up to 40
// bytes): it sends its command to a busy card, and the R1s it then takes
// are the device's: 0x03 (token error), 0x0F and 0x1F (crc error), 0x01
// (a failed write with no line).
void test_stock_wait_takes_the_busy_for_ready() {
  std::set<uint8_t> r1s;
  int busyCommands = 0;
  for (long lead = 0; lead <= 8; ++lead) {
    for (long done = lead + 1; done < 8 * 40; ++done) {
      const Line l{lead, done};
      const long ready = stockWait(l);
      TEST_ASSERT_TRUE(ready >= 0);
      if (l.busyAt(8 * (ready + 1)) || l.busyAt(8 * (ready + 1) + 47)) ++busyCommands;
      r1s.insert(r1After(l, ready));
    }
  }
  TEST_ASSERT_TRUE(busyCommands > 1000);
  for (uint8_t seen : {0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F}) TEST_ASSERT_TRUE(r1s.count(seen) == 1);
  char msg[96];
  snprintf(msg, sizeof(msg), "stock: %d commands sent to a busy card; %u distinct R1s taken", busyCommands,
           static_cast<unsigned>(r1s.size()));
  TEST_MESSAGE(msg);
}

// The patched wait: never a command to a busy card, whatever the lead (up
// to a whole byte: the pull-up's bits, or the byte before a Stop Tran's
// busy) and the busy's length; the R1 is always the card's own.
void test_patched_wait_never_sends_to_a_busy_card() {
  for (long lead = 0; lead <= 8; ++lead) {
    for (long done = 0; done < 8 * 300; ++done) {
      const Line l{lead, done};
      const long ready = patchedWait(l);
      TEST_ASSERT_TRUE(ready >= 0);
      TEST_ASSERT_FALSE(l.busyAt(8 * (ready + 1)));
      TEST_ASSERT_EQUAL_HEX8(0x5A, r1After(l, ready, 0x5A));
      // ... and it waits at most two bytes past the busy's end.
      TEST_ASSERT_TRUE(ready <= (done + 7) / 8 + 2);
    }
  }
}

// After a multiple write's Stop Tran token, the patched driver reads one
// byte (the card's before its busy) and then waits: safe for a card whose
// busy starts up to a byte later still. Without the skip a busy that
// starts two bytes after the token would be missed.
void test_stop_tran() {
  for (long late = 0; late <= 8; ++late) {
    for (long busy = 1; busy < 8 * 200; ++busy) {
      // From the token's end: the card's byte (8 bits of 1s), `late` more
      // bits of 1s, then the busy.
      const Line l{8 + late, 8 + late + busy};
      const long ready = patchedWait(l, sdbusy::kStopTranSkip);
      TEST_ASSERT_TRUE(ready >= 0);
      TEST_ASSERT_TRUE(8 * (ready + 1) >= l.done);
    }
  }
  const Line twoLate{16, 16 + 64};
  TEST_ASSERT_TRUE(8 * (patchedWait(twoLate, 0) + 1) < twoLate.done);  // (why the skip)
}

// The status check: an R1 with an error bit is asked again once; no answer
// (0xFF: the driver already tried three times) and a good one are not.
void test_status_retry() {
  TEST_ASSERT_EQUAL(2, sdbusy::kStatusTries);
  for (uint8_t r1 : {0x01, 0x03, 0x07, 0x04, 0x40}) {
    TEST_ASSERT_TRUE(sdbusy::askStatusAgain(r1, 1));
    TEST_ASSERT_FALSE(sdbusy::askStatusAgain(r1, 2));
  }
  TEST_ASSERT_FALSE(sdbusy::askStatusAgain(0x00, 1));
  TEST_ASSERT_FALSE(sdbusy::askStatusAgain(0xFF, 1));
  TEST_ASSERT_EQUAL(2, sdbusy::kWriteTries);
  TEST_ASSERT_EQUAL(1, sdbusy::kStopTranSkip);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_card);
  RUN_TEST(test_edge_bytes);
  RUN_TEST(test_stock_wait_takes_the_busy_for_ready);
  RUN_TEST(test_patched_wait_never_sends_to_a_busy_card);
  RUN_TEST(test_stop_tran);
  RUN_TEST(test_status_retry);
  return UNITY_END();
}
