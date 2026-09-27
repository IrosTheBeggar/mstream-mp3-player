// mstream-mp3-player — firmware entry point (M5Stack Core2).
//
// Wires the portable core (PlaybackController over the queue and the
// library index) to the Core2 audio backend, storage, the UI (ui/Ui: the
// tab bar and its pages, the one owner of the display), the input layer
// (touch buttons and glass) and a serial console. The queue is restored
// from the card at boot; the first time it's the whole library followed by
// the built-in test tones.

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_chip_info.h>

#include <cmath>
#include <vector>

#include "ButtonPolicy.h"
#include "HeadsetKeys.h"
#include "InputEvent.h"
#include "OutputModel.h"
#include "PlayGate.h"
#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueView.h"
#include "TrackCatalog.h"
#include "app/DanceMode.h"
#include "app/Diagnostics.h"
#include "app/Haptics.h"
#include "app/Library.h"
#include "app/Psram.h"
#include "app/QueueStore.h"
#include "app/Screenshot.h"
#include "app/SerialConsole.h"
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
// Shown in About.
#ifndef PLAYER_VERSION
#define PLAYER_VERSION "0.4-dev"
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
// What the touch buttons do, the same on every screen.
static ButtonPolicy buttonPolicy;
// The touch calibration screen (console a): in PSRAM, made on first use.
static CalibrationScreen* calibration = nullptr;
static bool calibrationUp() { return calibration && calibration->active(); }
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

static void toggleOutput() {
  if (silent) {
    Serial.println("[test] silent mode: the output stays on the speaker");
    return;
  }
  audio.setOutput(audio.output() == Output::Speaker ? Output::Bluetooth : Output::Speaker);
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
  void prev() override { player.prev(); }
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
  bool switchOutput() override {
    if (silent) {
      Serial.println("[test] silent mode: the output stays on the speaker");
      return false;
    }
    return selectOutput(!onBluetooth());
  }
  // False: refused (no headphones paired since Forget: nothing to connect to).
  static bool selectOutput(bool bluetooth);
};
static ButtonTransport buttonTransport;

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
      Serial.println("[output] bluetooth: no headphones paired (forgotten): pair them on the Output tab");
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
// connecting, s searching, p pairing, l the headphones lost (the dialog
// too), n no card (on Now Playing), w play waiting for the headphones (Now
// Playing's panel); uiF0 (or uiF) the real state. Display only: a button
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
    BtSink& bt = audio.bluetooth();
    s.onBluetooth = audio.output() == Output::Bluetooth;
    s.btConnected = bt.connected();
    s.btLost = btLost && s.onBluetooth && !s.btConnected;
    // Their name (read at each link; before the first, the name looked for).
    snprintf(s.btName, sizeof(s.btName), "%s", bt.deviceName()[0] ? bt.deviceName() : bt.sinkName());
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
    fake(s);
  }
  static void fake(ui::AppState& s) {
    if (!uiFake) return;
    if (uiFake == 'n') {
      // As after a boot with no card: nothing indexed, nothing queued (Now
      // Playing shows it; the Library and Queue lists read the real index
      // and queue, so they don't).
      s.card = false;
      s.libraryTracks = 0;
      s.current = -1;
      return;
    }
    s.btConnected = false;
    s.btDetail[0] = 0;
    s.btLink.remembered = true;
    s.btLink.phase = uiFake == 's' ? BtLink::Phase::Scanning
                     : uiFake == 'p' ? BtLink::Phase::Pairing
                                     : BtLink::Phase::Paging;
    s.btLink.attempt = 2;
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
  void prev() override { player.prev(); }
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
    return ButtonTransport::selectOutput(bluetooth);
  }
  void openCalibration() override;
  void btConnect() override {
    if (silent) {
      Serial.println("[test] silent mode: bluetooth stays off");
      return;
    }
    if (audio.bluetooth().nothingToFind()) {
      Serial.println("[ui] bluetooth: connect: no headphones paired");
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
      Serial.println("[storage] try again: still no card");
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
    snprintf(a.version, sizeof(a.version), "%s, built %s", PLAYER_VERSION, __DATE__);
    BtSink& bt = audio.bluetooth();
    const BtLink l = bt.link();
    snprintf(a.bluetooth, sizeof(a.bluetooth), "%s%s", l.remembered ? (bt.deviceName()[0] ? bt.deviceName() : "paired") : "none paired",
             bt.connected() ? ", connected" : "");
    const diag::Heap h = diag::heap();
    a.ramFree = h.internalFree;
    a.ramMin = h.internalMin;
    a.psramFree = h.psramFree;
  }
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
  if (!bt.connected() && audio.output() != Output::Bluetooth) return;
  // vol/control: the Bluetooth volume and who applies it (headphones = AVRCP
  // absolute volume, asking = waiting for them to accept it, software = the
  // Core2). headphones: their last reported volume. gain: the Core2's gain
  // stage (the headroom with absolute volume). headroom: its fixed
  // attenuation (-2.0dB unless set with h<n>). gap: longest wait between two
  // data callbacks since the last line (~10-30 ms is healthy).
  const BtSink::Stats b = bt.stats();
  char gain[12] = "mute";
  if (b.gainQ15 > 0) snprintf(gain, sizeof(gain), "%.1fdB", 20.0f * log10f(b.gainQ15 / 32768.0f));
  char headset[8] = "?";
  if (b.headsetVolume >= 0) snprintf(headset, sizeof(headset), "%d", b.headsetVolume);
  Serial.printf("[stats] bt vol=%u%% control=%s headphones=%s/127 gain=%s headroom=%.1fdB stream=%s gap=%lums "
                "events_dropped=%lu btapp_stack_free=%lu\n",
                (unsigned)b.volume, b.volumeControl, headset, gain, 20.0f * log10f(b.headroomQ15 / 32768.0f),
                b.stream, (unsigned long)b.maxGapMs, (unsigned long)b.eventsDropped,
                (unsigned long)b.appTaskStackFree);
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

// The whole library, then the built-in tracks, from the first: played now,
// or (start false) only queued, stopped, the way the playlist always was.
static bool queueEverything(bool start) {
  const LibraryIndex* index = library.index();
  const LibraryIndex::Span lib = index && index->ready() ? index->allTracks() : LibraryIndex::Span{};
  const LibraryIndex::Span builtins = TrackCatalog::builtins();
  const uint32_t n = lib.count + builtins.count;
  auto* ids = static_cast<uint32_t*>(psramAlloc(n * sizeof(uint32_t)));
  if (!ids) return false;
  if (lib.count) memcpy(ids, lib.ids, lib.count * sizeof(uint32_t));
  memcpy(ids + lib.count, builtins.ids, builtins.count * sizeof(uint32_t));
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
    default:
      Serial.println("[queue] q status, qa play all, qb built-ins, ql albums, qp<n>/qn<n>/q+<n> album n: play / "
                     "play next / add, qr<pos> remove, qc clear up next, qx clear, qu undo");
      return;
  }
  queueStore.printStatus();
}

