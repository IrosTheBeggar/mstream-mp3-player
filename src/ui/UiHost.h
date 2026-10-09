// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "ButtonPolicy.h"
#include "CardFormat.h"
#include "DeviceInfo.h"
#include "OutputModel.h"
#include "PlayGate.h"
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
  // The player and the queue (Waiting: a play waits for the headphones).
  PlayState play = PlayState::Stopped;
  bool failed = false;           // the current track can't be played
  bool seekable = true;          // ... can start part of the way in (PlaybackController::seekable(): the entry's path asked of the backend; the seek bar's knob)
  uint32_t trackId = 0xFFFFFFFFu;  // TrackCatalog id of the current entry
  uint32_t currentKey = 0xFFFFFFFFu;
  int32_t current = -1;          // queue position
  uint32_t queueSize = 0;
  uint32_t upNext = 0;
  uint32_t contentVersion = 0;   // QueueModel's
  uint32_t positionVersion = 0;
  uint32_t positionMs = 0;
  uint32_t durationMs = 0;       // 0: not known (yet)
  // Shuffle and repeat (docs/QUEUE-MODES.md): the queue shuffled; repeat
  // as PlaybackController::Repeat (0 Off, 1 All, 2 One).
  bool shuffle = false;
  uint8_t repeat = 0;
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
  // Play while the headphones aren't connected (PlayGate): waiting, or the
  // last wait failed; `gateFailures` counts the failures (the notice, once each).
  PlayGate::State gate = PlayGate::State::Idle;
  uint32_t gateFailures = 0;
  char btDetail[40] = "";        // "SBC 44.1 kHz, 175 ms" while connected
  // Storage and the library.
  bool card = false;             // a microSD card is mounted (not the flash fallback)
  // None mounted: what the card that is in is (LocalStorage::cardKind(),
  // the empty state's message); Unreadable with none in, or one mounted.
  cardformat::Kind cardKind = cardformat::Kind::Unreadable;
  uint32_t libraryTracks = 0;
  // The rest.
  uint8_t battery = 0;
  bool charging = false;
  uint32_t ringMs = 0;           // audio buffered
  uint32_t underruns = 0;
  bool ringMatters = false;      // playing, and the ring has filled once (a low ring is a risk)
  ButtonPolicy::Feedback feedback;  // the touch buttons' last hold (the HUD)
  // The screen's settings (ScreenPower's choices: "Screen off after",
  // "Brightness"), for the Output tab's rows.
  uint8_t screenTimeout = 1;
  uint8_t brightness = 1;
  // The sleep timer (SleepTimer; ENERGY.md section 3): running (counting,
  // armed for an end, or fading), fading, the sheet's outlined choice
  // (SleepSheet's 0-7, -1 none), the "..." row's state ("Off", "23 min",
  // "End of track"), the Sleep timer sheet's title after "Sleep timer: "
  // ("23 min left", "end of track", "fading", "off"), and the moon's text
  // on Now Playing ("23 min", "track", "fading"; "" when it doesn't run).
  // `sleepCanExtend`: +10 min would do something (SleepTimer::canExtend():
  // not on End of album / queue before its last track).
  bool sleepRunning = false;
  bool sleepFading = false;
  bool sleepCanExtend = false;
  int8_t sleepPick = -1;
  char sleepRow[16] = "Off";
  char sleepTitle[24] = "off";
  char sleepShort[12] = "";
  // The idle power-off (IdlePolicy; ENERGY.md item 4): the setting ("Turn
  // off when idle": its choice), and while it warns (its last 30 s), the
  // seconds left (0: no warning).
  uint8_t idleOff = 1;
  uint8_t idleWarnS = 0;
  // CPU speed and Bluetooth power (PowerChoices; ENERGY.md items 6 and 7):
  // the speed saved (the next boot's) and the clock set at boot (they
  // differ after the console's Pcb, until a restart), the Bluetooth power choice, and
  // whether the link that is up still has another choice's levels (the new
  // one applies from the next connection).
  uint16_t cpuMhz = 160;
  uint16_t cpuRunMhz = 160;
  uint8_t btPower = 1;
  bool btPowerPending = false;
};

// About (the Output tab): what the device is and has.
struct AboutInfo {
  char storage[48] = "";   // "microSD card, 29.7 GB"
  char version[40] = "";   // "v0.5.0", "v0.5.0-3-gabc1234-dirty" (app/Version)
  char built[12] = "";     // the commit's date, "2026-09-30"
  char elf[12] = "";       // the ELF's SHA-256, 8 hex digits
  char bluetooth[48] = ""; // "SPYDRONE (12:34:56:78:9A:BC)"
  char power[48] = "";     // "160 MHz; Normal (-12..+3 dBm)" (PowerChoices::aboutText())
  uint32_t ramFree = 0, ramMin = 0, psramFree = 0;
};

