// Host unit tests for BtControl: the media stream and the headphones' volume
// together, as PlayerA2dp drives them from the Bluetooth stack's events.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <vector>

#include "BtControl.h"
#include "GainRamp.h"
#include "VolumeMath.h"

using Cmd = StreamControl::Cmd;
using Mode = AbsVolumePolicy::Mode;
using Move = BtControl::GainMove;

namespace {
struct FakeIo : BtControl::Io {
  std::vector<Cmd> cmds;
  std::vector<uint8_t> sent;
  uint16_t gain = 0;
  std::vector<Move> moves;
  int volumeChanges = 0, remoteSuspends = 0, gaveUps = 0, publishes = 0;
  bool failNext = false;

  bool mediaCtrl(Cmd c) override {
    if (failNext) {
      failNext = false;
      return false;
    }
    cmds.push_back(c);
    return true;
  }
  void sendAbsoluteVolume(uint8_t a) override { sent.push_back(a); }
  void setGain(uint16_t q, Move m) override {
    gain = q;
    moves.push_back(m);
  }
  void volumeChanged() override { ++volumeChanges; }
  void volumeModeChanged(Mode, bool) override {}
  void remoteSuspend() override { ++remoteSuspends; }
  void commandGaveUp() override { ++gaveUps; }
  void streamStarted(uint32_t) override {}
  void publish() override { ++publishes; }

  Cmd last() const { return cmds.empty() ? Cmd::None : cmds.back(); }
};

uint16_t softwareGain(uint8_t percent) { return vol::mulQ15(vol::kHeadroomQ15, vol::softwareVolumeQ15(percent)); }

// Our stream up: CHECK, START, their acks and STARTED.
void startStream(BtControl& c, FakeIo& io, uint32_t t) {
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  c.mediaAck(Cmd::CheckReady, true, t);
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());
  c.mediaAck(Cmd::Start, true, t + 10);
  c.audioState(true, t + 20);
  TEST_ASSERT_TRUE(c.stream().streaming());
}
}  // namespace

void setUp() {}
void tearDown() {}

// Playing when absolute-volume headphones connect: the stream waits for the
// probe's answer, and then fades in straight to the right level.
void test_link_while_playing_waits_for_the_probe_then_lifts() {
  FakeIo io;
  BtControl c(io, 30);
  c.setWanted(true, 0);
  c.linkUp(100);
  TEST_ASSERT_EQUAL(Move::Snap, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), io.gain);
  TEST_ASSERT_TRUE(io.cmds.empty());  // the capabilities are on their way
  c.avrcpUp(300);
  c.capabilities(true, 400);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));  // the probe
  TEST_ASSERT_EQUAL_UINT8(38, io.sent[0]);
  c.tick(1800);
  TEST_ASSERT_TRUE(io.cmds.empty());  // still held back
  c.accepted(38, 1900);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Lift, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());  // the gate opened at once
  startStream(c, io, 1950);
}

// The review's case: no capabilities within the wait, the stream starts, and
// they arrive between START and STARTED. Media may already flow: a late
// probe, our gain dips to silence before anything is sent.
void test_capabilities_between_start_and_started_dip_first() {
  FakeIo io;
  BtControl c(io, 30);
  c.setWanted(true, 0);
  c.linkUp(0);
  c.tick(AbsVolumePolicy::kCapsWaitMs);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  c.mediaAck(Cmd::CheckReady, true, 1600);
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());
  c.capabilities(true, 1610);  // START out, not acknowledged yet
  TEST_ASSERT_TRUE(io.sent.empty());
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  c.mediaAck(Cmd::Start, true, 1620);
  c.capabilities(true, 1630);  // acknowledged, STARTED not reported yet
  c.audioState(true, 1640);
  TEST_ASSERT_TRUE(io.sent.empty());
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());  // the stream runs on, silent
  TEST_ASSERT_TRUE(c.stream().streaming());
  const uint32_t sentMs = 1610 + AbsVolumePolicy::kDuckSettleMs;
  c.tick(sentMs);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(38, io.sent[0]);
  // Their buttons now, louder than we asked: ours again, still silent; the
  // UI keeps our volume.
  c.headsetChanged(102, sentMs + 100);
  TEST_ASSERT_EQUAL(2, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(38, io.sent[1]);
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  TEST_ASSERT_EQUAL_UINT8(30, c.volume().percent());
  // No answer: software, up from silence by the ramp.
  c.tick(sentMs + 100 + AbsVolumePolicy::kLateProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), io.gain);
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());
}

