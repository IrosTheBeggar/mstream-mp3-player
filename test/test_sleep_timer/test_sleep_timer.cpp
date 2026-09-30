// Host tests for the sleep timer (SleepTimer, docs/ENERGY.md section 3) with
// the real PlaybackController and FadeStage, carried out the way main.cpp's
// stepSleep() does it. Run: pio test -e native
#include <unity.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "FadeStage.h"
#include "HeadsetKeys.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "SleepTimer.h"
#include "TrackCatalog.h"
#include "UiText.h"
#include "hal/IAudioBackend.h"

using Phase = SleepTimer::Phase;
using Choice = SleepTimer::Choice;

namespace {

class FakeAudio : public IAudioBackend {
public:
  bool playing = false, paused = false, finishedFlag = false;
  uint32_t position = 0;
  int plays = 0;
  bool play(const std::string&, uint32_t, uint32_t) override {
    playing = true;
    paused = false;
    finishedFlag = false;
    position = 0;
    ++plays;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override {
    playing = false;
    paused = false;
    position = 0;
  }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return position; }
  bool finished() const override { return finishedFlag; }
};

struct Hold : PlaybackController::Hold {
  bool held = false;
  bool holdPlay() const override { return held; }
};

// What main.cpp does with the timer's answers, in its order, logged.
enum class Did { Pause, Restore, ScreenOff, Release, FadeStarted, Expired };

struct Rig {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  QueueModel queue;
  FakeAudio audio;
  Hold hold;
  PlaybackController player{audio, queue, catalog};
  SleepTimer timer;
  FadeStage fade;
  uint32_t now = 1000;
  uint32_t durationMs = 200000;  // every track's length (0: unknown)
  bool lastOfAlbum = false;
  std::vector<std::pair<Did, uint32_t>> did;
  std::vector<uint16_t> targets;  // the fade's target, every pass
  size_t restoredAt = SIZE_MAX;   // the pass (its index in targets) that restored it

  explicit Rig(uint32_t tracks = 3) {
    const char* files[] = {"/music/a.mp3", "/music/b.mp3", "/music/c.mp3"};
    index.begin("/music");
    for (uint32_t i = 0; i < tracks; ++i) index.addFile(files[i]);
    index.finish();
    const uint32_t ids[] = {0, 1, 2};
    queue.assign(ids, tracks, 0);
    player.setHold(&hold);
  }

  bool saw(Did d) const {
    for (const auto& x : did) {
      if (x.first == d) return true;
    }
    return false;
  }
  int at(Did d) const {
    for (size_t i = 0; i < did.size(); ++i) {
      if (did[i].first == d) return static_cast<int>(i);
    }
    return -1;
  }

  // One loop pass: the timer, then the player (as main.cpp's loop).
  SleepTimer::Out pass() {
    SleepTimer::In in;
    in.nowMs = now;
    in.play = player.state();
    in.boundaryStops = player.timerStops();
    if (queue.current() >= 0) {
      in.positionMs = audio.position;
      in.durationMs = durationMs;
      in.lastOfQueue = static_cast<uint32_t>(queue.current()) + 1 >= queue.size();
      in.lastOfAlbum = lastOfAlbum || in.lastOfQueue;
    }
    const SleepTimer::Out o = timer.update(in);
    fade.setTarget(o.fadeQ15);
    targets.push_back(o.fadeQ15);
    player.setPauseAfterTrack(o.pauseAfterTrack);
    if (o.expired) did.push_back({Did::Expired, now});
    if (o.fadeStarted) did.push_back({Did::FadeStarted, now});
    if (o.pauseNow) {
      player.pauseByTimer();
      did.push_back({Did::Pause, now});
    }
    if (o.restore) {
      // Only while nothing is heard: the pause is confirmed.
      TEST_ASSERT_TRUE(player.state() != PlayState::Playing);
      fade.restore();
      did.push_back({Did::Restore, now});
      restoredAt = targets.size() - 1;
    }
    if (o.screenOff) did.push_back({Did::ScreenOff, now});
    if (o.release) did.push_back({Did::Release, now});
    player.update(now);
    return o;
  }

  // Passes every `stepMs` for `ms`; the track position moves while playing.
  void run(uint32_t ms, uint32_t stepMs = 20) {
    for (uint32_t t = 0; t < ms; t += stepMs) {
      now += stepMs;
      if (player.state() == PlayState::Playing) audio.position += stepMs;
      pass();
    }
  }

  // The track ends (the ring drained): the backend says finished.
  void endTrack() {
    audio.position = durationMs;
    audio.finishedFlag = true;
    now += 20;
    pass();
    audio.finishedFlag = false;
  }
};

float db(uint16_t q15) { return q15 ? 20.0f * std::log10(q15 / 32768.0f) : -120.0f; }

}  // namespace

