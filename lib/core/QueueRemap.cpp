// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueRemap.h"

#include <cstdlib>
#include <cstring>

#include "LibraryIndex.h"

namespace queueremap {

namespace {

// Counts what's written (the fallback text's first pass).
class CountSink : public ByteSink {
public:
  bool write(const void*, size_t n) override {
    size_ += n;
    return true;
  }
  size_t size() const { return size_; }

private:
  size_t size_ = 0;
};

// Writes into a block it doesn't own, and no further than its end.
class FixedSink : public ByteSink {
public:
  FixedSink(uint8_t* data, size_t cap) : data_(data), cap_(cap) {}
  bool write(const void* data, size_t n) override {
    if (n > cap_ - size_) return false;
    if (n) std::memcpy(data_ + size_, data, n);
    size_ += n;
    return true;
  }
  size_t size() const { return size_; }

private:
  uint8_t* data_;
  size_t cap_;
  size_t size_ = 0;
};

void* heapAlloc(size_t n) { return std::malloc(n); }
void heapFree(void* p) { std::free(p); }

// The re-read's current line: the position the queue had when the file's
// lines are its positions, whatever the header says (the file may be older
// than a move: the position went to NVS, not the file); else the file's own
// line as a boot picks it (QueueSaver::fileLine(); -1: the header's).
int32_t thePosition(const queuetext::Header& h, void* ctx) {
  const int32_t p = *static_cast<const int32_t*>(ctx);
  return p >= 0 ? p : h.current;
}

// The current entry's file: its path's FNV-1a (QueueSaver::pathHash(), as
// a resume point checks its file); false: none. A function of its own
// (never inlined), so its path buffer isn't on the loop task's stack while
// the library rebuilds under run().
__attribute__((noinline)) bool currentFile(const QueueModel& q, const TrackCatalog& catalog, uint32_t* hash) {
  if (q.current() < 0) return false;
  char path[TrackCatalog::kMaxPath];
  if (catalog.path(q.currentTrack(), path, sizeof(path)) == 0) return false;
  *hash = QueueSaver::pathHash(path);
  return true;
}

}  // namespace

Carry::Carry(MemorySink::AllocFn alloc, MemorySink::FreeFn release)
    : alloc_(alloc ? alloc : heapAlloc), free_(release ? release : heapFree) {}

Carry::~Carry() { dropText(); }

void Carry::dropText() {
  if (text_) free_(text_);
  text_ = nullptr;
  textSize_ = 0;
}

bool Carry::begin(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog,
                  Card& card, size_t textRoom) {
  carry(queue, saver, player, catalog, card, textRoom);
  // No route: nothing is given back (the build doesn't start, and the queue
  // isn't the listener's to lose for it).
  if (res_.via == Via::None) return false;
  lend(queue, player);
  return true;
}

void Carry::carry(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog,
                  Card& card, size_t textRoom) {
  res_ = Result{};
  carrying_ = false;
  dropText();
  // A start point waiting belongs to the entry's key, which the read gives
  // afresh: noted here, set again after.
  hadStart_ = player.startPoint(&startMs_, &startDurationMs_, &startAnchor_);
  position_ = queue.current();
  // The current entry's file: whether it is still the current one after is
  // a question of its path, not of a line number (the file may hold more
  // lines than the queue did).
  before_ = 0;
  hadCurrent_ = currentFile(queue, catalog, &before_);

  // 1. The queue on the card (or, failing that, in memory).
  if (card.flush()) {
    res_.via = Via::File;
    // The file holds more than the queue (QueueSaver::keptFile(): a boot or
    // a rebuild with no library left its library tracks out, or the queue
    // couldn't come across the last rebuild): it is read back whole, from
    // its own line, as the next boot would, not from the queue's.
    if (!saver.fileIsQueue()) position_ = saver.fileLine();
  } else {
    saver.abort();  // a write under way would go on with the new ids (flushNow() leaves none: to be sure)
    // The text in one block of exactly its size (a first pass measures it),
    // held through the rebuild: no more than it can spare.
    CountSink count;
    if (queuetext::write(queue, catalog, saver.generation(), count) && count.size() <= textRoom) {
      text_ = static_cast<uint8_t*>(alloc_(count.size()));
      if (text_) {
        FixedSink out(text_, count.size());
        if (queuetext::write(queue, catalog, saver.generation(), out) && out.size() == count.size()) {
          textSize_ = count.size();
        } else {
          dropText();
        }
      }
    }
    res_.via = text_ ? Via::Memory : Via::None;
    res_.textBytes = textSize_;
  }
}

void Carry::lend(QueueModel& queue, PlaybackController& player) {
  // 2. Its memory to the rebuild; the player reads neither the queue nor
  // the catalog until finish().
  res_.freedBytes = queue.memoryBytes();
  player.setFenced(true);
  queue.release();
  carrying_ = true;
}

Result Carry::finish(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog,
                     Card& card, bool rebuilt, uint32_t nowMs) {
  carrying_ = false;
  res_.rebuilt = rebuilt;
  // A track that ended inside the fence is noted now (queueReplaced() takes
  // it, and any join heard meanwhile).
  player.setFenced(false);

  // 4. Read back.
  if (res_.via == Via::File) {
    if (ByteSource* in = card.openFile()) {
      res_.read = queuetext::read(*in, catalog, queue, thePosition, &position_, alloc_, free_);
      card.closeFile();
    }
  } else if (res_.via == Via::Memory) {
    MemorySource in(text_, textSize_);
    res_.read = queuetext::read(in, catalog, queue, thePosition, &position_, alloc_, free_);
  }
  dropText();
  Result& res = res_;
  if (!res.read.ok) {
    // Cleared (release() kept the mode). The file keeps the last queue
    // saved, and its line: nothing is written over it until the listener
    // edits this one, and the next remap reads it from that line.
    player.queueReplaced(false);
    saver.keptFile(saver.generation(), saver.fileLine());
    return res;
  }
  // Kept: the same file is current (its key is new).
  uint32_t after = 0;
  res.read.currentKept = hadCurrent_ && currentFile(queue, catalog, &after) && after == before_;

  const LibraryIndex* index = catalog.index();
  // (A rebuild that failed before touching the index leaves the ids as
  // they were: the queue reads back the same.)
  res.noLibrary = !index || !index->ready() || index->trackCount() == 0;
  const uint32_t generation =
      res.read.header.generation > saver.generation() ? res.read.header.generation : saver.generation();
  if (res.noLibrary) {
    // Every library track dropped: not the listener's queue changing. The
    // file keeps them for when the library is back (the next boot, or the
    // next remap: it reads the file from the file's line), and nothing else
    // starts playing (a built-in track that survived, say).
    if (res.read.currentKept) {
      player.queueReplaced(true);
    } else {
      player.stop();
    }
    if (res.read.dropped > 0 || res.read.capped > 0) {
      saver.keptFile(generation, saver.fileLine());
    } else {
      saver.loaded(generation, res.via == Via::Memory, nowMs);  // nothing of the library's in it
    }
  } else {
    player.queueReplaced(res.read.currentKept);
    // The file is this queue unless tracks were dropped or left out by the
    // cap (a file from before it), or it never got there (the text came
    // through memory): then written again.
    saver.loaded(generation, res.read.dropped > 0 || res.read.capped > 0 || res.via == Via::Memory, nowMs);
  }
  if (hadStart_ && res.read.currentKept) {
    player.setStartPoint(startMs_, startDurationMs_, &startAnchor_);
    res.startCarried = true;
  }
  return res;
}

Result run(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog, Card& card,
           uint32_t nowMs, MemorySink::AllocFn alloc, MemorySink::FreeFn release) {
  Carry carry(alloc, release);
  // (No route: the queue is given back all the same, and cleared after:
  // the rebuild here runs anyway.)
  carry.carry(queue, saver, player, catalog, card, SIZE_MAX);
  carry.lend(queue, player);
  // 3. The rebuild.
  const bool rebuilt = card.rebuild();
  return carry.finish(queue, saver, player, catalog, card, rebuilt, nowMs);
}

}  // namespace queueremap
