// Host unit tests for PlaybackController. Run: pio test -e native
#include <unity.h>

#include <string>
#include <vector>

#include "PlaybackController.h"
#include "Track.h"
#include "hal/IAudioBackend.h"

namespace {
class FakeAudioBackend : public IAudioBackend {
public:
  std::string lastPath;
  int playCount = 0;
  bool playing = false;
  bool paused = false;
  bool finishedFlag = false;

  bool play(const std::string& p, uint32_t) override {
    lastPath = p;
    ++playCount;
    playing = true;
    paused = false;
    finishedFlag = false;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override { playing = false; paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return finishedFlag; }
};

std::vector<Track> twoTracks() {
  return {{"/a.mp3", "A", "x", 1000}, {"/b.mp3", "B", "y", 1000}};
}
}  // namespace

void setUp() {}
void tearDown() {}

void test_setPlaylist_selects_first_and_stops() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
}

void test_play_starts_selected_track() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  p.play(1);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/b.mp3", a.lastPath.c_str());
}

void test_toggle_play_pause_resume() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  p.togglePlayPause();  // Stopped -> Playing (track 0)
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  p.togglePlayPause();  // -> Paused
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_TRUE(a.paused);
  p.togglePlayPause();  // -> Playing
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_FALSE(a.paused);
}

void test_next_and_prev_wrap() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  p.play(1);
  p.next();  // wraps 1 -> 0
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  p.prev();  // wraps 0 -> 1
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
}

void test_auto_advance_when_track_finishes() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  p.play(0);
  a.finishedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);  // initial + advanced
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_setPlaylist_selects_first_and_stops);
  RUN_TEST(test_play_starts_selected_track);
  RUN_TEST(test_toggle_play_pause_resume);
  RUN_TEST(test_next_and_prev_wrap);
  RUN_TEST(test_auto_advance_when_track_finishes);
  return UNITY_END();
}
