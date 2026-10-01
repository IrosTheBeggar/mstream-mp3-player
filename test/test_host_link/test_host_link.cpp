// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host tests for HostLink (docs/USB-VISUALIZER.md "Sessions", "The
// visualizer's messages", "Errors", "Host mode on the Core2"): the session
// rules line by line, the epochs and hops, the way in and the ways out.
// Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "HostLink.h"

using E = HostLink::Event;
using W = HostLink::Why;
using Busy = HostLink::Busy;

namespace {
constexpr const char* kFw = "v0.5.0-beta.1";

// A line as the console hands it over (a writable copy).
HostLink::Out send(HostLink& l, const char* text, uint32_t nowMs, Busy busy = Busy::None) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s", text);
  return l.line(buf, nowMs, busy);
}

// A link in a session (id "7f3a") at `nowMs`, with epoch 1 at 44.1 kHz.
HostLink inSession(uint32_t nowMs, bool epoch = true) {
  HostLink l;
  l.begin(kFw);
  const HostLink::Out o = send(l, "@hello 1 7f3a viz", nowMs);
  TEST_ASSERT_TRUE(o.event == E::Enter);
  if (epoch) TEST_ASSERT_TRUE(send(l, "@e 1 44100 0", nowMs).event == E::Epoch);
  return l;
}

void expectReply(const HostLink::Out& o, const char* reply) { TEST_ASSERT_EQUAL_STRING(reply, o.reply); }
}  // namespace

void setUp() {}
void tearDown() {}

// ---- hello ----

// @hello with viz starts host mode: Enter, and the exact @ok.
void test_hello_enters() {
  HostLink l;
  l.begin(kFw);
  TEST_ASSERT_FALSE(l.active());
  const HostLink::Out o = send(l, "@hello 1 7f3a viz", 1000);
  TEST_ASSERT_TRUE(o.event == E::Enter);
  expectReply(o, "@ok 1 7f3a v0.5.0-beta.1 viz,log");
  TEST_ASSERT_TRUE(l.active());
  TEST_ASSERT_EQUAL_UINT32(1, l.proto());
  TEST_ASSERT_EQUAL_STRING("7f3a", l.session());
  TEST_ASSERT_EQUAL_UINT32(1000, l.enteredMs());
  TEST_ASSERT_FALSE(l.haveEpoch());
  // A newer sender: the Core2's highest; features it doesn't know are ignored.
  HostLink m;
  m.begin("v1 2");  // (a space in the version can't break the line)
  expectReply(send(m, "@hello 7 Z9 setup,viz,spectrum", 0), "@ok 1 Z9 v1_2 viz,log");
  // Extra trailing fields are ignored.
  HostLink n;
  n.begin(kFw);
  TEST_ASSERT_TRUE(send(n, "@hello 1 abc viz extra 42", 0).event == E::Enter);
}

// Versions below the Core2's lowest, malformed hellos, no known feature.
void test_hello_refusals() {
  HostLink l;
  l.begin(kFw);
  expectReply(send(l, "@hello 0 7f3a viz", 0), "@err 2 hello 1-1");
  expectReply(send(l, "@hello 1 7f3a setup", 0), "@err 7 hello");
  expectReply(send(l, "@hello 1 7f3a", 2000), "@err 1 hello");
  expectReply(send(l, "@hello x 7f3a viz", 2000), "@err 1 hello");
  expectReply(send(l, "@hello 1 7f-3a viz", 2000), "@err 1 hello");                 // session: [0-9A-Za-z]
  expectReply(send(l, "@hello 1 01234567890123456 viz", 2000), "@err 1 hello");  // 17 long
  expectReply(send(l, "@hello 1 7f3a Viz", 4000), "@err 1 hello");
  expectReply(send(l, "@hello 1 7f3a viz,", 4000), "@err 1 hello");
  expectReply(send(l, "@hello 1 7f3a ,viz", 4000), "@err 1 hello");
  TEST_ASSERT_FALSE(l.active());
  TEST_ASSERT_TRUE(send(l, "@hello 1 0123456789abcdef viz", 6000).event == E::Enter);  // 16: fine
}

