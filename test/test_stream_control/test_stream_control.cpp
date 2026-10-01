// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for StreamControl (when the A2DP media stream starts and
// suspends), including the event orderings ESP-IDF can produce.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <random>

#include "StreamControl.h"

using Cmd = StreamControl::Cmd;
using State = StreamControl::State;

namespace {
// Linked, wanted, CHECK_SRC_RDY sent at t=100.
StreamControl checking() {
  StreamControl s;
  s.linkUp(0);
  const auto a = s.setWanted(true, 100);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, a.send);
  return s;
}

// ...and our stream is running (t=200).
StreamControl streaming() {
  StreamControl s = checking();
  TEST_ASSERT_EQUAL(Cmd::Start, s.ack(Cmd::CheckReady, true, 150).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, true, 180).send);
  const auto a = s.audioState(true, 200);
  TEST_ASSERT_EQUAL_INT32(100, a.startedAfterMs);
  TEST_ASSERT_TRUE(s.streaming());
  return s;
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_start_is_check_then_start() {
  StreamControl s = checking();
  TEST_ASSERT_EQUAL(State::Checking, s.state());
  const auto a = s.ack(Cmd::CheckReady, true, 150);
  TEST_ASSERT_EQUAL(Cmd::Start, a.send);
  TEST_ASSERT_EQUAL(State::Starting, s.state());
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, true, 180).send);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  TEST_ASSERT_FALSE(s.streaming());  // not until the stack reports it
  TEST_ASSERT_EQUAL_INT32(100, s.audioState(true, 200).startedAfterMs);
  TEST_ASSERT_TRUE(s.streaming());
}

void test_nothing_before_the_link_is_up() {
  StreamControl s;
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(true, 0).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::CheckReady, true, 10).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.audioState(true, 20).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(5000).send);
  TEST_ASSERT_FALSE(s.streaming());
  // Wanted before the link: starts the moment it's up.
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.linkUp(6000).send);
}

void test_link_up_twice_changes_nothing() {
  StreamControl s = streaming();
  const auto a = s.linkUp(300);
  TEST_ASSERT_EQUAL(Cmd::None, a.send);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  TEST_ASSERT_TRUE(s.streaming());
}

void test_failed_check_retries_with_backoff() {
  StreamControl s = checking();  // sent at 100
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::CheckReady, false, 110).send);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(1099).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(1100).send);  // +1 s
  s.ack(Cmd::CheckReady, false, 1110);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(4099).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(4100).send);  // +3 s
  s.ack(Cmd::CheckReady, false, 4110);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(13099).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(13100).send);  // +9 s
  s.ack(Cmd::CheckReady, false, 13110);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(23099).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(23100).send);  // capped at 10 s
}

void test_busy_answer_counts_as_failure() {
  StreamControl s = checking();
  s.ack(Cmd::CheckReady, true, 150);  // START sent
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, false, 160).send);  // BUSY
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(1100).send);
}

void test_stray_acks_change_nothing() {
  StreamControl s = checking();
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, true, 110).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Suspend, true, 120).send);
  TEST_ASSERT_EQUAL(State::Checking, s.state());
  StreamControl t = streaming();
  TEST_ASSERT_EQUAL(Cmd::None, t.ack(Cmd::CheckReady, false, 300).send);
  TEST_ASSERT_EQUAL(Cmd::None, t.ack(Cmd::Start, false, 310).send);
  TEST_ASSERT_EQUAL(State::Started, t.state());
  TEST_ASSERT_TRUE(t.streaming());
}

void test_pause_suspends_after_3_s() {
  StreamControl s = streaming();
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(false, 1000).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(3999).send);
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(4000).send);
  TEST_ASSERT_EQUAL(State::Suspending, s.state());
  // Either order of the stack's two reports; neither is the headphones' doing.
  const auto a = s.audioState(false, 4050);
  TEST_ASSERT_FALSE(a.remoteSuspend);
  TEST_ASSERT_EQUAL(State::Suspending, s.state());
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Suspend, true, 4060).send);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  TEST_ASSERT_FALSE(s.streaming());
}

void test_suspend_ack_before_the_audio_state() {
  StreamControl s = streaming();
  s.setWanted(false, 1000);
  s.tick(4000);
  s.ack(Cmd::Suspend, true, 4050);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  const auto a = s.audioState(false, 4060);
  TEST_ASSERT_FALSE(a.remoteSuspend);
  TEST_ASSERT_FALSE(s.heldOff());
}

void test_resume_within_3_s_keeps_the_stream() {
  StreamControl s = streaming();
  s.setWanted(false, 1000);
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(true, 3000).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(10000).send);
  TEST_ASSERT_EQUAL(State::Started, s.state());
}

