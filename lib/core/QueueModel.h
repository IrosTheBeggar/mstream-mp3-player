// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

// The play queue: track ids (TrackCatalog's: library tracks and built-in
// ones), a current position, and one level of undo. Portable, host-tested.
//
// Each entry is a track id and a key: a number given to the entry when it
// joins the queue and never reused, so the UI can keep a selection (or the
// row it shows) across edits that move positions. Positions are 0-based.
//
// Memory: the entries (8 bytes each) and the undo snapshot (the same again)
// are flat arrays from allocator hooks (the firmware points them at PSRAM),
// growing by doubling. Nothing else is allocated, except a bit per entry
// for the length of a remove() or moveNext().
//
// The current position is -1 only when the queue is empty. The rules the
// edits follow (the tab bar design's Library and Queue actions):
//   replace()      Play: the queue becomes these tracks, current at `start`
//   insertNext()   Play next: right after the current entry
//   append()       + Queue: at the end
//   remove()       the selected positions; the current entry going makes
//                  the next one that stays current (the last one, and
//                  pastEnd, when none after it stays)
//   moveNext()     the selected positions, in queue order, right after the
//                  current entry (Play next in edit mode)
//   clearUpNext()  everything after the current entry (keeps what plays,
//                  and what played before it)
//   clear()        everything
// Adding to an empty queue makes the first new entry current. Each edit
// saves a snapshot first; undo() puts the queue back as it was before the
// last one. What the player does about an edit (start the new current
// track, stop) is PlaybackController's.
//
// Not thread-safe: the firmware's loop task owns it.
class QueueModel {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);

  static constexpr uint32_t kNone = 0xFFFFFFFFu;

  enum class Edit : uint8_t { None, Replace, InsertNext, Append, Remove, MoveNext, ClearUpNext, Clear };

  struct Removed {
    uint32_t count = 0;    // entries removed
    bool current = false;  // the current entry was one of them
    bool pastEnd = false;  // ... and none after it stayed: current is now the last entry (or none)
  };

  // nullptr hooks: malloc/free.
  explicit QueueModel(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~QueueModel();
  QueueModel(const QueueModel&) = delete;
  QueueModel& operator=(const QueueModel&) = delete;

  // ---- reading ----
  uint32_t size() const { return q_.size; }
  bool empty() const { return q_.size == 0; }
  int32_t current() const { return current_; }
  uint32_t trackAt(uint32_t pos) const { return pos < q_.size ? q_.data[pos].track : kNone; }
  uint32_t keyAt(uint32_t pos) const { return pos < q_.size ? q_.data[pos].key : kNone; }
  uint32_t currentTrack() const { return current_ >= 0 ? q_.data[current_].track : kNone; }
  uint32_t currentKey() const { return current_ >= 0 ? q_.data[current_].key : kNone; }
  // The position of the entry with `key`, or kNone (removed, or never).
  uint32_t positionOf(uint32_t key) const;
  // Entries after the current one ("12 up next").
  uint32_t upNext() const { return current_ >= 0 ? q_.size - 1 - static_cast<uint32_t>(current_) : 0; }
  // Bumped by every change to the entries (not by a move of the current
  // position alone): what the UI redraws on and the saver saves on.
  uint32_t contentVersion() const { return contentVersion_; }
  // Bumped whenever the current position or the entries change.
  uint32_t positionVersion() const { return positionVersion_; }

  // ---- editing: false when out of memory, the queue then unchanged ----
  bool replace(const uint32_t* tracks, uint32_t n, uint32_t start);
  bool insertNext(const uint32_t* tracks, uint32_t n);
  bool append(const uint32_t* tracks, uint32_t n);
  // Positions out of range and repeats are ignored; any order.
  Removed remove(const uint32_t* positions, uint32_t n);
  bool moveNext(const uint32_t* positions, uint32_t n);
  bool clearUpNext();
  bool clear();

  // ---- the current position ----
  bool setCurrent(uint32_t pos);
  // By `delta`; past either end it wraps when `wrap`, otherwise it stays
  // and returns false.
  bool step(int delta, bool wrap);

  // ---- undo (one level) ----
  // The last edit, if it can be undone (None after undo(), a restore, or a
  // snapshot that didn't fit in memory).
  Edit undoable() const { return undoEdit_; }
  // The queue as it was before the last edit. The current entry is the one
  // current now if it was there then (what plays keeps playing), otherwise
  // the one that was current then.
  bool undo();
  void dropUndo();

  // ---- restoring (persistence, a library rebuild) ----
  // The queue becomes these tracks with `current` (clamped; -1 for an
  // empty queue), with fresh keys and no undo.
  bool assign(const uint32_t* tracks, uint32_t n, int32_t current);

private:
  struct Entry {
    uint32_t track;
    uint32_t key;
  };
  struct Array {  // from the hooks
    Entry* data = nullptr;
    uint32_t size = 0;
    uint32_t cap = 0;
  };

  bool reserve(Array& a, uint32_t n);
  void drop(Array& a);
  // Copies the entries to the undo snapshot (the edit is then undoable);
  // false: no memory for it (the edit goes ahead, not undoable).
  bool snapshot(Edit edit);
  // A bit per position in `positions` (in range), in a block from the
  // hooks; nullptr: no memory. `count`: how many distinct positions.
  uint32_t* selection(const uint32_t* positions, uint32_t n, uint32_t* count);
  void changed();
  bool insertAt(uint32_t at, const uint32_t* tracks, uint32_t n, Edit edit);

  AllocFn allocFn_;
  FreeFn freeFn_;
  Array q_;  // the entries
  int32_t current_ = -1;
  uint32_t nextKey_ = 0;
  uint32_t contentVersion_ = 0;
  uint32_t positionVersion_ = 0;

  Array undo_;
  int32_t undoCurrent_ = -1;
  Edit undoEdit_ = Edit::None;
};
