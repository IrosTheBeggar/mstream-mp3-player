// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// Host unit tests for PlaybackController. Run: pio test -e native
#include <unity.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "LibraryIndex.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
#include "TrackSeek.h"
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
  RateRefusal refusal;          // why it failed, when its rate was why
  std::string failureText;      // ... or its own few words for the note ("": none)
  std::string unseekable;       // the one path seekable() is false for (an .opus, say)
  int stopCount = 0;
  uint32_t lastStartMs = 0;  // where the last play() asked to start
  uint32_t lastHintMs = 0;   // the length it was handed
  uint32_t position = 0;     // what positionMs() says
  uint32_t duration = 0;
  // Starts are taken up later, as on the Core2 (its decode task): until
  // take(), positionKnown() is false and the position is the last track's.
  bool asyncStarts = false;
  bool pending = false;
  // Resume anchors (docs/SEEK.md): off, this fake has none, as the others
  // (IAudioBackend's defaults). The anchor the last play() was handed, and
  // the one resumeAnchor() gives for the held track.
  bool anchorsOn = false;
  ResumeAnchor lastAnchor;
  int anchoredPlays = 0;
  ResumeAnchor held;

  bool play(const std::string& p, const StartAt& at) override {
    if (!anchorsOn) return IAudioBackend::play(p, at);
    lastAnchor = at.anchor;
    if (at.anchor.valid()) ++anchoredPlays;
    return play(p, at.hintMs, at.ms);
  }
  bool resumeAnchor(ResumeAnchor* out) const override {
    if (!anchorsOn || !held.valid()) return false;
    *out = held;
    return true;
  }

  bool play(const std::string& p, uint32_t hintMs, uint32_t startMs) override {
    lastPath = p;
    lastStartMs = startMs;
    lastHintMs = hintMs;
    if (asyncStarts) {
      pending = true;
    } else {
      position = startMs;
    }
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
  uint32_t positionMs() const override { return position; }
  bool positionKnown() const override { return !pending; }
  uint32_t durationMs() const override { return duration; }
  // The pending start taken up (asyncStarts).
  void take() {
    if (!pending) return;
    pending = false;
    position = lastStartMs;
  }
  bool finished() const override { return finishedFlag; }
  // The track reaches its natural end: finished(), the position at the end
  // it got to (the one a test set, else a second in: a natural end with
  // the position still at 0:00 is an empty track, which the player takes
  // for a failure, test_an_end_at_0_00_is_a_failure).
  void finish() {
    finishedFlag = true;
    if (position == 0) position = 1000;
  }
  bool failed() const override { return failedFlag; }
  RateRefusal rateRefusal() const override { return failedFlag ? refusal : RateRefusal{}; }
  size_t failureNote(char* buf, size_t size) const override {
    snprintf(buf, size, "%s", failedFlag ? failureText.c_str() : "");
    return strlen(buf);
  }
  bool seekable(const char* path) const override { return unseekable != path; }
  // Gapless playback: the words the player sends, and joins it is told
  // were heard (advances, taken once each).
  std::vector<Next> nexts;
  std::vector<uint32_t> advances;
  void setNext(const Next& n) override { nexts.push_back(n); }
  bool takeAdvance(uint32_t* token) override {
    if (advances.empty()) return false;
    *token = advances.front();
    advances.erase(advances.begin());
    position = 0;  // the joined track's, from its start
    return true;
  }
};

// A library of up to eight tracks at the root ("/music/a.mp3" is id 0, b 1,
// c 2 ... h 7: ids are in the order the files were added), a queue of all
// of them, and the player.
// A play must wait while `hold` (the Core2's: Bluetooth is the output and
// the headphones aren't connected).
struct TestHold : PlaybackController::Hold {
  bool hold = false;
  bool holdPlay() const override { return hold; }
};

struct Rig {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  QueueModel queue;
  FakeAudioBackend audio;
  PlaybackController player{audio, queue, catalog};

  explicit Rig(uint32_t tracks) {
    const char* files[] = {"/music/a.mp3", "/music/b.mp3", "/music/c.mp3", "/music/d.mp3",
                           "/music/e.mp3", "/music/f.mp3", "/music/g.mp3", "/music/h.mp3"};
    index.begin("/music");
    for (uint32_t i = 0; i < tracks; ++i) index.addFile(files[i]);
    index.finish();
    const uint32_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
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

// All (the engine's default): next and prev wrap at the queue's ends.
void test_repeat_all_wraps() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  TEST_ASSERT_EQUAL_INT((int)PlaybackController::Repeat::All, (int)p.repeat());
  p.play(1);
  p.next();  // wraps 1 -> 0
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  p.prev();  // wraps 0 -> 1
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  a.finish();
  p.update(0);  // the end of the last: the first, playing
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
}

void test_auto_advance_when_track_finishes() {
  Rig r(2);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.finish();
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
  TEST_ASSERT_EQUAL_UINT32(0, p.lastFailure().rate.hz);  // not its rate: "can't play it"
}

// A sample rate the backend refused goes with the record, for the note's
// why ("96 kHz isn't supported", "needs the 240 MHz CPU speed"); the next
// failure for another reason clears it.
void test_a_rate_refusal_is_recorded_with_the_failure() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.refusal.hz = 96000;
  a.refusal.needsCpu = true;
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
  TEST_ASSERT_EQUAL_UINT32(96000, p.lastFailure().rate.hz);
  TEST_ASSERT_TRUE(p.lastFailure().rate.needsCpu);
  a.refusal = {};
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(2, p.lastFailure().count);
  TEST_ASSERT_EQUAL_UINT32(0, p.lastFailure().rate.hz);
  TEST_ASSERT_FALSE(p.lastFailure().rate.needsCpu);
}

// The backend's own few words go with the record too ("surround Opus isn't
// supported": Now Playing's toast shows them instead of "can't play it");
// a failure without any leaves the note empty.
void test_the_backends_note_is_recorded_with_the_failure() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.failureText = "surround Opus isn't supported";
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
  TEST_ASSERT_EQUAL_STRING("surround Opus isn't supported", p.lastFailure().note);
  TEST_ASSERT_EQUAL_UINT32(0, p.lastFailure().rate.hz);
  a.failureText.clear();
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(2, p.lastFailure().count);
  TEST_ASSERT_EQUAL_STRING("", p.lastFailure().note);
}

// The seek bar's knob follows the current entry, not the last play(): the
// backend is asked by the entry's path, so an entry waiting after a boot
// (no play() yet), a track joined gaplessly (no play() of its own) and the
// entry after a skip each get their own answer.
void test_seekable_is_the_current_entrys() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  a.unseekable = "/music/b.mp3";
  TEST_ASSERT_TRUE(p.seekable());  // stopped on a, as after a boot
  r.queue.setCurrent(1);
  TEST_ASSERT_FALSE(p.seekable());  // stopped on b: its own answer, no play() asked
  p.play(0);
  TEST_ASSERT_TRUE(p.seekable());
  const uint32_t t1 = a.nexts.back().token;  // b joins a
  a.advances.push_back(t1);
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);  // heard, adopted: no play()
  TEST_ASSERT_FALSE(p.seekable());
  p.next();  // c
  TEST_ASSERT_TRUE(p.seekable());
  p.stop();
  p.clearQueue();
  TEST_ASSERT_FALSE(p.seekable());  // no entry
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
  a.finish();
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
  r.audio.finish();
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

// Off: the end of the last track stops there, on it; next at the last
// stops too; prev at the first plays the first again.
void test_repeat_off_stops_at_the_end() {
  Rig r(2);
  r.player.setRepeat(PlaybackController::Repeat::Off);
  r.player.play(1);
  r.audio.finish();
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  r.player.prev();
  r.player.prev();  // at the start: the first track again, no wrap
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  r.player.play(1);
  r.player.next();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
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

// ---- the sleep timer's asks (SleepTimer) ----

// "Pause after this track": at the natural end, the next entry, paused at
// 0:00 (cued: nothing held by the backend); a later play starts it.
void test_pause_after_this_track_cues_the_next_entry() {
  Rig r(3);
  r.player.play(0);
  r.player.setPauseAfterTrack(true);
  r.audio.finish();
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_FALSE(r.player.pauseAfterTrack());  // done: once
  TEST_ASSERT_EQUAL_UINT32(1, r.player.timerStops());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);    // nothing started
  TEST_ASSERT_FALSE(r.audio.playing);             // the finished track let go
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
}

// A skip or a failure isn't the track's end: the flag stays for the next.
void test_pause_after_this_track_survives_a_skip_and_a_failure() {
  Rig r(3);
  r.player.play(0);
  r.player.setPauseAfterTrack(true);
  r.player.next();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  r.audio.failedFlag = true;
  r.player.update(0);  // b fails: skipped to c
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  r.audio.finish();
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());  // (repeat: the first entry)
}

// At the end of the queue without repeat: the natural stop.
void test_pause_after_the_last_track_without_repeat_stops() {
  Rig r(2);
  r.player.setRepeat(PlaybackController::Repeat::Off);
  r.player.play(1);
  r.player.setPauseAfterTrack(true);
  r.audio.finish();
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(1, r.player.timerStops());
}

// pauseByTimer(): playing pauses, a wait ends paused, a pause is marked;
// stopped stays stopped. Any play clears the mark.
void test_pause_by_timer_marks_the_pause() {
  Rig r(2);
  r.player.pauseByTimer();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
  r.player.play(0);
  r.player.pauseByTimer();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_TRUE(r.audio.paused);
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  r.player.cueNext();  // (the headphones' next while paused: still the timer's pause)
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  r.player.togglePlayPause();
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
  r.player.togglePlayPause();  // the listener's own pause: not the timer's
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
  r.player.pauseByTimer();     // a pause that is there becomes the timer's
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  r.player.next();             // a skip plays: cleared
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
}