// Each reason host mode can't start now: @err 4 with its detail, nothing changed.
void test_hello_busy() {
  const struct {
    Busy busy;
    const char* reply;
  } cases[] = {{Busy::Ui, "@err 4 hello ui"},
               {Busy::Screen, "@err 4 hello screen"},
               {Busy::Pairing, "@err 4 hello pairing"},
               {Busy::Dance, "@err 4 hello dance"}};
  uint32_t t = 0;
  HostLink l;
  l.begin(kFw);
  for (const auto& c : cases) {
    const HostLink::Out o = send(l, "@hello 1 7f3a viz", t += 1000, c.busy);
    TEST_ASSERT_TRUE(o.event == E::None);
    expectReply(o, c.reply);
    TEST_ASSERT_FALSE(l.active());
  }
  TEST_ASSERT_TRUE(send(l, "@hello 1 7f3a viz", t += 1000).event == E::Enter);
  // In a session nothing is busy any more: a retry is answered @ok.
  expectReply(send(l, "@hello 1 7f3a viz", t += 1000, Busy::Ui), "@ok 1 7f3a v0.5.0-beta.1 viz,log");
}

// The same id again (a retry that crossed the @ok): @ok again, nothing
// changes. Another id: the sender started over (Restart): the epoch goes,
// host mode stays.
void test_hello_again() {
  HostLink l = inSession(0);
  TEST_ASSERT_TRUE(send(l, "@h 1 0 0.5 0.5", 10).event == E::Hop);
  HostLink::Out o = send(l, "@hello 1 7f3a viz", 20);
  TEST_ASSERT_TRUE(o.event == E::None);
  expectReply(o, "@ok 1 7f3a v0.5.0-beta.1 viz,log");
  TEST_ASSERT_TRUE(l.haveEpoch());
  TEST_ASSERT_TRUE(send(l, "@h 1 1 0.5 0.5", 30).event == E::Hop);  // carries on

  o = send(l, "@hello 1 b2 viz", 40);
  TEST_ASSERT_TRUE(o.event == E::Restart);
  expectReply(o, "@ok 1 b2 v0.5.0-beta.1 viz,log");
  TEST_ASSERT_TRUE(l.active());
  TEST_ASSERT_EQUAL_STRING("b2", l.session());
  TEST_ASSERT_FALSE(l.haveEpoch());
  expectReply(send(l, "@h 1 2 0.5 0.5", 50), "@err 9 h");  // a new @e first
  // The same epoch number again is a new epoch now: the tracker starts over.
  TEST_ASSERT_TRUE(send(l, "@e 1 44100 0", 60).event == E::Epoch);
  o = send(l, "@h 1 2 0.5 0.5", 70);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_TRUE(o.restart);
  TEST_ASSERT_FALSE(o.gap);
}

// ---- data lines before their time ----

// With no session: @err 3; in one before any @e: @err 9 (h, c); @log needs
// only the session.
void test_data_without_session_or_epoch() {
  HostLink l;
  l.begin(kFw);
  expectReply(send(l, "@e 1 44100 0", 0), "@err 3 e");
  expectReply(send(l, "@h 1 0 0.5 0.5", 0), "@err 3 h");
  expectReply(send(l, "@c 1 0 1", 0), "@err 3 c");
  expectReply(send(l, "@log 1", 0), "@err 3 log");
  // @bye outside a session: ignored.
  const HostLink::Out bye = send(l, "@bye", 1100);
  TEST_ASSERT_TRUE(bye.event == E::None);
  expectReply(bye, "");
  HostLink m = inSession(2000, false);
  expectReply(send(m, "@h 1 0 0.5 0.5", 2000), "@err 9 h");
  expectReply(send(m, "@c 1 0 1", 2000), "@err 9 c");
  const HostLink::Out lg = send(m, "@log 2", 2000);
  TEST_ASSERT_TRUE(lg.event == E::Log);
  TEST_ASSERT_EQUAL_UINT8(2, lg.level);
  TEST_ASSERT_EQUAL_UINT8(2, m.logLevel());
}

// ---- epochs ----

