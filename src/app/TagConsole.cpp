// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/TagConsole.h"

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstring>
#include <ctime>
#include <new>

#include "CardContainer.h"
#include "CardManifest.h"
#include "CardTags.h"
#include "CardWalk.h"
#include "LibraryBuilder.h"
#include "QueueView.h"
#include "TagScan.h"
#include "TagStore.h"
#include "app/Psram.h"
#include "storage/CardFat.h"
#include "storage/SectorDisk.h"

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

namespace {

constexpr const char* kDevicePath = "/.player/tags.bin";

// A cardcontract::Source over an open file (the loop task's reads).
class FileAt : public cc::Source {
public:
  explicit FileAt(fs::File& f) : f_(f), size_(static_cast<uint32_t>(f.size())) {}
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (offset > size_ || n > size_ - offset) return false;
    if (f_.position() != offset && !f_.seek(offset)) return false;
    return f_.read(static_cast<uint8_t*>(out), n) == n;
  }

private:
  fs::File& f_;
  uint32_t size_;
};

void printLine(void*, const char* line) { Serial.printf("[tags] %s\n", line); }

// A file's size in bytes ("" when it isn't there).
void sizeOf(fs::FS& fs, const char* path, char* buf, size_t size) {
  fs::File f = fs.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    snprintf(buf, size, "none");
    return;
  }
  char n[16];
  snprintf(buf, size, "%s B", queueview::grouped(static_cast<uint32_t>(f.size()), n, sizeof(n)));
  f.close();
}

// The FAT time of a file's last write, as the VFS gave it: FatFs's fdate
// and ftime turned into a time_t by mktime() with no time zone set (the
// firmware sets none), so gmtime() gives them back.
uint32_t fatTimeOf(fs::File& f) {
  const time_t t = f.getLastWrite();
  if (t <= 0) return 0;
  struct tm tm;
  if (!gmtime_r(&t, &tm)) return 0;
  return cc::fatTime(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}

}  // namespace

// A record found in a tags file.
struct TagConsole::Found {
  bool file = false;    // the tags file is there
  bool valid = false;   // ... and passed its checks
  bool present = false; // ... and has a record for the path
  cc::Why why = cc::Why::Ok;
  mptg::Record rec;
  LibraryBuilder::Row row;
  tagstore::DeviceInfo device;  // D's header (device files only)
};

// What a command reads with, in PSRAM. Everything big is here, not on the
// loop task's 8 KB stack, which the card's reads below a command already
// take about 2 KB of (the VFS, FatFs's 512 B long-name buffer, the SD
// driver): the check's Walker alone is 3.9 KB (mptg::check() would put it
// on the stack, and gs overflowed the loop task's on N11's 20k card).
struct TagConsole::Work {
  tagscan::Scanner scanner;
  mptg::Walker walker;  // the check's walk (find())
  cc::RunFields run;
  mptg::File file;
  cc::msmf::Manifest manifest;
  tagstore::BuilderRows rows;
  Found d, t;  // D's and T's
  char path[TrackCatalog::kMaxPath];  // gt's file
  char title[260];
  uint8_t buf[4096];
  uint8_t scratch[8192];
};

void TagConsole::command(const tagtext::Parsed& p) {
  using C = tagtext::Command;
  switch (p.command) {
    case C::Status:
      if (p.n == 1) {
        // gs0: L3's figures one condition at a time.
        if (!jobs_.resetStats) {
          Serial.println("[tags] gs0: no card worker in this build; nothing changed");
          return;
        }
        jobs_.resetStats();
        Serial.println("[card] the scan's figures start now: gs's waits, each job's steps (mean, longest), the "
                       "worker's stack and internal RAM's lowest while a step ran");
        return;
      }
      if (jobs_.idle) jobs_.idle();
      status();
      return;
    case C::Dump:
      if (jobs_.idle) jobs_.idle();
      dump(p.path);
      return;
    case C::Cache: cache(p.n); return;
    case C::Bench: bench(p.n == 1); return;
    case C::Rescan:
    case C::RescanAll: {
      const bool all = p.command == C::RescanAll;
      if (!jobs_.rescan) {
        Serial.printf("[tags] %s: no scanner in this build yet (the card worker: docs/METADATA.md N10); nothing "
                      "changed\n",
                      all ? "Rescan everything (gr!)" : "Rescan tags (gr)");
        return;
      }
      const bool ok = jobs_.rescan(all);
      Serial.printf("[tags] %s: %s\n", all ? "Rescan everything, the transfer's files included" : "Rescan tags",
                    ok ? "asked: the scan reads the device's records again in the background" : "REFUSED");
      return;
    }
    case C::Walk: job("walk the card now (gw)", jobs_.walk); return;
    case C::Build:
      if (p.n == 1) {
        job("the update step, deferred to the next boot as a short PSRAM would (gb!)", jobs_.buildAtBoot);
      } else {
        job("build the library now (gb)", jobs_.build);
      }
      return;
    case C::Verify: job("verify the transfer's files (gv)", jobs_.verify); return;
    default: Serial.printf("[tags] %s\n", tagtext::kHelp); return;
  }
}

