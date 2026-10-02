// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstdint>

#include "ByteStream.h"
#include "QueueModel.h"
#include "QueueText.h"
#include "ResumeAnchor.h"
#include "TrackCatalog.h"

// A resume point: `positionMs` into the entry at line `entry` of the queue
// file of `generation`, whose path hashes to `pathHash` (pathHash()), and
// the anchor that starts it on the very sample it paused at (docs/SEEK.md
// section 5; kind None: by the millisecond).
struct QueueResume {
  bool valid = false;
  uint32_t generation = 0;
  int32_t entry = -1;
  uint32_t pathHash = 0;
  uint32_t positionMs = 0;
  uint32_t durationMs = 0;  // the track's length then (0: not known), for Now Playing's bar before it plays
  ResumeAnchor anchor;
};

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
// - The resume point (the second the current entry picks up at after a
//   boot; ENERGY.md item 6): saved at every pause, so at every orderly
//   shutdown too (the CPU speed's restart pauses first, the idle power-off
//   only comes paused or stopped, the sleep timer's end is a pause), and
//   cleared as soon as playback moves on (a play, a skip, another entry,
//   an edit that changes the current entry). Never while playing: no
//   writes every second (flash wear), and a power cut while playing finds
//   none (the entry starts at 0:00, as before). It pairs with the file of
//   its generation, as the position does: saved only once the file holds
//   the queue as it is, and saved again (at the entry's new line) after an
//   edit that only moved it. A clear is written at once. Its anchor (the
//   bytes that start it exactly) is saved with it, and a resume point whose
//   anchor changed is saved again however little it moved: a pause's fade
//   reads 64 frames more, so at most one more write per pause, and the
//   saved sample is the one the in-RAM resume would continue from.
//
// flushNow() does all of it at once, for a power-off (ENERGY.md item 4): a
// write under way is finished (or, if the queue changed since it began,
// dropped and the queue written again whole), an edit not yet written is
// written without its 2 s, and the position and the resume point are
// saved. The temporary file only ever replaces the queue file once
// complete, so a flush that fails leaves the last good file.
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
    // The resume point (below); !valid: none (removed).
    virtual void saveResume(const QueueResume& r) = 0;

  protected:
    ~Store() = default;
  };

  static constexpr uint32_t kContentDelayMs = 2000;   // after the last edit
  static constexpr uint32_t kPositionDelayMs = 1000;  // after the last move
  static constexpr uint32_t kRetryMs = 10000;         // after a failed write
  static constexpr uint32_t kLinesPerPass = 32;       // ~2 KB of paths
  // A resume point that moved less than this since it was saved isn't
  // saved again (a paused output reads a few ms more as its fade ends),
  // unless its anchor changed.
  static constexpr uint32_t kResumeSlackMs = 250;

  // What the player says each pass (PlaybackController::resumePoint()):
  // where the current entry would pick up after a boot; `have` false while
  // it plays, or when it would start at 0:00 anyway.
  struct Transport {
    bool have = false;
    uint32_t positionMs = 0;
    uint32_t durationMs = 0;
    ResumeAnchor anchor;  // kind None: none (a built-in track, gapless trimming off)
  };

  // FNV-1a of a track's path: the resume point's check that the entry is
  // still the same file after a boot (ids change with a library rebuild).
  static uint32_t pathHash(const char* path);
  // Whether the resume point `r` read at boot is the current entry's: the
  // queue came from the file of `fileGeneration` with line `line` current,
  // that line's track is still there (`kept`) and its path is `path`.
  static bool resumeApplies(const QueueResume& r, uint32_t fileGeneration, int32_t line, bool kept, const char* path);

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
  // What the store holds as the resume point (at boot, applied or not: one
  // that doesn't apply is cleared at the next pass).
  void loadedResume(const QueueResume& r) { resume_ = r; }
  // Before each loop() and flushNow().
  void noteTransport(const Transport& t) { transport_ = t; }
  const QueueResume& resume() const { return resume_; }

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
  uint32_t resumeWrites() const { return resumeWrites_; }
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
  // The resume point: cleared at once when there is none; saved when the
  // file holds the queue as it is.
  void stepResume();

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

  Transport transport_;
  QueueResume resume_;  // what the store holds
  uint32_t resumeWrites_ = 0;
};