void setUp() {}
void tearDown() {}

// ---- each choice's expiry ----

// A timed choice playing: 30 s of fade after it expires (linear in dB to
// -40, then 0), the pause where it ends, then (once silent) the factor back
// to 1, the screen off, and 5 min later the headphones.
void test_timed_expiry_fades_pauses_restores_then_releases() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(60000, r.now);
  r.run(59000);
  TEST_ASSERT_EQUAL(Phase::Counting, r.timer.phase());
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
  r.run(1100);
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  TEST_ASSERT_TRUE(r.saw(Did::FadeStarted));
  // Halfway through the fade: -20 dB.
  r.run(15000);
  TEST_ASSERT_FLOAT_WITHIN(1.0f, -20.0f, db(r.fade.target()));
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  r.run(15200);
  TEST_ASSERT_TRUE(r.saw(Did::Pause));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  // Paused where the fade ended, at 0 (until the pause is confirmed).
  TEST_ASSERT_EQUAL(Phase::Ending, r.timer.phase());
  TEST_ASSERT_EQUAL_UINT16(0, r.fade.target());
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::Restore));
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
  TEST_ASSERT_TRUE(r.fade.atUnity());
  TEST_ASSERT_FALSE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL(Phase::Ended, r.timer.phase());
  TEST_ASSERT_FALSE(r.timer.running());
  // The pause stays: nothing resumes by itself.
  r.run(SleepTimer::kReleaseMs - 2000, 1000);
  TEST_ASSERT_FALSE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  r.run(3000, 1000);
  TEST_ASSERT_TRUE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL(Phase::Off, r.timer.phase());
}

// The order of release: the pause confirmed, then the factor back to 1,
// then the screen off, then (5 min later) the headphones.
void test_release_order() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(40000);
  r.run(SleepTimer::kReleaseMs + 1000, 500);
  const int pause = r.at(Did::Pause), restore = r.at(Did::Restore), off = r.at(Did::ScreenOff),
            release = r.at(Did::Release);
  TEST_ASSERT_TRUE(pause >= 0 && restore > pause && off > restore && release > off);
  // The factor went back only once the pause had settled (silent).
  TEST_ASSERT_TRUE(r.did[restore].second - r.did[pause].second >= SleepTimer::kSettleMs);
  TEST_ASSERT_TRUE(r.did[release].second - r.did[off].second >= SleepTimer::kReleaseMs);
}

// Expiring while paused (the listener's pause, or a Bluetooth drop during
// the countdown): no fade; the pause becomes the timer's, then the rest.
void test_expiry_while_paused_skips_the_fade() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(30000, r.now);
  r.run(10000);
  r.player.togglePlayPause();  // a pause during the countdown: it runs on
  TEST_ASSERT_EQUAL(Phase::Counting, r.timer.phase());
  r.run(21000);
  TEST_ASSERT_TRUE(r.saw(Did::Expired));
  TEST_ASSERT_FALSE(r.saw(Did::FadeStarted));
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  for (uint16_t t : r.targets) TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, t);
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
}

// Expiring during a PlayGate wait (the headphones not there): the wait
// ends paused, then the rest; nothing plays anywhere.
void test_expiry_during_a_wait_ends_it_paused() {
  Rig r;
  r.hold.held = true;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  r.timer.setTimed(5000, r.now);
  r.run(5100);
  TEST_ASSERT_TRUE(r.saw(Did::Pause));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_EQUAL(0, r.audio.plays);
  TEST_ASSERT_FALSE(r.saw(Did::FadeStarted));
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
}

// End of track: no fade until the last 10 s, which fade on the curve; at
// the boundary the player moves to the next entry and stays paused at 0:00.
void test_end_of_track_fades_the_last_10_s_and_pauses_on_the_next_entry() {
  Rig r;
  r.durationMs = 100000;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfTrack);
  r.run(85000);
  TEST_ASSERT_EQUAL(Phase::Armed, r.timer.phase());
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
  r.run(10000);  // 95 s: 5 s left
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  TEST_ASSERT_FLOAT_WITHIN(1.5f, -20.0f, db(r.fade.target()));
  r.run(4980);
  TEST_ASSERT_TRUE(db(r.fade.target()) < -38.0f);
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::Expired));  // (seen the pass after the boundary)
  TEST_ASSERT_FALSE(r.saw(Did::Pause));   // the player paused itself at the boundary
  TEST_ASSERT_TRUE(r.saw(Did::Restore));
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
  // A later play starts the next track from its beginning.
  const int plays = r.audio.plays;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(plays + 1, r.audio.plays);
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
}

