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
// row it shows) across edits that move positions. Positions are 0-based,
// and are the order that plays, shuffled or not.
//
// The cap: at most kMaxEntries (5,000) entries, whatever the library's
// size (docs/QUEUE-MODES.md section 15; the user's answer to
// docs/METADATA.md's U12). A Play of more takes kMaxEntries of them (not
// shuffled: the first, or the window that holds its start; shuffled: the
// chosen track and a random rest); assign() keeps the window that holds
// its current entry (window()). An add that doesn't fit first pushes out
// what already played (played(): the heard entries before the current
// one, below), oldest first (the first in play order, shuffled too), as
// many as it lacks (the user's choice, 2026-10-08: section 15.8), then
// takes as many as fit, the first ones in its order; it is refused
// (false, nothing changed) only when nothing can go: the queue full and
// no played entry before the current one. The current entry, everything
// after it and the unheard entries before it stay, in their order. The
// undo is kept whatever the size, and puts back what an add pushed out.
//
// Heard: an entry is marked once it has been the current entry (a step, a
// tap, a join taken, a Play's start, the next one a remove makes current:
// whatever makes it current, the player then plays or cues it), and keeps
// the mark through every move, toggle, sort and undo. An entry passed
// over never is: those a jump skips, a tapped start's earlier tracks, the
// unplayed ones a shuffle Off sorts in before the current entry. The
// marks aren't saved (queue.txt holds the order, NVS the position):
// assign() (the boot's restore, a remap's re-read) marks every entry
// before its current one, as the rule was before the marks (15.8). The
// mark is the key's top bit: keys are 31 bits, and it costs no memory.
//
// Memory: the entries (12 bytes each) and the undo snapshot (the same
// again) are flat arrays from allocator hooks (the firmware points them at
// PSRAM). They grow by doubling, never past the cap, and assign() (the
// boot's restore, its default queue, a remap's re-read) gives the entries
// a block of exactly their count and no snapshot: a full queue is 60 KB,
// and 120 KB once an edit takes its snapshot (docs/METADATA.md section
// 3.5). release() gives both back for a library rebuild. Nothing else is
// allocated, except a bit per entry for the length of a remove() or
// moveNext().
//
// The current position is -1 only when the queue is empty. The rules the
// edits follow (the tab bar design's Library and Queue actions):
//   replace()      Play: the queue becomes these tracks, current at `start`
//                  (past the cap, kMaxEntries of them: above)
//   insertNext()   Play next: right after the current entry (as many as fit,
//                  pushing out what played to make room)
//   append()       + Queue: at the end (as many as fit, the same)
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
// Shuffle (docs/QUEUE-MODES.md section 2) reorders the entries themselves,
// so a position is a play position everywhere (the Queue tab, the saver,
// step() and peek()) and the Queue tab shows the order that plays. Each
// entry carries a rank, its place in the queue's own order: while
// shuffled, ranks are distinct and sort into that order (gaps are fine);
// while not, they mean nothing (the own order is the positions).
//   setShuffled(true)   what is up next (after the current entry) is
//                       shuffled; the current entry and what played before
//                       it stay where they are
//   setShuffled(false)  the entries sorted back by rank: the own order,
//                       the current entry in its place there
// A toggle is no edit: it drops the undo and allocates nothing. Each
// snapshot carries the mode it was taken in, and undo() puts it back with
// the entries: the same mode for every edit but a Play that sets it
// (replace(.., shuffled): Shuffle all's, which turns it on), whose undo
// brings back the queue and the mode it had. While shuffled the edits
// keep the ranks: Play (replace()) puts the chosen track first (kAnyStart:
// a random one) and shuffles every other after it, the ranks the given
// order; Play next ranks right after the current entry and + Queue after
// the highest rank (of what stays, when it pushed out played entries:
// their ranks leave gaps), each in the given order and never shuffled in
// (the listener put them where they are); moveNext() ranks the moved right
// after the current entry; an add to an empty queue is laid out as a Play
// from its first. An add that would take a rank past 0xFFFFFFFF is refused
// as out of memory is (four billion adds without a Play: never, but it
// can't wrap silently). The generator is a hook (the firmware's
// esp_random(): fresh entropy each time); without one a fixed sequence, so
// the host tests repeat.
//
// Not thread-safe: the firmware's loop task owns it.
class QueueModel {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  // 32 random bits (esp_random on the firmware).
  using RandomFn = uint32_t (*)();