void test_epochs() {
  HostLink l = inSession(0, false);
  HostLink::Out o = send(l, "@e 1 44100 120", 10);
  TEST_ASSERT_TRUE(o.event == E::Epoch);
  TEST_ASSERT_EQUAL_UINT32(1, o.epoch);
  TEST_ASSERT_EQUAL_UINT32(44100, o.rate);
  TEST_ASSERT_EQUAL_FLOAT(120.0f, o.prior);
  expectReply(o, "");
  // The same epoch, rate and prior: nothing.
  TEST_ASSERT_TRUE(send(l, "@e 1 44100 120", 20).event == E::None);
  // The same epoch and rate, another prior (late metadata): Prior only.
  o = send(l, "@e 1 44100 87.5", 30);
  TEST_ASSERT_TRUE(o.event == E::Prior);
  TEST_ASSERT_EQUAL_FLOAT(87.5f, o.prior);
  TEST_ASSERT_EQUAL_FLOAT(87.5f, l.prior());
  // Another rate within the epoch: refused, nothing changed.
  expectReply(send(l, "@e 1 48000 87.5", 40), "@err 5 e");
  TEST_ASSERT_EQUAL_UINT32(44100, l.rate());
  // Another epoch (any other number: only equality counts), at 48 kHz, no prior.
  o = send(l, "@e 0 48000 0", 1500);
  TEST_ASSERT_TRUE(o.event == E::Epoch);
  TEST_ASSERT_EQUAL_UINT32(0, l.epoch());
  TEST_ASSERT_EQUAL_UINT32(48000, l.rate());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, l.prior());
  TEST_ASSERT_EQUAL_UINT32(2, l.stats().epochs);
  // Out of range: rates other than 44.1/48 kHz, priors other than 0 or 30-300.
  uint32_t t = 3000;  // (300 ms apart: under the 4 a second limit on @err)
  for (const char* s : {"@e 2 22050 0", "@e 2 96000 0", "@e 2 0 0", "@e 2 44100 29.9", "@e 2 44100 300.5",
                        "@e 2 44100 -120"}) {
    expectReply(send(l, s, t += 300), "@err 5 e");
  }
  TEST_ASSERT_TRUE(send(l, "@e 2 44100 30", t += 300).event == E::Epoch);
  TEST_ASSERT_TRUE(send(l, "@e 3 44100 300", t += 300).event == E::Epoch);
  // Malformed.
  for (const char* s : {"@e", "@e 1 44100", "@e x 44100 0", "@e 1 44100 nan", "@e 1 44100.0 0", "@e -1 44100 0"}) {
    expectReply(send(l, s, t += 300), "@err 1 e");
  }
}

// ---- hops ----

void test_hops_first_next_duplicate_gap() {
  HostLink l = inSession(0);
  // The first hop after @e: any number (normally 0), with a restart there.
  HostLink::Out o = send(l, "@h 1 7 0.672406 1.09918", 10);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_TRUE(o.restart);
  TEST_ASSERT_FALSE(o.gap);
  TEST_ASSERT_EQUAL_UINT32(7, o.hop);
  TEST_ASSERT_EQUAL_FLOAT(0.672406f, o.low);
  TEST_ASSERT_EQUAL_FLOAT(1.09918f, o.mid);
  TEST_ASSERT_EQUAL_UINT32(44100, o.rate);
  expectReply(o, "");
  // The next: fed, no restart.
  o = send(l, "@h 1 8 0.0291153 0.00515993", 11);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_FALSE(o.restart);
  // The same or lower: duplicates, dropped and counted.
  TEST_ASSERT_TRUE(send(l, "@h 1 8 0.1 0.1", 12).event == E::None);
  TEST_ASSERT_TRUE(send(l, "@h 1 3 0.1 0.1", 12).event == E::None);
  TEST_ASSERT_EQUAL_UINT32(2, l.stats().dup);
  // Higher: a gap, the tracker restarts at the new hop.
  o = send(l, "@h 1 12 1.49884555e-15 0", 13);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_TRUE(o.restart);
  TEST_ASSERT_TRUE(o.gap);
  TEST_ASSERT_EQUAL_UINT32(12, o.hop);
  TEST_ASSERT_EQUAL_UINT32(1, l.stats().gaps);
  o = send(l, "@h 1 13 0 0", 14);
  TEST_ASSERT_FALSE(o.restart);
  TEST_ASSERT_EQUAL_UINT32(4, l.stats().hops);
  // A new epoch: its first hop restarts, whatever its number.
  send(l, "@e 2 48000 0", 15);
  o = send(l, "@h 2 0 0.5 0.5", 16);
  TEST_ASSERT_TRUE(o.restart);
  TEST_ASSERT_FALSE(o.gap);
  TEST_ASSERT_EQUAL_UINT32(48000, o.rate);
}

