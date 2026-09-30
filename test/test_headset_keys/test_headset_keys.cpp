// Host unit tests for HeadsetKeys: the headphones' transport keys never start
// music that wasn't playing. Run: pio test -e native
#include <unity.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "HeadsetKeys.h"
#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

using Key = HeadsetKeys::Key;
using Action = HeadsetKeys::Action;

namespace {
class FakeAudioBackend : public IAudioBackend {
public:
  std::string lastPath;
  int playCount = 0;
  int resumeCount = 0;
  bool playing = false;
  bool paused = false;

  bool play(const std::string& p, uint32_t, uint32_t) override {
    lastPath = p;
    ++playCount;
    playing = true;
    paused = false;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override {
    paused = false;
    ++resumeCount;
  }
  void stop() override { playing = false; paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return false; }
  bool failed() const override { return false; }
};

// A library of up to three tracks at the root ("/music/a.mp3" is id 0, b 1,
// c 2: ids are in the order the files were added), a queue of all of them,
// and the player.
struct Rig {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  QueueModel queue;
  FakeAudioBackend audio;
  PlaybackController player{audio, queue, catalog};

  explicit Rig(uint32_t tracks) {
    const char* files[] = {"/music/a.mp3", "/music/b.mp3", "/music/c.mp3"};
    index.begin("/music");
    for (uint32_t i = 0; i < tracks; ++i) index.addFile(files[i]);
    index.finish();
    const uint32_t ids[] = {0, 1, 2};
    queue.assign(ids, tracks, 0);
  }
};

// Music is audible: the backend plays and isn't paused.
bool audible(const FakeAudioBackend& a) { return a.playing && !a.paused; }
}  // namespace

void setUp() {}
void tearDown() {}

void test_the_rule() {
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::decide(PlayState::Stopped, Key::Play));
  TEST_ASSERT_EQUAL(Action::Resume, HeadsetKeys::decide(PlayState::Paused, Key::Play));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::decide(PlayState::Playing, Key::Play));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::decide(PlayState::Stopped, Key::Pause));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::decide(PlayState::Paused, Key::Pause));
  TEST_ASSERT_EQUAL(Action::Pause, HeadsetKeys::decide(PlayState::Playing, Key::Pause));
  for (Key k : {Key::Next, Key::Prev}) {
    TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::decide(PlayState::Stopped, k));
    TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::decide(PlayState::Paused, k));
    TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::decide(PlayState::Playing, k));
  }
}

// After a boot and an automatic reconnect: nothing the headphones send starts music.
void test_nothing_starts_from_stopped() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  for (Key k : {Key::Play, Key::Next, Key::Play, Key::Prev, Key::Prev, Key::Pause, Key::Play}) {
    HeadsetKeys::apply(p, k);
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
    TEST_ASSERT_EQUAL_INT(0, a.playCount);
  }
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());  // +1, -1, -1 from 0, wrapped
}

void test_next_and_prev_while_paused_select_without_playing() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  TEST_ASSERT_EQUAL(Action::Pause, HeadsetKeys::apply(p, Key::Pause));
  TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::apply(p, Key::Next));  // a double-press while adjusting a bud
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_FALSE(audible(a));
  TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);
  TEST_ASSERT_FALSE(audible(a));
  // Their PLAY resumes: the selected track, from its start.
  TEST_ASSERT_EQUAL(Action::Resume, HeadsetKeys::apply(p, Key::Play));
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(0, a.resumeCount);
  TEST_ASSERT_TRUE(audible(a));
}

void test_play_and_pause_are_commands_not_toggles() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(1);
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::apply(p, Key::Play));  // ear detection: already playing
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL(Action::Pause, HeadsetKeys::apply(p, Key::Pause));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::apply(p, Key::Pause));  // a repeat doesn't undo it
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL(Action::Resume, HeadsetKeys::apply(p, Key::Play));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::apply(p, Key::Play));
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, a.resumeCount);  // the paused track itself, not a restart
  TEST_ASSERT_EQUAL_INT(1, a.playCount);
}

void test_next_and_prev_while_playing_skip() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Next));
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_TRUE(audible(a));
}