// Powerbeats Pro: the headphones' AVRCP and capabilities arrive seconds after
// the stream started. The stream is neither suspended nor restarted; our
// gain dips, the command goes out once the silence reached them, and their
// answer ramps our gain up to the headroom.
void test_late_capabilities_while_started_leave_the_stream_alone() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.setWanted(true, 10);
  c.tick(AbsVolumePolicy::kCapsWaitMs);  // no AVRCP yet: software
  startStream(c, io, 1600);
  const size_t cmds = io.cmds.size();
  c.avrcpUp(7000);
  c.capabilities(true, 7600);
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  TEST_ASSERT_TRUE(io.sent.empty());
  c.tick(7600 + AbsVolumePolicy::kDuckSettleMs - 1);
  TEST_ASSERT_TRUE(io.sent.empty());
  c.tick(7600 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(38, io.sent[0]);
  TEST_ASSERT_EQUAL(Move::Snap, io.moves.back());  // the silence, made sure of
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  c.accepted(38, 7600 + AbsVolumePolicy::kDuckSettleMs + 100);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL(cmds, io.cmds.size());
  TEST_ASSERT_EQUAL(StreamControl::State::Started, c.stream().state());
  TEST_ASSERT_TRUE(c.stream().streaming());
}

// The same with the Powerbeats Pro's timing (capabilities 1.1 s in, a SET
// answered 1.0 s after it) and one key press more: in the dip's silence the
// listener presses their VOL- (28, below our 38 still on its way). Their
// level goes back after ours, and our gain stays at silence until it is
// answered: 38 lands while nothing is heard, and only then does the gain
// ramp up. The stream runs on throughout.
void test_their_key_during_the_dip_keeps_the_silence_until_answered() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.setWanted(true, 10);
  c.tick(AbsVolumePolicy::kCapsWaitMs);
  startStream(c, io, 1600);
  const size_t cmds = io.cmds.size();
  const uint32_t caps = 1600 + 1100, sent = caps + AbsVolumePolicy::kDuckSettleMs;
  c.capabilities(true, caps);
  c.tick(sent);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(38, io.sent[0]);
  c.headsetChanged(28, sent + 400);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(2, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(28, io.sent[1]);
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  c.accepted(38, sent + 1000);  // lands in silence
  c.tick(sent + 1200);
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  c.accepted(28, sent + 1400);
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL_UINT8(vol::absToPercent(28), c.volume().percent());
  TEST_ASSERT_EQUAL(cmds, io.cmds.size());
  TEST_ASSERT_TRUE(c.stream().streaming());
}

// A late probe after a pause: sent at once, and a NEW start waits for the
// answer.
void test_late_probe_holds_a_new_start() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.setWanted(true, 10);
  c.tick(AbsVolumePolicy::kCapsWaitMs);
  startStream(c, io, 1600);
  c.setWanted(false, 5000);
  c.suspendNow(5000);
  c.mediaAck(Cmd::Suspend, true, 5100);
  c.audioState(false, 5110);
  c.capabilities(true, 6000);
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Snap, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  c.setWanted(true, 6100);
  TEST_ASSERT_EQUAL(Cmd::Suspend, io.last());  // held back
  c.accepted(38, 6200);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());  // the gate opened
}

