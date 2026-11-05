// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "app/QueueStore.h"

#include <Preferences.h>

#include "NvsLayout.h"
#include "QueueRemap.h"
#include "app/NvsLater.h"
#include "app/Psram.h"

namespace {

constexpr const char* kNvsNamespace = "queue";
constexpr size_t kBufSize = 2048;
// The queue's files on the card, for 2.12.6's cut-rename rule (lib/core TagStore): its twins
// are /.player/queue.xl1 to .xl9. (LocalStorage::stateDir() is /.player.)
const tagstore::Names kQueueNames{"/.player/queue.txt", "/.player/queue.tmp", "/.player/queue"};

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

uint32_t lookupClock() { return millis(); }

}  // namespace

QueueStore::QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player,
                       const TrackCatalog& catalog, TrackCatalog::Unlisted& unlisted)
    : storage_(storage), queue_(queue), player_(player), catalog_(catalog),
      saver_(*this, queue, catalog, psramAlloc, psramFree), unlisted_(unlisted), lookup_(fat_, unlisted) {}

void QueueStore::paths(char* file, char* temp, size_t size) {
  const char* dir = storage_.stateDir();
  snprintf(file, size, "%s/queue.txt", dir);
  snprintf(temp, size, "%s/queue.tmp", dir);
}

// The queue's files, read: FatFs's answer on the card; on the flash the VFS, which gives no
// reason for a failed open or read.
struct QueueStore::QueueFiles final : queuefile::Files {
  explicit QueueFiles(QueueStore& store) : store_(store), fat_(store.fat_) {}
  ByteSource* open(const char* path, bool* cardFailed) override {
    if (store_.onCard()) return fat_.open(path, cardFailed);
    *cardFailed = false;
    file_ = store_.storage_.fs().open(path, FILE_READ);
    return file_ ? &source_ : nullptr;
  }
  bool close() override {
    if (store_.onCard()) return fat_.close();
    file_.close();
    return false;
  }

  QueueStore& store_;
  queuefile::FsFiles fat_;
  File file_;
  FileSource source_{file_};
};

