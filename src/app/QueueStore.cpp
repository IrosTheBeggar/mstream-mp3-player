// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/QueueStore.h"

#include <Preferences.h>

#include "NvsLayout.h"
#include "QueueRemap.h"
#include "app/Psram.h"

namespace {

constexpr const char* kNvsNamespace = "queue";
constexpr size_t kBufSize = 2048;

struct SavedPosition {
  bool have = false;
  uint32_t generation = 0;
  int32_t position = -1;
};

SavedPosition readPosition() {
  SavedPosition s;
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return s;
  s.have = p.isKey("gen") && p.isKey("pos");
  s.generation = p.getUInt("gen", 0);
  s.position = p.getInt("pos", -1);
  p.end();
  return s;
}

// The position from NVS, when it was saved for this file's queue.
int32_t pickCurrent(const queuetext::Header& h, void* ctx) {
  const auto& s = *static_cast<const SavedPosition*>(ctx);
  return s.have && s.generation == h.generation ? s.position : h.current;
}

// The resume point as NVS keeps it: one blob, so it is written (or
// removed) in one go, its anchor with it; versioned (nvslayout: version 2,
// v0.5.0's version 1 and v0.5.0-beta.1's unversioned 20 bytes are still
// read, without an anchor).
constexpr const char* kResumeKey = "resume";

QueueResume readResume() {
  QueueResume r;
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return r;
  uint8_t b[nvslayout::kResumeMaxBytes];
  // (isKey() first: getBytesLength() of a missing key logs an [E] line at
  // every boot without a resume point.)
  const size_t n = p.isKey(kResumeKey) ? p.getBytesLength(kResumeKey) : 0;
  if (n > 0 && n <= sizeof(b) && p.getBytes(kResumeKey, b, n) == n && !nvslayout::decodeResume(b, n, &r)) {
    Serial.printf("[queue] resume point not read: a blob of %u bytes (version %u) this firmware doesn't know\n",
                  (unsigned)n, (unsigned)b[0]);
  }
  p.end();
  return r;
}

void mmss(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = ms / 1000;
  snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

const char* repeatName(uint8_t mode) { return mode == 2 ? "one" : mode == 1 ? "all" : "off"; }

}  // namespace

QueueStore::QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player,
                       const TrackCatalog& catalog)
    : storage_(storage), queue_(queue), player_(player), catalog_(catalog), saver_(*this, queue, catalog) {}

void QueueStore::paths(char* file, char* temp, size_t size) {
  const char* dir = storage_.stateDir();
  snprintf(file, size, "%s/queue.txt", dir);
  snprintf(temp, size, "%s/queue.tmp", dir);
}