// pauseByComputer() (the USB visualizer's entry) never starts anything:
// Playing pauses, Waiting ends paused (both marked the computer's), Paused
// and Stopped stay as they are with their marks, a cued entry stays cued.
// Any play clears the mark.
void test_pause_by_computer_never_starts_playback() {
  Rig r(3);
  r.player.pauseByComputer();  // stopped: stays stopped
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  TEST_ASSERT_FALSE(r.player.pausedByComputer());
  r.player.play(1);
  r.player.pauseByComputer();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_TRUE(r.audio.paused);
  TEST_ASSERT_TRUE(r.player.pausedByComputer());
  TEST_ASSERT_TRUE(r.player.pausedNotByListener());
  TEST_ASSERT_FALSE(r.player.pausedByTimer());
  r.player.pauseByComputer();  // paused: stays paused (togglePlayPause() would resume)
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_TRUE(r.audio.paused);
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  r.player.cueNext();  // paused on a cued entry: stays cued, nothing starts, still the computer's
  r.player.pauseByComputer();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_TRUE(r.player.pausedByComputer());
  r.player.togglePlayPause();  // the Core2's play: cleared
  TEST_ASSERT_FALSE(r.player.pausedByComputer());
  r.player.togglePlayPause();  // the listener's own pause: not the computer's
  r.player.pauseByComputer();  // ... and stays theirs
  TEST_ASSERT_FALSE(r.player.pausedByComputer());
  TEST_ASSERT_FALSE(r.player.pausedNotByListener());

  // Waiting for the headphones: ends paused, and nothing played meanwhile.
  TestHold hold;
  r.player.setHold(&hold);
  hold.hold = true;
  r.player.play(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
  const int plays = r.audio.playCount;
  r.player.pauseByComputer();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_TRUE(r.player.pausedByComputer());
  hold.hold = false;
  r.player.release();  // (a release after it: nothing waits any more)
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(plays, r.audio.playCount);
  r.player.stop();  // stopped: no mark
  TEST_ASSERT_FALSE(r.player.pausedByComputer());

  // The sleep timer's pause stays the timer's.
  r.player.play(0);
  r.player.pauseByTimer();
  r.player.pauseByComputer();
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_FALSE(r.player.pausedByComputer());
  TEST_ASSERT_TRUE(r.player.pausedNotByListener());
}

// ---- start points (the resume point after a boot) ----

// Set while stopped (as QueueStore does at boot): nothing plays by itself;
// the next play starts there, once; Now Playing reads it meanwhile.
void test_a_start_point_waits_for_the_next_play() {
  Rig r(3);
  r.queue.setCurrent(1);
  r.player.setStartPoint(83000, 240000);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(83000, ms);
  TEST_ASSERT_EQUAL_UINT32(240000, dur);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));  // what QueueSaver keeps
  TEST_ASSERT_EQUAL_UINT32(83000, ms);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(83000, r.audio.lastStartMs);
  TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));  // playing: nothing to save
  // Played again later (a tap on it): from its start.
  r.player.play(1);
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
}

// Next, another entry, or an edit that changes the current entry drops it;
// prev on it goes to 0:00 of the same entry, and nothing starts (as a
// paused track's restart).
void test_a_start_point_belongs_to_its_entry() {
  {
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0);
    r.player.next();
    TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  }
  {
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0);
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());  // the same entry
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
    uint32_t ms, dur;
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    r.player.togglePlayPause();  // a play: from 0:00
    TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    r.player.stop();
    r.player.prev();  // then prev as ever (stopped: the entry before, and it plays)
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  }
  {
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0);
    r.player.play(2);  // another entry
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    r.player.stop();
    r.queue.setCurrent(1);  // back to it: the point was dropped
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  }
  {
    // The current entry removed: its point goes with it.
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0);
    const uint32_t pos = 1;
    r.player.remove(&pos, 1);
    uint32_t ms, dur;
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    // An edit that leaves it current (an entry before it removed) keeps it.
    r.player.setStartPoint(5000, 0);
    const uint32_t first = 0;
    r.player.remove(&first, 1);
    TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_EQUAL_UINT32(5000, ms);
  }
  {
    // The headphones' cues while stopped: prev on it goes to 0:00 and
    // stays stopped; next moves on.
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0);
    r.player.cuePrev();
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
    uint32_t ms, dur;
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    r.player.setStartPoint(83000, 0);
    r.player.cueNext();
    TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  }
}

// Set while playing (the console's qs): it starts there now. Set while
// paused: the held track is let go, and play starts there.
void test_a_start_point_while_playing_or_paused() {
  Rig r(2);
  r.player.play(0);
  r.player.setStartPoint(30000, 0);
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(30000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  r.player.togglePlayPause();  // paused on the held track
  r.player.setStartPoint(60000, 0);
  TEST_ASSERT_EQUAL_INT(1, r.audio.stopCount);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT(3, r.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(60000, r.audio.lastStartMs);
  // 0: none.
  r.player.stop();
  r.player.setStartPoint(60000, 0);
  r.player.setStartPoint(0, 0);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
}

// A dropped start point stays dropped: the current entry removed (or the
// queue cleared), then an undo that brings the entry back, then prev or a
// tap on it: from 0:00, and nothing for QueueSaver to save again.
void test_a_dropped_start_point_doesnt_come_back_with_an_undo() {
  uint32_t ms = 0, dur = 0;
  for (int how = 0; how < 3; ++how) {
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(130000, 240000);
    if (how < 2) {
      const uint32_t pos = 1;
      r.player.remove(&pos, 1);  // current: the old entry 2 (now at 1)
      TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
      TEST_ASSERT_TRUE(r.player.undo());  // the entry back, still current elsewhere
      TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
      if (how == 0) {
        r.player.prev();  // to the restored entry
      } else {
        r.player.play(1);  // a tap on it
      }
      TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
      TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    } else {
      r.player.clearQueue();
      TEST_ASSERT_TRUE(r.player.undo());
      TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
      TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
      TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
      r.player.togglePlayPause();
      TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    }
  }
  {
    // The last entry removed while current (stopped on the new last one),
    // then an undo: the same.
    Rig r(3);
    r.queue.setCurrent(2);
    r.player.setStartPoint(130000, 240000);
    const uint32_t pos = 2;
    r.player.remove(&pos, 1);
    TEST_ASSERT_TRUE(r.player.undo());
    r.queue.setCurrent(2);
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  }
}

// The start point's length: the one given; else a waiting start point's,
// the held track's (the console's qs while paused), the one told for the
// entry (lengthHint(): stopped or cued, nothing held), or the catalog's
// hint. It goes to the backend with the play (a VBR file without a table
// of contents is placed by it); a play from 0:00 gets the catalog's hint.
void test_a_start_point_keeps_its_length() {
  uint32_t ms = 0, dur = 0;
  Rig r(2);
  r.player.play(0);
  r.audio.duration = 245000;
  r.audio.position = 60000;
  r.player.togglePlayPause();
  r.player.setStartPoint(120000, 0);  // qs120, paused
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(120000, ms);
  TEST_ASSERT_EQUAL_UINT32(245000, dur);
  r.audio.duration = 0;               // (the backend let it go)
  r.player.setStartPoint(90000, 0);   // qs again: the waiting one's length
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(245000, dur);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(90000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(245000, r.audio.lastHintMs);
  r.player.play(0);                   // from 0:00: the catalog's hint (none for a file)
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastHintMs);
  // Stopped, nothing held: the length told for the entry (its play from
  // the start point above), as the bar shows it meanwhile.
  r.player.stop();
  TEST_ASSERT_EQUAL_UINT32(245000, r.player.lengthHint());
  r.player.setStartPoint(30000, 0);
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(245000, dur);
  // An entry with no length told: the catalog's hint (none for a file).
  r.player.play(1);
  r.player.stop();
  r.player.setStartPoint(30000, 0);
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(0, dur);
}

// What QueueSaver saves: a paused track's position (the backend holds it),
// none while playing, stopped at 0:00, or cued.
void test_the_resume_point_is_a_paused_tracks_position() {
  Rig r(2);
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));  // stopped
  r.player.play(0);
  r.audio.position = 42500;
  r.audio.duration = 200000;
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));  // playing
  r.player.togglePlayPause();
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(42500, ms);
  TEST_ASSERT_EQUAL_UINT32(200000, dur);
  r.player.cueNext();  // the next entry, cued at 0:00
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
}

// The console's tests that borrow the backend (Rt, Rb, b<n>) stop the
// player keeping the listener's place: a paused or playing track's second
// becomes a start point, so QueueSaver keeps it and the next play picks up
// there; nothing plays by itself.
void test_stop_keeping_place_keeps_a_paused_tracks_second() {
  Rig r(2);
  r.player.play(1);
  r.audio.position = 1380000;  // 23:00 into an audiobook
  r.audio.duration = 3600000;
  r.player.togglePlayPause();
  TEST_ASSERT_TRUE(r.player.stopKeepingPlace());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_FALSE(r.audio.playing);
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));  // what QueueSaver keeps
  TEST_ASSERT_EQUAL_UINT32(1380000, ms);
  TEST_ASSERT_EQUAL_UINT32(3600000, dur);
  r.audio.position = 0;  // the test's own track came and went
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);  // nothing started
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(1380000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(3600000, r.audio.lastHintMs);
}

void test_stop_keeping_place_keeps_a_playing_tracks_second() {
  Rig r(2);
  r.player.play(0);
  r.audio.position = 61000;
  r.audio.duration = 200000;
  TEST_ASSERT_TRUE(r.player.stopKeepingPlace());
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(61000, ms);
  TEST_ASSERT_EQUAL_UINT32(200000, dur);
  // A start the backend hasn't taken up yet: where it was asked to start
  // (its length from the catalog, as after a boot).
  Rig q(2);
  q.audio.asyncStarts = true;
  q.player.play(0);
  q.audio.take();
  q.player.setStartPoint(90000, 0);  // playing: starts there now (pending)
  q.audio.position = 5;              // still the last start's
  TEST_ASSERT_TRUE(q.player.stopKeepingPlace());
  TEST_ASSERT_TRUE(q.player.resumePoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(90000, ms);
}

void test_stop_keeping_place_has_nothing_to_keep() {
  uint32_t ms = 0, dur = 0;
  {
    Rig r(2);  // stopped: at 0:00 anyway
    TEST_ASSERT_FALSE(r.player.stopKeepingPlace());
    TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
  }
  {
    Rig r(2);  // a failed track has no place
    r.player.play(0);
    r.audio.position = 30000;
    r.audio.failedFlag = true;
    TEST_ASSERT_FALSE(r.player.stopKeepingPlace());
    TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
  }
  {
    Rig r(2);  // cued: the next entry at 0:00
    r.player.play(0);
    r.audio.position = 30000;
    r.player.togglePlayPause();
    r.player.cueNext();
    TEST_ASSERT_FALSE(r.player.stopKeepingPlace());
    TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
  }
  {
    Rig r(2);  // a start point waiting (after a boot) stays as it was
    r.player.setStartPoint(45000, 120000);
    TEST_ASSERT_TRUE(r.player.stopKeepingPlace());
    TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));
    TEST_ASSERT_EQUAL_UINT32(45000, ms);
    TEST_ASSERT_EQUAL_UINT32(120000, dur);
  }
}

