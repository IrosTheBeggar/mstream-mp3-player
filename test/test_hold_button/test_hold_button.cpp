// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the format's second step, a hold (docs/HOST-STATUS.md,
// "@format"): the Dialog's hold mode (lib/core/HoldButton: a press seen on
// the red button 2 s, its fill, a lift or a slide that empties it, a loop
// that loses sight of it, the console's scripted finger), the dialog's
// buttons (lib/core/DialogLayout, and lib/core/DialogTouch: which touch
// answers what, ui/Overlays' Dialog's own), and the question
// (lib/core/FormatAsk: each step's minute, the second's settle, a step's
// answers, a loop pass's order). Then the two steps as ui/Ui routes the
// glass to them (Asker: the real DialogTouch and formatask; Ui's routing
// modelled, and kept in step with ui/Ui.cpp), a finger on the real
// TouchRecognizer every loop pass, the loop's pass after the pass's
// events (the firmware's order) and before them: a hold, an early release,
// a slide off, touch noise (jitter, drift, a lift read for a moment), the
// minute, and no tap, double tap or run of taps ever answering both; the
// loop stalled under a tap, under a hold and over a lift and a landing
// (never the answer, whatever the clock says); the scripted finger held to
// the end (never the answer).
// Run: pio test -e native -f test_hold_button
#include <unity.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

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
using Finger = HoldButton::Finger;

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
// ... of the console's scripted finger.
InputEvent sev(T type, uint32_t ms, int x, int y) {
  InputEvent e = ev(type, ms, x, y);
  e.scripted = true;
  return e;
}

// The buttons' centres on the screen (two buttons): the first question's
// Format... is the right-hand one; the second question's Format, the hold
// button, the left-hand one (where the first's Cancel was).
constexpr int kBw = dialog::buttonW(2);
constexpr int kLeftX = dialog::kX + dialog::buttonX(0, 2) + kBw / 2;
constexpr int kRightX = dialog::kX + dialog::buttonX(1, 2) + kBw / 2;
constexpr int kButtonY = dialog::kY + dialog::kButtonTop + dialog::kButtonH / 2;

// What a pass read of the panel: a hand's finger at (x, y), the scripted
// finger there, or none.
Finger at(int x, int y) {
  Finger f;
  f.down = true;
  f.x = static_cast<int16_t>(x);
  f.y = static_cast<int16_t>(y);
  return f;
}
Finger scriptAt(int x, int y) {
  Finger f = at(x, y);
  f.scripted = true;
  return f;
}
const Finger kUp;                                   // no finger
const Finger kOn = at(kLeftX, kButtonY);            // a hand's, on the left-hand button
const Finger kScript = scriptAt(kLeftX, kButtonY);  // the scripted finger there

// The finger seen every `step` ms from `from` (the first look) to `to` (the
// last): the first news that isn't None and when, or None.
struct Seen {
  News news = News::None;
  uint32_t ms = 0;
};
template <class H>
Seen looksAt(H& h, uint32_t from, uint32_t to, uint32_t step, const Finger& f) {
  Seen s;
  for (uint32_t t = from; static_cast<int32_t>(to - t) >= 0; t += step) {
    s.news = h.update(t, f);
    s.ms = t;
    if (s.news != News::None) return s;
  }
  return s;
}
Seen looks(HoldButton& h, uint32_t from, uint32_t to, uint32_t step, const Finger& f = kOn) {
  return looksAt(h, from, to, step, f);
}
// ... through a dialog (DialogTouch::tick()).
struct Ticked {
  DialogTouch& d;
  News update(uint32_t ms, const Finger& f) { return d.tick(ms, f); }
};
Seen ticks(DialogTouch& d, uint32_t from, uint32_t to, uint32_t step, const Finger& f = kOn) {
  Ticked t{d};
  return looksAt(t, from, to, step, f);
}

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
  TEST_ASSERT_EQUAL(News::None, h.update(5000, at(kRightX, kButtonY)));
  // On it, seen there every 10 ms: the fill, then Confirmed once, at 2 s
  // exactly, the finger still on it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 10000, kLeftX, kButtonY)));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL_INT(0, h.fill(kBw));
  TEST_ASSERT_EQUAL(News::None, looks(h, 10000, 11000, 10).news);
  TEST_ASSERT_EQUAL_INT(kBw / 2, h.fill(kBw));
  TEST_ASSERT_EQUAL_UINT32(1000, h.heldMs());
  TEST_ASSERT_EQUAL(News::None, looks(h, 11010, 11990, 10).news);
  TEST_ASSERT_EQUAL(News::None, h.update(11999, kOn));
  TEST_ASSERT_EQUAL_INT(kBw - 1, h.fill(kBw));
  TEST_ASSERT_EQUAL(News::Confirmed, h.update(12000, kOn));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_UINT32(2000, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, h.update(12010, kOn));  // once
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 12500, kLeftX, kButtonY)));  // its lift: nothing more
  TEST_ASSERT_EQUAL_INT(0, h.fill(kBw));
  // Seen every 30 ms (a busy loop): at the first look at or after 2 s.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  Seen s = looks(h, 20000, 23000, 30);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(22010, s.ms);
  TEST_ASSERT_EQUAL_UINT32(2010, h.lastHeldMs());
  // Seen every 400 ms, and every kGapMs exactly (a loop as slow as the
  // hold allows): still a hold, at 2 s.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  s = looks(h, 30400, 33000, 400);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(32000, s.ms);
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 40000, kLeftX, kButtonY)));
  s = looks(h, 40000 + HoldButton::kGapMs, 43000, HoldButton::kGapMs);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(42000, s.ms);
  // The fill is the time the finger was seen on it, never the clock's:
  // nothing after its landing until a look, then up to that look.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 50000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_INT(0, h.fill(kBw));
  TEST_ASSERT_EQUAL(News::None, h.update(50400, kOn));
  TEST_ASSERT_EQUAL_INT(kBw * 400 / 2000, h.fill(kBw));
  TEST_ASSERT_EQUAL_UINT32(400, h.heldMs());
  // A look whose time was taken before the press's: no time at all, no
  // wrap, no end.
  TEST_ASSERT_EQUAL(News::None, h.update(49990, kOn));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL_UINT32(400, h.heldMs());
  // Its ends: never past the width (a move seen on it at 2 s, before the
  // look that answers), none for no width.
  TEST_ASSERT_EQUAL(News::None, looks(h, 50800, 51600, 400).news);
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 52000, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL_INT(kBw, h.fill(kBw));
  TEST_ASSERT_EQUAL_INT(0, h.fill(0));
  TEST_ASSERT_TRUE(h.holding());  // (no event presses it)
  TEST_ASSERT_EQUAL(News::Confirmed, h.update(52000, at(kLeftX + 20, kButtonY)));
}

// A lift never presses it, however late it is seen (the defect: the hold
// was the wall time from the landing to the pass that saw the lift, so a
// tap with a stalled loop behind it erased the card).
void test_a_lift_never_presses_it() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // A 150 ms tap, the loop stalled 2.5 s, then the pass that sees the
  // lift: Short, the 150 ms it was seen for.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 1000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 1000, 1150, 10).news);
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, 3650, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(150, h.lastHeldMs());
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL(News::None, h.update(3650, kUp));
  TEST_ASSERT_EQUAL(News::None, h.update(9000, kOn));  // nothing left running
  // The same with no look at all between the landing and the lift, the
  // lift seen at 2 s exactly, after it, and a day after it: Short, 0 ms.
  for (uint32_t late : {2000u, 2001u, 5000u, 86400000u}) {
    TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 10000, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, 10000 + late, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL_UINT32(0, h.lastHeldMs());
  }
  // Every kind of lift: a long press's Release, a drag's end on it (Short)
  // and off it (Slid).
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Release, 23000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::DragEnd, 33000, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 40000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragEnd, 43000, kRightX, kButtonY)));
  // A real hold let go a pass before its end (seen to 1.99 s, the lift
  // seen at 2 s): Short. Only a look that sees the finger there answers.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 50000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 50000, 51990, 10).news);
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, 52000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(1990, h.lastHeldMs());
  // ... and the same lift whose pass looked first (the loop's pass before
  // the pass's events): that look sees no finger, and presses nothing.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 60000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 60000, 61990, 10).news);
  TEST_ASSERT_EQUAL(News::None, h.update(62000, kUp));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, 62000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(1990, h.lastHeldMs());
  // A lift whose event never comes (dropped on the way): the looks that
  // see no finger count nothing, and the gap ends it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 70000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 70000, 71000, 10).news);
  TEST_ASSERT_EQUAL(News::None, looks(h, 71010, 71000 + HoldButton::kGapMs, 10, kUp).news);
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL(News::Stalled, h.update(71000 + HoldButton::kGapMs + 10, kUp));
  TEST_ASSERT_EQUAL_UINT32(1000, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, looks(h, 71520, 76000, 10, kOn).news);
}

