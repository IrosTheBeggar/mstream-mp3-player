// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "PlaybackController.h"

#include <algorithm>
#include <cstdio>
#include <string>

#include "TrackSeek.h"

// The seek bar's reach stops short of the tail rule (a start there would go
// to 0:00).
static_assert(trackseek::kSeekGuardMs > trackseek::kTailMs, "a seek must never land in the tail");

void PlaybackController::startCurrent() {
  if (!hasTrack()) return;
  if (held()) {
    // The output can't be heard yet: this entry waits, selected. The
    // backend lets go of what it had (a track playing or paused).
    if (state_ != PlayState::Stopped && !cued_) audio_.stop();
    setPlaying(PlayState::Waiting);
    cued_ = true;
    return;
  }
  startNow();
}

void PlaybackController::startNow() {
  if (!hasTrack()) return;
  // An unknown id gives "": the backend fails it and update() skips on.
  char path[TrackCatalog::kMaxPath];
  const uint32_t id = queue_.currentTrack();
  catalog_.path(id, path, sizeof(path));
  const uint32_t at = hasStartPoint() ? startMs_ : 0;
  // Its length: a start point's (the resume point's, as the backend had it:
  // a VBR file without a table of contents is placed by it), else the
  // catalog's hint.
  const uint32_t hint = at > 0 && startDurationMs_ > 0 ? startDurationMs_ : catalog_.durationHintMs(id);
  IAudioBackend::StartAt start;
  start.ms = at;
  start.hintMs = hint;
  if (at > 0) start.anchor = startAnchor_;  // (it goes with the start point)
  clearStartPoint();  // once: a later start of the entry is from its beginning
  playedFromMs_ = at;
  noteLength(hint);  // (a resume point's length, a seek's, a built-in track's)
  audio_.play(std::string(path), start);
  setPlaying(PlayState::Playing);
  cued_ = false;
  // A new request: the backend's track is this play's (token 0), and the
  // word on what follows goes again, with a new token (one taken before
  // this play is never taken again).
  heardToken_ = 0;
  offer_ = Offered{};
  sent_ = false;
}

void PlaybackController::play(size_t position) {
  Act act(*this);
  if (position >= queue_.size()) return;
  failuresInARow_ = 0;
  queue_.setCurrent(static_cast<uint32_t>(position));
  startCurrent();
}

void PlaybackController::togglePlayPause() {
  Act act(*this);
  switch (state_) {
    case PlayState::Stopped:
      if (!queue_.empty()) play(queue_.current() < 0 ? 0 : static_cast<size_t>(queue_.current()));
      break;
    case PlayState::Playing:
      audio_.pause();
      state_ = PlayState::Paused;
      break;
    case PlayState::Paused:
      if (cued_) {
        startCurrent();
        break;
      }
      if (held()) {
        setPlaying(PlayState::Waiting);  // the backend keeps the paused track: release() resumes it
        break;
      }
      audio_.resume();
      setPlaying(PlayState::Playing);
      break;
    case PlayState::Waiting:
      state_ = PlayState::Paused;  // cancelled: a cued entry stays cued, a paused track paused
      break;
  }
}

void PlaybackController::pauseByComputer() {
  Act act(*this);
  if (state_ == PlayState::Playing) {
    audio_.pause();
  } else if (state_ != PlayState::Waiting) {
    return;  // Stopped, Paused: as they are
  }
  state_ = PlayState::Paused;  // (Waiting: a cued entry stays cued, a paused track paused)
  pausedByComputer_ = true;
}

void PlaybackController::release() {
  Act act(*this);
  if (state_ != PlayState::Waiting) return;
  if (cued_) {
    startNow();
    return;
  }
  audio_.resume();
  setPlaying(PlayState::Playing);
}

void PlaybackController::cancelWait() {
  Act act(*this);
  if (state_ == PlayState::Waiting) state_ = PlayState::Paused;
}

void PlaybackController::next() {
  Act act(*this);
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  clearStartPoint();  // (a queue of one wraps to the same entry: from its start)
  advance(false);
}

uint32_t PlaybackController::endNext() const {
  return repeat_ == Repeat::One ? queue_.peek(0, false) : queue_.peek(+1, wraps());
}

