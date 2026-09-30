#include "SleepTimer.h"

#include <cmath>
#include <cstdio>

#include "LibraryIndex.h"
#include "TrackCatalog.h"
#include "UiText.h"

namespace {
bool reached(uint32_t nowMs, uint32_t atMs) { return static_cast<int32_t>(nowMs - atMs) >= 0; }
}  // namespace

uint16_t SleepTimer::curveQ15(float p) {
  if (p <= 0.0f) return kUnity;
  if (p > 1.0f) p = 1.0f;
  const float gain = std::pow(10.0f, kFloorDb * p / 20.0f);
  const long q = std::lround(gain * kUnity);
  return static_cast<uint16_t>(q < 0 ? 0 : q > kUnity ? kUnity : q);
}

void SleepTimer::setTimed(uint32_t ms, uint32_t nowMs) {
  choice_ = Choice::Timed;
  phase_ = Phase::Counting;
  timedMs_ = ms;
  endAtMs_ = nowMs + ms;
  trackFade_ = false;
  held_ = kUnity;  // a fade that ran comes back up at the stage's slow rate
}

void SleepTimer::setEnd(Choice c) {
  if (c != Choice::EndOfTrack && c != Choice::EndOfAlbum && c != Choice::EndOfQueue) return;
  choice_ = c;
  phase_ = Phase::Armed;
  // Until the next update() reads the queue: End of track is always on its
  // boundary track; album and queue aren't known yet (+10 min waits).
  atBoundary_ = c == Choice::EndOfTrack;
  timedMs_ = 0;
  trackFade_ = false;
  held_ = kUnity;
}

bool SleepTimer::canExtend() const {
  switch (phase_) {
    case Phase::Counting: return true;
    case Phase::Fading: return true;  // a timed fade, or a track's (at its boundary track, or not after a skip)
    case Phase::Armed: return atBoundary_ && knownLength_;
    default: return false;
  }
}

bool SleepTimer::extend(uint32_t nowMs, uint32_t trackLeftMs) {
  if (!canExtend()) return false;
  if (phase_ == Phase::Counting) {
    endAtMs_ += kExtendMs;  // added to what is left
  } else if (phase_ == Phase::Fading && !trackFade_) {
    endAtMs_ = nowMs + kExtendMs;  // nothing was left: 10 min from now
  } else if (!atBoundary_) {
    // A track's fade, skipped away from the boundary track (End of album
    // or queue): what is left is more than this track, and unknown. The
    // level comes back up; the boundary is still ahead.
    phase_ = Phase::Armed;
    trackFade_ = false;
    held_ = kUnity;
    return true;
  } else {
    // End of track / album / queue on the boundary track (armed or
    // fading): what is left is the track's; a timed choice of that plus
    // 10 min.
    endAtMs_ = nowMs + trackLeftMs + kExtendMs;
  }
  choice_ = Choice::Timed;
  phase_ = Phase::Counting;
  timedMs_ = 0;  // (no pill of its own)
  trackFade_ = false;
  held_ = kUnity;
  return true;
}

void SleepTimer::cancel() {
  choice_ = Choice::Off;
  phase_ = Phase::Off;
  trackFade_ = false;
  held_ = kUnity;
}

bool SleepTimer::atBoundaryTrack(const In& in) const {
  switch (choice_) {
    case Choice::EndOfTrack: return true;
    case Choice::EndOfAlbum: return in.lastOfAlbum;
    case Choice::EndOfQueue: return in.lastOfQueue;
    default: return false;
  }
}

void SleepTimer::beginEnding(bool faded) {
  phase_ = Phase::Ending;
  // Faded: to 0 until the pause is confirmed (then restore()); else the
  // factor was never lowered.
  if (faded) held_ = 0;
  trackFade_ = false;
  quiet_ = false;
}

