// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the format's second step, a hold (docs/HOST-STATUS.md,
// "@format"): the Dialog's hold mode (lib/core/HoldButton: a press kept on
// the red button 2 s, its fill, a lift or a slide that empties it), the
// dialog's buttons (lib/core/DialogLayout, and lib/core/DialogTouch: which
// touch answers what, ui/Overlays' Dialog's own), and the question
// (lib/core/FormatAsk: each step's minute, the second's settle, a step's
// answers, a loop pass's order). Then the two steps as ui/Ui routes the
// glass to them (Asker: the real DialogTouch and formatask; Ui's routing
// modelled, and kept in step with ui/Ui.cpp), a finger on the real
// TouchRecognizer every loop pass: a hold, an early release, a slide off,
// touch noise (jitter, drift, a lift read for a moment), the minute, and
// no tap, double tap or run of taps ever answering both.
// Run: pio test -e native -f test_hold_button
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <string>

#include "DialogLayout.h"
#include "DialogTouch.h"
#include "FormatAsk.h"
#include "HoldButton.h"
#include "InputEvent.h"
#include "TouchRecognizer.h"

void setUp() {}
void tearDown() {}

using T = InputEvent::Type;
using News = HoldButton::News;

namespace {

InputEvent ev(T type, uint32_t ms, int x, int y) {
  InputEvent e;
  e.type = type;
  e.ms = ms;
  e.x = static_cast<int16_t>(x);
  e.y = static_cast<int16_t>(y);
  if (x >= 319) e.edges = InputEvent::kEdgeRight;
  return e;
}

// The buttons' centres on the screen (two buttons): the first question's
// Format... is the right-hand one; the second question's Format, the hold
// button, the left-hand one (where the first's Cancel was).
constexpr int kBw = dialog::buttonW(2);
constexpr int kLeftX = dialog::kX + dialog::buttonX(0, 2) + kBw / 2;
constexpr int kRightX = dialog::kX + dialog::buttonX(1, 2) + kBw / 2;
constexpr int kButtonY = dialog::kY + dialog::kButtonTop + dialog::kButtonH / 2;

// A small deterministic generator (the noise and the taps).
struct Lcg {
  uint32_t s;
  explicit Lcg(uint32_t seed) : s(seed * 2654435761u + 1) {}
  uint32_t next() {
    s = s * 1664525u + 1013904223u;
    return s >> 8;
  }
  int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<uint32_t>(hi - lo + 1)); }
};

}  // namespace

// ---- the dialog's buttons ----

// dialog::buttonAt() is the rule Dialog::buttonAt() had (the card's last
// 50 rows, its halves, the right one to the screen's edge), and each
// button's hitArea() is exactly where buttonAt() says it.
void test_the_buttons_hit_areas() {
  for (int n = 1; n <= 2; ++n) {
    for (int y = 0; y < 240; ++y) {
      for (int x = 0; x < 320; ++x) {
        int old = -1;
        if (!(y < dialog::kY + dialog::kH - 50 || y >= dialog::kY + dialog::kH) && x >= dialog::kX) {
          const int half = (x - dialog::kX) * 2 / dialog::kW;
          old = n == 1 ? 0 : (half < 1 ? half : 1);
        }
        const int now = dialog::buttonAt(x, y, n);
        TEST_ASSERT_EQUAL_INT(old, now);
        int in = -1;
        for (int i = 0; i < n; ++i) {
          const dialog::Area a = dialog::hitArea(i, n);
          if (x >= a.x0 && x < a.x1 && y >= a.y0 && y < a.y1) {
            TEST_ASSERT_EQUAL_INT(-1, in);  // never two
            in = i;
          }
        }
        TEST_ASSERT_EQUAL_INT(now, in);
      }
    }
  }
  // The drawn buttons sit inside their hit areas; the right one's reaches
  // the edge.
  for (int i = 0; i < 2; ++i) {
    const dialog::Area a = dialog::hitArea(i, 2);
    const int x0 = dialog::kX + dialog::buttonX(i, 2), y0 = dialog::kY + dialog::kButtonTop;
    TEST_ASSERT_TRUE(x0 >= a.x0 && x0 + kBw <= a.x1);
    TEST_ASSERT_TRUE(y0 >= a.y0 && y0 + dialog::kButtonH <= a.y1);
  }
  TEST_ASSERT_FALSE(dialog::hitArea(0, 2).toEdge);
  TEST_ASSERT_TRUE(dialog::hitArea(1, 2).toEdge);
  TEST_ASSERT_EQUAL_INT(131, kBw);
}

// ---- HoldButton ----

void test_a_hold_kept_to_its_end() {
  HoldButton h;
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Down, 1000, kLeftX, kButtonY)));  // not armed
  h.arm(dialog::hitArea(0, 2));
  TEST_ASSERT_TRUE(h.armed());
  // Off the button: nothing.
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Down, 1000, kRightX, kButtonY)));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL(News::None, h.update(5000));
  // On it: the fill, then Confirmed once, at 2 s exactly.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 10000, kLeftX, kButtonY)));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL_INT(0, h.fill(10000, kBw));
  TEST_ASSERT_EQUAL_INT(kBw / 2, h.fill(11000, kBw));
  TEST_ASSERT_EQUAL_UINT32(1000, h.heldMs(11000));
  TEST_ASSERT_EQUAL(News::None, h.update(11999));
  TEST_ASSERT_EQUAL_INT(kBw - 1, h.fill(11999, kBw));
  TEST_ASSERT_EQUAL(News::Confirmed, h.update(12000));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_UINT32(2000, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, h.update(12010));  // once
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 12500, kLeftX, kButtonY)));  // its lift: nothing more
  TEST_ASSERT_EQUAL_INT(0, h.fill(12500, kBw));
  // A lift seen at the end before the loop's pass saw it: the finger was
  // on it that long.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Confirmed, h.onEvent(ev(T::Tap, 22000, kLeftX, kButtonY)));
  // The fill's ends: never past the width, never negative.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_INT(kBw, h.fill(40000, kBw));
  TEST_ASSERT_EQUAL_INT(0, h.fill(29000, kBw));  // a time before the press: none yet
  TEST_ASSERT_EQUAL_INT(0, h.fill(31000, 0));
}

