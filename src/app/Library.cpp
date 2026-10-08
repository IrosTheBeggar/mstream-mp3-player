// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/Library.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>

#include "app/Psram.h"
#include "app/Version.h"
#include "storage/FileStream.h"

namespace {

constexpr const char* kRoot = "/music";
constexpr int kMaxDepth = 8;
// The device's own files (2.12.6): written aside, then replace()d
// (library.idx's are LibraryUpdate's).
constexpr tagstore::Names kTxtNames{"/.player/device.txt", "/.player/device.tmp", "/.player/device"};

uint32_t internalFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t psramFreeNow() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }
float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }
uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
// The index's trims in place (LibraryIndex::ShrinkFn): heap_caps_realloc to
// a smaller size splits the block where it is.
void* psramShrink(void* p, size_t bytes) { return heap_caps_realloc(p, bytes, MALLOC_CAP_SPIRAM); }

// The walk's signature: FNV-1a 64 over every path in walk order, each ended
// with '\n'. Seeded with what else shapes the index (the root, the depth).
struct PathHash {
  uint64_t h = 14695981039346656037ull;
  uint32_t files = 0;
  PathHash() {
    add(kRoot);
    mix(static_cast<uint8_t>(kMaxDepth));
    files = 0;
  }
  void mix(uint8_t b) {
    h ^= b;
    h *= 1099511628211ull;
  }
  void add(const char* s) {
    while (*s) mix(static_cast<uint8_t>(*s++));
    mix('\n');
    ++files;
  }
};

void onHash(const char* path, void* ctx) { static_cast<PathHash*>(ctx)->add(path); }

struct BuildCtx {
  LibraryIndex* index = nullptr;
  uint8_t options = 0;  // addFile()'s (kAddPending: the card's walk)
  PathHash hash;
  uint32_t added = 0;
  uint64_t addUs = 0;
  uint32_t minFree = UINT32_MAX;
};

void onBuild(const char* path, void* p) {
  auto& c = *static_cast<BuildCtx*>(p);
  c.hash.add(path);
  const int64_t t0 = esp_timer_get_time();
  if (c.index->addFile(path, c.options) == LibraryIndex::Add::Added) ++c.added;
  c.addUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
  c.minFree = std::min(c.minFree, internalFree());
}

// device.tmp: whole when it reads as a device.txt.
class TxtCheck : public tagstore::TmpCheck {
public:
  bool whole(tagstore::Fs& fs, const char* tmp) override {
    tagstore::File* f = fs.open(tmp, tagstore::Fs::Mode::Read);
    if (!f) return false;
    char text[512];
    const uint32_t n = std::min<uint32_t>(f->size(), sizeof(text));
    bool ok = n > 0 && f->read(0, text, n);
    fs.close(f);
    cardcontract::devicetxt::Info info;
    return ok && cardcontract::devicetxt::parse(text, n, &info);
  }
};

// D's producer string (TagStore::Config keeps the pointer).
char s_producer[48] = "";

}  // namespace

Library::Library(LocalStorage& storage) : storage_(storage) {}

bool Library::ensureIndex() {
  if (!index_) {
    index_ = psramNew<LibraryIndex>(psramAlloc, psramFree, psramShrink);
    // A rebuild takes the old track table's block again (3.4.2's memory
    // check counts on it: LibraryUpdate::roomToBuild()).
    if (index_) index_->keepTrackBlock(true);
  }
  if (!index_) Serial.println("[lib] no PSRAM for the library index");
  catalog_.setIndex(index_);
  return index_ != nullptr;
}

void Library::cachePath(char* buf, size_t size, bool temp) {
  snprintf(buf, size, "%s/%s", storage_.stateDir(), temp ? "library.tmp" : "library.idx");
}

Library::Walk Library::signatureWalk() {
  Walk w;
  PathHash hash;
  const int64_t t0 = esp_timer_get_time();
  w.files = storage_.forEachFile(onHash, &hash, kMaxDepth);
  w.ms = msSince(t0);
  w.signature = hash.h;
  return w;
}

