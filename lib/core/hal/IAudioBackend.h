// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "ResumeAnchor.h"

// HAL seam for audio. The portable PlaybackController drives transport through
// this interface and never touches a decoder or an output directly. On the
// Core2 it is Core2AudioBackend (decode task -> PCM ring -> Bluetooth or the
// internal speaker); host tests use a fake.
class IAudioBackend {
public:
  virtual ~IAudioBackend() = default;

  // Begin playing `path`, `startMs` into it (0: from its start; a resume
  // point: the backend decides where that really lands, and positionMs()
  // counts from there). `durationHintMs`: its length as known elsewhere (a
  // resume point's, as the backend had it), 0: none; a hint for placing a
  // start part of the way in (a VBR MP3 without a table of contents): the
  // backend learns the real end of the track from the decoder.
  virtual bool play(const std::string& path, uint32_t durationHintMs, uint32_t startMs) = 0;
  // Where a start begins (docs/SEEK.md section 4.4): `ms` in (0: the top),
  // `hintMs` its length as known elsewhere, and a resume point's anchor
  // (kind None: none), which a backend that can checks against the file
  // and starts by, on the very sample it paused at; one that doesn't
  // match, or a backend without anchors, starts by `ms`.
  struct StartAt {
    uint32_t ms = 0;
    uint32_t hintMs = 0;
    ResumeAnchor anchor;
  };
  virtual bool play(const std::string& path, const StartAt& at) { return play(path, at.hintMs, at.ms); }
  // The heard track's anchor at the read position: what a paused track's
  // resume point saves. False: none (a built-in track, gapless trimming
  // off, a start not taken up yet, a backend without anchors).
  virtual bool resumeAnchor(ResumeAnchor* out) const {
    (void)out;
    return false;
  }
  virtual void pause() = 0;
  virtual void resume() = 0;
  virtual void stop() = 0;

  // Called every firmware loop with the current monotonic time in ms.
  virtual void loop(uint32_t nowMs) = 0;

  virtual bool isPlaying() const = 0;     // playing and not paused
  virtual uint32_t positionMs() const = 0;
  // positionMs() is the last play()'s track: false while that start hasn't
  // been taken up yet (a backend that starts asynchronously may still count
  // the track before). PlaybackController's prev reads the position only
  // then; before, it goes by where it asked that play to start.
  virtual bool positionKnown() const { return true; }
  // The current track's length (0: not known).
  virtual uint32_t durationMs() const { return 0; }
  virtual bool finished() const = 0;      // reached end of the current track
  // The current track can't be played (missing file, unsupported format, or a
  // sample rate the backend doesn't take).
  virtual bool failed() const { return false; }
  // Whether a start of `path` part of the way in lands there: false for a
  // format whose seeks aren't built (none today: MP3, FLAC and Opus each
  // start where asked, docs/SEEK.md and docs/OPUS.md section 9; an Opus
  // track was refused here until M3, when a start asked part of the way in
  // played from 0:00), so Now Playing's seek bar shows no knob and takes no
  // touch on it. By the path alone, so the player asks it of its current
  // entry (PlaybackController::seekable()) whichever way that became
  // current (a play(), a gapless join heard, a cue, a boot's restore),
  // never of the last request.
  virtual bool seekable(const char* path) const {
    (void)path;
    return true;
  }
  // While failed(): whether its sample rate was why. hz: that rate (0: some
  // other reason); needsCpu: a setting would take it (the 240 MHz CPU speed).
  struct RateRefusal {
    uint32_t hz = 0;
    bool needsCpu = false;
  };
  virtual RateRefusal rateRefusal() const { return {}; }
  // While failed(): why, in a few words for Now Playing's note ("surround
  // Opus isn't supported", "Opus with 2.5 ms frames isn't supported"),
  // into `buf`; the length (0: nothing better than "can't play it": a
  // missing file, a decoder giving up). A rate refusal says why first.
  virtual size_t failureNote(char* buf, size_t size) const {
    if (size) buf[0] = 0;
    return 0;
  }

  // ---- gapless playback (docs/GAPLESS.md) ----
  // The player's word on what follows a track: the track with token
  // `after` (0: the one the last play() started; a joined track has the
  // token its Next had) is followed by `path` (`token`: unique, never 0;
  // `hintMs` its length as known elsewhere), or by nothing (`token` 0: it
  // ends as before gapless playback). A backend that can may decode it
  // ahead and join it on without a gap. A newer word replaces this one:
  // one that names another track, or nothing, takes what was decoded ahead
  // back out, unless it has already been heard (then takeAdvance() still
  // reports it, and the player sorts it out). Backends without gapless
  // playback ignore it.
  struct Next {
    uint32_t after = 0;
    uint32_t token = 0;
    std::string path;
    uint32_t hintMs = 0;
  };
  virtual void setNext(const Next& next) { (void)next; }
  // A joined track is being heard (the outputs have read past its first
  // frame): its token, once per join. positionMs(), durationMs() and the
  // rest are its own from the same call on. False: none.
  virtual bool takeAdvance(uint32_t* token) {
    (void)token;
    return false;
  }
};
