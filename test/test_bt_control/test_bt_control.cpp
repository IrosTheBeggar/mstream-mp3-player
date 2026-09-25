// Host unit tests for BtControl: the media stream and the headphones' volume
// together, as PlayerA2dp drives them from the Bluetooth stack's events.
// Run: pio test -e native
#include <unity.h>

#include <cstdint>
#include <vector>

#include "BtControl.h"
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
// they arrive between START and STARTED. No probe: media may already flow.
void test_capabilities_between_start_and_started_do_not_probe() {
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
  c.mediaAck(Cmd::Start, true, 1620);
  c.capabilities(true, 1630);  // acknowledged, STARTED not reported yet
  TEST_ASSERT_TRUE(io.sent.empty());
  c.audioState(true, 1640);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  // Their buttons now: nothing gets louder, the UI keeps our volume.
  const uint16_t gain = io.gain;
  c.headsetChanged(102, 5000);
  TEST_ASSERT_EQUAL_UINT16(gain, io.gain);
  TEST_ASSERT_EQUAL_UINT8(30, c.volume().percent());
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  // Not at the next pause either.
  c.setWanted(false, 6000);
  c.suspendNow(6000);
  c.mediaAck(Cmd::Suspend, true, 6100);
  c.audioState(false, 6110);
  TEST_ASSERT_TRUE(io.sent.empty());
  TEST_ASSERT_EQUAL_UINT16(gain, io.gain);
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
  // Its late answer changes nothing now.
  const uint16_t gain = io.gain;
  const size_t moves = io.moves.size();
  c.accepted(38, 3000);
  TEST_ASSERT_EQUAL(Mode::Software, c.volume().mode());
  TEST_ASSERT_EQUAL_UINT16(gain, io.gain);
  TEST_ASSERT_EQUAL(moves, io.moves.size());
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
  RUN_TEST(test_capabilities_between_start_and_started_do_not_probe);
  RUN_TEST(test_unanswered_probe_releases_the_stream);
  RUN_TEST(test_no_absolute_volume_without_avrcp);
  RUN_TEST(test_suspend_now_after_a_pause);
  RUN_TEST(test_media_ctrl_failure_waits_for_the_schedule);
  RUN_TEST(test_each_link_starts_safe);
  return UNITY_END();
}
