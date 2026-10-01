// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "OutputModel.h"
#include "PlaybackController.h"

// Play while Bluetooth is the output and the headphones aren't connected
// (they dropped overnight, idle, and the background search for them backs
// off, or rests). Without this, play went to "Playing" at 0:00 and waited for the
// link for good: no explanation, no timeout, no way out.
//
// PlaybackController holds such a play (PlayState::Waiting: nothing starts,
// the position holds; main.cpp's Hold says when). PlayGate decides the rest,
// fed every loop pass:
//   - a wait begins: connect now (BtSession::connect() and BtSink::connect():
//     the paging burst, "try 1 of 3", not the back-off's next page, and
//     from resting too; a page under way counts as the burst's first try,
//     BtSink's doing). While the Pair screen has the radio (its scan, or a
//     pairing) only the session is asked: that scan isn't stopped for it;
//   - the headphones connect: the wait is released, play starts on them
//     ("Now playing on SPYDRONE");
//   - the burst fails (its tries ran out: BtSession::failed(), or
//     kBackstopMs): the wait ends paused, and the UI says why ("Couldn't
//     reach SPYDRONE...": Try again, Play on speaker). The session's ask is
//     withdrawn as failed (BtSession::withdraw(): the card and the tab turn
//     red with the notice), the link left alone: the tries left and the
//     background reconnect cycle carry on quietly, and a link they bring
//     closes the notice and plays nothing, says nothing;
//   - the wait ends any other way (the listener cancelled it, the speaker,
//     the queue emptied): the ask is withdrawn too, so a link that comes
//     later says nothing ("Now playing on" while paused would be wrong);
//   - the output isn't Bluetooth any more while waiting (silent test mode):
//     the wait ends paused;
//   - no headphones are paired and nothing could find any (a release build
//     never scans by itself: SinkSearch): there is nothing to wait for. The
//     wait ends paused at once, nothing is asked of the radio, and the UI
//     says where pairing is. Not while the Pair screen has the radio: a
//     play waits for the pairing under way there.
// The listener's own ways out are the player's: a tap on play (or B) while
// waiting cancels it (Paused), and playOnSpeaker() is the explicit choice of
// the speaker. Nothing moves to the speaker by itself: every way out of a
// wait but that one ends paused.
//
// Portable, host-tested with the player and BtSession (test_play_gate).
class PlayGate {
public:
  // A wait that no link has answered in this long has failed, whatever the
  // tries are doing. The listener's number: the burst itself takes 20-30 s
  // (a try, then one every heartbeat, ~10 s), so this usually ends the wait
  // during the third try, which carries on (a link it brings closes the
  // notice, quietly).
  static constexpr uint32_t kBackstopMs = 20000;

  enum class State : uint8_t {
    Idle,     // nothing waits
    Waiting,  // a play waits for the headphones
    Failed,   // the last wait failed (the notice), until a link, a new wait, or the speaker
  };

  // What the loop reads every pass.
  struct In {
    PlayState play = PlayState::Stopped;  // the player's
    bool onBluetooth = false;             // Bluetooth is the output
    bool linked = false;                  // the headphones are connected (BtSink::connected())
    BtLink link;                          // what the link is doing (BtSink::link())
    bool sessionFailed = false;           // BtSession::failed(): the tries ran out
    bool nothingToFind = false;           // BtSink::nothingToFind(): none paired, no scan may find any
    uint32_t nowMs = 0;
  };

  enum class Do : uint8_t {
    None,
    Connect,  // a wait began: BtSession::connect() and BtSink::connect()
    Track,    // a wait began while the Pair screen has the radio: BtSession::connect() only
              // (unless the session wants the headphones already: a pairing's is kept)
    Release,  // the headphones are up: PlaybackController::release(), "Now playing on ..."
    GiveUp,   // the burst failed: PlaybackController::cancelWait(), BtSession::withdraw(failed), the notice
    Cancel,   // the output isn't Bluetooth any more: PlaybackController::cancelWait(), BtSession::withdraw()
    Ended,    // the wait ended some other way (cancelled, the speaker, stopped): BtSession::withdraw()
    NotPaired,  // nothing to wait for (none paired): PlaybackController::cancelWait(),
                // BtSession::withdraw(); the UI points to Output > Pair new headphones
  };

  Do update(const In& in);
  // update(), with what it asks of the player and the session done here
  // (Connect/Track: session.connect(); Release: player.release(); GiveUp,
  // Cancel, NotPaired: player.cancelWait(); GiveUp, Cancel, Ended,
  // NotPaired: session.withdraw()).
  // main.cpp is left the radio (Connect: BtSink::connect()) and what the UI
  // says.
  Do step(const In& in, PlaybackController& player, BtSession& session);

  State state() const { return state_; }
  bool waiting() const { return state_ == State::Waiting; }
  bool failed() const { return state_ == State::Failed; }
  // Counts the failures: the UI shows the notice once for each.
  uint32_t failures() const { return failures_; }
  // Since when the wait (or the failed one) began.
  uint32_t sinceMs() const { return sinceMs_; }

  // [Play on speaker]: the listener's explicit choice, while a play waits or
  // after one failed. The wait ends paused, `selectSpeaker()` moves the
  // output (main.cpp's: pausing first and cancelling a connection on its
  // way, as every move to the speaker does; false: it didn't move), and only
  // then does it play, there, at the speaker's own volume. True: it plays.
  template <typename F>
  static bool playOnSpeaker(PlaybackController& player, F&& selectSpeaker) {
    player.cancelWait();
    if (!selectSpeaker()) return false;
    if (player.state() != PlayState::Playing) player.togglePlayPause();
    return player.state() == PlayState::Playing;
  }

  static const char* stateName(State s);

private:
  State state_ = State::Idle;
  uint32_t sinceMs_ = 0;
  uint32_t failures_ = 0;
};
