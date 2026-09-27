// Host unit tests for PlaybackController. Run: pio test -e native
#include <unity.h>

#include <string>
#include <vector>

#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
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
  int stopCount = 0;

  bool play(const std::string& p, uint32_t) override {
    lastPath = p;
    ++playCount;
    playing = true;
    paused = false;
    finishedFlag = false;
    failedFlag = failEverything || p.empty();  // "": an id the catalog doesn't know
    return true;
  }
  void pause() override { paused = true; }
  void resume() override { paused = false; }
  void stop() override {
    playing = false;
    paused = false;
    ++stopCount;
  }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return finishedFlag; }
  bool failed() const override { return failedFlag; }
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
}  // namespace

void setUp() {}
void tearDown() {}

void test_a_new_queue_selects_first_and_stops() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
}

void test_play_starts_selected_track() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(1);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
}

void test_toggle_play_pause_resume() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
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
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(1);
  p.next();  // wraps 1 -> 0
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  p.prev();  // wraps 0 -> 1
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
}

void test_auto_advance_when_track_finishes() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.finishedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);  // initial + advanced
}

void test_failed_track_is_skipped() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
}

// The UI's note and the Queue's "!" come from the failure record: which
// entry failed, counted once per failure.
void test_a_failure_is_recorded_with_its_entry() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  TEST_ASSERT_EQUAL_UINT32(0, p.lastFailure().count);
  p.play(1);
  const uint32_t key = r.queue.keyAt(1);
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().track);
  TEST_ASSERT_EQUAL_UINT32(key, p.lastFailure().key);
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());  // skipped on
  // The next one plays: nothing new is recorded.
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
}

void test_all_tracks_failing_stops_after_one_pass() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  a.failEverything = true;
  p.play(0);
  p.update(0);  // track 0 failed -> try track 1
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  p.update(0);  // track 1 failed too: nothing in the queue plays
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  p.update(0);  // and it stays stopped
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
}

void test_a_finished_track_resets_the_failure_count() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
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
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  a.failEverything = true;
  p.play(0);
  p.update(0);  // 0 failed -> 1 (one failure)
  p.next();     // the user skips: counting starts over
  p.update(0);  // 0 failed -> 1
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_cue_while_stopped_only_moves() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.cueNext();
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  p.cuePrev();
  p.cuePrev();  // wraps 0 -> 2
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  TEST_ASSERT_EQUAL_INT(0, a.playCount);
  p.togglePlayPause();  // play starts the cued track
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_cue_while_paused_stays_paused_and_play_starts_it() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
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
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  p.togglePlayPause();  // an ordinary pause and resume again
  p.togglePlayPause();
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  TEST_ASSERT_FALSE(a.paused);
}

void test_cue_while_playing_skips() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(1);
  p.cueNext();
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  p.cuePrev();
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_next_after_a_cue_while_paused_plays() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
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

void test_cue_on_an_empty_queue_does_nothing() {
  Rig r(0);
  PlaybackController& p = r.player;
  p.cueNext();
  p.cuePrev();
  TEST_ASSERT_EQUAL_INT(-1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
}

// ---- the queue's edits and playback ----

void test_play_now_replaces_the_queue_and_plays_from_start() {
  Rig r(3);
  const uint32_t album[] = {2, 0, 1};
  TEST_ASSERT_TRUE(r.player.playNow(album, 3, 1));
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.audio.lastPath.c_str());
  r.audio.finishedFlag = true;
  r.player.update(0);  // the rest of the album follows
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
}

void test_play_next_and_add_leave_playback_alone() {
  Rig r(3);
  r.player.play(0);
  const uint32_t c[] = {2};
  TEST_ASSERT_TRUE(r.player.playNext(c, 1));
  TEST_ASSERT_TRUE(r.player.addToQueue(c, 1));
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  r.player.next();  // Play next: right after what plays
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(5, r.queue.size());
}

void test_adding_to_an_empty_queue_selects_without_playing() {
  Rig r(3);
  r.player.clearQueue();
  const uint32_t b[] = {1};
  r.player.addToQueue(b, 1);
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  r.player.togglePlayPause();  // B plays it
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
}

void test_removing_the_playing_track_plays_the_next() {
  Rig r(3);
  r.player.play(1);
  const uint32_t pos[] = {1};
  const QueueModel::Removed rm = r.player.remove(pos, 1);
  TEST_ASSERT_TRUE(rm.current);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  // Undo: c keeps playing, b is back before it.
  const int plays = r.audio.playCount;
  TEST_ASSERT_TRUE(r.player.undo());
  TEST_ASSERT_EQUAL_INT(plays, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(3, r.queue.size());
}

void test_removing_the_last_playing_track_stops() {
  Rig r(3);
  r.player.play(2);
  const uint32_t pos[] = {2};
  const QueueModel::Removed rm = r.player.remove(pos, 1);
  TEST_ASSERT_TRUE(rm.current && rm.pastEnd);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_FALSE(r.audio.playing);
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());  // on the last one left
}