QueueStore::Restore QueueStore::restore(bool libraryListsCard, void (*wait)(uint32_t ms)) {
  if (!storage_.available()) return Restore::None;
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  SavedPosition saved = readPosition();
  const QueueResume resume = readResume();
  saver_.setGeneration(saved.generation);
  saver_.loadedResume(resume);  // (one that doesn't apply is removed at the first pass)
  const LibraryIndex* index = catalog_.index();
  // No library at all this time (a card that failed to read, say), or one that may leave
  // tracks out and goes unsaved (`libraryListsCard`): the file keeps what it can't find,
  // below, with no questions.
  const bool library = libraryListsCard && index && index->ready();
  queuefile::Lookup* lookup = onCard() && library ? &lookup_ : nullptr;
  lookup_.limit(lookupClock, queuefile::kBootLookupMs);
  QueueFiles files(*this);
  const queuefile::Read got = queuefile::readAtBoot(files, file, temp, catalog_, queue_, pickCurrent, &saved, psramAlloc,
                                                    psramFree, onCard() ? wait : nullptr, lookup);
  // (No PSRAM for the read's blocks or the queue's: no more the file's word than a fault.)
  const char* why = !got.noMemory ? "a read of the card failed"
                    : got.faulted ? "a read of the card failed, or found no PSRAM"
                                  : "no PSRAM for its read";
  if (got.reads > 1) {
    Serial.printf("[queue] queue.txt: %s: read %lu times, %s\n", why, (unsigned long)got.reads,
                  got.got == queuefile::Got::Unread ? "and each failed" : "and the last was whole");
  }
  // A read on a card silent past its init (CachedDrive::lookAfterFault()) makes FatFs's next
  // call a remount: now, not under a card job later (main.cpp's guard restarts at that one,
  // and the next boot would do the same).
  if (onCard() && (got.faulted || lookup_.failed())) fat_.echoNow();
  if (got.got == queuefile::Got::Unread) {
    // Taken for no queue, a default one would be saved over a file that is only unread.
    saver_.keptFile(saved.generation, saved.have ? saved.position : -1);
    markUnread();
    Serial.printf("[queue] queue.txt unread (%s): no queue this session, the file left as it is\n", why);
    return Restore::Unread;
  }
  if (got.notWhole) Serial.printf("[queue] %s: not a whole queue file, ignored\n", got.notWhole);
  if (got.got == queuefile::Got::None) return Restore::None;
  const char* path = got.path;
  // queue.txt gone, not whole or unread, and queue.tmp whole: the newest queue on the card,
  // held until it takes queue.txt's place (openTemp()).
  tmpHeld_ = path == temp;
  const queuetext::Restored& r = got.read;
  player_.queueReplaced(r.currentKept);
  const bool fromNvs = saved.have && saved.generation == r.header.generation;
  const int32_t line = fromNvs ? saved.position : r.header.current;  // the line pickCurrent() took
  // Tracks that are gone, or a file from the wrong name: write it again.
  // Not for tracks dropped because there's no library at all this time (a
  // card that failed to read, say): the file keeps them, and its line,
  // for when the library is back (the next boot, or the rebuild of "Try
  // again", which reads the file again from that line: QueueRemap). Nor by
  // a library that may leave them out (`libraryListsCard`), or lines that may
  // be on the card with no room for them here (`held`).
  const uint32_t generation =
      r.header.generation > saved.generation ? r.header.generation : saved.generation;
  // A file longer than the queue holds (written before its cap) is read
  // in as its window: written again, at the queue's size.
  if (r.held > 0 || (r.dropped > 0 && !library)) {
    saver_.keptFile(generation, line);
  } else {
    saver_.loaded(generation, r.dropped > 0 || r.capped > 0 || path == temp, millis());
  }
  char capped[96] = "";
  if (r.capped > 0) {
    snprintf(capped, sizeof(capped), "; lines %lu-%lu read in, %lu left out (the queue holds %lu)",
             (unsigned long)r.first + 1, (unsigned long)(r.lines - r.capped + r.first),
             (unsigned long)r.capped, (unsigned long)QueueModel::kMaxEntries);
  }
  char unlisted[128] = "";
  if (lookup && (lookup->kept() || lookup->unasked() || lookup->held())) {
    snprintf(unlisted, sizeof(unlisted), "; %lu by their paths, the library not listing them (%lu the card has, %lu it "
             "didn't say), %lu with no room",
             (unsigned long)(lookup->kept() + lookup->unasked()), (unsigned long)lookup->kept(),
             (unsigned long)lookup->unasked(), (unsigned long)lookup->held());
  }
  Serial.printf("[queue] restored %lu of %lu tracks from %s (%lu no longer there%s%s), at %d of %lu (position from "
                "%s)%s\n",
                (unsigned long)r.entries, (unsigned long)r.lines, path, (unsigned long)r.dropped, capped, unlisted,
                queue_.current() + 1, (unsigned long)queue_.size(), fromNvs ? "NVS" : "the file",
                r.header.shuffled ? ", shuffled" : "");
  // The second it paused at: for this file's current line, if that track
  // is still the same file. Only shown and waiting: nothing plays.
  if (resume.valid) {
    char current[TrackCatalog::kMaxPath];
    catalog_.path(queue_.currentTrack(), current, sizeof(current));
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
  return Restore::Read;
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

// Queued: written once the taps rest (app/NvsLater; the "..." menu's Repeat
// row cycles it in place), not under the finger.
void QueueStore::saveRepeat(PlaybackController::Repeat r) {
  nvslater::putU8(kNvsNamespace, nvslayout::kRepeatKey, static_cast<uint8_t>(r));
}

void QueueStore::markUnread() {
  unread_ = true;
  saver_.loadedResume(QueueResume{});  // (so not removed for an empty queue: stepResume())
}

void QueueStore::keepUnread() {
  // (A shuffle toggled, an add undone before its write: nothing the listener chose
  // instead of a queue that may yet come back.)
  if (!unread_ || !queue_.empty()) return;
  saver_.abort();  // (a snapshot's write goes on without the queue, and would replace the file)
  saver_.keptFile(saver_.generation(), saver_.fileLine());
}

void QueueStore::loop(uint32_t nowMs) {
  if (!storage_.available() || carrying()) return;
  keepUnread();
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
  if (carrying()) {
    if (carry_->result().via == queueremap::Via::Memory) {
      Serial.println("[queue] NOT saved: the card refused queue.txt as the library update began: the queue is text in "
                     "PSRAM, queue.txt keeps the last one saved");
      return false;
    }
    if (unread_) {
      Serial.println("[queue] nothing to save: queue.txt couldn't be read, and is left as it is");
      return true;
    }
    Serial.println("[queue] saved now: queue.txt has the queue (flushed as the library update began)");
    return true;
  }
  const uint32_t t0 = millis();
  keepUnread();
  noteTransport();
  const bool wasWriting = saver_.writing(), wasDirty = saver_.contentDirty();
  const uint32_t writes = saver_.writes();
  const bool ok = saver_.flushNow(t0);
  noteFailures();
  if (unread_ && queue_.empty()) {
    Serial.println("[queue] nothing to save: queue.txt couldn't be read, and is left as it is");
    return ok;
  }
  Serial.printf("[queue] saved now%s%s in %lu ms: %s\n", wasWriting ? " (a write under way finished)" : "",
                saver_.writes() != writes ? ", the file written" : wasDirty ? "" : " (the file was up to date)",
                (unsigned long)(millis() - t0), ok ? "the card has the queue as it is" : "FAILED (the last file stays)");
  return ok;
}

void QueueStore::cardBack() { saver_.cardBack(millis()); }

void QueueStore::noteFailures() {
  if (saver_.failures() == failuresSeen_) return;
  failuresSeen_ = saver_.failures();
  Serial.printf("[queue] saving the queue failed (card full or gone?): trying again in %lu s\n",
                (unsigned long)(saver_.lastWaitMs() / 1000));
}

ByteSink* QueueStore::openTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  if (!readyTemp(file, temp)) {
    Serial.printf("[queue] couldn't write %s: what is there is left as it is (%s)\n", temp,
                  tmpHeld_ ? "it holds the queue, and couldn't take queue.txt's place" : "the card didn't say what it is");
    return nullptr;
  }
  if (!buf_) buf_ = static_cast<uint8_t*>(psramAlloc(kBufSize));
  if (buf_) file_ = storage_.fs().open(temp, FILE_WRITE);
  if (!buf_ || !file_) {
    Serial.printf("[queue] couldn't write %s\n", temp);
    return nullptr;
  }
  sink_.reset(&file_, buf_, kBufSize);
  return &sink_;
}

bool QueueStore::tmpWhole(const char* temp, size_t bytes) {
  uint32_t size = 0, time = 0;
  bool asked = false;
  if (onCard()) {
    asked = fat_.stat(temp, &size, &time);
  } else {
    File f = storage_.fs().open(temp, FILE_READ);
    asked = static_cast<bool>(f);
    if (asked) size = static_cast<uint32_t>(f.size());
    f.close();
  }
  if (asked && size == bytes) return true;
  if (asked) {
    Serial.printf("[queue] %s isn't whole after its close: %lu B there, %lu written\n", temp, (unsigned long)size,
                  (unsigned long)bytes);
  } else {
    // (Not there, the card failed the question, or no PSRAM for it: no word that it's whole.)
    Serial.printf("[queue] %s after its close: its size couldn't be had\n", temp);
  }
  return false;
}

bool QueueStore::replaceFile(const char* file, const char* temp) {
  // With twins on the card (or none counted yet), 2.12.6's rule (tagstore::replace()):
  // queue.txt is renamed to a twin when it shares a twin's chain, since removing it would
  // free clusters the twin holds, and a question the card fails stops it. Else, and on the
  // flash, as ever.
  if (onCard() && twins_ != 0) return tagstore::replace(fat_, kQueueNames);
  fs::FS& fs = storage_.fs();
  fs.remove(file);
  return fs.rename(temp, file);
}

bool QueueStore::readyTemp(const char* file, const char* temp) {
  // At the boot's first save, a queue.tmp with no queue.txt, whatever the boot made of it: it
  // takes that name first. The open to write it would empty it before anything new is safe,
  // and give FatFs the same clusters back, and the rule below would remove it. What the card
  // can't say (out, a read that failed) isn't "no such file": nothing done until it can.
  if (checkKept_) {
    using Seen = LocalStorage::Seen;
    const Seen txt = storage_.seen(file);
    const Seen tmp = txt == Seen::No ? storage_.seen(temp) : Seen::No;
    if (txt == Seen::Unknown || tmp == Seen::Unknown) return false;
    if (tmp == Seen::Yes && !storage_.fs().rename(temp, file)) return false;
    checkKept_ = false;
  }
  if (tmpHeld_ && !settleTemp(file, temp)) return false;
  if (onCard() && twins_ < 0) {
    // The first write of a session, or the first after a commit or settle that failed. A
    // queue.tmp left on queue.txt's own chain (a rename cut, or failed, between its two
    // directory writes, here or in an earlier session), which FA_CREATE_ALWAYS would free,
    // goes to a twin, and a leftover is removed, as 2.12.6 has it. Then the twins are
    // counted (a rename to one that FatFs said failed may have been done all the same), and
    // those a disk check parted removed. A question the card fails: nothing written, asked
    // again at the next write.
    if (!tagstore::prepareTmp(fat_, kQueueNames)) return false;
    bool unread = false;
    const uint32_t left = tagstore::collectTwins(fat_, kQueueNames, &unread);
    if (unread) return false;
    twins_ = static_cast<int32_t>(left);
    if (left) {
      Serial.printf("[queue] %lu copies of queue.txt share a chain on the card (/.player/queue.xl*): it wants a disk "
                    "check on a PC\n",
                    (unsigned long)left);
    }
  }
  return true;
}

bool QueueStore::settleTemp(const char* file, const char* temp) {
  if (!onCard()) {
    if (storage_.fs().exists(temp) && !replaceFile(file, temp)) return false;
    tmpHeld_ = false;
    return true;
  }
  // FatFs's word first, and a question the card fails answers nothing: nothing done. A
  // rename it said failed may be done in its view all the same (its directory sector written
  // again by the next call): no queue.tmp then, and queue.txt is the queue. Or half done,
  // when the two entries are in different sectors of the directory: both on one chain, so
  // queue.txt holds the same queue, and queue.tmp goes to a twin, which frees nothing. And
  // the twins are counted again after (readyTemp()): any of these renames may have made one.
  twins_ = -1;
  const tagstore::Fs::Entry t = fat_.entry(temp);
  if (t.cardFailed) return false;
  if (t.there) {
    const tagstore::Fs::Entry f = fat_.entry(file);
    if (f.cardFailed) return false;
    const bool oneChain = f.there && t.firstCluster != 0 && f.firstCluster == t.firstCluster;
    if (oneChain) {
      if (!tagstore::prepareTmp(fat_, kQueueNames)) return false;
      Serial.printf("[queue] %s and %s shared a chain (a rename failed between its writes): %s kept as a twin; the "
                    "card wants a disk check on a PC\n",
                    file, temp, temp);
    } else if (!replaceFile(file, temp)) {
      return false;
    }
  }
  tmpHeld_ = false;
  return true;
}

bool QueueStore::commitTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  bool ok = sink_.flush();
  file_.close();
  // What the close wrote (the VFS's last 4 KB, FatFs's last sector and the file's entry)
  // it doesn't say: a card that refused it would leave queue.tmp cut, or empty, and the
  // rename would put that in queue.txt's place. So queue.tmp's size, as the storage has it,
  // against what the sink took.
  if (ok && !tmpWhole(temp, sink_.taken())) ok = false;
  if (!ok) {
    storage_.fs().remove(temp);
    Serial.printf("[queue] couldn't replace %s: it stays as it was\n", file);
    return false;
  }
  // Whole: kept from here until it is queue.txt. A rename that fails may have queue.txt
  // gone already, and the boot takes a whole queue.tmp in its place.
  tmpHeld_ = true;
  if (!replaceFile(file, temp)) {
    // (Held now: the next write settles it first, and counts the twins again after, since a
    // rename that failed may have made one all the same.)
    Serial.printf("[queue] couldn't replace %s: %s kept, whole (a boot reads it if %s is gone)\n", file, temp, file);
    return false;
  }
  tmpHeld_ = false;
  unread_ = false;
  return true;
}

