// Host unit tests for AbsVolumePolicy (who applies the Bluetooth volume).
// Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

// Their level is unknown (they may have applied the probe silently): only a
// step down is sent, to at most the last value sent; a rise is our gain's.
void test_after_an_unanswered_probe_only_steps_down_are_sent() {
  AbsVolumePolicy p = probing();  // sent 38
  const auto t = p.tick(5000);    // timed out
  TEST_ASSERT_TRUE(t.probeUnanswered);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  const auto s = p.userSet(20, 6000);  // our gain, and the headphones may apply it too
  TEST_ASSERT_TRUE(s.gainChanged);
  TEST_ASSERT_FALSE(s.snap);
  TEST_ASSERT_TRUE(s.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, s.absolute);
  TEST_ASSERT_EQUAL_UINT16(softwareGain(20), p.gainQ15());
  const auto u = p.userSet(40, 6100);  // above what was sent: our gain only
  TEST_ASSERT_TRUE(u.gainChanged);
  TEST_ASSERT_FALSE(u.sendAbsolute);
  TEST_ASSERT_FALSE(p.userSet(25, 6200).sendAbsolute);  // 32: still above 25
  const auto d = p.userSet(15, 6300);
  TEST_ASSERT_TRUE(d.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(19, d.absolute);
}

// The probe (38) times out and the stream plays at the software level; then
// its answer arrives. They apply what we send: they take over, and our gain
// rises to the headroom at the gain stage's slow rate (not a lift: audio
// plays). A step down sent since is on its way; in case it was dropped,
// ours (below what they confirmed) goes again so they end on it.
void test_late_accept_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  p.userSet(20, 6000);  // sent 25 (they may apply it silently)
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, p.gainQ15());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, a.absolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(20, p.percent());
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

// The same with an echo of a command of ours instead of an ACCEPT.
void test_late_echo_after_audio_ramps_to_the_headphones() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  p.userSet(20, 6000);                        // 25
  const auto a = p.headsetChanged(24, 6300);  // 16-step headphones
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ramped(a));
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT8(20, p.percent());
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
  p.userSet(70, 6100);  // a rise since: our gain only
  const auto b = p.accepted(127, 6200);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  // A step down is news: sent, and what is asked for again after a stale
  // answer to it is at most what we last sent.
  TEST_ASSERT_EQUAL_UINT8(25, p.userSet(20, 6300).absolute);
  const auto c = p.accepted(127, 6400);
  TEST_ASSERT_TRUE(c.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, c.absolute);
  // Its real answer confirms.
  TEST_ASSERT_TRUE(ramped(p.accepted(25, 6500)));
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
}

// Headphones that can't set our level (they round up by more than
// kEchoTolerance, or have a floor) answer every command above it. Ours goes
// again once, not in reply to the answer to that repeat: no endless
// SET/ACCEPT trade. The probe runs out; a later step down that they do set
// confirms.
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
  // A step down they can set confirms (nothing heard yet: a lift).
  TEST_ASSERT_EQUAL_UINT8(32, p.userSet(25, 5000).absolute);
  const auto ok = p.accepted(32, 5100);
  TEST_ASSERT_EQUAL(Mode::Absolute, p.mode());
  TEST_ASSERT_TRUE(ok.lift);
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

