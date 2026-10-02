// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>

#include "GaplessJoin.h"
#include "PcmRing.h"
#include "RingFeed.h"
#include "TrimFeed.h"

// The decode task's side of gapless playback (docs/GAPLESS.md sections 3.2,
// 5.1 and 5.3): what happens from the moment the decoder reaches the end
// of its source. A resumable state machine: each step() does one thing
// and returns, so the decode task looks at a new request (TransportSync's
// generation) between any two steps, and a wait for ring room, a cut that
// is pending or the player's word never holds it up.
//
// At a source's end (sourceEnded()):
//   1. Ending: the trim's end (the padding dropped), everything staged into
//      the ring, the decoder closed. J (cutAt) is the ring's write index;
//      the track's exact length is frozen (GaplessJoin::freeze()). Then the
//      player's word on what follows this track (GaplessJoin::take()):
//      - Next: its file probed for its rate (Tracks::probe()). The same
//        rate, the converter configured: a continuous join, no reset, no
//        tail: B (heardAt) = J + the converter's tailFrames(). Another
//        rate: the tail first (Flushing), a new stream, B = the write
//        index after the tail. The feed is marked (RingFeed::mark()) as it
//        is at J, the decoder started (Tracks::start()), the boundary
//        recorded before its first frame: Producing again, no discardAll(),
//        no new generation.
//      - Nothing: the tail (Flushing), then Draining, as before gapless.
//      - NoWord (the player hasn't spoken yet: it speaks of a joined track
//        only once it has heard it begin): wait while the ring holds more
//        than kWaitMinMs, then as Nothing.
//   2. Draining: until the ring is empty (Ended). A late word (+ Queue
//      onto the last entry, the sleep timer turned off) is still taken
//      while the ring holds kWaitMinMs or more: a join after the tail (a
//      new stream; at a converting rate a fresh filter, at 44.1 kHz
//      seamless).
// Every step (and every decoding pass: cutDue()) the boundary is checked:
// when the player's word for the track before it changed (another track,
// nothing, gapless turned off: setEnabled(false)), or the joined track
// produced nothing (it failed before its first frame), the frames after J
// are cut back out of the ring (PcmRing::cutBack(): Done, Pending: try
// again in 1 ms, Crossed: too late, the reader is past J: it stays) and the
// feed rewound to its mark: back at the end of the track before, the word
// asked again.
//
// One decode task; GaplessJoin is its only shared state.
class GaplessEngine {
public:
  static constexpr uint32_t kWaitMinMs = 250;  // a word waited for, a late one taken, while the ring holds this
  static constexpr uint32_t kRoomWaitMs = 10;  // the ring full
  static constexpr uint32_t kWordWaitMs = 5;   // (setNext() wakes the task anyway)
  static constexpr uint32_t kCutRetryMs = 1;

  enum class Phase : uint8_t {
    Idle,       // nothing to decode (stopped, a bench)
    Producing,  // the decoder runs: the decode task's own passes
    Ending,     // its source ended: commit, then the player's word
    Flushing,   // the converter's tail into the ring
    Draining,   // nothing follows: the ring plays out
    Ended,      // played out
  };

  // What the log hears about (Tracks::note()).
  enum class Event : uint8_t {
    Joined,        // a continuous join
    JoinedReset,   // a join after the tail (another rate, or late)
    OpenFailed,    // the next track couldn't be probed or started: this one ends as before
    EmptyAhead,    // the joined track ended with no frame: it is cut back out
    Cut,           // the frames after J taken back out
    CutTooLate,    // the reader was past J: the joined track stays
  };
  struct Note {
    Event event = Event::Joined;
    uint32_t token = 0;    // the track joined, opened or cut
    uint32_t cutAt = 0;    // J
    uint32_t heardAt = 0;  // B
    uint32_t ringFrames = 0;  // what the ring held then (frames before the reader gets to J)
    uint32_t rate = 0;     // the joined track's, as probed
    bool late = false;     // a word taken while Draining
  };

  // The firmware's decoder side (ESP8266Audio and the files; the host
  // tests' synthetic tracks).
  class Tracks {
  public:
    // Opens `next` from its start for a join and reads what it says before
    // any frame: its rate (0: not said). False: it can't be played. Nothing
    // goes into the feed.
    virtual bool probe(const GaplessJoin::Offer& next, uint32_t* rate) = 0;
    // The probed track's decoder begins, its trim armed (TrimFeed::arm());
    // nothing fed yet. False: it can't.
    virtual bool start() = 0;
    // The decoder, or the probed file, closed.
    virtual void close() = 0;
    virtual void note(const Note& n) { (void)n; }

