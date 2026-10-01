// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for AbsVolumePolicy (who applies the Bluetooth volume).
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

#include "AbsVolumePolicy.h"
#include "VolumeMath.h"

using Mode = AbsVolumePolicy::Mode;

namespace {
uint16_t softwareGain(uint8_t percent) {
  return vol::mulQ15(vol::kHeadroomQ15, vol::softwareVolumeQ15(percent));
}

// Linked at 30 %, capabilities received, probe sent at t=1000.
AbsVolumePolicy probing() {
  AbsVolumePolicy p(30);
  p.linkUp(900);
  p.capabilities(true, 1000);
  return p;
}

// ...and the headphones accepted it at t=1100.
AbsVolumePolicy absolute() {
  AbsVolumePolicy p = probing();
  p.accepted(38, 1100);
  return p;
}

// Linked at 30 %, a stream running since t=100; no capabilities yet (their
// AVRCP comes up late, as with Powerbeats Pro).
AbsVolumePolicy playing() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  p.streamActive(true, 100);
  return p;
}

// ...their capabilities at t=1000 (a late probe: the dip), its command out
// kDuckSettleMs later.
constexpr uint32_t kLateCapsMs = 1000;
constexpr uint32_t kLateSentMs = kLateCapsMs + AbsVolumePolicy::kDuckSettleMs;
AbsVolumePolicy lateProbing() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, kLateCapsMs);
  p.tick(kLateSentMs);
  return p;
}

bool ramped(const AbsVolumePolicy::Actions& a) { return a.gainChanged && !a.snap && !a.lift; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_link_up_starts_in_software_mode_snapped() {
  AbsVolumePolicy p(30);
  const auto a = p.linkUp(0);
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_TRUE(a.snap);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  TEST_ASSERT_TRUE(p.gainQ15() < vol::kHeadroomQ15);
}

void test_link_up_caps_a_loud_volume() {
  AbsVolumePolicy p(90);
  const auto a = p.linkUp(0);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(AbsVolumePolicy::kMaxLinkUpPercent, p.percent());
  TEST_ASSERT_TRUE(a.snap);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(AbsVolumePolicy::kMaxLinkUpPercent), p.gainQ15());
  TEST_ASSERT_FALSE(AbsVolumePolicy(40).linkUp(0).volumeChanged);
}

void test_probe_sends_volume_but_keeps_attenuating() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  const auto a = p.capabilities(true, 10);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_TRUE(a.modeChanged);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
}

void test_capabilities_before_the_link_probe_on_link_up() {
  AbsVolumePolicy p(30);
  TEST_ASSERT_FALSE(p.capabilities(true, 0).sendAbsolute);
  const auto a = p.linkUp(5);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
}

void test_no_volume_notifications_stays_software() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  const auto a = p.capabilities(false, 10);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
}

// Nothing has been heard on the link (the stream waits for the answer), so
// the first stream fades in straight to the right level: a lift, no swell.
void test_accept_hands_volume_to_the_headphones_with_a_lift() {
  AbsVolumePolicy p = probing();
  const auto a = p.accepted(40, 1100);  // headphones rounded 38 to 40
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_TRUE(a.lift);
  TEST_ASSERT_FALSE(a.snap);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());  // the UI keeps what the user chose
  TEST_ASSERT_EQUAL_INT(40, p.headsetAbsolute());
}

void test_headphone_buttons_update_the_ui_and_are_not_echoed() {
  AbsVolumePolicy p = absolute();
  const auto a = p.headsetChanged(80, 9000);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(80), p.percent());
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
}

void test_echo_of_our_own_command_is_ignored() {
  AbsVolumePolicy p = absolute();
  const auto sent = p.userSet(50, 5000);
  TEST_ASSERT_TRUE(sent.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(64, sent.absolute);
  const auto a = p.headsetChanged(64, 5200);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(50, p.percent());
}

void test_quantised_echo_is_ignored() {
  AbsVolumePolicy p = absolute();
  p.userSet(40, 5000);  // sends 51; 16-step headphones land on 48
  const auto a = p.headsetChanged(48, 5300);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
}

void test_rapid_steps_do_not_bounce_the_ui() {
  AbsVolumePolicy p = absolute();
  p.userSet(40, 5000);  // 51
  p.userSet(50, 5100);  // 64
  p.userSet(60, 5200);  // 76
  TEST_ASSERT_FALSE(p.headsetChanged(48, 5300).volumeChanged);
  TEST_ASSERT_FALSE(p.headsetChanged(64, 5350).volumeChanged);
  TEST_ASSERT_FALSE(p.headsetChanged(80, 5400).volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(60, p.percent());
}

// A quantised echo the ACCEPT told us about.
void test_echo_of_what_they_accepted_is_ignored() {
  AbsVolumePolicy p = absolute();
  p.userSet(40, 5000);  // 51
  p.accepted(46, 5050);
  const auto a = p.headsetChanged(46, 5100);  // 5 off what we sent, but what they said
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
}

// One step of 16-step headphones (8) right after our command is theirs.
void test_their_step_right_after_ours_is_adopted() {
  AbsVolumePolicy p = absolute();
  p.userSet(50, 5000);  // 64
  const auto a = p.headsetChanged(64 - 8, 5500);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(56), p.percent());
}

// An echo is used up: a change of theirs that lands near it afterwards counts.
void test_an_echo_counts_once() {
  AbsVolumePolicy p = absolute();
  p.userSet(50, 5000);                                          // 64
  TEST_ASSERT_FALSE(p.headsetChanged(64, 5100).volumeChanged);  // the echo
  const auto a = p.headsetChanged(61, 5300);                    // theirs
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(61), p.percent());
}

void test_same_value_after_the_echo_window_is_adopted() {
  AbsVolumePolicy p = absolute();
  p.userSet(50, 5000);                          // 64
  const auto a = p.headsetChanged(60, 5000 + AbsVolumePolicy::kEchoWindowMs + 1);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(60), p.percent());
}

void test_notification_during_probe_proves_support() {
  AbsVolumePolicy p = probing();              // sent 38
  const auto a = p.headsetChanged(30, 1300);  // not an echo, but no louder than asked
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(30), p.percent());
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_TRUE(a.lift);
  // Their level, sent back: the last command they get is theirs.
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(30, a.absolute);
}

// The review's case: their notification crossed our probe, which lands
// after the handover. Their level, sent after it, ends it; neither the
// probe's echo nor that of their level moves the UI.
void test_their_level_is_sent_after_a_probe_still_on_its_way() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 10).absolute);
  const auto a = p.headsetChanged(20, 100);  // theirs, before our 64 landed
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), p.percent());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(20, a.absolute);
  p.streamActive(true, 150);
  TEST_ASSERT_FALSE(p.accepted(64, 200).volumeChanged);        // the probe, applied
  TEST_ASSERT_FALSE(p.headsetChanged(64, 210).volumeChanged);  // ...and its echo
  TEST_ASSERT_FALSE(p.accepted(20, 250).volumeChanged);        // theirs, back
  TEST_ASSERT_FALSE(p.headsetChanged(20, 260).volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), p.percent());
  // Their next step is theirs again.
  TEST_ASSERT_TRUE(p.headsetChanged(28, 2000).volumeChanged);
}

// The recheck's case, with the Powerbeats Pro's timing (a SET answered 1 s
// after it): the probe (64) is out, the listener presses their VOL- (20)
// before it lands. The handover at their level sends it back, and the first
// stream waits for its answer: the probe lands first, while nothing plays,
// instead of stepping them up by ~14 dB in the first stream. (The gate used
// to open at once.)
void test_their_report_during_the_probe_holds_the_first_stream() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
  const auto a = p.headsetChanged(20, 300);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.lift);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(20, a.absolute);
  TEST_ASSERT_FALSE(p.audioReady(300));
  p.accepted(64, 1000);  // the probe lands: still waiting for ours
  TEST_ASSERT_FALSE(p.audioReady(1000));
  p.tick(1100);
  TEST_ASSERT_FALSE(p.audioReady(1100));
  p.accepted(20, 1300);  // their level back, answered
  TEST_ASSERT_TRUE(p.audioReady(1300));
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), p.percent());

  // Headphones that only notify: no notification counts while the probe
  // has no ACCEPT. Its echo can't be told from a key of theirs near it,
  // and that of their level back from a key near it with the probe still
  // on its way: neither answers anything, nothing goes back, the UI stays
  // at their level back (it lands last, either way), and the stream waits
  // out the timeout. (The echoes used to release it at 1310: see
  // test_a_key_near_the_probe_keeps_it_unanswered for what that let through.)
  AbsVolumePolicy q(50);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.headsetChanged(20, 300);
  const auto e = q.headsetChanged(64, 1010);
  TEST_ASSERT_FALSE(e.volumeChanged);
  TEST_ASSERT_FALSE(e.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), q.percent());
  TEST_ASSERT_FALSE(q.audioReady(1010));
  const auto back = q.headsetChanged(20, 1310);
  TEST_ASSERT_FALSE(back.sendAbsolute);
  TEST_ASSERT_FALSE(back.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), q.percent());
  TEST_ASSERT_FALSE(q.audioReady(1310));
  TEST_ASSERT_FALSE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs - 1));
  TEST_ASSERT_TRUE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));

  // No answer (headphones that report nothing): the timeout.
  AbsVolumePolicy r(50);
  r.linkUp(0);
  r.capabilities(true, 0);
  r.headsetChanged(20, 300);
  TEST_ASSERT_FALSE(r.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs - 1));
  TEST_ASSERT_TRUE(r.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));

  // They rejected the probe (ESP-IDF reports only ACCEPTs): the one ACCEPT
  // pairs with it by order, not near what ours asked for. It can't be told
  // from the probe's, so the stream waits out the timeout.
  AbsVolumePolicy s(50);
  s.linkUp(0);
  s.capabilities(true, 0);
  s.headsetChanged(20, 300);
  s.accepted(20, 1300);
  TEST_ASSERT_FALSE(s.audioReady(1300));
  s.tick(300 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_TRUE(s.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));

  // A new link starts afresh.
  AbsVolumePolicy t(50);
  t.linkUp(0);
  t.capabilities(true, 0);
  t.headsetChanged(20, 300);
  t.linkDown();
  t.avrcpDown();
  t.linkUp(500);
  t.capabilities(false, 600);
  TEST_ASSERT_TRUE(t.audioReady(600));
}

// After an unanswered probe and a stream the headphones started (so their
// report isn't a handover), their report sends their level back after the
// probe that may still be on its way: a new stream waits for its answer too.
// (Software once audio has flowed used to open the gate at once.)
void test_their_report_after_an_unanswered_probe_holds_a_new_stream() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  p.capabilities(true, 0);  // 64
  p.streamActive(true, 100);  // theirs; ESP-IDF suspends it again
  p.streamActive(false, 150);
  p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // unanswered
  TEST_ASSERT_TRUE(p.audioReady(AbsVolumePolicy::kProbeTimeoutMs));  // heard: no grace
  const auto a = p.headsetChanged(20, 2500);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(20, a.absolute);
  TEST_ASSERT_FALSE(p.audioReady(2500));
  p.accepted(64, 3000);  // the late probe
  TEST_ASSERT_FALSE(p.audioReady(3000));
  p.accepted(20, 3500);
  TEST_ASSERT_TRUE(p.audioReady(3500));
}

