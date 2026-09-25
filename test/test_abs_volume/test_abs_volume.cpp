// Host unit tests for AbsVolumePolicy (who applies the Bluetooth volume).
// Run: pio test -e native
#include <unity.h>

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
// its answer arrives. Taking over now would raise what the listener hears by
// the whole software attenuation (~28 dB at 30 %): it changes nothing.
void test_late_accept_after_audio_changes_nothing() {
  AbsVolumePolicy p = probing();
  p.tick(5000);
  p.streamActive(true, 5100);
  p.userSet(40, 6000);  // sent 51 (they may apply it silently)
  const uint16_t gain = p.gainQ15();
  const auto a = p.accepted(38, 6500);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL_UINT16(gain, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(40, p.percent());
  // The stream stopping doesn't hand over either: the next one would be louder.
  const auto b = p.streamActive(false, 9000);
  TEST_ASSERT_FALSE(b.gainChanged);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
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

// Capabilities that arrive after audio has flowed: a probe would step the
// headphones from a level we don't know, now or at the next pause (the next
// resume would be louder). Software for the rest of this link.
void test_no_probe_once_audio_has_flowed() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  p.streamActive(true, 100);
  const auto a = p.capabilities(true, 200);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  const auto b = p.streamActive(false, 5000);
  TEST_ASSERT_FALSE(b.sendAbsolute);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_TRUE(p.audioReady(5000));
  // The next link starts fresh.
  p.linkDown();
  TEST_ASSERT_TRUE(p.linkUp(9000).sendAbsolute);
}

// The review's blocker: caps mid-stream (probe skipped), then the user
// presses VOLUME DOWN on the headphones. The notification must not hand over
// (that ramped our gain up ~28 dB) nor move the UI volume.
void test_notification_after_audio_never_raises_the_gain() {
  AbsVolumePolicy p(30);
  p.linkUp(0);
  p.streamActive(true, 1600);
  p.capabilities(true, 2000);
  const uint16_t gain = p.gainQ15();
  const auto a = p.headsetChanged(102, 5000);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_FALSE(a.sendAbsolute);
  TEST_ASSERT_FALSE(a.volumeChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_EQUAL_UINT16(gain, p.gainQ15());
  TEST_ASSERT_EQUAL_UINT8(30, p.percent());
  TEST_ASSERT_EQUAL_INT(102, p.headsetAbsolute());
  // Nor once the stream stops (the next resume would be louder).
  TEST_ASSERT_FALSE(p.streamActive(false, 8000).gainChanged);
  TEST_ASSERT_FALSE(p.headsetChanged(110, 9000).gainChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
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

// An ACCEPT that arrives while a stream the headphones started runs: no handover.
void test_accept_while_streaming_keeps_the_software_gain() {
  AbsVolumePolicy p = probing();
  p.streamActive(true, 1050);
  const uint16_t gain = p.gainQ15();
  const auto a = p.accepted(38, 1100);
  TEST_ASSERT_FALSE(a.gainChanged);
  TEST_ASSERT_EQUAL(Mode::Software, p.mode());
  TEST_ASSERT_EQUAL_UINT16(gain, p.gainQ15());
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
  int probesSent = 0, lifts = 0, heardChecks = 0;
  for (int i = 0; i < 40000; ++i) {
    now += rnd(700);
    const uint16_t gainBefore = p.gainQ15();
    const uint8_t percentBefore = p.percent();
    const bool wasStreaming = streaming;
    const bool wasHeard = heardLink && p.linked();
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

    TEST_ASSERT_TRUE(p.gainQ15() <= vol::kHeadroomQ15);       // headroom always applied
    if (a.snap) TEST_ASSERT_TRUE(p.gainQ15() <= gainBefore);  // jumps only go down
    if (p.mode() == Mode::Absolute) TEST_ASSERT_TRUE(p.linked());
    if (p.mode() != Mode::Absolute) TEST_ASSERT_EQUAL_UINT16(softwareGain(p.percent()), p.gainQ15());
    if (p.mode() == Mode::Probing) TEST_ASSERT_FALSE(p.audioReady(now));
    if (a.sendAbsolute) TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(p.percent()), a.absolute);
    // No echo loop: a change of theirs is never sent back once they have the volume.
    if (what == 5 || what == 6) TEST_ASSERT_FALSE(a.sendAbsolute && p.mode() == Mode::Absolute);
    // A lift, or a command that isn't the user's, only while nothing has been
    // heard on the link.
    if (a.lift) TEST_ASSERT_FALSE(wasHeard || wasStreaming || streaming);
    if (a.sendAbsolute && what != 7) {
      ++probesSent;
      TEST_ASSERT_FALSE(wasHeard || wasStreaming || streaming);
    }
    // Once audio has flowed on a link, what the listener hears (their level
    // times our gain) never rises except by the user's own step: never by an
    // answer, a notification, a timeout, the stream starting or stopping, and
    // a step down never makes it louder (quantisation aside).
    if (wasHeard && p.linked()) {
      ++heardChecks;
      const double before = headsetDb(levelBefore) + gainDb(gainBefore);
      const double after = headsetDb(level) + gainDb(p.gainQ15());
      if (what != 7) {
        TEST_ASSERT_TRUE(after <= before + 1e-9);
      } else if (p.percent() <= percentBefore) {
        TEST_ASSERT_TRUE(after <= before + 1.5);
      }
    }
  }
  TEST_ASSERT_TRUE(probesSent > 50);
  TEST_ASSERT_TRUE(lifts > 50);
  TEST_ASSERT_TRUE(heardChecks > 1000);
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
  RUN_TEST(test_late_accept_after_audio_changes_nothing);
  RUN_TEST(test_late_accept_before_any_audio_hands_over);
  RUN_TEST(test_rounded_accept_in_absolute_mode_keeps_the_ui);
  RUN_TEST(test_no_probe_once_audio_has_flowed);
  RUN_TEST(test_notification_after_audio_never_raises_the_gain);
  RUN_TEST(test_their_change_after_an_unanswered_probe_stops_our_sends);
  RUN_TEST(test_accept_while_streaming_keeps_the_software_gain);
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