void test_a_hold_let_go_early() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // A quick tap, a slow one, one a pass short of the end: Short each, and
  // each the next press starts from nothing.
  const uint32_t lengths[] = {40, 300, 1200, 1999};
  uint32_t t = 1000;
  for (uint32_t len : lengths) {
    TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, t, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL(News::None, h.update(t + len));
    TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, t + len, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL_UINT32(len, h.lastHeldMs());
    TEST_ASSERT_FALSE(h.holding());
    TEST_ASSERT_EQUAL(News::None, h.update(t + 5000));  // nothing left running
    t += 10000;
  }
  // A Release (the lift after a recogniser's long press) is a lift too.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, t, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Release, t + 700, kLeftX, kButtonY)));
  // An update whose time was taken before the press's: no wrap, no end.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, t + 1000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(t + 990));
  TEST_ASSERT_TRUE(h.holding());
}

void test_a_hold_slid_off() {
  HoldButton h;
  const dialog::Area a = dialog::hitArea(0, 2);  // x 14-159, y 172-221
  h.arm(a);
  constexpr int s = HoldButton::kSlackPx;
  // The slack around the area: on, then off by a pixel, each way.
  TEST_ASSERT_TRUE(h.on(a.x1 - 1 + s, kButtonY, s));
  TEST_ASSERT_FALSE(h.on(a.x1 + s, kButtonY, s));
  TEST_ASSERT_TRUE(h.on(a.x0 - s, kButtonY, s));
  TEST_ASSERT_FALSE(h.on(a.x0 - s - 1, kButtonY, s));
  TEST_ASSERT_TRUE(h.on(kLeftX, a.y0 - s, s));
  TEST_ASSERT_FALSE(h.on(kLeftX, a.y0 - s - 1, s));
  TEST_ASSERT_TRUE(h.on(kLeftX, a.y1 - 1 + s, s));
  TEST_ASSERT_FALSE(h.on(kLeftX, a.y1 + s, s));
  // A press lands only on the area itself.
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Down, 100, a.x1, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 200, a.x1 - 1, kButtonY)));
  // Moves on it (and in its slack) keep it; one past the slack ends it.
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 300, a.x1 + 5, kButtonY + 8)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragMove, 400, a.x0 - 3, a.y1 + 2)));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragMove, 900, kRightX, kButtonY)));  // onto Cancel
  TEST_ASSERT_EQUAL_UINT32(700, h.lastHeldMs());
  TEST_ASSERT_FALSE(h.holding());
  // Back onto it, the same touch: nothing (only a press that lands holds).
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragMove, 1000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(5000));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragEnd, 5000, kLeftX, kButtonY)));
  // Off the top, towards the body; and a drag that ends off it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 6000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragStart, 6100, kLeftX, 140)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 7000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragEnd, 7300, kRightX, kButtonY)));
  // A drag that ends on it, early: Short; at its end: Confirmed.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 8000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 8100, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::DragEnd, 8500, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 9000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 9100, kLeftX - 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Confirmed, h.onEvent(ev(T::DragEnd, 11000, kLeftX - 20, kButtonY)));
  // The right-hand button reaches the screen's edge: a clamped reading
  // there is on it, wherever the finger was.
  HoldButton r;
  r.arm(dialog::hitArea(1, 2));
  TEST_ASSERT_EQUAL(News::Began, r.onEvent(ev(T::Down, 100, kRightX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, r.onEvent(ev(T::DragMove, 200, 319, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, r.onEvent(ev(T::DragMove, 300, 400, kButtonY)));  // a correction past it
  TEST_ASSERT_EQUAL(News::Confirmed, r.update(2100));
  TEST_ASSERT_TRUE(r.on(400, kButtonY, 0));
  TEST_ASSERT_FALSE(h.on(400, kButtonY, s));  // (the left one has an edge of its own)
}

void test_a_hold_dropped_or_never_begun() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // The touch cancelled (a screen took it): Dropped, and nothing after.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 100, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Dropped, h.onEvent(ev(T::Cancel, 900, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(0, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, h.update(5000));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 5000, kLeftX, kButtonY)));
  // drop(): the Ui's (the dark, a banner); none with nothing under way.
  TEST_ASSERT_EQUAL(News::None, h.drop());
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 6000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Dropped, h.drop());
  TEST_ASSERT_EQUAL(News::None, h.update(9000));
  // A swipe up from the button strip is no press: it never begins one.
  InputEvent strip = ev(T::Down, 10000, kLeftX, kButtonY);
  strip.fromStrip = true;
  TEST_ASSERT_EQUAL(News::None, h.onEvent(strip));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 10000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(13000));
  // Armed again (another dialog), or disarmed (closed): a hold under way
  // ends with nothing.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  h.arm(dialog::hitArea(0, 2));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL(News::None, h.update(23000));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  h.disarm();
  TEST_ASSERT_FALSE(h.armed());
  TEST_ASSERT_EQUAL(News::None, h.update(33000));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Down, 40000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_STRING("slid off", HoldButton::name(News::Slid));
}

// ---- the question's clock ----