  protected:
    ~Tracks() = default;
  };

  struct Counters {
    uint32_t joins = 0;       // continuous
    uint32_t resets = 0;      // after the tail (another rate, late)
    uint32_t late = 0;        // of which taken while Draining
    uint32_t cuts = 0;        // Done
    uint32_t tooLate = 0;     // Crossed
    uint32_t retries = 0;     // Pending
    uint32_t failedOpens = 0;
    uint32_t emptyAhead = 0;
  };

  GaplessEngine(PcmRing& ring, RingFeed& feed, TrimFeed& trim, GaplessJoin& book, Tracks& tracks)
      : ring_(ring), feed_(feed), trim_(trim), book_(book), tracks_(tracks) {}

  // Two marks (RingFeed::Mark, ~2 KB each; the firmware's in PSRAM): one
  // for the boundary waiting to be heard, one for the decoding track's end.
  // None: no joins.
  void setMarks(RingFeed::Mark* a, RingFeed::Mark* b) {
    marks_[0] = a;
    marks_[1] = b;
  }
  // The console's G0/G1. Off: no word is taken, and a boundary waiting is
  // cut (v0.5.0's ends from then on).
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }  // (any task)
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // A request's track starts (after the ring's discardAll() and the feed's
  // reset): its first frame at `startIdx`, `startMs` into it. `offers`
  // false: never joined (a test play at a forced rate).
  void begin(uint32_t gen, uint32_t startIdx, uint32_t startMs, bool offers);
  // Nothing decodes (a stop, a bench): Idle.
  void idle(uint32_t gen);
  // Where the request's track really landed (a resume point).
  void setStartMs(uint32_t ms);

  // The decoding track's source ended: `early` (a decode error, a file cut
  // short, a rate refused mid-stream): what the trim holds is real audio.
  void sourceEnded(bool early);
  // Whether step() must run now, whatever the phase (a cut).
  bool cutDue();
  // One step (see the class): ms to rest before the next (0: at once). Only
  // while phase() isn't Producing, or cutDue().
  uint32_t step(uint32_t bufferedMs);

  Phase phase() const { return phase_; }
  // The decoding track's token (0: the request's own).
  uint32_t decodingToken() const { return token_; }
  // Ring frames of the decoding track so far (its first at decodeStart).
  uint32_t decodeStart() const { return decodeStart_; }
  bool boundaryUp() const { return boundaryUp_; }
  const Counters& counters() const { return counters_; }

private:
  struct Prev {  // the track before the boundary, for a cut back to its end
    uint32_t token = 0;
    uint32_t start = 0;
    uint32_t startMs = 0;
    uint32_t endAt = 0;
    uint32_t tail = 0;
    bool flushed = false;
    uint8_t mark = 0;
  };
  enum class Then : uint8_t { Drain, Join };

  uint32_t stepEnding(uint32_t bufferedMs);
  uint32_t stepFlush();
  uint32_t stepDrain(uint32_t bufferedMs);
  uint32_t stepCut();
  // The next track's decoder begins and the boundary is recorded.
  void join(bool continuous, bool late);
  // The feed now into the mark not held by a waiting boundary.
  bool takeMark();
  bool canJoin() const { return enabled() && offers_ && marks_[0] && marks_[1]; }
  void note(Event e, bool late = false);

  PcmRing& ring_;
  RingFeed& feed_;
  TrimFeed& trim_;
  GaplessJoin& book_;
  Tracks& tracks_;
  RingFeed::Mark* marks_[2] = {nullptr, nullptr};
  std::atomic<bool> enabled_{true};
  bool offers_ = true;

  Phase phase_ = Phase::Idle;
  uint32_t gen_ = 0;
  uint32_t token_ = 0;        // the decoding track's
  uint32_t decodeStart_ = 0;  // its first ring frame
  uint32_t startMs_ = 0;      // where it started, ms in
  bool early_ = false;
  bool committed_ = false;    // Ending: its end is in the ring
  uint32_t endAt_ = 0;        // ... at this write index (J)
  uint32_t tail_ = 0;         // ... the converter owing this many frames
  bool flushed_ = false;      // ... the tail in too (endAt_ after it)
  uint8_t mark_ = 0;          // the mark holding the feed at endAt_
  bool marked_ = false;
  Then then_ = Then::Drain;
  GaplessJoin::Offer offer_;  // the word taken
  uint32_t offerRate_ = 0;
  bool boundaryUp_ = false;
  bool failedAhead_ = false;  // the joined track produced nothing: cut it
  bool cutting_ = false;
  Prev prev_;
  uint32_t cutAt_ = 0;
  Counters counters_;
};