bool Library::loadCache(uint64_t signature) {
  char path[48];
  cachePath(path, sizeof(path), false);
  File f = storage_.fs().open(path, FILE_READ);
  if (!f) {
    stats_.cacheNote = "no cache yet";
    return false;
  }
  FileSource src(f);
  const int64_t t0 = esp_timer_get_time();
  const LibraryIndex::Load r = index_->load(src, signature);
  f.close();
  stats_.loadMs = msSince(t0);
  switch (r) {
    case LibraryIndex::Load::Loaded:
      stats_.fromCache = true;
      return true;
    case LibraryIndex::Load::Stale: stats_.cacheNote = "the card changed"; break;
    case LibraryIndex::Load::Outdated: stats_.cacheNote = "the cache is an older version's: rebuilt once"; break;
    case LibraryIndex::Load::Corrupt: stats_.cacheNote = "the cache was unreadable"; break;
    case LibraryIndex::Load::NoMemory: stats_.cacheNote = "no PSRAM to load the cache"; break;
  }
  return false;
}

bool Library::build(uint64_t* signature, uint8_t addOptions) {
  index_->clear();
  BuildCtx ctx;
  ctx.index = index_;
  ctx.options = addOptions;
  ctx.minFree = internalFree();
  const int64_t t0 = esp_timer_get_time();
  // The files seen by the signature walk (0: unknown) size the blocks up
  // front: a lower build peak than growing them.
  index_->begin(kRoot, stats_.walk.files);
  storage_.forEachFile(onBuild, &ctx, kMaxDepth);
  const int64_t t1 = esp_timer_get_time();
  const bool ok = index_->finish();
  stats_.addMs = ctx.addUs / 1000.0f;
  stats_.buildWalkMs = static_cast<float>(t1 - t0) / 1000.0f - stats_.addMs;
  stats_.finishMs = msSince(t1);
  stats_.added = ctx.added;
  stats_.internalMinDuring = std::min(ctx.minFree, internalFree());
  if (stats_.walk.files == 0) stats_.walk.files = ctx.hash.files;  // a rebuild has no signature walk
  *signature = ctx.hash.h;
  if (!ok) Serial.println("[lib] index build FAILED (out of PSRAM)");
  return ok;
}

void Library::saveCache(uint64_t signature) {
  char tmp[48], path[48];
  cachePath(tmp, sizeof(tmp), true);
  cachePath(path, sizeof(path), false);
  fs::FS& fs = storage_.fs();
  const int64_t t0 = esp_timer_get_time();
  File f = fs.open(tmp, FILE_WRITE);
  bool ok = static_cast<bool>(f);
  if (ok) {
    FileSink sink(f);
    ok = index_->save(sink, signature);
    f.close();
  }
  // Written aside, then swapped in: a cut-off write never leaves a cache
  // that looks whole (and load() checks its sum anyway).
  if (ok) {
    fs.remove(path);
    ok = fs.rename(tmp, path);
  } else {
    fs.remove(tmp);
  }
  stats_.saveMs = msSince(t0);
  if (!ok) Serial.printf("[lib] couldn't save the index cache to %s\n", path);
}

bool Library::begin() {
  if (!storage_.available()) {
    Serial.println("[lib] no storage: no library (the built-in tracks still play)");
    return false;
  }
  if (!ensureIndex()) return false;
  if (storage_.onCard() && cardfat::ready()) return beginCard();
  return beginFlash();
}

// ---- the flash: the walk and its signature, as before ----

bool Library::beginFlash() {
  stats_ = Stats{};
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  stats_.walk = signatureWalk();
  bool ok = loadCache(stats_.walk.signature);
  if (!ok) {
    uint64_t signature = 0;
    ok = build(&signature);
    if (ok) saveCache(signature);
  }
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  if (stats_.internalMinDuring == 0) stats_.internalMinDuring = internalFree();
  if (stats_.fromCache) {
    Serial.printf("[lib] %s /music: %lu files walked in %.0f ms, index loaded from the cache in %.0f ms\n",
                  storage_.name(), (unsigned long)stats_.walk.files, stats_.walk.ms, stats_.loadMs);
  } else {
    Serial.printf("[lib] %s /music: %lu files walked in %.0f ms; built (%s): walk %.0f ms, add %.0f ms, finish "
                  "%.0f ms; cache saved in %.0f ms\n",
                  storage_.name(), (unsigned long)stats_.walk.files, stats_.walk.ms, stats_.cacheNote,
                  stats_.buildWalkMs, stats_.addMs, stats_.finishMs, stats_.saveMs);
  }
  if (ok) {
    Serial.printf("[lib] %lu tracks, %lu artists, %lu albums; %u B of PSRAM; internal RAM %+ld B\n",
                  (unsigned long)index_->trackCount(), (unsigned long)index_->artistCount(),
                  (unsigned long)index_->albumCount(), (unsigned)index_->memory().total,
                  (long)stats_.internalDelta);
  }
  return ok;
}