// The recheck's case again, with headphones that step by 4 (32 steps): a
// key of theirs lands within kEchoTolerance of their level sent back while
// the probe is still out. It can't be told from the echo of ours, and the
// probe hasn't been answered: not an answer, nothing is used up, and the
// stream still waits for the answers. The UI stays at their level back:
// the probe, then it, land after the key, and they end there. (It used to
// be taken as the echo: the gate opened, and the probe, no longer known as
// ours, stepped them up ~12-15 dB in the first stream.)
void test_a_key_near_their_level_back_keeps_the_first_stream_held() {
  for (const uint8_t key : {16, 24}) {
    AbsVolumePolicy p(50);
    p.linkUp(0);
    TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
    TEST_ASSERT_EQUAL_UINT8(20, p.headsetChanged(20, 300).absolute);
    const auto k = p.headsetChanged(key, 400);
    TEST_ASSERT_FALSE(k.volumeChanged);
    TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), p.percent());
    TEST_ASSERT_FALSE(k.sendAbsolute);
    TEST_ASSERT_FALSE(p.audioReady(400));
    p.accepted(64, 1000);  // the probe lands while nothing plays
    TEST_ASSERT_FALSE(p.headsetChanged(64, 1010).volumeChanged);  // still known as ours
    TEST_ASSERT_FALSE(p.audioReady(1010));
    p.accepted(20, 1300);
    TEST_ASSERT_TRUE(p.audioReady(1300));
    TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), p.percent());
  }
  // Headphones that only notify: the probe never gets an ACCEPT, so no
  // notification answers it, nor releases the stream: the probe's echo
  // (64) could as well be a key of theirs near it with the probe still on
  // its way, and so could the one near their level back. The UI stays at
  // their level back, which lands last; the stream waits out the timeout.
  // (The probe's echo used to answer it, and the one near their level back
  // then released the stream at 1310: that is what a key of theirs near the
  // probe, then one near their level back, did too, with the probe still to
  // land in the first stream; see test_a_key_near_the_probe_keeps_it_unanswered.)
  AbsVolumePolicy q(50);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.headsetChanged(20, 300);
  q.headsetChanged(16, 400);
  TEST_ASSERT_FALSE(q.headsetChanged(64, 1010).volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), q.percent());
  TEST_ASSERT_FALSE(q.audioReady(1010));
  TEST_ASSERT_FALSE(q.headsetChanged(20, 1310).volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), q.percent());
  TEST_ASSERT_FALSE(q.audioReady(1310));
  TEST_ASSERT_FALSE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs - 1));
  TEST_ASSERT_TRUE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));
}

// The recheck's case (pre-audio, 30 %: the probe asks 38; headphones that
// land a command ~2 s after it, keys of 8 steps): their VOL- (28) hands over
// and goes back held; their VOL+ (36) lands near the probe, not near 28; their
// VOL- (28) then lands near their level back. Neither is an answer: the
// probe has no ACCEPT, so neither uses anything up, the probe stays
// unanswered and in the ACCEPT order, and the stream waits. The UI stays at
// 28: the probe, then 28, land after the keys, and they end there. (The
// VOL+ used to be taken as the probe's echo and use it up, so the VOL- near
// 28 released the stream at 1000: the probe then landed ~1 s into it,
// stepping them from 28 up to 38.)
void test_a_key_near_the_probe_keeps_it_unanswered() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(38, p.capabilities(true, 0).absolute);
  const auto h = p.headsetChanged(28, 300);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(h.lift);
  TEST_ASSERT_TRUE(h.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(28, h.absolute);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  const auto up = p.headsetChanged(36, 700);
  TEST_ASSERT_FALSE(up.volumeChanged);
  TEST_ASSERT_FALSE(up.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  TEST_ASSERT_FALSE(p.audioReady(700));
  const auto down = p.headsetChanged(28, 1000);
  TEST_ASSERT_FALSE(down.volumeChanged);
  TEST_ASSERT_FALSE(down.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  TEST_ASSERT_FALSE(p.audioReady(1000));  // (it used to open here)
  const auto landed = p.accepted(38, 2000);  // the probe: its ACCEPT pairs with it
  TEST_ASSERT_FALSE(landed.gainChanged);
  TEST_ASSERT_FALSE(landed.sendAbsolute);
  TEST_ASSERT_FALSE(p.audioReady(2000));
  p.accepted(28, 2200);  // their level back: answered, and in order
  TEST_ASSERT_TRUE(p.audioReady(2200));
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());

  // No ACCEPTs at all: the timeout.
  AbsVolumePolicy q(30);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.headsetChanged(28, 300);
  q.headsetChanged(36, 700);
  q.headsetChanged(28, 1000);
  TEST_ASSERT_FALSE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs - 1));
  TEST_ASSERT_TRUE(q.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));
}

// The same keys during a late probe's dip (Powerbeats Pro timing, the probe
// out into silence): the silence holds until their level back is answered,
// and our gain only then ramps up from it. (The VOL- near 28 used to end
// the silence, and the gain ramped up with the probe still on its way.)
void test_a_key_near_the_probe_keeps_the_dip_silent() {
  const uint32_t caps = 1100, sent = caps + AbsVolumePolicy::kDuckSettleMs;
  auto keys = [&](AbsVolumePolicy& p) {
    p.capabilities(true, caps);
    TEST_ASSERT_EQUAL_UINT8(38, p.tick(sent).absolute);
    const auto h = p.headsetChanged(28, sent + 300);
    TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
    TEST_ASSERT_TRUE(h.sendAbsolute);
    TEST_ASSERT_EQUAL_UINT8(28, h.absolute);
    const auto up = p.headsetChanged(36, sent + 700);
    TEST_ASSERT_FALSE(up.volumeChanged);
    TEST_ASSERT_FALSE(up.gainChanged);
    const auto down = p.headsetChanged(28, sent + 1000);
    TEST_ASSERT_FALSE(down.volumeChanged);
    TEST_ASSERT_FALSE(down.gainChanged);  // (it used to ramp up here)
    TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
    TEST_ASSERT_TRUE(p.ducked());
    TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
    TEST_ASSERT_FALSE(p.tick(sent + 1100).gainChanged);
  };
  AbsVolumePolicy p = playing();  // 30 %
  keys(p);
  TEST_ASSERT_FALSE(p.accepted(38, sent + 2000).gainChanged);  // lands in silence
  TEST_ASSERT_TRUE(p.ducked());
  const auto g = p.accepted(28, sent + 2200);
  TEST_ASSERT_TRUE(ramped(g));
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());

  AbsVolumePolicy q = playing();  // no ACCEPTs: the timeout
  keys(q);
  TEST_ASSERT_FALSE(q.tick(sent + 300 + AbsVolumePolicy::kProbeTimeoutMs - 1).gainChanged);
  TEST_ASSERT_TRUE(ramped(q.tick(sent + 300 + AbsVolumePolicy::kProbeTimeoutMs)));
}

// Answers out of order, no key near the probe: the probe (38) is out, the
// user raises the Core2 to 35 % (44), and their report of 30 hands over
// and goes back held. The probe lands, and its notification comes before
// its ACCEPT: not an answer, not used up, so the ACCEPT still pairs with it
// and the user's 44 stays unanswered; their key near 30 then doesn't
// release the stream. Neither moves the UI from 30: 44, then 30, land
// after them. (The notification used to use the probe up, its ACCEPT
// paired with 44, and the key released the stream: 44 landed in it, 14
// steps above the volume shown.)
void test_a_notification_before_its_accept_keeps_the_order() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(38, p.capabilities(true, 0).absolute);
  TEST_ASSERT_EQUAL_UINT8(44, p.userSet(35, 100).absolute);
  const auto h = p.headsetChanged(30, 300);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_EQUAL_UINT8(30, h.absolute);
  TEST_ASSERT_FALSE(p.headsetChanged(38, 1000).volumeChanged);  // the probe landed
  TEST_ASSERT_FALSE(p.accepted(38, 1010).sendAbsolute);         // ...its ACCEPT, paired with it
  TEST_ASSERT_FALSE(p.headsetChanged(30, 1200).volumeChanged);  // their VOL-, near 30
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(30), p.percent());
  TEST_ASSERT_FALSE(p.audioReady(1200));  // (it used to open here)
  p.accepted(44, 1500);  // the user's lands, still before anything is heard
  TEST_ASSERT_FALSE(p.audioReady(1500));
  p.accepted(30, 1800);  // their level back
  TEST_ASSERT_TRUE(p.audioReady(1800));
}

// No keys, the probe late (pre-audio, 30 %: 38), headphones that notify
// before they ACCEPT: their report of 28 hands over and goes back held; the
// probe then lands, its notification first. It isn't an answer (no ACCEPT
// yet), nor does it move the UI: 28 lands after it. Its ACCEPT pairs with
// it, and the echo of 28 releases the stream with the UI at 28, where they
// are. (The probe's notification, taken as theirs, used to move the UI to
// 30 %: the stream played at 28 under it, and their VOL- from there sent 30,
// a step up.) The same during a late probe's dip: the silence ends with
// the UI at 28.
void test_the_probe_landing_before_its_accept_leaves_the_ui_at_their_level() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(38, p.capabilities(true, 0).absolute);
  TEST_ASSERT_EQUAL_UINT8(28, p.headsetChanged(28, 300).absolute);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  const auto landed = p.headsetChanged(38, 1000);  // the probe, before its ACCEPT
  TEST_ASSERT_FALSE(landed.volumeChanged);
  TEST_ASSERT_FALSE(landed.sendAbsolute);
  TEST_ASSERT_FALSE(p.accepted(38, 1010).volumeChanged);
  TEST_ASSERT_FALSE(p.audioReady(1010));
  TEST_ASSERT_FALSE(p.headsetChanged(28, 1300).volumeChanged);  // 28's echo: answered
  TEST_ASSERT_TRUE(p.audioReady(1300));
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  p.accepted(28, 1310);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  const auto down = p.userSet(p.percent() - 6, 3000);  // their VOL-: a step down
  TEST_ASSERT_TRUE(down.sendAbsolute);
  TEST_ASSERT_TRUE(down.absolute < 28);

  const uint32_t caps = 1100, sent = caps + AbsVolumePolicy::kDuckSettleMs;
  AbsVolumePolicy q = playing();  // 30 %
  q.capabilities(true, caps);
  TEST_ASSERT_EQUAL_UINT8(38, q.tick(sent).absolute);
  TEST_ASSERT_EQUAL_UINT8(28, q.headsetChanged(28, sent + 300).absolute);
  TEST_ASSERT_FALSE(q.headsetChanged(38, sent + 1000).volumeChanged);
  TEST_ASSERT_FALSE(q.accepted(38, sent + 1010).gainChanged);
  TEST_ASSERT_TRUE(q.ducked());
  const auto g = q.headsetChanged(28, sent + 1300);
  TEST_ASSERT_TRUE(ramped(g));
  TEST_ASSERT_FALSE(g.volumeChanged);
  TEST_ASSERT_FALSE(q.ducked());
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), q.percent());
}

// Three Core2 presses during the probe fill the history, and their level
// sent back pushes the probe out of it with no ACCEPT: the ACCEPTs now pair
// one command late (the probe's with the first press, ..., the second
// press's with the third, still on its way). A notification near their level
// back doesn't release the stream then: that is left to an ACCEPT near it
// (by order, the one of the command before it, once all before it have
// landed) or the timeout. (Their key used to release it with the third
// press still on its way.) The same once a notification before the hold has
// used up a command of ours with no ACCEPT: it may have been a key of theirs
// near it, and that command may still be on its way.
void test_a_command_lost_unanswered_keeps_the_hold_from_notifications() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(38, p.capabilities(true, 0).absolute);
  TEST_ASSERT_EQUAL_UINT8(41, p.userSet(32, 100).absolute);
  TEST_ASSERT_EQUAL_UINT8(43, p.userSet(34, 150).absolute);
  TEST_ASSERT_EQUAL_UINT8(46, p.userSet(36, 200).absolute);
  TEST_ASSERT_EQUAL_UINT8(28, p.headsetChanged(28, 300).absolute);  // held; the probe leaves the history
  p.accepted(38, 1000);  // the probe's, paired with 41
  p.accepted(41, 1100);  // ...41's with 43
  p.accepted(43, 1200);  // ...43's with 46, still on its way
  const auto k = p.headsetChanged(30, 1300);  // a key of theirs near 28 (28 lands after it)
  TEST_ASSERT_FALSE(k.volumeChanged);
  TEST_ASSERT_FALSE(k.sendAbsolute);
  TEST_ASSERT_FALSE(p.audioReady(1300));  // (it used to open here)
  p.accepted(46, 1400);  // 46's, paired with 28: not near it, left to the wait
  TEST_ASSERT_FALSE(p.audioReady(1400));
  TEST_ASSERT_FALSE(p.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs - 1));
  TEST_ASSERT_TRUE(p.audioReady(300 + AbsVolumePolicy::kProbeTimeoutMs));

  // Used up before the hold: the probe (64 at 50 %) is out, the user steps
  // down to 30 % (38), a key of theirs lands near 64 (taken as its echo,
  // louder than ours: ours again), then their report of 30 hands over, held.
  // The ACCEPTs of 64 and 38 pair one late, with 38 and ours again; a key
  // near 30 doesn't release the stream with ours again still on its way.
  AbsVolumePolicy q(50);
  q.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, q.capabilities(true, 0).absolute);
  TEST_ASSERT_EQUAL_UINT8(38, q.userSet(30, 100).absolute);
  TEST_ASSERT_EQUAL_UINT8(38, q.headsetChanged(62, 200).absolute);  // ours again
  TEST_ASSERT_EQUAL(Mode::Probing, q.mode());
  TEST_ASSERT_EQUAL_UINT8(30, q.headsetChanged(30, 400).absolute);  // held
  TEST_ASSERT_EQUAL(Mode::Absolute, q.mode());
  q.accepted(64, 1000);
  q.accepted(38, 1100);
  TEST_ASSERT_FALSE(q.headsetChanged(33, 1200).volumeChanged);
  TEST_ASSERT_FALSE(q.audioReady(1200));  // (it used to open here)
  q.accepted(38, 1300);  // ours again, paired with 30: not near it
  TEST_ASSERT_FALSE(q.audioReady(1300));
  TEST_ASSERT_TRUE(q.audioReady(400 + AbsVolumePolicy::kProbeTimeoutMs));
}

