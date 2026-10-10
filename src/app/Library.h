// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>

#include "CardContract.h"
#include "CardRoot.h"
#include "LibraryBoot.h"
#include "LibraryBuilder.h"
#include "LibraryIndex.h"
#include "LibraryUpdate.h"
#include "TagScan.h"
#include "TagStore.h"
#include "TrackCatalog.h"
#include "storage/CardFat.h"
#include "storage/LocalStorage.h"

// The music library: the single store of what's on the card, a
// LibraryIndex in PSRAM that the queue and the player hold ids into (through
// the TrackCatalog, which adds the built-in tracks).
//
// On the SD card (docs/METADATA.md 3.2.2, 3.4; milestones N10, N12), the
// boot never walks the card to decide: the transfer's root
// (/.mstream/manifest.bin, cardroot::read()) and the device's records
// (/.player/tags.bin, D: TagStore::open(), its recovery first) are opened
// here, then lib/core LibraryUpdate::boot() settles library.tmp, reads the
// build-at-boot marker and library.idx's header, and loads it, builds from
// the records (the journals compacted first) or, with no records at all (a
// card-reader card, or this firmware's first boot), walks /music through
// walkMusic(); a build is saved. /.player/device.txt is rewritten when its
// content would change (2.15). The validation walk, the scan and the
// update step follow in the background (app/CardTasks runs LibraryUpdate's
// step: its build and its save on the card worker, its fence here: index()
// is nullptr behind it, and the catalog answers from a held copy of the
// playing track's names). An index loaded while the scan went on since its
// build (its soft inputs differ) is rebuilt at the scan's end
// (softStale()).
//
// On the flash fallback (no card) nothing of that exists: /music is walked
// at every boot into a path signature, and library.idx is loaded when it
// was saved for it, as before (the test audio of `pio run -t uploadfs`).
class Library {
public:
  explicit Library(LocalStorage& storage);

  // At boot, once storage is up. False: no index (no storage, no PSRAM, or
  // a build that ran out of memory: the build-at-boot marker's loads the
  // matching library.idx instead); the built-in tracks still play.
  bool begin();
  // The flash's rebuild (g0, "Try again"): /music walked and saved, on the
  // loop. Every library id changes: QueueStore::remap() wraps this so the
  // queue follows its tracks by path. On the card the update step is
  // LibraryUpdate's (app/CardTasks): this refuses.
  bool rebuild();

  // The index the loop may read: nullptr behind the update step's fence
  // (from the old index's release to the new one's end, 3.4.2), as when
  // there is none.
  LibraryIndex* index() { return update_ ? update_->readable() : index_; }
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
  void setTransferBad() {
    if (update_) update_->setTransferBad();
  }
  bool transferBad() const { return update_ && update_->transferBad(); }
  // The index was loaded, but D or its journal changed since its build:
  // the scan's end rebuilds it.
  bool softStale() const { return softStale_; }
  // The boot and the update step (lib/core LibraryUpdate); nullptr on the
  // flash (and before begin()).
  LibraryUpdate* update() { return update_; }

  // ---- the update step's fence (3.4.2, steps 2 and 5; main.cpp's) ----
  // Up: the names of `playing` (a catalog id: the current entry's) kept
  // for Now Playing, the catalog without its index (every library id
  // unknown: no path, no name); index() is nullptr from CardTasks::fenceUp()
  // right after (LibraryUpdate::fencedUp()). Down: the index back, the
  // copy dropped.
  void fence(uint32_t playing);
  void unfence();

  // The playing track's tags, read by the scan (3.3.3): shown until the
  // next build (TrackCatalog::Overlay; Now Playing redraws by itself).
  void setOverlay(uint32_t track, const tagscan::Record& rec);
  // The memory check's verdict as the update step last took it (a
  // deferral's reason), into `why`.
  void deferralWhy(char* why, size_t size) const;

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
  // LibraryUpdate's walk (no records at all): /music walked into a
  // path-named index, every track Pending. On the loop at boot; on the card
  // worker in an update step that finds no records.
  static bool walkMusic(LibraryIndex& index, void* self);
  void writeDeviceTxt();

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
  LibraryUpdate* update_ = nullptr;     // PSRAM
  TrackCatalog::Held* held_ = nullptr;  // PSRAM: Now Playing's names behind the fence
  bool softStale_ = false;
};