// ---- prev: this track again past 3 s, else the one before ----

void test_the_prev_rule() {
  using P = PlaybackController::Prev;
  const uint32_t k = PlaybackController::kRestartAfterMs;
  for (PlayState st : {PlayState::Playing, PlayState::Paused, PlayState::Waiting}) {
    TEST_ASSERT_EQUAL(P::Previous, PlaybackController::prevRule(st, false, true, 0));
    TEST_ASSERT_EQUAL(P::Previous, PlaybackController::prevRule(st, false, true, k));  // 3 s or less
    TEST_ASSERT_EQUAL(P::Restart, PlaybackController::prevRule(st, false, true, k + 1));
    TEST_ASSERT_EQUAL(P::Restart, PlaybackController::prevRule(st, false, true, 3600000));
    TEST_ASSERT_EQUAL(P::Previous, PlaybackController::prevRule(st, false, false, 200000));  // not known
    TEST_ASSERT_EQUAL(P::Restart, PlaybackController::prevRule(st, true, true, 0));  // a start point: its 0:00
  }
  // Stopped: the entry before, whatever the backend says; a start point
  // waiting (after a boot): its entry's 0:00, whatever the second.
  TEST_ASSERT_EQUAL(P::Previous, PlaybackController::prevRule(PlayState::Stopped, false, true, 200000));
  TEST_ASSERT_EQUAL(P::Restart, PlaybackController::prevRule(PlayState::Stopped, true, true, 0));
  TEST_ASSERT_EQUAL(P::Restart, PlaybackController::prevRule(PlayState::Stopped, true, false, 2000));
}

// Playing past 3 s: the same entry from 0:00, playing (a start like any:
// the backend fades it in). At 3 s or less: the entry before.
void test_prev_while_playing_past_3_s_restarts_the_track() {
  Rig r(3);
  r.player.play(1);
  r.audio.position = 3001;
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.position);
  r.player.prev();  // within the first 3 s: the entry before
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.audio.lastPath.c_str());
  r.audio.position = PlaybackController::kRestartAfterMs;  // 3 s exactly: still the entry before
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());  // (wrapped)
}

// Paused past 3 s: 0:00 of the same entry, still paused. Nothing starts
// (the speaker: nobody may be listening at that level); the backend lets
// the track go, so Now Playing reads 0:00 and there is no resume point to
// save. A play then starts it from its beginning.
void test_prev_while_paused_past_3_s_goes_to_0_and_stays_paused() {
  Rig r(3);
  r.player.play(1);
  r.audio.position = 95000;
  r.audio.duration = 240000;
  r.player.togglePlayPause();
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);  // nothing started
  TEST_ASSERT_EQUAL_INT(1, r.audio.stopCount);  // the paused track let go
  TEST_ASSERT_FALSE(r.audio.playing);
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));  // at 0:00: nothing to pick up in
  TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  r.player.update(0);  // nothing moves while paused
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  r.player.togglePlayPause();  // not a resume at 1:35: the entry from its start
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
}

// A second prev on the paused track now at 0:00 (cued: whatever the
// backend last said): the entry before, as prev at 3 s or less always did
// (it plays).
void test_a_second_prev_while_paused_goes_to_the_entry_before() {
  Rig r(3);
  r.player.play(1);
  r.audio.position = 95000;
  r.player.togglePlayPause();
  r.player.prev();
  TEST_ASSERT_EQUAL(PlaybackController::Prev::Previous, r.player.prevAction());
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.audio.lastPath.c_str());
  // Paused within the first 3 s: the entry before, as ever.
  r.audio.position = 2000;
  r.player.togglePlayPause();
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
}

// The headphones' PREV while paused (cuePrev()): the same rule, and still
// nothing starts: past 3 s, 0:00 of the same entry; then the entry before,
// cued.
void test_cue_prev_while_paused_past_3_s_restarts_paused() {
  Rig r(3);
  r.player.play(1);
  r.audio.position = 95000;
  r.player.togglePlayPause();
  r.player.cuePrev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_FALSE(r.audio.playing);
  r.player.cuePrev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  // Playing, the headphones' PREV is prev(): past 3 s, from 0:00.
  r.player.togglePlayPause();
  r.audio.position = 60000;
  r.player.cuePrev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(3, r.audio.playCount);
}

// Stopped (a track's position means nothing then): the entry before, as
// ever, and it plays.
void test_prev_while_stopped_goes_to_the_entry_before() {
  Rig r(3);
  r.player.play(1);
  r.audio.position = 95000;
  r.player.stop();
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

// The first entry at 3 s or less: as ever (with repeat, the last entry;
// without, the first again from its start). Past 3 s: the first from 0:00.
void test_prev_on_the_first_entry() {
  {
    Rig r(3);
    r.player.play(0);
    r.audio.position = 1500;
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  }
  {
    Rig r(3);
    r.player.setRepeat(PlaybackController::Repeat::Off);
    r.player.play(0);
    r.audio.position = 1500;
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
    r.audio.position = 50000;
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT(3, r.audio.playCount);
  }
  {
    // A queue of one: the same either way.
    Rig r(1);
    r.player.play(0);
    r.audio.position = 50000;
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT(3, r.audio.playCount);
  }
}

// The position isn't known while the backend hasn't taken a start up: it
// may still count the track before. A quick second prev after a restart
// (or a prev right after a skip) then goes to the entry before, not back
// to the start of the one just asked for.
void test_prev_before_the_backend_takes_a_start_goes_to_the_entry_before() {
  Rig r(3);
  r.audio.asyncStarts = true;
  r.player.play(2);
  r.audio.take();
  r.audio.position = 200000;
  r.player.prev();  // restart: the backend still counts 3:20 for a moment
  TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(200000, r.audio.position);
  r.player.prev();  // the second press: the entry before
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  r.audio.take();
  r.audio.position = 5000;  // once it counts this track's: the rule as ever
  TEST_ASSERT_EQUAL(PlaybackController::Prev::Restart, r.player.prevAction());
  // After next near a track's end, the same.
  r.audio.position = 230000;
  r.player.next();
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
}

// A start part of the way in (a resume point after a boot, qs while
// playing) is that far in from the moment it is asked for, as Now Playing
// shows it: a prev before the backend takes it up restarts the entry
// (the backend's position is still the track before's, near its start).
// The restart's own start is at 0:00: a second quick prev is the entry
// before.
void test_prev_right_after_a_start_part_of_the_way_in_restarts() {
  {
    Rig r(3);
    r.audio.asyncStarts = true;
    r.queue.setCurrent(2);
    r.player.setStartPoint(150000, 240000);  // the resume point, after a boot
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_UINT32(150000, r.audio.lastStartMs);
    TEST_ASSERT_FALSE(r.audio.positionKnown());
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.position);  // nothing of it counted yet
    TEST_ASSERT_EQUAL(PlaybackController::Prev::Restart, r.player.prevAction());
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(2, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    r.player.prev();  // the restart not taken up yet: at 0:00, the entry before
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  }
  {
    Rig r(3);
    r.audio.asyncStarts = true;
    r.player.play(0);
    r.audio.take();
    r.audio.position = 1200;
    r.player.setStartPoint(83000, 0);  // qs while playing: there, now
    TEST_ASSERT_EQUAL_UINT32(83000, r.audio.lastStartMs);
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    TEST_ASSERT_EQUAL_INT(3, r.audio.playCount);
  }
  {
    // Paused before the backend took it up: 0:00 of the entry, still paused.
    Rig r(3);
    r.audio.asyncStarts = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(60000, 240000);
    r.player.togglePlayPause();
    r.player.togglePlayPause();
    r.player.cuePrev();
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    TEST_ASSERT_FALSE(r.audio.playing);
  }
}

// A track that failed (a file that won't open) has no place to go back
// to, whatever its position says (a resume point it was asked to start
// at): prev goes to the entry before.
void test_prev_on_a_failed_track_goes_to_the_entry_before() {
  Rig r(3);
  r.player.play(2);
  r.audio.position = 83000;
  r.audio.failedFlag = true;
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
}

// The built-in tracks (tones, click tracks: their position is counted as
// any track's) follow the same rule.
void test_prev_restarts_a_builtin_track() {
  Rig r(0);
  const LibraryIndex::Span b = TrackCatalog::builtins();
  r.player.playNow(b.ids, b.count, 4);
  r.audio.position = 20000;
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(4, r.player.currentIndex());
  TEST_ASSERT_EQUAL_STRING("tone:click120", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(3, r.player.currentIndex());
}

// Waiting for the headphones: resuming a paused track past 3 s, prev
// makes it wait for the entry's 0:00 (the backend lets the track go);
// waiting on a cued entry, prev is the entry before, still waiting.
// Nothing plays until the wait is released.
void test_prev_while_waiting() {
  Rig r(3);
  TestHold hold;
  r.player.setHold(&hold);
  r.player.play(1);
  r.audio.position = 95000;
  r.player.togglePlayPause();
  hold.hold = true;
  r.player.togglePlayPause();  // waits to resume the paused track
  TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
  TEST_ASSERT_FALSE(r.audio.playing);
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  r.player.prev();  // cued at 0:00: the entry before, still waiting
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  hold.hold = false;
  r.player.release();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
}

// A restart is no skip, no end and no edit: "pause after this track"
// stays for the track's natural end (then the next entry, paused), and
// the timer's count of boundary pauses doesn't move.
void test_a_restart_keeps_the_sleep_timers_pause_after_this_track() {
  Rig r(3);
  r.player.play(0);
  r.player.setPauseAfterTrack(true);
  r.audio.position = 230000;
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  TEST_ASSERT_EQUAL_UINT32(0, r.player.timerStops());
  r.player.update(0);  // not an end
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_UINT32(0, r.player.timerStops());
  r.audio.finish();
  r.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(1, r.player.timerStops());
  // Paused by the timer, a restart keeps the mark (nothing played).
  Rig q(3);
  q.player.play(0);
  q.audio.position = 60000;
  q.player.pauseByTimer();
  q.player.cuePrev();
  TEST_ASSERT_TRUE(q.player.pausedByTimer());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)q.player.state());
  TEST_ASSERT_EQUAL_INT(0, q.player.currentIndex());
}