// Headphones slow enough that the probe (64, at 50 %) went unanswered, audio
// has played, and the user stepped down on the Core2 to 10 % (13, not sent).
// A key of theirs reports 28: above the volume shown. The probe may still be
// on its way; with nothing after it, it would step them up to 64 and stay.
// So the volume shown goes after it (below what they have: no step up), and
// a new stream waits for its answer. (Only their level at or below the
// volume shown used to go back: nothing did here.)
void test_their_report_above_the_volume_shown_still_ends_below_it() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  p.capabilities(true, 0);
  p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // unanswered
  p.streamActive(true, 3000);
  TEST_ASSERT_FALSE(p.userSet(10, 3200).sendAbsolute);
  p.streamActive(false, 3900);
  const auto a = p.headsetChanged(28, 4000);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(13, a.absolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(p.audioReady(4000));
  const auto b = p.accepted(64, 4500);  // the probe lands: no longer an answer to wait for
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.audioReady(4500));
  p.accepted(16, 5000);  // 13, as 16-step headphones set it
  TEST_ASSERT_TRUE(p.audioReady(5000));
  TEST_ASSERT_EQUAL_UINT8(10, p.percent());
}

// Louder than the probe asked for: asked again, the link-up cap holds.
void test_louder_notification_during_probe_is_answered_with_ours() {
  AbsVolumePolicy p = probing();
  const auto a = p.headsetChanged(90, 1300);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
  TEST_ASSERT_TRUE(p.headsetChanged(38, 1400).lift);  // they took it
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
}

void test_probe_times_out_to_software() {
  AbsVolumePolicy p = probing();
  TEST_ASSERT_FALSE(p.tick(1000 + AbsVolumePolicy::kProbeTimeoutMs - 1).modeChanged);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  const auto a = p.tick(1000 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_TRUE(a.modeChanged);
  TEST_ASSERT_FALSE(a.gainChanged);  // already at the software level
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // No second probe on the same link.
  TEST_ASSERT_FALSE(p.capabilities(true, 9000).sendAbsolute);
}

void test_probe_timeout_survives_millis_wraparound() {
  AbsVolumePolicy p(30);
  p.linkUp(0xFFFFFF00u);
  p.capabilities(true, 0xFFFFFF00u);
  p.tick(0x00000500u);  // 1536 ms later
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  p.tick(0x00000800u);  // 2304 ms later
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
}

// Their level is unknown (they may have kept their own, rejected the probe,
// or apply absolute volume only while streaming): any command could be the
// first they apply, at once, from their own level. So nothing is sent for
// the user's volume, a step down included (it used to be sent: at 20 %
// ours, 57 for a step down from 50 % to 45 % took them there, +10 dB);
// our gain alone follows it.
void test_after_an_unanswered_probe_nothing_is_sent() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
  const auto t = p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // timed out
  TEST_ASSERT_TRUE(t.probeUnanswered);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  p.streamActive(true, 3000);
  const auto s = p.userSet(45, 4000);  // a step down: our gain only
  TEST_ASSERT_TRUE(s.gainChanged);
  TEST_ASSERT_FALSE(s.snap);
  TEST_ASSERT_FALSE(s.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(45), p.gainQ15());
  TEST_ASSERT_FALSE(p.userSet(60, 4100).sendAbsolute);  // a rise
  TEST_ASSERT_FALSE(p.userSet(5, 4200).sendAbsolute);   // far down
  TEST_ASSERT_EQUAL_UINT16(softwareGain(5), p.gainQ15());
  // The same before anything was heard.
  AbsVolumePolicy q = probing();  // 38
  q.tick(1000 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_FALSE(q.userSet(20, 3500).sendAbsolute);
  TEST_ASSERT_FALSE(q.userSet(40, 3600).sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, q.mode());
}

// The probe (38) times out and the stream plays at the software level; then
// its answer arrives. They apply what we send: they take over, and our gain
// rises to the headroom at the gain stage's slow rate (not a lift: audio
// plays). The user's step down since was our gain's alone (not sent): ours,
// below what they confirmed, goes out now that they apply our commands, and
// our gain only rises once it is answered. (It used to rise at once, with
// them still at 38: see test_late_accept_after_a_step_down_holds_the_rise.)
void test_late_accept_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  TEST_ASSERT_FALSE(p.userSet(20, 6000).sendAbsolute);  // 25: not sent
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);  // still the software level
  TEST_ASSERT_EQUAL_UINT16(softwareGain(20), p.gainQ15());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, a.absolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(20, p.percent());
  const auto b = p.accepted(24, 7500);  // its answer (16-step headphones)
  TEST_ASSERT_TRUE(ramped(b));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_FALSE(b.sendAbsolute);
}

// The recheck's case: headphones slow enough that the probe (64, at 50 %)
// went unanswered, the stream plays at the software level, and the user
// steps down on the Core2 to 10 % (our gain only: nothing is sent there).
// Then they confirm the probe: they are at 64 now, and 13 goes out. Our gain
// stays at the software level for 10 % until 13 is answered; rising at once
// it would have taken what they play to absolute volume for 50 %, ~16 dB
// above what the user chose, until 13 landed. They took 3.5 s to confirm, so
// the wait lasts as long; after it, the rise goes ahead anyway.
void test_late_accept_after_a_step_down_holds_the_rise() {
  auto slow = [](AbsVolumePolicy& p) {
    p.linkUp(0);
    TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
    p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // unanswered
    p.streamActive(true, 3000);
    TEST_ASSERT_FALSE(p.userSet(10, 3200).sendAbsolute);
  };
  AbsVolumePolicy p(50);
  slow(p);
  const auto a = p.accepted(64, 3500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(13, a.absolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(10), p.gainQ15());
  TEST_ASSERT_FALSE(p.userSet(15, 4000).gainChanged);  // the user's own step: sent, our gain stays
  TEST_ASSERT_FALSE(p.userSet(10, 4100).gainChanged);
  TEST_ASSERT_FALSE(p.tick(5000).gainChanged);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(10), p.gainQ15());
  TEST_ASSERT_FALSE(p.audioReady(5000));
  const auto b = p.accepted(16, 6900);  // 13, as 16-step headphones set it
  TEST_ASSERT_TRUE(ramped(b));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_TRUE(p.audioReady(6900));
  TEST_ASSERT_EQUAL_UINT8(10, p.percent());

  // Its echo releases it as well.
  AbsVolumePolicy q(50);
  slow(q);
  q.accepted(64, 3500);
  TEST_ASSERT_TRUE(ramped(q.headsetChanged(16, 4900)));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, q.gainQ15());

  // No answer: as long as they took to confirm (3.5 s), then the rise.
  AbsVolumePolicy r(50);
  slow(r);
  r.accepted(64, 3500);
  TEST_ASSERT_FALSE(r.tick(3500 + 3500 - 1).gainChanged);
  TEST_ASSERT_FALSE(r.audioReady(3500 + 3500 - 1));
  const auto c = r.tick(3500 + 3500);
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, r.gainQ15());
  TEST_ASSERT_TRUE(r.audioReady(3500 + 3500));

  // A very late confirmation waits at most kHoldMaxMs.
  AbsVolumePolicy s(50);
  slow(s);
  s.accepted(64, 20000);
  TEST_ASSERT_FALSE(s.tick(20000 + AbsVolumePolicy::kHoldMaxMs - 1).gainChanged);
  TEST_ASSERT_TRUE(ramped(s.tick(20000 + AbsVolumePolicy::kHoldMaxMs)));

  // AVRCP goes: nothing waits any more, software volume.
  AbsVolumePolicy t(50);
  slow(t);
  t.accepted(64, 3500);
  t.avrcpDown();
  TEST_ASSERT_EQUAL(Mode::Software, t.mode());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(10), t.gainQ15());
  TEST_ASSERT_TRUE(t.audioReady(3600));
}

// A rise after the timeout was only our gain's, never sent: a late answer
// leaves them at the last value sent, and the volume shown comes down to
// it. Nothing is sent: they don't step up while audio plays.
void test_late_accept_after_a_rise_keeps_their_level() {
  AbsVolumePolicy p = probing();  // 38
  p.tick(5000);
  p.streamActive(true, 5100);
  TEST_ASSERT_FALSE(p.userSet(70, 6000).sendAbsolute);
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
  // The next step is one from 38, not from 89.
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(25), p.userSet(25, 7000).absolute);
}

// The same with an echo of a command of ours instead of an ACCEPT. (It used
// to be the echo of the user's step down sent after the timeout; nothing of
// the user's is sent there now, and an echo is only recognised within
// kEchoWindowMs of its command, so here it is that of ours again, sent in
// reply to a stale ACCEPT.)
void test_late_echo_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();  // 38
  p.tick(5000);
  p.streamActive(true, 5100);
  TEST_ASSERT_EQUAL_UINT8(38, p.accepted(127, 6000).absolute);  // not an answer: ours again
  const auto a = p.headsetChanged(36, 6300);                    // 16-step headphones
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
}

// An ACCEPT above what its command asked for is not an answer (a stale one,
// or a level of their own): ours goes again, and the probe waits on. Before
// anything was heard...
void test_accept_above_what_we_asked_is_not_an_answer() {
  AbsVolumePolicy p = probing();  // 38 at t=1000
  const auto a = p.accepted(127, 1100);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  TEST_ASSERT_FALSE(p.audioReady(1100));
  // The timeout restarted with it.
  TEST_ASSERT_FALSE(p.tick(1000 + AbsVolumePolicy::kProbeTimeoutMs).modeChanged);
  // Rounded up by less than kEchoTolerance counts.
  const auto b = p.accepted(38 + AbsVolumePolicy::kEchoTolerance, 1200);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(b.lift);
}

// ...during a late probe's dip (still silent)...
void test_accept_above_what_we_asked_during_the_dip() {
  AbsVolumePolicy p = lateProbing();  // 38
  const auto a = p.accepted(100, kLateSentMs + 100);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_TRUE(p.ducked());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  const auto b = p.accepted(38, kLateSentMs + 200);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(b));
}

