// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for RadioLinkNotes (the headphones' radio link as BtSink's
// PlayerA2dp notes it, and the link's set-up over it, counted for the
// card's scheduler: BtSink::linkSetups(), docs/METADATA.md 3.3.9, "The
// radio link's set-up", 2026-11-19): which reports turn the set-up on and
// off, the count (odd while one is set up, two for a new one over one still
// counted: a lost close, a page of ours over a radio link up), a failure
// that met their radio link keeping it, the close matched to the noted
// address, A2DP's rise and drop, Forget, a pairing and Bn mid-set-up, and
// rd-g5-fix1's boot.
// Run: pio test -e native -f test_radio_link_notes
#include <unity.h>

#include <cstdint>

#include "RadioLinkNotes.h"

void setUp() {}
void tearDown() {}

namespace {
using N = RadioLinkNotes;
using O = N::Opened;
constexpr uint64_t kThem = 0x0011223344AAull;   // the remembered headphones (packed)
constexpr uint64_t kOther = 0x0011223344BBull;  // another device
}  // namespace

// Their own radio link: the set-up from its open to A2DP's link; the radio
// link's close after the link changes nothing more.
void test_their_radio_link_then_the_link() {
  N n;
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
  TEST_ASSERT_FALSE(n.settingUp());
  TEST_ASSERT_EQUAL(O::Theirs, n.opened(kThem, true, false, 10015));
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_TRUE(n.theirsWaiting());
  TEST_ASSERT_TRUE(n.closes(kThem));
  TEST_ASSERT_FALSE(n.closes(kOther));
  TEST_ASSERT_EQUAL_UINT32(10015, n.upAtMs());
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  n.linkUp(kThem);
  TEST_ASSERT_FALSE(n.settingUp());
  TEST_ASSERT_FALSE(n.theirsWaiting());
  TEST_ASSERT_TRUE(n.linked());
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  // A2DP drops, the radio link stays (no set-up: the relink over it isn't
  // one), then a relink, then the radio link closes.
  n.linkDown();
  TEST_ASSERT_FALSE(n.settingUp());
  n.linkUp(kThem);
  TEST_ASSERT_TRUE(n.closed(kThem));
  TEST_ASSERT_FALSE(n.up());
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  n.linkDown();
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  // Handed to a burst (planStep()): no longer theirs to set up, still the set-up.
  N h;
  h.opened(kThem, true, false, 0);
  h.theirsHandled();
  TEST_ASSERT_FALSE(h.theirsWaiting());
  TEST_ASSERT_TRUE(h.settingUp());
  TEST_ASSERT_EQUAL_UINT32(1, h.count());
}

// A page of ours answered: the set-up, not theirs; a radio link that closes
// before A2DP ends it.
void test_a_page_answered_and_a_close_before_a2dp() {
  N n;
  TEST_ASSERT_EQUAL(O::Answered, n.opened(kThem, true, true, 25453));
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_FALSE(n.theirsWaiting());
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  TEST_ASSERT_TRUE(n.closed(kThem));
  TEST_ASSERT_FALSE(n.settingUp());
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  // A second report of the same close, or another device's: nothing.
  TEST_ASSERT_FALSE(n.closed(kThem));
  TEST_ASSERT_FALSE(n.closed(kOther));
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
}

// A failed Connection Complete: before their radio link (rd-g5-fix2: the
// boot's page met theirs, HCI 0x0B, 3.2 s before their radio link was
// reported) nothing is noted; while it is noted up, any code keeps it (a
// device has one radio link: the failure isn't its end); one for another
// address leaves the notes alone.
void test_failures() {
  N n;
  TEST_ASSERT_EQUAL(O::Failed, n.opened(kThem, false, true, 6797));
  TEST_ASSERT_FALSE(n.up());
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
  TEST_ASSERT_EQUAL(O::Theirs, n.opened(kThem, true, false, 10015));
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  // A page out when theirs came up, its failure after it: 0x0B or another code.
  for (int i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL(O::Kept, n.opened(kThem, false, i != 1, 10100 + i));
    TEST_ASSERT_TRUE(n.settingUp());
    TEST_ASSERT_TRUE(n.theirsWaiting());
    TEST_ASSERT_EQUAL_UINT32(10015, n.upAtMs());
    TEST_ASSERT_EQUAL_UINT32(1, n.count());
  }
  TEST_ASSERT_EQUAL(O::Failed, n.opened(kOther, false, true, 10200));
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_TRUE(n.closes(kThem));
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  // Linked, a failure is kept too (no page goes out while linked).
  n.linkUp(kThem);
  TEST_ASSERT_EQUAL(O::Kept, n.opened(kThem, false, false, 12000));
  TEST_ASSERT_TRUE(n.linked());
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
}

// A radio link opened over one whose close never came (BtAppT's queue
// full): a new set-up, two counted, its own time; and over one that came
// up for A2DP (its close lost too): one, the set-up begins.
void test_a_lost_close() {
  N n;
  n.opened(kThem, true, false, 1000);
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  TEST_ASSERT_EQUAL(O::Answered, n.opened(kThem, true, true, 9000));
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_FALSE(n.theirsWaiting());
  TEST_ASSERT_EQUAL_UINT32(9000, n.upAtMs());
  TEST_ASSERT_EQUAL_UINT32(3, n.count());
  n.linkUp(kThem);
  n.linkDown();
  TEST_ASSERT_EQUAL_UINT32(4, n.count());
  n.opened(kThem, true, false, 20000);
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_EQUAL_UINT32(5, n.count());
}

