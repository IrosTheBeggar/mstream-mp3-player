// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 IrosTheBeggar

#include "QueueModel.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "Shuffle.h"

namespace {

void* defaultAlloc(size_t n) { return std::malloc(n); }
void defaultFree(void* p) { std::free(p); }

bool isSet(const uint32_t* bits, uint32_t i) { return (bits[i >> 5] >> (i & 31)) & 1u; }

}  // namespace

QueueModel::QueueModel(AllocFn alloc, FreeFn release, RandomFn random)
    : allocFn_(alloc ? alloc : defaultAlloc), freeFn_(release ? release : defaultFree), randomFn_(random) {}

QueueModel::~QueueModel() {
  drop(q_);
  drop(undo_);
}

QueueModel::Window QueueModel::window(uint32_t n, int32_t current) {
  if (n <= kMaxEntries) return Window{0, n};
  const uint32_t cur = current < 0 ? 0 : static_cast<uint32_t>(current);
  if (cur < kMaxEntries) return Window{0, kMaxEntries};
  return Window{std::min(cur, n - kMaxEntries), kMaxEntries};
}

bool QueueModel::reserve(Array& a, uint32_t n) {
  if (n <= a.cap) return true;
  // Doubling, never past the cap (nothing asks for more), never less than
  // asked.
  uint32_t cap = !a.cap ? 16 : a.cap * 2;
  if (cap > kMaxEntries) cap = kMaxEntries;
  if (cap < n) cap = n;
  auto* p = static_cast<Entry*>(allocFn_(static_cast<size_t>(cap) * sizeof(Entry)));
  if (!p) return false;
  if (a.size) std::memcpy(p, a.data, static_cast<size_t>(a.size) * sizeof(Entry));
  if (a.data) freeFn_(a.data);
  a.data = p;
  a.cap = cap;
  return true;
}

void QueueModel::drop(Array& a) {
  if (a.data) freeFn_(a.data);
  a = Array{};
}

void QueueModel::changed() {
  ++contentVersion_;
  ++positionVersion_;
  hear();  // whatever the edit made current (the class: heard)
}

uint32_t QueueModel::newKey() {
  const uint32_t k = nextKey_;
  nextKey_ = (nextKey_ + 1) & ~kHeard;
  return k;
}

uint32_t QueueModel::draw(uint32_t bound) {
  uint32_t x;
  if (randomFn_) {
    x = randomFn_();
  } else {
    x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
  }
  return bound ? static_cast<uint32_t>((static_cast<uint64_t>(x) * bound) >> 32) : x;
}

uint32_t QueueModel::maxRank(uint32_t skip) const {
  uint32_t top = 0;
  for (uint32_t i = 0; i < q_.size; ++i) {
    if (skip && static_cast<int32_t>(i) < current_ && isHeard(q_.data[i])) {
      --skip;
      continue;
    }
    top = std::max(top, q_.data[i].rank);
  }
  return top;
}

uint32_t QueueModel::played() const {
  uint32_t n = 0;
  for (int32_t i = 0; i < current_; ++i) n += isHeard(q_.data[i]) ? 1 : 0;
  return n;
}

uint32_t QueueModel::pushedBy(uint32_t n) const {
  const uint32_t open = spare();
  const uint32_t lack = n > open ? n - open : 0;
  return lack ? std::min(lack, played()) : 0;
}

uint32_t QueueModel::positionOf(uint32_t key) const {
  if (key == kNone) return kNone;
  for (uint32_t i = 0; i < q_.size; ++i) {
    if (keyOf(q_.data[i]) == key) return i;
  }
  return kNone;
}

bool QueueModel::snapshot(Edit edit) {
  undoEdit_ = Edit::None;  // the older snapshot is gone either way
  if (!reserve(undo_, q_.size)) return false;
  if (q_.size) std::memcpy(undo_.data, q_.data, static_cast<size_t>(q_.size) * sizeof(Entry));
  undo_.size = q_.size;
  undoCurrent_ = current_;
  undoShuffled_ = shuffled_;
  undoPushed_ = 0;  // (an add that pushes out says so after)
  undoEdit_ = edit;
  return true;
}