class UiHost {
public:
  virtual void snapshot(AppState& s) = 0;
  // Play / pause (while play waits for the headphones: cancels the wait).
  virtual void playPause() = 0;
  // Play, not a toggle ("Couldn't reach" notice's Try again): paused or
  // stopped, it plays (or waits for the headphones again).
  virtual void play() = 0;
  // Play on speaker: the listener's explicit choice while play waits for the
  // headphones, or after they couldn't be reached. The speaker becomes the
  // output (paused first, the connection on its way cancelled), then plays
  // at its own volume.
  virtual void playOnSpeaker() = 0;
  virtual void next() = 0;
  virtual void prev() = 0;
  // Now Playing's playback menu (docs/QUEUE-MODES.md): shuffle on or off,
  // the repeat mode (PlaybackController::Repeat's value); applied, saved
  // (repeat in NVS; shuffle with the queue's file) and logged.
  virtual void setShuffle(bool on) = 0;
  virtual void setRepeat(uint8_t mode) = 0;
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
  // when it closes): the crosses, or (`check`) the check page.
  virtual void openCalibration(bool check) = 0;

  // ---- Bluetooth (the Output card and the Pair screen) ----
  virtual void btConnect() = 0;     // Connect, Try again
  virtual void btDisconnect() = 0;  // Disconnect, Cancel: the audio moves to the speaker, paused
  virtual void btForget() = 0;      // after Forget's second tap
  // The Pair screen's scan: on (the screen opened, Search again), off
  // (the screen closed: back to what it was doing).
  virtual void btPairScan(bool on) = 0;
  // The scan stopped by itself (its 2 min, the screen off) while the Pair
  // screen stays up: nothing else starts until it closes.
  virtual void btPairScanPause() = 0;
  // The scan's list, copied; returns its version.
  virtual uint32_t btScan(BtScanList& out) = 0;
  // Pair with a device the scan listed (it replaces the remembered pair
  // once it is linked). False: nothing to pair, it is the device linked
  // now (it is made the output instead).
  virtual bool btPairWith(const BtDevice& d) = 0;

  // ---- the card, the library ----
  // "Try again" with no card: false if there still is none, or it still
  // doesn't mount (the next snapshot's cardKind says which); with one
  // the firmware restarts to use it.
  virtual bool retryCard() = 0;
  // "Try again" with no music: walks /music again (the queue follows).
  virtual void rescanLibrary() = 0;
  // What the player learned of track lengths (the Queue's summary).
  virtual const queueview::DurationBook& durations() = 0;
  virtual void about(AboutInfo& a) = 0;
  // Device info (About's): the board, the chips, memory, the battery, the
  // library, the uptime, the build. The battery is read from the power
  // chip each time (I2C: the page asks every 3 s while it's open).
  virtual void deviceInfo(deviceinfo::Facts& f) = 0;

  // ---- the screen (ScreenPower; saved) ----
  // "Screen off after" and "Brightness": ScreenPower's choice indices.
  virtual void setScreenTimeout(int choice) = 0;
  virtual void setBrightness(int choice) = 0;
  // Something needs the listener (the headphones lost, "Couldn't reach", a
  // track that failed): the screen lights if it was dim or off, and the
  // countdown starts again. `why` is logged.
  virtual void wakeScreen(const char* why) = 0;

  // ---- the sleep timer (SleepTimer) ----
  // A choice on the Sleep timer sheet or the fade's toast: SleepSheet's
  // 0-4 (15-90 min: restarts from now), kTrack, kAlbum, kQueue, kExtend
  // (+10 min), kTurnOff.
  virtual void sleepChoose(int pick) = 0;

  // ---- the idle power-off (IdlePolicy; saved) ----
  // "Turn off when idle": IdlePolicy's choice index.
  virtual void setIdleOff(int choice) = 0;
  // The warning's Keep on (any touch keeps it on as well: logged).
  virtual void idleKeepOn() = 0;

  // ---- CPU speed and Bluetooth power (PowerChoices; saved) ----
  // "CPU speed": saves `mhz` (160 or 240). When that isn't the clock set
  // at boot, the player restarts at it (the UI asked first): paused, the queue and
  // its place saved, the headphones let go and the speaker's amp switched
  // off, then the restart (at most 3 s later); after it nothing plays by
  // itself. True: that restart is under way. False: saved with no restart
  // needed, or refused and nothing saved (a pairing under way, which the
  // restart would drop; NVS failed; a restart under way already).
  virtual bool setCpuSpeed(uint16_t mhz) = 0;
  // "Bluetooth power": PowerChoices' choice, saved and applied at once (a
  // link that is up keeps its level until the next connection).
  virtual void setBtPower(int choice) = 0;

  // ---- the screen's pocket rule (ScreenPower) ----
  // The touch on the glass now landed on a screen woken from off that
  // nobody had looked at yet (maybe a pocket's second contact): the fade
  // toast's +10 min and Turn off don't act on it (they raise the level).
  virtual bool touchLandedUnattended() const = 0;

protected:
  ~UiHost() = default;
};

}  // namespace ui
