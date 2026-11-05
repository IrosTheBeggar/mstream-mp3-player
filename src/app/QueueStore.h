// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <Arduino.h>
#include <FS.h>

#include "PlaybackController.h"
#include "QueueModel.h"
#include "QueueFile.h"
#include "QueueRemap.h"
#include "QueueSaver.h"
#include "QueueText.h"
#include "TrackCatalog.h"
#include "storage/CardFat.h"
#include "storage/FileStream.h"
#include "storage/LocalStorage.h"

// Keeps the queue across reboots and library rebuilds.
//
// - The tracks: /.player/queue.txt on the card, as paths (QueueText), so a
//   rebuilt library (new ids) finds them again. Rewritten 2 s after the
//   last edit, a few dozen lines per loop pass (a long queue never holds
//   the loop), into queue.tmp, which then replaces queue.txt: only once the
//   storage has all of it (the VFS's close says nothing of what it wrote
//   then), and a whole queue.tmp that couldn't take queue.txt's place is
//   kept (queue.txt may be gone by then), for the boot, and put in place
//   before anything writes over it.
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
//
// A line the library doesn't list stays in the queue by its path, on the card, unless
// the card says its file is gone (lib/core QueueFile: `unlisted` holds them, the
// catalog answers for them).
class QueueStore : private QueueSaver::Store {
public:
  QueueStore(LocalStorage& storage, QueueModel& queue, PlaybackController& player, const TrackCatalog& catalog,
             TrackCatalog::Unlisted& unlisted);

  enum class Restore : uint8_t {
    Read,    // the queue is the file's
    None,    // none, or not a whole file: the caller sets up a default queue
    Unread,  // the card failed each read of it: the queue is empty, and nothing goes over the file
  };
  // At boot, after the library: the saved queue and position, and the
  // resume point (the player's start point). `libraryListsCard` false
  // (Library::listsCard()): tracks it doesn't list may still be on the card, so the
  // file keeps them, as with no library at all, for the next boot. `wait`: the boot's
  // waits between reads the card fails (tagstore::kBootWaitsMs); nullptr: one read.
  Restore restore(bool libraryListsCard = true, void (*wait)(uint32_t ms) = nullptr);
  // The boot, or a read back (remapFinish()), couldn't read the file, and the queue is empty
  // for it. Nothing has replaced it since: the first write of a queue with something in it will.
  bool fileUnread() const { return unread_; }
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
  // and the queue's memory is the build's; or the queue is empty for a file
  // nobody could read, fileUnread(): nothing to save, the file as it is).
  // Carried as text, the card having refused that flush: false: queue.txt
  // keeps the last one saved.
  bool flushNow();
  // Something on its way to the card (a write under way, an edit or a
  // move waiting its delay; not a failed one waiting its retry).
  bool busy() const { return storage_.available() && saver_.busy(); }
  // The card came back (CachedDrive::cardBacks()): QueueSaver::cardBack().
  void cardBack();
  // The PSRAM a queue.txt write under way holds for its snapshot (lib/core
  // QueueSaver: at most 40 KB), which remapBegin()'s flush gives back (the
  // library update's memory check counts it in what the fence frees).
  size_t savingBytes() const { return saver_.snapshotBytes(); }
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
  // queue.tmp, closed, is `bytes` long on the storage (FatFs's word on the card).
  bool tmpWhole(const char* temp, size_t bytes);
  // A whole queue.tmp into queue.txt's place (queue.txt removed first, as ever).
  bool replaceFile(const char* file, const char* temp);
  // The held queue.tmp (tmpHeld_), before anything writes over it: into queue.txt's place,
  // or nothing left of it to keep. False: neither can be done now (nothing done).
  bool settleTemp(const char* file, const char* temp);
  void savePosition(uint32_t generation, int32_t current) override;
  void saveResume(const QueueResume& r) override;
  // The player's resume point, for the saver.
  void noteTransport();
  void paths(char* file, char* temp, size_t size);
  void noteFailures();
  // The card side of the remap (lib/core QueueRemap: queueremap::Card).
  struct RemapCard;
  void logRemap(const queueremap::Result& r);
  // The queue's files, read: through FatFs on the card, the VFS on the flash.
  struct QueueFiles;
  bool onCard() const { return storage_.onCard() && cardfat::ready(); }
  // Nobody could read the file: the queue is empty, and the file keeps its line and its
  // resume point for the next boot (no read of it again this session: such a read on a card
  // silent past its init is a remount, with a job under way a restart).
  void markUnread();
  // What leaves the queue empty of such a file isn't saved over it.
  void keepUnread();

  LocalStorage& storage_;
  QueueModel& queue_;
  PlaybackController& player_;
  const TrackCatalog& catalog_;
  QueueSaver saver_;
  TrackCatalog::Unlisted& unlisted_;
  cardfat::FatFs fat_;
  queuefile::Lookup lookup_;
  bool unread_ = false;
  // queue.tmp is whole and the newest queue on the card: a commit's rename failed (queue.txt
  // may be gone), or the boot read it. Nothing opens it for writing, or removes it, until
  // it has taken queue.txt's place (replaceFile()).
  bool tmpHeld_ = false;
  uint32_t failuresSeen_ = 0;

  File file_;
  uint8_t* buf_ = nullptr;  // PSRAM
  bool checkKept_ = true;   // a queue.tmp may be all there is: at the boot's first save
  BufferedFileSink sink_;
  queueremap::Carry* carry_ = nullptr;  // PSRAM: the two-halves remap's state (remapBegin())
  RemapCard* card_ = nullptr;           // PSRAM
  uint32_t fenceStopsSeen_ = 0;
};
