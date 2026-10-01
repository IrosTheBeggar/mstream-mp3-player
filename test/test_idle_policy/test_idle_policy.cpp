// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for the idle power-off (IdlePolicy, docs/ENERGY.md item 4 and
// step 5): the countdown for each choice, every condition that blocks it,
// input restarting it, the 30 s warning, the release before the power
// goes, and its texts. The queue's flushNow() is covered in test_queue.
// Run: pio test -e native
#include <unity.h>

#include <cstdlib>
#include <cstring>

#include "IdlePolicy.h"

void setUp() {}
void tearDown() {}

namespace {

using P = IdlePolicy::Phase;
using B = IdlePolicy::Blocker;
constexpr uint32_t kMin = 60000;

// What the loop saw, pass by pass.
struct Seen {
  int warns = 0, warnEnds = 0, shutdowns = 0, cancels = 0, powerOffs = 0;
  uint32_t warnAt = 0, shutdownAt = 0, powerOffAt = 0;
  void add(const IdlePolicy::Out& o, uint32_t t) {
    if (o.warn) {
      ++warns;
      warnAt = t;
    }
    if (o.warnEnd) ++warnEnds;
    if (o.shutdown) {
      ++shutdowns;
      shutdownAt = t;
    }
    if (o.cancelled) ++cancels;
    if (o.powerOff) {
      ++powerOffs;
      powerOffAt = t;
    }
  }
};

IdlePolicy::In idle(uint32_t t) {
  IdlePolicy::In in;
  in.nowMs = t;
  in.play = PlayState::Paused;
  return in;
}

// Passes every 100 ms from `from` up to `to` with `in` (its time set).
Seen run(IdlePolicy& p, uint32_t from, uint32_t to, IdlePolicy::In in) {
  Seen s;
  for (uint32_t t = from; t <= to; t += 100) {
    in.nowMs = t;
    s.add(p.update(in), t);
  }
  return s;
}

}  // namespace

void test_choices_and_defaults() {
  TEST_ASSERT_EQUAL_INT(4, IdlePolicy::kChoices);
  TEST_ASSERT_EQUAL_UINT32(10 * kMin, IdlePolicy::choiceMs(0));
  TEST_ASSERT_EQUAL_UINT32(20 * kMin, IdlePolicy::choiceMs(IdlePolicy::kDefaultChoice));
  TEST_ASSERT_EQUAL_UINT32(60 * kMin, IdlePolicy::choiceMs(2));
  TEST_ASSERT_EQUAL_UINT32(0, IdlePolicy::choiceMs(IdlePolicy::kNever));
  TEST_ASSERT_EQUAL_STRING("20 min", IdlePolicy::choiceLabel(1));
  TEST_ASSERT_EQUAL_STRING("Never", IdlePolicy::choiceLabel(3));
  // Out of range (an NVS value from elsewhere): the default.
  TEST_ASSERT_EQUAL_STRING("20 min", IdlePolicy::choiceLabel(9));
  IdlePolicy p;
  p.begin(-2, 0);
  TEST_ASSERT_EQUAL_INT(IdlePolicy::kDefaultChoice, p.choice());
  TEST_ASSERT_EQUAL_UINT32(20 * kMin, p.lengthMs());
}

// Paused on the battery, nothing touched: the warning 30 s before, then
// the shutdown at 20 min, then (the headphones unlinked) the power off.
void test_default_warns_then_turns_off_at_20_min() {
  IdlePolicy p;
  p.begin(IdlePolicy::kDefaultChoice, 0);
  Seen s = run(p, 0, 20 * kMin - 30000 - 100, idle(0));
  TEST_ASSERT_EQUAL_INT(0, s.warns);
  TEST_ASSERT_EQUAL(P::Counting, p.phase());
  TEST_ASSERT_EQUAL_UINT32(30100, p.msLeft(20 * kMin - 30100));
  s = run(p, 20 * kMin - 30000, 20 * kMin - 100, idle(0));
  TEST_ASSERT_EQUAL_INT(1, s.warns);
  TEST_ASSERT_EQUAL_UINT32(20 * kMin - 30000, s.warnAt);
  TEST_ASSERT_EQUAL(P::Warning, p.phase());
  TEST_ASSERT_EQUAL_INT(0, s.shutdowns);
  IdlePolicy::In in = idle(20 * kMin);
  in.linked = false;
  IdlePolicy::Out o = p.update(in);
  TEST_ASSERT_TRUE(o.shutdown);
  TEST_ASSERT_FALSE(o.powerOff);
  TEST_ASSERT_EQUAL(P::Releasing, p.phase());
  // Not linked: off at the next pass.
  in.nowMs += 20;
  o = p.update(in);
  TEST_ASSERT_TRUE(o.powerOff);
  TEST_ASSERT_EQUAL(P::Off, p.phase());
  // Nothing more, whatever comes.
  in.nowMs += 20;
  in.input = true;
  o = p.update(in);
  TEST_ASSERT_FALSE(o.powerOff || o.shutdown || o.cancelled || o.warn);
}

