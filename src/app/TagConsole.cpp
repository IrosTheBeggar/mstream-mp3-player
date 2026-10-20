// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/TagConsole.h"

#include <SPI.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <new>

#include "CardContainer.h"
#include "CardManifest.h"
#include "CardTags.h"
#include "CardWalk.h"
#include "LibraryBuilder.h"
#include "QueueView.h"
#include "SdCrc16.h"
#include "TagCheck.h"
#include "TagScan.h"
#include "TagStore.h"
#include "UiStrings.h"
#include "UiText.h"
#include "app/LoopStack.h"
#include "app/Psram.h"
#include "sd_bench.h"   // lib/SD: the driver's write bench
#include "sd_diskio.h"  // lib/SD: the card's size
#include "storage/CardFat.h"
#include "storage/CardSpace.h"
#include "storage/SectorDisk.h"
#include "ff.h"
#include "diskio.h"  // FatFs: disk_read(), disk_write() for gx's raw CMD25s (after ff.h's ffconf names them)

namespace cc = cardcontract;
namespace mptg = cardcontract::mptg;

namespace {

constexpr const char* kDevicePath = "/.player/tags.bin";

// A cardcontract::Source over an open file (the loop task's reads); gs's
// over the file it keeps open across passes (reset()).
class FileAt : public cc::Source {
public:
  FileAt() = default;
  explicit FileAt(fs::File& f) { reset(f); }
  void reset(fs::File& f) {
    f_ = &f;
    size_ = static_cast<uint32_t>(f.size());
  }
  uint32_t size() const override { return size_; }
  bool read(uint32_t offset, void* out, uint32_t n) override {
    if (!f_ || offset > size_ || n > size_ - offset) return false;
    if (f_->position() != offset && !f_->seek(offset)) return false;
    return f_->read(static_cast<uint8_t*>(out), n) == n;
  }

private:
  fs::File* f_ = nullptr;
  uint32_t size_ = 0;
};

uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }

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
  mptg::Walker walker;  // the check's walk (find(); gs's check)
  cc::RunFields run;
  mptg::File file;
  cc::msmf::Manifest manifest;
  tagstore::BuilderRows rows;
  Found d, t;  // D's and T's
  char path[TrackCatalog::kMaxPath];  // gt's file
  char title[260];
  uint8_t buf[4096];
  uint8_t scratch[8192];
  // gs: the file its check reads (D, then T) across passes, and the check.
  fs::File handle;
  FileAt src;
  tagstore::RowTally tally;
  tagcheck::Check check;
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
      if (!gs_.idle()) {
        Serial.println("[tags] gs: the last one still reads the card's records (its lines follow): not run again");
        return;
      }
      status();  // (it waits for the worker's step under way itself, a pass at a time)
      return;
    case C::Dump:
      if (jobs_.idle) jobs_.idle();
      // A compaction still under way after that wait (33-48 s were seen,
      // 2026-10-14) puts tags.tmp in tags.bin's place as gt would read D:
      // it would call a whole D absent. gt reads in one go: not now.
      if (jobs_.replacing && jobs_.replacing()) {
        Serial.printf("[tags] gt: a compaction is under way (it replaces %s): not read now, try again once it's "
                      "done (gs says when)\n",
                      kDevicePath);
        return;
      }
      dump(p.path);
      return;
    case C::Cache: cache(p.n); return;
    case C::Bench: bench(p.n == 1); return;
    case C::WriteBench: writeBench(p.n); return;
    case C::WriteSoak: writeSoak(p.n); return;
    case C::CrcBench: crcBench(p.n); return;
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

// ---- gx, gxs: the card's write bench (docs/SWAP-MODE.md section 7; lib/SD's sd_bench.h) ----