// A track whose length isn't known: no fade, the pause at the boundary.
void test_end_of_track_of_unknown_length_pauses_without_a_fade() {
  Rig r;
  r.durationMs = 0;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfTrack);
  r.run(300000, 100);
  TEST_ASSERT_EQUAL(Phase::Armed, r.timer.phase());
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_FALSE(r.saw(Did::FadeStarted));
  for (uint16_t t : r.targets) TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, t);
}

// End of album: tracks of the same album play on; the one before another
// album pauses.
void test_end_of_album_pauses_only_at_the_album_end() {
  Rig r;
  r.durationMs = 60000;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfAlbum);
  r.lastOfAlbum = false;
  r.run(1000);
  TEST_ASSERT_FALSE(r.player.pauseAfterTrack());
  r.run(55000);
  TEST_ASSERT_EQUAL(Phase::Armed, r.timer.phase());  // no fade: the album goes on
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  r.lastOfAlbum = true;  // entry 2 is another album's
  r.run(1000);
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  r.run(58000);
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
}

// End of queue: only on the last entry; without repeat it is the natural stop.
void test_end_of_queue_without_repeat_stops_at_the_end() {
  Rig r;
  r.durationMs = 30000;
  r.player.setRepeat(false);
  r.player.play(1);
  r.timer.setEnd(Choice::EndOfQueue);
  r.run(1000);
  TEST_ASSERT_FALSE(r.player.pauseAfterTrack());
  r.endTrack();
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  r.run(1000);
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Stopped, r.player.state());
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::Expired));
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
}

// With repeat, End of queue pauses on the first entry, at 0:00.
void test_end_of_queue_with_repeat_pauses_on_the_first_entry() {
  Rig r;
  r.durationMs = 30000;
  r.player.play(2);
  r.timer.setEnd(Choice::EndOfQueue);
  r.run(1000);
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
}

// ---- during the fade ----

// A skip during a timed fade: the timer runs on and the fade keeps its
// level (the new track starts faded, never louder).
void test_skip_during_the_fade_keeps_its_level() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(11000);  // 10 s into the fade
  const uint16_t before = r.fade.target();
  TEST_ASSERT_TRUE(before < SleepTimer::kUnity);
  r.player.next();
  r.run(20);
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  TEST_ASSERT_TRUE(r.fade.target() <= before);
  r.run(21000);
  TEST_ASSERT_TRUE(r.saw(Did::Pause));
}

// End of track: a skip during its fade holds the level on the new track,
// which the timer then applies to.
void test_skip_during_a_track_fade_holds_it_for_the_new_track() {
  Rig r;
  r.durationMs = 60000;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfTrack);
  r.run(55000);
  const uint16_t held = r.fade.target();
  TEST_ASSERT_TRUE(held < SleepTimer::kUnity);
  r.player.next();  // a deliberate act: the new track is "this track" now
  r.run(20000);
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  for (size_t i = 1; i < r.targets.size(); ++i) TEST_ASSERT_TRUE(r.targets[i] <= r.targets[i - 1]);
  r.endTrack();
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
}

// What holds a lit screen lit (fadeCountingDown()): a timed fade for its
// 30 s; a track's fade in the boundary track's last 10 s. A skip during a
// track's fade keeps the fade (and its toast) until the new track's own
// last 10 s, or the album's end: the screen isn't held meanwhile, so it
// times out as ever (it could be minutes, in a pocket).
void test_only_a_fade_that_counts_down_holds_the_screen() {
  {
    Rig r;
    r.player.play(0);
    r.timer.setTimed(1000, r.now);
    r.run(1000);
    TEST_ASSERT_TRUE(r.timer.fading());
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
    r.run(15000);
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
    r.player.next();  // a timed fade runs on through a skip: still 30 s at most
    r.run(20);
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
  }
  {
    // End of track: a skip in its last 10 s, to a 60 s track.
    Rig r;
    r.durationMs = 60000;
    r.player.play(0);
    r.timer.setEnd(Choice::EndOfTrack);
    r.run(49000);
    TEST_ASSERT_FALSE(r.timer.fading());
    TEST_ASSERT_FALSE(r.timer.fadeCountingDown());
    r.run(3000);
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
    r.player.next();
    r.run(20);
    TEST_ASSERT_TRUE(r.timer.fading());  // the toast stays, the level held
    TEST_ASSERT_FALSE(r.timer.fadeCountingDown());
    r.run(40000);
    TEST_ASSERT_TRUE(r.timer.fading());
    TEST_ASSERT_FALSE(r.timer.fadeCountingDown());
    r.run(12000);  // the new track's own last 10 s
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
  }
  {
    // End of album: a skip from the album's last track to the next album.
    Rig r;
    r.durationMs = 60000;
    r.player.play(0);
    r.timer.setEnd(Choice::EndOfAlbum);
    r.lastOfAlbum = true;
    r.run(55000);
    TEST_ASSERT_TRUE(r.timer.fadeCountingDown());
    r.lastOfAlbum = false;
    r.player.next();
    r.run(58000);  // the whole next track (even its last 10 s: not the album's end)
    TEST_ASSERT_TRUE(r.timer.fading());
    TEST_ASSERT_FALSE(r.timer.fadeCountingDown());
  }
}

