// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/DanceMode.h"

#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cmath>
#include <cstdint>

#include "app/Diagnostics.h"

namespace {
constexpr uint32_t kScratchFrames = 2048;  // mono frames handed to the tracker at a time (PSRAM)
// Aim the figure this much before the sound (then the LCD's own delay):
// ahead of the beat looks right, behind it looks late.
constexpr uint32_t kAimEarlyUs = 15000;
constexpr uint32_t kErrorWindowMs = 10000; // the phase error's median/p95 over this long
constexpr float kFlashBeats = 0.12f;       // the beat dot shows this much of a beat

void* psramAlloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
void psramFree(void* p) { heap_caps_free(p); }
}  // namespace

bool DanceMode::begin() {
  scratch_ = static_cast<int16_t*>(psramAlloc(kScratchFrames * sizeof(int16_t)));
  ready_ = scratch_ && tracker_.begin(BeatTracker::Config{}, psramAlloc, psramFree) && view_.begin(skin_);
  syncTaps();  // off until the Dance tab is up
  if (!ready_) {
    Serial.println("[dance] no PSRAM for the dance screen");
    return false;
  }
  benchTracker();
  return true;
}

// The tracker's own cost, measured once at boot with nothing else running on
// this core: 4 s of a click track, fed as the loop feeds it. (The [dance]
// line's `tracker` share is wall time on the loop task, preemptions
// included.)
void DanceMode::benchTracker() {
  auto* stereo = static_cast<int16_t*>(psramAlloc(kScratchFrames * 2 * sizeof(int16_t)));
  if (!stereo) return;
  ClickGen gen;
  ClickGen::Spec spec;
  gen.start(44100, spec, 4 * 44100);
  tracker_.reset(0);
  int64_t us = 0;
  uint32_t frames = 0;
  for (uint32_t n; (n = gen.generate(stereo, kScratchFrames)) > 0; frames += n) {
    for (uint32_t i = 0; i < n; ++i) scratch_[i] = static_cast<int16_t>((stereo[2 * i] + stereo[2 * i + 1]) >> 1);
    const int64_t t0 = esp_timer_get_time();
    tracker_.process(scratch_, n);
    us += esp_timer_get_time() - t0;
  }
  psramFree(stereo);
  Serial.printf("[dance] tracker bench: %.2f ms per second of audio (%.2f %% of a core), %s at %.2f BPM\n",
                us / 1000.0f / (frames / 44100.0f), us / 10.0f / (frames / 44100.0f) / 1000.0f,
                tracker_.locked() ? "locked" : "not locked", tracker_.bpm());
  tracker_.reset(0);
}

void DanceMode::setActive(bool on) {
  if (on && !ready_) {
    Serial.println("[dance] not available (no PSRAM)");
    return;
  }
  if (on == active_) return;
  active_ = on;
  syncTaps();  // on before follow(): the reader starts from what is written next
  if (!on) {
    Serial.println("[dance] off");
    return;
  }
  Serial.printf("[dance] on (%s)\n", dance::skinName(skin_));
  view_.enter();
  dancer_.reset();
  crab_.reset();
  if (!host_) {  // (in host mode the computer's hops feed the tracker, on or off)
    follow(audio_.output());
    freshWhy_ = "dance screen on";
  }
  pacer_.restart();
  scene_ = dancerate::Scene{};  // idle, as the dancer was reset
  targetFps_ = 0;               // logs the rate at the first frame
  lastFrameUs_ = 0;
  frames_ = 0;
  trackerUs_ = 0;
  framesSinceMs_ = millis();
}

void DanceMode::setPrior(float bpm) {
  prior_ = bpm > 0.0f ? bpm : 0.0f;
  tracker_.setPrior(prior_);
  if (prior_ > 0.0f) {
    Serial.printf("[dance] tempo prior %.1f BPM (until the track changes)\n", prior_);
  } else {
    Serial.println("[dance] no tempo prior");
  }
}

void DanceMode::onTrackChanged() {
  if (prior_ > 0.0f) Serial.println("[dance] track changed: tempo prior cleared");
  prior_ = 0.0f;
  // A click track's truth is armed from the track that starts an epoch
  // (restart()). A gapless join keeps the epoch (the beat runs on through a
  // segue), so the truth would be measured against the wrong track: off
  // until the next epoch (a skip, a seek, a start).
  truth_ = false;
  if (!host_) tracker_.setPrior(0.0f);  // (in host mode the tracker has the computer's epoch's)
}

