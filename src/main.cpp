// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

// mstream-mp3-player — firmware entry point (M5Stack Core2).
//
// Wires the portable core (PlaybackController over the queue and the
// library index) to the Core2 audio backend, storage, the UI (ui/Ui: the
// tab bar and its pages, the one owner of the display), the input layer
// (touch buttons and glass) and a serial console. The queue is restored
// from the card at boot; the first time it's the whole library followed by
// the built-in test tones. A computer can drive the Dance tab's dancer over
// the same USB serial port (app/UsbViz: the USB visualizer).

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <nvs_flash.h>

#include <cmath>
#include <vector>

#include "ButtonPolicy.h"
#include "ChipRevision.h"
#include "HeadsetKeys.h"
#include "IdlePolicy.h"
#include "InputEvent.h"
#include "OutputModel.h"
#include "PlayGate.h"
#include "PlaybackController.h"
#include "PowerChoices.h"
#include "QueueModel.h"
#include "QueueView.h"
#include "RateConverter.h"
#include "SleepTimer.h"
#include "ToneTrack.h"
#include "TrackCatalog.h"
#include "UiText.h"
#include "app/DanceMode.h"
#include "app/BoardGuard.h"
#include "app/BoardPower.h"
#include "app/Diagnostics.h"
#include "app/Haptics.h"
#include "app/IdlePower.h"
#include "app/Library.h"
#include "app/NvsSchema.h"
#include "app/PowerLab.h"
#include "app/PowerSettings.h"
#include "app/Psram.h"
#include "app/QueueStore.h"
#include "app/ScreenControl.h"
#include "app/Screenshot.h"
#include "app/SerialConsole.h"
#include "app/UsbViz.h"
#include "app/Version.h"
#include "audio/Core2AudioBackend.h"
#include "spike/Spike.h"
#include "storage/LocalStorage.h"
#include "ui/BootScreen.h"
#include "ui/CalibrationScreen.h"
#include "ui/Input.h"
#include "ui/Ui.h"
#include "ui/UiHost.h"

// Headphones to connect to; set in a gitignored local.ini (see platformio.ini).
#ifndef BT_SINK_NAME
#define BT_SINK_NAME ""
#endif

using Output = Core2AudioBackend::Output;

static LocalStorage storage;
static BootScreen bootScreen;
static Core2AudioBackend audio;
// The library (a LibraryIndex, the single store) and the play queue (track
// ids): both in PSRAM, only these small objects in internal RAM.
static Library library(storage);
static QueueModel queue(psramAlloc, psramFree);
static PlaybackController player(audio, queue, library.catalog());
static QueueStore queueStore(storage, queue, player, library.catalog());
static DanceMode danceMode(audio, player);
static Screenshot shot;
static Haptics haptics;
// The one input layer: the glass (corrected) and the button strip, as events.
static Input input(haptics);
// The screen policy (docs/ENERGY.md item 2; ScreenPower): lit, dim for the
// last 10 s, then off after the chosen time without input; a touch or the
// PWR key on a screen that isn't lit only wakes it (swallowed by the input
// layer), and so do the events that need the listener.
static ScreenControl screen(input);
// What the touch buttons do, the same on every screen.
static ButtonPolicy buttonPolicy;
// The touch check and calibration (Output > Touch calibration, the first
// boot's check, the rescue hold, console a): in PSRAM, made on first use.
static CalibrationScreen* calibration = nullptr;
static bool calibrationUp() { return calibration && calibration->active(); }
// The rescue (README: touch trouble): while the start-up screen shows (the
// UI not started, the glass's events going nowhere), a finger held anywhere
// on the glass for kRescueHoldMs opens the calibration, which needs no
// accuracy to reach. The UI's start waits while that finger is down. (A
// strip press isn't one: B's hold would switch the output at 500 ms.)
// The start-up screen stays at least kRescueWindowMs after its line about
// it shows (at the end of setup(), which may outlast kDiagnosticsScreenMs).
static constexpr uint32_t kRescueHoldMs = 2000;
static constexpr uint32_t kRescueWindowMs = 1500;
static bool rescueFinger = false;
static uint32_t rescueDownMs = 0;
static uint32_t bootHintMs = 0;
// The first boot's touch check was offered (or the rescue used) this boot.
static bool touchCheckOffered = false;
// UI spike tools (docs/UI-SPIKE.md): input lab, scroll lab, library index,
// font and thumbnail probes. Created in PSRAM on first use.
static Spike spike(audio, haptics, storage, library, input);
// Silent test mode (console z), until restart: the output stays on the
// speaker at volume 0 and headphones connecting don't take it over, so
// tests can run at night with the headphones connected.
static bool silent = false;
// The headphones dropped while they were the output (the tab bar's icon
// turns red) until they're back or the output moves.
static bool btLost = false;
// The UI (ui/Ui): the tab bar and its pages. In PSRAM, made in setup(); up
// once the boot screen has been shown for kDiagnosticsScreenMs.
static ui::Ui* userInterface = nullptr;
// What the listener asked of Bluetooth (OutputModel): the audio waits on
// its output until the headphones they asked for are linked.
static BtSession btSession;
// Play while Bluetooth is the output and the headphones aren't connected
// (PlayGate): the player waits (PlayState::Waiting), the headphones are
// connected at once, and the wait ends playing on them, or paused with a
// notice when they can't be reached.
static PlayGate playGate;
// The name of the headphones being paired from the Pair screen: once they
// are linked it becomes the sink name, so a later scan by name finds them.
static char pairName[32] = "";
// The sleep timer (docs/ENERGY.md section 3): RAM only, fed every loop
// pass (stepSleep()); what it says is carried out here: the fade's target,
// the pause, the screen off, and 5 min later the headphones let go.
static SleepTimer sleepTimer;
// The idle power-off (docs/ENERGY.md item 4; IdlePolicy): off after the
// chosen minutes stopped or paused on the battery with nobody around;
// stepIdle() feeds it and carries it out (the queue flushed, the
// headphones let go, then the power off).
static IdlePower idlePower;
// CPU speed and Bluetooth power (docs/ENERGY.md items 6 and 7; the Output
// tab's, saved): the speed set at boot (a change restarts:
// MainUiHost::setCpuSpeed()), the TX power handed to BtSink.
static PowerSettings powerSettings;
// Input this pass that the screen doesn't see: a headphone key that acted,
// the console, the idle warning's Keep on (the idle countdown starts again).
static bool idleInput = false;
// Track lengths learned as they play (the Queue's "49 min"): PSRAM.
static queueview::DurationBook durations(psramAlloc, psramFree);
// A card was found by "Try again": restart at this time (the UI's toast
// shows first). 0: none.
static uint32_t restartAtMs = 0;

static constexpr uint32_t kDiagnosticsScreenMs = 3000;
// Volume keys of headphones without absolute volume (AVRCP passthrough):
// about 1/16 of the range per press, like a phone.
static constexpr int kHeadphoneVolumeStep = 6;

static String kb(uint32_t bytes) { return String(bytes / 1024) + "K"; }