// A pause during the fade (the listener's, a drop, a move to the speaker)
// finishes the timer at once.
void test_a_pause_during_the_fade_finishes_it() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(6000);
  r.player.togglePlayPause();
  r.run(20);
  TEST_ASSERT_TRUE(r.saw(Did::Expired));
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_EQUAL(Phase::Ending, r.timer.phase());
  r.run(1000);
  TEST_ASSERT_TRUE(r.saw(Did::Restore));
  TEST_ASSERT_TRUE(r.saw(Did::ScreenOff));
}

// The factor never rises by itself and never exceeds 1.0, whatever the
// listener does that isn't +10 min, Turn off or a new choice.
void test_the_fade_never_rises_by_itself() {
  std::mt19937 rng(11);
  for (int round = 0; round < 40; ++round) {
    Rig r;
    r.durationMs = (rng() % 3) ? 40000 + rng() % 60000 : 0;
    r.player.play(0);
    const int kind = static_cast<int>(rng() % 4);
    if (kind == 0) {
      r.timer.setTimed(1000 + rng() % 20000, r.now);
    } else {
      r.timer.setEnd(kind == 1 ? Choice::EndOfTrack : kind == 2 ? Choice::EndOfAlbum : Choice::EndOfQueue);
    }
    r.targets.clear();
    for (int i = 0; i < 3000; ++i) {
      switch (rng() % 400) {
        case 0: r.player.next(); break;
        case 1: r.player.prev(); break;
        case 2:
          // The listener's pause, or a play before the fade (a play after
          // the timer paused is the Core2's play: the factor may rise then).
          if (r.player.state() == PlayState::Playing || r.timer.phase() == Phase::Counting ||
              r.timer.phase() == Phase::Armed) {
            r.player.togglePlayPause();
          }
          break;
        case 3: r.lastOfAlbum = !r.lastOfAlbum; break;
        case 4:
          if (r.durationMs) r.endTrack();
          break;
        default: break;
      }
      r.run(20);
      if (r.saw(Did::Restore)) break;  // back to 1.0 only once silent (checked in pass())
    }
    for (size_t i = 0; i < r.targets.size(); ++i) {
      TEST_ASSERT_TRUE(r.targets[i] <= SleepTimer::kUnity);
      if (i > 0 && i != r.restoredAt) {
        TEST_ASSERT_TRUE_MESSAGE(r.targets[i] <= r.targets[i - 1], "the fade rose by itself");
      }
    }
  }
}

// +10 min during the fade: the factor comes back up at the slow rate
// (never in one step), and the timer counts 10 min again.
void test_extend_during_the_fade_ramps_up_slowly() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(16000);  // 15 s into the fade: -20 dB
  // The outputs follow the target (a block per pass).
  std::vector<int16_t> block(2 * 882, 20000);
  for (int i = 0; i < 50; ++i) {
    r.fade.process(block.data(), 882);
    std::fill(block.begin(), block.end(), 20000);
  }
  const uint16_t low = r.fade.levelQ15();
  TEST_ASSERT_TRUE(db(low) < -18.0f);
  TEST_ASSERT_TRUE(r.timer.extend(r.now, 0));
  r.run(20);
  TEST_ASSERT_EQUAL(Phase::Counting, r.timer.phase());
  TEST_ASSERT_UINT32_WITHIN(100, SleepTimer::kExtendMs, r.timer.msLeft(r.now));
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
  // 1 s of audio (50 blocks of 20 ms) rises by at most ~20 dB.
  for (int i = 0; i < 50; ++i) r.fade.process(block.data(), 882);
  const float rose = db(r.fade.levelQ15()) - db(low);
  TEST_ASSERT_TRUE(rose > 5.0f);
  TEST_ASSERT_TRUE(rose <= 20.5f);
}

