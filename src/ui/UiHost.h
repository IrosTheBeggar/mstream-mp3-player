#pragma once
#include <cstdint>

#include "ButtonPolicy.h"
#include "OutputModel.h"
#include "PlaybackController.h"
#include "QueueView.h"

// What the UI reads of the rest of the firmware, and the few things it asks
// of it. main.cpp implements UiHost; the UI takes one AppState snapshot a
// loop pass (spec §10.1), and every screen redraws only what differs from
// what it drew. The player, the queue and the library the UI reaches
// directly (their edits are the Library and Queue screens' whole point);
// everything about the outputs, Bluetooth, the card and the battery goes
// through here.
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
  // Bluetooth for the Output card (OutputModel): what the link is doing,
  // and what the listener asked for (btSession.wanted(): the audio moves
  // to the headphones once they are linked; until then it stays put).
  BtLink btLink;
  BtSession btSession;
  char btDetail[40] = "";        // "SBC 44.1 kHz, 175 ms" while connected
  // Storage and the library.
  bool card = false;             // a microSD card is mounted (not the flash fallback)
  uint32_t libraryTracks = 0;
  // The rest.
  uint8_t battery = 0;
  bool charging = false;
  uint32_t ringMs = 0;           // audio buffered
  uint32_t underruns = 0;
  bool ringMatters = false;      // playing, and the ring has filled once (a low ring is a risk)
  ButtonPolicy::Feedback feedback;  // the touch buttons' last hold (the HUD)
};

// About (the Output tab): what the device is and has.
struct AboutInfo {
  char storage[48] = "";   // "microSD card, 29.7 GB"
  char version[48] = "";   // "mstream-mp3-player 0.4, built Sep 27 2026"
  char bluetooth[48] = ""; // "SPYDRONE (12:34:56:78:9A:BC)"
  uint32_t ramFree = 0, ramMin = 0, psramFree = 0;
};

class UiHost {
public:
  virtual void snapshot(AppState& s) = 0;
  virtual void playPause() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;
  // The active output's volume, by `delta` %.
  virtual void stepVolume(int delta) = 0;
  // One output's volume, by `delta` % (the Output tab's per-output sheet),
  // whichever is active.
  virtual void stepOutputVolume(bool bluetooth, int delta) = 0;
  // Makes Bluetooth (or the speaker) the output. To the speaker pauses
  // first, like the B hold: the music never moves out loud by itself. To
  // Bluetooth while the headphones aren't connected only connects them:
  // the audio stays where it is until they are linked. False: refused
  // (silent test mode; no headphones paired since Forget).
  virtual bool selectOutput(bool bluetooth) = 0;
  // The touch calibration screen (it takes the display; the UI resumes
  // when it closes).
  virtual void openCalibration() = 0;

  // ---- Bluetooth (the Output card and the Pair screen) ----
  virtual void btConnect() = 0;     // Connect, Try again
  virtual void btDisconnect() = 0;  // Disconnect, Cancel: the audio moves to the speaker, paused
  virtual void btForget() = 0;      // after Forget's second tap
  virtual void btPairScan(bool on) = 0;
  // The scan's list, copied; returns its version.
  virtual uint32_t btScan(BtScanList& out) = 0;
  // Pair with a device the scan listed (it replaces the remembered pair
  // once it is linked). False: nothing to pair, it is the device linked
  // now (it is made the output instead).
  virtual bool btPairWith(const BtDevice& d) = 0;

  // ---- the card, the library ----
  // "Try again" with no card: false if there still is none (with one, the
  // firmware restarts to use it).
  virtual bool retryCard() = 0;
  // "Try again" with no music: walks /music again (the queue follows).
  virtual void rescanLibrary() = 0;
  // What the player learned of track lengths (the Queue's summary).
  virtual const queueview::DurationBook& durations() = 0;
  virtual void about(AboutInfo& a) = 0;

protected:
  ~UiHost() = default;
};

}  // namespace ui