void test_each_choice_and_the_test_length() {
  for (int c = 0; c < IdlePolicy::kNever; ++c) {
    IdlePolicy p;
    p.begin(c, 1000);
    const Seen s = run(p, 1000, 1000 + IdlePolicy::choiceMs(c), idle(0));
    TEST_ASSERT_EQUAL_INT(1, s.warns);
    TEST_ASSERT_EQUAL_INT(1, s.shutdowns);
    TEST_ASSERT_EQUAL_UINT32(1000 + IdlePolicy::choiceMs(c), s.shutdownAt);
    TEST_ASSERT_EQUAL_UINT32(1000 + IdlePolicy::choiceMs(c) - IdlePolicy::kWarnMs, s.warnAt);
  }
  // The console's 2 min, then back to the setting.
  IdlePolicy p;
  p.begin(IdlePolicy::kNever, 0);
  p.setTestMs(2 * kMin, 0);
  TEST_ASSERT_EQUAL_UINT32(2 * kMin, p.lengthMs());
  Seen s = run(p, 0, 2 * kMin, idle(0));
  TEST_ASSERT_EQUAL_UINT32(2 * kMin, s.shutdownAt);
  IdlePolicy q;
  q.begin(IdlePolicy::kNever, 0);
  q.setTestMs(2 * kMin, 0);
  q.setTestMs(0, 0);
  TEST_ASSERT_EQUAL_UINT32(0, q.lengthMs());
  s = run(q, 0, 3 * kMin, idle(0));
  TEST_ASSERT_EQUAL_INT(0, s.shutdowns);
  TEST_ASSERT_EQUAL(B::Never, q.blocker());
}

// Every condition that blocks it: held over twice the length, nothing
// happens; once it clears, the whole length again from that moment.
void test_every_blocker_blocks_and_restarts_the_countdown() {
  enum Kind { Playing, Waiting, Usb, Pairing, QueueWrite, Busy, kKinds };
  const B expect[kKinds] = {B::Playing, B::Waiting, B::Usb, B::Pairing, B::QueueWrite, B::Busy};
  for (int k = 0; k < kKinds; ++k) {
    IdlePolicy p;
    p.begin(0, 0);  // 10 min
    IdlePolicy::In in = idle(0);
    if (k == Playing) in.play = PlayState::Playing;
    if (k == Waiting) in.play = PlayState::Waiting;
    in.usb = k == Usb;
    in.pairing = k == Pairing;
    in.queueWrite = k == QueueWrite;
    in.busy = k == Busy;
    Seen s = run(p, 0, 20 * kMin, in);
    TEST_ASSERT_EQUAL_INT(0, s.warns);
    TEST_ASSERT_EQUAL_INT(0, s.shutdowns);
    TEST_ASSERT_EQUAL(P::Blocked, p.phase());
    TEST_ASSERT_EQUAL(expect[k], p.blocker());
    TEST_ASSERT_EQUAL_UINT32(0, p.msLeft(20 * kMin));
    s = run(p, 20 * kMin + 100, 31 * kMin, idle(0));
    TEST_ASSERT_EQUAL_INT(1, s.shutdowns);
    TEST_ASSERT_EQUAL_UINT32(30 * kMin + 100, s.shutdownAt);
  }
  // Stopped counts as idle as paused does.
  IdlePolicy p;
  p.begin(0, 0);
  IdlePolicy::In in = idle(0);
  in.play = PlayState::Stopped;
  TEST_ASSERT_EQUAL_INT(1, run(p, 0, 10 * kMin, in).shutdowns);
}

// A touch, a button, the PWR key, a headphone key, the console: the
// countdown starts again from it.
void test_input_restarts_the_countdown() {
  IdlePolicy p;
  p.begin(0, 0);
  run(p, 0, 7 * kMin, idle(0));
  IdlePolicy::In in = idle(7 * kMin + 100);
  in.input = true;
  p.update(in);
  TEST_ASSERT_EQUAL_UINT32(10 * kMin, p.msLeft(7 * kMin + 100));
  const Seen s = run(p, 7 * kMin + 200, 18 * kMin, idle(0));
  TEST_ASSERT_EQUAL_UINT32(17 * kMin + 100, s.shutdownAt);
}