  static constexpr uint32_t kNone = 0xFFFFFFFFu;
  // replace()'s start: shuffled, a random first; not shuffled, the first
  // (0, not the clamp's last).
  static constexpr uint32_t kAnyStart = kNone;
  // The most entries the queue holds (the class comment): 60 KB of
  // entries, 120 KB with the undo snapshot. The user's choice (2026-10-07)
  // over dropping the undo of a long queue: a whole-library queue of
  // 20,000 was 240-480 KB of PSRAM next to a 20k index.
  static constexpr uint32_t kMaxEntries = 5000;

  // Of `n` entries with `current` the current one (-1: none), the ones a
  // queue keeps: all when they fit (n <= kMaxEntries); else the first
  // kMaxEntries when `current` is among them; else from `current` on (what
  // plays and what comes after it, what played before it left out), moved
  // back so it is still kMaxEntries long when fewer follow `current`.
  // assign()'s rule, replace()'s when not shuffled, and the queue file's
  // read (queuetext::read(): an older firmware's longer queue).
  struct Window {
    uint32_t first = 0;
    uint32_t count = 0;
  };
  static Window window(uint32_t n, int32_t current);

  enum class Edit : uint8_t { None, Replace, InsertNext, Append, Remove, MoveNext, ClearUpNext, Clear };

  struct Removed {
    uint32_t count = 0;    // entries removed
    bool current = false;  // the current entry was one of them
    bool pastEnd = false;  // ... and none after it stayed: current is now the last entry (or none)
  };

  // nullptr hooks: malloc/free; no `random`: a fixed xorshift32 sequence
  // (from 0x2545F491), so the host tests repeat.
  explicit QueueModel(AllocFn alloc = nullptr, FreeFn release = nullptr, RandomFn random = nullptr);
  ~QueueModel();
  QueueModel(const QueueModel&) = delete;
  QueueModel& operator=(const QueueModel&) = delete;

  // ---- reading ----
  uint32_t size() const { return q_.size; }
  bool empty() const { return q_.size == 0; }
  int32_t current() const { return current_; }
  uint32_t trackAt(uint32_t pos) const { return pos < q_.size ? q_.data[pos].track : kNone; }
  uint32_t keyAt(uint32_t pos) const { return pos < q_.size ? keyOf(q_.data[pos]) : kNone; }
  uint32_t currentTrack() const { return current_ >= 0 ? q_.data[current_].track : kNone; }
  uint32_t currentKey() const { return current_ >= 0 ? keyOf(q_.data[current_]) : kNone; }
  // The entry at `pos` has been the current one (the class: heard); false
  // out of range.
  bool heardAt(uint32_t pos) const { return pos < q_.size && isHeard(q_.data[pos]); }
  // The position of the entry with `key`, or kNone (removed, or never).
  uint32_t positionOf(uint32_t key) const;
  // Entries after the current one ("12 up next").
  uint32_t upNext() const { return current_ >= 0 ? q_.size - 1 - static_cast<uint32_t>(current_) : 0; }
  // How many tracks an add can take now: the places free under the cap
  // (spare()) and the played entries it would push out to make room
  // (played()); 0: the queue is full and no played entry can go, and an
  // add is refused. It moves with the current entry: what a caller reads
  // before PlaybackController's add can be stale by then (the add's Act
  // takes a heard gapless join first), so the UI reads what an add did
  // after it (queueview::addOutcome()).
  uint32_t room() const { return spare() + played(); }
  // The places free under the cap: kMaxEntries less the size.
  uint32_t spare() const { return q_.size < kMaxEntries ? kMaxEntries - q_.size : 0; }
  // The entries that already played: the heard ones (the class) before
  // the current one. What an add that doesn't fit pushes out, the first
  // in play order first (the positions, shuffled or not: a shuffled
  // queue's entries are in the order that plays); never the current entry,
  // what follows it, or an unheard entry. O(current()).
  uint32_t played() const;
  // How many played entries an add of `n` would push out now: what of it
  // the spare places can't take, at most played().
  uint32_t pushedBy(uint32_t n) const;
  // Bumped by every change to the entries (not by a move of the current
  // position alone): what the UI redraws on and the saver saves on.
  uint32_t contentVersion() const { return contentVersion_; }
  // Bumped whenever the current position or the entries change.
  uint32_t positionVersion() const { return positionVersion_; }
  // The bytes the queue holds from the hooks: the entries' block and the
  // undo snapshot's, at their capacity (the console's q, the remap's log).
  size_t memoryBytes() const {
    return (static_cast<size_t>(q_.cap) + static_cast<size_t>(undo_.cap)) * sizeof(Entry);
  }
  // Shuffled: the entries are in a shuffled order, each with its rank.
  bool shuffled() const { return shuffled_; }
  // Shuffled: the entry's rank (its place in the queue's own order);
  // otherwise `pos` itself. kNone out of range.
  uint32_t rankAt(uint32_t pos) const {
    return pos < q_.size ? (shuffled_ ? q_.data[pos].rank : pos) : kNone;
  }