// The queue's undo: a restart doesn't touch the queue, so the last edit
// is still the one undone, and undoing it leaves the restarted entry
// playing (it was in the queue then).
void test_a_restart_leaves_the_queues_undo_alone() {
  Rig r(3);
  r.player.play(1);
  const uint32_t more[] = {0, 2};
  TEST_ASSERT_TRUE(r.player.addToQueue(more, 2));
  const QueueModel::Edit edit = r.queue.undoable();
  r.audio.position = 60000;
  r.player.prev();
  TEST_ASSERT_EQUAL((int)edit, (int)r.queue.undoable());
  TEST_ASSERT_EQUAL_UINT32(5, r.queue.size());
  const int plays = r.audio.playCount;
  TEST_ASSERT_TRUE(r.player.undo());
  TEST_ASSERT_EQUAL_UINT32(3, r.queue.size());
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT(plays, r.audio.playCount);  // what plays carries on
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
}

// A start point waiting (the resume point after a boot, qs): prev goes to
// the entry's 0:00 whatever the second (2 s too), and starts nothing,
// stopped or paused; a second prev is the entry before.
void test_prev_on_a_start_point_goes_to_0_and_starts_nothing() {
  uint32_t ms = 0, dur = 0;
  {
    Rig r(3);
    r.queue.setCurrent(1);
    r.player.setStartPoint(2000, 240000);
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  }
  {
    Rig r(3);
    r.player.play(1);
    r.player.togglePlayPause();
    r.player.setStartPoint(83000, 0);  // qs while paused: the held track let go
    r.player.prev();
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  }
}

// ---- gapless playback: the word on what follows, the heard advance ----

// The word names what advance() would start: the next entry; none at the
// end without repeat; the same entry in a queue of one with repeat; none
// while stopped (nothing is sent), with "pause after this track", with the
// gate shut, or with gapless off.
void test_gapless_the_word_is_what_advance_would_start() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.update(0);
  TEST_ASSERT_TRUE(a.nexts.empty());  // stopped: nothing
  p.play(0);
  TEST_ASSERT_EQUAL_UINT32(1, a.nexts.size());
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().after);
  TEST_ASSERT_TRUE(a.nexts.back().token != 0);
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.nexts.back().path.c_str());
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, a.nexts.size());  // unchanged: not sent again
  p.play(2);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", a.nexts.back().path.c_str());  // repeat: wraps
  p.setRepeat(PlaybackController::Repeat::Off);
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);  // the end: nothing follows
  p.setRepeat(PlaybackController::Repeat::All);
  p.setPauseAfterTrack(true);
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);
  p.setPauseAfterTrack(false);
  p.update(0);
  TEST_ASSERT_TRUE(a.nexts.back().token != 0);
  p.setGapless(false);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);
  p.setGapless(true);
  TEST_ASSERT_TRUE(a.nexts.back().token != 0);
  struct Shut : PlaybackController::NextGate {
    bool endsHere() const override { return true; }
  } shut;
  p.setNextGate(&shut);
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);
  p.setNextGate(nullptr);
  Rig one(1);
  one.player.play(0);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", one.audio.nexts.back().path.c_str());  // itself, repeated
}

// The same track still next keeps its token (no cut); another track, or
// a new play(), gets a new one.
void test_gapless_tokens() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  const uint32_t t1 = a.nexts.back().token;
  const uint32_t c = 2;
  p.addToQueue(&c, 1);  // a, b, c, c: b still next
  TEST_ASSERT_EQUAL_UINT32(t1, a.nexts.back().token);
  p.playNext(&c, 1);  // a, c, b, c, c
  TEST_ASSERT_TRUE(a.nexts.back().token != t1);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  const uint32_t t2 = a.nexts.back().token;
  p.play(0);  // a new request: a new token for the same next track
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  TEST_ASSERT_TRUE(a.nexts.back().token != t2);
}

// A heard advance moves the entry without a play(), keeps the state,
// drops the start point and names the next one after it.
void test_gapless_an_advance_moves_the_entry_without_a_play() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  const uint32_t t1 = a.nexts.back().token;
  a.advances.push_back(t1);
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_UINT32(t1, a.nexts.back().after);  // the word after b
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, p.gaplessStats().adopted);
}

// An advance that isn't what comes next any more (an edit too late to cut
// it out): what advance() would start is started.
void test_gapless_a_stale_advance_starts_what_comes_next() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  const uint32_t t1 = a.nexts.back().token;  // b
  const uint32_t c = 2;
  p.playNext(&c, 1);  // a, c, b, c
  a.advances.push_back(t1);
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());  // c (entry 1)
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, p.gaplessStats().restarted);
}

// An advance comes before an action's own work: a next just after a join
// skips the joined track.
void test_gapless_actions_take_the_advance_first() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.advances.push_back(a.nexts.back().token);
  p.next();
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
}

// With "pause after this track" at the advance (chosen too late to cut
// the joined track out): the boundary's pause, the joined entry cued.
void test_gapless_an_advance_with_pause_after_pauses_at_once() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  const uint32_t t1 = a.nexts.back().token;
  p.setPauseAfterTrack(true);
  a.advances.push_back(t1);
  const int stops = a.stopCount;
  p.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT(stops + 1, a.stopCount);
  TEST_ASSERT_TRUE(p.pausedByTimer());
  TEST_ASSERT_FALSE(p.pauseAfterTrack());
}

// A failure after an advance is the joined entry's: the note names it.
void test_gapless_a_failure_after_an_advance_is_the_new_entry_s() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(0);
  a.advances.push_back(a.nexts.back().token);
  p.update(0);
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().track);  // b's id
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());          // skipped to c
}

// ---- repeat and shuffle (docs/QUEUE-MODES.md) ----

using Repeat = PlaybackController::Repeat;

// One: the entry again at its natural end (a play of the same key, gapless
// off), counted.
void test_repeat_one_plays_the_entry_again() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.setGapless(false);
  p.setRepeat(Repeat::One);
  TEST_ASSERT_EQUAL_INT((int)Repeat::One, (int)p.repeat());
  p.play(1);
  const uint32_t key = r.queue.currentKey();
  a.finish();
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(key, r.queue.currentKey());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_INT(2, a.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, a.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(1, p.repeats());
  a.finish();
  p.update(0);
  TEST_ASSERT_EQUAL_INT(3, a.playCount);
  TEST_ASSERT_EQUAL_UINT32(2, p.repeats());
  // Paused at the loop's start, nothing more.
  p.update(0);
  TEST_ASSERT_EQUAL_INT(3, a.playCount);
}

// With One, next and prev still move, and wrap at the queue's ends as
// All's do.
void test_repeat_one_next_and_prev_move_and_wrap() {
  Rig r(3);
  PlaybackController& p = r.player;
  p.setRepeat(Repeat::One);
  p.play(2);
  p.next();
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  p.prev();
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  p.cueNext();  // (playing: next)
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(0, p.repeats());  // no skip is a loop
}

// A failure is a skip: it moves on even with One; every track failing in
// a row still stops.
void test_repeat_one_moves_on_from_a_failure() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.setRepeat(Repeat::One);
  p.play(0);
  a.failedFlag = true;
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  Rig all(2);
  all.player.setRepeat(Repeat::One);
  all.audio.failEverything = true;
  all.player.play(0);
  all.player.update(0);
  TEST_ASSERT_EQUAL_INT(1, all.player.currentIndex());
  all.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)all.player.state());
  TEST_ASSERT_EQUAL_INT(2, all.audio.playCount);
}

// A natural end with the position still at 0:00 is an empty track (no
// samples to play: an MP3 that is all delay and padding, a FLAC of 0
// samples; an empty Opus file is refused at its open): a failure, noted
// "no audio in it", so One moves on instead of starting it again at once,
// round and round; alone in the queue it stops, as every track failing
// in a row does; a track that ends a second in loops as ever.
void test_an_end_at_0_00_is_a_failure() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.setRepeat(Repeat::One);
  p.play(0);
  a.finishedFlag = true;  // (not finish(): the position stays at 0:00)
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
  TEST_ASSERT_EQUAL_UINT32(0, p.lastFailure().track);
  TEST_ASSERT_EQUAL_STRING("no audio in it", p.lastFailure().note);
  TEST_ASSERT_EQUAL_UINT32(0, p.repeats());  // a skip, not a loop
  a.finish();  // b ends a second in: One plays it again
  p.update(0);
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_EQUAL_INT(3, a.playCount);
  TEST_ASSERT_EQUAL_UINT32(1, p.repeats());
  TEST_ASSERT_EQUAL_UINT32(1, p.lastFailure().count);
  Rig one(1);
  one.player.setRepeat(Repeat::One);
  one.player.play(0);
  one.audio.finishedFlag = true;
  one.player.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)one.player.state());
  TEST_ASSERT_EQUAL_INT(1, one.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(1, one.player.lastFailure().count);
  one.player.update(0);
  TEST_ASSERT_EQUAL_INT(1, one.audio.playCount);
}