void test_resume_while_suspending_restarts_after_the_ack() {
  StreamControl s = streaming();
  s.setWanted(false, 1000);
  s.tick(4000);  // SUSPEND sent
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(true, 4010).send);  // one command at a time
  s.audioState(false, 4020);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.ack(Cmd::Suspend, true, 4030).send);
}

void test_pause_while_starting_suspends_later() {
  StreamControl s = checking();
  s.ack(Cmd::CheckReady, true, 150);
  s.setWanted(false, 160);
  s.ack(Cmd::Start, true, 170);
  s.audioState(true, 180);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(3159).send);
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(3160).send);
}

void test_pause_while_checking_does_not_start() {
  StreamControl s = checking();
  s.setWanted(false, 120);
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::CheckReady, true, 150).send);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
}

void test_headphones_suspending_our_stream_hold_it_off() {
  StreamControl s = streaming();
  const auto a = s.audioState(false, 1000);
  TEST_ASSERT_TRUE(a.remoteSuspend);
  TEST_ASSERT_TRUE(s.heldOff());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(60000).send);  // no fighting it
  // The player pauses (wanted falls) and plays again: a fresh start.
  s.setWanted(false, 60001);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.setWanted(true, 60002).send);
  TEST_ASSERT_FALSE(s.heldOff());
}

void test_headphones_starting_a_stream_themselves_is_not_ours() {
  StreamControl s;
  s.linkUp(0);
  s.audioState(true, 100);  // not wanted: ESP-IDF suspends it itself
  TEST_ASSERT_EQUAL(State::Started, s.state());
  const auto a = s.audioState(false, 150);
  TEST_ASSERT_FALSE(a.remoteSuspend);
  TEST_ASSERT_FALSE(s.heldOff());
  TEST_ASSERT_EQUAL(State::Idle, s.state());
}

void test_unwanted_stream_started_by_them_is_suspended() {
  StreamControl s;
  s.linkUp(0);
  s.audioState(true, 100);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(2999).send);
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(3000).send);
}

// The collision the review found: the headphones start a stream while our
// START is on its way. ESP-IDF reports STARTED, acks our START (it is already
// started) and then suspends the stream it didn't ask for.
void test_remote_start_collision_does_not_stall_or_pause() {
  StreamControl s = checking();
  s.ack(Cmd::CheckReady, true, 150);  // START sent
  s.audioState(true, 160);            // theirs, before our answer
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, true, 170).send);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  const auto a = s.audioState(false, 180);  // ESP-IDF's own suspend
  TEST_ASSERT_FALSE(a.remoteSuspend);
  TEST_ASSERT_FALSE(s.heldOff());
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  // Still wanted: started again on the retry schedule, not stuck.
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(1100).send);
  s.ack(Cmd::CheckReady, true, 1110);
  s.ack(Cmd::Start, true, 1120);
  s.audioState(true, 1130);
  TEST_ASSERT_TRUE(s.streaming());
  TEST_ASSERT_TRUE(s.audioState(false, 2000).remoteSuspend);  // this one is ours
}

void test_remote_start_and_suspend_while_checking() {
  StreamControl s = checking();
  s.audioState(true, 110);
  s.audioState(false, 120);
  TEST_ASSERT_EQUAL(Cmd::Start, s.ack(Cmd::CheckReady, true, 130).send);
  s.ack(Cmd::Start, true, 140);
  const auto a = s.audioState(true, 150);
  TEST_ASSERT_EQUAL_INT32(50, a.startedAfterMs);
  TEST_ASSERT_TRUE(s.streaming());
}

void test_lost_start_ack_is_given_up_and_retried() {
  StreamControl s = checking();
  s.ack(Cmd::CheckReady, true, 150);  // START sent at 150, never answered
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(3149).send);
  const auto a = s.tick(3150);
  TEST_ASSERT_TRUE(a.gaveUp);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, a.send);  // the retry was due at 1100
  TEST_ASSERT_EQUAL(State::Checking, s.state());
}

void test_lost_suspend_ack_is_given_up_and_retried() {
  StreamControl s = streaming();
  s.setWanted(false, 1000);
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(4000).send);
  const auto a = s.tick(7000);
  TEST_ASSERT_TRUE(a.gaveUp);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(8999).send);
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(9000).send);  // kSuspendRetryMs after the first
}

void test_failed_suspend_is_retried() {
  StreamControl s = streaming();
  s.setWanted(false, 1000);
  s.tick(4000);
  s.ack(Cmd::Suspend, false, 4010);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  TEST_ASSERT_EQUAL(Cmd::Suspend, s.tick(9000).send);
}

void test_start_not_allowed_waits_then_starts_at_once() {
  StreamControl s;
  s.setStartAllowed(false, 0);
  s.linkUp(0);
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(true, 10).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(5000).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.setStartAllowed(true, 5001).send);
}

