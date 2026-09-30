// Host tests for play while Bluetooth is the output and the headphones
// aren't connected (PlayGate, with PlaybackController's Waiting and
// BtSession): the overnight bug, where play waited as "Playing" for good.
// Run: pio test -e native
#include <unity.h>

#include <string>

#include "ButtonPolicy.h"
#include "LibraryIndex.h"
#include "OutputModel.h"
#include "PlayGate.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "TrackCatalog.h"
#include "hal/IAudioBackend.h"

void setUp() {}
void tearDown() {}

namespace {

using P = BtLink::Phase;
using Do = PlayGate::Do;

class FakeAudioBackend : public IAudioBackend {
public:
  std::string lastPath;
  int playCount = 0;
  int stopCount = 0;
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
  void stop() override {
    playing = false;
    paused = false;
    ++stopCount;
  }
  void loop(uint32_t) override {}
  bool isPlaying() const override { return playing && !paused; }
  uint32_t positionMs() const override { return 0; }
  bool finished() const override { return false; }
  bool failed() const override { return false; }
};

// Three tracks, all queued, the player, the listener's Bluetooth session,
// the gate, and the output and the link as main.cpp reads them. pass() is
// main.cpp's loop pass: the session, then the gate (and a Connect pages).
struct Rig : PlaybackController::Hold {
  LibraryIndex index;
  TrackCatalog catalog{&index};
  QueueModel queue;
  FakeAudioBackend audio;
  PlaybackController player{audio, queue, catalog};
  BtSession session;
  PlayGate gate;
  bool onBluetooth = true;
  bool linked = false;
  BtLink link;
  int pages = 0;  // BtSink::connect() calls
  uint32_t now = 1000;

  Rig() {
    const char* files[] = {"/music/a.mp3", "/music/b.mp3", "/music/c.mp3"};
    index.begin("/music");
    for (const char* f : files) index.addFile(f);
    index.finish();
    const uint32_t ids[] = {0, 1, 2};
    queue.assign(ids, 3, 0);
    player.setHold(this);
    // Overnight: they dropped while idle; the background search has long
    // since come to rest (ReconnectPlanner: connectable only).
    link.phase = P::Resting;
    link.remembered = true;
    link.attempts = 3;
  }
  // main.cpp's: Bluetooth is the output and the headphones aren't connected.
  bool holdPlay() const override { return onBluetooth && !linked; }

  Do pass(uint32_t ms = 100) {
    now += ms;
    session.update(link, now);
    PlayGate::In in;
    in.play = player.state();
    in.onBluetooth = onBluetooth;
    in.linked = linked;
    in.link = link;
    in.sessionFailed = session.failed();
    in.nowMs = now;
    const Do d = gate.step(in, player, session);
    if (d == Do::Connect) {
      ++pages;
      paging(1);  // BtSink pages at once: "try 1 of 3"
    }
    return d;
  }
  void paging(uint8_t attempt) {
    link.phase = P::Paging;
    link.attempt = attempt;
  }
  // After a burst: a page now and then, no scan (ReconnectPlanner).
  void backingOff() {
    link.phase = P::Backoff;
    link.attempt = 0;
  }
  void up() {
    linked = true;
    link.phase = P::Linked;
    link.attempt = 0;
  }
  // They connect, and main.cpp's handleBluetooth() hears the Connected
  // event (before the session and the gate, as in loop()). True: it would
  // say "Now playing on ..." (its toast: a link the listener asked for and
  // no wait to say it).
  bool upWithEvent() {
    up();
    const BtSession::Answer a = session.onConnected();
    return a.asked && !gate.waiting() && player.state() != PlayState::Waiting;
  }
  // The card and the tab bar's icon (Ui::tabState()'s rule: on Bluetooth,
  // unlinked, red when lost or the session failed).
  BtCard card() const { return btCardView(link, session, false).card; }
  bool tabRed() const { return !linked && session.failed(); }
  // The tries run out: BtSink goes on to the back-off.
  void triesRunOut() {
    for (uint8_t t = 1; t <= 3; ++t) {
      paging(t);
      TEST_ASSERT_EQUAL(Do::None, pass(5000));
      TEST_ASSERT_EQUAL(PlayState::Waiting, player.state());
    }
    backingOff();
  }
  // Played on the headphones, then paused: the backend holds the track.
  void pausedMidTrack() {
    up();
    player.togglePlayPause();
    TEST_ASSERT_EQUAL(PlayState::Playing, player.state());
    player.togglePlayPause();
    TEST_ASSERT_EQUAL(PlayState::Paused, player.state());
    linked = false;  // then they dropped, idle
    backingOff();
    pass();
  }
  const char* path() const { return audio.lastPath.c_str(); }
};

// ButtonPolicy's transport over the rig (main.cpp's, the parts B uses).
struct Buttons : ButtonPolicy::Transport {
  Rig& r;
  explicit Buttons(Rig& rig) : r(rig) {}
  void prev() override { r.player.prev(); }
  void next() override { r.player.next(); }
  void playPause() override { r.player.togglePlayPause(); }
  bool playing() const override {
    return r.player.state() == PlayState::Playing || r.player.state() == PlayState::Waiting;
  }
  void pause() override {
    if (playing()) r.player.togglePlayPause();
  }
  void stepVolume(int) override {}
  int volume() const override { return 50; }
  bool onBluetooth() const override { return r.onBluetooth; }
  bool switchOutput() override {
    r.onBluetooth = !r.onBluetooth;
    return true;
  }
};

InputEvent click(int button, uint32_t ms) {
  InputEvent e;
  e.type = InputEvent::Type::Click;
  e.button = static_cast<uint8_t>(button);
  e.ms = ms;
  return e;
}

}  // namespace