// The stream stops while the late probe's command waits for the silence:
// what the headphones buffered before the dip may still play, so it waits
// on (the pause's SUSPEND is acked once transmission has stopped, not once
// they have played it all).
void test_late_probe_waits_when_the_stream_stops() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.setWanted(true, 10);
  c.tick(AbsVolumePolicy::kCapsWaitMs);
  startStream(c, io, 1600);
  c.capabilities(true, 5000);
  TEST_ASSERT_TRUE(io.sent.empty());
  c.setWanted(false, 5050);
  c.suspendNow(5050);
  TEST_ASSERT_TRUE(io.sent.empty());  // suspending: media may still flow
  c.mediaAck(Cmd::Suspend, true, 5100);
  c.audioState(false, 5110);
  TEST_ASSERT_TRUE(io.sent.empty());
  c.tick(5000 + AbsVolumePolicy::kDuckSettleMs - 1);
  TEST_ASSERT_TRUE(io.sent.empty());
  c.tick(5000 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
}

// The gain stage as BtSink drives it (PlayerA2dp::setGain).
struct RampIo : FakeIo {
  GainRamp ramp;
  void setGain(uint16_t q, Move m) override {
    FakeIo::setGain(q, m);
    switch (m) {
      case Move::Ramp: ramp.request(q, false); break;
      case Move::Snap: ramp.request(q, true); break;
      case Move::Lift: ramp.lift(q); break;
    }
  }
  uint16_t play(uint32_t frames) {
    std::vector<int16_t> lr(2 * frames, 10000);
    ramp.process(lr.data(), frames);
    return ramp.currentQ15();
  }
};

// A late probe while a resumed stream's START is out: no audio data flows,
// so the gain stage never sees the dip's target before the answer's
// replaces it. The silence snapped with the command keeps the resumed
// stream from fading back quickly to the level heard before the pause (at
// the headphones' new level): it rises from silence at the slow rate.
void test_late_probe_without_audio_data_keeps_the_silence() {
  RampIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  io.ramp.reset(softwareGain(30));
  c.setWanted(true, 10);
  c.tick(AbsVolumePolicy::kCapsWaitMs);
  startStream(c, io, 1600);
  io.ramp.restart();
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), io.play(4096));
  c.setWanted(false, 3000);
  c.suspendNow(3000);
  c.mediaAck(Cmd::Suspend, true, 3100);
  c.audioState(false, 3110);
  c.setWanted(true, 5000);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  c.mediaAck(Cmd::CheckReady, true, 5010);
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());  // START out: no data yet
  c.capabilities(true, 5020);
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  c.tick(5020 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  c.accepted(38, 5020 + AbsVolumePolicy::kDuckSettleMs + 100);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  c.mediaAck(Cmd::Start, true, 6000);
  c.audioState(true, 6010);
  io.ramp.restart();  // the resumed stream's first data
  // Where the quick fade back would have reached the old level: still near
  // silence (~-60 dB), rising at the slow rate.
  TEST_ASSERT_TRUE(io.play(GainRamp::kFadeFrames) < softwareGain(30) / 8);
}

// The headroom diagnostic reaches the gain stage by the ramp.
void test_headroom_setting() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 10);
  c.accepted(38, 20);
  c.setHeadroom(vol::dbToQ15(-6.0f), 30);
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::dbToQ15(-6.0f), io.gain);
  TEST_ASSERT_EQUAL_UINT16(vol::dbToQ15(-6.0f), c.volume().headroomQ15());
}

// The recheck's case, as BtControl runs it with the Powerbeats Pro's timing
// (a SET answered 1 s after it): playing at 50 % when they connect, the
// probe (64) goes out, and the listener presses their VOL- (20) before it
// lands. The handover at their level sends it back, and START waits for
// its ACCEPT: the probe lands (+14 dB on their side) while nothing plays,
// not ~400 ms into the first stream as before.
void test_their_report_during_the_probe_holds_the_start_until_answered() {
  FakeIo io;
  BtControl c(io, 50);
  c.setWanted(true, 0);
  c.linkUp(0);
  c.capabilities(true, 0);
  TEST_ASSERT_EQUAL(1, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(64, io.sent[0]);
  c.headsetChanged(20, 300);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Lift, io.moves.back());
  TEST_ASSERT_EQUAL(2, static_cast<int>(io.sent.size()));
  TEST_ASSERT_EQUAL_UINT8(20, io.sent[1]);
  TEST_ASSERT_TRUE(io.cmds.empty());  // held until it is answered
  c.tick(600);
  c.accepted(64, 1000);  // the probe lands
  c.tick(1100);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.accepted(20, 1300);  // their level back lands after it
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  startStream(c, io, 1350);
  TEST_ASSERT_EQUAL(2, static_cast<int>(io.sent.size()));

  // Headphones that never answer: START after kProbeTimeoutMs.
  FakeIo io2;
  BtControl d(io2, 50);
  d.setWanted(true, 0);
  d.linkUp(0);
  d.capabilities(true, 0);
  d.headsetChanged(20, 300);
  d.tick(300 + AbsVolumePolicy::kProbeTimeoutMs - 1);
  TEST_ASSERT_TRUE(io2.cmds.empty());
  d.tick(300 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io2.last());
}

