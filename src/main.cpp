// mstream-mp3-player — firmware entry point (M5Stack Core2).
//
// Wires the portable core (PlaybackController) to the Core2 audio backend,
// storage, screen, touch buttons and a serial console. The playlist is the
// library's files followed by the built-in test tones.

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_chip_info.h>

#include <cmath>
#include <vector>

#include "PlaybackController.h"
#include "Track.h"
#include "app/Diagnostics.h"
#include "app/SerialConsole.h"
#include "audio/Core2AudioBackend.h"
#include "storage/LocalStorage.h"
#include "ui/DisplayView.h"

// Headphones to connect to; set in a gitignored local.ini (see platformio.ini).
#ifndef BT_SINK_NAME
#define BT_SINK_NAME ""
#endif

using Output = Core2AudioBackend::Output;

static LocalStorage storage;
static DisplayView view;
static Core2AudioBackend audio;
static PlaybackController player(audio);
static std::vector<Track> library;  // files found in storage

static constexpr uint32_t kDiagnosticsScreenMs = 3000;
// Volume keys of headphones without absolute volume (AVRCP passthrough):
// about 1/16 of the range per press, like a phone.
static constexpr int kHeadphoneVolumeStep = 6;

static std::vector<Track> builtInTones() {
  return {
      {"tone:440", "Test tone 440 Hz", "built-in", 0},
      {"tone:1000", "Test tone 1 kHz", "built-in", 0},
      {"tone:left", "Left ear only", "built-in", 0},
  };
}

static String kb(uint32_t bytes) { return String(bytes / 1024) + "K"; }

static String mmss(uint32_t ms) {
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
  return buf;
}