// Lines for another epoch than the current one: dropped, counted as stale,
// no reply (and no change: the next hop of the current one carries on).
void test_stale_epochs() {
  HostLink l = inSession(0);
  send(l, "@h 1 0 0.5 0.5", 10);
  send(l, "@e 2 44100 0", 20);
  send(l, "@h 2 0 0.5 0.5", 30);
  HostLink::Out o = send(l, "@h 1 1 0.5 0.5", 40);
  TEST_ASSERT_TRUE(o.event == E::None);
  expectReply(o, "");
  o = send(l, "@c 1 512 1", 40);
  TEST_ASSERT_TRUE(o.event == E::None);
  expectReply(o, "");
  TEST_ASSERT_EQUAL_UINT32(2, l.stats().stale);
  o = send(l, "@h 2 1 0.5 0.5", 50);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_FALSE(o.restart);
}

// Energies: 0 to 1e4, finite; malformed hops.
void test_hop_ranges() {
  HostLink l = inSession(0);
  TEST_ASSERT_TRUE(send(l, "@h 1 0 10000 0", 1).event == E::Hop);
  uint32_t t = 0;  // (300 ms apart: under the 4 a second limit on @err)
  for (const char* s : {"@h 1 1 -0.1 0.5", "@h 1 1 0.5 -1e-9", "@h 1 1 10001 0", "@h 1 1 0.5 2e4"}) {
    expectReply(send(l, s, t += 300), "@err 5 h");
    TEST_ASSERT_EQUAL_UINT32(1, l.stats().hops);
  }
  for (const char* s : {"@h 1 1 0.5", "@h 1 1 nan 0.5", "@h 1 1 inf 0", "@h 1 -1 0.5 0.5", "@h 1 1 .5 0.5",
                        "@h 1 1 0.5 1e39"}) {
    expectReply(send(l, s, t += 300), "@err 1 h");
  }
  // Nothing above was fed: the next is still hop 1.
  HostLink::Out o = send(l, "@h 1 1 0.5 0.5", t += 300);
  TEST_ASSERT_TRUE(o.event == E::Hop);
  TEST_ASSERT_FALSE(o.restart);
  // Extra trailing fields are ignored.
  o = send(l, "@h 1 2 0.5 0.5 later", t + 1);
  TEST_ASSERT_TRUE(o.event == E::Hop);
}

// ---- the clock ----

void test_clock_lines() {
  HostLink l = inSession(0);
  HostLink::Out o = send(l, "@c 1 -6615 1", 10);
  TEST_ASSERT_TRUE(o.event == E::Clock);
  TEST_ASSERT_EQUAL_INT32(-6615, o.heard);
  TEST_ASSERT_TRUE(o.playing);
  TEST_ASSERT_EQUAL_UINT32(1, o.epoch);
  TEST_ASSERT_EQUAL_UINT32(44100, o.rate);
  o = send(l, "@c 1 1323000 0", 20);
  TEST_ASSERT_TRUE(o.event == E::Clock);
  TEST_ASSERT_FALSE(o.playing);
  expectReply(send(l, "@c 1 1323000 2", 30), "@err 5 c");
  expectReply(send(l, "@c 1 1.5 1", 30), "@err 1 c");
  expectReply(send(l, "@c 1 2147483648 1", 30), "@err 1 c");
  expectReply(send(l, "@c 1 5", 1100), "@err 1 c");
  TEST_ASSERT_EQUAL_UINT32(2, l.stats().clocks);
}

void test_log_levels() {
  HostLink l = inSession(0);
  TEST_ASSERT_EQUAL_UINT8(0, l.logLevel());
  for (uint8_t level : {1, 2, 0}) {
    char s[16];
    snprintf(s, sizeof(s), "@log %u", level);
    const HostLink::Out o = send(l, s, 10);
    TEST_ASSERT_TRUE(o.event == E::Log);
    TEST_ASSERT_EQUAL_UINT8(level, o.level);
  }
  expectReply(send(l, "@log 3", 20), "@err 5 log");
  expectReply(send(l, "@log", 20), "@err 1 log");
  send(l, "@log 2", 30);
  send(l, "@bye", 40);
  TEST_ASSERT_EQUAL_UINT8(0, l.logLevel());  // back to off when the session ends
}

// ---- the ways out ----