// ---- the bug: play while unlinked ----

void test_play_while_unlinked_waits_and_connects_now() {
  Rig r;
  r.player.togglePlayPause();  // the Now Playing button, after the night
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());  // not "Playing"
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);              // nothing started
  TEST_ASSERT_FALSE(r.audio.isPlaying());
  // The paging burst at once, even though the search rests.
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_TRUE(r.gate.waiting());
  TEST_ASSERT_TRUE(r.session.wanted());
  // The Output card agrees: connecting, try 1 of 3.
  const BtCardView v = btCardView(r.link, r.session, false);
  TEST_ASSERT_EQUAL(BtCard::Connecting, v.card);
  TEST_ASSERT_EQUAL_UINT8(1, r.link.attempt);
  // Later passes wait on, asking nothing more.
  for (int i = 0; i < 20; ++i) TEST_ASSERT_EQUAL(Do::None, r.pass(200));
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  // They connect: play starts on them.
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.path());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
}

void test_a_resume_waits_with_the_paused_track_kept() {
  Rig r;
  r.pausedMidTrack();
  const int plays = r.audio.playCount, stops = r.audio.stopCount;
  r.player.togglePlayPause();  // resume, unlinked
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_TRUE(r.audio.paused);  // held where it was, not dropped
  TEST_ASSERT_EQUAL_INT(stops, r.audio.stopCount);
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  // Resumed from where it paused, not started again.
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(plays, r.audio.playCount);
  TEST_ASSERT_EQUAL_INT(1, r.audio.resumeCount);
}

void test_play_while_linked_plays_at_once() {
  Rig r;
  r.up();
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  TEST_ASSERT_EQUAL_INT(0, r.pages);
  // On the speaker too: nothing to wait for.
  Rig s;
  s.onBluetooth = false;
  s.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Playing, s.player.state());
  TEST_ASSERT_EQUAL(Do::None, s.pass());
}

void test_a_wait_during_a_background_burst_gets_a_full_burst() {
  Rig r;
  r.paging(3);  // the background cycle's burst, at its last try
  r.player.togglePlayPause();
  // Not only followed (it would fail when that last try did): a burst of
  // its own, try 1 of 3 (BtSink counts a page on its way as that try).
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_EQUAL_UINT8(1, r.link.attempt);
  TEST_ASSERT_TRUE(r.session.wanted());
  // Tries 2 and 3 still come: the wait goes on through them.
  r.paging(2);
  TEST_ASSERT_EQUAL(Do::None, r.pass(5000));
  r.paging(3);
  TEST_ASSERT_EQUAL(Do::None, r.pass(5000));
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  r.backingOff();
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass());  // this burst's tries ran out
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
}

void test_the_pair_screens_scan_isnt_stopped_by_a_wait() {
  Rig r;
  r.link.phase = P::PairScan;  // Output > Pair, still Bluetooth, unlinked
  Buttons t(r);
  ButtonPolicy b;
  TEST_ASSERT_TRUE(b.handle(click(ButtonPolicy::kButtonB, 10), t));  // B works on every screen
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL(Do::Track, r.pass());  // no page: the scan goes on
  TEST_ASSERT_EQUAL_INT(0, r.pages);
  TEST_ASSERT_TRUE(r.session.wanted());
  // The Pair screen closes: the background cycle pages them, and they come.
  r.paging(1);
  TEST_ASSERT_EQUAL(Do::None, r.pass(3000));
  TEST_ASSERT_FALSE(r.upWithEvent());  // the release says it, once
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
}

