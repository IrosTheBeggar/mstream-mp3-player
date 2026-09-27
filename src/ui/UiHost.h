#pragma once
#include <cstdint>

#include "ButtonPolicy.h"
#include "PlaybackController.h"

// What the UI reads of the rest of the firmware, and the few things it asks
// of it. main.cpp implements UiHost; the UI takes one AppState snapshot a
// loop pass (spec §10.1), and every screen redraws only what differs from
// what it drew. The player, the queue and the library the UI reaches
// directly (their edits are the Library and Queue screens' whole point);
// everything about the outputs, Bluetooth and the battery goes through here.
namespace ui {

struct AppState {
  // The player and the queue.
  PlayState play = PlayState::Stopped;
  bool failed = false;           // the current track can't be played
  uint32_t trackId = 0xFFFFFFFFu;  // TrackCatalog id of the current entry
  uint32_t currentKey = 0xFFFFFFFFu;
  int32_t current = -1;          // queue position
  uint32_t queueSize = 0;
  uint32_t upNext = 0;
  uint32_t contentVersion = 0;   // QueueModel's
  uint32_t positionVersion = 0;
  uint32_t positionMs = 0;
  uint32_t durationMs = 0;       // 0: not known (yet)
  // The outputs.
  bool onBluetooth = false;
  bool btConnected = false;
  bool btLost = false;           // the headphones dropped while they were the output
  char btName[32] = "";
  uint8_t volume = 0;            // the active output's
  uint8_t speakerVolume = 0;
  uint8_t btVolume = 0;
  bool headphonesSetVolume = false;  // AVRCP absolute volume
  bool silent = false;           // silent test mode (console z)
  // The rest.
  uint8_t battery = 0;
  bool charging = false;
  uint32_t ringMs = 0;           // audio buffered
  uint32_t underruns = 0;
  bool ringMatters = false;      // playing, and the ring has filled once (a low ring is a risk)
  ButtonPolicy::Feedback feedback;  // the touch buttons' last hold (the HUD)
};

class UiHost {
public:
  virtual void snapshot(AppState& s) = 0;
  virtual void playPause() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;
  // The active output's volume, by `delta` %.
  virtual void stepVolume(int delta) = 0;
  // Makes Bluetooth (or the speaker) the output. To the speaker pauses
  // first, like the B hold: the music never moves out loud by itself.
  virtual void selectOutput(bool bluetooth) = 0;
  // The touch calibration screen (it takes the display; the UI resumes
  // when it closes).
  virtual void openCalibration() = 0;
  // Forgets the paired headphones and restarts to look for new ones.
  virtual void forgetHeadphones() = 0;

protected:
  ~UiHost() = default;
};

}  // namespace ui