void test_bye() {
  HostLink l = inSession(0);
  const HostLink::Out o = send(l, "@bye", 500);
  TEST_ASSERT_TRUE(o.event == E::Exit);
  TEST_ASSERT_TRUE(o.why == W::Bye);
  expectReply(o, "@bye ok");
  TEST_ASSERT_FALSE(l.active());
  TEST_ASSERT_FALSE(l.declined());
  expectReply(send(l, "@h 1 0 0.5 0.5", 600), "@err 3 h");  // gone: no session
  TEST_ASSERT_TRUE(send(l, "@hello 1 9 viz", 700).event == E::Enter);  // and back at once
}

// 3 s with no valid line: @bye timeout. Errors don't keep it alive; any
// valid line does (a paused sender's @c, a stale line, a duplicate hop).
void test_timeout() {
  HostLink l = inSession(1000);
  TEST_ASSERT_TRUE(l.poll(3999, true).event == E::None);
  send(l, "@c 1 0 0", 3500);  // paused: still alive
  TEST_ASSERT_TRUE(l.poll(6499, true).event == E::None);
  send(l, "@h 1 0 bad 0", 6000);  // an error: not alive
  send(l, "@zz", 6100);           // nor this
  TEST_ASSERT_TRUE(l.poll(6499, true).event == E::None);
  const HostLink::Out o = l.poll(6500, true);
  TEST_ASSERT_TRUE(o.event == E::Exit);
  TEST_ASSERT_TRUE(o.why == W::Timeout);
  expectReply(o, "@bye timeout");
  TEST_ASSERT_FALSE(l.active());
  TEST_ASSERT_FALSE(l.declined());
  TEST_ASSERT_TRUE(l.poll(10000, true).event == E::None);

  HostLink m = inSession(0);
  send(m, "@h 1 0 0.5 0.5", 2000);
  send(m, "@h 1 0 0.5 0.5", 4000);  // a duplicate: valid, alive
  TEST_ASSERT_TRUE(m.poll(6999, true).event == E::None);
  TEST_ASSERT_TRUE(m.poll(7000, true).event == E::Exit);
  // The millisecond counter wrapping doesn't end it early.
  HostLink w = inSession(0xFFFFFF00u);
  TEST_ASSERT_TRUE(w.poll(0xFFFFFF00u + 2999, true).event == E::None);
  TEST_ASSERT_TRUE(w.poll(0xFFFFFF00u + 3000, true).event == E::Exit);
}

// The loop reads millis() once a pass, then the console stamps that pass's
// lines with millis() again: poll() sees lines a few ms after its own now.
// That's no timeout and no end to a decline (unsigned, it was ~49 days of
// quiet: every session on the device ended in the pass it began).
void test_lines_stamped_after_the_polls_now() {
  HostLink l = inSession(1005);  // @hello and @e at 1005 ms
  TEST_ASSERT_TRUE(l.poll(1000, true).event == E::None);  // the pass's now: 1000
  TEST_ASSERT_TRUE(l.active());
  send(l, "@h 1 0 0.5 0.5", 2009);
  TEST_ASSERT_TRUE(l.poll(2003, true).event == E::None);
  TEST_ASSERT_TRUE(l.poll(5008, true).event == E::None);
  TEST_ASSERT_TRUE(l.poll(5009, true).event == E::Exit);  // 3 s after the last line
  // Across the counter's wrap too.
  HostLink w = inSession(2);
  TEST_ASSERT_TRUE(w.poll(0xFFFFFFFEu, true).event == E::None);
  TEST_ASSERT_TRUE(w.active());
  // A decline holds while lines come in, stamped after the pass's now.
  HostLink d = inSession(0);
  d.end(W::Touch, 1000);
  send(d, "@h 1 5 0.5 0.5", 1507);
  d.poll(1500, true);
  TEST_ASSERT_TRUE(d.declined());
  expectReply(send(d, "@hello 1 7f3a viz", 2010), "@err 8 hello");
  d.poll(2004, true);
  TEST_ASSERT_TRUE(d.declined());
}

// USB unplugged: out at once, nothing to say it on.
void test_unplugged() {
  HostLink l = inSession(0);
  const HostLink::Out o = l.poll(100, false);
  TEST_ASSERT_TRUE(o.event == E::Exit);
  TEST_ASSERT_TRUE(o.why == W::Unplugged);
  expectReply(o, "");
  TEST_ASSERT_FALSE(l.declined());
  // Not active: USB power doesn't matter.
  TEST_ASSERT_TRUE(l.poll(200, false).event == E::None);
}

