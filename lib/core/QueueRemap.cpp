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

// The fallback: the queue as text in one block of exactly its size (a
// first pass measures it), from the hooks; given back at the end.
class Text {
public:
  Text(MemorySink::AllocFn alloc, MemorySink::FreeFn release)
      : alloc_(alloc ? alloc : heapAlloc), free_(release ? release : heapFree) {}
  ~Text() {
    if (data_) free_(data_);
  }
  Text(const Text&) = delete;
  Text& operator=(const Text&) = delete;

  bool take(const QueueModel& q, const TrackCatalog& catalog, uint32_t generation) {
    CountSink count;
    if (!queuetext::write(q, catalog, generation, count)) return false;
    data_ = static_cast<uint8_t*>(alloc_(count.size()));
    if (!data_) return false;
    FixedSink out(data_, count.size());
    if (!queuetext::write(q, catalog, generation, out) || out.size() != count.size()) return false;
    size_ = count.size();
    return true;
  }
  const uint8_t* data() const { return data_; }
  size_t size() const { return size_; }

private:
  static void* heapAlloc(size_t n) { return std::malloc(n); }
  static void heapFree(void* p) { std::free(p); }

  MemorySink::AllocFn alloc_;
  MemorySink::FreeFn free_;
  uint8_t* data_ = nullptr;
  size_t size_ = 0;
};

// The re-read's current line: the position the queue had (its lines are
// its positions), whatever the header says (the file may be older than a
// move: the position went to NVS, not the file).
int32_t thePosition(const queuetext::Header&, void* ctx) { return *static_cast<const int32_t*>(ctx); }

}  // namespace

Result run(QueueModel& queue, QueueSaver& saver, PlaybackController& player, const TrackCatalog& catalog, Card& card,
           uint32_t nowMs, MemorySink::AllocFn alloc, MemorySink::FreeFn release) {
  Result res;
  // A start point waiting belongs to the entry's key, which the read gives
  // afresh: noted here, set again after.
  uint32_t startMs = 0, startDurationMs = 0;
  ResumeAnchor startAnchor;
  const bool hadStart = player.startPoint(&startMs, &startDurationMs, &startAnchor);
  int32_t position = queue.current();

  // 1. The queue on the card (or, failing that, in memory).
  Text text(alloc, release);
  if (card.flush()) {
    res.via = Via::File;
  } else {
    saver.abort();  // a write under way would go on with the new ids (flushNow() leaves none: to be sure)
    res.via = text.take(queue, catalog, saver.generation()) ? Via::Memory : Via::None;
    res.textBytes = text.size();
  }

  // 2. Its memory to the rebuild; 3. the rebuild.
  res.freedBytes = queue.memoryBytes();
  queue.release();
  res.rebuilt = card.rebuild();

  // 4. Read back.
  if (res.via == Via::File) {
    if (ByteSource* in = card.openFile()) {
      res.read = queuetext::read(*in, catalog, queue, thePosition, &position, alloc, release);
      card.closeFile();
    }
  } else if (res.via == Via::Memory) {
    MemorySource in(text.data(), text.size());
    res.read = queuetext::read(in, catalog, queue, thePosition, &position, alloc, release);
  }
  if (!res.read.ok) {
    // Cleared (release() kept the mode). The file keeps the last queue
    // saved: nothing is written over it until the listener edits this one.
    player.queueReplaced(false);
    saver.markSaved();
    return res;
  }

  const LibraryIndex* index = catalog.index();
  // (A rebuild that failed before touching the index leaves the ids as
  // they were: the queue reads back the same.)
  res.noLibrary = !index || !index->ready() || index->trackCount() == 0;
  if (res.noLibrary) {
    // Every library track dropped: not the listener's queue changing. The
    // file keeps them for when the library is back (the next boot), and
    // nothing else starts playing (a built-in track that survived, say).
    if (res.read.currentKept) {
      player.queueReplaced(true);
    } else {
      player.stop();
    }
    saver.markSaved();
  } else {
    player.queueReplaced(res.read.currentKept);
    // The file is this queue unless tracks were dropped, or it never got
    // there (the text came through memory): then written again.
    const uint32_t generation =
        res.read.header.generation > saver.generation() ? res.read.header.generation : saver.generation();
    saver.loaded(generation, res.read.dropped > 0 || res.via == Via::Memory, nowMs);
  }
  if (hadStart && res.read.currentKept) {
    player.setStartPoint(startMs, startDurationMs, &startAnchor);
    res.startCarried = true;
  }
  return res;
}

}  // namespace queueremap