// Turn off during the fade: the same slow rise; the timer is off.
void test_turn_off_during_the_fade_ramps_up_slowly() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(25000);
  std::vector<int16_t> block(2 * 882, 20000);
  for (int i = 0; i < 60; ++i) r.fade.process(block.data(), 882);
  const float low = db(r.fade.levelQ15());
  r.timer.cancel();
  r.run(20);
  TEST_ASSERT_EQUAL(Phase::Off, r.timer.phase());
  TEST_ASSERT_FALSE(r.player.pauseAfterTrack());
  for (int i = 0; i < 25; ++i) r.fade.process(block.data(), 882);  // 0.5 s
  const float rose = db(r.fade.levelQ15()) - low;
  TEST_ASSERT_TRUE(rose > 2.0f && rose <= 10.5f);
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
}

// +10 min adds to what is left; another duration restarts from now.
void test_extend_and_restart_during_the_countdown() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(15 * 60000, r.now);
  r.run(5 * 60000, 1000);
  TEST_ASSERT_UINT32_WITHIN(1000, 10 * 60000, r.timer.msLeft(r.now));
  r.timer.extend(r.now, 0);
  TEST_ASSERT_UINT32_WITHIN(1000, 20 * 60000, r.timer.msLeft(r.now));
  r.timer.setTimed(30 * 60000, r.now);
  TEST_ASSERT_EQUAL_UINT32(30 * 60000, r.timer.msLeft(r.now));
  TEST_ASSERT_EQUAL_INT(1, r.timer.timedIndex());
  // An end-of choice made timed: what the track has left, plus 10 min.
  r.timer.setEnd(Choice::EndOfTrack);
  TEST_ASSERT_TRUE(r.timer.extend(r.now, 42000));
  TEST_ASSERT_EQUAL_UINT32(42000 + SleepTimer::kExtendMs, r.timer.msLeft(r.now));
  TEST_ASSERT_EQUAL(Choice::Timed, r.timer.choice());
  r.timer.cancel();
  TEST_ASSERT_FALSE(r.timer.extend(r.now, 0));
}

// The Core2's play before the pause settles: nothing more happens (no
// screen off, no release), and the fade comes back up slowly.
void test_a_play_before_the_pause_settles_ends_it() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(31200);
  TEST_ASSERT_TRUE(r.saw(Did::Pause));
  r.player.togglePlayPause();  // the Core2's own play
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
  r.run(SleepTimer::kReleaseMs + 1000, 500);
  TEST_ASSERT_FALSE(r.saw(Did::Restore));
  TEST_ASSERT_FALSE(r.saw(Did::ScreenOff));
  TEST_ASSERT_FALSE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
}

// A play in the 5 min after the pause: the headphones aren't let go.
void test_a_play_after_the_pause_cancels_the_release() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(33000);
  TEST_ASSERT_EQUAL(Phase::Ended, r.timer.phase());
  r.run(60000, 1000);
  r.player.togglePlayPause();
  r.run(SleepTimer::kReleaseMs, 1000);
  TEST_ASSERT_FALSE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL(Phase::Off, r.timer.phase());
}

// After the timer's pause, the headphones' Play (in-ear detection as a
// sleeper turns over) does nothing; the Core2's play resumes.
void test_headphone_play_is_ignored_after_a_timer_pause() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(1000, r.now);
  r.run(33000);
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL(HeadsetKeys::Action::Ignore, HeadsetKeys::apply(r.player, HeadsetKeys::Key::Play));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.audio.paused);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  // A pause of the listener's own later: their Play works again.
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(HeadsetKeys::Action::Resume, HeadsetKeys::apply(r.player, HeadsetKeys::Key::Play));
}

// A drop during the countdown (paused, the output still Bluetooth) runs
// steps 2-5 at expiry, the pause kept. (That the timer never moves the
// output is by construction: SleepTimer::Out has nothing that could; the
// release's side is main.cpp's releaseHeadphones(), and the drop it causes
// is BtSession::onDisconnected()'s, tested in test_ui_output.)
void test_a_drop_during_the_countdown_ends_paused_and_releases() {
  Rig r;
  r.player.play(0);
  r.timer.setTimed(20000, r.now);
  r.run(5000);
  r.player.togglePlayPause();  // the drop's pause (main.cpp's pauseIfPlaying)
  r.run(20000);
  TEST_ASSERT_FALSE(r.saw(Did::FadeStarted));  // expired paused: no fade
  r.run(SleepTimer::kReleaseMs + 1000, 1000);
  TEST_ASSERT_TRUE(r.saw(Did::Release));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
}