void test_removing_the_paused_track_cues_the_next() {
  Rig r(3);
  r.player.play(0);
  r.player.togglePlayPause();
  const uint32_t pos[] = {0};
  r.player.remove(pos, 1);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_FALSE(r.audio.playing);  // the removed track can't be resumed
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  r.player.togglePlayPause();  // plays what is current now, from its start
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
}

void test_removing_other_tracks_changes_nothing_that_plays() {
  Rig r(3);
  r.player.play(1);
  const uint32_t pos[] = {0, 2};
  r.player.remove(pos, 2);
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

void test_clear_queue_stops_and_undo_brings_it_back_stopped() {
  Rig r(3);
  r.player.play(1);
  r.player.clearQueue();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(-1, r.player.currentIndex());
  r.player.togglePlayPause();  // nothing to play
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_TRUE(r.player.undo());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
}

void test_clear_up_next_keeps_playing() {
  Rig r(3);
  r.player.play(1);
  TEST_ASSERT_TRUE(r.player.clearUpNext());
  TEST_ASSERT_EQUAL_UINT32(2, r.queue.size());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

void test_undo_of_play_now_goes_back_to_the_old_track() {
  Rig r(3);
  r.player.play(2);
  const uint32_t one[] = {0};
  r.player.playNow(one, 1, 0);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_TRUE(r.player.undo());
  TEST_ASSERT_EQUAL_UINT32(3, r.queue.size());
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", r.audio.lastPath.c_str());  // it plays again, from its start
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

void test_builtin_tracks_play_by_their_tone_paths() {
  Rig r(0);
  const LibraryIndex::Span b = TrackCatalog::builtins();
  r.player.playNow(b.ids, b.count, 4);
  TEST_ASSERT_EQUAL_STRING("tone:click120", r.audio.lastPath.c_str());
}

void test_an_unknown_track_is_skipped() {
  Rig r(3);
  const uint32_t ids[] = {0, 77, 2};  // 77: not in the library (it was rebuilt, say)
  r.player.playNow(ids, 3, 1);
  TEST_ASSERT_EQUAL_STRING("", r.audio.lastPath.c_str());
  r.player.update(0);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

void test_without_repeat_the_end_of_the_queue_stops() {
  Rig r(2);
  r.player.setRepeat(false);
  r.player.play(1);
  r.audio.finishedFlag = true;
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  r.player.prev();
  r.player.prev();  // at the start: the first track again, no wrap
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
}

void test_a_replaced_queue_starts_the_new_current_if_the_old_one_is_gone() {
  Rig r(3);
  r.player.play(0);
  const uint32_t ids[] = {1, 2};
  r.queue.assign(ids, 2, 0);
  r.player.queueReplaced(/*currentKept=*/true);
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);  // what plays is still in the queue
  r.player.queueReplaced(/*currentKept=*/false);
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_new_queue_selects_first_and_stops);
  RUN_TEST(test_play_starts_selected_track);
  RUN_TEST(test_toggle_play_pause_resume);
  RUN_TEST(test_next_and_prev_wrap);
  RUN_TEST(test_auto_advance_when_track_finishes);
  RUN_TEST(test_failed_track_is_skipped);
  RUN_TEST(test_a_failure_is_recorded_with_its_entry);
  RUN_TEST(test_all_tracks_failing_stops_after_one_pass);
  RUN_TEST(test_a_finished_track_resets_the_failure_count);
  RUN_TEST(test_user_skip_resets_the_failure_count);
  RUN_TEST(test_cue_while_stopped_only_moves);
  RUN_TEST(test_cue_while_paused_stays_paused_and_play_starts_it);
  RUN_TEST(test_cue_while_playing_skips);
  RUN_TEST(test_next_after_a_cue_while_paused_plays);
  RUN_TEST(test_cue_on_an_empty_queue_does_nothing);
  RUN_TEST(test_play_now_replaces_the_queue_and_plays_from_start);
  RUN_TEST(test_play_next_and_add_leave_playback_alone);
  RUN_TEST(test_adding_to_an_empty_queue_selects_without_playing);
  RUN_TEST(test_removing_the_playing_track_plays_the_next);
  RUN_TEST(test_removing_the_last_playing_track_stops);
  RUN_TEST(test_removing_the_paused_track_cues_the_next);
  RUN_TEST(test_removing_other_tracks_changes_nothing_that_plays);
  RUN_TEST(test_clear_queue_stops_and_undo_brings_it_back_stopped);
  RUN_TEST(test_clear_up_next_keeps_playing);
  RUN_TEST(test_undo_of_play_now_goes_back_to_the_old_track);
  RUN_TEST(test_builtin_tracks_play_by_their_tone_paths);
  RUN_TEST(test_an_unknown_track_is_skipped);
  RUN_TEST(test_without_repeat_the_end_of_the_queue_stops);
  RUN_TEST(test_a_replaced_queue_starts_the_new_current_if_the_old_one_is_gone);
  return UNITY_END();
}