// The sleep timer wins: "pause after this track" with One cues this same
// entry at 0:00, paused, the timer's; a later play starts it from the top.
void test_repeat_one_with_pause_after_this_track() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.setRepeat(Repeat::One);
  p.play(1);
  p.setPauseAfterTrack(true);
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);  // no self-join decoded ahead
  a.position = 200000;
  a.finish();
  p.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());
  TEST_ASSERT_TRUE(p.pausedByTimer());
  TEST_ASSERT_FALSE(p.pauseAfterTrack());
  TEST_ASSERT_EQUAL_UINT32(1, p.timerStops());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);  // nothing started
  TEST_ASSERT_FALSE(a.playing);           // let go: cued
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_FALSE(p.resumePoint(&ms, &dur));  // at 0:00, nothing to resume in
  TEST_ASSERT_EQUAL_UINT32(0, p.repeats());
  p.togglePlayPause();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(0, a.lastStartMs);
  TEST_ASSERT_FALSE(p.pausedByTimer());
}

// A start point with no length said (the console's qs) on an entry the
// sleep timer's end-of-track pause cued, Repeat One's (the device check,
// QUEUE-MODES.md section 10 step 14): it takes the length told for the
// entry, which the bar shows meanwhile, so the bar stays live; the
// catalog's hint (0 for a library track) left it inert until a play. An
// entry with no length told keeps the catalog's hint, as before.
void test_a_start_point_on_a_cued_entry_keeps_its_told_length() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.setRepeat(Repeat::One);
  p.play(1);
  a.duration = 207000;
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, p.seek(r.queue.keyAt(1), 201000, 207000));
  p.setPauseAfterTrack(true);
  a.position = 207000;
  a.finish();
  p.update(0);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)p.state());
  TEST_ASSERT_EQUAL_INT(1, p.currentIndex());  // the same entry, cued at 0:00
  a.duration = 0;                              // (let go: the backend knows no length)
  a.position = 0;
  uint32_t pos = 1, len = 1;
  p.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(207000, len);  // live before the start point
  p.setStartPoint(60000, 0);              // qs60
  p.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(60000, pos);
  TEST_ASSERT_EQUAL_UINT32(207000, len);  // and after it
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, p.seek(r.queue.keyAt(1), 90000, len));
  p.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(90000, a.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(207000, a.lastHintMs);
  // Nothing told for the entry (the next one, never played): the catalog's.
  Rig s(3);
  s.player.setStartPoint(60000, 0);
  s.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(60000, pos);
  TEST_ASSERT_EQUAL_UINT32(0, len);
}

// The word at the last entry: Off nothing, All the first entry, One the
// entry itself, with a new token each loop (never the heard one).
void test_the_word_for_each_repeat_mode() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(2);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", a.nexts.back().path.c_str());  // All (the default)
  p.setRepeat(Repeat::Off);
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);
  p.setRepeat(Repeat::One);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  const uint32_t t1 = a.nexts.back().token;
  TEST_ASSERT_TRUE(t1 != 0);
  // The loop heard: the same entry stays current, no play(), a new word.
  a.advances.push_back(t1);
  p.update(0);
  TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
  TEST_ASSERT_EQUAL_INT(1, a.playCount);
  TEST_ASSERT_EQUAL_UINT32(1, p.gaplessStats().adopted);
  TEST_ASSERT_EQUAL_UINT32(1, p.repeats());
  TEST_ASSERT_EQUAL_UINT32(t1, a.nexts.back().after);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  const uint32_t t2 = a.nexts.back().token;
  TEST_ASSERT_TRUE(t2 != 0 && t2 != t1);
  a.advances.push_back(t2);
  p.update(0);
  TEST_ASSERT_EQUAL_UINT32(2, p.repeats());
  TEST_ASSERT_TRUE(a.nexts.back().token != t2);
  // Mid-queue: One's word is the entry itself, All's the next.
  p.play(0);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", a.nexts.back().path.c_str());
  p.setRepeat(Repeat::All);
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", a.nexts.back().path.c_str());
}

// setRepeat() is an action: the word changes before any update().
void test_set_repeat_is_an_action() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(2);
  const size_t words = a.nexts.size();
  p.setRepeat(Repeat::Off);
  TEST_ASSERT_EQUAL_UINT32(words + 1, a.nexts.size());
  TEST_ASSERT_EQUAL_UINT32(0, a.nexts.back().token);
  p.setRepeat(Repeat::One);
  TEST_ASSERT_EQUAL_UINT32(words + 2, a.nexts.size());
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
  p.setRepeat(Repeat::One);  // the same: nothing new to say
  TEST_ASSERT_EQUAL_UINT32(words + 2, a.nexts.size());
}

// A shuffle toggle changes nothing that plays: playing, paused, waiting,
// stopped with a start point. The same entry in the same state, no play or
// stop; the start point and the length kept; the word to the new next.
void test_set_shuffle_changes_nothing_that_plays() {
  for (int state = 0; state < 4; ++state) {
    Rig r(8);
    FakeAudioBackend& a = r.audio;
    PlaybackController& p = r.player;
    TestHold hold;
    p.setHold(&hold);
    uint32_t ms = 0, dur = 0;
    if (state == 3) {
      r.queue.setCurrent(2);
      p.setStartPoint(83000, 240000);  // stopped, waiting for the next play
    } else {
      if (state == 2) hold.hold = true;
      p.play(2);
      if (state == 1) p.togglePlayPause();
    }
    const PlayState st = p.state();
    const uint32_t key = r.queue.currentKey();
    const int plays = a.playCount, stops = a.stopCount;
    p.setShuffle(true);
    TEST_ASSERT_TRUE(p.shuffle());
    TEST_ASSERT_EQUAL_INT((int)st, (int)p.state());
    TEST_ASSERT_EQUAL_UINT32(key, r.queue.currentKey());
    TEST_ASSERT_EQUAL_INT(2, p.currentIndex());
    TEST_ASSERT_EQUAL_INT(plays, a.playCount);
    TEST_ASSERT_EQUAL_INT(stops, a.stopCount);
    if (state == 3) {
      TEST_ASSERT_TRUE(p.startPoint(&ms, &dur));
      TEST_ASSERT_EQUAL_UINT32(83000, ms);
      TEST_ASSERT_EQUAL_UINT32(240000, dur);
    }
    if (state == 0) {
      // The word names the new next entry at once.
      char want[32];
      r.catalog.path(r.queue.trackAt(3), want, sizeof(want));
      TEST_ASSERT_EQUAL_STRING(want, a.nexts.back().path.c_str());
    }
    p.setShuffle(false);
    TEST_ASSERT_FALSE(p.shuffle());
    TEST_ASSERT_EQUAL_UINT32(key, r.queue.currentKey());
    TEST_ASSERT_EQUAL_INT(2, p.currentIndex());  // (its own place)
    TEST_ASSERT_EQUAL_INT(plays, a.playCount);
    TEST_ASSERT_EQUAL_INT(stops, a.stopCount);
    if (state == 0) TEST_ASSERT_EQUAL_STRING("/music/d.mp3", a.nexts.back().path.c_str());
    if (state == 1) {
      p.togglePlayPause();  // resumes the same held track
      TEST_ASSERT_EQUAL_INT(plays, a.playCount);
      TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
    }
  }
}

// Off when the own order has the same next track: the word keeps its token
// (nothing is cut).
void test_shuffle_off_with_the_same_next_keeps_the_word() {
  Rig r(3);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  p.play(1);  // a [b] c: only c is up next, so on and off keep it next
  const uint32_t t = a.nexts.back().token;
  p.setShuffle(true);
  TEST_ASSERT_EQUAL_UINT32(t, a.nexts.back().token);
  p.setShuffle(false);
  TEST_ASSERT_EQUAL_UINT32(t, a.nexts.back().token);
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.nexts.back().path.c_str());
}

// Play while shuffled: the chosen track first, or (kAnyStart) a random
// one; not shuffled, kAnyStart is the first.
void test_play_now_while_shuffled() {
  Rig r(8);
  FakeAudioBackend& a = r.audio;
  PlaybackController& p = r.player;
  const uint32_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
  TEST_ASSERT_TRUE(p.playNow(ids, 8, PlaybackController::kAnyStart));
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", a.lastPath.c_str());
  p.setShuffle(true);
  TEST_ASSERT_TRUE(p.playNow(ids, 8, 5));
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_STRING("/music/f.mp3", a.lastPath.c_str());
  TEST_ASSERT_TRUE(p.playNow(ids, 8, PlaybackController::kAnyStart));
  TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
  const uint32_t first = r.queue.currentTrack();
  p.setShuffle(false);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(first), p.currentIndex());  // the given order around it
}

// Shuffle all (Ui::shuffleAll()): a Play that turns shuffle on, one edit;
// its Undo puts back the queue and the mode it found (docs/QUEUE-MODES.md
// section 2.6).
void test_shuffle_all_and_its_undo() {
  const uint32_t ids[] = {0, 1, 2, 3, 4, 5, 6, 7};
  {
    // Over a queue that plays, shuffle off.
    Rig r(8);
    FakeAudioBackend& a = r.audio;
    PlaybackController& p = r.player;
    p.play(2);
    TEST_ASSERT_TRUE(p.playNow(ids, 8, PlaybackController::kAnyStart, true));
    TEST_ASSERT_TRUE(p.shuffle());
    TEST_ASSERT_EQUAL_INT(0, p.currentIndex());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
    TEST_ASSERT_TRUE(p.undo());
    TEST_ASSERT_FALSE(p.shuffle());
    TEST_ASSERT_EQUAL_INT(2, p.currentIndex());  // the track that was current, in its own order
    TEST_ASSERT_EQUAL_STRING("/music/c.mp3", a.lastPath.c_str());
    TEST_ASSERT_EQUAL_STRING("/music/d.mp3", a.nexts.back().path.c_str());  // the word: the own order's next
    for (uint32_t i = 0; i < 8; ++i) TEST_ASSERT_EQUAL_UINT32(i, r.queue.trackAt(i));
  }
  {
    // From an empty queue (the empty states' button): empty, stopped and
    // off again, not "shuffle on" over nothing.
    Rig r(8);
    PlaybackController& p = r.player;
    p.clearQueue();
    TEST_ASSERT_TRUE(p.playNow(ids, 8, PlaybackController::kAnyStart, true));
    TEST_ASSERT_TRUE(p.shuffle());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)p.state());
    TEST_ASSERT_TRUE(p.undo());
    TEST_ASSERT_FALSE(p.shuffle());
    TEST_ASSERT_FALSE(p.hasTrack());
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)p.state());
  }
  {
    // Already shuffled: it stays on.
    Rig r(8);
    PlaybackController& p = r.player;
    p.setShuffle(true);
    TEST_ASSERT_TRUE(p.playNow(ids, 8, PlaybackController::kAnyStart, true));
    TEST_ASSERT_TRUE(p.undo());
    TEST_ASSERT_TRUE(p.shuffle());
  }
}