void test_a_wait_during_a_pairing_keeps_the_pairing() {
  Rig r;
  r.session.pairStarted(r.now);  // new headphones picked on the Pair screen
  r.link.phase = P::Pairing;
  r.link.attempt = 1;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Track, r.pass());
  TEST_ASSERT_EQUAL_INT(0, r.pages);
  TEST_ASSERT_TRUE(r.session.pairing());  // not turned into a plain connect
  r.player.togglePlayPause();             // cancelled
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_TRUE(r.session.wanted());  // the pairing is the listener's: kept
  TEST_ASSERT_TRUE(r.session.pairing());
  r.up();
  TEST_ASSERT_TRUE(r.session.onConnected().paired);  // its name is kept
}

// A wait while the search rests (or backs off): the full burst at once,
// and the card and tab bar follow it (amber, "try 1 of 3"), not the
// resting line.
void test_a_wait_while_resting_gets_a_full_burst() {
  Rig r;
  TEST_ASSERT_EQUAL(BtCard::Resting, r.card());
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_EQUAL(BtCard::Connecting, r.card());
  TEST_ASSERT_TRUE(r.session.wanted());
  // They answer the second try.
  r.paging(2);
  TEST_ASSERT_EQUAL(Do::None, r.pass(5000));
  TEST_ASSERT_FALSE(r.upWithEvent());
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  // From the back-off likewise.
  Rig b;
  b.backingOff();
  b.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, b.pass());
  TEST_ASSERT_EQUAL_INT(1, b.pages);
}

// ---- the ways a wait ends ----

void test_tries_run_out_ends_the_wait_paused_and_failed() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.triesRunOut();
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  TEST_ASSERT_TRUE(r.gate.failed());
  TEST_ASSERT_EQUAL_UINT32(1, r.gate.failures());
  TEST_ASSERT_TRUE(r.session.failed());  // the Output card: Couldn't connect, Try again
  TEST_ASSERT_EQUAL(BtCard::Failed, btCardView(r.link, r.session, false).card);
  // The notice stays; nothing else is asked (the background cycle goes on).
  for (int i = 0; i < 10; ++i) TEST_ASSERT_EQUAL(Do::None, r.pass(1000));
  TEST_ASSERT_TRUE(r.gate.failed());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
}

// The burst as the device runs it: try 1 at once, then one 10 s after the
// last began (ReconnectPlanner), the back-off ~5 s after the third. The
// 20 s backstop comes first, during the third try.
void test_the_backstop_during_the_last_try_settles_the_session() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());  // try 1
  r.paging(2);
  TEST_ASSERT_EQUAL(Do::None, r.pass(9000));
  r.paging(3);
  TEST_ASSERT_EQUAL(Do::None, r.pass(10000));
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass(1000));  // 20 s: the third try still pages
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.gate.failed());
  // The card and the tab agree with the notice from now on (not
  // "Connecting... try 3 of 3" in amber), and nothing waits on the link.
  TEST_ASSERT_FALSE(r.session.wanted());
  TEST_ASSERT_TRUE(r.session.failed());
  TEST_ASSERT_EQUAL(BtCard::Failed, r.card());
  TEST_ASSERT_TRUE(r.tabRed());
  TEST_ASSERT_EQUAL(Do::None, r.pass(3000));
  TEST_ASSERT_EQUAL(BtCard::Failed, r.card());
  // The third try reaches them: quietly (no "Now playing on", nothing
  // plays); the notice closes.
  TEST_ASSERT_FALSE(r.upWithEvent());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  TEST_ASSERT_FALSE(r.session.failed());
  TEST_ASSERT_EQUAL(BtCard::Connected, r.card());
}

void test_the_backstop_ends_a_wait_the_tries_dont() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  // Paging on and on (a page that hangs): nothing ends it but the backstop.
  const uint32_t since = r.gate.sinceMs();
  while (r.now + 500 - since < PlayGate::kBackstopMs) TEST_ASSERT_EQUAL(Do::None, r.pass(500));
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass(500));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_TRUE(r.gate.failed());
}

void test_a_tap_on_play_cancels_the_wait() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.player.togglePlayPause();  // the button (a spinner now) again: cancel
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  // The ask is withdrawn, the link left to its tries (no disconnect).
  TEST_ASSERT_FALSE(r.session.wanted());
  TEST_ASSERT_FALSE(r.session.failed());
  TEST_ASSERT_EQUAL(BtCard::Connecting, r.card());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  // They connect after all: nothing plays by itself, nothing says it does.
  TEST_ASSERT_FALSE(r.upWithEvent());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  // And the next play is an ordinary one.
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.path());
}