void test_the_question_clock() {
  formatask::Question q;
  TEST_ASSERT_FALSE(q.up());
  TEST_ASSERT_FALSE(q.expired(1000000));
  TEST_ASSERT_TRUE(q.lands(0));
  q.ask(5000);
  TEST_ASSERT_TRUE(q.step() == formatask::Step::Ask);
  TEST_ASSERT_TRUE(q.lands(5000));  // the first question has no settle
  TEST_ASSERT_FALSE(q.expired(5000 + formatask::kAskMs - 1));
  TEST_ASSERT_TRUE(q.expired(5000 + formatask::kAskMs));
  q.ask(30000);  // asked again while up: its minute as it was
  TEST_ASSERT_EQUAL_UINT32(5000, q.openedMs());
  TEST_ASSERT_TRUE(q.expired(65000));
  // The second: its own minute, and its settle.
  q.sure(40000);
  TEST_ASSERT_TRUE(q.step() == formatask::Step::Sure);
  TEST_ASSERT_FALSE(q.expired(65000));
  TEST_ASSERT_FALSE(q.expired(40000 + formatask::kAskMs - 1));
  TEST_ASSERT_TRUE(q.expired(40000 + formatask::kAskMs));
  TEST_ASSERT_FALSE(q.lands(40000));
  TEST_ASSERT_FALSE(q.lands(40000 + formatask::kSettleMs - 1));
  TEST_ASSERT_TRUE(q.lands(40000 + formatask::kSettleMs));
  TEST_ASSERT_FALSE(q.lands(39990));  // a touch stamped before it opened: in the settle, no wrap
  q.end();
  TEST_ASSERT_FALSE(q.up());
  TEST_ASSERT_FALSE(q.expired(200000));
  q.ask(300000);
  TEST_ASSERT_EQUAL_UINT32(300000, q.openedMs());
  // Across millis()' wrap.
  formatask::Question w;
  w.ask(0xFFFFF000u);
  TEST_ASSERT_FALSE(w.expired(0xFFFFF000u + 59999u));
  TEST_ASSERT_TRUE(w.expired(0xFFFFF000u + 60000u));
}

// ---- the dialog's buttons, as the glass answers them ----

// Every dialog (DialogTouch, ui/Overlays' Dialog's touch): a press on a
// button highlights it, a Tap answers it, a drag, a Cancel or a long
// press's lift lets it go.
void test_a_dialogs_buttons() {
  DialogTouch d;
  d.open(2, -1, false);
  TEST_ASSERT_EQUAL_INT(2, d.n());
  TEST_ASSERT_EQUAL_INT(1, d.primary());  // the last, unless said
  TEST_ASSERT_FALSE(d.holdMode());
  // A tap on the left one: its highlight, then its answer.
  DialogTouch::Result r = d.onEvent(ev(T::Down, 100, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::All);
  TEST_ASSERT_EQUAL_INT(0, d.pressed());
  r = d.onEvent(ev(T::Tap, 150, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, r.answer);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_TRUE(r.news == News::None);
  // The right one, its hit area to the screen's edge.
  d.onEvent(ev(T::Down, 200, 319, kButtonY));
  TEST_ASSERT_EQUAL_INT(1, d.onEvent(ev(T::Tap, 250, 319, kButtonY)).answer);
  // A drag lets it go (redrawn), and its end answers nothing.
  d.onEvent(ev(T::Down, 300, kRightX, kButtonY));
  r = d.onEvent(ev(T::DragStart, 320, kRightX + 20, kButtonY));
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::All);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 400, kRightX + 20, kButtonY)).answer);
  // A long press's lift (Release) and a Cancel: let go, no answer.
  d.onEvent(ev(T::Down, 500, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::LongPress, 1000, kRightX, kButtonY)).answer);
  TEST_ASSERT_EQUAL_INT(1, d.pressed());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Release, 1200, kRightX, kButtonY)).answer);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  d.onEvent(ev(T::Down, 1300, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Cancel, 1310, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::All);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 1320, kLeftX, kButtonY)).answer);
  // Elsewhere on the card (its body): nothing, nothing drawn.
  r = d.onEvent(ev(T::Down, 1400, 160, 120));
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::None);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 1450, 160, 120)).answer);
  // The answer is where the touch landed (the recogniser's Tap is within
  // its slop of it).
  d.onEvent(ev(T::Down, 1500, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 1550, kLeftX + 8, kButtonY)).answer);
  // One button: all of the strip; a count past 2, or a primary out of
  // range, kept in it.
  d.open(1, 0, false);
  d.onEvent(ev(T::Down, 2000, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 2050, kRightX, kButtonY)).answer);
  d.open(3, 7, false);
  TEST_ASSERT_EQUAL_INT(2, d.n());
  TEST_ASSERT_EQUAL_INT(1, d.primary());
  d.open(2, 2, false);
  TEST_ASSERT_EQUAL_INT(1, d.primary());
  d.open(0, 0, false);
  TEST_ASSERT_EQUAL_INT(1, d.n());
}