// +10 min never leaves less time than before. End of album / queue before
// the album's or queue's last track: what is left isn't known (more than
// this track), so it is refused and the choice stays; on the last track it
// becomes what the track has left plus 10 min. A track of unknown length:
// refused too.
void test_extend_never_shortens_an_end_of_choice() {
  for (const Choice c : {Choice::EndOfAlbum, Choice::EndOfQueue}) {
    Rig r;
    r.durationMs = 300000;
    r.player.play(0);
    r.timer.setEnd(c);
    TEST_ASSERT_FALSE(r.timer.canExtend());  // not known until the queue is read
    r.run(1000);
    TEST_ASSERT_FALSE(r.timer.canExtend());
    TEST_ASSERT_FALSE(r.timer.extend(r.now, 299000));
    TEST_ASSERT_EQUAL(c, r.timer.choice());
    TEST_ASSERT_EQUAL(Phase::Armed, r.timer.phase());
    // The album's (and the queue's) last track: what is left is the track's.
    r.player.play(2);
    r.run(100000);
    TEST_ASSERT_TRUE(r.timer.canExtend());
    const uint32_t left = r.durationMs - r.audio.position;
    TEST_ASSERT_TRUE(r.timer.extend(r.now, left));
    TEST_ASSERT_EQUAL(Choice::Timed, r.timer.choice());
    TEST_ASSERT_EQUAL_UINT32(left + SleepTimer::kExtendMs, r.timer.msLeft(r.now));
  }
  // End of track of unknown length: nothing known to add to.
  Rig r;
  r.durationMs = 0;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfTrack);
  r.run(1000);
  TEST_ASSERT_FALSE(r.timer.canExtend());
  TEST_ASSERT_FALSE(r.timer.extend(r.now, 0));
  TEST_ASSERT_EQUAL(Choice::EndOfTrack, r.timer.choice());
}

// End of album: a skip during the last track's fade to a track that isn't
// the album's last. +10 min brings the level back up (slowly) and the
// timer waits for the album's end again: not the skipped-to track's end.
void test_extend_after_a_skip_away_from_the_boundary_rearms() {
  Rig r;
  r.durationMs = 60000;
  r.player.play(0);
  r.timer.setEnd(Choice::EndOfAlbum);
  r.lastOfAlbum = true;
  r.run(55000);
  TEST_ASSERT_EQUAL(Phase::Fading, r.timer.phase());
  r.lastOfAlbum = false;  // entry 1 goes on in the same album
  r.player.next();
  r.run(20);
  TEST_ASSERT_TRUE(r.timer.canExtend());
  TEST_ASSERT_TRUE(r.timer.extend(r.now, 60000));
  TEST_ASSERT_EQUAL(Phase::Armed, r.timer.phase());
  TEST_ASSERT_EQUAL(Choice::EndOfAlbum, r.timer.choice());
  r.run(3000);
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, r.fade.target());
  TEST_ASSERT_FALSE(r.player.pauseAfterTrack());
}

// ---- a skip's stale position (main.cpp feeds EntryStart) ----

// The new entry's position and length count only once the backend has
// started it: its start count moved, or the position went back (or it was
// already under 1 s when the entry changed).
void test_entry_start_waits_for_the_backend() {
  EntryStart e;
  TEST_ASSERT_TRUE(e.update(10, 5, 200));  // at a track's start
  TEST_ASSERT_TRUE(e.update(10, 5, 150000));
  // A skip near the end: the backend still reports the old track.
  TEST_ASSERT_FALSE(e.update(11, 5, 295000));
  TEST_ASSERT_FALSE(e.update(11, 5, 295040));
  TEST_ASSERT_FALSE(e.started());
  TEST_ASSERT_TRUE(e.update(11, 6, 295060));  // it started the new one
  TEST_ASSERT_TRUE(e.update(11, 6, 20));
  // Another skip: the position going back also says so.
  TEST_ASSERT_FALSE(e.update(12, 6, 100000));
  TEST_ASSERT_TRUE(e.update(12, 6, 0));
  TEST_ASSERT_TRUE(e.update(12, 6, 1500));  // and it stays started
  // An entry that changes while the last one had barely begun.
  TEST_ASSERT_TRUE(e.update(13, 6, 400));
}