void test_b_double_press_starts_then_cancels_the_wait() {
  Rig r;
  Buttons t(r);
  ButtonPolicy b;
  // Two clicks, a pass between them: a wait, then cancelled.
  TEST_ASSERT_TRUE(b.handle(click(ButtonPolicy::kButtonB, 10), t));
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  TEST_ASSERT_TRUE(b.handle(click(ButtonPolicy::kButtonB, 250), t));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_FALSE(r.gate.waiting());
  TEST_ASSERT_FALSE(r.session.wanted());
  // Two clicks in one pass: nothing was asked, nothing waits.
  TEST_ASSERT_TRUE(b.handle(click(ButtonPolicy::kButtonB, 900), t));
  TEST_ASSERT_TRUE(b.handle(click(ButtonPolicy::kButtonB, 1000), t));
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  // A later link starts nothing and says nothing.
  TEST_ASSERT_FALSE(r.upWithEvent());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
}

void test_a_cancel_and_a_link_in_the_same_pass_say_nothing() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  // One loop pass: the tap (handleInput), then the Connected event
  // (handleBluetooth), before the gate hears of either.
  r.player.togglePlayPause();
  TEST_ASSERT_FALSE(r.upWithEvent());  // no "Now playing on" while paused
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
}

void test_a_b_hold_while_waiting_goes_to_the_speaker_paused() {
  Rig r;
  Buttons t(r);
  ButtonPolicy b;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  InputEvent hold;
  hold.type = InputEvent::Type::Hold;
  hold.button = ButtonPolicy::kButtonB;
  hold.ms = 2000;
  TEST_ASSERT_TRUE(b.handle(hold, t));
  // The wait counts as playing on the headphones: paused first, then moved.
  TEST_ASSERT_TRUE(b.feedback().paused);
  TEST_ASSERT_FALSE(r.onBluetooth);
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);  // never out loud
}

void test_play_on_speaker_is_the_explicit_choice() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  bool pausedWhenMoved = false;
  const bool playing = PlayGate::playOnSpeaker(r.player, [&] {
    pausedWhenMoved = r.player.state() == PlayState::Paused;  // the pause-first rule
    r.onBluetooth = false;
    r.session.cancel();  // main.cpp's selectOutput(false): the connection on its way cancelled
    return true;
  });
  TEST_ASSERT_TRUE(pausedWhenMoved);
  TEST_ASSERT_TRUE(playing);
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);
  TEST_ASSERT_EQUAL_STRING("/music/a.mp3", r.path());
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());  // the withdrawal touches only the session

  // From the failure's notice (paused) too; its notice goes.
  Rig f;
  f.player.togglePlayPause();
  f.pass();
  f.triesRunOut();
  TEST_ASSERT_EQUAL(Do::GiveUp, f.pass());
  TEST_ASSERT_TRUE(PlayGate::playOnSpeaker(f.player, [&] {
    f.onBluetooth = false;
    return true;
  }));
  TEST_ASSERT_EQUAL(Do::None, f.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, f.gate.state());

  // The output didn't move (silent test mode refuses): nothing plays.
  Rig n;
  n.player.togglePlayPause();
  n.pass();
  TEST_ASSERT_FALSE(PlayGate::playOnSpeaker(n.player, [] { return false; }));
  TEST_ASSERT_EQUAL(PlayState::Paused, n.player.state());
  TEST_ASSERT_EQUAL_INT(0, n.audio.playCount);
}

void test_a_link_after_the_failure_closes_the_notice_and_plays_nothing() {
  Rig r;
  r.player.togglePlayPause();
  r.pass();
  r.triesRunOut();
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass());
  TEST_ASSERT_TRUE(r.tabRed());
  // The background cycle finds them minutes later: quietly.
  r.pass(60000);
  TEST_ASSERT_FALSE(r.upWithEvent());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  // Play now: on them, at once.
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
}

void test_try_again_after_a_failure_waits_afresh() {
  Rig r;
  r.player.togglePlayPause();
  r.pass();
  r.triesRunOut();
  TEST_ASSERT_EQUAL(Do::GiveUp, r.pass());
  TEST_ASSERT_TRUE(r.session.failed());
  r.player.togglePlayPause();  // Try again
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  // A new burst; the old failure doesn't end it at once.
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  TEST_ASSERT_EQUAL_INT(2, r.pages);
  TEST_ASSERT_FALSE(r.session.failed());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_TRUE(r.gate.waiting());
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
}