// Two looks more than kGapMs apart say nothing of the time between them:
// the hold ends there, before anything else is asked.
void test_a_stalled_loop_ends_the_hold() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // A finger down, a 2.5 s gap with no events, then a look that sees a
  // finger there (kept on, or lifted and landed again in the gap: one
  // touch to the recogniser either way): Stalled, never Confirmed.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 1000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 1000, 1150, 10).news);
  TEST_ASSERT_EQUAL(News::Stalled, h.update(3650, kOn));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_UINT32(150, h.lastHeldMs());
  TEST_ASSERT_EQUAL_UINT32(2500, h.lastGapMs());
  TEST_ASSERT_EQUAL_INT(0, h.fill(kBw));
  // The finger kept on after it: nothing (that touch is over for the
  // button; one that lands again starts from nothing).
  TEST_ASSERT_EQUAL(News::None, looks(h, 3660, 9000, 10).news);
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 9010, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 10000, kLeftX, kButtonY)));
  Seen s = looks(h, 10000, 13000, 10);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(12000, s.ms);
  // The gap's own edge: kGapMs apart goes on, a millisecond more ends it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(20000 + HoldButton::kGapMs, kOn));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL(News::Stalled, h.update(20000 + 2 * HoldButton::kGapMs + 1, kOn));
  TEST_ASSERT_EQUAL_UINT32(HoldButton::kGapMs, h.lastHeldMs());
  TEST_ASSERT_EQUAL_UINT32(HoldButton::kGapMs + 1, h.lastGapMs());
  // Before the 2 s test, not after: seen to 1.9 s, the next look at 2.45 s
  // (the finger there, 2.45 s after it landed): Stalled.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 30000, 31900, 10).news);
  TEST_ASSERT_EQUAL(News::Stalled, h.update(32450, kOn));
  TEST_ASSERT_EQUAL_UINT32(1900, h.lastHeldMs());
  // A move on the button is a look too, and held to the same gap: after
  // the stall it ends the hold, it doesn't bridge it (the pass's events
  // before its look), ...
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 40000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 40000, 40150, 10).news);
  TEST_ASSERT_EQUAL(News::Stalled, h.onEvent(ev(T::DragStart, 42650, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(150, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, h.update(42650, at(kLeftX + 20, kButtonY)));
  // ... and the look before them: the same end, said a call sooner.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 50000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 50000, 50150, 10).news);
  TEST_ASSERT_EQUAL(News::Stalled, h.update(52650, at(kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 52650, kLeftX + 20, kButtonY)));
  // Moves within the gap keep it seen (a rolling finger under a busy loop).
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 60000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 60450, kLeftX + 15, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragMove, 60900, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(900, h.heldMs());
  TEST_ASSERT_EQUAL(News::None, h.update(61400, at(kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(61900, at(kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Confirmed, h.update(62000, at(kLeftX + 20, kButtonY)));
  // The stall's lift seen by the look first: Stalled, and the lift's event
  // after it finds nothing.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 70000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Stalled, h.update(72500, kUp));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 72500, kLeftX, kButtonY)));
}

// millis() wraps every 49.7 days: a hold across it is a hold, a gap a gap.
void test_the_clock_wraps() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  const uint32_t down = 0xFFFFFC00u;  // 1.024 s before the wrap
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, down, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, down, down + 1500, 10).news);  // (over the wrap)
  TEST_ASSERT_EQUAL_UINT32(1500, h.heldMs());
  TEST_ASSERT_EQUAL_INT(kBw * 1500 / 2000, h.fill(kBw));
  Seen s = looks(h, down + 1510, down + 3000, 10);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(down + 2000, s.ms);
  TEST_ASSERT_TRUE(s.ms < 2000);  // (past the wrap)
  TEST_ASSERT_EQUAL_UINT32(2000, h.lastHeldMs());
  // A stall over the wrap: Stalled, its length as it was.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, down, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, down, down + 300, 10).news);
  TEST_ASSERT_EQUAL(News::Stalled, h.update(down + 2800, kOn));
  TEST_ASSERT_EQUAL_UINT32(300, h.lastHeldMs());
  TEST_ASSERT_EQUAL_UINT32(2500, h.lastGapMs());
  // A tap over it, its lift read late: Short.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 0xFFFFFFF0u, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(0xFFFFFFFAu, kOn));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, 2600, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(10, h.lastHeldMs());
}

// Only a hand presses it: a hold any part of which was the console's
// scripted finger (its events and its passes say so) ends Scripted where
// it would have been Confirmed.
void test_only_a_hand_presses_it() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // The scripted finger, landing to end.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 1000, kLeftX, kButtonY)));
  Seen s = looks(h, 1000, 4000, 10, kScript);
  TEST_ASSERT_EQUAL(News::Scripted, s.news);
  TEST_ASSERT_EQUAL_UINT32(3000, s.ms);
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL_UINT32(2000, h.lastHeldMs());
  TEST_ASSERT_EQUAL_INT(0, h.fill(kBw));
  TEST_ASSERT_EQUAL(News::None, looks(h, 3010, 6000, 10, kScript).news);  // once, and nothing after
  // Its landing alone is enough (a hand that takes the touch over doesn't
  // make it a hand's): every look after it a hand's.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 10000, kLeftX, kButtonY)));
  s = looks(h, 10000, 13000, 10, kOn);
  TEST_ASSERT_EQUAL(News::Scripted, s.news);
  // A hand's landing, the scripted finger for one look in the middle (it
  // took the touch over as the hand lifted, the recogniser none the
  // wiser), the hand again: Scripted.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 20000, 21000, 10, kOn).news);
  TEST_ASSERT_EQUAL(News::None, h.update(21010, kScript));
  s = looks(h, 21020, 23000, 10, kOn);
  TEST_ASSERT_EQUAL(News::Scripted, s.news);
  TEST_ASSERT_EQUAL_UINT32(22000, s.ms);
  // A hand's landing and looks, one move of the scripted finger's on it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 30000, 30400, 10, kOn).news);
  TEST_ASSERT_EQUAL(News::None, h.onEvent(sev(T::DragStart, 30410, kLeftX + 20, kButtonY)));
  s = looks(h, 30420, 33000, 10, at(kLeftX + 20, kButtonY));
  TEST_ASSERT_EQUAL(News::Scripted, s.news);
  // Cut short it is as any other: Short, Slid, Stalled, Dropped.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 40000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 40000, 41880, 10, kScript).news);
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(sev(T::Tap, 41890, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(1880, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 50000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(sev(T::DragStart, 50500, kRightX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 60000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Stalled, h.update(63000, kScript));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(sev(T::Down, 70000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Dropped, h.drop());
  // The next hold is its own: a hand's, pressed.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 80000, kLeftX, kButtonY)));
  s = looks(h, 80000, 83000, 10, kOn);
  TEST_ASSERT_EQUAL(News::Confirmed, s.news);
  TEST_ASSERT_EQUAL_UINT32(82000, s.ms);
  TEST_ASSERT_EQUAL_STRING("scripted", HoldButton::name(News::Scripted));
  TEST_ASSERT_EQUAL_STRING("stalled", HoldButton::name(News::Stalled));
}

void test_a_hold_let_go_early() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // A quick tap, a slow one, one a pass short of the end: Short each, and
  // each the next press starts from nothing.
  const uint32_t lengths[] = {40, 300, 1200, 1990};
  uint32_t t = 1000;
  for (uint32_t len : lengths) {
    TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, t, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL(News::None, looks(h, t, t + len, 10).news);
    TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Tap, t + len + 10, kLeftX, kButtonY)));
    TEST_ASSERT_EQUAL_UINT32(len, h.lastHeldMs());
    TEST_ASSERT_FALSE(h.holding());
    TEST_ASSERT_EQUAL(News::None, h.update(t + 5000, kOn));  // nothing left running
    t += 10000;
  }
  // A Release (the lift after a recogniser's long press) is a lift too.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, t, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, t, t + 690, 10).news);
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::Release, t + 700, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(690, h.lastHeldMs());
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
  // Moves on it (and in its slack) keep it; one past the slack ends it,
  // at what it was seen for (its last move on it).
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 300, a.x1 + 5, kButtonY + 8)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragMove, 400, a.x0 - 3, a.y1 + 2)));
  TEST_ASSERT_TRUE(h.holding());
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragMove, 900, kRightX, kButtonY)));  // onto Cancel
  TEST_ASSERT_EQUAL_UINT32(200, h.lastHeldMs());
  TEST_ASSERT_FALSE(h.holding());
  // Back onto it, the same touch: nothing (only a press that lands holds).
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragMove, 1000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(5000, kOn));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragEnd, 5000, kLeftX, kButtonY)));
  // Off the top, towards the body; and a drag that ends off it.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 6000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragStart, 6100, kLeftX, 140)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 7000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.onEvent(ev(T::DragEnd, 7300, kRightX, kButtonY)));
  // A drag that ends on it: Short, early or late (no lift presses it).
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 8000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 8100, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::DragEnd, 8500, kLeftX + 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 9000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 9100, kLeftX - 20, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 9110, 10990, 10, at(kLeftX - 20, kButtonY)).news);
  TEST_ASSERT_EQUAL(News::Short, h.onEvent(ev(T::DragEnd, 11000, kLeftX - 20, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(1990, h.lastHeldMs());
  // The look itself sees where the finger is: off the button (a move
  // whose event comes after it, or one inside the recogniser's slop from
  // a landing at the area's edge), Slid; on its slack, on.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 12000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, h.update(12010, at(a.x1 - 1 + s, kButtonY)));
  TEST_ASSERT_EQUAL(News::Slid, h.update(12020, at(a.x1 + s, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(10, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 12020, a.x1 + s, kButtonY)));
  // ... at the end too: 2 s after it landed, the finger on Cancel.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 13000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 13000, 14990, 10).news);
  TEST_ASSERT_EQUAL(News::Slid, h.update(15000, at(kRightX, kButtonY)));
  // The right-hand button reaches the screen's edge: a clamped reading
  // there is on it, wherever the finger was.
  HoldButton r;
  r.arm(dialog::hitArea(1, 2));
  TEST_ASSERT_EQUAL(News::Began, r.onEvent(ev(T::Down, 100, kRightX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, r.onEvent(ev(T::DragMove, 200, 319, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, r.onEvent(ev(T::DragMove, 300, 400, kButtonY)));  // a correction past it
  TEST_ASSERT_EQUAL(News::Confirmed, looks(r, 310, 2100, 10, at(400, kButtonY)).news);
  TEST_ASSERT_TRUE(r.on(400, kButtonY, 0));
  TEST_ASSERT_FALSE(h.on(400, kButtonY, s));  // (the left one has an edge of its own)
}

void test_a_hold_dropped_or_never_begun() {
  HoldButton h;
  h.arm(dialog::hitArea(0, 2));
  // The touch cancelled (a screen took it): Dropped, and nothing after.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 100, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 100, 890, 10).news);
  TEST_ASSERT_EQUAL(News::Dropped, h.onEvent(ev(T::Cancel, 900, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL_UINT32(0, h.lastHeldMs());
  TEST_ASSERT_EQUAL(News::None, looks(h, 910, 5000, 10).news);
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::Tap, 5000, kLeftX, kButtonY)));
  // drop(): the Ui's (the dark, a banner); none with nothing under way.
  TEST_ASSERT_EQUAL(News::None, h.drop());
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 6000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::Dropped, h.drop());
  TEST_ASSERT_EQUAL(News::None, looks(h, 6010, 9000, 10).news);
  // A swipe up from the button strip is no press: it never begins one.
  InputEvent strip = ev(T::Down, 10000, kLeftX, kButtonY);
  strip.fromStrip = true;
  TEST_ASSERT_EQUAL(News::None, h.onEvent(strip));
  TEST_ASSERT_EQUAL(News::None, h.onEvent(ev(T::DragStart, 10000, kLeftX, kButtonY)));
  TEST_ASSERT_EQUAL(News::None, looks(h, 10010, 13000, 10).news);
  // Armed again (another dialog), or disarmed (closed): a hold under way
  // ends with nothing.
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 20000, kLeftX, kButtonY)));
  h.arm(dialog::hitArea(0, 2));
  TEST_ASSERT_FALSE(h.holding());
  TEST_ASSERT_EQUAL(News::None, looks(h, 20010, 23000, 10).news);
  TEST_ASSERT_EQUAL(News::Began, h.onEvent(ev(T::Down, 30000, kLeftX, kButtonY)));
  h.disarm();
  TEST_ASSERT_FALSE(h.armed());
  TEST_ASSERT_EQUAL(News::None, looks(h, 30010, 33000, 10).news);
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
  TEST_ASSERT_EQUAL_INT(0, r.drawButton);  // (that button alone)
  TEST_ASSERT_FALSE(r.drawHint);
  TEST_ASSERT_EQUAL_INT(0, d.pressed());
  r = d.onEvent(ev(T::Tap, 150, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, r.answer);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_TRUE(r.news == News::None);
  TEST_ASSERT_EQUAL_INT(-1, r.drawButton);  // (the answer closes it)
  // The right one, its hit area to the screen's edge.
  d.onEvent(ev(T::Down, 200, 319, kButtonY));
  TEST_ASSERT_EQUAL_INT(1, d.onEvent(ev(T::Tap, 250, 319, kButtonY)).answer);
  // A drag lets it go (redrawn), and its end answers nothing.
  d.onEvent(ev(T::Down, 300, kRightX, kButtonY));
  r = d.onEvent(ev(T::DragStart, 320, kRightX + 20, kButtonY));
  TEST_ASSERT_EQUAL_INT(1, r.drawButton);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 400, kRightX + 20, kButtonY)).answer);
  // A long press's lift (Release) and a Cancel: let go, no answer.
  d.onEvent(ev(T::Down, 500, kRightX, kButtonY));
  r = d.onEvent(ev(T::LongPress, 1000, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(-1, r.drawButton);  // (still pressed: nothing changed)
  TEST_ASSERT_EQUAL_INT(1, d.pressed());
  r = d.onEvent(ev(T::Release, 1200, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(1, r.drawButton);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  d.onEvent(ev(T::Down, 1300, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Cancel, 1310, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, r.drawButton);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 1320, kLeftX, kButtonY)).answer);
  // Elsewhere on the card (its body): nothing, nothing drawn.
  r = d.onEvent(ev(T::Down, 1400, 160, 120));
  TEST_ASSERT_EQUAL_INT(-1, r.drawButton);
  TEST_ASSERT_FALSE(r.drawHint);
  // ... and a drag from there lets nothing go.
  r = d.onEvent(ev(T::DragStart, 1420, 180, 120));
  TEST_ASSERT_EQUAL_INT(-1, r.drawButton);
  d.onEvent(ev(T::Down, 1430, 160, 120));
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 1450, 160, 120)).answer);
  // The answer is where the touch landed (the recogniser's Tap is within
  // its slop of it).
  d.onEvent(ev(T::Down, 1500, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 1550, kLeftX + 8, kButtonY)).answer);
  // A plain dialog has no hold: its looks do nothing, whatever they see,
  // and the scripted finger's tap answers as a hand's.
  TEST_ASSERT_TRUE(d.tick(1600, kOn) == News::None);
  TEST_ASSERT_TRUE(d.tick(9000, kScript) == News::None);
  d.onEvent(sev(T::Down, 9100, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(sev(T::Tap, 9150, kLeftX, kButtonY)).answer);
  // One button: all of the strip; a count past 2, or a primary out of
  // range, kept in it.
  d.open(1, 0, false);
  d.onEvent(ev(T::Down, 12000, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 12050, kRightX, kButtonY)).answer);
  d.open(3, 7, false);
  TEST_ASSERT_EQUAL_INT(2, d.n());
  TEST_ASSERT_EQUAL_INT(1, d.primary());
  d.open(2, 2, false);
  TEST_ASSERT_EQUAL_INT(1, d.primary());
  d.open(0, 0, false);
  TEST_ASSERT_EQUAL_INT(1, d.n());
}

