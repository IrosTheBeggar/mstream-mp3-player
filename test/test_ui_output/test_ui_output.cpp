// Host tests for the Output screen's decisions (OutputModel): the
// Bluetooth card for every link state, when a connection the listener
// asked for has failed, Forget's second tap, and the pairing scan's list.
// Run: pio test -e native
#include <unity.h>

#include <cstring>

#include <cstdio>
#include <initializer_list>

#include "OutputModel.h"
#include "UiText.h"

void setUp() {}
void tearDown() {}

namespace {

BtLink link(BtLink::Phase p, bool remembered = true, uint8_t attempt = 0, uint8_t attempts = 0) {
  BtLink l;
  l.phase = p;
  l.remembered = remembered;
  l.attempt = attempt;
  l.attempts = attempts;
  return l;
}

const uint8_t kA[6] = {1, 2, 3, 4, 5, 6};
const uint8_t kB[6] = {1, 2, 3, 4, 5, 7};

// Class of device: major Audio/Video (4), a minor.
uint32_t cod(uint32_t minor) { return (4u << 8) | (minor << 2); }

}  // namespace

// ---- the card ----

void test_card_for_every_link_state() {
  using P = BtLink::Phase;
  BtSession s;
  char buf[64];

  BtCardView v = btCardView(link(P::Linked), s, false);
  TEST_ASSERT_EQUAL(BtCard::Connected, v.card);
  TEST_ASSERT_EQUAL(BtTone::Cyan, v.tone);
  TEST_ASSERT_EQUAL_INT(3, v.buttonCount);
  TEST_ASSERT_EQUAL(BtButton::Disconnect, v.buttons[0]);
  TEST_ASSERT_EQUAL(BtButton::More, v.buttons[1]);  // Forget is in its sheet
  TEST_ASSERT_EQUAL(BtButton::Volume, v.buttons[2]);
  TEST_ASSERT_EQUAL_STRING("Connected  SBC 44.1 kHz, 175 ms",
                           btStatusLine(v, link(P::Linked), "SPYDRONE", "SBC 44.1 kHz, 175 ms", buf, sizeof(buf)));

  v = btCardView(link(P::Paging, true, 2, 3), s, false);
  TEST_ASSERT_EQUAL(BtCard::Connecting, v.card);
  TEST_ASSERT_TRUE(v.spinner);
  TEST_ASSERT_EQUAL(BtButton::Cancel, v.buttons[0]);
  TEST_ASSERT_EQUAL_INT(1, v.buttonCount);
  TEST_ASSERT_EQUAL_STRING("Connecting...  try 2 of 3",
                           btStatusLine(v, link(P::Paging, true, 2, 3), "SPYDRONE", "", buf, sizeof(buf)));

  v = btCardView(link(P::Scanning), s, false);
  TEST_ASSERT_EQUAL(BtCard::Searching, v.card);
  TEST_ASSERT_EQUAL_STRING("Looking for SPYDRONE...", btStatusLine(v, link(P::Scanning), "SPYDRONE", "", buf, sizeof(buf)));

  v = btCardView(link(P::Off), s, false);
  TEST_ASSERT_EQUAL(BtCard::Off, v.card);
  TEST_ASSERT_EQUAL(BtButton::Connect, v.buttons[0]);
  TEST_ASSERT_EQUAL(BtButton::Forget, v.buttons[1]);

  v = btCardView(link(P::Off, false), s, false);
  TEST_ASSERT_EQUAL(BtCard::NotPaired, v.card);
  TEST_ASSERT_EQUAL(BtButton::Pair, v.buttons[0]);
  TEST_ASSERT_EQUAL_STRING("No headphones paired", btStatusLine(v, link(P::Off, false), "", "", buf, sizeof(buf)));

  v = btCardView(link(P::Pairing, true, 1, 3), s, false);
  TEST_ASSERT_EQUAL(BtCard::Pairing, v.card);
  TEST_ASSERT_EQUAL(BtButton::Cancel, v.buttons[0]);

  // Dropped while it was the output: Lost, whatever it's doing to come back.
  v = btCardView(link(P::Paging, true, 1, 3), s, true);
  TEST_ASSERT_EQUAL(BtCard::Lost, v.card);
  TEST_ASSERT_EQUAL(BtTone::Red, v.tone);
  TEST_ASSERT_EQUAL_STRING("Lost: reconnecting...  try 1 of 3",
                           btStatusLine(v, link(P::Paging, true, 1, 3), "SPYDRONE", "", buf, sizeof(buf)));
  // ... but not once the listener stopped it trying.
  TEST_ASSERT_EQUAL(BtCard::Off, btCardView(link(P::Off), s, true).card);
}