// ---- resume anchors (docs/SEEK.md section 5) ----

namespace {
ResumeAnchor anchorAt(uint64_t sample) {
  ResumeAnchor a;
  a.kind = ResumeAnchor::Kind::Mp3;
  a.exact = true;
  a.rate = 44100;
  a.sample = sample;
  a.fileSize = 5000000;
  a.prerollByte = 100000;
  a.frameByte = 103000;
  a.skip = 77;
  a.frameHash = 0xBEEF;
  return a;
}
}  // namespace

// A start point with an anchor (the resume point after a boot): it reaches
// the backend with the play, once; QueueSaver reads it back meanwhile.
void test_a_start_points_anchor_reaches_the_play_once() {
  Rig r(3);
  r.audio.anchorsOn = true;
  r.queue.setCurrent(1);
  const ResumeAnchor a = anchorAt(3660300);
  r.player.setStartPoint(83000, 240000, &a);
  uint32_t ms = 0, dur = 0;
  ResumeAnchor got;
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur, &got));
  TEST_ASSERT_TRUE(got == a);
  got = ResumeAnchor{};
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_TRUE(got == a);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(83000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(240000, r.audio.lastHintMs);
  TEST_ASSERT_TRUE(r.audio.lastAnchor == a);
  TEST_ASSERT_EQUAL_INT(1, r.audio.anchoredPlays);
  // Played again: from its start, no anchor.
  r.player.play(1);
  TEST_ASSERT_FALSE(r.audio.lastAnchor.valid());
  TEST_ASSERT_EQUAL_INT(1, r.audio.anchoredPlays);
}

// The anchor goes with its start point: next, another entry, an edit that
// changes the entry, prev's restart; a qs (a second without one) replaces
// an older anchor.
void test_a_start_points_anchor_goes_with_it() {
  const ResumeAnchor a = anchorAt(3660300);
  {
    Rig r(3);
    r.audio.anchorsOn = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0, &a);
    r.player.next();
    TEST_ASSERT_FALSE(r.audio.lastAnchor.valid());
    TEST_ASSERT_EQUAL_INT(0, r.audio.anchoredPlays);
  }
  {
    Rig r(3);
    r.audio.anchorsOn = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0, &a);
    r.player.play(2);
    TEST_ASSERT_EQUAL_INT(0, r.audio.anchoredPlays);
  }
  {
    Rig r(3);
    r.audio.anchorsOn = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0, &a);
    const uint32_t pos = 1;
    r.player.remove(&pos, 1);
    TEST_ASSERT_TRUE(r.player.undo());
    uint32_t ms, dur;
    ResumeAnchor got;
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur, &got));
    r.player.play(1);
    TEST_ASSERT_EQUAL_INT(0, r.audio.anchoredPlays);
  }
  {
    Rig r(3);
    r.audio.anchorsOn = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0, &a);
    r.player.setStartPoint(90000, 0);  // qs90: no anchor
    uint32_t ms, dur;
    ResumeAnchor got = a;
    TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur, &got));
    TEST_ASSERT_EQUAL_UINT32(90000, ms);
    TEST_ASSERT_FALSE(got.valid());
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_UINT32(90000, r.audio.lastStartMs);
    TEST_ASSERT_FALSE(r.audio.lastAnchor.valid());
  }
  {
    Rig r(3);
    r.audio.anchorsOn = true;
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, 0, &a);
    r.player.prev();  // its 0:00
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    TEST_ASSERT_EQUAL_INT(0, r.audio.anchoredPlays);
  }
}

// resumePoint(): while paused, the backend's anchor for the held track
// (none from a backend without anchors); while a start point waits, its
// own; playing: nothing.
void test_the_resume_point_carries_the_held_tracks_anchor() {
  Rig r(2);
  r.audio.anchorsOn = true;
  r.player.play(0);
  r.audio.position = 42500;
  r.audio.duration = 200000;
  r.audio.held = anchorAt(1874250);
  uint32_t ms = 0, dur = 0;
  ResumeAnchor got;
  TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur, &got));  // playing
  r.player.togglePlayPause();
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(42500, ms);
  TEST_ASSERT_TRUE(got == r.audio.held);
  r.audio.held = ResumeAnchor{};  // (G0, a built-in track: none)
  got = anchorAt(1);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_FALSE(got.valid());
  // A start point while paused (qs): its own, none.
  r.audio.held = anchorAt(1874250);
  r.player.setStartPoint(60000, 0);
  got = anchorAt(1);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(60000, ms);
  TEST_ASSERT_FALSE(got.valid());
}

// stopKeepingPlace() (the benches borrow the backend): the start point it
// sets keeps the backend's anchor, which the next play gets; none while
// the backend hasn't taken its start up yet.
void test_stop_keeping_place_keeps_the_backends_anchor() {
  {
    Rig r(2);
    r.audio.anchorsOn = true;
    r.player.play(0);
    r.audio.position = 42500;
    r.audio.held = anchorAt(1874250);
    r.player.togglePlayPause();
    TEST_ASSERT_TRUE(r.player.stopKeepingPlace());
    uint32_t ms = 0, dur = 0;
    ResumeAnchor got;
    TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur, &got));
    TEST_ASSERT_EQUAL_UINT32(42500, ms);
    TEST_ASSERT_TRUE(got == r.audio.held);
    r.player.togglePlayPause();
    TEST_ASSERT_TRUE(r.audio.lastAnchor == anchorAt(1874250));
  }
  {
    Rig r(2);
    r.audio.anchorsOn = true;
    r.audio.asyncStarts = true;
    r.audio.held = anchorAt(1874250);
    r.player.play(0);
    r.player.setStartPoint(30000, 0);  // playing: starts there (not taken up yet)
    TEST_ASSERT_TRUE(r.player.stopKeepingPlace());
    uint32_t ms = 0, dur = 0;
    ResumeAnchor got = anchorAt(1);
    TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur, &got));
    TEST_ASSERT_EQUAL_UINT32(30000, ms);
    TEST_ASSERT_FALSE(got.valid());
  }
}

// A backend without anchors (every fake but this one's anchorsOn): starts
// and resume points as before, by the millisecond.
void test_a_backend_without_anchors_is_as_before() {
  Rig r(2);
  const ResumeAnchor a = anchorAt(3660300);
  r.player.setStartPoint(83000, 240000, &a);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(83000, r.audio.lastStartMs);  // the 3-argument play()
  TEST_ASSERT_EQUAL_UINT32(240000, r.audio.lastHintMs);
  r.audio.position = 90000;
  r.player.togglePlayPause();
  uint32_t ms = 0, dur = 0;
  ResumeAnchor got = a;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(90000, ms);
  TEST_ASSERT_FALSE(got.valid());
}

// ---- Now Playing's seek bar: seek() (docs/SEEK-BAR.md section 5) ----

namespace {
constexpr uint32_t kL = 245000;  // 4:05
}  // namespace

// Playing: it starts there now (one play, with the bar's length as the
// hint); until the backend takes it up, where it was asked to start is the
// position (and prev restarts the entry).
void test_a_seek_while_playing_starts_there_now() {
  Rig r(2);
  r.player.play(0);
  r.audio.position = 30000;
  r.audio.duration = kL;
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(0), 90000, kL));
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(90000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(kL, r.audio.lastHintMs);
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_FALSE(r.player.pendingStart(&ms));  // (this fake takes every start at once)
  // A backend that takes the start up later (the Core2's decode task).
  Rig q(2);
  q.audio.asyncStarts = true;
  q.player.play(0);
  q.audio.take();
  q.audio.position = 30000;
  q.audio.duration = kL;
  TEST_ASSERT_FALSE(q.player.pendingStart(&ms));
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, q.player.seek(q.queue.keyAt(0), 90000, kL));
  TEST_ASSERT_TRUE(q.player.pendingStart(&ms));
  TEST_ASSERT_EQUAL_UINT32(90000, ms);
  TEST_ASSERT_EQUAL_UINT32(30000, q.audio.positionMs());  // still the run before's
  TEST_ASSERT_EQUAL(PlaybackController::Prev::Restart, q.player.prevAction());
  q.audio.take();
  TEST_ASSERT_FALSE(q.player.pendingStart(&ms));
  TEST_ASSERT_EQUAL_UINT32(90000, q.audio.positionMs());
}

// 0:00 is prev's restart in every state (a start point of 0 only clears
// one): playing, a play from 0:00; paused, still paused at 0:00, the held
// track let go, nothing to resume at; stopped on a start point, it goes.
void test_a_seek_to_0_is_prevs_restart_in_every_state() {
  uint32_t ms = 0, dur = 0;
  {
    Rig r(2);
    r.player.play(0);
    r.audio.position = 60000;
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(0), 0, kL));
    TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  }
  {
    Rig r(2);
    r.player.play(0);
    r.audio.position = 60000;
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 0, kL));
    TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(1, r.audio.stopCount);
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_FALSE(r.player.resumePoint(&ms, &dur));
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
    TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  }
  {
    Rig r(2);
    r.queue.setCurrent(1);
    r.player.setStartPoint(83000, kL);  // the resume point, after a boot
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(1), 0, kL));
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  }
  {
    // The contrast: setStartPoint(0) while playing starts nothing.
    Rig r(2);
    r.player.play(0);
    r.audio.position = 60000;
    r.player.setStartPoint(0, kL);
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  }
}