// The warning: any input ends it (the countdown again), and so does
// something that blocks (music, USB); it counts its seconds down.
void test_the_warning_and_what_ends_it() {
  IdlePolicy p;
  p.begin(0, 0);
  run(p, 0, 10 * kMin - 30000, idle(0));
  TEST_ASSERT_EQUAL(P::Warning, p.phase());
  TEST_ASSERT_EQUAL_UINT32(30, p.warnSeconds(10 * kMin - 30000));
  TEST_ASSERT_EQUAL_UINT32(30, p.warnSeconds(10 * kMin - 29001));
  TEST_ASSERT_EQUAL_UINT32(29, p.warnSeconds(10 * kMin - 29000));
  TEST_ASSERT_EQUAL_UINT32(1, p.warnSeconds(10 * kMin - 1));
  IdlePolicy::In in = idle(10 * kMin - 10000);
  in.input = true;  // "Keep on", or any touch
  IdlePolicy::Out o = p.update(in);
  TEST_ASSERT_TRUE(o.warnEnd);
  TEST_ASSERT_FALSE(o.warn);
  TEST_ASSERT_EQUAL(P::Counting, p.phase());
  TEST_ASSERT_EQUAL_UINT32(0, p.warnSeconds(10 * kMin - 10000));
  // Warned again 30 s before the new end; then USB in ends it.
  Seen s = run(p, 10 * kMin - 9900, 20 * kMin - 20000, idle(0));
  TEST_ASSERT_EQUAL_INT(1, s.warns);
  TEST_ASSERT_EQUAL_UINT32(20 * kMin - 10000 - 30000, s.warnAt);
  in = idle(20 * kMin - 19900);
  in.usb = true;
  o = p.update(in);
  TEST_ASSERT_TRUE(o.warnEnd);
  TEST_ASSERT_EQUAL(P::Blocked, p.phase());
  // Music playing ends it too.
  IdlePolicy q;
  q.begin(0, 0);
  run(q, 0, 10 * kMin - 1000, idle(0));
  in = idle(10 * kMin - 900);
  in.play = PlayState::Playing;
  TEST_ASSERT_TRUE(q.update(in).warnEnd);
  TEST_ASSERT_EQUAL(B::Playing, q.blocker());
}

// The time is up: it waits for the headphones to go (at most 3 s), and
// input or a blocker meanwhile cancels it.
void test_the_release_waits_for_the_headphones_and_can_be_cancelled() {
  IdlePolicy p;
  p.begin(0, 0);
  IdlePolicy::In in = idle(0);
  in.linked = true;
  Seen s = run(p, 0, 10 * kMin + 2900, in);
  TEST_ASSERT_EQUAL_INT(1, s.shutdowns);
  TEST_ASSERT_EQUAL_INT(0, s.powerOffs);
  TEST_ASSERT_EQUAL(P::Releasing, p.phase());
  // Still linked at 3 s: off anyway.
  s = run(p, 10 * kMin + 3000, 10 * kMin + 3000, in);
  TEST_ASSERT_EQUAL_INT(1, s.powerOffs);
  // Unlinked sooner: off at once.
  IdlePolicy q;
  q.begin(0, 0);
  run(q, 0, 10 * kMin + 500, in);
  in.nowMs = 10 * kMin + 600;
  in.linked = false;
  TEST_ASSERT_TRUE(q.update(in).powerOff);
  // A touch during the release: it stays on, counting from the touch.
  for (int blocker = 0; blocker < 2; ++blocker) {
    IdlePolicy r;
    r.begin(0, 0);
    IdlePolicy::In li = idle(0);
    li.linked = true;
    run(r, 0, 10 * kMin + 1000, li);
    li.nowMs = 10 * kMin + 1100;
    li.input = blocker == 0;
    li.usb = blocker == 1;
    const IdlePolicy::Out o = r.update(li);
    TEST_ASSERT_TRUE(o.cancelled);
    TEST_ASSERT_FALSE(o.powerOff);
    TEST_ASSERT_EQUAL(blocker == 0 ? P::Counting : P::Blocked, r.phase());
    li.input = li.usb = false;
    li.linked = false;
    s = run(r, 10 * kMin + 1200, 21 * kMin, li);
    TEST_ASSERT_EQUAL_INT(1, s.shutdowns);
    TEST_ASSERT_EQUAL_UINT32(blocker == 0 ? 20 * kMin + 1100 : 20 * kMin + 1200, s.shutdownAt);
  }
}

