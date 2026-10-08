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
// The device's own files (2.12.6): written aside, then replace()d.
constexpr tagstore::Names kIdxNames{"/.player/library.idx", "/.player/library.tmp", "/.player/library"};
constexpr tagstore::Names kTxtNames{"/.player/device.txt", "/.player/device.tmp", "/.player/device"};
constexpr const char* kMarker = "/.player/build.req";

uint32_t internalFree() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)); }
uint32_t psramFreeNow() { return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)); }
float msSince(int64_t t0) { return static_cast<float>(esp_timer_get_time() - t0) / 1000.0f; }
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
  PathHash hash;
  uint32_t added = 0;
  uint64_t addUs = 0;
  uint32_t minFree = UINT32_MAX;
};

void onBuild(const char* path, void* p) {
  auto& c = *static_cast<BuildCtx*>(p);
  c.hash.add(path);
  const int64_t t0 = esp_timer_get_time();
  if (c.index->addFile(path) == LibraryIndex::Add::Added) ++c.added;
  c.addUs += static_cast<uint64_t>(esp_timer_get_time() - t0);
  c.minFree = std::min(c.minFree, internalFree());
}

// A file of the card (FatFs) as LibraryIndex's streams.
class FileIn : public ByteSource {
public:
  explicit FileIn(tagstore::File& f) : f_(f) {}
  size_t read(void* data, size_t n) override {
    const uint32_t left = f_.size() > at_ ? f_.size() - at_ : 0;
    const uint32_t k = static_cast<uint32_t>(std::min<size_t>(n, left));
    if (k == 0 || !f_.read(at_, data, k)) return 0;
    at_ += k;
    return k;
  }

private:
  tagstore::File& f_;
  uint32_t at_ = 0;
};

class FileOut : public ByteSink {
public:
  explicit FileOut(tagstore::File& f) : f_(f) {}
  bool write(const void* data, size_t n) override {
    if (!f_.write(at_, data, static_cast<uint32_t>(n))) return false;
    at_ += static_cast<uint32_t>(n);
    return true;
  }

private:
  tagstore::File& f_;
  uint32_t at_ = 0;
};

// library.tmp left alone by a cut (2.12.6's settle()): whole when its
// header reads and its sum holds (an older version's is taken as it is: it
// is rebuilt anyway).
class IdxCheck : public tagstore::TmpCheck {
public:
  explicit IdxCheck(LibraryIndex& idx) : idx_(idx) {}
  bool whole(tagstore::Fs& fs, const char* tmp) override {
    tagstore::File* f = fs.open(tmp, tagstore::Fs::Mode::Read);
    if (!f) return false;
    FileIn head(*f);
    LibraryIndex::Inputs got;
    const LibraryIndex::Load r = LibraryIndex::peek(head, &got);
    bool ok = r == LibraryIndex::Load::Outdated;
    if (r == LibraryIndex::Load::Loaded) {
      FileIn all(*f);
      ok = idx_.load(all, got) == LibraryIndex::Load::Loaded;
      idx_.clear();
    }
    fs.close(f);
    return ok;
  }

private:
  LibraryIndex& idx_;
};

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
  if (!index_) index_ = psramNew<LibraryIndex>(psramAlloc, psramFree, psramShrink);
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