// Any sequence of headphone keys: music is only audible after a key if it
// was playing, or paused and the key was PLAY.
void test_no_key_sequence_starts_music_that_was_not_playing() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  const Key keys[] = {Key::Play, Key::Pause, Key::Next, Key::Prev};
  uint32_t x = 12345;
  for (int i = 0; i < 2000; ++i) {
    x = x * 1664525u + 1013904223u;
    if ((x >> 28) == 0) {  // now and then the listener plays or pauses on the Core2
      p.togglePlayPause();
      continue;
    }
    const Key k = keys[(x >> 8) % 4];
    const PlayState before = p.state();
    HeadsetKeys::apply(p, k);
    if (before != PlayState::Playing && !(before == PlayState::Paused && k == Key::Play)) {
      TEST_ASSERT_FALSE(audible(a));
      TEST_ASSERT_TRUE(p.state() != PlayState::Playing);
    }
    TEST_ASSERT_EQUAL(p.state() == PlayState::Playing, audible(a));
  }
}

// The sleep timer's pause: the headphones' Play (in-ear detection as a
// sleeper turns over) doesn't resume it; the Core2's own play does, and
// clears the mark. Their Next/Prev still only select.
void test_play_does_not_resume_a_sleep_timer_pause() {
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::decide(PlayState::Paused, Key::Play, true));
  TEST_ASSERT_EQUAL(Action::Resume, HeadsetKeys::decide(PlayState::Paused, Key::Play, false));
  TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::decide(PlayState::Paused, Key::Next, true));
  Rig r(3);
  r.player.play(0);
  r.player.pauseByTimer();
  for (int i = 0; i < 5; ++i) {
    TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::apply(r.player, Key::Play));
    TEST_ASSERT_FALSE(audible(r.audio));
  }
  TEST_ASSERT_EQUAL(Action::Cue, HeadsetKeys::apply(r.player, Key::Next));
  TEST_ASSERT_EQUAL(Action::Ignore, HeadsetKeys::apply(r.player, Key::Play));
  TEST_ASSERT_FALSE(audible(r.audio));
  r.player.togglePlayPause();  // the Core2's play button
  TEST_ASSERT_TRUE(audible(r.audio));
  TEST_ASSERT_EQUAL(Action::Pause, HeadsetKeys::apply(r.player, Key::Pause));
  TEST_ASSERT_EQUAL(Action::Resume, HeadsetKeys::apply(r.player, Key::Play));  // their own pause
}

// Which keys are someone's input for the idle power-off: only a key that
// acted. In-ear detection's PLAY and PAUSE while paused (by the timer or
// not) do nothing and don't count, so a sleeper's buds can't keep the
// device on all night; after the timer's pause a cue doesn't count either.
void test_only_a_key_that_acts_is_idle_input() {
  TEST_ASSERT_TRUE(HeadsetKeys::isInput(Action::Pause, false));
  TEST_ASSERT_TRUE(HeadsetKeys::isInput(Action::Resume, false));
  TEST_ASSERT_TRUE(HeadsetKeys::isInput(Action::Skip, false));
  TEST_ASSERT_TRUE(HeadsetKeys::isInput(Action::Cue, false));
  TEST_ASSERT_FALSE(HeadsetKeys::isInput(Action::Cue, true));
  TEST_ASSERT_FALSE(HeadsetKeys::isInput(Action::Ignore, false));
  TEST_ASSERT_FALSE(HeadsetKeys::isInput(Action::Ignore, true));
  // As main.cpp feeds it: the mark read before the key.
  Rig r(3);
  r.player.play(0);
  bool byTimer = r.player.pausedByTimer();
  TEST_ASSERT_TRUE(HeadsetKeys::isInput(HeadsetKeys::apply(r.player, Key::Pause), byTimer));  // it paused
  for (int i = 0; i < 3; ++i) {
    byTimer = r.player.pausedByTimer();
    TEST_ASSERT_FALSE(HeadsetKeys::isInput(HeadsetKeys::apply(r.player, Key::Pause), byTimer));  // already paused
  }
  r.player.togglePlayPause();
  r.player.pauseByTimer();
  for (Key k : {Key::Play, Key::Pause, Key::Next, Key::Prev}) {
    byTimer = r.player.pausedByTimer();
    TEST_ASSERT_TRUE(byTimer);
    TEST_ASSERT_FALSE(HeadsetKeys::isInput(HeadsetKeys::apply(r.player, k), byTimer));
    TEST_ASSERT_FALSE(audible(r.audio));
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_rule);
  RUN_TEST(test_nothing_starts_from_stopped);
  RUN_TEST(test_next_and_prev_while_paused_select_without_playing);
  RUN_TEST(test_play_and_pause_are_commands_not_toggles);
  RUN_TEST(test_next_and_prev_while_playing_skip);
  RUN_TEST(test_no_key_sequence_starts_music_that_was_not_playing);
  RUN_TEST(test_play_does_not_resume_a_sleep_timer_pause);
  RUN_TEST(test_only_a_key_that_acts_is_idle_input);
  return UNITY_END();
}