uint32_t* QueueModel::selection(const uint32_t* positions, uint32_t n, uint32_t* count) {
  *count = 0;
  if (q_.size == 0 || n == 0 || !positions) return nullptr;
  const size_t bytes = (static_cast<size_t>(q_.size) + 31) / 32 * sizeof(uint32_t);
  auto* bits = static_cast<uint32_t*>(allocFn_(bytes));
  if (!bits) return nullptr;
  std::memset(bits, 0, bytes);
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t p = positions[k];
    if (p >= q_.size || isSet(bits, p)) continue;
    bits[p >> 5] |= 1u << (p & 31);
    ++*count;
  }
  return bits;
}

bool QueueModel::insertAt(uint32_t at, const uint32_t* tracks, uint32_t n, Edit edit) {
  if (n == 0) return true;
  if (!tracks) return false;
  // As many as fit under the cap, the first ones, once what played (the
  // heard entries before the current one) is pushed out to make room,
  // oldest first, as many as the spare places lack; none: refused (full,
  // and no played entry to push out).
  const uint32_t open = spare();
  const uint32_t room = open + played();
  if (n > room) n = room;
  if (n == 0) return false;
  const uint32_t push = n > open ? n - open : 0;  // (at most played())
  const bool wasEmpty = current_ < 0;
  // Shuffled, the new entries' ranks: Play next's right after the current
  // entry's (those above it go up by n), + Queue's after the highest of
  // what stays (the pushed-out ranks leave gaps, which ranks allow). The
  // highest rank grows by n either way: refused past 0xFFFFFFFF.
  uint32_t base = 0;
  if (shuffled_ && !wasEmpty) {
    const uint32_t top = maxRank(push);
    if (top > kNone - n) return false;
    base = edit == Edit::InsertNext ? q_.data[current_].rank + 1 : top + 1;
  }
  if (!reserve(q_, q_.size - push + n)) return false;
  // What played goes only with a way back: no memory for the snapshot (the
  // whole queue as it is, at most the cap's 60 KB), no push-out, and the
  // add is refused as out of memory, the last edit's undo kept (reserve()
  // copies the snapshot it holds).
  if (push && !reserve(undo_, q_.size)) return false;
  snapshot(edit);
  if (push) {
    // The oldest played entries go: the first heard ones before the
    // current entry. The unheard ones before it, the current entry and
    // what follows it move up, in their order, and keep their keys.
    const auto cur = static_cast<uint32_t>(current_);
    uint32_t w = 0, left = push;
    for (uint32_t i = 0; i < cur; ++i) {
      if (left && isHeard(q_.data[i])) {
        --left;
        continue;
      }
      q_.data[w++] = q_.data[i];
    }
    std::memmove(q_.data + w, q_.data + cur, static_cast<size_t>(q_.size - cur) * sizeof(Entry));
    q_.size -= push;
    current_ -= static_cast<int32_t>(push);
    at -= push;
    undoPushed_ = push;
  }
  if (shuffled_ && !wasEmpty && edit == Edit::InsertNext) {
    for (uint32_t i = 0; i < q_.size; ++i) {
      if (q_.data[i].rank >= base) q_.data[i].rank += n;
    }
  }
  std::memmove(q_.data + at + n, q_.data + at, static_cast<size_t>(q_.size - at) * sizeof(Entry));
  for (uint32_t i = 0; i < n; ++i) q_.data[at + i] = Entry{tracks[i], newKey(), base + i};
  q_.size += n;
  if (wasEmpty) {
    current_ = static_cast<int32_t>(at);  // the queue was empty: the first new entry
    // Shuffled: laid out as a Play from its first (the ranks the given
    // order), the rest shuffled after it.
    if (shuffled_) shuffle::permute(q_.data + at + 1, n - 1, draw());
  } else if (static_cast<int32_t>(at) <= current_) {
    current_ += static_cast<int32_t>(n);
  }
  changed();
  return true;
}

bool QueueModel::insertNext(const uint32_t* tracks, uint32_t n) {
  const uint32_t at = current_ < 0 ? q_.size : static_cast<uint32_t>(current_) + 1;
  return insertAt(at, tracks, n, Edit::InsertNext);
}