// The user wins: a touch, a button or the headphones' play key ends it
// (@bye user), and everything the computer sends is declined (@err 8)
// until it has been quiet for 3 s; then it gets in again.
void test_user_exit_then_declined_then_accepted() {
  for (W why : {W::Touch, W::Button, W::HeadsetKey}) {
    HostLink l = inSession(0);
    HostLink::Out o = l.end(why, 1000);
    TEST_ASSERT_TRUE(o.event == E::Exit);
    TEST_ASSERT_TRUE(o.why == why);
    expectReply(o, "@bye user");
    TEST_ASSERT_TRUE(l.declined());
    // A sender that keeps streaming stays declined.
    expectReply(send(l, "@h 1 5 0.5 0.5", 1500), "@err 8 h");
    expectReply(send(l, "@c 1 5 1", 2000), "@err 8 c");
    expectReply(send(l, "@hello 1 7f3a viz", 4900), "@err 8 hello");
    expectReply(send(l, "@hello 1 new viz", 7800), "@err 8 hello");
    TEST_ASSERT_TRUE(l.poll(10700, true).event == E::None);
    TEST_ASSERT_TRUE(l.declined());  // 2.9 s of quiet
    // Malformed lines count as the computer talking too.
    send(l, "@h 1", 10700);
    TEST_ASSERT_TRUE(l.poll(13699, true).event == E::None);
    TEST_ASSERT_TRUE(l.declined());
    l.poll(13700, true);
    TEST_ASSERT_FALSE(l.declined());
    o = send(l, "@hello 1 again viz", 14000);
    TEST_ASSERT_TRUE(o.event == E::Enter);
  }
  // Without a poll in between: the quiet is measured up to the line itself.
  HostLink l = inSession(0);
  l.end(W::Touch, 1000);
  TEST_ASSERT_TRUE(send(l, "@hello 1 back viz", 4000).event == E::Enter);
  // A @bye while declined: ignored (no session), and it isn't quiet.
  l.end(W::Button, 5000);
  TEST_ASSERT_TRUE(send(l, "@bye", 7000).event == E::None);
  expectReply(send(l, "@hello 1 back viz", 9999), "@err 8 hello");
}

// The Dance tab gone some other way: @bye dance, not declined. And end()
// outside a session does nothing.
void test_dance_gone_and_end_when_idle() {
  HostLink l = inSession(0);
  const HostLink::Out o = l.end(W::DanceGone, 10);
  TEST_ASSERT_TRUE(o.event == E::Exit);
  expectReply(o, "@bye dance");
  TEST_ASSERT_FALSE(l.declined());
  const HostLink::Out again = l.end(W::Touch, 20);
  TEST_ASSERT_TRUE(again.event == E::None);
  expectReply(again, "");
  TEST_ASSERT_FALSE(l.declined());
}

// ---- malformed lines and the error limit ----

void test_bad_lines_and_unknown_verbs() {
  HostLink l;
  l.begin(kFw);
  expectReply(l.bad(HostLine::Byte::Bad, 0), "@err 1 -");
  expectReply(l.bad(HostLine::Byte::Long, 0), "@err 6 -");
  HostLink::Out o = l.bad(HostLine::Byte::Restart, 0);  // cut off by an '@': no reply
  expectReply(o, "");
  TEST_ASSERT_EQUAL_UINT32(3, l.stats().bad);
  expectReply(send(l, "@", 0), "@err 1 -");
  l = HostLink{};
  l.begin(kFw);
  expectReply(send(l, "@Hello 1 a viz", 0), "@err 1 -");
  expectReply(send(l, "@s 1 0 00ff", 0), "@err 7 s");  // reserved for later: not in caps
  expectReply(send(l, "@t 1 dGl0bGU YXJ0", 0), "@err 7 t");
  expectReply(send(l, "@wifi.set c3NpZA cGFzcw", 0), "@err 7 wifi.set");  // never echoes the fields
  expectReply(send(l, "@ping 1", 2000), "@err 7 ping");
  expectReply(send(l, "@ok 1 a v viz", 2000), "@err 7 ok");
}