// Paused: the held track is let go once (the stop keeps the run's index),
// the state stays Paused, and the seek is the resume point (no anchor: the
// old one was the paused sample's), which QueueSaver saves in the same pass
// (its anchor changed). Play starts there, by the second.
void test_a_seek_while_paused_lets_the_track_go_and_is_the_resume_point() {
  Rig r(2);
  r.audio.anchorsOn = true;
  r.player.play(0);
  r.audio.position = 30000;
  r.audio.duration = kL;
  r.audio.held = anchorAt(1323000);
  r.player.togglePlayPause();
  uint32_t ms = 0, dur = 0;
  ResumeAnchor got;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(30000, ms);
  TEST_ASSERT_TRUE(got == r.audio.held);
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 120000, kL));
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.stopCount);
  got = anchorAt(1);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(120000, ms);
  TEST_ASSERT_EQUAL_UINT32(kL, dur);
  TEST_ASSERT_FALSE(got.valid());
  // A second paused seek: no second stop.
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 150000, kL));
  TEST_ASSERT_EQUAL_INT(1, r.audio.stopCount);
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 120000, kL));
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(120000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(kL, r.audio.lastHintMs);
  TEST_ASSERT_EQUAL_INT(0, r.audio.anchoredPlays);
}

// Waiting for the headphones: the start point waits with it; the release
// (they connected, or Play on speaker) starts there, and so does a play
// after the wait was cancelled.
void test_a_seek_while_waiting_starts_there_when_they_connect() {
  {
    Rig r(2);
    TestHold hold;
    r.player.setHold(&hold);
    hold.hold = true;
    r.player.play(0);
    TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 60000, kL));
    TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
    uint32_t ms = 0, dur = 0;
    TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
    TEST_ASSERT_EQUAL_UINT32(60000, ms);
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
    r.player.release();
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
    TEST_ASSERT_EQUAL_UINT32(60000, r.audio.lastStartMs);
  }
  {
    Rig r(2);
    TestHold hold;
    r.player.setHold(&hold);
    hold.hold = true;
    r.player.play(0);
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 60000, kL));
    r.player.cancelWait();
    TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
    hold.hold = false;
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
    TEST_ASSERT_EQUAL_UINT32(60000, r.audio.lastStartMs);
  }
  {
    // Playing when the hold comes (the headphones dropped): it waits there.
    Rig r(2);
    TestHold hold;
    r.player.setHold(&hold);
    r.player.play(0);
    hold.hold = true;
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 60000, kL));
    TEST_ASSERT_EQUAL_INT((int)PlayState::Waiting, (int)r.player.state());
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    hold.hold = false;
    r.player.release();
    TEST_ASSERT_EQUAL_UINT32(60000, r.audio.lastStartMs);
  }
}

// Stopped: the start point waits for the next play, with the bar's length.
void test_a_seek_while_stopped_waits() {
  Rig r(3);
  r.queue.setCurrent(1);
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(1), 60000, kL));
  TEST_ASSERT_EQUAL_INT((int)PlayState::Stopped, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  uint32_t ms = 0, dur = 0;
  TEST_ASSERT_TRUE(r.player.startPoint(&ms, &dur));
  TEST_ASSERT_EQUAL_UINT32(60000, ms);
  TEST_ASSERT_EQUAL_UINT32(kL, dur);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur));
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.audio.lastPath.c_str());
  TEST_ASSERT_EQUAL_UINT32(60000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(kL, r.audio.lastHintMs);
}

// The finger's entry isn't current any more: a join heard since (taken
// first, inside seek()), a skip, an empty queue. Nothing is done.
void test_a_seek_for_an_entry_that_moved_does_nothing() {
  uint32_t ms = 0, dur = 0;
  {
    Rig r(3);
    r.player.play(0);
    const uint32_t key0 = r.queue.keyAt(0);
    r.audio.advances.push_back(r.audio.nexts.back().token);  // the join is heard
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Moved, r.player.seek(key0, 120000, kL));
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  }
  {
    Rig r(3);
    r.player.play(0);
    const uint32_t key0 = r.queue.keyAt(0);
    r.player.next();
    TEST_ASSERT_EQUAL(PlaybackController::Seek::Moved, r.player.seek(key0, 120000, kL));
    TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
    TEST_ASSERT_EQUAL_INT(2, r.audio.playCount);
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  }
  {
    Rig r(0);
    TEST_ASSERT_EQUAL(PlaybackController::Seek::NoPlace, r.player.seek(QueueModel::kNone, 120000, kL));
    TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  }
}

// Never into the tail (a start in the last 5 s would go to 0:00): the
// length less 6 s, a whole second; a track of 6 s or less: its 0:00.
void test_a_seek_never_asks_the_tail() {
  Rig r(2);
  r.player.play(0);
  r.player.seek(r.queue.keyAt(0), 243000, kL);
  TEST_ASSERT_EQUAL_UINT32(239000, r.audio.lastStartMs);
  r.player.seek(r.queue.keyAt(0), 300000, kL);
  TEST_ASSERT_EQUAL_UINT32(239000, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_UINT32(239000, trackseek::seekLimitMs(kL));
  TEST_ASSERT_EQUAL_UINT32(kL - 239000 - 1000, trackseek::kTailMs);
  r.player.seek(r.queue.keyAt(0), 3000, 5000);
  TEST_ASSERT_EQUAL_UINT32(0, r.audio.lastStartMs);
  TEST_ASSERT_EQUAL_INT(4, r.audio.playCount);
  TEST_ASSERT_EQUAL_UINT32(0, trackseek::seekLimitMs(6000));
  TEST_ASSERT_EQUAL_UINT32(1000, trackseek::seekLimitMs(7999));
}

// A track that failed, or a length of 0 (the dotted line): nothing at all.
void test_a_seek_on_a_failed_or_unknown_length_track_does_nothing() {
  {
    Rig r(2);
    r.player.play(0);
    r.audio.failedFlag = true;  // (before the update that would skip it)
    TEST_ASSERT_EQUAL(PlaybackController::Seek::NoPlace, r.player.seek(r.queue.keyAt(0), 60000, kL));
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    TEST_ASSERT_EQUAL_INT(0, r.audio.stopCount);
  }
  {
    Rig r(2);
    r.player.play(0);
    r.player.togglePlayPause();
    TEST_ASSERT_EQUAL(PlaybackController::Seek::NoPlace, r.player.seek(r.queue.keyAt(0), 60000, 0));
    TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
    TEST_ASSERT_EQUAL_INT(0, r.audio.stopCount);
    uint32_t ms = 0, dur = 0;
    TEST_ASSERT_FALSE(r.player.startPoint(&ms, &dur));
  }
}

// The sleep timer's marks and "pause after this track" stay: a seek is no
// play from a pause, no skip and no end.
void test_a_seek_keeps_the_timers_marks_and_pause_after() {
  Rig r(3);
  r.player.play(0);
  r.player.setPauseAfterTrack(true);
  r.audio.position = 60000;
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(0), 120000, kL));
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  r.player.pauseByTimer();
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 30000, kL));
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_TRUE(r.player.pauseAfterTrack());
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Waits, r.player.seek(r.queue.keyAt(0), 0, kL));
  TEST_ASSERT_TRUE(r.player.pausedByTimer());
  TEST_ASSERT_EQUAL_UINT32(0, r.player.timerStops());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  // The computer's pause too (the USB visualizer).
  Rig q(2);
  q.player.play(0);
  q.player.pauseByComputer();
  q.player.seek(q.queue.keyAt(0), 30000, kL);
  TEST_ASSERT_TRUE(q.player.pausedByComputer());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)q.player.state());
}

// A seek is a listener's action: the count of failures in a row starts over
// (as after next or prev), so a queue that fails again isn't given up early.
void test_a_seek_resets_the_failures_in_a_row() {
  Rig r(2);
  r.player.play(0);
  r.audio.failedFlag = true;
  r.player.update(0);  // 0 failed: 1 plays (one failure in a row)
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(1), 60000, kL));
  r.audio.failedFlag = true;
  r.player.update(0);  // 1 failed: one in a row again, not two (which would stop)
  TEST_ASSERT_EQUAL_INT((int)PlayState::Playing, (int)r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
}

// lengthHint() belongs to its entry: another entry current, none; the same
// entry again (its track never changes), the same length.
void test_the_length_hint_follows_its_entry() {
  Rig r(3);
  r.player.play(0);
  TEST_ASSERT_EQUAL_UINT32(0, r.player.lengthHint());
  r.audio.position = 60000;
  r.player.seek(r.queue.keyAt(0), 120000, kL);
  TEST_ASSERT_EQUAL_UINT32(kL, r.player.lengthHint());
  r.player.next();
  TEST_ASSERT_EQUAL_UINT32(0, r.player.lengthHint());
  r.player.prev();  // (at 0:00: the entry before)
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_UINT32(kL, r.player.lengthHint());
  // A play's hint (a resume point's length) is noted too.
  r.player.stop();
  r.queue.setCurrent(2);
  r.player.setStartPoint(30000, 180000);
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL_UINT32(180000, r.player.lengthHint());
}

// Prev's restart while paused lets the track go (the backend knows no
// length then): the length it had stays the hint, so Now Playing keeps
// "0:00 / 4:05" and the seek bar.
void test_prev_restart_while_paused_keeps_the_length() {
  Rig r(2);
  r.player.play(0);
  r.audio.duration = kL;
  r.audio.position = 30000;
  r.player.togglePlayPause();
  r.player.prev();
  TEST_ASSERT_EQUAL_INT(0, r.player.currentIndex());
  TEST_ASSERT_EQUAL_INT((int)PlayState::Paused, (int)r.player.state());
  TEST_ASSERT_EQUAL_UINT32(kL, r.player.lengthHint());
}