bool QueueModel::append(const uint32_t* tracks, uint32_t n) { return insertAt(q_.size, tracks, n, Edit::Append); }

void QueueModel::put(uint32_t pos, uint32_t track, uint32_t rank) { q_.data[pos] = Entry{track, newKey(), rank}; }

bool QueueModel::replace(const uint32_t* tracks, uint32_t n, uint32_t start, bool shuffled) {
  if (n == 0 && shuffled == shuffled_) return clear();
  // Past the cap, kMaxEntries of them (the class): a block of that.
  const uint32_t keep = n < kMaxEntries ? n : kMaxEntries;
  if (n && (!tracks || !reserve(q_, keep))) return false;
  // The snapshot first: it holds the mode the queue was in (undo() puts it
  // back with the entries), then the mode this Play is laid out in.
  snapshot(n ? Edit::Replace : Edit::Clear);
  shuffled_ = shuffled;
  q_.size = keep;
  if (n == 0) {
    current_ = -1;
  } else if (shuffled_) {
    // The chosen track first (kAnyStart: a random one), every other one
    // shuffled after it, those before it in the list too; the ranks the
    // given order (each track's place in the list).
    const uint32_t s = start == kAnyStart ? draw(n) : (start < n ? start : n - 1);
    if (keep == n) {
      for (uint32_t i = 0; i < n; ++i) put(i, tracks[i], i);
      std::swap(q_.data[0], q_.data[s]);
    } else {
      // Past the cap: the chosen one, then a random kMaxEntries - 1 of the
      // others, picked in the list's order (the rank of each its place in
      // the whole list: Off gives them in the given order, gaps and all).
      put(0, tracks[s], s);
      uint32_t w = 1;
      shuffle::sample(n - 1, keep - 1, draw(), [&](uint32_t j) {
        const uint32_t i = j < s ? j : j + 1;  // (s itself is first already)
        put(w++, tracks[i], i);
      });
    }
    shuffle::permute(q_.data + 1, keep - 1, draw());
    current_ = 0;
  } else {
    // From `start` (kAnyStart: the first); past the cap, the window that
    // holds it.
    const uint32_t s = start == kAnyStart ? 0 : (start < n ? start : n - 1);
    const Window w = window(n, static_cast<int32_t>(s));
    for (uint32_t i = 0; i < w.count; ++i) put(i, tracks[w.first + i], i);
    current_ = static_cast<int32_t>(s - w.first);
  }
  changed();
  return true;
}

QueueModel::Removed QueueModel::remove(const uint32_t* positions, uint32_t n) {
  Removed r;
  uint32_t count = 0;
  uint32_t* bits = selection(positions, n, &count);
  if (!bits) return r;  // nothing selected, or no memory for the selection
  if (count == 0) {
    freeFn_(bits);
    return r;
  }
  snapshot(Edit::Remove);
  const int32_t old = current_;
  int32_t kept = -1, firstAfter = -1, lastBefore = -1;
  uint32_t w = 0;
  for (uint32_t i = 0; i < q_.size; ++i) {
    const auto pos = static_cast<int32_t>(i);
    if (isSet(bits, i)) {
      if (pos == old) r.current = true;
      continue;
    }
    if (pos < old) {
      lastBefore = static_cast<int32_t>(w);
    } else if (pos == old) {
      kept = static_cast<int32_t>(w);
    } else if (firstAfter < 0) {
      firstAfter = static_cast<int32_t>(w);
    }
    q_.data[w++] = q_.data[i];
  }
  freeFn_(bits);
  r.count = q_.size - w;
  q_.size = w;
  if (!r.current) {
    current_ = kept;
  } else if (firstAfter >= 0) {
    current_ = firstAfter;
  } else {
    current_ = lastBefore;  // -1 when the queue is now empty
    r.pastEnd = true;
  }
  changed();
  return r;
}