void DanceMode::freeze(int n) {
  frozen_ = n;
  if (n < 0) {
    Serial.println("[dance] following the beat again");
  } else {
    Serial.printf("[dance] frozen at phase %d/8 of a two-beat cycle (beat %s, phi %.3f)\n", n, n >= 8 ? "odd" : "even",
                  (n % 8) / 8.0f);
  }
}

void DanceMode::cycleSkin() {
  const dance::Skin next = dance::nextSkin(skin_);
  if (ready_ && !view_.setSkin(next)) {
    // No PSRAM for the other format: stay as we were if that still works.
    Serial.printf("[dance] no PSRAM for the %s sprite\n", dance::skinName(next));
    ready_ = view_.setSkin(skin_);
    if (!ready_) {
      Serial.println("[dance] dance screen off: no sprite");
      active_ = false;
      syncTaps();
    }
    return;
  }
  // The new skin takes over where the old one's dance weight was, so a
  // switch mid-song doesn't fade out and back in.
  const float w = skin_ == dance::Skin::Crab ? crab_.weight() : dancer_.weight();
  if (next == dance::Skin::Crab) {
    crab_.reset(w);
  } else {
    dancer_.reset(w);
  }
  skin_ = next;
  Serial.printf("[dance] skin: %s\n", dance::skinName(skin_));
}

void DanceMode::setTracking(bool on) {
  if (on && !tracking_) refollow_ = true;  // what the tap holds meanwhile is skipped
  if (!on) fresh_ = true;                  // no grid to dance to meanwhile: the dancer idles
  tracking_ = on;
  syncTaps();
}

void DanceMode::syncTaps() { audio_.setTapsOn(ready_ && active_ && tracking_ && !host_); }

void DanceMode::toggleVerbose() {
  verbose_ = !verbose_;
  Serial.printf("[dance] per-beat log %s\n", verbose_ ? "on" : "off");
}

void DanceMode::follow(Core2AudioBackend::Output output) {
  followed_ = output;
  reader_.attach(audio_.tap(output), static_cast<float>(audio_.sampleRate()));
  fresh_ = true;
  freshWhy_ = "output switch";
}

uint32_t DanceMode::latencyUs(char* how, size_t howLen) const {
  const int64_t us = static_cast<int64_t>(audio_.outputLatencyUs(followed_, how, howLen)) + offsetMs_ * 1000LL;
  return us > 0 ? static_cast<uint32_t>(us) : 0u;
}

void DanceMode::loop(uint32_t nowMs, bool silent) {
  silent_ = silent;
  if (!active_ || !ready_) return;
  if (tracking_ && !host_) {
    const Core2AudioBackend::Output out = audio_.output();
    if (refollow_ || out != followed_ || reader_.tap() != audio_.tap(out)) {
      refollow_ = false;
      follow(out);
    }
    reader_.setSampleRate(static_cast<float>(audio_.sampleRate()));
    reader_.poll(scratch_, kScratchFrames, [this](const TapReader::Run& r) { feed(r); });
  }

  const uint32_t elapsed = nowMs - framesSinceMs_;
  if (elapsed >= 1000) {
    fps_ = frames_ * 1000.0f / elapsed;
    trackerLoad_ = trackerUs_ / (elapsed * 1000.0f);
    frames_ = 0;
    trackerUs_ = 0;
    framesSinceMs_ = nowMs;
  }
  render(nowMs);
}

void DanceMode::restart(const TapReader::Run& run, const char* why) {
  tracker_.reset(run.trackFrame);
  fold_.reset();
  epoch_ = run.epoch;
  fresh_ = false;
  wasLocked_ = false;
  lastBeatIndex_ = 0;
  ++resets_;
  errors_.clear();
  haveError_ = false;

  // A click track: the truth to measure against.
  char path[TrackCatalog::kMaxPath];
  player_.currentPath(path, sizeof(path));
  ClickGen::Spec spec;
  truth_ = strncmp(path, "tone:", 5) == 0 && ClickGen::parse(std::string(path + 5), &spec);
  if (truth_) {
    clicks_.start(44100, spec, 0);
    const double period = 60.0 * 44100.0 / spec.bpm;
    const double first = (static_cast<double>(run.trackFrame) - clicks_.beatFrame(0)) / period;
    nextTruthBeat_ = first > 1.0 ? static_cast<uint32_t>(first) - 1 : 0;
    while (clicks_.beatFrame(nextTruthBeat_) < run.trackFrame) ++nextTruthBeat_;
  }
  const int rate = audio_.sampleRate();
  Serial.printf("[dance] tracker reset (%s) at %.2f s into track %d%s\n", why,
                rate > 0 ? run.trackFrame / static_cast<float>(rate) : 0.0f, player_.currentIndex(),
                truth_ ? ", a click track: measuring the phase error" : "");
}

