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
  bool failedFlag = false;
  bool failEverything = false;  // every track played from now on fails

  bool play(const std::string& p, uint32_t) override {
    lastPath = p;
    ++playCount;
    playing = true;
    paused = false;
    finishedFlag = false;
    failedFlag = failEverything;
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override { playing = false; paused = false; }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return finishedFlag; }
  bool failed() const override { return failedFlag; }
};

std::vector<Track> twoTracks() {
  return {{"/a.mp3", "A", "x", 1000}, {"/b.mp3", "B", "y", 1000}};
}

std::vector<Track> threeTracks() {
  return {{"/a.mp3", "A", "x", 1000}, {"/b.mp3", "B", "y", 1000}, {"/c.mp3", "C", "z", 1000}};
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

void test_failed_track_is_skipped() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  p.play(0);
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/b.mp3", a.lastPath.c_str());
}

void test_all_tracks_failing_stops_after_one_pass() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  a.failEverything = true;
  p.play(0);
  p.update(0);  // track 0 failed -> try track 1
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  p.update(0);  // track 1 failed too: nothing in the playlist plays
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  p.update(0);  // and it stays stopped
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
}

void test_a_finished_track_resets_the_failure_count() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.play(0);
  a.failedFlag = true;
  p.update(0);  // 0 failed -> 1
  a.finishedFlag = true;
  p.update(0);  // 1 played through -> 2
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  a.failedFlag = true;
  p.update(0);  // 2 failed -> 0: one failure since the last good track, keep going
  a.failedFlag = true;
  p.update(0);  // 0 failed -> 1
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
}

void test_user_skip_resets_the_failure_count() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(twoTracks());
  a.failEverything = true;
  p.play(0);
  p.update(0);  // 0 failed -> 1 (one failure)
  p.next();     // the user skips: counting starts over
  p.update(0);  // 0 failed -> 1
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_cue_while_stopped_only_moves() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.cueNext();
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  p.cuePrev();
  p.cuePrev();  // wraps 0 -> 2
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  TEST_ASSERT_EQUAL_INT(0, a.playCount);
  p.togglePlayPause();  // play starts the cued track
  TEST_ASSERT_EQUAL_STRING("/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_cue_while_paused_stays_paused_and_play_starts_it() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.play(0);
  p.togglePlayPause();  // paused in track 0
  p.cueNext();
  p.cueNext();
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);  // nothing started
  TEST_ASSERT_FALSE(a.playing);           // the paused track was dropped
  p.update(0);                            // nothing advances while paused
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  p.togglePlayPause();  // not a resume of track 0: the cued track from its start
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  p.togglePlayPause();  // an ordinary pause and resume again
  p.togglePlayPause();
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  TEST_ASSERT_FALSE(a.paused);
}

void test_cue_while_playing_skips() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.play(1);
  p.cueNext();
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/c.mp3", a.lastPath.c_str());
  p.cuePrev();
  TEST_ASSERT_EQUAL_STRING("/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_next_after_a_cue_while_paused_plays() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.setPlaylist(threeTracks());
  p.play(0);
  p.togglePlayPause();
  p.cueNext();  // paused on track 1, not started
  p.next();     // the Core2's own button: skips and plays, as before
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  p.togglePlayPause();  // then pause and resume are ordinary
  p.togglePlayPause();
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
}

void test_cue_on_an_empty_playlist_does_nothing() {
  FakeAudioBackend a;
  PlaybackController p(a);
  p.cueNext();
  p.cuePrev();
  TEST_ASSERT_EQUAL_INT(-1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_setPlaylist_selects_first_and_stops);
  RUN_TEST(test_play_starts_selected_track);
  RUN_TEST(test_toggle_play_pause_resume);
  RUN_TEST(test_next_and_prev_wrap);
  RUN_TEST(test_auto_advance_when_track_finishes);
  RUN_TEST(test_failed_track_is_skipped);
  RUN_TEST(test_all_tracks_failing_stops_after_one_pass);
  RUN_TEST(test_a_finished_track_resets_the_failure_count);
  RUN_TEST(test_user_skip_resets_the_failure_count);
  RUN_TEST(test_cue_while_stopped_only_moves);
  RUN_TEST(test_cue_while_paused_stays_paused_and_play_starts_it);
  RUN_TEST(test_cue_while_playing_skips);
  RUN_TEST(test_next_after_a_cue_while_paused_plays);
  RUN_TEST(test_cue_on_an_empty_playlist_does_nothing);
  return UNITY_END();
}