SleepTimer::Out SleepTimer::update(const In& in) {
  Out out;
  const uint32_t now = in.nowMs;
  const bool boundary = stopsSeen_ && in.boundaryStops != seenStops_;
  seenStops_ = in.boundaryStops;
  stopsSeen_ = true;
  const bool playing = in.play == PlayState::Playing;
  const bool knownLength = in.durationMs > 0;
  const uint32_t left = knownLength && in.durationMs > in.positionMs ? in.durationMs - in.positionMs : 0;

  switch (phase_) {
    case Phase::Off:
      break;

    case Phase::Counting:
      if (!reached(now, endAtMs_)) break;
      out.expired = true;
      if (playing) {
        phase_ = Phase::Fading;
        trackFade_ = false;
        fadeFromMs_ = now;
        out.fadeStarted = true;
        break;
      }
      // Paused, stopped, or waiting for the headphones: no fade. A wait
      // ends paused; a pause is marked the timer's.
      out.pauseNow = in.play != PlayState::Stopped;
      beginEnding(false);
      break;

    case Phase::Armed:
      if (boundary) {
        // The player paused at the boundary (or stopped: the end of the
        // queue without repeat).
        out.expired = true;
        beginEnding(false);
        break;
      }
      if (playing && atBoundaryTrack(in) && knownLength && left > 0 && left <= kTrackFadeMs) {
        phase_ = Phase::Fading;
        trackFade_ = true;
        out.fadeStarted = true;
      }
      break;

    case Phase::Fading:
      if (trackFade_ && boundary) {
        out.expired = true;
        beginEnding(true);
        break;
      }
      if (!playing) {
        // A pause during the fade (the listener's, a drop, a move to the
        // speaker, a wait): the timer ends here.
        out.expired = true;
        out.pauseNow = in.play != PlayState::Stopped;
        beginEnding(true);
        break;
      }
      if (trackFade_) {
        // The track's last 10 s; after a skip, the new track's (the level
        // held meanwhile: it never rises by itself).
        if (atBoundaryTrack(in) && knownLength) {
          const float p = 1.0f - static_cast<float>(left) / static_cast<float>(kTrackFadeMs);
          const uint16_t q = curveQ15(p < 0.0f ? 0.0f : p);
          if (q < held_) held_ = q;
        }
      } else {
        const uint32_t elapsed = now - fadeFromMs_;
        const uint16_t q = elapsed >= kFadeMs ? 0 : curveQ15(static_cast<float>(elapsed) / kFadeMs);
        if (q < held_) held_ = q;
        if (elapsed >= kFadeMs + kZeroMs) {
          out.expired = true;
          out.pauseNow = true;
          beginEnding(true);
        }
      }
      break;

    case Phase::Ending:
      if (playing || in.play == PlayState::Waiting) {
        // Played again before the pause settled (the Core2's play): the
        // factor comes back up at the slow rate, and nothing more happens.
        phase_ = Phase::Off;
        held_ = kUnity;
        break;
      }
      if (!quiet_) {
        quiet_ = true;  // (the pause asked for is carried out after this pass's update)
        quietSinceMs_ = now;
        break;
      }
      if (now - quietSinceMs_ < kSettleMs) break;
      out.restore = true;  // silent: the factor back to 1.0 at once, nothing heard
      held_ = kUnity;
      out.screenOff = true;
      phase_ = Phase::Ended;
      endedAtMs_ = now;
      break;

    case Phase::Ended:
      if (playing || in.play == PlayState::Waiting) {
        phase_ = Phase::Off;  // listening again: nothing to let go
        break;
      }
      if (now - endedAtMs_ >= kReleaseMs) {
        out.release = true;
        phase_ = Phase::Off;
      }
      break;
  }

  atBoundary_ = atBoundaryTrack(in);
  knownLength_ = knownLength;
  trackFadeLive_ = atBoundary_ && knownLength && left <= kTrackFadeMs;
  out.pauseAfterTrack = (phase_ == Phase::Armed || (phase_ == Phase::Fading && trackFade_)) && atBoundaryTrack(in);
  out.fadeQ15 = held_;
  return out;
}

SleepTimer::ToastButton SleepTimer::toastTap(int x, bool clampedRight, bool landedUnattended) {
  if (clampedRight || landedUnattended) return ToastButton::None;
  const int gap = uitext::kSleepToastOffX - (uitext::kSleepToastPlusX + uitext::kSleepToastPlusW);
  if (x >= uitext::kSleepToastOffX - gap / 2) return ToastButton::TurnOff;
  if (x >= uitext::kSleepToastPlusX - 6) return ToastButton::Extend;
  return ToastButton::None;
}

uint32_t SleepTimer::msLeft(uint32_t nowMs) const {
  if (phase_ != Phase::Counting || reached(nowMs, endAtMs_)) return 0;
  return endAtMs_ - nowMs;
}

int SleepTimer::timedIndex() const {
  if (phase_ != Phase::Counting && !(phase_ == Phase::Fading && !trackFade_)) return -1;
  for (int i = 0; i < kTimedChoices; ++i) {
    if (timedMs_ == kMinutes[i] * 60000u) return i;
  }
  return -1;
}