// The hold mode (the format's second question): the hold button answers
// only to a hold kept to its end; a press on it cut short sets the hint's
// alert (amber); the other button is a button as ever.
void test_a_dialogs_hold_mode() {
  DialogTouch d;
  d.open(2, formatask::kSureFormatButton, true);
  TEST_ASSERT_TRUE(d.holdMode());
  TEST_ASSERT_EQUAL_INT(0, d.primary());
  TEST_ASSERT_FALSE(d.hintAlert());  // (no alert yet: the hint not amber)
  // A tap on it: its hold began (no highlight), then fell short: no
  // answer, the hint's alert, drawn again.
  DialogTouch::Result r = d.onEvent(ev(T::Down, 1000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Began);
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::None);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_TRUE(d.holding());
  r = d.onEvent(ev(T::Tap, 1080, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::All);
  TEST_ASSERT_TRUE(d.hintAlert());
  // A long press's lift there, short of the end: the same.
  d.onEvent(ev(T::Down, 2000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Release, 3500, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  // Slid off onto Cancel and lifted there: Slid, and no Cancel either
  // (the touch began on the hold button: nothing was pressed).
  d.onEvent(ev(T::Down, 4000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::DragStart, 4300, kRightX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Slid);
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::All);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragMove, 4400, kRightX + 5, kButtonY)).answer);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 4500, kRightX + 5, kButtonY)).answer);
  // A Cancel under a hold: Dropped, the hold button drawn empty.
  d.onEvent(ev(T::Down, 5000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Cancel, 5500, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Dropped);
  TEST_ASSERT_TRUE(r.redraw == DialogTouch::Redraw::HoldButton);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  // dropHold(): the same for the Ui's (the dark, a banner); none when
  // nothing is under way.
  d.onEvent(ev(T::Down, 6000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.dropHold() == News::Dropped);
  TEST_ASSERT_TRUE(d.dropHold() == News::None);
  // Kept to its end: tick() at 2 s is the answer, and the lift after it
  // answers nothing more.
  d.onEvent(ev(T::Down, 7000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.tick(8999) == News::None);
  TEST_ASSERT_TRUE(d.tick(9000) == News::Confirmed);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 9050, kLeftX, kButtonY)).answer);
  // Lifted at its end before a pass saw it: the lift is the answer.
  d.onEvent(ev(T::Down, 10000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Tap, 12000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Confirmed);
  TEST_ASSERT_EQUAL_INT(formatask::kSureFormatButton, r.answer);
  // Cancel is a tap button as ever: highlighted, answered.
  r = d.onEvent(ev(T::Down, 13000, kRightX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::None);
  TEST_ASSERT_EQUAL_INT(1, d.pressed());
  TEST_ASSERT_EQUAL_INT(1, d.onEvent(ev(T::Tap, 13050, kRightX, kButtonY)).answer);
  // A touch flagged from the button strip that lands on it (no press: the
  // recogniser makes none such, but a dialog never takes it for one): no
  // hold, and no tap answers the hold button.
  InputEvent strip = ev(T::Down, 13500, kLeftX, kButtonY);
  strip.fromStrip = true;
  TEST_ASSERT_TRUE(d.onEvent(strip).news == News::None);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 13550, kLeftX, kButtonY)).answer);
  // A finger that lands beside the hold button (on the body) and slides
  // onto it: no hold, and no answer at its lift.
  d.onEvent(ev(T::Down, 14000, kLeftX, 120));
  TEST_ASSERT_FALSE(d.holding());
  d.onEvent(ev(T::DragStart, 14100, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 17000, kLeftX, kButtonY)).answer);
  TEST_ASSERT_TRUE(d.tick(17000) == News::None);
  // Opened again: the alert gone; closed, or opened plain: no hold mode.
  d.open(2, formatask::kSureFormatButton, true);
  TEST_ASSERT_FALSE(d.hintAlert());
  d.onEvent(ev(T::Down, 18000, kLeftX, kButtonY));
  d.close();
  TEST_ASSERT_FALSE(d.holdMode());
  TEST_ASSERT_TRUE(d.tick(30000) == News::None);
  d.open(2, 0, false);
  TEST_ASSERT_FALSE(d.holdMode());
  d.onEvent(ev(T::Down, 31000, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 31050, kLeftX, kButtonY)).answer);  // a plain dialog's tap
}

// ---- the question's answers, and a loop pass's order ----

void test_the_answers() {
  using formatask::Answer;
  formatask::Question q;
  // No step up: whatever comes, Declined.
  TEST_ASSERT_TRUE(q.answered(formatask::kAskFormatButton, 100) == Answer::Declined);
  TEST_ASSERT_TRUE(q.answered(formatask::kSureFormatButton, 100) == Answer::Declined);
  // Ask: its Cancel, or closed: Declined, the question over.
  q.ask(1000);
  TEST_ASSERT_TRUE(q.answered(0, 2000) == Answer::Declined);
  TEST_ASSERT_FALSE(q.up());
  q.ask(3000);
  TEST_ASSERT_TRUE(q.answered(-1, 4000) == Answer::Declined);
  TEST_ASSERT_FALSE(q.up());
  // Ask's Format...: Sure, from the answer's own time (the tap's, not
  // the loop pass's before it): its settle and its minute run from there.
  q.ask(5000);
  TEST_ASSERT_TRUE(q.answered(formatask::kAskFormatButton, 7005) == Answer::Second);
  TEST_ASSERT_TRUE(q.step() == formatask::Step::Sure);
  TEST_ASSERT_EQUAL_UINT32(7005, q.openedMs());
  TEST_ASSERT_FALSE(q.lands(7005 + formatask::kSettleMs - 1));
  TEST_ASSERT_TRUE(q.lands(7005 + formatask::kSettleMs));
  TEST_ASSERT_FALSE(q.expired(7005 + formatask::kAskMs - 1));
  TEST_ASSERT_TRUE(q.expired(7005 + formatask::kAskMs));
  // Sure's Cancel (its right-hand button): Declined. Sure's hold button:
  // Format. Either ends it.
  TEST_ASSERT_TRUE(q.answered(1, 8000) == Answer::Declined);
  TEST_ASSERT_FALSE(q.up());
  q.ask(9000);
  q.answered(formatask::kAskFormatButton, 9500);
  TEST_ASSERT_TRUE(q.answered(-1, 9600) == Answer::Declined);
  q.ask(10000);
  q.answered(formatask::kAskFormatButton, 10500);
  TEST_ASSERT_TRUE(q.answered(formatask::kSureFormatButton, 13000) == Answer::Format);
  TEST_ASSERT_FALSE(q.up());
  // Ask's own left-hand button is Cancel, never Format (a double tap's
  // first touch on the left, where Sure's hold button will be).
  q.ask(20000);
  TEST_ASSERT_TRUE(q.answered(formatask::kSureFormatButton, 20100) == Answer::Declined);
}