void QueueStore::discardTemp() {
  char file[48], temp[48];
  paths(file, temp, sizeof(file));
  file_.close();
  if (!tmpHeld_) storage_.fs().remove(temp);  // (one that holds the queue is never opened for a write: to be sure)
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

// The card and the caller's rebuild, for the portable sequence (flush,
// free, rebuild, re-read: lib/core/QueueRemap, docs/METADATA.md 3.4.2).
struct QueueStore::RemapCard : queueremap::Card {
  RemapCard(QueueStore& store, bool (*rebuild)(void*), void* ctx)
      : store_(store), rebuild_(rebuild), ctx_(ctx), files_(store) {}
  bool flush() override { return store_.storage_.available() && store_.flushNow(); }
  ByteSource* openFile() override {
    // A file the boot couldn't read isn't asked again under the update step: such a read on
    // a card silent past its init makes the next call a remount, with a job under way a
    // restart. The next boot asks.
    if (store_.fileUnread()) return nullptr;
    char file[48], temp[48];
    store_.paths(file, temp, sizeof(file));
    bool cardFailed = false;
    ByteSource* in = files_.open(file, &cardFailed);
    // Only queue.tmp: power went between removing queue.txt and the
    // rename, and a boot with no library kept it as it was (restore()).
    return in ? in : files_.open(temp, &cardFailed);
  }
  void closeFile() override { files_.close(); }
  bool rebuild() override { return rebuild_ && rebuild_(ctx_); }
  queuetext::UnknownLines* unknownLines(bool library) override {
    if (!store_.onCard()) return nullptr;
    store_.lookup_.begin();
    store_.lookup_.limit(lookupClock, queuefile::kRemapLookupMs);
    return library ? &store_.lookup_ : nullptr;
  }

  QueueStore& store_;
  bool (*rebuild_)(void*);
  void* ctx_;
  QueueFiles files_;
};

bool QueueStore::remap(bool (*rebuild)(void* ctx), void* ctx) {
  RemapCard card(*this, rebuild, ctx);
  const queueremap::Result r =
      queueremap::run(queue_, saver_, player_, catalog_, card, millis(), psramAlloc, psramFree);
  logRemap(r);
  return r.rebuilt;
}

bool QueueStore::remapBegin(size_t textRoom) {
  if (!carry_) carry_ = psramNew<queueremap::Carry>(psramAlloc, psramFree);
  if (!card_) card_ = psramNew<RemapCard>(*this, nullptr, nullptr);
  if (!carry_ || !card_) {
    Serial.println("[queue] no PSRAM to carry the queue across the library update");
    return false;
  }
  const bool ok = carry_->begin(queue_, saver_, player_, catalog_, *card_, textRoom);
  noteFailures();
  if (!ok) {
    // (Nothing given back: the queue plays on as it is, the step defers.)
    Serial.println("[queue] the card couldn't take queue.txt, and its text in PSRAM found no memory or would leave "
                   "the build short: the queue can't be carried across the library update (it stays as it is)");
  }
  return ok;
}

bool QueueStore::remapFinish(bool rebuilt) {
  if (!carrying()) return false;
  const bool wasUnread = unread_;
  const queueremap::Result r = carry_->finish(queue_, saver_, player_, catalog_, *card_, rebuilt, millis());
  // (finish() told the saver: keptFile().)
  if (!r.read.ok) markUnread();
  if (wasUnread && !r.read.ok) {
    // (Nothing to carry, and no read of it tried: RemapCard::openFile().)
    noteFailures();
    Serial.println("[queue] the queue stays empty across the update step: queue.txt couldn't be read, and is left as it is");
    return false;
  }
  logRemap(r);
  return r.read.ok && r.read.currentKept;
}

void QueueStore::logRemap(const queueremap::Result& r) {
  noteFailures();
  const queuetext::Restored& got = r.read;
  if (!got.ok) {
    const char* why = r.via == queueremap::Via::None ? "the card couldn't take queue.txt, and no PSRAM for its text"
                      : r.via != queueremap::Via::File ? "no PSRAM to read its text back"
                      : got.noMemory                   ? "no PSRAM to read queue.txt back"
                                                       : "queue.txt couldn't be read back";
    Serial.printf("[queue] couldn't carry the queue across the rebuild (%s): cleared; queue.txt keeps the last one "
                  "saved\n",
                  why);
    return;
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
    return;
  }
  // (Lines left out: a file from before the queue's cap, kept whole by a
  // boot with no library and read back now.)
  char capped[64] = "";
  if (got.capped > 0) {
    snprintf(capped, sizeof(capped), " (%lu left out: the queue holds %lu)", (unsigned long)got.capped,
             (unsigned long)QueueModel::kMaxEntries);
  }
  const char* const after = r.rebuilt ? "the rebuild" : "the update step that failed";
  if (lookup_.kept() || lookup_.unasked() || lookup_.held()) {
    Serial.printf("[queue] after %s, by their paths, the library not listing them: %lu the card has, %lu it didn't "
                  "say, %lu with no room; %lu gone\n",
                  after, (unsigned long)lookup_.kept(), (unsigned long)lookup_.unasked(),
                  (unsigned long)lookup_.held(), (unsigned long)lookup_.gone());
  }
  Serial.printf("[queue] after %s, carried %s: %lu of %lu tracks still there%s, at %d%s%s; the queue gave "
                "%lu KB to it and holds %lu KB\n",
                after, via, (unsigned long)got.entries, (unsigned long)got.lines, capped, queue_.current() + 1,
                got.currentKept ? "" : " (the current one is gone)", r.startCarried ? " (its start point kept)" : "",
                (unsigned long)(r.freedBytes / 1024), (unsigned long)(queue_.memoryBytes() / 1024));
  if (player_.fenceStops() != fenceStopsSeen_) {
    fenceStopsSeen_ = player_.fenceStops();
    Serial.println("[queue] the track ended while the library updated: the next one waits, paused (nothing starts "
                   "by itself)");
  }
}

void QueueStore::printStatus() const {
  static const char* const kEdits[] = {"none",   "play now",      "play next", "add",
                                       "remove", "move to next", "clear up next", "clear"};
  // An undo that would put the shuffle mode back too (Shuffle all's) says so.
  const char* undoMode = queue_.undoShuffled() == queue_.shuffled() ? ""
                         : queue_.undoShuffled()                    ? " (and shuffle on)"
                                                                    : " (and shuffle off)";
  // An add that pushed out what played (docs/QUEUE-MODES.md 15.8): its undo
  // puts those back.
  char undoBack[48] = "";
  if (const uint32_t pushed = queue_.undoPushed()) {
    snprintf(undoBack, sizeof(undoBack), " (and %lu played track%s back)", (unsigned long)pushed,
             pushed == 1 ? "" : "s");
  }
  // Full: an add pushes out what played (the heard entries before the
  // current one), or, with none of those, is refused.
  const char* full = queue_.room() == 0    ? " (full: no played track can make way, an add is refused)"
                     : queue_.spare() == 0 ? " (full: an add pushes out what played)"
                                           : "";
  Serial.printf("[queue] %lu tracks%s (%lu KB of PSRAM, the undo's included), at %d, %lu up next; shuffle %s, repeat "
                "%s; undo: %s%s%s; file generation %lu%s, %lu writes (%lu from a snapshot; last %lu ms), %lu "
                "failures\n",
                (unsigned long)queue_.size(), full, (unsigned long)((queue_.memoryBytes() + 1023) / 1024),
                queue_.current() + 1, (unsigned long)queue_.upNext(),
                queue_.shuffled() ? "on" : "off", repeatName(static_cast<uint8_t>(player_.repeat())),
                kEdits[static_cast<int>(queue_.undoable())], undoMode, undoBack, (unsigned long)saver_.generation(),
                saver_.writingSnapshot() ? " (writing a snapshot)"
                : saver_.writing()       ? " (writing)"
                : saver_.contentDirty()  ? " (to write)"
                                         : "",
                (unsigned long)saver_.writes(), (unsigned long)saver_.snapshotWrites(),
                (unsigned long)saver_.lastWriteMs(), (unsigned long)saver_.failures());
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
  if (fileUnread() || !saver_.fileIsQueue() || unlisted_.count()) {
    Serial.printf("[queue] queue.txt %s; %lu tracks by their paths (%lu KB of PSRAM)\n",
                  fileUnread()            ? "unread at the boot, left as it is"
                  : saver_.fileIsQueue()  ? "is the queue"
                                          : "holds more than the queue",
                  (unsigned long)unlisted_.count(), (unsigned long)((unlisted_.memoryBytes() + 1023) / 1024));
  }
}
