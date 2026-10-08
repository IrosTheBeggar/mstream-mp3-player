// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/TagConsole.h"

#include <esp_timer.h>

#include <cstring>
#include <ctime>

#include "CardContainer.h"
#include "CardManifest.h"
#include "CardTags.h"
#include "LibraryBuilder.h"
#include "QueueView.h"
#include "TagScan.h"
#include "TagStore.h"
#include "app/Psram.h"

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

// What a command reads with, in PSRAM.
struct TagConsole::Work {
  tagscan::Scanner scanner;
  cc::RunFields run;
  mptg::File file;
  cc::msmf::Manifest manifest;
  tagstore::BuilderRows rows;
  uint8_t buf[4096];
  uint8_t scratch[8192];
};

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

void TagConsole::command(const tagtext::Parsed& p) {
  using C = tagtext::Command;
  switch (p.command) {
    case C::Status: status(); return;
    case C::Dump: dump(p.path); return;
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
    case C::Build: job("build the library now (gb)", jobs_.build); return;
    case C::Verify: job("verify the transfer's files (gv)", jobs_.verify); return;
    default: Serial.printf("[tags] %s\n", tagtext::kHelp); return;
  }
}

void TagConsole::job(const char* what, bool (*fn)()) {
  if (!fn) {
    Serial.printf("[tags] %s: no card worker in this build yet (docs/METADATA.md N10, N12); nothing changed\n", what);
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
  *out = Found();
  fs::File f = storage_.fs().open(path, FILE_READ);
  if (!f || f.isDirectory()) return;
  out->file = true;
  FileAt src(f);
  if (device) {
    out->why = tagstore::openDevice(src, &out->device);
    if (out->why != cc::Why::Ok) return;
  }
  out->why = mptg::check(src, mptg::kUseHidx, w.scratch, sizeof(w.scratch));
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
  Found d;
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
    Found t;
    find(tpath, nullptr, false, *w, &t);
    if (!t.file) {
      Serial.printf("[tags] T: the root (generation %lu, commit %016llx) names %s, which ISN'T THERE\n",
                    static_cast<unsigned long>(gen), static_cast<unsigned long long>(commit), tpath);
    } else if (!t.valid) {
      Serial.printf("[tags] T: the root (generation %lu) names %s, ABSENT: it fails its checks (%s)\n",
                    static_cast<unsigned long>(gen), tpath, cc::whyName(t.why));
    } else {
      fs::File f = storage_.fs().open(tpath, FILE_READ);
      FileAt src(f);
      mptg::Info info;
      cc::Container c;
      mptg::openFile(c, src, &info);
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
  char path[TrackCatalog::kMaxPath];
  snprintf(path, sizeof(path), "%s", arg);
  size_t len = strlen(path);
  while (len && (path[len - 1] == ' ' || path[len - 1] == '\r' || path[len - 1] == '\n')) path[--len] = 0;
  if (strncmp(path, "/music/", 7) != 0 || len <= 7) {
    Serial.println("[tags] gt: a file under /music: gt/music/Artist/Album/01 - Title.mp3");
    return;
  }
  if (!storage_.available()) {
    Serial.println("[tags] no storage: no card, no tags");
    return;
  }
  const char* rel = path + 7;
  Work* w = psramNew<Work>();
  if (!w) {
    Serial.println("[tags] no PSRAM for the readers");
    return;
  }
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
  Found d;
  find(kDevicePath, rel, true, *w, &d);
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
  const tagstore::DeviceInfo device = d.device;
  const bool dValid = d.valid;
  const mptg::Record dRec = d.rec;
  const bool dPresent = d.present;
  const LibraryBuilder::Row dRow = d.row;
  // T.
  char tpath[64];
  uint64_t commit = 0;
  uint32_t gen = 0;
  Found tf;
  if (!transferFile(*w, tpath, sizeof(tpath), &commit, &gen)) {
    Serial.println("[tags] T: no transfer data");
  } else {
    find(tpath, rel, false, *w, &tf);
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
    char title[260];
    library_.catalog().title(id, title, sizeof(title));
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