// formatask::pass(): Ui::loop()'s order. The minute first, even over a
// hold that would end in the same pass; then the hold dropped when its
// touch isn't the dialog's; else ticked to its end.
void test_the_pass_order() {
  using formatask::Pass;
  formatask::Question q;
  DialogTouch d;
  q.ask(0);
  q.answered(formatask::kAskFormatButton, 1000);  // Sure from 1000
  d.open(2, formatask::kSureFormatButton, true);
  // A hold begun 2 s before the minute: its end and the minute in the same
  // pass, the minute wins.
  const uint32_t minute = 1000 + formatask::kAskMs;
  d.onEvent(ev(T::Down, minute - 2000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, q, d, minute - 1, true) == Pass::None);
  TEST_ASSERT_TRUE(d.holding());
  TEST_ASSERT_TRUE(formatask::pass(true, q, d, minute, true) == Pass::Expired);
  TEST_ASSERT_TRUE(d.holding());  // (untouched: the Ui closes the dialog)
  // The question's dialog not up (`asking` false): no minute; the hold
  // goes on to its end.
  TEST_ASSERT_TRUE(formatask::pass(false, q, d, minute, true) == Pass::Confirmed);
  TEST_ASSERT_FALSE(d.holding());
  // The touch not the dialog's (the dark took it): Dropped, once; a pass
  // with nothing under way, None.
  formatask::Question r;
  r.ask(100000);
  r.answered(formatask::kAskFormatButton, 100500);
  d.open(2, formatask::kSureFormatButton, true);
  d.onEvent(ev(T::Down, 102000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 103000, false) == Pass::Dropped);
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 105000, false) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 105000, true) == Pass::None);
  // Held, its touch the dialog's: None until 2 s, then Confirmed.
  d.onEvent(ev(T::Down, 106000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 107999, true) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 108000, true) == Pass::Confirmed);
  // No hold mode (the first question, a plain dialog): only the minute.
  formatask::Question s;
  s.ask(200000);
  DialogTouch plain;
  plain.open(2, -1, false);
  TEST_ASSERT_TRUE(formatask::pass(true, s, plain, 200000 + formatask::kAskMs - 1, false) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, s, plain, 200000 + formatask::kAskMs, false) == Pass::Expired);
}

// ---- the two steps, by finger ----

namespace {

// The format's question as ui/Ui routes the glass to it, a loop pass at a
// time (main.cpp's: Input::update() and its events, then Ui::loop()).
// The dialog's touch (DialogTouch: ui/Overlays' Dialog's), the step's
// answers (formatask::Question::answered(): Ui::formatAnswered()'s) and
// the loop's order (formatask::pass(): Ui::loop()'s) are the real ones.
// Modelled here, and to be kept in step with ui/Ui.cpp (each place there
// names this): Ui::onEvent()'s long press that nothing used, a slow tap
// where it pressed; Ui::route()'s Down (the second question's settle:
// touch_ None) and its touch's end; Ui::holdNews()'s Input::noHold() at
// a hold's start.
struct Asker {
  enum class End { Up, Formatted, Declined };
  TouchRecognizer glass;
  formatask::Question q;
  DialogTouch dialog;
  End end = End::Up;
  std::string why;
  uint32_t endMs = 0;
  bool touchIsDialogs = false;  // Ui's touch_ == TouchOn::Dialog
  bool holdUnused = false;      // Ui's holdUnused_ (a dialog uses no long press)
  InputEvent holdAt;            // Ui's hold_
  int hints = 0, begun = 0, dropped = 0, ignored = 0;

