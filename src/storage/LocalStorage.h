// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <FS.h>

#include "hal/IStorage.h"

// The player's local storage: the SD card if one is inserted, otherwise the
// LittleFS partition on internal flash (test audio flashed with
// `pio run -e core2 -t uploadfs`). The music is the .mp3/.flac files under
// /music, read into the library index (app/Library); the player's own files
// (the index's cache, the queue) are in /.player.
class LocalStorage : public IStorage {
public:
  bool begin() override;
  bool available() const override { return fs_ != nullptr; }

  // Walks /music (up to `maxDepth` folders deep) and calls fn(path, ctx) for
  // every file, in the file system's order; hidden names (".Trashes",
  // "._x.mp3") are skipped. Nothing is kept: the caller decides what to
  // store. It reads the directories through the VFS (readdir), not
  // Arduino's File, which opens and stats every entry (a directory search
  // per file: ~5.7 ms a file in the UI spike). Returns the files seen.
  uint32_t forEachFile(void (*fn)(const char* path, void* ctx), void* ctx, int maxDepth = 8);

  // The folder for the player's own files, created if missing ("/.player").
  const char* stateDir();

  // The mounted filesystem; only valid when available().
  fs::FS& fs() { return *fs_; }
  // "SD", "flash" or "none", for the diagnostics screen.
  const char* name() const { return name_; }
  // The microSD card is what's mounted (not the flash fallback).
  bool onCard() const { return name_[0] == 'S'; }  // "SD"
  // No card at boot: is one there now? (SD.begin() again: the UI's "Try
  // again". The firmware restarts to use it: the audio backend, the
  // library and the queue were all set up on the flash.) Loop task, never
  // inside an LcdLock (the card shares the LCD's bus and its lock).
  bool probeCard();
  // The mounted volume's size in bytes (0: none), for About.
  uint64_t totalBytes() const;
  // The VFS mount point ("/sd", "/littlefs"): a path the fs sees as
  // "/music/x" is "<vfsRoot>/music/x" to POSIX calls (open, stat, opendir),
  // which other tasks use (the thumbnail worker) without Arduino's File.
  const char* vfsRoot() const { return mount_; }

private:
  fs::FS* fs_ = nullptr;
  const char* name_ = "none";
  const char* mount_ = "";  // the VFS mount point: "/sd", "/littlefs"
  bool stateDirMade_ = false;
};
