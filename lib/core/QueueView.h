#pragma once
#include <cstddef>
#include <cstdint>

#include "QueueModel.h"

// The Queue screen's portable pieces (the tab bar spec §6.4, with the
// review's grafts), host-tested; ui/QueuePage draws them.
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

// ---- Shuffle all ----
// Fisher-Yates with a xorshift32 from `seed` (0 is taken as 1): the same
// seed gives the same order.
void shuffle(uint32_t* ids, uint32_t n, uint32_t seed);

}  // namespace queueview