  // Ui::askFormat(): {Cancel, Format...}, the primary the last.
  void open(uint32_t ms) {
    q.ask(ms);
    dialog.open(2, -1, false);
  }
  bool up() const { return end == End::Up; }
  bool sure() const { return up() && q.step() == formatask::Step::Sure; }
  // Ui::answerDialog() and formatAnswered(): the dialog closes, the step's
  // answer; Second, openFormatSure() (the hold mode).
  void answered(int button, uint32_t ms, const char* why) {
    dialog.close();
    switch (q.answered(button, ms)) {
      case formatask::Answer::Second:
        dialog.open(2, formatask::kSureFormatButton, true);
        return;
      case formatask::Answer::Format:
        finish(End::Formatted, why, ms);
        return;
      default:
        finish(End::Declined, why, ms);
        return;
    }
  }
  void finish(End e, const char* w, uint32_t ms) {
    end = e;
    why = w;
    endMs = ms;
  }
  // Ui::route(): the touch's owner at its Down, then the dialog's.
  void route(const InputEvent& e) {
    if (!up()) return;
    if (e.type == T::Down) {
      touchIsDialogs = q.lands(e.ms);
      if (!touchIsDialogs) ++ignored;
    }
    if (!touchIsDialogs) return;
    const DialogTouch::Result r = dialog.onEvent(e);
    if (r.news == News::Began) {
      glass.noHold();  // Ui::holdNews()
      ++begun;
    } else if (r.news == News::Short || r.news == News::Slid) {
      ++hints;
    } else if (r.news == News::Dropped) {
      ++dropped;
    }
    if (r.answer < 0) return;
    const bool held = r.news == News::Confirmed;
    const bool format = q.step() == formatask::Step::Sure && r.answer == formatask::kSureFormatButton;
    answered(r.answer, e.ms, held ? "held to its end (the lift)" : format ? "a TAP on Format" : "Cancel");
  }
  // One pass: the finger as the panel reads it (`down`: a finger on the
  // glass at x, y), its events, then the loop.
  void pass(uint32_t ms, bool down, int x, int y) {
    TouchRecognizer::Sample s;
    s.pressed = down;
    s.x = s.rawX = static_cast<int16_t>(x);
    s.y = s.rawY = static_cast<int16_t>(y);
    if (x >= 319) s.edges = InputEvent::kEdgeRight;
    InputEvent out[TouchRecognizer::kMaxEvents];
    const int n = glass.update(ms, s, out);
    for (int i = 0; i < n; ++i) {
      InputEvent e = out[i];
      // Ui::onEvent(): a long press nothing used, lifted where it pressed,
      // is a slow tap there.
      if (e.type == T::Down) holdUnused = false;
      if (e.type == T::LongPress) {
        route(e);
        holdUnused = touchIsDialogs;
        holdAt = e;
        continue;
      }
      if (e.type == T::Release && holdUnused) {
        holdUnused = false;
        if (std::abs(e.x - holdAt.x) <= 24 && std::abs(e.y - holdAt.y) <= 24) {
          const uint32_t ms = e.ms;
          e = holdAt;
          e.type = T::Tap;
          e.ms = ms;
        }
      }
      route(e);
      const T t = e.type;
      if (t == T::Tap || t == T::Release || t == T::DragEnd || t == T::Cancel) touchIsDialogs = false;
    }
    if (!up()) return;
    switch (formatask::pass(up(), q, dialog, ms, touchIsDialogs)) {
      case formatask::Pass::Expired:
        answered(-1, ms, "no answer in a minute");  // (closeModal(true): onDialog(-1))
        return;
      case formatask::Pass::Dropped:
        ++dropped;
        return;
      case formatask::Pass::Confirmed:
        touchIsDialogs = false;
        answered(dialog.primary(), ms, "held to its end");
        return;
      default:
        return;
    }
  }
  // The screen went dark under the finger (Ui::setDark(): touch_ None).
  void taken() { touchIsDialogs = false; }
};

constexpr uint32_t kPass = 10;

// A finger at (x, y) for `ms`, then lifted (one pass up): the time after.
uint32_t press(Asker& a, uint32_t t, int x, int y, uint32_t ms) {
  for (uint32_t d = 0; d <= ms; d += kPass) a.pass(t + d, true, x, y);
  a.pass(t + ms + kPass, false, 0, 0);
  return t + ms + 2 * kPass;
}

uint32_t rest(Asker& a, uint32_t t, uint32_t ms) {
  for (uint32_t d = 0; d < ms; d += kPass) a.pass(t + d, false, 0, 0);
  return t + ms;
}

// The first question answered: a tap on Format..., and the settle over.
uint32_t toTheSecond(Asker& a, uint32_t t) {
  a.open(t);
  t = rest(a, t + kPass, 800);
  t = press(a, t, kRightX, kButtonY, 80);
  TEST_ASSERT_TRUE(a.sure());
  return rest(a, t, 1000);
}

}  // namespace

void test_two_steps_a_hold_formats() {
  Asker a;
  uint32_t t = toTheSecond(a, 100000);
  // Down at t; the fill; formatted at the pass 2 s after it landed, the
  // finger still on it (its lift goes nowhere).
  const uint32_t down = t;
  for (; t < down + 1990; t += kPass) {
    a.pass(t, true, kLeftX, kButtonY);
    TEST_ASSERT_TRUE(a.sure());
  }
  TEST_ASSERT_EQUAL_INT(kBw * 1980 / 2000, a.dialog.hold().fill(t - kPass, kBw));
  a.pass(t, true, kLeftX, kButtonY);  // down + 1990: not yet
  TEST_ASSERT_TRUE(a.sure());
  t += kPass;
  a.pass(t, true, kLeftX, kButtonY);  // down + 2000
  TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
  TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
  TEST_ASSERT_EQUAL_STRING("held to its end", a.why.c_str());
  TEST_ASSERT_EQUAL_INT(1, a.begun);
  TEST_ASSERT_EQUAL_INT(0, a.hints);
  // No LongPress came of it (the recogniser's hold off): a press all along.
  rest(a, t + kPass, 100);
}

void test_two_steps_an_early_release_shows_the_hint() {
  Asker a;
  uint32_t t = toTheSecond(a, 0);
  // Lifted 1.98 s in (the lift seen at 1.99 s): nothing, the hint.
  t = press(a, t, kLeftX, kButtonY, 1980);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(1, a.hints);
  TEST_ASSERT_FALSE(a.dialog.holding());
  // A quick tap: the same.
  t = press(a, t + 300, kLeftX, kButtonY, 30);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(2, a.hints);
  // Then held: formatted, 2 s from its own landing (nothing carried over).
  const uint32_t down = t + 500;
  press(a, down, kLeftX, kButtonY, 2500);
  TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
  TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
}