bool QueueStore::restore() {
  if (!storage_.available()) return false;
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  SavedPosition saved = readPosition();
  const QueueResume resume = readResume();
  saver_.setGeneration(saved.generation);
  saver_.loadedResume(resume);  // (one that doesn't apply is removed at the first pass)
  // queue.tmp is only ever a whole file if power went between removing
  // queue.txt and renaming it: then it's the newest.
  for (const char* path : {file, temp}) {
    File f = storage_.fs().open(path, FILE_READ);
    if (!f) continue;
    FileSource src(f);
    const queuetext::Restored r = queuetext::read(src, catalog_, queue_, pickCurrent, &saved, psramAlloc, psramFree);
    f.close();
    if (!r.ok) {
      Serial.printf("[queue] %s: not a whole queue file, ignored\n", path);
      continue;
    }
    player_.queueReplaced(r.currentKept);
    // Tracks that are gone, or a file from the wrong name: write it again.
    // (Not for tracks dropped because there's no library at all this time,
    // a card that failed to read, say: the file keeps them for next time.)
    const LibraryIndex* index = catalog_.index();
    const bool rewrite = (r.dropped > 0 && index && index->ready()) || path == temp;
    const uint32_t generation =
        r.header.generation > saved.generation ? r.header.generation : saved.generation;
    saver_.loaded(generation, rewrite, millis());
    const bool fromNvs = saved.have && saved.generation == r.header.generation;
    Serial.printf("[queue] restored %lu of %lu tracks from %s (%lu no longer there), at %d of %lu (position from %s)%s\n",
                  (unsigned long)r.entries, (unsigned long)r.lines, path, (unsigned long)r.dropped,
                  queue_.current() + 1, (unsigned long)queue_.size(), fromNvs ? "NVS" : "the file",
                  r.header.shuffled ? ", shuffled" : "");
    // The second it paused at: for this file's current line, if that track
    // is still the same file. Only shown and waiting: nothing plays.
    if (resume.valid) {
      char current[TrackCatalog::kMaxPath];
      catalog_.path(queue_.currentTrack(), current, sizeof(current));
      const int32_t line = fromNvs ? saved.position : r.header.current;
      char at[12];
      mmss(resume.positionMs, at, sizeof(at));
      if (QueueSaver::resumeApplies(resume, r.header.generation, line, r.currentKept, current)) {
        player_.setStartPoint(resume.positionMs, resume.durationMs, &resume.anchor);
        char anchor[96];
        resumeanchor::describe(resume.anchor, anchor, sizeof(anchor));
        Serial.printf("[queue] resume point: %s into %d (stopped: play starts there); anchor: %s\n", at,
                      queue_.current() + 1, anchor);
      } else {
        Serial.printf("[queue] resume point %s: not this queue's current track any more (dropped)\n", at);
      }
    }
    return true;
  }
  return false;
}

PlaybackController::Repeat QueueStore::loadRepeat() {
  Preferences p;
  bool have = false;
  uint8_t stored = 0;
  if (p.begin(kNvsNamespace, true)) {
    // (isKey() first: reading a missing key logs an [E] line at every boot.)
    have = p.isKey(nvslayout::kRepeatKey);
    if (have) stored = p.getUChar(nvslayout::kRepeatKey, 0);
    p.end();
  }
  const uint8_t mode = nvslayout::repeatFrom(have, stored);
  if (!have) {
    Serial.printf("[queue] repeat: %s (none saved)\n", repeatName(mode));
  } else if (mode != stored) {
    Serial.printf("[queue] repeat: %s (an unknown %u saved)\n", repeatName(mode), (unsigned)stored);
  } else {
    Serial.printf("[queue] repeat: %s (saved)\n", repeatName(mode));
  }
  return static_cast<PlaybackController::Repeat>(mode);
}

void QueueStore::saveRepeat(PlaybackController::Repeat r) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) {
    Serial.println("[queue] repeat: NVS can't be opened: not saved");
    return;
  }
  if (p.putUChar(nvslayout::kRepeatKey, static_cast<uint8_t>(r)) != 1) Serial.println("[queue] repeat: NOT saved");
  p.end();
}

void QueueStore::loop(uint32_t nowMs) {
  if (!storage_.available()) return;
  noteTransport();
  saver_.loop(nowMs);
  noteFailures();
}

void QueueStore::noteTransport() {
  QueueSaver::Transport t;
  t.have = player_.resumePoint(&t.positionMs, &t.durationMs, &t.anchor);
  saver_.noteTransport(t);
}

bool QueueStore::flushNow() {
  if (!storage_.available()) return true;  // nothing is saved without storage
  const uint32_t t0 = millis();
  noteTransport();
  const bool wasWriting = saver_.writing(), wasDirty = saver_.contentDirty();
  const uint32_t writes = saver_.writes();
  const bool ok = saver_.flushNow(t0);
  noteFailures();
  Serial.printf("[queue] saved now%s%s in %lu ms: %s\n", wasWriting ? " (a write under way finished)" : "",
                saver_.writes() != writes ? ", the file written" : wasDirty ? "" : " (the file was up to date)",
                (unsigned long)(millis() - t0), ok ? "the card has the queue as it is" : "FAILED (the last file stays)");
  return ok;
}

