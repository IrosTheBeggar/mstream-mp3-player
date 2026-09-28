#pragma once
#include <cstdint>

#include "ByteStream.h"
#include "QueueModel.h"
#include "QueueText.h"
#include "TrackCatalog.h"

// When and how the queue is saved, without the card (app/QueueStore gives
// it the card and NVS; the host tests memory):
//
// - The tracks (QueueText, as paths): written 2 s after the last edit, a
//   few dozen lines per loop pass (a long queue never holds the loop), into
//   a temporary file that then replaces the queue file. An edit while a
//   write is under way drops that write; it starts again once the edits
//   settle. A failed write is tried again 10 s later.
// - The position (the current entry): at most once a second while it
//   moves, and only for the file of the same generation: a position saved
//   for a newer queue than the file holds (an edit not yet written) is
//   never paired with the older file.
//
// flushNow() does all of it at once, for a power-off (ENERGY.md item 4): a
// write under way is finished (or, if the queue changed since it began,
// dropped and the queue written again whole), an edit not yet written is
// written without its 2 s, and the position is saved. The temporary file
// only ever replaces the queue file once complete, so a flush that fails
// leaves the last good file.
//
// Portable (host-tested: test_queue). Loop task only.
class QueueSaver {
public:
  // Where the saver writes.
  class Store {
  public:
    // The temporary file, opened (truncated) for writing: a sink for its
    // bytes, or nullptr (can't).
    virtual ByteSink* openTemp() = 0;
    // The temporary file is complete: flush and close it, and make it the
    // queue file. False: failed (the temporary file is removed; the queue
    // file stays as it was).
    virtual bool commitTemp() = 0;
    // A write dropped: close and remove the temporary file.
    virtual void discardTemp() = 0;
    // The position (the current entry) of the file of this generation.
    virtual void savePosition(uint32_t generation, int32_t current) = 0;

  protected:
    ~Store() = default;
  };

  static constexpr uint32_t kContentDelayMs = 2000;   // after the last edit
  static constexpr uint32_t kPositionDelayMs = 1000;  // after the last move
  static constexpr uint32_t kRetryMs = 10000;         // after a failed write
  static constexpr uint32_t kLinesPerPass = 32;       // ~2 KB of paths

  QueueSaver(Store& store, const QueueModel& queue, const TrackCatalog& catalog)
      : store_(store), queue_(queue), catalog_(catalog) {}

  // The generation the saved position belongs to (NVS), before a file is read.
  void setGeneration(uint32_t g) { generation_ = g; }
  uint32_t generation() const { return generation_; }
  // The queue as it is now came from the file of `generation`; `rewrite`:
  // write it again (tracks gone, or read from the temporary file).
  void loaded(uint32_t generation, bool rewrite, uint32_t nowMs);
  // The queue as it is now counts as saved (it isn't the listener's edit:
  // a rebuild that left no library).
  void markSaved();

  // Every loop pass.
  void loop(uint32_t nowMs);
  // Everything now (see above). True: what is saved is the queue as it is.
  bool flushNow(uint32_t nowMs);
  // A write under way dropped (the library is about to be rebuilt: its
  // lines would mix old ids and new).
  void abort();

  bool writing() const { return writing_; }
  bool contentDirty() const { return contentDirty_; }
  // Something is on its way to the card: a write under way, or an edit or
  // a move waiting its delay. Not one whose write failed (waiting its
  // retry, maybe forever with the card gone): that's no reason to stay on.
  bool busy() const;
  uint32_t writes() const { return writes_; }
  uint32_t failures() const { return failures_; }
  uint32_t lastWriteMs() const { return lastWriteMs_; }

private:
  void noteChanges(uint32_t nowMs);
  bool startWrite(uint32_t nowMs);
  // One step of the write under way: done (the file replaced), dropped, or more to write.
  void stepWrite(uint32_t nowMs, uint32_t maxLines);
  void finishWrite(uint32_t nowMs);
  void dropWrite();
  void failed(uint32_t nowMs);
  void savePosition();

  Store& store_;
  const QueueModel& queue_;
  const TrackCatalog& catalog_;

  uint32_t generation_ = 0;     // of the queue file
  uint32_t savedContent_ = 0;   // the queue's contentVersion() the file holds
  uint32_t savedPosition_ = 0;  // positionVersion() last saved
  bool contentDirty_ = false;
  bool positionDirty_ = false;
  bool failedSinceEdit_ = false;
  uint32_t contentChangedMs_ = 0;
  uint32_t positionChangedMs_ = 0;
  uint32_t lastContent_ = 0, lastPosition_ = 0;  // versions seen last

  bool writing_ = false;
  ByteSink* sink_ = nullptr;
  queuetext::Writer writer_;
  uint32_t writeGeneration_ = 0;
  uint32_t writeStartMs_ = 0;
  uint32_t nextTryMs_ = 0;
  uint32_t lastWriteMs_ = 0;
  uint32_t writes_ = 0, failures_ = 0;
};