bool PlaybackController::stepAtEnd() {
  if (repeat_ == Repeat::One) {
    if (!hasTrack()) return false;
    ++repeats_;  // this entry again
    return true;
  }
  return queue_.step(+1, wraps());
}

void PlaybackController::advance(bool atEnd) {
  if (!(atEnd ? stepAtEnd() : queue_.step(+1, wraps()))) {
    stop();  // the end of the queue, repeat Off
    return;
  }
  startCurrent();
}

void PlaybackController::setRepeat(Repeat r) {
  Act act(*this);  // (the word follows at once: refreshOffer())
  repeat_ = r;
}

void PlaybackController::setShuffle(bool on) {
  Act act(*this);
  // The same entry (its key) stays current in the same state: nothing to
  // start, stop or cue, and its start point and length belong to the key.
  queue_.setShuffled(on);
}

PlaybackController::Prev PlaybackController::prevRule(PlayState state, bool startPointWaits, bool positionKnown,
                                                      uint32_t positionMs) {
  if (startPointWaits) return Prev::Restart;  // the entry it would pick up in, from 0:00
  if (state == PlayState::Stopped) return Prev::Previous;
  return positionKnown && positionMs > kRestartAfterMs ? Prev::Restart : Prev::Previous;
}

PlaybackController::Prev PlaybackController::prevAction() const {
  if (!hasTrack()) return Prev::Previous;
  // Stopped or cued, the backend holds nothing of this entry: at 0:00.
  // A start it hasn't taken up yet may still count the track before: it
  // is where that play asked to start (a resume point's 2:30 is 2:30 at
  // once, a restart's or a skip's 0:00 is 0:00). A track that failed has
  // no place to go back to.
  const bool holding = state_ != PlayState::Stopped && !cued_;
  if (!holding) return prevRule(state_, hasStartPoint(), true, 0);
  const uint32_t at = audio_.positionKnown() ? audio_.positionMs() : playedFromMs_;
  return prevRule(state_, hasStartPoint(), !audio_.failed(), at);
}

void PlaybackController::prev() {
  Act act(*this);
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  if (prevAction() == Prev::Restart) {
    restart();
    return;
  }
  clearStartPoint();
  queue_.step(-1, wraps());  // at the start with repeat Off: the first track again
  startCurrent();
}

