// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "BeatTracker.h"
#include "ClickGen.h"
#include "CrabPose.h"
#include "DanceRate.h"
#include "DancePose.h"
#include "DanceSkin.h"
#include "HostClock.h"
#include "HostLink.h"
#include "PlaybackController.h"
#include "RollingStats.h"
#include "TapReader.h"
#include "audio/Core2AudioBackend.h"
#include "ui/DanceView.h"

// The dancing character (proof of concept, docs/MASCOT-POC.md), the Dance
// tab's dancer (ui/DancePage draws the page around its box), on the loop task: the crab (the default) or the stick figure, swapped by
// cycleSkin(). Every pass while it's on: the new audio from the active
// output's tap goes to the BeatTracker (reset on an output switch, a track
// change, a skip, or frames lost), and the dancer is drawn for the moment
// the listener hears: the tap's clock minus the output's latency, a little
// early for the LCD. On the click tracks the true beat is known, so the
// tracker's phase error is measured as it plays.
//
// Its frame rate (DanceRate, ENERGY.md item 8): 10 fps while it idles, 30
// while it dances at 240 MHz, 24 below that; a change is logged ("[dance]
// 24 fps (dancing at 160 MHz)").
//
// The outputs write their taps only while it is on and tracking: it
// switches them (off from boot, on with the Dance tab, off with the tab,
// the screen going dark, or Pk0; ENERGY.md item 9).
//
// Host mode (the USB visualizer, docs/USB-VISUALIZER.md; app/UsbViz drives
// it): a computer plays the music and sends the tracker's hop energies and
// a heard clock over USB. The taps are off; the tracker is fed the
// computer's hops (BeatTracker::feedHop(), at the epoch's rate: 44.1 or 48
// kHz) and the dancer drawn for HostClock's frame in place of the tap's.
class DanceMode {
public:
  DanceMode(Core2AudioBackend& audio, PlaybackController& player) : audio_(audio), player_(player) {}

  // Tracker scratch and the sprite, in PSRAM. False: can't dance.
  bool begin();
  void setActive(bool on);
  bool active() const { return active_; }
  bool ready() const { return ready_; }
  // Every loop pass. Draws a frame when one is due (`silent`: the silent
  // test mode, noted in the stats).
  void loop(uint32_t nowMs, bool silent);

  // Console.
  void setPrior(float bpm);          // 0 clears; also cleared by a track change
  void onTrackChanged();
  void setOffsetMs(int ms) { offsetMs_ = ms; }
  void freeze(int n);                // phase n/8 of a two-beat cycle; -1 unfreezes
  void cycleSkin();                  // crab -> stick -> crab (console m, a tap on the box)
  dance::Skin skin() const { return skin_; }
  void toggleVerbose();
  // Power measurements (the console's Pk): the beat tracker stops reading
  // the tap and the taps stop (the dancer idles); on again it starts over
  // from what plays.
  void setTracking(bool on);
  bool tracking() const { return tracking_; }
  // What the Dance page shows around the dancer.
  float bpm() const { return tracker_.bpm(); }
  float confidence() const { return tracker_.confidence(); }
  bool locked() const { return tracker_.locked(); }
  bool frozen() const { return frozen_ >= 0; }
  float fps() const { return fps_; }
  uint32_t targetFps() const { return targetFps_; }
  void printStats(uint32_t nowMs);   // the [dance] line

  // For screenshots of the figure's box.
  DanceView& view() { return view_; }

