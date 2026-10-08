// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardContract.h"
#include "CardRoot.h"
#include "LibraryBoot.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "TagScan.h"
#include "TagStore.h"
#include "TrackCatalog.h"
#include "storage/CardFat.h"
#include "storage/LocalStorage.h"

// The music library: the single store of what's on the card, a
// LibraryIndex in PSRAM that the queue and the player hold ids into (through
// the TrackCatalog, which adds the built-in tracks).
//
// On the SD card (docs/METADATA.md 3.2.2, 3.4; milestone N10), the boot
// never walks the card to decide:
//   1. the transfer's root (/.mstream/manifest.bin, cardroot::read()) and
//      its tags file's header (T); the device's records (/.player/tags.bin,
//      D: TagStore::open(), its recovery first); library.idx's header
//      (LibraryIndex::peek()), its cut rename settled (2.12.6), and the
//      build-at-boot marker (/.player/build.req);
//   2. libraryboot::decide(): a library.idx built for this card's transfer
//      identity is loaded (no walk, no build); else the index is built
//      from the records (LibraryBuilder over T and D, the journals compacted
//      first); a card with no records at all (a card-reader card, or this
//      firmware's first boot) walks /music into a path-named index, as
//      before;
//   3. a build is saved (library.tmp, then 2.12.6's replace()) with what it
//      was built from (libraryboot::inputsOf()); /.player/device.txt is
//      rewritten when its content would change (2.15).
// The validation walk, the scan and the update step follow in the
// background (app/CardTasks). An index loaded while the scan went on since
// its build (its soft inputs differ) is rebuilt at the scan's end
// (softStale()).
//
// On the flash fallback (no card) nothing of that exists: /music is walked
// at every boot into a path signature, and library.idx is loaded when it
// was saved for it, as before (the test audio of `pio run -t uploadfs`).
class Library {
public:
  explicit Library(LocalStorage& storage);

  // At boot, once storage is up. False: no index (no storage, no PSRAM, or
  // a build that ran out of memory); the built-in tracks still play.
  bool begin();
  // The update step's build (3.4.2), whatever library.idx says: from the
  // records (the journals compacted first), or a walk when there are none
  // (the flash: always the walk); then saved. On the card, a card that
  // doesn't answer (/music, or the records the boot opened: pulled while
  // on) fails it before the index is touched. Every library id changes:
  // QueueStore::remap() wraps this so the queue follows its tracks by
  // path. Runs on the loop: the card worker must have no step under way
  // (app/CardTasks waits for it).
  bool rebuild();

  LibraryIndex* index() { return index_; }
  LocalStorage& storage() { return storage_; }
  const TrackCatalog& catalog() const { return catalog_; }
  // The [index] report (console g).
  void report() const;

  // ---- the card's records (the SD card only; nullptr on the flash) ----
  // The metadata is up: a mounted card, its FatFs drive known, the store
  // open.
  bool records() const { return store_ != nullptr; }
  tagstore::TagStore* store() { return store_; }
  cardfat::FatFs* fatfs() { return fat_; }
  cardroot::Root* root() { return root_; }
  // T failed its checks (the walk streamed it, or a build): the builds of
  // this session leave it out.
  void setTransferBad() { transferBad_ = true; }
  bool transferBad() const { return transferBad_; }
  // The index was loaded, but D or its journal changed since its build:
  // the scan's end rebuilds it.
  bool softStale() const { return softStale_; }

  // The playing track's tags, read by the scan (3.3.3): shown until the
  // next build (TrackCatalog::Overlay; Now Playing redraws by itself).
  void setOverlay(uint32_t track, const tagscan::Record& rec);
  // The update step's memory check (3.4.2): free PSRAM, with what the step
  // frees (the index, and `alsoFreed`: the queue's), is at least 1.1 x the
  // build's estimated peak, and the new track table (the builder's
  // reservation) fits in the old one's block, which the index keeps across
  // the rebuild, or in the largest free block with a sixteenth to spare.
  // False: `why` says what is short.
  bool roomToBuild(size_t alsoFreed, char* why, size_t size) const;
  // The build-at-boot marker (3.4.2): written when the update step can't
  // run for memory, so the next boot builds before the UI, on a fresh heap.
  bool deferToBoot();

private:
  struct Walk {
    uint32_t files = 0;
    uint64_t signature = 0;
    float ms = 0;
  };
  // The last load or build, for the report.
  struct Stats {
    bool fromCache = false;
    const char* cacheNote = "";  // why the cache wasn't used
    Walk walk;                   // the signature walk (the flash)
    float buildWalkMs = 0, addMs = 0, finishMs = 0, loadMs = 0, saveMs = 0;
    uint32_t added = 0;
    int32_t psramUsed = 0;       // PSRAM free before - after
    int32_t internalDelta = 0;   // internal free after - before
    uint32_t internalMinDuring = 0;
    uint32_t internalFreeBefore = 0;
    // The card's boot (3.2.2).
    bool card = false;
    libraryboot::Saved saved = libraryboot::Saved::Missing;
    libraryboot::Action action = libraryboot::Action::Walk;
    const char* why = "";
    float rootMs = 0, openMs = 0, peekMs = 0, compactMs = 0, buildMs = 0, deviceTxtMs = 0;
    LibraryBuilder::Result build;
    bool built = false;          // by the builder (not the walk)
    bool deviceTxtWritten = false;
    tagstore::TagStore::Opened opened;
  };

  bool ensureIndex();
  // The flash: today's signature walk and cache.
  Walk signatureWalk();
  bool loadCache(uint64_t signature);
  bool build(uint64_t* signature, uint8_t addOptions = 0);
  void saveCache(uint64_t signature);
  void cachePath(char* buf, size_t size, bool temp);
  bool beginFlash();
  // The card.
  bool beginCard();
  bool openRecords();
  // Builds from the records (T and D), or walks when there are none; saves.
  // `update`: the update step's (an index in use: records that don't open
  // fail it, the index untouched).
  bool buildCard(bool update = false);
  // /music walked into a path-named index, every track Pending.
  bool walkCard();
  // The track table the build would reserve (roomToBuild()).
  uint32_t buildTrackSlots() const;
  bool saveCard(const LibraryIndex::Inputs& inputs);
  LibraryIndex::Load loadCard(LibraryIndex::Inputs* saved);
  void writeDeviceTxt();
  void compactFirst();

  LocalStorage& storage_;
  LibraryIndex* index_ = nullptr;  // PSRAM
  TrackCatalog catalog_;
  Stats stats_;
  // The card's records (PSRAM).
  cardfat::FatFs* fat_ = nullptr;
  cardroot::Root* root_ = nullptr;
  tagstore::TagStore* store_ = nullptr;
  TrackCatalog::Overlay* overlay_ = nullptr;
  cardcontract::RunFields* run_ = nullptr;
  bool transferBad_ = false;
  bool softStale_ = false;
};