void test_not_allowed_leaves_a_running_stream_alone() {
  StreamControl s = streaming();
  TEST_ASSERT_EQUAL(Cmd::None, s.setStartAllowed(false, 300).send);
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(10000).send);
  TEST_ASSERT_TRUE(s.streaming());
}

void test_send_failure_waits_for_the_schedule() {
  StreamControl s = checking();  // at 100
  TEST_ASSERT_EQUAL(Cmd::None, s.sendFailed(100).send);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.tick(1100).send);
}

void test_link_down_forgets_the_stream() {
  StreamControl s = streaming();
  s.audioState(false, 500);  // held off
  s.linkDown();
  TEST_ASSERT_FALSE(s.streaming());
  TEST_ASSERT_FALSE(s.heldOff());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(1000).send);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, s.linkUp(2000).send);  // still wanted
}

void test_wanted_while_their_stream_runs_sends_nothing() {
  StreamControl s;
  s.linkUp(0);
  s.audioState(true, 10);
  TEST_ASSERT_EQUAL(Cmd::None, s.setWanted(true, 20).send);
  TEST_ASSERT_TRUE(s.streaming());
}

// ESP-IDF's stop_tx acks whatever command is pending with SUCCESS: a START
// can be "acknowledged" and never start. Not left in Started without audio.
void test_start_acked_but_never_started_is_retried() {
  StreamControl s = checking();                             // CHECK at 100
  TEST_ASSERT_EQUAL(Cmd::Start, s.ack(Cmd::CheckReady, true, 150).send);  // START at 150
  TEST_ASSERT_EQUAL(Cmd::None, s.ack(Cmd::Start, true, 160).send);
  TEST_ASSERT_EQUAL(State::Started, s.state());
  TEST_ASSERT_TRUE(s.active());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(150 + StreamControl::kAckTimeoutMs - 1).send);
  const auto a = s.tick(150 + StreamControl::kAckTimeoutMs);
  TEST_ASSERT_TRUE(a.gaveUp);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, a.send);
  TEST_ASSERT_EQUAL(State::Checking, s.state());
}

// Not wanted any more: the same watchdog, then nothing (no suspend of a
// stream that never ran).
void test_start_acked_but_never_started_while_unwanted_goes_idle() {
  StreamControl s = checking();
  s.ack(Cmd::CheckReady, true, 150);
  s.ack(Cmd::Start, true, 160);
  s.setWanted(false, 200);
  const auto a = s.tick(150 + StreamControl::kAckTimeoutMs);
  TEST_ASSERT_TRUE(a.gaveUp);
  TEST_ASSERT_EQUAL(Cmd::None, a.send);
  TEST_ASSERT_EQUAL(State::Idle, s.state());
  TEST_ASSERT_EQUAL(Cmd::None, s.tick(20000).send);
}

// A pause from the headphones' key: they pick their next key from the
// stream, so it is suspended at once rather than after 3 s.
void test_suspend_now_skips_the_wait() {
  StreamControl s = streaming();
  TEST_ASSERT_EQUAL(Cmd::None, s.suspendNow(900).send);  // still wanted: nothing
  s.setWanted(false, 1000);
  const auto a = s.suspendNow(1000);
  TEST_ASSERT_EQUAL(Cmd::Suspend, a.send);
  TEST_ASSERT_EQUAL(State::Suspending, s.state());
}

void test_active_covers_start_to_suspend() {
  StreamControl s = checking();
  TEST_ASSERT_FALSE(s.active());  // CHECK_SRC_RDY starts nothing
  s.ack(Cmd::CheckReady, true, 150);
  TEST_ASSERT_TRUE(s.active());   // START out: IDF may already send media
  s.ack(Cmd::Start, true, 160);
  TEST_ASSERT_TRUE(s.active());
  TEST_ASSERT_FALSE(s.streaming());
  s.audioState(true, 170);
  s.setWanted(false, 200);
  s.tick(200 + StreamControl::kSuspendAfterMs);
  TEST_ASSERT_EQUAL(State::Suspending, s.state());
  TEST_ASSERT_TRUE(s.active());
  s.ack(Cmd::Suspend, true, 3300);
  s.audioState(false, 3310);
  TEST_ASSERT_FALSE(s.active());
}