  // ---- shuffle (see the class) ----
  // False: it already was (nothing done). Allocates nothing; drops the
  // undo (its memory kept for the next snapshot); bumps both versions even
  // when nothing moves (nothing up next), so the saver writes the mode.
  bool setShuffled(bool on);

  // ---- editing: false when out of memory, the queue then unchanged ----
  // Past kMaxEntries, a Play keeps kMaxEntries of the tracks: not shuffled,
  // window(n, start) (kAnyStart: the first kMaxEntries); shuffled, the
  // chosen track (kAnyStart: a random one) and a random kMaxEntries - 1 of
  // the others, picked in the given order (their ranks), then shuffled.
  bool replace(const uint32_t* tracks, uint32_t n, uint32_t start) { return replace(tracks, n, start, shuffled_); }
  // Play in a mode (Shuffle all: shuffled): the mode set and the queue
  // replaced as one edit, laid out as the mode's Play; undo() puts both
  // back. Nothing to play (`n` 0) is a Clear, in that mode.
  bool replace(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffled);
  // The first room() of the tracks, in their order (all of them when they
  // fit), the played entries pushed out first, oldest first, as many as
  // the spare places lack (pushedBy()). False when none fit (room() 0: the
  // queue full and no played entry to push out) or out of memory; the
  // queue then unchanged and nothing to undo. An add that pushes out needs
  // memory for its snapshot as well (its undo is how what played comes
  // back): without it, refused as out of memory, the last edit's undo
  // kept.
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
  // Where step(delta, wrap) would go, without going: a position, or kNone
  // (an empty queue, or past an end without `wrap`).
  uint32_t peek(int delta, bool wrap) const;

  // ---- undo (one level) ----
  // The last edit, if it can be undone (None after undo(), a restore, or a
  // snapshot that didn't fit in memory).
  Edit undoable() const { return undoEdit_; }
  // The mode undo() puts back: the one the last edit was made in (it
  // differs from shuffled() only after a Play that set the mode).
  bool undoShuffled() const { return undoEdit_ != Edit::None ? undoShuffled_ : shuffled_; }
  // The played entries the last edit (an add) pushed out, which undo()
  // puts back where they were; 0: none, or nothing to undo.
  uint32_t undoPushed() const { return undoEdit_ != Edit::None ? undoPushed_ : 0; }
  // The queue as it was before the last edit, in the mode it was in then
  // (an add's pushed-out entries back in their places: the snapshot is the
  // whole queue). The current entry is the one current now if it was
  // there then (what plays keeps playing), otherwise the one that was
  // current then. What was heard since the edit stays heard (by key: the
  // queue left behind is sorted in place to look the keys up, O(n log n),
  // nothing allocated).
  bool undo();
  void dropUndo();

