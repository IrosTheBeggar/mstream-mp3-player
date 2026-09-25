// Host unit tests for AbsVolumePolicy (who applies the Bluetooth volume).
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
  TEST_ASSERT_FALSE(a.sendAbsolute);
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

void test_after_an_unanswered_probe_the_volume_is_still_sent() {
  AbsVolumePolicy p = probing();
  const auto t = p.tick(5000);  // timed out
  TEST_ASSERT_TRUE(t.probeUnanswered);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  const auto s = p.userSet(40, 6000);  // our gain, and the headphones may apply it too
  TEST_ASSERT_TRUE(s.gainChanged);
  TEST_ASSERT_FALSE(s.snap);
  TEST_ASSERT_TRUE(s.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(51, s.absolute);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(40), p.gainQ15());
}

// The probe (38) times out and the stream plays at the software level; then
// its answer arrives. They apply what we send: they take over, and our gain
// rises to the headroom at the gain stage's slow rate (not a lift: audio
// plays). Commands still on their way are made to end on ours.
void test_late_accept_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  p.userSet(40, 6000);  // sent 51 (they may apply it silently)
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(51, a.absolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
}

// The same with an echo of a command of ours instead of an ACCEPT.
void test_late_echo_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  p.userSet(40, 6000);  // 51
  const auto a = p.headsetChanged(48, 6300);  // 16-step headphones
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
}

// A late answer while nothing has been heard yet (paused all along) is as
// good as one in time; commands sent since are made to end on ours.
void test_late_accept_before_any_audio_hands_over() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.userSet(20, 6000);  // sent 25
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(a.lift);
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, a.absolute);
  TEST_ASSERT_EQUAL_UINT8(20, p.percent());
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

// No answer: software volume, still sent along; the gain rises from silence
// to the software level at the slow rate.
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
  const auto b = p.userSet(40, 5000);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(51, b.absolute);
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
// theirs); one above it is answered with ours again, still in silence.
void test_notification_during_a_late_probe() {
  AbsVolumePolicy p = lateProbing();  // sent 38
  const auto a = p.headsetChanged(30, kLateSentMs + 100);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_TRUE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(30), p.percent());
  TEST_ASSERT_FALSE(a.sendAbsolute);

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
// own change shows the listener sets their level there: we stop sending ours
// (a send from our stale value could step them up by more than one step).
void test_their_change_after_an_unanswered_probe_stops_our_sends() {
  AbsVolumePolicy p = probing();
  p.tick(5000);  // unanswered: still sent along
  p.streamActive(true, 5100);
  TEST_ASSERT_TRUE(p.userSet(40, 6000).sendAbsolute);
  const auto a = p.headsetChanged(20, 9000);  // not an echo of 51
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.volumeChanged);
  const auto b = p.userSet(50, 10000);
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
  TEST_ASSERT_TRUE(r.audioReady(100 + AbsVolumePolicy::kProbeTimeoutMs));
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
// presses its buttons. What the listener hears is its level times our gain.
namespace {
double headsetDb(int level) { return level <= 0 ? -100.0 : 40.0 * (level / 127.0 - 1.0); }
double gainDb(uint16_t q) { return q == 0 ? -100.0 : 20.0 * std::log10(q / 32768.0); }
// What the listener hears; our gain at 0 is silence, whatever their level.
double heardDb(int level, uint16_t q) { return q == 0 ? -1000.0 : headsetDb(level) + gainDb(q); }
int quantise(int abs) { return abs >= 124 ? 127 : ((abs + 4) / 8) * 8; }  // ~16 steps
}  // namespace