// Random events against a model of ESP-IDF's rule: never a second command
// while one is outstanding (sent, not answered and not given up on), and a
// remote suspend is only ever reported for a stream our START started.
void test_random_events_keep_one_command_outstanding() {
  std::mt19937 rng(12345);
  StreamControl s;
  uint32_t now = 0;
  Cmd outstanding = Cmd::None;  // the model's view of the stack
  bool audio = false;           // what the stack last reported
  bool ours = false;            // our START was answered before the stream reported STARTED
  bool linked = false;
  int starts = 0, remoteSuspends = 0;
  auto check = [&](const StreamControl::Actions& a) {
    if (a.gaveUp) outstanding = Cmd::None;
    if (a.send != Cmd::None) {
      TEST_ASSERT_EQUAL(Cmd::None, outstanding);
      outstanding = a.send;
    }
    if (a.remoteSuspend) {
      TEST_ASSERT_TRUE(ours);
      ++remoteSuspends;
    }
    if (a.startedAfterMs >= 0) ++starts;
  };
  for (int i = 0; i < 200000; ++i) {
    now += rng() % 700;
    switch (rng() % 10) {
      case 0:
        if (rng() % 20 == 0) {
          s.linkDown();
          linked = false;
          outstanding = Cmd::None;
          audio = ours = false;
        } else {
          check(s.linkUp(now));
          linked = true;
        }
        break;
      case 1:
        check(s.setWanted(rng() % 2, now));
        break;
      case 2:
        check(s.setStartAllowed(rng() % 4 != 0, now));
        break;
      case 3:
      case 4:
        if (outstanding != Cmd::None && rng() % 8 != 0) {  // its answer (1 in 8 is lost)
          const Cmd c = outstanding;
          const bool ok = rng() % 3 != 0;
          outstanding = Cmd::None;
          if (c == Cmd::Start && ok) ours = !audio;
          if (c == Cmd::Suspend && ok) audio = ours = false;
          check(s.ack(c, ok, now));
        } else {  // a stray answer, never to the outstanding command
          Cmd c = static_cast<Cmd>(1 + rng() % 3);
          if (c == outstanding) break;
          check(s.ack(c, rng() % 2, now));
        }
        break;
      case 5:
      case 6: {
        const bool started = rng() % 2;
        const auto a = s.audioState(started, now);
        check(a);
        if (linked && !started && audio) ours = false;
        if (linked) audio = started;
        break;
      }
      case 7:
        if (rng() % 4 == 0) {
          check(s.suspendNow(now));
          break;
        }
        check(s.tick(now));
        break;
      default:
        check(s.tick(now));
        break;
    }
    TEST_ASSERT_EQUAL(audio, s.streaming());
    if (s.streaming()) TEST_ASSERT_TRUE(s.active());
  }
  TEST_ASSERT_TRUE(starts > 100);  // the walk did reach the interesting states
  TEST_ASSERT_TRUE(remoteSuspends > 10);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_start_acked_but_never_started_is_retried);
  RUN_TEST(test_start_acked_but_never_started_while_unwanted_goes_idle);
  RUN_TEST(test_suspend_now_skips_the_wait);
  RUN_TEST(test_active_covers_start_to_suspend);
  RUN_TEST(test_start_is_check_then_start);
  RUN_TEST(test_nothing_before_the_link_is_up);
  RUN_TEST(test_link_up_twice_changes_nothing);
  RUN_TEST(test_failed_check_retries_with_backoff);
  RUN_TEST(test_busy_answer_counts_as_failure);
  RUN_TEST(test_stray_acks_change_nothing);
  RUN_TEST(test_pause_suspends_after_3_s);
  RUN_TEST(test_suspend_ack_before_the_audio_state);
  RUN_TEST(test_resume_within_3_s_keeps_the_stream);
  RUN_TEST(test_resume_while_suspending_restarts_after_the_ack);
  RUN_TEST(test_pause_while_starting_suspends_later);
  RUN_TEST(test_pause_while_checking_does_not_start);
  RUN_TEST(test_headphones_suspending_our_stream_hold_it_off);
  RUN_TEST(test_headphones_starting_a_stream_themselves_is_not_ours);
  RUN_TEST(test_unwanted_stream_started_by_them_is_suspended);
  RUN_TEST(test_remote_start_collision_does_not_stall_or_pause);
  RUN_TEST(test_remote_start_and_suspend_while_checking);
  RUN_TEST(test_lost_start_ack_is_given_up_and_retried);
  RUN_TEST(test_lost_suspend_ack_is_given_up_and_retried);
  RUN_TEST(test_failed_suspend_is_retried);
  RUN_TEST(test_start_not_allowed_waits_then_starts_at_once);
  RUN_TEST(test_not_allowed_leaves_a_running_stream_alone);
  RUN_TEST(test_send_failure_waits_for_the_schedule);
  RUN_TEST(test_link_down_forgets_the_stream);
  RUN_TEST(test_wanted_while_their_stream_runs_sends_nothing);
  RUN_TEST(test_random_events_keep_one_command_outstanding);
  return UNITY_END();
}
