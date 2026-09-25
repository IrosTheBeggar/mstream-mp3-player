// mstream-mp3-player — firmware entry point (M5Stack Core2).
//
// Bring-up build: starts the board, the local library and the Bluetooth stack,
// and shows what it found on screen and over serial. Playback comes next.

#include <Arduino.h>
#include <BluetoothA2DPSource.h>
#include <M5Unified.h>
#include <esp_chip_info.h>

#include <atomic>
#include <vector>

// Not used yet: linked so this build proves the decoders build alongside
// M5Unified and ESP32-A2DP.
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorMP3.h>

#include "Track.h"
#include "app/Diagnostics.h"
#include "storage/LocalStorage.h"
#include "ui/DisplayView.h"

static LocalStorage storage;
static DisplayView view;
static BluetoothA2DPSource a2dp;
static AudioGeneratorMP3 mp3;
static AudioGeneratorFLAC flac;

static std::vector<Track> tracks;
static std::atomic<int> btDevicesSeen{0};

// For now Bluetooth only lists the audio devices it can see (never connects),
// so you can find your headphones' name for local.ini.
static bool onBtDeviceFound(const char* name, esp_bd_addr_t, int rssi) {
  btDevicesSeen++;
  Serial.printf("[bt] found \"%s\" rssi=%d\n", name, rssi);
  return false;
}

static int32_t silence(Frame* frames, int32_t count) {
  memset(frames, 0, sizeof(Frame) * count);
  return count;
}

static String kb(uint32_t bytes) { return String(bytes / 1024) + "K"; }

static std::vector<DisplayView::Row> diagnosticsRows() {
  esp_chip_info_t chip;
  esp_chip_info(&chip);
  const diag::Heap h = diag::heap();
  const int battery = M5.Power.getBatteryLevel();
  const bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging;

  std::vector<DisplayView::Row> rows;
  rows.push_back({"Board", diag::boardName()});
  rows.push_back({"Power chip", diag::pmicName()});
  rows.push_back({"IMU", diag::imuName()});
  rows.push_back({"Chip", String(ESP.getChipModel()) + " rev " + (chip.revision / 100) + "." +
                              (chip.revision % 100)});
  rows.push_back({"Flash", String(ESP.getFlashChipSize() / (1024 * 1024)) + " MB"});
  rows.push_back({"PSRAM", kb(ESP.getPsramSize()) + " (" + kb(h.psramFree) + " free)"});
  rows.push_back({"Last reset", diag::resetReason()});
  rows.push_back({"Battery", String(battery) + "%" + (charging ? " charging" : "")});
  rows.push_back({"Library", String(storage.name()) + ", " + tracks.size() + " tracks"});
  rows.push_back({"Bluetooth", String("scanning, ") + btDevicesSeen.load() + " seen"});
  rows.push_back({"RAM free", kb(h.internalFree) + " (min " + kb(h.internalMin) + ", block " +
                                  kb(h.internalLargest) + ")"});
  rows.push_back({"Uptime", String(millis() / 1000) + " s"});
  return rows;
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;  // M5Unified leaves Serial off unless asked
  cfg.internal_mic = false;      // the mic shares GPIO0 with the speaker's I2S clock
  M5.begin(cfg);
  Serial.println("\nmstream-mp3-player bring-up");
  diag::logHeap("boot");

  view.begin();

  storage.begin();
  tracks = storage.listTracks();
  Serial.printf("[lib] %s: %u tracks\n", storage.name(), (unsigned)tracks.size());
  for (const Track& t : tracks) Serial.printf("[lib]   %s\n", t.path.c_str());
  diag::logHeap("storage");

  a2dp.set_local_name("mStream Player");
  a2dp.set_ssid_callback(onBtDeviceFound);
  a2dp.set_data_callback_in_frames(silence);
  a2dp.start();
  diag::logHeap("bt");

  for (const auto& row : diagnosticsRows()) {
    Serial.printf("[diag] %-10s %s\n", row.label.c_str(), row.value.c_str());
  }
}

void loop() {
  M5.update();

  const uint32_t now = millis();
  static uint32_t lastDraw = 0;
  static uint32_t lastLog = 0;
  if (now - lastDraw >= 1000) {
    view.showDiagnostics(diagnosticsRows());
    lastDraw = now;
  }
  if (now - lastLog >= 10000) {
    diag::logHeap("steady");
    lastLog = now;
  }
  delay(20);
}