// A connection the listener asked for: wanted until it links; failed when
// BtSink's tries run out (it goes on to scanning by name).
void test_a_connect_that_runs_out_of_tries_fails() {
  using P = BtLink::Phase;
  BtSession s;
  s.connect(0);
  TEST_ASSERT_TRUE(s.wanted());
  s.update(link(P::Off), 10);  // not started yet: not a failure
  TEST_ASSERT_TRUE(s.wanted());
  TEST_ASSERT_FALSE(s.failed());
  s.update(link(P::Paging, true, 1, 3), 100);
  s.update(link(P::Paging, true, 3, 3), 20000);
  TEST_ASSERT_TRUE(s.wanted());
  s.update(link(P::Scanning), 30000);
  TEST_ASSERT_FALSE(s.wanted());
  TEST_ASSERT_TRUE(s.failed());
  BtCardView v = btCardView(link(P::Scanning), s, false);
  TEST_ASSERT_EQUAL(BtCard::Failed, v.card);
  TEST_ASSERT_EQUAL(BtButton::TryAgain, v.buttons[0]);
  char buf[64];
  TEST_ASSERT_EQUAL_STRING("Couldn't connect. On and nearby?",
                           btStatusLine(v, link(P::Scanning), "", "", buf, sizeof(buf)));
  // They turn up by themselves later: connected, no longer failed.
  s.update(link(P::Linked), 60000);
  TEST_ASSERT_FALSE(s.failed());
  TEST_ASSERT_EQUAL(BtCard::Connected, btCardView(link(P::Linked), s, false).card);
}

void test_a_connect_that_links_is_satisfied() {
  using P = BtLink::Phase;
  BtSession s;
  s.connect(0);
  s.update(link(P::Paging, true, 1, 3), 100);
  s.update(link(P::Linked), 2000);
  TEST_ASSERT_FALSE(s.wanted());
  TEST_ASSERT_FALSE(s.failed());
  // Cancel before it links: neither wanted nor failed.
  s.connect(3000);
  s.update(link(P::Paging, true, 1, 3), 3100);
  s.cancel();
  s.update(link(P::Off), 3200);
  TEST_ASSERT_FALSE(s.wanted());
  TEST_ASSERT_FALSE(s.failed());
}

// A play that waited for the headphones ended without them (PlayGate): the
// ask is withdrawn, the link left paging. A link it brings answers nothing.
void test_a_withdrawn_connect_answers_nothing() {
  using P = BtLink::Phase;
  // Failed (the play's backstop, the third try still paging): the card is
  // red with the notice, and stays so while the tries go on.
  BtSession s;
  s.connect(0);
  s.update(link(P::Paging, true, 3, 3), 20000);
  s.withdraw(true);
  TEST_ASSERT_FALSE(s.wanted());
  TEST_ASSERT_TRUE(s.failed());
  s.update(link(P::Paging, true, 3, 3), 21000);
  s.update(link(P::Scanning), 30000);
  TEST_ASSERT_TRUE(s.failed());
  TEST_ASSERT_EQUAL(BtCard::Failed, btCardView(link(P::Scanning), s, false).card);
  // A link comes, phase first, then the event: not asked for.
  s.update(link(P::Linked), 40000);
  TEST_ASSERT_FALSE(s.failed());
  TEST_ASSERT_FALSE(s.onConnected().asked);
  // Cancelled: neither wanted nor failed; the card shows the page on its way.
  BtSession c;
  c.connect(0);
  c.update(link(P::Paging, true, 1, 3), 100);
  c.withdraw(false);
  TEST_ASSERT_FALSE(c.wanted());
  TEST_ASSERT_FALSE(c.failed());
  TEST_ASSERT_EQUAL(BtCard::Connecting, btCardView(link(P::Paging, true, 1, 3), c, false).card);
  c.update(link(P::Scanning), 30000);
  TEST_ASSERT_FALSE(c.failed());  // its tries running out isn't the listener's failure
  TEST_ASSERT_FALSE(c.onConnected().asked);
  // A pairing the listener started is theirs: kept.
  BtSession p;
  p.pairStarted(0);
  p.update(link(P::Pairing, true, 1, 3), 100);
  p.withdraw(false);
  TEST_ASSERT_TRUE(p.wanted());
  TEST_ASSERT_TRUE(p.pairing());
  const BtSession::Answer a = p.onConnected();
  TEST_ASSERT_TRUE(a.asked);
  TEST_ASSERT_TRUE(a.paired);
}