void test_invariants_under_random_events() {
  uint32_t seed = 12345;
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  AbsVolumePolicy p(30);
  uint32_t now = 0;
  bool streaming = false;
  bool heardLink = false;  // media has flowed on the current link
  // The headset.
  int level = 64;
  bool applies = true, answers = true, echoes = true;
  std::vector<int> acceptsDue, echoesDue;
  int probesSent = 0, lifts = 0, heardChecks = 0, lateProbes = 0, lateSends = 0, rampedRises = 0;
  uint32_t silentMs = 0;  // when our gain was last taken to silence
  uint32_t stopMs = 0;    // when media last stopped flowing
  int lastSent = -1;
  for (int i = 0; i < 40000; ++i) {
    now += rnd(700);
    const uint16_t gainBefore = p.gainQ15();
    const uint8_t percentBefore = p.percent();
    const bool wasStreaming = streaming;
    const bool wasHeard = heardLink && p.linked();
    const bool wasDucked = p.ducked();
    AbsVolumePolicy::Actions a;
    const uint32_t what = rnd(12);
    int levelBefore = level;
    switch (what) {
      case 0:
        if (!p.linked()) {  // a new link, maybe other headphones
          level = static_cast<int>(rnd(128));
          applies = rnd(4) != 0;
          answers = rnd(3) != 0;
          echoes = rnd(2) != 0;
          acceptsDue.clear();
          echoesDue.clear();
          heardLink = false;
          streaming = false;
          levelBefore = level;
        }
        a = p.linkUp(now);
        break;
      case 1:
        a = p.linkDown();
        streaming = false;
        heardLink = false;
        break;
      case 2:
        if (rnd(2)) p.avrcpUp(now);
        a = p.capabilities(rnd(4) != 0, now);
        break;
      case 3: a = p.avrcpDown(); break;
      case 4:
        if (acceptsDue.empty()) break;
        a = p.accepted(static_cast<uint8_t>(acceptsDue.front()), now);
        acceptsDue.erase(acceptsDue.begin());
        break;
      case 5:
        if (echoesDue.empty()) break;
        a = p.headsetChanged(static_cast<uint8_t>(echoesDue.front()), now);
        echoesDue.erase(echoesDue.begin());
        break;
      case 6:  // the listener presses the headset's own buttons
        level = level + (rnd(2) ? 8 : -8);
        level = level < 0 ? 0 : (level > 127 ? 127 : level);
        levelBefore = level;  // their doing, not ours
        a = p.headsetChanged(static_cast<uint8_t>(level), now);
        break;
      case 7: a = p.userSet(static_cast<uint8_t>(rnd(101)), now); break;
      case 8:  // (only linked: StreamControl has no stream without a link)
        if (!p.linked()) break;
        streaming = rnd(2) != 0;
        if (wasStreaming && !streaming) stopMs = now;
        a = p.streamActive(streaming, now);
        if (streaming) heardLink = true;
        break;
      default: a = p.tick(now); break;
    }
    if (a.sendAbsolute && p.linked() && applies) {
      level = quantise(a.absolute);
      if (answers) acceptsDue.push_back(level);
      if (echoes) echoesDue.push_back(level);
    }
    if (a.lift) ++lifts;
    if (p.ducked() && !wasDucked) ++lateProbes;
    if (p.gainQ15() == 0 && gainBefore != 0) silentMs = now;

    TEST_ASSERT_TRUE(p.gainQ15() <= vol::kHeadroomQ15);       // headroom always applied
    // Jumps only go down. (A link lost during a dip snaps to the next link's
    // level from silence: nothing plays, and the gain stage's snap never
    // raises anyway.)
    if (a.snap && !(what == 1 && gainBefore == 0)) TEST_ASSERT_TRUE(p.gainQ15() <= gainBefore);
    if (p.mode() == Mode::Absolute) TEST_ASSERT_TRUE(p.linked());
    if (p.ducked()) {
      TEST_ASSERT_EQUAL(Mode::Probing, p.mode());
      TEST_ASSERT_EQUAL_UINT16(0, p.gainQ15());
    } else if (p.mode() != Mode::Absolute) {
      TEST_ASSERT_EQUAL_UINT16(softwareGain(p.percent()), p.gainQ15());
    }
    if (p.mode() == Mode::Probing) TEST_ASSERT_FALSE(p.audioReady(now));
    if (a.sendAbsolute) TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(p.percent()), a.absolute);
    // No echo loop: a change of theirs is never sent back once they have the volume.
    if (what == 5 || what == 6) TEST_ASSERT_FALSE(a.sendAbsolute && p.mode() == Mode::Absolute);
    // A lift only while nothing has been heard on the link.
    if (a.lift) TEST_ASSERT_FALSE(wasHeard || wasStreaming || streaming);
    // A command that isn't the user's: before anything was heard, or into
    // silence that has reached their output, or ours again (what they
    // already have from the user).
    if (a.sendAbsolute && what != 7) {
      ++probesSent;
      if (wasHeard || wasStreaming || streaming) {
        const bool silent = p.gainQ15() == 0 && (gainBefore == 0 || (a.snap && !streaming));
        if (silent) {
          ++lateSends;
          // The silence has reached their output: the fade has had the
          // settle time, or nothing has flowed for that long (what they
          // buffered before a stop has played out).
          TEST_ASSERT_TRUE(now - silentMs >= AbsVolumePolicy::kDuckSettleMs ||
                           (!streaming && now - stopMs >= AbsVolumePolicy::kDuckSettleMs));
        } else {
          TEST_ASSERT_EQUAL_INT(lastSent, a.absolute);
        }
      }
    }
    if (a.sendAbsolute) lastSent = a.absolute;
    // Once audio has flowed on a link, what the listener hears (their level
    // times our gain) never jumps up except by the user's own step: an
    // answer, a notification, a timeout, the stream starting or stopping only
    // ever raise our gain by the ramp (the gain stage's slow rate) with their
    // level unchanged, and a handover lands no louder than what we sent.
    // A step down never makes it louder (quantisation aside).
    if (wasHeard && p.linked()) {
      ++heardChecks;
      const double before = heardDb(levelBefore, gainBefore);
      const double after = heardDb(level, p.gainQ15());
      if (what != 7) {
        if (after > before + 1e-9) {
          ++rampedRises;
          TEST_ASSERT_TRUE(a.gainChanged && !a.snap && !a.lift);
          TEST_ASSERT_EQUAL_INT(levelBefore, level);
          // (A stale notification may set the UI below what they were last sent.)
          if (p.mode() == Mode::Absolute) {
            TEST_ASSERT_TRUE(level <= std::max<int>(vol::percentToAbs(p.percent()), lastSent) + 5);
          }
        }
      } else if (p.percent() <= percentBefore) {
        TEST_ASSERT_TRUE(after <= before + 1.5);
      }
    }
  }
  TEST_ASSERT_TRUE(probesSent > 50);
  TEST_ASSERT_TRUE(lifts > 50);
  TEST_ASSERT_TRUE(heardChecks > 1000);
  TEST_ASSERT_TRUE(lateProbes > 20);
  TEST_ASSERT_TRUE(lateSends > 20);
  TEST_ASSERT_TRUE(rampedRises > 20);
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
  RUN_TEST(test_louder_notification_during_probe_is_answered_with_ours);
  RUN_TEST(test_probe_times_out_to_software);
  RUN_TEST(test_probe_timeout_survives_millis_wraparound);
  RUN_TEST(test_after_an_unanswered_probe_the_volume_is_still_sent);
  RUN_TEST(test_late_accept_after_audio_ramps_to_the_headphones);
  RUN_TEST(test_late_echo_after_audio_ramps_to_the_headphones);
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