void test_two_steps_a_slide_off_cancels_the_hold() {
  Asker a;
  uint32_t t = toTheSecond(a, 0);
  // Held 1 s, then slid onto Cancel and lifted there: the hold ends at the
  // slide, and the lift is no tap on Cancel (the touch began on Format).
  const uint32_t down = t;
  for (; t <= down + 1000; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  for (int x = kLeftX; x <= kRightX; x += 6, t += kPass) a.pass(t, true, x, kButtonY);
  TEST_ASSERT_EQUAL_INT(1, a.hints);
  TEST_ASSERT_FALSE(a.dialog.holding());
  for (uint32_t end = t + 3000; t < end; t += kPass) a.pass(t, true, kRightX, kButtonY);
  a.pass(t, false, 0, 0);
  TEST_ASSERT_TRUE(a.sure());  // neither formatted nor declined
  // Up, towards the body: the same.
  t = rest(a, t + kPass, 200);
  for (uint32_t end = t + 500; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  for (int y = kButtonY; y >= 120; y -= 4, t += kPass) a.pass(t, true, kLeftX, y);
  for (uint32_t end = t + 2500; t < end; t += kPass) a.pass(t, true, kLeftX, 120);
  a.pass(t, false, 0, 0);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(2, a.hints);
  // A slide that stays on it (and its slack): still held, formatted.
  t = rest(a, t + kPass, 200);
  const uint32_t down2 = t;
  for (int i = 0; a.up() && i < 300; ++i, t += kPass) {
    const int x = kLeftX - 60 + (i * 7) % 120;  // 31 to 150: the button and its slack's edge
    a.pass(t, true, x, kButtonY + (i % 2 ? 20 : -20));
  }
  TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
  TEST_ASSERT_EQUAL_UINT32(down2 + 2000, a.endMs);
}

void test_two_steps_touch_noise() {
  // A resting finger's jitter (inside the recogniser's slop: no drag at
  // all), and a shakier one (past it, as drags, but on the button): each
  // formats 2 s after it landed.
  for (int amp : {3, 25}) {
    Asker a;
    Lcg rng(static_cast<uint32_t>(amp));
    uint32_t t = toTheSecond(a, 0);
    const uint32_t down = t;
    for (int i = 0; a.up() && i < 400; ++i, t += kPass) {
      a.pass(t, true, kLeftX + rng.range(-amp, amp), kButtonY + rng.range(-amp / 2, amp / 2));
    }
    TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
    TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
    TEST_ASSERT_EQUAL_INT(0, a.hints);
  }
  // A panel that reads the finger lifted for a pass now and then (every
  // 0.4-1.9 s): each gap ends the hold and the next landing starts from
  // nothing, so a finger kept on for 20 s never formats.
  {
    Asker a;
    Lcg rng(7);
    uint32_t t = toTheSecond(a, 0);
    uint32_t nextGap = t + static_cast<uint32_t>(rng.range(400, 1900));
    for (uint32_t end = t + 20000; t < end; t += kPass) {
      const bool gap = t >= nextGap;
      if (gap) nextGap = t + kPass + static_cast<uint32_t>(rng.range(400, 1900));
      a.pass(t, !gap, kLeftX + rng.range(-3, 3), kButtonY + rng.range(-3, 3));
      TEST_ASSERT_TRUE(a.sure());
    }
    TEST_ASSERT_TRUE(a.hints >= 10);
    TEST_ASSERT_EQUAL_INT(a.hints + 1, a.begun);  // the last one still held
  }
  // A finger drifting slowly off the button's edge (a resting finger that
  // rolls): the hold ends there, never formats.
  {
    Asker a;
    uint32_t t = toTheSecond(a, 0);
    const dialog::Area area = dialog::hitArea(0, 2);
    int x = area.x1 - 4;
    for (int i = 0; i < 300; ++i, t += kPass) {
      if (i % 5 == 0) ++x;  // 20 px a second, to the right
      a.pass(t, true, x, kButtonY);
    }
    a.pass(t, false, 0, 0);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(1, a.hints);
  }
}

void test_two_steps_the_touch_taken() {
  // The dark (or a banner) takes the touch 1 s in: the hold drops, and
  // the finger kept on doesn't format.
  Asker a;
  uint32_t t = toTheSecond(a, 0);
  for (uint32_t end = t + 1000; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  a.taken();
  for (uint32_t end = t + 3000; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  a.pass(t, false, 0, 0);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(1, a.dropped);
  // The recogniser's Cancel (a screen changed under the finger): the same.
  t = rest(a, t + kPass, 100);
  for (uint32_t end = t + 1500; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  a.route(a.glass.cancel(t));
  a.touchIsDialogs = false;
  for (uint32_t end = t + 3000; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
  a.pass(t, false, 0, 0);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(2, a.dropped);
}

void test_two_steps_no_taps_answer_both() {
  // A double tap on Format...: the first question answered, the second
  // touch in the second's settle goes nowhere (neither Cancel nor a hold).
  {
    Asker a;
    a.open(0);
    uint32_t t = press(a, 500, kRightX, kButtonY, 60);
    t = press(a, t + 60, kRightX, kButtonY, 60);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(1, a.ignored);
    // Even held on past the end: the touch is the settle's.
    Asker b;
    b.open(0);
    t = press(b, 500, kRightX, kButtonY, 60);
    press(b, t + 100, kLeftX, kButtonY, 3000);
    TEST_ASSERT_TRUE(b.sure());
    TEST_ASSERT_EQUAL_INT(0, b.begun);
  }
  // Runs of taps anywhere on the two dialogs (the buttons mostly), quick
  // and slow (up to 1.98 s down), with gaps of nothing to 1.5 s, for two
  // minutes each: some decline, none formats.
  int declined = 0, reachedSure = 0, hinted = 0;
  for (uint32_t seed = 1; seed <= 300; ++seed) {
    Asker a;
    Lcg rng(seed);
    a.open(0);
    uint32_t t = kPass;
    bool sawSure = false;
    while (a.up() && t < 120000) {
      int x, y;
      const int where = rng.range(0, 9);
      if (where < 4) {
        x = kRightX + rng.range(-40, 40);  // Format... / the second's Cancel
        y = kButtonY + rng.range(-15, 15);
      } else if (where < 8) {
        x = kLeftX + rng.range(-40, 40);  // the first's Cancel / the hold button
        y = kButtonY + rng.range(-15, 15);
      } else {
        x = rng.range(0, 319);
        y = rng.range(40, 239);
      }
      const uint32_t len = rng.range(0, 3) == 0 ? static_cast<uint32_t>(rng.range(1000, 1980))
                                                 : static_cast<uint32_t>(rng.range(20, 400));
      t = press(a, t, x, y, len / kPass * kPass);
      if (a.sure()) sawSure = true;
      t = rest(a, t, static_cast<uint32_t>(rng.range(0, 150)) * kPass);
    }
    TEST_ASSERT_FALSE_MESSAGE(a.end == Asker::End::Formatted, a.why.c_str());
    if (a.end == Asker::End::Declined) ++declined;
    if (sawSure) ++reachedSure;
    if (a.hints) ++hinted;
  }
  // (The runs did reach the second question and tap its hold button.)
  TEST_ASSERT_TRUE(reachedSure > 100);
  TEST_ASSERT_TRUE(hinted > 50);
  TEST_ASSERT_TRUE(declined > 100);
}

void test_two_steps_the_minute() {
  // The first question unanswered: Cancel at its minute, not a pass before.
  {
    Asker a;
    a.open(1000);
    rest(a, 1000, formatask::kAskMs);
    TEST_ASSERT_TRUE(a.up());
    a.pass(1000 + formatask::kAskMs, false, 0, 0);
    TEST_ASSERT_TRUE(a.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_STRING("no answer in a minute", a.why.c_str());
    TEST_ASSERT_EQUAL_UINT32(1000 + formatask::kAskMs, a.endMs);
  }
  // The second has its own minute, from when it opened.
  {
    Asker a;
    a.open(0);
    rest(a, kPass, 50000);
    uint32_t t = press(a, 50000, kRightX, kButtonY, 50);
    TEST_ASSERT_TRUE(a.sure());
    const uint32_t opened = a.q.openedMs();
    t = rest(a, t, 30000);  // past the first question's minute
    TEST_ASSERT_TRUE(a.sure());
    rest(a, t, opened + formatask::kAskMs - t);
    a.pass(opened + formatask::kAskMs, false, 0, 0);
    TEST_ASSERT_TRUE(a.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_UINT32(opened + formatask::kAskMs, a.endMs);
  }
  // A hold begun a second before the minute: the minute wins, and the
  // finger kept on afterwards does nothing.
  {
    Asker a;
    a.open(0);
    uint32_t t = press(a, kPass, kRightX, kButtonY, 50);
    const uint32_t opened = a.q.openedMs();
    t = rest(a, t, opened + formatask::kAskMs - 1000 - t);
    for (uint32_t end = t + 2500; t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
    a.pass(t, false, 0, 0);
    TEST_ASSERT_TRUE(a.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_UINT32(opened + formatask::kAskMs, a.endMs);
    TEST_ASSERT_EQUAL_INT(1, a.begun);
  }
  // One begun 2.5 s before it: formatted, the minute never came.
  {
    Asker a;
    a.open(0);
    uint32_t t = press(a, kPass, kRightX, kButtonY, 50);
    const uint32_t opened = a.q.openedMs();
    t = rest(a, t, opened + formatask::kAskMs - 2500 - t);
    const uint32_t down = t;
    for (uint32_t end = t + 2500; a.up() && t < end; t += kPass) a.pass(t, true, kLeftX, kButtonY);
    TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
    TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
  }
}

void test_two_steps_cancel_and_the_settle() {
  // The second's Cancel in its settle: ignored; after it, Cancel.
  Asker a;
  a.open(0);
  uint32_t t = press(a, kPass, kRightX, kButtonY, 50);
  const uint32_t opened = a.q.openedMs();
  t = press(a, t + 100, kRightX, kButtonY, 50);
  TEST_ASSERT_TRUE(a.sure());
  TEST_ASSERT_EQUAL_INT(1, a.ignored);
  t = rest(a, t, opened + formatask::kSettleMs - t);
  press(a, t, kRightX, kButtonY, 50);
  TEST_ASSERT_TRUE(a.end == Asker::End::Declined);
  TEST_ASSERT_EQUAL_STRING("Cancel", a.why.c_str());
  // The first's Cancel: declined, no second question; held, even (a long
  // press nothing uses is a slow tap: it is a tap button there).
  Asker b;
  b.open(0);
  press(b, kPass, kLeftX, kButtonY, 2500);
  TEST_ASSERT_TRUE(b.end == Asker::End::Declined);
  TEST_ASSERT_EQUAL_INT(0, b.begun);
  // The first's Format... held: the second question, at its lift; the
  // second's Cancel held: Cancel.
  Asker c;
  c.open(0);
  uint32_t u = press(c, kPass, kRightX, kButtonY, 1500);
  TEST_ASSERT_TRUE(c.sure());
  u = rest(c, u, 1000);
  press(c, u, kRightX, kButtonY, 2500);
  TEST_ASSERT_TRUE(c.end == Asker::End::Declined);
  TEST_ASSERT_EQUAL_STRING("Cancel", c.why.c_str());
  TEST_ASSERT_EQUAL_INT(0, c.begun);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_buttons_hit_areas);
  RUN_TEST(test_a_hold_kept_to_its_end);
  RUN_TEST(test_a_hold_let_go_early);
  RUN_TEST(test_a_hold_slid_off);
  RUN_TEST(test_a_hold_dropped_or_never_begun);
  RUN_TEST(test_the_question_clock);
  RUN_TEST(test_a_dialogs_buttons);
  RUN_TEST(test_a_dialogs_hold_mode);
  RUN_TEST(test_the_answers);
  RUN_TEST(test_the_pass_order);
  RUN_TEST(test_two_steps_a_hold_formats);
  RUN_TEST(test_two_steps_an_early_release_shows_the_hint);
  RUN_TEST(test_two_steps_a_slide_off_cancels_the_hold);
  RUN_TEST(test_two_steps_touch_noise);
  RUN_TEST(test_two_steps_the_touch_taken);
  RUN_TEST(test_two_steps_no_taps_answer_both);
  RUN_TEST(test_two_steps_the_minute);
  RUN_TEST(test_two_steps_cancel_and_the_settle);
  return UNITY_END();
}