// End of track armed, 15 s before the end; a skip. The backend reports
// the old track's last seconds for a while: with EntryStart the timer sees
// an unknown length (no fade) until the new track has started, then its
// whole length (no fade either). Without it, the timer would fade the new
// track at once, and hold it low.
void test_end_of_track_after_a_skip_near_the_end_does_not_fade_the_new_track() {
  const uint32_t len = 300000;
  auto feed = [len](SleepTimer& t, EntryStart* guard, uint32_t now, uint32_t key, uint32_t seq, uint32_t pos) {
    SleepTimer::In in;
    in.nowMs = now;
    in.play = PlayState::Playing;
    in.positionMs = pos;
    const bool started = guard ? guard->update(key, seq, pos) : true;
    in.durationMs = started ? len : 0;
    return t.update(in);
  };
  for (int withGuard = 1; withGuard >= 0; --withGuard) {
    SleepTimer t;
    EntryStart guard;
    EntryStart* g = withGuard ? &guard : nullptr;
    t.setEnd(Choice::EndOfTrack);
    uint32_t now = 1000, pos = len - 15000;
    feed(t, g, now, 1, 7, pos);
    TEST_ASSERT_EQUAL(Phase::Armed, t.phase());
    // The skip: entry 2 now, the backend still at the old track's end.
    uint16_t lowest = SleepTimer::kUnity;
    for (int i = 0; i < 25; ++i) {
      now += 20;
      pos += 400;  // (fast: it reaches the last 10 s)
      lowest = std::min(lowest, feed(t, g, now, 2, 7, pos).fadeQ15);
    }
    if (!withGuard) {
      TEST_ASSERT_EQUAL(Phase::Fading, t.phase());  // the bug the guard is for
      continue;
    }
    TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, lowest);
    // The backend starts the new track: its whole length is ahead.
    for (uint32_t i = 0; i < 50; ++i) {
      now += 20;
      lowest = std::min(lowest, feed(t, g, now, 2, 8, i * 20u).fadeQ15);
    }
    TEST_ASSERT_EQUAL(Phase::Armed, t.phase());
    TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, lowest);
  }
}

// ---- what it shows ----

