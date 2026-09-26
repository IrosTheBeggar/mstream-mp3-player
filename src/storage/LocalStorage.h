#pragma once
#include <FS.h>

#include "hal/IStorage.h"

// The player's local library: the SD card if one is inserted, otherwise the
// LittleFS partition on internal flash (test audio flashed with
// `pio run -e core2 -t uploadfs`). Tracks are the .mp3/.flac files under
// /music, in path order.
class LocalStorage : public IStorage {
public:
  bool begin() override;
  std::vector<Track> listTracks() override;
  bool available() const override { return fs_ != nullptr; }

  // Walks /music (no cap, up to `maxDepth` folders deep) and calls
  // fn(path, ctx) for every file, in the file system's order. Nothing is
  // kept: the caller decides what to store (the library index keeps its
  // own copy in PSRAM). Returns the number of files seen.
  uint32_t forEachFile(void (*fn)(const char* path, void* ctx), void* ctx, int maxDepth = 8);

  // The mounted filesystem; only valid when available().
  fs::FS& fs() { return *fs_; }
  // "SD", "flash" or "none", for the diagnostics screen.
  const char* name() const { return name_; }

private:
  fs::FS* fs_ = nullptr;
  const char* name_ = "none";
};