void test_a_pairing_fails_when_it_stops_or_takes_too_long() {
  using P = BtLink::Phase;
  BtSession s;
  s.pairStarted(0);
  TEST_ASSERT_TRUE(s.pairing());
  TEST_ASSERT_TRUE(s.pairingUnderWay());  // (keeps the screen lit)
  s.update(link(P::Pairing, true, 1, 3), 100);
  TEST_ASSERT_TRUE(s.pairingUnderWay());
  s.update(link(P::Off), 30000);  // BtSink gave up
  TEST_ASSERT_TRUE(s.failed());
  // The card still says it was a pairing, but none is under way: the screen
  // may dim and go off (main.cpp's keepLit), however long the card is up.
  TEST_ASSERT_TRUE(s.pairing());
  TEST_ASSERT_FALSE(s.pairingUnderWay());
  s.update(link(P::Off), 3600000);
  TEST_ASSERT_FALSE(s.pairingUnderWay());
  BtCardView v = btCardView(link(P::Off), s, false);
  TEST_ASSERT_TRUE(v.pairFailed);
  // Try again (pick them again), or Connect: the old ones, still remembered.
  TEST_ASSERT_EQUAL(BtButton::TryAgain, v.buttons[0]);
  TEST_ASSERT_EQUAL(BtButton::Connect, v.buttons[1]);
  char buf[64];
  TEST_ASSERT_EQUAL_STRING("Couldn't pair. In pairing mode?", btStatusLine(v, link(P::Off), "", "", buf, sizeof(buf)));

  BtSession t;
  t.pairStarted(1000);
  t.update(link(P::Pairing, true, 1, 3), 1100);
  t.update(link(P::Pairing, true, 1, 3), 1000 + BtSession::kPairTimeoutMs - 1);
  TEST_ASSERT_FALSE(t.failed());
  t.update(link(P::Pairing, true, 1, 3), 1000 + BtSession::kPairTimeoutMs);
  TEST_ASSERT_TRUE(t.failed());
  TEST_ASSERT_FALSE(t.pairingUnderWay());  // (the 45 s backstop: not under way any more)
}

// A link let go on purpose (Disconnect, a new pairing): its drop is no
// "lost" dialog, and a new link ends the expectation.
void test_an_expected_drop_is_forgotten_by_the_next_link() {
  using P = BtLink::Phase;
  BtSession s;
  s.update(link(P::Linked), 0);
  s.expectDrop(0);
  TEST_ASSERT_TRUE(s.dropExpected());
  s.update(link(P::Pairing, true, 1, 3), 100);
  TEST_ASSERT_TRUE(s.dropExpected());
  s.update(link(P::Linked), 5000);
  TEST_ASSERT_FALSE(s.dropExpected());
}

