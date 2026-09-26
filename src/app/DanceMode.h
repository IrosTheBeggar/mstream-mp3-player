#pragma once
#include <Arduino.h>

#include "BeatTracker.h"
#include "ClickGen.h"
#include "DancePose.h"
#include "PlaybackController.h"
#include "RollingStats.h"
#include "TapReader.h"
#include "audio/Core2AudioBackend.h"
#include "ui/DanceView.h"

// The dancing stick figure (proof of concept, docs/MASCOT-POC.md), on the
// loop task. Every pass while it's on: the new audio from the active
// output's tap goes to the BeatTracker (reset on an output switch, a track
// change, a skip, or frames lost), and ~30 times a second the figure is drawn
// for the moment the listener hears: the tap's clock minus the output's
// latency, a little early for the LCD. On the click tracks the true beat is
// known, so the tracker's phase error is measured as it plays.
class DanceMode {
public:
  DanceMode(Core2AudioBackend& audio, PlaybackController& player) : audio_(audio), player_(player) {}

  // Tracker scratch and the sprite, in PSRAM. False: can't dance.
  bool begin();
  void setActive(bool on);
  bool active() const { return active_; }
  // Every loop pass. Draws a frame when one is due; `silent` goes in the title.
  void loop(uint32_t nowMs, bool silent);

  // Console.
  void setPrior(float bpm);          // 0 clears; also cleared by a track change
  void onTrackChanged();
  void setOffsetMs(int ms) { offsetMs_ = ms; }
  void freeze(int n);                // phase n/8 of a two-beat cycle; -1 unfreezes
  void toggleVerbose();
  void printStats(uint32_t nowMs);   // the [dance] line

  // For screenshots of the figure's box.
  DanceView& view() { return view_; }

private:
  void benchTracker();
  void follow(Core2AudioBackend::Output output);
  void feed(const TapReader::Run& run);
  void restart(const TapReader::Run& run, const char* why);
  void scoreTruth(uint32_t frame);
  void logBeat();
  void render(uint32_t nowMs);
  uint32_t latencyUs(char* how, size_t howLen) const;

  Core2AudioBackend& audio_;
  PlaybackController& player_;
  DanceView view_;
  BeatTracker tracker_;
  TapReader reader_;
  dance::Dancer dancer_;
  dance::TempoFold fold_;       // the dance tempo's octave, kept between frames
  int16_t* scratch_ = nullptr;  // PSRAM
  bool ready_ = false;
  bool active_ = false;

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
  uint32_t lastFrameMs_ = 0;
  uint32_t lastFrameUs_ = 0;
  uint32_t lastStatusMs_ = 0;
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
};
