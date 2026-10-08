// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <FS.h>

#include "CardFormat.h"
#include "hal/IStorage.h"

// The player's local storage: the SD card if one is inserted, otherwise the
// LittleFS partition on internal flash (test audio flashed with
// `pio run -e core2 -t uploadfs`). The music is the .mp3/.flac/.opus files
// under /music, read into the library index (app/Library); the player's own files
// (the index's cache, the queue) are in /.player.
//
// On the card, begin() also gives the card's FatFs drive to storage/CardFat
// (the validation walk's lister and the device's records go through FatFs
// itself: docs/METADATA.md 3.8) and puts the PSRAM sector cache under it
// (storage/SectorDisk, 3.2.4), right after the mount and before the audio
// starts. forEachFile() stays for the flash, the boot's walk of a card with
// no records, and the console's bench.
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
  // False: cardKind() read again.
  bool probeCard();
  // When the card didn't mount, what its first sectors say it is
  // (cardformat: read at begin() and at each failed probeCard()): the
  // empty state's message (uitext::cardMessage(): "No microSD card", "This
  // card is exFAT", ... "Can't read this card").
  cardformat::Kind cardKind() const { return cardKind_; }
  // The storage's size in bytes (0: none), for About: the card's (its
  // CSD's sector count, kept since the mount) or the flash partition's.
  // Never reads the card or takes its lock (not SD.totalBytes(), a hidden
  // free-space count: LocalStorage.cpp); there is no free space here.
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
  cardformat::Kind cardKind_ = cardformat::Kind::Unreadable;

  // SD.begin() failed: the card's sectors 0 (and its type 0x07 partition's
  // first), read raw, into cardKind_.
  void lookAtCard(int cs);
};