// ...and after the probe timed out, as a late confirmation: not taken; they
// are brought back down to ours.
void test_late_accept_above_what_we_asked_is_not_a_confirmation() {
  AbsVolumePolicy p = probing();  // 38
  p.tick(5000);
  p.streamActive(true, 5100);
  const auto a = p.accepted(127, 6000);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  // The answer to that repeat, again above it: not asked a third time.
  TEST_ASSERT_FALSE(p.userSet(70, 6100).sendAbsolute);  // a rise since: our gain only
  const auto b = p.accepted(127, 6200);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // (A step down used to be sent here, and answered again: nothing of the
  // user's goes out after an unanswered probe now.) A further stale ACCEPT
  // finds no command of ours waiting: ignored.
  TEST_ASSERT_FALSE(p.userSet(20, 6300).sendAbsolute);
  const auto c = p.accepted(127, 6400);
  TEST_ASSERT_FALSE(c.sendAbsolute);
  TEST_ASSERT_FALSE(c.gainChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
}

// Headphones that can't set our level (they round up by more than
// kEchoTolerance, or have a floor) answer every command above it. Ours goes
// again once, not in reply to the answer to that repeat: no endless
// SET/ACCEPT trade. The probe runs out, and stays out: the user's volume
// isn't sent after it.
void test_headphones_that_round_up_are_not_asked_for_ever() {
  AbsVolumePolicy p(28);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(36, p.capabilities(true, 10).absolute);
  const auto a = p.accepted(42, 100);  // their next step up
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(36, a.absolute);
  TEST_ASSERT_FALSE(p.accepted(42, 200).sendAbsolute);        // the answer to the repeat
  TEST_ASSERT_FALSE(p.headsetChanged(42, 210).sendAbsolute);  // ...and its echo
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  // Neither restarted the timeout (the repeat did).
  const auto t = p.tick(100 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_TRUE(t.probeUnanswered);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // Answers still on their way after the timeout: not asked again either.
  TEST_ASSERT_FALSE(p.accepted(42, 2200).sendAbsolute);
  TEST_ASSERT_FALSE(p.headsetChanged(42, 2210).sendAbsolute);
  // A key of theirs is news: asked once more (the cap holds).
  const auto k = p.headsetChanged(50, 2500);
  TEST_ASSERT_TRUE(k.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(36, k.absolute);
  TEST_ASSERT_FALSE(p.accepted(42, 2600).sendAbsolute);
  // (A step down they could set used to be sent, and confirmed. Nothing of
  // the user's goes out after an unanswered probe now: it stays Software.)
  TEST_ASSERT_FALSE(p.userSet(25, 5000).sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
}

// Ours again only brings them down: an ACCEPT above what an older, lower
// command asked for (headphones with a floor), after the user's rise sent
// during the probe, would otherwise step them up to that rise while audio
// plays.
void test_ours_again_never_steps_them_up() {
  AbsVolumePolicy p(4);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(5, p.capabilities(true, 0).absolute);
  TEST_ASSERT_EQUAL_UINT8(74, p.userSet(58, 500).absolute);  // sent during the probe
  p.tick(500 + AbsVolumePolicy::kProbeTimeoutMs);             // unanswered
  p.streamActive(true, 4000);
  const auto a = p.accepted(16, 4500);  // their floor, for the 5
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // The same during the probe: the 74 is on its way anyway.
  AbsVolumePolicy q(4);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.userSet(58, 500);
  TEST_ASSERT_FALSE(q.accepted(16, 600).sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Probing, q.mode());
}

// After an unanswered probe, an ACCEPT that doesn't confirm (a stale one, or
// a level of their own) gets ours again: they are applying our commands, so
// the bounded resend stays. Never above what we last sent (a rise since was
// our gain's alone, and a louder command could land once audio plays), and
// only below what they just reported. (This used to be shown with an echo
// of the user's steps down sent after the timeout; those aren't sent now.)
void test_ours_again_after_an_unanswered_probe_is_at_most_the_last_sent() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
  p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // unanswered
  p.streamActive(true, 3000);
  TEST_ASSERT_FALSE(p.userSet(70, 3100).sendAbsolute);  // a rise: our gain only
  const auto a = p.accepted(100, 3500);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(64, a.absolute);  // not 89
  // The user's step down since: what we ask for now, still below them.
  AbsVolumePolicy q(50);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.tick(AbsVolumePolicy::kProbeTimeoutMs);
  q.streamActive(true, 3000);
  TEST_ASSERT_FALSE(q.userSet(40, 3100).sendAbsolute);
  const auto b = q.accepted(100, 3500);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(51, b.absolute);
  // (An ACCEPT that could make it a step up, one for an older command that
  // asked for less, is test_ours_again_never_steps_them_up.)
}

// A stream the headphones started themselves has flowed during the probe
// (so no handover by the lift), and a notification of theirs below ours ends
// it: the probe may still be on its way, and landing later it would step
// them up while our stream plays. Their level goes after it, and our stream
// waits for its answer (it used to start at once: the probe landed in it).
void test_their_report_after_a_stream_they_started_sends_their_level_back() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
  p.streamActive(true, 200);  // theirs; ESP-IDF suspends it again
  p.streamActive(false, 250);
  const auto a = p.headsetChanged(20, 300);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(20, a.absolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_FALSE(p.audioReady(300));
  // The probe lands, then theirs: nothing more is sent, nothing moves, and
  // the stream may start once theirs is answered.
  const auto b1 = p.accepted(64, 1000);
  const auto b2 = p.headsetChanged(64, 1010);
  TEST_ASSERT_FALSE(p.audioReady(1010));
  const auto b3 = p.accepted(20, 1100);
  TEST_ASSERT_TRUE(p.audioReady(1100));
  p.streamActive(true, 1150);
  const auto b4 = p.headsetChanged(20, 1160);
  for (const auto& b : {b1, b2, b3, b4}) {
    TEST_ASSERT_FALSE(b.sendAbsolute);
    TEST_ASSERT_FALSE(b.gainChanged);
    TEST_ASSERT_FALSE(b.volumeChanged);
  }
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // At or above what is out, nothing needs to follow it.
  AbsVolumePolicy q(50);
  q.linkUp(0);
  q.capabilities(true, 0);
  q.streamActive(true, 200);
  q.streamActive(false, 250);
  TEST_ASSERT_FALSE(q.headsetChanged(80, 300).sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, q.mode());
}

// An echo only confirms if no louder than what we ask for now: one that
// matches an older, louder command of ours (or a key of theirs that lands
// near it) while a lower one is on its way isn't proof they are at ours.
void test_echo_of_an_older_louder_command_is_not_an_answer() {
  AbsVolumePolicy p = probing();  // 38
  p.userSet(90, 1100);            // 114
  p.userSet(30, 1200);            // 38 again
  const auto a = p.headsetChanged(112, 1300);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_TRUE(a.sendAbsolute);  // ours again
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  TEST_ASSERT_TRUE(p.headsetChanged(40, 1400).lift);  // the lower one landed
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());

  // After audio, in Software following an unanswered probe: taken as theirs.
  // (The older command used to be a step down of the user's, sent after the
  // timeout; nothing of the user's is sent there now: here it is ours again,
  // with the user's step down since left to our gain.)
  AbsVolumePolicy q = probing();
  q.tick(5000);
  q.streamActive(true, 5100);
  TEST_ASSERT_EQUAL_UINT8(38, q.accepted(127, 6000).absolute);  // ours again
  TEST_ASSERT_FALSE(q.userSet(10, 6100).sendAbsolute);          // 13: our gain only
  const auto b = q.headsetChanged(38, 6200);                    // its echo, above 13
  TEST_ASSERT_EQUAL(Mode::Software, q.mode());
  TEST_ASSERT_FALSE(b.gainChanged);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_FALSE(q.accepted(127, 6300).sendAbsolute);  // their level now: no more sends
}

// A probe's commands can't hold a new stream back for ever: the listener's
// VOL+ presses (each sent, each restarting the timeout) or their louder
// notifications end at the deadline.
void test_probe_has_a_hard_deadline() {
  AbsVolumePolicy p = probing();  // first command at t=1000
  const uint32_t deadline = 1000 + AbsVolumePolicy::kProbeDeadlineFactor * AbsVolumePolicy::kProbeTimeoutMs;
  uint32_t t = 1000;
  for (int i = 0; t + 1500 < deadline; ++i) {
    t += 1500;
    const auto a = (i % 2) ? p.userSet(static_cast<uint8_t>(31 + i), t) : p.headsetChanged(100, t);
    TEST_ASSERT_TRUE(a.sendAbsolute);
    TEST_ASSERT_FALSE(p.tick(t + 1).modeChanged);
  }
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_FALSE(p.tick(deadline - 1).modeChanged);
  const auto a = p.tick(deadline);
  TEST_ASSERT_TRUE(a.probeUnanswered);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(p.audioReady(deadline + AbsVolumePolicy::kProbeGraceMs));

  // A late probe too.
  AbsVolumePolicy q = lateProbing();
  const uint32_t lateDeadline = kLateSentMs + AbsVolumePolicy::kProbeDeadlineFactor * AbsVolumePolicy::kLateProbeTimeoutMs;
  q.userSet(35, kLateSentMs + 800);
  q.userSet(40, kLateSentMs + 1600);
  TEST_ASSERT_FALSE(q.tick(lateDeadline - 1).modeChanged);
  TEST_ASSERT_TRUE(q.tick(lateDeadline).probeUnanswered);
  TEST_ASSERT_FALSE(q.ducked());
}

// After an unanswered probe before anything was heard, a new stream waits a
// little longer; an answer in that time is as good as one in time.
void test_unanswered_probe_grace_before_the_first_stream() {
  AbsVolumePolicy p = probing();
  const uint32_t timeout = 1000 + AbsVolumePolicy::kProbeTimeoutMs;
  p.tick(timeout);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.audioReady(timeout + AbsVolumePolicy::kProbeGraceMs - 1));
  const auto a = p.accepted(38, timeout + 500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.lift);
  TEST_ASSERT_TRUE(p.audioReady(timeout + 500));

  // Not once audio has flowed (a stream the headphones started): it runs anyway.
  AbsVolumePolicy q = probing();
  q.streamActive(true, 1500);
  q.tick(timeout);
  TEST_ASSERT_TRUE(q.audioReady(timeout));
}

// AVRCP drops and comes back on a link that has played in Absolute mode: the
// headphones kept their level while our gain came down to the software level
// (both attenuate). Its return gets a late probe of its own (dip, command,
// ramp), so the link doesn't stay stacked until the next one.
void test_avrcp_back_on_a_link_that_has_played_probes_late() {
  AbsVolumePolicy p = absolute();
  p.streamActive(true, 2000);
  p.userSet(50, 3000);  // 64, on the headphones
  const auto down = p.avrcpDown();
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(ramped(down));
  TEST_ASSERT_EQUAL_UINT16(softwareGain(50), p.gainQ15());
  TEST_ASSERT_FALSE(p.userSet(55, 4000).sendAbsolute);  // nothing sent without AVRCP
  p.avrcpUp(6000);
  const auto caps = p.capabilities(true, 6500);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_TRUE(p.ducked());
  TEST_ASSERT_TRUE(ramped(caps));
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_FALSE(caps.sendAbsolute);
  const auto sent = p.tick(6500 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_TRUE(sent.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(55), sent.absolute);
  const auto ok = p.accepted(vol::percentToAbs(55), 6500 + AbsVolumePolicy::kDuckSettleMs + 200);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(ok));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(55, p.percent());
}

// A late answer while nothing has been heard yet (paused all along) is as
// good as one in time; ours goes out, since the user's change since was our
// gain's alone. A new stream waits for its answer, so that it lands before
// the first stream: a rise in it would be heard as a step.
void test_late_accept_before_any_audio_hands_over() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  TEST_ASSERT_FALSE(p.userSet(20, 6000).sendAbsolute);  // 25: our gain only
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.lift);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, a.absolute);
  TEST_ASSERT_EQUAL_UINT8(20, p.percent());
  TEST_ASSERT_FALSE(p.audioReady(6500));
  p.accepted(24, 7500);  // its answer (16-step headphones)
  TEST_ASSERT_TRUE(p.audioReady(7500));

  // The review's case: a rise the user made meanwhile. The first stream
  // must not start before it lands. These headphones took 5.5 s to answer
  // the probe (sent at t=1000), so the stream waits as long for the answer
  // to 76 (it used to wait kProbeTimeoutMs: 76 could land in the stream).
  AbsVolumePolicy q = probing();  // 38
  q.tick(5000);
  TEST_ASSERT_FALSE(q.userSet(60, 6000).sendAbsolute);  // 76: our gain only
  const auto b = q.accepted(38, 6500);
  TEST_ASSERT_TRUE(b.lift);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(76, b.absolute);
  TEST_ASSERT_FALSE(q.audioReady(6500));
  TEST_ASSERT_FALSE(q.audioReady(6500 + AbsVolumePolicy::kProbeTimeoutMs));
  TEST_ASSERT_FALSE(q.audioReady(6500 + 5500 - 1));
  TEST_ASSERT_TRUE(q.audioReady(6500 + 5500));  // or its timeout
  // Its echo releases it as well.
  AbsVolumePolicy r = probing();
  r.tick(5000);
  r.userSet(60, 6000);
  r.accepted(38, 6500);
  TEST_ASSERT_FALSE(r.headsetChanged(76, 7400).volumeChanged);
  TEST_ASSERT_TRUE(r.audioReady(7400));
  // Not if they confirmed what ours is: nothing is sent, nothing waits.
  AbsVolumePolicy s = probing();
  s.tick(5000);
  TEST_ASSERT_FALSE(s.accepted(38, 6500).sendAbsolute);
  TEST_ASSERT_TRUE(s.audioReady(6500));
}

