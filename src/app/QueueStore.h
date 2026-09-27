#pragma once
#include <Arduino.h>
#include <FS.h>

#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueText.h"
#include "TrackCatalog.h"
#include "storage/FileStream.h"
#include "storage/LocalStorage.h"

// Keeps the queue across reboots and library rebuilds.
//
// - The tracks: /.player/queue.txt on the card, as paths (QueueText), so a
//   rebuilt library (new ids) finds them again. Rewritten 2 s after the
//   last edit, a few dozen lines per loop pass (a long queue never holds
//   the loop), into queue.tmp, which then replaces queue.txt.
// - The position: NVS ("queue": generation, position), at most once a
//   second while it moves, so a track change doesn't rewrite the file. It
//   only counts for the file with the same generation: a position saved
//   for a newer queue than the file holds (an edit not yet written) is
//   never paired with the older file.
// After a boot the queue is where it was, stopped: nothing starts by itself.
class QueueStore {
public:
  QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player, const TrackCatalog& catalog);

  // At boot, after the library: the saved queue and position. False: none
  // (or unreadable); the caller sets up a default queue.
  bool restore();
  // Saves what changed; call every loop pass.
  void loop(uint32_t nowMs);
  // Runs `rebuild` (a library rebuild: every library id changes) with the
  // queue carried across it by its paths; the track that plays keeps
  // playing if it's still there. Returns what `rebuild` returned.
  bool remap(bool (*rebuild)(void* ctx), void* ctx);
  // "[queue] ..." for the console.
  void printStatus() const;

private:
  void startWrite();
  void stepWrite();
  void finishWrite();
  void abortWrite();
  void savePosition();
  void paths(char* file, char* temp, size_t size);

  LocalStorage& storage_;
  QueueModel& queue_;
  PlaybackController& player_;
  const TrackCatalog& catalog_;

  uint32_t generation_ = 0;       // of the file on the card
  uint32_t savedContent_ = 0;     // the queue's contentVersion() the file holds
  uint32_t savedPosition_ = 0;    // positionVersion() last saved to NVS
  bool contentDirty_ = false;
  bool positionDirty_ = false;
  uint32_t contentChangedMs_ = 0;
  uint32_t positionChangedMs_ = 0;
  uint32_t lastContent_ = 0, lastPosition_ = 0;  // versions seen last pass

  // A write in progress (across loop passes).
  bool writing_ = false;
  File file_;
  uint8_t* buf_ = nullptr;  // PSRAM
  BufferedFileSink sink_;
  queuetext::Writer writer_;
  uint32_t writeGeneration_ = 0;
  uint32_t writeStartMs_ = 0;
  uint32_t nextTryMs_ = 0;  // after a failed write
  uint32_t lastWriteMs_ = 0;  // how long the last complete write took
  uint32_t writes_ = 0, failures_ = 0;
};
