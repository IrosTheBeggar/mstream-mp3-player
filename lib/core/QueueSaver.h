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
// - Edits that never settle (Auto DJ appending a 1-second track every
//   second, each also pushing out the oldest entry at the cap: the device's
//   soak, 2026-10-13, wrote nothing for 59 minutes): once the queue has
//   differed from the file for kMaxStaleMs, the next write starts without
//   its 2 s and writes a snapshot of the queue as it is then (its tracks,
//   and ranks while shuffled: 4-8 bytes an entry, at most 40 KB, from the
//   hooks: the firmware's PSRAM), which edits don't drop: it runs to its
//   end, and the edits after it are the next write's. Its file is the
//   snapshot's whole queue, its header's current line the snapshot's: a
//   position (and a resume point) is saved for it only if the queue is
//   still the snapshot's, else NVS keeps the older generation's, which a
//   boot never pairs with this file (it takes the header's line). A queue
//   that fits one pass: the plain write. No memory for the snapshot: none
//   for kRetryMs, the plain write after 2 s of quiet meanwhile.
// - The position (the current entry): at most once a second while it
//   moves, and only for the file of the same generation: a position saved
//   for a newer queue than the file holds (an edit not yet written) is
//   never paired with the older file. Nor while the file holds more than
//   the queue (keptFile(): its library tracks left out when there was no
//   library): the file's line stays where it was.
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
// write under way is finished (or, if the queue changed since it began, a
// snapshot's too, dropped and the queue written again whole), an edit not
// yet written is written without its 2 s, and the position and the resume
// point are saved. The temporary file only ever replaces the queue file
// once complete, so a flush that fails, or a cut mid-write, leaves the
// last good file.
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
  // The longest the file may lag the queue before a write that edits
  // don't drop (above): a full queue's write takes 3-6 s of passes, so the
  // file is at most about 36 s behind.
  static constexpr uint32_t kMaxStaleMs = 30000;
  using AllocFn = QueueModel::AllocFn;
  using FreeFn = QueueModel::FreeFn;
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

  // `alloc`/`release`: the snapshot's memory (nullptr: malloc/free).
  QueueSaver(Store& store, const QueueModel& queue, const TrackCatalog& catalog, AllocFn alloc = nullptr,
             FreeFn release = nullptr);
  ~QueueSaver();
  QueueSaver(const QueueSaver&) = delete;
  QueueSaver& operator=(const QueueSaver&) = delete;

  // The generation the saved position belongs to (NVS), before a file is read.
  void setGeneration(uint32_t g) { generation_ = g; }
  uint32_t generation() const { return generation_; }
  // The queue as it is now came from the file of `generation`; `rewrite`:
  // write it again (tracks gone, or read from the temporary file).
  void loaded(uint32_t generation, bool rewrite, uint32_t nowMs);
  // The queue as it is now is less than the file of `generation`: what was
  // left of it when its library tracks couldn't be found (a boot or a
  // rebuild with no library), or nothing (a queue that couldn't come across
  // a rebuild). That isn't the listener's edit, so nothing is written: the
  // file keeps the whole queue, and `fileLine` (its current line, as a boot
  // picks it: the position saved for it, -1 for its header's) as the place
  // to come back to. Until the queue is the file's again (an edit written,
  // or loaded()), positions and resume points aren't saved (they would pair
  // this queue's positions with the file's lines), and fileIsQueue() is
  // false: the remap then reads the file from fileLine(), as a boot would,
  // not from the queue's own current entry.
  void keptFile(uint32_t generation, int32_t fileLine);
  // Whether the file's lines are the queue's positions (or will be once the
  // write waiting is done): false after keptFile().
  bool fileIsQueue() const { return fileIsQueue_; }
  // The file's current line, as a boot would pick it: the position last
  // saved for it, keptFile()'s, or -1 (the file's own, in its header).
  int32_t fileLine() const { return fileLine_; }
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
  // ... a snapshot (the class comment's kMaxStaleMs), and its memory (0:
  // none held; flushNow() and abort() give it back).
  bool writingSnapshot() const { return writing_ && snap_.ids != nullptr; }
  size_t snapshotBytes() const {
    return snap_.ids ? static_cast<size_t>(snap_.size) * sizeof(uint32_t) * (snap_.shuffled ? 2 : 1) : 0;
  }
  bool contentDirty() const { return contentDirty_; }
  // Something is on its way to the card: a write under way, or an edit or
  // a move waiting its delay. Not one whose write failed (waiting its
  // retry, maybe forever with the card gone): that's no reason to stay on.
  bool busy() const;
  uint32_t writes() const { return writes_; }
  uint32_t snapshotWrites() const { return snapshotWrites_; }  // of writes(), from a snapshot
  uint32_t resumeWrites() const { return resumeWrites_; }
  uint32_t failures() const { return failures_; }
  uint32_t lastWriteMs() const { return lastWriteMs_; }

private:
  // The queue as a stale write found it (the class comment): one block
  // (takeSnapshot(); snapshotBytes()).
  struct Snapshot {
    uint32_t* ids = nullptr;    // size tracks, then (shuffled) size ranks: one block
    uint32_t* ranks = nullptr;
    uint32_t size = 0;
    int32_t current = -1;
    bool shuffled = false;
    uint32_t version = 0;       // the queue's contentVersion() then
    uint32_t next = 0;          // the next entry to write
    bool headerDone = false;
  };

  void noteChanges(uint32_t nowMs);
  // `snapshot`: the file kMaxStaleMs behind and the edits not settled: a
  // snapshot's write when the queue needs more than a pass (false, nothing
  // begun: no memory for one).
  bool startWrite(uint32_t nowMs, bool snapshot);
  // One step of the write under way: done (the file replaced), dropped, or more to write.
  void stepWrite(uint32_t nowMs, uint32_t maxLines);
  // The snapshot's lines (QueueText's format, byte for byte).
  queuetext::Writer::Step stepSnapshot(uint32_t maxLines);
  bool takeSnapshot();
  void freeSnapshot();
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
  AllocFn alloc_;
  FreeFn free_;

  uint32_t generation_ = 0;     // of the queue file
  bool fileIsQueue_ = true;     // false after keptFile(): the file holds more than the queue
  int32_t fileLine_ = -1;       // the file's current line (fileLine())
  uint32_t savedContent_ = 0;   // the queue's contentVersion() the file holds
  uint32_t savedPosition_ = 0;  // positionVersion() last saved
  bool contentDirty_ = false;
  bool positionDirty_ = false;
  bool failedSinceEdit_ = false;
  uint32_t contentChangedMs_ = 0;
  uint32_t dirtySinceMs_ = 0;   // the queue first differed from the file (or the write under way's snapshot)
  uint32_t positionChangedMs_ = 0;
  uint32_t lastContent_ = 0, lastPosition_ = 0;  // versions seen last

  bool writing_ = false;
  ByteSink* sink_ = nullptr;
  queuetext::Writer writer_;
  Snapshot snap_;
  bool editedSinceSnap_ = false;  // an edit after the snapshot was taken ...
  uint32_t editedSinceSnapMs_ = 0;  // ... the first one's time
  uint32_t snapshotAtMs_ = 0;       // no memory for a snapshot: none tried before this
  uint32_t writeGeneration_ = 0;
  uint32_t writeStartMs_ = 0;
  uint32_t nextTryMs_ = 0;
  uint32_t lastWriteMs_ = 0;
  uint32_t writes_ = 0, snapshotWrites_ = 0, failures_ = 0;

  Transport transport_;
  QueueResume resume_;  // what the store holds
  uint32_t resumeWrites_ = 0;
};