void test_rounded_accept_in_absolute_mode_keeps_the_ui() {
  AbsVolumePolicy p = absolute();
  p.userSet(40, 5000);  // 51
  TEST_ASSERT_FALSE(p.accepted(48, 5100).volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
}

// Capabilities that arrive while the stream runs (the headphones' AVRCP came
// up late). A probe steps their volume at once, from a level we don't know:
// our gain fades to silence first, and the command only goes out once that
// silence has reached their output. Their answer hands over, and our gain
// rises from silence at the gain stage's slow rate (not a lift: no restart).
void test_late_capabilities_while_streaming_dip_then_ask() {
  AbsVolumePolicy p = playing();
  const auto a = p.capabilities(true, kLateCapsMs);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_TRUE(p.ducked());
  TEST_ASSERT_TRUE(a.modeChanged);
  TEST_ASSERT_TRUE(ramped(a));  // faded down (~23 ms): media may flow
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(p.audioReady(kLateCapsMs));  // only holds a new start
  TEST_ASSERT_FALSE(p.tick(kLateSentMs - 1).sendAbsolute);
  const auto b = p.tick(kLateSentMs);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, b.absolute);
  TEST_ASSERT_TRUE(b.gainChanged && b.snap);  // the silence made sure of
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_FALSE(p.tick(kLateSentMs + 100).sendAbsolute);  // once
  const auto c = p.accepted(38, kLateSentMs + 200);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_FALSE(c.sendAbsolute);
  TEST_ASSERT_FALSE(c.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());  // the UI keeps what the user chose
  TEST_ASSERT_TRUE(p.audioReady(kLateSentMs + 200));
}

// Long after a pause nothing plays, nor still sounds at the headphones: the
// gain drops at once and the command goes out with it.
void test_late_capabilities_while_paused_snap_and_send() {
  AbsVolumePolicy p = playing();
  p.streamActive(false, 3000);
  const auto a = p.capabilities(true, 5000);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_TRUE(a.snap);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  TEST_ASSERT_FALSE(p.tick(5000 + AbsVolumePolicy::kLateProbeTimeoutMs - 1).modeChanged);
  TEST_ASSERT_TRUE(ramped(p.accepted(38, 5500)));
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  // The next link starts fresh.
  p.linkDown();
  TEST_ASSERT_TRUE(p.linkUp(9000).sendAbsolute);
}

// The stream stops while the command waits for the silence: what they had
// buffered from before the dip may still play, so it still waits (the dip's
// settle time comes first here).
void test_late_probe_waits_when_the_stream_stops() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, kLateCapsMs);
  TEST_ASSERT_FALSE(p.streamActive(false, kLateCapsMs + 100).sendAbsolute);
  TEST_ASSERT_FALSE(p.tick(kLateSentMs - 1).sendAbsolute);
  const auto a = p.tick(kLateSentMs);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, a.absolute);
  // Its timeout runs from the send.
  TEST_ASSERT_FALSE(p.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs - 1).modeChanged);
  TEST_ASSERT_TRUE(p.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs).probeUnanswered);
}

// Capabilities just after the stream stopped: the gain snaps to silence, but
// what they still had from us before the stop may be playing, so the command
// waits until nothing has flowed for kDuckSettleMs.
void test_late_capabilities_just_after_a_stop_wait() {
  AbsVolumePolicy p = playing();
  p.streamActive(false, 3000);
  const auto a = p.capabilities(true, 3010);
  TEST_ASSERT_TRUE(a.gainChanged && a.snap);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(p.tick(3000 + AbsVolumePolicy::kDuckSettleMs - 1).sendAbsolute);
  const auto b = p.tick(3000 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, b.absolute);
  // A stream that starts in between restarts nothing: it plays silence, and
  // the dip's own settle time applies.
  AbsVolumePolicy q = playing();
  q.streamActive(false, 3000);
  q.capabilities(true, 3010);
  TEST_ASSERT_FALSE(q.streamActive(true, 3100).sendAbsolute);
  TEST_ASSERT_FALSE(q.tick(3000 + AbsVolumePolicy::kDuckSettleMs).sendAbsolute);
  TEST_ASSERT_TRUE(q.tick(3010 + AbsVolumePolicy::kDuckSettleMs).sendAbsolute);
  TEST_ASSERT_EQUAL_UINT16(0, q.gainQ15());
}

// The stream stops once the dip has settled, before a tick sent the
// command: it goes out then, with the user's latest volume.
void test_late_probe_stop_after_the_dip_settled() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, kLateCapsMs);
  p.userSet(40, kLateCapsMs + 10);  // (its tick has not come yet)
  const auto a = p.streamActive(false, kLateSentMs + 5);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(40), a.absolute);
}

// No answer: software volume; the gain rises from silence to the software
// level at the slow rate. The user's volume is our gain's alone from then
// (a step down used to be sent: it could be the first command they apply).
void test_late_probe_timeout_ramps_to_software() {
  AbsVolumePolicy p = lateProbing();
  TEST_ASSERT_FALSE(p.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs - 1).modeChanged);
  const auto a = p.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.modeChanged);
  TEST_ASSERT_TRUE(a.probeUnanswered);
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  TEST_ASSERT_FALSE(p.userSet(40, 5000).sendAbsolute);  // a rise: our gain only
  const auto b = p.userSet(20, 5100);                    // a step down: the same
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_TRUE(ramped(b));
  TEST_ASSERT_FALSE(p.capabilities(true, 6000).sendAbsolute);  // no second probe
}

// The user's volume during the dip: kept (not capped), silent until they
// answer. Before the command goes out, the command carries it.
void test_user_volume_during_the_dip_before_the_send() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, kLateCapsMs);
  const auto a = p.userSet(80, kLateCapsMs + 100);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(80, p.percent());
  const auto b = p.tick(kLateSentMs);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(80), b.absolute);
}

// ...after it went out: sent, and the timeout starts again.
void test_user_volume_during_the_dip_after_the_send() {
  AbsVolumePolicy p = lateProbing();
  const auto a = p.userSet(20, kLateSentMs + 600);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, a.absolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  p.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  p.tick(kLateSentMs + 600 + AbsVolumePolicy::kLateProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(20), p.gainQ15());
}

// A notification at or below what we sent answers the probe (the UI follows
// theirs); one above it is answered with ours again, still in silence. Their
// level goes back after our 38, which may still land, and the silence lasts
// until that is answered: 38 lands before our gain rises. (The gain used to
// rise at once: 38 landed during the rise, a step up.)
void test_notification_during_a_late_probe() {
  AbsVolumePolicy p = lateProbing();  // sent 38
  const auto a = p.headsetChanged(30, kLateSentMs + 100);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_TRUE(p.ducked());
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(30), p.percent());
  TEST_ASSERT_TRUE(a.sendAbsolute);  // theirs, after our 38 that may still land
  TEST_ASSERT_EQUAL_UINT8(30, a.absolute);
  TEST_ASSERT_FALSE(p.accepted(38, kLateSentMs + 150).gainChanged);  // it did
  const auto e = p.headsetChanged(38, kLateSentMs + 160);              // ...and its echo
  TEST_ASSERT_FALSE(e.volumeChanged);
  TEST_ASSERT_FALSE(e.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  const auto g = p.accepted(30, kLateSentMs + 500);  // theirs, answered: up from silence
  TEST_ASSERT_TRUE(ramped(g));
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(30), p.percent());

  AbsVolumePolicy q = lateProbing();
  const auto b = q.headsetChanged(90, kLateSentMs + 500);
  TEST_ASSERT_EQUAL(Mode::Probing, q.mode());
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, b.absolute);
  TEST_ASSERT_FALSE(b.gainChanged);
  TEST_ASSERT_FALSE(b.volumeChanged);
  TEST_ASSERT_EQUAL_UINT16(0, q.gainQ15());
  TEST_ASSERT_FALSE(q.tick(kLateSentMs + AbsVolumePolicy::kLateProbeTimeoutMs).modeChanged);  // restarted
  const auto c = q.headsetChanged(38, kLateSentMs + 600);  // they took it
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_EQUAL(Mode::Absolute, q.mode());
  TEST_ASSERT_EQUAL_UINT8(30, q.percent());
}

// The device's flow with one key press more (Powerbeats Pro: capabilities
// 1.1 s into playback, a SET answered 1.0 s after it): in the dip's silence
// the listener presses VOL- (28, below our 38 still on its way). That hands
// over at their level, sent back, and the silence lasts until it is
// answered: 38 lands while nothing is heard, and our gain rises from
// silence only once 28 has landed. (It used to rise at once: 38 landed
// ~0.6 s into the rise, a step up.) No answer: kProbeTimeoutMs.
void test_their_key_during_the_dip_keeps_the_silence_until_answered() {
  const uint32_t caps = 1100, sent = caps + AbsVolumePolicy::kDuckSettleMs;
  auto dip = [&](AbsVolumePolicy& p) {
    p.capabilities(true, caps);
    TEST_ASSERT_EQUAL_UINT8(38, p.tick(sent).absolute);
    const auto a = p.headsetChanged(28, sent + 400);
    TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
    TEST_ASSERT_TRUE(p.ducked());
    TEST_ASSERT_FALSE(a.gainChanged);
    TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
    TEST_ASSERT_TRUE(a.sendAbsolute);
    TEST_ASSERT_EQUAL_UINT8(28, a.absolute);
    TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), p.percent());
  };
  AbsVolumePolicy p = playing();  // 30 %
  dip(p);
  TEST_ASSERT_FALSE(p.accepted(38, sent + 1000).gainChanged);  // lands in silence
  TEST_ASSERT_FALSE(p.tick(sent + 1100).gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  const auto b = p.accepted(28, sent + 1400);
  TEST_ASSERT_TRUE(ramped(b));
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_TRUE(p.headsetChanged(20, 5000).volumeChanged);  // the rocker moves the UI

  AbsVolumePolicy q = playing();
  dip(q);
  TEST_ASSERT_FALSE(q.tick(sent + 400 + AbsVolumePolicy::kProbeTimeoutMs - 1).gainChanged);
  const auto c = q.tick(sent + 400 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_FALSE(q.ducked());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, q.gainQ15());

  // AVRCP goes meanwhile: software, up from silence.
  AbsVolumePolicy r = playing();
  dip(r);
  const auto d = r.avrcpDown();
  TEST_ASSERT_TRUE(ramped(d));
  TEST_ASSERT_FALSE(r.ducked());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(vol::absToPercent(28)), r.gainQ15());
}

// Their volume keys while the command still waits for the silence: louder
// than we'll ask for, nothing is sent now and nothing gets louder (the
// command does it when it goes out); no louder, they have it already.
void test_notification_before_a_late_probe_is_sent() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, 2000);
  const auto a = p.headsetChanged(102, 2100);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
  TEST_ASSERT_EQUAL_INT(102, p.headsetAbsolute());
  const auto b = p.tick(2000 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(38, b.absolute);

  AbsVolumePolicy q = playing();
  q.capabilities(true, 2000);
  const auto c = q.headsetChanged(20, 2100);
  TEST_ASSERT_EQUAL(Mode::Absolute, q.mode());
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_FALSE(c.sendAbsolute);  // nothing of ours was out
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(20), q.percent());
  TEST_ASSERT_FALSE(q.tick(2000 + AbsVolumePolicy::kDuckSettleMs).sendAbsolute);  // dropped
}

// A loud volume is capped as on a new link; the user may choose more.
void test_late_probe_caps_a_loud_volume() {
  AbsVolumePolicy p = playing();
  p.userSet(90, 200);
  const auto a = p.capabilities(true, kLateCapsMs);
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(AbsVolumePolicy::kMaxLinkUpPercent, p.percent());
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(AbsVolumePolicy::kMaxLinkUpPercent), p.tick(kLateSentMs).absolute);
  const auto b = p.userSet(90, kLateSentMs + 100);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(90), b.absolute);
  TEST_ASSERT_EQUAL_UINT8(90, p.percent());
}

// The link drops during the dip: at the safe software level for the next one.
void test_link_down_during_the_dip() {
  AbsVolumePolicy p = playing();
  p.capabilities(true, kLateCapsMs);
  const auto a = p.linkDown();
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_TRUE(a.snap);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  TEST_ASSERT_FALSE(p.tick(kLateSentMs).sendAbsolute);
}