namespace {
namespace xbench {

constexpr uint32_t kMb = 1024 * 1024;
// The pattern: the largest piece, and the read-back's chunk.
constexpr uint32_t kSrc = 64 * 1024;
// Each 4 KB block's first word is its offset in the file, salted: a block
// written at the wrong place, or one left from an earlier run, reads back
// wrong.
constexpr uint32_t kBlock = 4096;
// The next clock step above the mount's: 80 MHz / 3 (the HAL rounds down).
constexpr int kFastHz = 26666667;
constexpr const char* kFile = "/.player/xbench.tmp";

enum class Sync : uint8_t { End, EveryMb, EveryPiece };

struct Setting {
  const char* name;
  uint32_t piece;  // bytes, a multiple of kBlock, at most kSrc
  bool expand;     // f_expand first (opt 1: allocated now, one run of clusters)
  Sync sync;
  bool preErase;   // ACMD23 before each CMD25
  bool raw;        // CMD25s straight to the expanded file's sectors (no f_write)
  bool verify;     // read back and checked
};

// gx's runs: today's 4 KB pieces first, then one change at a time around
// the proposed receive path (32 KB pieces, f_expand, one sync at the end).
constexpr Setting kSweep[] = {
    {"4 KB pieces, plain (today's writers)", 4096, false, Sync::End, true, false, false},
    {"16 KB pieces, plain", 16384, false, Sync::End, true, false, false},
    {"32 KB pieces, plain", 32768, false, Sync::End, true, false, false},
    {"64 KB pieces, plain", 65536, false, Sync::End, true, false, false},
    {"32 KB pieces, f_expand (the receive path)", 32768, true, Sync::End, true, false, true},
    {"32 KB pieces, f_expand, a sync every MB", 32768, true, Sync::EveryMb, true, false, false},
    {"32 KB pieces, f_expand, a sync every piece", 32768, true, Sync::EveryPiece, true, false, false},
    {"32 KB pieces, f_expand, no ACMD23", 32768, true, Sync::End, false, false, false},
    {"32 KB raw CMD25s (no FatFs)", 32768, true, Sync::End, true, true, true},
};

// In PSRAM (about 137 KB): too big for the loop task's stack.
struct Work {
  FIL fil;
  uint8_t src[kSrc];   // the pattern, each piece stamped as it goes
  uint8_t back[kSrc];  // a chunk read back
  char path[48];
};

struct Result {
  bool ok = false;
  char why[96] = "";
  uint32_t bytes = 0, pieces = 0;
  uint64_t wallUs = 0;    // f_expand's start (or the first piece's) to the close's end
  uint64_t writeUs = 0;   // the pieces (f_write or raw CMD25s) and their syncs
  uint64_t expandUs = 0, closeUs = 0, verifyUs = 0;
  uint32_t p50 = 0, p99 = 0, max = 0;  // a piece, in µs
  bool fragmented = false;  // f_expand found no run of free clusters: plain writes
  bool verified = false;
  uint32_t badBlocks = 0;
  sd_bench_stats_t drv{};  // the writes' and the close's; crcErrors and retries to the read-back's end
  uint32_t cardWrites = 0, writeRetries = 0, writeFails = 0;  // the wrapper's (sectordisk)
};

void fillPattern(uint8_t* p, uint32_t n) {
  uint32_t x = 0x9E3779B9u;  // xorshift32
  for (uint32_t i = 0; i + 4 <= n; i += 4) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    memcpy(p + i, &x, 4);
  }
}

void stamp(uint8_t* piece, uint32_t offset, uint32_t n, uint32_t salt) {
  for (uint32_t b = 0; b < n; b += kBlock) {
    const uint32_t v = (offset + b) ^ salt;
    memcpy(piece + b, &v, 4);
  }
}

// The 4 KB blocks of a chunk read back from `offset` (a multiple of kSrc)
// that aren't what was written there.
uint32_t badBlocks(const uint8_t* chunk, const uint8_t* src, uint32_t offset, uint32_t n, uint32_t salt) {
  uint32_t bad = 0;
  for (uint32_t b = 0; b < n; b += kBlock) {
    uint32_t v;
    memcpy(&v, chunk + b, 4);
    if (v != ((offset + b) ^ salt) || memcmp(chunk + b + 4, src + b + 4, kBlock - 4) != 0) ++bad;
  }
  return bad;
}

// The expanded file's first sector on the drive: f_expand's opt 1 made its
// clusters one run.
LBA_t firstSector(const FIL& f) {
  const FATFS* fs = f.obj.fs;
  return fs->database + static_cast<LBA_t>(f.obj.sclust - 2) * fs->csize;
}

// Sectors of the bench's own file, under FatFs's volume lock as CardSpace's
// count reads (the wrapper's cache and the free count's watch see them as
// FatFs's own).
bool raw(FATFS* fs, bool write, uint8_t* p, LBA_t lba, UINT count) {
  if (!ff_mutex_take(fs->ldrv)) return false;
  const bool ok = (write ? disk_write(fs->pdrv, p, lba, count) : disk_read(fs->pdrv, p, lba, count)) == RES_OK;
  ff_mutex_give(fs->ldrv);
  return ok;
}

// The clock the bus runs for what the driver asks (the HAL's rounding).
uint32_t busHz(int asked) { return spiClockDivToFrequency(SPI.bus(), spiFrequencyToClockDiv(SPI.bus(), asked)); }

// One run: the file made, written as `s` says, closed, maybe read back; it
// is left on the card (the caller deletes it). `hz`: the clock for a raw
// run's own sectors (FatFs's reads and writes stay at the mount's).
void run(Work& w, uint32_t* times, const Setting& s, uint32_t bytes, uint32_t salt, int hz, uint8_t pdrv,
         Result& r) {
  r = Result{};
  r.bytes = bytes;
  f_unlink(w.path);  // (one an interrupted run left, usually none)
  FRESULT fr = f_open(&w.fil, w.path, FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
  if (fr != FR_OK) {
    snprintf(r.why, sizeof(r.why), "the open: FatFs %d", (int)fr);
    return;
  }
  const int mountHz = sdcard_frequency(pdrv);
  const sectordisk::Stats before = sectordisk::stats();
  sdcard_set_pre_erase(s.preErase);
  sdcard_bench(true);
  const int64_t t0 = esp_timer_get_time();
  bool contiguous = false;
  if (s.expand) {
    fr = f_expand(&w.fil, bytes, 1);
    r.expandUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
    if (fr == FR_OK) {
      contiguous = true;
    } else if (fr == FR_DENIED && !s.raw) {
      r.fragmented = true;
    } else {
      snprintf(r.why, sizeof(r.why), "f_expand: FatFs %d%s", (int)fr, fr == FR_DENIED ? " (no run of free clusters)" : "");
      f_close(&w.fil);
      sdcard_bench(false);
      sdcard_set_pre_erase(true);
      return;
    }
  }
  FATFS* fs = w.fil.obj.fs;
  const LBA_t base = contiguous ? firstSector(w.fil) : 0;
  bool ok = true;
  if (s.raw) sdcard_set_frequency(pdrv, hz);
  const uint32_t n = bytes / s.piece;
  for (uint32_t i = 0; i < n && ok; ++i) {
    const uint32_t off = i * s.piece;
    uint8_t* piece = w.src + off % kSrc;
    stamp(piece, off, s.piece, salt);
    const int64_t a = esp_timer_get_time();
    if (s.raw) {
      ok = raw(fs, true, piece, base + off / 512, s.piece / 512);
      if (!ok) snprintf(r.why, sizeof(r.why), "a raw write at %lu", (unsigned long)off);
    } else {
      UINT done = 0;
      fr = f_write(&w.fil, piece, s.piece, &done);
      ok = fr == FR_OK && done == s.piece;
      if (!ok) snprintf(r.why, sizeof(r.why), "f_write at %lu: FatFs %d, %u B", (unsigned long)off, (int)fr, done);
      const bool mb = (off + s.piece) % kMb == 0;
      if (ok && (s.sync == Sync::EveryPiece || (s.sync == Sync::EveryMb && mb))) {
        fr = f_sync(&w.fil);
        ok = fr == FR_OK;
        if (!ok) snprintf(r.why, sizeof(r.why), "f_sync at %lu: FatFs %d", (unsigned long)off, (int)fr);
      }
    }
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - a);
    times[i] = us;
    r.writeUs += us;
    r.pieces = i + 1;
  }
  sdcard_set_frequency(pdrv, mountHz);  // before anything of FatFs's own
  const int64_t c = esp_timer_get_time();
  fr = f_close(&w.fil);
  r.closeUs = static_cast<uint64_t>(esp_timer_get_time() - c);
  r.wallUs = static_cast<uint64_t>(esp_timer_get_time() - t0);
  if (ok && fr != FR_OK) {
    ok = false;
    snprintf(r.why, sizeof(r.why), "the close: FatFs %d", (int)fr);
  }
  r.drv = sdcard_bench_stats();
  sdcard_bench(false);
  sdcard_set_pre_erase(true);
  const sectordisk::Stats after = sectordisk::stats();
  r.cardWrites = after.cardWrites - before.cardWrites;
  r.writeRetries = after.writeRetries - before.writeRetries;
  r.writeFails = after.writeFails - before.writeFails;
  if (r.pieces) {
    std::sort(times, times + r.pieces);
    r.p50 = times[r.pieces / 2];
    r.p99 = times[std::min<uint32_t>(r.pieces - 1, r.pieces * 99 / 100)];
    r.max = times[r.pieces - 1];
  }
  r.ok = ok;
  if (!ok || !s.verify) return;
  // The read-back: a raw run's at its own clock (MISO's test), FatFs's
  // through f_read at the mount's.
  const sd_bench_stats_t drvBefore = sdcard_bench_stats();
  const int64_t v0 = esp_timer_get_time();
  if (s.raw) {
    sdcard_set_frequency(pdrv, hz);
    for (uint32_t off = 0; off < bytes && ok; off += kSrc) {
      ok = raw(fs, false, w.back, base + off / 512, kSrc / 512);
      if (ok) r.badBlocks += badBlocks(w.back, w.src, off, kSrc, salt);
    }
    sdcard_set_frequency(pdrv, mountHz);
  } else {
    ok = f_open(&w.fil, w.path, FA_READ) == FR_OK;
    for (uint32_t off = 0; off < bytes && ok; off += kSrc) {
      UINT got = 0;
      ok = f_read(&w.fil, w.back, kSrc, &got) == FR_OK && got == kSrc;
      if (ok) r.badBlocks += badBlocks(w.back, w.src, off, kSrc, salt);
    }
    f_close(&w.fil);
  }
  r.verifyUs = static_cast<uint64_t>(esp_timer_get_time() - v0);
  const sd_bench_stats_t drvAfter = sdcard_bench_stats();
  r.drv.crcErrors += drvAfter.crcErrors - drvBefore.crcErrors;
  r.drv.retries += drvAfter.retries - drvBefore.retries;
  r.verified = ok;
  if (!ok) snprintf(r.why, sizeof(r.why), "the read-back failed");
}

double mbps(uint32_t bytes, uint64_t us) { return us ? bytes / static_cast<double>(kMb) / (us / 1e6) : 0.0; }
double pct(uint64_t part, uint64_t whole) { return whole ? 100.0 * part / whole : 0.0; }

void print(const Setting& s, const Result& r) {
  if (!r.ok && !r.pieces) {
    Serial.printf("[xbench] %s: FAILED (%s)\n", s.name, r.why);
    return;
  }
  char expand[48] = "";
  if (r.expandUs) snprintf(expand, sizeof(expand), ", f_expand %.1f ms", r.expandUs / 1000.0);
  if (r.ok) {
    Serial.printf("[xbench] %s: %.1f MB at %.2f MB/s (%.2f s: the pieces %.2f s, the close %.1f ms%s%s); a piece "
                  "p50 %.2f ms, p99 %.2f, max %.2f\n",
                  s.name, r.bytes / static_cast<double>(kMb), mbps(r.bytes, r.wallUs), r.wallUs / 1e6,
                  r.writeUs / 1e6, r.closeUs / 1000.0, expand, r.fragmented ? "; FRAGMENTED: plain writes" : "",
                  r.p50 / 1000.0, r.p99 / 1000.0, r.max / 1000.0);
  } else {
    Serial.printf("[xbench] %s: FAILED at piece %lu of %lu (%s; %.1f ms)\n", s.name, (unsigned long)r.pieces,
                  (unsigned long)(r.bytes / s.piece), r.why, r.writeUs / 1000.0);
  }
  const sd_bench_stats_t& d = r.drv;
  // (The timers ran from before f_expand: its writes are in them.)
  const uint64_t timed = d.preEraseUs + d.crcUs + d.blockBusyUs + d.wireUs + d.stopBusyUs + d.statusUs;
  const uint64_t whole = r.expandUs + r.writeUs + r.closeUs;
  char refusal[48] = "";
  if (d.lastRefusal) snprintf(refusal, sizeof(refusal), " (the last refused block's response 0x%02x)", d.lastRefusal);
  Serial.printf("[xbench]   the driver: %lu CMD25s (%.1f blocks each); of the run's time: the wire %.0f%%, the wait "
                "before each block %.0f%%, each CMD25's end %.0f%% (the longest %.2f ms), ACMD23 %.0f%%, CRC16 %.0f%%, "
                "CMD13 %.0f%%, the rest (FatFs, the wrapper, the bus's lock) %.0f%%; %lu CRC errors, %lu retries%s; "
                "the wrapper: %lu writes (%lu again, %lu failed)\n",
                (unsigned long)d.multiWrites, d.multiWrites ? d.blocks / static_cast<double>(d.multiWrites) : 0.0,
                pct(d.wireUs, whole), pct(d.blockBusyUs, whole), pct(d.stopBusyUs, whole), d.maxStopBusyUs / 1000.0,
                pct(d.preEraseUs, whole), pct(d.crcUs, whole), pct(d.statusUs, whole),
                whole > timed ? pct(whole - timed, whole) : 0.0, (unsigned long)d.crcErrors, (unsigned long)d.retries,
                refusal, (unsigned long)r.cardWrites, (unsigned long)r.writeRetries, (unsigned long)r.writeFails);
  if (s.verify && r.ok) {
    Serial.printf("[xbench]   read back in %.2f s (%.2f MB/s): %s\n", r.verifyUs / 1e6, mbps(r.bytes, r.verifyUs),
                  !r.verified ? "FAILED" : r.badBlocks ? "BAD BLOCKS" : "every block as written");
    if (r.verified && r.badBlocks) Serial.printf("[xbench]   %lu of %lu blocks wrong\n", (unsigned long)r.badBlocks,
                                                 (unsigned long)(r.bytes / kBlock));
  }
  Serial.flush();
}

// The card, for the runs' context: its CID and CSD, the volume's layout,
// the free space and the clock.
void cardLines(uint8_t pdrv, const FATFS* fs) {
  uint8_t cid[16], csd[16];
  if (sdcard_read_cid(pdrv, cid)) {
    char oid[3], pnm[6];
    for (int i = 0; i < 2; ++i) oid[i] = cid[1 + i] >= 0x20 && cid[1 + i] < 0x7F ? static_cast<char>(cid[1 + i]) : '?';
    for (int i = 0; i < 5; ++i) pnm[i] = cid[3 + i] >= 0x20 && cid[3 + i] < 0x7F ? static_cast<char>(cid[3 + i]) : '?';
    oid[2] = pnm[5] = 0;
    Serial.printf("[xbench] the card's CID: maker 0x%02x, OEM \"%s\", product \"%s\" rev %u.%u, made %u-%02u\n",
                  cid[0], oid, pnm, cid[8] >> 4, cid[8] & 0x0F, 2000u + (((cid[13] & 0x0Fu) << 4) | (cid[14] >> 4)),
                  cid[14] & 0x0Fu);
  } else {
    Serial.println("[xbench] the card's CID: not read");
  }
  if (sdcard_read_csd(pdrv, csd)) {
    Serial.printf("[xbench] its CSD: version %u, TRAN_SPEED 0x%02x%s\n", (csd[0] >> 6) + 1u, csd[3],
                  csd[3] == 0x32 ? " (25 MHz)" : csd[3] == 0x5A ? " (50 MHz)" : "");
  }
  uint64_t spare = 0;
  char room[40];
  if (cardspace::freeBytes(&spare)) {
    snprintf(room, sizeof(room), "%.2f GB free", spare / 1e9);
  } else {
    snprintf(room, sizeof(room), "free space not counted");
  }
  if (fs) {
    const uint32_t au = (4u * kMb) / 512;  // a 4 MiB allocation unit, the common one
    Serial.printf("[xbench] the volume: %lu sectors on the card, FAT%s, %lu KB clusters, the data area from sector %lu "
                  "(%s 4 MiB boundary), %s\n",
                  (unsigned long)sdcard_num_sectors(pdrv), fs->fs_type == FS_FAT32 ? "32" : "16",
                  (unsigned long)(fs->csize * 512u / 1024u), (unsigned long)fs->database,
                  fs->database % au == 0 ? "on a" : "off a", room);
  }
  const int hz = sdcard_frequency(pdrv);
  Serial.printf("[xbench] the clock: the driver asks %d Hz, the bus runs %lu Hz; gxs's faster step %lu Hz; the data "
                "blocks' CRC16 %s\n",
                hz, (unsigned long)busHz(hz), (unsigned long)busHz(kFastHz),
                sdcard_crc16_fast() ? "two bytes a step (gxc1)" : "the stock one, a byte a step (gxc0)");
}

// The runs' common parts, in PSRAM: the Work and each piece's time.
struct Bench {
  Work* w = nullptr;
  uint32_t* times = nullptr;
  bool begin(uint32_t bytes) {
    w = psramNew<Work>();
    times = static_cast<uint32_t*>(psramAlloc((bytes / kBlock) * sizeof(uint32_t)));
    if (!w || !times) return false;
    fillPattern(w->src, kSrc);
    return cardfat::fatPath(kFile, w->path, sizeof(w->path));
  }
  ~Bench() {
    if (w) f_unlink(w->path);
    psramDelete(w);
    psramFree(times);
  }
  // The volume, for cardLines(): the file made and closed once.
  const FATFS* volume() {
    if (f_open(&w->fil, w->path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return nullptr;
    const FATFS* fs = w->fil.obj.fs;
    f_close(&w->fil);
    f_unlink(w->path);
    return fs;
  }
};

}  // namespace xbench
}  // namespace

bool TagConsole::writeBenchReady(const char* what, uint32_t bytes) {
  if (!storage_.onCard() || !cardfat::ready() || !sectordisk::installed()) {
    Serial.printf("[xbench] %s: a mounted card with the sector cache's wrapper (it counts the card's writes)\n", what);
    return false;
  }
  if (sectordisk::guard().foreign) {
    Serial.printf("[xbench] %s: another card is in: every write is refused until a restart\n", what);
    return false;
  }
  if (jobs_.playing && jobs_.playing()) {
    Serial.printf("[xbench] %s: not while music plays (the decoder shares the card and its lock): pause first\n", what);
    return false;
  }
  if (jobs_.idle) jobs_.idle();
  if (jobs_.busy && jobs_.busy()) {
    Serial.printf("[xbench] %s: the card worker's step is still under way after 10 s: try again\n", what);
    return false;
  }
  uint64_t spare = 0;
  if (cardspace::freeBytes(&spare) && spare < bytes + 16ull * xbench::kMb) {
    Serial.printf("[xbench] %s: %.1f MB free on the card, %lu MB wanted (and 16 MB to spare)\n", what,
                  spare / static_cast<double>(xbench::kMb), (unsigned long)(bytes / xbench::kMb));
    return false;
  }
  return true;
}

void TagConsole::writeBench(uint32_t mb) {
  using namespace xbench;
  const uint32_t bytes = mb * kMb;
  if (!writeBenchReady("gx", bytes)) return;
  Bench b;
  if (!b.begin(bytes)) {
    Serial.println("[xbench] gx: no PSRAM for the bench (about 140 KB, and 1 KB a MB)");
    return;
  }
  const uint8_t pdrv = cardfat::drive();
  cardLines(pdrv, b.volume());
  const uint32_t runs = sizeof(kSweep) / sizeof(kSweep[0]);
  Serial.printf("[xbench] gx: %lu runs of %lu MB to %s, the file deleted after each; the loop waits throughout "
                "(nothing is drawn)\n",
                (unsigned long)runs, (unsigned long)mb, kFile);
  Serial.flush();
  for (uint32_t i = 0; i < runs; ++i) {
    Result r;
    run(*b.w, b.times, kSweep[i], bytes, 0x5A17u * (i + 1), sdcard_frequency(pdrv), pdrv, r);
    f_unlink(b.w->path);
    print(kSweep[i], r);
  }
  Serial.println("[xbench] gx: done");
}

void TagConsole::writeSoak(uint32_t mb) {
  using namespace xbench;
  const uint32_t bytes = mb * kMb;
  if (!writeBenchReady("gxs", bytes)) return;
  Bench b;
  if (!b.begin(bytes)) {
    Serial.println("[xbench] gxs: no PSRAM for the bench (about 140 KB, and 1 KB a MB)");
    return;
  }
  const uint8_t pdrv = cardfat::drive();
  cardLines(pdrv, b.volume());
  const int mountHz = sdcard_frequency(pdrv);
  Serial.printf("[xbench] gxs: %lu MB written raw in 32 KB CMD25s inside one contiguous file and read back, at "
                "%lu Hz, then at %lu Hz (FatFs's own reads and writes stay at %lu Hz)\n",
                (unsigned long)mb, (unsigned long)busHz(mountHz), (unsigned long)busHz(kFastHz),
                (unsigned long)busHz(mountHz));
  Serial.flush();
  const Setting slow = {"raw 32 KB CMD25s at the mount's clock", 32768, true, Sync::End, true, true, true};
  const Setting fast = {"raw 32 KB CMD25s at 80/3 MHz", 32768, true, Sync::End, true, true, true};
  Result a, c;
  run(*b.w, b.times, slow, bytes, 0x51u, mountHz, pdrv, a);
  f_unlink(b.w->path);
  print(slow, a);
  run(*b.w, b.times, fast, bytes, 0xFA57u, kFastHz, pdrv, c);
  f_unlink(b.w->path);
  print(fast, c);
  const bool clean = c.ok && c.verified && !c.badBlocks && !c.drv.crcErrors && !c.drv.retries;
  if (!c.ok) {
    Serial.printf("[xbench] gxs: 80/3 MHz FAILED at piece %lu of %lu (%s; %lu retries, the last refused block's "
                  "response 0x%02x): the mount's clock is the card's on this board\n",
                  (unsigned long)c.pieces, (unsigned long)(c.bytes / fast.piece), c.why, (unsigned long)c.drv.retries,
                  c.drv.lastRefusal);
    return;
  }
  Serial.printf("[xbench] gxs: 80/3 MHz %s over %lu MB (%lu CRC errors, %lu retries, %lu bad blocks); writes %.2f "
                "against %.2f MB/s, reads %.2f against %.2f\n",
                clean ? "CLEAN" : "NOT CLEAN", (unsigned long)mb, (unsigned long)c.drv.crcErrors,
                (unsigned long)c.drv.retries, (unsigned long)c.badBlocks, mbps(c.bytes, c.wallUs),
                mbps(a.bytes, a.wallUs), mbps(c.bytes, c.verifyUs), mbps(a.bytes, a.verifyUs));
}

// ---- gxc: the data blocks' CRC16s, timed (docs/SWAP-MODE.md section 7.2) ----

extern "C" unsigned short CRC16(const char* data, int length);  // lib/SD's sd_diskio_crc.c: the stock one

namespace {
namespace crcbench {

// The driver's two-byte step (lib/core SdCrc16) again, its tables in flash
// (const): what keeping them in RAM is worth.
struct Tables {
  uint16_t t0[256];
  uint16_t t1[256];
};
constexpr Tables makeTables() {
  Tables t{};
  for (int i = 0; i < 256; ++i) {
    uint16_t c = static_cast<uint16_t>(i << 8);
    for (int bit = 0; bit < 8; ++bit) c = static_cast<uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
    t.t0[i] = c;
  }
  for (int i = 0; i < 256; ++i) t.t1[i] = static_cast<uint16_t>((t.t0[i] << 8) ^ t.t0[t.t0[i] >> 8]);
  return t;
}
const Tables kFlashTables = makeTables();

uint16_t stock(const uint8_t* p, size_t n) { return CRC16(reinterpret_cast<const char*>(p), static_cast<int>(n)); }
// CRC-16/XMODEM through the ROM: one '~' before and after (esp_rom_crc.h).
uint16_t rom(const uint8_t* p, size_t n) {
  return static_cast<uint16_t>(~esp_rom_crc16_be(static_cast<uint16_t>(~0u), p, static_cast<uint32_t>(n)));
}
uint16_t fast(const uint8_t* p, size_t n) { return sdcrc::crc16(p, n); }
uint16_t fastFlash(const uint8_t* p, size_t n) {
  uint16_t crc = 0;
  for (; n >= 2; p += 2, n -= 2) {
    const uint16_t x = static_cast<uint16_t>(crc ^ ((p[0] << 8) | p[1]));
    crc = static_cast<uint16_t>(kFlashTables.t1[x >> 8] ^ kFlashTables.t0[x & 0xFF]);
  }
  if (n) crc = static_cast<uint16_t>((crc << 8) ^ kFlashTables.t0[(crc >> 8) ^ p[0]]);
  return crc;
}

struct Variant {
  const char* name;
  uint16_t (*fn)(const uint8_t*, size_t);
};
constexpr Variant kVariants[] = {
    {"the stock table, a byte a step (lib/SD's CRC16(): the driver's before)", stock},
    {"the ROM's, a byte a step (esp_rom_crc16_be)", rom},
    {"two bytes a step, tables in RAM (lib/core SdCrc16: the driver's now)", fast},
    {"two bytes a step, tables in flash", fastFlash},
};

constexpr uint32_t kBlock = 512;
constexpr uint32_t kStream = 64 * 1024;  // twice the cache: every block's lines come from PSRAM
constexpr uint32_t kHotRounds = 2000;
constexpr uint32_t kStreamPasses = 8;

}  // namespace crcbench
}  // namespace

void TagConsole::crcBench(uint32_t n) {
  using namespace crcbench;
  if (n <= 1) {
    sdcard_set_crc16(n == 1);
    Serial.printf("[xbench] gxc%lu: the data blocks' CRC16 is now %s, until a restart\n", (unsigned long)n,
                  n ? "two bytes a step (the default)" : "the stock one, a byte a step (the driver's before)");
    return;
  }
  uint8_t* hot = static_cast<uint8_t*>(heap_caps_malloc(kBlock, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  uint8_t* stream = static_cast<uint8_t*>(psramAlloc(kStream));
  if (!hot || !stream) {
    Serial.println("[xbench] gxc: no memory for its buffers (512 B internal, 64 KB PSRAM)");
    heap_caps_free(hot);
    psramFree(stream);
    return;
  }
  uint32_t x = 0x1F2E3D4Cu;  // xorshift32
  for (uint32_t i = 0; i < kStream; ++i) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    stream[i] = static_cast<uint8_t>(x >> 24);
  }
  memcpy(hot, stream, kBlock);
  uint8_t ones[kBlock];
  memset(ones, 0xFF, sizeof(ones));
  const uint32_t mhz = getCpuFrequencyMhz();
  Serial.printf("[xbench] gxc: a 512 B block from internal RAM %lu times, and 512 B blocks streamed from %lu KB of "
                "PSRAM %lu times over; the CPU at %lu MHz; the driver's now: %s\n",
                (unsigned long)kHotRounds, (unsigned long)(kStream / 1024), (unsigned long)kStreamPasses,
                (unsigned long)mhz, sdcard_crc16_fast() ? "two bytes a step" : "the stock one");
  const uint16_t want = fast(hot, kBlock);
  bool agree = true;
  volatile uint16_t sink = 0;
  for (const Variant& v : kVariants) {
    // Each one's answers first: a block of 0xFF (the SD spec's 0x7FA1), the
    // hot block, and every streamed block against the two-byte one's.
    bool ok = v.fn(ones, kBlock) == 0x7FA1 && v.fn(hot, kBlock) == want;
    for (uint32_t off = 0; off < kStream && ok; off += kBlock) ok = v.fn(stream + off, kBlock) == fast(stream + off, kBlock);
    agree = agree && ok;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t k = 0; k < kHotRounds; ++k) sink = static_cast<uint16_t>(sink ^ v.fn(hot, kBlock));
    const double hotUs = (esp_timer_get_time() - t0) / static_cast<double>(kHotRounds);
    t0 = esp_timer_get_time();
    for (uint32_t pass = 0; pass < kStreamPasses; ++pass) {
      for (uint32_t off = 0; off < kStream; off += kBlock) sink = static_cast<uint16_t>(sink ^ v.fn(stream + off, kBlock));
    }
    const double streamUs = (esp_timer_get_time() - t0) / static_cast<double>(kStreamPasses * (kStream / kBlock));
    Serial.printf("[xbench]   %s: %.2f us a block from RAM (%.1f cycles a byte), %.2f us from PSRAM; %s\n", v.name,
                  hotUs, hotUs * mhz / kBlock, streamUs, ok ? "the same CRCs" : "DIFFERENT CRCs");
    Serial.flush();
  }
  (void)sink;
  Serial.printf("[xbench] gxc: %s\n", agree ? "every variant gave the same CRCs (and 0x7FA1 for 512 B of 0xFF)"
                                              : "the variants DISAGREE: see the lines above");
  heap_caps_free(hot);
  psramFree(stream);
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
  // The worker's part whole (208 B at most: five 5-digit counts, the
  // longest wait's name, the update step's clause with the safe point's
  // length; 96 B cut that clause off). No card read under it.
  char line[224] = "", text[96] = "";
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

// ---- gs: the lines that read no card, then D's and T's a piece a pass ----

void TagConsole::status() {
  if (!storage_.available()) {
    Serial.println("[tags] no storage: no card, no tags");
    return;
  }
  // The worker's step under way first, nothing handed after it; then no
  // new compaction until the last line (it would replace tags.bin under
  // the reads), and D not read while one is under way (tagcheck::Gs).
  const uint32_t now = millis();
  gsLeast_ = UINT32_MAX;
  gs_.begin(now, kWaitMs, *this);
  gs_.step(pass(now), *this);
  // Its [console] line at its end (loop()), its pieces' depth with this.
  if (!gs_.idle()) loopstack::holdLine();
}

tagcheck::Pass TagConsole::pass(uint32_t nowMs) const {
  tagcheck::Pass p;
  p.nowMs = nowMs;
  p.workerBusy = jobs_.busy && jobs_.busy();
  p.replacing = jobs_.replacing && jobs_.replacing();
  p.compactions = jobs_.compactions ? jobs_.compactions() : 0;
  // (FatFs's mounts again: an open file's handle gone. 0 without the
  // sector wrapper, as without a guard.)
  p.remounts = sectordisk::guard().remounts;
  p.listMoving = jobs_.listMoving && jobs_.listMoving();
  p.dark = jobs_.dark && jobs_.dark();
  return p;
}

void TagConsole::loop(uint32_t nowMs) {
  if (gs_.idle()) return;
  const tagcheck::Pass p = pass(nowMs);
  // No piece while a list moves on a lit screen: its frames first
  // (ScanScheduler's rule; in the dark none is seen moving).
  if (tagcheck::pieceWaits(p)) return;
  // Each piece's own depth: the stack under it painted again first.
  loopstack::arm();
  gs_.step(p, *this);  // a phase's lines, or a pass of D's or T's turn (kSliceUs of its check)
  const uint32_t left = loopstack::leftSinceArm();
  if (left < gsLeast_) gsLeast_ = left;
  if (gs_.idle()) loopstack::heldLine(gsLeast_);
}

void TagConsole::hold(tagcheck::Hold h) {
  if (jobs_.hold) jobs_.hold(h);
}

void TagConsole::firstLines() {
  scanLine();
  if (jobs_.report) jobs_.report();
  cache(tagtext::kCacheReport);
  const LibraryIndex* index = library_.index();
  if (index && index->ready()) {
    char t[192];
    tagtext::sourcesText(tagtext::countSources(*index), t, sizeof(t));
    Serial.printf("[tags] the index's names: %s\n", t);
  }
}

bool TagConsole::memory() {
  gsWork_ = psramNew<Work>();
  if (!gsWork_) Serial.println("[tags] no PSRAM for the readers");
  return gsWork_ != nullptr;
}

tagcheck::Check& TagConsole::check() { return gsWork_->check; }

bool TagConsole::open(bool device) {
  Work& w = *gsWork_;
  Found& f = device ? w.d : w.t;
  new (&f) Found();  // in place (`f = Found()` builds one on the stack first)
  w.handle = storage_.fs().open(device ? kDevicePath : gsPath_, FILE_READ);
  if (!w.handle || w.handle.isDirectory()) {
    w.handle.close();
    return false;
  }
  f.file = true;
  w.src.reset(w.handle);
  tagcheck::Memory m;
  m.walker = &w.walker;
  m.file = &w.file;
  m.rows = &w.tally;
  m.scratch = w.scratch;
  m.scratchBytes = sizeof(w.scratch);
  m.buf = w.buf;
  m.bufBytes = sizeof(w.buf);
  tagcheck::Config c;
  c.sliceUs = kSliceUs;
  c.nowUs = nowUs;
  w.check.begin(w.src, device, m, c);
  return true;
}

void TagConsole::close() { gsWork_->handle.close(); }

void TagConsole::lines(bool device, bool read) {
  Work& w = *gsWork_;
  if (read) {
    Found& f = device ? w.d : w.t;
    f.why = w.check.why();
    f.valid = w.check.valid();
    if (device) f.device = w.check.device();
  }
  if (device) {
    deviceLines();
  } else {
    transferLine();
  }
}

void TagConsole::waiting(bool device) {
  // (D's alone: a compaction replaces nothing else.)
  Serial.printf("[tags] %s %s: waiting for the compaction under way (it replaces it at its end): read once it's "
                "done\n",
                device ? "D" : "T", device ? kDevicePath : gsPath_);
}

void TagConsole::again(bool device, const tagcheck::Turn& t) {
  Serial.printf("[tags] %s %s: %s as gs read it: read again\n", device ? "D" : "T", device ? kDevicePath : gsPath_,
                t.remounted() ? "the card mounted again (its open file gone)" : "a compaction replaced it");
}

void TagConsole::notRead(bool device, const tagcheck::Turn& t) {
  const char* name = device ? "D" : "T";
  const char* path = device ? kDevicePath : gsPath_;
  if (t.waitedOut()) {
    Serial.printf("[tags] %s %s: NOT READ: a compaction still replaces it after %lu s (gs again once it's done)\n",
                  name, path, static_cast<unsigned long>(tagcheck::kReplaceWaitMs / 1000));
  } else {
    Serial.printf("[tags] %s %s: NOT READ: %s as gs read it, %lu times (gs again later)\n", name, path,
                  t.remounted() ? "the card mounted again" : "replaced", static_cast<unsigned long>(t.rereads()));
  }
}

void TagConsole::journals() {
  char a[24], b[24];
  sizeOf(storage_.fs(), "/.player/tags.jnl", a, sizeof(a));
  sizeOf(storage_.fs(), "/.player/walk.jnl", b, sizeof(b));
  Serial.printf("[tags]   journals: tags.jnl %s, walk.jnl %s\n", a, b);
}

bool TagConsole::root() {
  if (transferFile(*gsWork_, gsPath_, sizeof(gsPath_), &gsCommit_, &gsGeneration_)) return true;
  Serial.println("[tags] T: no transfer data (no valid /.mstream/manifest.bin or manifest.tmp)");
  return false;
}

void TagConsole::plan() {
  // An unfinished transfer (2.12.5): its plan's presence.
  const bool pending = storage_.fs().exists("/.mstream/pending.bin") || storage_.fs().exists("/.mstream/pending.tmp");
  if (pending) Serial.println("[tags] a transfer's plan is on the card (pending.bin): the last transfer didn't finish");
}

void TagConsole::end() {
  psramDelete(gsWork_);  // (its file closed)
  gsWork_ = nullptr;
}

void TagConsole::deviceLines() {
  const Work& w = *gsWork_;
  const Found& d = w.d;
  if (!d.file) {
    Serial.printf("[tags] D %s: none (the device has read no tags on this card)\n", kDevicePath);
    return;
  }
  if (!d.valid) {
    Serial.printf("[tags] D %s: ABSENT, it fails its checks (%s): the walk and the scan make it again\n", kDevicePath,
                  cc::whyName(d.why));
    return;
  }
  char n[16], m[16];
  const tagstore::DeviceInfo& di = d.device;
  const uint32_t* count = w.check.rows();  // (all 0 when DSTA fails, as BuilderRows' refusal)
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

void TagConsole::transferLine() {
  const Work& w = *gsWork_;
  const Found& t = w.t;
  const uint32_t gen = gsGeneration_;
  if (!t.file) {
    Serial.printf("[tags] T: the root (generation %lu, commit %016llx) names %s, which ISN'T THERE\n",
                  static_cast<unsigned long>(gen), static_cast<unsigned long long>(gsCommit_), gsPath_);
  } else if (!t.valid) {
    Serial.printf("[tags] T: the root (generation %lu) names %s, ABSENT: it fails its checks (%s)\n",
                  static_cast<unsigned long>(gen), gsPath_, cc::whyName(t.why));
  } else {
    // The check opened it (its frame and header: mptg::openFile()).
    char n[16], m[16];
    const mptg::Info& info = w.file.info();
    const bool matches = cc::msmf::companionMatches(w.manifest.tags(), info.frame, info.source, nullptr);
    Serial.printf("[tags] T: generation %lu, commit %016llx: %s, %s records in %s folders%s\n",
                  static_cast<unsigned long>(gen), static_cast<unsigned long long>(gsCommit_), gsPath_,
                  queueview::grouped(info.recordCount, n, sizeof(n)), queueview::grouped(info.folderCount, m, sizeof(m)),
                  matches ? "" : "; NOT the file the root names (its size or CRC differ): absent");
  }
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
    // (An artist folder's own tracks have no album: "(loose tracks)", as the
    // Library calls them in English.)
    const char* album = library_.catalog().album(id);
    // The console speaks English: a compilation's line as library.idx holds
    // it ("Various Artists"), where the catalog gives the UI's words for it
    // (librarytext::albumArtist() returns that very text, so the pointer
    // says which; a track's own artist is the index's, never it).
    const char* various = uistr::tr(uistr::S::library_various_artists);
    const char* artist = library_.catalog().artist(id);
    const char* line = library_.catalog().albumArtist(id);
    if (artist == various) artist = index->albumArtistLine(tr.album);
    if (line == various) line = index->albumArtistLine(tr.album);
    tagtext::lengthText(static_cast<uint32_t>(tr.durationS) * 1000u, t, sizeof(t));
    Serial.printf("[tags] the index: track %lu, named by %s%s: \"%s\" by \"%s\", album \"%s\" (%u), line \"%s\", disc "
                  "%u, number %u, length %s\n",
                  static_cast<unsigned long>(id), kSources[tr.flags & LibraryIndex::kSourceMask],
                  (tr.flags & LibraryIndex::kTrackPending) ? ", the scan's to read" : "", title, artist,
                  album[0] ? album : "(loose tracks)", static_cast<unsigned>(library_.catalog().year(id)), line,
                  static_cast<unsigned>(tr.disc), static_cast<unsigned>(tr.number), t);
  }
  psramDelete(w);
}