void QueueStore::noteFailures() {
  if (saver_.failures() == failuresSeen_) return;
  failuresSeen_ = saver_.failures();
  Serial.printf("[queue] saving the queue failed (card full or gone?): trying again in %lu s\n",
                (unsigned long)(QueueSaver::kRetryMs / 1000));
}

ByteSink* QueueStore::openTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  if (!buf_) buf_ = static_cast<uint8_t*>(psramAlloc(kBufSize));
  if (buf_) file_ = storage_.fs().open(temp, FILE_WRITE);
  if (!buf_ || !file_) {
    Serial.printf("[queue] couldn't write %s\n", temp);
    return nullptr;
  }
  sink_.reset(&file_, buf_, kBufSize);
  return &sink_;
}

bool QueueStore::commitTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  bool ok = sink_.flush();
  file_.close();
  fs::FS& fs = storage_.fs();
  if (ok) {
    fs.remove(file);
    ok = fs.rename(temp, file);
  }
  if (!ok) {
    fs.remove(temp);
    Serial.printf("[queue] couldn't replace %s\n", file);
  }
  return ok;
}

void QueueStore::discardTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  file_.close();
  storage_.fs().remove(temp);
}

void QueueStore::savePosition(uint32_t generation, int32_t current) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  p.putUInt("gen", generation);
  p.putInt("pos", current);
  p.end();
}

void QueueStore::saveResume(const QueueResume& r) {
  Preferences p;
  if (!p.begin(kNvsNamespace, false)) return;
  if (r.valid) {
    uint8_t b[nvslayout::kResumeBytes];
    p.putBytes(kResumeKey, b, nvslayout::encodeResume(r, b));
  } else if (p.isKey(kResumeKey)) {
    p.remove(kResumeKey);
  }
  p.end();
  char at[12];
  mmss(r.positionMs, at, sizeof(at));
  if (r.valid) {
    char anchor[96];
    resumeanchor::describe(r.anchor, anchor, sizeof(anchor));
    Serial.printf("[queue] resume point saved: %s into %d; anchor: %s\n", at, r.entry + 1, anchor);
  } else {
    Serial.println("[queue] resume point cleared (playback moved on)");
  }
}

bool QueueStore::remap(bool (*rebuild)(void* ctx), void* ctx) {
  // The card and the caller's rebuild, for the portable sequence (flush,
  // free, rebuild, re-read: lib/core/QueueRemap, docs/METADATA.md 3.4.2).
  struct Card : queueremap::Card {
    Card(QueueStore& store, bool (*rebuild)(void*), void* ctx) : store_(store), rebuild_(rebuild), ctx_(ctx) {}
    bool flush() override { return store_.storage_.available() && store_.flushNow(); }
    ByteSource* openFile() override {
      char file[48], temp[48];
      store_.paths(file, temp, sizeof(file));
      file_ = store_.storage_.fs().open(file, FILE_READ);
      return file_ ? &source_ : nullptr;
    }
    void closeFile() override { file_.close(); }
    bool rebuild() override { return rebuild_(ctx_); }

    QueueStore& store_;
    bool (*rebuild_)(void*);
    void* ctx_;
    File file_;
    FileSource source_{file_};
  } card(*this, rebuild, ctx);

  const queueremap::Result r =
      queueremap::run(queue_, saver_, player_, catalog_, card, millis(), psramAlloc, psramFree);
  noteFailures();
  const queuetext::Restored& got = r.read;
  if (!got.ok) {
    const char* why = r.via == queueremap::Via::None ? "the card couldn't take queue.txt, and no PSRAM for its text"
                      : r.via == queueremap::Via::File ? "queue.txt couldn't be read back"
                                                       : "no PSRAM to read its text back";
    Serial.printf("[queue] couldn't carry the queue across the rebuild (%s): cleared; queue.txt keeps the last one "
                  "saved\n",
                  why);
    return r.rebuilt;
  }
  char via[96] = "through queue.txt";
  if (r.via == queueremap::Via::Memory) {
    snprintf(via, sizeof(via), "as %lu KB of text in PSRAM (the card couldn't take queue.txt)",
             (unsigned long)((r.textBytes + 1023) / 1024));
  }
  if (r.noLibrary) {
    Serial.printf("[queue] the rebuild left no library: %lu of %lu tracks here until it's back (queue.txt stays as "
                  "it was last saved)%s\n",
                  (unsigned long)got.entries, (unsigned long)got.lines, got.currentKept ? "" : "; stopped");
    return r.rebuilt;
  }
  Serial.printf("[queue] after the rebuild, carried %s: %lu of %lu tracks still there, at %d%s%s; the queue gave "
                "%lu KB to the rebuild and holds %lu KB\n",
                via, (unsigned long)got.entries, (unsigned long)got.lines, queue_.current() + 1,
                got.currentKept ? "" : " (the current one is gone)", r.startCarried ? " (its start point kept)" : "",
                (unsigned long)(r.freedBytes / 1024), (unsigned long)(queue_.memoryBytes() / 1024));
  return r.rebuilt;
}