// AVRCP drops during the dip: software, the gain ramps up from silence to
// the software level; nothing is sent. Its next connection probes again.
void test_avrcp_down_during_the_dip() {
  AbsVolumePolicy p = lateProbing();
  const auto a = p.avrcpDown();
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.ducked());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  TEST_ASSERT_FALSE(p.tick(9000).modeChanged);

  AbsVolumePolicy q = playing();
  q.capabilities(true, kLateCapsMs);
  q.avrcpDown();
  TEST_ASSERT_FALSE(q.tick(kLateSentMs).sendAbsolute);  // the pending command is dropped
  const auto b = q.capabilities(true, 5000);
  TEST_ASSERT_EQUAL(Mode::Probing, q.mode());
  TEST_ASSERT_EQUAL_UINT16(0, q.gainQ15());
  TEST_ASSERT_TRUE(q.tick(5000 + AbsVolumePolicy::kDuckSettleMs).sendAbsolute);
  TEST_ASSERT_TRUE(b.gainChanged);
}

// A stream that starts during the dip (the headphones may start one
// themselves) stays silent.
void test_stream_starting_during_the_dip_stays_silent() {
  AbsVolumePolicy p = playing();
  p.streamActive(false, 100);
  p.capabilities(true, kLateCapsMs);  // sent at once: nothing has flowed since t=100
  const auto a = p.streamActive(true, kLateCapsMs + 100);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
}

// The headroom diagnostic: retargets our gain in either mode, by the ramp.
void test_headroom_setting_retargets_by_the_ramp() {
  const uint16_t six = vol::dbToQ15(-6.0f);
  AbsVolumePolicy p = absolute();
  const auto a = p.setHeadroom(six);
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(six, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT16(six, p.headroomQ15());
  const auto b = p.setHeadroom(40000);  // at most unity
  TEST_ASSERT_TRUE(ramped(b));
  TEST_ASSERT_EQUAL_UINT16(vol::kUnityQ15, p.gainQ15());
  TEST_ASSERT_FALSE(p.setHeadroom(vol::kUnityQ15).gainChanged);

  AbsVolumePolicy q(30);
  q.linkUp(0);
  const auto c = q.setHeadroom(six);
  TEST_ASSERT_TRUE(ramped(c));
  TEST_ASSERT_EQUAL_UINT16(vol::mulQ15(six, vol::softwareVolumeQ15(30)), q.gainQ15());

  // During a dip it waits for the answer.
  AbsVolumePolicy r = lateProbing();
  TEST_ASSERT_FALSE(r.setHeadroom(six).gainChanged);
  TEST_ASSERT_EQUAL_UINT16(0, r.gainQ15());
  r.accepted(38, kLateSentMs + 100);
  TEST_ASSERT_EQUAL_UINT16(six, r.gainQ15());
}

// After an unanswered probe and audio at the software level, the headphones'
// own change shows the listener sets their level there: it ends the wait for
// a late answer (the user's volume wasn't sent anyway, see
// test_after_an_unanswered_probe_nothing_is_sent). Their level goes back
// once, after the probe that may still be on its way; then nothing more,
// not even ours again for a stale ACCEPT.
void test_their_change_after_an_unanswered_probe_stops_our_sends() {
  AbsVolumePolicy p = probing();  // 38
  p.tick(5000);                   // unanswered
  p.streamActive(true, 5100);
  TEST_ASSERT_FALSE(p.userSet(20, 6000).sendAbsolute);
  const auto a = p.headsetChanged(8, 9000);  // theirs
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(8, a.absolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.accepted(127, 9500).sendAbsolute);
  const auto b = p.userSet(15, 10000);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_TRUE(b.gainChanged);  // our own gain, one step
}

// An ACCEPT that arrives while a stream the headphones started runs: the
// probe went out before any audio, so they apply ours; our gain rises slowly
// from the software level (no lift: audio plays).
void test_accept_while_streaming_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.streamActive(true, 1050);
  const auto a = p.accepted(38, 1100);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
}

void test_audio_waits_longer_once_avrcp_is_up() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  p.avrcpUp(1000);
  TEST_ASSERT_FALSE(p.audioReady(AbsVolumePolicy::kCapsWaitMs));
  TEST_ASSERT_FALSE(p.audioReady(1000 + AbsVolumePolicy::kCapsReplyWaitMs - 1));
  TEST_ASSERT_TRUE(p.audioReady(1000 + AbsVolumePolicy::kCapsReplyWaitMs));
  p.capabilities(false, 2000);
  TEST_ASSERT_TRUE(p.audioReady(2000));
}

void test_audio_waits_for_the_volume_to_settle() {
  AbsVolumePolicy p(30);
  TEST_ASSERT_TRUE(p.audioReady(0));  // no link: nothing to wait for
  p.linkUp(1000);
  TEST_ASSERT_FALSE(p.audioReady(1000));  // capabilities not known yet
  TEST_ASSERT_TRUE(p.audioReady(1000 + AbsVolumePolicy::kCapsWaitMs));  // none came: software
  p.capabilities(true, 1200);  // probe out
  TEST_ASSERT_FALSE(p.audioReady(1300));
  TEST_ASSERT_FALSE(p.audioReady(5000));  // still probing until tick() says otherwise
  p.accepted(38, 1400);
  TEST_ASSERT_TRUE(p.audioReady(1400));

  AbsVolumePolicy q(30);
  q.linkUp(0);
  q.capabilities(false, 100);
  TEST_ASSERT_TRUE(q.audioReady(100));  // no absolute volume: software at once

  AbsVolumePolicy r(30);
  r.linkUp(0);
  r.capabilities(true, 100);
  r.tick(100 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Software, r.mode());
  // A little longer, in case they apply it late.
  TEST_ASSERT_FALSE(r.audioReady(100 + AbsVolumePolicy::kProbeTimeoutMs));
  TEST_ASSERT_TRUE(r.audioReady(100 + AbsVolumePolicy::kProbeTimeoutMs + AbsVolumePolicy::kProbeGraceMs));
}

void test_repeated_link_up_changes_nothing() {
  AbsVolumePolicy p = absolute();
  const auto a = p.linkUp(9000);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
}

void test_user_volume_in_software_mode_ramps_our_gain() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  const auto a = p.userSet(50, 10);
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_FALSE(a.snap);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(50), p.gainQ15());
}

void test_user_volume_while_probing_does_both() {
  AbsVolumePolicy p = probing();
  const auto a = p.userSet(50, 1500);
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(64, a.absolute);
  // The new command restarted the timeout.
  p.tick(1000 + AbsVolumePolicy::kProbeTimeoutMs + 100);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
}

void test_user_volume_in_absolute_mode_is_sent_not_applied() {
  AbsVolumePolicy p = absolute();
  const auto a = p.userSet(70, 5000);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(89, a.absolute);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
}

void test_avrcp_loss_ramps_back_down_to_software() {
  AbsVolumePolicy p = absolute();
  const auto a = p.avrcpDown();
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.gainChanged);
  TEST_ASSERT_FALSE(a.snap);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
}

void test_avrcp_reconnect_on_the_same_link_probes_again() {
  AbsVolumePolicy p = absolute();
  p.avrcpDown();
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  const auto a = p.capabilities(true, 7000);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());  // still attenuating until proven
}

void test_accept_without_a_probe_is_ignored() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  p.capabilities(false, 10);  // never offered: nothing was sent on this link
  const auto a = p.accepted(38, 20);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
}

void test_next_link_never_starts_at_unity() {
  AbsVolumePolicy p = absolute();
  const auto down = p.linkDown();
  TEST_ASSERT_TRUE(down.snap);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  const auto up = p.linkUp(20000);
  TEST_ASSERT_TRUE(up.snap);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), p.gainQ15());
  // AVRCP stayed up across the A2DP drop: no new capabilities will come, so
  // the new link probes with what it knows (still attenuating).
  TEST_ASSERT_TRUE(up.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
}