// A drop expected of a link that never goes (the headphones picked on the
// Pair screen were the linked ones): not expected for ever, or a real drop
// much later would be taken for it (no pause, no dialog).
void test_a_drop_that_never_comes_is_no_longer_expected() {
  using P = BtLink::Phase;
  BtSession s;
  s.update(link(P::Linked), 0);
  s.expectDrop(1000);
  s.update(link(P::Linked), 1000 + BtSession::kDropWaitMs - 1);
  TEST_ASSERT_TRUE(s.dropExpected());
  s.update(link(P::Linked), 1000 + BtSession::kDropWaitMs);
  TEST_ASSERT_FALSE(s.dropExpected());
}

// Pairing new headphones while others are linked: the old link is still
// up for a while (BtSink lets it go first). That Linked isn't the answer.
void test_pairing_while_linked_waits_for_the_new_link() {
  using P = BtLink::Phase;
  BtSession s;
  s.update(link(P::Linked), 0);
  s.expectDrop(100);
  s.pairStarted(100);
  s.update(link(P::Linked), 105);  // the old headphones, not gone yet
  TEST_ASSERT_TRUE(s.pairing());
  TEST_ASSERT_TRUE(s.wanted());
  s.update(link(P::Linked), 900);
  TEST_ASSERT_TRUE(s.pairing());
  s.update(link(P::Pairing, true, 1, 3), 1000);  // gone; paging the new ones
  TEST_ASSERT_TRUE(s.pairing());
  s.update(link(P::Off), 20000);  // BtSink gave up
  TEST_ASSERT_TRUE(s.failed());
  const BtCardView v = btCardView(link(P::Off), s, false);
  TEST_ASSERT_EQUAL(BtCard::Failed, v.card);
  TEST_ASSERT_TRUE(v.pairFailed);
  TEST_ASSERT_EQUAL(BtButton::Connect, v.buttons[1]);  // the old ones, still remembered

  // The same, and the new ones link: a pairing that was asked for.
  BtSession t;
  t.update(link(P::Linked), 0);
  t.pairStarted(100);
  t.update(link(P::Linked), 105);
  t.update(link(P::Pairing, true, 1, 3), 1000);
  t.update(link(P::Linked), 3000);
  TEST_ASSERT_FALSE(t.wanted());
  const BtSession::Answer a = t.onConnected();
  TEST_ASSERT_TRUE(a.asked);
  TEST_ASSERT_TRUE(a.paired);
  // Once: a later reconnect by themselves wasn't asked for.
  TEST_ASSERT_FALSE(t.onConnected().asked);
}

// The Connected event and the Linked phase come from different tasks:
// the loop may see either first. The answer is the same.
void test_the_connected_event_answers_whichever_comes_first() {
  using P = BtLink::Phase;
  // The event first.
  BtSession s;
  s.pairStarted(0);
  s.update(link(P::Pairing, true, 1, 3), 10);
  BtSession::Answer a = s.onConnected();
  TEST_ASSERT_TRUE(a.asked);
  TEST_ASSERT_TRUE(a.paired);
  s.update(link(P::Linked), 20);
  TEST_ASSERT_FALSE(s.onConnected().asked);  // nothing left over
  // The phase first.
  BtSession t;
  t.connect(0);
  t.update(link(P::Paging, true, 1, 3), 10);
  t.update(link(P::Linked), 20);
  a = t.onConnected();
  TEST_ASSERT_TRUE(a.asked);
  TEST_ASSERT_FALSE(a.paired);
  // Not asked for (they came back by themselves).
  BtSession u;
  u.update(link(P::Scanning), 0);
  u.update(link(P::Linked), 10);
  TEST_ASSERT_FALSE(u.onConnected().asked);
  // Cancelled before it linked: not asked for any more.
  BtSession w;
  w.connect(0);
  w.cancel();
  w.update(link(P::Linked), 10);
  TEST_ASSERT_FALSE(w.onConnected().asked);
}