void PlaybackController::restart() {
  // The length it had stays shown at 0:00 (paused: "0:00 / 4:05", not
  // "--:--", while the backend holds nothing), and the seek bar with it.
  // Not while a start is pending: the backend's length may still be the
  // track before's (a seek to 0:00 then has noted the bar's already).
  if (state_ != PlayState::Stopped && !cued_ && audio_.positionKnown()) noteLength(audio_.durationMs());
  clearStartPoint();
  switch (state_) {
    case PlayState::Playing:
      startCurrent();  // from 0:00: a start like any (faded in; held: it waits)
      break;
    case PlayState::Paused:
    case PlayState::Waiting:
      // Nothing starts: the held track is let go and the next play (or the
      // wait's release) starts the entry from its beginning.
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

void PlaybackController::cueNext() {
  Act act(*this);
  cue(+1);
}
void PlaybackController::cuePrev() {
  Act act(*this);
  cue(-1);
}

void PlaybackController::cue(int delta) {
  if (state_ == PlayState::Playing || state_ == PlayState::Waiting) {
    delta > 0 ? next() : prev();
    return;
  }
  if (queue_.empty()) return;
  failuresInARow_ = 0;
  if (delta < 0 && prevAction() == Prev::Restart) {
    restart();  // as prev(): the same entry, at 0:00, still paused (or stopped)
    return;
  }
  clearStartPoint();
  queue_.step(delta, wraps());
  if (state_ == PlayState::Paused && !cued_) {
    audio_.stop();  // the paused track can't be resumed any more
    cued_ = true;
  }
}

void PlaybackController::stop() {
  Act act(*this);
  audio_.stop();
  state_ = PlayState::Stopped;
  cued_ = false;
  pausedByTimer_ = pausedByComputer_ = false;  // (stopped: headphone Play starts nothing anyway)
}

bool PlaybackController::stopKeepingPlace() {
  Act act(*this);
  uint32_t ms = 0, dur = 0;
  ResumeAnchor anchor;
  // A start point waiting (after a boot, qs): stop() leaves it as it is.
  if (hasStartPoint()) {
    stop();
    return true;
  }
  // The backend holds this entry's track: where it is, as prevAction()
  // reads it (a start not taken up yet: where it was asked to start; a
  // failed track has no place), with its anchor when it has one.
  const bool holding = hasTrack() && state_ != PlayState::Stopped && !cued_ && !audio_.failed();
  if (holding) {
    const bool known = audio_.positionKnown();
    ms = known ? audio_.positionMs() : playedFromMs_;
    dur = known ? audio_.durationMs() : 0;
    if (!known || !audio_.resumeAnchor(&anchor)) anchor = ResumeAnchor{};
  }
  stop();
  if (ms == 0) return false;
  setStartPoint(ms, dur, &anchor);  // stopped: it waits for the next play
  return true;
}

void PlaybackController::setStartPoint(uint32_t ms, uint32_t durationMs, const ResumeAnchor* anchor) {
  Act act(*this);
  placeStart(ms, durationMs, anchor);
}

void PlaybackController::placeStart(uint32_t ms, uint32_t durationMs, const ResumeAnchor* anchor) {
  if (!hasTrack() || ms == 0) {
    clearStartPoint();
    return;
  }
  if (durationMs == 0) {
    // Not said (the console's qs): the length as known here, so Now
    // Playing keeps it and the backend can place the start by it.
    if (hasStartPoint()) {
      durationMs = startDurationMs_;
    } else if (state_ != PlayState::Stopped && !cued_) {
      durationMs = audio_.durationMs();  // the backend holds this entry's track
    } else if (lengthHint() > 0) {
      // Stopped or cued (the sleep timer's end-of-track pause with Repeat
      // One cues the entry that just played): the length told for this
      // entry, which the bar shows meanwhile. The catalog's hint (0 for a
      // library track) would leave the bar inert until a play.
      durationMs = lengthHint();
    } else {
      durationMs = catalog_.durationHintMs(queue_.currentTrack());
    }
  }
  startMs_ = ms;
  startDurationMs_ = durationMs;
  startKey_ = queue_.currentKey();
  startAnchor_ = anchor ? *anchor : ResumeAnchor{};
  switch (state_) {
    case PlayState::Playing:
      failuresInARow_ = 0;
      startCurrent();  // there, now
      break;
    case PlayState::Paused:
    case PlayState::Waiting:
      // A held track (paused, or waiting to resume) is let go: the next
      // play starts the entry again, there.
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

bool PlaybackController::startPoint(uint32_t* ms, uint32_t* durationMs, ResumeAnchor* anchor) const {
  if (!hasStartPoint()) return false;
  *ms = startMs_;
  *durationMs = startDurationMs_;
  if (anchor) *anchor = startAnchor_;
  return true;
}

bool PlaybackController::resumePoint(uint32_t* ms, uint32_t* durationMs, ResumeAnchor* anchor) const {
  if (startPoint(ms, durationMs, anchor)) return true;
  if ((state_ != PlayState::Paused && state_ != PlayState::Waiting) || cued_ || !hasTrack()) return false;
  if (!audio_.positionKnown()) {
    // Paused before the backend took the start up (within ~150 ms of a
    // seek): its position may still be the run before's. Where that start
    // was asked for, as stopKeepingPlace() reads it, with no anchor.
    *ms = playedFromMs_;
    *durationMs = lengthHint();
    if (anchor) *anchor = ResumeAnchor{};
    return *ms > 0;
  }
  *ms = audio_.positionMs();
  *durationMs = audio_.durationMs();
  if (anchor && !audio_.resumeAnchor(anchor)) *anchor = ResumeAnchor{};
  return *ms > 0;
}

PlaybackController::Seek PlaybackController::seek(uint32_t key, uint32_t ms, uint32_t durationMs) {
  Act act(*this);  // the heard join first: the entry may not be the one the finger was on
  if (!hasTrack()) return Seek::NoPlace;
  if (queue_.currentKey() != key) return Seek::Moved;
  const bool holding = state_ != PlayState::Stopped && !cued_;
  if (durationMs == 0 || (holding && audio_.failed())) return Seek::NoPlace;
  failuresInARow_ = 0;  // a listener's action, as next and prev
  noteLength(durationMs);
  ms = std::min(ms, trackseek::seekLimitMs(durationMs));  // never into the tail (it would start at 0:00)
  if (ms == 0) {
    restart();  // (a start point of 0 only clears one)
  } else {
    placeStart(ms, durationMs, nullptr);  // (no nested Act between the check and the start)
  }
  ++seeks_;
  return state_ == PlayState::Playing ? Seek::Started : Seek::Waits;
}

bool PlaybackController::seekable() const {
  // By the entry's path, not by what the backend was last asked to play:
  // after a gapless join the heard track is the entry's without any play(),
  // and after a boot the restored entry waits with none yet.
  char path[TrackCatalog::kMaxPath];
  if (currentPath(path, sizeof(path)) == 0) return false;
  return audio_.seekable(path);
}

bool PlaybackController::pendingStart(uint32_t* ms) const {
  // As prevAction() reads it: only while the backend holds this entry's
  // track (the play it was asked for) and hasn't taken that start up.
  if (!hasTrack() || state_ == PlayState::Stopped || cued_ || audio_.positionKnown()) return false;
  *ms = playedFromMs_;
  return true;
}

void PlaybackController::shownTime(uint32_t* positionMs, uint32_t* durationMs) const {
  *positionMs = 0;
  *durationMs = 0;
  if (!hasTrack()) return;
  uint32_t ms = 0, length = 0;
  if (startPoint(&ms, &length)) {
    *positionMs = ms;
    *durationMs = length;
    return;
  }
  if (pendingStart(&ms)) {
    // Never the backend's length here: a skip's is the track before's until
    // the decode task takes the request up. A library track has no hint: no
    // length for that moment, and the seek bar is inert.
    *positionMs = ms;
    *durationMs = lengthHint();
    return;
  }
  *positionMs = audio_.positionMs();
  *durationMs = audio_.durationMs();
  if (*durationMs == 0 && !audio_.failed()) *durationMs = lengthHint();
}

void PlaybackController::pauseByTimer() {
  Act act(*this);
  switch (state_) {
    case PlayState::Playing:
      audio_.pause();
      state_ = PlayState::Paused;
      break;
    case PlayState::Waiting:
      state_ = PlayState::Paused;  // as cancelWait(): a cued entry stays cued
      break;
    case PlayState::Paused:
      break;
    case PlayState::Stopped:
      return;
  }
  pausedByTimer_ = true;
}

void PlaybackController::pauseAtBoundary() {
  pauseAfter_ = false;
  ++timerStops_;
  // Repeat One: no step, this entry is cued at 0:00 (the timer wins: a
  // later play starts it from the top, as One would have).
  if (repeat_ != Repeat::One && !queue_.step(+1, wraps())) {
    stop();  // the end of the queue, repeat Off: the natural stop
    return;
  }
  audio_.stop();  // the finished track lets go; the next one (or this one again) is cued
  state_ = PlayState::Paused;
  cued_ = true;
  pausedByTimer_ = true;
}

void PlaybackController::update(uint32_t nowMs) {
  (void)nowMs;  // the backend owns the clock via its own loop(); reserved here
  Act act(*this);  // a joined track heard first: its end, if it ended too, is its own
  checkEnd();
}

void PlaybackController::checkEnd() {
  if (state_ != PlayState::Playing) return;
  // A natural end with nothing heard (the position never left 0:00): a
  // track with no samples to play, which every format can hold (an Opus
  // file whose last granule is its pre-skip, an MP3 that is all encoder
  // delay and padding, a FLAC of 0 samples; the Opus reader refuses the
  // first at its open, the others end at once). Taken as a failure: by
  // the repeat mode it would be started again at once, and Repeat One,
  // or All with nothing else to play, would spin on it (an SD open and a
  // log line fifty times a second, Now Playing frozen at 0:00) until the
  // listener acted. A failure moves on even under Repeat One, and a
  // queue of nothing but such entries stops (docs/QUEUE-MODES.md).
  const bool empty = !audio_.failed() && audio_.finished() && audio_.positionKnown() && audio_.positionMs() == 0;
  if (audio_.failed() || empty) {
    ++failure_.count;
    failure_.track = queue_.currentTrack();
    failure_.key = queue_.currentKey();
    failure_.rate = audio_.rateRefusal();
    if (empty) {
      snprintf(failure_.note, sizeof(failure_.note), "no audio in it");
    } else {
      audio_.failureNote(failure_.note, sizeof(failure_.note));
    }
    if (++failuresInARow_ >= queue_.size()) {
      stop();  // every track failed in a row: nothing here plays
      return;
    }
    advance(false);  // a failure moves on (a skip), even with Repeat One
  } else if (audio_.finished()) {
    failuresInARow_ = 0;
    if (pauseAfter_) {
      pauseAtBoundary();  // the sleep timer: the next entry (One: this one), paused at 0:00
      return;
    }
    advance(true);  // by the repeat mode: All wraps at the end, One plays it again
  }
}

void PlaybackController::currentMoved() {
  failuresInARow_ = 0;
  // The start point was the old entry's: dropped, not only hidden (an undo
  // that brings that entry back must not bring its second back too).
  clearStartPoint();
  if (!hasTrack()) {
    stop();
    return;
  }
  switch (state_) {
    case PlayState::Playing:
    case PlayState::Waiting:
      startCurrent();
      break;
    case PlayState::Paused:
      if (!cued_) {
        audio_.stop();
        cued_ = true;
      }
      break;
    case PlayState::Stopped:
      break;
  }
}

bool PlaybackController::playNow(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffle) {
  Act act(*this);
  if (!queue_.replace(tracks, n, start, shuffle)) return false;
  failuresInARow_ = 0;
  clearStartPoint();
  if (hasTrack()) {
    startCurrent();
  } else {
    stop();
  }
  return true;
}

bool PlaybackController::playNext(const uint32_t* tracks, uint32_t n) {
  Act act(*this);
  return queue_.insertNext(tracks, n);
}

bool PlaybackController::addToQueue(const uint32_t* tracks, uint32_t n) {
  Act act(*this);
  return queue_.append(tracks, n);
}

bool PlaybackController::moveNext(const uint32_t* positions, uint32_t n) {
  Act act(*this);
  return queue_.moveNext(positions, n);
}

bool PlaybackController::clearUpNext() {
  Act act(*this);
  return queue_.clearUpNext();
}

QueueModel::Removed PlaybackController::remove(const uint32_t* positions, uint32_t n) {
  Act act(*this);
  const QueueModel::Removed r = queue_.remove(positions, n);
  if (!r.current) return r;
  clearStartPoint();  // (as currentMoved(); stop() doesn't)
  if (r.pastEnd) {
    stop();  // nothing after it stayed: stopped, on the last track
  } else {
    currentMoved();
  }
  return r;
}

void PlaybackController::clearQueue() {
  Act act(*this);
  clearStartPoint();  // an undo brings the queue back, not the second
  stop();
  queue_.clear();
}

bool PlaybackController::undo() {
  Act act(*this);
  const uint32_t key = queue_.currentKey();
  if (!queue_.undo()) return false;
  if (queue_.currentKey() != key) currentMoved();
  return true;
}

void PlaybackController::queueReplaced(bool currentKept) {
  Act act(*this);  // (kept: the keys are new; the word keeps its token for the same next track)
  if (!currentKept) currentMoved();
}

// ---- gapless playback ----

void PlaybackController::setGapless(bool on) {
  Act act(*this);
  gapless_ = on;
}

void PlaybackController::remember(const Offered& o) {
  history_[historyAt_] = o;
  historyAt_ = static_cast<uint8_t>((historyAt_ + 1) % (sizeof(history_) / sizeof(history_[0])));
}

const PlaybackController::Offered* PlaybackController::offered(uint32_t token) const {
  for (const Offered& o : history_) {
    if (o.token == token && token != 0) return &o;
  }
  return nullptr;
}

void PlaybackController::refreshOffer() {
  // Only while the backend holds this entry's track: otherwise its next
  // play() comes first (and the word after it).
  const bool holding = hasTrack() && state_ != PlayState::Stopped && !cued_;
  if (!holding) {
    sent_ = false;
    return;
  }
  const bool gate = gate_ && gate_->endsHere();
  Signature sig;
  sig.position = queue_.positionVersion();
  sig.content = queue_.contentVersion();
  sig.heard = heardToken_;
  sig.repeat = static_cast<uint8_t>(repeat_);
  sig.pauseAfter = pauseAfter_;
  sig.gapless = gapless_;
  sig.gate = gate;
  if (sent_ && sig == signature_) return;
  signature_ = sig;

  // What advance() would start at the natural end (Repeat One: this entry
  // itself, the self-join), unless the track must end as it would without
  // gapless playback.
  uint32_t pos = QueueModel::kNone;
  if (gapless_ && !pauseAfter_ && !gate) pos = endNext();
  char path[TrackCatalog::kMaxPath];
  path[0] = 0;
  uint32_t track = QueueModel::kNone;
  if (pos != QueueModel::kNone) {
    track = queue_.trackAt(pos);
    if (catalog_.path(track, path, sizeof(path)) == 0) pos = QueueModel::kNone;  // (an unknown id: play() fails it)
  }
  if (pos == QueueModel::kNone) {
    offer_ = Offered{};
  } else {
    const uint32_t key = queue_.keyAt(pos);
    // The same track still next keeps its token (no cut): its own entry,
    // or the entry that took its place when its key went (a library
    // rebuild's fresh keys, one of two duplicates removed). Never the
    // heard token: the backend took it already, and would answer a word
    // with it "nothing follows" (Repeat One, or a queue of one on repeat:
    // the same entry after itself, a new token each time round).
    const bool same = offer_.token != 0 && offer_.token != heardToken_ && offer_.track == track &&
                      (offer_.key == key || queue_.positionOf(offer_.key) == QueueModel::kNone);
    if (same) {
      offer_.key = key;
      for (Offered& o : history_) {
        if (o.token == offer_.token) o.key = key;
      }
    } else {
      if (++nextToken_ == 0) ++nextToken_;
      offer_.token = nextToken_;
      offer_.key = key;
      offer_.track = track;
      remember(offer_);
    }
  }
  if (sent_ && offer_.token == sentToken_ && heardToken_ == sentAfter_) return;  // nothing new to say
  IAudioBackend::Next n;
  n.after = heardToken_;
  n.token = offer_.token;
  if (n.token) {
    n.path = path;
    n.hintMs = catalog_.durationHintMs(track);
  }
  audio_.setNext(n);
  sent_ = true;
  sentToken_ = n.token;
  sentAfter_ = n.after;
  ++gaplessStats_.offers;
}

void PlaybackController::syncHeard() {
  uint32_t token = 0;
  while (audio_.takeAdvance(&token)) {
    // (Stopped or cued, the backend has started something else since: a
    // new request drops its joins. Not reached.)
    if (!hasTrack() || state_ == PlayState::Stopped || cued_) continue;
    heardToken_ = token;  // the backend is on it, whatever follows here
    failuresInARow_ = 0;  // the track before played through
    playedFromMs_ = 0;
    clearStartPoint();  // (it was the track before's)
    // What update() would do at that track's natural end, now.
    if (pauseAfter_ || (gate_ && gate_->endsHere())) {
      pauseAtBoundary();  // the next entry cued at 0:00, paused: the timer's
      ++gaplessStats_.paused;
      continue;
    }
    const Offered* o = offered(token);
    const uint32_t expected = endNext();
    const bool match = o && expected != QueueModel::kNone &&
                       (queue_.keyAt(expected) == o->key ||
                        (queue_.positionOf(o->key) == QueueModel::kNone && queue_.trackAt(expected) == o->track));
    if (match && !(state_ == PlayState::Playing && held())) {
      // No play(): the backend plays it already. Repeat One: the same
      // position (nothing bumps; the heard token moved, so the next word
      // gets a new one), a loop. (A queue of one on All loops the same
      // way, but that is no Repeat One loop.)
      if (repeat_ == Repeat::One && static_cast<int32_t>(expected) == queue_.current()) ++repeats_;
      queue_.setCurrent(expected);
      ++gaplessStats_.adopted;
      continue;
    }
    // Not what comes next any more (an edit too late to cut it out), or
    // the output can't be heard: the entry advance() would start.
    ++gaplessStats_.restarted;
    if (state_ == PlayState::Playing) {
      advance(true);
    } else if (!stepAtEnd()) {
      stop();  // (paused at the queue's end, repeat Off: stopped there, as an end would)
    } else {
      audio_.stop();  // paused (a pause's fade read past the join): the new entry cued
      cued_ = true;
    }
  }
}