// ---- the card: the boot's decision (3.2.2, LibraryUpdate) ----

bool Library::openRecords() {
  if (store_) return true;
  storage_.stateDir();  // /.player
  fat_ = psramNew<cardfat::FatFs>();
  root_ = psramNew<cardroot::Root>();
  overlay_ = psramNew<TrackCatalog::Overlay>();
  run_ = psramNew<cardcontract::RunFields>();
  held_ = psramNew<TrackCatalog::Held>();
  auto* scratch = static_cast<uint8_t*>(psramAlloc(1024));
  if (!fat_ || !root_ || !overlay_ || !run_ || !held_ || !scratch) {
    psramFree(scratch);
    return false;
  }
  int64_t t0 = esp_timer_get_time();
  cardroot::read(*fat_, scratch, 1024, root_);
  psramFree(scratch);
  stats_.rootMs = msSince(t0);
  snprintf(s_producer, sizeof(s_producer), "mstream-player %s", version::player());
  tagstore::TagStore::Config c;
  c.dir = "/.player";
  c.producer = s_producer;
  c.parserVersion = tagscan::kParserVersion;
  c.cardId = root_->present ? root_->identity.cardId : 0;
  t0 = esp_timer_get_time();
  store_ = psramNew<tagstore::TagStore>(*fat_, c, psramAlloc, psramFree);
  if (!store_) return false;
  stats_.opened = store_->open();
  stats_.openMs = msSince(t0);
  LibraryUpdate::Config u;
  u.fs = fat_;
  u.store = store_;
  u.root = root_;
  u.index = index_;
  u.alloc = psramAlloc;
  u.release = psramFree;
  u.musicRoot = kRoot;
  u.walk = walkMusic;
  u.walkCtx = this;
  u.nowUs = nowUs;
  update_ = psramNew<LibraryUpdate>(u);
  if (!update_) {
    psramDelete(store_);
    store_ = nullptr;
    return false;
  }
  catalog_.setOverlay(overlay_);
  return true;
}

bool Library::walkMusic(LibraryIndex& index, void* self) {
  // No records at all: /music walked into a path-named index (the
  // validation walk then makes D, and the scan reads the tags). Every file
  // is unread: Pending, so the scan reads the playing track, the queue's
  // and the Library tab's first (3.3.3), and the status line counts them.
  Library& lib = *static_cast<Library*>(self);
  (void)index;  // (the one index: lib.index_)
  lib.stats_.walk = Walk{};
  uint64_t signature = 0;
  const int64_t t0 = esp_timer_get_time();
  if (!lib.build(&signature, LibraryIndex::kAddPending)) return false;
  lib.stats_.buildMs = msSince(t0);
  Serial.printf("[lib] /music walked: %lu files in %.0f ms (add %.0f ms, finish %.0f ms)\n",
                (unsigned long)lib.stats_.walk.files, lib.stats_.buildWalkMs + lib.stats_.addMs, lib.stats_.addMs,
                lib.stats_.finishMs);
  return true;
}