// ---- gc: the sector cache (storage/SectorDisk; METADATA.md 3.2.4, 6.3's L0 and L1) ----

void TagConsole::cache(uint32_t n) {
  if (!sectordisk::installed()) {
    Serial.printf("[cache] no sector cache: %s\n", !storage_.onCard() ? "no card (the flash)"
                                                  : MSTREAM_SECTOR_CACHE ? "no PSRAM for it at the mount"
                                                                         : "this build leaves it out (MSTREAM_SECTOR_CACHE=0)");
    return;
  }
  const bool flip = n <= 2;
  if (flip) {
    // The counts so far first (the reset is taken at the next disk call,
    // under FatFs's lock: read after it, they'd still be these).
    cacheCounts("up to the switch: ");
    sectordisk::setEnabled(n != 0);
    sectordisk::setVerify(n == 2);
    sectordisk::resetStats();
  }
  const sectordisk::Stats st = sectordisk::stats();
  Serial.printf("[cache] the sector cache: %s%s; %lu sectors (%u B of PSRAM), %lu held%s\n",
                sectordisk::enabled() ? "on" : "OFF (every read from the card)",
                sectordisk::verifying() ? ", every hit checked against the card" : "", (unsigned long)st.capacity,
                (unsigned)st.bytes, (unsigned long)st.held, flip ? " (the counts start now)" : "");
  if (!flip) cacheCounts("");
}

void TagConsole::cacheCounts(const char* when) {
  const sectordisk::Stats st = sectordisk::stats();
  const SectorCache::Stats& c = st.cache;
  const uint32_t reads = c.hits + c.misses;
  Serial.printf("[cache] %ssingle-sector reads %lu: %lu hits (%.1f%%), %lu misses; multi-sector %lu (%lu sectors); "
                "writes %lu (%lu sectors, %lu cached sectors refreshed, %lu failed); %lu dropped, %lu evicted\n",
                when, (unsigned long)reads, (unsigned long)c.hits, reads ? 100.0 * c.hits / reads : 0.0,
                (unsigned long)c.misses, (unsigned long)c.bypassed, (unsigned long)c.bypassedSectors,
                (unsigned long)c.writes, (unsigned long)c.writtenSectors, (unsigned long)c.updated,
                (unsigned long)c.failedWrites, (unsigned long)c.dropped, (unsigned long)c.evicted);
  Serial.printf("[cache] %sthe card: %lu reads (%lu of one sector; %lu sectors) in %.0f ms (%.2f ms a read), %lu "
                "writes, %lu trims; %lu mounts\n",
                when, (unsigned long)st.cardReads, (unsigned long)st.cardSingleReads, (unsigned long)st.cardReadSectors,
                st.cardReadUs / 1000.0,
                st.cardReads ? st.cardReadUs / 1000.0 / st.cardReads : 0.0, (unsigned long)st.cardWrites,
                (unsigned long)st.trims, (unsigned long)st.inits);
  if (sectordisk::verifying() || st.verified) {
    Serial.printf("[cache] %sverify: %lu hits checked, %lu STALE%s\n", when, (unsigned long)st.verified,
                  (unsigned long)st.stale, st.stale ? " (a cache bug: L1 fails)" : "");
    if (st.stale) Serial.printf("[cache] STALE: sector %lu was the last\n", (unsigned long)st.staleLba);
  }
}

// ---- gl: L0's bench (METADATA.md 6.3; 3.2.7's model: tools/fatmodel.py) ----