void DanceMode::feed(const TapReader::Run& run) {
  if (fresh_) {
    restart(run, freshWhy_);
  } else if (run.epoch != epoch_) {
    restart(run, "track change or skip");
  } else if (run.trackFrame != nextFrame_) {
    restart(run, "gap in the audio");
  }
  const int64_t t0 = esp_timer_get_time();
  // Split at the true beats, so each is scored against the grid as it stood
  // when the audio reached it (as in the host tests).
  uint32_t done = 0;
  while (done < run.frames) {
    uint32_t n = run.frames - done;
    if (truth_) {
      const uint32_t at = run.trackFrame + done;
      const uint32_t beat = clicks_.beatFrame(nextTruthBeat_);
      if (static_cast<int32_t>(beat - at) < 0) {  // behind us (can't happen: runs are contiguous)
        ++nextTruthBeat_;
        continue;
      }
      if (beat - at < n) {
        n = beat - at;
        if (n == 0) {
          scoreTruth(beat);
          continue;
        }
      }
    }
    tracker_.process(run.samples + done, n);
    done += n;
  }
  trackerUs_ += static_cast<uint32_t>(esp_timer_get_time() - t0);
  nextFrame_ = run.trackFrame + run.frames;
  lockChanged(static_cast<float>(audio_.sampleRate()));
  if (verbose_) logBeat();
}

// "locked" or "lost the beat", when that changes.
void DanceMode::lockChanged(float rate) {
  if (tracker_.locked() == wasLocked_) return;
  wasLocked_ = tracker_.locked();
  if (wasLocked_) {
    // (In host mode with the epoch: tools/usb_viz.py --measure scores it there.)
    char epoch[32] = "";
    if (host_) snprintf(epoch, sizeof(epoch), " (computer: epoch %lu)", static_cast<unsigned long>(hostEpoch_));
    Serial.printf("[dance] locked: %.2f BPM, %.2f s after the reset%s\n", tracker_.bpm(),
                  tracker_.framesSinceReset() / rate, epoch);
  } else {
    Serial.printf("[dance] lost the beat (confidence %.2f)\n", tracker_.confidence());
  }
}

void DanceMode::scoreTruth(uint32_t frame) {
  ++nextTruthBeat_;
  if (!tracker_.locked()) return;
  const double period = 60.0 * 44100.0 / clicks_.spec().bpm;
  const auto ms = static_cast<float>(tracker_.grid().errorAgainst(frame, period) * 1000.0 / 44100.0);
  errors_.add(ms, millis());
  lastErrorMs_ = ms;
  haveError_ = true;
}

// Verbose: a line per beat of the tracker's grid. In host mode with the
// epoch, the next beat's frame in it (to a tenth) and the hop it was
// predicted at, so the computer can score it against its own truth
// (tools/usb_viz.py --measure).
void DanceMode::logBeat() {
  const BeatTracker::Grid g = tracker_.grid();
  if (!g.valid || g.beatIndex == lastBeatIndex_) return;
  lastBeatIndex_ = g.beatIndex;
  char err[24] = "";
  if (truth_ && haveError_) snprintf(err, sizeof(err), " err=%+.1fms", lastErrorMs_);
  if (host_) {
    const double frame = static_cast<int32_t>(g.beatFrame) + static_cast<double>(g.beatFrac);  // (may be negative)
    Serial.printf("[beat] #%ld next at %.3fs (epoch %lu, frame %.1f, hop %lu) bpm=%.2f conf=%.2f%s\n",
                  static_cast<long>(g.beatIndex), frame / hostRate_, static_cast<unsigned long>(hostEpoch_), frame,
                  static_cast<unsigned long>(lastHop_), tracker_.bpm(), tracker_.confidence(),
                  tracker_.locked() ? " locked" : "");
    return;
  }
  Serial.printf("[beat] #%ld next at %.3fs bpm=%.2f conf=%.2f%s%s\n", static_cast<long>(g.beatIndex),
                g.beatFrame / static_cast<float>(audio_.sampleRate()), tracker_.bpm(), tracker_.confidence(),
                tracker_.locked() ? " locked" : "", err);
}

// ---- host mode (the USB visualizer: docs/USB-VISUALIZER.md) ----

