// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueModel.h"

// The Queue screen's portable pieces (the tab bar spec §6.4, with the
// review's grafts), host-tested; ui/QueuePage draws them. And the queue's
// side of the Undo toast (UndoWatch: Ui's loop).
namespace queueview {

// ---- "12 up next · 49 min" ----
//
// Track lengths aren't in the library index (the decoders read a length
// when a track opens), so the player learns them as tracks play: a
// DurationBook is a second count per library track id (2 bytes each, from
// allocator hooks: PSRAM in the firmware), 0 while unknown. The summary
// adds up what is known of what comes next, and says when that is only a
// part ("49+ min").
class DurationBook {
public:
  using AllocFn = void* (*)(size_t bytes);
  using FreeFn = void (*)(void* p);
  explicit DurationBook(AllocFn alloc = nullptr, FreeFn release = nullptr);
  ~DurationBook();
  DurationBook(const DurationBook&) = delete;
  DurationBook& operator=(const DurationBook&) = delete;

  // Room for track ids 0..tracks-1, all unknown (a new or rebuilt
  // library: the ids changed). False: no memory (nothing is learned).
  bool reset(uint32_t tracks);
  uint32_t tracks() const { return n_; }
  // A track's length, learned (ids outside the book are ignored). Lengths
  // over 18 h are kept as 18 h.
  void note(uint32_t id, uint32_t ms);
  // Its length in seconds, 0 if unknown.
  uint32_t seconds(uint32_t id) const { return id < n_ ? s_[id] : 0; }
  // Bumped by every note() that changed something (the summary is redone).
  uint32_t version() const { return version_; }

private:
  AllocFn alloc_;
  FreeFn free_;
  uint16_t* s_ = nullptr;
  uint32_t n_ = 0;
  uint32_t version_ = 0;
};

struct Time {
  uint32_t knownS = 0;   // the lengths known, added up
  uint32_t unknown = 0;  // entries whose length isn't known
};
// The entries after the current one. `hintMs` gives a length the book
// can't (the built-in tracks'); nullptr: none.
Time upNextTime(const QueueModel& q, const DurationBook& book, uint32_t (*hintMs)(void* ctx, uint32_t id),
                void* ctx);
// "12 up next · 49 min", "12 up next · 49+ min" (only part known), "12 up
// next" (nothing known), "1 up next · 4 min", "Nothing up next". With
// `position` > 0: "4 of 16 · " first (the long form). Minutes round up
// (a 20 s track is "1 min"); over 99 min: "2 h 5 min". `withUpNext`
// false (with a position): the count left out, "4 of 16 · 49 min" (or
// "4 of 16"), the header's form when the long one doesn't fit: where
// the queue is matters more than how many are left. Returns buf.
char* summary(uint32_t upNext, const Time& t, uint32_t position, uint32_t size, char* buf, size_t bufSize,
              bool withUpNext = true);

// ---- what a Library add put in the queue ----
//
// The review's graft: after Play next or + Queue in the Library, the next
// visit to the Queue scrolls to what was added and highlights it. Each
// entry's key is given when it joins and only grows, so everything added
// since the first add after the last visit has a key at or above that
// add's first key (a Play in between replaces the queue: clear()).
class AddedMark {
public:
  static constexpr uint32_t kNone = QueueModel::kNone;
  // An add; `firstKey` its first entry's key (keys are consecutive).
  void noteAdded(uint32_t firstKey) {
    if (since_ == kNone || firstKey < since_) since_ = firstKey;
  }
  void clear() { since_ = kNone; }
  bool pending() const { return since_ != kNone; }
  bool marks(uint32_t key) const { return since_ != kNone && key != kNone && key >= since_; }
  // The first position holding a marked entry (the one to scroll to), or
  // kNone (all undone or removed). A pass over the queue.
  uint32_t firstPosition(const QueueModel& q) const;
  uint32_t since() const { return since_; }

private:
  uint32_t since_ = kNone;
};

// ---- the queue's cap (QueueModel::kMaxEntries: docs/QUEUE-MODES.md 15) ----
//
// `n` with its thousands grouped ("19,412"), as the cap's texts write
// their counts. Returns buf.
char* grouped(uint32_t n, char* buf, size_t size);
// The toast of a Play, a Shuffle all or an add the cap cut short, `took`
// of `asked`: "Shuffling 5,000 of 19,412: the queue holds 5,000 tracks",
// "Playing ...", "Added 37 of 300: ...", "37 of 300 play next: ..."
// (uitext's kCap* texts). Returns buf.
enum class Capped : uint8_t { Shuffle, Play, Add, Next };
char* cappedText(Capped what, uint32_t took, uint32_t asked, char* buf, size_t size);
// The toast of an add (`what`: Next, Play next; anything else, + Queue)
// that pushed out `pushed` played tracks to make room (docs/QUEUE-MODES.md
// 15.8; `took` of `asked` went in): "Added 12 tracks: 12 played tracks made
// way", "Plays next: 1 played track made way" (one track: its title gives
// way to the news), "Added 37 of 300: 37 played tracks made way" (cut short
// by the cap too). uitext's kPush* texts. Returns buf.
char* pushedText(Capped what, uint32_t took, uint32_t asked, uint32_t pushed, char* buf, size_t size);

// ---- tracks that failed to play ----
//
// The last few queue entries (by key) whose track couldn't be played, so
// their rows keep a small amber "!" (spec §7). A ring: the oldest goes.
class KeyRing {
public:
  static constexpr int kSize = 16;
  void add(uint32_t key);
  bool has(uint32_t key) const;
  void clear() { n_ = 0; }
  int count() const { return n_; }

private:
  uint32_t k_[kSize] = {};
  int n_ = 0;
  int next_ = 0;
};

// ---- the Undo toast, when the undo goes from under it ----
//
// The queue's one undo can go while a toast still offers it: a shuffle
// toggle drops it (docs/QUEUE-MODES.md 2.6), and the console's qu uses it.
// That toast's Undo would then answer "Nothing to undo", so it goes, and
// the log says why. Ui calls pass() once a loop. Shuffle all's Play sets
// the mode inside the edit (still undoable: its toast stays), so qu's undo
// of it puts the mode back: a change of mode that is no toggle, which
// undone() tells apart.
class UndoWatch {
public:
  enum class Gone : uint8_t { Stays, Toggle, Undone };
  // The console's qu undid the queue's last edit (since the last pass).
  void undone() { undone_ = true; }
  // `shuffled`: the mode now; `undoToast`: a toast offering Undo is up;
  // `undoable`: the queue's. Whether that toast goes, and why: Undone
  // (qu undid since the last pass) before Toggle (the mode changed since
  // the last pass); Stays while the undo is there, or nothing took it.
  Gone pass(bool shuffled, bool undoToast, QueueModel::Edit undoable);
  // The last pass saw the mode change (a toggle, or an undo that put it
  // back): no Queue badge flash for it (Off can grow "up next" without
  // adding anything).
  bool modeChanged() const { return changed_; }

private:
  bool last_ = false;
  bool undone_ = false;
  bool changed_ = false;
};

// ---- a refused add's note ----
//
// An add the full queue refuses (nothing in it played, so nothing to push
// out: docs/QUEUE-MODES.md 15.8) changes nothing (15.2: the last edit's
// undo stays), so its note ("Nothing played yet: the queue holds 5,000
// tracks") keeps the buttons of the toast it covers: the Undo and the View
// a Play all or an add was offering moments before (15.5). Else that undo
// would be out of reach while the queue still keeps it.
struct ToastButtons {
  bool undo = false;
  uint32_t viewKey = QueueModel::kNone;  // kNone: no View
};
// The toast up (`up`), whether it offers Undo and View (`viewKey` its
// View's), and the queue's undoable edit: what the note keeps.
ToastButtons keptByRefusal(bool up, bool undo, bool view, uint32_t viewKey, QueueModel::Edit undoable);

}  // namespace queueview
