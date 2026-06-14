#include "storage/SdStorage.h"

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

#include "Pins.h"

namespace {
bool hasAudioExtension(const String& name) {
  String n = name;
  n.toLowerCase();
  return n.endsWith(".mp3") || n.endsWith(".flac") || n.endsWith(".wav") ||
         n.endsWith(".aac") || n.endsWith(".m4a") || n.endsWith(".ogg");
}
}  // namespace

bool SdStorage::begin() {
  // Display and SD share the SPI bus; the display owns SPI.begin(), so here we
  // just bring up SD on the same pins with its own chip-select.
  SPI.begin(PIN_SPI_SCLK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_SD_CS);
  mounted_ = SD.begin(PIN_SD_CS, SPI);
  releasedToHost_ = false;
  if (!mounted_) {
    Serial.println("[SD] mount failed — falling back to demo library");
  }
  return mounted_;
}

std::vector<Track> SdStorage::listTracks() {
  std::vector<Track> tracks;
  if (!available()) return tracks;

  File root = SD.open("/");
  if (!root) return tracks;

  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    if (!f.isDirectory()) {
      String path = f.name();
      if (path.length() && path[0] != '/') path = "/" + path;
      if (hasAudioExtension(path)) {
        Track t{};  // value-init so durationMs is 0 before we fill fields
        t.path = path.c_str();
        // Title = filename without extension; real metadata parsing comes later.
        int slash = path.lastIndexOf('/');
        int dot = path.lastIndexOf('.');
        String title = path.substring(slash + 1, dot < 0 ? path.length() : dot);
        t.title = title.c_str();
        t.artist = "Unknown Artist";
        t.durationMs = 0;  // unknown until decoded; sim falls back to a default
        tracks.push_back(t);
      }
    }
    f.close();
  }
  root.close();
  return tracks;
}

void SdStorage::releaseToHost() {
  // Unmount so the host gets exclusive access during the USB-MSC handoff.
  SD.end();
  releasedToHost_ = true;
  Serial.println("[SD] released to host (USB-MSC)");
}

void SdStorage::reclaim() {
  releasedToHost_ = false;
  mounted_ = SD.begin(PIN_SD_CS, SPI);
  Serial.printf("[SD] reclaimed (mounted=%d)\n", mounted_);
}