// At most 4 @err a second; the rest only counted. A refusal never changes
// anything.
void test_error_rate_limit() {
  HostLink l = inSession(0);
  int sent = 0;
  for (int i = 0; i < 10; ++i) sent += send(l, "@zz", 5000 + i).reply[0] != '\0';
  TEST_ASSERT_EQUAL_INT(4, sent);
  TEST_ASSERT_EQUAL_UINT32(10, l.stats().errors);
  TEST_ASSERT_EQUAL_UINT32(6, l.stats().suppressed);
  expectReply(send(l, "@zz", 5999), "");
  expectReply(send(l, "@zz", 6000), "@err 7 zz");  // the next second
  // Replies that aren't errors aren't limited.
  expectReply(send(l, "@hello 1 7f3a viz", 6001), "@ok 1 7f3a v0.5.0-beta.1 viz,log");
  TEST_ASSERT_TRUE(l.active());
}

// A whole session as docs/USB-VISUALIZER.md "A session, line by line" shows it.
void test_a_session_line_by_line() {
  HostLink l;
  l.begin(kFw);
  uint32_t t = 0;
  expectReply(send(l, "@hello 1 7f3a viz", t, Busy::Ui), "@err 4 hello ui");
  expectReply(send(l, "@hello 1 7f3a viz", t += 1000), "@ok 1 7f3a v0.5.0-beta.1 viz,log");
  TEST_ASSERT_TRUE(send(l, "@e 1 44100 120", t += 5).event == E::Epoch);
  TEST_ASSERT_TRUE(send(l, "@c 1 -6615 1", t += 5).event == E::Clock);
  const char* hops[] = {"@h 1 0 0.672406 1.09918", "@h 1 1 0.0291153 0.00515993", "@h 1 2 0.00766506 5.37276e-05",
                        "@h 1 3 0.0036862 2.56443e-05"};
  for (int i = 0; i < 4; ++i) {
    const HostLink::Out o = send(l, hops[i], t += 3);
    TEST_ASSERT_TRUE(o.event == E::Hop);
    TEST_ASSERT_EQUAL(i == 0, o.restart);
  }
  TEST_ASSERT_TRUE(send(l, "@c 1 -2205 1", t += 50).event == E::Clock);
  TEST_ASSERT_FALSE(send(l, "@c 1 1323000 0", t += 50).playing);
  TEST_ASSERT_TRUE(send(l, "@c 1 1323000 1", t += 2900).playing);  // resumed: same epoch, hops carry on
  TEST_ASSERT_TRUE(send(l, "@h 1 4 0.1 0.1", t += 10).event == E::Hop);
  TEST_ASSERT_TRUE(send(l, "@e 2 48000 0", t += 10).event == E::Epoch);
  TEST_ASSERT_TRUE(send(l, "@h 2 0 0.1 0.1", t += 10).restart);
  const HostLink::Out bye = send(l, "@bye", t += 10);
  TEST_ASSERT_TRUE(bye.event == E::Exit);
  expectReply(bye, "@bye ok");
  const HostStats& s = l.stats();
  TEST_ASSERT_EQUAL_UINT32(2, s.epochs);
  TEST_ASSERT_EQUAL_UINT32(6, s.hops);
  TEST_ASSERT_EQUAL_UINT32(0, s.gaps);
  TEST_ASSERT_EQUAL_UINT32(0, s.errors);  // (the busy @err came before the session: not counted in it)
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_hello_enters);
  RUN_TEST(test_hello_refusals);
  RUN_TEST(test_hello_busy);
  RUN_TEST(test_hello_again);
  RUN_TEST(test_data_without_session_or_epoch);
  RUN_TEST(test_epochs);
  RUN_TEST(test_hops_first_next_duplicate_gap);
  RUN_TEST(test_stale_epochs);
  RUN_TEST(test_hop_ranges);
  RUN_TEST(test_clock_lines);
  RUN_TEST(test_log_levels);
  RUN_TEST(test_bye);
  RUN_TEST(test_timeout);
  RUN_TEST(test_lines_stamped_after_the_polls_now);
  RUN_TEST(test_unplugged);
  RUN_TEST(test_user_exit_then_declined_then_accepted);
  RUN_TEST(test_dance_gone_and_end_when_idle);
  RUN_TEST(test_bad_lines_and_unknown_verbs);
  RUN_TEST(test_error_rate_limit);
  RUN_TEST(test_a_session_line_by_line);
  return UNITY_END();
}