namespace {

// 3.2.3's walk with nothing written: CardWalk over FatFs into a sink that
// only counts (no D, no T).
class CountSink : public cardwalk::Sink {
public:
  bool folder(const char*, size_t, const cardwalk::FolderRow&) override { return true; }
  bool folderGone(const char*, size_t) override { return true; }
  bool file(const char*, size_t, cardwalk::Change, const cardwalk::FileRow&) override { return true; }
  bool fileGone(const char*, size_t) override { return true; }
  bool doubt(const char*, size_t, const cardwalk::Doubt&) override { return true; }
  bool rewindDoubts() override { return true; }
  Read nextDoubt(char*, size_t*, cardwalk::Doubt*) override { return Read::End; }
  bool finish(const cardwalk::Summary&) override { return true; }
  void abort() override {}
};

void countFile(const char*, void* ctx) { ++*static_cast<uint32_t*>(ctx); }

// The opens' lookups, in PSRAM (names and paths: 2.8 KB, which the loop
// task's stack can't spare over FatFs's calls).
struct BenchWork {
  FF_DIR dir;
  FILINFO fi;
  char names[4][256];  // the 1st, 353rd and 703rd entries' names, and the last so far
  char probe[3][300];  // each probe's path under /music
  char path[300];      // a probe's FatFs path
  char sub[256], file[256];
};

// The card's reads since `before` (every read that reached the SD driver,
// the cache on or off).
uint32_t readsSince(const sectordisk::Stats& before) { return sectordisk::stats().cardReads - before.cardReads; }

}  // namespace