void QueueStore::printStatus() const {
  static const char* const kEdits[] = {"none",   "play now",      "play next", "add",
                                       "remove", "move to next", "clear up next", "clear"};
  // An undo that would put the shuffle mode back too (Shuffle all's) says so.
  const char* undoMode = queue_.undoShuffled() == queue_.shuffled() ? ""
                         : queue_.undoShuffled()                    ? " (and shuffle on)"
                                                                    : " (and shuffle off)";
  Serial.printf("[queue] %lu tracks (%lu KB of PSRAM, the undo's included), at %d, %lu up next; shuffle %s, repeat "
                "%s; undo: %s%s; file generation %lu%s, %lu writes (last %lu ms), %lu failures\n",
                (unsigned long)queue_.size(), (unsigned long)((queue_.memoryBytes() + 1023) / 1024),
                queue_.current() + 1, (unsigned long)queue_.upNext(),
                queue_.shuffled() ? "on" : "off", repeatName(static_cast<uint8_t>(player_.repeat())),
                kEdits[static_cast<int>(queue_.undoable())], undoMode, (unsigned long)saver_.generation(),
                saver_.writing() ? " (writing)" : saver_.contentDirty() ? " (to write)" : "",
                (unsigned long)saver_.writes(), (unsigned long)saver_.lastWriteMs(), (unsigned long)saver_.failures());
  // The resume point in NVS, and the player's start point (qs<sec>).
  const QueueResume& r = saver_.resume();
  char saved[160] = "none";
  if (r.valid) {
    char at[16], anchor[96];
    const uint32_t ms = r.anchor.valid() ? resumeanchor::ms(r.anchor) : r.positionMs;
    snprintf(at, sizeof(at), "%lu:%02lu.%03lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60),
             (unsigned long)(ms % 1000));
    resumeanchor::describe(r.anchor, anchor, sizeof(anchor));
    snprintf(saved, sizeof(saved), "%s into %d (generation %lu): %s", at, r.entry + 1, (unsigned long)r.generation,
             anchor);
  }
  uint32_t ms = 0, dur = 0;
  ResumeAnchor anchor;
  char start[24] = "none";
  if (player_.startPoint(&ms, &dur, &anchor)) {
    mmss(ms, start, sizeof(start));
    if (anchor.valid()) snprintf(start + strlen(start), sizeof(start) - strlen(start), " (anchored)");
  }
  Serial.printf("[queue] resume point saved: %s; start point waiting: %s; %lu resume writes\n", saved, start,
                (unsigned long)saver_.resumeWrites());
}