static std::vector<DisplayView::Row> diagnosticsRows() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  const diag::Heap h = diag::heap();
  std::vector<DisplayView::Row> rows;
  rows.push_back({"Board", diag::boardName()});
  rows.push_back({"Power chip", diag::pmicName()});
  rows.push_back({"IMU", diag::imuName()});
  rows.push_back({"Chip", String(ESP.getChipModel()) + " rev " + (chip.revision / 100) + "." +
                              (chip.revision % 100)});
  rows.push_back({"Flash", String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB"});
  rows.push_back({"PSRAM", kb(ESP.getPsramSize()) + " (" + kb(h.psramFree) + " free)"});
  rows.push_back({"Last reset", diag::resetReason()});
  rows.push_back({"Battery", String(M5.Power.getBatteryLevel()) + "%"});
  rows.push_back({"Library", String(storage.name()) + ", " + library.size() + " tracks"});
  rows.push_back({"RAM free", kb(h.internalFree) + " (min " + kb(h.internalMin) + ", block " +
                                  kb(h.internalLargest) + ")"});
  return rows;
}

// ---- actions shared by the touch buttons and the serial console ----

static void toggleOutput() {
  audio.setOutput(audio.output() == Output::Speaker ? Output::Bluetooth : Output::Speaker);
}

static bool headphonesSetVolume() {
  return audio.output() == Output::Bluetooth && audio.bluetooth().headphonesControlVolume();
}

// The active output's volume (the speaker and Bluetooth each keep their own).
// Bluetooth applies the step on its own task, so the value logged is where
// it should land (the screen shows the result at its next redraw).
static void stepVolume(int delta) {
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

// Play and pause as commands, not a toggle: in-ear detection sends them too,
// and a repeat must not undo the first. PLAY only resumes what was paused:
// putting a bud back in (or fiddling with one) must never start music the
// listener didn't have playing.
static void resumeIfPaused() {
  if (player.state() == PlayState::Paused) player.togglePlayPause();
}

static void pauseIfPlaying() {
  if (player.state() == PlayState::Playing) player.togglePlayPause();
}

static const char* stateName() {
  if (audio.failed()) return "failed";
  switch (player.state()) {
    case PlayState::Playing: return "playing";
    case PlayState::Paused: return "paused";
    default: return "stopped";
  }
}

static void printStats() {
  const auto& s = audio.stats();
  const diag::Heap h = diag::heap();
  Serial.printf(
      "[stats] track=%d %s pos=%.1fs out=%s%s buf=%lums underruns=%lu bt=%lufps load=%.1f%% "
      "stack_free=%lu ram=%luK min=%luK psram=%luK bat=%d%%\n",
      player.currentIndex(), stateName(), audio.positionMs() / 1000.0f,
      audio.output() == Output::Bluetooth ? "bt" : "speaker",
      audio.output() == Output::Bluetooth ? (audio.bluetooth().connected() ? "(connected)" : "(searching)")
                                          : "",
      (unsigned long)s.bufferedMs, (unsigned long)s.underruns, (unsigned long)s.btFramesPerSec,
      s.decodeLoad * 100.0f, (unsigned long)s.decodeStackFree, (unsigned long)(h.internalFree / 1024),
      (unsigned long)(h.internalMin / 1024), (unsigned long)(h.psramFree / 1024),
      (int)M5.Power.getBatteryLevel());

  BtSink& bt = audio.bluetooth();
  if (!bt.connected() && audio.output() != Output::Bluetooth) return;
  // vol/control: the Bluetooth volume and who applies it (headphones = AVRCP
  // absolute volume, asking = waiting for them to accept it, software = the
  // Core2). headphones: their last reported volume. gain: the Core2's gain
  // stage (-2.0dB with absolute volume). gap: longest wait between two data
  // callbacks since the last line (~10-30 ms is healthy).
  const BtSink::Stats b = bt.stats();
  char gain[12] = "mute";
  if (b.gainQ15 > 0) snprintf(gain, sizeof(gain), "%.1fdB", 20.0f * log10f(b.gainQ15 / 32768.0f));
  char headset[8] = "?";
  if (b.headsetVolume >= 0) snprintf(headset, sizeof(headset), "%d", b.headsetVolume);
  Serial.printf("[stats] bt vol=%u%% control=%s headphones=%s/127 gain=%s stream=%s gap=%lums "
                "events_dropped=%lu btapp_stack_free=%lu\n",
                (unsigned)b.volume, b.volumeControl, headset, gain, b.stream,
                (unsigned long)b.maxGapMs, (unsigned long)b.eventsDropped, (unsigned long)b.appTaskStackFree);
}

static void listTracks() {
  const auto& tracks = player.playlist();
  for (size_t i = 0; i < tracks.size(); ++i) {
    Serial.printf("  %u%s %s\n", (unsigned)i, (int)i == player.currentIndex() ? "*" : " ",
                  tracks[i].path.c_str());
  }
}

static SerialConsole console({
    [] { player.next(); },
    [] { player.prev(); },
    [] { player.togglePlayPause(); },
    toggleOutput,
    stepVolume,
    printStats,
    listTracks,
    [](int i) { player.play(static_cast<size_t>(i)); },
    [](int i) {
      const auto& tracks = player.playlist();
      if (i < 0 || static_cast<size_t>(i) >= tracks.size()) return;
      player.stop();
      audio.bench(tracks[i].path);
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
});

static void handleButtons() {
  // The three touch buttons under the screen: click, or hold for half a second.
  if (M5.BtnA.wasClicked()) player.prev();
  if (M5.BtnA.wasHold()) stepVolume(-10);
  if (M5.BtnB.wasClicked()) player.togglePlayPause();
  if (M5.BtnB.wasHold()) toggleOutput();
  if (M5.BtnC.wasClicked()) player.next();
  if (M5.BtnC.wasHold()) stepVolume(+10);
}

// Link changes and headphone buttons, queued by BtSink on the Bluetooth tasks.
static void handleBluetooth() {
  BtSink& bt = audio.bluetooth();
  for (BtSink::Event e = bt.takeEvent(); e != BtSink::Event::None; e = bt.takeEvent()) {
    switch (e) {
      case BtSink::Event::Connected:
        Serial.printf("[bt] connected%s%s\n", bt.deviceName()[0] ? " to " : "", bt.deviceName());
        audio.setOutput(Output::Bluetooth);
        diag::logHeap("bt-link");
        break;
      case BtSink::Event::Disconnected:
        Serial.println("[bt] disconnected");
        // Like a phone: don't carry on through the speaker, pause.
        if (audio.output() == Output::Bluetooth) pauseIfPlaying();
        break;
      case BtSink::Event::Suspended:
        // The headphones stopped the stream themselves: show it as paused;
        // play (here or on them) starts it again.
        if (audio.output() == Output::Bluetooth) pauseIfPlaying();
        break;
      case BtSink::Event::VolumeChanged:
        Serial.printf("[bt] volume now %u%%\n", bt.volume());  // the screen follows at its next redraw
        break;
      case BtSink::Event::Play:
        if (player.state() != PlayState::Paused) {
          Serial.printf("[bt] headphones: play (ignored: %s)\n", stateName());
          break;
        }
        Serial.println("[bt] headphones: play");
        if (bt.connected() && audio.output() != Output::Bluetooth) audio.setOutput(Output::Bluetooth);
        resumeIfPaused();
        break;
      case BtSink::Event::Pause:
        Serial.println("[bt] headphones: pause");
        if (audio.output() != Output::Bluetooth) break;  // not the speaker's playback
        pauseIfPlaying();
        // They pick their next key from the stream: suspend it now, so the
        // next press is PLAY. Also when we were paused already (paused on the
        // Core2, the stream still in its 3 s tail): this press did nothing,
        // the next one plays.
        bt.suspendPromptly();
        break;
      case BtSink::Event::Next:
        Serial.println("[bt] headphones: next");
        player.next();
        break;
      case BtSink::Event::Prev:
        Serial.println("[bt] headphones: previous");
        player.prev();
        break;
      case BtSink::Event::VolumeUp:
        stepBluetoothVolume(+kHeadphoneVolumeStep);
        break;
      case BtSink::Event::VolumeDown:
        stepBluetoothVolume(-kHeadphoneVolumeStep);
        break;
      case BtSink::Event::None:
        break;
    }
  }
}

static void render() {
  DisplayView::NowPlaying np;
  const Track* t = player.currentTrack();
  const auto& tracks = player.playlist();
  np.battery = String(M5.Power.getBatteryLevel()) + "%";
  np.position = "No tracks";
  if (t) {
    np.position = "Track " + String(player.currentIndex() + 1) + " of " + String(tracks.size());
    const std::string tagTitle = audio.trackTitle();
    np.title = tagTitle.empty() ? t->title.c_str() : tagTitle.c_str();
  }
  // "Artist  -  MP3, 44100 Hz": tag artist, else the track's, then what's decoding.
  std::string artist = audio.trackArtist();
  if (artist.empty() && t) artist = t->artist;
  const std::string desc = audio.description();
  np.subtitle = artist.c_str();
  if (!artist.empty() && !desc.empty()) np.subtitle += "  -  ";
  np.subtitle += desc.c_str();

  const String at = mmss(audio.positionMs());
  switch (player.state()) {
    case PlayState::Playing: np.status = "Playing  " + at; break;
    case PlayState::Paused: np.status = "Paused  " + at; break;
    case PlayState::Stopped: np.status = "Stopped"; break;
  }
  if (audio.failed()) np.status = "Can't play";

  np.output = "Speaker";
  if (audio.output() == Output::Bluetooth) {
    BtSink& bt = audio.bluetooth();
    if (!bt.connected()) {
      np.output = "Bluetooth: looking for headphones...";
    } else if (bt.deviceName()[0]) {
      np.output = "Bluetooth: " + String(bt.deviceName());
    } else {
      np.output = "Bluetooth: connected";
    }
  }
  np.volume = "Volume " + String(audio.volume()) + "%";
  if (headphonesSetVolume()) np.volume += " (headphones)";
  np.note = audio.note().c_str();

  const auto& s = audio.stats();
  char stats[80];
  snprintf(stats, sizeof(stats), "buf %lums  underruns %lu  cpu %.1f%%  ram %luK",
           (unsigned long)s.bufferedMs, (unsigned long)s.underruns, s.decodeLoad * 100.0f,
           (unsigned long)(diag::heap().internalFree / 1024));
  np.stats = stats;
  view.showNowPlaying(np);
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;  // M5Unified leaves Serial off unless asked
  cfg.internal_mic = false;      // the mic shares GPIO0 with the speaker's I2S clock
  M5.begin(cfg);
  Serial.println("\nmstream-mp3-player");
  diag::logHeap("boot");

  view.begin();

  storage.begin();
  library = storage.listTracks();
  Serial.printf("[lib] %s: %u tracks\n", storage.name(), (unsigned)library.size());
  diag::logHeap("storage");

  if (!audio.begin(storage.available() ? &storage.fs() : nullptr, BT_SINK_NAME)) {
    Serial.println("[audio] failed to start");
  }
  diag::logHeap("audio");

  std::vector<Track> playlist = library;
  for (const Track& tone : builtInTones()) playlist.push_back(tone);
  player.setPlaylist(playlist);

  const auto rows = diagnosticsRows();
  for (const auto& row : rows) Serial.printf("[diag] %-10s %s\n", row.label.c_str(), row.value.c_str());
  view.showDiagnostics(rows);
  const char* headphones = audio.bluetooth().sinkName();
  Serial.printf("[bt] headphones: %s\n",
                headphones[0] ? ("name contains \"" + String(headphones) + "\"").c_str() : "any very close device");
  Serial.println("[console] n/p next/prev, space play/pause, o output, +/- volume, s stats, l list, "
                 "f forget bt; with Enter: i<n> play, b<n> bench, c<name> headphones name");
}

void loop() {
  M5.update();
  const uint32_t now = millis();

  handleButtons();
  console.poll();
  handleBluetooth();
  player.update(now);
  audio.loop(now);

  static uint32_t lastDraw = 0;
  static uint32_t lastStats = 0;
  static bool heapLoggedWhilePlaying = false;
  if (!heapLoggedWhilePlaying && audio.isPlaying() && audio.positionMs() > 2000) {
    diag::logHeap("playing");
    heapLoggedWhilePlaying = true;
  }
  if (now >= kDiagnosticsScreenMs && now - lastDraw >= 250) {
    render();
    lastDraw = now;
  }
  if (now - lastStats >= 5000) {
    printStats();
    lastStats = now;
  }
  delay(5);
}