// The hold mode (the format's second question): the hold button answers
// only to a hold seen to its end, by a look (tick()), never to an event;
// a press on it that came to nothing sets the hint's alert (amber); the
// other button is a button as ever.
void test_a_dialogs_hold_mode() {
  constexpr int kHold = formatask::kSureFormatButton;
  DialogTouch d;
  d.open(2, kHold, true);
  TEST_ASSERT_TRUE(d.holdMode());
  TEST_ASSERT_EQUAL_INT(0, d.primary());
  TEST_ASSERT_FALSE(d.hintAlert());  // (no alert yet: the hint not amber)
  // A tap on it: its hold began (no highlight), then fell short: no
  // answer, the hint's alert, its row and the hold button drawn again, each
  // alone (never the whole dialog).
  DialogTouch::Result r = d.onEvent(ev(T::Down, 1000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Began);
  TEST_ASSERT_EQUAL_INT(-1, r.drawButton);  // (its fill: the dialog's tick())
  TEST_ASSERT_FALSE(r.drawHint);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_TRUE(d.holding());
  r = d.onEvent(ev(T::Tap, 1080, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  TEST_ASSERT_TRUE(r.drawHint);
  TEST_ASSERT_TRUE(d.hintAlert());
  // A long press's lift there, short of the end: the same, the hint amber
  // already (its row not drawn again).
  d.onEvent(ev(T::Down, 2000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Release, 3500, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  TEST_ASSERT_FALSE(r.drawHint);
  TEST_ASSERT_TRUE(d.hintAlert());
  // Slid off onto Cancel and lifted there: Slid, and no Cancel either
  // (the touch began on the hold button: nothing was pressed).
  d.onEvent(ev(T::Down, 4000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::DragStart, 4300, kRightX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Slid);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  TEST_ASSERT_FALSE(r.drawHint);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragMove, 4400, kRightX + 5, kButtonY)).answer);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 4500, kRightX + 5, kButtonY)).answer);
  // A Cancel under a hold: Dropped, the hold button drawn empty.
  d.onEvent(ev(T::Down, 5000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Cancel, 5500, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Dropped);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  TEST_ASSERT_FALSE(r.drawHint);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  // dropHold(): the same for the Ui's (the dark, a banner); none when
  // nothing is under way.
  d.onEvent(ev(T::Down, 6000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.dropHold() == News::Dropped);
  TEST_ASSERT_TRUE(d.dropHold() == News::None);
  // Seen to its end: the look at 2 s is the answer, and the lift after it
  // answers nothing more.
  d.onEvent(ev(T::Down, 7000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(ticks(d, 7000, 8990, 10).news == News::None);
  TEST_ASSERT_TRUE(d.tick(8999, kOn) == News::None);
  TEST_ASSERT_TRUE(d.tick(9000, kOn) == News::Confirmed);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 9050, kLeftX, kButtonY)).answer);
  // No event answers it. A lift 2 s after it landed that no look saw
  // first (the loop stalled under a tap): Short, no answer.
  d.onEvent(ev(T::Down, 10000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::Tap, 12000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  // ... and one a pass after the last look, at 2 s: Short, no answer.
  d.onEvent(ev(T::Down, 13000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(ticks(d, 13000, 14990, 10).news == News::None);
  r = d.onEvent(ev(T::Tap, 15000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Short);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  // The loop lost sight of it (a look more than kGapMs after the last):
  // Stalled, nothing answered, the hint's alert on.
  d.open(2, kHold, true);
  TEST_ASSERT_FALSE(d.hintAlert());
  d.onEvent(ev(T::Down, 16000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.tick(16010, kOn) == News::None);
  TEST_ASSERT_TRUE(d.tick(16600, kOn) == News::Stalled);
  TEST_ASSERT_TRUE(d.hintAlert());
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_TRUE(ticks(d, 16610, 20000, 10).news == News::None);
  // ... told by a move's event first: the same, with its draw orders.
  d.open(2, kHold, true);
  d.onEvent(ev(T::Down, 21000, kLeftX, kButtonY));
  r = d.onEvent(ev(T::DragStart, 21600, kLeftX + 20, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::Stalled);
  TEST_ASSERT_EQUAL_INT(-1, r.answer);
  TEST_ASSERT_EQUAL_INT(kHold, r.drawButton);
  TEST_ASSERT_TRUE(r.drawHint);
  TEST_ASSERT_TRUE(d.hintAlert());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 24000, kLeftX + 20, kButtonY)).answer);
  // The scripted finger held to the end: Scripted, nothing answered, the
  // alert; its lift after it nothing.
  d.open(2, kHold, true);
  d.onEvent(sev(T::Down, 25000, kLeftX, kButtonY));
  const Seen scripted = ticks(d, 25000, 28000, 10, kScript);
  TEST_ASSERT_TRUE(scripted.news == News::Scripted);
  TEST_ASSERT_EQUAL_UINT32(27000, scripted.ms);
  TEST_ASSERT_TRUE(d.hintAlert());
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(sev(T::Tap, 27300, kLeftX, kButtonY)).answer);
  // Seen off the button by the look itself: Slid, the alert.
  d.open(2, kHold, true);
  d.onEvent(ev(T::Down, 29000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.tick(29010, at(kRightX, kButtonY)) == News::Slid);
  TEST_ASSERT_TRUE(d.hintAlert());
  // Cancel is a tap button as ever: highlighted, answered; by the scripted
  // finger too (it declines as a finger does).
  r = d.onEvent(ev(T::Down, 30000, kRightX, kButtonY));
  TEST_ASSERT_TRUE(r.news == News::None);
  TEST_ASSERT_EQUAL_INT(1, r.drawButton);  // (that one alone, the hold button left as it is)
  TEST_ASSERT_EQUAL_INT(1, d.pressed());
  TEST_ASSERT_EQUAL_INT(1, d.onEvent(ev(T::Tap, 30050, kRightX, kButtonY)).answer);
  d.onEvent(sev(T::Down, 30500, kRightX, kButtonY));
  TEST_ASSERT_EQUAL_INT(1, d.onEvent(sev(T::Tap, 30550, kRightX, kButtonY)).answer);
  // A touch flagged from the button strip that lands on it (no press: the
  // recogniser makes none such, but a dialog never takes it for one): no
  // hold, and no tap answers the hold button.
  InputEvent strip = ev(T::Down, 31500, kLeftX, kButtonY);
  strip.fromStrip = true;
  TEST_ASSERT_TRUE(d.onEvent(strip).news == News::None);
  TEST_ASSERT_EQUAL_INT(-1, d.pressed());
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::Tap, 31550, kLeftX, kButtonY)).answer);
  // A finger that lands beside the hold button (on the body) and slides
  // onto it: no hold, and no answer at its lift or from the looks.
  d.onEvent(ev(T::Down, 32000, kLeftX, 120));
  TEST_ASSERT_FALSE(d.holding());
  d.onEvent(ev(T::DragStart, 32100, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(ticks(d, 32110, 35000, 10).news == News::None);
  TEST_ASSERT_EQUAL_INT(-1, d.onEvent(ev(T::DragEnd, 35000, kLeftX, kButtonY)).answer);
  // Opened again: the alert gone (the first press cut short says so again);
  // closed, or opened plain: no hold mode.
  d.open(2, kHold, true);
  TEST_ASSERT_FALSE(d.hintAlert());
  d.onEvent(ev(T::Down, 35500, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(d.onEvent(ev(T::Tap, 35550, kLeftX, kButtonY)).drawHint);
  d.onEvent(ev(T::Down, 36000, kLeftX, kButtonY));
  d.close();
  TEST_ASSERT_FALSE(d.holdMode());
  TEST_ASSERT_TRUE(ticks(d, 36010, 40000, 10).news == News::None);
  d.open(2, 0, false);
  TEST_ASSERT_FALSE(d.holdMode());
  d.onEvent(ev(T::Down, 41000, kLeftX, kButtonY));
  TEST_ASSERT_EQUAL_INT(0, d.onEvent(ev(T::Tap, 41050, kLeftX, kButtonY)).answer);  // a plain dialog's tap
}

// What a press costs: the dialog draws only what the event says, each control
// alone over the card as rendered (ui/Overlays' Dialog::drawButton(),
// drawHint(); the whole card was 44 ms a press). So what those orders leave on
// the screen has to be what the dialog's state says, after every event of any
// touch and every look: a button's press look, the hint's colour. And no
// event of any touch answers the hold button.
void test_a_dialog_draws_each_control_alone() {
  for (int hold = 0; hold <= 1; ++hold) {
    Lcg rng(static_cast<uint32_t>(7 + hold));
    DialogTouch d;
    d.open(2, hold ? formatask::kSureFormatButton : -1, hold != 0);
    int lit = -1;        // the button the screen shows in its press look
    bool amber = false;  // the hint as the screen shows it
    uint32_t ms = 1000;
    int answers = 0;
    const auto step = [&](T type, int x, int y) {
      const DialogTouch::Result r = d.onEvent(ev(type, ms, x, y));
      TEST_ASSERT_TRUE(r.drawButton >= -1 && r.drawButton < d.n());
      TEST_ASSERT_TRUE(r.news != News::Confirmed && r.news != News::Scripted);
      if (hold) TEST_ASSERT_TRUE(r.answer != formatask::kSureFormatButton);
      if (r.drawButton >= 0) {
        if (d.pressed() == r.drawButton) {
          lit = r.drawButton;
        } else if (lit == r.drawButton) {
          lit = -1;
        }
      }
      if (r.drawHint) amber = d.hintAlert();
      if (r.answer >= 0) {  // (answered: the dialog closes, whatever it shows)
        ++answers;
        d.open(2, hold ? formatask::kSureFormatButton : -1, hold != 0);
        lit = -1;
        amber = false;
      }
      TEST_ASSERT_EQUAL_INT(d.pressed(), lit);
      TEST_ASSERT_EQUAL(d.hintAlert(), amber);
    };
    // The loop's look (ui/Overlays' Dialog::tick(): the hold button drawn
    // empty and the hint's row, when the hold ended in it with nothing done).
    const auto look = [&](const Finger& f) {
      const bool was = d.hintAlert();
      const News n = d.tick(ms, f);
      if (n == News::Stalled || n == News::Slid || n == News::Scripted) {
        if (d.hintAlert() != was) amber = d.hintAlert();
      } else if (n == News::Confirmed) {  // (the hold button's answer: the dialog closes)
        d.open(2, hold ? formatask::kSureFormatButton : -1, hold != 0);
        lit = -1;
        amber = false;
      }
      TEST_ASSERT_EQUAL_INT(d.pressed(), lit);
      TEST_ASSERT_EQUAL(d.hintAlert(), amber);
    };
    for (int i = 0; i < 2000; ++i) {
      static const int kXs[4] = {kLeftX, kRightX, kRightX + 60, 160};
      const int x = kXs[rng.range(0, 3)];
      const int y = rng.range(0, 3) == 0 ? 120 : kButtonY;  // (1 in 4 on the card's body)
      step(T::Down, x, y);
      ms += static_cast<uint32_t>(rng.range(10, 1900));  // (short of a hold's end)
      if (rng.range(0, 2) == 0) look(rng.range(0, 3) == 0 ? kUp : rng.range(0, 3) == 0 ? scriptAt(x, y) : at(x, y));
      switch (rng.range(0, 4)) {
        case 0: step(T::Tap, x, y); break;
        case 1:
          step(T::LongPress, x, y);
          step(T::Release, x, y);
          break;
        case 2: step(T::Cancel, x, y); break;
        case 3:  // slid off, lifted elsewhere
          step(T::DragStart, x, 60);
          step(T::DragMove, x, 58);
          step(T::DragEnd, x, 58);
          break;
        default:  // moved on it a little, then lifted
          step(T::DragStart, x + 3, y);
          step(T::DragEnd, x + 3, y);
          break;
      }
      ms += static_cast<uint32_t>(rng.range(10, 300));
    }
    TEST_ASSERT_TRUE(answers > 100);  // (the other buttons were answered all along)
  }
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
// touch isn't the dialog's; else looked at, to its end or to what ends it
// with nothing done.
void test_the_pass_order() {
  using formatask::Pass;
  formatask::Question q;
  DialogTouch d;
  q.ask(0);
  q.answered(formatask::kAskFormatButton, 1000);  // Sure from 1000
  d.open(2, formatask::kSureFormatButton, true);
  // A hold begun 2 s before the minute, seen all along: its end and the
  // minute in the same pass, the minute wins.
  const uint32_t minute = 1000 + formatask::kAskMs;
  d.onEvent(ev(T::Down, minute - 2000, kLeftX, kButtonY));
  for (uint32_t t = minute - 2000; t < minute; t += 10)
    TEST_ASSERT_TRUE(formatask::pass(true, q, d, t, true, kOn) == Pass::None);
  TEST_ASSERT_TRUE(d.holding());
  TEST_ASSERT_TRUE(formatask::pass(true, q, d, minute, true, kOn) == Pass::Expired);
  TEST_ASSERT_TRUE(d.holding());  // (untouched: the Ui closes the dialog)
  // The question's dialog not up (`asking` false): no minute; the hold
  // goes on to its end.
  TEST_ASSERT_TRUE(formatask::pass(false, q, d, minute, true, kOn) == Pass::Confirmed);
  TEST_ASSERT_FALSE(d.holding());
  // The touch not the dialog's (the dark took it): Dropped, once; a pass
  // with nothing under way, None.
  formatask::Question r;
  r.ask(100000);
  r.answered(formatask::kAskFormatButton, 100500);
  d.open(2, formatask::kSureFormatButton, true);
  d.onEvent(ev(T::Down, 102000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 102400, false, kOn) == Pass::Dropped);
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 105000, false, kOn) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 105000, true, kOn) == Pass::None);
  // Held, its touch the dialog's, seen every pass: None until 2 s, then
  // Confirmed.
  d.onEvent(ev(T::Down, 106000, kLeftX, kButtonY));
  for (uint32_t t = 106000; t < 108000; t += 10)
    TEST_ASSERT_TRUE(formatask::pass(true, r, d, t, true, kOn) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 107999, true, kOn) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 108000, true, kOn) == Pass::Confirmed);
  // A pass that comes too late: Stalled. One that sees the finger off the
  // button: Slid. The scripted finger's hold at its end: Scripted. Each
  // ends the hold with nothing answered.
  d.open(2, formatask::kSureFormatButton, true);
  d.onEvent(ev(T::Down, 110000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 110700, true, kOn) == Pass::Stalled);
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_TRUE(d.hintAlert());
  d.onEvent(ev(T::Down, 111000, kLeftX, kButtonY));
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 111010, true, at(kRightX, kButtonY)) == Pass::Slid);
  TEST_ASSERT_FALSE(d.holding());
  d.onEvent(sev(T::Down, 112000, kLeftX, kButtonY));
  for (uint32_t t = 112000; t < 114000; t += 10)
    TEST_ASSERT_TRUE(formatask::pass(true, r, d, t, true, kScript) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 114000, true, kScript) == Pass::Scripted);
  TEST_ASSERT_FALSE(d.holding());
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 114010, true, kScript) == Pass::None);
  // A pass that sees no finger (its lift's event still to come): None,
  // 2 s after it landed too; the hold as it was, for that event to end.
  d.onEvent(ev(T::Down, 115000, kLeftX, kButtonY));
  for (uint32_t t = 115000; t < 117000; t += 10)
    TEST_ASSERT_TRUE(formatask::pass(true, r, d, t, true, kOn) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, r, d, 117000, true, kUp) == Pass::None);
  TEST_ASSERT_TRUE(d.holding());
  TEST_ASSERT_TRUE(d.onEvent(ev(T::Tap, 117000, kLeftX, kButtonY)).news == News::Short);
  // No hold mode (the first question, a plain dialog): only the minute.
  formatask::Question s;
  s.ask(200000);
  DialogTouch plain;
  plain.open(2, -1, false);
  TEST_ASSERT_TRUE(formatask::pass(true, s, plain, 200000 + formatask::kAskMs - 1, false, kUp) == Pass::None);
  TEST_ASSERT_TRUE(formatask::pass(true, s, plain, 200000 + formatask::kAskMs, false, kUp) == Pass::Expired);
}