static void openCalibration(int targets, bool check) {
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
  if (check) {
    calibration->openCheck();
  } else {
    calibration->open(targets);
  }
}

void MainUiHost::openCalibration() { ::openCalibration(CalibrationScreen::kMaxTargets, false); }

static void closeCalibration() {
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
      openCalibration(CalibrationScreen::kMaxTargets, false);
    }
    return;
  }
  if (c >= '0' && c <= '9') {
    const int n = atoi(a);
    if (n < CalibrationScreen::kMinTargets || n > CalibrationScreen::kMaxTargets) {
      Serial.println("[input] a<n>: 5-9 crosshairs");
      return;
    }
    openCalibration(n, false);
    return;
  }
  if (c == 'q') {
    closeCalibration();
    return;
  }
  if (c == 'c') {
    openCalibration(0, true);
    return;
  }
  if (c == 'd') {
    input.resetCalibration();
    Serial.println("[input] touch: back to the default table (saved)");
  } else if (c == 'h' && flag) {
    input.setHapticsOn(on);
  } else if (c == 'r' && flag) {
    input.setRailTicksOn(on);
  } else if (c != 's') {
    Serial.println("[input] a calibrate (9 crosshairs; a5-a9: fewer), ac check the touch, as status, ad default "
                   "table, ah0/ah1 haptics off/on, ar0/ar1 rail ticks off/on, aq close");
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
// that ends there (no button).
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

static SerialConsole console({
    [] { player.next(); },
    [] { player.prev(); },
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
      player.stop();
      audio.bench(path);
    },
    [] {
      audio.bluetooth().forgetDevice(/*waitMs=*/3000);  // before the restart
      Serial.println("[bt] forgot the remembered device; restarting to scan");
      Serial.flush();
      ESP.restart();
    },
    [](const char* name) {
      audio.bluetooth().setSinkName(name);
      Serial.printf("[bt] headphones: %s (saved)\n",
                    name[0] ? ("name contains \"" + String(name) + "\"").c_str() : "any very close device");
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
    if (e.type == InputEvent::Type::Click) Serial.printf("[button] %c click: nothing to play\n", b);
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
static void handleInput(uint32_t now) {
  input.setSuspended(spike.ownsInput());
  input.update(now);
  for (InputEvent e; input.poll(e);) {
    if (e.isButton()) {
      handleButton(e);
      continue;
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
      Serial.printf("[touch] %s %d,%d (raw %d,%d)%s\n", InputEvent::name(e.type), e.x, e.y, e.rawX, e.rawY,
                    input.scriptedTouch() ? " scripted" : "");
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
      case BtSink::Event::Disconnected:
        Serial.println("[bt] disconnected");
        if (btSession.dropExpected()) {
          // Let go on purpose (Disconnect, Forget, a new pairing): the
          // audio is on the speaker already, paused.
          btSession.dropSeen();
          break;
        }
        // Like a phone: don't carry on through the speaker, pause (and say why).
        if (audio.output() == Output::Bluetooth) {
          btLost = true;
          if (pauseIfPlaying() && userInterface) userInterface->headphonesLost();
        }
        break;
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
        if (HeadsetKeys::decide(player.state(), HeadsetKeys::Key::Play) != HeadsetKeys::Action::Resume) {
          Serial.printf("[bt] headphones: play (ignored: %s)\n", stateName());
          break;
        }
        Serial.println("[bt] headphones: play");
        if (bt.connected() && audio.output() != Output::Bluetooth && !silent) audio.setOutput(Output::Bluetooth);
        HeadsetKeys::apply(player, HeadsetKeys::Key::Play);
        break;
      case BtSink::Event::Pause:
        Serial.println("[bt] headphones: pause");
        if (audio.output() != Output::Bluetooth) break;  // not the speaker's playback
        HeadsetKeys::apply(player, HeadsetKeys::Key::Pause);
        // They pick their next key from the stream: suspend it now, so the
        // next press is PLAY. Also when we were paused already (paused on the
        // Core2, the stream still in its 3 s tail): this press did nothing,
        // the next one plays.
        bt.suspendPromptly();
        break;
      case BtSink::Event::Next:
      case BtSink::Event::Prev: {
        const bool next = e == BtSink::Event::Next;
        const HeadsetKeys::Action a =
            HeadsetKeys::apply(player, next ? HeadsetKeys::Key::Next : HeadsetKeys::Key::Prev);
        Serial.printf("[bt] headphones: %s (track %d, %s)\n", next ? "next" : "previous", player.currentIndex(),
                      a == HeadsetKeys::Action::Skip ? "playing" : "selected, not started");
        break;
      }
      case BtSink::Event::VolumeUp:
        stepBluetoothVolume(+kHeadphoneVolumeStep);
        if (userInterface) userInterface->volumeKeys();  // the same HUD as the A/C holds
        break;
      case BtSink::Event::VolumeDown:
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
  in.nowMs = now;
  const PlayGate::Do d = playGate.step(in, player, btSession);
  if (d == PlayGate::Do::None) return;
  const char* name = bt.deviceName()[0] ? bt.deviceName() : bt.sinkName();
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
    case PlayGate::Do::Ended:
      // Cancelled (or the speaker, or stopped): a link that comes later
      // answers nothing; the radio carries on as it was.
      Serial.printf("[play] the wait for %s ended (%s)\n", name, stateName());
      break;
    case PlayGate::Do::None:
      break;
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;  // M5Unified leaves Serial off unless asked
  cfg.internal_mic = false;      // the mic shares GPIO0 with the speaker's I2S clock
  M5.begin(cfg);
  Serial.println("\nmstream-mp3-player");
  diag::logHeap("boot");

  bootScreen.begin();

  storage.begin();
  diag::logHeap("storage");

  if (!audio.begin(storage.available() ? &storage.fs() : nullptr, BT_SINK_NAME)) {
    Serial.println("[audio] failed to start");
  }
  diag::logHeap("audio");

  // The library (from the card's cache when /music is unchanged) and the
  // queue as it was. Both live in PSRAM: this measures what they still cost
  // in internal RAM (the old std::vector<Track> playlist, 77 tracks, took
  // ~13 KB for its two copies).
  const uint32_t freeBeforeLibrary = diag::heap().internalFree;
  library.begin();
  if (!queueStore.restore()) {
    queueEverything(false);
    Serial.printf("[queue] no saved queue: the whole library, %lu tracks with the built-in ones\n",
                  (unsigned long)queue.size());
  }
  Serial.printf("[lib] library + queue: internal RAM %lu B free before, %lu B after\n",
                (unsigned long)freeBeforeLibrary, (unsigned long)diag::heap().internalFree);
  diag::logHeap("library");

  const auto rows = diagnosticsRows();
  for (const auto& row : rows) Serial.printf("[diag] %-10s %s\n", row.label.c_str(), row.value.c_str());
  bootScreen.show(rows);
  const char* headphones = audio.bluetooth().sinkName();
  Serial.printf("[bt] headphones: %s\n",
                headphones[0] ? ("name contains \"" + String(headphones) + "\"").c_str() : "any very close device");
  player.setHold(&outputHold);  // a play waits while the headphones aren't connected (PlayGate)
  danceMode.begin();
  diag::logHeap("dance");
  haptics.begin();
  input.begin();
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
  diag::logHeap("ui");
  Serial.println("[console] n/p next/prev, space play/pause, o output, +/- volume, s stats, l list, "
                 "f forget bt, z silent test mode, d dance tab, m next dancer, x/X screenshot dancer/screen, v beat log; "
                 "with Enter: i<n> play, b<n> bench, c<name> headphones name, h<n> bt headroom -n dB, "
                 "q queue (q? for its commands), a touch calibration (a5-a9 fewer crosshairs, ac check, as status, "
                 "ad default table, ah0/1 haptics, ar0/1 rail ticks), "
                 "t<bpm> tempo prior (t clears), y<ms> dance latency offset, k<n> freeze pose 0-15 (k unfreezes); "
                 "ui the UI's navigation (ui0-ui4 tab, uib back, uic coach cards, uit/uih/uis/uid/uip scripted finger, "
                 "uiF<c/s/p/l/n/w> show a faked Bluetooth or no-card state (uiF0 the real one), uiV the volume HUD, uil<n> a synthetic "
                 "library of n tracks in the Library tab, uil0 the card's); "
                 "UI spike (with Enter): u input lab (u0-u3, us summary), w scroll lab (w0 interactive, w1-w3 stress, wm0/wm1 redraw/hw scroll, wp refill pacing), "
                 "g library index (g0 SD card, g<n> synthetic), e font probe (e1-e5), j thumbnail probe (j<n>, jw, ja)");
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  handleInput(now);
  console.poll();
  handleBluetooth();
  btSession.update(audio.bluetooth().link(), now);
  stepPlayGate(now);
  player.update(now);
  audio.loop(now);
  queueStore.loop(now);
  // What a track's length is, once the backend knows it (read from the
  // file; or its estimate 20 s in, exact for a constant bitrate): the
  // Queue's "49 min"; once per entry (each change redoes the Queue's sum).
  // A skip changes the entry at once, but the backend starts it a moment
  // later: until then the position and length are the last track's. So a
  // length is noted only once this entry has started: a start since the
  // change, the position gone back, or a change right at a track's start.
  if (queue.current() >= 0 && audio.isPlaying()) {
    static uint32_t lengthKey = QueueModel::kNone;
    static uint8_t lengthNoted = 0;  // 1 the estimate, 2 the file's
    static uint32_t startSeqAtKey = 0, posAtKey = 0;
    static bool started = false;
    const uint32_t pos = audio.positionMs();
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
  if (screenTaken() && danceMode.active()) danceMode.setActive(false);
  // The dancer (the Dance tab) draws its own frames (~30/s) into its box.
  danceMode.loop(now, silent);
  // The UI: the tab bar, overlays, the page (list frames on 30 fps deadlines).
  if (userInterface) {
    if (!userInterface->started() && now >= kDiagnosticsScreenMs && !screenTaken()) userInterface->start(now);
    userInterface->loop(now);
  }
  shot.poll();

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
  // while a list frame is due soon.
  delay(userInterface ? userInterface->idleMs(millis()) : 5);
}
