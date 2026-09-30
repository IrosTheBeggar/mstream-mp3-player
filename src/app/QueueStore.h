#pragma once
#include <Arduino.h>
#include <FS.h>

#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueSaver.h"
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
// - The resume point: NVS ("queue"/"resume": generation, line, path hash,
//   ms, length), written at a pause and removed when playback moves on
//   (QueueSaver says when). At boot, one saved for the restored file's
//   current line, whose track is still that file, becomes the player's
//   start point: Now Playing shows that second, and play starts there.
// When and what to write is QueueSaver's (lib/core, host-tested); this is
// its card and NVS. After a boot the queue is where it was, stopped (at
// the second it paused at, if it did): nothing starts by itself.
class QueueStore : private QueueSaver::Store {
public:
  QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player, const TrackCatalog& catalog);

  // At boot, after the library: the saved queue and position, and the
  // resume point (the player's start point). False: none (or unreadable);
  // the caller sets up a default queue.
  bool restore();
  // Saves what changed; call every loop pass.
  void loop(uint32_t nowMs);
  // Everything now, synchronously (before a power-off: ENERGY.md item 4):
  // a write under way finished (or written again whole if the queue
  // changed since it began), an edit not yet written, the position, the
  // resume point. True: the card has the queue as it is (or there is no
  // storage).
  bool flushNow();
  // Something on its way to the card (a write under way, an edit or a
  // move waiting its delay; not a failed one waiting its retry).
  bool busy() const { return storage_.available() && saver_.busy(); }
  // Runs `rebuild` (a library rebuild: every library id changes) with the
  // queue carried across it by its paths; the track that plays keeps
  // playing if it's still there. Returns what `rebuild` returned.
  bool remap(bool (*rebuild)(void* ctx), void* ctx);
  // "[queue] ..." for the console.
  void printStatus() const;

private:
  // QueueSaver::Store: queue.tmp through a PSRAM buffer, then renamed.
  ByteSink* openTemp() override;
  bool commitTemp() override;
  void discardTemp() override;
  void savePosition(uint32_t generation, int32_t current) override;
  void saveResume(const QueueResume& r) override;
  // The player's resume point, for the saver.
  void noteTransport();
  void paths(char* file, char* temp, size_t size);
  void noteFailures();

  LocalStorage& storage_;
  QueueModel& queue_;
  PlaybackController& player_;
  const TrackCatalog& catalog_;
  QueueSaver saver_;
  uint32_t failuresSeen_ = 0;

  File file_;
  uint8_t* buf_ = nullptr;  // PSRAM
  BufferedFileSink sink_;
};
