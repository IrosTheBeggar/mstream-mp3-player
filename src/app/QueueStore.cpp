#include "app/QueueStore.h"

#include <Preferences.h>

#include "app/Psram.h"

namespace {

constexpr const char* kNvsNamespace = "queue";
constexpr uint32_t kContentDelayMs = 2000;   // after the last edit
constexpr uint32_t kPositionDelayMs = 1000;  // after the last move
constexpr uint32_t kRetryMs = 10000;         // after a failed write
constexpr uint32_t kLinesPerPass = 32;       // ~2 KB of paths
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

bool timeFor(uint32_t now, uint32_t since, uint32_t delay) { return now - since >= delay; }

}  // namespace

QueueStore::QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player,
                       const TrackCatalog& catalog)
    : storage_(storage), queue_(queue), player_(player), catalog_(catalog) {}

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
  generation_ = saved.generation;
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
    if (r.header.generation > generation_) generation_ = r.header.generation;
    player_.queueReplaced(r.currentKept);
    savedContent_ = lastContent_ = queue_.contentVersion();
    savedPosition_ = lastPosition_ = queue_.positionVersion();
    // Tracks that are gone, or a file from the wrong name: write it again.
    // (Not for tracks dropped because there's no library at all this time,
    // a card that failed to read, say: the file keeps them for next time.)
    const LibraryIndex* index = catalog_.index();
    contentDirty_ = (r.dropped > 0 && index && index->ready()) || path == temp;
    contentChangedMs_ = millis();
    const bool fromNvs = saved.have && saved.generation == r.header.generation;
    Serial.printf("[queue] restored %lu of %lu tracks from %s (%lu no longer there), at %d of %lu (position from %s)\n",
                  (unsigned long)r.entries, (unsigned long)r.lines, path, (unsigned long)r.dropped,
                  queue_.current() + 1, (unsigned long)queue_.size(), fromNvs ? "NVS" : "the file");
    return true;
  }
  return false;
}

void QueueStore::loop(uint32_t nowMs) {
  if (!storage_.available()) return;
  const uint32_t cv = queue_.contentVersion();
  const uint32_t pv = queue_.positionVersion();
  if (cv != lastContent_) {
    lastContent_ = cv;
    contentChangedMs_ = nowMs;
    contentDirty_ = cv != savedContent_;
  }
  if (pv != lastPosition_) {
    lastPosition_ = pv;
    positionChangedMs_ = nowMs;
    positionDirty_ = pv != savedPosition_;
  }
  if (writing_) {
    stepWrite();
    return;
  }
  if (contentDirty_ && timeFor(nowMs, contentChangedMs_, kContentDelayMs) &&
      static_cast<int32_t>(nowMs - nextTryMs_) >= 0) {
    startWrite();
    return;
  }
  // A position only means something for the queue the file holds.
  if (positionDirty_ && !contentDirty_ && timeFor(nowMs, positionChangedMs_, kPositionDelayMs)) savePosition();
}

void QueueStore::startWrite() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  if (!buf_) buf_ = static_cast<uint8_t*>(psramAlloc(kBufSize));
  if (buf_) file_ = storage_.fs().open(temp, FILE_WRITE);
  if (!buf_ || !file_) {
    ++failures_;
    nextTryMs_ = millis() + kRetryMs;
    Serial.printf("[queue] couldn't write %s (retrying in %lu s)\n", temp, (unsigned long)(kRetryMs / 1000));
    return;
  }
  sink_.reset(&file_, buf_, kBufSize);
  writeGeneration_ = generation_ + 1;
  writer_.begin(queue_, writeGeneration_);
  writeStartMs_ = millis();
  writing_ = true;
  stepWrite();
}

void QueueStore::stepWrite() {
  switch (writer_.step(queue_, catalog_, sink_, kLinesPerPass)) {
    case queuetext::Writer::Step::More:
      break;
    case queuetext::Writer::Step::Done:
      finishWrite();
      break;
    case queuetext::Writer::Step::Changed:
      abortWrite();  // edited meanwhile: written again once it settles
      break;
    case queuetext::Writer::Step::Failed:
      abortWrite();
      ++failures_;
      nextTryMs_ = millis() + kRetryMs;
      Serial.println("[queue] writing the queue file failed (card full or gone?)");
      break;
  }
}

void QueueStore::finishWrite() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  bool ok = sink_.flush();
  file_.close();
  writing_ = false;
  fs::FS& fs = storage_.fs();
  if (ok) {
    fs.remove(file);
    ok = fs.rename(temp, file);
  }
  if (!ok) {
    fs.remove(temp);
    ++failures_;
    nextTryMs_ = millis() + kRetryMs;
    Serial.printf("[queue] couldn't replace %s\n", file);
    return;
  }
  generation_ = writeGeneration_;
  savedContent_ = queue_.contentVersion();  // Done: unchanged since begin()
  contentDirty_ = false;
  ++writes_;
  lastWriteMs_ = millis() - writeStartMs_;
  savePosition();  // paired with this generation from now on
}

void QueueStore::abortWrite() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  file_.close();
  storage_.fs().remove(temp);
  writing_ = false;
}

void QueueStore::savePosition() {
  Preferences p;
  if (p.begin(kNvsNamespace, false)) {
    p.putUInt("gen", generation_);
    p.putInt("pos", queue_.current());
    p.end();
  }
  savedPosition_ = queue_.positionVersion();
  positionDirty_ = false;
}

bool QueueStore::remap(bool (*rebuild)(void* ctx), void* ctx) {
  if (writing_) abortWrite();  // its lines would mix old ids and new
  // The queue as paths while the old index can still name them.
  MemorySink text(psramAlloc, psramFree);
  const bool saved = queuetext::write(queue_, catalog_, generation_, text) && !text.failed();
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
    } else {
      player_.stop();
    }
    savedContent_ = lastContent_ = queue_.contentVersion();
    savedPosition_ = lastPosition_ = queue_.positionVersion();
    contentDirty_ = positionDirty_ = false;
    Serial.printf("[queue] the rebuild left no library: %lu of %lu tracks here until it's back (queue.txt stays as "
                  "it was last saved)%s\n",
                  (unsigned long)r.entries, (unsigned long)r.lines, r.currentKept ? "" : "; stopped");
    return ok;
  }
  player_.queueReplaced(r.currentKept);
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
                kEdits[static_cast<int>(queue_.undoable())], (unsigned long)generation_,
                writing_ ? " (writing)" : contentDirty_ ? " (to write)" : "", (unsigned long)writes_,
                (unsigned long)lastWriteMs_, (unsigned long)failures_);
}