void test_link_after_avrcp_loss_waits_for_new_capabilities() {
  AbsVolumePolicy p = absolute();
  p.linkDown();
  p.avrcpDown();
  const auto up = p.linkUp(20000);
  TEST_ASSERT_FALSE(up.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(p.audioReady(20000));
}

void test_nothing_happens_to_the_mode_without_a_link() {
  AbsVolumePolicy p(30);
  p.accepted(100, 0);
  p.headsetChanged(100, 0);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
}

// Random event sequences against a simulated headset, with the invariants
// that keep ears safe. The headset has its own level (unknown to the policy),
// may or may not apply absolute volume, answer it or echo it, and its user
// presses its buttons. Our commands reach it in order, each after its own
// delay: at once, within a few hundred ms, or seconds late (past the probe's
// timeout, grace and deadline); its answers and echoes of what it applied
// come later still, but before a notification of a later key of its own.
// It sets ~16 steps (~32 on some links): to the nearest, rounding up, or
// above a floor. What
// the listener hears is its level times our gain. Our streams start only
// when audioReady() lets them; the headset starts its own at any time.
// `slow`: headphones of ~16 steps that land every command 1-3.5 s late, and
// a listener quick on their keys, so that their keys cross our commands
// (see Held).
namespace {
double headsetDb(int level) { return level <= 0 ? -100.0 : 40.0 * (level / 127.0 - 1.0); }
double gainDb(uint16_t q) { return q == 0 ? -100.0 : 20.0 * std::log10(q / 32768.0); }
// What the listener hears; our gain at 0 is silence, whatever their level.
double heardDb(int level, uint16_t q) { return q == 0 ? -1000.0 : headsetDb(level) + gainDb(q); }
// How the headset sets a level: in steps of `step` (8: ~16 steps, 4: ~32),
// to the nearest one, up to the next one (more than kEchoTolerance above
// what we asked for, at times), or the nearest one but never below a floor.
enum class Rounding { Nearest, Up, Floor };
int quantise(int abs, Rounding r, int step) {
  abs = abs < 0 ? 0 : (abs > 127 ? 127 : abs);
  const int nearest = abs >= 128 - step / 2 ? 127 : ((abs + step / 2) / step) * step;
  switch (r) {
    case Rounding::Up: return abs > 128 - step ? 127 : ((abs + step - 1) / step) * step;
    case Rounding::Floor: return std::max(16, nearest);
    default: return nearest;
  }
}

// A command sent in reply to an answer (an ACCEPT or a notification of what
// they applied) of a command that was itself one: how deep that goes. Ours
// again once (1), then a late confirmation's (2). Deeper would be a loop
// with headphones that can't set our level.
constexpr int kMaxReplyDepth = 2;

void runRandomEvents(uint32_t seed, Rounding rounding, bool full, bool slow = false) {
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  int step = 8;  // the headset's key step (and how it sets a level)
  auto quantise = [rounding, &step](int abs) { return ::quantise(abs, rounding, step); };
  AbsVolumePolicy p(30);
  uint32_t now = 0;
  bool streaming = false;
  bool heardLink = false;  // media has flowed on the current link
  // The headset.
  int level = 64;
  bool applies = true, answers = true, echoes = true;
  struct InFlight {
    int value;
    uint32_t at;   // when it lands
    bool user;     // the user's own volume (userSet())
    uint8_t percent;  // the volume shown when it was sent
    int depth;        // sent in reply to an answer: see kMaxReplyDepth
    int id;           // in the order sent
  };
  struct Due {
    int level;
    int depth;  // of the command it answers
    int id;     // ...and its id
    int sent;   // ...and what that asked for
  };
  std::deque<InFlight> inFlight;
  std::vector<Due> acceptsDue, echoesDue;
  auto apply = [&](int value, int depth, int id) {
    level = quantise(value);
    if (answers) acceptsDue.push_back({level, depth, id, value});
    if (echoes) echoesDue.push_back({level, depth, id, value});
  };
  // A command that ended a wait while one of ours may have been on its way
  // (their level back, ours after a late confirmation): a stream of ours
  // must not start, nor our gain rise once audio has flowed, until it is
  // answered or kProbeTimeoutMs has passed. Answered: an ACCEPT or echo of
  // it or of a later command (they land in order), or one the policy can't
  // tell from its own (ESP-IDF names no command): an ACCEPT near what it
  // asked for, an echo of an older command near it or a later command, or
  // within 1 of an ACCEPT since. A key of theirs near those only once
  // nothing older of ours is still on its way.
  // The recheck's sequence: meanwhile a notification near an older command
  // of ours with no ACCEPT yet (on its way, or landed with its ACCEPT still
  // to come), not near those; then a key of theirs near them with an older
  // command still on its way. Neither answers it: the gate stays shut.
  struct Held {
    bool on = false;
    bool answered = false;
    int id = 0;
    uint32_t ms = 0;
    std::vector<int> sent;      // it and the commands sent since
    std::vector<int> accepted;  // ACCEPTs since
    bool olderNear = false;     // a notification near an older command with no ACCEPT yet
  } held;
  auto near = [](const std::vector<int>& v, int x, int tol) {
    for (int y : v) {
      if (std::abs(x - y) <= tol) return true;
    }
    return false;
  };
  int nextId = 0, heldCmds = 0, heldBack = 0, ourStarts = 0, heldChecks = 0;
  int olderNears = 0, recheckSeqs = 0;  // the recheck's sequence (see Held): its steps
  int probesSent = 0, lifts = 0, heardChecks = 0, lateProbes = 0, lateSends = 0, rampedRises = 0;
  int delayed = 0, landings = 0, landingRises = 0, handBacks = 0, timeouts = 0;
  uint32_t silentMs = 0;  // when our gain was last taken to silence
  uint32_t stopMs = 0;    // when media last stopped flowing
  int lastSent = -1;
  bool probeOut = false;       // Probing, and a command of the probe has gone out
  uint32_t probeFirstMs = 0;   // ...the first one
  int maxDepth = 0;
  int desynced = 0;  // user steps down from a UI above their level (see below)
  for (int i = 0; i < 60000; ++i) {
    now += rnd(slow ? 250 : 700);
    const uint16_t gainBefore = p.gainQ15();
    const uint8_t percentBefore = p.percent();
    const Mode modeBefore = p.mode();
    const bool wasStreaming = streaming;
    const bool wasHeard = heardLink && p.linked();
    const bool wasDucked = p.ducked();
    AbsVolumePolicy::Actions a;
    uint32_t what = rnd(14);
    if (slow && what >= 12) what = 6;  // their keys, more often
    int levelBefore = level;
    int delivered = -1;  // what an ACCEPT or a notification said
    int deliveredDepth = -1;  // ...answering a command of this depth (-1: a key of theirs)
    int deliveredId = -1;     // ...and its id
    bool landed = false;
    InFlight cmd{};
    switch (what) {
      case 0:
        if (!p.linked()) {  // a new link, maybe other headphones
          step = slow || rnd(3) != 0 ? 8 : 4;
          level = quantise(static_cast<int>(rnd(128)));
          applies = rnd(4) != 0;
          answers = rnd(3) != 0;
          echoes = rnd(2) != 0;
          inFlight.clear();
          acceptsDue.clear();
          echoesDue.clear();
          heardLink = false;
          streaming = false;
          levelBefore = level;
          held = Held{};
        }
        a = p.linkUp(now);
        break;
      case 1:  // (AVRCP may stay: commands in flight still land)
        a = p.linkDown();
        streaming = false;
        heardLink = false;
        break;
      case 2:
        if (rnd(2)) p.avrcpUp(now);
        a = p.capabilities(rnd(4) != 0, now);
        break;
      case 3:  // with it, what was in flight over it
        a = p.avrcpDown();
        inFlight.clear();
        acceptsDue.clear();
        echoesDue.clear();
        held = Held{};
        break;
      case 4:
        if (acceptsDue.empty()) break;
        delivered = acceptsDue.front().level;
        deliveredDepth = acceptsDue.front().depth;
        deliveredId = acceptsDue.front().id;
        acceptsDue.erase(acceptsDue.begin());
        a = p.accepted(static_cast<uint8_t>(delivered), now);
        break;
      case 5:
        if (echoesDue.empty()) break;
        delivered = echoesDue.front().level;
        deliveredDepth = echoesDue.front().depth;
        deliveredId = echoesDue.front().id;
        echoesDue.erase(echoesDue.begin());
        a = p.headsetChanged(static_cast<uint8_t>(delivered), now);
        break;
      case 6:  // the listener presses the headset's own buttons
        // (Their answers and notifications arrive in order: one AVRCP
        // channel, then BTC_TASK and BtAppT's queue. A key pressed after a
        // command of ours landed is reported after its ACCEPT and echo.)
        if (!echoesDue.empty() || !acceptsDue.empty()) break;
        level = quantise(level + (rnd(2) ? step : -step));
        levelBefore = level;  // their doing, not ours
        delivered = level;
        a = p.headsetChanged(static_cast<uint8_t>(level), now);
        break;
      case 7: a = p.userSet(static_cast<uint8_t>(rnd(101)), now); break;
      case 8: {  // (only linked: StreamControl has no stream without a link)
        if (!p.linked()) break;
        const bool start = rnd(2) != 0;
        if (start && !wasStreaming && rnd(4) != 0) {  // ours: through the gate
          const bool heldNow = held.on && !held.answered && now - held.ms < AbsVolumePolicy::kProbeTimeoutMs;
          if (!p.audioReady(now)) {
            if (heldNow) ++heldBack;
            break;
          }
          ++ourStarts;
          // Nothing of ours still on its way lands into it unanswered.
          TEST_ASSERT_FALSE(heldNow);
        }
        streaming = start;
        if (wasStreaming && !streaming) stopMs = now;
        a = p.streamActive(streaming, now);
        if (streaming) heardLink = true;
        break;
      }
      case 9:
      case 10:  // a command of ours lands at the headset
        if (inFlight.empty() || inFlight.front().at > now) break;
        cmd = inFlight.front();
        inFlight.pop_front();
        landed = true;
        ++landings;
        apply(cmd.value, cmd.depth, cmd.id);
        break;
      default: a = p.tick(now); break;
    }
    // Answers to a held command (before a command sent now can be one).
    if (held.on && delivered >= 0) {
      if (what == 4) {
        if (deliveredId >= held.id || std::abs(delivered - held.sent.front()) <= AbsVolumePolicy::kEchoTolerance) {
          held.answered = true;
        }
        held.accepted.push_back(delivered);
      } else {
        const bool nearOurs =
            near(held.sent, delivered, AbsVolumePolicy::kEchoTolerance) || near(held.accepted, delivered, 1);
        bool olderOut = false;  // a command sent before it still on its way
        for (const InFlight& f : inFlight) olderOut = olderOut || f.id < held.id;
        // Near a command sent before it that has no ACCEPT yet (on its way,
        // or landed with its ACCEPT still to come).
        const int tol = AbsVolumePolicy::kEchoTolerance;
        bool nearOlder = false;
        for (const InFlight& f : inFlight) nearOlder = nearOlder || (f.id < held.id && std::abs(delivered - f.value) <= tol);
        for (const Due& d : acceptsDue) nearOlder = nearOlder || (d.id < held.id && std::abs(delivered - d.sent) <= tol);
        if (deliveredId >= held.id || (nearOurs && (what == 5 || !olderOut))) held.answered = true;
        if (!held.answered && now - held.ms < AbsVolumePolicy::kProbeTimeoutMs) {
          if (nearOlder && !nearOurs) {
            held.olderNear = true;
            ++olderNears;
          } else if (what == 6 && nearOurs && olderOut && held.olderNear) {
            ++recheckSeqs;  // the gate must stay shut: checked below
          }
        }
      }
    }
    const int depth = deliveredDepth >= 0 ? deliveredDepth + 1 : 0;
    const int id = a.sendAbsolute ? nextId++ : -1;
    if (a.sendAbsolute && held.on) held.sent.push_back(a.absolute);
    // What ends a wait while one of ours may be on its way (see Held): a
    // handover's (their level, ours after a late confirmation), or a report
    // of theirs ending one after audio has flowed (their level, or the
    // volume shown if lower).
    if (a.sendAbsolute && what != 7 &&
        ((a.modeChanged && p.mode() == Mode::Absolute) ||
         ((what == 5 || what == 6) && p.mode() == Mode::Software && (wasHeard || wasStreaming || streaming)))) {
      held = Held{};
      held.on = true;
      held.id = id;
      held.ms = now;
      held.sent.push_back(a.absolute);
      ++heldCmds;
    }
    if (a.sendAbsolute && p.linked() && applies) {
      const uint32_t kind = rnd(3);
      const uint32_t delay =
          slow ? 1000 + rnd(2500) : (kind == 0 ? 0 : (kind == 1 ? rnd(400) : rnd(6000)));
      if (delay == 0 && inFlight.empty()) {
        apply(a.absolute, depth, id);
      } else {
        uint32_t at = now + delay;
        if (!inFlight.empty() && static_cast<int32_t>(inFlight.back().at - at) > 0) at = inFlight.back().at;
        inFlight.push_back({a.absolute, at, what == 7, p.percent(), depth, id});
        ++delayed;
      }
    }
    if (a.lift) ++lifts;
    if (p.ducked() && !wasDucked) ++lateProbes;
    if (p.gainQ15() == 0 && gainBefore != 0) {
      // Silent at their output once the fade has passed their buffer; if
      // nothing flows, once what flowed before the stop has (at once if that
      // was long enough ago). A stream started after the snap only carries
      // silence. (It used to count from the snap then, which the slow runs
      // tripped over: a send kDuckSettleMs after the stop, not the snap.)
      silentMs = !wasStreaming && !streaming ? stopMs : now;
    }

    TEST_ASSERT_TRUE(p.gainQ15() <= vol::kHeadroomQ15);  // headroom always applied
    // Jumps only go down. (A link lost during a dip, or while a late
    // handover's rise waits for an answer, snaps to the next link's level
    // from below: nothing plays, and the gain stage's snap never raises
    // anyway.)
    if (a.snap && !(what == 1 && (gainBefore == 0 || modeBefore == Mode::Absolute))) {
      TEST_ASSERT_TRUE(p.gainQ15() <= gainBefore);
    }
    if (p.mode() == Mode::Absolute) TEST_ASSERT_TRUE(p.linked());
    if (p.ducked()) {  // a late probe, or its handover waiting for their level back
      TEST_ASSERT_TRUE(p.mode() != Mode::Software);
      TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
    } else if (p.mode() != Mode::Absolute) {
      TEST_ASSERT_EQUAL_UINT16(softwareGain(p.percent()), p.gainQ15());
    }
    if (p.mode() == Mode::Probing) TEST_ASSERT_FALSE(p.audioReady(now));
    // The gate stays shut while a command that ended a wait is unanswered
    // inside its window (see Held), whether or not a start of ours comes.
    if (p.linked() && held.on && !held.answered && now - held.ms < AbsVolumePolicy::kProbeTimeoutMs) {
      ++heldChecks;
      TEST_ASSERT_FALSE(p.audioReady(now));
    }
    // A probe ends by its deadline, however often its timeout restarted.
    if (p.mode() != Mode::Probing) {
      probeOut = false;
    } else if (a.sendAbsolute && !probeOut) {
      probeOut = true;
      probeFirstMs = now;
    }
    if (probeOut && what >= 11) {
      const uint32_t timeout = p.ducked() ? AbsVolumePolicy::kLateProbeTimeoutMs : AbsVolumePolicy::kProbeTimeoutMs;
      TEST_ASSERT_TRUE(now - probeFirstMs < AbsVolumePolicy::kProbeDeadlineFactor * timeout);
    }
    if (a.probeUnanswered) ++timeouts;
    if (a.sendAbsolute) {
      // What is sent is the volume shown: the user's exactly; otherwise it
      // or less (their own level back may be one off, percent rounding).
      if (what == 7) {
        TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(p.percent()), a.absolute);
      } else {
        TEST_ASSERT_TRUE(a.absolute <= vol::percentToAbs(p.percent()) + 1);
      }
      // In Software mode (after an unanswered probe), only steps down go
      // out: ours again, their own level back; never the user's volume.
      if (p.mode() == Mode::Software) TEST_ASSERT_TRUE(lastSent >= 0 && a.absolute <= lastSent);
    }
    if (what == 7 && modeBefore == Mode::Software) TEST_ASSERT_FALSE(a.sendAbsolute);
    // No command loop: replies to answers of replies end.
    if (a.sendAbsolute) {
      maxDepth = std::max(maxDepth, depth);
      TEST_ASSERT_TRUE(depth <= kMaxReplyDepth);
    }
    // No echo loop: a change of theirs is never sent back once they have the
    // volume, except at the handover itself (their level, or a late
    // confirmation making sure they end on ours).
    if ((what == 5 || what == 6) && a.sendAbsolute && p.mode() == Mode::Absolute) {
      TEST_ASSERT_TRUE(a.modeChanged);
      if (a.absolute == delivered) ++handBacks;
    }
    // A lift only while nothing has been heard on the link.
    if (a.lift) TEST_ASSERT_FALSE(wasHeard || wasStreaming || streaming);
    // A command that isn't the user's: before anything was heard, or into
    // silence that has reached their output, or no louder than what they
    // have from us or said they have: ours again, their level back, a lower
    // one after a late confirmation.
    if (a.sendAbsolute && what != 7) {
      ++probesSent;
      if (wasHeard || wasStreaming || streaming) {
        // (Silent from a late probe's dip, not from a volume of 0 %.)
        const bool silent =
            (p.ducked() || wasDucked) && p.gainQ15() == 0 && (gainBefore == 0 || (a.snap && !streaming));
        if (silent) {
          ++lateSends;
          // The silence has reached their output: the fade has had the
          // settle time, or nothing has flowed for that long (what they
          // buffered before a stop has played out).
          TEST_ASSERT_TRUE(now - silentMs >= AbsVolumePolicy::kDuckSettleMs ||
                           (!streaming && now - stopMs >= AbsVolumePolicy::kDuckSettleMs));
        } else {
          TEST_ASSERT_TRUE(a.absolute == lastSent || (delivered >= 0 && a.absolute <= delivered));
        }
      }
    }
    if (a.sendAbsolute) lastSent = a.absolute;
    // Once audio has flowed on a link, what the listener hears (their level
    // times our gain) never jumps up except by the user's own step: an
    // answer, a notification, a timeout, the stream starting or stopping only
    // ever raise our gain by the ramp (the gain stage's slow rate) with their
    // level unchanged or lower, and a handover lands no louder than what we
    // sent.
    // A step down never makes it louder (quantisation aside).
    if (wasHeard && p.linked()) {
      ++heardChecks;
      const double before = heardDb(levelBefore, gainBefore);
      const double after = heardDb(level, p.gainQ15());
      if (landed) {
        // A command of ours landing late. The user's own is their step;
        // anything else was sent before audio or into silence (checked
        // above), and landing now is the residual risk the policy's header
        // states: at most their level for what we sent times our gain, never
        // above what absolute volume gives for the volume shown then.
        if (after > before + 1e-9 && !cmd.user) {
          ++landingRises;
          TEST_ASSERT_TRUE(cmd.value <= vol::percentToAbs(cmd.percent) + 1);
          TEST_ASSERT_TRUE(after <= headsetDb(quantise(vol::percentToAbs(cmd.percent) + 1)) +
                                        gainDb(vol::kHeadroomQ15) + 1e-9);
        }
      } else if (what != 7) {
        if (after > before + 1e-9) {
          ++rampedRises;
          TEST_ASSERT_TRUE(a.gainChanged && !a.snap && !a.lift);
          // Not while a command that ended a wait may still be on its way
          // unanswered (see Held): what it follows could land in the rise.
          TEST_ASSERT_FALSE(held.on && !held.answered && now - held.ms < AbsVolumePolicy::kProbeTimeoutMs);
          // Their level unchanged, or lowered (their own level sent back).
          TEST_ASSERT_TRUE(level <= levelBefore);
          // With nothing in flight, they are at no more than what we sent
          // (as they round it). (A stale notification may set the UI below
          // what they were last sent.)
          if (p.mode() == Mode::Absolute && inFlight.empty()) {
            const int most = std::max<int>(vol::percentToAbs(p.percent()), lastSent);
            TEST_ASSERT_TRUE(level <= std::max(most + 5, quantise(most + 1)));
          }
        }
      } else if (p.percent() <= percentBefore) {
        // (Unless, in Absolute mode, they are below the level shown: a
        // report of theirs, or a stale echo taken as one, overtook a
        // command of ours that landed after it. The step down from the UI
        // then sends more than they have. A known race of the Absolute
        // UI, which follows their reports, not the policy's rules above.)
        if (modeBefore == Mode::Absolute && levelBefore < quantise(vol::percentToAbs(percentBefore)) - 8) {
          ++desynced;
        } else {
          TEST_ASSERT_TRUE(after <= before + 1.5);
        }
      }
    }
  }
  printf("random events (rounding %d): %d probes/non-user sends, %d lifts, %d late probes, %d handbacks, "
         "%d timeouts, %d landing rises, reply depth up to %d, %d steps from a UI above them, "
         "%d held commands (gate checked shut %d times), %d of our starts (%d held back for one), "
         "%d notifications near an older command while held, %d keys near the held one after one%s\n",
         static_cast<int>(rounding), probesSent, lifts, lateProbes, handBacks, timeouts, landingRises,
         maxDepth, desynced, heldCmds, heldChecks, ourStarts, heldBack, olderNears, recheckSeqs,
         slow ? " (slow keys)" : "");
  TEST_ASSERT_TRUE(probesSent > 50);
  TEST_ASSERT_TRUE(heardChecks > 1000);
  TEST_ASSERT_TRUE(landings > (slow ? 150 : 300));  // (slow: fewer, each seconds late)
  if (slow) {
    // The recheck's sequence occurs, and the gate stayed shut through it.
    TEST_ASSERT_TRUE(olderNears > 20);
    TEST_ASSERT_TRUE(recheckSeqs > 5);
    TEST_ASSERT_TRUE(heldChecks > 100);
  }
  if (!full) return;
  TEST_ASSERT_TRUE(lifts > 50);
  TEST_ASSERT_TRUE(lateProbes > 20);
  TEST_ASSERT_TRUE(lateSends > 20);
  TEST_ASSERT_TRUE(rampedRises > 20);
  TEST_ASSERT_TRUE(delayed > 1000);
  TEST_ASSERT_TRUE(handBacks > 5);
  TEST_ASSERT_TRUE(timeouts > 20);
  TEST_ASSERT_TRUE(landingRises > 0);  // the residual risk does occur in this model
  TEST_ASSERT_TRUE(heldCmds > 20);
  TEST_ASSERT_TRUE(heldBack > 5);
  TEST_ASSERT_TRUE(heldChecks > 100);
}
}  // namespace