void DanceMode::setHost(bool on, const HostStats* stats) {
  if (on == host_) return;
  host_ = on;
  hostStats_ = on ? stats : nullptr;
  hostEpochOn_ = false;
  hostLog_ = 0;
  resetLogged_ = false;
  resetsUnlogged_ = 0;
  wasLocked_ = false;
  fresh_ = true;
  if (on) {
    if (frozen_ >= 0) Serial.println("[dance] unfrozen: the computer drives the dancer");
    frozen_ = -1;
    truth_ = false;  // (the computer measures: tools/usb_viz.py --measure)
    freshWhy_ = "the computer";
    clock_.start(hostRate_);
    syncTaps();  // off: the hops come from the computer
    return;
  }
  // Back to the Core2's own audio: its rate (the rate converter's 44.1
  // kHz), the console's prior, the taps on (if the tab is up) and followed
  // afresh.
  if (!tracker_.setSampleRate(static_cast<uint32_t>(audio_.sampleRate()))) {
    Serial.printf("[dance] no PSRAM for the tracker at %d Hz: the dancer idles\n", audio_.sampleRate());
  }
  tracker_.setPrior(prior_);
  syncTaps();
  if (active_) follow(audio_.output());
  freshWhy_ = "the computer's visualizer ended";
}

void DanceMode::hostEpoch(uint32_t epoch, uint32_t rate, float prior) {
  if (!host_) return;
  if (!tracker_.setSampleRate(rate)) {
    Serial.printf("[dance] no PSRAM for the tracker at %lu Hz: the dancer idles\n", static_cast<unsigned long>(rate));
  }
  tracker_.setPrior(prior);
  clock_.start(rate);
  hostEpoch_ = epoch;
  hostRate_ = rate;
  hostPrior_ = prior;
  hostEpochOn_ = true;
  fresh_ = true;  // idle until the epoch's first hop resets the tracker
  freshWhy_ = "the computer: a new epoch";
}

void DanceMode::hostPrior(float prior) {
  if (!host_) return;
  hostPrior_ = prior;
  tracker_.setPrior(prior);
  Serial.printf("[dance] tempo prior %.1f BPM (the computer's, for this epoch)\n", prior);
}

void DanceMode::hostForget() {
  hostEpochOn_ = false;
  fresh_ = true;
  freshWhy_ = "the computer started over";
  clock_.start(hostRate_);
}

void DanceMode::hostRestart(uint32_t hop, bool gap) {
  tracker_.reset(hop * tracker_.config().hop);
  fold_.reset();
  fresh_ = false;
  wasLocked_ = false;
  lastBeatIndex_ = 0;
  ++resets_;
  // A gap's at most a line a second (a sender losing lines would reset on
  // each); an epoch's first hop always (one per epoch: a seek, a new track).
  const uint32_t now = millis();
  if (gap && resetLogged_ && now - resetLogMs_ < 1000) {
    ++resetsUnlogged_;
    return;
  }
  char more[40] = "";
  if (resetsUnlogged_) snprintf(more, sizeof(more), "; %lu more not logged", static_cast<unsigned long>(resetsUnlogged_));
  Serial.printf("[dance] tracker reset (computer: epoch %lu, %s) at hop %lu, %lu Hz%s%s\n",
                static_cast<unsigned long>(hostEpoch_), gap ? "a gap in its hops" : "its first hop",
                static_cast<unsigned long>(hop), static_cast<unsigned long>(hostRate_),
                hostPrior_ > 0.0f ? ", with a tempo prior" : "", more);
  if (gap) {
    resetLogged_ = true;
    resetLogMs_ = now;
  }
  resetsUnlogged_ = 0;
}

void DanceMode::hostHop(uint32_t hop, float low, float mid, bool restart, bool gap) {
  if (!host_ || !hostEpochOn_ || !ready_) return;
  if (restart || fresh_) hostRestart(hop, gap);
  const int64_t t0 = esp_timer_get_time();
  tracker_.feedHop(low, mid);
  trackerUs_ += static_cast<uint32_t>(esp_timer_get_time() - t0);
  lastHop_ = hop;
  lockChanged(static_cast<float>(hostRate_));
  if (verbose_ || hostLog_ >= 1) logBeat();
}

void DanceMode::hostClock(uint32_t nowUs, int32_t heard, bool playing) {
  if (!host_ || !hostEpochOn_) return;
  clock_.sample(nowUs, heard, playing);
}