void test_skip_while_waiting_stays_waiting_on_the_new_track() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  const uint32_t since = r.gate.sinceMs();
  r.player.next();
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.queue.current());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  // The same wait: no new burst, the backstop not restarted.
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL_UINT32(since, r.gate.sinceMs());
  r.player.next();
  r.player.prev();
  r.player.cueNext();  // a headphone key's cue (while waiting: a skip)
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL_INT(2, r.queue.current());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  TEST_ASSERT_EQUAL_INT(1, r.pages);
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Playing, r.player.state());
  TEST_ASSERT_EQUAL_INT(1, r.audio.playCount);  // the new track, once
  TEST_ASSERT_EQUAL_STRING("/music/c.mp3", r.path());
}

void test_a_skip_from_a_paused_track_waits_and_drops_it() {
  Rig r;
  r.pausedMidTrack();
  const int stops = r.audio.stopCount;
  r.player.next();  // C: skip-and-play, unlinked
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL_INT(stops + 1, r.audio.stopCount);  // the paused track let go
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.path());
}

void test_library_and_queue_plays_wait_the_same() {
  Rig r;
  const uint32_t album[] = {2, 1};
  TEST_ASSERT_TRUE(r.player.playNow(album, 2, 0));  // the Library's Play
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.player.play(1);  // the Queue's Play now, still waiting
  TEST_ASSERT_EQUAL(PlayState::Waiting, r.player.state());
  TEST_ASSERT_EQUAL(Do::None, r.pass());
  r.up();
  TEST_ASSERT_EQUAL(Do::Release, r.pass());
  TEST_ASSERT_EQUAL_STRING("/music/b.mp3", r.path());
}

void test_the_output_moving_away_ends_the_wait_paused() {
  Rig r;
  r.player.togglePlayPause();
  TEST_ASSERT_EQUAL(Do::Connect, r.pass());
  r.onBluetooth = false;  // silent test mode takes the speaker
  TEST_ASSERT_EQUAL(Do::Cancel, r.pass());
  TEST_ASSERT_EQUAL(PlayState::Paused, r.player.state());
  TEST_ASSERT_FALSE(r.session.wanted());
  TEST_ASSERT_EQUAL_INT(0, r.audio.playCount);  // not on out loud by itself
}

void test_stop_while_waiting_ends_it() {
  Rig r;
  r.player.togglePlayPause();
  r.pass();
  r.player.clearQueue();
  TEST_ASSERT_EQUAL(PlayState::Stopped, r.player.state());
  TEST_ASSERT_EQUAL(Do::Ended, r.pass());
  TEST_ASSERT_EQUAL(PlayGate::State::Idle, r.gate.state());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_play_while_unlinked_waits_and_connects_now);
  RUN_TEST(test_a_resume_waits_with_the_paused_track_kept);
  RUN_TEST(test_play_while_linked_plays_at_once);
  RUN_TEST(test_a_wait_during_a_background_burst_gets_a_full_burst);
  RUN_TEST(test_a_wait_while_resting_gets_a_full_burst);
  RUN_TEST(test_the_pair_screens_scan_isnt_stopped_by_a_wait);
  RUN_TEST(test_a_wait_during_a_pairing_keeps_the_pairing);
  RUN_TEST(test_tries_run_out_ends_the_wait_paused_and_failed);
  RUN_TEST(test_the_backstop_during_the_last_try_settles_the_session);
  RUN_TEST(test_the_backstop_ends_a_wait_the_tries_dont);
  RUN_TEST(test_a_tap_on_play_cancels_the_wait);
  RUN_TEST(test_b_double_press_starts_then_cancels_the_wait);
  RUN_TEST(test_a_cancel_and_a_link_in_the_same_pass_say_nothing);
  RUN_TEST(test_a_b_hold_while_waiting_goes_to_the_speaker_paused);
  RUN_TEST(test_play_on_speaker_is_the_explicit_choice);
  RUN_TEST(test_a_link_after_the_failure_closes_the_notice_and_plays_nothing);
  RUN_TEST(test_try_again_after_a_failure_waits_afresh);
  RUN_TEST(test_skip_while_waiting_stays_waiting_on_the_new_track);
  RUN_TEST(test_a_skip_from_a_paused_track_waits_and_drops_it);
  RUN_TEST(test_library_and_queue_plays_wait_the_same);
  RUN_TEST(test_the_output_moving_away_ends_the_wait_paused);
  RUN_TEST(test_stop_while_waiting_ends_it);
  return UNITY_END();
}
