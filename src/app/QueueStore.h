// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>
#include <FS.h>

#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueRemap.h"
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
//   ms, length, and its anchor: nvslayout's version 2), written at a pause
//   and removed when playback moves on (QueueSaver says when). At boot, one
//   saved for the restored file's current line, whose track is still that
//   file, becomes the player's start point with its anchor: Now Playing
//   shows that second, and play starts there, on the very sample when the
//   anchor is still the file's (docs/SEEK.md section 5).
// - Shuffle: with the queue, in the file (queue.txt version 2 while
//   shuffled: docs/QUEUE-MODES.md section 2.9), so the mode and the ranks
//   it needs are one write. Repeat: NVS ("queue"/"repeat", a u8; schema
//   2), written at once on a change, read at boot before the queue.
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
  // The repeat mode saved (Off when none is, or an unknown value), logged.
  // At boot, before restore() and the first play.
  PlaybackController::Repeat loadRepeat();
  // A change of mode (the menu, the console): written at once.
  void saveRepeat(PlaybackController::Repeat r);
  // Saves what changed; call every loop pass (nothing while the queue is
  // carried across a build: remapBegin()).
  void loop(uint32_t nowMs);
  // Everything now, synchronously (before a power-off: ENERGY.md item 4):
  // a write under way finished (or written again whole if the queue
  // changed since it began), an edit not yet written, the position, the
  // resume point. True: the card has the queue as it is (or there is no
  // storage; or it is carried across a build: queue.txt was flushed then,
  // and the queue's memory is the build's).
  bool flushNow();
  // Something on its way to the card (a write under way, an edit or a
  // move waiting its delay; not a failed one waiting its retry).
  bool busy() const { return storage_.available() && saver_.busy(); }
  // Runs `rebuild` (a library rebuild: every library id changes) with the
  // queue carried across it by its paths, through queue.txt: flushed, the
  // queue's memory given to the rebuild, read back after
  // (lib/core/QueueRemap; as text in PSRAM only when the card can't take
  // the file). The track that plays keeps playing if it's still there.
  // Returns what `rebuild` returned.
  bool remap(bool (*rebuild)(void* ctx), void* ctx);
  // The same in two halves around a build that runs while the loop goes on
  // (the update step's on the card worker, docs/METADATA.md 3.4.2):
  // remapBegin() flushes and gives the queue's memory to the build (the
  // player fenced); remapFinish() reads queue.txt back with the new ids
  // (`rebuilt`: the build's result). Between them the queue is empty and
  // nothing here writes. When the card can't take queue.txt, the queue's
  // text is held in PSRAM through the build instead, at most `textRoom`
  // (what the build's memory check had to spare: LibraryUpdate::spare()).
  // False: the queue can't be carried (no PSRAM for the carry, or the text
  // over `textRoom` or without memory): nothing done, the queue as it is
  // (the caller doesn't start the build: LibraryUpdate::cantFence()).
  bool remapBegin(size_t textRoom = SIZE_MAX);
  // True: the current entry is the same file as before (its key is new).
  bool remapFinish(bool rebuilt);
  bool carrying() const { return carry_ && carry_->carrying(); }
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
  // The card side of the remap (lib/core QueueRemap: queueremap::Card).
  struct RemapCard;
  void logRemap(const queueremap::Result& r);

  LocalStorage& storage_;
  QueueModel& queue_;
  PlaybackController& player_;
  const TrackCatalog& catalog_;
  QueueSaver saver_;
  uint32_t failuresSeen_ = 0;

  File file_;
  uint8_t* buf_ = nullptr;  // PSRAM
  BufferedFileSink sink_;
  queueremap::Carry* carry_ = nullptr;  // PSRAM: the two-halves remap's state (remapBegin())
  RemapCard* card_ = nullptr;           // PSRAM
  uint32_t fenceStopsSeen_ = 0;
};