bool Library::beginCard() {
  stats_ = Stats{};
  stats_.card = true;
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  const int64_t boot = esp_timer_get_time();
  if (!openRecords()) {
    Serial.println("[lib] no PSRAM for the card's records: /music walked as before");
    return beginFlash();
  }
  const uint32_t minBefore = internalFree();
  const LibraryUpdate::Booted b = update_->boot();
  stats_.internalMinDuring = std::min(minBefore, internalFree());
  stats_.saved = b.saved;
  stats_.action = b.decision.action;
  stats_.why = b.noMemory ? "no PSRAM" : b.decision.why;
  stats_.peekMs = b.peekMs;
  stats_.loadMs = b.loadMs;
  stats_.compactMs = b.compactMs;
  if (!b.walked) stats_.buildMs = b.buildMs;
  stats_.saveMs = b.saveMs;
  stats_.fromCache = (b.decision.action == libraryboot::Action::Load && b.ok) || b.loadedShort;
  stats_.build = b.build;
  stats_.built = b.built;
  softStale_ = b.softStale;
  if (b.compacted) {
    const tagstore::TagStore::Compacted& c = b.compaction;
    Serial.printf("[lib] the journals compacted into tags.bin in %.0f ms: %s (%lu records, %lu chunks merged%s)\n",
                  b.compactMs, c.ok ? "ok" : c.error ? c.error : "FAILED", (unsigned long)c.records,
                  (unsigned long)c.chunksMerged, c.walkMerged ? ", the walk's" : "");
  }
  if (b.built) {
    const LibraryBuilder::Result& r = b.build;
    Serial.printf("[lib] built from the records in %.0f ms: %lu tracks, %lu from the transfer's, %lu from the "
                  "device's, %lu by their paths (%lu for the scan)%s%s; peak %u B of PSRAM\n",
                  b.buildMs, (unsigned long)index_->trackCount(), (unsigned long)r.fromTransfer,
                  (unsigned long)r.fromDevice, (unsigned long)r.fromPath, (unsigned long)r.pending,
                  r.restarted ? "; restarted without a file that failed its checks" : "",
                  update_->transferBad() ? "; T left out" : "", (unsigned)index_->memory().buildPeak);
  }
  if (b.journalsLeft) Serial.println("[lib] the journals couldn't be compacted first: built from tags.bin alone");
  if (b.readErrors) {
    Serial.println("[lib] a read of the card's records FAILED as they were built (the card?): what was built is "
                   "saved as one that left records out (the next boot rebuilds it at its scan's end)");
  }
  if (b.loadedShort) {
    Serial.printf("[lib] the build-at-boot marker's build ran out of PSRAM: library.idx (it matches the card) loaded "
                  "instead, the marker %s; no update step this session writes it again for a short PSRAM\n",
                  b.markerRemoved ? "removed" : "COULDN'T be removed");
  }
  if (b.saveFailed) Serial.println("[lib] couldn't save library.idx (the next boot builds again)");
  if (b.noMemory && b.decision.action == libraryboot::Action::Load) {
    Serial.println("[lib] no PSRAM to load library.idx: no library (the built-in tracks still play)");
  } else if (!b.ok) {
    Serial.printf("[lib] the build from the records FAILED (%s)\n", b.noMemory ? "out of PSRAM" : "unreadable");
  }
  writeDeviceTxt();
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  if (stats_.internalMinDuring == 0) stats_.internalMinDuring = internalFree();

  char rootLine[200];
  cardroot::describe(*root_, rootLine, sizeof(rootLine));
  const tagstore::DeviceInfo& di = store_->device();
  Serial.printf("[lib] the card: %s; D %s (%lu records, %lu journal chunks%s), read in %.0f ms + %.0f ms%s\n",
                rootLine, di.present ? "present" : "none", (unsigned long)(di.present ? di.tags.recordCount : 0),
                (unsigned long)store_->chunkCount(), store_->hasWalk() ? ", a walk to merge" : "", stats_.rootMs,
                stats_.openMs, store_->twins() || b.tmpSettled.twins ? "; TWINS: the card wants a disk check on a PC" : "");
  Serial.printf("[lib] library.idx %s%s%s: %s (%s)%s in %.0f ms (header %.0f ms)\n", libraryboot::savedName(b.saved),
                b.tmpSettled.what == tagstore::Settle::Promoted ? " (library.tmp taken: a cut fell mid-save)" : "",
                b.marker ? ", the build-at-boot marker set" : "", libraryboot::actionName(b.decision.action),
                b.decision.why,
                softStale_ ? "; the scan went on since its build: rebuilt at the scan's end" : "",
                b.decision.action == libraryboot::Action::Load ? b.loadMs : b.buildMs + b.compactMs, b.peekMs);
  if (b.ok) {
    Serial.printf("[lib] %lu tracks, %lu artists, %lu albums; %u B of PSRAM; browsable %.0f ms after the mount; "
                  "internal RAM %+ld B\n",
                  (unsigned long)index_->trackCount(), (unsigned long)index_->artistCount(),
                  (unsigned long)index_->albumCount(), (unsigned)index_->memory().total, msSince(boot),
                  (long)stats_.internalDelta);
  }
  return b.ok;
}