bool Library::build(uint64_t* signature) {
  index_->clear();
  BuildCtx ctx;
  ctx.index = index_;
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

// ---- the card: the boot's decision (3.2.2) ----

bool Library::openRecords() {
  if (store_) return true;
  storage_.stateDir();  // /.player
  fat_ = psramNew<cardfat::FatFs>();
  root_ = psramNew<cardroot::Root>();
  overlay_ = psramNew<TrackCatalog::Overlay>();
  run_ = psramNew<cardcontract::RunFields>();
  auto* scratch = static_cast<uint8_t*>(psramAlloc(1024));
  if (!fat_ || !root_ || !overlay_ || !run_ || !scratch) {
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
  catalog_.setOverlay(overlay_);
  return true;
}

LibraryIndex::Load Library::loadCard(LibraryIndex::Inputs* saved) {
  tagstore::File* f = fat_->open(kIdxNames.path, tagstore::Fs::Mode::Read);
  if (!f) return LibraryIndex::Load::Corrupt;
  FileIn in(*f);
  const int64_t t0 = esp_timer_get_time();
  const LibraryIndex::Load r = index_->load(in, *saved);  // its own inputs: the decision compared them
  stats_.loadMs = msSince(t0);
  fat_->close(f);
  return r;
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
  // library.idx: a cut rename settled (2.12.6), the marker, its header.
  IdxCheck check(*index_);
  const tagstore::Settled idxSettled = tagstore::settle(*fat_, kIdxNames, &check);
  const bool marker = fat_->exists(kMarker);
  int64_t t0 = esp_timer_get_time();
  LibraryIndex::Inputs saved;
  libraryboot::In in;
  in.saved = libraryboot::Saved::Missing;
  if (tagstore::File* f = fat_->open(kIdxNames.path, tagstore::Fs::Mode::Read)) {
    FileIn head(*f);
    in.saved = libraryboot::savedOf(LibraryIndex::peek(head, &saved), saved, root_->identity);
    fat_->close(f);
  }
  stats_.peekMs = msSince(t0);
  in.marker = marker;
  in.transfer = root_->present;
  in.device = store_->device().present;
  libraryboot::Decision d = libraryboot::decide(in);
  stats_.saved = in.saved;
  bool ok = false;
  if (d.action == libraryboot::Action::Load) {
    const LibraryIndex::Load r = loadCard(&saved);
    if (r == LibraryIndex::Load::Loaded) {
      ok = true;
      stats_.fromCache = true;
      softStale_ = saved.deviceCrc != store_->deviceCrc() || saved.journalSeq != store_->journalSeq();
    } else if (r == LibraryIndex::Load::NoMemory) {
      Serial.println("[lib] no PSRAM to load library.idx: no library (the built-in tracks still play)");
      stats_.action = d.action;
      stats_.why = "no PSRAM";
      return false;
    } else {
      in.saved = libraryboot::Saved::Corrupt;  // its sum failed: built or walked instead
      d = libraryboot::decide(in);
    }
  }
  stats_.action = d.action;
  stats_.why = d.why;
  if (!ok && d.action == libraryboot::Action::Build) ok = buildCard();
  if (!ok && d.action == libraryboot::Action::Walk) ok = walkCard();
  if (ok && d.removeMarker) fat_->remove(kMarker);
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
                stats_.openMs, store_->twins() || idxSettled.twins ? "; TWINS: the card wants a disk check on a PC" : "");
  Serial.printf("[lib] library.idx %s%s: %s (%s)%s in %.0f ms (header %.0f ms)\n", libraryboot::savedName(in.saved),
                marker ? ", the build-at-boot marker set" : "", libraryboot::actionName(d.action), d.why,
                softStale_ ? "; the scan went on since its build: rebuilt at the scan's end" : "",
                d.action == libraryboot::Action::Load ? stats_.loadMs : stats_.buildMs + stats_.compactMs,
                stats_.peekMs);
  if (ok) {
    Serial.printf("[lib] %lu tracks, %lu artists, %lu albums; %u B of PSRAM; browsable %.0f ms after the mount; "
                  "internal RAM %+ld B\n",
                  (unsigned long)index_->trackCount(), (unsigned long)index_->artistCount(),
                  (unsigned long)index_->albumCount(), (unsigned)index_->memory().total, msSince(boot),
                  (long)stats_.internalDelta);
  }
  return ok;
}

void Library::compactFirst() {
  if (!store_->hasJournals() && !store_->wantsCompaction()) return;
  const int64_t t0 = esp_timer_get_time();
  const tagstore::TagStore::Compacted c = store_->compact();
  stats_.compactMs = msSince(t0);
  Serial.printf("[lib] the journals compacted into tags.bin in %.0f ms: %s (%lu records, %lu chunks merged%s)\n",
                stats_.compactMs, c.ok ? "ok" : c.error ? c.error : "FAILED", (unsigned long)c.records,
                (unsigned long)c.chunksMerged, c.walkMerged ? ", the walk's" : "");
}

bool Library::buildCard() {
  compactFirst();
  const tagstore::DeviceInfo& di = store_->device();
  const bool useT = root_->present && !transferBad_;
  tagstore::File* t = useT ? fat_->open(root_->tagsPath, tagstore::Fs::Mode::Read) : nullptr;
  tagstore::File* d = di.present ? fat_->open(store_->devicePath(), tagstore::Fs::Mode::Read) : nullptr;
  tagstore::File* dRows = di.present ? fat_->open(store_->devicePath(), tagstore::Fs::Mode::Read) : nullptr;
  tagstore::File* dFacts = di.present ? fat_->open(store_->devicePath(), tagstore::Fs::Mode::Read) : nullptr;
  auto closeAll = [&] {
    for (tagstore::File* f : {t, d, dRows, dFacts})
      if (f) fat_->close(f);
  };
  if (!t && !d) {
    closeAll();
    return walkCard();
  }
  constexpr uint32_t kRowsBuf = 1024, kFactsBuf = 3072;
  auto* bufs = static_cast<uint8_t*>(psramAlloc(kRowsBuf + kFactsBuf));
  auto* rows = psramNew<tagstore::BuilderRows>();
  auto* facts = psramNew<tagstore::BuilderFacts>();
  LibraryBuilder builder(psramAlloc, psramFree);
  LibraryBuilder::Config bc;
  bc.root = kRoot;
  bc.transfer = t;
  // T's skew as the walk found it at this commit; before the first walk
  // after it T's paths count as present (the software listed the card
  // moments ago) and its records are taken as they are.
  const bool walkedHere = di.present && di.header.walked && di.header.walk == root_->identity;
  bc.skew = walkedHere ? di.header.skew : 0;
  bc.transferLists = t && !walkedHere;
  bc.device = d;
  if (d && bufs && rows && dRows && rows->begin(*dRows, bufs, kRowsBuf)) bc.rows = rows;
  if (d && bufs && facts && dFacts && facts->begin(*dFacts, bufs + kRowsBuf, kFactsBuf)) bc.facts = facts;
  bc.libraryRoots = root_->rootList();
  bc.libraryRootCount = root_->rootCount;
  const int64_t t0 = esp_timer_get_time();
  const uint32_t minBefore = internalFree();
  stats_.build = builder.build(*index_, bc);
  stats_.buildMs = msSince(t0);
  stats_.internalMinDuring = std::min(minBefore, internalFree());
  stats_.built = true;
  closeAll();
  psramDelete(rows);
  psramDelete(facts);
  psramFree(bufs);
  const LibraryBuilder::Result& r = stats_.build;
  if (useT && !r.transferUsed && r.transferWhy != cardcontract::Why::Ok) transferBad_ = true;
  if (r.noRecords) return walkCard();
  if (!r.built) {
    Serial.printf("[lib] the build from the records FAILED (%s)\n", r.noMemory ? "out of PSRAM" : "unreadable");
    return false;
  }
  Serial.printf("[lib] built from the records in %.0f ms: %lu tracks, %lu from the transfer's, %lu from the device's, "
                "%lu by their paths (%lu for the scan)%s%s; peak %u B of PSRAM\n",
                stats_.buildMs, (unsigned long)index_->trackCount(), (unsigned long)r.fromTransfer,
                (unsigned long)r.fromDevice, (unsigned long)r.fromPath, (unsigned long)r.pending,
                r.restarted ? "; restarted without a file that failed its checks" : "",
                useT && !r.transferUsed ? "; T left out" : "", (unsigned)index_->memory().buildPeak);
  softStale_ = false;
  return saveCard(libraryboot::inputsOf(root_->identity, r.transferUsed, store_->deviceCrc(), store_->journalSeq()));
}

bool Library::walkCard() {
  // No records at all: /music walked into a path-named index (the
  // validation walk then makes D, and the scan reads the tags).
  stats_.walk = Walk{};
  uint64_t signature = 0;
  if (!build(&signature)) return false;
  Serial.printf("[lib] /music walked: %lu files in %.0f ms (add %.0f ms, finish %.0f ms)\n",
                (unsigned long)stats_.walk.files, stats_.buildWalkMs + stats_.addMs, stats_.addMs, stats_.finishMs);
  softStale_ = false;
  return saveCard(libraryboot::inputsOf(root_->identity, false, store_->deviceCrc(), store_->journalSeq()));
}

bool Library::saveCard(const LibraryIndex::Inputs& inputs) {
  const int64_t t0 = esp_timer_get_time();
  bool ok = tagstore::prepareTmp(*fat_, kIdxNames);
  tagstore::File* f = ok ? fat_->open(kIdxNames.tmp, tagstore::Fs::Mode::Create) : nullptr;
  ok = f != nullptr;
  if (ok) {
    FileOut out(*f);
    ok = index_->save(out, inputs);
    ok = fat_->close(f) && ok;
  }
  ok = ok && tagstore::replace(*fat_, kIdxNames);
  if (!ok) fat_->remove(kIdxNames.tmp);
  stats_.saveMs = msSince(t0);
  if (!ok) Serial.println("[lib] couldn't save library.idx (the next boot builds again)");
  return true;  // the index is ready either way
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
  if (!records()) {
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
  const tagstore::TagStore::Opened opened = stats_.opened;
  stats_ = Stats{};
  stats_.card = true;
  stats_.opened = opened;
  stats_.cacheNote = "the update step";
  stats_.why = "the update step";
  stats_.action = libraryboot::Action::Build;
  stats_.internalFreeBefore = internalFree();
  const uint32_t psBefore = psramFreeNow();
  const bool records = (root_->present && !transferBad_) || store_->device().present || store_->hasJournals();
  const bool ok = records ? buildCard() : walkCard();
  stats_.psramUsed = static_cast<int32_t>(psBefore) - static_cast<int32_t>(psramFreeNow());
  stats_.internalDelta = static_cast<int32_t>(internalFree()) - static_cast<int32_t>(stats_.internalFreeBefore);
  return ok;
}

void Library::setOverlay(uint32_t track, const tagscan::Record& rec) {
  if (!overlay_ || !run_ || !index_ || !index_->ready()) return;
  rec.toRunFields(run_);
  const LibraryIndex::TagView v = LibraryBuilder::viewOf(rec.rec, run_, LibraryIndex::kFromDevice);
  overlay_->set(*index_, track, v);
}

bool Library::roomToBuild(size_t alsoFreed, char* why, size_t size) const {
  if (why && size) why[0] = 0;
  if (!index_) return false;
  const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  const LibraryIndex::Memory m = index_->memory();
  // The build's peak (ESTIMATED): the index it replaces and an eighth more
  // (3.4.4 measured 1.99 MB for 1.78 MB at 20k), the builder's own (about
  // 58 KB) and its buffers. L4 measures it.
  const size_t peak = m.total + m.total / 8 + 96 * 1024;
  const size_t room = freeNow + m.total + alsoFreed;
  if (room * 10 < peak * 11) {
    if (why) snprintf(why, size, "%u KB of PSRAM for a peak of about %u KB", (unsigned)(room / 1024), (unsigned)(peak / 1024));
    return false;
  }
  // The track table is one block: freed and asked again at about its size.
  const size_t table = static_cast<size_t>(index_->trackCount()) * sizeof(LibraryIndex::Track);
  if (std::max(largest, m.tracks) < table + table / 16) {
    if (why) snprintf(why, size, "the largest PSRAM block is %u KB, the track table %u KB", (unsigned)(largest / 1024),
                      (unsigned)(table / 1024));
    return false;
  }
  return true;
}

bool Library::deferToBoot() {
  if (!fat_) return false;
  tagstore::File* f = fat_->open(kMarker, tagstore::Fs::Mode::Create);
  return f && fat_->close(f);
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
