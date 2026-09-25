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

// A probe that is never answered: the stream starts after the timeout, at
// the software level.
void test_unanswered_probe_releases_the_stream() {
  FakeIo io;
  BtControl c(io, 30);
  c.linkUp(0);
  c.capabilities(true, 100);  // probe out
  c.setWanted(true, 200);
  TEST_ASSERT_TRUE(io.cmds.empty());
  c.tick(100 + AbsVolumePolicy::kProbeTimeoutMs);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_EQUAL(Cmd::CheckReady, io.last());
  startStream(c, io, 2200);
  // Its late answer: they apply ours, so the gain rises by the ramp only,
  // and the stream runs on.
  const size_t cmds = io.cmds.size();
  c.accepted(38, 3000);
  TEST_ASSERT_EQUAL(Mode::Absolute, c.volume().mode());
  TEST_ASSERT_EQUAL(Move::Ramp, io.moves.back());
  TEST_ASSERT_EQUAL_UINT16(vol::kHeadroomQ15, io.gain);
  TEST_ASSERT_EQUAL(cmds, io.cmds.size());
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
  RUN_TEST(test_late_probe_holds_a_new_start);
  RUN_TEST(test_late_probe_waits_when_the_stream_stops);
  RUN_TEST(test_late_probe_without_audio_data_keeps_the_silence);
  RUN_TEST(test_headroom_setting);
  RUN_TEST(test_unanswered_probe_releases_the_stream);
  RUN_TEST(test_no_absolute_volume_without_avrcp);
  RUN_TEST(test_suspend_now_after_a_pause);
  RUN_TEST(test_media_ctrl_failure_waits_for_the_schedule);
  RUN_TEST(test_each_link_starts_safe);
  return UNITY_END();
}