void Library::writeDeviceTxt() {
  const int64_t t0 = esp_timer_get_time();
  const char* v = version::player();
  if (v[0] == 'v') ++v;
  char want[400];
  const size_t n = cardcontract::devicetxt::format(cardcontract::devicetxt::current(v), want, sizeof(want));
  if (n == 0) return;
  TxtCheck check;
  tagstore::settle(*fat_, kTxtNames, &check);
  bool same = false;
  if (tagstore::File* f = fat_->open(kTxtNames.path, tagstore::Fs::Mode::Read)) {
    char have[400];
    same = f->size() == n && f->read(0, have, static_cast<uint32_t>(n)) && std::memcmp(have, want, n) == 0;
    fat_->close(f);
  }
  if (!same && tagstore::prepareTmp(*fat_, kTxtNames)) {
    tagstore::File* f = fat_->open(kTxtNames.tmp, tagstore::Fs::Mode::Create);
    bool ok = f && f->write(0, want, static_cast<uint32_t>(n));
    if (f) ok = fat_->close(f) && ok;
    ok = ok && tagstore::replace(*fat_, kTxtNames);
    if (!ok) fat_->remove(kTxtNames.tmp);
    stats_.deviceTxtWritten = ok;
    Serial.printf("[lib] /.player/device.txt %s (firmware %s)\n", ok ? "written" : "COULDN'T be written", v);
  }
  stats_.deviceTxtMs = msSince(t0);
}

bool Library::rebuild() {
  if (!storage_.available() || !ensureIndex()) return false;
  if (records()) {
    Serial.println("[lib] the card's library updates through the update step (gb, g0): not here");
    return false;
  }
  stats_ = Stats{};
  stats_.cacheNote = "asked for";
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  uint64_t signature = 0;
  const bool ok = build(&signature);
  if (ok) saveCache(signature);
  stats_.walk.signature = signature;
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  return ok;
}

void Library::fence(uint32_t playing) {
  if (!update_) return;
  // Taken while the catalog still has its index: Now Playing's names (a
  // built-in track's need no copy).
  if (held_) {
    catalog_.take(playing, held_);
    catalog_.setHeld(held_);
  }
  catalog_.setIndex(nullptr);
}

void Library::unfence() {
  catalog_.setIndex(index_);
  catalog_.setHeld(nullptr);
  if (held_) *held_ = TrackCatalog::Held{};
}

void Library::setOverlay(uint32_t track, const tagscan::Record& rec) {
  LibraryIndex* idx = index();
  if (!overlay_ || !run_ || !idx || !idx->ready()) return;
  rec.toRunFields(run_);
  const LibraryIndex::TagView v = LibraryBuilder::viewOf(rec.rec, run_, LibraryIndex::kFromDevice);
  overlay_->set(*idx, track, v);
}