namespace {
// "23 min" (rounded up), or "45 s" in the last minute.
void timeText(uint32_t ms, char* buf, size_t size) {
  if (ms > 60000) {
    std::snprintf(buf, size, "%lu min", static_cast<unsigned long>((ms + 59999) / 60000));
  } else {
    std::snprintf(buf, size, "%lu s", static_cast<unsigned long>((ms + 999) / 1000));
  }
}
}  // namespace

void SleepTimer::rowText(uint32_t nowMs, char* buf, size_t size) const {
  switch (phase_) {
    case Phase::Counting: timeText(msLeft(nowMs), buf, size); return;
    case Phase::Armed:
      std::snprintf(buf, size, "%s", choice_ == Choice::EndOfTrack   ? "End of track"
                                     : choice_ == Choice::EndOfAlbum ? "End of album"
                                                                     : "End of queue");
      return;
    case Phase::Fading: std::snprintf(buf, size, "Fading"); return;
    default: std::snprintf(buf, size, "Off"); return;
  }
}

void SleepTimer::shortText(uint32_t nowMs, char* buf, size_t size) const {
  switch (phase_) {
    case Phase::Counting: timeText(msLeft(nowMs), buf, size); return;
    case Phase::Armed:
      std::snprintf(buf, size, "%s", choice_ == Choice::EndOfTrack   ? "track"
                                     : choice_ == Choice::EndOfAlbum ? "album"
                                                                     : "queue");
      return;
    case Phase::Fading: std::snprintf(buf, size, "fading"); return;
    default:
      if (size) buf[0] = 0;
      return;
  }
}

void SleepTimer::titleText(uint32_t nowMs, char* buf, size_t size) const {
  switch (phase_) {
    case Phase::Counting: {
      char t[16];
      timeText(msLeft(nowMs), t, sizeof(t));
      std::snprintf(buf, size, "%s left", t);
      return;
    }
    case Phase::Armed:
      std::snprintf(buf, size, "%s", choice_ == Choice::EndOfTrack   ? "end of track"
                                     : choice_ == Choice::EndOfAlbum ? "end of album"
                                                                     : "end of queue");
      return;
    case Phase::Fading: std::snprintf(buf, size, "fading"); return;
    default: std::snprintf(buf, size, "off"); return;
  }
}

const char* SleepTimer::phaseName(Phase p) {
  switch (p) {
    case Phase::Off: return "off";
    case Phase::Counting: return "counting";
    case Phase::Armed: return "armed";
    case Phase::Fading: return "fading";
    case Phase::Ending: return "ending";
    case Phase::Ended: return "ended";
  }
  return "?";
}

const char* SleepTimer::choiceName(Choice c) {
  switch (c) {
    case Choice::Off: return "off";
    case Choice::Timed: return "timed";
    case Choice::EndOfTrack: return "end of track";
    case Choice::EndOfAlbum: return "end of album";
    case Choice::EndOfQueue: return "end of queue";
  }
  return "?";
}

bool SleepTimer::albumEndsBetween(const LibraryIndex* index, uint32_t a, uint32_t b) {
  if (b == LibraryIndex::kNone) return true;
  const bool haveA = index && index->ready() && !TrackCatalog::isBuiltin(a) && a < index->trackCount();
  const bool haveB = index && index->ready() && !TrackCatalog::isBuiltin(b) && b < index->trackCount();
  if (!haveA || !haveB) return true;  // a built-in track (or one not in the index): an album of its own
  const LibraryIndex::Track& ta = index->track(a);
  const LibraryIndex::Track& tb = index->track(b);
  const bool albumA = ta.album != LibraryIndex::kNone && index->albumName(ta.album)[0];
  const bool albumB = tb.album != LibraryIndex::kNone && index->albumName(tb.album)[0];
  if (albumA && albumB) return ta.album != tb.album;
  if (albumA != albumB) return true;
  return ta.folder != tb.folder;  // no album information: the folder
}

bool EntryStart::update(uint32_t key, uint32_t startSeq, uint32_t positionMs) {
  if (!seen_ || key != key_) {
    seen_ = true;
    key_ = key;
    seq_ = startSeq;
    pos_ = positionMs;
    started_ = positionMs < kFreshMs;  // this entry's start already, or the last one barely begun
  }
  if (!started_ && (startSeq != seq_ || positionMs < pos_)) started_ = true;
  return started_;
}