void test_invariants_under_random_events() {
  runRandomEvents(12345, Rounding::Nearest, /*full=*/true);
  // Headphones that can't always set our level: never a command loop, and
  // the same invariants.
  runRandomEvents(777, Rounding::Up, /*full=*/false);
  runRandomEvents(4242, Rounding::Floor, /*full=*/false);
  // Slow headphones and quick keys: the recheck's sequence (a key near an
  // older command of ours with no ACCEPT, then one near the held command)
  // with each rounding.
  runRandomEvents(31337, Rounding::Nearest, /*full=*/false, /*slow=*/true);
  runRandomEvents(2718, Rounding::Up, /*full=*/false, /*slow=*/true);
  runRandomEvents(1618, Rounding::Floor, /*full=*/false, /*slow=*/true);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_link_up_starts_in_software_mode_snapped);
  RUN_TEST(test_link_up_caps_a_loud_volume);
  RUN_TEST(test_probe_sends_volume_but_keeps_attenuating);
  RUN_TEST(test_capabilities_before_the_link_probe_on_link_up);
  RUN_TEST(test_no_volume_notifications_stays_software);
  RUN_TEST(test_accept_hands_volume_to_the_headphones_with_a_lift);
  RUN_TEST(test_headphone_buttons_update_the_ui_and_are_not_echoed);
  RUN_TEST(test_echo_of_our_own_command_is_ignored);
  RUN_TEST(test_quantised_echo_is_ignored);
  RUN_TEST(test_rapid_steps_do_not_bounce_the_ui);
  RUN_TEST(test_echo_of_what_they_accepted_is_ignored);
  RUN_TEST(test_their_step_right_after_ours_is_adopted);
  RUN_TEST(test_an_echo_counts_once);
  RUN_TEST(test_same_value_after_the_echo_window_is_adopted);
  RUN_TEST(test_notification_during_probe_proves_support);
  RUN_TEST(test_their_level_is_sent_after_a_probe_still_on_its_way);
  RUN_TEST(test_their_report_during_the_probe_holds_the_first_stream);
  RUN_TEST(test_their_report_after_an_unanswered_probe_holds_a_new_stream);
  RUN_TEST(test_a_key_near_their_level_back_keeps_the_first_stream_held);
  RUN_TEST(test_a_key_near_the_probe_keeps_it_unanswered);
  RUN_TEST(test_a_key_near_the_probe_keeps_the_dip_silent);
  RUN_TEST(test_a_notification_before_its_accept_keeps_the_order);
  RUN_TEST(test_the_probe_landing_before_its_accept_leaves_the_ui_at_their_level);
  RUN_TEST(test_a_command_lost_unanswered_keeps_the_hold_from_notifications);
  RUN_TEST(test_their_report_above_the_volume_shown_still_ends_below_it);
  RUN_TEST(test_louder_notification_during_probe_is_answered_with_ours);
  RUN_TEST(test_probe_times_out_to_software);
  RUN_TEST(test_probe_timeout_survives_millis_wraparound);
  RUN_TEST(test_after_an_unanswered_probe_nothing_is_sent);
  RUN_TEST(test_late_accept_after_audio_ramps_to_the_headphones);
  RUN_TEST(test_late_accept_after_a_step_down_holds_the_rise);
  RUN_TEST(test_late_accept_after_a_rise_keeps_their_level);
  RUN_TEST(test_late_echo_after_audio_ramps_to_the_headphones);
  RUN_TEST(test_accept_above_what_we_asked_is_not_an_answer);
  RUN_TEST(test_accept_above_what_we_asked_during_the_dip);
  RUN_TEST(test_late_accept_above_what_we_asked_is_not_a_confirmation);
  RUN_TEST(test_headphones_that_round_up_are_not_asked_for_ever);
  RUN_TEST(test_ours_again_never_steps_them_up);
  RUN_TEST(test_ours_again_after_an_unanswered_probe_is_at_most_the_last_sent);
  RUN_TEST(test_their_report_after_a_stream_they_started_sends_their_level_back);
  RUN_TEST(test_echo_of_an_older_louder_command_is_not_an_answer);
  RUN_TEST(test_probe_has_a_hard_deadline);
  RUN_TEST(test_unanswered_probe_grace_before_the_first_stream);
  RUN_TEST(test_avrcp_back_on_a_link_that_has_played_probes_late);
  RUN_TEST(test_late_accept_before_any_audio_hands_over);
  RUN_TEST(test_rounded_accept_in_absolute_mode_keeps_the_ui);
  RUN_TEST(test_late_capabilities_while_streaming_dip_then_ask);
  RUN_TEST(test_late_capabilities_while_paused_snap_and_send);
  RUN_TEST(test_late_probe_waits_when_the_stream_stops);
  RUN_TEST(test_late_capabilities_just_after_a_stop_wait);
  RUN_TEST(test_late_probe_stop_after_the_dip_settled);
  RUN_TEST(test_late_probe_timeout_ramps_to_software);
  RUN_TEST(test_user_volume_during_the_dip_before_the_send);
  RUN_TEST(test_user_volume_during_the_dip_after_the_send);
  RUN_TEST(test_notification_during_a_late_probe);
  RUN_TEST(test_their_key_during_the_dip_keeps_the_silence_until_answered);
  RUN_TEST(test_notification_before_a_late_probe_is_sent);
  RUN_TEST(test_late_probe_caps_a_loud_volume);
  RUN_TEST(test_link_down_during_the_dip);
  RUN_TEST(test_avrcp_down_during_the_dip);
  RUN_TEST(test_stream_starting_during_the_dip_stays_silent);
  RUN_TEST(test_headroom_setting_retargets_by_the_ramp);
  RUN_TEST(test_their_change_after_an_unanswered_probe_stops_our_sends);
  RUN_TEST(test_accept_while_streaming_ramps_to_the_headphones);
  RUN_TEST(test_audio_waits_longer_once_avrcp_is_up);
  RUN_TEST(test_audio_waits_for_the_volume_to_settle);
  RUN_TEST(test_repeated_link_up_changes_nothing);
  RUN_TEST(test_user_volume_in_software_mode_ramps_our_gain);
  RUN_TEST(test_user_volume_while_probing_does_both);
  RUN_TEST(test_user_volume_in_absolute_mode_is_sent_not_applied);
  RUN_TEST(test_avrcp_loss_ramps_back_down_to_software);
  RUN_TEST(test_avrcp_reconnect_on_the_same_link_probes_again);
  RUN_TEST(test_accept_without_a_probe_is_ignored);
  RUN_TEST(test_next_link_never_starts_at_unity);
  RUN_TEST(test_link_after_avrcp_loss_waits_for_new_capabilities);
  RUN_TEST(test_nothing_happens_to_the_mode_without_a_link);
  RUN_TEST(test_invariants_under_random_events);
  return UNITY_END();
}