void Library::deferralWhy(char* why, size_t size) const {
  if (!size) return;
  why[0] = 0;
  if (!update_) return;
  const LibraryUpdate::Verdict& v = update_->verdict();
  if (update_->deferForced()) {
    snprintf(why, size, "gb! asked for the deferral");
  } else if (v.shortOf == LibraryUpdate::Short::Room) {
    snprintf(why, size, "%u KB of PSRAM for a peak of about %u KB", (unsigned)(v.room / 1024),
             (unsigned)(v.peak / 1024));
  } else if (v.shortOf == LibraryUpdate::Short::Carry) {
    snprintf(why, size, "the queue can't be carried across the build (no PSRAM; or the card refused queue.txt and "
                        "its text would leave the build short)");
  } else if (v.shortOf == LibraryUpdate::Short::Table) {
    const size_t table = static_cast<size_t>(update_->trackSlots()) * sizeof(LibraryIndex::Track);
    snprintf(why, size, "the track table %u KB fits neither the old one's block (%u KB) nor the largest free one (%u KB)",
             (unsigned)(table / 1024), (unsigned)(index_ ? index_->memory().tracks / 1024 : 0),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
  }
}

void Library::report() const {
  if (!index_ || !index_->ready()) {
    Serial.println("[index] library: empty (no card, or the build failed; g0 rebuilds it)");
    return;
  }
  const LibraryIndex::Memory m = index_->memory();
  const uint32_t t = index_->trackCount();
  Serial.printf("[index] library, %s /music: %lu tracks, %lu artists, %lu albums, %lu folders\n", storage_.name(),
                (unsigned long)t, (unsigned long)index_->artistCount(), (unsigned long)index_->albumCount(),
                (unsigned long)index_->folderCount());
  if (stats_.card) {
    Serial.printf("[index] the card's boot: library.idx %s, %s (%s); root %.1f ms, D %.1f ms, header %.1f ms, load "
                  "%.1f ms, compaction %.1f ms, build %.1f ms, save %.1f ms, device.txt %.1f ms%s\n",
                  libraryboot::savedName(stats_.saved), libraryboot::actionName(stats_.action), stats_.why,
                  stats_.rootMs, stats_.openMs, stats_.peekMs, stats_.loadMs, stats_.compactMs, stats_.buildMs,
                  stats_.saveMs, stats_.deviceTxtMs, stats_.deviceTxtWritten ? " (rewritten)" : "");
    if (stats_.built) {
      const LibraryBuilder::Result& r = stats_.build;
      Serial.printf("[index] the builder: %lu from the transfer's records, %lu from the device's, %lu by their paths "
                    "(%lu for the scan), %lu dropped, %lu ignored; T %s, D %s%s; its own memory %u B\n",
                    (unsigned long)r.fromTransfer, (unsigned long)r.fromDevice, (unsigned long)r.fromPath,
                    (unsigned long)r.pending, (unsigned long)r.dropped, (unsigned long)r.ignored,
                    r.transferUsed ? "used" : cardcontract::whyName(r.transferWhy),
                    r.deviceUsed ? "used" : cardcontract::whyName(r.deviceWhy),
                    r.restarted ? " (restarted)" : "", (unsigned)r.workBytes);
    }
    if (update_ && update_->steps() > 0) {
      const LibraryUpdate::Step& s = update_->last();
      Serial.printf("[index] the last update step (%s): %s in %.1f ms on the card worker, the fence up %lu ms, %s in "
                    "%.1f ms%s; %lu steps, %lu deferred to the boot\n",
                    s.why, s.built ? (s.walked ? "walked" : "built") : "FAILED", s.buildMs, (unsigned long)s.fenceMs,
                    s.saved ? "saved" : "NOT saved", s.saveMs, s.markerRemoved ? " (the marker removed)" : "",
                    (unsigned long)update_->steps(), (unsigned long)update_->deferrals());
    }
  } else if (stats_.fromCache) {
    Serial.printf("[index] time: signature walk %.1f ms (%lu files), cache load %.1f ms\n", stats_.walk.ms,
                  (unsigned long)stats_.walk.files, stats_.loadMs);
  } else {
    Serial.printf("[index] time: signature walk %.1f ms; built (%s): walk %.1f ms (the file system), add %.1f ms, "
                  "finish %.1f ms (sort + views + trim), cache save %.1f ms\n",
                  stats_.walk.ms, stats_.cacheNote, stats_.buildWalkMs, stats_.addMs, stats_.finishMs, stats_.saveMs);
  }
  Serial.printf("[index] PSRAM: %u B held (%.1f B/track): strings %u, tracks %u, artists %u, albums %u, folders %u, "
                "views %u; build peak %u B; PSRAM free fell %ld B\n",
                (unsigned)m.total, t ? static_cast<float>(m.total) / t : 0.0f, (unsigned)m.strings,
                (unsigned)m.tracks, (unsigned)m.artists, (unsigned)m.albums, (unsigned)m.folders, (unsigned)m.views,
                (unsigned)m.buildPeak, (long)stats_.psramUsed);
  Serial.printf("[index] internal RAM: free %lu B before, %+ld B after, lowest %lu B during; the index object is in "
                "PSRAM (%u B)\n",
                (unsigned long)stats_.internalFreeBefore, (long)stats_.internalDelta,
                (unsigned long)stats_.internalMinDuring, (unsigned)sizeof(LibraryIndex));
  const LibraryIndex::Span a = index_->artistsAZ();
  Serial.print("[index] artists A-Z:");
  for (uint32_t i = 0; i < a.count && i < 4; ++i) Serial.printf(" \"%s\"", index_->artistName(a[i]));
  Serial.printf("%s; rail buckets:", a.count > 4 ? " ..." : "");
  for (int b = 0; b < LibraryIndex::kBuckets; ++b) {
    const uint32_t n = index_->bucketStart(LibraryIndex::View::Artists, b + 1) -
                       index_->bucketStart(LibraryIndex::View::Artists, b);
    if (n) Serial.printf(" %c%lu", b == 0 ? '#' : 'A' + b - 1, (unsigned long)n);
  }
  Serial.println();
}