// A connect with no headphones remembered is a scan by name: the card
// says so (with Cancel), and it fails after a while instead of waiting
// for ever.
void test_a_connect_with_none_remembered_searches_then_fails() {
  using P = BtLink::Phase;
  BtSession s;
  TEST_ASSERT_EQUAL(BtCard::NotPaired, btCardView(link(P::Scanning, false), s, false).card);
  s.connect(1000);
  BtCardView v = btCardView(link(P::Scanning, false), s, false);
  TEST_ASSERT_EQUAL(BtCard::Searching, v.card);
  TEST_ASSERT_TRUE(v.spinner);
  TEST_ASSERT_EQUAL(BtButton::Cancel, v.buttons[0]);
  s.update(link(P::Scanning, false), 1000 + BtSession::kFindTimeoutMs - 1);
  TEST_ASSERT_TRUE(s.wanted());
  s.update(link(P::Scanning, false), 1000 + BtSession::kFindTimeoutMs);
  TEST_ASSERT_FALSE(s.wanted());
  TEST_ASSERT_TRUE(s.failed());
  v = btCardView(link(P::Scanning, false), s, false);
  TEST_ASSERT_EQUAL(BtCard::Failed, v.card);
  // Nothing to forget: try again, or pair.
  TEST_ASSERT_EQUAL(BtButton::TryAgain, v.buttons[0]);
  TEST_ASSERT_EQUAL(BtButton::Pair, v.buttons[1]);
  // A connect with them remembered has its tries: no timeout of this kind.
  BtSession t;
  t.connect(0);
  t.update(link(P::Paging, true, 1, 3), BtSession::kFindTimeoutMs + 5);
  TEST_ASSERT_TRUE(t.wanted());
}

// ---- Forget's second tap ----

void test_forget_needs_a_second_tap_within_3_s() {
  ConfirmTap c;
  TEST_ASSERT_FALSE(c.tap(1000));
  TEST_ASSERT_TRUE(c.armed(1000));
  TEST_ASSERT_TRUE(c.armed(3999));
  TEST_ASSERT_TRUE(c.tap(3999));  // confirmed
  TEST_ASSERT_FALSE(c.armed(4000));
  // Too late: the second tap arms it again.
  TEST_ASSERT_FALSE(c.tap(10000));
  TEST_ASSERT_TRUE(c.expired(13000));
  TEST_ASSERT_FALSE(c.tap(13000));
  TEST_ASSERT_TRUE(c.tap(13500));
}

// ---- the pairing scan's list ----

void test_scan_list_keeps_each_device_once_in_the_order_found() {
  BtScanList l;
  l.note(kA, "WH-1000XM4", -70, cod(6));
  l.note(kB, "JBL Flip 5", -50, cod(7));
  TEST_ASSERT_EQUAL_INT(2, l.count());
  // Seen again, stronger: updated in place, not moved.
  l.note(kA, "", -48, 0);
  TEST_ASSERT_EQUAL_INT(2, l.count());
  TEST_ASSERT_EQUAL_STRING("WH-1000XM4", l.at(0).name);  // the name kept
  TEST_ASSERT_EQUAL_INT(-48, l.at(0).rssi);
  TEST_ASSERT_EQUAL(BtDevice::Kind::Headphones, l.at(0).kind);  // cod 0 keeps the kind
  TEST_ASSERT_EQUAL(BtDevice::Kind::Speaker, l.at(1).kind);
  TEST_ASSERT_EQUAL_STRING("speaker", BtScanList::kindName(l.at(1).kind));
}

void test_scan_list_full_drops_the_weakest_for_a_stronger_one() {
  BtScanList l;
  uint8_t a[6] = {9, 9, 9, 9, 9, 0};
  for (int i = 0; i < BtScanList::kMax; ++i) {
    a[5] = static_cast<uint8_t>(i);
    l.note(a, "x", -60 - i, cod(6));
  }
  TEST_ASSERT_EQUAL_INT(BtScanList::kMax, l.count());
  a[5] = 200;
  l.note(a, "weak", -95, cod(6));  // weaker than all: not kept
  TEST_ASSERT_EQUAL_INT(BtScanList::kMax, l.count());
  for (int i = 0; i < l.count(); ++i) TEST_ASSERT_NOT_EQUAL(200, l.at(i).addr[5]);
  a[5] = 201;
  l.note(a, "strong", -40, cod(6));  // takes the weakest's place (-71)
  bool found = false;
  for (int i = 0; i < l.count(); ++i) {
    TEST_ASSERT_NOT_EQUAL(BtScanList::kMax - 1, l.at(i).addr[5]);
    if (l.at(i).addr[5] == 201) found = true;
  }
  TEST_ASSERT_TRUE(found);
}

