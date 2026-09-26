// Host unit tests for HeadsetKeys: the headphones' transport keys never start
// music that wasn't playing. Run: pio test -e native
#include <unity.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "HeadsetKeys.h"
#include "PlaybackController.h"
#include "Track.h"
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

  bool play(const std::string& p, uint32_t) override {
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

std::vector<Track> threeTracks() {
  return {{"/a.mp3", "A", "x", 1000}, {"/b.mp3", "B", "y", 1000}, {"/c.mp3", "C", "z", 1000}};
}

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
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  for (Key k : {Key::Play, Key::Next, Key::Play, Key::Prev, Key::Prev, Key::Pause, Key::Play}) {
    HeadsetKeys::apply(p, k);
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
    TEST_ASSERT_EQUAL_INT(0, a.playCount);
  }
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());  // +1, -1, -1 from 0, wrapped
}

void test_next_and_prev_while_paused_select_without_playing() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
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
  TEST_ASSERT_EQUAL_STRING("/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(0, a.resumeCount);
  TEST_ASSERT_TRUE(audible(a));
}

void test_play_and_pause_are_commands_not_toggles() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
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
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.play(0);
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Next));
  TEST_ASSERT_EQUAL_STRING("/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL(Action::Skip, HeadsetKeys::apply(p, Key::Prev));
  TEST_ASSERT_EQUAL_STRING("/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_TRUE(audible(a));
}

// Any sequence of headphone keys: music is only audible after a key if it
// was playing, or paused and the key was PLAY.
void test_no_key_sequence_starts_music_that_was_not_playing() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_the_rule);
  RUN_TEST(test_nothing_starts_from_stopped);
  RUN_TEST(test_next_and_prev_while_paused_select_without_playing);
  RUN_TEST(test_play_and_pause_are_commands_not_toggles);
  RUN_TEST(test_next_and_prev_while_playing_skip);
  RUN_TEST(test_no_key_sequence_starts_music_that_was_not_playing);
  return UNITY_END();
}
