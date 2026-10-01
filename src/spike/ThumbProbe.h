// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>
#include <FS.h>
#include <M5GFX.h>

#include "LibraryIndex.h"
#include "audio/Core2AudioBackend.h"
#include "ui/LcdLock.h"

// UI spike: the thumbnail probe (console j, docs/UI-SPIKE.md). How much an
// album cover costs: an album folder's cover.jpg (~500x500 from the SD card)
// decoded with M5GFX's drawJpg (TJpgDec) into 40x40 and 80x80 RGB565 PSRAM
// sprites, the internal RAM the decoder takes while it runs (its work area is
// malloc'd, so under 4 KB it lands in internal RAM), then the thumbnail
// cached in PSRAM and redrawn from there, and optionally written as a raw
// .565 file and redrawn from the card.
//
//   j      the first album (albums A-Z) with a cover: every measurement, and
//          the thumbnails on screen
//   j<n>   album n (in albums A-Z order)
//   jw<n>  the same, plus the .565 cache files on the SD card
//          (/uispike/<album>_40.565 and _80.565; delete the folder when done)
//   ja     decode every album's cover to 40x40 (timing percentiles only)
//   jq     close (j alone again also closes)
//
// The internal RAM figure comes from sampling the free internal heap at each
// read the decoder makes (the work area is allocated before the first read
// and freed after the last), so it is the decoder's peak, JPEG pool plus
// anything the file system allocates, to within what other tasks do meanwhile.
//
// The SD card's side is measured too: every file access goes through
// f.read/f.write calls of at most kChunk bytes (FatFs holds the volume, and
// the SD driver the SPI bus the LCD shares, for a whole call: one read of a
// 150 KB cover would lock the decoder's reads out for 100 ms or more), each
// call is timed, and the ring fill (its minimum) and the underrun count are
// sampled across each step. A [thumb] "SD side" line per step gives the
// longest single call, the ring minimum and the underruns.
class ThumbProbe {
public:
  static constexpr uint32_t kChunk = 4096;

  explicit ThumbProbe(Core2AudioBackend* audio = nullptr) : audio_(audio) {}
  ~ThumbProbe();
  void command(const char* arg, fs::FS* fs, const LibraryIndex* index);
  bool active() const { return active_; }
  void close();

private:
  struct Jpeg {
    uint32_t bytes = 0;
    int width = 0, height = 0;
    bool progressive = false;
  };
  struct Decode {
    bool ok = false;
    float ms = 0;
    uint32_t internalPeak = 0;  // free before - lowest free during
    uint32_t reads = 0;
    uint32_t maxReadUs = 0;     // the longest single file read (0: from memory)
  };

public:
  // The SD card and the audio across one step of the probe.
  struct SdWatch {
    Core2AudioBackend* audio = nullptr;
    uint32_t ringMin = UINT32_MAX;
    uint32_t underruns0 = 0;
    uint32_t calls = 0;
    uint32_t maxCallUs = 0;
    void start(Core2AudioBackend* a);
    void sampleRing();
    void call(uint32_t us);
    uint32_t underruns() const;
    void log(const char* step) const;
  };

private:
  bool ensure();
  bool coverPath(fs::FS& fs, const LibraryIndex* index, uint32_t album, char* buf, size_t size) const;
  static bool parseJpeg(const uint8_t* data, uint32_t len, Jpeg* out);
  Decode decodeFromMemory(M5Canvas& dst, int size, const uint8_t* data, uint32_t len, const Jpeg& j);
  Decode decodeFromFile(M5Canvas& dst, int size, fs::FS& fs, const char* path, const Jpeg& j, SdWatch* watch);
  // f.read/f.write in calls of at most kChunk bytes, each timed into `watch`.
  static uint32_t readChunked(fs::File& f, uint8_t* dst, uint32_t len, SdWatch* watch);
  static uint32_t writeChunked(fs::File& f, const uint8_t* src, uint32_t len, SdWatch* watch);
  void probe(fs::FS& fs, const LibraryIndex* index, uint32_t album, bool write);
  void probeAll(fs::FS& fs, const LibraryIndex* index);

  Core2AudioBackend* audio_;
  bool active_ = false;
  bool ready_ = false;
  M5Canvas thumb40_, thumb80_;
  uint16_t* cache40_ = nullptr;  // PSRAM copies (the thumbnail cache)
  uint16_t* cache80_ = nullptr;
  SpiHoldStats lock_;
};