void test_texts() {
  SleepTimer t;
  char row[16], sh[12];
  t.rowText(0, row, sizeof(row));
  t.shortText(0, sh, sizeof(sh));
  TEST_ASSERT_EQUAL_STRING("Off", row);
  TEST_ASSERT_EQUAL_STRING("", sh);
  t.setTimed(30 * 60000, 1000);
  t.rowText(1000 + 7 * 60000 + 1, row, sizeof(row));
  TEST_ASSERT_EQUAL_STRING("23 min", row);
  t.shortText(1000 + 30 * 60000 - 45000, sh, sizeof(sh));
  TEST_ASSERT_EQUAL_STRING("45 s", sh);  // the last minute counts seconds
  t.setEnd(Choice::EndOfAlbum);
  t.rowText(0, row, sizeof(row));
  t.shortText(0, sh, sizeof(sh));
  TEST_ASSERT_EQUAL_STRING("End of album", row);
  TEST_ASSERT_EQUAL_STRING("album", sh);
  TEST_ASSERT_EQUAL_INT(-1, t.timedIndex());
  // The sheet's title, lower case after "Sleep timer: ".
  char title[24];
  t.titleText(0, title, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("end of album", title);
  t.setTimed(30 * 60000, 1000);
  t.titleText(1000 + 7 * 60000 + 1, title, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("23 min left", title);
  t.titleText(1000 + 30 * 60000 - 45000, title, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("45 s left", title);
  t.cancel();
  t.titleText(0, title, sizeof(title));
  TEST_ASSERT_EQUAL_STRING("off", title);
}

void test_the_curve() {
  TEST_ASSERT_EQUAL_UINT16(SleepTimer::kUnity, SleepTimer::curveQ15(0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, -20.0f, db(SleepTimer::curveQ15(0.5f)));
  TEST_ASSERT_FLOAT_WITHIN(0.2f, -40.0f, db(SleepTimer::curveQ15(1.0f)));
  uint16_t last = SleepTimer::kUnity;
  for (int i = 0; i <= 100; ++i) {
    const uint16_t q = SleepTimer::curveQ15(i / 100.0f);
    TEST_ASSERT_TRUE(q <= last);
    last = q;
  }
}

// End of album reads the album; with no album information, the folder.
void test_album_ends_between() {
  LibraryIndex index;
  index.begin("/music");
  index.addFile("/music/A/X/01 - a.mp3");  // 0: album X
  index.addFile("/music/A/X/02 - b.mp3");  // 1: album X
  index.addFile("/music/A/Y/01 - c.mp3");  // 2: album Y
  index.addFile("/music/A/03 - d.mp3");    // 3: A's loose tracks (no album)
  index.addFile("/music/A/04 - e.mp3");    // 4: the same folder
  index.addFile("/music/B/05 - f.mp3");    // 5: another folder, no album
  index.finish();
  auto id = [&](const char* path) { return TrackCatalog(&index).find(path); };
  const uint32_t a = id("/music/A/X/01 - a.mp3"), b = id("/music/A/X/02 - b.mp3"), c = id("/music/A/Y/01 - c.mp3"),
                 d = id("/music/A/03 - d.mp3"), e = id("/music/A/04 - e.mp3"), f = id("/music/B/05 - f.mp3");
  TEST_ASSERT_FALSE(SleepTimer::albumEndsBetween(&index, a, b));
  TEST_ASSERT_TRUE(SleepTimer::albumEndsBetween(&index, b, c));
  TEST_ASSERT_TRUE(SleepTimer::albumEndsBetween(&index, c, d));
  TEST_ASSERT_FALSE(SleepTimer::albumEndsBetween(&index, d, e));
  TEST_ASSERT_TRUE(SleepTimer::albumEndsBetween(&index, e, f));
  TEST_ASSERT_TRUE(SleepTimer::albumEndsBetween(&index, a, LibraryIndex::kNone));
  TEST_ASSERT_TRUE(SleepTimer::albumEndsBetween(&index, a, TrackCatalog::builtins().ids[0]));
}

// The fade toast's buttons (SleepTimer::toastTap()): +10 min and Turn off,
// each to half the gap, Turn off to the edge. Neither on a clamped
// reading (a pocket's fabric), nor on the touch that attended a screen
// woken from off (a pocket's second contact): both raise the level.
void test_the_fade_toast_buttons() {
  using B = SleepTimer::ToastButton;
  using namespace uitext;
  TEST_ASSERT_EQUAL(B::None, SleepTimer::toastTap(kToastTextX + 10, false, false));
  TEST_ASSERT_EQUAL(B::Extend, SleepTimer::toastTap(kSleepToastPlusX + 5, false, false));
  TEST_ASSERT_EQUAL(B::Extend, SleepTimer::toastTap(kSleepToastPlusX + kSleepToastPlusW, false, false));
  TEST_ASSERT_EQUAL(B::TurnOff, SleepTimer::toastTap(kSleepToastOffX, false, false));
  TEST_ASSERT_EQUAL(B::TurnOff, SleepTimer::toastTap(319, false, false));
  for (int x = 0; x < 320; x += 7) {
    TEST_ASSERT_EQUAL(B::None, SleepTimer::toastTap(x, true, false));   // clamped at the edge
    TEST_ASSERT_EQUAL(B::None, SleepTimer::toastTap(x, false, true));   // the touch that attended
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_timed_expiry_fades_pauses_restores_then_releases);
  RUN_TEST(test_release_order);
  RUN_TEST(test_expiry_while_paused_skips_the_fade);
  RUN_TEST(test_expiry_during_a_wait_ends_it_paused);
  RUN_TEST(test_end_of_track_fades_the_last_10_s_and_pauses_on_the_next_entry);
  RUN_TEST(test_end_of_track_of_unknown_length_pauses_without_a_fade);
  RUN_TEST(test_end_of_album_pauses_only_at_the_album_end);
  RUN_TEST(test_end_of_queue_without_repeat_stops_at_the_end);
  RUN_TEST(test_end_of_queue_with_repeat_pauses_on_the_first_entry);
  RUN_TEST(test_skip_during_the_fade_keeps_its_level);
  RUN_TEST(test_skip_during_a_track_fade_holds_it_for_the_new_track);
  RUN_TEST(test_only_a_fade_that_counts_down_holds_the_screen);
  RUN_TEST(test_a_pause_during_the_fade_finishes_it);
  RUN_TEST(test_the_fade_never_rises_by_itself);
  RUN_TEST(test_extend_during_the_fade_ramps_up_slowly);
  RUN_TEST(test_turn_off_during_the_fade_ramps_up_slowly);
  RUN_TEST(test_extend_and_restart_during_the_countdown);
  RUN_TEST(test_a_play_before_the_pause_settles_ends_it);
  RUN_TEST(test_a_play_after_the_pause_cancels_the_release);
  RUN_TEST(test_headphone_play_is_ignored_after_a_timer_pause);
  RUN_TEST(test_a_drop_during_the_countdown_ends_paused_and_releases);
  RUN_TEST(test_extend_never_shortens_an_end_of_choice);
  RUN_TEST(test_extend_after_a_skip_away_from_the_boundary_rearms);
  RUN_TEST(test_entry_start_waits_for_the_backend);
  RUN_TEST(test_end_of_track_after_a_skip_near_the_end_does_not_fade_the_new_track);
  RUN_TEST(test_the_fade_toast_buttons);
  RUN_TEST(test_texts);
  RUN_TEST(test_the_curve);
  RUN_TEST(test_album_ends_between);
  return UNITY_END();
}