  // ---- host mode (the USB visualizer) ----
  // On: the taps off, a freeze cleared, nothing measured against a click
  // track, the dancer idle until the computer's first epoch and hop. Off:
  // the tracker back at the output's rate with the console's prior, the
  // tap's audio followed afresh, the per-beat log back to the console's.
  // `stats`: the session's counters, for the [dance] line.
  void setHost(bool on, const HostStats* stats = nullptr);
  bool host() const { return host_; }
  // A new epoch: the tracker at `rate` (44100 or 48000) with `prior` (0:
  // none), the clock started over; the dancer idles until its first hop.
  void hostEpoch(uint32_t epoch, uint32_t rate, float prior);
  void hostPrior(float prior);  // the same epoch, a prior that came late
  // The computer started over (another session id): no epoch until its next.
  void hostForget();
  // A hop's energies. `restart`: the tracker starts over at this hop first
  // (the epoch's first, or after `gap`).
  void hostHop(uint32_t hop, float low, float mid, bool restart, bool gap);
  // An @c, stamped `nowUs` (esp_timer) as it arrived.
  void hostClock(uint32_t nowUs, int32_t heard, bool playing);
  // @log: 1 a [beat] line per tracker beat, 2 also a [flash] line per beat drawn.
  void setHostLog(uint8_t level);

private:
  void benchTracker();
  void follow(Core2AudioBackend::Output output);
  void syncTaps();  // the outputs' taps on only while on and tracking
  void feed(const TapReader::Run& run);
  void restart(const TapReader::Run& run, const char* why);
  void scoreTruth(uint32_t frame);
  void logBeat();
  // `gap`: a gap in the hops (logged at most once a second); otherwise the
  // epoch's first hop (always logged: --measure learns the epoch from it).
  void hostRestart(uint32_t hop, bool gap);
  void lockChanged(float rate);
  void render(uint32_t nowMs);
  uint32_t latencyUs(char* how, size_t howLen) const;

  Core2AudioBackend& audio_;
  PlaybackController& player_;
  DanceView view_;
  BeatTracker tracker_;
  TapReader reader_;
  dance::Skin skin_ = dance::kDefaultSkin;
  dance::Dancer dancer_;        // the stick figure's state
  crab::Crab crab_;             // the crab's
  dance::TempoFold fold_;       // the dance tempo's octave, kept between frames
  int16_t* scratch_ = nullptr;  // PSRAM
  bool ready_ = false;
  bool active_ = false;
  bool tracking_ = true;
  bool refollow_ = false;  // tracking came back: attach to the tap afresh

  // What the tracker is following.
  Core2AudioBackend::Output followed_ = Core2AudioBackend::Output::Speaker;
  bool fresh_ = true;           // the next run starts the tracker over
  const char* freshWhy_ = "start";
  uint32_t epoch_ = 0;
  uint32_t nextFrame_ = 0;      // track frame that continues what was fed
  uint32_t resets_ = 0;
  bool wasLocked_ = false;
  int32_t lastBeatIndex_ = 0;
  float prior_ = 0.0f;

  // Truth, on the click tracks.
  bool truth_ = false;
  ClickGen clicks_;
  uint32_t nextTruthBeat_ = 0;
  float lastErrorMs_ = 0.0f;
  bool haveError_ = false;
  RollingStats<64> errors_;     // phase error (ms), stamped with millis()

  // Drawing.
  dancerate::Pacer pacer_;
  dancerate::Scene scene_;      // what the last frame showed: the next one's rate
  uint32_t targetFps_ = 0;      // the rate now (0: none yet since on)
  uint32_t lastFrameUs_ = 0;
  int frozen_ = -1;
  int offsetMs_ = 0;
  bool verbose_ = false;
  bool silent_ = false;
  // Measured, smoothed (us), and counted since the last stats line.
  float drawUs_ = 2000.0f, pushUs_ = 8000.0f;
  uint32_t frames_ = 0;
  uint32_t framesSinceMs_ = 0;
  uint32_t trackerUs_ = 0;      // time in tracker_.process() since the last stats line
  float fps_ = 0.0f;
  float trackerLoad_ = 0.0f;

  // Host mode.
  bool host_ = false;
  const HostStats* hostStats_ = nullptr;
  HostClock clock_;
  bool hostEpochOn_ = false;    // an epoch to dance in
  uint32_t hostEpoch_ = 0;
  uint32_t hostRate_ = 44100;
  float hostPrior_ = 0.0f;
  uint32_t lastHop_ = 0;
  uint8_t hostLog_ = 0;
  uint32_t resetLogMs_ = 0;     // the last gap reset logged (at most one a second)
  uint32_t resetsUnlogged_ = 0;
  bool resetLogged_ = false;
  int64_t flashBeat_ = 0;       // the beat the last [flash] line was for
};