bool QueueModel::moveNext(const uint32_t* positions, uint32_t n) {
  if (current_ < 0) return true;  // nothing to move after
  uint32_t count = 0;
  uint32_t* bits = selection(positions, n, &count);
  if (!bits) return n == 0 || !positions;  // no memory, unless nothing was asked
  const auto cur = static_cast<uint32_t>(current_);
  if (isSet(bits, cur)) {  // the current entry can't go after itself
    bits[cur >> 5] &= ~(1u << (cur & 31));
    --count;
  }
  if (count == 0) {
    freeFn_(bits);
    return true;
  }
  // Shuffled, the moved ones rank right after the current entry and the
  // ranks above it go up by their count: refused past 0xFFFFFFFF.
  if (shuffled_ && maxRank() > kNone - count) {
    freeFn_(bits);
    return false;
  }
  // The snapshot is also the source: the queue is rebuilt from it.
  if (!snapshot(Edit::MoveNext)) {
    freeFn_(bits);
    return false;
  }
  uint32_t w = 0;
  for (uint32_t i = 0; i < undo_.size; ++i) {
    if (isSet(bits, i)) continue;
    q_.data[w++] = undo_.data[i];
    if (i != cur) continue;
    current_ = static_cast<int32_t>(w - 1);
    for (uint32_t j = 0; j < undo_.size; ++j) {
      if (isSet(bits, j)) q_.data[w++] = undo_.data[j];
    }
  }
  freeFn_(bits);
  if (shuffled_) {
    // The moved ones are now right after the current entry, in play order.
    const auto first = static_cast<uint32_t>(current_) + 1;
    const uint32_t curRank = q_.data[current_].rank;
    for (uint32_t i = 0; i < q_.size; ++i) {
      if (i >= first && i < first + count) {
        q_.data[i].rank = curRank + 1 + (i - first);
      } else if (q_.data[i].rank > curRank) {
        q_.data[i].rank += count;
      }
    }
  }
  changed();
  return true;
}

bool QueueModel::clearUpNext() {
  if (current_ < 0 || static_cast<uint32_t>(current_) + 1 >= q_.size) return true;
  snapshot(Edit::ClearUpNext);
  q_.size = static_cast<uint32_t>(current_) + 1;
  changed();
  return true;
}

bool QueueModel::clear() {
  if (q_.size == 0) return true;
  snapshot(Edit::Clear);
  q_.size = 0;
  current_ = -1;
  changed();
  return true;
}

bool QueueModel::setCurrent(uint32_t pos) {
  if (pos >= q_.size) return false;
  if (current_ != static_cast<int32_t>(pos)) {
    current_ = static_cast<int32_t>(pos);
    ++positionVersion_;
  }
  hear();  // (a jump: only where it lands; what it passed over isn't)
  return true;
}

bool QueueModel::step(int delta, bool wrap) {
  const uint32_t next = peek(delta, wrap);
  if (next == kNone) return false;
  current_ = static_cast<int32_t>(next);
  ++positionVersion_;
  hear();
  return true;
}

uint32_t QueueModel::peek(int delta, bool wrap) const {
  if (current_ < 0) return kNone;
  const int64_t n = q_.size;
  int64_t next = static_cast<int64_t>(current_) + delta;
  if (next < 0 || next >= n) {
    if (!wrap) return kNone;
    next = ((next % n) + n) % n;
  }
  return static_cast<uint32_t>(next);
}

bool QueueModel::undo() {
  if (undoEdit_ == Edit::None) return false;
  const uint32_t key = currentKey();
  std::swap(q_, undo_);  // undo_ keeps the edited entries' memory for the next snapshot
  undoEdit_ = Edit::None;
  shuffled_ = undoShuffled_;  // the snapshot's mode (a Play that set it: the one before)
  carryHeard();
  const uint32_t pos = positionOf(key);
  current_ = pos != kNone ? static_cast<int32_t>(pos) : undoCurrent_;
  if (current_ >= static_cast<int32_t>(q_.size)) current_ = static_cast<int32_t>(q_.size) - 1;
  changed();
  return true;
}

