// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <atomic>
#include <cstdint>

#include "ThumbCache.h"
#include "ThumbScaler.h"

class Library;
class CardWorker;

// Album covers as thumbnails for the UI: 40 x 40 in the list rows, 96 x 96
// on Now Playing. The pages ask get() while they draw; a miss draws the
// placeholder and asks, and when the thumbnail arrives the Ui has the page
// redraw just what shows that album (a row, the cover).
//
// Where they come from, first that works (docs/METADATA.md 2.14.3):
//   1. the PSRAM LRU (ThumbCache: 64 small, 6 large; ~315 KB);
//   2. the transfer's thumbnail, /.mstream/thumbs/<h>/<8 HEX>.565 (MPTH v1,
//      keyed by the album folder's path hash: 2.14.1), when the index says
//      the album has one (LibraryIndex::kTransferThumb: T flags its folder
//      THUMB and no image the listener added beats it); read only, never
//      written: a 3.2 KB or 18 KB read, no decode;
//   3. the card's copy of the device's own, /.player/thumbs/<h>/<hash>.565
//      (thumbfile): a 3.2 KB or 18 KB read, what the next boot finds;
//   4. the cover image the library index picked for the album's folder
//      (cover.jpg, folder.jpg, front.jpg, else the largest .jpg there),
//      decoded once into both sizes (ThumbScaler, box-filtered), then saved
//      as 3. The JPEG is streamed (3.5, "Covers stream"): its header walked
//      at offsets (jpeg::parseFile()), TJpgDec's input fed from the file in
//      16 KB pieces, never the whole file in PSRAM (it read up to 2 MB
//      whole before). A progressive JPEG (TJpgDec reads baseline only) or a
//      damaged one keeps the placeholder, is logged once, and gets a
//      header-only file so it isn't tried again at the next boot either.
//
// The decode takes 125-300 ms (the UI spike), far too long for the loop
// task, and TJpgDec can't stop half-way. So a cover is a step of the one
// card worker (app/CardWorker, shared with the card's walk and scan:
// METADATA.md 3.3.4), on core 1 below the audio decoder; the loop's
// scheduler (app/CardTasks, ScanScheduler) hands it one when a row asked
// (wantsCover()) and no list moves, at the loop's priority (1), dropped to
// 0 while a list moves under it, so no frame waits for it. Its card reads
// are 4 KB at most (the SPI bus and the FAT lock are the decoder's too).
// The rows that stay on screen when a list stops are asked last, so they
// come first.
//
// Its RAM: the worker's 6 KB stack in internal RAM (CardWorker); TJpgDec's
// 3.9 KB work pool in PSRAM, not in internal RAM as M5GFX's drawJpg()
// allocates it: the step drives lgfx_tjpgd itself, and reads the card with
// POSIX calls (no stdio buffer); everything else in PSRAM: the input
// buffer (16 KB), the scaler's sums (~170 KB while decoding), the job.
//
// Loop task only, except the step (run() and what it calls), which
// touches only the job it was handed.
namespace ui {

class Thumbs {
public:
  using Size = ThumbCache::Size;
  static constexpr uint32_t kSmallSlots = 64;
  static constexpr uint32_t kLargeSlots = 6;
  // The JPEG's input, read from the file a piece at a time.
  static constexpr uint32_t kInputBytes = 16 * 1024;

  explicit Thumbs(Library& library) : library_(library) {}

  // The pools and the job's buffers (PSRAM). False: no memory (every
  // cover is then the placeholder).
  bool begin();
  // Whether `album` (of the library's index) has a cover at all (an image,
  // or the transfer's thumbnail): without one it's the placeholder, and
  // nothing is asked.
  bool hasCover(uint32_t album) const;
  // `album`'s thumbnail at `s` (big-endian RGB565, px x px), or nullptr:
  // the placeholder for now (it has been asked for).
  const uint16_t* get(uint32_t album, Size s);
  // Every loop pass: takes in what the worker made. Returns the album whose
  // thumbnail just arrived, or kNone.
  uint32_t loop(uint32_t nowMs);
  // A row asked for a cover and none is being made (ScanScheduler's
  // In::cover).
  bool wantsCover(uint32_t nowMs) const;
  // The newest request as the worker's step, at `priority`. False: nothing
  // asked, or the worker couldn't take it (asked again when a row draws).
  bool startCover(CardWorker& worker, uint8_t priority, uint32_t nowMs);
  // The library was rebuilt: album ids mean other albums.
  void libraryChanged();
  // The library's update step (docs/METADATA.md 3.4.2) borrows the pools
  // for its build (about 315 KB: ThumbCache::release()): every cover is the
  // placeholder and nothing is asked meanwhile; restore() makes them again,
  // empty (false: no PSRAM for them now: the placeholders stay, and the
  // next update step tries again). The job's own buffers stay.
  void lend();
  bool restore();
  bool lent() const { return lent_; }
  // What lend() gives back (the update step's memory check counts it).
  size_t poolBytes() const { return lent_ ? 0 : cache_.bytes(); }
  // Console uiT: every cover is decoded again this session, not read from
  // its card copy (which is rewritten): the decode's timings on demand.
  void redecode();
  // "[thumb] ..." lines for 'ui'.
  void printState() const;

  static constexpr uint32_t kNone = ThumbCache::kNone;
  enum State : uint8_t { Idle, Queued, Working, Done };
  struct Job;  // Thumbs.cpp: the one in flight, shared with the worker

private:
  static void stepEntry(void* self);
  bool prepare(uint32_t album, uint8_t sizes);
  void finish(uint32_t* arrived);

  Library& library_;
  ThumbCache cache_;
  Job* job_ = nullptr;          // PSRAM
  bool ready_ = false;
  bool lent_ = false;           // lend(): the pools are the library update's
  uint32_t generation_ = 0;     // bumped by libraryChanged(): a job from before is dropped
  uint32_t retryAtMs_ = 0;      // the worker couldn't be made: try again then
  bool skipCard_ = false;       // redecode()
  // For the report.
  uint32_t decoded_ = 0, fromCard_ = 0, fromTransfer_ = 0, failures_ = 0, progressive_ = 0;
  uint32_t decodeMsSum_ = 0, decodeMsMax_ = 0;
  uint32_t internalMin_ = UINT32_MAX;
};

}  // namespace ui