// A probe that is never answered: the stream starts after the timeout and a
// short grace (in case they apply it late), at the software level.
void test_unanswered_probe_releases_the_stream() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 100);  // probe out
  c.setWanted(true, 200);
  TEST_ASSERT_TRUE(io.cmds.empty());
  const uint32_t timeout = 100 + AbsVolumePolicy::kProbeTimeoutMs;
  c.tick(timeout);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_TRUE(io.cmds.empty());  // the grace
  c.tick(timeout + AbsVolumePolicy::kProbeGraceMs - 1);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.tick(timeout + AbsVolumePolicy::kProbeGraceMs);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  startStream(c, io, 3200);
  // Its late answer: they apply ours, so the gain rises by the ramp only,
  // and the stream runs on.
  const size_t cmds = io.cmds.size();
  c.accepted(38, 4000);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL(cmds, io.cmds.size());
}

// An answer within the grace is as good as one in time: the stream waits
// no longer, and fades in straight to the handed-over level.
void test_answer_within_the_grace_lifts() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 100);
  c.setWanted(true, 200);
  c.tick(100 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.accepted(38, 2500);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Lift, io.moves.back());
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
}

// The narrow race of a pre-audio handover: the headphones start a stream
// themselves while our probe is out, and ESP-IDF runs the data callback
// (which restarts the gain stage for the new stream) before STARTED reaches
// BtAppT. The ACCEPT that follows is still taken as a pre-audio handover (a
// lift), but its restart is used up: the gain stage drops the lift and ramps
// to the headroom at the up rate from the software level, with no step.
void test_lift_after_a_stream_the_headphones_started_only_ramps() {
  RampIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  io.ramp.reset(softwareGain(30));
  c.capabilities(true, 100);  // probe out
  io.ramp.restart();          // their stream's first data, unknown to BtAppT
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), io.play(GainRamp::kFadeFrames));
  c.accepted(38, 200);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Lift, io.moves.back());
  constexpr uint32_t kWindow = 441;  // 10 ms
  uint16_t last = softwareGain(30);
  uint32_t windows = 0;
  while (last != vol::kHeadroomQ15 && windows < 1000) {
    const uint16_t now = io.play(kWindow);
    TEST_ASSERT_TRUE(now >= last);
    TEST_ASSERT_TRUE(vol::q15ToDb(now) - vol::q15ToDb(last) <= 0.22f);  // <= ~20 dB/s, +rounding
    last = now;
    ++windows;
  }
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, last);  // ends at the handover level
  const float rise = vol::q15ToDb(vol::kHeadroomQ15) - vol::q15ToDb(softwareGain(30));
  TEST_ASSERT_TRUE(windows >= static_cast<uint32_t>(rise / 20.5f * 100.0f));
  // STARTED reaches BtAppT: nothing moves.
  const size_t moves = io.moves.size();
  c.audioState(true, 2000);
  TEST_ASSERT_EQUAL(moves, io.moves.size());
}