void QueueModel::carryHeard() {
  // The snapshot's marks are as old as its edit; an entry heard since (a
  // track played on, a skip, a tap) is marked in the queue left behind.
  // That one is of no further use (undo_ now, the next snapshot's memory):
  // sorted by key in place (introsort allocates nothing), each entry put
  // back looks its key up there. Marks are only ever set, so nothing is
  // cleared.
  Entry* const old = undo_.data;
  const uint32_t n = undo_.size;
  if (n == 0 || q_.size == 0) return;
  std::sort(old, old + n, [](const Entry& a, const Entry& b) { return keyOf(a) < keyOf(b); });
  for (uint32_t i = 0; i < q_.size; ++i) {
    Entry& e = q_.data[i];
    if (isHeard(e)) continue;
    const uint32_t k = keyOf(e);
    const Entry* hit = std::lower_bound(old, old + n, k, [](const Entry& a, uint32_t key) { return keyOf(a) < key; });
    if (hit != old + n && keyOf(*hit) == k && isHeard(*hit)) e.key |= kHeard;
  }
}

void QueueModel::dropUndo() {
  undoEdit_ = Edit::None;
  drop(undo_);
}

bool QueueModel::setShuffled(bool on) {
  if (on == shuffled_) return false;
  if (on) {
    // Every rank its position (the own order now), then what is up next
    // shuffled: the current entry and what played before it stay.
    for (uint32_t i = 0; i < q_.size; ++i) q_.data[i].rank = i;
    const auto from = static_cast<uint32_t>(current_ + 1);  // (0 for an empty queue)
    if (from < q_.size) shuffle::permute(q_.data + from, q_.size - from, draw());
  } else {
    // The own order back: by rank (the key breaks a tie, which only a
    // hand-edited file can make). Introsort allocates nothing; a stable
    // sort would ask the heap for a buffer.
    const uint32_t key = currentKey();
    std::sort(q_.data, q_.data + q_.size, [](const Entry& a, const Entry& b) {
      return a.rank != b.rank ? a.rank < b.rank : keyOf(a) < keyOf(b);
    });
    if (current_ >= 0) current_ = static_cast<int32_t>(positionOf(key));
  }
  shuffled_ = on;
  undoEdit_ = Edit::None;  // a toggle is no edit (the snapshot's memory stays for the next one)
  changed();
  return true;
}

bool QueueModel::assign(const uint32_t* tracks, uint32_t n, int32_t current, bool shuffled, const uint32_t* ranks) {
  if (n && !tracks) return false;
  // Past the cap, the window that holds the current entry (its ranks with
  // it: gaps are fine).
  const Window kept = window(n, current);
  if (kept.first) {
    tracks += kept.first;
    if (ranks) ranks += kept.first;
    current -= static_cast<int32_t>(kept.first);  // (>= 0: the window starts at most at it)
  }
  n = kept.count;
  // A block of exactly n entries (docs/METADATA.md section 3.5): the new
  // one first, so a failure leaves the queue as it was; what it held isn't
  // copied (all of it is replaced). A smaller n with no new block to be had
  // keeps the bigger one; nothing to hold gives the block back.
  if (n == 0) {
    drop(q_);
  } else if (n != q_.cap) {
    auto* p = static_cast<Entry*>(allocFn_(static_cast<size_t>(n) * sizeof(Entry)));
    if (p) {
      if (q_.data) freeFn_(q_.data);
      q_.data = p;
      q_.cap = n;
    } else if (n > q_.cap) {
      return false;
    }
  }
  for (uint32_t i = 0; i < n; ++i) q_.data[i] = Entry{tracks[i], newKey(), ranks ? ranks[i] : i};
  shuffled_ = shuffled;
  q_.size = n;
  if (n == 0) {
    current_ = -1;
  } else {
    current_ = current < 0 ? 0 : (current >= static_cast<int32_t>(n) ? static_cast<int32_t>(n) - 1 : current);
  }
  // What played isn't in the file: every entry before the current one
  // counts as heard (the class; docs/QUEUE-MODES.md 15.8).
  for (int32_t i = 0; i < current_; ++i) q_.data[i].key |= kHeard;
  dropUndo();
  changed();
  return true;
}

void QueueModel::release() {
  drop(q_);
  current_ = -1;
  dropUndo();
  changed();
}