  // ---- restoring (persistence, a library rebuild) ----
  // The queue becomes these tracks with `current` (clamped; -1 for an
  // empty queue), with fresh keys and no undo, the entries before
  // `current` heard (the file can't say which were: the class), shuffled
  // or not, with `ranks` (nullptr: the positions; read back from a
  // shuffled file, they may have gaps where tracks were dropped). Past kMaxEntries, only
  // window(n, current) of them (the boot's whole library with no saved
  // queue: its first kMaxEntries). The entries' block is then exactly as
  // long as what is kept (a new block when it was any other size; if none
  // can be had for a smaller count, the bigger block is kept and still
  // filled) and the snapshot's is given back: the first edit after takes
  // one of the queue's exact size.
  bool assign(const uint32_t* tracks, uint32_t n, int32_t current, bool shuffled = false,
              const uint32_t* ranks = nullptr);
  // Gives back every block (the entries and the undo snapshot): the queue
  // is empty (current -1, no undo) and holds no memory, its mode kept
  // (shuffle is the listener's, not the queue's) and its keys never
  // reused. Both versions are bumped. For a library rebuild
  // (docs/METADATA.md section 3.4.2, step 3; queueremap::run()): the queue
  // file holds the queue while the index is rebuilt in its memory, and an
  // assign() reads it back after. Nothing may save the queue meanwhile (the
  // saver would write it empty): the flash's rebuild holds the loop, and
  // the card's update step holds QueueStore's saver behind its fence (N12:
  // queueremap::Carry).
  void release();

private:
  struct Entry {
    uint32_t track;
    uint32_t key;   // its key (31 bits), and kHeard
    uint32_t rank;  // shuffled: its place in the own order; otherwise unused
  };
  // In Entry::key: the entry has been the current one (the class).
  static constexpr uint32_t kHeard = 0x80000000u;
  static uint32_t keyOf(const Entry& e) { return e.key & ~kHeard; }
  static bool isHeard(const Entry& e) { return (e.key & kHeard) != 0; }
  struct Array {  // from the hooks
    Entry* data = nullptr;
    uint32_t size = 0;
    uint32_t cap = 0;
  };

  bool reserve(Array& a, uint32_t n);
  void drop(Array& a);
  // The next key (31 bits: past 0x7FFFFFFF, 0 again).
  uint32_t newKey();
  // Entry `pos` becomes `track` with the next key and `rank`.
  void put(uint32_t pos, uint32_t track, uint32_t rank);
  // The current entry is heard (none: nothing).
  void hear() {
    if (current_ >= 0) q_.data[current_].key |= kHeard;
  }
  // undo(): the marks of the queue left behind (undo_ now, sorted here by
  // key) go to the same entries in the one put back.
  void carryHeard();
  // Copies the entries and the mode to the undo snapshot (the edit is then
  // undoable); false: no memory for it (the edit goes ahead, not undoable).
  bool snapshot(Edit edit);
  // A bit per position in `positions` (in range), in a block from the
  // hooks; nullptr: no memory. `count`: how many distinct positions.
  uint32_t* selection(const uint32_t* positions, uint32_t n, uint32_t* count);
  void changed();
  bool insertAt(uint32_t at, const uint32_t* tracks, uint32_t n, Edit edit);
  // The next draw from the hook (or the fixed sequence): its 32 bits for
  // `bound` 0 (a seed), else in [0, bound) by multiply-shift.
  uint32_t draw(uint32_t bound = 0);
  // The highest rank of the entries (0 for none), less the first `skip`
  // heard ones before the current entry (those a push-out takes): O(n).
  uint32_t maxRank(uint32_t skip = 0) const;

  AllocFn allocFn_;
  FreeFn freeFn_;
  RandomFn randomFn_;
  uint32_t rng_ = 0x2545F491u;  // the fixed sequence's state (no hook)
  bool shuffled_ = false;
  Array q_;  // the entries
  int32_t current_ = -1;
  uint32_t nextKey_ = 0;
  uint32_t contentVersion_ = 0;
  uint32_t positionVersion_ = 0;

  Array undo_;
  int32_t undoCurrent_ = -1;
  bool undoShuffled_ = false;  // the mode the snapshot was taken in
  uint32_t undoPushed_ = 0;    // the played entries that edit pushed out
  Edit undoEdit_ = Edit::None;
};