// ---- the two steps, by finger ----

namespace {

// The format's question as ui/Ui routes the glass to it, a loop pass at a
// time (main.cpp's: Input::update() and its events, then Ui::loop()).
// The dialog's touch (DialogTouch: ui/Overlays' Dialog's), the step's
// answers (formatask::Question::answered(): Ui::formatAnswered()'s) and
// the loop's order (formatask::pass(): Ui::loop()'s, with what the pass
// read of the panel: Input::finger()) are the real ones.
// Modelled here, and to be kept in step with ui/Ui.cpp (each place there
// names this): Ui::onEvent()'s long press that nothing used, a slow tap
// where it pressed; Ui::route()'s Down (the second question's settle:
// touch_ None) and its touch's end; Ui::holdNews()'s Input::noHold() at
// a hold's start. `loopFirst`: the loop's pass before the pass's events,
// not the firmware's order: what comes of a touch is the same either way.
struct Asker {
  enum class End { Up, Formatted, Declined };
  bool loopFirst = false;
  TouchRecognizer glass;
  formatask::Question q;
  DialogTouch dialog;
  End end = End::Up;
  std::string why;
  uint32_t endMs = 0;
  bool touchIsDialogs = false;  // Ui's touch_ == TouchOn::Dialog
  bool holdUnused = false;      // Ui's holdUnused_ (a dialog uses no long press)
  InputEvent holdAt;            // Ui's hold_
  int hints = 0, begun = 0, dropped = 0, ignored = 0, stalls = 0, scripts = 0;