static std::vector<BootScreen::Row> diagnosticsRows() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  const diag::Heap h = diag::heap();
  std::vector<BootScreen::Row> rows;
  rows.push_back({"Board", diag::boardName()});
  rows.push_back({"Power chip", diag::pmicName()});
  rows.push_back({"IMU", diag::imuName()});
  rows.push_back({"Chip", String(ESP.getChipModel()) + " rev " + (chip.revision / 100) + "." +
                              (chip.revision % 100)});
  rows.push_back({"Flash", String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB"});
  rows.push_back({"PSRAM", kb(ESP.getPsramSize()) + " (" + kb(h.psramFree) + " free)"});
  rows.push_back({"Last reset", diag::resetReason()});
  rows.push_back({"Battery", String(M5.Power.getBatteryLevel()) + "%"});
  const LibraryIndex* index = library.index();
  const uint32_t tracks = index && index->ready() ? index->trackCount() : 0;
  rows.push_back({"Library", String(storage.name()) + ", " + tracks + " tracks"});
  rows.push_back({"RAM free", kb(h.internalFree) + " (min " + kb(h.internalMin) + ", block " +
                                  kb(h.internalLargest) + ")"});
  return rows;
}

// ---- actions shared by the touch buttons and the serial console ----

// Console o: as the B hold does (ButtonTransport::selectOutput(), below):
// to Bluetooth connects them when they aren't linked (it used to select
// Bluetooth and page nothing after a Disconnect: "Not connected" until
// Connect was tapped), and to the speaker pauses first.
static bool selectOutputFromConsole();
static void toggleOutput() {
  if (silent) {
    Serial.println("[test] silent mode: the output stays on the speaker");
    return;
  }
  (void)selectOutputFromConsole();
}

static void enterSilentMode() {
  silent = true;
  // Muted before the speaker takes the ring: the pump preempts loop(), so
  // muting after the switch could let a buffer play at the old volume.
  audio.setSpeakerVolume(0);
  if (audio.output() != Output::Speaker) audio.setOutput(Output::Speaker);
  Serial.println("[test] silent mode: speaker muted, bluetooth won't take over");
}

// A screen of its own (the touch calibration, a spike screen: the scroll
// lab, a probe) owns the display while it's up: the UI is suspended.
static bool screenTaken() { return calibrationUp() || spike.ownsScreen(); }

// A spike command is running: the UI stays suspended until it's done (a
// screen it closes on the way mustn't bring the UI back in between).
static bool uiHeld = false;

// The UI draws again once no other screen has the display.
static void uiResume() {
  if (userInterface && !uiHeld && !screenTaken()) userInterface->resume();
}

// Console d: the Dance tab, and back to where you were.
static void toggleDance() {
  if (screenTaken()) {
    Serial.println("[dance] not while another screen is up (close it first)");
    return;
  }
  if (userInterface) userInterface->toggleDance();
}

static bool headphonesSetVolume() {
  return audio.output() == Output::Bluetooth && audio.bluetooth().headphonesControlVolume();
}

// The active output's volume (the speaker and Bluetooth each keep their own).
// Bluetooth applies the step on its own task, so the value logged is where
// it should land (the screen shows the result at its next redraw).
static void stepVolume(int delta) {
  if (silent && audio.output() == Output::Speaker) {
    Serial.println("[test] silent mode: the speaker stays at volume 0");
    return;
  }
  const int expected = constrain(audio.volume() + delta, 0, 100);
  audio.stepVolume(delta);
  Serial.printf("[audio] volume %d%% (%s)\n", expected,
                audio.output() == Output::Speaker ? "speaker"
                : headphonesSetVolume()           ? "bluetooth, sent to the headphones"
                                                  : "bluetooth, applied by the Core2");
}

// Headphone volume keys change the Bluetooth volume, whichever output is active.
static void stepBluetoothVolume(int delta) {
  BtSink& bt = audio.bluetooth();
  bt.stepVolume(delta);
  Serial.printf("[bt] volume %d%%\n", constrain(bt.volume() + delta, 0, 100));
}

// Prev from the Core2 (the A click, Now Playing's, the console's p):
// PlaybackController's rule, past a track's first 3 s the same track from
// 0:00 (paused: still paused), said in the log.
static void prevTrack() {
  const bool restart = player.prevAction() == PlaybackController::Prev::Restart;
  player.prev();
  if (restart) {
    // (Said after: a restart that must wait for the headphones waits.)
    const PlayState s = player.state();
    Serial.printf("[player] prev: this track again from 0:00 (%s)\n",
                  s == PlayState::Playing   ? "playing"
                  : s == PlayState::Waiting ? "waiting for the headphones"
                                            : "nothing starts");
  }
}

// Playing, or waiting for the headphones (the wait counts as playing on
// them: a move to the speaker ends it paused, never playing out loud).
static bool pauseIfPlaying() {
  if (player.state() == PlayState::Waiting) {
    player.cancelWait();
    return true;
  }
  if (player.state() != PlayState::Playing) return false;
  player.togglePlayPause();
  return true;
}

// The player's hold (PlaybackController::Hold): a play waits while
// Bluetooth is the output and the headphones aren't connected (PlayGate
// connects them, and releases or ends the wait).
struct OutputHold : PlaybackController::Hold {
  bool holdPlay() const override {
    return !silent && audio.output() == Output::Bluetooth && !audio.bluetooth().connected();
  }
};
static OutputHold outputHold;

// What the touch buttons drive (ButtonPolicy decides which button does what).
struct ButtonTransport : ButtonPolicy::Transport {
  void prev() override { prevTrack(); }
  void next() override { player.next(); }
  // (Waiting: cancels the wait, paused.)
  void playPause() override { player.togglePlayPause(); }
  // Waiting for the headphones counts: a B hold then ends the wait paused.
  bool playing() const override {
    return player.state() == PlayState::Playing || player.state() == PlayState::Waiting;
  }
  void pause() override { (void)pauseIfPlaying(); }
  void stepVolume(int delta) override { ::stepVolume(delta); }
  int volume() const override { return audio.volume(); }
  // Bluetooth, or on its way to it (asked for, not linked yet): a B hold
  // then goes back to the speaker, which cancels the connection.
  bool onBluetooth() const override { return audio.output() == Output::Bluetooth || btSession.wanted(); }
  // Only music on the headphones is paused by a B hold: music still on the
  // speaker while they connect plays on (as the Output tab's Speaker row).
  bool audioOnBluetooth() const override { return audio.output() == Output::Bluetooth; }
  // Nothing queued: the clicks are inert (the "inert" buzz, not the tick).
  bool idle() const override { return queue.size() == 0; }
  // The screen woke from off and nobody has touched the glass since (a
  // pocket, maybe): B doesn't start the speaker (ScreenPower's pocket rule).
  // The headphones aren't out loud: B plays on them (or waits for them).
  bool startRefused() const override { return audio.output() != Output::Bluetooth && screen.unattended(); }
  bool switchOutput() override {
    if (silent) {
      Serial.println("[test] silent mode: the output stays on the speaker");
      return false;
    }
    return selectOutput(!onBluetooth());
  }
  // False: refused (no headphones paired, and nothing scans for any:
  // BtSink::nothingToFind()). The output stays; nothing is scanned.
  static bool selectOutput(bool bluetooth);
};
static ButtonTransport buttonTransport;

static bool selectOutputFromConsole() { return ButtonTransport::selectOutput(!buttonTransport.onBluetooth()); }

// The one way the output changes (the B hold, the Output tab):
//   - to Bluetooth: at once if the headphones are linked; otherwise they
//     are connected and the audio stays where it is until they are
//     (handleBluetooth() moves it on Connected);
//   - to the speaker: paused first (ButtonPolicy pauses before a B hold;
//     the Output tab's tap here), and a connection on its way is
//     cancelled.
bool ButtonTransport::selectOutput(bool bluetooth) {
  BtSink& bt = audio.bluetooth();
  if (bluetooth) {
    if (bt.connected()) {
      if (audio.output() != Output::Bluetooth) audio.setOutput(Output::Bluetooth);
      Serial.println("[output] bluetooth");
      return true;
    }
    if (bt.nothingToFind()) {
      // (The UI's note says where: uitext::kNoHeadphones.)
      Serial.println("[output] bluetooth: no headphones paired: pair them on Output > Pair new headphones (nothing is "
                     "scanned for); the output stays on the speaker");
      return false;
    }
    btSession.connect(millis());
    bt.connect();
    Serial.println("[output] bluetooth asked for: connecting; the audio stays on the speaker until it's up");
    return true;
  }
  const bool paused = audio.output() == Output::Bluetooth && pauseIfPlaying();
  if (btSession.wanted()) {
    btSession.cancel();
    bt.disconnect();
    Serial.println("[output] the connection on its way is cancelled");
  }
  if (audio.output() != Output::Speaker) audio.setOutput(Output::Speaker);
  btLost = false;
  Serial.printf("[output] the speaker%s\n", paused ? " (paused first)" : "");
  return true;
}

// Disconnect (and Forget): the audio goes to the speaker, paused, and the
// headphones are let go; nothing tries to connect until the listener asks.
static void letGoOfHeadphones(bool forget) {
  BtSink& bt = audio.bluetooth();
  // Off the headphones: paused first (only then: music already on the
  // speaker, waiting for a connection that is cancelled, plays on).
  const bool paused = audio.output() == Output::Bluetooth && pauseIfPlaying();
  if (audio.output() != Output::Speaker) audio.setOutput(Output::Speaker);
  btLost = false;
  if (bt.connected()) btSession.expectDrop(millis());  // no "lost" dialog for it
  btSession.cancel();
  // For good: not looked for by name either, so they can't come back by
  // themselves (at the next boot, or a B hold) until paired again.
  if (forget) bt.forgetDevice(0, /*forGood=*/true);
  bt.disconnect();
  Serial.printf("[output] %s the headphones%s\n", forget ? "forgot" : "disconnected", paused ? " (paused first)" : "");
}

// The sleep timer, 5 min after its pause (ENERGY.md section 3, step 5),
// and the idle power-off before the power goes (item 4): the headphones
// are let go (their battery; their keys go quiet) and the search rests.
// Not letGoOfHeadphones(): the output stays as it is (on Bluetooth a later
// play waits for them and pages them: PlayGate), nothing is refused (the
// Core2 stays connectable: they come back when switched on), and the drop
// is expected (no "lost" dialog). Unlinked: the search rests (no pages
// while nobody listens). `why` begins the log line.
static void releaseHeadphones(const char* why) {
  BtSink& bt = audio.bluetooth();
  const bool linked = bt.connected();
  if (linked) btSession.expectDrop(millis());  // no "lost" dialog for it
  bt.releaseHeadphones();
  Serial.printf("%s: %s; the output stays %s\n", why,
                linked ? "letting go of the headphones (resting after, still connectable)"
                       : "not linked: the search for them rests",
                audio.output() == Output::Bluetooth ? "bluetooth" : "the speaker");
}

// [Play on speaker] (Now Playing while play waits for the headphones, the
// "Couldn't reach" notice): the listener's explicit choice. The wait ends
// paused, the speaker becomes the output, and then it plays, at the
// speaker's own volume. From both places the headphones are still looked
// for quietly in the background: only the ask is withdrawn (not the radio
// let go, as the Speaker row does for a connection on its way), so they
// can come back by themselves once they are on.
static void playOnSpeaker() {
  btSession.withdraw(/*failed=*/false);
  const bool playing = PlayGate::playOnSpeaker(player, [] { return ButtonTransport::selectOutput(false); });
  Serial.printf("[output] play on the speaker%s\n", playing ? "" : ": nothing to play");
}

// uiF<k>: a state the UI is shown, for screenshots of the states a
// test can't safely cause (the radio and the card are left alone): c
// connecting, s searching, p pairing, r resting (the search stopped: "They'll
// reconnect when switched on"), l the headphones lost (the dialog too), n
// no card (on Now Playing), f a card that isn't FAT32 (the same), w play
// waiting for the headphones (Now Playing's panel); uiF0 (or uiF) the real
// state. Display only: a button
// on a faked card still does what it does.
static char uiFake = 0;

// What the UI reads and asks for (ui/UiHost.h).
struct MainUiHost : ui::UiHost {
  uint32_t batteryAtMs = 0;
  uint8_t battery = 0;
  bool charging = false;

  void snapshot(ui::AppState& s) override {
    s.play = player.state();
    s.failed = audio.failed();
    s.current = queue.current();
    s.trackId = queue.currentTrack();
    s.currentKey = queue.currentKey();
    s.queueSize = queue.size();
    s.upNext = queue.upNext();
    s.contentVersion = queue.contentVersion();
    s.positionVersion = queue.positionVersion();
    s.positionMs = s.current >= 0 ? audio.positionMs() : 0;
    s.durationMs = s.current >= 0 ? audio.durationMs() : 0;
    // A start point waiting (the resume point after a boot, or qs): Now
    // Playing shows that second, and the length as it was then, until the
    // play that starts there.
    uint32_t startMs = 0, startDurationMs = 0;
    if (s.current >= 0 && player.startPoint(&startMs, &startDurationMs)) {
      s.positionMs = startMs;
      s.durationMs = startDurationMs;
    }
    BtSink& bt = audio.bluetooth();
    s.onBluetooth = audio.output() == Output::Bluetooth;
    s.btConnected = bt.connected();
    s.btLost = btLost && s.onBluetooth && !s.btConnected;
    // Their name (read at each link; before the first, the name looked for;
    // none with nothing paired: BtSink::shownName()).
    snprintf(s.btName, sizeof(s.btName), "%s", bt.shownName());
    s.volume = audio.volume();
    s.speakerVolume = audio.speakerVolume();
    s.btVolume = bt.volume();
    s.headphonesSetVolume = headphonesSetVolume();
    s.silent = silent;
    s.btLink = bt.link();
    s.btSession = btSession;
    s.gate = playGate.state();
    s.gateFailures = playGate.failures();
    s.btDetail[0] = 0;
    if (s.btConnected) {
      const uint32_t delayMs = bt.delayReportUs() / 1000;
      const char* codec = bt.codec();
      if (codec[0] && delayMs) {
        snprintf(s.btDetail, sizeof(s.btDetail), "%s, %lu ms", codec, (unsigned long)delayMs);
      } else if (codec[0]) {
        snprintf(s.btDetail, sizeof(s.btDetail), "%s", codec);
      } else if (delayMs) {
        snprintf(s.btDetail, sizeof(s.btDetail), "%lu ms delay", (unsigned long)delayMs);
      }
    }
    s.card = storage.onCard();
    s.cardNotFat32 = !s.card && storage.cardNotFat32();
    const LibraryIndex* index = library.index();
    s.libraryTracks = index && index->ready() ? index->trackCount() : 0;
    // The battery is an I2C read of the power chip: every 10 s is plenty.
    const uint32_t now = millis();
    if (batteryAtMs == 0 || now - batteryAtMs >= 10000) {
      batteryAtMs = now ? now : 1;
      battery = static_cast<uint8_t>(constrain(M5.Power.getBatteryLevel(), 0, 100));
      charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
    }
    s.battery = battery;
    s.charging = charging;
    s.ringMs = audio.bufferedMsNow();
    s.underruns = audio.underrunsNow();
    s.ringMatters = audio.isPlaying() && audio.ringSteady();
    s.feedback = buttonPolicy.feedback();
    s.screenTimeout = static_cast<uint8_t>(screen.timeoutChoice());
    s.brightness = static_cast<uint8_t>(screen.brightnessChoice());
    s.sleepRunning = sleepTimer.running();
    s.sleepFading = sleepTimer.fading();
    s.sleepPick = static_cast<int8_t>(sleepPick());
    s.sleepCanExtend = sleepTimer.canExtend();
    sleepTimer.rowText(now, s.sleepRow, sizeof(s.sleepRow));
    sleepTimer.titleText(now, s.sleepTitle, sizeof(s.sleepTitle));
    sleepTimer.shortText(now, s.sleepShort, sizeof(s.sleepShort));
    s.idleOff = static_cast<uint8_t>(idlePower.choice());
    s.idleWarnS = static_cast<uint8_t>(idlePower.policy().warnSeconds(now));
    s.cpuMhz = powerSettings.cpuSaved();
    s.cpuRunMhz = PowerSettings::cpuBootMhz();
    s.btPower = static_cast<uint8_t>(powerSettings.btChoice());
    s.btPowerPending = powerSettings.btPending();
    fake(s);
  }
  static void fake(ui::AppState& s) {
    if (!uiFake) return;
    if (uiFake == 'n' || uiFake == 'f') {
      // As after a boot with no card (f: with one that isn't FAT32):
      // nothing indexed, nothing queued (Now Playing shows it; the Library
      // and Queue lists read the real index and queue, so they don't).
      s.card = false;
      s.cardNotFat32 = uiFake == 'f';
      s.libraryTracks = 0;
      s.current = -1;
      return;
    }
    s.btConnected = false;
    s.btDetail[0] = 0;
    s.btLink.remembered = true;
    s.btLink.phase = uiFake == 's' ? BtLink::Phase::Scanning
                     : uiFake == 'p' ? BtLink::Phase::Pairing
                     : uiFake == 'r' ? BtLink::Phase::Resting
                                     : BtLink::Phase::Paging;
    s.btLink.attempt = uiFake == 'r' ? 0 : 2;
    s.btLink.attempts = 3;
    if (uiFake == 'w') {
      s.onBluetooth = true;
      if (s.current >= 0) s.play = PlayState::Waiting;
      s.gate = PlayGate::State::Waiting;
      return;
    }
    if (uiFake == 'l') {
      s.onBluetooth = true;
      s.btLost = true;
    }
  }
  void playPause() override { player.togglePlayPause(); }
  void play() override {
    if (player.state() == PlayState::Paused || player.state() == PlayState::Stopped) player.togglePlayPause();
  }
  void playOnSpeaker() override { ::playOnSpeaker(); }
  void next() override { player.next(); }
  void prev() override { prevTrack(); }
  void stepVolume(int delta) override { ::stepVolume(delta); }
  void stepOutputVolume(bool bluetooth, int delta) override {
    if (bluetooth == (audio.output() == Output::Bluetooth)) {
      ::stepVolume(delta);  // the active one: as the buttons do
    } else if (bluetooth) {
      stepBluetoothVolume(delta);  // as the headphones' keys do
    } else if (silent) {
      Serial.println("[test] silent mode: the speaker stays at volume 0");
    } else {
      const int v = constrain(audio.speakerVolume() + delta, 0, 100);
      audio.setSpeakerVolume(static_cast<uint8_t>(v));
      Serial.printf("[audio] speaker volume %d%% (not the output now)\n", v);
    }
  }
  bool selectOutput(bool bluetooth) override {
    if (silent && bluetooth) {
      Serial.println("[test] silent mode: the output stays on the speaker");
      return false;
    }
    Serial.printf("[ui] output: %s\n", bluetooth ? "bluetooth" : "the speaker");
    const bool ok = ButtonTransport::selectOutput(bluetooth);
    // Refused: none paired (the card's Connect or Try again after the
    // fresh-unit test, say). Nothing is scanned: the note says where
    // pairing is, as for a B hold.
    if (!ok && userInterface) userInterface->warn(uitext::kNoHeadphones);
    return ok;
  }
  void openCalibration(bool check) override;
  void btConnect() override {
    if (silent) {
      Serial.println("[test] silent mode: bluetooth stays off");
      return;
    }
    if (audio.bluetooth().nothingToFind()) {
      Serial.println("[ui] bluetooth: connect: no headphones paired");
      if (userInterface) userInterface->warn(uitext::kNoHeadphones);
      return;
    }
    btSession.connect(millis());
    audio.bluetooth().connect();
    Serial.println("[ui] bluetooth: connect");
  }
  void btDisconnect() override { letGoOfHeadphones(false); }
  void btForget() override { letGoOfHeadphones(true); }
  void btPairScan(bool on) override {
    BtSink& bt = audio.bluetooth();
    if (on) {
      bt.startPairScan();
    } else {
      bt.stopPairScan();
    }
  }
  void btPairScanPause() override { audio.bluetooth().pausePairScan(); }
  uint32_t btScan(BtScanList& out) override { return audio.bluetooth().scanList(out); }
  bool btPairWith(const BtDevice& d) override {
    BtSink& bt = audio.bluetooth();
    if (bt.isLinkedTo(d.addr)) {
      // The headphones linked now (still discoverable): nothing to pair,
      // nothing to let go. They are the output, as a tap on the card does.
      Serial.printf("[ui] bluetooth: pair with \"%s\": linked already\n", d.name);
      if (!silent && audio.output() != Output::Bluetooth) audio.setOutput(Output::Bluetooth);
      return false;
    }
    // The link that is up goes away: paused on the speaker meanwhile; the
    // new headphones take the audio once linked.
    const bool paused = audio.output() == Output::Bluetooth && pauseIfPlaying();
    if (audio.output() != Output::Speaker) audio.setOutput(Output::Speaker);
    btLost = false;
    if (bt.connected()) btSession.expectDrop(millis());
    btSession.pairStarted(millis());
    snprintf(pairName, sizeof(pairName), "%s", d.name);
    bt.pairWith(d.addr);
    Serial.printf("[ui] bluetooth: pair with \"%s\" %02x:%02x:%02x:%02x:%02x:%02x%s\n", d.name, d.addr[0], d.addr[1],
                  d.addr[2], d.addr[3], d.addr[4], d.addr[5], paused ? " (paused first)" : "");
    return true;
  }
  bool retryCard() override {
    if (!storage.probeCard()) {
      Serial.printf("[storage] try again: %s\n", storage.cardNotFat32() ? "the card still isn't FAT32" : "still no card");
      return false;
    }
    Serial.println("[storage] try again: a card is in: restarting to use it");
    restartAtMs = millis() + 1200;  // the toast shows first
    return true;
  }
  void rescanLibrary() override;
  const queueview::DurationBook& durations() override { return ::durations; }
  void about(ui::AboutInfo& a) override {
    const uint64_t bytes = storage.totalBytes();
    if (!storage.available()) {
      snprintf(a.storage, sizeof(a.storage), "No storage");
    } else if (storage.onCard()) {
      snprintf(a.storage, sizeof(a.storage), "microSD card, %.1f GB", bytes / 1e9);
    } else {
      snprintf(a.storage, sizeof(a.storage), "Internal flash, %.1f MB (no card)", bytes / 1e6);
    }
    snprintf(a.version, sizeof(a.version), "%s", version::player());
    snprintf(a.built, sizeof(a.built), "%s", version::commitDate());
    snprintf(a.elf, sizeof(a.elf), "%s", version::elfSha());
    BtSink& bt = audio.bluetooth();
    const BtLink l = bt.link();
    snprintf(a.bluetooth, sizeof(a.bluetooth), "%s%s", l.remembered ? (bt.deviceName()[0] ? bt.deviceName() : "paired") : "none paired",
             bt.connected() ? ", connected" : "");
    powerchoice::aboutText(PowerSettings::cpuBootMhz(), powerSettings.btChoice(), a.power, sizeof(a.power));
    const diag::Heap h = diag::heap();
    a.ramFree = h.internalFree;
    a.ramMin = h.internalMin;
    a.psramFree = h.psramFree;
  }
  void sleepChoose(int pick) override;
  // The Sleep timer sheet's outlined pill (SleepSheet's 0-7), or -1.
  static int sleepPick() {
    if (!sleepTimer.running()) return -1;
    switch (sleepTimer.choice()) {
      case SleepTimer::Choice::Timed: return sleepTimer.timedIndex();
      case SleepTimer::Choice::EndOfTrack: return ui::SleepSheet::kTrack;
      case SleepTimer::Choice::EndOfAlbum: return ui::SleepSheet::kAlbum;
      case SleepTimer::Choice::EndOfQueue: return ui::SleepSheet::kQueue;
      default: return -1;
    }
  }
  void setScreenTimeout(int choice) override { screen.setTimeout(choice); }
  void setIdleOff(int choice) override { idlePower.setChoice(choice); }
  bool setCpuSpeed(uint16_t mhz) override;
  void setBtPower(int choice) override { powerSettings.setBt(choice, audio.bluetooth()); }
  bool touchLandedUnattended() const override { return screen.landedUnattended(); }
  void idleKeepOn() override {
    idleInput = true;
    Serial.println("[power] idle: Keep on");
  }
  void setBrightness(int choice) override { screen.setBrightness(choice); }
  void wakeScreen(const char* why) override { screen.wake(why); }
};
static MainUiHost uiHost;

// The library walked and built again (g0, the UI's "Try again" with no
// music): the queue follows its tracks by path, the UI's ids start over,
// the lengths learned are for the old ids.
static bool rebuildLibrary() {
  const bool ok = queueStore.remap([](void*) { return library.rebuild(); }, nullptr);
  const LibraryIndex* index = library.index();
  durations.reset(index && index->ready() ? index->trackCount() : 0);
  if (userInterface) userInterface->libraryChanged();  // every index id changed
  return ok;
}

void MainUiHost::rescanLibrary() {
  Serial.println("[ui] try again: walking /music");
  rebuildLibrary();
}

static const char* stateName() {
  if (audio.failed()) return "failed";
  switch (player.state()) {
    case PlayState::Playing: return "playing";
    case PlayState::Paused: return "paused";
    case PlayState::Waiting: return "waiting";
    default: return "stopped";
  }
}

// Power measurements and their A/B knobs (the console's P, app/PowerLab):
// nothing runs until a P command.
static PowerLab powerLab(audio, player, danceMode, storage, screen, powerSettings,
                         {[] { return stateName(); }, [] { return silent; }});

static void printStats() {
  const auto& s = audio.stats();
  const diag::Heap h = diag::heap();
  Serial.printf(
      "[stats] track=%d/%lu %s pos=%.1fs out=%s%s buf=%lums underruns=%lu bt=%lufps load=%.1f%% "
      "stack_free=%lu ram=%luK min=%luK psram=%luK bat=%d%%\n",
      player.currentIndex() + 1, (unsigned long)queue.size(), stateName(), audio.positionMs() / 1000.0f,
      audio.output() == Output::Bluetooth ? "bt" : "speaker",
      audio.output() == Output::Bluetooth ? (audio.bluetooth().connected() ? "(connected)" : "(searching)")
      : silent                            ? "(silent test mode)"
                                          : "",
      (unsigned long)s.bufferedMs, (unsigned long)s.underruns, (unsigned long)s.btFramesPerSec,
      s.decodeLoad * 100.0f, (unsigned long)s.decodeStackFree, (unsigned long)(h.internalFree / 1024),
      (unsigned long)(h.internalMin / 1024), (unsigned long)(h.psramFree / 1024),
      (int)M5.Power.getBatteryLevel());
  if (danceMode.active()) danceMode.printStats(millis());  // every 5 s while dancing

  BtSink& bt = audio.bluetooth();
  // Unlinked and not the output: only while the radio looks for them (a
  // burst, the back-off, a scan) or did in the last minute.
  const char* search = bt.reconnectPhase();
  const bool looking = strcmp(search, "idle") != 0 && strcmp(search, "resting") != 0;
  if (!bt.connected() && audio.output() != Output::Bluetooth && !looking && bt.radioBusyPercent() <= 0) return;
  // vol/control: the Bluetooth volume and who applies it (headphones = AVRCP
  // absolute volume, asking = waiting for them to accept it, software = the
  // Core2). headphones: their last reported volume. gain: the Core2's gain
  // stage (the headroom with absolute volume). headroom: its fixed
  // attenuation (-2.0dB unless set with h<n>). gap: longest wait between two
  // data callbacks since the last line (~10-30 ms is healthy). search: how
  // the Core2 looks for them while unlinked (burst, backoff, resting, scan,
  // idle); radio: the share of the last minute spent paging or scanning.
  const BtSink::Stats b = bt.stats();
  char gain[12] = "mute";
  if (b.gainQ15 > 0) snprintf(gain, sizeof(gain), "%.1fdB", 20.0f * log10f(b.gainQ15 / 32768.0f));
  char headset[8] = "?";
  if (b.headsetVolume >= 0) snprintf(headset, sizeof(headset), "%d", b.headsetVolume);
  Serial.printf("[stats] bt vol=%u%% control=%s headphones=%s/127 gain=%s headroom=%.1fdB stream=%s gap=%lums "
                "events_dropped=%lu btapp_stack_free=%lu search=%s radio=%d%%/min\n",
                (unsigned)b.volume, b.volumeControl, headset, gain, 20.0f * log10f(b.headroomQ15 / 32768.0f),
                b.stream, (unsigned long)b.maxGapMs, (unsigned long)b.eventsDropped,
                (unsigned long)b.appTaskStackFree, b.reconnect, b.radioBusyPercent);
}

static void listTracks() {
  char path[TrackCatalog::kMaxPath];
  for (uint32_t i = 0; i < queue.size(); ++i) {
    library.catalog().path(queue.trackAt(i), path, sizeof(path));
    Serial.printf("  %u%s %s\n", (unsigned)i, (int)i == queue.current() ? "*" : " ",
                  path[0] ? path : "(not in the library)");
  }
  queueStore.printStatus();
}

// The whole library from the first track: played now, or (start false) only
// queued, stopped. The built-in test tones and click tracks stay out (the
// console's qb queues them): a new player's first queue is its music only,
// and with no card it is empty (Now Playing shows the no-card state).
static bool queueEverything(bool start) {
  const LibraryIndex* index = library.index();
  const LibraryIndex::Span lib = index && index->ready() ? index->allTracks() : LibraryIndex::Span{};
  const uint32_t n = lib.count;
  if (n == 0) {
    if (start) return false;
    const bool ok = queue.assign(nullptr, 0, 0);
    player.queueReplaced(false);
    return ok;
  }
  auto* ids = static_cast<uint32_t*>(psramAlloc(n * sizeof(uint32_t)));
  if (!ids) return false;
  memcpy(ids, lib.ids, n * sizeof(uint32_t));
  bool ok;
  if (start) {
    ok = player.playNow(ids, n, 0);
  } else {
    ok = queue.assign(ids, n, 0);
    player.queueReplaced(false);
  }
  psramFree(ids);
  return ok;
}

// q...: the queue from the console, until the Queue and Library screens
// exist. Positions are the ones `l` lists (0-based); albums are numbered by
// `ql` (A-Z).
static void queueCommand(const char* a) {
  const LibraryIndex* index = library.index();
  const bool haveLibrary = index && index->ready();
  const char c = a[0];
  const bool number = c && a[1] >= '0' && a[1] <= '9';
  const long n = number ? atol(a + 1) : -1;
  switch (c) {
    case 0:
      break;
    case 'a':
      queueEverything(true);
      break;
    case 'b': {
      const LibraryIndex::Span b = TrackCatalog::builtins();
      player.playNow(b.ids, b.count, 0);
      break;
    }
    case 'l':
      if (!haveLibrary) break;
      for (uint32_t i = 0; i < index->albumCount(); ++i) {
        const uint32_t album = index->albumsAZ()[i];
        Serial.printf("  %lu  %s - %s (%lu)\n", (unsigned long)i, index->artistName(index->album(album).artist),
                      index->albumName(album), (unsigned long)index->album(album).trackCount);
      }
      break;
    case 'p':
    case 'n':
    case '+': {
      if (!haveLibrary || n < 0 || static_cast<uint32_t>(n) >= index->albumCount()) {
        Serial.println("[queue] no such album (ql lists them)");
        break;
      }
      const LibraryIndex::Span t = index->tracksOfAlbum(index->albumsAZ()[n]);
      const bool ok = c == 'p' ? player.playNow(t.ids, t.count, 0)
                      : c == 'n' ? player.playNext(t.ids, t.count)
                                 : player.addToQueue(t.ids, t.count);
      Serial.printf("[queue] %s %s: %lu tracks%s\n", c == 'p' ? "playing" : c == 'n' ? "plays next:" : "added",
                    index->albumName(index->albumsAZ()[n]), (unsigned long)t.count, ok ? "" : " (NO MEMORY)");
      break;
    }
    case 'r': {
      if (n < 0) {
        Serial.println("[queue] qr<n>: remove entry n (l lists them)");
        break;
      }
      const uint32_t pos = static_cast<uint32_t>(n);
      const QueueModel::Removed r = player.remove(&pos, 1);
      Serial.printf("[queue] removed %lu%s\n", (unsigned long)r.count, r.current ? " (it was the current one)" : "");
      break;
    }
    case 'c':
      player.clearUpNext();
      break;
    case 'x':
      player.clearQueue();
      break;
    case 'u':
      Serial.printf("[queue] undo: %s\n", player.undo() ? "done" : "nothing to undo");
      break;
    case 's': {
      // A test of the resume point without a restart: the current entry
      // starts n s in at its next play, as after a boot with that second
      // saved (playing: now; paused: the held track is let go), with the
      // length as known (the held track's, else the catalog's). qs0 (or
      // qs) clears it. The backend logs where it really landed.
      if (queue.current() < 0) {
        Serial.println("[queue] qs<sec>: nothing is current");
        break;
      }
      const uint32_t ms = n > 0 ? static_cast<uint32_t>(n) * 1000u : 0;
      player.setStartPoint(ms, 0);
      Serial.printf("[queue] start point: %s (%s)\n", ms ? (String(n) + " s into the current entry").c_str() : "none",
                    stateName());
      break;
    }
    default:
      Serial.println("[queue] q status, qa play all, qb built-ins, ql albums, qp<n>/qn<n>/q+<n> album n: play / "
                     "play next / add, qr<pos> remove, qc clear up next, qx clear, qu undo, qs<sec> start the "
                     "current entry that far in (as a resume point; qs0 none)");
      return;
  }
  queueStore.printStatus();
}

// A calibration asked for while the screen is off (the console's a, ac,
// ab): the screen is woken and it opens once the panel is awake (loop()).
// Nothing may be drawn into a sleeping panel (ScreenControl: the pixels
// land garbled), and Ui::suspend() lets the screen that takes over draw at
// once: measured, the A hint band drawn in sleep-in came out shifted and
// in the wrong colours.
static bool calibrationPending = false;
static CalibrationScreen::Start pendingHow = CalibrationScreen::Start::Crosses;
static int pendingTargets = CalibrationScreen::kMaxTargets;

static void openCalibration(CalibrationScreen::Start how, int targets = CalibrationScreen::kMaxTargets) {
  if (!userInterface) {
    Serial.println("[input] the calibration draws with the UI's sprites: not without the UI (no PSRAM)");
    return;
  }
  if (screen.off() || screen.panelAsleep()) {
    calibrationPending = true;
    pendingHow = how;
    pendingTargets = targets;
    screen.wake("the touch calibration");
    Serial.println("[cal] the screen is off: it opens once the screen is awake");
    return;
  }
  calibrationPending = false;
  if (!calibration) {
    calibration = psramNew<CalibrationScreen>(input);
    if (!calibration) {
      Serial.println("[input] no PSRAM for the calibration screen");
      return;
    }
    calibration->onClosed(uiResume);  // the UI draws again
  }
  // It takes the screen: whatever had it lets go first.
  if (userInterface) userInterface->suspend();
  spike.closeAll();
  if (danceMode.active()) danceMode.setActive(false);
  input.cancelTouch(millis());
  calibration->open(how, targets);
}

void MainUiHost::openCalibration(bool check) {
  ::openCalibration(check ? CalibrationScreen::Start::Check : CalibrationScreen::Start::Crosses);
}

static void closeCalibration() {
  calibrationPending = false;
  if (calibration) calibration->close();
}

// a...: the input layer: touch calibration, haptics.
static void touchCommand(const char* a) {
  const char c = a[0];
  const bool on = a[1] == '1';
  const bool flag = a[1] == '0' || a[1] == '1';
  if (c == 0) {
    if (calibrationUp()) {
      closeCalibration();
    } else {
      openCalibration(CalibrationScreen::Start::Crosses);
    }
    return;
  }
  if (c >= '0' && c <= '9') {
    const int n = atoi(a);
    if (n < CalibrationScreen::kMinTargets || n > CalibrationScreen::kMaxTargets) {
      Serial.println("[input] a<n>: 5-9 crosshairs");
      return;
    }
    openCalibration(CalibrationScreen::Start::Crosses, n);
    return;
  }
  if (c == 'q') {
    closeCalibration();
    return;
  }
  if (c == 'c') {
    openCalibration(CalibrationScreen::Start::Check);
    return;
  }
  if (c == 'b') {
    if (a[1] == '0') {
      // The next boot asks again (if nothing is calibrated then).
      input.setTouchCheckAnswered(false);
      Serial.println("[input] the first-boot touch check: not answered (it shows at the next boot with no "
                     "calibration saved)");
    } else {
      openCalibration(CalibrationScreen::Start::FirstBoot);
      return;
    }
  } else if (c == 'd') {
    input.resetCalibration();
    Serial.println("[input] touch: calibration removed: no correction (saved)");
  } else if (c == 'h' && flag) {
    input.setHapticsOn(on);
  } else if (c == 'r' && flag) {
    input.setRailTicksOn(on);
  } else if (c != 's') {
    Serial.println("[input] a calibrate (9 crosses; a5-a9: fewer), ac test taps, ab the first-boot touch "
                   "check (ab0: ask again at the next boot), as status, ad remove the calibration (no correction), "
                   "ah0/ah1 haptics off/on, ar0/ar1 rail ticks off/on, aq close");
    return;
  }
  input.printStatus();
}

// A spike command may bring its screen up: the UI lets go of the display
// (and the LCD's hardware scroll, which the scroll lab needs) first, and
// takes it back if nothing took it.
// The touch calibration closes first: two screens drawing (and the scroll
// lab moving the LCD's scroll under the crosshairs) would pair taps with the
// wrong targets.
static void spikeCommand(void (Spike::*fn)(const char*), const char* a) {
  uiHeld = true;
  if (userInterface) userInterface->suspend();
  if (calibrationUp()) {
    Serial.println("[cal] closed: a spike command takes the screen");
    closeCalibration();
  }
  (spike.*fn)(a);
  // g<n> rebuilt the synthetic library, or g0 dropped it: the Library tab,
  // if it browses it, follows before it draws again.
  if (userInterface && userInterface->browsingSynthetic()) userInterface->browse(spike.synthetic());
  uiHeld = false;
  uiResume();
}

// uil<n>: the Library tab browses a synthetic library of n tracks (the
// spike's g<n>: 6 artists and 15 albums per 100 tracks), to see the lists,
// the A-Z rail and the jump grid at the scale of thousands (the card has 6
// artists). Look only: its ids aren't the player's. uil0 (or uil): the
// card's library again.
static void browseCommand(const char* a) {
  if (!userInterface) return;
  const long n = atol(a);
  if (n <= 0) {
    userInterface->browse(nullptr);
    return;
  }
  char num[16];
  snprintf(num, sizeof(num), "%ld", n);
  spikeCommand(&Spike::index, num);  // builds it (and reports it) while the UI is held
  if (!spike.synthetic()) {
    Serial.println("[ui] no synthetic library was made");
    return;
  }
  userInterface->browse(spike.synthetic());
}

// uit/uih/uis/uid/uip: a scripted finger (Input::simulate), for tests
// without a hand on the device: t<x>,<y> tap; h<x>,<y> long press
// (800 ms); s<x0>,<y0>,<x1>,<y1>,<ms> swipe and lift (a fling when fast);
// d<...> the same but resting 150 ms before the lift (a drag, no fling);
// p<x>,<ms> a press on the button strip (y 260) for ms (a click, or from
// 500 ms a hold). Screen pixels, as the corrected touch reports them; y
// from 240 is the button strip, which takes the same path as a finger's
// (StripButtons): uit160,260 clicks B, uis160,200,160,264,80 is a swipe
// that ends there (no button), uis160,265,160,100,120 a swipe up from it
// (it flings the list).
static bool simulatedTouch(const char* a) {
  const char c = a[0];
  if (c != 't' && c != 'h' && c != 's' && c != 'd' && c != 'p') return false;
  int v[5] = {0, 0, 0, 0, 0};
  const int n = sscanf(a + 1, "%d,%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
  if ((c == 't' || c == 'h') && n >= 2) {
    input.simulate(v[0], v[1], v[0], v[1], c == 't' ? 60 : 800, 0, 0);
  } else if ((c == 's' || c == 'd') && n == 5 && v[4] > 0) {
    input.simulate(v[0], v[1], v[2], v[3], 30, static_cast<uint32_t>(v[4]), c == 'd' ? 150 : 0);
  } else if (c == 'p' && n == 2 && v[1] > 0) {
    input.simulate(v[0], 260, v[0], 260, static_cast<uint32_t>(v[1]), 0, 0);
  } else {
    Serial.println("[input] uit<x>,<y> tap, uih<x>,<y> long press, uis<x0>,<y0>,<x1>,<y1>,<ms> swipe, uid... drag, "
                   "uip<x>,<ms> a press on the button strip (y >= 240 is the strip in all of them)");
    return true;
  }
  Serial.printf("[input] scripted finger: %s\n", a);
  return true;
}

// The sleep timer's console command (T; below, with the rest of the timer).
static void sleepCommand(const char* a);
// The idle power-off's (I; below, with stepIdle()).
static void idleCommand(const char* a);

// B...: Bluetooth tests for developers that leave the listener's pairing
// alone (BtSink's developer tests):
//   B    what the Core2 may look for now
//   Bs   auto-pair by signal for the next scan (RAM only, off at boot,
//        logged), and that scan starts now (none may be remembered); Bs0 off
//   Bf   the next boot is a fresh unit (a flag in NVS that boot clears: as
//        if nothing were remembered and there were no BT_SINK_NAME; the
//        stored address and the bond are left alone): restarts now
//   Bn   the same for this session (RAM only; not while linked or
//        pairing); Bn0 ends it (so does any restart)
// R...: the rate converter (docs/RESAMPLER.md section 6; every track goes
// to the ring at 44.1 kHz):
//   R      the current track's conversion (its exact ratio, source frames
//          taken against ring frames made, clamped samples), and the help
//   Rt     the test tracks (a 1 kHz tone and silence made at other rates:
//          TrackCatalog::rateTests()); Rt<n> plays one, Rt<tone:...> any
//          built-in path ("tone:silence@37800" shows a refusal). It plays on
//          its own: the player is stopped first, so when it ends or fails
//          nothing follows it (the queue's music never starts by itself).
//          On Bluetooth only silence (headphones may be on someone's ears);
//          a tone only in silent mode (z: the speaker at volume 0).
//   Rf</music/...>  a file on its own, the same way (silent mode z only:
//          it may be music); for the converter's measurements on real tracks.
//          Rf48000</music/...> converts it as if it were 48 kHz: a 44.1 kHz
//          MP3 then costs what a 48 kHz one would (the pitch is wrong)
//   Rx     stops what Rt or Rf started (the player is stopped already)
//   Rb     the converter's bench: playback stopped, ~10 s of audio per rate
// Rt, Rb (and b<n>) stop the player keeping the listener's place: the
// paused or playing track's second waits as a start point (and stays the
// saved resume point), so a play afterwards picks up there.

// The player stopped for a test that borrows the backend; logs the place kept.
static void stopForTest(const char* what) {
  const bool kept = player.stopKeepingPlace();
  uint32_t ms = 0, dur = 0;
  if (kept && player.startPoint(&ms, &dur)) {
    Serial.printf("[%s] playback stopped; the current entry picks up %.1f s in at its next play\n", what, ms / 1000.0);
  } else {
    Serial.printf("[%s] playback stopped\n", what);
  }
}

static void rateCommand(const char* a) {
  const char c = a[0];
  if (c == 'b') {
    stopForTest("rate");
    Serial.println("[rate] bench: 10 s of audio per rate (about half a minute in all)");
    audio.rateBench();
    return;
  }
  if (c == 'f') {
    // A file on its own, for the converter's measurements on real tracks
    // (a 44.1 kHz MP3's load against the old path, a 48 kHz file's). Only
    // in silent mode: it may be music.
    if (!silent) {
      Serial.println("[rate] Rf: only in silent mode (z: the speaker at volume 0)");
      return;
    }
    // Rf<hz></music/...>: converted as if it were at that rate (a 44.1 kHz
    // MP3 as a 48 kHz one: the decoder's and the converter's work of a
    // 48 kHz track, where the card has none; the pitch is wrong).
    const char* path = a + 1;
    uint32_t asHz = 0;
    while (isDigit(*path)) asHz = asHz * 10 + static_cast<uint32_t>(*path++ - '0');
    if (*path != '/') {
      Serial.println("[rate] Rf</music/...>: plays that file on its own (silent mode only); Rf48000</music/...> "
                     "converts it as if it were 48 kHz (a load test); Rx stops it");
      return;
    }
    stopForTest("rate");
    if (asHz != 0) {
      audio.playAsRate(path, asHz);
    } else {
      audio.play(path, 0, 0);
    }
    Serial.printf("[rate] playing %s on its own%s (the speaker, silent test mode; the player is stopped: nothing "
                  "follows it)\n",
                  path, asHz ? ", converted as if at the rate asked" : "");
    return;
  }
  if (c == 'x') {
    // Whatever Rt or Rf started (the player is stopped already: nothing to keep).
    if (player.state() != PlayState::Stopped) {
      Serial.println("[rate] Rx: the player isn't stopped: its own controls stop it");
      return;
    }
    audio.stop();
    Serial.println("[rate] stopped");
    return;
  }
  if (c == 't') {
    const LibraryIndex::Span tests = TrackCatalog::rateTests();
    const TrackCatalog& catalog = library.catalog();
    char path[TrackCatalog::kMaxPath] = "";
    if (!a[1]) {
      for (uint32_t i = 0; i < tests.count; ++i) {
        catalog.path(tests[i], path, sizeof(path));
        Serial.printf("  Rt%lu  %s\n", (unsigned long)i, path);
      }
      Serial.println("[rate] Rt<n> plays one on its own (the player stopped: nothing follows it); Rt<tone:...@<rate>> "
                     "any built-in path; silence only on Bluetooth, a tone only in silent mode (z)");
      return;
    }
    if (isDigit(a[1])) {
      const long n = atol(a + 1);
      if (n < 0 || static_cast<uint32_t>(n) >= tests.count) {
        Serial.println("[rate] Rt: no such test track (Rt lists them)");
        return;
      }
      catalog.path(tests[n], path, sizeof(path));
    } else {
      snprintf(path, sizeof(path), "%s", a + 1);
    }
    ToneTrack t;
    if (!ToneTrack::parse(path, &t)) {
      Serial.printf("[rate] Rt: \"%s\" isn't a built-in track (tone:1000@48000, tone:silence@96000, ...)\n", path);
      return;
    }
    const bool quiet = t.kind == ToneTrack::Kind::Silence;
    if (!quiet && audio.output() == Output::Bluetooth) {
      Serial.println("[rate] Rt: only silence on Bluetooth (headphones may be on someone's ears): tone:silence@<rate>");
      return;
    }
    if (!quiet && !silent) {
      Serial.println("[rate] Rt: a tone only in silent mode (z: the speaker at volume 0); silence plays anywhere");
      return;
    }
    // On its own: the player stopped, so PlaybackController::update() has
    // nothing to advance when it ends or fails (Pz's way would queue it
    // ahead of the listener's music). The listener's place is kept.
    stopForTest("rate");
    audio.play(path, 0, 0);
    Serial.printf("[rate] playing %s on its own (%s; the player is stopped: nothing follows it)\n", path,
                  audio.output() == Output::Bluetooth ? "bluetooth"
                  : silent                            ? "the speaker, silent test mode"
                                                      : "the speaker");
    return;
  }
  const Core2AudioBackend::RateStatus r = audio.rateStatus();
  if (r.rate == 0) {
    Serial.println("[rate] no track at a known rate");
  } else if (!r.route[0]) {
    Serial.printf("[rate] %lu Hz: refused (%s)\n", (unsigned long)r.rate, audio.note().c_str());
  } else {
    const uint64_t want = (static_cast<uint64_t>(r.taken) * r.num + r.den - 1) / r.den;
    Serial.printf("[rate] %lu Hz -> %d Hz (%s): %lu/%lu ring frames per source frame; %lu source frames taken, %lu ring "
                  "frames made, ceil(taken x %lu/%lu) = %lu (%ld still in the filter: 0 once the track has ended); "
                  "%lu samples clamped; position %.3f s\n",
                  (unsigned long)r.rate, audio.sampleRate(), r.route, (unsigned long)r.num, (unsigned long)r.den,
                  (unsigned long)r.taken, (unsigned long)r.made, (unsigned long)r.num, (unsigned long)r.den,
                  (unsigned long)want, (long)(static_cast<int64_t>(want) - r.made), (unsigned long)r.clamped,
                  audio.positionMs() / 1000.0);
  }
  const Core2AudioBackend::TableStatus t = Core2AudioBackend::tableStatus();
  Serial.printf("[rate] filter tables: %s; %lu copies found no room since boot; internal free %u B, largest block "
                "%u B\n",
                t.inRam ? "the internal-RAM copy" : "flash (no copy now)", (unsigned long)t.noRoom,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  Serial.printf("[rate] CPU set at boot: %u MHz; 88.2/96 kHz %s; R status, Rt the test tracks (Rt<n> plays one "
                "on its own), Rb the converter's bench (stops playback, keeps your place)\n",
                (unsigned)PowerSettings::cpuBootMhz(),
                RateConverter::kHiResOn ? "need 240 MHz" : "off in this build (MSTREAM_HIRES_RATES=1 turns them on)");
}

static void bluetoothTestCommand(const char* a) {
  BtSink& bt = audio.bluetooth();
  const BtLink l = bt.link();
  switch (a[0]) {
    case 's':
      if (a[1] == '0') {
        bt.setBySignal(false);
        return;
      }
      if (l.remembered || bt.connected()) {
        Serial.println("[bt] Bs: headphones are remembered: they are paged, never scanned for (Bn first: a "
                       "fresh-unit session, the pairing left alone)");
        return;
      }
      if (silent) {
        Serial.println("[test] silent mode: bluetooth stays off");
        return;
      }
      bt.setBySignal(true);
      // The scan, as a connect asks for it (the audio moves once linked).
      (void)ButtonTransport::selectOutput(true);
      return;
    case 'f':
      BtSink::armFreshBoot();
      Serial.println("[bt] Bf: the next boot is a fresh unit (that boot only; the stored pairing is left alone): "
                     "restarting");
      restartAtMs = millis() + 500;
      return;
    case 'n':
      bt.setFreshSession(a[1] != '0');
      return;
    default:
      break;
  }
  const char* fresh = bt.fresh() == BtSink::Fresh::Boot      ? "this boot (Bf)"
                      : bt.fresh() == BtSink::Fresh::Session ? "this session (Bn)"
                                                             : "off";
  Serial.printf("[bt] headphones %s; scan by name: %s; auto-pair by signal (Bs): %s; fresh-unit test: %s%s\n",
                l.remembered ? "remembered" : "none remembered",
                bt.scansByName() ? bt.sinkName() : "no (no BT_SINK_NAME in this build, or the fresh-unit test)",
                bt.bySignal() ? "armed" : "off", fresh,
                bt.nothingToFind() ? "; nothing is scanned for: pairing is Output > Pair new headphones" : "");
  Serial.println("[bt] B status, Bs / Bs0 auto-pair by signal for the next scan (starts it) / off, Bf the next boot "
                 "as a fresh unit (restarts), Bn / Bn0 this session as one / back");
}

// ---- the USB visualizer (docs/USB-VISUALIZER.md; app/UsbViz) ----

// A pairing under way (below, with the idle power-off).
static bool pairingUnderWay();

// Why a computer's @hello can't start host mode now.
static HostLink::Busy vizBusy() {
  if (!userInterface || !userInterface->started()) return HostLink::Busy::Ui;  // the start-up screen
  if (screenTaken() || uiHeld || userInterface->suspended()) return HostLink::Busy::Screen;
  if (pairingUnderWay()) return HostLink::Busy::Pairing;
  if (!danceMode.ready()) return HostLink::Busy::Dance;  // no PSRAM for the dancer: for good
  return HostLink::Busy::None;
}

// Host mode starts: the player paused (Playing pauses, a wait for the
// headphones ends paused; nothing resumes by itself afterwards), a track
// the console's Rt/Rf plays on its own stopped, the screen woken, the
// Dance tab up. True: something was playing.
static bool vizEnter() {
  const PlayState was = player.state();
  player.pauseByComputer();  // (marked: headphone Play won't resume it, HeadsetKeys)
  bool test = false;
  if (player.state() == PlayState::Stopped && audio.isPlaying()) {
    audio.stop();  // (the player is stopped already: nothing to keep)
    test = true;
  }
  screen.wake("the computer's visualizer");
  if (userInterface) userInterface->showDance();
  return was == PlayState::Playing || was == PlayState::Waiting || test;
}

// The Dance tab isn't up any more: another screen took the display, or the
// console moved the UI (d, ui<n>). Not while the screen is dark: the dancer
// stops then, and comes back when it wakes (host mode wakes it, keeps it lit).
static bool vizDanceGone() { return !danceMode.active() && !(userInterface && userInterface->dark()); }

static UsbViz usbViz(danceMode, {vizBusy, vizEnter, vizDanceGone});

static SerialConsole console({
    [] { player.next(); },
    prevTrack,
    [] { player.togglePlayPause(); },
    toggleOutput,
    stepVolume,
    [] {
      printStats();
      if (!danceMode.active()) danceMode.printStats(millis());  // on request also when not dancing
    },
    listTracks,
    [](int i) { player.play(static_cast<size_t>(i)); },
    [](int i) {
      if (i < 0 || static_cast<uint32_t>(i) >= queue.size()) return;
      char path[TrackCatalog::kMaxPath];
      library.catalog().path(queue.trackAt(i), path, sizeof(path));
      stopForTest("bench");
      audio.bench(path);
    },
    [] {
      audio.bluetooth().forgetDevice(/*waitMs=*/3000);  // before the restart
      Serial.printf("[bt] forgot the remembered device; restarting (%s)\n",
                    audio.bluetooth().scansByName() ? "it scans by name" : "no name to scan by: pair on the Output tab");
      Serial.flush();
      ESP.restart();
    },
    [](const char* name) {
      BtSink& bt = audio.bluetooth();
      bt.setSinkName(name);
      Serial.printf("[bt] headphones: %s (%s)%s\n",
                    name[0] ? ("name contains \"" + String(name) + "\"").c_str() : "no name: nothing scanned for",
                    bt.fresh() == BtSink::Fresh::No ? "saved" : "the fresh-unit test: not saved",
                    bt.scansByName() || !name[0]
                        ? ""
                        : ": not scanned for (no BT_SINK_NAME in this build, or the fresh-unit test); pair on the "
                          "Output tab");
    },
    [](int db) {
      audio.bluetooth().setHeadroomDb(static_cast<uint8_t>(db));
      Serial.printf("[bt] headroom -%d dB (until restart; the stats line shows it once applied)\n", db);
    },
    enterSilentMode,
    toggleDance,
    [] { danceMode.cycleSkin(); },
    [](bool full) {
      DanceView& v = danceMode.view();
      M5Canvas* figure = danceMode.active() ? &v.sprite() : nullptr;  // what the box should show
      if (full) {
        shot.request(0, 0, M5.Display.width(), M5.Display.height(), figure, DanceView::kBoxX, DanceView::kBoxY);
      } else {
        shot.request(DanceView::kBoxX, DanceView::kBoxY, DanceView::kBoxW, DanceView::kBoxH, figure,
                     DanceView::kBoxX, DanceView::kBoxY);
      }
    },
    [] { danceMode.toggleVerbose(); },
    [](float bpm) { danceMode.setPrior(bpm); },
    [](int ms) {
      danceMode.setOffsetMs(ms);
      Serial.printf("[dance] latency offset %+d ms (not saved)\n", ms);
    },
    [](int n) { danceMode.freeze(n); },
    [](const char* a) {
      // uk0/uk1/uk2: the scripted finger on a skewed panel (Input::setSimSkew).
      if (a[0] == 'k') {
        input.setSimSkew(static_cast<uint8_t>(a[1] == '1' ? 1 : a[1] == '2' ? 2 : 0));
        Serial.printf("[input] scripted finger: %s\n",
                      input.simSkew() == 2 ? "skewed like the input lab's panel (x reads up to ~40 px right), with "
                                             "up to 4 px of jitter a touch"
                      : input.simSkew()    ? "skewed like the input lab's panel (x reads up to ~40 px right)"
                                           : "where it's told (already corrected)");
        return;
      }
      // ui (u + "i"): the UI's navigation state; ui0-ui4 a tab, uib back.
      if (a[0] == 'i') {
        if (a[1] == 'l') {
          browseCommand(a + 2);
          return;
        }
        if (simulatedTouch(a + 1)) return;
        if (a[1] == 'F') {
          uiFake = a[2] == '0' ? 0 : a[2];
          Serial.printf("[ui] shown state: %s\n", uiFake ? "faked (uiF0: the real one)" : "the real one");
          if (uiFake == 'l' && userInterface) userInterface->headphonesLost();
          return;
        }
        if (a[1] == 'V') {
          if (userInterface) userInterface->volumeKeys();  // the HUD, the volume unchanged
          return;
        }
        if (userInterface) userInterface->command(a + 1);
        return;
      }
      spikeCommand(&Spike::inputLab, a);
    },
    [](const char* a) { spikeCommand(&Spike::scrollLab, a); },
    [](const char* a) { spikeCommand(&Spike::index, a); },
    [](const char* a) { spikeCommand(&Spike::fontProbe, a); },
    [](const char* a) { spikeCommand(&Spike::thumbProbe, a); },
    queueCommand,
    touchCommand,
    [](const char* a) { powerLab.command(a); },
    sleepCommand,
    idleCommand,
    bluetoothTestCommand,
    diag::printPartitionTable,
    rateCommand,
    [](char* line, HostLine::Byte kind) { usbViz.onLine(line, kind); },
});

// Touch buttons: the same on every screen (ButtonPolicy). Each click and
// hold is logged; repeats show as the volume lines.
static void handleButton(const InputEvent& e) {
  const bool acted = buttonPolicy.handle(e, buttonTransport);
  // The tick for a click or hold that did something; a double buzz for a
  // click with nothing to play (spec §4, §7: inert).
  input.buttonFeedback(e, acted);
  const char b = static_cast<char>('A' + e.button);
  if (!acted) {
    if (e.type != InputEvent::Type::Click) return;
    if (e.button == ButtonPolicy::kButtonB && !buttonTransport.idle() && buttonTransport.startRefused()) {
      // Not out loud from a pocket: the screen woke from off and nobody
      // has touched the glass since. A tap on the glass, then B plays.
      Serial.println("[button] B click: not played: the screen woke from off and nothing touched the glass since "
                     "(a pocket?); the speaker would play out loud");
      if (userInterface) userInterface->warn(uitext::kTouchFirst);
      return;
    }
    Serial.printf("[button] %c click: nothing to play\n", b);
    return;
  }
  if (e.type == InputEvent::Type::Repeat) return;
  const ButtonPolicy::Feedback& f = buttonPolicy.feedback();
  if (e.type == InputEvent::Type::Hold && f.kind == ButtonPolicy::Hud::Output) {
    Serial.printf("[button] B hold: output to %s%s%s\n", f.toBluetooth ? "bluetooth" : "the speaker",
                  f.paused ? " (paused first: B plays)" : "", f.refused ? ": refused" : "");
  } else if (e.type == InputEvent::Type::Click) {
    Serial.printf("[button] %c click: %s\n", b, b == 'A' ? "previous" : b == 'B' ? "play/pause" : "next");
  }
}

// Every input event, to whoever owns the screen: the calibration screen, a
// spike screen, or the UI. The input lab reads the panel and the buttons
// itself: no events while it's open.
// A finger landed this pass (the glass, or a strip button's first event):
// the sleep timer's fade shows its toast.
static bool touchedThisPass = false;

// While a computer drives the dancer (the USB visualizer), the listener's
// first touch outside the dancer's box, or any button (the PWR key too),
// ends it, and does nothing else: the rest of that touch, or of that
// button's hold, is dropped. A tap on the dancer still switches it.
static bool vizSwallowTouch = false;  // until the next touch lands
static int vizSwallowButton = -1;     // until this button's HoldEnd

// True: the event is taken (it ended host mode, or belongs to what did).
static bool vizInput(const InputEvent& e) {
  using T = InputEvent::Type;
  if (e.isButton()) {
    if (vizSwallowButton == e.button) {
      if (e.type == T::HoldEnd) vizSwallowButton = -1;
      return true;
    }
    if (!usbViz.active()) return false;
    Serial.printf("[button] %c %s: ends the computer's visualizer\n", static_cast<char>('A' + e.button),
                  InputEvent::name(e.type));
    usbViz.userEnded(HostLink::Why::Button);
    input.tapTick();
    if (e.type == T::Hold || e.type == T::Repeat) vizSwallowButton = e.button;
    return true;
  }
  const bool lands = e.type == T::Down || (e.type == T::DragStart && e.fromStrip);
  if (lands) vizSwallowTouch = false;  // a new touch
  if (vizSwallowTouch) return true;
  if (!usbViz.active() || !lands) return false;
  if (e.type == T::Down && DanceView::inBox(e.x, e.y)) return false;  // the dancer: switches it
  Serial.printf("[touch] %s %d,%d (raw %d,%d): ends the computer's visualizer\n", InputEvent::name(e.type), e.x,
                e.y, e.rawX, e.rawY);
  usbViz.userEnded(HostLink::Why::Touch);
  input.tapTick();
  vizSwallowTouch = true;
  return true;
}

static void handleInput(uint32_t now) {
  // The screen: the PWR key, USB, and whether a touch now would only wake
  // it (dim or off: the input layer swallows that touch through its lift).
  screen.beginPass(now);
  // (The PWR key only wakes the screen or counts as input there: ending
  // the visualizer is all it does here.)
  if (usbViz.active() && M5.BtnPWR.wasClicked()) {
    Serial.println("[button] PWR click: ends the computer's visualizer");
    usbViz.userEnded(HostLink::Why::Button);
  }
  input.setSuspended(spike.ownsInput());
  input.update(now);
  screen.afterInput(now);  // the wake it saw (logged), or input: the countdown again
  touchedThisPass = false;
  for (InputEvent e; input.poll(e);) {
    if (e.type == InputEvent::Type::Down || e.isButton()) touchedThisPass = true;
    if (vizInput(e)) continue;
    // The one exception to "the buttons do the same everywhere": while the
    // touch check or calibration is up, A's click is its way out (Cancel,
    // Not now, Discard, Done), which works however far off the glass reads.
    // A's hold, and all of B and C, stay ButtonPolicy's.
    if (e.isButton() && e.type == InputEvent::Type::Click && e.button == ButtonPolicy::kButtonA && calibrationUp()) {
      Serial.println("[button] A click: the calibration's way out");
      calibration->leave();
      continue;
    }
    if (e.isButton()) {
      handleButton(e);
      continue;
    }
    // The rescue hold: a glass finger while the start-up screen shows.
    if (!(userInterface && userInterface->started()) && !calibrationUp()) {
      using T = InputEvent::Type;
      if (e.type == T::Down) {
        rescueFinger = true;
        rescueDownMs = e.ms;
      } else if (e.type == T::Tap || e.type == T::Release || e.type == T::DragEnd || e.type == T::Cancel) {
        rescueFinger = false;
      }
    } else {
      rescueFinger = false;
    }
    if (calibrationUp()) {
      calibration->onEvent(e);
      continue;
    }
    if (spike.onGlass(e)) continue;
    // A line per touch (its landing and how it ended): what a finger did is
    // in the log next to what it caused.
    using T = InputEvent::Type;
    if (e.type == T::Down || e.type == T::Tap || e.type == T::LongPress || e.type == T::Fling) {
      Serial.printf("[touch] %s %d,%d (raw %d,%d)%s%s\n", InputEvent::name(e.type), e.x, e.y, e.rawX, e.rawY,
                    e.fromStrip ? " from the strip" : "", input.scriptedNote());
    }
    if (userInterface) userInterface->onEvent(e);
  }
}

// The link-up volume cap lands within this long of a connection: not a HUD.
static constexpr uint32_t kLinkUpQuietMs = 2000;

// Link changes and headphone buttons, queued by BtSink on the Bluetooth tasks.
static void handleBluetooth() {
  static uint32_t connectedAtMs = 0;
  BtSink& bt = audio.bluetooth();
  for (BtSink::Event e = bt.takeEvent(); e != BtSink::Event::None; e = bt.takeEvent()) {
    switch (e) {
      case BtSink::Event::Connected: {
        Serial.printf("[bt] connected%s%s\n", bt.deviceName()[0] ? " to " : "", bt.deviceName());
        connectedAtMs = millis();
        btLost = false;
        // Whether the listener asked for this link (and paired it): the
        // session answers the same whether its phase or this event came first.
        const BtSession::Answer answer = btSession.onConnected();
        if (answer.paired && pairName[0]) {
          // Paired from the Pair screen: a later scan by name finds them.
          bt.setSinkName(pairName);
          Serial.printf("[bt] headphones: \"%s\" from now on (saved)\n", pairName);
          pairName[0] = 0;
        }
        if (silent) {
          Serial.println("[test] silent mode: staying on the speaker");
        } else {
          audio.setOutput(Output::Bluetooth);
          // Asked for: the audio moved, seconds after the tap; say where. (A
          // play waiting for them: PlayGate's release says it, once; a wait
          // cancelled in this pass, before PlayGate withdrew its ask: paused,
          // nothing to say.)
          if (answer.asked && userInterface && !playGate.waiting() && player.state() != PlayState::Waiting) {
            userInterface->headphonesConnected();
          }
        }
        diag::logHeap("bt-link");
        break;
      }
      case BtSink::Event::Disconnected: {
        Serial.println("[bt] disconnected");
        // BtSession decides: let go on purpose (Disconnect, Forget, a new
        // pairing: the audio is on the speaker already, paused; the sleep
        // timer's or the idle power-off's release: the output stays
        // Bluetooth), or lost.
        const bool expected = btSession.dropExpected();
        const BtSession::Drop d =
            btSession.onDisconnected(audio.output() == Output::Bluetooth, player.state() == PlayState::Playing);
        if (expected) {
          if (d.pause) {
            // A play between a release and its drop: no link to carry it.
            (void)pauseIfPlaying();
            Serial.println("[bt] let go while a play had just started: paused (play pages them)");
          }
          break;
        }
        // Like a phone: don't carry on through the speaker, pause (and say why).
        if (d.lost) {
          btLost = true;
          if (d.pause && pauseIfPlaying() && userInterface) userInterface->headphonesLost();
        }
        break;
      }
      case BtSink::Event::Suspended:
        // The headphones stopped the stream themselves: show it as paused;
        // play (here or on them) starts it again.
        if (audio.output() == Output::Bluetooth) (void)pauseIfPlaying();
        break;
      case BtSink::Event::VolumeChanged:
        // The headphones' own keys with absolute volume (or the link-up cap,
        // just after connecting: no HUD for that). The Output tab's % follows
        // at its next redraw either way.
        Serial.printf("[bt] volume now %u%%\n", bt.volume());
        if (userInterface && bt.connected() && millis() - connectedAtMs >= kLinkUpQuietMs) {
          userInterface->volumeKeys();  // the same HUD as the A/C holds
        }
        break;
      // Transport keys: HeadsetKeys decides (headphone input never starts
      // music that wasn't playing); this adds the output and the stream.
      case BtSink::Event::Play:
        // The computer's visualizer ends first (a key on the headphones is
        // someone there). What played before it stays paused: the computer
        // paused it, not them, and in-ear detection sends Play as a bud goes
        // back in (pausedByComputer(); the Core2's play button resumes it).
        if (usbViz.active()) usbViz.userEnded(HostLink::Why::HeadsetKey);
        if (HeadsetKeys::decide(player.state(), HeadsetKeys::Key::Play, player.pausedNotByListener()) !=
            HeadsetKeys::Action::Resume) {
          // (Paused by the sleep timer: in-ear detection sends Play when a
          // sleeper turns over. The Core2's play button resumes it.)
          Serial.printf("[bt] headphones: play (ignored: %s%s)\n", stateName(),
                        player.pausedByTimer()      ? ", paused by the sleep timer"
                        : player.pausedByComputer() ? ", paused for the computer's visualizer"
                                                    : "");
          break;
        }
        Serial.println("[bt] headphones: play");
        idleInput = true;  // (it resumes: HeadsetKeys::isInput())
        if (bt.connected() && audio.output() != Output::Bluetooth && !silent) audio.setOutput(Output::Bluetooth);
        HeadsetKeys::apply(player, HeadsetKeys::Key::Play);
        break;
      case BtSink::Event::Pause: {
        if (audio.output() != Output::Bluetooth) {
          Serial.println("[bt] headphones: pause (ignored: the speaker's playback)");
          break;
        }
        // Only a pause that acted is someone's input for the idle power-off
        // (a bud taken out sends PAUSE while paused too).
        const bool notTheirs = player.pausedNotByListener();
        const HeadsetKeys::Action a = HeadsetKeys::apply(player, HeadsetKeys::Key::Pause);
        Serial.printf("[bt] headphones: pause%s\n", a == HeadsetKeys::Action::Ignore ? " (nothing plays)" : "");
        if (HeadsetKeys::isInput(a, notTheirs)) idleInput = true;
        // They pick their next key from the stream: suspend it now, so the
        // next press is PLAY. Also when we were paused already (paused on the
        // Core2, the stream still in its 3 s tail): this press did nothing,
        // the next one plays.
        bt.suspendPromptly();
        break;
      }
      case BtSink::Event::Next:
      case BtSink::Event::Prev: {
        const bool next = e == BtSink::Event::Next;
        const bool notTheirs = player.pausedNotByListener();
        // (Past a track's first 3 s their PREV restarts it, as every prev does.)
        const bool restart = !next && player.prevAction() == PlaybackController::Prev::Restart;
        const HeadsetKeys::Action a =
            HeadsetKeys::apply(player, next ? HeadsetKeys::Key::Next : HeadsetKeys::Key::Prev);
        // (A cue after the sleep timer's or the computer's pause isn't
        // input: a bud adjusted in bed sends these too.)
        if (HeadsetKeys::isInput(a, notTheirs)) idleInput = true;
        Serial.printf("[bt] headphones: %s (track %d, %s)\n",
                      next ? "next" : restart ? "previous: this track from 0:00" : "previous", player.currentIndex(),
                      a == HeadsetKeys::Action::Skip ? "playing" : "selected, not started");
        break;
      }
      case BtSink::Event::VolumeUp:
        idleInput = true;
        stepBluetoothVolume(+kHeadphoneVolumeStep);
        if (userInterface) userInterface->volumeKeys();  // the same HUD as the A/C holds
        break;
      case BtSink::Event::VolumeDown:
        idleInput = true;
        stepBluetoothVolume(-kHeadphoneVolumeStep);
        if (userInterface) userInterface->volumeKeys();
        break;
      case BtSink::Event::None:
        break;
    }
  }
}

// Play while Bluetooth is the output and the headphones aren't connected:
// PlayGate's decisions (the player and the session are done in step()), and
// what is left to do here: the radio and what the UI says.
static void stepPlayGate(uint32_t now) {
  BtSink& bt = audio.bluetooth();
  PlayGate::In in;
  in.play = player.state();
  in.onBluetooth = audio.output() == Output::Bluetooth;
  in.linked = bt.connected();
  in.link = bt.link();
  in.sessionFailed = btSession.failed();
  in.nothingToFind = bt.nothingToFind();
  in.nowMs = now;
  const PlayGate::Do d = playGate.step(in, player, btSession);
  if (d == PlayGate::Do::None) return;
  const char* name = bt.shownName();
  if (!name[0]) name = "the headphones";
  switch (d) {
    case PlayGate::Do::Connect:
      // The paging burst now: try 1 of 3 (a background page on its way
      // counts as that try: BtSink doesn't page on top of it).
      bt.connect();
      // fall through
    case PlayGate::Do::Track:
      // Asked for now: connecting, not lost (the tab bar and the card agree).
      btLost = false;
      Serial.printf("[play] waiting for %s: %s\n", name,
                    d == PlayGate::Do::Connect ? "connecting now" : "the Pair screen has the radio");
      break;
    case PlayGate::Do::Release:
      Serial.printf("[play] %s connected after %lu ms: playing\n", name, (unsigned long)(now - playGate.sinceMs()));
      if (userInterface) userInterface->headphonesConnected();
      break;
    case PlayGate::Do::GiveUp:
      // The session's ask is withdrawn as failed: the card and the tab turn
      // red with the notice; the radio carries on quietly (no disconnect).
      Serial.printf("[play] %s not reached in %lu ms (link %s): paused\n", name,
                    (unsigned long)(now - playGate.sinceMs()), btPhaseName(in.link.phase));
      break;
    case PlayGate::Do::Cancel:
      Serial.println("[play] the output isn't bluetooth any more: the wait ends, paused");
      break;
    case PlayGate::Do::NotPaired:
      // Nothing to wait for: paused, the output left as it is, no scan.
      Serial.println("[play] no headphones paired: not waiting (paused); pair them on Output > Pair new headphones");
      if (userInterface) userInterface->warn(uitext::kNoHeadphones);
      break;
    case PlayGate::Do::Ended:
      // Cancelled (or the speaker, or stopped): a link that comes later
      // answers nothing; the radio carries on as it was.
      Serial.printf("[play] the wait for %s ended (%s)\n", name, stateName());
      break;
    case PlayGate::Do::None:
      break;
  }
}

// ---- the sleep timer (docs/ENERGY.md section 3) ----

static const char* sleepEndName(SleepTimer::Choice c) {
  return c == SleepTimer::Choice::EndOfTrack   ? "the end of this track"
         : c == SleepTimer::Choice::EndOfAlbum ? "the end of this album"
                                               : "the end of the queue";
}

// Whether the backend's position and length are the current entry's yet
// (stepSleep() feeds it every pass).
static EntryStart sleepEntry;

// What is left of the playing track (0: not known).
static uint32_t trackLeftMs() {
  if (queue.current() < 0 || !sleepEntry.started()) return 0;
  const uint32_t d = audio.durationMs(), p = audio.positionMs();
  return d > p ? d - p : 0;
}

static void printSleep() {
  const uint32_t now = millis();
  char row[16];
  sleepTimer.rowText(now, row, sizeof(row));
  const uint16_t target = audio.fadeTargetQ15(), level = audio.fadeLevelQ15();
  char fade[40] = "none";
  if (target < SleepTimer::kUnity || level < SleepTimer::kUnity) {
    snprintf(fade, sizeof(fade), "level %.1f dB, target %.1f dB", level ? 20.0f * log10f(level / 32768.0f) : -99.0f,
             target ? 20.0f * log10f(target / 32768.0f) : -99.0f);
  }
  Serial.printf("[sleep] %s (%s, %s); fade: %s; player %s%s%s\n", row, SleepTimer::phaseName(sleepTimer.phase()),
                SleepTimer::choiceName(sleepTimer.choice()), fade, stateName(),
                player.pausedByTimer() ? ", paused by the timer (headphone play ignored)" : "",
                sleepTimer.releasePending() ? "; the headphones are let go 5 min after the pause" : "");
}

// A choice from the Sleep timer sheet, the fade's toast or the console:
// SleepSheet's 0-4 (15-90 min), kTrack, kAlbum, kQueue, kExtend, kTurnOff.
static void chooseSleep(int pick) {
  const uint32_t now = millis();
  char text[48];
  if (pick >= 0 && pick < SleepTimer::kTimedChoices) {
    sleepTimer.setTimed(SleepTimer::kMinutes[pick] * 60000u, now);
    snprintf(text, sizeof(text), "Sleep timer: %lu min", (unsigned long)SleepTimer::kMinutes[pick]);
  } else if (pick >= ui::SleepSheet::kTrack && pick <= ui::SleepSheet::kQueue) {
    const SleepTimer::Choice c = pick == ui::SleepSheet::kTrack   ? SleepTimer::Choice::EndOfTrack
                                 : pick == ui::SleepSheet::kAlbum ? SleepTimer::Choice::EndOfAlbum
                                                                  : SleepTimer::Choice::EndOfQueue;
    sleepTimer.setEnd(c);
    snprintf(text, sizeof(text), "Sleep timer: %s", c == SleepTimer::Choice::EndOfTrack   ? "end of track"
                                                    : c == SleepTimer::Choice::EndOfAlbum ? "end of album"
                                                                                          : "end of queue");
    Serial.printf("[sleep] pauses at %s\n", sleepEndName(c));
  } else if (pick == ui::SleepSheet::kExtend) {
    if (!sleepTimer.extend(now, trackLeftMs())) {
      // (Nothing runs; or End of album / queue before its last track, or
      // a track of unknown length: what is left isn't known, and +10 must
      // never shorten it. The sheet shows +10 min dim then.)
      Serial.printf("[sleep] +10 min: %s\n", sleepTimer.running() ? "refused (what is left isn't known)"
                                                                   : "no timer runs");
      return;
    }
    char left[24];
    sleepTimer.titleText(now, left, sizeof(left));
    snprintf(text, sizeof(text), "Sleep timer: %s", left);
  } else if (pick == ui::SleepSheet::kTurnOff) {
    sleepTimer.cancel();
    snprintf(text, sizeof(text), "Sleep timer off");
  } else {
    return;
  }
  Serial.printf("[sleep] %s\n", text);
  if (userInterface) userInterface->toast(text, false);
  printSleep();
}

void MainUiHost::sleepChoose(int pick) { chooseSleep(pick); }

// T...: the sleep timer from the console. T status, T<min> minutes (from
// now), Ts<sec> seconds (tests), Tt / Ta / Tq the end of the track, album,
// queue, T+ +10 min, T0 off.
static void sleepCommand(const char* a) {
  const uint32_t now = millis();
  if (!a[0]) {
    printSleep();
    return;
  }
  if (a[0] == 's' && a[1] >= '0' && a[1] <= '9') {
    const long sec = atol(a + 1);
    if (sec <= 0) {
      Serial.println("[sleep] Ts<sec>: a timer of that many seconds");
      return;
    }
    sleepTimer.setTimed(static_cast<uint32_t>(sec) * 1000u, now);
    Serial.printf("[sleep] %ld s (a test length)\n", sec);
    printSleep();
    return;
  }
  if (a[0] >= '0' && a[0] <= '9') {
    const long min = atol(a);
    if (min <= 0) {
      chooseSleep(ui::SleepSheet::kTurnOff);
      return;
    }
    sleepTimer.setTimed(static_cast<uint32_t>(min) * 60000u, now);
    Serial.printf("[sleep] %ld min\n", min);
    printSleep();
    return;
  }
  switch (a[0]) {
    case 't': chooseSleep(ui::SleepSheet::kTrack); return;
    case 'a': chooseSleep(ui::SleepSheet::kAlbum); return;
    case 'q': chooseSleep(ui::SleepSheet::kQueue); return;
    case '+': chooseSleep(ui::SleepSheet::kExtend); return;
    default:
      Serial.println("[sleep] T status, T<min> minutes, Ts<sec> seconds (tests), Tt/Ta/Tq end of track/album/queue, "
                     "T+ +10 min, T0 off");
      return;
  }
}

// Every loop pass, before the player's (its "pause after this track" must
// be set for a track that ends in this pass). The order at expiry is the
// timer's: the pause, then (confirmed, silent) the factor back to 1.0 and
// the screen off, then 5 min later the headphones let go.
static void stepSleep(uint32_t now) {
  SleepTimer::In in;
  in.nowMs = now;
  in.play = player.state();
  in.boundaryStops = player.timerStops();
  const int cur = queue.current();
  // The position and length are this entry's only once it has started
  // (EntryStart: after a skip the backend reports the last track's for a
  // moment). Unknown (0) until then, as the lengths the Queue learns (in
  // loop()).
  const bool started = sleepEntry.update(queue.currentKey(), audio.startTiming().seq, audio.positionMs());
  if (cur >= 0) {
    in.positionMs = audio.positionMs();
    in.durationMs = started ? audio.durationMs() : 0;
    const bool last = static_cast<uint32_t>(cur) + 1 >= queue.size();
    in.lastOfQueue = last;
    // The queue's end is an album's end too (with repeat, what comes next
    // may be the same album again: it still ends here).
    if (sleepTimer.choice() == SleepTimer::Choice::EndOfAlbum) {
      in.lastOfAlbum = last || SleepTimer::albumEndsBetween(library.index(), queue.currentTrack(),
                                                       queue.trackAt(static_cast<uint32_t>(cur) + 1));
    }
  }
  const SleepTimer::Phase before = sleepTimer.phase();
  const SleepTimer::Out o = sleepTimer.update(in);
  audio.setFade(o.fadeQ15);  // never sent to the headphones: our gain only
  if (player.pauseAfterTrack() != o.pauseAfterTrack) {
    player.setPauseAfterTrack(o.pauseAfterTrack);
    Serial.printf("[sleep] %s\n", o.pauseAfterTrack ? "this track is the last: pausing at its end"
                                                    : "not pausing at this track's end");
  }
  // (Once when it goes off, and again when its fade ends: said apart.)
  if (o.expired) {
    Serial.printf("[sleep] %s (%s)\n", before == SleepTimer::Phase::Fading ? "the fade ended" : "the timer went off",
                  stateName());
  }
  if (o.fadeStarted) {
    Serial.printf("[sleep] fading out (%s; our gain only, nothing sent to the headphones)\n",
                  sleepTimer.phase() == SleepTimer::Phase::Fading && before == SleepTimer::Phase::Armed
                      ? "the track's last 10 s"
                      : "30 s, then pause");
    if (userInterface) userInterface->sleepFading();
  }
  if (o.pauseNow) {
    const PlayState was = player.state();
    player.pauseByTimer();
    Serial.printf("[sleep] %s: paused by the timer (headphone play won't resume it; the Core2's does)\n",
                  was == PlayState::Waiting ? "the wait for the headphones ended" : "pause");
  }
  if (o.restore) {
    audio.restoreFade();
    Serial.println("[sleep] the pause is confirmed (silent): the fade back to 0 dB");
  }
  if (o.screenOff) {
    screen.sleepTimerOff();
    Serial.println("[sleep] the screen off; the headphones are let go in 5 min unless something plays");
  }
  if (o.release) releaseHeadphones("[sleep] 5 min paused");
  if (before != sleepTimer.phase() && sleepTimer.phase() == SleepTimer::Phase::Off &&
      (before == SleepTimer::Phase::Ending || before == SleepTimer::Phase::Ended) && !o.release) {
    Serial.println("[sleep] playing again: nothing more to do");
  }
}

// ---- the idle power-off (docs/ENERGY.md item 4) ----

// A console test only (Iu1, until restart): the policy is told "on
// battery" while USB is in, so the countdown, the warning and the release
// can be run on the bench. The last-moment read in stepIdle() is the real
// register's, so on USB it still stays on ("USB power at the last moment").
static bool idleFakeBattery = false;

// I...: I status, I<min> a test length in minutes, Is<sec> in seconds
// (until restart, in place of the setting), I0 the setting's again. Tests:
// Iu1/Iu0 pretend on battery (above), Ib<sec> leave the power-off note
// for the next boot's toast (as if it had turned off after that long).
static void idleCommand(const char* a) {
  const uint32_t now = millis();
  IdlePolicy& p = idlePower.policy();
  if (a[0] == 'u' && (a[1] == '0' || a[1] == '1')) {
    idleFakeBattery = a[1] == '1';
    Serial.printf("[power] idle: %s\n", idleFakeBattery ? "TEST: told on battery while USB is in (until restart; "
                                                          "the last-moment USB read still keeps it on)"
                                                        : "the real USB state again");
  } else if (a[0] == 'b' && a[1] >= '0' && a[1] <= '9') {
    const long sec = atol(a + 1);
    idlePower.noteOff(sec > 0 ? static_cast<uint32_t>(sec) * 1000u : p.lengthMs());
    Serial.println("[power] idle: TEST: the power-off note is left for the next boot's toast");
    return;
  } else if (a[0] == 's' && a[1] >= '0' && a[1] <= '9') {
    const long sec = atol(a + 1);
    p.setTestMs(sec > 0 ? static_cast<uint32_t>(sec) * 1000u : 0, now);
    Serial.printf("[power] idle: %s\n", sec > 0 ? "a test length (until restart)" : "the setting's length again");
  } else if (a[0] >= '0' && a[0] <= '9') {
    const long min = atol(a);
    p.setTestMs(min > 0 ? static_cast<uint32_t>(min) * 60000u : 0, now);
    Serial.printf("[power] idle: %s\n", min > 0 ? "a test length (until restart)" : "the setting's length again");
  } else if (a[0]) {
    Serial.println("[power] I status, I<min> a test length in minutes, Is<sec> in seconds (until restart), I0 the "
                   "setting's again (Output tab: Turn off when idle); tests: Iu1/Iu0 pretend on battery, Ib<sec> "
                   "the boot toast's note");
    return;
  }
  idlePower.printStatus(now);
}

// A pairing under way (the Pair screen's scan, or one picked there): the
// idle power-off waits for it, and the CPU speed's restart isn't offered.
static bool pairingUnderWay() {
  const BtLink link = audio.bluetooth().link();
  return link.phase == BtLink::Phase::PairScan || link.phase == BtLink::Phase::Pairing || btSession.pairingUnderWay();
}

// Every loop pass, after the player's (a pause this pass counts from now).
// IdlePolicy decides; this carries it out: the warning (the UI reads it
// from the snapshot), then the queue flushed, the note for the next boot,
// the headphones let go, and once they are gone (at most 3 s) the power.
static void stepIdle(uint32_t now, bool input) {
  BtSink& bt = audio.bluetooth();
  IdlePolicy::In in;
  in.nowMs = now;
  in.play = player.state();
  in.usb = screen.externalPower() && !idleFakeBattery;
  in.input = input;
  in.pairing = pairingUnderWay();
  in.queueWrite = queueStore.busy();
  in.busy = screenTaken() || usbViz.active();  // (the visualizer: someone is watching)
  in.linked = bt.linkUp();  // (until the disconnect is done: connected() drops as it starts)
  IdlePolicy& p = idlePower.policy();
  const IdlePolicy::Phase before = p.phase();
  const IdlePolicy::Out o = p.update(in);
  // What it waits for, when that changes (not every touch: those restart
  // the countdown quietly).
  if (p.phase() != before && (p.phase() == IdlePolicy::Phase::Blocked || before == IdlePolicy::Phase::Blocked) &&
      p.phase() != IdlePolicy::Phase::Releasing) {
    if (p.phase() == IdlePolicy::Phase::Blocked) {
      Serial.printf("[power] idle: waiting (%s)\n", IdlePolicy::blockerName(p.blocker()));
    } else {
      Serial.printf("[power] idle: counting: off in %lu s unless something happens\n",
                    (unsigned long)((p.msLeft(now) + 999) / 1000));
    }
  }
  if (o.warn) {
    Serial.printf("[power] idle: turning off in %lu s (%s); any input keeps it on\n",
                  (unsigned long)p.warnSeconds(now),
                  screen.off() ? "the screen is off and stays off: it may be night"
                               : "the warning is up, and the screen stays lit until it ends");
  }
  if (o.warnEnd) {
    Serial.printf("[power] idle: kept on (%s)\n", p.blocker() != IdlePolicy::Blocker::None
                                                      ? IdlePolicy::blockerName(p.blocker())
                                                      : "input");
  }
  if (o.shutdown) {
    const uint32_t len = p.lengthMs();
    Serial.printf("[power] off after idle (%lu %s %s, on battery, no input): saving the queue, letting go of the "
                  "headphones\n",
                  (unsigned long)(len >= 60000 ? len / 60000 : len / 1000), len >= 60000 ? "min" : "s", stateName());
    queueStore.flushNow();
    idlePower.noteOff(p.lengthMs());
    releaseHeadphones("[power] turning off");
  }
  if (o.cancelled) {
    idlePower.clearNote();
    Serial.printf("[power] idle: not turning off after all (%s); the headphones stay let go (play pages them)\n",
                  p.blocker() != IdlePolicy::Blocker::None ? IdlePolicy::blockerName(p.blocker()) : "input");
  }
  if (o.powerOff) {
    // The last look at the power: plugged in during the release, it stays
    // on (the AXP192 wouldn't stay off anyway).
    if (ScreenControl::readExternalPower()) {
      p.cancel(now);
      idlePower.clearNote();
      Serial.println("[power] idle: USB power at the last moment: staying on");
      return;
    }
    Serial.printf("[power] off now (%s)\n", bt.linkUp() ? "the headphones still linked after 3 s" : "headphones let go");
    haptics.stop();
    idlePower.powerOff();  // (doesn't return)
  }
}

// ---- the CPU speed (docs/ENERGY.md item 6) ----

// A restart asked for at a new CPU speed (the Output tab's, after its
// dialog): the speed (0: none), and when.
static uint16_t cpuRestartMhz = 0;
static uint32_t cpuRestartAskedMs = 0;

// The Output tab's CPU speed. 240 <-> 160 retunes the PLL the Bluetooth
// radio runs from, so the clock is set at boot only: the choice is saved,
// and when it isn't the clock that runs, the player restarts at it, the
// idle power-off's orderly way: paused first (after the restart nothing
// plays by itself: the queue comes back stopped, as after any boot, at the
// second it paused at: the flush saves the resume point), the queue and its
// place flushed, the note for the next boot's toast, the
// headphones let go (a clean disconnect, no "lost" dialog). The speaker's
// amp is switched off too, the orderly way (its enable before its I2S, once
// the pause's fade has played out): esp_restart() doesn't reset the AXP192,
// so the amp would otherwise stay live while the reset and M5.begin()
// reconfigure its clock pins (a pop). stepCpuRestart() restarts once both
// are done, at most 3 s later. Not while a pairing is under way: the
// restart would drop it, maybe half-bonded (the UI says to wait).
bool MainUiHost::setCpuSpeed(uint16_t mhz) {
  if (cpuRestartMhz) {
    // (Another tap on the row in the restart's last 3 s.)
    Serial.printf("[power] CPU speed %u MHz: ignored, restarting at %u MHz already\n", (unsigned)mhz,
                  (unsigned)cpuRestartMhz);
    return false;
  }
  const bool restart = powerchoice::validCpuMhz(mhz) && mhz != PowerSettings::cpuBootMhz();
  if (restart && pairingUnderWay()) {
    Serial.printf("[power] CPU speed %u MHz: not now, a pairing is under way (nothing saved)\n", (unsigned)mhz);
    return false;
  }
  const uint16_t before = powerSettings.cpuSaved();
  if (!powerSettings.saveCpu(mhz)) {
    Serial.printf("[power] CPU speed %u MHz: couldn't save it\n", (unsigned)mhz);
    return false;
  }
  if (!restart) {
    // (The console's Pcb had saved the other one since this boot.)
    Serial.printf("[power] CPU speed: %u -> %u MHz (saved): it runs at that already, no restart\n",
                  (unsigned)before, (unsigned)mhz);
    return false;
  }
  const bool paused = pauseIfPlaying();
  const bool amp = SpeakerSink::ampOn();
  Serial.printf("[power] CPU speed: %u -> %u MHz (saved): restarting%s; saving the queue, letting go of the "
                "headphones%s\n",
                (unsigned)before, (unsigned)mhz, paused ? " (paused first)" : "",
                amp ? ", the speaker's amp off" : "");
  queueStore.flushNow();
  powerSettings.noteRestart(mhz);
  releaseHeadphones("[power] restarting");
  // Carried out by the speaker's pump once its fade has played out (as Pa0).
  if (amp) audio.speaker().requestAmp(SpeakerSink::Amp::Off);
  cpuRestartMhz = mhz;
  cpuRestartAskedMs = millis();
  return true;
}

// Every loop pass: the restart setCpuSpeed() asked for, once the headphones
// are gone and the speaker's amp is off (at most 3 s, as before the idle
// power-off).
static void stepCpuRestart(uint32_t now) {
  if (!cpuRestartMhz) return;
  // (Until the disconnect is done: connected() drops as soon as it starts.)
  const bool linked = audio.bluetooth().linkUp();
  const bool amp = SpeakerSink::ampOn();
  if (!powerchoice::cpuRestartDue(now, cpuRestartAskedMs, linked, amp)) return;
  if (queueStore.busy()) queueStore.flushNow();  // an edit meanwhile
  Serial.printf("[power] restarting now at %u MHz (%s%s)\n", (unsigned)cpuRestartMhz,
                linked ? "the headphones still linked after 3 s" : "headphones let go",
                amp ? "; the speaker's amp still on after 3 s" : "");
  haptics.stop();
  powerSettings.restart();  // (doesn't return)
}

// NVS (Preferences: the settings, the touch calibration, the Bluetooth
// pairing and bond, the resume point) must work before the first read, the
// boot clock's. initArduino() erases and retries it only for NO_FREE_PAGES
// and NEW_VERSION_FOUND. Foreign bytes in the region (a unit moved to this
// flash layout, where NVS sits on old LittleFS blocks) are normally no
// problem: NVS treats them as corrupt pages and erases each before use. This
// is the net for anything else, which would otherwise leave every
// Preferences call failing, silently, until the flash is erased. A second
// nvs_flash_init() is ESP_OK at once when the first worked (Preferences
// calls it on every labelled begin()). Out of memory or no partition: no
// erase, nothing unreadable to fix. Logged once Serial is up.
static esp_err_t nvsBootErr = ESP_OK;    // the first init's failure, if any
static esp_err_t nvsRetryErr = ESP_OK;   // after the erase
static bool nvsErased = false;

static void ensureNvs() {
  nvsBootErr = nvs_flash_init();
  if (nvsBootErr == ESP_OK || nvsBootErr == ESP_ERR_NO_MEM || nvsBootErr == ESP_ERR_NOT_FOUND) return;
  nvsRetryErr = nvs_flash_erase();
  if (nvsRetryErr != ESP_OK) return;
  nvsErased = true;
  nvsRetryErr = nvs_flash_init();
}

static void logNvs() {
  if (nvsBootErr == ESP_OK) return;
  if (nvsErased && nvsRetryErr == ESP_OK) {
    Serial.printf("[nvs] %s: erased, now ok (the settings, calibration and pairing start over)\n",
                  esp_err_to_name(nvsBootErr));
  } else if (nvsErased) {
    Serial.printf("[nvs] FAILED: %s, and %s after an erase: settings won't be saved\n",
                  esp_err_to_name(nvsBootErr), esp_err_to_name(nvsRetryErr));
  } else {
    Serial.printf("[nvs] FAILED: %s (%s%s): settings won't be saved\n", esp_err_to_name(nvsBootErr),
                  nvsRetryErr == ESP_OK ? "not erased: it wouldn't help" : "the erase failed: ",
                  nvsRetryErr == ESP_OK ? "" : esp_err_to_name(nvsRetryErr));
  }
}

// The build leaves the rev-1 PSRAM cache workaround out of the code it
// compiles (tools/no_psram_fix.py, docs/ENERGY.md section 5, P3a), which is
// safe only from revision 3 (every Core2 is an ESP32-D0WDQ6-V3), and the
// shipped sdkconfig doesn't refuse an older chip. So on one, say so on the
// console and the screen and stop, before anything here uses PSRAM.
static void haltOnOldChip() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  if (chiprev::supported(chip.revision)) return;
  char rev[8];
  chiprev::text(chip.revision, rev, sizeof(rev));
  char line[96];
  snprintf(line, sizeof(line), "this ESP32 is revision %s; this firmware needs revision 3 or later", rev);
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.internal_mic = false;
  cfg.output_power = false;
  M5.begin(cfg);
  M5.Display.setBrightness(128);
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 8);
  M5.Display.println(line);
  for (;;) {
    Serial.printf("[boot] %s\n", line);  // again every 5 s, for a console opened later
    delay(5000);
  }
}

void setup() {
  haltOnOldChip();  // first: no PSRAM used yet
  ensureNvs();  // before the first Preferences read
  nvsschema::check();  // the layout's number, migrated if older, before anything reads a key
  // The CPU speed saved (or the default), before Bluetooth starts.
  PowerSettings::applyBootClock();
  // The computer's visualizer lines (docs/USB-VISUALIZER.md: ~3.7 KB/s)
  // must outlast a slow loop pass: 1 KB (270 ms of them) instead of 256 B.
  // Only before begin(), which M5.begin() calls.
  Serial.setRxBufferSize(SerialConsole::kRxBuffer);
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;  // M5Unified leaves Serial off unless asked
  cfg.internal_mic = false;      // the mic shares GPIO0 with the speaker's I2S clock
  // The 5 V boost (EXTEN, the M-Bus/Grove 5 V) off: nothing is plugged in,
  // and the speaker amp isn't fed from it on USB (measured; ENERGY.md item
  // 9, still to check by ear on battery). The console's Pe1 turns it on.
  cfg.output_power = false;
  M5.begin(cfg);
  // Before the banner (a sender waits for it): was a computer's line under
  // way as Serial started? (HostLine's Sync: its tail is no key.)
  console.begin();
  Serial.printf("\nmstream-mp3-player %s (commit %s, %s), ELF %s\n", version::player(),
                version::commit()[0] ? version::commit() : "none", version::commitDate(), version::elfSha());
  // The licence notice once (GPLv3 section 5(d); About shows it too).
  Serial.printf("Copyright (C) 2026 IrosTheBeggar. Licence: %s.\n", uitext::kAboutLicence);
  Serial.printf("Source, licence texts and third-party notices: %s\n", uitext::kSourceUrl);
  board::requireCore2();  // another board: says so and stops here, before the card, audio and Bluetooth
  diag::logRunningPartition();  // the flash layout (console L: the whole table)
  logNvs();  // only when NVS needed ensureNvs()
  nvsschema::log();
  powerSettings.begin();  // what the boot clock is, the Bluetooth power, and whether it restarted for the speed
  board::applyBootPower();  // the IMU suspended: nothing reads it
  diag::logHeap("boot");

  bootScreen.begin(version::player());

  storage.begin();
  diag::logHeap("storage");

  // The Bluetooth power, applied as the controller comes up (before any page).
  powerSettings.beginBluetooth(audio.bluetooth());
  if (!audio.begin(storage.available() ? &storage.fs() : nullptr, BT_SINK_NAME)) {
    Serial.println("[audio] failed to start");
  }
  // 88.2/96 kHz tracks need 240 MHz: the speed set at boot, not the clock
  // of the moment (the console's Pc80 lowers that for quiet spells).
  audio.setCpuMhz(PowerSettings::cpuBootMhz());
  diag::logHeap("audio");

  // The library (from the card's cache when /music is unchanged) and the
  // queue as it was. Both live in PSRAM: this measures what they still cost
  // in internal RAM (the old std::vector<Track> playlist, 77 tracks, took
  // ~13 KB for its two copies).
  const uint32_t freeBeforeLibrary = diag::heap().internalFree;
  library.begin();
  if (!queueStore.restore()) {
    queueEverything(false);
    Serial.printf("[queue] no saved queue: the whole library, %lu tracks\n",
                  (unsigned long)queue.size());
  }
  Serial.printf("[lib] library + queue: internal RAM %lu B free before, %lu B after\n",
                (unsigned long)freeBeforeLibrary, (unsigned long)diag::heap().internalFree);
  diag::logHeap("library");

  const auto rows = diagnosticsRows();
  for (const auto& row : rows) Serial.printf("[diag] %-10s %s\n", row.label.c_str(), row.value.c_str());
  bootScreen.show(rows);
  {
    BtSink& bt = audio.bluetooth();
    const char* headphones = bt.sinkName();
    Serial.printf("[bt] headphones: %s\n",
                  bt.scansByName() && headphones[0]
                      ? ("with none remembered, a scan for a name containing \"" + String(headphones) + "\"").c_str()
                      : "paired on Output > Pair new headphones only (never picked by the Core2 itself)");
  }
  player.setHold(&outputHold);  // a play waits while the headphones aren't connected (PlayGate)
  danceMode.begin();
  usbViz.begin(version::player());
  diag::logHeap("dance");
  haptics.begin();
  input.begin();
  screen.begin(millis());
  idlePower.begin(millis());  // the setting, and whether it turned itself off last time
  screen.onDark([](bool dark) {
    if (userInterface) userInterface->setDark(dark);
  });
  spike.onScreenReleased(uiResume);  // the UI draws again
  spike.onRebuild(rebuildLibrary);
  {
    const LibraryIndex* index = library.index();
    durations.reset(index && index->ready() ? index->trackCount() : 0);
  }
  // The UI, in PSRAM (its sprites too): it takes the display after the boot screen.
  const uint32_t freeBeforeUi = diag::heap().internalFree;
  userInterface = psramNew<ui::Ui>(uiHost, input, player, queue, library, danceMode);
  if (!userInterface || !userInterface->begin()) {
    Serial.println("[ui] can't start the UI (no PSRAM): the console still works");
    psramDelete(userInterface);
    userInterface = nullptr;
  }
  Serial.printf("[ui] internal RAM %lu B free before the UI, %lu B after\n", (unsigned long)freeBeforeUi,
                (unsigned long)diag::heap().internalFree);
  // The rescue hold, on the start-up screen (with the UI's fonts, now loaded).
  if (userInterface) {
    bootScreen.hint(uitext::kBootTouchHint);
    bootHintMs = millis();
  }
  diag::logHeap("ui");
  Serial.println("[console] n/p next/prev, space play/pause, o output, +/- volume, s stats, l list, "
                 "f forget bt, z silent test mode, d dance tab, m next dancer, x/X screenshot dancer/screen, v beat log; "
                 "with Enter: i<n> play, b<n> bench, c<name> headphones name, h<n> bt headroom -n dB, "
                 "q queue (q? for its commands; qs<sec> a resume point), "
                 "a touch calibration (a5-a9 fewer crosses, ac check, ab first-boot check, ab0 ask it again, "
                 "as status, ad remove it, ah0/1 haptics, ar0/1 rail ticks), "
                 "t<bpm> tempo prior (t clears), y<ms> dance latency offset, k<n> freeze pose 0-15 (k unfreezes); "
                 "ui the UI's navigation (ui0-ui4 tab, uib back, uic coach cards, uit/uih/uis/uid/uip scripted finger, "
                 "uk1/uk2/uk0 the scripted finger on a skewed panel (uk2 with jitter) or not, "
                 "uiF<c/s/p/r/l/n/f/w> show a faked Bluetooth, no-card or not-FAT32 state (uiF0 the real one), uiV the volume HUD, uil<n> a synthetic "
                 "library of n tracks in the Library tab, uil0 the card's); "
                 "UI spike (with Enter): u input lab (u0-u3, us summary), w scroll lab (w0 interactive, w1-w3 stress, wm0/wm1 redraw/hw scroll, wp refill pacing), "
                 "g library index (g0 SD card, g<n> synthetic), e font probe (e1-e5), j thumbnail probe (j<n>, jw, ja); "
                 "P power measurement (P a line, Pl log, P? the knobs; Ps the screen, Ps0/Ps1 off/on); "
                 "T sleep timer (T status, T<min>, Ts<sec> for tests, Tt/Ta/Tq end of track/album/queue, T+ +10 min, "
                 "T0 off); I idle power-off (I status, I<min>/Is<sec> a test length, I0 the setting's); "
                 "R rate converter (R status, Rt test tracks, Rt<n> play one on its own, Rf</music/...> a file on its own "
                 "(silent mode), Rx stop it, Rb bench); "
                 "@ lines: a computer's (the USB visualizer, docs/USB-VISUALIZER.md), never commands");
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  idleInput = false;
  handleInput(now);
  if (console.poll()) idleInput = true;  // someone is at the console
  handleBluetooth();
  // Nobody around: the screen is off and nothing plays or waits. After a
  // burst the search for the headphones then rests at once (ENERGY.md item
  // 1).
  // Not after a drop while listening (btLost): the pause is the drop's,
  // and the listener may still be wearing them: the back-off runs its 15
  // minutes (a range drop is what it is for).
  // (And while a computer drives the dancer: the player is paused for it.)
  audio.bluetooth().setQuiet((screen.off() && player.state() != PlayState::Playing &&
                              player.state() != PlayState::Waiting && !btLost) ||
                             usbViz.active());
  btSession.update(audio.bluetooth().link(), now);
  stepPlayGate(now);
  // The fade's toast: a touch (swallowed as a wake, or on a lit screen) or
  // the PWR key while it fades; only its buttons act on the timer.
  const bool woken = screen.takeWoken();
  if (sleepTimer.fading() && (woken || touchedThisPass) && userInterface) userInterface->sleepFading();
  stepSleep(now);
  player.update(now);
  stepIdle(now, idleInput || touchedThisPass || screen.takeInput());
  powerSettings.update(audio.bluetooth().connected());
  stepCpuRestart(now);
  audio.loop(now);
  powerLab.loop(now);
  queueStore.loop(now);
  // What a track's length is, once the backend knows it (read from the
  // file; or its estimate 20 s in, exact for a constant bitrate): the
  // Queue's "49 min"; once per entry (each change redoes the Queue's sum).
  // A skip changes the entry at once, but the backend starts it a moment
  // later: until then the position and length are the last track's. So a
  // length is noted only once this entry has started: a start since the
  // change, the position gone back, or a change right at a track's start.
  // (Counted from where the track started: a resume point starts it part
  // of the way in.)
  // (The player playing it: a track the console's Rt plays on its own,
  // with the player stopped, isn't the current entry's.)
  if (queue.current() >= 0 && player.state() == PlayState::Playing && audio.isPlaying()) {
    static uint32_t lengthKey = QueueModel::kNone;
    static uint8_t lengthNoted = 0;  // 1 the estimate, 2 the file's
    static uint32_t startSeqAtKey = 0, posAtKey = 0;
    static bool started = false;
    const uint32_t at = audio.positionMs(), from = audio.startOffsetMs();
    const uint32_t pos = at > from ? at - from : 0;
    const uint32_t seq = audio.startTiming().seq;
    if (queue.currentKey() != lengthKey) {
      lengthKey = queue.currentKey();
      lengthNoted = 0;
      startSeqAtKey = seq;
      posAtKey = pos;
      started = pos < 1000;  // this entry's start already, or the last one barely begun
    }
    if (!started && (seq != startSeqAtKey || pos < posAtKey)) started = true;
    if (!started) {
      // not this entry's position yet
    } else if (lengthNoted < 2 && audio.durationKnown() && pos > 3000) {
      durations.note(queue.currentTrack(), audio.durationMs());
      lengthNoted = 2;
    } else if (lengthNoted == 0 && pos > 20000) {
      durations.note(queue.currentTrack(), audio.durationMs());
      lengthNoted = 1;
    }
  }
  if (restartAtMs && static_cast<int32_t>(now - restartAtMs) >= 0) {
    Serial.flush();
    ESP.restart();
  }

  // A new track (skip, jump, natural end, a queue edit) drops the tempo prior.
  static uint32_t lastEntry = QueueModel::kNone;
  if (queue.currentKey() != lastEntry) {
    if (lastEntry != QueueModel::kNone) danceMode.onTrackChanged();
    Serial.printf("[queue] now at %d of %lu (%s)\n", queue.current() + 1, (unsigned long)queue.size(), stateName());
    lastEntry = queue.currentKey();
  }
  // A screen of its own (calibration, a spike screen) owns the display
  // while it's up: the UI is suspended, the dancer too.
  spike.loop(now);
  if (calibration) calibration->loop(now);
  if (screenTaken() && danceMode.active()) danceMode.setActive(false);
  // The dancer (the Dance tab) draws its own frames (10/s idle, 24-30
  // dancing: DanceRate) into its box.
  danceMode.loop(now, silent);
  // The UI: the tab bar, overlays, the page (list frames on 30 fps deadlines).
  if (rescueFinger && !calibrationUp() && now - rescueDownMs >= kRescueHoldMs) {
    rescueFinger = false;
    touchCheckOffered = true;  // (the calibration is what it would offer)
    Serial.println("[cal] a finger held on the start-up screen: the calibration (the rescue)");
    input.tapTick();
    openCalibration(CalibrationScreen::Start::Crosses);
  }
  if (userInterface) {
    // The first boot with nothing calibrated: the touch check, before the
    // UI (and so before its tips). The UI waits for it, and for a finger
    // resting on the start-up screen (the rescue hold).
    const bool bootShown = now >= kDiagnosticsScreenMs && now - bootHintMs >= kRescueWindowMs;
    if (!userInterface->started() && bootShown && !screenTaken() && !rescueFinger && !touchCheckOffered) {
      touchCheckOffered = true;
      if (touchcheck::due(input.calibrated(), input.touchCheckAnswered())) {
        openCalibration(CalibrationScreen::Start::FirstBoot);
      }
    }
    if (!userInterface->started() && bootShown && !screenTaken() && !rescueFinger) {
      userInterface->start(now);
      // It turned itself off last time, or restarted for a new CPU speed:
      // say so, once (stopped or paused where it was).
      char note[48];
      if (idlePower.takeBootNote(note, sizeof(note)) || powerSettings.takeBootNote(note, sizeof(note))) {
        userInterface->note(note, 6000);
      }
    }
    userInterface->loop(now);
  }
  // The computer's visualizer: its timeout, USB unplugged, the Dance tab gone.
  usbViz.loop(now, screen.externalPower());
  shot.poll();
  // The screen, last: the countdown, and what keeps it lit (a screen of its
  // own, a play waiting for the headphones, a pairing, a computer driving the
// dancer). Going off, the UI
  // goes dark first; waking, it draws everything before the panel's
  // sleep-out. A toast with a countdown (the idle power-off's warning, the
  // sleep timer's fade while it counts down to the pause) holds a lit screen
  // lit until it ends, and leaves an off one off (it may be night). A
  // track's fade held after a skip (until the new track's last 10 s, or the
  // album's end) keeps its toast but not the screen: that can be minutes.
  {
    const BtLink link = audio.bluetooth().link();
    // (A pairing under way, not one whose failure the card still shows.)
    const bool keepLit = screenTaken() || player.state() == PlayState::Waiting ||
                         link.phase == BtLink::Phase::Pairing || btSession.pairingUnderWay() || usbViz.active();
    const bool holdLit = idlePower.policy().phase() == IdlePolicy::Phase::Warning || sleepTimer.fadeCountingDown();
    screen.step(millis(), keepLit, holdLit);
  }
  // A calibration asked for in the dark: now that the panel is awake.
  if (calibrationPending && !screen.off() && !screen.panelAsleep()) {
    openCalibration(pendingHow, pendingTargets);
  }

  static uint32_t lastStats = 0;
  static bool heapLoggedWhilePlaying = false;
  if (!heapLoggedWhilePlaying && audio.isPlaying() && audio.positionMs() > 2000) {
    diag::logHeap("playing");
    heapLoggedWhilePlaying = true;
  }
  if (now - lastStats >= 5000) {
    printStats();
    lastStats = now;
  }
  // Yield every pass (the decoder runs above the loop on this core), less
  // while a list frame is due soon, 20 ms while the screen is off (the UI
  // says so: nothing to draw, touches only wake; ENERGY.md item 9).
  // (A power measurement's Pd stretches the wait while nothing animates.)
  const uint32_t uiIdleMs = userInterface ? userInterface->idleMs(millis()) : screen.off() ? 20 : 5;
  delay(powerLab.loopDelayMs(uiIdleMs, danceMode.active()));
}