void DanceMode::setHostLog(uint8_t level) {
  hostLog_ = level;
  flashBeat_ = INT64_MIN;  // the next beat drawn gets a [flash] line
  lastBeatIndex_ = tracker_.grid().beatIndex;  // from the next beat on
  Serial.printf("[dance] the computer's beat log: %s\n", level >= 2 ? "beats and flashes" : level ? "beats" : "off");
}

void DanceMode::render(uint32_t nowMs) {
  // The rate for what the last frame showed, at the clock that runs (the
  // console's Pc can change it).
  const uint32_t mhz = getCpuFrequencyMhz();
  const dancerate::Mode mode = dancerate::mode(scene_);
  const uint32_t fps = dancerate::fps(mode, mhz);
  if (fps != targetFps_) {
    targetFps_ = fps;
    if (mode == dancerate::Mode::Idle) {
      Serial.printf("[dance] %lu fps (idle)\n", static_cast<unsigned long>(fps));
    } else {
      Serial.printf("[dance] %lu fps (dancing at %lu MHz)\n", static_cast<unsigned long>(fps),
                    static_cast<unsigned long>(mhz));
    }
  }
  // On deadlines (the loop's delay(5) and the rest would otherwise add a few
  // ms to every frame); a new rate starts over from the last frame drawn.
  if (!pacer_.due(nowMs, dancerate::periodMs(fps))) return;
  const auto nowUs = static_cast<uint32_t>(esp_timer_get_time());
  const float dt = lastFrameUs_ ? static_cast<int32_t>(nowUs - lastFrameUs_) * 1e-6f : 0.0f;
  lastFrameUs_ = nowUs;

  bool flash = false, dancing = false, beat = false;
  const bool isCrab = skin_ == dance::Skin::Crab;
  dance::Pose pose;
  crab::Pose crabPose;
  float weight = 1.0f;
  if (frozen_ >= 0) {
    const float phi = (frozen_ % 8) / 8.0f;
    if (isCrab) {
      crabPose = crab::Crab::frozen(phi, frozen_ >= 8);
    } else {
      pose = dancer_.frozen(phi, frozen_ >= 8);
    }
    flash = phi < kFlashBeats;
    dancing = true;
  } else {
    // The moment this frame will be on the LCD: after drawing and half the push.
    const auto lead = static_cast<uint32_t>(drawUs_ + pushUs_ / 2.0f) + kAimEarlyUs;
    const BeatTracker::Grid g = tracker_.grid();
    double beats = 0.0;
    if (host_) {
      // The computer's heard frame at that moment (its output's latency is
      // in what it sends; the console's y<ms> moves it, + later).
      const HostClock::Heard heard = clock_.at(nowUs, static_cast<int32_t>(lead) - offsetMs_ * 1000);
      beat = heard.valid && g.valid && !fresh_ && hostEpochOn_;
      if (beat) beats = g.beatIndex + g.beatsAt(static_cast<uint32_t>(heard.frame), heard.frac);
      if (beat && hostLog_ >= 2) {
        // A [flash] line per beat drawn: the clock's frame now (and the one
        // aimed at), for the computer to set against its own clock.
        const auto n = static_cast<int64_t>(std::floor(beats));
        if (n != flashBeat_) {
          flashBeat_ = n;
          const HostClock::Heard now = clock_.at(nowUs);
          Serial.printf("[flash] #%lld heard=%ld aim=%ld\n", static_cast<long long>(n), static_cast<long>(now.frame),
                        static_cast<long>(heard.frame));
        }
      }
    } else {
      char how[48];
      const uint32_t latency = latencyUs(how, sizeof(how));
      const TapReader::Audible heard = reader_.audibleAt(nowUs + lead, latency);
      beat = heard.valid && g.valid && !fresh_ && heard.epoch == epoch_;
      if (beat) beats = g.beatIndex + g.beatsAt(heard.trackFrame, heard.frac);
    }
    dance::Step step;
    if (beat) {
      const float bpm = tracker_.bpm();
      step = dance::danceStep(beats, bpm, fold_.apply(bpm));
    }
    // Danced only while the tracker is locked: an unlocked grid is a guess
    // (its confidence may sit just under the lock for a while), and the
    // dance weight's scale is the locked one (dance::danceWeight).
    const bool lockedBeat = beat && tracker_.locked();
    if (isCrab) {
      crabPose = crab_.update(step.phi, step.odd, tracker_.confidence(), lockedBeat, dt);
      weight = crab_.weight();
    } else {
      pose = dancer_.update(step.phi, step.odd, tracker_.confidence(), lockedBeat, dt);
      weight = dancer_.weight();
    }
    dancing = weight > dancerate::kIdleWeight;
    flash = beat && dancing && step.phi < kFlashBeats;
  }
  scene_.frozen = frozen_ >= 0;
  scene_.beat = beat;
  scene_.locked = tracker_.locked();
  scene_.weight = weight;
  const int64_t t0 = esp_timer_get_time();
  if (isCrab) {
    view_.drawCrab(crabPose, weight, flash);
  } else {
    view_.drawFigure(pose, flash, dancing);
  }
  const int64_t t1 = esp_timer_get_time();
  view_.push();
  const int64_t t2 = esp_timer_get_time();
  drawUs_ += (static_cast<float>(t1 - t0) - drawUs_) * 0.1f;
  pushUs_ += (static_cast<float>(t2 - t1) - pushUs_) * 0.1f;
  ++frames_;
}

