// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/QueueStore.h"

#include <Preferences.h>

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
// removed) in one go.
constexpr const char* kResumeKey = "resume";
struct ResumeBlob {
  uint32_t generation;
  int32_t entry;
  uint32_t pathHash;
  uint32_t positionMs;
  uint32_t durationMs;
};

QueueResume readResume() {
  QueueResume r;
  Preferences p;
  if (!p.begin(kNvsNamespace, true)) return r;
  ResumeBlob b;
  // (isKey() first: getBytesLength() of a missing key logs an [E] line at
  // every boot without a resume point.)
  if (p.isKey(kResumeKey) && p.getBytesLength(kResumeKey) == sizeof(b) &&
      p.getBytes(kResumeKey, &b, sizeof(b)) == sizeof(b)) {
    r.valid = true;
    r.generation = b.generation;
    r.entry = b.entry;
    r.pathHash = b.pathHash;
    r.positionMs = b.positionMs;
    r.durationMs = b.durationMs;
  }
  p.end();
  return r;
}

void mmss(uint32_t ms, char* buf, size_t size) {
  const uint32_t s = ms / 1000;
  snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

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
    Serial.printf("[queue] restored %lu of %lu tracks from %s (%lu no longer there), at %d of %lu (position from %s)\n",
                  (unsigned long)r.entries, (unsigned long)r.lines, path, (unsigned long)r.dropped,
                  queue_.current() + 1, (unsigned long)queue_.size(), fromNvs ? "NVS" : "the file");
    // The second it paused at: for this file's current line, if that track
    // is still the same file. Only shown and waiting: nothing plays.
    if (resume.valid) {
      char current[TrackCatalog::kMaxPath];
      catalog_.path(queue_.currentTrack(), current, sizeof(current));
      const int32_t line = fromNvs ? saved.position : r.header.current;
      char at[12];
      mmss(resume.positionMs, at, sizeof(at));
      if (QueueSaver::resumeApplies(resume, r.header.generation, line, r.currentKept, current)) {
        player_.setStartPoint(resume.positionMs, resume.durationMs);
        Serial.printf("[queue] resume point: %s into %d (stopped: play starts there)\n", at, queue_.current() + 1);
      } else {
        Serial.printf("[queue] resume point %s: not this queue's current track any more (dropped)\n", at);
      }
    }
    return true;
  }
  return false;
}

void QueueStore::loop(uint32_t nowMs) {
  if (!storage_.available()) return;
  noteTransport();
  saver_.loop(nowMs);
  noteFailures();
}

void QueueStore::noteTransport() {
  QueueSaver::Transport t;
  t.have = player_.resumePoint(&t.positionMs, &t.durationMs);
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
    const ResumeBlob b{r.generation, r.entry, r.pathHash, r.positionMs, r.durationMs};
    p.putBytes(kResumeKey, &b, sizeof(b));
  } else if (p.isKey(kResumeKey)) {
    p.remove(kResumeKey);
  }
  p.end();
  char at[12];
  mmss(r.positionMs, at, sizeof(at));
  if (r.valid) {
    Serial.printf("[queue] resume point saved: %s into %d\n", at, r.entry + 1);
  } else {
    Serial.println("[queue] resume point cleared (playback moved on)");
  }
}

bool QueueStore::remap(bool (*rebuild)(void* ctx), void* ctx) {
  saver_.abort();  // its lines would mix old ids and new
  // A start point waiting (the resume point after a boot) belongs to the
  // entry's key, which the read below gives afresh: carried across.
  uint32_t startMs = 0, startDurationMs = 0;
  const bool hadStart = player_.startPoint(&startMs, &startDurationMs);
  // The queue as paths while the old index can still name them.
  MemorySink text(psramAlloc, psramFree);
  const bool saved = queuetext::write(queue_, catalog_, saver_.generation(), text) && !text.failed();
  const bool ok = rebuild(ctx);
  if (!saved) {
    // Its ids now name other tracks, or none: better no queue than a wrong one.
    queue_.assign(nullptr, 0, -1);
    player_.queueReplaced(false);
    Serial.println("[queue] no PSRAM to carry the queue across the rebuild: cleared");
    return ok;
  }
  MemorySource in(text.data(), text.size());
  const queuetext::Restored r = queuetext::read(in, catalog_, queue_, nullptr, nullptr, psramAlloc, psramFree);
  if (!r.ok) {
    queue_.assign(nullptr, 0, -1);
    player_.queueReplaced(false);
    Serial.println("[queue] couldn't carry the queue across the rebuild: cleared");
    return ok;
  }
  // No library came of the rebuild (it failed: no PSRAM; or the walk found
  // nothing: the card went away): every library track was dropped. As in
  // restore(), that's not the user's queue changing: the file keeps it for
  // when the library is back (the next boot), and nothing else starts
  // playing (a built-in tone that survived, say).
  const LibraryIndex* index = catalog_.index();
  // (A rebuild that failed before touching the index leaves the ids as they
  // were: the queue reads back the same.)
  if (!index || !index->ready() || index->trackCount() == 0) {
    if (r.currentKept) {
      player_.queueReplaced(true);
      if (hadStart) player_.setStartPoint(startMs, startDurationMs);
    } else {
      player_.stop();
    }
    saver_.markSaved();
    Serial.printf("[queue] the rebuild left no library: %lu of %lu tracks here until it's back (queue.txt stays as "
                  "it was last saved)%s\n",
                  (unsigned long)r.entries, (unsigned long)r.lines, r.currentKept ? "" : "; stopped");
    return ok;
  }
  player_.queueReplaced(r.currentKept);
  if (hadStart && r.currentKept) player_.setStartPoint(startMs, startDurationMs);
  Serial.printf("[queue] after the rebuild: %lu of %lu tracks still there, at %d%s\n", (unsigned long)r.entries,
                (unsigned long)r.lines, queue_.current() + 1, r.currentKept ? "" : " (the current one is gone)");
  return ok;
}

void QueueStore::printStatus() const {
  static const char* const kEdits[] = {"none",   "play now",      "play next", "add",
                                       "remove", "move to next", "clear up next", "clear"};
  Serial.printf("[queue] %lu tracks, at %d, %lu up next; undo: %s; file generation %lu%s, %lu writes (last %lu ms), "
                "%lu failures\n",
                (unsigned long)queue_.size(), queue_.current() + 1, (unsigned long)queue_.upNext(),
                kEdits[static_cast<int>(queue_.undoable())], (unsigned long)saver_.generation(),
                saver_.writing() ? " (writing)" : saver_.contentDirty() ? " (to write)" : "",
                (unsigned long)saver_.writes(), (unsigned long)saver_.lastWriteMs(), (unsigned long)saver_.failures());
  // The resume point in NVS, and the player's start point (qs<sec>).
  const QueueResume& r = saver_.resume();
  char saved[48] = "none";
  if (r.valid) {
    char at[12];
    mmss(r.positionMs, at, sizeof(at));
    snprintf(saved, sizeof(saved), "%s into %d (generation %lu)", at, r.entry + 1, (unsigned long)r.generation);
  }
  uint32_t ms = 0, dur = 0;
  char start[24] = "none";
  if (player_.startPoint(&ms, &dur)) mmss(ms, start, sizeof(start));
  Serial.printf("[queue] resume point saved: %s; start point waiting: %s; %lu resume writes\n", saved, start,
                (unsigned long)saver_.resumeWrites());
}