// Paused before the backend took a seek's start up: the resume point is
// where it was asked to start (not the run before's second), no anchor.
void test_the_resume_point_while_a_start_is_pending() {
  Rig r(2);
  r.audio.asyncStarts = true;
  r.audio.anchorsOn = true;
  r.player.play(0);
  r.audio.take();
  r.audio.position = 30000;
  r.audio.duration = kL;
  r.audio.held = anchorAt(1323000);
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(0), 90000, kL));
  r.player.togglePlayPause();
  uint32_t ms = 0, dur = 0;
  ResumeAnchor got = anchorAt(1);
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_EQUAL_UINT32(90000, ms);
  TEST_ASSERT_EQUAL_UINT32(kL, dur);
  TEST_ASSERT_FALSE(got.valid());
  r.audio.take();  // taken up (paused): the backend's own again
  r.audio.position = 90000;
  TEST_ASSERT_TRUE(r.player.resumePoint(&ms, &dur, &got));
  TEST_ASSERT_TRUE(got == r.audio.held);
}

// What Now Playing shows (shownTime(), the snapshot's): a skip's start the
// backend hasn't taken up yet is 0:00 with no length, never the track
// before's, which the backend still gives until its decode task takes the
// request up (the seek bar would seek the new entry by it); then the
// backend's. A seek's pending start: its target and the bar's length. A
// backend that knows no length: the one told, unless the track failed.
void test_the_shown_time_never_lends_the_track_befores_length() {
  Rig r(2);
  uint32_t pos = 1, len = 1;
  r.player.shownTime(&pos, &len);  // (stopped at entry 0, nothing held)
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  TEST_ASSERT_EQUAL_UINT32(0, len);
  r.audio.asyncStarts = true;
  r.player.play(0);
  r.audio.take();
  r.audio.position = 120000;
  r.audio.duration = 300000;
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(120000, pos);
  TEST_ASSERT_EQUAL_UINT32(300000, len);
  r.player.next();  // pending: the backend still says the last track's
  TEST_ASSERT_EQUAL_UINT32(300000, r.audio.durationMs());
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  TEST_ASSERT_EQUAL_UINT32(0, len);  // not known: the bar is inert
  r.audio.take();
  r.audio.duration = 180000;
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  TEST_ASSERT_EQUAL_UINT32(180000, len);
  // A seek: its target and the bar's length until taken up.
  r.audio.position = 30000;
  r.audio.duration = 179000;  // (an estimate that moved meanwhile)
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(1), 90000, 180000));
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(90000, pos);
  TEST_ASSERT_EQUAL_UINT32(180000, len);
  r.audio.take();
  r.audio.duration = 0;  // a header-less file's first second
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(90000, pos);
  TEST_ASSERT_EQUAL_UINT32(180000, len);
  r.audio.failedFlag = true;
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(0, len);
}

// A seek to 0:00 while a start is pending (an entry whose length was told
// before, come round again by prev) keeps the bar's length: restart()
// doesn't note the backend's, which is still the track before's.
void test_a_seek_to_0_while_a_start_is_pending_keeps_the_bars_length() {
  Rig r(3);
  r.audio.asyncStarts = true;
  r.player.play(0);
  r.audio.take();
  r.player.next();  // entry 1
  r.audio.take();
  r.audio.duration = 180000;
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(1), 60000, 180000));
  r.audio.take();
  r.player.next();  // entry 2 (no hint: entry 1's length stays told)
  r.audio.take();
  r.audio.duration = 300000;
  r.audio.position = 0;
  r.player.prev();  // entry 1 again, pending: the backend still says entry 2's
  TEST_ASSERT_EQUAL_INT(1, r.player.currentIndex());
  uint32_t pos = 1, len = 1;
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(0, pos);
  TEST_ASSERT_EQUAL_UINT32(180000, len);
  TEST_ASSERT_EQUAL(PlaybackController::Seek::Started, r.player.seek(r.queue.keyAt(1), 0, 180000));
  TEST_ASSERT_EQUAL_UINT32(180000, r.player.lengthHint());
  r.player.shownTime(&pos, &len);
  TEST_ASSERT_EQUAL_UINT32(180000, len);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_new_queue_selects_first_and_stops);
  RUN_TEST(test_play_starts_selected_track);
  RUN_TEST(test_toggle_play_pause_resume);
  RUN_TEST(test_repeat_all_wraps);
  RUN_TEST(test_auto_advance_when_track_finishes);
  RUN_TEST(test_failed_track_is_skipped);
  RUN_TEST(test_a_failure_is_recorded_with_its_entry);
  RUN_TEST(test_a_rate_refusal_is_recorded_with_the_failure);
  RUN_TEST(test_the_backends_note_is_recorded_with_the_failure);
  RUN_TEST(test_seekable_is_the_current_entrys);
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
  RUN_TEST(test_repeat_off_stops_at_the_end);
  RUN_TEST(test_a_replaced_queue_starts_the_new_current_if_the_old_one_is_gone);
  RUN_TEST(test_pause_after_this_track_cues_the_next_entry);
  RUN_TEST(test_pause_after_this_track_survives_a_skip_and_a_failure);
  RUN_TEST(test_pause_after_the_last_track_without_repeat_stops);
  RUN_TEST(test_pause_by_timer_marks_the_pause);
  RUN_TEST(test_pause_by_computer_never_starts_playback);
  RUN_TEST(test_a_start_point_waits_for_the_next_play);
  RUN_TEST(test_a_start_point_belongs_to_its_entry);
  RUN_TEST(test_a_start_point_while_playing_or_paused);
  RUN_TEST(test_the_resume_point_is_a_paused_tracks_position);
  RUN_TEST(test_stop_keeping_place_keeps_a_paused_tracks_second);
  RUN_TEST(test_stop_keeping_place_keeps_a_playing_tracks_second);
  RUN_TEST(test_stop_keeping_place_has_nothing_to_keep);
  RUN_TEST(test_a_dropped_start_point_doesnt_come_back_with_an_undo);
  RUN_TEST(test_a_start_point_keeps_its_length);
  RUN_TEST(test_the_prev_rule);
  RUN_TEST(test_prev_while_playing_past_3_s_restarts_the_track);
  RUN_TEST(test_prev_while_paused_past_3_s_goes_to_0_and_stays_paused);
  RUN_TEST(test_a_second_prev_while_paused_goes_to_the_entry_before);
  RUN_TEST(test_cue_prev_while_paused_past_3_s_restarts_paused);
  RUN_TEST(test_prev_while_stopped_goes_to_the_entry_before);
  RUN_TEST(test_prev_on_the_first_entry);
  RUN_TEST(test_prev_before_the_backend_takes_a_start_goes_to_the_entry_before);
  RUN_TEST(test_prev_right_after_a_start_part_of_the_way_in_restarts);
  RUN_TEST(test_prev_on_a_failed_track_goes_to_the_entry_before);
  RUN_TEST(test_prev_restarts_a_builtin_track);
  RUN_TEST(test_prev_while_waiting);
  RUN_TEST(test_a_restart_keeps_the_sleep_timers_pause_after_this_track);
  RUN_TEST(test_a_restart_leaves_the_queues_undo_alone);
  RUN_TEST(test_prev_on_a_start_point_goes_to_0_and_starts_nothing);
  RUN_TEST(test_gapless_the_word_is_what_advance_would_start);
  RUN_TEST(test_gapless_tokens);
  RUN_TEST(test_gapless_an_advance_moves_the_entry_without_a_play);
  RUN_TEST(test_gapless_a_stale_advance_starts_what_comes_next);
  RUN_TEST(test_gapless_actions_take_the_advance_first);
  RUN_TEST(test_gapless_an_advance_with_pause_after_pauses_at_once);
  RUN_TEST(test_gapless_a_failure_after_an_advance_is_the_new_entry_s);
  RUN_TEST(test_repeat_one_plays_the_entry_again);
  RUN_TEST(test_repeat_one_next_and_prev_move_and_wrap);
  RUN_TEST(test_repeat_one_moves_on_from_a_failure);
  RUN_TEST(test_an_end_at_0_00_is_a_failure);
  RUN_TEST(test_repeat_one_with_pause_after_this_track);
  RUN_TEST(test_a_start_point_on_a_cued_entry_keeps_its_told_length);
  RUN_TEST(test_the_word_for_each_repeat_mode);
  RUN_TEST(test_set_repeat_is_an_action);
  RUN_TEST(test_set_shuffle_changes_nothing_that_plays);
  RUN_TEST(test_shuffle_off_with_the_same_next_keeps_the_word);
  RUN_TEST(test_play_now_while_shuffled);
  RUN_TEST(test_shuffle_all_and_its_undo);
  RUN_TEST(test_a_start_points_anchor_reaches_the_play_once);
  RUN_TEST(test_a_start_points_anchor_goes_with_it);
  RUN_TEST(test_the_resume_point_carries_the_held_tracks_anchor);
  RUN_TEST(test_stop_keeping_place_keeps_the_backends_anchor);
  RUN_TEST(test_a_backend_without_anchors_is_as_before);
  RUN_TEST(test_a_seek_while_playing_starts_there_now);
  RUN_TEST(test_a_seek_to_0_is_prevs_restart_in_every_state);
  RUN_TEST(test_a_seek_while_paused_lets_the_track_go_and_is_the_resume_point);
  RUN_TEST(test_a_seek_while_waiting_starts_there_when_they_connect);
  RUN_TEST(test_a_seek_while_stopped_waits);
  RUN_TEST(test_a_seek_for_an_entry_that_moved_does_nothing);
  RUN_TEST(test_a_seek_never_asks_the_tail);
  RUN_TEST(test_a_seek_on_a_failed_or_unknown_length_track_does_nothing);
  RUN_TEST(test_a_seek_keeps_the_timers_marks_and_pause_after);
  RUN_TEST(test_a_seek_resets_the_failures_in_a_row);
  RUN_TEST(test_the_length_hint_follows_its_entry);
  RUN_TEST(test_prev_restart_while_paused_keeps_the_length);
  RUN_TEST(test_the_resume_point_while_a_start_is_pending);
  RUN_TEST(test_the_shown_time_never_lends_the_track_befores_length);
  RUN_TEST(test_a_seek_to_0_while_a_start_is_pending_keeps_the_bars_length);
  return UNITY_END();
}