void DanceMode::printStats(uint32_t nowMs) {
  char how[48];
  const uint32_t latency = latencyUs(how, sizeof(how));
  const float rate = static_cast<float>(host_ ? hostRate_ : audio_.sampleRate());
  // Where the dancer's time comes from: the output's latency, or in host
  // mode the computer's session and its heard clock.
  char source[260];
  if (host_) {
    const HostStats st = hostStats_ ? *hostStats_ : HostStats{};
    const HostClock::Stats c = clock_.stats(static_cast<uint32_t>(esp_timer_get_time()));
    char epoch[48] = "no epoch yet";
    if (hostEpochOn_) {
      snprintf(epoch, sizeof(epoch), "epoch=%lu %luHz prior=%.0f", static_cast<unsigned long>(hostEpoch_),
               static_cast<unsigned long>(hostRate_), hostPrior_);
    }
    snprintf(source, sizeof(source),
             "viz %s hops=%lu gaps=%lu dup=%lu stale=%lu bad=%lu errs=%lu | clock %s age=%lums snaps=%lu "
             "slew=%+.1f%% spread=%.1fms offset=%+dms",
             epoch, static_cast<unsigned long>(st.hops), static_cast<unsigned long>(st.gaps),
             static_cast<unsigned long>(st.dup), static_cast<unsigned long>(st.stale), static_cast<unsigned long>(st.bad),
             static_cast<unsigned long>(st.errors), c.valid ? "ok" : "idle", static_cast<unsigned long>(c.ageMs),
             static_cast<unsigned long>(c.snaps), c.slew * 100.0f, c.spreadMs, offsetMs_);
  } else {
    snprintf(source, sizeof(source), "latency=%.1fms (%s) offset=%+dms", latency / 1000.0f, how, offsetMs_);
  }
  char lockAfter[16] = "-";
  if (tracker_.framesToLock() >= 0) snprintf(lockAfter, sizeof(lockAfter), "%.2fs", tracker_.framesToLock() / rate);
  char error[80] = "err=n/a (not a click track)";
  if (host_) snprintf(error, sizeof(error), "err=n/a (the computer measures it)");
  if (truth_) {
    const auto s = errors_.summary(nowMs, kErrorWindowMs);
    if (s.count > 0) {
      snprintf(error, sizeof(error), "err_med=%.1fms err_p95=%.1fms err_mean=%+.1fms n=%lu", s.medianAbs, s.p95Abs,
               s.mean, static_cast<unsigned long>(s.count));
    } else {
      snprintf(error, sizeof(error), "err=n/a (not locked)");
    }
  }
  const diag::Heap h = diag::heap();
  Serial.printf("[dance] %s skin=%s fps=%.1f/%lu (%s) draw=%.1fms push=%.1fms | "
                "bpm=%.2f conf=%.2f %s lock_after=%s %s | %s | tracker=%.2f%% resets=%lu lost=%lu | ram=%luK min=%luK%s\n",
                active_ ? "on" : "off", dance::skinName(skin_), fps_, static_cast<unsigned long>(targetFps_),
                dancerate::modeName(dancerate::mode(scene_)), drawUs_ / 1000.0f, pushUs_ / 1000.0f,
                tracker_.bpm(), tracker_.confidence(), tracker_.locked() ? "locked" : "unlocked", lockAfter, error,
                source, trackerLoad_ * 100.0f, static_cast<unsigned long>(resets_),
                static_cast<unsigned long>(reader_.lostFrames()), static_cast<unsigned long>(h.internalFree / 1024),
                static_cast<unsigned long>(h.internalMin / 1024), silent_ ? " | silent" : "");
}