void TagConsole::bench(bool walks) {
  if (!storage_.onCard() || !cardfat::ready() || !sectordisk::installed()) {
    Serial.println("[bench] a mounted card with the sector cache's wrapper (it counts the card's reads)");
    return;
  }
  if (jobs_.idle) jobs_.idle();
  Serial.println("[bench] L0: playback stopped, or the decoder's reads count too; the cache goes back as it was");
  const bool wasOn = sectordisk::enabled();
  // 1. The card's time per sector (the stock driver, never the cache).
  const sectordisk::Bench b = sectordisk::benchReads(200);
  Serial.printf("[bench] the card: %lu single-sector reads over the card, mean %.2f ms (fastest %.2f, slowest %.2f), "
                "%lu failed\n",
                (unsigned long)b.reads, b.meanUs / 1000.0, b.minUs / 1000.0, b.maxUs / 1000.0, (unsigned long)b.failed);
  // 2. The opens of a file under /music's 1st, 353rd and 703rd entries
  // (directory order; the last entry for a probe past a smaller folder's
  // end): the entry's first subfolder's first file (N11's card puts a plain
  // MP3 album first there), looked up as an open does, uncached, then the
  // cache cold (cleared), then warm. 3.2.7's model on N11's card: 4, 55 and
  // 108 reads uncached, give or take the album's own folder sectors.
  BenchWork* bw = psramNew<BenchWork>();
  FF_DIR* dir = bw ? &bw->dir : nullptr;
  FILINFO* fi = bw ? &bw->fi : nullptr;
  char* names = bw ? bw->names[0] : nullptr;
  char* probe = bw ? bw->probe[0] : nullptr;
  char* path = bw ? bw->path : nullptr;
  char music[16];
  const uint32_t want[3] = {1, 353, 703};
  uint32_t got[3] = {0, 0, 0}, entries = 0;
  if (bw && cardfat::musicPath("", 0, music, sizeof(music)) && f_opendir(dir, music) == FR_OK) {
    for (;;) {
      if (f_readdir(dir, fi) != FR_OK || fi->fname[0] == 0) break;
      ++entries;
      for (int k = 0; k < 3; ++k) {
        if (entries != want[k]) continue;
        snprintf(names + k * 256, 256, "%s", fi->fname);
        got[k] = entries;
      }
      snprintf(names + 3 * 256, 256, "%s", fi->fname);  // the last so far
    }
    f_closedir(dir);
    for (int k = 0; k < 3; ++k) {
      if (got[k] || !entries) continue;
      snprintf(names + k * 256, 256, "%s", names + 3 * 256);
      got[k] = entries;
    }
    // Each probe's path: down the first subfolder to the first file (or
    // the entry itself, when it is a file or holds nothing).
    for (int k = 0; k < 3; ++k) {
      char* rel = probe + k * 300;
      snprintf(rel, 300, "%s", names + k * 256);
      for (int depth = 0; depth < 2 && got[k]; ++depth) {
        if (!cardfat::musicPath(rel, strlen(rel), path, sizeof(bw->path)) || f_opendir(dir, path) != FR_OK) break;
        char* sub = bw->sub;
        char* file = bw->file;
        sub[0] = file[0] = 0;
        while (f_readdir(dir, fi) == FR_OK && fi->fname[0] != 0) {
          if (fi->fname[0] == '.') continue;
          if ((fi->fattrib & AM_DIR) && !sub[0]) snprintf(sub, sizeof(bw->sub), "%s", fi->fname);
          if (!(fi->fattrib & AM_DIR) && !file[0]) snprintf(file, sizeof(bw->file), "%s", fi->fname);
        }
        f_closedir(dir);
        const char* next = depth == 0 && sub[0] ? sub : file;
        if (!next[0]) break;
        const size_t n = strlen(rel);
        snprintf(rel + n, 300 - n, "/%s", next);
        if (next == file) break;
      }
    }
  }
  Serial.printf("[bench] /music has %lu entries\n", (unsigned long)entries);
  for (int k = 0; k < 3; ++k) {
    if (!got[k] || !bw) continue;
    const char* rel = probe + k * 300;
    if (!cardfat::musicPath(rel, strlen(rel), path, sizeof(bw->path))) continue;
    uint32_t reads[3];
    float ms[3];
    for (int pass = 0; pass < 3; ++pass) {
      // 0 uncached; 1 the cache cold (off, then on: cleared); 2 warm. A
      // look at the root first, so FatFs's own one-sector window holds
      // nothing of the path.
      if (pass == 0) sectordisk::setEnabled(false);
      if (pass == 1) sectordisk::setEnabled(true);
      char root[16];
      if (cardfat::fatPath("/.player", root, sizeof(root))) f_stat(root, fi);
      const sectordisk::Stats before = sectordisk::stats();
      const int64_t t0 = esp_timer_get_time();
      f_stat(path, fi);
      ms[pass] = (esp_timer_get_time() - t0) / 1000.0f;
      reads[pass] = readsSince(before);
    }
    Serial.printf("[bench] the open under entry %lu (probe %lu, %u levels): uncached %lu reads in %.1f ms; cached "
                  "cold %lu in %.1f ms, warm %lu in %.1f ms\n",
                  (unsigned long)got[k], (unsigned long)want[k], 1u + (unsigned)(strchr(rel, '/') != nullptr) +
                      (unsigned)(strchr(rel, '/') && strchr(strchr(rel, '/') + 1, '/') != nullptr),
                  (unsigned long)reads[0], ms[0], (unsigned long)reads[1], ms[1], (unsigned long)reads[2], ms[2]);
  }
  psramDelete(bw);
  // 3. The walks (glw): the stock one (POSIX readdir, forEachFile) and
  // 3.2.3's (CardWalk over FatFs), each uncached and cached (cleared).
  if (walks) {
    for (int w = 0; w < 2; ++w) {
      for (int cached = 0; cached < 2; ++cached) {
        sectordisk::setEnabled(false);
        if (cached) sectordisk::setEnabled(true);  // on again: cleared
        const sectordisk::Stats before = sectordisk::stats();
        const int64_t t0 = esp_timer_get_time();
        uint32_t files = 0, folders = 0;
        bool ok = true;
        if (w == 0) {
          storage_.forEachFile(countFile, &files, 8);
        } else {
          cardfat::FatCard* card = psramNew<cardfat::FatCard>();
          cardwalk::CardWalk* walk = psramNew<cardwalk::CardWalk>();
          CountSink* sink = psramNew<CountSink>();
          uint8_t* scratch = static_cast<uint8_t*>(psramAlloc(cardwalk::CardWalk::kDeviceScratch));
          ok = card && walk && sink && scratch && card->begin();
          if (ok) {
            cardwalk::CardWalk::Config c;
            c.lister = card;
            c.sink = sink;
            c.firstAfterCommit = true;
            c.scratch = scratch;
            c.scratchBytes = cardwalk::CardWalk::kDeviceScratch;
            ok = walk->begin(c) && walk->run() == cardwalk::CardWalk::State::Done;
            const cardwalk::CardWalk::Result& r = walk->result();
            files = r.audio + r.images + r.others;
            folders = r.folders;
          }
          psramDelete(card);
          psramDelete(walk);
          psramDelete(sink);
          psramFree(scratch);
        }
        const float ms = (esp_timer_get_time() - t0) / 1000.0f;
        Serial.printf("[bench] %s walk, %s: %lu files, %lu folders, in %.0f ms, %lu card reads%s\n",
                      w == 0 ? "the stock (forEachFile)" : "3.2.3's (CardWalk over FatFs)",
                      cached ? "cached" : "uncached", (unsigned long)files, (unsigned long)folders, ms,
                      (unsigned long)readsSince(before), ok ? "" : " (FAILED)");
      }
    }
  }
  sectordisk::setEnabled(wasOn);
  Serial.printf("[bench] PSRAM free %u B (largest block %u B), internal RAM free %u B\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void TagConsole::job(const char* what, bool (*fn)()) {
  if (!fn) {
    Serial.printf("[tags] %s: no card worker here (the flash, or no PSRAM for its jobs); nothing changed\n", what);
    return;
  }
  Serial.printf("[tags] %s: %s\n", what, fn() ? "asked" : "REFUSED");
}

void TagConsole::scanLine() {
  if (!jobs_.state) {
    Serial.println("[tags] the scan: none in this build yet (the card worker: docs/METADATA.md N10); the names come "
                   "from library.idx's build");
    return;
  }
  librarytext::Status s;
  char line[96] = "", text[96] = "";
  jobs_.state(&s, line, sizeof(line));
  librarytext::statusText(s, text, sizeof(text));
  Serial.printf("[tags] the scan: %s%s%s\n", text[0] ? text : "idle", line[0] ? "; " : "", line);
}

void TagConsole::summary() {
  const LibraryIndex* index = library_.index();
  if (index && index->ready()) {
    char t[192];
    tagtext::sourcesText(tagtext::countSources(*index), t, sizeof(t));
    Serial.printf("[tags] names: %s\n", t);
  }
  scanLine();
}

void TagConsole::find(const char* path, const char* rel, bool device, Work& w, Found* out) {
  new (out) Found();  // in place (`*out = Found()` builds one on the stack first)
  fs::File f = storage_.fs().open(path, FILE_READ);
  if (!f || f.isDirectory()) return;
  out->file = true;
  FileAt src(f);
  if (device) {
    out->why = tagstore::openDevice(src, &out->device);
    if (out->why != cc::Why::Ok) return;
  }
  // mptg::check()'s walk to the end, with Work's Walker.
  out->why = w.walker.begin(src, mptg::kUseHidx, w.scratch, sizeof(w.scratch));
  while (out->why == cc::Why::Ok) {
    const mptg::Walker::Step s = w.walker.next();
    if (s == mptg::Walker::Step::End) break;
    if (s == mptg::Walker::Step::Bad) out->why = w.walker.why();
  }
  if (out->why == cc::Why::Ok) out->why = w.file.open(src);
  if (out->why != cc::Why::Ok) return;
  out->valid = true;
  if (!rel) return;
  const uint32_t i = w.file.find(rel, strlen(rel));
  if (i == mptg::File::kNotFound || !w.file.record(i, &out->rec)) return;
  out->present = true;
  w.run.clear();
  if (out->rec.strings) w.file.run(out->rec.strings, &w.run);
  if (device && w.rows.begin(src, w.buf, sizeof(w.buf))) out->row = w.rows.row(i);
}

bool TagConsole::transferFile(Work& w, char* path, size_t size, uint64_t* commitId, uint32_t* generation) {
  cc::RootCandidate cand[2];
  const char* const names[2] = {"/.mstream/manifest.bin", "/.mstream/manifest.tmp"};
  for (int k = 0; k < 2; ++k) {
    fs::File f = storage_.fs().open(names[k], FILE_READ);
    if (!f || f.isDirectory()) continue;
    FileAt src(f);
    if (w.manifest.open(src, w.scratch, sizeof(w.scratch)) == cc::Why::Ok) cand[k] = w.manifest.candidate();
  }
  const cc::Election e = cc::elect(cand[0], cand[1]);
  if (e.pick == cc::Pick::None) return false;
  // The winner open again (the manifest object holds the last one opened).
  fs::File f = storage_.fs().open(names[e.pick == cc::Pick::Bin ? 0 : 1], FILE_READ);
  if (!f) return false;
  FileAt src(f);
  if (w.manifest.open(src, w.scratch, sizeof(w.scratch)) != cc::Why::Ok) return false;
  snprintf(path, size, "/.mstream/%s", w.manifest.tagsName());
  *commitId = w.manifest.commitId();
  *generation = w.manifest.frame().generation;
  return true;
}

void TagConsole::status() {
  if (!storage_.available()) {
    Serial.println("[tags] no storage: no card, no tags");
    return;
  }
  scanLine();
  if (jobs_.report) jobs_.report();
  cache(tagtext::kCacheReport);
  const LibraryIndex* index = library_.index();
  if (index && index->ready()) {
    char t[192];
    tagtext::sourcesText(tagtext::countSources(*index), t, sizeof(t));
    Serial.printf("[tags] the index's names: %s\n", t);
  }
  Work* w = psramNew<Work>();
  if (!w) {
    Serial.println("[tags] no PSRAM for the readers");
    return;
  }
  char n[16], m[16];
  // D, its rows by status, its journals.
  Found& d = w->d;
  find(kDevicePath, nullptr, true, *w, &d);
  if (!d.file) {
    Serial.printf("[tags] D %s: none (the device has read no tags on this card)\n", kDevicePath);
  } else if (!d.valid) {
    Serial.printf("[tags] D %s: ABSENT, it fails its checks (%s): the walk and the scan make it again\n", kDevicePath,
                  cc::whyName(d.why));
  } else {
    const tagstore::DeviceInfo& di = d.device;
    uint32_t count[4] = {};
    fs::File f = storage_.fs().open(kDevicePath, FILE_READ);
    FileAt src(f);
    if (w->rows.begin(src, w->buf, sizeof(w->buf))) {
      for (uint32_t i = 0; i < di.tags.recordCount; ++i) ++count[static_cast<int>(w->rows.row(i).status) & 3];
    }
    Serial.printf("[tags] D %s: %s records in %s folders, parser %u, rules %u, epoch %lu; %s\n", kDevicePath,
                  queueview::grouped(di.tags.recordCount, n, sizeof(n)), queueview::grouped(di.tags.folderCount, m, sizeof(m)),
                  static_cast<unsigned>(di.tags.parserVersion), static_cast<unsigned>(di.tags.readRules),
                  static_cast<unsigned long>(di.header.epoch),
                  di.header.walked ? "walked" : "not walked since its last change");
    if (di.header.walked) {
      Serial.printf("[tags]   the walk compared against %s (generation %lu, commit %016llx), skew %ld s\n",
                    di.header.walk.present ? "the transfer's root" : "no transfer data",
                    static_cast<unsigned long>(di.header.walk.generation),
                    static_cast<unsigned long long>(di.header.walk.commitId), static_cast<long>(di.header.skew));
    }
    using S = LibraryBuilder::Status;
    Serial.printf("[tags]   rows: %lu %s, %lu %s, %lu %s, %lu %s\n", static_cast<unsigned long>(count[0]),
                  tagtext::statusName(S::Scanned), static_cast<unsigned long>(count[1]), tagtext::statusName(S::Software),
                  static_cast<unsigned long>(count[2]), tagtext::statusName(S::Pending),
                  static_cast<unsigned long>(count[3]), tagtext::statusName(S::Unreadable));
  }
  char a[24], b[24];
  sizeOf(storage_.fs(), "/.player/tags.jnl", a, sizeof(a));
  sizeOf(storage_.fs(), "/.player/walk.jnl", b, sizeof(b));
  Serial.printf("[tags]   journals: tags.jnl %s, walk.jnl %s\n", a, b);
  // T: the root and its tags file.
  char tpath[64];
  uint64_t commit = 0;
  uint32_t gen = 0;
  if (!transferFile(*w, tpath, sizeof(tpath), &commit, &gen)) {
    Serial.println("[tags] T: no transfer data (no valid /.mstream/manifest.bin or manifest.tmp)");
  } else {
    Found& t = w->t;
    find(tpath, nullptr, false, *w, &t);
    if (!t.file) {
      Serial.printf("[tags] T: the root (generation %lu, commit %016llx) names %s, which ISN'T THERE\n",
                    static_cast<unsigned long>(gen), static_cast<unsigned long long>(commit), tpath);
    } else if (!t.valid) {
      Serial.printf("[tags] T: the root (generation %lu) names %s, ABSENT: it fails its checks (%s)\n",
                    static_cast<unsigned long>(gen), tpath, cc::whyName(t.why));
    } else {
      // find() opened it (its frame and header: mptg::openFile()).
      const mptg::Info& info = w->file.info();
      const bool matches = cc::msmf::companionMatches(w->manifest.tags(), info.frame, info.source, nullptr);
      Serial.printf("[tags] T: generation %lu, commit %016llx: %s, %s records in %s folders%s\n",
                    static_cast<unsigned long>(gen), static_cast<unsigned long long>(commit), tpath,
                    queueview::grouped(info.recordCount, n, sizeof(n)), queueview::grouped(info.folderCount, m, sizeof(m)),
                    matches ? "" : "; NOT the file the root names (its size or CRC differ): absent");
    }
  }
  // An unfinished transfer (2.12.5): its plan's presence.
  const bool plan = storage_.fs().exists("/.mstream/pending.bin") || storage_.fs().exists("/.mstream/pending.tmp");
  if (plan) Serial.println("[tags] a transfer's plan is on the card (pending.bin): the last transfer didn't finish");
  psramDelete(w);
}

void TagConsole::dump(const char* arg) {
  // The argument without its trailing blanks (a path's length at most).
  size_t len = strnlen(arg, TrackCatalog::kMaxPath - 1);
  while (len && (arg[len - 1] == ' ' || arg[len - 1] == '\r' || arg[len - 1] == '\n')) --len;
  if (strncmp(arg, "/music/", 7) != 0 || len <= 7) {
    Serial.println("[tags] gt: a file under /music: gt/music/Artist/Album/01 - Title.mp3");
    return;
  }
  if (!storage_.available()) {
    Serial.println("[tags] no storage: no card, no tags");
    return;
  }
  Work* w = psramNew<Work>();
  if (!w) {
    Serial.println("[tags] no PSRAM for the readers");
    return;
  }
  char* path = w->path;
  snprintf(path, sizeof(w->path), "%.*s", static_cast<int>(len), arg);
  const char* rel = path + 7;
  char n[16], t[48];
  // The file now.
  LibraryBuilder::Seen now;
  {
    fs::File f = storage_.fs().open(path, FILE_READ);
    if (!f || f.isDirectory()) {
      Serial.printf("[tags] %s: NOT FOUND on the card\n", path);
    } else {
      now.present = true;
      now.size = static_cast<uint32_t>(f.size());
      now.fatTime = fatTimeOf(f);
      tagtext::fatTimeText(now.fatTime, t, sizeof(t));
      Serial.printf("[tags] %s: %s B, written %s\n", path, queueview::grouped(now.size, n, sizeof(n)), t);
      const tagscan::Kind kind = tagscan::kindOf(path);
      if (kind == tagscan::Kind::Unknown) {
        Serial.println("[tags] not a track (.mp3, .flac, .opus): no tags read");
      } else {
        FileAt src(f);
        const int64_t t0 = esp_timer_get_time();
        const tagscan::Result r = w->scanner.scan(src, kind, w->buf, sizeof(w->buf));
        const float ms = static_cast<float>(esp_timer_get_time() - t0) / 1000.0f;
        static const char* const kResults[] = {"Ok", "UNREADABLE", "A READ ERROR", "Partial (the read budget ran out)"};
        const tagscan::Stats& st = w->scanner.stats();
        Serial.printf("[tags] read now (parser %u): %s, %lu reads, %s B, %.1f ms",
                      static_cast<unsigned>(tagscan::kParserVersion), kResults[static_cast<int>(r) & 3],
                      static_cast<unsigned long>(st.reads), queueview::grouped(st.bytes, n, sizeof(n)), ms);
        if (w->scanner.issues()) {
          Serial.printf(", issues 0x%04lx (tagscan::Issue)", static_cast<unsigned long>(w->scanner.issues()));
        }
        Serial.println();
        w->scanner.record().toRunFields(&w->run);
        tagtext::dumpRecord(w->scanner.record().rec, &w->run, printLine, nullptr);
      }
    }
  }
  // D.
  const Found& d = w->d;
  find(kDevicePath, rel, true, *w, &w->d);
  if (!d.file) {
    Serial.printf("[tags] D: none (%s isn't there)\n", kDevicePath);
  } else if (!d.valid) {
    Serial.printf("[tags] D: ABSENT, %s fails its checks (%s)\n", kDevicePath, cc::whyName(d.why));
  } else if (!d.present) {
    Serial.println("[tags] D: no row for it (the last walk didn't see it)");
  } else {
    tagtext::fatTimeText(d.rec.fatTime, t, sizeof(t));
    Serial.printf("[tags] D: %s%s, parser %u, its row at %s B, %s\n", tagtext::statusName(d.row.status),
                  d.row.confirmed ? " (T's qfp confirmed)" : "", static_cast<unsigned>(d.device.tags.parserVersion),
                  queueview::grouped(d.rec.size, n, sizeof(n)), t);
    if (d.row.status == LibraryBuilder::Status::Scanned || d.row.status == LibraryBuilder::Status::Unreadable) {
      tagtext::dumpRecord(d.rec, &w->run, printLine, nullptr);
    }
  }
  const tagstore::DeviceInfo& device = d.device;
  const bool dValid = d.valid;
  const mptg::Record& dRec = d.rec;
  const bool dPresent = d.present;
  const LibraryBuilder::Row dRow = d.row;
  // T.
  char tpath[64];
  uint64_t commit = 0;
  uint32_t gen = 0;
  const Found& tf = w->t;  // (nothing found, as the Work came: no transfer data)
  if (!transferFile(*w, tpath, sizeof(tpath), &commit, &gen)) {
    Serial.println("[tags] T: no transfer data");
  } else {
    find(tpath, rel, false, *w, &w->t);
    if (!tf.file || !tf.valid) {
      Serial.printf("[tags] T: %s ABSENT (%s)\n", tpath, tf.file ? cc::whyName(tf.why) : "not there");
    } else if (!tf.present) {
      Serial.printf("[tags] T: %s has no record for it (the listener's file)\n", tpath);
    } else {
      tagtext::fatTimeText(tf.rec.fatTime, t, sizeof(t));
      Serial.printf("[tags] T: %s: its record at %s B, %s\n", tpath, queueview::grouped(tf.rec.size, n, sizeof(n)), t);
      tagtext::dumpRecord(tf.rec, &w->run, printLine, nullptr);
    }
  }
  // The builder's pick (2.9), from D's row as the listing (the walk's sight
  // of the file) and T's record, as a build now would.
  LibraryBuilder::Seen ts, ds;
  if (tf.present) {
    ts.present = true;
    ts.size = tf.rec.size;
    ts.fatTime = tf.rec.fatTime;
    ts.flags = tf.rec.flags;
  }
  if (dValid && dPresent) {
    ds.present = true;
    ds.size = dRec.size;
    ds.fatTime = dRec.fatTime;
    ds.flags = dRec.flags;
  }
  const bool lists = tf.present && (!dValid || !device.header.walked || !device.header.walk.present ||
                                    device.header.walk.commitId != commit || device.header.walk.generation != gen);
  const bool parserOk = dValid && device.tags.parserVersion >= LibraryBuilder::kMinDeviceParser;
  const LibraryBuilder::Choice c =
      LibraryBuilder::choose(ts, ds, dRow, parserOk, dValid ? device.header.skew : 0, lists);
  Serial.printf("[tags] a build now takes %s%s%s\n", tagtext::pickName(c.pick), c.pending ? " (the scan should read it)" : "",
                lists ? "; T lists the card (no walk since its commit)" : "");
  if (ds.present && now.present && (ds.size != now.size || ds.fatTime != now.fatTime)) {
    Serial.println("[tags]   (the file changed since the last walk: the next walk sees it)");
  }
  // The index.
  const LibraryIndex* index = library_.index();
  const uint32_t id = index && index->ready() ? index->findTrack(path) : LibraryIndex::kNone;
  if (id == LibraryIndex::kNone) {
    Serial.println("[tags] the index: not in it");
  } else {
    const LibraryIndex::Track& tr = index->track(id);
    static const char* const kSources[4] = {"its path", "the device's record", "the transfer's record", "?"};
    char* title = w->title;
    library_.catalog().title(id, title, sizeof(w->title));
    tagtext::lengthText(static_cast<uint32_t>(tr.durationS) * 1000u, t, sizeof(t));
    Serial.printf("[tags] the index: track %lu, named by %s%s: \"%s\" by \"%s\", album \"%s\" (%u), line \"%s\", disc "
                  "%u, number %u, length %s\n",
                  static_cast<unsigned long>(id), kSources[tr.flags & LibraryIndex::kSourceMask],
                  (tr.flags & LibraryIndex::kTrackPending) ? ", the scan's to read" : "", title,
                  library_.catalog().artist(id), library_.catalog().album(id),
                  static_cast<unsigned>(library_.catalog().year(id)), library_.catalog().albumArtist(id),
                  static_cast<unsigned>(tr.disc), static_cast<unsigned>(tr.number), t);
  }
  psramDelete(w);
}