// After an unanswered probe, an echo that doesn't confirm (of an older,
// louder command of ours) gets ours again, but never above what we last sent:
// a rise since was our gain's alone, and a louder command could land after
// the grace, once audio plays.
void test_echo_after_an_unanswered_probe_is_answered_with_at_most_the_last_sent() {
  AbsVolumePolicy p(50);
  p.linkUp(0);
  TEST_ASSERT_EQUAL_UINT8(64, p.capabilities(true, 0).absolute);
  p.tick(AbsVolumePolicy::kProbeTimeoutMs);  // unanswered
  TEST_ASSERT_EQUAL_UINT8(57, p.userSet(45, 2100).absolute);
  TEST_ASSERT_EQUAL_UINT8(51, p.userSet(40, 2200).absolute);
  TEST_ASSERT_FALSE(p.userSet(50, 2300).sendAbsolute);  // a rise: our gain only
  const auto a = p.headsetChanged(57, 2500);           // the 57 landed
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(a.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(51, a.absolute);
}

// A stream the headphones started themselves has flowed during the probe
// (so no handover by the lift), and a notification of theirs below ours ends
// it: the probe may still be on its way, and landing later it would step
// them up while our stream plays. Their level goes after it.
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
  TEST_ASSERT_TRUE(p.audioReady(300));
  // The probe lands, then theirs: nothing more is sent, nothing moves.
  p.streamActive(true, 600);
  for (const auto& b : {p.accepted(64, 1000), p.headsetChanged(64, 1010), p.accepted(20, 1100),
                        p.headsetChanged(20, 1110)}) {
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
  AbsVolumePolicy q = probing();
  q.tick(5000);
  q.streamActive(true, 5100);
  q.userSet(20, 6000);  // 25; the echo window of the probe's 38 is over
  q.userSet(10, 6100);  // 13
  const auto b = q.headsetChanged(24, 6200);  // near 25, above 13
  TEST_ASSERT_EQUAL(Mode::Software, q.mode());
  TEST_ASSERT_FALSE(b.gainChanged);
  TEST_ASSERT_FALSE(q.userSet(5, 6300).sendAbsolute);  // their level now: no more sends
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
  TEST_ASSERT_FALSE(p.userSet(40, 5000).sendAbsolute);  // a rise: our gain only
  const auto b = p.userSet(20, 5100);
  TEST_ASSERT_TRUE(b.sendAbsolute);
  TEST_ASSERT_EQUAL_UINT8(25, b.absolute);
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
  TEST_ASSERT_TRUE(a.sendAbsolute);  // theirs, after our 38 that may still land
  TEST_ASSERT_EQUAL_UINT8(30, a.absolute);
  TEST_ASSERT_FALSE(p.headsetChanged(38, kLateSentMs + 150).volumeChanged);  // it did: an echo

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
// own change shows the listener sets their level there: we stop sending ours
// (a send from our stale value could step them up by more than one step).
void test_their_change_after_an_unanswered_probe_stops_our_sends() {
  AbsVolumePolicy p = probing();
  p.tick(5000);  // unanswered: still sent along
  p.streamActive(true, 5100);
  TEST_ASSERT_TRUE(p.userSet(20, 6000).sendAbsolute);
  const auto a = p.headsetChanged(8, 9000);  // not an echo of 25
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.volumeChanged);
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
// It sets ~16 steps: to the nearest, rounding up, or above a floor. What
// the listener hears is its level times our gain.
namespace {
double headsetDb(int level) { return level <= 0 ? -100.0 : 40.0 * (level / 127.0 - 1.0); }
double gainDb(uint16_t q) { return q == 0 ? -100.0 : 20.0 * std::log10(q / 32768.0); }
// What the listener hears; our gain at 0 is silence, whatever their level.
double heardDb(int level, uint16_t q) { return q == 0 ? -1000.0 : headsetDb(level) + gainDb(q); }
// How the headset sets a level: ~16 steps, to the nearest one, up to the
// next one (more than kEchoTolerance above what we asked for, at times), or
// the nearest one but never below a floor.
enum class Rounding { Nearest, Up, Floor };
int quantise(int abs, Rounding r) {
  abs = abs < 0 ? 0 : (abs > 127 ? 127 : abs);
  switch (r) {
    case Rounding::Up: return abs > 120 ? 127 : ((abs + 7) / 8) * 8;
    case Rounding::Floor: return std::max(16, abs >= 124 ? 127 : ((abs + 4) / 8) * 8);
    default: return abs >= 124 ? 127 : ((abs + 4) / 8) * 8;
  }
}

// A command sent in reply to an answer (an ACCEPT or a notification of what
// they applied) of a command that was itself one: how deep that goes. Ours
// again once (1), then a late confirmation's (2). Deeper would be a loop
// with headphones that can't set our level.
constexpr int kMaxReplyDepth = 2;

void runRandomEvents(uint32_t seed, Rounding rounding, bool full) {
  auto rnd = [&seed](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return (seed >> 8) % n;
  };
  auto quantise = [rounding](int abs) { return ::quantise(abs, rounding); };
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
  };
  struct Due {
    int level;
    int depth;  // of the command it answers
  };
  std::deque<InFlight> inFlight;
  std::vector<Due> acceptsDue, echoesDue;
  auto apply = [&](int value, int depth) {
    level = quantise(value);
    if (answers) acceptsDue.push_back({level, depth});
    if (echoes) echoesDue.push_back({level, depth});
  };
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
    now += rnd(700);
    const uint16_t gainBefore = p.gainQ15();
    const uint8_t percentBefore = p.percent();
    const Mode modeBefore = p.mode();
    const bool wasStreaming = streaming;
    const bool wasHeard = heardLink && p.linked();
    const bool wasDucked = p.ducked();
    AbsVolumePolicy::Actions a;
    const uint32_t what = rnd(14);
    int levelBefore = level;
    int delivered = -1;  // what an ACCEPT or a notification said
    int deliveredDepth = -1;  // ...answering a command of this depth (-1: a key of theirs)
    bool landed = false;
    InFlight cmd{};
    switch (what) {
      case 0:
        if (!p.linked()) {  // a new link, maybe other headphones
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
        break;
      case 4:
        if (acceptsDue.empty()) break;
        delivered = acceptsDue.front().level;
        deliveredDepth = acceptsDue.front().depth;
        acceptsDue.erase(acceptsDue.begin());
        a = p.accepted(static_cast<uint8_t>(delivered), now);
        break;
      case 5:
        if (echoesDue.empty()) break;
        delivered = echoesDue.front().level;
        deliveredDepth = echoesDue.front().depth;
        echoesDue.erase(echoesDue.begin());
        a = p.headsetChanged(static_cast<uint8_t>(delivered), now);
        break;
      case 6:  // the listener presses the headset's own buttons
        // (Their answers and notifications arrive in order: one AVRCP
        // channel, then BTC_TASK and BtAppT's queue. A key pressed after a
        // command of ours landed is reported after its ACCEPT and echo.)
        if (!echoesDue.empty() || !acceptsDue.empty()) break;
        level = quantise(level + (rnd(2) ? 8 : -8));
        levelBefore = level;  // their doing, not ours
        delivered = level;
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
      case 9:
      case 10:  // a command of ours lands at the headset
        if (inFlight.empty() || inFlight.front().at > now) break;
        cmd = inFlight.front();
        inFlight.pop_front();
        landed = true;
        ++landings;
        apply(cmd.value, cmd.depth);
        break;
      default: a = p.tick(now); break;
    }
    const int depth = deliveredDepth >= 0 ? deliveredDepth + 1 : 0;
    if (a.sendAbsolute && p.linked() && applies) {
      const uint32_t kind = rnd(3);
      const uint32_t delay = kind == 0 ? 0 : (kind == 1 ? rnd(400) : rnd(6000));
      if (delay == 0 && inFlight.empty()) {
        apply(a.absolute, depth);
      } else {
        uint32_t at = now + delay;
        if (!inFlight.empty() && static_cast<int32_t>(inFlight.back().at - at) > 0) at = inFlight.back().at;
        inFlight.push_back({a.absolute, at, what == 7, p.percent(), depth});
        ++delayed;
      }
    }
    if (a.lift) ++lifts;
    if (p.ducked() && !wasDucked) ++lateProbes;
    if (p.gainQ15() == 0 && gainBefore != 0) {
      // Silent at their output once the fade has passed their buffer; at
      // once if nothing has flowed for that long anyway.
      const bool settled = !wasStreaming && !streaming && now - stopMs >= AbsVolumePolicy::kDuckSettleMs;
      silentMs = settled ? now - AbsVolumePolicy::kDuckSettleMs : now;
    }

    TEST_ASSERT_TRUE(p.gainQ15() <= vol::kHeadroomQ15);  // headroom always applied
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
      // After an unanswered probe, only steps down go out: the user's, ours
      // again, their own level back.
      if (p.mode() == Mode::Software) TEST_ASSERT_TRUE(lastSent >= 0 && a.absolute <= lastSent);
    }
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
         "%d timeouts, %d landing rises, reply depth up to %d, %d steps from a UI above them\n",
         static_cast<int>(rounding), probesSent, lifts, lateProbes, handBacks, timeouts, landingRises,
         maxDepth, desynced);
  TEST_ASSERT_TRUE(probesSent > 50);
  TEST_ASSERT_TRUE(heardChecks > 1000);
  TEST_ASSERT_TRUE(landings > 300);
  if (!full) return;
  TEST_ASSERT_TRUE(lifts > 50);
  TEST_ASSERT_TRUE(lateProbes > 20);
  TEST_ASSERT_TRUE(lateSends > 20);
  TEST_ASSERT_TRUE(rampedRises > 20);
  TEST_ASSERT_TRUE(delayed > 1000);
  TEST_ASSERT_TRUE(handBacks > 5);
  TEST_ASSERT_TRUE(timeouts > 20);
  TEST_ASSERT_TRUE(landingRises > 0);  // the residual risk does occur in this model
}
}  // namespace