// AVRCP drops on a link that has played in Absolute mode and comes back: the
// headphones kept their level while our gain came down, so the return gets
// a late probe of its own (dip, command once silent, ramp) instead of
// staying stacked. The stream runs on throughout.
void test_avrcp_back_on_a_link_that_has_played() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 10);
  c.accepted(38, 20);
  c.setWanted(true, 30);
  startStream(c, io, 40);
  const size_t cmds = io.cmds.size();
  c.avrcpDown(5000);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(30), io.gain);
  const size_t sent = io.sent.size();
  c.capabilities(true, 8000);  // AVRCP back
  TEST_ASSERT_TRUE(c.avrcpConnected());
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
  TEST_ASSERT_TRUE(c.volume().ducked());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(0, io.gain);
  TEST_ASSERT_EQUAL(sent, io.sent.size());
  c.tick(8000 + AbsVolumePolicy::kDuckSettleMs);
  TEST_ASSERT_EQUAL(sent + 1, io.sent.size());
  TEST_ASSERT_EQUAL_UINT8(38, io.sent.back());
  c.accepted(38, 8000 + AbsVolumePolicy::kDuckSettleMs + 150);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL(cmds, io.cmds.size());
  TEST_ASSERT_TRUE(c.stream().streaming());
}

// Sends only reach the headphones while AVRCP is connected.
void test_no_absolute_volume_without_avrcp() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 100);
  c.accepted(38, 200);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  c.avrcpDown(300);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());  // down, quickly
  const size_t sent = io.sent.size();
  c.stepVolume(10, 400);
  TEST_ASSERT_EQUAL(sent, io.sent.size());
}

// A pause from the headphones' key suspends at once.
void test_suspend_now_after_a_pause() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(false, 10);
  c.setWanted(true, 20);
  startStream(c, io, 30);
  c.setWanted(false, 1000);
  TEST_ASSERT_EQUAL(Cmd::Start, io.last());  // no suspend yet
  c.suspendNow(1000);
  TEST_ASSERT_EQUAL(Cmd::Suspend, io.last());
}

// A command the stack won't queue: retried on the schedule, not in a loop.
void test_media_ctrl_failure_waits_for_the_schedule() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(false, 10);
  io.failNext = true;
  c.setWanted(true, 20);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.tick(500);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.tick(20 + StreamControl::kFirstRetryMs);
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
}

// A link that comes and goes: every link starts at the safe level and gets
// its own handover.
void test_each_link_starts_safe() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 10);
  c.accepted(38, 20);
  c.setVolume(90, 30);  // on the headphones: 90 %
  c.linkDown(40);
  TEST_ASSERT_EQUAL(Move::Snap, io.moves.back());
  c.linkUp(50);  // AVRCP stayed: probes at once, capped
  TEST_ASSERT_EQUAL_UINT8(AbsVolumePolicy::kMaxLinkUpPercent, c.volume().percent());
  TEST_ASSERT_EQUAL_UINT8(vol::percentToAbs(AbsVolumePolicy::kMaxLinkUpPercent), io.sent.back());
  TEST_ASSERT_EQUAL_UINT16(softwareGain(AbsVolumePolicy::kMaxLinkUpPercent), io.gain);
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
  c.linkUp(60);  // repeated: nothing
  TEST_ASSERT_EQUAL(Mode::Probing, c.volume().mode());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_link_while_playing_waits_for_the_probe_then_lifts);
  RUN_TEST(test_capabilities_between_start_and_started_dip_first);
  RUN_TEST(test_late_capabilities_while_started_leave_the_stream_alone);
  RUN_TEST(test_their_key_during_the_dip_keeps_the_silence_until_answered);
  RUN_TEST(test_late_probe_holds_a_new_start);
  RUN_TEST(test_late_probe_waits_when_the_stream_stops);
  RUN_TEST(test_late_probe_without_audio_data_keeps_the_silence);
  RUN_TEST(test_headroom_setting);
  RUN_TEST(test_their_report_during_the_probe_holds_the_start_until_answered);
  RUN_TEST(test_unanswered_probe_releases_the_stream);
  RUN_TEST(test_answer_within_the_grace_lifts);
  RUN_TEST(test_lift_after_a_stream_the_headphones_started_only_ramps);
  RUN_TEST(test_avrcp_back_on_a_link_that_has_played);
  RUN_TEST(test_no_absolute_volume_without_avrcp);
  RUN_TEST(test_suspend_now_after_a_pause);
  RUN_TEST(test_media_ctrl_failure_waits_for_the_schedule);
  RUN_TEST(test_each_link_starts_safe);
  return UNITY_END();
}
