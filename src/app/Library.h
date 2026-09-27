#pragma once
#include <Arduino.h>

#include "LibraryIndex.h"
#include "TrackCatalog.h"
#include "storage/LocalStorage.h"

// The music library: the single store of what's on the card, a
// LibraryIndex in PSRAM that the queue and the player hold ids into (through
// the TrackCatalog, which adds the built-in tracks).
//
// At boot /music is walked (readdir, names only) into a signature, a hash
// of every path. If the cache on the card (/.player/library.idx, the index
// as LibraryIndex::save() writes it) was saved for that signature, it's
// loaded: no build, no sorting, no build peak. Otherwise the walk is done
// again, this time into a new index, which is then saved for next time. So
// a file added, removed or renamed anywhere under /music rebuilds it; the
// same card again only costs the walk and a read of the cache.
class Library {
public:
  explicit Library(LocalStorage& storage);

  // At boot, once storage is up. False: no index (no storage, no PSRAM, or
  // a build that ran out of memory); the built-in tracks still play.
  bool begin();
  // Walks /music and builds again, whatever the cache says (console g0).
  // Every library id changes: QueueStore::remap() wraps this so the queue
  // follows its tracks by path.
  bool rebuild();

  LibraryIndex* index() { return index_; }
  LocalStorage& storage() { return storage_; }
  const TrackCatalog& catalog() const { return catalog_; }
  // The [index] report (console g).
  void report() const;

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
    Walk walk;                   // the signature walk
    float buildWalkMs = 0, addMs = 0, finishMs = 0, loadMs = 0, saveMs = 0;
    uint32_t added = 0;
    int32_t psramUsed = 0;       // PSRAM free before - after
    int32_t internalDelta = 0;   // internal free after - before
    uint32_t internalMinDuring = 0;
    uint32_t internalFreeBefore = 0;
  };

  bool ensureIndex();
  Walk signatureWalk();
  bool loadCache(uint64_t signature);
  // A walk into a new index; `signature`: that walk's (what was built from).
  bool build(uint64_t* signature);
  void saveCache(uint64_t signature);
  void cachePath(char* buf, size_t size, bool temp);

  LocalStorage& storage_;
  LibraryIndex* index_ = nullptr;  // PSRAM
  TrackCatalog catalog_;
  Stats stats_;
};