void test_invariants_under_random_events() {
  runRandomEvents(12345, Rounding::Nearest, /*full=*/true);
  // Headphones that can't always set our level: never a command loop, and
  // the same invariants.
  runRandomEvents(777, Rounding::Up, /*full=*/false);
  runRandomEvents(4242, Rounding::Floor, /*full=*/false);
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
  RUN_TEST(test_louder_notification_during_probe_is_answered_with_ours);
  RUN_TEST(test_probe_times_out_to_software);
  RUN_TEST(test_probe_timeout_survives_millis_wraparound);
  RUN_TEST(test_after_an_unanswered_probe_only_steps_down_are_sent);
  RUN_TEST(test_late_accept_after_audio_ramps_to_the_headphones);
  RUN_TEST(test_late_accept_after_a_rise_keeps_their_level);
  RUN_TEST(test_late_echo_after_audio_ramps_to_the_headphones);
  RUN_TEST(test_accept_above_what_we_asked_is_not_an_answer);
  RUN_TEST(test_accept_above_what_we_asked_during_the_dip);
  RUN_TEST(test_late_accept_above_what_we_asked_is_not_a_confirmation);
  RUN_TEST(test_headphones_that_round_up_are_not_asked_for_ever);
  RUN_TEST(test_ours_again_never_steps_them_up);
  RUN_TEST(test_echo_after_an_unanswered_probe_is_answered_with_at_most_the_last_sent);
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