// The sleep timer's pause (ENERGY.md section 3, step 6): the countdown
// runs from the pause, not from the last touch before the music.
void test_it_counts_from_the_pause() {
  IdlePolicy p;
  p.begin(IdlePolicy::kDefaultChoice, 0);
  IdlePolicy::In in = idle(0);
  in.play = PlayState::Playing;
  Seen s = run(p, 0, 90 * kMin, in);  // 90 min of music nobody touched
  TEST_ASSERT_EQUAL_INT(0, s.shutdowns);
  s = run(p, 90 * kMin + 100, 111 * kMin, idle(0));  // the timer paused it
  TEST_ASSERT_EQUAL_INT(1, s.shutdowns);
  TEST_ASSERT_EQUAL_UINT32(110 * kMin + 100, s.shutdownAt);
}

// The setting changed (a tap on its row): the countdown starts again, and
// a warning up goes.
void test_a_new_choice_restarts() {
  IdlePolicy p;
  p.begin(0, 0);
  run(p, 0, 10 * kMin - 5000, idle(0));
  TEST_ASSERT_EQUAL(P::Warning, p.phase());
  p.setChoice(2, 10 * kMin - 4900);
  TEST_ASSERT_EQUAL_UINT32(0, p.warnSeconds(10 * kMin - 4900));
  const Seen s = run(p, 10 * kMin - 4900, 71 * kMin, idle(0));
  TEST_ASSERT_EQUAL_UINT32(70 * kMin - 4900, s.shutdownAt);
  p.setChoice(IdlePolicy::kNever, 0);
  TEST_ASSERT_EQUAL(P::Off, p.phase());  // (off is off: nothing undoes it)
  // cancel(): the caller saw USB at the last moment.
  IdlePolicy q;
  q.begin(0, 0);
  run(q, 0, 10 * kMin + 100, idle(0));
  TEST_ASSERT_EQUAL(P::Off, q.phase());
  q.cancel(10 * kMin + 200);
  TEST_ASSERT_EQUAL(P::Blocked, q.phase());
  const Seen s2 = run(q, 10 * kMin + 300, 21 * kMin, idle(0));
  TEST_ASSERT_EQUAL_UINT32(20 * kMin + 300, s2.shutdownAt);
}

// A random run: never a shutdown while anything blocks or since an input
// less than the length ago; always one once idle that long.
void test_random_never_off_while_blocked_or_touched() {
  srand(7);
  IdlePolicy p;
  p.begin(0, 0);
  p.setTestMs(60000, 0);
  uint32_t quietSince = 0;
  bool wasBlocked = true;
  for (uint32_t t = 0; t < 4 * 3600 * 1000u; t += 250) {
    IdlePolicy::In in = idle(t);
    const int r = rand() % 4000;
    in.input = r < 4;
    in.usb = (t / 600000) % 3 == 1;
    in.play = (t / 450000) % 4 == 2 ? PlayState::Playing : PlayState::Paused;
    in.queueWrite = r >= 4 && r < 6;
    const bool blocked = in.usb || in.play == PlayState::Playing || in.queueWrite;
    if (blocked || in.input || wasBlocked) quietSince = t;
    wasBlocked = blocked;
    const IdlePolicy::Out o = p.update(in);
    if (o.shutdown) {
      TEST_ASSERT_FALSE(blocked);
      TEST_ASSERT_FALSE(in.input);
      TEST_ASSERT_TRUE(t - quietSince >= 60000);
      p.cancel(t);  // carry on, as if USB was seen at the last moment
      wasBlocked = true;
    } else if (p.phase() != P::Releasing) {
      TEST_ASSERT_TRUE(blocked || in.input || t - quietSince < 60000);
    }
  }
}

void test_texts() {
  char b[48];
  IdlePolicy::warnText(30, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Turning off in 30 s", b);
  IdlePolicy::offText(20 * kMin, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Turned off after 20 minutes idle", b);
  IdlePolicy::offText(kMin, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Turned off after 1 minute idle", b);
  IdlePolicy::offText(45000, b, sizeof(b));
  TEST_ASSERT_EQUAL_STRING("Turned off after 45 s idle", b);
  TEST_ASSERT_EQUAL_STRING("on USB power", IdlePolicy::blockerName(B::Usb));
  TEST_ASSERT_EQUAL_STRING("warning", IdlePolicy::phaseName(P::Warning));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_choices_and_defaults);
  RUN_TEST(test_default_warns_then_turns_off_at_20_min);
  RUN_TEST(test_each_choice_and_the_test_length);
  RUN_TEST(test_every_blocker_blocks_and_restarts_the_countdown);
  RUN_TEST(test_input_restarts_the_countdown);
  RUN_TEST(test_the_warning_and_what_ends_it);
  RUN_TEST(test_the_release_waits_for_the_headphones_and_can_be_cancelled);
  RUN_TEST(test_it_counts_from_the_pause);
  RUN_TEST(test_a_new_choice_restarts);
  RUN_TEST(test_random_never_off_while_blocked_or_touched);
  RUN_TEST(test_texts);
  return UNITY_END();
}
