#include "storage/LocalStorage.h"

#include <LittleFS.h>
#include <M5Unified.h>
#include <SD.h>
#include <SPI.h>

#include <algorithm>  // std::sort

namespace {
constexpr const char* kMusicDir = "/music";
constexpr size_t kMaxTracks = 200;  // track paths live in internal RAM
constexpr int kMaxDepth = 4;

bool isAudioFile(const String& path) {
  String p = path;
  p.toLowerCase();
  return p.endsWith(".mp3") || p.endsWith(".flac");
}

void scan(fs::FS& fs, const String& dir, int depth, std::vector<Track>& out) {
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return;
  for (File f = d.openNextFile(); f && out.size() < kMaxTracks; f = d.openNextFile()) {
    const String path = f.path();
    if (f.isDirectory()) {
      if (depth < kMaxDepth) scan(fs, path, depth + 1, out);
    } else if (isAudioFile(path)) {
      Track t{};  // value-init so durationMs is 0 (unknown)
      t.path = path.c_str();
      // Title = filename without extension until tags are read at play time.
      const int slash = path.lastIndexOf('/');
      const int dot = path.lastIndexOf('.');
      t.title = path.substring(slash + 1, dot < 0 ? path.length() : dot).c_str();
      out.push_back(t);
    }
    f.close();
  }
}
uint32_t walk(fs::FS& fs, const char* dir, int depth, int maxDepth, void (*fn)(const char*, void*), void* ctx) {
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return 0;
  uint32_t n = 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    // A copy of the path: the File's buffer goes with it, and recursion opens more.
    char path[256];
    strlcpy(path, f.path(), sizeof(path));
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) {
      if (depth < maxDepth) n += walk(fs, path, depth + 1, maxDepth, fn, ctx);
    } else {
      fn(path, ctx);
      ++n;
    }
  }
  return n;
}
}  // namespace

uint32_t LocalStorage::forEachFile(void (*fn)(const char* path, void* ctx), void* ctx, int maxDepth) {
  if (!available()) return 0;
  return walk(*fs_, kMusicDir, 0, maxDepth, fn, ctx);
}

bool LocalStorage::begin() {
  // The SD card shares the LCD's SPI bus; M5Unified knows the pins.
  const int cs = M5.getPin(m5::pin_name_t::sd_spi_cs);
  SPI.begin(M5.getPin(m5::pin_name_t::sd_spi_sclk), M5.getPin(m5::pin_name_t::sd_spi_miso),
            M5.getPin(m5::pin_name_t::sd_spi_mosi), cs);
  if (SD.begin(cs, SPI, 25000000)) {
    fs_ = &SD;
    name_ = "SD";
  } else if (LittleFS.begin(true /* format the partition if it has never been used */)) {
    fs_ = &LittleFS;
    name_ = "flash";
  }
  return available();
}

std::vector<Track> LocalStorage::listTracks() {
  std::vector<Track> tracks;
  if (!available()) return tracks;
  scan(*fs_, kMusicDir, 0, tracks);
  std::sort(tracks.begin(), tracks.end(),
            [](const Track& a, const Track& b) { return a.path < b.path; });
  return tracks;
}