  explicit Asker(bool loopBeforeEvents = false) : loopFirst(loopBeforeEvents) {}

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
  // Ui::holdNews(): what a touch, or a pass, did to the dialog's hold.
  void news(News n) {
    switch (n) {
      case News::Began:
        glass.noHold();
        ++begun;
        break;
      case News::Short:
      case News::Slid:
        ++hints;
        break;
      case News::Stalled:
        ++stalls;
        break;
      case News::Dropped:
        ++dropped;
        break;
      default:
        break;
    }
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
    news(r.news);
    if (r.answer < 0) return;
    // (A tap's answer. No event answers the hold button: one that did
    // would be the first of these.)
    const bool format = q.step() == formatask::Step::Sure && r.answer == formatask::kSureFormatButton;
    answered(r.answer, e.ms, format ? "an EVENT on Format" : "Cancel");
  }
  // Ui::loop()'s part: the question's pass, with what this pass read of
  // the panel.
  void loopPass(uint32_t ms, const Finger& finger) {
    if (!up()) return;
    switch (formatask::pass(up(), q, dialog, ms, touchIsDialogs, finger)) {
      case formatask::Pass::Expired:
        answered(-1, ms, "no answer in a minute");  // (closeModal(true): onDialog(-1))
        return;
      case formatask::Pass::Dropped:
        ++dropped;
        return;
      case formatask::Pass::Stalled:
        news(News::Stalled);
        return;
      case formatask::Pass::Slid:
        news(News::Slid);
        return;
      case formatask::Pass::Scripted:
        ++scripts;  // ("a scripted finger never erases a card")
        return;
      case formatask::Pass::Confirmed:
        touchIsDialogs = false;
        answered(dialog.primary(), ms, "held to its end");
        return;
      default:
        return;
    }
  }
  // One pass: the finger as the panel reads it (`down`: a finger on the
  // glass at x, y; `scripted`: the console's), its events, and the loop.
  void pass(uint32_t ms, bool down, int x, int y, bool scripted = false) {
    TouchRecognizer::Sample s;
    s.pressed = down;
    s.scripted = down && scripted;
    s.x = s.rawX = static_cast<int16_t>(x);
    s.y = s.rawY = static_cast<int16_t>(y);
    if (x >= 319) s.edges = InputEvent::kEdgeRight;
    Finger finger;  // Input::finger()
    finger.down = s.pressed;
    finger.scripted = s.scripted;
    finger.x = s.x;
    finger.y = s.y;
    if (loopFirst) loopPass(ms, finger);
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
    if (!loopFirst) loopPass(ms, finger);
  }
  // The screen went dark under the finger (Ui::setDark(): touch_ None).
  void taken() { touchIsDialogs = false; }
};

constexpr uint32_t kPass = 10;

// A finger at (x, y) for `ms`, then lifted (one pass up): the time after.
uint32_t press(Asker& a, uint32_t t, int x, int y, uint32_t ms, bool scripted = false) {
  for (uint32_t d = 0; d <= ms; d += kPass) a.pass(t + d, true, x, y, scripted);
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

// The loop's pass after the pass's events (the firmware's order), then
// before them.
const bool kOrders[2] = {false, true};

}  // namespace

void test_two_steps_a_hold_formats() {
  for (bool loopFirst : kOrders) {
    Asker a(loopFirst);
    uint32_t t = toTheSecond(a, 100000);
    // Down at t; the fill; formatted at the pass 2 s after it landed, the
    // finger still on it (its lift goes nowhere).
    const uint32_t down = t;
    for (; t < down + 1990; t += kPass) {
      a.pass(t, true, kLeftX, kButtonY);
      TEST_ASSERT_TRUE(a.sure());
    }
    TEST_ASSERT_EQUAL_INT(kBw * 1980 / 2000, a.dialog.hold().fill(kBw));
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
}

void test_two_steps_an_early_release_shows_the_hint() {
  for (bool loopFirst : kOrders) {
    Asker a(loopFirst);
    uint32_t t = toTheSecond(a, 0);
    // Lifted 1.98 s in (the lift seen at 1.99 s): nothing, the hint.
    t = press(a, t, kLeftX, kButtonY, 1980);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(1, a.hints);
    TEST_ASSERT_FALSE(a.dialog.holding());
    // Seen to 1.99 s, the lift seen at 2 s: nothing either (no lift is
    // the answer; only a pass that sees the finger there).
    t = press(a, t + 300, kLeftX, kButtonY, 1990);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(2, a.hints);
    // A quick tap: the same.
    t = press(a, t + 300, kLeftX, kButtonY, 30);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(3, a.hints);
    // Then held: formatted, 2 s from its own landing (nothing carried over).
    const uint32_t down = t + 500;
    press(a, down, kLeftX, kButtonY, 2500);
    TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
    TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
    TEST_ASSERT_EQUAL_STRING("held to its end", a.why.c_str());
  }
}

void test_two_steps_a_slide_off_cancels_the_hold() {
  for (bool loopFirst : kOrders) {
    Asker a(loopFirst);
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
}

void test_two_steps_touch_noise() {
  for (bool loopFirst : kOrders) {
    // A resting finger's jitter (inside the recogniser's slop: no drag at
    // all), and a shakier one (past it, as drags, but on the button): each
    // formats 2 s after it landed.
    for (int amp : {3, 25}) {
      Asker a(loopFirst);
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
      Asker a(loopFirst);
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
      Asker a(loopFirst);
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
}

void test_two_steps_the_touch_taken() {
  for (bool loopFirst : kOrders) {
    // The dark (or a banner) takes the touch 1 s in: the hold drops, and
    // the finger kept on doesn't format.
    Asker a(loopFirst);
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
}

void test_two_steps_no_taps_answer_both() {
  for (bool loopFirst : kOrders) {
    // A double tap on Format...: the first question answered, the second
    // touch in the second's settle goes nowhere (neither Cancel nor a hold).
    {
      Asker a(loopFirst);
      a.open(0);
      uint32_t t = press(a, 500, kRightX, kButtonY, 60);
      t = press(a, t + 60, kRightX, kButtonY, 60);
      TEST_ASSERT_TRUE(a.sure());
      TEST_ASSERT_EQUAL_INT(1, a.ignored);
      // Even held on past the end: the touch is the settle's.
      Asker b(loopFirst);
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
      Asker a(loopFirst);
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
}

// The loop stalled (a card write on the loop task, the display's bus: no
// pass for a while, then one). Whatever the finger did meanwhile, and
// whatever the clock says at the pass after it, that time is no hold.
void test_two_steps_a_stalled_loop_never_formats() {
  for (bool loopFirst : kOrders) {
    // A 150 ms tap on the red button, no pass for 2.5 s, then the pass
    // that sees it lifted (the defect: 2.65 s from the landing to the
    // lift's pass formatted the card).
    {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      for (uint32_t d = 0; d <= 150; d += kPass) a.pass(t + d, true, kLeftX, kButtonY);
      a.pass(t + 150 + 2500, false, 0, 0);
      TEST_ASSERT_TRUE(a.sure());
      TEST_ASSERT_EQUAL_INT(1, a.begun);
      TEST_ASSERT_EQUAL_INT(1, a.hints + a.stalls);  // (its lift's event first: short; the loop's pass first: stalled)
      TEST_ASSERT_FALSE(a.dialog.holding());
      rest(a, t + 2660, 3000);
      TEST_ASSERT_TRUE(a.sure());
    }
    // The finger there at the pass after the stall, kept on or lifted and
    // landed again inside it (the panel reads the same at both passes, one
    // touch to the recogniser): the hold is over, and the finger kept on
    // 5 s more does nothing. Lifted, and held again: formatted, 2 s from
    // that landing.
    {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      for (uint32_t d = 0; d <= 150; d += kPass) a.pass(t + d, true, kLeftX, kButtonY);
      t += 150 + 2500;
      for (uint32_t end = t + 5000; t < end; t += kPass) {
        a.pass(t, true, kLeftX, kButtonY);
        TEST_ASSERT_TRUE(a.sure());
      }
      TEST_ASSERT_EQUAL_INT(1, a.stalls);
      TEST_ASSERT_EQUAL_INT(0, a.hints);
      TEST_ASSERT_EQUAL_INT(1, a.begun);
      a.pass(t, false, 0, 0);
      t = rest(a, t + kPass, 300);
      const uint32_t down = t;
      press(a, down, kLeftX, kButtonY, 2500);
      TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
      TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
      TEST_ASSERT_EQUAL_STRING("held to its end", a.why.c_str());
    }
    // Lifted and landed again elsewhere on the button inside the stall (a
    // move on the button, to the recogniser): the same.
    {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      for (uint32_t d = 0; d <= 150; d += kPass) a.pass(t + d, true, kLeftX - 30, kButtonY);
      t += 150 + 2500;
      for (uint32_t end = t + 3000; t < end; t += kPass) a.pass(t, true, kLeftX + 30, kButtonY);
      a.pass(t, false, 0, 0);
      TEST_ASSERT_TRUE(a.sure());
      TEST_ASSERT_EQUAL_INT(1, a.stalls);
      TEST_ASSERT_EQUAL_INT(0, a.hints);
    }
    // A hold all but whole: seen to 1.9 s, the next pass 0.55 s later, the
    // finger there, 2.45 s after it landed: no.
    {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      for (uint32_t d = 0; d <= 1900; d += kPass) a.pass(t + d, true, kLeftX, kButtonY);
      a.pass(t + 2450, true, kLeftX, kButtonY);
      TEST_ASSERT_TRUE(a.sure());
      TEST_ASSERT_EQUAL_INT(1, a.stalls);
      rest(a, t + 2460, 500);
      TEST_ASSERT_TRUE(a.sure());
    }
    // The gap's edge: a pass kGapMs after the last goes on, and formats at
    // 2 s; 10 ms later than that ends the hold.
    {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      const uint32_t down = t;
      for (uint32_t d = 0; d <= 1000; d += kPass) a.pass(down + d, true, kLeftX, kButtonY);
      for (uint32_t d = 1000 + HoldButton::kGapMs; a.up() && d <= 3000; d += kPass)
        a.pass(down + d, true, kLeftX, kButtonY);
      TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
      TEST_ASSERT_EQUAL_UINT32(down + 2000, a.endMs);
      Asker b(loopFirst);
      t = toTheSecond(b, 0);
      for (uint32_t d = 0; d <= 1000; d += kPass) b.pass(t + d, true, kLeftX, kButtonY);
      for (uint32_t d = 1000 + HoldButton::kGapMs + kPass; d <= 4000; d += kPass)
        b.pass(t + d, true, kLeftX, kButtonY);
      TEST_ASSERT_TRUE(b.sure());
      TEST_ASSERT_EQUAL_INT(1, b.stalls);
    }
    // A slow loop is still a loop: a pass every 400 ms formats at 2 s, one
    // every 30 ms at the first pass at or after it.
    for (uint32_t every : {400u, 30u}) {
      Asker a(loopFirst);
      uint32_t t = toTheSecond(a, 0);
      const uint32_t down = t;
      for (; a.up() && t < down + 3000; t += every) a.pass(t, true, kLeftX, kButtonY);
      TEST_ASSERT_TRUE(a.end == Asker::End::Formatted);
      TEST_ASSERT_EQUAL_UINT32(down + (every == 400 ? 2000u : 2010u), a.endMs);
      TEST_ASSERT_EQUAL_INT(0, a.stalls);
    }
  }
}

// Taps under a loop that stalls at random: presses of 20 ms to 1.98 s on
// the second question (its red button mostly; its Cancel, its body), and
// from the first question on, 0.52 s apart or more (a lift shorter than
// the gap a slow loop may not see is a panel's flicker, not a tap), the
// loop's passes 10 ms apart, or 30-400 ms, or 0.6-3 s, for two minutes
// each: some decline, none formats. And a real hold under a loop that is
// only slow (never over kGapMs between passes) always does.
void test_two_steps_taps_under_a_stalling_loop() {
  for (bool loopFirst : kOrders) {
    int declined = 0, reachedSure = 0, stalled = 0, hinted = 0;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
      Asker a(loopFirst);
      Lcg rng(seed * 7919u);
      // Three runs in four from the second question, the rest from the
      // first (where the left-hand button is Cancel).
      const bool fromFirst = seed % 4 == 0;
      uint32_t start = kPass;
      if (fromFirst) {
        a.open(0);
      } else {
        start = toTheSecond(a, 0);
      }
      // The finger: on the glass from `on` to `off`, at (x, y).
      uint32_t on = start, off = start;
      int x = 0, y = 0;
      bool sawSure = false;
      for (uint32_t t = start; a.up() && t < start + 120000;) {
        while (t >= off) {  // the next press
          on = off + static_cast<uint32_t>(rng.range(520, 2500));
          off = on + (rng.range(0, 2) == 0 ? static_cast<uint32_t>(rng.range(1000, 1980))
                                           : static_cast<uint32_t>(rng.range(20, 400)));
          const int where = rng.range(0, 19);
          if (where < (fromFirst ? 10 : 1)) {
            x = kRightX + rng.range(-40, 40);  // Format... / the second's Cancel
            y = kButtonY + rng.range(-15, 15);
          } else if (where < 18) {
            x = kLeftX + rng.range(-40, 40);  // the first's Cancel / the hold button
            y = kButtonY + rng.range(-15, 15);
          } else {
            x = rng.range(0, 319);
            y = rng.range(40, 239);
          }
        }
        a.pass(t, t >= on, x, y);
        if (a.sure()) sawSure = true;
        const int pace = rng.range(0, 19);
        t += pace < 14 ? kPass : pace < 18 ? static_cast<uint32_t>(rng.range(30, 400))
                                           : static_cast<uint32_t>(rng.range(600, 3000));
      }
      TEST_ASSERT_FALSE_MESSAGE(a.end == Asker::End::Formatted, a.why.c_str());
      if (a.end == Asker::End::Declined) ++declined;
      if (sawSure) ++reachedSure;
      if (a.stalls) ++stalled;
      if (a.hints) ++hinted;
    }
    // (The runs did reach the second question, and stalls and lifts did
    // fall on its hold button; each ended declined, by a Cancel or the
    // minute.)
    TEST_ASSERT_TRUE(reachedSure > 300);
    TEST_ASSERT_TRUE(stalled > 150);
    TEST_ASSERT_TRUE(hinted > 150);
    TEST_ASSERT_TRUE(declined > 300);
    // A finger kept on the red button 2.6 s, passes 10 to kGapMs apart at
    // random: formatted, at the first pass 2 s or more after the one that
    // saw it land.
    for (uint32_t seed = 1; seed <= 200; ++seed) {
      Asker a(loopFirst);
      Lcg rng(seed * 104729u);
      uint32_t t = toTheSecond(a, 0);
      const uint32_t down = t;
      uint32_t last = t;
      for (; a.up() && t <= down + 2600; t += static_cast<uint32_t>(rng.range(10, static_cast<int>(HoldButton::kGapMs)))) {
        a.pass(t, true, kLeftX, kButtonY);
        last = t;
      }
      TEST_ASSERT_TRUE_MESSAGE(a.end == Asker::End::Formatted, "a real hold under a slow loop");
      TEST_ASSERT_EQUAL_UINT32(last, a.endMs);
      TEST_ASSERT_TRUE(a.endMs - down >= 2000 && a.endMs - down < 2000 + HoldButton::kGapMs);
      TEST_ASSERT_EQUAL_STRING("held to its end", a.why.c_str());
    }
  }
}

// The console's scripted finger (uit, uid: ui/Input::simulate()) is a
// finger to everything but the hold: it answers the first question and
// declines the second, and held on the red button to its end it presses
// nothing. A computer on USB brings the question up (@format) and can tap
// it; the hold that erases is a hand's.
void test_two_steps_the_scripted_finger_never_formats() {
  for (bool loopFirst : kOrders) {
    // `@format`, `uit233,197` (a tap on Format...), then
    // `uid91,194,91,194,2100`: 2.28 s on the red button.
    Asker a(loopFirst);
    a.open(0);
    uint32_t t = rest(a, kPass, 800);
    t = press(a, t, kRightX, kButtonY, 60, true);
    TEST_ASSERT_TRUE(a.sure());  // (the first question is the scripted finger's to answer)
    t = rest(a, t, 1000);
    t = press(a, t, kLeftX, kButtonY, 2280, true);
    TEST_ASSERT_TRUE(a.sure());  // not formatted: the question still up
    TEST_ASSERT_EQUAL_INT(1, a.begun);
    TEST_ASSERT_EQUAL_INT(1, a.scripts);
    TEST_ASSERT_EQUAL_INT(0, a.hints);
    TEST_ASSERT_TRUE(a.dialog.hintAlert());
    TEST_ASSERT_FALSE(a.dialog.holding());
    // Again, for 10 s: once more, the same.
    t = press(a, t + 300, kLeftX, kButtonY, 10000, true);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(2, a.scripts);
    // The device check's `uid91,194,91,194,1700`, 1.88 s on it: let go
    // short, as a hand's would be (it presses nothing for two reasons).
    t = press(a, t + 300, kLeftX, kButtonY, 1880, true);
    TEST_ASSERT_TRUE(a.sure());
    TEST_ASSERT_EQUAL_INT(1, a.hints);
    TEST_ASSERT_EQUAL_INT(2, a.scripts);
    // It declines as a finger does: a scripted tap on Cancel.
    press(a, t + 300, kRightX, kButtonY, 60, true);
    TEST_ASSERT_TRUE(a.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_STRING("Cancel", a.why.c_str());

    // The first question by the scripted finger, the hold by hand:
    // formatted.
    Asker b(loopFirst);
    b.open(0);
    t = rest(b, kPass, 800);
    t = press(b, t, kRightX, kButtonY, 60, true);
    t = rest(b, t, 1000);
    const uint32_t down = t;
    press(b, down, kLeftX, kButtonY, 2500);
    TEST_ASSERT_TRUE(b.end == Asker::End::Formatted);
    TEST_ASSERT_EQUAL_UINT32(down + 2000, b.endMs);
    TEST_ASSERT_EQUAL_STRING("held to its end", b.why.c_str());

    // A hand lands on it and the scripted finger takes the touch over as
    // the hand lifts (no pass sees none: one touch to the recogniser): no
    // hand held it to the end.
    Asker c(loopFirst);
    t = toTheSecond(c, 0);
    for (uint32_t d = 0; d < 800; d += kPass) c.pass(t + d, true, kLeftX, kButtonY);
    for (uint32_t d = 800; d <= 2500; d += kPass) c.pass(t + d, true, kLeftX, kButtonY, true);
    c.pass(t + 2510, false, 0, 0);
    TEST_ASSERT_TRUE(c.sure());
    TEST_ASSERT_EQUAL_INT(1, c.scripts);
    // The other way round: the scripted finger lands, a hand takes over.
    Asker d(loopFirst);
    t = toTheSecond(d, 0);
    for (uint32_t k = 0; k < 800; k += kPass) d.pass(t + k, true, kLeftX, kButtonY, true);
    for (uint32_t k = 800; k <= 2500; k += kPass) d.pass(t + k, true, kLeftX, kButtonY);
    d.pass(t + 2510, false, 0, 0);
    TEST_ASSERT_TRUE(d.sure());
    TEST_ASSERT_EQUAL_INT(1, d.scripts);
    // One pass of it in the middle of a hand's hold.
    Asker e(loopFirst);
    t = toTheSecond(e, 0);
    for (uint32_t k = 0; k <= 2500; k += kPass) e.pass(t + k, true, kLeftX, kButtonY, k == 1000);
    e.pass(t + 2510, false, 0, 0);
    TEST_ASSERT_TRUE(e.sure());
    TEST_ASSERT_EQUAL_INT(1, e.scripts);
  }
}

void test_two_steps_the_minute() {
  for (bool loopFirst : kOrders) {
    // The first question unanswered: Cancel at its minute, not a pass before.
    {
      Asker a(loopFirst);
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
      Asker a(loopFirst);
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
      Asker a(loopFirst);
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
      Asker a(loopFirst);
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
}

void test_two_steps_cancel_and_the_settle() {
  for (bool loopFirst : kOrders) {
    // The second's Cancel in its settle: ignored; after it, Cancel.
    Asker a(loopFirst);
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
    Asker b(loopFirst);
    b.open(0);
    press(b, kPass, kLeftX, kButtonY, 2500);
    TEST_ASSERT_TRUE(b.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_INT(0, b.begun);
    // The first's Format... held: the second question, at its lift; the
    // second's Cancel held: Cancel.
    Asker c(loopFirst);
    c.open(0);
    uint32_t u = press(c, kPass, kRightX, kButtonY, 1500);
    TEST_ASSERT_TRUE(c.sure());
    u = rest(c, u, 1000);
    press(c, u, kRightX, kButtonY, 2500);
    TEST_ASSERT_TRUE(c.end == Asker::End::Declined);
    TEST_ASSERT_EQUAL_STRING("Cancel", c.why.c_str());
    TEST_ASSERT_EQUAL_INT(0, c.begun);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_buttons_hit_areas);
  RUN_TEST(test_a_hold_kept_to_its_end);
  RUN_TEST(test_a_lift_never_presses_it);
  RUN_TEST(test_a_stalled_loop_ends_the_hold);
  RUN_TEST(test_the_clock_wraps);
  RUN_TEST(test_only_a_hand_presses_it);
  RUN_TEST(test_a_hold_let_go_early);
  RUN_TEST(test_a_hold_slid_off);
  RUN_TEST(test_a_hold_dropped_or_never_begun);
  RUN_TEST(test_the_question_clock);
  RUN_TEST(test_a_dialogs_buttons);
  RUN_TEST(test_a_dialogs_hold_mode);
  RUN_TEST(test_a_dialog_draws_each_control_alone);
  RUN_TEST(test_the_answers);
  RUN_TEST(test_the_pass_order);
  RUN_TEST(test_two_steps_a_hold_formats);
  RUN_TEST(test_two_steps_an_early_release_shows_the_hint);
  RUN_TEST(test_two_steps_a_slide_off_cancels_the_hold);
  RUN_TEST(test_two_steps_touch_noise);
  RUN_TEST(test_two_steps_the_touch_taken);
  RUN_TEST(test_two_steps_no_taps_answer_both);
  RUN_TEST(test_two_steps_a_stalled_loop_never_formats);
  RUN_TEST(test_two_steps_taps_under_a_stalling_loop);
  RUN_TEST(test_two_steps_the_scripted_finger_never_formats);
  RUN_TEST(test_two_steps_the_minute);
  RUN_TEST(test_two_steps_cancel_and_the_settle);
  return UNITY_END();
}