void test_signal_bars_and_device_kinds() {
  TEST_ASSERT_EQUAL_INT(4, BtScanList::bars(-40));
  TEST_ASSERT_EQUAL_INT(3, BtScanList::bars(-60));
  TEST_ASSERT_EQUAL_INT(2, BtScanList::bars(-75));
  TEST_ASSERT_EQUAL_INT(1, BtScanList::bars(-88));
  TEST_ASSERT_EQUAL_INT(0, BtScanList::bars(-100));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Headphones, BtScanList::kindOf(cod(1)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Headphones, BtScanList::kindOf(cod(2)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Speaker, BtScanList::kindOf(cod(5)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Speaker, BtScanList::kindOf(cod(10)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Car, BtScanList::kindOf(cod(8)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Other, BtScanList::kindOf(cod(9)));
  TEST_ASSERT_EQUAL(BtDevice::Kind::Other, BtScanList::kindOf((1u << 8) | (6u << 2)));  // a computer
  // A real headphones' class of device (0x240404: rendering + audio, headset).
  TEST_ASSERT_EQUAL(BtDevice::Kind::Headphones, BtScanList::kindOf(0x240404));
}

// The background search rests (15 min, or nobody around): the card says
// they'll come back by themselves, offers Connect, spins nothing, and
// isn't amber; "lost" doesn't claim it is still looking.
void test_the_resting_card() {
  using P = BtLink::Phase;
  BtSession s;
  char buf[64];
  BtCardView v = btCardView(link(P::Resting), s, false);
  TEST_ASSERT_EQUAL(BtCard::Resting, v.card);
  TEST_ASSERT_EQUAL(BtTone::Dim, v.tone);
  TEST_ASSERT_FALSE(v.spinner);
  TEST_ASSERT_TRUE(v.hint);
  TEST_ASSERT_EQUAL_INT(1, v.buttonCount);
  TEST_ASSERT_EQUAL(BtButton::Connect, v.buttons[0]);
  TEST_ASSERT_EQUAL_STRING("Not connected", btStatusLine(v, link(P::Resting), "SPYDRONE", "", buf, sizeof(buf)));
  // With the hint: "Not connected. They'll reconnect when switched on."
  char whole[96];
  snprintf(whole, sizeof(whole), "%s. %s %s", buf, uitext::kBtHintLine1, uitext::kBtHintLine2);
  TEST_ASSERT_EQUAL_STRING("Not connected. They'll reconnect when switched on.", whole);
  // Dropped while the output: still the resting card (not "Lost: looking
  // for them", a spinner and Cancel), red as the tab bar is.
  TEST_ASSERT_FALSE(v.lostHint);
  v = btCardView(link(P::Resting), s, true);
  TEST_ASSERT_EQUAL(BtCard::Resting, v.card);
  TEST_ASSERT_EQUAL(BtTone::Red, v.tone);
  TEST_ASSERT_FALSE(v.spinner);
  // ... resting only after the whole back-off (the quiet rule doesn't count
  // the drop's pause): maybe back in range, their own reconnect given up.
  TEST_ASSERT_TRUE(v.hint);
  TEST_ASSERT_TRUE(v.lostHint);
  snprintf(whole, sizeof(whole), "%s. %s %s", buf, uitext::kBtLostHintLine1, uitext::kBtLostHintLine2);
  TEST_ASSERT_EQUAL_STRING("Not connected. Back in range? Tap Connect.", whole);
  // Nothing remembered: nothing to rest for, the pairing card.
  TEST_ASSERT_EQUAL(BtCard::NotPaired, btCardView(link(P::Resting, false), s, false).card);
  // The back-off pages now and then: still looking.
  v = btCardView(link(P::Backoff), s, false);
  TEST_ASSERT_EQUAL(BtCard::Searching, v.card);
  TEST_ASSERT_TRUE(v.spinner);
  TEST_ASSERT_EQUAL_STRING("Looking for SPYDRONE...", btStatusLine(v, link(P::Backoff), "SPYDRONE", "", buf, sizeof(buf)));
  v = btCardView(link(P::Backoff), s, true);
  TEST_ASSERT_EQUAL(BtCard::Lost, v.card);
  TEST_ASSERT_EQUAL_STRING("Lost: looking for SPYDRONE...",
                           btStatusLine(v, link(P::Backoff), "SPYDRONE", "", buf, sizeof(buf)));
}

// A connect's burst that ends in the back-off (or resting) has failed, as
// one that ended in a scan did.
void test_a_burst_that_backs_off_has_failed() {
  using P = BtLink::Phase;
  for (P after : {P::Backoff, P::Resting}) {
    BtSession s;
    s.connect(0);
    s.update(link(P::Paging, true, 1, 3), 100);
    s.update(link(P::Paging, true, 3, 3), 20000);
    TEST_ASSERT_TRUE(s.wanted());
    s.update(link(after), 26000);
    TEST_ASSERT_FALSE(s.wanted());
    TEST_ASSERT_TRUE(s.failed());
    TEST_ASSERT_EQUAL(BtCard::Failed, btCardView(link(after), s, false).card);
  }
  // Asked while resting, before the burst shows: not a failure yet.
  BtSession r;
  r.connect(0);
  r.update(link(P::Resting), 10);
  TEST_ASSERT_TRUE(r.wanted());
  TEST_ASSERT_FALSE(r.failed());
}

// The Pair screen's scan stops by itself after 2 minutes, once.
void test_the_pair_search_stops_after_2_minutes() {
  PairSearch p;
  TEST_ASSERT_FALSE(p.searching());
  TEST_ASSERT_FALSE(p.due(5000));
  p.start(1000);
  TEST_ASSERT_TRUE(p.searching());
  TEST_ASSERT_FALSE(p.due(1000 + PairSearch::kForMs - 1));
  TEST_ASSERT_TRUE(p.due(1000 + PairSearch::kForMs));
  TEST_ASSERT_FALSE(p.searching());
  TEST_ASSERT_FALSE(p.due(1000 + 2 * PairSearch::kForMs));  // once
  p.start(500000);  // Search again: another 2 minutes
  TEST_ASSERT_FALSE(p.due(500000 + PairSearch::kForMs - 1));
  p.stop();  // the page left
  TEST_ASSERT_FALSE(p.due(500000 + PairSearch::kForMs));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_card_for_every_link_state);
  RUN_TEST(test_a_connect_that_runs_out_of_tries_fails);
  RUN_TEST(test_a_connect_that_links_is_satisfied);
  RUN_TEST(test_a_withdrawn_connect_answers_nothing);
  RUN_TEST(test_a_pairing_fails_when_it_stops_or_takes_too_long);
  RUN_TEST(test_an_expected_drop_is_forgotten_by_the_next_link);
  RUN_TEST(test_a_drop_that_never_comes_is_no_longer_expected);
  RUN_TEST(test_pairing_while_linked_waits_for_the_new_link);
  RUN_TEST(test_the_connected_event_answers_whichever_comes_first);
  RUN_TEST(test_a_connect_with_none_remembered_searches_then_fails);
  RUN_TEST(test_forget_needs_a_second_tap_within_3_s);
  RUN_TEST(test_scan_list_keeps_each_device_once_in_the_order_found);
  RUN_TEST(test_scan_list_full_drops_the_weakest_for_a_stronger_one);
  RUN_TEST(test_signal_bars_and_device_kinds);
  RUN_TEST(test_the_resting_card);
  RUN_TEST(test_a_burst_that_backs_off_has_failed);
  RUN_TEST(test_the_pair_search_stops_after_2_minutes);
  return UNITY_END();
}