// A2DP up with no radio link seen to open (its report lost, or the link
// made before the notes began): noted up with the link, no set-up counted;
// its close clears it.
void test_the_link_with_no_open_seen() {
  N n;
  n.linkUp(kThem);
  TEST_ASSERT_TRUE(n.up());
  TEST_ASSERT_TRUE(n.linked());
  TEST_ASSERT_TRUE(n.closes(kThem));
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
  TEST_ASSERT_TRUE(n.closed(kThem));
  TEST_ASSERT_FALSE(n.up());
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
}

// The radio link closed while A2DP's drop hasn't been reported, and a new
// one opened: no set-up while A2DP still reads up; it begins at the drop.
void test_a_radio_link_before_the_drop_is_reported() {
  N n;
  n.opened(kThem, true, true, 0);
  n.linkUp(kThem);
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  TEST_ASSERT_TRUE(n.closed(kThem));
  n.opened(kThem, true, false, 5000);
  TEST_ASSERT_TRUE(n.up());
  TEST_ASSERT_FALSE(n.settingUp());
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  n.linkDown();
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_EQUAL_UINT32(3, n.count());
}

// A page of ours over a radio link up with no A2DP over it (a burst's over
// theirs, a back-off's over one that lingers): A2DP opened over it, a
// set-up again (two). Not to another address, nor with nothing noted, nor
// once linked.
void test_a_page_over_a_radio_link() {
  N n;
  n.paged(kThem);
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
  n.opened(kThem, true, false, 12828);
  n.paged(kThem);
  TEST_ASSERT_TRUE(n.settingUp());
  TEST_ASSERT_EQUAL_UINT32(3, n.count());
  n.paged(kOther);
  TEST_ASSERT_EQUAL_UINT32(3, n.count());
  n.paged(kThem);
  TEST_ASSERT_EQUAL_UINT32(5, n.count());
  n.linkUp(kThem);
  TEST_ASSERT_EQUAL_UINT32(6, n.count());
  n.paged(kThem);
  TEST_ASSERT_EQUAL_UINT32(6, n.count());
}

// The remembered address changing mid-set-up (Forget, a pairing's start or
// its failure, the console's Bn): the notes start afresh, the set-up's end
// counted; a later close of that radio link is nobody's. While linked the
// notes stay (they are about the linked headphones).
void test_forget() {
  N n;
  n.opened(kThem, true, false, 0);
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  n.forget();
  TEST_ASSERT_FALSE(n.up());
  TEST_ASSERT_FALSE(n.settingUp());
  TEST_ASSERT_FALSE(n.closes(kThem));
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  TEST_ASSERT_FALSE(n.closed(kThem));
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  n.forget();  // (nothing noted: nothing changes)
  TEST_ASSERT_EQUAL_UINT32(2, n.count());
  N l;
  l.opened(kThem, true, true, 0);
  l.linkUp(kThem);
  l.forget();
  TEST_ASSERT_TRUE(l.up());
  TEST_ASSERT_TRUE(l.linked());
  TEST_ASSERT_TRUE(l.closes(kThem));
  TEST_ASSERT_EQUAL_UINT32(2, l.count());
}

// rd-g5-fix1 (2026-11-19) as BtAppT saw it: page 1 unanswered (HCI 0x04);
// their radio link at 12.83 s, pages 2 and 3 over it (17.08 s, 36.34 s);
// its close at 43.17 s with no A2DP (HCI 0x08); their next radio link at
// 46.03 s, A2DP up over it at 46.64 s. Odd while a set-up holds; each page
// over the first one a new set-up.
void test_rd_g5_fix1() {
  N n;
  TEST_ASSERT_EQUAL(O::Failed, n.opened(kThem, false, true, 11484));
  TEST_ASSERT_EQUAL_UINT32(0, n.count());
  TEST_ASSERT_EQUAL(O::Theirs, n.opened(kThem, true, false, 12828));
  TEST_ASSERT_EQUAL_UINT32(1, n.count());
  n.theirsHandled();  // (4 s on, in a burst its next page goes over it)
  n.paged(kThem);
  TEST_ASSERT_EQUAL_UINT32(3, n.count());
  n.paged(kThem);
  TEST_ASSERT_EQUAL_UINT32(5, n.count());
  TEST_ASSERT_TRUE(n.closed(kThem));
  TEST_ASSERT_EQUAL_UINT32(6, n.count());
  TEST_ASSERT_EQUAL(O::Theirs, n.opened(kThem, true, false, 46031));
  TEST_ASSERT_EQUAL_UINT32(7, n.count());
  n.linkUp(kThem);
  TEST_ASSERT_EQUAL_UINT32(8, n.count());
  TEST_ASSERT_FALSE(n.settingUp());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_their_radio_link_then_the_link);
  RUN_TEST(test_a_page_answered_and_a_close_before_a2dp);
  RUN_TEST(test_failures);
  RUN_TEST(test_a_lost_close);
  RUN_TEST(test_the_link_with_no_open_seen);
  RUN_TEST(test_a_radio_link_before_the_drop_is_reported);
  RUN_TEST(test_a_page_over_a_radio_link);
  RUN_TEST(test_forget);
  RUN_TEST(test_rd_g5_fix1);
  return UNITY_END();
}
