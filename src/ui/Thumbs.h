// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

#include "ThumbCache.h"
#include "ThumbScaler.h"

class Library;

// Album covers as thumbnails for the UI: 40 x 40 in the list rows, 96 x 96
// on Now Playing. The pages ask get() while they draw; a miss draws the
// placeholder and asks, and when the thumbnail arrives the Ui has the page
// redraw just what shows that album (a row, the cover).
//
// Where they come from, first that works:
//   1. the PSRAM LRU (ThumbCache: 64 small, 6 large; ~315 KB);
//   2. the card's copy, /.player/thumbs/<h>/<hash>.565 (thumbfile): a
//      3.2 KB or 18 KB read, what the next boot finds;
//   3. the cover image the library index picked for the album's folder
//      (cover.jpg, folder.jpg, front.jpg, else the largest .jpg there),
//      decoded once into both sizes (ThumbScaler, box-filtered), then saved
//      as 2. A progressive JPEG (TJpgDec reads baseline only) or a damaged
//      one keeps the placeholder, is logged once, and gets a header-only
//      file so it isn't tried again at the next boot either.
//
// The decode takes 125-300 ms (the UI spike), far too long for the loop
// task: a list would stop answering the finger. TJpgDec has no way to stop
// half-way and carry on later (its entropy decoding runs through the whole
// file in one call), so "a slice of work per idle loop pass" isn't
// possible without a coroutine. So it runs on a WORKER TASK on core 1,
// below the audio decoder (2). While nothing moves it runs at the loop's
// priority (1): at priority 0 it shared what the loop and the decoder left
// with the idle task, and on the device a 200 x 200 cover took 0.6-1.5 s
// and the Discovery cover 2.2 s of decoding while an MP3 played. The
// moment a list moves (a job is never started then, but one may be under
// way) it drops to priority 0, below the loop, so no frame waits for it.
// Its card reads are 4 KB each (the SPI bus and the
// FAT lock are the decoder's too; priority inheritance lifts it while it
// holds them, ~3 ms). And no new job starts while a list is moving: the
// rows that stay on screen when it stops are asked last, so they come
// first.
//
// Its RAM, against the ~58-63 KB of internal RAM free while playing:
//   - a 6 KB stack in internal RAM (a task that reads the card or the
//     flash can't have its stack in PSRAM), only while it lives: it is
//     made for the first job and ends itself after 3 s without one (the
//     'ui' report prints its stack's high-water mark);
//   - TJpgDec's 3.9 KB work pool in PSRAM, not in internal RAM as M5GFX's
//     drawJpg() allocates it (that and Arduino's File buffers were the
//     spike's 8.6 KB): the worker drives lgfx_tjpgd itself, and reads the
//     card with POSIX calls (no stdio buffer); so the transient left is the
//     file system's own (the report logs the lowest internal free seen
//     during each job);
//   - everything else in PSRAM: the JPEG file (read whole, up to 2 MB),
//     the scaler's sums (~170 KB while decoding), the job.
//
// Loop task only, except the worker's side (run() and what it calls),
// which touches only the job it was handed.
namespace ui {

class Thumbs {
public:
  using Size = ThumbCache::Size;
  static constexpr uint32_t kSmallSlots = 64;
  static constexpr uint32_t kLargeSlots = 6;
  static constexpr uint32_t kStackBytes = 6144;
  // 3 s, not 10: its 6 KB stack alive while the queue was saved made the
  // device's lowest internal RAM free 50 KB in the soak.
  static constexpr uint32_t kIdleExitMs = 3000;
  // The worker's priority for a job: the loop's (1). The audio decoder (2)
  // stays above it; a list that starts moving drops it to 0 (see the top).
  static constexpr UBaseType_t kWorkPriority = 1;
  static constexpr uint32_t kMaxJpegBytes = 2u << 20;

  explicit Thumbs(Library& library) : library_(library) {}

  // The pools and the worker's buffers (PSRAM). False: no memory (every
  // cover is then the placeholder).
  bool begin();
  // Whether `album` (of the library's index) has a cover image at all:
  // without one it's the placeholder, and nothing is asked.
  bool hasCover(uint32_t album) const;
  // `album`'s thumbnail at `s` (big-endian RGB565, px x px), or nullptr:
  // the placeholder for now (it has been asked for).
  const uint16_t* get(uint32_t album, Size s);
  // Every loop pass: takes in what the worker made, hands it the next
  // request (unless `busy`: a list is moving). Returns the album whose
  // thumbnail just arrived, or kNone.
  uint32_t loop(uint32_t nowMs, bool busy);
  // The library was rebuilt: album ids mean other albums.
  void libraryChanged();
  // Console uiT: every cover is decoded again this session, not read from
  // its card copy (which is rewritten): the decode's timings on demand.
  void redecode();
  // "[thumb] ..." lines for 'ui'.
  void printState() const;

  static constexpr uint32_t kNone = ThumbCache::kNone;
  enum State : uint8_t { Idle, Queued, Working, Done, Exit };
  struct Job;  // Thumbs.cpp: the one in flight, shared with the worker

private:

  static void taskEntry(void* self);
  void work();
  bool prepare(uint32_t album, uint8_t sizes);
  void finish(uint32_t* arrived);
  void setWorkerPriority(UBaseType_t prio);

  Library& library_;
  ThumbCache cache_;
  Job* job_ = nullptr;          // PSRAM
  TaskHandle_t task_ = nullptr;
  std::atomic<bool> taskAlive_{false};
  bool ready_ = false;
  uint32_t generation_ = 0;     // bumped by libraryChanged(): a job from before is dropped
  uint32_t lastJobMs_ = 0;
  uint32_t retryAtMs_ = 0;
  bool skipCard_ = false;       // redecode()
  UBaseType_t workerPrio_ = 0;  // what it runs at now      // the worker couldn't be made: try again then
  // For the report.
  uint32_t decoded_ = 0, fromCard_ = 0, failures_ = 0, progressive_ = 0;
  uint32_t decodeMsSum_ = 0, decodeMsMax_ = 0;
  uint32_t internalMin_ = UINT32_MAX;
  uint32_t stackLeft_ = 0;
};

}  // namespace ui
